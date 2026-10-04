// tests/esp_heap_caps.h —— 主机测试桩:radio_biglist.c 挂载前看一眼最大块
// 决定要不要借洞;appfw_mcp 的设备信息工具读空闲堆。主机上内存充裕,恒大数。
#pragma once

#include <stddef.h>

#define MALLOC_CAP_INTERNAL 1

static inline size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 1 << 20;
}

static inline size_t heap_caps_get_free_size(int caps)
{
    (void)caps;
    return 200 * 1024;
}
