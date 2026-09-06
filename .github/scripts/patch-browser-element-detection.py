from pathlib import Path

cpp = Path('Src/Win/CutMask.cpp')
text = cpp.read_text(encoding='utf-8')

hp = Path('Src/Win/CutMask.h')
h = hp.read_text(encoding='utf-8')
old = 'bool detectUiElements{ false };'
if old in h:
    h = h.replace(old, 'bool detectUiElements{ true };', 1)
elif 'bool detectUiElements{ true };' not in h:
    raise SystemExit('detectUiElements marker not found')
hp.write_text(h, encoding='utf-8')

marker = '\tPOINT screenPos{ localPos.x + win->x, localPos.y + win->y };\n\n\tauto clippedCandidate'
insert = '\tPOINT screenPos{ localPos.x + win->x, localPos.y + win->y };\n\n\tHWND probeHwnd = hwnd;\n\tfor (int depth = 0; probeHwnd && depth < 16; ++depth) {\n\t\tPOINT client = screenPos;\n\t\tif (!ScreenToClient(probeHwnd, &client)) break;\n\t\tHWND child = ChildWindowFromPointEx(probeHwnd, client,\n\t\t\tCWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);\n\t\tif (!child || child == probeHwnd) break;\n\t\tRECT childRect{};\n\t\tif (!GetWindowRect(child, &childRect) || !PtInRect(&childRect, screenPos)) break;\n\t\tprobeHwnd = child;\n\t}\n\n\tauto clippedCandidate'
if marker in text:
    text = text.replace(marker, insert, 1)
elif '\tHWND probeHwnd = hwnd;' not in text:
    raise SystemExit('screenPos marker not found')

old_root = '\t\tComPtr<IUIAutomationElement> current;\n\t\tif (SUCCEEDED(automation->ElementFromHandle(hwnd, current.GetAddressOf())) && current) {'
new_root = '\t\tComPtr<IUIAutomationElement> current;\n\t\tHRESULT rootHr = automation->ElementFromHandle(probeHwnd, current.GetAddressOf());\n\t\tif ((FAILED(rootHr) || !current) && probeHwnd != hwnd) {\n\t\t\tcurrent.Reset();\n\t\t\trootHr = automation->ElementFromHandle(hwnd, current.GetAddressOf());\n\t\t}\n\t\tif (SUCCEEDED(rootHr) && current) {'
if old_root in text:
    text = text.replace(old_root, new_root, 1)
elif 'HRESULT rootHr = automation->ElementFromHandle(probeHwnd' not in text:
    raise SystemExit('UIA root block not found')

old_msaa = '\tComPtr<IAccessible> acc;\n\tif (SUCCEEDED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,\n\t\treinterpret_cast<void**>(acc.GetAddressOf()))) && acc) {'
new_msaa = '\tComPtr<IAccessible> acc;\n\tHRESULT accHr = AccessibleObjectFromWindow(probeHwnd, OBJID_CLIENT, IID_IAccessible,\n\t\treinterpret_cast<void**>(acc.GetAddressOf()));\n\tif ((FAILED(accHr) || !acc) && probeHwnd != hwnd) {\n\t\tacc.Reset();\n\t\taccHr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,\n\t\t\treinterpret_cast<void**>(acc.GetAddressOf()));\n\t}\n\tif (SUCCEEDED(accHr) && acc) {'
if old_msaa in text:
    text = text.replace(old_msaa, new_msaa, 1)
elif 'HRESULT accHr = AccessibleObjectFromWindow(probeHwnd' not in text:
    raise SystemExit('MSAA root block not found')

