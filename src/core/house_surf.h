#ifndef HOUSE_SURF_H
#define HOUSE_SURF_H

/* The houses' shells (residential roadmap HM3a; src/gen/home_shell.c builds them, src/city/homes.c sets them
 * down, shaders/city/house.glsl shades them). The one definition of what C and GLSL share about a MAT_HOUSE
 * vertex: #defines only, in syntax both languages accept (as doors.h).
 *
 *   params y  the surface (HF_*)
 *   params z  its finish: a wall's, a reveal's or a stack's HW_*, a roof's or a deck's HRM_*, steps' HST_*
 *   params w  its colour: an entry of its finish's palette (house.glsl), or the trim's, door's or garage's
 *   aux       HOUSE_AUX_FAR on the base layer's far shell (dropped while its chunk's near layer is drawn:
 *             the chunk record's bodies bit CHUNK_HOMES_NEAR_BIT, city.vert); a far wall's painted windows
 *             (HOUSE_AUX_WINDOWS and its rhythm: HOUSE_AUX_STOREYH and the rest below); an opening's size and
 *             pattern (glass, a door, a garage door)
 *   seed      the unit's (its plot's)
 *   uv        a wall's: metres along it and up (the row's frame, so courses run on across its pieces; a far
 *             wall's from its own start); a pitched roof's: metres along its eaves and up its slope; a flat
 *             surface's: the row's x and z; an opening's: metres across and up from its lower left corner */

#define HF_WALL      0    /* a wall, a parapet's sides, an upstand's */
#define HF_ROOF      1    /* a pitched roof's covering */
#define HF_FLAT      2    /* a flat roof's deck */
#define HF_SOFFIT    3    /* under the eaves, the verges and a flat slab's overhang */
#define HF_FASCIA    4    /* the boards along the eaves and the verges' bargeboards */
#define HF_COPING    5    /* a parapet's, an upstand's or a stack's top */
#define HF_CHIMNEY   6    /* a stack's sides */
#define HF_GLASS     7    /* a window's glass, set back in its opening */
#define HF_REVEAL    8    /* an opening's sides, head and sill */
#define HF_DOOR      9    /* a front door, set back */
#define HF_GARAGE    10   /* a garage door, set back */
#define HF_STEP      11   /* steps, a stoop, a porch's floor */
#define HF_TRIM      12   /* painted timber: a porch's posts and its pediment */
#define HF_CEILING   13   /* a porch's ceiling, a recessed porch's */
#define HF_COUNT     14

/* Wall finishes (params z of HF_WALL, HF_REVEAL, HF_CHIMNEY, HF_COPING) */
#define HW_BRICK     0
#define HW_RENDER    1    /* render and stucco, painted */
#define HW_SIDING    2    /* painted timber or fibre-cement boards */
#define HW_STONE     3    /* ashlar, brownstone */
#define HW_BLOCK     4    /* concrete block, bare or painted */
#define HW_COUNT     5

/* params w of a brick wall, reveal, stack or coping: old stock brick, sooted (assets/materials/house_brick.json),
 * the styles built before the war; without it, facade_brick's wire-cut. The palette entry is the low three bits. */
#define HOUSE_COL_OLD 0x10u

/* Roof finishes (params z of HF_ROOF and HF_FLAT) */
#define HRM_CLAY     0    /* plain clay tiles, pantiles */
#define HRM_SLATE    1
#define HRM_CONCRETE 2    /* interlocking concrete tiles */
#define HRM_SHINGLE  3    /* asphalt shingles */
#define HRM_METAL    4    /* corrugated sheet */
#define HRM_BARREL   5    /* Spanish barrel tiles */
#define HRM_GLAZED   6    /* glazed kawara and Chinese grey tiles */
#define HRM_FELT     7    /* a flat roof's felt or membrane */
#define HRM_SLAB     8    /* a flat roof's bare concrete slab */
#define HRM_COUNT    9

/* Steps' finishes (params z of HF_STEP) */
#define HST_CONCRETE 0
#define HST_STONE    1
#define HST_TILE     2
#define HST_TIMBER   3

/* aux */
#define HOUSE_AUX_FAR       0x80000000u   /* the base layer's far shell */
#define HOUSE_AUX_WINDOWS   0x40000000u   /* a far wall with windows painted on it, in this rhythm: */
#define HOUSE_AUX_STOREYH   0             /* 7 bits: floor to floor, cm over 200 */
#define HOUSE_AUX_PLINTH    7             /* 8 bits: the ground floor over the garden, cm */
#define HOUSE_AUX_STOREYS   15            /* 2 bits: storeys */
#define HOUSE_AUX_LENGTH    17            /* 10 bits: the wall's length, dm (its windows are spread along it) */
/* an opening's (HF_GLASS, HF_DOOR, HF_GARAGE): its size in cm, and its pattern (glazing bars, a door's panels) */
#define HOUSE_AUX_W         0             /* 10 bits */
#define HOUSE_AUX_H         10            /* 10 bits */
#define HOUSE_AUX_PATTERN   20            /* 4 bits */

/* Glazing (HOUSE_AUX_PATTERN of HF_GLASS) */
#define HG_PLAIN     0    /* one pane */
#define HG_SASH      1    /* a sash window, two panes one over the other */
#define HG_GEORGIAN  2    /* six over six */
#define HG_CASEMENT  3    /* two casements and a top light */
#define HG_GRID      4    /* a grid of small panes (craftsman, Japanese) */

/* The character kit (HM4: src/gen/pmodel_house.c, materials PMAT_HTRIM on; shaders/city/house.glsl shadeHouseKit):
 * what a house kit instance carries of its unit's finishes, so its parts take the shell's colours. In the
 * instance's 16 flag bits (KitInst.flags, the fragment's params w): */
#define HOUSE_KIT_TRIM      0     /* 3 bits: the trim's palette entry (H_TRIM) */
#define HOUSE_KIT_DOOR      3     /* 4 bits: the door's (H_DOOR; shutters too) */
#define HOUSE_KIT_BRICK     7     /* 3 bits: the brick's (H_BRICK) */
#define HOUSE_KIT_OLD       10    /* 1 bit: old stock brick (HOUSE_COL_OLD) */
#define HOUSE_KIT_STONE     11    /* 2 bits: the stone's (H_STONE) */
#define HOUSE_KIT_RAIN      13    /* 2 bits: rainwater goods, HKR_* */
/* ...and in its part byte (the fragment's params z): */
#define HOUSE_KIT_ROOF      0     /* 4 bits: the roof's covering (HRM_*) */
#define HOUSE_KIT_ROOFCOL   4     /* 3 bits: its colour */
/* Rainwater goods */
#define HKR_IRON     0    /* black painted cast iron */
#define HKR_WHITE    1    /* white plastic */
#define HKR_GREY     2    /* grey plastic */
#define HKR_BROWN    3    /* brown plastic */

/* The chunk record's bodies bit (ChunkGPU.bodies, src/core/common.h): the chunk's houses' near layer is drawn,
 * so the base layer's far shells give way (city.vert). The vendors' bodies keep bits 0-126 (VENDOR_BODIES_MAX). */
#define CHUNK_HOMES_NEAR_BIT 127

#endif
