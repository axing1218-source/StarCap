from pathlib import Path

root = Path(__file__).resolve().parents[1]
cap_path = root / 'Src/Win/CapLong.cpp'
mask_path = root / 'Src/Win/CutMask.cpp'

cap = cap_path.read_text(encoding='utf-8')
mask = mask_path.read_text(encoding='utf-8')

# -----------------------------------------------------------------------------
# 1) Re-introduce the strongest part of the old Phase 2 matcher as a dedicated
#    fixed-background / sparse-chat path. It only influences seam selection.
#    Final output pixels remain untouched and are still copied verbatim.
# -----------------------------------------------------------------------------
anchor = '    LRESULT CALLBACK longCaptureKeyboardProc(int code, WPARAM wParam, LPARAM lParam)\n'
if anchor not in cap:
    raise SystemExit('CapLong keyboard anchor changed')

helpers = r'''    struct ChangedRunV2
    {
        int x0{};
        int x1{};
        int width() const { return x1 - x0 + 1; }
    };

    struct ChangedMatchV2
    {
        int offset{ 0 };
        double score{ DBL_MAX };
        double alternative{ DBL_MAX };
        double restScore{ DBL_MAX };
        int runCount{ 0 };
    };

    std::vector<BYTE> toGrayV2(const std::vector<BYTE>& frame, int width, int height)
    {
        std::vector<BYTE> gray(static_cast<size_t>(width) * height);
        if (frame.size() < static_cast<size_t>(width) * height * 4) return gray;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t i = (static_cast<size_t>(y) * width + x) * 4;
                gray[static_cast<size_t>(y) * width + x] = static_cast<BYTE>(
                    (static_cast<int>(frame[i]) * 29 + static_cast<int>(frame[i + 1]) * 150
                        + static_cast<int>(frame[i + 2]) * 77) >> 8);
            }
        }
        return gray;
    }

    int edgeAtV2(const std::vector<BYTE>& gray, int width, int height, int x, int y)
    {
        x = std::clamp(x, 1, width - 2);
        y = std::clamp(y, 1, height - 2);
        int h = std::abs(static_cast<int>(gray[static_cast<size_t>(y) * width + x + 1])
            - static_cast<int>(gray[static_cast<size_t>(y) * width + x - 1]));
        int v = std::abs(static_cast<int>(gray[static_cast<size_t>(y + 1) * width + x])
            - static_cast<int>(gray[static_cast<size_t>(y - 1) * width + x]));
        return h + v;
    }

    std::vector<ChangedRunV2> detectChangedRunsV2(const std::vector<BYTE>& oldFrame,
        const std::vector<BYTE>& newFrame, int width, int height, int bodyTop, int bodyBottom)
    {
        std::vector<ChangedRunV2> runs;
        if (width <= 4 || height <= 4 || oldFrame.size() != newFrame.size()) return runs;

        bodyTop = std::clamp(bodyTop, 0, height - 1);
        bodyBottom = std::clamp(bodyBottom, bodyTop + 1, height);
        int usableH = bodyBottom - bodyTop;
        if (usableH < 40) return runs;

        int colStep = std::max(1, width / 260);
        int rowStep = std::max(1, usableH / 110);
        int safeRight = width - std::max(12, width / 70); // scrollbar / fixed right-edge controls
        safeRight = std::max(3, safeRight);
        std::vector<int> changed;

        for (int x = 0; x < safeRight; x += colStep) {
            int hits = 0;
            for (int y = bodyTop; y < bodyBottom; y += rowStep) {
                size_t i = (static_cast<size_t>(y) * width + x) * 4;
                int diff = std::abs(static_cast<int>(oldFrame[i]) - static_cast<int>(newFrame[i]))
                    + std::abs(static_cast<int>(oldFrame[i + 1]) - static_cast<int>(newFrame[i + 1]))
                    + std::abs(static_cast<int>(oldFrame[i + 2]) - static_cast<int>(newFrame[i + 2]));
                if (diff > 24 && ++hits >= 2) break;
            }
            if (hits >= 2) changed.push_back(x);
        }

        auto flush = [&](int first, int last) {
            int trim = colStep;
            int x0 = std::max(0, first + trim);
            int x1 = std::min(safeRight - 1, last - trim);
            if (x1 <= x0) {
                x0 = std::max(0, first);
                x1 = std::min(safeRight - 1, last);
            }
            if (x1 - x0 + 1 >= std::max(14, width / 36)) runs.push_back({ x0, x1 });
        };

        if (!changed.empty()) {
            int first = changed.front();
            int last = changed.front();
            int maxGap = colStep * 3;
            for (size_t i = 1; i < changed.size(); ++i) {
                if (changed[i] - last <= maxGap) last = changed[i];
                else {
                    flush(first, last);
                    first = last = changed[i];
                }
            }
            flush(first, last);
        }

        std::sort(runs.begin(), runs.end(), [](const ChangedRunV2& a, const ChangedRunV2& b) {
            return a.width() > b.width();
        });
        if (runs.size() > 6) runs.resize(6);
        return runs;
    }

    double scoreChangedOffsetV2(const std::vector<BYTE>& oldGray, const std::vector<BYTE>& newGray,
        int width, int height, int bodyTop, int bodyBottom, int offset,
        const std::vector<ChangedRunV2>& runs, int yStep, bool fine)
    {
        int rows = bodyBottom - bodyTop - offset;
        if (rows < 36 || runs.empty()) return DBL_MAX;
        bodyTop = std::max(1, bodyTop);
        bodyBottom = std::min(height - 1, bodyBottom);

        double weightedError = 0.0;
        double totalWeight = 0.0;
        for (const auto& run : runs) {
            int x0 = std::clamp(run.x0, 1, width - 2);
            int x1 = std::clamp(run.x1, 1, width - 2);
            if (x1 <= x0) continue;
            int xStep = std::max(1, (x1 - x0 + 1) / (fine ? 100 : 55));
            for (int r = 0; r < rows; r += yStep) {
                int oldY = bodyTop + offset + r;
                int newY = bodyTop + r;
                if (oldY >= bodyBottom || newY >= bodyBottom) break;
                for (int x = x0; x <= x1; x += xStep) {
                    BYTE a = oldGray[static_cast<size_t>(oldY) * width + x];
                    BYTE b = newGray[static_cast<size_t>(newY) * width + x];
                    int ea = edgeAtV2(oldGray, width, height, x, oldY);
                    int eb = edgeAtV2(newGray, width, height, x, newY);
                    double weight = 1.0 + std::min(4.0, static_cast<double>(std::max(ea, eb)) / 48.0);
                    double error = std::abs(static_cast<int>(a) - static_cast<int>(b))
                        + 0.35 * std::abs(ea - eb);
                    weightedError += weight * error;
                    totalWeight += weight;
                }
            }
        }
        return totalWeight > 0.0 ? weightedError / totalWeight : DBL_MAX;
    }

    ChangedMatchV2 findChangedOffsetV2(const std::vector<BYTE>& oldFrame,
        const std::vector<BYTE>& newFrame, int width, int height, int bodyTop, int bodyBottom)
    {
        ChangedMatchV2 result{};
        int contentH = bodyBottom - bodyTop;
        if (width <= 4 || contentH < 120 || oldFrame.size() != newFrame.size()) return result;

        auto runs = detectChangedRunsV2(oldFrame, newFrame, width, height, bodyTop, bodyBottom);
        result.runCount = static_cast<int>(runs.size());
        if (runs.empty()) return result;

        auto oldGray = toGrayV2(oldFrame, width, height);
        auto newGray = toGrayV2(newFrame, width, height);
        int minOverlap = std::max(70, contentH / 4);
        int maxOffset = contentH - minOverlap;
        if (maxOffset <= 1) return result;

        int coarseStep = std::max(1, maxOffset / 170);
        int coarseYStep = std::max(2, contentH / 120);
        std::vector<std::pair<double, int>> coarse;
        for (int d = 1; d <= maxOffset; d += coarseStep) {
            double score = scoreChangedOffsetV2(oldGray, newGray, width, height,
                bodyTop, bodyBottom, d, runs, coarseYStep, false);
            if (score < DBL_MAX) coarse.push_back({ score, d });
        }
        if (coarse.empty()) return result;
        std::sort(coarse.begin(), coarse.end());

        int coarseBest = coarse.front().second;
        int refineRadius = std::max(3, coarseStep * 2);
        int refineStart = std::max(1, coarseBest - refineRadius);
        int refineEnd = std::min(maxOffset, coarseBest + refineRadius);
        int fineYStep = std::max(1, contentH / 210);
        std::vector<std::pair<double, int>> refined;
        for (int d = refineStart; d <= refineEnd; ++d) {
            double score = scoreChangedOffsetV2(oldGray, newGray, width, height,
                bodyTop, bodyBottom, d, runs, fineYStep, true);
            if (score < DBL_MAX) refined.push_back({ score, d });
        }
        if (refined.empty()) return result;
        std::sort(refined.begin(), refined.end());

        result.score = refined.front().first;
        int bestD = refined.front().second;
        int separation = std::max(4, coarseStep * 2);
        for (const auto& item : refined) {
            if (std::abs(item.second - bestD) >= separation)
                result.alternative = std::min(result.alternative, item.first);
        }
        for (const auto& item : coarse) {
            if (std::abs(item.second - bestD) >= separation)
                result.alternative = std::min(result.alternative, item.first);
        }
        result.restScore = scoreChangedOffsetV2(oldGray, newGray, width, height,
            bodyTop, bodyBottom, 0, runs, coarseYStep, false);

        double gap = result.alternative < DBL_MAX ? result.alternative - result.score : DBL_MAX;
        bool absoluteOk = result.score <= 22.0;
        bool distinct = result.score <= 1.5 || gap >= std::max(0.75, result.score * 0.06);
        bool moved = result.restScore < DBL_MAX
            && result.restScore >= result.score + std::max(0.8, result.score * 0.05);
        if (absoluteOk && distinct && moved) result.offset = bestD;
        return result;
    }

'''
cap = cap.replace(anchor, helpers + anchor, 1)

