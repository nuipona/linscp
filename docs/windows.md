# LinuxSCP on Windows

LinuxSCP runs on Windows 10 (1803) and later, and on Windows 11, with
the same commands, script syntax and exit codes as on Linux. This
document covers building, installing and using the Windows build, and
the places where Windows behaviour differs.

## Requirements

| What | Why |
| --- | --- |
| Windows 10 1803+ or Windows 11 (64-bit) | `curl.exe` first ships in-box with 1803 |
| CMake 3.16+ | the Windows build system |
| Visual Studio 2022 (MSVC) **or** MinGW-w64 GCC | C++17 compiler |
| Python 3.9+ (only for the GUI) | `linuxscp-gui` runs `linuxscp_gui.py` |
| `curl.exe` on `PATH` at run time | transfer engine for every protocol on Windows; in-box as `%SystemRoot%\System32\curl.exe` |

No third-party runtime libraries are needed. The console client is a
single `linuxscp.exe`; the GUI additionally uses the Python script and
the `web` folder next to it.

## Building

With Visual Studio:

```bat
windows\build.cmd
:: or by hand:
cmake -S . -B build-windows -A x64
cmake --build build-windows --config Release
ctest --test-dir build-windows -C Release --output-on-failure
build-windows\Release\linuxscp.exe -version
```

`windows\build.cmd` falls back to the MinGW Makefiles generator when
the Visual Studio generator is unavailable. With MinGW-w64 installed:

```bat
cmake -S . -B build-windows -G "MinGW Makefiles"
cmake --build build-windows
```

Cross-compiling from Linux (no Windows machine needed to *build*):

```sh
sudo apt install g++-mingw-w64-x86-64
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-mingw64.cmake
cmake --build build-win   # -> build-win/linuxscp.exe
```

Installing copies the GUI files next to the executable:

```bat
cmake --install build-windows --config Release --prefix "C:\Program Files\LinuxSCP"
```

which produces:

```
C:\Program Files\LinuxSCP\bin\linuxscp.exe
C:\Program Files\LinuxSCP\bin\linuxscp_gui.py
C:\Program Files\LinuxSCP\bin\linuxscp-gui.bat
C:\Program Files\LinuxSCP\bin\web\...
```

Add `...\bin` to `PATH`, or run `linuxscp.exe` by full path.

## Using the client

```bat
linuxscp.exe -batch -script=script.txt
linuxscp.exe -batch -command="open sftp://user@example.com/ /data ; ls ; get * C:\Downloads"
linuxscp.exe -gui
```

Options accept the WinSCP-style slash form as well as dashes on
Windows (`/script=script.txt`, `/batch`, `/hostkey=*`). Local paths in
commands and scripts may use backslashes or forward slashes,
drive letters (`C:\data`, `C:/data`) and UNC shares
(`\\server\share\data`); remote paths are always `/`-separated, as on
Linux. `~` expands to `%USERPROFILE%`. Wildcards (`get *.txt`) work as
usual, and local wildcard matching is case-insensitive, matching the
Windows file system.

The GUI (`linuxscp -gui`, `linuxscp-gui.bat`, or
`python linuxscp_gui.py`) opens the dual-pane Commander in the browser
at `http://localhost:8080/`. In the local pane, entering `/` lists the
available drive letters. The "Terminal" button opens an interactive
shell on the connected server, like WinSCP's "Open Terminal": PuTTY
when it is installed, otherwise Windows Terminal, and `ssh.exe` (the
in-box OpenSSH client) as a last resort.

## How SFTP works on Windows (read this)

On Linux, SFTP uses the native PuTTY engine linked into the binary.
That engine needs the PuTTY Unix platform layer, which does not exist
for Windows in this tree, so **on Windows every protocol — SFTP and
SCP included — is served by `curl.exe`** through the same
`ProtocolClient` backend that already serves FTP/FTPS/WebDAV/S3 on
Linux. The command surface is identical (`open`, `cd`, `ls`, `get`,
`put`, `rm`, `rmdir`, `mkdir`, `mv`, `chmod`, `stat`, `synchronize`,
`-resume`, `-preservetime`, ...). The differences that follow from the
backend are:

* **The in-box `curl.exe` cannot speak SFTP at all.** The build in
  `%SystemRoot%\System32` has no SSH support in it, so `sftp://` and
  `scp://` are *unsupported protocols* there rather than unreachable
  servers. `open` reports that and fails; it does not start a session that
  cannot transfer anything. Check with `curl --version` - a usable build
  lists `SSH` among its features - and put that curl earlier on `PATH` if
  you have one. Everything below assumes such a build.

