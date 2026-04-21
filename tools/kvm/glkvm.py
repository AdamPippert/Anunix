#!/usr/bin/env python3
"""
glkvm.py — CLI for operating the GL.iNet KVM (glkvm on Tailscale).

Usage:
    glkvm.py auth                          # get + print token
    glkvm.py screenshot [output.jpg]       # capture screen (starts streamer if needed)
    glkvm.py type "text to type"           # type text via HID
    glkvm.py key ShiftLeft                 # tap a key (press + release)
    glkvm.py shell "command"               # run a shell command on PiKVM itself
    glkvm.py shell-bg "command"            # run a background shell command on PiKVM
    glkvm.py hid                           # show HID / keyboard LED state
    glkvm.py streamer                      # show streamer status
    glkvm.py start-streamer                # start ustreamer via PiKVM shell
    glkvm.py atx                           # show ATX / power state

Connection details:
    Host:     glkvm (100.123.44.77 on Tailscale)
    Auth:     POST /api/auth/login  user=admin&passwd=<GLKVM_PASS>
    Token:    Header "Token: <token>"  (NOT Bearer)
    Webterm:  wss://glkvm/extras/webterm/ttyd/ws  subprotocol=tty
              init: send text '{"columns":200,"rows":50}'
              input: binary frame with prefix byte 0x30 (ASCII '0')
              output: binary frame with prefix byte 0x30 (ASCII '0')

Environment variables:
    GLKVM_HOST   hostname or IP (default: glkvm)
    GLKVM_PASS   admin password (default: reads ~/.glkvm_pass or prompts)
    GLKVM_TOKEN  pre-obtained token (skips auth if set)
"""

import sys
import os
import json
import ssl
import time
import re
import threading
import urllib.request
import urllib.parse

HOST = os.environ.get("GLKVM_HOST", "glkvm")
BASE = f"https://{HOST}"

# SSL context that skips cert verification (self-signed cert on PiKVM)
_ctx = ssl.create_default_context()
_ctx.check_hostname = False
_ctx.verify_mode = ssl.CERT_NONE


def _load_pass():
    p = os.environ.get("GLKVM_PASS")
    if p:
        return p
    path = os.path.expanduser("~/.glkvm_pass")
    if os.path.exists(path):
        return open(path).read().strip()
    import getpass
    return getpass.getpass(f"glkvm password for admin@{HOST}: ")


def _request(method, path, *, token=None, body=None, form=None, raw=False):
    url = BASE + path
    headers = {}
    if token:
        headers["Token"] = token
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    elif form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        resp = urllib.request.urlopen(req, context=_ctx, timeout=15)
        return resp.read() if raw else json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body_bytes = e.read()
        try:
            return json.loads(body_bytes)
        except Exception:
            raise RuntimeError(f"HTTP {e.code}: {body_bytes[:200]}")


def cmd_auth():
    """Authenticate and print the session token."""
    pw = _load_pass()
    result = _request("POST", "/api/auth/login", form={"user": "admin", "passwd": pw})
    if isinstance(result, dict) and not result.get("ok", True):
        sys.exit(f"Auth failed: {result}")
    # Token is returned as plain text or in a cookie — check response body
    # On GL.iNet PiKVM, login returns the token as a JSON body or sets a cookie.
    # Try JSON first, then cookie header.
    if isinstance(result, dict) and "token" in result.get("result", {}):
        token = result["result"]["token"]
    else:
        # Response body IS the token (plain text)
        token = result if isinstance(result, str) else str(result)
    print(token)
    return token


def _get_token():
    t = os.environ.get("GLKVM_TOKEN")
    if t:
        return t
    # Try login
    return cmd_auth()


