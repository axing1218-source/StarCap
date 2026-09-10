from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8-sig")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("patched", label)


# 1) Clipboard: keep relative time and append exact local time.
path = "Src/ClipboardHistoryV099Utools_part4.inc"
old = '''    inline void v099DrawBottomMeta(HDC dc, const Item& item, const RECT& panel, bool expandable)
    {
        RECT timeRc{ panel.left + 14, panel.bottom - 24, panel.left + 130, panel.bottom - 7 };
        drawText(dc, relativeTime(item.updated), timeRc, v099Faint(), smallFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
'''
new = '''    inline std::wstring v099TimeLabel(uint64_t ticks)
    {
        auto relative = relativeTime(ticks);
        ULARGE_INTEGER value{};
        value.QuadPart = ticks;
        FILETIME ft{ value.LowPart, value.HighPart };
        SYSTEMTIME utc{}, local{};
        if (!FileTimeToSystemTime(&ft, &utc) ||
            !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return relative;
        return std::format(L"{}（{:04}-{:02}-{:02} {:02}:{:02}）", relative,
            local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    }

    inline void v099DrawBottomMeta(HDC dc, const Item& item, const RECT& panel, bool expandable)
    {
        RECT timeRc{ panel.left + 14, panel.bottom - 24,
            std::min(panel.left + 300, panel.right - 185), panel.bottom - 7 };
        drawText(dc, v099TimeLabel(item.updated), timeRc, v099Faint(), smallFont,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
'''
replace_once(path, old, new, "clipboard exact timestamp")


