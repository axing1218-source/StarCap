from pathlib import Path

root = Path(__file__).resolve().parents[1]
cap_path = root / 'Src/Win/CapLong.cpp'
mask_path = root / 'Src/Win/CutMask.cpp'

cap = cap_path.read_text(encoding='utf-8')
mask = mask_path.read_text(encoding='utf-8')

# 1) Motion-aware matcher: static wallpaper must not dominate seam scoring.
start = cap.index('    double featureScore(')
end = cap.index('    LRESULT CALLBACK longCaptureKeyboardProc', start)
new_match_helpers = r'''    std::vector<float> makeMotionWeights(const std::vector<float>& oldFeatures,
        const std::vector<float>& newFeatures, int height)
    {
        const int dims = featureBins * 2;
        std::vector<float> weights(static_cast<size_t>(height) * featureBins, 0.0f);
        if (oldFeatures.size() != newFeatures.size() ||
            oldFeatures.size() < static_cast<size_t>(height) * dims) return weights;

        std::vector<float> raw(weights.size(), 0.0f);
        for (int y = 0; y < height; ++y) {
            for (int bin = 0; bin < featureBins; ++bin) {
                size_t base = static_cast<size_t>(y) * dims + bin * 2;
                double grayDelta = std::abs(static_cast<double>(oldFeatures[base]) - newFeatures[base]);
                double edgeDelta = std::abs(static_cast<double>(oldFeatures[base + 1]) - newFeatures[base + 1]);
                double signal = grayDelta * 0.75 + edgeDelta * 1.45;
                raw[static_cast<size_t>(y) * featureBins + bin] =
                    static_cast<float>(std::clamp((signal - 1.25) / 13.0, 0.0, 1.0));
            }
        }

        // Dilate a little so text/bubble edges carry their nearby pixels with them,
        // but do not let a large stationary wallpaper take over the score again.
        for (int y = 0; y < height; ++y) {
            for (int bin = 0; bin < featureBins; ++bin) {
                float best = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= height) continue;
                    for (int db = -1; db <= 1; ++db) {
                        int bb = bin + db;
                        if (bb < 0 || bb >= featureBins) continue;
                        best = std::max(best, raw[static_cast<size_t>(yy) * featureBins + bb]);
                    }
                }
                weights[static_cast<size_t>(y) * featureBins + bin] = best;
            }
        }
        return weights;
    }

    double motionCoverage(const std::vector<float>& weights, int height, int bodyTop, int bodyBottom)
    {
        if (weights.size() < static_cast<size_t>(height) * featureBins || bodyBottom <= bodyTop) return 0.0;
        size_t moving = 0;
        size_t total = 0;
        for (int y = bodyTop; y < bodyBottom; y += 3) {
            for (int bin = 0; bin < featureBins; ++bin) {
                if (weights[static_cast<size_t>(y) * featureBins + bin] >= 0.18f) ++moving;
                ++total;
            }
        }
        return total ? static_cast<double>(moving) / total : 0.0;
    }

    double featureScore(const std::vector<float>& oldFeatures, const std::vector<float>& newFeatures,
        const std::vector<float>& motionWeights, int height, int offset, int bodyTop, int bodyBottom)
    {
        const int dims = featureBins * 2;
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 1.0;
        const bool motionAware = motionWeights.size() >= static_cast<size_t>(height) * featureBins;
        double sum = 0.0;
        double weightSum = 0.0;
        for (int y = bodyTop; y < end; y += 3) {
            const float* a = oldFeatures.data() + static_cast<size_t>(y + offset) * dims;
            const float* b = newFeatures.data() + static_cast<size_t>(y) * dims;
            for (int bin = 0; bin < featureBins; ++bin) {
                double cellWeight = 1.0;
                if (motionAware) {
                    float w0 = motionWeights[static_cast<size_t>(y) * featureBins + bin];
                    float w1 = motionWeights[static_cast<size_t>(std::min(height - 1, y + offset)) * featureBins + bin];
                    // Keep a small floor so extremely sparse chats still retain global context.
                    cellWeight = 0.07 + 0.93 * std::max(w0, w1);
                }
                for (int channel = 0; channel < 2; ++channel) {
                    int d = bin * 2 + channel;
                    double semanticWeight = channel ? 1.35 : 1.0;
                    double w = cellWeight * semanticWeight;
                    sum += std::abs(static_cast<double>(a[d]) - static_cast<double>(b[d])) * w;
                    weightSum += w;
                }
            }
        }
        if (weightSum <= 0.0) return 1.0;
        return sum / (weightSum * 255.0);
    }

    double pixelMadAtOffset(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        const std::vector<float>& motionWeights, int width, int height, int offset, int bodyTop, int bodyBottom)
    {
        int margin = std::max(8, width / 20);
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 255.0;
        const bool motionAware = motionWeights.size() >= static_cast<size_t>(height) * featureBins;
        double sum = 0.0;
        double weightSum = 0.0;
        int xStep = std::max(5, width / 100);
        for (int y = bodyTop; y < end; y += 7) {
            int oldY = y + offset;
            for (int x = margin; x < width - margin; x += xStep) {
                double w = 1.0;
                if (motionAware) {
                    int bin = std::clamp(x * featureBins / std::max(1, width), 0, featureBins - 1);
                    float w0 = motionWeights[static_cast<size_t>(y) * featureBins + bin];
                    float w1 = motionWeights[static_cast<size_t>(std::min(height - 1, oldY)) * featureBins + bin];
                    w = 0.07 + 0.93 * std::max(w0, w1);
                }
                size_t ia = (static_cast<size_t>(oldY) * width + x) * 4;
                size_t ib = (static_cast<size_t>(y) * width + x) * 4;
                sum += std::abs(static_cast<int>(oldFrame[ia]) - static_cast<int>(newFrame[ib])) * w;
                sum += std::abs(static_cast<int>(oldFrame[ia + 1]) - static_cast<int>(newFrame[ib + 1])) * w;
                sum += std::abs(static_cast<int>(oldFrame[ia + 2]) - static_cast<int>(newFrame[ib + 2])) * w;
                weightSum += 3.0 * w;
            }
        }
        return weightSum > 0.0 ? sum / weightSum : 255.0;
    }

'''
cap = cap[:start] + new_match_helpers + cap[end:]

