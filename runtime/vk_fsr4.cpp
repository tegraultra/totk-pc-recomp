// AMD FSR 4 through AMD's signed FidelityFX SDK 2.x DLLs on Vulkan (vk_fsr4.h). FSR 4 support by MK-JH.
//
// The SDK's ffx-api, loaded at run time: amd_fidelityfx_loader_vk.dll next to totk.exe finds
// amd_fidelityfx_upscaler_vk.dll beside it and hands out a provider per GPU (FSR 4's machine-learning model where the
// GPU can run it, FSR 3.1 otherwise). Only a provider whose version is 4.x is used: without one FSR 4 is off for the
// run and the frame goes on without a temporal upscaler (vk_dlaa.c). The headers are AMD's (third_party/ffx_api).
//
// The inputs are the ones vk_dlaa.c gives DLSS: the scene's light target (linear HDR), the reversed D32F depth, the
// motion vectors in render pixels (RG16F, pointing to where the pixel was last frame), the characters' bias mask as the
// reactive mask, and the jitter of the frame in render pixels.
#include "vk_fsr4.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define FFX_API_ENTRY  // (called through pointers: not the SDK's dllexport)
#include <windows.h>

#include "third_party/ffx_api/api/include/ffx_api.h"
#include "third_party/ffx_api/api/include/ffx_api_types.h"
#include "third_party/ffx_api/api/include/vk/ffx_api_vk.h"
#include "third_party/ffx_api/upscalers/include/ffx_upscale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vector>

extern "C" void rt_log(const char *fmt, ...);

static const wchar_t *const k_loader = L"amd_fidelityfx_loader_vk.dll";
static const wchar_t *const k_upscaler = L"amd_fidelityfx_upscaler_vk.dll";

static HMODULE mod;
static PfnFfxCreateContext p_create;
static PfnFfxDestroyContext p_destroy;
static PfnFfxQuery p_query;
static PfnFfxDispatch p_dispatch;
static bool failed;   // given up for this run
static ffxContext ctx;
static bool have_ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static VkDevice ctx_dev;
static VkPhysicalDevice g_phys;
static PFN_vkGetDeviceProcAddr g_gdpa;
static VkDeviceContext dev_ctx;   // the backend keeps a pointer to it
static uint64_t provider_id;
static char version_name[64];

static bool exe_dir(wchar_t *out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return false;
    while (n && out[n - 1] != L'\\' && out[n - 1] != L'/') n--;
    out[n] = 0;
    return n > 0;
}

