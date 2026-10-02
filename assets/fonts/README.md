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

Requires `lv_font_conv` (here `npx lv_font_conv@1.5.3`) and Source Han Sans
`NotoSansSC-Regular.otf`. The 7.9 MB source font is **not committed**; fetch it
once into this directory:

```bash
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
```

Then generate:

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

Use `--symbols`, not `--range`: `--range` only accepts numeric spans such as
`0x20-0x7F` and rejects a bare character list with `invalid range value`.

## After adding UI text

1. Add the new characters to `radio_charset.txt` (and to `radio_charset_24.txt`
   if the title changed).
2. Regenerate as above.
3. Run `./tools/validate.sh`. `tests/test_ui_charset.py` checks both the
   character list **and the real Unicode ranges inside the generated file**.

Step 3 is not ceremony. If the list is updated but the font is not regenerated,
only the generated-file check can catch it; otherwise the device shows blanks
or tofu boxes while both the build and the rest of the gate pass.
