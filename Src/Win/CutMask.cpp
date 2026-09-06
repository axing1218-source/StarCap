#include "pch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <dwmapi.h>
#include <imm.h>
#include <UIAutomation.h>
#include <include/Ling.h>
#include "CutMask.h"
#include "WinCap.h"
#include "../Util.h"
#include "../Setting.h"
#include "../StarCapOcr.h"

#pragma comment(lib, "Uiautomationcore.lib")
#pragma comment(lib, "Imm32.lib")

using namespace Microsoft::WRL;

std::vector<D2D1_RECT_F> CutMask::regionHistory;

namespace
{
	bool pointInRect(const D2D1_RECT_F& r, POINT p)
	{
		return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
	}

	float rectArea(const D2D1_RECT_F& r)
	{
		return std::max(0.f, r.right - r.left) * std::max(0.f, r.bottom - r.top);
	}

	D2D1_RECT_F intersectRectF(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
	{
		return D2D1::RectF(
			std::max(a.left, b.left), std::max(a.top, b.top),
			std::min(a.right, b.right), std::min(a.bottom, b.bottom));
	}

	bool sameRectRounded(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
	{
		return std::lround(a.left) == std::lround(b.left) &&
			std::lround(a.top) == std::lround(b.top) &&
			std::lround(a.right) == std::lround(b.right) &&
			std::lround(a.bottom) == std::lround(b.bottom);
	}

	D2D1_RECT_F rectFromScreenRect(const RECT& r, const Ling::WinBase* win)
	{
		return D2D1::RectF(
			(float)(r.left - win->x), (float)(r.top - win->y),
			(float)(r.right - win->x), (float)(r.bottom - win->y));
	}
}


class MagnifierPopup : public Ling::WinBase
{
public:
    explicit MagnifierPopup(CutMask* owner) : owner(owner)
    {
        dpi = owner->win->dpi;
        setSize(179.f, 227.f);
    }

    void setCursorPoint(POINT p)
    {
        cursor = p;
        refresh();
    }

private:
    void onCreated() override
    {
        canvas = body->makeChild<Ling::Canvas>();
        canvas->enableSwapChain();
        canvas->setSizePercent(100.f, 100.f);
        show();
    }

    void layout() override
    {
        Ling::WinBase::layout();
        if (!canvas || !owner) return;
        auto ctx = canvas->startPaint();
        if (!ctx) return;
        ctx->Clear(0);
        owner->paintMagnifierPanel(ctx, cursor, 0.f, 0.f);
        canvas->finishPaint();
    }

    LRESULT onHitTest(const POINT) override
    {
        return HTTRANSPARENT;
    }

private:
    CutMask* owner{ nullptr };
    Ling::Canvas* canvas{ nullptr };
    POINT cursor{};
};

CutMask::CutMask(Ling::WinBase* win) : win{ win }
{
	auto borderWidth = std::clamp(Setting::get()->getToolNum(L"capture", L"borderWidth", 2.f), 0.f, 8.f);
	strokeWidth = borderWidth * win->dpi;
	paddingTop *= win->dpi;
	paddingMargin *= win->dpi;

	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.55f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x2080F0), brushBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x2080F0), brushHandle.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushHandleOutline.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .737f), brushPanelBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .737f), brushPanelBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .729f), brushHelpBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .565f), brushLabelBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 1.f), brushKeyBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x2080F0, .34f), brushAccentSoft.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brushCenterBorder.GetAddressOf());

	CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(automation.ReleaseAndGetAddressOf()));

	onMouseMoveToken = win->onMouseMove.add([this](POINT pos) {
		cursorPos = pos;
		auto* cap = static_cast<WinCap*>(this->win);
		if (!cap || hideLabel) {
			hideMagnifierPopup();
			return;
		}
		if (cap->stage == WinCap::CapStage::Adjust)
			updateMagnifierPopup(pos);
		else
			hideMagnifierPopup();
		if (cap->stage == WinCap::CapStage::Select || cap->stage == WinCap::CapStage::Adjust)
			this->win->refresh();
	});
	onKeyDownToken = win->onKeyDown.add([this](UINT key) { handleCaptureKey(key); });
	onMouseUpToken = win->onMouseUp.add([this](POINT, bool isRight) {
		if (isRight) return;
		auto* cap = static_cast<WinCap*>(this->win);
		if (!cap) return;
		if (cap->stage == WinCap::CapStage::Adjust && hasRect()) historyCursor = regionHistory.size();
	});

	historyCursor = regionHistory.size();
	initWinRect();
}

CutMask::~CutMask()
{
	hideMagnifierPopup();
	if (magnifierPopup) magnifierPopup->close();
	rememberRegion();
	if (win) {
		win->onMouseMove.remove(onMouseMoveToken);
		win->onKeyDown.remove(onKeyDownToken);
		win->onMouseUp.remove(onMouseUpToken);
	}
}

bool CutMask::validRect(const D2D1_RECT_F& rect) const
{
	return rect.right - rect.left >= minSize && rect.bottom - rect.top >= minSize;
}

