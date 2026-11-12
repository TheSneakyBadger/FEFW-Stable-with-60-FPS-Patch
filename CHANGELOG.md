# Changelog

## v1.6.8.6

60 FPS mode with menus and spoken text at their normal speed ([#46](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/46)), and cutscene effects that no longer jump.

#### ⏱️ 60 FPS mode

- **Menus, windows and other UI animations play at their normal speed.** The game moves every UI animation on by one step per frame, so at 60 FPS menu transitions, windows opening and closing and other UI effects ran twice as fast. Each step is now scaled to the real frame time, so they play as they do at 30 FPS, at any frame rate in between too.
- **Spoken text prints at its normal speed.** Each frame, the game shows 3 more letters of the current line, so at 60 FPS lines printed twice as fast. Letters now come in on the game's own 30 steps a second, so text prints as fast as it does at 30 FPS. Pressing the button to show the whole line works as before.

#### 🎨 Graphics

- **Effects no longer jump back mid-animation.** Some effects, such as the dust clouds in cutscenes, could snap back to an earlier frame now and then. Reactive flushing (Graphics > Advanced) fixes this and is now on by default. Existing settings are switched on once; you can turn it off again if you want.

#### 🔧 Fixes

- **Menu check boxes no longer overlap the text** in the Fortune's Eden and Qt Dark themes ([#39](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/39)).

Everything else is as in [v1.6.8.5](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8.5).

## v1.6.8.5

60 FPS mode with mods that reach the game's other modules ([#45](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/45)).

#### 🔧 Fixes

- **60 FPS mode no longer freezes the game on Continue with some mods.** Since v1.6.8.3, when a mod uses the free space after the game's code, 60 FPS mode puts its routines in memory Fortune's Eden adds. That memory went right after the game's main code, which moved the rest of the game's code along a little. Mods whose code reaches those other parts at fixed distances, such as settings mods that add their own options, then read the wrong places and the game froze when loading a save. The memory now goes after all of the game's code, so nothing moves.

Everything else is as in [v1.6.8.4](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8.4).

## v1.6.8.4

Pre-rendered cutscenes that keep up in their heaviest scenes ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)).

#### 🎬 Cutscenes

- **The busiest 1440p cutscenes keep up with the voices.** Scenes full of fire or fast camera moves took 30–43 ms per frame to decode, over the 33 ms a 30 FPS video has, so the movie fell further and further behind. The video decoder now works on 4 frames at once, so a finished frame comes out every 10–15 ms.
- **No more frozen movies or a game that won't close.** With several frames at once, the video decoder's threads sometimes all went to sleep with work still left, on PCs with many cores. The movie then froze for good, and closing the game left it running in the background. Fortune's Eden now notices within 20 ms and wakes them, so the movie carries on without dropping frames. (This is a known problem in the decoder itself, [dav1d #444](https://code.videolan.org/videolan/dav1d/-/issues/444).)

Everything else is as in [v1.6.8.3](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8.3).

## v1.6.8.3