cap = cap.replace('    if (sameRatio >= 0.988) {', '    if (sameRatio >= 0.996) {', 1)

old = '''    auto oldFeatures = makeRowFeatures(oldFrame, imgW, imgH);\n    auto newFeatures = makeRowFeatures(newFrame, imgW, imgH);\n\n    struct Candidate {\n'''
new = '''    auto oldFeatures = makeRowFeatures(oldFrame, imgW, imgH);\n    auto newFeatures = makeRowFeatures(newFrame, imgW, imgH);\n    auto motionWeights = makeMotionWeights(oldFeatures, newFeatures, imgH);\n    double movingCoverage = motionCoverage(motionWeights, imgH, bodyTop, bodyBottom);\n    const bool motionAware = movingCoverage >= 0.030;\n    const std::vector<float> noMotionWeights;\n    const auto& scoreWeights = motionAware ? motionWeights : noMotionWeights;\n\n    struct Candidate {\n'''
if old not in cap:
    raise SystemExit('matchFrame feature anchor changed')
cap = cap.replace(old, new, 1)

cap = cap.replace(
    'double raw = featureScore(oldFeatures, newFeatures, imgH, offset, bodyTop, bodyBottom);',
    'double raw = featureScore(oldFeatures, newFeatures, scoreWeights, imgH, offset, bodyTop, bodyBottom);', 1)
cap = cap.replace(
    'result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, imgW, imgH, best.offset, bodyTop, bodyBottom);',
    'result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, scoreWeights, imgW, imgH, best.offset, bodyTop, bodyBottom);', 1)

old = '''        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n'''
new = '''        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} motion={:.4f}/{} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio, movingCoverage, motionAware ? 1 : 0,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n'''
if old not in cap:
    raise SystemExit('match log anchor changed')
cap = cap.replace(old, new, 1)

