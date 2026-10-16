#!/usr/bin/env python3
"""
Nautilus (GNOME Files) extension for LinSCP.

Registers a right-click "Upload with LinSCP" action that uploads
the selected files/folders to the current remote directory.  The
extension is installed under ~/.local/share/nautilus-python/extensions/
and requires the nautilus-python package.

Safety: the filenames are passed as literal arguments (-literal) to the
LinSCP core, never interpolated into a shell.
"""

import os
import subprocess
import urllib.parse

try:
    from gi.repository import Nautilus, GObject
except ImportError:
    Nautilus = None

if Nautilus is not None:
    class LinSCPExtension(GObject.GObject, Nautilus.MenuProvider):
        def __init__(self):
            super().__init__()

        def _get_linuxscp_bin(self):
            for candidate in [
                "/usr/bin/linuxscp",
                "/usr/local/bin/linuxscp",
                os.path.expanduser("~/.local/bin/linuxscp"),
            ]:
                if os.path.isfile(candidate):
                    return candidate
            return "linuxscp"

        def menu_activate_cb(self, menu, files):
            paths = []
            for f in files:
                uri = f.get_uri()
                if uri.startswith("file://"):
                    path = urllib.parse.unquote(uri[7:])
                    paths.append(path)
            if not paths:
                return
            bin_path = self._get_linuxscp_bin()
            # Use -literal to prevent %...% expansion of filenames.
            for path in paths:
                cmd = [bin_path, "-literal",
                       "-command=put '%s'" % path.replace("'", "'\\''")]
                subprocess.Popen(cmd,
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)

        def get_file_items(self, window, files):
            item = Nautilus.MenuItem(
                name="LinSCP::upload",
                label="Upload with LinSCP",
                tip="Upload selected files to the remote directory")
            item.connect("activate", self.menu_activate_cb, files)
            return [item]