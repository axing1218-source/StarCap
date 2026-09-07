#include "pch.h"
#include <include/Ling.h>
#include <UIAutomation.h>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include "CapLong.h"
#include "WinCap.h"
#include "CutMask.h"
#include "WinPin.h"
#include "../Tool/ToolLong.h"
#include "../App.h"
#include "../Util.h"
#include "../Lang.h"
#include "../StarCapDiag.h"

#pragma comment(lib, "uiautomationcore.lib")

using namespace Microsoft::WRL;

namespace {
    constexpr UINT autoScrollTimerId = 18;
    constexpr UINT frameCaptureTimerId = 20;
    constexpr int frameCaptureMs = 45;
    constexpr int autoScrollMs = 65;
    constexpr int maxFrameRing = 8;
    constexpr int noProgressBeforeFallback = 12;
    constexpr int rejectedBeforePause = 5;
    constexpr int featureBins = 12;
    constexpr size_t maxLongBufferBytes = 384ull * 1024ull * 1024ull;
    constexpr int maxLongResultHeight = 150000;

    HHOOK gLongCaptureHook = nullptr;
    CapLong* gLongCapture = nullptr;
    bool gSpaceDown = false;
    bool gEnterDown = false;
    bool gEscapeDown = false;

    std::wstring windowClassName(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd)) return L"<none>";
        wchar_t cls[256]{};
        int n = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
        if (n <= 0) return L"<unknown>";
        return std::wstring(cls, static_cast<size_t>(n));
    }

    double sampledSameRatio(const std::vector<BYTE>& a, const std::vector<BYTE>& b, int width, int height)
    {
        if (a.size() != b.size() || width <= 0 || height <= 0) return 0.0;
        int xStep = std::max(4, width / 120);
        int yStep = std::max(3, height / 120);
        int same = 0;
        int total = 0;
        for (int y = 0; y < height; y += yStep) {
            for (int x = 0; x < width; x += xStep) {
                size_t i = (static_cast<size_t>(y) * width + x) * 4;
                int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]))
                    + std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]))
                    + std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (d <= 18) ++same;
                ++total;
            }
        }
        return total > 0 ? static_cast<double>(same) / total : 0.0;
    }

    double rowMad(const std::vector<BYTE>& a, const std::vector<BYTE>& b, int width, int y)
    {
        int margin = std::max(6, width / 30);
        int step = std::max(2, width / 180);
        double sum = 0.0;
        int count = 0;
        for (int x = margin; x < width - margin; x += step) {
            size_t i = (static_cast<size_t>(y) * width + x) * 4;
            sum += (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]))
                + std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]))
                + std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]))) / 3.0;
            ++count;
        }
        return count > 0 ? sum / count : 255.0;
    }

    std::pair<int, int> detectStaticBands(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame, int width, int height)
    {
        if (oldFrame.size() != newFrame.size() || width <= 0 || height <= 0) return { 0, 0 };
        int maxBand = std::max(0, height * 35 / 100);
        std::vector<bool> stable(static_cast<size_t>(height), false);
        for (int y = 0; y < height; ++y) stable[y] = rowMad(oldFrame, newFrame, width, y) <= 4.0;

        int top = 0;
        int misses = 0;
        for (int y = 0; y < maxBand; ++y) {
            if (stable[y]) {
                top = y + 1;
                misses = 0;
            }
            else if (++misses >= 3) break;
        }

        int bottom = 0;
        misses = 0;
        for (int y = height - 1; y >= height - maxBand; --y) {
            if (stable[y]) {
                bottom = height - y;
                misses = 0;
            }
            else if (++misses >= 3) break;
        }

        if (top < 18) top = 0;
        if (bottom < 18) bottom = 0;
        if (top + bottom > height * 55 / 100) {
            top = 0;
            bottom = 0;
        }
        return { top, bottom };
    }

    std::vector<float> makeRowFeatures(const std::vector<BYTE>& frame, int width, int height)
    {
        const int dims = featureBins * 2;
        std::vector<float> features(static_cast<size_t>(height) * dims, 0.0f);
        if (frame.empty() || width <= 0 || height <= 0) return features;

        int margin = std::max(8, width / 18);
        int usable = std::max(featureBins, width - margin * 2);
        for (int y = 0; y < height; ++y) {
            for (int bin = 0; bin < featureBins; ++bin) {
                int x0 = margin + usable * bin / featureBins;
                int x1 = margin + usable * (bin + 1) / featureBins;
                x1 = std::min(x1, width - margin);
                if (x1 <= x0) x1 = std::min(width - margin, x0 + 1);

                double graySum = 0.0;
                double edgeSum = 0.0;
                int count = 0;
                int previous = -1;
                for (int x = x0; x < x1; x += 2) {
                    size_t i = (static_cast<size_t>(y) * width + x) * 4;
                    int gray = (static_cast<int>(frame[i]) * 29
                        + static_cast<int>(frame[i + 1]) * 150
                        + static_cast<int>(frame[i + 2]) * 77) >> 8;
                    graySum += gray;
                    if (previous >= 0) edgeSum += std::abs(gray - previous);
                    previous = gray;
                    ++count;
                }
                size_t base = static_cast<size_t>(y) * dims + bin * 2;
                if (count > 0) {
                    features[base] = static_cast<float>(graySum / count);
                    features[base + 1] = static_cast<float>(edgeSum / std::max(1, count - 1));
                }
            }
        }
        return features;
    }

    double featureScore(const std::vector<float>& oldFeatures, const std::vector<float>& newFeatures,
        int height, int offset, int bodyTop, int bodyBottom)
    {
        const int dims = featureBins * 2;
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 1.0;
        double sum = 0.0;
        size_t count = 0;
        for (int y = bodyTop; y < end; y += 3) {
            const float* a = oldFeatures.data() + static_cast<size_t>(y + offset) * dims;
            const float* b = newFeatures.data() + static_cast<size_t>(y) * dims;
            for (int d = 0; d < dims; ++d) {
                double weight = (d & 1) ? 1.35 : 1.0;
                sum += std::abs(static_cast<double>(a[d]) - static_cast<double>(b[d])) * weight;
                count += static_cast<size_t>(weight > 1.0 ? 1 : 1);
            }
        }
        if (count == 0) return 1.0;
        return sum / (static_cast<double>(count) * 255.0);
    }

    double pixelMadAtOffset(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame,
        int width, int height, int offset, int bodyTop, int bodyBottom)
    {
        int margin = std::max(8, width / 20);
        int end = bodyBottom - offset;
        if (end <= bodyTop) return 255.0;
        double sum = 0.0;
        size_t count = 0;
        int xStep = std::max(5, width / 100);
        for (int y = bodyTop; y < end; y += 7) {
            int oldY = y + offset;
            for (int x = margin; x < width - margin; x += xStep) {
                size_t ia = (static_cast<size_t>(oldY) * width + x) * 4;
                size_t ib = (static_cast<size_t>(y) * width + x) * 4;
                sum += std::abs(static_cast<int>(oldFrame[ia]) - static_cast<int>(newFrame[ib]));
                sum += std::abs(static_cast<int>(oldFrame[ia + 1]) - static_cast<int>(newFrame[ib + 1]));
                sum += std::abs(static_cast<int>(oldFrame[ia + 2]) - static_cast<int>(newFrame[ib + 2]));
                count += 3;
            }
        }
        return count > 0 ? sum / count : 255.0;
    }

    LRESULT CALLBACK longCaptureKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && gLongCapture) {
            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;

            if (kb->vkCode == VK_SPACE) {
                if (down && !gSpaceDown) {
                    gSpaceDown = true;
                    gLongCapture->toggleAutoScroll();
                }
                if (up) gSpaceDown = false;
                return 1;
            }
            if (kb->vkCode == VK_RETURN) {
                if (down && !gEnterDown) {
                    gEnterDown = true;
                    gLongCapture->hotkeyEnter();
                }
                if (up) gEnterDown = false;
                return 1;
            }
            if (kb->vkCode == VK_ESCAPE) {
                if (down && !gEscapeDown) {
                    gEscapeDown = true;
                    gLongCapture->hotkeyEscape();
                }
                if (up) gEscapeDown = false;
                return 1;
            }
        }
        return CallNextHookEx(gLongCaptureHook, code, wParam, lParam);
    }
}

