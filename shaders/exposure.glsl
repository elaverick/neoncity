// The auto exposure's buffer (src/render/exposure_gpu.c): the meter's
// histogram, which exposure_meter.comp fills and exposure_adapt.comp reads and
// clears, then the eye's state. The composite reads the state. The constants
// are shared with C (src/core/light_units.h).
#include "light_units.h"

#ifndef METER_ACCESS
#define METER_ACCESS
#endif
layout(set = 0, binding = METER_BINDING, std430) METER_ACCESS buffer Meter {
    uint bins[METER_BINS];
    vec4 state;   // x the adapted EV100 the frame is shown at, y where it is heading, z the metered EV100, w 1 once set
} meter;

// The log2 of the engine's luminance a bin stands for (bin 0: black).
float meterBinLog(uint i) {
    const float step = (METER_LOG_HI - METER_LOG_LO) / float(METER_BINS - 1);
    return i == 0u ? METER_LOG_LO - 2.0 : METER_LOG_LO + (float(i) - 0.5) * step;
}

// The bin a luminance (engine units) falls in.
uint meterBin(float l) {
    if (!(l > exp2(METER_LOG_LO))) return 0u;   // black, or not a number
    float t = (log2(l) - METER_LOG_LO) / (METER_LOG_HI - METER_LOG_LO);
    return uint(clamp(1.0 + t * float(METER_BINS - 1), 1.0, float(METER_BINS - 1)));
}

// The exposure the eye settles on for a metered EV100 (light_units.h: the key).
float meterTarget(float metered) {
    float night = EV100_REF + KEY_NIGHT_SLOPE * (min(metered, KEY_DUSK_EV) - KEY_NIGHT_EV);
    return clamp(night + max(metered - KEY_DUSK_EV, 0.0), EXPOSURE_MIN, EXPOSURE_MAX);
}

#ifdef METER_PASS
// The push constants of both passes (exposure_gpu.c: MeterPush).
layout(push_constant) uniform MeterPush {
    float invPre;     // one over the pre-exposure the scene was written with (common.glsl: EXPOSE)
    float dt;         // seconds since the last frame
    float manualEv;   // --ev: shown at this (flags & 1)
    int   flags;      // 1 manual, 2 no adaptation: straight to the target (a capture, a new frame)
} pc;
#endif
