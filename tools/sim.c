// tools/sim.c —— 主机模拟器:在 PC 上渲染设备上会看到的画面,输出 PNG。
//
// 为什么需要它:ESP32 掌机没法截图,而这套 BSP(I2C + SPI 屏 + ADC 按键)
// 在主机上根本跑不起来。所以这里绕开 BSP,只用 LVGL + 应用自己的
// 「频谱可视化」模块(它不依赖 BSP),把同样的字库、同样的 240x320 尺寸
// 渲染成 PNG。看到的像素就是设备上的像素。
//
// 用法:见 tools/build_sim.sh。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "radio_viz.h"
#include "radio_viz_view.h"

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_24);

#define W 240
#define H 320

static uint16_t g_fb[W * H];   // RGB565,与设备帧缓冲同格式

/* 把渲染结果写成 PNG。用标准库的 zlib 之外的最简实现:直接存未压缩
 * (stored) 的 deflate 块,不需要任何外部依赖。 */
static void put32(FILE *f, uint32_t v)
{
    fputc((v >> 24) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

// 同上,但目标是内存缓冲(adler32 写进 zlib 流尾部时用)。
static void put32_mem(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void png_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    put32(f, (uint32_t)n);
    uint8_t *tmp = (uint8_t *)malloc(n + 4);
    memcpy(tmp, tag, 4);
    if (n) memcpy(tmp + 4, data, n);
    fwrite(tmp, 1, n + 4, f);
    put32(f, crc32_of(tmp, n + 4));
    free(tmp);
}

static void write_png(const char *path)
{
    /* RGB888 原始扫描行,每行前面加一个 0 过滤器字节 */
    const size_t stride = (size_t)W * 3 + 1;
    const size_t rawlen = stride * H;
    uint8_t *raw = (uint8_t *)malloc(rawlen);
    for (int y = 0; y < H; y++) {
        uint8_t *row = raw + stride * y;
        *row++ = 0;
        for (int x = 0; x < W; x++) {
            const uint16_t c = g_fb[y * W + x];
            /* RGB565 -> RGB888 */
            const uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
            *row++ = r; *row++ = g; *row++ = b;
        }
    }

    /* zlib:0x78 0x01 + stored deflate 块 + adler32。
     * 注意 stored 块的长度字段只有 16 位,单块最多 65535 字节,而一帧
     * 240*320*3 + 320 = 230720 字节,超了三倍多——必须切成多块,
     * 只写一块会得到解码失败的坏 PNG。 */
    const size_t nblocks = (rawlen + 65534) / 65535;
    const size_t zlen = 2 + nblocks * 5 + rawlen + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;

    for (size_t pos = 0; pos < rawlen; ) {
        size_t n = rawlen - pos;
        if (n > 65535) n = 65535;
        const uint16_t nlen = (uint16_t)n;
        const uint16_t nnlen = (uint16_t)~nlen;
        z[zi++] = (uint8_t)((pos + n >= rawlen) ? 0x01 : 0x00);  /* BFINAL, BTYPE=00 */
        z[zi++] = (uint8_t)(nlen & 0xFF);
        z[zi++] = (uint8_t)(nlen >> 8);
        z[zi++] = (uint8_t)(nnlen & 0xFF);
        z[zi++] = (uint8_t)(nnlen >> 8);
        memcpy(z + zi, raw + pos, n);
        zi += n;
        pos += n;
    }

    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawlen; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32_mem(z + zi, (b << 16) | a);
    zi += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "无法写 %s\n", path); return; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(W >> 24), (uint8_t)(W >> 16), (uint8_t)(W >> 8), (uint8_t)W,
                        (uint8_t)(H >> 24), (uint8_t)(H >> 16), (uint8_t)(H >> 8), (uint8_t)H,
                        8, 2, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    printf("  已输出 %s\n", path);
}

/* ---------------------------------------------------------------- LVGL 驱动 */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    /* px 就是 RGB565 原始字节,直接整行搬进自己的帧缓冲即可 ——
     * LVGL 9 没有 lv_color_from_u16,不必绕道 lv_color_t。 */
    const int w = lv_area_get_width(area);
    const uint16_t *src = (const uint16_t *)px;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(&g_fb[y * W + area->x1], src + (y - area->y1) * w, (size_t)w * sizeof(uint16_t));
    }
    lv_display_flush_ready(disp);
}