CapLong::CapLong(WinCap* win) : win(win)
{
    auto d2d = Ling::D2D::get();
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), textBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.68f), bgBrush.GetAddressOf());

    isCapturing = true;
    win->hollowWin();
    makeTool();
    firstStep();
    installControlHook();
}

CapLong::~CapLong()
{
    uninstallControlHook();
    releaseUiaScroll();
}

void CapLong::dispose()
{
    win->killTimer(autoScrollTimerId);
    win->killTimer(frameCaptureTimerId);
    uninstallControlHook();
    releaseUiaScroll();
    if (tool) tool->close();
}

void CapLong::paint(ID2D1DeviceContext* ctx)
{
    paintImgPreview(ctx);
    if (isFinish && layoutTextEnd) {
        auto borderRadius{ 4.f * win->dpi };
        ctx->FillRoundedRectangle(D2D1::RoundedRect(stopTextRect, borderRadius, borderRadius), bgBrush.Get());
        ctx->DrawTextLayout(stopTextPos, layoutTextEnd.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void CapLong::setCursor()
{
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

void CapLong::onMove(POINT)
{
}

void CapLong::onUp(POINT)
{
}

void CapLong::scheduleFrameCapture(int delayMs)
{
    if (!isCapturing || isFinish) return;
    win->setTimer(delayMs, frameCaptureTimerId);
}

void CapLong::scheduleAutoScroll(int delayMs)
{
    if (!isCapturing || isFinish || !autoScroll) return;
    win->setTimer(delayMs, autoScrollTimerId);
}

void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == frameCaptureTimerId) {
        win->killTimer(frameCaptureTimerId);
        if (!isCapturing || isFinish) return;
        captureFrame();
        scheduleFrameCapture(frameCaptureMs);
    }
    else if (timerId == autoScrollTimerId) {
        win->killTimer(autoScrollTimerId);
        if (!isCapturing || isFinish || !autoScroll) return;
        dispatchAutoScroll();
        scheduleAutoScroll(autoScrollMs);
    }
}

void CapLong::firstStep()
{
    auto& maskRect = win->cutMask->maskRect;
    imgW = static_cast<int>(maskRect.right - maskRect.left);
    imgH = static_cast<int>(maskRect.bottom - maskRect.top);
    resultH = imgH;
    capStartPos.x = static_cast<int>(maskRect.left);
    capStartPos.y = static_cast<int>(maskRect.top);
    ClientToScreen(win->hwnd, &capStartPos);

    committedFrame = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    if (committedFrame.empty()) {
        setState(CaptureState::Mismatch, L"initial-capture-empty");
        return;
    }

    imgData = committedFrame;
    materializedDirty = false;
    frameRing.push_back({ committedFrame, GetTickCount64() });

    resolveScrollTargets();
    initializeUiaScroll();
    queryUiaMetrics(pendingUiaBeforePercent, pendingUiaViewSize);

    StarCapDiag::append(std::format(
        L"[long-next] init rect={}x{} target={} root={} uia={} view={:.3f} percent={:.3f}",
        imgW, imgH, windowClassName(targetHwnd), windowClassName(targetRootHwnd),
        uiaScrollPattern ? 1 : 0, pendingUiaViewSize, pendingUiaBeforePercent));

    makeImgPreview();
    setState(CaptureState::Ready, L"initialized");
    scheduleFrameCapture(frameCaptureMs);
}

void CapLong::captureFrame()
{
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
    if (data.empty()) {
        StarCapDiag::append(L"[long-next] frame-empty");
        return;
    }
    processFrame(std::move(data));
}

void CapLong::processFrame(std::vector<BYTE> data)
{
    if (data.size() != committedFrame.size() || data.empty()) return;

    frameRing.push_back({ data, GetTickCount64() });
    if (frameRing.size() > maxFrameRing) frameRing.erase(frameRing.begin());

    MatchResult match = matchFrame(committedFrame, data);
    if (match.duplicate) {
        rejectedFrames = 0;
        if (autoScroll) {
            ++noProgressFrames;
            if (noProgressFrames >= noProgressBeforeFallback) {
                noProgressFrames = 0;
                if (!advanceScrollStrategy()) pauseAuto(L"no-progress");
            }
            else if (state != CaptureState::Mismatch) setState(CaptureState::Waiting, L"no-motion-yet");
        }
        else {
            setState(acceptedFrames > 0 ? CaptureState::Confirmed : CaptureState::Ready, L"stable-or-recovered");
        }
        return;
    }

    if (!match.accepted) {
        ++rejectedFrames;
        noProgressFrames = 0;
        setState(CaptureState::Mismatch, L"match-rejected");
        StarCapDiag::append(std::format(
            L"[long-next] reject visual={:.5f} second={:.5f} pixelMad={:.2f} expected={:.1f} static={}+{} rejected={} auto={}",
            match.visualScore, match.secondScore, match.pixelMad, match.expectedOffset,
            match.staticTop, match.staticBottom, rejectedFrames, autoScroll ? 1 : 0));
        if (autoScroll && rejectedFrames >= rejectedBeforePause) {
            std::filesystem::path temp = std::filesystem::temp_directory_path() / L"StarCapLongNext";
            std::error_code ec;
            std::filesystem::create_directories(temp, ec);
            auto stamp = std::to_wstring(GetTickCount64());
            auto oldPath = (temp / (L"reject_" + stamp + L"_committed.png")).wstring();
            auto newPath = (temp / (L"reject_" + stamp + L"_candidate.png")).wstring();
            std::vector<BYTE> oldCopy = committedFrame;
            std::vector<BYTE> newCopy = data;
            Util::saveToFile(oldPath, imgW, imgH, oldCopy.data());
            Util::saveToFile(newPath, imgW, imgH, newCopy.data());
            StarCapDiag::append(std::format(L"[long-next] reject-dump old={} new={}", oldPath, newPath));
            pauseAuto(L"ambiguous-seam");
        }
        return;
    }

    rejectedFrames = 0;
    noProgressFrames = 0;
    commitFrame(data, match);
    ++acceptedFrames;
    setState(CaptureState::Confirmed, L"seam-confirmed");

    StarCapDiag::append(std::format(
        L"[long-next] accept offset={} visual={:.5f} second={:.5f} margin={:.5f} pixelMad={:.2f} expected={:.1f} structured={} static={}+{} resultH={} chunks={} bytes={}",
        match.offset, match.visualScore, match.secondScore, match.secondScore - match.visualScore,
        match.pixelMad, match.expectedOffset, match.usedStructuredPrior ? 1 : 0,
        staticTop, staticBottom, resultH, bodyChunks.size(), bufferedBytes()));
}

CapLong::MatchResult CapLong::matchFrame(const std::vector<BYTE>& oldFrame, const std::vector<BYTE>& newFrame)
{
    MatchResult result;
    if (oldFrame.size() != newFrame.size() || imgW <= 0 || imgH <= 0) return result;

    double sameRatio = sampledSameRatio(oldFrame, newFrame, imgW, imgH);
    if (sameRatio >= 0.988) {
        result.duplicate = true;
        result.visualScore = 0.0;
        result.pixelMad = 0.0;
        return result;
    }

    auto detected = detectStaticBands(oldFrame, newFrame, imgW, imgH);
    int top = slicesInitialized ? staticTop : detected.first;
    int bottom = slicesInitialized ? staticBottom : detected.second;
    int bodyTop = top;
    int bodyBottom = imgH - bottom;
    int bodySize = bodyBottom - bodyTop;
    result.staticTop = top;
    result.staticBottom = bottom;
    if (bodySize < 120) return result;

    double currentPercent = -1.0;
    double viewSize = -1.0;
    structuredExpectedOffset = 0.0;
    if (queryUiaMetrics(currentPercent, viewSize) && pendingUiaBeforePercent >= 0.0 && viewSize > 0.0 && viewSize < 100.0) {
        double maxScrollPx = imgH * (100.0 / viewSize - 1.0);
        double deltaPercent = currentPercent - pendingUiaBeforePercent;
        if (deltaPercent > 0.0001 && maxScrollPx > 0.0) {
            structuredExpectedOffset = deltaPercent * maxScrollPx / 100.0;
        }
    }
    result.expectedOffset = structuredExpectedOffset;

    auto oldFeatures = makeRowFeatures(oldFrame, imgW, imgH);
    auto newFeatures = makeRowFeatures(newFrame, imgW, imgH);

    struct Candidate {
        int offset;
        double raw;
        double adjusted;
    };
    std::vector<Candidate> candidates;

    int minOverlap = std::max(90, bodySize * 24 / 100);
    int maxOffset = std::max(1, bodySize - minOverlap);
    for (int offset = 1; offset <= maxOffset; ++offset) {
        double raw = featureScore(oldFeatures, newFeatures, imgH, offset, bodyTop, bodyBottom);
        double adjusted = raw;
        if (structuredExpectedOffset >= 4.0) {
            double distance = std::abs(offset - structuredExpectedOffset);
            double norm = distance / std::max(20.0, structuredExpectedOffset);
            adjusted += std::min(0.035, norm * 0.022);
        }
        candidates.push_back({ offset, raw, adjusted });
    }
    if (candidates.empty()) return result;

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.adjusted < b.adjusted;
    });

    const Candidate best = candidates.front();
    Candidate second{ 0, 1.0, 1.0 };
    for (const auto& candidate : candidates) {
        if (std::abs(candidate.offset - best.offset) >= 5) {
            second = candidate;
            break;
        }
    }

    result.offset = best.offset;
    result.visualScore = best.raw;
    result.secondScore = second.raw;
    result.pixelMad = pixelMadAtOffset(oldFrame, newFrame, imgW, imgH, best.offset, bodyTop, bodyBottom);

    double adjustedMargin = second.adjusted - best.adjusted;
    bool nearExact = best.raw <= 0.018 && result.pixelMad <= 7.0;
    bool robust = best.raw <= 0.055 && result.pixelMad <= 20.0 && adjustedMargin >= 0.0035;
    bool structured = false;

    if (structuredExpectedOffset >= 4.0) {
        double tolerance = std::max(12.0, structuredExpectedOffset * 0.20);
        structured = std::abs(best.offset - structuredExpectedOffset) <= tolerance
            && best.raw <= 0.080 && result.pixelMad <= 28.0 && adjustedMargin >= 0.0010;
    }

    result.accepted = nearExact || robust || structured;
    result.usedStructuredPrior = structured;

    StarCapDiag::append(std::format(
        L"[long-next] match best={} raw={:.5f} adj={:.5f} second={} raw2={:.5f} adj2={:.5f} margin={:.5f} mad={:.2f} expected={:.1f} same={:.4f} body=[{},{}] static={}+{} accept={} mode={}",
        best.offset, best.raw, best.adjusted, second.offset, second.raw, second.adjusted,
        adjustedMargin, result.pixelMad, structuredExpectedOffset, sameRatio,
        bodyTop, bodyBottom, top, bottom, result.accepted ? 1 : 0,
        nearExact ? L"exact" : (structured ? L"structured" : (robust ? L"robust" : L"reject"))));

    return result;
}

