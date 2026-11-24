#pragma once
#include "pmodel.h"

/* The other regions' kit (residential roadmap HM5b1, ~/.claude/plans/residential-buildings.md, section "HM5b1: the other
 * regions' kit, style by style"; the user's concept sheets in docs/reference/international-residential): what the
 * sheets of the regions beyond Britain and North America show on their houses that HM4's kit and HM5c1's retrofit kit
 * lack. HM5b1a, East and South-East Asia:
 *
 *   Japan        an aluminium sliding window, clear or frosted; a storm-shutter (amado) box beside it and the rail its
 *                shutters run in; a thin hood over a door (hisashi); a horizontal slat screen (panel 5)
 *   China        the gatehouse roof's upturned ridge ends and its row of eave-end tiles; ring knockers on the red
 *                doors, drum stones beside them; red couplets and the fu diamond; a paper lantern; timber lattice
 *                windows; steel cages projecting from the blocks' windows (panel 2)
 *   Korea        hazard sleeves on the pilotis' columns; a bank of mailboxes by the stair (panels 2, 5); the hanok's
 *                ridge ends and fine-grid lattice (panel 4)
 *   Shophouse    pilasters and the five-foot way's round columns; full-height louvred French windows; carved vent
 *                panels over them; the parapet's scrolled crest; swing half doors (pintu pagar); canvas awnings
 *   Link house   the curved car-porch canopy; a gable's vent roundel; small hoods over the windows
 *
 * Frames follow pmodel_house.h's: a wall piece's face at z 0, out along +z, x along the wall, y up; runs x 0..1
 * stretched by their instance (sections in y and z); in-opening pieces centred on x 0, the opening's foot at y 0, the
 * glass's plane at z 0, built at a usual size and stretched to the opening's (within about a quarter); standing pieces
 * on y 0 centred. Instances keep to moderate stretches: the city turns normals by the instance's own matrix.
 * Materials are the houses' (PMAT_HTRIM on), with HM5b1a's own: aluminium, red paper, the lantern's glow, hazard
 * stripes, canvas, frosted glass (house.glsl shadeHouseKit). Placing them by style is HM5b2's. */

/* usual sizes (m) */
#define HK_SLIDE_W     1.60f   /* a Japanese sliding window: two sashes */
#define HK_SLIDE_H     1.10f
#define HK_AMADO_W     0.34f   /* a storm-shutter box beside its window (x 0..W out from the opening's edge) */
#define HK_AMADO_H     1.30f   /* ...standing from 0.05 under the opening's foot */
#define HK_HISASHI_D   0.50f   /* a door hood's reach */
#define HK_RIDGE_END_H 0.70f   /* a Chinese ridge end's height over the ridge line */
#define HK_EAVE_PITCH  0.25f   /* eave-end tiles' spacing (four a metre) */
#define HK_COUPLET_W   0.24f   /* a couplet's strip */
#define HK_COUPLET_H   1.50f
#define HK_FU          0.42f   /* the fu diamond's side */
#define HK_LANTERN_H   0.62f   /* a paper lantern from its hook (y 0) down to its tassel's end */
#define HK_LAT_W       1.00f   /* lattice windows */
#define HK_LAT_H       1.20f
#define HK_CAGE_D      0.36f   /* a window cage's reach from the wall */
#define HK_HAZARD_H    1.20f   /* a hazard sleeve's height */
#define HK_MAIL_W      0.96f   /* a bank of six mailboxes, two rows of three */
#define HK_MAIL_H      0.50f
#define HK_COL_H       3.00f   /* pilasters and round columns, base to abacus (stretched to the storey) */
#define HK_COL_R       0.17f   /* a round column's shaft at its foot */
#define HK_PIL_W       0.36f   /* a pilaster's shaft across */
#define HK_LOUVRE_W    1.20f   /* a louvred French window (the shophouse's) */
#define HK_LOUVRE_H    2.20f
#define HK_VENT_W      1.00f   /* a vent panel over a window */
#define HK_VENT_H      0.36f
#define HK_CREST_W     2.40f   /* a parapet's crest */
#define HK_CREST_H     0.95f
#define HK_PAGAR_W     1.40f   /* swing half doors, both leaves */
#define HK_PAGAR_H     1.35f   /* a leaf's height */
#define HK_PAGAR_Y     0.30f   /* their foot over the step */
#define HK_AWNING_D    1.10f   /* an awning's reach */
#define HK_AWNING_DROP 0.55f   /* ...and its fall over it */
#define HK_CARPORT_D   5.00f   /* a car porch's canopy, wall to front (stretched within a quarter) */
#define HK_CARPORT_RISE 0.55f  /* its crown over its springing at the wall */
#define HK_VENT_R      0.30f   /* a gable vent's roundel */
#define HK_HOOD_D      0.45f   /* a window hood's reach */

