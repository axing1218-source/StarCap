from pathlib import Path

root = Path(__file__).resolve().parents[1]
cap_h_path = root / 'Src/Win/CapLong.h'
cap_cpp_path = root / 'Src/Win/CapLong.cpp'
win_h_path = root / 'Src/Win/WinCap.h'
win_cpp_path = root / 'Src/Win/WinCap.cpp'
cut_cpp_path = root / 'Src/Win/CutMask.cpp'

cap_h = cap_h_path.read_text(encoding='utf-8')
cap_cpp = cap_cpp_path.read_text(encoding='utf-8')
win_h = win_h_path.read_text(encoding='utf-8')
win_cpp = win_cpp_path.read_text(encoding='utf-8')
cut_cpp = cut_cpp_path.read_text(encoding='utf-8')

# CapLong public interaction surface.
old = '''    void dispose();\n    void onMove(POINT pos);\n    void onUp(POINT pos);\n'''
new = '''    void dispose();\n    void onDown(POINT pos);\n    void onMove(POINT pos);\n    void onUp(POINT pos);\n'''
if old not in cap_h:
    raise SystemExit('CapLong public input anchor changed')
cap_h = cap_h.replace(old, new, 1)

old = '''    bool isAutoScrolling() const { return autoScroll; }\n    void hotkeyEnter();\n    void hotkeyEscape();\n'''
new = '''    bool isAutoScrolling() const { return autoScroll; }\n    bool canResizeSelection() const { return isCapturing && !isFinish && !autoScroll && acceptedFrames == 0; }\n    void hotkeyEnter();\n    void hotkeyEscape();\n'''
if old not in cap_h:
    raise SystemExit('CapLong resize capability anchor changed')
cap_h = cap_h.replace(old, new, 1)

old = '''    void firstStep();\n    void makeTool();\n'''
new = '''    void firstStep();\n    void restartForCurrentRect();\n    void makeTool();\n'''
if old not in cap_h:
    raise SystemExit('CapLong restart anchor changed')
cap_h = cap_h.replace(old, new, 1)

old = '''    bool storageLimitReached{ false };\n\n    CaptureState state{ CaptureState::Ready };\n'''
new = '''    bool storageLimitReached{ false };\n    bool resizingSelection{ false };\n\n    CaptureState state{ CaptureState::Ready };\n'''
if old not in cap_h:
    raise SystemExit('CapLong resize state anchor changed')
cap_h = cap_h.replace(old, new, 1)

# WinCap grants CapLong access to the capture/press state used by CutMask resizing.
old = '''class WinCap:public Ling::WinBase\n{\n\tfriend class CutMask;\n'''
new = '''class WinCap:public Ling::WinBase\n{\n\tfriend class CutMask;\n\tfriend class CapLong;\n'''
if old not in win_h:
    raise SystemExit('WinCap friend anchor changed')
win_h = win_h.replace(old, new, 1)

# Route mouse down to CapLong while in long-capture stage.
old = '''    else if (stage == CapStage::Adjust) {\n        // During a drag keep the toolbar stationary/off-screen instead of moving a\n        // separate topmost HWND on every WM_MOUSEMOVE. Reposition once on mouse-up.\n        isPress = true;\n        cutMask->startAdjust(pos);\n        if (toolCap) toolCap->hide();\n    }\n}\n'''
new = '''    else if (stage == CapStage::Adjust) {\n        // During a drag keep the toolbar stationary/off-screen instead of moving a\n        // separate topmost HWND on every WM_MOUSEMOVE. Reposition once on mouse-up.\n        isPress = true;\n        cutMask->startAdjust(pos);\n        if (toolCap) toolCap->hide();\n    }\n    else if (stage == CapStage::Long && capLong) {\n        capLong->onDown(pos);\n    }\n}\n'''
if old not in win_cpp:
    raise SystemExit('WinCap long mouse-down anchor changed')
win_cpp = win_cpp.replace(old, new, 1)

