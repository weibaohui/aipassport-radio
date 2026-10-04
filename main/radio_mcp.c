// main/radio_mcp.c —— 收音机的 MCP 工具表:把点播/搜台/音量/状态开放给
// 局域网里的 AI 宿主。协议壳在框架(appfw_mcp),本文件只有"工具做什么"。
// 工具跑在 MCP 服务的 ai_mcp 任务上下文:radio_play/radio_stop 本就线程
// 安全;清单读写沿用 store 的按需路径。当前台从播放器快照实时反查,
// 与按键/门户/MCP 三条播放路径天然一致,不另存会话状态。
#include "radio_mcp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "appfw_mcp.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_storage.h"
#include "radio_player.h"
#include "radio_store.h"

static const char *TAG = "radio_mcp";

// 当前播放中的台在清单里的下标(未在播 -1)。
static int current_index(void)
{
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_PLAYING && s.state != RADIO_CONNECTING) return -1;
    if (!s.station[0]) return -1;
    return radio_store_find(s.station);
}

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
    appfw_mcp_resp_addf(resp, "已切台:第 %d 台 %s(连接中,几秒后 get_state 确认)",
                        one, st.name);
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
    appfw_mcp_resp_addf(resp, "已切台:第 %d 台 %s(连接中,几秒后 get_state 确认)",
                        idx + 1, st.name);
    return 0;
}

static int tool_next(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const int total = radio_store_count();
    if (total <= 0) { appfw_mcp_resp_addf(resp, "清单为空"); return 1; }
    const int cur = current_index();
    const int idx = (cur < 0) ? 0 : (cur + 1) % total;   // 没在播:从第一台开始
    radio_station_t st;
    if (!radio_store_get(idx, &st)) {
        appfw_mcp_resp_addf(resp, "台目读取失败");
        return 1;
    }
    radio_play(st.name, st.url);
    appfw_mcp_resp_addf(resp, "已切台:第 %d 台 %s(连接中,几秒后 get_state 确认)",
                        idx + 1, st.name);
    return 0;
}

static int tool_prev(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const int total = radio_store_count();
    if (total <= 0) { appfw_mcp_resp_addf(resp, "清单为空"); return 1; }
    const int cur = current_index();
    const int idx = (cur < 0) ? total - 1 : (cur + total - 1) % total;
    radio_station_t st;
    if (!radio_store_get(idx, &st)) {
        appfw_mcp_resp_addf(resp, "台目读取失败");
        return 1;
    }
    radio_play(st.name, st.url);
    appfw_mcp_resp_addf(resp, "已切台:第 %d 台 %s(连接中,几秒后 get_state 确认)",
                        idx + 1, st.name);
    return 0;
}

static int tool_random(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const int total = radio_store_count();
    if (total <= 0) { appfw_mcp_resp_addf(resp, "清单为空"); return 1; }
    const int cur = current_index();
    int idx = (int)(esp_random() % (unsigned)total);
    if (cur >= 0 && total > 1) {                         // 避开当前台
        while (idx == cur) idx = (int)(esp_random() % (unsigned)total);
    }
    radio_station_t st;
    if (!radio_store_get(idx, &st)) {
        appfw_mcp_resp_addf(resp, "台目读取失败");
        return 1;
    }
    radio_play(st.name, st.url);
    appfw_mcp_resp_addf(resp, "已切台:第 %d 台 %s(连接中,几秒后 get_state 确认)",
                        idx + 1, st.name);
    return 0;
}

static int tool_stop(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    radio_stop();
    appfw_mcp_resp_addf(resp, "已停止");
    return 0;
}

static int tool_pause(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_PLAYING) {
        appfw_mcp_resp_addf(resp, "没有正在播放的电台,无需暂停");
        return 1;
    }
    radio_player_pause();
    appfw_mcp_resp_addf(resp, "已暂停(连接保持,取流不断;用 resume 恢复)");
    return 0;
}