void CapLong::initializeSlices(const MatchResult& firstMatch)
{
    staticTop = std::max(0, std::min(imgH / 3, firstMatch.staticTop));
    staticBottom = std::max(0, std::min(imgH / 3, firstMatch.staticBottom));
    if (staticTop + staticBottom >= imgH - 80) {
        staticTop = 0;
        staticBottom = 0;
    }

    const int rowBytes = imgW * 4;
    headerData.clear();
    footerData.clear();
    bodyChunks.clear();

    if (staticTop > 0) {
        headerData.resize(static_cast<size_t>(rowBytes) * staticTop);
        CopyMemory(headerData.data(), committedFrame.data(), headerData.size());
    }

    int bodyRows = imgH - staticTop - staticBottom;
    ImageChunk initial;
    initial.height = bodyRows;
    initial.pixels.resize(static_cast<size_t>(rowBytes) * bodyRows);
    CopyMemory(initial.pixels.data(), committedFrame.data() + static_cast<size_t>(staticTop) * rowBytes, initial.pixels.size());
    bodyChunks.push_back(std::move(initial));
    bodyHeight = bodyRows;

    if (staticBottom > 0) {
        footerData.resize(static_cast<size_t>(rowBytes) * staticBottom);
        CopyMemory(footerData.data(), committedFrame.data() + static_cast<size_t>(imgH - staticBottom) * rowBytes, footerData.size());
    }

    slicesInitialized = true;
    resultH = staticTop + bodyHeight + staticBottom;
    invalidateMaterialized();
    StarCapDiag::append(std::format(L"[long-next] slices-init header={} body={} footer={}", staticTop, bodyHeight, staticBottom));
}

