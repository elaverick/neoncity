/* model_pack: converts a textured glTF binary (.glb) into one of NeonCity's
 * models: geometry compiled into the generator, maps loaded at run time as an
 * authored PBR material.
 *
 *   model_pack <in.glb> <name> [options]
 *
 *   --height H | --width W | --depth D   scale (uniformly) to this size, metres (default: as is)
 *   --yaw DEG        turn about the vertical first (counter-clockwise from above), so the front faces +z
 *   --stretch Y DY   lengthen whatever passes through height Y (metres, after scaling, from the
 *                    model's foot) by DY: everything above Y rises by DY, so a post grows
 *   --tris N         simplify to about N triangles (default 800)
 *   --tex PX         the maps' size (default 1024)
 *   --ao RAYS        ambient occlusion rays per texel (default 24; 0: none)
 *   --floor          the ground under the model occludes too (it stands on it)
 *   --glow           also bake emissive.png: the pale figures printed on the model's front,
 *                    those on its left half in red, its right in green (a pedestrian signal)
 *   --inc FILE       write the geometry as C (default src/city/models/<name>.inc)
 *   --maps DIR       write the maps here (default assets/textures/<name>)
 *   --preview FILE   draw the result from the front, the side, above and three-quarters (PNG)
 *
 * The files this is for are scans, or made like them: a million triangles in
 * hundreds of texture islands, each tessellated on its own, so no simplifier
 * can reduce them and keep their texture coordinates. So it starts again: the
 * shape alone is simplified (clustered, then collapsed), unwrapped into a new
 * atlas (xatlas), and every texel of that atlas is baked from the original by
 * casting a ray into it along the new surface's normal. One material comes out
 * however many the file has:
 *   albedo.png  base colour, sRGB
 *   normal.png  tangent-space normal, OpenGL convention (+Y green = image up),
 *               in the frame the city's shader builds from the surface's
 *               derivatives (shaders/city/props.glsl): T along +u, B along -v,
 *               both made square to the interpolated normal
 *   orm.png     R = ambient occlusion, G = roughness, B = metalness
 * The model stands on y = 0, centred on x and z, its front towards +z. The
 * geometry (<name>.inc) is indexed triangles in metres: see src/city/props.c,
 * which includes it. */
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <meshoptimizer.h>
#include <xatlas_c.h>

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "model_pack: %s%s%s\n", msg, arg ? " " : "", arg ? arg : "");
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory", NULL);
    return p;
}

typedef struct { float x, y, z; } v3;
static v3 V3(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static v3 add(v3 a, v3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 mul(v3 a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(v3 a) { return sqrtf(dot(a, a)); }
static v3 norm(v3 a) { float l = len(a); return l > 1e-20f ? mul(a, 1.0f / l) : V3(0, 1, 0); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
static unsigned char to_byte(float c) { c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c; return (unsigned char)(c * 255.0f + 0.5f); }

/* ------------------------------------------------------------- the file --- */

typedef struct Img { int w, h; unsigned char *px; } Img;   /* RGBA8 */
typedef struct Mat { Img albedo, mr, normal, ao; float baseFactor[3], roughF, metalF, normalScale; } Mat;

/* The original: every triangle of the file, in the model's frame. */
typedef struct High {
    v3 *p, *n;
    float (*uv)[2];
    size_t nv;
    unsigned *ix;     /* 3 per triangle */
    int *mat;         /* per triangle */
    size_t nt;
    Mat mats[16];
    int nm;
} High;

static Img load_image(const cgltf_texture_view *tv, const char *what) {
    Img im = { 0 };
    if (!tv->texture || !tv->texture->image) return im;
    const cgltf_image *ci = tv->texture->image;
    if (!ci->buffer_view) die("image not embedded in the file:", what);
    const unsigned char *src = (const unsigned char *)ci->buffer_view->buffer->data + ci->buffer_view->offset;
    int c;
    im.px = stbi_load_from_memory(src, (int)ci->buffer_view->size, &im.w, &im.h, &c, 4);
    if (!im.px) die("can't decode the image:", what);
    printf("    %-11s %dx%d\n", what, im.w, im.h);
    return im;
}

static Mat load_material(const cgltf_material *m) {
    Mat M;
    memset(&M, 0, sizeof(M));
    if (!m || !m->has_pbr_metallic_roughness) die("a material isn't metallic-roughness PBR", NULL);
    const cgltf_pbr_metallic_roughness *pb = &m->pbr_metallic_roughness;
    M.albedo = load_image(&pb->base_color_texture, "albedo");
    M.mr = load_image(&pb->metallic_roughness_texture, "metal-rough");
    M.normal = load_image(&m->normal_texture, "normal");
    M.ao = load_image(&m->occlusion_texture, "occlusion");
    for (int k = 0; k < 3; ++k) M.baseFactor[k] = pb->base_color_factor[k];
    M.roughF = pb->roughness_factor;
    M.metalF = pb->metallic_factor;
    M.normalScale = m->normal_texture.texture && m->normal_texture.scale > 0.0f ? m->normal_texture.scale : 1.0f;
    return M;
}

/* Bilinear, wrapping (the files' samplers repeat). */
static void sample(const Img *im, float u, float v, float *out, int srgb) {
    if (!im->px) { out[0] = out[1] = out[2] = -1.0f; return; }
    float x = u * im->w - 0.5f, y = v * im->h - 0.5f;
    float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    int x0 = (int)fx, y0 = (int)fy;
    float acc[3] = { 0, 0, 0 };
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int xi = ((x0 + i) % im->w + im->w) % im->w, yi = ((y0 + j) % im->h + im->h) % im->h;
            const unsigned char *p = im->px + ((size_t)yi * im->w + xi) * 4;
            float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
            for (int k = 0; k < 3; ++k) acc[k] += w * (srgb ? srgb_to_linear(p[k] / 255.0f) : p[k] / 255.0f);
        }
    for (int k = 0; k < 3; ++k) out[k] = acc[k];
}

static void read_file(const char *in, High *H) {
    cgltf_options opt;
    memset(&opt, 0, sizeof(opt));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&opt, in, &data) != cgltf_result_success) die("can't read", in);
    if (cgltf_load_buffers(&opt, data, in) != cgltf_result_success) die("can't load the buffers of", in);
    const cgltf_material *mats[16];
    memset(H, 0, sizeof(*H));
    size_t capv = 0, capt = 0;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            H->p = (v3 *)xmalloc(sizeof(v3) * capv);
            H->n = (v3 *)xmalloc(sizeof(v3) * capv);
            H->uv = (float (*)[2])xmalloc(sizeof(float) * 2 * capv);
            H->ix = (unsigned *)xmalloc(sizeof(unsigned) * 3 * capt);
            H->mat = (int *)xmalloc(sizeof(int) * capt);
        }
        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
            const cgltf_node *nd = &data->nodes[ni];
            if (!nd->mesh) continue;
            float mtx[16];
            cgltf_node_transform_world(nd, mtx);
            for (cgltf_size pi = 0; pi < nd->mesh->primitives_count; ++pi) {
                const cgltf_primitive *pr = &nd->mesh->primitives[pi];
                if (pr->type != cgltf_primitive_type_triangles) continue;
                const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL;
                for (cgltf_size a = 0; a < pr->attributes_count; ++a) {
                    const cgltf_attribute *at = &pr->attributes[a];
                    if (at->type == cgltf_attribute_type_position) pos = at->data;
                    else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
                    else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv = at->data;
                }
                if (!pos || !nrm || !uv) die("a primitive lacks positions, normals or texture coordinates", NULL);
                size_t nt = (pr->indices ? pr->indices->count : pos->count) / 3;
                if (pass == 0) { capv += pos->count; capt += nt; continue; }
                int mi = 0;
                while (mi < H->nm && mats[mi] != pr->material) mi++;
                if (mi == H->nm) {
                    if (H->nm == 16) die("too many materials", NULL);
                    printf("  material %d\n", mi);
                    mats[H->nm] = pr->material;
                    H->mats[H->nm++] = load_material(pr->material);
                }
                size_t base = H->nv;
                for (size_t i = 0; i < pos->count; ++i) {
                    float p[3], n[3], t[2];
                    cgltf_accessor_read_float(pos, i, p, 3);
                    cgltf_accessor_read_float(nrm, i, n, 3);
                    cgltf_accessor_read_float(uv, i, t, 2);
                    float wp[3], wn[3];
                    for (int k = 0; k < 3; ++k) {
                        wp[k] = mtx[k] * p[0] + mtx[4 + k] * p[1] + mtx[8 + k] * p[2] + mtx[12 + k];
                        wn[k] = mtx[k] * n[0] + mtx[4 + k] * n[1] + mtx[8 + k] * n[2];
                    }
                    H->p[H->nv] = V3(wp[0], wp[1], wp[2]);
                    H->n[H->nv] = norm(V3(wn[0], wn[1], wn[2]));
                    H->uv[H->nv][0] = t[0]; H->uv[H->nv][1] = t[1];
                    H->nv++;
                }
                for (size_t t = 0; t < nt; ++t) {
                    for (int k = 0; k < 3; ++k)
                        H->ix[H->nt * 3 + k] = (unsigned)(base + (pr->indices ? cgltf_accessor_read_index(pr->indices, t * 3 + k) : t * 3 + k));
                    H->mat[H->nt++] = mi;
                }
            }
        }
    }
    if (!H->nt) die("no triangles in", in);
    cgltf_free(data);   /* the images were decoded already */
}

