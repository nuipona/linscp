To create 'LinuxSCP-x.x.x-Setup.exe' install package, follow these steps:
- Build 'LinuxSCP.exe', 'Console.com', 'DragExt.dll' and 'DragExt64.dll' (see ..\readme)
- Install 'Inno Setup'
  https://jrsoftware.org/isinfo.php
- Install 'PuTTY' package
  https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html
- Create 'translations' subfolder
- Copy the 'Default.isl' from Inno Setup installation to 'translations\LinuxSCP.en.isl'
- Append the 'linuxscp.isl' to the 'translations\LinuxSCP.en.isl'
- Run 'iscc linuxscpsetup.iss'
- File 'LinuxSCP-x.x.x-Setup.exe' is created