start_marker = '    // Measure exactly what will be painted.'
if start_marker in text:
    start = text.index(start_marker)
    end = text.index('\n    const float firstY = top + firstPad;', start)
    lines = [
'    auto measureKeyGroup = [&](const HelpRow& row) {',
'        float width = 0.f;',
'        for (size_t i = 0; i < row.keys.size(); ++i) {',
'            auto keyLayout = d2d->makeTextLayout(row.keys[i], 12.5f * scale);',
'            if (!keyLayout) continue;',
'            DWRITE_TEXT_METRICS km{};',
'            keyLayout->GetMetrics(&km);',
'            width += std::max(22.f * scale, km.width + 10.f * scale);',
'            if (i + 1 < row.keys.size()) width += keyGap;',
'        }',
'        return width;',
'    };',
'',
'    float keyColumnW = 0.f;',
'    float descColumnW = 0.f;',
'    for (const auto& row : rows) {',
'        keyColumnW = std::max(keyColumnW, measureKeyGroup(row));',
'        auto descLayout = d2d->makeTextLayout(row.desc, 16.f * scale);',
'        if (descLayout) {',
'            DWRITE_TEXT_METRICS dm{};',
'            descLayout->GetMetrics(&dm);',
'            descColumnW = std::max(descColumnW, dm.width);',
'        }',
'    }',
'',
'    const float panelW = sidePad + keyColumnW + descGap + descColumnW + sidePad;',
'    const float panelH = firstPad + (rows.size() - 1) * step + keyH + bottomPad;',
'',
'    POINT cursorScreen{};',
'    GetCursorPos(&cursorScreen);',
'    HMONITOR monitor = MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);',
'    MONITORINFO mi{ sizeof(mi) };',
'    RECT work{ win->x, win->y, win->x + (LONG)std::lround(win->w), win->y + (LONG)std::lround(win->h) };',
'    if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;',
'',
'    const float workLeft = (float)(work.left - win->x);',
'    const float workTop = (float)(work.top - win->y);',
'    const float workRight = (float)(work.right - win->x);',
'    const float workBottom = (float)(work.bottom - win->y);',
'    const float margin = 14.f * dpiScale;',
'    float left = workLeft + margin;',
'    float top = workBottom - margin - panelH;',
'    left = std::clamp(left, workLeft, std::max(workLeft, workRight - panelW));',
'    top = std::clamp(top, workTop, std::max(workTop, workBottom - panelH));',
'',
'    const auto panel = D2D1::RectF(left, top, left + panelW, top + panelH);',
'',
'    POINT cursorLocal = cursorScreen;',
'    ScreenToClient(win->hwnd, &cursorLocal);',
'    if (pointInRect(panel, cursorLocal)) return;',
'',
'    ctx->FillRectangle(panel, brushHelpBg.Get());',
'',
'    const float keyColumnRight = left + sidePad + keyColumnW;',
'    const float descColumnLeft = keyColumnRight + descGap;',
'',
'    auto drawRow = [&](float rowY, const HelpRow& row) {',
'        const float groupW = measureKeyGroup(row);',
'        float x = keyColumnRight - groupW;',
'        for (size_t i = 0; i < row.keys.size(); ++i) {',
'            auto keyLayout = d2d->makeTextLayout(row.keys[i], 12.5f * scale);',
'            if (!keyLayout) continue;',
'            DWRITE_TEXT_METRICS km{};',
'            keyLayout->GetMetrics(&km);',
'            const float kw = std::max(22.f * scale, km.width + 10.f * scale);',
'            const float kl = std::round(x);',
'            const float kt = std::round(rowY);',
'            const float kr = std::round(x + kw);',
'            const float kb = std::round(rowY + keyH);',
'            constexpr float keyStroke = 1.f;',
'            const auto oldAA = ctx->GetAntialiasMode();',
'            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);',
'            ctx->FillRectangle(D2D1::RectF(kl, kt, kr, kt + keyStroke), brushKeyBorder.Get());',
'            ctx->FillRectangle(D2D1::RectF(kl, kb - keyStroke, kr, kb), brushKeyBorder.Get());',
'            ctx->FillRectangle(D2D1::RectF(kl, kt + keyStroke, kl + keyStroke, kb - keyStroke), brushKeyBorder.Get());',
'            ctx->FillRectangle(D2D1::RectF(kr - keyStroke, kt + keyStroke, kr, kb - keyStroke), brushKeyBorder.Get());',
'            ctx->SetAntialiasMode(oldAA);',
'            ctx->DrawTextLayout({ x + (kw - km.width) * .5f, rowY + 1.5f * scale },',
'                keyLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);',
'            x += kw;',
'            if (i + 1 < row.keys.size()) x += keyGap;',
'        }',
'        auto descLayout = d2d->makeTextLayout(row.desc, 16.f * scale);',
'        if (descLayout) ctx->DrawTextLayout({ descColumnLeft, rowY - 0.5f * scale },',
'            descLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);',
'    };',
    ]
    replacement = '\n'.join(lines) + '\n'
    text = text[:start] + replacement + text[end:]
elif 'const float keyColumnRight = left + sidePad + keyColumnW;' not in text:
    raise SystemExit('help layout marker not found')

cpp.write_text(text, encoding='utf-8')
