#pragma once
#include "pmodel.h"

/* The houses' shared retrofit kit (residential roadmap HM5c1, ~/.claude/plans/residential-buildings.md; the user's
 * concept sheets in docs/reference/international-residential, where every sheet carries it on panel 1, the
 * original house, as well as on the retrofits of panels 2-6): what a megacity's owners bolt onto a house of any
 * age. Solar canopies on steel frames with downlights under them and panels laid flat on slopes; water tanks,
 * black plastic and steel, on the roof and on tall steel stands, with their pipework; copper and steel risers
 * down the walls; breeze-block screens; pots, troughs, climbing vines and trailing plants; LED strips and
 * downlights under slabs and canopies; wall lanterns and bulkheads; meter boxes; aerials; roof railings; steel
 * security gates and razor wire.
 *
 * Frames follow pmodel_house.h's (a wall piece's face at z 0, out along +z, x along the wall, y up; runs x 0..1
 * stretched by their instance; standing pieces on y 0 centred). Materials are the houses' own (PMAT_HTRIM on,
 * shaded by house.glsl shadeHouseKit), the retrofit's from PMAT_HGALV: galvanised and painted steel, copper,
 * black tank plastic, solar cells, breeze concrete, flowers, and two that glow at night by the city's clock
 * (an LED's diffuser, a lantern's glass). A gate's paint is the unit's door colour (PMAT_HDOOR). */

/* usual sizes (m) */
#define HR_PANEL_W     1.00f   /* a solar panel across (x) */
#define HR_PANEL_L     1.65f   /* ...and along (z) */
#define HR_PANEL_H     0.10f   /* its mounting feet to its glass */
#define HR_ROW_N       3       /* panels in a solar row module (side by side along x) */
#define HR_TANK_D      1.08f   /* a 1000 litre tank's diameter, and height */
#define HR_TANK_H      1.25f
#define HR_STEEL_D     0.90f   /* a steel tank */
#define HR_STEEL_H     1.55f   /* with its legs */
#define HR_STAND_W     1.20f   /* a tank stand's legs, square */
#define HR_STAND_BAY   1.50f   /* one braced bay of a stand */
#define HR_STAND_TOP   0.10f   /* its platform's depth */
#define HR_PIPE_CU     0.028f  /* a copper riser's radius */
#define HR_PIPE_FE     0.040f  /* a steel one's */
#define HR_BEND        0.10f   /* an elbow's bend radius (the pipes' axes) */
#define HR_PIPE_Z      0.07f   /* a riser's axis off the wall */
#define HR_BREEZE      1.20f   /* a breeze-block panel (three blocks of 0.4 m square) */
#define HR_BREEZE_T    0.10f
#define HR_RAIL_H      1.00f   /* a roof railing */
#define HR_GATE_W      1.00f   /* a security gate's leaf */
#define HR_GATE_H      2.10f
#define HR_SHEET_H     1.80f   /* a sheet gate (a yard's: HM7 sets it in the boundary) */
#define HR_RAZOR_D     0.50f   /* a razor wire coil */
#define HR_METER_W     0.36f
#define HR_METER_H     0.50f
#define HR_LANTERN_Z   0.22f   /* a lantern's middle off the wall */
#define HR_AERIAL_H    2.40f   /* a mast */
#define HR_POST        0.08f   /* a canopy's square post */
#define HR_BEAM_H      0.14f   /* a canopy's beams */
#define HR_VINE_DEPTH  0.10f   /* HMv: a vine's card off its wall: the depth of its leaves (vine.json's params[0] must match) */
/* HMv: the vine atlas's plants, in its order (tools/vine_pack.py; house.glsl shadeVine) */
enum { HR_VINE_CLEMATIS, HR_VINE_IVY, HR_VINE_WISTERIA, HR_VINE_PASSION, HR_VINE_BOUGAINVILLEA, HR_VINE_THUNBERGIA, HR_VINE_SPECIES };

/* the builders (pmodel_kit.c's table holds them) */
void m_r_solar_panel(Pm *pm);  void m_r_solar_row(Pm *pm);
void m_r_frame_post(Pm *pm);   void m_r_frame_beam(Pm *pm);
void m_r_downlight(Pm *pm);    void m_r_led_strip(Pm *pm);
void m_r_tank_poly(Pm *pm);    void m_r_tank_steel(Pm *pm);  void m_r_stand_bay(Pm *pm);  void m_r_stand_top(Pm *pm);
void m_r_pipe_cu(Pm *pm);      void m_r_pipe_fe(Pm *pm);     void m_r_elbow_cu(Pm *pm);   void m_r_elbow_fe(Pm *pm);
void m_r_valve(Pm *pm);        void m_r_pipe_clip(Pm *pm);
void m_r_breeze_diamond(Pm *pm); void m_r_breeze_ring(Pm *pm); void m_r_breeze_star(Pm *pm);
void m_r_rail(Pm *pm);
void m_r_meter(Pm *pm);        void m_r_lantern(Pm *pm);     void m_r_bulkhead(Pm *pm);
void m_r_gate_bars(Pm *pm);    void m_r_gate_sheet(Pm *pm);  void m_r_razor(Pm *pm);
void m_r_pot(Pm *pm);          void m_r_pot_palm(Pm *pm);    void m_r_trough(Pm *pm);
void m_r_vine(Pm *pm);         void m_r_trail(Pm *pm);
void m_r_aerial_yagi(Pm *pm);  void m_r_aerial_cross(Pm *pm);
/* HM5e3: boundaries (x 0..1 and y 0..1 stretched, centred on z 0: a run's piece or a gate's leaf) */
void m_r_fence_steel(Pm *pm);  void m_r_fence_mesh(Pm *pm);  void m_r_gate_pierced(Pm *pm);

