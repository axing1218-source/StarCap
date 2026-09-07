#include "pch.h"
#include "../Win/WinCap.h"
#include "../Win/CapLong.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolLong.h"

ToolLong::ToolLong(WinCap* win, CapLong* capLong) : Ling::WinBase(), win(win), capLong(capLong)
{
    dpi = win->dpi;
    refreshSize();
    onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
    onDpiChanged.add([this]() { dpiChanged = true; });
    onSizeChanged.add([this]() {
        if (!dpiChanged) return;
        dpiChanged = false;
        refreshSize();
        this->win->layoutLongTool();
    });
}

void ToolLong::refreshSize()
{
    setSize(btnSize * static_cast<float>(btnIds.size()), btnSize);
}

ToolLong::~ToolLong()
{
}

void ToolLong::onCreated()
{
    tip = std::make_unique<Tip>(this);
    // The tooltip is a separate top-level window. Keep it visible to the user, but never let
    // long-capture frame grabs record it as page content.
    tip->excludeFromCapture();
    body->setBg(0xFFFFFFFF);
    body->setBorder(1.f, 0xA8A8A8ff);
    body->setAlignItems(Ling::Align::Center);
    body->setFlexDirection(Ling::FlexDirection::Row);

    for (size_t i = 0; i < btnIds.size(); i++)
    {
        auto btn = body->makeChild<Ling::Button>();
        btn->setId(btnIds[i]);
        btn->setText(btnCodes[i]);
        btn->setHeightPercent(100.f);
        btn->setFlexGrow(1.f);
        btn->setHoverBg(0xF2F2F2ff);

        if (btnIds[i] == L"status") {
            statusBtn = btn;
            btn->setFontFamily(L"Microsoft YaHei");
            btn->setFontSize(13.f);
            tip->bind(btn, L"长截图状态：等待滚动");
        }
        else if (btnIds[i] == L"auto" || btnIds[i] == L"translate") {
            btn->setFontFamily(L"Microsoft YaHei");
            btn->setFontSize(12.f);
            if (btnIds[i] == L"auto") {
                autoBtn = btn;
                tip->bind(btn, L"自动滚动 / 暂停（Space）");
            }
            else tip->bind(btn, L"翻译长截图");
        }
        else {
            btn->setFontFamily(L"icon");
            btn->setFontSize(13.f);
            if (btnIds[i] == L"ocr") tip->bind(btn, Lang::get(L"cap.ocr"));
            else tip->bind(btn, Lang::get(std::format(L"tool.{}", btnIds[i])));
        }

        btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
    }

    show();
}

void ToolLong::setCaptureStatus(const std::wstring& code)
{
    if (statusBtn) statusBtn->setText(code);
}

void ToolLong::setAutoRunning(bool running)
{
    if (autoBtn) autoBtn->setText(running ? L"Ⅱ" : L"▶");
}

void ToolLong::onClick(Ling::Button* btn)
{
    if (btn->id == L"status") return;

    if (btn->id == L"auto") {
        if (capLong) capLong->toggleAutoScroll();
        return;
    }
    if (btn->id == L"ocr") {
        if (!capLong || !capLong->ocr()) return;
        win->close();
        return;
    }
    if (btn->id == L"translate") {
        if (!capLong || !capLong->translate()) return;
        win->close();
        return;
    }
    if (btn->id == L"pin") {
        win->longPin();
    }
    else if (btn->id == L"clipboard") {
        win->longCopyToClipboard();
    }
    else if (btn->id == L"save") {
        if (!win->longSaveToFile()) return;
    }
    win->close();
}

void ToolLong::onMinMaxInfo(MINMAXINFO* mmi)
{
    mmi->ptMinTrackSize.x = 1;
    mmi->ptMinTrackSize.y = 1;
}
