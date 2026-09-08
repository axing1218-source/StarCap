from pathlib import Path

root = Path(__file__).resolve().parents[1]
cap_path = root / 'Src/Win/CapLong.cpp'
mask_path = root / 'Src/Win/CutMask.cpp'
win_path = root / 'Src/Win/WinCap.cpp'

cap = cap_path.read_text(encoding='utf-8')
mask = mask_path.read_text(encoding='utf-8')
win = win_path.read_text(encoding='utf-8')

# Long capture: background can influence MATCHING less, but output pixels are never rewritten.
# This intentionally leaves appendBodyRows() exactly as v6: accepted rows are copied verbatim.
start = cap.index('    double featureScore(')
end = cap.index('    LRESULT CALLBACK longCaptureKeyboardProc', start)
new_helpers = r'''    std::vector<float> makeMotionWeights(const std::vector<float>& oldFeatures,
        const std::vector<float>& newFeatures, int height)
    {
        const int dims = featureBins * 2;
        std::vector<float> weights(static_cast<size_t>(height) * featureBins, 0.0f);
        if (oldFeatures.size() != newFeatures.size() ||
            oldFeatures.size() < static_cast<size_t>(height) * dims) return weights;

        std::vector<float> raw(weights.size(), 0.0f);
        for (int y = 0; y < height; ++y) {
            for (int bin = 0; bin < featureBins; ++bin) {
                size_t base = static_cast<size_t>(y) * dims + bin * 2;
                double grayDelta = std::abs(static_cast<double>(oldFeatures[base]) - newFeatures[base]);
                double edgeDelta = std::abs(static_cast<double>(oldFeatures[base + 1]) - newFeatures[base + 1]);
                double signal = grayDelta * 0.75 + edgeDelta * 1.45;
                raw[static_cast<size_t>(y) * featureBins + bin] =
                    static_cast<float>(std::clamp((signal - 1.25) / 13.0, 0.0, 1.0));
            }
        }

        // Small dilation keeps text/bubble edges together. This mask is MATCH-ONLY:
        // it never changes pixels written into the resulting long screenshot.
        for (int y = 0; y < height; ++y) {
            for (int bin = 0; bin < featureBins; ++bin) {
                float best = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= height) continue;
                    for (int db = -1; db <= 1; ++db) {
                        int bb = bin + db;
                        if (bb < 0 || bb >= featureBins) continue;
                        best = std::max(best, raw[static_cast<size_t>(yy) * featureBins + bb]);
                    }
                }
                weights[static_cast<size_t>(y) * featureBins + bin] = best;
            }
        }
        return weights;
    }

    double motionCoverage(const std::vector<float>& weights, int height, int bodyTop, int bodyBottom)
    {
        if (weights.size() < static_cast<size_t>(height) * featureBins || bodyBottom <= bodyTop) return 0.0;
        size_t moving = 0;
        size_t total = 0;
        for (int y = bodyTop; y < bodyBottom; y += 3) {
            for (int bin = 0; bin < featureBins; ++bin) {
                if (weights[static_cast<size_t>(y) * featureBins + bin] >= 0.20f) ++moving;
                ++total;
            }
        }
        return total ? static_cast<double>(moving) / total : 0.0;
    }

    double featureScore(const std::vector<float>& oldFeatures, const std::vector<float>& newFeatures,
        const std::vector<float>& motionWeights, int height, int offset, int bodyTop, int bodyBottom)
    {
        const int dims = featureBins * 2;
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 1.0;
        const bool motionAware = motionWeights.size() >= static_cast<size_t>(height) * featureBins;
        double sum = 0.0;
        double weightSum = 0.0;
        for (int y = bodyTop; y < end; y += 3) {
            const float* a = oldFeatures.data() + static_cast<size_t>(y + offset) * dims;
            const float* b = newFeatures.data() + static_cast<size_t>(y) * dims;
            for (int bin = 0; bin < featureBins; ++bin) {
                double cellWeight = 1.0;
                if (motionAware) {
                    float w0 = motionWeights[static_cast<size_t>(y) * featureBins + bin];
                    float w1 = motionWeights[static_cast<size_t>(std::min(height - 1, y + offset)) * featureBins + bin];
                    // Retain 12% global context so sparse chats do not become underconstrained.
                    cellWeight = 0.12 + 0.88 * std::max(w0, w1);
                }
                for (int channel = 0; channel < 2; ++channel) {
                    int d = bin * 2 + channel;
                    double semanticWeight = channel ? 1.35 : 1.0;
                    double w = cellWeight * semanticWeight;
                    sum += std::abs(static_cast<double>(a[d]) - static_cast<double>(b[d])) * w;
                    weightSum += w;
                }
            }
        }
        if (weightSum <= 0.0) return 1.0;
        return sum / (weightSum * 255.0);
    }

    double pixelMadAtOffset(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        const std::vector<float>& motionWeights, int width, int height, int offset, int bodyTop, int bodyBottom)
    {
        int margin = std::max(8, width / 20);
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 255.0;
        const bool motionAware = motionWeights.size() >= static_cast<size_t>(height) * featureBins;
        double sum = 0.0;
        double weightSum = 0.0;
        int xStep = std::max(5, width / 100);
        for (int y = bodyTop; y < end; y += 7) {
            int oldY = y + offset;
            for (int x = margin; x < width - margin; x += xStep) {
                double w = 1.0;
                if (motionAware) {
                    int bin = std::clamp(x * featureBins / std::max(1, width), 0, featureBins - 1);
                    float w0 = motionWeights[static_cast<size_t>(y) * featureBins + bin];
                    float w1 = motionWeights[static_cast<size_t>(std::min(height - 1, oldY)) * featureBins + bin];
                    w = 0.12 + 0.88 * std::max(w0, w1);
                }
                size_t ia = (static_cast<size_t>(oldY) * width + x) * 4;
                size_t ib = (static_cast<size_t>(y) * width + x) * 4;
                sum += std::abs(static_cast<int>(oldFrame[ia]) - static_cast<int>(newFrame[ib])) * w;
                sum += std::abs(static_cast<int>(oldFrame[ia + 1]) - static_cast<int>(newFrame[ib + 1])) * w;
                sum += std::abs(static_cast<int>(oldFrame[ia + 2]) - static_cast<int>(newFrame[ib + 2])) * w;
                weightSum += 3.0 * w;
            }
        }
        return weightSum > 0.0 ? sum / weightSum : 255.0;
    }

'''
cap = cap[:start] + new_helpers + cap[end:]

