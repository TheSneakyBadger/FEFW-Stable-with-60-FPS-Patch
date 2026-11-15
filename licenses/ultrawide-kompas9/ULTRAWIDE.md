# Fire Emblem: Fortune's Weave - 3440x1440 ultrawide fix

Package **v1.2** combines the user-tested camera, UI/clipping, movie, world-marker, 2D-effect and stat-chart patches
into **one mod folder**. Target aspect ratio is **43:18 (3440x1440)**, commonly
called 21:9. This is not a universal patch for other ultrawide ratios.

## Changes in v1.2

- Fixes the radar charts on the unit screen's Stats page (basic, battle and growth-rate): the polygons were
  displaced from their circles. The game draws them itself, not as a layout pane, so the UI fix never reached them;
  their x is now put on the centred 16:9 area.
- Everything else is unchanged from v1.1.
- Replace v1.1 and all earlier ultrawide folders; enable only this mod.

## Changes in v1.1

- Corrects floating world/NPC marker positions as the camera moves.
- Corrects 2D menu-effect projection and clips matching effects to the centred 16:9 UI area.
- Retains the previous camera, HUD, list clipping and movie corrections.
- Replaced v1.0 and the World-UI / UI-Effects test folders.

## Included fixes

- **3D camera:** widens matching default perspective cameras from 16:9 to 43:18.
- **UI:** keeps matching HUD, text, portraits and dialogue projections centered
  at 16:9 proportions, with corresponding UI scissor correction for lists.
- **Prerendered movies:** fits the native-movie path into a centered 16:9 region,
  clears the side bars to opaque black, then restores the full viewport.

At 3440x1440, the intended UI/movie area is 2560x1440, with 440 pixels on each
side. The wider world remains visible behind the UI; movies have black bars.
Integer viewport rounding can cause a subpixel/one-pixel difference.

## Supported build

| Item | Value |
| --- | --- |
| Game version | 1.1.0 |
| Title ID | `04007EF01F8AE000` |
| Main Build ID, trailing zeros omitted | `DB93B0125F4850601D61C3EDDE012F8EB2ECFE77` |
| Package version | 1.1 |
| Output aspect | 3440x1440 / 43:18 |

Other game updates need a matching patch. Resolution scaling does not change
the intended output aspect, but not every scaling/emulator setting is tested.

## Download and install

1. Download [Fire-Emblem-Fortunes-Weave-Ultrawide-3440x1440-v1.2.zip](https://github.com/Kompas9/Switch-Game-Patches/raw/refs/heads/main/downloads/Fire-Emblem-Fortunes-Weave-Ultrawide-3440x1440-v1.2.zip).
2. Stop the game. **Disable/remove all earlier ultrawide test mods:**
   - `FE-Fortunes-Weave-3440x1440-TEST`
   - `FE-Fortunes-Weave-3440x1440-UI-TEST2`
   - `FE-Fortunes-Weave-3440x1440-UI-TEST3`
   - `FE-Fortunes-Weave-3440x1440-MOVIE-TEST1`
   - `Ultrawide-3440x1440-v1.1`
3. Right-click Fire Emblem in Lichii -> **Open Mod Data Location**.
4. Copy the entire **`Ultrawide-3440x1440-v1.2`** folder there and enable it in Properties / Add-Ons:

   ```text
   <game mod data location>/
     Ultrawide-3440x1440-v1.2/
       exefs/
         DB93B0125F4850601D61C3EDDE012F8EB2ECFE77.ips
   ```

5. Select **Graphics -> Aspect Ratio -> Stretch to Window** and use a
   **3440x1440 window/fullscreen output**. Restart the game.
6. Keep emulator speed at **100%** and normal guest clock settings.

**The FPS patch is separate:** `60FPS-v1.1` may remain enabled. This package
neither includes nor overwrites its two instruction changes. No emulator
rebuild is needed. Other aspect/FOV mods should be disabled.

The log should show `Applying IPS patch from mod "Ultrawide-3440x1440-v1.2"`.
To uninstall, stop the game, disable/remove this folder, then restart. Select
the normal 16:9 output setting if you no longer use the ultrawide camera patch.
Original game files and emulator binaries are not replaced.

## Testing and limitations

The user reported correct-looking wide gameplay, centered UI, corrected weapon
selection clipping, and movie proportions after testing the three components.
Tests used the exact game build above with Lichii build 88 and/or the project's
custom PGO/AV1 build. This is limited visual testing, not a full playthrough.

The latest combined local test was reported working by the user, including the Stats page
radar charts added in v1.2. Offline ARM64 checks include 91 world-coordinate cases and 18
scoped 2D-effect cases, plus an emulated check of the stat-chart code (9 rescaled x values,
other data untouched), in addition to earlier camera/UI/movie validation. This is limited
visual testing, not a full playthrough. v1.2 is v1.1 plus five records (the stat-chart
redirect and its code cave); the v1.1 records are unchanged.

**Known issue:** torch illumination can flicker after loading free roam and become
stable after a battle. This has been reported without the ultrawide mod too;
its cause is still under investigation. This update does not claim a lighting fix.

Some non-default camera/UI paths, unusual source-video shapes, fullscreen
effects, input hitboxes and other emulator/hardware combinations may still
need adjustments. Report the game Build ID, emulator build, scene, output
dimensions, screenshot and log for regressions. Higher FPS is not guaranteed.

## Implementation and credits

`ultrawide-patch-records.json` records exact original/replacement bytes and
provenance. `tools/build_ultrawide_patch.py` recreates the IPS32 and can verify
the supported decompressed NSO dump without changing it. The source dump,
captured frames, logs, game assets, keys, firmware and saves are not distributed.

Located and tested during Kompas9's local Lichii / Fortune's Weave work.
[Fl4sh9174's Three Houses ultrawide patch](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods)
was inspected as a reference; its offsets were not copied. The Fortune's Weave
camera, UI, scissor, movie, world-marker and 2D-effect hooks were located in the matching executable. The stat-chart site was found while porting these fixes to
the fefw-recomp runtime.
Original documentation and tools are covered by the repository MIT license;
game software retains its respective rights.
