// main/radio_biglist.c —— 见 radio_biglist.h。
#include "radio_biglist.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "appfw_files.h"
#include "appfw_net.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "radio_player.h"

static const char *TAG = "radio_biglist";

#define LINE_BUF_CAP   512   // 合并清单最长行 435B,留余量
#define MAGIC          "RIDX"
#define HDR_VER        1
// 头部:魔数 4B + 版本 u16 + 条数 u16 + m3u 字节数 u32 + 保留 u32 = 16B。
typedef struct {
    char magic[4];
    uint16_t ver;
    uint16_t count;
    uint32_t m3u_size;
    uint32_t rsv;
} idx_hdr_t;
#define HDR_SIZE 16          // sizeof(idx_hdr_t),字段自然对齐无填充

static char s_dir[64] = "/files";   // 测试可改;设备上恒为 /files
static uint16_t s_count;            // RAM 里唯一常驻的清单状态(+文件大小)
static uint32_t s_m3u_size;
static bool s_avail;
static int64_t s_miss_check_us;     // 无文件时 1s 一探;就绪态每次都轻探
static uint32_t s_prev_sz;          // 上一轮轻探到的文件大小(判断"上传中")
static bool s_prev_valid;
static bool s_borrowed;             // 本次挂载借了解码器预留的洞(要还)
static int64_t s_last_read_us;      // 最近一次文件读(借洞 10s 无读即归还)

static void path_of(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", s_dir, name);
}

static void m3u_path(char *out, size_t cap) { path_of("radio.m3u", out, cap); }
static void idx_path(char *out, size_t cap) { path_of("radio.idx", out, cap); }

// 与 radio_m3u.c 同规:UTF-8 安全截断(截断点不落在多字节字符中间)。
static void copy_trunc(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) len = cap - 1;
    while (len && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// FAT 挂载(~9KB 连续堆)与 60KB 解码器预留装不进同一个堆:预留让位再
// 挂载(借洞);poll 里空闲 10s 卸载并把预留补回。设备上挂载失败十有八九
// 是预留握着最大块,放掉就能过。失败退避 10s:失败的挂载会漏磨损均衡
// 句柄(上限 8 个,漏光前必须少试)。
static int64_t s_mount_fail_us;

static bool fat_open(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_mount_fail_us != 0 && now - s_mount_fail_us < 10LL * 1000000LL)
        return false;
    if (appfw_files_ensure_mounted()) { s_mount_fail_us = 0; return true; }
    radio_player_release_reserve();
    if (!appfw_files_ensure_mounted()) {
        s_mount_fail_us = now;
        ESP_LOGW(TAG, "FAT 挂载失败,10s 内不再试");
        return false;
    }
    s_mount_fail_us = 0;
    s_borrowed = true;
    return true;
}

// 借的洞要还:10s 没有文件读就卸载 FAT,把 60KB 预留补回(播放/连接中
// 不补,radio_player_reacquire_reserve 自己会判断)。
static void fat_idle_check(int64_t now)
{
    if (s_last_read_us == 0) return;   // 从没用过文件
    if (now - s_last_read_us < 10LL * 1000000LL) return;
    (void)appfw_files_unmount();
    if (s_borrowed) radio_player_reacquire_reserve();
    s_borrowed = false;
}

static bool is_http_url(const char *s, size_t len)
{
    return (len >= 7 && strncmp(s, "http://", 7) == 0) ||
           (len >= 8 && strncmp(s, "https://", 8) == 0);
}

// 读一行:去行尾 \r\n 与空白、行首空白。超长行的剩余字节吞掉(与建索引时
// fgets 分行方式一致,偏移才对得上)。返回 false = 文件结束。
static bool read_line(FILE *f, char *buf, size_t cap, size_t *out_len)
{
    if (!fgets(buf, (int)cap, f)) return false;
    size_t len = strlen(buf);
    if (len == cap - 1 && buf[len - 1] != '\n') {
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') len++;
    }
    while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
                   buf[len - 1] == ' ' || buf[len - 1] == '\t')) len--;
    size_t lead = 0;
    while (lead < len && (buf[lead] == ' ' || buf[lead] == '\t')) lead++;
    if (lead) {
        memmove(buf, buf + lead, len - lead + 1);
        len -= lead;
    }
    buf[len] = '\0';
    *out_len = len;
    return true;
}

