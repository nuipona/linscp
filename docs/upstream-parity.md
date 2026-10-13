# WinSCP parity audit and implementation plan

**Full feature parity is not achieved.** LinuxSCP is a substantially reduced,
cross-platform client using part of WinSCP's PuTTY code, not a build of the
complete upstream application. A protocol URL being accepted, a similar GUI,
or a passing build is not evidence of behavioral parity.

## Baseline and evidence

Audit date: **2026-10-13**. Upstream: [winscp/winscp at
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

[upstream-parity.json](upstream-parity.json) tracks all **40** command
registrations in that upstream script implementation (including aliases and
internal `copyid`), plus 15 feature areas. `partial` means some implementation
exists, not that every variant works. `missing` includes commands recognized
only to print an unsupported error. No entry is certified `verified` yet.
LinuxSCP-specific aliases and commands are not counted as upstream coverage.

## Major gaps

| Area | Current implementation and remaining work |
| --- | --- |
| SFTP | Native PuTTY on POSIX, curl on Windows. Verify extension negotiation, metadata, links, server-side copy/checksum and backend equivalence. |
| SCP | curl-dependent transfer path, not WinSCP's remote-shell filesystem engine. Listing and directory operations need a real SCP implementation. |
| FTP/FTPS | curl and a text listing parser. Verify server compatibility, MLSD, active/passive mode, TLS settings and resume semantics. |
| WebDAV | HTTP operations exist, but listing does not implement DAV PROPFIND/XML properties. Not a complete WebDAV filesystem. |
| S3 | URL maps to HTTPS, without AWS SigV4 signing or bucket/object listing. Authenticated Amazon S3 support is **not implemented**. |
| Transfers | Binary recursive transfers and basic offset resume. Missing full masks/exclusions, ASCII/automatic conversion, speed limits, retry/reconnect, atomic temporary-file policy and complete metadata handling. |
| Synchronization | A planner with previews (`-preview`, `SYNCPLAN`/`SYNCSUMMARY`), `-criteria=time\|size\|either\|none`, `-filemask=`, `-delete`, `-recursive` and a state record that tells a change from a conflict, with unit coverage and live-server checks. Missing `-mirror`, `-nodelete`, `-continue`, path exclusions, text conversion and a real watch mode: `keepuptodate` repeats a full comparison on a timer. |
| Sessions | `sessions.ini` site manager shared by the console and the GUI: `session`, `savesession`, `rmsession`, `open <name>`, export/import and last-used tracking; the GUI holds one persistent connection per tab. Missing folder trees, raw settings, encrypted passwords and the full upstream configuration model. |
| Security | POSIX password/key/agent and host-key policy exist. GSSAPI is compiled out. Proxy/tunnel configuration and curl fingerprint equivalence are incomplete. The GUI still defaults to `-hostkey=*`, but it reports the fingerprint it accepted for one-click pinning and never stores a wildcard or a password in `sessions.ini`; pinning by default remains the goal before claiming WinSCP security parity. |
| GUI | Browser Commander with one persistent session per tab, dual panes, a transfer queue with progress/pause/cancel/retry, synchronization preview, remote search, a remote editor and the site manager, driven by a local HTTP API over `-server`; the client has a jsdom regression test. Still not upstream Commander/Explorer: no drag and drop, custom commands, synchronized browsing or localization, and the API is bound to the loopback interface without authentication. |
| Editing/search | `edit` downloads, runs the configured editor and uploads on change, with a size limit (`option editmaxsize`) and a refusal when the remote file changed meanwhile; remote search walks directories by name mask. Missing save/upload monitoring of local edits, full-text search, encoding handling and the custom command system. |
| Automation | Subset CLI. No equivalent .NET/PowerShell API, XML action log, full parameter/environment expansion or command abbreviation resolution. |
| Integration | Debian launcher/icons and external SSH terminal, not full desktop drag/drop, shell integration and extensions. Windows-only mechanisms need equivalent workflows, not silently skipped requirements. |
| Localization/accessibility | No upstream translation catalog or equivalent accessibility validation. |
| Platforms | Linux/Windows builds exist; each protocol depends on platform/backend capabilities. Windows in-box curl cannot be assumed to support SSH. |

## Scripting changes made with this audit

* `-script` and `-command` default to aborting after a command reports an error.
  `option batch abort` and `on` stop subsequent commands; `continue` and `off`
  continue, but retain a nonzero process exit status when errors occurred.
  Noninteractive modes suppress prompts. This does **not** yet implement
  upstream's full per-file error/prompt policy inside a multi-file command.
* Added `man`, `recv`, `mget`, `send`, `mput` dispatch aliases and `binary`.
  Added `lls [directory[/mask]]` using local filesystem enumeration, with
  names-only output. `ascii` and remote shell alias `!` fail explicitly.
* Unsupported transfer switches now fail before any connection or transfer
  instead of being ignored or treated as paths. `-filemask` is **not** implemented;
  `-transfer=binary` is valid, while ASCII/automatic mode fails. Only bare
  `-resume` is implemented; valued resume-policy switches are rejected.
  Prefix filenames beginning with `-` with `./`.
* Reconnect and nonbinary transfer options no longer falsely report success.
* Synchronization validates the direction, argument count and switches before
  use; `both -delete` is rejected rather than risking destructive sequential
  reconciliation. Unsupported criteria/filter switches fail explicitly.

These intentional fail-closed changes can break scripts that previously relied
on silently ignored settings. Review those scripts; do not remove a safety
filter just to make a transfer proceed.

## Reproduce the checks

```sh
make -C source/linux all check
python3 source/linux/tests/ScriptTest.py source/linux/build/linuxscp
python3 tools/check_upstream_parity.py
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

New or removed command registrations cause failure until reviewed. Record the
new revision in the manifest when updating the baseline. The offline default
checks documented implemented commands against the local dispatcher and
validates statuses; it does not download upstream or test behavior.

`python3 tools/check_upstream_parity.py --require-full` is an opt-in release gate
and **currently fails**, because gaps remain. A green inventory check must
never be advertised as full parity. A future `verified` status needs reproducible
behavioral evidence on each supported platform, not merely a dispatcher entry.

## Prioritized completion plan

1. **Safety and automation:** pin host keys by default instead of accepting
   `*`, require authentication (or a per-run token) on the GUI API; implement
   file masks, batch per-file semantics, parameter expansion, retries and atomic
   transfers. Add live-server tests for failed opens, denied operations and
   interruption/data integrity.
2. **Real protocol coverage:** replace generic listing approximations with
   protocol-specific SCP, WebDAV and signed S3 clients; expand FTP/SFTP handling.
   Test CRUD, directory enumeration, metadata, authentication and resume against
   controlled servers on Linux and Windows.
3. **Synchronization:** the planner exists with previews, criteria, filtering
   and deletion guards; remaining work is `-mirror`/`-nodelete`/`-continue`,
   path exclusions, text conversion, a conflict resolution workflow and a real
   file-system watch mode behind `keepuptodate`.
4. **Session and desktop capabilities:** session folders, raw settings and
   encrypted passwords; queue priorities, speed limits and reconnect; local-side
   editing, full-text search and custom commands; Commander/Explorer workflows,
   drag and drop, accessibility and translations, with GUI workflow tests.
5. **External compatibility:** remaining script commands, structured logs,
   automation API equivalents and platform integrations. Maintain explicit
   exclusions only if product scope is intentionally narrowed; exclusions do
   not qualify as full upstream parity.

Completion requires a reviewed, exhaustive feature inventory, differential
script/behavior tests against WinSCP where possible, protocol integration tests,
GUI workflow tests and platform-specific acceptance tests. This audit and the
initial scripting fixes are a starting point, not completion of that project.