void CapLong::appendBodyRows(const std::vector<BYTE>& frame, int sourceY, int rows)
{
    if (rows <= 0 || sourceY < 0 || sourceY + rows > imgH) return;
    const int rowBytes = imgW * 4;
    ImageChunk chunk;
    chunk.height = rows;
    chunk.pixels.resize(static_cast<size_t>(rowBytes) * rows);
    CopyMemory(chunk.pixels.data(), frame.data() + static_cast<size_t>(sourceY) * rowBytes, chunk.pixels.size());
    bodyChunks.push_back(std::move(chunk));
    bodyHeight += rows;
    resultH = staticTop + bodyHeight + staticBottom;
    invalidateMaterialized();
}

void CapLong::updateFooter(const std::vector<BYTE>& frame)
{
    if (staticBottom <= 0) return;
    const int rowBytes = imgW * 4;
    footerData.resize(static_cast<size_t>(rowBytes) * staticBottom);
    CopyMemory(footerData.data(), frame.data() + static_cast<size_t>(imgH - staticBottom) * rowBytes, footerData.size());
    invalidateMaterialized();
}

void CapLong::commitFrame(const std::vector<BYTE>& data, const MatchResult& match)
{
    if (!slicesInitialized) initializeSlices(match);

    int bodyBottom = imgH - staticBottom;
    int bodySize = bodyBottom - staticTop;
    int rows = std::max(1, std::min(match.offset, bodySize));
    int sourceY = bodyBottom - rows;

    int limit = storageHeightLimit();
    if (resultH + rows > limit) {
        storageLimitReached = true;
        StarCapDiag::append(std::format(L"[long-next] storage-limit resultH={} add={} limit={} bytes={}", resultH, rows, limit, bufferedBytes()));
        stopCap(true);
        return;
    }

    appendBodyRows(data, sourceY, rows);
    updateFooter(data);
    committedFrame = data;

    double percent = -1.0;
    double view = -1.0;
    if (queryUiaMetrics(percent, view)) {
        pendingUiaBeforePercent = percent;
        pendingUiaViewSize = view;
    }

    makeImgPreview();
    win->refresh();
}