def _shell(token, command, wait_for="CMD_END", timeout=30, background=False):
    """Run a shell command on the PiKVM via webterm WebSocket."""
    try:
        import websocket
    except ImportError:
        sys.exit("pip3 install websocket-client required for shell commands")

    output = []
    ws_ref = [None]
    connected = threading.Event()

    def on_msg(ws, msg):
        if isinstance(msg, bytes) and len(msg) > 1:
            output.append(msg[1:].decode("utf-8", errors="replace"))

    def on_open(ws):
        ws_ref[0] = ws
        ws.send('{"columns":200,"rows":50}')
        connected.set()

    ws_obj = websocket.WebSocketApp(
        f"wss://{HOST}/extras/webterm/ttyd/ws",
        on_open=on_open,
        on_message=on_msg,
        on_close=lambda ws, c, m: output.append("[CLOSED]"),
        on_error=lambda ws, e: connected.set(),
        header={"Token": token, "Origin": f"https://{HOST}"},
        subprotocols=["tty"],
    )
    t = threading.Thread(
        target=ws_obj.run_forever,
        kwargs={"sslopt": {"cert_reqs": ssl.CERT_NONE}, "ping_interval": 5},
    )
    t.daemon = True
    t.start()

    connected.wait(timeout=10)
    wss = ws_ref[0]
    if not wss:
        sys.exit("Failed to connect to webterm")

    # Wait for bash prompt
    deadline = time.time() + 10
    while time.time() < deadline:
        if "bash-5.2#" in "".join(output) or "# " in "".join(output)[-20:]:
            break
        time.sleep(0.2)

    # Send command (input type byte: ASCII '0' = 0x30)
    marker = "CMD_END_" + str(int(time.time()))
    if background:
        full_cmd = f"( {command} ) & echo {marker}\n"
    else:
        full_cmd = f"( {command} ); echo {marker}\n"
    wss.send(b"\x30" + full_cmd.encode())

    # Wait for marker
    deadline = time.time() + timeout
    while time.time() < deadline:
        if marker in "".join(output):
            break
        time.sleep(0.3)

    ws_obj.close()

    clean = re.sub(
        r"\x1b\[[0-9;]*[a-zA-Z]|\x1b\][^\x07]*\x07|\r", "", "".join(output)
    )
    # Strip everything before the command echo
    idx = clean.find(command[:30])
    if idx >= 0:
        clean = clean[idx + len(command[:30]):]
    # Strip marker
    clean = clean.replace(marker, "").strip()
    return clean


USTREAMER_CMD = (
    "pkill -f 'ustreamer.*ustreamer.sock' 2>/dev/null; "
    "rm -f /run/kvmd/ustreamer.sock; "
    "setsid nohup /usr/bin/ustreamer "
    "--device=/dev/video0 "
    "-r 1920x1080 "
    "--venc-format=0 "
    "--tcp-nodelay "
    "--vi-buffer-num=4 "
    "--vi-format=4 "
    "--unix=/run/kvmd/ustreamer.sock "
    "--unix-rm "
    "--unix-mode=0660 "
    "--jpeg-sink=kvmd::ustreamer::jpeg "
    "--jpeg-sink-mode=0660 "
    "--h264-sink=kvmd::ustreamer::h264 "
    "--h264-sink-mode=0660 "
    "--h264-bitrate=2000 "
    "--h264-gop=30 "
    "--zero-delay=0 "
    "< /dev/null > /tmp/ustreamer.log 2>&1 & "
    "sleep 4; "
    "ls /run/kvmd/ustreamer.sock 2>/dev/null && echo ustreamer-ok || echo ustreamer-failed"
)


def cmd_start_streamer(token):
    """Start ustreamer on the PiKVM if not already running."""
    print("Starting ustreamer...", file=sys.stderr)
    out = _shell(token, USTREAMER_CMD, timeout=20)
    print(out, file=sys.stderr)


def cmd_screenshot(token, output_path="screenshot.jpg"):
    """
    Capture a screenshot via ustreamer's unix socket.
    Writes JPEG to PiKVM, serves it via a temp HTTP server, fetches via Tailscale subnet.
    PiKVM local IP: 192.168.0.18 — reachable via superrouter Tailscale subnet.
    """
    PIKVM_LOCAL_IP = "100.123.44.77"  # glkvm Tailscale IP (local 192.168.0.18 unreachable)
    HTTP_PORT = 9977

    START_CMD = (
        "if [ ! -S /run/kvmd/ustreamer.sock ]; then "
        + USTREAMER_CMD + "; fi"
    )
    SERVE_CMD = (
        "curl -s --unix-socket /run/kvmd/ustreamer.sock "
        "http://localhost/snapshot > /tmp/glkvm_snap.jpg && "
        f"python3 -m http.server {HTTP_PORT} --directory /tmp "
        "> /tmp/httpd.log 2>&1 & echo $! > /tmp/httpd.pid && echo httpd-ok"
    )
    KILL_CMD = "kill $(cat /tmp/httpd.pid 2>/dev/null) 2>/dev/null; rm -f /tmp/httpd.pid"

    print("Capturing screenshot...", file=sys.stderr)
    _shell(token, START_CMD, timeout=20)

    out = _shell(token, SERVE_CMD, timeout=15)
    if "httpd-ok" not in out:
        sys.exit(f"Failed to start HTTP server: {out[:200]}")

    try:
        url = f"http://{PIKVM_LOCAL_IP}:{HTTP_PORT}/glkvm_snap.jpg"
        req = urllib.request.Request(url, method="GET")
        resp = urllib.request.urlopen(req, timeout=10)
        data = resp.read()
        if data[:2] != b"\xff\xd8":
            sys.exit(f"Not a JPEG: {len(data)} bytes, magic={data[:4].hex()}")
        with open(output_path, "wb") as f:
            f.write(data)
        print(f"Saved {len(data)} bytes to {output_path}")
        return output_path
    except Exception as e:
        sys.exit(f"Fetch failed: {e}")
    finally:
        _shell(token, KILL_CMD, timeout=8)


