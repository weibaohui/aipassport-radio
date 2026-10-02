<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# CJK font subsets

The LVGL built-in font (Montserrat) has no CJK glyphs, so Chinese text needs a
font shipped with the application. Embedding the full set costs about 392 KB of
flash, so only the characters the source actually uses are embedded.

- `radio_charset.txt` — every character used at 16px (ASCII plus CJK and
  full-width punctuation)
- `radio_charset_24.txt` — the characters used at 24px, which is **only the
  five glyphs of the top-bar title**
- `app_font_16.c` / `app_font_24.c` — generated output, **do not edit by hand**

## Why two sizes

The framework uses the 24px font at exactly one place: the top-bar title in
`appfw_ui.c`. The full 24px set would cost 392 KB while the title needs five
glyphs (85 KB), so embedding only the title saves 78%.

The cost is that **changing `home_title` requires regenerating the 24px font**.
`tests/test_ui_charset.py` enforces this.

## Regenerating

Do not hand-maintain the character lists. `tools/gen_fonts.py` derives them
from the sources that LVGL actually renders, generates both fonts, and then
runs the glyph gate as its own self-check:

```bash
cd assets/fonts
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
cd ../..
python3 tools/gen_fonts.py
```

The 7.9 MB source font is not committed; fetch it once as shown.

Under the hood it calls, for each size:

```bash
npx --yes lv_font_conv@1.5.3 --font NotoSansSC-Regular.otf \
  --size 16 --format lvgl --bpp 4 --lv-include lvgl.h \
  --no-compress --force-fast-kern-format --symbols "$SYMS" -o app_font_16.c
```

Use `--symbols`, not `--range`: `--range` only accepts numeric spans such as
`0x20-0x7F` and rejects a bare character list with `invalid range value`.

## After adding UI text

1. Run `python3 tools/gen_fonts.py`.
2. Run `./tools/validate.sh`. `tests/test_ui_charset.py` checks both the
   character list **and the real Unicode ranges inside the generated file**.

Step 2 is what catches the case where a developer edits a source string and
never regenerates. Without the generated-file check, the build and the rest of
the gate pass while the device shows blanks or tofu boxes.

The generator deliberately over-collects: this application keeps the portal
HTML card in the same file as the UI strings, and those portal strings are
scanned too. A few spare glyphs cost about 200 bytes each and remove a whole
class of surprise.
