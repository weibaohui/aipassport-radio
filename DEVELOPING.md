# DEVELOPING.md —— 开发者文档

面向改代码的人。使用者请看 [README.md](README.md)。

## 环境与构建

```bash
# ESP-IDF 5.5(注意:默认 python 环境缺失,必须显式指定 3.13 venv)
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env
source ~/esp/esp-idf/export.sh

idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

- 设备串口 `/dev/cu.usbmodem1101`;刷机前确认没有残留 monitor 占口
  (`lsof /dev/cu.usbmodem1101`)。
- **分段刷**(bootloader + 分区表 + app,即 `idf.py flash` 的默认行为)
  不触碰 NVS 与 files 分区;**不要刷 0x0 合并镜像**(会清 NVS)。

## 门禁(改动必跑)

```bash
tools/validate.sh --static     # 框架检查 + 主机测试 + 字形覆盖
tools/validate.sh --firmware   # 固件布局校验 + 归档
```

- 主机测试映射在 `tests/host_tests.txt`(<名字> <源文件...>);应用仓的
  `tests/*.h` 是主机桩(include 路径优先于框架真头文件),固件构建不受影响。
- 框架自身的测试与门禁实现都在 submodule 里
  (`components/framework/tools/validate.sh`)。

## 字库(改了中文文案必做)

任何会显示在屏幕上的中文文案改动后:

```bash
python3 tools/gen_fonts.py
```

它从源码字面量推导字符集,重新生成 16/24px 字库、码点白名单并跑自检。
漏跑的后果:新字在真机上显示空白。

## 内置台目再生成

台目本体是固件 rodata(`main/radio_catalog.h`,勿手改),由播放清单生成:

```bash
tools/gen_catalog.py
```

台目收录标准:8 秒持续供流实测存活、码率 ≤130kbps、只收 http/https 直链。
电台会下线,定期重筛;用户侧临时补台用 MCP 的 `playlist_add_station`。

## 架构

```
main/                       应用层:业务与数据
  radio_store.c             清单存储 = 台目 rodata(345,只读) + 自定义台 NVS
  radio_player.c            播放引擎:流接入/解码/暂停/重试退避/https 分身
  radio_mcp.c               MCP 工具表(协议与传输在框架)
  radio_pages.c             界面(列表/播放页/频谱/设置)
  radio_catalog.h           台目数据(gen_catalog.py 生成,勿手改)
components/framework/       框架 aipassport-fw(submodule,业务无关)
  appfw_mcp.c / _srv.c      MCP 协议核 + 常驻极简 TCP 服务(8080)
  appfw_portal.c            captive 配网门户(仅配网期存在,空闲自卸)
  appfw_net/netlist/storage UI 骨架、WiFi 引擎(多热点回退)、NVS 封装
  appfw_stream/hls/icy      通用流管线(HLS/ICY 元数据/重定向/https 回退)
```

分层规则:**机制进框架,数据与领域策略留应用**。框架内不允许出现
"电台"等业务词汇——需要新能力时加注入点,不加 if(app==X)。

## 存储模型

- **无文件系统**(FAT 已整体移除,files 分区保留在分区表但不再使用——
  保分段刷机兼容,勿改动 partitions.csv 前三行偏移)。
- 清单 = 台目 rodata + 自定义台逐条 NVS(`u_cnt` + `u0..`,上限 100;
  NVS 分区 24KB 是物理天花板,写满由 `radio_store_add` 干净报错)。
- 旧版清单键(`r_cnt`/`r0..r47`)开机被一次性清空(内容全是台目副本)。

## 内存布局红线(动了就出事)

- 开机预留:**解码器 60KB + 音频 arena 20KB**(HLS AAC 初始化要 ~60KB
  连续块,改小必炸);解码器惰性分配必须在连接之前用探针烧掉
  (`radio_mp3_probe.h`/`radio_ts_probe.h`),否则 TLS 先占洞。
- LVGL 绘制缓冲 8 行(`BSP_LVGL_DRAW_BUFFER_LINES`),别调大。
- LVGL 专用池 32KB(`CONFIG_LV_MEM_SIZE`)是硬分区设计:调小曾致 MP3
  初始化直接失败。
- 常驻任务栈:播放 6KB / 按键 6KB / AI 服务 6KB(ai_mcp,4096 曾真机
  打穿——FAT 时代峰值 ~4.5KB,现 FAT 已删仍留 6144 余量)。
- mbedtls 证书错 0x4290 是堆布局问题不是证书问题;部分 qtfm 系新证书链
  仍间歇失败,http 分身可用(播放器自动重试)。
- esp_http_client 不自动跟大写 "HTTP://" 的 302 与跨域重定向,
  `radio_player.c` 已手工跟 ≤5 跳。

## 已知边界

- 网络电台存活是流动的:台目需定期重筛。
- 码率天花板 ~130kbps(设备内存舒适区)。
- MCP 端点无鉴权,信任模型 = 可信局域网,勿暴露公网。
- 固件内编解码器全开(AAC/MP3 之外多数用不到),预编译库拆不动单个,
  可经 `AUDIO_DECODER_*_SUPPORT` 配置瘦身 flash(~100-200KB 空间)。
