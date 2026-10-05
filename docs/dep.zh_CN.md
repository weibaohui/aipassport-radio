<p align="right">
  <a href="dep.md">English</a> · <strong>简体中文</strong>
</p>

# 依赖版本记录(dep)

应用与底层框架(aipassport-fw)的版本配对台账。框架以 git 子模块挂在
`components/framework`;每次更换框架提交并整机验证通过后,在这里补一行,
否则以后无从知道哪一版是验证过的。

## 当前配对(已知正常)

| 日期 | 应用(aipassport-radio) | 框架(aipassport-fw) | 说明 |
| --- | --- | --- | --- |
| 2026-10-05 | `9fed91e` | `7c85191` | 配网重构/门户截断修复/扫描两段式/柱阵平滑/音量行;社区 rev 2053 提交版 |
| 2026-10-05 | `46ce525` | `a4628b6` | 框架全量中文字库(常用 3500 字,LV_FONT_FMT_TXT_LARGE);应用删除自有字库 |
| 2026-10-06 | `46ce525` | `026ffd9` | 字库扩至 GB2312 一级(4827 字符);经同硬件的 kbmic 真机验证,未重刷 |

- 整机镜像:`build/FoloToy-AI-Passport-full.bin`(每次发版重新出,0x0 全量)
- 运行期核对:屏幕 设置 → 设备信息,或 MCP `get_device_info`,报告
  「框架 <fw短hash>.<日期> | 应用 <app短hash>」,与上表对得上即为正常版

## 规则

1. 更新框架子模块(`git -C components/framework fetch && pull`)后必须
   重新编译 + 真机验证,通过后才提交子模块指针,并在这里登记。
2. 应用提交里含子模块指针变更时,dep.md 同一批更新。
3. 回滚 = 把子模块指回表中的框架提交,再整机重刷。
4. 框架仓单独发版时,在 aipassport-fw 的 dep 行为不变;配对关系以本表为准。