static int tool_resume(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    if (!radio_player_resume()) {
        appfw_mcp_resp_addf(resp, "没有可恢复的播放(不在暂停态)");
        return 1;
    }
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    appfw_mcp_resp_addf(resp, "已恢复:%s", s.station);
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
    appfw_mcp_resp_addf(resp, " | 地址 %s", s.url[0] ? s.url : "--");
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

// ---- 自定义清单(NVS):单条即生效,台目段只读 ----
static int tool_add_station(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(args, "name");
    const cJSON *uu = cJSON_GetObjectItemCaseSensitive(args, "url");
    if (!cJSON_IsString(nm) || !nm->valuestring[0] ||
        !cJSON_IsString(uu) || !uu->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 name/url(string)缺失");
        return 1;
    }
    if (!radio_store_add(nm->valuestring, uu->valuestring)) {
        appfw_mcp_resp_addf(resp, "添加失败:url 必须是 http(s) 直链且短于 %d 字节,"
                                  "名称不含控制字符;或自定义清单已满(上限 %d)",
                            RADIO_URL_MAX, RADIO_MAX_STATIONS);
        return 1;
    }
    appfw_mcp_resp_addf(resp, "已加入 %s(自定义共 %d 台)", nm->valuestring,
                        radio_store_count() - radio_store_catalog_count());
    return 0;
}

static int tool_remove_station(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(args, "index");
    if (!cJSON_IsNumber(idx)) {
        appfw_mcp_resp_addf(resp, "参数 index(int,从 1 开始)缺失");
        return 1;
    }
    const int one = idx->valueint;
    const int total = radio_store_count();
    if (one < 1 || one > total) {
        appfw_mcp_resp_addf(resp, "下标 %d 不存在(有效范围 1-%d)", one, total);
        return 1;
    }
    if (one <= radio_store_catalog_count()) {
        appfw_mcp_resp_addf(resp, "第 %d 台是内置台目,不可删除", one);
        return 1;
    }
    radio_station_t st;
    if (!radio_store_get(one - 1, &st) || !radio_store_remove(one - 1)) {
        appfw_mcp_resp_addf(resp, "删除失败");
        return 1;
    }
    appfw_mcp_resp_addf(resp, "已删除 %s(自定义剩 %d 台)", st.name,
                        total - 1 - radio_store_catalog_count());
    return 0;
}

static int tool_clear_custom(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    radio_store_import_begin();
    appfw_mcp_resp_addf(resp, "已清空自定义电台,回到内置 %d 台",
                        radio_store_catalog_count());
    return 0;
}

// ---- WiFi 增强(WIFI 使能才挂载):添加/列出/删除已存热点 ----
static int tool_wifi_add(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    const cJSON *pwd = cJSON_GetObjectItemCaseSensitive(args, "password");
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(args, "connect_now");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 ssid(string)缺失");
        return 1;
    }
    const char *pwd_s = cJSON_IsString(pwd) ? pwd->valuestring : "";

    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    (void)appfw_store_netlist_load(&list);
    if (!appfw_netlist_add(&list, ssid->valuestring, pwd_s)) {
        appfw_mcp_resp_addf(resp, "添加失败:列表已满或参数过长");
        return 1;
    }
    // 新加的设为首选(开机自动连接时优先试它)
    (void)appfw_netlist_select(&list, ssid->valuestring);
    (void)appfw_store_netlist_save(&list);
    appfw_net_reload_config();
    if (cJSON_IsTrue(now)) {
        appfw_net_connect_ssid(ssid->valuestring);
        appfw_mcp_resp_addf(resp, "已添加热点 %s 并正在连接,稍后用 wifi_status 查询结果",
                            ssid->valuestring);
    } else {
        appfw_mcp_resp_addf(resp, "已添加热点 %s(未连接)", ssid->valuestring);
    }
    return 0;
}

