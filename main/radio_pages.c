// main/radio_pages.c —— 见 radio_pages.h。
//
// 界面分工:框架负责顶栏(标题+电量)、设置菜单、WiFi/配网/设备信息子页;
// 应用只画主页——电台列表、光标、播放状态与正在播放的曲名。
#include "radio_pages.h"

#include <stdio.h>
#include <string.h>

#include "appfw_client.h"
#include "appfw_net.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "radio_player.h"
#include "radio_streams.h"

static const char *TAG = "radio_pages";

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_24);

#define LIST_MAX      5                       // 一屏行数
#define ROW_H         28
#define LIST_Y        50
#define EXTRA_ROWS    1                       // 列表末尾的「设置」行
#define NVS_LIST_KEY  "radio_stations"        // 用户自加电台(JSON 数组)

static radio_list_t s_list;
static int s_sel;                             // 选中下标 0..count-1,count=设置行
static int s_off;                             // 滚动窗口起始
static uint8_t s_vol = 55;

// 主页控件
static lv_obj_t *s_rows[LIST_MAX];
static lv_obj_t *s_extra_row;
static lv_obj_t *s_state_label;
static lv_obj_t *s_title_label;
static lv_obj_t *s_vol_label;

static lv_font_t s_f16, s_f24;
static bool s_font_ready;

static void ensure_fonts(void)
{
    if (s_font_ready) return;
    s_f16 = app_font_16;
    s_f16.fallback = &lv_font_montserrat_14;
    s_f24 = app_font_24;
    s_f24.fallback = &lv_font_montserrat_20;
    s_font_ready = true;
}

static void style(lv_obj_t *l, const lv_font_t *f, uint32_t color)
{
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

static void row_style(lv_obj_t *l, bool selected)
{
    lv_obj_remove_style_all(l);
    style(l, &s_f16, selected ? 0x0B1F16 : 0xC8D3DC);
    lv_obj_set_style_bg_color(l, lv_color_hex(selected ? 0x1E3A2C : 0x111820), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 6, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
}

static void show_row(lv_obj_t *l, const char *text)
{
    lv_label_set_text(l, text);
    lv_obj_set_hidden(l, false);
}

static void hide_row(lv_obj_t *l)
{
    lv_obj_set_hidden(l, true);
}

int radio_pages_cursor(void) { return s_sel; }

// ---------------------------------------------------------------- 列表

static int total_rows(void) { return (int)s_list.count + EXTRA_ROWS; }

// 把「用户自加电台」并入列表(同名覆盖)。
static void merge_user(const radio_list_t *user)
{
    for (uint8_t i = 0; i < user->count; i++) {
        if (!radio_list_add(&s_list, user->items[i].name, user->items[i].url)) {
            ESP_LOGW(TAG, "用户电台被丢弃(非法或已满): %s", user->items[i].name);
        }
    }
}

static void clamp_cursor(void)
{
    const int total = total_rows();
    if (s_sel >= total) s_sel = total - 1;
    if (s_sel < 0) s_sel = 0;
    if (s_off > (int)s_list.count - LIST_MAX) s_off = (int)s_list.count - LIST_MAX;
    if (s_off < 0) s_off = 0;
    if (s_sel < s_off) s_off = s_sel;
    if (s_sel >= s_off + LIST_MAX) s_off = s_sel - LIST_MAX + 1;
}

void radio_pages_home_build(lv_obj_t *page)
{
    ensure_fonts();
    for (int i = 0; i < LIST_MAX; i++) {
        s_rows[i] = lv_label_create(page);
        row_style(s_rows[i], false);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_rows[i], 212);
        lv_obj_set_pos(s_rows[i], 14, LIST_Y + i * ROW_H);
    }

    s_extra_row = lv_label_create(page);
    row_style(s_extra_row, false);
    lv_obj_set_width(s_extra_row, 212);
    lv_obj_set_pos(s_extra_row, 14, LIST_Y + LIST_MAX * ROW_H);

    s_state_label = lv_label_create(page);
    style(s_state_label, &s_f16, 0x8B98A5);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_state_label, 212);
    lv_obj_set_pos(s_state_label, 14, 214);
    lv_label_set_text(s_state_label, "按 OK 播放");

    s_title_label = lv_label_create(page);
    style(s_title_label, &s_f16, 0x7FD4A0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_label, 212);
    lv_obj_set_pos(s_title_label, 14, 238);
    lv_label_set_text(s_title_label, "");

    s_vol_label = lv_label_create(page);
    style(s_vol_label, &s_f16, 0x6E7A86);
    lv_obj_set_pos(s_vol_label, 14, 284);
    lv_label_set_text_fmt(s_vol_label, "音量 %u%%   长按上下键调整", s_vol);

    clamp_cursor();
}

