// tests/appfw_net.h —— 主机测试桩:biglist 只读 net 状态决定开机窗口是否
// 推迟挂载;appfw_mcp 的内置工具还读 IP/AP 名。主机上恒为 ONLINE。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APPFW_NET_IDLE 0
#define APPFW_NET_SCANNING 1
#define APPFW_NET_CONNECTING 2
#define APPFW_NET_ONLINE 3
#define APPFW_NET_OFFLINE_RETRY 4

typedef struct {
    int state;
    bool portal_active;
    char ip[16];
    char ap_ssid[33];
} appfw_net_status_t;

static inline void appfw_net_get_status(appfw_net_status_t *st)
{
    st->state = APPFW_NET_ONLINE;
    st->portal_active = false;
    st->ip[0] = '\0';
    st->ap_ssid[0] = '\0';
}

static inline void appfw_net_connect_ssid(const char *ssid) { (void)ssid; }
