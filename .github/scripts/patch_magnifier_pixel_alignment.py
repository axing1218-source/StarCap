from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')

old = '''    const float cx = left + centerCol * cellW;\n    const float cy = top + centerRow * cellH;\n    const float crossThickness = 6.3f * scale;\n    const float crossHalf = crossThickness * .5f;\n    const float cellCenterX = cx + cellW * .5f;\n    const float cellCenterY = cy + cellH * .5f;\n\n    // Snipaste-style target: the blue cross stops at a small black frame.\n    // The frame itself is outside the observation area and its interior is\n    // completely transparent, leaving the sampled source pixel unobscured.\n    // At 100% DPI this is an 11x11 outer frame with a 1px hard black border\n    // and a 9x9 transparent center.\n    const float outlineThickness = 1.f;\n    const float frameSize = 11.f * scale;\n    const float frameL = std::round(cellCenterX - frameSize * .5f);\n    const float frameT = std::round(cellCenterY - frameSize * .5f);\n    const float frameR = frameL + frameSize;\n    const float frameB = frameT + frameSize;\n'''

new = '''    const float cx = left + centerCol * cellW;\n    const float cy = top + centerRow * cellH;\n    const float cellCenterX = cx + cellW * .5f;\n    const float cellCenterY = cy + cellH * .5f;\n\n    // Pixel-align the target exactly like Snipaste. At 100% DPI both the\n    // 11x11 black frame and the 7px cross are centered on the same integer\n    // source-pixel index. Using a floating 6.3px band plus round(frame-5.5)\n    // produced an asymmetric 6px horizontal arm and made the frame appear to\n    // overhang by one pixel on one side.\n    const float centerPxX = std::round(cellCenterX);\n    const float centerPxY = std::round(cellCenterY);\n    const float outlineThickness = 1.f;\n    const float frameL = centerPxX - 5.f * scale;\n    const float frameT = centerPxY - 5.f * scale;\n    const float frameR = centerPxX + 6.f * scale;\n    const float frameB = centerPxY + 6.f * scale;\n    const float crossL = centerPxX - 3.f * scale;\n    const float crossT = centerPxY - 3.f * scale;\n    const float crossR = centerPxX + 4.f * scale;\n    const float crossB = centerPxY + 4.f * scale;\n'''

if old not in text:
    raise SystemExit('geometry block not found')
text = text.replace(old, new, 1)

old2 = '''            const float crossRight = cellCenterX + crossHalf;\n            const float crossBottom = cellCenterY + crossHalf;\n            // Right side, then the remaining bottom-left/bottom-center area.\n            // These rectangles form one union, so bottom-right is not shaded twice.\n            ctx->FillRectangle(D2D1::RectF(crossRight, top, left + panelW, top + imageH), quadrantShade.Get());\n            ctx->FillRectangle(D2D1::RectF(left, crossBottom, crossRight, top + imageH), quadrantShade.Get());\n'''
new2 = '''            // Right side, then the remaining bottom-left/bottom-center area.\n            // Split exactly at the pixel-aligned cross edge; bottom-right is\n            // not shaded twice.\n            ctx->FillRectangle(D2D1::RectF(crossR, top, left + panelW, top + imageH), quadrantShade.Get());\n            ctx->FillRectangle(D2D1::RectF(left, crossB, crossR, top + imageH), quadrantShade.Get());\n'''
if old2 not in text:
    raise SystemExit('shade split block not found')
text = text.replace(old2, new2, 1)

old3 = '''    ctx->FillRectangle(D2D1::RectF(left, cellCenterY - crossHalf, frameL, cellCenterY + crossHalf), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, top, cellCenterX + crossHalf, frameT), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH), brushAccentSoft.Get());\n'''
new3 = '''    ctx->FillRectangle(D2D1::RectF(left, crossT, frameL, crossB), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(frameR, crossT, left + panelW, crossB), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(crossL, top, crossR, frameT), brushAccentSoft.Get());\n    ctx->FillRectangle(D2D1::RectF(crossL, frameB, crossR, top + imageH), brushAccentSoft.Get());\n'''
if old3 not in text:
    raise SystemExit('cross drawing block not found')
text = text.replace(old3, new3, 1)

path.write_text(text, encoding='utf-8')