static const char *state_text(const radio_player_snap_t *s)
{
    switch (s->state) {
    case RADIO_CONNECTING: return "正在连接…";
    case RADIO_PLAYING:    return "正在收听";
    case RADIO_ERROR:
        switch (s->err_code) {
        case RADIO_ERR_URL:     return "地址不合法";
        case RADIO_ERR_RESOLVE: return "解析失败,检查网络";
        case RADIO_ERR_CONNECT: return "连接失败";
        case RADIO_ERR_HTTP:    return "服务端拒绝";
        case RADIO_ERR_DECODE:  return "音频解码失败";
        case RADIO_ERR_TIMEOUT: return "流已断开";
        default:                 return "未知错误";
        }
    default: return "按 OK 播放";
    }
}

void radio_pages_home_poll(void)
{
    radio_player_snap_t s;
    radio_player_snapshot(&s);

    clamp_cursor();
    for (int i = 0; i < LIST_MAX; i++) {
        const int idx = s_off + i;
        if (idx >= (int)s_list.count) { hide_row(s_rows[i]); continue; }
        // LVGL 9 的 LV_SYMBOL_* 是字符串(不是单字符码),必须用 %s 拼。
        char text[RADIO_NAME_MAX + 24];
        const bool playing = (s.state == RADIO_PLAYING || s.state == RADIO_CONNECTING) &&
                             strcmp(s.station, s_list.items[idx].name) == 0;
        snprintf(text, sizeof(text), "%s %s%s",
                 (idx == s_sel) ? LV_SYMBOL_RIGHT : " ",
                 playing ? LV_SYMBOL_PLAY " " : "",
                 s_list.items[idx].name);
        show_row(s_rows[i], text);
        row_style(s_rows[i], idx == s_sel);
    }
    {
        char text[24];
        snprintf(text, sizeof(text), "%s %s",
                 s_sel == (int)s_list.count ? LV_SYMBOL_RIGHT : " ", "设置");
        show_row(s_extra_row, text);
        row_style(s_extra_row, s_sel == (int)s_list.count);
    }

    lv_label_set_text(s_state_label, state_text(&s));
    lv_label_set_text(s_title_label, s.title[0] ? s.title : "");
    lv_label_set_text_fmt(s_vol_label, "音量 %u%%   长按上下键调整", s.volume);
    const uint32_t col = (s.state == RADIO_ERROR) ? 0xE5484D
                       : (s.state == RADIO_PLAYING) ? 0x35C26B : 0x8B98A5;
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(col), 0);
}

// ---------------------------------------------------------------- 按键

static void toggle_station(int idx)
{
    if (idx < 0 || idx >= (int)s_list.count) return;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_STOPPED && s.state != RADIO_ERROR &&
        strcmp(s.station, s_list.items[idx].name) == 0) {
        radio_stop();
        return;
    }
    radio_play(s_list.items[idx].name, s_list.items[idx].url);
}

appfw_key_action_t radio_pages_home_key(int btn, int ev)
{
    const int total = total_rows();
    switch (ev) {
    case 0: // 单击
        if (btn == 0) { s_sel = (s_sel - 1 + total) % total; return APPFW_KEY_CONSUMED; }
        if (btn == 1) { s_sel = (s_sel + 1) % total; return APPFW_KEY_CONSUMED; }
        if (btn == 2) {
            if (s_sel == (int)s_list.count) return APPFW_KEY_MENU;  // 「设置」行
            toggle_station(s_sel);
            return APPFW_KEY_CONSUMED;
        }
        return APPFW_KEY_CONSUMED;
    case 3: // 长按
        if (btn == 0) { s_vol = s_vol >= 100 ? 0 : (uint8_t)(s_vol + 5); radio_set_volume(s_vol); return APPFW_KEY_CONSUMED; }
        if (btn == 1) { s_vol = s_vol <= 5 ? 100 : (uint8_t)(s_vol - 5); radio_set_volume(s_vol); return APPFW_KEY_CONSUMED; }
        return APPFW_KEY_CONSUMED;
    default:
        return APPFW_KEY_CONSUMED;  // 双击等无意义事件不惊动框架
    }
}

