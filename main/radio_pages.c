// main/radio_pages.c —— 见 radio_pages.h。
//
// 界面分工:框架负责顶栏(标题+电量)、设置菜单、WiFi/配网/设备信息子页;
// 应用只画主页——电台列表、光标、播放状态与正在播放的曲名。
#include "radio_pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "appfw_client.h"
#include "appfw_ui.h"
#include "appfw_net.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "bsp_battery.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "radio_player.h"
#include "radio_store.h"
#include "radio_streams.h"
#include "radio_viz_view.h"

static const char *TAG = "radio_pages";

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_24);

#define LIST_MAX      5                       // 一屏行数
#define ROW_H         28
#define LIST_Y        50
#define ROW_X         14                      // 行左边距
#define ROW_W         212                     // 行宽

// 清单本体在 flash(radio_store,逐条 NVS);这里只留光标/窗口和一个条数缓存。
// 常驻 RAM 的清单副本已全部移除(旧实现 5 份 × 6.2KB ≈ 31KB 静态内存)。
static int s_sel;                             // 选中下标 0..count-1,count=设置行
static int s_off;                             // 滚动窗口起始
static int s_cur_idx = -1;                    // 正在播的台下标缓存(开播时更新;
                                              // 与播放器台名对不上就退回全表查找)
static uint8_t s_vol = 55;

// 主页有两层:电台列表,以及盖在它上面的频谱播放页。框架只给一个 home page
// (见 appfw_ui_cfg_t),所以播放页是应用自己叠上去的浮层,用显隐切换。
typedef enum { PAGE_LIST = 0, PAGE_PLAY } radio_page_t;
static radio_page_t s_page;

// 主页控件
static lv_obj_t *s_rows[LIST_MAX];
static lv_obj_t *s_state_label;
static lv_obj_t *s_title_label;
static lv_obj_t *s_vol_label;
static lv_obj_t *s_list_layer;               // 列表+状态区这一层
static lv_obj_t *s_play_layer;               // 频谱播放页
static radio_viz_view_t *s_play;

static lv_timer_t *s_viz_timer;

// 假频谱 10fps:32 根柱的整数运算本身微不足道,成本在 LVGL 重绘;10Hz 是
// "看着在动"与省电省 CPU 的折中。非播放态回调直接返回,零开销。
#define VIZ_PERIOD_MS 100

static lv_font_t s_f16, s_f24;
static lv_font_t s_f16_golden, s_f24_golden;   // [临时调试] 字体结构哨兵
static bool s_font_ready;

static const char *state_text(const radio_player_snap_t *s);

static void ensure_fonts(void)
{
    if (s_font_ready) return;
    s_f16 = app_font_16;
    s_f16.fallback = &lv_font_montserrat_14;
    s_f24 = app_font_24;
    s_f24.fallback = &lv_font_montserrat_20;
    s_f16_golden = s_f16;
    s_f24_golden = s_f24;
    s_font_ready = true;
}

