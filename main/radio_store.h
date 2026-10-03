// main/radio_store.h —— 电台清单的 flash 存储(逐条 NVS,按需加载)。
//
// 动机:C3 无 PSRAM,SRAM 每 KB 都要留给 LVGL/WiFi/TLS/解码器。旧实现把整份
// 清单(48 台 × 128B ≈ 6KB)常驻 RAM,门户/启动路径还各有整表临时副本,合计
// ~31KB 静态内存。改为逐条落 NVS 后,RAM 里只留一个条数;列表页画哪 5 行就读
// 哪 5 条,开播时只取当前 1 条,不播列表时清单完全不在内存里。
//
// 存储布局:键 r_cnt = 条数;键 r0..r47 = "台名\tURL"。前 6 条按固定顺序物化
// 内置台(开箱即用、门户可改址但不可删)。
//
// 一致性模型:单用户设备,写只来自门户(httpd 任务)与启动迁移,读来自
// LVGL/输入任务。不加锁——与旧实现一致;最坏情形是删除与浏览并发时某一屏
// 读到移动中的一条,下一轮 500ms 轮询自愈。NVS 单键读写本身线程安全。
#pragma once

#include "radio_streams.h"

// 启动调用一次(须在 appfw_store_init 之后、建 UI 之前):
//   - NVS 里已有清单(有 r_cnt)→ 只回读条数;
//   - 空机 → 物化出厂清单(内置 6 + 固件内嵌出厂 42);
//   - 发现旧版整份 M3U 键(radio_m3u)→ 迁移为逐条,迁移完释放旧键。
void radio_store_init(void);

// 当前条数(0..RADIO_MAX_STATIONS)。RAM 里唯一常驻的清单状态。
int radio_store_count(void);

// 读第 idx 条。out 为 128B 的 radio_station_t,调用方栈上放即可。
bool radio_store_get(int idx, radio_station_t *out);

// 按台名找下标(逐条读 flash 线性扫,最坏 48 次小读);未找到 -1。
int radio_store_find(const char *name);

// 追加;同名视为改地址(含改内置台地址)。非法/已满返回 false。
bool radio_store_add(const char *name, const char *url);

// 删除下标,其后条目整体前移。与内置台完全一致的条目拒删。
bool radio_store_remove(int idx);

// 整表替换(门户导入)的起点:清空全部条目,随后用 radio_store_add 逐条装回。
void radio_store_import_begin(void);

// 恢复出厂清单(内置 6 + 出厂 42),返回条数。
int radio_store_restore_factory(void);

// 与内置台名称、地址都完全一致(门户"内置台"标记与拒删判定用)。
bool radio_store_is_builtin(const char *name, const char *url);
