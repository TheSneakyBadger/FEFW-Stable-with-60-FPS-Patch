# Builds the local eden-fefw source (this folder) with MSVC + Ninja, Release, x64.
# Run from any PowerShell window in the folder that holds this script and the source:
#   Set-ExecutionPolicy -Scope Process Bypass; .\Build-Local-MSVC.ps1
# Output goes to .\Eden-FEFW-local\eden.exe. The existing .\eden.exe is not touched.
# 1.12 build:  .\Build-Local-MSVC.ps1 -Version 1.12   (source eden-fefw-1.12, output Eden-FEFW-1.12-local)
# Shareable:   .\Build-Local-MSVC.ps1 -Version 1.12 -Portable
#              (any x64 CPU, VC++ runtime bundled, output FEFW-Stable-60FPS-Portable + .zip)
# PGO for the shareable build (profile-guided optimisation, still runs on any x64 CPU):
#   1. .\Build-Local-MSVC.ps1 -Version 1.12 -Portable -Pgo Train
#      builds a measuring copy in FEFW-PGO-Training. Play Fortune's Weave in it (busy areas,
#      battles, menus) for 10-20 minutes, then close the emulator normally.
#   2. .\Build-Local-MSVC.ps1 -Version 1.12 -Portable -Pgo Use
#      builds FEFW-Stable-60FPS-Portable optimised with what step 1 recorded (pgo-fefw\).

[CmdletBinding()]
param(
    [ValidateSet('kcode', '1.12')]
    [string]$Version = 'kcode',
    [ValidateSet('AVX512', 'AVX2', 'None')]
    [string]$RenderIsa = 'AVX512',          # Ryzen 7 9800X3D (Zen 5) supports AVX-512
    [ValidateSet('Auto', 'On', 'Off', 'Train', 'Use')]
    [string]$Pgo = 'Auto',                  # Auto: only with the compiler and source the profile was made with
                                            # Train/Use: portable PGO, see the header
    [switch]$Portable,                      # build for any x64 CPU and package it for other people
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Symbols,                       # with -Pgo Train/Use: debugging symbols (eden.pdb) for
                                            # profiling, built in a separate folder; same code
    [switch]$Clean
)
if ($Pgo -in 'Train', 'Use' -and -not $Portable) { throw '-Pgo Train/Use are for the portable build: add -Portable.' }
if ($Portable) { $RenderIsa = 'None'; if ($Pgo -notin 'Train', 'Use') { $Pgo = 'Off' } }

$ErrorActionPreference = 'Stop'
$root      = $PSScriptRoot
$pgoDir    = Join-Path $root 'pgo-crowd-20261104'
if ($Version -eq '1.12') {
    $sourceDir = Join-Path $root 'eden-fefw-1.12'
    $buildDir  = Join-Path $root 'build-1.12'
    $outDir    = Join-Path $root 'Eden-FEFW-1.12-local'
    $zip       = Join-Path $root 'eden-fefw-1.12-local.zip'
    if (-not (Test-Path $sourceDir) -and (Test-Path $zip)) {
        Write-Host "Extracting $zip"
        Expand-Archive -LiteralPath $zip -DestinationPath $root
    }
} else {
    $sourceDir = Join-Path $root 'eden-fefw'
    $buildDir  = Join-Path $root 'build'    # must sit beside pgo-crowd-20261104 (profile path is ../)
    $outDir    = Join-Path $root 'Eden-FEFW-local'
}
if ($Portable) {
    $buildDir = "$buildDir-portable"
    $outDir   = Join-Path $root 'FEFW-Stable-60FPS-Portable'
}
# Portable PGO: Train and Use share one build folder (compiled with /GL), so Use only re-links.
$fefwPgoDir = Join-Path $root 'pgo-fefw'
$trainDir   = Join-Path $root 'FEFW-PGO-Training'
if ($Pgo -in 'Train', 'Use') {
    $buildDir = "$buildDir-pgo"
    if ($Pgo -eq 'Train') { $outDir = $trainDir }
    if ($Symbols) { $buildDir = "$buildDir-sym" }
    if ($root -match '\s') { throw "PGO needs a folder path without spaces; move this folder (now $root)." }
}
$logFile   = Join-Path $root 'build-log.txt'
$pgoCompiler = '19.51.36260'
$toolLog   = Join-Path $root 'cmake-output.txt'