/* ------------------------------------------------------------------ BVH --- */

typedef struct Node { float mn[3], mx[3]; int left, first, count; } Node;   /* count 0: inner, children left, left + 1 */
typedef struct Bvh { Node *node; int nn; unsigned *tri; const High *H; } Bvh;

static void tri_bounds(const High *H, unsigned t, float *mn, float *mx) {
    for (int k = 0; k < 3; ++k) { mn[k] = FLT_MAX; mx[k] = -FLT_MAX; }
    for (int c = 0; c < 3; ++c) {
        const float *p = &H->p[H->ix[t * 3 + c]].x;
        for (int k = 0; k < 3; ++k) { mn[k] = fminf(mn[k], p[k]); mx[k] = fmaxf(mx[k], p[k]); }
    }
}

static float *g_cent;   /* triangles' centroids, for the build's sort */
static int g_axis;
static int cmp_cent(const void *a, const void *b) {
    float x = g_cent[*(const unsigned *)a * 3 + g_axis], y = g_cent[*(const unsigned *)b * 3 + g_axis];
    return x < y ? -1 : x > y;
}

static void bvh_build(Bvh *B, const High *H) {
    B->H = H;
    B->tri = (unsigned *)xmalloc(sizeof(unsigned) * H->nt);
    B->node = (Node *)xmalloc(sizeof(Node) * (2 * H->nt + 1));
    g_cent = (float *)xmalloc(sizeof(float) * 3 * H->nt);
    for (unsigned t = 0; t < H->nt; ++t) {
        B->tri[t] = t;
        float mn[3], mx[3];
        tri_bounds(H, t, mn, mx);
        for (int k = 0; k < 3; ++k) g_cent[t * 3 + k] = 0.5f * (mn[k] + mx[k]);
    }
    int stack[128], sp = 0;
    B->nn = 1;
    B->node[0].first = 0; B->node[0].count = (int)H->nt;
    stack[sp++] = 0;
    while (sp) {
        Node *nd = &B->node[stack[--sp]];
        for (int k = 0; k < 3; ++k) { nd->mn[k] = FLT_MAX; nd->mx[k] = -FLT_MAX; }
        float cmn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, cmx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int i = 0; i < nd->count; ++i) {
            unsigned t = B->tri[nd->first + i];
            float mn[3], mx[3];
            tri_bounds(H, t, mn, mx);
            for (int k = 0; k < 3; ++k) {
                nd->mn[k] = fminf(nd->mn[k], mn[k]); nd->mx[k] = fmaxf(nd->mx[k], mx[k]);
                cmn[k] = fminf(cmn[k], g_cent[t * 3 + k]); cmx[k] = fmaxf(cmx[k], g_cent[t * 3 + k]);
            }
        }
        if (nd->count <= 4) continue;
        g_axis = cmx[0] - cmn[0] > cmx[1] - cmn[1] ? 0 : 1;
        if (cmx[2] - cmn[2] > cmx[g_axis] - cmn[g_axis]) g_axis = 2;
        if (cmx[g_axis] - cmn[g_axis] <= 0.0f) continue;
        qsort(B->tri + nd->first, (size_t)nd->count, sizeof(unsigned), cmp_cent);
        int half = nd->count / 2, l = B->nn;
        B->nn += 2;
        B->node[l].first = nd->first; B->node[l].count = half;
        B->node[l + 1].first = nd->first + half; B->node[l + 1].count = nd->count - half;
        nd->left = l; nd->count = 0;
        if (sp + 2 > 128) die("BVH too deep", NULL);
        stack[sp++] = l; stack[sp++] = l + 1;
    }
    free(g_cent);
}

