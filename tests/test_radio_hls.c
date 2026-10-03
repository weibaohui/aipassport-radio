// tests/test_radio_hls.c —— HLS 播放列表解析的主机测试。
// 钉死:直播首切最新段、追新段序号、点播顺播+ENDLIST、相对地址拼接、
// 变体流识别、无新段返回 false、EXTINF 属性里带逗号。
#include "radio_hls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    hls_pick_t p;

    // ---- 1. 直播首切:取最后一段(哨兵 UINT64_MAX) ----
    const char *live =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:3\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXTINF:3.0,\n"
        "seg100.ts\n"
        "#EXTINF:3.0,\n"
        "seg101.ts\n"
        "#EXTINF:3.0,\n"
        "seg102.ts\n";
    assert(hls_pick_segment(live, "http://r.example/live/pl.m3u8", false, UINT64_MAX, &p));
    assert(strcmp(p.seg_url, "http://r.example/live/seg102.ts") == 0);
    assert(p.media_seq == 102);
    assert(p.target_dur == 3);
    assert(!p.endlist && !p.is_playlist);

    // ---- 2. 追新:已有 102,列表滚到 103 才有新段;没有则 false ----
    assert(!hls_pick_segment(live, "http://r.example/live/pl.m3u8", false, 102, &p));
    const char *live2 =
        "#EXTM3U\n#EXT-X-TARGETDURATION:3\n#EXT-X-MEDIA-SEQUENCE:101\n"
        "#EXTINF:3.0,\nseg101.ts\n#EXTINF:3.0,\nseg102.ts\n#EXTINF:3.0,\nseg103.ts\n";
    assert(hls_pick_segment(live2, "http://r.example/live/pl.m3u8", false, 102, &p));
    assert(p.media_seq == 103 && strcmp(p.seg_url, "http://r.example/live/seg103.ts") == 0);

    // ---- 3. 点播:从第一段顺播;ENDLIST 列表始终给出"最新段",播完的判定
    // (序号与已播相同=放完)在播放器 hls_advance 里做 ----
    const char *vod =
        "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-ENDLIST\n"
        "#EXTINF:9.0,title, with comma\na.ts\n#EXTINF:9.0,\nb.ts\n";
    assert(hls_pick_segment(vod, "http://r.example/vod/pl.m3u8", true, 0, &p));
    assert(strcmp(p.seg_url, "http://r.example/vod/a.ts") == 0 && p.media_seq == 0);
    assert(p.endlist);
    // 放完 a.ts(seq0)后按"最新段"挑:b.ts(seq1)
    assert(hls_pick_segment(vod, "http://r.example/vod/pl.m3u8", false, 0, &p));
    assert(strcmp(p.seg_url, "http://r.example/vod/b.ts") == 0 && p.media_seq == 1);
    // 放完 b.ts(seq1)后再挑:还是 b.ts(seq1) —— 播放器据此判定播完
    assert(hls_pick_segment(vod, "http://r.example/vod/pl.m3u8", false, 1, &p));
    assert(p.media_seq == 1);

    // ---- 4. 相对地址拼接 ----
    char out[HLS_URL_MAX];
    assert(hls_join_url("http://h.example/a/b/pl.m3u8", "s.ts", out, sizeof(out)));
    assert(strcmp(out, "http://h.example/a/b/s.ts") == 0);
    assert(hls_join_url("http://h.example/a/b/pl.m3u8", "/root/s.ts", out, sizeof(out)));
    assert(strcmp(out, "http://h.example/root/s.ts") == 0);
    assert(hls_join_url("http://h.example/a/pl.m3u8", "http://other/x.ts", out, sizeof(out)));
    assert(strcmp(out, "http://other/x.ts") == 0);
    assert(hls_join_url("http://h.example/a/pl.m3u8?token=1", "s.ts", out, sizeof(out)));
    assert(strcmp(out, "http://h.example/a/s.ts") == 0);   // query 不参与相对基准

    // ---- 5. 变体流:段地址还是 m3u8 要标记出来 ----
    const char *variant =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\nlo.m3u8\n";
    assert(hls_pick_segment(variant, "http://r.example/live/pl.m3u8", false, UINT64_MAX, &p));
    assert(p.is_playlist && strcmp(p.seg_url, "http://r.example/live/lo.m3u8") == 0);

    // ---- 6. 空列表/纯注释 ----
    assert(!hls_pick_segment("#EXTM3U\n", "http://x/pl.m3u8", false, UINT64_MAX, &p));
    assert(!hls_pick_segment("", "http://x/pl.m3u8", true, 0, &p));

    // ---- 7. 扩展名识别 ----
    assert(hls_is_playlist_url("https://a.b/live.m3u8"));
    assert(hls_is_playlist_url("https://a.b/live.m3u8?token=x"));
    assert(!hls_is_playlist_url("https://a.b/live.mp3"));
    assert(!hls_is_playlist_url("https://a.b/seg.ts"));

    printf("test_radio_hls: PASS (7 用例组)\n");
    return 0;
}