static void style(lv_obj_t *l, const lv_font_t *f, uint32_t color)
{
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

// index 是 0..LIST_MAX, LIST_MAX 那行是末尾的「设置」行。
//
// 注意:下面第一行的 remove_style_all() 会把**所有本地样式**清掉,包括
// lv_obj_set_width()/set_pos() 写进去的 style_width/style_x/style_y —— LVGL 9
// 里这两个函数存的就是 selector 0 的本地样式。而选中态每次 home_poll(500ms)
// 都要重刷一遍,所以几何必须在这里重设,否则第一次 poll 之后所有行都会塌回
// (0,0) 堆到屏幕顶部。改动本函数时别把 set_width/set_pos 再挪出去。
static void row_style(lv_obj_t *l, int index, bool selected)
{
    lv_obj_remove_style_all(l);
    style(l, &s_f16, selected ? 0x0B1F16 : 0xC8D3DC);
    lv_obj_set_style_bg_color(l, lv_color_hex(selected ? 0x1E3A2C : 0x111820), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 6, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_width(l, ROW_W);
    lv_obj_set_pos(l, ROW_X, LIST_Y + index * ROW_H);
}

static void show_row(lv_obj_t *l, const char *text)
{
    lv_label_set_text(l, text);
    lv_obj_set_hidden(l, false);
}

static void hide_row(lv_obj_t *l)
{
    lv_obj_set_hidden(l, true);
}

int radio_pages_cursor(void) { return s_sel; }

// ---------------------------------------------------------------- 列表

static int station_count(void) { return radio_store_count(); }
static int total_rows(void) { return station_count(); }

static void clamp_cursor(void)
{
    const int total = total_rows();
    if (s_sel >= total) s_sel = total - 1;
    if (s_sel < 0) s_sel = 0;
    if (s_off > station_count() - LIST_MAX) s_off = station_count() - LIST_MAX;
    if (s_off < 0) s_off = 0;
    if (s_sel < s_off) s_off = s_sel;
    if (s_sel >= s_off + LIST_MAX) s_off = s_sel - LIST_MAX + 1;
}

static void apply_page(void)
{
    if (!s_list_layer || !s_play_layer) return;
    lv_obj_set_hidden(s_play_layer, s_page != PAGE_PLAY);
    lv_obj_set_hidden(s_list_layer, s_page != PAGE_LIST);
}

// 频谱分析只在音频线程跑(见 radio_player.c),这里只读它导出的快照。
// 顶栏的时间/电量/信号要调 BSP 和框架,变化很慢,没必要每帧重算 ——
// 每 500ms 刷一次就够了,频谱本身仍然是 30ms 一帧。
#define CHROME_PERIOD_MS 500

// title 最长 RADIO_TITLE_MAX(64),加上 "MP3 · 128 kbps · 44.1 kHz" 也要放得下。
static char s_ch_buf[2][80];
static char s_title_buf[RADIO_TITLE_MAX];   // 过滤后的曲名(列表页用)
static radio_viz_chrome_t s_chrome;

// rssi(dBm)→ 信号格数。-50 以上满格,-80 以下没信号。
static int rssi_bars(int8_t rssi)
{
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

#include "radio_title_table.h"

// 曲名白名单过滤:流里的歌名是任意文本,16px 字库只覆盖静态文案的字。
// 字库外的字进 LVGL 只会渲染成方框 —— 这里直接丢弃(按 UTF-8 逐字判断,
// ASCII 全保留,CJK/全角查自动生成的码点表,二分查找)。
static bool title_cp_in_font(uint32_t cp)
{
    int lo = 0, hi = RADIO_TITLE_CP_COUNT - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k_radio_title_cps[mid] == cp) return true;
        if (k_radio_title_cps[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

static void sanitize_title(const char *src, char *dst, size_t cap)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < cap; ) {
        const uint8_t b = (uint8_t)src[i];
        if (b < 0x80) {                       // ASCII 原样保留
            dst[di++] = src[i++];
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((b & 0xE0) == 0xC0 && (src[i + 1] & 0xC0) == 0x80) {
            len = 2; cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0 && (src[i + 1] & 0xC0) == 0x80 &&
                   (src[i + 2] & 0xC0) == 0x80) {
            len = 3; cp = (b & 0x0F) << 12;
            cp |= ((uint32_t)src[i + 1] & 0x3F) << 6;
            cp |= (uint32_t)src[i + 2] & 0x3F;
        } else {                              // 坏字节/4 字节(emoji 等):丢
            i++;
            continue;
        }
        if (title_cp_in_font(cp) && di + len < cap) {
            for (int k = 0; k < len; k++) dst[di++] = src[i + k];
        }
        i += len;
    }
    dst[di] = '\0';
}

// "假频谱"包络:两道波沿频段方向传播、8 帧一循环的静态表(ROM)。柱高 =
// 真实音量电平 × 包络值/256 —— 全整数,无 FFT、无浮点、无逐采样统计。
// 视觉上"有声音就动,越响越烈",不承诺频率真实性。
static const uint8_t K_ENV[8][APPFW_VIZ_BANDS] = {
    {158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87},
    {229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158},
    {255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229},
    {229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255},
    {158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229},
    { 87,  60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158},
    { 60,  87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87},
    { 87, 158, 229, 255, 229, 158,  87,  60,  87, 158, 229, 255, 229, 158,  87,  60},
};

static const radio_viz_chrome_t *play_chrome(const radio_player_snap_t *s)
{
    // GCC 误报压制:chrome 缓冲的指针逃逸进 s_chrome(供 LVGL 异步读),
    // 跨调用分析推不出数组界,把合法写入报成 stringop-overflow。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    snprintf(s_ch_buf[0], sizeof(s_ch_buf[0]), "RADIO");

    // 时间:和框架一样,没对时就显示 --:--,免得给出 1970 误导。
    time_t now = time(NULL);
    if (now > 1000000000) {
        struct tm tm_local;
        gmtime_r(&(time_t){ now + 8 * 3600 }, &tm_local);   // 设备无时区配置,固定东八区
        snprintf(s_ch_buf[1], sizeof(s_ch_buf[1]), "%02d:%02d",
                 tm_local.tm_hour, tm_local.tm_min);
    } else {
        snprintf(s_ch_buf[1], sizeof(s_ch_buf[1]), "--:--");
    }

    appfw_net_status_t net;
    appfw_net_get_status(&net);

    const int soc = bsp_battery_soc();
    if (soc >= 0) snprintf(s_ch_buf[3], sizeof(s_ch_buf[3]), "%d%%", soc);
    else          snprintf(s_ch_buf[3], sizeof(s_ch_buf[3]), "--");

    const int total = total_rows();
    const int cur = (s->station[0]) ? s_sel + 1 : 0;
    snprintf(s_ch_buf[4], sizeof(s_ch_buf[4]), "CH %02d / %02d",
             cur > total ? 0 : cur, total);

    // ICY 曲名(有就显示;音频参数移到信息卡常驻展示)。
    if (s->title[0]) {
        sanitize_title(s->title, s_ch_buf[5], sizeof(s_ch_buf[5]));
    } else {
        s_ch_buf[5][0] = '\0';
    }

    // ---- 信息卡:码率·采样率·声道(单行常驻) ----
    if (s->bitrate) {
        const unsigned br = (unsigned)s->bitrate;
        const unsigned khz = (unsigned)(s->sample_rate / 1000);
        const unsigned tent = (unsigned)((s->sample_rate % 1000) / 100);
        snprintf(s_ch_buf[2], sizeof(s_ch_buf[2]), "%u kbps · %u.%u kHz · %uch",
                 br, khz, tent, (unsigned)s->channels);
    } else if (s->sample_rate) {
        snprintf(s_ch_buf[2], sizeof(s_ch_buf[2]), "%u.%u kHz · %uch",
                 (unsigned)(s->sample_rate / 1000),
                 (unsigned)((s->sample_rate % 1000) / 100), (unsigned)s->channels);
    } else {
        snprintf(s_ch_buf[2], sizeof(s_ch_buf[2]), "—");
    }

    snprintf(s_ch_buf[7], sizeof(s_ch_buf[7]), "%s",
             s->station[0] ? s->station : "—");

    snprintf(s_ch_buf[6], sizeof(s_ch_buf[6]), "%s", state_text(s));
#pragma GCC diagnostic pop

    s_chrome.app_name    = s_ch_buf[0];
    s_chrome.clock       = s_ch_buf[1];
    s_chrome.signal_bars = rssi_bars(net.rssi);
    s_chrome.battery     = s_ch_buf[3];
    s_chrome.channel     = s_ch_buf[4];
    s_chrome.title       = s_ch_buf[5];
    s_chrome.station     = s_ch_buf[7];
    s_chrome.status      = s_ch_buf[6];
    s_chrome.info2       = s_ch_buf[2];
    s_chrome.status_bad  = (s->state == RADIO_ERROR);
    return &s_chrome;
}



// ---- 动态效果前置(实现在本文件下方) ----
static uint8_t s_effect;
static void fx_led_render(const uint8_t *bands);
static void fx_sym_render(const uint8_t *bands);

static void viz_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_play) return;
    apply_page();                    // 按键只改 s_page(锁外),显隐在这里落地
    if (s_page != PAGE_PLAY) return;

    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_PLAYING) return;   // 连接中/出错/停止:冻结画面,零开销

    static uint8_t tick8;
    tick8 = (uint8_t)((tick8 + 1) & 7);
    // 活动下限 60:轻声/停顿柱子仍有低幅度的舞动,响度越大起伏越大。
    const uint8_t lvl = radio_player_level();
    const uint8_t eff = lvl < 60 ? 60 : lvl;
    uint8_t bands[APPFW_VIZ_BANDS];
    static uint8_t disp[APPFW_VIZ_BANDS];   // 每根柱的平滑值(起快落慢)
    for (int k = 0; k < APPFW_VIZ_BANDS; k++) {
        // >>7(而非 >>8):包络增益 ×2,中等响度就有可感的柱高。
        const uint16_t v = ((uint16_t)eff * K_ENV[tick8][k]) >> 7;
        const uint8_t target = (uint8_t)(v > 255 ? 255 : v);
        // 整数平滑:上升 >>1(快),回落 >>3(慢)—— 消除逐帧跳变的"假"感。
        uint8_t d = disp[k];
        disp[k] = (uint8_t)(target > d ? d + ((target - d) >> 1)
                                       : d - ((d - target) >> 3));
        bands[k] = disp[k];
    }

    // [临时调试] 字体结构哨兵:被改写即刻记录时刻+字段偏移+新旧值
    {
        static uint8_t reported;
        const lv_font_t *cur[2] = { &s_f16, &s_f24 };
        const lv_font_t *gold[2] = { &s_f16_golden, &s_f24_golden };
        for (int fi = 0; fi < 2; fi++) {
            const uint8_t *c = (const uint8_t *)cur[fi];
            const uint8_t *g = (const uint8_t *)gold[fi];
            for (size_t b = 0; b < sizeof(lv_font_t); b++) {
                if (c[b] != g[b] && !(reported & (1 << (fi * 8 + (b > 7 ? 7 : b))))) {
                    reported |= (uint8_t)(1 << (fi * 8 + (b > 7 ? 7 : b)));
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000LL);
                    ESP_LOGE(TAG, "[哨兵] 字体%d 偏移+%u 被改: %02x→%02x (t=%us)",
                             fi, (unsigned)b, g[b], c[b], now);
                }
            }
        }
    }
    // [临时调试] 播放中强制进播放页(复现按键播放的 UI 路径)
    if (s.state == RADIO_PLAYING && s_page != PAGE_PLAY) {
        s_page = PAGE_PLAY;
        apply_page();
    }
    static uint32_t chrome_tick;
    if (++chrome_tick % (CHROME_PERIOD_MS / VIZ_PERIOD_MS) == 0) play_chrome(&s);

    if (s_effect == 0) {
        radio_viz_view_update(s_play, bands, lvl, s.volume, &s_chrome);
        return;
    }
    if (s_effect == 1) fx_led_render(bands);       // LED 电平表
    else fx_sym_render(bands);                     // 对称频谱
}

