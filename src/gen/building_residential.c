#include "home_internal.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * Houses: section 13's grammar for the three house archetypes (residential roadmap HM2)
 *
 *   House    -> Main + Porch + Bay? + GarageWing? + RearExtension? + Chimney*
 *   Semi     -> Pair(two mirrored halves) + SharedRoof + Porch x 2
 *   Terrace  -> Row(n units) + Outrigger* + Bay* + Porch x n + Chimney x party walls
 *
 * A house is chosen as a plan (the main and its optional parts), then given
 * its roof, then its features (doors, porches, bays, chimneys), then its
 * garden (boundaries, gates, paths, drives, sheds, cars, trees). The style
 * (the region's palette, docs/residential-target.md) sets the choices'
 * weights; the rules (building_residential_rules.c) are the same everywhere.
 * ========================================================================= */

/* The styles' grammar weights. Values are shares or chances unless a unit is named. */
static const HStyleDef STYLES[HS_COUNT] = {
    /* North America: craftsman bungalows with deep verandas, Spanish revival in stucco and clay,
     * ranches (the departure: 18-25 degree hips), tract houses with garages; the north-east's
     * rowhouses on stoops and triple-deckers with stacked porches */
    [HS_CRAFTSMAN] = { .name = "craftsman", .region = PLAN_REGION_NA, .arch = { 0.30f, 0.30f, 0.00f }, .storeys = { 0.45f, 0.55f, 0.0f },
        .roof = { 0.8f, 0.2f, 0, 0 }, .pitch0 = 25, .pitch1 = 35, .minPitch = 25, .eave0 = 0.6f, .eave1 = 0.9f, .depth0 = 8.5f, .depth1 = 11.0f,
        .bay = 0.15f, .canted = 0.5f, .garage = 0.15f, .extension = 0.3f,
        .porch = { 0.2f, 0, 0, 0, 0, 0.8f }, .chimney = 1.0f, .bound = { 0.7f, 0.05f, 0.1f, 0.15f }, .plinth0 = 0.4f, .plinth1 = 0.8f },
    [HS_SPANISH] = { .name = "spanish", .region = PLAN_REGION_NA, .arch = { 0.25f, 0.20f, 0.0f }, .storeys = { 0.6f, 0.4f, 0.0f },
        .roof = { 0.4f, 0.3f, 0, 0.3f }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.2f, .eave1 = 0.4f, .depth0 = 9.0f, .depth1 = 12.0f,
        .bay = 0.05f, .garage = 0.35f, .extension = 0.2f,
        .porch = { 0.3f, 0, 0, 0.4f, 0.3f, 0 }, .chimney = 0.5f, .bound = { 0.5f, 0.5f, 0, 0 }, .plinth0 = 0.15f, .plinth1 = 0.3f },
    [HS_RANCH] = { .name = "ranch", .region = PLAN_REGION_NA, .arch = { 0.25f, 0.30f, 0.0f }, .storeys = { 1.0f, 0.0f, 0.0f },
        .roof = { 0.3f, 0.7f, 0, 0 }, .pitch0 = 18, .pitch1 = 25, .minPitch = 18, .eave0 = 0.4f, .eave1 = 0.7f, .depth0 = 9.0f, .depth1 = 12.0f,
        .bay = 0.1f, .canted = 0.3f, .garage = 0.7f, .extension = 0.25f,
        .porch = { 0.2f, 0.3f, 0, 0.3f, 0.2f, 0 }, .chimney = 0.5f, .bound = { 0.85f, 0, 0.1f, 0.05f }, .plinth0 = 0.15f, .plinth1 = 0.3f },
    [HS_TRACT] = { .name = "tract", .region = PLAN_REGION_NA, .arch = { 0.20f, 0.20f, 0.10f }, .storeys = { 0.25f, 0.75f, 0.0f },
        .roof = { 0.6f, 0.4f, 0, 0 }, .pitch0 = 25, .pitch1 = 35, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.5f, .depth0 = 8.0f, .depth1 = 11.0f,
        .bay = 0.1f, .canted = 0.4f, .garage = 0.8f, .extension = 0.25f,
        .porch = { 0.3f, 0.2f, 0, 0.2f, 0.3f, 0 }, .chimney = 0.3f, .bound = { 0.9f, 0, 0, 0.1f }, .plinth0 = 0.15f, .plinth1 = 0.3f },
    [HS_ROWHOUSE] = { .name = "rowhouse", .region = PLAN_REGION_NA, .arch = { 0.0f, 0.0f, 1.0f }, .storeys = { 0.0f, 0.0f, 0.4f, 0.6f },
        .roof = { 0, 0, 0, 1.0f }, .pitch0 = 30, .pitch1 = 40, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.5f, .depth0 = 9.0f, .depth1 = 11.5f,
        .bay = 0.6f, .canted = 0.7f, .bayFull = 1, .outrigger = 0.4f, .extension = 0.3f,
        .porch = { 0, 0, 0, 0.3f, 0.7f, 0 }, .chimney = 1.0f, .bound = { 0.4f, 0.3f, 0, 0.3f }, .plinth0 = 1.1f, .plinth1 = 1.5f, .handed = 1 },
    [HS_TRIPLEDECKER] = { .name = "tripledecker", .region = PLAN_REGION_NA, .arch = { 0.15f, 0.0f, 0.0f }, .storeys = { 0.0f, 0.0f, 1.0f },
        .roof = { 0.5f, 0, 0, 0.5f }, .pitch0 = 30, .pitch1 = 40, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.5f, .depth0 = 10.0f, .depth1 = 13.0f,
        .bay = 0.7f, .bay2 = 1.0f, .canted = 0.6f, .extension = 0.1f,
        .porch = { 0.6f, 0, 0.3f, 0, 0.1f, 0 }, .chimney = 0.3f, .bound = { 0.5f, 0, 0, 0.5f }, .plinth0 = 0.5f, .plinth1 = 0.9f },
    /* Britain and NW Europe: Victorian bays, outriggers and tall stacks; Georgian flat fronts behind
     * a parapet; inter-war hipped semis with two-storey bays; post-war and modern plainer */
    [HS_VICTORIAN] = { .name = "victorian", .region = PLAN_REGION_UK, .arch = { 0.20f, 0.20f, 0.50f }, .storeys = { 0.0f, 0.75f, 0.25f },
        .roof = { 0.7f, 0.3f, 0, 0 }, .pitch0 = 35, .pitch1 = 45, .minPitch = 25, .eave0 = 0.2f, .eave1 = 0.4f, .depth0 = 8.0f, .depth1 = 10.0f,
        .bay = 0.8f, .bay2 = 0.4f, .canted = 0.7f, .outrigger = 0.8f, .extension = 0.3f,
        .porch = { 0.1f, 0, 0.1f, 0.6f, 0.2f, 0 }, .chimney = 2.0f, .bound = { 0, 0.7f, 0.3f, 0 }, .plinth0 = 0.3f, .plinth1 = 0.6f, .handed = 1 },
    [HS_GEORGIAN] = { .name = "georgian", .region = PLAN_REGION_UK, .arch = { 0.10f, 0.05f, 0.15f }, .storeys = { 0.0f, 0.1f, 0.5f, 0.4f },
        .roof = { 1.0f, 0, 0, 0 }, .pitch0 = 30, .pitch1 = 40, .minPitch = 25, .eave0 = 0.0f, .eave1 = 0.0f, .depth0 = 8.0f, .depth1 = 10.0f,
        .outrigger = 0.4f, .extension = 0.2f,
        .porch = { 0, 0, 0, 0, 1.0f, 0 }, .chimney = 2.0f, .bound = { 0, 0.9f, 0.1f, 0 }, .plinth0 = 0.4f, .plinth1 = 0.9f, .parapet = 1, .handed = 1 },
    [HS_INTERWAR] = { .name = "interwar", .region = PLAN_REGION_UK, .arch = { 0.30f, 0.45f, 0.05f }, .storeys = { 0.0f, 1.0f, 0.0f },
        .roof = { 0.4f, 0.6f, 0, 0 }, .pitch0 = 35, .pitch1 = 45, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.5f, .depth0 = 7.5f, .depth1 = 9.5f,
        .bay = 0.9f, .bay2 = 0.8f, .canted = 0.5f, .garage = 0.3f, .extension = 0.3f,
        .porch = { 0.2f, 0.1f, 0.3f, 0.3f, 0.1f, 0 }, .chimney = 1.0f, .bound = { 0, 0.5f, 0.4f, 0.1f }, .plinth0 = 0.2f, .plinth1 = 0.4f },
    [HS_POSTWAR] = { .name = "postwar", .region = PLAN_REGION_UK, .arch = { 0.20f, 0.25f, 0.20f }, .storeys = { 0.1f, 0.9f, 0.0f },
        .roof = { 0.7f, 0.3f, 0, 0 }, .pitch0 = 30, .pitch1 = 40, .minPitch = 25, .eave0 = 0.2f, .eave1 = 0.3f, .depth0 = 7.0f, .depth1 = 9.0f,
        .garage = 0.3f, .extension = 0.3f,
        .porch = { 0, 0.2f, 0.1f, 0, 0.7f, 0 }, .chimney = 0.6f, .bound = { 0.2f, 0.3f, 0.2f, 0.3f }, .plinth0 = 0.15f, .plinth1 = 0.3f },
    [HS_MODERN] = { .name = "modern", .region = PLAN_REGION_UK, .arch = { 0.20f, 0.10f, 0.15f }, .storeys = { 0.0f, 0.85f, 0.15f },
        .roof = { 0.6f, 0.1f, 0, 0.3f }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.2f, .eave1 = 0.4f, .depth0 = 8.0f, .depth1 = 10.0f,
        .garage = 0.5f, .extension = 0.2f,
        .porch = { 0, 0.2f, 0, 0.3f, 0.5f, 0 }, .chimney = 0.0f, .bound = { 0.4f, 0.3f, 0.1f, 0.2f }, .plinth0 = 0.15f, .plinth1 = 0.3f },
    /* Southern Europe */
    [HS_SEU_VILLA] = { .name = "villa", .region = PLAN_REGION_SEU, .arch = { 1.0f, 0.5f, 0.0f }, .storeys = { 0.05f, 0.6f, 0.35f },
        .roof = { 0.25f, 0.75f, 0, 0 }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.6f, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.3f, .extension = 0.2f,
        .porch = { 0.3f, 0.2f, 0, 0.3f, 0.2f, 0 }, .chimney = 0.5f, .bound = { 0, 0.8f, 0.1f, 0.1f }, .plinth0 = 0.2f, .plinth1 = 0.5f,
        .wallH0 = 1.2f, .wallH1 = 1.8f },
    [HS_TOWNHOUSE] = { .name = "townhouse", .region = PLAN_REGION_SEU, .arch = { 0.0f, 0.3f, 1.0f }, .storeys = { 0.0f, 0.0f, 0.6f, 0.4f },
        .roof = { 0.5f, 0, 0, 0.5f }, .pitch0 = 25, .pitch1 = 28, .minPitch = 25, .eave0 = 0.25f, .eave1 = 0.4f, .depth0 = 9.0f, .depth1 = 12.0f,
        .outrigger = 0.3f, .extension = 0.2f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.3f, .bound = { 1.0f, 0, 0, 0 }, .plinth0 = 0.05f, .plinth1 = 0.15f,
        .street = 1, .arched = 0.4f, .doorW0 = 1.0f, .doorW1 = 1.25f },
    /* East Asia */
    [HS_JAPAN] = { .name = "japan", .region = PLAN_REGION_EAS, .arch = { 1.0f, 0.5f, 0.3f }, .storeys = { 0.1f, 0.9f, 0.0f },
        .roof = { 0.5f, 0.5f, 0, 0 }, .pitch0 = 25, .pitch1 = 32, .minPitch = 25, .eave0 = 0.4f, .eave1 = 0.7f, .depth0 = 7.0f, .depth1 = 10.0f,
        .garage = 0.3f, .extension = 0.1f,
        .porch = { 0, 0.3f, 0, 0.3f, 0.4f, 0 }, .chimney = 0.0f, .bound = { 0.3f, 0.5f, 0.1f, 0.1f }, .plinth0 = 0.3f, .plinth1 = 0.5f },
    [HS_CHINA] = { .name = "china", .region = PLAN_REGION_EAS, .arch = { 0.4f, 0.2f, 1.0f }, .storeys = { 0.25f, 0.05f, 0.55f, 0.15f },
        .roof = { 0.25f, 0, 0, 0.75f }, .pitch0 = 25, .pitch1 = 32, .minPitch = 25, .eave0 = 0.4f, .eave1 = 0.6f, .depth0 = 8.0f, .depth1 = 11.0f,
        .outrigger = 0.1f, .garage = 0.05f, .extension = 0.3f,
        .porch = { 0, 0, 0, 0.6f, 0.4f, 0 }, .chimney = 0.0f, .bound = { 0, 0.9f, 0, 0.1f }, .plinth0 = 0.15f, .plinth1 = 0.3f,
        .wallH0 = 1.8f, .wallH1 = 2.4f, .doorW0 = 1.1f, .doorW1 = 1.4f },
    [HS_KOREA] = { .name = "korea", .region = PLAN_REGION_EAS, .arch = { 1.0f, 0.5f, 0.5f }, .storeys = { 0.0f, 0.0f, 0.45f, 0.55f },
        .roof = { 0, 0.35f, 0, 0.65f }, .pitch0 = 28, .pitch1 = 38, .minPitch = 25, .eave0 = 0.2f, .eave1 = 0.35f, .depth0 = 10.0f, .depth1 = 12.0f,
        .garage = 0.0f, .extension = 0.1f,
        .porch = { 0, 0, 0, 0.5f, 0.5f, 0 }, .chimney = 0.0f, .bound = { 0, 0.8f, 0, 0.2f }, .plinth0 = 0.05f, .plinth1 = 0.15f,
        .pilotis = 0.8f },
    /* South-East Asia: shophouses behind their five-foot way (a veranda here), link houses with car porches */
    [HS_SHOPHOUSE] = { .name = "shophouse", .region = PLAN_REGION_SEA, .arch = { 0.0f, 0.0f, 1.0f }, .storeys = { 0.0f, 0.55f, 0.45f },
        .roof = { 1.0f, 0, 0, 0 }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.3f, .eave1 = 0.5f, .depth0 = 10.0f, .depth1 = 12.0f,
        .outrigger = 0.2f, .extension = 0.4f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.0f, .bound = { 1.0f, 0, 0, 0 }, .plinth0 = 0.15f, .plinth1 = 0.25f, .parapet = 1,
        .street = 1, .arcade = 1.0f, .shop = 1.0f, .storeyH0 = 3.1f, .storeyH1 = 3.5f },
    [HS_LINKHOUSE] = { .name = "linkhouse", .region = PLAN_REGION_SEA, .arch = { 1.0f, 0.5f, 0.8f }, .storeys = { 0.1f, 0.9f, 0.0f },
        .roof = { 0.6f, 0.4f, 0, 0 }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.5f, .eave1 = 0.8f, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.4f, .extension = 0.4f,
        .porch = { 0.3f, 0.3f, 0, 0, 0.4f, 0 }, .chimney = 0.0f, .bound = { 0, 0.3f, 0, 0.7f }, .plinth0 = 0.1f, .plinth1 = 0.25f },
    /* South Asia */
    [HS_RCC] = { .name = "rcc", .region = PLAN_REGION_SAS, .arch = { 0.5f, 0.2f, 1.0f }, .storeys = { 0.0f, 0.2f, 0.5f, 0.3f },
        .roof = { 0, 0, 0, 1.0f }, .pitch0 = 25, .pitch1 = 25, .minPitch = 25, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.05f, .extension = 0.3f,
        .porch = { 0, 0, 0, 0.3f, 0.7f, 0 }, .chimney = 0.0f, .bound = { 0, 0.9f, 0, 0.1f }, .plinth0 = 0.1f, .plinth1 = 0.25f,
        .shop = 0.3f, .uneven = 0.25f },
    [HS_BUNGALOW] = { .name = "bungalow", .region = PLAN_REGION_SAS, .arch = { 1.0f, 0.5f, 0.2f }, .storeys = { 0.8f, 0.2f, 0.0f },
        .roof = { 0, 0.85f, 0, 0.15f }, .pitch0 = 25, .pitch1 = 32, .minPitch = 25, .eave0 = 0.5f, .eave1 = 0.8f, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.2f, .extension = 0.3f,
        .porch = { 0.2f, 0.1f, 0, 0, 0, 0.7f }, .chimney = 0.0f, .bound = { 0, 0.9f, 0.1f, 0 }, .plinth0 = 0.4f, .plinth1 = 0.8f },
    /* Middle East and North Africa: walled villas, courtyard rows */
    [HS_MENA_VILLA] = { .name = "villa", .region = PLAN_REGION_MENA, .arch = { 1.0f, 0.5f, 0.3f }, .storeys = { 0.0f, 0.6f, 0.4f },
        .roof = { 0, 0, 0, 1.0f }, .pitch0 = 25, .pitch1 = 25, .minPitch = 25, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.3f, .extension = 0.2f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.0f, .bound = { 0, 1.0f, 0, 0 }, .plinth0 = 0.2f, .plinth1 = 0.5f,
        .wallH0 = 1.8f, .wallH1 = 2.6f, .arched = 1.0f, .doorW0 = 1.2f, .doorW1 = 1.5f, .storeyH0 = 3.0f, .storeyH1 = 3.3f },
    [HS_COURTYARD] = { .name = "courtyard", .region = PLAN_REGION_MENA, .arch = { 0.3f, 0.0f, 1.0f }, .storeys = { 0.45f, 0.55f, 0.0f },
        .roof = { 0, 0, 0, 1.0f }, .pitch0 = 25, .pitch1 = 25, .minPitch = 25, .depth0 = 9.0f, .depth1 = 12.0f,
        .outrigger = 0.5f, .extension = 0.2f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.0f, .bound = { 1.0f, 0, 0, 0 }, .plinth0 = 0.05f, .plinth1 = 0.15f,
        .street = 1, .arched = 1.0f, .doorW0 = 1.4f, .doorW1 = 1.8f, .storeyH0 = 3.2f, .storeyH1 = 3.6f },
    /* Sub-Saharan Africa */
    [HS_BLOCK] = { .name = "block", .region = PLAN_REGION_SSA, .arch = { 1.0f, 0.5f, 0.5f }, .storeys = { 0.75f, 0.25f, 0.0f },
        .roof = { 0.25f, 0.75f, 0, 0 }, .pitch0 = 22, .pitch1 = 28, .minPitch = 20, .eave0 = 0.4f, .eave1 = 0.7f, .depth0 = 8.0f, .depth1 = 11.0f,
        .garage = 0.05f, .extension = 0.3f,
        .porch = { 0.2f, 0.2f, 0, 0, 0, 0.6f }, .chimney = 0.0f, .bound = { 0.2f, 0.6f, 0, 0.2f }, .plinth0 = 0.3f, .plinth1 = 0.6f },
    [HS_WALLED] = { .name = "walled", .region = PLAN_REGION_SSA, .arch = { 1.0f, 0.5f, 0.3f }, .storeys = { 0.3f, 0.7f, 0.0f },
        .roof = { 0, 0.8f, 0, 0.2f }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.4f, .eave1 = 0.7f, .depth0 = 9.0f, .depth1 = 12.0f,
        .garage = 0.3f, .extension = 0.2f,
        .porch = { 0.3f, 0, 0, 0.2f, 0, 0.5f }, .chimney = 0.0f, .bound = { 0, 1.0f, 0, 0 }, .plinth0 = 0.2f, .plinth1 = 0.5f,
        .wallH0 = 2.0f, .wallH1 = 2.6f },
    /* Latin America */
    [HS_SELFBUILT] = { .name = "selfbuilt", .region = PLAN_REGION_LAT, .arch = { 0.5f, 0.3f, 1.0f }, .storeys = { 0.1f, 0.4f, 0.4f, 0.1f },
        .roof = { 0, 0, 0, 1.0f }, .pitch0 = 25, .pitch1 = 25, .minPitch = 25, .depth0 = 8.0f, .depth1 = 12.0f,
        .extension = 0.4f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.0f, .bound = { 1.0f, 0, 0, 0 }, .plinth0 = 0.05f, .plinth1 = 0.15f,
        .street = 1, .shop = 0.4f, .uneven = 0.4f },
    [HS_COLONIAL] = { .name = "colonial", .region = PLAN_REGION_LAT, .arch = { 0.3f, 0.3f, 0.8f }, .storeys = { 1.0f, 0.0f, 0.0f },
        .roof = { 0.8f, 0.2f, 0, 0 }, .pitch0 = 25, .pitch1 = 30, .minPitch = 25, .eave0 = 0.6f, .eave1 = 0.9f, .depth0 = 9.0f, .depth1 = 12.0f,
        .outrigger = 0.5f, .extension = 0.2f,
        .porch = { 0, 0, 0, 1.0f, 0, 0 }, .chimney = 0.0f, .bound = { 1.0f, 0, 0, 0 }, .plinth0 = 0.05f, .plinth1 = 0.15f,
        .street = 1, .arched = 0.7f, .doorW0 = 1.2f, .doorW1 = 1.5f, .storeyH0 = 3.3f, .storeyH1 = 3.8f },
};

const HStyleDef *home_style_def(int s) { return &STYLES[s >= 0 && s < HS_COUNT ? s : HS_TRACT]; }
int home_style_street(int s) { return home_style_def(s)->street; }

/* The yards by region (HM2b): see HomeYard. Britain the largest (a terraced street's 6-10 m back yards),
 * Southern Europe next, North America's small lots, then the Middle East and Africa; East and South Asia
 * nearest Tokyo's half-metre sides and two-metre backs. Pads: Malaysia's link houses park in their car
 * porches, North America on its driveway aprons; Britain's semis have paved over their front gardens. */
static const HomeYard YARDS[PLAN_REGION_COUNT] = {
    [PLAN_REGION_NA]   = { .frontMin = 1.5f, .rearMin = 4.0f, .front0 = 2.0f, .front1 = 3.5f, .side = 1.0f, .semiOpen = 1.0f, .gap = 2.0f, .proj = 0.5f,
                           .width = { { 9.0f, 12.5f }, { 6.0f, 8.0f }, { 4.2f, 6.0f } }, .depth = { { 20.0f, 26.0f }, { 20.0f, 25.0f }, { 17.0f, 23.0f } },
                           .pad = { 0.75f, 0.5f, 0.0f }, .surface = { 0.45f, 0.20f, 0.35f, 0.0f } },
    [PLAN_REGION_UK]   = { .frontMin = 1.5f, .rearMin = 6.0f, .front0 = 1.5f, .front1 = 3.0f, .side = 1.0f, .semiOpen = 1.0f, .gap = 2.0f, .proj = 0.5f,
                           .width = { { 9.0f, 13.0f }, { 6.0f, 8.5f }, { 4.2f, 6.0f } }, .depth = { { 22.0f, 28.0f }, { 21.0f, 27.0f }, { 19.0f, 25.0f } },
                           .pad = { 0.55f, 0.45f, 0.1f }, .surface = { 0.50f, 0.15f, 0.35f, 0.0f } },
    [PLAN_REGION_SEU]  = { .frontMin = 1.0f, .rearMin = 4.5f, .front0 = 1.0f, .front1 = 2.5f, .side = 1.0f, .semiOpen = 1.0f, .gap = 2.0f, .proj = 0.4f,
                           .width = { { 9.0f, 13.0f }, { 6.0f, 8.5f }, { 4.5f, 6.5f } }, .depth = { { 20.0f, 26.0f }, { 20.0f, 25.0f }, { 16.0f, 22.0f } },
                           .pad = { 0.5f, 0.4f, 0.1f }, .surface = { 0.55f, 0.15f, 0.30f, 0.0f } },
    [PLAN_REGION_EAS]  = { .frontMin = 1.0f, .rearMin = 2.0f, .front0 = 1.0f, .front1 = 2.0f, .side = 0.5f, .semiOpen = 0.5f, .gap = 1.0f, .proj = 0.4f,
                           .width = { { 7.0f, 10.0f }, { 5.5f, 7.0f }, { 4.2f, 6.0f } }, .depth = { { 14.0f, 18.0f }, { 14.0f, 18.0f }, { 13.0f, 17.0f } },
                           .pad = { 0.5f, 0.3f, 0.15f }, .surface = { 0.45f, 0.25f, 0.30f, 0.0f } },
    [PLAN_REGION_SEA]  = { .frontMin = 1.5f, .rearMin = 2.5f, .front0 = 1.5f, .front1 = 3.0f, .side = 0.75f, .semiOpen = 0.75f, .gap = 1.5f, .proj = 0.5f,
                           .width = { { 8.0f, 11.0f }, { 6.0f, 8.0f }, { 4.5f, 6.5f } }, .depth = { { 17.0f, 22.0f }, { 18.0f, 22.0f }, { 17.0f, 22.0f } },
                           .pad = { 0.6f, 0.6f, 0.55f }, .surface = { 0.65f, 0.05f, 0.25f, 0.05f } },
    [PLAN_REGION_SAS]  = { .frontMin = 1.0f, .rearMin = 2.0f, .front0 = 1.0f, .front1 = 2.5f, .side = 0.75f, .semiOpen = 0.75f, .gap = 1.5f, .proj = 0.4f,
                           .width = { { 7.0f, 10.0f }, { 6.0f, 8.0f }, { 4.2f, 6.0f } }, .depth = { { 15.0f, 20.0f }, { 15.0f, 20.0f }, { 14.0f, 18.0f } },
                           .pad = { 0.3f, 0.2f, 0.1f }, .surface = { 0.55f, 0.05f, 0.15f, 0.25f } },
    [PLAN_REGION_MENA] = { .frontMin = 1.5f, .rearMin = 3.0f, .front0 = 2.0f, .front1 = 3.5f, .side = 1.0f, .semiOpen = 1.0f, .gap = 2.0f, .proj = 0.5f,
                           .width = { { 9.0f, 13.0f }, { 6.0f, 8.5f }, { 4.5f, 6.5f } }, .depth = { { 18.0f, 24.0f }, { 18.0f, 23.0f }, { 15.0f, 20.0f } },
                           .pad = { 0.5f, 0.4f, 0.1f }, .surface = { 0.60f, 0.15f, 0.15f, 0.10f } },
    [PLAN_REGION_SSA]  = { .frontMin = 1.5f, .rearMin = 3.0f, .front0 = 2.0f, .front1 = 4.0f, .side = 1.0f, .semiOpen = 1.0f, .gap = 2.0f, .proj = 0.5f,
                           .width = { { 9.0f, 13.0f }, { 6.0f, 8.5f }, { 4.5f, 6.5f } }, .depth = { { 18.0f, 24.0f }, { 18.0f, 23.0f }, { 15.0f, 20.0f } },
                           .pad = { 0.45f, 0.3f, 0.05f }, .surface = { 0.40f, 0.10f, 0.10f, 0.40f } },
    [PLAN_REGION_LAT]  = { .frontMin = 1.0f, .rearMin = 2.5f, .front0 = 1.0f, .front1 = 2.5f, .side = 0.75f, .semiOpen = 0.75f, .gap = 1.5f, .proj = 0.4f,
                           .width = { { 7.0f, 10.0f }, { 6.0f, 8.0f }, { 4.2f, 6.5f } }, .depth = { { 15.0f, 21.0f }, { 15.0f, 20.0f }, { 14.0f, 20.0f } },
                           .pad = { 0.4f, 0.3f, 0.3f }, .surface = { 0.60f, 0.05f, 0.15f, 0.20f } },
};

const HomeYard *home_yard(int region) { return &YARDS[region >= 0 && region < PLAN_REGION_COUNT ? region : PLAN_REGION_NA]; }

const char *home_surface_name(int s) {
    static const char *const N[HY_COUNT] = { "paved", "gravel", "planted", "soil" };
    return s >= 0 && s < HY_COUNT ? N[s] : "?";
}
const char *home_style_name(int s) { return s >= 0 && s < HS_COUNT ? STYLES[s].name : "?"; }
int home_style_region(int s) { return s >= 0 && s < HS_COUNT ? STYLES[s].region : -1; }
float home_style_min_pitch(int s) { return home_style_def(s)->minPitch; }

int home_style_find(int region, const char *name) {
    /* "villa:mena": a name qualified by its region's code (two styles share "villa") */
    const char *colon = strchr(name, ':');
    if (colon) {
        char base[32];
        const size_t n = (size_t)(colon - name) < sizeof(base) - 1 ? (size_t)(colon - name) : sizeof(base) - 1;
        memcpy(base, name, n);
        base[n] = 0;
        for (int r = 0; r < PLAN_REGION_COUNT; ++r)
            if (strcmp(plan_region_code(r), colon + 1) == 0) return home_style_find(r, base);
        return -1;
    }
    for (int s = 0; s < HS_COUNT; ++s)
        if ((region < 0 || STYLES[s].region == region) && strcmp(STYLES[s].name, name) == 0) return s;
    return -1;
}

static const char *const ROLE_NAMES[HV_ROLE_COUNT] = { "main", "pair", "row", "wing", "extension", "bay", "outrigger", "porch" };
static const char *const ROOF_NAMES[HR_COUNT] = { "gable", "hip", "lean", "flat" };
static const char *const PORCH_NAMES[HP_COUNT] = { "open", "lean-to", "enclosed", "recessed", "canopy", "veranda", "arcade", "pilotis" };
const char *home_role_name(int r) { return r >= 0 && r < HV_ROLE_COUNT ? ROLE_NAMES[r] : "?"; }
const char *home_roof_name(int r) { return r >= 0 && r < HR_COUNT ? ROOF_NAMES[r] : "?"; }
const char *home_porch_name(int k) { return k >= 0 && k < HP_COUNT ? PORCH_NAMES[k] : "?"; }

/* A row's style: its region's, by archetype. North America's rowhouses and triple-deckers stand in
 * the north-east's old cities only, its Spanish revival mostly in the south-west; East Asia splits
 * by country (Korea, Japan, else China). */
int home_style_pick(int region, int arch, double lat, double lon, uint32_t h) {
    float w[HS_COUNT], sum = 0.0f;
    const int ne = lon > -82.0 && lat > 37.0, sw = lat < 37.0 && lon < -95.0;
    const int korea = lon > 124.0 && lon < 129.6 && lat > 33.0 && lat < 38.7, japan = !korea && lon > 129.0;
    for (int s = 0; s < HS_COUNT; ++s) {
        w[s] = STYLES[s].region == region ? STYLES[s].arch[arch % HOME_ARCH_COUNT] : 0.0f;
        if (region == PLAN_REGION_NA) {
            if ((s == HS_ROWHOUSE || s == HS_TRIPLEDECKER) && !ne) w[s] = 0.0f;
            if (s == HS_SPANISH && !sw) w[s] *= 0.2f;
        }
        if (region == PLAN_REGION_EAS)
            w[s] *= s == HS_KOREA ? (korea ? 1.0f : 0.0f) : s == HS_JAPAN ? (japan ? 1.0f : 0.0f) : (korea || japan ? 0.0f : 1.0f);
        sum += w[s];
    }
    if (sum <= 0.0f) {   /* an archetype the region doesn't build (forced): its first style that builds houses at all */
        for (int s = 0; s < HS_COUNT; ++s)
            if (STYLES[s].region == region) return s;
        return HS_TRACT;
    }
    float x = hash_float(h) * sum;
    for (int s = 0; s < HS_COUNT; ++s) {
        if (x < w[s]) return s;
        x -= w[s];
    }
    for (int s = HS_COUNT - 1; s >= 0; --s)
        if (w[s] > 0.0f) return s;
    return HS_TRACT;
}

int home_style_storeys(int style, int arch, uint32_t h) {
    const HStyleDef *S = home_style_def(style);
    float sum = 0.0f;
    for (int k = 0; k < HOME_STOREYS_MAX; ++k) sum += S->storeys[k];
    float x = hash_float(h) * sum;
    int n = 0;
    for (int k = 0; k < HOME_STOREYS_MAX && !n; ++k) {
        if (x < S->storeys[k]) n = k + 1;
        x -= S->storeys[k];
    }
    for (int k = HOME_STOREYS_MAX - 1; k >= 0 && !n; --k)
        if (S->storeys[k] > 0.0f) n = k + 1;
    if (!n) n = 2;
    if (arch == HOME_SEMI && n == 1) n = 2;   /* a semi-detached bungalow pair reads as a terrace stub; the pairs are two storeys */
    return n;
}

const HVol *home_unit_vol(const HGraph *g, int vi, int u, HVol *tmp) {
    const HVol *v = &g->vol[vi];
    if (v->unit != HG_ROW_WIDE || u < 0 || u >= g->nunit || !g->unit[u].storeys || g->unit[u].storeys >= v->storeys) return v;
    *tmp = *v;
    tmp->storeys = g->unit[u].storeys;
    tmp->eaves = g->plinth + tmp->storeys * g->storeyH;
    home_vol_top(tmp);
    return tmp;
}

/* ---------------------------------------------------------------------------
 * The grammar
 * ------------------------------------------------------------------------- */

typedef struct HCtx {
    HGraph          *g;
    BLog            *log;
    const HomePlot  *P;
    const HStyleDef *S;
    const HomeYard  *Y;      /* the region's yards (HM2b) */
    float            aff;
    int              level;
    Rng              r;
    /* the row's choices, shared by its units */
    int              roof, porch, bay, bay2, canted, outrig, outrigGable, garage, front, pad;
    float            pitch, eave, depth, wallH, frontH;
    /* HM5e1's, drawn from a stream of their own (r5) so the styles it left alone keep their houses */
    Rng              r5;
    int              ground;      /* the row's ground floor: HP_ARCADE, HP_PILOTIS, or -1 a house's */
    int              arched;      /* its doors under arches */
    float            groundD;     /* the arcade's or the pilotis' depth */
} HCtx;

static int wpick(Rng *r, const float *w, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += w[i];
    if (sum <= 0.0f) return 0;
    float x = rng_float(r) * sum;
    for (int i = 0; i < n; ++i) {
        if (x < w[i]) return i;
        x -= w[i];
    }
    return n - 1;
}

static int chance(Rng *r, float p) { return rng_float(r) < p; }

void home_repair(HGraph *g, BLog *log, const char *tag, const char *fmt, ...) {
    va_list ap;
    blog_printf(log, "  repair %s: ", tag);
    va_start(ap, fmt);
    blog_vprintf(log, fmt, ap);
    va_end(ap);
    blog_printf(log, "\n");
    g->repairs++;
}

static int add_vol(HGraph *g, int role, int unit, int parent, float x0, float z0, float x1, float z1, int storeys, int roof) {
    if (g->nvol >= HG_MAX_VOL) return -1;
    HVol *v = &g->vol[g->nvol];
    memset(v, 0, sizeof(*v));
    v->role = (uint8_t)role; v->unit = (uint8_t)unit; v->parent = (int8_t)parent;
    v->x0 = x0; v->z0 = z0; v->x1 = x1; v->z1 = z1;
    v->storeys = (uint8_t)storeys; v->roof = (uint8_t)roof;
    v->ridgeX = (x1 - x0) >= (z1 - z0);   /* a pitched roof spans the shorter side (section 13.4) */
    return g->nvol++;
}

/* The roof's top from its eaves, pitch and span (a flat roof's parapet or fascia). */
void home_vol_top(HVol *v) {
    const float t = tanf(v->pitch * 0.0174532925f);
    switch (v->roof) {
    case HR_GABLE: case HR_HIP: v->top = v->eaves + t * 0.5f * (v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0); break;
    case HR_LEAN:  v->top = v->eaves + t * (v->high == HSIDE_S || v->high == HSIDE_N ? v->z1 - v->z0 : v->x1 - v->x0); break;
    default:       v->top = v->eaves + (v->parapet == 2 ? (v->role <= HV_ROW ? 0.7f : 0.3f) : 0.25f); break;   /* a main's parapet, an extension's upstand */
    }
}

/* A volume's main: a pair's or row's for its units' parts, or the detached house's. */
static int main_of(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

/* The flat roof of a habitable volume carries a parapet (section 13.4). */
static void set_flat(HVol *v) { v->roof = HR_FLAT; v->parapet = (uint8_t)(v->role <= HV_EXTENSION || v->role == HV_OUTRIGGER ? 2 : 0); v->overhang = 0.0f; }

/* The main's roof from the row's choice; ridges along the longer side. */
static void roof_main(HCtx *c, HVol *m) {
    m->roof = (uint8_t)c->roof;
    m->pitch = c->pitch;
    m->overhang = c->eave;
    m->ridgeX = (m->x1 - m->x0) >= (m->z1 - m->z0);
    if (m->roof == HR_FLAT) set_flat(m);
    else if (c->S->parapet) m->parapet = 1;
    m->eaves = c->g->plinth + m->storeys * c->g->storeyH;
}

/* A lean-to roof against its parent: rising to the side it touches. */
static void lean_against(HVol *v, const HVol *p, float pitch) {
    v->roof = HR_LEAN;
    v->pitch = pitch;
    v->ridgeX = 0;
    if (fabsf(v->z1 - p->z0) < 0.05f) v->high = HSIDE_N;
    else if (fabsf(v->z0 - p->z1) < 0.05f) v->high = HSIDE_S;
    else if (fabsf(v->x1 - p->x0) < 0.05f) v->high = HSIDE_E;
    else v->high = HSIDE_W;
}

/* --- a unit's front: its door, porch and bay ------------------------------- */

typedef struct Front { float ux0, ux1; int side; } Front;   /* the unit's span of the main's front wall; door side -1 left, 0 centre, 1 right */

/* An arch's rise over a door or recess w wide springing at `spring` over the floor, under a head at `head`: a half
 * circle where there is room, else a segment as tall as there is (at least a fifth of its span), in cm; 0 no arch. */
static uint8_t arch_rise(float w, float spring, float head) {
    const float rise = fminf(0.5f * w, head - spring);
    return rise < 0.2f * w ? 0 : (uint8_t)fminf(250.0f, rise * 100.0f + 0.5f);
}

static void front_features(HCtx *c, int ui, int mi, Front f) {
    HGraph *g = c->g;
    HUnit *u = &g->unit[ui];
    const HVol *m = &g->vol[mi];
    const float front = m->z0, W = f.ux1 - f.ux0;
    float dw = c->S->parapet ? 1.1f : rng_range(&c->r, 1.0f, 1.1f);
    if (c->S->doorW0 > 0.0f) dw = fminf(rng_range(&c->r5, c->S->doorW0, c->S->doorW1), W - 1.0f);   /* a style's wide doors (double leaves) */
    if (dw < 1.0f) dw = 1.0f;
    const float margin = rng_range(&c->r, 0.35f, 0.65f);
    float dx = f.side < 0 ? f.ux0 + margin + 0.5f * dw : f.side > 0 ? f.ux1 - margin - 0.5f * dw : 0.5f * (f.ux0 + f.ux1);
    /* the porch: the row's kind, an owner's own one time in seven */
    int kind = c->porch;
    if (c->level < 2 && chance(&c->r, 0.14f)) kind = wpick(&c->r, c->S->porch, HP_COUNT);
    if (c->level >= 2) kind = HP_CANOPY;
    /* a front on the street line has no yard for a porch: its door stands in a reveal (HM5e1) */
    if (c->S->street) kind = HP_RECESSED;
    /* the row's ground floor (HM5e1): a five-foot way or pilotis, the door at its back */
    const int ground = c->level < 2 ? c->ground : -1;
    if (ground >= 0) kind = ground;
    float pw, pd;
    switch (kind) {
    case HP_CANOPY:   pw = dw + rng_range(&c->r, 0.4f, 0.8f); pd = rng_range(&c->r, 0.8f, 1.1f); break;
    case HP_RECESSED: pw = dw + rng_range(&c->r, 0.2f, 0.5f); pd = rng_range(&c->r, 0.9f, 1.5f); break;
    case HP_VERANDA:  pw = W * rng_range(&c->r, 0.7f, 0.95f); pd = rng_range(&c->r, 1.8f, 2.5f); break;
    case HP_ENCLOSED: pw = dw + rng_range(&c->r, 0.8f, 1.4f); pd = rng_range(&c->r, 1.2f, 1.8f); break;
    case HP_ARCADE: case HP_PILOTIS: pw = W; pd = c->groundD; break;
    default:          pw = dw + rng_range(&c->r, 0.8f, 1.6f); pd = rng_range(&c->r, 1.2f, 2.0f); break;
    }
    /* a street-line front's door reveal: shallower (0.3-0.7 m) */
    if (c->S->street && kind == HP_RECESSED) { pd = rng_range(&c->r5, 0.3f, 0.7f); pw = dw + rng_range(&c->r5, 0.25f, 0.5f); }
    /* a small front yard (HM2b): the porch the yard has room for, else a canopy over the door */
    const float room = front - c->Y->proj;
    if (!home_porch_recessed(kind) && kind != HP_CANOPY && pd > room) {
        const float least = kind == HP_VERANDA ? 1.5f : 1.2f;
        if (room >= least) pd = room;
        else { kind = HP_CANOPY; pw = dw + rng_range(&c->r, 0.4f, 0.8f); pd = rng_range(&c->r, 0.8f, 1.1f); }
    }
    if (kind == HP_CANOPY && pd > room) pd = fmaxf(0.6f, room);
    if (pw > W) pw = W;
    float px0 = kind == HP_VERANDA ? 0.5f * (f.ux0 + f.ux1) - 0.5f * pw : dx - 0.5f * pw;
    if (kind == HP_ARCADE) {
        /* the colonnade's piers: half a shared one at each party wall, a whole one at a row's end */
        const float pl = f.ux0 > m->x0 + 0.05f ? ARCADE_PIER : 2.0f * ARCADE_PIER, pr = f.ux1 < m->x1 - 0.05f ? ARCADE_PIER : 2.0f * ARCADE_PIER;
        px0 = f.ux0 + pl; pw = W - pl - pr;
    } else if (kind == HP_PILOTIS) px0 = f.ux0;
    if (px0 < f.ux0) px0 = f.ux0;
    if (px0 + pw > f.ux1) px0 = f.ux1 - pw;
    if (kind == HP_VERANDA && (dx - 0.5f * dw < px0 || dx + 0.5f * dw > px0 + pw)) dx = px0 + 0.5f * pw;   /* the door under its veranda */
    if (home_porch_recessed(kind) && (dx - 0.5f * dw < px0 + 0.1f || dx + 0.5f * dw > px0 + pw - 0.1f)) dx = px0 + 0.5f * pw;   /* within its reveal */
    const int recessed = home_porch_recessed(kind);
    const float pz0 = recessed ? front : front - pd, pz1 = recessed ? front + pd : front;
    const int pi = add_vol(g, HV_PORCH, ui, mi, px0, pz0, px0 + pw, pz1, 1, HR_LEAN);
    if (pi < 0) return;
    HVol *p = &g->vol[pi];
    p->kind = (uint8_t)kind;
    /* an arched door (HM5e1: the courtyard house's, the villa's, the colonial's): it springs at about 2 m, its reveal's
     * arch over it, both under the storey's head */
    float spring = 2.1f;
    uint8_t doorArch = 0;
    if (c->arched && kind == HP_RECESSED) {
        spring = rng_range(&c->r5, 1.95f, 2.2f);
        const float head = g->storeyH - 0.3f;
        p->arch = arch_rise(pw, spring, head);
        doorArch = p->arch ? arch_rise(dw, spring, head - 0.08f) : 0;
        if (!doorArch) p->arch = 0;
        if (!p->arch) spring = 2.1f;
    }
    if (kind == HP_ARCADE) { set_flat(p); p->eaves = g->plinth + g->storeyH - 0.35f; }
    else if (kind == HP_PILOTIS) { set_flat(p); p->eaves = g->plinth + g->storeyH - 0.25f; }
    else if (recessed) { set_flat(p); p->eaves = p->arch ? g->plinth + spring + 0.01f * p->arch : g->plinth + g->storeyH - 0.3f; }
    else if (kind == HP_OPEN) { p->roof = HR_GABLE; p->ridgeX = 0; p->pitch = fmaxf(c->pitch, c->S->minPitch); p->eaves = g->plinth + rng_range(&c->r, 2.3f, 2.6f); }
    else {
        lean_against(p, &g->vol[mi], fmaxf(c->S->minPitch, c->pitch - 5.0f));
        p->eaves = g->plinth + (kind == HP_CANOPY ? 2.45f : rng_range(&c->r, 2.3f, 2.6f));
    }
    if (kind == HP_CANOPY) p->y0 = g->plinth + 2.25f;   /* on brackets over the door: supported_by its wall */
    home_vol_top(p);
    /* the door, in the main's front wall (at the back of a recessed porch) */
    if (g->ndoor < HG_MAX_DOOR) {
        HDoor *d = &g->door[g->ndoor];
        memset(d, 0, sizeof(*d));
        d->unit = (int8_t)ui; d->vol = (int8_t)mi; d->path = -1; d->side = HSIDE_S;
        d->x = dx; d->z = pz1; d->width = dw; d->height = doorArch ? spring : 2.1f; d->arch = doorArch;
        u->door = (int8_t)g->ndoor++;
    }
    /* a shop front (HM5e1): in the ground storey beside the door, at the back of a five-foot way, or in the front wall
     * clear of the door's reveal */
    if (c->level < 2 && kind != HP_PILOTIS && chance(&c->r5, c->S->shop)) {
        const int arcade = kind == HP_ARCADE;
        const float a0 = arcade ? px0 + 0.3f : f.ux0 + 0.35f, a1 = arcade ? px0 + pw - 0.3f : f.ux1 - 0.35f;
        const float dl = arcade ? dx - 0.5f * dw - 0.3f : fminf(dx - 0.5f * dw, px0) - 0.3f;    /* the door's (or its reveal's) edges */
        const float dr = arcade ? dx + 0.5f * dw + 0.3f : fmaxf(dx + 0.5f * dw, px0 + pw) + 0.3f;
        const float s0 = a1 - dr >= dl - a0 ? dr : a0, s1 = a1 - dr >= dl - a0 ? a1 : dl;
        if (s1 - s0 >= 1.8f) { u->shopX0 = s0; u->shopX1 = s1; }
        else home_repair(g, c->log, "shop", "unit %d: %.1f m beside its door, no shop front", ui, s1 - s0);
    }
    /* the bay, on the other side of the door */
    if (!c->bay || c->level >= 2 || kind == HP_VERANDA || ground >= 0 || u->shopX1 > u->shopX0) return;
    float b0, b1;
    const int left = f.side > 0 || (f.side == 0 && chance(&c->r, 0.5f));
    if (left) { b0 = f.ux0 + 0.35f; b1 = px0 - 0.4f; }
    else      { b0 = px0 + pw + 0.4f; b1 = f.ux1 - 0.35f; }
    float bw = rng_range(&c->r, 1.8f, 2.7f);
    const float face0 = rng_range(&c->r, CANT_FACE0, CANT_FACE1);
    if (b1 - b0 < 1.8f) { home_repair(g, c->log, "bay", "unit %d: no room beside its door (%.1f m), no bay", ui, b1 - b0); return; }
    if (bw > b1 - b0) bw = b1 - b0;
    const float bc = 0.5f * (b0 + b1);
    float proj = fminf(rng_range(&c->r, 0.6f, 0.9f), front - c->Y->proj);
    if (proj < 0.45f) { home_repair(g, c->log, "bay", "unit %d: a %.1f m front yard, no bay", ui, front); return; }
    /* a canted bay is set out from its front light (CANT_FACE0-1), its sides at 45 degrees as deep as it projects:
     * where the room beside the door can't hold that, it projects less, then narrows its light to CANT_FACE_MIN,
     * and below that it is built square (the user, 2026-11-17: they came to a point) */
    int canted = c->canted;
    if (canted) {
        const float room = b1 - b0;
        float face = face0;
        if (face + 2.0f * proj > room) proj = fmaxf(0.45f, 0.5f * (room - face));
        if (face + 2.0f * proj > room) face = room - 2.0f * proj;
        if (face < CANT_FACE_MIN) {
            canted = 0;
            home_repair(g, c->log, "bay", "unit %d: %.1f m beside its door, too narrow for a canted bay: square", ui, room);
        } else bw = face + 2.0f * proj;
    }
    const int bi = add_vol(g, HV_BAY, ui, mi, bc - 0.5f * bw, front - proj, bc + 0.5f * bw, front, 1, HR_LEAN);
    if (bi < 0) return;
    HVol *b = &g->vol[bi];
    b->kind = (uint8_t)(canted ? HBAY_CANTED : HBAY_SQUARE);
    if (c->S->bayFull && m->storeys >= 2) {
        /* the brownstone's (HM5e1): every storey, its flat top under the main's cornice */
        b->storeys = m->storeys;
        set_flat(b);
        b->eaves = m->eaves - 0.35f;
        home_vol_top(b);
        return;
    }
    b->storeys = (uint8_t)(c->bay2 && m->storeys >= 2 ? 2 : 1);
    lean_against(b, m, fmaxf(c->S->minPitch, c->pitch));
    /* its roof tucked under the main's eaves */
    b->eaves = fminf(g->plinth + b->storeys * g->storeyH - 0.3f, m->eaves - 0.05f - tanf(b->pitch * 0.0174532925f) * proj);
    home_vol_top(b);
}

/* --- the parts behind and beside ------------------------------------------- */

static void add_extension(HCtx *c, int ui, int mi, float x0, float x1) {
    HGraph *g = c->g;
    const HVol *m = &g->vol[mi];
    const float d = fminf(rng_range(&c->r, 2.5f, 5.0f), g->unit[ui].depth - c->Y->rearMin - m->z1);   /* what the rear yard spares */
    const int flat = chance(&c->r, 0.5f);
    if (d < 2.0f) return;
    const int ei = add_vol(g, HV_EXTENSION, ui, mi, x0, m->z1, x1, m->z1 + d, 1, HR_LEAN);
    if (ei < 0) return;
    HVol *e = &g->vol[ei];
    if (flat) { set_flat(e); e->eaves = g->plinth + g->storeyH - 0.15f; }
    else { lean_against(e, m, fmaxf(c->S->minPitch, c->pitch - 5.0f)); e->eaves = g->plinth + 0.85f * g->storeyH; }
    home_vol_top(e);
}

static void add_outrigger(HCtx *c, int ui, int mi, Front f) {
    HGraph *g = c->g;
    const HVol *m = &g->vol[mi];
    float w = rng_range(&c->r, 2.4f, 3.4f);
    const float d = fminf(rng_range(&c->r, 3.0f, 7.0f), g->unit[ui].depth - c->Y->rearMin - m->z1);   /* what the rear yard spares */
    const int lower = chance(&c->r, 0.5f);
    if (w > f.ux1 - f.ux0 - 1.0f) w = f.ux1 - f.ux0 - 1.0f;   /* a side return of a metre at least */
    if (w < 2.0f || d < 2.0f) return;
    /* against the party wall on the door's side: a pair of units' outriggers back to back */
    const float x0 = f.side > 0 ? f.ux1 - w : f.ux0;
    const int oi = add_vol(g, HV_OUTRIGGER, ui, mi, x0, m->z1, x0 + w, m->z1 + d, m->storeys > 1 && lower ? m->storeys - 1 : m->storeys, HR_GABLE);
    if (oi < 0) return;
    HVol *o = &g->vol[oi];
    o->pitch = fmaxf(c->S->minPitch, c->pitch);
    o->overhang = 0.5f * c->eave;
    if (c->roof == HR_FLAT) set_flat(o);
    else if (!c->outrigGable) lean_against(o, m, o->pitch);
    o->eaves = g->plinth + o->storeys * g->storeyH - 0.2f;
    home_vol_top(o);
}

/* A garage wing on the side of the main: set back from its front, a lean-to or flat roof. */
static int add_garage(HCtx *c, int ui, int mi, float x0, float x1) {
    HGraph *g = c->g;
    const HVol *m = &g->vol[mi];
    const float set = rng_range(&c->r, 0.5f, 2.0f), d = rng_range(&c->r, 5.5f, 6.5f);
    const int flat = chance(&c->r, 0.4f);
    const int gi = add_vol(g, HV_WING, ui, mi, x0, m->z0 + set, x1, m->z0 + set + d, 1, HR_LEAN);
    if (gi < 0) return -1;
    HVol *w = &g->vol[gi];
    w->kind = HWING_GARAGE;
    if (flat) { set_flat(w); w->eaves = 2.7f; }
    else { lean_against(w, m, fmaxf(c->S->minPitch, c->pitch - 5.0f)); w->eaves = 2.6f; }
    home_vol_top(w);
    return gi;
}

/* --- chimneys (section 13.4: on the ridge, at a gable end or on a party wall) ------- */

/* inside: kept within the volume's walls (all but a stack standing outside its gable) */
static void add_chimney(HCtx *c, int vi, int ui, int on, float x, float z, int pots, int inside) {
    HGraph *g = c->g;
    if (g->nchim >= HG_MAX_CHIM) return;
    for (int i = 0; i < g->nchim; ++i)   /* one stack a place: a second where the first stands is not built */
        if (fabsf(g->chim[i].x - x) < 1.6f && fabsf(g->chim[i].z - z) < 1.6f) return;
    const HVol *v = &g->vol[vi];
    HChimney *k = &g->chim[g->nchim++];
    memset(k, 0, sizeof(*k));
    k->vol = (int8_t)vi; k->unit = (int8_t)ui; k->on = (uint8_t)on; k->pots = (uint8_t)pots;
    const float w = rng_range(&c->r, 0.6f, 0.6f + 0.15f * pots), d = rng_range(&c->r, 0.45f, 0.6f);
    k->w = v->ridgeX ? w : d;   /* x extent: along a ridge running in x */
    k->d = v->ridgeX ? d : w;
    if (inside) {
        x = fmaxf(v->x0 + 0.5f * k->w + 0.05f, fminf(v->x1 - 0.5f * k->w - 0.05f, x));
        z = fmaxf(v->z0 + 0.5f * k->d + 0.05f, fminf(v->z1 - 0.5f * k->d - 0.05f, z));
    }
    k->x = x; k->z = z;
    k->top = v->top + rng_range(&c->r, 0.9f, 1.5f);
}

static float ridge_line(const HVol *v) { return v->ridgeX ? 0.5f * (v->z0 + v->z1) : 0.5f * (v->x0 + v->x1); }

/* A detached house's stacks: on its ridge, or at its gable ends (inside the wall, or a stack
 * outside it, the craftsman's). */
static void chimneys_detached(HCtx *c, int mi) {
    const HVol *m = &c->g->vol[mi];
    if (m->roof == HR_FLAT || c->level >= 2) return;
    const float e = c->S->chimney;
    int n = (int)e;
    if (chance(&c->r, e - (float)n)) n++;
    if (n > 2) n = 2;
    const float L0 = m->ridgeX ? m->x0 : m->z0, L1 = m->ridgeX ? m->x1 : m->z1;
    const float half = 0.5f * (m->ridgeX ? m->z1 - m->z0 : m->x1 - m->x0);
    int end = chance(&c->r, 0.5f);
    for (int k = 0; k < n; ++k, end = !end) {
        const float pos = rng_range(&c->r, 0.2f, 0.4f);
        if (m->roof == HR_GABLE && chance(&c->r, 0.6f)) {
            /* a craftsman's stack stands outside a side gable */
            const int outside = c->g->style == HS_CRAFTSMAN && m->ridgeX;
            const float out = outside ? 0.3f : -0.35f;
            const float a = end ? L1 + out : L0 - out, r = ridge_line(m);
            add_chimney(c, mi, 0, HC_GABLE, m->ridgeX ? a : r, m->ridgeX ? r : a, 2, !outside);
        } else {
            /* on the ridge: a hip's runs between its hips */
            const float h0 = m->roof == HR_HIP ? L0 + half : L0, h1 = m->roof == HR_HIP ? L1 - half : L1;
            const float a = h1 > h0 ? h0 + (end ? 1.0f - pos : pos) * (h1 - h0) : 0.5f * (L0 + L1), r = ridge_line(m);
            add_chimney(c, mi, 0, HC_RIDGE, m->ridgeX ? a : r, m->ridgeX ? r : a, 2, 1);
        }
    }
}

/* --- the archetypes --------------------------------------------------------- */

/* A clearance: the region's least and up to 60% over it. */
static float clear_draw(HCtx *c, float min) { return min + rng_range(&c->r, 0.0f, 0.6f * min); }

static int build_detached(HCtx *c) {
    HGraph *g = c->g;
    HUnit *u = &g->unit[0];
    const float W = u->x1 - u->x0, SIDE = c->Y->side;
    int garage = c->garage, gside = chance(&c->r, 0.5f);
    float cl = clear_draw(c, SIDE), cr = clear_draw(c, SIDE);
    float gw = rng_range(&c->r, 3.0f, 3.6f);
    const float gclr = home_garage_clear(c->Y) + rng_range(&c->r, 0.0f, 0.6f);
    if (chance(&c->r, 0.4f * c->aff)) gw = rng_range(&c->r, 5.4f, 6.0f);   /* a double garage */
    float maxW = c->g->style == HS_RANCH ? 16.0f : 13.0f;
    if (garage) { if (gside) cr = gclr + gw; else cl = gclr + gw; }
    if (garage && W - cl - cr < MIN_MAIN_W + 1.0f) {
        home_repair(g, c->log, "garage", "plot %.1f m wide: no room for a %.1f m garage", W, gw);
        garage = 0;
        cl = clear_draw(c, SIDE); cr = clear_draw(c, SIDE);
    }
    float mw = W - cl - cr;
    if (mw < MIN_MAIN_W) { cl = cr = SIDE; mw = W - cl - cr; }
    float x0 = u->x0 + cl, x1 = u->x1 - cr;
    if (fminf(x1 - x0, maxW) > SPAN_MAX && c->depth > SPAN_MAX) c->depth = SPAN_MAX;
    if (mw > maxW) {   /* a smaller house on a wide plot: set where the slack falls */
        const float slack = mw - maxW, at = garage ? (gside ? slack : 0.0f) : rng_range(&c->r, 0.0f, slack);
        x0 += at; x1 = x0 + maxW;
    }
    const int mi = add_vol(g, HV_MAIN, 0, -1, x0, u->front, x1, u->front + c->depth, c->P[0].storeys, c->roof);
    if (mi < 0) return 0;
    roof_main(c, &g->vol[mi]);
    home_vol_top(&g->vol[mi]);
    /* the door's side: central on the formal styles, else to one side with the bay across */
    const int formal = g->style == HS_GEORGIAN || g->style == HS_COLONIAL || g->style == HS_SPANISH || g->style == HS_MENA_VILLA || g->style == HS_KOREA;
    const float side = rng_float(&c->r);
    Front f = { x0, x1, formal && side < 0.6f ? 0 : side < 0.8f ? -1 : 1 };
    u->mirrored = (uint8_t)(f.side > 0);
    if (garage) {
        const float g0 = gside ? x1 : u->x0 + gclr, g1 = gside ? u->x1 - gclr : x0;
        if (g1 - g0 >= 2.7f) add_garage(c, 0, mi, g0, g1);
    }
    front_features(c, 0, mi, f);
    if (c->level == 0 && chance(&c->r, c->S->extension + 0.2f * c->aff)) {
        const float frac = rng_range(&c->r, 0.5f, 1.0f), ew = frac * (x1 - x0), at = rng_range(&c->r, 0.0f, (x1 - x0) - ew);
        add_extension(c, 0, mi, x0 + at, x0 + at + ew);
    }
    chimneys_detached(c, mi);
    return 1;
}

/* A flat row whose owners built up unevenly (HM5e1: the self-built and RCC sheets): a unit a storey (or two) short of
 * the row's tallest, never under one. The main keeps the tallest; home_unit_vol gives each unit's own. */
static void uneven_storeys(HCtx *c, int mi) {
    HGraph *g = c->g;
    const HVol *m = &g->vol[mi];
    if (c->S->uneven <= 0.0f || m->roof != HR_FLAT || m->storeys < 2 || c->level >= 2) return;
    for (int k = 0; k < g->nunit; ++k) {
        int st = m->storeys;
        if (chance(&c->r5, c->S->uneven)) st--;
        if (st > 1 && chance(&c->r5, 0.3f * c->S->uneven)) st--;
        g->unit[k].storeys = (uint8_t)(st < m->storeys ? st : 0);
    }
}

static int build_semi(HCtx *c) {
    HGraph *g = c->g;
    if (g->nunit != 2) return 0;
    const float party = g->unit[0].x1;
    int garage = c->garage;
    const float open = clear_draw(c, c->Y->semiOpen), gclr = home_garage_clear(c->Y) + rng_range(&c->r, 0.0f, 0.5f), gw = rng_range(&c->r, 2.8f, 3.4f);
    float cl = garage ? gclr + gw : open;
    const float half = fminf(party - g->unit[0].x0, g->unit[1].x1 - party);
    if (garage && half - cl < MIN_MAIN_W) {
        home_repair(g, c->log, "garage", "halves %.1f m wide: no room for a %.1f m garage", half, gw);
        garage = 0;
        cl = open;
    }
    if (half - cl < MIN_MAIN_W) cl = c->Y->semiOpen;
    const float x0 = party - fminf(half - cl, 7.5f), x1 = party + (party - x0);   /* mirrored: the pair is symmetric about its party wall */
    const int mi = add_vol(g, HV_PAIR, HG_ROW_WIDE, -1, x0, g->unit[0].front, x1, g->unit[0].front + c->depth, c->P[0].storeys, c->roof);
    if (mi < 0) return 0;
    roof_main(c, &g->vol[mi]);
    home_vol_top(&g->vol[mi]);
    uneven_storeys(c, mi);
    /* doors by the party wall (handed) or at the outer sides, the pair's choice */
    const int byParty = chance(&c->r, 0.5f);
    for (int k = 0; k < 2; ++k) {
        g->unit[k].mirrored = (uint8_t)k;
        Front f = { k ? party : x0, k ? x1 : party, 0 };
        f.side = byParty ? (k ? -1 : 1) : (k ? 1 : -1);
        if (garage) {
            const float g0 = k ? x1 : g->unit[0].x0 + gclr, g1 = k ? g->unit[1].x1 - gclr : x0;
            if (g1 - g0 >= 2.7f) add_garage(c, k, mi, g0, g1);
        }
        front_features(c, k, mi, f);
        if (c->level == 0 && chance(&c->r, c->S->extension + 0.2f * c->aff)) {
            const float w = rng_range(&c->r, 0.6f, 1.0f) * (f.ux1 - f.ux0);
            add_extension(c, k, mi, k ? party : party - w, k ? party + w : party);
        }
    }
    HVol *m = &g->vol[mi];
    if (g->nparty < HG_MAX_PARTY) {
        HParty *p = &g->party[g->nparty++];
        p->a = 0; p->b = 1; p->vol = (int8_t)mi; p->x = party; p->z0 = m->z0; p->z1 = m->z1; p->top = m->top;
    }
    /* stacks: one shared on the party wall at the ridge, or one at each outer gable end */
    if (m->roof != HR_FLAT && c->level < 2) {
        const float x = rng_float(&c->r);
        if (x < 0.6f) add_chimney(c, mi, 0, HC_PARTY, party, m->ridgeX ? ridge_line(m) : 0.5f * (m->z0 + m->z1), 4, 1);
        else if (x < 0.85f && m->roof == HR_GABLE && m->ridgeX) {
            add_chimney(c, mi, 0, HC_GABLE, m->x0, ridge_line(m), 2, 1);
            add_chimney(c, mi, 1, HC_GABLE, m->x1, ridge_line(m), 2, 1);
        }
    }
    return 1;
}

static int build_terrace(HCtx *c) {
    HGraph *g = c->g;
    const int n = g->nunit;
    if (n < 2) return 0;
    const int mi = add_vol(g, HV_ROW, HG_ROW_WIDE, -1, g->unit[0].x0, g->unit[0].front, g->unit[n - 1].x1, g->unit[0].front + c->depth, c->P[0].storeys, c->roof);
    if (mi < 0) return 0;
    roof_main(c, &g->vol[mi]);
    home_vol_top(&g->vol[mi]);
    uneven_storeys(c, mi);
    for (int k = 0; k < n; ++k) {
        HUnit *u = &g->unit[k];
        /* handed rows pair their doors (and outriggers) on alternate party walls */
        u->mirrored = (uint8_t)(c->S->handed ? (k & 1) == 0 : 0);
        Front f = { u->x0, u->x1, u->mirrored ? 1 : -1 };
        front_features(c, k, mi, f);
        if (c->outrig && c->level == 0) add_outrigger(c, k, mi, f);
        else if (c->level == 0 && chance(&c->r, c->S->extension + 0.15f * c->aff)) {
            const float w = rng_range(&c->r, 0.6f, 1.0f) * (u->x1 - u->x0);
            add_extension(c, k, mi, f.side > 0 ? u->x1 - w : u->x0, f.side > 0 ? u->x1 : u->x0 + w);
        }
    }
    HVol *m = &g->vol[mi];
    for (int k = 0; k + 1 < n && g->nparty < HG_MAX_PARTY; ++k) {
        HParty *p = &g->party[g->nparty++];
        p->a = (int8_t)k; p->b = (int8_t)(k + 1); p->vol = (int8_t)mi; p->x = g->unit[k].x1; p->z0 = m->z0; p->z1 = m->z1; p->top = m->top;
    }
    /* stacks on the party walls at the ridge, each serving the two units beside it; at the gable ends inside the wall */
    if (m->roof != HR_FLAT && c->level < 2 && c->S->chimney > 0.0f) {   /* none where the style builds none (HM5e1) */
        const float rate = fminf(1.0f, c->S->chimney * 0.5f + 0.25f);
        for (int k = 0; k + 1 < n; ++k)
            if (chance(&c->r, rate)) add_chimney(c, mi, k, HC_PARTY, g->unit[k].x1, ridge_line(m), 4, 1);
        if (m->roof == HR_GABLE && m->ridgeX) {
            if (chance(&c->r, rate * 0.5f)) add_chimney(c, mi, 0, HC_GABLE, m->x0, ridge_line(m), 2, 1);
            if (chance(&c->r, rate * 0.5f)) add_chimney(c, mi, n - 1, HC_GABLE, m->x1, ridge_line(m), 2, 1);
        }
    }
    return 1;
}

/* --- the garden ------------------------------------------------------------- */

static int add_site(HGraph *g, int kind, int unit, float x0, float z0, float x1, float z1) {
    if (g->nsite >= HG_MAX_SITE) return -1;
    HSite *s = &g->site[g->nsite];
    s->kind = (uint8_t)kind; s->unit = (int8_t)unit; s->door = -1;
    s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1;
    return g->nsite++;
}

static void add_bound(HGraph *g, int kind, int role, int unit, float x0, float z0, float x1, float z1, float h) {
    if (kind == HB_NONE || g->nbound >= HG_MAX_BOUND || (fabsf(x1 - x0) < 0.05f && fabsf(z1 - z0) < 0.05f)) return;
    HBound *b = &g->bound[g->nbound++];
    b->kind = (uint8_t)kind; b->role = (uint8_t)role; b->unit = (int8_t)unit;
    b->x0 = x0; b->z0 = z0; b->x1 = x1; b->z1 = z1; b->h = h;
}

/* How far back the volumes standing on the line x reach (from z0): where a fence along it can start. */
static float back_on_line(const HGraph *g, float x, float from) {
    float z = from;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role == HV_PORCH && v->kind == HP_CANOPY) continue;
        if (x >= v->x0 - 0.05f && x <= v->x1 + 0.05f && v->z1 > z) z = v->z1;
    }
    return z;
}

