// tests/test_appfw_biglist.c —— 大清单(files 分区 m3u + 偏移索引)的主机测试。
//
// appfw_biglist 的文件操作全是 POSIX stdio,主机上直接跑真文件(临时目录),
// 钉死这些行为:索引重建的收录规则(EXTINF 配对/只收 http/https/注释与裸
// URL 忽略)、偏移在长行/空行/CRLF 下仍然对得上、get 按需读单条、find 线性
// 找名、换文件自动重建、索引损坏自愈、discard 删文件退回、UTF-8 安全截断。
#include "appfw_biglist.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// 台目兜底的假目录:两条固定条目,验证无文件时的 rodata 回退路径。
static const appfw_biglist_entry_t FAKE_CAT[] = {
    { "台目一", "http://cat.example/1" },
    { "台目二", "https://cat.example/2" },
};
static int fake_count(void) { return (int)(sizeof(FAKE_CAT) / sizeof(FAKE_CAT[0])); }
static bool fake_get(int idx, appfw_biglist_entry_t *out)
{
    if (idx < 0 || idx >= fake_count()) return false;
    *out = FAKE_CAT[idx];
    return true;
}

static char g_dir[128];

static void make_dir(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/radio-biglist-%d", (int)getpid());
    (void)system("rm -rf /tmp/radio-biglist-*");
    assert(mkdir(g_dir, 0755) == 0);
    appfw_biglist_set_dir(g_dir);
    appfw_biglist_init();
}

static void write_m3u(const char *text)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/radio.m3u", g_dir);
    FILE *f = fopen(p, "wb");
    assert(f);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}


// 稳定性闸门要求连续两轮 poll 一致才动手,测试统一用这个双探。
static bool ready(void)
{
    appfw_biglist_poll();
    return appfw_biglist_poll();
}

static bool file_exists(const char *name)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void case_no_file(void)
{
    make_dir();
    appfw_biglist_set_catalog(fake_count, fake_get);   // 注册台目兜底
    // 无文件 = 内置台目兜底:仍是"大清单模式",条数/读取全走内存。
    assert(ready());
    assert(appfw_biglist_available());
    assert(appfw_biglist_count() == fake_count());
    char nm[APPFW_M3U_NAME_MAX], uu[APPFW_M3U_URL_MAX];
    assert(appfw_biglist_get(0, nm, sizeof(nm), uu, sizeof(uu)));
    assert(nm[0] != '\0' && uu[0] != '\0');
    assert(!appfw_biglist_get(fake_count(), nm, sizeof(nm), uu, sizeof(uu)));
    assert(appfw_biglist_find(FAKE_CAT[1].name) == 1);
    assert(appfw_biglist_find("不存在的台") == -1);
    printf("ok  无文件落内置台目(%d 台)\n", fake_count());

    // 关掉台目(测试钩子):无文件才是真正"不可用",NVS 小清单路径由此接管。
    appfw_biglist_set_catalog(NULL, NULL);
    assert(ready() == false);
    assert(!appfw_biglist_available());
    assert(appfw_biglist_count() == 0);
    assert(!appfw_biglist_get(0, nm, sizeof(nm), uu, sizeof(uu)));
    appfw_biglist_set_catalog(fake_count, fake_get);
    printf("ok  关台目后无文件不可用\n");
}

static void case_basic_parse(void)
{
    make_dir();
    // 含 CRLF、注释行、无逗号的 EXTINF(不收)、无配对的裸 URL(不收)、
    // 非法协议(不收)、行首/行尾空白。
    write_m3u("#EXTM3U\r\n"
              "#EXTINF:-1,CNN\r\n"
              "http://cnn.example/live\r\n"
              "\r\n"
              "#这是一条注释\r\n"
              "#EXTINF:-1\r\n"                    // 无逗号:不收
              "http://ignored.example/a\n"
              "   #EXTINF:-1,前导空白台 \n"
              "   https://fm.example/stream.mp3?token=1 \n"
              "rtsp://no.example/x\n"              // 无配对:不收
              "#EXTINF:-1,测试台\n"
              "http://test.example/中国\n");
    assert(ready());
    assert(appfw_biglist_available());
    assert(appfw_biglist_count() == 3);

    char nm[APPFW_M3U_NAME_MAX], uu[APPFW_M3U_URL_MAX];
    assert(appfw_biglist_get(0, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "CNN") == 0);
    assert(strcmp(uu, "http://cnn.example/live") == 0);

    assert(appfw_biglist_get(1, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "前导空白台") == 0);
    assert(strcmp(uu, "https://fm.example/stream.mp3?token=1") == 0);

    assert(appfw_biglist_get(2, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "测试台") == 0);
    assert(strcmp(uu, "http://test.example/中国") == 0);

    assert(!appfw_biglist_get(-1, nm, sizeof(nm), uu, sizeof(uu)));
    assert(!appfw_biglist_get(3, nm, sizeof(nm), uu, sizeof(uu)));
    printf("ok  解析与收录规则\n");
}

