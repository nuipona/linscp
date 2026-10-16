# Linux feature inventory

Source: [upstream-parity.json](upstream-parity.json), pinned to winscp/winscp `69edabbe2dc2c62661a7d3fc7e2396b5f42d504d`. **This is not full WinSCP parity or an exhaustive upstream behavioral certification.** The 40 registered upstream script commands are tracked separately in [the parity audit](upstream-parity.md); these rows decompose the 15 feature areas into specific Linux behaviors and named gaps.

87 capabilities: 49 verified for the stated behavior, 23 partial, 15 missing. `verified` means the **row's** behavior has an automated case on Linux, not that every upstream variant or protocol works. `partial` names a working subset and the next gap. `missing` has no implementation. Evidence skipped by a missing dependency is not a pass; consult each suite's `evidence:` line in CI.

Regenerate after editing the JSON: `python3 tools/check_upstream_parity.py --write-report`. The default check fails if this view or a cited test case drifts.

## SFTP

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sftp.native-session` | verified | live-sftp | Native PuTTY SFTP session refuses bad credentials and unexpected SSH host keys. |
| `sftp.directory-crud` | partial | live-sftp | Transfers, directories, renames, symbolic links and chmod use SFTP requests rather than curl. **Next:** Exercise links, different SFTP versions and metadata behavior against more servers. |
| `sftp.copy-data` | verified | live-sftp | Native copy-data replies are used when offered; a multi-source/recursive streamed fallback works when absent. |
| `sftp.check-file-name` | verified | live-sftp | Server check-file-name is used for sha-256 and all documented hashes fall back to downloaded bytes when refused. |
| `sftp.permissions-and-times` | partial | live-sftp | Symbolic recursive chmod and bidirectional timestamp preservation work against the test server. **Next:** Owner/group changes, per-site permission policies and metadata preservation across varied SFTP servers remain. |

## SCP

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `scp.curl-file-transfer` | partial | live-curl | An scp:// URL selects curl for authenticated file transfers when curl is built with SCP. Downloads retain local .filepart staging; automatic uploads go directly to the final remote name because curl's SCP backend has no remote rename, and explicit upload resume is refused rather than silently pretending to resume. **Next:** Implement a safe resume contract only if the backend can provide remote staging, because curl is not the WinSCP SCP engine. |
| `scp.shell-directory-engine` | missing | — | No WinSCP-style remote shell filesystem engine or SCP directory listing. **Next:** Implement and test remote-shell listing, stat and directory operations before claiming SCP parity. |
| `scp.command-and-metadata` | missing | — | No SCP-specific remote command and metadata semantics matching upstream. **Next:** Implement and test SCP shell commands and metadata with an actual SCP server. |

## FTP/FTPS

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `ftp.curl-transfer` | partial | live-curl | curl handles authenticated FTP/FTPS downloads and uploads and attempts FTP directory operations; the Linux live suite covers throwaway FTP and implicit FTPS endpoints with final-byte checks. **Next:** Broaden the live FTP/FTPS compatibility matrix to cover resume, errors and varied server implementations. |
| `ftp.listings-and-mlsd` | verified | live-curl, unit | Plain FTP is served by the native FTP engine, which asks for MLSD where the server advertises it: size, type, time and permissions come from the protocol instead of from an ls -l line each server formats its own way. MLST answers stat, a server that refuses MLSD falls back to LIST, FtpUseMlsd=0 asks for LIST from the start, and FtpUseMlsd=2 (require) fails the listing rather than returning one without the machine-readable facts. The ls -l parser is shared with the curl backend, which serves the LIST replies and every transfer. |
| `ftp.active-passive` | verified | live-curl, unit | FtpPasvMode picks the data connection the native engine opens: passive by default, EPSV first with PASV as the fallback, and FtpPasvMode=0 opens it from the client instead, binding a listener and telling the server where it is with PORT or EPRT. FtpForcePasvIp decides whose address a PASV reply may use - 0 the control connection's, 1 (the default) the reply's unless it is unroutable, 2 the reply verbatim - and FtpAccount answers a server that asks for an account name on top of the credentials. |
| `ftp.tls-policy` | verified | live-curl, unit | FTPS stays on curl, the TLS client of this build, and upstream's setting names drive it: ftps:// (Ftps=1) is implicit TLS on port 990, ftpes:// (Ftps=2 or 3 on an ftp:// URL) is explicit TLS through --ssl-reqd, MinTlsVersion and MaxTlsVersion become --tlsv1.x and --tls-max, TlsCertificateFile and TlsCertificateKeyFile become --cert and --key, CaCertificateFile and CaCertificateDir become --cacert and --capath, and PinnedPublicKey becomes --pinnedpubkey. A policy curl cannot honour, or one given to a protocol without TLS, is refused before the connection rather than dropped; hostkey='*' is the only value that turns certificate validation off. |

## WebDAV

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `dav.propfind-and-stat` | verified | live-dav | Depth 0/1 PROPFIND parses collection type, size and modification time; stat distinguishes absent paths. |
| `dav.basic-crud` | verified | live-dav | MKCOL, PUT, GET, DELETE and refusal of nonempty collection removal work against the live server. |
| `dav.copy-and-move` | verified | live-dav | MOVE and COPY send absolute Destination URIs; a refused COPY falls back to a byte-exact stream. |
| `dav.escaped-paths` | verified | live-dav | curl URL paths, DAV href comparisons and Destination headers preserve spaces and literal %, ? and # in names. |
| `dav.locks` | verified | live-dav, unit | 'lock <file> [-timeout=SECONDS]' takes an exclusive write lock and keeps its token in the session; 'unlock <file>' releases it. Every later upload, delete and move of that path carries the token as the If header, so a second writer without it is refused with 423 and the locked file keeps its content. A server that does not implement LOCK answers 405 and the command prints that instead of reporting a protected write. Upstream's WebDAV backend sends no LOCK or UNLOCK at all, so this is a LinSCP extension. |
| `dav.dead-properties-and-preconditions` | verified | live-dav, unit | 'property <file> -set=NAME=VALUE' stores one dead property through PROPPATCH and 'property <file> -get=NAME' reads it back with a named-property PROPFIND; both use this client's own namespace (urn:linuxscp:properties), a name that would build invalid XML is refused before any request, and a property the server does not have prints 'NAME is not set' rather than failing. The conditional write is the lock token: PUT, DELETE and MOVE of a locked path send If, which is what makes the server refuse a write whose lock is gone (423). |

## Amazon S3

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `s3.sigv4-auth` | verified | live-s3 | Requests are signed with AWS Signature Version 4; absent or incorrect credentials fail against a TLS verifier. |
| `s3.bucket-object-listing` | verified | live-s3 | ListBuckets, ListObjectsV2, HEAD and virtual directories are exercised against the test endpoint. |
| `s3.small-objects-and-range-get` | verified | live-s3 | PutObject, GET and a resumed HTTP byte-range download roundtrip. |
| `s3.multipart-upload` | verified | live-s3 | Files over 16 MiB use initiate/parts/complete and abort on handled completion errors. |
| `s3.copy-object` | verified | live-s3 | Single-object CopyObject, embedded HTTP-200 errors and rename-as-copy-plus-delete are tested. |
| `s3.multipart-restart-and-copy` | partial | live-s3 | Large uploads restart from part one; single-object copies use CopyObject. **Next:** Persist upload IDs/parts to resume interrupted uploads and implement multipart copy of large objects. |
| `s3.object-options` | missing | — | No ACL, versioning, storage class, SSE or custom object metadata options. **Next:** Implement signed request options and tests for these S3 object policies. |

## Transfers

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `transfer.masks` | verified | live-sftp, unit | Name/path/directory masks and size/time criteria filter transfers and synchronization. |
| `transfer.ascii-automatic` | verified | live-dav | ASCII conversion is byte-checked through a live upload; automatic mode selects text files by mask. |
| `transfer.filepart-resume` | verified | live-sftp | Interrupted uploads/downloads stage in .filepart and continue there rather than splicing the final destination. |
| `transfer.resume-modes` | verified | live-sftp, script | Automatic/on/off and -resumesupport thresholds are parsed; resume-off transfers without staging. |
| `transfer.time-permissions` | partial | live-sftp | SFTP preserves file times in both directions; -preservetime=all and -permissions are accepted. **Next:** Expand live directory timestamp/permission checks and test metadata semantics across curl transports. |
| `transfer.speed-and-append` | partial | — | Native SFTP supports append and paced transfers; curl uses --limit-rate and refuses append. **Next:** Automate timing and append tests across the supported native and curl protocol matrix. |
| `transfer.presets` | verified | live-dav, script | Named, per-site and rule-selected presets change live transfer settings. |
| `transfer.desktop-queue` | partial | gui-browser-upload | GUI task queue supports pause, cancel and retry; browser-originated files are privately staged. **Next:** Add queue priorities and live cancellation/data-integrity tests for curl-backed tasks. |
| `transfer.curl-progress` | partial | unit | curl-backed downloads and uploads stream curl's progress meter from the Linux child process and translate it into the same byte-progress callback used by native SFTP; credentials stay in argv only in the child and both output pipes are drained concurrently. LiveCurlTest now checks authenticated FTP/FTPS/SCP transfers and the SCP upload-resume refusal, while the unit suite exercises both meter columns. **Next:** Add a direct live assertion of intermediate progress callbacks and cancellation/data-integrity behavior; the S3 small-object and multipart paths report progress, but the CLI live suite currently proves final bytes and protocol policy rather than observing the callback itself. |

## Synchronization

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sync.one-way` | verified | live-dav | Local-to-remote keepuptodate and recursive one-way planning/upload run against the live WebDAV server. |
| `sync.both-state` | partial | unit | A state file distinguishes one-sided changes from conflicts for both-direction sync. **Next:** Validate conflict resolution and state changes after partial failures with live two-sided cases. |
| `sync.checksum` | verified | live-sftp, unit | Checksum criteria find same-size/same-time changes and stop if contents cannot be hashed. |
| `sync.exclusions-delete-guard` | verified | live-sftp | An excluded subtree is pruned from the plan and cannot be deleted at the destination. |
| `sync.keepuptodate-inotify` | verified | live-dav | Nested local arrivals wake the recursive inotify watcher before the rescan interval. |
| `sync.criteria-and-mirror` | partial | script | Time/size/checksum and mirror are planned; unsupported criteria for both direction are refused. **Next:** Broaden live checks for all mirror/metadata/deletion combinations against varied servers. |
| `sync.conflict-ui` | missing | — | The planner reports a conflict but provides no WinSCP-style interactive resolver. **Next:** Add a reviewed, tested conflict-resolution dialog and policy for both-direction sync. |