# Long-capture pre-scroll resize interaction.
old = '''void CapLong::setCursor()\n{\n    SetCursor(LoadCursor(nullptr, IDC_ARROW));\n}\n\nvoid CapLong::onMove(POINT)\n{\n}\n\nvoid CapLong::onUp(POINT)\n{\n}\n'''
new = r'''void CapLong::setCursor()
{
    if (canResizeSelection()) {
        POINT pos{};
        GetCursorPos(&pos);
        ScreenToClient(win->hwnd, &pos);
        switch (win->cutMask->hitTest(pos))
        {
        case MaskHit::TopLeft:
        case MaskHit::BottomRight:
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            return;
        case MaskHit::TopRight:
        case MaskHit::BottomLeft:
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
            return;
        case MaskHit::Top:
        case MaskHit::Bottom:
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return;
        case MaskHit::Left:
        case MaskHit::Right:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return;
        default:
            break;
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void CapLong::onDown(POINT pos)
{
    if (!canResizeSelection()) return;
    auto hit = win->cutMask->hitTest(pos);
    if (hit == MaskHit::None || hit == MaskHit::Inside) return;

    resizingSelection = true;
    win->isPress = true;
    win->killTimer(frameCaptureTimerId);
    win->killTimer(autoScrollTimerId);
    win->restoreWin();
    SetCapture(win->hwnd);
    win->cutMask->startAdjust(pos);
    if (tool) tool->hide();
    StarCapDiag::append(std::format(L"[long-next] resize-begin hit={}", static_cast<int>(hit)));
    win->refresh();
}

void CapLong::onMove(POINT pos)
{
    if (!resizingSelection) return;
    win->cutMask->adjust(pos);
}

void CapLong::onUp(POINT pos)
{
    if (!resizingSelection) return;
    win->cutMask->adjust(pos);
    resizingSelection = false;
    win->isPress = false;
    if (GetCapture() == win->hwnd) ReleaseCapture();

    // Re-open the interior to the target application before re-capturing the new first frame.
    win->hollowWin();
    restartForCurrentRect();
    if (tool) {
        layoutTool();
        tool->show();
    }
    StarCapDiag::append(std::format(L"[long-next] resize-end rect={}x{}", imgW, imgH));
    win->refresh();
}
'''
if old not in cap_cpp:
    raise SystemExit('CapLong input implementation anchor changed')
cap_cpp = cap_cpp.replace(old, new, 1)

# Release mouse capture if a session is disposed mid-resize.
old = '''void CapLong::dispose()\n{\n    win->killTimer(autoScrollTimerId);\n    win->killTimer(frameCaptureTimerId);\n'''
new = '''void CapLong::dispose()\n{\n    if (resizingSelection && GetCapture() == win->hwnd) ReleaseCapture();\n    resizingSelection = false;\n    win->isPress = false;\n    win->killTimer(autoScrollTimerId);\n    win->killTimer(frameCaptureTimerId);\n'''
if old not in cap_cpp:
    raise SystemExit('CapLong dispose anchor changed')
cap_cpp = cap_cpp.replace(old, new, 1)

# Reset only the long-capture session state after a pre-scroll resize; matcher/output logic is untouched.
anchor = '''void CapLong::captureFrame()\n{\n'''
restart = r'''void CapLong::restartForCurrentRect()
{
    win->killTimer(autoScrollTimerId);
    win->killTimer(frameCaptureTimerId);
    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
    releaseUiaScroll();

    slicesInitialized = false;
    materializedDirty = false;
    storageLimitReached = false;
    noProgressFrames = 0;
    rejectedFrames = 0;
    acceptedFrames = 0;
    scrollSequence = 0;
    staticTop = 0;
    staticBottom = 0;
    bodyHeight = 0;
    resultH = 0;
    imgW = 0;
    imgH = 0;

    frameRing.clear();
    committedFrame.clear();
    imgData.clear();
    headerData.clear();
    footerData.clear();
    bodyChunks.clear();
    imgPreview.Reset();
    layoutTextEnd.Reset();
    stopTextRect = {};
    stopTextPos = {};
    state = CaptureState::Ready;

    firstStep();
}

'''
if anchor not in cap_cpp:
    raise SystemExit('CapLong restart insertion anchor changed')
cap_cpp = cap_cpp.replace(anchor, restart + anchor, 1)

# Show resize handles in Long only while the selection is still editable.
if '#include "CapLong.h"' not in cut_cpp:
    cut_cpp = cut_cpp.replace('#include "WinCap.h"\n', '#include "WinCap.h"\n#include "CapLong.h"\n', 1)
old = '''void CutMask::paintHandles(ID2D1DeviceContext* ctx)\n{\n\tif (!ctx || !hasRect() || hideLabel || !brushHandle) return;\n\tauto* cap = static_cast<WinCap*>(win);\n'''
new = '''void CutMask::paintHandles(ID2D1DeviceContext* ctx)\n{\n\tif (!ctx || !hasRect() || !brushHandle) return;\n\tauto* cap = static_cast<WinCap*>(win);\n\tconst bool longResize = cap && cap->stage == WinCap::CapStage::Long && cap->capLong && cap->capLong->canResizeSelection();\n\tif (hideLabel && !longResize) return;\n'''
if old not in cut_cpp:
    raise SystemExit('CutMask paintHandles anchor changed')
cut_cpp = cut_cpp.replace(old, new, 1)

cap_h_path.write_text(cap_h, encoding='utf-8')
cap_cpp_path.write_text(cap_cpp, encoding='utf-8')
win_h_path.write_text(win_h, encoding='utf-8')
win_cpp_path.write_text(win_cpp, encoding='utf-8')
cut_cpp_path.write_text(cut_cpp, encoding='utf-8')
