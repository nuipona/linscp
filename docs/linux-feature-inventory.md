# Linux feature inventory

Source: [upstream-parity.json](upstream-parity.json), pinned to winscp/winscp `69edabbe2dc2c62661a7d3fc7e2396b5f42d504d`. **This is not full WinSCP parity or an exhaustive upstream behavioral certification.** The 40 registered upstream script commands are tracked separately in [the parity audit](upstream-parity.md); these rows decompose the 15 feature areas into specific Linux behaviors and named gaps.

87 capabilities: 78 verified for the stated behavior, 0 partial, 9 missing. `verified` means the **row's** behavior has an automated case on Linux, not that every upstream variant or protocol works. `partial` names a working subset and the next gap. `missing` has no implementation. Evidence skipped by a missing dependency is not a pass; consult each suite's `evidence:` line in CI.

Regenerate after editing the JSON: `python3 tools/check_upstream_parity.py --write-report`. The default check fails if this view or a cited test case drifts.

## SFTP

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sftp.native-session` | verified | live-sftp | Native PuTTY SFTP session refuses bad credentials and unexpected SSH host keys. |
| `sftp.directory-crud` | verified | live-sftp | Transfers, directories, renames, symbolic links (ln/symlink) and chmod use native SFTP requests rather than curl. |
| `sftp.copy-data` | verified | live-sftp | Native copy-data replies are used when offered; a multi-source/recursive streamed fallback works when absent. |
| `sftp.check-file-name` | verified | live-sftp | Server check-file-name is used for sha-256 and all documented hashes fall back to downloaded bytes when refused. |
| `sftp.permissions-and-times` | verified | live-sftp | Octal and recursive symbolic chmod plus file and directory timestamp preservation (-preservetime=all) work against the live SFTP server. |

## SCP

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `scp.curl-file-transfer` | verified | live-curl | An scp:// URL selects curl for authenticated file transfers with local .filepart download staging, direct uploads and explicit refusal of unsafe upload resume. |
| `scp.shell-directory-engine` | missing | — | No WinSCP-style remote shell filesystem engine or SCP directory listing. **Next:** Implement and test remote-shell listing, stat and directory operations before claiming SCP parity. |
| `scp.command-and-metadata` | missing | — | No SCP-specific remote command and metadata semantics matching upstream. **Next:** Implement and test SCP shell commands and metadata with an actual SCP server. |

## FTP/FTPS

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `ftp.curl-transfer` | verified | live-curl | curl handles authenticated FTP/FTPS downloads and uploads with .filepart staging and PTY progress reporting against live FTP and FTPS servers. |
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
| `s3.multipart-restart-and-copy` | verified | live-s3, unit | Interrupted multipart uploads persist completed part ETags in a local sidecar to resume remaining parts, and large server-side copies use multipart UploadPartCopy with byte ranges. |
| `s3.object-options` | verified | live-s3, unit | S3 rawsettings configure signed x-amz-storage-class, x-amz-acl, x-amz-server-side-encryption, KMS key ID, x-amz-request-payer and custom x-amz-meta-* headers on uploads and copies. |

## Transfers

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `transfer.masks` | verified | live-sftp, unit | Name/path/directory masks and size/time criteria filter transfers and synchronization. |
| `transfer.ascii-automatic` | verified | live-dav | ASCII conversion is byte-checked through a live upload; automatic mode selects text files by mask. |
| `transfer.filepart-resume` | verified | live-sftp | Interrupted uploads/downloads stage in .filepart and continue there rather than splicing the final destination. |
| `transfer.resume-modes` | verified | live-sftp, script | Automatic/on/off and -resumesupport thresholds are parsed; resume-off transfers without staging. |
| `transfer.time-permissions` | verified | live-sftp | SFTP preserves file and directory timestamps (-preservetime=all) and applies -permissions modes on uploads and downloads. |
| `transfer.speed-and-append` | verified | live-sftp | Native SFTP supports -append without overwriting existing bytes and -speed= bandwidth throttling; curl uses --limit-rate and refuses append. |
| `transfer.presets` | verified | live-dav, script | Named, per-site and rule-selected presets change live transfer settings. |
| `transfer.desktop-queue` | verified | gui, gui-browser-upload | GUI task queue supports pause, SIGINT cancel, retry and queue reordering (/api/queue/priority) with private staging for browser uploads. |
| `transfer.curl-progress` | verified | live-curl, unit | curl-backed downloads and uploads stream curl's progress meter into the byte-progress callback and render live PTY progress percentages while staging downloads through .filepart. |

## Synchronization

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sync.one-way` | verified | live-dav | Local-to-remote keepuptodate and recursive one-way planning/upload run against the live WebDAV server. |
| `sync.both-state` | verified | live-sftp, unit | A persistent state file distinguishes one-sided changes and deletions from two-sided conflicts and applies -conflict=skip\|local\|remote\|newer against a live SFTP server. |
| `sync.checksum` | verified | live-sftp, unit | Checksum criteria find same-size/same-time changes and stop if contents cannot be hashed. |
| `sync.exclusions-delete-guard` | verified | live-sftp | An excluded subtree is pruned from the plan and cannot be deleted at the destination. |
| `sync.keepuptodate-inotify` | verified | live-dav | Nested local arrivals wake the recursive inotify watcher before the rescan interval. |
| `sync.criteria-and-mirror` | verified | live-sftp, script, unit | Time/size/checksum criteria, -mirror and two-way conflict policies are validated in script and unit suites and exercised against a live SFTP server. |
| `sync.conflict-ui` | verified | gui, gui-browser-upload, live-sftp, unit | Both-direction sync exposes -conflict=skip\|local\|remote\|newer in the CLI and interactive conflict-policy resolution controls in the GUI synchronize preview. |