// ---- 动态效果(设置「动态效果」三选一,面板 220x44 内渲染) ----
#define FX_PANEL_Y   206
#define FX_PANEL_H   44
#define FX_SEGS      4                          // LED 表 4 段(44px/段 11px)
#define FX_SEG_H     11
#define FX_GREEN     0x35C26B
#define FX_YELLOW    0xFFC531
#define FX_RED       0xE5484D
#define FX_DIM       0x16202E

static uint16_t s_fx_cap[APPFW_VIZ_BANDS];          // LED 峰帽 q8
static lv_obj_t *s_fx_led[APPFW_VIZ_BANDS * 2];     // 柱 + 帽
static lv_obj_t *s_fx_sym[APPFW_VIZ_BANDS * 2 + 1]; // 上柱 + 下柱 + 中心线
static uint8_t s_fx_last_h[APPFW_VIZ_BANDS];        // 差分缓存

static lv_obj_t *fx_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
                         int32_t h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// 经典频谱 ↔ 效果组 的互斥显隐
static void fx_show_bars(bool show)
{
    if (s_play) {
        if (show) lv_obj_clear_flag(s_play->bars.root, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_play->bars.root, LV_OBJ_FLAG_HIDDEN);
        s_play->bars.rainbow = show;           // 经典频谱:彩虹流动色
    }
}

static void fx_teardown(void)
{
    for (int i = 0; i < APPFW_VIZ_BANDS * 2; i++) {
        if (s_fx_led[i]) { lv_obj_delete(s_fx_led[i]); s_fx_led[i] = NULL; }
    }
    for (int i = 0; i < APPFW_VIZ_BANDS * 2 + 1; i++) {
        if (s_fx_sym[i]) { lv_obj_delete(s_fx_sym[i]); s_fx_sym[i] = NULL; }
    }
    memset(s_fx_last_h, 0, sizeof(s_fx_last_h));
}

