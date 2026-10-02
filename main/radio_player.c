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

static const char *TAG = "radio_player";

// MP3 一帧最多 1152 个采样;立体声 16bit 即 4608 字节,留一倍余量。
#define PCM_BUF_SIZE  8192
// 一次从 socket 读多少音频字节。Simple Decoder 自己缓存跨块的半帧,
// 所以这里不需要为"帧尾巴"预留额外空间。
#define RX_BUF_SIZE   4096
#define WORK_BUF_SIZE RX_BUF_SIZE
// recv 超时:让任务能被切台请求及时打断,同时不至于频繁空转。
#define RX_TIMEOUT_MS 500
// 连续多少次读不到数据判定为断流(约 20 秒)。
#define RX_TIMEOUT_MAX 40
// 连续多少个解码失败后放弃重连。广播流偶有坏帧,单帧失败不该断流。
#define MP3_MAX_BAD_FRAMES 32
// 同一个台失败后的自动重连间隔(毫秒)。
#define RETRY_DELAY_MS 3000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static radio_player_snap_t s_snap;

// 切台请求:由 UI 线程写、任务读。用任务通知唤醒,避免忙等。
static TaskHandle_t s_task;
static volatile bool s_quit;          // 置位表示要放弃当前流
static char s_req_name[RADIO_URL_MAX > 32 ? 32 : RADIO_URL_MAX];
static char s_req_url[RADIO_URL_MAX];
static volatile bool s_req_pending;
static volatile uint8_t s_req_vol = 55;
// 跨读边界的曲名拼接余量(见 scan_stream_title),单线程使用。
static uint8_t s_title_tail[sizeof("StreamTitle='") - 2];
static size_t s_title_tail_len;

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

// 读满 len 字节。返回实际读到的字节数;0 表示对端已关闭,-1 表示超时/出错。
// aborted 置位表示收到切台请求。
static int read_exact(int fd, uint8_t *buf, size_t len, bool *aborted)
{
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        if (s_quit) { if (aborted) *aborted = true; return -1; }
        const int n = recv(fd, buf + got, len - got, 0);
        if (n > 0) { got += (size_t)n; idle = 0; continue; }
        if (n == 0) return (int)got;                    // 对端关闭,把已读的交回去
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // SO_RCVTIMEO 到期。必须计数后返回,否则对端静默时会在这里死转,
            // 既不喂狗也永远等不到断流判定。
            if (++idle >= RX_TIMEOUT_MAX) return -1;
            continue;
        }
        return -1;
    }
    return (int)got;
}

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

