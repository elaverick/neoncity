#include "renderer.h"
#include "textures.h"

/* SPIR-V is compiled at build time and embedded as word lists. */
static const uint32_t SPV_city_vert[] = {
#include "city.vert.inc"
};
static const uint32_t SPV_city_frag[] = {
#include "city.frag.inc"
};
static const uint32_t SPV_fullscreen_vert[] = {
#include "fullscreen.vert.inc"
};
static const uint32_t SPV_sky_frag[] = {
#include "sky.frag.inc"
};
static const uint32_t SPV_composite_frag[] = {
#include "composite.frag.inc"
};
static const uint32_t SPV_bloom_down[] = {
#include "bloom_down.comp.inc"
};
static const uint32_t SPV_bloom_up[] = {
#include "bloom_up.comp.inc"
};
static const uint32_t SPV_cloud_noise[] = {
#include "cloud_noise.comp.inc"
};
static const uint32_t SPV_clouds[] = {
#include "clouds.comp.inc"
};
static const uint32_t SPV_cloud_composite_frag[] = {
#include "cloud_composite.frag.inc"
};
static const uint32_t SPV_ssr[] = {
#include "ssr.comp.inc"
};
static const uint32_t SPV_ssr_apply_frag[] = {
#include "ssr_apply.frag.inc"
};
static const uint32_t SPV_shelter_vert[] = {
#include "shelter.vert.inc"
};
static const uint32_t SPV_shelter_frag[] = {
#include "shelter.frag.inc"
};

#define HDR_FORMAT   VK_FORMAT_R16G16B16A16_SFLOAT
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define BLOOM_MIPS   7
#define CLOUD_NOISE_SIZE 128   /* cloud noise volume edge (texels) */

typedef struct BloomPush { float threshold, knee, radius; int prefilter; } BloomPush;
/* right/up/fwd: the camera ray basis (as FrameUBO.rayBasis); right.w = near
 * plane, up.w = far plane, fwd.w = rain intensity (composite.frag draws the rain). */
typedef struct CompositePush {
    float time, bloom, exposure, grain;
    float width, height, aberration, vignette;
    float right[4], up[4], fwd[4];
    float shelter[4];   /* as FrameUBO.shelter */
} CompositePush;
typedef struct CloudPush { float zn, zf; } CloudPush;

struct Renderer {
    VkCore      *vk;
    ChunkSystem *chunks;
    uint64_t     frameIndex;

    VkCommandPool   cmdPool;
    VkCommandBuffer cmd[MAX_FRAMES];
    VkFence         fence[MAX_FRAMES];
    VkSemaphore     imageReady[MAX_FRAMES];
    Buffer          ubo[MAX_FRAMES];
    Buffer          skyBuf[MAX_FRAMES];   /* SkyGrid: signs lighting the clouds */

    /* size-dependent targets. depthResolve is the single-sample copy of the MSAA
     * depth the clouds read (without MSAA they read depth itself); cloud and
     * cloudDist are the half-resolution cloud march (clouds.comp). */
    Image        msaaColor, depth, depthResolve, hdr, bloom, cloud, cloudDist, mist;   /* mist: rain mist (clouds.comp) */
    /* reflection G-buffer (city.frag outRefl; reflMsaa is the multisampled
     * attachment resolved into refl) and the screen-space reflections */
    Image        reflMsaa, refl, ssr;
    VkImageView  bloomMip[BLOOM_MIPS];
    uint32_t     bloomMips;
    VkDescriptorPool targetPool;
    VkDescriptorSet  bloomDownSet[BLOOM_MIPS], bloomUpSet[BLOOM_MIPS], compositeSet;
    VkDescriptorSet  cloudSet[MAX_FRAMES], cloudCompSet;
    VkDescriptorSet  ssrSet[MAX_FRAMES], ssrApplySet[MAX_FRAMES];

    VkSampler    linearClamp, nearestClamp, linearRepeat;
    Textures    *textures;
    Image        cloudNoise;
    Image        shelter;   /* shelter map: top-down depth of the city round the camera (record_shelter) */
    VkDescriptorPool      staticPool;
    VkDescriptorSetLayout sceneLayout, texLayout, bloomLayout, cloudLayout, cloudCompLayout, ssrLayout, ssrApplyLayout;
    VkDescriptorSet       sceneSet[MAX_FRAMES];
    VkPipelineLayout      scenePL, compositePL, bloomPL, cloudPL, cloudCompPL, ssrPL, ssrApplyPL, shelterPL;
    VkPipeline            cityPipe, skyPipe, compositePipe, bloomDownPipe, bloomUpPipe, cloudPipe, cloudCompPipe, ssrPipe, ssrApplyPipe,
                          shelterPipe;

    char   shotPath[512];
    bool   shotPending;
    Buffer shotBuf;
};