static void fx_apply(uint8_t v)
{
    s_effect = v;
    fx_teardown();
    fx_show_bars(v == 0);
    if (v == 1 && s_play) {
        for (int i = 0; i < APPFW_VIZ_BANDS; i++) {
            const int32_t x = 10 + 6 + i * 13;     // 面板内 16 列(与柱阵同槽)
            s_fx_led[i] = fx_rect(s_play_layer, x, FX_PANEL_Y + FX_PANEL_H,
                                  11, 1, FX_GREEN);
            s_fx_led[APPFW_VIZ_BANDS + i] =
                fx_rect(s_play_layer, x, FX_PANEL_Y + FX_PANEL_H - 2, 11, 2, FX_YELLOW);
        }
    } else if (v == 2 && s_play) {
        const int cx = FX_PANEL_Y + FX_PANEL_H / 2;  // 面板中线 y=228
        for (int i = 0; i < APPFW_VIZ_BANDS; i++) {
            const int32_t x = 10 + 6 + i * 13;
            const uint16_t idx = (uint16_t)((uint32_t)i * 26 / 10);
            s_fx_sym[i] = fx_rect(s_play_layer, x, cx - 1, 11, 1, 0x2F8C86);
            s_fx_sym[APPFW_VIZ_BANDS + i] =
                fx_rect(s_play_layer, x, cx, 11, 1, 0x2F8C86);
            (void)idx;
        }
        s_fx_sym[APPFW_VIZ_BANDS * 2] =
            fx_rect(s_play_layer, 16, cx - 1, 208, 2, 0x3A4A5C);
    }
}

void radio_pages_set_effect(uint8_t v)
{
    if (v > 2) v = 2;
    if (v == s_effect && s_fx_led[0] == NULL && s_fx_sym[0] == NULL && v == 0) return;
    s_effect = v;
    if (!s_play || !s_play_layer) {
        // 播放页还没建(开机读回/页面重建中):只记档位,home_build 末尾统一应用
        ESP_LOGI(TAG, "动态效果待应用:%u", (unsigned)v);
        return;
    }
    // on_change 跑在锁外上下文,对象操作必须自己持 LVGL 锁
    if (!bsp_lvgl_lock(500)) return;
    fx_apply(v);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "动态效果:%s", v == 0 ? "经典频谱" : v == 1 ? "LED 电平表" : "对称频谱");
}

