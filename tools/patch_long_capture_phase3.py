from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f'{label} marker not found')
    return text.replace(old, new, 1)

# Phase 3 replaces the repeatedly reallocated monolithic long-image buffer with a tail-trimmable
# chunk store. The full BGRA image is materialized only when an output operation actually needs it.
h_path = Path('Src/Win/CapLong.h')
h = h_path.read_text(encoding='utf-8-sig')

h = replace_once(
    h,
    '\tbool hasImage() const { return !imgData.empty(); }\n',
    '\tbool hasImage() const { return resultH > 0 && (!imageChunks.empty() || !imgData.empty()); }\n',
    'hasImage chunk store')

old_ocr = r'''\tbool ocr()
\t{
\t\tif (imgData.empty() || imgW <= 0 || resultH <= 0) return false;
\t\tstopCap();
\t\tauto data = imgData;
\t\tStarCapOcr::showPixels(std::move(data), imgW, resultH, true);
\t\treturn true;
\t}
\tbool translate()
\t{
\t\tif (imgData.empty() || imgW <= 0 || resultH <= 0) return false;
\t\tstopCap();
\t\tauto data = imgData;
\t\tStarCapOcr::showTranslationPixels(std::move(data), imgW, resultH, true);
\t\treturn true;
\t}
'''
new_ocr = r'''\tbool ocr()
\t{
\t\tif (!hasImage() || imgW <= 0 || resultH <= 0) return false;
\t\tstopCap();
\t\tif (!ensureMaterialized()) return false;
\t\tauto data = std::move(imgData);
\t\timageChunks.clear();
\t\tStarCapOcr::showPixels(std::move(data), imgW, resultH, true);
\t\treturn true;
\t}
\tbool translate()
\t{
\t\tif (!hasImage() || imgW <= 0 || resultH <= 0) return false;
\t\tstopCap();
\t\tif (!ensureMaterialized()) return false;
\t\tauto data = std::move(imgData);
\t\timageChunks.clear();
\t\tStarCapOcr::showTranslationPixels(std::move(data), imgW, resultH, true);
\t\treturn true;
\t}
'''
h = replace_once(h, old_ocr, new_ocr, 'OCR materialization')

h = replace_once(
    h,
    '''private:\n\tenum class AutoScrollStrategy\n''',
    '''private:\n\tstruct ImageChunk\n\t{\n\t\tint height{ 0 };\n\t\tstd::vector<BYTE> pixels;\n\t};\n\n\tenum class AutoScrollStrategy\n''',
    'ImageChunk declaration')

h = replace_once(
    h,
    '''\tvoid firstStep();\n\tvoid makeImgPreview();\n\tvoid capStep();\n''',
    '''\tvoid firstStep();\n\tvoid makeImgPreview();\n\tvoid appendChunkRows(const BYTE* src, int rows);\n\tvoid trimChunksToHeight(int keepHeight);\n\tconst BYTE* imageRowPtr(int y) const;\n\tbool ensureMaterialized();\n\tsize_t bufferedBytes() const;\n\tint storageHeightLimit() const;\n\tvoid capStep();\n''',
    'chunk helpers declarations')

h = replace_once(
    h,
    '''\tstd::vector<BYTE> imgData;\n\tstd::vector<BYTE> img1;\n''',
    '''\tstd::vector<BYTE> imgData; // Lazy full-image cache; normally empty while capture is active.\n\tstd::vector<ImageChunk> imageChunks;\n\tstd::vector<BYTE> img1;\n''',
    'chunk members')

h = replace_once(
    h,
    '''\tint resultH{ 0 };\n\tPOINT capStartPos{};\n''',
    '''\tint resultH{ 0 };\n\tint stitchCount{ 0 };\n\tint nextBufferLogHeight{ 10000 };\n\tbool storageLimitReached{ false };\n\tPOINT capStartPos{};\n''',
    'phase3 state')

h_path.write_text(h, encoding='utf-8-sig')

