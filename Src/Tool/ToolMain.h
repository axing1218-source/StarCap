#pragma once
#include <include/Ling.h>
class WinPin;
class Tip;
class ToolMain : public Ling::WinBase
{
public:
	ToolMain(WinPin* win);
	~ToolMain();
	static void init();
	static void queueInitialTool(const std::wstring& id);
	static void queueEditorOpen();
	float getBtnCenterX();
	void cancelSelect();
public:
	std::wstring curId;
private:
	void onCreated() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
	void applyQueuedInitialTool();
	void applyNormalStyle(Ling::Button* btn);
	void refreshSize();
	void installPinInteractions();
	void applyPinToolbarVisibility();
	void showPinContextMenu();
private:
	WinPin* win;
	bool dpiChanged{ false };
	bool pinToolbarVisible{ false };
	bool pinHooksInstalled{ false };
	winrt::event_token pinMouseDownToken{};
	winrt::event_token pinMouseUpToken{};
	static constexpr float btnSize{ 32.f };
	static constexpr float spliterW{ 1.f };
	static constexpr UINT_PTR initialToolTimerId{ 0x7A11 };
	static constexpr UINT_PTR pinHookTimerId{ 0x7A12 };
	std::vector<std::wstring> btnIds = { L"rect",L"ellipse",L"arrow",L"number",L"line",L"text",L"mosaic",L"eraser",L"|",L"undo",L"redo",L"|",L"rotate",L"mirror",L"|",L"close",L"save",L"clipboard" };
	std::vector<std::wstring> btnCodes = { L"\ue8e8",L"\ue6bc",L"\ue603",L"\ue776",L"\ue601",L"\ue6ec",L"\ue82e",L"\ue6be",L"|",L"\ued85",L"\ued8a",L"|",L"↻",L"⇆",L"|",L"\ue62d",L"\ue608",L"\ue6ad" };
	std::vector<Ling::Button*> btns;
	std::unique_ptr<Tip> tip;
};
