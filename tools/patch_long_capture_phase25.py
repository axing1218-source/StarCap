from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f'{label} marker not found')
    return text.replace(old, new, 1)

# Public toggle API used by both the toolbar and keyboard control layer.
h_path = Path('Src/Win/CapLong.h')
h = h_path.read_text(encoding='utf-8-sig')
h = replace_once(
    h,
    '\tvoid startAutoScroll();\n',
    '\tvoid startAutoScroll();\n\tvoid toggleAutoScroll();\n',
    'CapLong toggle declaration')
h_path.write_text(h, encoding='utf-8-sig')

# Make the existing Auto toolbar button a real start/pause toggle.
tool_path = Path('Src/Tool/ToolLong.cpp')
tool = tool_path.read_text(encoding='utf-8-sig')
tool = replace_once(
    tool,
    '''\tif (btn->id == L"auto") {\n\t\tif (capLong) capLong->startAutoScroll();\n\t\treturn;\n\t}\n''',
    '''\tif (btn->id == L"auto") {\n\t\tif (capLong) capLong->toggleAutoScroll();\n\t\treturn;\n\t}\n''',
    'ToolLong auto toggle')
tool_path.write_text(tool, encoding='utf-8-sig')

cpp_path = Path('Src/Win/CapLong.cpp')
cpp = cpp_path.read_text(encoding='utf-8-sig')

# A short-lived low-level keyboard hook is installed only while Long Capture exists. It is needed
# because hollowing the selected region intentionally returns keyboard focus to the app underneath.
const_marker = '''    constexpr UINT manualCaptureMsgId = 20;\n'''
control_code = r'''    constexpr UINT longControlMsgId = 21;
    constexpr int longControlPollMs = 30;

    HHOOK longControlHook{};
    volatile LONG longPendingEsc{};
    volatile LONG longPendingSpace{};
    volatile LONG longPendingEnter{};
    volatile LONG longEscHeld{};
    volatile LONG longSpaceHeld{};
    volatile LONG longEnterHeld{};

    LRESULT CALLBACK longControlKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && lParam) {
            const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            volatile LONG* pending = nullptr;
            volatile LONG* held = nullptr;
            if (info->vkCode == VK_ESCAPE) {
                pending = &longPendingEsc; held = &longEscHeld;
            }
            else if (info->vkCode == VK_SPACE) {
                pending = &longPendingSpace; held = &longSpaceHeld;
            }
            else if (info->vkCode == VK_RETURN) {
                pending = &longPendingEnter; held = &longEnterHeld;
            }
            if (pending && held) {
                if (down && InterlockedExchange(held, 1) == 0) {
                    InterlockedExchange(pending, 1);
                }
                else if (up) {
                    InterlockedExchange(held, 0);
                }
                // Do not leak these Long Capture control keys into WhatsApp/the browser beneath
                // the hollow capture surface.
                return 1;
            }
        }
        return CallNextHookEx(longControlHook, code, wParam, lParam);
    }
'''
cpp = replace_once(cpp, const_marker, const_marker + control_code, 'control constants')

ctor_marker = '''    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());\n\n'''
ctor_code = r'''    InterlockedExchange(&longPendingEsc, 0);
    InterlockedExchange(&longPendingSpace, 0);
    InterlockedExchange(&longPendingEnter, 0);
    InterlockedExchange(&longEscHeld, 0);
    InterlockedExchange(&longSpaceHeld, 0);
    InterlockedExchange(&longEnterHeld, 0);
    if (!longControlHook) {
        longControlHook = SetWindowsHookExW(WH_KEYBOARD_LL, longControlKeyboardProc,
            GetModuleHandleW(nullptr), 0);
        if (!longControlHook) {
            StarCapDiag::append(std::format(L"[long-v2] control-hook-failed error={}", GetLastError()));
        }
    }
    win->setTimer(longControlPollMs, longControlMsgId);

'''
cpp = replace_once(cpp, ctor_marker, ctor_marker + ctor_code, 'constructor control hook')

# Dispose the control timer/hook with the long-capture session.
dispose_old = r'''void CapLong::dispose()
{
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->killTimer(manualCaptureMsgId);
    if (tool) tool->close();
}
'''
dispose_new = r'''void CapLong::dispose()
{
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    win->killTimer(manualCaptureMsgId);
    win->killTimer(longControlMsgId);
    if (longControlHook) {
        UnhookWindowsHookEx(longControlHook);
        longControlHook = nullptr;
    }
    if (tool) tool->close();
}
'''
cpp = replace_once(cpp, dispose_old, dispose_new, 'dispose hook cleanup')

