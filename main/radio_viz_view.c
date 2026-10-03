// main/radio_viz_view.c —— 见 radio_viz_view.h。
#include "radio_viz_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "radio_viz.h"

// ---- 版面常量(240x320) -----------------------------------------------------
#define MARGIN_X     14
#define BAR_TOP      34      // 顶栏 + 分隔线的高度
#define TOP_Y        3

#define CH_Y         42      // CH 03 / 04
#define STATION_Y    64      // 台名(大字)
#define SUBTITLE_Y   94      // 码率 / 采样率

#define PANEL_X      10
#define PANEL_W      220

#define SCALE_Y      118
#define SCALE_H      78      // 频率刻度面板
#define SCALE_PAD    6

#define VIZ_Y        202     // 频谱面板
#define VIZ_H        62
#define VIZ_PAD      6
#define VIZ_BOT      8
#define VIZ_TOP      8

#define STATUS_Y     262
#define HINT_Y       280

#define BAR_GAP      1
#define MIN_BAR_H    4       // 再矮也留 4px,一排都是小圆头

// ---- 配色 ------------------------------------------------------------------
#define C_BG         0x080D14      // 整屏底色
#define C_TOPLINE    0x1B2534      // 顶栏分隔线
#define C_YELLOW     0xFFC531      // 应用名 / CH / 峰值:黄
#define C_TITLE      0xF2F6FA      // 台名:近白
#define C_DIM        0x7A8899      // 副标题 / 电量
#define C_TEXT       0xDCE6F2      // 刻度数字
#define C_SCALE_BG   0x16233C      // 刻度面板底
#define C_VIZ_BG     0x16202E      // 频谱面板底
#define C_OK         0x35C26B      // 播放中
#define C_BAD        0xE5484D      // 出错
#define C_HINT       0x5A6B7D

// 柱色随高度(0..255)从青绿走到亮黄 —— 只有真正突出的频段才会变黄。
//
// 色相不是线性走的:用 gamma=1.6 把大部分柱子压在青绿区,只有最高的
// 20% 才推到黄。线性映射的话中等响的柱子就已经是纯绿了,看着像
// 绿→黄两段,少了参考图里那种"一片薄荷色里挑出几根黄的"层次。
//
// 注意 s/v 是**百分比 0-100**,lv_color_hsv_to_rgb 内部自己乘 255/100。
// 当成 0-255 传会被 uint8_t 截断(231→77、218→37),整屏变成暗褐色,
// 而且症状很隐蔽:条子照样画得出来,只是颜色全不对(踩过一次)。
static lv_color_t bar_color(uint8_t lv255)
{
    const int t = lv255;
    // gamma 必须是 0..100。早先用整数近似 (t*t*41)/(255*52) 想写
    // (t/255)^1.6*100,结果 t=255 时算出 201,色相 172-249 变成负数,
    // 一转 uint8_t 回绕成 179 —— 那一屏的柱子直接变成了蓝青色。
    const int g = (int)(powf((float)t / 255.0f, 1.6f) * 100.0f + 0.5f);
    const uint8_t h = (uint8_t)(172 - g * 124 / 100);   // 色相 172°(青)→ 48°(黄)
    const uint8_t s = (uint8_t)(65 + t * 23 / 255);      // 饱和度 65% → 88%
    const uint8_t v = (uint8_t)(55 + t * 45 / 255);      // 明度   55% → 100%
    return lv_color_hsv_to_rgb(h, s, v);
}

// 频段的中心频率排成一条刻度尺,标注用 "55" / "160" / "1.3k" 这样的短写法。
static void hz_label(char *buf, size_t n, float hz)
{
    if (hz < 1000.0f) snprintf(buf, n, "%d", (int)(hz + 0.5f));
    else if (hz < 10000.0f) snprintf(buf, n, "%.1fk", hz / 1000.0f);
    else snprintf(buf, n, "%dk", (int)(hz / 1000.0f + 0.5f));
}

