from pathlib import Path

CPP = Path('Src/Win/CapLong.cpp')
HDR = Path('Src/Win/CapLong.h')


def one(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f'{label}: expected 1 match, got {n}')
    return text.replace(old, new, 1)


def first(text, old, new, label):
    if old not in text:
        raise RuntimeError(f'{label}: match not found')
    return text.replace(old, new, 1)


h = HDR.read_text(encoding='utf-8-sig')
h = one(h, '    bool advanceScrollStrategy();\n    void pauseAuto(const wchar_t* reason);\n',
        '    bool advanceScrollStrategy();\n    void resetAutoStep();\n    void pauseAuto(const wchar_t* reason, bool hardPause = false);\n', 'header helpers')
h = one(h, '    bool autoScroll{ false };\n    bool slicesInitialized{ false };\n',
        '    bool autoScroll{ false };\n    bool hardPaused{ false };\n    bool autoStepPending{ false };\n    bool slicesInitialized{ false };\n', 'header bools')
h = one(h, '    int acceptedFrames{ 0 };\n    int scrollSequence{ 0 };\n',
        '    int acceptedFrames{ 0 };\n    int scrollSequence{ 0 };\n    int autoStepFrames{ 0 };\n', 'header counters')
HDR.write_text(h, encoding='utf-8')

c = CPP.read_text(encoding='utf-8-sig')

c = one(c,
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
''', 'scheduleFrameCapture')

c = one(c,
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
''', 'onTimerCB')

c = one(c,
'''void CapLong::restartForCurrentRect()
{
    win->killTimer(autoScrollTimerId);
    win->killTimer(frameCaptureTimerId);
    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
''',
'''void CapLong::restartForCurrentRect()
{
    win->killTimer(autoScrollTimerId);
    win->killTimer(frameCaptureTimerId);
    autoScroll = false;
    hardPaused = false;
    resetAutoStep();
    if (tool) tool->setAutoRunning(false);
''', 'restart reset')

c = one(c,
'''void CapLong::captureFrame()
{
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
''',
'''void CapLong::captureFrame()
{
    if (hardPaused) return;
    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);
''', 'captureFrame')

c = one(c,
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

    // v12: keep the v10 matcher, but only allow one automatic wheel in flight.
    // We intentionally do not use v11's whole-frame motion gate because sparse chat
    // windows can have a large fixed wallpaper even while the message column scrolls.
    if (autoScroll && autoStepPending) {
        ++autoStepFrames;
        if (autoStepFrames < 3) return; // roughly 135 ms settle window
    }

    MatchResult match = matchFrame(committedFrame, data);
''', 'process prefix')

c = one(c,
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
        ++rejectedFrames;
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
        // Stay on the same scrolled view for a few samples instead of issuing another wheel.
        // This prevents gaps while still allowing late paint/animation to settle.
        if (autoScroll && autoStepPending && autoStepFrames < 9) {
            setState(CaptureState::Waiting, L"settling-retry");
            return;
        }
        ++rejectedFrames;
''', 'process duplicate/reject')

c = one(c,
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
''', 'commit next step')

c = one(c,
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

c = one(c,
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
''', 'dispatch step')

c = one(c,
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
''', 'pause/start')

c = one(c,
'''    autoScroll = true;
    rejectedFrames = 0;
    noProgressFrames = 0;
''',
'''    autoScroll = true;
    resetAutoStep();
    rejectedFrames = 0;
    noProgressFrames = 0;
''', 'start reset')

c = one(c,
'''void CapLong::toggleAutoScroll()
{
    if (!isCapturing || isFinish) return;
    if (autoScroll) pauseAuto(L"user-pause");
    else startAutoScroll();
}
''',
'''void CapLong::toggleAutoScroll()
{
    if (!isCapturing || isFinish) return;
    if (autoScroll) pauseAuto(L"user-pause", true);
    else startAutoScroll();
}
''', 'toggle')

c = one(c,
'''void CapLong::stopCap(bool showMessage)
{
    if (isFinish) return;
    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    if (tool) tool->setAutoRunning(false);
''',
'''void CapLong::stopCap(bool showMessage)
{
    if (isFinish) return;
    isFinish = true;
    isCapturing = false;
    autoScroll = false;
    hardPaused = false;
    resetAutoStep();
    if (tool) tool->setAutoRunning(false);
''', 'stop')

CPP.write_text(c, encoding='utf-8')
print('Long Capture Next v12b patch applied.')
