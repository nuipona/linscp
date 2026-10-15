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

### What counts as evidence

`docs/upstream-parity.json` carries a `verification` block, and it is the
reason a status in this file means something. It lists the platforms LinuxSCP
ships on, and for each one: the command that produces the evidence, the test
suites that exist there and what each can see, and - for Windows - what it
cannot see at all. An entry then records its own `verified_on`: a map from
platform to the suites that checked it.

The rules are enforced by `tools/check_upstream_parity.py`, which fails the
build on any of them:

* `verified` requires an entry in `verified_on` for **every** platform in the
  ledger. Evidence from one host is a claim about that host.
* A suite can only be cited on a platform that has it, so `manual` evidence -
  the checks listed under that key, with the commands to repeat them - cannot
  be borrowed for another platform, and cannot be cited for Windows at all.
* A platform record without a `how`, a dated `date` and described suites is an
  error, because evidence nobody can reproduce is not evidence.

As it stands: 50 entries have evidence recorded on Linux, 45 on Windows,
and **34 are `verified`**. The gap is not timidity - Windows has no way to test the
native PuTTY transport, permissions on uploads, or a POSIX server's path
semantics, and the ledger says so per entry instead of rounding the whole
inventory down to `partial`.

The count is not taken on trust either. Each test suite prints one line when
it finishes - `evidence: suite=live-sftp platform=linux ran=6 skipped=0
failed=0` - and the CI jobs repeat it as an annotation on the run. A suite that
could not run because `paramiko` is missing shows up as `skipped=6`, which is
the case a green check mark used to hide. The S3 suite reports its
reference-signature test skipped the same way when `botocore` is absent.

`edit`, `savesession`, `rmsession`, `sessions`, `show`, `attr` and the extra
aliases `del`, `md`, `sync`, `quit` and `end` are in that second list. `edit` in
particular is a LinuxSCP command: the upstream *script interpreter* has none,
even though WinSCP edits files from its GUI and through its .NET assembly.

## Major gaps