# Windows PowerShell's transcript misses native stderr, so run tools through cmd and tee everything.
function Invoke-Logged([string]$Exe, [string[]]$Argv) {
    $line = ($Argv | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + $_ + '"' } else { $_ } }) -join ' '
    Write-Host "`n> $Exe $line" -ForegroundColor Cyan
    & cmd.exe /d /s /c "`"$Exe $line 2>&1`"" | Tee-Object -FilePath $toolLog -Append | Out-Host
    return $LASTEXITCODE
}

Start-Transcript -Path $logFile -Force | Out-Null
if (Test-Path $toolLog) { Remove-Item -LiteralPath $toolLog -Force }
try {
    foreach ($p in @($sourceDir, (Join-Path $sourceDir 'CMakeLists.txt'), (Join-Path $root 'cmake-options.txt'))) {
        if (-not (Test-Path -LiteralPath $p)) { throw "Missing: $p" }
    }

    # Enter the x64 MSVC environment if this isn't already a Developer PowerShell.
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path $vswhere)) { throw 'Visual Studio not found (no vswhere.exe). Install VS with "Desktop development with C++".' }
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { throw 'No Visual Studio install with the x64 C++ tools found.' }
        Write-Host "Using Visual Studio at $vs"
        Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    }
    foreach ($tool in 'cl', 'cmake', 'ninja', 'git') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "'$tool' not found. cmake/ninja come with VS's 'C++ CMake tools for Windows' component; git with Git for Windows."
        }
    }

    $clBanner = (& cmd.exe /d /c 'cl 2>&1' | Out-String)
    if ($clBanner -notmatch '(19\.\d+\.\d+)') { throw 'Could not read the MSVC version.' }
    $clVersion = $Matches[1]
    if (((& cmake --version) | Select-Object -First 1) -notmatch '(\d+\.\d+\.\d+)') { throw 'Could not read the CMake version.' }
    $cmakeVersion = [version]$Matches[1]
    if ($cmakeVersion -lt [version]'3.31') { throw "CMake 3.31+ needed, found $cmakeVersion." }
    Write-Host "MSVC $clVersion, CMake $cmakeVersion, $Jobs jobs, renderer ISA $RenderIsa"

    $usePgo = switch ($Pgo) {
        'On'   { $true }
        'Off'  { $false }
        'Auto' { $Version -eq 'kcode' -and $clVersion -eq $pgoCompiler -and (Test-Path (Join-Path $pgoDir 'eden.pgd')) }
        default { $Pgo -eq 'Use' }
    }
    $pgd = (Join-Path $fefwPgoDir 'eden.pgd').Replace('\', '/')
    if ($Pgo -eq 'Use') {
        # The instrumented eden.exe writes eden!N.pgc files when it closes; the linker merges the
        # ones next to the .pgd, so gather any left beside the training copy.
        if (-not (Test-Path (Join-Path $fefwPgoDir 'eden.pgd'))) { throw 'No training profile yet: run -Pgo Train first.' }
        if (Test-Path $trainDir) {
            Get-ChildItem -LiteralPath $trainDir -Filter 'eden!*.pgc' -File |
                Move-Item -Destination $fefwPgoDir -Force
        }
        $pgc = @(Get-ChildItem -LiteralPath $fefwPgoDir -Filter 'eden!*.pgc' -File)
        if (-not $pgc) { throw "No eden!*.pgc files in $fefwPgoDir or $trainDir. Play the game in FEFW-PGO-Training\eden.exe and close it normally first." }
        Write-Host "PGO: using $($pgc.Count) training run(s)" -ForegroundColor Green
    } elseif ($Pgo -eq 'Train') {
        # A new training build makes a new .pgd; old runs no longer match it.
        if (Test-Path $fefwPgoDir) { Remove-Item -LiteralPath $fefwPgoDir -Recurse -Force }
        New-Item -ItemType Directory -Path $fefwPgoDir | Out-Null
    }

    if ($Clean -and (Test-Path $buildDir)) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    if ($RenderIsa -eq 'None' -and $Pgo -in 'Train', 'Use') {
        Write-Host "Portable: no CPU-specific code, PGO stage $Pgo" -ForegroundColor Green
        # Whole-program optimisation (/GL) for the emulator's hot libraries, which PGO needs to work
        # on them, plus the profile stage on the eden.exe link.
        $stage = if ($Pgo -eq 'Train') { "/FASTGENPROFILE:NOEXACT,COUNTER64,PGD=$pgd" } else { "/USEPROFILE:PGD=$pgd" }
        # -Symbols: debug information in the objects (/Z7) and a full .pdb. /OPT can't be set
        # here: with /USEPROFILE the linker refuses options the training link didn't have.
        $symCompile = if ($Symbols) { '/Z7' } else { '' }
        $symLink = if ($Symbols) { '/DEBUG:FULL' } else { '' }
        if ($Symbols) { Write-Host "Symbols: on (build folder $buildDir)" -ForegroundColor Green }
        $include = Join-Path $buildDir 'pgo-fefw.cmake'
        New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
        @"
function(eden_fefw_pgo)
    if(MSVC)
        foreach(lib IN ITEMS common core video_core shader_recompiler audio_core hid_core)
            if(TARGET `${lib})
                set_property(TARGET `${lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
                if("$symCompile")
                    target_compile_options(`${lib} PRIVATE $symCompile)
                endif()
            endif()
        endforeach()
        if(TARGET yuzu)
            target_link_options(yuzu PRIVATE /LTCG $stage $symLink)
            if("$symCompile")
                target_compile_options(yuzu PRIVATE $symCompile)
            endif()
        endif()
    endif()
