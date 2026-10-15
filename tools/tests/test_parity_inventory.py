"""Validate the inventory checker, including intentionally failing gates."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from check_upstream_parity import check_verification, upstream_commands


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


    def ledger_errors(self, entries, platforms=None, policy="cite evidence"):
        """Run just the verification rules over a fabricated inventory.

        The fabricated ledger has two platforms on purpose: LinSCP ships on
        Linux alone, but the rule under test is the general one - an entry
        called verified has to cite every platform the ledger names - and a
        single-platform fixture could not tell that rule apart from 'cite
        anything'.
        """
        if platforms is None:
            platforms = {
                "linux": {"how": "make check", "date": "2026-10-14",
                          "suites": {"unit": "the C++ checks",
                                     "manual": "by hand, with the commands"}},
                "bsd": {"how": "ctest", "date": "2026-10-14",
                        "suites": {"unit": "the C++ checks"}},
            }
        errors = []
        check_verification({"verification": {"policy": policy,
                                             "platforms": platforms}},
                           entries, errors)
        return errors

    def test_verified_needs_evidence_from_every_platform(self):
        entry = {"name": "get", "status": "verified", "notes": "works"}
        self.assertTrue(any("cites no evidence" in e for e in
                            self.ledger_errors([entry])))
        entry["verified_on"] = {"linux": ["unit"]}
        errors = self.ledger_errors([entry])
        self.assertTrue(any("without evidence from: bsd" in e for e in errors))
        entry["verified_on"]["bsd"] = ["unit"]
        self.assertEqual(self.ledger_errors([entry]), [])

    def test_cited_suites_have_to_exist(self):
        """'manual' evidence exists on Linux only, so citing it for the second
        platform of the fabricated ledger is a claim nobody can reproduce."""
        entry = {"name": "put", "status": "partial", "notes": "works",
                 "verified_on": {"linux": ["unit"], "bsd": ["manual"]}}
        errors = self.ledger_errors([entry])
        self.assertTrue(any("'bsd' has no suite 'manual'" in e for e in errors))

        entry["verified_on"] = {"linux": ["unit"], "bsd": []}
        self.assertTrue(any("names no suite" in e for e in self.ledger_errors([entry])))
        entry["verified_on"] = {"linux": ["unit"], "beos": ["unit"]}
        self.assertTrue(any("unknown platform" in e for e in self.ledger_errors([entry])))

    def test_an_evidence_ledger_has_to_be_readable(self):
        """A platform that does not say how it produces evidence cannot be
        cited, so the record itself is checked."""
        errors = self.ledger_errors([], platforms={
            "linux": {"how": "", "date": "14/10/2026", "suites": {}}})
        self.assertTrue(any("'linux' has no 'how'" in e for e in errors), errors)
        self.assertTrue(any("is not YYYY-MM-DD" in e for e in errors), errors)
        self.assertTrue(any("lists no test suites" in e for e in errors), errors)

        errors = self.ledger_errors([], platforms={
            "linux": {"how": "make check", "date": "2026-10-14",
                      "suites": {"unit": ""}}})
        self.assertTrue(any("is not described" in e for e in errors), errors)

        self.assertTrue(any("'verification.policy' is empty" in e for e in
                            self.ledger_errors([], policy="")),
                        "a ledger with no policy is not a rule")
        self.assertTrue(any("no 'verification' block" in e for e in
                            self.ledger_errors_no_block()),
                        "a missing ledger has to be an error, not a pass")

    def ledger_errors_no_block(self):
        errors = []
        check_verification({}, [{"name": "get", "status": "verified",
                                 "notes": "works"}], errors)
        return errors

    def test_the_real_ledger_is_consistent(self):
        """The shipped inventory has to satisfy its own rules, and the evidence
        it claims has to be evidence the platform it ships on produces."""
        inventory = json.loads((ROOT / "docs/upstream-parity.json").read_text())
        entries = (list(inventory["commands"].values()) + inventory["features"] +
                   inventory.get("linuxscp_additions", []))
        errors = []
        check_verification(inventory, entries, errors)
        self.assertEqual(errors, [])
        # LinSCP is a Linux application: the ledger names exactly that
        # platform, so no entry can cite evidence from a host this client is
        # not built for.
        platforms = inventory["verification"]["platforms"]
        self.assertEqual(sorted(platforms), ["linux"])
        cited = [e for e in entries if e.get("verified_on")]
        self.assertTrue(cited, "no entry records any evidence at all")
        for entry in cited:
            self.assertEqual(sorted(entry["verified_on"]), ["linux"],
                             str(entry.get("name")) + ": cites a platform the "
                             "ledger does not ship on")
        # Promotion is allowed only when every aspect of the claim is
        # accounted for: the platform the ledger ships on contributes at
        # least one suite that actually ran the behaviours the notes
        # describe, the notes say which suites those are, and no aspect
        # of the entry is still listed as missing inside the claim.
        verified = [e for e in entries if e["status"] == "verified"]
        for entry in verified:
            label = entry.get("name") or "?"
            cited = entry.get("verified_on")
            self.assertTrue(cited, label + ": verified with no evidence at all")
            self.assertEqual(sorted(cited), sorted(platforms),
                             label + ": a platform the ledger ships on has "
                             "no evidence for this entry")
            for platform, suites in cited.items():
                self.assertTrue(suites,
                                label + ": " + platform + " cites no suite")
            self.assertRegex(
                entry["notes"], r"(?i)covered by|covered on",
                label + ": the notes never say which evidence covers which "
                "behaviour")
            self.assertNotIn(
                "Missing:", entry["notes"],
                label + ": a verified claim still lists a missing aspect")


if __name__ == "__main__":
    unittest.main()
