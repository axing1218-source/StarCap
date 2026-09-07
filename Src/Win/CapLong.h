#pragma once
#include <include/Ling.h>
#include "../StarCapOcr.h"

class WinCap;
class ToolLong;

class CapLong
{
public:
    CapLong(WinCap* win);
    ~CapLong();

    void dispose();
    void onMove(POINT pos);
    void onUp(POINT pos);
    void onTimerCB(UINT timerId);
    void setCursor();
    void paint(ID2D1DeviceContext* ctx);

    void copyToClipboard();
    bool saveToFile();
    void pin();
    bool ocr();
    bool translate();

    bool hasImage() const { return resultH > 0 && imgW > 0; }
    void layoutTool();

    // Long Capture Next keeps manual scrolling first-class. The toolbar button and Space both
    // toggle the adaptive automatic scroll driver instead of permanently switching modes.
    void startAutoScroll();
    void toggleAutoScroll();
    bool isAutoScrolling() const { return autoScroll; }
    void hotkeyEnter();
    void hotkeyEscape();

private:
    enum class CaptureState
    {
        Ready,
        Confirmed,
        Waiting,
        Mismatch,
        Paused,
    };

    enum class ScrollStrategy
    {
        Uia = 0,
        ChildWheel,
        RootWheel,
        SendInput,
    };

    struct MatchResult
    {
        bool accepted{ false };
        bool duplicate{ false };
        int offset{ 0 };
        int staticTop{ 0 };
        int staticBottom{ 0 };
        double visualScore{ 1.0 };
        double secondScore{ 1.0 };
        double pixelMad{ 255.0 };
        double expectedOffset{ 0.0 };
        bool usedStructuredPrior{ false };
    };

    struct Frame
    {
        std::vector<BYTE> pixels;
        ULONGLONG tick{ 0 };
    };

    struct ImageChunk
    {
        int height{ 0 };
        std::vector<BYTE> pixels;
    };

    void firstStep();
    void makeTool();
    void makeImgPreview();
    void paintImgPreview(ID2D1DeviceContext* ctx);
    void stopCap(bool showMessage = false);
    void makeStopText();

    void scheduleFrameCapture(int delayMs = 45);
    void scheduleAutoScroll(int delayMs = 65);
    void captureFrame();
    void processFrame(std::vector<BYTE> data);
    MatchResult matchFrame(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame);
    void commitFrame(const std::vector<BYTE>& data, const MatchResult& match);

    void initializeSlices(const MatchResult& firstMatch);
    void appendBodyRows(const std::vector<BYTE>& frame, int sourceY, int rows);
    void updateFooter(const std::vector<BYTE>& frame);
    const BYTE* logicalRowPtr(int y) const;
    bool ensureMaterialized();
    void invalidateMaterialized();
    size_t bufferedBytes() const;
    int storageHeightLimit() const;

    void setState(CaptureState state, const wchar_t* reason = nullptr);
    void resolveScrollTargets();
    bool initializeUiaScroll();
    void releaseUiaScroll();
    bool queryUiaMetrics(double& verticalPercent, double& verticalViewSize) const;
    bool dispatchUiaScroll();
    bool dispatchWheelScroll();
    void dispatchAutoScroll();
    bool advanceScrollStrategy();
    void pauseAuto(const wchar_t* reason);

    void installControlHook();
    void uninstallControlHook();

private:
    WinCap* win{ nullptr };
    std::unique_ptr<ToolLong> tool;

    bool isCapturing{ false };
    bool isFinish{ false };
    bool autoScroll{ false };
    bool slicesInitialized{ false };
    bool materializedDirty{ false };
    bool storageLimitReached{ false };

    CaptureState state{ CaptureState::Ready };
    ScrollStrategy scrollStrategy{ ScrollStrategy::Uia };
    int noProgressFrames{ 0 };
    int rejectedFrames{ 0 };
    int acceptedFrames{ 0 };
    int scrollSequence{ 0 };

    D2D1_RECT_F stopTextRect{};
    D2D1_POINT_2F stopTextPos{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextEnd;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> imgPreview;

    POINT capStartPos{};
    POINT autoTargetPoint{};
    HWND targetHwnd{ nullptr };
    HWND targetRootHwnd{ nullptr };

    // Kept as IUnknown here so the public header does not need to pull UIAutomation headers into
    // every translation unit. CapLong.cpp casts them to the native UIA interfaces.
    Microsoft::WRL::ComPtr<IUnknown> uiaAutomation;
    Microsoft::WRL::ComPtr<IUnknown> uiaScrollPattern;
    double pendingUiaBeforePercent{ -1.0 };
    double pendingUiaViewSize{ -1.0 };
    double structuredExpectedOffset{ 0.0 };

    std::vector<Frame> frameRing;
    std::vector<BYTE> committedFrame;
    std::vector<BYTE> imgData;

    std::vector<BYTE> headerData;
    std::vector<BYTE> footerData;
    std::vector<ImageChunk> bodyChunks;
    int staticTop{ 0 };
    int staticBottom{ 0 };
    int bodyHeight{ 0 };

    int imgW{ 0 };
    int imgH{ 0 };
    int resultH{ 0 };
};
