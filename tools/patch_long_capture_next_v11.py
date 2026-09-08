from pathlib import Path

cpp_path = Path('Src/Win/CapLong.cpp')
h_path = Path('Src/Win/CapLong.h')
cpp = cpp_path.read_text(encoding='utf-8-sig')
h = h_path.read_text(encoding='utf-8-sig')

h = h.replace(
'''    void pauseAuto(const wchar_t* reason);\n''',
'''    void pauseAuto(const wchar_t* reason, bool hardPause = false);\n    void resetAutoStepState();\n''')

h = h.replace(
'''    bool storageLimitReached{ false };\n    bool resizingSelection{ false };\n''',
'''    bool storageLimitReached{ false };\n    bool resizingSelection{ false };\n    bool hardPaused{ false };\n    bool autoStepPending{ false };\n    bool autoStepSawMotion{ false };\n''')

h = h.replace(
'''    int acceptedFrames{ 0 };\n    int scrollSequence{ 0 };\n''',
'''    int acceptedFrames{ 0 };\n    int scrollSequence{ 0 };\n    int autoStepFrames{ 0 };\n    int autoStableFrames{ 0 };\n''')

h = h.replace(
'''    std::vector<Frame> frameRing;\n    std::vector<BYTE> committedFrame;\n''',
'''    std::vector<Frame> frameRing;\n    std::vector<BYTE> committedFrame;\n    std::vector<BYTE> autoLastObservedFrame;\n''')

cpp = cpp.replace(
'''void CapLong::scheduleFrameCapture(int delayMs)\n{\n    if (!isCapturing || isFinish) return;\n    win->setTimer(delayMs, frameCaptureTimerId);\n}\n''',
'''void CapLong::scheduleFrameCapture(int delayMs)\n{\n    if (!isCapturing || isFinish || hardPaused) return;\n    win->setTimer(delayMs, frameCaptureTimerId);\n}\n''')

cpp = cpp.replace(
'''    if (timerId == frameCaptureTimerId) {\n        win->killTimer(frameCaptureTimerId);\n        if (!isCapturing || isFinish) return;\n        captureFrame();\n        scheduleFrameCapture(frameCaptureMs);\n    }\n    else if (timerId == autoScrollTimerId) {\n        win->killTimer(autoScrollTimerId);\n        if (!isCapturing || isFinish || !autoScroll) return;\n        dispatchAutoScroll();\n        scheduleAutoScroll(autoScrollMs);\n    }\n''',
'''    if (timerId == frameCaptureTimerId) {\n        win->killTimer(frameCaptureTimerId);\n        if (!isCapturing || isFinish || hardPaused) return;\n        captureFrame();\n        scheduleFrameCapture(frameCaptureMs);\n    }\n    else if (timerId == autoScrollTimerId) {\n        win->killTimer(autoScrollTimerId);\n        if (!isCapturing || isFinish || !autoScroll || hardPaused || autoStepPending) return;\n        dispatchAutoScroll();\n    }\n''')

cpp = cpp.replace(
'''    autoScroll = false;\n    if (tool) tool->setAutoRunning(false);\n    releaseUiaScroll();\n''',
'''    autoScroll = false;\n    hardPaused = false;\n    resetAutoStepState();\n    if (tool) tool->setAutoRunning(false);\n    releaseUiaScroll();\n''', 1)

cpp = cpp.replace(
'''void CapLong::captureFrame()\n{\n    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);\n''',
'''void CapLong::captureFrame()\n{\n    if (hardPaused) return;\n    auto data = Util::captureScreen(capStartPos.x, capStartPos.y, imgW, imgH);\n''')