/* the builders (pmodel_kit.c's table holds them) */
void m_h_win_sliding(Pm *pm);  void m_h_win_sliding_frost(Pm *pm);
void m_h_amado(Pm *pm);        void m_h_amado_rail(Pm *pm);    void m_h_hisashi(Pm *pm);    void m_h_slats(Pm *pm);
void m_h_ridge_end(Pm *pm);    void m_h_ridge_end_kr(Pm *pm);  void m_h_eave_tiles(Pm *pm);
void m_h_knocker(Pm *pm);      void m_h_drum_stone(Pm *pm);    void m_h_couplet(Pm *pm);    void m_h_fu(Pm *pm);
void m_h_lantern_paper(Pm *pm);
void m_h_lattice(Pm *pm);      void m_h_lattice_kr(Pm *pm);    void m_h_cage(Pm *pm);
void m_h_hazard(Pm *pm);       void m_h_mailbox(Pm *pm);
void m_h_pilaster(Pm *pm);     void m_h_column(Pm *pm);        void m_h_win_louvre(Pm *pm); void m_h_vent_panel(Pm *pm);
void m_h_crest(Pm *pm);        void m_h_pintu_pagar(Pm *pm);   void m_h_awning(Pm *pm);
void m_h_carport(Pm *pm);      void m_h_gable_vent(Pm *pm);    void m_h_win_hood(Pm *pm);

/* Module frames:
 *   win_sliding(_frost)  in an opening HK_SLIDE_W x HK_SLIDE_H: an aluminium frame, two sashes overlapping at the middle,
 *                        the right one in front; _frost carries milky panes in both (lit by night)
 *   amado                on a wall: the pocket box x 0..HK_AMADO_W beside an opening's edge (x 0), y -0.05..HK_AMADO_H-0.05,
 *                        0.13 m deep; its face ribbed
 *   amado_rail           a run x 0..1: the channel the shutters slide in, over an opening's head (its foot on y 0)
 *   hisashi              a run x 0..1: a thin flat hood out HK_HISASHI_D from the wall, its top at y 0 falling 3 degrees
 *   slats                a run x 0..1 and y 0..1 (both stretched): horizontal timber slats on two battens off the wall
 *   ridge_end(_kr)       standing on a ridge's end: the ridge runs along +x from x 0 at y 0 (its tiles' top); the
 *                        ornament rises to HK_RIDGE_END_H and curls back over the ridge (Korea's a lower upswept block)
 *   eave_tiles           a run x 0..1: four round end caps and drip tiles under them along an eave's edge, the origin at
 *                        the covering's lower edge (z 0 its face), the caps facing +z
 *   knocker              on a door's face: a round plate and a hanging ring, centred on the origin
 *   drum_stone           standing: a stone block, a drum on edge over it (its axis along x), centred
 *   couplet / fu         flat on a wall: a strip HK_COUPLET_W x HK_COUPLET_H from y 0 centred on x; the diamond centred
 *   lantern_paper        hung from its hook at the origin: a red paper body, black caps, a tassel down to -HK_LANTERN_H
 *   lattice(_kr)         in an opening HK_LAT_W x HK_LAT_H: a timber frame, the fret (China's) or the fine grid
 *                        (Korea's), paper behind (frosted glass's material, lit by night)
 *   cage                 over an opening: x -0.5..0.5, y 0..1 (stretched to the opening and a margin), out to HK_CAGE_D
 *   hazard               a sleeve round a square column: x and z -0.5..0.5 (stretched to the column), y 0..HK_HAZARD_H
 *   mailbox              on a wall: the bank's foot at y 0, centred, HK_MAIL_W x HK_MAIL_H
 *   pilaster             on a wall: a base, a shaft HK_PIL_W across, a capital, y 0..HK_COL_H
 *   column               standing: plinth, base, a tapering round shaft, a capital, y 0..HK_COL_H
 *   win_louvre           in an opening HK_LOUVRE_W x HK_LOUVRE_H: two leaves of louvres, a top light
 *   vent_panel           on a wall: a pierced panel HK_VENT_W x HK_VENT_H from y 0 centred (over a window's head)
 *   crest                standing on a parapet's top (y 0), centred, its face on z 0: HK_CREST_W x HK_CREST_H
 *   pintu_pagar          in a door's opening, both leaves HK_PAGAR_W across, their foot HK_PAGAR_Y over y 0, on the
 *                        wall's face (z 0..0.05)
 *   awning               a run x 0..1 (whole metres): canvas from the wall at y 0 out HK_AWNING_D and down HK_AWNING_DROP,
 *                        a valance, one folding arm a metre
 *   carport              a run x 0..1 (bays of about 1.5 m): a curved steel rib at x 0 from the wall (z 0, y 0) out to
 *                        HK_CARPORT_D, frosted sheet over it, the front's edge beam
 *   gable_vent           on a wall: a louvred roundel of radius HK_VENT_R centred on the origin
 *   win_hood             a run x 0..1: an aluminium hood over a window, its back at y 0 on the wall, out HK_HOOD_D */
