// main/radio_streams.c —— 见 radio_streams.h。本文件不依赖 ESP-IDF/LVGL。
#include "radio_streams.h"

#include <string.h>

// 内置台。URL 全部为 2026-10 在设备所在网络逐个实测通过的 http:// 裸 MP3 流。
//
// 为什么全是国内台:原内置的 KEXP(西雅图)和 Radio Paradise(加州)都是跨境流,
// 实测下载速率虽然够(KEXP 220kbps、Paradise 91kbps),但跨境链路的抖动和
// 首包时延不受控,表现为播放断续。既然目的是"随手能听",就该用本地源。
//
// 选台标准(实测数据,两次独立 8s 采样):
//   - 两次采样都 > 20KB,排除"只回个头就断"
//   - 实测码率 ≤ 130 kbps。C3 + 32KB LVGL 池的舒适区;实测 AsiaFM 亚洲热歌
//     231kbps、亚洲经典 170kbps 就偏高,没选
//   - 字节流里 MP3 同步字 > 200,确认是裸 MP3 而不是 AAC/AAC+/HLS 换壳
//
// 已排除的常见候选(都有实测依据,不是拍脑袋):
//   - ctt.rgd.com.cn(广东各台)  域名已解析不了
//   - radio.sxtvs.com(陕西)      Empty reply
//   - 蜻蜓 live/20207761、live/20500195  两次都只回 119 字节
//   - 所有 *.m3u8 / *.ogg        HLS 和 Ogg 容器,固件没有解封装器,只吃裸 MP3
//   - 所有 https://              radio_url_valid() 明确拒绝:https 要常驻 TLS
//                               状态机,本机 DRAM 余量放不下
//
// 注意:本文件必须列进 tools/gen_fonts.py 的 SOURCES_16 —— 台名会直接进
// lv_label_set_text(),漏了就真机上显示成空白。
static const radio_station_t BUILTIN[] = {
    { "上海交通广播 FM105.7", "http://lhttp.qingting.fm/live/266/64k.mp3" },
    { "中国之声",             "http://lhttp.qingting.fm/live/15318317/64k.mp3" },
    { "上海东广新闻广播",     "http://lhttp.qingting.fm/live/275/64k.mp3" },
    { "大连音乐广播",         "http://lhttp.qingting.fm/live/1084/64k.mp3" },
    { "CityFM 城市音乐台",    "http://lhttp.qingting.fm/live/20500153/64k.mp3" },
    { "三亚旅游之声 103.8",   "http://lhttp.qingting.fm/live/15318203/64k.mp3" },
};
#define BUILTIN_N ((uint8_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

void radio_list_builtin(radio_list_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < BUILTIN_N && i < RADIO_MAX_STATIONS; i++) {
        out->items[i] = BUILTIN[i];
        out->count++;
    }
}

void radio_list_reset(radio_list_t *l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
}

bool radio_url_valid(const char *url)
{
    if (!url) return false;
    const size_t len = strlen(url);
    if (len == 0 || len >= RADIO_URL_MAX) return false;
    // 只收 http://。https:// 需要常驻 TLS 状态,本机内存预算放不下;
    // 误填 https 应当明确拒绝,而不是悄悄降级或连不上后只报"连接失败"。
    static const char prefix[] = "http://";
    if (strncmp(url, prefix, sizeof(prefix) - 1) != 0) return false;
    // 空白字符会让 http_client 解析出奇怪的请求行,直接拒。
    for (size_t i = 0; i < len; i++) {
        const char c = url[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c < 0x20) return false;
    }
    // scheme 之后必须有 host。
    const char *rest = url + sizeof(prefix) - 1;
    const char *slash = strchr(rest, '/');
    const size_t host_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (host_len == 0) return false;
    // host 段内不允许再出现 '@'(内嵌凭据)或 '?'(无路径时紧跟 host 的 query,
    // 会让请求行变得含糊)。路径之后的 query 是允许的——部分电台用它传 token。
    for (size_t i = 0; i < host_len; i++) {
        if (rest[i] == '@' || rest[i] == '?') return false;
    }
    return true;
}

bool radio_url_hostport(const char *url, char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!radio_url_valid(url)) return false;

    const char *rest = url + sizeof("http://") - 1;
    const char *slash = strchr(rest, '/');
    const size_t host_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (host_len == 0 || host_len + 1 > RADIO_HOST_MAX) return false;

    // 端口:host 段里最后一个 ':' 之后、且不含非数字。
    const char *colon = NULL;
    for (size_t i = 0; i < host_len; i++) {
        if (rest[i] == ':') colon = rest + i;
    }
    size_t name_len = host_len;
    int have_port = 0;
    if (colon) {
        const char *p = colon + 1;
        if (p == rest + host_len) return false;      // 结尾冒号,非法
        for (const char *q = p; q < rest + host_len; q++) {
            if (*q < '0' || *q > '9') return false;  // 非数字端口,拒
        }
        name_len = (size_t)(colon - rest);
        if (name_len == 0) return false;
        have_port = 1;
    }
    if (name_len + (have_port ? (size_t)6 : 0) + 1 > RADIO_HOST_MAX) return false;

    memcpy(out, rest, name_len);
    out[name_len] = '\0';
    if (have_port) {
        out[name_len] = ':';                 // 分隔符,不是结尾
        const char *p = colon + 1;
        size_t n = (size_t)(rest + host_len - p);
        memcpy(out + name_len + 1, p, n);
        out[name_len + 1 + n] = '\0';
    }
    return true;
}

int radio_list_find(const radio_list_t *l, const char *name)
{
    if (!l || !name) return -1;
    for (uint8_t i = 0; i < l->count && i < RADIO_MAX_STATIONS; i++) {
        if (strcmp(l->items[i].name, name) == 0) return (int)i;
    }
    return -1;
}

bool radio_list_add(radio_list_t *l, const char *name, const char *url)
{
    if (!l || !name || !url) return false;
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= RADIO_NAME_MAX) return false;
    if (!radio_url_valid(url)) return false;

    const int existing = radio_list_find(l, name);
    if (existing >= 0) {                       // 同名=改 URL,不新增行
        memcpy(l->items[existing].url, url, strlen(url) + 1);
        return true;
    }
    if (l->count >= RADIO_MAX_STATIONS) return false;

    memcpy(l->items[l->count].name, name, nlen + 1);
    memcpy(l->items[l->count].url, url, strlen(url) + 1);
    l->count++;
    return true;
}

bool radio_list_remove(radio_list_t *l, uint8_t index)
{
    if (!l || index >= l->count) return false;
    for (uint8_t i = index; i + 1 < l->count; i++) {
        l->items[i] = l->items[i + 1];
    }
    l->count--;
    memset(&l->items[l->count], 0, sizeof(l->items[0]));
    return true;
}
