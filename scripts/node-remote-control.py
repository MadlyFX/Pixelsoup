#!/usr/bin/env python3
"""Start and stop a render node remotely over HTTP.

Run this on the render machine instead of starting run-output-node.ps1 by
hand. It owns the node script as a child process, so the control machine can
start it, stop it (which releases pixelsoup.exe on a shared build folder so
a new build can link), restart it, and ask whether it is running.

Standard library only. Windows only (it starts PowerShell and uses taskkill).

  python node-remote-control.py --token SECRET -- -DirectorHost 192.168.200.154 -Build release

Everything after "--" is passed to run-output-node.ps1 unchanged. Add
--autostart to start the node as soon as this server is up.

From the control machine (PowerShell):

  $h = @{ 'X-Token' = 'SECRET' }
  Invoke-RestMethod http://192.168.25.61:47850/status  -Headers $h
  Invoke-RestMethod http://192.168.25.61:47850/stop    -Headers $h -Method Post
  Invoke-RestMethod http://192.168.25.61:47850/start   -Headers $h -Method Post
  Invoke-RestMethod http://192.168.25.61:47850/restart -Headers $h -Method Post

or curl:  curl -X POST -H "X-Token: SECRET" http://192.168.25.61:47850/stop

Endpoints (JSON unless noted):
  GET  /status   whether the node script is running, and every pixelsoup.exe
  GET  /log      the node's log as plain text (see below)
  POST /start    start the node script if it is not already running; refuses
                 while another pixelsoup.exe runs unless ?force=1
  POST /stop     stop the node script and its whole process tree; with
                 ?all=1 also end pixelsoup.exe processes it did not start
  POST /restart  stop, then start

/log query parameters:
  file=node      node-<index>.log, the render process (default)
  file=agent     agent-<index>.log, the lifecycle agent
  file=control   this script's own log
  previous=1     the previous run (node-<index>.1.log / agent-<index>.1.log)
  lines=N        last N lines (default 400, at most 20000; 0 = whole file)
  grep=TEXT      only lines containing TEXT (case-insensitive)

  Invoke-RestMethod "http://192.168.25.61:47850/log?lines=200" -Headers $h

Windows Firewall must allow the port (TCP) inbound. From an elevated prompt:

  netsh advfirewall firewall add rule name="Pixelsoup Remote Control" ^
    dir=in action=allow protocol=TCP localport=47850

Anyone who can reach the port and knows the token can start and stop the
node, so use a token and keep the port on the show network.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hmac
import io
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

EXE_NAME = "pixelsoup.exe"
CREATE_NEW_CONSOLE = 0x00000010
CREATE_NO_WINDOW = 0x08000000
MAX_LOG_LINES = 20000
MAX_LOG_BYTES = 16 * 1024 * 1024

_control_log_path: Path | None = None
_control_log_lock = threading.Lock()


def log(message: str) -> None:
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"{stamp} {message}"
    print(line, flush=True)
    if _control_log_path is not None:
        with _control_log_lock:
            try:
                with open(_control_log_path, "a", encoding="utf-8") as stream:
                    stream.write(line + "\n")
            except OSError:
                pass


def script_option(script_args: list[str], name: str) -> str | None:
    """The value after -Name in the node script's arguments, if given."""
    lowered = name.lower()
    for index, arg in enumerate(script_args[:-1]):
        if arg.lower() == lowered:
            return script_args[index + 1]
    return None


def read_log_tail(path: Path, lines: int, grep: str | None) -> str:
    """The last `lines` lines of a log the node may still be writing.

    Reads at most MAX_LOG_BYTES from the end, so a large log never has to be
    loaded whole. Python opens files with read/write sharing on Windows, so
    this never blocks the node's own writes.
    """
    size = path.stat().st_size
    start = max(0, size - MAX_LOG_BYTES)
    with open(path, "rb") as stream:
        stream.seek(start)
        data = stream.read()
    text = data.decode("utf-8", errors="replace").splitlines()
    if start > 0 and text:
        text = text[1:]  # the first line was cut part-way through
    if grep:
        needle = grep.lower()
        text = [line for line in text if needle in line.lower()]
    if lines > 0:
        text = text[-lines:]
    return "\n".join(text) + ("\n" if text else "")


def run_quiet(argv: list[str], timeout: float = 15.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        argv,
        capture_output=True,
        text=True,
        timeout=timeout,
        creationflags=CREATE_NO_WINDOW,
    )


def media_server_processes() -> list[dict]:
    """Every pixelsoup.exe on this machine, as {pid, memory}."""
    try:
        result = run_quiet(
            ["tasklist", "/FI", f"IMAGENAME eq {EXE_NAME}", "/FO", "CSV", "/NH"]
        )
    except (OSError, subprocess.SubprocessError):
        return []
    rows = []
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) >= 5 and row[0].lower() == EXE_NAME:
            try:
                rows.append({"pid": int(row[1]), "memory": row[4]})
            except ValueError:
                continue
    return rows