old_process = '''void CapLong::processFrame(std::vector<BYTE> data)\n{\n    if (data.size() != committedFrame.size() || data.empty()) return;\n\n    frameRing.push_back({ data, GetTickCount64() });\n    if (frameRing.size() > maxFrameRing) frameRing.erase(frameRing.begin());\n\n    MatchResult match = matchFrame(committedFrame, data);\n'''
new_process = '''void CapLong::processFrame(std::vector<BYTE> data)\n{\n    if (hardPaused || data.size() != committedFrame.size() || data.empty()) return;\n\n    frameRing.push_back({ data, GetTickCount64() });\n    if (frameRing.size() > maxFrameRing) frameRing.erase(frameRing.begin());\n\n    // Serialize automatic capture: one wheel -> wait for visible motion -> wait until the\n    // candidate stops changing -> match/commit once -> only then issue the next wheel.\n    if (autoScroll && autoStepPending) {\n        ++autoStepFrames;\n        double committedSame = sampledSameRatio(committedFrame, data, imgW, imgH);\n        if (committedSame < 0.992) autoStepSawMotion = true;\n\n        double observedSame = 0.0;\n        if (!autoLastObservedFrame.empty())\n            observedSame = sampledSameRatio(autoLastObservedFrame, data, imgW, imgH);\n        autoLastObservedFrame = data;\n\n        if (autoStepSawMotion && observedSame >= 0.992) ++autoStableFrames;\n        else autoStableFrames = 0;\n\n        bool stableEnough = autoStepSawMotion && autoStableFrames >= 1;\n        bool forceLatest = autoStepSawMotion && autoStepFrames >= 10;\n        if (!stableEnough && !forceLatest) {\n            if (!autoStepSawMotion && autoStepFrames >= 6) {\n                resetAutoStepState();\n                ++noProgressFrames;\n                StarCapDiag::append(std::format(L"[long-next] auto-step-no-motion noProgress={}", noProgressFrames));\n                if (noProgressFrames >= noProgressBeforeFallback) {\n                    noProgressFrames = 0;\n                    if (!advanceScrollStrategy()) pauseAuto(L"no-progress");\n                    else scheduleAutoScroll(45);\n                }\n                else scheduleAutoScroll(45);\n            }\n            return;\n        }\n\n        StarCapDiag::append(std::format(L"[long-next] auto-step-stable frames={} stable={} forced={} committedSame={:.4f} observedSame={:.4f}",\n            autoStepFrames, autoStableFrames, forceLatest ? 1 : 0, committedSame, observedSame));\n    }\n\n    MatchResult match = matchFrame(committedFrame, data);\n'''
if old_process not in cpp:
    raise SystemExit('processFrame marker not found')
cpp = cpp.replace(old_process, new_process, 1)

cpp = cpp.replace(
'''        if (autoScroll) {\n            ++noProgressFrames;\n            if (noProgressFrames >= noProgressBeforeFallback) {\n                noProgressFrames = 0;\n                if (!advanceScrollStrategy()) pauseAuto(L"no-progress");\n            }\n            else if (state != CaptureState::Mismatch) setState(CaptureState::Waiting, L"no-motion-yet");\n        }\n''',
'''        if (autoScroll) {\n            resetAutoStepState();\n            ++noProgressFrames;\n            if (noProgressFrames >= noProgressBeforeFallback) {\n                noProgressFrames = 0;\n                if (!advanceScrollStrategy()) pauseAuto(L"no-progress");\n                else scheduleAutoScroll(45);\n            }\n            else {\n                if (state != CaptureState::Mismatch) setState(CaptureState::Waiting, L"no-motion-yet");\n                scheduleAutoScroll(45);\n            }\n        }\n''', 1)

marker = '''    if (!match.accepted) {\n'''
micro = '''    if (match.accepted && match.offset > 0 && match.offset < 4 && match.expectedOffset < 3.0) {\n        StarCapDiag::append(std::format(L"[long-next] micro-motion-ignored offset={} score={:.5f} mad={:.2f} auto={}",\n            match.offset, match.visualScore, match.pixelMad, autoScroll ? 1 : 0));\n        if (autoScroll) {\n            resetAutoStepState();\n            ++noProgressFrames;\n            scheduleAutoScroll(45);\n            setState(CaptureState::Waiting, L"micro-motion-ignored");\n        }\n        return;\n    }\n\n'''
if marker not in cpp:
    raise SystemExit('reject marker not found')
cpp = cpp.replace(marker, micro + marker, 1)

cpp = cpp.replace(
'''    commitFrame(data, match);\n    ++acceptedFrames;\n    setState(CaptureState::Confirmed, L"seam-confirmed");\n''',
'''    commitFrame(data, match);\n    ++acceptedFrames;\n    if (autoScroll) {\n        resetAutoStepState();\n        scheduleAutoScroll(45);\n    }\n    setState(CaptureState::Confirmed, L"seam-confirmed");\n''', 1)

cpp = cpp.replace(
'''void CapLong::dispatchAutoScroll()\n{\n    if (!autoScroll || !isCapturing || isFinish) return;\n    ++scrollSequence;\n''',
'''void CapLong::dispatchAutoScroll()\n{\n    if (!autoScroll || !isCapturing || isFinish || hardPaused || autoStepPending) return;\n    ++scrollSequence;\n''')

