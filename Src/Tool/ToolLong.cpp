#include "pch.h"
#include "../Win/WinCap.h"
#include "../Win/CapLong.h"
#include "../Lang.h"
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
    body->setBg(0xFFFFFFFF);
    body->setBorder(1.f, 0xA8A8A8ff);
    body->setAlignItems(Ling::Align::Center);
    body->setFlexDirection(Ling::FlexDirection::Row);

    // Long capture intentionally has no hover tooltips. The frame source is a
    // desktop BitBlt, so auxiliary StarCap windows can become part of a stitched
    // image when they overlap the selected region.
    for (size_t i = 0; i < btnIds.size(); i++)
    {
        auto btn = body->makeChild<Ling::Button>();
        btn->setId(btnIds[i]);
        btn->setText(btnCodes[i]);
        btn->setHeightPercent(100.f);
        btn->setFlexGrow(1.f);
        btn->setHoverBg(0xF2F2F2ff);

        if (btnIds[i] == L"auto" || btnIds[i] == L"translate") {
            btn->setFontFamily(L"Microsoft YaHei");
            btn->setFontSize(12.f);
            if (btnIds[i] == L"auto") autoBtn = btn;
        }
        else {
            btn->setFontFamily(L"icon");
            btn->setFontSize(13.f);
        }

        btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
    }

    show();
}

void ToolLong::setCaptureStatus(const std::wstring&)
{
    // Deliberately no-op. Capture state continues to be written to diagnostics,
    // but changing a visible glyph every frame caused needless toolbar repaints.
}

void ToolLong::setAutoRunning(bool running)
{
    if (autoBtn) autoBtn->setText(running ? L"Ⅱ" : L"▶");
}

void ToolLong::onClick(Ling::Button* btn)
{
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
