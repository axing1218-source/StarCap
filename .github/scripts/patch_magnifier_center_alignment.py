from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')
old = '''    const float outlineThickness = 1.f;\n    const float frameSize = 11.f * scale;\n    const float frameL = std::round(cellCenterX - frameSize * .5f);\n    const float frameT = std::round(cellCenterY - frameSize * .5f);\n    const float frameR = frameL + frameSize;\n    const float frameB = frameT + frameSize;\n'''
new = '''    const float outlineThickness = 1.f;\n    const float frameSize = 11.f * scale;\n    const float frameL = std::round(cellCenterX - frameSize * .5f);\n    const float frameT = std::round(cellCenterY - frameSize * .5f);\n    const float frameR = frameL + frameSize;\n    const float frameB = frameT + frameSize;\n    // Use the exact raster-aligned frame center for every other magnifier\n    // overlay. The frame edges are snapped to physical pixels for a crisp\n    // 1px border, while cellCenterX/Y can remain fractional. Mixing those two\n    // centers caused the black target to sit about half a pixel off the cross.\n    const float targetCenterX = (frameL + frameR) * .5f;\n    const float targetCenterY = (frameT + frameB) * .5f;\n'''
if old not in text:
    raise SystemExit('frame block not found')
text = text.replace(old, new, 1)
text = text.replace('const float crossRight = cellCenterX + crossHalf;', 'const float crossRight = targetCenterX + crossHalf;', 1)
text = text.replace('const float crossBottom = cellCenterY + crossHalf;', 'const float crossBottom = targetCenterY + crossHalf;', 1)
text = text.replace('D2D1::RectF(left, cellCenterY - crossHalf, frameL, cellCenterY + crossHalf)', 'D2D1::RectF(left, targetCenterY - crossHalf, frameL, targetCenterY + crossHalf)', 1)
text = text.replace('D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf)', 'D2D1::RectF(frameR, targetCenterY - crossHalf, left + panelW, targetCenterY + crossHalf)', 1)
text = text.replace('D2D1::RectF(cellCenterX - crossHalf, top, cellCenterX + crossHalf, frameT)', 'D2D1::RectF(targetCenterX - crossHalf, top, targetCenterX + crossHalf, frameT)', 1)
text = text.replace('D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH)', 'D2D1::RectF(targetCenterX - crossHalf, frameB, targetCenterX + crossHalf, top + imageH)', 1)
path.write_text(text, encoding='utf-8')
