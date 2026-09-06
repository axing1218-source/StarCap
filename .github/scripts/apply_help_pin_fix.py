from pathlib import Path

# Tutorial panel: preserve dynamic symmetric horizontal padding, but sharpen key caps
# and enlarge the explanatory text.
p = Path('Src/Win/CutMask.cpp')
text = p.read_text(encoding='utf-8')
text = text.replace(
    'd2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, .64f), brushKeyBorder.GetAddressOf());',
    'd2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 1.f), brushKeyBorder.GetAddressOf());',
    1)
start = text.index('void CutMask::paintHelp(ID2D1DeviceContext* ctx)')
end = text.index('\nvoid CutMask::suppressLegacyMagnifier', start)
block = text[start:end]
block = block.replace('const float descGap = 10.f * scale;', 'const float descGap = 13.f * scale;')
block = block.replace('const float keyH = 21.f * scale;', 'const float keyH = 22.f * scale;')
block = block.replace('const float step = 34.f * scale;', 'const float step = 39.f * scale;')
block = block.replace('d2d->makeTextLayout(row.keys[i], 11.5f * scale)', 'd2d->makeTextLayout(row.keys[i], 12.5f * scale)')
block = block.replace('std::max(21.f * scale, km.width + 9.f * scale)', 'std::max(22.f * scale, km.width + 10.f * scale)')
block = block.replace('d2d->makeTextLayout(row.desc, 12.f * scale)', 'd2d->makeTextLayout(row.desc, 16.f * scale)')
old_draw = '''  ctx->DrawRectangle(D2D1::RectF(x, rowY, x + kw, rowY + keyH),
      brushKeyBorder.Get(), std::max(1.f, scale));
  ctx->DrawTextLayout({ x + (kw - km.width) * .5f, rowY + 1.5f * scale },
      keyLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);'''
new_draw = '''  // Pure white, one-physical-pixel hard frame. Use four filled bars on
  // rounded device coordinates so the key cap has no antialiased/foggy edge.
  const float kl = std::round(x);
  const float kt = std::round(rowY);
  const float kr = std::round(x + kw);
  const float kb = std::round(rowY + keyH);
  constexpr float keyStroke = 1.f;
  const auto oldAA = ctx->GetAntialiasMode();
  ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  ctx->FillRectangle(D2D1::RectF(kl, kt, kr, kt + keyStroke), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kl, kb - keyStroke, kr, kb), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kl, kt + keyStroke, kl + keyStroke, kb - keyStroke), brushKeyBorder.Get());
  ctx->FillRectangle(D2D1::RectF(kr - keyStroke, kt + keyStroke, kr, kb - keyStroke), brushKeyBorder.Get());
  ctx->SetAntialiasMode(oldAA);
  ctx->DrawTextLayout({ x + (kw - km.width) * .5f, rowY + 1.5f * scale },
      keyLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);'''
if old_draw not in block:
    raise SystemExit('key cap draw block not found')
block = block.replace(old_draw, new_draw, 1)
block = block.replace(
    '''        if (descLayout) ctx->DrawTextLayout({ x + descGap, rowY + 1.5f * scale },
  descLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);''',
    '''        if (descLayout) ctx->DrawTextLayout({ x + descGap, rowY - 0.5f * scale },
  descLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);''',
    1)
text = text[:start] + block + text[end:]
p.write_text(text, encoding='utf-8')

# ToolMain: expose context menu to WinPin and stop relying on the delayed right-click hook.
p = Path('Src/Tool/ToolMain.h')
text = p.read_text(encoding='utf-8')
if '\tvoid showPinContextMenu();\npublic:\n' not in text:
    text = text.replace('\tvoid cancelSelect();\npublic:\n', '\tvoid cancelSelect();\n\tvoid showPinContextMenu();\npublic:\n', 1)
text = text.replace('\tvoid applyPinToolbarVisibility();\n\tvoid showPinContextMenu();\n', '\tvoid applyPinToolbarVisibility();\n', 1)
text = text.replace('\twinrt::event_token pinMouseDownToken{};\n', '', 1)
p.write_text(text, encoding='utf-8')

p = Path('Src/Tool/ToolMain.cpp')
text = p.read_text(encoding='utf-8')
text = text.replace(
    '''\tif (pinHooksInstalled && win) {\n\t\twin->onMouseDown.remove(pinMouseDownToken);\n\t\twin->onMouseUp.remove(pinMouseUpToken);\n\t}\n''',
    '''\tif (pinHooksInstalled && win) {\n\t\twin->onMouseUp.remove(pinMouseUpToken);\n\t}\n''', 1)
text = text.replace(
    '''\tpinHooksInstalled = true;\n\tpinMouseDownToken = win->onMouseDown.add([this](POINT, bool isRight) {\n\t\tif (!isRight) return;\n\t\tshowPinContextMenu();\n\t});\n\tpinMouseUpToken = win->onMouseUp.add([this](POINT, bool isRight) {\n''',
    '''\tpinHooksInstalled = true;\n\t// Right-click is handled directly by WinPin::onUp, even while this toolbar is hidden.\n\tpinMouseUpToken = win->onMouseUp.add([this](POINT, bool isRight) {\n''', 1)
p.write_text(text, encoding='utf-8')

# WinPin: invoke menu synchronously on right-button release.
p = Path('Src/Win/WinPin.cpp')
text = p.read_text(encoding='utf-8')
pos = text.index('void WinPin::onUp(POINT pos, BOOL isRight)')
tail = text[pos:]
old = '''\tif (isRight) return;\n\tisMouseDown = false;\n\tReleaseCapture();\n'''
new = '''\tif (isRight) {\n\t\t// The pinned image owns its context-menu gesture. This remains reliable\n\t\t// when ToolMain is hidden and never changes toolbar visibility by itself.\n\t\tif (toolMain) toolMain->showPinContextMenu();\n\t\treturn;\n\t}\n\tisMouseDown = false;\n\tReleaseCapture();\n'''
if old not in tail:
    raise SystemExit('WinPin onUp right-click block not found')
tail = tail.replace(old, new, 1)
text = text[:pos] + tail
p.write_text(text, encoding='utf-8')
