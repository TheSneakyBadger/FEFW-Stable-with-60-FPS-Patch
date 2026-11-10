# Fortune's Eden: source

This branch is the complete source of **Fortune's Eden**, a portable Windows build of Eden made for
Fire Emblem: Fortune's Weave. Releases, the changelog and the build script are on the `main`
branch of this repository: https://github.com/TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch

The tip of this branch is the current development state, which can be ahead of the latest
release. Releases from v1.6.7.2 on are tagged `source-v<version>` (for example
`source-v1.6.7.2`), and every release package also carries its source as `source.zip`.

## Based on

- [Eden](https://git.eden-emu.dev/eden-emu/eden) (GPL-3.0-or-later)
- Duck912's [eden-ns2-fe](https://github.com/Duck912/eden-ns2-fe): Switch 2 support and the first
  Fortune's Weave fixes (the first commits of this history)
- xDarkWindx's [eden-fefw](https://github.com/xDarkWindx/eden-fefw) 1.12
- The NUP loader and DLSS work from [Eglantine](https://github.com/Djihads80/Eglantine), and fixes
  ported from KektJ's Eglantine builds
- nanachi7700's texture cache bookkeeping patch
- Kompas9's ultrawide patch (MIT, `src/core/file_sys/fefw_ultrawide.h`)
- [dav1d](https://code.videolan.org/videolan/dav1d) for the AV1 cutscenes (BSD 2-clause,
  `externals/dav1d/COPYING`)

## Building (Windows)

Use `Build-Local-MSVC.ps1` from the `main` branch with Visual Studio 2026 (MSVC 19.5x) and CMake:

```powershell
.\Build-Local-MSVC.ps1 -Version 1.12 -Portable
```

It configures this tree with the options in `cmake-options.txt`, downloads the dependencies through
CPM, and packages a portable folder. No keys, firmware, games or saves are part of this source.

## Licence

GPL-3.0-or-later, as Eden (see `LICENSE.txt` and `LICENSES/`). Third-party components keep their
own licences, listed above.