static int slab(const Node *nd, v3 o, v3 inv, float t0, float t1) {
    const float *oo = &o.x, *iv = &inv.x;
    for (int k = 0; k < 3; ++k) {
        float a = (nd->mn[k] - oo[k]) * iv[k], b = (nd->mx[k] - oo[k]) * iv[k];
        if (a > b) { float c = a; a = b; b = c; }
        t0 = fmaxf(t0, a); t1 = fminf(t1, b);
        if (t0 > t1) return 0;
    }
    return 1;
}

/* The hit along o + t d with t in [t0, t1] nearest t = 0 (any: the first one found). */
typedef struct Hit { int tri; float t, b1, b2; } Hit;
static Hit bvh_cast(const Bvh *B, v3 o, v3 d, float t0, float t1, int any) {
    Hit h = { -1, FLT_MAX, 0, 0 };
    v3 inv = V3(1.0f / (fabsf(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (fabsf(d.y) > 1e-12f ? d.y : 1e-12f), 1.0f / (fabsf(d.z) > 1e-12f ? d.z : 1e-12f));
    int stack[128], sp = 0;
    stack[sp++] = 0;
    const High *H = B->H;
    while (sp) {
        const Node *nd = &B->node[stack[--sp]];
        float lo = t0, hi = t1;
        if (h.tri >= 0) { lo = fmaxf(lo, -fabsf(h.t)); hi = fminf(hi, fabsf(h.t)); }
        if (!slab(nd, o, inv, lo, hi)) continue;
        if (nd->count == 0) { stack[sp++] = nd->left; stack[sp++] = nd->left + 1; continue; }
        for (int i = 0; i < nd->count; ++i) {
            unsigned t = B->tri[nd->first + i];
            v3 a = H->p[H->ix[t * 3]], b = H->p[H->ix[t * 3 + 1]], c = H->p[H->ix[t * 3 + 2]];
            v3 e1 = sub(b, a), e2 = sub(c, a), pv = cross(d, e2);
            float det = dot(e1, pv);
            if (fabsf(det) < 1e-14f) continue;
            float id = 1.0f / det;
            v3 tv = sub(o, a);
            float u = dot(tv, pv) * id;
            if (u < 0.0f || u > 1.0f) continue;
            v3 qv = cross(tv, e1);
            float v = dot(d, qv) * id;
            if (v < 0.0f || u + v > 1.0f) continue;
            float tt = dot(e2, qv) * id;
            if (tt < t0 || tt > t1 || fabsf(tt) >= fabsf(h.t)) continue;
            h.tri = (int)t; h.t = tt; h.b1 = u; h.b2 = v;
            if (any) return h;
        }
    }
    return h;
}

/* ----------------------------------------------------------- the shape --- */

typedef struct Low { v3 *p, *n; float (*uv)[2]; size_t nv; unsigned *ix; size_t ni; } Low;

/* The shape alone, simplified: positions welded, clustered down if the
 * collapses can't get there on their own (the islands' borders hold them), then collapsed. */
static void simplify_shape(const High *H, size_t tris, v3 **outP, size_t *outNv, unsigned **outIx, size_t *outNi) {
    unsigned *remap = (unsigned *)xmalloc(sizeof(unsigned) * H->nv);
    size_t nv = meshopt_generateVertexRemap(remap, H->ix, H->nt * 3, H->p, H->nv, sizeof(v3));
    v3 *p = (v3 *)xmalloc(sizeof(v3) * nv);
    unsigned *ix = (unsigned *)xmalloc(sizeof(unsigned) * H->nt * 3), *tmp = (unsigned *)xmalloc(sizeof(unsigned) * H->nt * 3);
    meshopt_remapVertexBuffer(p, H->p, H->nv, sizeof(v3), remap);
    meshopt_remapIndexBuffer(ix, H->ix, H->nt * 3, remap);
    free(remap);
    size_t ni = H->nt * 3, target = tris * 3;
    float err = 0.0f;
    size_t n = meshopt_simplify(tmp, ix, ni, &p[0].x, nv, sizeof(v3), target, FLT_MAX, 0, &err);
    printf("  collapsed %zu -> %zu triangles (error %.4f)\n", ni / 3, n / 3, err);
    if (n > target * 5 / 4) {
        n = meshopt_simplifySloppy(tmp, ix, ni, &p[0].x, nv, sizeof(v3), NULL, target * 3, FLT_MAX, &err);
        printf("  clustered to %zu triangles (error %.4f)\n", n / 3, err);
        memcpy(ix, tmp, sizeof(unsigned) * n);
        ni = n;
        n = meshopt_simplify(tmp, ix, ni, &p[0].x, nv, sizeof(v3), target, FLT_MAX, 0, &err);
        printf("  collapsed to %zu triangles (error %.4f)\n", n / 3, err);
    }
    memcpy(ix, tmp, sizeof(unsigned) * n);
    free(tmp);
    ni = n;
    remap = (unsigned *)xmalloc(sizeof(unsigned) * nv);
    size_t used = meshopt_optimizeVertexFetchRemap(remap, ix, ni, nv);
    v3 *q = (v3 *)xmalloc(sizeof(v3) * used);
    meshopt_remapVertexBuffer(q, p, nv, sizeof(v3), remap);
    meshopt_remapIndexBuffer(ix, ix, ni, remap);
    free(remap); free(p);
    *outP = q; *outNv = used; *outIx = ix; *outNi = ni;
}

/* Smooth normals, split at creases sharper than `crease` degrees: each corner
 * averages the faces round its vertex that lie within the crease of its own. */
static void crease_normals(const v3 *p, size_t nv, const unsigned *ix, size_t ni, float crease, Low *L) {
    size_t nt = ni / 3;
    v3 *fn = (v3 *)xmalloc(sizeof(v3) * nt);
    for (size_t t = 0; t < nt; ++t) fn[t] = cross(sub(p[ix[t * 3 + 1]], p[ix[t * 3]]), sub(p[ix[t * 3 + 2]], p[ix[t * 3]]));   /* area-weighted */
    /* the faces round each vertex */
    int *cnt = (int *)calloc(nv + 1, sizeof(int)), *list = (int *)xmalloc(sizeof(int) * ni);
    for (size_t i = 0; i < ni; ++i) cnt[ix[i] + 1]++;
    for (size_t v = 0; v < nv; ++v) cnt[v + 1] += cnt[v];
    int *fill = (int *)xmalloc(sizeof(int) * nv);
    memcpy(fill, cnt, sizeof(int) * nv);
    for (size_t i = 0; i < ni; ++i) list[fill[ix[i]]++] = (int)(i / 3);
    free(fill);
    float cc = cosf(crease * 3.14159265f / 180.0f);
    L->p = (v3 *)xmalloc(sizeof(v3) * ni);
    L->n = (v3 *)xmalloc(sizeof(v3) * ni);
    L->ix = (unsigned *)xmalloc(sizeof(unsigned) * ni);
    L->nv = 0; L->ni = ni;
    for (size_t i = 0; i < ni; ++i) {
        size_t t = i / 3;
        unsigned v = ix[i];
        v3 own = norm(fn[t]), s = V3(0, 0, 0);
        for (int k = cnt[v]; k < cnt[v + 1]; ++k)
            if (dot(norm(fn[list[k]]), own) >= cc) s = add(s, fn[list[k]]);
        L->p[L->nv] = p[v];
        L->n[L->nv] = norm(s);
        L->ix[i] = (unsigned)L->nv++;
    }
    free(cnt); free(list); free(fn);
    /* weld the corners that came out alike */
    typedef struct PN { v3 p, n; } PN;
    PN *pn = (PN *)xmalloc(sizeof(PN) * L->nv);
    for (size_t i = 0; i < L->nv; ++i) { pn[i].p = L->p[i]; pn[i].n = L->n[i]; }
    unsigned *remap = (unsigned *)xmalloc(sizeof(unsigned) * L->nv);
    size_t u = meshopt_generateVertexRemap(remap, L->ix, ni, pn, L->nv, sizeof(PN));
    PN *w = (PN *)xmalloc(sizeof(PN) * u);
    meshopt_remapVertexBuffer(w, pn, L->nv, sizeof(PN), remap);
    meshopt_remapIndexBuffer(L->ix, L->ix, ni, remap);
    for (size_t i = 0; i < u; ++i) { L->p[i] = w[i].p; L->n[i] = w[i].n; }
    L->nv = u;
    free(pn); free(w); free(remap);
}

/* A new atlas for the simplified shape. */
static void unwrap(Low *L, int size, int padding) {
    xatlasAtlas *atlas = xatlasCreate();
    xatlasMeshDecl md;
    xatlasMeshDeclInit(&md);
    md.vertexPositionData = L->p; md.vertexPositionStride = sizeof(v3);
    md.vertexNormalData = L->n; md.vertexNormalStride = sizeof(v3);
    md.vertexCount = (uint32_t)L->nv;
    md.indexData = L->ix; md.indexCount = (uint32_t)L->ni; md.indexFormat = XATLAS_INDEX_FORMAT_UINT32;
    if (xatlasAddMesh(atlas, &md, 1) != XATLAS_ADD_MESH_ERROR_SUCCESS) die("xatlas won't take the mesh", NULL);
    xatlasChartOptions co;
    xatlasChartOptionsInit(&co);
    xatlasComputeCharts(atlas, &co);
    /* packed at a density that fills `size`, which xatlas can overshoot: then again, a little sparser */
    xatlasPackOptions po;
    xatlasPackOptionsInit(&po);
    po.resolution = (uint32_t)size;
    po.padding = (uint32_t)padding;
    po.bilinear = true;
    po.bruteForce = true;
    xatlasPackCharts(atlas, &po);
    for (int tries = 0; tries < 8 && (atlas->width > (uint32_t)size || atlas->height > (uint32_t)size); ++tries) {
        float big = (float)(atlas->width > atlas->height ? atlas->width : atlas->height);
        po.texelsPerUnit = atlas->texelsPerUnit * (float)size / big * 0.98f;
        po.resolution = 0;   /* one atlas, as big as it takes */
        xatlasPackCharts(atlas, &po);
    }
    /* the texture coordinates are in texels of the whole map, which is `size` square */
    atlas->width = atlas->height = (uint32_t)size;
    if (atlas->atlasCount != 1) die("the charts didn't fit one atlas", NULL);
    const xatlasMesh *m = &atlas->meshes[0];
    printf("  atlas: %u charts, %ux%u, %.0f%% used, %u vertices\n", atlas->chartCount, atlas->width, atlas->height,
           atlas->utilization[0] * 100.0f, m->vertexCount);
    v3 *p = (v3 *)xmalloc(sizeof(v3) * m->vertexCount), *n = (v3 *)xmalloc(sizeof(v3) * m->vertexCount);
    float (*uv)[2] = (float (*)[2])xmalloc(sizeof(float) * 2 * m->vertexCount);
    for (uint32_t i = 0; i < m->vertexCount; ++i) {
        const xatlasVertex *v = &m->vertexArray[i];
        p[i] = L->p[v->xref]; n[i] = L->n[v->xref];
        uv[i][0] = v->uv[0] / (float)atlas->width;
        uv[i][1] = v->uv[1] / (float)atlas->height;
    }
    free(L->p); free(L->n);
    L->p = p; L->n = n; L->uv = uv; L->nv = m->vertexCount;
    if (m->indexCount != L->ni) die("xatlas changed the triangles", NULL);
    for (uint32_t i = 0; i < m->indexCount; ++i) L->ix[i] = m->indexArray[i];
    xatlasDestroy(atlas);
}

/* ----------------------------------------------------------------- bake --- */

/* A triangle's texture frame: T along +u, B along -v (image up), made square to n. */
static void frame(v3 p0, v3 p1, v3 p2, const float *t0, const float *t1, const float *t2, v3 n, v3 *T, v3 *B) {
    v3 e1 = sub(p1, p0), e2 = sub(p2, p0);
    float du1 = t1[0] - t0[0], dv1 = t1[1] - t0[1], du2 = t2[0] - t0[0], dv2 = t2[1] - t0[1];
    float det = du1 * dv2 - du2 * dv1;
    if (fabsf(det) < 1e-20f) det = 1e-20f;
    v3 dpdu = mul(sub(mul(e1, dv2), mul(e2, dv1)), 1.0f / det);
    v3 dpdv = mul(sub(mul(e2, du1), mul(e1, du2)), 1.0f / det);
    v3 t = norm(sub(dpdu, mul(n, dot(n, dpdu))));
    float hand = dot(cross(n, t), mul(dpdv, -1.0f)) < 0.0f ? -1.0f : 1.0f;
    *T = t;
    *B = mul(cross(n, t), hand);
}

typedef struct Sample { float albedo[3], rough, metal, occ; v3 n; } Sample;

/* What the original's surface is at triangle t, barycentrics (b1, b2): its maps there, its normal mapped. */
static Sample surface(const High *H, int t, float b1, float b2) {
    Sample s;
    const unsigned *ix = &H->ix[t * 3];
    float b0 = 1.0f - b1 - b2;
    float u = b0 * H->uv[ix[0]][0] + b1 * H->uv[ix[1]][0] + b2 * H->uv[ix[2]][0];
    float v = b0 * H->uv[ix[0]][1] + b1 * H->uv[ix[1]][1] + b2 * H->uv[ix[2]][1];
    const Mat *M = &H->mats[H->mat[t]];
    float c[3];
    sample(&M->albedo, u, v, c, 1);
    for (int k = 0; k < 3; ++k) s.albedo[k] = (c[0] < 0.0f ? 1.0f : c[k]) * M->baseFactor[k];
    sample(&M->mr, u, v, c, 0);
    s.rough = (c[0] < 0.0f ? 1.0f : c[1]) * M->roughF;
    s.metal = (c[0] < 0.0f ? 1.0f : c[2]) * M->metalF;
    sample(&M->ao, u, v, c, 0);
    s.occ = c[0] < 0.0f ? 1.0f : c[0];
    v3 n = norm(add(add(mul(H->n[ix[0]], b0), mul(H->n[ix[1]], b1)), mul(H->n[ix[2]], b2)));
    sample(&M->normal, u, v, c, 0);
    if (c[0] >= 0.0f) {
        v3 T, B;
        frame(H->p[ix[0]], H->p[ix[1]], H->p[ix[2]], H->uv[ix[0]], H->uv[ix[1]], H->uv[ix[2]], n, &T, &B);
        v3 tn = V3((c[0] * 2.0f - 1.0f) * M->normalScale, (c[1] * 2.0f - 1.0f) * M->normalScale, c[2] * 2.0f - 1.0f);
        n = norm(add(add(mul(T, tn.x), mul(B, tn.y)), mul(n, tn.z)));
    }
    s.n = n;
    return s;
}

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

/* How open the hemisphere over p (normal n) is: rays at it from the original's surface, and the floor's. */
static float occlusion(const Bvh *B, v3 p, v3 n, int rays, float reach, int floor_, uint32_t seed) {
    if (rays <= 0) return 1.0f;
    v3 t = norm(fabsf(n.y) < 0.9f ? cross(n, V3(0, 1, 0)) : cross(n, V3(1, 0, 0))), b = cross(n, t);
    int open = 0;
    v3 o = add(p, mul(n, reach * 0.004f));
    for (int r = 0; r < rays; ++r) {
        float u1 = ((r + (hash32(seed + r * 2u) & 0xFFFF) / 65536.0f)) / rays;   /* stratified in the elevation */
        float u2 = (hash32(seed * 7u + r * 2u + 1u) & 0xFFFFFF) / 16777216.0f;
        float rr = sqrtf(u1), phi = 6.2831853f * u2;
        v3 d = norm(add(add(mul(t, rr * cosf(phi)), mul(b, rr * sinf(phi))), mul(n, sqrtf(fmaxf(0.0f, 1.0f - u1)))));
        if (floor_ && d.y < 0.0f && -o.y / d.y < reach) continue;   /* the ground it stands on */
        if (bvh_cast(B, o, d, 0.0f, reach, 1).tri < 0) open++;
    }
    return (float)open / rays;
}

/* The maps being baked; emis (--glow) is NULL unless asked for: R the figures
 * lit on the model's left half (x < 0), G those on its right, on its front
 * (z >= glowZ) inside its frame (|x| <= glowX, glowY0 <= y <= glowY1): a
 * signal head's hand and walking man. */
typedef struct Out { int size; float *albedo, *orm, *nrm, *emis; float glowZ, glowX, glowY0, glowY1; unsigned char *mask; } Out;

/* Bakes every texel the low mesh covers: ss x ss samples a texel, each cast
 * along the low surface's normal into the original, from `reach` out to `reach` in. */
static void bake(const Low *L, const High *H, const Bvh *B, Out *O, float reach, int aoRays, float aoReach, int floor_) {
    int S = O->size;
    const int ss = 2;
    int nt = (int)(L->ni / 3), t;
    long missed = 0, hit = 0;
#pragma omp parallel for schedule(dynamic, 4) reduction(+ : missed, hit)
    for (t = 0; t < nt; ++t) {
        const unsigned *ix = &L->ix[t * 3];
        float px[3], py[3];
        for (int k = 0; k < 3; ++k) { px[k] = L->uv[ix[k]][0] * S; py[k] = L->uv[ix[k]][1] * S; }
        float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (fabsf(area) < 1e-12f) continue;
        v3 fnrm = norm(cross(sub(L->p[ix[1]], L->p[ix[0]]), sub(L->p[ix[2]], L->p[ix[0]])));
        int x0 = (int)floorf(fminf(px[0], fminf(px[1], px[2]))) - 1, x1 = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2]))) + 1;
        int y0 = (int)floorf(fminf(py[0], fminf(py[1], py[2]))) - 1, y1 = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2]))) + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > S - 1) x1 = S - 1;
        if (y1 > S - 1) y1 = S - 1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float acc[3] = { 0, 0, 0 }, rough = 0, metal = 0, occ = 0, glow[2] = { 0, 0 };
                v3 nacc = V3(0, 0, 0);
                int got = 0, inside = 0;
                for (int sj = 0; sj < ss; ++sj)
                    for (int si = 0; si < ss; ++si) {
                        float sx = x + (si + 0.5f) / ss, sy = y + (sj + 0.5f) / ss;
                        float w1 = ((px[2] - px[0]) * (sy - py[0]) - (py[2] - py[0]) * (sx - px[0])) / -area;
                        float w2 = ((px[1] - px[0]) * (sy - py[0]) - (py[1] - py[0]) * (sx - px[0])) / area;
                        float w0 = 1.0f - w1 - w2;
                        /* a little past the edges too, so the charts' borders are filled */
                        const float e = -0.02f;
                        if (w0 < e || w1 < e || w2 < e) continue;
                        inside++;
                        v3 P = add(add(mul(L->p[ix[0]], w0), mul(L->p[ix[1]], w1)), mul(L->p[ix[2]], w2));
                        v3 N = norm(add(add(mul(L->n[ix[0]], w0), mul(L->n[ix[1]], w1)), mul(L->n[ix[2]], w2)));
                        Hit h = bvh_cast(B, P, N, -reach, reach, 0);
                        if (h.tri < 0) h = bvh_cast(B, P, fnrm, -reach, reach, 0);
                        if (h.tri < 0) continue;
                        Sample s = surface(H, h.tri, h.b1, h.b2);
                        v3 T, Bt;
                        frame(L->p[ix[0]], L->p[ix[1]], L->p[ix[2]], L->uv[ix[0]], L->uv[ix[1]], L->uv[ix[2]], N, &T, &Bt);
                        v3 tn = V3(dot(s.n, T), dot(s.n, Bt), dot(s.n, N));
                        if (tn.z < 0.05f) { tn.z = 0.05f; tn = norm(tn); }   /* faces the surface's way, however the original folds */
                        for (int k = 0; k < 3; ++k) acc[k] += s.albedo[k];
                        rough += s.rough; metal += s.metal;
                        nacc = add(nacc, tn);
                        if (O->emis && N.z > 0.6f && P.z >= O->glowZ && fabsf(P.x) <= O->glowX && P.y >= O->glowY0 && P.y <= O->glowY1) {
                            /* pale, unsaturated print on the front: the figures (the frame is saturated yellow, the panel black) */
                            float hi = fmaxf(s.albedo[0], fmaxf(s.albedo[1], s.albedo[2])), lo = fminf(s.albedo[0], fminf(s.albedo[1], s.albedo[2]));
                            float lum = 0.2126f * s.albedo[0] + 0.7152f * s.albedo[1] + 0.0722f * s.albedo[2];
                            if (lum > 0.08f && (hi - lo) < 0.45f * hi) glow[P.x < 0.0f ? 0 : 1] += 1.0f;
                        }
                        if (got == 0) {
                            const unsigned *hx = &H->ix[h.tri * 3];
                            float b0 = 1.0f - h.b1 - h.b2;
                            v3 hp = add(add(mul(H->p[hx[0]], b0), mul(H->p[hx[1]], h.b1)), mul(H->p[hx[2]], h.b2));
                            v3 hn = norm(cross(sub(H->p[hx[1]], H->p[hx[0]]), sub(H->p[hx[2]], H->p[hx[0]])));
                            if (dot(hn, N) < 0.0f) hn = mul(hn, -1.0f);
                            occ = s.occ * occlusion(B, hp, hn, aoRays, aoReach, floor_, (uint32_t)(y * S + x) * 2654435761u);
                        }
                        got++;
                    }
                if (!inside) continue;
                if (!got) { missed++; continue; }
                hit++;
                size_t o = (size_t)y * S + x;
                /* the texel's own triangle wins over a neighbour's skirt: only fill what's empty or less covered */
                if (O->mask[o] >= (unsigned char)got) continue;
                O->mask[o] = (unsigned char)got;
                for (int k = 0; k < 3; ++k) O->albedo[o * 3 + k] = acc[k] / got;
                O->orm[o * 3 + 0] = occ;
                O->orm[o * 3 + 1] = rough / got;
                O->orm[o * 3 + 2] = metal / got;
                v3 n = norm(nacc);
                O->nrm[o * 3 + 0] = n.x; O->nrm[o * 3 + 1] = n.y; O->nrm[o * 3 + 2] = n.z;
                if (O->emis) { O->emis[o * 3] = glow[0] / got; O->emis[o * 3 + 1] = glow[1] / got; O->emis[o * 3 + 2] = 0.0f; }
            }
    }
    printf("  baked %ld texels (%ld found nothing within %.3f m)\n", hit, missed, reach);
}

