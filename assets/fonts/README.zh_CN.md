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

需要 `lv_font_conv`(本机用 `npx lv_font_conv@1.5.3`)和思源黑体
`NotoSansSC-Regular.otf`。**7.9MB 的源字体不入库**,需要时先下载到本目录:

```bash
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
```

然后生成:

```bash
cd assets/fonts

S16=$(python3 -c "print(''.join(sorted(set(open('radio_charset.txt',encoding='utf-8').read().strip()))))")
S24=$(python3 -c "print(''.join(sorted(set(open('radio_charset_24.txt',encoding='utf-8').read().strip()))))")

npx --yes lv_font_conv@1.5.3 --font NotoSansSC-Regular.otf \
  --size 16 --format lvgl --bpp 4 --lv-include lvgl.h \
  --no-compress --force-fast-kern-format --symbols "$S16" -o app_font_16.c

npx --yes lv_font_conv@1.5.3 --font NotoSansSC-Regular.otf \
  --size 24 --format lvgl --bpp 4 --lv-include lvgl.h \
  --no-compress --force-fast-kern-format --symbols "$S24" -o app_font_24.c
```

要用 `--symbols` 而不是 `--range`:`--range` 只接受 `0x20-0x7F` 这样的数值
区间,喂字符列表会报 `invalid range value`。

## 加了新文案怎么办

1. 把新字加进 `radio_charset.txt`(改了标题还要同步 `radio_charset_24.txt`);
2. 按上面重新生成;
3. 跑 `./tools/validate.sh`——`tests/test_ui_charset.py` 会同时校验
   **字符清单**和**生成物里真实的 Unicode 区间**。

第 3 步不是走形式:清单改了却忘了重新生成时,只有生成物级校验能发现。否则
设备上就是空白或方框,而编译和门禁其余部分照样全绿。