# 2) Layer-aware append. Keep moving chat foreground untouched; only very-stationary
# background blocks get a continuous viewport phase instead of repeating the same bottom strip.
start = cap.index('void CapLong::appendBodyRows(')
end = cap.index('void CapLong::updateFooter(', start)
new_append = r'''void CapLong::appendBodyRows(const std::vector<BYTE>& frame, int sourceY, int rows)
{
    if (rows <= 0 || sourceY < 0 || sourceY + rows > imgH) return;
    const int rowBytes = imgW * 4;
    ImageChunk chunk;
    chunk.height = rows;
    chunk.pixels.resize(static_cast<size_t>(rowBytes) * rows);

    const int bodyTop = staticTop;
    const int bodyBottom = imgH - staticBottom;
    const int bodyRows = std::max(1, bodyBottom - bodyTop);
    const double stationarySceneRatio = committedFrame.size() == frame.size()
        ? sampledSameRatio(committedFrame, frame, imgW, imgH) : 0.0;
    const bool layeredScene = stationarySceneRatio >= 0.55 && stationarySceneRatio < 0.996;
    const int blockW = std::max(8, imgW / 48);
    int stabilizedBlocks = 0;
    int consideredBlocks = 0;

    auto blockStationary = [&](int y, int x0, int x1) {
        if (!layeredScene || committedFrame.size() != frame.size() || y < 0 || y >= imgH) return false;
        int same = 0;
        int total = 0;
        int step = std::max(2, (x1 - x0) / 5);
        for (int yy = std::max(bodyTop, y - 1); yy <= std::min(bodyBottom - 1, y + 1); ++yy) {
            for (int x = x0; x < x1; x += step) {
                size_t i = (static_cast<size_t>(yy) * imgW + x) * 4;
                int d = std::abs(static_cast<int>(committedFrame[i]) - static_cast<int>(frame[i]))
                    + std::abs(static_cast<int>(committedFrame[i + 1]) - static_cast<int>(frame[i + 1]))
                    + std::abs(static_cast<int>(committedFrame[i + 2]) - static_cast<int>(frame[i + 2]));
                if (d <= 9) ++same;
                ++total;
            }
        }
        return total >= 6 && same * 100 >= total * 94;
    };

    for (int r = 0; r < rows; ++r) {
        const int sy = sourceY + r;
        BYTE* dstRow = chunk.pixels.data() + static_cast<size_t>(r) * rowBytes;
        const BYTE* srcRow = frame.data() + static_cast<size_t>(sy) * rowBytes;
        if (!layeredScene) {
            CopyMemory(dstRow, srcRow, rowBytes);
            continue;
        }

        const int phaseY = bodyTop + ((bodyHeight + r) % bodyRows);
        const BYTE* phaseRow = frame.data() + static_cast<size_t>(phaseY) * rowBytes;
        for (int x0 = 0; x0 < imgW; x0 += blockW) {
            int x1 = std::min(imgW, x0 + blockW);
            ++consideredBlocks;
            const bool sourceStatic = blockStationary(sy, x0, x1);
            const bool phaseStatic = blockStationary(phaseY, x0, x1);
            const BYTE* chosen = (sourceStatic && phaseStatic) ? phaseRow : srcRow;
            if (sourceStatic && phaseStatic) ++stabilizedBlocks;
            CopyMemory(dstRow + static_cast<size_t>(x0) * 4,
                chosen + static_cast<size_t>(x0) * 4,
                static_cast<size_t>(x1 - x0) * 4);
        }
    }

    bodyChunks.push_back(std::move(chunk));
    bodyHeight += rows;
    resultH = staticTop + bodyHeight + staticBottom;
    invalidateMaterialized();
    if (layeredScene) {
        StarCapDiag::append(std::format(
            L"[long-next] layer-append same={:.4f} rows={} stabilized={}/{} bodyHeight={}",
            stationarySceneRatio, rows, stabilizedBlocks, consideredBlocks, bodyHeight));
    }
}

'''
cap = cap[:start] + new_append + cap[end:]

# 3) Resize flicker: while the user is actively dragging a handle, do not hide the
# magnifier just because the cursor is momentarily outside the *previous* mask rect.
old = '''    if (!cap || hideLabel || cap->stage != WinCap::CapStage::Adjust ||\n        !hasRect() || !pointInRect(maskRect, live)) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
new = '''    const bool activeAdjustDrag = cap && cap->isPress && adjustHit != MaskHit::None;\n    if (!cap || hideLabel || cap->stage != WinCap::CapStage::Adjust ||\n        !hasRect() || (!pointInRect(maskRect, live) && !activeAdjustDrag)) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
if old not in mask:
    raise SystemExit('updateMagnifierPopup guard changed')
mask = mask.replace(old, new, 1)

old = '''    if (hasRect() && !pointInRect(maskRect, live)) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
new = '''    const bool activeAdjustDrag = cap->isPress && adjustHit != MaskHit::None;\n    if (hasRect() && !pointInRect(maskRect, live) && !activeAdjustDrag) {\n        hideMagnifierPopup();\n        return;\n    }\n'''
if old not in mask:
    raise SystemExit('paintMagnifier guard changed')
mask = mask.replace(old, new, 1)

cap_path.write_text(cap, encoding='utf-8')
mask_path.write_text(mask, encoding='utf-8')
print('patched Long Capture Next v7')
