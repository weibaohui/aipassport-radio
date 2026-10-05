// tests/esp_log.h —— 主机测试桩:把 ESP-IDF 日志宏映射到 printf,
// 让带日志的模块(radio_store 等)能进主机测试。仅 tests/ 可见。
#pragma once

#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)

// ---- appfw_mcp 诊断工具所需 ----
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
               ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static inline void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    (void)tag; (void)level;
}