static lv_obj_t *plain_obj(lv_obj_t *parent, int32_t w, int32_t h,
                           int32_t x, int32_t y, uint32_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    // bg_opa 必须显式设:过了 lv_obj_remove_style_all() 主题给的默认
    // 背景不透明度已被清掉,不设就是全透明 —— 设了颜色也看不见。
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (radius) lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *flat_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color,
                            int32_t w, int32_t x, int32_t y, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, text);
    return l;
}

// 多行标签。LV_LABEL_LONG_DOT 是**单行**模式:超宽就整行缩成 "...",
// 换行符根本不生效 —— 要两行必须用 WRAP。
static lv_obj_t *wrap_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color,
                            int32_t w, int32_t h, int32_t x, int32_t y, const char *text)
{
    lv_obj_t *l = flat_label(parent, f, color, w, x, y, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(l, h);
    return l;
}

lv_obj_t *radio_viz_view_create(lv_obj_t *parent,
                                const lv_font_t *font16, const lv_font_t *font24)
{
    radio_viz_view_t *v = (radio_viz_view_t *)lv_calloc(1, sizeof(*v));
    if (!v) return NULL;

    v->w = lv_obj_get_width(parent)  > 0 ? lv_obj_get_width(parent)  : 240;
    v->h = lv_obj_get_height(parent) > 0 ? lv_obj_get_height(parent) : 320;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, v->w, v->h);
    lv_obj_set_pos(v->root, 0, 0);
    lv_obj_set_style_bg_color(v->root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(v->root, LV_SCROLLBAR_MODE_OFF);

    // ---- 顶栏 ----
    v->app_name = flat_label(v->root, font16, C_YELLOW, 86, MARGIN_X, 11, "RADIO");
    v->clock    = flat_label(v->root, font24, C_TITLE, 66, 100, TOP_Y, "--:--");
    // 信号格:4 根由矮到高的小竖条,底边对齐。用控件画而不是字符 ——
    // 方块/柱状字符不一定在字库里,缺字会渲染成占位方块。
    for (int i = 0; i < 4; i++) {
        const int hgt = 4 + i * 2;
        v->signal[i] = plain_obj(v->root, 3, hgt, 170 + i * 4, 21 - hgt, C_OK, 1);
    }
    // 宽度给到 42:"88%" 在 16px 下约 34px,给窄了会把 "%" 挤到第二行。
    v->battery  = flat_label(v->root, font16, C_DIM, 42, 188, 11, "--");
    v->divider  = plain_obj(v->root, v->w, 1, 0, BAR_TOP, C_TOPLINE, 0);

    // ---- CH / 台名 / 副标题 ----
    v->channel  = flat_label(v->root, font16, C_YELLOW, 120, MARGIN_X, CH_Y, "CH 00 / 00");
    v->station  = flat_label(v->root, font24, C_TITLE, v->w - MARGIN_X * 2,
                             MARGIN_X, STATION_Y, "—");
    v->title = flat_label(v->root, font16, C_DIM, v->w - MARGIN_X * 2,
                             MARGIN_X, SUBTITLE_Y, "");

    // ---- 频率刻度面板 ----
    v->scale = plain_obj(v->root, PANEL_W, SCALE_H, PANEL_X, SCALE_Y, C_SCALE_BG, 8);
    lv_obj_set_scrollbar_mode(v->scale, LV_SCROLLBAR_MODE_OFF);

    v->scale_w = PANEL_W - SCALE_PAD * 2;
    v->scale_x = PANEL_X + SCALE_PAD;

    // 刻度数字横着铺开。用 flex 让 LVGL 自己算位置 —— 绝对定位需要知道
    // 标签宽度,而 LVGL 9 在布局算完之前读回宽度是 0,会全部叠在左边。
    {
        lv_obj_t *row = lv_obj_create(v->scale);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, v->scale_w, 18);
        lv_obj_set_pos(row, v->scale_x - PANEL_X, 6);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        for (int i = 0; i < RADIO_VIZ_TICKS; i++) {
            char buf[12];
            // 刻度 i 落在对数轴的 i/(TICKS-1) 处,和频段划分同一把尺子。
            const float hz = radio_viz_band_hz(
                (int)((float)i * (RADIO_VIZ_BANDS - 1) / (RADIO_VIZ_TICKS - 1) + 0.5f));
            hz_label(buf, sizeof(buf), hz);
            lv_obj_t *t = lv_label_create(row);
            lv_obj_set_style_text_font(t, font16, 0);
            lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
            lv_label_set_text(t, buf);
        }
    }

    // 刻度短线(和数字同一套 flex,视觉上对齐)
    {
        lv_obj_t *row = lv_obj_create(v->scale);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, v->scale_w, 7);
        lv_obj_set_pos(row, v->scale_x - PANEL_X, 26);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        for (int i = 0; i < RADIO_VIZ_TICKS; i++) {
            (void)plain_obj(row, 1, 7, 0, 0, 0x3E5A78, 0);
        }
    }

    // 黄色峰值指针:跟着当前最响的频段左右移动
    v->mark = plain_obj(v->scale, 3, 16, 0, 22, C_YELLOW, 1);
    v->peak = flat_label(v->scale, font16, C_YELLOW, v->scale_w, SCALE_PAD,
                         50, "峰值 —");

    // ---- 频谱面板 ----
    v->panel = plain_obj(v->root, PANEL_W, VIZ_H, PANEL_X, VIZ_Y, C_VIZ_BG, 8);
    lv_obj_set_scrollbar_mode(v->panel, LV_SCROLLBAR_MODE_OFF);

    v->baseline = VIZ_Y + VIZ_H - VIZ_BOT;
    v->max_h    = VIZ_H - VIZ_TOP - VIZ_BOT;
    {
        const int32_t usable = PANEL_W - VIZ_PAD * 2;
        v->slot  = usable / RADIO_VIZ_BAR_N;
        v->bar_w = v->slot - BAR_GAP;
        v->x0    = PANEL_X + VIZ_PAD + (usable - v->slot * RADIO_VIZ_BAR_N) / 2;
    }

    for (int k = 0; k < RADIO_VIZ_BAR_N; k++) {
        v->bar[k] = plain_obj(v->root, v->bar_w, MIN_BAR_H,
                              v->x0 + k * v->slot, v->baseline - MIN_BAR_H,
                              0x1F8C86, v->bar_w / 2);   // 胶囊形柱顶
    }

    // ---- 状态 / 提示 ----
    v->status = flat_label(v->root, font16, C_OK, v->w - MARGIN_X * 2,
                           MARGIN_X, STATUS_Y, "");
    v->hint   = wrap_label(v->root, font16, C_HINT, v->w - MARGIN_X * 2, 38,
                           MARGIN_X, HINT_Y, "上下切台，长按OK选台，短按OK暂停");

    lv_obj_set_user_data(v->root, v);
    return v->root;
}

