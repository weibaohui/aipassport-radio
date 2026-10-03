// main/radio_biglist.h —— 大清单:files 分区里的整份 M3U + 偏移索引。
//
// 3000+ 台装不进 24KB 的 NVS(逐条 radio_store 只适合 ≤48 台的小清单),改存
// 1MB FAT files 分区(门户「文件管理」页可上传,单文件 ≤512KB):
//   /files/radio.m3u   清单本体(用户上传的合并清单,498KB/3473 台)
//   /files/radio.idx   偏移索引(每条 4B 的 #EXTINF 行偏移,3473×4B≈14KB)
// RAM 里只留条数与文件大小;浏览/播放都是"按需读一条":列表页画哪 5 行就读
// 哪 5 条(FATFS 上每次 fopen+lseek+fgets ≈ 1-3ms),开播时才取当前 1 条。
//
// 索引有效性 = 头部魔数/版本/条数自洽 且 记录的 m3u 字节数与当前文件一致;
// 不一致(换传了新清单)自动重建。上传新清单后无需重启:radio_biglist_poll
// 节流复检(每秒一次),发现大小变化即重建索引。
//
// 大清单模式是"只读"的:增删改在电脑上编辑 m3u 后重新上传(门户 JSON 导入
// 上限 4KB 装不下整表);设备上「恢复出厂」删除这两个文件退回小清单模式。
#pragma once

#include <stdbool.h>

#include "radio_streams.h"

// 每次开机调用一次;此后 poll 由 radio_store_count 等高频入口带动。
// 文件不存在 → 不可用(radio_store 走小清单),每秒复探是否新上传。
void radio_biglist_init(void);

// 复检清单/索引是否就绪。就绪态每次调用只做一次 fopen+ftell 轻探(看文件
// 有没有换);大小连续两轮一致才走校验/重建(上传中的文件每轮都在长,不动)。
// 文件不存在 → 不可用(radio_store 走小清单),每 5s 复探是否新上传。
bool radio_biglist_poll(void);

// 大清单是否可用(决定 radio_store 是否委托与只读)。
bool radio_biglist_available(void);

// 条数(0 = 不可用)。
int radio_biglist_count(void);

// 读第 idx 条(索引 4B + m3u 两行;不占常驻内存)。
bool radio_biglist_get(int idx, radio_station_t *out);

// 按台名找下标:整份 m3u 流式线性扫(498KB ≈ 数百 ms,兜底路径;主路径是
// 播放下标缓存)。未找到 -1。
int radio_biglist_find(const char *name);

// 恢复出厂:删除 m3u 与索引并复位状态(之后 poll 不可用,radio_store 退回
// NVS 小清单并物化出厂清单)。
void radio_biglist_discard(void);

// 仅测试用:改写 m3u/idx 所在目录(默认 /files),主机测试指到临时目录。
void radio_biglist_set_dir(const char *dir);
