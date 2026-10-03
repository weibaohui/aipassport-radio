// tests/test_radio_m3u.c —— M3U 解析/序列化的纯逻辑主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "radio_m3u.h"

// 通用收集器:通过筛选的条目存进本地数组,统计计数。
struct coll { struct radio_m3u_entry items[16]; size_t n; };
static void collect_cb(void *user, const char *name, const char *url) {
    struct coll *c = (struct coll *)user;
    if (c->n < 16) {
        snprintf(c->items[c->n].name, RADIO_NAME_MAX, "%s", name);
        snprintf(c->items[c->n].url, RADIO_URL_MAX, "%s", url);
        c->n++;
    }
}

static bool accept_no_https(const char *name, const char *url, void *user) {
    (void)name; (void)user;
    // 与播放器能力对齐:只收 http:// 直链,HLS(.m3u8)不支持。
    return strncmp(url, "http://", 7) == 0 && strstr(url, ".m3u8") == NULL;
}

static void test_parse_basic(void) {
    const char *m3u =
        "#EXTM3U\n"
        "#EXTINF:-1,中国之声\r\n"
        "http://a.example/1.mp3\n"
        "#EXTINF:-1 tvg-id=\"x\",CityFM 城市音乐台\n"
        "http://b.example/2.mp3\n"
        "# 注释行\n"
        "http://orphan.example/noinf.mp3\n";   // 无 EXTINF 配对:忽略
    struct coll c = { 0 };
    radio_m3u_stats_t st;
    const size_t n = radio_m3u_parse(m3u, NULL, NULL, &c, collect_cb, &st);
    assert(n == 2 && c.n == 2);
    assert(st.extinf == 2 && st.accepted == 2 && st.rejected == 0);
    assert(strcmp(c.items[0].name, "中国之声") == 0);
    assert(strcmp(c.items[0].url, "http://a.example/1.mp3") == 0);
    assert(strcmp(c.items[1].name, "CityFM 城市音乐台") == 0);
}

static void test_parse_filter_and_counts(void) {
    const char *m3u =
        "#EXTM3U\n"
        "#EXTINF:-1,https 台\n"
        "https://secure.example/s.mp3\n"
        "#EXTINF:-1,hls 台\n"
        "http://cdn.example/live.m3u8\n"
        "#EXTINF:-1,直链台\n"
        "http://ok.example/live.mp3\n"
        "#EXTINF:-1,\n"
        "http://ok.example/noname.mp3\n";   // 空标题:拒绝
    struct coll c = { 0 };
    radio_m3u_stats_t st;
    const size_t n = radio_m3u_parse(m3u, NULL, accept_no_https, &c, collect_cb, &st);
    assert(n == 1 && c.n == 1);
    assert(st.extinf == 4 && st.accepted == 1 && st.rejected == 3);
    assert(strcmp(c.items[0].name, "直链台") == 0);
}

// 容量语义在回调侧:合并函数只收前 2 条(模拟列表满),第 3 条被拒。
static bool accept_cap2(const char *name, const char *url, void *user) {
    (void)name; (void)url;
    int *accepted = (int *)user;
    if (*accepted >= 2) return false;
    (*accepted)++;
    return true;
}

static void test_parse_capacity_and_truncation(void) {
    const char *m3u =
        "#EXTM3U\n"
        "#EXTINF:-1,t1\nhttp://a/1\n"
        "#EXTINF:-1,t2\nhttp://a/2\n"
        "#EXTINF:-1,超长标题超长标题超长标题超长标题超长标题超长标题超长标题\n"
        "http://a/3\n";
    struct coll c = { 0 };
    int delivered = 0;
    radio_m3u_stats_t st = { 0 };
    const size_t n = radio_m3u_parse(m3u, &delivered, accept_cap2, &c, collect_cb, &st);
    printf("cap: n=%zu delivered=%d acc=%d rej=%d\n", n, delivered, st.accepted, st.rejected);
    assert(n == 2);
    assert(delivered == 2);
    assert(st.accepted == 2 && st.rejected == 1);
    assert(strlen(c.items[1].url) < RADIO_URL_MAX);
}

static void test_parse_crlf_and_quoted_comma(void) {
    const char *m3u =
        "#EXTM3U\r\n"
        "#EXTINF:-1,台名, 带逗号\r\n"
        "http://a/x.mp3\r\n";
    struct coll c = { 0 };
    const size_t n = radio_m3u_parse(m3u, NULL, NULL, &c, collect_cb, NULL);
    assert(n == 1);
    // 标题取第一个逗号之后:含逗号的标题整段保留。
    assert(strcmp(c.items[0].name, "台名, 带逗号") == 0);
}

static void test_serialize_roundtrip(void) {
    struct radio_m3u_entry in[2] = { 0 };
    strcpy(in[0].name, "中国之声");
    strcpy(in[0].url, "http://a/1.mp3");
    strcpy(in[1].name, "KEXP");
    strcpy(in[1].url, "http://kexp/128.mp3");

    char buf[1024];
    const size_t len = radio_m3u_serialize(in, 2, buf, sizeof(buf));
    assert(len > 0);
    assert(strncmp(buf, "#EXTM3U\n", 8) == 0);
    assert(strstr(buf, "#EXTINF:-1,中国之声\nhttp://a/1.mp3\n") != NULL);

    // 往返:序列化产物再解析,得到同一条目集。
    struct coll back = { 0 };
    const size_t n = radio_m3u_parse(buf, NULL, NULL, &back, collect_cb, NULL);
    assert(n == 2);
    assert(strcmp(back.items[0].name, "中国之声") == 0 && strcmp(back.items[0].url, in[0].url) == 0);
    assert(strcmp(back.items[1].name, "KEXP") == 0);

    // 缓冲不足:返回 0,不写半截
    char tiny[8];
    assert(radio_m3u_serialize(in, 2, tiny, sizeof(tiny)) == 0);
}

int main(void) {
    test_parse_basic();
    test_parse_filter_and_counts();
    test_parse_capacity_and_truncation();
    test_parse_crlf_and_quoted_comma();
    test_serialize_roundtrip();
    printf("radio_m3u: 全部通过\n");
    return 0;
}
