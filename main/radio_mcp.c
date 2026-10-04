// main/radio_mcp.c —— 收音机的 MCP 工具表:把点播/搜台/音量/状态开放给
// 局域网里的 AI 宿主。协议壳在框架(appfw_mcp),本文件只有"工具做什么"。
// 工具跑在门户 httpd 任务上下文:radio_play/radio_stop 本就线程安全;
// 清单读取沿用 store 的按需挂载路径。
#include "radio_mcp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "appfw_biglist.h"
#include "appfw_mcp.h"
#include "appfw_storage.h"
#include "radio_player.h"
#include "radio_store.h"

static const char *TAG = "radio_mcp";

static int tool_play_index(cJSON *args, appfw_mcp_resp_t *resp)
{
    // 与 list/search 显示一致:1 = 第 1 台(内置精品第一行)。
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!cJSON_IsNumber(idx)) {
        appfw_mcp_resp_addf(resp, "参数 index(int,从 1 开始)缺失");
        return 1;
    }
    const int one = idx->valueint;
    const int total = radio_store_count();
    radio_station_t st;
    if (one < 1 || one > total || !radio_store_get(one - 1, &st)) {
        appfw_mcp_resp_addf(resp, "下标 %d 不存在(有效范围 1-%d)",
                            one, total);
        return 1;
    }
    radio_play(st.name, st.url);
    appfw_mcp_resp_addf(resp, "正在播放第 %d 台:%s", one, st.name);
    return 0;
}

static int tool_play_name(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (!cJSON_IsString(n) || !n->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 name(string)缺失");
        return 1;
    }
    const int idx = radio_store_find(n->valuestring);
    if (idx < 0) {
        appfw_mcp_resp_addf(resp, "没找到叫「%s」的台;可先用 search_stations 模糊搜",
                            n->valuestring);
        return 1;
    }
    radio_station_t st;
    if (!radio_store_get(idx, &st)) {
        appfw_mcp_resp_addf(resp, "台目读取失败");
        return 1;
    }
    radio_play(st.name, st.url);
    appfw_mcp_resp_addf(resp, "正在播放第 %d 台:%s", idx + 1, st.name);
    return 0;
}

static int tool_stop(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    radio_stop();
    appfw_mcp_resp_addf(resp, "已停止");
    return 0;
}

static int tool_volume(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *lv = cJSON_GetObjectItemCaseSensitive(args, "level");
    if (!cJSON_IsNumber(lv) || lv->valueint < 0 || lv->valueint > 100) {
        appfw_mcp_resp_addf(resp, "参数 level(int 0-100)缺失或越界");
        return 1;
    }
    radio_set_volume((uint8_t)lv->valueint);
    (void)appfw_store_set_u16("opt_volume", (uint16_t)lv->valueint);
    appfw_mcp_resp_addf(resp, "音量已设为 %d%%", lv->valueint);
    return 0;
}

static int tool_state(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    static const char *K[] = { "未在收听", "正在连接", "正在收听", "已暂停", "出错" };
    appfw_mcp_resp_addf(resp, "%s %s | 采样率 %u Hz %u 声道 | 音量 %u%% | 空闲堆 %u",
                        K[s.state & 3], s.station,
                        (unsigned)s.sample_rate, (unsigned)s.channels,
                        (unsigned)s.volume,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (s.state == RADIO_ERROR) {
        static const char *E[] = { "", "地址不合法", "解析失败", "连接失败",
                                   "服务端拒绝", "解码失败", "流已断" };
        const int e = s.err_code < 1 || s.err_code > 6 ? 0 : s.err_code;
        appfw_mcp_resp_addf(resp, " | 错误:%s", E[e]);
    }
    return 0;
}

static int tool_list(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(args, "from");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, "count");
    int from = cJSON_IsNumber(f) ? f->valueint : 0;
    int count = cJSON_IsNumber(c) ? c->valueint : 10;
    if (count < 1 || count > 50) count = 10;
    const int total = radio_store_count();
    if (from < 0) from = 0;
    if (from >= total) {
        appfw_mcp_resp_addf(resp, "起点越界:共 %d 台", total);
        return 1;
    }
    appfw_mcp_resp_addf(resp, "共 %d 台,第 %d 台起:", total, from + 1);
    radio_station_t st;
    for (int i = from; i < total && count > 0; i++, count--) {
        if (!radio_store_get(i, &st)) continue;
        appfw_mcp_resp_addf(resp, "\n%d. %s", i + 1, st.name);
    }
    return 0;
}

