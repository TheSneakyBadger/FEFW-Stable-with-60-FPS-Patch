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
    [switch]$Clean,
    [string]$TestSave = 'D:\save',         # test save copied into test builds (never into -Pgo Use)
    [string]$TestName = '',                 # with -Portable (no PGO): build into FEFW-Test-<name> instead
                                            # of the release folder, ready to run (test save, keys, stats launcher)
    [string]$TestKeys = (Join-Path $env:USERPROFILE 'Documents\FEFW v1.1\user\keys')
)
if ($Pgo -in 'Train', 'Use' -and -not $Portable) { throw '-Pgo Train/Use are for the portable build: add -Portable.' }
if ($Portable -and $Pgo -notin 'Train', 'Use') {
    # Releases are PGO builds (with whole-program optimisation): a plain portable build is about
    # 10-20% slower on the CPU side. v1.6.2 shipped like this by mistake.
    Write-Warning 'Portable build WITHOUT PGO: fine for testing, but releases should use -Pgo Train, then -Pgo Use.'
}
if ($Portable) { $RenderIsa = 'None'; if ($Pgo -notin 'Train', 'Use') { $Pgo = 'Off' } }

$ErrorActionPreference = 'Stop'
$root      = $PSScriptRoot

# Copies the standard test save into a test build's portable user folder: the save tree (its
# 0000000000000000 folder) into user\nand\user\save, and, if the test save has one, profiles.dat
# into the profile store, so the save's profile is a real one there (no "orphaned profile" warning).
# Runs after any zip is made, so packages never contain it.
function Add-TestSave([string]$BuildDir) {
    $saveRoot = Join-Path $TestSave '0000000000000000'
    if (-not (Test-Path $saveRoot)) { Write-Warning "No test save at $saveRoot; the test build starts without one."; return }
    $nand = Join-Path $BuildDir 'user\nand'
    $saveDest = Join-Path $nand 'user\save'
    New-Item -ItemType Directory -Force -Path $saveDest | Out-Null
    Copy-Item -LiteralPath $saveRoot -Destination $saveDest -Recurse -Force
    $profiles = Join-Path $TestSave 'profiles.dat'
    if (Test-Path $profiles) {
        $avatars = Join-Path $nand 'system\save\8000000000000010\su\avators'
        New-Item -ItemType Directory -Force -Path $avatars | Out-Null
        Copy-Item -LiteralPath $profiles -Destination $avatars -Force
        Write-Host "Test save copied in from $TestSave (with its profile)." -ForegroundColor Green
    } else {
        Write-Warning "Test save copied in from $TestSave, but $profiles is missing: Eden will make a new profile and report the save as orphaned."
    }
}

