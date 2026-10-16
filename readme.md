<p align="center">
  <img src="logo/linuxscp-mark.png" alt="LinSCP logo" width="160" height="160" />
</p>

# LinSCP

LinSCP is a free, open-source file transfer client for Debian GNU/Linux. It
connects to SFTP servers over SSH and transfers files from a command line, a
script or an interactive prompt. The client is built from source with `make`
and packaged as a `.deb`.

The logo is a fairy penguin (*Eudyptula minor*, the smallest penguin) with a
blue arrow arriving from the left and a green arrow leaving to the right -
downloads in, uploads out.

## Requirements

Debian 12 (bookworm) or newer, on any architecture with a GCC or Clang
toolchain. Native SFTP uses the bundled PuTTY core; the other transfer paths
require a protocol-capable `curl` executable. The GUI requires Python 3. No
external SSH library is required for native SFTP.

    sudo apt install build-essential dpkg-dev curl python3

LinSCP is a Linux application: the console client, the native SFTP transport,
the `keepuptodate` watcher, the stored-session files and the desktop
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
`botocore`, and the FTP/FTPS live tests use `pyftpdlib` plus `pyOpenSSL`;
the SCP case additionally uses a local `sshd` when available. Their evidence
lines report skipped cases when optional dependencies are missing. CI installs
the Python dependencies.

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
parity**. It has a command-line interpreter, native SFTP through the bundled
PuTTY core, curl-backed transfer paths, basic recursive transfers, resume and
synchronization, and a browser-based dual-pane Commander interface
(`linuxscp-gui`) with a dark theme, WinSCP-style menu and F1–F10 layout, local
and remote file viewing/editing, a transfer queue and bounded UTF-8 full-text
search of remote files. The `keepuptodate` command reflects local changes onto
the remote side, watching the local tree recursively with inotify and falling
back to interval polling. The remote pane queues files and folders dropped
from a file manager, retaining nested paths and empty directories where the
browser exposes file-entry APIs; local-pane and cross-desktop drops still need
work.

Protocol URL acceptance is not proof of complete support: WebDAV lists and
stats through PROPFIND, moves with MOVE, and file copies with COPY (falling
back to a streamed copy when the server refuses COPY); `lock`/`unlock` take
an exclusive write lock whose token every later upload, delete and move of
that path carries (the `If` header), and `property -get`/`-set` reads and
writes dead properties. Upstream's WebDAV backend has no locking or dead
properties at all, so those are LinSCP extensions; collection-level
preconditions beyond the lock token remain gaps. Amazon S3 signs with
Signature Version 4, lists and stats objects, uses CopyObject for remote
copies, and switches uploads larger than 16 MiB to multipart requests.
Interrupted multipart uploads restart from the first part; ACLs, versioning,
storage classes and server-side encryption options remain unsupported.

Host keys are verified rather than accepted: `-hostkey=` takes a SHA-256 pin
(hex, `SHA256:base64` or `sha256//base64`) or an MD5 one, and where the
installed curl cannot enforce a SHA-256 pin (its `--hostpubsha256` needs the
libssh2 backend, not libssh) the session is refused with that statement
instead of being used with the pin dropped. Scripting covers the WinSCP
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
this build cannot use is refused by name instead of being dropped. The .NET
assembly and PowerShell automation APIs, SSH tunnels and session folder trees
remain open gaps.

See [the upstream parity audit and completion plan](docs/upstream-parity.md)
and its [Linux capability matrix](docs/linux-feature-inventory.md) for the
pinned upstream baseline, named test evidence, limitations and remaining gaps.
The matrix currently has 87 scoped capabilities across 15 areas; it is not
an exhaustive behavioral parity certification. Refresh it after changing
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

## Naming

The product is branded **LinSCP**. The established `linuxscp` executable,
`linuxscp(1)` manual page, `linuxscp-gui` launcher, package name and on-disk
configuration paths are retained for compatibility. `LinSCP` is the name in the
user-visible strings; `LinuxSCP` remains the name of the configuration
directory (`~/.config/LinuxSCP`). The upstream PuTTY and
WinSCP code keeps its original attribution; see [`license.txt`](license.txt).

## Licence

The code is distributed under the GNU General Public License version 3, with
the attribution and third-party notices described in
[`license.txt`](license.txt). The logo artwork in `/logo` is not under the GPL;
see section B of that file.
