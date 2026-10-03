// tests/esp_heap_caps.h —— 主机测试桩:radio_biglist.c 挂载前看一眼最大块
// 决定要不要借洞;主机上内存充裕,恒返回大数(永不借洞)。
#pragma once

#include <stddef.h>

#define MALLOC_CAP_INTERNAL 1

static inline size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return 1 << 20;
}