# A named test build is made ready to run: this PC's keys (never in a package: the zip is made
# first) and a launcher with the per-frame timing statistics on.
function Add-TestRunner([string]$BuildDir) {
    if (Test-Path $TestKeys) {
        Copy-Item -LiteralPath $TestKeys -Destination (Join-Path $BuildDir 'user') -Recurse -Force
    } else {
        Write-Warning "No keys at ${TestKeys}: copy your keys into $BuildDir\user\keys before running it."
    }
    $exeName = Get-ChildItem -LiteralPath $BuildDir -Filter '*.exe' -File |
               Where-Object { $_.Name -like "Fortune*" -or $_.Name -eq 'eden.exe' } | Select-Object -First 1
    @(
        '@echo off'
        'cd /d "%~dp0"'
        'set EDEN_BACK_THREAD_STATS=1'
        'set EDEN_TEXTURE_TABLE_STATS=1'
        "start `"`" `"$($exeName.Name)`""
    ) | Set-Content -LiteralPath (Join-Path $BuildDir 'Start with full stats.bat') -Encoding ASCII
}
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
    if ($TestName) {
        if ($Pgo -in 'Train', 'Use') { throw '-TestName is for test builds, not -Pgo Train/Use.' }
        $outDir = Join-Path $root "FEFW-Test-$TestName"
    }
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
            # Give each one the next free number: Save-PGO.ps1 numbers its own files in pgo-fefw,
            # so the same name can mean a different run (overwriting it would lose that run).
            foreach ($file in @(Get-ChildItem -LiteralPath $trainDir -Filter 'eden!*.pgc' -File)) {
                $n = 1; while (Test-Path (Join-Path $fefwPgoDir "eden!$n.pgc")) { $n++ }
                Move-Item -LiteralPath $file.FullName -Destination (Join-Path $fefwPgoDir "eden!$n.pgc")
            }
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

    # Bundled packages (Qt, OpenSSL, SDL3, FFmpeg...) are kept in cpm-cache next to this script,
    # not inside the source folder, so extracting a new source zip doesn't download them again.
    $cpmCache = Join-Path $root 'cpm-cache'
    $cmakeArgs = @('-S', $sourceDir, '-B', $buildDir, '-G', 'Ninja',
                   '-DCMAKE_EXE_LINKER_FLAGS:STRING=/machine:x64',
                   "-DCPM_SOURCE_CACHE:PATH=$cpmCache")
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

    # CMake downloads checksums for its bundled packages from GitHub on every configure (and the
    # build reruns configure when CMake files change). Those downloads fail now and then; retry
    # a failed step up to 4 times when that is the reason, instead of failing the whole build.
    function Invoke-LoggedRetry([string]$Exe, [string[]]$Arguments) {
        for ($attempt = 1; ; $attempt++) {
            $before = 0
            if (Test-Path $toolLog) { $before = "$(Get-Content -LiteralPath $toolLog -Raw)".Length }
            $result = Invoke-Logged $Exe $Arguments
            if (-not $result -or $attempt -ge 4) { return $result }
            $text = "$(Get-Content -LiteralPath $toolLog -Raw)"
            $tail = if ($text.Length -gt $before) { $text.Substring($before) } else { $text }
            if ($tail -notmatch 'Failed to download') { return $result }
            Write-Warning "A GitHub download failed (attempt $attempt of 4); retrying in 20 seconds..."
            Start-Sleep -Seconds 20
        }
    }

    $code = Invoke-LoggedRetry 'cmake' $cmakeArgs
    if ($code) { throw "CMake configure failed ($code). See $toolLog" }

    $code = Invoke-LoggedRetry 'cmake' @('--build', $buildDir, '--target', 'yuzu', '--parallel', "$Jobs")
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
        # Optional AMD FSR 4: AMD's signed FSR 4.1.2 Vulkan upscaler library (FSR SDK 2.4), from the
        # fsr4 folder next to this script. It is AMD's binary, under the SDK's licence (the same
        # MIT licence as licenses\amd-fidelityfx-sdk); only an AMD-signed copy is packaged.
        $fsr4Dll = Join-Path $root 'fsr4\amd_fidelityfx_upscaler_vk.dll'
        if (Test-Path $fsr4Dll) {
            $signature = Get-AuthenticodeSignature -LiteralPath $fsr4Dll
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Advanced Micro Devices') {
                throw "fsr4\amd_fidelityfx_upscaler_vk.dll is not validly signed by AMD ($($signature.Status)); not packaging it."
            }
            Copy-Item -LiteralPath $fsr4Dll -Destination $outDir
            $fsr4Notice = Join-Path $outDir 'licenses\amd-fsr4'
            New-Item -ItemType Directory -Force -Path $fsr4Notice | Out-Null
            $fsr4Version = (Get-Item -LiteralPath $fsr4Dll).VersionInfo.FileVersion
            @(
                'amd_fidelityfx_upscaler_vk.dll: AMD FidelityFX (Vulkan) Upscaler Library'
                "Version $fsr4Version, from AMD's FSR SDK 2.4 (FSR Upscaling 4.1.2), signed by Advanced Micro Devices."
                'Copyright (C) Advanced Micro Devices, Inc. Redistributed unmodified under the AMD FidelityFX SDK'
                'licence (MIT); see ..\amd-fidelityfx-sdk\LICENSE.txt. https://gpuopen.com/fidelityfx-sdk/'
            ) | Set-Content -LiteralPath (Join-Path $fsr4Notice 'NOTICE.txt') -Encoding ASCII
        } else {
            Write-Warning 'fsr4\amd_fidelityfx_upscaler_vk.dll not found: the package has no AMD FSR 4.'
        }
        # 60 FPS mode is built in since v1.6.7.4 (Settings > Graphics): no mod is packaged.
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
        # Kompas9's ultrawide patch is built into the exe (setting fefw_ultrawide): its MIT notice.
        $uwLicense = Join-Path $root 'licenses\ultrawide-kompas9'
        if (Test-Path $uwLicense) {
            $uwDest = Join-Path $outDir 'licenses\ultrawide-kompas9'
            New-Item -ItemType Directory -Force -Path $uwDest | Out-Null
            Copy-Item -Path (Join-Path $uwLicense '*') -Destination $uwDest
        } else {
            Write-Warning 'licenses\ultrawide-kompas9 not found: the package will lack the ultrawide patch notice.'
        }
        # dav1d decodes the AV1 cutscenes: its BSD 2-clause notice.
        $dav1dLicense = Join-Path $sourceDir 'externals\dav1d\COPYING'
        if (Test-Path $dav1dLicense) {
            $dav1dDest = Join-Path $outDir 'licenses\dav1d'
            New-Item -ItemType Directory -Force -Path $dav1dDest | Out-Null
            Copy-Item -LiteralPath $dav1dLicense -Destination (Join-Path $dav1dDest 'COPYING.txt')
        }
        # AMD FSR 3.1 (option amd_fsr3) is built from AMD's FidelityFX SDK shaders: its MIT notice.
        $ffxLicense = Join-Path $sourceDir 'externals\FidelityFX-SDK\LICENSE.txt'
        if (Test-Path $ffxLicense) {
            $ffxDest = Join-Path $outDir 'licenses\amd-fidelityfx-sdk'
            New-Item -ItemType Directory -Force -Path $ffxDest | Out-Null
            Copy-Item -LiteralPath $ffxLicense -Destination $ffxDest
        } else {
            Write-Warning 'externals\FidelityFX-SDK\LICENSE.txt not found: the package will lack the FSR 3.1 notice.'
        }
        $fpsText = @"

