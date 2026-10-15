"""Live tests against the throwaway WebDAV endpoint in this directory.

TestSftpServer.py covers SFTP, TestS3Server.py covers signed HTTP, and
WebDAV is a third dialect again: PROPFIND listings, MKCOL, MOVE, the
Depth header and 401 challenges rather than signatures. This suite
starts TestDavServer.py (plain HTTP on a kernel-picked port, Basic
credentials testuser/testpass) and drives the console client through
what a DAV filesystem has to answer:

  - a wrong password refused by the challenge, before any command runs;
  - listings built from PROPFIND properties (type, size, date), which
    is what a server that refuses GET on collections can only answer;
  - stat with the real size and time, and a missing path that says so;
  - mkdir refusing an existing name, rmdir refusing a non-empty
    directory, remove of a missing file failing;
  - rename through MOVE leaving exactly the destination behind;
  - an upload/download roundtrip with a checksum.

Unlike the SFTP and S3 suites this one needs no POSIX-only pieces: the
client uses curl over plain HTTP and the server is the standard
library, so the suite also runs where CTest runs - Windows included.

Evidence convention (same as every other suite):

    evidence: suite=live-dav platform=<sys> ran=N skipped=N failed=N
"""

import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())
SUITE_NAME = "live-dav"
HERE = pathlib.Path(__file__).resolve().parent

