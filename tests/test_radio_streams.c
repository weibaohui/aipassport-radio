// tests/test_radio_streams.c —— 电台列表与 URL 解析的主机测试(无 ESP/LVGL 依赖)。
//
// 编译:cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_radio_streams.c \
//          main/radio_streams.c -o t && ./t
#include "radio_streams.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 内置台数量。改动 main/radio_streams.c 的 BUILTIN 数组时必须同步改这里,
// 否则门禁会在这里 abort —— 这是刻意的:台数变了却没同步测试,说明有人
// 忘了确认新台是不是都通过了下面的合法性检查。
#define BUILTIN_N 6

static void test_builtin_list(void)
{
    radio_list_t l;
    radio_list_builtin(&l);
    assert(l.count == BUILTIN_N);
    assert(l.count > 0 && l.count <= RADIO_MAX_STATIONS);
    // 内置台必须能通过合法性检查,否则设备开箱即用就是坏的
    for (uint8_t i = 0; i < l.count; i++) {
        assert(strstr(l.items[i].url, "http://") == l.items[i].url);
        assert(radio_url_valid(l.items[i].url));
        assert(l.items[i].name[0] != '\0');
        // 台名会进 lv_label_set_text(),超长会被截断成看不懂的东西
        assert(strlen(l.items[i].name) < RADIO_NAME_MAX);
    }
    printf("  builtin: %u stations, all names/URLs valid\n", l.count);
}

static void test_url_valid(void)
{
    assert(radio_url_valid("http://a.com/s"));
    assert(radio_url_valid("http://192.168.1.9:8000/stream"));
    assert(radio_url_valid("http://host"));                 // 无路径
    assert(radio_url_valid("http://host/"));

    assert(!radio_url_valid(NULL));
    assert(!radio_url_valid(""));
    assert(!radio_url_valid("https://a.com/s"));            // 只收 http
    assert(!radio_url_valid("ftp://a.com/s"));
    assert(!radio_url_valid("http://"));                    // 无 host
    assert(!radio_url_valid("http:///path"));
    assert(!radio_url_valid("http://has space/s"));
    assert(!radio_url_valid("http://has\ttab/s"));
    assert(!radio_url_valid("http://user@host/s"));         // 不接受内嵌凭据
    assert(!radio_url_valid("http://host?a=1"));            // query 紧跟 host(无路径)→ 拒绝
    assert(radio_url_valid("http://host/s?a=1"));           // 路径后的 query 允许
    assert(radio_url_valid("http://host/s?token=abc&x=1"));
    char toolong[RADIO_URL_MAX + 8];
    memset(toolong, 'a', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    memcpy(toolong, "http://", 7);
    assert(!radio_url_valid(toolong));                      // 超长
    printf("  url_valid: accept/reject cases OK\n");
}

static void test_hostport(void)
{
    char hp[RADIO_HOST_MAX];

    assert(radio_url_hostport("http://a.com/s", hp, sizeof(hp)));
    assert(strcmp(hp, "a.com") == 0);

    assert(radio_url_hostport("http://a.com", hp, sizeof(hp)));
    assert(strcmp(hp, "a.com") == 0);

    assert(radio_url_hostport("http://a.com:8000/x", hp, sizeof(hp)));
    assert(strcmp(hp, "a.com:8000") == 0);

    assert(radio_url_hostport("http://192.168.0.9:8000/s", hp, sizeof(hp)));
    assert(strcmp(hp, "192.168.0.9:8000") == 0);

    assert(!radio_url_hostport("https://a.com/s", hp, sizeof(hp)));
    assert(radio_url_hostport("http://a.com:1/s", hp, sizeof(hp)));
    assert(strcmp(hp, "a.com:1") == 0);
    assert(radio_url_hostport("http://a.com:65535/s", hp, sizeof(hp)));
    assert(strcmp(hp, "a.com:65535") == 0);

    assert(!radio_url_hostport("http://a.com:80x/s", hp, sizeof(hp)));  // 端口非数字
    assert(!radio_url_hostport("http://a.com:/s", hp, sizeof(hp)));     // 空端口
    assert(!radio_url_hostport(NULL, hp, sizeof(hp)));
    assert(hp[0] == '\0');                                            // 失败时清空

    // 两个内置台都要能解析
    radio_list_t l;
    radio_list_builtin(&l);
    for (uint8_t i = 0; i < l.count; i++) {
        assert(radio_url_hostport(l.items[i].url, hp, sizeof(hp)));
        assert(hp[0] != '\0');
    }
    printf("  hostport: parse cases OK\n");
}

static void test_list_ops(void)
{
    radio_list_t l;
    radio_list_reset(&l);
    assert(l.count == 0);
    assert(radio_list_find(&l, "x") == -1);
    assert(!radio_list_remove(&l, 0));                     // 空表删除

    assert(radio_list_add(&l, "A", "http://a.com/1"));
    assert(radio_list_add(&l, "B", "http://b.com/2"));
    assert(l.count == 2);
    assert(radio_list_find(&l, "B") == 1);

    // 同名=改 URL,不新增
    assert(radio_list_add(&l, "A", "http://a.com/new"));
    assert(l.count == 2);
    assert(strcmp(l.items[0].url, "http://a.com/new") == 0);

    // 非法输入一律拒绝
    assert(!radio_list_add(&l, "", "http://c.com/"));
    assert(!radio_list_add(&l, "C", "https://c.com/"));
    assert(!radio_list_add(&l, "C", NULL));
    char longname[RADIO_NAME_MAX + 4];
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    assert(!radio_list_add(&l, longname, "http://c.com/"));
    assert(l.count == 2);

    // 删中间项后整体前移,尾项清零
    assert(radio_list_remove(&l, 0));
    assert(l.count == 1);
    assert(strcmp(l.items[0].name, "B") == 0);
    assert(l.items[1].name[0] == '\0');
    assert(l.items[1].url[0] == '\0');

    // 容量上限
    radio_list_reset(&l);
    for (int i = 0; i < RADIO_MAX_STATIONS; i++) {
        char n[8], u[32];
        snprintf(n, sizeof(n), "S%d", i);
        snprintf(u, sizeof(u), "http://h%d.com/s", i);
        assert(radio_list_add(&l, n, u));
    }
    assert(l.count == RADIO_MAX_STATIONS);
    assert(!radio_list_add(&l, "OVER", "http://over.com/s"));
    assert(l.count == RADIO_MAX_STATIONS);
    printf("  list ops: add/update/remove/capacity OK\n");
}

int main(void)
{
    printf("test_radio_streams\n");
    test_builtin_list();
    test_url_valid();
    test_hostport();
    test_list_ops();
    printf("all checks passed\n");
    return 0;
}
