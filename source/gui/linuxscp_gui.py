#!/usr/bin/env python3
"""
LinuxSCP Graphical Commander Interface Server
Provides a web-based dual-pane Norton/WinSCP Commander interface
backed by the LinuxSCP protocol engine.

Architecture
------------
* One *persistent core process* per tab (``linuxscp -server``). It holds
  the SSH connection, so browsing, renaming and editing do not log in
  again for every operation.
* Transfers, synchronizations and searches run as *tasks* in their own
  short-lived core processes, so they can be cancelled, retried and
  reported on without blocking the session they belong to.
* Stored sessions are read from (and written through) the same file the
  console client uses, ``sessions.ini`` in the configuration directory.
"""

import os
import sys
import json
import time
import shutil
import stat
import getpass
import queue
import posixpath
import subprocess
import tempfile
import threading
import urllib.parse
from fnmatch import fnmatch
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

PORT = 8080
HOST = "0.0.0.0"
IS_WINDOWS = os.name == "nt"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(BASE_DIR, "web")
if not os.path.isdir(WEB_DIR):
    fallbacks = ["/usr/share/linuxscp/gui"]
    if IS_WINDOWS:
        for root in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
            if root:
                fallbacks.append(os.path.join(root, "LinuxSCP", "web"))
                fallbacks.append(os.path.join(root, "LinuxSCP", "bin", "web"))
    for candidate in fallbacks:
        if os.path.isdir(candidate):
            WEB_DIR = candidate
            break


def _default_user():
    try:
        return getpass.getuser() or "user"
    except Exception:
        return "user"


def find_linuxscp_binary():
    names = ["linuxscp" + EXE_SUFFIX, "linuxscp"]
    candidates = []
    # Next to the GUI script / installed next to linuxscp.exe.
    for name in names:
        candidates.append(os.path.join(BASE_DIR, name))
        candidates.append(os.path.join(BASE_DIR, "..", "linux", "build", name))
        candidates.append(os.path.join(BASE_DIR, "..", "..", "source", "linux", "build", name))
        candidates.append(os.path.join(BASE_DIR, "..", "..", "build-windows", name))
        candidates.append(os.path.join(BASE_DIR, "..", "..", "build", name))
    if IS_WINDOWS:
        for root in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"),
                     os.environ.get("LOCALAPPDATA")):
            if root:
                candidates.append(os.path.join(root, "LinuxSCP", "linuxscp.exe"))
                candidates.append(os.path.join(root, "LinuxSCP", "bin", "linuxscp.exe"))
    else:
        candidates.append("/usr/bin/linuxscp")
        candidates.append("/usr/local/bin/linuxscp")
    for name in names:
        found = shutil.which(name)
        if found:
            candidates.append(found)
    for c in candidates:
        if c and os.path.isfile(c) and (IS_WINDOWS or os.access(c, os.X_OK)):
            return os.path.abspath(c)
    return "linuxscp" + EXE_SUFFIX


LINUXSCP_BIN = find_linuxscp_binary()

STATE_LOCK = threading.RLock()
STATE = {
    "logs": [],
    "tabs": [],          # list of CoreSession
    "active_tab": None,
    "tasks": [],         # newest first
    "paused": False,
    "next_tab": 1,
    "next_task": 1,
}


def add_log(message, level="info"):
    timestamp = time.strftime("%H:%M:%S")
    with STATE_LOCK:
        entry = {"time": timestamp, "text": message, "level": level}
        STATE["logs"].append(entry)
        if len(STATE["logs"]) > 400:
            del STATE["logs"][:-200]


# ---------------------------------------------------------------------------
# Stored sessions (the site manager)
# ---------------------------------------------------------------------------

def session_store_path():
    """The sessions.ini the console client and this server share."""
    override = os.environ.get("LINUXSCP_CONFIG_DIR")
    if override:
        return os.path.join(override, "sessions.ini")
    if IS_WINDOWS:
        root = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(root, "LinuxSCP", "sessions.ini")
    root = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(root, "LinuxSCP", "sessions.ini")


def _unescape(text):
    out = []
    i = 0
    while i < len(text):
        char = text[i]
        if char == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({"n": "\n", "r": "\r", "t": "\t"}.get(nxt, nxt))
            i += 2
            continue
        out.append(char)
        i += 1
    return "".join(out)


def _escape(text):
    return (text.replace("\\", "\\\\").replace("\n", "\\n")
                .replace("\r", "\\r").replace("\t", "\\t"))


SESSION_KEYS = {
    "protocol": "protocol",
    "host": "host",
    "hostname": "host",
    "port": "port",
    "username": "username",
    "user": "username",
    "private_key": "keyfile",
    "privatekey": "keyfile",
    "keyfile": "keyfile",
    "hostkey": "hostkey",
    "host_key": "hostkey",
    "remote_directory": "initial_dir",
    "remotedir": "initial_dir",
    "local_directory": "local_dir",
    "localdir": "local_dir",
    "password": "password",
    "last_used": "last_used",
}


def read_stored_sessions():
    """Parse sessions.ini into the dictionaries the web client uses."""
    path = session_store_path()
    sessions = []
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
    except OSError:
        return sessions
    current = None
    for line in raw.splitlines():
        text = line.strip()
        if not text or text[0] in "#;":
            continue
        if text.startswith("[") and text.endswith("]"):
            if current:
                sessions.append(current)
            current = {
                "name": _unescape(text[1:-1]),
                "protocol": "sftp", "host": "", "port": 22, "username": "",
                "password": "", "keyfile": "", "hostkey": "",
                "initial_dir": "/", "local_dir": "", "last_used": 0,
            }
            continue
        if current is None or "=" not in text:
            continue
        key, value = text.split("=", 1)
        field = SESSION_KEYS.get(key.strip().lower())
        if field is None:
            continue
        value = _unescape(value.strip())
        if field == "port":
            try:
                current["port"] = int(value)
            except ValueError:
                current["port"] = 22
        elif field == "last_used":
            try:
                current["last_used"] = int(value)
            except ValueError:
                pass
        else:
            current[field] = value
    if current:
        sessions.append(current)
    sessions.sort(key=lambda item: item["name"].lower())
    return sessions