void radio_viz_view_update(radio_viz_view_t *v, const uint8_t *bands, uint8_t level,
                           uint8_t volume, const radio_viz_chrome_t *chrome)
{
    if (!v || !v->root) return;

    int peak_i = 0;
    uint8_t peak_v = 0;
    for (int j = 0; j < RADIO_VIZ_BAR_N; j++) {
        // 一根柱子对应一个真实频段(BAR_N == RADIO_VIZ_BANDS 时 u 恒等于 j)。
        // 保留这层线性插值:以后若把 BAR_N 调回 2x,偶数下标落在真实频段上、
        // 奇数是两侧的线性中间值,天然不会超过邻段,也就不会造出假峰值。
        uint8_t lv = 0;
        if (bands) {
            const int32_t u = (int32_t)j * (RADIO_VIZ_BANDS - 1) / (RADIO_VIZ_BAR_N - 1);
            const int i0 = u;
            const int i1 = (i0 + 1 < RADIO_VIZ_BANDS) ? i0 + 1 : i0;
            const int frac = u - i0;
            lv = (uint8_t)(bands[i0] + ((int)bands[i1] - (int)bands[i0]) * frac / 255);
        }
        if (lv > peak_v) { peak_v = lv; peak_i = j; }

        int32_t hgt = (int32_t)((int)lv * v->max_h / 255);
        if (hgt < MIN_BAR_H) hgt = MIN_BAR_H;
        if (hgt > v->max_h) hgt = v->max_h;

        // 宽度用创建时算好的 v->bar_w,不要 lv_obj_get_width() 读回:
        // LVGL 9 在布局计算前读回是 0,会把条子宽度清成 0 直接消失。
        lv_obj_set_size(v->bar[j], v->bar_w, hgt);
        lv_obj_set_pos(v->bar[j], v->x0 + j * v->slot, v->baseline - hgt);
        lv_obj_set_style_bg_color(v->bar[j], bar_color(lv), 0);
    }

    // 频谱底板随总电平微微发亮,音乐越大越"热"
    lv_obj_set_style_bg_color(v->panel,
        lv_color_make((uint8_t)(22 + level / 20), (uint8_t)(32 + level / 16),
                      (uint8_t)(46 + level / 10)), 0);

    // 峰值指针:停在最响的那根柱子上;没声音就藏起来。
    // show_peak=false 时整个指针+读数永久隐藏(假频谱不占用频率刻度)。
    if (!v->show_peak) {
        lv_obj_set_hidden(v->mark, true);
        lv_label_set_text(v->peak, "");
    } else if (peak_v > 24) {
        const int32_t cx = v->scale_x + (peak_i + 0.5) * v->scale_w / RADIO_VIZ_BAR_N;
        lv_obj_set_pos(v->mark, cx - v->scale_x - 1, 22);
        char hz[12], buf[24];
        hz_label(hz, sizeof(hz), radio_viz_band_hz(peak_i * RADIO_VIZ_BANDS / RADIO_VIZ_BAR_N));
        snprintf(buf, sizeof(buf), "峰值 %s Hz", hz);
        lv_label_set_text(v->peak, buf);
        lv_obj_set_style_text_color(v->peak, lv_color_hex(C_YELLOW), 0);
        lv_obj_set_hidden(v->mark, false);
    } else {
        lv_label_set_text(v->peak, "峰值 —");
        lv_obj_set_style_text_color(v->peak, lv_color_hex(0x4A5563), 0);
        lv_obj_set_hidden(v->mark, true);
    }

    if (chrome) {
        if (chrome->app_name) lv_label_set_text(v->app_name, chrome->app_name);
        if (chrome->clock)    lv_label_set_text(v->clock, chrome->clock);
        if (chrome->signal_bars >= 0) {
            for (int i = 0; i < 4; i++) {
                lv_obj_set_hidden(v->signal[i], i >= chrome->signal_bars);
            }
        }
        if (chrome->battery)  lv_label_set_text(v->battery, chrome->battery);
        if (chrome->channel)  lv_label_set_text(v->channel, chrome->channel);
        if (chrome->station)  lv_label_set_text(v->station, chrome->station);
        if (chrome->title)    lv_label_set_text(v->title, chrome->title);
        if (chrome->status) {
            lv_label_set_text(v->status, chrome->status);
            lv_obj_set_style_text_color(v->status,
                lv_color_hex(chrome->status_bad ? C_BAD : C_OK), 0);
        }
    }

    // 音量不再单画一条:顶栏 + 两块面板已经把 320px 排满,音量并进
    // 副标题(音量条宽度就是副标题后面那一小段)会更挤,不如直接显示数值。
    (void)volume;
}
