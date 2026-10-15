"""Server-free CLI regression tests, shared by Linux and Windows builds."""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())
SUITE_NAME = "script"


class ScriptTests(unittest.TestCase):
    def setUp(self):
        # Every test gets its own configuration directory, so that a test
        # never reads or writes the sessions of the user running them.
        self._config = tempfile.TemporaryDirectory(prefix="linuxscp-config-")
        self.config = self._config.name
        self.addCleanup(self._config.cleanup)

    def environment(self, extra=None):
        env = dict(os.environ)
        env["LINUXSCP_CONFIG_DIR"] = self.config
        if extra:
            env.update(extra)
        return env

    def run_commands(self, text, file=False, env=None):
        env = self.environment(env)
        if file:
            with tempfile.TemporaryDirectory() as tmp:
                path = pathlib.Path(tmp) / "commands.txt"
                path.write_text(text.replace(";", "\n"), encoding="utf-8")
                return subprocess.run([BINARY, "-script=" + str(path)],
                                      capture_output=True, text=True, timeout=10,
                                      env=env)
        return subprocess.run([BINARY, "-command=" + text],
                              capture_output=True, text=True, timeout=10, env=env)

    def test_default_abort(self):
        for file in (False, True):
            with self.subTest(file=file):
                result = self.run_commands("invalid-command; echo SHOULD_NOT_RUN", file)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_batch_modes(self):
        for file in (False, True):
            for mode in ("on", "abort", "off", "continue"):
                with self.subTest(mode=mode, file=file):
                    result = self.run_commands(
                        f"option batch {mode}; invalid-command; echo AFTER_ERROR", file)
                    self.assertEqual(result.returncode, 1)
                    self.assertEqual("AFTER_ERROR" in result.stdout,
                                     mode in ("off", "continue"))

    def test_batch_queries(self):
        for mode in ("on", "abort", "off", "continue"):
            with self.subTest(mode=mode):
                result = self.run_commands(f"option batch {mode}; option batch")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("batch = " + mode, result.stdout)

    def test_reject_noop_options(self):
        for command in ("option reconnecttime2 10", "option batch garbage",
                        "option transfer nonsense", "option reconnecttime nope",
                        "option include *.txt|bad|mask"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_option_abbreviations(self):
        # An unambiguous abbreviation names one option, like the upstream
        # command table; an ambiguous one is refused instead of guessing.
        result = self.run_commands("option synchdel on; option synchdelete")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("synchdelete = on", result.stdout)
        result = self.run_commands("option transfer asc; option transfer")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("transfer = ascii", result.stdout)
        # 'e' could be echo, editor, editmaxsize or exclude; the client must
        # not guess which one a script meant.
        for command in ("option e on", "option ed nano", "option re 10"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1)
                self.assertIn("ambiguous", result.stderr)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)
        # An unambiguous abbreviation of both name and value is accepted.
        result = self.run_commands("option bat cont; option batch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("batch = continue", result.stdout)

    def test_command_abbreviations(self):
        result = self.run_commands("opt bat cont; echo AFTER")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("AFTER", result.stdout)
        # 'se' could be session or sessions, and 'l' lcd, lls, ln, lpwd or
        # ls; guessing is refused, while 'co' unambiguously means copyid.
        for command in ("se", "s", "l"):
            with self.subTest(command=command):
                result = self.run_commands(command)
                self.assertEqual(result.returncode, 1)
                self.assertIn("ambiguous", result.stderr)
        # An unambiguous abbreviation reaches the command itself.
        result = self.run_commands("synch local")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("ambiguous", result.stderr)
        self.assertNotIn("Unknown command", result.stderr)
        # A name that matches nothing is still an unknown command.
        result = self.run_commands("nosuchcommand")
        self.assertIn("Unknown command", result.stderr)

    def test_script_parameters(self):
        # %1% .. %9%, environment variables and %TIMESTAMP% are expanded
        # before the command is parsed, as the upstream console does.
        env = {"LSCP_TEST_PARAM": "from-env"}
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "s.txt"
            script.write_text("echo %1%-%2%-end\n", encoding="utf-8")
            result = subprocess.run(
                [BINARY, "-script=" + str(script), "-parameter", "one", "two"],
                capture_output=True, text=True, timeout=10,
                env=self.environment(env))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("one-two-end", result.stdout)
        result = subprocess.run(
            [BINARY, "-command=echo [%LSCP_TEST_PARAM%]", "-parameter", "x"],
            capture_output=True, text=True, timeout=10, env=self.environment(env))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("[from-env]", result.stdout)
        # An unset variable is left alone rather than turned into nothing,
        # and so is a parameter that was not given.
        result = self.run_commands("echo [%NO_SUCH_VAR%] [%3%]")
        self.assertIn("[%NO_SUCH_VAR%] [%3%]", result.stdout)
        # A malformed timestamp offset fails instead of transferring to a
        # path that still contains the placeholder.
        result = self.run_commands("echo %TIMESTAMP-1Q#yyyymmdd%")
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not valid", result.stderr)
        result = self.run_commands("echo %TIMESTAMP#yyyymmdd%")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout.strip(), r"^[0-9]{8}$")

    def test_parameters_are_not_expanded_in_server_mode(self):
        # The GUI talks to -server and may send a file name that holds a
        # percent sign; those lines are commands, not templates.
        result = subprocess.run([BINARY, "-server"], input="echo %1%\n",
                                capture_output=True, text=True, timeout=10,
                                env=self.environment())
        self.assertIn("LSCP-RESULT 0", result.stdout)
        self.assertIn("%1%", result.stdout)

    def test_transfer_modes_are_real_options(self):
        # ascii/automatic and reconnecttime used to be rejected; they are
        # implemented now, so they must stop the run with success.
        for command in ("option transfer ascii", "option transfer automatic",
                        "ascii", "binary", "option reconnecttime 30",
                        "option echo on", "option failonnomatch on",
                        "option synchdelete on", "option include *.txt",
                        "option exclude *.bak"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo OK")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("OK", result.stdout)

    def test_option_listing(self):
        # 'option' with no name lists every supported option.
        result = self.run_commands("option")
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("echo", "batch", "confirm", "resume", "transfer",
                     "include", "exclude", "failonnomatch", "synchdelete",
                     "reconnecttime", "editor", "editmaxsize"):
            self.assertIn(name + " = ", result.stdout)
        # The default resume mode is upstream's 'automatic' (rsSmart):
        # binary files from a 100 KB threshold stage through .filepart.
        self.assertIn("resume = automatic", result.stdout)

    def test_option_resume_values(self):
        for value, shown in (("on", "on"), ("off", "off"),
                             ("automatic", "automatic"), ("aut", "automatic")):
            with self.subTest(value=value):
                result = self.run_commands(f"option resume {value}; option resume")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("resume = " + shown, result.stdout)
        result = self.run_commands("option resume sometimes")
        self.assertEqual(result.returncode, 1)
        self.assertIn("on, off or automatic", result.stderr)
        # 'option resume on' forces binary transfers, so an ASCII transfer
        # is refused before any connection, like an explicit -resume.
        result = self.run_commands("option resume on; get -transfer=ascii x /tmp/")
        self.assertEqual(result.returncode, 1)
        self.assertIn("binary only", result.stderr)
        self.assertNotIn("Not connected", result.stderr)

    def test_option_echo_prints_commands(self):
        result = self.run_commands("option echo on; echo HELLO")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("echo HELLO", result.stdout)
        self.assertIn("HELLO", result.stdout)

    def test_ascii_resume_is_rejected_before_connection(self):
        for command in ("get -transfer=ascii -resume x /tmp/", "put -resume -transfer=automatic x /tmp/"):
            with self.subTest(command=command):
                result = self.run_commands(command)
                self.assertEqual(result.returncode, 1)
                self.assertIn("binary only", result.stderr)
                self.assertNotIn("Not connected", result.stderr)

    def test_reject_transfer_switches_before_connection(self):
        # Implemented switches (filemask, transfer modes, permissions) pass
        # the switch check and stop at the connection; the rest is refused
        # up front instead of being silently ignored.
        for command in ("get", "recv", "mget", "put", "send", "mput"):
            for switch in ("-resumegarbage", "-resume=off", "-speed=x", "-speed=1K",
                           "-unknown", "-latest", "-criteria=time",
                           "-transfer=garbage", "-filemask=",
                           "-filemask=*.txt|*.a|*.b", "-permissions=999",
                           "-preservetime=hours", "-resumesupport=maybe",
                           "-append -resume"):
                with self.subTest(command=command, switch=switch):
                    result = self.run_commands(f"{command} {switch} source destination")
                    self.assertEqual(result.returncode, 1)
                    self.assertTrue("Unsupported switch" in result.stderr or
                                    "Wrong arguments" in result.stderr,
                                    result.stderr)
                    self.assertNotIn("Not connected", result.stderr)

    def test_transfer_switches_are_accepted(self):
        for command in ("get", "recv", "mget", "put", "send", "mput"):
            for switch in ("-filemask=*.txt|*.bak",
                           "-filemask=*.jpg>1K<1M",
                           "-transfer=binary", "-transfer=ascii",
                           "-transfer=asc", "-transfer=automatic",
                           "-speed=10", "-speed=0", "-resumesupport=on",
                           "-resumesupport=off", "-resumesupport=1024",
                           "-preservetime=all", "-noneweronly", "-append",
                           "-nopreservetime", "-neweronly"):
                with self.subTest(command=command, switch=switch):
                    result = self.run_commands(f"{command} {switch} source destination")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("Not connected", result.stderr)

    def test_transfer_aliases_dispatch(self):
        for command in ("get", "recv", "mget", "put", "send", "mput"):
            with self.subTest(command=command):
                result = self.run_commands(f"{command} -transfer=binary source destination")
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Unknown command", result.stderr)
                self.assertNotIn("Unsupported switch", result.stderr)

    def test_sync_validation(self):
        for args in ("local -criteria=whenever",
                     "local -filemask=*.txt>garbage", "sideways", "local a b c", ""):
            with self.subTest(args=args):
                result = self.run_commands("synchronize " + args)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)

    def test_sync_delete_needs_a_connection(self):
        # 'both -delete' is no longer rejected outright: the recorded
        # state decides what is obsolete. It still needs a server.
        result = self.run_commands("synchronize both -delete /tmp /tmp")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Wrong arguments", result.stderr)
        self.assertIn("Not connected", result.stderr)

    def test_sync_criteria_values(self):
        """'-criteria=' takes upstream's words ('none', 'either', 'both') and
        a comma-separated list of the three comparisons, including
        'checksum'. Anything else is refused with the words that are known,
        rather than being quietly dropped as upstream does."""
        for value in ("time", "size", "either", "both", "none", "checksum",
                      "CHECKSUM", "time,size", "time,checksum",
                      "size,checksum", "time,size,checksum"):
            with self.subTest(value=value):
                result = self.run_commands(
                    "synchronize local -criteria=%s /tmp /tmp" % value)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Not connected", result.stderr)
        for value in ("whenever", "sha256", "check", "time,", ",size",
                      "time,,size", "none,time", "either,size",
                      "checksum,none", "time,none"):
            with self.subTest(value=value):
                result = self.run_commands(
                    "synchronize local -criteria=%s /tmp /tmp" % value)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)
                self.assertNotIn("Not connected", result.stderr)
        # The refusal names every word that is accepted.
        result = self.run_commands("synchronize local -criteria=whenever /tmp /tmp")
        for word in ("none", "either", "both", "time", "size", "checksum"):
            self.assertIn(word, result.stderr)
        # 'keepuptodate' takes the same switches and has no 'both' mode.
        result = self.run_commands("keepuptodate -criteria=time,checksum /tmp /tmp")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Not connected", result.stderr)
        # The help of the command documents the new value.
        result = self.run_commands("help synchronize")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("checksum", result.stdout)

    def test_sync_both_direction_refuses_criteria_it_cannot_honour(self):
        """In the 'both' direction the record of the previous
        synchronization decides, so a criteria value it could not apply is
        refused instead of being ignored (upstream drops the criteria for
        smBoth)."""
        for value, why in (("checksum",
                            "record of the previous synchronization"),
                           ("none", "no way to tell which side changed")):
            with self.subTest(value=value):
                result = self.run_commands(
                    "synchronize both -criteria=%s /tmp /tmp" % value)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)
                self.assertIn(why, result.stderr)
                self.assertNotIn("Not connected", result.stderr)
        # The comparisons the 'both' direction does make still parse.
        for value in ("time", "size", "either", "both", "time,size"):
            with self.subTest(value=value):
                result = self.run_commands(
                    "synchronize both -criteria=%s /tmp /tmp" % value)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Not connected", result.stderr)

    def test_keepuptodate_validation(self):
        # 'keepuptodate' takes no direction word (it always updates the
        # local side), so bare directories parse fine and stop at the
        # connection check.
        for args in ("", "sideways"):
            with self.subTest(args=args):
                result = self.run_commands("keepuptodate " + args)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Not connected", result.stderr)
        for args in ("local -bogus", "a b c"):
            with self.subTest(args=args):
                result = self.run_commands("keepuptodate " + args)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)
        # Without a terminal it needs a fixed number of passes.
        result = self.run_commands("keepuptodate /tmp /tmp")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Not connected", result.stderr)

    def test_edit_needs_a_connection(self):
        result = self.run_commands("edit /tmp/file.txt")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Not connected", result.stderr)

    def test_stored_sessions(self):
        result = self.run_commands("sessions")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no stored sessions", result.stdout)

        result = self.run_commands(
            "savesession demo -host=demo.example.com -username=alice -port=2222")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Stored session 'demo'", result.stdout)

        result = self.run_commands("sessions")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("demo", result.stdout)
        self.assertIn("sftp://alice@demo.example.com:2222", result.stdout)

        # The file the graphical client reads is the one written here.
        stored = pathlib.Path(self.config) / "sessions.ini"
        self.assertTrue(stored.is_file())
        text = stored.read_text(encoding="utf-8")
        self.assertIn("[demo]", text)
        self.assertIn("host=demo.example.com", text)
        self.assertIn("port=2222", text)
        self.assertNotIn("password", text)

        result = self.run_commands("rmsession demo")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_commands("sessions")
        self.assertIn("no stored sessions", result.stdout)

    def test_savesession_stores_password_encrypted(self):
        """A password given to savesession is written encrypted under a
        key file, never in the clear, and the session list stays quiet
        about it."""
        import base64

        result = self.run_commands(
            "savesession secret -host=secret.example.com -username=alice "
            "-password=hunter2-plain")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stored encrypted", result.stdout)
        self.assertIn("sessions.key", result.stdout)

        stored = pathlib.Path(self.config) / "sessions.ini"
        keyfile = pathlib.Path(self.config) / "sessions.key"
        text = stored.read_text(encoding="utf-8")
        self.assertNotIn("hunter2-plain", text)
        self.assertIn("password=lscp1$", text)
        self.assertTrue(keyfile.is_file())
        key = base64.b64decode(keyfile.read_text().strip())
        self.assertEqual(len(key), 32)
        if os.name == "posix":
            # Group or world readable is a bug, whatever the umask was.
            # Windows has no permission bits to compare against.
            self.assertEqual(keyfile.stat().st_mode & 0o077, 0)

        # The listing shows the session, never the password.
        result = self.run_commands("sessions")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("hunter2", result.stdout)

        # Saving again without -password clears a password it could read
        # (the documented way to remove one).
        result = self.run_commands(
            "savesession secret -host=secret.example.com -username=alice")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("password=", stored.read_text(encoding="utf-8"))

    def test_savesession_keeps_a_password_it_cannot_decrypt(self):
        """Without the key file the encrypted value is unreadable, and an
        unrelated re-save must not destroy what it cannot see."""
        import base64

        result = self.run_commands(
            "savesession locked -host=locked.example.com -password=keepme")
        self.assertEqual(result.returncode, 0, result.stderr)

        stored = pathlib.Path(self.config) / "sessions.ini"
        keyfile = pathlib.Path(self.config) / "sessions.key"
        text = stored.read_text(encoding="utf-8")
        blob = None
        for line in text.splitlines():
            if line.startswith("password="):
                blob = line.split("=", 1)[1]
        self.assertIsNotNone(blob)
        self.assertTrue(blob.startswith("lscp1$"))
        self.assertNotIn("keepme", text)
        saved_key = keyfile.read_bytes()
        keyfile.unlink()

        # Re-save without the password: the entry is replaced, but the
        # value it could not decrypt stays exactly as it was.
        result = self.run_commands(
            "savesession locked -host=locked.example.com -port=22")
        self.assertEqual(result.returncode, 0, result.stderr)
        text = stored.read_text(encoding="utf-8")
        self.assertIn("password=" + blob, text)
        self.assertNotIn("keepme", text)

        # With the key restored the same value decrypts again (the core
        # reports the failure rather than using an empty password; the
        # success side is covered by the live suite).
        keyfile.write_bytes(saved_key)
        key = base64.b64decode(saved_key.decode().strip())
        self.assertEqual(len(key), 32)

    def test_session_export_import(self):
        self.run_commands("savesession one -host=one.example.com")
        exported = pathlib.Path(self.config) / "exported.ini"
        result = self.run_commands(f'sessions export "{exported}"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Exported 1 session", result.stdout)
        self.assertTrue(exported.is_file())

        result = self.run_commands("rmsession one")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_commands(f'sessions import "{exported}"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Imported 1 session", result.stdout)
        result = self.run_commands("sessions")
        self.assertIn("one.example.com", result.stdout)

    def test_savesession_rejects_unknown_switches(self):
        for args in ("demo", "demo -nonsense=1", ""):
            with self.subTest(args=args):
                result = self.run_commands("savesession " + args)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)

    def test_editor_options(self):
        result = self.run_commands("option editor vim; option editor")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("editor = vim", result.stdout)
        for value in ("abc", "0", "-5"):
            with self.subTest(value=value):
                result = self.run_commands("option editmaxsize " + value)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)
        result = self.run_commands("option editmaxsize 4096; option editmaxsize")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("editmaxsize = 4096", result.stdout)

    def test_command_server(self):
        # The graphical client talks to this: one command per line on
        # stdin, with the outcome of each one marked.
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "server.txt"
            script.write_text("echo first\necho second\nbogus-command\nexit\n",
                              encoding="utf-8")
            with open(script, "rb") as handle:
                result = subprocess.run([BINARY, "-server"], stdin=handle,
                                        capture_output=True, text=True, timeout=20,
                                        env=self.environment())
        self.assertEqual(result.returncode, 1)  # 'bogus-command' failed
        self.assertIn("LSCP-SERVER 1 ready", result.stdout)
        self.assertEqual(result.stdout.count("LSCP-RESULT"), 4)
        self.assertIn("LSCP-RESULT 0", result.stdout)
        self.assertIn("LSCP-RESULT 1", result.stdout)
        self.assertIn("Unknown command 'bogus-command'", result.stderr)

    def test_server_rejects_command_options(self):
        result = subprocess.run([BINARY, "-server", "-command=echo x"],
                                capture_output=True, text=True, timeout=10,
                                env=self.environment())
        self.assertEqual(result.returncode, 1)
        self.assertIn("-server", result.stderr)

    def test_local_listing(self):
        with tempfile.TemporaryDirectory(prefix="linuxscp script ") as tmp:
            root = pathlib.Path(tmp)
            (root / "hello world.txt").write_text("data")
            (root / "other.bin").write_bytes(b"data")
            (root / "directory").mkdir()
            result = self.run_commands(f'lls "{root}"')
            self.assertEqual(result.returncode, 0, result.stderr)
            # Upstream's columns: date, time, size (or <DIR>), then name.
            lines = result.stdout.splitlines()
            self.assertEqual(len(lines), 3)
            self.assertRegex(lines[0], r"\d{2}:\d{2} +<DIR> +directory$")
            self.assertRegex(lines[1], r"\d{2}:\d{2} +4 hello world\.txt$")
            self.assertRegex(lines[2], r"\d{2}:\d{2} +4 other\.bin$")
            result = self.run_commands(f'lls "{root / "*.txt"}"')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(result.stdout.splitlines()), 1)
            self.assertRegex(result.stdout,
                             r"\d{2}:\d{2} +4 hello world\.txt$")
            # A path that matches nothing is a no-match line, not a
            # failure - unless failonnomatch turns it into one.
            result = self.run_commands(f'lls "{root / "missing"}"')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("No files found matching", result.stdout)
            self.assertIn("No such file or directory", result.stdout)
            result = self.run_commands(f'lls "{root / "*.none"}"')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("No files found matching '*.none'", result.stdout)
            result = self.run_commands(
                f'option failonnomatch on; lls "{root / "missing"}"')
            self.assertEqual(result.returncode, 1)
            self.assertIn("No files found matching", result.stderr)

    def test_help_lists_commands_and_command_help(self):
        """'help' lists every command in upstream's name/description
        columns; 'help <command>' (aliases included) prints its help."""
        result = self.run_commands("help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"(?m)^ascii\s+Transfers files")
        self.assertRegex(result.stdout,
                         r"(?m)^session\s+Lists the open sessions")
        self.assertRegex(result.stdout, r"(?m)^attr\s+Prints path, size")
        result = self.run_commands("man")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"(?m)^ascii\s+Transfers files")
        result = self.run_commands("help rename")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("mv <source>", result.stdout)
        result = self.run_commands("help nosuchthing")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unknown command", result.stderr)

    def test_exit_and_bye_end_the_script(self):
        """exit and bye stop the script where they are, like upstream's
        continue flag: nothing after them runs."""
        for stopper in ("exit", "bye"):
            with self.subTest(stopper=stopper):
                result = self.run_commands(f"echo ONE; {stopper}; echo TWO")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("ONE", result.stdout)
                self.assertNotIn("TWO", result.stdout)

    def test_lcd_prints_where_it_went(self):
        """lcd reports the directory it changed into, like cd does."""
        with tempfile.TemporaryDirectory(prefix="linuxscp lcd ") as tmp:
            result = self.run_commands(f'lpwd; lcd "{tmp}"; lpwd; echo MARK')
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = result.stdout.splitlines()
            changed = lines[1]
            self.assertIn(changed, (str(pathlib.Path(tmp)),
                                    str(pathlib.Path(tmp).resolve())))
            self.assertEqual(lines[2], changed)
            self.assertIn("MARK", result.stdout)
            result = self.run_commands(f'lcd "{tmp}/nope-here"; echo AFTER')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("AFTER", result.stdout)

    def test_show_is_refused_with_the_reason(self):
        """'show' exists only to say that this build has no shell state:
        the connection runs the SFTP subsystem."""
        result = self.run_commands("show ; echo AFTER")
        self.assertEqual(result.returncode, 1)
        self.assertIn("SFTP subsystem", result.stderr)
        self.assertNotIn("AFTER", result.stdout)

    def test_exit_aliases_end_the_script(self):
        """quit and end stand for exit; '?' stands for help."""
        for stopper in ("quit", "end"):
            with self.subTest(stopper=stopper):
                result = self.run_commands(f"echo ONE; {stopper}; echo TWO")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("ONE", result.stdout)
                self.assertNotIn("TWO", result.stdout)
        result = self.run_commands("?")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"(?m)^ascii\s+Transfers files")

    def test_sync_alias_reaches_synchronize(self):
        """'sync' validates as synchronize does - an unknown switch is
        named as such rather than as an unknown command."""
        result = self.run_commands(
            "sync local -criteria=whenever ; echo AFTER")
        self.assertEqual(result.returncode, 1)
        self.assertIn("synchronize", result.stderr)
        self.assertNotIn("Unknown command", result.stderr)

    def test_session_and_close_need_a_connection(self):
        """Like upstream's CheckSession, 'session' and 'close' refuse to
        run before anything is open - argument checks happen after."""
        for command in ("session", "session 1", "close", "close 1",
                        "close 2"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo AFTER")
                self.assertEqual(result.returncode, 1)
                self.assertIn("Not connected", result.stderr)
                self.assertNotIn("AFTER", result.stdout)


    def test_xml_action_log(self):
        """'-xmllog=' writes a parseable log of what the script did."""
        import xml.etree.ElementTree as ET
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / "actions.xml"
            # 'batch continue' keeps the run going past the two failures, so
            # that the log holds several commands of each kind.
            result = subprocess.run(
                [BINARY, "-xmllog=" + str(log), "-batch",
                 "-command=option batch continue ; echo text ; "
                 "no-such-command ; open sftp://user:secret@127.0.0.1:1/"],
                capture_output=True, text=True, timeout=30,
                env=self.environment())
            self.assertTrue(log.exists(), result.stderr)
            root = ET.parse(log).getroot()
            self.assertEqual(root.tag, "log_session")
            self.assertIn("LinuxSCP", root.get("client", ""))
            self.assertTrue(root.get("created"))
            elements = [element.tag for element in root]
            self.assertIn("command", elements)
            self.assertIn("result", elements)
            lines = [element.get("line", "").strip() for element in root
                     if element.tag == "command"]
            self.assertEqual(len(lines), 4, lines)
            self.assertEqual(lines[1], "echo text")
            self.assertEqual(lines[2], "no-such-command")
            statuses = [element.get("status") for element in root
                        if element.tag == "result"]
            self.assertEqual(statuses, ["ok", "ok", "error", "error"])
            kinds = dict((element.get("type"), 0) for element in root
                         if element.tag == "message")
            self.assertEqual(sorted(kinds), ["error", "output"])
            messages = [element.text or "" for element in root
                        if element.tag == "message"]
            self.assertIn("text", "".join(messages))
            self.assertIn("Unknown command", "".join(messages))
            # A password must not be written to a file the user keeps.
            self.assertNotIn("secret", log.read_text(encoding="utf-8"))

    def test_xml_action_log_is_closed(self):
        """The log ends in one complete document, not in a half element."""
        import xml.etree.ElementTree as ET
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / "closed.xml"
            subprocess.run([BINARY, "-xmllog=" + str(log), "-batch",
                            "-command=echo one"],
                           capture_output=True, text=True, timeout=30,
                           env=self.environment())
            root = ET.parse(log).getroot()
            self.assertEqual([element.tag for element in root][-1], "result")

    def test_copyid_arguments(self):
        result = self.run_commands("copyid one two; echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Syntax: copyid", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_chmod_modes_are_parsed(self):
        # A symbolic mode is a mode: only the missing connection may be
        # complained about, and that happens after the arguments are checked.
        for command in ("chmod u+x file", "chmod a=r,go-w file",
                        "chmod 640 file -recursive", "chmod -recursive 700 file",
                        "chmod a+r file +x", "chmod a+r file +q"):
            with self.subTest(command=command):
                result = self.run_commands(command)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Wrong arguments", result.stderr)
                self.assertIn("Not connected", result.stderr)
        for command in ("chmod 999 file", "chmod u?q file", "chmod a+r",
                        "chmod , file", "chmod -recursive", "chmod 12345 file"):
            # Everything after the mode is a file name, so a stray '+q' is
            # not a syntax error; that case is in the accepted list above.
            with self.subTest(rejected=command):
                result = self.run_commands(command + "; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)


    def test_open_fails_on_unreachable_endpoint(self):
        """'open' has to report an endpoint that is not there.

        The curl-backed protocols have no connection of their own - every
        command runs its own transfer - so a dead port would otherwise be
        reported as a successful open and each command would fail on its own.
        Only the native engine is checked, because on Windows curl refuses
        'sftp' and 'scp' before it ever reaches the port (the curl.exe built
        into Windows has neither protocol), so a failure there would prove
        nothing about reachability. The classification of an endpoint that
        does answer is covered for both platforms by
        test_open_accepts_a_server_that_refuses_the_start_directory.
        """
        if os.name != "posix":
            self.skipTest("no SFTP-capable curl on this platform to test with")
        for protocol in ("sftp", "scp"):
            with self.subTest(protocol=protocol):
                result = self.run_commands(
                    f"open {protocol}://user:secret@127.0.0.1:1/ ; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)
                self.assertIn("Cannot open session", result.stderr)

    def test_open_accepts_a_server_that_refuses_the_start_directory(self):
        """Reaching the server is enough; a start directory it will not list
        is the command's problem, not the connection's. Without that
        distinction the reachability check would reject valid sessions."""
        import functools
        import http.server
        import threading
        with tempfile.TemporaryDirectory() as tmp:
            handler = functools.partial(http.server.SimpleHTTPRequestHandler,
                                        directory=tmp)
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
            port = server.server_address[1]
            threading.Thread(target=server.serve_forever, daemon=True).start()
            try:
                result = self.run_commands(
                    f"open http://127.0.0.1:{port}/missing-dir/ ; echo CONNECTED")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("CONNECTED", result.stdout)
            finally:
                server.shutdown()
                server.server_close()

    def test_transfer_presets_and_permission_switches(self):
        """Transfer presets end to end without a server: store, list,
        update, remove, activate through 'option', per-site storage, and
        the download form of '-permissions' plus '-rawtransfersettings'
        validation."""
        # store, list, update, remove
        result = self.run_commands(
            "savepreset asciitxt -transfer=ascii -preservetime=on "
            "-mask=*.txt; presets")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("transfer=ascii", result.stdout)
        self.assertIn("mask=*.txt", result.stdout)
        result = self.run_commands(
            "savepreset asciitxt -transfer=binary; presets")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("transfer=binary", result.stdout)
        self.assertNotIn("transfer=ascii", result.stdout)
        result = self.run_commands("rmspreset nosuchpreset")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no transfer preset named", result.stderr)

        # activate, show, clear; unknown names are refused loudly
        result = self.run_commands(
            "savepreset p1 -transfer=binary; option preset p1; option")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("preset = p1", result.stdout)
        result = self.run_commands("option preset nosuchpreset")
        self.assertEqual(result.returncode, 1)
        self.assertIn("There is no transfer preset named", result.stderr)
        result = self.run_commands("option preset p1; option preset off; option")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("preset = off", result.stdout)

        # a site stores and validates its preset
        result = self.run_commands(
            "savesession sx -host=host.example -preset=unknownpreset")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no transfer preset named", result.stderr)
        result = self.run_commands(
            "savepreset p1 -transfer=binary; "
            "savesession sx -host=host.example -preset=p1; sessions")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("(preset p1)", result.stdout)

        # '-permissions' without a value is understood on both directions
        # (the run stops at the missing connection, not at the switch)
        for command in ("get -permissions x /tmp/",
                        "put -nopermissions x /tmp/",
                        "put -permissions=640 x /tmp/"):
            with self.subTest(command=command):
                result = self.run_commands(command)
                self.assertEqual(result.returncode, 1, command)
                self.assertNotIn("Unsupported switch", result.stderr)
                self.assertNotIn("applies to uploads only", result.stderr)

        # -rawtransfersettings: the optional cap, upstream's own key
        # names, and honest errors for the settings this build lacks
        result = self.run_commands(
            "get -rawtransfersettings=abc x /tmp/")
        self.assertEqual(result.returncode, 1)
        self.assertIn("number of settings", result.stderr)
        result = self.run_commands(
            "get -rawtransfersettings=1 calculateSize=1024 x /tmp/")
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no equivalent", result.stderr)
        self.assertIn("calculateSize", result.stderr)
        result = self.run_commands(
            "get -rawtransfersettings=1 NoSuchKey=1 x /tmp/")
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not known to this build", result.stderr)
        result = self.run_commands(
            "get -rawtransfersettings=1 TransferMode=1 ResumeSupport=0 x /tmp/")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("raw transfer setting", result.stderr)
        self.assertIn("Not connected", result.stderr)

        # savepreset vocabulary errors name the setting at fault
        result = self.run_commands("savepreset bad -nosuchsetting=1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("nosuchsetting", result.stderr)
        result = self.run_commands("savepreset bad -speed=lots")
        self.assertEqual(result.returncode, 1)
        self.assertIn("speed", result.stderr)


    def test_rawsettings_refuse_a_setting_this_build_would_drop(self):
        """Upstream's '-rawsettings' names session settings. One this build
        does not apply is refused by name - with the names it does apply -
        before any connection is attempted, because a setting silently
        dropped is a session that connects somewhere else than asked."""
        result = self.run_commands(
            "open sftp://alice@example.com/ -rawsettings SFTPUploadQueue=2 ; "
            "echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1)
        self.assertIn("SFTPUploadQueue", result.stderr)
        self.assertIn("ProxyMethod", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_rawsettings_validate_their_values(self):
        cases = {
            "ProxyMethod=SOCKS9": "proxy method",
            "ProxyPort=99999": "proxy port",
            "ProxyDNS=maybe": "ProxyDNS",
            "ProxyLocalhost=perhaps": "ProxyLocalhost",
        }
        for setting, expected in cases.items():
            with self.subTest(setting=setting):
                result = self.run_commands(
                    "open sftp://alice@example.com/ -rawsettings %s ; "
                    "echo SHOULD_NOT_RUN" % setting)
                self.assertEqual(result.returncode, 1)
                self.assertIn(expected, result.stderr)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

        # A method that needs a host has to have one: the alternative is a
        # connection attempt that fails with a message about the network.
        result = self.run_commands(
            "open sftp://alice@example.com/ -rawsettings ProxyMethod=SOCKS5")
        self.assertEqual(result.returncode, 1)
        self.assertIn("ProxyHost", result.stderr)

        # '-rawsettings=N' takes exactly N parameters, the way upstream's
        # list switches do.
        result = self.run_commands(
            "open sftp://alice@example.com/ -rawsettings=2 ProxyHost=proxy")
        self.assertEqual(result.returncode, 1)
        self.assertIn("-rawsettings=2", result.stderr)

    def test_a_proxy_curl_cannot_speak_is_refused(self):
        """The curl-backed protocols translate SOCKS4, SOCKS5 and HTTP
        proxies into curl options. A telnet or local-command proxy has no
        curl option, and going direct instead would send the session
        somewhere it did not ask to go."""
        for method in ("Telnet", "Cmd"):
            with self.subTest(method=method):
                result = self.run_commands(
                    "open ftp://alice@example.com/ -rawsettings "
                    "ProxyMethod=%s ProxyHost=proxy.example.com "
                    "ProxyTelnetCommand=nc ; echo SHOULD_NOT_RUN" % method)
                self.assertEqual(result.returncode, 1)
                self.assertIn("curl", result.stderr)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_a_stored_session_keeps_its_proxy(self):
        """'savesession -rawsettings' stores the proxy with the site, its
        password encrypted like a session password, a re-save without the
        switch keeps it, and 'ProxyMethod=None' clears it."""
        result = self.run_commands(
            "savesession proxied -host=demo.example.com -username=alice "
            "-rawsettings ProxyMethod=SOCKS5 ProxyHost=proxy.example.com "
            "ProxyPort=1080 ProxyUsername=proxyuser "
            "ProxyPassword=proxy-secret")
        self.assertEqual(result.returncode, 0, result.stderr)
        stored = pathlib.Path(self.config) / "sessions.ini"
        text = stored.read_text(encoding="utf-8")
        self.assertIn("proxy_method=SOCKS5", text)
        self.assertIn("proxy_host=proxy.example.com", text)
        self.assertIn("proxy_port=1080", text)
        self.assertIn("proxy_username=proxyuser", text)
        self.assertIn("proxy_password=lscp1$", text)
        self.assertNotIn("proxy-secret", text)

        # A re-save that says nothing about the proxy keeps it, and keeps a
        # password value the file holds.
        result = self.run_commands(
            "savesession proxied -host=demo.example.com -username=alice")
        self.assertEqual(result.returncode, 0, result.stderr)
        text = stored.read_text(encoding="utf-8")
        self.assertIn("proxy_method=SOCKS5", text)
        self.assertIn("proxy_password=lscp1$", text)

        # The settings of a stored site are refused by name just the same.
        result = self.run_commands(
            "open proxied -rawsettings Nope=1 ; echo SHOULD_NOT_RUN")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Nope", result.stderr)
        self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

        result = self.run_commands(
            "savesession proxied -host=demo.example.com "
            "-rawsettings ProxyMethod=None")
        self.assertEqual(result.returncode, 0, result.stderr)
        text = stored.read_text(encoding="utf-8")
        self.assertNotIn("proxy_method", text)
        self.assertNotIn("proxy_password", text)

    def test_proxy_passwords_are_masked_in_logs(self):
        """A proxy password authenticates, so it is masked wherever the
        command is repeated: the echoed line, the session log and the XML
        action log."""
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / "session.log"
            actions = pathlib.Path(tmp) / "actions.xml"
            result = subprocess.run(
                [BINARY, "-batch", "-log=" + str(log),
                 "-xmllog=" + str(actions),
                 "-command=option echo on ; open sftp://alice@example.com/ "
                           "-rawsettings ProxyMethod=SOCKS5 "
                           "ProxyHost=127.0.0.1 ProxyPort=1 "
                           "ProxyPassword=proxy-secret ; exit"],
                capture_output=True, text=True, timeout=30,
                env=self.environment())
            self.assertNotEqual(result.returncode, 0, result.stdout)
            # The echoed line is masked, and so is the command the XML
            # action log records.
            self.assertIn("ProxyPassword=***", result.stdout)
            for path in (log, actions):
                text = path.read_text(encoding="utf-8", errors="replace")
                self.assertTrue(text.strip(), path)
                self.assertNotIn("proxy-secret", text)
            recorded = actions.read_text(encoding="utf-8", errors="replace")
            self.assertIn("ProxyPassword=***", recorded)


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
