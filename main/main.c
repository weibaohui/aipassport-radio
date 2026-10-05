// main/main.c —— 网络收音机(appfw 框架版)启动装配。
//
// 应用只负责:1) 框架初始化顺序;2) 注入业务(电台列表 UI / 门户片段 / 私有端点)。
// 全部通用能力(WiFi 引擎 / 配网门户 / 存储 / UI 骨架 / 熄屏 / 按键 / BSP)来自
// components/appfw(框架 submodule)。
#include <stdbool.h>

#include "appfw_client.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "appfw_ui.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "radio_pages.h"
#include "radio_mcp.h"
#include "appfw_mcp.h"
#include "appfw_netlog.h"
#include "radio_player.h"

static const char *TAG = "main";

// ---- 输入任务(框架提供事件规整与栈预算,应用只给回调) ----
static QueueHandle_t s_key_queue;
static volatile bool s_keys_ready;

static void on_key_from_bsp(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_keys_ready || !s_key_queue) return;
    const int msg = (int)btn | ((int)ev << 4);
    (void)xQueueSend(s_key_queue, &msg, 0);
}

static void key_task(void *arg)
{
    (void)arg;
    int msg;
    for (;;) {
        if (xQueueReceive(s_key_queue, &msg, portMAX_DELAY) == pdTRUE) {
            appfw_ui_on_key(msg & 0xF, (msg >> 4) & 0xF);
        }
    }
}

static void apply_volume(uint16_t percent)
{
    radio_set_volume((uint8_t)percent);
}

static void apply_effect(uint16_t v)
{
    radio_pages_set_effect((uint8_t)v);
}

static bool portal_ready(void *httpd)
{
    return radio_pages_portal_register(httpd);
}

// 门户启动前腾内存(覆盖框架弱符号):httpd 任务/控制块/路由表要 ~10KB,
// 而 60KB 解码器预留正好握在手里。释放它;下次开播会自动重新预留。
void appfw_portal_pre_start_hook(void)
{
    radio_player_release_reserve();
}

static void second_tick_cb(void *arg)
{
    (void)arg;
    appfw_ui_second_tick();
}

// 设置菜单显示哪些框架自带项(与 MCP 挂载的基础工具同一份配置)。
#define APP_MENU_SHOW_MASK (APPFW_MENU_ITEM_ALL & ~APPFW_MENU_ITEM_REFRESH_PERIOD)