/* Grows the baked texels outwards over the empty ones, so filtering and mipmaps
 * don't bleed the background in at the charts' edges. */
static void dilate(Out *O) {
    int S = O->size;
    unsigned char *m = O->mask, *nm = (unsigned char *)xmalloc((size_t)S * S);
    for (int it = 0; it < 64; ++it) {
        memcpy(nm, m, (size_t)S * S);
        int grew = 0;
        for (int y = 0; y < S; ++y)
            for (int x = 0; x < S; ++x) {
                size_t o = (size_t)y * S + x;
                if (m[o]) continue;
                float a[11] = { 0 };
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= S || yy >= S) continue;
                        size_t q = (size_t)yy * S + xx;
                        if (!m[q]) continue;
                        for (int k = 0; k < 3; ++k) { a[k] += O->albedo[q * 3 + k]; a[3 + k] += O->orm[q * 3 + k]; a[6 + k] += O->nrm[q * 3 + k]; }
                        if (O->emis) for (int k = 0; k < 2; ++k) a[9 + k] += O->emis[q * 3 + k];
                        n++;
                    }
                if (!n) continue;
                for (int k = 0; k < 3; ++k) { O->albedo[o * 3 + k] = a[k] / n; O->orm[o * 3 + k] = a[3 + k] / n; O->nrm[o * 3 + k] = a[6 + k] / n; }
                if (O->emis) for (int k = 0; k < 2; ++k) O->emis[o * 3 + k] = a[9 + k] / n;
                nm[o] = 1;
                grew = 1;
            }
        memcpy(m, nm, (size_t)S * S);
        if (!grew) break;
    }
    /* whatever is left: flat, mid grey */
    for (size_t o = 0; o < (size_t)S * S; ++o)
        if (!m[o]) {
            for (int k = 0; k < 3; ++k) O->albedo[o * 3 + k] = 0.2f;
            O->orm[o * 3] = 1.0f; O->orm[o * 3 + 1] = 0.8f; O->orm[o * 3 + 2] = 0.0f;
            O->nrm[o * 3] = 0.0f; O->nrm[o * 3 + 1] = 0.0f; O->nrm[o * 3 + 2] = 1.0f;
        }
    free(nm);
}

