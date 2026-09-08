#pragma once

#include <include/Ling.h>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include "Win/WinCap.h"
#include "Win/CutMask.h"
#include "Setting.h"
#include "Util.h"
#include "GeminiClient.h"
#include "StarCapTextGeometry.h"
#include "StarCapParagraphLayout.h"

namespace StarCapCaptureTranslate
{
    class LoadingOverlay : public Ling::WinBase
    {
    public:
        LoadingOverlay(int screenX, int screenY, int imageW, int imageH,
            std::vector<BYTE> pixels, float borderWidth)
            : pixels(std::move(pixels)), imageW(imageW), imageH(imageH), borderWidth(borderWidth)
        {
            x = screenX; y = screenY; w = (float)imageW; h = (float)imageH;
            disableWinAnimation();
        }

        void open()
        {
            createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, WS_POPUP);
        }

    protected:
        void onCreated() override
        {
            disableBorderRadius();
            canvas = body->makeChild<Ling::Canvas>();
            canvas->enableSwapChain();
            canvas->setSizePercent(100.f, 100.f);
            if (imageW > 0 && imageH > 0 && !pixels.empty()) {
                D2D1_BITMAP_PROPERTIES1 props{};
                props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
                props.dpiX = 96.f; props.dpiY = 96.f;
                Ling::D2D::get()->deviceContext->CreateBitmap(
                    D2D1::SizeU((UINT32)imageW, (UINT32)imageH), pixels.data(), (UINT32)imageW * 4,
                    &props, imageBitmap.GetAddressOf());
            }
            show();
        }