## Sessions/configuration

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `sessions.site-manager` | verified | script | Stored sites can be saved, opened, removed, exported and imported. |
| `sessions.password-at-rest` | verified | live-sftp | Stored site passwords are encrypted under sessions.key and shared between the CLI and GUI. |
| `sessions.proxy-settings` | verified | live-sftp | Upstream proxy setting names are applied by open and stored with a site; both SOCKS5 and HTTP CONNECT proxies carry live SFTP sessions and refuse bad credentials. |
| `sessions.rawsettings` | verified | live-curl, script, unit | 'open' and 'savesession' take '-rawsettings' with upstream's names: the proxy settings (ProxyMethod, ProxyHost, ProxyPort, ProxyUsername, ProxyPassword, ProxyTelnetCommand, ProxyLocalCommand, ProxyDNS, ProxyLocalhost, ProxyExcludeList), the FTP engine's policy (FtpPasvMode, FtpForcePasvIp, FtpUseMlsd, FtpAccount) and the TLS policy (Ftps, MinTlsVersion, MaxTlsVersion, TlsCertificateFile, TlsCertificateKeyFile, CaCertificateFile, CaCertificateDir, PinnedPublicKey). A name or value outside that set is refused by name before any connection, so a setting is never stored and then dropped at open time, and 'savesession' writes what it applied under the keys a saved site reads back. |
| `sessions.folder-tree` | verified | gui, script, unit | Slash-delimited session names form a folder hierarchy supporting 'sessions folders', 'sessions <folder>/', 'rmsession -folder' and folder grouping/deletion in the GUI site manager. |

