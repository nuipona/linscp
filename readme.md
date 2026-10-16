<p align="center">
  <img src="logo/linuxscp-mark.png" alt="LinSCP logo" width="160" height="160" />
</p>

<img width="1681" height="1148" alt="image" src="https://github.com/user-attachments/assets/642f19d0-e14f-4f45-a8d7-02b2654d77b5" />


# LinSCP

LinSCP is a free, open-source file transfer client for Debian GNU/Linux. It
connects to SFTP and SCP servers over SSH and transfers files from a command
line, a script or an interactive prompt. The client is built from source with `make`
and packaged as a `.deb`.

## Requirements

Debian 12 (bookworm) or newer, on any architecture with a GCC or Clang
toolchain. Both SSH protocols use the bundled PuTTY core - SFTP as the
filesystem protocol it is, SCP as the login shell plus rcp transfer protocol it
actually is - and no external SSH library is needed for either. The remaining
transfer paths (FTP/FTPS, HTTP/WebDAV, Amazon S3) require a protocol-capable
`curl` executable. The GUI requires Python 3.

    sudo apt install build-essential dpkg-dev curl python3

LinSCP is a Linux application: the console client, the native SFTP and SCP
transports, the `keepuptodate` watcher, the stored-session files and the desktop
integration are all built for Linux, and nothing else is supported or tested.

## Building

    make -C source/linux            # -> source/linux/build/linuxscp
    make -C source/linux check      # builds and runs the unit tests
    make -C source/linux check-live # the script and live-server suites
    make -C source/linux clean

CMake builds the same client, and registers the test suites with CTest:

    cmake -S . -B build && cmake --build build -j
    ctest --test-dir build --output-on-failure
    python3 tools/check_upstream_parity.py # evidence/report drift check

Live SFTP tests require `paramiko`, the S3 signature-reference test uses
`botocore`, and the FTP/FTPS live tests use `pyftpdlib` plus `pyOpenSSL`. The
live SCP suite needs no Python package: it starts a throwaway `sshd` of its own
on a port the kernel picks, with keys it generated for the run, so it needs
`openssh-server` and skips loudly, per case, without it. Every suite's evidence
line reports the cases it skipped, so a run that checked less says so.

To build a Debian package:

    dpkg-buildpackage -b -us -uc    # -> ../linuxscp_1.0-2_<arch>.deb
    sudo dpkg -i ../linuxscp_1.0-2_*.deb

See [`docs/debian.md`](docs/debian.md) for details, usage and the verified
feature list. The manual page is [`docs/linuxscp.1`](docs/linuxscp.1).

## Usage

    linuxscp -batch -script=script.txt
    linuxscp -batch -command="open sftp://user@example.com/ /data ; ls ; get * /tmp"

## Commander interface and upstream parity

LinSCP implements a subset of WinSCP workflows, **not full upstream feature
parity**. It has a command-line interpreter, native SFTP and native SCP through
the bundled PuTTY core, curl-backed transfer paths for the protocols that have
no engine of their own, basic recursive transfers, resume and
synchronization, and a browser-based dual-pane Commander interface
(`linuxscp-gui`) with a dark theme, WinSCP-style menu and F1–F10 layout, local
and remote file viewing/editing, a transfer queue and bounded UTF-8 full-text
search of remote files. The `keepuptodate` command reflects local changes onto
the remote side, watching the local tree recursively with inotify and falling
back to interval polling. The remote and local panes accept files and folders dropped from a file
manager, retaining nested paths and empty directories where the browser
exposes file-entry APIs.

An `scp://` session is served by the client itself, the way upstream drives
it: SCP is not a filesystem protocol but a login shell plus the rcp transfer
protocol that runs on it, so a listing is an `ls -l -a --full-time` the client
parses (exact timestamps, names with spaces, symbolic links kept as links), a
`stat` is an `ls -l -d`, `cd` is `cd <path> && pwd`, and the directory and
metadata commands are the shell's own `mkdir`, `rmdir`, `rm`, `mv`, `chmod`,
`cp -p`, `ln -s` and `touch -t`. The bytes go through the server's own
`scp -f`/`scp -t` on an SSH channel whose wire protocol the client speaks -
timestamps, modes, sizes, acknowledgements, recursive trees - and every path is
quoted for a POSIX shell, with a path holding a control character refused
instead of being sent. What the protocol cannot do is continue a partial
transfer: it has no offset, so `-resume` is refused by name and a `.filepart`
an interrupted run left behind is discarded rather than spliced onto. A refusal
arrives as the sentence the server printed.