void app_main(void)
{
    ESP_LOGI(TAG, "网络收音机(appfw)启动");
    radio_player_reserve();   // 最早预留 60KB 连续块(WiFi/LVGL 会碎片化堆)。
                              // 大清单的 FAT 用"借洞"方式与此共存(钩子在 radio_store.c 注入)。
    bsp_i2c_init();
    (void)bsp_battery_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);
    if (appfw_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败(电台配置将无法保存)");
    }

    // 音频:收听任务真正用到时才打开 codec,这里只做一次初始化检查。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败,收听将不可用");
    }
    // 音量:设置菜单(框架应用选项页)里选过的值优先;没存过用 55。
    uint16_t vol = 55;
    appfw_store_get_u16("opt_volume", &vol, 55);
    radio_set_volume((uint8_t)vol);
    uint16_t fx = 0;
    appfw_store_get_u16("opt_effect", &fx, 0);
    radio_pages_set_effect((uint8_t)fx);

    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) appfw_netlist_reset(&list);
    const int net_err = appfw_net_init(&list, false);
    if (net_err != 0) {
        ESP_LOGW(TAG, "WiFi 初始化返回 %d", net_err);
    }
    appfw_netlog_init();          // 网络日志:环形缓冲常开,AI 可取;UDP 推送按配置
    radio_mcp_init();             // MCP 工具表:AI 经门户 /mcp 操作设备
    // 框架基础功能跟随菜单使能位:菜单显示的项,AI 也能操作;没显示的不挂载。
    appfw_mcp_set_builtin_tools(APP_MENU_SHOW_MASK);

    // 载入电台列表(要在建页之前)。大清单模式下第一次 count 会"借洞"挂载
    // files 分区 FAT 建索引,空闲 10s 后自动卸载把预留补回。
    radio_pages_init();

    if (radio_player_start() != 0) {
        ESP_LOGE(TAG, "收听任务启动失败");
    }

    // 音量与响度均衡进框架设置菜单(应用选项页):选中即存 NVS 并生效。
    static const uint16_t k_vol_opts[] = { 0, 20, 40, 60, 80, 100 };
    static const char *const k_vol_lbls[] = { "0%", "20%", "40%", "60%", "80%", "100%" };
    static const uint16_t k_fx_opts[] = { 0, 1, 2 };
    static const char *const k_fx_lbls[] = { "经典频谱", "LED 电平表", "对称频谱" };
    static const appfw_menu_opt_t k_menu_opts[] = { {
        .key = "opt_volume", .label = "音量", .symbol = LV_SYMBOL_VOLUME_MID,
        .opts = k_vol_opts, .lbls = k_vol_lbls, .count = 6,
        .on_change = apply_volume,
    }, {
        // 动态效果:播放页律动面板三选一。
        .key = "opt_effect", .label = "动态效果", .symbol = LV_SYMBOL_SETTINGS,
        .opts = k_fx_opts, .lbls = k_fx_lbls, .count = 3,
        .on_change = apply_effect,
    } };

    const appfw_ui_cfg_t ucfg = {
        .home_title = "网络收音机",
        .home_build = radio_pages_home_build,
        .home_poll  = radio_pages_home_poll,
        .home_key   = radio_pages_home_key,
        .info_rows  = radio_pages_info_rows,
        .app_config_apply = radio_pages_app_config_apply,
        .app_config_fill  = radio_pages_app_config_fill,
        // 音量交给框架设置菜单;主页按键全被 home_key 接管,默认入口关掉。
        .menu_opts = k_menu_opts,
        .menu_opts_count = 2,
        // 内置菜单显式使能(默认全关):收音机要五项,刷新周期无意义不开。
        .menu_show_mask = APP_MENU_SHOW_MASK,
        // 主页长按动作表:上=设置菜单,下=音量页(长按 OK 留给应用自己)。
        .long_press_up = APPFW_LONG_PRESS_OPEN_MENU,
        .long_press_down = APPFW_LONG_PRESS_OPEN_APP_OPTION_1,
        .menu_open_btn = 0xFF,
        .page_reset = radio_pages_page_reset,
    };

    s_key_queue = xQueueCreate(8, sizeof(int));

    if (s_key_queue &&
        xTaskCreate(key_task, "app_input", 6144, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key_from_bsp, NULL) == ESP_OK) {
        s_keys_ready = true;
    } else {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    if (bsp_lvgl_lock(1000)) {
        appfw_ui_init(&ucfg);
        bsp_lvgl_unlock();
        s_keys_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,界面未创建");
    }

    const appfw_prov_cfg_t pcfg = {
        .app_config_apply = radio_pages_app_config_apply,
        .app_config_fill  = radio_pages_app_config_fill,
        .on_httpd_ready   = portal_ready,
    };
    appfw_prov_configure(&pcfg);
    // 配网门户按需(见 appfw_ui_second_tick):只在没网要配网时自启,联网后
    // 空闲 5 分钟自动卸载。AI 入口不在这条路上——MCP 常驻独立极简服务
    // (8080/mcp,设置菜单「AI管理」页查看地址)。

    esp_timer_handle_t tick;
    const esp_timer_create_args_t ta = { .callback = second_tick_cb, .name = "tick" };
    if (esp_timer_create(&ta, &tick) == ESP_OK) esp_timer_start_periodic(tick, 1000000);

    ESP_LOGI(TAG, "启动完成:net=%d heap=%u largest=%u", net_err,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
