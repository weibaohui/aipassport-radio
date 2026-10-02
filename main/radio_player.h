// main/radio_player.h —— 收听任务:TCP 拉流 → ICY 元数据 → MP3 解码 → I2S。
//
// 独立工作任务,不碰 LVGL;UI 通过 radio_player_snapshot() 读线程安全快照。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radio_streams.h"  // station[].size 依赖 RADIO_URL_MAX

#define RADIO_TITLE_MAX 64   // ICY 曲名显示上限(超长截断)

typedef enum {
    RADIO_STOPPED = 0,  // 未在收听
    RADIO_CONNECTING,   // 正在解析地址/建连/读响应头
    RADIO_PLAYING,      // 正常解码播放
    RADIO_ERROR,        // 出错(见 err_code)
} radio_state_t;

// 错误码:UI 据此显示文案。0 表示无错。
typedef enum {
    RADIO_ERR_NONE = 0,
    RADIO_ERR_URL,      // URL 非法
    RADIO_ERR_RESOLVE,  // 域名解析失败
    RADIO_ERR_CONNECT,  // TCP 连接失败/超时
    RADIO_ERR_HTTP,     // 服务端未返回 200,或响应头不完整
    RADIO_ERR_DECODE,   // MP3 解码器打开/解码失败
    RADIO_ERR_TIMEOUT,  // 连续多次读不到数据(流断了)
} radio_err_t;

typedef struct {
    radio_state_t state;
    radio_err_t err_code;
    char station[RADIO_URL_MAX > 32 ? 32 : RADIO_URL_MAX]; // 当前台名
    char title[RADIO_TITLE_MAX];                           // ICY 正在播放
    uint32_t sample_rate;   // 解码器上报的实际采样率
    uint8_t channels;       // 实际声道数(解码器上报)
    uint32_t bitrate;       // kbps,由 icy-br 或解码器上报
    uint8_t volume;         // 0..100
} radio_player_snap_t;

// 启动收听任务(常驻)。成功返回 0。
int radio_player_start(void);

// 切台。传入台名仅用于显示;url 必须是合法的 http:// 流地址。
// 立即返回:真正建连在任务里做。任何时刻可调用,内部会中止上一次收听。
void radio_play(const char *name, const char *url);

// 停止收听(关闭 socket 与解码器)。
void radio_stop(void);

// 设置音量 0..100。
void radio_set_volume(uint8_t percent);

// 读取当前状态快照(自旋锁保护,整体拷贝)。out 为 NULL 时忽略。
void radio_player_snapshot(radio_player_snap_t *out);
