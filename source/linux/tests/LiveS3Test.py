"""Live tests against the throwaway S3 endpoint in this directory.

TestSftpServer.py covers the SFTP protocol; Amazon S3 is a signed-HTTP
protocol and needs its own server, so this file exists: it starts
TestS3Server.py (TLS on a port the kernel picks, credentials fixed to
AKIDTEST/testsecret42) and drives the console client against it -
bucket and object listings, mkdir/rmdir markers, upload, download,
rename (copy-plus-delete), remove, a resumed download that must go
through an HTTP byte range, and the two refusal paths: a wrong secret
rejected by signature verification and missing credentials refused
before any request. One test additionally signs a request with
botocore, Amazon's own reference implementation, so that the server's
verification and the client's signing are both checked against
something neither of them wrote.

No third-party requirement beyond the standard library - botocore is
optional and the test that needs it skips with that as its reason, the
same way the SFTP suite treats paramiko. The suite is POSIX in
practice (it shells out to curl over TLS against 127.0.0.1); where it
cannot run it says so per test.

Evidence convention (same as every other suite):

    evidence: suite=live-s3 platform=linux ran=N skipped=N failed=N
"""

import base64
import hashlib
import importlib.util
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())
SUITE_NAME = "live-s3"
HERE = pathlib.Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
CERT = FIXTURES / "s3-server-cert.pem"
KEY = FIXTURES / "s3-server-key.pem"

ACCESS_KEY = "AKIDTEST"
SECRET_KEY = "testsecret42"
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def posix_only(reason):
    return unittest.skipIf(os.name != "posix", "POSIX only: " + reason)


