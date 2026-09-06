from pathlib import Path

cpp = Path('Src/Win/WinPin.cpp')
text = cpp.read_text(encoding='utf-8')
old = '''void WinPin::initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data)\n{\n\t// ToolMain is constructed inside WinPin, so queue editor visibility before\n\t// creating the WinPin object. This gives OCR's "标注图片" the same fixed\n\t// canvas + immediately visible full annotation toolbar as screenshot Mark.\n\tToolMain::queueEditorOpen();\n\tauto ptr = new WinPin(x, y, w, h, &data, true);\n'''
new = '''void WinPin::initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data)\n{\n\t// OCR's "标注图片" should open the full annotation toolbar immediately,\n\t// but unlike screenshot Mark it still behaves like a movable image window\n\t// whenever no drawing tool is selected. Passing editorMode=false preserves\n\t// the normal WinPin drag path while selected tools continue to receive mouse\n\t// input for annotation.\n\tToolMain::queueEditorOpen();\n\tauto ptr = new WinPin(x, y, w, h, &data, false);\n'''
if old not in text:
    raise SystemExit('Expected initEditorFromData block not found')
cpp.write_text(text.replace(old, new, 1), encoding='utf-8')

hdr = Path('Src/Win/WinPin.h')
text = hdr.read_text(encoding='utf-8')
old = '''\t// Data-backed annotation editor used by OCR / translated-image windows.\n\t// Unlike initFromData, this is a fixed editor canvas and opens the full toolbar immediately.\n\tstatic void initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data);\n'''
new = '''\t// Data-backed annotation window used by OCR / translated-image windows.\n\t// It opens the full toolbar immediately, but remains movable whenever no\n\t// drawing tool is selected.\n\tstatic void initEditorFromData(int x, int y, int w, int h, std::vector<BYTE>& data);\n'''
if old not in text:
    raise SystemExit('Expected initEditorFromData header comment not found')
hdr.write_text(text.replace(old, new, 1), encoding='utf-8')
