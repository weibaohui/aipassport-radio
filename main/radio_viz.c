// main/radio_viz.c —— 见 radio_viz.h。
#include "radio_viz.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// 频带边界:对数分布,从 55Hz 到 11kHz。音乐的能量与听感都集中在低频,
// 线性分频会让右边几段几乎不动。
#define F_LOW_HZ   55.0f
#define F_HIGH_HZ  11000.0f
// 动态范围下限(相对当前峰值多少 dB 就当 0)。音乐从底鼓到镲片天然差
// 20~30dB,线性映射的话右边几段永远是 0,屏幕上只剩左边一小撮颜色。
// 把 [FLOOR_DB, 0] 这段线性压到 0~255,整屏都用起来。
//
// 关键是**不按频段加权**:早先试过给高频段乘一个递增的"倾斜系数",
// 结果相邻频段之间 1dB 的差距被放大成 3dB,连 FFT 泄漏都能翻转主导
// 频带(110Hz 从第 2 段跑到第 3 段)。dB 压缩对所有频段一视同仁,
// "哪段更响"仍然是真实结果。
#define FLOOR_DB   45.0f

// Q15 乘法:(a*b)>>15,用 int64 承载积。C3 无 FPU,这是全部热路径的
// 基本运算 —— 一条 MUL + 一次移位,比软浮点调用省两个数量级。
#define Q15_MUL(a, b) ((int32_t)(((int64_t)(a) * (int32_t)(b)) >> 15))

void radio_viz_init(radio_viz_t *v, float sample_rate)
{
    memset(v, 0, sizeof(*v));
    v->sample_rate = (sample_rate > 8000.0f) ? sample_rate : 44100.0f;

    for (int n = 0; n < RADIO_VIZ_FFT; n++) {
        const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)n / (float)RADIO_VIZ_FFT);
        // 32767 而不是 32768:Q15 里 -32768 没有配对的正数,避免边缘溢出。
        v->win[n] = (int16_t)lrintf(w * 32767.0f);
    }
    for (int j = 0; j < RADIO_VIZ_FFT / 2; j++) {
        const float ang = -2.0f * (float)M_PI * (float)j / (float)RADIO_VIZ_FFT;
        v->wcos[j] = (int16_t)lrintf(cosf(ang) * 32767.0f);
        v->wsin[j] = (int16_t)lrintf(sinf(ang) * 32767.0f);
    }

    // 记录每一段覆盖哪些 bin,便于运行时按实际采样率重映射。
    const float ratio = logf(F_HIGH_HZ / F_LOW_HZ);
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) {
        const float f0 = F_LOW_HZ * expf(ratio * (float)k / (float)RADIO_VIZ_BANDS);
        const float f1 = F_LOW_HZ * expf(ratio * (float)(k + 1) / (float)RADIO_VIZ_BANDS);
        v->bin_lo[k] = (int)floorf(f0 * (float)RADIO_VIZ_FFT / v->sample_rate);
        v->bin_hi[k] = (int)ceilf(f1 * (float)RADIO_VIZ_FFT / v->sample_rate);
        if (v->bin_lo[k] < 1) v->bin_lo[k] = 1;                 // 跳过 DC
        if (v->bin_hi[k] <= v->bin_lo[k]) v->bin_hi[k] = v->bin_lo[k] + 1;
        if (v->bin_hi[k] > RADIO_VIZ_FFT / 2) v->bin_hi[k] = RADIO_VIZ_FFT / 2;
    }
}

// 原地迭代基-2 FFT,全部定点整数。re/im 长度均为 RADIO_VIZ_FFT。
//
// 每级蝶形右移 1 位(scale >>1):既防溢出(积 ≤2^30),又让数值在 9 级里
// 保持 Q15 量级。代价是结果整体缩小 512 倍 —— 对所有 bin 一致,归一化后
// 形状不变。
static void fft_fixed(radio_viz_t *v)
{
    const int N = RADIO_VIZ_FFT;

    // 位反转置换
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const int32_t tr = v->re[i]; v->re[i] = v->re[j]; v->re[j] = tr;
            const int32_t ti = v->im[i]; v->im[i] = v->im[j]; v->im[j] = ti;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int stride = N / len;
        const int half = len / 2;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; k++) {
                const int tw = k * stride;
                const int32_t vr = v->re[i + k],        vi = v->im[i + k];
                const int32_t xr = v->re[i + k + half], xi = v->im[i + k + half];
                const int32_t tr = (Q15_MUL(xr, v->wcos[tw]) - Q15_MUL(xi, v->wsin[tw])) >> 1;
                const int32_t ti = (Q15_MUL(xr, v->wsin[tw]) + Q15_MUL(xi, v->wcos[tw])) >> 1;
                v->re[i + k]        = vr + tr;
                v->im[i + k]        = vi + ti;
                v->re[i + k + half] = vr - tr;
                v->im[i + k + half] = vi - ti;
            }
        }
    }
}

