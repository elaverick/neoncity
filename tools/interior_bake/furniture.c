#include "furniture.h"
#include "cgltf.h"

/* See furniture.h. Encoded textures are kept as they came, for gpu.c to decode into its arrays when the
 * scene loads, so a --hash of a scene needs no decoding. */

static int image_add(Scene *s, const void *bytes, size_t size, bool colour, const char *name) {
    for (int i = 0; i < s->nimages; ++i)
        if (!strcmp(s->images[i].name, name)) {   /* one file used twice: one layer */
            int layer = 0;
            for (int j = 0; j < i; ++j) layer += s->images[j].colour == colour;
            return layer;
        }
    if (s->nimages == s->capImages) {
        s->capImages = s->capImages ? s->capImages * 2 : 32;
        s->images = realloc(s->images, sizeof(BakeImage) * (size_t)s->capImages);
        if (!s->images) fatal("out of memory holding the textures");
    }
    BakeImage *im = &s->images[s->nimages++];
    im->bytes = malloc(size);
    if (!im->bytes) fatal("out of memory holding a texture");
    memcpy(im->bytes, bytes, size);
    im->size = size;
    im->colour = colour;
    snprintf(im->name, sizeof(im->name), "%s", name);
    return colour ? s->ncolour++ : s->ndata++;
}

static int image_file(Scene *s, const char *dir, const char *file, bool colour) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "furniture: can't read %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) fatal("out of memory reading a texture");
    fclose(f);
    int layer = image_add(s, b, (size_t)n, colour, file);
    free(b);
    return layer;
}

static int view_image(Scene *s, const cgltf_texture_view *v, bool colour, const char *name) {
    if (!v->texture || !v->texture->image || !v->texture->image->buffer_view) return -1;
    const cgltf_buffer_view *bv = v->texture->image->buffer_view;
    return image_add(s, (const uint8_t *)bv->buffer->data + bv->offset, bv->size, colour, name);
}

/* A primitive's mesh, with its normals and UVs. area: its total, m2 (may be NULL). */
static int prim_mesh(Scene *s, const cgltf_primitive *p, float *area) {
    const cgltf_accessor *pos = NULL, *nrm = NULL, *tex = NULL;
    for (cgltf_size i = 0; i < p->attributes_count; ++i) {
        if (p->attributes[i].type == cgltf_attribute_type_position) pos = p->attributes[i].data;
        if (p->attributes[i].type == cgltf_attribute_type_normal) nrm = p->attributes[i].data;
        if (p->attributes[i].type == cgltf_attribute_type_texcoord && !tex) tex = p->attributes[i].data;
    }
    if (!pos || !nrm || !tex || !p->indices) return -1;
    uint32_t nv = (uint32_t)pos->count, ni = (uint32_t)p->indices->count;
    float *v = malloc(sizeof(float) * 3 * nv), *a = malloc(sizeof(float) * 5 * nv), *tmp = malloc(sizeof(float) * 3 * nv);
    uint32_t *ix = malloc(sizeof(uint32_t) * ni);
    if (!v || !a || !tmp || !ix) fatal("out of memory reading a model");
    cgltf_accessor_unpack_floats(pos, v, nv * 3);
    cgltf_accessor_unpack_floats(nrm, tmp, nv * 3);
    for (uint32_t i = 0; i < nv; ++i) memcpy(a + i * 5, tmp + i * 3, sizeof(float) * 3);
    cgltf_accessor_unpack_floats(tex, tmp, nv * 2);
    for (uint32_t i = 0; i < nv; ++i) memcpy(a + i * 5 + 3, tmp + i * 2, sizeof(float) * 2);
    for (uint32_t i = 0; i < ni; ++i) ix[i] = (uint32_t)cgltf_accessor_read_index(p->indices, i);
    if (area) {
        double sum = 0.0;
        for (uint32_t i = 0; i + 2 < ni; i += 3) {
            const float *A = v + ix[i] * 3, *B = v + ix[i + 1] * 3, *C = v + ix[i + 2] * 3;
            float e[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, g[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            float c[3] = { e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0] };
            sum += 0.5 * sqrt((double)(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]));
        }
        *area = (float)sum;
    }
    int id = mesh_add_attr(s, v, a, nv, ix, ni);
    free(v); free(a); free(tmp); free(ix);
    return id;
}

