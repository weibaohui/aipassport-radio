#!/usr/bin/env python3
"""中文字形覆盖门禁。

只查"字符清单里有没有"是不够的:清单与生成物可能不同步。因此这里做两级校验:

1. 受检源码字符串里的每个 CJK/全角码点,都必须出现在对应字体的字符清单里;
2. 该码点还必须真的出现在生成的 .c 字体的 unicode_list 中——这才是设备上
   能否显示的证据。

两个字号分别检查:16px 承载全部 UI 文本;24px 只用于顶栏标题(home_title),
见 appfw_ui.c 中 s_font24 的唯一使用点。框架的 appfw_ui.c 也用应用提供的
字库渲染,故一并受检。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "components" / "framework"
CHARSET_16 = ROOT / "assets" / "fonts" / "radio_charset.txt"
CHARSET_24 = ROOT / "assets" / "fonts" / "radio_charset_24.txt"
FONT_16 = ROOT / "assets" / "fonts" / "app_font_16.c"
FONT_24 = ROOT / "assets" / "fonts" / "app_font_24.c"

# 16px 承载的源码:框架 UI + 本应用 UI。必须和 tools/gen_fonts.py 的同名
# 列表一致 —— 两边都漏掉某个文件,门禁就会跟着一起漏,等于没查。
SOURCES_16 = [
    FW / "appfw" / "src" / "appfw_ui.c",
    ROOT / "main" / "radio_pages.c",
    ROOT / "main" / "radio_viz_view.c",
    ROOT / "main" / "radio_streams.c",
    ROOT / "main" / "main.c",
]

# 需要覆盖的字符范围:CJK 统一表意文字 + 本应用用到的全角标点/符号。
PUNCT = set("，。：；？！（）《》—…·℃─")


def is_checked(ch: str) -> bool:
    cp = ord(ch)
    return 0x4E00 <= cp <= 0x9FFF or ch in PUNCT


def strip_comments(text: str) -> str:
    """先剥注释再扫字面量:注释里带引号的中文(如"假频谱")不是屏显文案,
    不剥的话门禁会误报。与 aipassport-appfw 仓的同名门禁行为一致。"""
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

    used: set[str] = set()
    for path in SOURCES_16:
        if not path.is_file():
            problems.append(f"缺少受检源码: {path}")
            continue
        used |= string_literal_chars(path)

    cs16 = charset_file(CHARSET_16)
    missing = sorted(used - cs16)
    for ch in missing:
        problems.append(f"16px 字符集缺少 U+{ord(ch):04X} {ch!r}")

    # 第二级:生成物里是否真的有这些字形
    for label, font_path, cs in (
        ("16px", FONT_16, cs16),
        ("24px", FONT_24, charset_file(CHARSET_24)),
    ):
        if not font_path.is_file():
            problems.append(f"缺少生成的字体文件: {font_path}")
            continue
        try:
            covered = font_unicode_codepoints(font_path)
        except FontFormatError as exc:
            problems.append(f"{label} {exc}")
            continue
        if not covered:
            problems.append(f"{label} 字体 {font_path.name} 里解析不到 unicode_list,无法校验覆盖")
            continue
        want = {ord(c) for c in cs if ord(c) > 0x7F}
        gap = sorted(want - covered)
        for cp in gap:
            problems.append(f"{label} 生成物缺少字形 U+{cp:04X} (清单里有但没生成出来)")

    # 24px 只需覆盖顶栏标题。标题改动而字库没重生成,是这里要挡住的坑。
    main_c = (ROOT / "main" / "main.c").read_text(encoding="utf-8")
    m = re.search(r'\.home_title\s*=\s*"([^"]*)"', main_c)
    if m:
        cs24 = charset_file(CHARSET_24)
        for ch in m.group(1):
            if is_checked(ch) and ch not in cs24:
                problems.append(
                    f"顶栏标题含 {ch!r} 但 24px 字符集没有;24px 只用于标题,"
                    f"改标题后必须重新生成 app_font_24.c"
                )
    else:
        problems.append("main.c 里找不到 .home_title,无法校验 24px 覆盖")

    if problems:
        print("字形覆盖检查失败:")
        for line in problems:
            print(f"  {line}")
        print("请把缺字补入 assets/fonts/radio_charset*.txt 并重新生成字体。")
        return 1

    n16 = len([c for c in cs16 if ord(c) > 0x7F])
    n24 = len([c for c in charset_file(CHARSET_24) if ord(c) > 0x7F])
    print(f"字形覆盖检查通过:16px 覆盖 {len(used)} 个受检汉字;清单非 ASCII {n16} 字,"
          f"24px 标题字 {n24} 字")
    return 0


if __name__ == "__main__":
    sys.exit(main())