void CapLong::invalidateMaterialized()
{
    materializedDirty = true;
    imgData.clear();
}

size_t CapLong::bufferedBytes() const
{
    size_t total = headerData.size() + footerData.size();
    for (const auto& chunk : bodyChunks) total += chunk.pixels.size();
    if (!slicesInitialized) total += committedFrame.size();
    return total;
}

int CapLong::storageHeightLimit() const
{
    if (imgW <= 0) return 0;
    size_t rowBytes = static_cast<size_t>(imgW) * 4;
    size_t byMemory = rowBytes > 0 ? maxLongBufferBytes / rowBytes : 0;
    return static_cast<int>(std::min<size_t>(maxLongResultHeight, byMemory));
}

const BYTE* CapLong::logicalRowPtr(int y) const
{
    if (y < 0 || y >= resultH) return nullptr;
    const int rowBytes = imgW * 4;

    if (!slicesInitialized) {
        if (committedFrame.empty() || y >= imgH) return nullptr;
        return committedFrame.data() + static_cast<size_t>(y) * rowBytes;
    }

    if (y < staticTop) return headerData.data() + static_cast<size_t>(y) * rowBytes;
    y -= staticTop;
    if (y < bodyHeight) {
        for (const auto& chunk : bodyChunks) {
            if (y < chunk.height) return chunk.pixels.data() + static_cast<size_t>(y) * rowBytes;
            y -= chunk.height;
        }
        return nullptr;
    }
    y -= bodyHeight;
    if (y < staticBottom) return footerData.data() + static_cast<size_t>(y) * rowBytes;
    return nullptr;
}

bool CapLong::ensureMaterialized()
{
    if (!materializedDirty && !imgData.empty()) return true;
    if (imgW <= 0 || resultH <= 0) return false;

    const size_t rowBytes = static_cast<size_t>(imgW) * 4;
    try {
        std::vector<BYTE> data(rowBytes * static_cast<size_t>(resultH));
        for (int y = 0; y < resultH; ++y) {
            const BYTE* src = logicalRowPtr(y);
            if (!src) return false;
            CopyMemory(data.data() + static_cast<size_t>(y) * rowBytes, src, rowBytes);
        }
        imgData = std::move(data);
        materializedDirty = false;
        StarCapDiag::append(std::format(L"[long-next] materialize image={}x{} bytes={}", imgW, resultH, imgData.size()));
        return true;
    }
    catch (const std::bad_alloc&) {
        StarCapDiag::append(std::format(L"[long-next] materialize-oom image={}x{}", imgW, resultH));
        return false;
    }
}

void CapLong::makeImgPreview()
{
    imgPreview.Reset();
    if (!tool || imgW <= 0 || resultH <= 0) return;

    float scale = static_cast<float>(tool->w) / static_cast<float>(imgW);
    int previewW = std::max(1, static_cast<int>(imgW * scale));
    int totalPreviewH = std::max(1, static_cast<int>(resultH * scale));
    int maxPreviewH = std::max(120, static_cast<int>(std::min(520.0f * win->dpi, win->h * 0.62f)));
    int previewH = std::min(totalPreviewH, maxPreviewH);
    int srcStartY = 0;
    if (totalPreviewH > previewH) {
        int visibleSourceRows = std::max(1, static_cast<int>(previewH / scale));
        srcStartY = std::max(0, resultH - visibleSourceRows);
    }

    std::vector<BYTE> scaled(static_cast<size_t>(previewW) * previewH * 4);
    for (int y = 0; y < previewH; ++y) {
        int srcY = srcStartY + static_cast<int>(y / scale);
        srcY = std::min(resultH - 1, srcY);
        const BYTE* srcRow = logicalRowPtr(srcY);
        if (!srcRow) continue;
        for (int x = 0; x < previewW; ++x) {
            int srcX = std::min(imgW - 1, static_cast<int>(x / scale));
            size_t si = static_cast<size_t>(srcX) * 4;
            size_t di = (static_cast<size_t>(y) * previewW + x) * 4;
            scaled[di] = srcRow[si];
            scaled[di + 1] = srcRow[si + 1];
            scaled[di + 2] = srcRow[si + 2];
            scaled[di + 3] = srcRow[si + 3];
        }
    }

    D2D1_BITMAP_PROPERTIES1 props = {
        .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)},
        .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(previewW, previewH), scaled.data(), previewW * 4, props, imgPreview.GetAddressOf());
}