60 FPS MODE AND PERFORMANCE (Fortune's Weave 1.1.0)
- 60 FPS mode (Emulation > Configure > Graphics, off by default): the game runs at up to
  60 FPS instead of its native 30. Game speed follows your real frame rate, so it stays
  correct anywhere between 30 and 60 FPS (no slow motion when your PC can't hold 60). Capes
  and hair are simulated every frame.
  Takes effect when the game starts. Made for game version 1.1.0 (build ID DB93B0125F485060);
  on other versions it turns on when every place it changes is found. It works alongside mods,
  even those that use the free space after the game's code its routines usually take; with a
  mod that changes the same game code it stays off. The older "60fps" add-on folder becomes the
  setting.
- Emulation > Configure > Performance: Reflections (Full / Reduced / Off), Turn ambient
  occlusion off, Reduced crowd detail and Reduced scenery detail: lighter versions of the
  game's own effects for PCs that can't hold the frame rate (each tick makes the game
  lighter). Nothing on disk is changed. Take effect when the game starts.
"@
        $optionalText = @"

ULTRAWIDE 3440x1440 (optional, OFF by default) - for 21:9 monitors: Emulation > Configure >
Graphics, tick "Ultrawide 3440x1440 (21:9) for Fortune's Weave", then start the game and play
fullscreen. It widens the 3D camera and keeps the HUD, menus and movies centred at 16:9
(movies get black bars). Kompas9's patch (MIT, licenses\ultrawide-kompas9); only for game
version 1.1.0 (build ID DB93B0125F485060).
"@
        @"
Fortune's Eden (FEFW Stable with 60 FPS Patch) v1.6.8.4 - portable Windows x64 build
(Eden FE: Fortune's Weave fork, built on Duck912's eden-ns2-fe and xDarkWindx's eden-fefw $Version)

Runs on any 64-bit Windows 10/11 PC with a Vulkan-capable GPU. No CPU-specific code
and the Microsoft C++ runtime is included, so nothing needs installing.

GETTING STARTED
1. Extract this whole folder somewhere (not inside Program Files) and run Fortune's Eden.exe.
2. Add your game folder: double-click the empty game list and choose the folder.

LEGAL USE ONLY. Piracy is not acceptable and is not supported in any way. No games, keys,
firmware or other Nintendo files are included. Use your own legally obtained copy of the game, dumped from your own
console, as loose files (exefs/romfs) or a single .nup file, with keys and firmware dumped
from that same console. Not affiliated with, endorsed by or sponsored by Nintendo, NVIDIA or AMD.
SAVES ARE STORED HERE, next to Fortune's Eden.exe (user\nand), NOT in %APPDATA%\eden.
Loose files and .nup now share the same saves. Saves made with loose files in older versions
are copied across automatically the first time.
Everything (settings, saves, shader cache) stays in the user\ folder here, so this doesn't
touch any other Eden install. To move it, move the whole folder. Deleting it deletes your saves.

SETTINGS are already set up (Emulation > Configure), so they apply however the game is dumped
or packed: the 12 GB memory layout the game needs, GPU mode Fast and ASTC recompression Auto
are always used; asynchronous shaders on, vsync Mailbox, 16x anisotropic filtering and VRAM
Usage Mode Conservative are the defaults. Settings has seven pages (General, System, Graphics,
Performance, Audio, Controls, Advanced); Game Settings only has the game's add-ons and input profiles.
Pick the resolution so the output matches your screen (1.5X for 4K).
Only controls need setting: Emulation > Configure > Controls.
The launch screen has the main ones beside Play: 60 FPS, Ultrawide, HDR (with paper white),
Upscaler, Upscaler quality, Frame generation, and a button to the Performance page.

Upscaler: Emulation > Configure > Graphics > Upscaler. Takes effect when the game starts.
- AMD FSR 3.1 (default, any card): replaces the game's own DLSS network, which takes work off
  the emulator. Its sharpening follows the FSR Sharpness slider (25%).
- Game's own DLSS: the game's own small network, run as compute passes. Some prefer its look.
- NVIDIA DLSS (GeForce RTX): the best picture by far. Choose it, then click "Download from
  NVIDIA" in the panel below the list (NVIDIA's files aren't included).
- AMD FSR 4 (AMD Radeon RX 6000/7000/9000): AMD's own FSR 4.1.2 Vulkan library from the FSR SDK
  2.4 (amd_fidelityfx_upscaler_vk.dll, signed by AMD; notice in licenses\amd-fsr4). RX 9000 cards
  run the full model, RX 7000 and RX 6000 cards an INT8 model. It needs an AMD card.
NVIDIA DLSS and FSR 4 fall back to FSR 3.1 when they can't start.

Upscaler quality (Graphics): how big the 3D scene is drawn before it's upscaled to 1440p.
Native (DLAA with NVIDIA DLSS) is the sharpest and heaviest; Ultra Quality, Quality and
Balanced (the default) are in between; Performance is the game's own 720p and the lightest.
Menus and text always stay at full size. Works at 1X resolution with FSR 3.1, NVIDIA DLSS or
FSR 4; on a laptop or older card, Performance may run better.

HDR (optional): turn on HDR in Windows, tick "HDR output (HDR10)" in Graphics and start the
game; its Brightness and UI Brightness options then work. Restart the game after changing it.

Frame generation (experimental, off by default): Graphics > Frame generation, or the launch
screen. Makes a frame between each two of the game's, so 60 FPS shows as 120, in the 3D scenes.
Needs a display faster than the game's frame rate (120 Hz or more for 60 FPS).
- AMD FSR 3.1: any card (AMD's FidelityFX SDK, MIT).
- NVIDIA DLSS: GeForce RTX 40/50 and hardware-accelerated GPU scheduling on in Windows. Choose
  it, then click "Download from NVIDIA" in the panel below the setting (NVIDIA's files aren't
  included). Falls back to AMD's.
The status bar names the one running, next to the upscaler.
$fpsText$optionalText

New in v1.6.8.4:
- Pre-rendered cutscenes decode 4 frames at once again, so the busiest 1440p scenes (fire,
  fast camera moves) keep up with the voices. On PCs with many cores, the video decoder's
  threads sometimes all went to sleep with work left and the movie froze (with the game
  stuck when closed); Fortune's Eden now wakes them, so the movie carries on.

New in v1.6.8.3:
- Fixed a crash in long pre-rendered cutscenes with frame generation on (from v1.6.8.2): the
  emulator closed to the desktop about 30 seconds into the video.
- 60 FPS mode now works with mods that fill the free space after the game's code (settings
  mods, quality-of-life mods): its routines go in memory Fortune's Eden adds after the game's
  code instead of turning 60 FPS mode off.

New in v1.6.8.2:
- Video memory is cleaned up at loading screens: what the previous area used is freed when the
  next one has loaded, so memory drops at each area change instead of climbing all session.
- Earlier scenes' render targets are freed after 30 seconds unused, and small leftover images
  after 5 minutes (they were kept for good and grew by gigabytes over long sessions).
- Less waiting on the emulator in busy scenes: the game's writes to graphics memory no longer
  wait for the thread that draws (the small freezes when moving units on some CPUs, #41).
- Performance page: "Turn ambient occlusion off" (ambient occlusion stays on unless ticked;
  an earlier "off" is kept), so every tick there makes the game lighter. 60 FPS mode moves to
  the Graphics page.
- F10 no longer switches between docked and handheld (easy to hit by accident; handheld drops
  the game to 1080p). The menu item and the controller shortcut, Home+X, stay.

New in v1.6.8.1:
- Frame generation is back in Settings > Graphics (it only showed on the launch screen), with
  the "Download from NVIDIA" button for NVIDIA's frame generation files.
- AMD frame generation with HDR on: the UI no longer ghosts when the camera moves.
- NVIDIA DLSS frame generation works when a game is started again: after it has run, Play
  reopens Fortune's Eden into the game (NVIDIA's software can't be set up twice in one run).

New in v1.6.8:
- Frame generation: AMD FSR 3.1 (any card) or NVIDIA DLSS (RTX 40/50). The game's UI is kept
  still, so it doesn't ghost when the camera moves.
- The main settings are on the launch screen, beside Play.
- Video memory no longer creeps up over long sessions (drops to 10 FPS, red/green garbage or
  a crash after an hour or two, mostly on 8 GB cards): images the GPU drew into that can't be
  written back are freed when unused. NVIDIA DLSS stays on when memory is over budget.
- 60 FPS mode works with mods (it goes only where no mod writes, and turns itself off if a mod
  changes the same code), and on other game versions when all its places are found.
- About 30% less game CPU time in busy scenes; cutscene video decoding on its own thread;
  Reduced scenery detail (Performance) and Audio buffer (Audio) settings; HDR paper white;
  rumble on its own thread; the updater resumes dropped downloads.

New in v1.6.7.4:
- 60 FPS mode is built in (Settings > Performance). Game speed follows the real frame rate
  from 30 to 60 FPS, so a PC that drops below 60 no longer plays in slow motion. About 50
  places in the game that counted per frame were traced and fixed (map cursor and markers,
  menus, physics, cutscene shake, character creation and more); capes and hair are
  simulated every frame with physics scaled to the frame time.
- New Performance page: Reflections, Ambient occlusion and Reduced crowd detail.
- Video memory creeps up far less over long sessions. Freed memory is returned to the
  driver (large textures get their own allocation, smaller blocks for the rest), cleanup goes
  by the memory actually in use, unused buffers are freed, and busy scenes are cleaned up
  straight away instead of filling to the limit. This should mean fewer stutters in long
  sessions and on 8 GB cards.
- The Play button on the launch screen fits its text.

New in v1.6.7.3:
- Mouse mode: the PC mouse drives the game's Joy-Con mouse cursor (Ctrl + Enter, or the
  button at the bottom of the window; right-click it for sensitivity).
- Cutscenes decode much faster (dav1d), busy scenes such as the Saramis Guesthouse run
  faster, and NVIDIA DLSS tries again after a failure instead of giving up.

New in v1.6.7.2:
- Players who updated from v1.6.2 or earlier and were still on the game's own DLSS are
  moved to AMD FSR 3.1 (the default) once. Choosing the game's own DLSS again is kept.

New in v1.6.7.1:
- Updated installs get the tuned settings too (vsync Mailbox, asynchronous shaders on,
  16x anisotropic filtering, reactive flushing off, FSR sharpening 150%).
- Text fits in buttons and lists at Windows display scaling.

New in v1.6.7:
- A new look: Fortune's Eden theme, app icon, and a launch screen with Play, Game Settings,
  Settings and Check for Updates.
- A much shorter Settings window (six pages); Game Settings has only add-ons and input
  profiles, so the settings in Settings always apply.
- Add-ons (the 60fps mod) can be changed while the game runs, applied at the next start.

New in v1.6.6.2:
- The Saramis hub (war arc) no longer drops to 15-20 FPS on mid-range PCs: a safety check
  waited for the GPU several times a second there.

New in v1.6.6.1:
- Fixed a broken picture with NVIDIA DLSS (and FSR) in some cutscenes: the upscaler was given
  the game's motion data in the wrong format.

New in v1.6.6:
- AMD FSR 4 now really starts on Radeon RX 6000/7000/9000 cards (v1.6.4 and v1.6.5 still fell
  back to FSR 3.1).
- The status bar names the upscaler's mode, e.g. "NVIDIA DLSS - DLAA" or "AMD FSR 3.1 -
  Balanced". Upscaler quality is greyed out with the game's own DLSS, and a note says when the
  resolution means it won't apply.

New in v1.6.5:
- Upscaler quality (Graphics): Native, Ultra Quality, Quality, Balanced (the new default) or
  Performance. The 3D scene can now be drawn bigger than the game's own 720p while the menus
  and text stay at 1440p.

New in v1.6.4:
- Busy scenes run faster: each draw is prepared about 40% faster where the game draws a lot
  (the Saramis hub: 53-55 FPS before, a steady 60 with the 60 FPS mod on the test PC).
- Automatic updates: on start it checks GitHub for a newer version and asks first. Updating
  downloads it, checks it against GitHub's checksum, then restarts into the new version.
  Saves, settings, keys and mods are never touched. (Settings: "Check for updates".)
- AMD FSR 4 now starts on AMD Radeon RX 6000/7000/9000 cards (it fell back to FSR 3.1 before).

New in v1.6.3:
- A PGO build again: v1.6.2's download was built without it and ran slower on the CPU side.
- Video memory cleanup starts earlier on big cards, so long sessions stay well below the limit.
- AMD FSR 3.1 is the default upscaler, and the upscaler is one dropdown (Graphics > Upscaler).
  Updating keeps your choice.

New in v1.6.2:
- HDR output (HDR10), and optional AMD FSR 3.1 and AMD FSR 4 in place of the game's DLSS.
- Fixes from a full code review: crashes (memory corruption, GPU map and texture lifetime
  races, crash or hang after a slow stop), old saves copied across reliably, sound clicks at
  loops and battle stutter on 4-core CPUs, alpha-tested edges, missing bath-scene effects.
- 8 GB cards: far fewer texture cleanup passes, and one GPU wait per pass instead of up to 40.
- Less CPU work per frame, smaller shader-build hitches, cheats no longer pause every core.
- New defaults: Bilinear filter, anti-aliasing off, vsync Mailbox, FSR sharpness 25%.

New in v1.6.1:
- Fixes 8 GB cards dropping to about 2 FPS for minutes when a scene loads many textures at
  once: the GPU texture conversion's temporary images now have a memory limit.
- UI smearing at the screen edges while moving comes from frame generation (NVIDIA Smooth
  Motion or Lossless Scaling), not the emulator: turn it off for this program.
- The resolution options show this game's real sizes: 1X is 1440p, 1.5X 4K, 2X 5K.

New in v1.6:
- ASTC textures are re-compressed to BC3 on the GPU (from Eglantine, Duck912), so battle starts
  no longer freeze while textures convert, and Auto picks BC3 on every card (about half the video memory).
- Smoother play when video memory fills up: ASTC textures unused for 30 s are freed early.

New in v1.5.2:
- Fixes a crash inside the NVIDIA driver (nvoglv64.dll) a few minutes into some sessions with
  NVIDIA DLSS on: drawing DLSS's output could start with a framebuffer the texture cache had
  just removed. From Eglantine (Duck912).

New in v1.5.1:
- Game updates: the main menu shows the update's version (1.1.0, not 1.0.0), also for .nup
  files that don't record one; loose dumps can carry an update in an updates\<number>\ folder.
- ASTC recompression Auto (see above), and big cards may use three quarters of their video
  memory instead of about 7 GB.
- Laptops and 4-core CPUs: sound no longer stutters in battles (audio threads at top priority;
  the GPU thread split turns itself off with 4 or fewer fast cores).
- Threads wait properly on newer CPUs (from Eglantine); fewer shader hitches during fades;
  crash fixes for window resizing, failed shaders, NVIDIA depth copies and stopping the game.
- Cutscene video keeps up with the voices on slower PCs.
- Logs show frame pacing every 10 s and video memory use every minute.

Changes from upstream:
- Optional NVIDIA DLSS in place of the game's own DLSS (from Duck912's work in Eglantine).
- The sky no longer turns black after opening the map: the texture cache threw away the game's
  sky colour table while the map covered the screen, without keeping what the GPU had drawn.
  Small images the GPU drew are now always kept.
- No more recurring stutter from an experiment left on by mistake in v1.3.1 to v1.4.2 (held
  CPU writes reached the GPU a frame late, so frames came out unevenly).
- CPU Clocks (Boost/Overclock) is gone: it ran the game's clock 1.7-2x fast, so cutscene voices
  dropped out and dialogue animations ended early.
- Slightly smoother busy scenes: the GPU back thread answers most "has the GPU written here?"
  checks from a page bitmap instead of searching a tree (same answers).
- Much smaller logs: no line per frame, one line per unmapped memory read, so long sessions'
  logs stay small enough to send.
- Optional ultrawide 3440x1440 (21:9) setting, off by default: Kompas9's patch (MIT), built in.
- Bath scenes no longer lose their background: a compute shader using a shared-memory
  compare-and-swap (ATOMS.CAS) is now translated instead of skipped.
- Cutscenes keep up: AV1 videos are decoded several frames at once, so the busiest 1440p
  cutscenes no longer drop the game to about 5 FPS.
- A user folder copied from another build uses its own saves and folders here, not the old
  build's (and Open Save Data Location opens the one next to this exe).
- Smoother 60 FPS in busy scenes: texture-handle tables the game builds on the GPU and never
  changes are no longer waited for once a frame.
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
        Add-TestSave $outDir
        Write-Host "`nTRAINING BUILD READY: $outDir\eden.exe" -ForegroundColor Green
        Write-Host @"
Next:
 1. The test save from $TestSave is already in (if it exists).
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
        # Test builds get the test save in their folder (after zipping, so the zip has none).
        if ($Pgo -ne 'Use') { Add-TestSave $outDir }
        if ($TestName) { Add-TestRunner $outDir }
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
