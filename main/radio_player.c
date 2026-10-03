// main/radio_player.c —— 见 radio_player.h。
//
// 为什么用裸 lwIP socket 而不用 esp_http_client:音频流是"没有结束符的响应体",
// 而 esp_http_client 面向的是有 Content-Length 的请求/响应,它会等着把整个
// body 收完。裸 socket 才做得干净。附带好处:整条路径不引入 TLS 常驻内存。
#include "radio_player.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "portmacro.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include "radio_streams.h"
#include "radio_frame.h"
#include "radio_icy.h"
#include "radio_viz.h"

static const char *TAG = "radio_player";

// MP3 一帧最多 1152 个采样;立体声 16bit = 4608 字节,取 5120(帧最大值向上留一档,
// 与参考实现一致)。再大就是白占内存:这台机器的堆要同时容纳环桶和解码器内部缓冲。
#define PCM_BUF_SIZE  5120
// 一次喂给解码器的字节数。喂料节奏由 bsp_audio_write 阻塞在 I2S 上按实时走,
// 这个值只决定每轮喂数据的粒度。
#define FEED_CHUNK    2048
// 抖动吸收环桶:网络 → 解码 之间必须有一层蓄水,否则欠载是必然的——
// 以前"读满 4KB 才解码":64kbps 下凑满一块要 ~500ms,而 codec 的 DMA
// 只有 87ms,每个周期必然欠载,听感就是固定周期的"颤抖"。
// 环桶按空闲堆从大到小试探分配;半桶作为开播预灌水位。上限压在 8KB:
// helix MP3 解码器首次解码要一次性惰性分配 ~20KB 连续堆,环桶给得太大,
// 解码器就开不起来(真机 ret 10 刷屏的根因)。64kbps 下 8KB 也有 1 秒余量。
#define RING_CAP_MAX  (8 * 1024)
#define RING_CAP_MIN  (4 * 1024)
// recv 超时:让任务能被切台请求及时打断,同时不至于频繁空转。
#define RX_TIMEOUT_MS 500
// 连续多少次读不到数据判定为断流(约 20 秒)。
#define RX_TIMEOUT_MAX 40
// 连续多少个解码失败后放弃重连。广播流偶有坏帧,单帧失败不该断流。
#define MP3_MAX_BAD_FRAMES 32
// 断流重连(播出过声音,台是活的)与开播失败的重试间隔分开:
// 前者短——用户对"换台到出声"的延迟最敏感;后者长一点,给服务端喘息。
// 连续开播失败到上限就停下来,把错误留在界面上,不再无限循环。
#define RECONNECT_DELAY_MS 1200
#define RETRY_DELAY_MS 2500
#define MAX_CONSECUTIVE_FAILURES 3

// 单生产者单消费者的字节环桶。整个收流循环都在收听任务里跑,
// 读写两侧同任务,不需要锁;w/r 用单调计数,占用 = w - r。
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t r, w;
} sbuf_t;

static size_t sbuf_used(const sbuf_t *q) { return q->w - q->r; }

static size_t sbuf_free(const sbuf_t *q) { return q->cap - 1 - sbuf_used(q); }

static size_t sbuf_read(sbuf_t *q, uint8_t *out, size_t len)
{
    const size_t used = sbuf_used(q);
    if (len > used) len = used;
    const size_t r = q->r % q->cap;
    const size_t tail = q->cap - r;
    const size_t first = len < tail ? len : tail;
    memcpy(out, q->buf + r, first);
    memcpy(out + first, q->buf, len - first);
    q->r += len;
    return len;
}

// recv 直接收进环桶的连续段(零额外拷贝)。返回 recv 原值;>0 时桶内
// 新数据的起点在 (q->w - n) % cap,长度 n 必然落在同一段内。
static int sbuf_recv_into(sbuf_t *q, int fd, int flags)
{
    size_t free_bytes = sbuf_free(q);
    if (free_bytes == 0) return 0;
    const size_t w = q->w % q->cap;
    size_t contig = q->cap - w;
    if (contig > free_bytes) contig = free_bytes;
    if (contig > FEED_CHUNK) contig = FEED_CHUNK;
    const int n = recv(fd, q->buf + w, contig, flags);
    if (n > 0) q->w += (size_t)n;
    return n;
}

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_player_snap_t s_snap;
// 最近一帧 PCM 峰值(0..255)。单字节 volatile,读写无撕裂;每帧 26ms 更新一次,
// UI 以 10Hz 读 —— 播放器侧唯一为显示付出的成本是下混循环里多一次整数比较。
static volatile uint8_t s_lvl;

