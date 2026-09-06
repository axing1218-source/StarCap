from pathlib import Path

# 1) Keep an explicitly visible pin toolbar visible while the pin is dragged,
# and add an editor-from-data entry point for OCR/translated images.
cpp_path = Path('Src/Win/WinPin.cpp')
cpp = cpp_path.read_text(encoding='utf-8')

old_drag = '''\tif (toolMain->curId == L"") { // plain pin: no tool means drag the pinned window\n\t\ttoolMain->hide();\n\t\treturn;\n\t}\n'''
new_drag = '''\tif (toolMain->curId == L"") { // plain pin: no tool means drag the pinned window\n\t\t// Do not hide ToolMain here. A normal pin starts with its toolbar hidden,\n\t\t// but once the user explicitly enables it from the context menu it must\n\t\t// remain visible and follow the pin while the window is moved. Hiding the\n\t\t// native toolbar without changing pinToolbarVisible left the menu checked\n\t\t// while the window itself was invisible, forcing an off/on toggle to recover.\n\t\treturn;\n\t}\n'''
if old_drag not in cpp:
    raise SystemExit('Expected plain-pin drag toolbar block not found')
cpp = cpp.replace(old_drag, new_drag, 1)

old_editor = '''void WinPin::initEditor(int x, int y, int w, int h)\n{\n\tauto ptr = new WinPin(x, y, w, h, nullptr, true);\n\tstd::unique_ptr<WinPin> winPin{ ptr };\n\tptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);\n\twinPins.push_back(std::move(winPin));\n}\n\n'''
new_editor = old_editor + '''void WinPin::initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data)\n{\n\t// ToolMain is constructed inside WinPin, so queue editor visibility before\n\t// creating the WinPin object. This gives OCR's "标注图片" the same fixed\n\t// canvas + immediately visible full annotation toolbar as screenshot Mark.\n\tToolMain::queueEditorOpen();\n\tauto ptr = new WinPin(x, y, w, h, &data, true);\n\tstd::unique_ptr<WinPin> winPin{ ptr };\n\tptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);\n\twinPins.push_back(std::move(winPin));\n}\n\n'''
if old_editor not in cpp:
    raise SystemExit('Expected initEditor block not found')
cpp = cpp.replace(old_editor, new_editor, 1)
cpp_path.write_text(cpp, encoding='utf-8')

# 2) Declare the data-backed editor entry point.
h_path = Path('Src/Win/WinPin.h')
h = h_path.read_text(encoding='utf-8')
old_decl = '''\tstatic void initEditor(int x, int y, int w, int h);\n\t// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。\n'''
new_decl = '''\tstatic void initEditor(int x, int y, int w, int h);\n\t// Data-backed annotation editor used by OCR / translated-image windows.\n\t// Unlike initFromData, this is a fixed editor canvas and opens the full toolbar immediately.\n\tstatic void initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data);\n\t// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。\n'''
if old_decl not in h:
    raise SystemExit('Expected initEditor declaration block not found')
h = h.replace(old_decl, new_decl, 1)
h_path.write_text(h, encoding='utf-8')

# 3) OCR "标注图片" must use editor semantics, not desktop pin semantics.
ocr_path = Path('Src/StarCapOcrV2.h')
ocr = ocr_path.read_text(encoding='utf-8')
old_ocr = '            WinPin::initFromData(posX, posY, imageW, imageH, editorPixels);\n'
new_ocr = '            WinPin::initEditorFromData(posX, posY, imageW, imageH, editorPixels);\n'
if old_ocr not in ocr:
    raise SystemExit('Expected OCR annotation pin call not found')
ocr = ocr.replace(old_ocr, new_ocr, 1)
ocr_path.write_text(ocr, encoding='utf-8')