void CapLong::makeTool()
{
    tool = std::make_unique<ToolLong>(win, this);
    layoutTool();
    tool->createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

void CapLong::layoutTool()
{
    if (!tool) return;
    auto& r = win->cutMask->maskRect;
    int gap = std::max(2, static_cast<int>(2 * win->dpi));
    int toolW = static_cast<int>(tool->w);
    int toolH = static_cast<int>(tool->h);
    int winW = static_cast<int>(win->w);
    int winH = static_cast<int>(win->h);

    int rightX = static_cast<int>(r.right) + gap;
    int leftX = static_cast<int>(r.left) - toolW - gap;
    int x = 0;
    if (rightX + toolW <= winW) x = rightX;
    else if (leftX >= 0) x = leftX;
    else x = std::max(0, std::min(winW - toolW, static_cast<int>(r.right) - toolW - gap));

    int y = static_cast<int>(r.bottom) - toolH;
    y = std::max(0, std::min(winH - toolH, y));

    POINT pos{ x, y };
    ClientToScreen(win->hwnd, &pos);
    tool->setPosition(pos.x, pos.y);
}

void CapLong::paintImgPreview(ID2D1DeviceContext* ctx)
{
    if (!imgPreview || !tool) return;
    auto bitmapSize = imgPreview->GetPixelSize();
    float drawW = static_cast<float>(bitmapSize.width);
    float drawH = static_cast<float>(bitmapSize.height);
    POINT pos{ tool->x, tool->y - static_cast<int>(drawH) - static_cast<int>(2 * win->dpi) };
    ScreenToClient(win->hwnd, &pos);
    if (pos.y < 0) pos.y = 0;
    D2D1_RECT_F destRect = D2D1::RectF(static_cast<float>(pos.x), static_cast<float>(pos.y), pos.x + drawW, pos.y + drawH);
    ctx->DrawBitmap(imgPreview.Get(), destRect);
}

void CapLong::setState(CaptureState newState, const wchar_t* reason)
{
    if (state == newState && !reason) return;
    state = newState;
    if (tool) {
        switch (state) {
        case CaptureState::Ready: tool->setCaptureStatus(L"○"); break;
        case CaptureState::Confirmed: tool->setCaptureStatus(L"✓"); break;
        case CaptureState::Waiting: tool->setCaptureStatus(L"…"); break;
        case CaptureState::Mismatch: tool->setCaptureStatus(L"!"); break;
        case CaptureState::Paused: tool->setCaptureStatus(L"Ⅱ"); break;
        }
    }
    if (reason) StarCapDiag::append(std::format(L"[long-next] state={} reason={}", static_cast<int>(state), reason));
}

void CapLong::resolveScrollTargets()
{
    auto& r = win->cutMask->maskRect;
    autoTargetPoint = { static_cast<LONG>((r.left + r.right) / 2.0f), static_cast<LONG>((r.top + r.bottom) / 2.0f) };
    ClientToScreen(win->hwnd, &autoTargetPoint);
    targetHwnd = WindowFromPoint(autoTargetPoint);
    targetRootHwnd = targetHwnd ? GetAncestor(targetHwnd, GA_ROOT) : nullptr;
}

bool CapLong::initializeUiaScroll()
{
    releaseUiaScroll();

    ComPtr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (FAILED(hr) || !automation) {
        scrollStrategy = ScrollStrategy::ChildWheel;
        StarCapDiag::append(std::format(L"[long-next] uia-create-failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    ComPtr<IUIAutomationElement> element;
    hr = automation->ElementFromPoint(autoTargetPoint, &element);
    if (FAILED(hr) || !element) {
        scrollStrategy = ScrollStrategy::ChildWheel;
        return false;
    }

    ComPtr<IUIAutomationTreeWalker> walker;
    automation->get_ControlViewWalker(&walker);
    for (int depth = 0; element && depth < 12; ++depth) {
        ComPtr<IUnknown> unknownPattern;
        if (SUCCEEDED(element->GetCurrentPattern(UIA_ScrollPatternId, &unknownPattern)) && unknownPattern) {
            ComPtr<IUIAutomationScrollPattern> pattern;
            if (SUCCEEDED(unknownPattern.As(&pattern)) && pattern) {
                BOOL verticallyScrollable = FALSE;
                if (SUCCEEDED(pattern->get_CurrentVerticallyScrollable(&verticallyScrollable)) && verticallyScrollable) {
                    ComPtr<IUnknown> automationUnknown;
                    automation.As(&automationUnknown);
                    uiaAutomation = automationUnknown;
                    uiaScrollPattern = unknownPattern;
                    scrollStrategy = ScrollStrategy::Uia;
                    StarCapDiag::append(std::format(L"[long-next] uia-scroll-found depth={}", depth));
                    return true;
                }
            }
        }

        if (!walker) break;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(element.Get(), &parent)) || !parent) break;
        element = parent;
    }

    scrollStrategy = ScrollStrategy::ChildWheel;
    StarCapDiag::append(L"[long-next] uia-scroll-unavailable fallback=child-wheel");
    return false;
}

void CapLong::releaseUiaScroll()
{
    uiaScrollPattern.Reset();
    uiaAutomation.Reset();
    pendingUiaBeforePercent = -1.0;
    pendingUiaViewSize = -1.0;
    structuredExpectedOffset = 0.0;
}

bool CapLong::queryUiaMetrics(double& verticalPercent, double& verticalViewSize) const
{
    verticalPercent = -1.0;
    verticalViewSize = -1.0;
    if (!uiaScrollPattern) return false;
    ComPtr<IUIAutomationScrollPattern> pattern;
    if (FAILED(uiaScrollPattern.As(&pattern)) || !pattern) return false;
    BOOL verticallyScrollable = FALSE;
    if (FAILED(pattern->get_CurrentVerticallyScrollable(&verticallyScrollable)) || !verticallyScrollable) return false;
    if (FAILED(pattern->get_CurrentVerticalScrollPercent(&verticalPercent))) return false;
    if (FAILED(pattern->get_CurrentVerticalViewSize(&verticalViewSize))) return false;
    return verticalPercent >= 0.0 && verticalViewSize > 0.0;
}

bool CapLong::dispatchUiaScroll()
{
    if (!uiaScrollPattern) return false;
    ComPtr<IUIAutomationScrollPattern> pattern;
    if (FAILED(uiaScrollPattern.As(&pattern)) || !pattern) return false;

    double percent = -1.0;
    double viewSize = -1.0;
    if (!queryUiaMetrics(percent, viewSize)) return false;

    HRESULT hr = E_FAIL;
    double desiredPx = std::max(42.0, std::min(82.0, imgH * 0.09));
    if (viewSize > 0.0 && viewSize < 100.0) {
        double maxScrollPx = imgH * (100.0 / viewSize - 1.0);
        if (maxScrollPx > 1.0) {
            double deltaPercent = desiredPx / maxScrollPx * 100.0;
            double target = std::min(100.0, percent + std::max(0.05, deltaPercent));
            hr = pattern->SetScrollPercent(-1.0, target);
            StarCapDiag::append(std::format(L"[long-next] auto-uia seq={} from={:.4f} target={:.4f} view={:.4f} desiredPx={:.1f} hr=0x{:08X}",
                scrollSequence, percent, target, viewSize, desiredPx, static_cast<unsigned>(hr)));
        }
    }

    if (FAILED(hr)) {
        hr = pattern->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement);
        StarCapDiag::append(std::format(L"[long-next] auto-uia-small seq={} hr=0x{:08X}", scrollSequence, static_cast<unsigned>(hr)));
    }
    return SUCCEEDED(hr);
}

bool CapLong::dispatchWheelScroll()
{
    const SHORT delta = static_cast<SHORT>(-WHEEL_DELTA / 2);
    WPARAM wheelParam = MAKEWPARAM(0, static_cast<WORD>(delta));
    LPARAM pointParam = MAKELPARAM(static_cast<SHORT>(autoTargetPoint.x), static_cast<SHORT>(autoTargetPoint.y));

    if (scrollStrategy == ScrollStrategy::ChildWheel && targetHwnd && IsWindow(targetHwnd)) {
        BOOL ok = PostMessageW(targetHwnd, WM_MOUSEWHEEL, wheelParam, pointParam);
        StarCapDiag::append(std::format(L"[long-next] auto-wheel seq={} strategy=child ok={}", scrollSequence, ok ? 1 : 0));
        return ok != FALSE;
    }
    if (scrollStrategy == ScrollStrategy::RootWheel && targetRootHwnd && IsWindow(targetRootHwnd)) {
        BOOL ok = PostMessageW(targetRootHwnd, WM_MOUSEWHEEL, wheelParam, pointParam);
        StarCapDiag::append(std::format(L"[long-next] auto-wheel seq={} strategy=root ok={}", scrollSequence, ok ? 1 : 0));
        return ok != FALSE;
    }

    POINT oldPos{};
    GetCursorPos(&oldPos);
    SetCursorPos(autoTargetPoint.x, autoTargetPoint.y);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(delta));
    UINT sent = SendInput(1, &input, sizeof(INPUT));
    SetCursorPos(oldPos.x, oldPos.y);
    StarCapDiag::append(std::format(L"[long-next] auto-wheel seq={} strategy=sendinput sent={}", scrollSequence, sent));
    return sent == 1;
}

