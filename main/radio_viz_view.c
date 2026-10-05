// main/radio_viz_view.c —— 见 radio_viz_view.h。
#include "radio_viz_view.h"

#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include <string.h>

#include "appfw_viz.h"

// ---- 版面常量(240x320) -----------------------------------------------------
#define MARGIN_X     14
#define BAR_TOP      34      // 顶栏 + 分隔线的高度
#define TOP_Y        3

#define CH_Y         42      // CH 03 / 04
#define STATION_Y    64      // 台名(大字)
#define SUBTITLE_Y   94      // 码率 / 采样率

#define PANEL_X      10
#define PANEL_W      220

// 频率刻度面板已删(2026-10-03):假频谱不做真实频率分析,65/175/473/1.3k
// 的刻度没有意义。
// 高度压到 44px(2026-10-03 真机反馈):132px 的大柱阵每帧变化太扎眼,
// "刷新感很重";矮条的视觉扰动小,律动感还在。
#define VIZ_Y        198     // 频谱面板(加高:信息卡单行后中段全让给它)
#define VIZ_H        60
#define VIZ_PAD      6

#define INFO2_Y      172     // 信息卡:码率/采样率/声道(单行)
#define STATUS_Y     266
#define HINT_Y       280

// ---- 配色 ------------------------------------------------------------------
#define C_BG         0x080D14      // 整屏底色
#define C_TOPLINE    0x1B2534      // 顶栏分隔线
#define C_YELLOW     0xFFC531      // 应用名 / CH:黄
#define C_TITLE      0xF2F6FA      // 台名:近白
#define C_DIM        0x7A8899      // 副标题 / 电量
#define C_OK         0x35C26B      // 播放中
#define C_BAD        0xE5484D      // 出错
#define C_HINT       0x5A6B7D

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
    // LONG_DOT 会把 "..." 原地写进 label 文本缓冲(池内),曾疑似引发
    // text_len 被写花(0xFFFFFFFF)导致字体查表崩溃——改 CLIP,只裁不改。
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, text);
    return l;
}

// 多行标签。LV_LABEL_LONG_DOT 是**单行**模式:超宽就整行缩成 "...",
// 换行符根本不生效 —— 要两行必须用 WRAP。
static lv_obj_t *wrap_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color,
                            int32_t w, int32_t h, int32_t x, int32_t y, const char *text)
{
    lv_obj_t *l = flat_label(parent, f, color, w, x, y, text);
    // 多行必须 WRAP(CRASH 根因是 DOT 的原地改写;WRAP 只做布局,不改缓冲)。
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

    // ---- 频谱面板(已无频率刻度:假频谱不做真实频率分析) ----
    // 柱阵本体在框架(appfw_bars):柱数/最矮高度/内边距是应用的版面参数。
    appfw_bars_create(&v->bars, v->root, PANEL_X, VIZ_Y, PANEL_W, VIZ_H,
                      VIZ_PAD, 4, RADIO_VIZ_BAR_N);

    // ---- 信息卡(台名与频谱面板之间的空档,单行:码率/采样率/声道) ----
    lv_obj_t *kl = flat_label(v->root, font16, C_DIM, 60, MARGIN_X, INFO2_Y, "码率");
    (void)kl;
    v->info2 = flat_label(v->root, font16, C_TITLE, v->w - MARGIN_X * 2 - 60,
                          MARGIN_X + 60, INFO2_Y, "—");

    // ---- 状态 / 提示 ----
    v->status = flat_label(v->root, font16, C_OK, v->w - MARGIN_X * 2,
                           MARGIN_X, STATUS_Y, "");
    v->hint   = wrap_label(v->root, font16, C_HINT, v->w - MARGIN_X * 2, 38,
                           MARGIN_X, HINT_Y, "上下切台，短按OK暂停，长按OK选台");

    lv_obj_set_user_data(v->root, v);
    return v->root;
}

void radio_viz_view_update(radio_viz_view_t *v, const uint8_t *bands, uint8_t level,
                           uint8_t volume, const radio_viz_chrome_t *chrome)
{
    if (!v || !v->root) return;

    // 柱阵/底板辉光是框架控件的事;分析是 APPFW_VIZ_BANDS 段,柱数不同
    // 时控件内部线性插值(不造假峰)。
    appfw_bars_update(&v->bars, bands, bands ? APPFW_VIZ_BANDS : 0, level);

    if (chrome) {
        if (chrome->info2) lv_label_set_text(v->info2, chrome->info2);
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
