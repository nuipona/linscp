"""Server-free CLI regression tests, shared by Linux and Windows builds."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class ScriptTests(unittest.TestCase):
    def run_commands(self, text, file=False):
        if file:
            with tempfile.TemporaryDirectory() as tmp:
                path = pathlib.Path(tmp) / "commands.txt"
                path.write_text(text.replace(";", "\n"), encoding="utf-8")
                return subprocess.run([BINARY, "-script=" + str(path)],
                                      capture_output=True, text=True, timeout=10)
        return subprocess.run([BINARY, "-command=" + text],
                              capture_output=True, text=True, timeout=10)

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
        for command in ("option reconnect 10", "option reconnecttime 10",
                        "option transfer ascii", "option transfer automatic", "ascii",
                        "option batch garbage"):
            with self.subTest(command=command):
                result = self.run_commands(command + "; echo SHOULD_NOT_RUN")
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("SHOULD_NOT_RUN", result.stdout)

    def test_binary(self):
        result = self.run_commands("option transfer binary; binary; echo OK")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK", result.stdout)

    def test_reject_transfer_switches_before_connection(self):
        for command in ("get", "recv", "mget", "put", "send", "mput"):
            for switch in ("-filemask=*.txt", "-transfer=ascii", "-transfer=automatic",
                           "-resumegarbage", "-resume=off", "-speed=10", "-unknown"):
                with self.subTest(command=command, switch=switch):
                    result = self.run_commands(f"{command} {switch} source destination")
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("Unsupported switch", result.stderr)
                    self.assertNotIn("Not connected", result.stderr)

    def test_transfer_aliases_dispatch(self):
        for command in ("get", "recv", "mget", "put", "send", "mput"):
            with self.subTest(command=command):
                result = self.run_commands(f"{command} -transfer=binary source destination")
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Unknown command", result.stderr)
                self.assertNotIn("Unsupported switch", result.stderr)

    def test_sync_validation(self):
        for args in ("both -delete", "local -criteria=size", "remote -mirror",
                     "local -filemask=*.txt", "sideways", "local a b c", ""):
            with self.subTest(args=args):
                result = self.run_commands("synchronize " + args)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Wrong arguments", result.stderr)

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