// ---------------------------------------------------------------- 持久化

// 与内置台完全一致(同名同址)的项不算用户自加。存进去会让门户把内置台
// 也列成"自加电台",还会把内置地址冻结住,以后改了内置源用户这边不会跟着变。
static bool is_builtin_exact(const char *name, const char *url)
{
    static radio_list_t b;   // 1KB,别放栈上:调用方可能在 httpd 任务里
    radio_list_builtin(&b);
    radio_list_builtin(&b);
    for (uint8_t i = 0; i < b.count; i++) {
        if (strcmp(b.items[i].name, name) == 0 && strcmp(b.items[i].url, url) == 0) return true;
    }
    return false;
}

// 用户自加电台以 JSON 数组存一条 NVS 记录;格式稳定,便于导出与迁移。
static void save_user(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    for (uint8_t i = 0; i < s_list.count; i++) {
        if (is_builtin_exact(s_list.items[i].name, s_list.items[i].url)) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "n", s_list.items[i].name);
        cJSON_AddStringToObject(o, "u", s_list.items[i].url);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (txt) {
        (void)appfw_store_set_str(NVS_LIST_KEY, txt);
        cJSON_free(txt);
    }
}

// 这几个局部量合计约 3KB,而 app_main 跑在 main 任务上,栈只有 3584 字节
// (CONFIG_ESP_MAIN_TASK_STACK_SIZE)。放栈上会直接触发 stack protection fault,
// 所以一律 static:这里本来就是启动期跑一次的初始化/刷新路径。
static void load_user(void)
{
    static radio_list_t builtin;
    static radio_list_t user;
    static char buf[1024];

    radio_list_builtin(&builtin);
    s_list = builtin;

    if (!appfw_store_get_str(NVS_LIST_KEY, buf, sizeof(buf)) || !buf[0]) return;
    cJSON *arr = cJSON_Parse(buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    radio_list_reset(&user);
    const cJSON *o = NULL;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "u");
        if (cJSON_IsString(n) && cJSON_IsString(u)) {
            (void)radio_list_add(&user, n->valuestring, u->valuestring);
        }
    }
    cJSON_Delete(arr);
    merge_user(&user);
    ESP_LOGI(TAG, "已载入 %u 个用户电台,合计 %u", user.count, s_list.count);
}

void radio_pages_init(void)
{
    s_sel = 0;
    s_off = 0;
    load_user();
}

// ---------------------------------------------------------------- 设备信息

int radio_pages_info_rows(char (*keys)[16], char (*vals)[72], int max)
{
    int n = 0;
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (n < max) {
        snprintf(keys[n], 16, "电台数");
        snprintf(vals[n], 72, "%u 个", (unsigned)s_list.count);
        n++;
    }
    if (n < max) {
        snprintf(keys[n], 16, "当前台");
        snprintf(vals[n], 72, "%s", s.station[0] ? s.station : "未选择");
        n++;
    }
    return n;
}

// ---------------------------------------------------------------- 门户配置