// LED 电平表:高度量化成段(分段感),彩虹流动色,峰帽 2px 缓落
static void fx_led_render(const uint8_t *bands)
{
    if (!s_play_layer) return;
    static uint16_t hue;
    hue = (uint16_t)((hue + 4) % 360);
    for (int i = 0; i < APPFW_VIZ_BANDS; i++) {
        const uint8_t seg = (uint8_t)((uint32_t)bands[i] * FX_SEGS / 256);
        const uint8_t h = (uint8_t)(seg * FX_SEG_H);
        lv_obj_t *col = s_fx_led[i];
        if (!col) continue;
        if (h != s_fx_last_h[i]) {
            s_fx_last_h[i] = h;
            if (h) {
                lv_obj_clear_flag(col, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(col, h);
                lv_obj_set_y(col, FX_PANEL_Y + FX_PANEL_H - h);
            } else {
                lv_obj_add_flag(col, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 彩虹流动色:每列错开 36°,整体每帧推进
        const uint16_t rh2 = (uint16_t)((hue + (uint32_t)i * 36) % 360);
        lv_obj_set_style_bg_color(col, lv_color_hsv_to_rgb(rh2, 82, 80), 0);
        // 峰帽
        lv_obj_t *cap = s_fx_led[APPFW_VIZ_BANDS + i];
        uint32_t c = s_fx_cap[i];
        if (bands[i] >= c) c = bands[i];
        else c = c > (140u * VIZ_PERIOD_MS / 1000u) ? c - (140u * VIZ_PERIOD_MS / 1000u) : 0;
        s_fx_cap[i] = (uint16_t)c;
        const int32_t cy = FX_PANEL_Y + FX_PANEL_H - 2 - (int32_t)c * FX_PANEL_H / 256;
        lv_obj_set_y(cap, (int32_t)(int16_t)cy);
    }
}

// 对称频谱:面板中线向上下生长,下半为上半的 0.55 倍
static void fx_sym_render(const uint8_t *bands)
{
    if (!s_play_layer) return;
    static uint16_t hue;
    hue = (uint16_t)((hue + 4) % 360);
    const int32_t cx = FX_PANEL_Y + FX_PANEL_H / 2;
    for (int i = 0; i < APPFW_VIZ_BANDS; i++) {
        const int32_t up = (int32_t)bands[i] * 22 / 256;       // 上半最大 22px
        const int32_t dn = up * 55 / 100;
        lv_obj_t *u = s_fx_sym[i], *d = s_fx_sym[APPFW_VIZ_BANDS + i];
        if (!u || !d) continue;
        const uint16_t rh_ = (uint16_t)((hue + (uint32_t)i * 36) % 360);
        const lv_color_t rc = lv_color_hsv_to_rgb(rh_, 82, 80);
        lv_obj_set_style_bg_color(u, rc, 0);
        lv_obj_set_style_bg_color(d, rc, 0);
        if (up) {
            lv_obj_clear_flag(u, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(u, up);
            lv_obj_set_y(u, cx - up);
        } else {
            lv_obj_add_flag(u, LV_OBJ_FLAG_HIDDEN);
        }
        if (dn) {
            lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(d, dn);
        } else {
            lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 框架重建页面前回调(见 appfw_ui_cfg_t::page_reset):旧页面对象即将被删,
// 把所有挂在上面的把手清空,等 home_build 重建时再重新绑定。
void radio_pages_page_reset(void)
{
    if (s_viz_timer) { lv_timer_del(s_viz_timer); s_viz_timer = NULL; }   // 防重建泄漏
    memset(s_rows, 0, sizeof(s_rows));
    s_state_label = NULL;
    s_title_label = NULL;
    s_vol_label = NULL;
    s_list_layer = NULL;
    s_play_layer = NULL;
    s_play = NULL;
    memset(s_fx_led, 0, sizeof(s_fx_led));
    memset(s_fx_sym, 0, sizeof(s_fx_sym));
}

void radio_pages_home_build(lv_obj_t *page)
{
    ensure_fonts();

    // 列表层:原来的列表 + 状态区都挂到这里,便于和播放页整体切换。
    s_list_layer = lv_obj_create(page);
    lv_obj_remove_style_all(s_list_layer);
    lv_obj_set_size(s_list_layer, 240, 320);
    lv_obj_set_pos(s_list_layer, 0, 0);
    lv_obj_set_scrollbar_mode(s_list_layer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_list_layer, LV_OPA_TRANSP, 0);

    for (int i = 0; i < LIST_MAX; i++) {
        s_rows[i] = lv_label_create(s_list_layer);
        row_style(s_rows[i], i, false);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_DOT);
    }

    s_state_label = lv_label_create(s_list_layer);
    style(s_state_label, &s_f16, 0x8B98A5);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_state_label, 212);
    lv_obj_set_pos(s_state_label, 14, 214);
    lv_label_set_text(s_state_label, "按 OK 播放");

    s_title_label = lv_label_create(s_list_layer);
    style(s_title_label, &s_f16, 0x7FD4A0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_label, 212);
    lv_obj_set_pos(s_title_label, 14, 238);
    lv_label_set_text(s_title_label, "");

    s_vol_label = lv_label_create(s_list_layer);
    style(s_vol_label, &s_f16, 0x6E7A86);
    lv_obj_set_pos(s_vol_label, 14, 284);
    lv_label_set_text_fmt(s_vol_label, "音量 %u%%   设置菜单可调", s_vol);

    // 播放页浮层
    s_play_layer = radio_viz_view_create(page, &s_f16, &s_f24);
    s_play = (radio_viz_view_t *)lv_obj_get_user_data(s_play_layer);
    if (!s_play) {
        ESP_LOGE(TAG, "播放页创建失败");
    } else {
        s_viz_timer = lv_timer_create(viz_timer_cb, VIZ_PERIOD_MS, NULL);
        fx_apply(s_effect);          // 按当前档位建效果对象(开机/页面重建统一入口)
    }

    s_page = PAGE_LIST;
    apply_page();
    clamp_cursor();
}

static const char *state_text(const radio_player_snap_t *s)
{
    switch (s->state) {
    case RADIO_CONNECTING: return "正在连接…";
    case RADIO_PLAYING:    return "正在收听";
    case RADIO_PAUSED:     return "已暂停";
    case RADIO_ERROR:
        switch (s->err_code) {
        case RADIO_ERR_URL:     return "地址不合法";
        case RADIO_ERR_RESOLVE: return "解析失败,检查网络";
        case RADIO_ERR_CONNECT: return "连接失败";
        case RADIO_ERR_HTTP:    return "服务端拒绝";
        case RADIO_ERR_DECODE:  return "音频解码失败";
        case RADIO_ERR_TIMEOUT: return "流已断开";
        default:                 return "未知错误";
        }
    default: return "OK播放 · 双击上下翻页";
    }
}

void radio_pages_home_poll(void)
{
    radio_player_snap_t s;
    radio_player_snapshot(&s);

    // 播放页:只按 2Hz 刷新台名/状态/曲名文字,频谱与一切动画已停——
    // 把 CPU 全部让给解码与 I2S,播放流畅度优先(2026-10-03)。
    if (s_page == PAGE_PLAY) {
        if (s_play) {
            static uint8_t chrome_tick;
            if (++chrome_tick >= CHROME_PERIOD_MS / 500) {
                chrome_tick = 0;
                play_chrome(&s);
                radio_viz_view_update(s_play, NULL, 0, s.volume, &s_chrome);
            }
        }
        if (s.state == RADIO_STOPPED) s_page = PAGE_LIST;
        apply_page();
        return;
    }

    clamp_cursor();
    const int count = station_count();
    for (int i = 0; i < LIST_MAX; i++) {
        const int idx = s_off + i;
        radio_station_t st;                    // 128B,栈上;画哪行读哪条
        if (idx >= count || !radio_store_get(idx, &st)) { hide_row(s_rows[i]); continue; }
        // LVGL 9 的 LV_SYMBOL_* 是字符串(不是单字符码),必须用 %s 拼。
        char text[RADIO_NAME_MAX + 24];
        const bool playing = (s.state == RADIO_PLAYING || s.state == RADIO_CONNECTING) &&
                             strcmp(s.station, st.name) == 0;
        snprintf(text, sizeof(text), "%s %s%s",
                 (idx == s_sel) ? LV_SYMBOL_RIGHT : " ",
                 playing ? LV_SYMBOL_PLAY " " : "",
                 st.name);
        show_row(s_rows[i], text);
        row_style(s_rows[i], i, idx == s_sel);
    }

    lv_label_set_text(s_state_label, state_text(&s));
    if (s.title[0]) sanitize_title(s.title, s_title_buf, sizeof(s_title_buf));
    else s_title_buf[0] = '\0';
    lv_label_set_text(s_title_label, s_title_buf);
    lv_label_set_text_fmt(s_vol_label, "音量 %u%%  长按:上设置 下音量", s.volume);
    const uint32_t col = (s.state == RADIO_ERROR) ? 0xE5484D
                       : (s.state == RADIO_PLAYING) ? 0x35C26B : 0x8B98A5;
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(col), 0);

    // 只有**主动停止**才退回列表。出错时留在播放页:状态行会写清楚
    // 「解析失败,检查网络」这类原因,直接跳回列表反而把原因藏了 ——
    // 而且在模拟器/没网的机器上,频谱页是唯一能看到频谱页版面的地方。
    if (s_page == PAGE_PLAY && s.state == RADIO_STOPPED) {
        s_page = PAGE_LIST;
    }
    apply_page();
}

// ---------------------------------------------------------------- 按键

// 上一台/下一台。直播电台没有"曲目"概念,对收音机而言换台就是上一首/下一首。
// 长按 OK = 下一台,双击 OK = 上一台(这两个事件在框架规整后是空闲的:
// 0=单击 2=双击 3=长按,按下瞬间已被框架丢弃)。
static void step_station(int delta)
{
    const int n = station_count();
    if (n == 0) return;
    radio_player_snap_t s;
    radio_player_snapshot(&s);

    // 先信下标缓存(开播时已记),对不上台名再全表按名找——正常换台只读 2 条。
    radio_station_t st;
    int cur = s_cur_idx;
    if (cur < 0 || cur >= n || !radio_store_get(cur, &st) ||
        strcmp(st.name, s.station) != 0) {
        cur = radio_store_find(s.station);
    }
    const int next = (cur < 0)
                   ? (delta > 0 ? 0 : n - 1)                       // 没在听就从两端起
                   : ((cur + delta) % n + n) % n;
    if (!radio_store_get(next, &st)) return;
    radio_play(st.name, st.url);
    s_cur_idx = next;
    s_sel = next;            // 光标跟着正在播的台走:列表高亮与 CH 号才不会说谎
    clamp_cursor();
    s_page = PAGE_PLAY;
}

static void toggle_station(int idx)
{
    radio_station_t st;
    if (idx < 0 || idx >= station_count()) return;
    if (!radio_store_get(idx, &st)) return;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_STOPPED && s.state != RADIO_ERROR &&
        strcmp(s.station, st.name) == 0) {
        radio_stop();
        return;
    }
    radio_play(st.name, st.url);
    s_cur_idx = idx;
    s_page = PAGE_PLAY;      // 开始播就切到频谱页,不然按了 OK 看不到反应
}

appfw_key_action_t radio_pages_home_key(int btn, int ev)
{
    const int total = total_rows();

    // 长按动作表在框架配置里(lp_up=菜单 / lp_down=音量页,见 main.c),
    // 上/下长按返回 DEFAULT 交给框架;应用只保留自管页面:长按 OK = 选台列表。
    if (ev == 3) {
        if (btn == 2) { s_page = PAGE_LIST; return APPFW_KEY_CONSUMED; }
        return APPFW_KEY_DEFAULT;
    }

    switch (ev) {
    case 0: // 单击
        if (s_page == PAGE_PLAY) {
            // 播放页:上下切台;OK 暂停/继续。
            if (btn == 0) step_station(-1);
            else if (btn == 1) step_station(+1);
            else if (btn == 2) radio_player_toggle_pause();
            return APPFW_KEY_CONSUMED;
        }
        if (total <= 0) return APPFW_KEY_CONSUMED;
        if (btn == 0) { s_sel = (s_sel - 1 + total) % total; return APPFW_KEY_CONSUMED; }
        if (btn == 1) { s_sel = (s_sel + 1) % total; return APPFW_KEY_CONSUMED; }
        if (btn == 2) {
            toggle_station(s_sel);
            return APPFW_KEY_CONSUMED;
        }
        return APPFW_KEY_CONSUMED;
    case 2: // 双击 = 上一页/下一页按钮(统一一页 5 行,设备一屏正好 5 行):
            // 游标落新页首行,整窗翻动;播放页忽略。
        if (s_page != PAGE_PLAY && total > 0 && btn <= 1) {
            int off = s_off + ((btn == 1) ? LIST_MAX : -LIST_MAX);
            if (off < 0) off = 0;
            if (off > total - LIST_MAX) off = total - LIST_MAX;
            s_off = off;
            s_sel = off;
        }
        return APPFW_KEY_CONSUMED;
    default:
        // 双击等其余事件不再承担功能(按 2026-10-03 定稿:上下选台/OK 播放/长按回列表)。
        return APPFW_KEY_CONSUMED;
    }
}

// ---------------------------------------------------------------- 持久化

// 清单持久化整体在 radio_store:台目 rodata + 自定义台逐条 NVS,
// 本文件不再持有任何清单状态,也不再有内存借洞/挂载这类文件系统事务。
void radio_pages_init(void)
{
    s_sel = 0;
    s_off = 0;
    s_cur_idx = -1;
    // 音量与设置菜单(框架应用选项页)同源:两边都读写 opt_volume,
    // 开机读回一次,列表页的"音量 N%"才不会和实际音量脱节。
    uint16_t vol = 55;
    appfw_store_get_u16("opt_volume", &vol, 55);
    s_vol = (uint8_t)vol;
    radio_store_init();
}

// ---------------------------------------------------------------- 设备信息

int radio_pages_info_rows(char (*keys)[16], char (*vals)[72], int max)
{
    int n = 0;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (n < max) {
        snprintf(keys[n], 16, "电台数");
        snprintf(vals[n], 72, "%d 个", station_count());
        n++;
    }
    if (n < max) {
        snprintf(keys[n], 16, "当前台");
        snprintf(vals[n], 72, "%s", s.station[0] ? s.station : "未选择");
        n++;
    }
    return n;
}

// ---------------------------------------------------------------- 门户配置

// 大清单模式的配置页:只读 + 分页浏览(10 条/页,"下一页"按钮点击再取 10 条,
// 永远不在网页/内存里摊开整表)。增删改的入口是局域网 AI:MCP 的
// playlist_import_begin/add/finish 逐条导入(2026-10-03 起,不再有网页上传)。


void radio_pages_app_config_fill(void *obj)
{
    cJSON *root = (cJSON *)obj;
    // 框架的 /api/status 与 /api/config/export 不再携带整份清单:48 台的
    // cJSON 树+打印要 15-25KB 瞬时堆,这台机器给不起(实测间歇 500)。
    // 清单回显走应用私有端点 /api/radio {op:"list"}(手拼 JSON 文本,~6KB);
    // 清单本体在逐条 NVS(radio_store),导出/导入不覆盖它。
    cJSON_AddItemToObject(root, "stations", cJSON_CreateArray());
}

bool radio_pages_app_config_apply(void *root_obj)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive((cJSON *)root_obj, "stations");
    if (!cJSON_IsArray(arr)) return true;   // 没有该字段 = 不改电台

    // 整表替换:清空自定义台,再按导入顺序逐条装回(下标即门户看到的序号)。
    // 台目段不受导入影响,始终在前。
    radio_store_import_begin();
    const cJSON *o = NULL;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "u");
        if (!cJSON_IsString(n) || !cJSON_IsString(u)) continue;
        if (!radio_store_add(n->valuestring, u->valuestring)) {
            ESP_LOGW(TAG, "导入时丢弃非法电台: %s", n->valuestring);
        }
    }
    s_sel = 0;
    s_off = 0;
    s_cur_idx = -1;
    ESP_LOGI(TAG, "导入完成,共 %d 个电台", station_count());
    return true;
}

// ---------------------------------------------------------------- 私有端点

void radio_pages_restore_user(void)
{
    radio_store_restore_factory();
    s_sel = 0;
    s_off = 0;
    s_cur_idx = -1;
}

// 清单回显改为**手拼 JSON 文本**:整表走 cJSON(树+打印)要 15-25KB 瞬时堆,
// 门户开着时空闲堆只有 ~20KB,实测 /api/status 与 del/add 的列表回显间歇 oom
// (连接被直接掐断)。文本直拼只需 ~6KB,堆水位 8KB 以下才截断。
// URL 经 radio_url_valid 校验(无空白/控制字符);台名再剔一遍引号与反斜杠,
// 保证不破坏 JSON 结构。
static size_t radio_json_escape(char *dst, size_t cap, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 2 < cap; i++) {
        const char c = src[i];
        if (c == '\x22' || c == '\\') {   // \x22 = 双引号(字面量别写成 '"',会干扰字形收集的正则)
            if (di + 2 >= cap) break;
            dst[di++] = '\\';
        }
        dst[di++] = c;
    }
    dst[di] = '\0';
    return di;
}

