<p align="right">
  <strong>English</strong> · <a href="development.zh_CN.md">简体中文</a>
</p>

# aipassport-radio — Development Guide

For people changing the code. User-facing documentation is in the
[README](/README.md).

## Environment and build

```bash
# ESP-IDF 5.5 (the default python env is missing — point at the 3.13 venv)
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env
source ~/esp/esp-idf/export.sh

idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

- Device serial port is `/dev/cu.usbmodem1101`; before flashing make sure no
  stale monitor holds it (`lsof /dev/cu.usbmodem1101`).
- **Segmented flashing** (bootloader + partition table + app, the `idf.py
  flash` default) leaves NVS untouched. **Never flash a 0x0 merged image** —
  it wipes NVS.

## Gates (mandatory for every change)

```bash
tools/validate.sh --static     # repo checks + host tests + glyph coverage
tools/validate.sh --firmware   # image layout checks + archive
```

- Host-test mapping lives in `tests/host_tests.txt` (`<name> <sources...>`).
  The `tests/*.h` headers are host stubs (the include path ranks them above
  the real framework headers); firmware builds are unaffected.
- The framework's own tests and the gate implementation live in the submodule
  (`components/framework/tools/validate.sh`).

## Fonts (mandatory after any user-visible Chinese text)

After changing any Chinese string that reaches the screen:

```bash
python3 tools/gen_fonts.py
```

It derives the charset from source literals, regenerates the 16/24 px fonts
and the codepoint whitelist, and runs a self-check. Skipping it shows the new
characters as blanks on the device.

## Built-in catalog regeneration

The station catalog is firmware rodata (`main/radio_catalog.h`, do not edit
by hand); it is generated from a playlist:

```bash
tools/gen_catalog.py
```

Catalog criteria: 8-second continuous-stream survival test, bitrate
≤130 kbps, http/https direct links only. Stations go offline over time, so
re-screen periodically; for one-off additions use the MCP tool
`playlist_add_station`.

## Architecture

```
main/                       Application layer: business logic and data
  radio_store.c             Playlist = catalog rodata (345, read-only)
                            + custom stations in NVS
  radio_player.c            Playback engine: stream in / decode / pause /
                            retry backoff / https mirror fallback
  radio_mcp.c               MCP tool table (protocol & transport in framework)
  radio_pages.c             UI (list / play page / spectrum / settings)
  radio_catalog.h           Catalog data (generated, do not hand-edit)
components/framework/       Framework aipassport-fw (submodule, business-free)
  appfw_mcp.c / _srv.c      MCP protocol core + resident minimal TCP server
  appfw_portal.c            Captive provisioning portal (only while needed)
  appfw_viz.c               Spectrum analyzer (pure logic, host-tested)
  appfw_bars.c              Bar-array LVGL widget (the play-page spectrum)
  appfw_net/netlist/storage UI skeleton, WiFi engine, NVS wrappers
  appfw_loudness.c          Loudness leveling (smart volume, slow AGC)
  appfw_viz.c / bars.c      Spectrum analyzer / bar-array widget
  appfw_netlog.c            Network logging (ring buffer + UDP syslog)
  appfw_stream/hls/icy      Generic stream pipeline (HLS/ICY/redirect/https)
```

Layering rule: **mechanism goes to the framework, data and domain policy stay
in the application.** The framework contains no "station" vocabulary — new
capabilities arrive as injection points, never as `if (app == X)`.

The play page drives its bars with a synthesized envelope (`K_ENV` in
`radio_pages.c`); the real FFT (`appfw_viz`) is wired and host-tested but
switched off on device (`APPFW_VIZ_ENABLED 0` in `radio_player.c`) because
even the fixed-point FFT cost ~¼ CPU. Flip the switch to bring it back.

## Storage model

- **No filesystem** (FAT removed entirely; the files partition stays in the
  partition table unused — keep the first three offsets in `partitions.csv`
  unchanged for segmented-flash compatibility).
- Playlist = catalog rodata + custom stations as per-item NVS records
  (`u_cnt` + `u0..`, cap 100; the 24 KB NVS partition is the physical limit —
  `radio_store_add` fails cleanly when full).
- Legacy playlist keys (`r_cnt` / `r0..r47`) are wiped once at boot (their
  content duplicated the catalog).

## Memory red lines (breaking these breaks the device)

- Boot-time reservations: **60 KB decoder + 20 KB audio arena** (HLS AAC
  init needs a ~60 KB contiguous block; shrinking them bricks AAC). The
  decoder's lazy allocation must be burned by the embedded probes
  (`radio_mp3_probe.h` / `radio_ts_probe.h`) *before* connecting, or TLS
  grabs the hole first.
- LVGL draw buffer is 8 lines (`BSP_LVGL_DRAW_BUFFER_LINES`) — do not raise.
- The 32 KB LVGL pool (`CONFIG_LV_MEM_SIZE`) is a hard partition by design:
  shrinking it once broke MP3 decoder init.
- Resident task stacks: player 6 KB / keys 6 KB / AI server 6 KB (`ai_mcp`;
  4096 once died on device under deep mount chains, 6144 kept as margin).
- mbedtls cert error 0x4290 is a heap-layout problem, not a certificate
  problem; some qtfm CDN chains still fail intermittently — the http mirror
  works (the player retries automatically).
- esp_http_client does not follow redirects with an uppercase `"HTTP://"`
  scheme or cross-host redirects; `radio_player.c` follows up to 5 manually.

## Known limits

- Station liveness drifts: re-screen the catalog periodically.
- Bitrate ceiling ~130 kbps (the device's memory comfort zone).
- The MCP endpoint is unauthenticated; trusted-LAN only, never expose it.
- All audio decoders ship enabled (AAC/MP3 are the used ones); the
  precompiled codec library can be slimmed ~100-200 KB of flash via the
  `AUDIO_DECODER_*_SUPPORT` options.
