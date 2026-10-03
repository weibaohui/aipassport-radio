// tests/test_radio_viz.c —— 频谱分析的主机测试(纯逻辑,不需要设备)。
//
// 门禁按约定发现:tests/test_<stem>.c 会自动带上 main/<stem>.c。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio_viz.h"

static int fails;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("  FAIL: %s\n", what);
        fails++;
    }
}

// 生成 n 个采样点的单音(幅度 amp、频率 hz)。
static void gen_tone(int16_t *pcm, size_t n, float hz, float amp, float fs)
{
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)(amp * 32767.0f * sinf(2.0f * (float)M_PI * hz * (float)i / fs));
    }
}

// 频谱重心(哪个频带能量最高),用来判断"高低"是否被正确分辨。
static int dominant_band(const uint8_t *lv)
{
    int best = 0;
    for (int k = 1; k < RADIO_VIZ_BANDS; k++) {
        if (lv[k] > lv[best]) best = k;
    }
    return best;
}

int main(void)
{
    const float fs = 44100.0f;
    const size_t N = 2048;
    int16_t *pcm = (int16_t *)malloc(N * sizeof(int16_t));
    uint8_t lv[RADIO_VIZ_BANDS];
    radio_viz_t v;

    // --- 低音应该落在低段 ---
    radio_viz_init(&v, fs);
    for (int rep = 0; rep < 40; rep++) {          // 喂足够多轮,让自适应峰值收敛
        gen_tone(pcm, N, 110.0f, 0.5f, fs);
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    const int low = dominant_band(lv);
    printf("110Hz → 主导频带 %d, 电平 %u\n", low, radio_viz_level(&v));
    check(low <= 2, "110Hz 应落在最低的几段");
    check(radio_viz_level(&v) > 100, "有声音时总电平应明显");

    // --- 高音应该落在高段 ---
    radio_viz_init(&v, fs);
    for (int rep = 0; rep < 40; rep++) {
        gen_tone(pcm, N, 6000.0f, 0.5f, fs);
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    const int high = dominant_band(lv);
    printf("6000Hz → 主导频带 %d\n", high);
    check(high >= RADIO_VIZ_BANDS - 3, "6000Hz 应落在最高的几段");
    check(high > low + 5, "高音的频带号必须明显高于低音");

    // --- 声音大小:同样的音,幅度大 → 电平高 ---
    uint8_t quiet_lvl, loud_lvl;
    radio_viz_init(&v, fs);
    for (int rep = 0; rep < 60; rep++) {
        gen_tone(pcm, N, 440.0f, 0.05f, fs);
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    quiet_lvl = radio_viz_level(&v);
    radio_viz_init(&v, fs);
    for (int rep = 0; rep < 60; rep++) {
        gen_tone(pcm, N, 440.0f, 0.9f, fs);
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    loud_lvl = radio_viz_level(&v);
    printf("同音 幅度0.05 → 电平 %u;幅度0.9 → 电平 %u\n", quiet_lvl, loud_lvl);
    check(loud_lvl > quiet_lvl + 60, "音量大小必须体现在总电平上");

    // --- 静音:电平应衰减到接近 0 ---
    radio_viz_init(&v, fs);
    for (int rep = 0; rep < 20; rep++) {
        gen_tone(pcm, N, 440.0f, 0.9f, fs);
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    memset(pcm, 0, N * sizeof(int16_t));
    for (int rep = 0; rep < 200; rep++) {         // 长时间静音
        radio_viz_push(&v, pcm, N * sizeof(int16_t));
        radio_viz_render(&v, lv);
    }
    printf("静音 200 轮后电平 %u\n", radio_viz_level(&v));
    check(radio_viz_level(&v) < 20, "静音后总电平必须降下来");
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) {
        check(lv[k] < 40, "静音后各频段都应基本落下去");
    }

    // --- 退化输入不应崩 ---
    radio_viz_init(&v, fs);
    radio_viz_push(&v, NULL, 0);
    radio_viz_push(&v, pcm, 1);                  // 奇数字节
    radio_viz_render(&v, lv);
    radio_viz_init(&v, 0.0f);                     // 非法采样率应回落到 44.1k
    gen_tone(pcm, N, 440.0f, 0.5f, 44100.0f);
    radio_viz_push(&v, pcm, N * sizeof(int16_t));
    radio_viz_render(&v, lv);
    check(1, "退化输入不崩溃");

    free(pcm);
    if (fails) {
        printf("radio_viz: %d 项失败\n", fails);
        return 1;
    }
    printf("radio_viz: 全部通过\n");
    return 0;
}