// 把清单(或其一段)按 HTTP 分块发出去(调用方已发过前缀)。零大块分配——
// 整表 cJSON 或 malloc(12KB) 在碎片堆上都拿不到(实测 500/断连),512B 一条
// 总能发出去。from/limit = [from, from+limit) 下标窗口(门户分页拉取);
// limit<=0 时回显到 PORTAL_ECHO_CAP 封顶。trunc 置 true=封顶/堆不足/发送失败。
#define PORTAL_ECHO_CAP 200
static esp_err_t radio_stations_chunks(httpd_req_t *req, bool *trunc, int *emitted,
                                       int from, int limit)
{
    // 最坏情形:名/址 JSON 转义后各翻倍(64 + 512)加结构开销,缓冲必须
    // 装得下,snprintf 的截断告警才是真无截断。
    char line[RADIO_NAME_MAX * 2 + RADIO_URL_MAX * 2 + 48];
    *trunc = false;
    *emitted = 0;
    radio_station_t st;
    const int count = station_count();
    if (from < 0 || from > count) from = 0;
    if (limit <= 0 || limit > PORTAL_ECHO_CAP) limit = PORTAL_ECHO_CAP;
    for (int i = from; i < count && *emitted < limit; i++) {
        if (!radio_store_get(i, &st)) continue;
        // 堆水位只是门户自己的保护线(发送本身不占堆)。
        if (i > 0 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 6 * 1024) {
            *trunc = true;
            ESP_LOGW(TAG, "清单回显截断:%d/%d 台(堆不足)", i, count);
            break;
        }
        char n[RADIO_NAME_MAX * 2], u[RADIO_URL_MAX * 2];
        radio_json_escape(n, sizeof(n), st.name);
        radio_json_escape(u, sizeof(u), st.url);
        snprintf(line, sizeof(line), "%s{\"n\":\"%s\",\"u\":\"%s\",\"builtin\":%s}",
                 (*emitted) ? "," : "", n, u,
                 i < radio_store_catalog_count() ? "true" : "false");
        // ≤256B 分片发送:堆紧时 lwip 的 TCP 发送缓冲很小,4KB 一口的
        // send 会在半路 MEM 失败(实测响应恰好断在 4080 字节)。
        const char *p = line;
        size_t len = strlen(line);
        while (len) {
            const size_t n = len < 256 ? len : 256;
            const esp_err_t e = httpd_resp_send_chunk(req, p, (int)n);
            if (e != ESP_OK) { *trunc = true; return e; }
            p += n;
            len -= n;
        }
        (*emitted)++;
    }
    return ESP_OK;
}

