#include "pch.h"
#include <include/Ling.h>
#include "CapLong.h"
#include "WinCap.h"
#include "CutMask.h"
#include "WinPin.h"
#include "../Tool/ToolLong.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../StarCapDiag.h"
using namespace Microsoft::WRL;

namespace {
    constexpr UINT scrollMsgId = 18;
    constexpr UINT scrollEndMsgId = 19;
    constexpr UINT manualCaptureMsgId = 20;
    constexpr int comparisonH = 100;
    constexpr int maxDismissTime = 3;
    constexpr int autoScrollDelayMs = 120;
    constexpr int autoSettleFirstMs = 60;
    constexpr int autoSettlePollMs = 45;
    constexpr int maxAutoSettleChecks = 7;
    constexpr int settleRecheckMs = 180;
    constexpr int maxSettleRecheck = 2;
    constexpr int manualPollMs = 120;
    constexpr double bottomMatchMinRatio = 0.9;
    constexpr double bottomMatchMaxError = 40000;

    std::vector<BYTE> toGrayscale(const BYTE* bgra, int width, int height, int stride)
    {
        std::vector<BYTE> gray((size_t)width * height);
        for (int y = 0; y < height; y++) {
            const BYTE* src = bgra + (size_t)y * stride;
            BYTE* dst = gray.data() + (size_t)y * width;
            for (int x = 0; x < width; x++) {
                dst[x] = (BYTE)((src[x * 4] * 114 + src[x * 4 + 1] * 587 + src[x * 4 + 2] * 299) / 1000);
            }
        }
        return gray;
    }

    int findMostSimilarY(const BYTE* gray1, int gray1H, const BYTE* gray2, int gray2H, int width)
    {
        int searchH = gray1H - gray2H + 1;
        if (searchH <= 0) return 0;
        double minAvgError = DBL_MAX;
        int bestY = 0;
        for (int y = 0; y < searchH; y++) {
            double error = 0.0;
            for (int row = 0; row < gray2H; row++) {
                const BYTE* row1 = gray1 + (size_t)(y + row) * width;
                const BYTE* row2 = gray2 + (size_t)row * width;
                for (int x = 0; x < width; x++) {
                    int diff = (int)row1[x] - (int)row2[x];
                    error += diff * diff;
                }
            }
            double avgError = error / gray2H;
            if (avgError < minAvgError) {
                minAvgError = avgError;
                bestY = y;
            }
        }
        return bestY;
    }

    bool framesDiffer(const std::vector<BYTE>& a, const std::vector<BYTE>& b)
    {
        if (a.size() != b.size()) return true;
        return memcmp(a.data(), b.data(), a.size()) != 0;
    }

    bool bottomBandStable(const std::vector<BYTE>& a, const std::vector<BYTE>& b, int width, int height)
    {
        if (width <= 0 || height <= 0 || a.size() != b.size()) return false;
        size_t expected = (size_t)width * height * 4;
        if (a.size() < expected) return false;

        int bandH = std::min(height, std::max(60, height * 3 / 10));
        int startY = height - bandH;
        int xStep = std::max(1, width / 80);
        int yStep = std::max(1, bandH / 40);
        int same = 0;
        int total = 0;
        for (int y = startY; y < height; y += yStep) {
            for (int x = 0; x < width; x += xStep) {
                size_t i = ((size_t)y * width + x) * 4;
                int diff = abs((int)a[i] - (int)b[i])
                    + abs((int)a[i + 1] - (int)b[i + 1])
                    + abs((int)a[i + 2] - (int)b[i + 2]);
                if (diff <= 12) same++;
                total++;
            }
        }
        return total > 0 && (double)same / total >= 0.992;
    }

    std::wstring windowClassName(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd)) return L"<none>";
        wchar_t cls[256]{};
        int n = GetClassNameW(hwnd, cls, (int)std::size(cls));
        if (n <= 0) return L"<unknown>";
        return std::wstring(cls, (size_t)n);
    }

    int findScrollByBottomStrip(const BYTE* grayOld, const BYTE* grayNew, int width, int stripH)
    {
        double minAvgError = DBL_MAX;
        double avgAtZero = DBL_MAX;
        int bestS = 0;
        for (int s = 0; s < stripH; s++) {
            int rows = stripH - s;
            double error = 0.0;
            for (int r = 0; r < rows; r++) {
                const BYTE* row1 = grayOld + (size_t)(s + r) * width;
                const BYTE* row2 = grayNew + (size_t)r * width;
                for (int x = 0; x < width; x++) {
                    int diff = (int)row1[x] - (int)row2[x];
                    error += diff * diff;
                }
            }
            double avgError = error / rows;
            if (s == 0) avgAtZero = avgError;
            if (avgError < minAvgError) {
                minAvgError = avgError;
                bestS = s;
            }
        }
        if (bestS <= 0) return 0;
        if (minAvgError >= avgAtZero * bottomMatchMinRatio) return 0;
        if (minAvgError > bottomMatchMaxError) return 0;
        return bestS;
    }
}