old = '''    auto motionWeights = makeMotionWeights(oldFeatures, newFeatures, imgH);\n    double movingCoverage = motionCoverage(motionWeights, imgH, bodyTop, bodyBottom);\n    const bool motionAware = movingCoverage >= 0.040;\n    const std::vector<float> noMotionWeights;\n    const auto& scoreWeights = motionAware ? motionWeights : noMotionWeights;\n\n    struct Candidate {\n'''
new = '''    auto motionWeights = makeMotionWeights(oldFeatures, newFeatures, imgH);\n    double movingCoverage = motionCoverage(motionWeights, imgH, bodyTop, bodyBottom);\n    const bool motionAware = movingCoverage >= 0.040;\n    const std::vector<float> noMotionWeights;\n    const auto& scoreWeights = motionAware ? motionWeights : noMotionWeights;\n\n    // Sparse chat windows often keep the wallpaper fixed in viewport coordinates while\n    // messages move. In that case use the old Phase 2 changed-column matcher to select\n    // the seam from actual moving content instead of letting the wallpaper vote.\n    const bool layeredCandidate = sameRatio >= 0.40 && movingCoverage >= 0.025 && movingCoverage <= 0.60;\n    ChangedMatchV2 changedMatch;\n    if (layeredCandidate)\n        changedMatch = findChangedOffsetV2(oldFrame, newFrame, imgW, imgH, bodyTop, bodyBottom);\n\n    struct Candidate {\n'''
if old not in cap:
    raise SystemExit('motion block changed')