# Sparse chat content can move while a large fixed wallpaper remains identical.
cap = cap.replace('    if (sameRatio >= 0.988) {', '    if (sameRatio >= 0.996) {', 1)

old = '''    auto oldFeatures = makeRowFeatures(oldFrame, imgW, imgH);\n    auto newFeatures = makeRowFeatures(newFrame, imgW, imgH);\n\n    struct Candidate {\n'''
new = '''    auto oldFeatures = makeRowFeatures(oldFrame, imgW, imgH);\n    auto newFeatures = makeRowFeatures(newFrame, imgW, imgH);\n    auto motionWeights = makeMotionWeights(oldFeatures, newFeatures, imgH);\n    double movingCoverage = motionCoverage(motionWeights, imgH, bodyTop, bodyBottom);\n    const bool motionAware = movingCoverage >= 0.040;\n    const std::vector<float> noMotionWeights;\n    const auto& scoreWeights = motionAware ? motionWeights : noMotionWeights;\n\n    struct Candidate {\n'''
if old not in cap:
    raise SystemExit('matchFrame feature anchor changed')
cap = cap.replace(old, new, 1)
cap = cap.replace(
    'double raw = featureScore(oldFeatures, newFeatures, imgH, offset, bodyTop, bodyBottom);',
    'double raw = featureScore(oldFeatures, newFeatures, scoreWeights, imgH, offset, bodyTop, bodyBottom);', 1)
