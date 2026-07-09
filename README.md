# Claude Buddy

A desk companion for the **Waveshare ESP32-S3-Touch-LCD-3.49** (172x640 bar
display). A little animated face lives on the display and reacts to what
Claude Code is doing on your Windows PC — plus your official Claude usage
limits, context-window gauge, clock, battery and PC stats. Works over USB or
Bluetooth LE (battery powered).

```
┌──────────┬──────────────────────────┬───────────────┐
│  ^   ^   │  Claude is working       │  14:32  ⚡🔋📶 │
│  o   o   │  Edit: main.cpp          │   Wed Jul 09  │
│   ___    │  Desktop Buddy           │ context ▓▓▓░  │
│  \___/   │  ● ● ●                   │  323k / 1.0M  │
└──────────┴──────────────────────────┴───────────────┘
```

## Screens (tap to cycle)

1. **Buddy** — animated face + Claude status + clock/date/battery +
   context-window gauge for the most recently active session
2. **Zen clock** — big clock
3. **Claude usage** — official numbers from Anthropic's usage API:
   5-hour block, weekly all-models, weekly current-model, with reset times
4. **PC stats** — CPU and RAM

Long-press the face to pet the buddy.

## Moods

Blinks and looks around when idle; focuses with typing dots while Claude
works; wide-eyed amber when Claude is waiting on you (permission prompt);
smiles when a task finishes; frowns on errors; sleeps with dimmed backlight
when the companion is offline. Toast notifications slide in for usage
warnings (80% / 95%) and anything pushed over the serial protocol.

## Layout

```
Desktop Buddy/
├── firmware/     PlatformIO project (ESP32-S3, LVGL 9, NimBLE)
│   └── flash.bat     build + flash (uses pio.exe directly)
└── companion/    Windows-side Python app
    ├── buddy_companion.py   main app (run_buddy.bat launches it)
    ├── buddy_hook.py        Claude Code hook → event bridge
    └── buddy.log            runtime log (gitignored)
```

## Setup

**Firmware** (already flashed; for updates):
```powershell
cd firmware
.\flash.bat        # PowerShell/cmd only — Git Bash breaks the toolchain installer
```
First-time flash on a factory board: hold BOOT, tap PWR, release BOOT.
After that, flashing needs no buttons.

**Companion**: `pip install -r companion/requirements.txt`, then run
`companion\run_buddy.bat` (or add a shortcut to it in `shell:startup`).
It auto-detects the board on USB (Espressif VID) and falls back to
Bluetooth LE ("ClaudeBuddy", Nordic UART service) when unplugged — with a
30 s heartbeat that forces a reconnect if the link goes silent.

**Battery**: hold PWR ~2 s to power on (firmware latches power via the
TCA9554 expander), hold ~3 s to power off. Battery %, charge state and
USB/BLE link show top-right on every screen.

## Data sources

| What | Where it comes from |
|---|---|
| Official 5-hour / weekly limits | `GET api.anthropic.com/api/oauth/usage` with the OAuth token from `~/.claude/.credentials.json` (polled every 5 min; token auto-refreshed via `platform.claude.com/v1/oauth/token` and persisted back) |
| Live mood / tool activity | Claude Code hooks (PreToolUse, UserPromptSubmit, Notification, Stop) registered in `~/.claude/settings.json` → `buddy_hook.py` → `~/.claude/buddy_events.jsonl` |
| Context-window gauge | Last assistant turn's token usage in the newest transcript (`~/.claude/projects/**/*.jsonl`); window 1M for Fable, 200k otherwise |
| Token totals / fallback gauge | Transcript parsing (5-hour blocks; limit auto-estimated, override in `companion/buddy_config.json`: `{"block_limit_tokens": N}`) |
| Daily cost | Claude Code OpenTelemetry → companion's OTLP listener on `127.0.0.1:4318` (env vars in `~/.claude/settings.json`) |

**Secrets:** nothing sensitive lives in this folder. OAuth tokens stay in
`~/.claude/.credentials.json`; hook events in `~/.claude/`; the OTel
listener binds to localhost only. `buddy.log` (activity text) is gitignored.

## Serial/BLE protocol (one JSON per line, same on both transports)

```jsonc
{"t":"s","cpu":31,"ram":62,"clk":"14:32","date":"Wed Jul 09",
 "claude":"working","head":"Claude is working","msg":"Edit: main.cpp",
 "proj":"my-app","ctx":32,"ctxt":"323k / 1.0M",
 "use":{"pct":56,"rst":"4h 19m","blk":"38M tok","day":"41M tok · $12.40",
        "w":38,"wrst":"Jul 12","wm":61}}
{"t":"n","kind":"ok","msg":"Build finished"}      // toast: ok | err | info
{"t":"ping"}                                       // → {"t":"pong","fw":"..."}
```
`claude` states: `working`, `waiting`, `done`, `error`, `idle`, `sleep`.
Buddy → PC: `hello` on boot, `pong`, `pet`. 30 s without frames → sleep mood.

## Troubleshooting

- **Build fails with `'xtensa-esp32s3-elf-g++' is not recognized`** — build
  from PowerShell/cmd, never Git Bash (MSys breaks the toolchain installer).
- **`python -m platformio` says "No module named platformio"** — your shell's
  `%APPDATA%` is redirected; use `flash.bat` (hardcodes the pio.exe path).
- **Buddy stuck "waiting for companion"** — check `companion\buddy.log`;
  the heartbeat reconnects within ~90 s, or restart `run_buddy.bat`.
- **Weekly bars empty** — the OAuth token needs one successful refresh;
  check the log for "usage API" lines.
- **Short PWR press flashes static then dies** — normal near-empty battery
  behavior; charge over USB.
- **Touch feels rotated** — mapping in `firmware/src/lvgl_port.c`
  (`TouchInputReadCallback`).
- **Factory firmware restore** — prebuilt binaries in the
  [vendor repo](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49)
  (`Firmware/` folder).

## Feature ideas / roadmap

- Now-playing screen with touch controls (Spotify/YouTube/anything) via
  Windows SMTC (`winsdk` package) — no per-service API keys needed
- Battery-saver mood on BLE (dimmer backlight, slower updates)
- Pomodoro timer screen (touch to start/stop)
- Build/test results pushed as toasts from CI
- Use the onboard IMU: react when you pick the display up
- Speech bubble showing Claude's last reply summary
- Wi-Fi mode (companion streams over TCP, no BT needed)
