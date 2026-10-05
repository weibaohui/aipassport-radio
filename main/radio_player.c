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
#include "portmacro.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include "radio_streams.h"
#include "appfw_frame.h"
#include "appfw_hls.h"
#include "appfw_stream.h"
#include "radio_mp3_probe.h"
#include "radio_ts_probe.h"
#include "appfw_viz.h"
#include "appfw_loudness.h"

static const char *TAG = "radio_player";

// MP3 一帧最多 1152 个采样;立体声 16bit = 4608 字节,取 5120(帧最大值向上留一档,
// 与参考实现一致)。再大就是白占内存:这台机器的堆要同时容纳环桶和解码器内部缓冲。
#define PCM_BUF_SIZE  8192
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

static size_t sbuf_write(sbuf_t *q, const uint8_t *in, size_t len)
{
    const size_t used = sbuf_used(q);
    if (len > q->cap - 1 - used) len = q->cap - 1 - used;   // 保留 r!=w 判空
    const size_t w = q->w % q->cap;
    const size_t tail = q->cap - w;
    const size_t first = len < tail ? len : tail;
    memcpy(q->buf + w, in, first);
    memcpy(q->buf, in + first, len - first);
    q->w += len;
    return len;
}

// 从音频 arena 顺序取一块(调用方按序申请,总量 ≤20KB;收台时归零复用)。
static uint8_t *s_arena;
static size_t s_arena_used;
// 解码器预留块:helix MP3 首次解码要一次性 malloc ~20KB 连续堆,播放开始时
// 释放这块给它,播完再收回。开机时与 arena 一起预留(总量 47KB)。
static uint8_t *s_dec_reserve;
static void reacquire_reserve_force(void);   // 连接/收尾路径补回预留(定义在后)
static uint8_t *audio_arena_take(size_t len)
{
    if (!s_arena || s_arena_used + len > 20 * 1024) return NULL;
    uint8_t *p = s_arena + s_arena_used;
    s_arena_used += len;
    memset(p, 0, len);
    return p;
}

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


static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_player_snap_t s_snap;
// 最近一帧 PCM 峰值(0..255)。单字节 volatile,读写无撕裂;每帧 26ms 更新一次,
// UI 以 10Hz 读 —— 播放器侧唯一为显示付出的成本是下混循环里多一次整数比较。
static volatile uint8_t s_lvl;

// 音频 arena:开机预留 20KB,播放的全部缓冲从这里 bump 分配(见
// radio_player_reserve/audio_arena_take),不与主堆互抢连续块。
// HLS 播放列表文本缓冲也从 arena 出(传给框架传输层)。
#define HLS_TEXT_CAP 4096

// 切台请求:由 UI 线程写、任务读。用任务通知唤醒,避免忙等。
static TaskHandle_t s_task;
static volatile bool s_quit;          // 置位表示要放弃当前流
static char s_req_name[RADIO_URL_MAX > 32 ? 32 : RADIO_URL_MAX];
static char s_req_url[RADIO_URL_MAX];
static volatile bool s_req_pending;
static volatile uint8_t s_req_vol = 55;
static volatile bool s_paused;              // 暂停:保持连接,丢弃音频
static volatile uint8_t s_applied_vol;      // codec 当前实际套用的音量
// 音频专用 arena(开机预留 20KB,常驻):环桶/PCM/喂数/解复用缓冲全部从
// 这里 bump 分配——若从主堆散着分配,会把 helix 解码器初始化需要的连续块
// 挤碎(ret 10 刷屏、永远"正在连接")。解码器句柄仍从主堆分配。