static int tool_search(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(args, "keyword");
    if (!cJSON_IsString(k) || !k->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 keyword(string)缺失");
        return 1;
    }
    int limit = 10;
    const cJSON *lm = cJSON_GetObjectItemCaseSensitive(args, "limit");
    if (cJSON_IsNumber(lm) && lm->valueint > 0 && lm->valueint <= 50) limit = lm->valueint;

    const int total = radio_store_count();
    radio_station_t st;
    int found = 0;
    for (int i = 0; i < total && found < limit; i++) {
        if (!radio_store_get(i, &st)) continue;
        if (strstr(st.name, k->valuestring) == NULL) continue;
        appfw_mcp_resp_addf(resp, "%s%d. %s", found ? "\n" : "", i + 1, st.name);
        found++;
    }
    appfw_mcp_resp_addf(resp, found ? "" : "没有名字含「%s」的台", k->valuestring);
    return 0;
}

static int tool_delete_playlist(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_biglist_discard();
    appfw_mcp_resp_addf(resp, "已删除上传的清单文件,设备回到内置 339 台");
    return 0;
}

static int tool_device_info(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const esp_app_desc_t *app = esp_app_get_description();
    appfw_mcp_resp_addf(resp,
        "固件 %s %s | 空闲堆 %uKB(最大块 %uKB) | 运行 %u 分钟 | 清单 %d 台",
        app->project_name, app->version,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(esp_timer_get_time() / 60000000LL),
        radio_store_count());
    return 0;
}

static int tool_screen_off(cJSON *args, appfw_mcp_resp_t *resp)
{
    // 框架能力:熄屏时间(秒;0=永不)。合法档位与设置菜单一致。
    static const uint16_t OKV[] = { 0, 60, 300, 600, 900, 1800 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "seconds");
    if (!cJSON_IsNumber(v)) {
        uint16_t cur = 0;
        (void)appfw_store_get_screen_off(&cur);
        appfw_mcp_resp_addf(resp, "当前熄屏时间 %u 秒(0=永不);可设 0/60/300/600/900/1800",
                            (unsigned)cur);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(OKV) / sizeof(OKV[0]); i++) {
        if (OKV[i] == (uint16_t)v->valueint) {
            (void)appfw_store_set_screen_off(OKV[i]);
            appfw_mcp_resp_addf(resp, "熄屏时间已设为 %d 秒%s",
                                v->valueint, v->valueint == 0 ? "(永不)" : "");
            return 0;
        }
    }
    appfw_mcp_resp_addf(resp, "非法值 %d:可选 0/60/300/600/900/1800", v->valueint);
    return 1;
}

static const appfw_mcp_tool_t TOOLS[] = {
    { "play_index", "按下标播放电台(下标 1-6 是内置精品台)",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_play_index },
    { "play_name", "按确切台名播放;不确定名字先 search_stations",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
      tool_play_name },
    { "search_stations", "按关键词模糊搜台名,返回匹配的下标与名字",
      "{\"type\":\"object\",\"properties\":{\"keyword\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"keyword\"]}",
      tool_search },
    { "list_stations", "分页列出电台(内置在前)",
      "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"integer\"},\"count\":{\"type\":\"integer\"}}}",
      tool_list },
    { "stop", "停止播放", "{}", tool_stop },
    { "set_volume", "设置音量 0-100",
      "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\"}},\"required\":[\"level\"]}",
      tool_volume },
    { "get_state", "查询播放状态(台名/采样率/音量/错误)",
      "{}", tool_state },
    { "get_device_info", "查询设备信息(固件版本/内存/运行时长/电台数)",
      "{}", tool_device_info },
    { "set_screen_off", "设置屏幕熄屏时间(秒;0=永不;不带参数=查询当前值)",
      "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\"}}}", tool_screen_off },
    { "delete_playlist_file", "删除上传的清单文件,回到内置 339 台(慎用)",
      "{}", tool_delete_playlist },
};

void radio_mcp_init(void)
{
    appfw_mcp_set_tools(TOOLS, (int)(sizeof(TOOLS) / sizeof(TOOLS[0])));
    ESP_LOGI(TAG, "MCP 工具已注册:%d 个", (int)(sizeof(TOOLS) / sizeof(TOOLS[0])));
}
