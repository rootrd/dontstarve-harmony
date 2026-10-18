// M2-0：宿主自己开一条 Vulkan 通条到 XComponent 表面出图 —— 去风险用的冒烟层。
// 目标不是画面，是在真机（Maleoon 920 / VK 1.3）上钉死四件事：
//   1) VK_KHR_surface + VK_OHOS_surface 能不能建 VkSurface（用 OHNativeWindow）；
//   2) 队列族是否同时支持 graphics + present；
//   3) swapchain 的 format / presentMode / extent / imageCount 实测值（M2 重放层要用）；
//   4) acquire→clear→submit→present 循环能否稳定跑（含 OUT_OF_DATE / SUBOPTIMAL 处理）。
// 开关：DM_VK_SMOKE=1（aa start --ps DM_VK_SMOKE 1）。画面 = 每帧循环色相，肉眼可见在动。
#include <hilog/log.h>
#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_window/external_window.h>
#include "vk_smoke.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "D2R"
#define LOG_DOMAIN 0x3300
#define VLOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

static void *g_window;
static uint64_t g_width, g_height;
static volatile int g_window_live;
static volatile int g_stop;
static pthread_t g_th;
static int g_thread_live;

enum { NSTAGE = 2, SMOKE_FRAMES = 1200 };

static const char *rname(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "OK";
    case VK_TIMEOUT: return "TIMEOUT";
    case VK_NOT_READY: return "NOT_READY";
    case VK_INCOMPLETE: return "INCOMPLETE";
    case VK_ERROR_OUT_OF_DATE_KHR: return "OUT_OF_DATE";
    case VK_SUBOPTIMAL_KHR: return "SUBOPTIMAL";
    case VK_ERROR_SURFACE_LOST_KHR: return "SURFACE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "INIT_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_ABSENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXT_ABSENT";
    default: return "ERR";
    }
}

// 冒烟只需要一个函数：VkResult 非 OK 就报出来
#define VKCHK(label, call) do { VkResult _r = (call); VLOG("[vk] %{public}s = %{public}d (%{public}s)", label, (int)_r, rname(_r)); if (_r != VK_SUCCESS) { fail = 1; } } while (0)

static void hsv(float h, float s, float v, float *out)
{
    float c = v * s, x = c * (1 - fabsf(fmodf(h * 6.0f, 2) - 1)), m = v - c;
    float r, g, b;
    int seg = (int)(h * 6.0f);
    switch (seg % 6) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    out[0] = r + m; out[1] = g + m; out[2] = b + m; out[3] = 1.0f;
}

