// main/radio_hls.h —— HLS(m3u8)播放列表解析(纯逻辑,零 ESP 依赖,主机可测)。
//
// 只做"网络收音机"够用的那一版 HLS:直播场景从最新的媒体段切入,轮播刷新追新;
// 点播(ENDLIST)从第一段顺播。不支持:多级变体流(取到的还是 m3u8 时按
// 第一条嵌套列表处理)、加密段(EXT-X-KEY 非 NONE 直接不收)、离散广告段。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLS_URL_MAX 256   // 段地址上限(绝对 URL;相对地址在 join 时拼装)

typedef struct {
    // 输入:播放列表文本(原地在 buf 里,解析会做行内改写)
    // 输出:最新(或最早,见 take_oldest)一个媒体段的地址与节奏信息
    char seg_url[HLS_URL_MAX];   // 解析出的段地址(已拼成绝对地址,需 base_url)
    uint32_t target_dur;         // EXT-X-TARGETDURATION(秒);缺省 0
    uint64_t media_seq;          // 本段的全局序号(MEDIA-SEQUENCE + 行内偏移;无头则 0)
    bool endlist;                // EXT-X-ENDLIST(点播)
    bool is_playlist;            // 段地址本身还是 .m3u8(变体流未展开)
} hls_pick_t;

// 从播放列表文本里挑一个媒体段。
//   base_url: 播放列表的 URL(相对段地址用它拼绝对地址)
//   want_oldest: true=第一段(点播顺播);false=最后一段(直播追边上)
//   last_seq: 直播追新时的"已播序号";want_oldest=false 且没有任何比它新的段
//             时返回 false(还没有新段,调用方等一轮再刷)
// 返回 false:没有可播的媒体段(空列表/全是加密段/参数空)。
bool hls_pick_segment(const char *text, const char *base_url,
                      bool want_oldest, uint64_t last_seq, hls_pick_t *out);

// 相对地址拼接:ref 相对于 base(去掉最后一段路径)。out 容量 HLS_URL_MAX。
bool hls_join_url(const char *base, const char *ref, char *out, size_t cap);

// URL 是否指向播放列表(按扩展名,内容探测在播放器侧)。
bool hls_is_playlist_url(const char *url);