* **Host keys.** curl/libssh2 verifies against the OpenSSH
  `known_hosts` file (`%USERPROFILE%\.ssh\known_hosts`) — the same
  trust store as the Windows OpenSSH client. `-hostkey=*` skips
  verification for that connection (`curl --insecure`). An MD5
  fingerprint pin (`-hostkey=aa:bb:...`) is passed to curl as
  `--hostpubmd5`; SHA-256 pins cannot be expressed for curl, so with a
  SHA-256 pin verification falls back to `known_hosts`. First contact
  with an unknown host therefore fails in `-batch` mode, exactly as on
  Linux: connect once interactively or with `ssh`/`sftp` from Windows
  OpenSSH to record the key, or use `-hostkey=*` for throwaway targets.
* **Private keys.** Use OpenSSH/PEM keys (`-privatekey=C:\path\id_ed25519`,
  forwarded to curl as `--key`). PuTTY `.ppk` files are *not* understood
  by curl; convert with PuTTYgen ("Conversions → Export OpenSSH key")
  or keep using `.ppk` with the Linux build. Pageant/ssh-agent keys are
  not offered to curl; use a key file or a password.
* **Passwords** work exactly as elsewhere: `-password=`,
  `LINUXSCP_PASSWORD`, or an interactive prompt (echo is disabled via
  the console API).
* **Directory metadata.** SFTP listings come from curl's `ls -l`-style
  output: names, sizes and permissions are exact, modification times
  are not reported, so `-neweronly` comparisons against SFTP targets
  treat the remote time as unknown and transfer. `synchronize` works,
  with the same caveat.
* **`chmod`** works for SFTP (via `chmod` quote command) and FTP
  (`SITE CHMOD`); it reports an error for WebDAV/S3, where the concept
  does not exist. (This also fixes the Linux behaviour, where `chmod`
  previously called the SFTP layer even for non-SFTP sessions.)

## Configuration and files

* Per-user state follows the platform: Linux keeps it under
  `$XDG_CONFIG_HOME/LinuxSCP` (`~/.config/LinuxSCP`); the Windows
  console client is stateless apart from curl/OpenSSH state in
  `%USERPROFILE%\.ssh`.
* Session logs (`-log=file`) are written in binary append mode on both
  platforms, so logged raw traffic is never mangled by Windows text
  translation.
* Long paths: the executable embeds a manifest with
  `longPathAware` and a UTF-8 active code page, so paths beyond the
  legacy 260-character limit and non-ASCII file names (handled as
  UTF-8 throughout) work on current Windows 10/11 builds.
* Line endings: script files written with Windows editors (CRLF) are
  accepted on both platforms; `get`/`put` always transfer bytes
  verbatim.

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| `curl was not found on PATH` | You are on a stripped Windows image; install curl or add `%SystemRoot%\System32` to `PATH`. Windows 10 1803+ always has `curl.exe`. |
| `Cannot start the LinuxSCP core binary` (GUI) | Build the console client (`windows\build.cmd`) or put `linuxscp.exe` next to `linuxscp_gui.py` / on `PATH`, then restart the GUI. |
| `Permission denied` with a key that works in PuTTY | The key is probably `.ppk`; export an OpenSSH key from PuTTYgen (see above). |
| Host key verification failed in batch mode | Record the key in `%USERPROFILE%\.ssh\known_hosts` (e.g. with Windows' `ssh` client), pin the MD5 fingerprint, or use `-hostkey=*` for one-off targets. |
| Paths with spaces fail | Quote them in scripts: `get "my file.txt" "C:\My Documents"` — a backslash is a path separator, never an escape character, in the Windows build. |
| Nothing happens on `linuxscp -gui` | Python 3 must be on `PATH`; or run `python linuxscp_gui.py` from the install `bin` folder and read its output. |

## What is verified

GitHub Actions (`.github/workflows/build.yml`) builds and tests every
push on:

* Linux with the supported Makefile (`make -C source/linux clean all check`),
* Linux with CMake,
* Windows with MSVC on `windows-latest`: CMake build, the full unit
  test suite via CTest, a console smoke test, and a GUI smoke test
  (`/api/status`, local listing of `C:\`),
* Ubuntu cross-compiling both Windows executables with MinGW-w64.

The unit tests include Windows-only coverage of drive-letter, rooted
and UNC path resolution, `~` expansion against `%USERPROFILE%`,
backslash-preserving command splitting, and process capture through
`CreateProcess`. The same sources are additionally cross-compiled with
a Windows target during development, so POSIX-only headers or APIs
cannot sneak back into the front end unnoticed.