static void render(lv_display_t *disp, int frames)
{
    for (int i = 0; i < frames; i++) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
    (void)disp;
}

/* ---------------------------------------------------------------- 合成音频 */

// 简易 xorshift,用来给"鼓点"加一点随机,免得每次谱都一样。
static uint32_t g_rng = 0x2545F491u;
static float frnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFF) / 32768.0f - 1.0f;
}

// 一个"像那么回事"的混音:底鼓 + 贝斯 + 和弦 + 镲,各带包络。
// 纯音场景(sin)看不出律动,这个才是设备上实际的样子。
static float music(float t, float x)
{
    const float beat = t * 2.0f;                       // 120BPM
    const float ph   = beat - floorf(beat);            // 拍内相位 0..1
    const float bar  = floorf(beat) / 4.0f;

    // 底鼓:每拍一下,快速衰减
    const float kick_env = expf(-ph * 11.0f);
    const float kick = sinf(2 * M_PI * (58.0f + 42.0f * kick_env) * x) * kick_env * 0.85f;

    // 贝斯:跟着和弦走,每小节换一个根音
    const float roots[4] = { 55.0f, 73.4f, 49.0f, 65.4f };
    const float root = roots[(int)bar & 3];
    const float bass = (sinf(2 * M_PI * root * x) + 0.4f * sinf(2 * M_PI * root * 2 * x)) * 0.30f;

    // 和弦:三音,缓慢起伏
    const float swell = 0.55f + 0.45f * sinf(2 * M_PI * 0.13f * t);
    const float chord = (sinf(2 * M_PI * root * 4 * x)
                       + sinf(2 * M_PI * root * 6 * x)
                       + sinf(2 * M_PI * root * 8 * x)) * 0.11f * swell;

    // 镲:反拍,高频噪声
    const float hat_ph = ph - 0.5f;
    const float hat_env = (hat_ph > 0.0f && hat_ph < 0.25f) ? expf(-hat_ph * 26.0f) : 0.0f;
    const float hat = frnd() * hat_env * 0.20f;

    return kick + bass + chord + hat;
}

