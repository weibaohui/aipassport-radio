<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 中文字体子集

LVGL 自带的字体(Montserrat)没有中文字形,中文界面必须自带字库。嵌全字集要
约 392 KB flash,这里只嵌**源码里真正用到的字**。

- `radio_charset.txt` —— 16px 用到的全部字符(ASCII + 中文/全角标点)
- `radio_charset_24.txt` —— 24px 用到的字符,**只有标题「网络收音机」5 个字**
- `app_font_16.c` / `app_font_24.c` —— 生成物,**不要手改**

## 为什么要分两个字号

框架的 24px 字体只有一处使用点:`appfw_ui.c` 里的顶栏标题。24px 全集要
392 KB,而标题只要 5 个字(85 KB);只嵌标题省下 78%。

代价是:**改了 `home_title` 就必须重新生成 24px 字库**。这条由
`tests/test_ui_charset.py` 强制。

## 重新生成

不要手工维护字符清单。`tools/gen_fonts.py` 会从**真正会被 LVGL 渲染的源码**
里推导字符集,生成两个字库,最后自己跑一遍字形门禁当作自检:

```bash
cd assets/fonts
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
cd ../..
python3 tools/gen_fonts.py
```

7.9MB 的源字体不入库,按上面下载一次即可。

它内部对每个字号调用:

```bash
npx --yes lv_font_conv@1.5.3 --font NotoSansSC-Regular.otf \
  --size 16 --format lvgl --bpp 4 --lv-include lvgl.h \
  --no-compress --force-fast-kern-format --symbols "$SYMS" -o app_font_16.c
```

要用 `--symbols` 而不是 `--range`:`--range` 只接受 `0x20-0x7F` 这样的数值
区间,喂字符列表会报 `invalid range value`。

## 加了新文案怎么办

1. 跑 `python3 tools/gen_fonts.py`。
2. 跑 `./tools/validate.sh`——`tests/test_ui_charset.py` 会同时校验
   **字符清单**和**生成物里真实的 Unicode 区间**。

第 2 步挡的是"改了源码文案却忘了重新生成字库"。没有生成物级校验的话,
编译和门禁其余部分照样全绿,而设备上是一片空白或方框。

生成器会**故意多收**:本应用把门户 HTML 卡片和 UI 字符串写在同一个文件里,
门户那些字符串也会被扫进去。多几个字每个约 200 字节,换来的是少一整类意外。
