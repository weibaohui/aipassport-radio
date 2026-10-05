#!/usr/bin/env python3
"""中文字形覆盖门禁。

字库在框架仓(components/framework/appfw/fonts/,常见 3500 字全量),本测试
做三级校验:

1. 受检源码字符串里的每个 CJK/全角码点,都必须出现在框架字符清单里
   (清单外的新字 → 补清单并重跑 gen_fonts.py);
2. 框架生成的 .c 字体里必须真的包含清单的全部非 ASCII 码点
   (清单改了但没重新生成字体的证据级校验);
3. main/radio_title_table.h 必须与框架清单一致
   (子模块更新后忘了重跑 tools/gen_title_table.py 会在这里挡住)。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "components" / "framework"
CHARSET = FW / "appfw" / "fonts" / "appfw_common_charset.txt"
FONT_16 = FW / "appfw" / "fonts" / "app_font_16.c"
FONT_24 = FW / "appfw" / "fonts" / "app_font_24.c"
TITLE_TABLE = ROOT / "main" / "radio_title_table.h"

# 会把文本送进 LVGL 渲染的源码:框架 UI + 本应用 UI + 内置台名数据。
# 台名/曲名是动态文本,由 radio_title_table.h 过滤,不在此列。
SOURCES = [
    FW / "appfw" / "src" / "appfw_ui.c",
    ROOT / "main" / "radio_pages.c",
    ROOT / "main" / "radio_viz_view.c",
    ROOT / "main" / "radio_streams.c",
    ROOT / "main" / "radio_catalog.h",
    ROOT / "main" / "main.c",
]

# 需要覆盖的字符范围:CJK 统一表意文字 + 本应用用到的全角标点/符号。
PUNCT = set("，。：；？！（）《》—…·℃─")


def is_checked(ch: str) -> bool:
    cp = ord(ch)
    return 0x4E00 <= cp <= 0x9FFF or ch in PUNCT


def strip_comments(text: str) -> str:
    """先剥注释再扫字面量:注释里带引号的中文不是屏显文案,不剥会误报。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def string_literal_chars(path: Path) -> set[str]:
    """源码里 C 字符串字面量中出现的中文/全角字符(注释已剥离)。"""
    text = strip_comments(path.read_text(encoding="utf-8"))
    out: set[str] = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        out |= {c for c in lit if is_checked(c)}
    return out


def charset_file(path: Path) -> set[str]:
    return set(path.read_text(encoding="utf-8").strip())


class FontFormatError(RuntimeError):
    """生成物结构与预期不符——宁可大声报错,也不要静默通过。"""


def font_unicode_codepoints(path: Path) -> set[int]:
    """从生成的 LVGL 字体 .c 中取出真实收录的码点。

    直接解析生成物比"清单里有"更能说明问题——清单可以手改,.c 不能。

    关键:lv_font_conv 的 cmaps[] 统一用「相对码点」存储,公式是
        unicode = range_start + i
    连续区间(FORMAT0_TINY,如 ASCII)直接是 i = 0..range_length-1;
    稀疏区间(SPARSE_TINY)由 unicode_list_N[] 显式列出 i,例如 16px 字库的
        .range_start = 183, unicode_list_1 = {0x0, 0x1f5d, 0x4d53, ...}
    解出来是 0xB7(·)、0x2014(—)、0x4E0A(上)。把这些偏移误当绝对码点,
    会得到一份"全都没有字形"的假报告。
    """
    text = path.read_text(encoding="utf-8")

    # 稀疏表:unicode_list_N[] = { 0x..., ... }
    lists: dict[str, list[int]] = {}
    for name, body in re.findall(r"(unicode_list_\d+)\[\]\s*=\s*\{(.*?)\}", text, re.S):
        lists[name] = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body)]

    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise FontFormatError(f"{path.name}: 找不到 cmaps[] 表")

    covered: set[int] = set()
    blocks = re.findall(r"\{([^{}]*)\}", m.group(1))
    if not blocks:
        raise FontFormatError(f"{path.name}: cmaps[] 里没有条目")

    for block in blocks:
        sm = re.search(r"\.range_start\s*=\s*(\d+)", block)
        lm = re.search(r"\.range_length\s*=\s*(\d+)", block)
        um = re.search(r"\.unicode_list\s*=\s*(\w+)", block)
        if not (sm and lm and um):
            raise FontFormatError(f"{path.name}: cmaps 条目缺字段\n{block.strip()[:200]}")
        start, length = int(sm.group(1)), int(lm.group(1))
        name = um.group(1)

        if name == "NULL":
            covered |= set(range(start, start + length))
            continue

        if name not in lists:
            raise FontFormatError(f"{path.name}: cmaps 引用了不存在的表 {name}")
        offsets = lists[name]
        # 自检:range_length 应恰好覆盖最大偏移。差分编码若理解错了,这里先炸。
        if offsets and max(offsets) + 1 > length:
            raise FontFormatError(
                f"{path.name}: 偏移 {max(offsets)} 超出 range_length {length},"
                f"差分编码假设不成立"
            )
        covered |= {start + off for off in offsets}

    return covered