void CapLong::dispatchAutoScroll()
{
    if (!autoScroll || !isCapturing || isFinish) return;
    ++scrollSequence;
    setState(CaptureState::Waiting, L"auto-scroll");

    bool dispatched = false;
    if (scrollStrategy == ScrollStrategy::Uia) dispatched = dispatchUiaScroll();
    else dispatched = dispatchWheelScroll();

    if (!dispatched) {
        StarCapDiag::append(std::format(L"[long-next] auto-dispatch-failed strategy={}", static_cast<int>(scrollStrategy)));
        if (!advanceScrollStrategy()) pauseAuto(L"scroll-driver-unavailable");
    }
}

bool CapLong::advanceScrollStrategy()
{
    ScrollStrategy old = scrollStrategy;
    switch (scrollStrategy) {
    case ScrollStrategy::Uia: scrollStrategy = ScrollStrategy::ChildWheel; break;
    case ScrollStrategy::ChildWheel: scrollStrategy = ScrollStrategy::RootWheel; break;
    case ScrollStrategy::RootWheel: scrollStrategy = ScrollStrategy::SendInput; break;
    case ScrollStrategy::SendInput: return false;
    }
    rejectedFrames = 0;
    noProgressFrames = 0;
    StarCapDiag::append(std::format(L"[long-next] auto-strategy {}->{}", static_cast<int>(old), static_cast<int>(scrollStrategy)));
    return true;
}

