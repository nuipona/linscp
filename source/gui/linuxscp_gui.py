#!/usr/bin/env python3
"""
LinuxSCP Graphical Commander Interface Server
Provides a web-based dual-pane Norton/WinSCP Commander interface
backed by the LinuxSCP protocol engine.
"""

import os
import sys
import json
import time
import shutil
import stat
import getpass
import subprocess
import threading
import urllib.parse
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

# Global Application State
STATE = {
    "connected": False,
    "session": {
        "protocol": "sftp",
        "host": "localhost",
        "port": 22,
        "username": os.environ.get("USERNAME") or os.environ.get("USER") or _default_user(),
        "password": "",
        "keyfile": "",
        "hostkey": "*",
        "initial_dir": "/"
    },
    "local_dir": os.path.expanduser("~"),
    "remote_dir": "/",
    "logs": [],
    "transfers": [],
    "lock": threading.Lock()
}

def add_log(message, level="info"):
    timestamp = time.strftime("%H:%M:%S")
    with STATE["lock"]:
        entry = {"time": timestamp, "text": message, "level": level}
        STATE["logs"].append(entry)
        if len(STATE["logs"]) > 200:
            STATE["logs"].pop(0)

def run_linuxscp(command_script, session=None):
    if session is None:
        session = STATE["session"]
    
    bin_path = find_linuxscp_binary()
    proto = session.get("protocol", "sftp")
    host = session.get("host", "localhost")
    port = session.get("port", 22)
    user = session.get("username", "")
    pw = session.get("password", "")
    key = session.get("keyfile", "")
    hostkey = session.get("hostkey", "*")

    url = f"{proto}://"
    if user:
        if pw:
            url += f"{urllib.parse.quote(user)}:{urllib.parse.quote(pw)}@"
        else:
            url += f"{urllib.parse.quote(user)}@"
    url += host
    if port and port != 22:
        url += f":{port}"
    url += "/"

    # The GUI asks its own questions (the web client confirms before it
    # deletes or overwrites anything), so the core must not stop at its
    # interactive 'are you sure' guard: every operation here is
    # non-interactive and would otherwise come back as 'Cannot confirm
    # this action without a terminal'.
    full_commands = (f"open -hostkey={hostkey} {url} ; option confirm off ; "
                     f"{command_script} ; exit")
    
    cmd = [
        bin_path,
        "-batch=on",
        f"-hostkey={hostkey}",
        f"-command={full_commands}"
    ]
    if key:
        cmd.append(f"-privatekey={key}")
    
    add_log(f"Executing: {command_script}", "cmd")
    popen_kwargs = {
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
    }
    if IS_WINDOWS:
        # No console window flashing up for every operation.
        popen_kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        proc = subprocess.Popen(cmd, **popen_kwargs)
    except OSError as exc:
        message = (f"Cannot start the LinuxSCP core binary '{bin_path}': {exc}. "
                   "Build it first (windows\build.cmd or make -C source/linux) "
                   "or put linuxscp" + EXE_SUFFIX + " on PATH.")
        add_log(message, "error")
        return 1, "", message
    try:
        stdout, stderr = proc.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        message = "LinuxSCP core timed out after 120 seconds."
        add_log(message, "error")
        return 1, stdout or "", message
    add_log(f"Finished (exit code {proc.returncode})", "info" if proc.returncode == 0 else "error")
    if stderr.strip():
        add_log(stderr.strip(), "error")
    return proc.returncode, stdout, stderr

