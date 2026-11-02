#pragma once
#include "kit.h"

/* The office floors' high-poly furniture (docs/interior-impostors.md, section 19): bake-only models,
 * assets_src/interiors/<name>.glb (tools/interior_models.py makes them), and the printed matter and
 * textures that go with them. Each is a mesh with its normals and UVs, its materials carrying their
 * textures; the game never loads any of this. */

#define PIECE_VARIANTS 3
#define FURNITURE_BOOKS 7   /* textures/book_<n>.jpg (interior_models.py books) */
typedef struct Piece {
    int   mesh;
    int   mats[PIECE_VARIANTS], nmats;   /* its texture variants */
    int   extra;                         /* the panel that lights up (a material named screen or diffuser): a mesh, or -1 */
    float extraArea;                     /* m2 */
    float mn[3], mx[3];
} Piece;

typedef struct Furniture {
    Piece chair, desk, computer, lamp, partition, cabinet, blackboard, intray, phone, pen, armchair, printer, sideTable, table;
    int   card;            /* a unit cube whose every face carries a whole image */
    int   bookFlat, bookUp;   /* unit cuboids in a book's layout: lying with its cover up and spine to -x; standing, spine to +z */
    int   paper[4], chart[6], screen[4], book[FURNITURE_BOOKS], rug;   /* materials */
} Furniture;

/* Loads them into s, and dresses the kit's ducts and ceiling lights in their textures. assets: the
 * assets folder, which assets_src sits beside. False, with the reason on stderr, if a file is missing:
 * a bake doesn't quietly fall back on stand-ins. */
bool furniture_load(Scene *s, Kit *k, Furniture *f, const char *assets);

/* A model standing at (x, y, z), turned by yaw, stretched by (sx, sy, sz). */
void piece_place(Scene *s, const Piece *p, int variant, uint32_t flags, float x, float y, float z, float yaw, float sx, float sy, float sz);
