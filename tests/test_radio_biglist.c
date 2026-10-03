// tests/test_radio_biglist.c —— 大清单(files 分区 m3u + 偏移索引)的主机测试。
//
// radio_biglist 的文件操作全是 POSIX stdio,主机上直接跑真文件(临时目录),
// 钉死这些行为:索引重建的收录规则(EXTINF 配对/只收 http/https/注释与裸
// URL 忽略)、偏移在长行/空行/CRLF 下仍然对得上、get 按需读单条、find 线性
// 找名、换文件自动重建、索引损坏自愈、discard 删文件退回、UTF-8 安全截断。
#include "radio_biglist.h"
#include "radio_player.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// radio_biglist 的借洞挂载会调用播放器预留接口;主机测试不连 radio_player.c,
// 给空实现(测试桩的 ensure_mounted 恒真,借洞路径根本不会走到)。
void radio_player_release_reserve(void) {}
void radio_player_reacquire_reserve(void) {}

static char g_dir[128];

static void make_dir(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/radio-biglist-%d", (int)getpid());
    (void)system("rm -rf /tmp/radio-biglist-*");
    assert(mkdir(g_dir, 0755) == 0);
    radio_biglist_set_dir(g_dir);
    radio_biglist_init();
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
    radio_biglist_poll();
    return radio_biglist_poll();
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
    assert(ready() == false);
    assert(!radio_biglist_available());
    assert(radio_biglist_count() == 0);
    radio_station_t st;
    assert(!radio_biglist_get(0, &st));
    assert(radio_biglist_find("任意") == -1);
    printf("ok  无文件不可用\n");
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
    assert(radio_biglist_available());
    assert(radio_biglist_count() == 3);

    radio_station_t st;
    assert(radio_biglist_get(0, &st));
    assert(strcmp(st.name, "CNN") == 0);
    assert(strcmp(st.url, "http://cnn.example/live") == 0);

    assert(radio_biglist_get(1, &st));
    assert(strcmp(st.name, "前导空白台") == 0);
    assert(strcmp(st.url, "https://fm.example/stream.mp3?token=1") == 0);

    assert(radio_biglist_get(2, &st));
    assert(strcmp(st.name, "测试台") == 0);
    assert(strcmp(st.url, "http://test.example/中国") == 0);

    assert(!radio_biglist_get(-1, &st));
    assert(!radio_biglist_get(3, &st));
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
    assert(radio_biglist_count() == 2);
    radio_station_t st;
    assert(radio_biglist_get(1, &st));
    assert(strcmp(st.name, "第二台") == 0);
    assert(strcmp(st.url, "https://two.example/b") == 0);
    assert(radio_biglist_find("第一台") == 0);
    assert(radio_biglist_find("第二台") == 1);
    assert(radio_biglist_find("不存在") == -1);
    printf("ok  长行下偏移正确\n");
}

static void case_reindex_on_change(void)
{
    make_dir();
    write_m3u("#EXTINF:-1,A\nhttp://a.example/1\n");
    assert(ready());
    assert(radio_biglist_count() == 1);

    // 换一份更大的清单(大小变化 → 重建)
    static char text[16384];
    int n = snprintf(text, sizeof(text), "#EXTM3U\n");
    for (int i = 0; i < 150; i++) {           // 跨过 64 条/批的写缓冲边界
        n += snprintf(text + n, sizeof(text) - n,
                      "#EXTINF:-1,台%d\nhttp://s.example/%d\n", i, i);
    }
    write_m3u(text);
    assert(ready());
    assert(radio_biglist_count() == 150);
    radio_station_t st;
    assert(radio_biglist_get(0, &st));
    assert(strcmp(st.name, "台0") == 0);
    assert(radio_biglist_get(64, &st));
    assert(strcmp(st.name, "台64") == 0);
    assert(radio_biglist_get(149, &st));
    assert(strcmp(st.url, "http://s.example/149") == 0);
    assert(!radio_biglist_get(150, &st));

    // 索引损坏(截断)→ 自愈重建(运行中靠缓存的文件大小短路,自愈发生在
    // 下次 init/poll 重新校验时——设备上就是重启或换文件)
    radio_biglist_init();
    char p[160];
    snprintf(p, sizeof(p), "%s/radio.idx", g_dir);
    assert(truncate(p, 100) == 0);
    assert(ready());
    assert(radio_biglist_count() == 150);

    // 头部魔数破坏 → 自愈重建
    radio_biglist_init();
    FILE *f = fopen(p, "r+b");
    assert(f);
    fseek(f, 0, SEEK_SET);
    fwrite("XXXX", 1, 4, f);
    fclose(f);
    assert(ready());
    assert(radio_biglist_count() == 150);
    assert(radio_biglist_find("台128") == 128);
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
    radio_station_t st;
    assert(radio_biglist_get(0, &st));
    assert(strlen(st.name) == 30);            // 10 个汉字,无残缺字节
    assert(radio_biglist_find(st.name) == 0); // 按截断后的名字也能找到
    assert(radio_biglist_find(name) == 0);    // 全名也行(截断后相同)
    printf("ok  UTF-8 安全截断\n");
}

static void case_discard(void)
{
    make_dir();
    write_m3u("#EXTINF:-1,A\nhttp://a.example/1\n");
    assert(ready());
    assert(radio_biglist_available());
    radio_biglist_discard();
    assert(!radio_biglist_available());
    assert(radio_biglist_count() == 0);
    assert(!file_exists("radio.m3u"));
    assert(!file_exists("radio.idx"));
    printf("ok  discard 删文件退回\n");
}

int main(void)
{
    case_no_file();
    case_basic_parse();
    case_offsets_with_long_lines();
    case_reindex_on_change();
    case_utf8_truncation();
    case_discard();
    printf("全部通过\n");
    return 0;
}
