#include "pch.h"
#include "../Win/WinPin.h"
#include "../Win/PinTransform.h"
#include "../History.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolMain.h"
#include "ToolSub.h"

namespace {
	std::wstring queuedInitialTool;
	bool queuedEditorOpen{ false };

	bool toolDarkMode()
	{
		auto* setting = Setting::get();
		return setting && setting->getToolFlag(L"app", L"darkMode", false);
	}
	uint32_t toolBg() { return toolDarkMode() ? 0x25262AFF : 0xFFFFFFFF; }
	uint32_t toolBorder() { return toolDarkMode() ? 0x4A4C52FF : 0xA8A8A8FF; }
	uint32_t toolText() { return toolDarkMode() ? 0xE8EAEDFF : 0x000000FF; }
	uint32_t toolHover() { return toolDarkMode() ? 0x36383DFF : 0xF2F2F2FF; }
	uint32_t toolSelected() { return toolDarkMode() ? 0x30364EFF : 0xE6F4FFFF; }
	uint32_t toolSplitter() { return toolDarkMode() ? 0x4A4C52FF : 0xDDDDDDFF; }
}

ToolMain::ToolMain(WinPin* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	x = win->x;
	y = win->y + win->h + 5.f * win->dpi;
	refreshSize();
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onTimer.add([this](UINT id) {
		if (id == initialToolTimerId) {
			KillTimer(hwnd, initialToolTimerId);
			applyQueuedInitialTool();
			return;
		}
		if (id == pinHookTimerId) {
			KillTimer(hwnd, pinHookTimerId);
			installPinInteractions();
		}
	});
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTools();
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	SetTimer(hwnd, pinHookTimerId, 1, nullptr);
}

void ToolMain::refreshSize()
{
	float logicW{ 0.f };
	for (auto& id : btnIds) logicW += (id == L"|") ? spliterW : btnSize;
	setSize(logicW, btnSize);
}

ToolMain::~ToolMain()
{
	if (pinHooksInstalled && win) {
		win->onMouseDown.remove(pinMouseDownToken);
		win->onMouseUp.remove(pinMouseUpToken);
	}
}

void ToolMain::init() {}

void ToolMain::queueInitialTool(const std::wstring& id)
{
	queuedInitialTool = id;
}

void ToolMain::queueEditorOpen()
{
	queuedEditorOpen = true;
}

float ToolMain::getBtnCenterX()
{
	float result{ 0.f };
	size_t btnIndex{ 0 };
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (btnIds[i] == L"|") { result += dpi; continue; }
		if (curId == btnIds[i]) {
			result += btns[btnIndex]->w / 2.f;
			return result;
		}
		result += btns[btnIndex]->w;
		btnIndex++;
	}
	return result;
}

void ToolMain::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(toolBg());
	body->setBorder(1.f, toolBorder());
	body->setAlignItems(Ling::Align::Center);
	body->setFlexDirection(Ling::FlexDirection::Row);
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			auto spliter = body->makeChild<Ling::Node>();
			spliter->setSize(dpi, 18.f);
			spliter->setBg(toolSplitter());
		}
		else {
			auto btn = body->makeChild<Ling::Button>();
			btn->setId(id);
			btn->setText(btnCodes[i]);
			btn->setHeightPercent(100.f);
			btn->setFlexGrow(1.f);
			btn->setColor(toolText());
			btn->setHoverColor(toolText());
			btn->setHoverBg(toolHover());
			if (id == L"rotate" || id == L"mirror") {
				btn->setFontFamily(L"Segoe UI Symbol");
				btn->setFontSize(16.f);
			}
			else {
				btn->setFontFamily(L"icon");
				btn->setFontSize(13.f);
			}
			btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
			if (id == L"rotate") tip->bind(btn, L"顺时针旋转 90°");
			else if (id == L"mirror") tip->bind(btn, L"水平镜像");
			else tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			btns.push_back(btn);
		}
	}
	// Edit/mark and plain pin are intentionally different entry points:
	// edit opens the complete toolbar immediately, while a normal pin remains
	// image-only until the user asks for the toolbar from the context menu.
	pinToolbarVisible = queuedEditorOpen || !queuedInitialTool.empty();
	queuedEditorOpen = false;
	if (pinToolbarVisible) show(); else hide();
	if (!queuedInitialTool.empty()) SetTimer(hwnd, initialToolTimerId, 1, nullptr);
}

