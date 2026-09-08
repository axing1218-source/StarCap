from pathlib import Path

CPP = Path('Src/Win/CapLong.cpp')
HDR = Path('Src/Win/CapLong.h')


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly one match, got {count}')
    return text.replace(old, new, 1)


h = HDR.read_text(encoding='utf-8-sig')
h = replace_once(h,
'''    bool advanceScrollStrategy();
    void pauseAuto(const wchar_t* reason);
''',
'''    bool advanceScrollStrategy();
    void resetAutoStep();
    void pauseAuto(const wchar_t* reason, bool hardPause = false);
''', 'header helpers')
h = replace_once(h,
'''    bool autoScroll{ false };
    bool slicesInitialized{ false };
''',
'''    bool autoScroll{ false };
    bool hardPaused{ false };
    bool autoStepPending{ false };
    bool slicesInitialized{ false };
''', 'header bool state')
h = replace_once(h,
'''    int acceptedFrames{ 0 };
    int scrollSequence{ 0 };
''',
'''    int acceptedFrames{ 0 };
    int scrollSequence{ 0 };
    int autoStepFrames{ 0 };
''', 'header step frames')
HDR.write_text(h, encoding='utf-8')

c = CPP.read_text(encoding='utf-8-sig')

c = replace_once(c,
'''void CapLong::scheduleFrameCapture(int delayMs)
{
    if (!isCapturing || isFinish) return;
    win->setTimer(delayMs, frameCaptureTimerId);
}
''',
'''void CapLong::scheduleFrameCapture(int delayMs)
{
    if (!isCapturing || isFinish || hardPaused) return;
    win->setTimer(delayMs, frameCaptureTimerId);
}
''', 'schedule frame hard pause')

c = replace_once(c,
'''void CapLong::onTimerCB(UINT timerId)
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
''',
'''void CapLong::onTimerCB(UINT timerId)
{
    if (timerId == frameCaptureTimerId) {
        win->killTimer(frameCaptureTimerId);
        if (!isCapturing || isFinish || hardPaused) return;
        captureFrame();
        scheduleFrameCapture(frameCaptureMs);
    }
    else if (timerId == autoScrollTimerId) {
        win->killTimer(autoScrollTimerId);
        if (!isCapturing || isFinish || !autoScroll || hardPaused || autoStepPending) return;
        dispatchAutoScroll();
    }
}
''', 'timer serialization')

c = replace_once(c,
'''    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
''',
'''    autoScroll = false;
    hardPaused = false;
    resetAutoStep();
    if (tool) tool->setAutoRunning(false);
''', 'restart state reset')

c = replace_once(c,
'''void CapLong::captureFrame()
{
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
''',
'''void CapLong::captureFrame()
{
    if (hardPaused) return;
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
''', 'capture hard pause')

c = replace_once(c,
'''void CapLong::processFrame(std::vector<BYTE> data)
{
    if (data.size() != committedFrame.size() || data.empty()) return;

    frameRing.push_back({ data, GetTickCount64() });
    if (frameRing.size() > maxFrameRing) frameRing.erase(frameRing.begin());

    MatchResult match = matchFrame(committedFrame, data);
''',
'''void CapLong::processFrame(std::vector<BYTE> data)
{
    if (hardPaused || data.size() != committedFrame.size() || data.empty()) return;

    frameRing.push_back({ data, GetTickCount64() });
    if (frameRing.size() > maxFrameRing) frameRing.erase(frameRing.begin());

    // v12 keeps the proven v10 matcher, but serializes automatic scrolling without the
    // v11 whole-frame motion gate. One wheel is allowed in flight at a time. Give the
    // target a short settle window, then let the normal matcher decide the seam.
    if (autoScroll && autoStepPending) {
        ++autoStepFrames;
        if (autoStepFrames < 3) return;
    }

    MatchResult match = matchFrame(committedFrame, data);
''', 'process prefix')

c = replace_once(c,
'''    if (match.duplicate) {
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
''',
'''    if (match.duplicate) {
        rejectedFrames = 0;
        if (autoScroll) {
            if (autoStepPending && autoStepFrames >= 8) {
                resetAutoStep();
                ++noProgressFrames;
                StarCapDiag::append(std::format(L"[long-next] auto-step-no-motion noProgress={}", noProgressFrames));
                if (noProgressFrames >= noProgressBeforeFallback) {
                    noProgressFrames = 0;
                    if (!advanceScrollStrategy()) pauseAuto(L"no-progress");
                    else scheduleAutoScroll(55);
                }
                else scheduleAutoScroll(55);
            }
            else if (state != CaptureState::Mismatch) {
                setState(CaptureState::Waiting, L"no-motion-yet");
            }
        }
        else {
            setState(acceptedFrames > 0 ? CaptureState::Confirmed : CaptureState::Ready, L"stable-or-recovered");
        }
        return;
    }

    if (match.accepted && match.offset > 0 && match.offset < 4 && match.expectedOffset < 3.0) {
        StarCapDiag::append(std::format(L"[long-next] micro-motion-ignored offset={} score={:.5f} mad={:.2f} auto={}",
            match.offset, match.visualScore, match.pixelMad, autoScroll ? 1 : 0));
        if (autoScroll && autoStepPending && autoStepFrames >= 8) {
            resetAutoStep();
            ++noProgressFrames;
            scheduleAutoScroll(55);
        }
        return;
    }

    if (!match.accepted) {
''', 'duplicate and micro motion')

