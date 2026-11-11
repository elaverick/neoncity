#include "renderer_internal.h"

/* SPIR-V is compiled at build time and embedded as word lists. */
static const uint32_t SPV_city_vert[] = {
#include "city.vert.inc"
};
static const uint32_t SPV_kit_vert[] = {
#include "kit.vert.inc"
};
static const uint32_t SPV_kit_frag[] = {
#include "kit.frag.inc"
};
static const uint32_t SPV_kit_rt_frag[] = {
#include "kit_rt.frag.inc"
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
/* ray-traced shadows: the scene pass with ray queries, the depth prepass, the trace and the denoiser */
static const uint32_t SPV_city_rt_frag[] = {
#include "city_rt.frag.inc"
};
static const uint32_t SPV_city_pre_vert[] = {
#include "city_pre.vert.inc"
};
static const uint32_t SPV_impostor_demand_frag[] = {
#include "impostor_demand.frag.inc"
};
static const uint32_t SPV_prepass_frag[] = {
#include "prepass.frag.inc"
};
static const uint32_t SPV_shadow_trace[] = {
#include "shadow_trace.comp.inc"
};
static const uint32_t SPV_shadow_denoise[] = {
#include "shadow_denoise.comp.inc"
};
static const uint32_t SPV_terrain_vert[] = {
#include "terrain/terrain.vert.inc"
};
static const uint32_t SPV_terrain_frag[] = {
#include "terrain/terrain.frag.inc"
};
static const uint32_t SPV_terrain_rt_frag[] = {
#include "terrain/terrain_rt.frag.inc"
};
static const uint32_t SPV_terrain_pre_frag[] = {
#include "terrain/terrain_pre.frag.inc"
};
static const uint32_t SPV_terrain_cut_frag[] = {
#include "terrain/terrain_cut.frag.inc"
};
static const uint32_t SPV_planet_vert[] = {
#include "terrain/planet.vert.inc"
};
static const uint32_t SPV_planet_frag[] = {
#include "terrain/planet.frag.inc"
};
static const uint32_t SPV_cave_vert[] = {
#include "terrain/cave.vert.inc"
};
static const uint32_t SPV_cave_frag[] = {
#include "terrain/cave.frag.inc"
};
static const uint32_t SPV_cave_rt_frag[] = {
#include "terrain/cave_rt.frag.inc"
};
static const uint32_t SPV_cave_shelter_vert[] = {
#include "terrain/cave_shelter.vert.inc"
};
static const uint32_t SPV_cave_shelter_frag[] = {
#include "terrain/cave_shelter.frag.inc"
};
static const uint32_t SPV_proxy_vert[] = {
#include "proxy/proxy.vert.inc"
};
static const uint32_t SPV_proxy_frag[] = {
#include "proxy/proxy.frag.inc"
};
static const uint32_t SPV_flame_vert[] = {
#include "flame.vert.inc"
};
static const uint32_t SPV_flame_frag[] = {
#include "flame.frag.inc"
};
static const uint32_t SPV_smoke_vert[] = {
#include "smoke.vert.inc"
};
static const uint32_t SPV_smoke_frag[] = {
#include "smoke.frag.inc"
};

/* ------------------------------------------------------------ layouts --- */

VkDescriptorSetLayout make_set_layout(VkCore *vk, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = n;
    ci.pBindings = b;
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(vk->device, &ci, NULL, &l));
    return l;
}

