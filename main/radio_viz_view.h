// main/radio_viz_view.h —— 播放页的可视化(纯 LVGL,无 BSP / FreeRTOS / appfw 依赖)。
//
// 单独拆出来的原因和 radio_viz 一样:它只依赖 LVGL,主机模拟器
// (tools/sim.c)可以直接编译它来渲染出真实像素,不用 ESP-IDF。
//
// 版面照着一台真实网络收音机的样子排(自上而下):
//   顶栏  应用名 / 时间 / 信号 / 电量,下面一条分隔线
//   CH    频道序号 CH 03 / 04
//   台名  大字
//   副标题 码率 / 采样率
//   频谱面板  一排随声音起伏的彩条(2026-10-03 删除了频率刻度面板:假频谱
//            不做真实频率分析,65/175/473/1.3k 那排数字没有意义)
//   状态行  播放中 / 出错
//   提示行  按键说明
//
// 顶栏那些字符串由调用方填好再传进来(时间、电量、信号都要调 BSP/框架,
// 视图层保持不依赖它们,主机模拟器才能单独编译)。
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "appfw_bars.h"
#include "appfw_viz.h"     // APPFW_VIZ_BANDS

// 显示用的柱子数。分析仍然是 APPFW_VIZ_BANDS(16)段对数分频,这里把每段
// 线性插值成两根柱子 —— 16 根在 240px 宽的屏上太稀疏,看着像柱子阵而不是
// 频谱。插值值不会超过两侧的均值,所以不会凭空造出假峰值。
//
// 为什么是 16 而不是 32:每根柱子都是一个独立的 lv_obj + 本地样式,在
// ESP32-C3 上要吃掉可观的 DRAM。实测 32 根时 MP3 解码器初始化直接返回
// ESP_ERR_NO_MEM(日志里的 "Fail to init MP3 decoder ret 10"),音频只能
// 播出断续残帧,听着像"颤抖"。每台机器的 DRAM 都要留给解码器,UI 这边
// 让一步。16 根在 240px 宽下每根约 9px,视觉上仍然是密集柱阵。
// 若将来要加回 32 根,先在真机日志里确认解码器初始化成功再提交。
#define RADIO_VIZ_BAR_N APPFW_VIZ_BANDS

// 顶栏 + 副标题 + 状态行的文案。指针为 NULL 的项保持上一次不变。
typedef struct {
    const char *app_name;   // 顶栏左:应用名(黄色)
    const char *clock;      // 顶栏:时间 "04:22" / "--:--"
    int signal_bars;        // 顶栏:信号格 0..4(视图自己画,不用字库)
    const char *battery;    // 顶栏:电量 "88%" / "--"
    const char *channel;    // "CH 03 / 04"
    const char *station;    // 台名(大字)
    const char *title;      // 曲名(灰色小字)
    const char *status;     // 状态行,如 "正在播放"
    bool status_bad;        // 状态是错误(显示红色)
    const char *info0;      // 信息卡:格式(如 MP3/AAC/—)
    const char *info1;      // 信息卡:协议(http/https/HLS;NULL=不变)
    const char *info2;      // 信息卡:码率/采样率(如 "128 kbps · 44.1 kHz")
} radio_viz_chrome_t;

typedef struct {
    lv_obj_t *root;

    lv_obj_t *app_name;
    lv_obj_t *clock;
    lv_obj_t *signal[4];   // 信号格,由低到高 4 根
    lv_obj_t *battery;
    lv_obj_t *divider;

    lv_obj_t *channel;     // CH 03 / 04
    lv_obj_t *station;     // 台名(大字)
    lv_obj_t *title;       // 曲名(灰色小字)

    appfw_bars_t bars;     // 频谱柱阵(控件在框架:柱阵/色阶/辉光是机制)

    lv_obj_t *info0;       // 信息卡:格式
    lv_obj_t *info1;       // 信息卡:协议
    lv_obj_t *info2;       // 信息卡:码率/采样率
    lv_obj_t *status;      // 播放状态 / 错误
    lv_obj_t *hint;        // 底部按键提示

    // 屏幕尺寸。设备是 240x320,模拟器用同样尺寸,这样看到的就是真实效果。
    int32_t w;
    int32_t h;
} radio_viz_view_t;

// 创建播放页视图。font16/font24 由调用方提供(设备用应用自带字库,
// 模拟器也用同一套字库,保证渲染出来的就是设备上真实的样子)。
lv_obj_t *radio_viz_view_create(lv_obj_t *parent,
                                const lv_font_t *font16, const lv_font_t *font24);

// 更新。bands 长度 APPFW_VIZ_BANDS,level 0..255。
// chrome 为 NULL 表示顶栏文案不变;其中单项为 NULL 同样保持不变。
void radio_viz_view_update(radio_viz_view_t *v, const uint8_t *bands, uint8_t level,
                           uint8_t volume, const radio_viz_chrome_t *chrome);