static void write_maps(const Out *O, const char *dir) {
    int S = O->size;
    unsigned char *px = (unsigned char *)xmalloc((size_t)S * S * 3);
    char path[1200];
    make_dir(dir);
    for (size_t i = 0; i < (size_t)S * S * 3; ++i) px[i] = to_byte(linear_to_srgb(O->albedo[i]));
    snprintf(path, sizeof(path), "%s/albedo.png", dir);
    if (!stbi_write_png(path, S, S, 3, px, S * 3)) die("can't write", path);
    for (size_t i = 0; i < (size_t)S * S; ++i) {
        v3 n = norm(V3(O->nrm[i * 3], O->nrm[i * 3 + 1], O->nrm[i * 3 + 2]));
        px[i * 3] = to_byte(n.x * 0.5f + 0.5f); px[i * 3 + 1] = to_byte(n.y * 0.5f + 0.5f); px[i * 3 + 2] = to_byte(n.z * 0.5f + 0.5f);
    }
    snprintf(path, sizeof(path), "%s/normal.png", dir);
    if (!stbi_write_png(path, S, S, 3, px, S * 3)) die("can't write", path);
    for (size_t i = 0; i < (size_t)S * S * 3; ++i) px[i] = to_byte(O->orm[i]);
    snprintf(path, sizeof(path), "%s/orm.png", dir);
    if (!stbi_write_png(path, S, S, 3, px, S * 3)) die("can't write", path);
    if (O->emis) {
        for (size_t i = 0; i < (size_t)S * S * 3; ++i) px[i] = to_byte(O->emis[i]);
        snprintf(path, sizeof(path), "%s/emissive.png", dir);
        if (!stbi_write_png(path, S, S, 3, px, S * 3)) die("can't write", path);
    }
    printf("  wrote %s/{albedo,normal,orm%s}.png (%dx%d)\n", dir, O->emis ? ",emissive" : "", S, S);
    free(px);
}

