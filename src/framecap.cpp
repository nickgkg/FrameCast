// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.

// framecap: capture the SteamVR "Headset View" on the Steam Frame and write raw RGBA frames to stdout.
//
// The headset view is read with IVROverlayView::AcquireOverlayView on the system.HeadsetView overlay
// (works while the overlay is hidden). Each frame is blitted on the GPU to the output size and read back.
// The standard mirror APIs (GetMirrorTextureGL, IVROverlay::GetOverlayTexture) do not work on the Frame.
//
// Stabilization ("keep the horizon level"): when the --control file contains 1, each frame is rotated against the
// HMD roll by a compute shader (stabilize.comp) instead of the plain blit. The file is checked twice a second, so
// the mode changes during a running cast. Near straight up/down the roll is undefined and the correction fades out.
//
// Usage: framecap [--width 1280] [--height 720] [--fps 30] [--control FILE] [--zoom 1.2] | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -framerate 30 -i - ...
#include <chrono>
#include <cmath>
#include <csignal>
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "openvr.h"

#define LOG(...) fprintf(stderr, "framecap: " __VA_ARGS__)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("%s failed: %d\n", #x, r_); exit(1); } } while (0)

static volatile sig_atomic_t g_stop = 0;

static const uint32_t kStabilizeSpv[] =
#include "stabilize_spv.h"
;

struct StabParams {
    float uvMin[2], uvMax[2], outSize[2];
    float cosA, sinA, zoom;
    int32_t srgbSource;
};

static bool isSrgb(uint32_t fmt) {
    return fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// Read the control file (first char '1' = on). Missing file = off.
static bool readControl(const char *path) {
    if (!path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

// HMD roll in radians (positive = head tilted to the left) and a 0..1 weight that fades the correction
// out when looking almost straight up or down.
static bool hmdRoll(float &roll, float &weight) {
    vr::TrackedDevicePose_t pose;
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
    if (!pose.bPoseIsValid) return false;
    const auto &m = pose.mDeviceToAbsoluteTracking.m;
    roll = atan2f(m[1][0], m[1][1]);          // World-up component of the HMD right axis vs. up axis.
    float fwdY = -m[1][2];                     // World-up component of the view direction.
    float cosPitch = sqrtf(std::max(0.0f, 1.0f - fwdY * fwdY));
    weight = std::clamp((cosPitch - 0.25f) / 0.35f, 0.0f, 1.0f);
    return true;
}

static std::vector<std::string> split(const char *s) {
    std::vector<std::string> v; std::istringstream is(s); std::string t;
    while (is >> t) v.push_back(t);
    return v;
}

struct Vk {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        LOG("no memory type for flags 0x%x\n", want); exit(1);
    }
};

static Vk initVulkan() {
    Vk v{};
    char buf[4096];
    vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof buf);
    auto iext = split(buf);
    std::vector<const char *> iextp; for (auto &e : iext) iextp.push_back(e.c_str());
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName = "framecap"; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai; ici.enabledExtensionCount = iextp.size(); ici.ppEnabledExtensionNames = iextp.data();
    VK(vkCreateInstance(&ici, nullptr, &v.inst));

    uint64_t phys = 0;
    vr::VRSystem()->GetOutputDevice(&phys, vr::TextureType_Vulkan, v.inst);
    if (phys) v.pd = (VkPhysicalDevice)phys;
    else { uint32_t n = 1; vkEnumeratePhysicalDevices(v.inst, &n, &v.pd); }
    vkGetPhysicalDeviceMemoryProperties(v.pd, &v.mp);

    vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(v.pd, buf, sizeof buf);
    auto dext = split(buf);
    std::vector<const char *> dextp; for (auto &e : dext) dextp.push_back(e.c_str());

    uint32_t nqf = 0; vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf); vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nqf, qfs.data());
    for (uint32_t i = 0; i < nqf; i++) if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v.qfi = i; break; }
    float prio = 0.0f; // Stay out of the way of the game.
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = v.qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = dextp.size(); dci.ppEnabledExtensionNames = dextp.data();
    VK(vkCreateDevice(v.pd, &dci, nullptr, &v.dev));
    vkGetDeviceQueue(v.dev, v.qfi, 0, &v.q);
    return v;
}