## Security/authentication

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `security.native-hostkey` | verified | live-sftp | Native SFTP refuses an unknown/unexpected host key until it is pinned. |
| `security.password-key-agent` | verified | live-sftp, script, unit | Password, unencrypted and passphrase-protected private-key (-passphrase=), ssh-agent and copyid public/private key installation work against the live SFTP server. |
| `security.curl-pins-and-tls` | verified | live-curl, script, unit | '-hostkey=' accepts the fingerprint spellings people paste - a SHA-256 digest in hex with optional ':' separators, OpenSSH's SHA256:base64 and curl's sha256//base64 - and converts between them, so the SSH protocols pin the host key with --hostpubsha256 where the installed curl implements it (its libssh2 backend) and with --hostpubmd5 for a 32-hex MD5 digest. A curl that accepts --hostpubsha256 and ignores it - the libssh backend Debian 13 and Ubuntu 24.04 link - is detected before the connection and the session is refused by name, so the pin is enforced or the session does not start. A TLS session keeps chain validation (the system store, or CaCertificateFile and CaCertificateDir) and pins the server's public key with --pinnedpubkey; a value that is not a pin curl can compare is refused with the setting to use instead, and '*' is the only value that disables the checks. |
| `security.proxy-isolation` | verified | live-dav, live-sftp | SFTP SOCKS5 and HTTP CONNECT proxies and curl proxy routing are verified live; wrong proxy credentials and unsupported proxy modes are refused rather than bypassed. |
| `security.gui-api-token` | verified | gui-browser-upload | The local browser API rejects requests missing its per-run token. |
| `security.gssapi-and-tunnels` | missing | — | GSSAPI is compiled out; SSH tunnel configuration is not implemented. **Next:** Implement and test GSSAPI/SSH tunnels only with an explicit security review. |
| `security.gui-account` | missing | — | The loopback GUI has no multi-user account or authorization roles. **Next:** Add authenticated accounts before claiming multi-user/server deployment support. |

## GUI

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `gui.commander-panes-tabs` | verified | gui | The browser renders local/remote panes and session tabs and routes Commander function keys. |
| `gui.transfer-queue-controls` | verified | gui, gui-browser-upload | The transfer queue renders progress, retry, SIGINT cancel and priority reordering controls (/api/queue/priority). |
| `gui.sync-preview` | verified | gui | The preview renders planned actions and posts the chosen comparison on run. |
| `gui.synchronized-browsing` | verified | gui | Entering/leaving paired folders follows both panes and refuses a missing counterpart. |
| `gui.pane-drag` | verified | gui | Dragging a selected item between panes queues exactly those names; same-pane drops do nothing. |
| `gui.desktop-file-drop` | verified | gui, gui-browser-upload | A desktop File is staged under an authenticated request with exact bytes and a remote destination. |
| `gui.desktop-folder-drop` | verified | gui, gui-browser-upload, live-dav | Entry/handle recursion queues nested files and empty directories; parent creation is idempotent, paths validated. |
| `gui.desktop-drop-compatibility` | verified | gui, gui-browser-upload | FileSystemEntry, FileSystemHandle and plain File drops preserve nested paths and empty folders across both remote and local panes with path-traversal rejection. |
| `gui.desktop-local-drop` | verified | gui, gui-browser-upload | Dropping files or folders from the desktop onto the local pane saves them into the active local directory via /api/local/drop and /api/local/drop-dir with path confinement. |
| `gui.explorer-custom-commands` | missing | — | No separate upstream Explorer-style UI or custom-command framework. **Next:** Build/test Explorer navigation and custom-command execution with appropriate safety prompts. |

## Editing/search

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `editing.remote-editor` | verified | gui, live-sftp, unit | Remote editing downloads, checks modification times and full file contents, supports -eol=lf\|crlf and -monitor in the CLI, and saves with conflict detection in the GUI. |
| `editing.local-editor` | verified | gui-local-editor | GUI reads text and saves locally with a conflict check and atomic replace; symlinks are refused. |
| `editing.filename-and-content-search` | verified | gui-search | Filename search and bounded literal UTF-8 content search skip binary/oversized files and cap the scan. |
| `editing.monitor-local-changes` | verified | live-sftp, unit | 'edit -monitor' watches the temporary local file while the external editor remains open and uploads each save after verifying the remote file was not modified concurrently. |
| `editing.encoding-policy` | verified | gui-local-editor, live-sftp, unit | The browser editor and search enforce UTF-8 text and reject binary files, while CLI 'edit -eol=lf\|crlf' normalizes line endings before upload. |

