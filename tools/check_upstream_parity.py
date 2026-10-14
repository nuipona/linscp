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
    gaps = sum(entry["status"] != "verified" for entry in entries)
    additions = len(inventory.get("linuxscp_additions", []))
    print(f"Upstream {inventory['upstream']['commit']}: "
          f"{len(commands)} commands, {additions} LinuxSCP-specific entries, "
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