def new_session():
    return {
        "protocol": "sftp",
        "host": "localhost",
        "port": 22,
        "username": os.environ.get("USERNAME") or os.environ.get("USER") or _default_user(),
        "password": "",
        "keyfile": "",
        "hostkey": "*",
        "initial_dir": "/",
        "local_dir": os.path.expanduser("~"),
    }


def session_url(session, path=None):
    proto = session.get("protocol") or "sftp"
    host = session.get("host") or "localhost"
    port = int(session.get("port") or 22)
    user = session.get("username") or ""
    password = session.get("password") or ""
    url = proto + "://"
    if user:
        url += urllib.parse.quote(user, safe="")
        if password:
            url += ":" + urllib.parse.quote(password, safe="")
        url += "@"
    url += host
    if port and port != 22:
        url += ":%d" % port
    url += "/"
    if path:
        url += path.lstrip("/")
    return url


def core_command_line(session):
    """The arguments every core process of this session needs."""
    cmd = [find_linuxscp_binary(), "-batch=on"]
    hostkey = session.get("hostkey") or "*"
    cmd.append("-hostkey=" + hostkey)
    if session.get("keyfile"):
        cmd.append("-privatekey=" + session["keyfile"])
    if session.get("username"):
        cmd.append("-username=" + session["username"])
    if session.get("password"):
        cmd.append("-password=" + session["password"])
    return cmd


# ---------------------------------------------------------------------------
# Running the console client
# ---------------------------------------------------------------------------

def popen_kwargs():
    kwargs = {
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "stdin": subprocess.PIPE,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
    }
    if IS_WINDOWS:
        # No console window flashing up for every operation.
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return kwargs


def run_linuxscp(command_script, session=None, timeout=300):
    """Run one script in a short-lived core process. Returns (code, out, err)."""
    session = session or new_session()
    bin_path = find_linuxscp_binary()
    cmd = core_command_line(session)
    cmd.append("-command=" + "option confirm off ; " + command_script)
    add_log("Executing: " + command_script, "cmd")
    try:
        proc = subprocess.Popen(cmd, **popen_kwargs())
    except OSError as exc:
        message = ("Cannot start the LinuxSCP core binary '%s': %s. "
                   "Build it first (windows\\build.cmd or make -C source/linux) "
                   "or put linuxscp%s on PATH." % (bin_path, exc, EXE_SUFFIX))
        add_log(message, "error")
        return 1, "", message
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        add_log("LinuxSCP core timed out after %d seconds." % timeout, "error")
        return 1, stdout or "", "Timed out."
    if stderr.strip():
        add_log(stderr.strip(), "error")
    return proc.returncode, stdout or "", stderr or ""


# ---------------------------------------------------------------------------
# A persistent session: one core process, one connection
# ---------------------------------------------------------------------------

class CoreSession(object):
    """A tab: a core process in server mode, with its connection open."""

    def __init__(self, tab_id, session, name=None):
        self.id = tab_id
        self.session = dict(session)
        self.name = name or self._default_name()
        self.remote_dir = session.get("initial_dir") or "/"
        self.local_dir = session.get("local_dir") or os.path.expanduser("~")
        self.connected = False
        self.observed_hostkey = ""
        self.error = ""
        self.proc = None
        self.lock = threading.RLock()
        self._queue = None
        self._stderr = []

    def _default_name(self):
        host = self.session.get("host") or "localhost"
        user = self.session.get("username") or ""
        return "%s@%s" % (user, host) if user else host

    # -- process management ------------------------------------------------

    def start(self):
        with self.lock:
            if self.proc is not None and self.proc.poll() is None:
                return True, ""
            cmd = [find_linuxscp_binary(), "-server", "-batch=on",
                   "-hostkey=" + (self.session.get("hostkey") or "*")]
            if self.session.get("keyfile"):
                cmd.append("-privatekey=" + self.session["keyfile"])
            try:
                self.proc = subprocess.Popen(cmd, **popen_kwargs())
            except OSError as exc:
                self.error = str(exc)
                return False, self.error
            self._queue = queue.Queue()
            threading.Thread(target=self._read_stdout, daemon=True).start()
            threading.Thread(target=self._read_stderr, daemon=True).start()
            try:
                line = self._queue.get(timeout=15)
            except queue.Empty:
                self.error = "The core process did not start."
                return False, self.error
            if line is None or "LSCP-SERVER" not in line:
                self.error = "Unexpected greeting from the core process."
                return False, self.error
            return True, ""

    def connect(self):
        ok, error = self.start()
        if not ok:
            self.connected = False
            self.error = error
            return False, error
        path = self.session.get("initial_dir") or "/"
        code, output, errors = self.run_capture("open " + session_url(self.session, path))
        if code != 0:
            detail = output + errors
            self.error = "\n".join(detail).strip() or "Cannot connect."
            self.connected = False
            return False, self.error
        self.run("option confirm off")
        # Remember what a wildcard host key turned out to be, so that the
        # connection can be pinned instead of accepted again next time.
        for line in output:
            if line.startswith("HOSTKEY "):
                self.observed_hostkey = line.split(" ", 2)[-1].strip()
        code, output = self.run("pwd")
        if code == 0:
            for line in output:
                if line.startswith("/"):
                    self.remote_dir = line.strip()
                    break
        self.connected = True
        self.error = ""
        return True, ""

    def run_many(self, commands, timeout=120):
        """Run several commands, stopping at the first failure.

        The server takes one command per line; unlike '-command=' it does
        not split a line on ';'.
        """
        code, lines, errors = 0, [], []
        for command in commands:
            code, output, errs = self.run_capture(command, timeout=timeout)
            lines.extend(output)
            errors.extend(errs)
            if code != 0:
                break
        return code, lines

    def run_many_capture(self, commands, timeout=120):
        """run_many that also reports what the core wrote to stderr."""
        code, lines, errors = 0, [], []
        for command in commands:
            code, output, errs = self.run_capture(command, timeout=timeout)
            lines.extend(output)
            errors.extend(errs)
            if code != 0:
                break
        return code, lines, errors

    def _read_stdout(self):
        try:
            for line in self.proc.stdout:
                self._queue.put(line.rstrip("\r\n"))
        except (ValueError, OSError):
            pass
        self._queue.put(None)

    def _read_stderr(self):
        try:
            for line in self.proc.stderr:
                text = line.strip()
                if text:
                    add_log("[%s] %s" % (self.name, text), "error")
                    try:
                        self._stderr.append(text)
                    except Exception:
                        pass
        except (ValueError, OSError):
            pass

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def run(self, command, timeout=120):
        """Run one command in the session. Returns (code, output lines)."""
        code, lines, _ = self.run_capture(command, timeout)
        return code, lines

    def run_capture(self, command, timeout=120):
        """Run one command, also reporting what the core wrote to stderr."""
        with self.lock:
            if not self.alive():
                if not self.connected:
                    return 1, ["Not connected."], []
                started, error = self.start()
                if not started:
                    self.connected = False
                    return 1, [error or "The session process is gone."], []
                self.connected = True
            del self._stderr[:]
            try:
                self.proc.stdin.write(command + "\n")
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                self.connected = False
                return 1, ["The session process is gone."], []
            lines = []
            deadline = time.time() + timeout
            while True:
                remaining = deadline - time.time()
                if remaining <= 0:
                    self.terminate()
                    self.connected = False
                    return 1, lines + ["The command timed out after %d seconds." % timeout], list(self._stderr)
                try:
                    line = self._queue.get(timeout=min(remaining, 0.5))
                except queue.Empty:
                    continue
                if line is None:
                    self.connected = False
                    return 1, lines + ["The session process ended unexpectedly."], list(self._stderr)
                if line.startswith("LSCP-RESULT"):
                    try:
                        code = int(line.split()[1])
                    except (IndexError, ValueError):
                        code = 1
                    return code, lines, list(self._stderr)
                lines.append(line)

    def terminate(self):
        with self.lock:
            if self.proc is None:
                return
            try:
                self.proc.kill()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=5)
            except Exception:
                pass
            self.proc = None
            self.connected = False

    def describe(self):
        return {
            "id": self.id,
            "name": self.name,
            "connected": self.connected and self.alive(),
            "session": {key: value for key, value in self.session.items()
                        if key != "password"},
            "remote_dir": self.remote_dir,
            "local_dir": self.local_dir,
            "observed_hostkey": self.observed_hostkey,
            "error": self.error,
        }


