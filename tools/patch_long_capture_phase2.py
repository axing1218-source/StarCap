from pathlib import Path

path = Path('Src/Win/CapLong.cpp')
text = path.read_text(encoding='utf-8-sig')

helper_marker = '''    int findScrollByBottomStrip(const BYTE* grayOld, const BYTE* grayNew, int width, int stripH)\n'''
if helper_marker not in text:
    raise SystemExit('helper insertion marker not found')

helpers = r'''    struct ChangedRun
    {
        int x0{};
        int x1{};
        int width() const { return x1 - x0 + 1; }
    };

    struct OffsetMatch
    {
        int offset{ 0 };
        double score{ DBL_MAX };
        double alternative{ DBL_MAX };
        double restScore{ DBL_MAX };
        int runCount{ 0 };
    };

    int firstMeaningfulChangedY(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        int width, int height)
    {
        if (width <= 0 || height <= 0 || oldFrame.size() != newFrame.size()) return -1;
        size_t expected = (size_t)width * height * 4;
        if (oldFrame.size() < expected) return -1;

        // Ignore the bottom UI band (chat composer, status bar, scrollbar controls). A blinking
        // caret or animated send button must not be mistaken for the start of a real scroll.
        int endY = height - std::max(20, height / 12);
        if (endY <= 0) endY = height;
        int xStep = std::max(1, width / 160);
        int samplesPerRow = (width + xStep - 1) / xStep;
        int minHits = std::max(3, samplesPerRow / 35);

        for (int y = 0; y < endY; y++) {
            int hits = 0;
            for (int x = 0; x < width; x += xStep) {
                size_t i = ((size_t)y * width + x) * 4;
                int diff = abs((int)oldFrame[i] - (int)newFrame[i])
                    + abs((int)oldFrame[i + 1] - (int)newFrame[i + 1])
                    + abs((int)oldFrame[i + 2] - (int)newFrame[i + 2]);
                if (diff > 28 && ++hits >= minHits) return y;
            }
        }
        return -1;
    }

    std::vector<ChangedRun> detectChangedRuns(const std::vector<BYTE>& oldFrame,
        const std::vector<BYTE>& newFrame, int width, int height, int startY)
    {
        std::vector<ChangedRun> runs;
        if (width <= 2 || height <= 2 || oldFrame.size() != newFrame.size()) return runs;

        int endY = height - std::max(24, height / 10);
        startY = std::clamp(startY, 0, std::max(0, endY - 1));
        int usableH = endY - startY;
        if (usableH < 20) return runs;

        int colStep = std::max(1, width / 240);
        int rowStep = std::max(1, usableH / 96);
        int safeRight = width - std::max(10, width / 80); // avoid the scrollbar edge
        safeRight = std::max(2, safeRight);
        std::vector<int> changed;

        for (int x = 0; x < safeRight; x += colStep) {
            int hits = 0;
            for (int y = startY; y < endY; y += rowStep) {
                size_t i = ((size_t)y * width + x) * 4;
                int diff = abs((int)oldFrame[i] - (int)newFrame[i])
                    + abs((int)oldFrame[i + 1] - (int)newFrame[i + 1])
                    + abs((int)oldFrame[i + 2] - (int)newFrame[i + 2]);
                if (diff > 24 && ++hits >= 2) break;
            }
            if (hits >= 2) changed.push_back(x);
        }

        auto flush = [&](int first, int last) {
            int trim = colStep;
            int x0 = std::max(0, first + trim);
            int x1 = std::min(safeRight - 1, last - trim);
            if (x1 <= x0) { x0 = std::max(0, first); x1 = std::min(safeRight - 1, last); }
            if (x1 - x0 + 1 >= std::max(16, width / 32)) runs.push_back({ x0, x1 });
        };

        if (!changed.empty()) {
            int first = changed[0];
            int last = changed[0];
            int maxGap = colStep * 3;
            for (size_t i = 1; i < changed.size(); i++) {
                if (changed[i] - last <= maxGap) last = changed[i];
                else { flush(first, last); first = last = changed[i]; }
            }
            flush(first, last);
        }

        std::sort(runs.begin(), runs.end(), [](const ChangedRun& a, const ChangedRun& b) {
            return a.width() > b.width();
        });
        if (runs.size() > 6) runs.resize(6);

        // Sparse pages can produce fragmented changed columns. The fallback is deliberately
        // narrower than the full frame so fixed sidebars and the scrollbar cannot dominate.
        if (runs.empty()) {
            int margin = std::max(8, width / 24);
            int x0 = margin;
            int x1 = std::max(x0 + 1, safeRight - margin - 1);
            if (x1 > x0) runs.push_back({ x0, x1 });
        }
        return runs;
    }

    int edgeAt(const std::vector<BYTE>& gray, int width, int height, int x, int y)
    {
        x = std::clamp(x, 1, width - 2);
        y = std::clamp(y, 1, height - 2);
        int h = abs((int)gray[(size_t)y * width + x + 1] - (int)gray[(size_t)y * width + x - 1]);
        int v = abs((int)gray[(size_t)(y + 1) * width + x] - (int)gray[(size_t)(y - 1) * width + x]);
        return h + v;
    }

    double scoreOffset(const std::vector<BYTE>& oldGray, const std::vector<BYTE>& newGray,
        int width, int height, int startY, int endY, int offset,
        const std::vector<ChangedRun>& runs, int yStep, bool fine)
    {
        int rows = endY - startY - offset;
        if (rows < 30 || runs.empty()) return DBL_MAX;
        startY = std::max(1, startY);
        endY = std::min(height - 1, endY);
        double weightedError = 0.0;
        double totalWeight = 0.0;

        for (const auto& run : runs) {
            int x0 = std::clamp(run.x0, 1, width - 2);
            int x1 = std::clamp(run.x1, 1, width - 2);
            if (x1 <= x0) continue;
            int xStep = std::max(1, (x1 - x0 + 1) / (fine ? 100 : 55));
            for (int r = 0; r < rows; r += yStep) {
                int oldY = startY + offset + r;
                int newY = startY + r;
                if (oldY >= endY || newY >= endY) break;
                for (int x = x0; x <= x1; x += xStep) {
                    BYTE a = oldGray[(size_t)oldY * width + x];
                    BYTE b = newGray[(size_t)newY * width + x];
                    int ea = edgeAt(oldGray, width, height, x, oldY);
                    int eb = edgeAt(newGray, width, height, x, newY);
                    double weight = 1.0 + std::min(4.0, (double)std::max(ea, eb) / 48.0);
                    double error = abs((int)a - (int)b) + 0.35 * abs(ea - eb);
                    weightedError += weight * error;
                    totalWeight += weight;
                }
            }
        }
        return totalWeight > 0.0 ? weightedError / totalWeight : DBL_MAX;
    }

    OffsetMatch findScrollOffsetV2(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        int width, int height, int startY)
    {
        OffsetMatch result{};
        if (width <= 4 || height <= 80 || oldFrame.size() != newFrame.size()) return result;

        int endY = height - std::max(24, height / 10);
        startY = std::clamp(startY, 0, std::max(0, endY - 1));
        int contentH = endY - startY;
        int minOverlap = std::max(60, contentH / 4);
        int maxOffset = contentH - minOverlap;
        if (maxOffset <= 1) return result;

        auto runs = detectChangedRuns(oldFrame, newFrame, width, height, startY);
        result.runCount = (int)runs.size();
        if (runs.empty()) return result;

        auto oldGray = toGrayscale(oldFrame.data(), width, height, width * 4);
        auto newGray = toGrayscale(newFrame.data(), width, height, width * 4);
        int coarseStep = std::max(1, maxOffset / 160);
        int coarseYStep = std::max(2, contentH / 120);
        std::vector<std::pair<double, int>> candidates;
        candidates.reserve((size_t)maxOffset / coarseStep + 2);

        for (int d = 1; d <= maxOffset; d += coarseStep) {
            double score = scoreOffset(oldGray, newGray, width, height, startY, endY, d, runs, coarseYStep, false);
            if (score < DBL_MAX) candidates.push_back({ score, d });
        }
        if (candidates.empty()) return result;
        std::sort(candidates.begin(), candidates.end());
        int coarseBest = candidates.front().second;

        int refineRadius = std::max(3, coarseStep * 2);
        int refineStart = std::max(1, coarseBest - refineRadius);
        int refineEnd = std::min(maxOffset, coarseBest + refineRadius);
        int fineYStep = std::max(1, contentH / 200);
        std::vector<std::pair<double, int>> refined;
        for (int d = refineStart; d <= refineEnd; d++) {
            double score = scoreOffset(oldGray, newGray, width, height, startY, endY, d, runs, fineYStep, true);
            if (score < DBL_MAX) refined.push_back({ score, d });
        }
        if (refined.empty()) return result;
        std::sort(refined.begin(), refined.end());
        result.score = refined.front().first;
        int bestD = refined.front().second;

        int separation = std::max(4, coarseStep * 2);
        for (const auto& item : refined) {
            if (abs(item.second - bestD) >= separation) {
                result.alternative = std::min(result.alternative, item.first);
            }
        }
        for (const auto& item : candidates) {
            if (abs(item.second - bestD) >= separation) {
                result.alternative = std::min(result.alternative, item.first);
            }
        }
        result.restScore = scoreOffset(oldGray, newGray, width, height, startY, endY, 0, runs, coarseYStep, false);

        double alternativeGap = result.alternative < DBL_MAX ? result.alternative - result.score : DBL_MAX;
        bool absoluteOk = result.score <= 22.0;
        bool distinct = result.score <= 1.5 || alternativeGap >= std::max(0.75, result.score * 0.06);
        bool moved = result.restScore < DBL_MAX
            && result.restScore >= result.score + std::max(0.8, result.score * 0.05);
        if (absoluteOk && distinct && moved) result.offset = bestD;
        return result;
    }

'''
text = text.replace(helper_marker, helpers + helper_marker, 1)

