from pathlib import Path

root = Path(__file__).resolve().parents[1]
h = root / 'Src/Win/CutMask.h'
c = root / 'Src/Win/CutMask.cpp'

ht = h.read_text(encoding='utf-8')
ct = c.read_text(encoding='utf-8')

old = '\tD2D1_RECT_F detectRegionAt(POINT pos, HWND* matchedWindow = nullptr);'
new = '\tD2D1_RECT_F detectRegionAt(POINT pos, HWND* matchedWindow = nullptr, bool precise = false);'
if old in ht:
    ht = ht.replace(old, new, 1)
elif new not in ht:
    raise SystemExit('CutMask.h detectRegionAt declaration changed')

anchor = '\tD2D1_RECT_F detectNativeChildRect(HWND hwnd, POINT localPos, const D2D1_RECT_F& fallback) const;\n'
insert = anchor + '\tvoid schedulePreciseHover(POINT pos, HWND hwnd);\n\tvoid cancelPreciseHover();\n\tvoid onHoverTimer(UINT id);\n\tbool autoPreciseEligible(HWND hwnd) const;\n'
if 'void schedulePreciseHover(POINT pos, HWND hwnd);' not in ht:
    if anchor not in ht:
        raise SystemExit('CutMask.h native child declaration anchor changed')
    ht = ht.replace(anchor, insert, 1)

anchor = '\twinrt::event_token onMouseUpToken{};\n'
insert = anchor + '\twinrt::event_token onTimerToken{};\n'
if 'onTimerToken' not in ht:
    if anchor not in ht:
        raise SystemExit('CutMask.h token anchor changed')
    ht = ht.replace(anchor, insert, 1)

ht = ht.replace('\tbool detectUiElements{ false };', '\tbool detectUiElements{ true };')

anchor = '\tbool imeDisabled{ false };\n'
insert = anchor + '\tPOINT preciseHoverPos{ INT_MAX, INT_MAX };\n\tHWND preciseHoverHwnd{ nullptr };\n\tstatic constexpr UINT preciseHoverTimerId{ 0x5A31 };\n\tstatic constexpr UINT preciseHoverDelayMs{ 130 };\n'
if 'preciseHoverTimerId' not in ht:
    if anchor not in ht:
        raise SystemExit('CutMask.h hover state anchor changed')
    ht = ht.replace(anchor, insert, 1)

anchor = '''\tonMouseUpToken = win->onMouseUp.add([this](POINT, bool isRight) {\n\t\tif (isRight) return;\n\t\tauto* cap = static_cast<WinCap*>(this->win);\n\t\tif (!cap) return;\n\t\tif (cap->stage == WinCap::CapStage::Adjust && hasRect()) historyCursor = regionHistory.size();\n\t});\n'''
insert = anchor + '\tonTimerToken = win->onTimer.add([this](UINT id) { onHoverTimer(id); });\n'
if 'onTimerToken = win->onTimer.add' not in ct:
    if anchor not in ct:
        raise SystemExit('CutMask.cpp constructor anchor changed')
    ct = ct.replace(anchor, insert, 1)

old = '''\tif (win) {\n\t\twin->onMouseMove.remove(onMouseMoveToken);\n\t\twin->onKeyDown.remove(onKeyDownToken);\n\t\twin->onMouseUp.remove(onMouseUpToken);\n\t}\n'''
new = '''\tif (win) {\n\t\tcancelPreciseHover();\n\t\twin->onMouseMove.remove(onMouseMoveToken);\n\t\twin->onKeyDown.remove(onKeyDownToken);\n\t\twin->onMouseUp.remove(onMouseUpToken);\n\t\twin->onTimer.remove(onTimerToken);\n\t}\n'''
if 'win->onTimer.remove(onTimerToken);' not in ct:
    if old not in ct:
        raise SystemExit('CutMask.cpp destructor anchor changed')
    ct = ct.replace(old, new, 1)

old = 'D2D1_RECT_F CutMask::detectRegionAt(POINT pos, HWND* matchedWindow)\n'
new = 'D2D1_RECT_F CutMask::detectRegionAt(POINT pos, HWND* matchedWindow, bool precise)\n'
if old in ct:
    ct = ct.replace(old, new, 1)
elif new not in ct:
    raise SystemExit('CutMask.cpp detectRegionAt signature changed')

old = '\t\treturn detectUiElements ? detectUiElementRect(item.hwnd, pos, item.rect) : item.rect;\n'
new = '\t\treturn (precise && detectUiElements) ? detectUiElementRect(item.hwnd, pos, item.rect) : item.rect;\n'
if old in ct:
    ct = ct.replace(old, new, 1)
elif new not in ct:
    raise SystemExit('CutMask.cpp detectRegionAt return changed')