def get_tab(tab_id=None):
    with STATE_LOCK:
        if not STATE["tabs"]:
            return None
        if tab_id is None:
            tab_id = STATE["active_tab"]
        for tab in STATE["tabs"]:
            if tab.id == tab_id:
                return tab
        return STATE["tabs"][0]


def require_tab(tab_id=None):
    tab = get_tab(tab_id)
    if tab is None or not tab.connected:
        return None
    return tab


# ---------------------------------------------------------------------------
# The transfer queue
# ---------------------------------------------------------------------------

class Task(object):
    def __init__(self, task_id, kind, command, session, source, destination,
                 tab_id, progress_path=None, progress_remote=None):
        self.id = task_id
        self.kind = kind
        self.command = command
        self.session = dict(session)
        self.source = source
        self.destination = destination
        self.tab_id = tab_id
        self.status = "Queued"
        self.error = ""
        self.bytes_done = 0
        self.bytes_total = 0
        self.detail = ""
        self.started = ""
        self.finished = ""
        self.proc = None
        self.progress_path = progress_path      # local file that grows
        self.progress_remote = progress_remote  # (tab_id, remote path)

    def as_dict(self):
        percent = 0
        if self.bytes_total and self.bytes_total > 0:
            percent = min(100, int(self.bytes_done * 100 / self.bytes_total))
        return {
            "id": self.id,
            "type": self.kind,
            "source": self.source,
            "destination": self.destination,
            "status": self.status,
            "error": self.error,
            "bytes_done": self.bytes_done,
            "bytes_total": self.bytes_total,
            "percent": percent,
            "detail": self.detail,
            "start_time": self.started,
            "end_time": self.finished,
            "tab": self.tab_id,
        }

    def poll_progress(self):
        if self.progress_path:
            try:
                self.bytes_done = os.path.getsize(self.progress_path)
            except OSError:
                pass
        elif self.progress_remote:
            tab_id, remote_path = self.progress_remote
            tab = get_tab(tab_id)
            if tab is not None and tab.connected:
                # A session that is busy with a transfer is free to answer:
                # the transfer runs in its own process.
                try:
                    info = remote_stat(tab, remote_path, timeout=10)
                    if info is not None:
                        self.bytes_done = info["size"]
                except Exception:
                    pass