cap = cap.replace(
    'result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, imgW, imgH, best.offset, bodyTop, bodyBottom);',
    'result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, scoreWeights, imgW, imgH, best.offset, bodyTop, bodyBottom);', 1)
old = '''        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n'''
new = '''        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} motion={:.4f}/{} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio, movingCoverage, motionAware ? 1 : 0,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n'''
if old not in cap:
    raise SystemExit('match log anchor changed')
cap = cap.replace(old, new, 1)

# CutMask: during an active resize, do not move/show a separate top-level magnifier popup.
# Draw the magnifier on the already double-buffered host canvas instead.
old = '''void CutMask::updateMagnifierPopup(POINT live)\n{\n    auto* cap = static_cast<WinCap*>(win);\n    if (!cap || hideLabel || cap->stage != WinCap::CapStage::Adjust ||\n        !hasRect() || !pointInRect(maskRect, live)) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
new = '''void CutMask::updateMagnifierPopup(POINT live)\n{\n    auto* cap = static_cast<WinCap*>(win);\n    if (!cap || hideLabel || cap->stage != WinCap::CapStage::Adjust || cap->isPress ||\n        !hasRect() || !pointInRect(maskRect, live)) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
if old not in mask:
    raise SystemExit('magnifier popup guard changed')
mask = mask.replace(old, new, 1)

old = '''    // Adjust stage uses its own topmost popup so the magnifier can cover ToolCap.\n    if (cap->stage == WinCap::CapStage::Adjust) {\n        updateMagnifierPopup(live);\n        return;\n    }\n    hideMagnifierPopup();\n\n    const float scale = win->dpi;\n'''
new = '''    // While idle in Adjust, keep the native popup so it can cover ToolCap.\n    // While actively resizing/moving a selection, keep all feedback on the host\n    // swap-chain canvas; moving two separate topmost windows on every mouse move\n    // caused visible flashing, especially when extending the bottom edge.\n    if (cap->stage == WinCap::CapStage::Adjust && !cap->isPress) {\n        updateMagnifierPopup(live);\n        return;\n    }\n    hideMagnifierPopup();\n\n    const float scale = win->dpi;\n'''
if old not in mask:
    raise SystemExit('paintMagnifier adjust anchor changed')
mask = mask.replace(old, new, 1)

# WinCap: Esc must close capture independent of keyboard focus. Only hook Esc and only
# while normal Select/Adjust capture is active; long capture already owns its control hook.
anchor = '''namespace\n{\n    constexpr float scaleNum{ 5.f }, srcW{ 50.f }, srcH{ 30.f };\n    constexpr float pixImgH{ scaleNum * srcH };\n    constexpr float pixW{ srcW * scaleNum };\n}\n'''
replacement = r'''namespace
{
    constexpr float scaleNum{ 5.f }, srcW{ 50.f }, srcH{ 30.f };
    constexpr float pixImgH{ scaleNum * srcH };
    constexpr float pixW{ srcW * scaleNum };

    HHOOK gCaptureEscapeHook = nullptr;
    HWND gCaptureEscapeWindow = nullptr;
    bool gCaptureEscapeDown = false;

    LRESULT CALLBACK captureEscapeProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && gCaptureEscapeWindow) {
            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (kb->vkCode == VK_ESCAPE) {
                if (down && !gCaptureEscapeDown) {
                    gCaptureEscapeDown = true;
                    if (IsWindow(gCaptureEscapeWindow))
                        PostMessageW(gCaptureEscapeWindow, WM_CLOSE, 0, 0);
                }
                if (up) gCaptureEscapeDown = false;
                return 1;
            }
        }
        return CallNextHookEx(gCaptureEscapeHook, code, wParam, lParam);
    }

    void installCaptureEscapeHook(HWND hwnd)
    {
        gCaptureEscapeWindow = hwnd;
        gCaptureEscapeDown = false;
        if (!gCaptureEscapeHook)
            gCaptureEscapeHook = SetWindowsHookExW(WH_KEYBOARD_LL, captureEscapeProc, GetModuleHandleW(nullptr), 0);
    }

    void uninstallCaptureEscapeHook()
    {
        if (gCaptureEscapeHook) {
            UnhookWindowsHookEx(gCaptureEscapeHook);
            gCaptureEscapeHook = nullptr;
        }
        gCaptureEscapeWindow = nullptr;
        gCaptureEscapeDown = false;
    }
}
'''
if anchor not in win:
    raise SystemExit('WinCap namespace anchor changed')
win = win.replace(anchor, replacement, 1)

old = '''    setPixPos(pos);\n    show();\n}\n'''
new = '''    setPixPos(pos);\n    show();\n    installCaptureEscapeHook(hwnd);\n}\n'''
if old not in win:
    raise SystemExit('onCreated anchor changed')
win = win.replace(old, new, 1)

old = '''    if (isClosed) return;\n    isClosed = true;\n    if (capVideo) capVideo->dispose();\n'''
new = '''    if (isClosed) return;\n    isClosed = true;\n    uninstallCaptureEscapeHook();\n    if (capVideo) capVideo->dispose();\n'''
if old not in win:
    raise SystemExit('onClosed anchor changed')
win = win.replace(old, new, 1)

old = '''    else if (stage == CapStage::Adjust) {\n        // 选区外面按下不是重新框选，而是按落点所在的那一块调对应的边或角\n        isPress = true;\n        cutMask->startAdjust(pos);\n        layoutTool(toolCap.get());\n    }\n'''
new = '''    else if (stage == CapStage::Adjust) {\n        // During a drag keep the toolbar stationary/off-screen instead of moving a\n        // separate topmost HWND on every WM_MOUSEMOVE. Reposition once on mouse-up.\n        isPress = true;\n        cutMask->startAdjust(pos);\n        if (toolCap) toolCap->hide();\n    }\n'''
if old not in win:
    raise SystemExit('onDown Adjust anchor changed')
win = win.replace(old, new, 1)

old = '''    else if (stage == CapStage::Adjust) {\n        if (!isPress) return;\n        cutMask->adjust(pos);\n        // 选区变了，工具条跟着走位\n        layoutTool(toolCap.get());\n    }\n'''
new = '''    else if (stage == CapStage::Adjust) {\n        if (!isPress) return;\n        cutMask->adjust(pos);\n    }\n'''
if old not in win:
    raise SystemExit('onMove Adjust anchor changed')
win = win.replace(old, new, 1)

old = '''    else if (stage == CapStage::Adjust) {\n        isPress = false;\n    }\n'''
new = '''    else if (stage == CapStage::Adjust) {\n        isPress = false;\n        if (toolCap) {\n            layoutTool(toolCap.get());\n            toolCap->show();\n        }\n        cutMask->syncMagnifier(pos);\n    }\n'''
if old not in win:
    raise SystemExit('onUp Adjust anchor changed')
win = win.replace(old, new, 1)

old = '''void WinCap::enterLiveStage()\n{\n    // 底图是拖框那一刻的静态截图，从这里开始不能再画它 ——\n'''
new = '''void WinCap::enterLiveStage()\n{\n    // Long capture has its own keyboard control hook; video must not globally steal Esc.\n    uninstallCaptureEscapeHook();\n    // 底图是拖框那一刻的静态截图，从这里开始不能再画它 ——\n'''
if old not in win:
    raise SystemExit('enterLiveStage anchor changed')
win = win.replace(old, new, 1)

cap_path.write_text(cap, encoding='utf-8')
mask_path.write_text(mask, encoding='utf-8')
win_path.write_text(win, encoding='utf-8')
print('Patched Long Capture Next v8')