cap = cap.replace(old, new, 1)

old = '''    result.offset = best.offset;\n    result.visualScore = best.raw;\n    result.secondScore = second.raw;\n    result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, scoreWeights, imgW, imgH, best.offset, bodyTop, bodyBottom);\n\n    double adjustedMargin = second.adjusted - best.adjusted;\n    bool nearExact = best.raw <= 0.018 && result.pixelMad <= 7.0;\n    bool robust = best.raw <= 0.055 && result.pixelMad <= 20.0 && adjustedMargin >= 0.0035;\n    bool structured = false;\n\n    if (structuredExpectedOffset >= 4.0) {\n        double tolerance = std::max(12.0, structuredExpectedOffset * 0.20);\n        structured = std::abs(best.offset - structuredExpectedOffset) <= tolerance\n            && best.raw <= 0.080 && result.pixelMad <= 28.0 && adjustedMargin >= 0.0010;\n    }\n\n    result.accepted = nearExact || robust || structured;\n    result.usedStructuredPrior = structured;\n\n    StarCapDiag::append(std::format(\n        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} motion={:.4f}/{} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio, movingCoverage, motionAware ? 1 : 0,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n        nearExact ? L"exact" : (structured ? L"structured" : (robust ? L"robust" : L"reject"))));\n'''
new = '''    int chosenOffset = best.offset;\n    bool usedChangedRuns = false;\n    if (changedMatch.offset > 0) {\n        bool structuredCompatible = true;\n        if (structuredExpectedOffset >= 4.0) {\n            double tolerance = std::max(14.0, structuredExpectedOffset * 0.24);\n            structuredCompatible = std::abs(changedMatch.offset - structuredExpectedOffset) <= tolerance;\n        }\n        double changedMad = pixelMadAtOffset(oldFrame, newFrame, scoreWeights, imgW, imgH,\n            changedMatch.offset, bodyTop, bodyBottom);\n        if (structuredCompatible && changedMad <= 25.0) {\n            chosenOffset = changedMatch.offset;\n            usedChangedRuns = true;\n        }\n    }\n\n    result.offset = chosenOffset;\n    result.visualScore = featureScore(oldFeatures, newFeatures, scoreWeights, imgH,\n        chosenOffset, bodyTop, bodyBottom);\n    result.secondScore = second.raw;\n    result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, scoreWeights, imgW, imgH,\n        chosenOffset, bodyTop, bodyBottom);\n\n    double adjustedMargin = second.adjusted - best.adjusted;\n    bool nearExact = !usedChangedRuns && best.raw <= 0.018 && result.pixelMad <= 7.0;\n    bool robust = !usedChangedRuns && best.raw <= 0.055 && result.pixelMad <= 20.0 && adjustedMargin >= 0.0035;\n    bool structured = false;\n    bool changed = usedChangedRuns;\n\n    if (!usedChangedRuns && structuredExpectedOffset >= 4.0) {\n        double tolerance = std::max(12.0, structuredExpectedOffset * 0.20);\n        structured = std::abs(best.offset - structuredExpectedOffset) <= tolerance\n            && best.raw <= 0.080 && result.pixelMad <= 28.0 && adjustedMargin >= 0.0010;\n    }\n\n    result.accepted = changed || nearExact || robust || structured;\n    result.usedStructuredPrior = structured;\n\n    StarCapDiag::append(std::format(\n        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} motion={:.4f}/{} changed={}/{:.3f}/{:.3f}/{} chosen={} body=[{},{}] static={}+{} accept={} mode={}",\n        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,\n        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio, movingCoverage, motionAware ? 1 : 0,\n        changedMatch.offset, changedMatch.score, changedMatch.restScore, changedMatch.runCount, chosenOffset,\n        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,\n        changed ? L"changed-runs" : (nearExact ? L"exact" : (structured ? L"structured" : (robust ? L"robust" : L"reject")))));\n'''
if old not in cap:
    raise SystemExit('match result block changed')
