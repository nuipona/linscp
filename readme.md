<p align="center">
  <img src="logo/linuxscp-mark.png" alt="LinSCP logo" width="160" height="160" />
</p>

<img width="1681" height="1148" alt="image" src="https://github.com/user-attachments/assets/642f19d0-e14f-4f45-a8d7-02b2654d77b5" />

# LinSCP

LinSCP is a free, open-source file transfer client for Debian GNU/Linux. It has
a command-line client with a WinSCP-style scripting language, and a
browser-based dual-pane Commander (`linuxscp-gui`).

LinSCP implements a subset of WinSCP's workflows. It is **not full WinSCP
parity**: see [Protocols](#protocols) and the [parity audit](docs/upstream-parity.md).

## Requirements

- Debian 12 (bookworm) or newer, with a GCC or Clang toolchain.
- `curl` at run time, for FTP transfers, FTPS, WebDAV and S3.
- Python 3, for the graphical client.

```sh
sudo apt install build-essential dpkg-dev curl python3
```

LinSCP is Linux-only. The Makefile refuses to build on other systems.

## Building and testing

```sh
make -C source/linux                 # -> source/linux/build/linuxscp
make -C source/linux check           # unit tests
make -C source/linux check-live      # script and live-server suites
```

CMake builds the same client and registers the suites with CTest:

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

The live suites use optional Python packages: `paramiko` (SFTP),
`botocore` (S3 signature reference), and `pyftpdlib` with `pyOpenSSL`
(FTP/FTPS). The SCP suite needs `openssh-server`; it starts its own `sshd` on a
free port. A suite that skips a case says so on its evidence line.

The GUI suites (`source/gui/tests/`) run separately, as in CI. For the
Debian package:

```sh
dpkg-buildpackage -b -us -uc         # -> ../linuxscp_1.0-2_<arch>.deb
```

## Usage

```sh
linuxscp -batch -script=script.txt
linuxscp -batch -command="open sftp://user@example.com/; cd /data; get *.csv /tmp"
linuxscp-gui                         # browser Commander on 127.0.0.1:8080
```

Run `linuxscp -help` for the options and `man linuxscp` for the full reference.

## Protocols

| URL | Served by | Notes |
| --- | --- | --- |
| `sftp://` | native SFTP over the bundled PuTTY core | full command set |
| `scp://`, `ssh://` | the client's SSH code: the server's shell for directory work, rcp for bytes | no resume; the protocol has no offset |
| `ftp://` | native FTP engine for listing and metadata; curl for transfers | |
| `ftps://` | curl, implicit TLS; `ftp://` with explicit TLS settings also uses curl | |
| `dav://`, `webdav://` | curl, with PROPFIND, MOVE, COPY, locks and dead properties | partial |
| `s3://` | curl, with SigV4 signing and multipart transfers | partial |

Host keys are verified. `-hostkey=` takes a SHA-256 or MD5 fingerprint. An
unknown key in a run that cannot prompt is refused, and the key is printed so
it can be pinned.

## Features

- **Transfers:** binary, ASCII and automatic modes; WinSCP file masks; recursive
  `get` and `put`; resume through `.filepart` staging; `-append` (SFTP only);
  `-speed=` limits; `-preservetime`; transfer presets.
- **Synchronization:** `synchronize` in one or both directions, and
  `keepuptodate`, which watches the local tree with inotify.
- **Remote operations:** `chmod` (octal and symbolic), `cp`, `mv`, `ln`,
  `rm`, `mkdir`, `stat`, `checksum`, `edit`, `call`, and `copyid`.
- **Scripting:** unambiguous command and option abbreviations, `%1%`..`%9%`
  parameters, `%TIMESTAMP%` and environment expansion, `option batch` and
  `option confirm`, and an XML action log (`-xmllog=`).
- **Stored sessions:** shared between the CLI and the GUI, with folders,
  encrypted passwords in `sessions.key`, and proxy settings through
  `-rawsettings`.
- **Graphical client:** dual-pane layout, transfer queue, remote search,
  local file editing and drag-and-drop uploads from a file manager.
- **Localization:** English and Spanish, for the CLI and the GUI.

Not implemented: the .NET assembly API, GSSAPI, SSH tunnel configuration, and
multi-user access to the GUI. `automation/LinSCP.psm1`, a PowerShell 7 wrapper
over the CLI, is installed by the Makefile but no automated test runs it.

## Documentation

- [`docs/debian.md`](docs/debian.md): options, commands, protocols,
  authentication, packaging and troubleshooting.
- [`docs/linuxscp.1`](docs/linuxscp.1): the manual page.
- [`docs/upstream-parity.md`](docs/upstream-parity.md): the parity audit, its
  evidence rules and how to maintain them.
- [`docs/linux-feature-inventory.md`](docs/linux-feature-inventory.md) and
  [`docs/remaining-parity-plan.md`](docs/remaining-parity-plan.md): generated
  from `docs/upstream-parity.json`. Regenerate them with
  `python3 tools/check_upstream_parity.py --write-report`.
- [`docs/localization.md`](docs/localization.md): language selection and catalogues.

## Repository layout

```
automation/    PowerShell 7 wrapper over the CLI (not tested)
debian/        Debian packaging
docs/          documentation, manual page and parity inventory
libs/putty/    POSIX platform layer and charset code from PuTTY 0.85
logo/          logo artwork
source/gui/    browser Commander: Python server, web client, tests
source/linux/  linuxscp client: front end, protocol glue, tests, Makefile
source/putty/  platform-neutral PuTTY SSH and SFTP core
tools/         parity inventory and translation checkers
```

## Licence

LinSCP is distributed under the GNU General Public License version 3. The
attribution and third-party notices are in [`license.txt`](license.txt). The
logo artwork is not GPL-licensed; see section B of that file.