VkPipelineLayout make_pipeline_layout(VkCore *vk, VkDescriptorSetLayout set, uint32_t pushSize, VkShaderStageFlags pushStages) {
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

void write_as_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkAccelerationStructureKHR as) {
    VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    wa.accelerationStructureCount = 1;
    wa.pAccelerationStructures = &as;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.pNext = &wa;
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

void write_buf_set(VkCore *vk, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize size) {
    VkDescriptorBufferInfo bi = { b, 0, size };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1;
    w.descriptorType = type; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

/* The authored materials' maps (binding 3) and properties (binding 8), as the
 * scene set has them, into another set: the terrain's (shaders/pbr.glsl). */
void write_material_set(Renderer *r, VkDescriptorSet set) {
    VkDescriptorImageInfo maps[PBR_TEXTURE_COUNT];
    materials_descriptors(r->materials, maps);
    VkDescriptorBufferInfo mb = { materials_table(r->materials), 0, sizeof(MaterialGPU) * PBR_MATERIAL_COUNT };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    w[0].dstSet = set; w[0].dstBinding = 3; w[0].descriptorCount = PBR_TEXTURE_COUNT;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = maps;
    w[1].dstSet = set; w[1].dstBinding = 8; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &mb;
    vkUpdateDescriptorSets(r->vk->device, 2, w, 0, NULL);
}

void create_layouts(Renderer *r) {
    VkCore *vk = r->vk;
    VkDescriptorSetLayoutBinding scene[16] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shelter map (litter: city.vert) */
        { 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the materials' properties (materials.c) */
        { 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the adverts' table (adverts.c) */
        { 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ADVERT_TABLE_SIZE, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* each advert's frames */
        { 11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the interior impostors' table (interiors.c) */
        { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, INTERIOR_IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* each module's channels */
        { 13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* which modules were drawn near the eye */
        { 14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the sky-view table (atmosphere_gpu.c: sky.frag) */
        { 15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the air's transmittance */
        /* ray-traced shadows only */
        { 5, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shadow slot ids */
        { 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shadow slot visibility */
    };
    r->sceneLayout = make_set_layout(vk, scene, r->rt ? 16 : 13);
    if (r->rt) {
        const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        VkDescriptorSetLayoutBinding sh[11] = {
            { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, C, NULL },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL },              /* chunks */
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL },              /* light tables */
            { 5, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, C, NULL },
            { 8, CIS, 1, C, NULL },    /* prepass depth, sample 0 */
            { 9, CIS, 1, C, NULL },    /* prepass G-buffer, sample 0 */
            { 10, SI, 1, C, NULL },    /* slot ids */
            { 11, SI, 1, C, NULL },    /* raw rays */
            { 12, SI, 1, C, NULL },    /* slot visibility */
            { 13, CIS, 1, C, NULL },   /* the previous frame's visibility */
            { 14, CIS, 1, C, NULL },   /* the previous frame's ids */
        };
        r->shadowLayout = make_set_layout(vk, sh, 11);
        r->shadowPL = make_pipeline_layout(vk, r->shadowLayout, sizeof(uint32_t), C);
    }
    VkDescriptorSetLayoutBinding tex[6] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* scene depth (rain) */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* rain mist (rain lighting) */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* shelter map (rain) */
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },           /* the eye's exposure (exposure_gpu.c) */
    };
    r->texLayout = make_set_layout(vk, tex, 6);
    VkDescriptorSetLayoutBinding bloom[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    r->bloomLayout = make_set_layout(vk, bloom, 2);
    VkDescriptorSetLayoutBinding cloud[8] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },           /* sky lights */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* noise volume */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },   /* scene depth */
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* cloud */
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* cloudDist */
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },            /* mist */
        { 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },           /* the adverts' table (screens' sky lights) */
    };
    r->cloudLayout = make_set_layout(vk, cloud, 8);
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
    VkDescriptorSetLayoutBinding flame[5] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* FlameSet */
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },                        /* scene depth */
        { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },                        /* noise volume */
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* PlumeSet */
    };
    r->flameLayout = make_set_layout(vk, flame, 5);
    VkDescriptorSetLayoutBinding terrain[7] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* clipmap samples (the fragments: the moon's shadows) */
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* roads (road_gpu.c; the clipmap's only) */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the materials' maps, as the scene set's */
        { 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the materials' properties */
        { 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },  /* the grass's patch states (grass_gpu.c: --grass-debug cull; the clipmap's only) */
        { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the caves' mouths' holes (cave_gpu.c; the clipmap's only) */
    };
    r->terrainLayout = make_set_layout(vk, terrain, 7);
    VkDescriptorSetLayoutBinding cave[8] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },   /* the bricks' pool (cave_gpu.c) */
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the frame's systems and draws */
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the materials' maps */
        { 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the materials' properties */
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the clipmap's samples (the cut at the mouths) */
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* the holes and the terrain's levels */
        { 6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },   /* ray tracing only: the TLAS (the moon's rays, cave_rt.frag) */
    };
    r->caveLayout = make_set_layout(vk, cave, r->rt ? 8 : 7);
    VkDescriptorSetLayoutBinding proxy[3] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },   /* boxes */
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },   /* tile origins */
    };
    r->proxyLayout = make_set_layout(vk, proxy, 3);

    r->scenePL = make_pipeline_layout(vk, r->sceneLayout, 0, 0);
    r->compositePL = make_pipeline_layout(vk, r->texLayout, sizeof(CompositePush), VK_SHADER_STAGE_FRAGMENT_BIT);
    r->bloomPL = make_pipeline_layout(vk, r->bloomLayout, sizeof(BloomPush), VK_SHADER_STAGE_COMPUTE_BIT);
    r->cloudPL = make_pipeline_layout(vk, r->cloudLayout, 0, 0);
    r->cloudCompPL = make_pipeline_layout(vk, r->cloudCompLayout, sizeof(CloudPush), VK_SHADER_STAGE_FRAGMENT_BIT);
    r->ssrPL = make_pipeline_layout(vk, r->ssrLayout, 0, 0);
    r->ssrApplyPL = make_pipeline_layout(vk, r->ssrApplyLayout, 0, 0);
    r->flamePL = make_pipeline_layout(vk, r->flameLayout, 0, 0);
    r->shelterPL = make_pipeline_layout(vk, r->sceneLayout, sizeof(mat4), VK_SHADER_STAGE_VERTEX_BIT);
    {   /* the terrain's set, and the scene set as set 1 for the city's lamps on the land (terrain.frag) */
        VkDescriptorSetLayout sets[2] = { r->terrainLayout, r->sceneLayout };
        VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TerrainPush) };
        VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(vk->device, &ci, NULL, &r->terrainPL));
    }
    r->planetPL = make_pipeline_layout(vk, r->terrainLayout, sizeof(PlanetPush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    r->cavePL = make_pipeline_layout(vk, r->caveLayout, 4 * sizeof(int32_t), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);   /* cave.glsl: CaveView */
    r->caveShelterPL = make_pipeline_layout(vk, r->caveLayout, sizeof(mat4), VK_SHADER_STAGE_VERTEX_BIT);
    r->proxyPL = make_pipeline_layout(vk, r->proxyLayout, 0, 0);

    VkDescriptorPoolSize sizes[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (PBR_TEXTURE_COUNT + ADVERT_TABLE_SIZE + INTERIOR_IMAGES + 5) * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = r->rt ? 4 : 3;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &r->staticPool));

    VkDescriptorImageInfo maps[PBR_TEXTURE_COUNT];
    materials_descriptors(r->materials, maps);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r->staticPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->sceneLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &r->sceneSet[i]));
        VkDescriptorBufferInfo ub = { r->ubo[i].buf, 0, sizeof(FrameUBO) };
        VkDescriptorBufferInfo sb = { chunks_gpu_buffer(r->chunks, i), 0, chunks_gpu_buffer_size() };
        VkDescriptorBufferInfo lb = { chunks_light_buffer(r->chunks), 0, chunks_light_buffer_size() };
        VkDescriptorBufferInfo mb = { materials_table(r->materials), 0, sizeof(MaterialGPU) * PBR_MATERIAL_COUNT };
        VkDescriptorBufferInfo ab = { adverts_table(r->adverts), 0, sizeof(AdvertTable) };
        VkDescriptorImageInfo af[ADVERT_TABLE_SIZE];
        adverts_frames(r->adverts, af);
        VkDescriptorBufferInfo ib = { interiors_table(r->interiors), 0, sizeof(InteriorTable) };
        VkDescriptorImageInfo ii[INTERIOR_IMAGES];
        interiors_images(r->interiors, ii);
        VkDescriptorBufferInfo db = { interiors_demand(r->interiors, i), 0, sizeof(InteriorDemand) };
        VkWriteDescriptorSet w[10] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        w[0].dstSet = r->sceneSet[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ub;
        w[1].dstSet = r->sceneSet[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &sb;
        w[2].dstSet = r->sceneSet[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &lb;
        w[3].dstSet = r->sceneSet[i]; w[3].dstBinding = 3; w[3].descriptorCount = PBR_TEXTURE_COUNT;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = maps;
        w[4].dstSet = r->sceneSet[i]; w[4].dstBinding = 8; w[4].descriptorCount = 1;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[4].pBufferInfo = &mb;
        w[5].dstSet = r->sceneSet[i]; w[5].dstBinding = 9; w[5].descriptorCount = 1;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[5].pBufferInfo = &ab;
        w[6].dstSet = r->sceneSet[i]; w[6].dstBinding = 10; w[6].descriptorCount = ADVERT_TABLE_SIZE;
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[6].pImageInfo = af;
        w[7].dstSet = r->sceneSet[i]; w[7].dstBinding = 11; w[7].descriptorCount = 1;
        w[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[7].pBufferInfo = &ib;
        w[8].dstSet = r->sceneSet[i]; w[8].dstBinding = 12; w[8].descriptorCount = INTERIOR_IMAGES;
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[8].pImageInfo = ii;
        w[9].dstSet = r->sceneSet[i]; w[9].dstBinding = 13; w[9].descriptorCount = 1;
        w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[9].pBufferInfo = &db;
        vkUpdateDescriptorSets(vk->device, 10, w, 0, NULL);
        if (r->rt) write_as_set(vk, r->sceneSet[i], 5, rt_tlas(r->rt, i));
    }
    interiors_set_descriptors(r->interiors, r->sceneSet, 12);

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

void make_composite_pipe(Renderer *r) {
    VkCore *vk = r->vk;
    if (r->compositePipe) vkDestroyPipeline(vk->device, r->compositePipe, NULL);
    GfxDesc comp = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_composite_frag, sizeof(SPV_composite_frag),
                     r->compositePL, vk->swapFormat, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false };
    r->compositePipe = make_graphics(vk, &comp);
}

VkPipeline make_graphics(VkCore *vk, const GfxDesc *d) {
    VkShaderModule vs = vk_shader(vk, d->vs, d->vsBytes), fs = vk_shader(vk, d->fs, d->fsBytes);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkVertexInputBindingDescription bind = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[7] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) },
        { 1, 0, VK_FORMAT_R8G8B8A8_SNORM,   offsetof(Vertex, nrm) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, uv) },
        { 3, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, seed) },
        { 4, 0, VK_FORMAT_R8G8B8A8_UINT,    offsetof(Vertex, params) },
        { 5, 0, VK_FORMAT_R32_UINT,         offsetof(Vertex, aux) },
        { 6, 0, VK_FORMAT_R32_SFLOAT,       offsetof(Vertex, base) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (d->vertexInput) {
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = d->positionOnly ? 1 : 7; vi.pVertexAttributeDescriptions = attrs;
    }
    /* the kit's instances (kit_gpu.h KitInstGPU): three rows of its transform, then seed, chunk, part, base */
    VkVertexInputBindingDescription kbind[2] = { bind, { 1, sizeof(KitInstGPU), VK_VERTEX_INPUT_RATE_INSTANCE } };
    VkVertexInputAttributeDescription kattrs[11];
    if (d->kit) {
        memcpy(kattrs, attrs, sizeof(attrs));
        for (uint32_t i = 0; i < 3; ++i)
            kattrs[7 + i] = (VkVertexInputAttributeDescription){ 7 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(offsetof(KitInstGPU, row) + 16 * i) };
        kattrs[10] = (VkVertexInputAttributeDescription){ 10, 1, VK_FORMAT_R32G32B32A32_UINT, (uint32_t)offsetof(KitInstGPU, seed) };
        vi.vertexBindingDescriptionCount = 2; vi.pVertexBindingDescriptions = kbind;
        vi.vertexAttributeDescriptionCount = 11; vi.pVertexAttributeDescriptions = kattrs;
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
    cba[0].colorWriteMask = cba[1].colorWriteMask = d->noColor ? 0 : 0xF;
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

VkPipeline make_compute(VkCore *vk, const uint32_t *code, size_t bytes, VkPipelineLayout layout) {
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

void create_pipelines(Renderer *r) {
    VkCore *vk = r->vk;
    GfxDesc city = { SPV_city_vert, sizeof(SPV_city_vert), r->rt ? SPV_city_rt_frag : SPV_city_frag,
                     r->rt ? sizeof(SPV_city_rt_frag) : sizeof(SPV_city_frag),
                     r->scenePL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, true, true, true, true };
    city.colorFormat2 = HDR_FORMAT;   /* reflection G-buffer */
    r->cityPipe = make_graphics(vk, &city);
    GfxDesc demand = city;            /* which interior modules are near the eye: over the city's depth, drawing nothing */
    demand.fs = SPV_impostor_demand_frag; demand.fsBytes = sizeof(SPV_impostor_demand_frag);
    demand.depthWrite = false;
    demand.noColor = true;
    r->impDemandPipe = make_graphics(vk, &demand);
    GfxDesc kit = city;               /* the vendors' kit, instanced */
    kit.vs = SPV_kit_vert; kit.vsBytes = sizeof(SPV_kit_vert);
    kit.fs = r->rt ? SPV_kit_rt_frag : SPV_kit_frag; kit.fsBytes = r->rt ? sizeof(SPV_kit_rt_frag) : sizeof(SPV_kit_frag);
    kit.kit = true;
    r->kitPipe = make_graphics(vk, &kit);
    city.cull = false;                /* lobby plants: a leaf is a single surface, seen from both sides */
    r->plantPipe = make_graphics(vk, &city);
    GfxDesc sky = { SPV_fullscreen_vert, sizeof(SPV_fullscreen_vert), SPV_sky_frag, sizeof(SPV_sky_frag),
                    r->scenePL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, false, false };
    sky.colorFormat2 = HDR_FORMAT;
    r->skyPipe = make_graphics(vk, &sky);
    GfxDesc terrain = { SPV_terrain_vert, sizeof(SPV_terrain_vert), SPV_terrain_frag, sizeof(SPV_terrain_frag),
                        r->terrainPL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, true, true };
    terrain.colorFormat2 = HDR_FORMAT;
    GfxDesc terrainLit = terrain;   /* with ray tracing, the lamps' shadows on the land */
    if (r->rt) { terrainLit.fs = SPV_terrain_rt_frag; terrainLit.fsBytes = sizeof(SPV_terrain_rt_frag); }
    r->terrainPipe = make_graphics(vk, &terrainLit);
    GfxDesc terrainCut = terrain;
    terrainCut.fs = SPV_terrain_cut_frag; terrainCut.fsBytes = sizeof(SPV_terrain_cut_frag);
    r->terrainCutPipe = make_graphics(vk, &terrainCut);
    GfxDesc planet = terrain;
    planet.vs = SPV_planet_vert; planet.vsBytes = sizeof(SPV_planet_vert);
    planet.fs = SPV_planet_frag; planet.fsBytes = sizeof(SPV_planet_frag);
    planet.layout = r->planetPL;
    r->planetPipe = make_graphics(vk, &planet);
    GfxDesc cave = terrain;
    cave.vs = SPV_cave_vert; cave.vsBytes = sizeof(SPV_cave_vert);
    cave.fs = SPV_cave_frag; cave.fsBytes = sizeof(SPV_cave_frag);
    cave.layout = r->cavePL;
    GfxDesc caveLit = cave;   /* with ray tracing, the moon traced in the caves */
    if (r->rt) { caveLit.fs = SPV_cave_rt_frag; caveLit.fsBytes = sizeof(SPV_cave_rt_frag); }
    r->cavePipe = make_graphics(vk, &caveLit);
    cave.cull = false;
    r->caveXrayPipe = make_graphics(vk, &cave);
    GfxDesc proxy = { SPV_proxy_vert, sizeof(SPV_proxy_vert), SPV_proxy_frag, sizeof(SPV_proxy_frag),
                      r->proxyPL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, true, true };
    proxy.colorFormat2 = HDR_FORMAT;
    r->proxyPipe = make_graphics(vk, &proxy);
    make_composite_pipe(r);
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
    GfxDesc csh = { SPV_cave_shelter_vert, sizeof(SPV_cave_shelter_vert), SPV_cave_shelter_frag, sizeof(SPV_cave_shelter_frag),
                    r->caveShelterPL, VK_FORMAT_UNDEFINED, DEPTH_FORMAT, VK_SAMPLE_COUNT_1_BIT, false, true, true, false };
    r->caveShelterPipe = make_graphics(vk, &csh);
    /* flames: boxes added to the HDR target, no depth attachment (flame.frag reads the depth) */
    GfxDesc fl = { SPV_flame_vert, sizeof(SPV_flame_vert), SPV_flame_frag, sizeof(SPV_flame_frag),
                   r->flamePL, HDR_FORMAT, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false };
    fl.additive = true;
    r->flamePipe = make_graphics(vk, &fl);
    /* smoke: the same, blended over the scene by its transmittance (smoke.frag) */
    GfxDesc sm = { SPV_smoke_vert, sizeof(SPV_smoke_vert), SPV_smoke_frag, sizeof(SPV_smoke_frag),
                   r->flamePL, HDR_FORMAT, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false, false, false, false, true };
    r->smokePipe = make_graphics(vk, &sm);
    if (r->rt) {
        GfxDesc pre = { SPV_city_pre_vert, sizeof(SPV_city_pre_vert), SPV_prepass_frag, sizeof(SPV_prepass_frag),
                        r->scenePL, GBUF_FORMAT, DEPTH_FORMAT, vk->msaa, true, true, true, true };
        r->prepassPipe = make_graphics(vk, &pre);
        GfxDesc kpre = pre;   /* the vendors' kit */
        kpre.vs = SPV_kit_vert; kpre.vsBytes = sizeof(SPV_kit_vert);
        kpre.kit = true;
        r->kitPrePipe = make_graphics(vk, &kpre);
        pre.cull = false;   /* lobby plants */
        r->prepassPlantPipe = make_graphics(vk, &pre);
        GfxDesc tpre = { SPV_terrain_vert, sizeof(SPV_terrain_vert), SPV_terrain_pre_frag, sizeof(SPV_terrain_pre_frag),
                         r->terrainPL, GBUF_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, true, true };
        r->terrainPrePipe = make_graphics(vk, &tpre);
        r->tracePipe = make_compute(vk, SPV_shadow_trace, sizeof(SPV_shadow_trace), r->shadowPL);
        r->denoisePipe = make_compute(vk, SPV_shadow_denoise, sizeof(SPV_shadow_denoise), r->shadowPL);
    }
}
