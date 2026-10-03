// main/radio_m3u.c —— 见 radio_m3u.h。
#include "radio_m3u.h"

#include <stdio.h>
#include <string.h>

static void copy_trunc(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) len = cap - 1;
    // UTF-8 安全:截断点不能落在多字节字符中间(否则 JSON/显示全是坏字节)。
    while (len && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

size_t radio_m3u_parse(const char *text,
                       void *accept_user, radio_m3u_accept_fn accept,
                       void *entry_user, radio_m3u_entry_fn on_entry,
                       radio_m3u_stats_t *stats)
{
    if (stats) { stats->extinf = 0; stats->accepted = 0; stats->rejected = 0; }
    if (!text) return 0;

    size_t delivered = 0;
    char name[RADIO_NAME_MAX] = "";
    bool pending = false;   // 已见 EXTINF,等配对的 URL 行
    const char *p = text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        const char *line = p;
        if (len && (*line == ' ' || *line == '\t')) { line++; len--; }

        if (len && *line == '#') {
            if (len >= 8 && strncmp(line, "#EXTINF:", 8) == 0) {
                // 标题 = 第一个逗号之后的部分(时长/属性在前;标题内允许逗号)。
                const char *comma = memchr(line, ',', len);
                if (comma) {
                    copy_trunc(name, sizeof(name), comma + 1,
                               len - (size_t)(comma + 1 - line));
                    pending = true;
                    if (stats) stats->extinf++;
                }
            }
            // 其他 # 行(EXTM3U/EXTGRP/注释)一律忽略。
        } else if (len) {
            // URL 行:必须挂着 EXTINF 标题才算一个台(裸 URL 不收,防误入)。
            if (pending) {
                pending = false;
                char urlbuf[RADIO_URL_MAX];
                copy_trunc(urlbuf, sizeof(urlbuf), line, len);   // 行非 NUL 结尾,先落缓冲
                const bool ok = (name[0] != '\0') &&
                                (!accept || accept(name, urlbuf, accept_user));
                if (stats) {
                    if (ok) stats->accepted++;
                    else stats->rejected++;   // 筛掉/空标题都算未收
                }
                if (ok && on_entry) {
                    on_entry(entry_user, name, urlbuf);
                    delivered++;
                }
            }
            // 无标题的裸 URL:忽略(格式不对,不猜)。
        }
        p = eol ? eol + 1 : p + len;
    }
    return delivered;
}

size_t radio_m3u_serialize(const struct radio_m3u_entry *entries, uint8_t count,
                           char *out, size_t cap)
{
    size_t used = 0;
    const char *hdr = "#EXTM3U\n";
    const size_t hdr_len = strlen(hdr);
    if (hdr_len + 1 > cap) return 0;
    memcpy(out, hdr, hdr_len + 1);
    used = hdr_len;

    for (uint8_t i = 0; i < count; i++) {
        char line[RADIO_NAME_MAX + RADIO_URL_MAX + 16];
        const int n = snprintf(line, sizeof(line), "#EXTINF:-1,%s\n%s\n",
                               entries[i].name, entries[i].url);
        if (n < 0 || (size_t)n + used + 1 > cap) return 0;   // 缓冲不足:整体放弃
        memcpy(out + used, line, (size_t)n);
        used += (size_t)n;
    }
    out[used] = '\0';
    return used;
}