uint64_t renderer_frame_index(Renderer *r) { return r->frameIndex; }

void renderer_request_screenshot(Renderer *r, const char *path) {
    snprintf(r->shotPath, sizeof(r->shotPath), "%s", path);
    r->shotPending = true;
}

static void write_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h, bool bgr) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16);
    memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &planesBpp, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)calloc(row, 1);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = px + (size_t)(h - 1 - y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            line[x * 3 + 0] = bgr ? src[x * 4 + 0] : src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = bgr ? src[x * 4 + 2] : src[x * 4 + 0];
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    printf("Saved screenshot %s\n", path);
}

/* ------------------------------------------------------------ layouts --- */

static VkDescriptorSetLayout make_set_layout(VkCore *vk, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = n;
    ci.pBindings = b;
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &l));
    return l;
}

static VkPipelineLayout make_pipeline_layout(VkCore *vk, VkDescriptorSetLayout set, uint32_t pushSize, VkShaderStageFlags pushStages) {
    VkPushConstantRange pr = { pushStages, 0, pushSize };
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = pushSize ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(vk->device, &ci, NULL, &l));
    return l;
}

static void write_tex_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                          VkSampler s, VkImageView v, VkImageLayout layout);

static void create_layouts(Renderer *r) {
    VkCore *vk = r->vk;
    VkDescriptorSetLayoutBinding scene[5] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shelter map */
    };
    r->sceneLayout = make_set_layout(vk, scene, 5);
    VkDescriptorSetLayoutBinding tex[5] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* scene depth (rain) */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* rain mist (rain lighting) */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shelter map (rain) */
    };
    r->texLayout = make_set_layout(vk, tex, 5);
    VkDescriptorSetLayoutBinding bloom[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    r->bloomLayout = make_set_layout(vk, bloom, 2);
    VkDescriptorSetLayoutBinding cloud[7] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },           /* sky lights */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* noise volume */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* scene depth */
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* cloud */
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* cloudDist */
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* mist */
    };
    r->cloudLayout = make_set_layout(vk, cloud, 7);
    VkDescriptorSetLayoutBinding cloudComp[4] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    r->cloudCompLayout = make_set_layout(vk, cloudComp, 4);
    VkDescriptorSetLayoutBinding ssr[5] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* HDR scene */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* scene depth */
        { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* reflection G-buffer */
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* ssr */
    };
    r->ssrLayout = make_set_layout(vk, ssr, 5);
    VkDescriptorSetLayoutBinding ssrApply[3] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },  /* ssr */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },  /* reflection G-buffer */
    };
    r->ssrApplyLayout = make_set_layout(vk, ssrApply, 3);

    r->scenePL = make_pipeline_layout(vk, r->sceneLayout, 0, 0);
    r->compositePL = make_pipeline_layout(vk, r->texLayout, sizeof(CompositePush), VK_SHADER_STAGE_FRAGMENT_BIT);
    r->bloomPL = make_pipeline_layout(vk, r->bloomLayout, sizeof(BloomPush), VK_SHADER_STAGE_COMPUTE_BIT);
    r->cloudPL = make_pipeline_layout(vk, r->cloudLayout, 0, 0);
    r->cloudCompPL = make_pipeline_layout(vk, r->cloudCompLayout, sizeof(CloudPush), VK_SHADER_STAGE_FRAGMENT_BIT);
    r->ssrPL = make_pipeline_layout(vk, r->ssrLayout, 0, 0);
    r->ssrApplyPL = make_pipeline_layout(vk, r->ssrApplyLayout, 0, 0);
    r->shelterPL = make_pipeline_layout(vk, r->sceneLayout, sizeof(mat4), VK_SHADER_STAGE_VERTEX_BIT);

    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (PBR_TEXTURE_COUNT + 1) * MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &r->staticPool));

    VkDescriptorImageInfo maps[PBR_TEXTURE_COUNT];
    textures_descriptors(r->textures, maps);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r->staticPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->sceneLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &r->sceneSet[i]));
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkDescriptorBufferInfo sb = { chunks_gpu_buffer(r->chunks, i), 0, chunks_gpu_buffer_size() };
        VkDescriptorBufferInfo lb = { chunks_light_buffer(r->chunks), 0, chunks_light_buffer_size() };
        VkWriteDescriptorSet w[4] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        w[0].dstSet = r->sceneSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        w[1].dstSet = r->sceneSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &sb;
        w[2].dstSet = r->sceneSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &lb;
        w[3].dstSet = r->sceneSet[i]; w[3].dstBinding = 3; w[3].descriptorCount = PBR_TEXTURE_COUNT;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = maps;
        vkUpdateDescriptorSets(vk->device, 4, w, 0, NULL);
    }

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(vk->device, &si, NULL, &r->linearClamp));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;   /* depth and exact texel reads */
    VK_CHECK(vkCreateSampler(vk->device, &si, NULL, &r->nearestClamp));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;    /* the tiling cloud noise volume */
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK_CHECK(vkCreateSampler(vk->device, &si, NULL, &r->linearRepeat));

    for (int i = 0; i < MAX_FRAMES; ++i)
        write_tex_set(vk, r->sceneSet[i], 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, r->nearestClamp, r->shelter.view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* ---------------------------------------------------------- pipelines --- */

typedef struct GfxDesc {
    const uint32_t *vs; size_t vsBytes;
    const uint32_t *fs; size_t fsBytes;
    VkPipelineLayout layout;
    VkFormat colorFormat;
    VkFormat depthFormat;
    VkSampleCountFlagBits samples;
    bool vertexInput, depthTest, depthWrite, cull;
    bool blend;   /* out = src.rgb + dst.rgb * src.a (premultiplied transmittance) */
    /* colorFormat UNDEFINED: depth only */
    bool additive;           /* out = src.rgb + dst.rgb */
    VkFormat colorFormat2;   /* a second colour attachment (UNDEFINED: none), written unblended */
    bool positionOnly;       /* the vertex shader reads only the position attribute */
} GfxDesc;

static VkPipeline make_graphics(VkCore *vk, const GfxDesc *d) {
    VkShaderModule vs = vk_shader(vk, d->vs, d->vsBytes), fs = vk_shader(vk, d->fs, d->fsBytes);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkVertexInputBindingDescription bind = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[6] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) },
        { 1, 0, VK_FORMAT_R8G8B8A8_SNORM,   offsetof(Vertex, nrm) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv) },
        { 3, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, seed) },
        { 4, 0, VK_FORMAT_R8G8B8A8_UINT,    offsetof(Vertex, params) },
        { 5, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, aux) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (d->vertexInput) {
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = d->positionOnly ? 1 : 6; vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = d->samples;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = d->depthTest;
    ds.depthWriteEnable = d->depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   /* reverse-Z */
    VkPipelineColorBlendAttachmentState cba[2] = { { 0 }, { 0 } };
    cba[0].colorWriteMask = cba[1].colorWriteMask = 0xF;
    if (d->blend || d->additive) {
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = d->additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    uint32_t ncolor = d->colorFormat == VK_FORMAT_UNDEFINED ? 0 : d->colorFormat2 != VK_FORMAT_UNDEFINED ? 2 : 1;
    VkFormat formats[2] = { d->colorFormat, d->colorFormat2 };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = ncolor; cb.pAttachments = cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = d->depthFormat;

    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.pNext = &ri;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d->layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    vkDestroyShaderModule(vk->device, vs, NULL);
    vkDestroyShaderModule(vk->device, fs, NULL);
    return p;
}

static VkPipeline make_compute(VkCore *vk, const uint32_t *code, size_t bytes, VkPipelineLayout layout) {
    VkShaderModule cs = vk_shader(vk, code, bytes);
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    vkDestroyShaderModule(vk->device, cs, NULL);
    return p;
}

static void create_pipelines(Renderer *r) {
    VkCore *vk = r->vk;
    GfxDesc city = { SPV_city_vert, sizeof(SPV_city_vert), SPV_city_frag, sizeof(SPV_city_frag),
                     r->scenePL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, true, true, true, true };
    city.colorFormat2 = HDR_FORMAT;   /* reflection G-buffer */
    r->cityPipe = make_graphics(vk, &city);
    GfxDesc sky = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_sky_frag, sizeof(SPV_sky_frag),
                    r->scenePL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, false, false };
    sky.colorFormat2 = HDR_FORMAT;
    r->skyPipe = make_graphics(vk, &sky);
    GfxDesc comp = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_composite_frag, sizeof(SPV_composite_frag),
                     r->compositePL, vk->swapFormat, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false };
    r->compositePipe = make_graphics(vk, &comp);
    r->bloomDownPipe = make_compute(vk, SPV_bloom_down, sizeof(SPV_bloom_down), r->bloomPL);
    r->bloomUpPipe = make_compute(vk, SPV_bloom_up, sizeof(SPV_bloom_up), r->bloomPL);
    r->cloudPipe = make_compute(vk, SPV_clouds, sizeof(SPV_clouds), r->cloudPL);
    GfxDesc cc = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_cloud_composite_frag, sizeof(SPV_cloud_composite_frag),
                   r->cloudCompPL, HDR_FORMAT, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false, true };
    r->cloudCompPipe = make_graphics(vk, &cc);
    r->ssrPipe = make_compute(vk, SPV_ssr, sizeof(SPV_ssr), r->ssrPL);
    GfxDesc sa = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_ssr_apply_frag, sizeof(SPV_ssr_apply_frag),
                   r->ssrApplyPL, HDR_FORMAT, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false };
    sa.additive = true;
    r->ssrApplyPipe = make_graphics(vk, &sa);
    GfxDesc sh = { SPV_shelter_vert, sizeof(SPV_shelter_vert), SPV_shelter_frag, sizeof(SPV_shelter_frag),
                   r->shelterPL, VK_FORMAT_UNDEFINED, DEPTH_FORMAT, VK_SAMPLE_COUNT_1_BIT, true, true, true, false };
    sh.positionOnly = true;
    r->shelterPipe = make_graphics(vk, &sh);
}