void CapLong::pauseAuto(const wchar_t* reason)
{
    if (!autoScroll) return;
    autoScroll = false;
    win->killTimer(autoScrollTimerId);
    if (tool) tool->setAutoRunning(false);
    setState(CaptureState::Paused, reason);
    StarCapDiag::append(std::format(L"[long-next] auto-paused reason={} resultH={} accepted={} rejected={} strategy={}",
        reason ? reason : L"?", resultH, acceptedFrames, rejectedFrames, static_cast<int>(scrollStrategy)));
}

void CapLong::startAutoScroll()
{
    if (!isCapturing || isFinish || autoScroll) return;
    resolveScrollTargets();
    if (!uiaScrollPattern) initializeUiaScroll();
    if (!uiaScrollPattern && scrollStrategy == ScrollStrategy::Uia) scrollStrategy = ScrollStrategy::ChildWheel;
    autoScroll = true;
    rejectedFrames = 0;
    noProgressFrames = 0;
    if (tool) tool->setAutoRunning(true);
    setState(CaptureState::Waiting, L"auto-start");
    scheduleAutoScroll(20);
}

void CapLong::toggleAutoScroll()
{
    if (!isCapturing || isFinish) return;
    if (autoScroll) pauseAuto(L"user-pause");
    else startAutoScroll();
}

void CapLong::installControlHook()
{
    gLongCapture = this;
    gSpaceDown = gEnterDown = gEscapeDown = false;
    if (!gLongCaptureHook) {
        gLongCaptureHook = SetWindowsHookExW(WH_KEYBOARD_LL, longCaptureKeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!gLongCaptureHook) StarCapDiag::append(std::format(L"[long-next] control-hook-failed error={}", GetLastError()));
    }
}

void CapLong::uninstallControlHook()
{
    if (gLongCapture == this) gLongCapture = nullptr;
    if (gLongCaptureHook) {
        UnhookWindowsHookEx(gLongCaptureHook);
        gLongCaptureHook = nullptr;
    }
    gSpaceDown = gEnterDown = gEscapeDown = false;
}

void CapLong::hotkeyEnter()
{
    if (!isCapturing || isFinish) return;
    StarCapDiag::append(L"[long-next] control=enter-copy");
    copyToClipboard();
    win->close();
}

void CapLong::hotkeyEscape()
{
    if (isFinish) return;
    StarCapDiag::append(L"[long-next] control=escape");
    win->close();
}

void CapLong::stopCap(bool showMessage)
{
    if (isFinish) return;
    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
    win->killTimer(autoScrollTimerId);
    win->killTimer(frameCaptureTimerId);
    uninstallControlHook();
    releaseUiaScroll();
    if (showMessage) makeStopText();
    win->restoreWin();
    win->refresh();
}

void CapLong::makeStopText()
{
    layoutTextEnd = Ling::D2D::get()->makeTextLayout(Lang::get(L"long.tooLong"), 13 * win->dpi);
    if (!layoutTextEnd) return;
    DWRITE_TEXT_METRICS tm{};
    layoutTextEnd->GetMetrics(&tm);
    auto& maskRect = win->cutMask->maskRect;
    auto halfX = maskRect.left + (maskRect.right - maskRect.left) / 2;
    auto halfW = tm.width / 2;
    float padding{ 8 * win->dpi };
    stopTextRect.left = halfX - halfW - padding;
    stopTextRect.top = maskRect.bottom - 30 * win->dpi - padding;
    stopTextRect.right = halfX + halfW + padding;
    stopTextRect.bottom = maskRect.bottom - padding;
    layoutTextEnd->SetMaxWidth(stopTextRect.right - stopTextRect.left);
    layoutTextEnd->SetMaxHeight(stopTextRect.bottom - stopTextRect.top);
    stopTextPos = { halfX - halfW, stopTextRect.top + (stopTextRect.bottom - stopTextRect.top - tm.height) / 2 };
}

void CapLong::copyToClipboard()
{
    if (!ensureMaterialized()) return;
    if (!isFinish) stopCap(false);
    Util::saveToClipboard(imgW, resultH, imgData.data());
}

bool CapLong::saveToFile()
{
    if (!ensureMaterialized()) return false;
    if (!isFinish) stopCap(false);
    auto path = Util::getSaveFilePath(win->hwnd);
    if (path.empty()) return false;
    return Util::saveToFile(path, imgW, resultH, imgData.data());
}

bool CapLong::ocr()
{
    if (!ensureMaterialized()) return false;
    if (!isFinish) stopCap(false);
    auto data = imgData;
    StarCapOcr::showPixels(std::move(data), imgW, resultH, true);
    return true;
}

bool CapLong::translate()
{
    if (!ensureMaterialized()) return false;
    if (!isFinish) stopCap(false);
    auto data = imgData;
    StarCapOcr::showTranslationPixels(std::move(data), imgW, resultH, true);
    return true;
}

void CapLong::pin()
{
    if (!ensureMaterialized()) return;
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
