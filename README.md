<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# aipassport-radio Internet Radio

An AI-first internet radio on the FoloToy AI Passport board (ESP32-C3,
1.77" color display, speaker): **you control it by talking**. An AI assistant
on your LAN (Claude, ZCode, or any MCP-capable client) is the only remote
control it needs.

No web admin, no app, no cloud account. Boot it, join your WiFi, plug the AI
in — then just say:

> "Play some music" · "Something from Shandong" · "Volume 30" · "Pause" ·
> "Random station" · "Import my usual stations"

## Getting started

### 1. Provision WiFi (once)

Provisioning is **manual**: long-press the UP button to open Settings, choose
Provisioning, then "1. Open hotspot". The device leaves its current network and
switches to dedicated hotspot mode; the screen shows the hotspot name, two QR
codes and on-screen guidance.

Then, on your phone: join that hotspot (scan the QR or pick it from the WiFi
list — it is open), and the portal opens automatically (or visit
http://192.168.4.1). Tap "Scan", tick your home WiFi, enter the password, then
tap "Save & connect". The hotspot closes, the device joins your WiFi, shows
"Done" on screen and returns to the play view — provisioning finished.

Changed networks later: repeat the steps above (the device leaves its current
network first), or just tell the AI "list saved hotspots / remove hotspot XXX /
connect to hotspot XXX".

### 2. Plug the AI in

The device's MCP endpoint is **always on** (it does not depend on playback or
idle state):

```
http://<device-ip>:8080/mcp
```

Add it to any MCP client. Claude example:

```json
{ "mcpServers": { "radio": { "url": "http://192.168.31.252:8080/mcp" } } }
```

**Where to find the IP**: on the device, open Settings → Device info — the
"AI address" row is the full URL. Or check your router's client list.

> ⚠️ The MCP endpoint has no authentication; the trust model is a trusted
> home LAN. Do not expose port 8080 to the internet.

### 3. Start talking

Any MCP client sees 27 tools (table below). You never need tool names — speak
naturally and the AI picks:

| You say | It does |
| --- | --- |
| "Play some music" | searches station names, tunes in, reports back |
| "Something from Shandong" | searches "Shandong", lists candidates, plays |
| "Next / random" | play_next / play_random |
| "Pause, resume later" | pause (silent but keeps the stream) → resume |
| "Volume 30" | set_volume (persisted across reboots) |
| "What's playing?" | get_state (station + stream URL + sample rate) |
| "Add a Dongying station" | playlist_add_station (you provide the stream URL) |

## What's inside

- **345 built-in stations**, available at first boot: each one verified with
  an 8-second continuous-stream test at ≤130 kbps. Stored in firmware rodata
  (zero RAM) and immune to factory reset.
- **Up to 100 custom stations** in on-device NVS, persistent across reboots.
  The AI can add, modify (same name = new URL), remove, or clear them.
- **Smart volume (always on)**: stations broadcast at different loudness —
  the device levels them automatically, so one volume setting sounds
  consistent across stations (slow leveling, no pumping).
- **Three play animations**: pick in Settings → Effect — classic spectrum /
  LED level meter / symmetric spectrum, all with flowing rainbow colors.
- **Adjustable backlight**: Settings → Brightness, five gears, persisted.
- **Multi-AP WiFi fallback**: save several hotspots; the engine reconnects
  and falls back in order.

## Tool reference (for AIs and the curious)

Playback (`play_*` returns "tuned" immediately; the connection settles in a
few seconds — follow up with `get_state`):

| Tool | Args | Notes |
| --- | --- | --- |
| `play_index` | `index` | 1-based over the whole list (1-6 are featured) |
| `play_name` | `name` | exact station name |
| `play_next` / `play_prev` | — | next / previous (wraps around) |
| `play_random` | — | random station, avoids the current one |
| `pause` | — | pause: silent but keeps connection and stream; only while playing |
| `resume` | — | resume from pause; refused otherwise |
| `stop` | — | stop and disconnect |
| `set_volume` | `level` 0-100 | persisted on device |
| `get_state` | — | station / stream URL / sample rate / volume / error |

Discovery and playlist:

| Tool | Args | Notes |
| --- | --- | --- |
| `search_stations` | `keyword`, `limit?` | fuzzy name search |
| `list_stations` | `from?`, `count?` | paged browsing |
| `playlist_add_station` | `name`, `url` | add (http/https direct link); same name = update URL |
| `playlist_remove_station` | `index` | remove (built-in catalog is protected) |
| `playlist_clear_custom` | — | clear all custom stations |

WiFi and device:

| Tool | Args | Notes |
| --- | --- | --- |
| `wifi_add_hotspot` | `ssid`, `password?`, `connect_now?` | add a hotspot, optionally connect now |
| `wifi_list_saved` / `wifi_remove_hotspot` | — / `ssid` | list / remove saved hotspots |
| `wifi_status` | — | connection state and IP |
| `wifi_connect_saved` | `ssid` | connect to a saved hotspot |
| `set_brightness` | `percent?` | backlight level (no arg = query; 10-100, gear-snapped) |
| `get_device_info` | — | framework/app versions / IP / memory panorama / uptime |
| `get_provisioning_status` | — | provisioning portal state |
| `get_recent_logs` | `count?` | recent on-device logs (ring buffer, chronological) |
| `set_log_level` | `tag?`, `level` | adjust log level (esp_log) |
| `set_netlog` | `on`, `ip?`, `port?` | UDP syslog push; receiver: `nc -kul 5514` |
| `set_screen_off` | `seconds?` | screen-sleep timer (no arg = query; 0 = never) |

## Buttons (for the non-AI moments)

| Screen | Action | Result |
| --- | --- | --- |
| List (home) | up / down | move cursor |
| List | OK | play selected; press again on the same station = stop |
| List | double-click up/down | page turn (5 rows per page) |
| Play | up / down | previous / next station |
| Play | OK | pause / resume |
| Global | long-press OK | station list |
| Global | long-press up | settings menu |
| Global | long-press down | volume page |

## FAQ

- **"Station X won't play / dies after a beep"**: streams are alive — they go
  offline and move. The player already retries via an http mirror when https
  is cut; otherwise try another station or ask the AI for a similar one.
- **"I want a station you don't have"**: find its stream URL and have the AI
  `playlist_add_station` it (up to 100, http/https direct links only).
- **"Pause vs stop?"**: pause keeps the connection (instant resume, no gap in
  the live stream); stop disconnects and saves data. Overnight: `stop`.
- **"Do volume, stations and brightness survive a reboot?"** Yes — all
  on-device (volume, custom stations, brightness, effect choice).
- **"Stations sound louder/quieter than each other"** — smart volume levels
  them automatically; give it a few seconds after switching.
- **"Shake to change station?"** No — this hardware has no gyroscope or
  accelerometer.
- **"How do I update the firmware?"** See the
  [developer guide](/docs/development.md).

## For developers

Architecture, build, flashing, gates, and catalog regeneration live in the
[development guide](/docs/development.md). The firmware is built on our
shared framework
[aipassport-fw](https://github.com/weibaohui/aipassport-fw) (submodule);
the radio application lives in `main/`.
