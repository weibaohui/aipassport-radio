// main/radio_viz.h —— 音频频谱分析(纯逻辑,无 LVGL / 无 FreeRTOS / 无 BSP 依赖)。
//
// 单独拆出来有两个原因:
//   1. 它是纯计算,可以在主机上跑单元测试(见 tests/test_radio_viz.c);
//   2. 主机模拟器(tools/sim.c)要渲染显示效果,需要直接喂它合成音频。
//
// 全部热路径定点整数:C3 无 FPU,逐采样/逐蝶形的软浮点调用(ROM 辅助函数)
// 曾经吃掉整个核——音频颤抖、界面冻住、看门狗刷屏。浮点只允许出现在
// 初始化(窗/旋转因子)与每帧一次的 17 段平滑里,量级可忽略。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一屏 16 段频谱。240px 宽时每段 15px,和琴谱/均衡器的观感一致。
#define RADIO_VIZ_BANDS 16
// 变换长度(必须是 2 的幂)。
#define RADIO_VIZ_FFT   512

typedef struct {
    float sample_rate;

    int16_t buf[RADIO_VIZ_FFT];  // 滑动窗口(原始采样)
    int16_t win[RADIO_VIZ_FFT];  // Q15 Hann 窗
    int32_t re[RADIO_VIZ_FFT];
    int32_t im[RADIO_VIZ_FFT];
    int16_t wcos[RADIO_VIZ_FFT / 2]; // Q15 旋转因子 cos 表
    int16_t wsin[RADIO_VIZ_FFT / 2]; // Q15 旋转因子 sin 表
    int fill;
    uint32_t windows;             // 已攒满的窗口数(FFT 降频节流)

    int bin_lo[RADIO_VIZ_BANDS];
    int bin_hi[RADIO_VIZ_BANDS];

    float band[RADIO_VIZ_BANDS];  // 0..1 归一化后的频带值
    float disp[RADIO_VIZ_BANDS];  // 平滑后的显示值
    float gain;                   // 跟随整体电平缓慢回落的全局增益(整数幅度域)

    int16_t peak_in;              // 本次累积的峰值(原始 int16 采样)
    float level;                  // 总电平,快起慢落
} radio_viz_t;

// 初始化。sample_rate 取解码器上报的实际采样率(非法值会回落到 44.1kHz)。
void radio_viz_init(radio_viz_t *v, float sample_rate);

// 喂一段单声道 16bit PCM(调用方已完成立体声下混)。
void radio_viz_push(radio_viz_t *v, const int16_t *pcm, size_t bytes);

// 取当前平滑后的频谱,每段 0..255。out 长度须为 RADIO_VIZ_BANDS。
void radio_viz_render(radio_viz_t *v, uint8_t *out);

// 取总电平 0..255(用于整体呼吸/亮度)。
uint8_t radio_viz_level(const radio_viz_t *v);

// 第 k 段(0..RADIO_VIZ_BANDS-1)的中心频率,Hz。播放页用它画频率刻度尺
// 和峰值指针。k 越界返回 0。
float radio_viz_band_hz(int k);