def parse_ls_output(stdout):
    entries = []
    lines = stdout.splitlines()
    in_listing = False
    for line in lines:
        line = line.strip()
        if line.startswith("Reading directory"):
            in_listing = True
            continue
        if "entries" in line or line.startswith("sftp>") or line.startswith("Connecting") or line.startswith("Connected"):
            continue
        if not in_listing:
            continue
        if not line:
            continue

        parts = line.split(None, 8)
        if len(parts) >= 9:
            perms, links, owner, group, size_str, m1, m2, m3, name = parts
            is_dir = perms.startswith("d")
            try:
                size = int(size_str)
            except ValueError:
                size = 0
            date_str = f"{m1} {m2} {m3}"
            if name in [".", ".."]:
                continue
            entries.append({
                "name": name,
                "is_dir": is_dir,
                "size": size,
                "permissions": perms,
                "date": date_str,
                "owner": f"{owner}:{group}"
            })
    # Sort folders first, then files
    entries.sort(key=lambda x: (not x["is_dir"], x["name"].lower()))
    return entries

def list_windows_drives():
    import string
    entries = []
    for letter in string.ascii_uppercase:
        root = f"{letter}:\\"
        if os.path.exists(root):
            entries.append({
                "name": root,
                "is_dir": True,
                "size": 0,
                "permissions": "drwxr-xr-x",
                "date": "",
                "mtime": 0,
                "owner": ""
            })
    return entries


