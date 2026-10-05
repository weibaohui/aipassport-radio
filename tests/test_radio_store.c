// tests/test_radio_store.c —— 电台清单存储的主机测试。
//
// radio_store = 固件台目(RADIO_CATALOG rodata,只读) + 自定义台(逐条 NVS)。
// 主机测试用内存版键值表(appfw_storage.h 桩)钉死这些行为:台目恒在、
// 自定义增改删、同名改址、台目段拒删、删除后前移、容量封顶、整表导入、
// 恢复出厂、"重启"不丢、用户改址同名覆盖台目、旧版 r* 键一次性清空。
#include "radio_store.h"
#include "radio_catalog.h"
#include "appfw_storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 内存 NVS

#define FSLOTS 128
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

int main(void)
{
    const int base = radio_builtin_count() + RADIO_CATALOG_N;
    assert(base > 300);                       // 台目在,固件没编错
    radio_station_t e, b0;
    assert(radio_builtin_get(0, &b0));

    // ---- 1. 空机 init:台目恒在,自定义为空 ----
    fake_store_reset();
    radio_store_init();
    assert(radio_store_catalog_count() == base);
    assert(radio_store_count() == base);
    assert(radio_store_get(0, &e));
    assert(strcmp(e.name, b0.name) == 0 && strcmp(e.url, b0.url) == 0);
    assert(radio_store_get(radio_builtin_count(), &e));
    assert(strcmp(e.name, RADIO_CATALOG[0].name) == 0);
    assert(!radio_store_get(-1, &e) && !radio_store_get(base, &e));
    printf("ok  空机:台目 %d 台\n", base);

    // ---- 2. 台目查找 ----
    assert(radio_store_find(b0.name) == 0);
    assert(radio_store_find(RADIO_CATALOG[7].name) == radio_builtin_count() + 7);
    assert(radio_store_find("不存在的台") == -1);

    // ---- 3. 增改:追加、同名改址、非法拒绝 ----
    assert(radio_store_add("测试台A", "https://a.example.com/live.mp3"));
    assert(radio_store_count() == base + 1);
    const int idx_a = radio_store_find("测试台A");
    assert(idx_a == base);
    assert(radio_store_add("测试台A", "http://a2.example.com/live.mp3"));   // 同名改址
    assert(radio_store_count() == base + 1);
    assert(radio_store_get(idx_a, &e) &&
           strcmp(e.url, "http://a2.example.com/live.mp3") == 0);
    assert(!radio_store_add("", "http://x.com/"));            // 空名
    assert(!radio_store_add("坏地址", "ftp://x.com/"));       // 非 http(s)
    assert(!radio_store_add("坏地址", "http://x.com/a m3u8")); // 空白
    assert(!radio_store_add("名\t带制表", "http://x.com/"));  // 记录分隔符
    assert(radio_store_count() == base + 1);
    printf("ok  增改与拒绝\n");

    // ---- 4. 删除:台目段拒删,自定义可删,后条前移 ----
    assert(!radio_store_remove(0));                           // 台目
    assert(!radio_store_remove(base - 1));                    // 台目末位
    assert(radio_store_remove(idx_a));                        // 自定义台
    assert(radio_store_count() == base);
    assert(radio_store_find("测试台A") == -1);

    assert(radio_store_add("中台1", "http://m1.com/"));
    assert(radio_store_add("中台2", "http://m2.com/"));
    const int i1 = radio_store_find("中台1");
    assert(radio_store_remove(i1));
    assert(radio_store_find("中台1") == -1);
    assert(radio_store_find("中台2") == i1);                  // 后条补位
    assert(radio_store_remove(radio_store_find("中台2")));
    assert(radio_store_count() == base);
    printf("ok  删除与前移\n");

    // ---- 5. 容量封顶(自定义上限) ----
    for (int i = (int)0; i < RADIO_MAX_STATIONS; i++) {
        char name[16], url[48];
        snprintf(name, sizeof(name), "灌%d", i);
        snprintf(url, sizeof(url), "http://fill.%d.com/", i);
        assert(radio_store_add(name, url));
    }
    assert(radio_store_count() == base + RADIO_MAX_STATIONS);
    assert(!radio_store_add("超出", "http://over.com/"));     // 满
    assert(radio_store_remove(radio_store_find("灌99")));     // 腾一格
    assert(radio_store_add("再进", "http://again.com/"));
    assert(radio_store_count() == base + RADIO_MAX_STATIONS);
    printf("ok  容量封顶 %d\n", RADIO_MAX_STATIONS);

    // ---- 6. "重启":再次 init 走已落盘路径,自定义清单不丢 ----
    radio_store_init();
    assert(radio_store_count() == base + RADIO_MAX_STATIONS);
    assert(radio_store_find("再进") >= 0);
    assert(radio_store_get(0, &e) && strcmp(e.name, b0.name) == 0);

    // ---- 7. 整表导入:清空自定义重装,台目不受影响 ----
    radio_store_import_begin();
    assert(radio_store_count() == base);
    assert(radio_store_add("导入1", "http://i1.com/"));
    assert(radio_store_add("导入2", "https://i2.com/"));
    assert(radio_store_count() == base + 2);
    assert(radio_store_get(base, &e) && strcmp(e.name, "导入1") == 0);
    assert(radio_store_get(base + 1, &e) && strcmp(e.name, "导入2") == 0);
    radio_store_init();                                       // 重启不重置导入结果
    assert(radio_store_count() == base + 2);
    printf("ok  整表导入\n");

    // ---- 8. 恢复出厂:清空自定义,回到台目 ----
    assert(radio_store_restore_factory() == base);
    assert(radio_store_count() == base);

    // ---- 9. 用户改址同名覆盖台目:自定义段在列表尾部,播放查找优先用户版 ----
    assert(radio_store_add(RADIO_CATALOG[3].name, "http://user-mirror.example/"));
    const int shadow = radio_store_find(RADIO_CATALOG[3].name);
    assert(shadow == base);                                   // 用户版在前
    assert(radio_store_get(shadow, &e) &&
           strcmp(e.url, "http://user-mirror.example/") == 0);
    assert(radio_store_get(radio_builtin_count() + 3, &e));   // 台目本体还在
    assert(strcmp(e.url, RADIO_CATALOG[3].url) == 0);
    radio_store_restore_factory();

    // ---- 10. 旧版 r* 键一次性清空(内容全是台目副本,腾 NVS) ----
    fake_store_reset();
    assert(appfw_store_set_u16("r_cnt", 2));
    assert(appfw_store_set_str("r0", "旧台一\thttp://old.example.com/a.mp3"));
    assert(appfw_store_set_str("r1", "旧台二\thttps://old.example.com/b.mp3"));
    radio_store_init();
    assert(!fake_store_has("r_cnt") && !fake_store_has("r0"));
    assert(radio_store_count() == base);                      // 旧内容不复活
    assert(radio_store_find("旧台一") == -1);
    radio_store_init();                                       // 再启动:不重复迁移
    assert(radio_store_count() == base);

    // ---- 11. 收藏:加/幂等/查/删/重启保持;按台名,台目与自定义通用 ----
    fake_store_reset();
    radio_store_init();
    assert(radio_store_fav_count() == 0);
    assert(radio_store_fav_add("测试台A"));
    assert(radio_store_fav_add("测试台A"));               // 幂等
    assert(radio_store_fav_count() == 1);
    assert(radio_store_fav_has("测试台A"));
    assert(radio_store_fav_add(RADIO_CATALOG[5].name));   // 台目也可收藏
    assert(radio_store_fav_count() == 2);
    char favname[RADIO_NAME_MAX];
    assert(radio_store_fav_get(0, favname, sizeof(favname)));
    assert(strcmp(favname, "测试台A") == 0);
    assert(radio_store_fav_remove("测试台A"));
    assert(!radio_store_fav_remove("测试台A"));           // 不存在=false
    assert(radio_store_fav_count() == 1);
    radio_store_init();                                    // 重启保持
    assert(radio_store_fav_count() == 1);
    assert(radio_store_fav_has(RADIO_CATALOG[5].name));

    printf("test_radio_store: PASS (%d 用例组, 台目 %d 台)\n", 11, base);
    return 0;
}
