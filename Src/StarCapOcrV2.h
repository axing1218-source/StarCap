#pragma once

#include <include/Ling.h>
#include <robuffer.h>
#include <dwmapi.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include "Win/WinCap.h"
#include "Win/CutMask.h"
#include "Win/WinPin.h"
#include "Util.h"
#include "Setting.h"
#include "GeminiClient.h"
#include "StarCapTextGeometry.h"
#include "StarCapParagraphLayout.h"

namespace StarCapOcrV2
{
    struct Result
    {
        std::wstring text;
        std::wstring error;
    };

    class OcrResultWindow;
    inline OcrResultWindow* activeWindow{ nullptr };
    inline std::atomic<unsigned long long> requestId{ 0 };

    inline bool darkMode()
    {
        auto* setting = Setting::get();
        return setting && setting->getToolFlag(L"app", L"darkMode", false);
    }

    // OCR/translation has several independent surfaces, so keep its palette in one
    // place instead of darkening the entire window. The screenshot pixels themselves
    // are always rendered unchanged.
    inline uint32_t uiWindowBg()      { return darkMode() ? 0x1F2023FF : 0xF4F4F4FF; }
    inline uint32_t uiPanelBg()       { return darkMode() ? 0x202124FF : 0xFFFFFFFF; }
    inline uint32_t uiCanvasBg()      { return darkMode() ? 0x191A1DFF : 0xF2F2F2FF; }
    inline uint32_t uiToolbarBg()     { return darkMode() ? 0x25262AFF : 0xFAFAFAFF; }
    inline uint32_t uiSurface()       { return darkMode() ? 0x2A2C31FF : 0xF3F3F3FF; }
    inline uint32_t uiSurfaceHover()  { return darkMode() ? 0x35383EFF : 0xE9E9E9FF; }
    inline uint32_t uiText()          { return darkMode() ? 0xE8EAEDFF : 0x222222FF; }
    inline uint32_t uiSecondary()     { return darkMode() ? 0xB8BCC5FF : 0x666666FF; }
    inline uint32_t uiMuted()         { return darkMode() ? 0x90959FFF : 0x999999FF; }
    inline uint32_t uiBorder()        { return darkMode() ? 0x464950FF : 0xDDDDDDFF; }
    inline uint32_t uiTextBoxBg()     { return darkMode() ? 0x24262BFF : 0xFAFAFAFF; }
    inline uint32_t uiLoadingBg()     { return darkMode() ? 0x2D3036FF : 0xE2E2E2CC; }
    inline uint32_t uiSelectedBg()    { return darkMode() ? 0x303A5CFF : 0xE8F3FFFF; }
    inline uint32_t uiSelectionBg()   { return darkMode() ? 0x4B67A899 : 0xB8DDF799; }
    inline uint32_t uiAccent()        { return 0x1677FFFF; }

    inline bool copyTextReliable(HWND owner, const std::wstring& text)
    {
        if (text.empty()) return false;
        bool opened = false;
        for (int i = 0; i < 8; ++i) {
            if (OpenClipboard(owner)) { opened = true; break; }
            Sleep(8);
        }
        if (!opened) return false;
        bool ok = false;
        if (EmptyClipboard()) {
            const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            if (mem) {
                auto dst = static_cast<wchar_t*>(GlobalLock(mem));
                if (dst) {
                    memcpy(dst, text.c_str(), text.size() * sizeof(wchar_t));
                    dst[text.size()] = L'\0';
                    GlobalUnlock(mem);
                    if (SetClipboardData(CF_UNICODETEXT, mem)) { ok = true; mem = nullptr; }
                }
                if (mem) GlobalFree(mem);
            }
        }
        CloseClipboard();
        return ok;
    }