class TaskManager(object):
    def __init__(self):
        self.condition = threading.Condition()
        threading.Thread(target=self._worker, daemon=True).start()

    def submit(self, kind, command, session, source, destination, tab_id,
               progress_path=None, progress_remote=None):
        with STATE_LOCK:
            task = Task(STATE["next_task"], kind, command, session, source,
                        destination, tab_id, progress_path, progress_remote)
            STATE["next_task"] += 1
            STATE["tasks"].insert(0, task)
        with self.condition:
            self.condition.notify_all()
        add_log("Queued %s: %s -> %s" % (kind, source, destination), "info")
        return task

    def _next(self):
        with STATE_LOCK:
            if STATE["paused"]:
                return None
            for task in reversed(STATE["tasks"]):
                if task.status == "Queued":
                    return task
        return None

    def _worker(self):
        while True:
            with self.condition:
                task = self._next()
                if task is None:
                    self.condition.wait(1.0)
                    continue
            self._run(task)

    def _run(self, task):
        task.status = "Running"
        task.started = time.strftime("%H:%M:%S")
        task.error = ""
        add_log("%s started: %s" % (task.kind, task.command), "cmd")
        # A task runs in its own process, so it opens its own connection;
        # that is what makes it cancellable without dropping the session
        # the panes are browsing.
        cmd = core_command_line(task.session)
        cmd.append("-command=" + "open " + session_url(task.session) +
                   " ; option confirm off ; " + task.command)
        try:
            task.proc = subprocess.Popen(cmd, **popen_kwargs())
        except OSError as exc:
            task.status = "Failed"
            task.error = str(exc)
            add_log(str(exc), "error")
            return

        captured = []

        def reader(stream, sink):
            try:
                for line in stream:
                    text = line.rstrip("\r\n")
                    if text:
                        sink(text)
            except (ValueError, OSError):
                pass

        def on_output(text):
            captured.append(text)
            if text.startswith(("Uploaded", "Downloaded", "Created", "Removed")):
                task.detail = text
                add_log(text, "info")

        threading.Thread(target=reader, args=(task.proc.stdout, on_output), daemon=True).start()
        threading.Thread(target=reader,
                         args=(task.proc.stderr, lambda text: add_log(text, "error")),
                         daemon=True).start()

        while task.proc.poll() is None:
            if task.status == "Cancelling":
                try:
                    task.proc.kill()
                except OSError:
                    pass
                break
            task.poll_progress()
            time.sleep(0.4)
        task.poll_progress()
        try:
            task.proc.wait(timeout=10)
        except Exception:
            pass
        code = task.proc.returncode
        if task.status == "Cancelling":
            task.status = "Cancelled"
            task.finished = time.strftime("%H:%M:%S")
            add_log("%s cancelled: %s" % (task.kind, task.source), "info")
            return
        task.status = "Completed" if code == 0 else "Failed"
        task.finished = time.strftime("%H:%M:%S")
        if code != 0:
            task.error = "\n".join(captured[-6:]) or "The operation failed."
        add_log("%s %s: %s" % (task.kind, task.status.lower(), task.source),
                "success" if code == 0 else "error")
        # Refresh the panes of the tab this task belongs to.
        tab = get_tab(task.tab_id)
        if tab is not None and tab.connected:
            code, lines = tab.run("pwd")
            if code == 0:
                for line in lines:
                    if line.startswith("/"):
                        tab.remote_dir = line.strip()
                        break

    # -- queue controls ----------------------------------------------------

    def cancel(self, task_id):
        with STATE_LOCK:
            for task in STATE["tasks"]:
                if str(task.id) == str(task_id):
                    if task.status == "Queued":
                        task.status = "Cancelled"
                        task.finished = time.strftime("%H:%M:%S")
                    elif task.status == "Running":
                        task.status = "Cancelling"
                    return True
        return False

    def retry(self, task_id):
        with STATE_LOCK:
            for task in STATE["tasks"]:
                if str(task.id) == str(task_id):
                    if task.status not in ("Failed", "Cancelled"):
                        return False
                    task.status = "Queued"
                    task.error = ""
                    task.finished = ""
                    task.bytes_done = 0
                    break
            else:
                return False
        with self.condition:
            self.condition.notify_all()
        return True

    def pause(self):
        with STATE_LOCK:
            STATE["paused"] = True
        add_log("Queue paused; queued transfers wait.", "info")

    def resume(self):
        with STATE_LOCK:
            STATE["paused"] = False
        with self.condition:
            self.condition.notify_all()
        add_log("Queue resumed.", "info")

    def clear_finished(self):
        with STATE_LOCK:
            STATE["tasks"] = [task for task in STATE["tasks"]
                              if task.status in ("Queued", "Running", "Cancelling")]

    def cancel_all(self):
        with STATE_LOCK:
            for task in STATE["tasks"]:
                if task.status == "Queued":
                    task.status = "Cancelled"
                elif task.status == "Running":
                    task.status = "Cancelling"


TASKS = TaskManager()


# ---------------------------------------------------------------------------
# Listing helpers
# ---------------------------------------------------------------------------

def quote_remote(path):
    """Quote a remote path so that it survives the command splitter."""
    if path and not any(ch in path for ch in " \t\"';"):
        return path
    return '"' + path.replace('"', '\\"') + '"'


def parse_ls_output(stdout):
    entries = []
    lines = stdout.splitlines() if isinstance(stdout, str) else list(stdout)
    in_listing = False
    for line in lines:
        line = line.strip()
        if line.startswith("Reading directory"):
            in_listing = True
            continue
        if "entries" in line or line.startswith("sftp>") or line.startswith("Connecting") \
                or line.startswith("Connected") or line.startswith("LSCP-RESULT"):
            continue
        if not in_listing or not line:
            continue

        parts = line.split(None, 8)
        if len(parts) >= 9:
            perms, links, owner, group, size_str, m1, m2, m3, name = parts
            is_dir = perms.startswith("d")
            try:
                size = int(size_str)
            except ValueError:
                size = 0
            date_str = "%s %s %s" % (m1, m2, m3)
            if name in (".", ".."):
                continue
            entries.append({
                "name": name,
                "is_dir": is_dir,
                "size": size,
                "permissions": perms,
                "date": date_str,
                "owner": "%s:%s" % (owner, group)
            })
    # Sort folders first, then files
    entries.sort(key=lambda x: (not x["is_dir"], x["name"].lower()))
    return entries


def list_windows_drives():
    import string
    entries = []
    for letter in string.ascii_uppercase:
        root = "%s:\\" % letter
        if os.path.exists(root):
            entries.append({
                "name": root, "is_dir": True, "size": 0,
                "permissions": "drwxr-xr-x", "date": "", "mtime": 0, "owner": ""
            })
    return entries


def list_local_directory(path):
    if IS_WINDOWS and path in ("/", "\\", ""):
        return list_windows_drives(), None
    if not os.path.isabs(path):
        path = os.path.abspath(os.path.join(os.path.expanduser("~"), path))
    if not os.path.exists(path):
        return None, "Path does not exist"
    if not os.path.isdir(path):
        return None, "Path is not a directory"

    entries = []
    try:
        with os.scandir(path) as it:
            for item in it:
                try:
                    st = item.stat()
                    is_dir = item.is_dir()
                    date_str = time.strftime("%b %d %H:%M", time.localtime(st.st_mtime))
                    perms = stat.filemode(st.st_mode)
                    entries.append({
                        "name": item.name,
                        "is_dir": is_dir,
                        "size": st.st_size if not is_dir else 0,
                        "permissions": perms,
                        "date": date_str,
                        "mtime": st.st_mtime,
                        "owner": "%s:%s" % (st.st_uid, st.st_gid)
                    })
                except OSError:
                    continue
    except Exception as exc:
        return None, str(exc)

    entries.sort(key=lambda x: (not x["is_dir"], x["name"].lower()))
    return entries, None


# ---------------------------------------------------------------------------
# Synchronization plans (the console client does the comparison)
# ---------------------------------------------------------------------------