Protocol URL acceptance is not proof of complete support: WebDAV lists and
stats through PROPFIND, moves with MOVE, and file copies with COPY (falling
back to a streamed copy when the server refuses COPY); `lock`/`unlock` take
an exclusive write lock whose token every later upload, delete and move of
that path carries (the `If` header), and `property -get`/`-set` reads and
writes dead properties. Upstream's WebDAV backend has no locking or dead
properties at all, so those are LinSCP extensions. Amazon S3 signs with
Signature Version 4, lists and stats objects, uses CopyObject (and multipart
UploadPartCopy for large objects) for remote copies, switches uploads larger
than 16 MiB to multipart requests with local sidecar state to resume
interrupted multipart uploads, and signs storage class, ACL, server-side
encryption, requester-pays and custom metadata headers via `-rawsettings`.

Host keys are verified rather than accepted: `-hostkey=` takes a SHA-256 pin
(hex, with or without `:` separators, `SHA256:base64` or `sha256//base64`) or
an MD5 one (with or without the `MD5:` marker `ssh-keygen` prints), and both
SSH protocols compare it with the fingerprints the bundled SSH core reports. A
key that does not match is refused, an unknown key in a run nobody can answer a
prompt in is refused with the key printed so that it can be pinned next time,
and a key that was checked is remembered. No SSH session is handed to curl, so
what a pin enforces no longer depends on which SSH library the installed curl
was linked against. Scripting covers the WinSCP
transfer switches (file masks, `-transfer=`,
`-resume`, `-append`, `-speed=`, `-resumesupport=`, `-preservetime`) with
upstream's `.filepart` staging for resume-capable transfers, named and
rule-based transfer presets, unambiguous command and option abbreviations,
script parameters with `%TIMESTAMP%` and environment expansion, and an XML
action log. A session can be reached through a proxy: `open -rawsettings
ProxyMethod=SOCKS5 ProxyHost=... ProxyPort=...` applies upstream's proxy
settings (SOCKS 4 and 5, HTTP CONNECT, a telnet or local command, with their
DNS, localhost and exclude-list policy), `savesession -rawsettings` stores
them with the site and encrypts the proxy password, and a setting or proxy
this build cannot use is refused by name instead of being dropped. Stored
sessions support slash-delimited folder hierarchies (`sessions folders`,
`rmsession -folder`) in both the CLI and the GUI site manager. The .NET
assembly API, GSSAPI and SSH tunnels remain intentional non-goals on Linux.
`automation/LinSCP.psm1` is a PowerShell shim over the console client that no
automated case exercises yet, so what it proves is nothing beyond the CLI it
wraps.

See [the upstream parity audit and completion plan](docs/upstream-parity.md)
and its [Linux capability matrix](docs/linux-feature-inventory.md) for the
pinned upstream baseline, named test evidence, limitations and remaining gaps.
The generated inventory covers upstream registrations, Linux additions,
feature areas, granular capabilities and linked evidence; its companion plan
lists acceptance criteria for every tracked gap. Neither is an exhaustive
behavioral parity certification. Refresh both after changing
`docs/upstream-parity.json` with
`python3 tools/check_upstream_parity.py --write-report`.
Unsupported compatibility options fail explicitly instead of silently doing
nothing. Batch scripts default to stopping after errors; `option batch continue`
permits subsequent commands while preserving a failing process exit status.

The supported commands and validation history are listed in
[`docs/debian.md`](docs/debian.md). The manual page is
[`docs/linuxscp.1`](docs/linuxscp.1).

## Repository layout

    /debian                 Debian packaging (control, rules, changelog, copyright)
    /docs                   build and usage documentation, manual page
    /libs/putty             POSIX platform layer and charset code vendored from
                            PuTTY 0.85 (libs/putty/unix, libs/putty/charset)
    /logo                   project logo artwork
    /source/gui             the browser Commander: Python server, web client
                            and its tests
    /source/linux           the linuxscp client: front end, SFTP glue, tests
                            and the Makefile
    /source/putty           PuTTY SSH/SFTP core (platform-neutral)
    /tools                  the upstream parity inventory checker

## Licence

The code is distributed under the GNU General Public License version 3, with
the attribution and third-party notices described in
[`license.txt`](license.txt). The logo artwork in `/logo` is not under the GPL;
see section B of that file.
