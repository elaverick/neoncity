#pragma once
#include "pmodel.h"

/* The houses' character kit (residential roadmap HM4, ~/.claude/plans/residential-buildings.md; the target in
 * docs/residential-target.md, "The character kit"): the modules that dress a house shell (src/gen/home_shell.c)
 * with what gives a real house its depth, and the assemblies that set them down (HM5 places them on houses).
 *
 * The shell already carries the massing: bodies, roof slabs with their soffits, fascias and verge boards,
 * parapets, stacks, porch floors and posts, steps, and openings with their glass set back in reveals. The kit
 * adds, as instances of shared modules (NB2): eaves (rafter tails, knee braces, brackets), bargeboards and
 * finials, gutters and downpipes, ridge tiles; string courses, plinths, quoins, soldier courses, half-timbering,
 * hung tiles; sills, lintels, keystones, arches and hood moulds; frames with their glazing bars; shutters,
 * grilles, window boxes; doors, fanlights and doorcases; porch posts, spandrels, balustrades and door
 * canopies; bays' posts, cornices and aprons; dormers; chimney caps and pots, vent pipes and rooflights.
 *
 * Frames (each module's own):
 *   on a wall     the wall's face is z 0, out along +z, x along the wall, y up. Nothing a wall piece carries
 *                 goes behind z 0 but what sits in an opening's reveal (sills, frames, doors), which stops at
 *                 the glass's or the door's plane or is hidden behind it.
 *   runs          (gutters, courses, ridge tiles, cornices, bargeboards, balustrades) run x 0..1 m and are
 *                 stretched along x by their instance; their sections lie in y and z, so stretching keeps them
 *                 true (normals square to x are untouched by it). A run of repeated parts (ridge tiles,
 *                 balusters, scallops) is placed as whole metres, each stretched a little (hk_run).
 *   in an opening frames, doors, fanlights: centred on x 0, the opening's foot at y 0, the glass's (or the
 *                 door's) plane at z 0, standing out towards +z; built at a usual size (below) and stretched to
 *                 the opening's by their instance (within about a quarter, so bars stay bars).
 *   standing      posts, pots, finials stand on y 0 centred.
 *   on a roof     dormers and rooflights: their front foot on the roof plane at the origin, the slope rising
 *                 towards -z (pmodel_house.c builds them for the shallowest pitch a style takes and buries the rest
 *                 under steeper ones).
 *
 * Materials are the houses' own (pmodel.h PMAT_HTRIM on): colours come from the instance (house_surf.h
 * HOUSE_KIT_*), so one module serves every house. Instances must not be mirrored (a negative scale turns a
 * module inside out under the city's culling): handed parts come as the assemblies place them. */

/* usual sizes the in-opening modules are built at (m) */
#define HK_SASH_W     0.95f
#define HK_SASH_H     1.60f
#define HK_CASE_W     1.50f
#define HK_CASE_H     1.20f
#define HK_GRID_W     1.00f
#define HK_GRID_H     1.40f
#define HK_PICT_W     1.50f
#define HK_PICT_H     1.10f
#define HK_UPVC_W     1.20f
#define HK_UPVC_H     1.20f
#define HK_DOOR_W     0.90f
#define HK_DOOR_H     2.05f
#define HK_FAN_W      1.00f   /* a fanlight over a door (its frame), 0.5 m high */
#define HK_FAN_H      0.50f
#define HK_DORMER_W   1.60f   /* a dormer's front, cheek to cheek */
#define HK_DORMER_P   30.0f   /* the shallowest roof (degrees) a dormer meets; steeper roofs bury its back */
#define HK_STACK_W    1.00f   /* the stack a corbelled cap is built for (x by z) */
#define HK_STACK_D    0.50f
#define HK_SWAN_DZ    0.30f   /* a swan neck's reach from the gutter's outlet back to the pipe's line (_L: 0.60) */
#define HK_RAFTER_OUT 0.55f   /* a rafter tail's reach past the wall */
#define HK_POST_H     2.40f   /* porch posts (stretched to the porch's eaves) */
#define HK_PIPE_Z     0.055f  /* a downpipe's axis off the wall */
#define HK_GUTTER_Z   0.065f  /* a gutter's middle off the fascia */

