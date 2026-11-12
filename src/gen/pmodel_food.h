#pragma once
#include "pmodel.h"

/* The street-food trades' modules (pmodel_food.c, roadmap SF1): their builders, which pmodel_kit.c's table
 * holds, and the heights the cooking line sets things down at (each over the module's foot). */
void m_char_grill(Pm *pm);
void m_skewers(Pm *pm);
void m_skewer_tray(Pm *pm);
void m_spit(Pm *pm);
void m_griddle(Pm *pm);
void m_griddle_s(Pm *pm);
void m_patty(Pm *pm);
void m_sausage(Pm *pm);
void m_bun(Pm *pm);
void m_bread_stack(Pm *pm);
void m_tortilla(Pm *pm);
void m_fryer(Pm *pm);
void m_fry_basket(Pm *pm);
void m_fried_tray(Pm *pm);
void m_crepe_plate(Pm *pm);
void m_crepe(Pm *pm);
void m_crepe_rake(Pm *pm);
void m_urn(Pm *pm);
void m_espresso(Pm *pm);
void m_cup_stack(Pm *pm);
void m_chestnut_wok(Pm *pm);
void m_roast_pan(Pm *pm);
void m_dumpling_tray(Pm *pm);
void m_gn_greens(Pm *pm);
void m_gn_meat(Pm *pm);
void m_gn_sauce(Pm *pm);
void m_curry_pot(Pm *pm);
void m_paper_cones(Pm *pm);
void m_icon_skewer(Pm *pm);
void m_icon_dumpling(Pm *pm);
void m_icon_kebab(Pm *pm);
void m_icon_burger(Pm *pm);
void m_icon_hotdog(Pm *pm);
void m_icon_drumstick(Pm *pm);
void m_icon_fries(Pm *pm);
void m_icon_taco(Pm *pm);
void m_icon_crepe(Pm *pm);
void m_icon_chestnut(Pm *pm);
void m_icon_teapot(Pm *pm);
void m_icon_flame(Pm *pm);

#define FOOD_GRILL_RIM    0.19f    /* the charcoal grill's rims: the skewers' sticks rest on them */
#define FOOD_GRILL_BED    0.10f    /* the top of its coals */
#define FOOD_GRIDDLE_TOP  0.185f   /* a griddle's plate */
#define FOOD_FRYER_TOP    0.27f    /* the fryer's frame round its wells */
#define FOOD_FRYER_OIL    0.225f   /* its oil */
#define FOOD_FRYER_RAIL   0.36f    /* the rail its baskets hang from (z -0.21) */
#define FOOD_CREPE_TOP    0.12f    /* a crepe plate's top */
#define FOOD_CHESTNUT_BED 0.085f   /* the chestnut wok's grit, over its bottom */
#define FOOD_CHESTNUT_S   0.36f    /* the chestnut wok's sphere (its rim 0.26 out, its ears to 0.29 at its sides) */
