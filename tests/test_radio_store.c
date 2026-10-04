// tests/test_radio_store.c —— 电台清单 flash 存储的主机测试。
//
// radio_store 是逐条 NVS 的清单存储,主机测试用内存版键值表(appfw_storage.h
// 桩)钉死这些行为:出厂物化、旧版 M3U 迁移、同名改址、拒删内置台、删除后
// 前移、整表导入、恢复出厂、以及"重启"(重复 init)不重置用户清单。
#include "radio_store.h"
#include "radio_biglist.h"
#include "appfw_storage.h"
#include "appfw_m3u.h"
#include "radio_m3u_default.h"
#include "radio_player.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// radio_store 委托的 radio_biglist 在主机桩上恒"未挂载借洞",播放器预留
// 接口给空实现(主机测试不连 radio_player.c)。
void radio_player_release_reserve(void) {}
void radio_player_reacquire_reserve(void) {}
void radio_player_snapshot(radio_player_snap_t *s)
{
    s->station[0] = '\0';
    s->title[0] = '\0';
    s->state = RADIO_STOPPED;
    s->err_code = RADIO_ERR_NONE;
    s->sample_rate = 0;
    s->channels = 0;
    s->bitrate = 0;
    s->volume = 50;
}

// ---------------------------------------------------------------- 内存 NVS

#define FSLOTS 64
static struct {
    bool used;
    char key[16];
    char val[600];
} S[FSLOTS];

void fake_store_reset(void) { memset(S, 0, sizeof(S)); }

bool fake_store_has(const char *key)
{
    char buf[8];
    return appfw_store_get_str(key, buf, sizeof(buf)) && buf[0];
}

bool appfw_store_get_str(const char *key, char *buf, size_t buf_len)
{
    if (!key || !buf || buf_len == 0) return false;
    for (int i = 0; i < FSLOTS; i++) {
        if (S[i].used && strcmp(S[i].key, key) == 0) {
            snprintf(buf, buf_len, "%s", S[i].val);
            return true;
        }
    }
    buf[0] = '\0';
    return false;
}

bool appfw_store_set_str(const char *key, const char *value)
{
    if (!key || !value) return false;
    int slot = -1;
    for (int i = 0; i < FSLOTS; i++) {
        if (S[i].used && strcmp(S[i].key, key) == 0) { slot = i; break; }
        if (slot < 0 && !S[i].used) slot = i;
    }
    if (slot < 0) return false;
    S[slot].used = true;
    snprintf(S[slot].key, sizeof(S[slot].key), "%s", key);
    snprintf(S[slot].val, sizeof(S[slot].val), "%s", value);
    return true;
}

bool appfw_store_get_u16(const char *key, uint16_t *out, uint16_t fallback)
{
    if (!out) return false;
    *out = fallback;
    char tmp[16];
    if (!appfw_store_get_str(key, tmp, sizeof(tmp)) || tmp[0] == '\0') return false;
    const long v = strtol(tmp, NULL, 10);
    if (v <= 0 || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

bool appfw_store_set_u16(const char *key, uint16_t value)
{
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)value);
    return appfw_store_set_str(key, tmp);
}

// ---------------------------------------------------------------- 用例

// 出厂清单(固件内嵌)在主机上同样可解析:先按 store 同款筛选(http/https
// 直链、剔除 HLS)解析一遍拿到期望条目,避免硬编码台数——清单内容更新时
// 测试不必跟着改。
static bool accept_like_store(const char *name, const char *url, void *user)
{
    (void)name; (void)user;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
           && strstr(url, ".m3u8") == NULL;
}

static radio_station_t g_factory[64];
static int g_factory_n;
static void collect_factory(void *user, const char *name, const char *url)
{
    (void)user;
    assert(g_factory_n < 64);
    snprintf(g_factory[g_factory_n].name, RADIO_NAME_MAX, "%s", name);
    snprintf(g_factory[g_factory_n].url, RADIO_URL_MAX, "%s", url);
    g_factory_n++;
}