// ICY 曲名不按 metaint 分帧去取,而是在音频流里直接扫 StreamTitle='...'。
//
// 原因:metaint 只有"这条流声称"的份量。实测 KEXP 声明 icy-metaint: 4096,
// 第一个元数据块也确实是标题,但紧接着的块取出来是一堆 MP3 字节——它并不
// 稳定遵守自己的 metaint。严格按 metaint 切分就会在第二个块之后永久错位,
// 而错位之后音频虽然还能靠解码器重新同步继续放,曲名却永远拿不到。
//
// 扫全流反而更稳:模式串 12 字节且极特殊,16KB/s 的音频里误命中概率可以忽略,
// 即使命中也只会短暂显示一次乱码标题。这比"信任服务端声明的分帧"可靠得多。
// 注意仍然要发 Icy-MetaData: 1——不声明的话服务端根本不会把标题插进流里。
static void scan_stream_title(const uint8_t *buf, size_t len)
{
    static const char key[] = "StreamTitle='";
    static const size_t klen = sizeof(key) - 1;
    if (len < klen) return;

    // 曲名可能正好被读边界切成两半,所以先把上一块末尾 klen-1 字节接到本块前面
    // 一起扫,扫完再留本块末尾那一段。代价只有几十字节,换来不会漏曲名。
    static uint8_t tail[sizeof("StreamTitle='") - 2];
    const size_t tail_len = s_title_tail_len;
    uint8_t *joined = malloc(tail_len + len);
    if (!joined) return;
    memcpy(joined, tail, tail_len);
    memcpy(joined + tail_len, buf, len);

    for (size_t i = 0; i + klen < tail_len + len; i++) {
        if (memcmp(joined + i, key, klen) != 0) continue;
        size_t j = i + klen;
        while (j < tail_len + len && joined[j] != '\'' && joined[j] != ';' && joined[j] != '\0') j++;
        char title[RADIO_TITLE_MAX];
        if (j < tail_len + len) {
            size_t n = j - (i + klen);
            if (n >= sizeof(title)) n = sizeof(title) - 1;
            memcpy(title, joined + i + klen, n);
            title[n] = '\0';
            if (title[0]) set_title(title);
        }
        break;
    }

    memcpy(tail, joined + (tail_len + len) - (klen - 1), klen - 1);
    s_title_tail_len = klen - 1;
    free(joined);
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
    uint32_t rate;      // 解码器上报的采样率(用 get_info 拿,不是每次 decode 都带)
    uint8_t ch;         // 解码器上报的声道数
    int logged_head;    // 是否已打印过流首字节(诊断用)
    int logged_err;     // 已打印的错误码条数,避免刷屏
} mp3_ctx_t;

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

            if (!c->audio_started) {
                // ES8311 是单声道 codec,立体声流在这里下混后送出:
                // I2S 流量减半,也更贴合硬件。
                if (bsp_audio_set_format(c->rate, 16, 1) != ESP_OK) {
                    ESP_LOGE(TAG, "codec 格式设置失败 %uHz", (unsigned)c->rate);
                    return RADIO_ERR_DECODE;
                }
                bsp_audio_set_volume(c->vol);
                c->audio_started = true;
                set_snap(RADIO_PLAYING, RADIO_ERR_NONE);
                ESP_LOGI(TAG, "开始播放: %uHz %uch", (unsigned)c->rate, c->ch);
            }

            if (c->ch >= 2) {
                const int16_t *in = (const int16_t *)out.buffer;
                int16_t *mono = (int16_t *)out.buffer;   // 原地下混,不额外占内存
                const int frames = (int)(out.decoded_size / 4);
                for (int i = 0; i < frames; i++) {
                    mono[i] = (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
                }
                bsp_audio_write(mono, (size_t)frames * 2);
            } else {
                bsp_audio_write(out.buffer, out.decoded_size);
            }
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
static radio_err_t run_one_stream(const char *url)
{
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

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "域名解析失败: %s", host);
        return RADIO_ERR_RESOLVE;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return RADIO_ERR_CONNECT; }

    struct timeval tv = { .tv_sec = 0, .tv_usec = RX_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "连接失败 %s:%d", host, port);
        close(fd);
        freeaddrinfo(res);
        return RADIO_ERR_CONNECT;
    }
    freeaddrinfo(res);

    // 请求行 + 头。用 HTTP/1.0,服务端直接吐原始流,不做 chunked 编码。
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
    if (send(fd, req, (size_t)rlen, 0) != rlen) { close(fd); return RADIO_ERR_CONNECT; }

    // 读响应头(上限 1KB,足够放 status + 几个 ICY 头)
    char hdr[1024];
    size_t hlen = 0;
    bool aborted = false;
    while (hlen < sizeof(hdr) - 1) {
        const int n = recv(fd, (uint8_t *)hdr + hlen, 1, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (s_quit) { aborted = true; break; }
            close(fd);
            return RADIO_ERR_HTTP;
        }
        hlen += (size_t)n;
        hdr[hlen] = '\0';
        if (hlen >= 4 && strstr(hdr, "\r\n\r\n")) break;
    }
    if (aborted) { close(fd); return RADIO_ERR_NONE; }
    hdr[hlen] = '\0';
    if (!strstr(hdr, "\r\n\r\n") || strncmp(hdr, "HTTP/1.", 7) != 0) { close(fd); return RADIO_ERR_HTTP; }
    if (strstr(hdr, " 200") == NULL) {
        ESP_LOGW(TAG, "非 200 响应: %.40s", strchr(hdr, '\r'));
        close(fd);
        return RADIO_ERR_HTTP;
    }

    long metaint = 0;
    char val[64];
    if (header_value(hdr, "icy-metaint", val, sizeof(val))) metaint = atol(val);
    if (header_value(hdr, "icy-br", val, sizeof(val))) {
        set_snap_stream_info(0, 0, (uint32_t)atoi(val));
    }
    if (metaint < 0) metaint = 0;
    // 这条路径的栈用量(请求行 + 响应头 + host 等)约 1.5KB,报一下余量,
    // 真机上如果余量很小就能提前发现,而不是等到某天莫名其妙地 stack fault。
    ESP_LOGI(TAG, "已连接 %s, icy-metaint=%ld, 任务栈余量 %u 字节",
             host, metaint, (unsigned)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)));

    // Simple Decoder 需要两层注册:底层解码器 + 上层的解析/聚合层。
    // MP3 不需要额外的 dec_cfg(见组件自带用例的 default 分支)。
    //
    // register_default() 会把组件里所有解码器都注册进来,镜像因此多了约 580KB
    // (1560KB → 2144KB)。分区有 7.27MB,够用,所以没去抠这块体积。真要抠得用
    // esp_audio_simple_dec_register(type, reg_info) 按类型注册,但那要自己拼
    // 一整张函数指针表,组件也没给 MP3 的现成 OPS 宏;为了省 580KB 去手拼一张
    // 表、赌它别把解码搞坏,不划算。
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器注册失败");
        close(fd);
        return RADIO_ERR_DECODE;
    }
    esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,   // false = 由它解析并缓存跨块的半帧
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&dcfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 解码器打开失败");
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        close(fd);
        return RADIO_ERR_DECODE;
    }

    // Simple Decoder 自己在内部缓存半帧,所以这里只要一块纯读缓冲。
    uint8_t *work = malloc(WORK_BUF_SIZE);
    uint8_t *pcm = malloc(PCM_BUF_SIZE);
    if (!work || !pcm) {
        ESP_LOGE(TAG, "缓冲分配失败(剩余堆 %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(work); free(pcm);
        esp_audio_simple_dec_close(dec);
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        close(fd);
        return RADIO_ERR_DECODE;
    }

    int timeouts = 0;
    radio_err_t result = RADIO_ERR_NONE;
    mp3_ctx_t ctx = {
        .dec = dec, .pcm = pcm, .pcm_cap = PCM_BUF_SIZE,
        .vol = s_req_vol, .audio_started = false, .bad_frames = 0,
        .rate = 0, .ch = 0, .logged_head = 0, .logged_err = 0,
    };
    // 不再按 icy-metaint 分帧:实测有服务端(KEXP)并不稳定遵守自己声明的
    // metaint,严格切分会在第二个块之后永久错位。曲名改为在音频流里扫
    // StreamTitle 模式串(见 scan_stream_title),所以这里就是单纯的
    // "读一块 → 扫曲名 → 喂解码器"。
    s_title_tail_len = 0;

    while (!s_quit) {
        const int n = read_exact(fd, work, WORK_BUF_SIZE, &aborted);
        if (aborted) break;
        if (n <= 0) {
            if (++timeouts < RX_TIMEOUT_MAX) continue;
            result = RADIO_ERR_TIMEOUT;
            break;
        }
        timeouts = 0;
        scan_stream_title(work, (size_t)n);
        const radio_err_t de = mp3_feed(&ctx, work, (size_t)n);
        if (de != RADIO_ERR_NONE) { result = de; break; }
    }

    free(work);
    free(ctx.pcm);   // 可能是 realloc 过的,不能 free 原始 pcm
    esp_audio_simple_dec_close(dec);
    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();
    close(fd);
    return result;
}