static void synth(int16_t *pcm, size_t n, float t, int kind)
{
    for (size_t i = 0; i < n; i++) {
        const float x = (float)i / 44100.0f;
        float s = 0.0f;
        switch (kind) {
        case 0:   /* 低音鼓点:55Hz + 110Hz */
            s = 0.5f * sinf(2 * M_PI * 55.0f * x)
              + 0.3f * sinf(2 * M_PI * 110.0f * x)
              + 0.2f * sinf(2 * M_PI * 220.0f * x);
            break;
        case 1:   /* 人声/中频:300-800Hz */
            s = 0.5f * sinf(2 * M_PI * 440.0f * x)
              + 0.25f * sinf(2 * M_PI * 880.0f * x)
              + 0.15f * sinf(2 * M_PI * 1320.0f * x);
            break;
        case 2:   /* 镲片/高频:6-10kHz */
            s = 0.4f * sinf(2 * M_PI * 6000.0f * x)
              + 0.3f * sinf(2 * M_PI * 9000.0f * x)
              + 0.2f * sinf(2 * M_PI * 11000.0f * x);
            break;
        case 3:   /* 安静 */
            s = 0.02f * sinf(2 * M_PI * 300.0f * x);
            break;
        default:  /* 真实混音 */
            s = music(t, x);
            break;
        }
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)(s * 26000.0f);
    }
}

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : ".";
    char path[512];

    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    static lv_color_t buf[W * 20];
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    /* fallback 的选择必须和 main/radio_pages.c 里的完全一致,
     * 否则 ASCII(电台名/曲名多为拉丁字符)画出来和设备不一样。 */
    lv_font_t f16 = app_font_16;
    f16.fallback = &lv_font_montserrat_14;
    lv_font_t f24 = app_font_24;
    f24.fallback = &lv_font_montserrat_20;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_size(scr, W, H);
    lv_obj_t *view = radio_viz_view_create(scr, &f16, &f24);
    radio_viz_view_t *v = (radio_viz_view_t *)lv_obj_get_user_data(view);

    static radio_viz_t viz;
    radio_viz_init(&viz, 44100.0f);

    static int16_t pcm[2048];
    const struct {
        const char *file; int kind; int shots;
        const char *station; const char *title;
    } cases[] = {
        { "sim-1-music-a.png",  4, 1, "KEXP 90.3 FM",   "The Kent 3 - Satellite" },
        { "sim-2-music-b.png",  4, 1, "KEXP 90.3 FM",   "The Kent 3 - Satellite" },
        { "sim-3-music-c.png",  4, 1, "KEXP 90.3 FM",   "The Kent 3 - Satellite" },
        { "sim-4-bass.png",     0, 1, "KEXP 90.3 FM",   "The Kent 3 - Satellite" },
        { "sim-5-vocal.png",    1, 1, "KEXP 90.3 FM",   "Japan - Alien" },
        { "sim-6-high.png",     2, 1, "Radio Paradise", "Al Green - Take Me To The River" },
        { "sim-7-quiet.png",    3, 1, "KEXP 90.3 FM",   "" },
    };
    // 混音三个场景从同一段音乐的不同位置取,用来展示"律动":
    // 同一时刻拍一张是看不出动不动的。
    const int music_step0[3] = { 0, 26, 54 };

    int cur_music = 0;
    uint8_t bands[RADIO_VIZ_BANDS];

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const int step0 = (cases[c].kind == 4) ? music_step0[cur_music++] : 0;

        /* radio_viz_render() 是**每帧**调用的平滑器(上升 0.45 / 下降 0.10),
         * 设备上每帧都调。这里必须照着来:灌完全部音频才调一次的话,
         * disp[] 和 level 会原地保留上一个场景 90% 的值,静音场景就降不下来。 */
        for (int step = step0; step < step0 + 40; step++) {
            synth(pcm, 2048, step * 0.0464f, cases[c].kind);
            radio_viz_push(&viz, pcm, 2048 * sizeof(int16_t));
            radio_viz_render(&viz, bands);
        }
        // 顶栏文案在真机上由 radio_pages.c 调 BSP/框架取,这里手填一样的值,
        // 保证渲染出来的版式和设备一致。
        static radio_viz_chrome_t ch;
        ch.app_name    = "RADIO";
        ch.clock       = "04:22";
        ch.signal_bars = 3;
        ch.battery     = "88%";
        ch.channel     = (c >= 5) ? "CH 06 / 08" : "CH 03 / 08";
        ch.station     = cases[c].station;
        ch.title       = cases[c].title[0] ? cases[c].title : "MP3 · 128 kbps · 44.1 kHz";
        ch.status      = (c == 6) ? "正在播放" : "正在播放";
        ch.status_bad  = false;
        radio_viz_view_update(v, bands, radio_viz_level(&viz), 55, &ch);
        render(disp, 10);
        snprintf(path, sizeof(path), "%s/%s", outdir, cases[c].file);
        write_png(path);

        printf("  %-18s 电平=%3u  频谱=", cases[c].file, radio_viz_level(&viz));
        for (int k = 0; k < RADIO_VIZ_BANDS; k++) printf("%3d ", bands[k]);
        printf("\n");
    }
    return 0;
}