/* ------------------------------------------------------------- preview --- */

/* Draws the low mesh with its baked maps, lit by one light, in an orthographic view. */
static void draw_view(unsigned char *rgb, float *zb, int W, int x0, int size, const Low *L, const Out *O,
                      v3 R, v3 U, v3 F, float cy, float half) {
    v3 light = norm(V3(0.4f, 0.8f, 0.6f));
    int S = O->size;
    for (size_t t = 0; t + 2 < L->ni; t += 3) {
        float sx[3], sy[3], sz[3];
        const unsigned *ix = &L->ix[t];
        for (int k = 0; k < 3; ++k) {
            v3 p = L->p[ix[k]];
            sx[k] = x0 + (dot(p, R) + half) / (2 * half) * size;
            sy[k] = (1.0f - (dot(p, U) - cy + half) / (2 * half)) * size;
            sz[k] = dot(p, F);
        }
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-6f) continue;
        int bx0 = (int)fmaxf((float)x0, floorf(fminf(sx[0], fminf(sx[1], sx[2])))), bx1 = (int)fminf((float)(x0 + size - 1), ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2]))));
        int by0 = (int)fmaxf(0.0f, floorf(fminf(sy[0], fminf(sy[1], sy[2])))), by1 = (int)fminf((float)(size - 1), ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2]))));
        v3 N0 = norm(cross(sub(L->p[ix[1]], L->p[ix[0]]), sub(L->p[ix[2]], L->p[ix[0]])));
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                float qx = x + 0.5f, qy = y + 0.5f;
                float w0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
                float w1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                size_t o = (size_t)y * W + x;
                if (z <= zb[o]) continue;
                zb[o] = z;
                float u = w0 * L->uv[ix[0]][0] + w1 * L->uv[ix[1]][0] + w2 * L->uv[ix[2]][0];
                float v = w0 * L->uv[ix[0]][1] + w1 * L->uv[ix[1]][1] + w2 * L->uv[ix[2]][1];
                int tx = (int)(u * S), ty = (int)(v * S);
                tx = tx < 0 ? 0 : tx >= S ? S - 1 : tx; ty = ty < 0 ? 0 : ty >= S ? S - 1 : ty;
                size_t q = (size_t)ty * S + tx;
                v3 N = norm(add(add(mul(L->n[ix[0]], w0), mul(L->n[ix[1]], w1)), mul(L->n[ix[2]], w2)));
                if (dot(N, F) < 0.0f && dot(N0, F) < 0.0f) N = mul(N, -1.0f);
                v3 T, B;
                frame(L->p[ix[0]], L->p[ix[1]], L->p[ix[2]], L->uv[ix[0]], L->uv[ix[1]], L->uv[ix[2]], N, &T, &B);
                v3 n = norm(add(add(mul(T, O->nrm[q * 3]), mul(B, O->nrm[q * 3 + 1])), mul(N, O->nrm[q * 3 + 2])));
                float lit = (0.25f + 0.75f * fmaxf(0.0f, dot(n, light))) * (0.4f + 0.6f * O->orm[q * 3]);
                for (int k = 0; k < 3; ++k) rgb[o * 3 + k] = to_byte(linear_to_srgb(O->albedo[q * 3 + k] * lit * 1.6f));
            }
    }
}