static void case_offsets_with_long_lines(void)
{
    make_dir();
    char big[1600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char text[3500];
    // 第一条后面塞一条 >512B 的注释行(会把 fgets 分成多次读),偏移必须不受影响。
    snprintf(text, sizeof(text),
             "#EXTINF:-1,第一台\nhttp://one.example/a\n"
             "#%s\n"
             "#EXTINF:-1,第二台\nhttps://two.example/b\n", big);
    write_m3u(text);
    assert(ready());
    assert(appfw_biglist_count() == 2);
    char nm[APPFW_M3U_NAME_MAX], uu[APPFW_M3U_URL_MAX];
    assert(appfw_biglist_get(1, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "第二台") == 0);
    assert(strcmp(uu, "https://two.example/b") == 0);
    assert(appfw_biglist_find("第一台") == 0);
    assert(appfw_biglist_find("第二台") == 1);
    assert(appfw_biglist_find("不存在") == -1);
    printf("ok  长行下偏移正确\n");
}

static void case_reindex_on_change(void)
{
    make_dir();
    write_m3u("#EXTINF:-1,A\nhttp://a.example/1\n");
    assert(ready());
    assert(appfw_biglist_count() == 1);

    // 换一份更大的清单(大小变化 → 重建)
    static char text[16384];
    int n = snprintf(text, sizeof(text), "#EXTM3U\n");
    for (int i = 0; i < 150; i++) {           // 跨过 64 条/批的写缓冲边界
        n += snprintf(text + n, sizeof(text) - n,
                      "#EXTINF:-1,台%d\nhttp://s.example/%d\n", i, i);
    }
    write_m3u(text);
    assert(ready());
    assert(appfw_biglist_count() == 150);
    char nm[APPFW_M3U_NAME_MAX], uu[APPFW_M3U_URL_MAX];
    assert(appfw_biglist_get(0, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "台0") == 0);
    assert(appfw_biglist_get(64, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(nm, "台64") == 0);
    assert(appfw_biglist_get(149, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strcmp(uu, "http://s.example/149") == 0);
    assert(!appfw_biglist_get(150, nm, sizeof(nm), uu, sizeof(uu)));

    // 索引损坏(截断)→ 自愈重建(运行中靠缓存的文件大小短路,自愈发生在
    // 下次 init/poll 重新校验时——设备上就是重启或换文件)
    appfw_biglist_init();
    char p[160];
    snprintf(p, sizeof(p), "%s/radio.idx", g_dir);
    assert(truncate(p, 100) == 0);
    assert(ready());
    assert(appfw_biglist_count() == 150);

    // 头部魔数破坏 → 自愈重建
    appfw_biglist_init();
    FILE *f = fopen(p, "r+b");
    assert(f);
    fseek(f, 0, SEEK_SET);
    fwrite("XXXX", 1, 4, f);
    fclose(f);
    assert(ready());
    assert(appfw_biglist_count() == 150);
    assert(appfw_biglist_find("台128") == 128);
    printf("ok  换清单重建/索引自愈\n");
}

static void case_utf8_truncation(void)
// 台名超过 32B 上限时按字符边界截断;find 用同样的截断规则,长名也能找到。
{
    make_dir();
    // 汉字 3B:22 个汉字 = 66B > 31。截断点必须落在字符边界(31 会劈开
    // 第 11 个汉字,回退到 30)。
    char name[128] = "";
    for (int i = 0; i < 22; i++) strcat(name, "汉");
    char text[256];
    snprintf(text, sizeof(text), "#EXTINF:-1,%s\nhttp://u.example/1\n", name);
    write_m3u(text);
    assert(ready());
    char nm[APPFW_M3U_NAME_MAX], uu[APPFW_M3U_URL_MAX];
    assert(appfw_biglist_get(0, nm, sizeof(nm), uu, sizeof(uu)));
    assert(strlen(nm) == 30);            // 10 个汉字,无残缺字节
    assert(appfw_biglist_find(nm) == 0); // 按截断后的名字也能找到
    assert(appfw_biglist_find(name) == 0);    // 全名也行(截断后相同)
    printf("ok  UTF-8 安全截断\n");
}

static void case_file_gone_falls_back(void)
{
    make_dir();
    write_m3u("#EXTINF:-1,甲台\nhttp://a.example/1\n");
    assert(ready());
    assert(appfw_biglist_count() == 1);
    char p[160];
    snprintf(p, sizeof(p), "%s/radio.m3u", g_dir);
    assert(remove(p) == 0);
    appfw_biglist_init();                       // 模拟重启
    assert(ready());
    assert(appfw_biglist_count() == fake_count());   // 落回台目
    printf("ok  文件消失落回台目\n");
}

static void case_discard(void)
{
    make_dir();
    write_m3u("#EXTINF:-1,A\nhttp://a.example/1\n");
    assert(ready());
    assert(appfw_biglist_available());
    appfw_biglist_discard();
    assert(appfw_biglist_available());          // 落回内置台目
    assert(appfw_biglist_count() == fake_count());
    assert(!file_exists("radio.m3u"));
    assert(!file_exists("radio.idx"));
    printf("ok  discard 删文件落回台目\n");
}

int main(void)
{
    case_no_file();
    case_basic_parse();
    case_offsets_with_long_lines();
    case_reindex_on_change();
    case_utf8_truncation();
    case_file_gone_falls_back();
    case_discard();
    printf("全部通过\n");
    return 0;
}
