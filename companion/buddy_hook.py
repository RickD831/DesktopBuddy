"""Claude Code hook -> buddy event bridge.

Registered in ~/.claude/settings.json for PreToolUse, UserPromptSubmit,
Notification and Stop. Reads the hook payload from stdin and appends one
compact event line to ~/.claude/buddy_events.jsonl, which the companion
watches. Never blocks Claude: exits 0 with no output on any failure.
"""

import json
import os
import sys
import time
from pathlib import Path

EVENTS = Path.home() / ".claude" / "buddy_events.jsonl"
MAX_SIZE = 512 * 1024


def main() -> None:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return

    ev = data.get("hook_event_name", "")
    cwd = data.get("cwd") or ""
    proj = os.path.basename(cwd) if cwd else ""

    if ev == "PreToolUse":
        tool = data.get("tool_name", "tool")
        ti = data.get("tool_input") or {}
        desc = ""
        if isinstance(ti, dict):
            desc = ti.get("description") or ti.get("file_path") or ti.get("pattern") or ""
        if isinstance(desc, str) and "\\" in desc:
            desc = desc.rsplit("\\", 1)[-1]
        state, msg = "working", (f"{tool}: {desc}"[:80] if desc else f"using {tool}")
    elif ev == "UserPromptSubmit":
        state, msg = "working", "reading your message"
    elif ev == "Notification":
        state = "waiting"
        msg = str(data.get("message") or "needs your attention")[:80]
    elif ev == "Stop":
        state, msg = "done", "finished"
    else:
        return

    line = json.dumps({"ts": time.time(), "state": state, "msg": msg, "proj": proj})
    try:
        if EVENTS.exists() and EVENTS.stat().st_size > MAX_SIZE:
            EVENTS.write_text(line + "\n", encoding="utf-8")
        else:
            with open(EVENTS, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
    except OSError:
        pass


if __name__ == "__main__":
    main()
