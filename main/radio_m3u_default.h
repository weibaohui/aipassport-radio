#pragma once
// main/radio_m3u_default.h —— 出厂默认电台清单(M3U 文本,C 字符串拼接)。
// 2026-10-03 重制:仅保留逐台探测存活的大陆台(旧 42 台 24 条已死,
// 且混有港台澳台与对外语广播),不足部分按省从大清单存活直链中补足。
// 更换清单请走门户的 M3U 导入/导出,不需要改固件。
#pragma once
#define RADIO_M3U_DEFAULT \
	"#EXTINF:-1,上海·上海新闻广播\n" \
	"http://lhttp.qingting.fm/live/270/64k.mp3\n" \
	"#EXTINF:-1,山西·山西广播电视台故事广播\n" \
	"http://lhttp.qingting.fm/live/5022511/64k.mp3\n" \
	"#EXTINF:-1,辽宁·沈阳新闻广播\n" \
	"http://lhttp.qingting.fm/live/20024/64k.mp3\n" \
	"#EXTINF:-1,吉林·吉林市交通广播\n" \
	"http://lhttp.qingting.fm/live/1819/64k.mp3\n" \
	"#EXTINF:-1,江苏·苏州儿童广播\n" \
	"http://lhttp.qingting.fm/live/2807/64k.mp3\n" \
	"#EXTINF:-1,河南·郑州人民广播电台 新闻广播\n" \
	"http://lhttp.qingting.fm/live/1220/64k.mp3\n" \
	"#EXTINF:-1,海南·海南音乐广播\n" \
	"http://lhttp.qingting.fm/live/4878/64k.mp3\n" \
	"#EXTINF:-1,海南·三亚旅游之声103.8\n" \
	"http://lhttp.qingting.fm/live/15318203/64k.mp3\n" \
	"#EXTINF:-1,贵州·贵州交通广播\n" \
	"http://lhttp.qingting.fm/live/20057/64k.mp3\n" \
	"#EXTINF:-1,青海·青海交通音乐广播 FM97.2\n" \
	"http://lhttp.qtfm.cn/live/5009/64k.mp3\n" \
	"#EXTINF:-1,CNR-1 中国之声 (64k 镜像)\n" \
	"https://lhttp.qtfm.cn/live/15318317/64k.mp3\n" \
	"#EXTINF:-1,云南·云南新闻广播\n" \
	"https://lhttp.qtfm.cn/live/1926/64k.mp3\n" \
	"#EXTINF:-1,国际新闻\n" \
	"https://lhttp.qtfm.cn/live/20500172/64k.mp3\n" \
	"#EXTINF:-1,内蒙古·内蒙古新闻综合广播\n" \
	"https://lhttp-hw.qtfm.cn/live/1883/64k.mp3\n" \
	"#EXTINF:-1,北京·北京新闻广播\n" \
	"https://lhttp.qtfm.cn/live/339/64k.mp3\n" \
	"#EXTINF:-1,吉林·吉林市音乐广播\n" \
	"https://lhttp.qtfm.cn/live/20211679/64k.mp3\n" \
	"#EXTINF:-1,四川·四川新闻广播\n" \
	"https://lhttp.qtfm.cn/live/4906/64k.mp3\n" \
	"#EXTINF:-1,天津·天津新闻广播\n" \
	"https://lhttp-hw.qtfm.cn/live/5022134/64k.mp3\n" \
	"#EXTINF:-1,宁夏·宁夏音乐广播\n" \
	"https://lhttp.qtfm.cn/live/15318294/64k.mp3\n" \
	"#EXTINF:-1,安徽·安徽综合广播\n" \
	"https://lhttp.qtfm.cn/live/4919/64k.mp3\n" \
	"#EXTINF:-1,山东·青岛新闻广播\n" \
	"http://lhttp.qingting.fm/live/1673/64k.mp3\n" \
	"#EXTINF:-1,广东·广东新闻广播\n" \
	"https://lhttp.qtfm.cn/live/1254/64k.mp3\n" \
	"#EXTINF:-1,广西·广西文艺广播 FM950广西音乐台\n" \
	"https://lhttp-hw.qtfm.cn/live/4875/64k.mp3\n" \
	"#EXTINF:-1,新疆·新疆交通广播\n" \
	"https://lhttp.qtfm.cn/live/1910/64k.mp3\n" \
	"#EXTINF:-1,江苏·苏州戏曲广播\n" \
	"http://lhttp.qingting.fm/live/20211622/64k.mp3\n" \
	"#EXTINF:-1,江西·江西广播电视台新闻广播\n" \
	"http://lhttp.qingting.fm/live/1809/64k.mp3\n" \
	"#EXTINF:-1,高阳县怀旧金曲964\n" \
	"http://lhttp.qingting.fm/live/5021555/64k.mp3?\n" \
	"#EXTINF:-1,浙江·温州经济生活广播\n" \
	"http://lhttp.qingting.fm/live/1157/64k.mp3\n" \
	"#EXTINF:-1,海南·海南新闻广播\n" \
	"http://lhttp.qingting.fm/live/1861/64k.mp3\n" \
	"#EXTINF:-1,湖北·武汉新闻广播\n" \
	"http://lhttp.qingting.fm/live/20198/64k.mp3\n"