cpp = cpp.replace(
'''    if (!dispatched) {\n        StarCapDiag::append(std::format(L"[long-next] auto-dispatch-failed strategy={}", static_cast<int>(scrollStrategy)));\n        if (!advanceScrollStrategy()) pauseAuto(L"scroll-driver-unavailable");\n    }\n}\n''',
'''    if (!dispatched) {\n        StarCapDiag::append(std::format(L"[long-next] auto-dispatch-failed strategy={}", static_cast<int>(scrollStrategy)));\n        if (!advanceScrollStrategy()) pauseAuto(L"scroll-driver-unavailable");\n        else scheduleAutoScroll(45);\n        return;\n    }\n\n    autoStepPending = true;\n    autoStepSawMotion = false;\n    autoStepFrames = 0;\n    autoStableFrames = 0;\n    autoLastObservedFrame = committedFrame;\n}\n''')

old_pause = '''void CapLong::pauseAuto(const wchar_t* reason)\n{\n    if (!autoScroll) return;\n    autoScroll = false;\n    win->killTimer(autoScrollTimerId);\n    if (tool) tool->setAutoRunning(false);\n    setState(CaptureState::Paused, reason);\n    StarCapDiag::append(std::format(L"[long-next] auto-paused reason={} resultH={} accepted={} rejected={} strategy={}",\n        reason ? reason : L"?", resultH, acceptedFrames, rejectedFrames, static_cast<int>(scrollStrategy)));\n}\n\nvoid CapLong::startAutoScroll()\n'''
new_pause = '''void CapLong::resetAutoStepState()\n{\n    autoStepPending = false;\n    autoStepSawMotion = false;\n    autoStepFrames = 0;\n    autoStableFrames = 0;\n    autoLastObservedFrame.clear();\n}\n\nvoid CapLong::pauseAuto(const wchar_t* reason, bool hardPause)\n{\n    if (!autoScroll && !hardPaused) return;\n    autoScroll = false;\n    resetAutoStepState();\n    win->killTimer(autoScrollTimerId);\n    if (hardPause) {\n        hardPaused = true;\n        win->killTimer(frameCaptureTimerId);\n    }\n    if (tool) tool->setAutoRunning(false);\n    setState(CaptureState::Paused, reason);\n    StarCapDiag::append(std::format(L"[long-next] auto-paused reason={} hard={} resultH={} accepted={} rejected={} strategy={}",\n        reason ? reason : L"?", hardPause ? 1 : 0, resultH, acceptedFrames, rejectedFrames, static_cast<int>(scrollStrategy)));\n}\n\nvoid CapLong::startAutoScroll()\n'''
if old_pause not in cpp:
    raise SystemExit('pause block not found')
cpp = cpp.replace(old_pause, new_pause, 1)

cpp = cpp.replace(
'''void CapLong::startAutoScroll()\n{\n    if (!isCapturing || isFinish || autoScroll) return;\n    resolveScrollTargets();\n''',
'''void CapLong::startAutoScroll()\n{\n    if (!isCapturing || isFinish || autoScroll) return;\n    if (hardPaused) {\n        hardPaused = false;\n        frameRing.clear();\n        resetAutoStepState();\n        scheduleFrameCapture(15);\n        StarCapDiag::append(std::format(L"[long-next] hard-pause-resume resultH={} accepted={}", resultH, acceptedFrames));\n    }\n    resolveScrollTargets();\n''')

cpp = cpp.replace(
'''    autoScroll = true;\n    rejectedFrames = 0;\n    noProgressFrames = 0;\n''',
'''    autoScroll = true;\n    resetAutoStepState();\n    rejectedFrames = 0;\n    noProgressFrames = 0;\n''', 1)

cpp = cpp.replace(
'''    if (autoScroll) pauseAuto(L"user-pause");\n    else startAutoScroll();\n''',
'''    if (autoScroll) pauseAuto(L"user-pause", true);\n    else startAutoScroll();\n''')

cpp = cpp.replace(
'''    autoScroll = false;\n    if (tool) tool->setAutoRunning(false);\n    win->killTimer(autoScrollTimerId);\n''',
'''    autoScroll = false;\n    hardPaused = false;\n    resetAutoStepState();\n    if (tool) tool->setAutoRunning(false);\n    win->killTimer(autoScrollTimerId);\n''', 1)

cpp_path.write_text(cpp, encoding='utf-8-sig')
h_path.write_text(h, encoding='utf-8-sig')
print('Long Capture Next v11 patch applied')