// ---------------------------------------------------------------- 任务

static void player_task(void *arg)
{
    (void)arg;
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

        radio_err_t err = run_one_stream(url);
        if (s_quit) { s_quit = false; continue; }          // 用户切台,不算失败
        if (err == RADIO_ERR_NONE) { set_snap(RADIO_STOPPED, RADIO_ERR_NONE); continue; }

        set_snap(RADIO_ERROR, err);
        ESP_LOGW(TAG, "收听失败,err=%d,%.2fs 后重连同一个台", (int)err, RETRY_DELAY_MS / 1000.0);
        // 退避期间仍可被切台打断
        for (int i = 0; i < RETRY_DELAY_MS / 50 && !s_quit; i++) vTaskDelay(pdMS_TO_TICKS(50));
        // 必须把请求重新置位,否则外层会回到"等用户操作"的空转里,
        // 明明日志写着"后重试",实际上要等用户再按一次 OK 才重连。
        // 用户在退避期间切台或停止的话,请求本身已被换掉,这里再置位无副作用。
        portENTER_CRITICAL(&s_lock);
        s_req_pending = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

int radio_player_start(void)
{
    if (s_task) return 0;
    // 收听任务栈:音频写与解码都在这里,给足但别浪费(无 PSRAM)。
    const BaseType_t ok = xTaskCreate(player_task, "radio", 6144, NULL, 5, &s_task);
    return ok == pdPASS ? 0 : -1;
}

void radio_play(const char *name, const char *url)
{
    if (!url) { radio_stop(); return; }
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
    s_snap.volume = v;
    portEXIT_CRITICAL(&s_lock);
    bsp_audio_set_volume(v);
}

void radio_player_snapshot(radio_player_snap_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    portEXIT_CRITICAL(&s_lock);
}