// 对滑动窗口做一次定点 FFT,按频带聚合出 band[]。
static void do_fft(radio_viz_t *v)
{
    for (int n = 0; n < RADIO_VIZ_FFT; n++) {
        v->re[n] = Q15_MUL((int32_t)v->buf[n], v->win[n]);
        v->im[n] = 0;
    }
    fft_fixed(v);

    // 每一段取其覆盖 bin 里的最大值(而不是求和):窄峰也能顶到满格,
    // 视觉上更像琴键被敲响,而不是被摊平。幅度用 |re|+|im| 整数近似
    // (单调、无 sqrt),相对大小与真幅度一致。
    int32_t maxb = 0;
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) {
        int32_t m = 0;
        for (int b = v->bin_lo[k]; b < v->bin_hi[k]; b++) {
            const int32_t mag = abs(v->re[b]) + abs(v->im[b]);
            if (mag > m) m = mag;
        }
        if (m > maxb) maxb = m;
        v->band[k] = (float)m;
    }

    // 0dB 参考:跟住最大频带并缓慢回落,安静时整体压低。
    if ((float)maxb > v->gain) v->gain = (float)maxb;
    else v->gain += ((float)maxb - v->gain) * 0.01f;

    // dB 域压缩:FLOOR_DB 以下当 0,0dB 当满格。每 FFT 只有 17 次 log10f。
    const float ref = (v->gain > 1e-6f) ? v->gain : 1e-6f;
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) {
        const float rel = v->band[k] / ref;            // 1.0 = 当前最响的段
        if (rel <= 1e-5f) { v->band[k] = 0.0f; continue; }
        float t = (10.0f * log10f(rel) + FLOOR_DB) / FLOOR_DB;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        v->band[k] = t;
    }
}

void radio_viz_push(radio_viz_t *v, const int16_t *pcm, size_t bytes)
{
    if (!pcm || bytes < 2) return;
    const size_t n = bytes / 2;

    for (size_t i = 0; i < n; i++) {
        const int16_t s = pcm[i];
        // 峰值用整数比较:别把逐采样 fabsf 软浮点调用请回来
        // (44.1kHz × 若干次/采样,曾占 ~20% CPU)。
        const int16_t a = (s >= 0) ? s : (int16_t)(-s);
        if (a > v->peak_in) v->peak_in = a;

        v->buf[v->fill++] = s;
        if (v->fill >= RADIO_VIZ_FFT) {
            v->fill = 0;
            v->windows++;
            // 1/4 降频:定点 FFT 每 4 窗算一次(~1% CPU)。仿真器指令级仿真
            // 比真机慢一个量级,这里每省一分 CPU 都是肉眼可见的流畅度。
            if (((v->windows - 1) & 3u) == 0u) do_fft(v);
        }
    }
}

void radio_viz_render(radio_viz_t *v, uint8_t *out)
{
    if (!out) return;
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) {
        const float norm = v->band[k];
        // 上升快、下降慢,视觉上更接近音乐律动。(每帧 17 段×2 次浮点、
        // 30ms 一次 —— 每秒约两千次浮点调用,量级可忽略。)
        const float d = v->disp[k];
        if (norm > d) v->disp[k] = d + (norm - d) * 0.45f;
        else v->disp[k] = d + (norm - d) * 0.10f;
        out[k] = (uint8_t)(v->disp[k] * 255.0f + 0.5f);
    }
    // 总电平:快起慢落,读数更接近听感。peak_in 是 int16 原始峰值,
    // 每帧只做一次除法转浮点。
    const float p = (float)v->peak_in / 32768.0f;
    if (p > v->level) v->level += (p - v->level) * 0.5f;
    else v->level += (p - v->level) * 0.06f;
    v->peak_in = 0;
}

uint8_t radio_viz_level(const radio_viz_t *v)
{
    return (uint8_t)(v->level * 255.0f + 0.5f);
}

float radio_viz_band_hz(int k)
{
    if (k < 0 || k >= RADIO_VIZ_BANDS) return 0.0f;
    const float ratio = logf(F_HIGH_HZ / F_LOW_HZ);
    return F_LOW_HZ * expf(ratio * ((float)k + 0.5f) / (float)RADIO_VIZ_BANDS);
}