// 回复状态段(station/title/解码信息/剩余堆),写进 head 并返回长度。
static size_t radio_status_json(char *head, size_t cap)
{
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    size_t used = (size_t)snprintf(head, cap, "\"station\":\"");
    used += (size_t)radio_json_escape(head + used, cap - used,
                                      s.station[0] ? s.station : "");
    used += (size_t)snprintf(head + used, cap - used, "\",\"title\":\"");
    used += (size_t)radio_json_escape(head + used, cap - used, s.title);
    used += (size_t)snprintf(head + used, cap - used,
        "\",\"state\":%d,\"err\":%d,\"sample_rate\":%u,\"channels\":%u,"
        "\"bitrate\":%u,\"volume\":%u,\"heap_free\":%u}",
        (int)s.state, (int)s.err_code,
        (unsigned)s.sample_rate, (unsigned)s.channels,
        (unsigned)s.bitrate, (unsigned)s.volume,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return used;
}

// 统一回复:{"ok":..[,error][,"stations":[..流式..],stations_truncated]
//          [,"status":{..}]}。全部分块发送,堆再紧也发得出去。
static esp_err_t radio_stations_reply(httpd_req_t *req, bool ok, const char *err,
                                      bool want_list)
{
    char part[320];
    size_t used = (size_t)snprintf(part, sizeof(part), "{\"ok\":%s", ok ? "true" : "false");
    if (err) used += (size_t)snprintf(part + used, sizeof(part) - used,
                                      ",\"error\":\"%s\"", err);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send_chunk(req, part, (int)used);

    bool trunc = false;
    int emitted = 0;
    if (e == ESP_OK && want_list) {
        e = httpd_resp_send_chunk(req, ",\"stations\":[", HTTPD_RESP_USE_STRLEN);
        if (e == ESP_OK) e = radio_stations_chunks(req, &trunc, &emitted, 0, 0);
        if (e == ESP_OK) e = httpd_resp_send_chunk(req, "]", HTTPD_RESP_USE_STRLEN);
        if (e == ESP_OK && trunc) {
            e = httpd_resp_send_chunk(req, ",\"stations_truncated\":true",
                                      HTTPD_RESP_USE_STRLEN);
        }
    }
    if (e == ESP_OK) {
        used = (size_t)snprintf(part, sizeof(part), ",\"status\":{");
        used += radio_status_json(part + used, sizeof(part) - used);
        used += (size_t)snprintf(part + used, sizeof(part) - used, "}");
        e = httpd_resp_send_chunk(req, part, (int)used);
    }
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

// 门户列表专用:只回清单的一段(from/limit,大清单分页拉取的关键——3473 台
// 全量 JSON 会拖垮 httpd)。{"ok":true,"total":N,"from":F[,"more":true]
// [,"stations_truncated":true],"stations":[...]}
static esp_err_t radio_list_reply(httpd_req_t *req, int from, int limit)
{
    char part[96];
    const int count = station_count();
    if (from < 0 || from > count) from = 0;
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send_chunk(req, "{\"ok\":true,\"stations\":[",
                                        HTTPD_RESP_USE_STRLEN);
    bool trunc = false;
    int emitted = 0;
    if (e == ESP_OK) e = radio_stations_chunks(req, &trunc, &emitted, from, limit);
    // 即便中途出错也把收尾发完(socket 已死时这些调用无害地失败)。
    (void)httpd_resp_send_chunk(req, "]", HTTPD_RESP_USE_STRLEN);
    if (e == ESP_OK) {
        int used = snprintf(part, sizeof(part), ",\"total\":%d,\"from\":%d",
                            count, from);
        if (emitted >= limit && from + emitted < count) {
            used += snprintf(part + used, sizeof(part) - used, ",\"more\":true");
        }
        if (trunc) used += snprintf(part + used, sizeof(part) - used,
                                    ",\"stations_truncated\":true");
        e = httpd_resp_send_chunk(req, part, used);
    }
    (void)httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    (void)httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

static esp_err_t radio_api_handler(httpd_req_t *req)
{
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;

    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    bool ok = true;
    const char *err = NULL;

    if (!cJSON_IsString(op)) {
        ok = false; err = "缺少 op";
    } else if (strcmp(op->valuestring, "list") == 0) {
        // 门户列表:可带 from/count 分页(大清单 10 条一页);不带则全量封顶回显。
        const cJSON *fj = cJSON_GetObjectItemCaseSensitive(root, "from");
        const cJSON *cj = cJSON_GetObjectItemCaseSensitive(root, "count");
        const int from = cJSON_IsNumber(fj) ? fj->valueint : 0;
        const int pcnt = cJSON_IsNumber(cj) ? cj->valueint : 0;
        cJSON_Delete(root);
        return radio_list_reply(req, from, pcnt);
    } else if (strcmp(op->valuestring, "add") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
        bool want_list = false;
        if (!cJSON_IsString(n) || !cJSON_IsString(u)) { ok = false; err = "缺少 name 或 url"; }
        else if (radio_store_add(n->valuestring, u->valuestring)) {
            // 已落盘(逐条 NVS),屏幕下一轮 500ms 轮询自然刷新。
            want_list = true;                        // 门户要重渲染列表
        } else {
            ok = false; err = "地址不合法或自定义清单已满(上限 100)";
        }
        cJSON_Delete(root);
        return radio_stations_reply(req, ok, err, want_list);
    } else if (strcmp(op->valuestring, "del") == 0) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "index");
        radio_station_t st;
        bool want_list = false;
        if (!cJSON_IsNumber(i)) { ok = false; err = "缺少 index"; }
        else if (i->valueint < 0 || i->valueint >= station_count()) { ok = false; err = "下标越界"; }
        else if (!radio_store_get((int)i->valueint, &st)) { ok = false; err = "下标越界"; }
        else if ((int)i->valueint < radio_store_catalog_count()) {
            // 台目是固件 rodata,删掉就再也回不来(要改代码),不如明说。
            ok = false; err = "内置台目不能删除";
        } else if (radio_store_remove((int)i->valueint)) {
            if (s_cur_idx == (int)i->valueint) s_cur_idx = -1;
            want_list = true;                        // 门户要重渲染列表
        } else { ok = false; err = "删除失败"; }
        cJSON_Delete(root);
        return radio_stations_reply(req, ok, err, want_list);
    } else if (strcmp(op->valuestring, "play") == 0) {
        // 电台本来就在这个页面上管理,顺手也能开播/停播:调试时不用守在机器前按键。
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "index");
        radio_station_t st;
        if (!cJSON_IsNumber(i)) { ok = false; err = "缺少 index"; }
        else if (i->valueint < 0 || i->valueint >= station_count()) { ok = false; err = "下标越界"; }
        else if (!radio_store_get((int)i->valueint, &st)) { ok = false; err = "下标越界"; }
        else {
            radio_play(st.name, st.url);
            s_cur_idx = (int)i->valueint;
        }
    } else if (strcmp(op->valuestring, "stop") == 0) {
        radio_stop();
    } else if (strcmp(op->valuestring, "state") == 0) {
        // 纯查询,不改任何东西。
    } else {
        ok = false; err = "未知 op";
    }

    cJSON_Delete(root);
    // play/stop/state 的回复不带清单(播放期间堆见底,整表 JSON 会挤死 httpd)。
    return radio_stations_reply(req, ok, err, false);
}

bool radio_pages_portal_register(void *httpd)
{
    httpd_handle_t h = (httpd_handle_t)httpd;
    static const httpd_uri_t uri = {
        .uri = "/api/radio", .method = HTTP_POST, .handler = radio_api_handler,
    };
    return httpd_register_uri_handler(h, &uri) == ESP_OK;
}
