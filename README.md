<p align="center">
  <img src="images/banner.jpg" alt="Fortune's Eden: Fire Emblem: Fortune's Weave on PC" width="100%">
</p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/github/v/release/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch?label=latest&color=e0b04a&style=for-the-badge" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-3a3448?style=for-the-badge" alt="Windows 10 or 11">
  <a href="LICENSE"><img src="https://img.shields.io/badge/licence-GPL--3.0-8a2a3c?style=for-the-badge" alt="GPL-3.0 licence"></a>
</p>

<p align="center">
  <b><a href="../../releases/latest">Download</a></b> &nbsp;·&nbsp;
  <a href="#getting-started">Getting started</a> &nbsp;·&nbsp;
  <a href="#upscalers">Upscalers</a> &nbsp;·&nbsp;
  <a href="#troubleshooting">Troubleshooting</a> &nbsp;·&nbsp;
  <a href="CHANGELOG.md">Changelog</a> &nbsp;·&nbsp;
  <a href="../../issues">Report a problem</a>
</p>

A portable, ready-to-play Windows build of the Eden emulator, made for *Fire Emblem: Fortune's Weave*. Unzip it, point it at your own copy of the game and press **Play**. Nothing to install, it updates itself, and it won't touch any other Eden setup you have.

It builds on [Duck912's Fortune's Weave fork](https://github.com/Duck912/eden-ns2-fe) and [xDarkWindx's eden-fefw 1.12](https://github.com/xDarkWindx/eden-fefw/releases/tag/1.12).

## What's new

- **v1.6.8.5:** 60 FPS mode no longer freezes the game on Continue with mods whose code reaches the game's other modules, such as settings mods ([#45](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/45)).
- **v1.6.8.4:** the busiest pre-rendered cutscenes keep up with the voices: the video decoder works on 4 frames at once, and no longer freezes the movie (or leaves the game running after closing) on PCs with many cores ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)).
- **v1.6.8.3:** no more crash in long pre-rendered cutscenes with frame generation on, and 60 FPS mode works with mods that use the free space after the game's code ([#30](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/30), [#39](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/39)).
- **v1.6.8.2:** video memory is cleaned up at loading screens and no longer builds up over long sessions ([#31](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/31), [#37](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/37)), and the game no longer waits on the emulator's drawing thread when it writes graphics memory, for small freezes when moving units on some CPUs ([#41](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/41)). *Turn ambient occlusion off* replaces the Ambient occlusion box, 60 FPS mode moves to *Settings → Graphics*, and F10 no longer switches to handheld.
- **v1.6.8.1:** Frame generation is back in *Settings → Graphics* (with NVIDIA's download button), the UI no longer ghosts with AMD frame generation and HDR, and NVIDIA DLSS frame generation works when a game is started again.
- **v1.6.8:** frame generation (experimental): AMD FSR 3.1 on any card, or NVIDIA DLSS on GeForce RTX 40/50, so 60 FPS shows as 120. The game's UI stays still.
- **v1.6.8:** the main settings are on the launch screen, beside Play: 60 FPS, Ultrawide, HDR, upscaler, quality and frame generation.
- **v1.6.8:** video memory no longer creeps up over long sessions ([#35](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/35), [#37](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/37), [#38](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/38)), and 60 FPS mode works with mods ([#30](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/30)).
- **v1.6.7.4:** a real 60 FPS mode (*Settings → Performance*): game speed follows your frame rate from 30 to 60 FPS, so no slow motion below 60, and capes and hair move smoothly.
- **v1.6.7.4:** new *Performance* page: Reflections, Ambient occlusion and Reduced crowd detail.
- **v1.6.7.4:** video memory creeps up far less in long sessions.
- **v1.6.7.3:** mouse mode: your PC mouse drives the game's Joy-Con mouse cursor (button at the bottom of the window, or Ctrl + Enter).
- **v1.6.7.3:** faster cutscenes ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)) and busy scenes such as the Saramis Guesthouse ([#27](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/27)); fixes for long sessions.
- **v1.6.7.3:** volume is always 100% (use your PC's volume).
- **v1.6.7.2:** players who updated from v1.6.2 or earlier and were still on the game's own DLSS are moved to AMD FSR 3.1, the default, once.
- **v1.6.7.1:** updated installs now get the tuned image and speed settings too (vsync Mailbox, asynchronous shaders, 16x filtering).
- **v1.6.7.1:** text fits in buttons and lists at Windows display scaling.
- **v1.6.7.1:** no more "Decryption keys are missing" question at startup.

v1.6.7 brought the new look (theme, icon and launch screen) and a much shorter Settings window. Every version is in the [changelog](CHANGELOG.md).

## Highlights

| Feature | What you get |
|---|---|
| 🎮 **Launch screen** | The game's card with **Play** and the main settings beside it: 60 FPS, Ultrawide, HDR, upscaler, quality and frame generation |
| ✨ **Upscalers** | AMD FSR 3.1 (default, any card), AMD FSR 4 (Radeon), NVIDIA DLSS (GeForce RTX), with quality modes from Native/DLAA to Performance |
| 🖱️ **Mouse mode** | Your PC mouse as the game's Joy-Con mouse cursor, with its own controls and sensitivity |
| 🖥️ **Ultrawide** | Optional 21:9 (3440x1440) with the HUD and menus kept at 16:9, using [Kompas9](https://github.com/Kompas9/Switch-Game-Patches)'s patch (game version 1.1.0) |
| 🌈 **HDR** | The game's own Switch 2 HDR mode, with its brightness options |
| ⚡ **Runs well** | PGO-optimised for this game, fewer shader stutters, smoother busy scenes |
| 🎞️ **60 FPS mode** | Built in, off by default: game speed follows your real frame rate (30–60 FPS) |
| 🚀 **Frame generation** | Experimental: AMD FSR 3.1 (any card) or NVIDIA DLSS (RTX 40/50) doubles the frames shown, with the UI kept still |
| 🎛️ **Performance options** | Lighter reflections, ambient occlusion, crowd and scenery detail for slower PCs |
| 🔄 **Auto-updates** | Asks first, checks the download, never touches saves or settings |
| 💾 **Portable** | Settings, saves and shader cache live next to the .exe |

> [!CAUTION]
> **Legal use only. Piracy is not acceptable and is not supported in any way.** Please support the developers by buying the games you play.
> - **Nothing of Nintendo's is included or linked:** no games, ROMs, keys (`prod.keys`, `title.keys`), firmware, system files or copyrighted game assets. This repository, its releases and its downloads contain only the emulator, its source code and settings.
> - **You need your own legally obtained copy of the game, dumped yourself from your own console**, along with your own keys and firmware dumped from that same console. How to get these is not covered here, and no help is given for obtaining them any other way.
> - **Issues, discussions or requests involving pirated games, shared keys or downloaded firmware will be closed** without help.
> - This is a free, non-commercial project: no donations, paywalls or paid early access.
> - Not affiliated with, endorsed by or sponsored by Nintendo, NVIDIA or AMD. *Fire Emblem: Fortune's Weave* and Nintendo Switch are trademarks of Nintendo; NVIDIA and DLSS are trademarks of NVIDIA; AMD, FSR and FidelityFX are trademarks of AMD. Names are used only to describe compatibility.
> - Development and testing were done only with legally dumped copies of the game, keys and firmware from the maintainer's own hardware.

## Requirements

- 64-bit Windows 10 or 11
- A Vulkan-capable GPU (an NVIDIA RTX card is recommended)
- **16 GB of RAM or more, with a large page file** (system-managed, or about 32 GB): the 12 GB memory layout reserves a lot of memory up front, and too small a page file makes the emulator close as the game starts
- Your own copy of the game, dumped from your own console, as loose files (exefs/romfs) or a single `.nup` file. **No games, keys or firmware are included.**

## Getting started

1. Download the [latest release](../../releases/latest) (`FEFW-Stable-60FPS-Portable.zip`, about 74 MB).
2. Extract the whole folder somewhere outside Program Files and run `Fortune's Eden.exe`.
3. Double-click the empty game list and choose the folder containing your game (or use *File → Load File* to open a `.nup` directly).
4. The game's card appears at the top: press **Play**.

**Updating:** Fortune's Eden checks for new versions when it starts and asks before installing one (or press **Check for Updates**). Your saves, settings and shader cache are kept.

> [!IMPORTANT]
> **Your saves are stored next to `Fortune's Eden.exe`, not in the normal Eden location:** in `user\nand\user\save` inside the folder you extracted, *not* in `%APPDATA%\eden`. Deleting the folder deletes its saves. See [Saves](#saves).

## Upscalers

The game draws its 3D scene at 720p and upscales it. Choose what does that in *Settings → Graphics → Upscaler*:

| Upscaler | Cards | Notes |
|---|---|---|
| **AMD FSR 3.1** (default) | Any | Takes work off the emulator. Sharpness follows the *FSR Sharpness* slider (150% by default). |
| **AMD FSR 4** | Radeon RX 6000/7000/9000 | AMD's own signed FSR 4.1.2 library from the [FSR SDK 2.4](https://gpuopen.com/manuals/fidelityfx_sdk2/whats-new/). RX 9000 runs the full model, RX 6000/7000 an INT8 model. |
| **NVIDIA DLSS** | GeForce RTX | The best picture. Click **Download from NVIDIA** in the panel below the setting and accept NVIDIA's licences (NVIDIA's files aren't included). |
| **Game's own DLSS** | Any | The game's own small network, run as compute passes. Some prefer its look; it costs the emulator more. |

NVIDIA DLSS and FSR 4 fall back to FSR 3.1 if they can't start. The status bar and the launch screen show which one is running, e.g. *NVIDIA DLSS - DLAA*.

**Upscaler quality** (at **1X** resolution) sets how big the 3D scene is drawn before upscaling, while menus and text stay sharp at 1440p: **Native** (DLAA with DLSS), **Ultra Quality**, **Quality**, **Balanced** (default) or **Performance**. Higher looks better and costs more GPU time. With the game's own DLSS it always runs at Performance.

## Frame generation (experimental)

**Frame generation** makes a frame between each two of the game's, so 60 FPS shows as 120 (or 30 as 60). Choose it on the launch screen or in *Settings → Graphics → Frame generation*; it's off by default and applies when the game starts.

| Option | Cards | Notes |
|---|---|---|
| **AMD FSR 3.1** | Any | AMD's FidelityFX SDK frame interpolation and optical flow, built in. |
| **NVIDIA DLSS** | GeForce RTX 40/50 | Needs hardware-accelerated GPU scheduling on in Windows. Choose it, then click **Download from NVIDIA** in the panel below the setting and accept NVIDIA's licences (NVIDIA's files aren't included). Falls back to AMD's if it can't start. |

It works with any upscaler, HDR and ultrawide. The game's UI is kept still, so menus, prompts and the minimap don't ghost when the camera moves. It only runs in the 3D scenes (menus and movies have no motion data), needs a display faster than the game's frame rate (120 Hz or more for 60 FPS), and adds about half a frame of input delay. The status bar shows which one is running.

## HDR

Fortune's Weave has an HDR mode on Switch 2. Turn on HDR in Windows (*Settings → System → Display → Use HDR*), tick **HDR output (HDR10)** in *Settings → Graphics* (or **HDR** on the launch screen), and start the game. **Paper white** sets how bright the game's white is. Its *Brightness* and *UI Brightness* options then adjust the picture. The game checks for HDR once when it starts, so restart it after changing either setting.

## 60 FPS mode

**30 FPS is how Fortune's Weave runs on Switch 2**, and it's how this build runs by default. The built-in **60 FPS mode** is off by default: turn it on in *Settings → Graphics → 60 FPS mode* (or **60 FPS** on the launch screen) and start the game.

Unlike the old 60 FPS cheat, which forced a fixed 60 and made the whole game run in slow motion whenever a PC fell below it, game speed follows your real frame rate anywhere from 30 to 60 FPS. About 50 places in the game that counted per frame (the map cursor and markers, menus, physics, cutscene shake, character creation and more) were traced and fixed so they keep their normal speed, and capes and hair are simulated every frame with physics scaled to the frame time. It was made for game version 1.1.0; on other versions it turns on when every place it changes is found. It works alongside mods, including those that use the free space after the game's code its routines usually take (they then go in memory Fortune's Eden adds), and turns itself off (the mod still works) only if a mod changes the same game code. An old `60fps` add-on folder becomes the setting.

*Settings → Performance* has **Reflections**, **Turn ambient occlusion off**, **Reduced crowd detail** and **Reduced scenery detail**, lighter versions of the game's own effects for PCs that can't hold the frame rate. Each tick makes the game lighter.

## Saves

- **Where they are:** `<your folder>\user\nand\user\save\0000000000000000\<profile ID>\04007EF01F8AE000\`. Nothing is written to `%APPDATA%\eden`. Loose files and `.nup` both use this folder, and *File → Open Save Data Location* opens it.
- **Bring saves from normal Eden or another build:** close both, then copy the whole `%APPDATA%\eden\nand` folder (or the old build's `user\nand`) into this build's `user` folder, replacing the `nand` there. Copy the whole `nand`, not just the save files: it holds your user profile too.
- **Back them up:** copy or zip the `user\nand` folder.

<details>
<summary>Coming from a version older than v1.3 with loose files?</summary>

Older versions saved loose-file games under game ID `0000000000000000`. v1.3 and later copy that save to `04007EF01F8AE000` the first time you start the game, if there's no save there yet. If you also have an older `04007EF01F8AE000` folder (from playing a `.nup`), that one is used instead; keep whichever is newer as `04007EF01F8AE000`.
</details>

## Troubleshooting

- **Closes as soon as the game starts:** usually not enough memory for the 12 GB layout. Enlarge the Windows page file and close other heavy apps.
- **Short hitches in new areas:** that's first-time shader translation. It's saved to the shader cache, so revisits are smooth.
- **Freezes of about a second after playing for a while:** make sure **VRAM Usage Mode** is **Conservative** (the default) in *Settings → Advanced*. On *Aggressive* the texture cache can spill into much slower system RAM during a long session.
- **The .exe still shows Eden's old icon after updating:** that's Windows' icon cache. Run `ie4uinit.exe -show` (Win+R), or sign out and back in.
- **Something else?** [Open an issue](../../issues) and attach your log (*File → Open Eden Folders → Log Folder*, file `eden_log.txt`).

<details>
<summary><b>Everything that's different from upstream (xDarkWindx 1.12)</b></summary>

- **HDR output (HDR10):** the game's own Switch 2 HDR mode, with its Brightness and UI Brightness options. Off by default; see [HDR](#hdr).
- **AMD FSR 3.1 by default, AMD FSR 4 optional** in place of the game's own DLSS, with quality modes from Native to Performance: FSR 3.1 on any card, FSR 4 on AMD Radeon RX 6000/7000/9000 (AMD's own FSR 4.1.2 library from the [FSR SDK 2.4](https://gpuopen.com/manuals/fidelityfx_sdk2/whats-new/)). See [Upscalers](#upscalers).
- **Optional NVIDIA DLSS:** on GeForce RTX cards, the game's own lightweight DLSS (the Switch 2's upscaler, which can leave ghosting around moving characters) can be replaced with the full PC version of NVIDIA DLSS. Off by default; see [Upscalers](#upscalers). Ported from Duck912's work in [Eglantine](https://github.com/Djihads80/Eglantine).
- **Smoother 60 FPS in busy scenes:** the game builds some texture-handle tables on the GPU and reads them a few draws later. The emulator used to stop and wait for the GPU about once a frame to read them back, although they never change. It now uses the copy it has and checks it against the GPU's version afterwards, going back to waiting if they ever differ. CPU writes to GPU memory also take the cache lock far less often. In Dagsion's busiest section at 60 FPS (1x), doubled frames went from about 45% to about 20%.
- **Fewer shader-compile stutters:** translated shaders are reused between graphics pipelines that share them, so new areas hitch less the first time (Wael3rd, from PokeDinner, an Eglantine fork).
- **Faster on the CPU side:** built with profile-guided optimisation (PGO) trained on Fortune's Weave (Dagsion, battles, menus). The emulator's own code for feeding the GPU is laid out for the paths the game actually uses; it still runs on any x64 CPU. In testing at 60 FPS it cut doubled frames by about a quarter.
- **One save folder for loose files and `.nup`:** loose-file dumps now use the game's real ID, like `.nup`, cartridge and eShop dumps. Saves made with loose files in older versions are copied across automatically the first time (the old copy is kept).
- **Plays `.nup` files:** load the game as a single `.nup` package as well as loose files. The NUP loader is ported from [Djihads80's Eglantine](https://github.com/Djihads80/Eglantine). Playing only: packing, unpacking and editing NUPs aren't included, and add-on content inside a `.nup` isn't loaded.
- **AMD graphics fix:** shader registers that a game reads before writing are treated as zero, as NVIDIA hardware does, instead of leftover garbage, which on AMD GPUs can show as stray coloured patches. Ported from Eglantine (by arisqz).
- **Shutdown and restart fixes:** no hang when stopping the game if its audio renderer outlives its manager (kraibse), and starting the game again in the same window no longer hangs or crashes (Takoroka), both from PokeDinner, an Eglantine fork. Clearer logs for missing keys and unsupported system calls, and an error-reporting crash fix (xDarkWindx).
- **Start-up crash fix:** the emulator no longer closes on launch when Windows refuses to delete an empty save-profile folder, and never deletes a save folder it can't read.
- **Settings ready for Fortune's Weave:** the 12 GB memory layout the game needs, GPU accuracy Fast, asynchronous shaders on, vsync Mailbox, 16x anisotropic filtering, VRAM Usage Mode Conservative, Bilinear window filter and anti-aliasing off. Settings that don't apply to the game are hidden and kept at the right values, so only controls need setting up.
- **Fully portable:** settings, **saves** and shader cache all stay in the `user` folder next to `Fortune's Eden.exe` (see [Saves](#saves)).
- **Runs on any PC:** built without CPU-specific instructions, with the Microsoft C++ runtime included.
</details>

<details>
<summary><b>Building from source</b></summary>

The emulator source for this build is in `source.zip` (also inside the release download) and on the [`source`](../../tree/source) branch, with each release's exact source on a `source-v<version>` branch. It builds with Visual Studio 2026 (MSVC 19.51) and CMake:

1. Put `Build-Local-MSVC.ps1`, `Save-PGO.ps1`, `cmake-options.txt`, `LICENSE`, the `portable-config` folder and `source.zip` together in a folder whose path has no spaces, and rename `source.zip` to `eden-fefw-1.12-local.zip`.
2. In PowerShell in that folder: `Set-ExecutionPolicy -Scope Process Bypass; .\Build-Local-MSVC.ps1 -Version 1.12 -Portable`

That gives `FEFW-Stable-60FPS-Portable` (with `Fortune's Eden.exe`) and a zip of it, with the settings from `portable-config/`. For the PGO build used in releases:

1. `.\Build-Local-MSVC.ps1 -Version 1.12 -Portable -Pgo Train` builds a measuring copy in `FEFW-PGO-Training`.
2. Play Fortune's Weave in it for 10–20 minutes, and **while it's still running** run `.\Save-PGO.ps1` to save the measurements.
3. `.\Build-Local-MSVC.ps1 -Version 1.12 -Portable -Pgo Use` builds the optimised package.
</details>

## Linux

> [!NOTE]
> From v1.6.8.2, each release has a **Linux build**: **[Fortunes-Eden-linux-x86_64.tar.gz](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/latest/download/Fortunes-Eden-linux-x86_64.tar.gz)** (64-bit, AppImage). It's newer and tested on fewer PCs than the Windows build, so please [report how it goes](../../issues) with your distribution, GPU and driver, and attach `user/log/eden_log.txt`.

- Extract it and run `./start-fortunes-eden.sh`: settings, saves and shader cache stay in the `user` folder next to it, apart from any other Eden install. Put your keys in `user/keys`.
- It's the same emulator as the Windows build, but without NVIDIA DLSS or AMD FSR 4 (both are Windows-only); AMD FSR 3.1 is the default upscaler.
- It updates itself like the Windows build: the AppImage is replaced with the new release's (`Fortunes-Eden-x86_64.AppImage`), and the `user` folder is kept.
- If it doesn't start, install FUSE 2 (`fuse2` or `libfuse2`), or run `./Fortunes-Eden.AppImage --appimage-extract-and-run`.
- Source: the [`linux-build`](../../tree/linux-build) branch.

## Credits and licence

Built on [Eden](https://eden-emu.dev), [Duck912's Fortune's Weave fork](https://github.com/Duck912/eden-ns2-fe) and [xDarkWindx's 1.12 release](https://github.com/xDarkWindx/eden-fefw), with the NUP loader, AMD fix and NVIDIA DLSS support from [Djihads80's Eglantine](https://github.com/Djihads80/Eglantine) (Duck912, arisqz), and shader reuse and shutdown fixes from PokeDinner (Wael3rd, kraibse, Takoroka). AMD FSR 3.1 is built from AMD's FidelityFX SDK (MIT) with the upscaler from Eglantine (Duck912); AMD FSR 4 is AMD's own signed FSR 4.1.2 Vulkan library from the FSR SDK 2.4, redistributed unmodified under the FidelityFX SDK licence (`licenses\amd-fsr4`). AMD and FidelityFX are trademarks of AMD; this project is not made or endorsed by AMD. NVIDIA DLSS and Streamline are NVIDIA's; none of NVIDIA's files are included. Licensed under **GPL-3.0-or-later**; the full source for this build is included as `source.zip`. Not affiliated with, endorsed by or sponsored by Nintendo or NVIDIA. For use only with games, keys and firmware legally dumped from your own console; see the legal notice at the top.
