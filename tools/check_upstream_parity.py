#!/usr/bin/env python3
"""Check parity inventory completeness, NOT behavioral equivalence.

By default this is offline. To detect upstream command drift supply upstream
source/core/Script.cpp and Script.h from the same commit. --require-full is an
explicit release gate that fails while any tracked gap remains.
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def upstream_commands(script, header):
    constants = dict(re.findall(r'#define\s+(\w+_COMMAND)\s+L"([^"]+)"', header))
    tokens = re.findall(r'FCommands->Register\(\s*(L"[^"]+"|\w+)', script)
    if not tokens:
        raise ValueError("No upstream command registrations found")
    return {token[2:-1] if token.startswith('L"') else constants[token]
            for token in tokens}


def command_table(source):
    """The names the interpreter can abbreviate to, in Script.cpp."""
    block = re.search(r"const char \* const kCommandNames\[\] = \{(.*?)\};",
                      source, re.S)
    if not block:
        raise ValueError("No kCommandNames table in source/linux/Script.cpp")
    return set(re.findall(r'"([^"]+)"', block.group(1)))


VERIFICATION_DATE = re.compile(r"\d{4}-\d{2}-\d{2}")


def check_verification(inventory, entries, errors):
    """'verified' is a claim about behavior, so it has to cite its evidence.

    A status in this inventory says what the code does, not what somebody
    hoped. An entry may therefore only be called 'verified' when it names the
    platforms whose test evidence covers it, and every platform in the ledger
    has to say how that evidence is produced and what it can prove. Anything
    that cannot be reproduced is not evidence, so the suites an entry cites
    have to be suites the platform actually has.
    """
    verification = inventory.get("verification")
    if not isinstance(verification, dict):
        errors.append("Inventory has no 'verification' block, so no entry can "
                      "claim to be verified: add how each platform produces "
                      "its evidence")
        return None
    if not str(verification.get("policy", "")).strip():
        errors.append("'verification.policy' is empty: say what an entry has "
                      "to show before it may be called verified")
    platforms = verification.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        errors.append("'verification.platforms' is empty: there is no "
                      "evidence to cite, so nothing may be verified")
        return None
    suites = {}
    for name, record in sorted(platforms.items()):
        for field in ("how", "date"):
            if not str(record.get(field, "")).strip():
                errors.append(f"Verification record for '{name}' has no '{field}'")
        date = str(record.get("date", ""))
        if date and not VERIFICATION_DATE.fullmatch(date):
            errors.append(f"Verification date for '{name}' is not YYYY-MM-DD: {date}")
        listed = record.get("suites")
        if not isinstance(listed, dict) or not listed:
            errors.append(f"Verification record for '{name}' lists no test "
                          "suites, so no entry can cite evidence from it")
            listed = {}
        for suite, description in sorted(listed.items()):
            if not str(description).strip():
                errors.append(f"'{name}' suite '{suite}' is not described: "
                              "say what it proves")
        suites[name] = set(listed)
    covered = {name: 0 for name in suites}
    for entry in entries:
        label = entry.get("name", entry.get("command", "?"))
        claimed = entry.get("verified_on")
        if entry["status"] == "verified" and not claimed:
            errors.append(f"{label} is verified but cites no evidence: name "
                          "the platform and the suites that checked it")
        if claimed is None:
            continue
        if not isinstance(claimed, dict) or not claimed:
            errors.append(f"{label} has an empty 'verified_on': cite the "
                          "platform and the suites, or record nothing")
            continue
        for platform, cited in sorted(claimed.items()):
            if platform not in suites:
                errors.append(f"'verified_on' names an unknown platform: {platform}")
                continue
            if not isinstance(cited, list) or not cited:
                errors.append(f"'verified_on.{platform}' names no suite: "
                              "evidence has to point at a test")
                continue
            unknown = set(cited) - suites[platform]
            for suite in sorted(unknown):
                errors.append(f"'{platform}' has no suite '{suite}' to cite "
                              f"for {label}")
            covered[platform] += 1
        if entry["status"] == "verified" and set(claimed) != set(suites):
            missing = sorted(set(suites) - set(claimed))
            errors.append(f"{label} is verified without evidence from: "
                          + ", ".join(missing))
    return {"covered": covered,
            "verified": sum(1 for e in entries if e["status"] == "verified"),
            "entries": len(entries)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-script", type=pathlib.Path)
    parser.add_argument("--upstream-header", type=pathlib.Path)
    parser.add_argument("--require-full", action="store_true")
    args = parser.parse_args()
    inventory = json.loads((ROOT / "docs/upstream-parity.json").read_text())
    commands = inventory["commands"]
    entries = (list(commands.values()) + inventory["features"] +
               inventory.get("linuxscp_additions", []))
    errors = []
    for entry in entries:
        if entry["status"] not in {"partial", "missing", "verified"} or not entry["notes"]:
            errors.append("Invalid or unexplained status: " + repr(entry))
    source = (ROOT / "source/linux/Script.cpp").read_text()
    dispatched = set(re.findall(r'command == "([^"]+)"', source))
    for command, entry in commands.items():
        if entry["status"] != "missing" and command not in dispatched:
            errors.append(f"Documented command has no dispatcher: {command}")
    # Abbreviations are resolved against kCommandNames, so that list and the
    # dispatcher have to agree: an unlisted command cannot be abbreviated to,
    # and a listed one that is not dispatched resolves to an error message.
    table = command_table(source)
    for name in sorted(table - dispatched):
        errors.append(f"Abbreviatable command is not dispatched: {name}")
    for name in sorted(dispatched - table):
        errors.append(f"Dispatched command is not in kCommandNames: {name}")
    if bool(args.upstream_script) != bool(args.upstream_header):
        errors.append("Supply both --upstream-script and --upstream-header")
    elif args.upstream_script:
        actual = upstream_commands(args.upstream_script.read_text(), args.upstream_header.read_text())
        for command in sorted(actual - commands.keys()):
            errors.append(f"Untracked upstream command: {command}")
        for command in sorted(commands.keys() - actual):
            errors.append(f"Command no longer registered upstream: {command}")
    evidence = check_verification(inventory, entries, errors)
    gaps = sum(entry["status"] != "verified" for entry in entries)
    additions = len(inventory.get("linuxscp_additions", []))
    if evidence:
        summary = ", ".join(f"{name}: {count}"
                            for name, count in sorted(evidence["covered"].items()))
        print(f"Evidence recorded per platform ({summary}); "
              f"{evidence['verified']} of {evidence['entries']} entries verified.")
    print(f"Upstream {inventory['upstream']['commit']}: "
          f"{len(commands)} commands, {additions} LinSCP-specific entries, "
          f"{len(inventory['features'])} feature areas, {gaps} tracked gaps. "
          "Inventory validation is not a parity certification.")
    if args.require_full and gaps:
        errors.append("Full parity gate failed: tracked gaps remain")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyError, ValueError, OSError) as exc:
        print(f"Parity inventory check failed: {exc}", file=sys.stderr)
        sys.exit(1)
