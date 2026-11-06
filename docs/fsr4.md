# AMD FSR 4

FSR 4 (AMD FSR Upscaling 4.x, AMD's machine-learning upscaler) can take the place of NVIDIA DLSS for the 3D scene. Windows
only. FSR 4 support by MK-JH, Telegram: https://t.me/MKJH_APPS

## What you need

- A Radeon RX 6000 series GPU or newer with a current driver (Vulkan 1.3). On RX 6000 and 7000 the INT8 model runs; RDNA 4
  GPUs also use FP8.
- AMD's two signed DLLs from the FidelityFX SDK 2.x (FSR Upscaling 4.x) in the folder of `totk.exe`:
  `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`. They are not part of this repository: AMD's SDK
  licence allows them in binary form only, with its notice.

## Turning it on

In the game's menu (F1), Graphics, "AMD FSR 4", or `fsr4 = 1` in `totk.ini`. It applies after a restart. The DLSS mode
row sets the resolution it works from: DLAA is the native scene, Quality, Balanced, Performance and Ultra Performance
draw the scene at 67, 58, 50 and 33 % and let FSR 4 scale it up. With `fsr4` and `dlaa` both on, FSR 4 runs and NGX is
not loaded.

If the DLLs are missing, or the GPU has no FSR 4 provider, `totk.log` says so (`fsr4:` lines) and the game runs as
without it.

## How it works

`runtime/vk_fsr4.cpp` loads the loader DLL at run time, asks it for the newest 4.x provider of the GPU and creates an
upscaling context once for the output's size. Each frame `runtime/vk_dlaa.c` `evaluate` gives it the scene's light
target, the reversed D32F depth, the motion vectors (render pixels), the characters' mask as its reactive mask and the
frame's jitter, exactly what DLSS gets.

## Test switches (environment variables)

- `TOTK_FSR4_DEBUG=1`: AMD's checks of the inputs, reported in the log.
- `TOTK_FSR4_VIEW=1`: the upscaler's own debug view of its inputs in the output.