CapLong::CapLong(WinCap* win) : win(win)
{
    auto d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());

    // Clicking "long screenshot" now starts immediately. The selected area becomes a hole so the
    // underlying app receives the user's wheel gestures; we poll the pixels and stitch only when
    // the user actually scrolls. Automatic scrolling remains an optional toolbar action.
    isCapturing = true;
    win->hollowWin();
    makeTool();
    firstStep();
}

CapLong::~CapLong()
{
}

void CapLong::dispose()
{
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->killTimer(manualCaptureMsgId);
    if (tool) tool->close();
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
    paintImgPreview(ctx);
    if (isFinish && layoutTextEnd) {
        auto borderRadius{ 4.f * win->dpi };
        ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
        ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void CapLong::setCursor()
{
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void CapLong::onMove(POINT)
{
}

void CapLong::onUp(POINT)
{
}

void CapLong::scheduleNextCapture(int delayMs)
{
    if (!isCapturing || isFinish || autoScroll) return;
    win->setTimer(delayMs, manualCaptureMsgId);
}

void CapLong::scheduleAutoScroll(int delayMs)
{
    if (!isCapturing || isFinish || !autoScroll) return;
    win->setTimer(delayMs, scrollMsgId);
}

void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == manualCaptureMsgId) {
        win->killTimer(manualCaptureMsgId);
        if (!isCapturing || isFinish || autoScroll) return;
        // The first timer tick happens after CapLong has been assigned into WinCap, so this also
        // makes the initial preview visible even though firstStep ran inside the constructor.
        win->refresh();
        capStep();
    }
    else if (timerId == scrollMsgId) {
        win->killTimer(scrollMsgId);
        if (!isCapturing || isFinish || !autoScroll) return;
        dispatchAutoScroll();
    }
    else if (timerId == scrollEndMsgId) {
        win->killTimer(scrollEndMsgId);
        if (!isCapturing || isFinish || !autoScroll) return;
        sampleAutoSettle();
    }
}

void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    imgW = int(maskRect.right - maskRect.left);
    imgH = int(maskRect.bottom - maskRect.top);
    resultH = imgH;
    capStartPos.x = (int)maskRect.left;
    capStartPos.y = (int)maskRect.top;
    ClientToScreen(win->hwnd, &capStartPos);
    imgData = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    img1 = imgData;
    makeImgPreview();
    scheduleNextCapture(manualPollMs);
}

void CapLong::makeImgPreview()
{
    imgPreview.Reset();
    if (imgW <= 0 || resultH <= 0 || imgData.empty()) return;
    float previewScaleW = tool ? (float)tool->w / (float)imgW : 1.0f;
    int previewW = (int)((float)imgW * previewScaleW);
    int previewH = (int)((float)resultH * previewScaleW);
    if (previewW <= 0 || previewH <= 0) return;

    std::vector<BYTE> scaledData((size_t)previewW * 4 * previewH);
    for (int y = 0; y < previewH; y++) {
        int srcY = (int)((float)y / previewScaleW);
        if (srcY >= resultH) srcY = resultH - 1;
        for (int x = 0; x < previewW; x++) {
            int srcX = (int)((float)x / previewScaleW);
            if (srcX >= imgW) srcX = imgW - 1;
            int srcIdx = (srcY * imgW + srcX) * 4;
            int dstIdx = (y * previewW + x) * 4;
            scaledData[dstIdx] = imgData[srcIdx];
            scaledData[dstIdx + 1] = imgData[srcIdx + 1];
            scaledData[dstIdx + 2] = imgData[srcIdx + 2];
            scaledData[dstIdx + 3] = imgData[srcIdx + 3];
        }
    }
    D2D1_BITMAP_PROPERTIES1 props = {
        .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
        .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(previewW, previewH), scaledData.data(), previewW * 4, props, imgPreview.GetAddressOf());
}

void CapLong::capStep()
{
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    if (data.empty()) {
        if (autoScroll) scheduleAutoScroll(autoScrollDelayMs);
        else scheduleNextCapture(manualPollMs);
        return;
    }
    processFrame(std::move(data));
}