static bool idx_valid(uint32_t m3u_size, uint16_t *out_count)
{
    char p[96];
    idx_path(p, sizeof(p));
    FILE *fi = fopen(p, "rb");
    if (!fi) return false;
    idx_hdr_t h;
    bool ok = fread(&h, sizeof(h), 1, fi) == 1;
    if (ok) {
        ok = memcmp(h.magic, MAGIC, 4) == 0 && h.ver == HDR_VER &&
             h.count > 0 && h.m3u_size == m3u_size;
        if (ok && fseek(fi, 0, SEEK_END) == 0) {
            ok = (long)(HDR_SIZE + 4u * h.count) == ftell(fi);
        }
    }
    fclose(fi);
    if (ok && out_count) *out_count = h.count;
    return ok;
}

// 扫描 m3u 建索引:先写 radio.idx.tmp 占位头,流式扫完回填条数再原子改名。
// 收录规则与 radio_m3u_parse 对齐:#EXTINF(取第一个逗号后的标题)配对的
// 下一行是 http/https URL 才收;注释行忽略;无配对的裸 URL 不收。
static bool rebuild_index(uint32_t m3u_size, uint16_t *out_count)
{
    char pm3u[96], ptmp[96];
    m3u_path(pm3u, sizeof(pm3u));
    path_of("radio.idx.tmp", ptmp, sizeof(ptmp));
    FILE *fm = fopen(pm3u, "rb");
    if (!fm) return false;
    FILE *fi = fopen(ptmp, "wb");
    if (!fi) { fclose(fm); return false; }

    const idx_hdr_t placeholder = { { MAGIC[0], MAGIC[1], MAGIC[2], MAGIC[3] },
                                    HDR_VER, 0, m3u_size, 0 };
    bool ok = fwrite(&placeholder, sizeof(placeholder), 1, fi) == 1;
    uint32_t batch[64];
    uint16_t nbatch = 0, count = 0;
    bool pending = false;
    uint32_t pending_off = 0;
    char line[LINE_BUF_CAP];
    long pos = ftell(fm);

    while (ok && fgets(line, sizeof(line), fm)) {
        const long next = pos + (long)strlen(line);
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
        size_t lead = 0;   // 与 read_line 同规:行首空白剥掉再分类(偏移仍记行首)
        while (lead < len && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        if (lead) {
            memmove(line, line + lead, len - lead + 1);
            len -= lead;
        }
        if (len && line[0] == '#') {
            if (len >= 8 && strncmp(line, "#EXTINF:", 8) == 0 &&
                memchr(line, ',', len)) {
                pending = true;                 // 同名重挂:后一个 EXTINF 生效
                pending_off = (uint32_t)pos;
            }
        } else if (len) {
            if (pending && is_http_url(line, len) && count < UINT16_MAX) {
                pending = false;
                batch[nbatch++] = pending_off;
                if (nbatch == 64) {
                    ok = fwrite(batch, 4, 64, fi) == 64;
                    nbatch = 0;
                }
                count++;
            } else {
                pending = false;                // 无配对/被拒:EXTINF 作废
            }
        }
        pos = next;
    }
    if (ok && nbatch) ok = fwrite(batch, 4, nbatch, fi) == nbatch;
    if (ok && count > 0) {
        const idx_hdr_t hdr = { { MAGIC[0], MAGIC[1], MAGIC[2], MAGIC[3] },
                                HDR_VER, count, m3u_size, 0 };
        ok = fseek(fi, 0, SEEK_SET) == 0 && fwrite(&hdr, sizeof(hdr), 1, fi) == 1;
    }
    fclose(fi);
    fclose(fm);
    if (!ok || count == 0) {
        remove(ptmp);
        return false;
    }
    char pidx[96];
    idx_path(pidx, sizeof(pidx));
    (void)remove(pidx);                // FATFS 的 rename 不覆盖已存在目标,先删
    if (rename(ptmp, pidx) != 0) {
        ESP_LOGW(TAG, "索引改名失败,下轮重试");
        remove(ptmp);
        return false;
    }
    if (out_count) *out_count = count;
    ESP_LOGI(TAG, "索引重建:%u 台", (unsigned)count);
    return true;
}

void radio_biglist_init(void)
{
    s_count = 0;
    s_m3u_size = 0;
    s_avail = false;
    s_miss_check_us = INT64_MIN;
    s_prev_sz = 0;
    s_prev_valid = false;
}

bool radio_biglist_poll(void)
{
    const int64_t now = esp_timer_get_time();
    // 开机 WiFi 连接窗口不碰文件:那时堆最大块只有 ~7KB,挂载必然借走
    // 60KB 预留的洞,HLS/AAC 就废了。等 ONLINE 后最大块 ~34KB,挂载无需
    // 借洞,预留全程完好(真离线配网机不受此限,反正也播不了 AAC)。
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    // 只有联网稳定(ONLINE)、正在掉线重试、或开机 8 秒还没网(纯配网机)
    // 才允许碰文件——连接/扫描中间态的堆最破碎,挂载会借走 AAC 的洞。
    const bool net_ready = (st.state == APPFW_NET_ONLINE ||
                            st.state == APPFW_NET_OFFLINE_RETRY ||
                            now >= 8LL * 1000000LL);
    if (!net_ready) return false;
    // 无文件态整体 5s 一探(挂/卸 FAT 不便宜,别每秒折腾);就绪态不受限。
    if (!s_avail && s_miss_check_us != INT64_MIN &&
        now - s_miss_check_us < 5LL * 1000000LL) {
        return false;
    }

    fat_idle_check(now);

    char pm3u[48];
    m3u_path(pm3u, sizeof(pm3u));
    FILE *fm = fopen(pm3u, "rb");
    // 播放/连接期间不发起挂载:FAT 未挂载时轮询只报缓存条数,别去和
    // 解码器抢内存(挂载失败还漏 WL 句柄)。用户按键切台的 get 单发,
    // 不走这里。
    if (!fm) {
        radio_player_snap_t ps;
        radio_player_snapshot(&ps);
        if (ps.state == RADIO_CONNECTING || ps.state == RADIO_PLAYING)
            return s_avail;
    }
    if (!fm && fat_open()) fm = fopen(pm3u, "rb");
    if (!fm) {
        // 没有大清单:退回小清单,别让 FAT(≈8KB)常驻占播放/TLS 的内存。
        s_miss_check_us = now;
        if (s_avail) ESP_LOGI(TAG, "radio.m3u 消失,退回小清单");
        s_avail = false;
        s_count = 0;
        (void)appfw_files_unmount();
        if (s_borrowed) radio_player_reacquire_reserve();
        s_borrowed = false;
        return false;
    }
    s_miss_check_us = INT64_MIN;
    long sz = -1;
    if (fseek(fm, 0, SEEK_END) == 0) sz = ftell(fm);
    fclose(fm);
    if (sz <= 0) {
        s_avail = false;
        s_count = 0;
        return false;
    }
    // 就绪且大小没变:轻探结束(每次调用一次 fopen+ftell,零重建)。
    if (s_avail && (long)s_m3u_size == sz) {
        s_prev_sz = (uint32_t)sz;
        s_prev_valid = true;
        return true;
    }
    // 大小变了/首次见到:必须连续两轮一致才动手——上传中的文件每秒都在长,
    // 对半截文件建索引是白费,还会和上传写入抢 FAT。
    s_last_read_us = now;   // 真正动文件才续"借洞"计时(轻探不算)
    if (!s_prev_valid || s_prev_sz != (uint32_t)sz) {
        s_prev_sz = (uint32_t)sz;
        s_prev_valid = true;
        return s_avail;
    }

    uint16_t n = 0;
    if (!idx_valid((uint32_t)sz, &n) && !rebuild_index((uint32_t)sz, &n)) {
        s_avail = false;
        s_count = 0;
        return false;
    }
    s_m3u_size = (uint32_t)sz;
    s_count = n;
    s_avail = true;
    ESP_LOGI(TAG, "大清单就绪:%u 台(%ld 字节)", (unsigned)n, sz);
    return true;
}

bool radio_biglist_available(void) { return s_avail; }

int radio_biglist_count(void) { return s_avail ? (int)s_count : 0; }

bool radio_biglist_get(int idx, radio_station_t *out)
{
    if (!out || idx < 0 || idx >= (int)s_count || !fat_open()) return false;
    s_last_read_us = esp_timer_get_time();
    char pidx[96], pm3u[96];
    idx_path(pidx, sizeof(pidx));
    m3u_path(pm3u, sizeof(pm3u));

    FILE *fi = fopen(pidx, "rb");
    if (!fi) return false;
    uint32_t off = 0;
    const bool got = fseek(fi, HDR_SIZE + 4u * (uint32_t)idx, SEEK_SET) == 0 &&
                     fread(&off, 4, 1, fi) == 1;
    fclose(fi);
    if (!got) return false;

    FILE *fm = fopen(pm3u, "rb");
    if (!fm || fseek(fm, (long)off, SEEK_SET) != 0) {
        if (fm) fclose(fm);
        return false;
    }
    char line[LINE_BUF_CAP];
    size_t len = 0;
    bool ok = read_line(fm, line, sizeof(line), &len) &&
              len >= 8 && strncmp(line, "#EXTINF:", 8) == 0;
    if (ok) {
        const char *comma = memchr(line, ',', len);
        if (comma) {
            copy_trunc(out->name, sizeof(out->name), comma + 1,
                       len - (size_t)(comma + 1 - line));
        } else {
            ok = false;
        }
    }
    if (ok) {
        ok = read_line(fm, line, sizeof(line), &len) && is_http_url(line, len);
        if (ok) copy_trunc(out->url, sizeof(out->url), line, len);
    }
    fclose(fm);
    return ok;
}

int radio_biglist_find(const char *name)
{
    if (!name || !name[0] || !fat_open()) return -1;
    s_last_read_us = esp_timer_get_time();
    char pm3u[96];
    m3u_path(pm3u, sizeof(pm3u));
    FILE *fm = fopen(pm3u, "rb");
    if (!fm) return -1;

    char want[RADIO_NAME_MAX];
    copy_trunc(want, sizeof(want), name, strlen(name));
    char line[LINE_BUF_CAP];
    char nm[RADIO_NAME_MAX];
    bool match = false;
    uint32_t idx = 0;
    size_t len = 0;
    while (read_line(fm, line, sizeof(line), &len)) {
        if (len && line[0] == '#') {
            if (len >= 8 && strncmp(line, "#EXTINF:", 8) == 0) {
                const char *comma = memchr(line, ',', len);
                match = false;
                if (comma) {
                    copy_trunc(nm, sizeof(nm), comma + 1,
                               len - (size_t)(comma + 1 - line));
                    match = strcmp(nm, want) == 0;
                }
            }
        } else if (len) {
            if (match && is_http_url(line, len)) {
                fclose(fm);
                return (int)idx;
            }
            match = false;
            if (is_http_url(line, len)) idx++;   // 只数收录条目,与索引一致
        }
    }
    fclose(fm);
    return -1;
}

void radio_biglist_discard(void)
{
    char p[96];
    idx_path(p, sizeof(p));
    (void)remove(p);
    m3u_path(p, sizeof(p));
    (void)remove(p);
    s_avail = false;
    s_count = 0;
    s_m3u_size = 0;
    s_miss_check_us = INT64_MIN;
    (void)appfw_files_unmount();
    ESP_LOGI(TAG, "大清单已删除,退回小清单");
}

void radio_biglist_set_dir(const char *dir)
{
    snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : "/files");
}