void CutMask::initWinRect()
{
	winRect.clear();
	EnumWindows([](HWND hwnd, LPARAM lparam)
		{
			if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) return TRUE;
			const auto exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			if ((exStyle & WS_EX_LAYERED) != 0) {
				COLORREF key{}; BYTE alpha{ 255 }; DWORD flags{};
				if (GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) &&
					(flags & LWA_ALPHA) && alpha == 0) return TRUE;
				if (exStyle & WS_EX_TRANSPARENT) return TRUE;
			}
			RECT rect{};
			if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
				if (!GetWindowRect(hwnd, &rect)) return TRUE;
			}
			if (rect.right - rect.left <= 3 || rect.bottom - rect.top <= 3) return TRUE;
			auto self = reinterpret_cast<CutMask*>(lparam);
			auto host = self->win;
			const LONG hostL = host->x, hostT = host->y;
			const LONG hostR = host->x + (LONG)std::lround(host->w);
			const LONG hostB = host->y + (LONG)std::lround(host->h);
			rect.left = std::max(rect.left, hostL);
			rect.top = std::max(rect.top, hostT);
			rect.right = std::min(rect.right, hostR);
			rect.bottom = std::min(rect.bottom, hostB);
			if (rect.right - rect.left <= 3 || rect.bottom - rect.top <= 3) return TRUE;
			WindowCandidate item;
			item.hwnd = hwnd;
			item.rect = D2D1::RectF((float)(rect.left - hostL), (float)(rect.top - hostT),
				(float)(rect.right - hostL), (float)(rect.bottom - hostT));
			self->winRect.push_back(item);
			return TRUE;
		}, (LPARAM)this);
}

D2D1_RECT_F CutMask::monitorRectAt(POINT localPos) const
{
	POINT screenPos{ localPos.x + win->x, localPos.y + win->y };
	HMONITOR monitor = MonitorFromPoint(screenPos, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(mi) };
	if (!monitor || !GetMonitorInfoW(monitor, &mi))
		return D2D1::RectF(0.f, 0.f, win->w, win->h);
	return D2D1::RectF((float)(mi.rcMonitor.left - win->x), (float)(mi.rcMonitor.top - win->y),
		(float)(mi.rcMonitor.right - win->x), (float)(mi.rcMonitor.bottom - win->y));
}

