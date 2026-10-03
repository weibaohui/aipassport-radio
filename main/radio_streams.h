// main/radio_streams.h —— 电台列表与流 URL 解析(纯逻辑,零 ESP/LVGL 依赖)。
//
// 之所以拆成纯逻辑层:地址合法性、host:port 解析、列表增删改都是主机可测的
// 规则,放进 host tests 才能在不上设备的情况下把边界情况钉死。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_MAX_STATIONS  48    // 小清单容量(内置 6 + 用户 42;M3U 导入按此封顶)
#define RADIO_NAME_MAX      32    // 台名显示上限
// 256:合并清单(3473 台)URL 长度 p99=151、最长 252 —— 96 会截断约 120 条。
#define RADIO_URL_MAX       256   // 流 URL 长度上限
#define RADIO_HOST_MAX      64    // 解析出的 host[:port] 上限

typedef struct {
    char name[RADIO_NAME_MAX];  // NUL 结尾;空串视为无效条目
    char url[RADIO_URL_MAX];
} radio_station_t;

typedef struct {
    radio_station_t items[RADIO_MAX_STATIONS];
    uint8_t count;              // 有效条数 0..RADIO_MAX_STATIONS
} radio_list_t;

// 内置台:均为从设备所在网络实测可达的 HTTP MP3 流(非 TLS)。
// 保留内置台是为了设备开箱即用;用户可在门户里追加/删除自己的流。
void radio_list_builtin(radio_list_t *out);

// 逐条读内置台(数据在 flash rodata,不占 RAM;radio_store 物化清单用)。
int radio_builtin_count(void);
bool radio_builtin_get(int idx, radio_station_t *out);

// 清空(只留零条目)。
void radio_list_reset(radio_list_t *l);

// 追加或覆盖(同名视为改 URL)。返回 false:参数非法、URL 不合法、列表已满。
bool radio_list_add(radio_list_t *l, const char *name, const char *url);

// 按下标删除;越界返回 false。
bool radio_list_remove(radio_list_t *l, uint8_t index);

// 按台名查找,返回下标;未找到返回 -1。
int radio_list_find(const radio_list_t *l, const char *name);

// 流 URL 是否可用。只接受 http://(见 radio_url_hostport 的说明)。
bool radio_url_valid(const char *url);

// 从流 URL 解析出 TCP 连接用的 "host[:port]",写入 out。
// 缺省端口取 80。成功返回 true;URL 非法或 host 为空返回 false。
//
// http 与 https 都放行(播放器走 esp_http_client,TLS 按需加载)。但这台机器
// 无 PSRAM,96KB 堆要同时留给 LVGL、WiFi、TLS 握手缓冲和 MP3 解码器,https
// 流播放期间的 TLS 上下文(约 40KB 连续内存)经常挤不出来 —— 内置/出厂台
// 仍刻意全选 http 明文源,把 https 留给用户自加的台。
bool radio_url_hostport(const char *url, char *out, size_t out_len);
