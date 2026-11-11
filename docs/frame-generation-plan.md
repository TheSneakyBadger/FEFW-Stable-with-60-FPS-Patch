# Frame generation (FSR 3 frame interpolation) — plan

Goal for 1.6.8: an optional setting that presents a generated frame between each pair of real
frames, from AMD FidelityFX SDK **v1.1.4** (`c6efa6bf`, the version `externals/FidelityFX-SDK`
already vendors for the FSR 3.1 upscaler), ported the same way: the SDK's Vulkan GLSL pass files
compiled by `src/video_core/host_shaders`, the host side rewritten as our own compute passes
(`vk_fsr3_upscaler.cpp` is the model: `Pass` with a descriptor-kind list, `Resource`, one
uniform staging block per dispatch, everything in `GENERAL`). No SDK backend, no swapchain proxy.

## Inputs we have

- **Motion vectors and depth**: the game's DLSS evaluation (`TemporalUpscale`, the "DLSS
  replacement inputs" log line): render size (720p in the hub), RG16F motion, reversed depth.
  Only present where the game runs its own DLSS pass (the 3D scenes); menus and movies have none,
  and there generation falls back to optical flow only or is skipped.
- **Colour**: the frame the presenter shows (display size, after the upscaler, **with the HUD**).
  No HUD-less copy: `HUDLessAttachedFactor = 0`; the SDK then treats UI through optical flow.
- **Camera**: none from DLSS. As in the upscaler port: near 0.1, vertical FOV 60°, reversed
  infinite depth (`setupDeviceDepthToViewSpaceDepthParams`, inverted + infinite).

## Passes

Optical flow context (`sdk/src/components/opticalflow/ffx_opticalflow.cpp`, shaders
`sdk/src/backends/vk/shaders/opticalflow/`), bindings in order (`T` sampled, `I` storage image,
`B` storage buffer, `U` uniform):

| Pass | Bindings |
|---|---|
| prepare_luma | SRV input colour 0, UAV luma 1, CB common 2 |
| compute_luminance_pyramid | UAV luma level 0..6 (0-6), CB common 7, CB SPD 8 |
| generate_scd_histogram | SRV luma 0, UAV histogram 1, CB 2 |
| compute_scd_divergence | UAV histogram 0, previous histogram 1, temp 2, SCD output 3, CB 4 |
| compute_optical_flow_advanced_v5 | SRV luma 0, previous luma 1, UAV flow 2, SCD output 3, CB 4 |
| filter_optical_flow_v5 | SRV previous flow 0, UAV flow 1, CB 2 |
| scale_optical_flow_advanced_v5 | SRV luma 0, previous luma 1, flow 2, UAV next level 3, SCD 4, CB 5 |

Frame interpolation (`ffx_frameinterpolation.cpp` lines 899-1210), in dispatch order:

1. **reconstruct_and_dilate** (prepare): SRV MV 0, depth 1; UAV reconstructed previous depth 2,
   dilated MV 3, dilated depth 4; CB 5. Clear reconstructed previous depth first (0 for inverted).
2. **setup**: SRV SCD 0; UAV game MV field X 1, Y 2, OF MV field X 3, Y 4, disocclusion mask 5,
   counters 6 (buffer); CB 7.
3. Unless reset: clear reconstructed depth (interpolated frame); **reconstruct_previous_depth**
   (SRV dilated MV 0, dilated depth 1, current source 2, distortion 3; UAV 4; CB 5);
   **game_motion_vector_field** (SRV dilated MV 0, dilated depth 1, previous source 2, current
   source 3, distortion 4; UAV X 5, Y 6; CB 7); **game_vector_field_inpainting_pyramid** (SRV X 0,
   Y 1; UAV counters 2, pyramid mips 0-12 3-15; CB 16, CB pyramid 17; SPD setup over render size);
   **optical_flow_vector_field** (SRV OF 0, OF confidence 1, dilated depth 2, previous 3,
   current 4; UAV X 6, Y 7; CB 8 — binding 5 unused); **disocclusion_mask** (SRV game X 0, Y 1,
   reconstructed previous depth 2, dilated depth 3, reconstructed interpolated depth 4,
   inpainting pyramid 5, distortion 6; UAV mask 7; CB 8).
4. **interpolation** (`ffx_frameinterpolation_pass`): SRV game X 0, Y 1, OF X 2, Y 3, previous
   source 4, current source 5, disocclusion 6, inpainting pyramid 7, counters 8 (readonly
   buffer); UAV output 9 (formatless image: needs `shaderStorageImageReadWithoutFormat`); CB 10.
5. **inpainting_pyramid** (SRV output 0; UAV counters 1, mips 0-12 2-14; CB 15, CB pyramid 16;
   SPD over display size), then **inpainting** (SRV SCD 0, pyramid 1, present backbuffer 2,
   current source 3; UAV output 7; CB 8 — 4-6 unused).
6. Copy current source → previous interpolation source.

The binding lists have gaps (unused numbers), so the port's `Pass` needs explicit binding numbers.

Internal resources (`frameinterpolationCreate`): R32_UINT reconstructed depth, game MV X/Y,
OF MV X/Y (render size); RGBA16F inpainting pyramid (display/2, full mip chain); R8 inpainting
mask and RG8 disocclusion mask; RG8 1x1 default distortion field (zero); 8-byte counters
buffer; previous interpolation source (display size, the back buffer's format).

Constants: `FrameInterpolationConstants` (`ffx_frameinterpolation_private.h`, 160 bytes with the
trailing jitter and MV scale), filled as in `ffxFrameInterpolationDispatch`.

## Presenting

The presenter (`vk_present_manager` / `renderer_vulkan`) gets a frame N with its inputs. With
generation on: dispatch interpolation between N-1 and N into a spare image, present it, wait half
the measured frame interval, then present N. Costs: half a frame of latency, one extra blit, the
passes (~1-2 ms at 1440p on a mid-range GPU per AMD's numbers). Skip generation when the frame
interval is under ~8 ms (already high), on a reset (scene cut: SCD flags it), or with no inputs.

## Build order

1. Shaders compiled into `host_shaders` (FI 10 passes + OF 7), host classes `FrameInterpolator`
   and `OpticalFlow` with the binding tables above; a debug env var that writes the interpolated
   frame instead of the real one, to check the output on screen.
2. Presenter pacing and the setting (*Graphics → Frame generation*, off by default).
3. HDR (the colour is scRGB/PQ: `backBufferTransferFunction`, `minMaxLuminance`).