static void preview(const char *path, const Low *L, const Out *O, const float mn[3], const float mx[3]) {
    const int S = 420, W = S * 4;
    unsigned char *rgb = (unsigned char *)xmalloc((size_t)W * S * 3);
    float *zb = (float *)xmalloc(sizeof(float) * W * S);
    for (int i = 0; i < W * S; ++i) { zb[i] = -FLT_MAX; rgb[i * 3] = 40; rgb[i * 3 + 1] = 44; rgb[i * 3 + 2] = 52; }
    float ext = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2])) * 0.6f, cy = 0.5f * (mn[1] + mx[1]), s = 0.70710678f;
    draw_view(rgb, zb, W, 0, S, L, O, V3(1, 0, 0), V3(0, 1, 0), V3(0, 0, 1), cy, ext);        /* front */
    draw_view(rgb, zb, W, S, S, L, O, V3(0, 0, -1), V3(0, 1, 0), V3(1, 0, 0), cy, ext);       /* its right side */
    draw_view(rgb, zb, W, 2 * S, S, L, O, V3(1, 0, 0), V3(0, 0, -1), V3(0, 1, 0), 0.0f, ext); /* above, front at the foot */
    draw_view(rgb, zb, W, 3 * S, S, L, O, V3(s, 0, -s), V3(-0.35f * s, 0.94f, -0.35f * s), V3(s * 0.94f, 0.35f, s * 0.94f), cy * 0.94f, ext);
    int bar = (int)(S / (2 * ext));   /* a metre, along the foot of the front view */
    for (int x = 10; x < 10 + bar && x < S; ++x)
        for (int y = S - 8; y < S - 5; ++y) { size_t o = (size_t)y * W + x; rgb[o * 3] = 255; rgb[o * 3 + 1] = 210; rgb[o * 3 + 2] = 0; }
    stbi_write_png(path, W, S, 3, rgb, W * 3);
    printf("  preview %s\n", path);
    free(rgb); free(zb);
}

