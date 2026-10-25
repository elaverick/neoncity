#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Models: authored meshes the city sets down whole (src/city/props.c), each a
 * file of its own, assets/models/<name>.glb, read once at startup.
 *
 * The file is a glTF 2.0 binary holding the mesh alone: one triangle
 * primitive with POSITION, NORMAL and TEXCOORD_0 (floats) and 16- or 32-bit
 * indices, in metres, standing on y = 0, centred on x and z, its front
 * towards +z. Its materials are only names: the authored materials that
 * shade it, assets/materials/<material>.json (PBR_<MATERIAL>), whose maps are
 * baked to its texture coordinates. The primitive's own is the first; more
 * are its liveries, the file's variants (KHR_materials_variants), each piece
 * of it set down wearing one. tools/model_pack.c makes them (README, "Models").
 *
 * The generator reads them, so they are loaded before any chunk is (the game
 * in main, the tests before they generate), and are read-only afterwards. A
 * model whose file is missing or malformed is absent: what would use it isn't
 * placed (props.c), or keeps its procedural stand-in (the lobby sofa), the
 * same in every chunk, its mesh and its collision. */

/* PM_SIGNAL's value is shaders/city/props.glsl's too */
enum { PM_BOLLARD = 0, PM_BIN, PM_MAILBOX, PM_DUMPSTER, PM_AIRCON, PM_SIGNAL, PM_BUTTON, PM_SOFA, PM_COUNT };
#define MODEL_LIVERIES 4   /* materials to a model, at most */

typedef struct ModelVertex { float p[3], n[3], uv[2]; } ModelVertex;
typedef struct Model {
    ModelVertex *v;
    uint32_t    *ix;
    uint32_t     nv, ni;
    float        mn[3], mx[3];   /* bounds */
    uint8_t      pbr[MODEL_LIVERIES];   /* the materials that shade it (PBR_*): its own, then its liveries' */
    uint8_t      liveries;              /* how many (at least 1) */
} Model;

/* Each model's file name (without .glb). */
extern const char *const MODEL_NAMES[PM_COUNT];

/* Loads every model from dir/models (dir NULL: the asset roots, src/core/assets.h),
 * replacing any loaded before. Returns how many loaded; reports the rest on stderr. */
int          models_load(const char *dir);
void         models_free(void);
/* Model pm, or NULL if it isn't loaded. */
const Model *model_get(int pm);
/* Reads one model file into m (allocating its arrays; model_free releases
 * them), finding its materials among the authored ones. On error returns false with the reason in err. */
bool         model_read(const char *path, Model *m, char *err, size_t errSize);
void         model_free(Model *m);