static int tool_wifi_list_saved(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list) || list.count == 0) {
        appfw_mcp_resp_addf(resp, "没有已保存的热点");
        return 0;
    }
    appfw_mcp_resp_addf(resp, "已保存 %d 个热点:", list.count);
    for (int i = 0; i < list.count; i++) {
        const bool open = list.items[i].pwd[0] == '\0';
        appfw_mcp_resp_addf(resp, "\n%d. %s%s", i + 1, list.items[i].ssid,
                            open ? "(开放网络)" : "");
    }
    return 0;
}

static int tool_wifi_remove(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 ssid(string)缺失");
        return 1;
    }
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) {
        appfw_mcp_resp_addf(resp, "没有已保存的热点");
        return 1;
    }
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].ssid, ssid->valuestring) == 0) {
            memmove(&list.items[i], &list.items[i + 1],
                    sizeof(list.items[0]) * (size_t)(list.count - i - 1));
            list.count--;
            (void)appfw_store_netlist_save(&list);
            appfw_net_reload_config();
            appfw_mcp_resp_addf(resp, "已删除热点 %s", ssid->valuestring);
            return 0;
        }
    }
    appfw_mcp_resp_addf(resp, "%s 不在已保存列表里", ssid->valuestring);
    return 1;
}

static const appfw_mcp_tool_t TOOLS[] = {
    { "play_index", "按下标播放电台(1 起的全清单编号,1-6 是内置精品台;编号用 search_stations/list_stations 查)",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_play_index },
    { "play_name", "按确切台名播放;不确定名字先 search_stations",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
      tool_play_name },
    { "play_next", "播放下一台(到尾部绕回第一台;没在播则从第一台开始)",
      "{}", tool_next },
    { "play_prev", "播放上一台(到头部绕回最后一台)",
      "{}", tool_prev },
    { "play_random", "随机播放一台(避开当前台)",
      "{}", tool_random },
    { "search_stations", "按关键词模糊搜台名,返回匹配的下标与名字",
      "{\"type\":\"object\",\"properties\":{\"keyword\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"keyword\"]}",
      tool_search },
    { "list_stations", "分页列出电台(内置在前)",
      "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"integer\"},\"count\":{\"type\":\"integer\"}}}",
      tool_list },
    { "stop", "停止播放(断开连接;区别于 pause 的保持连接)",
      "{}", tool_stop },
    { "pause", "暂停播放:保持连接与取流,不出声;只在正在播放时执行",
      "{}", tool_pause },
    { "resume", "从暂停恢复播放;不在暂停态则不执行(返回失败)",
      "{}", tool_resume },
    { "set_volume", "设置音量 0-100(存 NVS,重启保持)",
      "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\"}},\"required\":[\"level\"]}",
      tool_volume },
    { "get_state", "查询播放状态(台名/流地址/采样率/音量/错误)",
      "{}", tool_state },
    { "playlist_add_station", "向自定义清单追加一台(台名+流媒体地址),单条即生效;同名已存在则改为新地址;内置台目不受影响",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"}},\"required\":[\"name\",\"url\"]}",
      tool_add_station },
    { "playlist_remove_station", "从自定义清单删除一台(1 起的下标;1-345 是内置台目,不可删)",
      "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"]}",
      tool_remove_station },
    { "playlist_clear_custom", "清空全部自定义电台,回到内置台目(慎用)",
      "{}", tool_clear_custom },
    { "wifi_add_hotspot", "添加新热点(名称+密码);connect_now=true 立即连接",
      "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"connect_now\":{\"type\":\"boolean\"}},\"required\":[\"ssid\"]}",
      tool_wifi_add },
    { "wifi_list_saved", "列出已保存的热点",
      "{}", tool_wifi_list_saved },
    { "wifi_remove_hotspot", "从已保存列表删除一个热点",
      "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"}},\"required\":[\"ssid\"]}",
      tool_wifi_remove },
};

void radio_mcp_init(void)
{
    appfw_mcp_set_tools(TOOLS, (int)(sizeof(TOOLS) / sizeof(TOOLS[0])));
    ESP_LOGI(TAG, "MCP 工具已注册:%d 个", (int)(sizeof(TOOLS) / sizeof(TOOLS[0])));
}