/* The rear wall of a unit: the back of everything standing on its plot. */
float home_unit_rear(const HGraph *g, int ui) {
    const HUnit *u = &g->unit[ui];
    float z = 0.0f;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->unit != ui && v->unit != HG_ROW_WIDE) continue;
        if (v->x1 <= u->x0 + 0.01f || v->x0 >= u->x1 - 0.01f) continue;
        if (v->z1 > z) z = v->z1;
    }
    return z;
}

static int rect_clear(const HGraph *g, float x0, float z0, float x1, float z1, float pad) {
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (x1 + pad > v->x0 && x0 - pad < v->x1 && z1 + pad > v->z0 && z0 - pad < v->z1) return 0;
    }
    for (int i = 0; i < g->nsite; ++i) {
        const HSite *s = &g->site[i];
        if (s->kind == HE_FRONT || s->kind == HE_REAR) continue;
        if (x1 > s->x0 && x0 < s->x1 && z1 > s->z0 && z0 < s->z1) return 0;
    }
    return 1;
}

static void lay_site(HCtx *c) {
    HGraph *g = c->g;
    const int mi = main_of(g);
    if (mi < 0) return;
    const HVol *m = &g->vol[mi];
    for (int k = 0; k < g->nunit; ++k) {
        HUnit *u = &g->unit[k];
        const float rear = home_unit_rear(g, k);
        /* the yards' surfaces (HM2b): no lawns in a city; bare earth where poor, beds and pots where better off;
         * the back mostly as the front */
        float sw[HY_COUNT];
        for (int s = 0; s < HY_COUNT; ++s) sw[s] = c->Y->surface[s];
        sw[HY_SOIL] *= 1.6f - 1.2f * c->aff;
        sw[HY_PLANTED] *= 0.6f + 0.8f * c->aff;
        const int sf = wpick(&c->r, sw, HY_COUNT), sr = chance(&c->r, 0.7f) ? sf : wpick(&c->r, sw, HY_COUNT);
        int yi = add_site(g, HE_FRONT, k, u->x0, 0.0f, u->x1, m->z0);
        if (yi >= 0) g->site[yi].surface = (uint8_t)sf;
        yi = add_site(g, HE_REAR, k, u->x0, rear, u->x1, u->depth);
        if (yi >= 0) g->site[yi].surface = (uint8_t)sr;
        int kind = c->front;
        if (c->level < 2 && chance(&c->r, 0.12f)) kind = wpick(&c->r, c->S->bound, HB_COUNT);   /* an owner's own */
        const float h = kind == HB_WALL ? c->wallH : kind == HB_HEDGE ? rng_range(&c->r, 0.8f, 1.8f) : kind == HB_FENCE ? rng_range(&c->r, 0.9f, 1.6f) : 0.0f;
        /* the gaps: the gate on the path, the drive's */
        float gap0[3], gap1[3];
        int ng = 0;
        if (u->door >= 0) {
            HDoor *d = &g->door[u->door];
            /* the path runs from the door (its porch's step) to the street */
            float zEnd = d->z;
            for (int i = 0; i < g->nvol; ++i) {
                const HVol *p = &g->vol[i];
                if (p->role != HV_PORCH || p->unit != k) continue;
                zEnd = p->kind == HP_CANOPY || home_porch_recessed(p->kind) ? m->z0 : p->z0;
            }
            const float pw = rng_range(&c->r, 0.9f, 1.2f);
            float px0 = d->x - 0.5f * pw, px1 = d->x + 0.5f * pw;
            if (px0 < u->x0) { px1 += u->x0 - px0; px0 = u->x0; }
            if (px1 > u->x1) { px0 -= px1 - u->x1; px1 = u->x1; }
            const int si = add_site(g, HE_PATH, k, px0, 0.0f, px1, zEnd);
            if (si >= 0) { g->site[si].door = u->door; d->path = (int8_t)si; }
            if (kind != HB_NONE && g->ngate < HG_MAX_GATE) {
                const float gw = fminf(1.2f, pw + 0.2f), gx = 0.5f * (px0 + px1);   /* centred on the path */
                HGate *gt = &g->gate[g->ngate++];
                gt->unit = (int8_t)k; gt->drive = 0; gt->x = gx; gt->w = gw;
                gap0[ng] = gx - 0.5f * gw; gap1[ng++] = gx + 0.5f * gw;
            }
        }
        for (int i = 0; i < g->nvol; ++i) {
            const HVol *w = &g->vol[i];
            if (w->role != HV_WING || w->kind != HWING_GARAGE || w->unit != k) continue;
            add_site(g, HE_DRIVE, k, w->x0, 0.0f, w->x1, w->z0);
            if (w->z0 >= 5.2f && chance(&c->r, 0.55f)) {
                const float cx = 0.5f * (w->x0 + w->x1);
                add_site(g, HE_CAR, k, cx - 0.95f, w->z0 - 4.9f, cx + 0.95f, w->z0 - 0.3f);
            }
            if (kind != HB_NONE && g->ngate < HG_MAX_GATE && ng < 3) {
                HGate *gt = &g->gate[g->ngate++];
                gt->unit = (int8_t)k; gt->drive = 1; gt->x = 0.5f * (w->x0 + w->x1); gt->w = w->x1 - w->x0;
                gap0[ng] = w->x0; gap1[ng++] = w->x1;
            }
        }
        /* a car pad (HM2b): the row parks in its front yard, a paved strip beside the path, clear of porches and bays */
        if (c->pad && m->z0 >= HOME_PAD0 - HEPS && ng < 3) {
            const float pw = rng_range(&c->r, 2.6f, 3.0f);
            const int first = chance(&c->r, 0.5f);
            for (int t = 0; t < 2; ++t) {
                const int right = (t == 0) == first;
                const float x0 = right ? u->x1 - pw : u->x0, x1 = x0 + pw;
                if (x0 < u->x0 - HEPS || x1 > u->x1 + HEPS || !rect_clear(g, x0, 0.0f, x1, m->z0 - HEPS, 0.0f)) continue;
                add_site(g, HE_DRIVE, k, x0, 0.0f, x1, m->z0);
                if (chance(&c->r, 0.7f)) {
                    const float cx = 0.5f * (x0 + x1), cz = 0.5f * m->z0;
                    add_site(g, HE_CAR, k, cx - 0.95f, cz - 2.3f, cx + 0.95f, cz + 2.3f);
                }
                if (kind != HB_NONE && g->ngate < HG_MAX_GATE) {
                    HGate *gt = &g->gate[g->ngate++];
                    gt->unit = (int8_t)k; gt->drive = 1; gt->x = 0.5f * (x0 + x1); gt->w = pw;
                    gap0[ng] = x0; gap1[ng++] = x1;
                }
                break;
            }
        }
        /* the front boundary with its gaps (sorted), and the side lines */
        for (int a = 1; a < ng; ++a)
            for (int b = a; b > 0 && gap0[b] < gap0[b - 1]; --b) {
                float t = gap0[b]; gap0[b] = gap0[b - 1]; gap0[b - 1] = t;
                t = gap1[b]; gap1[b] = gap1[b - 1]; gap1[b - 1] = t;
            }
        float x = u->x0;
        for (int a = 0; a < ng; ++a) {
            if (gap0[a] > x) add_bound(g, kind, HBR_FRONT, k, x, 0.0f, gap0[a], 0.0f, h);
            if (gap1[a] > x) x = gap1[a];
        }
        if (u->x1 > x) add_bound(g, kind, HBR_FRONT, k, x, 0.0f, u->x1, 0.0f, h);
        /* the side lines: low between the front gardens, a fence from the house back */
        const int sideKind = c->S->wallH0 > 0.0f && c->front == HB_WALL ? HB_WALL : HB_FENCE;
        const float sideH = sideKind == HB_WALL ? c->wallH : rng_range(&c->r, 1.5f, 1.9f);
        for (int e = 0; e < 2; ++e) {
            if (e == 1 && k + 1 < g->nunit) continue;   /* the next unit draws the line it shares */
            const float lx = e ? u->x1 : u->x0;
            const int role = (e == 0 && k > 0) ? HBR_PARTY : HBR_SIDE;
            const float zs = back_on_line(g, lx, m->z0);
            add_bound(g, kind, role, k, lx, 0.0f, lx, fminf(m->z0, zs), h);
            if (zs < u->depth - 0.5f) add_bound(g, sideKind, role, k, lx, zs, lx, u->depth, sideH);
        }
        add_bound(g, sideKind, HBR_REAR, k, u->x0, u->depth, u->x1, u->depth, sideH);
        /* a shed at the back of a long garden, trees where they stand clear */
        const float rd = u->depth - rear;
        if (c->level < 2 && rd >= 9.0f && chance(&c->r, 0.3f + 0.3f * c->aff)) {
            const float sw = rng_range(&c->r, 1.8f, 2.6f), sd = rng_range(&c->r, 2.0f, 3.0f);
            const int right = chance(&c->r, 0.5f);
            if (sw + 0.8f < u->x1 - u->x0) {
                const float sx0 = right ? u->x1 - 0.4f - sw : u->x0 + 0.4f;
                if (rect_clear(g, sx0, u->depth - 0.4f - sd, sx0 + sw, u->depth - 0.4f, 1.0f))
                    add_site(g, HE_SHED, k, sx0, u->depth - 0.4f - sd, sx0 + sw, u->depth - 0.4f);
            }
        }
        for (int t = 0; t < 2 && c->level < 2; ++t) {
            const int back = t == 0;
            const float room = back ? rd : m->z0;
            if (room < (back ? 9.0f : 5.5f)) continue;
            float r = rng_range(&c->r, 1.5f, back ? 3.0f : 2.0f);
            const float cx = rng_range(&c->r, u->x0 + 0.3f * (u->x1 - u->x0), u->x0 + 0.7f * (u->x1 - u->x0));
            const float cz = back ? rear + rng_range(&c->r, 0.45f, 0.75f) * rd : rng_range(&c->r, 0.35f, 0.6f) * m->z0;
            const float zlo = back ? rear : 0.0f, zhi = back ? u->depth : m->z0;
            r = fminf(r, fminf(fminf(cx - u->x0, u->x1 - cx), fminf(cz - zlo, zhi - cz)) - 0.2f);
            if (r < 1.0f) continue;
            if (rect_clear(g, cx - r, cz - r, cx + r, cz + r, 1.0f)) add_site(g, HE_TREE, k, cx - r, cz - r, cx + r, cz + r);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The driver (section 13.10)
 * ------------------------------------------------------------------------- */

static void init_graph(HCtx *c, const HomePlot *row, int n) {
    HGraph *g = c->g;
    memset(g, 0, sizeof(*g));
    g->seed = row[0].seed;
    g->arch = row[0].arch; g->style = row[0].style; g->region = row[0].region;
    g->affluence = c->aff;
    g->u0 = row[0].u0;
    g->ox = row[0].ox; g->oz = row[0].oz; g->tx = row[0].tx; g->tz = row[0].tz; g->nx = row[0].nx; g->nz = row[0].nz;
    /* section 13.5: a row at most 48 m long (HM1 cuts none longer): drop units until it fits */
    while (n > 1 && row[n - 1].u1 - row[0].u0 > ROW_MAX + 1e-3f) {
        home_repair(g, c->log, "row", "%.1f m long: unit %d dropped", row[n - 1].u1 - row[0].u0, n);
        n--;
    }
    g->nunit = n > HG_MAX_UNITS ? HG_MAX_UNITS : n;
    for (int k = 0; k < g->nunit; ++k) {
        HUnit *u = &g->unit[k];
        u->x0 = row[k].u0 - g->u0; u->x1 = row[k].u1 - g->u0;
        u->depth = row[0].depth; u->front = row[0].front;
        u->seed = row[k].seed; u->door = -1;
    }
}

/* HM5e1's choices for the row, each style's body to its sheet's panel 1, from their own stream: tall rooms, the
 * ground floor (a five-foot way, pilotis), arched doors; China's one-storey rows are hutong courtyards under grey
 * tile, its taller ones flat concrete blocks. */
static void choose_row_hm5e(HCtx *c) {
    const HStyleDef *S = c->S;
    HGraph *g = c->g;
    if (S->storeyH0 > 0.0f) g->storeyH = rng_range(&c->r5, S->storeyH0, S->storeyH1);
    c->arched = chance(&c->r5, S->arched);
    c->ground = -1;
    c->groundD = 0.0f;
    const float x = rng_float(&c->r5);
    if (x < S->arcade) { c->ground = HP_ARCADE; c->groundD = rng_range(&c->r5, 1.6f, 2.1f); }
    else if (x < S->arcade + S->pilotis && c->depth >= 8.0f) {
        /* the pilotis deep enough for a car (4.6 m and a margin), a core of 2.8 m at least behind for the stair and door */
        c->ground = HP_PILOTIS;
        c->groundD = fminf(6.2f, c->depth - rng_range(&c->r5, 2.8f, 3.6f));
        if (c->groundD < 5.0f) { c->ground = -1; c->groundD = 0.0f; }
    }
    if (g->style == HS_CHINA && c->level < 2) c->roof = c->P[0].storeys <= 1 ? HR_GABLE : HR_FLAT;
}

static void choose_row(HCtx *c) {
    const HStyleDef *S = c->S;
    HGraph *g = c->g;
    g->storeyH = rng_range(&c->r, 2.7f, 3.0f);
    g->plinth = rng_range(&c->r, S->plinth0, S->plinth1);
    c->roof = wpick(&c->r, S->roof, HR_COUNT);
    if (c->roof == HR_LEAN) c->roof = HR_GABLE;
    c->pitch = rng_range(&c->r, S->pitch0, S->pitch1);
    c->eave = rng_range(&c->r, S->eave0, S->eave1);
    c->depth = rng_range(&c->r, S->depth0, S->depth1);
    if (g->arch == HOME_TERRACE) c->depth += rng_range(&c->r, 0.0f, 1.0f);
    c->porch = wpick(&c->r, S->porch, HP_COUNT);
    c->bay = chance(&c->r, S->bay);
    c->bay2 = chance(&c->r, S->bay2);
    c->canted = chance(&c->r, S->canted);
    c->outrig = g->arch == HOME_TERRACE && chance(&c->r, S->outrigger);
    c->outrigGable = chance(&c->r, 0.6f);
    c->garage = g->arch != HOME_TERRACE && chance(&c->r, S->garage + 0.3f * (c->aff - 0.5f));
    c->front = wpick(&c->r, S->bound, HB_COUNT);
    c->wallH = S->wallH0 > 0.0f ? rng_range(&c->r, S->wallH0, S->wallH1) : rng_range(&c->r, 0.6f, 1.2f);
    /* a row that parks in its front yard has no garage wing (HM2b) */
    c->pad = c->P[0].pad;
    if (c->pad) c->garage = 0;
    /* the main no deeper than the plot leaves between the front yard and the rear (HM2b: tight plots) */
    const float room = g->unit[0].depth - g->unit[0].front - c->Y->rearMin;
    if (c->depth > room) c->depth = fmaxf(MIN_DEPTH, room);
    /* level 1 drops the optional parts, level 2 is the plain form (section 13.10) */
    if (c->level >= 1) { c->garage = 0; c->outrig = 0; }
    /* the span (the shorter side) stays within 12 m: a main wider than that is no deeper (section 13.5) */
    if (g->arch != HOME_DETACHED && c->depth > SPAN_MAX) c->depth = SPAN_MAX;
    if (c->level >= 2) {
        c->bay = 0;
        c->roof = S->roof[HR_GABLE] + S->roof[HR_HIP] > 0.0f ? HR_GABLE : HR_FLAT;
        c->depth = fminf(c->depth, 9.0f);
        c->porch = HP_CANOPY;
    }
    choose_row_hm5e(c);
}

/* The simplest valid house (after level 2): the plain main at the least clearances, a canopy and a path. */
static void build_simplest(HCtx *c) {
    HGraph *g = c->g;
    c->level = 3;
    c->r = rng_make(hash_combine(g->seed, 0x5133u));
    c->r5 = rng_make(hash_combine(g->seed, 0x5134u));
    c->ground = -1;
    c->arched = 0;
    g->storeyH = c->S->storeyH0 > 0.0f ? 0.5f * (c->S->storeyH0 + c->S->storeyH1) : 2.85f;
    g->plinth = 0.3f;
    c->roof = c->S->roof[HR_GABLE] + c->S->roof[HR_HIP] > 0.0f ? HR_GABLE : HR_FLAT;
    c->pitch = fmaxf(30.0f, c->S->minPitch);
    c->eave = 0.3f;
    c->porch = HP_CANOPY;
    c->bay = c->garage = c->outrig = 0;
    c->front = HB_NONE;
    const HUnit *u = &g->unit[0];
    c->pad = 0;
    for (int k = 0; k < g->nunit; ++k) g->unit[k].front = fmaxf(c->Y->frontMin, fminf(g->unit[k].front, g->unit[k].depth - c->Y->rearMin - MIN_DEPTH));
    c->depth = fminf(9.0f, u->depth - g->unit[0].front - c->Y->rearMin);
    if (g->arch == HOME_TERRACE) build_terrace(c);
    else if (g->arch == HOME_SEMI && g->nunit == 2) build_semi(c);
    else build_detached(c);
}

void home_repair_all(HGraph *g, int level, BLog *log);

void home_build(const HomePlot *row, int n, float affluence, HGraph *g, BLog *log) { home_build_at(row, n, affluence, 0, g, log); }

void home_build_at(const HomePlot *row, int n, float affluence, int first, HGraph *g, BLog *log) {
    HCtx c;
    memset(&c, 0, sizeof(c));
    c.g = g; c.log = log; c.P = row; c.aff = affluence;
    c.S = home_style_def(row[0].style);
    c.Y = home_yard(row[0].region);
    blog_printf(log, "home %08x: %s, %s (%s), %d plot%s, %.1f m by %.1f, front garden %.1f, %d storeys\n", row[0].seed,
                n > 0 && row[0].arch == HOME_TERRACE ? "terrace" : row[0].arch == HOME_SEMI ? "semi" : "detached",
                home_style_name(row[0].style), plan_region_code(row[0].region), n, n == 1 ? "" : "s",
                n > 0 ? row[n - 1].u1 - row[0].u0 : 0.0f, n > 0 ? row[0].depth : 0.0f, n > 0 ? row[0].front : 0.0f, n > 0 ? row[0].storeys : 0);
    if (n <= 0) { memset(g, 0, sizeof(*g)); g->empty = 1; return; }
    for (int level = first; level < 3; ++level) {
        c.level = level;
        c.r = rng_make(hash_combine(hash_combine(row[0].seed, 0x40E5u), (uint32_t)level));   /* stream(seed, "home", level) */
        c.r5 = rng_make(hash_combine(hash_combine(row[0].seed, 0x45E1u), (uint32_t)level));  /* HM5e1's choices */
        init_graph(&c, row, n);
        choose_row(&c);
        blog_printf(log, " level %d: %s roof at %.0f deg, main %.1f m deep%s%s%s\n", level, home_roof_name(c.roof), c.pitch, c.depth,
                    c.garage ? ", garage" : "", c.bay ? ", bays" : "", c.outrig ? ", outriggers" : "");
        int built = g->arch == HOME_TERRACE ? build_terrace(&c) : g->arch == HOME_SEMI ? build_semi(&c) : build_detached(&c);
        if (!built) { blog_printf(log, "  rejected fit: the proposal does not fit its plots\n"); continue; }
        home_repair_all(g, level, log);
        lay_site(&c);
        if (home_validate(g, log)) {
            g->level = (uint8_t)level;
            g->attempts = level + 1;
            blog_printf(log, " accepted at level %d: %d volumes, %d doors, %d chimneys, %d repairs\n", level, g->nvol, g->ndoor, g->nchim, g->repairs);
            return;
        }
        blog_printf(log, "  reject: retrying at level %d\n", level + 1);
    }
    init_graph(&c, row, n);
    build_simplest(&c);
    home_repair_all(g, 3, log);
    lay_site(&c);
    g->attempts = 4;
    g->level = 3;
    if (home_validate(g, log)) { blog_printf(log, " accepted the simplest: %d volumes\n", g->nvol); return; }
    blog_printf(log, " no house: the plot is left a garden\n");
    const int nu = g->nunit;
    memset(g, 0, sizeof(*g));
    g->seed = row[0].seed; g->arch = row[0].arch; g->style = row[0].style; g->region = row[0].region;
    g->nunit = nu; g->empty = 1; g->attempts = 4; g->level = 3; g->affluence = c.aff;
}

void home_to_world(const HGraph *g, float x, float z, double *wx, double *wz) {
    const double u = (double)x + g->u0;
    *wx = g->ox + u * g->tx + (double)z * g->nx;
    *wz = g->oz + u * g->tz + (double)z * g->nz;
}