static void *smoke_main(void *p)
{
    (void)p;
    int fail = 0;
    extern void crash_install(void);
    crash_install();   // 驱动 abort（SIGABRT）也要在我们的 hilog 通道里留下 pc/backtrace
    VLOG("[vk] 冒烟开始：窗口 %{public}p 尺寸 %{public}u x %{public}u", g_window, (unsigned)g_width, (unsigned)g_height);

    // ---- 1) instance ----
    VkInstance inst = NULL;
    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "D2R-M2-0";
    ai.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    ai.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VKCHK("vkCreateInstance(KHR_surface + OHOS_surface)", vkCreateInstance(&ici, NULL, &inst));
    if (fail) return NULL;

    // ---- 2) 表面 ----
    VkSurfaceKHR surf = NULL;
    VkSurfaceCreateInfoOHOS sm = { VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS };
    sm.window = (OHNativeWindow *)g_window;
    VKCHK("vkCreateSurfaceOHOS", vkCreateSurfaceOHOS(inst, &sm, NULL, &surf));
    if (fail) { vkDestroyInstance(inst, NULL); return NULL; }

    // ---- 3) 选物理设备 + 队列族（要 graphics 且 present）----
    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, NULL);
    VLOG("[vk] 物理设备数 = %{public}u", npd);
    if (!npd) { vkDestroySurfaceKHR(inst, surf, NULL); vkDestroyInstance(inst, NULL); return NULL; }
    VkPhysicalDevice pdlist[16];
    if (npd > 16) npd = 16;
    vkEnumeratePhysicalDevices(inst, &npd, pdlist);
    VkPhysicalDevice pd = NULL;
    uint32_t qfamily = 0;
    int have_present = 0;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pdlist[i], &pp);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pdlist[i], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pdlist[i], &nq, qp);
        int fam = -1;
        for (uint32_t q = 0; q < nq; q++) {
            if (!(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pdlist[i], q, surf, &sup);
            VLOG("[vk] 设备%{public}u 队列族%{public}u flags=%{public}#x present=%{public}d", i, q, (unsigned)qp[q].queueFlags, (int)sup);
            if (sup) { fam = (int)q; break; }
        }
        VLOG("[vk] 设备%{public}u name=%{public}s api=%{public}u.%{public}u driver=%{public}u",
             i, pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), pp.driverVersion);
        if (fam >= 0 && !pd) { pd = pdlist[i]; qfamily = (uint32_t)fam; have_present = 1; }
    }
    if (!pd || !have_present) { VLOG("【红】没有同时支持 graphics+present 的队列族"); vkDestroySurfaceKHR(inst, surf, NULL); vkDestroyInstance(inst, NULL); return NULL; }

    // ---- 4) 设备 ----
    // 设备扩展实测清单打出来（M2 要照这份挑），并只启用真实存在的那些
    uint32_t ndext = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ndext, NULL);
    VLOG("[vk] 设备扩展数 = %{public}u", ndext);
    VkExtensionProperties dprops[128];
    if (ndext > 128) ndext = 128;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ndext, dprops);
    int swap_ok = 0;
    // M2 的 Vulkan 回放层要照这份清单挑扩展与限制：全量打名，并点名几颗决定架构的（零拷贝池 / 描述符 / 动态渲染 / 时间线栅栏）
    // The M2 Vulkan replay layer picks its extensions and limits from this list: dump every name and call out the few that decide the architecture (zero-copy pool / descriptors / dynamic rendering / timeline fences)
    static const char *WANT[] = { "VK_EXT_external_memory_host", "VK_KHR_external_memory", "VK_EXT_descriptor_indexing", "VK_KHR_push_descriptor",
                                  "VK_KHR_descriptor_update_template", "VK_KHR_dynamic_rendering", "VK_KHR_timeline_semaphore", "VK_KHR_bind_memory2",
                                  "VK_KHR_create_renderpass2", "VK_KHR_maintenance4", "VK_KHR_maintenance5", "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2",
                                  "VK_EXT_vertex_input_dynamic_state", "VK_KHR_map_memory2", "VK_EXT_host_query_reset", "VK_KHR_spirv_1_4", "VK_EXT_scalar_block_layout",
                                  "VK_KHR_portability_subset", "VK_EXT_4444_formats", "VK_KHR_driver_properties" };
    for (uint32_t i = 0; i < ndext; i++) {
        if (!strcmp(dprops[i].extensionName, "VK_KHR_swapchain")) swap_ok = 1;
        VLOG("[vk]   设备扩展 %{public}u %{public}s", i, dprops[i].extensionName);
    }
    for (unsigned w = 0; w < sizeof WANT / sizeof *WANT; w++) {
        int p = 0;
        for (uint32_t i = 0; i < ndext; i++) if (!strcmp(dprops[i].extensionName, WANT[w])) p = (int)i + 1;
        VLOG("[vk] 要的扩展 %{public}s = %{public}d（0 = 没有）", WANT[w], p);
    }
    {   // 决定「游戏内存 → VkBuffer」能不能零拷贝，以及描述符上限
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        VLOG("[vk] 内存类型 %{public}u 堆 %{public}u", mp.memoryTypeCount, mp.memoryHeapCount);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            VLOG("[vk]   memType %{public}u bits=0x%{public}x heap=%{public}u", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            VLOG("[vk]   memHeap %{public}u size=%{public}llu MB flags=0x%{public}x", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20), mp.memoryHeaps[i].flags);
        VkPhysicalDeviceProperties pp2;
        vkGetPhysicalDeviceProperties(pd, &pp2);
        VkPhysicalDeviceLimits L = pp2.limits;
        VLOG("[vk] limits sets=%{public}u perStageCombined=%{public}u perStageUbo=%{public}u perStageSamp=%{public}u perStageSbuf=%{public}u uboAlign=%{public}u nonCoherent=%{public}u maxImg=%{public}u gran=%{public}llu colAtt=%{public}u ts=%{public}d",
             L.maxBoundDescriptorSets, L.maxPerStageDescriptorSampledImages, L.maxPerStageDescriptorUniformBuffers, L.maxPerStageDescriptorSamplers,
             L.maxPerStageDescriptorStorageBuffers, (unsigned)L.minUniformBufferOffsetAlignment, (unsigned)L.nonCoherentAtomSize, L.maxImageDimension2D,
             (unsigned long long)L.bufferImageGranularity, L.maxColorAttachments, (int)L.timestampComputeAndGraphics);
    }
    VLOG("[vk] VK_KHR_swapchain 在场 = %{public}d", swap_ok);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // VK_KHR_swapchain 是设备级扩展：不在这里开，装载器（libvulkan.so）里驱动的分发指针就是 NULL，
    // 调 vkCreateSwapchainKHR 直接 abort —— 实测第 30 轮的现场就是这么报的：
    //   "VulkanLoader ERROR: vkCreateSwapchainKHR: Driver's function pointer was NULL ... Was the VK_KHR_swapchain extension enabled?"
    // VK_KHR_swapchain is a device-level extension: without enabling it here the loader's driver pointer stays NULL and calling vkCreateSwapchainKHR aborts.
    { const char *dext[] = { "VK_KHR_swapchain" };
      if (swap_ok) { dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext; } }
    VkDevice dev = NULL;
    VKCHK("vkCreateDevice", vkCreateDevice(pd, &dci, NULL, &dev));
    if (fail) goto OUT_SURFACE;
    VkQueue queue = NULL;
    vkGetDeviceQueue(dev, qfamily, 0, &queue);

    {
        // ---- 5) 表面能力 + 格式（M2 要抄的实测值）----
        VkSurfaceCapabilitiesKHR cap;
        VKCHK("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &cap));
        VLOG("[vk] 能力 min/maxImage=%{public}u/%{public}u curExtent=%{public}u x %{public}u minExtent=%{public}u x %{public}u maxExtent=%{public}u x %{public}u maxLayers=%{public}u preTransform=%{public}#x supportedUsage=%{public}#x",
             cap.minImageCount, cap.maxImageCount, cap.currentExtent.width, cap.currentExtent.height,
             cap.minImageExtent.width, cap.minImageExtent.height, cap.maxImageExtent.width, cap.maxImageExtent.height,
             cap.maxImageArrayLayers, (unsigned)cap.currentTransform, (unsigned)cap.supportedUsageFlags);
        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL);
        VkSurfaceFormatKHR fmts[32];
        if (nf > 32) nf = 32;
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
        VLOG("[vk] 表面格式 %{public}u 种：", nf);
        for (uint32_t i = 0; i < nf; i++)
            VLOG("[vk]   #%{public}u format=%{public}d colorspace=%{public}d", i, (int)fmts[i].format, (int)fmts[i].colorSpace);
        uint32_t npm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, NULL);
        VkPresentModeKHR pms[16];
        if (npm > 16) npm = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pms);
        VLOG("[vk] 呈现模式 %{public}u 种：", npm);
        for (uint32_t i = 0; i < npm; i++) VLOG("[vk]   #%{public}u %{public}d", i, (int)pms[i]);
        if (fail) goto OUT_DEVICE;

        // 首选 RGBA/BGRA UNORM + sRGB 或 NON_EXTENDED；FIFO（=vsync）
        VkFormat want = VK_FORMAT_B8G8R8A8_UNORM;
        int okf = -1;
        static const VkFormat order[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_FORMAT_R8G8B8_UNORM };
        for (uint32_t o = 0; o < 5 && okf < 0; o++)
            for (uint32_t i = 0; i < nf; i++)
                if (fmts[i].format == order[o] && (fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) { okf = (int)i; break; }
        if (okf < 0 && nf) okf = 0;
        { const char *e = getenv("DM_VK_FMT");   // 直接指定表面格式表下标，换格式不用重编
          if (e && *e) { long v = strtol(e, NULL, 0); if (v >= 0 && (uint32_t)v < nf) okf = (int)v; } }
        if (okf < 0) { VLOG("【红】表面没有可用格式"); goto OUT_DEVICE; }
        want = fmts[okf].format;
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
        int okpm = 0;
        for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_FIFO_KHR) { okpm = 1; break; }
        if (!okpm && npm) pm = pms[0];
        VLOG("[vk] 选定 format=%{public}d colorspace=%{public}d presentMode=%{public}d", (int)want, (int)fmts[okf].colorSpace, (int)pm);
        VLOG("[vk] supportedTransforms=%{public}#x supportedCompositeAlpha=%{public}#x", (int)cap.supportedTransforms, (int)cap.supportedCompositeAlpha);

        // ---- 6) swapchain ----
        uint32_t nimg = cap.minImageCount + 1;
        if (cap.maxImageCount && nimg > cap.maxImageCount) nimg = cap.maxImageCount;
        if (nimg > 3) nimg = 3;
        uint32_t w = cap.currentExtent.width != 0xFFFFFFFFu ? cap.currentExtent.width : (uint32_t)g_width;
        uint32_t h = cap.currentExtent.height != 0xFFFFFFFFu ? cap.currentExtent.height : (uint32_t)g_height;
        // 合成 alpha：驱动只支持 PREMULTIPLIED 时给 OPAQUE 会被拒（实测这里直接 abort 过一次），照能力挑
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
            alpha = (cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
                        ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        VkSurfaceTransformFlagBitsKHR pretr = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        if (!(cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
            pretr = cap.currentTransform;
        // 外部覆盖（一次装机可试多种组合，不用重编）：DM_VK_ALPHA / DM_VK_PRETR / DM_VK_PM / DM_VK_IMG / DM_VK_EXT
        { const char *e;
          if ((e = getenv("DM_VK_ALPHA")) && *e) alpha = (VkCompositeAlphaFlagBitsKHR)strtoul(e, NULL, 0);
          if ((e = getenv("DM_VK_PRETR")) && *e) pretr = (VkSurfaceTransformFlagBitsKHR)strtoul(e, NULL, 0);
          if ((e = getenv("DM_VK_PM")) && *e) pm = (VkPresentModeKHR)strtol(e, NULL, 0);
          if ((e = getenv("DM_VK_IMG")) && *e) { nimg = (uint32_t)strtoul(e, NULL, 0);
              if (nimg < cap.minImageCount || (cap.maxImageCount && nimg > cap.maxImageCount)) nimg = cap.minImageCount + 1; }
          // 小尺寸交换链：若 2848x1276 abort 而 64x64 能建，锅在 gralloc/buffer 分配；两者都 abort 才是窗口/队列本身
          if ((e = getenv("DM_VK_EXT")) && *e) { uint32_t s = (uint32_t)strtoul(e, NULL, 0);
              if (s >= cap.minImageExtent.width && s <= cap.maxImageExtent.width
                  && s >= cap.minImageExtent.height && s <= cap.maxImageExtent.height) { w = s; h = s; } } }
        VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = surf;
        sci.minImageCount = nimg;
        sci.imageFormat = want;
        sci.imageColorSpace = fmts[okf].colorSpace;
        sci.imageExtent = (VkExtent2D){ w, h };
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = pretr;
        sci.compositeAlpha = alpha;
        sci.presentMode = pm;
        sci.clipped = VK_TRUE;
        VkSwapchainKHR sw = NULL;
        VLOG("[vk] 建交换链：%{public}u x %{public}u img=%{public}u format=%{public}d alpha=%{public}#x preTransform=%{public}#x mode=%{public}d",
             w, h, nimg, (int)want, (int)alpha, (int)pretr, (int)pm);
        // 窗口生产者几何：XComponent 的 OHNativeWindow 建 swapchain 前先把 buffer 尺寸设给它是 OHOS 通行做法
        // （驱动侧若拿到 0x0 几何会直接 abort —— 实测 create 处 SIGABRT，先排掉这条）
        { const char *e = getenv("DM_VK_GEOM");
          if (!e || e[0] != '0') {
              void *nh = dlopen("libnative_window.so", RTLD_NOW | RTLD_GLOBAL);
              typedef int32_t (*pfn_OPT)(OHNativeWindow *, int, ...);
              pfn_OPT opt = (pfn_OPT)dlsym(nh ? nh : RTLD_DEFAULT, "OH_NativeWindow_NativeWindowHandleOpt");
              if (opt) VLOG("[vk] SET_BUFFER_GEOMETRY(%{public}u, %{public}u) rc=%{public}d",
                            (unsigned)w, (unsigned)h, opt((OHNativeWindow *)g_window, 0, (int32_t)w, (int32_t)h));
              else VLOG("[vk] OH_NativeWindow_NativeWindowHandleOpt 没取到（句柄 %{public}p）", nh);
          } }
        crash_install();   // 驱动会自装信号处理器抢走现场，建链前再装一次
        // 现场 bt 是不带库名的裸地址：把这几家的映射区间打出来，事后能把帧归到 libvulkan / 驱动 / libdmhost
        { FILE *fp = fopen("/proc/self/maps", "r");
          if (fp) { char ln[512]; int nmap = 0;
            while (fgets(ln, sizeof ln, fp) && nmap < 40)
              if (strstr(ln, "vulkan") || strstr(ln, "maleoon") || strstr(ln, "native_window") || strstr(ln, "libdmhost")) {
                  char *p = ln; while (*p == ' ') p++;
                  p[strcspn(p, "\n")] = 0;
                  VLOG("[maps] %{public}s", p); nmap++;
              }
            fclose(fp); } }
        VKCHK("vkCreateSwapchainKHR", vkCreateSwapchainKHR(dev, &sci, NULL, &sw));
        if (fail) goto OUT_DEVICE;
        VLOG("[vk] swapchain %{public}u 张 %{public}u x %{public}u", nimg, w, h);

        uint32_t ni = 0;
        vkGetSwapchainImagesKHR(dev, sw, &ni, NULL);
        VLOG("[vk] 实际交换链图像数 = %{public}u", ni);
        VkImage *imgs = calloc(ni, sizeof *imgs);
        vkGetSwapchainImagesKHR(dev, sw, &ni, imgs);

        // ---- 7) render pass / imageview / framebuffer ----
        VkRenderPass rp = NULL;
        {
            VkAttachmentDescription a = {0};
            a.format = want;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription sp = {0};
            sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sp.colorAttachmentCount = 1;
            sp.pColorAttachments = &ar;
            VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                0, 0, VK_DEPENDENCY_BY_REGION_BIT };
            VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            rpc.attachmentCount = 1; rpc.pAttachments = &a;
            rpc.subpassCount = 1; rpc.pSubpasses = &sp;
            rpc.dependencyCount = 1; rpc.pDependencies = &dep;
            VKCHK("vkCreateRenderPass", vkCreateRenderPass(dev, &rpc, NULL, &rp));
        }
        VkImageView *views = calloc(ni, sizeof *views);
        VkFramebuffer *fbs = calloc(ni, sizeof *fbs);
        for (uint32_t i = 0; i < ni && !fail; i++) {
            VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = imgs[i];
            iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            iv.format = want;
            iv.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VKCHK("vkCreateImageView", vkCreateImageView(dev, &iv, NULL, &views[i]));
            VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbi.renderPass = rp;
            fbi.attachmentCount = 1; fbi.pAttachments = &views[i];
            fbi.width = w; fbi.height = h; fbi.layers = 1;
            VKCHK("vkCreateFramebuffer", vkCreateFramebuffer(dev, &fbi, NULL, &fbs[i]));
        }

        // ---- 8) 每帧：acquire → 录 clear → submit → present ----
        VkCommandPool cp = NULL;
        {
            VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpi.queueFamilyIndex = qfamily;
            VKCHK("vkCreateCommandPool", vkCreateCommandPool(dev, &cpi, NULL, &cp));
        }
        VkCommandBuffer cb[NSTAGE];
        {
            VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ca.commandPool = cp; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = NSTAGE;
            VKCHK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &ca, cb));
        }
        VkSemaphore acq[NSTAGE], vis[NSTAGE];
        VkFence fnd[NSTAGE];
        for (int i = 0; i < NSTAGE && !fail; i++) {
            VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VKCHK("vkCreateSemaphore", vkCreateSemaphore(dev, &s, NULL, &acq[i]));
            VKCHK("vkCreateSemaphore", vkCreateSemaphore(dev, &s, NULL, &vis[i]));
            VkFenceCreateInfo f = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VKCHK("vkCreateFence", vkCreateFence(dev, &f, NULL, &fnd[i]));
        }

        uint64_t t0 = 0, tlast = 0;
        int frames = 0, odo = 0;
        while (!fail && !g_stop && frames < SMOKE_FRAMES && g_window_live) {
            int s = frames % NSTAGE;
            VKCHK("vkWaitForFences", vkWaitForFences(dev, 1, &fnd[s], VK_TRUE, 1000ull * 1000 * 1000));
            if (fail) break;
            vkResetFences(dev, 1, &fnd[s]);
            uint32_t idx = 0;
            VkResult ar = vkAcquireNextImageKHR(dev, sw, 1000ull * 1000 * 1000, acq[s], NULL, &idx);
            if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
                VLOG("[vk] acquire → %{public}s（尺寸变了/交换链需重建）idx=%{public}u", rname(ar), idx);
                odo++;
                if (ar == VK_ERROR_OUT_OF_DATE_KHR) break;
            }
            float hue = (float)((frames % 180) / 180.0);
            float col[4];
            hsv(hue, 0.9f, 0.9f, col);
            vkResetCommandBuffer(cb[s], 0);
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkBeginCommandBuffer(cb[s], &bi);
            VkClearValue cv = {0};
            cv.color.float32[0] = col[0]; cv.color.float32[1] = col[1];
            cv.color.float32[2] = col[2]; cv.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpb.renderPass = rp; rpb.framebuffer = fbs[idx];
            rpb.renderArea = (VkRect2D){{ 0, 0 }, { w, h }};
            rpb.clearValueCount = 1; rpb.pClearValues = &cv;
            vkCmdBeginRenderPass(cb[s], &rpb, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb[s]);
            vkEndCommandBuffer(cb[s]);

            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acq[s];
            si.pWaitDstStageMask = &stage;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb[s];
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &vis[s];
            VKCHK("vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fnd[s]));
            if (fail) break;
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &vis[s];
            pi.swapchainCount = 1; pi.pSwapchains = &sw; pi.pImageIndices = &idx;
            VkResult pr = vkQueuePresentKHR(queue, &pi);
            if (pr != VK_SUCCESS) {
                VLOG("[vk] present → %{public}d (%{public}s)", (int)pr, rname(pr));
                if (pr == VK_ERROR_OUT_OF_DATE_KHR) break;
                if (pr != VK_SUBOPTIMAL_KHR) { fail = 1; break; }
            }
            frames++;
            if (frames == 1 || frames == 60 || frames % 300 == 0) {
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
                if (!t0) t0 = now;
                double fps = tlast ? frames * 1000000.0 / (double)(now - t0) : 0;
                VLOG("[vk] 已呈现 %{public}d 帧（%{public}u x %{public}u，重建信号 %{public}d，均速 %{public}.1f fps）",
                     frames, w, h, odo, fps);
                tlast = now;
            }
        }
        VLOG("[vk] 冒烟循环结束：frames=%{public}d fail=%{public}d stop=%{public}d live=%{public}d", frames, fail, g_stop, g_window_live);

        vkDeviceWaitIdle(dev);
        for (int i = 0; i < NSTAGE; i++) { vkDestroySemaphore(dev, acq[i], NULL); vkDestroySemaphore(dev, vis[i], NULL); vkDestroyFence(dev, fnd[i], NULL); }
        vkDestroyCommandPool(dev, cp, NULL);
        for (uint32_t i = 0; i < ni; i++) { vkDestroyFramebuffer(dev, fbs[i], NULL); vkDestroyImageView(dev, views[i], NULL); }
        free(views); free(fbs); free(imgs);
        vkDestroyRenderPass(dev, rp, NULL);
        vkDestroySwapchainKHR(dev, sw, NULL);
    }