void ToolMain::installPinInteractions()
{
	if (pinHooksInstalled || !win) return;
	pinHooksInstalled = true;
	pinMouseDownToken = win->onMouseDown.add([this](POINT, bool isRight) {
		if (!isRight) return;
		showPinContextMenu();
	});
	pinMouseUpToken = win->onMouseUp.add([this](POINT, bool isRight) {
		if (isRight) return;
		applyPinToolbarVisibility();
	});
	applyPinToolbarVisibility();
}

void ToolMain::applyPinToolbarVisibility()
{
	if (!win || !win->toolSub) return;
	if (pinToolbarVisible) {
		win->layoutTools();
		show();
	}
	else {
		cancelSelect();
		hide();
		win->toolSub->hideTools();
	}
}

void ToolMain::showPinContextMenu()
{
	if (!win || !win->hwnd) return;
	HMENU menu = CreatePopupMenu();
	if (!menu) return;
	constexpr UINT ID_TOOLBAR = 6101;
	constexpr UINT ID_COPY = 6102;
	constexpr UINT ID_SAVE = 6103;
	constexpr UINT ID_CLOSE = 6104;
	AppendMenuW(menu, MF_STRING, ID_TOOLBAR, pinToolbarVisible ? L"隐藏编辑工具栏" : L"显示编辑工具栏");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, ID_COPY, L"复制图片");
	AppendMenuW(menu, MF_STRING, ID_SAVE, L"另存为...");
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, ID_CLOSE, L"关闭贴图");

	POINT pt{};
	GetCursorPos(&pt);
	SetForegroundWindow(win->hwnd);
	const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
		pt.x, pt.y, 0, win->hwnd, nullptr);
	DestroyMenu(menu);
	PostMessageW(win->hwnd, WM_NULL, 0, 0);

	if (cmd == ID_TOOLBAR) {
		pinToolbarVisible = !pinToolbarVisible;
		applyPinToolbarVisibility();
		return;
	}
	if (cmd == ID_COPY) { win->copyToClipboard(); return; }
	if (cmd == ID_SAVE) { win->saveToFile(); return; }
	if (cmd == ID_CLOSE) { win->close(); return; }
	applyPinToolbarVisibility();
}

void ToolMain::applyQueuedInitialTool()
{
	if (queuedInitialTool.empty()) return;
	const auto id = queuedInitialTool;
	queuedInitialTool.clear();
	pinToolbarVisible = true;
	for (auto* btn : btns) {
		if (btn && btn->id == id) {
			onClick(btn);
			applyPinToolbarVisibility();
			return;
		}
	}
}

void ToolMain::applyNormalStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setColor(toolText());
	btn->setHoverColor(toolText());
	btn->setHoverBg(toolHover());
}

void ToolMain::cancelSelect()
{
	if (curId.empty()) return;
	for (auto b : btns) if (b->id == curId) applyNormalStyle(b);
	curId.clear();
	win->toolSub->hideTools();
	win->layoutTools();
}

void ToolMain::onClick(Ling::Button* btn)
{
	if (btn->id == L"close") { win->close(); return; }
	else if (btn->id == L"undo") { win->history->undo(); return; }
	else if (btn->id == L"redo") { win->history->redo(); return; }
	else if (btn->id == L"rotate") { StarCapPinTransform::rotateClockwise(win); return; }
	else if (btn->id == L"mirror") { StarCapPinTransform::mirrorHorizontal(win); return; }
	else if (btn->id == L"save") { win->saveToFile(); return; }
	else if (btn->id == L"clipboard") { win->copyToClipboard(); return; }

	if (btn->id == curId) { cancelSelect(); return; }
	for (auto b : btns)
	{
		if (b->id == curId) applyNormalStyle(b);
		if (b->id == btn->id) {
			b->setBg(toolSelected());
			b->setHoverBg(toolSelected());
			b->setColor(toolText());
			b->setHoverColor(toolText());
		}
	}
	curId = btn->id;
	if (curId == L"rect") win->toolSub->showRectTools();
	else if (curId == L"ellipse") win->toolSub->showEllipseTools();
	else if (curId == L"arrow") win->toolSub->showArrowTools();
	else if (curId == L"number") win->toolSub->showNumberTools();
	else if (curId == L"line") win->toolSub->showLineTools();
	else if (curId == L"text") win->toolSub->showTextTools();
	else if (curId == L"mosaic") win->toolSub->showMosaicTools();
	else if (curId == L"eraser") win->toolSub->showEraserTools();
	else win->toolSub->hideTools();
	win->layoutTools();
}

void ToolMain::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
