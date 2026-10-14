"""Server-free CLI regression tests, shared by Linux and Windows builds."""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


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
        for command in ("option reconnect 10", "option batch garbage",
                        "option transfer nonsense", "option reconnecttime nope",
                        "option include *.txt|bad|mask"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

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
            for switch in ("-resumegarbage", "-resume=off", "-speed=10",
                           "-unknown", "-latest", "-criteria=time",
                           "-transfer=garbage", "-filemask=",
                           "-filemask=*.txt|*.a|*.b", "-permissions=999"):
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
                           "-transfer=automatic"):
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
        result = self.run_commands("session")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no stored sessions", result.stdout)

        result = self.run_commands(
            "savesession demo -host=demo.example.com -username=alice -port=2222")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Stored session 'demo'", result.stdout)

        result = self.run_commands("session")
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
        result = self.run_commands("session")
        self.assertIn("no stored sessions", result.stdout)

    def test_session_export_import(self):
        self.run_commands("savesession one -host=one.example.com")
        exported = pathlib.Path(self.config) / "exported.ini"
        result = self.run_commands(f'session export "{exported}"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Exported 1 session", result.stdout)
        self.assertTrue(exported.is_file())

        result = self.run_commands("rmsession one")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_commands(f'session import "{exported}"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Imported 1 session", result.stdout)
        result = self.run_commands("session")
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
            self.assertEqual(result.stdout.splitlines(),
                             ["directory", "hello world.txt", "other.bin"])
            result = self.run_commands(f'lls "{root / "*.txt"}"')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["hello world.txt"])
            result = self.run_commands(f'lls "{root / "missing"}"')
            self.assertEqual(result.returncode, 1)

    def test_help_alias(self):
        result = self.run_commands("man")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Commands:", result.stdout)


if __name__ == "__main__":
    unittest.main()