OUT_DEVICE:
    vkDestroyDevice(dev, NULL);
OUT_SURFACE:
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    VLOG("[vk] 冒烟资源已释放");
    return NULL;
}

void vk_smoke_set_window(void *window, uint64_t w, uint64_t h)
{
    g_window = window;
    g_width = w;
    g_height = h;
    g_window_live = 1;
    vk_smoke_maybe_start();
}

void vk_smoke_window_gone(void)
{
    g_window_live = 0;
    g_stop = 1;
}

void vk_smoke_maybe_start(void)
{
    const char *e = getenv("DM_VK_SMOKE");
    if (!e || e[0] == '0') { VLOG("[vk] DM_VK_SMOKE 未开启（窗口 %{public}p %{public}u x %{public}u）", g_window, (unsigned)g_width, (unsigned)g_height); return; }
    if (g_thread_live || !g_window || !g_window_live) {
        VLOG("[vk] 冒烟等待条件：thread=%{public}d window=%{public}p live=%{public}d", g_thread_live, g_window, g_window_live);
        return;
    }
    g_thread_live = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_th, &a, smoke_main, NULL)) { g_thread_live = 0; VLOG("【红】冒烟线程创建失败"); return; }
    pthread_detach(g_th);
    VLOG("[vk] 冒烟线程已创建");
}