old_first = r'''    if (firstCheck) {
        changeStartY = -1;
        for (int y = 0; y < imgH; y++) {
            for (int x = 0; x < imgW; x++) {
                int idx = (y * imgW + x) * 4;
                if (img1[idx] != data[idx] || img1[idx + 1] != data[idx + 1] || img1[idx + 2] != data[idx + 2]) {
                    changeStartY = y;
                    break;
                }
            }
            if (changeStartY != -1) break;
        }
        if (changeStartY == -1) {
            if (autoScroll) handleAutoNoProgress();
            else scheduleNextCapture(manualPollMs);
            return;
        }
        firstCheck = false;
    }
'''
new_first = r'''    if (firstCheck) {
        changeStartY = firstMeaningfulChangedY(img1, data, imgW, imgH);
        if (changeStartY == -1) {
            if (autoScroll) handleAutoNoProgress();
            else scheduleNextCapture(manualPollMs);
            return;
        }
        firstCheck = false;
    }
'''
if old_first not in text:
    raise SystemExit('firstCheck block not found')
text = text.replace(old_first, new_first, 1)

old_match = r'''    int rowPix{ imgW * 4 };
    int stripH = std::min(comparisonH, imgH - changeStartY);
    if (stripH <= 0) {
        if (autoScroll) handleAutoNoProgress();
        else scheduleNextCapture(manualPollMs);
        return;
    }

    int img1StripH = imgH - changeStartY;
    auto gray1 = toGrayscale(img1.data() + (size_t)changeStartY * rowPix, imgW, img1StripH, rowPix);
    auto gray2 = toGrayscale(data.data() + (size_t)changeStartY * rowPix, imgW, stripH, rowPix);
    int y = findMostSimilarY(gray1.data(), img1StripH, gray2.data(), stripH, imgW);
    if (y == 0) {
        auto gray1Bottom = toGrayscale(img1.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
        auto gray2Bottom = toGrayscale(data.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
        y = findScrollByBottomStrip(gray1Bottom.data(), gray2Bottom.data(), imgW, stripH);
    }
'''
new_match = r'''    int rowPix{ imgW * 4 };
    auto v2Match = findScrollOffsetV2(img1, data, imgW, imgH, changeStartY);
    int y = v2Match.offset;
    bool usedV2 = y > 0;
    bool ambiguousV2 = y == 0 && v2Match.score < 35.0;

    // Keep the proven v0.10.4 matcher as a compatibility fallback only when V2 could not form a
    // plausible candidate. If V2 found two near-identical seams, refusing the stitch is safer than
    // letting repetitive chat bubbles or table rows select a wrong join.
    if (y == 0 && !ambiguousV2) {
        int stripH = std::min(comparisonH, imgH - changeStartY);
        if (stripH <= 0) {
            if (autoScroll) handleAutoNoProgress();
            else scheduleNextCapture(manualPollMs);
            return;
        }
        int img1StripH = imgH - changeStartY;
        auto gray1 = toGrayscale(img1.data() + (size_t)changeStartY * rowPix, imgW, img1StripH, rowPix);
        auto gray2 = toGrayscale(data.data() + (size_t)changeStartY * rowPix, imgW, stripH, rowPix);
        y = findMostSimilarY(gray1.data(), img1StripH, gray2.data(), stripH, imgW);
        if (y == 0) {
            auto gray1Bottom = toGrayscale(img1.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
            auto gray2Bottom = toGrayscale(data.data() + (size_t)(imgH - stripH) * rowPix, imgW, stripH, rowPix);
            y = findScrollByBottomStrip(gray1Bottom.data(), gray2Bottom.data(), imgW, stripH);
        }
    }

    if (usedV2) {
        int score100 = (int)(v2Match.score * 100.0);
        int alt100 = v2Match.alternative < DBL_MAX ? (int)(v2Match.alternative * 100.0) : -1;
        int rest100 = v2Match.restScore < DBL_MAX ? (int)(v2Match.restScore * 100.0) : -1;
        StarCapDiag::append(std::format(L"[long-v2] stitch-v2 offset={} score100={} alt100={} rest100={} runs={} startY={}",
            y, score100, alt100, rest100, v2Match.runCount, changeStartY));
    }
    else if (ambiguousV2 && framesDiffer(data, img1)) {
        int score100 = (int)(v2Match.score * 100.0);
        int alt100 = v2Match.alternative < DBL_MAX ? (int)(v2Match.alternative * 100.0) : -1;
        StarCapDiag::append(std::format(L"[long-v2] stitch-rejected score100={} alt100={} runs={} startY={}",
            score100, alt100, v2Match.runCount, changeStartY));
    }
'''
if old_match not in text:
    raise SystemExit('legacy match block not found')