endfunction()
cmake_language(DEFER CALL eden_fefw_pgo)
"@ | Set-Content -LiteralPath $include -Encoding ASCII
    } elseif ($RenderIsa -eq 'None') {
        Write-Host 'Portable: no CPU-specific code, no PGO' -ForegroundColor Green
        $include = $null
    } elseif ($usePgo) {
        Write-Host 'PGO: on (profile matches this compiler)' -ForegroundColor Green
        $include = Join-Path $pgoDir 'link-profile.cmake'   # applies the profile and the renderer ISA
    } else {
        Write-Warning "PGO: off (the profile only matches the kcode source with MSVC $pgoCompiler; this is $Version with $clVersion). AVX-512 renderer files still apply."
        # The repo's include fails without a profile stage, so apply just the ISA part.
        $include = Join-Path $buildDir 'render-isa-only.cmake'
        @'
function(eden_render_isa_options)
    if(MSVC AND TARGET video_core AND EDEN_LOCAL_RENDER_ISA)
        foreach(unit IN ITEMS
                renderer_vulkan/vk_graphics_pipeline.cpp
                renderer_vulkan/vk_texture_cache_base.cpp
                renderer_vulkan/vk_buffer_cache.cpp
                renderer_vulkan/vk_rasterizer.cpp
                renderer_vulkan/vk_compute_pipeline.cpp)
            set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/video_core/${unit}"
                TARGET_DIRECTORY video_core APPEND PROPERTY COMPILE_OPTIONS
                "/arch:${EDEN_LOCAL_RENDER_ISA}" /fp:precise)
        endforeach()
    endif()
