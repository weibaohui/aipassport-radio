// main/radio_store.h —— 电台清单存储:固件台目(只读 rodata) + 自定义台(逐条 NVS)。
//
// 动机:C3 无 PSRAM,SRAM 每 KB 都要留给 LVGL/WiFi/TLS/解码器。清单常驻 RAM
// 的旧实现合计 ~31KB 静态内存;改为"台目走 flash 内存映射 + 自定义台逐条落
// NVS"后,RAM 里只留一个自定义条数;列表页画哪 5 行就读哪 5 条,开播时只取
// 当前 1 条。
//
// 台目 = 内置 6 精品 + 固件内嵌 RADIO_CATALOG(339 台,tools/gen_catalog.py
// 生成),纯 rodata 不占 RAM,永不可删改。自定义台 = 键 u_cnt + u0..u99 =
// "台名\tURL",上限 RADIO_MAX_STATIONS;NVS 分区 24KB 是真实天花板——地址
// 都很长时可能先写满,add 会干净地返回 false。
//
// 一致性模型:单用户设备,写只来自门户(httpd 任务)/MCP 任务与启动迁移,
// 读来自 LVGL/输入任务。不加锁——NVS 单键读写本身线程安全。
#pragma once

#include "radio_streams.h"

// 台目基数(内置精品 + 固件台目),恒可播、下标 0..count-1 的只读段。
int radio_store_catalog_count(void);

// 启动调用一次(须在 appfw_store_init 之后、建 UI 之前):
//   - 读自定义台数(键 u_cnt);
//   - 发现旧版清单键(r_cnt/r0..r47,内容全是台目副本)→ 一次性清空腾 NVS。
void radio_store_init(void);

// 当前条数 = 台目基数 + 自定义台数。RAM 里唯一常驻的清单状态是自定义条数。
int radio_store_count(void);

// 读第 idx 条。out 为 128B 的 radio_station_t,调用方栈上放即可。
bool radio_store_get(int idx, radio_station_t *out);

// 按台名找下标:自定义台优先(同名改址的用户版本优先于台目),未找到 -1。
int radio_store_find(const char *name);

// 追加自定义台;同名(自定义列表内)视为改地址。非法/已满返回 false。
bool radio_store_add(const char *name, const char *url);

// 删除下标,其后自定义条目整体前移。台目段(idx < catalog_count)拒删。
bool radio_store_remove(int idx);

// 清空全部自定义台(门户整表导入 / MCP 导入 / 回出厂的起点)。台目不受影响。
void radio_store_import_begin(void);

// 回出厂状态:清空自定义台,返回条数(= 台目基数)。
int radio_store_restore_factory(void);