// 切台请求:由 UI 线程写、任务读。用任务通知唤醒,避免忙等。
static TaskHandle_t s_task;
static volatile bool s_quit;          // 置位表示要放弃当前流
static char s_req_name[RADIO_URL_MAX > 32 ? 32 : RADIO_URL_MAX];
static char s_req_url[RADIO_URL_MAX];
static volatile bool s_req_pending;
static volatile uint8_t s_req_vol = 55;
static volatile bool s_paused;              // 暂停:保持连接,丢弃音频
static volatile uint8_t s_applied_vol;      // codec 当前实际套用的音量
// ICY 解复用器:只有收听任务访问(见 radio_icy.c,移植自 shulinbao/ai-passport-radio)。
// 元数据字节在这里被剥掉,解码器只吃纯音频;曲名从解出的元数据块里取。
static radio_icy_t s_icy;

static void set_snap(radio_state_t st, radio_err_t err)
{
    portENTER_CRITICAL(&s_lock);
    s_snap.state = st;
    s_snap.err_code = err;
    portEXIT_CRITICAL(&s_lock);
}

static void set_snap_stream_info(uint32_t rate, uint8_t ch, uint32_t kbps)
{
    portENTER_CRITICAL(&s_lock);
    s_snap.sample_rate = rate;
    s_snap.channels = ch;
    if (kbps) s_snap.bitrate = kbps;
    portEXIT_CRITICAL(&s_lock);
}

static void set_title(const char *t)
{
    portENTER_CRITICAL(&s_lock);
    if (!t || !t[0]) {
        s_snap.title[0] = '\0';
    } else {
        snprintf(s_snap.title, sizeof(s_snap.title), "%s", t);
    }
    portEXIT_CRITICAL(&s_lock);
}

// ---------------------------------------------------------------- HTTP 头解析

// 从一段 HTTP 响应头里取值(大小写不敏感)。命中返回 true。
static bool header_value(const char *headers, const char *name, char *out, size_t out_len)
{
    const size_t nlen = strlen(name);
    for (const char *p = headers; p && *p; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - p) > nlen) {
            for (size_t i = 0; i < nlen; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) break;
                if (i + 1 == nlen) {
                    if (p[nlen] == ':') {
                        const char *v = p + nlen + 1;
                        while (*v == ' ') v++;
                        size_t k = 0;
                        while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < out_len) {
                            out[k] = v[k]; k++;
                        }
                        out[k] = '\0';
                        return true;
                    }
                }
            }
        }
        p = eol + 2;
    }
    return false;
}

// ---------------------------------------------------------------- MP3 解码

// 用组件的 Simple Decoder,不用裸的 esp_mp3_dec_*。
//
// 原因:socket 给回来的块不对齐 MP3 帧边界,而裸解码器要求"输入就是完整帧",
// 半截帧会返回 DATA_LACK/FAIL,自己拼尾巴很容易在"丢字节重同步"和"卡住"之间
// 反复横跳——两种都表现为断断续续的杂音。Simple Decoder(esp_audio_simple_dec_*
// )就是为这种场景做的:use_frame_dec=false 时它自己解析并缓存跨块的半帧,
// 返回 OK 同时表示"解出了"或"已收进内部缓存",调用方不需要管帧边界。
//
// 仍然必须按 raw.consumed 推进:一次 process 未必吃完整个输入块。
typedef struct {
    esp_audio_simple_dec_handle_t dec;
    uint8_t *pcm;
    uint32_t pcm_cap;
    uint8_t vol;
    bool audio_started;
    int bad_frames;
    int write_fails;    // I2S 写连续失败计数:写不进去却继续喂,就是一把空转
    uint32_t rate;      // 解码器上报的采样率(用 get_info 拿,不是每次 decode 都带)
    uint8_t ch;         // 解码器上报的声道数
    int logged_head;    // 是否已打印过流首字节(诊断用)
    int logged_err;     // 已打印的错误码条数,避免刷屏
} mp3_ctx_t;

// codec 当前实际配置的采样率。0 = 还没开过。
// 单独记一份是因为 bsp_audio_set_format() 会 open 一次 codec,而不同电台的
// 采样率不一样(实测 44.1k 和 48k 混着),换台后必须重配,否则播放速率错位
// 听起来就是"颤抖"。0 这个初值保证第一次一定走 open 分支。
static uint32_t s_coded_rate;

// ---------------------------------------------------------------- 频谱快照