def memory_kib(text: str) -> int:
    """tasklist's memory column ("20,232 K") in KiB; 0 when unreadable."""
    digits = "".join(ch for ch in text if ch.isdigit())
    return int(digits) if digits else 0


def kill_tree(pid: int) -> str:
    try:
        result = run_quiet(["taskkill", "/PID", str(pid), "/T", "/F"])
    except (OSError, subprocess.SubprocessError) as error:
        return f"taskkill failed: {error}"
    return (result.stdout + result.stderr).strip()


class NodeController:
    def __init__(self, script: Path, script_args: list[str], new_console: bool) -> None:
        self._script = script
        self._script_args = script_args
        self._new_console = new_console
        self._process: subprocess.Popen | None = None
        self._started_at: float | None = None
        self._lock = threading.Lock()

    def command(self) -> list[str]:
        return [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(self._script),
            *self._script_args,
        ]

    def _running_locked(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def status(self) -> dict:
        with self._lock:
            running = self._running_locked()
            exit_code = None
            if self._process is not None and not running:
                exit_code = self._process.returncode
            return {
                "running": running,
                "pid": self._process.pid if running else None,
                "uptime_s": round(time.time() - self._started_at, 1)
                if running and self._started_at
                else None,
                "last_exit_code": exit_code,
                "command": self.command(),
                "media_server_processes": media_server_processes(),
            }

    def start(self, force: bool = False) -> dict:
        with self._lock:
            if self._running_locked():
                return {"ok": True, "message": "already running", "pid": self._process.pid}
            if not self._script.is_file():
                return {"ok": False, "message": f"node script not found: {self._script}"}
            # A process holding under 1 MB is a husk Windows has not reaped
            # (seen at 32 K), not a node: the agent alone holds ~20 MB.
            strays = [] if force else [
                p for p in media_server_processes() if memory_kib(p["memory"]) >= 1024
            ]
            if strays:
                # A node started by hand, or by an earlier run of this script,
                # still holds the display and the director slot.
                return {
                    "ok": False,
                    "message": "pixelsoup.exe is already running but was not started "
                    "by this script; POST /stop?all=1 first, or /start?force=1",
                    "media_server_processes": strays,
                }
            flags = CREATE_NEW_CONSOLE if self._new_console else CREATE_NO_WINDOW
            try:
                self._process = subprocess.Popen(
                    self.command(),
                    cwd=str(self._script.parent.parent),
                    creationflags=flags,
                )
            except OSError as error:
                self._process = None
                return {"ok": False, "message": f"could not start: {error}"}
            self._started_at = time.time()
            log(f"started node script, pid {self._process.pid}")
            return {"ok": True, "message": "started", "pid": self._process.pid}

    def stop(self, include_strays: bool) -> dict:
        with self._lock:
            details = []
            if self._running_locked():
                pid = self._process.pid
                details.append(kill_tree(pid))
                try:
                    self._process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    details.append(f"pid {pid} did not exit within 10 s")
                log(f"stopped node script, pid {pid}")
            else:
                details.append("node script was not running")
            if include_strays:
                for process in media_server_processes():
                    details.append(kill_tree(process["pid"]))
            # The share keeps the exe locked until the last handle closes;
            # wait briefly so a build started right after this call can link.
            def live() -> list[dict]:
                return [p for p in media_server_processes() if memory_kib(p["memory"]) >= 1024]

            deadline = time.time() + 10
            remaining = live()
            while remaining and time.time() < deadline:
                time.sleep(0.25)
                remaining = live()
            return {
                "ok": True,
                "message": "stopped",
                "details": [d for d in details if d],
                "media_server_processes": remaining,
            }

    def restart(self) -> dict:
        stopped = self.stop(include_strays=False)
        started = self.start(force=True)
        return {"ok": started.get("ok", False), "stop": stopped, "start": started}


class LogFiles:
    def __init__(self, log_dir: Path, node_index: str) -> None:
        self.log_dir = log_dir
        self.node_index = node_index

    def path(self, which: str, previous: bool) -> Path | None:
        if which == "control":
            return _control_log_path
        if which not in ("node", "agent"):
            return None
        suffix = ".1.log" if previous else ".log"
        return self.log_dir / f"{which}-{self.node_index}{suffix}"


def make_handler(controller: NodeController, logs: LogFiles, token: str | None):
    class Handler(BaseHTTPRequestHandler):
        server_version = "PixelsoupNodeControl/1"

        def log_message(self, fmt: str, *args) -> None:  # noqa: N802 (stdlib name)
            log(f"{self.client_address[0]} {fmt % args}")

        def _authorised(self, query: dict) -> bool:
            if token is None:
                return True
            supplied = self.headers.get("X-Token") or query.get("token", [""])[0]
            return hmac.compare_digest(supplied.encode(), token.encode())

        def _reply(self, code: int, body: dict) -> None:
            data = json.dumps(body, indent=2).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _reply_text(self, code: int, text: str) -> None:
            data = text.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _log(self, query: dict) -> None:
            which = query.get("file", ["node"])[0].lower()
            previous = query.get("previous", ["0"])[0] in ("1", "true", "yes")
            try:
                lines = int(query.get("lines", ["400"])[0])
            except ValueError:
                self._reply(400, {"ok": False, "message": "lines must be a number"})
                return
            lines = max(0, min(lines, MAX_LOG_LINES))
            grep = query.get("grep", [None])[0]
            path = logs.path(which, previous)
            if path is None:
                self._reply(400, {"ok": False, "message": "file must be node, agent or control"})
                return
            if not path.is_file():
                self._reply(404, {"ok": False, "message": f"no log at {path}"})
                return
            try:
                self._reply_text(200, read_log_tail(path, lines, grep))
            except OSError as error:
                self._reply(500, {"ok": False, "message": f"could not read {path}: {error}"})

        def _route(self, method: str) -> None:
            url = urlparse(self.path)
            query = parse_qs(url.query)
            if not self._authorised(query):
                self._reply(401, {"ok": False, "message": "missing or wrong token"})
                return
            path = url.path.rstrip("/") or "/"
            if path in ("/", "/status") and method in ("GET", "POST"):
                self._reply(200, controller.status())
            elif path == "/log" and method == "GET":
                self._log(query)
            elif method != "POST":
                self._reply(405, {"ok": False, "message": "use POST for this action"})
            elif path == "/start":
                force = query.get("force", ["0"])[0] in ("1", "true", "yes")
                result = controller.start(force=force)
                self._reply(200 if result["ok"] else 500, result)
            elif path == "/stop":
                include = query.get("all", ["0"])[0] in ("1", "true", "yes")
                self._reply(200, controller.stop(include_strays=include))
            elif path == "/restart":
                result = controller.restart()
                self._reply(200 if result["ok"] else 500, result)
            else:
                self._reply(404, {"ok": False, "message": f"unknown path {url.path}"})

        def do_GET(self) -> None:  # noqa: N802 (stdlib name)
            self._route("GET")

        def do_POST(self) -> None:  # noqa: N802 (stdlib name)
            self._route("POST")

    return Handler


def main() -> int:
    if os.name != "nt":
        print("This script controls a Windows render node and only runs on Windows.")
        return 2
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Start and stop a Pixelsoup render node remotely.",
        epilog='Arguments after "--" go to run-output-node.ps1, '
        'for example: -- -DirectorHost 192.168.200.154 -Build release',
    )
    parser.add_argument("--listen", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=47850, help="TCP port (default 47850)")
    parser.add_argument(
        "--token",
        default=os.environ.get("NODE_CONTROL_TOKEN"),
        help="shared secret required in the X-Token header (or set NODE_CONTROL_TOKEN)",
    )
    parser.add_argument(
        "--no-token",
        action="store_true",
        help="accept requests without a token (anyone on the network can control the node)",
    )
    parser.add_argument(
        "--script",
        type=Path,
        default=here / "run-output-node.ps1",
        help="node script to run (default: run-output-node.ps1 beside this file)",
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=None,
        help="folder holding node-<index>.log (default: the node script's -LogDir, "
        r"else %%LOCALAPPDATA%%\Pixelsoup\logs)",
    )
    parser.add_argument("--autostart", action="store_true", help="start the node immediately")
    parser.add_argument(
        "--hidden",
        action="store_true",
        help="run the node script without its own console window",
    )
    argv = sys.argv[1:]
    script_args: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, script_args = argv[:split], argv[split + 1 :]
    args = parser.parse_args(argv)

    if not args.token and not args.no_token:
        parser.error("set --token (or NODE_CONTROL_TOKEN), or pass --no-token deliberately")
    if not any(a.lower() == "-directorhost" for a in script_args):
        parser.error('pass the node script\'s arguments after "--", at least -DirectorHost <address>')

    log_dir = args.log_dir
    if log_dir is None:
        script_log_dir = script_option(script_args, "-LogDir")
        log_dir = Path(script_log_dir) if script_log_dir else (
            Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "Pixelsoup" / "logs"
        )
    node_index = script_option(script_args, "-NodeIndex") or "0"
    global _control_log_path
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        _control_log_path = log_dir / f"remote-control-{node_index}.log"
    except OSError:
        _control_log_path = None
    logs = LogFiles(log_dir, node_index)

    controller = NodeController(args.script.resolve(), script_args, new_console=not args.hidden)
    server = ThreadingHTTPServer(
        (args.listen, args.port),
        make_handler(controller, logs, None if args.no_token else args.token),
    )
    log(f"node remote control on {args.listen}:{args.port}")
    log(f"node logs read from {log_dir} (node index {node_index})")
    log(f"node command: {' '.join(controller.command())}")
    if args.autostart:
        log(json.dumps(controller.start()))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("shutting down; the node keeps running unless stopped first")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