## Sessions/configuration

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sessions.site-manager` | verified | script | Stored sites can be saved, opened, removed, exported and imported. |
| `sessions.password-at-rest` | verified | live-sftp | Stored site passwords are encrypted under sessions.key and shared between the CLI and GUI. |
| `sessions.proxy-settings` | partial | live-sftp | Upstream proxy setting names are applied by open and stored with a site; SOCKS5 works live for SFTP. **Next:** Test SOCKS4/HTTP CONNECT and telnet/local-command proxy behavior on each applicable transport. |
| `sessions.rawsettings` | verified | live-curl, script, unit | 'open' and 'savesession' take '-rawsettings' with upstream's names: the proxy settings (ProxyMethod, ProxyHost, ProxyPort, ProxyUsername, ProxyPassword, ProxyTelnetCommand, ProxyLocalCommand, ProxyDNS, ProxyLocalhost, ProxyExcludeList), the FTP engine's policy (FtpPasvMode, FtpForcePasvIp, FtpUseMlsd, FtpAccount) and the TLS policy (Ftps, MinTlsVersion, MaxTlsVersion, TlsCertificateFile, TlsCertificateKeyFile, CaCertificateFile, CaCertificateDir, PinnedPublicKey). A name or value outside that set is refused by name before any connection, so a setting is never stored and then dropped at open time, and 'savesession' writes what it applied under the keys a saved site reads back. |
| `sessions.folder-tree` | missing | — | Saved sessions have names, not upstream folder-tree behavior and per-folder settings. **Next:** Design and test a compatible folder tree shared by GUI and scripting. |

## Security/authentication

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `security.native-hostkey` | verified | live-sftp | Native SFTP refuses an unknown/unexpected host key until it is pinned. |
| `security.password-key-agent` | partial | live-sftp | Password, private-key and ssh-agent authentication paths exist. **Next:** Test key/agent login, passphrase prompts and policy variations against real servers. |
| `security.curl-pins-and-tls` | verified | live-curl, unit | '-hostkey=' accepts the fingerprint spellings people paste - a SHA-256 digest in hex with optional ':' separators, OpenSSH's SHA256:base64 and curl's sha256//base64 - and converts between them, so the SSH protocols pin the host key with --hostpubsha256, or with --hostpubmd5 for a 32-hex MD5 digest. A TLS session keeps chain validation (the system store, or CaCertificateFile and CaCertificateDir) and pins the server's public key with --pinnedpubkey; a value that is not a pin curl can compare is refused with the setting to use instead, and '*' is the only value that disables the checks. |
| `security.proxy-isolation` | partial | live-dav, live-sftp | SFTP SOCKS5 and curl proxy routing are tested; unusable proxy modes are refused instead of bypassed. **Next:** Exercise every supported proxy method and DNS/exclusion policy across all transports. |
| `security.gui-api-token` | verified | gui-browser-upload | The local browser API rejects requests missing its per-run token. |
| `security.gssapi-and-tunnels` | missing | — | GSSAPI is compiled out; SSH tunnel configuration is not implemented. **Next:** Implement and test GSSAPI/SSH tunnels only with an explicit security review. |
| `security.gui-account` | missing | — | The loopback GUI has no multi-user account or authorization roles. **Next:** Add authenticated accounts before claiming multi-user/server deployment support. |

## GUI

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `gui.commander-panes-tabs` | verified | gui | The browser renders local/remote panes and session tabs and routes Commander function keys. |
| `gui.transfer-queue-controls` | partial | gui | The queue renders progress/retry UI and supports pause/cancel on queued tasks. **Next:** Test real cancellation, priority and resumability under live failures. |
| `gui.sync-preview` | verified | gui | The preview renders planned actions and posts the chosen comparison on run. |
| `gui.synchronized-browsing` | verified | gui | Entering/leaving paired folders follows both panes and refuses a missing counterpart. |
| `gui.pane-drag` | verified | gui | Dragging a selected item between panes queues exactly those names; same-pane drops do nothing. |
| `gui.desktop-file-drop` | verified | gui, gui-browser-upload | A desktop File is staged under an authenticated request with exact bytes and a remote destination. |
| `gui.desktop-folder-drop` | verified | gui, gui-browser-upload, live-dav | Entry/handle recursion queues nested files and empty directories; parent creation is idempotent, paths validated. |
| `gui.desktop-drop-compatibility` | partial | — | Chromium FileSystemEntry/Handle and plain File drops work with a 1000-entry/32-level UI limit. **Next:** Test real file-manager/browser combinations, symlinks, huge trees and permission expiry. |
| `gui.desktop-local-drop` | missing | — | Desktop drops target the remote pane only; the local pane rejects them. **Next:** Provide a safe, explicit local destination workflow if needed. |
| `gui.explorer-custom-commands` | missing | — | No separate upstream Explorer-style UI or custom-command framework. **Next:** Build/test Explorer navigation and custom-command execution with appropriate safety prompts. |

## Editing/search

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `editing.remote-editor` | partial | gui | Remote text editing downloads, checks modification times and uploads only changed files. **Next:** Cover external-editor monitoring, encoding and conflict policy against live servers. |
| `editing.local-editor` | verified | gui-local-editor | GUI reads text and saves locally with a conflict check and atomic replace; symlinks are refused. |
| `editing.filename-and-content-search` | verified | gui-search | Filename search and bounded literal UTF-8 content search skip binary/oversized files and cap the scan. |
| `editing.monitor-local-changes` | missing | — | The external edit command does not monitor a local working copy after the editor exits. **Next:** Add and test local-side edit monitoring with safe re-upload decisions. |
| `editing.encoding-policy` | partial | gui-local-editor | The browser editor/search expects UTF-8 text and refuses binary files. **Next:** Implement a selectable encoding strategy and test non-UTF-8 roundtrips. |

## Automation

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `automation.command-interpreter` | verified | script | Batch abort/continue, aliases, abbreviated commands and rejected unknown options are checked server-free. |
| `automation.parameter-and-timestamp` | verified | script | %1%..%9%, timestamp formats and environment expansion follow tested script behavior. |
| `automation.literal-gui-command` | verified | gui-browser-upload, script | CLI -literal suppresses expansion, and both the GUI command runner and its background task queue use it so names containing %...% stay literal. |
| `automation.xml-action-log` | partial | script | An XML action log records commands, results and transfers with masked passwords. **Next:** Compare fields/order against upstream logs and add differential tests. |
| `automation.dotnet` | missing | — | No WinSCP .NET assembly or equivalent object model for Linux. **Next:** Define an API contract and test automation clients; a CLI is not the assembly. |
| `automation.powershell` | missing | — | No PowerShell cmdlets or compatibility shim. **Next:** Provide and test PowerShell automation only if this is a product requirement. |

## Desktop integration

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `desktop.scheme-handlers` | verified | gui-desktop-uri | The desktop entry and GUI parse sftp://, ftp:// and ftps:// startup URLs with escaped paths. |
| `desktop.launcher-prefix` | verified | gui-desktop-uri, script | Installed, Makefile and CMake binaries select their own GUI launcher; the GUI finds the matching core under a relocatable prefix. |
| `desktop.file-manager-drops` | partial | gui-browser-upload | Remote file/folder drops work through browser entry APIs and a token-protected queue. **Next:** Verify cross-browser/file-manager behavior and add local-pane drops if required. |
| `desktop.shell-and-extensions` | missing | — | No system file-manager shell commands or upstream extension system. **Next:** Specify desired desktop environment integrations and build/test them individually. |

## Localization/accessibility

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `locale.translations` | missing | — | No upstream language catalogs or translation pipeline. **Next:** Choose supported locales and add translated strings with GUI/CLI language tests. |
| `locale.accessibility` | missing | — | No assistive-technology, keyboard-only or screen-reader acceptance suite. **Next:** Audit and test focus order, ARIA roles, contrast and screen-reader announcements. |

## Packaging/platforms

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `platform.make-linux` | partial | — | The Makefile builds the native Linux/PuTTY client and refuses a non-Linux host. **Next:** Exercise supported Debian architectures and compiler/toolchain versions. |
| `platform.cmake-linux` | partial | — | CMake builds Linux binaries, registers CTest, refuses unsupported hosts and has an installed-prefix smoke check in CI. **Next:** Compare release artifacts on multiple Linux distributions/architectures and test upgrades in place. |
| `platform.debian-package` | partial | — | The Debian package installs the CLI, GUI, desktop entry, icons and man page; CI smokes the installed paths. **Next:** Verify installation/upgrade/uninstall on Debian stable and non-root environments. |
| `platform.prefix-lookup` | verified | gui-desktop-uri | An arbitrary PREFIX/share/linuxscp/gui resolves PREFIX/bin/linuxscp before /usr/bin. |
| `platform.posix-runtime-services` | verified | unit | The Linux platform layer supplies temporary directories, home/config lookup and concurrent child stdout/stderr capture. |
| `platform.terminal-and-console` | verified | script, unit | Linux command-line parsing, terminal-facing configuration and the native console entry point are built from the Linux sources. |
| `platform.linux-file-events` | verified | live-dav | The Linux synchronization watcher uses filesystem events and retains interval polling as a safety net when a watch cannot be installed. |
