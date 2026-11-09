# fsr4-vk in Fortune's Eden

**Correction (v1.6.4):** the `amd_fidelityfx_upscaler_vk.dll` shipped with Fortune's Eden v1.6.2
and later is not built from this source. It is AMD's own signed FidelityFX (Vulkan) Upscaler
Library (FSR Upscaling 4.1.2, FSR SDK 2.4), redistributed unmodified under the FidelityFX SDK
licence; see `licenses/amd-fsr4` in the package. The emulator only compiles in the FidelityFX API
headers kept here in `third_party/fidelityfx-api` (MIT, AMD) to call it. The rest of this folder
is fsr4-vk's source as it was vendored, kept for reference.

Fortune's Eden ships `amd_fidelityfx_upscaler_vk.dll`, built from
[fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) (GPL-3.0-only, see `LICENSE`) at commit
`b0125ea` ("add more windows fp8 performance patches"). The emulator loads it at run time for
its optional "Use AMD FSR 4" setting; nothing of it is linked into the emulator, which only
compiles in the FidelityFX API headers in `third_party/fidelityfx-api` (MIT, AMD).

This folder holds fsr4-vk's own source at that commit. Its large third-party submodules are
unmodified upstream projects, pinned at:

| Path | Repository | Commit |
| --- | --- | --- |
| third_party/SPIRV-Headers | https://github.com/KhronosGroup/SPIRV-Headers | 01e0577914a75a2569c846778c2f93aa8e6feddd |
| third_party/SPIRV-Tools | https://github.com/KhronosGroup/SPIRV-Tools | 7f2d9ee926f98fc77a3ed1e1e0f113b8c9c49458 |
| third_party/Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | e3b1eec08173d6b825cd3ac88c885a63b621504a |
| third_party/Vulkan-Loader | https://github.com/KhronosGroup/Vulkan-Loader | 5f157b62e333c63260d05d81bf66faa216ab0fb8 |
| third_party/dxil-spirv | https://github.com/HansKristian-Work/dxil-spirv | ec8c7961e59b74d70a6e285da1a4d71dc04c8d04 |
| third_party/glslang | https://github.com/KhronosGroup/glslang | 168d452a4f460d24b588fed08477a81c44ee27a1 |

The FSR 4 models and shaders are extracted from AMD's `amd_fidelityfx_upscaler_dx12.dll`
(FidelityFX SDK 2.3.0, version 4.1.1.2740, SHA-256
`d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`), which AMD's SDK licence
lists under the MIT licence (`LICENSES/AMD-FidelityFX-SDK-license.md`). Get it from
https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.3.0, then follow
`README.md` (tools/setup.py, then tools/build/windows.ps1).

fsr4-vk is independent of AMD and not endorsed by AMD. AMD, FidelityFX and FSR are trademarks
of Advanced Micro Devices, Inc.
