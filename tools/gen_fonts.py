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
# 凡是会调 lv_label_set_text* 的文件都要列进来 —— 漏一个,那个文件里的
# 汉字就不在字库里,真机上显示成空白(播放页就踩过这个)。
# radio_streams.c 也必须在列:电台名直接进 lv_label_set_text(),
# 不加的话内置台名全是空白。
SOURCES_16 = [
    ROOT / "main" / "radio_catalog.h",   # 台名数据:直接扫,不依赖仓外清单文件
    FW / "appfw" / "src" / "appfw_ui.c",
    ROOT / "main" / "radio_pages.c",
    ROOT / "main" / "radio_viz_view.c",
    ROOT / "main" / "radio_streams.c",
    ROOT / "main" / "main.c",
]
# 24px 用于顶栏标题(main.c 的 .home_title)和播放页台名(radio_streams.c 的
# 电台名)。台名不收进 24px 的话,播放页大字全是方框(2026-10-03 真机踩坑)。
SOURCES_24 = [ROOT / "main" / "main.c", ROOT / "main" / "radio_streams.c",
              FW / "appfw" / "src" / "appfw_ui.c"]

# 大清单的台名是运行时数据,但集合是已知的:把清单文件的台名一并收进字库,
# 否则翻到某个台就是一排方框(2026-10-03 真机踩坑)。文件不在(换机器)时
# 跳过,字库退回"只覆盖源码"。
LIST_M3U = Path("/Users/weibh/Desktop/转写/radio-stations/m3u/11-可播放清单.m3u")


def list_name_chars(path: Path) -> set[str]:
    if not path.is_file():
        print(f"  (跳过台名清单: {path} 不存在)")
        return set()
    out: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#EXTINF:") and "," in line:
            out |= {c for c in line.split(",", 1)[1] if is_checked(c)}
    return out


PUNCT = set("，。：；？！（）《》【】—…·℃─′″")
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


def is_checked(ch: str) -> bool:
    cp = ord(ch)
    # CJK 基本区+扩展A、CJK 标点、全角形式、拉丁补充(ü 等)、常用引号破折号。
    return (0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF or
            0x3000 <= cp <= 0x303F or 0xFF00 <= cp <= 0xFFEF or
            0x00C0 <= cp <= 0x00FF or ch in PUNCT)


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
    names = list_name_chars(LIST_M3U)
    print(f"  台名额外贡献 {len(names)} 字")
    write_charset(FONTS / "radio_charset.txt", collect(SOURCES_16) | names)
    write_charset(FONTS / "radio_charset_24.txt", collect(SOURCES_24) | names)

    # 16px 字库的完整码点表(升序),给曲名显示做白名单:流里的歌名是动态
    # 文本,字库外的字只能显示成方框 —— 显示前直接过滤掉(见 radio_pages.c)。
    cps = sorted(ord(c) for c in collect(SOURCES_16) | names if ord(c) > 0x7F)
    lines = [f"    0x{cp:04X}," for cp in cps]
    table = (
        "// 由 tools/gen_fonts.py 自动生成 —— 16px 字库收录的非 ASCII 码点(升序)。\n"
        "// 曲名等动态文本显示前按此表过滤,字库外的字不进 LVGL(否则是方框)。\n"
        "#include <stdint.h>\n\n"
        f"#define RADIO_TITLE_CP_COUNT {len(cps)}\n"
        "static const uint16_t k_radio_title_cps[RADIO_TITLE_CP_COUNT] = {\n"
        + "\n".join(lines) + "\n};\n"
    )
    out_header = ROOT / "main" / "radio_title_table.h"
    out_header.write_text(table, encoding="utf-8")
    print(f"  {out_header.name}: {len(cps)} 个码点")

    print("生成字体(需要 npx,首次会下载 lv_font_conv):")
    generate(16, FONTS / "radio_charset.txt", FONTS / "app_font_16.c")
    generate(24, FONTS / "radio_charset_24.txt", FONTS / "app_font_24.c")

    print("自检:")
    subprocess.run([sys.executable, str(ROOT / "tests" / "test_ui_charset.py")], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