cap = cap.replace(old, new, 1)

# -----------------------------------------------------------------------------
# 2) Qt element mode: do not walk a huge UIA RawView tree. Telegram's Qt UIA
#    provider can block badly. One point hit-test + native fallback is enough.
#    Chromium / browser UIA path stays unchanged.
# -----------------------------------------------------------------------------
old = '''\tauto semanticWeight = [](CONTROLTYPEID type) {\n'''
qt_fast = r'''	auto isQtWindow = [](HWND h) {
		if (!h) return false;
		wchar_t cls[128]{};
		int n = GetClassNameW(h, cls, static_cast<int>(std::size(cls)));
		if (n <= 0) return false;
		return wcsncmp(cls, L"Qt", 2) == 0 || wcsstr(cls, L"QWindow") != nullptr;
	};

	if (isQtWindow(probeHwnd) || isQtWindow(hwnd)) {
		ComPtr<IAccessible> pointAcc;
		VARIANT child{};
		VariantInit(&child);
		HRESULT hr = AccessibleObjectFromPoint(screenPos, pointAcc.GetAddressOf(), &child);
		if (SUCCEEDED(hr) && pointAcc) {
			long l{}, t{}, w{}, h{};
			if (SUCCEEDED(pointAcc->accLocation(&l, &t, &w, &h, child)) && w >= 3 && h >= 3) {
				RECT rr{ l, t, l + w, t + h };
				D2D1_RECT_F local{};
				if (clippedCandidate(rr, fallback, local) && rectArea(local) < rectArea(fallback) * .995f) {
					VariantClear(&child);
					return local;
				}
			}
		}
		VariantClear(&child);
		return detectNativeChildRect(hwnd, localPos, fallback);
	}

'''
if old not in mask:
    raise SystemExit('CutMask semantic anchor changed')
mask = mask.replace(old, qt_fast + old, 1)

cap_path.write_text(cap, encoding='utf-8')
mask_path.write_text(mask, encoding='utf-8')
print('Long Capture Next v10 source patched.')
