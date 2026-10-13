"""Validate the inventory checker, including intentionally failing gates."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from check_upstream_parity import upstream_commands


class InventoryTests(unittest.TestCase):
    def test_literal_and_constant_registrations(self):
        script = 'FCommands->Register(L"get", x); FCommands->Register(EXIT_COMMAND, x);'
        header = '#define EXIT_COMMAND L"exit"'
        self.assertEqual(upstream_commands(script, header), {"get", "exit"})

    def test_fail_if_source_not_recognized(self):
        with self.assertRaises(ValueError):
            upstream_commands("not a script source file", "")
        with self.assertRaises(KeyError):
            upstream_commands("FCommands->Register(NEW_COMMAND, x);", "")

    def test_full_gate_fails_for_current_gaps(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/check_upstream_parity.py"),
                                 "--require-full"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Full parity gate failed", result.stderr)

    def test_command_drift_detection(self):
        names = json.loads((ROOT / "docs/upstream-parity.json").read_text())["commands"]
        script = "\n".join(f'FCommands->Register(L"{name}", x);' for name in names)
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "Script.cpp"
            header = pathlib.Path(tmp) / "Script.h"
            header.write_text("")
            args = [sys.executable, str(ROOT / "tools/check_upstream_parity.py"),
                    "--upstream-script", str(path), "--upstream-header", str(header)]
            path.write_text(script)
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            path.write_text(script + '\nFCommands->Register(L"newcommand", x);')
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Untracked upstream command: newcommand", result.stderr)
            path.write_text(script.replace('FCommands->Register(L"get", x);', ''))
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Command no longer registered upstream: get", result.stderr)

    def test_requires_both_files(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/check_upstream_parity.py"),
                                 "--upstream-script", "unused.cpp"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Supply both", result.stderr)


if __name__ == "__main__":
    unittest.main()
