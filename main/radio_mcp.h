// main/radio_mcp.h —— 收音机的 MCP 工具注册(把设备能力开放给局域网 AI)。
#pragma once

void radio_mcp_init(void);   // 注册工具表(appfw_mcp_set_tools);可重复调用