    inline bool copyCutPixels(WinCap* win, std::vector<BYTE>& pixels, int& outW, int& outH)
    {
        if (!win) return false;
        auto img = win->getCutImg();
        if (!img) return false;
        const auto size = img->GetPixelSize();
        if (!size.width || !size.height) return false;
        outW = (int)size.width; outH = (int)size.height;
        D2D1_BITMAP_PROPERTIES1 prop{};
        prop.pixelFormat = img->GetPixelFormat();
        prop.dpiX = 96.f; prop.dpiY = 96.f;
        prop.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
        auto ctx = Ling::D2D::get()->deviceContext;
        if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &prop, cpu.GetAddressOf())) || !cpu) return false;
        if (FAILED(cpu->CopyFromBitmap(nullptr, img.Get(), nullptr))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)) || !mapped.bits) return false;
        const size_t rowBytes = (size_t)outW * 4;
        pixels.resize(rowBytes * (size_t)outH);
        for (int row = 0; row < outH; ++row) {
            memcpy(pixels.data() + (size_t)row * rowBytes,
                mapped.bits + (size_t)row * mapped.pitch, rowBytes);
        }
        cpu->Unmap();
        return true;
    }

    inline Result recognizeWindows(std::vector<BYTE> pixels, int width, int height)
    {
        Result result;
        bool apartmentReady = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartmentReady = true;
            using namespace winrt::Windows::Storage::Streams;
            using namespace winrt::Windows::Graphics::Imaging;
            using namespace winrt::Windows::Media::Ocr;
            const uint32_t byteCount = (uint32_t)pixels.size();
            Buffer buffer(byteCount); buffer.Length(byteCount);
            auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
            BYTE* dst{ nullptr };
            winrt::check_hresult(byteAccess->Buffer(&dst));
            memcpy(dst, pixels.data(), pixels.size());
            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Ignore);
            bitmap.CopyFromBuffer(buffer);
            auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) result.error = L"当前 Windows 没有可用的 OCR 语言包。";
            else {
                auto ocr = engine.RecognizeAsync(bitmap).get();
                bool first = true;
                for (auto const& line : ocr.Lines()) {
                    if (!first) result.text += L"\r\n";
                    first = false;
                    result.text += line.Text().c_str();
                }
                if (result.text.empty()) result.error = L"没有识别到文字。";
            }
            winrt::uninit_apartment(); apartmentReady = false;
        }
        catch (const winrt::hresult_error& e) {
            result.error = std::wstring(L"OCR 失败：") + e.message().c_str();
            if (apartmentReady) { try { winrt::uninit_apartment(); } catch (...) {} }
        }
        catch (...) {
            result.error = L"OCR 失败：发生未知错误。";
            if (apartmentReady) { try { winrt::uninit_apartment(); } catch (...) {} }
        }
        return result;
    }

    class OcrResultWindow : public Ling::WinBase
    {
    public:
        OcrResultWindow(std::vector<BYTE> data, int imgW, int imgH, bool fromLongScreenshot = false)
            : pixels(std::move(data)), imageW(imgW), imageH(imgH), isLongScreenshotSource(fromLongScreenshot)
        {
            setTitle(L"StarCap - 文字识别 / 翻译");
            setSize(1120.f, 700.f);
            setMinSize(800.f, 460.f);
            setCenter();
            onMouseDown.add([this](POINT pos, bool isRight) {
                if (!imageCanvas || !imageCanvas->isPosIn(pos)) return;
                if (isRight) { showImageContextMenu(); return; }
                imageDragging = true;
                dragStart = pos;
                dragStartOffsetX = imageOffsetX;
                dragStartOffsetY = imageOffsetY;
                SetCapture(hwnd);
            });
            onMouseMove.add([this](POINT pos) {
                if (!imageDragging) return;
                imageOffsetX = dragStartOffsetX + (pos.x - dragStart.x);
                imageOffsetY = dragStartOffsetY + (pos.y - dragStart.y);
                clampImageOffset();
                refresh();
            });
            onMouseUp.add([this](POINT, bool isRight) {
                if (isRight || !imageDragging) return;
                imageDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            });
            onMouseWheel.add([this](POINT pos, float delta) {
                if (!imageCanvas || !imageCanvas->isPosIn(pos) || delta == 0.f) return;
                zoomBy(delta > 0.f ? 1.15f : (1.f / 1.15f));
            });
            onSizeChanged.add([this]() { clampImageOffset(); updateZoomLabel(); refresh(); });
            onDestroy.add([this]() {
                ++requestId;
                if (activeWindow == this) activeWindow = nullptr;
                auto dying = this;
                Ling::App::get()->dq.TryEnqueue([dying]() {
                    delete dying;
                    auto app = Ling::App::get();
                    auto it = app->args.find(L"--auto-quit");
                    if (it != app->args.end() && it->second == L"true" && !WinPin::hasWindow()) app->quit(0);
                });
            });
        }

        void open() { createNativeWindow(0, WS_OVERLAPPEDWINDOW); }

        // Used by direct translation of a stitched long screenshot: open the same viewer but
        // skip a separate OCR request and immediately translate the image.
        void beginImageTranslation()
        {
            originalText.clear();
            geminiOcrBlocks.clear();
            if (textBox) textBox->setText(L"");
            startGeminiTranslation();
        }

        void setLocalOcrResult(Result result)
        {
            if (!textBox || !status) return;
            geminiOcrBlocks.clear();
            if (!result.error.empty()) {
                originalText.clear();
                textBox->setText(result.error);
                status->setText(L"未设置 Gemini API Key，当前使用 Windows OCR");
            }
            else {
                originalText = std::move(result.text);
                textBox->setText(originalText);
                status->setText(L"未设置 Gemini API Key，当前使用 Windows OCR；可在设置中填写 Key");
            }
            textBox->setPlaceholder(L"");
            showingTranslationText = false;
            refresh();
        }

        void setGeminiOcrResult(GeminiClient::OcrResult result)
        {
            if (!textBox || !status) return;
            if (!result.ok) {
                originalText.clear(); geminiOcrBlocks.clear();
                textBox->setPlaceholder(L"");
                textBox->setText(result.error.empty() ? L"Gemini OCR 失败。" : result.error);
                status->setText(L"Gemini 文字识别失败；不会静默退回低准确率 OCR");
                return;
            }
            originalText = std::move(result.text);
            geminiOcrBlocks = std::move(result.blocks);
            showingTranslationText = false;
            textBox->setPlaceholder(L"");
            textBox->setText(originalText);
            status->setText(L"Gemini 文字识别完成；可拖选文字复制，点击“翻译”继续");
            refresh();
        }

        void setTranslationResult(GeminiClient::TranslationResult result)
        {
            translating = false;
            if (!status || !textBox) return;
            textBox->setBg(uiTextBoxBg());
            if (!result.ok) {
                status->setText(result.error.empty() ? L"Gemini 翻译失败。" : result.error);
                textBox->setText(originalText);
                if (translateBtn) translateBtn->setText(L"翻译");
                refresh();
                return;
            }
            if (originalText.empty() && !result.sourceText.empty()) {
                originalText = std::move(result.sourceText);
            }
            StarCapTextGeometry::stabilize(result.blocks, pixels, imageW, imageH, L"result");
            StarCapParagraphLayout::apply(result.blocks, pixels, imageW, imageH, L"result");
            translatedText = std::move(result.translatedText);
            translationBlocks = std::move(result.blocks);
            translationReady = true;
            if (originalTab) originalTab->show();
            if (translatedTab) translatedTab->show();
            showMode(true);
        }

    protected:
        void onCreated() override
        {
            if (darkMode()) {
                BOOL dark = TRUE;
                // Use the literal to remain compatible with SDKs where the enum name is
                // unavailable while the Windows 10/11 DWM attribute is still supported.
                constexpr DWORD immersiveDarkModeAttribute = 20;
                DwmSetWindowAttribute(hwnd, immersiveDarkModeAttribute, &dark, sizeof(dark));
            }

            body->setBg(uiWindowBg());
            body->setFlexDirection(Ling::FlexDirection::Row);

            auto left = body->makeChild<Ling::Node>();
            left->setFlexGrow(1.f);
            left->setHeightPercent(100.f);
            left->setBg(uiCanvasBg());
            left->setFlexDirection(Ling::FlexDirection::Column);

            auto zoomRow = left->makeChild<Ling::Node>();
            zoomRow->setHeight(38.f); zoomRow->setWidthPercent(100.f);
            zoomRow->setPaddingLeft(10.f); zoomRow->setPaddingRight(10.f);
            zoomRow->setBg(uiToolbarBg());
            zoomRow->setFlexDirection(Ling::FlexDirection::Row);
            zoomRow->setAlignItems(Ling::Align::Center);

            zoomFitBtn = zoomRow->makeChild<Ling::Button>();
            zoomFitBtn->setText(L"适合"); zoomFitBtn->setSize(52.f, 26.f); zoomFitBtn->setFontSize(12.f);
            zoomFitBtn->setBorderRadius(4.f); zoomFitBtn->setBg(uiSelectedBg()); zoomFitBtn->setColor(uiAccent());
            zoomFitBtn->setHoverColor(uiAccent()); zoomFitBtn->setHoverBg(uiSelectedBg());
            zoomFitBtn->setMarginRight(6.f);
            zoomFitBtn->onClick.add([this](Ling::Button*) { setFitZoom(); });

            zoomActualBtn = zoomRow->makeChild<Ling::Button>();
            zoomActualBtn->setText(L"1:1"); zoomActualBtn->setSize(46.f, 26.f); zoomActualBtn->setFontSize(12.f);
            zoomActualBtn->setBorderRadius(4.f); zoomActualBtn->setBg(uiSurface()); zoomActualBtn->setColor(uiText());
            zoomActualBtn->setHoverColor(uiText()); zoomActualBtn->setHoverBg(uiSurfaceHover());
            zoomActualBtn->setMarginRight(10.f);
            zoomActualBtn->onClick.add([this](Ling::Button*) { setActualZoom(); });

            zoomOutBtn = zoomRow->makeChild<Ling::Button>();
            zoomOutBtn->setText(L"−"); zoomOutBtn->setSize(30.f, 26.f); zoomOutBtn->setFontSize(15.f);
            zoomOutBtn->setBorderRadius(4.f); zoomOutBtn->setBg(uiSurface()); zoomOutBtn->setColor(uiText());
            zoomOutBtn->setHoverColor(uiText()); zoomOutBtn->setHoverBg(uiSurfaceHover());
            zoomOutBtn->setMarginRight(4.f);
            zoomOutBtn->onClick.add([this](Ling::Button*) { zoomBy(1.f / 1.25f); });

            zoomLabel = zoomRow->makeChild<Ling::Label>();
            zoomLabel->setText(L"适合"); zoomLabel->setWidth(72.f); zoomLabel->setFontSize(12.f);
            zoomLabel->setColor(uiSecondary());

            zoomInBtn = zoomRow->makeChild<Ling::Button>();
            zoomInBtn->setText(L"+"); zoomInBtn->setSize(30.f, 26.f); zoomInBtn->setFontSize(15.f);
            zoomInBtn->setBorderRadius(4.f); zoomInBtn->setBg(uiSurface()); zoomInBtn->setColor(uiText());
            zoomInBtn->setHoverColor(uiText()); zoomInBtn->setHoverBg(uiSurfaceHover());
            zoomInBtn->setMarginLeft(4.f);
            zoomInBtn->onClick.add([this](Ling::Button*) { zoomBy(1.25f); });

            auto zoomHint = zoomRow->makeChild<Ling::Label>();
            zoomHint->setText(L"拖动图片可查看超出区域"); zoomHint->setFontSize(11.f); zoomHint->setColor(uiMuted());
            zoomHint->setFlexGrow(1.f);

            imageCanvas = left->makeChild<Ling::Canvas>();
            imageCanvas->setFlexGrow(1.f);
            imageCanvas->setWidthPercent(100.f);
            imageCanvas->setBg(uiCanvasBg());
            auto divider = body->makeChild<Ling::Node>();
            divider->setWidth(1.f); divider->setHeightPercent(100.f); divider->setBg(uiBorder());
            auto right = body->makeChild<Ling::Node>();
            right->setWidth(430.f); right->setHeightPercent(100.f); right->setPadding(12.f);
            right->setBg(uiPanelBg()); right->setFlexDirection(Ling::FlexDirection::Column);

            auto header = right->makeChild<Ling::Node>();
            header->setHeight(38.f); header->setWidthPercent(100.f);
            header->setFlexDirection(Ling::FlexDirection::Row); header->setAlignItems(Ling::Align::Center);
            auto title = header->makeChild<Ling::Label>();
            title->setText(L"文字识别"); title->setFontSize(16.f); title->setColor(uiText()); title->setFlexGrow(1.f);
            auto annotate = header->makeChild<Ling::Button>();
            annotate->setText(L"标注图片"); annotate->setSize(76.f, 28.f); annotate->setFontSize(12.f);
            annotate->setBg(uiSurface()); annotate->setHoverBg(uiSurfaceHover());
            annotate->setColor(uiText()); annotate->setHoverColor(uiText());
            annotate->setBorder(1.f, uiBorder()); annotate->setBorderRadius(4.f); annotate->setMarginRight(8.f);
            annotate->onClick.add([this](Ling::Button*) { openAnnotationEditor(); });
            auto copyAll = header->makeChild<Ling::Button>();
            copyAll->setText(L"复制全部"); copyAll->setSize(76.f, 28.f); copyAll->setFontSize(12.f);
            copyAll->setBg(uiSurface()); copyAll->setHoverBg(uiSurfaceHover());
            copyAll->setColor(uiText()); copyAll->setHoverColor(uiText());
            copyAll->setBorder(1.f, uiBorder()); copyAll->setBorderRadius(4.f);
            copyAll->onClick.add([this](Ling::Button*) {
                const auto& text = showingTranslationText && translationReady ? translatedText : originalText;
                if (copyTextReliable(hwnd, text)) status->setText(L"已复制当前文字");
                else if (!text.empty()) status->setText(L"复制失败，请重试");
            });

            auto modeRow = right->makeChild<Ling::Node>();
            modeRow->setHeight(36.f); modeRow->setWidthPercent(100.f);
            modeRow->setFlexDirection(Ling::FlexDirection::Row); modeRow->setAlignItems(Ling::Align::Center);
            originalTab = modeRow->makeChild<Ling::Button>();
            originalTab->setText(L"原文"); originalTab->setSize(58.f, 28.f); originalTab->setFontSize(12.f);
            originalTab->setBorderRadius(4.f); originalTab->setMarginRight(6.f);
            originalTab->setBg(uiSurface()); originalTab->setColor(uiText());
            originalTab->setHoverBg(uiSurfaceHover()); originalTab->setHoverColor(uiText());
            originalTab->onClick.add([this](Ling::Button*) { if (translationReady) showMode(false); });
            originalTab->hide();
            translatedTab = modeRow->makeChild<Ling::Button>();
            translatedTab->setText(L"译文"); translatedTab->setSize(58.f, 28.f); translatedTab->setFontSize(12.f);
            translatedTab->setBorderRadius(4.f);
            translatedTab->setBg(uiSurface()); translatedTab->setColor(uiText());
            translatedTab->setHoverBg(uiSurfaceHover()); translatedTab->setHoverColor(uiText());
            translatedTab->onClick.add([this](Ling::Button*) { if (translationReady) showMode(true); });
            translatedTab->hide();
            auto spacer = modeRow->makeChild<Ling::Node>(); spacer->setFlexGrow(1.f);
            translateBtn = modeRow->makeChild<Ling::Button>();
            translateBtn->setText(L"翻译"); translateBtn->setSize(78.f, 28.f); translateBtn->setFontSize(12.f);
            translateBtn->setColor(0xFFFFFFFF); translateBtn->setBg(uiAccent());
            translateBtn->setHoverColor(0xFFFFFFFF); translateBtn->setHoverBg(0x4096FFFF); translateBtn->setBorderRadius(4.f);
            translateBtn->onClick.add([this](Ling::Button*) { startGeminiTranslation(); });

            status = right->makeChild<Ling::Label>();
            status->setHeight(30.f); status->setWidthPercent(100.f);
            status->setText(L"正在识别..."); status->setFontSize(12.f); status->setColor(uiMuted());
            textBox = right->makeChild<Ling::TextBox>();
            textBox->setFlexGrow(1.f); textBox->setWidthPercent(100.f); textBox->setFontSize(14.f);
            textBox->setPadding(10.f); textBox->setBg(uiTextBoxBg());
            textBox->setBorder(1.f, uiBorder()); textBox->setBorderRadius(4.f);
            textBox->setColor(uiText()); textBox->setCaretColor(uiText());
            textBox->setSelectionBgColor(uiSelectionBg()); textBox->setPlaceholderColor(uiMuted());
            textBox->setPlaceholder(L"正在识别...");

            if (imageW > 0 && imageH > 0 && !pixels.empty()) {
                D2D1_BITMAP_PROPERTIES1 props{};
                props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE; props.dpiX = 96.f; props.dpiY = 96.f;
                Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU((UINT32)imageW, (UINT32)imageH),
                    pixels.data(), (UINT32)imageW * 4, &props, imageBitmap.GetAddressOf());
            }
            show(); SetForegroundWindow(hwnd);
        }

        void layout() override { Ling::WinBase::layout(); paintImage(); }
        LRESULT onHitTest(const POINT pos) override { return DefWindowProcW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pos.x, pos.y)); }

    private:
        void showMode(bool translated)
        {
            if (!translationReady) translated = false;
            showTranslatedImage = translated;
            showingTranslationText = translated;
            if (textBox) {
                textBox->setBg(uiTextBoxBg());
                textBox->setText(translated ? translatedText : originalText);
            }
            if (translateBtn) translateBtn->setText(translated ? L"原文" : L"译文");
            updateTextTabs();
            if (status) status->setText(translated
                ? L"当前显示译文；再次点击“原文”可立即切回对比"
                : L"当前显示原文；再次点击“译文”可立即切回翻译结果");
            refresh();
        }

        void updateTextTabs()
        {
            if (!originalTab || !translatedTab) return;
            if (showingTranslationText) {
                originalTab->setBg(uiSurface()); originalTab->setColor(uiText());
                originalTab->setHoverBg(uiSurfaceHover()); originalTab->setHoverColor(uiText());
                translatedTab->setBg(uiSelectedBg()); translatedTab->setColor(uiAccent());
                translatedTab->setHoverBg(uiSelectedBg()); translatedTab->setHoverColor(uiAccent());
            }
            else {
                originalTab->setBg(uiSelectedBg()); originalTab->setColor(uiAccent());
                originalTab->setHoverBg(uiSelectedBg()); originalTab->setHoverColor(uiAccent());
                translatedTab->setBg(uiSurface()); translatedTab->setColor(uiText());
                translatedTab->setHoverBg(uiSurfaceHover()); translatedTab->setHoverColor(uiText());
            }
        }

        float getFitScale() const
        {
            if (!imageCanvas || imageW <= 0 || imageH <= 0) return 1.f;
            const float margin = 18.f * dpi;
            const float availW = std::max(1.f, imageCanvas->w - margin * 2.f);
            const float availH = std::max(1.f, imageCanvas->h - margin * 2.f);
            return std::clamp(std::min(availW / imageW, availH / imageH), .02f, 8.f);
        }

        float getImageScale() const
        {
            return imageFitMode ? getFitScale() : std::clamp(imageManualScale, .02f, 8.f);
        }

        void updateZoomButtons()
        {
            if (zoomFitBtn) {
                zoomFitBtn->setBg(imageFitMode ? uiSelectedBg() : uiSurface());
                zoomFitBtn->setColor(imageFitMode ? uiAccent() : uiText());
                zoomFitBtn->setHoverBg(imageFitMode ? uiSelectedBg() : uiSurfaceHover());
                zoomFitBtn->setHoverColor(imageFitMode ? uiAccent() : uiText());
            }
            if (zoomActualBtn) {
                const bool actual = !imageFitMode && fabsf(imageManualScale - 1.f) < .001f;
                zoomActualBtn->setBg(actual ? uiSelectedBg() : uiSurface());
                zoomActualBtn->setColor(actual ? uiAccent() : uiText());
                zoomActualBtn->setHoverBg(actual ? uiSelectedBg() : uiSurfaceHover());
                zoomActualBtn->setHoverColor(actual ? uiAccent() : uiText());
            }
        }

        void updateZoomLabel()
        {
            updateZoomButtons();
            if (!zoomLabel) return;
            const int pct = (int)std::round(getImageScale() * 100.f);
            zoomLabel->setText(imageFitMode ? std::format(L"适合 {}%", pct) : std::format(L"{}%", pct));
        }

        void clampImageOffset()
        {
            if (!imageCanvas || imageW <= 0 || imageH <= 0) return;
            if (imageFitMode) { imageOffsetX = imageOffsetY = 0.f; return; }
            const float margin = 18.f * dpi;
            const float availW = std::max(1.f, imageCanvas->w - margin * 2.f);
            const float availH = std::max(1.f, imageCanvas->h - margin * 2.f);
            const float scale = getImageScale();
            const float dw = imageW * scale, dh = imageH * scale;
            const float limitX = std::max(0.f, (dw - availW) * .5f);
            const float limitY = std::max(0.f, (dh - availH) * .5f);
            imageOffsetX = std::clamp(imageOffsetX, -limitX, limitX);
            imageOffsetY = std::clamp(imageOffsetY, -limitY, limitY);
        }

        void setFitZoom()
        {
            imageFitMode = true;
            imageOffsetX = imageOffsetY = 0.f;
            updateZoomLabel();
            refresh();
        }

        void setActualZoom()
        {
            imageFitMode = false;
            imageManualScale = 1.f;
            imageOffsetX = imageOffsetY = 0.f;
            clampImageOffset();
            updateZoomLabel();
            refresh();
        }

        void zoomBy(float factor)
        {
            const float current = getImageScale();
            imageFitMode = false;
            imageManualScale = std::clamp(current * factor, .02f, 8.f);
            clampImageOffset();
            updateZoomLabel();
            refresh();
        }
        D2D1_COLOR_F sampleBackground(const GeminiClient::TranslationBlock& block) const
        {
            if (pixels.empty() || imageW <= 0 || imageH <= 0) return D2D1::ColorF(D2D1::ColorF::White);
            int x1 = std::clamp(block.xmin * imageW / 1000, 0, imageW - 1);
            int x2 = std::clamp(block.xmax * imageW / 1000, x1 + 1, imageW);
            int y1 = std::clamp(block.ymin * imageH / 1000, 0, imageH - 1);
            int y2 = std::clamp(block.ymax * imageH / 1000, y1 + 1, imageH);
            int sx = std::max(1, (x2 - x1) / 16), sy = std::max(1, (y2 - y1) / 10);
            unsigned long long r = 0, g = 0, b = 0, count = 0;
            for (int yy = y1; yy < y2; yy += sy) for (int xx = x1; xx < x2; xx += sx) {
                size_t idx = ((size_t)yy * imageW + xx) * 4;
                b += pixels[idx]; g += pixels[idx + 1]; r += pixels[idx + 2]; ++count;
            }
            if (!count) return D2D1::ColorF(D2D1::ColorF::White);
            return D2D1::ColorF((float)r / count / 255.f, (float)g / count / 255.f,
                (float)b / count / 255.f, 0.97f);
        }

        void paintTranslationBlocks(ID2D1DeviceContext* ctx, const D2D1_RECT_F& imageRect)
        {
            if (!ctx || !showTranslatedImage || translationBlocks.empty()) return;
            const float dw = imageRect.right - imageRect.left;
            const float dh = imageRect.bottom - imageRect.top;
            if (dw <= .5f || dh <= .5f) return;

            auto glyphUnits = [](const std::wstring& text) {
                float units = 0.f;
                for (wchar_t ch : text) {
                    if (ch == L'\r' || ch == L'\n') continue;
                    if (iswspace(ch)) { units += .32f; continue; }
                    if (ch >= 0x2E80) { units += 1.f; continue; }
                    if (iswalnum(ch)) { units += .55f; continue; }
                    units += .38f;
                }
                return std::max(.75f, units);
            };
            auto isBody = [](const GeminiClient::TranslationBlock& b) {
                return b.role.empty() || b.role == L"body";
            };
            auto rectFromBlock = [&](const GeminiClient::TranslationBlock& b) {
                return D2D1::RectF(
                    imageRect.left + dw * b.xmin / 1000.f,
                    imageRect.top + dh * b.ymin / 1000.f,
                    imageRect.left + dw * b.xmax / 1000.f,
                    imageRect.top + dh * b.ymax / 1000.f);
            };

            // The body baseline comes from physical source-line occupied height, not from
            // paragraph area.  A wide two-line paragraph can no longer inflate its font.
            std::vector<float> physicalBody;
            for (const auto& b : translationBlocks) {
                if (isBody(b) && b.sourceLineHeight > 0.f)
                    physicalBody.push_back(dh * b.sourceLineHeight / 1000.f);
            }
            float bodyOccupied = 0.f;
            if (!physicalBody.empty()) {
                std::sort(physicalBody.begin(), physicalBody.end());
                bodyOccupied = physicalBody[physicalBody.size() / 2];
            }

            auto fallbackFont = [&](const GeminiClient::TranslationBlock& b, float bw, float bh) {
                const auto& source = b.source.empty() ? b.translation : b.source;
                const float units = glyphUnits(source);
                const float area = std::sqrt(std::max(.01f, bw * bh) / (units * 1.18f));
                const float line = bh / (1.18f * std::max(1, b.sourceLines));
                return std::max(.01f, area * .55f + line * .45f);
            };

            struct Item {
                GeminiClient::TranslationBlock block;
                D2D1_RECT_F slot{};
                float target{};
                float font{};
                float padX{}, padY{};
            };
            std::vector<Item> items;
            items.reserve(translationBlocks.size());

            for (const auto& b : translationBlocks) {
                Item it;
                it.block = b;
                it.slot = rectFromBlock(b);
                const float bw = std::max(.5f, it.slot.right - it.slot.left);
                const float bh = std::max(.5f, it.slot.bottom - it.slot.top);

                if (b.sourceLineHeight > 0.f) {
                    float occupied = dh * b.sourceLineHeight / 1000.f;
                    if (isBody(b) && bodyOccupied > 0.f) occupied = bodyOccupied * .82f + occupied * .18f;
                    // DirectWrite em size is close to, but not identical with, visible glyph
                    // occupancy. This calibration is relative to measured source ink and is
                    // shared across all screenshot sizes.
                    it.target = occupied * .93f;
                    if (bodyOccupied > 0.f) {
                        if (b.role == L"title") it.target = std::max(it.target, bodyOccupied * 1.30f);
                        else if (b.role == L"heading") it.target = std::max(it.target, bodyOccupied * 1.14f);
                        else if (b.role == L"caption") it.target = std::min(it.target, bodyOccupied * .88f);
                    }
                }
                else {
                    it.target = fallbackFont(b, bw, bh);
                }
                it.target = std::max(.01f, it.target);
                items.push_back(std::move(it));
            }

            auto overlapArea = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
                const float w = std::max(0.f, std::min(a.right, b.right) - std::max(a.left, b.left));
                const float h = std::max(0.f, std::min(a.bottom, b.bottom) - std::max(a.top, b.top));
                return w * h;
            };

            // Local paragraph slots should already be disjoint.  Keep a deterministic
            // midpoint partition only as a fallback for unmatched Gemini geometry.
            for (size_t pass = 0; pass < 2; ++pass) {
                for (size_t i = 0; i < items.size(); ++i) for (size_t j = i + 1; j < items.size(); ++j) {
                    auto& a = items[i].slot; auto& b = items[j].slot;
                    if (overlapArea(a, b) <= .25f) continue;
                    const float acy = (a.top + a.bottom) * .5f, bcy = (b.top + b.bottom) * .5f;
                    const float acx = (a.left + a.right) * .5f, bcx = (b.left + b.right) * .5f;
                    const float xov = std::max(0.f, std::min(a.right,b.right)-std::max(a.left,b.left));
                    const float yov = std::max(0.f, std::min(a.bottom,b.bottom)-std::max(a.top,b.top));
                    if (xov >= yov) {
                        const float mid = (acy + bcy) * .5f;
                        if (acy <= bcy) { a.bottom = std::min(a.bottom, mid); b.top = std::max(b.top, mid); }
                        else { b.bottom = std::min(b.bottom, mid); a.top = std::max(a.top, mid); }
                    } else {
                        const float mid = (acx + bcx) * .5f;
                        if (acx <= bcx) { a.right = std::min(a.right, mid); b.left = std::max(b.left, mid); }
                        else { b.right = std::min(b.right, mid); a.left = std::max(a.left, mid); }
                    }
                }
            }

            auto fits = [](const std::wstring& text, float fs, float w, float h) {
                if (w <= .1f || h <= .1f || fs <= .01f) return false;
                auto tl = Ling::D2D::makeTextLayout(text, fs, w, 16384.f);
                if (!tl) return false;
                DWRITE_TEXT_METRICS m{};
                return SUCCEEDED(tl->GetMetrics(&m)) && m.height <= h + .35f && m.width <= w + .75f;
            };

            int collisions = 0, fitFailures = 0;
            for (size_t i = 0; i < items.size(); ++i)
                for (size_t j = i + 1; j < items.size(); ++j)
                    if (overlapArea(items[i].slot, items[j].slot) > .25f) ++collisions;

            for (auto& it : items) {
                const float sw = std::max(.5f, it.slot.right - it.slot.left);
                const float sh = std::max(.5f, it.slot.bottom - it.slot.top);
                it.padX = std::min(sw * .018f, it.target * .10f);
                it.padY = std::min(sh * .025f, it.target * .06f);
                const float iw = std::max(.25f, sw - it.padX * 2.f);
                const float ih = std::max(.25f, sh - it.padY * 2.f);

                // Preserve original visual size. Never enlarge a short Chinese translation
                // merely because its source paragraph rectangle is wide. Shrink only if the
                // translated text cannot fit inside the source occupied region.
                it.font = it.target;
                if (!fits(it.block.translation, it.font, iw, ih)) {
                    float lo = std::max(.01f, it.target * .08f), hi = it.target;
                    while (lo > .011f && !fits(it.block.translation, lo, iw, ih)) lo *= .5f;
                    for (int k = 0; k < 18; ++k) {
                        const float mid = (lo + hi) * .5f;
                        if (fits(it.block.translation, mid, iw, ih)) lo = mid; else hi = mid;
                    }
                    it.font = std::max(.01f, lo);
                }
                if (!fits(it.block.translation, it.font, iw, ih)) ++fitFailures;
            }

            StarCapDiag::append(std::format(
                L"layout-v023 path=result blocks={} physical_body={:.2f} collisions={} fit_failures={}",
                items.size(), bodyOccupied, collisions, fitFailures));

            for (auto& it : items) {
                if (it.slot.right <= it.slot.left || it.slot.bottom <= it.slot.top) continue;
                // Keep translated screenshot content visually consistent in both app themes.
                // The translation layer is image content, not UI: use the same uniform white
                // surface as direct screenshot translation and always render black text on it.
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush;
                ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), bgBrush.GetAddressOf());
                ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), textBrush.GetAddressOf());
                if (!bgBrush || !textBrush) continue;
                ctx->FillRectangle(it.slot, bgBrush.Get());

                const float iw = std::max(.25f, (it.slot.right-it.slot.left) - it.padX*2.f);
                const float ih = std::max(.25f, (it.slot.bottom-it.slot.top) - it.padY*2.f);
                auto tl = Ling::D2D::makeTextLayout(it.block.translation, it.font, iw, ih);
                if (!tl) continue;
                const bool centered = it.block.role == L"label";
                tl->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
                tl->SetParagraphAlignment(centered ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                if (it.block.role == L"title" || it.block.role == L"heading") {
                    DWRITE_TEXT_RANGE range{0, (UINT32)it.block.translation.size()};
                    tl->SetFontWeight(it.block.role == L"title" ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM, range);
                }
                ctx->DrawTextLayout({it.slot.left + it.padX, it.slot.top + it.padY}, tl.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        void paintImage()
        {
            if (!imageCanvas) return;
            auto ctx = imageCanvas->startPaint(); if (!ctx) return;
            ctx->Clear(D2D1::ColorF(darkMode() ? 0x191A1D : 0xF2F2F2));
            if (imageBitmap && imageW > 0 && imageH > 0) {
                float cw = imageCanvas->w, ch = imageCanvas->h;
                float scale = getImageScale();
                float dw = imageW * scale, dh = imageH * scale;
                clampImageOffset();
                float left = (cw - dw) * .5f + imageOffsetX;
                float top = (ch - dh) * .5f + imageOffsetY;
                auto dest = D2D1::RectF(left, top, left + dw, top + dh);
                // Only the canvas surrounding the screenshot changes with the theme.
                // The bitmap and translated bitmap are intentionally never tinted/dimmed.
                ctx->DrawBitmap(imageBitmap.Get(), dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                if (translating) {
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayBrush, loadingTextBrush;
                    ctx->CreateSolidColorBrush(D2D1::ColorF(.38f, .38f, .38f, .46f), overlayBrush.GetAddressOf());
                    ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), loadingTextBrush.GetAddressOf());
                    if (overlayBrush) ctx->FillRectangle(dest, overlayBrush.Get());
                    if (loadingTextBrush) {
                        const float loadingFont = std::clamp(16.f * dpi, 12.f * dpi, 22.f * dpi);
                        auto loadingLayout = Ling::D2D::makeTextLayout(L"正在翻译中...", loadingFont,
                            std::max(1.f, dw), std::max(1.f, dh));
                        if (loadingLayout) {
                            loadingLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                            loadingLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                            ctx->DrawTextLayout({ dest.left, dest.top }, loadingLayout.Get(), loadingTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    }
                }
                else {
                    paintTranslationBlocks(ctx, dest);
                }
                updateZoomLabel();
            }
            imageCanvas->finishPaint();
        }

        bool makeTranslatedPixels(std::vector<BYTE>& out)
        {
            if (!translationReady || translationBlocks.empty() || !imageBitmap) { out = pixels; return !out.empty(); }
            auto ctx = Ling::D2D::get()->deviceContext.Get();
            D2D1_BITMAP_PROPERTIES1 targetProps{
                .pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
                .dpiX{ 96.f }, .dpiY{ 96.f }, .bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
            };
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
            auto size = D2D1::SizeU((UINT32)imageW, (UINT32)imageH);
            if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &targetProps, target.GetAddressOf()))) return false;
            ctx->SetTarget(target.Get()); ctx->SetTransform(D2D1::Matrix3x2F::Identity()); ctx->BeginDraw();
            ctx->Clear(D2D1::ColorF(0, 0.f));
            D2D1_RECT_F full = D2D1::RectF(0.f, 0.f, (float)imageW, (float)imageH);
            ctx->DrawBitmap(imageBitmap.Get(), full, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            bool prev = showTranslatedImage; showTranslatedImage = true; paintTranslationBlocks(ctx, full); showTranslatedImage = prev;
            auto hr = ctx->EndDraw(); ctx->SetTarget(nullptr); if (FAILED(hr)) return false;
            D2D1_BITMAP_PROPERTIES1 cpuProps{
                .pixelFormat{ target->GetPixelFormat() }, .dpiX{ 96.f }, .dpiY{ 96.f },
                .bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
            };
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
            if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpu.GetAddressOf()))) return false;
            if (FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;
            D2D1_MAPPED_RECT mapped{}; if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
            const size_t rowBytes = (size_t)imageW * 4; out.resize(rowBytes * imageH);
            for (int row = 0; row < imageH; ++row)
                memcpy(out.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
            cpu->Unmap(); return true;
        }

        void startGeminiTranslation()
        {
            if (translating) return;
            if (translationReady) { showMode(!showTranslatedImage); return; }
            auto setting = Setting::get();
            auto apiKey = setting ? setting->getGeminiApiKey() : L"";
            auto model = setting ? setting->getGeminiModel() : L"gemini-3.7-flash";
            if (apiKey.empty()) { if (status) status->setText(L"请先在“设置 > 通用设置”填写 Gemini API Key"); return; }
            translating = true;
            if (translateBtn) translateBtn->setText(L"翻译中...");
            if (textBox) {
                textBox->setBg(uiLoadingBg());
                textBox->setText(L"正在翻译中...");
            }
            refresh();
            // Always re-read the complete image for layout-aware translation.
            // OCR blocks are optimized for text extraction/copying, but can be split by visual
            // lines. Reusing those blocks for translation causes cramped, fragmented Chinese.
            // The full-image path returns paragraph/title regions and uses the same renderer as
            // direct screenshot translation.
            const bool preferImageTranslation = true;
            if (status) status->setText(preferImageTranslation
                ? L"正在按完整图片版式翻译..."
                : (geminiOcrBlocks.empty()
                    ? L"正在让 Gemini 识别图片并翻译..."
                    : L"正在翻译已识别文字（无需再次上传图片）..."));
            const auto myRequest = requestId.load();
            auto blocks = geminiOcrBlocks;
            auto imagePixels = pixels;
            std::thread([blocks = std::move(blocks), imagePixels = std::move(imagePixels),
                width = imageW, height = imageH, apiKey = std::move(apiKey), model = std::move(model),
                myRequest, preferImageTranslation]() mutable {
                GeminiClient::TranslationResult r;
                if (!blocks.empty() && !preferImageTranslation) r = GeminiClient::translateOcrBlocks(blocks, apiKey, model);
                else r = GeminiClient::translateImage(imagePixels, width, height, apiKey, model);
                Ling::App::get()->dq.TryEnqueue([r = std::move(r), myRequest]() mutable {
                    if (requestId.load() != myRequest || !activeWindow) return;
                    activeWindow->setTranslationResult(std::move(r));
                });
            }).detach();
        }

        void openAnnotationEditor()
        {
            if (pixels.empty() || imageW <= 0 || imageH <= 0) return;
            MONITORINFO mi{ sizeof(MONITORINFO) };
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!mon || !GetMonitorInfo(mon, &mi)) return;
            int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
            int posX = mi.rcWork.left + (workW - std::min(imageW, workW)) / 2;
            int posY = mi.rcWork.top + (workH - std::min(imageH, workH)) / 2;
            std::vector<BYTE> editorPixels;
            if (showTranslatedImage && translationReady) makeTranslatedPixels(editorPixels);
            if (editorPixels.empty()) editorPixels = pixels;
            WinPin::initEditorFromData(posX, posY, imageW, imageH, editorPixels);
            if (status) status->setText(L"已打开标注编辑器；当前图片状态会带入编辑器");
        }

        void showImageContextMenu()
        {
            HMENU menu = CreatePopupMenu(); if (!menu) return;
            AppendMenuW(menu, MF_STRING, 3, L"标注 / 马赛克...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 1, L"复制图片");
            AppendMenuW(menu, MF_STRING, 2, L"另存为 PNG...");
            POINT pt{}; GetCursorPos(&pt); SetForegroundWindow(hwnd);
            UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu); PostMessageW(hwnd, WM_NULL, 0, 0);
            std::vector<BYTE> currentPixels;
            if (showTranslatedImage && translationReady) makeTranslatedPixels(currentPixels);
            if (currentPixels.empty()) currentPixels = pixels;
            if (cmd == 3) openAnnotationEditor();
            else if (cmd == 1 && !currentPixels.empty()) Util::saveToClipboard(imageW, imageH, currentPixels.data());
            else if (cmd == 2) {
                auto path = Util::getSaveFilePath(hwnd, L"png");
                if (!path.empty() && !currentPixels.empty()) Util::saveToFile(path, imageW, imageH, currentPixels.data());
            }
        }

    private:
        std::vector<BYTE> pixels;
        int imageW{ 0 }, imageH{ 0 };
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> imageBitmap;
        Ling::Canvas* imageCanvas{ nullptr };
        Ling::TextBox* textBox{ nullptr };
        Ling::Label* status{ nullptr };
        Ling::Label* zoomLabel{ nullptr };
        Ling::Button* zoomFitBtn{ nullptr };
        Ling::Button* zoomActualBtn{ nullptr };
        Ling::Button* zoomOutBtn{ nullptr };
        Ling::Button* zoomInBtn{ nullptr };
        Ling::Button* originalTab{ nullptr };
        Ling::Button* translatedTab{ nullptr };
        Ling::Button* translateBtn{ nullptr };
        bool imageFitMode{ true };
        bool isLongScreenshotSource{ false };
        float imageManualScale{ 1.f };
        float imageOffsetX{ 0.f }, imageOffsetY{ 0.f };
        bool imageDragging{ false };
        POINT dragStart{ 0,0 };
        float dragStartOffsetX{ 0.f }, dragStartOffsetY{ 0.f };
        std::wstring originalText, translatedText;
        std::vector<GeminiClient::OcrBlock> geminiOcrBlocks;
        std::vector<GeminiClient::TranslationBlock> translationBlocks;
        bool translating{ false }, translationReady{ false };
        bool showingTranslationText{ false }, showTranslatedImage{ false };
    };

    inline bool containsPoint(POINT) { return false; }
    inline bool hasWindow() { return activeWindow != nullptr; }

    inline void showPixels(std::vector<BYTE> pixels, int width, int height, bool fromLongScreenshot = false)
    {
        if (pixels.empty() || width <= 0 || height <= 0) return;
        if (pixels.size() < (size_t)width * (size_t)height * 4) return;
        const auto myRequest = ++requestId;
        if (activeWindow) activeWindow->close();
        activeWindow = new OcrResultWindow(pixels, width, height, fromLongScreenshot);
        activeWindow->open();

        auto setting = Setting::get();
        auto apiKey = setting ? setting->getGeminiApiKey() : L"";
        auto model = setting ? setting->getGeminiModel() : L"gemini-3.7-flash";
        if (!apiKey.empty()) {
            std::thread([pixels = std::move(pixels), width, height, apiKey = std::move(apiKey),
                model = std::move(model), myRequest]() mutable {
                auto result = GeminiClient::recognizeImage(pixels, width, height, apiKey, model);
                Ling::App::get()->dq.TryEnqueue([result = std::move(result), myRequest]() mutable {
                    if (requestId.load() != myRequest || !activeWindow) return;
                    activeWindow->setGeminiOcrResult(std::move(result));
                });
            }).detach();
        }
        else {
            std::thread([pixels = std::move(pixels), width, height, myRequest]() mutable {
                auto result = recognizeWindows(std::move(pixels), width, height);
                Ling::App::get()->dq.TryEnqueue([result = std::move(result), myRequest]() mutable {
                    if (requestId.load() != myRequest || !activeWindow) return;
                    activeWindow->setLocalOcrResult(std::move(result));
                });
            }).detach();
        }
    }

    inline void showTranslationPixels(std::vector<BYTE> pixels, int width, int height, bool fromLongScreenshot = false)
    {
        if (pixels.empty() || width <= 0 || height <= 0) return;
        if (pixels.size() < (size_t)width * (size_t)height * 4) return;
        ++requestId;
        if (activeWindow) activeWindow->close();
        activeWindow = new OcrResultWindow(std::move(pixels), width, height, fromLongScreenshot);
        activeWindow->open();
        activeWindow->beginImageTranslation();
    }
    inline void show(WinCap* win)
    {
        if (!win || !win->cutMask || !win->cutMask->hasRect()) return;
        std::vector<BYTE> pixels; int width{ 0 }, height{ 0 };
        if (!copyCutPixels(win, pixels, width, height)) return;
        showPixels(std::move(pixels), width, height);
        win->close();
    }
}