def main() -> int:
    problems: list[str] = []

    if not CHARSET.is_file():
        print(f"字形覆盖检查失败:缺少框架字符清单 {CHARSET}")
        return 1
    charset = charset_file(CHARSET)

    # 第一级:源码文案 ⊆ 框架字符清单
    used: set[str] = set()
    for path in SOURCES:
        if not path.is_file():
            problems.append(f"缺少受检源码: {path}")
            continue
        used |= string_literal_chars(path)
    for ch in sorted(used - charset):
        problems.append(f"框架字库缺少 U+{ord(ch):04X} {ch!r}"
                        f"(补入 appfw_common_charset.txt 并重新生成)")

    # 第二级:框架生成物真的收录了清单里的全部字形
    for label, font_path in (("16px", FONT_16), ("24px", FONT_24)):
        if not font_path.is_file():
            problems.append(f"缺少框架生成的字体文件: {font_path}")
            continue
        try:
            covered = font_unicode_codepoints(font_path)
        except FontFormatError as exc:
            problems.append(f"{label} {exc}")
            continue
        if not covered:
            problems.append(f"{label} 字体 {font_path.name} 里解析不到 unicode_list,无法校验覆盖")
            continue
        want = {ord(c) for c in charset if ord(c) > 0x7F}
        for cp in sorted(want - covered):
            problems.append(f"{label} 生成物缺少字形 U+{cp:04X}"
                            f"(清单改了但字库没重新生成,重跑 appfw/fonts/gen_fonts.py)")

    # 第三级:曲名过滤表与框架清单同步
    if TITLE_TABLE.is_file():
        tt = TITLE_TABLE.read_text(encoding="utf-8")
        have = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4}),", tt)}
        want = {ord(c) for c in charset if ord(c) > 0x7F}
        stale = sorted(want - have)
        extra = sorted(have - want)
        for cp in stale:
            problems.append(f"radio_title_table.h 缺 0x{cp:04X}(框架清单更新后"
                            f"须重跑 tools/gen_title_table.py)")
        for cp in extra:
            problems.append(f"radio_title_table.h 多出 0x{cp:04X}(清单已不含,重跑生成)")
    else:
        problems.append(f"缺少 {TITLE_TABLE},重跑 tools/gen_title_table.py")

    if problems:
        print("字形覆盖检查失败:")
        for line in problems:
            print(f"  {line}")
        return 1

    ncjk = len([c for c in charset if 0x4E00 <= ord(c) <= 0x9FFF])
    print(f"字形覆盖检查通过:文案 {len(used)} 个受检字符 ⊆ 框架字库"
          f"(含常用字 {ncjk} 个);曲名表 {TITLE_TABLE.name} 同步")
    return 0


if __name__ == "__main__":
    sys.exit(main())
