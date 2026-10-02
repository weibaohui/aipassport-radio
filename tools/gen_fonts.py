#!/usr/bin/env python3
"""从源码推导中文字体字符集,再生成 LVGL 字体。

手维护字符清单迟早会漏字。这里直接从会被渲染的源码里抽出所有 CJK/全角字符,
写成 radio_charset.txt,再交给 lv_font_conv 生成 .c。这样"加文案"只需要
重新跑一次本脚本,而不是靠人记得补字。

用法:
    cd assets/fonts
    curl -L -o NotoSansSC-Regular.otf <思源黑体地址>   # 首次需要,不入库
    python3 ../../tools/gen_fonts.py

约束:
- 24px 只放顶栏标题那几个字(框架里 24px 只有顶栏标题一处使用点),
  标题改了必须重跑本脚本。
- 只扫描真正会被 LVGL 渲染的源码。但本应用的门户 HTML 卡片和 UI 字符串
  写在同一个文件里,会被一并算进去——宁可多几个字(每个约 200 字节),
  也不要漏。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "components" / "framework"
FONTS = ROOT / "assets" / "fonts"
OTF = FONTS / "NotoSansSC-Regular.otf"

# 16px 承载的源码:框架 UI + 本应用 UI。
SOURCES_16 = [
    FW / "appfw" / "src" / "appfw_ui.c",
    ROOT / "main" / "radio_pages.c",
    ROOT / "main" / "main.c",
]
# 24px 只用于顶栏标题,标题在 main.c 的 .home_title。
SOURCES_24 = [ROOT / "main" / "main.c"]

PUNCT = set("，。：；？！（）《》—…·℃─")
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


def is_checked(ch: str) -> bool:
    cp = ord(ch)
    return 0x4E00 <= cp <= 0x9FFF or ch in PUNCT


def literal_chars(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    out: set[str] = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        out |= {c for c in lit if is_checked(c)}
    return out


def collect(sources: list[Path]) -> set[str]:
    out: set[str] = set()
    for path in sources:
        if not path.is_file():
            sys.exit(f"缺少源码: {path}")
        out |= literal_chars(path)
    return out


def home_title() -> str:
    text = (ROOT / "main" / "main.c").read_text(encoding="utf-8")
    m = re.search(r'\.home_title\s*=\s*"([^"]*)"', text)
    if not m:
        sys.exit("main.c 里找不到 .home_title,无法推导 24px 字符集")
    return m.group(1)


def write_charset(path: Path, chars: set[str]) -> None:
    path.write_text(ASCII + "".join(sorted(chars)), encoding="utf-8")
    print(f"  {path.name}: {len(chars)} 个汉字/全角")


def generate(size: int, charset: Path, out: Path) -> None:
    syms = "".join(sorted(set(charset.read_text(encoding="utf-8").strip())))
    subprocess.run(
        [
            "npx", "--yes", "lv_font_conv@1.5.3",
            "--font", str(OTF), "--size", str(size), "--format", "lvgl", "--bpp", "4",
            "--lv-include", "lvgl.h", "--no-compress", "--force-fast-kern-format",
            "--symbols", syms, "-o", str(out),
        ],
        check=True,
    )
    print(f"  {out.name}: {out.stat().st_size} 字节")


def main() -> int:
    if not OTF.is_file():
        sys.exit(f"缺少源字体 {OTF.name},见 assets/fonts/README.md 的下载命令")

    print("推导字符集:")
    write_charset(FONTS / "radio_charset.txt", collect(SOURCES_16))
    write_charset(FONTS / "radio_charset_24.txt",
                  {c for c in home_title() if is_checked(c)})

    print("生成字体(需要 npx,首次会下载 lv_font_conv):")
    generate(16, FONTS / "radio_charset.txt", FONTS / "app_font_16.c")
    generate(24, FONTS / "radio_charset_24.txt", FONTS / "app_font_24.c")

    print("自检:")
    subprocess.run([sys.executable, str(ROOT / "tests" / "test_ui_charset.py")], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