// 把 helix 首帧的惰性分配提前"烧"掉:喂一小段内嵌 MP3(radio_mp3_probe.h),
// 让它的 ~20KB 在解码器预留洞里落位——必须发生在连接之前,否则 https 的
// TLS 握手(内 8K/外 4K)先吃洞,首帧时最大连续块就不够了(ret 10 刷屏,
// 真机 https 台全军覆没的根因)。输出直接丢进 PCM 缓冲,不碰 codec;
// 解码器对真实流自行重同步。失败不致命:大不了回到"首帧惰性分配"的老路径。
// TS(AAC,HLS)同理,用 radio_ts_probe.h 的探针喂 TS 解码器。
static void dec_prewarm(esp_audio_simple_dec_handle_t dec,
                        esp_audio_simple_dec_type_t type,
                        uint8_t *pcm, size_t pcm_cap)
{
    const uint8_t *probe = (type == ESP_AUDIO_SIMPLE_DEC_TYPE_TS)
                               ? K_TS_PROBE : K_MP3_PROBE;
    const size_t probe_len = (type == ESP_AUDIO_SIMPLE_DEC_TYPE_TS)
                                 ? sizeof(K_TS_PROBE) : sizeof(K_MP3_PROBE);
    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)probe,
        .len = probe_len,
        .eos = false,
        .consumed = 0,
    };
    int rounds = 0;
    while (raw.len > 0 && rounds++ < 16) {
        esp_audio_simple_dec_out_t out = {
            .buffer = pcm, .len = (uint32_t)pcm_cap,
            .needed_size = 0, .decoded_size = 0,
        };
        const esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &out);
        if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) break;
        if (raw.consumed == 0) break;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        raw.consumed = 0;
    }
    ESP_LOGI(TAG, "解码器预热完成(%d 轮)", rounds);
}


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

// FFT 分析只跑在音频线程这一侧:appfw_viz_render() 是带平滑的(上升 0.45 /
// 下降 0.10),必须和解码同频调用才有"律动"。UI 线程只读下面这 17 个字节,
// 不碰 appfw_viz_t 内部状态 —— 跨线程共享 buf/gain 会引出真正的竞态。
static appfw_viz_t s_viz;
static uint32_t s_viz_rate;
static uint8_t  s_viz_raw[APPFW_VIZ_BANDS];
// 逐字节写、逐字节读。撕裂最多让某一帧的某一段跳一格,肉眼不可见;
// 为这 17 字节上互斥量反而不值得。
static volatile uint8_t s_viz_out[APPFW_VIZ_BANDS];
static volatile uint8_t s_viz_level;

// 频谱动画已整体停用(2026-10-03):FFT 即便定点也占 ~1/4 CPU,仿真器上更是
// 直接把解码挤到欠载。当前唯一目标是播放流畅——解码出的 PCM 只写 I2S。
// 要恢复频谱:把 APPFW_VIZ_ENABLED 置 1 即可(实现完好,框架主机测试仍在跑)。
#define APPFW_VIZ_ENABLED 0
static void viz_feed(const int16_t *pcm, size_t bytes, uint32_t rate)
{
    if (!APPFW_VIZ_ENABLED) return;
    if (rate == 0) return;
    if (s_viz_rate != rate) {          // 换台/换流可能换采样率,分频要重算
        appfw_viz_init(&s_viz, (float)rate);
        s_viz_rate = rate;
    }
    appfw_viz_push(&s_viz, pcm, bytes);
    appfw_viz_render(&s_viz, s_viz_raw);
    for (int k = 0; k < APPFW_VIZ_BANDS; k++) s_viz_out[k] = s_viz_raw[k];
    s_viz_level = appfw_viz_level(&s_viz);
}

// ---- 响度均衡(智能维持) ----
static appfw_loudness_t s_loud;
static bool s_loud_on = true;        // 默认开;设置菜单「智能维持」持久化

void radio_player_set_loudness(bool on)
{
    s_loud_on = on;
    ESP_LOGI(TAG, "响度均衡:%s", on ? "开" : "关");
}

