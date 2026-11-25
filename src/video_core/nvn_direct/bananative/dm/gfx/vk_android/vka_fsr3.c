// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Lichii: AMD FSR 3.1 in place of NVN2's DLSS evaluation (OP_DLSS, vka_dlss.c), as the normal renderer runs it (renderer_vulkan/vk_fsr3_upscaler.cpp, its
// HdrReversedDepth profile): the same SPIR-V (host_shaders), passes, internal images and constants, recorded into the replay's command buffer. Every image stays in
// GENERAL; descriptors are pushed. DM_VK_FSR3=0 keeps the spatial upscale; DM_VK_FSR3_SHARPEN=<stops> (default 0.25: the sharpness slider's default 25%), off: no RCAS
#if defined(__ANDROID__)
#include "vka.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "video_core/host_shaders/fsr3_upscaler_accumulate_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_accumulate_sharpen_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_luma_instability_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_luma_pyramid_nowave_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_prepare_inputs_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_prepare_reactivity_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_rcas_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_shading_change_comp_spv.h"
#include "video_core/host_shaders/fsr3_upscaler_shading_change_pyramid_nowave_comp_spv.h"

// cbFSR3Upscaler (std140, the SDK's Fsr3UpscalerConstants)
typedef struct {
    int32_t render_size[2], prev_render_size[2], upscale_size[2], prev_upscale_size[2], max_render_size[2], max_upscale_size[2];
    float device_to_view_depth[4], jitter[2], prev_jitter[2], mv_scale[2], downscale[2], mv_jitter_cancel[2];
    float tan_half_fov, jitter_phase_count, delta_time, delta_pre_exposure, view_space_to_meters, frame_index, velocity_factor, reactiveness_scale, shading_change_scale,
          accumulation_added_per_frame, min_disocclusion_accumulation;
} fsr_consts;
_Static_assert(sizeof(fsr_consts) == 148, "cbFSR3Upscaler");
typedef struct { uint32_t mips, num_work_groups, work_group_offset[2], render_size[2]; } spd_consts;

enum { T, I, U };   // the passes' descriptors in binding order (FSR3UPSCALER_BIND_*); bindings 1000 / 1001 are the point / linear clamp samplers
enum { P_PREP, P_LUMA, P_SCP, P_SC, P_REACT, P_INST, P_ACC, P_ACCS, P_RCAS, NPASS };
static const uint8_t K_PREP[] = {T, T, T, I, I, I, I, I, U}, K_LUMA[] = {T, T, I, I, I, I, I, I, I, I, I, U, U}, K_SCP[] = {T, T, T, T, I, I, I, I, I, I, I, U, U},
                     K_SC[] = {T, I, U}, K_REACT[] = {T, T, T, T, T, T, T, T, T, I, I, I, U}, K_INST[] = {T, T, T, T, T, T, T, I, I, U},
                     K_ACC[] = {T, T, T, T, T, T, T, T, T, I, I, I, U}, K_RCAS[] = {T, T, I, U, U};
static const struct { const uint8_t *k; int n; const uint32_t *spv; size_t size; } g_def[NPASS] = {
#define D(k, s) {k, sizeof k, s, sizeof s}
    D(K_PREP, FSR3_UPSCALER_PREPARE_INPUTS_COMP_SPV), D(K_LUMA, FSR3_UPSCALER_LUMA_PYRAMID_NOWAVE_COMP_SPV), D(K_SCP, FSR3_UPSCALER_SHADING_CHANGE_PYRAMID_NOWAVE_COMP_SPV),
    D(K_SC, FSR3_UPSCALER_SHADING_CHANGE_COMP_SPV), D(K_REACT, FSR3_UPSCALER_PREPARE_REACTIVITY_COMP_SPV), D(K_INST, FSR3_UPSCALER_LUMA_INSTABILITY_COMP_SPV),
    D(K_ACC, FSR3_UPSCALER_ACCUMULATE_COMP_SPV), D(K_ACC, FSR3_UPSCALER_ACCUMULATE_SHARPEN_COMP_SPV), D(K_RCAS, FSR3_UPSCALER_RCAS_COMP_SPV),
#undef D
};
static struct { VkDescriptorSetLayout dsl; VkPipelineLayout lay; VkPipeline p; } g_pass[NPASS];
static VkSampler g_samp[2];
static int g_state;   // 0 not made, 1 ready, -1 failed

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view, lv[16]; uint32_t levels; } fimg;
enum { SPD_MIPS = 6 };
// The images, as vk_fsr3_upscaler.cpp CreateResources makes them; old ones are kept to the teardown (shortcut: a size change leaks a set until then: the replay
// has no idle point to free them at; free them in vka_fsr3_destroy if sizes change often)
typedef struct fset {
    fimg acc[2], luma[2], luma_hist[2], up[2], far_depth, shading, locks, spd, far_mip1, dmv, ddepth, prev_depth, dreact, frame_info, spd_atomic, lut, def_react, out;
    uint32_t rw, rh, uw, uh; struct fset *old;
} fset;
static fset *g_set;
static struct { int32_t prev_rs[2], prev_us[2]; float prev_jitter[2], phases, frame_index; uint32_t parity; int first; double last_t; } S;
static vka_span g_ubo; static VkDeviceSize g_slot; static uint32_t g_ring;
enum { RING = 32 };   // evaluations whose constants may still be read (more than the replay's frames in flight)

