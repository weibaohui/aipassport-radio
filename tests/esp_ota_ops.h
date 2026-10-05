// tests/esp_ota_ops.h —— 主机测试桩:appfw_mcp 的设备信息工具取运行分区
// 算程序占用;主机上没有分区表,返回 NULL(工具侧已有 NULL 容忍)。
#pragma once

typedef struct {
    unsigned size;
} esp_partition_t;

static inline const esp_partition_t *esp_ota_get_running_partition(void)
{
    return 0;
}