def cmd_type(token, text):
    """Type text via HID keyboard."""
    result = _request("POST", "/api/hid/print",
                      token=token, body={"text": text, "limit": 0})
    if not result.get("ok"):
        sys.exit(f"HID type failed: {result}")
    print("ok")


def _send_key_event(token, key_name, state):
    """Send a single key up or down event. Returns True on success."""
    try:
        result = _request("POST", "/api/hid/events/send-key",
                          token=token, body={"key": key_name, "state": state})
        if isinstance(result, dict) and result.get("ok", True):
            return True
    except Exception:
        pass
    try:
        _request("POST", f"/api/hid/keysym?key={key_name}&state={'1' if state else '0'}",
                 token=token)
        return True
    except Exception:
        return False


def cmd_key(token, key_name):
    """Tap a key (press then release)."""
    _send_key_event(token, key_name, True)
    time.sleep(0.05)
    _send_key_event(token, key_name, False)
    print(f"tapped {key_name}")


def cmd_chord(token, keys):
    """Press multiple keys simultaneously (e.g. Super+Return). keys is a list."""
    for k in keys:
        _send_key_event(token, k, True)
        time.sleep(0.03)
    time.sleep(0.05)
    for k in reversed(keys):
        _send_key_event(token, k, False)
        time.sleep(0.03)
    print(f"chorded {'+'.join(keys)}")


def cmd_hid(token):
    result = _request("GET", "/api/hid", token=token)
    print(json.dumps(result.get("result", result), indent=2))


def cmd_streamer(token):
    result = _request("GET", "/api/streamer", token=token)
    print(json.dumps(result.get("result", result), indent=2))


def cmd_atx(token):
    result = _request("GET", "/api/atx", token=token)
    print(json.dumps(result.get("result", result), indent=2))


def cmd_shell(token, command, background=False, timeout=30):
    out = _shell(token, command, background=background, timeout=timeout)
    print(out)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)

    subcmd = args[0]

    if subcmd == "auth":
        cmd_auth()
        return

    token = _get_token()

    if subcmd == "screenshot":
        path = args[1] if len(args) > 1 else "screenshot.jpg"
        cmd_screenshot(token, path)

    elif subcmd == "type":
        if len(args) < 2:
            sys.exit("usage: glkvm.py type <text>")
        cmd_type(token, args[1])

    elif subcmd == "key":
        if len(args) < 2:
            sys.exit("usage: glkvm.py key <KeyName>")
        cmd_key(token, args[1])

    elif subcmd == "chord":
        if len(args) < 2:
            sys.exit("usage: glkvm.py chord Key1 Key2 ...")
        cmd_chord(token, args[1:])

    elif subcmd == "shell":
        if len(args) < 2:
            sys.exit("usage: glkvm.py shell <command>")
        timeout = int(args[2]) if len(args) > 2 else 30
        cmd_shell(token, args[1], timeout=timeout)

    elif subcmd == "shell-bg":
        if len(args) < 2:
            sys.exit("usage: glkvm.py shell-bg <command>")
        cmd_shell(token, args[1], background=True)

    elif subcmd == "start-streamer":
        cmd_start_streamer(token)

    elif subcmd == "hid":
        cmd_hid(token)

    elif subcmd == "streamer":
        cmd_streamer(token)

    elif subcmd == "atx":
        cmd_atx(token)

    else:
        sys.exit(f"Unknown subcommand: {subcmd}\n{__doc__}")


if __name__ == "__main__":
    main()