int main(void)
{
    // 关掉内置台目兜底:本文件测的是 NVS 小清单路径(台目开着时 store
    // 永远委托台目,这些路径摸不到)。
    radio_biglist_set_catalog_enabled(false);

    // 解析出厂清单作为期望值(与 store 同款筛选),再按 store 同款合并语义
    // (同名覆盖、首现位置)折叠——出厂清单里有重名台,合并后台数 < 解析条数。
    appfw_m3u_stats_t st = { 0 };
    appfw_m3u_parse(RADIO_M3U_DEFAULT, NULL, accept_like_store,
                    g_factory, collect_factory, &st);
    static radio_station_t g_expect[80];
    int expect_n = 0;
    for (int i = 0; i < g_factory_n; i++) {
        int at = -1;
        for (int j = 0; j < expect_n; j++) {
            if (strcmp(g_expect[j].name, g_factory[i].name) == 0) { at = j; break; }
        }
        if (at < 0) {
            assert(expect_n < 80);
            g_expect[at = expect_n++] = g_factory[i];
        } else {
            snprintf(g_expect[at].url, RADIO_URL_MAX, "%s", g_factory[i].url);
        }
    }
    const int expect_total = 6 + expect_n;

    // ---- 1. 空机 init:物化 内置6 + 出厂42 ----
    fake_store_reset();
    radio_store_init();
    assert(radio_store_count() == expect_total);
    radio_station_t b0, e;
    assert(radio_builtin_get(0, &b0));
    assert(radio_store_get(0, &e));
    assert(strcmp(e.name, b0.name) == 0 && strcmp(e.url, b0.url) == 0);
    assert(radio_store_get(6, &e));
    assert(strcmp(e.name, g_expect[0].name) == 0);
    assert(strcmp(e.url, g_expect[0].url) == 0);
    assert(!radio_store_get(-1, &e) && !radio_store_get(expect_total, &e));

    // ---- 2. 查找 / 内置台判定 ----
    assert(radio_store_find(b0.name) == 0);
    assert(radio_store_find("不存在的台") == -1);
    assert(radio_store_is_builtin(b0.name, b0.url));
    assert(!radio_store_is_builtin(b0.name, "http://changed/"));

    // ---- 3. 增改:追加、同名改址、非法拒绝 ----
    assert(radio_store_add("测试台A", "https://a.example.com/live.mp3"));
    assert(radio_store_count() == expect_total + 1);
    const int idx_a = radio_store_find("测试台A");
    assert(idx_a == expect_total);
    assert(radio_store_add("测试台A", "http://a2.example.com/live.mp3"));   // 同名改址
    assert(radio_store_count() == expect_total + 1);
    assert(radio_store_get(idx_a, &e) && strcmp(e.url, "http://a2.example.com/live.mp3") == 0);
    assert(!radio_store_add("", "http://x.com/"));           // 空名
    assert(!radio_store_add("坏地址", "ftp://x.com/"));      // 非 http(s)
    assert(!radio_store_add("坏地址", "http://x.com/a m3u8")); // 空格
    assert(!radio_store_add("名\t带制表", "http://x.com/"));  // 记录分隔符
    assert(radio_store_count() == expect_total + 1);

    // ---- 4. 删除:内置拒删,自加可删,后条前移 ----
    assert(!radio_store_remove(0));                          // 内置台
    assert(radio_store_remove(idx_a));                       // 尾部自加台
    assert(radio_store_count() == expect_total);
    assert(radio_store_find("测试台A") == -1);

    // 删中间项:序号整体前移(顺序即门户/屏幕序号)
    assert(radio_store_add("中台1", "http://m1.com/"));
    assert(radio_store_add("中台2", "http://m2.com/"));
    const int i1 = radio_store_find("中台1");
    assert(radio_store_remove(i1));
    assert(radio_store_find("中台1") == -1);
    assert(radio_store_find("中台2") == i1);                 // 后条补位
    assert(radio_store_remove(radio_store_find("中台2")));
    assert(radio_store_count() == expect_total);

    // ---- 5. 容量封顶 ----
    for (int i = radio_store_count(); i < 48; i++) {
        char name[16], url[48];
        snprintf(name, sizeof(name), "灌%d", i);
        snprintf(url, sizeof(url), "http://fill.%d.com/", i);
        assert(radio_store_add(name, url));
    }
    assert(radio_store_count() == 48);
    assert(!radio_store_add("超出", "http://over.com/"));     // 满
    assert(radio_store_remove(radio_store_find("灌47")));     // 腾一格
    assert(radio_store_add("再进", "http://again.com/"));
    assert(radio_store_count() == 48);

    // ---- 6. "重启":再次 init 走已落盘路径,用户清单不丢 ----
    radio_store_init();
    assert(radio_store_count() == 48);
    assert(radio_store_find("再进") >= 0);
    assert(radio_store_get(0, &e) && strcmp(e.name, b0.name) == 0);

    // ---- 7. 整表导入:清空重装 + 内置兜底 ----
    radio_store_import_begin();
    assert(radio_store_count() == 0);
    assert(radio_store_add("导入1", "http://i1.com/"));
    assert(radio_store_add("导入2", "https://i2.com/"));
    assert(radio_store_count() == 2);
    assert(radio_store_get(0, &e) && strcmp(e.name, "导入1") == 0);
    assert(radio_store_get(1, &e) && strcmp(e.name, "导入2") == 0);
    radio_store_init();                                      // 重启不重置导入结果
    assert(radio_store_count() == 2);

    // ---- 8. 恢复出厂:回到 内置6+出厂42 ----
    assert(radio_store_restore_factory() == expect_total);
    assert(radio_store_get(6, &e) && strcmp(e.name, g_expect[0].name) == 0);

    // ---- 9. 旧版 M3U 迁移:整份文本 → 逐条;迁移后旧键清空 ----
    fake_store_reset();
    assert(appfw_store_set_str("radio_m3u",
        "#EXTM3U\n"
        "#EXTINF:-1,老台一\nhttp://old.example.com/a.mp3\n"
        "#EXTINF:-1,老台二\nhttps://old.example.com/b.mp3\n"
        "#EXTINF:-1,坏台\nftp://old.example.com/c.mp3\n"));
    radio_store_init();
    // 内置6在前,旧清单有效条目随后(坏协议被筛掉)
    assert(radio_store_count() == 6 + 2);
    assert(radio_store_get(6, &e) && strcmp(e.name, "老台一") == 0);
    assert(radio_store_get(7, &e) && strcmp(e.url, "https://old.example.com/b.mp3") == 0);
    assert(!fake_store_has("radio_m3u"));                    // 迁移完释放旧键
    radio_store_init();                                      // 再次启动:不重复迁移
    assert(radio_store_count() == 6 + 2);

    // ---- 10. 旧清单同名条目顶掉同名的内置/出厂台(用户改动优先) ----
    fake_store_reset();
    radio_station_t f0 = g_expect[0];
    char legacy[512];
    snprintf(legacy, sizeof(legacy),
             "#EXTINF:-1,%s\nhttp://user-modified.example.com/\n", f0.name);
    assert(appfw_store_set_str("radio_m3u", legacy));
    radio_store_init();
    // 迁移基底 = 内置6;旧清单条目与出厂台同名 → 以旧清单地址为准,落在内置台后
    assert(radio_store_count() == 6 + 1);
    const int fi = radio_store_find(f0.name);
    assert(fi == 6);
    assert(radio_store_get(fi, &e) &&
           strcmp(e.url, "http://user-modified.example.com/") == 0);
    assert(!radio_store_is_builtin(f0.name, e.url));         // 改过址就不再是内置台

    printf("test_radio_store: PASS (%d 用例组, 出厂 %d 台)\n", 10, g_factory_n);
    return 0;
}
