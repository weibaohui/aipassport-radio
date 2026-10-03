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