# Handle Esc / Space / Enter even though the underlying app owns focus. Enter retains StarCap's
# existing meaning: copy the current capture to clipboard and close.
timer_old = r'''void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == manualCaptureMsgId) {
'''
timer_new = r'''void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == longControlMsgId) {
        win->killTimer(longControlMsgId);
        if (InterlockedExchange(&longPendingEsc, 0)) {
            StarCapDiag::append(L"[long-v2] control=escape");
            win->close();
            return;
        }
        if (InterlockedExchange(&longPendingEnter, 0)) {
            if (hasImage()) {
                StarCapDiag::append(L"[long-v2] control=enter-copy");
                win->longCopyToClipboard();
                win->close();
                return;
            }
        }
        if (InterlockedExchange(&longPendingSpace, 0)) {
            toggleAutoScroll();
        }
        win->setTimer(longControlPollMs, longControlMsgId);
        return;
    }
    if (timerId == manualCaptureMsgId) {
'''
cpp = replace_once(cpp, timer_old, timer_new, 'control timer handler')

# The Auto button and Space now share the same pause/resume behavior. Pausing leaves manual capture
# polling active, so the user can still wheel by hand while automatic scrolling is suspended.
start_marker = r'''void CapLong::startAutoScroll()
{
'''
toggle_code = r'''void CapLong::toggleAutoScroll()
{
    if (!isCapturing || isFinish) return;
    if (!autoScroll) {
        StarCapDiag::append(L"[long-v2] auto-control=resume");
        startAutoScroll();
        return;
    }

    autoScroll = false;
    autoStrategyConfirmed = false;
    dismissTime = 0;
    settleRecheckCount = 0;
    autoSettleChecks = 0;
    autoSettleFrame.clear();
    firstCheck = true;
    changeStartY = -1;
    win->killTimer(scrollMsgId);
    win->killTimer(scrollEndMsgId);
    StarCapDiag::append(L"[long-v2] auto-control=pause");
    scheduleNextCapture(80);
}

'''
cpp = replace_once(cpp, start_marker, toggle_code + start_marker, 'toggle implementation')

# Never permanently steal the cursor. SendInput still needs the pointer over the scroll target, so
# move it there only for the actual wheel injection and restore the user's current pointer position.
send_old = r'''    if (autoStrategy == AutoScrollStrategy::SendInput) {
        SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA;
        sent = SendInput(1, &input, sizeof(INPUT)) == 1;
    }
'''
send_new = r'''    if (autoStrategy == AutoScrollStrategy::SendInput) {
        POINT restorePos{};
        GetCursorPos(&restorePos);
        const bool movedCursor = restorePos.x != autoTargetPoint.x || restorePos.y != autoTargetPoint.y;
        if (movedCursor) SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA;
        sent = SendInput(1, &input, sizeof(INPUT)) == 1;
        if (movedCursor) SetCursorPos(restorePos.x, restorePos.y);
    }
'''
cpp = replace_once(cpp, send_old, send_new, 'cursor restore sendinput')

# startAutoScroll no longer parks the pointer at the center; dispatchAutoScroll handles temporary
# positioning only when the SendInput strategy is actually in use.
cpp = replace_once(
    cpp,
    '''    SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);\n    resolveAutoTargets();\n''',
    '''    resolveAutoTargets();\n''',
    'remove persistent auto cursor move')

# Keep the toolbar inside the virtual desktop when a selection reaches one or both screen edges.
layout_old = r'''void CapLong::layoutTool()
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
'''
layout_new = r'''void CapLong::layoutTool()
{
    if (!tool) return;
    const LONG toolW = (LONG)tool->w;
    const LONG toolH = (LONG)tool->h;
    const LONG clientW = (LONG)win->w;
    const LONG clientH = (LONG)win->h;
    const LONG margin = std::max<LONG>(4, (LONG)(8.f * win->dpi));
    auto& cutMask = win->cutMask;
    const auto& r = cutMask->maskRect;
    const LONG gap = std::max<LONG>(2, (LONG)(cutMask->strokeWidth + 2.f * win->dpi));
    const bool roomRight = clientW - (LONG)r.right - gap >= toolW;
    const bool roomLeft = (LONG)r.left - gap >= toolW;

    POINT pos{ 0,0 };
    if (roomRight) {
        pos.x = (LONG)r.right + gap;
    }
    else if (roomLeft) {
        pos.x = (LONG)r.left - toolW - gap;
    }
    else {
        // Near/full-screen selection: place the control strip inside the capture surface instead
        // of calculating a negative/off-desktop X position.
        LONG maxX = std::max<LONG>(margin, clientW - toolW - margin);
        pos.x = std::clamp<LONG>((LONG)r.right - toolW - margin, margin, maxX);
    }

    LONG maxY = std::max<LONG>(margin, clientH - toolH - margin);
    pos.y = std::clamp<LONG>((LONG)r.bottom - toolH, margin, maxY);
    ClientToScreen(win->hwnd, &pos);
    tool->setPosition(pos.x, pos.y);
}
'''
cpp = replace_once(cpp, layout_old, layout_new, 'clamped long toolbar layout')

cpp_path.write_text(cpp, encoding='utf-8-sig')
print('Long Capture V2 Phase 2.5 controls patch applied.')