/* the builders (pmodel_kit.c's table holds them) */
void m_h_rafter_tail(Pm *pm);   void m_h_knee_brace(Pm *pm);   void m_h_eaves_bracket(Pm *pm);
void m_h_barge_scallop(Pm *pm); void m_h_barge_gothic(Pm *pm);  void m_h_finial(Pm *pm);
void m_h_gutter_half(Pm *pm);   void m_h_gutter_ogee(Pm *pm);   void m_h_gutter_stop(Pm *pm);
void m_h_swan_neck(Pm *pm);     void m_h_swan_neck_l(Pm *pm);   void m_h_downpipe(Pm *pm);
void m_h_pipe_clip(Pm *pm);     void m_h_pipe_shoe(Pm *pm);     void m_h_hopper(Pm *pm);
void m_h_ridge_roll(Pm *pm);    void m_h_ridge_crest(Pm *pm);
void m_h_string_course(Pm *pm); void m_h_plinth(Pm *pm);        void m_h_quoin(Pm *pm);
void m_h_soldier(Pm *pm);       void m_h_timber(Pm *pm);        void m_h_hung_tiles(Pm *pm);
void m_h_sill_stone(Pm *pm);    void m_h_sill_timber(Pm *pm);   void m_h_sill_concrete(Pm *pm);
void m_h_lintel_stone(Pm *pm);  void m_h_lintel_concrete(Pm *pm); void m_h_keystone(Pm *pm);
void m_h_arch_brick(Pm *pm);    void m_h_hood_mould(Pm *pm);    void m_h_casing(Pm *pm);
void m_h_win_sash(Pm *pm);      void m_h_win_georgian(Pm *pm);  void m_h_win_casement(Pm *pm);
void m_h_win_grid(Pm *pm);      void m_h_win_picture(Pm *pm);   void m_h_win_upvc(Pm *pm);
void m_h_shutter_louvre(Pm *pm); void m_h_shutter_panel(Pm *pm); void m_h_grille(Pm *pm); void m_h_window_box(Pm *pm);
void m_h_door_4p(Pm *pm);       void m_h_door_6p(Pm *pm);       void m_h_door_craft(Pm *pm);
void m_h_door_half(Pm *pm);     void m_h_door_upvc(Pm *pm);
void m_h_fanlight(Pm *pm);      void m_h_doorcase(Pm *pm);      void m_h_doorcase_ped(Pm *pm);
void m_h_post_turned(Pm *pm);   void m_h_post_craft(Pm *pm);    void m_h_post_square(Pm *pm);
void m_h_spandrel(Pm *pm);      void m_h_balustrade(Pm *pm);
void m_h_canopy_flat(Pm *pm);   void m_h_canopy_gable(Pm *pm);
void m_h_bay_post(Pm *pm);      void m_h_bay_cornice(Pm *pm);   void m_h_bay_apron(Pm *pm);
void m_h_dormer_gable(Pm *pm);  void m_h_dormer_hip(Pm *pm);    void m_h_dormer_flat(Pm *pm);
void m_h_pot_plain(Pm *pm);     void m_h_pot_cannon(Pm *pm);    void m_h_pot_crown(Pm *pm);    void m_h_pot_cowl(Pm *pm);
void m_h_stack_cap(Pm *pm);     void m_h_vent_pipe(Pm *pm);     void m_h_rooflight(Pm *pm);

/* ------------------------------------------------------------- assemblies --- */
/* Each places modules (pm_place: instances, or built in) with the builder's current transform as its frame. */

/* A run module (x 0..1) laid from x 0 to len as whole pieces of about `piece` metres (each stretched a little:
 * the repeats keep their size), or as one stretched piece where piece is 0 (a plain section). */
void hk_run(Pm *pm, int module, float len, float piece);

enum { HK_FRAME_NONE = 0, HK_FRAME_SASH, HK_FRAME_GEORGIAN, HK_FRAME_CASEMENT, HK_FRAME_GRID, HK_FRAME_PICTURE, HK_FRAME_UPVC };
enum { HK_SILL_NONE = 0, HK_SILL_STONE, HK_SILL_TIMBER, HK_SILL_CONCRETE };
/* heads: a stone lintel (and a keystone), a brick arch, a soldier course, a Victorian hood mould, a concrete
 * lintel, a craftsman's casing (boards round the opening on the wall, a capped head) */
enum { HK_HEAD_NONE = 0, HK_HEAD_LINTEL, HK_HEAD_KEYSTONE, HK_HEAD_ARCH, HK_HEAD_SOLDIER, HK_HEAD_HOOD, HK_HEAD_CONCRETE, HK_HEAD_CASING };
enum { HK_SHUT_NONE = 0, HK_SHUT_LOUVRE, HK_SHUT_PANEL };
typedef struct HkWindow {
    uint8_t frame, sill, head, shutters, grille, box;
    float   w, h;        /* the opening */
    float   reveal;      /* the glass's depth behind the wall's face */
} HkWindow;
/* A window: its origin the opening's foot at the middle, on the wall's face. */
void hk_window(Pm *pm, const HkWindow *w);