// 解码输出统一过这里再进 I2S。增益变化每 5 秒记一条日志,给真机调参看曲线。
static void loudness_apply(int16_t *pcm, size_t bytes)
{
    if (!s_loud_on) return;
    appfw_loudness_process(&s_loud, pcm, bytes);
    static uint32_t blocks;
    if (++blocks >= 50) {            // ≈5s
        blocks = 0;
        ESP_LOGI(TAG, "响度增益 %+d.%02ddB", (int)(s_loud.gain_mdB / 100),
                 (int)abs(s_loud.gain_mdB % 100));
    }
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
                appfw_loudness_reset(&s_loud, c->rate);   // 每台独立适应
                set_snap(RADIO_PLAYING, RADIO_ERR_NONE);
                ESP_LOGI(TAG, "开始播放: %uHz %uch", (unsigned)c->rate, c->ch);
            } else if (!c->audio_started) {
                if (bsp_audio_set_format(c->rate, 16, 1) != ESP_OK) {
                    ESP_LOGE(TAG, "codec 格式设置失败 %uHz", (unsigned)c->rate);
                    return RADIO_ERR_DECODE;
                }
                bsp_audio_set_volume(c->vol);
                c->audio_started = true;
                appfw_loudness_reset(&s_loud, c->rate);   // 每台独立适应
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
                loudness_apply(mono, (size_t)frames * 2);
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
                loudness_apply((int16_t *)out.buffer, out.decoded_size);
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

// 框架传输层的 ICY 曲名回调:落快照(与收听任务同上下文,无并发问题)。
static void stream_title_cb(void *user, const char *title)
{
    (void)user;
    set_title(title);
}

// ---------------------------------------------------------------- 收听一轮

// 尝试完整收听一个流,返回错误码。成功会一直播到 s_quit 或断流。
static radio_err_t run_one_stream(const char *url, bool *played)
{
    radio_err_t result = RADIO_ERR_NONE;
    esp_audio_simple_dec_handle_t dec = NULL;
    appfw_stream_t *stream = NULL;
    uint8_t *pcm = NULL;
    uint8_t *ring_buf = NULL;
    uint8_t *feed = NULL;
    uint8_t *scratch = NULL;
    uint8_t *pl_buf = NULL;
    size_t ring_cap = 0;
    sbuf_t ring = { 0 };
    size_t prefill = 0;

    if (played) *played = false;

    if (!radio_url_valid(url)) return RADIO_ERR_URL;

    set_snap(RADIO_CONNECTING, RADIO_ERR_NONE);
    set_title(NULL);

    // ---- 先开解码器、拿全部分配,再建连接 ----
    // (吸收 shulinbao/ai-passport-radio 的教训)连接建立后 TLS/TCP 会话会把堆
    // 切碎,那时再开解码器就是 MEM_LACK;helix MP3 首次解码还要一次性惰性分配
    // 约 20KB 连续堆。顺序必须是:解码器 → 全部缓冲 → 连接。环桶上限也为此压
    // 到 8KB——它挤占的正是解码器要用的那块连续内存(真机 ret 10 刷屏的根因)。
    ESP_LOGI(TAG, "解码器分配前 heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // HLS(.m3u8):解码器走 TS(内部 AAC);直连(.mp3)走 MP3。
    // HLS(.m3u8):解码器走 TS(内部 AAC);直链走 MP3。
    const bool is_hls = appfw_hls_is_playlist_url(url);
    const esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type = is_hls ? ESP_AUDIO_SIMPLE_DEC_TYPE_TS : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
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
    // (不再卸载 FAT:清单里已无 HLS,FAT 常驻 ~8KB 完全可承受。此前每次
    // 切台都卸载+重挂,挂载一旦失败(句柄漏光)就表现为"第 7 台起切不动"。)
    // 解码器预留块先行释放:helix 初始化要 ~20KB 连续堆,先给它腾地方。
    if (s_dec_reserve) {
        free(s_dec_reserve);
        s_dec_reserve = NULL;
        ESP_LOGI(TAG, "解码器预留块已释放(最大块 %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    // 全部缓冲从开机预留的音频 arena 里 bump 分配(不碰主堆,保解码器的
    // 连续块)。每次收台把 arena 用量归零复用。
    if (!s_arena) { result = RADIO_ERR_DECODE; goto done; }
    s_arena_used = 0;
    pcm = audio_arena_take(PCM_BUF_SIZE);
    // HLS 需要 4KB 放播放列表文本;环桶同压到 4KB,arena 总量才装得下。
    ring_cap = is_hls ? (4 * 1024)
             : ((strncmp(url, "https://", 8) == 0) ? (4 * 1024) : RING_CAP_MAX);
    ring_buf = audio_arena_take(ring_cap);
    feed = audio_arena_take(FEED_CHUNK);
    scratch = audio_arena_take(FEED_CHUNK);
    if (is_hls) {
        pl_buf = audio_arena_take(HLS_TEXT_CAP);
        if (!pl_buf) {
            ESP_LOGE(TAG, "arena 分配失败(播放列表缓冲)");
            result = RADIO_ERR_DECODE;
            goto done;
        }
    }
    if (!pcm || !ring_buf || !feed || !scratch) {
        ESP_LOGE(TAG, "arena 分配失败(需 17KB)");
        result = RADIO_ERR_DECODE;
        goto done;
    }
    ring.buf = ring_buf;
    ring.cap = ring_cap;
    prefill = ring_cap / 2;   // 半桶才开播:64kbps 下约 1 秒的网络抖动余量
    ESP_LOGI(TAG, "抖动缓冲 %uKB,预灌 %u 字节",
             (unsigned)(ring_cap / 1024), (unsigned)prefill);

    // 连接前预热解码器(见 dec_prewarm):解码器的 ~20KB 必须先于 TLS 落位。
    dec_prewarm(dec, dcfg.dec_type, pcm, PCM_BUF_SIZE);

    // ---- 连接(传输层在框架:重定向/HLS 段轮换/ICY 剥离/http 分身回退) ----
    const appfw_stream_cfg_t scfg = {
        .url = url,
        .playlist = (char *)pl_buf,
        .playlist_cap = is_hls ? HLS_TEXT_CAP : 0,
        .on_title = stream_title_cb,
    };
    const appfw_stream_err_t se = appfw_stream_open(&stream, &scfg);
    if (se != APPFW_STREAM_ERR_NONE) {
        result = (se == APPFW_STREAM_ERR_HTTP)   ? RADIO_ERR_HTTP
               : (se == APPFW_STREAM_ERR_URL)    ? RADIO_ERR_URL
                                                 : RADIO_ERR_CONNECT;
        goto done;
    }
    ESP_LOGI(TAG, "已连接:%s", appfw_stream_url(stream));

    // 换台:清掉"codec 已配好"的记忆,让新台的首帧按**它自己的**采样率
    // 重新 open 一次。不清的话会沿用上一台的采样率,播出来速率错位。
    s_coded_rate = 0;

    mp3_ctx_t ctx = {
        .dec = dec, .pcm = pcm, .pcm_cap = PCM_BUF_SIZE,
        .vol = s_req_vol, .audio_started = false, .bad_frames = 0,
        .rate = 0, .ch = 0, .logged_head = 0, .logged_err = 0,
    };

    // 主循环:桶低于水位就阻塞读一轮(esp_http_client_read 内部等到数据或
    // 超时),桶里就绪后喂一块——阻塞在 I2S 写上按实时走。稳态水位钉在
    // 预灌线附近,TCP 抖动由桶深吸收,不再打穿 codec 的 DMA。
    bool eof = false;      // 对端关闭/服务端停推:播完桶里剩余就收尾
    bool started = false;  // 已开播(过了预灌水位);之前不喂,先攒水
    int timeouts = 0;      // 连续读不到数据(超时)的轮数:断流判定
    int empty = 0;         // 客户端缓冲连续为空的轮数:服务端停推判定
    size_t fed_total = 0;  // 喂给解码器的总字节数(迟迟解不动的兜底计数)
    uint8_t c_vol_applied = s_req_vol;   // codec 当前实际套用的音量
    for (;;) {
        if (s_quit) break;

        // 暂停:保持连接,读到的音频直接丢弃;恢复即从最新流继续(直播语义)。
        if (s_paused) {
            const int n = appfw_stream_read(stream, feed, FEED_CHUNK);
            if (n > 0) { s_lvl = 0; timeouts = 0; continue; }
            if (n == 0) { if (++empty >= 3) { eof = true; } vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            result = RADIO_ERR_CONNECT;
            goto done;
        }

        // 补桶:桶低于水位才读(读会阻塞到数据到达;桶里还有料时不读,防饥饿)。
        // 框架传输层读出的就是剥完 ICY 的纯音频;HLS 的段轮换/列表刷新也在
        // 里面完成(阻塞直到有数据)。曲名经 on_title 回调落到快照。
        while (!eof && sbuf_used(&ring) < prefill) {
            const int n = appfw_stream_read(stream, scratch, FEED_CHUNK);
            if (n > 0) {
                timeouts = 0; empty = 0;
                sbuf_write(&ring, scratch, (size_t)n);
                continue;
            }
            if (n == 0) {
                // 客户端缓冲空且网络无数据:连续多轮判定为断流。
                if (++empty >= 3) { eof = true; break; }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ESP_LOGW(TAG, "流读取失败 errno=%d", errno);
            result = RADIO_ERR_CONNECT;
            goto done;
        }
        if (s_quit) break;

        const size_t used = sbuf_used(&ring);
        if (used == 0) {
            if (eof) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t take = used < FEED_CHUNK ? used : FEED_CHUNK;
        sbuf_read(&ring, feed, take);
        // 起始帧对齐已停用:实测(2026-10-03)跳到帧头喂入反而让解码器
        // 初始化失败(ret 10);从字节 0 原样喂,解码器自行重同步。
        fed_total += take;
        const radio_err_t de = mp3_feed(&ctx, feed, take);
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
    appfw_stream_close(stream);
    esp_audio_simple_dec_close(dec);
    reacquire_reserve_force();
    // 收台:让 codec 回到静音。ES8311 配好格式后会一直按当前采样率输出,
    // 不 mute 的话最后一帧的余音会在扬声器里拖出去,切台时"咔"一下。
    bsp_audio_set_volume(0);
    s_coded_rate = 0;

    return result;
}

void radio_player_release_reserve(void)
{
    if (s_dec_reserve) {
        free(s_dec_reserve);
        s_dec_reserve = NULL;
        ESP_LOGI(TAG, "解码器预留块已释放(让位)");
    }
}

// 无条件补回(连接流程内部用:那时快照还显示"连接中",不能做状态检查)。
static void reacquire_reserve_force(void)
{
    if (s_dec_reserve) return;
    size_t got = 0;
    for (size_t sz = 60 * 1024; sz >= 8 * 1024; sz -= 4 * 1024) {
        if ((s_dec_reserve = malloc(sz))) { got = sz; break; }
    }
    if (got) ESP_LOGI(TAG, "解码器预留块已恢复(%uKB)", (unsigned)(got / 1024));
}

// 尽量把解码器预留块补回 60KB。文件库用完卸载后调用;播放/连接中不补
// (预留块本来就该让位,播放停了再补)。
void radio_player_reacquire_reserve(void)
{
    if (s_dec_reserve) return;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state == RADIO_CONNECTING || s.state == RADIO_PLAYING) return;
    reacquire_reserve_force();
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
        snprintf(s_snap.url, sizeof(s_snap.url), "%s", url);
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

void radio_player_reserve(void)
{
    if (s_arena) return;
    // 20KB = 环桶 8K + PCM 5K + 喂数 2K + 解复用 2K(+1K 余量)
    s_arena = malloc(20 * 1024);
    // 解码器预留块:播放开始时释放,要先装下 helix 预热(~20KB)+ https 时
    // TLS 会话(内 8K/外 4K)。27KB 是 http 时代的数;https 要 TLS(12KB)+ 解码器;HLS 的 TS+AAC 更是
    // 要 40KB 级连续块,最终提到 60KB(HLS 的 AAC 初始化要 ~60KB 连续块)。
    s_dec_reserve = malloc(60 * 1024);
    ESP_LOGI(TAG, "音频 arena 预留 %s(20KB) + 解码器预留 %s(60KB)",
             s_arena ? "ok" : "fail", s_dec_reserve ? "ok" : "fail");
}

int radio_player_start(void)
{
    if (s_task) return 0;
    // 响度均衡默认参数;每台开始时 reset 重算块长与测量状态。
    appfw_loudness_init(&s_loud, NULL);
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
    // https 的 HLS 在这台机器必然内存不足(TLS ~16KB + AAC 60KB 两个连续块
    // 装不下,实测 mbedtls_ssl_setup -0x7F00):一律按 http 请求。国内电台
    // CDN 普遍双协议;只有 https 的 HLS 台会连不上,界面如实报错。
    char u[RADIO_URL_MAX];
    snprintf(u, sizeof(u), "%s", url);
    if (strncmp(u, "https://", 8) == 0 && strstr(u, ".m3u8")) {
        memmove(u + 7, u + 8, strlen(u) - 8 + 1);   // 去掉 s,少一个 '/'
        memcpy(u, "http://", 7);
        ESP_LOGI(TAG, "HLS 降级 http:%s", u);
    }
    portENTER_CRITICAL(&s_lock);
    if (name) snprintf(s_req_name, sizeof(s_req_name), "%s", name);
    snprintf(s_req_url, sizeof(s_req_url), "%s", u);
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

// 开机尽早调用:预留一块大连续内存给播放管线(解码器/环桶/PCM)。
void radio_player_reserve(void);

void radio_player_pause(void)
{
    if (s_snap.state != RADIO_PLAYING) return;
    s_paused = true;
    set_snap(RADIO_PAUSED, RADIO_ERR_NONE);
}

bool radio_player_resume(void)
{
    if (s_snap.state != RADIO_PAUSED) return false;
    s_paused = false;
    set_snap(RADIO_PLAYING, RADIO_ERR_NONE);
    return true;
}

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