int main(int argc, char **argv) {
    uint32_t W = 1280, H = 720, fps = 30;
    float zoom = 1.2f;
    const char *key = "system.HeadsetView";
    const char *control = nullptr;
    float testRoll = NAN; // --test-roll DEG: fixed roll instead of the HMD pose (for testing).
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--width")) W = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--height")) H = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--fps")) fps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--overlay")) key = argv[i + 1];
        else if (!strcmp(argv[i], "--control")) control = argv[i + 1];
        else if (!strcmp(argv[i], "--test-roll")) testRoll = (float)atof(argv[i + 1]) * (float)M_PI / 180.0f;
        else if (!strcmp(argv[i], "--zoom")) zoom = std::max(0.5f, (float)atof(argv[i + 1]));
        else { LOG("unknown option %s\n", argv[i]); return 2; }
    }
    signal(SIGINT, [](int) { g_stop = 1; });
    signal(SIGTERM, [](int) { g_stop = 1; });
    signal(SIGPIPE, [](int) { g_stop = 1; });

    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Overlay);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 1; }

    vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->FindOverlay(key, &ov) != vr::VROverlayError_None) { LOG("overlay %s not found\n", key); return 1; }

    Vk v = initVulkan();

    // Output image (GPU, blit target) and readback buffer (host visible).
    const VkFormat outFmt = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = outFmt; ici.extent = {W, H, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage outImg; VK(vkCreateImage(v.dev, &ici, nullptr, &outImg));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(v.dev, outImg, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = v.memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory outMem; VK(vkAllocateMemory(v.dev, &mai, nullptr, &outMem)); vkBindImageMemory(v.dev, outImg, outMem, 0);

    const VkDeviceSize frameBytes = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = frameBytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf; VK(vkCreateBuffer(v.dev, &bci, nullptr, &buf));
    vkGetBufferMemoryRequirements(v.dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = v.memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkDeviceMemory bufMem; VK(vkAllocateMemory(v.dev, &mai, nullptr, &bufMem)); vkBindBufferMemory(v.dev, buf, bufMem, 0);
    unsigned char *mapped; VK(vkMapMemory(v.dev, bufMem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = bufMem; range.size = VK_WHOLE_SIZE;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = v.qfi; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; VK(vkCreateCommandPool(v.dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(v.dev, &cai, &cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VK(vkCreateFence(v.dev, &fci, nullptr, &fence));

    // Stabilization: storage image (UNORM bytes, sRGB-encoded by the shader) and compute pipeline.
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage stabImg; VK(vkCreateImage(v.dev, &ici, nullptr, &stabImg));
    vkGetImageMemoryRequirements(v.dev, stabImg, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = v.memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory stabMem; VK(vkAllocateMemory(v.dev, &mai, nullptr, &stabMem)); vkBindImageMemory(v.dev, stabImg, stabMem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = stabImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView stabView; VK(vkCreateImageView(v.dev, &vci, nullptr, &stabView));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; VK(vkCreateSampler(v.dev, &sci, nullptr, &sampler));

    VkDescriptorSetLayoutBinding binds[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2; dlci.pBindings = binds;
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(v.dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(StabParams)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl; VK(vkCreatePipelineLayout(v.dev, &plci, nullptr, &pl));
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof kStabilizeSpv; smci.pCode = kStabilizeSpv;
    VkShaderModule sm; VK(vkCreateShaderModule(v.dev, &smci, nullptr, &sm));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpci.layout = pl;
    VkPipeline pipe; VK(vkCreateComputePipelines(v.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
    VkDescriptorPoolSize dps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = dps;
    VkDescriptorPool dpool; VK(vkCreateDescriptorPool(v.dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(v.dev, &dsai, &ds));
    std::map<VkImage, VkImageView> srcViews; // The compositor cycles through a few images.

    bool stabOn = readControl(control);
    float stabWeight = stabOn ? 1.0f : 0.0f; // Eases between off and on so a toggle does not jump.
    auto lastControlCheck = std::chrono::steady_clock::now();

    vr::VRVulkanDevice_t vdev{v.inst, v.dev, v.pd, v.q, v.qfi};
    vr::VRNativeDevice_t nd{&vdev, vr::DeviceType_Vulkan};
    vr::VROverlayView_t view{};

    std::vector<unsigned char> black(frameBytes, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;

    LOG("capturing %s at %ux%u %u fps, stabilization %s (zoom %.2f)\n", key, W, H, fps, stabOn ? "on" : "off", zoom);
    const auto period = std::chrono::nanoseconds(1000000000ull / fps);
    auto next = std::chrono::steady_clock::now();
    uint64_t frames = 0, empty = 0;
    auto lastReport = next;

    while (!g_stop) {
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) { LOG("SteamVR quit\n"); g_stop = 1; }
        if (g_stop) break;

        auto tnow = std::chrono::steady_clock::now();
        if (tnow - lastControlCheck > std::chrono::milliseconds(500)) {
            lastControlCheck = tnow;
            bool on = readControl(control);
            if (on != stabOn) LOG("stabilization %s\n", on ? "on" : "off");
            stabOn = on;
        }
        stabWeight = std::clamp(stabWeight + (stabOn ? 1.0f : -1.0f) * 3.0f / fps, 0.0f, 1.0f);

        const unsigned char *src = black.data();
        vr::EVROverlayError oerr = vr::VROverlayView()->AcquireOverlayView(ov, &nd, &view, sizeof view);
        if (oerr == vr::VROverlayError_None && view.texture.handle) {
            auto *vt = (vr::VRVulkanTextureData_t *)view.texture.handle;
            VkImage img = (VkImage)vt->m_nImage;
            const auto &b = view.textureBounds;
            int32_t x0 = (int32_t)lroundf(b.uMin * vt->m_nWidth), x1 = (int32_t)lroundf(b.uMax * vt->m_nWidth);
            int32_t y0 = (int32_t)lroundf(b.vMin * vt->m_nHeight), y1 = (int32_t)lroundf(b.vMax * vt->m_nHeight);

            float roll = 0, pitchWeight = 0;
            bool stabilize = stabWeight > 0.0f && hmdRoll(roll, pitchWeight);
            if (!std::isnan(testRoll)) { roll = testRoll; pitchWeight = 1; stabilize = stabWeight > 0.0f; }
            float angle = roll * pitchWeight * stabWeight;
            // Smoothstep the ease so switching looks deliberate; full zoom only when fully on.
            float ease = stabWeight * stabWeight * (3 - 2 * stabWeight);
            float curZoom = 1.0f + (zoom - 1.0f) * ease;

            VK(vkResetCommandBuffer(cb, 0));
            VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &cbi);
            VkImage readImg;

            if (stabilize) {
                auto it = srcViews.find(img);
                if (it == srcViews.end()) {
                    if (srcViews.size() > 8) { // Old images were freed; their handles may come back.
                        vkDeviceWaitIdle(v.dev);
                        for (auto &kv : srcViews) vkDestroyImageView(v.dev, kv.second, nullptr);
                        srcViews.clear();
                    }
                    VkImageViewCreateInfo svci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    svci.image = img; svci.viewType = VK_IMAGE_VIEW_TYPE_2D; svci.format = (VkFormat)vt->m_nFormat;
                    svci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkImageView sv; VK(vkCreateImageView(v.dev, &svci, nullptr, &sv));
                    it = srcViews.emplace(img, sv).first;
                }
                VkDescriptorImageInfo srcInfo{sampler, it->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, stabView, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                wr[0].dstSet = ds; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
                wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &srcInfo;
                wr[1].dstSet = ds; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
                wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &dstInfo;
                vkUpdateDescriptorSets(v.dev, 2, wr, 0, nullptr);

                VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; bar.image = stabImg;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

                // The head rolled left by `angle`, so the world looks rotated clockwise: turn it back counterclockwise.
                StabParams pc{{b.uMin, b.vMin}, {b.uMax, b.vMax}, {(float)W, (float)H},
                              cosf(angle), sinf(angle), curZoom, isSrgb(vt->m_nFormat) ? 1 : 0};
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
                vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cb, (W + 15) / 16, (H + 15) / 16, 1);

                bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
                readImg = stabImg;
            } else {
                VkImageMemoryBarrier bars[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
                bars[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bars[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                bars[0].image = img;
                bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bars[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bars[1].image = outImg;
                for (auto &bar : bars) {
                    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                }
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bars);

                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[0] = {x0, y0, 0}; blit.srcOffsets[1] = {x1, y1, 1};
                blit.dstOffsets[0] = {0, 0, 0}; blit.dstOffsets[1] = {(int32_t)W, (int32_t)H, 1};
                vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

                // Give the source back to the compositor in the layout it expects; prepare output for readback.
                bars[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bars[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bars[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bars[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bars[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bars[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bars[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 2, bars);
                readImg = outImg;
            }
            VkBufferImageCopy reg{}; reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; reg.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cb, readImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
            vkEndCommandBuffer(cb);

            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            VK(vkQueueSubmit(v.q, 1, &si, fence));
            VK(vkWaitForFences(v.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkResetFences(v.dev, 1, &fence);
            vkInvalidateMappedMemoryRanges(v.dev, 1, &range);
            src = mapped;
        } else {
            empty++;
        }

        // Write one full frame; ffmpeg reads at a constant rate, so every tick must produce a frame.
        size_t off = 0;
        while (off < frameBytes && !g_stop) {
            ssize_t n = write(STDOUT_FILENO, src + off, frameBytes - off);
            if (n <= 0) { g_stop = 1; break; }
            off += n;
        }
        frames++;

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(30)) {
            LOG("%llu frames, %llu without image\n", (unsigned long long)frames, (unsigned long long)empty);
            lastReport = now;
        }
        next += period;
        if (now > next + period * 5) next = now; // Fell far behind (e.g. suspend): resync instead of bursting.
        std::this_thread::sleep_until(next);
    }

    LOG("stopping after %llu frames\n", (unsigned long long)frames);
    vkDeviceWaitIdle(v.dev);
    for (auto &kv : srcViews) vkDestroyImageView(v.dev, kv.second, nullptr);
    vr::VROverlayView()->ReleaseOverlayView(&view);
    vr::VR_Shutdown();
    return 0;
}