cpp_path = Path('Src/Win/CapLong.cpp')
cpp = cpp_path.read_text(encoding='utf-8-sig')

cpp = replace_once(
    cpp,
    '''    constexpr int manualPollMs = 120;\n''',
    '''    constexpr int manualPollMs = 120;\n    constexpr size_t maxLongBufferBytes = 384ull * 1024ull * 1024ull;\n    constexpr int maxLongResultHeight = 150000;\n    constexpr int maxPreviewHeightLogical = 360;\n''',
    'phase3 constants')

old_first = r'''void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    imgW = int(maskRect.right - maskRect.left);
    imgH = int(maskRect.bottom - maskRect.top);
    resultH = imgH;
    capStartPos.x = (int)maskRect.left;
    capStartPos.y = (int)maskRect.top;
    ClientToScreen(win->hwnd, &capStartPos);
    imgData = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    img1 = imgData;
    makeImgPreview();
    scheduleNextCapture(manualPollMs);
}
'''
new_first = r'''void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    imgW = int(maskRect.right - maskRect.left);
    imgH = int(maskRect.bottom - maskRect.top);
    resultH = imgH;
    capStartPos.x = (int)maskRect.left;
    capStartPos.y = (int)maskRect.top;
    ClientToScreen(win->hwnd, &capStartPos);
    auto firstFrame = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    if (firstFrame.empty()) return;
    img1 = firstFrame;
    imgData.clear();
    imageChunks.clear();
    appendChunkRows(firstFrame.data(), imgH);
    storageLimitReached = false;
    stitchCount = 0;
    nextBufferLogHeight = 10000;
    StarCapDiag::append(std::format(L"[long-v3] buffer-init width={} height={} bytes={} limitH={}",
        imgW, resultH, bufferedBytes(), storageHeightLimit()));
    makeImgPreview();
    scheduleNextCapture(manualPollMs);
}
'''
cpp = replace_once(cpp, old_first, new_first, 'firstStep chunk init')