D2D1_RECT_F CutMask::detectNativeChildRect(HWND hwnd, POINT localPos, const D2D1_RECT_F& fallback) const
{
	if (!hwnd) return fallback;
	POINT screenPos{ localPos.x + win->x, localPos.y + win->y };
	HWND current = hwnd;
	RECT best{};
	if (!GetWindowRect(current, &best)) return fallback;
	for (int depth = 0; depth < 12; ++depth) {
		POINT client = screenPos;
		if (!ScreenToClient(current, &client)) break;
		HWND child = ChildWindowFromPointEx(current, client,
			CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
		if (!child || child == current) break;
		RECT childRect{};
		if (!GetWindowRect(child, &childRect)) break;
		if (!PtInRect(&childRect, screenPos)) break;
		if (childRect.right - childRect.left < 3 || childRect.bottom - childRect.top < 3) break;
		best = childRect;
		current = child;
	}
	auto r = rectFromScreenRect(best, win);
	const auto clipped = intersectRectF(r, fallback);
	return validRect(clipped) ? clipped : fallback;
}

D2D1_RECT_F CutMask::detectUiElementRect(HWND hwnd, POINT localPos, const D2D1_RECT_F& fallback)
{
	if (!automation || !hwnd) return detectNativeChildRect(hwnd, localPos, fallback);
	POINT screenPos{ localPos.x + win->x, localPos.y + win->y };
	ComPtr<IUIAutomationElement> direct;
	if (SUCCEEDED(automation->ElementFromPoint(screenPos, direct.GetAddressOf())) && direct) {
		UIA_HWND nativeHandle{};
		RECT rr{};
		BOOL offscreen = FALSE;
		direct->get_CurrentNativeWindowHandle(&nativeHandle);
		if ((HWND)nativeHandle != win->hwnd &&
			SUCCEEDED(direct->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
			SUCCEEDED(direct->get_CurrentBoundingRectangle(&rr))) {
			auto local = intersectRectF(rectFromScreenRect(rr, win), fallback);
			const float area = rectArea(local);
			if (validRect(local) && area >= 36.f && area < rectArea(fallback) * .995f)
				return local;
		}
	}
	ComPtr<IUIAutomationElement> current;
	if (SUCCEEDED(automation->ElementFromHandle(hwnd, current.GetAddressOf())) && current) {
		ComPtr<IUIAutomationTreeWalker> walker;
		automation->get_ControlViewWalker(walker.GetAddressOf());
		D2D1_RECT_F best = fallback;
		for (int depth = 0; walker && current && depth < 14; ++depth) {
			ComPtr<IUIAutomationElement> child;
			if (FAILED(walker->GetFirstChildElement(current.Get(), child.GetAddressOf())) || !child) break;
			ComPtr<IUIAutomationElement> bestChild;
			D2D1_RECT_F bestChildRect{};
			float bestChildArea = FLT_MAX;
			int siblingCount = 0;
			while (child && siblingCount++ < 256) {
				BOOL offscreen = FALSE;
				RECT rr{};
				if (SUCCEEDED(child->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
					SUCCEEDED(child->get_CurrentBoundingRectangle(&rr)) &&
					rr.right - rr.left >= 3 && rr.bottom - rr.top >= 3 && PtInRect(&rr, screenPos)) {
					auto local = intersectRectF(rectFromScreenRect(rr, win), fallback);
					const float area = rectArea(local);
					if (validRect(local) && area >= 36.f && area < bestChildArea) {
						bestChildArea = area;
						bestChildRect = local;
						bestChild = child;
					}
				}
				ComPtr<IUIAutomationElement> next;
				if (FAILED(walker->GetNextSiblingElement(child.Get(), next.GetAddressOf()))) break;
				child = next;
			}
			if (!bestChild) break;
			best = bestChildRect;
			current = bestChild;
		}
		if (validRect(best) && rectArea(best) < rectArea(fallback) * .995f) return best;
	}
	return detectNativeChildRect(hwnd, localPos, fallback);
}

D2D1_RECT_F CutMask::detectRegionAt(POINT pos, HWND* matchedWindow)
{
	if (matchedWindow) *matchedWindow = nullptr;
	for (const auto& item : winRect) {
		if (!pointInRect(item.rect, pos)) continue;
		wchar_t cls[96]{};
		GetClassNameW(item.hwnd, cls, (int)std::size(cls));
		if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
			return monitorRectAt(pos);
		if (matchedWindow) *matchedWindow = item.hwnd;
		return detectUiElements ? detectUiElementRect(item.hwnd, pos, item.rect) : item.rect;
	}
	return monitorRectAt(pos);
}

void CutMask::applyDetectedRect(const D2D1_RECT_F& rect, bool refreshWindow)
{
	if (!validRect(rect)) return;
	if (sameRectRounded(rect, maskRect)) return;
	maskRect = rect;
	makeLayout();
	if (refreshWindow) win->refresh();
}

bool CutMask::highlight(POINT pos)
{
	cursorPos = pos;
	HWND matched{};
	auto rect = detectRegionAt(pos, &matched);
	if (!validRect(rect) || sameRectRounded(rect, maskRect)) return false;
	applyDetectedRect(rect, true);
	return true;
}

void CutMask::makeLayout()
{
	layout.Reset();
	layoutRect = {};
	if (!hasRect()) return;

	const int width = std::max(0, (int)std::lround(maskRect.right - maskRect.left));
	const int height = std::max(0, (int)std::lround(maskRect.bottom - maskRect.top));
	const auto text = std::format(L"{} x {} px", width, height);
	layout = Ling::D2D::get()->makeTextLayout(text, 11.f * win->dpi);
	if (!layout) return;

	DWRITE_TEXT_METRICS tm{};
	layout->GetMetrics(&tm);
	const float scale = win->dpi;
	const float boxH = 30.f * scale;
	const float padX = 7.f * scale;
	const float gap = 3.f * scale;
	const float boxW = tm.width + padX * 2.f;
	const auto r = maskRect;

	// Hard rule: the size label may never cover even one pixel of the capture.
	// Try the four outside corners first, then the two vertical sides. If a
	// fullscreen/edge-to-edge selection leaves no external room, hide it.
	const std::array<D2D1_RECT_F, 8> candidates{
		D2D1::RectF(r.left,             r.top - gap - boxH, r.left + boxW,  r.top - gap),
		D2D1::RectF(r.right - boxW,     r.top - gap - boxH, r.right,        r.top - gap),
		D2D1::RectF(r.left,             r.bottom + gap,      r.left + boxW,  r.bottom + gap + boxH),
		D2D1::RectF(r.right - boxW,     r.bottom + gap,      r.right,        r.bottom + gap + boxH),
		D2D1::RectF(r.left - gap-boxW,  r.top,               r.left-gap,     r.top + boxH),
		D2D1::RectF(r.right + gap,      r.top,               r.right+gap+boxW, r.top + boxH),
		D2D1::RectF(r.left - gap-boxW,  r.bottom-boxH,       r.left-gap,     r.bottom),
		D2D1::RectF(r.right + gap,      r.bottom-boxH,       r.right+gap+boxW, r.bottom)
	};

	auto insideWindow = [this](const D2D1_RECT_F& c) {
		return c.left >= 0.f && c.top >= 0.f && c.right <= win->w && c.bottom <= win->h;
	};
	auto overlapsCapture = [r](const D2D1_RECT_F& c) {
		return c.left < r.right && c.right > r.left && c.top < r.bottom && c.bottom > r.top;
	};

	bool placed = false;
	for (const auto& candidate : candidates) {
		if (!insideWindow(candidate) || overlapsCapture(candidate)) continue;
		layoutRect = candidate;
		placed = true;
		break;
	}
	if (!placed) {
		layout.Reset();
		layoutRect = {};
		return;
	}

	layout->SetMaxWidth(std::max(1.f, boxW - padX * 2.f));
	layout->SetMaxHeight(boxH);
}

void CutMask::startMakeRect(POINT pos)
{
	pressPos = pos;
	cursorPos = pos;
}

void CutMask::makeRect(POINT pos)
{
	cursorPos = pos;
	auto [left, right] = std::minmax(pressPos.x, pos.x);
	auto [top, bottom] = std::minmax(pressPos.y, pos.y);
	maskRect.left = (float)left;
	maskRect.right = (float)right;
	maskRect.top = (float)top;
	maskRect.bottom = (float)bottom;
	makeLayout();
	win->refresh();
}

bool CutMask::hasRect() const
{
	return maskRect.right > maskRect.left && maskRect.bottom > maskRect.top;
}

std::array<std::pair<D2D1_POINT_2F, MaskHit>, 8> CutMask::handlePoints() const
{
	const float cx = (maskRect.left + maskRect.right) * .5f;
	const float cy = (maskRect.top + maskRect.bottom) * .5f;
	return {{
		{ D2D1::Point2F(maskRect.left,  maskRect.top),    MaskHit::TopLeft },
		{ D2D1::Point2F(cx,             maskRect.top),    MaskHit::Top },
		{ D2D1::Point2F(maskRect.right, maskRect.top),    MaskHit::TopRight },
		{ D2D1::Point2F(maskRect.right, cy),              MaskHit::Right },
		{ D2D1::Point2F(maskRect.right, maskRect.bottom), MaskHit::BottomRight },
		{ D2D1::Point2F(cx,             maskRect.bottom), MaskHit::Bottom },
		{ D2D1::Point2F(maskRect.left,  maskRect.bottom), MaskHit::BottomLeft },
		{ D2D1::Point2F(maskRect.left,  cy),              MaskHit::Left }
	}};
}

MaskHit CutMask::hitTest(POINT pos) const
{
	if (!hasRect()) return MaskHit::None;
	const float px = (float)pos.x, py = (float)pos.y;
	const float hitRadius = std::max(6.f, 6.f * win->dpi);
	const float hitRadius2 = hitRadius * hitRadius;
	for (const auto& [p, hit] : handlePoints()) {
		const float dx = px - p.x, dy = py - p.y;
		if (dx * dx + dy * dy <= hitRadius2) return hit;
	}
	const float edge = std::max(4.f, 4.f * win->dpi);
	const auto& r = maskRect;
	const bool withinX = px >= r.left - edge && px <= r.right + edge;
	const bool withinY = py >= r.top - edge && py <= r.bottom + edge;
	if (withinX && std::fabs(py - r.top) <= edge) return MaskHit::Top;
	if (withinX && std::fabs(py - r.bottom) <= edge) return MaskHit::Bottom;
	if (withinY && std::fabs(px - r.left) <= edge) return MaskHit::Left;
	if (withinY && std::fabs(px - r.right) <= edge) return MaskHit::Right;
	if (px > r.left && px < r.right && py > r.top && py < r.bottom) return MaskHit::Inside;
	return MaskHit::None;
}

void CutMask::startAdjust(POINT pos)
{
	if (StarCapOcr::containsPoint(pos)) {
		adjustHit = MaskHit::None;
		return;
	}
	cursorPos = pos;
	adjustHit = hitTest(pos);
	adjustStartRect = maskRect;
	adjustPressPos = pos;
	if (adjustHit != MaskHit::None && adjustHit != MaskHit::Inside) adjust(pos);
}

void CutMask::adjust(POINT pos)
{
	cursorPos = pos;
	if (adjustHit == MaskHit::None) return;
	auto r = adjustStartRect;
	const float px = (float)pos.x, py = (float)pos.y;
	if (adjustHit == MaskHit::Inside) {
		const float rw = r.right - r.left, rh = r.bottom - r.top;
		const float left = std::clamp(r.left + px - adjustPressPos.x, 0.f, win->w - rw);
		const float top = std::clamp(r.top + py - adjustPressPos.y, 0.f, win->h - rh);
		r = D2D1::RectF(left, top, left + rw, top + rh);
	}
	else {
		const float cx = std::clamp(px, 0.f, win->w);
		const float cy = std::clamp(py, 0.f, win->h);
		switch (adjustHit)
		{
		case MaskHit::Left: r.left = cx; break;
		case MaskHit::Right: r.right = cx; break;
		case MaskHit::Top: r.top = cy; break;
		case MaskHit::Bottom: r.bottom = cy; break;
		case MaskHit::TopLeft: r.left = cx; r.top = cy; break;
		case MaskHit::TopRight: r.right = cx; r.top = cy; break;
		case MaskHit::BottomRight: r.right = cx; r.bottom = cy; break;
		case MaskHit::BottomLeft: r.left = cx; r.bottom = cy; break;
		default: break;
		}
		const float l = std::min(r.left, r.right), rr = std::max(r.left, r.right);
		const float t = std::min(r.top, r.bottom), b = std::max(r.top, r.bottom);
		r = D2D1::RectF(l, t, rr, b);
		const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
		const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
		if (r.right - r.left < minSize) {
			if (moveLeft) r.left = r.right - minSize;
			else r.right = r.left + minSize;
		}
		if (r.bottom - r.top < minSize) {
			if (moveTop) r.top = r.bottom - minSize;
			else r.bottom = r.top + minSize;
		}
	}
	if (sameRectRounded(r, maskRect)) return;
	maskRect = r;
	makeLayout();
	win->refresh();
}

void CutMask::paintHandles(ID2D1DeviceContext* ctx)
{
	if (!ctx || !hasRect() || hideLabel || !brushHandle) return;
	const float radius = std::max(3.2f, 3.6f * win->dpi);
	for (const auto& [p, _] : handlePoints()) {
		D2D1_ELLIPSE outer{ p, radius, radius };
		if (brushHandleOutline) ctx->FillEllipse(outer, brushHandleOutline.Get());
		const float innerRadius = std::max(1.f, radius - 1.f * win->dpi);
		ctx->FillEllipse(D2D1_ELLIPSE{ p, innerRadius, innerRadius }, brushHandle.Get());
	}
}

COLORREF CutMask::sampleCapturedPixel(POINT pos)
{
	if (pos.x == sampledPos.x && pos.y == sampledPos.y) return sampledColor;
	auto* cap = static_cast<WinCap*>(win);
	if (!cap || !cap->screenImg || pos.x < 0 || pos.y < 0 ||
		pos.x >= (int)std::lround(win->w) || pos.y >= (int)std::lround(win->h))
		return sampledColor;
	if (!screenCpuCopy) {
		auto ctx = Ling::D2D::get()->deviceContext.Get();
		D2D1_BITMAP_PROPERTIES1 props{};
		props.pixelFormat = cap->screenImg->GetPixelFormat();
		props.dpiX = 96.f;
		props.dpiY = 96.f;
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
		auto size = cap->screenImg->GetPixelSize();
		if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &props, screenCpuCopy.GetAddressOf())) || !screenCpuCopy)
			return sampledColor;
		if (FAILED(screenCpuCopy->CopyFromBitmap(nullptr, cap->screenImg.Get(), nullptr))) {
			screenCpuCopy.Reset();
			return sampledColor;
		}
	}
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(screenCpuCopy->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return sampledColor;
	const BYTE* px = mapped.bits + (size_t)pos.y * mapped.pitch + (size_t)pos.x * 4;
	const BYTE b = px[0], g = px[1], r = px[2];
	screenCpuCopy->Unmap();
	sampledColor = RGB(r, g, b);
	sampledPos = pos;
	return sampledColor;
}

std::wstring CutMask::colorText(COLORREF color) const
{
	if (colorHex)
		return std::format(L"#{:02X}{:02X}{:02X}", GetRValue(color), GetGValue(color), GetBValue(color));
	return std::format(L"{}, {}, {}", GetRValue(color), GetGValue(color), GetBValue(color));
}

void CutMask::paintMagnifierPanel(ID2D1DeviceContext* ctx, POINT live, float left, float top)
{
    if (!ctx) return;
    auto* cap = static_cast<WinCap*>(win);
    if (!cap || !cap->screenImg) return;

    const float scale = win->dpi;
    const int cols = 15;
    const int rows = 10;
    const float panelW = 179.f * scale;
    const float imageH = 122.f * scale;
    const float infoH = 105.f * scale;
    const float panelH = imageH + infoH;
    const float cellW = panelW / cols;
    const float cellH = imageH / rows;
    const auto panelRect = D2D1::RectF(left, top, left + panelW, top + panelH);
    const auto imageRect = D2D1::RectF(left, top, left + panelW, top + imageH);

    ctx->FillRectangle(panelRect, brushPanelBg.Get());

    const int centerCol = cols / 2;
    const int centerRow = rows / 2;
    const int wantL = live.x - centerCol;
    const int wantT = live.y - centerRow;
    const int validL = std::max(wantL, 0);
    const int validT = std::max(wantT, 0);
    const int validR = std::min(wantL + cols, (int)std::lround(win->w));
    const int validB = std::min(wantT + rows, (int)std::lround(win->h));
    if (validR > validL && validB > validT) {
        D2D1_RECT_F src{ (float)validL, (float)validT, (float)validR, (float)validB };
        D2D1_RECT_F dst{
            left + (validL - wantL) * cellW,
            top + (validT - wantT) * cellH,
            left + (validR - wantL) * cellW,
            top + (validB - wantT) * cellH
        };
        ctx->DrawBitmap(cap->screenImg.Get(), dst, 1.f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);
    }

    // Keep the pixel locator hard-edged. The blue cross and the black one-pixel
    // outline are both 10% thinner than V3 and are painted with aliased geometry.
    const auto oldAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    const float cx = left + centerCol * cellW;
    const float cy = top + centerRow * cellH;
    const float crossThickness = 6.3f * scale;
    const float crossHalf = crossThickness * .5f;
    const float cellCenterX = cx + cellW * .5f;
    const float cellCenterY = cy + cellH * .5f;

    // Snipaste-style target: the blue cross stops at a small black frame.
    // The frame itself is outside the observation area and its interior is
    // completely transparent, leaving the sampled source pixel unobscured.
    // At 100% DPI this is an 11x11 outer frame with a 1px hard black border
    // and a 9x9 transparent center.
    const float outlineThickness = 1.f;
    const float frameSize = 11.f * scale;
    const float frameL = std::round(cellCenterX - frameSize * .5f);
    const float frameT = std::round(cellCenterY - frameSize * .5f);
    const float frameR = frameL + frameSize;
    const float frameB = frameT + frameSize;

    // While the user is actively dragging a new capture, keep the top-left
    // magnifier quadrant fully clear and de-emphasize the other three with a
    // translucent black layer. The center target and cross bands are excluded
    // so the sampled pixel remains completely unobscured.
    if (cap->stage == WinCap::CapStage::Select && cap->isPress) {
        ComPtr<ID2D1SolidColorBrush> quadrantShade;
        ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .40f), quadrantShade.GetAddressOf());
        if (quadrantShade) {
            ctx->FillRectangle(D2D1::RectF(frameR, top, left + panelW, frameT), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(left, frameB, frameL, top + imageH), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(frameR, frameB, left + panelW, top + imageH), quadrantShade.Get());
        }
    }

    // Cross arms end at the OUTSIDE edge of the black frame; nothing is
    // painted across the transparent center target.
    ctx->FillRectangle(D2D1::RectF(left, cellCenterY - crossHalf, frameL, cellCenterY + crossHalf), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, top, cellCenterX + crossHalf, frameT), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH), brushAccentSoft.Get());

    // Four equal filled bars guarantee the top/bottom/left/right border has
    // identical thickness. Do not fill the center.
    ctx->FillRectangle(D2D1::RectF(frameL, frameT, frameR, frameT + outlineThickness), brushCenterBorder.Get());
    ctx->FillRectangle(D2D1::RectF(frameL, frameB - outlineThickness, frameR, frameB), brushCenterBorder.Get());
    ctx->FillRectangle(D2D1::RectF(frameL, frameT + outlineThickness, frameL + outlineThickness, frameB - outlineThickness), brushCenterBorder.Get());
    ctx->FillRectangle(D2D1::RectF(frameR - outlineThickness, frameT + outlineThickness, frameR, frameB - outlineThickness), brushCenterBorder.Get());
    // Give the magnifier image border the same treatment as the center target:
    // one physical pixel, pure black, hard edged, four equal bars, transparent
    // interior. This avoids the fuzzy/asymmetric rasterization of DrawRectangle.
    const float hardBorder = 1.f;
    auto fillHardFrame = [&](const D2D1_RECT_F& r) {
        ctx->FillRectangle(D2D1::RectF(r.left, r.top, r.right, r.top + hardBorder), brushCenterBorder.Get());
        ctx->FillRectangle(D2D1::RectF(r.left, r.bottom - hardBorder, r.right, r.bottom), brushCenterBorder.Get());
        ctx->FillRectangle(D2D1::RectF(r.left, r.top + hardBorder, r.left + hardBorder, r.bottom - hardBorder), brushCenterBorder.Get());
        ctx->FillRectangle(D2D1::RectF(r.right - hardBorder, r.top + hardBorder, r.right, r.bottom - hardBorder), brushCenterBorder.Get());
    };
    fillHardFrame(imageRect);
    ctx->SetAntialiasMode(oldAA);

    const COLORREF color = sampleCapturedPixel(live);
    auto d2d = Ling::D2D::get();
    auto drawLine = [&](const std::wstring& text, float y, float fontSize, float xOffset = 0.f) {
        auto tl = d2d->makeTextLayout(text, fontSize * scale);
        if (!tl) return;
        ctx->DrawTextLayout({ left + 10.f * scale + xOffset, y }, tl.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    };

    const float infoTop = top + imageH;
    const POINT screenPos{ live.x + win->x, live.y + win->y };
    drawLine(std::format(L"({}, {})", screenPos.x, screenPos.y), infoTop + 8.f * scale, 11.5f);

    const float swatchSize = 12.f * scale;
    const float swatchTop = infoTop + 35.f * scale;
    ComPtr<ID2D1SolidColorBrush> swatch;
    ctx->CreateSolidColorBrush(D2D1::ColorF(
        GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f, 1.f), swatch.GetAddressOf());
    if (swatch) ctx->FillRectangle(D2D1::RectF(left + 11.f * scale, swatchTop,
        left + 11.f * scale + swatchSize, swatchTop + swatchSize), swatch.Get());
    ctx->DrawRectangle(D2D1::RectF(left + 11.f * scale, swatchTop,
        left + 11.f * scale + swatchSize, swatchTop + swatchSize),
        brushText.Get(), std::max(1.f, scale));
    drawLine(colorText(color), infoTop + 32.f * scale, 11.5f, 32.f * scale);
    drawLine(L"C  复制颜色值", infoTop + 60.f * scale, 10.5f);
    drawLine(L"Shift  切换 RGB/HEX", infoTop + 82.f * scale, 10.5f);

    // Outer magnifier frame: same 1px pure-black hard-edge construction.
    const auto oldPanelAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    fillHardFrame(panelRect);
    ctx->SetAntialiasMode(oldPanelAA);
}