def list_local_directory(path):
    if IS_WINDOWS and path in ("/", "\\\\", ""):
        return list_windows_drives(), None
    if not os.path.isabs(path):
        path = os.path.abspath(os.path.join(STATE["local_dir"], path))
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
                        "owner": f"{st.st_uid}:{st.st_gid}"
                    })
                except OSError:
                    continue
    except Exception as e:
        return None, str(e)
    
    entries.sort(key=lambda x: (not x["is_dir"], x["name"].lower()))
    return entries, None

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

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/api/status":
            with STATE["lock"]:
                self.send_json({
                    "connected": STATE["connected"],
                    "session": STATE["session"],
                    "local_dir": STATE["local_dir"],
                    "remote_dir": STATE["remote_dir"],
                    "binary": LINUXSCP_BIN
                })
            return

        if path == "/api/logs":
            with STATE["lock"]:
                self.send_json({"logs": list(STATE["logs"])})
            return

        if path == "/api/transfers":
            with STATE["lock"]:
                self.send_json({"transfers": list(STATE["transfers"])})
            return

        if path == "/api/local/list":
            req_path = query.get("path", [STATE["local_dir"]])[0]
            req_path = os.path.expanduser(req_path)
            if not os.path.isabs(req_path):
                req_path = os.path.abspath(os.path.join(STATE["local_dir"], req_path))
            
            entries, err = list_local_directory(req_path)
            if err:
                self.send_json({"success": False, "error": err}, status=400)
            else:
                STATE["local_dir"] = req_path
                self.send_json({"success": True, "path": req_path, "entries": entries})
            return

        if path == "/api/remote/list":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            req_path = query.get("path", [STATE["remote_dir"]])[0]
            code, stdout, stderr = run_linuxscp(f"cd {req_path} ; pwd ; ls -la")
            if code == 0:
                entries = parse_ls_output(stdout)
                # extract resolved pwd
                pwd_found = req_path
                for line in stdout.splitlines():
                    if line.startswith("/"):
                        pwd_found = line.strip()
                        break
                STATE["remote_dir"] = pwd_found
                self.send_json({"success": True, "path": pwd_found, "entries": entries})
            else:
                self.send_json({"success": False, "error": stderr or "Directory read failed"}, status=400)
            return

        return super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        body = self.read_json_body()

        if path == "/api/session/connect":
            session = body.get("session", {})
            for k in ["protocol", "host", "port", "username", "password", "keyfile", "hostkey", "initial_dir"]:
                if k in session:
                    STATE["session"][k] = session[k]
            
            add_log(f"Connecting to {STATE['session']['host']} via {STATE['session']['protocol']}...", "info")
            code, stdout, stderr = run_linuxscp("pwd")
            if code == 0:
                STATE["connected"] = True
                STATE["remote_dir"] = STATE["session"].get("initial_dir") or "/"
                add_log(f"Successfully connected to {STATE['session']['host']}", "success")
                self.send_json({"success": True, "remote_dir": STATE["remote_dir"]})
            else:
                STATE["connected"] = False
                err_msg = stderr or stdout or "Failed to connect"
                add_log(f"Connection failed: {err_msg}", "error")
                self.send_json({"success": False, "error": err_msg}, status=400)
            return

        if path == "/api/session/disconnect":
            STATE["connected"] = False
            add_log("Disconnected from session", "info")
            self.send_json({"success": True})
            return

        if path == "/api/local/mkdir":
            folder_name = body.get("name")
            target = os.path.join(STATE["local_dir"], folder_name)
            try:
                os.makedirs(target, exist_ok=True)
                add_log(f"Created local directory: {folder_name}", "success")
                self.send_json({"success": True})
            except Exception as e:
                self.send_json({"success": False, "error": str(e)}, status=400)
            return

        if path == "/api/local/delete":
            name = body.get("name")
            target = os.path.join(STATE["local_dir"], name)
            try:
                if os.path.isdir(target):
                    shutil.rmtree(target)
                else:
                    os.remove(target)
                add_log(f"Deleted local item: {name}", "success")
                self.send_json({"success": True})
            except Exception as e:
                self.send_json({"success": False, "error": str(e)}, status=400)
            return

        if path == "/api/remote/mkdir":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            name = body.get("name")
            code, stdout, stderr = run_linuxscp(f"cd {STATE['remote_dir']} ; mkdir {name}")
            if code == 0:
                add_log(f"Created remote directory: {name}", "success")
                self.send_json({"success": True})
            else:
                self.send_json({"success": False, "error": stderr or "mkdir failed"}, status=400)
            return

        if path == "/api/remote/delete":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            name = body.get("name")
            is_dir = body.get("is_dir", False)
            cmd_name = "rmdir" if is_dir else "rm"
            code, stdout, stderr = run_linuxscp(f"cd {STATE['remote_dir']} ; {cmd_name} {name}")
            if code == 0:
                add_log(f"Deleted remote item: {name}", "success")
                self.send_json({"success": True})
            else:
                self.send_json({"success": False, "error": stderr or "Delete failed"}, status=400)
            return

        if path == "/api/remote/rename":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            old_name = body.get("old_name")
            new_name = body.get("new_name")
            code, stdout, stderr = run_linuxscp(f"cd {STATE['remote_dir']} ; mv {old_name} {new_name}")
            if code == 0:
                add_log(f"Renamed {old_name} to {new_name}", "success")
                self.send_json({"success": True})
            else:
                self.send_json({"success": False, "error": stderr or "Rename failed"}, status=400)
            return

        if path == "/api/transfer/download":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            remote_item = body.get("remote_item")
            local_dir = body.get("local_dir", STATE["local_dir"])
            resume = body.get("resume", True)
            recursive = body.get("recursive", True)
            newer_only = body.get("newer_only", False)

            switches = []
            if resume:
                switches.append("-resume")
            if recursive:
                switches.append("-r")
            if newer_only:
                switches.append("-neweronly")

            switch_str = " ".join(switches)
            
            task_id = str(int(time.time() * 1000))
            task = {
                "id": task_id,
                "type": "Download",
                "source": remote_item,
                "destination": local_dir,
                "status": "In Progress",
                "start_time": time.strftime("%H:%M:%S")
            }
            with STATE["lock"]:
                STATE["transfers"].insert(0, task)

            def do_download():
                cmd = f"lcd {local_dir} ; cd {STATE['remote_dir']} ; get {switch_str} {remote_item}"
                code, stdout, stderr = run_linuxscp(cmd)
                with STATE["lock"]:
                    task["status"] = "Completed" if code == 0 else "Failed"
                    if code != 0:
                        task["error"] = stderr
                add_log(f"Download of {remote_item} {'succeeded' if code == 0 else 'failed'}",
                        "success" if code == 0 else "error")

            threading.Thread(target=do_download, daemon=True).start()
            self.send_json({"success": True, "task_id": task_id})
            return

        if path == "/api/transfer/upload":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            local_item = body.get("local_item")
            remote_dir = body.get("remote_dir", STATE["remote_dir"])
            resume = body.get("resume", True)
            recursive = body.get("recursive", True)
            newer_only = body.get("newer_only", False)

            switches = []
            if resume:
                switches.append("-resume")
            if recursive:
                switches.append("-r")
            if newer_only:
                switches.append("-neweronly")

            switch_str = " ".join(switches)
            
            task_id = str(int(time.time() * 1000))
            task = {
                "id": task_id,
                "type": "Upload",
                "source": local_item,
                "destination": remote_dir,
                "status": "In Progress",
                "start_time": time.strftime("%H:%M:%S")
            }
            with STATE["lock"]:
                STATE["transfers"].insert(0, task)

            def do_upload():
                cmd = f"lcd {STATE['local_dir']} ; cd {remote_dir} ; put {switch_str} {local_item}"
                code, stdout, stderr = run_linuxscp(cmd)
                with STATE["lock"]:
                    task["status"] = "Completed" if code == 0 else "Failed"
                    if code != 0:
                        task["error"] = stderr
                add_log(f"Upload of {local_item} {'succeeded' if code == 0 else 'failed'}",
                        "success" if code == 0 else "error")

            threading.Thread(target=do_upload, daemon=True).start()
            self.send_json({"success": True, "task_id": task_id})
            return

        if path == "/api/transfer/synchronize":
            if not STATE["connected"]:
                self.send_json({"success": False, "error": "Not connected"}, status=400)
                return
            mode = body.get("mode", "local")
            delete_obsolete = body.get("delete", False)
            sync_cmd = f"synchronize {mode}"
            if delete_obsolete:
                sync_cmd += " -delete"
            sync_cmd += f" {STATE['local_dir']} {STATE['remote_dir']}"

            task_id = str(int(time.time() * 1000))
            task = {
                "id": task_id,
                "type": f"Synchronize ({mode})",
                "source": STATE['local_dir'],
                "destination": STATE['remote_dir'],
                "status": "In Progress",
                "start_time": time.strftime("%H:%M:%S")
            }
            with STATE["lock"]:
                STATE["transfers"].insert(0, task)

            def do_sync():
                code, stdout, stderr = run_linuxscp(sync_cmd)
                with STATE["lock"]:
                    task["status"] = "Completed" if code == 0 else "Failed"
                    if code != 0:
                        task["error"] = stderr
                add_log(f"Synchronization ({mode}) {'succeeded' if code == 0 else 'failed'}",
                        "success" if code == 0 else "error")

            threading.Thread(target=do_sync, daemon=True).start()
            self.send_json({"success": True, "task_id": task_id})
            return

        if path == "/api/command/exec":
            cmd_text = body.get("command", "")
            if not cmd_text:
                self.send_json({"success": False, "error": "Empty command"}, status=400)
                return
            code, stdout, stderr = run_linuxscp(cmd_text)
            self.send_json({
                "success": code == 0,
                "exit_code": code,
                "stdout": stdout,
                "stderr": stderr
            })
            return

        self.send_json({"error": "Not Found"}, status=404)

def main():
    port = PORT
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            try:
                port = int(arg.split("=")[1])
            except ValueError:
                pass

    server_address = (HOST, port)
    httpd = ThreadingHTTPServer(server_address, LinuxScpGuiHandler)
    print(f"LinuxSCP Commander GUI running at http://localhost:{port}/")
    print(f"Serving web client from: {WEB_DIR}")
    print(f"LinuxSCP core binary: {LINUXSCP_BIN}")
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping LinuxSCP GUI server...")
        httpd.server_close()

if __name__ == "__main__":
    import multiprocessing
    multiprocessing.freeze_support()
    main()