preview_start = cpp.index('void CapLong::makeImgPreview()\n{')
preview_end = cpp.index('\nvoid CapLong::capStep()\n{', preview_start)
old_preview = cpp[preview_start:preview_end]
new_preview = r'''void CapLong::appendChunkRows(const BYTE* src, int rows)
{
    if (!src || rows <= 0 || imgW <= 0) return;
    const size_t rowBytes = (size_t)imgW * 4;
    ImageChunk chunk;
    chunk.height = rows;
    chunk.pixels.resize(rowBytes * (size_t)rows);
    CopyMemory(chunk.pixels.data(), src, chunk.pixels.size());
    imageChunks.push_back(std::move(chunk));
    imgData.clear();
}

void CapLong::trimChunksToHeight(int keepHeight)
{
    keepHeight = std::max(0, keepHeight);
    const size_t rowBytes = (size_t)imgW * 4;
    int total = 0;
    for (const auto& chunk : imageChunks) total += chunk.height;

    while (!imageChunks.empty() && total > keepHeight) {
        auto& tail = imageChunks.back();
        int beforeTail = total - tail.height;
        if (keepHeight <= beforeTail) {
            total = beforeTail;
            imageChunks.pop_back();
            continue;
        }
        int keepRows = keepHeight - beforeTail;
        tail.height = keepRows;
        tail.pixels.resize(rowBytes * (size_t)keepRows);
        total = keepHeight;
    }
    imgData.clear();
}

const BYTE* CapLong::imageRowPtr(int y) const
{
    if (y < 0 || y >= resultH || imgW <= 0) return nullptr;
    const size_t rowBytes = (size_t)imgW * 4;
    const size_t expected = rowBytes * (size_t)resultH;
    if (imgData.size() == expected) return imgData.data() + rowBytes * (size_t)y;

    int baseY = 0;
    for (const auto& chunk : imageChunks) {
        if (y < baseY + chunk.height) {
            return chunk.pixels.data() + rowBytes * (size_t)(y - baseY);
        }
        baseY += chunk.height;
    }
    return nullptr;
}

size_t CapLong::bufferedBytes() const
{
    size_t total = 0;
    for (const auto& chunk : imageChunks) total += chunk.pixels.size();
    return total;
}

int CapLong::storageHeightLimit() const
{
    if (imgW <= 0) return 0;
    const size_t rowBytes = (size_t)imgW * 4;
    if (rowBytes == 0) return 0;
    const size_t byMemory = maxLongBufferBytes / rowBytes;
    return (int)std::min<size_t>((size_t)maxLongResultHeight, byMemory);
}

bool CapLong::ensureMaterialized()
{
    if (imgW <= 0 || resultH <= 0) return false;
    const size_t rowBytes = (size_t)imgW * 4;
    const size_t expected = rowBytes * (size_t)resultH;
    if (imgData.size() == expected) return true;

    int chunkRows = 0;
    for (const auto& chunk : imageChunks) chunkRows += chunk.height;
    if (chunkRows != resultH) {
        StarCapDiag::append(std::format(L"[long-v3] materialize-invalid chunkRows={} resultH={} chunks={}",
            chunkRows, resultH, imageChunks.size()));
        return false;
    }

    const ULONGLONG started = GetTickCount64();
    std::vector<BYTE> full;
    try {
        full.resize(expected);
    }
    catch (const std::bad_alloc&) {
        StarCapDiag::append(std::format(L"[long-v3] materialize-oom bytes={} height={}", expected, resultH));
        return false;
    }

    size_t offset = 0;
    for (const auto& chunk : imageChunks) {
        if (!chunk.pixels.empty()) {
            CopyMemory(full.data() + offset, chunk.pixels.data(), chunk.pixels.size());
            offset += chunk.pixels.size();
        }
    }
    if (offset != expected) return false;
    imgData = std::move(full);
    StarCapDiag::append(std::format(L"[long-v3] materialized height={} bytes={} chunks={} ms={}",
        resultH, expected, imageChunks.size(), GetTickCount64() - started));
    return true;
}

void CapLong::makeImgPreview()
{
    imgPreview.Reset();
    if (imgW <= 0 || resultH <= 0 || !hasImage()) return;
    float previewScaleW = tool ? (float)tool->w / (float)imgW : 1.0f;
    if (previewScaleW <= 0.f) return;
    int previewW = std::max(1, (int)((float)imgW * previewScaleW));
    int naturalPreviewH = std::max(1, (int)((float)resultH * previewScaleW));
    int maxPreviewH = std::max(120, (int)(maxPreviewHeightLogical * win->dpi));
    int previewH = std::min(naturalPreviewH, maxPreviewH);
    int sourceRows = std::min(resultH, std::max(1, (int)ceil((double)previewH / previewScaleW)));
    int sourceStartY = resultH - sourceRows;

    std::vector<BYTE> scaledData((size_t)previewW * 4 * previewH);
    for (int y = 0; y < previewH; y++) {
        int srcY = sourceStartY + (int)((double)y * sourceRows / std::max(1, previewH));
        srcY = std::clamp(srcY, sourceStartY, resultH - 1);
        const BYTE* srcRow = imageRowPtr(srcY);
        if (!srcRow) return;
        for (int x = 0; x < previewW; x++) {
            int srcX = (int)((float)x / previewScaleW);
            srcX = std::clamp(srcX, 0, imgW - 1);
            int srcIdx = srcX * 4;
            int dstIdx = (y * previewW + x) * 4;
            scaledData[dstIdx] = srcRow[srcIdx];
            scaledData[dstIdx + 1] = srcRow[srcIdx + 1];
            scaledData[dstIdx + 2] = srcRow[srcIdx + 2];
            scaledData[dstIdx + 3] = srcRow[srcIdx + 3];
        }
    }
    D2D1_BITMAP_PROPERTIES1 props = {
        .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
        .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(previewW, previewH), scaledData.data(), previewW * 4, props, imgPreview.GetAddressOf());
}
'''
cpp = cpp[:preview_start] + new_preview + cpp[preview_end:]

