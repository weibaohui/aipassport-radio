// main/radio_store.c —— 见 radio_store.h。
#include "radio_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appfw_storage.h"
#include "esp_log.h"

#include "appfw_biglist.h"
#include "appfw_m3u.h"
#include "radio_m3u_default.h"

static const char *TAG = "radio_store";

#define STORE_CNT_KEY    "r_cnt"       // 条数(u16)
#define STORE_LEGACY_KEY "radio_m3u"   // 旧版整份 M3U 键(迁移后释放)
#define STORE_REC_CAP    (RADIO_NAME_MAX + RADIO_URL_MAX + 2)  // "名\tURL" + NUL
#define LEGACY_BUF_CAP   (12 * 1024)   // 旧版 M3U 整份读出缓冲(仅开机迁移用)

static uint8_t s_count;                // RAM 里唯一常驻的清单状态

static void key_of(int idx, char *key, size_t cap)
{
    snprintf(key, cap, "r%d", idx);
}

// 一条记录 = "台名\tURL"。台名里的制表符/控制字符会让记录无法拆回,直接拒。
static bool entry_pack(const char *name, const char *url, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "%s\t%s", name, url);
    return n > 0 && (size_t)n < cap;
}

static bool entry_unpack(const char *rec, radio_station_t *out)
{
    const char *tab = strchr(rec, '\t');
    if (!tab) return false;
    const size_t nlen = (size_t)(tab - rec);
    if (nlen == 0 || nlen >= RADIO_NAME_MAX) return false;
    if (strlen(tab + 1) == 0 || strlen(tab + 1) >= RADIO_URL_MAX) return false;
    memcpy(out->name, rec, nlen);
    out->name[nlen] = '\0';
    memcpy(out->url, tab + 1, strlen(tab + 1) + 1);
    return true;
}

// M3U 收录策略与旧版一致:只收 http/https 直链,剔除 HLS 换壳。
static bool store_accept_http(const char *name, const char *url, void *user)
{
    (void)name; (void)user;
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
           && strstr(url, ".m3u8") == NULL;
}

static bool get_entry(int idx, radio_station_t *out)
{
    char key[8], rec[STORE_REC_CAP];
    if (idx < 0 || idx >= (int)s_count) return false;
    key_of(idx, key, sizeof(key));
    if (!appfw_store_get_str(key, rec, sizeof(rec))) return false;
    return entry_unpack(rec, out);
}

static int find_entry(const char *name)
{
    radio_station_t st;
    if (!name || !name[0]) return -1;
    for (int i = 0; i < (int)s_count; i++) {
        if (get_entry(i, &st) && strcmp(st.name, name) == 0) return i;
    }
    return -1;
}

// 追加(不查重)。迁移/导入路径用;门户 add 走 radio_store_add(带查重)。
static bool append_entry(const char *name, const char *url)
{
    char key[8], rec[STORE_REC_CAP];
    if (s_count >= RADIO_MAX_STATIONS) return false;
    if (!entry_pack(name, url, rec, sizeof(rec))) return false;
    key_of(s_count, key, sizeof(key));
    if (!appfw_store_set_str(key, rec)) return false;
    s_count++;
    return true;
}

// 迁移回调:同名覆盖(沿用旧版"整表合并"语义——用户改过地址的内置台
// 以用户地址为准),否则追加。
static void migrate_entry_cb(void *user, const char *name, const char *url)
{
    (void)user;
    char key[8], rec[STORE_REC_CAP];
    const int i = find_entry(name);
    if (i >= 0) {
        if (!entry_pack(name, url, rec, sizeof(rec))) return;
        key_of(i, key, sizeof(key));
        (void)appfw_store_set_str(key, rec);
        return;
    }
    (void)append_entry(name, url);
}

// 物化清单基底:内置 6 台在前(顺序即开机列表顺序)。
static void materialize_builtin(void)
{
    radio_station_t b;
    for (int i = 0; i < radio_builtin_count(); i++) {
        if (radio_builtin_get(i, &b)) (void)append_entry(b.name, b.url);
    }
}

