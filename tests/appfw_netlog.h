// tests/appfw_netlog.h —— 主机测试桩:appfw_mcp 的诊断工具调用网络日志
// 接口;主机上没有钩子/UDP/环形缓冲,给空实现(真模块的真机验证另做)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline void appfw_netlog_init(void) {}
static inline int appfw_netlog_recent(char *out, size_t cap, int max_lines,
                                      uint32_t *dropped)
{
    (void)max_lines;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (dropped) *dropped = 0;
    return 0;
}
static inline bool appfw_netlog_push_configure(bool on, const char *ip, uint16_t port)
{
    (void)on; (void)ip; (void)port;
    return on;
}
static inline bool appfw_netlog_push_active(void) { return false; }
static inline void appfw_netlog_push_dest(char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
}
