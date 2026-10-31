from pathlib import Path
root = Path(__file__).resolve().parents[1]
html = (root / 'index.html').read_text(encoding='utf-8')
assert ')BLUI"' not in html
(root / 'include/ui.h').write_text('#pragma once\n#include <Arduino.h>\nconst char UI_HTML[] PROGMEM = R"BLUI(' + html + ')BLUI";\n', encoding='utf-8')
