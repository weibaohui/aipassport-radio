// tests/appfw_client.h —— 主机测试桩:appfw_mcp.c 的 bi_refresh 在设置刷新
// 周期后调 appfw_client_refresh_now;主机上没有客户端轮询,空实现。
#pragma once

static inline void appfw_client_refresh_now(void) {}