class LiveS3Tests(unittest.TestCase):
    """A fresh server (and therefore a fresh in-memory store) per test,
    started over TLS on a kernel-picked port."""

    @classmethod
    def setUpClass(cls):
        if not CERT.is_file() or not KEY.is_file():
            raise unittest.SkipTest(
                "the TLS fixtures in fixtures/ are missing; generate them "
                "with openssl (see TestS3Server.py)")
        cls.workdir = tempfile.TemporaryDirectory(prefix="live-s3-")
        cls.root = pathlib.Path(cls.workdir.name) / "local"
        cls.root.mkdir()
        cls.config = pathlib.Path(cls.workdir.name) / "config"
        cls.config.mkdir()
        cls.addClassCleanup(cls.workdir.cleanup)

    def setUp(self):
        self.start_server()

    def tearDown(self):
        self.stop_server()

    # -- the server ---------------------------------------------------------
    def start_server(self):
        env = dict(os.environ, LSCP_TEST_PORT="0")
        popen_args = dict(env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)
        if os.name == "posix":
            popen_args["start_new_session"] = True
        self.server = subprocess.Popen(
            [sys.executable, str(HERE / "TestS3Server.py")], **popen_args)
        self.port = None
        timer = threading.Timer(30.0, self.server.kill)
        timer.start()
        try:
            for line in self.server.stdout:
                line = line.strip()
                if line.startswith("READY "):
                    self.port = int(line.split()[1])
                    break
                if line.startswith("MISSING-FIXTURES"):
                    self.fail(line)
            if self.port is None:
                self.fail("the S3 test server did not report a port")
        finally:
            timer.cancel()

    def stop_server(self):
        if getattr(self, "server", None) is None:
            return
        self.server.terminate()
        try:
            self.server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.server.wait(timeout=5)
        self.server = None

    # -- driving the client -------------------------------------------------
    def run_script(self, commands, creds=None, extra_args=(), timeout=60):
        """'open sftp-style' by URL, then the script; creds defaults to
        the server's fixed pair."""
        user, secret = creds if creds else (ACCESS_KEY, SECRET_KEY)
        argv = [BINARY, "-batch", "-hostkey=*",
                "-log=" + str(self.config / "session.log")]
        argv += list(extra_args)
        argv.append("-command=open s3://%s:%s@127.0.0.1:%d/ ; %s" %
                    (user, secret, self.port, commands))
        env = dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config))
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout, env=env)

    def open_only(self, creds=None, extra_args=()):
        user, secret = creds if creds else (ACCESS_KEY, SECRET_KEY)
        return self.run_script("echo OPENED", creds=creds,
                               extra_args=extra_args)

    # -- tests --------------------------------------------------------------
    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_missing_secret_is_refused_before_any_request(self):
        """A site without a secret key cannot be signed at all, and says
        so instead of trying anonymously."""
        argv = [BINARY, "-batch", "-hostkey=*",
                "-command=open s3://%s@127.0.0.1:%d/ ; echo SHOULD_NOT_RUN" %
                (ACCESS_KEY, self.port)]
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=60,
                                env=dict(os.environ,
                                         LINUXSCP_CONFIG_DIR=str(self.config)))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("secret access key", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_wrong_secret_is_rejected_by_signature_verification(self):
        """The server re-derives every signature; a wrong secret has to
        come back as a named S3 error, not a silent open."""
        result = self.run_script("echo SHOULD_NOT_RUN",
                                 creds=(ACCESS_KEY, "not-the-secret"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("SignatureDoesNotMatch", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_listing_shows_buckets_directories_and_objects(self):
        """mkdir creates buckets and markers, put creates objects, and
        both levels of listing report them with sizes."""
        source = self.root / "hello.txt"
        source.write_text("hello s3 world\n", encoding="utf-8")
        result = self.run_script(
            "mkdir /photos ; mkdir /photos/2026 ; "
            "put %s /photos/hello.txt ; "
            "ls ; ls -l /photos ; echo LIST_OK" % source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LIST_OK", result.stdout)
        # Root listing: the bucket as a directory.
        root_part = result.stdout.split("Reading directory /...")[1]
        self.assertRegex(root_part, r"drwx[-rwx]+\s+\S+\s+\S+\s+\S+\s+\d+\s+"
                                    r"\S+\s+\S+\s+\S+\s+photos")
        # Bucket listing: the object with its size, the directory.
        bucket_part = result.stdout.split("Reading directory /photos...")[1]
        self.assertIn("hello.txt", bucket_part)
        self.assertRegex(bucket_part, r"\s15\s.*hello\.txt")
        self.assertIn("2026", bucket_part)

        # And a fresh connection still sees it all (the store is the
        # server's, not the session's).
        result = self.run_script("ls -l /photos ; echo STILL_THERE")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hello.txt", result.stdout)

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_rmdir_refuses_nonempty_and_accepts_virtual_directories(self):
        """rmdir on S3 has to behave like everywhere else: a directory
        with contents fails, a marker-less 'virtual' prefix with none
        succeeds, and the bucket itself is not a directory."""
        source = self.root / "payload.txt"
        source.write_text("payload\n", encoding="utf-8")
        result = self.run_script(
            "option confirm off ; "
            "mkdir /photos ; mkdir /photos/full ; "
            "put %s /photos/full/keep.txt ; "
            "rmdir /photos/full ; echo SHOULD_NOT_SEE" % source)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not empty", result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

        # A prefix that was never mkdir'd: object underneath, then the
        # object removed - rmdir has nothing marker-wise to delete and
        # must still succeed.
        result = self.run_script(
            "option confirm off ; "
            "put %s /photos/virtual/deep.txt ; "
            "rm /photos/virtual/deep.txt ; rmdir /photos/virtual ; "
            "ls /photos ; echo VIRTUAL_OK" % source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("VIRTUAL_OK", result.stdout)
        listing = result.stdout.split("Reading directory /photos...")[1]
        listing = listing.split("VIRTUAL_OK")[0]
        self.assertNotIn("virtual", listing)

        # rmdir on the bucket itself is refused with a way out.
        result = self.run_script(
            "option confirm off ; rmdir /photos ; echo SHOULD_NOT_SEE")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("bucket itself", result.stderr)

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_upload_and_download_roundtrip(self):
        source = self.root / "roundtrip.bin"
        source.write_bytes(bytes(range(256)) * 64)
        downloaded = self.root / "roundtrip-downloaded.bin"
        result = self.run_script(
            "mkdir /photos ; put %s /photos/roundtrip.bin ; "
            "get /photos/roundtrip.bin %s ; echo ROUNDTRIP_OK" %
            (source, downloaded))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ROUNDTRIP_OK", result.stdout)
        self.assertEqual(sha256_of(source), sha256_of(downloaded))

        # attr reports the size the server does; stat prints the single
        # listing line upstream prints, with no key/value lines at all.
        result = self.run_script("attr /photos/roundtrip.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SizeBytes:   %d" % source.stat().st_size,
                      result.stdout)
        result = self.run_script("stat /photos/roundtrip.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("SizeBytes:", result.stdout)
        self.assertRegex(result.stdout, r"(?m)^\S.* roundtrip\.bin$")

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_resumed_download_continues_through_a_byte_range(self):
        """A pre-existing staging file makes the download resume with
        curl -C; the server answers only with a range, so a client that
        ignored the offset would get a whole-object 200 and curl would
        refuse it (exit 33) - completion and checksum prove the range
        path was taken."""
        source = self.root / "large.bin"
        payload = bytes(bytearray((i * 7 + 3) % 256 for i in range(300000)))
        source.write_bytes(payload)
        destination = self.root / "large-dl.bin"
        staging = pathlib.Path(str(destination) + ".filepart")
        staging.write_bytes(payload[:123456])  # resume from here

        result = self.run_script(
            "mkdir /photos ; put %s /photos/large.bin ; "
            "get -resume /photos/large.bin %s ; echo RESUME_OK" %
            (source, destination))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RESUME_OK", result.stdout)
        self.assertFalse(staging.exists())
        self.assertEqual(sha256_of(source), sha256_of(destination))

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_rename_and_remove(self):
        """Rename is CopyObject plus a delete of the source; remove is a
        DELETE that has to fail on a missing key afterwards."""
        first = self.root / "a.txt"
        first.write_text("alpha\n", encoding="utf-8")
        second = self.root / "b.txt"
        second.write_text("bravo\n", encoding="utf-8")
        result = self.run_script(
            "option confirm off ; "
            "mkdir /photos ; put %s /photos/a.txt ; put %s /photos/b.txt ; "
            "mv /photos/a.txt /photos/renamed.txt ; "
            "ls -l /photos ; "
            "get /photos/renamed.txt %s/renamed.txt ; "
            "rm /photos/b.txt ; "
            "rm /photos/b.txt ; echo SHOULD_NOT_SEE" %
            (first, second, self.root))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("renamed.txt", result.stdout)
        # renamed.txt is the copy of a.txt; b.txt was the one removed.
        self.assertEqual((self.root / "renamed.txt").read_text(),
                         "alpha\n")
        # The second rm (b.txt already gone) is what must fail the script.
        self.assertIn("No such file or directory", result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

        # a.txt really moved: the old name is gone.
        result = self.run_script(
            "get /photos/a.txt %s/gone.txt ; echo SHOULD_NOT_SEE" % self.root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

    @posix_only("the client shells out to curl against 127.0.0.1")
    def test_unsigned_and_botocore_signed_requests(self):
        """Two directions of agreement: a request with no signature at
        all is refused, and a request signed by botocore - the AWS
        reference implementation - is accepted by this server, which is
        what makes the client's signatures mean something."""
        context = ssl._create_unverified_context()

        unsigned = urllib.request.Request(
            "https://127.0.0.1:%d/" % self.port, method="GET")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(unsigned, context=context, timeout=15)
        self.assertEqual(caught.exception.code, 403)
        body = caught.exception.read().decode("utf-8", "replace")
        self.assertIn("InvalidRequest", body)

        if importlib.util.find_spec("botocore") is None:
            self.skipTest(
                "botocore is not installed; skipping the reference-"
                "implementation signature check (pip install botocore)")
        import botocore.auth
        import botocore.awsrequest
        from botocore.credentials import Credentials

        # Seed a bucket the botocore request can list.
        self.assertEqual(self.run_script(
            "mkdir /botobucket").returncode, 0)

        request = botocore.awsrequest.AWSRequest(
            method="GET", url="https://127.0.0.1:%d/" % self.port,
            headers={"x-amz-content-sha256": EMPTY_SHA256})
        botocore.auth.SigV4Auth(
            Credentials(ACCESS_KEY, SECRET_KEY), "s3",
            "us-east-1").add_auth(request)
        prepared = request.prepare()
        wire = urllib.request.Request(prepared.url, method="GET")
        for name, value in request.headers.items():
            if name.lower() != "host":
                wire.add_header(name, value)
        response = urllib.request.urlopen(wire, context=context, timeout=15)
        self.assertEqual(response.status, 200)
        xml = response.read().decode("utf-8")
        self.assertIn("<Name>botobucket</Name>", xml)


def run_and_report():
    """Run the suite, then print one line that says what was actually
    run (the convention every suite shares for CI annotations)."""
    suite = unittest.defaultTestLoader.loadTestsFromModule(
        sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    line = ("evidence: suite=%s platform=%s ran=%d skipped=%d failed=%d" %
            (SUITE_NAME, sys.platform, result.testsRun, len(result.skipped),
             len(result.failures) + len(result.errors)))
    if not result.testsRun and result.skipped:
        reasons = "; ".join(sorted(set(str(reason)[:160]
                                       for _, reason in result.skipped)))
        line += " because: " + reasons
    print(line)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run_and_report())