void CutMask::hideMagnifierPopup()
{
    if (magnifierPopup && magnifierPopup->hwnd)
        ShowWindow(magnifierPopup->hwnd, SW_HIDE);
}

void CutMask::updateMagnifierPopup(POINT live)
{
    auto* cap = static_cast<WinCap*>(win);
    if (!cap || hideLabel || cap->stage != WinCap::CapStage::Adjust ||
        !hasRect() || !pointInRect(maskRect, live)) {
        hideMagnifierPopup();
        return;
    }

    const float scale = win->dpi;
    const float panelW = 179.f * scale;
    const float panelH = 227.f * scale;
    const float dx = 8.f * scale;
    const float dy = 20.f * scale;
    float left = live.x + dx;
    float top = live.y + dy;
    if (left + panelW > win->w) left = live.x - dx - panelW;
    if (top + panelH > win->h) top = live.y - dy - panelH;
    left = std::clamp(left, 0.f, std::max(0.f, win->w - panelW));
    top = std::clamp(top, 0.f, std::max(0.f, win->h - panelH));

    const int screenX = win->x + (int)std::lround(left);
    const int screenY = win->y + (int)std::lround(top);
    if (!magnifierPopup) {
        magnifierPopup = std::make_unique<MagnifierPopup>(this);
        magnifierPopup->setPosition(screenX, screenY);
        magnifierPopup->createNativeWindow(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            WS_POPUP);
    }

    magnifierPopup->setCursorPoint(live);
    if (magnifierPopup->hwnd) {
        SetWindowPos(magnifierPopup->hwnd, HWND_TOPMOST, screenX, screenY, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void CutMask::syncMagnifier(POINT pos)
{
    auto* cap = static_cast<WinCap*>(win);
    if (cap && cap->stage == WinCap::CapStage::Adjust)
        updateMagnifierPopup(pos);
    else
        hideMagnifierPopup();
}

void CutMask::paintMagnifier(ID2D1DeviceContext* ctx)
{
    if (!ctx || hideLabel) {
        hideMagnifierPopup();
        return;
    }
    auto* cap = static_cast<WinCap*>(win);
    if (!cap || !cap->screenImg) {
        hideMagnifierPopup();
        return;
    }
    if (cap->stage != WinCap::CapStage::Select && cap->stage != WinCap::CapStage::Adjust) {
        hideMagnifierPopup();
        return;
    }

    POINT live{};
    GetCursorPos(&live);
    ScreenToClient(win->hwnd, &live);
    cursorPos = live;
    if (live.x < 0 || live.y < 0 || live.x >= (int)std::lround(win->w) || live.y >= (int)std::lround(win->h)) {
        hideMagnifierPopup();
        return;
    }
    if (hasRect() && !pointInRect(maskRect, live)) {
        hideMagnifierPopup();
        return;
    }

    // Adjust stage uses its own topmost popup so the magnifier can cover ToolCap.
    if (cap->stage == WinCap::CapStage::Adjust) {
        updateMagnifierPopup(live);
        return;
    }
    hideMagnifierPopup();

    const float scale = win->dpi;
    const float panelW = 179.f * scale;
    const float panelH = 227.f * scale;
    const float dx = 8.f * scale;
    const float dy = 20.f * scale;
    float left = live.x + dx;
    float top = live.y + dy;
    if (left + panelW > win->w) left = live.x - dx - panelW;
    if (top + panelH > win->h) top = live.y - dy - panelH;
    left = std::clamp(left, 0.f, std::max(0.f, win->w - panelW));
    top = std::clamp(top, 0.f, std::max(0.f, win->h - panelH));
    paintMagnifierPanel(ctx, live, left, top);
}

void CutMask::paintHelp(ID2D1DeviceContext* ctx)
{
    if (!ctx || hideLabel) return;
    auto* cap = static_cast<WinCap*>(win);
    if (!cap || cap->stage != WinCap::CapStage::Select) return;

    const float scale = win->dpi;
    const bool dragging = cap->isPress;
    auto d2d = Ling::D2D::get();

    struct HelpRow
    {
        std::vector<std::wstring> keys;
        std::wstring desc;
    };

    std::vector<HelpRow> rows{
        {{ L"W", L"A", L"S", L"D" }, L"移动鼠标指针 1 像素"},
        {{ L"Tab" }, L"切换窗口 / 界面元素检测"},
        {{ L"Ctrl", L"A" }, L"当前屏幕 / 全屏"}
    };
    if (!dragging) {
        rows.push_back({ { L"R", L"Shift", L"R" }, L"使用上一次截图区域" });
        rows.push_back({ { L",", L"." }, L"回溯截图区域历史" });
        rows.push_back({ { L"C", L"Shift" }, L"复制颜色 / 切换 RGB/HEX" });
    }

    const float sidePad = 13.f * scale;
    const float keyGap = 5.f * scale;
    const float descGap = 13.f * scale;
    const float keyH = 22.f * scale;
    const float firstPad = 14.f * scale;
    const float bottomPad = 14.f * scale;
    const float step = 39.f * scale;

    // Measure exactly what will be painted. The longest rendered row determines
    // the panel width, then the same sidePad is added to both sides. This makes
    // the W key-cap's left edge -> panel edge distance equal to the final glyph
    // -> right panel edge distance, rather than relying on a guessed fixed width.
    float contentW = 0.f;
    for (const auto& row : rows) {
        float rowW = 0.f;
        for (size_t i = 0; i < row.keys.size(); ++i) {
  auto keyLayout = d2d->makeTextLayout(row.keys[i], 12.5f * scale);
  if (!keyLayout) continue;
  DWRITE_TEXT_METRICS km{};
  keyLayout->GetMetrics(&km);
  rowW += std::max(22.f * scale, km.width + 10.f * scale);
  if (i + 1 < row.keys.size()) rowW += keyGap;
        }
        auto descLayout = d2d->makeTextLayout(row.desc, 16.f * scale);
        if (descLayout) {
  DWRITE_TEXT_METRICS dm{};
  descLayout->GetMetrics(&dm);
  rowW += descGap + dm.width;
        }
        contentW = std::max(contentW, rowW);
    }

    const float panelW = sidePad + contentW + sidePad;
    const float panelH = firstPad + (rows.size() - 1) * step + keyH + bottomPad;

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    HMONITOR monitor = MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    RECT work{ win->x, win->y, win->x + (LONG)std::lround(win->w), win->y + (LONG)std::lround(win->h) };
    if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

    const float workLeft = (float)(work.left - win->x);
    const float workTop = (float)(work.top - win->y);
    const float workRight = (float)(work.right - win->x);
    const float workBottom = (float)(work.bottom - win->y);
    const float margin = 14.f * scale;
    float left = workLeft + margin;
    float top = workBottom - margin - panelH;
    left = std::clamp(left, workLeft, std::max(workLeft, workRight - panelW));
    top = std::clamp(top, workTop, std::max(workTop, workBottom - panelH));

    const auto panel = D2D1::RectF(left, top, left + panelW, top + panelH);
    ctx->FillRectangle(panel, brushHelpBg.Get());
    // No outer white stroke. Key-cap strokes remain.

    auto drawRow = [&](float rowY, const HelpRow& row) {
        float x = left + sidePad;
        for (size_t i = 0; i < row.keys.size(); ++i) {
  auto keyLayout = d2d->makeTextLayout(row.keys[i], 12.5f * scale);
  if (!keyLayout) continue;
  DWRITE_TEXT_METRICS km{};
  keyLayout->GetMetrics(&km);
  const float kw = std::max(22.f * scale, km.width + 10.f * scale);
  // Pure white, one-physical-pixel hard frame. Use four filled bars on
  // rounded device coordinates so the key cap has no antialiased/foggy edge.
  const float kl = std::round(x);
  const float kt = std::round(rowY);
  const float kr = std::round(x + kw);
  const float kb = std::round(rowY + keyH);
  constexpr float keyStroke = 1.f;
  const auto oldAA = ctx->GetAntialiasMode();
  ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  ctx->FillRectangle(D2D1::RectF(kl, kt, kr, kt + keyStroke), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kl, kb - keyStroke, kr, kb), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kl, kt + keyStroke, kl + keyStroke, kb - keyStroke), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kr - keyStroke, kt + keyStroke, kr, kb - keyStroke), brushKeyBorder.Get());
  ctx->SetAntialiasMode(oldAA);
  ctx->DrawTextLayout({ x + (kw - km.width) * .5f, rowY + 1.5f * scale },
      keyLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
  x += kw;
  if (i + 1 < row.keys.size()) x += keyGap;
        }
        auto descLayout = d2d->makeTextLayout(row.desc, 16.f * scale);
        if (descLayout) ctx->DrawTextLayout({ x + descGap, rowY - 0.5f * scale },
  descLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    };

    const float firstY = top + firstPad;
    for (size_t i = 0; i < rows.size(); ++i)
        drawRow(firstY + step * (float)i, rows[i]);
}
void CutMask::suppressLegacyMagnifier(ID2D1DeviceContext* ctx)
{
	auto* cap = static_cast<WinCap*>(win);
	if (!cap || !ctx) return;
	if (!legacyMagnifierSuppressed) {
		ComPtr<ID2D1SolidColorBrush> transparent;
		ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0.f), transparent.GetAddressOf());
		if (transparent) {
			cap->brushBg = transparent;
			cap->brushText = transparent;
			cap->crossBrush = transparent;
		}
		legacyMagnifierSuppressed = true;
	}
	cap->pixSrcRect = D2D1::RectF(0, 0, 0, 0);
}

