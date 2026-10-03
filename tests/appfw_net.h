// tests/appfw_net.h —— 主机测试桩:radio_biglist.c 只读 net 状态来决定
// 开机窗口是否推迟挂载;主机上恒为 ONLINE(测试不等 WiFi)。
#pragma once

#include <stdbool.h>

#define APPFW_NET_IDLE 0
#define APPFW_NET_SCANNING 1
#define APPFW_NET_CONNECTING 2
#define APPFW_NET_ONLINE 3
#define APPFW_NET_OFFLINE_RETRY 4

typedef struct {
    int state;
} appfw_net_status_t;

static inline void appfw_net_get_status(appfw_net_status_t *st)
{
    st->state = APPFW_NET_ONLINE;
}