SYNC_ACTIONS = ("download", "upload", "delete-local", "delete-remote", "conflict")


def build_sync_command(mode, local_dir, remote_dir, delete=False, criteria="time",
                       filemask="", recursive=True, preview=False):
    command = "synchronize " + mode
    if delete:
        command += " -delete"
    if preview:
        command += " -preview"
    if not recursive:
        command += " -nonrecursive"
    if criteria and criteria != "time":
        command += " -criteria=" + criteria
    if filemask:
        command += ' -filemask="%s"' % filemask.replace('"', "")
    command += ' "%s" "%s"' % (local_dir, remote_dir)
    return command


def parse_sync_preview(output):
    """Turn the 'synchronize -preview' output into a plan the UI can show."""
    items = []
    summary = {}
    header = {}
    for line in output:
        line = line.rstrip("\r\n")
        if line.startswith("SYNCPLAN"):
            # The first field carries 'local=...'; the rest are tab separated.
            for field in line[len("SYNCPLAN"):].split("\t"):
                if "=" in field:
                    key, value = field.split("=", 1)
                    header[key] = value
            continue
        if line.startswith("SYNCSUMMARY"):
            for field in line.split(" ")[1:]:
                if "=" in field:
                    key, value = field.split("=", 1)
                    try:
                        summary[key] = int(value)
                    except ValueError:
                        summary[key] = value
            continue
        parts = line.split("\t")
        if parts and parts[0] in SYNC_ACTIONS and len(parts) >= 2:
            items.append({
                "action": parts[0],
                "path": parts[1],
                "reason": parts[2] if len(parts) > 2 else "",
            })
    return {"header": header, "items": items, "summary": summary}


# ---------------------------------------------------------------------------
# HTTP API
# ---------------------------------------------------------------------------

class LinuxScpGuiHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self):
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length <= 0:
            return {}
        raw = self.rfile.read(content_length).decode("utf-8")
        try:
            return json.loads(raw)
        except Exception:
            return {}

    # -- GET ---------------------------------------------------------------

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/api/status":
            with STATE_LOCK:
                tabs = [tab.describe() for tab in STATE["tabs"]]
                active = STATE["active_tab"]
            tab = get_tab()
            self.send_json({
                "connected": tab is not None and tab.connected,
                "session": tab.session if tab else new_session(),
                "local_dir": tab.local_dir if tab else os.path.expanduser("~"),
                "remote_dir": tab.remote_dir if tab else "/",
                "binary": LINUXSCP_BIN,
                "tabs": tabs,
                "active_tab": active,
                "paused": STATE["paused"],
                "sessions_file": session_store_path(),
            })
            return

        if path == "/api/logs":
            with STATE_LOCK:
                self.send_json({"logs": list(STATE["logs"])})
            return

        if path == "/api/transfers":
            with STATE_LOCK:
                self.send_json({
                    "transfers": [task.as_dict() for task in STATE["tasks"]],
                    "paused": STATE["paused"],
                })
            return

        if path == "/api/sessions":
            self.send_json({"sessions": read_stored_sessions(),
                            "file": session_store_path()})
            return

        if path == "/api/local/list":
            req_path = query.get("path", [None])[0]
            tab = get_tab(query.get("tab", [None])[0])
            if not req_path:
                req_path = tab.local_dir if tab else os.path.expanduser("~")
            if not os.path.isabs(req_path):
                base = tab.local_dir if tab else os.path.expanduser("~")
                req_path = os.path.abspath(os.path.join(base, req_path))
            entries, err = list_local_directory(req_path)
            if err:
                self.send_json({"success": False, "error": err}, status=400)
            else:
                if tab is not None:
                    tab.local_dir = req_path
                self.send_json({"success": True, "path": req_path, "entries": entries})
            return

        if path == "/api/remote/list":
            tab = require_tab(query.get("tab", [None])[0])
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            req_path = query.get("path", [None])[0] or tab.remote_dir
            code, lines = tab.run_many(["cd " + quote_remote(req_path), "pwd", "ls -la"])
            if code != 0:
                self.send_json({"success": False,
                                "error": "\n".join(lines) or "Directory read failed"},
                               status=400)
                return
            resolved = req_path
            for line in lines:
                if line.startswith("/"):
                    resolved = line.strip()
                    break
            tab.remote_dir = resolved
            self.send_json({"success": True, "path": resolved,
                            "entries": parse_ls_output(lines)})
            return

        if path == "/api/remote/edit":
            tab = require_tab(query.get("tab", [None])[0])
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            remote_path = query.get("path", [""])[0]
            if not remote_path:
                self.send_json({"success": False, "error": "No file given"}, status=400)
                return
            content, info, error = download_remote_text(tab, remote_path)
            if error:
                self.send_json({"success": False, "error": error}, status=400)
                return
            self.send_json({"success": True, "path": remote_path, "content": content,
                            "info": info})
            return

        return super().do_GET()

    # -- POST --------------------------------------------------------------

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        body = self.read_json_body()

        if path == "/api/tabs/open":
            session = dict(new_session())
            given = body.get("session") or {}
            for key in ("protocol", "host", "port", "username", "password",
                        "keyfile", "hostkey", "initial_dir", "local_dir"):
                if key in given:
                    session[key] = given[key]
            with STATE_LOCK:
                tab = CoreSession(STATE["next_tab"], session,
                                  name=body.get("name") or given.get("name"))
                STATE["next_tab"] += 1
            add_log("Connecting to %s (%s)..." % (session.get("host"), session.get("protocol")),
                    "info")
            ok, error = tab.connect()
            with STATE_LOCK:
                if ok or not STATE["tabs"]:
                    STATE["tabs"].append(tab)
                    STATE["active_tab"] = tab.id
            if ok:
                add_log("Connected to %s" % tab.name, "success")
                self.send_json({"success": True, "tab": tab.describe()})
            else:
                add_log("Connection failed: %s" % error, "error")
                self.send_json({"success": False, "error": error,
                                "tab": tab.describe()}, status=400)
            return

        if path == "/api/tabs/close":
            tab_id = body.get("tab")
            with STATE_LOCK:
                for index, tab in enumerate(STATE["tabs"]):
                    if tab.id == tab_id or str(tab.id) == str(tab_id):
                        STATE["tabs"].pop(index)
                        tab.terminate()
                        if STATE["active_tab"] == tab.id:
                            STATE["active_tab"] = STATE["tabs"][0].id if STATE["tabs"] else None
                        break
            add_log("Closed session tab %s" % tab_id, "info")
            self.send_json({"success": True})
            return

        if path == "/api/tabs/select":
            tab_id = body.get("tab")
            tab = get_tab(tab_id)
            if tab is not None:
                with STATE_LOCK:
                    STATE["active_tab"] = tab.id
                self.send_json({"success": True, "tab": tab.describe()})
            else:
                self.send_json({"success": False, "error": "No such tab"}, status=400)
            return

        if path == "/api/tabs/reconnect":
            tab = get_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "No such tab"}, status=400)
                return
            ok, error = tab.connect()
            self.send_json({"success": ok, "tab": tab.describe(),
                            "error": error if not ok else ""},
                           status=200 if ok else 400)
            return

        if path == "/api/session/terminal":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            add_log("Opening a terminal on the server", "info")
            code, stdout, stderr = run_linuxscp(
                "open %s ; terminal" % session_url(tab.session), tab.session)
            if code == 0:
                add_log("Terminal opened", "success")
                self.send_json({"success": True, "output": stdout})
            else:
                message = stderr.strip() or stdout.strip() or "Cannot open a terminal"
                add_log(message, "error")
                self.send_json({"success": False, "error": message}, status=400)
            return

        if path == "/api/sessions/save":
            name = (body.get("name") or "").strip()
            session = body.get("session") or {}
            if not name:
                self.send_json({"success": False, "error": "A session name is required"},
                               status=400)
                return
            command = "savesession %s" % quote_remote(name)
            for key, flag in (("host", "-host"), ("port", "-port"), ("username", "-username"),
                              ("protocol", "-protocol"), ("keyfile", "-privatekey"),
                              ("hostkey", "-hostkey"), ("initial_dir", "-remotedir"),
                              ("local_dir", "-localdir")):
                value = session.get(key)
                if value not in (None, ""):
                    if key == "hostkey" and value == "*" and not body.get("pin_hostkey"):
                        continue
                    command += ' %s="%s"' % (flag, str(value).replace('"', ""))
            code, stdout, stderr = run_linuxscp(command, session or new_session())
            if code == 0:
                add_log("Saved session '%s'" % name, "success")
                self.send_json({"success": True, "sessions": read_stored_sessions()})
            else:
                message = stderr.strip() or stdout.strip() or "Cannot save the session"
                add_log(message, "error")
                self.send_json({"success": False, "error": message}, status=400)
            return

        if path == "/api/sessions/delete":
            name = (body.get("name") or "").strip()
            if not name:
                self.send_json({"success": False, "error": "A session name is required"},
                               status=400)
                return
            code, stdout, stderr = run_linuxscp("rmsession %s" % quote_remote(name))
            if code == 0:
                add_log("Removed session '%s'" % name, "success")
                self.send_json({"success": True, "sessions": read_stored_sessions()})
            else:
                message = stderr.strip() or stdout.strip() or "Cannot remove the session"
                self.send_json({"success": False, "error": message}, status=400)
            return

        if path == "/api/local/mkdir":
            tab = get_tab(body.get("tab"))
            base = tab.local_dir if tab else os.path.expanduser("~")
            target = os.path.join(base, body.get("name") or "")
            try:
                os.makedirs(target, exist_ok=True)
                add_log("Created local directory: %s" % target, "success")
                self.send_json({"success": True})
            except Exception as exc:
                self.send_json({"success": False, "error": str(exc)}, status=400)
            return

        if path == "/api/local/delete":
            tab = get_tab(body.get("tab"))
            base = tab.local_dir if tab else os.path.expanduser("~")
            names = body.get("names") or ([body["name"]] if body.get("name") else [])
            errors = []
            for name in names:
                target = os.path.join(base, name)
                try:
                    if os.path.isdir(target) and not os.path.islink(target):
                        shutil.rmtree(target)
                    else:
                        os.remove(target)
                    add_log("Deleted local item: %s" % name, "success")
                except Exception as exc:
                    errors.append("%s: %s" % (name, exc))
            if errors:
                self.send_json({"success": False, "error": "; ".join(errors)}, status=400)
            else:
                self.send_json({"success": True})
            return

        if path == "/api/local/rename":
            tab = get_tab(body.get("tab"))
            base = tab.local_dir if tab else os.path.expanduser("~")
            old, new = body.get("old_name"), body.get("new_name")
            if not old or not new:
                self.send_json({"success": False, "error": "Both names are required"},
                               status=400)
                return
            try:
                os.rename(os.path.join(base, old), os.path.join(base, new))
                add_log("Renamed %s to %s" % (old, new), "success")
                self.send_json({"success": True})
            except Exception as exc:
                self.send_json({"success": False, "error": str(exc)}, status=400)
            return

        if path == "/api/remote/mkdir":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            code, lines = tab.run_many(["cd " + quote_remote(tab.remote_dir),
                                        "mkdir " + quote_remote(body.get("name") or "")])
            if code == 0:
                add_log("Created remote directory: %s" % body.get("name"), "success")
                self.send_json({"success": True})
            else:
                self.send_json({"success": False,
                                "error": "\n".join(lines) or "mkdir failed"}, status=400)
            return

        if path == "/api/remote/delete":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            names = body.get("names") or []
            errors, deleted = [], []
            for item in names:
                name = item if isinstance(item, str) else item.get("name")
                is_dir = item.get("is_dir", False) if isinstance(item, dict) else False
                code, lines = tab.run_many([
                    "cd " + quote_remote(tab.remote_dir),
                    ("rmdir" if is_dir else "rm") + " " + quote_remote(name or "")])
                if code == 0:
                    deleted.append(name)
                else:
                    errors.append("\n".join(lines))
            if errors:
                self.send_json({"success": False, "error": "; ".join(errors)}, status=400)
            else:
                add_log("Deleted remote item(s): %s" % ", ".join(deleted), "success")
                self.send_json({"success": True})
            return

        if path == "/api/remote/rename":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            code, lines = tab.run_many([
                "cd " + quote_remote(tab.remote_dir),
                "mv " + quote_remote(body.get("old_name") or "") + " " +
                quote_remote(body.get("new_name") or "")])
            if code == 0:
                add_log("Renamed %s to %s" % (body.get("old_name"), body.get("new_name")),
                        "success")
                self.send_json({"success": True})
            else:
                self.send_json({"success": False,
                                "error": "\n".join(lines) or "Rename failed"}, status=400)
            return

        if path == "/api/remote/chmod":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            code, lines = tab.run_many([
                "cd " + quote_remote(tab.remote_dir),
                "chmod %s %s" % (body.get("permissions") or "644",
                                 quote_remote(body.get("name") or ""))])
            if code == 0:
                self.send_json({"success": True})
            else:
                self.send_json({"success": False,
                                "error": "\n".join(lines) or "chmod failed"}, status=400)
            return

        if path == "/api/remote/search":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            pattern = body.get("pattern") or "*"
            start = body.get("path") or tab.remote_dir
            limit = int(body.get("limit") or 500)
            case_sensitive = bool(body.get("case_sensitive"))
            try:
                matches = remote_search(tab, start, pattern, limit, case_sensitive)
            except Exception as exc:
                self.send_json({"success": False, "error": str(exc)}, status=400)
                return
            self.send_json({"success": True, "matches": matches, "truncated": len(matches) >= limit})
            return

        if path == "/api/remote/save":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            remote_path = body.get("path") or ""
            content = body.get("content")
            if not remote_path or content is None:
                self.send_json({"success": False, "error": "A path and content are required"},
                               status=400)
                return
            ok, error = upload_remote_text(tab, remote_path, content,
                                           body.get("base_mtime"))
            if ok:
                self.send_json({"success": True})
            else:
                self.send_json({"success": False, "error": error}, status=400)
            return

        if path == "/api/transfer/download":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            names = body.get("names") or []
            local_dir = body.get("local_dir") or tab.local_dir
            switches = transfer_switches(body)
            for name in names:
                remote_path = posixpath.join(tab.remote_dir, name)
                local_target = os.path.join(local_dir, name)
                command = 'lcd "%s" ; cd %s ; get %s %s' % (
                    local_dir, quote_remote(tab.remote_dir), switches, quote_remote(name))
                task = TASKS.submit("Download", command, tab.session, remote_path,
                                    local_target, tab.id, progress_path=local_target)
                try:
                    task.bytes_total = remote_size(tab, remote_path) or 0
                except Exception:
                    task.bytes_total = 0
            self.send_json({"success": True})
            return

        if path == "/api/transfer/upload":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            names = body.get("names") or []
            remote_dir = body.get("remote_dir") or tab.remote_dir
            switches = transfer_switches(body)
            for name in names:
                local_path = os.path.join(tab.local_dir, name)
                remote_target = posixpath.join(remote_dir, name)
                command = 'lcd "%s" ; cd %s ; put %s %s' % (
                    tab.local_dir, quote_remote(remote_dir), switches,
                    '"%s"' % name)
                task = TASKS.submit("Upload", command, tab.session, local_path,
                                    remote_target, tab.id,
                                    progress_remote=(tab.id, remote_target))
                try:
                    task.bytes_total = os.path.getsize(local_path)
                except OSError:
                    task.bytes_total = 0
            self.send_json({"success": True})
            return

        if path == "/api/sync/preview":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            command = build_sync_command(
                body.get("mode") or "local",
                body.get("local_dir") or tab.local_dir,
                body.get("remote_dir") or tab.remote_dir,
                delete=bool(body.get("delete")),
                criteria=body.get("criteria") or "time",
                filemask=body.get("filemask") or "",
                recursive=body.get("recursive", True),
                preview=True)
            code, stdout, stderr = run_linuxscp(
                "open %s ; %s" % (session_url(tab.session, tab.remote_dir), command),
                tab.session)
            if code != 0:
                self.send_json({"success": False,
                                "error": stderr.strip() or stdout.strip() or
                                "Cannot read the synchronization plan"}, status=400)
                return
            plan = parse_sync_preview(stdout.splitlines())
            plan["command"] = command
            add_log("Synchronization preview: %s item(s)" % len(plan["items"]), "info")
            self.send_json({"success": True, "plan": plan})
            return

        if path == "/api/sync/run":
            tab = require_tab(body.get("tab"))
            if tab is None:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            local_dir = body.get("local_dir") or tab.local_dir
            remote_dir = body.get("remote_dir") or tab.remote_dir
            command = build_sync_command(
                body.get("mode") or "local", local_dir, remote_dir,
                delete=bool(body.get("delete")),
                criteria=body.get("criteria") or "time",
                filemask=body.get("filemask") or "",
                recursive=body.get("recursive", True))
            task = TASKS.submit("Synchronize (%s)" % (body.get("mode") or "local"),
                                command, tab.session, local_dir, remote_dir, tab.id)
            self.send_json({"success": True, "task_id": task.id})
            return

        if path == "/api/queue/cancel":
            ok = TASKS.cancel(body.get("task_id"))
            self.send_json({"success": ok})
            return

        if path == "/api/queue/retry":
            ok = TASKS.retry(body.get("task_id"))
            self.send_json({"success": ok})
            return

        if path == "/api/queue/pause":
            TASKS.pause()
            self.send_json({"success": True})
            return

        if path == "/api/queue/resume":
            TASKS.resume()
            self.send_json({"success": True})
            return

        if path == "/api/queue/clear":
            TASKS.clear_finished()
            self.send_json({"success": True})
            return

        if path == "/api/command/exec":
            cmd_text = body.get("command", "")
            if not cmd_text:
                self.send_json({"success": False, "error": "Empty command"}, status=400)
                return
            tab = get_tab(body.get("tab"))
            if tab is not None and tab.connected:
                # Run it in the live session: 'cd' and 'lcd' then stick.
                # '-command=' splits on ';', so the terminal does too.
                code, lines, errors = tab.run_many_capture(
                    [part for part in cmd_text.split(";") if part.strip()])
                if code == 0:
                    code, pwd = tab.run("pwd")
                    for line in pwd:
                        if line.startswith("/"):
                            tab.remote_dir = line.strip()
                            break
                self.send_json({"success": code == 0, "exit_code": code,
                                "stdout": "\n".join(lines),
                                "stderr": "\n".join(errors)})
                return
            code, stdout, stderr = run_linuxscp(cmd_text)
            self.send_json({"success": code == 0, "exit_code": code,
                            "stdout": stdout, "stderr": stderr})
            return

        self.send_json({"error": "Not Found"}, status=404)


