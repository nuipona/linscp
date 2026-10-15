#!/usr/bin/env python3
"""Offline safety checks for local Commander view/edit support."""

import os
import pathlib
import sys
import tempfile
import types
import unittest

GUI_DIR = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_DIR))
import linuxscp_gui as gui  # noqa: E402


class LocalEditorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="linscp-local-edit-")
        self.root = pathlib.Path(self.temporary.name)
        self.path = self.root / "notes.txt"
        self.path.write_text("first line\n", encoding="utf-8")
        self.tab = types.SimpleNamespace(local_dir=str(self.root))

    def tearDown(self):
        self.temporary.cleanup()

    def test_read_and_atomic_save_round_trip(self):
        content, info, error = gui.read_local_text(str(self.path))
        self.assertEqual(error, "")
        self.assertEqual(content, "first line\n")
        self.assertGreater(info["mtime_ns"], 0)

        ok, error, saved = gui.write_local_text(
            str(self.path), "updated line\n", info["mtime_ns"])
        self.assertTrue(ok, error)
        self.assertEqual(self.path.read_text(encoding="utf-8"), "updated line\n")
        self.assertEqual(saved["size"], len("updated line\n"))

    def test_save_refuses_a_stale_editor_version(self):
        _, info, error = gui.read_local_text(str(self.path))
        self.assertEqual(error, "")
        self.path.write_text("changed elsewhere\n", encoding="utf-8")
        os.utime(self.path, ns=(info["mtime_ns"] + 2_000_000_000,
                                info["mtime_ns"] + 2_000_000_000))
        ok, message, _ = gui.write_local_text(
            str(self.path), "my stale edit\n", info["mtime_ns"])
        self.assertFalse(ok)
        self.assertIn("changed while it was open", message)
        self.assertEqual(self.path.read_text(encoding="utf-8"), "changed elsewhere\n")

    def test_view_rejects_binary_and_oversized_files(self):
        binary = self.root / "binary.dat"
        binary.write_bytes(b"text\x00binary")
        content, _, error = gui.read_local_text(str(binary))
        self.assertIsNone(content)
        self.assertIn("binary", error)

        large = self.root / "large.txt"
        large.write_bytes(b"x" * 32)
        content, _, error = gui.read_local_text(str(large), max_size=16)
        self.assertIsNone(content)
        self.assertIn("too large", error)

    def test_edit_path_is_limited_to_the_active_local_panel(self):
        resolved, error = gui.resolve_local_file(self.tab, str(self.path))
        self.assertEqual(error, "")
        self.assertEqual(resolved, str(self.path))

        outside = self.root.parent / (self.root.name + "-outside.txt")
        resolved, error = gui.resolve_local_file(self.tab, str(outside))
        self.assertEqual(resolved, "")
        self.assertIn("outside the current local panel", error)

    def test_symbolic_link_cannot_be_overwritten_by_editor(self):
        target = self.root / "target.txt"
        target.write_text("target\n", encoding="utf-8")
        link = self.root / "link.txt"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            self.skipTest("symbolic links are not available on this platform")
        _, info, error = gui.read_local_text(str(link))
        self.assertEqual(error, "")
        ok, message, _ = gui.write_local_text(str(link), "new\n", info["mtime_ns"])
        self.assertFalse(ok)
        self.assertIn("symbolic link", message)
        self.assertEqual(target.read_text(encoding="utf-8"), "target\n")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LocalEditorTests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    failed = len(result.failures) + len(result.errors)
    print("evidence: suite=gui-local-editor platform=%s ran=%d skipped=%d failed=%d" %
          ("windows" if os.name == "nt" else "linux", result.testsRun,
           len(result.skipped), failed))
    sys.exit(0 if result.wasSuccessful() else 1)
