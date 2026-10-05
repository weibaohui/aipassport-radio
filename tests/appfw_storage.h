// tests/appfw_storage.h —— 主机测试桩:radio_store.c 在主机上编译时,
// 用这个头顶替框架的 NVS 配置存储(实现在 test_radio_store.c 的内存版键值表)。
//
// 只在 tests/ 目录里可见:门禁给应用主机测试的 include 路径是 main → tests →
// thirdparty,固件构建不受影响(那份代码用 framework 里的真头文件)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 语义与 components/framework/appfw/include/appfw_storage.h 完全一致:
// get_str 未存=false 且 buf 置空;set_str 空串=存空串(读回为空,等同失效);
// get_u16 未存/值为 0 = false 且 *out=fallback。
bool appfw_store_get_str(const char *key, char *buf, size_t buf_len);
bool appfw_store_set_str(const char *key, const char *value);
bool appfw_store_get_u16(const char *key, uint16_t *out, uint16_t fallback);
bool appfw_store_set_u16(const char *key, uint16_t value);

// 测试专用:清空内存键值表(每个用例开头调用,等效"换了一台新机")。
void fake_store_reset(void);
// 测试专用:直接检查某键是否还存着非空值(迁移后旧键应已清)。
bool fake_store_has(const char *key);

// ---- appfw_mcp 主机测试所需(内存版桩,固定值) ----
// 真实类型在 appfw_netlist.h / appfw_storage.h;主机测试只走"没有已存热点"
// 和固定档位回显,不测 NVS 本身(那是 test_radio_store 的事)。
typedef struct {
    char ssid[33];
    char pwd[65];
} appfw_netlist_entry_t;

typedef struct {
    appfw_netlist_entry_t items[8];
    uint8_t count;
    int8_t selected;
} appfw_netlist_t;

static inline bool appfw_store_netlist_load(appfw_netlist_t *list)
{
    list->count = 0;
    list->selected = -1;
    return false;
}
static inline bool appfw_store_get_screen_off(uint16_t *out) { *out = 300; return true; }
static inline bool appfw_store_set_screen_off(uint16_t v) { (void)v; return true; }
static inline bool appfw_store_get_period(uint16_t *out) { *out = 300; return true; }
static inline bool appfw_store_set_period(uint16_t v) { (void)v; return true; }

static inline bool appfw_store_get_brightness(uint16_t *pct) { *pct = 100; return true; }
static inline bool appfw_store_set_brightness(uint16_t pct) { (void)pct; return true; }

static inline uint32_t appfw_storage_app_image_used(const void *p) { (void)p; return 0; }