// FFT 分析只跑在音频线程这一侧:radio_viz_render() 是带平滑的(上升 0.45 /
// 下降 0.10),必须和解码同频调用才有"律动"。UI 线程只读下面这 17 个字节,
// 不碰 radio_viz_t 内部状态 —— 跨线程共享 buf/gain 会引出真正的竞态。
static radio_viz_t s_viz;
static uint32_t s_viz_rate;
static uint8_t  s_viz_raw[RADIO_VIZ_BANDS];
// 逐字节写、逐字节读。撕裂最多让某一帧的某一段跳一格,肉眼不可见;
// 为这 17 字节上互斥量反而不值得。
static volatile uint8_t s_viz_out[RADIO_VIZ_BANDS];
static volatile uint8_t s_viz_level;

// 频谱动画已整体停用(2026-10-03):FFT 即便定点也占 ~1/4 CPU,仿真器上更是
// 直接把解码挤到欠载。当前唯一目标是播放流畅——解码出的 PCM 只写 I2S。
// 要恢复频谱:把 RADIO_VIZ_ENABLED 置 1 即可(实现完好,主机测试仍在跑)。
#define RADIO_VIZ_ENABLED 0
static void viz_feed(const int16_t *pcm, size_t bytes, uint32_t rate)
{
    if (!RADIO_VIZ_ENABLED) return;
    if (rate == 0) return;
    if (s_viz_rate != rate) {          // 换台/换流可能换采样率,分频要重算
        radio_viz_init(&s_viz, (float)rate);
        s_viz_rate = rate;
    }
    radio_viz_push(&s_viz, pcm, bytes);
    radio_viz_render(&s_viz, s_viz_raw);
    for (int k = 0; k < RADIO_VIZ_BANDS; k++) s_viz_out[k] = s_viz_raw[k];
    s_viz_level = radio_viz_level(&s_viz);
}

void radio_player_viz_snapshot(uint8_t *out, uint8_t bands, uint8_t *level)
{
    if (out && bands) for (int k = 0; k < bands; k++) out[k] = s_viz_out[k];
    if (level) *level = s_viz_level;
}