const char *radio_pages_app_config_html(void)
{
    return ""
    "<div class=\"card\"><h2>0 · 应用配置(网络收音机)</h2>\n"
    "<div style=\"margin:2px 0 8px;color:#9fb0bf;font-size:13px\">"
    "内置两台已实测可达的英文电台(KEXP / Radio Paradise)。"
    "当前网络下多数境外电台不可达,你可以在这里填自己的流地址。</div>\n"
    "<input type=\"text\" id=\"rname\" placeholder=\"电台名称(必填)\">\n"
    "<input type=\"text\" id=\"rurl\" placeholder=\"http://主机/流路径.mp3(必须 http://)\">\n"
    "<button onclick=\"radioAdd()\">添加</button>\n"
    "<div id=\"rmsg\"></div>\n"
    "<ul id=\"rlist\" style=\"padding-left:18px\"></ul>\n"
    "<div style=\"margin-top:10px;color:#9fb0bf;font-size:13px\">"
    "现在播放:<span id=\"rnow\">—</span>　"
    "<a href=\"#\" onclick=\"radioPlay(0);return false\">播放第 1 台</a> · "
    "<a href=\"#\" onclick=\"radioStop();return false\">停止</a></div>\n"
    "</div>\n"
    "<script>\n"
    "function radioMsg(s,e){const x=$('rmsg');if(x)x.innerHTML='<small class='+(e?'err':'ok')+'>'+s+'</small>';}\n"
    "const RNAMES=['未在收听','正在连接','正在收听','出错'];\n"
    "function radioNow(st){\n"
    "  if(!st||!st.station){$('rnow').textContent='—';return;}\n"
    "  $('rnow').textContent=esc((RNAMES[st.state]||'?')+' '+st.station+(st.title?' — '+st.title:''));\n"
    "}\n"
    "function radioRender(list){\n"
    "  const u=$('rlist');\n"
    "  u.innerHTML=(list||[]).map((s,i)=>'<li>'+esc(s.name)+' — <small>'+esc(s.url)+'</small> '\n"
    "     +'<a href=\"#\" onclick=\"radioPlay('+i+');return false\">播放</a> '\n"
    "     +'<a href=\"#\" onclick=\"radioDel('+i+');return false\">删除</a></li>').join('')||'<li>无自加电台</li>';\n"
    "}\n"
    "async function radioAdd(){\n"
    "  const name=$('rname').value.trim(),url=$('rurl').value.trim();\n"
    "  if(!name||!url){radioMsg('名称与地址都要填',1);return;}\n"
    "  if(url.indexOf('http://')!==0){radioMsg('只支持 http:// 开头(本机内存放不下 TLS)',1);return;}\n"
    "  const r=await jpost('/api/radio',{op:'add',name:name,url:url});\n"
    "  if(r.ok){radioMsg('已添加: '+name,0);$('rname').value='';$('rurl').value='';}\n"
    "  else radioMsg(r.error||'添加失败',1);\n"
    "  radioRender(r.stations);\n"
    "}\n"
    "async function radioDel(i){\n"
    "  if(!confirm('删除第 '+(i+1)+' 个自加电台?'))return;\n"
    "  const r=await jpost('/api/radio',{op:'del',index:i});\n"
    "  if(r.ok)radioMsg('已删除',0);else radioMsg(r.error||'删除失败',1);\n"
    "  radioRender(r.stations);\n"
    "}\n"
    "async function radioPlay(i){\n"
    "  const r=await jpost('/api/radio',{op:'play',index:i});\n"
    "  if(!r.ok)radioMsg(r.error||'播放失败',1);\n"
    "  radioNow(r.status); setTimeout(radioPoll,3000);\n"
    "}\n"
    "async function radioStop(){\n"
    "  const r=await jpost('/api/radio',{op:'stop'});\n"
    "  radioNow(r.status);\n"
    "}\n"
    "async function radioPoll(){\n"
    "  try{const r=await jpost('/api/radio',{op:'state'});radioNow(r.status);}catch(e){}\n"
    "  setTimeout(radioPoll,5000);\n"
    "}\n"
    "(async()=>{try{const s=await jget('/api/status');radioRender(s.stations);}catch(e){}})();\n"
    "radioPoll();\n"
    "</script>\n";
}

void radio_pages_app_config_fill(void *obj)
{
    cJSON *root = (cJSON *)obj;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    // 只回显用户自加的那些(与内置台完全同名的同址的不算)。
    for (uint8_t i = 0; i < s_list.count; i++) {
        if (is_builtin_exact(s_list.items[i].name, s_list.items[i].url)) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "n", s_list.items[i].name);
        cJSON_AddStringToObject(o, "u", s_list.items[i].url);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "stations", arr);
}