static bool file_here(const wchar_t *dir, const wchar_t *name) {
    wchar_t p[MAX_PATH + 64];
    swprintf(p, sizeof p / sizeof *p, L"%ls%ls", dir, name);
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

extern "C" int vk_fsr4_dlls_present(void) {
    static int present = -1;
    if (present < 0) {
        wchar_t dir[MAX_PATH];
        present = exe_dir(dir, MAX_PATH) && file_here(dir, k_loader) && file_here(dir, k_upscaler);
        rt_log("fsr4: AMD's FSR DLLs %s", present ? "found next to the program" : "not found next to the program (docs/fsr4.md)");
    }
    return present;
}

extern "C" const char *vk_fsr4_version(void) { return have_ctx && version_name[0] ? version_name : NULL; }

static void message(uint32_t type, const wchar_t *msg) {
    char buf[512];
    snprintf(buf, sizeof buf, "%ls", msg);
    rt_log("fsr4: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

static bool load_loader(void) {
    if (mod) return true;
    wchar_t dir[MAX_PATH], p[MAX_PATH + 64];
    if (exe_dir(dir, MAX_PATH)) {
        swprintf(p, sizeof p / sizeof *p, L"%ls%ls", dir, k_loader);
        mod = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   // its neighbours are looked for beside it
    }
    if (!mod) mod = LoadLibraryW(k_loader);
    if (!mod) {
        rt_log("fsr4: could not load %ls (error %lu)", k_loader, (unsigned long)GetLastError());
        return false;
    }
    p_create = (PfnFfxCreateContext)GetProcAddress(mod, "ffxCreateContext");
    p_destroy = (PfnFfxDestroyContext)GetProcAddress(mod, "ffxDestroyContext");
    p_query = (PfnFfxQuery)GetProcAddress(mod, "ffxQuery");
    p_dispatch = (PfnFfxDispatch)GetProcAddress(mod, "ffxDispatch");
    if (!p_create || !p_destroy || !p_query || !p_dispatch) {
        rt_log("fsr4: %ls lacks ffx-api functions", k_loader);
        FreeLibrary(mod);
        mod = NULL;
        return false;
    }
    return true;
}

extern "C" void vk_fsr4_destroy(void) {
    if (!have_ctx) return;
    vkDeviceWaitIdle(ctx_dev);
    p_destroy(&ctx, nullptr);
    ctx = nullptr;
    have_ctx = false;
    version_name[0] = 0;
}

// The backend's device functions: a KHR name the device was not made with is the core function it was promoted to, and
// debug labels do nothing without VK_EXT_debug_utils.
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = g_gdpa(dev, name);
    if (!f && !strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!f && !strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = g_gdpa(dev, core);
    }
    return f;
}

// The first number in a provider's name: "4.1.2" -> 4, "FSR 3.1.6" -> 3 (0: none)
static int major_of(const char *name) {
    for (; name && *name; name++)
        if (*name >= '0' && *name <= '9') return atoi(name);
    return 0;
}

// The id of the newest 4.x provider this GPU has (0: none)
static uint64_t find_fsr4(void) {
    dev_ctx = {ctx_dev, g_phys, device_proc};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &dev_ctx;
    uint64_t n = 0;
    q.outputCount = &n;
    ffxReturnCode_t rc = p_query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK || !n) {
        rt_log("fsr4: version query: error %u, %llu versions", (unsigned)rc, (unsigned long long)n);
        return 0;
    }
    std::vector<uint64_t> ids(n);
    std::vector<const char *> names(n);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    rc = p_query(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) {
        rt_log("fsr4: version query: error %u", (unsigned)rc);
        return 0;
    }
    uint64_t best = 0;
    int best_major = 0;
    for (uint64_t i = 0; i < n; i++) {
        int m = major_of(names[i]);
        rt_log("fsr4: upscaler provider %llu: %s", (unsigned long long)i, names[i] ? names[i] : "?");
        if (m >= 4 && (!best || m >= best_major)) best = ids[i], best_major = m;
    }
    return best;
}

extern "C" int vk_fsr4_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (failed) return 0;
    g_phys = phys, ctx_dev = dev, g_gdpa = gdpa;
    if (!vk_fsr4_dlls_present() || !load_loader()) return failed = true, 0;
    provider_id = find_fsr4();
    if (!provider_id) {
        rt_log("fsr4: no FSR 4 provider for this GPU: FSR 4 off");
        return failed = true, 0;
    }
    return 1;
}

