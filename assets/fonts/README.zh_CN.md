<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# 中文字库(已迁至框架)

本应用不再自带字库。中文字库由框架提供:`components/framework/appfw/fonts/`,
收录《现代汉语常用字表》3500 字全量 + 生僻地名/繁体台名,16/24px 两个字号,
由框架在链接时经 `-u` 锚点自动接入。

- 字符清单与重新生成命令:见 `components/framework/appfw/fonts/README.zh_CN.md`
- 动态曲名的码点过滤表:`main/radio_title_table.h`,由 `tools/gen_title_table.py`
  从框架清单推导
- 门禁:`tests/test_ui_charset.py`

`NotoSansSC-Regular.otf`(7.9MB)与 OFL 授权文件保留在本目录供框架重新
生成字库时取用,不入库(见 .gitignore)。
