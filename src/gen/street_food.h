#pragma once

/* The street-food trades (roadmap SF1 in ~/.claude/plans/street-vendors.md; the user, 2026-11-10: street food
 * only for now). The vendor grammar picks one per vendor (vendor_tables.c V_TRADES: affinities, the old
 * archetypes' painted stock and props); a canopy bar builds the trade's cooking line from it
 * (src/gen/noodle_bar.c nb_line: its station on the range, its bench, its food, steam, icons and menu).
 *
 *   noodles and ramen      stock pot; noodle baskets, a wok, a steamer or a saucepan
 *   soup and pho           two stock pots, herbs and sprouts in pans
 *   dumplings and bao      steamer stacks on both burners, trays of dumplings
 *   skewers                a charcoal grill, skewers over the coals
 *   kebab and shawarma     the vertical spit, a griddle for the bread
 *   burgers and hot dogs   a flat griddle, patties, sausages, buns
 *   fried food             a twin-well fryer, its baskets, a draining tray
 *   curry and rice         pots of curry, two rice cookers, a long bain-marie
 *   tacos                  a plancha, tortillas, fillings in pans
 *   crepes                 two round crepe plates
 *   chestnuts and yams     a wok of hot grit and chestnuts, a pan of sweet potatoes
 *   tea, coffee and drinks a water urn and an espresso machine; no burners */
enum { VT_NOODLE = 0, VT_SOUP, VT_DUMPLING, VT_SKEWER, VT_KEBAB, VT_BURGER, VT_FRIED, VT_CURRY, VT_TACO, VT_CREPE,
       VT_ROAST, VT_DRINKS, VT_COUNT };
