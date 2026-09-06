from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')
old = '''    const float frameL = crossL;\n    const float frameT = crossT;\n    const float frameR = crossR;\n    const float frameB = crossB;\n'''
new = '''    // Draw the 1px black target border OUTSIDE the 7x7 cross-intersection\n    // square. The transparent observation area therefore remains exactly\n    // crossL..crossR / crossT..crossB, while the complete outer target is 9x9\n    // at 100% DPI. No border pixel is allowed to consume the sampled center.\n    const float frameL = crossL - 1.f * scale;\n    const float frameT = crossT - 1.f * scale;\n    const float frameR = crossR + 1.f * scale;\n    const float frameB = crossB + 1.f * scale;\n'''
if old not in text:
    raise SystemExit('Expected inward target frame block not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
