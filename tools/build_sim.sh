#!/usr/bin/env bash
# 在主机上构建并运行屏幕模拟器,输出 PNG 到 build/sim/。
#
# 不需要 ESP-IDF、不需要 SDL2 窗口:只用 LVGL + 应用自己的可视化模块,
# 渲染 240x320 的真实像素。看到的和设备上的一致(同一套字库、同一尺寸)。
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="${root}/build/sim"
lvgl="${root}/managed_components/lvgl__lvgl"
src="${lvgl}/src"

if [[ ! -d "${src}" ]]; then
    echo "缺少 ${src};请先在 ${root} 跑一次 idf.py build 把组件拉下来。" >&2
    exit 1
fi

mkdir -p "${out}"

# LVGL 自身的源文件(排除示例/移植层,只要核心 + 我们用到的 widget)
mapfile -t lvgl_srcs < <(find "${src}" -name '*.c' \
    ! -path '*/demos/*' ! -path '*/examples/*' ! -path '*/tests/*' \
    ! -path '*/drivers/*' ! -path '*/env_support/*' \
    -print)

cc -std=gnu11 -O2 \
    -I "${root}/tools/sim" \
    -I "${root}/main" \
    -I "${lvgl}" -I "${src}" \
    -DLV_CONF_INCLUDE_SIMPLE \
    -D_GNU_SOURCE \
    "${root}/tools/sim.c" \
    "${root}/main/radio_viz.c" \
    "${root}/main/radio_viz_view.c" \
    "${root}/assets/fonts/app_font_16.c" \
    "${root}/assets/fonts/app_font_24.c" \
    "${lvgl_srcs[@]}" \
    -lm -o "${out}/sim"

echo "模拟器已构建,渲染中:"
"${out}/sim" "${out}"
echo
echo "输出目录:${out}"
