#pragma once
#include <array>
#include <include/Ling.h>
#include <UIAutomation.h>

class MagnifierPopup;

// 光标落在哪一块。Inside 是选区内部；8 个方向对应四角和四边中点控制柄。
enum class MaskHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// 截图遮罩与截图阶段辅助 UI。
// 交互按 Snipaste 的截图阶段重新整理：
// - 默认快速吸附窗口/原生子窗口；需要 UI 元素级细分时用 Tab 显式开启；
// - 尺寸标签只显示在选区外，外部无空间时隐藏；
// - 鼠标在选区内时，于右下显示像素放大镜、坐标和 RGB/HEX 取色；
// - 选区确定后放大镜使用独立最上层窗口，可覆盖截图工具栏；
// - 左下角按截图阶段显示完整/精简快捷键提示；
// - 选区保留 8 个可拖动控制点。
class CutMask
{
	friend class MagnifierPopup;
public:
	CutMask(Ling::WinBase* win);
	~CutMask();
	bool highlight(POINT pos);
	void startMakeRect(POINT pos);
	void makeRect(POINT pos);
	MaskHit hitTest(POINT pos) const;
	void startAdjust(POINT pos);
	void adjust(POINT pos);
	bool hasRect() const;
	void paint(ID2D1DeviceContext* ctx);
	void syncMagnifier(POINT pos);
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 选区定死之后（录屏 / 滚动截图）所有辅助 UI 都隐藏，避免录入内容。
	bool hideLabel{ false };
private:
	struct WindowCandidate
	{
		HWND hwnd{ nullptr };
		D2D1_RECT_F rect{};
	};

	void initWinRect();
	D2D1_RECT_F detectRegionAt(POINT pos, HWND* matchedWindow = nullptr);
	D2D1_RECT_F detectUiElementRect(HWND hwnd, POINT localPos, const D2D1_RECT_F& fallback);
	D2D1_RECT_F detectNativeChildRect(HWND hwnd, POINT localPos, const D2D1_RECT_F& fallback) const;
	D2D1_RECT_F monitorRectAt(POINT localPos) const;
	void applyDetectedRect(const D2D1_RECT_F& rect, bool refreshWindow);
	void makeLayout();
	void paintHandles(ID2D1DeviceContext* ctx);
	void paintMagnifier(ID2D1DeviceContext* ctx);
	void paintMagnifierPanel(ID2D1DeviceContext* ctx, POINT live, float left, float top);
	void updateMagnifierPopup(POINT live);
	void hideMagnifierPopup();
	void paintHelp(ID2D1DeviceContext* ctx);
	void suppressLegacyMagnifier(ID2D1DeviceContext* ctx);
	COLORREF sampleCapturedPixel(POINT pos);
	std::wstring colorText(COLORREF color) const;
	bool copyCurrentColor();
	void handleCaptureKey(UINT key);
	void moveCursorBy(int dx, int dy);
	void useCurrentScreenOrFull();
	void restoreHistory(int direction);
	void rememberRegion();
	std::array<std::pair<D2D1_POINT_2F, MaskHit>, 8> handlePoints() const;
	bool validRect(const D2D1_RECT_F& rect) const;
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleOutline;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushPanelBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushPanelBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHelpBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLabelBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushKeyBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushAccentSoft;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushCenterBorder;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenCpuCopy;
	D2D1_RECT_F layoutRect{};
	std::vector<WindowCandidate> winRect;
	POINT pressPos{};
	POINT cursorPos{ INT_MAX, INT_MAX };
	Ling::WinBase* win{ nullptr };
	Microsoft::WRL::ComPtr<IUIAutomation> automation;
	std::unique_ptr<MagnifierPopup> magnifierPopup;
	winrt::event_token onMouseMoveToken{};
	winrt::event_token onKeyDownToken{};
	winrt::event_token onMouseUpToken{};
	float paddingTop{ 2.f }, paddingMargin{3.f};
	MaskHit adjustHit{ MaskHit::None };
	D2D1_RECT_F adjustStartRect{};
	POINT adjustPressPos{};
	COLORREF sampledColor{ RGB(0, 0, 0) };
	POINT sampledPos{ INT_MAX, INT_MAX };
	bool colorHex{ false };
	// UIA/MSAA element walking can be expensive in Qt and some desktop apps.
	// Keep the normal hover path cheap; Tab explicitly enables deep element detection.
	bool detectUiElements{ false };
	bool fullScreenToggle{ false };
	bool legacyMagnifierSuppressed{ false };
	bool initialDetectionDone{ false };
	bool imeDisabled{ false };
	size_t historyCursor{ 0 };
	static constexpr float minSize{ 4.f };
	static std::vector<D2D1_RECT_F> regionHistory;
};
