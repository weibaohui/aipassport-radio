// tests/test_appfw_mcp.c —— MCP 协议核(appfw_mcp_handle 分发)的主机测试。
//
// MCP 搬进常驻极简 TCP 服务后,协议核与传输解耦,主机上就能钉死分发语义:
// initialize 版本协商、tools/list 注册表顺序与 schema 透传、tools/call 的
// 成功/失败/未知工具、通知回 202、解析错误 -32700、内置工具按使能位挂载。
#include "appfw_mcp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_called;
static int fake_echo(cJSON *args, appfw_mcp_resp_t *resp)
{
    g_called++;
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(args, "n");
    appfw_mcp_resp_addf(resp, "fake ok n=%d",
                        cJSON_IsNumber(n) ? n->valueint : -1);
    return 0;
}
static int fake_fail(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_mcp_resp_addf(resp, "boom");
    return 7;
}

static const appfw_mcp_tool_t FAKE[] = {
    { "fake_echo", "test tool",
      "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}",
      fake_echo },
    { "fake_fail", "test tool", "{}", fake_fail },
};

static cJSON *call(const char *body, int want_status)
{
    int st = -1;
    cJSON *r = appfw_mcp_handle(body, strlen(body), &st);
    assert(st == want_status);
    return r;
}

static cJSON *result_of(cJSON *r)
{
    cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "result");
    assert(res != NULL);
    return res;
}

static const cJSON *err_code_of(cJSON *r)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "error");
    assert(e != NULL);
    return cJSON_GetObjectItemCaseSensitive(e, "code");
}

int main(void)
{
    appfw_mcp_set_tools(FAKE, (int)(sizeof(FAKE) / sizeof(FAKE[0])));
    appfw_mcp_set_builtin_tools(0);
    assert(appfw_mcp_tool_count() == 2);

    // initialize:回显客户端协议版本 + 服务器信息
    cJSON *r = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    "\"params\":{\"protocolVersion\":\"2024-11-05\"}}", 200);
    assert(result_of(r)->valuestring == NULL);
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(result_of(r), "protocolVersion");
    assert(cJSON_IsString(pv) && strcmp(pv->valuestring, "2024-11-05") == 0);
    cJSON *info = cJSON_GetObjectItemCaseSensitive(result_of(r), "serverInfo");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(info, "name")->valuestring,
                  "ai-passport") == 0);
    cJSON_Delete(r);

    // initialize 不带版本:回默认版本
    r = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{}}",
             200);
    pv = cJSON_GetObjectItemCaseSensitive(result_of(r), "protocolVersion");
    assert(cJSON_IsString(pv) && strcmp(pv->valuestring, "2025-03-26") == 0);
    cJSON_Delete(r);

    // tools/list:应用工具在前,schema 字符串原样 parse 进 inputSchema
    r = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}", 200);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result_of(r), "tools");
    assert(cJSON_GetArraySize(tools) == 2);
    cJSON *t0 = cJSON_GetArrayItem(tools, 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(t0, "name")->valuestring,
                  "fake_echo") == 0);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(t0, "inputSchema");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(schema, "type")->valuestring,
                  "object") == 0);
    cJSON_Delete(r);

    // tools/call:成功 / 失败(isError)/ 未知工具
    r = call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"fake_echo\",\"arguments\":{\"n\":41}}}", 200);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result_of(r), "content");
    cJSON *item = cJSON_GetArrayItem(content, 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(item, "text")->valuestring,
                  "fake ok n=41") == 0);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result_of(r), "isError")));
    assert(g_called == 1);
    cJSON_Delete(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"fake_fail\",\"arguments\":{}}}", 200);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result_of(r), "isError")));
    cJSON_Delete(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"nope\",\"arguments\":{}}}", 200);
    assert(err_code_of(r)->valueint == -32602);
    cJSON_Delete(r);

    // 未知方法 / 缺 method / 坏 JSON
    r = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bogus\"}", 200);
    assert(err_code_of(r)->valueint == -32601);
    cJSON_Delete(r);
    r = call("{\"jsonrpc\":\"2.0\",\"id\":8}", 200);
    assert(err_code_of(r)->valueint == -32601);
    cJSON_Delete(r);
    r = call("this is not json", 200);
    assert(err_code_of(r)->valueint == -32700);
    cJSON_Delete(r);

    // 通知(无 id):无应答体,202
    r = call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", 202);
    assert(r == NULL);
    // ping:空 result 对象
    r = call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}", 200);
    assert(cJSON_GetArraySize(result_of(r)) == 0);
    cJSON_Delete(r);

    // 内置工具按使能位挂载:bit0+bit2 → 刷新周期 + wifi_status + wifi_connect_saved,
    // 排在应用工具之后;bit5(AI 管理信息页)不挂任何工具
    appfw_mcp_set_builtin_tools(0b0101);
    assert(appfw_mcp_tool_count() == 2 + 3);
    r = call("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\"}", 200);
    tools = cJSON_GetObjectItemCaseSensitive(result_of(r), "tools");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools, 2),
                  "name")->valuestring, "set_refresh_period") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools, 3),
                  "name")->valuestring, "wifi_status") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools, 4),
                  "name")->valuestring, "wifi_connect_saved") == 0);
    cJSON_Delete(r);

    printf("全部通过\n");
    return 0;
}
