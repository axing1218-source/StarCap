from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')
old = '''    const float frameL = centerPxX - 5.f * scale;\n    const float frameT = centerPxY - 5.f * scale;\n    const float frameR = centerPxX + 6.f * scale;\n    const float frameB = centerPxY + 6.f * scale;\n    const float crossL = centerPxX - 3.f * scale;\n    const float crossT = centerPxY - 3.f * scale;\n    const float crossR = centerPxX + 4.f * scale;\n    const float crossB = centerPxY + 4.f * scale;\n'''
new = '''    // The target frame must have exactly the same outer bounds as the 7px\n    // square created by the horizontal/vertical cross intersection. Keeping\n    // an 11px frame here made the black target visibly overhang the cross by\n    // two pixels on every side during an active drag. The 1px black border is\n    // drawn inward, so the 5x5 center remains transparent for color inspection.\n    const float crossL = centerPxX - 3.f * scale;\n    const float crossT = centerPxY - 3.f * scale;\n    const float crossR = centerPxX + 4.f * scale;\n    const float crossB = centerPxY + 4.f * scale;\n    const float frameL = crossL;\n    const float frameT = crossT;\n    const float frameR = crossR;\n    const float frameB = crossB;\n'''
if old not in text:
    raise SystemExit('Expected 11px target block not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
