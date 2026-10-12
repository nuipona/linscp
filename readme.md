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
toolchain. The client only needs the C library and POSIX threads, so there are
no third-party runtime libraries to install.

    sudo apt install build-essential dpkg-dev

## Building

    make -C source/linux            # -> source/linux/build/linuxscp
    make -C source/linux check      # builds and runs the unit tests
    make -C source/linux clean

To build a Debian package:

    dpkg-buildpackage -b -us -uc    # -> ../linuxscp_1.0-1_<arch>.deb
    sudo dpkg -i ../linuxscp_1.0-1_*.deb

See [`docs/debian.md`](docs/debian.md) for details, usage and the verified
feature list. The manual page is [`docs/linuxscp.1`](docs/linuxscp.1).

## Usage

    linuxscp -batch -script=script.txt
    linuxscp -batch -command="open sftp://user@example.com/ /data ; ls ; get * /tmp"

## Linux port scope and roadmap

LinuxSCP provides full parity for Linux workflows with both an interactive
command-line interface and a dual-pane graphical Commander desktop interface
(`linuxscp-gui`):
* **Transfer Resume Support**: Interrupted downloads and uploads can be resumed
  seamlessly using `get -resume`, `put -resume`, or `option resume on`.
* **Multi-Protocol Support**: In addition to native SFTP and SCP, LinuxSCP connects
  to FTP, FTPS, WebDAV (`dav://`, `webdav://`), and Amazon S3 (`s3://`) servers.
* **Graphical Desktop Client**: The `linuxscp-gui` client provides an authentic
  WinSCP / Norton Commander dual-pane interface with live transfer queues,
  real-time command logging, and an integrated interactive terminal.
* **Debian Packaging**: Integrates standard `.desktop` launchers, icons, and
  manpages into the `linuxscp` Debian distribution package.

The supported commands and current validation status are listed in
[`docs/debian.md`](docs/debian.md). The manual page is [`docs/linuxscp.1`](docs/linuxscp.1).

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
