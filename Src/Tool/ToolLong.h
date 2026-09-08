#pragma once
#include <include/Ling.h>

class WinCap;
class CapLong;

class ToolLong : public Ling::WinBase
{
public:
    ToolLong(WinCap* win, CapLong* capLong);
    ~ToolLong();

    // Kept as a no-op so CapLong can continue emitting internal state transitions
    // to diagnostics without repainting the toolbar on every frame.
    void setCaptureStatus(const std::wstring& code);
    void setAutoRunning(bool running);

private:
    void onCreated() override;
    void onClick(Ling::Button* btn);
    void onMinMaxInfo(MINMAXINFO* mmi) override;
    void refreshSize();

private:
    WinCap* win;
    CapLong* capLong;
    bool dpiChanged{ false };
    static constexpr float btnSize{ 32.f };
    std::vector<std::wstring> btnIds = { L"auto",L"ocr",L"translate",L"pin",L"close",L"save",L"clipboard" };
    std::vector<std::wstring> btnCodes = { L"▶",L"\ue67b",L"译",L"\ue6a2",L"\ue62d",L"\ue608",L"\ue6ad" };
    Ling::Button* autoBtn{ nullptr };
};