void CutMask::paint(ID2D1DeviceContext* ctx)
{
	if (!ctx) return;
	if (!imeDisabled && win && win->hwnd) {
		ImmAssociateContext(win->hwnd, nullptr);
		imeDisabled = true;
	}
	suppressLegacyMagnifier(ctx);
	if (!initialDetectionDone) {
		initialDetectionDone = true;
		POINT p{};
		GetCursorPos(&p);
		ScreenToClient(win->hwnd, &p);
		cursorPos = p;
		auto* cap = static_cast<WinCap*>(win);
		if (cap && cap->stage == WinCap::CapStage::Select && !cap->isPress) {
			auto rect = detectRegionAt(p);
			if (validRect(rect)) {
				maskRect = rect;
				makeLayout();
			}
		}
	}
	if (!hasRect()) {
		paintMagnifier(ctx);
		paintHelp(ctx);
		return;
	}
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, win->w, maskRect.top), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.bottom, win->w, win->h), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.top, maskRect.left, maskRect.bottom), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(maskRect.right, maskRect.top, win->w, maskRect.bottom), brushBg.Get());
	if (strokeWidth > 0.f) {
		const auto half = strokeWidth / 2.f;
		ctx->DrawRectangle(D2D1::RectF(maskRect.left - half, maskRect.top - half,
			maskRect.right + half, maskRect.bottom + half), brushBorder.Get(), strokeWidth);
	}
	if (!hideLabel && layout) {
		ctx->FillRectangle(layoutRect, brushLabelBg.Get());
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		const float y = layoutRect.top + std::max(0.f, (layoutRect.bottom - layoutRect.top - tm.height) * .5f);
		ctx->DrawTextLayout({ layoutRect.left + 6.f * win->dpi, y },
			layout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
	paintHandles(ctx);
	paintMagnifier(ctx);
	paintHelp(ctx);
}

bool CutMask::copyCurrentColor()
{
	if (!win || !win->hwnd) return false;
	POINT p{};
	GetCursorPos(&p);
	ScreenToClient(win->hwnd, &p);
	if (p.x < 0 || p.y < 0 || p.x >= (int)std::lround(win->w) || p.y >= (int)std::lround(win->h))
		return false;
	// C only acts while the magnifier is actually available.
	if (hasRect() && !pointInRect(maskRect, p)) return false;
	cursorPos = p;
	Ling::Util::setTextToClipboard(colorText(sampleCapturedPixel(p)));
	return true;
}

void CutMask::moveCursorBy(int dx, int dy)
{
	POINT p{};
	GetCursorPos(&p);
	const LONG minX = (LONG)win->x, minY = (LONG)win->y;
	const LONG maxX = (LONG)win->x + (LONG)std::lround(win->w) - 1;
	const LONG maxY = (LONG)win->y + (LONG)std::lround(win->h) - 1;
	p.x = std::clamp<LONG>(p.x + dx, minX, maxX);
	p.y = std::clamp<LONG>(p.y + dy, minY, maxY);
	SetCursorPos(p.x, p.y);
}

void CutMask::useCurrentScreenOrFull()
{
	POINT p{};
	GetCursorPos(&p);
	ScreenToClient(win->hwnd, &p);
	if (!fullScreenToggle) {
		applyDetectedRect(monitorRectAt(p), true);
		fullScreenToggle = true;
	}
	else {
		applyDetectedRect(D2D1::RectF(0.f, 0.f, win->w, win->h), true);
		fullScreenToggle = false;
	}
}

void CutMask::restoreHistory(int direction)
{
	if (regionHistory.empty()) return;
	if (historyCursor > regionHistory.size()) historyCursor = regionHistory.size();
	if (direction < 0) {
		if (historyCursor == 0) return;
		--historyCursor;
	}
	else {
		if (historyCursor + 1 >= regionHistory.size()) return;
		++historyCursor;
	}
	applyDetectedRect(regionHistory[historyCursor], true);
}

void CutMask::rememberRegion()
{
	auto* cap = static_cast<WinCap*>(win);
	if (!cap || cap->stage == WinCap::CapStage::Select || !hasRect()) return;
	D2D1_RECT_F r = maskRect;
	r.left = std::clamp(r.left, 0.f, win->w);
	r.top = std::clamp(r.top, 0.f, win->h);
	r.right = std::clamp(r.right, 0.f, win->w);
	r.bottom = std::clamp(r.bottom, 0.f, win->h);
	if (!validRect(r)) return;
	if (!regionHistory.empty() && sameRectRounded(regionHistory.back(), r)) return;
	if (regionHistory.size() >= 20) regionHistory.erase(regionHistory.begin());
	regionHistory.push_back(r);
}

void CutMask::handleCaptureKey(UINT key)
{
	auto* cap = static_cast<WinCap*>(win);
	if (!cap || hideLabel) return;
	if (cap->stage != WinCap::CapStage::Select && cap->stage != WinCap::CapStage::Adjust) return;
	const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (!ctrl && !alt && key == 'C') {
		if (copyCurrentColor()) cap->close();
		return;
	}
	if (!ctrl && !alt && key == VK_SHIFT) {
		colorHex = !colorHex;
		win->refresh();
		return;
	}
	if (!ctrl && !alt && cap->stage == WinCap::CapStage::Select && !cap->isPress && key == VK_TAB) {
		detectUiElements = !detectUiElements;
		POINT p{};
		GetCursorPos(&p);
		ScreenToClient(win->hwnd, &p);
		applyDetectedRect(detectRegionAt(p), true);
		return;
	}
	if (ctrl && key == 'A') {
		useCurrentScreenOrFull();
		return;
	}
	if (!ctrl && !alt && key == 'R') {
		if (!regionHistory.empty()) {
			historyCursor = regionHistory.size() - 1;
			applyDetectedRect(regionHistory.back(), true);
		}
		return;
	}
	if (!ctrl && !alt && key == VK_OEM_COMMA) {
		restoreHistory(-1);
		return;
	}
	if (!ctrl && !alt && key == VK_OEM_PERIOD) {
		restoreHistory(1);
		return;
	}
	if (!ctrl && !alt && !shift) {
		if (key == 'W') moveCursorBy(0, -1);
		else if (key == 'S') moveCursorBy(0, 1);
		else if (key == 'A') moveCursorBy(-1, 0);
		else if (key == 'D') moveCursorBy(1, 0);
	}
}
