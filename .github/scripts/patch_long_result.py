from pathlib import Path
import re


def read(path):
    return Path(path).read_text(encoding="utf-8-sig")


def write(path, text):
    Path(path).write_text(text, encoding="utf-8")


def replace_once(path, old, new, label):
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    write(path, text.replace(old, new, 1))
    print("patched", label)


def regex_once(path, pattern, repl, label):
    text = read(path)
    new_text, count = re.subn(pattern, repl, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{label}: expected one regex match, got {count}")
    write(path, new_text)
    print("patched", label)


# CapLong public/private API for completed-result crop + annotation.
path = "Src/Win/CapLong.h"
replace_once(path,
    """    bool ocr();
    bool translate();

    bool hasImage() const { return resultH > 0 && imgW > 0; }
    void layoutTool();
""",
    """    bool ocr();
    bool translate();
    bool mark();
    bool enterResultAdjust();

    bool hasImage() const { return resultH > 0 && imgW > 0; }
    bool isResultEditing() const { return resultEditing; }
    void layoutTool();
""",
    "CapLong public result editing API")
replace_once(path,
    """    bool isAutoScrolling() const { return autoScroll; }
    bool canResizeSelection() const { return isCapturing && !isFinish && !autoScroll && acceptedFrames == 0; }
""",
    """    bool isAutoScrolling() const { return autoScroll; }
    bool canResizeSelection() const {
        return resultEditing || (isCapturing && !isFinish && !autoScroll && acceptedFrames == 0);
    }
""",
    "CapLong resize policy")
replace_once(path,
    """    void makeTool();
    void makeImgPreview();
    void paintImgPreview(ID2D1DeviceContext* ctx);
""",
    """    void makeTool();
    void makeImgPreview();
    void makeResultEditPreview();
    void paintImgPreview(ID2D1DeviceContext* ctx);
    void clampResultEditRect();
    bool applyResultCrop();
""",
    "CapLong result crop helpers")
replace_once(path,
    """    bool storageLimitReached{ false };
    bool resizingSelection{ false };
""",
    """    bool storageLimitReached{ false };
    bool resizingSelection{ false };
    bool resultEditing{ false };
""",
    "CapLong result editing state")
replace_once(path,
    """    D2D1_RECT_F stopTextRect{};
    D2D1_POINT_2F stopTextPos{};
""",
    """    D2D1_RECT_F stopTextRect{};
    D2D1_POINT_2F stopTextPos{};
    D2D1_RECT_F resultPreviewRect{};
""",
    "CapLong result preview rect")

# Long toolbar: add crop/adjust and image-mark entry points.
path = "Src/Tool/ToolLong.h"
replace_once(path,
    """    std::vector<std::wstring> btnIds = { L\"auto\",L\"ocr\",L\"translate\",L\"pin\",L\"close\",L\"save\",L\"clipboard\" };
    std::vector<std::wstring> btnCodes = { L\"▶\",L\"\\ue67b\",L\"译\",L\"\\ue6a2\",L\"\\ue62d\",L\"\\ue608\",L\"\\ue6ad\" };
""",
    """    std::vector<std::wstring> btnIds = { L\"auto\",L\"crop\",L\"mark\",L\"ocr\",L\"translate\",L\"pin\",L\"close\",L\"save\",L\"clipboard\" };
    std::vector<std::wstring> btnCodes = { L\"▶\",L\"裁\",L\"\\ue97f\",L\"\\ue67b\",L\"译\",L\"\\ue6a2\",L\"\\ue62d\",L\"\\ue608\",L\"\\ue6ad\" };
""",
    "ToolLong buttons")

path = "Src/Tool/ToolLong.cpp"
replace_once(path,
    """        if (btnIds[i] == L\"auto\" || btnIds[i] == L\"translate\") {
""",
    """        if (btnIds[i] == L\"auto\" || btnIds[i] == L\"translate\" || btnIds[i] == L\"crop\") {
""",
    "ToolLong text buttons")
replace_once(path,
    """    if (btn->id == L\"ocr\") {
""",
    """    if (btn->id == L\"crop\") {
        if (capLong) capLong->enterResultAdjust();
        return;
    }
    if (btn->id == L\"mark\") {
        if (!capLong || !capLong->mark()) return;
        win->close();
        return;
    }
    if (btn->id == L\"ocr\") {
""",
    "ToolLong crop and mark actions")

# Full-screen capture host: draw long result first, then CutMask handles above it.
path = "Src/Win/WinCap.cpp"
regex_once(path,
    r"void WinCap::layout\(\)\n\{.*?\n\}\n\nBOOL WinCap::setCursor\(\)",
    """void WinCap::layout()
{
    Ling::WinBase::layout();
    if (!canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)w, (float)h);
    if (!hideScreenImg) {
        ctx->DrawBitmap(screenImg.Get(), destRect);
    }
    if (capLong && capLong->isResultEditing()) {
        capLong->paint(ctx);
        cutMask->paint(ctx);
    }
    else {
        cutMask->paint(ctx);
        if (capLong) capLong->paint(ctx);
    }
    paintPix(ctx);
    canvas->finishPaint();
}

BOOL WinCap::setCursor()""",
    "WinCap long-result paint order")

# Auto-fit oversized data-backed annotation windows.
path = "Src/Win/WinPin.cpp"
regex_once(path,
    r"void WinPin::initEditorFromData\(int x, int y, int w, int h, std::vector<BYTE>& data\)\n\{.*?\n\}\n\nvoid WinPin::initFromData",
    """void WinPin::initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data)
{
    ToolMain::queueEditorOpen();
    auto ptr = new WinPin(x, y, w, h, &data, false);
    std::unique_ptr<WinPin> winPin{ ptr };
    ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);

    RECT sourceRect{ x, y, x + w, y + h };
    MONITORINFO mi{ sizeof(MONITORINFO) };
    HMONITOR mon = MonitorFromRect(&sourceRect, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfo(mon, &mi)) {
        const int workW = mi.rcWork.right - mi.rcWork.left;
        const int workH = mi.rcWork.bottom - mi.rcWork.top;
        const float fit = std::min(1.f, std::min(
            (workW * 0.82f) / std::max(1, w),
            (workH * 0.78f) / std::max(1, h)));
        if (fit < 0.999f) {
            ptr->scale = std::max(0.1f, fit);
            ptr->applyWinSize();
            const int drawW = static_cast<int>(std::lround(w * ptr->scale));
            const int drawH = static_cast<int>(std::lround(h * ptr->scale));
            ptr->setPosition(mi.rcWork.left + (workW - drawW) / 2,
                mi.rcWork.top + (workH - drawH) / 2);
            ptr->layoutTools();
            ptr->refresh();
        }
    }
    winPins.push_back(std::move(winPin));
}

void WinPin::initFromData""",
    "WinPin long editor auto-fit")

# CapLong completed-result crop flow.
path = "Src/Win/CapLong.cpp"
text = read(path)
marker = "void CapLong::makeImgPreview()\n"
if text.count(marker) != 1:
    raise SystemExit("CapLong result helper insertion marker mismatch")
helpers = r'''bool CapLong::enterResultAdjust()
{
    if (!ensureMaterialized()) return false;
    if (!isFinish) stopCap(false);
    resultEditing = true;
    win->hideScreenImg = true;
    win->cutMask->hideLabel = false;
    makeResultEditPreview();
    if (!imgPreview) {
        resultEditing = false;
        return false;
    }
    win->cutMask->maskRect = resultPreviewRect;
    layoutTool();
    win->refresh();
    StarCapDiag::append(std::format(L"[long-next] result-adjust-enter image={}x{} preview=[{:.0f},{:.0f},{:.0f},{:.0f}]",
        imgW, resultH, resultPreviewRect.left, resultPreviewRect.top,
        resultPreviewRect.right, resultPreviewRect.bottom));
    return true;
}

void CapLong::makeResultEditPreview()
{
    imgPreview.Reset();
    if (imgData.empty() || imgW <= 0 || resultH <= 0) return;

    const float margin = std::max(18.f, 28.f * win->dpi);
    const float maxW = std::max(120.f, win->w - margin * 2.f);
    const float maxH = std::max(120.f, win->h - margin * 2.f - 54.f * win->dpi);
    const float scale = std::min(1.f, std::min(maxW / imgW, maxH / resultH));
    const int previewW = std::max(1, static_cast<int>(std::lround(imgW * scale)));
    const int previewH = std::max(1, static_cast<int>(std::lround(resultH * scale)));

    std::vector<BYTE> scaled(static_cast<size_t>(previewW) * previewH * 4);
    for (int y = 0; y < previewH; ++y) {
        const int srcY = std::min(resultH - 1,
            static_cast<int>((static_cast<long long>(y) * resultH) / previewH));
        for (int x = 0; x < previewW; ++x) {
            const int srcX = std::min(imgW - 1,
                static_cast<int>((static_cast<long long>(x) * imgW) / previewW));
            const size_t si = (static_cast<size_t>(srcY) * imgW + srcX) * 4;
            const size_t di = (static_cast<size_t>(y) * previewW + x) * 4;
            scaled[di] = imgData[si];
            scaled[di + 1] = imgData[si + 1];
            scaled[di + 2] = imgData[si + 2];
            scaled[di + 3] = imgData[si + 3];
        }
    }

    D2D1_BITMAP_PROPERTIES1 props = {
        .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
        .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(previewW, previewH),
        scaled.data(), previewW * 4, props, imgPreview.GetAddressOf());

    const float left = std::max(margin, (win->w - previewW) * .5f);
    const float top = std::max(margin, (win->h - previewH - 44.f * win->dpi) * .5f);
    resultPreviewRect = D2D1::RectF(left, top, left + previewW, top + previewH);
}

void CapLong::clampResultEditRect()
{
    if (!resultEditing) return;
    auto& r = win->cutMask->maskRect;
    r.left = std::clamp(r.left, resultPreviewRect.left, resultPreviewRect.right - 4.f);
    r.top = std::clamp(r.top, resultPreviewRect.top, resultPreviewRect.bottom - 4.f);
    r.right = std::clamp(r.right, r.left + 4.f, resultPreviewRect.right);
    r.bottom = std::clamp(r.bottom, r.top + 4.f, resultPreviewRect.bottom);
}

bool CapLong::applyResultCrop()
{
    if (!resultEditing) return true;
    if (imgData.empty() || imgW <= 0 || resultH <= 0) return false;
    clampResultEditRect();
    const auto& r = win->cutMask->maskRect;
    const float pw = resultPreviewRect.right - resultPreviewRect.left;
    const float ph = resultPreviewRect.bottom - resultPreviewRect.top;
    if (pw <= 0.f || ph <= 0.f) return false;

    auto mapX = [&](float x) { return (x - resultPreviewRect.left) * imgW / pw; };
    auto mapY = [&](float y) { return (y - resultPreviewRect.top) * resultH / ph; };
    int left = std::clamp(static_cast<int>(std::floor(mapX(r.left))), 0, imgW - 1);
    int top = std::clamp(static_cast<int>(std::floor(mapY(r.top))), 0, resultH - 1);
    int right = std::clamp(static_cast<int>(std::ceil(mapX(r.right))), left + 1, imgW);
    int bottom = std::clamp(static_cast<int>(std::ceil(mapY(r.bottom))), top + 1, resultH);

    const int oldW = imgW, oldH = resultH;
    const int newW = right - left, newH = bottom - top;
    if (left != 0 || top != 0 || right != oldW || bottom != oldH) {
        std::vector<BYTE> cropped(static_cast<size_t>(newW) * newH * 4);
        const size_t srcRow = static_cast<size_t>(oldW) * 4;
        const size_t dstRow = static_cast<size_t>(newW) * 4;
        for (int y = 0; y < newH; ++y) {
            const BYTE* src = imgData.data() + static_cast<size_t>(top + y) * srcRow + static_cast<size_t>(left) * 4;
            CopyMemory(cropped.data() + static_cast<size_t>(y) * dstRow, src, dstRow);
        }
        imgData = std::move(cropped);
        imgW = newW;
        resultH = newH;
        materializedDirty = false;
        StarCapDiag::append(std::format(L"[long-next] result-crop {}x{} -> {}x{} rect=[{},{},{},{}]",
            oldW, oldH, newW, newH, left, top, right, bottom));
    }
    resultEditing = false;
    return true;
}

'''
write(path, text.replace(marker, helpers + marker, 1))
print("patched CapLong result helper methods")

# Result preview paints as centered full-image view while editing.
regex_once(path,
    r"void CapLong::paintImgPreview\(ID2D1DeviceContext\* ctx\)\n\{.*?\n\}\n\nvoid CapLong::setState",
    """void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
    if (!imgPreview) return;
    if (resultEditing) {
        ctx->DrawBitmap(imgPreview.Get(), resultPreviewRect);
        return;
    }
    if (!tool) return;
    auto bitmapSize = imgPreview->GetPixelSize();
    float drawW = static_cast<float>(bitmapSize.width);
    float drawH = static_cast<float>(bitmapSize.height);
    POINT pos{ tool->x, tool->y - static_cast<int>(drawH) - static_cast<int>(2 * win->dpi) };
    ScreenToClient(win->hwnd, &pos);
    if (pos.y < 0) pos.y = 0;
    D2D1_RECT_F destRect = D2D1::RectF(static_cast<float>(pos.x), static_cast<float>(pos.y), pos.x + drawW, pos.y + drawH);
    ctx->DrawBitmap(imgPreview.Get(), destRect);
}

void CapLong::setState""",
    "CapLong result preview paint")

# Mouse resize: capture-source before capture, result crop after completion.
regex_once(path,
    r"void CapLong::onDown\(POINT pos\)\n\{.*?\n\}\n\nvoid CapLong::onMove\(POINT pos\)\n\{.*?\n\}\n\nvoid CapLong::onUp\(POINT pos\)\n\{.*?\n\}\n\nvoid CapLong::scheduleFrameCapture",
    """void CapLong::onDown(POINT pos)
{
    if (!canResizeSelection()) return;
    auto hit = win->cutMask->hitTest(pos);
    if (hit == MaskHit::None || hit == MaskHit::Inside) return;

    resizingSelection = true;
    win->isPress = true;
    SetCapture(win->hwnd);
    win->cutMask->startAdjust(pos);
    if (tool) tool->hide();

    if (resultEditing) {
        StarCapDiag::append(std::format(L"[long-next] result-crop-resize-begin hit={}", static_cast<int>(hit)));
        return;
    }

    win->killTimer(frameCaptureTimerId);
    win->killTimer(autoScrollTimerId);
    win->restoreWin();
    StarCapDiag::append(std::format(L"[long-next] resize-begin hit={}", static_cast<int>(hit)));
    win->refresh();
}

void CapLong::onMove(POINT pos)
{
    if (!resizingSelection) return;
    win->cutMask->adjust(pos);
    if (resultEditing) clampResultEditRect();
}

void CapLong::onUp(POINT pos)
{
    if (!resizingSelection) return;
    win->cutMask->adjust(pos);
    if (resultEditing) clampResultEditRect();
    resizingSelection = false;
    win->isPress = false;
    if (GetCapture() == win->hwnd) ReleaseCapture();

    if (resultEditing) {
        if (tool) {
            layoutTool();
            tool->show();
        }
        StarCapDiag::append(L"[long-next] result-crop-resize-end");
        win->refresh();
        return;
    }

    win->hollowWin();
    restartForCurrentRect();
    if (tool) {
        layoutTool();
        tool->show();
    }
    StarCapDiag::append(std::format(L"[long-next] resize-end rect={}x{}", imgW, imgH));
    win->refresh();
}

void CapLong::scheduleFrameCapture""",
    "CapLong result crop mouse handling")

# All output actions consume crop first; Mark opens existing WinPin annotation engine.
regex_once(path,
    r"void CapLong::copyToClipboard\(\)\n\{.*?\n\}\n\nbool CapLong::saveToFile\(\)\n\{.*?\n\}\n\nbool CapLong::ocr\(\)\n\{.*?\n\}\n\nbool CapLong::translate\(\)\n\{.*?\n\}\n\nvoid CapLong::pin\(\)\n\{.*?\n\}\s*$",
    """void CapLong::copyToClipboard()
{
    if (!ensureMaterialized()) return;
    if (resultEditing && !applyResultCrop()) return;
    if (!isFinish) stopCap(false);
    Util::saveToClipboard(imgW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (!ensureMaterialized()) return false;
    if (resultEditing && !applyResultCrop()) return false;
    if (!isFinish) stopCap(false);
    auto path = Util::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    return Util::saveToFile(path, imgW, resultH, imgData.data());
}

bool CapLong::ocr()
{
    if (!ensureMaterialized()) return false;
    if (resultEditing && !applyResultCrop()) return false;
    if (!isFinish) stopCap(false);
    auto data = imgData;
    StarCapOcr::showPixels(std::move(data), imgW, resultH, true);
    return true;
}

bool CapLong::translate()
{
    if (!ensureMaterialized()) return false;
    if (resultEditing && !applyResultCrop()) return false;
    if (!isFinish) stopCap(false);
    auto data = imgData;
    StarCapOcr::showTranslationPixels(std::move(data), imgW, resultH, true);
    return true;
}

bool CapLong::mark()
{
    if (!ensureMaterialized()) return false;
    if (resultEditing && !applyResultCrop()) return false;
    if (!isFinish) stopCap(false);
    auto monitor = MonitorFromPoint({ win->x, win->y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (!GetMonitorInfo(monitor, &mi)) return false;
    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;
    const int posX = mi.rcWork.left + (workW - std::min(imgW, workW)) / 2;
    const int posY = mi.rcWork.top + (workH - std::min(resultH, workH)) / 2;
    WinPin::initEditorFromData(posX, posY, imgW, resultH, imgData);
    return true;
}

void CapLong::pin()
{
    if (!ensureMaterialized()) return;
    if (resultEditing && !applyResultCrop()) return;
    if (!isFinish) stopCap(false);
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(monitor, &mi);
    auto& workArea = mi.rcWork;
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;
    int posX = workArea.left + (screenW - imgW) / 2;
    int posY = workArea.top + (screenH - std::min(resultH, screenH)) / 2;
    WinPin::initFromData(posX, posY, imgW, resultH, imgData);
}
""",
    "CapLong cropped output and mark action")

print("all source patches applied")