static VkImageView mkview(VkImage im, VkFormat f, uint32_t base, uint32_t n) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, n, 0, 1}};
    VkImageView v = VK_NULL_HANDLE; return vk.vkCreateImageView(V.dev, &vi, NULL, &v) ? VK_NULL_HANDLE : v;
}
static int mkimg(fimg *t, VkFormat f, uint32_t w, uint32_t h, uint32_t levels) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w ? w : 1, h ? h : 1, 1}, .mipLevels = levels, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (f == VK_FORMAT_R16_SNORM ? 0 : VK_IMAGE_USAGE_STORAGE_BIT)};
    if (vk.vkCreateImage(V.dev, &ci, NULL, &t->img)) return 0;
    VkMemoryRequirements mr; vk.vkGetImageMemoryRequirements(V.dev, t->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &t->mem) || vk.vkBindImageMemory(V.dev, t->img, t->mem, 0)) return 0;
    t->levels = levels; t->view = mkview(t->img, f, 0, levels);
    if (levels > 1) for (uint32_t i = 0; i < levels && i < 16; i++) if (!(t->lv[i] = mkview(t->img, f, i, 1))) return 0;
    return t->view != VK_NULL_HANDLE;
}
static void rmimg(fimg *t) {
    for (uint32_t i = 0; i < 16; i++) if (t->lv[i]) vk.vkDestroyImageView(V.dev, t->lv[i], NULL);
    if (t->view) vk.vkDestroyImageView(V.dev, t->view, NULL);
    if (t->img) vk.vkDestroyImage(V.dev, t->img, NULL);
    if (t->mem) vk.vkFreeMemory(V.dev, t->mem, NULL);
}
#define EACH(s, X) do { for (int i_ = 0; i_ < 2; i_++) { X(&(s)->acc[i_]); X(&(s)->luma[i_]); X(&(s)->luma_hist[i_]); X(&(s)->up[i_]); } \
    X(&(s)->far_depth); X(&(s)->shading); X(&(s)->locks); X(&(s)->spd); X(&(s)->far_mip1); X(&(s)->dmv); X(&(s)->ddepth); X(&(s)->prev_depth); X(&(s)->dreact); \
    X(&(s)->frame_info); X(&(s)->spd_atomic); X(&(s)->lut); X(&(s)->def_react); X(&(s)->out); } while (0)

