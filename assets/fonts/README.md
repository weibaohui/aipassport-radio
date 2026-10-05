<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CJK fonts (moved to the framework)

This app no longer ships its own fonts. The framework provides them at
`components/framework/appfw/fonts/`: the complete 3500-character Modern
Chinese Common Characters table plus rare place-name / traditional
station-name glyphs, in both 16 px and 24 px, linked automatically via
`-u` anchors.

- Charset table and regeneration: see `components/framework/appfw/fonts/README.zh_CN.md`
- Dynamic song-title codepoint filter: `main/radio_title_table.h`, derived by
  `tools/gen_title_table.py` from the framework table
- Gate: `tests/test_ui_charset.py`

`NotoSansSC-Regular.otf` (7.9 MB) and its OFL license file stay in this
directory for framework font regeneration; they are not committed.