USER = "testuser"
PASSWORD = "testpass"


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class LiveDavTests(unittest.TestCase):
    """A fresh server (and therefore a fresh store) per test."""

    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.TemporaryDirectory(prefix="live-dav-")
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
            [sys.executable, str(HERE / "TestDavServer.py")], **popen_args)
        self.port = None
        timer = threading.Timer(30.0, self.server.kill)
        timer.start()
        try:
            for line in self.server.stdout:
                line = line.strip()
                if line.startswith("READY "):
                    self.port = int(line.split()[1])
                    break
            if self.port is None:
                self.fail("the WebDAV test server did not report a port")
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
    def run_script(self, commands, creds=None, timeout=60):
        user, secret = creds if creds else (USER, PASSWORD)
        argv = [BINARY, "-batch", "-hostkey=*",
                "-log=" + str(self.config / "session.log"),
                "-command=open dav://%s:%s@127.0.0.1:%d/ ; %s" %
                (user, secret, self.port, commands)]
        env = dict(os.environ, LINUXSCP_CONFIG_DIR=str(self.config))
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout, env=env)

    # -- tests --------------------------------------------------------------
    def test_wrong_password_is_refused_by_the_challenge(self):
        """--anyauth lets the server challenge, and the second attempt
        with the wrong password still comes back 401 - so 'open' stops
        there instead of running commands against a server that will
        never accept them."""
        result = self.run_script("echo SHOULD_NOT_RUN",
                                 creds=(USER, "not-the-password"))
        self.assertEqual(result.returncode, 1,
                         result.stdout + result.stderr)
        self.assertIn("401 Unauthorized", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_listing_comes_from_propfind_properties(self):
        """mkdir, put and then two levels of listing: the directory as
        a directory, the object with its byte count and date."""
        source = self.root / "hello.txt"
        # Bytes, not write_text: Windows write_text would translate LF to
        # CRLF and change the size this test asserts on both platforms.
        source.write_bytes(b"hello dav world\n")
        result = self.run_script(
            "mkdir /docs ; mkdir /docs/2026 ; "
            "put %s /docs/hello.txt ; "
            "ls ; ls -l /docs ; echo LIST_OK" % source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LIST_OK", result.stdout)
        root_part = result.stdout.split("Reading directory /...")[1]
        self.assertRegex(root_part, r"drwx[-rwx]+\s+\S+\s+\S+\s+\S+\s+\d+\s+"
                                    r"\S+\s+\S+\s+\S+\s+docs")
        bucket_part = result.stdout.split("Reading directory /docs...")[1]
        self.assertIn("hello.txt", bucket_part)
        self.assertRegex(bucket_part, r"\s16\s.*hello\.txt")
        self.assertIn("2026", bucket_part)

        # A second session still sees it: the store is the server's.
        result = self.run_script("ls -l /docs ; echo STILL_THERE")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hello.txt", result.stdout)

    def test_stat_reports_size_and_time_and_missing_paths(self):
        source = self.root / "sized.bin"
        source.write_bytes(b"x" * 4321)
        self.assertEqual(self.run_script(
            "mkdir /docs ; put %s /docs/sized.bin" % source).returncode, 0)

        result = self.run_script("stat /docs/sized.bin ; echo STAT_OK")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # One listing line like upstream's, carrying the size in its
        # column; the key/value breakdown is what 'attr' prints now.
        stat_lines = [line for line in result.stdout.splitlines()
                      if line.rstrip().endswith("sized.bin")]
        self.assertEqual(len(stat_lines), 1, result.stdout)
        self.assertIn("4321", stat_lines[0])
        self.assertNotIn("SizeBytes:", result.stdout)
        self.assertIn("STAT_OK", result.stdout)
        result = self.run_script("attr /docs/sized.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SizeBytes:   4321", result.stdout)

        result = self.run_script("stat /docs/no-such-file ; echo NOPE")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("No such file or directory", result.stderr)
        self.assertNotIn("NOPE", result.stdout)

    def test_upload_and_download_roundtrip(self):
        source = self.root / "roundtrip.bin"
        source.write_bytes(bytes(range(256)) * 32)
        downloaded = self.root / "roundtrip-downloaded.bin"
        result = self.run_script(
            "mkdir /docs ; put %s /docs/roundtrip.bin ; "
            "get /docs/roundtrip.bin %s ; echo ROUNDTRIP_OK" %
            (source, downloaded))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ROUNDTRIP_OK", result.stdout)
        self.assertEqual(sha256_of(source), sha256_of(downloaded))

    def test_transfer_preset_drives_ascii_and_loads_with_the_site(self):
        """Transfer presets against a real server: the active preset
        converts a text upload exactly like -transfer=ascii would, and a
        site that stores the preset loads it when it is opened."""
        source = self.root / "preset-lf.txt"
        source.write_bytes(b"a\nb\n")
        out_active = self.root / "preset-active.txt"
        out_off = self.root / "preset-off.txt"
        out_site = self.root / "preset-site.txt"

        # An active preset supplies the settings; 'option preset off'
        # hands the transfer back to its own (binary) defaults.
        result = self.run_script(
            "mkdir /docs ; "
            "savepreset presetascii -transfer=ascii -host=127.0.0.1 ; "
            "presets ; option preset presetascii ; "
            "put %s /docs/preset-ascii.txt ; "
            "savepreset presetbinary -transfer=binary -host=127.0.0.1 ; "
            "option preset presetbinary ; "
            "put %s /docs/preset-binary.txt ; "
            "option preset off ; "
            "get /docs/preset-ascii.txt %s ; "
            "get /docs/preset-binary.txt %s ; "
            "echo PRESET_OK" % (source, source, out_active, out_off))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PRESET_OK", result.stdout)
        self.assertIn("presetascii", result.stdout)
        self.assertEqual(out_active.read_bytes(), b"a\r\nb\r\n")
        self.assertEqual(out_off.read_bytes(), b"a\nb\n")

        # The same preset stored with the site: opening the site restores
        # it, and the conversion happens without any switch on 'put'.
        result = self.run_script(
            "savepreset sitepreset -transfer=ascii -host=127.0.0.1 ; "
            "savesession dasite -protocol=dav -host=127.0.0.1 -port=%d "
            "-username=%s -password=%s -preset=sitepreset ; "
            "sessions dasite ; "
            "put %s /docs/site-ascii.txt ; "
            "option preset off ; "
            "get /docs/site-ascii.txt %s ; "
            "echo SITE_OK" % (self.port, USER, PASSWORD, source, out_site))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SITE_OK", result.stdout)
        self.assertEqual(out_site.read_bytes(), b"a\r\nb\r\n")

        # Removing the preset leaves the file without it, and the test
        # takes its presets and site out of the shared configuration
        # again: a host-matching preset would otherwise (correctly!)
        # apply to every later test that connects to this server.
        result = self.run_script(
            "rmspreset sitepreset ; rmspreset presetascii ; "
            "rmspreset presetbinary ; rmsession dasite ; presets")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Removed transfer preset", result.stdout)
        self.assertNotIn("  sitepreset", result.stdout)
        self.assertNotIn("  presetascii", result.stdout)
        self.assertNotIn("  presetbinary", result.stdout)

    def test_download_permissions_and_raw_transfer_settings(self):
        """Two switch families against the wire: a raw transfer setting
        in upstream's key vocabulary converting an upload, and
        '-permissions=<mode>' giving the downloaded local file that
        mode."""
        source = self.root / "raw-lf.txt"
        source.write_bytes(b"x\ny\n")
        got = self.root / "raw-got.txt"
        protected = self.root / "raw-protected.txt"
        result = self.run_script(
            "mkdir /docs ; "
            "put -rawtransfersettings=1 TransferMode=1 %s /docs/raw.txt ; "
            "get /docs/raw.txt %s ; "
            "get -permissions=444 /docs/raw.txt %s ; "
            "echo RAW_OK" % (source, got, protected))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RAW_OK", result.stdout)
        # TransferMode=1 in the raw settings means ascii: the upload
        # converted LF to CRLF, and the plain binary download shows it.
        self.assertEqual(got.read_bytes(), b"x\r\ny\r\n")
        # The download was told to make the local file read-only.
        self.assertEqual(os.stat(protected).st_mode & 0o200, 0)

    def test_mkdir_refuses_an_existing_name(self):
        result = self.run_script(
            "mkdir /docs ; mkdir /docs ; echo SHOULD_NOT_SEE")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("already exists", result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

    def test_rmdir_refuses_a_nonempty_directory(self):
        source = self.root / "keep.txt"
        source.write_text("keep\n", encoding="utf-8")
        result = self.run_script(
            "option confirm off ; "
            "mkdir /docs ; mkdir /docs/full ; "
            "put %s /docs/full/keep.txt ; "
            "rmdir /docs/full ; echo SHOULD_NOT_SEE" % source)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not empty", result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

        # Empty it and the same rmdir has to go through - and the
        # directory with it.
        result = self.run_script(
            "option confirm off ; rm /docs/full/keep.txt ; "
            "rmdir /docs/full ; ls /docs ; echo RMDIR_OK")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RMDIR_OK", result.stdout)
        listing = result.stdout.split("Reading directory /docs...")[1]
        self.assertNotIn("full", listing)

    def test_rename_moves_and_remove_reports_a_missing_file(self):
        first = self.root / "a.txt"
        first.write_text("alpha\n", encoding="utf-8")
        second = self.root / "b.txt"
        second.write_text("bravo\n", encoding="utf-8")
        result = self.run_script(
            "option confirm off ; "
            "mkdir /docs ; put %s /docs/a.txt ; put %s /docs/b.txt ; "
            "mv /docs/a.txt /docs/renamed.txt ; "
            "ls -l /docs ; "
            "get /docs/renamed.txt %s/renamed.txt ; "
            "rm /docs/b.txt ; "
            "rm /docs/b.txt ; echo SHOULD_NOT_SEE" %
            (first, second, self.root))
        # The second rm (b.txt is gone) is what fails the script.
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual((self.root / "renamed.txt").read_text(), "alpha\n")
        self.assertIn("No such file or directory", result.stderr)
        self.assertNotIn("SHOULD_NOT_SEE", result.stdout)

        # The source of the rename is gone; the destination survived.
        listing = self.run_script("ls -l /docs")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("renamed.txt", listing.stdout)
        self.assertNotIn("a.txt", listing.stdout.split("a.txt")[0])
        self.assertNotIn(
            " a.txt", listing.stdout.replace("renamed.txt", ""))

        # And a move of something that was never there reports it.
        result = self.run_script(
            "option confirm off ; mv /docs/ghost.txt /docs/x ; "
            "echo SHOULD_NOT_SEE")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("No such file or directory", result.stderr)


    def test_session_stat_close_and_no_match_follow_upstream(self):
        """The console outputs that need a live session, checked against
        the shapes upstream produces: the open-session listing of
        'session', the single listing line of 'stat', the line 'close'
        prints, and the no-match sentence of a mask that matched
        nothing - with and without failonnomatch."""
        source = self.root / "hello.txt"
        source.write_bytes(b"hello dav world\n")
        # stat in its own run, so its one listing line is unambiguous.
        result = self.run_script(
            "mkdir /docs ; put %s /docs/hello.txt ; stat /docs/hello.txt" %
            source)
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        stat_lines = [line for line in result.stdout.splitlines()
                      if line.startswith("-")
                      and line.rstrip().endswith("hello.txt")]
        self.assertEqual(len(stat_lines), 1, result.stdout)
        self.assertNotIn("SizeBytes:", result.stdout)

        # cd, pwd, dir, session and close in a second run against it.
        result = self.run_script(
            "cd /docs ; pwd ; dir ; session ; session 1 ; "
            "close ; echo AFTER_CLOSE")
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        # cd and pwd print the directory, like upstream, and dir is ls.
        self.assertRegex(result.stdout, r"(?m)^/docs$")
        self.assertIn("hello.txt", result.stdout.split("Active session")[0])
        # session: '%3d  name' for every open session plus the active line.
        self.assertRegex(result.stdout, r"(?m)^\s*1  127\.0\.0\.1$")
        self.assertIn("Active session: 1 (127.0.0.1)", result.stdout)
        # close: the closed session named, and the script carried on.
        self.assertIn("Session '127.0.0.1' closed.", result.stdout)
        self.assertIn("AFTER_CLOSE", result.stdout)

        # An index that does not exist is refused while connected.
        result = self.run_script("session 5 ; echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Invalid session index", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

        # After close the session is gone and the next command says so.
        result = self.run_script("close ; pwd ; echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Session '127.0.0.1' closed.", result.stdout)
        self.assertIn("Not connected", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

        # An empty mask prints the no-match sentence instead of failing;
        # failonnomatch turns the same sentence into the script's error.
        result = self.run_script(
            "mkdir /empty ; ls /empty/*.txt ; echo AFTER_MATCH")
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn("No files found matching", result.stdout)
        self.assertIn("AFTER_MATCH", result.stdout)
        result = self.run_script(
            "option failonnomatch on ; ls /empty/*.txt ; "
            "echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("No files found matching", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)


    def test_alias_names_reach_the_commands_they_stand_for(self):
        """md, rename, recv, mget, mput and del are this client's aliases;
        they have to do the work of the command, and the script stops at
        quit like it stops at exit."""
        source = self.root / "alias.txt"
        source.write_bytes(b"alias data\n")
        downloaded = self.root / "alias-downloaded.txt"
        reuploaded = self.root / "alias-reuploaded.txt"
        result = self.run_script(
            "option confirm off ; "
            "mkdir /docs ; md /docs/sub ; put %s /docs/a.txt ; "
            "rename /docs/a.txt /docs/b.txt ; "
            "recv /docs/b.txt %s ; mget /docs/b.txt %s ; "
            "mput %s /docs/c.txt ; del /docs/b.txt ; del /docs/c.txt ; "
            "ls /docs ; echo ALIASES_OK ; quit ; echo SHOULD_NOT_RUN" %
            (source, downloaded, reuploaded, reuploaded))
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn("ALIASES_OK", result.stdout)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)
        self.assertEqual(sha256_of(source), sha256_of(downloaded))
        self.assertEqual(sha256_of(source), sha256_of(reuploaded))
        listing = self.run_script("ls /docs")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("sub", listing.stdout)
        self.assertNotIn("a.txt", listing.stdout)
        self.assertNotIn("b.txt", listing.stdout)
        self.assertNotIn("c.txt", listing.stdout)


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
