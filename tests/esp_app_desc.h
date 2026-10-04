// tests/esp_app_desc.h —— 主机测试桩:esp_app_get_description 只有固件里
// 存在(读镜像头);主机测试给固定值。
#pragma once

#include <stdint.h>

typedef struct {
    char version[32];
    char project_name[20];
} esp_app_desc_t;

static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "9.9.9", "host-test" };
    return &d;
}