bool radio_pages_app_config_apply(void *root_obj)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive((cJSON *)root_obj, "stations");
    if (!cJSON_IsArray(arr)) return true;   // 没有该字段 = 不改电台

    // 同样不放栈上:这里由门户导入调用,跑在 httpd 任务里。
    static radio_list_t builtin;
    static radio_list_t user;
    radio_list_builtin(&builtin);
    radio_list_reset(&user);
    const cJSON *o = NULL;
    cJSON_ArrayForEach(o, arr) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "n");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(o, "u");
        if (!cJSON_IsString(n) || !cJSON_IsString(u)) continue;
        if (!radio_list_add(&user, n->valuestring, u->valuestring)) {
            ESP_LOGW(TAG, "导入时丢弃非法电台: %s", n->valuestring);
        }
    }

    s_list = builtin;
    merge_user(&user);
    save_user();
    s_sel = 0;
    s_off = 0;
    ESP_LOGI(TAG, "导入完成,共 %u 个电台", (unsigned)s_list.count);
    return true;
}

// ---------------------------------------------------------------- 私有端点

void radio_pages_restore_user(void)
{
    memset(&s_list, 0, sizeof(s_list));
    load_user();
}

// 回复里带上当前收听状态和剩余堆。堆是这台机器最紧的资源(C3 无 PSRAM,
// LVGL + WiFi + TLS 门户已占掉大半),播放时能从这里直接看出还剩多少。
static esp_err_t radio_stations_reply(httpd_req_t *req, bool ok, const char *err)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;
    cJSON_AddBoolToObject(root, "ok", ok);
    if (err) cJSON_AddStringToObject(root, "error", err);
    radio_pages_app_config_fill(root);

    radio_player_snap_t s;
    radio_player_snapshot(&s);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "station", s.station[0] ? s.station : "");
    cJSON_AddStringToObject(st, "title", s.title);
    cJSON_AddNumberToObject(st, "state", (int)s.state);
    cJSON_AddNumberToObject(st, "err", (int)s.err_code);
    cJSON_AddNumberToObject(st, "sample_rate", s.sample_rate);
    cJSON_AddNumberToObject(st, "channels", s.channels);
    cJSON_AddNumberToObject(st, "bitrate", s.bitrate);
    cJSON_AddNumberToObject(st, "volume", s.volume);
    cJSON_AddItemToObject(root, "status", st);
    cJSON_AddNumberToObject(root, "heap_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return ESP_OK;
}

static esp_err_t radio_api_handler(httpd_req_t *req)
{
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;

    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    bool ok = true;
    const char *err = NULL;

    if (!cJSON_IsString(op)) {
        ok = false; err = "缺少 op";
    } else if (strcmp(op->valuestring, "add") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (!cJSON_IsString(n) || !cJSON_IsString(u)) { ok = false; err = "缺少 name 或 url"; }
        else if (radio_list_add(&s_list, n->valuestring, u->valuestring)) {
            save_user();
        } else {
            ok = false; err = "地址不合法或列表已满(最多 8 个)";
        }
    } else if (strcmp(op->valuestring, "del") == 0) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "index");
        if (!cJSON_IsNumber(i)) { ok = false; err = "缺少 index"; }
        else if (radio_list_remove(&s_list, (uint8_t)i->valueint)) {
            save_user();
        } else { ok = false; err = "下标越界"; }
    } else if (strcmp(op->valuestring, "play") == 0) {
        // 电台本来就在这个页面上管理,顺手也能开播/停播:调试时不用守在机器前按键。
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "index");
        if (!cJSON_IsNumber(i)) { ok = false; err = "缺少 index"; }
        else if (i->valueint < 0 || i->valueint >= (int)s_list.count) { ok = false; err = "下标越界"; }
        else {
            radio_play(s_list.items[i->valueint].name, s_list.items[i->valueint].url);
        }
    } else if (strcmp(op->valuestring, "stop") == 0) {
        radio_stop();
    } else if (strcmp(op->valuestring, "state") == 0) {
        // 纯查询,不改任何东西。
    } else {
        ok = false; err = "未知 op";
    }

    cJSON_Delete(root);
    return radio_stations_reply(req, ok, err);
}

bool radio_pages_portal_register(void *httpd)
{
    httpd_handle_t h = (httpd_handle_t)httpd;
    static const httpd_uri_t uri = {
        .uri = "/api/radio", .method = HTTP_POST, .handler = radio_api_handler,
    };
    return httpd_register_uri_handler(h, &uri) == ESP_OK;
}
