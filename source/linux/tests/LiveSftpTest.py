"""Live tests against the throwaway SFTP server in this directory.

ScriptTest.py deliberately needs no server, so it cannot see what a real
transfer does. This file covers the parts that only show up against one: a
refused open, operations the server denies, an interrupted transfer and a
resume across a connection that died half way through.

The server is TestSftpServer.py, started per test on a port the kernel picks,
so a run cannot collide with a server somebody else is using. paramiko is the
only external requirement and it is optional: without it the whole module
skips, loudly, rather than pretending to pass.

What cannot be done from another platform says so per test instead of taking
the whole suite with it, because a run that silently does less is worth more
than one that does nothing. The suite as a whole is POSIX, for a reason worth
knowing: on Windows the client speaks SFTP through curl, and the curl.exe
built into Windows has no SFTP or SCP support, so there is no session for
these tests to open - not a slow test, an impossible one. A Windows install
that puts a curl built with libssh2 on PATH gets these tests back.
"""
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


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def posix_only(reason):
    """Marks a test that only this platform can run, with the reason someone
    reading the skip list of the other one needs."""
    return unittest.skipIf(os.name != "posix", "POSIX only: " + reason)


class LiveSftpTests(unittest.TestCase):
    """Requires paramiko; individual tests additionally require a POSIX
    host, and say which of the two is missing when they skip."""

    @classmethod
    def setUpClass(cls):
        if os.name != "posix":
            # First, because it is the binding reason: paramiko on a Windows
            # box would buy nothing while curl cannot speak the protocol, and
            # a recorded reason that names the wrong blocker is worse than no
            # recorded reason at all.
            raise unittest.SkipTest(
                "SFTP on Windows goes through curl, and the curl.exe built "
                "into Windows has no SFTP support: there is no session for "
                "these tests to open. Put a curl built with libssh2 on PATH "
                "and this restriction goes away.")
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
        self.start_server()

    def tearDown(self):
        self.stop_server()

    # -- the server ---------------------------------------------------------
    def start_server(self):
        env = dict(os.environ, LSCP_TEST_ROOT=str(self.root), LSCP_TEST_PORT="0")
        popen_args = dict(env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
        if os.name == "posix":
            # Its own process group, so that tearDown can stop the server and
            # anything it left behind without touching the test runner.
            popen_args["start_new_session"] = True
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
                if os.name == "posix" and not force:
                    os.killpg(os.getpgid(server.pid), signal.SIGTERM)
                else:
                    (server.kill if force else server.terminate)()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=10)
            if server.stdout is not None:
                server.stdout.close()
        self.server = None
        self.port = None

    # -- the client ---------------------------------------------------------
    def run_script(self, commands, extra_args=(), timeout=60):
        argv = [BINARY, "-batch", "-hostkey=*",
                "-log=" + str(self.config / "session.log")]
        argv += list(extra_args)
        argv.append("-command=open sftp://%s:%s@127.0.0.1:%d/ ; %s" %
                    (USER, PASSWORD, self.port, commands))
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
    def test_open_with_a_wrong_password_is_refused(self):
        argv = [BINARY, "-batch", "-hostkey=*",
                "-command=open sftp://%s:not-the-password@127.0.0.1:%d/" %
                (USER, self.port)]
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config)))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Cannot open session", result.stderr)
        self.assertNotIn("Connected", result.stdout)

    @posix_only("curl can only pin an MD5 fingerprint; a SHA256 pin falls "
                "back to known_hosts on that path, so there is nothing to "
                "refuse here")
    def test_open_with_an_unexpected_host_key_is_refused(self):
        """A pinned key that does not match has to stop the connection, and
        say that it was the key rather than the network."""
        result = self.run_script("echo SHOULD_NOT_RUN", extra_args=[
            "-hostkey=ssh-ed25519 256 SHA256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"])
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)
        combined = (result.stdout + result.stderr).lower()
        self.assertIn("host key", combined)

    @posix_only("write protection of a directory is not a mode bit elsewhere")
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

    @posix_only("Ctrl-C is delivered as a signal")
    def test_interrupted_get_can_be_resumed_and_the_file_is_intact(self):
        """Ctrl-C in the middle of a download must leave a partial file that
        'get -resume' completes byte for byte - and must not touch the
        source."""
        source = self.write_in_jail("big.bin", 6 << 20)
        digest = sha256_of(source)
        target = self.local / "big.bin"
        client = self.start_detached("get -speed=512 /big.bin %s" % target)
        deadline = time.time() + 60
        while time.time() < deadline:
            if target.exists() and target.stat().st_size >= (1 << 20):
                break
            if client.poll() is not None:
                self.fail("the download ended before it could be interrupted: " +
                          client.stderr.read())
            time.sleep(0.05)
        else:
            self.fail("the download never got far enough to interrupt")
        client.send_signal(signal.SIGINT)
        # Drain the pipes rather than just waiting: an interrupt has to be
        # reported, and the child has to be reaped with its output read.
        self.assertNotEqual(client.wait(timeout=60), 0)
        client.communicate(timeout=60)
        partial = target.stat().st_size
        self.assertLess(partial, source.stat().st_size)
        self.assertEqual(sha256_of(source), digest)

        result = self.run_script("get -resume /big.bin %s" % target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(target.stat().st_size, source.stat().st_size)
        self.assertEqual(sha256_of(target), digest)

    def test_resume_across_a_connection_that_died(self):
        """The connection going away mid-transfer is the case '-resume' exists
        for: the retry has to finish the file, not restart it or splice it."""
        source = self.write_in_jail("dying.bin", 8 << 20)
        digest = sha256_of(source)
        target = self.local / "dying.bin"
        client = self.start_detached("get -speed=256 /dying.bin %s" % target)
        deadline = time.time() + 60
        while time.time() < deadline:
            if target.exists() and target.stat().st_size >= (1 << 20):
                break
            if client.poll() is not None:
                break
            time.sleep(0.05)
        # The server dies under the transfer, which is not a graceful close.
        self.stop_server(force=True)
        result = client.communicate(timeout=90)
        self.assertNotEqual(client.returncode, 0)
        partial = target.stat().st_size
        self.assertGreater(partial, 0)
        self.assertLess(partial, source.stat().st_size)
        self.assertIn("failed", (result[0] + result[1]).lower())

        # The port and the process are replaced together; the next 'open'
        # has to reach the new one.
        self.stop_server()
        self.start_server()
        result = self.run_script("get -resume /dying.bin %s" % target, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(target.stat().st_size, source.stat().st_size)
        self.assertEqual(sha256_of(target), digest)

    @posix_only("curl cannot set the modification time of an upload")
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


def run_and_report():
    """Run the suite, then print one line that says what was actually run.

    'make check' and CI are green whether or not the tests could run, so a
    missing dependency used to be invisible: this line, which the CI jobs
    repeat as an annotation, records the skip count next to the pass count.
    """
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    line = ("evidence: suite=%s platform=%s ran=%d skipped=%d failed=%d" %
            (SUITE_NAME, sys.platform, result.testsRun, len(result.skipped),
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