# 2) OCR V2: replace the singleton request/window routing with a live-window registry.
path = "Src/StarCapOcrV2.h"
replace_once(
    path,
    '#include "StarCapParagraphLayout.h"\n',
    '#include "StarCapParagraphLayout.h"\n#include "StarCapTranslationLanguage.h"\n',
    "OCR translation language include",
)
replace_once(
    path,
    '''    class OcrResultWindow;
    inline OcrResultWindow* activeWindow{ nullptr };
    inline std::atomic<unsigned long long> requestId{ 0 };
''',
    '''    class OcrResultWindow;
    // Compatibility pointer to the newest OCR window. Async work is routed by window id.
    inline OcrResultWindow* activeWindow{ nullptr };
    inline std::vector<OcrResultWindow*> windows;
    inline std::atomic<unsigned long long> nextWindowId{ 0 };
    inline OcrResultWindow* findWindowById(unsigned long long id);
''',
    "OCR window registry",
)
replace_once(
    path,
    '''            : pixels(std::move(data)), imageW(imgW), imageH(imgH), isLongScreenshotSource(fromLongScreenshot)
        {
            setTitle(L"StarCap - 文字识别 / 翻译");
''',
    '''            : pixels(std::move(data)), imageW(imgW), imageH(imgH), isLongScreenshotSource(fromLongScreenshot)
        {
            windowId = ++nextWindowId;
            targetLanguageIndex = StarCapTranslationLanguage::defaultIndex();
            setTitle(L"StarCap - 文字识别 / 翻译");
''',
    "OCR per-window identity",
)
replace_once(
    path,
    '''            onDestroy.add([this]() {
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
''',
    '''            onDestroy.add([this]() {
                windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
                if (activeWindow == this) activeWindow = windows.empty() ? nullptr : windows.back();
                auto dying = this;
                Ling::App::get()->dq.TryEnqueue([dying]() {
                    delete dying;
                    auto app = Ling::App::get();
                    auto it = app->args.find(L"--auto-quit");
                    if (it != app->args.end() && it->second == L"true" &&
                        windows.empty() && !WinPin::hasWindow()) app->quit(0);
                });
            });
''',
    "OCR destroy lifetime",
)
replace_once(
    path,
    '''        void open() { createNativeWindow(0, WS_OVERLAPPEDWINDOW); }
''',
    '''        void open() { createNativeWindow(0, WS_OVERLAPPEDWINDOW); }
        unsigned long long id() const { return windowId; }
        int getTargetLanguageIndex() const { return targetLanguageIndex; }
        void setTargetLanguageIndex(int index) { targetLanguageIndex = StarCapTranslationLanguage::clampIndex(index); }
        void cascade(size_t ordinal)
        {
            if (!hwnd || ordinal <= 1) return;
            RECT rc{};
            if (!GetWindowRect(hwnd, &rc)) return;
            const int offset = 28 * static_cast<int>((ordinal - 1) % 6);
            SetWindowPos(hwnd, nullptr, rc.left + offset, rc.top + offset, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
''',
    "OCR public window helpers",
)
replace_once(
    path,
    '''            const auto myRequest = requestId.load();
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
''',
    '''            const auto myWindowId = windowId;
            auto blocks = geminiOcrBlocks;
            auto imagePixels = pixels;
            auto targetLanguage = StarCapTranslationLanguage::prompt(targetLanguageIndex);
            std::thread([blocks = std::move(blocks), imagePixels = std::move(imagePixels),
                width = imageW, height = imageH, apiKey = std::move(apiKey), model = std::move(model),
                targetLanguage = std::move(targetLanguage), myWindowId, preferImageTranslation]() mutable {
                GeminiClient::TranslationResult r;
                if (!blocks.empty() && !preferImageTranslation)
                    r = GeminiClient::translateOcrBlocks(blocks, apiKey, model, targetLanguage);
                else r = GeminiClient::translateImage(imagePixels, width, height, apiKey, model, targetLanguage);
                Ling::App::get()->dq.TryEnqueue([r = std::move(r), myWindowId]() mutable {
                    auto* target = findWindowById(myWindowId);
                    if (!target) return;
                    target->setTranslationResult(std::move(r));
                });
            }).detach();
''',
    "OCR translation result routing",
)
replace_once(
    path,
    '''    private:
        std::vector<BYTE> pixels;
''',
    '''    private:
        unsigned long long windowId{ 0 };
        int targetLanguageIndex{ 0 };
        std::vector<BYTE> pixels;
''',
    "OCR per-window members",
)
old_tail = '''    inline bool containsPoint(POINT) { return false; }
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
'''
new_tail = '''    inline OcrResultWindow* findWindowById(unsigned long long id)
    {
        for (auto* window : windows) {
            if (window && window->id() == id) return window;
        }
        return nullptr;
    }

    inline bool containsPoint(POINT point)
    {
        for (auto* window : windows) {
            if (!window || !window->hwnd || !IsWindow(window->hwnd) || !IsWindowVisible(window->hwnd)) continue;
            RECT rc{};
            if (GetWindowRect(window->hwnd, &rc) && PtInRect(&rc, point)) return true;
        }
        return false;
    }
    inline bool hasWindow() { return !windows.empty(); }

    inline void showPixels(std::vector<BYTE> pixels, int width, int height, bool fromLongScreenshot = false)
    {
        if (pixels.empty() || width <= 0 || height <= 0) return;
        if (pixels.size() < (size_t)width * (size_t)height * 4) return;
        auto* target = new OcrResultWindow(pixels, width, height, fromLongScreenshot);
        windows.push_back(target);
        activeWindow = target;
        target->open();
        target->cascade(windows.size());
        const auto myWindowId = target->id();

        auto setting = Setting::get();
        auto apiKey = setting ? setting->getGeminiApiKey() : L"";
        auto model = setting ? setting->getGeminiModel() : L"gemini-3.7-flash";
        if (!apiKey.empty()) {
            std::thread([pixels = std::move(pixels), width, height, apiKey = std::move(apiKey),
                model = std::move(model), myWindowId]() mutable {
                auto result = GeminiClient::recognizeImage(pixels, width, height, apiKey, model);
                Ling::App::get()->dq.TryEnqueue([result = std::move(result), myWindowId]() mutable {
                    auto* window = findWindowById(myWindowId);
                    if (!window) return;
                    window->setGeminiOcrResult(std::move(result));
                });
            }).detach();
        }
        else {
            std::thread([pixels = std::move(pixels), width, height, myWindowId]() mutable {
                auto result = recognizeWindows(std::move(pixels), width, height);
                Ling::App::get()->dq.TryEnqueue([result = std::move(result), myWindowId]() mutable {
                    auto* window = findWindowById(myWindowId);
                    if (!window) return;
                    window->setLocalOcrResult(std::move(result));
                });
            }).detach();
        }
    }

    inline void showTranslationPixels(std::vector<BYTE> pixels, int width, int height, bool fromLongScreenshot = false)
    {
        if (pixels.empty() || width <= 0 || height <= 0) return;
        if (pixels.size() < (size_t)width * (size_t)height * 4) return;
        auto* target = new OcrResultWindow(std::move(pixels), width, height, fromLongScreenshot);
        windows.push_back(target);
        activeWindow = target;
        target->open();
        target->cascade(windows.size());
        target->beginImageTranslation();
    }
'''
replace_once(path, old_tail, new_tail, "OCR multi-window open and async routing")


