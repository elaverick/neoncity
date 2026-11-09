/* The world's clock and the sky's ephemerides (src/app/sky_clock.c) against
 * reference values: Meeus's worked examples, a known full and new moon, the
 * total eclipse of 2024 April 8, sunrise in New York and in Moscow on one
 * day, the sun's height at noon on the solstice at Greenwich.
 *
 * sky_test            the checks
 * sky_test --at UTC LAT LON   the sky over a place at an instant
 * sky_test --find     instants in a span when the moon over Los Angeles
 *                     stands near where it used to stand fixed (for
 *                     SKY_CAPTURE_UTC) */
#include "sky_clock.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG (3.14159265358979323846 / 180.0)
static int g_fail;

static void check(bool ok, const char *what, double got, double want) {
    printf("%s %-58s got %.6f want %.6f\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) g_fail++;
}
static void near(const char *what, double got, double want, double tol) { check(fabs(got - want) <= tol, what, got, want); }
static double angdiff(double a, double b) { double d = fmod(a - b + 540.0, 360.0) - 180.0; return fabs(d); }
static void near_angle(const char *what, double got, double want, double tol) { check(angdiff(got, want) <= tol, what, got, want); }

static void up_at(double lat, double lon, double up[3]) {
    up[0] = cos(lat * DEG) * cos(lon * DEG);
    up[1] = cos(lat * DEG) * sin(lon * DEG);
    up[2] = sin(lat * DEG);
}
static double sun_alt(double jd, const double up[3]) {
    SkyState s;
    sky_state(jd, &s);
    double alt, az;
    sky_alt_az(s.sun, up, &alt, &az);
    return alt / DEG;
}
static void moon_alt_az(double jd, const double up[3], double *alt, double *az) {
    SkyState s;
    sky_state(jd, &s);
    double m[3];
    sky_moon_topocentric(&s, up, m);
    sky_alt_az(m, up, alt, az);
    *alt /= DEG; *az /= DEG;
}

/* The first sunrise (the upper limb on the horizon: -0.833 degrees) after jd0 within a day. */
static double sunrise(double jd0, double lat, double lon) {
    double up[3];
    up_at(lat, lon, up);
    const double H = -0.833;
    double step = 1.0 / 288.0;   /* 5 min */
    for (double t = jd0; t < jd0 + 1.0; t += step) {
        if (sun_alt(t, up) < H && sun_alt(t + step, up) >= H) {
            double a = t, b = t + step;
            for (int i = 0; i < 40; ++i) {
                double m = 0.5 * (a + b);
                if (sun_alt(m, up) < H) a = m; else b = m;
            }
            return 0.5 * (a + b);
        }
    }
    return -1.0;
}
static double hours_utc(double jd) { return (jd + 0.5 - floor(jd + 0.5)) * 24.0; }

static void print_sky(double jd, double lat, double lon) {
    double up[3], alt, az;
    up_at(lat, lon, up);
    SkyState s;
    sky_state(jd, &s);
    char when[64];
    sky_format_utc(jd, when, sizeof(when));
    sky_alt_az(s.sun, up, &alt, &az);
    printf("%s at %.3f %.3f: local solar time %.2f h; sun alt %.2f az %.2f", when, lat, lon, sky_local_solar_hours(jd, lon), alt / DEG, az / DEG);
    double m[3];
    sky_moon_topocentric(&s, up, m);
    sky_alt_az(m, up, &alt, &az);
    printf("; moon alt %.2f az %.2f, %.0f%% lit, %.2f of full\n", alt / DEG, az / DEG, s.moonLit * 100.0, s.moonBright);
}

/* Instants (every 10 min over two years from 2026-11-01) when the moon over
 * Los Angeles is within 3 degrees of altitude 25, azimuth 70.6 (the old
 * fixed moon), at least 80% lit, with the sun 18 degrees down. */
static void find(void) {
    double up[3];
    up_at(34.05, -118.25, up);
    double jd0 = sky_jd(2026, 11, 1, 0, 0, 0.0), lastHit = 0.0;
    for (double t = jd0; t < jd0 + 730.0; t += 1.0 / 144.0) {
        double alt, az;
        moon_alt_az(t, up, &alt, &az);
        if (fabs(alt - 25.0) > 3.0 || angdiff(az, 70.6) > 3.0) continue;
        SkyState s;
        sky_state(t, &s);
        if (s.moonLit < 0.8 || sun_alt(t, up) > -18.0) continue;
        if (t - lastHit < 0.5) continue;
        lastHit = t;
        print_sky(t, 34.05, -118.25);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--find") == 0) { find(); return 0; }
    if (argc > 4 && strcmp(argv[1], "--at") == 0) {
        double jd;
        if (!sky_parse_utc(argv[2], &jd)) { fprintf(stderr, "--at %s: not a UTC instant\n", argv[2]); return 1; }
        print_sky(jd, atof(argv[3]), atof(argv[4]));
        return 0;
    }

    /* the calendar (Meeus 7.a, 7.b) and the clock's text */
    near("JD 2000-01-01 12:00", sky_jd(2000, 1, 1, 12, 0, 0.0), 2451545.0, 1e-9);
    near("JD 1957-10-04.81 (Meeus 7.a)", sky_jd(1957, 10, 4, 19, 26, 24.0), 2436116.31, 1e-6);
    near("JD 1600-01-01 0h (Meeus 7)", sky_jd(1600, 1, 1, 0, 0, 0.0), 2305447.5, 1e-9);
    double jd;
    check(sky_parse_utc("2026-11-09T22:00", &jd) && fabs(jd - sky_jd(2026, 11, 9, 22, 0, 0.0)) < 1e-9, "parse 2026-11-09T22:00", jd, sky_jd(2026, 11, 9, 22, 0, 0.0));
    check(sky_parse_utc("2026-11-09 22:00:30Z", &jd) && fabs(jd - sky_jd(2026, 11, 9, 22, 0, 30.0)) < 1e-9, "parse 2026-11-09 22:00:30Z", jd, sky_jd(2026, 11, 9, 22, 0, 30.0));
    check(sky_parse_utc("2026-11-09", &jd) && fabs(jd - sky_jd(2026, 11, 9, 0, 0, 0.0)) < 1e-9, "parse 2026-11-09", jd, sky_jd(2026, 11, 9, 0, 0, 0.0));
    check(!sky_parse_utc("2026-13-09T22:00", &jd) && !sky_parse_utc("22:00", &jd) && !sky_parse_utc("2026-11-09T22", &jd), "parse rejects nonsense", 0, 0);
    char text[64];
    sky_format_utc(sky_jd(2026, 11, 9, 22, 5, 7.0), text, sizeof(text));
    check(strcmp(text, "2026-11-09 22:05:07 UTC") == 0, text, 0, 0);
    SkyClock clock;
    sky_clock_init(&clock, 2451545.0, 10.0, false);
    near("the clock runs at 1x", sky_clock_jd(&clock, 10.0 + 86400.0), 2451546.0, 1e-9);
    sky_clock_init(&clock, 2451545.0, 10.0, true);
    near("a capture's clock is held", sky_clock_jd(&clock, 1000.0), 2451545.0, 0.0);
    near("local solar time at 90 W, 12:00 UTC", sky_local_solar_hours(sky_jd(2026, 3, 1, 12, 0, 0.0), -90.0), 6.0, 1e-6);
    /* the day of the week there (0 Monday): 2000-01-01 was a Saturday; the captures' evening in Los
     * Angeles (SKY_CAPTURE_UTC, Thursday 02:50 UTC) is still Wednesday there; Tokyo is on Thursday */
    near("weekday 2000-01-01 12:00 at 0 E", sky_local_weekday(2451545.0, 0.0), 5.0, 0.0);
    near("weekday of the captures in Los Angeles", sky_local_weekday(sky_jd(2026, 12, 24, 2, 50, 0.0), -118.24), 2.0, 0.0);
    near("weekday of the captures in Tokyo", sky_local_weekday(sky_jd(2026, 12, 24, 2, 50, 0.0), 139.69), 3.0, 0.0);
    near("weekday just before midnight at 0 E", sky_local_weekday(sky_jd(2026, 11, 9, 23, 59, 0.0), 0.0), 0.0, 0.0);

    SkyState s;
    /* sidereal time (Meeus 12.a): 1987 April 10, 0h UT, 13h10m46.3668s */
    sky_state(2446895.5, &s);
    near("GMST 1987-04-10 0h (Meeus 12.a), degrees", s.gmst / DEG, 197.693195, 1e-5);

    /* the sun (Meeus 25.a): 1992 October 13.0, apparent RA 13h13m31.4s, dec -7d47'06" */
    sky_state(2448908.5, &s);
    near("sun RA 1992-10-13 (Meeus 25.a), degrees", s.sunRa / DEG, 198.38083, 0.01);
    near("sun dec 1992-10-13 (Meeus 25.a), degrees", s.sunDec / DEG, -7.78507, 0.01);

    /* the moon (Meeus 47.a): 1992 April 12, 0h TD, apparent RA 134.688470, dec 13.768368, 368409.7 km */
    sky_state(2448724.5, &s);
    near("moon RA 1992-04-12 (Meeus 47.a), degrees", s.moonRa / DEG, 134.688470, 0.1);
    near("moon dec 1992-04-12 (Meeus 47.a), degrees", s.moonDec / DEG, 13.768368, 0.1);
    near("moon distance 1992-04-12 (Meeus 47.a), km", s.moonKm, 368409.7, 300.0);

    /* phases: full moon 2024-01-25 17:54 UTC, new moon 2024-04-08 18:21 UTC */
    sky_state(sky_jd(2024, 1, 25, 17, 54, 0.0), &s);
    check(s.moonLit > 0.995 && s.moonBright > 0.85, "full moon 2024-01-25 17:54: lit share", s.moonLit, 1.0);
    sky_state(sky_jd(2024, 4, 8, 18, 21, 0.0), &s);
    check(s.moonLit < 0.005 && s.moonBright < 1e-3, "new moon 2024-04-08 18:21: lit share", s.moonLit, 0.0);
    /* first quarter 2026-01-26 04:47 UTC: half lit, about a tenth as bright as full */
    sky_state(sky_jd(2026, 1, 26, 4, 47, 0.0), &s);
    near("first quarter 2026-01-26 04:47: lit share", s.moonLit, 0.5, 0.02);
    near("first quarter: brightness against full", s.moonBright, 0.09, 0.02);

    /* the total eclipse of 2024 April 8: greatest at 18:17:16 UTC, at 25.29 N
     * 104.14 W, the sun 69.8 degrees up; there the moon covers the sun (the
     * parallax matters: from the Earth's centre they stand apart) */
    {
        double up[3], m[3];
        up_at(25.29, -104.14, up);
        sky_state(sky_jd(2024, 4, 8, 18, 17, 16.0), &s);
        sky_moon_topocentric(&s, up, m);
        double sep = acos(fmin(1.0, m[0] * s.sun[0] + m[1] * s.sun[1] + m[2] * s.sun[2])) / DEG;
        near("eclipse 2024-04-08: moon to sun from Nazas, degrees", sep, 0.0, 0.15);
        double alt, az;
        sky_alt_az(s.sun, up, &alt, &az);
        near("eclipse 2024-04-08: the sun's altitude there", alt / DEG, 69.8, 0.2);
    }

    /* sunrise on 2026-11-09 (sunrise-sunset tables): New York 06:36 EST (11:36 UTC), Moscow 07:52 MSK (04:52 UTC) */
    {
        double day = sky_jd(2026, 11, 9, 0, 0, 0.0);
        double ny = sunrise(day, 40.7128, -74.0060), mo = sunrise(day, 55.7558, 37.6173);
        near("sunrise New York 2026-11-09, hours UTC", hours_utc(ny), 11.0 + 36.0 / 60.0, 3.0 / 60.0);
        near("sunrise Moscow 2026-11-09, hours UTC", hours_utc(mo), 4.0 + 52.0 / 60.0, 3.0 / 60.0);
        double up[3];
        up_at(40.7128, -74.0060, up);
        check(sun_alt(day + 4.0 / 24.0, up) < -20.0, "night in New York at 04:00 UTC", sun_alt(day + 4.0 / 24.0, up), -40.0);
        up_at(55.7558, 37.6173, up);
        check(sun_alt(day + 10.0 / 24.0, up) > 5.0, "day in Moscow at 10:00 UTC", sun_alt(day + 10.0 / 24.0, up), 15.0);
    }

    /* noon on the June solstice at Greenwich: 90 - 51.4769 + 23.4362 */
    {
        double up[3], best = -90.0;
        up_at(51.4769, 0.0, up);
        double day = sky_jd(2026, 6, 21, 11, 0, 0.0);
        for (int i = 0; i < 240; ++i) best = fmax(best, sun_alt(day + i / 1440.0, up));
        near("sun at noon, Greenwich, 2026-06-21, degrees", best, 61.959, 0.03);
        sky_state(sky_jd(2026, 3, 20, 14, 46, 0.0), &s);
        near("sun's declination at the March equinox 2026, degrees", s.sunDec / DEG, 0.0, 0.01);
    }

    /* the capture's instant is a night with the moon up in the ENE over Los Angeles */
    {
        double up[3], alt, az;
        up_at(34.05, -118.25, up);
        check(sky_parse_utc(SKY_CAPTURE_UTC, &jd), "SKY_CAPTURE_UTC parses", jd, 0);
        moon_alt_az(jd, up, &alt, &az);
        near("capture: the moon's altitude over LA", alt, 25.0, 3.0);
        near_angle("capture: the moon's azimuth over LA", az, 70.6, 3.0);
        check(sun_alt(jd, up) < -18.0, "capture: the sun 18 degrees down", sun_alt(jd, up), -30.0);
    }

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