enum { HK_DOOR_4P = 0, HK_DOOR_6P, HK_DOOR_CRAFT, HK_DOOR_HALF, HK_DOOR_UPVC };
enum { HK_CASE_NONE = 0, HK_CASE_FLAT, HK_CASE_PEDIMENT, HK_CASE_FANLIGHT };
/* A front door in its opening (w x h, the leaf's plane `recess` behind the wall's face), and its surround on the
 * wall: a doorcase (pilasters, an entablature or a pediment, a fanlight over the door) or a fanlight alone. */
void hk_door(Pm *pm, int kind, int surround, float w, float h, float recess);

enum { HK_GUTTER_HALF = 0, HK_GUTTER_OGEE };
/* A gutter along an eaves from x 0 to len: its back on the fascia's face (z 0), its top at y 0; stop ends at
 * both (bit 0 the start, bit 1 the end). */
void hk_gutter(Pm *pm, int kind, float len, int stops);
/* A downpipe on a wall (its foot on the ground at the origin, on the wall's face) from a gutter's outlet at
 * height top, reach metres out from the wall (the fascia's face plus the gutter's middle): a swan neck back to
 * the wall, or a hopper head; clips every ~1.8 m, a shoe at the foot. */
void hk_downpipe(Pm *pm, float top, float reach, int hopper);

enum { HK_BARGE_SCALLOP = 0, HK_BARGE_GOTHIC };
/* Bargeboards on a gable's verge, the origin at its apex on the verge's face (z 0, out +z): each side runs down
 * at `pitch` degrees to the eaves half metres out; a finial at the apex if asked. */
void hk_bargeboards(Pm *pm, int kind, float half, float pitch, int finial);
/* Ridge tiles along a ridge (or a hip: tilt the frame) from x 0 to len, on its line at y 0. */
void hk_ridge(Pm *pm, int crested, float len);
/* Exposed rafter tails along an eaves from x 0 to len at spacing, the wall's face z 0, the wall plate at y 0;
 * they fall at pitch degrees and reach `out` past the wall. */
void hk_rafter_tails(Pm *pm, float len, float spacing, float pitch, float out);
/* Quoins up a corner from y 0 to h: the corner at the origin, one face the plane z 0 (out +z, running to -x),
 * the other the plane x 0 (out +x, running to -z). */
void hk_quoins(Pm *pm, float h);
/* Mock-Tudor half-timbering on a gable: the origin at the middle of its foot (the eaves' level) on the wall's
 * face; half its width, its pitch; studs about every `spacing`, braces in the outer bays, a rail along the foot. */
void hk_tudor_gable(Pm *pm, float half, float pitch, float spacing);
/* Hung tiles over a w x h panel from the origin (its lower left corner on the wall's face). */
void hk_hung_tiles(Pm *pm, float w, float h);

enum { HK_BAY_SQUARE = 0, HK_BAY_CANTED };
typedef struct HkBay {
    uint8_t kind, storeys, frame, sill;
    float   front;       /* the front light (a canted bay's front face; a square bay's whole front) */
    float   proj;        /* out from the wall */
    float   storeyH;     /* floor to floor */
    float   sillH, headH;/* each storey's opening, over its floor */
} HkBay;
/* A bay's dressing over the shell's bay: posts at its corners, a frame, sill and apron in each face's light, a
 * cornice round each storey's head. The origin at the bay's middle on the main wall's face, at its floor. */
void hk_bay(Pm *pm, const HkBay *b);

enum { HK_DORMER_GABLE = 0, HK_DORMER_HIP, HK_DORMER_FLAT };
/* A dormer on a roof: its front foot at the origin on the roof plane, its front facing +z (down the slope),
 * w wide (stretched from HK_DORMER_W). */
void hk_dormer(Pm *pm, int kind, float w);

enum { HK_POT_PLAIN = 0, HK_POT_CANNON, HK_POT_CROWN, HK_POT_COWL };
/* A stack's top: its corbelled cap over the stack's last 0.25 m and its flaunching (the origin at the stack's top
 * in its middle, w along x by d), and pots (a flue each) in a line along x, their kinds from seed. */
void hk_chimney_top(Pm *pm, float w, float d, int pots, uint32_t seed);

/* A porch balustrade from x 0 to len (balusters every ~0.11 m, rails), centred on z 0. */
void hk_balustrade(Pm *pm, float len);
