// main/radio_m3u.h —— M3U 播放列表解析/序列化(纯逻辑,主机可测)。
//
// 电台清单的交换格式定稿为 M3U(2026-10-03):导入导出、门户 HTTP、NVS 持久化
// 全走同一种格式,换设备/换清单就是一份 .m3u 文件的事。
//
// 解析支持:#EXTM3U 头、#EXTINF:<dur>[ attrs],<标题> 与 URL 成对(标题里
// 允许逗号,取第一个逗号之后的部分)、CRLF、其他 # 注释行忽略。
// 筛选(只收 http 直链、剔除 HLS)是应用策略,经 accept 回调注入,本模块
// 保持格式纯粹。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_streams.h"

// 条目缓冲与 radio_station_t 同构,便于整块搬运。
struct radio_m3u_entry {
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
};

typedef struct {
    int extinf;     // 文件里的 EXTINF 条目总数
    int accepted;   // 通过筛选写入 out 的条数
    int rejected;   // 被筛选拒绝/超出容量的条数
} radio_m3u_stats_t;

// 条目筛选回调:返回 false = 跳过该条。NULL = 全收。
typedef bool (*radio_m3u_accept_fn)(const char *name, const char *url, void *user);
// 条目接收回调:每个通过筛选的条目回调一次(应用在此做合并/入表)。
typedef void (*radio_m3u_entry_fn)(void *user, const char *name, const char *url);

// 解析 M3U 文本。通过筛选的条目逐条回调 on_entry(user, name, url);
// 返回通过筛选并已交付的条数。无 EXTINF 配对的裸 URL 行忽略;
// 标题/URL 按各自上限截断。
size_t radio_m3u_parse(const char *text,
                       void *accept_user, radio_m3u_accept_fn accept,
                       void *entry_user, radio_m3u_entry_fn on_entry,
                       radio_m3u_stats_t *stats);

// 序列化为 M3U 文本(#EXTM3U 头 + EXTINF/URL 对)。返回写入长度(不含 NUL);
// 缓冲不足时返回 0(调用方应给 count*(RADIO_NAME_MAX+RADIO_URL_MAX) 量级)。
size_t radio_m3u_serialize(const struct radio_m3u_entry *entries, uint8_t count,
                           char *out, size_t cap);
