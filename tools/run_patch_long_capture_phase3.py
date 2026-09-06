from pathlib import Path

path = Path('tools/patch_long_capture_phase3.py')
source = path.read_text(encoding='utf-8')
source = source.replace("old_ocr = r'''", "old_ocr = '''", 1)
source = source.replace("new_ocr = r'''", "new_ocr = '''", 1)
exec(compile(source, str(path), 'exec'))
