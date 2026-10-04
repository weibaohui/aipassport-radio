// main/radio_pages.h —— 电台列表 UI 与门户配置卡片(挂到 appfw_ui 框架上)。
#pragma once

#include "appfw_ui.h"

// 装配期初始化:载入内置台与用户自加电台。必须在 home_build 之前调用。
void radio_pages_init(void);
// 主页:电台列表 + 光标 + 状态区。
// 框架重建页面前回调:清空应用侧悬空对象把手(见 appfw_ui_cfg_t::page_reset)。
void radio_pages_page_reset(void);

void radio_pages_home_build(lv_obj_t *page);
void radio_pages_home_poll(void);

// 主页按键接管:上/下移光标、OK 播放或停止、长按调音量、"设置"行回菜单。
// 返回 appfw_key_action_t,见 appfw_ui.h。
appfw_key_action_t radio_pages_home_key(int btn, int ev);

// 设备信息页追加行。
int radio_pages_info_rows(char (*keys)[16], char (*vals)[72], int max);

// 门户:应用配置卡片 HTML。
// 门户:导入/保存时写回用户自加电台。
bool radio_pages_app_config_apply(void *cjson_root);
// 门户:状态/导出回显。
void radio_pages_app_config_fill(void *cjson_obj);

// httpd 就绪后注册应用私有端点(POST /api/radio)。
bool radio_pages_portal_register(void *httpd);

// 当前选中项下标(供测试与调试)。
int radio_pages_cursor(void);