void CapLong::processFrame(std::vector<BYTE> data)
{
    if (data.empty()) {
        if (autoScroll) scheduleAutoScroll(autoScrollDelayMs);
        else scheduleNextCapture(manualPollMs);
        return;
    }

    if (firstCheck) {
        changeStartY = -1;
        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                int idx = (y * imgW + x) * 4;
                if (img1[idx] != data[idx] || img1[idx + 1] != data[idx + 1] || img1[idx + 2] != data[idx + 2]) {
                    changeStartY = y;
                    break;
                }
            }
            if (changeStartY != -1) break;
        }
        if (changeStartY == -1) {
            if (autoScroll) handleAutoNoProgress();
            else scheduleNextCapture(manualPollMs);
            return;
        }
        firstCheck = false;
    }

    int rowPix{ imgW * 4 };
    int stripH = std::min(comparisonH, imgH - changeStartY);
    if (stripH <= 0) {
        if (autoScroll) handleAutoNoProgress();
        else scheduleNextCapture(manualPollMs);
        return;
    }

    int img1StripH = imgH - changeStartY;
    auto gray1 = toGrayscale(img1.data() + (size_t)changeStartY * rowPix, imgW, img1StripH, rowPix);
    auto gray2 = toGrayscale(data.data() + (size_t)changeStartY * rowPix, imgW, stripH, rowPix);
    int y = findMostSimilarY(gray1.data(), img1StripH, gray2.data(), stripH, imgW);
    if (y == 0) {
        auto gray1Bottom = toGrayscale(img1.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
        auto gray2Bottom = toGrayscale(data.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
        y = findScrollByBottomStrip(gray1Bottom.data(), gray2Bottom.data(), imgW, stripH);
    }

    if (y == 0) {
        // Automatic mode already waited for the newly exposed bottom band to settle before
        // reaching this matcher. Manual mode keeps the older recheck path for smooth wheel input.
        if (!autoScroll && framesDiffer(data, img1) && settleRecheckCount < maxSettleRecheck) {
            settleRecheckCount++;
            scheduleNextCapture(settleRecheckMs);
            return;
        }
        settleRecheckCount = 0;
        if (autoScroll) handleAutoNoProgress();
        else {
            // Manual mode never decides that the user is "done" merely because they paused.
            scheduleNextCapture(manualPollMs);
        }
        return;
    }

    settleRecheckCount = 0;
    int paintStart = resultH - (imgH - y - changeStartY);
    int newResultH = paintStart + (imgH - changeStartY);
    if (paintStart < 0 || newResultH <= resultH) {
        if (autoScroll) handleAutoNoProgress();
        else scheduleNextCapture(manualPollMs);
        return;
    }

    int addedH = newResultH - resultH;
    std::vector<BYTE> newResult((size_t)rowPix * newResultH);
    CopyMemory(newResult.data(), imgData.data(), imgData.size());
    for (int row = 0; row < imgH - changeStartY; row++) {
        CopyMemory(newResult.data() + (size_t)(paintStart + row) * rowPix,
            data.data() + (size_t)(changeStartY + row) * rowPix, rowPix);
    }
    imgData = std::move(newResult);
    img1 = std::move(data);
    resultH = newResultH;
    dismissTime = 0;

    if (autoScroll && !autoStrategyConfirmed) {
        autoStrategyConfirmed = true;
        StarCapDiag::append(std::format(L"[long-v2] strategy-confirmed={} added={} resultH={}",
            autoStrategyName(), addedH, resultH));
    }

    if (resultH > 36000) { stopCap(); return; }
    makeImgPreview();
    win->refresh();
    if (autoScroll) scheduleAutoScroll(autoScrollDelayMs);
    else scheduleNextCapture(manualPollMs);
}

void CapLong::resolveAutoTargets()
{
    HWND child = WindowFromPoint(autoTargetPoint);
    if (child && IsWindow(child)) targetHwnd = child;
    if (targetHwnd && IsWindow(targetHwnd)) {
        HWND root = GetAncestor(targetHwnd, GA_ROOT);
        targetRootHwnd = root && IsWindow(root) ? root : targetHwnd;
    }
    else {
        targetRootHwnd = nullptr;
    }
}

const wchar_t* CapLong::autoStrategyName() const
{
    switch (autoStrategy) {
    case AutoScrollStrategy::SendInput: return L"sendinput";
    case AutoScrollStrategy::ChildWheelMessage: return L"child-wheel";
    case AutoScrollStrategy::RootWheelMessage: return L"root-wheel";
    default: return L"unknown";
    }
}

void CapLong::dispatchAutoScroll()
{
    if (!isCapturing || isFinish || !autoScroll) return;
    resolveAutoTargets();

    if (!autoStrategyConfirmed) {
        StarCapDiag::append(std::format(L"[long-v2] probe strategy={} child={} root={}",
            autoStrategyName(), windowClassName(targetHwnd), windowClassName(targetRootHwnd)));
    }

    bool sent = false;
    if (autoStrategy == AutoScrollStrategy::SendInput) {
        SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA;
        sent = SendInput(1, &input, sizeof(INPUT)) == 1;
    }
    else {
        HWND target = autoStrategy == AutoScrollStrategy::ChildWheelMessage ? targetHwnd : targetRootHwnd;
        if (target && IsWindow(target)) {
            WPARAM wheelParam = static_cast<WPARAM>(static_cast<WORD>(-WHEEL_DELTA)) << 16;
            LPARAM pointParam = MAKELPARAM(static_cast<WORD>(autoTargetPoint.x), static_cast<WORD>(autoTargetPoint.y));
            sent = PostMessageW(target, WM_MOUSEWHEEL, wheelParam, pointParam) != FALSE;
        }
    }

    if (!sent && !autoStrategyConfirmed) {
        StarCapDiag::append(std::format(L"[long-v2] dispatch-failed strategy={}", autoStrategyName()));
    }

    autoSettleFrame.clear();
    autoSettleChecks = 0;
    win->setTimer(autoSettleFirstMs, scrollEndMsgId);
}

void CapLong::sampleAutoSettle()
{
    if (!isCapturing || isFinish || !autoScroll) return;
    auto frame = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    if (frame.empty()) {
        if (++autoSettleChecks >= maxAutoSettleChecks) handleAutoNoProgress();
        else win->setTimer(autoSettlePollMs, scrollEndMsgId);
        return;
    }

    if (autoSettleFrame.empty()) {
        autoSettleFrame = std::move(frame);
        autoSettleChecks = 1;
        win->setTimer(autoSettlePollMs, scrollEndMsgId);
        return;
    }

    bool stable = bottomBandStable(autoSettleFrame, frame, imgW, imgH);
    autoSettleChecks++;
    if (!stable && autoSettleChecks < maxAutoSettleChecks) {
        autoSettleFrame = std::move(frame);
        win->setTimer(autoSettlePollMs, scrollEndMsgId);
        return;
    }

    autoSettleFrame.clear();
    autoSettleChecks = 0;
    processFrame(std::move(frame));
}

bool CapLong::advanceAutoScrollStrategy()
{
    resolveAutoTargets();
    int next = (int)autoStrategy + 1;
    int last = (int)AutoScrollStrategy::RootWheelMessage;
    while (next <= last) {
        auto candidate = (AutoScrollStrategy)next;
        bool usable = true;
        if (candidate == AutoScrollStrategy::ChildWheelMessage) {
            usable = targetHwnd && IsWindow(targetHwnd);
        }
        else if (candidate == AutoScrollStrategy::RootWheelMessage) {
            usable = targetRootHwnd && IsWindow(targetRootHwnd) && targetRootHwnd != targetHwnd;
        }
        if (usable) {
            autoStrategy = candidate;
            firstCheck = true;
            changeStartY = -1;
            settleRecheckCount = 0;
            autoSettleChecks = 0;
            autoSettleFrame.clear();
            dismissTime = 0;
            StarCapDiag::append(std::format(L"[long-v2] switch-strategy={}", autoStrategyName()));
            return true;
        }
        next++;
    }
    return false;
}

void CapLong::fallbackToManual()
{
    if (!autoScroll) return;
    autoScroll = false;
    autoStrategyConfirmed = false;
    firstCheck = true;
    changeStartY = -1;
    dismissTime = 0;
    settleRecheckCount = 0;
    autoSettleChecks = 0;
    autoSettleFrame.clear();
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    StarCapDiag::append(L"[long-v2] auto-scroll unavailable; fallback=manual");
    scheduleNextCapture(80);
}

void CapLong::handleAutoNoProgress()
{
    if (!autoScroll || !isCapturing || isFinish) return;

    if (!autoStrategyConfirmed) {
        if (advanceAutoScrollStrategy()) {
            scheduleAutoScroll(90);
        }
        else {
            fallbackToManual();
        }
        return;
    }

    if (++dismissTime > maxDismissTime) {
        StarCapDiag::append(std::format(L"[long-v2] reached-bottom strategy={} resultH={}",
            autoStrategyName(), resultH));
        stopCap();
        return;
    }
    scheduleAutoScroll(autoScrollDelayMs);
}

void CapLong::startAutoScroll()
{
    if (!isCapturing || isFinish || autoScroll) return;
    autoScroll = true;
    autoStrategyConfirmed = false;
    autoStrategy = AutoScrollStrategy::SendInput;
    dismissTime = 0;
    settleRecheckCount = 0;
    autoSettleChecks = 0;
    autoSettleFrame.clear();
    firstCheck = true;
    changeStartY = -1;
    win->killTimer(manualCaptureMsgId);

    // Aim at the center of the selected live region. Phase 1 probes global input first, then
    // addressed WM_MOUSEWHEEL messages to the deepest child and finally the top-level root.
    auto& r = win->cutMask->maskRect;
    autoTargetPoint = { (LONG)((r.left + r.right) / 2.f), (LONG)((r.top + r.bottom) / 2.f) };
    ClientToScreen(win->hwnd, &autoTargetPoint);
    SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);
    resolveAutoTargets();
    StarCapDiag::append(std::format(L"[long-v2] auto-start point=({}, {}) child={} root={}",
        autoTargetPoint.x, autoTargetPoint.y, windowClassName(targetHwnd), windowClassName(targetRootHwnd)));
    scheduleAutoScroll(80);
}

