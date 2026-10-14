# WinSCP parity audit and implementation plan

**Full feature parity is not achieved.** LinuxSCP is a substantially reduced,
cross-platform client using part of WinSCP's PuTTY code, not a build of the
complete upstream application. A protocol URL being accepted, a similar GUI,
or a passing build is not evidence of behavioral parity.

## Baseline and evidence

Audit date: **2026-10-13**, re-checked against the same upstream commit on
**2026-10-14**. Upstream: [winscp/winscp at
69edabbe2dc2c62661a7d3fc7e2396b5f42d504d](https://github.com/winscp/winscp/tree/69edabbe2dc2c62661a7d3fc7e2396b5f42d504d),
the `master` revision returned by GitHub during this audit. This is a development
snapshot, not a claim about a particular released WinSCP version.

The initial source review compared upstream `source/core/Script.cpp`, `Script.h`,
`CopyParam.h`, `Terminal.h` and the core/forms directory inventories with this
repository's `Script`, `Session`, `ProtocolClient`, build configuration and GUI.
Upstream has dedicated SCP, FTP, WebDAV, S3, configuration, queue and file-mask
engines, VCL desktop forms, and a `dotnet` assembly; this checkout does not port
those components. This is a **gap inventory**, not an exhaustive behavioral
certification or a completed security review.

[upstream-parity.json](upstream-parity.json) tracks the **40** command names
that upstream registers in its script implementation at that commit (aliases
and the internal `copyid` included), plus 15 feature areas, and lists the
LinuxSCP-specific commands separately under `linuxscp_additions` so that they
are not mistaken for upstream coverage. `partial` means some implementation
exists, not that every variant works. `missing` includes commands recognized
only to print an unsupported error.

No entry is certified `verified` yet: the checks below run on Linux only, and
`verified` would need the same evidence on every supported platform.

`edit`, `savesession`, `rmsession`, `sessions`, `show` and the extra aliases
`del`, `md`, `sync`, `quit` and `end` are in that second list. `edit` in
particular is a LinuxSCP command: the upstream *script interpreter* has none,
even though WinSCP edits files from its GUI and through its .NET assembly.

## Major gaps

| Area | Current implementation and remaining work |
| --- | --- |
| SFTP | Native PuTTY on POSIX, curl on Windows. SYMLINK, server-side copy-data (with a streamed fallback) and check-file-name (with a local hash fallback) are implemented and were exercised against a live in-process SFTP server; unknown extensions still fail explicitly. Remaining: metadata policies and Windows-side equivalence. |
| SCP | curl-dependent transfer path, not WinSCP's remote-shell filesystem engine. Listing and directory operations need a real SCP implementation. |
| FTP/FTPS | curl and a text listing parser. Verify server compatibility, MLSD, active/passive mode, TLS settings and resume semantics. |
| WebDAV | HTTP operations exist, but listing does not implement DAV PROPFIND/XML properties. Not a complete WebDAV filesystem. |
| S3 | URL maps to HTTPS, without AWS SigV4 signing or bucket/object listing. Authenticated Amazon S3 support is **not implemented**. |
| Transfers | Binary/ASCII/automatic transfers with full WinSCP file masks (name, directory and path masks plus size/time criteria), `option include/exclude`, `-filemask=`, `-transfer=`, fail-on-no-match, resume (binary only), and reconnect via `option reconnecttime`; ASCII conversion, `-speed=`, `-append`, `-resumesupport=` and `-preservetime[=all]` were verified against a live server. Missing: transfer presets, the `.filepart` temporary-name policy and atomic temporary-file policies in general. |
| Synchronization | A planner with previews (`-preview`, `SYNCPLAN`/`SYNCSUMMARY`), `-criteria=time\|size\|either\|none`, `-filemask=`, `-transfer=binary\|ascii\|automatic`, `-delete`/`-nodelete` (default from `option synchdelete`), `-mirror` (rejected with `both`), `-nonrecursive`, `-interval=`, `-count=`, `keepuptodate -continue` (keeps watching after a failed pass) and a state record that tells a change from a conflict; `-mirror` and ASCII conversion verified against a live server. Missing: path exclusions and a real watch mode: `keepuptodate` still repeats a full comparison on a timer. |
| Sessions | `sessions.ini` site manager shared by the console and the GUI: `session`, `savesession`, `rmsession`, `open <name>`, export/import and last-used tracking; the GUI holds one persistent connection per tab. Missing folder trees, raw settings, encrypted passwords and the full upstream configuration model. |
| Security | POSIX password/key/agent and host-key policy exist, and an unknown or changed key is reported on a `HOSTKEY` line instead of being accepted. GSSAPI is compiled out; proxy/tunnel configuration and curl fingerprint equivalence are incomplete. The GUI verifies by default, pins on request, stores a pin rather than a wildcard, and serves its API on loopback behind a per-run token with a `Host` check. Still no user account, encrypted credential store or upstream key-management UI. |
| GUI | Browser Commander with one persistent session per tab, dual panes, a transfer queue with progress/pause/cancel/retry, synchronization preview, remote search, a remote editor, the site manager and host-key pinning, driven by a loopback HTTP API that requires a per-run token and checks the `Host` header; the client has a jsdom regression test. Still not upstream Commander/Explorer: no drag and drop, custom commands, synchronized browsing or localization. |
| Editing/search | `edit` downloads, runs the configured editor and uploads on change, with a size limit (`option editmaxsize`) and a refusal when the remote file changed meanwhile; remote search walks directories by name mask. Missing save/upload monitoring of local edits, full-text search, encoding handling and the custom command system. |
| Automation | CLI scripting with WinSCP switches, unambiguous command and option abbreviations, script parameters, `%TIMESTAMP%` and environment expansion, and an XML action log (`-xmllog=`) with masked passwords. Missing: the .NET assembly and PowerShell APIs, and log-format equivalence - our log mirrors upstream's structure but does not copy a schema, since upstream has no XML session log to copy. |
| Integration | Debian launcher/icons and external SSH terminal, not full desktop drag/drop, shell integration and extensions. Windows-only mechanisms need equivalent workflows, not silently skipped requirements. |
| Localization/accessibility | No upstream translation catalog or equivalent accessibility validation. |
| Platforms | Linux/Windows builds exist; each protocol depends on platform/backend capabilities. Windows in-box curl cannot be assumed to support SSH. |

## Scripting changes made with this audit

* `-script` and `-command` default to aborting after a command reports an error.
  `option batch abort` and `on` stop subsequent commands; `continue` and `off`
  continue, but retain a nonzero process exit status when errors occurred.
  Noninteractive modes suppress prompts. This does **not** yet implement
  upstream's full per-file error/prompt policy inside a multi-file command.
* Added `man`, `recv`, `mget`, `send`, `mput` dispatch aliases, `binary`,
  `ascii` (both set `option transfer`), `lls [directory[/mask]]` with local
  enumeration, and per-command `help`/`man` syntax lines.
* Implemented WinSCP file masks (`-filemask=`, `option include/exclude`) with
  name/directory/path masks and size/time criteria, plus `-transfer=binary|
  ascii|automatic` conversion (CRLF/LF) around every transfer, including
  planned synchronizations. Resuming stays binary-only and is rejected
  explicitly when combined with ASCII.
* New remote commands, each verified against a live in-process SFTP server:
  `cp` (server-side copy-data with streamed fallback, recursive),
  `ln`/`symlink` (SFTP SYMLINK), `checksum <alg> <file>` (server-side
  check-file-name first, local sha-256/sha-1/md5/crc32 through the PuTTY core
  as fallback, printed as `<hash> <file>`), and `call`/`!` (SSH exec in a
  one-shot second connection, leaving the SFTP session open).
* Command semantics completed: `cd` with no argument goes home and prints the
  directory, `ls` takes its mask from the trailing path component, `rm`
  supports `-onlyfile`, `rmdir`/`mv` take multiple arguments (with an existing
  directory as the `mv` target), and `option` lists all settings when called
  without a name (echo, batch, confirm, resume, transfer, include/exclude,
  failonnomatch, synchdelete, reconnecttime, editor, editmaxsize).
* Unsupported transfer switches and option values still fail before any
  connection or transfer instead of being ignored or treated as paths; prefix
  filenames beginning with `-` with `./`. `option reconnecttime <seconds>|off`
  now really reconnects a dropped session instead of reporting success.
* Synchronization validates the direction, argument count and switches before
  use; `both -delete` is rejected rather than risking destructive sequential
  reconciliation, and `-mirror` is rejected with `both`.

* `copyid` now installs the public half of the session key (or of a key file
  given to it) in the server's `~/.ssh/authorized_keys`, creating `~/.ssh`
  `0700` and the file `0600` and leaving an already installed key untouched.
  OpenSSH public keys, OpenSSH private keys and PuTTY `.ppk` files are read
  through the bundled PuTTY core; an encrypted key is asked for
  interactively and refused in batch mode. The Windows build states that it
  cannot read key files rather than doing nothing.
* `get`, `put` and their aliases gained the remaining upstream transfer
  switches: `-speed=<KB/s>`, `-append`, `-resumesupport=on|off|<KB>`,
  `-neweronly`/`-noneweronly` and `-preservetime`/`-preservetime=all` (the
  value form also stamps directories). `-append` needs the native SFTP
  transport and cannot be combined with `-resume`; both are refused on the
  curl backends instead of silently becoming an overwrite.
* `chmod` accepts symbolic modes (`u+x`, `a=r,go-w`, `u+s`, `X`) as well as
  octal ones, and `-recursive` walks a remote directory tree without
  following symbolic links. Upstream's script command passes an octal string
  only, so this is deliberately a little beyond upstream rather than a copy
  of it.
* Command, option and value names are resolved by unambiguous prefix, as
  upstream's command table does (`synch local`, `option bat cont`); an
  ambiguous prefix names the candidates and is not guessed at. The
  dispatcher and `kCommandNames` in `Script.cpp` are now checked against each
  other by `tools/check_upstream_parity.py`, because abbreviation resolution
  is only correct while the two agree.
* `-parameter` feeds `%1%`..`%9%`, and `%TIMESTAMP%` (with an optional signed
  offset and format) and `%NAME%` environment variables are expanded on
  script and interactive lines before dispatch; text in single quotes is left
  alone, and a malformed timestamp placeholder fails instead of reaching the
  server with a `%` in its path. `-server` input is not expanded: the
  graphical client sends literal paths.
* `-xmllog=FILE` writes an XML action log (`log_session` holding `command`,
  `result`, `message`, `session` and `transfer` elements in time order). It
  is an equivalent record, not an upstream format; passwords in site URLs
  and in `-password=` switches are masked before anything is written, also
  under `option echo on`.

These intentional fail-closed changes can break scripts that previously relied
on silently ignored settings. Review those scripts; do not remove a safety
filter just to make a transfer proceed.

## Reproduce the checks

```sh
make -C source/linux all check
python3 source/linux/tests/ScriptTest.py source/linux/build/linuxscp
python3 tools/check_upstream_parity.py
```

The inventory check compares three things: every documented command has a
dispatcher entry, every dispatched command is in the abbreviation table
`kCommandNames` (and vice versa, so an abbreviation can never resolve to a
command that then reports 'Unknown command'), and every entry has a status and
an explanation. It does not test behavior.

An optional live check runs an in-process SFTP server (Python `paramiko`,
`pip install --user paramiko`) and exercises `cd`/`ls` masks, `cp`, `ln`,
`checksum`, `call`, `mv`, `rmdir`, `rm -onlyfile`, ASCII transfers,
`synchronize -mirror -transfer=`, `chmod` (octal, symbolic and `-recursive`),
`copyid`, `-speed=` (timed), `-append`, `-resumesupport=` and
`-preservetime[=all]` against a real connection. The harness implements
`chattr` for `SETSTAT` - paramiko routes that request there, not to
`setstat` - which is what makes `chmod` and the timestamp handling testable
against it at all:

```sh
python3 source/linux/tests/TestSftpServer.py &   # listens on 127.0.0.1:12765
# testuser/testpass, /tmp/lscp-sftp-root as the visible root
```

The Python CLI regression suite exercises default/explicit batch policies,
file and command input, alias routing, local listing, binary mode, rejected
no-op switches/options and synchronization validation without a remote server.
CMake registers it with CTest when Python 3 is available. CI runs the inventory
and regression checks. A separate weekly/manual workflow compares command
registrations with the latest upstream `master` to flag inventory drift. It does
not silently change the pinned baseline or certify parity. These tests do not
establish protocol/GUI parity.

To check the inventory against an upstream checkout (at the pinned revision or
a later candidate), without requiring network access in the checker:

```sh
python3 tools/check_upstream_parity.py \
  --upstream-script /path/to/winscp/source/core/Script.cpp \
  --upstream-header /path/to/winscp/source/core/Script.h
```

New or removed command registrations cause failure until reviewed, and the
check against the pinned commit currently passes: `linuxscp_additions` holds
the names upstream does not register. Record the new revision in the manifest
when updating the baseline. The offline default
checks documented implemented commands against the local dispatcher and
validates statuses; it does not download upstream or test behavior.

`python3 tools/check_upstream_parity.py --require-full` is an opt-in release gate
and **currently fails**, because gaps remain. A green inventory check must
never be advertised as full parity. A future `verified` status needs reproducible
behavioral evidence on each supported platform, not merely a dispatcher entry.

## Prioritized completion plan

1. **Safety and automation:** host keys are verified and pinned rather than
   accepted, and the GUI API is loopback-only behind a per-run token; file
   masks, ASCII/automatic conversion, reconnect and a live-server test harness
   are in place. What is left is a user account or encrypted credential store,
   `copyid`'s authorized_keys editing, batch per-file semantics, parameter
   expansion and atomic transfers, plus live-server tests for failed opens,
   denied operations and interruption/data integrity.
2. **Real protocol coverage:** replace generic listing approximations with
   protocol-specific SCP, WebDAV and signed S3 clients; expand FTP/SFTP handling.
   Test CRUD, directory enumeration, metadata, authentication and resume against
   controlled servers on Linux and Windows.
3. **Synchronization:** the planner exists with previews, criteria, filtering,
   `-mirror`, `-transfer=` conversion, deletion guards and `keepuptodate
   -continue`; remaining work is path exclusions, a conflict resolution
   workflow and a real file-system watch mode behind `keepuptodate`.
   `-resumesupport=` thresholds and `-append` are in place for the native
   transport, with the curl backends refusing them explicitly.
4. **Session and desktop capabilities:** session folders, raw settings and
   encrypted passwords; queue priorities and transfer presets (speed limiting
   and reconnect exist); local-side
   editing, full-text search and custom commands; Commander/Explorer workflows,
   drag and drop, accessibility and translations, with GUI workflow tests.
5. **External compatibility:** the remaining upstream script commands and
   their exact output text, structured-log field equivalence, automation API
   equivalents and platform integrations. Maintain explicit
   exclusions only if product scope is intentionally narrowed; exclusions do
   not qualify as full upstream parity.

Completion requires a reviewed, exhaustive feature inventory, differential
script/behavior tests against WinSCP where possible, protocol integration tests,
GUI workflow tests and platform-specific acceptance tests. This audit and the
initial scripting fixes are a starting point, not completion of that project.