# ---------------------------------------------------------------------------
# Helpers used by the endpoints
# ---------------------------------------------------------------------------

def transfer_switches(body):
    switches = []
    if body.get("resume", True):
        switches.append("-resume")
    if body.get("recursive", True):
        switches.append("-r")
    if body.get("newer_only"):
        switches.append("-neweronly")
    if body.get("preserve_time", True):
        switches.append("-preservetime")
    return " ".join(switches)


def remote_stat(tab, remote_path, timeout=30):
    """Parse the 'stat' output of the core into a dictionary."""
    code, lines = tab.run("stat " + quote_remote(remote_path), timeout=timeout)
    if code != 0:
        return None
    info = {"path": remote_path, "size": 0, "mtime": 0, "is_dir": False,
            "permissions": "", "raw": "\n".join(lines)}
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("SizeBytes:"):
            info["size"] = int(stripped.split(":", 1)[1].strip() or 0)
        elif stripped.startswith("ModifiedEpoch:"):
            info["mtime"] = int(stripped.split(":", 1)[1].strip() or 0)
        elif stripped.startswith("Permissions:"):
            info["permissions"] = stripped.split(":", 1)[1].strip()
            info["is_dir"] = info["permissions"].startswith("d")
        elif stripped.startswith("Size:"):
            digits = "".join(ch for ch in stripped.split(":", 1)[1] if ch.isdigit())
            if digits and not info["size"]:
                info["size"] = int(digits)
    return info


