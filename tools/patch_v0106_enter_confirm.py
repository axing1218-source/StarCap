from pathlib import Path

# WinCap: make Enter focus-independent, route translated current-view Enter
# to the translation overlay, and allow Select-stage highlighted regions.
path = Path('Src/Win/WinCap.cpp')
s = path.read_text(encoding='utf-8')

old = '#include "../Tool/ToolCap.h"\nusing namespace Microsoft::WRL;'
new = '#include "../Tool/ToolCap.h"\n#include "../StarCapCaptureTranslate.h"\nusing namespace Microsoft::WRL;'
if s.count(old) != 1:
    raise SystemExit(f'WinCap include anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '    bool gCaptureTabDown = false;\n'
new = '    bool gCaptureTabDown = false;\n    bool gCaptureEnterDown = false;\n'
if s.count(old) != 1:
    raise SystemExit(f'WinCap enter state anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''                if (up) gCaptureTabDown = false;
            }
        }
        return CallNextHookEx(gCaptureEscapeHook, code, wParam, lParam);'''
new = '''                if (up) gCaptureTabDown = false;
            }
            // Enter is the primary confirm shortcut. Route it through the low-level
            // hook as well, so confirmation never depends on which top-level window
            // currently owns keyboard focus. If the translated overlay is the current
            // visible view, send Enter there; otherwise send it to WinCap.
            if (kb->vkCode == VK_RETURN) {
                const bool modified = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
                    || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                if (!modified) {
                    if (down && !gCaptureEnterDown) {
                        gCaptureEnterDown = true;
                        HWND target = StarCapCaptureTranslate::translatedViewHwnd(WinCap::get());
                        if (!target) target = gCaptureEscapeWindow;
                        if (IsWindow(target))
                            PostMessageW(target, WM_KEYDOWN, VK_RETURN, 0);
                    }
                    if (up) gCaptureEnterDown = false;
                    return 1;
                }
                if (up) gCaptureEnterDown = false;
            }
        }
        return CallNextHookEx(gCaptureEscapeHook, code, wParam, lParam);'''
if s.count(old) != 1:
    raise SystemExit(f'WinCap hook anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''        gCaptureEscapeDown = false;
        gCaptureTabDown = false;
        if (!gCaptureEscapeHook)'''
new = '''        gCaptureEscapeDown = false;
        gCaptureTabDown = false;
        gCaptureEnterDown = false;
        if (!gCaptureEscapeHook)'''
if s.count(old) != 1:
    raise SystemExit(f'WinCap install anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''        gCaptureEscapeWindow = nullptr;
        gCaptureEscapeDown = false;
        gCaptureTabDown = false;
    }
}'''
new = '''        gCaptureEscapeWindow = nullptr;
        gCaptureEscapeDown = false;
        gCaptureTabDown = false;
        gCaptureEnterDown = false;
    }
}'''
if s.count(old) != 1:
    raise SystemExit(f'WinCap uninstall anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''    if (stage == CapStage::Adjust) {
        // 选区里的像素，与在窗口里双击同一条路（见 onDown）。它自己会关窗
        copyToClipboard();
    }'''
new = '''    if ((stage == CapStage::Select || stage == CapStage::Adjust)
        && cutMask && cutMask->hasRect()) {
        // Select may already contain a valid auto-detected window/element rectangle.
        // Enter confirms that highlighted rectangle immediately; Adjust keeps the
        // existing behavior for manually selected/adjusted regions.
        copyToClipboard();
    }'''
if s.count(old) != 1:
    raise SystemExit(f'WinCap Select Enter anchor count={s.count(old)}')
s = s.replace(old, new, 1)
path.write_text(s, encoding='utf-8')

# Translation state: expose only the HWND of the actually visible translated
# view so the capture keyboard hook can route Enter deterministically.
path = Path('Src/StarCapCaptureTranslate.h')
s = path.read_text(encoding='utf-8')
old = '''    inline bool busy{ false }, ready{ false }, showing{ false }, hooksInstalled{ false };
    inline int cachedX{ 0 }, cachedY{ 0 }, cachedW{ 0 }, cachedH{ 0 };

    inline void closeOverlay()'''
new = '''    inline bool busy{ false }, ready{ false }, showing{ false }, hooksInstalled{ false };
    inline int cachedX{ 0 }, cachedY{ 0 }, cachedW{ 0 }, cachedH{ 0 };

    inline HWND translatedViewHwnd(WinCap* win)
    {
        if (owner != win || !ready || !showing || !overlay || !overlay->hwnd) return nullptr;
        return IsWindowVisible(overlay->hwnd) ? overlay->hwnd : nullptr;
    }

    inline void closeOverlay()'''
if s.count(old) != 1:
    raise SystemExit(f'Translate HWND anchor count={s.count(old)}')
s = s.replace(old, new, 1)
path.write_text(s, encoding='utf-8')

# Toolbar check/clipboard button is an explicit original-capture action.
# Dismiss any translation overlay before copying the immutable screenImg.
path = Path('Src/Tool/ToolCap.cpp')
s = path.read_text(encoding='utf-8')
old = '\telse if (btn->id == L"clipboard") win->copyToClipboard();'
new = '''\telse if (btn->id == L"clipboard") {
\t\tStarCapCaptureTranslate::reset(win);
\t\twin->copyToClipboard();
\t}'''
if s.count(old) != 1:
    raise SystemExit(f'ToolCap clipboard anchor count={s.count(old)}')
s = s.replace(old, new, 1)
path.write_text(s, encoding='utf-8')