| Area | Current implementation and remaining work |
| --- | --- |
| SFTP | Native PuTTY on POSIX, curl on Windows. SYMLINK, server-side copy-data (with a streamed fallback) and check-file-name (with a local hash fallback) are implemented and were exercised against a live in-process SFTP server; unknown extensions still fail explicitly. Remaining: metadata policies, and the native transport, which has no Windows side to verify. |
| SCP | curl-dependent transfer path, not WinSCP's remote-shell filesystem engine. Listing and directory operations need a real SCP implementation. |
| FTP/FTPS | curl and a text listing parser. Verify server compatibility, MLSD, active/passive mode, TLS settings and resume semantics. |
| WebDAV | A PROPFIND filesystem: Depth 0/1 listings parsed from the 207 multi-status (type, size, date - namespace prefix agnostic), stat with real sizes/times and proper missing-path reporting, MKCOL with named refusals, MOVE with an absolute-URI Destination, COPY with Destination/Overwrite for single-file `cp` (streamed fallback when COPY is refused; both paths checked live), DELETE with 409 as 'not empty' and `rmdir` checking children first because DAV collection deletes are recursive; `open` probes with PROPFIND and refuses a 401 after the `--anyauth` challenge or a 405 that means the server is not DAV (`LiveDavTest.py` against a stdlib mock, Windows-portable). Missing: LOCK/UNLOCK, dead properties, collection-level precondition handling and `If` headers. The Windows MSVC CI CTest run passed these DAV assertions on 2026-10-15. |
| S3 | AWS SigV4 over curl with path-style bucket access: `s3://` URLs (region from `AWS_REGION`/`AWS_DEFAULT_REGION`/the hostname/us-east-1), ListBuckets, ListObjectsV2 with delimiter and continuation, HEAD stat, get with range resume and `Last-Modified` preservation, PutObject for small files, multipart initiation/part upload/completion for files over 16 MiB, directory markers with bucket-aware `mkdir`/`rmdir`, CopyObject for `cp` and rename-as-copy-plus-delete, and refusal of unsigned or wrongly signed requests - signed vectors pinned to botocore in the unit suite and the whole loop against a TLS mock that re-derives signatures itself, including synthetic HTTP-200 Error bodies and abort cleanup (`LiveS3Test.py`, Linux-only evidence). On a handled failure the client attempts to abort; a retry restarts from part one because upload IDs/parts are not persisted. Multipart copy is not implemented. Missing: ACLs, versioning, storage classes, SSE options, object metadata beyond content, and any Windows claim. |
| Transfers | Binary/ASCII/automatic transfers with full WinSCP file masks (name, directory and path masks plus size/time criteria), `option include/exclude`, `-filemask=`, `-transfer=`, fail-on-no-match, resume (binary only), and reconnect via `option reconnecttime`; ASCII conversion, `-speed=`, `-append`, `-resumesupport=` and `-preservetime[=all]` were verified against a live server, and refused opens, denied operations, an interrupted download and its resume are automated in `LiveSftpTest.py`. Resume-capable transfers stage through upstream's `.filepart` name and are renamed into place only when complete - `option resume automatic` (default) from upstream's 100 KB threshold, `on` always, `off` never - so an interrupted transfer never leaves a truncated file under the final name and a continuation reads the staging file instead of splicing the existing destination; the live suite covers both directions, the stale-staging-file restart and `option resume off`. That suite cannot run on Windows at all, because the `curl.exe` built into Windows has no SFTP support, so nothing about a Windows transfer is verified (`docs/windows.md`). Transfer presets (named, per-site and rule-based, using upstream's copy-parameter vocabulary) drive these transfers too, checked live on both platforms. Missing: per-file progress reporting on the curl paths. |
| Synchronization | A planner with previews (`-preview`, `SYNCPLAN`/`SYNCSUMMARY`), `-criteria=` taking upstream's `none`, `either` and `both` and any comma-separated list of `time`, `size` and `checksum` (`-criteria=checksum` reads both files, through the server's checksum when the transport can ask for it and a downloaded copy otherwise, and a file that cannot be hashed refuses the plan instead of counting as equal; the `both` direction decides from its state record and so refuses the criteria that record cannot honour), `-filemask=`, `-transfer=binary\|ascii\|automatic`, `-delete`/`-nodelete` (default from `option synchdelete`), `-mirror` (rejected with `both`), `-nonrecursive`, `-interval=`, `-count=`, `keepuptodate -continue` (keeps watching after a failed pass) and a state record that tells a change from a conflict; `-mirror`, ASCII conversion and a change only `-criteria=checksum` can see were verified against a live server. `keepuptodate` now recursively watches local changes with inotify on Linux and `ReadDirectoryChangesW` on Windows; `-interval=` remains a full-rescan safety net, with timer polling if native watch setup fails. Windows MSVC CI passed the native watcher and local-to-remote assertions on 2026-10-15. Missing: path exclusions and WinSCP's conflict-resolution UI. |
| Sessions | `sessions.ini` site manager shared by the console and the GUI: `sessions` (list, open by name, export/import), `savesession`, `rmsession`, `open <name>` and last-used tracking, while `session` itself follows upstream - open-session listing, active line and 1-based switching; the GUI holds one persistent connection per tab. Stored passwords are encrypted under a sibling `sessions.key`, in a format the GUI reads and writes too, and a proxy travels with a site: `open -rawsettings` applies upstream's proxy settings, `savesession -rawsettings` stores them with the proxy password encrypted like a session password, and a site whose stored proxy cannot be decrypted is refused rather than opened without it. Missing folder trees, the raw settings beyond the proxy ones, and the full upstream configuration model. |
| Security | POSIX password/key/agent and host-key policy exist, and an unknown or changed key is reported on a `HOSTKEY` line instead of being accepted. GSSAPI is compiled out, and SSH tunnels and curl fingerprint equivalence are incomplete. Proxy configuration follows upstream's setting names (`ProxyMethod`, `ProxyHost`, `ProxyPort`, `ProxyUsername`, `ProxyPassword`, `ProxyTelnetCommand`, `ProxyLocalCommand`, `ProxyDNS`, `ProxyLocalhost`, `ProxyExcludeList`): SOCKS 4 and 5, HTTP CONNECT and a telnet or local command through the PuTTY core on the native transport, and `--proxy`/`--proxy-user`/`--noproxy` on the curl protocols, which refuse a proxy they cannot run instead of connecting around it - the one setting the curl path cannot express is `ProxyLocalhost=0`, since curl proxies localhost too. A proxy password is masked in the echoed command line and in the XML action log. The GUI verifies by default, pins on request, stores a pin rather than a wildcard, and serves its API on loopback behind a per-run token with a `Host` check. Still no user account or upstream key-management UI; stored session passwords are encrypted at rest under `sessions.key` rather than kept in the clear. |
| GUI | Browser Commander with one persistent session per tab, dual panes, a transfer queue with progress/pause/cancel/retry, synchronization preview, remote filename search plus bounded UTF-8 content search, a remote editor, the site manager and host-key pinning, driven by a loopback HTTP API that requires a per-run token and checks the `Host` header. Synchronized browsing (both panes move as one, with a refusal - not a divergence - when the counterpart folder is missing) and drag and drop between the panes (payload, target highlight, the drop queuing exactly the dragged names, same-pane drops ignored) match WinSCP's Commander, asserted in `ClientTest.js`. Missing: custom commands, localization, and drops from the operating system's file manager. |
| Editing/search | `edit` downloads, runs the configured editor and uploads on change, with a size limit (`option editmaxsize`) and a refusal when the remote file changed meanwhile; GUI search walks remote directories by filename mask or searches literal UTF-8 content in matching files, bounded to 8 MiB per file and 64 MiB/200 files per search with skipped/truncated counts (`RemoteSearchTest.py`, `ClientTest.js`). Missing save/upload monitoring of local edits, broader encoding handling and the custom command system. |
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
* Transfer presets with upstream's copy-parameter vocabulary: named presets
  (`savepreset`/`rmspreset`/`presets`, `option preset <name|off>`), the
  preset stored with a site (`savesession -preset=`, restored by `open`) and
  host/user/directory/mask rules applying one automatically - always under
  the command's own switches, never over them. `-permissions` now works on
  downloads too (bare = the remote file's mode, `=octal` = that mode) and
  `-rawtransfersettings[=N]` accepts upstream's key=value settings with
  refused-by-name errors for everything this build does not apply, all
  covered by the script suite and a live WebDAV preset roundtrip on both
  platforms.
* New remote commands, each verified against a live in-process SFTP server:
  `cp` (server-side copy-data with streamed fallback, recursive),
  `ln`/`symlink` (SFTP SYMLINK), `checksum <alg> <file>` (server-side
  check-file-name first, local sha-256/sha-1/md5/crc32 through the PuTTY core
  as fallback, printed as `<hash> <file>`), and `call`/`!` (SSH exec in a
  one-shot second connection, leaving the SFTP session open).
* Console parity pass against upstream `Script.cpp`: `session` now lists the
  open sessions and switches by 1-based index (the stored sites moved to
  `sessions`), `close [index]` prints the session it closed and refuses a
  foreign index, `stat` prints the single listing line upstream prints (the
  key/value block lives on as the `attr` addition the GUI parses), `lls`
  prints upstream's date/time/size columns with `<DIR>` for directories,
  `ls`/`lls` print the no-match sentence gated by `option failonnomatch`,
  `lcd` prints the directory it changed into, and `help` lists commands in
  upstream's two columns with aliases resolving to their command - each
  covered by the script suite and, for anything needing a connection, the
  live WebDAV suite, which is cited for Windows as well as Linux.
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
* Resume-capable transfers stage through `<destination>.filepart` and are
  renamed into place only when complete, which is upstream's policy
  (`PartialExt`): `option resume` gained its upstream third value
  `automatic` as the default (binary files from 100 KB on, rsSmart),
  alongside `on` (rsOn) and `off` (rsOff), and `-resumesupport=` still
  overrides all three. The continuation offset is read only from the
  staging file - never from an existing destination, whose content may be
  an unrelated complete version - so a destination is replaced whole and
  never spliced. Two script-visible consequences: an interrupted transfer
  leaves `name.filepart` instead of a truncated `name`, and a batch
  overwrite of an existing destination now also needs `option confirm
  off` when `-resume` finds no staging file to continue, because the
  overwrite is confirmed before the transfer instead of being assumed
  from the switch.
* `savesession <name> -password=...` stores the password encrypted under a
  key file (`sessions.ini` -> `sessions.key`, created on first use with
  user-only access) in the `lscp1$` format the graphical client also reads;
  plaintext entries from before are upgraded on the next save. Omitting
  `-password` still clears a password that could be read, but a value the
  key file cannot decrypt is preserved across an unrelated re-save, and
  `open <name>` reports why a stored password cannot be used instead of
  silently connecting without it.

These intentional fail-closed changes can break scripts that previously relied
on silently ignored settings. Review those scripts; do not remove a safety
filter just to make a transfer proceed.

## Reproduce the checks

```sh
make -C source/linux all check           # the C++ checks, no I/O
make -C source/linux check-live          # ScriptTest.py + LiveSftpTest.py
python3 tools/check_upstream_parity.py   # the inventory and its evidence rules
```

On Windows, the CMake/CTest workflow runs the Windows-portable script and DAV
suites. The MSVC CI run on 2026-10-15 passed the live DAV checks, including the
local-to-remote watcher and server-side COPY assertions, and the bounded GUI
search backend suite. The live SFTP suite
skips its connection cases because the `curl.exe` built into Windows has no
SFTP support; the live S3 TLS mock is POSIX-only. Those are transport/test
limitations, not a blanket skip of Windows live testing.

The inventory check compares three things: every documented command has a
dispatcher entry, every dispatched command is in the abbreviation table
`kCommandNames` (and vice versa, so an abbreviation can never resolve to a
command that then reports 'Unknown command'), and every entry has a status and
an explanation. It does not test behavior.

`source/linux/tests/LiveSftpTest.py` is the automated half: it starts
`TestSftpServer.py` (Python `paramiko`, `pip install paramiko`) on a port the
kernel picks, in a jail of its own, and covers what a server-free test cannot:
an open with the wrong password, an open with a pinned host key that does not
match, `put`/`rm`/`mkdir` into a directory the server will not let us write, a
download interrupted by Ctrl-C that stays in `.filepart` and is completed by a
plain `get`, the same for an interrupted upload, a continuation that must not
splice an existing destination, a transfer whose server dies under it and the
same resume against a restarted server, `option resume off` disabling the
staging, `-preservetime` in both directions, and the shared stored-password
format: a password encrypted by the GUI's module authenticating the console
client, a missing key file reported without losing the value, and a
core-written password decrypted back by the GUI's reader.

The server itself is a plain filesystem-backed SFTP + exec endpoint (user
`testuser`, `LSCP_TEST_ROOT` as the visible root) that implements `chattr` for
`SETSTAT` - paramiko routes that request there, not to `setstat`, which is what
makes `chmod` and the timestamp handling testable against it at all. Run it by
hand the same way when a case is not automated yet:

```sh
LSCP_TEST_ROOT=/tmp/lscp-sftp-root python3 source/linux/tests/TestSftpServer.py &
# prints "READY <port>"; testuser/testpass
source/linux/build/linuxscp -batch -hostkey='*' \
  -command="open sftp://testuser:testpass@127.0.0.1:12765/ ; chmod 750 -recursive ptree"
```

What only that hand-run covers so far - and is recorded as `manual` evidence in
the ledger, on Linux only: `cd`/`ls` masks, `cp`, `ln`, `checksum`, `call`,
`mv`, `rmdir`, `rm -onlyfile`, ASCII transfers, `synchronize -mirror
-transfer=`, `chmod` (octal, symbolic and `-recursive`), `copyid`, `-speed=`
(timed), `-append`, `-resumesupport=` and `-preservetime[=all]` on directories.
Automating those is the cheapest way to turn entries in this table into entries
with evidence.

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
never be advertised as full parity. `verified` is no longer a word anyone can
add to the inventory: it needs an entry in `verified_on` for every platform,
citing suites that exist there, and the entries it would cite have to be able
to say what those suites proved.

## Prioritized completion plan

1. **Safety and automation:** host keys are verified and pinned rather than
   accepted, and the GUI API is loopback-only behind a per-run token; file
   masks, ASCII/automatic conversion, reconnect, parameter expansion,
   `.filepart` staging (the atomic temporary-file policy) and a live-server
   test harness - including refused opens, denied operations and
   interruption/data integrity - are in place. What is left is a user
   account and upstream's batch per-file error/prompt semantics.
2. **Real protocol coverage:** replace generic listing approximations with
   protocol-specific SCP, WebDAV and signed S3 clients; expand FTP/SFTP handling.
   Test CRUD, directory enumeration, metadata, authentication and resume against
   controlled servers on Linux and Windows.
3. **Synchronization:** the planner exists with previews, criteria, filtering,
   `-mirror`, `-transfer=` conversion, deletion guards and `keepuptodate
   -continue`; Linux has recursive inotify, and Windows has a recursive
   `ReadDirectoryChangesW` watcher for local-to-remote `keepuptodate` behavior.
   Remaining work is path exclusions, a conflict-resolution workflow and broader
   sync semantics; the native watcher and DAV direction have Windows CI coverage.
   `-resumesupport=` thresholds and `-append` are in place for the native
   transport, with the curl backends refusing them explicitly.
4. **Session and desktop capabilities:** session folders, and the raw settings
   beyond the proxy ones `open -rawsettings` applies; queue priorities
   (transfer presets, speed limiting and reconnect exist); local-side
   edit monitoring and custom commands; Commander/Explorer workflows,
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
