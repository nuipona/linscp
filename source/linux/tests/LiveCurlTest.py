"""Live tests for the curl-backed FTP, FTPS and SCP protocols.

The generic curl path is deliberately separate from the native SFTP suite:
FTP/FTPS use a throwaway pyftpdlib server (the dependency is optional), while
SCP uses a throwaway OpenSSH daemon when sshd is installed. Each test checks
an authenticated download and upload byte-for-byte. The SCP test also checks
the fail-closed upload-resume policy: curl's SCP backend has no remote rename,
so the Linux client must not pretend that its .filepart staging is resumable.

The suite is optional in a source-tree build. Missing pyftpdlib/OpenSSL skips
the FTP cases, and missing sshd skips SCP, with the evidence line making the
limitation explicit rather than turning an empty run into a passing claim.

Evidence convention:

    evidence: suite=live-curl platform=linux ran=N skipped=N failed=N
"""

import os
import pathlib
import pwd
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())
SUITE_NAME = "live-curl"

try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
    from pyftpdlib.handlers.ftps.ssl import SSL
    from pyftpdlib.servers import FTPServer
    FTP_IMPORT_ERROR = ""
except ImportError as exc:
    DummyAuthorizer = FTPHandler = TLS_FTPHandler = SSL = FTPServer = None
    FTP_IMPORT_ERROR = "pyftpdlib with pyOpenSSL is not installed: %s" % exc


class ImplicitTLSHandler(TLS_FTPHandler if TLS_FTPHandler is not None else object):
    """pyftpdlib's normal FTPS handler expects AUTH TLS; curl ftps:// is
    implicit TLS, so wrap the accepted control socket before FTP commands."""

    if TLS_FTPHandler is not None:
        def __init__(self, conn, server, ioloop=None):
            wrapped = SSL.Connection(self.get_ssl_context(), conn)
            wrapped.set_accept_state()
            super().__init__(wrapped, server, ioloop)
            if getattr(self, "connected", False):
                self._ssl_requested = True
                self._ssl_accepting = True


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_port(port, process):
    deadline = time.time() + 15
    while time.time() < deadline:
        if process.poll() is not None:
            detail = process.stderr.read() if process.stderr is not None else ""
            raise RuntimeError("sshd exited before listening: " + detail)
        sock = socket.socket()
        sock.settimeout(0.2)
        try:
            sock.connect(("127.0.0.1", port))
            sock.close()
            return
        except OSError:
            sock.close()
            time.sleep(0.05)
    raise RuntimeError("sshd did not listen on port %d" % port)


class LiveCurlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.TemporaryDirectory(prefix="live-curl-")
        cls.root = pathlib.Path(cls.workdir.name) / "remote"
        cls.root.mkdir()
        cls.local = pathlib.Path(cls.workdir.name) / "local"
        cls.local.mkdir()
        cls.config = pathlib.Path(cls.workdir.name) / "config"
        cls.config.mkdir()
        cls.cert = pathlib.Path(cls.workdir.name) / "ftp-cert.pem"
        cls.addClassCleanup(cls.workdir.cleanup)

    def setUp(self):
        self.ftp_server = None
        self.ftp_thread = None
        self.sshd = None

    def tearDown(self):
        if self.ftp_server is not None:
            self.ftp_server.close_all()
        if self.ftp_thread is not None:
            self.ftp_thread.join(timeout=5)
        if self.sshd is not None and self.sshd.poll() is None:
            try:
                os.killpg(os.getpgid(self.sshd.pid), signal.SIGTERM)
            except (OSError, ProcessLookupError):
                self.sshd.terminate()
            try:
                self.sshd.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.sshd.kill()
                self.sshd.wait()
        self.sshd = None

    def start_ftp(self, tls=False):
        if FTP_IMPORT_ERROR:
            self.skipTest(FTP_IMPORT_ERROR)
        authorizer = DummyAuthorizer()
        authorizer.add_user("test", "test", str(self.root), perm="elradfmwMT")
        if tls:
            if shutil.which("openssl") is None:
                self.skipTest("openssl is not installed; cannot create the FTPS certificate")
            if not self.cert.exists():
                result = subprocess.run(
                    ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                     "-keyout", str(self.cert), "-out", str(self.cert), "-days", "1",
                     "-subj", "/CN=127.0.0.1"],
                    capture_output=True, text=True)
                if result.returncode != 0:
                    self.skipTest("openssl could not create the FTPS certificate: " +
                                  result.stderr.strip())
            base = ImplicitTLSHandler
        else:
            base = FTPHandler

        class Handler(base):
            pass

        Handler.authorizer = authorizer
        # The client uses passive mode by default. A fixed, private range
        # keeps the throwaway endpoint easy to diagnose without exposing it.
        Handler.passive_ports = range(42000, 42011)
        if tls:
            Handler.certfile = str(self.cert)
            Handler.ssl_context = None
            Handler.tls_control_required = True
            Handler.tls_data_required = True

        self.ftp_server = FTPServer(("127.0.0.1", 0), Handler)
        self.ftp_thread = threading.Thread(
            target=self.ftp_server.serve_forever, daemon=True)
        self.ftp_thread.start()
        return self.ftp_server.socket.getsockname()[1]

    def start_sshd(self):
        sshd_binary = shutil.which("sshd")
        if sshd_binary is None:
            for candidate in ("/usr/sbin/sshd", "/usr/bin/sshd"):
                if pathlib.Path(candidate).is_file():
                    sshd_binary = candidate
                    break
        keygen_binary = shutil.which("ssh-keygen")
        curl_binary = shutil.which("curl")
        if sshd_binary is None or keygen_binary is None:
            self.skipTest("sshd and ssh-keygen are required for live SCP coverage")
        if curl_binary is None:
            self.skipTest("curl is not installed; SCP is a curl-backed protocol")
        curl_info = subprocess.run([curl_binary, "--version"],
                                   capture_output=True, text=True)
        if not any(line.startswith("Protocols:") and " scp" in line
                   for line in curl_info.stdout.splitlines()):
            self.skipTest("the installed curl has no SCP protocol support")
        user = pwd.getpwuid(os.getuid()).pw_name
        host_key = pathlib.Path(self.workdir.name) / "host_ed25519"
        user_key = pathlib.Path(self.workdir.name) / "user_ed25519"
        for key in (host_key, user_key):
            result = subprocess.run(
                [keygen_binary, "-q", "-t", "ed25519", "-N", "", "-f", str(key)],
                capture_output=True, text=True)
            if result.returncode != 0:
                self.skipTest("ssh-keygen could not create the SCP test key: " +
                              result.stderr.strip())
        authorized = pathlib.Path(self.workdir.name) / "authorized_keys"
        authorized.write_text(pathlib.Path(str(user_key) + ".pub").read_text())
        authorized.chmod(0o600)
        port = free_port()
        config = pathlib.Path(self.workdir.name) / "sshd_config"
        config.write_text("\n".join([
            "Port %d" % port,
            "ListenAddress 127.0.0.1",
            "HostKey %s" % host_key,
            "AuthorizedKeysFile %s" % authorized,
            "PubkeyAuthentication yes",
            "PasswordAuthentication no",
            "KbdInteractiveAuthentication no",
            "UsePAM no",
            "StrictModes no",
            "PermitRootLogin no",
            "AllowUsers %s" % user,
            "PidFile %s" % (pathlib.Path(self.workdir.name) / "sshd.pid"),
            "LogLevel VERBOSE",
            "",
        ]))
        checked = subprocess.run(
            [sshd_binary, "-t", "-f", str(config)], capture_output=True, text=True)
        if checked.returncode != 0:
            self.skipTest("sshd rejected the SCP test configuration: " +
                          checked.stderr.strip())
        self.sshd = subprocess.Popen(
            [sshd_binary, "-D", "-e", "-f", str(config)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            start_new_session=True)
        wait_for_port(port, self.sshd)
        return port, user_key, user

    def run_client(self, url, commands, extra=()):
        args = [BINARY, "-batch", "-hostkey=*", "-timeout=15"]
        args += list(extra)
        args.append("-command=open %s ; %s" % (url, commands))
        return subprocess.run(
            args, cwd=self.local, capture_output=True, text=True, timeout=90,
            env=dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config)))

    def test_ftp_authenticated_download_and_upload(self):
        port = self.start_ftp()
        payload = b"live FTP download\n" * 8192
        upload = b"live FTP upload\n" * 8192
        (self.root / "remote.txt").write_bytes(payload)
        (self.local / "upload.txt").write_bytes(upload)
        result = self.run_client(
            "ftp://test:test@127.0.0.1:%d/" % port,
            "get /remote.txt download.txt ; put upload.txt /uploaded.txt ; exit")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.local / "download.txt").read_bytes(), payload)
        self.assertEqual((self.root / "uploaded.txt").read_bytes(), upload)

    def test_ftps_authenticated_download_and_upload(self):
        port = self.start_ftp(tls=True)
        payload = b"live FTPS download\n" * 8192
        upload = b"live FTPS upload\n" * 8192
        (self.root / "ftps-remote.txt").write_bytes(payload)
        (self.local / "ftps-upload.txt").write_bytes(upload)
        result = self.run_client(
            "ftps://test:test@127.0.0.1:%d/" % port,
            "get /ftps-remote.txt ftps-download.txt ; "
            "put ftps-upload.txt /ftps-uploaded.txt ; exit")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.local / "ftps-download.txt").read_bytes(), payload)
        self.assertEqual((self.root / "ftps-uploaded.txt").read_bytes(), upload)

    def test_scp_authenticated_download_upload_and_resume_policy(self):
        port, private_key, user = self.start_sshd()
        payload = b"live SCP download\n" * 8192
        upload = b"live SCP upload\n" * 8192
        remote = pathlib.Path(self.workdir.name) / "scp-remote.txt"
        remote.write_bytes(payload)
        local_upload = self.local / "scp-upload.txt"
        local_upload.write_bytes(upload)
        downloaded = self.local / "scp-download.txt"
        uploaded = pathlib.Path(self.workdir.name) / "scp-uploaded.txt"
        extra = ["-privatekey=" + str(private_key)]
        result = self.run_client(
            "scp://%s@127.0.0.1:%d/" % (user, port),
            "get %s %s ; put %s %s ; exit" %
            (remote, downloaded, local_upload, uploaded), extra=extra)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(downloaded.read_bytes(), payload)
        self.assertEqual(uploaded.read_bytes(), upload)

        refused = pathlib.Path(self.workdir.name) / "scp-resume-refused.txt"
        result = self.run_client(
            "scp://%s@127.0.0.1:%d/" % (user, port),
            "put -resume %s %s ; exit" % (local_upload, refused), extra=extra)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot resume", (result.stdout + result.stderr).lower())
        self.assertFalse(refused.exists())


def run_and_report():
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    line = ("evidence: suite=%s platform=linux ran=%d skipped=%d failed=%d" %
            (SUITE_NAME, result.testsRun, len(result.skipped),
             len(result.failures) + len(result.errors)))
    if not result.testsRun and result.skipped:
        reasons = "; ".join(sorted(set(str(reason)[:160]
                                       for _, reason in result.skipped)))
        line += " because: " + reasons
    print(line)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run_and_report())