// 把 len 字节的 MP3 喂进去,解出的 PCM 直接送去 I2S。返回 RADIO_ERR_NONE 表示继续。
static radio_err_t mp3_feed(mp3_ctx_t *c, const uint8_t *data, size_t len)
{
    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)data,
        .len = (uint32_t)len,
        .eos = false,
        .consumed = 0,
    };

    // 打印一次流首字节:出问题时这是判断"服务端给的是不是 MP3"最快的证据。
    if (!c->logged_head) {
        c->logged_head = 1;
        char hex[3 * 16 + 1];
        const size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", data[i]);
        ESP_LOGI(TAG, "音频流首字节: %s", hex);
    }

    while (raw.len > 0) {
        esp_audio_simple_dec_out_t out = {
            .buffer = c->pcm, .len = c->pcm_cap,
            .needed_size = 0, .decoded_size = 0,
        };

        const esp_audio_err_t dr = esp_audio_simple_dec_process(c->dec, &raw, &out);

        if (dr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // 一帧最多 1152 采样 ×2ch×2B = 4608 字节,8192 足够。真不够说明
            // 编码参数超常规,扩缓冲会进一步挤压这无 PSRAM 的堆,所以只记一次。
            if (c->logged_err < 2) {
                c->logged_err++;
                ESP_LOGW(TAG, "PCM 缓冲不足,解码器要 %u 字节", (unsigned)out.needed_size);
            }
            if (out.needed_size <= c->pcm_cap) return RADIO_ERR_NONE;  // 尺寸没变,再试也是死循环
            uint8_t *nb = realloc(c->pcm, out.needed_size);
            if (!nb) return RADIO_ERR_DECODE;
            c->pcm = nb;
            c->pcm_cap = out.needed_size;
            continue;   // 组件约定:扩容后重试同一次调用
        }

        if (dr != ESP_AUDIO_ERR_OK) {
            if (c->logged_err < 4) {
                c->logged_err++;
                ESP_LOGW(TAG, "解码返回 %d(第 %d 次),consumed=%u,剩余 %u",
                         (int)dr, c->bad_frames + 1,
                         (unsigned)raw.consumed, (unsigned)raw.len);
            }
            if (++c->bad_frames > MP3_MAX_BAD_FRAMES) {
                ESP_LOGE(TAG, "连续 %d 帧解码失败,放弃", c->bad_frames);
                return RADIO_ERR_DECODE;
            }
            if (raw.consumed == 0) { raw.len = 0; break; }   // 保证有进展
        }
        c->bad_frames = 0;

        if (out.decoded_size > 0) {
            esp_audio_simple_dec_info_t info = { 0 };
            if (esp_audio_simple_dec_get_info(c->dec, &info) == ESP_AUDIO_ERR_OK &&
                info.sample_rate > 0) {
                if (info.sample_rate != c->rate || info.channel != c->ch) {
                    c->rate = info.sample_rate;
                    c->ch = info.channel;
                    set_snap_stream_info(c->rate, c->ch, info.bitrate / 1000);
                }
            }

            if (c->rate == 0) goto advance;   // 还没拿到有效帧头,先不出声

            // codec 的采样率是**开一次就只能用一次**的。实测国内台是混的:
            // 中国之声 / CityFM 是 48kHz,其余几家是 44.1kHz。切换电台后
            // 沿用上一台配好的采样率,或者流中途变采样率,声音就会"颤抖"——
            // 数据按 48k 喂进按 44.1k 跑的 codec,播放速度就是错的。
            // 所以每次发现实际采样率和 codec 当前配置不一致,重新 open 一次。
            if (c->rate != s_coded_rate) {
                ESP_LOGI(TAG, "重配 codec: %u -> %u Hz", (unsigned)s_coded_rate,
                         (unsigned)c->rate);
                if (bsp_audio_set_format(c->rate, 16, 1) != ESP_OK) {
                    ESP_LOGE(TAG, "codec 格式设置失败 %uHz", (unsigned)c->rate);
                    return RADIO_ERR_DECODE;
                }
                s_coded_rate = c->rate;
                bsp_audio_set_volume(c->vol);
                c->audio_started = true;
                set_snap(RADIO_PLAYING, RADIO_ERR_NONE);
                ESP_LOGI(TAG, "开始播放: %uHz %uch", (unsigned)c->rate, c->ch);
            } else if (!c->audio_started) {
                if (bsp_audio_set_format(c->rate, 16, 1) != ESP_OK) {
                    ESP_LOGE(TAG, "codec 格式设置失败 %uHz", (unsigned)c->rate);
                    return RADIO_ERR_DECODE;
                }
                bsp_audio_set_volume(c->vol);
                c->audio_started = true;
                set_snap(RADIO_PLAYING, RADIO_ERR_NONE);
                ESP_LOGI(TAG, "开始播放: %uHz %uch", (unsigned)c->rate, c->ch);
            }

            // 假频谱的电平源:下混循环里顺带统计 |采样| 峰值(整数),发布到
            // s_lvl 供 UI 以 10Hz 取用。除这一次比较外零额外开销。
            int32_t peak = 0;
            if (c->ch >= 2) {
                const int16_t *in = (const int16_t *)out.buffer;
                int16_t *mono = (int16_t *)out.buffer;   // 原地下混,不额外占内存
                const int frames = (int)(out.decoded_size / 4);
                for (int i = 0; i < frames; i++) {
                    const int32_t m = ((int32_t)in[2 * i] + in[2 * i + 1]) / 2;
                    mono[i] = (int16_t)m;
                    const int32_t a = (m >= 0) ? m : -m;
                    if (a > peak) peak = a;
                }
                const bool wok = bsp_audio_write(mono, (size_t)frames * 2) == ESP_OK;
                viz_feed(mono, (size_t)frames * 2, c->rate);
                if (!wok && ++c->write_fails > 64) {
                    ESP_LOGE(TAG, "I2S 连续写入失败,放弃本台");
                    return RADIO_ERR_DECODE;
                }
                if (wok) c->write_fails = 0;
            } else {
                const int16_t *in = (const int16_t *)out.buffer;
                const size_t n = out.decoded_size / 2;
                for (size_t i = 0; i < n; i++) {
                    const int32_t a = (in[i] >= 0) ? in[i] : -in[i];
                    if (a > peak) peak = a;
                }
                const bool wok = bsp_audio_write(out.buffer, out.decoded_size) == ESP_OK;
                viz_feed((const int16_t *)out.buffer, out.decoded_size, c->rate);
                if (!wok && ++c->write_fails > 64) {
                    ESP_LOGE(TAG, "I2S 连续写入失败,放弃本台");
                    return RADIO_ERR_DECODE;
                }
                if (wok) c->write_fails = 0;
            }
            // 满幅 32768 → 512,>>6 后封顶 255:典型音乐(~1/3 幅度)落在 150
            // 上下,轻声也有 30-60,柱子有可感的起伏。
            s_lvl = (uint8_t)((peak >> 6) > 255 ? 255 : (peak >> 6));
        }

advance:
        if (raw.consumed == 0) break;   // 没前进就必须退出,否则死循环
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    return RADIO_ERR_NONE;
}