marker = 'void CutMask::applyDetectedRect(const D2D1_RECT_F& rect, bool refreshWindow)\n'
helpers = r'''bool CutMask::autoPreciseEligible(HWND hwnd) const
{
	if (!hwnd) return false;
	wchar_t cls[96]{};
	GetClassNameW(hwnd, cls, (int)std::size(cls));
	// Browser accessibility trees are useful and fast enough when queried only
	// after hover settles. Qt apps (Telegram in particular) can block heavily in
	// UIA/MSAA, so they stay on instant window snapping unless Tab explicitly
	// requests element-level detection.
	if (wcsncmp(cls, L"Qt", 2) == 0) return false;
	if (wcsstr(cls, L"Chrome_WidgetWin") != nullptr) return true;
	if (wcscmp(cls, L"MozillaWindowClass") == 0) return true;
	if (wcscmp(cls, L"ApplicationFrameWindow") == 0) return true;
	return false;
}

void CutMask::cancelPreciseHover()
{
	if (win && win->hwnd) KillTimer(win->hwnd, preciseHoverTimerId);
	preciseHoverHwnd = nullptr;
	preciseHoverPos = { INT_MAX, INT_MAX };
}

void CutMask::schedulePreciseHover(POINT pos, HWND hwnd)
{
	cancelPreciseHover();
	if (!detectUiElements || !win || !win->hwnd || !autoPreciseEligible(hwnd)) return;
	preciseHoverPos = pos;
	preciseHoverHwnd = hwnd;
	SetTimer(win->hwnd, preciseHoverTimerId, preciseHoverDelayMs, nullptr);
}

void CutMask::onHoverTimer(UINT id)
{
	if (id != preciseHoverTimerId || !win || !win->hwnd) return;
	KillTimer(win->hwnd, preciseHoverTimerId);
	HWND expectedHwnd = preciseHoverHwnd;
	POINT expectedPos = preciseHoverPos;
	preciseHoverHwnd = nullptr;
	preciseHoverPos = { INT_MAX, INT_MAX };

	auto* cap = static_cast<WinCap*>(win);
	if (!cap || cap->stage != WinCap::CapStage::Select || cap->isPress || !detectUiElements || !expectedHwnd) return;
	POINT live{};
	GetCursorPos(&live);
	ScreenToClient(win->hwnd, &live);
	if (std::abs(live.x - expectedPos.x) > 2 || std::abs(live.y - expectedPos.y) > 2) return;

	HWND matched{};
	auto rect = detectRegionAt(live, &matched, true);
	if (matched != expectedHwnd || !validRect(rect)) return;
	applyDetectedRect(rect, true);
}

'''
if 'bool CutMask::autoPreciseEligible(HWND hwnd) const' not in ct:
    if marker not in ct:
        raise SystemExit('CutMask.cpp applyDetectedRect marker changed')
    ct = ct.replace(marker, helpers + marker, 1)

old = '''bool CutMask::highlight(POINT pos)\n{\n\tcursorPos = pos;\n\tHWND matched{};\n\tauto rect = detectRegionAt(pos, &matched);\n\tif (!validRect(rect) || sameRectRounded(rect, maskRect)) return false;\n\tapplyDetectedRect(rect, true);\n\treturn true;\n}\n'''
new = '''bool CutMask::highlight(POINT pos)\n{\n\tcursorPos = pos;\n\tHWND matched{};\n\tauto rect = detectRegionAt(pos, &matched, false);\n\tschedulePreciseHover(pos, matched);\n\tif (!validRect(rect) || sameRectRounded(rect, maskRect)) return false;\n\tapplyDetectedRect(rect, true);\n\treturn true;\n}\n'''
if 'schedulePreciseHover(pos, matched);' not in ct:
    if old not in ct:
        raise SystemExit('CutMask.cpp highlight block changed')
    ct = ct.replace(old, new, 1)

old = '''\tif (!ctrl && !alt && cap->stage == WinCap::CapStage::Select && !cap->isPress && key == VK_TAB) {\n\t\tdetectUiElements = !detectUiElements;\n\t\tPOINT p{};\n\t\tGetCursorPos(&p);\n\t\tScreenToClient(win->hwnd, &p);\n\t\tapplyDetectedRect(detectRegionAt(p), true);\n\t\treturn;\n\t}\n'''
new = '''\tif (!ctrl && !alt && cap->stage == WinCap::CapStage::Select && !cap->isPress && key == VK_TAB) {\n\t\tdetectUiElements = !detectUiElements;\n\t\tcancelPreciseHover();\n\t\tPOINT p{};\n\t\tGetCursorPos(&p);\n\t\tScreenToClient(win->hwnd, &p);\n\t\t// Tab is explicit user intent: when element mode is enabled, run the full\n\t\t// UIA/MSAA path immediately even for applications excluded from auto-hover.\n\t\tapplyDetectedRect(detectRegionAt(p, nullptr, detectUiElements), true);\n\t\treturn;\n\t}\n'''
if 'detectRegionAt(p, nullptr, detectUiElements)' not in ct:
    if old not in ct:
        raise SystemExit('CutMask.cpp Tab block changed')
    ct = ct.replace(old, new, 1)

h.write_text(ht, encoding='utf-8')
c.write_text(ct, encoding='utf-8')
print('patched capture hover v5')