void CapLong::makeTool()
{
    tool = std::make_unique<ToolLong>(win, this);
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

void CapLong::layoutTool()
{
    if (!tool) return;
    auto toolW{ tool->w };
    POINT pos{ 0,0 };
    auto& cutMask = win->cutMask;
    if (win->w - cutMask->maskRect.right - 2 * win->dpi < toolW) {
        pos.x = (LONG)(cutMask->maskRect.left - toolW - cutMask->strokeWidth - 2 * win->dpi);
    }
    else {
        pos.x = (LONG)(cutMask->maskRect.right + cutMask->strokeWidth + 2 * win->dpi);
    }
    pos.y = (LONG)(cutMask->maskRect.bottom - tool->h);
    ClientToScreen(win->hwnd, &pos);
    tool->setPosition(pos.x, pos.y);
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
    if (!imgPreview || !tool) return;
    auto bitmapSize = imgPreview->GetPixelSize();
    float drawW = (float)bitmapSize.width;
    float drawH = (float)bitmapSize.height;
    POINT pos{ tool->x, tool->y - (int)drawH - (int)(2 * win->dpi) };
    ScreenToClient(win->hwnd, &pos);
    D2D1_RECT_F destRect = D2D1::RectF((float)pos.x, (float)pos.y, pos.x + drawW, pos.y + drawH);
    ctx->DrawBitmap(imgPreview.Get(), destRect);
}

void CapLong::stopCap()
{
    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    autoStrategyConfirmed = false;
    autoSettleFrame.clear();
    makeStopText();
    win->restoreWin();
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->killTimer(manualCaptureMsgId);
    win->refresh();
}

void CapLong::makeStopText()
{
    if (resultH > 36000) {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);
    }
    else {
        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.reachedBottom"), 13 * win->dpi);
    }
    if (!layoutTextEnd) return;
    DWRITE_TEXT_METRICS tm{};
    layoutTextEnd->GetMetrics(&tm);
    auto& maskRect = win->cutMask->maskRect;
    auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
    auto halfW = tm.width / 2;
    float padding{ 8 * win->dpi };
    stopTextRect.left = halfX - halfW - padding;
    stopTextRect.top = maskRect.bottom - 30 * win->dpi - padding;
    stopTextRect.right = halfX + halfW + padding;
    stopTextRect.bottom = maskRect.bottom - padding;
    layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
    layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
    stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

void CapLong::copyToClipboard()
{
    if (imgData.empty()) return;
    if (!isFinish) stopCap();
    Util::saveToClipboard(imgW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (imgData.empty()) return false;
    if (!isFinish) stopCap();
    auto path = Util::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    return Util::saveToFile(path, imgW, resultH, imgData.data());
}

void CapLong::pin()
{
    if (imgData.empty()) return;
    if (!isFinish) stopCap();
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(monitor, &mi);
    auto& workArea = mi.rcWork;
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;
    int posX = workArea.left + (screenW - imgW) / 2;
    int posY = workArea.top + (screenH - std::min(resultH, screenH)) / 2;
    WinPin::initFromData(posX, posY, imgW, resultH, imgData);
}