text = text.replace(old_match, new_match, 1)

old_nomatch = r'''    if (y == 0) {
        // Automatic mode already waited for the newly exposed bottom band to settle before
        // reaching this matcher. Manual mode keeps the older recheck path for smooth wheel input.
        if (!autoScroll && framesDiffer(data, img1) && settleRecheckCount < maxSettleRecheck) {
            settleRecheckCount++;
            scheduleNextCapture(settleRecheckMs);
            return;
        }
        settleRecheckCount = 0;
        if (autoScroll) handleAutoNoProgress();
        else {
            // Manual mode never decides that the user is "done" merely because they paused.
            scheduleNextCapture(manualPollMs);
        }
        return;
    }
'''
new_nomatch = r'''    if (y == 0) {
        // Automatic mode already waited for the newly exposed bottom band to settle before
        // reaching this matcher. Manual mode keeps the older recheck path for smooth wheel input.
        if (!autoScroll && framesDiffer(data, img1) && settleRecheckCount < maxSettleRecheck) {
            settleRecheckCount++;
            scheduleNextCapture(settleRecheckMs);
            return;
        }
        settleRecheckCount = 0;
        // Until the first real seam is accepted, go back to meaningful-change detection. This
        // prevents a caret blink or animated badge from permanently pinning changeStartY.
        if (resultH == imgH) {
            firstCheck = true;
            changeStartY = -1;
        }
        if (autoScroll) handleAutoNoProgress();
        else {
            // Manual mode never decides that the user is "done" merely because they paused.
            scheduleNextCapture(manualPollMs);
        }
        return;
    }
'''
if old_nomatch not in text:
    raise SystemExit('no-match block not found')
text = text.replace(old_nomatch, new_nomatch, 1)

path.write_text(text, encoding='utf-8-sig')
print('Long Capture V2 Phase 2 patch applied')
