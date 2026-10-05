#!/usr/bin/env python3
"""生成 main/radio_title_table.h —— 动态曲名的码点白名单。

字库已迁到框架(appfw/fonts/,常见 3500 字全量),本脚本不再生成字体,
只从框架字符清单 appfw_common_charset.txt 推导"非 ASCII 码点"表。流里的
歌名是任意文本,字库外的字进 LVGL 只会渲染成方框,显示前按此表过滤
(见 radio_pages.c 的 sanitize_title)。

框架字符清单更新后(子模块指针变更)重跑:
    python3 tools/gen_title_table.py
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHARSET = ROOT / "components" / "framework" / "appfw" / "fonts" / "appfw_common_charset.txt"
OUT = ROOT / "main" / "radio_title_table.h"


def main() -> int:
    if not CHARSET.is_file():
        sys.exit(f"缺少框架字符清单 {CHARSET},先更新 components/framework 子模块")
    chars = CHARSET.read_text(encoding="utf-8").strip()
    cps = sorted(ord(c) for c in chars if ord(c) > 0x7F)
    lines = [f"    0x{cp:04X}," for cp in cps]
    table = (
        "// 由 tools/gen_title_table.py 自动生成 —— 框架字库收录的非 ASCII 码点(升序)。\n"
        "// 曲名等动态文本显示前按此表过滤,字库外的字不进 LVGL(否则是方框)。\n"
        "#include <stdint.h>\n\n"
        f"#define RADIO_TITLE_CP_COUNT {len(cps)}\n"
        "static const uint16_t k_radio_title_cps[RADIO_TITLE_CP_COUNT] = {\n"
        + "\n".join(lines) + "\n};\n"
    )
    OUT.write_text(table, encoding="utf-8")
    print(f"{OUT.name}: {len(cps)} 个码点")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
