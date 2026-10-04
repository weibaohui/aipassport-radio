// main/radio_store.c —— 见 radio_store.h。
#include "radio_store.h"

#include <stdio.h>
#include <string.h>

#include "appfw_storage.h"
#include "esp_log.h"

#include "radio_catalog.h"

static const char *TAG = "radio_store";

#define USER_CNT_KEY  "u_cnt"       // 自定义台数(u16)
#define USER_REC_CAP  (RADIO_NAME_MAX + RADIO_URL_MAX + 2)  // "名\tURL" + NUL
#define LEGACY_CNT_KEY "r_cnt"      // 旧版清单计数键(内容全是台目副本,清空)
#define LEGACY_MAX     48           // 旧版上限(r0..r47)

static uint8_t s_user_n;            // RAM 里唯一常驻的清单状态

int radio_store_catalog_count(void)
{
    return radio_builtin_count() + RADIO_CATALOG_N;
}

static void user_key(int idx, char *key, size_t cap)
{
    snprintf(key, cap, "u%d", idx);
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

static bool user_get(int idx, radio_station_t *out)
{
    char key[8], rec[USER_REC_CAP];
    if (idx < 0 || idx >= (int)s_user_n) return false;
    user_key(idx, key, sizeof(key));
    if (!appfw_store_get_str(key, rec, sizeof(rec))) return false;
    return entry_unpack(rec, out);
}

static int user_find(const char *name)
{
    radio_station_t st;
    for (int i = 0; i < (int)s_user_n; i++) {
        if (user_get(i, &st) && strcmp(st.name, name) == 0) return i;
    }
    return -1;
}

void radio_store_init(void)
{
    // 旧版小清单(r0..r47)与台目完全重复,一次性清空腾 NVS;自定义台自 u* 起。
    uint16_t legacy = 0;
    if (appfw_store_get_u16(LEGACY_CNT_KEY, &legacy, 0) && legacy != 0) {
        char key[8];
        for (int i = 0; i < LEGACY_MAX; i++) {
            snprintf(key, sizeof(key), "r%d", i);
            (void)appfw_store_set_str(key, "");
        }
        (void)appfw_store_set_str(LEGACY_CNT_KEY, "");
        ESP_LOGI(TAG, "旧版清单键已清空(%u 台)", (unsigned)legacy);
    }

    uint16_t n = 0xFFFF;
    if (appfw_store_get_u16(USER_CNT_KEY, &n, 0xFFFF) && n <= RADIO_MAX_STATIONS) {
        s_user_n = (uint8_t)n;
    } else {
        s_user_n = 0;
    }
    ESP_LOGI(TAG, "台目 %d 台 + 自定义 %u 台", radio_store_catalog_count(),
             (unsigned)s_user_n);
}

int radio_store_count(void)
{
    return radio_store_catalog_count() + (int)s_user_n;
}

bool radio_store_get(int idx, radio_station_t *out)
{
    if (!out) return false;
    const int builtin_n = radio_builtin_count();
    if (idx < builtin_n) return radio_builtin_get(idx, out);
    const int ci = idx - builtin_n;
    if (ci < RADIO_CATALOG_N) {
        *out = RADIO_CATALOG[ci];
        return true;
    }
    return user_get(ci - RADIO_CATALOG_N, out);
}

int radio_store_find(const char *name)
{
    if (!name || !name[0]) return -1;
    const int u = user_find(name);
    if (u >= 0) return radio_store_catalog_count() + u;   // 用户改址优先

    radio_station_t st;
    for (int i = 0; i < radio_builtin_count(); i++) {
        if (radio_builtin_get(i, &st) && strcmp(st.name, name) == 0) return i;
    }
    for (int i = 0; i < RADIO_CATALOG_N; i++) {
        if (strcmp(RADIO_CATALOG[i].name, name) == 0) {
            return radio_builtin_count() + i;
        }
    }
    return -1;
}

bool radio_store_add(const char *name, const char *url)
{
    if (!name || !url) return false;
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= RADIO_NAME_MAX) return false;
    for (const char *p = name; *p; p++) {
        if ((unsigned char)*p < 0x20) return false;   // 含 \t\r\n,破坏记录格式
    }
    if (!radio_url_valid(url)) return false;

    char key[8], rec[USER_REC_CAP];
    if (!entry_pack(name, url, rec, sizeof(rec))) return false;
    const int i = user_find(name);
    if (i >= 0) {                                     // 同名 = 改地址
        user_key(i, key, sizeof(key));
        return appfw_store_set_str(key, rec);
    }
    if (s_user_n >= RADIO_MAX_STATIONS) return false;
    user_key(s_user_n, key, sizeof(key));
    if (!appfw_store_set_str(key, rec)) return false; // NVS 满:干净报错
    s_user_n++;
    (void)appfw_store_set_u16(USER_CNT_KEY, s_user_n);
    return true;
}

bool radio_store_remove(int idx)
{
    const int base = radio_store_catalog_count();
    if (idx < base) return false;                     // 台目段只读
    const int u = idx - base;
    radio_station_t st;
    if (!user_get(u, &st)) return false;

    // 后续条目整体前移(顺序即门户/屏幕上的序号),尾键释放。
    char key[8], rec[USER_REC_CAP];
    for (int i = u + 1; i < (int)s_user_n; i++) {
        user_key(i, key, sizeof(key));
        if (!appfw_store_get_str(key, rec, sizeof(rec))) return false;
        user_key(i - 1, key, sizeof(key));
        if (!appfw_store_set_str(key, rec)) return false;
    }
    user_key(s_user_n - 1, key, sizeof(key));
    (void)appfw_store_set_str(key, "");
    s_user_n--;
    (void)appfw_store_set_u16(USER_CNT_KEY, s_user_n);
    return true;
}

void radio_store_import_begin(void)
{
    char key[8];
    for (int i = 0; i < (int)s_user_n; i++) {
        user_key(i, key, sizeof(key));
        (void)appfw_store_set_str(key, "");
    }
    s_user_n = 0;
    (void)appfw_store_set_u16(USER_CNT_KEY, 0);
}

int radio_store_restore_factory(void)
{
    radio_store_import_begin();
    ESP_LOGI(TAG, "恢复出厂:自定义台已清空,台目 %d 台", radio_store_catalog_count());
    return radio_store_catalog_count();
}