old_buffer = r'''    int addedH = newResultH - resultH;
    std::vector<BYTE> newResult((size_t)rowPix * newResultH);
    CopyMemory(newResult.data(), imgData.data(), imgData.size());
    for (int row = 0; row < imgH - changeStartY; row++) {
        CopyMemory(newResult.data() + (size_t)(paintStart + row) * rowPix,
            data.data() + (size_t)(changeStartY + row) * rowPix, rowPix);
    }
    imgData = std::move(newResult);
    img1 = std::move(data);
    resultH = newResultH;
    dismissTime = 0;
'''
new_buffer = r'''    const int oldResultH = resultH;
    const int heightLimit = storageHeightLimit();
    int finalResultH = newResultH;
    if (heightLimit > 0 && finalResultH > heightLimit) {
        finalResultH = heightLimit;
        storageLimitReached = true;
    }
    int rowsToWrite = finalResultH - paintStart;
    if (rowsToWrite <= 0) {
        storageLimitReached = true;
        stopCap();
        return;
    }

    // Preserve the exact Phase 2 seam semantics without copying the entire historical image:
    // remove only the suffix that the old monolithic implementation would overwrite, then store
    // the replacement/new rows as one new chunk.
    trimChunksToHeight(paintStart);
    appendChunkRows(data.data() + (size_t)changeStartY * rowPix, rowsToWrite);
    resultH = finalResultH;
    int addedH = resultH - oldResultH;
    img1 = std::move(data);
    dismissTime = 0;
    stitchCount++;

    if (resultH >= nextBufferLogHeight || storageLimitReached) {
        StarCapDiag::append(std::format(L"[long-v3] buffer height={} chunks={} bytes={} stitches={} limitH={}",
            resultH, imageChunks.size(), bufferedBytes(), stitchCount, heightLimit));
        while (nextBufferLogHeight <= resultH) nextBufferLogHeight += 10000;
    }
'''
cpp = replace_once(cpp, old_buffer, new_buffer, 'chunked process buffer')

cpp = replace_once(
    cpp,
    '''    if (resultH > 36000) { stopCap(); return; }\n    makeImgPreview();\n''',
    '''    makeImgPreview();\n    if (storageLimitReached) {\n        win->refresh();\n        stopCap();\n        return;\n    }\n''',
    'dynamic long limit')

cpp = replace_once(
    cpp,
    '''    if (resultH > 36000) {\n        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);\n''',
    '''    if (storageLimitReached) {\n        layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);\n''',
    'limit message state')

old_outputs = r'''void CapLong::copyToClipboard()
{
    if (imgData.empty()) return;
    if (!isFinish) stopCap();
    Util::saveToClipboard(imgW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (imgData.empty()) return false;
    if (!isFinish) stopCap();
    auto path = Util::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    return Util::saveToFile(path, imgW, resultH, imgData.data());
}

void CapLong::pin()
{
    if (imgData.empty()) return;
    if (!isFinish) stopCap();
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
'''
new_outputs = r'''void CapLong::copyToClipboard()
{
    if (!hasImage()) return;
    if (!isFinish) stopCap();
    if (!ensureMaterialized()) return;
    Util::saveToClipboard(imgW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (!hasImage()) return false;
    if (!isFinish) stopCap();
    auto path = Util::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    if (!ensureMaterialized()) return false;
    return Util::saveToFile(path, imgW, resultH, imgData.data());
}

void CapLong::pin()
{
    if (!hasImage()) return;
    if (!isFinish) stopCap();
    if (!ensureMaterialized()) return;
    auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
'''
cpp = replace_once(cpp, old_outputs, new_outputs, 'lazy output materialization')

cpp_path.write_text(cpp, encoding='utf-8-sig')
print('Long Capture V2 Phase 3 chunk-buffer patch applied.')
