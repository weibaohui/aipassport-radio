// main/radio_streams.c —— 见 radio_streams.h。本文件不依赖 ESP-IDF/LVGL。
#include "radio_streams.h"

#include <string.h>

// 内置台。URL 与 ICY 参数均为 2026-10 从设备所在网络实测:
//   KEXP 90.3 FM      http://kexp.streamguys1.com/kexp128.mp3     icy-metaint 4096
//   Radio Paradise    http://stream.radioparadise.com/mp3-128      icy-metaint 16000
// 两者均为 128kbps / 44.1kHz / 立体声 MP3。同一网络下大量其他公开电台
// (SomaFM 全部 Icecast 节点、若干欧美 NPR 台站)不可达,因此内置台以实测
// 为准,不照抄公开列表。
static const radio_station_t BUILTIN[] = {
    { "KEXP 90.3 FM",   "http://kexp.streamguys1.com/kexp128.mp3" },
    { "Radio Paradise", "http://stream.radioparadise.com/mp3-128"  },
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