/* Module frames:
 *   solar panel   HR_PANEL_W (x) by HR_PANEL_L (z), centred; its feet on y 0, its glass HR_PANEL_H up, facing +y
 *   solar row     HR_ROW_N panels side by side along x (x -1.5..1.5), as one
 *   frame post    a square steel post, y 0..1 (stretched), its base plate on y 0, centred
 *   frame beam    a steel beam x 0..1 (stretched), its top on y 0, centred on z 0, HR_BEAM_H deep
 *   downlight     a puck, its back on y 0 (a ceiling), its lens facing -y
 *   led strip     an aluminium channel x 0..1 (stretched), its back on y 0, its diffuser facing -y
 *   tank poly     a black ribbed 1000 litre tank, standing on y 0 centred (scaled for 500 and 2000 litres)
 *   tank steel    an upright steel tank on four legs, standing on y 0 centred
 *   stand bay     one braced bay of a tank stand, y 0..HR_STAND_BAY, legs on the corners of HR_STAND_W square
 *   stand top     the stand's platform, its underside on y 0, its deck HR_STAND_TOP up (what stands on it)
 *   pipe          y 0..1 (stretched), its axis the origin's
 *   elbow         a quarter bend: in along +y to the origin, out along +z from (0, HR_BEND, HR_BEND)
 *   valve         on a pipe along y, the body round the origin, its handwheel out along +z
 *   pipe clip     the riser's axis at the origin, a saddle back to the wall (z -HR_PIPE_Z); scaled for the pipe
 *   breeze        x 0..HR_BREEZE, y 0..HR_BREEZE, centred on z 0 (HR_BREEZE_T thick)
 *   rail          a roof railing x 0..1 (stretched), on y 0, centred on z 0: a post at x 0, rails, balusters
 *   meter         on a wall, its foot's middle at the origin; a conduit stub up from its top
 *   lantern       on a wall, the bracket's middle at the origin; the lantern HR_LANTERN_Z out
 *   bulkhead      on a wall, its middle at the origin
 *   gate bars     a leaf HR_GATE_W x HR_GATE_H, its foot's middle at the origin, z 0..0.04 (stretched to its door)
 *   gate sheet    HR_GATE_W x HR_SHEET_H, likewise (for HM7's boundaries)
 *   razor         a coil x 0..1 (stretched), sitting on y 0 centred on z 0
 *   pot, palm     standing on y 0 centred
 *   trough        x 0..1 (stretched), standing on y 0, z -0.17..0.17
 *   vine          a climbing plant's card (HMv), x -0.5..0.5, y 0..1 (both stretched), at z HR_VINE_DEPTH, facing +z
 *   trail         plants hanging from an edge: x 0..1, from y 0 down to -1 (stretched), out z 0..0.25
 *   aerial        a mast standing on y 0 (its bracket's foot), HR_AERIAL_H tall */

/* ------------------------------------------------------------- assemblies --- */
/* Each places modules (pm_place: instances, or built in) with the builder's current transform as its frame. */

/* A solar canopy: a deck of panel rows w (x) by d (z) on a steel frame, its posts standing on y 0 at the corners
 * (and every ~3.5 m along), h tall at the front (z -d/2) and rising `rise` to the back; beams round it and across,
 * downlights under the beams if lights. feet (NULL: all 0) gives each post's foot (post i along x, front then back:
 * feet[2 i], feet[2 i + 1]) where it stands on a slope: 1 + ceil(w / 3.6) posts a side over 3.6 m, else 2. */
void hr_solar_canopy(Pm *pm, float w, float d, float h, float rise, int lights, const float *feet);
/* Solar panels laid on a plane (a roof's slope: the frame's y its normal, z up the slope): cols across (x) by rows
 * up it, from the origin (the array's lower middle). */
void hr_solar_flush(Pm *pm, int cols, int rows);
/* A tank on a stand `stand` metres tall (0: on the roof on its own), the stand's foot at the origin. kind 0 a black
 * plastic tank scaled by size (1: 1000 litres), 1 a steel one. Returns its top. */
float hr_tank(Pm *pm, int kind, float size, float stand);

/* A pipe along an axis-aligned polyline (each corner a right angle; elbows at them, straight lengths between);
 * steel if fe, else copper. */
void hr_pipe_path(Pm *pm, int fe, const float (*p)[3], int n);
/* Clips along a pipe's straight run from a to b (on a wall facing +z: the clips' backs to it), every ~1.2 m. */
void hr_pipe_clips(Pm *pm, int fe, const float a[3], const float b[3]);

/* Breeze-block panels over w x h from the origin (its lower left, centred on the frame's z 0), whole panels
 * stretched to fit; kind 0 diamonds, 1 rings, 2 the star lattice (HM5c2b: a mashrabiya's). */
void hr_breeze(Pm *pm, int kind, float w, float h);
/* A roof railing along x from 0 to len, its end post at len. */
void hr_rail(Pm *pm, float len);
/* Razor wire along x from 0 to len, in coils of about a metre. */
void hr_razor(Pm *pm, float len);
/* A climbing plant up a wall (HMv): one card w wide and h tall from the origin (its foot's middle), of `species`
 * (HR_VINE_*); the shader grows it as overlapping columns of the atlas, the outer ones stopping short. */
void hr_vine(Pm *pm, float w, float h, int species);
/* Plants trailing over an edge along x from 0 to len, hanging `drop` metres (varied piece to piece). */
void hr_trail(Pm *pm, float len, float drop, uint32_t seed);
/* An LED strip along x from 0 to len (one stretched piece). */
void hr_led(Pm *pm, float len);
