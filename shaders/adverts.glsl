#ifndef ADVERTS_GLSL
#define ADVERTS_GLSL
// Authored adverts (src/core/advert_slots.h, src/render/adverts.c): their table, and
// which a screen shows. Included by city/signs.glsl, which draws them, and by
// clouds.comp, which the screens light. Binding 9 in both sets.

// AdvertGPU (src/render/adverts.h)
struct Advert {
    uvec4 info;     // x frames, yz its frame size (LEDs)
    vec4  play;     // x fps, y brightness, z glow, w its weight (0 unless it loaded)
    vec4  mean;     // rgb its frames' mean colour (linear)
};
layout(std140, set = 0, binding = 9) uniform Adverts {
    Advert advert[ADVERT_TABLE_SIZE];
};

// Whether advert i suits a screen of the given aspect (width / height): its frames'
// within ADVERT_ASPECT_FIT of it.
bool advertFits(int i, float aspect) {
    if (advert[i].play.w <= 0.0) return false;
    float own = float(advert[i].info.y) / float(advert[i].info.z);
    return abs(log(aspect / own)) <= log(1.0 + ADVERT_ASPECT_FIT);
}

// The authored advert a screen of that aspect shows in the slot whose hash is h, or
// -1 for one of the procedural scenes. Its screen time lies along one line: the
// scenes' share first (ADVERT_PLACEHOLDER_WEIGHT), then, by its weight, each advert
// that fits it.
int advertPick(uint h, float aspect) {
    float total = ADVERT_PLACEHOLDER_WEIGHT;
    for (int i = 0; i < ADVERT_COUNT; ++i)
        if (advertFits(i, aspect)) total += advert[i].play.w;
    float x = hfloat(hash_combine(h, 0xAD5u)) * total - ADVERT_PLACEHOLDER_WEIGHT;
    for (int i = 0; i < ADVERT_COUNT && x >= 0.0; ++i)
        if (advertFits(i, aspect)) {
            if (x < advert[i].play.w) return i;
            x -= advert[i].play.w;
        }
    return -1;
}

#endif
