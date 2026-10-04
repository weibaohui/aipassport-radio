#!/usr/bin/env python3
"""把精选清单 m3u 生成为固件内置台目(radio_catalog.h)。

用法:
    python3 tools/gen_catalog.py [m3u 路径]
    # 默认读 /Users/weibh/Desktop/转写/radio-stations/m3u/11-可播放清单.m3u

生成 main/radio_catalog.h:static const 数组,flash rodata,C3 上零 RAM 占用。
换清单内容的完整流程:重跑筛选脚本 → gen_catalog.py → gen_fonts.py(台名
可能变)→ 刷机。
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_M3U = Path("/Users/weibh/Desktop/转写/radio-stations/m3u/11-可播放清单.m3u")
OUT = ROOT / "main" / "radio_catalog.h"


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_M3U
    if not src.is_file():
        sys.exit(f"缺少清单: {src}")

    pairs, name = [], None
    for line in src.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("#EXTINF:"):
            name = line.split(",", 1)[1].strip() if "," in line else None
        elif line and not line.startswith("#") and name:
            pairs.append((name, line))
            name = None

    lines = [
        "#pragma once",
        "// main/radio_catalog.h —— 内置台目(由 tools/gen_catalog.py 生成,勿手改)。",
        f"// 来源: {src.name},全部经过 8 秒持续供流测试、码率 ≤130kbps。",
        "// flash rodata 常量:ESP32-C3 上 rodata 走内存映射,不占 RAM。",
        '#include "radio_streams.h"',
        "",
        "static const radio_station_t RADIO_CATALOG[] = {",
    ]
    for n, u in pairs:
        nb, ub = n.encode(), u.encode()
        if len(nb) >= 32 or ub == b"" or len(ub) >= 256:
            sys.exit(f"条目超长/URL 为空,先修清单: {n!r} ({len(nb)}B) {u[:40]!r}")
        lines.append(f'    {{ "{c_escape(n)}", "{c_escape(u)}" }},')
    lines += [
        "};",
        "#define RADIO_CATALOG_N ((int)(sizeof(RADIO_CATALOG) / sizeof(RADIO_CATALOG[0])))",
    ]
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{OUT.name}: {len(pairs)} 台,{OUT.stat().st_size} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