        void layout() override
        {
            Ling::WinBase::layout();
            if (!canvas) return;
            auto ctx = canvas->startPaint();
            if (!ctx) return;
            ctx->Clear(0);
            auto full = D2D1::RectF(0.f, 0.f, (float)imageW, (float)imageH);
            if (imageBitmap) ctx->DrawBitmap(imageBitmap.Get(), full, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dimBrush, textBrush, borderBrush;
            ctx->CreateSolidColorBrush(D2D1::ColorF(.36f, .36f, .36f, .54f), dimBrush.GetAddressOf());
            ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
            if (dimBrush) ctx->FillRectangle(full, dimBrush.Get());

            if (textBrush && imageW > 0 && imageH > 0) {
                float fontSize = std::min(18.f * dpi, std::max(6.f * dpi, imageH * .28f));
                auto tl = Ling::D2D::makeTextLayout(L"正在翻译中...", fontSize,
                    std::max(1.f, (float)imageW), std::max(1.f, (float)imageH));
                if (tl) {
                    tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    ctx->DrawTextLayout({ 0.f, 0.f }, tl.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            if (borderWidth > 0.f) {
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f, 0.9f), borderBrush.GetAddressOf());
                if (borderBrush) ctx->DrawRectangle(full, borderBrush.Get(), borderWidth);
            }
            canvas->finishPaint();
        }

        LRESULT onHitTest(const POINT) override { return HTTRANSPARENT; }

    private:
        std::vector<BYTE> pixels;
        int imageW{ 0 }, imageH{ 0 };
        float borderWidth{ 0.f };
        Ling::Canvas* canvas{ nullptr };
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> imageBitmap;
    };

    class TranslationOverlay : public Ling::WinBase
    {
    public:
        TranslationOverlay(int screenX, int screenY, int imageW, int imageH,
            std::vector<BYTE> pixels, std::vector<GeminiClient::TranslationBlock> blocks, float borderWidth,
            WinCap* captureOwner)
            : pixels(std::move(pixels)), imageW(imageW), imageH(imageH), blocks(std::move(blocks)),
              borderWidth(borderWidth), captureOwner(captureOwner)
        {
            StarCapTextGeometry::stabilize(this->blocks, this->pixels, imageW, imageH, L"direct");
            StarCapParagraphLayout::apply(this->blocks, this->pixels, imageW, imageH, L"direct");
            x = screenX; y = screenY; w = (float)imageW; h = (float)imageH;
            disableWinAnimation();
            onKeyDown.add([this](UINT key) {
                if (key == VK_RETURN) {
                    copyTranslatedToClipboardAndClose();
                    return;
                }
                if (key != VK_ESCAPE) return;
                auto* target = this->captureOwner;
                hide();
                // Do not destroy this overlay from inside its own key callback. Queue the
                // capture close; WinCap's destroy hook then resets the translation state.
                Ling::App::get()->dq.TryEnqueue([target]() {
                    if (target && WinCap::get() == target) target->close();
                });
            });
        }

        void open()
        {
            createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, WS_POPUP);
            if (hwnd) {
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
            }
        }

    protected:
        void onCreated() override
        {
            disableBorderRadius();
            canvas = body->makeChild<Ling::Canvas>();
            canvas->enableSwapChain();
            canvas->setSizePercent(100.f, 100.f);
            if (imageW > 0 && imageH > 0 && !pixels.empty()) {
                D2D1_BITMAP_PROPERTIES1 props{};
                props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
                props.dpiX = 96.f; props.dpiY = 96.f;
                Ling::D2D::get()->deviceContext->CreateBitmap(
                    D2D1::SizeU((UINT32)imageW, (UINT32)imageH), pixels.data(), (UINT32)imageW * 4,
                    &props, imageBitmap.GetAddressOf());
            }
            show();
        }

        void layout() override
        {
            Ling::WinBase::layout();
            if (!canvas) return;
            auto ctx = canvas->startPaint();
            if (!ctx) return;
            ctx->Clear(0);

            auto full = D2D1::RectF(0.f, 0.f, (float)imageW, (float)imageH);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> backgroundBrush;
            ctx->CreateSolidColorBrush(
                D2D1::ColorF(254.f / 255.f, 254.f / 255.f, 254.f / 255.f, 1.f),
                backgroundBrush.GetAddressOf());
            if (backgroundBrush) ctx->FillRectangle(full, backgroundBrush.Get());

            paintBlocks(ctx, full);
            if (borderWidth > 0.f) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f, 0.9f), border.GetAddressOf());
                if (border) ctx->DrawRectangle(full, border.Get(), borderWidth);
            }
            canvas->finishPaint();
        }

        LRESULT onHitTest(const POINT) override { return HTTRANSPARENT; }

    private:
        bool renderTranslatedPixels(std::vector<BYTE>& out)
        {
            out.clear();
            if (imageW <= 0 || imageH <= 0) return false;

            Microsoft::WRL::ComPtr<ID2D1Device> device;
            Ling::D2D::get()->deviceContext->GetDevice(device.GetAddressOf());
            if (!device) return false;

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> offscreen;
            if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                offscreen.GetAddressOf())) || !offscreen) return false;

            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            targetProps.dpiX = 96.f;
            targetProps.dpiY = 96.f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
            if (FAILED(offscreen->CreateBitmap(D2D1::SizeU((UINT32)imageW, (UINT32)imageH),
                nullptr, 0, &targetProps, target.GetAddressOf())) || !target) return false;

            offscreen->SetTarget(target.Get());
            offscreen->BeginDraw();
            constexpr float c = 254.f / 255.f;
            offscreen->Clear(D2D1::ColorF(c, c, c, 1.f));
            paintBlocks(offscreen.Get(), D2D1::RectF(0.f, 0.f, (float)imageW, (float)imageH));
            if (FAILED(offscreen->EndDraw())) return false;

            D2D1_BITMAP_PROPERTIES1 cpuProps{};
            cpuProps.pixelFormat = targetProps.pixelFormat;
            cpuProps.dpiX = 96.f;
            cpuProps.dpiY = 96.f;
            cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
            if (FAILED(offscreen->CreateBitmap(D2D1::SizeU((UINT32)imageW, (UINT32)imageH),
                nullptr, 0, &cpuProps, cpu.GetAddressOf())) || !cpu) return false;
            if (FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;

            D2D1_MAPPED_RECT mapped{};
            if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
            const size_t rowBytes = (size_t)imageW * 4;
            out.resize(rowBytes * (size_t)imageH);
            for (int row = 0; row < imageH; ++row) {
                BYTE* dst = out.data() + (size_t)row * rowBytes;
                memcpy(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
                for (size_t i = 3; i < rowBytes; i += 4) dst[i] = 255;
            }
            cpu->Unmap();
            return true;
        }

        void copyTranslatedToClipboardAndClose()
        {
            std::vector<BYTE> rendered;
            if (!renderTranslatedPixels(rendered) || rendered.empty()) return;
            Util::saveToClipboard(imageW, imageH, rendered.data());

            auto* target = captureOwner;
            hide();
            Ling::App::get()->dq.TryEnqueue([target]() {
                if (target && WinCap::get() == target) target->close();
            });
        }

        D2D1_COLOR_F sampleBackground(const GeminiClient::TranslationBlock& block) const
        {
            if (pixels.empty()) return D2D1::ColorF(D2D1::ColorF::White);
            int x1 = std::clamp(block.xmin * imageW / 1000, 0, imageW - 1);
            int x2 = std::clamp(block.xmax * imageW / 1000, x1 + 1, imageW);
            int y1 = std::clamp(block.ymin * imageH / 1000, 0, imageH - 1);
            int y2 = std::clamp(block.ymax * imageH / 1000, y1 + 1, imageH);
            int sx = std::max(1, (x2 - x1) / 16), sy = std::max(1, (y2 - y1) / 10);
            unsigned long long r = 0, g = 0, b = 0, n = 0;
            for (int yy = y1; yy < y2; yy += sy) for (int xx = x1; xx < x2; xx += sx) {
                size_t idx = ((size_t)yy * imageW + xx) * 4;
                b += pixels[idx]; g += pixels[idx + 1]; r += pixels[idx + 2]; ++n;
            }
            if (!n) return D2D1::ColorF(D2D1::ColorF::White);
            return D2D1::ColorF((float)r / n / 255.f, (float)g / n / 255.f, (float)b / n / 255.f, .97f);
        }

        void paintBlocks(ID2D1DeviceContext* ctx, const D2D1_RECT_F& imageRect)
        {
            if (!ctx || blocks.empty()) return;
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
            for (const auto& b : blocks) {
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
            items.reserve(blocks.size());

            for (const auto& b : blocks) {
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
                L"layout-v023 path=direct blocks={} physical_body={:.2f} collisions={} fit_failures={}",
                items.size(), bodyOccupied, collisions, fitFailures));

            for (auto& it : items) {
                if (it.slot.right <= it.slot.left || it.slot.bottom <= it.slot.top) continue;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
                ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), textBrush.GetAddressOf());
                if (!textBrush) continue;

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

        std::vector<BYTE> pixels;
        int imageW{ 0 }, imageH{ 0 };
        std::vector<GeminiClient::TranslationBlock> blocks;
        float borderWidth{ 0.f };
        WinCap* captureOwner{ nullptr };
        Ling::Canvas* canvas{ nullptr };
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> imageBitmap;
    };

    inline std::unique_ptr<TranslationOverlay> overlay;
    inline std::unique_ptr<LoadingOverlay> loadingOverlay;
    inline WinCap* owner{ nullptr };
    inline std::atomic<unsigned long long> requestId{ 0 };
    inline bool busy{ false }, ready{ false }, showing{ false }, hooksInstalled{ false };
    inline int cachedX{ 0 }, cachedY{ 0 }, cachedW{ 0 }, cachedH{ 0 };

    inline HWND translatedViewHwnd(WinCap* win)
    {
        if (owner != win || !ready || !showing || !overlay || !overlay->hwnd) return nullptr;
        return IsWindowVisible(overlay->hwnd) ? overlay->hwnd : nullptr;
    }

    inline void closeOverlay()
    {
        if (overlay) { overlay->close(); overlay.reset(); }
        if (loadingOverlay) { loadingOverlay->close(); loadingOverlay.reset(); }
        showing = false;
    }

    inline void reset(WinCap* expected = nullptr)
    {
        if (expected && owner != expected) return;
        ++requestId;
        closeOverlay();
        owner = nullptr; busy = false; ready = false; hooksInstalled = false;
        cachedX = cachedY = cachedW = cachedH = 0;
    }

    inline bool copyCutPixels(WinCap* win, std::vector<BYTE>& pixels, int& outW, int& outH)
    {
        if (!win) return false;
        auto img = win->getCutImg(); if (!img) return false;
        auto size = img->GetPixelSize(); if (!size.width || !size.height) return false;
        outW = (int)size.width; outH = (int)size.height;
        D2D1_BITMAP_PROPERTIES1 prop{};
        prop.pixelFormat = img->GetPixelFormat(); prop.dpiX = 96.f; prop.dpiY = 96.f;
        prop.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
        auto ctx = Ling::D2D::get()->deviceContext;
        if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &prop, cpu.GetAddressOf()))) return false;
        if (FAILED(cpu->CopyFromBitmap(nullptr, img.Get(), nullptr))) return false;
        D2D1_MAPPED_RECT mapped{}; if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        size_t rowBytes = (size_t)outW * 4; pixels.resize(rowBytes * outH);
        for (int row = 0; row < outH; ++row)
            memcpy(pixels.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
        cpu->Unmap(); return true;
    }

    inline void installHooks(WinCap* win)
    {
        if (hooksInstalled || !win) return;
        hooksInstalled = true;
        win->onDestroy.add([win]() { reset(win); });
        // 用户回到截图本体去拖动/调整选区时先露出原图；下次点“译”若选区已变化会重新请求。
        win->onMouseDown.add([win](POINT, bool isRight) {
            if (owner != win || isRight) return;
            if (busy) {
                ++requestId;
                if (loadingOverlay) { loadingOverlay->close(); loadingOverlay.reset(); }
                busy = false;
            }
            if (showing && overlay) {
                overlay->hide(); showing = false;
            }
        });
    }

    inline void toggle(WinCap* win)
    {
        if (!win || !win->cutMask || !win->cutMask->hasRect()) return;
        if (owner && owner != win) reset();
        if (!owner) owner = win;
        installHooks(win);
        auto& r = win->cutMask->maskRect;
        int sx = win->x + (int)r.left, sy = win->y + (int)r.top;
        int sw = std::max(1, (int)(r.right - r.left)), sh = std::max(1, (int)(r.bottom - r.top));

        if (ready && (sx != cachedX || sy != cachedY || sw != cachedW || sh != cachedH)) {
            ++requestId; closeOverlay(); ready = false; busy = false;
        }
        if (ready && overlay) {
            if (showing) {
                overlay->hide();
                showing = false;
                if (win->hwnd) {
                    SetForegroundWindow(win->hwnd);
                    SetFocus(win->hwnd);
                }
            }
            else {
                overlay->show();
                showing = true;
                if (overlay->hwnd) {
                    SetForegroundWindow(overlay->hwnd);
                    SetFocus(overlay->hwnd);
                }
            }
            return;
        }
        if (busy) return;

        auto setting = Setting::get();
        auto apiKey = setting ? setting->getGeminiApiKey() : L"";
        auto model = setting ? setting->getGeminiModel() : L"gemini-3.7-flash";
        if (apiKey.empty()) {
            MessageBoxW(win->hwnd, L"请先在 设置 > 通用设置 中填写并保存 Gemini API Key。",
                L"StarCap 翻译", MB_OK | MB_ICONINFORMATION);
            return;
        }
        busy = true;
        const auto myRequest = ++requestId;
        const float border = win->cutMask->strokeWidth;

        // Dim first, then do the CPU bitmap copy. This removes the short clear flash
        // immediately after clicking Translate.
        loadingOverlay = std::make_unique<LoadingOverlay>(sx, sy, sw, sh, std::vector<BYTE>{}, border);
        loadingOverlay->open();

        std::vector<BYTE> pixels; int width{ 0 }, height{ 0 };
        if (!copyCutPixels(win, pixels, width, height)) {
            if (loadingOverlay) { loadingOverlay->close(); loadingOverlay.reset(); }
            busy = false;
            return;
        }
        cachedX = sx; cachedY = sy; cachedW = width; cachedH = height;

        auto sourcePixels = pixels;
        std::thread([win, pixels = std::move(pixels), sourcePixels = std::move(sourcePixels), width, height,
            apiKey = std::move(apiKey), model = std::move(model), myRequest, sx, sy, border]() mutable {
            auto result = GeminiClient::translateImage(pixels, width, height, apiKey, model);
            Ling::App::get()->dq.TryEnqueue([win, result = std::move(result), sourcePixels = std::move(sourcePixels),
                width, height, myRequest, sx, sy, border]() mutable {
                if (requestId.load() != myRequest || owner != win || WinCap::get() != win) return;
                busy = false;
                if (!result.ok) {
                    if (loadingOverlay) { loadingOverlay->close(); loadingOverlay.reset(); }
                    auto msg = result.error.empty() ? std::wstring(L"Gemini 翻译失败。") : result.error;
                    MessageBoxW(win->hwnd, msg.c_str(), L"StarCap 翻译", MB_OK | MB_ICONWARNING);
                    return;
                }
                overlay = std::make_unique<TranslationOverlay>(sx, sy, width, height,
                    std::move(sourcePixels), std::move(result.blocks), border, win);
                overlay->open();
                // Only remove the dim layer after the translated window is actually open.
                if (loadingOverlay) { loadingOverlay->close(); loadingOverlay.reset(); }
                ready = true; showing = true;
            });
        }).detach();
    }
}











