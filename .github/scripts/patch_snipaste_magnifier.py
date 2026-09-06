from pathlib import Path

path = Path('Src/Win/CutMask.cpp')
text = path.read_text(encoding='utf-8')
old = '''    // Cross arms end at the OUTSIDE edge of the black frame; nothing is
    // painted across the transparent center target. Use the solid accent brush
    // rather than the old 34% translucent blue, which looked like a milky white
    // coating over bright source pixels.
    ctx->FillRectangle(D2D1::RectF(left, cellCenterY - crossHalf, frameL, cellCenterY + crossHalf), brushHandle.Get());
    ctx->FillRectangle(D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf), brushHandle.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, top, cellCenterX + crossHalf, frameT), brushHandle.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH), brushHandle.Get());

    // During an active drag, leave the top-left quadrant plus the LEFT and TOP
    // cross arms clear. Apply the same 40% black veil to the other three
    // quadrants and to the RIGHT and BOTTOM arms. The center target stays clear.
    if (cap->stage == WinCap::CapStage::Select && cap->isPress) {
        ComPtr<ID2D1SolidColorBrush> quadrantShade;
        ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .40f), quadrantShade.GetAddressOf());
        if (quadrantShade) {
            // Three de-emphasized quadrants.
            ctx->FillRectangle(D2D1::RectF(frameR, top, left + panelW, frameT), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(left, frameB, frameL, top + imageH), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(frameR, frameB, left + panelW, top + imageH), quadrantShade.Get());
            // Right and bottom arms use the same veil; left and top remain clear.
            ctx->FillRectangle(D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH), quadrantShade.Get());
        }
    }
'''
new = '''    // Snipaste-style layering: shade right/bottom first. The split starts at
    // the actual cross edge, not at the wider target-frame edge, so no bright
    // white seam can leak between the cross and the center target.
    if (cap->stage == WinCap::CapStage::Select && cap->isPress) {
        ComPtr<ID2D1SolidColorBrush> quadrantShade;
        ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, .40f), quadrantShade.GetAddressOf());
        if (quadrantShade) {
            const float crossRight = cellCenterX + crossHalf;
            const float crossBottom = cellCenterY + crossHalf;
            // Right side, then the remaining bottom-left/bottom-center area.
            // These rectangles form one union, so bottom-right is not shaded twice.
            ctx->FillRectangle(D2D1::RectF(crossRight, top, left + panelW, top + imageH), quadrantShade.Get());
            ctx->FillRectangle(D2D1::RectF(left, crossBottom, crossRight, top + imageH), quadrantShade.Get());
        }
    }

    // Paint the same translucent blue on all four arms after the dark veil.
    // White under left/top produces Snipaste's pale blue; shaded right/bottom
    // naturally produces the darker blue-gray. Keep the target center clear.
    ctx->FillRectangle(D2D1::RectF(left, cellCenterY - crossHalf, frameL, cellCenterY + crossHalf), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(frameR, cellCenterY - crossHalf, left + panelW, cellCenterY + crossHalf), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, top, cellCenterX + crossHalf, frameT), brushAccentSoft.Get());
    ctx->FillRectangle(D2D1::RectF(cellCenterX - crossHalf, frameB, cellCenterX + crossHalf, top + imageH), brushAccentSoft.Get());
'''
if old not in text:
    raise SystemExit('Expected magnifier block not found')
path.write_text(text.replace(old, new, 1), encoding='utf-8')
