"""Desktop Buddy companion — feeds the Waveshare ESP32-S3 buddy display.

Runs on Windows. Every couple of seconds it sends a JSON status line over
USB serial: CPU/RAM usage, clock, what Claude Code is currently doing
(inferred from session transcript activity under ~/.claude/projects), and
what OpenAI's Codex CLI is doing (inferred from its session logs under
~/.codex/sessions — see CodexWatcher).

Usage:
    python buddy_companion.py            # auto-detect the board's COM port
    python buddy_companion.py --port COM7
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

try:
    import psutil
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Missing dependencies. Run:  pip install -r requirements.txt")
    sys.exit(1)

ESPRESSIF_VID = 0x303A          # native USB CDC of the ESP32-S3
CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"
EVENTS_FILE = Path.home() / ".claude" / "buddy_events.jsonl"  # optional, fed by hooks
CONFIG_FILE = Path(__file__).parent / "buddy_config.json"     # optional overrides

STATUS_INTERVAL = 2.0           # seconds between status frames
ACTIVE_WINDOW = 10              # transcript touched within N s => Claude is working
SESSION_WINDOW = 45             # sessions "concurrent" if touched within N s
DONE_WINDOW = 25                # activity stopped within N s => show "done" smile
RESCAN_INTERVAL = 60            # how often to re-list transcript files
FRESH_FILE_AGE = 6 * 3600       # only watch transcripts touched in the last 6h

BLOCK_HOURS = 5                 # Claude's rolling usage-limit window
USAGE_HISTORY_DAYS = 14         # history used to auto-estimate your block limit
MIN_LIMIT_TOKENS = 500_000      # floor for the auto-estimated limit


def find_port(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for p in serial.tools.list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


class SerialTransport:
    """USB serial link to the buddy."""

    def __init__(self, port: str) -> None:
        self.name = f"USB {port}"
        self._ser = serial.Serial(port, 115200, timeout=0, write_timeout=2)

    def write_line(self, line: str, reliable: bool = False) -> None:
        self._ser.write((line + "\n").encode())

    def read_text(self) -> str:
        return self._ser.read(4096).decode("utf-8", errors="replace")

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


# Nordic UART Service UUIDs (matches the firmware's BLE service)
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # we write here
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # buddy notifies here


class BleTransport:
    """Bluetooth LE link to the buddy (used when no USB cable is attached)."""

    def __init__(self) -> None:
        import asyncio
        import threading
        from bleak import BleakClient, BleakScanner

        self.name = "BLE"
        self._asyncio = asyncio
        self._rx_lock = threading.Lock()
        self._rx_buf = ""
        self._loop = asyncio.new_event_loop()
        threading.Thread(target=self._loop.run_forever, daemon=True).start()

        async def connect():
            dev = await BleakScanner.find_device_by_filter(
                lambda d, ad: ad.local_name == "ClaudeBuddy"
                or d.name == "ClaudeBuddy"
                or NUS_SERVICE in (ad.service_uuids or []),
                timeout=10)
            if dev is None:
                raise RuntimeError("no ClaudeBuddy advertising over BLE")
            client = BleakClient(dev, timeout=20)
            for attempt in range(3):   # first GATT connect often times out on Windows
                try:
                    await client.connect()
                    break
                except Exception:
                    if attempt == 2:
                        raise
                    await asyncio.sleep(2)
            await client.start_notify(NUS_TX, self._on_notify)
            return client

        fut = asyncio.run_coroutine_threadsafe(connect(), self._loop)
        self._client = fut.result(timeout=30)
        self.name = f"BLE {self._client.address}"

    def _on_notify(self, _char, data: bytearray) -> None:
        with self._rx_lock:
            self._rx_buf += data.decode("utf-8", errors="replace")

    def write_line(self, line: str, reliable: bool = False) -> None:
        """reliable=True uses acknowledged writes — BLE may silently drop
        write-without-response packets under load (album art bursts)."""
        if not self._client.is_connected:
            raise OSError("BLE disconnected")
        data = (line + "\n").encode()
        mtu = getattr(self._client, "mtu_size", 23) or 23
        chunk = max(20, mtu - 3)

        async def send():
            for i in range(0, len(data), chunk):
                await self._client.write_gatt_char(NUS_RX, data[i:i + chunk],
                                                   response=reliable)

        fut = self._asyncio.run_coroutine_threadsafe(send(), self._loop)
        fut.result(timeout=15)

    def read_text(self) -> str:
        with self._rx_lock:
            out, self._rx_buf = self._rx_buf, ""
        return out

    def close(self) -> None:
        try:
            fut = self._asyncio.run_coroutine_threadsafe(
                self._client.disconnect(), self._loop)
            fut.result(timeout=5)
        except Exception:
            pass


def open_transport(args) -> "SerialTransport | BleTransport | None":
    """USB when available, otherwise BLE (if bleak is installed)."""
    if not args.ble:
        port = find_port(args.port)
        if port:
            return SerialTransport(port)
    try:
        import bleak  # noqa: F401
    except ImportError:
        return None
    try:
        return BleTransport()
    except Exception as e:
        print(f"BLE: {e!r}")
        return None


class ClaudeWatcher:
    """Infers Claude Code state from transcript file activity."""

    def __init__(self) -> None:
        self._files: list[Path] = []
        self._last_scan = 0.0
        self._last_active = 0.0
        self._active_file: Path | None = None
        self._newest_file: Path | None = None

    def _rescan(self) -> None:
        self._files = []
        if not CLAUDE_PROJECTS.is_dir():
            return
        cutoff = time.time() - FRESH_FILE_AGE
        try:
            for proj_dir in CLAUDE_PROJECTS.iterdir():
                if not proj_dir.is_dir():
                    continue
                for f in proj_dir.glob("*.jsonl"):
                    try:
                        if f.stat().st_mtime > cutoff:
                            self._files.append(f)
                    except OSError:
                        pass
        except OSError:
            pass

    @staticmethod
    def _tail_line(path: Path) -> dict | None:
        """Last JSON object in the transcript, or None."""
        try:
            with open(path, "rb") as fh:
                fh.seek(0, os.SEEK_END)
                size = fh.tell()
                fh.seek(max(0, size - 16384))
                chunk = fh.read().decode("utf-8", errors="replace")
            for line in reversed(chunk.strip().splitlines()):
                line = line.strip()
                if line.startswith("{"):
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError:
                        continue
        except OSError:
            pass
        return None

    @staticmethod
    def _describe(entry: dict | None) -> str:
        """Short human text for what Claude is doing right now."""
        if not entry:
            return "working"
        try:
            msg = entry.get("message") or {}
            content = msg.get("content")
            if isinstance(content, list):
                for item in content:
                    if isinstance(item, dict) and item.get("type") == "tool_use":
                        name = item.get("name", "a tool")
                        desc = ""
                        inp = item.get("input") or {}
                        if isinstance(inp, dict):
                            desc = inp.get("description") or inp.get("file_path") or ""
                            if isinstance(desc, str) and "\\" in desc:
                                desc = desc.rsplit("\\", 1)[-1]
                        return f"{name}: {desc}"[:80] if desc else f"using {name}"
                    if isinstance(item, dict) and item.get("type") == "text":
                        return "writing a reply"
            if entry.get("type") == "user":
                return "reading your message"
        except Exception:
            pass
        return "working"

    def status(self) -> tuple[str, str, str]:
        """Returns (state, msg, project). With several concurrent sessions the
        project field becomes e.g. "2 sessions: buddy, tax-app"."""
        now = time.time()
        if now - self._last_scan > RESCAN_INTERVAL or not self._files:
            self._rescan()
            self._last_scan = now

        newest_mtime = 0.0
        newest_file: Path | None = None
        recent: list[tuple[float, Path]] = []   # sessions active in SESSION_WINDOW
        for f in self._files:
            try:
                m = f.stat().st_mtime
            except OSError:
                continue
            if m > newest_mtime:
                newest_mtime, newest_file = m, f
            if now - m <= SESSION_WINDOW:
                recent.append((m, f))

        age = now - newest_mtime if newest_file else 1e9
        self._newest_file = newest_file

        # Optional richer state from Claude Code hooks (see README)
        hook = self._hook_state(now)
        if hook:
            return hook

        if age <= ACTIVE_WINDOW and newest_file:
            self._last_active = now
            self._active_file = newest_file
            entry = self._tail_line(newest_file)
            proj = self._session_summary(recent, newest_file, entry)
            return "working", self._describe(entry), proj

        if self._last_active and now - self._last_active <= DONE_WINDOW:
            proj = self._project_name(self._active_file, None) if self._active_file else ""
            return "done", "finished a task", proj

        return "idle", "", ""

    def context(self) -> tuple[int, str] | None:
        """Context-window usage of the most recent session: (pct, "323k / 1.0M").
        Sums the input-side token counts of the last assistant turn."""
        if self._newest_file is None:
            return None
        try:
            with open(self._newest_file, "rb") as fh:
                fh.seek(0, os.SEEK_END)
                size = fh.tell()
                fh.seek(max(0, size - 131072))
                chunk = fh.read().decode("utf-8", errors="replace")
        except OSError:
            return None
        for line in reversed(chunk.strip().splitlines()):
            if '"usage"' not in line:
                continue
            try:
                entry = json.loads(line)
                msg = entry.get("message") or {}
                usage = msg.get("usage")
                if not usage:
                    continue
                ctx = (usage.get("input_tokens", 0)
                       + usage.get("cache_read_input_tokens", 0)
                       + usage.get("cache_creation_input_tokens", 0)
                       + usage.get("output_tokens", 0))
                if ctx <= 0:
                    continue
                model = str(msg.get("model", "")).lower()
                # every current Claude model has a 1M context window by default
                # except Haiku, which stays at 200k
                window = 200_000 if "haiku" in model else 1_000_000
                pct = round(ctx / window * 100)
                return pct, f"{_fmt_tokens(ctx)} / {_fmt_tokens(window)}"
            except (json.JSONDecodeError, TypeError, ValueError):
                continue
        return None

    def _session_summary(self, recent: list[tuple[float, Path]],
                         newest: Path, newest_entry: dict | None) -> str:
        """One project name, or 'N sessions: a, b' when several are active."""
        names: list[str] = []
        for _, f in sorted(recent, reverse=True):
            name = (self._project_name(f, newest_entry) if f == newest
                    else self._project_name(f, self._tail_line(f)))
            if name and name not in names:
                names.append(name)
        if len(names) <= 1:
            return names[0] if names else self._project_name(newest, newest_entry)
        return f"{len(names)} sessions: " + ", ".join(names[:3])

    @staticmethod
    def _project_name(path: Path | None, entry: dict | None) -> str:
        if entry and isinstance(entry.get("cwd"), str):
            return os.path.basename(entry["cwd"])
        if path is not None:
            # directory names encode the project path with '-' separators
            return path.parent.name.split("-")[-1]
        return ""

    # How long each hook-reported state stays authoritative. "waiting" lingers
    # because a permission prompt sits there until you answer it.
    _HOOK_TTL = {"working": 15, "waiting": 300, "done": 20}

    @classmethod
    def _hook_state(cls, now: float) -> tuple[str, str, str] | None:
        """Latest event from Claude Code hooks (buddy_hook.py), if still fresh.
        Events look like {"ts": epoch, "state": ..., "msg": ..., "proj": ...}."""
        try:
            if not EVENTS_FILE.is_file():
                return None
            if now - EVENTS_FILE.stat().st_mtime > max(cls._HOOK_TTL.values()):
                return None
            with open(EVENTS_FILE, "rb") as fh:
                fh.seek(max(0, fh.seek(0, os.SEEK_END) - 4096))
                lines = fh.read().decode("utf-8", errors="replace").strip().splitlines()
            for line in reversed(lines):
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                state = ev.get("state", "working")
                age = now - float(ev.get("ts", 0))
                if age <= cls._HOOK_TTL.get(state, 15):
                    return state, ev.get("msg", ""), ev.get("proj", "")
                return None  # newest event expired; fall back to transcripts
        except (OSError, ValueError):
            pass
        return None


CODEX_SESSIONS = Path.home() / ".codex" / "sessions"


class CodexWatcher:
    """Context-window and usage-limit gauges for OpenAI's Codex CLI/desktop
    app, read straight from its local session logs (no API/OAuth needed).
    Codex writes a "token_count" event after every turn to
    ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl with both the running
    token total against the model's context window, and OpenAI's own
    rate-limit percentage for the account (the same numbers Codex itself
    shows)."""

    def __init__(self) -> None:
        self._newest_file: Path | None = None
        self._last_scan = 0.0
        self._last_active = 0.0

    def _rescan(self) -> None:
        self._last_scan = time.time()
        for days_back in (0, 1):
            day = datetime.now() - timedelta(days=days_back)
            day_dir = CODEX_SESSIONS / f"{day:%Y/%m/%d}"
            try:
                files = list(day_dir.glob("rollout-*.jsonl"))
            except OSError:
                files = []
            if files:
                self._newest_file = max(files, key=lambda f: f.stat().st_mtime)
                return
        self._newest_file = None

    @staticmethod
    def _fmt_reset(epoch: float) -> str:
        try:
            s = int(epoch - time.time())
            if s <= 0:
                return "now"
            d, rem = divmod(s, 86400)
            h, m = divmod(rem, 3600)
            return f"{d}d {h}h" if d else f"{h}h {m // 60:02d}m"
        except (TypeError, ValueError):
            return ""

    def snapshot(self) -> dict:
        """{"ctx", "ctxt", "pct", "rst"} — only fields that parsed."""
        if time.time() - self._last_scan > RESCAN_INTERVAL or self._newest_file is None:
            self._rescan()
        if self._newest_file is None:
            return {}
        try:
            with open(self._newest_file, "rb") as fh:
                fh.seek(0, os.SEEK_END)
                size = fh.tell()
                fh.seek(max(0, size - 32768))
                chunk = fh.read().decode("utf-8", errors="replace")
        except OSError:
            return {}
        for line in reversed(chunk.strip().splitlines()):
            if '"token_count"' not in line:
                continue
            try:
                entry = json.loads(line)
                info = entry["payload"]["info"]
            except (json.JSONDecodeError, KeyError, TypeError):
                continue
            out: dict = {}
            try:
                # last_token_usage is what was actually sent to the model on
                # the most recent turn (i.e. current context fill).
                # total_token_usage is a cumulative session total and can
                # exceed the context window entirely — not usable here.
                ctx = info["last_token_usage"]["total_tokens"]
                window = info["model_context_window"]
                if window > 0:
                    out["ctx"] = round(ctx / window * 100)
                    out["ctxt"] = f"{_fmt_tokens(ctx)} / {_fmt_tokens(window)}"
            except (KeyError, TypeError, ZeroDivisionError):
                pass
            try:
                primary = entry["payload"]["rate_limits"]["primary"]
                out["pct"] = round(primary["used_percent"])
                if primary.get("resets_at"):
                    out["rst"] = self._fmt_reset(float(primary["resets_at"]))
            except (KeyError, TypeError, ValueError):
                pass
            return out
        return {}

    @staticmethod
    def _describe(payload: dict) -> str:
        """Short human text for what Codex is doing right now, from the
        inner payload.type of the last event line."""
        kind = payload.get("type")
        if kind == "function_call":
            return f"using {payload.get('name', 'a tool')}"[:80]
        if kind == "agent_message":
            return "writing a reply"
        if kind == "user_message":
            return "reading your message"
        if kind == "reasoning":
            return "thinking"
        return "working"

    def status(self) -> tuple[str, str, str]:
        """Returns (state, msg, project), mirroring ClaudeWatcher.status()
        but mtime-only — Codex has no hook-file equivalent and no reliable
        "needs approval" signal in the logs, so only working/done/idle."""
        now = time.time()
        if now - self._last_scan > RESCAN_INTERVAL or self._newest_file is None:
            self._rescan()
        if self._newest_file is None:
            return "idle", "", ""
        try:
            mtime = self._newest_file.stat().st_mtime
        except OSError:
            return "idle", "", ""
        age = now - mtime

        if age > ACTIVE_WINDOW:
            if self._last_active and now - self._last_active <= DONE_WINDOW:
                return "done", "finished a task", ""
            return "idle", "", ""
        self._last_active = now

        try:
            with open(self._newest_file, "rb") as fh:
                fh.seek(0, os.SEEK_END)
                size = fh.tell()
                fh.seek(max(0, size - 16384))
                chunk = fh.read().decode("utf-8", errors="replace")
        except OSError:
            return "working", "working", ""

        msg = "working"
        proj = ""
        for line in reversed(chunk.strip().splitlines()):
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            payload = entry.get("payload")
            if not isinstance(payload, dict):
                continue
            if not proj and isinstance(payload.get("cwd"), str):
                proj = os.path.basename(payload["cwd"])
            if msg == "working" and payload.get("type") not in (
                    "session_meta", "turn_context", "task_started"):
                msg = self._describe(payload)
            if msg != "working" and proj:
                break
        return "working", msg, proj


class UsageAPI:
    """Official rate-limit numbers from Anthropic's OAuth usage endpoint —
    the same 5-hour / weekly percentages Claude Code shows in its usage panel.

    Reads the OAuth token from ~/.claude/.credentials.json. If it's expired,
    tries the standard refresh flow (with a long cooldown — the endpoint
    rate-limits hard) and persists the result. Also picks up fresh tokens
    whenever Claude Code rewrites the file. Returns {} until data arrives;
    the buddy falls back to transcript-based numbers meanwhile.
    """

    CRED = Path.home() / ".claude" / ".credentials.json"
    CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"  # Claude Code public client
    UA = "claude-code/2.1.0"
    FETCH_INTERVAL = 300        # be polite: endpoint 429s below ~180s
    REFRESH_COOLDOWN = 1800

    def __init__(self) -> None:
        self._data: dict = {}
        self._last_fetch = 0.0
        self._last_refresh_try = 0.0
        self._announced = False

    def _load(self) -> dict | None:
        try:
            return json.loads(self.CRED.read_text())
        except (OSError, json.JSONDecodeError):
            return None

    def _refresh(self, creds: dict) -> str | None:
        import urllib.request
        now = time.time()
        if now - self._last_refresh_try < self.REFRESH_COOLDOWN:
            return None
        self._last_refresh_try = now
        oauth = creds["claudeAiOauth"]
        body = json.dumps({"grant_type": "refresh_token",
                           "refresh_token": oauth["refreshToken"],
                           "client_id": self.CLIENT_ID}).encode()
        req = urllib.request.Request(
            "https://platform.claude.com/v1/oauth/token", data=body,
            headers={"Content-Type": "application/json",
                     "User-Agent": "claude-cli/2.1.0 (external, cli)"})
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                tok = json.loads(r.read())
            oauth["accessToken"] = tok["access_token"]
            if tok.get("refresh_token"):
                oauth["refreshToken"] = tok["refresh_token"]
            oauth["expiresAt"] = int(time.time() * 1000) + tok.get("expires_in", 3600) * 1000
            self.CRED.write_text(json.dumps(creds))
            print("usage API: token refreshed")
            return oauth["accessToken"]
        except Exception as e:
            print(f"usage API: token refresh failed ({e}); will retry later")
            return None

    def poll(self) -> None:
        import urllib.error
        import urllib.request
        now = time.time()
        if now - self._last_fetch < self.FETCH_INTERVAL:
            return
        self._last_fetch = now

        creds = self._load()
        if not creds or "claudeAiOauth" not in creds:
            return
        oauth = creds["claudeAiOauth"]
        token = oauth.get("accessToken")
        if oauth.get("expiresAt", 0) < now * 1000 + 60000:
            token = self._refresh(creds)
            if not token:
                return

        req = urllib.request.Request(
            "https://api.anthropic.com/api/oauth/usage",
            headers={"Authorization": f"Bearer {token}",
                     "anthropic-beta": "oauth-2025-04-20",
                     "User-Agent": self.UA})
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                self._data = json.loads(r.read())
            if not self._announced:
                self._announced = True
                print(f"usage API: live official data: {self.snapshot()}")
        except urllib.error.HTTPError as e:
            if e.code == 401:
                self._refresh(creds)
            # 429 etc: keep old data, try again next interval

    @staticmethod
    def _pct(node) -> int:
        try:
            return round(float(node.get("utilization", -1)))
        except (AttributeError, TypeError, ValueError):
            return -1

    @staticmethod
    def _when(node, fmt: str) -> str:
        try:
            dt = datetime.fromisoformat(node["resets_at"].replace("Z", "+00:00"))
            dt = dt.astimezone()
            if fmt == "remain":
                s = int(dt.timestamp() - time.time())
                return f"{s // 3600}h {(s % 3600) // 60:02d}m" if s > 0 else "now"
            return dt.strftime("%b %d")
        except (KeyError, TypeError, ValueError, AttributeError):
            return ""

    def snapshot(self) -> dict:
        """{"pct","rst","w","wrst","wm"} — only fields that are known."""
        d = self._data
        if not d:
            return {}
        out: dict = {}
        # Preferred: the "limits" array (same source the app's panel renders)
        for lim in d.get("limits") or []:
            kind = lim.get("kind")
            pct = lim.get("percent")
            if not isinstance(pct, (int, float)):
                continue
            if kind == "session":
                out["pct"] = round(pct)
                out["rst"] = self._when(lim, "remain")
            elif kind == "weekly_all":
                out["w"] = round(pct)
                out["wrst"] = self._when(lim, "date")
            elif kind == "weekly_scoped":
                out["wm"] = round(pct)
        if out:
            return out
        # Fallback: top-level five_hour / seven_day objects
        five = d.get("five_hour")
        if five and self._pct(five) >= 0:
            out["pct"] = self._pct(five)
            out["rst"] = self._when(five, "remain")
        week = d.get("seven_day")
        if week and self._pct(week) >= 0:
            out["w"] = self._pct(week)
            out["wrst"] = self._when(week, "date")
        return out


class MediaWatcher:
    """Now-playing info + transport control via Windows' system media API
    (SMTC) — sees anything that shows in the volume-flyout media card:
    Spotify, YouTube in a browser, VLC, etc. No per-service API keys."""

    APP_NAMES = {"spotify": "Spotify", "chrome": "Chrome", "msedge": "Edge",
                 "edge": "Edge", "firefox": "Firefox", "vlc": "VLC",
                 "apple": "Apple Music"}

    ART_SIZE = 120

    def __init__(self) -> None:
        import asyncio
        import threading
        self._asyncio = asyncio
        self._lock = threading.Lock()
        self._snap: dict | None = None
        self._session = None
        self._art: bytes | None = None      # RGB565-LE pixels for current track
        self._art_key = ""
        self._loop = asyncio.new_event_loop()
        threading.Thread(target=self._thread_main, daemon=True).start()

    def _thread_main(self) -> None:
        self._asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._poll_forever())
        except Exception as e:
            print(f"media watcher stopped: {e!r}")

    def _app_name(self, aumid: str) -> str:
        low = (aumid or "").lower()
        for key, name in self.APP_NAMES.items():
            if key in low:
                return name
        return aumid or ""

    async def _poll_forever(self) -> None:
        from winrt.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager,
        )
        mgr = await Manager.request_async()
        while True:
            snap = None
            try:
                s = mgr.get_current_session()
                if s is not None:
                    info = await s.try_get_media_properties_async()
                    status = int(s.get_playback_info().playback_status)
                    tl = s.get_timeline_properties()
                    title = (info.title or "").strip()
                    if status in (4, 5) and title:   # 4 playing, 5 paused
                        pos = tl.position.total_seconds()
                        dur = tl.end_time.total_seconds()
                        # players freeze their reported position; extrapolate
                        # from when they last updated it to get the live value
                        if status == 4:
                            try:
                                from datetime import datetime, timezone
                                lu = tl.last_updated_time
                                elapsed = (datetime.now(timezone.utc)
                                           - lu).total_seconds()
                                if 0 <= elapsed < 3600:
                                    pos += elapsed
                            except (AttributeError, TypeError, OSError):
                                pass
                        if dur > 0:
                            pos = min(pos, dur)
                        snap = {
                            "t": title[:60],
                            "a": (info.artist or "")[:40],
                            "app": self._app_name(s.source_app_user_model_id),
                            "st": "playing" if status == 4 else "paused",
                            "pos": int(pos),
                            "dur": int(dur),
                        }
                        key = f"{snap['t']}|{snap['a']}"
                        if key != self._art_key:
                            art = await self._fetch_art(info)
                            with self._lock:
                                self._art_key = key
                                self._art = art
                self._session = s
            except Exception:
                self._session = None
            with self._lock:
                self._snap = snap
            await self._asyncio.sleep(2)

    async def _fetch_art(self, info) -> bytes | None:
        """Album thumbnail -> 120x120 RGB565 little-endian raw pixels."""
        try:
            import io
            from PIL import Image
            from winrt.windows.storage.streams import Buffer, InputStreamOptions
            ref = info.thumbnail
            if ref is None:
                return None
            stream = await ref.open_read_async()
            size = stream.size
            if not size or size > 4_000_000:
                return None
            buf = Buffer(size)
            await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
            img = Image.open(io.BytesIO(bytes(memoryview(buf))))
            img = img.convert("RGB").resize((self.ART_SIZE, self.ART_SIZE))
            rgb = img.tobytes()
            out = bytearray(self.ART_SIZE * self.ART_SIZE * 2)
            for p in range(0, len(rgb), 3):
                v = ((rgb[p] >> 3) << 11) | ((rgb[p + 1] >> 2) << 5) | (rgb[p + 2] >> 3)
                o = (p // 3) * 2
                out[o] = v & 0xFF
                out[o + 1] = v >> 8
            return bytes(out)
        except Exception as e:
            print(f"album art fetch failed: {e!r}")
            return None

    def snapshot(self) -> dict | None:
        with self._lock:
            return dict(self._snap) if self._snap else None

    def art(self) -> tuple[str, bytes | None]:
        """(track key, pixels) — pixels for the current track, or None."""
        with self._lock:
            return self._art_key, self._art

    def command(self, cmd: str) -> None:
        self._asyncio.run_coroutine_threadsafe(self._do_command(cmd), self._loop)

    async def _do_command(self, cmd: str) -> None:
        s = self._session
        if s is None:
            return
        try:
            if cmd == "play":
                await s.try_toggle_play_pause_async()
            elif cmd == "next":
                await s.try_skip_next_async()
            elif cmd == "prev":
                await s.try_skip_previous_async()
        except Exception as e:
            print(f"media command {cmd} failed: {e!r}")


class OtelReceiver:
    """Tiny OTLP/HTTP (json) listener for Claude Code's built-in telemetry.

    Claude Code (with CLAUDE_CODE_ENABLE_TELEMETRY=1) POSTs metrics to
    /v1/metrics. Counters are cumulative per session, so we track the latest
    value per (session, datapoint) and sum them for an exact daily cost.
    """

    PORT = 4318

    def __init__(self) -> None:
        import threading
        self._lock = threading.Lock()
        self._cost: dict[tuple, float] = {}     # (session, attrs) -> latest USD
        self._day = datetime.now().date()
        self._server = None
        self._start(threading)

    def _start(self, threading) -> None:
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        rx = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                if self.path.rstrip("/").endswith("metrics"):
                    try:
                        rx._ingest(json.loads(body))
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        pass
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *a):  # silence request logging
                pass

        try:
            self._server = ThreadingHTTPServer(("127.0.0.1", self.PORT), Handler)
            threading.Thread(target=self._server.serve_forever, daemon=True).start()
            print(f"OTel receiver listening on 127.0.0.1:{self.PORT}")
        except OSError as e:
            print(f"OTel receiver disabled (port {self.PORT} busy?): {e}")

    def _ingest(self, payload: dict) -> None:
        with self._lock:
            self._roll_day()
            for rm in payload.get("resourceMetrics", []):
                session = json.dumps(
                    (rm.get("resource") or {}).get("attributes", []), sort_keys=True)
                for sm in rm.get("scopeMetrics", []):
                    for metric in sm.get("metrics", []):
                        if metric.get("name") != "claude_code.cost.usage":
                            continue
                        for dp in (metric.get("sum") or {}).get("dataPoints", []):
                            val = float(dp.get("asDouble", dp.get("asInt", 0)))
                            key = (session, json.dumps(dp.get("attributes", []),
                                                       sort_keys=True))
                            self._cost[key] = val

    def _roll_day(self) -> None:
        today = datetime.now().date()
        if today != self._day:
            self._day = today
            self._cost.clear()

    def day_cost(self) -> float | None:
        with self._lock:
            self._roll_day()
            return sum(self._cost.values()) if self._cost else None


def _fmt_tokens(n: float) -> str:
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}k"
    return str(int(n))


class UsageTracker:
    """Aggregates token usage from Claude Code transcripts into 5-hour blocks
    (the shape of Claude's rate-limit window). The block limit is taken from
    buddy_config.json {"block_limit_tokens": N} if present, otherwise
    auto-estimated as the biggest block seen in the last two weeks."""

    def __init__(self) -> None:
        self._entries: list[tuple[float, int]] = []   # (epoch, tokens)
        self._offsets: dict[Path, int] = {}
        self._limit_override = self._read_config_limit()
        self._last_discover = 0.0
        self._watched: list[Path] = []
        self.day_cost = None       # optional callable installed by OtelReceiver
        self._seed_history()

    @staticmethod
    def _read_config_limit() -> int | None:
        try:
            cfg = json.loads(CONFIG_FILE.read_text())
            v = int(cfg.get("block_limit_tokens", 0))
            return v if v > 0 else None
        except (OSError, ValueError, json.JSONDecodeError):
            return None

    @staticmethod
    def _entry_usage(line: str) -> tuple[float, int] | None:
        """(timestamp, total tokens) for an assistant entry, else None."""
        if '"usage"' not in line:
            return None
        try:
            obj = json.loads(line)
            usage = (obj.get("message") or {}).get("usage")
            ts_str = obj.get("timestamp")
            if not usage or not ts_str:
                return None
            ts = datetime.fromisoformat(ts_str.replace("Z", "+00:00")).timestamp()
            tokens = (usage.get("input_tokens", 0) + usage.get("output_tokens", 0)
                      + usage.get("cache_creation_input_tokens", 0)
                      + usage.get("cache_read_input_tokens", 0))
            return (ts, tokens) if tokens > 0 else None
        except (json.JSONDecodeError, ValueError, TypeError):
            return None

    def _discover_files(self) -> None:
        cutoff = time.time() - USAGE_HISTORY_DAYS * 86400
        found: list[Path] = []
        try:
            for proj_dir in CLAUDE_PROJECTS.iterdir():
                if proj_dir.is_dir():
                    for f in proj_dir.glob("*.jsonl"):
                        try:
                            if f.stat().st_mtime > cutoff:
                                found.append(f)
                        except OSError:
                            pass
        except OSError:
            pass
        self._watched = found

    def _seed_history(self) -> None:
        """One-time full read of recent transcripts at startup."""
        self._discover_files()
        self._last_discover = time.time()
        for f in self._watched:
            try:
                with open(f, "r", encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        e = self._entry_usage(line)
                        if e:
                            self._entries.append(e)
                    self._offsets[f] = fh.tell()
            except OSError:
                pass
        self._entries.sort()

    def poll(self) -> None:
        """Incremental read of anything appended since last poll."""
        now = time.time()
        if now - self._last_discover > RESCAN_INTERVAL:
            self._discover_files()
            self._last_discover = now
        added = False
        for f in self._watched:
            try:
                size = f.stat().st_size
                offset = self._offsets.get(f, 0)
                if size <= offset:
                    if size < offset:      # file replaced/truncated
                        self._offsets[f] = 0
                    continue
                with open(f, "r", encoding="utf-8", errors="replace") as fh:
                    fh.seek(offset)
                    for line in fh:
                        e = self._entry_usage(line)
                        if e:
                            self._entries.append(e)
                            added = True
                    self._offsets[f] = fh.tell()
            except OSError:
                pass
        if added:
            self._entries.sort()
        # prune old entries
        cutoff = now - USAGE_HISTORY_DAYS * 86400
        while self._entries and self._entries[0][0] < cutoff:
            self._entries.pop(0)

    def _blocks(self) -> list[tuple[float, int]]:
        """[(block_start_epoch, total_tokens)] — blocks start at the top of
        the hour of the first activity after the previous block expires."""
        blocks: list[tuple[float, int]] = []
        start, total = None, 0
        for ts, tok in self._entries:
            if start is None or ts >= start + BLOCK_HOURS * 3600:
                if start is not None:
                    blocks.append((start, total))
                start = ts - (ts % 3600)   # floor to the hour
                total = 0
            total += tok
        if start is not None:
            blocks.append((start, total))
        return blocks

    def snapshot(self) -> dict:
        """Fields for the status frame's "use" object."""
        now = time.time()
        blocks = self._blocks()

        cur_start, cur_total = None, 0
        if blocks and now < blocks[-1][0] + BLOCK_HOURS * 3600:
            cur_start, cur_total = blocks[-1]

        limit = self._limit_override or max(
            [t for _, t in blocks] + [MIN_LIMIT_TOKENS])

        pct = round(cur_total / limit * 100) if cur_start is not None else 0

        rst = ""
        if cur_start is not None:
            remain = int(cur_start + BLOCK_HOURS * 3600 - now)
            rst = f"{remain // 3600}h {(remain % 3600) // 60:02d}m"

        midnight = datetime.now().replace(hour=0, minute=0, second=0,
                                          microsecond=0).timestamp()
        day_total = sum(t for ts, t in self._entries if ts >= midnight)

        day = _fmt_tokens(day_total) + " tok"
        if self.day_cost is not None:
            cost = self.day_cost()
            if cost is not None:
                day += f" · ${cost:.2f}"

        return {
            "pct": pct,
            "rst": rst,
            "blk": _fmt_tokens(cur_total) + " tok",
            "day": day,
        }


HEADLINES = {
    "working": "Claude is working",
    "done": "all done!",
    "waiting": "Claude needs you",
    "error": "something failed",
    "idle": "standing by",
    "sleep": "resting",
}

CODEX_HEADLINES = {
    "working": "Codex is working",
    "done": "Codex finished a task",
    "idle": "Codex standing by",
}


def run(transport, usage: UsageTracker, usage_api: "UsageAPI | None" = None,
        media: "MediaWatcher | None" = None) -> None:
    watcher = ClaudeWatcher()
    codex = CodexWatcher()
    usage_api = usage_api or UsageAPI()
    psutil.cpu_percent(interval=None)  # prime the counter

    print(f"connected to buddy via {transport.name}")
    last_state = ""
    last_pct = 0
    last_heard = time.time()
    last_ping = 0.0
    last_art_key = ""
    art_attempts = 0
    ART_RAW_CHUNK = 480

    def send_art(art) -> None:
        import base64
        n = (len(art) + ART_RAW_CHUNK - 1) // ART_RAW_CHUNK
        for i in range(n):
            chunk = art[i * ART_RAW_CHUNK:(i + 1) * ART_RAW_CHUNK]
            transport.write_line(json.dumps(
                {"t": "art", "w": media.ART_SIZE, "h": media.ART_SIZE,
                 "seq": i, "n": n, "d": base64.b64encode(chunk).decode()}))
            time.sleep(0.015)
        print(f"[{datetime.now():%H:%M:%S}] sent album art ({n} chunks)")
    try:
        while True:
            state, msg, proj = watcher.status()
            usage.poll()
            use = usage.snapshot()
            usage_api.poll()
            use.update(usage_api.snapshot())   # official numbers win
            now = datetime.now()
            frame = {
                "t": "s",
                "cpu": round(psutil.cpu_percent(interval=None)),
                "ram": round(psutil.virtual_memory().percent),
                "clk": now.strftime("%H:%M"),
                "date": now.strftime("%a %b %d"),
                "claude": state,
                "head": HEADLINES.get(state, state),
                "msg": msg,
                "proj": proj,
                "use": use,
            }
            ctx = watcher.context()
            if ctx:
                frame["ctx"], frame["ctxt"] = ctx
            cdx = codex.snapshot()
            cstate, cmsg, cproj = codex.status()
            cdx["state"] = cstate
            cdx["head"] = CODEX_HEADLINES.get(cstate, cstate)
            cdx["msg"] = cmsg
            cdx["proj"] = cproj
            frame["cdx"] = cdx
            if media:
                m = media.snapshot()
                if m:
                    frame["med"] = m
            transport.write_line(json.dumps(frame))

            # stream album art once per track change
            if media:
                key, art = media.art()
                if key != last_art_key:
                    last_art_key = key
                    art_attempts = 0
                    if art:
                        art_attempts = 1
                        send_art(art)

            # warn when crossing usage thresholds
            for threshold, kind in ((80, "info"), (95, "err")):
                if last_pct < threshold <= use["pct"]:
                    toast = {"t": "n", "kind": kind,
                             "msg": f"Claude usage {use['pct']}% - resets in {use['rst']}"}
                    transport.write_line(json.dumps(toast))
                    print(f"[{now:%H:%M:%S}] usage warning: {use['pct']}%")
            last_pct = use["pct"]

            if state != last_state:
                print(f"[{now:%H:%M:%S}] {state}" + (f" — {msg}" if msg else ""))
                last_state = state

            # heartbeat: buddy answers pings; silence means a dead link
            # (e.g. the board rebooted and Windows kept a stale handle)
            if time.time() - last_ping > 30:
                last_ping = time.time()
                transport.write_line('{"t":"ping"}')
            if time.time() - last_heard > 90:
                raise OSError("no heartbeat from buddy for 90s")

            # read anything the buddy sends back (e.g. pets)
            for raw in transport.read_text().splitlines():
                raw = raw.strip()
                if not raw.startswith("{"):
                    continue
                try:
                    ev = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                last_heard = time.time()
                if ev.get("t") == "mc" and media:
                    media.command(str(ev.get("cmd", "")))
                    print(f"[{now:%H:%M:%S}] media: {ev.get('cmd')}")
                elif ev.get("t") == "artok":
                    print(f"[{now:%H:%M:%S}] buddy: album art displayed")
                elif ev.get("t") == "artdrop":
                    print(f"[{now:%H:%M:%S}] buddy: art incomplete "
                          f"({ev.get('got')}/{ev.get('n')} chunks)")
                    if media and art_attempts < 3:
                        _, art = media.art()
                        if art:
                            art_attempts += 1
                            print(f"[{now:%H:%M:%S}] resending art "
                                  f"(attempt {art_attempts})")
                            send_art(art)
                elif ev.get("t") == "pet":
                    print(f"[{now:%H:%M:%S}] you petted the buddy \\(^-^)/")
                elif ev.get("t") == "hello":
                    print(f"[{now:%H:%M:%S}] buddy firmware {ev.get('fw')} says hello")
                elif ev.get("t") == "pong" and ev.get("imu"):
                    print(f"[{now:%H:%M:%S}] imu {ev['imu']}")

            time.sleep(STATUS_INTERVAL)
    finally:
        transport.close()


def main() -> None:
    ap = argparse.ArgumentParser(description="Desktop Buddy companion")
    ap.add_argument("--port", help="COM port (default: auto-detect ESP32-S3)")
    ap.add_argument("--ble", action="store_true", help="force Bluetooth LE")
    args = ap.parse_args()

    print("indexing Claude usage history...")
    usage = UsageTracker()
    otel = OtelReceiver()
    usage.day_cost = otel.day_cost
    usage_api = UsageAPI()
    try:
        media = MediaWatcher()
        print("media watcher active (Windows SMTC)")
    except Exception as e:
        media = None
        print(f"media watcher unavailable: {e!r} (pip install winrt-runtime "
              "winrt-Windows.Media.Control winrt-Windows.Foundation)")
    print(f"usage right now: {usage.snapshot()}")

    while True:
        transport = None
        try:
            transport = open_transport(args)
            if transport is None:
                print("no buddy found on USB or BLE (retrying in 5s)")
                time.sleep(5)
                continue
            run(transport, usage, usage_api, media)
        except KeyboardInterrupt:
            print("\nbye!")
            return
        except Exception as e:
            print(f"connection lost ({e}); reconnecting in 3s")
            time.sleep(3)


if __name__ == "__main__":
    main()
