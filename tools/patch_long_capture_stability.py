from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f'{label} marker not found')
    return text.replace(old, new, 1)

# This patch runs after Phase 2, Phase 2.5 and Phase 3 have been applied by CI.
# Philosophy: automatic long capture is user-controlled. StarCap may pause when it loses confidence,
# but it must not declare "bottom reached" and finish the capture on its own.

h_path = Path('Src/Win/CapLong.h')
h = h_path.read_text(encoding='utf-8-sig')

h = replace_once(
    h,
    '\tvoid handleAutoNoProgress();\n',
    '\tvoid handleAutoNoProgress();\n\tbool autoOffsetFitsQuantum(int offset) const;\n\tvoid observeAutoQuantum(int offset, double score);\n',
    'stability helper declarations')

h = replace_once(
    h,
    '\tint dismissTime{ 0 };\n',
    '\tint dismissTime{ 0 };\n\tint autoScrollQuantum{ 0 };\n\tint autoQuantumSamples{ 0 };\n',
    'auto quantum state')

h_path.write_text(h, encoding='utf-8-sig')

cpp_path = Path('Src/Win/CapLong.cpp')
cpp = cpp_path.read_text(encoding='utf-8-sig')

# Reject implausible automatic offsets once the current scroll quantum has been learned.
# This is intentionally auto-only: manual capture remains unrestricted because users can scroll
# by touchpad or drag a scrollbar by arbitrary distances.
quantum_guard_marker = r'''    if (usedV2) {
        int score100 = (int)(v2Match.score * 100.0);
'''
quantum_guard = r'''    if (usedV2 && autoScroll && autoQuantumSamples >= 2 && !autoOffsetFitsQuantum(y)) {
        StarCapDiag::append(std::format(L"[long-v2] stitch-rejected-quantum offset={} quantum={} samples={} score100={}",
            y, autoScrollQuantum, autoQuantumSamples, (int)(v2Match.score * 100.0)));
        y = 0;
        usedV2 = false;
        ambiguousV2 = true;
    }

'''
cpp = replace_once(cpp, quantum_guard_marker, quantum_guard + quantum_guard_marker, 'quantum guard')

# Learn only from accepted high-confidence V2 seams. A smaller clean step becomes the new base;
# larger integer multiples reinforce confidence without changing the base quantum.
observe_marker = r'''    settleRecheckCount = 0;
    int paintStart = resultH - (imgH - y - changeStartY);
'''
observe_code = r'''    if (usedV2 && autoScroll) observeAutoQuantum(y, v2Match.score);

'''
cpp = replace_once(cpp, observe_marker, observe_marker.replace('\n    int paintStart', '\n' + observe_code + '    int paintStart'), 'quantum observation')

# Insert the quantum helpers immediately before handleAutoNoProgress.
handle_marker = r'''void CapLong::handleAutoNoProgress()
{
'''
helpers = r'''bool CapLong::autoOffsetFitsQuantum(int offset) const
{
    if (offset <= 0 || autoScrollQuantum <= 0 || autoQuantumSamples < 2) return true;
    // A final partial scroll smaller than one quantum is legitimate near the bottom.
    if (offset < autoScrollQuantum) return true;
    int multiple = std::max(1, (offset + autoScrollQuantum / 2) / autoScrollQuantum);
    int expected = multiple * autoScrollQuantum;
    int tolerance = std::max(6, autoScrollQuantum * 12 / 100);
    return abs(offset - expected) <= tolerance;
}

void CapLong::observeAutoQuantum(int offset, double score)
{
    if (offset <= 0 || score > 1.0) return;
    if (autoScrollQuantum <= 0) {
        autoScrollQuantum = offset;
        autoQuantumSamples = 1;
        return;
    }

    int tolerance = std::max(6, autoScrollQuantum * 12 / 100);
    if (offset < autoScrollQuantum - tolerance) {
        // We probably first observed a skipped/multi-notch frame. Prefer the smaller clean unit.
        autoScrollQuantum = offset;
        autoQuantumSamples = 1;
        return;
    }

    if (abs(offset - autoScrollQuantum) <= tolerance) {
        int weight = std::min(autoQuantumSamples, 7);
        autoScrollQuantum = (autoScrollQuantum * weight + offset) / (weight + 1);
        autoQuantumSamples = std::min(autoQuantumSamples + 1, 8);
        if (autoQuantumSamples == 2) {
            StarCapDiag::append(std::format(L"[long-v2] auto-quantum-locked={} samples={}",
                autoScrollQuantum, autoQuantumSamples));
        }
        return;
    }

    int multiple = std::max(1, (offset + autoScrollQuantum / 2) / autoScrollQuantum);
    if (abs(offset - multiple * autoScrollQuantum) <= tolerance) {
        autoQuantumSamples = std::min(autoQuantumSamples + 1, 8);
    }
}

'''
cpp = replace_once(cpp, handle_marker, helpers + handle_marker, 'quantum helpers')

# Once a strategy has proven it can scroll, repeated no-progress no longer means "bottom".
# Pause automatic movement and fall back to manual polling while keeping the entire long-capture
# session alive. Space can resume auto; Enter finishes; Esc cancels.
old_bottom = r'''    if (++dismissTime > maxDismissTime) {
        StarCapDiag::append(std::format(L"[long-v2] reached-bottom strategy={} resultH={}",
            autoStrategyName(), resultH));
        stopCap();
        return;
    }
    scheduleAutoScroll(autoScrollDelayMs);
'''
new_bottom = r'''    if (++dismissTime > maxDismissTime) {
        StarCapDiag::append(std::format(L"[long-v2] auto-paused-no-progress strategy={} resultH={} quantum={}",
            autoStrategyName(), resultH, autoScrollQuantum));
        autoScroll = false;
        autoStrategyConfirmed = false;
        dismissTime = 0;
        settleRecheckCount = 0;
        autoSettleChecks = 0;
        autoSettleFrame.clear();
        firstCheck = true;
        changeStartY = -1;
        win->killTimer(scrollMsgId);
        win->killTimer(scrollEndMsgId);
        scheduleNextCapture(80);
        return;
    }
    scheduleAutoScroll(autoScrollDelayMs);
'''
cpp = replace_once(cpp, old_bottom, new_bottom, 'remove automatic bottom stop')

cpp_path.write_text(cpp, encoding='utf-8-sig')
print('Long Capture stability pass applied: no auto-finish + auto quantum guard.')