endfunction()
cmake_language(DEFER CALL eden_render_isa_options)
'@ | Set-Content -LiteralPath $include -Encoding ASCII
    }

    $cmakeArgs = @('-S', $sourceDir, '-B', $buildDir, '-G', 'Ninja',
                   '-DCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64')
    foreach ($line in Get-Content -LiteralPath (Join-Path $root 'cmake-options.txt')) {
        $entry = $line.Trim()
        if ($entry -notmatch '^([A-Za-z0-9_]+):[A-Za-z0-9_]+=') { continue }
        $key = $Matches[1]
        if ($key -eq 'EDEN_LOCAL_RENDER_ISA') { continue }
        if ($key -eq 'EDEN_LOCAL_PROFILE_STAGE' -and -not $usePgo) { continue }
        if ($key -eq 'YUZU_TESTS') { $entry = 'YUZU_TESTS:BOOL=OFF' }   # not needed for eden.exe
        $cmakeArgs += "-D$entry"
    }
    if ($include) {
        $cmakeArgs += "-DEDEN_LOCAL_RENDER_ISA:STRING=$RenderIsa"
        $cmakeArgs += "-DCMAKE_PROJECT_yuzu_INCLUDE:FILEPATH=$include"
    }

    $code = Invoke-Logged 'cmake' $cmakeArgs
    if ($code) { throw "CMake configure failed ($code). See $toolLog" }

    $code = Invoke-Logged 'cmake' @('--build', $buildDir, '--target', 'yuzu', '--parallel', "$Jobs")
    if ($code) { throw "Build failed ($code). See $toolLog" }

    $exe = Join-Path $buildDir 'bin\eden.exe'
    if (-not (Test-Path $exe)) { throw "Build finished but $exe is missing." }

    if (Test-Path $outDir) { Remove-Item -LiteralPath $outDir -Recurse -Force }
    New-Item -ItemType Directory -Path $outDir | Out-Null
    Copy-Item -Path (Join-Path $buildDir 'bin\*') -Destination $outDir -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $outDir -ErrorAction SilentlyContinue

    # The shareable build is called Fortune's Eden (the training copy keeps eden.exe for Save-PGO.ps1).
    $appExe = if ($Portable -and $Pgo -ne 'Train') { "Fortune's Eden.exe" } else { 'eden.exe' }
    if ($appExe -ne 'eden.exe') { Rename-Item -LiteralPath (Join-Path $outDir 'eden.exe') -NewName $appExe }

    if ($Portable) {
        if (-not $Symbols) { Remove-Item -LiteralPath (Join-Path $outDir 'eden.pdb') -ErrorAction SilentlyContinue }
        # App-local VC++ runtime, so nobody needs to install the redistributable.
        if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is not set; run from a normal PowerShell so the script sets up the VS environment.' }
        $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
               Select-Object -First 1
        if (-not $crt) { throw "VC++ runtime DLLs not found under $env:VCToolsRedistDir" }
        Copy-Item -Path (Join-Path $crt.FullName '*.dll') -Destination $outDir
        # GPL: ship the exact source alongside the binary.
        $srcZip = if ($Version -eq '1.12') { Join-Path $root 'eden-fefw-1.12-local.zip' } else { $null }
        if ($srcZip -and (Test-Path $srcZip)) { Copy-Item -LiteralPath $srcZip -Destination (Join-Path $outDir 'source.zip') }
        # Optional AMD FSR 4: fsr4-vk's Vulkan provider (GPL-3.0, its source is in source.zip under
        # externals\fsr4-vk) and its licences, from the fsr4 folder next to this script.
        $fsr4Dll = Join-Path $root 'fsr4\amd_fidelityfx_upscaler_vk.dll'
        if (Test-Path $fsr4Dll) {
            Copy-Item -LiteralPath $fsr4Dll -Destination $outDir
            $fsr4Licenses = Join-Path $outDir 'licenses\fsr4-vk'
            New-Item -ItemType Directory -Force -Path $fsr4Licenses | Out-Null
            Copy-Item -Path (Join-Path $root 'fsr4\licenses\*') -Destination $fsr4Licenses -Recurse
        } else {
            Write-Warning 'fsr4\amd_fidelityfx_upscaler_vk.dll not found: the package has no AMD FSR 4.'
        }
        # The Fortune's Weave 60 fps cheat from this PC's profile, for others to copy into theirs.
        $fpsMod = Join-Path $env:APPDATA 'eden\load\04007EF01F8AE000\60fps'
        $hasFpsMod = Test-Path $fpsMod
        # Portable profile: a "user" folder next to the exe keeps keys, firmware, settings and
        # saves inside this folder instead of %APPDATA%\eden.
        $userDir = Join-Path $outDir 'user'
        New-Item -ItemType Directory -Force -Path $userDir | Out-Null
        'Eden keeps its settings, saves and shader cache in this folder. Delete it to use %APPDATA%\eden instead.' |
            Set-Content -LiteralPath (Join-Path $userDir 'README.txt') -Encoding ASCII
        # Pre-set settings (cleaned copy of this PC's: no paths, controller IDs or logins). Everything,
        # including the 12 GB memory layout Fortune's Weave needs, is global so it applies whatever
        # title ID the game is loaded under.
        $presetConfig = Join-Path $root 'portable-config'
        if (Test-Path $presetConfig) {
            Copy-Item -Path $presetConfig -Destination (Join-Path $userDir 'config') -Recurse
        } else {
            Write-Warning 'portable-config not found: the package will start with default settings, and Fortune''s Weave needs the 12 GB memory layout.'
        }
        if ($hasFpsMod) {
            $modDest = Join-Path $userDir 'load\04007EF01F8AE000'
            New-Item -ItemType Directory -Force -Path $modDest | Out-Null
            Copy-Item -LiteralPath $fpsMod -Destination $modDest -Recurse
        }
        $fpsText = if ($hasFpsMod) { @"

60 FPS MOD (Fortune's Weave) - included in user\load\04007EF01F8AE000\60fps, but
OFF by default. 30 fps is the game's native framerate on Switch 2 and runs smoothly
everywhere, while 60 fps can feel choppy.
Fast PC? It's fairly smooth on high-end CPUs at 1x resolution (FSR upscales to your
screen); higher resolutions roughly double the choppiness. To try it, right-click the
game > Properties > Add-Ons and tick 60fps.
If it causes problems, untick it; if it still feels choppy, DELETE the folder
user\load\04007EF01F8AE000\60fps (unticking it isn't always enough).
It only works with the game version whose build ID starts DB93B0125F485060;
other versions just ignore it.
"@ } else { '' }
        @"
Fortune's Eden (FEFW Stable with 60 FPS Patch) v1.4.2 - portable Windows x64 build
(Eden FE: Fortune's Weave fork, built on Duck912's eden-ns2-fe and xDarkWindx's eden-fefw $Version)

Runs on any 64-bit Windows 10/11 PC with a Vulkan-capable GPU. No CPU-specific code
and the Microsoft C++ runtime is included, so nothing needs installing.

GETTING STARTED
1. Extract this whole folder somewhere (not inside Program Files) and run Fortune's Eden.exe.
2. Add your game folder: double-click the empty game list and choose the folder.

No games, keys or firmware are included. Use your own copy of the game, dumped from
your own console, as loose files (exefs/romfs) or a single .nup file.
SAVES ARE STORED HERE, next to Fortune's Eden.exe (user\nand), NOT in %APPDATA%\eden.
Loose files and .nup now share the same saves. Saves made with loose files in older versions
are copied across automatically the first time.
Everything (settings, saves, shader cache) stays in the user\ folder here, so this doesn't
touch any other Eden install. To move it, move the whole folder. Deleting it deletes your saves.

SETTINGS are already set up globally (Emulation > Configure), so they apply however the game
is dumped or packed: the 12 GB memory layout the game needs, GPU accuracy Fast, asynchronous
shaders on, vsync off, 16x anisotropic filtering, VRAM Usage Mode Conservative (avoids
long-session freezes), AMD FSR window filter at 0% sharpness and FXAA anti-aliasing.
Only controls need setting: Emulation > Configure > Controls.

NVIDIA DLSS (optional, GeForce RTX cards): Emulation > Configure > Graphics, tick
"Use NVIDIA DLSS in place of the game's DLSS", then click "Download from NVIDIA" in the panel
below it (NVIDIA's files aren't included). Takes effect when the game starts. It replaces the
game's own lightweight DLSS with the full PC version: sharper, with less ghosting.
$fpsText

Changes from upstream:
- Optional NVIDIA DLSS in place of the game's own DLSS (from Duck912's work in Eglantine).
- Bath scenes no longer lose their background: a compute shader using a shared-memory
  compare-and-swap (ATOMS.CAS) is now translated instead of skipped.
- Cutscenes keep up: AV1 videos are decoded several frames at once, so the busiest 1440p
  cutscenes no longer drop the game to about 5 FPS.
- A user folder copied from another build uses its own saves and folders here, not the old
  build's (and Open Save Data Location opens the one next to this exe).
- Smoother 60 FPS in busy scenes: texture-handle tables the game builds on the GPU and never
  changes are no longer waited for once a frame, and CPU writes to GPU memory take the cache
  lock far less often. In Dagsion's busiest section at 60 FPS, doubled frames went from about
  45% to about 20%.
- Fewer shader-compile stutters: translated shaders are reused between pipelines that share
  them (Wael3rd, from PokeDinner).
- Loose-file dumps use the game's real ID, so their saves sit with .nup saves.
- Shutdown and restart fixes: no hang when the audio renderer outlives its manager
  (kraibse), and a second game in the same window no longer hangs (Takoroka); both from
  PokeDinner (an Eglantine fork). Clearer logs for missing keys and unsupported commands
  (xDarkWindx).
- Loads .nup game files (NUP loader ported from Djihads80's Eglantine, GPL-3.0).
- AMD fix: shader registers read before being written are zero, as on NVIDIA, instead of
  garbage (stray coloured patches on AMD GPUs). From Eglantine (arisqz), GPL-3.0.
- The start-up check that clears empty save profile folders no longer crashes when Windows
  refuses the delete, and never deletes a folder it could not read.

Licence: GPL-3.0-or-later (see LICENSE). The full source for this build is in source.zip.
Built with MSVC $clVersion on $(Get-Date -Format 'yyyy-MM-dd')$(if ($Pgo -eq 'Use') { ', with profile-guided optimisation trained on Fortune''s Weave' }).
"@ | Set-Content -LiteralPath (Join-Path $outDir 'README.txt') -Encoding ASCII
    }
    if ($Pgo -eq 'Train') {
        # The measuring eden.exe needs the PGO runtime beside it.
        $pgort = Get-ChildItem -Path (Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64') -Filter 'pgort*.dll' -File |
                 Select-Object -First 1
        if (-not $pgort) { throw "pgort*.dll not found under $env:VCToolsInstallDir" }
        Copy-Item -LiteralPath $pgort.FullName -Destination $outDir
        Write-Host "`nTRAINING BUILD READY: $outDir\eden.exe" -ForegroundColor Green
        Write-Host @"
Next:
 1. Optional: copy your saves in (your play folder's user\nand -> $outDir\user\nand).
 2. Run $outDir\eden.exe and play Fortune's Weave for 10-20 minutes: busy towns,
    battles, menus, a cutscene. It runs a little slower while measuring; that's expected.
 3. Close the emulator normally (not from Task Manager), so it saves the measurements.
 4. Run: .\Build-Local-MSVC.ps1 -Version 1.12 -Portable -Pgo Use
"@ -ForegroundColor Yellow
        return
    }
    if ($Portable) {
        $pkg = "$outDir.zip"
        if (Test-Path $pkg) { Remove-Item -LiteralPath $pkg -Force }
        # Zip with forward-slash entry names (Compress-Archive on Windows PowerShell writes
        # backslashes, which 7-Zip and non-Windows tools extract as flat file names).
        Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::Open($pkg, [IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem -LiteralPath $outDir -Recurse -File | ForEach-Object {
                $entry = $_.FullName.Substring($outDir.Length + 1).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $archive, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
        Write-Host "Package: $pkg" -ForegroundColor Green
    }

    Write-Host "`nBUILD SUCCEEDED: $outDir\$appExe" -ForegroundColor Green
    Write-Host "MSVC $clVersion | PGO $(if ($usePgo) {'on'} else {'off'}) | renderer $RenderIsa"
}
catch {
    Write-Host "`nBUILD FAILED: $_" -ForegroundColor Red
    throw
}
finally {
    Stop-Transcript | Out-Null
}
