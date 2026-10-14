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