c = replace_once(c,
'''        if (autoScroll && rejectedFrames >= rejectedBeforePause) {
            std::filesystem::path temp = std::filesystem::temp_directory_path() / L"StarCapLongNext";
''',
'''        if (autoScroll && autoStepPending && autoStepFrames < 9) {
            return;
        }
        if (autoScroll && rejectedFrames >= rejectedBeforePause) {
            std::filesystem::path temp = std::filesystem::temp_directory_path() / L"StarCapLongNext";
''', 'reject settle retry')

c = replace_once(c,
'''    commitFrame(data, match);
    ++acceptedFrames;
    setState(CaptureState::Confirmed, L"seam-confirmed");
''',
'''    commitFrame(data, match);
    ++acceptedFrames;
    if (autoScroll) {
        resetAutoStep();
        scheduleAutoScroll(45);
    }
    setState(CaptureState::Confirmed, L"seam-confirmed");
''', 'commit schedules next step')

c = replace_once(c,
'''void CapLong::dispatchAutoScroll()
{
    if (!autoScroll || !isCapturing || isFinish) return;
    ++scrollSequence;
''',
'''void CapLong::dispatchAutoScroll()
{
    if (!autoScroll || !isCapturing || isFinish || hardPaused || autoStepPending) return;
    ++scrollSequence;
''', 'dispatch guard')

c = replace_once(c,
'''    if (!dispatched) {
        StarCapDiag::append(std::format(L"[long-next] auto-dispatch-failed strategy={}", static_cast<int>(scrollStrategy)));
        if (!advanceScrollStrategy()) pauseAuto(L"scroll-driver-unavailable");
    }
}

bool CapLong::advanceScrollStrategy()
''',
'''    if (!dispatched) {
        StarCapDiag::append(std::format(L"[long-next] auto-dispatch-failed strategy={}", static_cast<int>(scrollStrategy)));
        if (!advanceScrollStrategy()) pauseAuto(L"scroll-driver-unavailable");
        else scheduleAutoScroll(55);
        return;
    }

    autoStepPending = true;
    autoStepFrames = 0;
    StarCapDiag::append(std::format(L"[long-next] auto-step-begin seq={} strategy={}",
        scrollSequence, static_cast<int>(scrollStrategy)));
}

bool CapLong::advanceScrollStrategy()
''', 'dispatch step begin')

c = replace_once(c,
'''void CapLong::pauseAuto(const wchar_t* reason)
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
''',
'''void CapLong::resetAutoStep()
{
    autoStepPending = false;
    autoStepFrames = 0;
}

void CapLong::pauseAuto(const wchar_t* reason, bool hardPause)
{
    if (!autoScroll && !hardPaused) return;
    autoScroll = false;
    resetAutoStep();
    win->killTimer(autoScrollTimerId);
    if (hardPause) {
        hardPaused = true;
        win->killTimer(frameCaptureTimerId);
    }
    if (tool) tool->setAutoRunning(false);
    setState(CaptureState::Paused, reason);
    StarCapDiag::append(std::format(L"[long-next] auto-paused reason={} hard={} resultH={} accepted={} rejected={} strategy={}",
        reason ? reason : L"?", hardPause ? 1 : 0, resultH, acceptedFrames, rejectedFrames, static_cast<int>(scrollStrategy)));
}

void CapLong::startAutoScroll()
{
    if (!isCapturing || isFinish || autoScroll) return;
    if (hardPaused) {
        hardPaused = false;
        frameRing.clear();
        scheduleFrameCapture(15);
        StarCapDiag::append(std::format(L"[long-next] hard-pause-resume resultH={} accepted={}", resultH, acceptedFrames));
    }
''', 'pause/start hard pause')

c = replace_once(c,
'''    autoScroll = true;
    rejectedFrames = 0;
    noProgressFrames = 0;
''',
'''    autoScroll = true;
    resetAutoStep();
    rejectedFrames = 0;
    noProgressFrames = 0;
''', 'start step reset')

c = replace_once(c,
'''    if (!isCapturing || isFinish) return;
    if (autoScroll) pauseAuto(L"user-pause");
    else startAutoScroll();
''',
'''    if (!isCapturing || isFinish) return;
    if (autoScroll) pauseAuto(L"user-pause", true);
    else startAutoScroll();
''', 'toggle hard pause')

c = replace_once(c,
'''    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
''',
'''    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    hardPaused = false;
    resetAutoStep();
    if (tool) tool->setAutoRunning(false);
''', 'stop state reset')

CPP.write_text(c, encoding='utf-8')
print('Long Capture Next v12 patch applied.')
