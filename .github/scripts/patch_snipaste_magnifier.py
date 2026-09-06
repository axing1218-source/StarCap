from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')
old = 'ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .40f), quadrantShade.GetAddressOf());'
new = 'ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .55f), quadrantShade.GetAddressOf());'
if old not in text:
    raise SystemExit('Expected 40% magnifier shade not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
