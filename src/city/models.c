#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include "models.h"
#include "assets.h"
#include "pbr.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The model files (models.h): read once, before the generator runs. */

const char *const MODEL_NAMES[PM_COUNT] = {
    "bollard", "litter_bin", "mailbox", "dumpster", "aircon", "crossing_sign", "crossing_button", "sofa",
};
/* the authored materials, by id (pbr.h), to find a model's by name */
static const char *const MATERIAL_NAMES[PBR_MATERIAL_COUNT] = {
#include "pbr_material_names.inc"
};

static Model g_models[PM_COUNT];
static bool  g_loaded[PM_COUNT];

void model_free(Model *m) {
    free(m->v);
    free(m->ix);
    memset(m, 0, sizeof(*m));
}

static int material_id(const cgltf_material *cm) {
    if (!cm || !cm->name) return -1;
    for (int k = 0; k < PBR_MATERIAL_COUNT; ++k) if (!strcmp(MATERIAL_NAMES[k], cm->name)) return k;
    return -1;
}

bool model_read(const char *path, Model *m, char *err, size_t errSize) {
    memset(m, 0, sizeof(*m));
    cgltf_options opt;
    memset(&opt, 0, sizeof(opt));
    cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) { snprintf(err, errSize, "can't read it"); return false; }
    if (cgltf_load_buffers(&opt, d, path) != cgltf_result_success || cgltf_validate(d) != cgltf_result_success) {
        snprintf(err, errSize, "its buffers are missing or malformed");
        cgltf_free(d);
        return false;
    }
    const char *why = NULL;
    const cgltf_primitive *pr = d->meshes_count == 1 && d->meshes[0].primitives_count == 1 ? &d->meshes[0].primitives[0] : NULL;
    const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL;
    if (!pr || pr->type != cgltf_primitive_type_triangles || !pr->indices) why = "it isn't one mesh of one indexed triangle primitive";
    else {
        for (cgltf_size a = 0; a < pr->attributes_count; ++a) {
            const cgltf_attribute *at = &pr->attributes[a];
            if (at->type == cgltf_attribute_type_position) pos = at->data;
            else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
            else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv = at->data;
        }
        if (!pos || !nrm || !uv) why = "it lacks positions, normals or texture coordinates";
        else if (pr->indices->count % 3 || pr->indices->count == 0) why = "its indices aren't whole triangles";
    }
    /* its own material, then a livery for each of the file's variants, in order */
    int id = why ? -1 : material_id(pr->material);
    if (!why && id < 0) why = "its material isn't one of assets/materials";
    if (!why) {
        m->pbr[0] = (uint8_t)id;
        m->liveries = 1;
        for (cgltf_size v = 0; v < d->variants_count && !why; ++v) {
            const cgltf_material *cm = NULL;
            for (cgltf_size k = 0; k < pr->mappings_count; ++k) if (pr->mappings[k].variant == v) cm = pr->mappings[k].material;
            int lv = cm ? material_id(cm) : -1;
            if (lv < 0) why = "a livery's material isn't one of assets/materials";
            else if (lv == id && v == 0) continue;   /* the first variant is usually its own */
            else if (m->liveries == MODEL_LIVERIES) why = "it has too many liveries";
            else m->pbr[m->liveries++] = (uint8_t)lv;
        }
    }
    if (why) { snprintf(err, errSize, "%s", why); cgltf_free(d); return false; }
    m->nv = (uint32_t)pos->count;
    m->ni = (uint32_t)pr->indices->count;
    m->v = (ModelVertex *)malloc(sizeof(ModelVertex) * m->nv);
    m->ix = (uint32_t *)malloc(sizeof(uint32_t) * m->ni);
    if (!m->v || !m->ix) { snprintf(err, errSize, "out of memory"); model_free(m); cgltf_free(d); return false; }
    for (int k = 0; k < 3; ++k) { m->mn[k] = FLT_MAX; m->mx[k] = -FLT_MAX; }
    for (uint32_t i = 0; i < m->nv; ++i) {
        ModelVertex *v = &m->v[i];
        cgltf_accessor_read_float(pos, i, v->p, 3);
        cgltf_accessor_read_float(nrm, i, v->n, 3);
        cgltf_accessor_read_float(uv, i, v->uv, 2);
        for (int k = 0; k < 3; ++k) { m->mn[k] = v->p[k] < m->mn[k] ? v->p[k] : m->mn[k]; m->mx[k] = v->p[k] > m->mx[k] ? v->p[k] : m->mx[k]; }
    }
    for (uint32_t i = 0; i < m->ni; ++i) {
        cgltf_size x = cgltf_accessor_read_index(pr->indices, i);
        if (x >= m->nv) { snprintf(err, errSize, "an index is out of range"); model_free(m); cgltf_free(d); return false; }
        m->ix[i] = (uint32_t)x;
    }
    cgltf_free(d);
    return true;
}

void models_free(void) {
    for (int i = 0; i < PM_COUNT; ++i) { model_free(&g_models[i]); g_loaded[i] = false; }
}

int models_load(const char *dir) {
    models_free();
    char roots[2][1024];
    int nr = dir ? 1 : asset_roots(roots);
    if (dir) snprintf(roots[0], sizeof(roots[0]), "%s", dir);
    int n = 0;
    for (int i = 0; i < PM_COUNT; ++i) {
        char err[256] = "not found", path[1200];
        for (int r = 0; r < nr && !g_loaded[i]; ++r) {
            snprintf(path, sizeof(path), "%s/models/%s.glb", roots[r], MODEL_NAMES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fclose(f);
            g_loaded[i] = model_read(path, &g_models[i], err, sizeof(err));
        }
        if (g_loaded[i]) n++;
        else fprintf(stderr, "model %s: %s; what uses it is left out\n", MODEL_NAMES[i], err);
    }
    return n;
}

const Model *model_get(int pm) { return pm >= 0 && pm < PM_COUNT && g_loaded[pm] ? &g_models[pm] : NULL; }