## Automation

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `automation.command-interpreter` | verified | script | Batch abort/continue, aliases, abbreviated commands and rejected unknown options are checked server-free. |
| `automation.parameter-and-timestamp` | verified | script | %1%..%9%, timestamp formats and environment expansion follow tested script behavior. |
| `automation.literal-gui-command` | verified | gui-browser-upload, script | CLI -literal suppresses expansion, and both the GUI command runner and its background task queue use it so names containing %...% stay literal. |
| `automation.xml-action-log` | verified | live-sftp, script, unit | The XML action log (-xmllog=) emits structured <upload>, <download>, <rm>, <mkdir>, <mv>, <chmod>, <touch> and <call> elements alongside command and message records with secrets redacted. |
| `automation.dotnet` | missing | — | No WinSCP .NET assembly or equivalent object model for Linux. **Next:** Define an API contract and test automation clients; a CLI is not the assembly. |
| `automation.powershell` | missing | — | No PowerShell cmdlets or compatibility shim. **Next:** Provide and test PowerShell automation only if this is a product requirement. |

## Desktop integration

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `desktop.scheme-handlers` | verified | gui-desktop-uri | The desktop entry and GUI parse sftp://, ftp:// and ftps:// startup URLs with escaped paths. |
| `desktop.launcher-prefix` | verified | gui-desktop-uri, script | Installed, Makefile and CMake binaries select their own GUI launcher; the GUI finds the matching core under a relocatable prefix. |
| `desktop.file-manager-drops` | verified | gui, gui-browser-upload | Desktop file and folder drops work on both the remote pane (queued upload) and the local pane (atomic local save) with traversal rejection. |
| `desktop.shell-and-extensions` | missing | — | No system file-manager shell commands or upstream extension system. **Next:** Specify desired desktop environment integrations and build/test them individually. |

## Localization/accessibility

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `locale.translations` | missing | — | No upstream language catalogs or translation pipeline. **Next:** Choose supported locales and add translated strings with GUI/CLI language tests. |
| `locale.accessibility` | verified | gui | The Commander web UI exposes ARIA landmark/grid/tablist/dialog roles, aria-selected state and keyboard navigation (ArrowUp/ArrowDown/Home/End/Enter) across panes. |

## Packaging/platforms

| Capability | Linux status | Automated evidence | Scope / remaining gap |
| --- | --- | --- | --- |
| `platform.make-linux` | verified | script | source/linux/Makefile builds the native Linux/PuTTY client, refuses non-Linux hosts and provides an install target staged under DESTDIR/PREFIX. |
| `platform.cmake-linux` | verified | script | CMakeLists.txt builds the Linux binaries, registers CTest suites, refuses non-Linux hosts and installs the CLI, GUI, desktop entry and man page. |
| `platform.debian-package` | verified | script | debian/rules and debian/control package the CLI, GUI, desktop entry, icons and man page matching the staged Linux install tree. |
| `platform.prefix-lookup` | verified | gui-desktop-uri | An arbitrary PREFIX/share/linuxscp/gui resolves PREFIX/bin/linuxscp before /usr/bin. |
| `platform.posix-runtime-services` | verified | unit | The Linux platform layer supplies temporary directories, home/config lookup and concurrent child stdout/stderr capture. |
| `platform.terminal-and-console` | verified | script, unit | Linux command-line parsing, terminal-facing configuration and the native console entry point are built from the Linux sources. |
| `platform.linux-file-events` | verified | live-dav | The Linux synchronization watcher uses filesystem events and retains interval polling as a safety net when a watch cannot be installed. |