void radio_store_init(void)
{
    // 大清单优先:files 分区里有 radio.m3u 就完全忽略 NVS 旧键(休眠不删,
    // 恢复出厂删文件后可退回)。RAM 里依旧只有一个条数。
    appfw_biglist_init();   // 复位节流标记(静态初值 0 会被误当"刚探过没有")
    if (appfw_biglist_poll()) {
        ESP_LOGI(TAG, "大清单模式:%d 台(NVS 清单休眠)", appfw_biglist_count());
        return;
    }

    uint16_t n = 0;
    if (appfw_store_get_u16(STORE_CNT_KEY, &n, 0xFFFF) &&
        n != 0xFFFF && n <= RADIO_MAX_STATIONS) {
        s_count = (uint8_t)n;
        ESP_LOGI(TAG, "清单已在 flash:%u 台", (unsigned)s_count);
        return;
    }

    // 首次开机(或计数损坏):物化。内置台永远在前;有旧版整份 M3U 就迁移
    // (含用户改动,同名覆盖),否则落固件内嵌的出厂清单。
    s_count = 0;
    materialize_builtin();
    appfw_m3u_stats_t st = { 0 };
    char *buf = malloc(LEGACY_BUF_CAP);
    bool migrated = false;
    if (buf && appfw_store_get_str(STORE_LEGACY_KEY, buf, LEGACY_BUF_CAP) && buf[0]) {
        appfw_m3u_parse(buf, NULL, store_accept_http, NULL, migrate_entry_cb, &st);
        migrated = st.accepted > 0;
        ESP_LOGI(TAG, "旧版 M3U 迁移:%d 台", st.accepted);
    } else {
        appfw_m3u_parse(RADIO_M3U_DEFAULT, NULL, store_accept_http,
                        NULL, migrate_entry_cb, &st);
        ESP_LOGI(TAG, "物化出厂清单:+%d 台", st.accepted);
    }
    free(buf);
    (void)appfw_store_set_u16(STORE_CNT_KEY, s_count);
    if (migrated) (void)appfw_store_set_str(STORE_LEGACY_KEY, "");   // 迁移完释放旧键
    ESP_LOGI(TAG, "清单落盘完成:%u 台", (unsigned)s_count);
}

int radio_store_count(void)
{
    // 大清单模式:内置精品台永远排最前,导入清单跟在后面(不整体覆盖)。
    const int base = radio_builtin_count();
    if (appfw_biglist_poll()) return base + appfw_biglist_count();
    // 大清单模式粘住(见 appfw_biglist_available):临时读不到(FAT 被挤、
    // 上传中)返回缓存的条数,绝不静默掉回 48 台出厂清单。
    if (appfw_biglist_available()) return base + appfw_biglist_count();
    // 大清单在运行中被删除(恢复出厂/手动)后回落小清单:开机时走大清单
    // 分支没读过 NVS 条数,这里捡一次。
    static bool picked;
    if (!picked) {
        picked = true;
        uint16_t n = 0;
        if (appfw_store_get_u16(STORE_CNT_KEY, &n, 0xFFFF) && n != 0xFFFF &&
            n <= RADIO_MAX_STATIONS && n > 0) {
            s_count = (uint8_t)n;
            ESP_LOGI(TAG, "退回小清单:%u 台", (unsigned)s_count);
        }
    }
    return (int)s_count;
}

bool radio_store_readonly(void) { return appfw_biglist_available(); }

bool radio_store_get(int idx, radio_station_t *out)
{
    if (!out) return false;
    if (appfw_biglist_available()) {
        const int base = radio_builtin_count();
        if (idx < base) return radio_builtin_get(idx, out);   // 内置在前
        radio_station_t e;
        const bool ok = appfw_biglist_get(idx - base, e.name, sizeof(e.name),
                                          e.url, sizeof(e.url));
        if (ok) *out = e;
        return ok;
    }
    return get_entry(idx, out);
}