// ---------------------------------------------------------------- 收听一轮

// 尝试完整收听一个流,返回错误码。成功会一直播到 s_quit 或断流。
static radio_err_t run_one_stream(const char *url, bool *played)
{
    radio_err_t result = RADIO_ERR_NONE;
    esp_audio_simple_dec_handle_t dec = NULL;
    uint8_t *pcm = NULL;
    uint8_t *ring_buf = NULL;
    uint8_t *feed = NULL;
    int fd = -1;
    size_t ring_cap = 0;
    sbuf_t ring = { 0 };
    size_t prefill = 0;

    if (played) *played = false;

    char hostport[RADIO_HOST_MAX];
    if (!radio_url_hostport(url, hostport, sizeof(hostport))) return RADIO_ERR_URL;

    char host[RADIO_HOST_MAX];
    int port = 80;
    char *colon = strrchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        snprintf(host, sizeof(host), "%s", hostport);
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return RADIO_ERR_URL;
    } else {
        snprintf(host, sizeof(host), "%s", hostport);
    }

    set_snap(RADIO_CONNECTING, RADIO_ERR_NONE);
    set_title(NULL);

    // ---- 先开解码器、拿全部分配,再建连接 ----
    // (吸收 shulinbao/ai-passport-radio 的教训)连接建立后 lwIP/TCP 会话会把堆
    // 切碎,那时再开解码器就是 MEM_LACK;helix MP3 首次解码还要一次性惰性分配
    // 约 20KB 连续堆。顺序必须是:解码器 → 全部缓冲 → 连接。环桶上限也为此压
    // 到 8KB——它挤占的正是解码器要用的那块连续内存(真机 ret 10 刷屏的根因)。
    ESP_LOGI(TAG, "解码器分配前 heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,   // false = 由它解析并缓存跨块的半帧
    };
    if (esp_audio_simple_dec_open(&dcfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 解码器打开失败(heap=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return RADIO_ERR_DECODE;
    }
    pcm = malloc(PCM_BUF_SIZE);
    ring_cap = RING_CAP_MAX;
    while (ring_cap >= RING_CAP_MIN && !(ring_buf = malloc(ring_cap))) ring_cap /= 2;
    feed = malloc(FEED_CHUNK);
    if (!pcm || !ring_buf || !feed) {
        ESP_LOGE(TAG, "缓冲分配失败(剩余堆 %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        result = RADIO_ERR_DECODE;
        goto done;
    }
    ring.buf = ring_buf;
    ring.cap = ring_cap;
    prefill = ring_cap / 2;   // 半桶才开播:64kbps 下约 1 秒的网络抖动余量
    ESP_LOGI(TAG, "抖动缓冲 %uKB,预灌 %u 字节",
             (unsigned)(ring_cap / 1024), (unsigned)prefill);

    // ---- 连接 ----
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "域名解析失败: %s", host);
        result = RADIO_ERR_RESOLVE;
        goto done;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); res = NULL; result = RADIO_ERR_CONNECT; goto done; }

    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = RX_TIMEOUT_MS * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "连接失败 %s:%d", host, port);
        result = RADIO_ERR_CONNECT;
        goto done;
    }

    // 请求行 + 头。用 HTTP/1.0,服务端直接吐原始流,不做 chunked 编码。
    {
        const char *path = strchr(url + 7, '/');
        char req[256];
        const int rlen = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: AI-Passport-Radio/1.0\r\n"
            "Icy-MetaData: 1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n",
            path ? path : "/", host);
        if (send(fd, req, (size_t)rlen, 0) != rlen) { result = RADIO_ERR_CONNECT; goto done; }
    }

    // 读响应头(上限 1KB,足够放 status + 几个 ICY 头)
    {
        char hdr[1024];
        size_t hlen = 0;
        bool aborted = false;
        while (hlen < sizeof(hdr) - 1) {
            const int n = recv(fd, (uint8_t *)hdr + hlen, 1, 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                if (s_quit) { aborted = true; break; }
                result = RADIO_ERR_HTTP;
                goto done;
            }
            hlen += (size_t)n;
            hdr[hlen] = '\0';
            if (hlen >= 4 && strstr(hdr, "\r\n\r\n")) break;
        }
        if (aborted) goto done;   // s_quit:result 保持 NONE,任务层按切台处理
        if (!strstr(hdr, "\r\n\r\n") || strncmp(hdr, "HTTP/1.", 7) != 0) { result = RADIO_ERR_HTTP; goto done; }
        if (strstr(hdr, " 200") == NULL) {
            ESP_LOGW(TAG, "非 200 响应: %.40s", strchr(hdr, '\r'));
            result = RADIO_ERR_HTTP;
            goto done;
        }

        long metaint = 0;
        char val[64];
        if (header_value(hdr, "icy-metaint", val, sizeof(val))) metaint = atol(val);
        if (header_value(hdr, "icy-br", val, sizeof(val))) {
            set_snap_stream_info(0, 0, (uint32_t)atoi(val));
        }
        if (metaint < 0) metaint = 0;
        ESP_LOGI(TAG, "已连接 %s, icy-metaint=%ld, 任务栈余量 %u 字节",
                 host, metaint, (unsigned)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)));
        radio_icy_init(&s_icy, (size_t)metaint);
    }

    // 换台:清掉"codec 已配好"的记忆,让新台的首帧按**它自己的**采样率
    // 重新 open 一次。不清的话会沿用上一台的采样率,播出来速率错位。
    s_coded_rate = 0;

    // 主循环:每轮先"补桶"再"喂一块"。
    // 补桶:已开播后用非阻塞 recv 尽力灌到预灌水位(I2S 写阻塞期间网络也在
    // 进 socket,这里只是把它收进桶);桶空或还没开播才阻塞等——那时反正
    // 没声音,顺带做 20s 断流判定。喂一块:阻塞在 bsp_audio_write 上按实时走。
    // 稳态水位钉在预灌线附近,WiFi 抖动由桶深吸收,不再打穿 codec 的 DMA。
    mp3_ctx_t ctx = {
        .dec = dec, .pcm = pcm, .pcm_cap = PCM_BUF_SIZE,
        .vol = s_req_vol, .audio_started = false, .bad_frames = 0, .write_fails = 0,
        .rate = 0, .ch = 0, .logged_head = 0, .logged_err = 0,
    };
    bool eof = false;      // 对端已关闭:播完桶里剩余就收尾
    bool started = false;  // 已开播(过了预灌水位);之前不喂,先攒水
    bool synced = false;   // 已对齐到第一个完整帧头
    int timeouts = 0;
    uint8_t c_vol_applied = s_req_vol;   // 已套用到 codec 的音量
    size_t fed_total = 0;  // 喂给解码器的总字节数(迟迟解不动的兜底计数)
    for (;;) {
        if (s_quit) break;

        // ---- 暂停:保持连接、丢弃音频;恢复即从最新流继续(直播语义) ----
        if (s_paused) {
            s_lvl = 0;
            const int nd = sbuf_recv_into(&ring, fd, MSG_DONTWAIT);
            if (nd > 0) { ring.r = ring.w; timeouts = 0; continue; }   // 丢
            if (nd == 0) { eof = true; s_paused = false; set_snap(RADIO_STOPPED, RADIO_ERR_NONE); break; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { vTaskDelay(pdMS_TO_TICKS(80)); continue; }
            ESP_LOGW(TAG, "流读取失败 errno=%d", errno);
            result = RADIO_ERR_CONNECT;
            goto done;
        }

        // ---- 补桶 ----
        for (;;) {
            const size_t used = sbuf_used(&ring);
            const bool blocking = !started || used == 0;
            if (sbuf_free(&ring) == 0) break;          // 桶满:满不等于 EOF
            if (!blocking && used >= prefill) break;   // 已开播且到水位:去喂
            const int n = sbuf_recv_into(&ring, fd, blocking ? 0 : MSG_DONTWAIT);
            if (n > 0) {
                timeouts = 0;
                continue;
            }
            if (n == 0) { eof = true; break; }        // 对端关闭
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (blocking) {
                    if (++timeouts >= RX_TIMEOUT_MAX) { result = RADIO_ERR_TIMEOUT; goto done; }
                    continue;   // SO_RCVTIMEO 到期,重试(断流判定靠计数)
                }
                break;          // 非阻塞暂无数据:先喂桶里已有的
            }
            ESP_LOGW(TAG, "流读取失败 errno=%d", errno);
            result = RADIO_ERR_CONNECT;
            goto done;
        }
        if (s_quit) break;

        // ---- 喂一块 ----
        const size_t used = sbuf_used(&ring);
        if (used == 0) {
            if (eof) break;   // 对端已关且桶已清空:流自然结束
            continue;
        }
        const size_t n = sbuf_read(&ring, feed,
                                   used < FEED_CHUNK ? used : FEED_CHUNK);
        // ICY 解复用(吸收参考实现):元数据字节在这里剥掉,解码器只吃纯音频;
        // 曲名也从解出的元数据块里取,不再在音频流里暴力扫模式串。
        size_t audio_len = 0;
        for (size_t i = 0; i < n; i++) {
            if (radio_icy_consume(&s_icy, feed[i]) == RADIO_ICY_AUDIO) {
                feed[audio_len++] = feed[i];
            }
        }
        const char *title = radio_icy_title(&s_icy);
        if (title[0]) set_title(title);
        if (audio_len == 0) continue;

        // 流起始对齐:Icecast 从"正在播"的位置开始发,第一批字节常落在帧中间
        // (实测偏移十几到近百字节)。找到"连续两帧帧头都对得上"的位置,把半截
        // 帧丢掉再给解码器,避免开头爆音或 init 失败;找不到就原样喂,由解码器
        // 自己对齐——不在对齐上死等,否则一个不含合法帧头的流永远停在连接中。
        if (!synced) {
            size_t skip = 0;
            if (radio_frame_find(feed, audio_len, RADIO_FRAME_KIND_MP3, &skip)) {
                if (skip > 0) {
                    memmove(feed, feed + skip, audio_len - skip);
                    audio_len -= skip;
                    ESP_LOGI(TAG, "跳过开头 %u 字节的半截帧", (unsigned)skip);
                }
                synced = true;
            }
        }

        fed_total += audio_len;
        const radio_err_t de = mp3_feed(&ctx, feed, audio_len);
        // ★ 开播后必须把 started 置位:否则补桶永远按"未开播"阻塞到桶满才喂,
        // 播 250ms 断 ~750ms(64kbps),听感就是锯齿状卡顿——曾因拼接代码丢失
        // 这一行,模拟器与真机同症。
        if (ctx.audio_started) {
            started = true;
            if (played) *played = true;
        }
        // 音量套用的安全点:两次 I2S 写之间,不与 codec 设备层并发。
        if (ctx.audio_started && c_vol_applied != s_req_vol) {
            c_vol_applied = s_req_vol;
            ctx.vol = s_req_vol;
            bsp_audio_set_volume(s_req_vol);
        }
        if (de != RADIO_ERR_NONE) { result = de; goto done; }
        timeouts = 0;
    }

