// tests/appfw_files.h —— 主机测试桩:radio_biglist.c 在主机上编译时,用这个
// 头顶替框架的文件管理接口。真框架的懒挂载在主机上不存在,直接返回就绪。
#pragma once

#include <stdbool.h>

static inline bool appfw_files_ensure_mounted(void) { return true; }
static inline int appfw_files_unmount(void) { return 0; }