int radio_store_find(const char *name)
{
    if (appfw_biglist_available()) {
        radio_station_t b;
        for (int i = 0; i < radio_builtin_count(); i++) {
            if (radio_builtin_get(i, &b) && strcmp(b.name, name) == 0) return i;
        }
        const int i = appfw_biglist_find(name);
        return i >= 0 ? i + radio_builtin_count() : -1;
    }
    return find_entry(name);
}

bool radio_store_add(const char *name, const char *url)
{
    if (appfw_biglist_available()) return false;   // 大清单只读,拒绝静默成功
    if (!name || !url) return false;
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= RADIO_NAME_MAX) return false;
    for (const char *p = name; *p; p++) {
        if ((unsigned char)*p < 0x20) return false;   // 含 \t\r\n,破坏记录格式
    }
    if (!radio_url_valid(url)) return false;

    char key[8], rec[STORE_REC_CAP];
    if (!entry_pack(name, url, rec, sizeof(rec))) return false;
    const int i = find_entry(name);
    if (i >= 0) {                                     // 同名 = 改地址
        key_of(i, key, sizeof(key));
        return appfw_store_set_str(key, rec);
    }
    if (s_count >= RADIO_MAX_STATIONS) return false;
    key_of(s_count, key, sizeof(key));
    if (!appfw_store_set_str(key, rec)) return false;
    s_count++;
    (void)appfw_store_set_u16(STORE_CNT_KEY, s_count);
    return true;
}

bool radio_store_remove(int idx)
{
    if (appfw_biglist_available()) return false;
    radio_station_t st;
    if (!get_entry(idx, &st)) return false;
    if (radio_store_is_builtin(st.name, st.url)) return false;

    // 后续条目整体前移(顺序即门户/屏幕上的序号),尾键释放。
    char key[8], rec[STORE_REC_CAP];
    for (int i = idx + 1; i < (int)s_count; i++) {
        key_of(i, key, sizeof(key));
        if (!appfw_store_get_str(key, rec, sizeof(rec))) return false;
        key_of(i - 1, key, sizeof(key));
        if (!appfw_store_set_str(key, rec)) return false;
    }
    key_of(s_count - 1, key, sizeof(key));
    (void)appfw_store_set_str(key, "");
    s_count--;
    (void)appfw_store_set_u16(STORE_CNT_KEY, s_count);
    return true;
}

void radio_store_import_begin(void)
{
    if (appfw_biglist_available()) return;   // 大清单只读:门户导入整体跳过
    char key[8];
    for (int i = 0; i < (int)s_count; i++) {
        key_of(i, key, sizeof(key));
        (void)appfw_store_set_str(key, "");
    }
    s_count = 0;
    (void)appfw_store_set_u16(STORE_CNT_KEY, 0);
}

int radio_store_restore_factory(void)
{
    appfw_m3u_stats_t st = { 0 };
    // 大清单也算用户数据:恢复出厂 = 删 m3u/索引退回小清单,再物化出厂清单。
    if (appfw_biglist_available()) appfw_biglist_discard();
    radio_store_import_begin();
    materialize_builtin();
    appfw_m3u_parse(RADIO_M3U_DEFAULT, NULL, store_accept_http,
                    NULL, migrate_entry_cb, &st);
    (void)appfw_store_set_u16(STORE_CNT_KEY, s_count);
    ESP_LOGI(TAG, "恢复出厂清单:%u 台", (unsigned)s_count);
    return (int)s_count;
}

bool radio_store_is_builtin(const char *name, const char *url)
{
    radio_station_t b;
    for (int i = 0; i < radio_builtin_count(); i++) {
        if (!radio_builtin_get(i, &b)) continue;
        if (strcmp(b.name, name) == 0 && strcmp(b.url, url) == 0) return true;
    }
    return false;
}