done:
    if (fd >= 0) close(fd);
    esp_audio_simple_dec_close(dec);
    free(feed);
    free(ring_buf);
    free(pcm);
    // 收台:让 codec 回到静音。ES8311 配好格式后会一直按当前采样率输出,
    // 不 mute 的话最后一帧的余音会在扬声器里拖出去,切台时"咔"一下。
    bsp_audio_set_volume(0);
    s_coded_rate = 0;
    return result;
}

// ---------------------------------------------------------------- 任务

static void player_task(void *arg)
{
    (void)arg;
    int failures = 0;                    // 同一台连续失败的次数(吸收参考实现:封顶止损)
    char last_url[RADIO_URL_MAX] = "";

    for (;;) {
        // 等切台请求。radio_play/radio_stop 会发任务通知把这里立刻叫醒;
        // 200ms 的超时只作兜底,通知万一丢了也不会卡住。
        for (;;) {
            portENTER_CRITICAL(&s_lock);
            const bool has = s_req_pending;
            portEXIT_CRITICAL(&s_lock);
            if (has || s_quit) break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        }
        s_quit = false;

        portENTER_CRITICAL(&s_lock);
        s_req_pending = false;
        char url[RADIO_URL_MAX];
        char name[sizeof(s_req_name)];
        snprintf(url, sizeof(url), "%s", s_req_url);
        snprintf(name, sizeof(name), "%s", s_req_name);
        s_snap.station[0] = '\0';
        snprintf(s_snap.station, sizeof(s_snap.station), "%s", name);
        s_snap.sample_rate = 0;
        s_snap.channels = 0;
        s_snap.title[0] = '\0';
        portEXIT_CRITICAL(&s_lock);

        if (url[0] == '\0') { set_snap(RADIO_STOPPED, RADIO_ERR_NONE); continue; }

        // 用户换了台:失败计数重新计。同一台才累计,连续失败到上限就停手,
        // 把原因留在界面上——无脑重试既费电,也会让状态一直在连接/失败间闪。
        if (strcmp(url, last_url) != 0) {
            snprintf(last_url, sizeof(last_url), "%s", url);
            failures = 0;
        }

        bool played = false;
        const radio_err_t err = run_one_stream(url, &played);
        if (s_quit) { s_quit = false; continue; }          // 用户切台,不算失败

        if (err == RADIO_ERR_NONE && played) {
            // 播出过声音说明这个台是活的,只是断流了:短暂等待后重连同一台。
            failures = 0;
            for (int i = 0; i < RECONNECT_DELAY_MS / 50 && !s_quit; i++) vTaskDelay(pdMS_TO_TICKS(50));
            portENTER_CRITICAL(&s_lock);
            s_req_pending = true;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }
        if (err == RADIO_ERR_NONE) { set_snap(RADIO_STOPPED, RADIO_ERR_NONE); continue; }

        set_snap(RADIO_ERROR, err);
        failures++;
        if (failures >= MAX_CONSECUTIVE_FAILURES) {
            ESP_LOGW(TAG, "连续 %d 次收听失败(err=%d),停止重试,请换个台", failures, (int)err);
            failures = 0;
            continue;   // 不再重新置位:回到等用户,错误原因已落到界面
        }
        ESP_LOGW(TAG, "收听失败 err=%d,%.1fs 后重试(%d/%d)",
                 (int)err, RETRY_DELAY_MS / 1000.0, failures, MAX_CONSECUTIVE_FAILURES);
        // 退避期间仍可被切台打断;必须把请求重新置位,否则外层会回到
        // "等用户操作"的空转里,明明日志写着"后重试",实际要用户再按一次。
        for (int i = 0; i < RETRY_DELAY_MS / 50 && !s_quit; i++) vTaskDelay(pdMS_TO_TICKS(50));
        portENTER_CRITICAL(&s_lock);
        s_req_pending = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

int radio_player_start(void)
{
    if (s_task) return 0;
    // 解码器两层注册只做一次。以前每次收台都注册/反注册一遍,纯浪费,
    // 还埋着"上一台没反注册干净影响下一台"的状态错乱风险。
    // register_default() 会把组件里所有解码器都注册进来,镜像因此多约 580KB;
    // 分区有 7.27MB,够用,不为了体积去手拼函数指针表赌解码不出问题。
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器注册失败");
        return -1;
    }
    // 收听任务栈:音频写与解码都在这里,给足但别浪费(无 PSRAM)。
    const BaseType_t ok = xTaskCreate(player_task, "radio", 6144, NULL, 5, &s_task);
    return ok == pdPASS ? 0 : -1;
}

void radio_play(const char *name, const char *url)
{
    if (!url) { radio_stop(); return; }
    s_paused = false;
    portENTER_CRITICAL(&s_lock);
    if (name) snprintf(s_req_name, sizeof(s_req_name), "%s", name);
    snprintf(s_req_url, sizeof(s_req_url), "%s", url);
    s_req_pending = true;
    portEXIT_CRITICAL(&s_lock);
    s_quit = true;                 // 唤醒任务并让它放弃当前流
    if (s_task) xTaskNotifyGive(s_task);
}

void radio_stop(void)
{
    s_paused = false;
    portENTER_CRITICAL(&s_lock);
    s_req_url[0] = '\0';
    s_req_pending = true;
    portEXIT_CRITICAL(&s_lock);
    s_quit = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void radio_set_volume(uint8_t percent)
{
    const uint8_t v = percent > 100 ? 100 : percent;
    s_req_vol = v;
    portENTER_CRITICAL(&s_lock);
    const bool busy = (s_snap.state == RADIO_PLAYING || s_snap.state == RADIO_PAUSED);
    s_snap.volume = v;
    portEXIT_CRITICAL(&s_lock);
    // 播放中绝不从这里并发调 esp_codec_dev(与收听任务的 esp_codec_dev_write
    // 撞在 codec 内部锁上,系统整体静默楔死,连 panic 都没有——真机实测)。
    // 收听任务在两次写块之间套用 s_req_vol;未播放时 codec 未打开,直接设安全。
    if (!busy) bsp_audio_set_volume(v);
}

uint8_t radio_player_level(void) { return s_lvl; }

void radio_player_toggle_pause(void)
{
    if (s_snap.state != RADIO_PLAYING && s_snap.state != RADIO_PAUSED) return;
    s_paused = !s_paused;
    set_snap(s_paused ? RADIO_PAUSED : RADIO_PLAYING, RADIO_ERR_NONE);
}

void radio_player_snapshot(radio_player_snap_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    portEXIT_CRITICAL(&s_lock);
}