A fix for a crash in cutscenes with frame generation, and 60 FPS mode with mods ([#30](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/30), [#39](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/39)).

#### 🔧 Fixes

- **No more crash in long pre-rendered cutscenes with frame generation on.** In v1.6.8.2, about 30 seconds into a pre-rendered video, Fortune's Eden could close to the desktop. Frame generation kept a reference to the game's UI image, which the new video memory cleanup freed while the video played (it was unused for 30 seconds), and NVIDIA's driver crashed when the next frame used it.

#### ⚡ 60 FPS mode

- **Works alongside more mods.** Mods that use the free space after the game's code (settings and quality-of-life mods among them) turned 60 FPS mode off, because its routines go in that same space. They now go in memory Fortune's Eden adds after the game's code. A mod that changes the same game code as 60 FPS mode still turns it off.

Everything else is as in [v1.6.8.2](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8.2).

## v1.6.8.2

Video memory that no longer builds up over long sessions ([#31](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/31), [#37](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/37)), and less waiting on the emulator in busy scenes ([#41](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/41)).

#### 🧠 Video memory

- **Cleaned up at loading screens.** Fortune's Eden now notices when the game loads a new area (from its reads of the map's data). Once the new area is running, it frees what the previous one used. Memory drops at each area change instead of climbing all session: in testing, one area change took it from 12.4 GB back to 5.6 GB.
- **Old render targets are freed after 30 seconds unused.** Before, they were only freed once memory reached the faster cleanup level (12.6 GB on a 24 GB card). Even then, the cleanup threw out the game's textures first, so memory sat at that level, reloading textures every few seconds.
- **Small leftover images go after 5 minutes unused.** They used to be kept for good, and grew to 3.6 GB over an hour.

This helps cards with 8 GB or less most: they start each area with a clean cache.

#### ⚡ Performance

- **The game's writes to graphics memory no longer wait for the drawing thread.** On busy battle maps the game makes tens of thousands of these writes a second. Each one waited while the emulator's drawing thread drew, which caused small freezes when moving units on some CPUs ([#41](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/41)). They're now collected and applied together before the GPU uses them.

#### ⚙️ Settings

- **"Turn ambient occlusion off"** replaces the "Ambient occlusion" box on the Performance page. Ambient occlusion stays on unless you tick it, and if you had already turned it off, it stays off. Every tick box on the Performance page now makes the game lighter.
- **60 FPS mode moves to the Graphics page**, next to Ultrawide. It's still on the launch screen too.
- **F10 no longer switches between docked and handheld.** It was easy to hit by accident, and handheld mode drops the game to 1080p. The menu item and the controller shortcut (Home+X) still work.

#### 🐧 Linux

The Linux build now comes with each release: **Fortunes-Eden-linux-x86_64.tar.gz** below. Extract it and run `./start-fortunes-eden.sh`. From this version on it updates itself, like the Windows build. It's newer and tested on fewer PCs, so please [report how it goes](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues).

Everything else is as in [v1.6.8.1](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8.1).

## v1.6.8.1

A quick fix for three frame generation problems in v1.6.8.

#### 🔧 Fixes

- **Frame generation is back in Settings → Graphics.** In v1.6.8 it only showed on the launch screen, so the **Download from NVIDIA** button for NVIDIA's frame generation files was missing too. Choose **NVIDIA DLSS** there to see the button.
- **No more UI ghosting with HDR on.** With AMD FSR 3.1 frame generation and HDR, the UI moved with the scene and left trails when the camera moved. The UI now stays still, as it does without HDR.
- **NVIDIA DLSS frame generation works when you start a game again.** After you stopped a game, starting it again fell back to AMD FSR 3.1 for both the upscaler and frame generation. NVIDIA's software can't be set up twice in one run, so after NVIDIA frame generation has run, **Play** now reopens Fortune's Eden straight into the game.
- **Clearer log when NVIDIA frame generation can't start.** If NVIDIA's frame generation files are missing, the log now names them. Before, it blamed the graphics card or Windows.

Everything else is as in [v1.6.8](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/releases/tag/v1.6.8).

## v1.6.8

Frame generation (AMD and NVIDIA), the main settings on the launch screen, and video memory that no longer creeps up over long sessions.

#### 🎞️ Frame generation (experimental)

A frame made between each two of the game's, so **60 FPS shows as 120** (or 30 as 60). Choose it on the launch screen or in **Settings → Graphics → Frame generation**. It's off by default and applies when the game starts.

| Option | What it needs |
|---|---|
| **AMD FSR 3.1** | Any graphics card. |
| **NVIDIA DLSS** | A GeForce RTX 40 or 50 card, hardware-accelerated GPU scheduling on in Windows, and NVIDIA's frame generation files: **Settings → Graphics → Frame generation** has a **Download from NVIDIA** button for them (NVIDIA's files aren't included). If it can't run, AMD's is used instead. |

- **Works with any upscaler**, and with HDR and the ultrawide patch.
- **The UI stays still.** The game draws its UI separately, so frame generation moves only the 3D scene. Menus, prompts and the minimap don't ghost or shimmer when the camera moves.
- **Each shown frame gets its own motion.** The game presents a frame after it has already started the next two; frame generation matches every frame to the motion and UI it was drawn with.
- **Only in the 3D scenes.** It uses the game's own motion data, which menus and movies don't have, and pauses there.
- **Needs a display faster than the game's frame rate**: 120 Hz or more for 60 FPS. It adds about half a frame of input delay.
- **The status bar shows which one is running**, next to the upscaler (for example *NVIDIA DLSS Frame Gen*).
- Built on AMD's FidelityFX SDK 1.1.4 (FSR 3.1 frame interpolation and optical flow, MIT) and NVIDIA Streamline 2.14 (DLSS frame generation, loaded from your own DLLs: none are included).

#### 🐧 Linux (test build)

Updated to v1.6.8: **[Fortunes-Eden-linux-x86_64.tar.gz](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/raw/test-builds/Fortunes-Eden-linux-x86_64.tar.gz)**

- NVIDIA DLSS (upscaling and frame generation) is Windows-only.
- AMD FSR 3.1 frame generation is included, but untested on Linux so far.

#### 🏠 The main settings on the launch screen

Beside **Play**, so nobody misses them:

- **60 FPS**, **Ultrawide** and **HDR** (with its **paper white** slider) checkboxes.
- **Upscaler**, **Upscaler quality** and **Frame generation** drop-downs.
- A **Performance Settings** button that opens the Performance page.

They're the same settings as in the Settings window and take effect when the game starts.

#### 🧠 Video memory over long sessions ([#31](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/31), [#35](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/35), [#37](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/37), [#38](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/38))

Over an hour or two, video memory crept up until the card was full. On 8 GB cards that meant drops to 10 FPS or red and green garbage and a crash; on bigger cards, slowdown after a couple of hours.

- **The cause:** images the GPU had drawn into but couldn't copy back to the game were never cleaned up. These were mostly render targets from earlier scenes, and they piled up.
- **Now** those unused for 30 seconds are freed once memory passes the cleanup level, so memory levels off.
- **On low-memory cards**, cleanup no longer copies back and frees images used in the last 2 seconds, which caused stretches of 20 FPS on 6 GB cards.
- **NVIDIA DLSS stays on** if it fails while video memory is over budget, instead of switching to AMD FSR 3.1 for the rest of the session. It also falls back to FSR 3.1 when it fails inside NVIDIA's code.

#### 🎛️ 60 FPS mode

- **Works with mods ([#30](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/30), [#39](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/39)).** It's applied after mods and only into space no mod uses, so the two no longer crash together. If a mod changes the same instructions, 60 FPS mode turns itself off and the mod still works.
- **Other game versions:** 60 FPS mode turns on when all 57 places it changes are found unchanged (it was made for 1.1.0).
- The old **60fps** add-on folder becomes the setting.

#### ⚡ Speed and sound

- **About 30% less game CPU time** in busy scenes such as the Saramis hub: the game's writes to memory the GPU wrote no longer wait for the draw thread.
- **Cutscenes ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)):** the video decoder runs on its own thread.
- **New: Settings → Performance → Reduced scenery detail**, for slower PCs.
- **New: Settings → Audio → Audio buffer** (Low, Medium, High) for crackling sound ([#29](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/29)).
- **Controller rumble** is sent from its own thread, so it no longer holds up the game ([#36](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/36)).

#### 🔧 Other changes

- **HDR paper white** setting ([#33](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/33)).
- **The updater resumes dropped downloads** ([#32](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/32)).
- **Mouse mode:** the hint next to its button points at the button (right-click it for the sensitivity).
- The log says what the emulator built during slow frames, and what the cached images are every 5 minutes, to help with reports.

## v1.6.7.4

A proper 60 FPS mode, a new Performance page, and video memory that stays put in long sessions.

#### 🎞️ 60 FPS mode, built in

Turn it on in **Settings → Performance → 60 FPS mode**, then start the game. It's off by default.

- **No more slow motion.** The old 60 FPS cheat forced a fixed 60, so the whole game slowed down whenever your PC dropped below it. Now game speed follows your real frame rate anywhere from **30 to 60 FPS**: at 45 FPS the game still runs at normal speed, just with fewer frames.
- **About 50 per-frame timers fixed.** The map cursor and markers, menu animations and scrolling text, physics, cutscene screen shake, character creation, look-at and hold timers all keep their normal speed at any frame rate.
- **Smooth capes and hair.** They're simulated every frame, with gravity, wind, damping and stiffness scaled to the frame time.
- **Steadier on hitches.** A single slow frame (a shader being built) is ignored instead of making the game jump.
- Only for game version **1.1.0**. An old `60fps` add-on folder still turns it on; you can delete it.
- Thanks to **Jbuck7**, whose 60 FPS cheat was the map of where the game counts per frame. The fixes themselves are new code.

#### 🎛️ New Performance page

Lighter versions of the game's own graphics effects, for PCs that can't hold the frame rate. Nothing on disk is changed; they apply when the game starts.

| Option | What it does |
|---|---|
| **Reflections** | Full, Reduced (8 rays instead of 32, a little grainier) or Off. Mostly helps in the hubs. |
| **Ambient occlusion** | The soft shading in corners. Off saves GPU time everywhere, with flatter lighting. |
| **Reduced crowd detail** | Crowd characters switch to simpler models twice as soon, lightening busy towns and battles. |

#### 🧠 Video memory stays put in long sessions

On a 24 GB card, video memory used to climb towards 18 GB within 15 minutes and then clean up in stutter-causing bursts. At 1x it now settles around 14 GB.

- **Freed memory goes back to the driver.** Large textures get their own allocation and the rest use smaller blocks, so cleanups no longer leave gigabytes stranded.
- **Cleanup measures what's really in use**, so it stops throwing out textures it doesn't need to.
- **Unused buffers are freed** after 30 seconds (before, most cards never freed them).
- **Busy scenes are cleaned up straight away** instead of filling to the limit, while images the GPU drew into that can't be written back are kept, so effects don't go black.

This should also help the slowdown some players saw over an hour or two, and 8 GB cards. If you still see a slowdown in long sessions, please send your log.

#### 🔧 Other fixes

- The **Play** button on the launch screen is tall enough for its text again.
- **Linux (test build):** no more "Too many open files", and the updater is Windows-only.

## v1.6.7.3

Mouse controls, faster busy scenes and cutscenes, and fixes for long sessions.

- **New: mouse mode.** Fortune's Weave has a Joy-Con mouse cursor on Switch 2, and your PC mouse can now drive it: the cursor moves with the mouse, left and right click work, and the scroll wheel zooms (like W/S).
  - Turn it on with the **Click to turn mouse mode on** button at the bottom of the window, or **Ctrl + Enter**. While it's on, the PC cursor is hidden and kept inside the game window; Ctrl + Enter or the button turns it off again.
  - **Right-click the button to set the mouse sensitivity** (1–20, default 4), also while the game is running.
  - **Separate controls for mouse play.** Player 1 now has two sets of controls, normal and mouse, and turning mouse mode on or off swaps between them. Pick which set to edit in *Settings → Controls*. The mouse set starts as a copy of your normal controls.
  - The game normally only shows its cursor while the right Joy-Con lies flat; a small built-in patch to the game's 1.1.0 executable lets the PC mouse show it whatever the controller is doing.
- **Cutscenes decode much faster ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)).** The game's 1440p AV1 cutscenes are one tile per frame, and the old decoder could only use one CPU thread for that, so a heavy frame took about 50 ms and some cutscenes dropped to 5–8 FPS on slower CPUs. They're now decoded with dav1d, which splits each frame across all threads and has AVX2/AVX-512 code. (The old decoder is still there as a fallback.)
- **The Saramis Guesthouse and other busy scenes run faster ([#27](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/27)).**
  - A shader instruction the game uses to add up values in shared memory reported every successful update as a failure, so the game's loops kept adding until their safety limit: wrong results and a ~10 ms compute shader that held the Guesthouse at 45 FPS. Found and measured by nanachi7700.
  - Crowds bind three texture arrays of 1,024 entries on every draw; they're now capped at 256 entries, which KektJ measured at about 40 ms frames instead of 52–66 ms, with correct textures.
  - The texture cache no longer goes through every cached image view and framebuffer each time it removes an image (nanachi7700's patch).
- **VRAM Usage Mode is now hidden and kept at Conservative.** On Aggressive the texture cache grew until it filled the card (an RX 7700 XT log showed it reach 10 GiB over 90 minutes, with the game down to 16 FPS until restarted). Some players still see a gradual slowdown over an hour or two on Conservative; that is being looked into, and restarting the game clears it.
- **NVIDIA DLSS no longer gives up for the whole session after one failure ([#31](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/31)).** On an 8 GB RTX 3070 Laptop, DLSS ran out of video memory after 15 minutes; the emulator then switched to AMD FSR 3.1 for good while still holding DLSS's memory, and the game fell to 15–30 FPS. Now DLSS's memory is freed, FSR 3.1 stands in for 30 seconds and DLSS is tried again (up to 3 times). On 8 GB cards, *Upscaler quality* Performance uses the least video memory.
- **Volume is always 100%.** The volume setting, its status-bar button and hotkeys are gone; use your PC's volume instead. Mute still works.
- ***File → Open Save Data Location* opens the folder that really holds your save**, next to Fortune's Eden.exe. With a `.nup` or loose files it used to open an empty folder.
- **The launch screen and game list show the game's real version** for `.nup` and loose files, instead of 1.0.0 (based on a55uka's Eglantine change).
- **Fixes from a Linux player's investigation (RX 6700 XT):** some of the game's compute shaders wrote far past their local memory (a GPU fault and lost device on Linux's RADV driver), presentation could wait longer than needed, and one shader feature was declared incompletely.
- Two more fixes from Eglantine: shader inputs at the top of the attribute range (gabrielskaminski) and the compute subgroup size on GPUs that only support it for other stages (Yuri Olive).
- The source is now also on this repository's [`source`](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/tree/source) branch, with each release's exact source on a `source-v<version>` branch, as well as in `source.zip`.

## v1.6.7.2

- **AMD FSR 3.1 for everyone who updated from v1.6.2 or earlier.** v1.6.3 replaced the old upscaler tick boxes with one *Upscaler* list and made AMD FSR 3.1 the default, but a config with no box ticked was moved to the game's own DLSS, the default before then. So most players who've been updating since then were still on the game's own DLSS, with *Upscaler quality* greyed out. They're now moved to AMD FSR 3.1 once. If you prefer the game's own DLSS, choose it again in *Settings → Graphics → Upscaler*; that choice is kept.

## v1.6.7.1

- **Updated installs get the tuned image and speed settings.** A fresh install starts from the package's settings file, but an update keeps your own, where settings you never changed say "use the default", and that default was the emulator's built-in value, not the package's. So updated installs had vsync FIFO instead of Mailbox, asynchronous shaders off, anisotropic filtering Automatic instead of 16x, reactive flushing on and FSR sharpening 25% instead of 150%. The built-in defaults are now the package's values, so they apply after updating too. Settings you changed yourself are kept.
- **Text fits in buttons, tabs and lists at Windows display scaling** (125%, 150%): the theme's padding was too big, and the launch screen's Play button sized itself from a smaller font than it drew.
- **No more "Decryption keys are missing" question at startup.** *Tools → Install Decryption Keys* is still there.
- **The .exe's icon updates with the app's icon in future builds.** If Explorer still shows Eden's old icon for `Fortune's Eden.exe` after updating, that's Windows' icon cache: run `ie4uinit.exe -show` (Win+R) or sign out and back in.

## v1.6.7

A new look and a much shorter Settings window.

- **New look.** A Fortune's Eden theme (deep plum and gold, rounded controls, card-style groups, a sidebar-style settings list) is the new default; if you were on the previous default dark theme you're switched to it once, and a theme you picked yourself is kept. A new app icon (original artwork: a sword through a woven gold-and-crimson ring with laurel) replaces Eden's.
- **Launch screen.** Above the game list, a card for Fire Emblem: Fortune's Weave shows its icon, version and play time, the upscaler and its mode, and **Play**, **Game Settings**, **Settings** and **Check for Updates** buttons.
- **Settings cleaned up.** About 55 settings that don't apply to Fortune's Weave, duplicate the upscaler, or only tune the emulator's internals are no longer shown. Settings now has six pages: **General, System, Graphics, Audio, Controls** and **Advanced** (advanced graphics and post-processing effects, game folders and paths, logging).
  - Gone from view: library applet modes, the window filter and anti-aliasing (the upscaler does those jobs), the frame pacing target (its "60 FPS" never unlocked 60 FPS: the 60fps add-on does), the renderer (Vulkan only: the upscalers, HDR and FSR 4 need it), aspect ratio (Ultrawide covers 21:9), region, time zone and custom clock, the microphone, the network page (no online play), and GPU and CPU emulation internals.
  - **Hidden settings are reset to the right values every time Fortune's Eden starts**, so nothing changed in an older version can stay stuck with no way to change it back: Vulkan, multi-core on, 16:9, no anti-aliasing, Bilinear window filter, Auto frame pacing, the 12 GB memory layout the game needs, GPU mode Fast, ASTC recompression Auto, and the rest at their defaults.
- **Game Settings has only Add-Ons and Input Profiles ([#27](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/27)).** A per-game value of any other setting used to silently beat the one in Settings (a per-game Upscaler quality of Native kept a player at DLAA after they'd chosen Performance). Per-game values of those settings are now ignored, so Settings always applies.
- **Add-ons can be changed while the game is running ([#26](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/26)),** applied the next time the game starts. If you start the game from its file, you can now tick the 60fps add-on from *Emulation → Configure Current Game → Add-Ons*.

## v1.6.6.2

- **The Saramis hub (war arc) no longer drops to 15–20 FPS on mid-range PCs ([#18](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/18)).** The game builds some texture lookup tables on the GPU. The emulator reuses its own copy and checks it against the GPU's a frame later, but as a safety net it also did a real, blocking read of each table every 250 ms. A blocking read waits for all the work queued on the GPU; in the hub on an RTX 3060 that took about 85 ms, about 6 times a second, with the game's own threads stalled behind it (and a pause menu brought the frame rate straight back). The safety read now runs every 2 seconds; the per-frame check is unchanged.

## v1.6.6.1

- **Fixed: a broken picture with NVIDIA DLSS (and FSR) in some cutscenes.** In one cutscene the game's motion vectors sat in memory the emulator had first seen used for an 8-bit colour image, and the upscaler was handed them in that colour format: DLSS got garbage motion (smearing, warped edges), and NVIDIA's Streamline logged "Cannot have undefined format" several times a frame. The upscalers now read the colour, motion, depth and output images in the formats the game's own DLSS pass reads them in.

## v1.6.6

- **AMD FSR 4 now really starts on Radeon cards.** v1.6.4 and v1.6.5 still fell back to FSR 3.1 ("FidelityFX error 1", seen on an RX 7700 XT). AMD's FSR 4.1.2 library (FSR SDK 2.4) requires the app to say which API version it was built for, and takes the Vulkan device under a newer descriptor type; the emulator now does both, as AMD documents (and still tries the older way if that fails). Confirmed working on an RX 7700 XT. No particular AMD driver version is needed.
- **The status bar names the upscaler's mode:** *NVIDIA DLSS - DLAA*, *AMD FSR 3.1 - Balanced*, *AMD FSR 4 - Quality* and so on, instead of the scene's scale. *Scale* now always shows the picture's scale (1x with the quality modes).
- **Upscaler quality settings explain themselves:** above 1X resolution, a note under the list says the modes only work at 1X; with the game's own DLSS chosen, the list is greyed out, with a note that it always runs at Performance.

## v1.6.5

- **New: Upscaler quality.** *Emulation → Configure → Graphics → Upscaler quality* chooses how big the game's 3D scene is drawn before the upscaler brings it to the full picture. Fortune's Weave itself always draws at 720p and upscales to 1440p (Performance); the emulator can now draw the scene bigger while the menus, text and the rest of the picture stay at 1440p:

  | Mode | 3D scene (docked, 1X) | NVIDIA DLSS runs as |
  |---|---|---|
  | Native | 2560×1440 | DLAA |
  | Ultra Quality | 1920×1080 | Quality |
  | Quality | 1680×945 | Quality |
  | **Balanced (new default)** | 1600×900 | Balanced |
  | Performance | 1280×720 (the game's own) | Performance |

  Native is the sharpest and the heaviest: the upscaler only smooths edges. Each step up costs GPU time (Balanced draws about 56% more scene pixels than Performance), so on a laptop or an older card Performance may still be the better choice. It works with AMD FSR 3.1, NVIDIA DLSS and AMD FSR 4 at the 1X resolution (the default). At other resolutions, and with the game's own DLSS, it stays at Performance for now. Takes effect when a game starts; the status bar shows the scene's scale (e.g. *Scale: 1x, scene 1.25x*).
- **AMD FSR 4: the log now says why it can't start.** On some Radeon cards AMD's FSR 4 library still refused to start ("FidelityFX error 1") and FSR 3.1 ran instead; AMD's own messages are now written to the log. (Fixed in v1.6.6; a driver update didn't help.)
- This build reuses v1.6.3's PGO profile, as v1.6.4 did.

## v1.6.4

- **Busy scenes run faster: each draw is prepared about 40% faster where the game draws a lot** ([#18](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/18), [#21](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/21), [#28](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/28)). Measured standing still in the Saramis hub (war arc), with the per-draw timing statistics: 8.9 µs per draw in v1.6.3, 5.3 µs now; the thread that prepares draws went from 89% busy to 68%, and the hub from 53–55 FPS to a steady 60 with the 60 FPS mod on a Ryzen 7 9800X3D. Slower CPUs that dropped to 15 or 25 FPS there should gain the most.
  - The game's large texture arrays (about 2,800 entries a draw) are decoded once and shared by every render pipeline instead of each pipeline rebuilding its own copy: rebuilds went from about 9,000 a second to about 560, and 99% of draws now reuse their texture descriptors as they are.
  - The per-draw check for a draw sampling its own depth target only does that work when a depth target is attached, through a per-texture-bank list, instead of going over every bound texture on every draw.
- **Automatic updates.** On start, Fortune's Eden checks GitHub for a newer release and asks whether to update (*Update Now*, *What's New*, *Skip This Version* or *Later*; during a game it waits until you stop). Update Now downloads the new version with a progress bar, checks it against the SHA-256 checksum GitHub lists (no checksum, no install), then closes, installs and restarts into the new version. The `user` folder (saves, settings, keys, mods, shader cache) is never touched. Turn it off with *Check for updates* in the settings. **From v1.6.3 or older, download v1.6.4 by hand once**; later versions arrive through the updater.
- **AMD FSR 4 now starts on AMD cards.** The emulator's workaround for broken float16 maths in recent AMD drivers (and on NVIDIA RTX 30-series and newer, and Intel) switched float16 off for the whole device, which FSR 4's networks need, so it always fell back to FSR 3.1 (seen on an RX 7700 XT). Float16 now stays available to FSR 4 while the emulator's own shaders still avoid it.
- **Corrected: the FSR 4 library is AMD's own.** `amd_fidelityfx_upscaler_vk.dll` in v1.6.2 and v1.6.3 was AMD's signed FSR 4.1.2 Vulkan library from the FSR SDK 2.4, not the fsr4-vk community port the notes named. The notes and licences now say so (`licenses\amd-fsr4`), and the build only packages an AMD-signed copy. It supports Radeon RX 6000, 7000 and 9000.
- The timing statistics (`EDEN_BACK_THREAD_STATS=1`) split out texture-table builds, render-target updates and the feedback-loop check; the texture-table statistics (`EDEN_TEXTURE_TABLE_STATS=1`) show how many changed arrays came from another pipeline's decode.
- This build reuses v1.6.3's PGO profile: code unchanged since v1.6.3 keeps its profile-guided optimisation; the reworked draw setup is optimised without it.

## v1.6.3

- **Speed back to v1.6.1 levels: the v1.6.2 download was built without profile-guided optimisation (PGO).** Every release since v1.3 was a PGO build, with whole-program optimisation; v1.6.2 was a plain build by mistake, so the emulator's CPU-side code ran noticeably slower (typically 10–20% for an emulator). That showed most in CPU-limited moments: busy battles, turning around in Dagsion, ultrawide, 60 FPS and laptops. v1.6.3 is a PGO build again, trained on v1.6.3's own code.
- **Video memory no longer creeps past the limit in long sessions ([#23](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/23)).** v1.6.2 took memory still waiting to be destroyed off its measurement of video memory in use. Under steady texture cleanup on 6 GB cards that let real use climb up to 2 GB past the limit over a couple of hours, spilling into system memory, with growing stutter and audio dropouts. v1.6.3 measures real use again and instead pauses cleanup for 6 frames after a pass (what the v1.6.2 change was trying to achieve), except near the limit, where it cleans up every frame as v1.6.1 did. GPU texture conversion also no longer runs past its memory limit when a batch of large textures loads. The once-a-minute "Video memory in use" log line now also shows the texture estimate and that minute's cleanup activity.
- **Video memory: cleanup starts earlier on big cards.** Long sessions could creep towards the limit, where the driver and other programs push video memory over and performance drops. Faster cleanup now starts at 70% of the limit and hard cleanup at 85%, instead of 2.4 GB and 0.8 GB below it. A 24 GB card now settles around 12–15 GB instead of 15–17 GB; a 16 GB card around 8.4–10.2 GB; 8 GB cards are almost unchanged.
- **AMD FSR 3.1 is the default upscaler**, and the upscaler is now one dropdown, *Emulation → Configure → Graphics → Upscaler*, instead of three tick boxes: **AMD FSR 3.1** (any card, the default), **Game's own DLSS**, **NVIDIA DLSS** (GeForce RTX, the best picture; the download button appears under the list when it's chosen) and **AMD FSR 4** (Radeon RX 7000/9000). FSR 3.1 replaces the game's own DLSS network, which runs as dozens of emulated compute passes a frame, so it takes work off the emulator; the game's own DLSS is still there if you prefer its look. NVIDIA DLSS and FSR 4 fall back to FSR 3.1 when they can't start. **Updating keeps your choice:** if you'd ticked NVIDIA DLSS or FSR 4 it stays selected, otherwise you keep the game's own DLSS (v1.6.2's default); new installs and reset settings get FSR 3.1.
- **RomFS mods now work with `.nup` games ([#24](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/24)):** translations and texture replacements under `user\load\04007EF01F8AE000\<mod>\romfs` were listed in Add-Ons but never applied to a `.nup` (or a loose-file dump). Patch by ZinthosFett, adapted from Eglantine.
- **The status bar shows which upscaler is running**, next to the resolution: AMD FSR 3.1, NVIDIA DLSS, AMD FSR 4 or Game's own DLSS. If NVIDIA DLSS or FSR 4 can't start, it shows the FSR 3.1 that replaced it.
- **Experimental, for slow AV1 cutscenes ([#22](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/22)):** `EDEN_VIDEO_LOOKAHEAD=4` (2–8) lets the game queue several cutscene frames before it waits for one to be decoded. Normally every frame the game hands in waits for the oldest one, so the multi-threaded AV1 decoder only ever has about one frame to work on, and very heavy 1440p scenes (~50 ms a frame) drop the cutscene to 5–8 FPS. Off by default until it's confirmed not to break any cutscene.
- Build script: warns when a portable build is made without PGO.

## v1.6.2

A big fixes release: a full review of the emulator's code (about 120 findings) went into it, plus HDR and two new optional upscalers.

- **New: HDR output (HDR10).** Fortune's Weave has its own HDR mode on Switch 2, and it now runs on PC: turn on Windows HDR, then tick *Emulation → Configure → Graphics → HDR output (HDR10)* and start the game. The game's *Brightness* and *UI Brightness* options then work. Off by default; without Windows HDR the picture stays SDR.
- **New: optional AMD FSR 3.1 and AMD FSR 4** in place of the game's own DLSS (*Graphics*):
  - **Use AMD FSR 3.1 in place of the game's DLSS** runs on any graphics card (AMD's FidelityFX SDK, MIT; the upscaler from Eglantine, Duck912). In testing the game's own DLSS looked better at this game's 2× upscale, so FSR 3.1 is off by default; it's there for anyone who prefers it. Its sharpening follows the FSR Sharpness slider.
  - **Use AMD FSR 4 in place of the game's DLSS** is for AMD Radeon RX 7000/9000 cards, through [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk), a community Vulkan port of FSR 4.1.1 (not made or endorsed by AMD; RX 9000 runs the full FP8 model, RX 7000 the INT8 one; RX 6000 cards may run the INT8 one too, more slowly, but that's untested). Its provider, `amd_fidelityfx_upscaler_vk.dll`, is next to the exe. It didn't start on an NVIDIA RTX card in testing; when it can't start, FSR 3.1 (when ticked) or the game's own DLSS runs instead.
  - Order: FSR 4 or NVIDIA DLSS when ticked (only one of the two), then FSR 3.1 when ticked, else the game's own DLSS, the default. **On GeForce RTX cards, NVIDIA DLSS looks much better than either.**
- **New default image settings** (new installs and reset settings; your own settings are kept): *Window Adapting Filter* **Bilinear** (was FSR), anti-aliasing **off** (was FXAA, which only softened the game DLSS's ghosting and blurred text), **vsync Mailbox** (was off: no tearing, little added latency; drivers without Mailbox fall back to normal vsync) and FSR sharpness **25%** (only FSR 3.1 and the FSR window filter use it).
- **Crash fixes:**
  - Service threads reading memory the GPU had cached wrote past a 6-entry per-core table (random heap corruption).
  - The GPU's page-kind map was changed without its lock while the GPU thread read it.
  - Textures kept alive for a queued download could be destroyed while the GPU still used them (device lost); they now wait the usual 8 frames, and are no longer written back into memory the game has released.
  - A texture lookup could return a texture deleted earlier in the same pass.
  - The graphics driver service's open-file table had no lock; its fence callbacks are now removed before it's destroyed (crash at stop).
  - A stop that takes over 10 seconds no longer kills the emulation thread and reuses the half-stopped system (crash or hang on the next start): it asks to keep waiting or quit. The status bar no longer reads statistics while the game shuts down.
  - Switching between two 8 GB-plus memory layouts no longer keeps the old memory block.
- **Saves:**
  - Old saves from loose-file dumps (title 0) are now copied across even when an empty save folder for the game already exists, which some boot paths create first; the copy goes into a temporary folder and is only moved into place once it's complete, and the old save is kept. (Renaming folders always failed before; fixed.)
  - A `user` folder copied from another build, with a custom NAND folder set, now uses this build's own `user\nand` saves instead of the custom NAND.
- **Sound:**
  - Clicks at sound loops and where streamed sounds join: the ADPCM decoder read every sample with the wrong sign.
  - Battle sound stutter on 4-core CPUs ([#8](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/8)): the thread that answers the game's audio calls ran at normal priority while holding the lock the top-priority mixer needs. It now runs at top priority, and 6 sound buffers are queued instead of 4 (10 ms more latency, more margin).
  - A sound buffer could be played before its samples were written (garbage audio and growing latency, rarely).
  - Master volume above 100% dipped at 101% and clipped badly at 200%; it's now a straight 2× gain at 200%.
- **Graphics:**
  - Alpha-tested edges (foliage, hair, fences) no longer lose or gain one level of transparency (v1.5.1's fade fix rounded the cut-off onto a step instead of between steps).
  - Float atomic shaders reading through more than one buffer no longer fail to build (a missing effect; the bath scenes use them).
  - Engine uploads and the software blitter now use the Switch 2's texture layout.
  - A clear of one layer of a layered target no longer wipes every layer.
  - Shader fixes: unsigned shifts by 32 or more give 0; folded bit-field inserts are masked; each nested loop gets its full safety budget; the AMD-only discard reordering no longer drops discards or moves them out of loops.
  - AMD RX 5000/6000/7000/9000 cards keep min/max sampler filtering (it was switched off on every AMD card).
  - Drawing an upscaler's output (or the game's texture draws) no longer leaves the next draw thinking its pipeline is still bound.
  - A minimised window no longer spins the CPU.
  - HDR's 10-bit picture is shown with the right format.
- **Performance:**
  - **8 GB cards:** the texture cleanup counted video memory still waiting to be freed as in use, so it cleaned up again every frame for several frames, about 6× more than needed; and every texture written back waited for the GPU separately (up to 40 waits, the 20–90 ms hitches). Both fixed: one wait per pass.
  - About 1–3 ms less work per frame on the GPU thread: draws no longer copy their whole descriptor list.
  - GPU texture conversion submits work every 8 textures instead of after each one.
  - Shader builds: each block uses 4 KB instead of 12.5 KB, and the largest compute shaders sort their jumps much faster (smaller hitches).
  - Cheats that only write data no longer pause every CPU core 12 times a second to flush compiled code.
  - The sound mixer no longer clears 32 KB per voice every 5 ms.
- **Other fixes:** cheat files load once (Windows found them under both letter cases, so toggle cheats cancelled out); a `.nup` whose first update has no version name no longer shows 1.0.0; ultrawide only stretches the picture when its patch actually applied; corrupt `.romfs`, `.nup` and keys files can no longer hang or crash the loader; a rumble dropped by the 15 ms filter is no longer recorded as sent; writes crossing a 2 KiB boundary no longer lose half their GPU tracking; after three cancelled GPU waits the next one no longer pauses every CPU core; cutscene frames are invalidated after being written, and a stopped video decoder hands back its unshown frames.
- `EDEN_NVIDIA_VIDS=1` turns on dynamic vertex input on NVIDIA drivers 580.119.02 and newer (the old driver check was broken and always turned it off). Off by default, because it changes every pipeline and the shader cache would rebuild.

## v1.6.1

- **Fixed: 8 GB graphics cards dropping to about 2 FPS for minutes** (reported on an RX 7600 at nightfall). To re-compress a texture on the GPU, v1.6 first decodes it into a temporary full-colour image four times the texture's size, which can't be freed until the GPU is done with it. When a scene loads many textures at once, these piled up: on that card, 757 MB in half a second, past what the driver allows, so textures spilled into system memory and the game crawled at 2 FPS for three minutes. The temporary images now share a limit of 1/64 of the card's memory (128 MB on an 8 GB card, at most 256 MB), and over it the GPU's earlier work is waited for and its images reused. The log notes the first 20 times this happens.
- **Correction: v1.6 never had a UI ghosting bug.** The ghosting reported during v1.6 testing (button prompts at the screen edges leaving a faint shifted copy while moving) came from **NVIDIA Smooth Motion**, the driver's frame generation, which was switched on for `Fortune's Eden.exe` in the NVIDIA app. It only showed with that exe name, which is why renamed test builds looked clean, and it was wrongly blamed on a faulty v1.6 build. If you see UI smearing while moving, turn off Smooth Motion (NVIDIA app → Graphics → Program Settings) or frame generation in Lossless Scaling for this program.
- **The resolution options show this game's real sizes.** They still listed the original Switch 1 sizes (1X as 720p/1080p). Fortune's Weave on Switch 2 draws its 3D scene at 720p and upscales it with its own DLSS to 1440p, and the setting scales both, so the options now read 1X (1440p, native), 1.5X (2160p, 4K), 2X (2880p, 5K), 3X (4320p, 8K) and so on (docked mode).

## v1.6

- **Battle starts no longer freeze while textures convert: ASTC textures are now re-compressed on the GPU** (ported from Eglantine, Duck912). The game's textures are ASTC, which PC graphics cards can't use directly. Until now there were two choices: *Uncompressed*, converted on the GPU but needing so much video memory that only 20 GB cards could use it safely, or *BC3*, a quarter of the size but converted on the CPU, on the thread that prepares every frame, so the picture and sound froze for a moment whenever a battle loaded many textures at once. Now, with *ASTC decoding* on *GPU* (the default), the graphics card decodes each texture and re-compresses it to BC3 itself, off the CPU. In testing, a large battle on BC3 that used to freeze ran at 57-60 FPS throughout, using 7-11 GB of video memory where Uncompressed used 15-19 GB.
- **ASTC recompression Auto now always picks BC3** when the GPU re-compresses (Vulkan with *ASTC decoding* on *GPU*). The 20 GB rule is gone, so 8-16 GB cards get the same smooth result as big ones. With the CPU decoding modes or OpenGL, Auto keeps the v1.5.1 rule. *Uncompressed* is still there for anyone who wants the best image and has the video memory; *BC1* and *BC3* chosen by hand are re-compressed on the GPU too. The log says what Auto picked and when the first texture is re-compressed on the GPU. `EDEN_GPU_BCN_TRANSCODE=0` goes back to re-compressing on the CPU, for comparisons.
- **Smoother play when video memory fills up.** ASTC textures are slow to convert, so the texture cache kept them until memory was almost full and then had to throw many away at once, which stuttered. Once memory use passes the expected level, ASTC textures that haven't been used for 30 seconds are now freed early instead. In testing with video memory capped at 7 GB, a scene that dropped to about 35 FPS ran at 55-60.
- Includes v1.5.2's fix for the NVIDIA DLSS crash.

## v1.5.2

- **Fixes a crash with NVIDIA DLSS on** (ported from Eglantine, Duck912). A few minutes into some sessions, often at a scene change, the emulator closed with an error in `nvoglv64.dll` (NVIDIA's driver). Drawing DLSS's output into the game's image read its target framebuffer on the graphics driver's thread, after the texture cache could already have removed it, so the driver was asked to draw into a framebuffer that no longer existed. It now reads the framebuffer when the command is recorded. This was also in v1.5 and v1.5.1.

## v1.5.1

- **Game updates: the right version, and loose updates** ([#11](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/11)). The game's main menu always showed v1.0.0 for a `.nup` or a loose dump, even with an update in it, because the emulator only knew the version of installed games. It now reports the update's version, also for a `.nup` that doesn't record one (it then reads the update's own control data, or recognises the 1.1.0 build). Loose dumps can now carry an update too: put it in an `updates\<number>\` folder (with its `exefs`, `romfs` and `control.nacp`) next to the dump's `exefs`/`romfs`, or next to a `base` folder holding the dump. The newest one runs, with its files laid over the base's. **Note:** the ultrawide option is made for game version 1.1.0 (code build `DB93B012…`); other versions have different code, so it doesn't apply to them. The 60 FPS patch is set up for 1.1.0 too and may also work on 1.0.0 (not confirmed yet). The log's `name=main` line shows which build is running.
- **Big graphics cards can use more of their video memory.** The default *VRAM Usage Mode* (Conservative) capped every card from 8 GB up at about 7 GB, even a 24 GB one, so at higher resolutions or with the ultrawide option the emulator kept clearing and reloading textures. It now allows three quarters of the card's memory, and never less than before: 8 GB and smaller cards are unchanged, a 12 GB card gets 9 GB, a 16 GB card 12 GB, a 24 GB card 18 GB. The free quarter avoids the stutter *Aggressive* can cause by running right up to the card's limit. The log shows the limit.
- **ASTC recompression is now Auto by default.** The game's textures are ASTC, which PC graphics cards can't use directly, so the emulator converts them. *Uncompressed* is converted on the GPU (smoothest, best quality) but needs a lot of video memory; *BC3* takes a quarter of the space but is converted on the CPU, which can freeze the picture and sound for a moment when a battle loads many textures at once. *Auto* picks for you at each start: **Uncompressed** when the emulator may use about 15 GB of video memory or more (in practice 20 GB cards and up, such as an RTX 3090/4090 or RX 7900 XT/XTX) and the resolution is below 1.5x (with the ultrawide option, only at 1x); **BC3** otherwise. The log says which it picked and why. Choosing Uncompressed, BC1 or BC3 yourself still works as before. New installs get Auto, and so do configs that never changed this setting.
  > ⚠️ **Don't pick Uncompressed by hand on a card with less than 20 GB of video memory.** The game used 13.5 GB within a minute at 1x with ultrawide on Uncompressed: on smaller cards it runs out of memory, and the game stutters or drops to single-digit FPS (an RX 7700 XT fell to about 5 FPS in Dagsion). If you copied an old `user` folder that has Uncompressed set, change it to Auto in *Graphics → Advanced → ASTC recompression*.
- **Sound no longer stutters when the CPU is busy** ([#8](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/8)). The threads that mix the game's sound ran at a lower priority than the emulated CPU cores and the GPU threads, so on CPUs with 4 fast cores they only got leftover time in battles and busy cutscenes, and the sound stuttered or crackled. They now run at the same top priority. They only mix for a few milliseconds at a time, so nothing else slows down.
- **The emulator's threads wait properly on newer CPUs** (ported from Eglantine #120 and #137). On Intel 12th gen and newer, and in some cases on AMD, a thread waiting for a set time could wake early and report "timed out" (or miss a signal), so threads like the timing thread kept waking and rechecking instead of sleeping, using CPU the game could have had. They now wait until the deadline or the signal.
- **Laptops and 4-core CPUs: smoother sound.** This build splits the GPU work over two busy threads for extra speed. On CPUs with only 4 fast cores (many laptops have 4 fast and 4 slow ones) that second thread competed with the game's own threads, and battles' sound crackled or beeped even at 30 FPS ([#8](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/8)). It's now off automatically on CPUs with 4 or fewer fast cores; the log says which way it went. `EDEN_GPU_SPLIT=2` turns it back on, `EDEN_GPU_SPLIT=0` turns it off anywhere.
- **Logs show frame pacing again, without growing back.** Every 10 seconds one line gives the FPS, the average and worst time between frames, and how many frames took over 50 ms and over 100 ms. So a log shows whether a PC keeps up, at about 360 lines an hour instead of one line per frame (v1.4.2's 27 MB logs).
- **Logs show video memory use** about once a minute, the texture cache's limits at the start, and any cleanup of old textures that takes 20 ms or more.
- **Logs record what NVIDIA DLSS is given whenever it changes**, not only at the start, to track down scenes where it misbehaves ([#5](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/5)).
- **Fewer shader hitches during fades** (from Eglantine #544, measured on this game: one scene built 101 shaders instead of 925). When the game fades something in or out with its alpha test on, the cut-off value moved by less than one visible step each frame, and each value built a new shader. It's now rounded to the 256 steps the game's textures can actually show.
- **Cutscene video keeps up with the voices** (adapted from Eglantine). The game shows one video frame for each frame it draws, so on a PC that runs a cutscene below the video's frame rate, the picture fell further and further behind the sound. Frames the sound has already passed are now skipped, so the picture catches up. Nothing changes on a PC that keeps up. `EDEN_VIDEO_PACE=0` turns it off.
- **Crash fixes from Eglantine:**
  - The GPU no longer keeps old data after some memory copies.
  - The window can be resized, fullscreened or alt-tabbed without the swapchain being destroyed while the GPU still uses it.
  - A shader that fails to build no longer crashes the driver; its draws are skipped.
  - A depth-buffer copy no longer loses the device on NVIDIA.
  - Two threads can no longer clear the same list of GPU-written memory at once.
  - Stopping the game no longer crashes with *ASTC decoding* set to *CPU Asynchronously* (a texture still decoding wrote into freed memory).

## v1.5

- **Fixed: the sky turning black after opening the map** (in Dagsion: blue in the menu, black after the map, sometimes staying black). The emulator's texture cache frees images that haven't been used for a few frames. Images the GPU drew into are normally kept or copied back to game memory first, but some (ones that overlap other images awkwardly) were freed with nothing kept. The game draws its sky colour table once and keeps reading it, so when the map covered the screen long enough for the table to be freed, the emulator rebuilt it from game memory, which never had it: black sky, clouds still fine. Small images the GPU drew (up to 4 MB; the sky table is one) are now always kept, even when video memory is nearly full; larger ones are still freed as before. In testing the sky stayed blue through every map visit (it went black on the first visit before), keeping at most about 160 MB more.
- **Slightly smoother busy scenes.** In crowds the GPU back thread, which limits the frame rate there, asks "has the GPU written to this memory?" about 850,000 times a second, and 87% of the answers are no. A bitmap with one bit per 4 KiB page now answers most of those without searching the range tree. The answers are exactly the same (a page is only cleared after a full search finds it empty); tested against a byte-by-byte reference over about 1.7 million random queries. A small gain in crowded towns, none elsewhere. `EDEN_NO_PAGE_FILTER=1` turns it off.
- **Fixed: recurring stutter for some players** ([#2](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/2)). Since v1.3.1, each CPU core skipped the GPU-cache check for the last 16 pages it wrote to instead of only the last one. Testing had already shown that this made the game "feel like 10 FPS" while the FPS counter looked fine: the game's writes reached the GPU one batch late, so frames came out unevenly. It went into the build anyway, by mistake, and the docs said it was off. It's off now (back to 1 page, as in v1.3). `EDEN_CPU_WRITE_PAGES=16` still turns it on for testing. Thanks to the player who traced it.
- **Fixed: cutscene voices missing and dialogue animations ending early with CPU Clocks on Boost or Overclock** ([#1](https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch/issues/1)). Those settings made the game's clock run 1.7× (Boost) or 2× (Overclock) too fast, so movies played at about 51 FPS instead of 30 and their voices dropped out. The game's clock now always runs at the real rate, and the CPU Clocks setting is gone; a saved Boost or Overclock no longer does anything.
- **Much smaller logs.** A 34-minute session wrote a 27 MB log, too big to send most places: 87% of it was one line written every frame. That line now only appears for the first few frames and when the picture size changes, and "Unmapped Device ReadBlock" is logged once per read instead of once per 4 KiB page. The same session now writes about 1-2 MB.
- **The version is in the window title** (*Fortune's Eden v1.5*) and at the top of the log, so it's clear which release a log came from.
- **Optional ultrawide 3440x1440 (21:9) setting, off by default.** *Graphics → Ultrawide 3440x1440 (21:9) for Fortune's Weave* applies [Kompas9](https://github.com/Kompas9/Switch-Game-Patches)'s ultrawide patch (v1.1, MIT, built in) when the game starts and stretches the picture to the window: the 3D camera is widened to 3440x1440 and the HUD, menus and movies stay centred at 16:9 (movies get black bars). For 21:9 monitors, played fullscreen. Only for game version 1.1.0 (build ID `DB93B0125F485060`); other versions are left alone. A setting rather than a mod folder, so it stays off when an older `user` folder is copied over this build.

**Updating from v1.4.2:** replace `Fortune's Eden.exe` with the new one, or extract v1.5 to a new folder and copy your old `user` folder into it.

## v1.4.2

- **Fixed: 3D scenes losing their background** (bath scenes went black around the characters, sometimes only after a while). Some scenes use a compute shader with a shared-memory compare-and-swap (`ATOMS.CAS`), which the native shader translator didn't support, so every run of it was skipped. It's now translated (to a Vulkan atomic compare-exchange).
- **Another bath-scene shader no longer skipped:** a float add to GPU memory (`RED.ADD.F32`) is now translated too.

**Updating from v1.4.1:** replace `Fortune's Eden.exe` with the new one, or extract v1.4.2 to a new folder and copy your old `user` folder into it.

## v1.4.1

- **Cutscenes no longer drop to about 5 FPS.** Fortune's Weave's 1440p cutscenes are AV1 videos, and in their busiest scenes decoding a frame took up to ~40 ms, more than the 33 ms a 30 FPS video allows. Whenever the video fell behind, the game's frame rate collapsed until the scene got lighter. Their frames are encoded as a single tile, so the decoder (libgav1) worked mostly on one thread however many it had. It now decodes several frames at once (frame-parallel, 12 threads on a 16-thread CPU): in the same cutscene the decoder now keeps up with time to spare, and the cutscene plays at a steady 30 FPS. `EDEN_AV1_THREADS=N` sets the thread count; `EDEN_AV1_FRAME_PARALLEL=0` decodes one frame at a time as before.
- **Stricter check on the v1.3.1 handle-table shortcut** (a precaution for the sky sometimes turning dark in Dagsion). The shortcut reuses texture-handle tables the game builds on the GPU instead of waiting for them, and checks the GPU's copy afterwards. A table now needs 32 identical reads before the shortcut is used for it (was 8), a table that ever changes is always waited for from then on, and every table is still read for real at least every 250 ms. Changes are logged (`Texture-handle table ... changed`). In testing no table changed, so 60 FPS keeps the v1.3.1 gain. `EDEN_SPECULATIVE_HANDLES=0` still turns the shortcut off.

- **A copied `user` folder now uses its own saves.** Settings stored the full path of the build's NAND, SD, mod and dump folders, so a `user` folder copied into a new build (as the update steps say) kept using the folders of the build it came from: saves kept going to the old folder, and *Open Save Data Location* opened that one. Paths pointing at another build's `user\nand` (and `sdmc`, `load`, `dump`, `tas`) are now ignored in favour of this build's own, and this build's own folders are stored without a full path, so the `user` folder works wherever it is copied. A folder you picked yourself elsewhere is kept. If you updated by copying a `user` folder before, your latest saves may still be in the old build's `user\nand`: copy that `nand` folder over this build's `user\nand`.

**Updating from v1.4:** extract v1.4.1 to a new folder and copy your old `user` folder into it.

## v1.4

> **30 FPS is how Fortune's Weave runs on Switch 2**, and this build runs it smoothly by default. The 60 FPS patch stays **off by default**.

- **Optional NVIDIA DLSS in place of the game's own DLSS** (GeForce RTX cards). The game upscales with a small DLSS network made for the Switch 2, which can leave ghosting around moving characters. With *Graphics → Use NVIDIA DLSS in place of the game's DLSS* ticked, the full PC version of NVIDIA DLSS (through NVIDIA Streamline) does that step instead, from the game's own colour, depth, motion vectors and camera jitter. Off by default. NVIDIA's files aren't included: the **Download from NVIDIA** button under the setting fetches them from NVIDIA's Streamline release (or uses the DLSS the NVIDIA app has downloaded). If DLSS can't run, the game's own DLSS runs as before. Ported from Duck912's work in Eglantine; this build looks up DLSS's input images without creating any, which stopped driver crashes in an early port.
- **New default image settings:** AMD FSR window filter at **0% sharpness** (was 50%) and **FXAA** anti-aliasing (was off). FXAA softens the ghosting of the game's own DLSS, and 50% sharpness looked oversharpened with NVIDIA DLSS.

**Updating from v1.3.1:** extract v1.4 to a new folder and copy your old `user` folder into it. A copied `user` folder keeps your old image settings: set *Window Adapting Filter* sharpness to 0% and *Anti-Aliasing* to FXAA yourself if you want the new defaults.

## v1.3.1

> **30 FPS is how Fortune's Weave runs on Switch 2**, and this build still runs it smoothly by default. The 60 FPS patch stays **off by default**, but it's noticeably smoother in busy scenes now; on a fast PC, try it at **1x resolution**.

- **Smoother 60 FPS in busy scenes.** The game builds some texture-handle tables with compute shaders and draws with them a few draws later. Reading them back made the emulator wait for the GPU about once a frame (up to ~1.3 ms), although the tables never change. Once a table has come back identical 8 times in a row, the emulator uses the copy it already has, checks the GPU's version once it's ready, and goes back to waiting for that table if they ever differ (in testing they never did). `EDEN_SPECULATIVE_HANDLES=0` turns this off.
- **Less lock contention from CPU writes.** Each CPU core now remembers the last 16 GPU-owned pages it wrote to (was 1), so repeated writes skip the cache lock: up to ~200,000 fewer lock calls a second in busy scenes. `EDEN_CPU_WRITE_PAGES=1` restores the old behaviour.
- **Fewer shader-compile stutters:** translated shaders are reused between graphics pipelines that share them (Wael3rd, from PokeDinner, an Eglantine fork). `EDEN_NX2_SHADER_REUSE=0` turns it off.
- **Measured** on a Ryzen 7 9800X3D + RTX 4090 at 60 FPS, 1x, running the same loop in Dagsion: doubled frames in the busiest section went from about 45% to about 20%; over the whole run, frames on time went from 78% to 89%.
- **Profile-guided optimisation retrained** with these changes.
- **Build script:** `-Symbols` builds a copy with debugging symbols for profiling (same code).

**Updating from v1.3:** extract v1.3.1 to a new folder and copy your old `user` folder into it (saves, settings and shader cache), or just `user\nand` for saves only.

## v1.3

> **30 FPS is how Fortune's Weave runs on Switch 2**, and this build runs it smoothly by default. The 60 FPS patch stays **off by default**; on a fast PC, try it at **1x resolution**.

> **Saves are stored next to `Fortune's Eden.exe`** (in `user\nand`), **not** in `%APPDATA%\eden`. See [Saves](README.md#saves).

- **Renamed to Fortune's Eden:** the app is now `Fortune's Eden.exe`, and its window is titled *Fortune's Eden*.
- **Profile-guided optimisation** trained on Fortune's Weave (Dagsion, battles, menus). The emulator's CPU-side code is laid out for the paths the game uses; still runs on any x64 CPU. At 60 FPS it cut doubled frames by about a quarter in testing; at 30 FPS the game was already on time almost every frame.
- **Loose files and `.nup` share one save folder.** Loose-file dumps now register the game's real ID (`04007EF01F8AE000`) instead of `0000000000000000`. The first time you start the game, an old loose-file save is copied across automatically if there's no save under the new ID yet (the old folder is kept).
- **Shutdown and restart fixes** from PokeDinner (an Eglantine fork): no hang on stop when the audio renderer outlives its manager (kraibse); starting the game again in the same window no longer hangs or crashes (Takoroka).
- **Error-reporting crash fix** and clearer logs for unsupported system calls and missing keys (xDarkWindx).
- **60 FPS advice:** measured on a Ryzen 7 9800X3D + RTX 4090, 1x resolution halved the 60 FPS choppiness compared with 2x (doubled frames 3.4% vs 6.5%).
- **Build script:** portable PGO (`-Pgo Train` / `-Pgo Use`) and `Save-PGO.ps1`.

**Updating from v1.2.x:** extract v1.3 to a new folder and copy your old `user\nand` folder into it (and `user\cache` to keep the shader cache). Loose-file saves are carried over automatically.

## v1.2.1

- **All settings are now global** (*Emulation → Configure*) instead of in a Fortune's Weave per-game profile, including the 12 GB memory layout. A per-game profile only applies when the game's ID matches exactly, so some dumps (for example a `.nup` packed with a different ID) or an old `user` folder copied over could fall back to the defaults, including 4 GB, which hangs on loading. The global settings now match what the profile had: GPU accuracy Fast, asynchronous shaders on, vsync off, 16x anisotropic filtering, reactive flushing off, VRAM Conservative, FSR at 50% sharpness, anti-aliasing off. The emulator itself is unchanged from v1.2.

**Updating from v1.2:** no need to re-download if your game loads. Otherwise set *Emulation → Configure → System → Memory Layout* to **12 GB**, or extract v1.2.1 and copy your `user\nand` folder into it.

## v1.2

> **The 60 FPS patch is now OFF by default.** Its frame pacing is poor and can feel choppy, while 30 FPS runs flawlessly. It's still included: to try it, right-click the game → *Properties → Add-Ons* and tick **60fps**.

> **Saves are stored next to `eden.exe`** (in `user\nand`), **not** in `%APPDATA%\eden`. See [Saves](README.md#saves).

- **60 FPS patch off by default** (unticked under *Add-Ons*; the files are still included).
- **AMD graphics fix:** shader registers read before being written are now zero, as on NVIDIA, instead of leftover garbage that can show as stray coloured patches on AMD GPUs. Ported from [Djihads80's Eglantine](https://github.com/Djihads80/Eglantine) (by arisqz). No change on NVIDIA.
- **Preset graphics:** AMD FSR window adapting filter at 50% sharpness, anti-aliasing off (was FXAA).
- **Building from source fixed:** `cmake-options.txt` is now in the repo, and the README lists exactly what to put together.
- `source.zip` updated with the AMD fix.

**Updating from v1.1:** extract v1.2 to a new folder and copy your old `user\nand` folder into it to keep your saves. If you copy the whole old `user` folder instead, your old graphics and 60 FPS settings come with it.

## v1.1

> **The 60 FPS patch has poor frame pacing: it can feel choppy and uneven. At the game's normal 30 FPS this build runs flawlessly.**
> The patch is on by default. For the smoothest experience, turn it off. If it still feels choppy **delete** the folder `user\load\04007EF01F8AE000\60fps` (next to `eden.exe`). Unticking it under *Add-Ons* isn't always enough.

> **Saves are stored next to `eden.exe`** (in `user\nand`), **not** in `%APPDATA%\eden`. Copy your `nand` folder in to keep existing saves. See [Saves](README.md#saves).

- **`.nup` support.** Fortune's Weave can now be played from a single `.nup` package as well as from loose files. Add the folder that holds it to the game list, or open it with *File → Load File*. The NUP loader is ported from [Djihads80's Eglantine](https://github.com/Djihads80/Eglantine) (GPL-3.0-or-later). Only playing is supported: packing, unpacking and editing NUPs aren't included, and add-on content inside a `.nup` isn't loaded.
- **VRAM Usage Mode is now Conservative** in the Fortune's Weave profile (it was Aggressive). On Aggressive the texture cache can grow to almost the whole graphics card during a long session and spill into system RAM, causing repeated one-second freezes of picture and sound after about half an hour. Conservative caps it at about 6 GB plus 1 GB per resolution-scale step.
- The download zip now uses standard forward-slash paths, so it extracts correctly with 7-Zip and other tools as well as Windows.
- `source.zip` is updated with the NUP code.
- **Switching from loose files to `.nup`?** The two save to different folders (`0000000000000000` vs `04007EF01F8AE000` inside your profile folder). Copy the first to the second to carry your progress over. See [Saves](README.md#saves).

**Updating from v1.0:** extract v1.1 to a new folder, then copy your old `user\nand` folder into it to keep your saves (or copy the whole old `user` folder to keep your settings and shader cache too). If you copy the whole `user` folder, set VRAM Usage Mode to Conservative yourself: right-click the game → *Properties → Graphics → Advanced*.

## v1.0

- First release: portable build of xDarkWindx's eden-fefw 1.12 (on Duck912's Fortune's Weave fork) with a start-up crash fix, settings tuned for Fortune's Weave, the 60 FPS patch and the Microsoft C++ runtime included.