static bool piece_load(Scene *s, Piece *out, const char *dir, const char *name) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.glb", dir, name);
    cgltf_options opt = { 0 };
    cgltf_data *d = NULL;
    memset(out, 0, sizeof(*out));
    out->extra = -1;
    bool ok = false;
    if (cgltf_parse_file(&opt, path, &d) == cgltf_result_success && cgltf_load_buffers(&opt, d, path) == cgltf_result_success && d->meshes_count == 1) {
        const cgltf_mesh *m = &d->meshes[0];
        out->mesh = prim_mesh(s, &m->primitives[0], NULL);
        ok = out->mesh >= 0;
        if (ok) { memcpy(out->mn, s->meshes[out->mesh].mn, sizeof(out->mn)); memcpy(out->mx, s->meshes[out->mesh].mx, sizeof(out->mx)); }
        for (cgltf_size i = 1; ok && i < m->primitives_count; ++i)
            out->extra = prim_mesh(s, &m->primitives[i], &out->extraArea);
        for (cgltf_size i = 0; ok && i < d->materials_count && out->nmats < PIECE_VARIANTS; ++i) {
            const cgltf_material *mt = &d->materials[i];
            if (!mt->has_pbr_metallic_roughness || !mt->pbr_metallic_roughness.base_color_texture.texture) continue;   /* the panel's */
            char label[64];
            int id = mat_add(s, 1.0f, 1.0f, 1.0f, 0, 0, 0);
            mat_surface(s, id, 1.0f, 1.0f, 0.0f, 0.0f);   /* as the maps say */
            snprintf(label, sizeof(label), "%s.%d.colour", name, (int)i);
            int c = view_image(s, &mt->pbr_metallic_roughness.base_color_texture, true, label);
            snprintf(label, sizeof(label), "%s.%d.orm", name, (int)i);
            int r = view_image(s, &mt->pbr_metallic_roughness.metallic_roughness_texture, false, label);
            snprintf(label, sizeof(label), "%s.%d.normal", name, (int)i);
            int n = view_image(s, &mt->normal_texture, false, label);
            BakeMat *b = &s->mats[id];
            b->tex[0] = c; b->tex[1] = r; b->tex[2] = n;
            out->mats[out->nmats++] = id;
        }
        ok = ok && out->nmats > 0;
    }
    if (d) cgltf_free(d);
    if (!ok) fprintf(stderr, "furniture: can't read %s (tools/interior_models.py makes it; is Git LFS pulled?)\n", path);
    return ok;
}

