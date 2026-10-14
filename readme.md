<p align="center">
  <img src="logo/linuxscp-mark.png" alt="LinuxSCP logo" width="160" height="160" />
</p>

# LinuxSCP

LinuxSCP is a free, open-source file transfer client for Debian GNU/Linux. It
connects to SFTP servers over SSH and transfers files from a command line, a
script or an interactive prompt. The client is built from source with `make`
and packaged as a `.deb`.

The logo is a fairy penguin (*Eudyptula minor*, the smallest penguin) with a
blue arrow arriving from the left and a green arrow leaving to the right -
downloads in, uploads out.

## Requirements

Debian 12 (bookworm) or newer, on any architecture with a GCC or Clang
toolchain. Native SFTP uses the bundled PuTTY core; other transfer paths require a
protocol-capable `curl` executable. The GUI requires Python 3. No external
SSH library is required for native POSIX SFTP.

    sudo apt install build-essential dpkg-dev curl python3

On Windows: Windows 10 (1803) or later with CMake and Visual Studio
2022 or MinGW-w64. The resulting `linuxscp.exe` needs a protocol-capable
`curl.exe` at run time (plus Python 3 for the GUI). The in-box curl build
may not provide SSH protocols. See
[`docs/windows.md`](docs/windows.md).

## Building

    make -C source/linux            # -> source/linux/build/linuxscp
    make -C source/linux check      # builds and runs the unit tests
    make -C source/linux clean

On Windows (or via CMake on any platform):

    windows\build.cmd               :: -> build-windows\Release\linuxscp.exe
    cmake -S . -B build && cmake --build build   # Linux/CMake alternative

To build a Debian package:

    dpkg-buildpackage -b -us -uc    # -> ../linuxscp_1.0-1_<arch>.deb
    sudo dpkg -i ../linuxscp_1.0-1_*.deb

See [`docs/debian.md`](docs/debian.md) for details, usage and the verified
feature list. The manual page is [`docs/linuxscp.1`](docs/linuxscp.1).

## Usage

    linuxscp -batch -script=script.txt
    linuxscp -batch -command="open sftp://user@example.com/ /data ; ls ; get * /tmp"

## Linux port scope and upstream parity

LinuxSCP implements a subset of WinSCP workflows, **not full upstream feature
parity**. It has a command-line interpreter, native SFTP on POSIX, curl-backed
transfer paths, basic recursive transfers/resume/synchronization, and a
browser-based dual-pane Commander interface (`linuxscp-gui`).

Protocol URL acceptance is not proof of complete support: WebDAV directory
listing is incomplete, and authenticated Amazon S3 (signing and bucket/object
listing) is not implemented. Windows SSH protocols depend on the installed
curl build. Scripting covers the WinSCP transfer switches (file masks,
`-transfer=`, `-resume`, `-append`, `-speed=`, `-resumesupport=`,
`-preservetime`), unambiguous command and option abbreviations, script
parameters with `%TIMESTAMP%` and environment expansion, and an XML action
log; the .NET assembly and PowerShell automation APIs, transfer presets,
session folder trees and encrypted stored passwords remain open gaps.

See [the upstream parity audit and completion plan](docs/upstream-parity.md)
for the pinned upstream baseline, command inventory, limitations and tests.
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
    /source/linux           the linuxscp client: front end, SFTP glue, tests
                            and the Makefile
    /source/putty           PuTTY SSH/SFTP core (platform-neutral)

## Naming

Everything is branded `LinuxSCP`: the `linuxscp` executable, the `linuxscp(1)`
manual page and the Debian package. The upstream PuTTY and WinSCP code keeps
its original attribution, see [`license.txt`](license.txt).

## Licence

The code is distributed under the GNU General Public License version 3, with
the attribution and third-party notices described in
[`license.txt`](license.txt). The logo artwork in `/logo` is not under the GPL;
see section B of that file.
