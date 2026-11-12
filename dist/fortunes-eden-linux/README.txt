Fortune's Eden @VERSION@: Linux test build
===========================================

A test build of Fortune's Eden (Eden, made for Fire Emblem: Fortune's Weave) for 64-bit
Linux. It's the same emulator as the Windows release, built for Linux. Expect rough edges,
and please report what you find.

You need your own copy of the game, dumped from your own console, as loose files or a .nup,
plus your own keys and firmware. None are included.


HOW TO RUN
----------
1. Extract this folder anywhere you can write to.
2. Run "start-fortunes-eden.sh" (from a terminal: ./start-fortunes-eden.sh).
   It starts Fortunes-Eden.AppImage from this folder, so the emulator keeps its settings,
   saves and shader cache in the "user" folder here and doesn't touch any other Eden
   setup (~/.local/share/eden). If you start the AppImage some other way, it uses
   ~/.local/share/eden instead.
3. If it doesn't start, your distribution may need FUSE 2 (a package called fuse2 or
   libfuse2), or run: ./Fortunes-Eden.AppImage --appimage-extract-and-run
4. Put prod.keys and title.keys in user/keys, then add your game folder.


DIFFERENT FROM THE WINDOWS BUILD
--------------------------------
- NVIDIA DLSS upscaling needs NVIDIA's proprietary driver. Choose NVIDIA DLSS as the upscaler:
  Settings > Graphics has a "Download from NVIDIA" button for the DLSS library (it isn't
  included). NVIDIA DLSS frame generation is Windows-only.
- AMD FSR 3.1 frame generation is included, but hasn't been tested much on Linux yet.
- AMD FSR 4: only if you have AMD's amd_fidelityfx_upscaler_vk.so; put it next to the AppImage
  or in the "user" folder. Otherwise FSR 3.1 runs instead.
- No automatic updates: download new Linux builds by hand.
- Mouse mode on Wayland: the AppImage runs through XWayland so the cursor can be held in
  the window.


REPORTING PROBLEMS
------------------
Open an issue at https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues
with your distribution, GPU and driver (Mesa/RADV or NVIDIA version), and attach
user/log/eden_log.txt.


Licence: GPL-3.0-or-later. The full source is the "linux-build" branch of the repository
above. 60 FPS mode is built in: the "60 FPS" checkbox on the launch screen.
