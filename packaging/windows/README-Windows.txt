SanctuaryPort for Windows
=========================

A native PC port of the PS4 version of Diablo III: the Brazilian release (CUSA00243) with its
1.47 update (Eternal Collection) and the US Reaper of Souls - Ultimate Evil Edition (CUSA00242).

The Windows build has been checked less than the Linux one: report what you find.

No game files are included. You need your own copy of the game, dumped from your own PS4, as
fake PKG files or as a folder containing eboot.bin, sce_module and sce_sys.

Requirements
- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX: Intel Sandy Bridge (2011) or newer, AMD FX or newer.
- A graphics card with Vulkan 1.3 and an up-to-date driver.
- 16 GB of RAM is recommended.
  Nothing else: Python and the libraries are inside this folder.

Starting
1. Extract this folder anywhere, for example C:\Games\SanctuaryPort (not inside the zip).
2. Open "SanctuaryPort.exe". If Windows shows "Windows protected your PC", click More info,
   then Run anyway: the program is not digitally signed.
3. Game -> Install from PKG with the game, its update and the add-ons, or choose the folder with
   eboot.bin (and the update folder, if the update is in its own folder).
4. Click PLAY. The first start takes a little longer while the game image is prepared.

While playing
- Insert on the keyboard, or R3 + L2 on the controller, opens the port's menu: image,
  performance and the performance overlay. F12 takes a screenshot.
- More gamepads are more players (local co-op up to 4): press the bottom face button on the new
  gamepad to join.
- Keyboard keys: Controls in the launcher.

Next time
- "Play SanctuaryPort.exe" starts the game straight away with the saved settings. Make a
  desktop shortcut to it, or add it to Steam with "Add a Non-Steam Game".

Data
- Saves, shader cache and the log: user\ next to SanctuaryPort.exe (keep it when updating).
- Settings: sanctuary.ini next to SanctuaryPort.exe; launcher options in %APPDATA%.
- Generated files (prepared game image): out\.

Problems
- The game does not open: check Log in the launcher, and send user\last_run.log with your
  report.
- Memory error (error 1450): increase the Windows page file. Win+R ->
  SystemPropertiesAdvanced -> Performance Settings -> Advanced -> Virtual memory Change ->
  set C: to 16384 MB initial, 24576 MB maximum -> Set -> OK, then restart your PC.
- RTSS/Afterburner and ReShade must target bin\sanctuary-probe.exe, not the launcher. Select
  Vulkan in the ReShade installer.

This project is not affiliated with Blizzard Entertainment or Sony Interactive Entertainment.
Diablo is a trademark of Blizzard Entertainment, Inc.