# 3) Wrapper: retain old OCR windows and allow explicit per-window translation target.
path = "Src/StarCapOcr.h"
old = '''namespace GeminiClient
{
    inline TranslationResult translateImageStarCapTarget(
        const std::vector<BYTE>& pixels, int width, int height,
        const std::wstring& apiKey, const std::wstring& model)
    {
        return translateImage(pixels, width, height, apiKey, model,
            StarCapTranslationLanguage::activePrompt());
    }

    inline TranslationResult translateOcrBlocksStarCapTarget(
        const std::vector<OcrBlock>& sourceBlocks,
        const std::wstring& apiKey, const std::wstring& model)
    {
        return translateOcrBlocks(sourceBlocks, apiKey, model,
            StarCapTranslationLanguage::activePrompt());
    }
}
'''
new = '''namespace GeminiClient
{
    inline TranslationResult translateImageStarCapTarget(
        const std::vector<BYTE>& pixels, int width, int height,
        const std::wstring& apiKey, const std::wstring& model)
    {
        return translateImage(pixels, width, height, apiKey, model,
            StarCapTranslationLanguage::prompt(StarCapTranslationLanguage::defaultIndex()));
    }

    inline TranslationResult translateImageStarCapTarget(
        const std::vector<BYTE>& pixels, int width, int height,
        const std::wstring& apiKey, const std::wstring& model, const std::wstring& targetLanguage)
    {
        return translateImage(pixels, width, height, apiKey, model, targetLanguage);
    }

    inline TranslationResult translateOcrBlocksStarCapTarget(
        const std::vector<OcrBlock>& sourceBlocks,
        const std::wstring& apiKey, const std::wstring& model)
    {
        return translateOcrBlocks(sourceBlocks, apiKey, model,
            StarCapTranslationLanguage::prompt(StarCapTranslationLanguage::defaultIndex()));
    }

    inline TranslationResult translateOcrBlocksStarCapTarget(
        const std::vector<OcrBlock>& sourceBlocks,
        const std::wstring& apiKey, const std::wstring& model, const std::wstring& targetLanguage)
    {
        return translateOcrBlocks(sourceBlocks, apiKey, model, targetLanguage);
    }
}
'''
replace_once(path, old, new, "OCR explicit target-language wrappers")
p = Path(path)
text = p.read_text(encoding="utf-8-sig")
close_line = "        if (StarCapOcrV2::activeWindow) StarCapOcrV2::activeWindow->close();\n"
session_line = "        StarCapTranslationLanguage::resetSessionToDefault();\n"
if text.count(close_line) != 3:
    raise SystemExit(f"wrapper close count changed: {text.count(close_line)}")
if text.count(session_line) != 3:
    raise SystemExit(f"wrapper session count changed: {text.count(session_line)}")
text = text.replace(close_line, "").replace(session_line, "")
p.write_text(text, encoding="utf-8")
marker = '''    inline bool hasWindow()
    {
        return StarCapOcrV2::hasWindow();
    }
'''
addition = marker + '''
    // Long capture needs the application behind OCR result windows to remain the scroll target.
    inline std::vector<HWND> captureHiddenWindows;

    inline void suspendForLongCapture()
    {
        if (!captureHiddenWindows.empty()) return;
        for (auto* window : StarCapOcrV2::windows) {
            if (!window || !window->hwnd || !IsWindow(window->hwnd) || !IsWindowVisible(window->hwnd)) continue;
            captureHiddenWindows.push_back(window->hwnd);
            ShowWindow(window->hwnd, SW_HIDE);
        }
    }

    inline void restoreAfterLongCapture()
    {
        for (HWND hwnd : captureHiddenWindows) {
            if (hwnd && IsWindow(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        captureHiddenWindows.clear();
    }
'''
replace_once(path, marker, addition, "OCR long-capture suspend/restore")


