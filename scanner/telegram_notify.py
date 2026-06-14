"""
Telegram channel notifications for completed scans.

Set TELEGRAM_TOKEN and TELEGRAM_CHAT_ID environment variables to enable.
If either is unset, all calls are silently ignored.

Setup:
  1. Create a bot with @BotFather → copy the token.
  2. Create a channel, add the bot as admin with "Post Messages" permission.
  3. Get the channel chat ID:
       curl "https://api.telegram.org/bot<TOKEN>/getUpdates"
     after forwarding any message from the channel to your bot, or use
     @username_to_id_bot.  Channel IDs look like -100xxxxxxxxxx.
  4. Export env vars (or set them in the systemd unit):
       TELEGRAM_TOKEN=123456:ABC-...
       TELEGRAM_CHAT_ID=-100123456789
"""

import mimetypes
import os
import pathlib
import re
import socket
import subprocess
import sys
import threading
import urllib.error
import urllib.request
import uuid

_TOKEN   = os.environ.get("TELEGRAM_TOKEN",  "")
_CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")


def enabled() -> bool:
    return bool(_TOKEN and _CHAT_ID)


def _multipart(fields: dict, files: list[tuple]) -> tuple[bytes, str]:
    """Build a multipart/form-data body.

    files: list of (field_name, filename, bytes_data)
    """
    boundary = uuid.uuid4().hex.encode()
    parts: list[bytes] = []
    for name, value in fields.items():
        parts += [
            b"--" + boundary + b"\r\n",
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
            str(value).encode() + b"\r\n",
        ]
    for name, filename, data in files:
        ct = mimetypes.guess_type(filename)[0] or "application/octet-stream"
        parts += [
            b"--" + boundary + b"\r\n",
            f'Content-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'.encode(),
            f"Content-Type: {ct}\r\n\r\n".encode(),
            data + b"\r\n",
        ]
    parts.append(b"--" + boundary + b"--\r\n")
    return b"".join(parts), f"multipart/form-data; boundary={boundary.decode()}"


def _api(method: str, fields: dict, files: list[tuple] = ()) -> None:
    url = f"https://api.telegram.org/bot{_TOKEN}/{method}"
    body, ct = _multipart(fields, files)
    req = urllib.request.Request(url, data=body, headers={"Content-Type": ct})
    with urllib.request.urlopen(req, timeout=30) as r:
        r.read()


def _send(pic_path: pathlib.Path, caption: str) -> None:
    data = pic_path.read_bytes()
    name = pic_path.name
    # Inline preview — Telegram recompresses to fit the chat.
    _api("sendPhoto",
         {"chat_id": _CHAT_ID, "caption": caption},
         [("photo", name, data)])
    # Original-quality file attachment.
    _api("sendDocument",
         {"chat_id": _CHAT_ID},
         [("document", name, data)])


def _send_safe(pic_path: pathlib.Path, caption: str) -> None:
    try:
        _send(pic_path, caption)
        sys.stderr.write(f"[telegram] sent {pic_path.name}\n")
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"[telegram] HTTP {e.code}: {e.read().decode(errors='replace')}\n")
    except Exception as e:
        sys.stderr.write(f"[telegram] ERROR: {e}\n")


def notify(pic_path: pathlib.Path, caption: str = "") -> None:
    """Fire-and-forget: send photo + document to the configured channel."""
    if not enabled():
        return
    threading.Thread(
        target=_send_safe, args=(pic_path, caption),
        daemon=True, name="tg-notify",
    ).start()


def _local_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except Exception:
        return "unknown"


def _startup_message() -> str:
    # Try neofetch first; strip ANSI escape codes.
    try:
        out = subprocess.check_output(
            ["neofetch", "--stdout"], stderr=subprocess.DEVNULL, timeout=10,
        ).decode(errors="replace")
        out = re.sub(r"\x1b\[[0-9;]*[mK]", "", out).strip()
        return f"🖨 Scanner service started · <code>{_local_ip()}</code>\n\n<pre>{_escape_html(out)}</pre>"
    except (FileNotFoundError, subprocess.SubprocessError):
        pass
    return f"🖨 Scanner service started\nLocal IP: <code>{_local_ip()}</code>"


def _escape_html(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _send_startup() -> None:
    try:
        _api("sendMessage", {
            "chat_id": _CHAT_ID,
            "text": _startup_message(),
            "parse_mode": "HTML",
        })
        sys.stderr.write("[telegram] startup message sent\n")
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"[telegram] HTTP {e.code}: {e.read().decode(errors='replace')}\n")
    except Exception as e:
        sys.stderr.write(f"[telegram] ERROR: {e}\n")


def notify_startup() -> None:
    """Send a startup banner to the channel (fire-and-forget)."""
    if not enabled():
        return
    threading.Thread(target=_send_startup, daemon=True, name="tg-startup").start()