static bool make_context(const VkFsr4Dispatch *d) {
    vk_fsr4_destroy();
    // The scene is linear light above 1 where it is bright: HDR input, and the exposure left to the upscaler. Depth is
    // reversed (1 near, 0 far) and the motion vectors are at the render size without jitter in them.
    // TOTK_FSR4_DEBUG (environment): AMD's checks of the inputs, reported in the log.
    static ffxCreateContextDescUpscale cu;   // the descriptions stay alive for the context
    static ffxCreateBackendVKDesc bd;
    static ffxCreateContextDescUpscaleVersion vd;
    static ffxOverrideVersion ov;
    cu = {};
    bd = {};
    vd = {};
    ov = {};
    cu.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cu.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
               FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (getenv("TOTK_FSR4_DEBUG") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
    // The context is made for the output's size as its largest scene: the game's dynamic resolution moves the scene
    // between sizes below it, and a new context per size would hitch.
    cu.maxRenderSize = {d->out_w, d->out_h};
    cu.maxUpscaleSize = {d->out_w, d->out_h};
    cu.fpMessage = message;
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    bd.deviceContext = dev_ctx;
    vd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    vd.version = FFX_UPSCALER_VERSION;
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = provider_id;
    cu.header.pNext = &bd.header;
    bd.header.pNext = &vd.header;
    vd.header.pNext = &ov.header;
    ffxReturnCode_t rc = p_create(&ctx, &cu.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        rt_log("fsr4: context: error %u", (unsigned)rc);
        ctx = nullptr;
        return false;
    }
    have_ctx = true;
    ctx_rw = d->out_w, ctx_rh = d->out_h, ctx_ow = d->out_w, ctx_oh = d->out_h;
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (p_query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        snprintf(version_name, sizeof version_name, "%s", pv.versionName);
    rt_log("fsr4: context up to %ux%u -> %ux%u, provider %s", ctx_rw, ctx_rh, ctx_ow, ctx_oh, version_name[0] ? version_name : "?");
    return true;
}

static FfxApiResource resource(VkImage img, VkFormat fmt, uint32_t w, uint32_t h, bool uav, bool depth) {
    FfxApiResourceDescription rd = {};
    rd.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    rd.format = ffxApiGetSurfaceFormatVK(fmt);
    rd.width = w, rd.height = h, rd.depth = 1, rd.mipCount = 1;
    rd.flags = FFX_API_RESOURCE_FLAGS_NONE;
    rd.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (depth) rd.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    // the images of the Vulkan back end are kept in GENERAL: COMMON is that state
    return ffxApiGetResourceVK((void *)img, rd, FFX_API_RESOURCE_STATE_COMMON);
}

static FfxApiResource none(void) {
    FfxApiResource r = {};
    r.state = FFX_API_RESOURCE_STATE_COMMON;
    return r;
}

extern "C" int vk_fsr4_dispatch(const VkFsr4Dispatch *d) {
    if (failed || !provider_id) return 0;
    if ((!have_ctx || d->render_w > ctx_rw || d->render_h > ctx_rh || d->out_w != ctx_ow || d->out_h != ctx_oh) &&
        !make_context(d)) {
        failed = true;
        return 0;
    }
    ffxDispatchDescUpscale dd = {};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = (void *)d->cb;
    dd.color = resource(d->color, d->color_fmt, d->render_w, d->render_h, false, false);
    dd.depth = resource(d->depth, d->depth_fmt, d->render_w, d->render_h, false, true);
    dd.motionVectors = resource(d->motion, d->motion_fmt, d->render_w, d->render_h, false, false);
    dd.exposure = none();
    dd.reactive = d->reactive ? resource(d->reactive, d->reactive_fmt, d->render_w, d->render_h, false, false) : none();
    dd.transparencyAndComposition = none();
    dd.output = resource(d->output, d->output_fmt, d->out_w, d->out_h, true, false);
    dd.jitterOffset = {d->jitter_x, d->jitter_y};
    dd.motionVectorScale = {1.0f, 1.0f};
    dd.renderSize = {d->render_w, d->render_h};
    dd.upscaleSize = {d->out_w, d->out_h};
    dd.enableSharpening = false;
    dd.sharpness = 0.0f;
    dd.frameTimeDelta = d->frame_ms;
    dd.preExposure = 1.0f;
    dd.reset = d->reset != 0;
    dd.cameraNear = 0.1f;   // the camera is not known here; only the upscaler's depth-based tests read these
    dd.cameraFar = 10000.0f;
    dd.cameraFovAngleVertical = 0.9f;
    dd.viewSpaceToMetersFactor = 1.0f;
    static int view = -1;   // TOTK_FSR4_VIEW=1: the upscaler's own debug view of its inputs and state in the output
    if (view < 0) view = getenv("TOTK_FSR4_VIEW") != NULL;
    if (view) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    ffxReturnCode_t rc = p_dispatch(&ctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 8) rt_log("fsr4: dispatch: error %u", (unsigned)rc);
        return 0;
    }
    return 1;
}

#else   // not Windows: AMD ships the Vulkan DLLs for Windows only
extern "C" int vk_fsr4_dlls_present(void) { return 0; }
extern "C" int vk_fsr4_init(VkPhysicalDevice, VkDevice, PFN_vkGetDeviceProcAddr) { return 0; }
extern "C" int vk_fsr4_dispatch(const VkFsr4Dispatch *) { return 0; }
extern "C" void vk_fsr4_destroy(void) {}
extern "C" const char *vk_fsr4_version(void) { return NULL; }
#endif