def remote_size(tab, remote_path):
    info = remote_stat(tab, remote_path)
    return info["size"] if info else 0


def remote_search(tab, start, pattern, limit=500, case_sensitive=False, depth=0, matches=None):
    """Recursive search over the remote tree, breadth first."""
    if matches is None:
        matches = []
    if depth > 12 or len(matches) >= limit:
        return matches
    code, lines = tab.run_many(["cd " + quote_remote(start), "ls -la"], timeout=30)
    if code != 0:
        return matches
    entries = parse_ls_output(lines)
    needle = pattern if case_sensitive else pattern.lower()
    for entry in entries:
        name = entry["name"] if case_sensitive else entry["name"].lower()
        if fnmatch(name, needle):
            matches.append({
                "path": posixpath.join(start, entry["name"]),
                "name": entry["name"],
                "is_dir": entry["is_dir"],
                "size": entry.get("size", 0),
                "date": entry.get("date", ""),
                "permissions": entry.get("permissions", ""),
            })
            if len(matches) >= limit:
                return matches
    for entry in entries:
        if entry["is_dir"]:
            remote_search(tab, posixpath.join(start, entry["name"]), pattern,
                          limit, case_sensitive, depth + 1, matches)
            if len(matches) >= limit:
                break
    return matches


def edit_temp_root():
    root = os.path.join(tempfile.gettempdir(), "linuxscp-gui-edit")
    os.makedirs(root, exist_ok=True)
    return root


def temp_path_for(remote_path):
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in remote_path)
    return os.path.join(edit_temp_root(), "%d-%s" % (int(time.time()), safe[-64:]))


def download_remote_text(tab, remote_path, max_size=8 * 1024 * 1024):
    """Download a remote file for the editor. Returns (content, info, error)."""
    info = remote_stat(tab, remote_path)
    if info is None:
        return None, {}, "Cannot read the file."
    if info["is_dir"]:
        return None, {}, "That is a directory, not a file."
    size = info["size"]
    if size and size > max_size:
        return None, {}, "The file is %d bytes, too large for the editor (limit %d)." % (
            size, max_size)
    local = temp_path_for(remote_path)
    code, lines = tab.run("get -preservetime %s \"%s\"" % (
        quote_remote(remote_path), local), timeout=300)
    if code != 0:
        return None, {}, "\n".join(lines) or "Cannot download the file."
    try:
        with open(local, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        return None, {}, str(exc)
    if b"\0" in data:
        return None, {}, "The file is binary; the editor can only edit text files."
    info = {"size": len(data), "mtime": int(os.path.getmtime(local)),
            "temp": local, "path": remote_path}
    try:
        return data.decode("utf-8"), info, ""
    except UnicodeDecodeError:
        return data.decode("utf-8", "replace"), info, ""


def upload_remote_text(tab, remote_path, content, base_mtime=None):
    """Upload edited content, refusing to clobber a file changed since it
    was opened."""
    if base_mtime:
        info = remote_stat(tab, remote_path)
        if info is not None:
            current = info["mtime"]
            if current and int(base_mtime) and current != int(base_mtime):
                return False, ("The file changed on the server while it was open "
                               "(reload it before saving, or save it under a new name).")
    local = temp_path_for(remote_path)
    try:
        with open(local, "wb") as handle:
            handle.write(content.encode("utf-8"))
    except OSError as exc:
        return False, str(exc)
    code, lines = tab.run("put \"%s\" %s" % (local, quote_remote(remote_path)),
                          timeout=300)
    if code != 0:
        return False, "\n".join(lines) or "Cannot upload the file."
    try:
        os.remove(local)
    except OSError:
        pass
    add_log("Saved %s" % remote_path, "success")
    return True, ""


def main():
    port = PORT
    host = HOST
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            try:
                port = int(arg.split("=", 1)[1])
            except ValueError:
                pass
        elif arg.startswith("--host="):
            host = arg.split("=", 1)[1]

    server_address = (host, port)
    httpd = ThreadingHTTPServer(server_address, LinuxScpGuiHandler)
    print("LinuxSCP Commander GUI running at http://localhost:%d/" % port)
    print("Serving web client from: %s" % WEB_DIR)
    print("LinuxSCP core binary: %s" % LINUXSCP_BIN)
    print("Stored sessions: %s" % session_store_path())
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping LinuxSCP GUI server...")
        TASKS.cancel_all()
        for tab in list(STATE["tabs"]):
            tab.terminate()
        httpd.server_close()


if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    main()