static fset *mkset(uint32_t rw, uint32_t rh, uint32_t uw, uint32_t uh) {
    fset *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->rw = rw; s->rh = rh; s->uw = uw; s->uh = uh;
    uint32_t spd_levels = (uint32_t)floorf(log2f((float)(rw / 2 > rh / 2 ? rw / 2 : rh / 2))) + 1; if (spd_levels < SPD_MIPS) spd_levels = SPD_MIPS;
    int ok = 1;
    for (int i = 0; i < 2; i++)
        ok &= mkimg(&s->acc[i], VK_FORMAT_R8_UNORM, rw, rh, 1) & mkimg(&s->luma[i], VK_FORMAT_R16_SFLOAT, rw, rh, 1) & mkimg(&s->luma_hist[i], VK_FORMAT_R8G8B8A8_UNORM, rw, rh, 1) &
              mkimg(&s->up[i], VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh, 1);
    ok &= mkimg(&s->far_depth, VK_FORMAT_R16_SFLOAT, rw, rh, 1) & mkimg(&s->shading, VK_FORMAT_R8_UNORM, rw / 2, rh / 2, 1) & mkimg(&s->locks, VK_FORMAT_R8_UNORM, uw, uh, 1) &
          mkimg(&s->spd, VK_FORMAT_R16G16_SFLOAT, rw / 2, rh / 2, spd_levels) & mkimg(&s->far_mip1, VK_FORMAT_R16_SFLOAT, rw / 2, rh / 2, 1) &
          mkimg(&s->dmv, VK_FORMAT_R16G16_SFLOAT, rw, rh, 1) & mkimg(&s->ddepth, VK_FORMAT_R32_SFLOAT, rw, rh, 1) & mkimg(&s->prev_depth, VK_FORMAT_R32_UINT, rw, rh, 1) &
          mkimg(&s->dreact, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, 1) & mkimg(&s->frame_info, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1) & mkimg(&s->spd_atomic, VK_FORMAT_R32_UINT, 1, 1, 1) &
          mkimg(&s->lut, VK_FORMAT_R16_SNORM, 128, 1, 1) & mkimg(&s->def_react, VK_FORMAT_R8_UNORM, rw, rh, 1) & mkimg(&s->out, VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh, 1);
    if (!ok) { EACH(s, rmimg); free(s); VRED_ONCE("FSR 3: images for %ux%u → %ux%u not made: the spatial upscale stands in", rw, rh, uw, uh); return NULL; }
    VLOG("FSR 3.1 in place of DLSS: %ux%u → %ux%u", rw, rh, uw, uh);
    return s;
}

static int ready(void) {
    if (g_state) return g_state > 0;
    g_state = -1;
    VkPhysicalDeviceFeatures f; vk.vkGetPhysicalDeviceFeatures(V.pd, &f);
    if (!V.push_desc || V.max_push < 15 || !f.shaderStorageImageExtendedFormats || !f.shaderStorageImageReadWithoutFormat || !f.shaderStorageImageWriteWithoutFormat) {
        VRED("FSR 3: the device lacks push descriptors (%u) or format-less storage images: the spatial upscale stands in", V.max_push); return 0; }
    const VkFormat fmts[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT};
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmts[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            VRED("FSR 3: format %d is not a storage image here: the spatial upscale stands in", fmts[i]); return 0; }
    }
    for (int i = 0; i < 2; i++) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .mipmapMode = i ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
        if (vk.vkCreateSampler(V.dev, &si, NULL, &g_samp[i])) return 0;
    }
    for (int p = 0; p < NPASS; p++) {
        VkDescriptorSetLayoutBinding b[16]; int n = g_def[p].n;
        for (int i = 0; i < n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, g_def[p].k[i] == T ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : g_def[p].k[i] == I ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        b[n] = (VkDescriptorSetLayoutBinding){1000, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_samp[0]};
        b[n + 1] = (VkDescriptorSetLayoutBinding){1001, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_samp[1]};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                              .bindingCount = (uint32_t)n + 2, .pBindings = b};
        if (vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_pass[p].dsl)) { VRED("FSR 3: pass %d's set layout not made: the spatial upscale stands in", p); return 0; }
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_pass[p].dsl};
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_def[p].size, .pCode = g_def[p].spv};
        VkShaderModule m = VK_NULL_HANDLE;
        if (vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_pass[p].lay) || vk.vkCreateShaderModule(V.dev, &mi, NULL, &m)) return 0;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_pass[p].lay};
        VkResult r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_pass[p].p);
        vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (r) { VRED("FSR 3: pass %d's pipeline: %d: the spatial upscale stands in", p, r); return 0; }
    }
    VkDeviceSize a = V.props.limits.minUniformBufferOffsetAlignment; if (a < 16) a = 16;
    g_slot = (sizeof(fsr_consts) + a - 1) / a * a;   // per evaluation: the FSR, SPD and RCAS constants, each in a slot, then the Lanczos table (the first evaluation's)
    if (!vka_static(g_slot * 3 * RING + 256, a, &g_ubo)) return 0;
    g_state = 1;
    return 1;
}

