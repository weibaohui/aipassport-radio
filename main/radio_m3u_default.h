#pragma once
// main/radio_m3u_default.h —— 出厂默认电台清单(M3U 文本,C 字符串拼接)。
// 由用户播放清单的前 42 个可播条目生成(http 直链、非 HLS,不含 #EXTM3U 头)。
// 更换清单请走门户的 M3U 导入/导出,不需要改固件。
#pragma once
#define RADIO_M3U_DEFAULT \
	"#EXTM3U\n" \
	"#EXTINF:-1,Digital Radio Hong Kong 香港数码广播\n" \
	"http://ice.digitalradiohk.net:8000/drhk\n" \
	"#EXTINF:-1,香港电台第一台 RTHK Radio 1 (ICY 直链)\n" \
	"http://stm1.rthk.hk/radio1\n" \
	"#EXTINF:-1,城市广播网 城市广播 FM92.9\n" \
	"http://fm929.cityfm.tw:8080/929.mp3\n" \
	"#EXTINF:-1,城市广播网 台南知音 FM97.1\n" \
	"http://fm971.cityfm.tw:8080/971.mp3\n" \
	"#EXTINF:-1,Classical 古典音乐台 FM97.7\n" \
	"http://59.120.88.155:8000/live.mp3\n" \
	"#EXTINF:-1,台湾广播 台北一台\n" \
	"http://test.taiwan-radio.tw:8000/TP.AM1323\n" \
	"#EXTINF:-1,台中广播\n" \
	"http://211.20.119.101:8081/\n" \
	"#EXTINF:-1,Romance Radio 澳門 95.5\n" \
	"http://162.220.162.10:8011/stream\n" \
	"#EXTINF:-1,上海·上海新闻广播\n" \
	"http://lhttp.qingting.fm/live/270/64k.mp3\n" \
	"#EXTINF:-1,上海·上海经典音乐广播\n" \
	"http://lhttp.qingting.fm/live/267/64k.mp3\n" \
	"#EXTINF:-1,上海·上海交通广播\n" \
	"http://lhttp.qingting.fm/live/266/64k.mp3\n" \
	"#EXTINF:-1,山西·山西广播电视台故事广播\n" \
	"http://lhttp.qingting.fm/live/5022511/64k.mp3\n" \
	"#EXTINF:-1,山西·太原老年之声\n" \
	"http://lhttp.qingting.fm/live/20211701/64k.mp3\n" \
	"#EXTINF:-1,内蒙古·内蒙古文艺之声\n" \
	"http://lhttp.qingting.fm/live/1886/64k.mp3\n" \
	"#EXTINF:-1,内蒙古·内蒙古城乡生活广播\n" \
	"http://lhttp.qingting.fm/live/1888/64k.mp3\n" \
	"#EXTINF:-1,辽宁·沈阳新闻广播\n" \
	"http://lhttp.qingting.fm/live/20024/64k.mp3\n" \
	"#EXTINF:-1,辽宁·辽宁广播电视台音乐广播\n" \
	"http://lhttp.qingting.fm/live/1101/64k.mp3\n" \
	"#EXTINF:-1,辽宁·辽宁广播电视台都市广播\n" \
	"http://lhttp.qingting.fm/live/1099/64k.mp3\n" \
	"#EXTINF:-1,辽宁·辽宁广播电视台生活广播\n" \
	"http://lhttp.qingting.fm/live/1102/64k.mp3\n" \
	"#EXTINF:-1,吉林·吉林市交通广播\n" \
	"http://lhttp.qingting.fm/live/1819/64k.mp3\n" \
	"#EXTINF:-1,吉林·吉林市广播电视台经济广播\n" \
	"http://lhttp.qingting.fm/live/1823/64k.mp3\n" \
	"#EXTINF:-1,江苏·苏州戏曲广播\n" \
	"http://lhttp.qingting.fm/live/20211622/64k.mp3\n" \
	"#EXTINF:-1,江苏·苏州儿童广播\n" \
	"http://lhttp.qingting.fm/live/2807/64k.mp3\n" \
	"#EXTINF:-1,江苏·苏州生活广播\n" \
	"http://lhttp.qingting.fm/live/2801/64k.mp3\n" \
	"#EXTINF:-1,浙江·温州经济生活广播\n" \
	"http://lhttp.qingting.fm/live/1157/64k.mp3\n" \
	"#EXTINF:-1,安徽·安徽生活广播\n" \
	"http://lhttp.qingting.fm/live/1948/64k.mp3\n" \
	"#EXTINF:-1,江西·江西广播电视台新闻广播\n" \
	"http://lhttp.qingting.fm/live/1809/64k.mp3\n" \
	"#EXTINF:-1,江西·江西广播电视台文艺音乐广播\n" \
	"http://lhttp.qingting.fm/live/1802/64k.mp3\n" \
	"#EXTINF:-1,山东·青岛新闻广播\n" \
	"http://lhttp.qingting.fm/live/1673/64k.mp3\n" \
	"#EXTINF:-1,山东·青岛老年广播\n" \
	"http://lhttp.qingting.fm/live/4956/64k.mp3\n" \
	"#EXTINF:-1,河南·郑州人民广播电台 新闻广播\n" \
	"http://lhttp.qingting.fm/live/1220/64k.mp3\n" \
	"#EXTINF:-1,河南·河南星河音乐广播\n" \
	"http://lhttp.qingting.fm/live/20210755/64k.mp3\n" \
	"#EXTINF:-1,湖北·武汉新闻广播\n" \
	"http://lhttp.qingting.fm/live/20198/64k.mp3\n" \
	"#EXTINF:-1,湖南·长沙城市之声\n" \
	"http://lhttp.qingting.fm/live/4237/64k.mp3\n" \
	"#EXTINF:-1,广东·深圳生活广播\n" \
	"http://lhttp.qingting.fm/live/1273/64k.mp3\n" \
	"#EXTINF:-1,海南·海南新闻广播\n" \
	"http://lhttp.qingting.fm/live/1861/64k.mp3\n" \
	"#EXTINF:-1,海南·海南音乐广播\n" \
	"http://lhttp.qingting.fm/live/4878/64k.mp3\n" \
	"#EXTINF:-1,海南·三亚旅游之声103.8\n" \
	"http://lhttp.qingting.fm/live/15318203/64k.mp3\n" \
	"#EXTINF:-1,海南·海南民生广播\n" \
	"http://lhttp.qingting.fm/live/21243/64k.mp3\n" \
	"#EXTINF:-1,贵州·贵州交通广播\n" \
	"http://lhttp.qingting.fm/live/20057/64k.mp3\n" \
	"#EXTINF:-1,青海·青海交通音乐广播 FM97.2\n" \
	"http://lhttp.qtfm.cn/live/5009/64k.mp3\n" \
	"#EXTINF:-1,青海·西宁交通文艺广播\n" \
	"http://lhttp.qingting.fm/live/5022283/64k.mp3\n"