/* ---------------------------------------------------------------- main --- */

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: model_pack <in.glb> <name> [--height H | --width W | --depth D] [--yaw DEG] [--stretch Y DY]\n"
                        "                  [--tris N] [--tex PX] [--ao RAYS] [--floor] [--inc FILE] [--maps DIR] [--preview FILE]\n");
        return 1;
    }
    const char *in = argv[1], *name = argv[2];
    int sizeAxis = -1, texSize = 1024, aoRays = 24, floor_ = 0, glowOn = 0;
    float sizeM = 0.0f, yaw = 0.0f, stretchY = 0.0f, stretchD = 0.0f;
    size_t tris = 800;
    char incPath[1024], mapsDir[1024], prevPath[1024] = "";
    snprintf(incPath, sizeof(incPath), "src/city/models/%s.inc", name);
    snprintf(mapsDir, sizeof(mapsDir), "assets/textures/%s", name);
    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--height") && more) { sizeAxis = 1; sizeM = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--width") && more) { sizeAxis = 0; sizeM = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--depth") && more) { sizeAxis = 2; sizeM = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--yaw") && more) yaw = (float)atof(argv[++i]);
        else if (!strcmp(a, "--stretch") && i + 2 < argc) { stretchY = (float)atof(argv[++i]); stretchD = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--tris") && more) tris = (size_t)atol(argv[++i]);
        else if (!strcmp(a, "--tex") && more) texSize = atoi(argv[++i]);
        else if (!strcmp(a, "--ao") && more) aoRays = atoi(argv[++i]);
        else if (!strcmp(a, "--floor")) floor_ = 1;
        else if (!strcmp(a, "--glow")) glowOn = 1;
        else if (!strcmp(a, "--inc") && more) snprintf(incPath, sizeof(incPath), "%s", argv[++i]);
        else if (!strcmp(a, "--maps") && more) snprintf(mapsDir, sizeof(mapsDir), "%s", argv[++i]);
        else if (!strcmp(a, "--preview") && more) snprintf(prevPath, sizeof(prevPath), "%s", argv[++i]);
        else die("unknown option", a);
    }
    for (const char *c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) die("names are lower case letters, digits and underscores:", name);

    printf("%s\n", in);
    High H;
    read_file(in, &H);
    printf("  %zu vertices, %zu triangles, %d material%s\n", H.nv, H.nt, H.nm, H.nm > 1 ? "s" : "");

    /* turn, scale, stand it on y = 0 centred, then stretch */
    float cs = cosf(yaw * 3.14159265f / 180.0f), sn = sinf(yaw * 3.14159265f / 180.0f);
    float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (size_t i = 0; i < H.nv; ++i) {
        v3 p = H.p[i], n = H.n[i];
        H.p[i] = V3(cs * p.x + sn * p.z, p.y, -sn * p.x + cs * p.z);
        H.n[i] = V3(cs * n.x + sn * n.z, n.y, -sn * n.x + cs * n.z);
        const float *q = &H.p[i].x;
        for (int k = 0; k < 3; ++k) { mn[k] = fminf(mn[k], q[k]); mx[k] = fmaxf(mx[k], q[k]); }
    }
    float scale = sizeAxis >= 0 ? sizeM / (mx[sizeAxis] - mn[sizeAxis]) : 1.0f;
    v3 off = V3(-0.5f * (mn[0] + mx[0]), -mn[1], -0.5f * (mn[2] + mx[2]));
    for (int k = 0; k < 3; ++k) { mn[k] = FLT_MAX; mx[k] = -FLT_MAX; }
    for (size_t i = 0; i < H.nv; ++i) {
        v3 p = mul(add(H.p[i], off), scale);
        if (stretchD != 0.0f && p.y >= stretchY) p.y += stretchD;
        H.p[i] = p;
        const float *q = &p.x;
        for (int k = 0; k < 3; ++k) { mn[k] = fminf(mn[k], q[k]); mx[k] = fmaxf(mx[k], q[k]); }
    }
    float extent = fmaxf(mx[0] - mn[0], fmaxf(mx[1] - mn[1], mx[2] - mn[2]));
    printf("  size %.3f x %.3f x %.3f m (scale %.4f)\n", mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], scale);

    v3 *sp;
    size_t snv, sni;
    unsigned *six;
    simplify_shape(&H, tris, &sp, &snv, &six, &sni);
    Low L;
    crease_normals(sp, snv, six, sni, 40.0f, &L);
    free(sp); free(six);
    unwrap(&L, texSize, 4);

    Bvh B;
    bvh_build(&B, &H);
    Out O;
    O.size = texSize;
    O.albedo = (float *)calloc((size_t)texSize * texSize * 3, sizeof(float));
    O.orm = (float *)calloc((size_t)texSize * texSize * 3, sizeof(float));
    O.nrm = (float *)calloc((size_t)texSize * texSize * 3, sizeof(float));
    O.mask = (unsigned char *)calloc((size_t)texSize * texSize, 1);
    O.emis = glowOn ? (float *)calloc((size_t)texSize * texSize * 3, sizeof(float)) : NULL;
    O.glowZ = mx[2] - 0.35f * (mx[2] - mn[2]);
    O.glowX = 0.42f * (mx[0] - mn[0]);
    O.glowY0 = mn[1] + 0.14f * (mx[1] - mn[1]);
    O.glowY1 = mx[1] - 0.14f * (mx[1] - mn[1]);
    if (!O.albedo || !O.orm || !O.nrm || !O.mask) die("out of memory", NULL);
    bake(&L, &H, &B, &O, extent * 0.04f, aoRays, extent * 0.35f, floor_);
    dilate(&O);
    write_maps(&O, mapsDir);
    if (prevPath[0]) preview(prevPath, &L, &O, mn, mx);

    FILE *f = fopen(incPath, "w");
    if (!f) die("can't write", incPath);
    if (L.nv > 65535) die("too many vertices for 16-bit indices", NULL);
    const char *base = strrchr(in, '/') ? strrchr(in, '/') + 1 : strrchr(in, '\\') ? strrchr(in, '\\') + 1 : in;
    fprintf(f, "/* Generated by tools/model_pack.c from %s: don't edit.\n", base);
    fprintf(f, " * %.3f x %.3f x %.3f m, %zu triangles, %zu vertices. */\n", mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], L.ni / 3, L.nv);
    fprintf(f, "static const ModelVertex %s_v[%zu] = {\n", name, L.nv);
    for (size_t i = 0; i < L.nv; ++i)
        fprintf(f, "    {{%.4ff,%.4ff,%.4ff},{%d,%d,%d},{%.5ff,%.5ff}},\n", L.p[i].x, L.p[i].y, L.p[i].z,
                (int)lrintf(L.n[i].x * 127.0f), (int)lrintf(L.n[i].y * 127.0f), (int)lrintf(L.n[i].z * 127.0f), L.uv[i][0], L.uv[i][1]);
    fprintf(f, "};\nstatic const uint16_t %s_i[%zu] = {\n", name, L.ni);
    for (size_t i = 0; i < L.ni; ++i)
        fprintf(f, "%s%u,%s", i % 24 == 0 ? "    " : "", L.ix[i], i % 24 == 23 || i + 1 == L.ni ? "\n" : "");
    fprintf(f, "};\nstatic const float %s_box[6] = { %.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff };\n", name, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    fclose(f);
    printf("  wrote %s\n", incPath);
    (void)lerpf;
    return 0;
}
