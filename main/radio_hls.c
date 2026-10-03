// main/radio_hls.c —— 见 radio_hls.h。本文件不依赖 ESP-IDF/LVGL。
#include "radio_hls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hls_is_playlist_url(const char *url)
{
    if (!url) return false;
    const char *q = strchr(url, '?');
    const size_t len = q ? (size_t)(q - url) : strlen(url);
    return len >= 5 && strncmp(url + len - 5, ".m3u8", 5) == 0;
}

// scheme://host 与其后的路径分界:返回 base 里最后一个 '/' 的位置。
static const char *last_slash(const char *url)
{
    const char *s = strstr(url, "://");
    if (!s) return NULL;
    const char *slash = NULL;
    for (const char *p = s + 3; *p; p++) {
        if (*p == '/') slash = p;
        else if (*p == '?') break;
    }
    return slash;
}

bool hls_join_url(const char *base, const char *ref, char *out, size_t cap)
{
    if (!base || !ref || !out || cap == 0) return false;
    if (strstr(ref, "://")) {                       // 已是绝对地址
        if (strlen(ref) >= cap) return false;
        strcpy(out, ref);
        return true;
    }
    if (ref[0] == '/') {                            // 根相对:scheme://host + ref
        const char *s = strstr(base, "://");
        if (!s) return false;
        const char *path = strchr(s + 3, '/');
        const size_t host_len = path ? (size_t)(path - base) : strlen(base);
        if (host_len + strlen(ref) + 1 > cap) return false;
        memcpy(out, base, host_len);
        strcpy(out + host_len, ref);
        return true;
    }
    const char *slash = last_slash(base);
    if (!slash) return false;
    const size_t dir_len = (size_t)(slash - base) + 1;   // 含最后一个 '/'
    if (dir_len + strlen(ref) + 1 > cap) return false;
    memcpy(out, base, dir_len);
    strcpy(out + dir_len, ref);
    return true;
}

// 取该行的值部分("TAG:value";容错冒号后空格)。
static const char *tag_value(const char *line, const char *tag)
{
    const size_t tlen = strlen(tag);
    if (strncmp(line, tag, tlen) != 0) return NULL;
    const char *v = line + tlen;
    while (*v == ' ') v++;
    return v;
}

bool hls_pick_segment(const char *text, const char *base_url,
                      bool want_oldest, uint64_t last_seq, hls_pick_t *out)
{
    if (!text || !base_url || !out) return false;
    memset(out, 0, sizeof(*out));

    uint64_t seq_base = 0;         // EXT-X-MEDIA-SEQUENCE(缺省 0)
    uint64_t first_seq = 0, last_seq_in_file = 0;
    bool have_first = false;
    char first_url[HLS_URL_MAX] = "", last_url[HLS_URL_MAX] = "";
    bool first_is_pl = false, last_is_pl = false;

    char line[512];
    const char *p = text;
    uint64_t idx = 0;              // 当前段在文件内的偏移序号

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && (p[len-1] == '\r' || p[len-1] == ' ' || p[len-1] == '\t')) len--;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : p + len;

        const char *v;
        if ((v = tag_value(line, "#EXT-X-MEDIA-SEQUENCE:")) != NULL) {
            seq_base = strtoull(v, NULL, 10);
        } else if ((v = tag_value(line, "#EXT-X-TARGETDURATION:")) != NULL) {
            out->target_dur = (uint32_t)strtoul(v, NULL, 10);
        } else if (tag_value(line, "#EXT-X-ENDLIST") != NULL) {
            out->endlist = true;
        } else if (strncmp(line, "#EXTINF", 7) == 0 ||
                   strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
            // 标签行之后最近的非 # 非空行就是地址(媒体段或变体列表)
            const char *q = p;
            while (*q) {
                const char *e2 = strchr(q, '\n');
                size_t l2 = e2 ? (size_t)(e2 - q) : strlen(q);
                while (l2 && (q[l2-1] == '\r' || q[l2-1] == ' ')) l2--;
                if (l2 && *q != '#') {
                    if (l2 >= HLS_URL_MAX) l2 = HLS_URL_MAX - 1;
                    char raw[HLS_URL_MAX];
                    memcpy(raw, q, l2);
                    raw[l2] = '\0';
                    char abs[HLS_URL_MAX];
                    const char *use = hls_join_url(base_url, raw, abs, sizeof(abs)) ? abs : raw;
                    const uint64_t seq = seq_base + idx;
                    if (!have_first) {
                        have_first = true;
                        first_seq = seq;
                        snprintf(first_url, sizeof(first_url), "%s", use);
                        first_is_pl = hls_is_playlist_url(use);
                    }
                    last_seq_in_file = seq;
                    snprintf(last_url, sizeof(last_url), "%s", use);
                    last_is_pl = hls_is_playlist_url(use);
                    idx++;
                    // 跳过已消费的地址行
                    p = e2 ? e2 + 1 : q + l2;
                    break;
                }
                q = e2 ? e2 + 1 : q + l2;
                if (!*q) break;
            }
        }
    }

    if (!have_first) return false;
    out->media_seq = want_oldest ? first_seq : last_seq_in_file;

    // 变体流(段地址还是 m3u8):返回它本身,调用方按播放列表递归处理(限深在调用方)。
    const char *url = want_oldest ? first_url : last_url;
    const bool is_pl = want_oldest ? first_is_pl : last_is_pl;
    // last_seq==UINT64_MAX 是"首切"哨兵:无条件取最新段(直播进场不听回放)。
    if (!want_oldest && !out->endlist &&
        last_seq != UINT64_MAX && last_seq_in_file <= last_seq) {
        return false;              // 直播追新:还没有比已播更新的段
    }
    if (strlen(url) >= sizeof(out->seg_url)) return false;
    strcpy(out->seg_url, url);
    out->is_playlist = is_pl;
    return true;
}
