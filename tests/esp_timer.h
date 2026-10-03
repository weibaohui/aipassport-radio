// tests/esp_timer.h —— 主机测试桩:把 esp_timer_get_time 映射到单调时钟,
// 让带时间逻辑的模块(radio_biglist 的复探节流)能进主机测试。仅 tests/ 可见。
#pragma once

#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
