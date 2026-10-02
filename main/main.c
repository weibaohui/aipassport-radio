// main/main.c —— 网络收音机(appfw 框架版)启动装配。
//
// 应用只负责:1) 框架初始化顺序;2) 注入业务(电台列表 UI / 门户片段 / 私有端点)。
// 全部通用能力(WiFi 引擎 / 配网门户 / 存储 / UI 骨架 / 熄屏 / 按键 / BSP)来自
// components/appfw(框架 submodule)。
#include <stdbool.h>

#include "appfw_client.h"
#include "appfw_files.h"
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
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "radio_pages.h"
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

static bool portal_ready(void *httpd)
{
    return radio_pages_portal_register(httpd);
}

static void second_tick_cb(void *arg)
{
    (void)arg;
    appfw_ui_second_tick();
}

void app_main(void)
{
    ESP_LOGI(TAG, "网络收音机(appfw)启动");
    bsp_i2c_init();
    (void)bsp_battery_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);
    if (appfw_files_init() != ESP_OK) {
        ESP_LOGE(TAG, "文件分区挂载失败(文件管理不可用)");
    }
    if (appfw_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败(电台配置将无法保存)");
    }

    // 音频:收听任务真正用到时才打开 codec,这里只做一次初始化检查。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败,收听将不可用");
    }
    radio_set_volume(55);

    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) appfw_netlist_reset(&list);
    const int net_err = appfw_net_init(&list, false);
    if (net_err != 0) {
        ESP_LOGW(TAG, "WiFi 初始化返回 %d", net_err);
    }
    if (radio_player_start() != 0) {
        ESP_LOGE(TAG, "收听任务启动失败");
    }

    // 载入电台列表(内置 + 用户自加),必须在建页之前。
    radio_pages_init();

    const appfw_ui_cfg_t ucfg = {
        .home_title = "网络收音机",
        .home_build = radio_pages_home_build,
        .home_poll  = radio_pages_home_poll,
        .home_key   = radio_pages_home_key,
        .info_rows  = radio_pages_info_rows,
        .app_config_html  = radio_pages_app_config_html,
        .app_config_apply = radio_pages_app_config_apply,
        .app_config_fill  = radio_pages_app_config_fill,
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
        .app_config_html  = radio_pages_app_config_html,
        .app_config_apply = radio_pages_app_config_apply,
        .app_config_fill  = radio_pages_app_config_fill,
        .on_httpd_ready   = portal_ready,
    };
    appfw_prov_configure(&pcfg);
    (void)appfw_portal_start();

    esp_timer_handle_t tick;
    const esp_timer_create_args_t ta = { .callback = second_tick_cb, .name = "tick" };
    if (esp_timer_create(&ta, &tick) == ESP_OK) esp_timer_start_periodic(tick, 1000000);

    ESP_LOGI(TAG, "启动完成:net=%d", net_err);
}
