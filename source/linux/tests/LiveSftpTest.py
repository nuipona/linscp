"""Live tests against the throwaway SFTP server in this directory.

ScriptTest.py deliberately needs no server, so it cannot see what a real
transfer does. This file covers the parts that only show up against one: a
refused open, operations the server denies, an interrupted transfer and a
resume across a connection that died half way through.

The server is TestSftpServer.py, started per test on a port the kernel picks,
so a run cannot collide with a server somebody else is using. paramiko is the
only external requirement and it is optional: without it the whole module
skips, loudly, rather than pretending to pass.

A test that cannot run says so per test instead of taking the whole suite
with it, because a run that silently does less is worth more than one that
does nothing.
"""
import base64
import hashlib
import importlib.util
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())
SUITE_NAME = "live-sftp"
HERE = pathlib.Path(__file__).resolve().parent

sys.path.insert(0, str(HERE))
if importlib.util.find_spec("paramiko") is not None:
    import TestSftpServer as server_module
else:
    server_module = None

USER = "testuser"
PASSWORD = "testpass"


class ProxyLines(list):
    """Reports are emitted by the proxy and read on a different thread.

    Waiting on the condition is deterministic: the client may return before
    the reader has drained a REJECT or AUTH line, especially on a loaded CI
    host. Reading an unsynchronized list after subprocess.run is racy.
    """
    def __init__(self):
        super().__init__()
        self.updated = threading.Condition()

    def report(self, text):
        with self.updated:
            self.append(text)
            self.updated.notify_all()

    def contains(self, predicate, timeout=3):
        with self.updated:
            return self.updated.wait_for(lambda: any(predicate(line) for line in self),
                                         timeout=timeout)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class LiveSftpTests(unittest.TestCase):
    """Requires paramiko, and says so when it is missing."""

    @classmethod
    def setUpClass(cls):
        if server_module is None:
            raise unittest.SkipTest(
                "paramiko is not installed; the live SFTP tests are skipped. "
                "Install it with 'pip install paramiko' to run them.")
        cls.workdir = tempfile.TemporaryDirectory(prefix="linuxscp-live-")
        cls.root = pathlib.Path(cls.workdir.name) / "jail"
        cls.root.mkdir()
        cls.local = pathlib.Path(cls.workdir.name) / "local"
        cls.local.mkdir()
        cls.config = pathlib.Path(cls.workdir.name) / "config"
        cls.config.mkdir()
        cls.addClassCleanup(cls.workdir.cleanup)

    def setUp(self):
        self.proxies = []
        self.start_server()

    def tearDown(self):
        self.stop_server()
        while self.proxies:
            self.stop_proxy(*self.proxies.pop())

    # -- the server ---------------------------------------------------------
    def start_server(self, copy_data=False, check_file_name=False):
        env = dict(os.environ, LSCP_TEST_ROOT=str(self.root), LSCP_TEST_PORT="0",
                   LSCP_TEST_COPY_DATA="1" if copy_data else "0",
                   LSCP_TEST_CHECK_FILE_NAME="1" if check_file_name else "0")
        # Its own process group, so that tearDown can stop the server and
        # anything it left behind without touching the test runner.
        popen_args = dict(env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          start_new_session=True)
        self.server = subprocess.Popen(
            [sys.executable, str(HERE / "TestSftpServer.py")], **popen_args)
        self.port = None
        timer = threading.Timer(30.0, self.server.kill)
        timer.start()
        try:
            for line in self.server.stdout:
                if line.startswith("READY"):
                    self.port = int(line.split()[1])
                    break
        finally:
            timer.cancel()
        if self.port is None:
            self.stop_server()
            self.fail("the test SFTP server did not start")

    def stop_server(self, force=False):
        server = getattr(self, "server", None)
        if server is not None:
            if server.poll() is None:
                if force:
                    server.kill()
                else:
                    # The whole process group: the server may have children.
                    os.killpg(os.getpgid(server.pid), signal.SIGTERM)
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=10)
            if server.stdout is not None:
                server.stdout.close()
        self.server = None
        self.port = None

    # -- the proxy ----------------------------------------------------------
    def start_proxy(self, kind="socks5", credentials=""):
        """Start TestProxyServer.py and collect what it reports.

        The proxy prints one line per request, which is the only way to tell
        a session that went through it from one that quietly went around it:
        a successful transfer proves nothing about the path it took.
        """
        env = dict(os.environ, LSCP_TEST_PORT="0", LSCP_TEST_PROXY_KIND=kind,
                   LSCP_TEST_PROXY_AUTH=credentials)
        popen_args = dict(env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          start_new_session=True)
        proxy = subprocess.Popen(
            [sys.executable, str(HERE / "TestProxyServer.py")], **popen_args)
        lines = ProxyLines()
        port = []
        ready = threading.Event()

        def collect():
            for line in proxy.stdout:
                line = line.strip()
                if not line:
                    continue
                lines.report(line)
                if line.startswith("READY ") and not port:
                    port.append(int(line.split()[1]))
                    ready.set()

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        if not ready.wait(30.0):
            self.stop_proxy(proxy, reader)
            self.fail("the test proxy did not start")
        self.proxies.append((proxy, reader))
        return port[0], lines

    def stop_proxy(self, proxy, reader):
        if proxy.poll() is None:
            try:
                os.killpg(os.getpgid(proxy.pid), signal.SIGTERM)
            except (OSError, ProcessLookupError):
                proxy.terminate()
            try:
                proxy.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proxy.kill()
                proxy.wait(timeout=10)
        if proxy.stdout is not None:
            proxy.stdout.close()
        reader.join(timeout=5)

    # -- the client ---------------------------------------------------------
    def run_script(self, commands, extra_args=(), timeout=60, open_args=()):
        """Run one script. 'extra_args' are switches of the client itself,
        'open_args' switches of the 'open' command it starts with - the
        difference matters, because a session setting such as a proxy belongs
        to 'open' and not to the process."""
        argv = [BINARY, "-batch", "-hostkey=*",
                "-log=" + str(self.config / "session.log")]
        argv += list(extra_args)
        argv.append("-command=open sftp://%s:%s@127.0.0.1:%d/ %s ; %s" %
                    (USER, PASSWORD, self.port, " ".join(open_args), commands))
        env = dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config))
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout, env=env)

    def start_detached(self, commands, extra_args=()):
        """Run a script without waiting for it, so that it can be interrupted."""
        argv = [BINARY, "-batch", "-hostkey=*"]
        argv += list(extra_args)
        argv.append("-command=open sftp://%s:%s@127.0.0.1:%d/ ; %s" %
                    (USER, PASSWORD, self.port, commands))
        env = dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config))
        return subprocess.Popen(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, env=env)

    def write_in_jail(self, name, size):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as handle:
            block = os.urandom(1 << 16)
            written = 0
            while written < size:
                chunk = block[:min(len(block), size - written)]
                handle.write(chunk)
                written += len(chunk)
        return path

    # -- tests --------------------------------------------------------------
    def test_cp_multiple_sources_and_recursive_tree(self):
        """The copy-data extension is optional: SFTP copies must still work
        through the streamed fallback for multiple files and whole trees."""
        first = self.write_in_jail("copy-input/first.bin", 8193)
        second = self.write_in_jail("copy-input/second.bin", 12291)
        nested = self.write_in_jail("copy-input/sub/nested.bin", 4097)
        copies = [self.local / "first-copy.bin",
                  self.local / "second-copy.bin",
                  self.local / "nested-copy.bin"]
        result = self.run_script(
            "option confirm off ; mkdir /many ; "
            "cp /copy-input/first.bin /copy-input/second.bin /many ; "
            "cp /copy-input /whole-copy ; "
            "get /many/first.bin %s ; get /many/second.bin %s ; "
            "get /whole-copy/sub/nested.bin %s" % tuple(copies))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sha256_of(first), sha256_of(copies[0]))
        self.assertEqual(sha256_of(second), sha256_of(copies[1]))
        self.assertEqual(sha256_of(nested), sha256_of(copies[2]))
        self.assertIn("copying through this client", result.stdout)

    def test_cp_respects_batch_overwrite_confirmation(self):
        source = self.write_in_jail("copy-overwrite-source.txt", 4096)
        target = self.write_in_jail("copy-overwrite-target.txt", 31)
        original_target = sha256_of(target)
        result = self.run_script(
            "cp /copy-overwrite-source.txt /copy-overwrite-target.txt")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Cannot confirm this action", result.stderr)
        self.assertEqual(sha256_of(target), original_target)

        result = self.run_script(
            "option confirm off ; cp /copy-overwrite-source.txt "
            "/copy-overwrite-target.txt ; get /copy-overwrite-target.txt %s" %
            (self.local / "copy-overwrite-result.txt"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sha256_of(source),
                         sha256_of(self.local / "copy-overwrite-result.txt"))

    def test_cp_uses_native_copy_data_when_the_server_advertises_it(self):
        source = self.write_in_jail("native-copy-source.bin", 131_117)
        downloaded = self.local / "native-copy-result.bin"
        self.stop_server()
        self.start_server(copy_data=True)
        result = self.run_script(
            "option confirm off ; cp /native-copy-source.bin "
            "/native-copy-target.bin ; get /native-copy-target.bin %s" %
            downloaded)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("on the server", result.stdout)
        self.assertEqual(sha256_of(source), sha256_of(downloaded))

    def test_chmod_symbolic_mode_recurses_over_directories_and_files(self):
        """A recursive symbolic mode has to apply to every directory and
        file in the remote subtree, not just the requested top-level path."""
        root = self.root / "chmod-tree"
        nested_dir = root / "nested"
        nested_dir.mkdir(parents=True)
        nested_file = nested_dir / "data.txt"
        nested_file.write_bytes(b"mode test" + bytes([10]))
        os.chmod(root, 0o755)
        os.chmod(nested_dir, 0o750)
        os.chmod(nested_file, 0o640)

        result = self.run_script("chmod -recursive g+w /chmod-tree")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(root.stat().st_mode & 0o777, 0o775)
        self.assertEqual(nested_dir.stat().st_mode & 0o777, 0o770)
        self.assertEqual(nested_file.stat().st_mode & 0o777, 0o660)

    def test_copyid_installs_public_key_once_with_private_permissions(self):
        """Install a public key into the test server's SFTP home, creating
        ~/.ssh and authorized_keys with the expected modes; a repeat is
        idempotent."""
        key = server_module.paramiko.RSAKey.generate(2048)
        public_file = self.local / "copyid.pub"
        public_file.write_text("%s %s live-copyid-test" %
                               (key.get_name(), key.get_base64()) + "\n")

        result = self.run_script("copyid %s" % public_file)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ssh_dir = self.root / ".ssh"
        authorized_keys = ssh_dir / "authorized_keys"
        self.assertEqual(ssh_dir.stat().st_mode & 0o777, 0o700)
        self.assertEqual(authorized_keys.stat().st_mode & 0o777, 0o600)
        lines = authorized_keys.read_text().splitlines()
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith(key.get_name() + " " +
                                            key.get_base64()), lines[0])

        result = self.run_script("copyid %s" % public_file)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(authorized_keys.read_text().splitlines(), lines)

    def test_open_with_a_wrong_password_is_refused(self):
        argv = [BINARY, "-batch", "-hostkey=*",
                "-command=open sftp://%s:not-the-password@127.0.0.1:%d/" %
                (USER, self.port)]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config)))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Cannot open session", result.stderr)
        self.assertNotIn("Connected", result.stdout)

    def test_open_with_an_unexpected_host_key_is_refused(self):
        """A pinned key that does not match has to stop the connection, and
        say that it was the key rather than the network."""
        result = self.run_script("echo SHOULD_NOT_RUN", extra_args=[
            "-hostkey=ssh-ed25519 256 SHA256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)
        combined = (result.stdout + result.stderr).lower()
        self.assertIn("host key", combined)

    def test_operations_the_server_refuses_are_reported(self):
        """A denied transfer or removal has to fail the command, not quietly
        produce nothing."""
        if os.geteuid() == 0:
            self.skipTest("running as root, so write protection does not apply")
        locked = self.root / "locked"
        locked.mkdir()
        (locked / "keep.txt").write_text("do not remove me\n")
        (self.local / "upload.txt").write_text("payload\n")
        os.chmod(locked, 0o500)
        try:
            result = self.run_script("put -neweronly %s locked/upload.txt" %
                          (self.local / "upload.txt"))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse((locked / "upload.txt").exists())

            result = self.run_script("rm locked/keep.txt")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue((locked / "keep.txt").exists())

            result = self.run_script("mkdir locked/sub")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse((locked / "sub").exists())
        finally:
            os.chmod(locked, 0o700)

    def test_interrupted_get_stages_in_filepart_and_resumes(self):
        """Ctrl-C in the middle of a download must leave the partial bytes
        in '<name>.filepart' - the final name is written only when the file
        is complete - and a retry must finish it byte for byte without
        ever touching the source."""
        source = self.write_in_jail("big.bin", 6 << 20)
        digest = sha256_of(source)
        target = self.local / "big.bin"
        part = self.local / "big.bin.filepart"
        client = self.start_detached("get -speed=512 /big.bin %s" % target)
        deadline = time.time() + 60
        while time.time() < deadline:
            if part.exists() and part.stat().st_size >= (1 << 20):
                break
            if client.poll() is not None:
                self.fail("the download ended before it could be interrupted: " +
                          client.stderr.read())
            time.sleep(0.05)
        else:
            self.fail("the download never got far enough to interrupt")
        # The safety property: nothing under the final name until it is whole.
        self.assertFalse(target.exists())
        client.send_signal(signal.SIGINT)
        # Drain the pipes rather than just waiting: an interrupt has to be
        # reported, and the child has to be reaped with its output read.
        self.assertNotEqual(client.wait(timeout=60), 0)
        client.communicate(timeout=60)
        self.assertTrue(part.exists())
        self.assertFalse(target.exists())
        partial = part.stat().st_size
        self.assertLess(partial, source.stat().st_size)
        self.assertEqual(sha256_of(source), digest)

        # The default 'option resume automatic' continues the staging file.
        result = self.run_script("get /big.bin %s" % target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(part.exists())
        self.assertEqual(target.stat().st_size, source.stat().st_size)
        self.assertEqual(sha256_of(target), digest)

    def test_option_resume_off_transfers_without_a_staging_file(self):
        """'option resume off' is upstream's rsOff: the file is written
        under its final name directly, staging file or not."""
        source = self.write_in_jail("nostage.bin", 200 << 10)
        target = self.local / "nostage.bin"
        part = self.local / "nostage.bin.filepart"
        client = self.start_detached(
            "option resume off ; get -speed=1024 /nostage.bin %s" % target)
        deadline = time.time() + 60
        while time.time() < deadline:
            if target.exists() and target.stat().st_size >= (32 << 10):
                break
            if client.poll() is not None:
                self.fail("the download ended before it could be interrupted: " +
                          client.stderr.read())
            time.sleep(0.05)
        else:
            self.fail("the download never got far enough to interrupt")
        client.send_signal(signal.SIGINT)
        self.assertNotEqual(client.wait(timeout=60), 0)
        client.communicate(timeout=60)
        self.assertFalse(part.exists())
        self.assertTrue(target.exists())

    def test_staged_resume_continues_the_staging_file_not_the_old_final(self):
        """The complete file already at the destination must be replaced
        whole, never spliced with the tail of a new download: only the
        staging file supplies the offset."""
        source = self.write_in_jail("fresh.bin", 2 << 20)
        digest = sha256_of(source)
        target = self.local / "fresh.bin"
        part = self.local / "fresh.bin.filepart"
        target.write_bytes(b"OLD CONTENT OF A COMPLETE FILE\n" * 4096)
        with open(source, "rb") as src, open(part, "wb") as dst:
            dst.write(src.read(512 << 10))
        # A continuation needs no confirmation; a fresh overwrite would.
        result = self.run_script("get /fresh.bin %s" % target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(part.exists())
        self.assertEqual(target.stat().st_size, source.stat().st_size)
        self.assertEqual(sha256_of(target), digest)

    def test_stale_filepart_bigger_than_the_source_is_replaced(self):
        """A staging file that cannot have come from this source (it is
        bigger than the source) is dropped, not resumed."""
        source = self.write_in_jail("smallfresh.bin", 200 << 10)
        digest = sha256_of(source)
        target = self.local / "smallfresh.bin"
        part = self.local / "smallfresh.bin.filepart"
        part.write_bytes(b"\x5a" * (300 << 10))
        result = self.run_script("get /smallfresh.bin %s" % target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(part.exists())
        self.assertEqual(sha256_of(target), digest)

    def test_interrupted_put_stages_in_filepart_and_resumes(self):
        """The upload direction stages too: an interrupted 'put' leaves
        the remote final name alone and the retry continues the remote
        staging file."""
        local = self.local / "upload-big.bin"
        block = os.urandom(1 << 16)
        with open(local, "wb") as handle:
            for _ in range(64):  # 4 MiB
                handle.write(block)
        digest = sha256_of(local)
        remote = self.root / "upload-big.bin"
        remote_part = self.root / "upload-big.bin.filepart"
        client = self.start_detached(
            "put -speed=512 %s /upload-big.bin" % local)
        deadline = time.time() + 60
        while time.time() < deadline:
            if remote_part.exists() and remote_part.stat().st_size >= (1 << 20):
                break
            if client.poll() is not None:
                self.fail("the upload ended before it could be interrupted: " +
                          client.stderr.read())
            time.sleep(0.05)
        else:
            self.fail("the upload never got far enough to interrupt")
        self.assertFalse(remote.exists())
        client.send_signal(signal.SIGINT)
        self.assertNotEqual(client.wait(timeout=60), 0)
        client.communicate(timeout=60)
        self.assertTrue(remote_part.exists())
        self.assertFalse(remote.exists())
        self.assertLess(remote_part.stat().st_size, local.stat().st_size)

        result = self.run_script("put %s /upload-big.bin" % local)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(remote_part.exists())
        self.assertEqual(remote.stat().st_size, local.stat().st_size)
        self.assertEqual(sha256_of(remote), digest)

    def test_resume_across_a_connection_that_died(self):
        """The connection going away mid-transfer is the case '-resume' exists
        for: the retry has to finish the file, not restart it or splice it,
        and the bytes arrive in the staging file as upstream does it."""
        source = self.write_in_jail("dying.bin", 8 << 20)
        digest = sha256_of(source)
        target = self.local / "dying.bin"
        part = self.local / "dying.bin.filepart"
        client = self.start_detached("get -speed=256 /dying.bin %s" % target)
        deadline = time.time() + 60
        while time.time() < deadline:
            if part.exists() and part.stat().st_size >= (1 << 20):
                break
            if client.poll() is not None:
                break
            time.sleep(0.05)
        # The server dies under the transfer, which is not a graceful close.
        self.stop_server(force=True)
        result = client.communicate(timeout=90)
        self.assertNotEqual(client.returncode, 0)
        partial = part.stat().st_size
        self.assertGreater(partial, 0)
        self.assertLess(partial, source.stat().st_size)
        self.assertFalse(target.exists())
        self.assertIn("failed", (result[0] + result[1]).lower())

        # The port and the process are replaced together; the next 'open'
        # has to reach the new one.
        self.stop_server()
        self.start_server()
        result = self.run_script("get -resume /dying.bin %s" % target, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(part.exists())
        self.assertEqual(target.stat().st_size, source.stat().st_size)
        self.assertEqual(sha256_of(target), digest)

    # -- stored passwords, shared with the GUI ------------------------------
    def clean_sessions(self):
        """This config directory is shared by the class; a session file
        left behind by one test must not decide the next one's outcome."""
        for name in ("sessions.ini", "sessions.key"):
            candidate = self.config / name
            if candidate.exists():
                candidate.unlink()

    def run_stored(self, command, timeout=60):
        """Run a script that is NOT prefaced with an open-by-URL, so a
        stored session's own credentials are what gets used."""
        argv = [BINARY, "-batch", "-hostkey=*",
                "-command=" + command]
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout,
                              env=dict(os.environ,
                                       LINUXSCP_CONFIG_DIR=str(self.config)))

    def gui_crypto(self):
        """The graphical client's own reader for sessions.ini."""
        gui_dir = HERE.parents[1] / "gui"
        if str(gui_dir) not in sys.path:
            sys.path.insert(0, str(gui_dir))
        import linscp_crypto
        return linscp_crypto

    def test_password_encrypted_by_python_opens_the_core(self):
        """The whole point of the shared format: a password encrypted by
        the GUI's module must authenticate the console client."""
        linscp_crypto = self.gui_crypto()
        self.clean_sessions()
        try:
            key = os.urandom(32)
            (self.config / "sessions.key").write_text(
                base64.b64encode(key).decode("ascii") + "\n")
            (self.config / "sessions.ini").write_text(
                "[vault]\n"
                "protocol=sftp\n"
                "host=127.0.0.1\n"
                "port=%d\n"
                "username=%s\n"
                "password=%s\n" %
                (self.port, USER, linscp_crypto.encrypt(PASSWORD, key)),
                encoding="utf-8")

            result = self.run_stored("open vault ; ls ; echo OPENED")
            self.assertEqual(result.returncode, 0,
                             result.stdout + result.stderr)
            self.assertIn("OPENED", result.stdout)

            # Listing the sessions never prints the password either.
            result = self.run_stored("sessions")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(PASSWORD, result.stdout + result.stderr)
        finally:
            self.clean_sessions()

    def test_missing_key_file_is_reported_and_the_password_preserved(self):
        """An undecryptable password stops nobody who has a key, says so
        loudly, and survives a re-save that cannot read it."""
        linscp_crypto = self.gui_crypto()
        self.clean_sessions()
        try:
            key = os.urandom(32)
            key_bytes = base64.b64encode(key).decode("ascii") + "\n"
            (self.config / "sessions.key").write_text(key_bytes)
            (self.config / "sessions.ini").write_text(
                "[vault]\n"
                "protocol=sftp\n"
                "host=127.0.0.1\n"
                "port=%d\n"
                "username=%s\n"
                "password=%s\n" %
                (self.port, USER, linscp_crypto.encrypt(PASSWORD, key)),
                encoding="utf-8")
            before = (self.config / "sessions.ini").read_text(encoding="utf-8")
            (self.config / "sessions.key").unlink()

            result = self.run_stored("open vault")
            combined = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, combined)
            self.assertIn("cannot be used", combined)
            self.assertIn("sessions.key", combined)
            # The file itself was not rewritten on the way to failure.
            self.assertEqual(
                (self.config / "sessions.ini").read_text(encoding="utf-8"),
                before)

            # An unrelated re-save must keep the value it cannot read.
            result = self.run_stored(
                "savesession vault -host=127.0.0.1 -port=%d -username=%s" %
                (self.port, USER))
            self.assertEqual(result.returncode, 0, result.stderr)
            text = (self.config / "sessions.ini").read_text(encoding="utf-8")
            blob = None
            for line in before.splitlines():
                if line.startswith("password="):
                    blob = line.split("=", 1)[1]
            self.assertTrue(blob and blob.startswith("lscp1$"), before)
            self.assertIn("password=" + blob, text)

            # Key restored: the same session opens, by decryption.
            (self.config / "sessions.key").write_text(key_bytes)
            result = self.run_stored("open vault ; ls ; echo STILL_GOT_IT")
            self.assertEqual(result.returncode, 0,
                             result.stdout + result.stderr)
            self.assertIn("STILL_GOT_IT", result.stdout)
        finally:
            self.clean_sessions()

    def test_password_stored_by_the_core_decrypts_in_python(self):
        """And the other direction: what the console client encrypts has
        to come back out through the GUI's reader, password and all."""
        linscp_crypto = self.gui_crypto()
        gui_dir = HERE.parents[1] / "gui"
        if str(gui_dir) not in sys.path:
            sys.path.insert(0, str(gui_dir))
        self.clean_sessions()
        try:
            result = self.run_stored(
                "savesession katsite -host=127.0.0.1 -port=%d -username=%s "
                "-password=%s" % (self.port, USER, PASSWORD))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("stored encrypted", result.stdout)

            text = (self.config / "sessions.ini").read_text(encoding="utf-8")
            self.assertNotIn(PASSWORD, text)
            blob = None
            for line in text.splitlines():
                if line.startswith("password="):
                    blob = line.split("=", 1)[1]
            self.assertTrue(blob and blob.startswith("lscp1$"), text)

            key = linscp_crypto.load_key(
                linscp_crypto.key_path_for(str(self.config / "sessions.ini")))
            self.assertEqual(linscp_crypto.decrypt(blob, key), PASSWORD)

            # The GUI's own session reader (the one the site list uses)
            # reports it as usable, not locked.
            import linuxscp_gui
            old = os.environ.get("LINUXSCP_CONFIG_DIR")
            os.environ["LINUXSCP_CONFIG_DIR"] = str(self.config)
            try:
                sessions = linuxscp_gui.read_stored_sessions()
            finally:
                if old is None:
                    os.environ.pop("LINUXSCP_CONFIG_DIR", None)
                else:
                    os.environ["LINUXSCP_CONFIG_DIR"] = old
            match = [s for s in sessions if s["name"] == "katsite"]
            self.assertEqual(len(match), 1, sessions)
            self.assertEqual(match[0]["password"], PASSWORD)
            self.assertFalse(match[0].get("password_locked"))
        finally:
            self.clean_sessions()

    def test_timestamps_survive_in_both_directions(self):
        """'-preservetime' means the server time and the local time agree
        after a transfer, whichever way the bytes went - which only a real
        server can confirm."""
        when = 1234567890
        remote = self.write_in_jail("stamp.txt", 64)
        os.utime(remote, (when, when))
        target = self.local / "stamp.txt"
        result = self.run_script("get -preservetime /stamp.txt %s" % target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(int(target.stat().st_mtime), when)

        upload = self.local / "up.txt"
        upload.write_text("payload\n")
        os.utime(upload, (when, when))
        result = self.run_script("put -preservetime %s /up.txt" % upload)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(int((self.root / "up.txt").stat().st_mtime), when)


    def test_checksum_fallback_supports_all_documented_algorithms(self):
        """When the server lacks the extension, every documented checksum
        algorithm hashes the downloaded bytes and reports the correct digest."""
        data = (b"checksum fallback" + bytes([0])) * 4097
        self.write_in_jail("fallback-checksum.bin", len(data)).write_bytes(data)
        expected = {
            "sha-256": hashlib.sha256(data).hexdigest(),
            "sha-1": hashlib.sha1(data).hexdigest(),
            "md5": hashlib.md5(data).hexdigest(),
            "crc32": "%08x" % (zlib.crc32(data) & 0xffffffff),
        }
        for algorithm, digest in expected.items():
            with self.subTest(algorithm=algorithm):
                result = self.run_script(
                    "checksum %s /fallback-checksum.bin" % algorithm)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                self.assertIn(digest + " /fallback-checksum.bin",
                              result.stdout)
                self.assertIn("computed from the downloaded file",
                              result.stdout)

    def test_checksum_uses_native_check_file_name_extension(self):
        """A server implementing check-file-name should calculate the digest
        itself rather than forcing the client's download-and-hash fallback."""
        self.stop_server()
        self.start_server(check_file_name=True)
        remote = self.write_in_jail("native-checksum.bin", 97 << 10)
        expected = sha256_of(remote)

        result = self.run_script("checksum sha-256 /native-checksum.bin")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected + " /native-checksum.bin", result.stdout)
        self.assertNotIn("cannot calculate checksums", result.stdout)
        self.assertNotIn("downloaded file", result.stdout)

    def test_synchronize_filemask_prunes_excluded_subtrees(self):
        """A path exclusion prunes its subtree in both the transfer and
        delete phases, leaving excluded destination files untouched."""
        local_root = self.local / "sync-exclusion"
        (local_root / "skip").mkdir(parents=True)
        keep = b"included by synchronization" + bytes([10])
        excluded = b"excluded source" + bytes([10])
        (local_root / "keep.txt").write_bytes(keep)
        (local_root / "skip" / "new.txt").write_bytes(excluded)

        remote_preserved = self.write_in_jail(
            "sync-exclusion/skip/old.txt", 0)
        remote_preserved.write_bytes(b"must survive -delete" + bytes([10]))
        preview = self.run_script(
            "synchronize remote -filemask=|skip/ -delete -preview %s "
            "/sync-exclusion" % local_root)
        self.assertEqual(preview.returncode, 0,
                         preview.stdout + preview.stderr)
        self.assertIn("keep.txt", preview.stdout)
        self.assertNotIn("new.txt", preview.stdout)
        self.assertNotIn("old.txt", preview.stdout)

        result = self.run_script(
            "option confirm off ; synchronize remote -filemask=|skip/ "
            "-delete %s /sync-exclusion" % local_root)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "sync-exclusion/keep.txt").read_bytes(),
                         keep)
        self.assertEqual(remote_preserved.read_bytes(),
                         b"must survive -delete" + bytes([10]))
        self.assertFalse((self.root /
                          "sync-exclusion/skip/new.txt").exists())

    def test_synchronize_by_checksum_sees_what_the_timestamp_hides(self):
        """A file whose contents changed without its size or its timestamp
        changing is invisible to the default criteria and visible to
        '-criteria=checksum': the preview lists it and the run brings the
        local copy back in line. Only a real server can show this, because
        the remote checksum comes from the server or, when it has no
        checksum extension, from a downloaded copy of the file."""
        (self.root / "hidden").mkdir()
        remote = self.write_in_jail("hidden/data.bin", 8 << 10)
        digest = sha256_of(remote)

        local_dir = self.local / "hidden"
        local_dir.mkdir()
        local_copy = local_dir / "data.bin"
        local_copy.write_bytes(remote.read_bytes())
        # The same size and the same timestamp, different contents: exactly
        # what a time or size comparison cannot see.
        with open(local_copy, "r+b") as handle:
            handle.write(b"\0" * 32)
        stamp = remote.stat().st_mtime
        os.utime(local_copy, (stamp, stamp))
        self.assertNotEqual(sha256_of(local_copy), digest)

        # The timestamp criteria leave the file alone.
        result = self.run_script(
            "synchronize local -criteria=time -preview %s /hidden" % local_dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("criteria=time", result.stdout)
        self.assertNotIn("download\tdata.bin", result.stdout)

        # The checksum sees it, and says which comparison found it.
        result = self.run_script(
            "synchronize local -criteria=checksum -preview %s /hidden" %
            local_dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("criteria=checksum", result.stdout)
        self.assertIn("download\tdata.bin", result.stdout)
        self.assertIn("contents differ", result.stdout)

        # A list of criteria works the same way, and running the plan makes
        # the two sides equal again.
        result = self.run_script(
            "synchronize local -criteria=time,size,checksum %s /hidden" %
            local_dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sha256_of(local_copy), digest)

        # With the contents equal again, nothing is left to do.
        result = self.run_script(
            "synchronize local -criteria=checksum -preview %s /hidden" %
            local_dir)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("download\tdata.bin", result.stdout)

    def test_a_socks5_proxy_carries_the_session(self):
        """'-rawsettings ProxyMethod=SOCKS5 ...' has to change the path the
        connection takes, not just be accepted: the proxy must see the
        destination, and a transfer through it must still be byte for byte."""
        proxy_port, proxy_lines = self.start_proxy()
        source = self.write_in_jail("through-proxy.bin", 256 << 10)
        digest = sha256_of(source)
        target = self.local / "through-proxy.bin"
        result = self.run_script(
            "get /through-proxy.bin %s" % target,
            open_args=["-rawsettings", "ProxyMethod=SOCKS5",
                       "ProxyHost=127.0.0.1", "ProxyPort=%d" % proxy_port,
                       # Upstream leaves a localhost destination alone
                       # unless the session asks for it explicitly.
                       "ProxyLocalhost=1"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sha256_of(target), digest)
        self.assertTrue(proxy_lines.contains(
            lambda line: line == "CONNECT 127.0.0.1:%d" % self.port),
            "the session did not go through the proxy: %s" % (proxy_lines,))

    def test_proxy_credentials_are_used_and_a_wrong_one_is_refused(self):
        """A proxy that authenticates has to be given the credentials, and a
        wrong password has to stop the session with the proxy named rather
        than fall back to a direct connection."""
        proxy_port, proxy_lines = self.start_proxy(
            credentials="proxyuser:proxypass")
        settings = ["-rawsettings", "ProxyMethod=SOCKS5",
                    "ProxyHost=127.0.0.1", "ProxyPort=%d" % proxy_port,
                    "ProxyLocalhost=1", "ProxyUsername=proxyuser"]

        result = self.run_script("pwd", open_args=settings +
                                 ["ProxyPassword=nonsense"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        combined = result.stdout + result.stderr
        self.assertIn("Cannot open session", combined)
        self.assertNotIn("Connected", result.stdout)
        self.assertTrue(proxy_lines.contains(lambda line: line.startswith("REJECT")),
                        proxy_lines)

        del proxy_lines[:]
        result = self.run_script("pwd", open_args=settings +
                                 ["ProxyPassword=proxypass"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(proxy_lines.contains(lambda line: line == "AUTH proxyuser"),
                        proxy_lines)
        self.assertTrue(proxy_lines.contains(
            lambda line: line == "CONNECT 127.0.0.1:%d" % self.port), proxy_lines)

    def test_a_stored_session_keeps_its_proxy(self):
        """'savesession -rawsettings' stores the proxy encrypted, 'open'
        brings it back, and the stored site still goes through it - which is
        what makes a proxy a session setting rather than a switch."""
        proxy_port, proxy_lines = self.start_proxy()
        store = self.config / "sessions.ini"
        self.clean_sessions()
        try:
            save = subprocess.run(
                [BINARY, "-batch",
                 "-command=savesession proxied -host=127.0.0.1 -port=%d "
                           "-username=%s -protocol=sftp -hostkey=* "
                           "-password=%s -rawsettings ProxyMethod=SOCKS5 "
                           "ProxyHost=127.0.0.1 ProxyPort=%d ProxyLocalhost=1"
                 % (self.port, USER, PASSWORD, proxy_port)],
                capture_output=True, text=True, timeout=60,
                env=dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config)))
            self.assertEqual(save.returncode, 0, save.stdout + save.stderr)
            text = store.read_text()
            self.assertIn("proxy_method=SOCKS5", text)
            self.assertIn("proxy_port=%d" % proxy_port, text)
            self.assertNotIn("proxypass", text)

            result = subprocess.run(
                [BINARY, "-batch", "-command=open proxied ; pwd ; exit"],
                capture_output=True, text=True, timeout=60,
                env=dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config)))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(proxy_lines.contains(
                lambda line: line == "CONNECT 127.0.0.1:%d" % self.port),
                proxy_lines)
        finally:
            self.clean_sessions()


def run_and_report():
    """Run the suite, then print one line that says what was actually run.

    'make check' and CI are green whether or not the tests could run, so a
    missing dependency used to be invisible: this line, which the CI jobs
    repeat as an annotation, records the skip count next to the pass count.
    """
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    line = ("evidence: suite=%s platform=linux ran=%d skipped=%d failed=%d" %
            (SUITE_NAME, result.testsRun, len(result.skipped),
             len(result.failures) + len(result.errors)))
    if not result.testsRun and result.skipped:
        # A suite that ran nothing is not a pass, and 'skipped' alone does not
        # say whether that is a platform limit or a missing dependency.
        reasons = "; ".join(sorted(set(str(reason)[:160]
                                       for _, reason in result.skipped)))
        line += " because: " + reasons
    print(line)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run_and_report())