/* A unit cube with a whole image on each face, upright from the front (+z) and, for the top, from +z's side. */
static int card_mesh(Scene *s) {
    static const float C[6][12] = {   /* each face: four corners, counter-clockwise from outside, from image top left */
        { -0.5f, 0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f, 0.5f, 0.5f },       /* +z */
        { 0.5f, 0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,  -0.5f, 0.5f, -0.5f },   /* -z */
        { -0.5f, 0.5f, -0.5f,  -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, -0.5f },       /* +y */
        { -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f, 0.5f },   /* -y */
        { 0.5f, 0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f, -0.5f, -0.5f,  0.5f, 0.5f, -0.5f },       /* +x */
        { -0.5f, 0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f, 0.5f },   /* -x */
    };
    static const float UV[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
    float v[72], a[120];
    uint32_t ix[36];
    memset(a, 0, sizeof(a));
    for (int f = 0; f < 6; ++f) {
        memcpy(v + f * 12, C[f], sizeof(C[f]));
        for (int c = 0; c < 4; ++c) { a[(f * 4 + c) * 5 + 3] = UV[c][0]; a[(f * 4 + c) * 5 + 4] = UV[c][1]; }
        uint32_t b = (uint32_t)f * 4;
        uint32_t t[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
        memcpy(ix + f * 6, t, sizeof(t));
    }
    return mesh_add_attr(s, v, a, 24, ix, 36);
}

/* A book, a cuboid wearing interior_models.py's layout: covers on the image's left 5/8, the spine in the
 * next 1/8, page edges on the last 1/4. Lying flat (cover on +y, head towards -z, spine on -x), or standing
 * (head up, spine on +z, covers on +-x). A unit cube; instances scale it to the book. */
static int book_mesh(Scene *s, bool standing) {
    /* lying flat; corners from image top left, counter-clockwise from outside */
    static const float C[6][12] = {
        { -0.5f, 0.5f, -0.5f,  -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f, -0.5f },       /* +y: the front cover */
        { 0.5f, -0.5f, -0.5f,  0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f },   /* -y: the back cover */
        { -0.5f, -0.5f, -0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, -0.5f },   /* -x: the spine */
        { 0.5f, 0.5f, -0.5f,  0.5f, 0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f, -0.5f, -0.5f },       /* +x: the fore-edge */
        { 0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f },       /* +z: the tail */
        { -0.5f, 0.5f, -0.5f,  0.5f, 0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f },   /* -z: the head */
    };
    const float cover[4] = { 0.0f, 0.0f, 0.625f, 1.0f }, spine[4] = { 0.625f, 0.0f, 0.75f, 1.0f }, pages[4] = { 0.75f, 0.0f, 1.0f, 1.0f };
    const float *R[6] = { cover, cover, spine, pages, pages, pages };
    float v[72], a[120];
    uint32_t ix[36];
    memset(a, 0, sizeof(a));
    for (int f = 0; f < 6; ++f) {
        const float *r = R[f];
        const float uv[4][2] = { { r[0], r[1] }, { r[0], r[3] }, { r[2], r[3] }, { r[2], r[1] } };
        for (int c = 0; c < 4; ++c) {
            float x = C[f][c * 3], y = C[f][c * 3 + 1], z = C[f][c * 3 + 2];
            /* standing: the flat book's spine edge (-x) turns to +z, its cover (+y) to +x, its head (-z) up */
            float *o = v + (f * 4 + c) * 3;
            if (standing) { o[0] = y; o[1] = -z; o[2] = -x; }
            else          { o[0] = x; o[1] = y;  o[2] = z; }
            a[(f * 4 + c) * 5 + 3] = uv[c][0];
            a[(f * 4 + c) * 5 + 4] = uv[c][1];
        }
        uint32_t b = (uint32_t)f * 4;
        uint32_t t[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
        memcpy(ix + f * 6, t, sizeof(t));
    }
    return mesh_add_attr(s, v, a, 24, ix, 36);
}

static int sheet_mat(Scene *s, int layer, float rough) {
    int id = mat_add(s, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    mat_surface(s, id, rough, 0.0f, 0.0f, 0.0f);
    s->mats[id].tex[0] = layer;
    return id;
}

/* The emission a material gives lumens over area at a temperature, as mat_light works it out. */
static void relight(Scene *s, int id, float lumens, float area, float kelvin) {
    float c[3], L = lumens / (PI_F * area) * BAKE_NIT;
    kelvin_unit(kelvin, c);
    for (int i = 0; i < 3; ++i) s->mats[id].emission[i] = c[i] * L;
}

bool furniture_load(Scene *s, Kit *k, Furniture *f, const char *assets) {
    char dir[1100], tex[1200], fixture[1200];
    snprintf(dir, sizeof(dir), "%s/../assets_src/interiors", assets);
    snprintf(tex, sizeof(tex), "%s/textures", dir);
    snprintf(fixture, sizeof(fixture), "%s/textures/light_fixture", assets);
    bool ok = piece_load(s, &f->chair, dir, "chair") && piece_load(s, &f->desk, dir, "desk") && piece_load(s, &f->computer, dir, "computer")
           && piece_load(s, &f->lamp, dir, "lamp") && piece_load(s, &f->partition, dir, "partition") && piece_load(s, &f->cabinet, dir, "filing_cabinet")
           && piece_load(s, &f->blackboard, dir, "blackboard") && piece_load(s, &f->intray, dir, "intray") && piece_load(s, &f->phone, dir, "phone")
           && piece_load(s, &f->pen, dir, "fountain_pen") && piece_load(s, &f->armchair, dir, "armchair") && piece_load(s, &f->printer, dir, "printer")
           && piece_load(s, &f->sideTable, dir, "side_table") && piece_load(s, &f->table, dir, "conference_table");
    if (!ok) return false;
    f->card = card_mesh(s);
    f->bookFlat = book_mesh(s, false);
    f->bookUp = book_mesh(s, true);
    char name[64];
    for (int i = 0; i < FURNITURE_BOOKS; ++i) {
        snprintf(name, sizeof(name), "book_%d.jpg", i);
        int l = image_file(s, tex, name, true);
        if (l < 0) return false;
        f->book[i] = sheet_mat(s, l, 0.42f);
    }
    int rc = image_file(s, tex, "rug_albedo.jpg", true), rn = image_file(s, tex, "rug_normal.jpg", false);
    if (rc < 0 || rn < 0) return false;
    f->rug = sheet_mat(s, rc, 1.0f);
    s->mats[f->rug].tex[2] = rn;
    for (int i = 0; i < 4; ++i) {
        snprintf(name, sizeof(name), "paper_%d.png", i);
        int l = image_file(s, tex, name, true);
        if (l < 0) return false;
        f->paper[i] = sheet_mat(s, l, 0.8f);
    }
    for (int i = 0; i < 6; ++i) {
        snprintf(name, sizeof(name), "chart_%d.png", i);
        int l = image_file(s, tex, name, true);
        if (l < 0) return false;
        f->chart[i] = sheet_mat(s, l, 0.7f);
    }
    /* A CRT's face is a sixth of a flat panel's area: it glows brighter, for a third of the light on the desk. */
    const BakeMat screen = s->mats[k->screenOn];
    for (int i = 0; i < 4; ++i) {
        snprintf(name, sizeof(name), "screen_%d.png", i);
        int l = image_file(s, tex, name, true);
        if (l < 0) return false;
        int id = mat_add(s, screen.albedo[0], screen.albedo[1], screen.albedo[2], 2.0f, 2.0f, 2.0f);
        mat_surface(s, id, screen.albedo[3], 0.0f, 0.0f, 0.0f);
        s->mats[id].surface[3] = screen.surface[3];
        s->mats[id].tex[3] = l;
        f->screen[i] = id;
    }
    /* The ducts: the supplied steel, box mapped, dulled from its clean sheet (albedo 0.74, roughness 0.27) to
     * galvanised: as supplied, the ducts' undersides mirror the screens below them. */
    int mc = image_file(s, tex, "metal_albedo.jpg", true), mo = image_file(s, tex, "metal_orm.jpg", false), mn = image_file(s, tex, "metal_normal.jpg", false);
    if (mc < 0 || mo < 0 || mn < 0) return false;
    BakeMat *duct = &s->mats[k->duct];
    duct->albedo[0] = duct->albedo[1] = duct->albedo[2] = 0.5f;
    duct->albedo[3] = 1.8f;
    duct->emission[3] = 1.0f;
    duct->tex[0] = mc; duct->tex[1] = mo; duct->tex[2] = mn;
    duct->tile[0] = 1.5f;
    /* the ceiling lights: the lobby's fitting (assets/materials/light_fixture.json) on a square of 1.2 m */
    int fc = image_file(s, fixture, "albedo.png", true), fo = image_file(s, fixture, "orm.png", false), fn = image_file(s, fixture, "normal.png", false), fe = image_file(s, fixture, "emissive.png", true);
    if (fc < 0 || fo < 0 || fn < 0 || fe < 0) return false;
    BakeMat *off = &s->mats[k->trofferOff];
    off->albedo[0] = off->albedo[1] = off->albedo[2] = 1.0f;
    off->albedo[3] = 1.0f;
    off->tex[0] = fc; off->tex[1] = fo; off->tex[2] = fn;
    BakeMat *on = &s->mats[k->trofferOn];
    on->albedo[0] = on->albedo[1] = on->albedo[2] = 1.0f;
    on->tex[0] = fc; on->tex[3] = fe;
    relight(s, k->trofferOn, LM_TROFFER, 1.2f * 1.2f, 4000.0f);
    /* the lamp lights by the panel in its shade, the desk's strip by the part in its kneehole */
    relight(s, k->lampOn, LM_LAMP, f->lamp.extraArea, 2700.0f);
    relight(s, k->stripOn, LM_STRIP, 0.6f * 0.015f, 3000.0f);
    return true;
}

void piece_place(Scene *s, const Piece *p, int variant, uint32_t flags, float x, float y, float z, float yaw, float sx, float sy, float sz) {
    inst_add(s, p->mesh, p->mats[variant % p->nmats], flags, x, y, z, sx, sy, sz, yaw);
}