# 4) Translation language UI: keep one controller and one temporary language selection per OCR window.
path = "Src/StarCapOcrTranslationLanguageUI.h"
replace_once(path, '#include <memory>\n#include <cmath>\n', '#include <memory>\n#include <vector>\n#include <cmath>\n', "language UI vector include")
replace_once(
    path,
    '''        explicit Controller(StarCapOcrV2::OcrResultWindow* owner) : owner(owner)
        {
            install();
        }

    private:
''',
    '''        explicit Controller(StarCapOcrV2::OcrResultWindow* owner) : owner(owner)
        {
            install();
        }
        StarCapOcrV2::OcrResultWindow* window() const { return owner; }

    private:
''',
    "language controller window accessor",
)
replace_once(
    path,
    '''            targetBtn->setText(StarCapTranslationLanguage::label(
                StarCapTranslationLanguage::activeIndex()) + L"  ▾");
''',
    '''            targetBtn->setText(StarCapTranslationLanguage::label(
                owner->getTargetLanguageIndex()) + L"  ▾");
''',
    "language initial per-window label",
)
replace_once(path, '            const int current = StarCapTranslationLanguage::activeIndex();\n', '            const int current = owner->getTargetLanguageIndex();\n', "language current per-window selection")
replace_once(path, '            StarCapTranslationLanguage::sessionIndex = StarCapTranslationLanguage::clampIndex(selected);\n', '            owner->setTargetLanguageIndex(selected);\n', "language save per-window selection")
old = '''    inline std::unique_ptr<Controller> controllerOwner;
    inline StarCapOcrV2::OcrResultWindow* activeLanguageWindow{ nullptr };

    inline void detach(StarCapOcrV2::OcrResultWindow* window = nullptr)
    {
        if (window && activeLanguageWindow != window) return;
        controllerOwner.reset();
        activeLanguageWindow = nullptr;
        StarCapTranslationLanguage::clearSession();
    }

    inline void attach(StarCapOcrV2::OcrResultWindow* window)
    {
        detach();
        if (!window) return;
        StarCapTranslationLanguage::resetSessionToDefault();
        controllerOwner = std::make_unique<Controller>(window);
        activeLanguageWindow = window;
        window->onDestroy.add([window]() {
            if (activeLanguageWindow == window) detach(window);
        });
    }
'''
new = '''    inline std::vector<std::unique_ptr<Controller>> controllerOwners;

    inline void detach(StarCapOcrV2::OcrResultWindow* window)
    {
        for (auto it = controllerOwners.begin(); it != controllerOwners.end(); ) {
            if ((*it)->window() == window) it = controllerOwners.erase(it);
            else ++it;
        }
    }

    inline void attach(StarCapOcrV2::OcrResultWindow* window)
    {
        if (!window) return;
        for (const auto& controller : controllerOwners) {
            if (controller->window() == window) return;
        }
        controllerOwners.push_back(std::make_unique<Controller>(window));
        window->onDestroy.add([window]() { detach(window); });
    }
'''
replace_once(path, old, new, "language controller multi-window lifetime")


# 5) Long capture: temporarily hide OCR windows during capture so they cannot cover pixels
# or become the UIA/wheel target. Restore them as soon as capture finishes or is disposed.
path = "Src/Win/CapLong.cpp"
replace_once(path, '    isCapturing = true;\n    win->hollowWin();\n', '    isCapturing = true;\n    StarCapOcr::suspendForLongCapture();\n    win->hollowWin();\n', "long capture hide OCR windows")
replace_once(
    path,
    '''CapLong::~CapLong()
{
    uninstallControlHook();
    releaseUiaScroll();
}
''',
    '''CapLong::~CapLong()
{
    uninstallControlHook();
    releaseUiaScroll();
    StarCapOcr::restoreAfterLongCapture();
}
''',
    "long capture destructor restore OCR",
)
replace_once(
    path,
    '''    uninstallControlHook();
    releaseUiaScroll();
    if (tool) tool->close();
}
''',
    '''    uninstallControlHook();
    releaseUiaScroll();
    StarCapOcr::restoreAfterLongCapture();
    if (tool) tool->close();
}
''',
    "long capture dispose restore OCR",
)
replace_once(
    path,
    '''    uninstallControlHook();
    releaseUiaScroll();
    if (showMessage) makeStopText();
''',
    '''    uninstallControlHook();
    releaseUiaScroll();
    StarCapOcr::restoreAfterLongCapture();
    if (showMessage) makeStopText();
''',
    "long capture stop restore OCR",
)

v2 = Path("Src/StarCapOcrV2.h").read_text(encoding="utf-8")
if "if (activeWindow) activeWindow->close();" in v2:
    raise SystemExit("old OCR singleton close remains")
if "requestId" in v2:
    raise SystemExit("old global OCR requestId remains")
print("All source patches applied successfully.")
