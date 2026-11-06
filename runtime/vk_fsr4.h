// AMD FSR 4 (AMD FSR Upscaling 4.x) through AMD's own signed FidelityFX SDK 2.x DLLs (vk_fsr4.cpp), as the temporal
// upscaler of vk_dlaa.c in place of NVIDIA's DLSS. Windows only; elsewhere nothing is available. The DLLs are not part
// of this repository (docs/fsr4.md says where they go). FSR 4 support by MK-JH (https://t.me/MKJH_APPS).
#pragma once
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll are both in the folder of totk.exe (cached).
int vk_fsr4_dlls_present(void);
// Loads the loader and checks that the GPU has an FSR 4 (4.x) provider; 0 means FSR 4 stays off for the run.
int vk_fsr4_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);

typedef struct {
    VkCommandBuffer cb;
    VkImage color, depth, motion, reactive, output;   // reactive may be VK_NULL_HANDLE
    VkFormat color_fmt, depth_fmt, motion_fmt, reactive_fmt, output_fmt;
    uint32_t render_w, render_h, out_w, out_h;
    float jitter_x, jitter_y;   // render pixels, the offset this frame's scene was drawn with
    float frame_ms;
    int reset;
} VkFsr4Dispatch;

// Records FSR 4 into d->cb (the context is made again when the sizes change); 0 when it failed.
int vk_fsr4_dispatch(const VkFsr4Dispatch *d);
void vk_fsr4_destroy(void);
const char *vk_fsr4_version(void);   // the provider's name once a context exists ("4.1.2"), else NULL

#ifdef __cplusplus
}
#endif
