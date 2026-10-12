<p align="center">
  <img src="logo/linuxscp-mark.png" alt="LinuxSCP logo" width="160" height="160" />
</p>

# LinuxSCP

LinuxSCP is a free, open-source file transfer client and remote file manager
for Windows. It moves files over SFTP, SCP, FTP, FTPS, WebDAV and Amazon S3, or
between two local folders, in a twin-panel or Explorer-style window, and it can
be driven from batch scripts, the command-line interface or a .NET assembly.

The logo is a fairy penguin (*Eudyptula minor*, the smallest penguin) with a
blue arrow arriving from the left and a green arrow leaving to the right -
downloads in, uploads out.

## Building

You need:

- [Embarcadero C++Builder 11 Professional](https://www.embarcadero.com/products/cbuilder)
- [Build Tools and Agents for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) (for C# 9.0)
- [nasm](https://www.nasm.us/) (store it in `buildtools/tools/nasm.exe`)
- [Object file converter](https://www.agner.org/optimize/#objconv) (store it in `buildtools/tools/objconv.exe`)

To build LinuxSCP yourself, adjust and run [`build.bat`](build.bat) in the root
folder. It builds the native libraries, the `LinuxSCP.cbproj` project group and
the `LinuxSCPnet` .NET assembly.

## Repository layout

    /source                 project files of all native libraries and executables
    /source/components      native LinuxSCP visual components
    /source/console         console interface (LinuxSCP.com)
    /source/core            core (non-visual) part of LinuxSCP
                            (SSH, SFTP, FTP, WebDAV, S3 and SCP code)
    /source/dragext         drag&drop shell extension
    /source/filezilla       source code of FileZilla FTP client
    /source/forms           visual part of LinuxSCP (dialogs and windows)
    /source/packages        general visual components (own and 3rd party)
    /source/putty           source code of PuTTY SSH client
    /source/resource        resource strings and application icons
    /source/windows         other sources
    /deployment             Inno Setup script for the setup package
                            (see deployment/readme.txt), ADMX templates,
                            NuGet packaging metadata
    /dotnet                 source code of the LinuxSCP .NET assembly
    /libs                   3rd party libraries
    /logo                   project logo artwork and exports
    /translations           translations of LinuxSCP strings

## Naming

Everything in this tree is branded `LinuxSCP`: `linuxscp.exe` and
`linuxscp.com`, the `LinuxSCP.ini` configuration file, the `LinuxSCP` /
`LinuxSCP.com` installers, the `LinuxSCPnet` .NET assembly and NuGet package,
the `LinuxSCP` registry path under `HKCU\Software\nuipona`, the machine policy
path `HKLM\SOFTWARE\Wow6432Node\nuipona\LinuxSCP 2` used by the ADMX
templates in `deployment/admx`, and the session-log XML namespace
`https://github.com/nuipona/linuxscp/schema/session/1.0`. If you rebrand
further, `deployment/linuxscpsetup.iss` and `source/resource/TextsWin1.rc` hold
most of the shared definitions.

This fork sends no usage statistics and checks no vendor server for updates;
see section C of [`license.txt`](license.txt).

## Licence

The code is distributed under the GNU General Public License version 3, with
the attribution and third-party notices described in
[`license.txt`](license.txt). The logo and icon artwork in `/logo` and
`/source/resource` is not under the GPL; see section B of that file.
