#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "../StarCapOcr.h"
#include "../StarCapCaptureTranslate.h"
#include "ToolMain.h"
#include "ToolCap.h"

ToolCap::ToolCap(WinCap* win) : Ling::WinBase(), win(win)
{
	dpi = win->dpi;
	refreshSize();
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTool(this);
	});
}

void ToolCap::refreshSize()
{
	float logicW{ 0.f };
	for (auto& id : btnIds) logicW += (id == L"spliter") ? spliterW : btnSize;
	setSize(logicW, btnSize);
}

ToolCap::~ToolCap() {}

void ToolCap::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setAlignItems(Ling::Align::Center);
	body->setFlexDirection(Ling::FlexDirection::Row);
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (btnIds[i] == L"spliter") {
			auto spliter = body->makeChild<Ling::Node>();
			spliter->setSize(spliterW, 18.f);
			spliter->setBg(0xDDDDDDff);
			continue;
		}
		auto btn = body->makeChild<Ling::Button>();
		btn->setId(btnIds[i]);
		btn->setText(btnCodes[i]);
		btn->setWidth(btnSize);
		btn->setHeightPercent(100.f);
		btn->setHoverBg(0xF2F2F2ff);
		if (btnIds[i] == L"translate") {
			btn->setFontFamily(L"Microsoft YaHei");
			btn->setFontSize(12.f);
			tip->bind(btn, L"翻译 / 原文");
		}
		else if (btnIds[i] == L"pin") {
			btn->setFontFamily(L"Segoe MDL2 Assets");
			btn->setFontSize(12.f);
			tip->bind(btn, Lang::get(btnTips[i]));
		}
		else {
			btn->setFontFamily(L"icon");
			btn->setFontSize(13.f);
			if (!btnTips[i].empty()) tip->bind(btn, Lang::get(btnTips[i]));
		}
		btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
	}
	show();
}

void ToolCap::onClick(Ling::Button* btn)
{
	tip->hide();
	// Edit and plain pin are different entry points. Edit opens the complete
	// annotation toolbar immediately; Pin keeps its image-only default.
	if (btn->id == L"mark") {
		ToolMain::queueEditorOpen();
		win->startEditor();
	}
	else if (btn->id == L"long") win->startLong();
	else if (btn->id == L"video") win->startVideo();
	else if (btn->id == L"ocr") StarCapOcr::show(win);
	else if (btn->id == L"translate") StarCapCaptureTranslate::toggle(win);
	else if (btn->id == L"qrcode") win->startQrcode();
	else if (btn->id == L"pin") win->startPin();
	else if (btn->id == L"save") win->saveToFile();
	else if (btn->id == L"clipboard") {
		// The checkmark/clipboard button follows the same current-view rule as Enter:
		// translated view -> translated image, source view -> original screenshot.
		HWND translated = StarCapCaptureTranslate::translatedViewHwnd(win);
		if (translated && IsWindow(translated)) {
			PostMessageW(translated, WM_KEYDOWN, VK_RETURN, 0);
		}
		else {
			win->copyToClipboard();
		}
	}
	else if (btn->id == L"close") win->close();
}

void ToolCap::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