/* ------------------------------------------------------------ targets --- */

static void write_tex_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                          VkSampler s, VkImageView v, VkImageLayout layout) {
    VkDescriptorImageInfo ii = { s, v, layout };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
    w.descriptorType = type; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

static VkDescriptorSet alloc_set(Renderer *r, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = r->targetPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet s;
    VK_CHECK(vkAllocateDescriptorSets(r->vk->device, &ai, &s));
    return s;
}

/* The cloud noise volume (shaders/cloud_noise.comp), built once on the GPU. */
static void build_cloud_noise(Renderer *r) {
    VkCore *vk = r->vk;
    r->cloudNoise = vk_image_3d(vk, CLOUD_NOISE_SIZE, CLOUD_NOISE_SIZE, CLOUD_NOISE_SIZE, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayout layout = make_set_layout(vk, &b, 1);
    VkPipelineLayout pl = make_pipeline_layout(vk, layout, 0, 0);
    VkPipeline pipe = make_compute(vk, SPV_cloud_noise, sizeof(SPV_cloud_noise), pl);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set;
    VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &set));
    write_tex_set(vk, set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, r->cloudNoise.view, VK_IMAGE_LAYOUT_GENERAL);

    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_image_barrier(cmd, r->cloudNoise.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, CLOUD_NOISE_SIZE / 4, CLOUD_NOISE_SIZE / 4, CLOUD_NOISE_SIZE / 4);
    vk_image_barrier(cmd, r->cloudNoise.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkFreeCommandBuffers(vk->device, r->cmdPool, 1, &cmd);
    vkDestroyPipeline(vk->device, pipe, NULL);
    vkDestroyPipelineLayout(vk->device, pl, NULL);
    vkDestroyDescriptorSetLayout(vk->device, layout, NULL);
    vkDestroyDescriptorPool(vk->device, pool, NULL);
}

static void create_targets(Renderer *r) {
    VkCore *vk = r->vk;
    uint32_t w = vk->extent.width, h = vk->extent.height;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    if (msaa)
        r->msaaColor = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                vk->msaa, VK_IMAGE_ASPECT_COLOR_BIT);
    /* The clouds read scene depth: a sample-zero resolve of the MSAA depth, or the depth itself. */
    r->depth = vk_image(vk, w, h, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        (msaa ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT),
                        vk->msaa, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (msaa)
        r->depthResolve = vk_image(vk, w, h, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView sceneDepth = msaa ? r->depthResolve.view : r->depth.view;
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    r->cloud = vk_image(vk, cw, ch, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->cloudDist = vk_image(vk, cw, ch, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->mist = vk_image(vk, cw, ch, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (msaa)
        r->reflMsaa = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                               vk->msaa, VK_IMAGE_ASPECT_COLOR_BIT);
    r->refl = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->ssr = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    r->hdr = vk_image(vk, w, h, 1, HDR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    uint32_t bw = w / 2 > 1 ? w / 2 : 1, bh = h / 2 > 1 ? h / 2 : 1;
    r->bloomMips = 1;
    while (r->bloomMips < BLOOM_MIPS && (bw >> r->bloomMips) >= 4 && (bh >> r->bloomMips) >= 4) r->bloomMips++;
    r->bloom = vk_image(vk, bw, bh, r->bloomMips, HDR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    for (uint32_t i = 0; i < r->bloomMips; ++i)
        r->bloomMip[i] = vk_view(vk, r->bloom.img, HDR_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, i, 1);

    VkDescriptorPoolSize sizes[4] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * BLOOM_MIPS + 7 + 2 * MAX_FRAMES + 4 + 5 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * BLOOM_MIPS + 3 * MAX_FRAMES + MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2 * BLOOM_MIPS + 2 + MAX_FRAMES + 1 + 2 * MAX_FRAMES;
    pci.poolSizeCount = 4;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &r->targetPool));

    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (uint32_t i = 0; i < r->bloomMips; ++i) {
        r->bloomDownSet[i] = alloc_set(r, r->bloomLayout);
        if (i == 0) write_tex_set(vk, r->bloomDownSet[i], 0, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else        write_tex_set(vk, r->bloomDownSet[i], 0, CIS, r->linearClamp, r->bloomMip[i - 1], VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->bloomDownSet[i], 1, SI, VK_NULL_HANDLE, r->bloomMip[i], VK_IMAGE_LAYOUT_GENERAL);
    }
    for (uint32_t i = 0; i + 1 < r->bloomMips; ++i) {
        r->bloomUpSet[i] = alloc_set(r, r->bloomLayout);
        write_tex_set(vk, r->bloomUpSet[i], 0, CIS, r->linearClamp, r->bloomMip[i + 1], VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->bloomUpSet[i], 1, SI, VK_NULL_HANDLE, r->bloomMip[i], VK_IMAGE_LAYOUT_GENERAL);
    }
    r->compositeSet = alloc_set(r, r->texLayout);
    write_tex_set(vk, r->compositeSet, 0, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->compositeSet, 1, CIS, r->linearClamp, r->bloomMip[0], VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->compositeSet, 2, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->compositeSet, 3, CIS, r->linearClamp, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->compositeSet, 4, CIS, r->nearestClamp, r->shelter.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    for (int i = 0; i < MAX_FRAMES; ++i) {
        r->cloudSet[i] = alloc_set(r, r->cloudLayout);
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkDescriptorBufferInfo sb = { r->skyBuf[i].buf, 0, sizeof(SkyGrid) };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        w[0].dstSet = r->cloudSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        w[1].dstSet = r->cloudSet[i]; w[1].dstBinding = 7; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &sb;
        vkUpdateDescriptorSets(vk->device, 2, w, 0, NULL);
        write_tex_set(vk, r->cloudSet[i], 3, CIS, r->linearRepeat, r->cloudNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->cloudSet[i], 4, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->cloudSet[i], 5, SI, VK_NULL_HANDLE, r->cloud.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->cloudSet[i], 6, SI, VK_NULL_HANDLE, r->cloudDist.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->cloudSet[i], 8, SI, VK_NULL_HANDLE, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);
    }
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        r->ssrSet[i] = alloc_set(r, r->ssrLayout);
        r->ssrApplySet[i] = alloc_set(r, r->ssrApplyLayout);
        for (int k = 0; k < 2; ++k) {
            w[k].dstSet = k ? r->ssrApplySet[i] : r->ssrSet[i]; w[k].dstBinding = 0; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[k].pBufferInfo = &ub;
        }
        vkUpdateDescriptorSets(vk->device, 2, w, 0, NULL);
        write_tex_set(vk, r->ssrSet[i], 3, CIS, r->linearClamp, r->hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 4, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 5, CIS, r->nearestClamp, r->refl.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_tex_set(vk, r->ssrSet[i], 6, SI, VK_NULL_HANDLE, r->ssr.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->ssrApplySet[i], 3, CIS, r->nearestClamp, r->ssr.view, VK_IMAGE_LAYOUT_GENERAL);
        write_tex_set(vk, r->ssrApplySet[i], 4, CIS, r->nearestClamp, r->refl.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    r->cloudCompSet = alloc_set(r, r->cloudCompLayout);
    write_tex_set(vk, r->cloudCompSet, 0, CIS, r->nearestClamp, r->cloud.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->cloudCompSet, 1, CIS, r->nearestClamp, r->cloudDist.view, VK_IMAGE_LAYOUT_GENERAL);
    write_tex_set(vk, r->cloudCompSet, 2, CIS, r->nearestClamp, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_tex_set(vk, r->cloudCompSet, 3, CIS, r->nearestClamp, r->mist.view, VK_IMAGE_LAYOUT_GENERAL);
}

static void destroy_targets(Renderer *r) {
    VkCore *vk = r->vk;
    vkDestroyDescriptorPool(vk->device, r->targetPool, NULL);
    for (uint32_t i = 0; i < r->bloomMips; ++i) vkDestroyImageView(vk->device, r->bloomMip[i], NULL);
    vk_image_destroy(vk, &r->bloom);
    vk_image_destroy(vk, &r->hdr);
    vk_image_destroy(vk, &r->depth);
    vk_image_destroy(vk, &r->depthResolve);
    vk_image_destroy(vk, &r->msaaColor);
    vk_image_destroy(vk, &r->cloud);
    vk_image_destroy(vk, &r->cloudDist);
    vk_image_destroy(vk, &r->mist);
    vk_image_destroy(vk, &r->reflMsaa);
    vk_image_destroy(vk, &r->refl);
    vk_image_destroy(vk, &r->ssr);
}

static void recreate_swapchain(Renderer *r) {
    VkCore *vk = r->vk;
    int w = 0, h = 0;
    glfwGetFramebufferSize(vk->window, &w, &h);
    while (w == 0 || h == 0) {        /* minimised */
        glfwWaitEvents();
        glfwGetFramebufferSize(vk->window, &w, &h);
    }
    vkDeviceWaitIdle(vk->device);
    destroy_targets(r);
    vk_destroy_swapchain(vk);
    vk_create_swapchain(vk);
    create_targets(r);
}

/* ------------------------------------------------------------- create --- */

Renderer *renderer_create(VkCore *vk, ChunkSystem *chunks) {
    Renderer *r = (Renderer *)calloc(1, sizeof(Renderer));
    r->vk = vk;
    r->chunks = chunks;

    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = vk->queueFamily;
    VK_CHECK(vkCreateCommandPool(vk->device, &cp, NULL, &r->cmdPool));
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, r->cmd));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VK_CHECK(vkCreateFence(vk->device, &fi, NULL, &r->fence[i]));
        VK_CHECK(vkCreateSemaphore(vk->device, &si, NULL, &r->imageReady[i]));
        r->ubo[i] = vk_buffer(vk, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r->skyBuf[i] = vk_buffer(vk, sizeof(SkyGrid), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    r->textures = textures_create(vk);
    r->shelter = vk_image(vk, SHELTER_SIZE, SHELTER_SIZE, 1, DEPTH_FORMAT,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    create_layouts(r);
    create_pipelines(r);
    build_cloud_noise(r);
    create_targets(r);
    return r;
}

void renderer_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    vkDeviceWaitIdle(vk->device);
    destroy_targets(r);
    vk_buffer_destroy(vk, &r->shotBuf);
    VkPipeline pipes[] = { r->cityPipe, r->skyPipe, r->compositePipe, r->bloomDownPipe, r->bloomUpPipe, r->cloudPipe, r->cloudCompPipe,
                           r->ssrPipe, r->ssrApplyPipe, r->shelterPipe };
    for (size_t i = 0; i < ARRAY_LEN(pipes); ++i) vkDestroyPipeline(vk->device, pipes[i], NULL);
    vkDestroyPipelineLayout(vk->device, r->scenePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->compositePL, NULL);
    vkDestroyPipelineLayout(vk->device, r->bloomPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->cloudPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->cloudCompPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->ssrPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->ssrApplyPL, NULL);
    vkDestroyPipelineLayout(vk->device, r->shelterPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->sceneLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->texLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->bloomLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->cloudLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->cloudCompLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->ssrLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, r->ssrApplyLayout, NULL);
    vkDestroyDescriptorPool(vk->device, r->staticPool, NULL);
    vkDestroySampler(vk->device, r->linearClamp, NULL);
    vkDestroySampler(vk->device, r->nearestClamp, NULL);
    vkDestroySampler(vk->device, r->linearRepeat, NULL);
    vk_image_destroy(vk, &r->cloudNoise);
    vk_image_destroy(vk, &r->shelter);
    textures_destroy(r->textures);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vkDestroyFence(vk->device, r->fence[i], NULL);
        vkDestroySemaphore(vk->device, r->imageReady[i], NULL);
        vk_buffer_destroy(vk, &r->ubo[i]);
        vk_buffer_destroy(vk, &r->skyBuf[i]);
    }
    vkDestroyCommandPool(vk->device, r->cmdPool, NULL);
    free(r);
}

/* -------------------------------------------------------------- frame --- */

static void set_viewport(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void record_bloom(Renderer *r, VkCommandBuffer cmd) {
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    vk_image_barrier(cmd, r->bloom.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | CS, 0,
                     CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, r->bloomMips);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomDownPipe);
    for (uint32_t i = 0; i < r->bloomMips; ++i) {
        BloomPush pc = { 0.9f, 0.6f, 1.0f, i == 0 ? 1 : 0 };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomPL, 0, 1, &r->bloomDownSet[i], 0, NULL);
        vkCmdPushConstants(cmd, r->bloomPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t w = r->bloom.w >> i, h = r->bloom.h >> i;
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomUpPipe);
    for (int i = (int)r->bloomMips - 2; i >= 0; --i) {
        BloomPush pc = { 0, 0, 1.0f, 0 };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->bloomPL, 0, 1, &r->bloomUpSet[i], 0, NULL);
        vkCmdPushConstants(cmd, r->bloomPL, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t w = r->bloom.w >> i, h = r->bloom.h >> i;
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          CS | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}

/* Clouds: march at half resolution against the scene depth (clouds.comp), then
 * blend into the HDR target with a depth-aware upsample (cloud_composite.frag). */
static void record_clouds(Renderer *r, VkCommandBuffer cmd, int slot) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkImage outs[3] = { r->cloud.img, r->cloudDist.img, r->mist.img };
    for (int i = 0; i < 3; ++i)
        vk_image_barrier(cmd, outs[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         FS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cloudPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->cloudPL, 0, 1, &r->cloudSet[slot], 0, NULL);
    vkCmdDispatch(cmd, (r->cloud.w + 7) / 8, (r->cloud.h + 7) / 8, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    /* the reflections pass wrote the HDR target; the blend reads it back */
    vk_memory_barrier(cmd, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->hdr.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, vk->extent.width, vk->extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cloudCompPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cloudCompPL, 0, 1, &r->cloudCompSet, 0, NULL);
    CloudPush cp = { NEAR_PLANE, FAR_PLANE };
    vkCmdPushConstants(cmd, r->cloudCompPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cp), &cp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

/* Shelter map: the city round the camera seen from straight above, depth
 * only, keeping the highest surface over each spot (shelter.vert). Rain
 * streaks and the wet coat test against it, so none falls under a canopy,
 * arcade or overhang. */
static void record_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown) {
    const VkPipelineStageFlags2 DEPTHS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags2 FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    vk_image_barrier(cmd, r->shelter.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     FS, 0, DEPTHS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1);
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->shelter.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f;   /* nothing there: below SHELTER_LO */
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = ri.renderArea.extent.height = SHELTER_SIZE;
    ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, SHELTER_SIZE, SHELTER_SIZE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shelterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->shelterPL, 0, 1, &r->sceneSet[slot], 0, NULL);
    vkCmdPushConstants(cmd, r->shelterPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), topDown);
    chunks_draw_shelter(r->chunks, cmd, slot);
    vkCmdEndRendering(cmd);
    vk_image_barrier(cmd, r->shelter.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     FS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);
}

/* Screen-space reflections: trace the wet surfaces' mirror rays against the
 * scene depth (ssr.comp), then streak and blend them into the HDR target
 * (ssr_apply.frag). Leaves the scene depth readable for the clouds after. */
static void record_reflections(Renderer *r, VkCommandBuffer cmd, int slot) {
    VkCore *vk = r->vk;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkAccessFlags2 READ = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    vk_image_barrier(cmd, msaa ? r->depthResolve.img : r->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | COLOR,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     CS | FS, READ, 0, 1);
    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, READ, 0, 1);
    vk_image_barrier(cmd, r->refl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS | FS, READ, 0, 1);
    vk_image_barrier(cmd, r->ssr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     FS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 0, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->ssrPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r->ssrPL, 0, 1, &r->ssrSet[slot], 0, NULL);
    vkCmdDispatch(cmd, (r->ssr.w + 7) / 8, (r->ssr.h + 7) / 8, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, FS, READ);
    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     CS, 0, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->hdr.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, vk->extent.width, vk->extent.height);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ssrApplyPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ssrApplyPL, 0, 1, &r->ssrApplySet[slot], 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void renderer_frame(Renderer *r, const FrameUBO *uboIn, const double cam[3], bool resized) {
    VkCore *vk = r->vk;
    int slot = (int)(r->frameIndex % MAX_FRAMES);
    if (resized) recreate_swapchain(r);

    VK_CHECK(vkWaitForFences(vk->device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX));
    uint32_t imageIndex;
    VkResult acq = vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, r->imageReady[slot], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(r); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);
    VK_CHECK(vkResetFences(vk->device, 1, &r->fence[slot]));

    /* Projection depends on the (possibly new) aspect ratio. */
    FrameUBO ubo = *uboIn;
    uint32_t W = vk->extent.width, H = vk->extent.height;
    ubo.screen[0] = (float)W; ubo.screen[1] = (float)H; ubo.screen[2] = 1.0f / W; ubo.screen[3] = 1.0f / H;
    /* Shelter map: centred on the camera, snapped to its texels so it doesn't
     * shimmer as the camera moves; camera-relative x, z to the square, height
     * (SHELTER_LO..SHELTER_HI) to depth, highest nearest. */
    const double texel = 2.0 * SHELTER_RADIUS / SHELTER_SIZE;
    float scx = (float)(floor(cam[0] / texel + 0.5) * texel - cam[0]);
    float scz = (float)(floor(cam[2] / texel + 0.5) * texel - cam[2]);
    ubo.shelter[0] = scx; ubo.shelter[1] = scz; ubo.shelter[2] = SHELTER_RADIUS; ubo.shelter[3] = 0.0f;
    mat4 topDown = { { 0 } };
    topDown.m[0] = 1.0f / SHELTER_RADIUS;  topDown.m[12] = -scx / SHELTER_RADIUS;
    topDown.m[9] = 1.0f / SHELTER_RADIUS;  topDown.m[13] = -scz / SHELTER_RADIUS;
    topDown.m[6] = 1.0f / (SHELTER_HI - SHELTER_LO);  topDown.m[14] = -SHELTER_LO / (SHELTER_HI - SHELTER_LO);
    topDown.m[15] = 1.0f;
    memcpy(r->ubo[slot].mapped, &ubo, sizeof(ubo));

    VkCommandBuffer cmd = r->cmd[slot];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    chunks_update(r->chunks, cmd, slot, r->frameIndex, cam, &ubo.viewProj);
    chunks_sky_lights(r->chunks, cam, (SkyGrid *)r->skyBuf[slot].mapped);
    record_shelter(r, cmd, slot, &topDown);

    /* ---- scene pass (MSAA HDR, resolved into r->hdr) ---- */
    const VkPipelineStageFlags2 COLOR = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 DEPTHS = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    bool msaa = vk->msaa != VK_SAMPLE_COUNT_1_BIT;
    if (msaa)
        vk_image_barrier(cmd, r->msaaColor.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    /* (the previous frame's cloud passes may still be reading the scene depth) */
    const VkPipelineStageFlags2 READERS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    vk_image_barrier(cmd, r->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     DEPTHS | READERS, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, DEPTHS,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1);
    if (msaa)
        vk_image_barrier(cmd, r->depthResolve.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         READERS, 0, DEPTHS | COLOR,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    vk_image_barrier(cmd, r->refl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     READERS, 0, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    if (msaa)
        vk_image_barrier(cmd, r->reflMsaa.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);

    /* colour, and the reflection G-buffer (both cleared to zero) */
    VkRenderingAttachmentInfo ca[2] = { { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO }, { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO } };
    VkImageView colorMs[2] = { r->msaaColor.view, r->reflMsaa.view }, colorRes[2] = { r->hdr.view, r->refl.view };
    for (int i = 0; i < 2; ++i) {
        ca[i].imageView = msaa ? colorMs[i] : colorRes[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca[i].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (msaa) {
            ca[i].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca[i].resolveImageView = colorRes[i];
            ca[i].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = r->depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f;   /* reverse-Z: 0 is infinitely far */
    if (msaa) {
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;   /* always supported */
        da.resolveImageView = r->depthResolve.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = vk->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 2;
    ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    set_viewport(cmd, W, H);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->scenePL, 0, 1, &r->sceneSet[slot], 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cityPipe);
    chunks_draw(r->chunks, cmd, slot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->skyPipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    /* ---- screen-space reflections, then clouds, blended into r->hdr ---- */
    record_reflections(r, cmd, slot);
    record_clouds(r, cmd, slot);

    vk_image_barrier(cmd, r->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0, 1);

    /* ---- bloom ---- */
    record_bloom(r, cmd);

    /* ---- composite / tonemap to the swapchain ---- */
    VkImage swapImg = vk->images[imageIndex];
    vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     COLOR, 0, COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, 1);
    VkRenderingAttachmentInfo sa = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    sa.imageView = vk->views[imageIndex];
    sa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sa.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2 = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri2.renderArea.extent = vk->extent;
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &sa;
    vkCmdBeginRendering(cmd, &ri2);
    set_viewport(cmd, W, H);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->compositePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->compositePL, 0, 1, &r->compositeSet, 0, NULL);
    CompositePush cp = { ubo.camPos[3], 0.14f, 1.0f, 0.035f, (float)W, (float)H, 0.008f, 0.32f };
    for (int k = 0; k < 3; ++k) {
        cp.right[k] = ubo.rayBasis.m[k]; cp.up[k] = ubo.rayBasis.m[4 + k]; cp.fwd[k] = ubo.rayBasis.m[8 + k];
    }
    cp.right[3] = NEAR_PLANE; cp.up[3] = FAR_PLANE; cp.fwd[3] = ubo.weather[0];
    memcpy(cp.shelter, ubo.shelter, sizeof(cp.shelter));
    vkCmdPushConstants(cmd, r->compositePL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cp), &cp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    bool shot = r->shotPending;
    if (shot) {
        r->shotPending = false;
        vk_buffer_destroy(vk, &r->shotBuf);
        r->shotBuf = vk_buffer(vk, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
        VkBufferImageCopy bc = { 0 };
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = W; bc.imageExtent.height = H; bc.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->shotBuf.buf, 1, &bc);
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0, 0, 1);
    } else {
        vk_image_barrier(cmd, swapImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         COLOR, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 0, 1);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = r->imageReady[slot];
    wait.stageMask = COLOR;
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = vk->renderDone[imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 1; si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1; si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(vk->queue, 1, &si, r->fence[slot]));
    if (shot) {
        VK_CHECK(vkWaitForFences(vk->device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX));
        bool bgr = vk->swapFormat == VK_FORMAT_B8G8R8A8_SRGB || vk->swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
        write_bmp(r->shotPath, (const uint8_t *)r->shotBuf.mapped, W, H, bgr);
    }

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk->renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk->swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(vk->queue, &pi);
    r->frameIndex++;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) recreate_swapchain(r);
    else if (pr != VK_SUCCESS) VK_CHECK(pr);
}