typedef struct { VkImageView v; VkDeviceSize off; } res;   // a view, or (v 0) a slot of this evaluation's constants
static void dispatch(VkCommandBuffer cb, int p, const res *r, uint32_t gx, uint32_t gy) {
    VkWriteDescriptorSet w[18]; VkDescriptorImageInfo ii[18]; VkDescriptorBufferInfo bi[16]; int n = g_def[p].n;
    for (int i = 0; i < n; i++) {
        const int k = g_def[p].k[i];
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                      .descriptorType = k == T ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : k == I ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        if (k == U) { bi[i] = (VkDescriptorBufferInfo){g_ubo.buf, r[i].off, g_slot}; w[i].pBufferInfo = &bi[i]; }
        else { ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, r[i].v, VK_IMAGE_LAYOUT_GENERAL}; w[i].pImageInfo = &ii[i]; }
    }
    for (int i = 0; i < 2; i++) {   // the immutable samplers' bindings are pushed too (each binding of a push set must be written before use)
        ii[n + i] = (VkDescriptorImageInfo){g_samp[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[n + i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1000u + (uint32_t)i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                          .pImageInfo = &ii[n + i]};
    }
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pass[p].p);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pass[p].lay, 0, (uint32_t)n + 2, w);
    vk.vkCmdDispatch(cb, gx, gy, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static uint16_t half(float f) {   // (normal values only: RCAS's sharpness, 0.25 to 1)
    uint32_t x; memcpy(&x, &f, 4);
    return (uint16_t)(((x >> 16) & 0x8000) | ((((x >> 23) & 0xff) - 112) << 10) | ((x >> 13) & 0x3ff));
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// One evaluation: colour, depth and motion (render size rw x rh at 0,0 of their views) into the output, RGBA16F uw x uh in GENERAL, ready for a fragment shader;
// jitter in render pixels (input texel k sampled k + 0.5 - jitter), mv_scale: a motion vector times it is in render pixels. NULL: not evaluated
VkImageView vka_fsr3_eval(VkCommandBuffer cb, VkImageView color, VkImageView depth, VkImageView motion, uint32_t rw, uint32_t rh, uint32_t uw, uint32_t uh,
                          const float jitter[2], const float mv_scale[2], int reset) {
    if (!VKA_ENV("DM_VK_FSR3", 1) || !ready() || rw < 2 || rh < 2) return VK_NULL_HANDLE;
    if (!g_set || g_set->rw != rw || g_set->rh != rh || g_set->uw != uw || g_set->uh != uh) {
        fset **pp = &g_set, *s = NULL; int n = 0;
        for (; *pp; pp = &(*pp)->old, n++) if ((*pp)->rw == rw && (*pp)->rh == rh && (*pp)->uw == uw && (*pp)->uh == uh) { s = *pp; *pp = s->old; break; }
        if (!s && n >= 8) { VRED_ONCE("FSR 3: 8 input sizes made: the spatial upscale stands in for new ones"); return VK_NULL_HANDLE; }
        if (!s && !(s = mkset(rw, rh, uw, uh))) return VK_NULL_HANDLE;
        s->old = g_set; g_set = s; S.first = 1; S.parity = 0;   // (a set taken back starts again too: its history is another size's past)
        VLOG("FSR 3: motion vector scale %g,%g (render pixels per unit), jitter %.3f,%.3f", mv_scale[0], mv_scale[1], jitter[0], jitter[1]);
    }
    fset *s = g_set;
    const int first = S.first, rs = reset || first; S.first = 0;
    const double t = now_ms(); const float dt = first || S.last_t == 0 ? 16.6f : (float)(t - S.last_t); S.last_t = t;

    fsr_consts c = {.render_size = {(int32_t)rw, (int32_t)rh}, .upscale_size = {(int32_t)uw, (int32_t)uh}};
    memcpy(c.prev_render_size, first ? c.render_size : S.prev_rs, 8); memcpy(c.prev_upscale_size, first ? c.upscale_size : S.prev_us, 8);
    memcpy(c.max_render_size, c.render_size, 8); memcpy(c.max_upscale_size, c.upscale_size, 8);
    const float fov = 1.04719755f, aspect = (float)rw / (float)rh, cot = cosf(0.5f * fov) / sinf(0.5f * fov);   // DLSS takes no camera: 60 degrees, near 0.1 (as the normal renderer)
    c.device_to_view_depth[0] = -FLT_EPSILON; c.device_to_view_depth[1] = 0.1f; c.device_to_view_depth[2] = 1.0f / (cot / aspect); c.device_to_view_depth[3] = 1.0f / cot;
    c.tan_half_fov = tanf(atanf(tanf(fov * 0.5f) * aspect));
    memcpy(c.jitter, jitter, 8); memcpy(c.prev_jitter, first ? jitter : S.prev_jitter, 8);
    c.mv_scale[0] = mv_scale[0] / (float)rw; c.mv_scale[1] = mv_scale[1] / (float)rh;
    c.downscale[0] = (float)rw / (float)uw; c.downscale[1] = (float)rh / (float)uh;
    const float ratio = (float)uw / (float)rw, phases = (float)(int32_t)(8.0f * ratio * ratio);
    S.phases = rs || S.phases == 0 ? phases : phases > S.phases ? S.phases + 1 : phases < S.phases ? S.phases - 1 : S.phases;
    c.jitter_phase_count = S.phases;
    c.delta_time = dt / 1000.0f > 1 ? 1 : dt < 0 ? 0 : dt / 1000.0f;
    c.delta_pre_exposure = 1; c.view_space_to_meters = 1;
    S.frame_index = rs ? 0 : S.frame_index + 1; c.frame_index = S.frame_index;
    c.velocity_factor = 1; c.reactiveness_scale = 1; c.shading_change_scale = 1; c.accumulation_added_per_frame = 1.0f / 3.0f; c.min_disocclusion_accumulation = -1.0f / 3.0f;
    memcpy(S.prev_rs, c.render_size, 8); memcpy(S.prev_us, c.upscale_size, 8); memcpy(S.prev_jitter, jitter, 8);

    const uint32_t sgx = (rw - 1) / 64 + 1, sgy = (rh - 1) / 64 + 1;
    spd_consts spd = {.mips = (uint32_t)fminf(floorf(log2f((float)(rw > rh ? rw : rh))), 12.0f), .num_work_groups = sgx * sgy, .render_size = {rw, rh}};
    static int sharpen = -1; static float stops = 0.25f;
    if (sharpen < 0) { const char *e = getenv("DM_VK_FSR3_SHARPEN"); sharpen = !(e && !strcmp(e, "off")); if (e && sharpen) stops = (float)atof(e); }
    float sharp = 1.0f - stops / 2.0f; sharp = sharp < 0 ? 0 : sharp > 1 ? 1 : sharp;
    const float rf = exp2f(-(2.0f - 2.0f * sharp)); const uint16_t h = half(rf); uint32_t rcas[4];
    memcpy(&rcas[0], &rf, 4); rcas[1] = (uint32_t)h | (uint32_t)h << 16; rcas[2] = rcas[3] = 0;
    const VkDeviceSize base = g_ubo.off + (VkDeviceSize)(g_ring++ % RING) * g_slot * 3, lut_off = g_ubo.off + g_slot * 3 * RING;
    uint8_t *m = g_ubo.ptr + (base - g_ubo.off);
    memcpy(m, &c, sizeof c); memcpy(m + g_slot, &spd, sizeof spd); memcpy(m + 2 * g_slot, rcas, sizeof rcas);
    const res cb_fsr = {0, base}, cb_spd = {0, base + g_slot}, cb_rcas = {0, base + 2 * g_slot};

    // The inputs come from the game's draws; ours from the last evaluation
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    const VkClearColorValue zero = {{0}};
    const uint32_t p = S.parity, q = p ^ 1;
    if (first) {
        VkImageMemoryBarrier ib[32]; int n = 0;
#define TOGEN(t) (ib[n++] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, \
                  .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = (t)->img, .subresourceRange = all})
        EACH(s, TOGEN);
#undef TOGEN
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)n, ib);
        int16_t wts[128];   // FSR's Lanczos-2 table (fsr3upscalerCreate)
        for (int i = 0; i < 128; i++) { const float x = 2.0f * i / 127.0f, px = 3.14159265f * x; const float y = fabsf(x) < FLT_EPSILON ? 1.0f : sinf(px) / px * (sinf(0.5f * px) / (0.5f * px));
                                        wts[i] = (int16_t)lrintf(y * 32767.0f); }
        memcpy(g_ubo.ptr + (lut_off - g_ubo.off), wts, sizeof wts);
        VkBufferImageCopy bc = {.bufferOffset = lut_off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {128, 1, 1}};
        vk.vkCmdCopyBufferToImage(cb, g_ubo.buf, s->lut.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
#define CLR(t) do { if ((t) != &s->lut && (t) != &s->frame_info) vk.vkCmdClearColorImage(cb, (t)->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all); } while (0)
        EACH(s, CLR);
#undef CLR
    } else {
        vk.vkCmdClearColorImage(cb, s->spd_atomic.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        vk.vkCmdClearColorImage(cb, s->spd.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        if (rs) vk.vkCmdClearColorImage(cb, s->acc[p].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    vk.vkCmdClearColorImage(cb, s->prev_depth.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);   // reversed depth: 0 is the farthest
    if (rs) { const VkClearColorValue fi = {.float32 = {-1, 1, 0, 0}}; vk.vkCmdClearColorImage(cb, s->frame_info.img, VK_IMAGE_LAYOUT_GENERAL, &fi, 1, &all); }
    VkMemoryBarrier tb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &tb, 0, NULL, 0, NULL);

#define V_(t) ((res){(t).view, 0})
#define MIP(l) ((res){s->spd.lv[(l) < s->spd.levels ? (l) : s->spd.levels - 1], 0})
    const res col = {color, 0}, dep = {depth, 0}, mot = {motion, 0}, exposure = V_(s->frame_info), luma = V_(s->luma[p]);
    const uint32_t g8x = (rw + 7) / 8, g8y = (rh + 7) / 8;
    { const res r[] = {mot, dep, col, V_(s->dmv), V_(s->ddepth), V_(s->prev_depth), V_(s->far_depth), luma, cb_fsr}; dispatch(cb, P_PREP, r, g8x, g8y); }
    { const res r[] = {luma, V_(s->far_depth), V_(s->spd_atomic), V_(s->frame_info), MIP(0), MIP(1), MIP(2), MIP(3), MIP(4), MIP(5), V_(s->far_mip1), cb_fsr, cb_spd};
      dispatch(cb, P_LUMA, r, sgx, sgy); }
    { const res r[] = {luma, V_(s->luma[q]), V_(s->dmv), exposure, V_(s->spd_atomic), MIP(0), MIP(1), MIP(2), MIP(3), MIP(4), MIP(5), cb_fsr, cb_spd}; dispatch(cb, P_SCP, r, sgx, sgy); }
    { const res r[] = {V_(s->spd), V_(s->shading), cb_fsr}; dispatch(cb, P_SC, r, (rw / 2 + 7) / 8, (rh / 2 + 7) / 8); }
    { const res r[] = {V_(s->prev_depth), V_(s->dmv), V_(s->ddepth), V_(s->def_react), V_(s->def_react), V_(s->acc[p]), V_(s->shading), luma, exposure, V_(s->dreact), V_(s->locks),
                       V_(s->acc[q]), cb_fsr}; dispatch(cb, P_REACT, r, g8x, g8y); }
    { const res r[] = {exposure, V_(s->dreact), V_(s->dmv), V_(s->frame_info), V_(s->luma_hist[p]), V_(s->far_mip1), luma, V_(s->luma_hist[q]), V_(s->far_depth), cb_fsr};
      dispatch(cb, P_INST, r, g8x, g8y); }
    { const res r[] = {exposure, V_(s->dreact), V_(s->dmv), V_(s->up[p]), V_(s->lut), V_(s->far_mip1), luma, V_(s->far_depth), col, V_(s->up[q]), V_(s->out), V_(s->locks), cb_fsr};
      dispatch(cb, sharpen ? P_ACCS : P_ACC, r, (uw + 7) / 8, (uh + 7) / 8); }
    if (sharpen) { const res r[] = {exposure, V_(s->up[q]), V_(s->out), cb_fsr, cb_rcas}; dispatch(cb, P_RCAS, r, (uw + 15) / 16, (uh + 15) / 16); }
#undef V_
#undef MIP
    VkMemoryBarrier ob = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &ob, 0, NULL, 0, NULL);
    S.parity = q;
    vka_dyn_invalidate();   // (the compute binds and pushes disturb the replay's own)
    return s->out.view;
}

void vka_fsr3_destroy(void) {
    for (fset *s = g_set, *o; s; s = o) { o = s->old; EACH(s, rmimg); free(s); }
    g_set = NULL;
    for (int p = 0; p < NPASS; p++) {
        if (g_pass[p].p) vk.vkDestroyPipeline(V.dev, g_pass[p].p, NULL);
        if (g_pass[p].lay) vk.vkDestroyPipelineLayout(V.dev, g_pass[p].lay, NULL);
        if (g_pass[p].dsl) vk.vkDestroyDescriptorSetLayout(V.dev, g_pass[p].dsl, NULL);
    }
    for (int i = 0; i < 2; i++) if (g_samp[i]) vk.vkDestroySampler(V.dev, g_samp[i], NULL);
    memset(g_pass, 0, sizeof g_pass); memset(g_samp, 0, sizeof g_samp); g_state = 0;
}
#endif
