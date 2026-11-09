#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The world's clock and the sky it sets (day/night cycle DN0).
 *
 * The clock is UTC, kept as a Julian date in doubles. At run time it is the
 * system's UTC at 1x, or starts at --utc and runs on from there; a capture
 * (--shot) holds it, at --utc or at SKY_CAPTURE_UTC, so before/after shots
 * stay comparable.
 *
 * The sun, the moon and the stars are worked out in the planet's Earth-fixed
 * frame (src/terrain/terrain.h: +Z the north pole, +X latitude 0 longitude
 * 0, +Y 90 E), so they are right for every place at once: it is night in
 * Moscow while it is afternoon in New York. The sun is the NOAA/Meeus
 * low-precision solution (about 0.01 degrees), the moon Meeus's truncated
 * series (chapter 47's largest terms: 0.005 degrees off in its example), the stars
 * turned by Greenwich mean sidereal time. The formulae want Terrestrial
 * Time; UTC is used (69 s off: 0.01 degrees of the moon). */

/* The instant a capture takes without --utc: an evening in Los Angeles
 * (18:57 local solar time, Christmas Eve) with the full moon 24 degrees up
 * in the east-north-east, near where the moon used to stand fixed (altitude
 * 25, azimuth 71: sky_test --find). */
#define SKY_CAPTURE_UTC "2026-12-24T02:50"

/* The Julian date of a UTC calendar instant (Meeus 7.1; Gregorian). */
double sky_jd(int year, int month, int day, int hour, int minute, double second);
/* "YYYY-MM-DD[THH:MM[:SS]][Z]" (a space for the T will do) as a Julian date; false if it isn't one. */
bool   sky_parse_utc(const char *s, double *jd);
/* The Julian date as "YYYY-MM-DD HH:MM:SS UTC". */
void   sky_format_utc(double jd, char *buf, size_t n);
/* The system's UTC now. */
double sky_jd_now(void);

typedef struct SkyClock {
    double jd0;    /* the Julian date at t0 */
    double t0;     /* the app's clock (s) then */
    bool   held;   /* a capture's: it doesn't run */
} SkyClock;
void   sky_clock_init(SkyClock *c, double jd, double t, bool held);
double sky_clock_jd(const SkyClock *c, double t);

/* The sky at an instant, Earth-fixed and geocentric. */
typedef struct SkyState {
    double jd;
    double gmst;        /* Greenwich mean sidereal angle (rad): the stars' frame turned by it about +Z is the Earth's */
    double sun[3];      /* towards the sun (unit) */
    double sunRa, sunDec;     /* its apparent right ascension and declination (rad) */
    double moon[3];     /* towards the moon (unit), from the Earth's centre */
    double moonKm;      /* its distance (km) */
    double moonRa, moonDec;   /* its geocentric right ascension and declination (rad) */
    double moonPhase;   /* its phase angle (rad): 0 full, pi new */
    double moonLit;     /* the share of its disc lit */
    double moonBright;  /* its brightness against a full moon's (the phase law, Allen) */
} SkyState;
void   sky_state(double jd, SkyState *s);

/* The moon as seen from a place on the Earth's surface whose up (Earth-fixed,
 * unit) is `up`: its parallax moves it by up to about a degree. */
void   sky_moon_topocentric(const SkyState *s, const double up[3], double out[3]);
/* Direction d's altitude (rad, above the horizon) and azimuth (rad, from
 * north through east) at a place whose up is `up` (both Earth-fixed). */
void   sky_alt_az(const double d[3], const double up[3], double *alt, double *az);
/* The local mean solar time (hours, 0..24) at longitude lon (degrees east). */
double sky_local_solar_hours(double jd, double lon_deg);
/* The day of the week there by that solar time: 0 Monday .. 6 Sunday. */
int    sky_local_weekday(double jd, double lon_deg);
/* The Earth-fixed direction of a star's frame (equatorial, of date) vector c. */
void   sky_celestial_to_earth(const SkyState *s, const double c[3], double out[3]);
