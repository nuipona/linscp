#!/usr/bin/env python3
"""A throwaway SFTP + exec server for exercising linuxscp live.

Runs one SSH server on 127.0.0.1:12765, user 'testuser', password 'testpass'.
The SFTP "home" is ROOT (the server process chdir()s there, so relative paths
resolve inside it and canonicalize() makes them absolute). Channel exec
requests run locally (sandbox only) so 'call' works.
"""
import os
import socket
import stat
import subprocess
import sys
import threading

import paramiko
from paramiko.sftp import SFTP_OP_UNSUPPORTED
from paramiko.sftp_attr import SFTPAttributes
from paramiko.sftp import SFTP_OP_UNSUPPORTED  # noqa: F811
from paramiko.sftp_handle import SFTPHandle

HOST, PORT = "127.0.0.1", 12765
USER, PASSWORD = "testuser", "testpass"
ROOT = "/tmp/lscp-sftp-root"

HOST_KEY = paramiko.RSAKey.generate(2048)


def _err(exc_info=None):
    _, e, _ = exc_info or sys.exc_info()
    return paramiko.SFTPServer.convert_errno(e.errno)


class SftpSI(paramiko.SFTPServerInterface):
    """Filesystem-backed SFTP backend with ROOT as the visible '/'."""

    def _map(self, path):
        if path.startswith("/"):
            return ROOT + path
        return path  # relative: resolved against the server cwd (ROOT)

    def canonicalize(self, path):
        real = os.path.realpath(self._map(path))
        if real == ROOT:
            return "/"
        if real.startswith(ROOT + "/"):
            return real[len(ROOT):]
        return "/"  # outside the jail

    def list_folder(self, path):
        try:
            out = []
            for fname in os.listdir(self._map(path)):
                attr = SFTPAttributes.from_stat(
                    os.stat(os.path.join(self._map(path), fname)))
                attr.filename = fname
                out.append(attr)
            return out
        except OSError:
            return _err()

    def stat(self, path):
        try:
            return SFTPAttributes.from_stat(os.stat(self._map(path)))
        except OSError:
            return _err()

    def lstat(self, path):
        try:
            return SFTPAttributes.from_stat(os.lstat(self._map(path)))
        except OSError:
            return _err()

    def open(self, path, flags, attr):
        try:
            fd = os.open(self._map(path), flags)
        except OSError:
            return _err()
        if attr and getattr(attr, "st_mode", None) is not None:
            try:
                os.fchmod(fd, attr.st_mode)
            except OSError:
                pass
        handle = SFTPHandle(fd)
        mode = "r+b"  # both directions; SFTP handles manage offsets
        try:
            handle.readfile = handle.writefile = os.fdopen(fd, mode, 0)
        except OSError:
            return _err()
        return handle

    def remove(self, path):
        try:
            os.remove(self._map(path))
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    def rename(self, oldpath, newpath):
        try:
            os.rename(self._map(oldpath), self._map(newpath))
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    def posix_rename(self, oldpath, newpath):
        return self.rename(oldpath, newpath)

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._map(path))
            if attr and getattr(attr, "st_mode", None) is not None:
                os.chmod(self._map(path), attr.st_mode)
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    def rmdir(self, path):
        try:
            os.rmdir(self._map(path))
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    def symlink(self, target_path, path):
        try:
            os.symlink(self._map(target_path), self._map(path))
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    def readlink(self, path):
        try:
            return os.readlink(self._map(path))
        except OSError:
            return _err()

    def chattr(self, path, attr):
        # paramiko routes SSH_FXP_SETSTAT here (older releases called it
        # 'setstat', which is why the alias below exists).
        try:
            mpath = self._map(path)
            if getattr(attr, "st_mode", None) is not None:
                os.chmod(mpath, attr.st_mode)
            if getattr(attr, "st_size", None) is not None:
                with open(mpath, "r+") as f:
                    f.truncate(attr.st_size)
            times = None
            a = getattr(attr, "st_atime", None)
            m = getattr(attr, "st_mtime", None)
            if a is not None or m is not None:
                st = os.stat(mpath)
                times = (a or st.st_atime, m or st.st_mtime)
            if times:
                os.utime(mpath, times)
            return paramiko.SFTP_OK
        except OSError:
            return _err()

    setstat = chattr  # older paramiko releases


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL if (username == USER and password == PASSWORD) else paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED

    def check_channel_exec_request(self, channel, command):
        def run():
            proc = subprocess.run(command, shell=True, capture_output=True,
                                  cwd=ROOT)
            channel.sendall(proc.stdout)
            channel.send_exit_status(proc.returncode)
            channel.close()
        threading.Thread(target=run, daemon=True).start()
        return True

    def check_channel_shell_request(self, channel):
        return False
    # No subsystem override: paramiko's default starts the handler that
    # was registered with Transport.set_subsystem_handler.


def handle(client):
    transport = paramiko.Transport(client)
    transport.add_server_key(HOST_KEY)
    transport.set_subsystem_handler("sftp", paramiko.SFTPServer, SftpSI)
    server = Server()
    try:
        transport.start_server(server=server)
        channel = transport.accept(60)
        if channel is not None:
            channel.recv_exit_status()  # wait until the channel closes
    except Exception as exc:  # noqa
        print("server error:", exc, file=sys.stderr)
    finally:
        transport.close()


def main():
    os.chdir(ROOT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, PORT))
    sock.listen(16)
    print("READY", flush=True)
    while True:
        client, _ = sock.accept()
        threading.Thread(target=handle, args=(client,), daemon=True).start()


if __name__ == "__main__":
    main()
