from pathlib import Path

path = Path('Src/StarCapCaptureTranslate.h')
s = path.read_text(encoding='utf-8')

old = '#include "Setting.h"\n#include "GeminiClient.h"'
new = '#include "Setting.h"\n#include "Util.h"\n#include "GeminiClient.h"'
if s.count(old) != 1:
    raise SystemExit(f'include anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''            onKeyDown.add([this](UINT key) {
                if (key != VK_ESCAPE) return;
                auto* target = this->captureOwner;
                hide();
                // Do not destroy this overlay from inside its own key callback. Queue the
                // capture close; WinCap's destroy hook then resets the translation state.
                Ling::App::get()->dq.TryEnqueue([target]() {
                    if (target && WinCap::get() == target) target->close();
                });
            });'''
new = '''            onKeyDown.add([this](UINT key) {
                if (key == VK_RETURN) {
                    copyTranslatedToClipboardAndClose();
                    return;
                }
                if (key != VK_ESCAPE) return;
                auto* target = this->captureOwner;
                hide();
                // Do not destroy this overlay from inside its own key callback. Queue the
                // capture close; WinCap's destroy hook then resets the translation state.
                Ling::App::get()->dq.TryEnqueue([target]() {
                    if (target && WinCap::get() == target) target->close();
                });
            });'''
if s.count(old) != 1:
    raise SystemExit(f'key handler anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''    private:
        D2D1_COLOR_F sampleBackground(const GeminiClient::TranslationBlock& block) const
        {'''
new = '''    private:
        bool renderTranslatedPixels(std::vector<BYTE>& out)
        {
            out.clear();
            if (imageW <= 0 || imageH <= 0) return false;

            Microsoft::WRL::ComPtr<ID2D1Device> device;
            Ling::D2D::get()->deviceContext->GetDevice(device.GetAddressOf());
            if (!device) return false;

            Microsoft::WRL::ComPtr<ID2D1DeviceContext> offscreen;
            if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                offscreen.GetAddressOf())) || !offscreen) return false;

            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            targetProps.dpiX = 96.f;
            targetProps.dpiY = 96.f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
            if (FAILED(offscreen->CreateBitmap(D2D1::SizeU((UINT32)imageW, (UINT32)imageH),
                nullptr, 0, &targetProps, target.GetAddressOf())) || !target) return false;

            offscreen->SetTarget(target.Get());
            offscreen->BeginDraw();
            constexpr float c = 254.f / 255.f;
            offscreen->Clear(D2D1::ColorF(c, c, c, 1.f));
            paintBlocks(offscreen.Get(), D2D1::RectF(0.f, 0.f, (float)imageW, (float)imageH));
            if (FAILED(offscreen->EndDraw())) return false;

            D2D1_BITMAP_PROPERTIES1 cpuProps{};
            cpuProps.pixelFormat = targetProps.pixelFormat;
            cpuProps.dpiX = 96.f;
            cpuProps.dpiY = 96.f;
            cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpu;
            if (FAILED(offscreen->CreateBitmap(D2D1::SizeU((UINT32)imageW, (UINT32)imageH),
                nullptr, 0, &cpuProps, cpu.GetAddressOf())) || !cpu) return false;
            if (FAILED(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;

            D2D1_MAPPED_RECT mapped{};
            if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
            const size_t rowBytes = (size_t)imageW * 4;
            out.resize(rowBytes * (size_t)imageH);
            for (int row = 0; row < imageH; ++row) {
                BYTE* dst = out.data() + (size_t)row * rowBytes;
                memcpy(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
                for (size_t i = 3; i < rowBytes; i += 4) dst[i] = 255;
            }
            cpu->Unmap();
            return true;
        }

        void copyTranslatedToClipboardAndClose()
        {
            std::vector<BYTE> rendered;
            if (!renderTranslatedPixels(rendered) || rendered.empty()) return;
            Util::saveToClipboard(imageW, imageH, rendered.data());

            auto* target = captureOwner;
            hide();
            Ling::App::get()->dq.TryEnqueue([target]() {
                if (target && WinCap::get() == target) target->close();
            });
        }

        D2D1_COLOR_F sampleBackground(const GeminiClient::TranslationBlock& block) const
        {'''
if s.count(old) != 1:
    raise SystemExit(f'render anchor count={s.count(old)}')
s = s.replace(old, new, 1)

old = '''        if (ready && overlay) {
            if (showing) { overlay->hide(); showing = false; }
            else { overlay->show(); showing = true; }
            return;
        }'''
new = '''        if (ready && overlay) {
            if (showing) {
                overlay->hide();
                showing = false;
                if (win->hwnd) {
                    SetForegroundWindow(win->hwnd);
                    SetFocus(win->hwnd);
                }
            }
            else {
                overlay->show();
                showing = true;
                if (overlay->hwnd) {
                    SetForegroundWindow(overlay->hwnd);
                    SetFocus(overlay->hwnd);
                }
            }
            return;
        }'''
if s.count(old) != 1:
    raise SystemExit(f'toggle focus anchor count={s.count(old)}')
s = s.replace(old, new, 1)

path.write_text(s, encoding='utf-8')
print('Patched current-view output behavior.')
