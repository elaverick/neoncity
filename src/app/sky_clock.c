#include "sky_clock.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

#define DEG (3.14159265358979323846 / 180.0)
#define AU_KM 149597870.7
#define EARTH_KM 6378.14   /* the Earth's equatorial radius, for the moon's parallax (not the game's planet) */

double sky_jd(int year, int month, int day, int hour, int minute, double second) {
    if (month <= 2) { year -= 1; month += 12; }
    int a = year / 100, b = 2 - a + a / 4;
    double d = day + (hour + (minute + second / 60.0) / 60.0) / 24.0;
    return floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1)) + d + b - 1524.5;
}

bool sky_parse_utc(const char *s, double *jd) {
    int y, mo, d, h = 0, mi = 0, n = 0;
    double sec = 0.0;
    char sep;
    if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &n) != 3) return false;
    s += n;
    if (*s == 'T' || *s == 't' || *s == ' ') {
        if (sscanf(s, "%c%d:%d%n", &sep, &h, &mi, &n) != 3) return false;
        s += n;
        if (*s == ':') {
            if (sscanf(s, ":%lf%n", &sec, &n) != 1) return false;
            s += n;
        }
    }
    if (*s == 'Z' || *s == 'z') ++s;
    if (*s || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0.0 || sec >= 61.0) return false;
    *jd = sky_jd(y, mo, d, h, mi, sec);
    return true;
}

void sky_format_utc(double jd, char *buf, size_t n) {
    /* Meeus 7.3 */
    double z = floor(jd + 0.5), f = jd + 0.5 - z;
    double a = z;
    if (z >= 2299161.0) {
        double al = floor((z - 1867216.25) / 36524.25);
        a = z + 1 + al - floor(al / 4.0);
    }
    double b = a + 1524, c = floor((b - 122.1) / 365.25), d = floor(365.25 * c), e = floor((b - d) / 30.6001);
    int day = (int)(b - d - floor(30.6001 * e));
    int month = (int)(e < 14 ? e - 1 : e - 13);
    int year = (int)(month > 2 ? c - 4716 : c - 4715);
    long s = lround(f * 86400.0);
    if (s >= 86400) s = 86399;   /* (no carry into the next day: a second at worst) */
    snprintf(buf, n, "%04d-%02d-%02d %02ld:%02ld:%02ld UTC", year, month, day, s / 3600, s / 60 % 60, s % 60);
}

double sky_jd_now(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) { ts.tv_sec = time(NULL); ts.tv_nsec = 0; }
    return 2440587.5 + ((double)ts.tv_sec + ts.tv_nsec * 1e-9) / 86400.0;
}

void sky_clock_init(SkyClock *c, double jd, double t, bool held) {
    c->jd0 = jd;
    c->t0 = t;
    c->held = held;
}

double sky_clock_jd(const SkyClock *c, double t) { return c->held ? c->jd0 : c->jd0 + (t - c->t0) / 86400.0; }

static double wrap360(double a) { a = fmod(a, 360.0); return a < 0.0 ? a + 360.0 : a; }

/* The ecliptic direction (longitude lam, latitude bet, rad) turned by the
 * obliquity eps into the equatorial frame of date. */
static void ecliptic_to_equatorial(double lam, double bet, double eps, double out[3]) {
    double x = cos(bet) * cos(lam), y = cos(bet) * sin(lam), z = sin(bet);
    out[0] = x;
    out[1] = y * cos(eps) - z * sin(eps);
    out[2] = y * sin(eps) + z * cos(eps);
}

static void ra_dec(const double c[3], double *ra, double *dec) {
    *ra = atan2(c[1], c[0]);
    if (*ra < 0.0) *ra += 2.0 * 3.14159265358979323846;
    *dec = asin(fmax(-1.0, fmin(1.0, c[2])));
}

/* Meeus table 47.A, the largest terms: the multiples of D, M, M', F, then the
 * longitude's (1e-6 degrees) and the distance's (1e-3 km) coefficients. */
static const int MOON_LR[][6] = {
    { 0, 0, 1, 0, 6288774, -20905355 }, { 2, 0, -1, 0, 1274027, -3699111 }, { 2, 0, 0, 0, 658314, -2955968 },
    { 0, 0, 2, 0, 213618, -569925 },    { 0, 1, 0, 0, -185116, 48888 },     { 0, 0, 0, 2, -114332, -3149 },
    { 2, 0, -2, 0, 58793, 246158 },     { 2, -1, -1, 0, 57066, -152138 },   { 2, 0, 1, 0, 53322, -170733 },
    { 2, -1, 0, 0, 45758, -204586 },    { 0, 1, -1, 0, -40923, -129620 },   { 1, 0, 0, 0, -34720, 108743 },
    { 0, 1, 1, 0, -30383, 104755 },     { 2, 0, 0, -2, 15327, 10321 },      { 0, 0, 1, 2, -12528, 0 },
    { 0, 0, 1, -2, 10980, 79661 },      { 4, 0, -1, 0, 10675, -34782 },     { 0, 0, 3, 0, 10034, -23210 },
    { 4, 0, -2, 0, 8548, -21636 },      { 2, 1, -1, 0, -7888, 24208 },      { 2, 1, 0, 0, -6766, 30824 },
    { 1, 0, -1, 0, -5163, -8379 },      { 1, 1, 0, 0, 4987, -16675 },       { 2, -1, 1, 0, 4036, -12831 },
    { 2, 0, 2, 0, 3994, -10445 },       { 4, 0, 0, 0, 3861, -11650 },       { 2, 0, -3, 0, 3665, 14403 },
    { 0, 1, -2, 0, -2689, -7003 },      { 2, 0, -1, 2, -2602, 0 },          { 2, -1, -2, 0, 2390, 10056 },
    { 1, 0, 1, 0, -2348, 6322 },        { 2, -2, 0, 0, 2236, -9884 },
};
/* Table 47.B, the largest terms: D, M, M', F, the latitude's coefficient (1e-6 degrees). */
static const int MOON_B[][5] = {
    { 0, 0, 0, 1, 5128122 }, { 0, 0, 1, 1, 280602 }, { 0, 0, 1, -1, 277693 }, { 2, 0, 0, -1, 173237 },
    { 2, 0, -1, 1, 55413 },  { 2, 0, -1, -1, 46271 }, { 2, 0, 0, 1, 32573 },  { 0, 0, 2, 1, 17198 },
    { 2, 0, 1, -1, 9266 },   { 0, 0, 2, -1, 8822 },  { 2, -1, 0, -1, 8216 },  { 2, 0, -2, -1, 4324 },
    { 2, 0, 1, 1, 4200 },    { 2, 1, 0, -1, -3359 }, { 2, -1, -1, 1, 2463 },  { 2, -1, 0, 1, 2211 },
    { 2, -1, -1, -1, 2065 }, { 0, 1, -1, -1, -1870 }, { 4, 0, -1, -1, 1828 }, { 0, 1, 0, 1, -1794 },
};

void sky_state(double jd, SkyState *s) {
    double T = (jd - 2451545.0) / 36525.0;
    s->jd = jd;

    /* Greenwich mean sidereal time (Meeus 12.4) */
    double th = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + T * T * (0.000387933 - T / 38710000.0);
    s->gmst = wrap360(th) * DEG;

    /* the nutation's largest term and the obliquity (Meeus 22) */
    double om = (125.04452 - 1934.136261 * T) * DEG;
    double eps = (23.439291111 - 0.0130041667 * T - 1.639e-7 * T * T + 5.036e-7 * T * T * T + 0.00256 * cos(om)) * DEG;

    /* the sun (Meeus 25, low precision: apparent longitude) */
    double L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T);
    double M = (357.52911 + T * (35999.05029 - 0.0001537 * T)) * DEG;
    double ecc = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    double C = (1.914602 - T * (0.004817 + 0.000014 * T)) * sin(M) + (0.019993 - 0.000101 * T) * sin(2.0 * M) + 0.000289 * sin(3.0 * M);
    double nu = M + C * DEG;
    double R = 1.000001018 * (1.0 - ecc * ecc) / (1.0 + ecc * cos(nu));   /* AU */
    double lamSun = (L0 + C - 0.00569 - 0.00478 * sin(om)) * DEG;
    double cs[3];
    ecliptic_to_equatorial(lamSun, 0.0, eps, cs);
    ra_dec(cs, &s->sunRa, &s->sunDec);
    sky_celestial_to_earth(s, cs, s->sun);

    /* the moon (Meeus 47, truncated) */
    double Lp = 218.3164477 + T * (481267.88123421 + T * (-0.0015786 + T * (1.0 / 538841.0 - T / 65194000.0)));
    double D  = (297.8501921 + T * (445267.1114034 + T * (-0.0018819 + T * (1.0 / 545868.0 - T / 113065000.0)))) * DEG;
    double Ms = (357.5291092 + T * (35999.0502909 + T * (-0.0001536 + T / 24490000.0))) * DEG;
    double Mm = (134.9633964 + T * (477198.8675055 + T * (0.0087414 + T * (1.0 / 69699.0 - T / 14712000.0)))) * DEG;
    double F  = (93.2720950 + T * (483202.0175233 + T * (-0.0036539 + T * (-1.0 / 3526000.0 + T / 863310000.0)))) * DEG;
    double E = 1.0 - T * (0.002516 + 0.0000074 * T);
    double A1 = (119.75 + 131.849 * T) * DEG, A2 = (53.09 + 479264.290 * T) * DEG, A3 = (313.45 + 481266.484 * T) * DEG;
    double sl = 0.0, sr = 0.0, sb = 0.0;
    for (size_t i = 0; i < sizeof(MOON_LR) / sizeof(MOON_LR[0]); ++i) {
        const int *t = MOON_LR[i];
        double arg = t[0] * D + t[1] * Ms + t[2] * Mm + t[3] * F;
        double e = t[1] == 0 ? 1.0 : (t[1] == 1 || t[1] == -1 ? E : E * E);
        sl += t[4] * e * sin(arg);
        sr += t[5] * e * cos(arg);
    }
    for (size_t i = 0; i < sizeof(MOON_B) / sizeof(MOON_B[0]); ++i) {
        const int *t = MOON_B[i];
        double arg = t[0] * D + t[1] * Ms + t[2] * Mm + t[3] * F;
        double e = t[1] == 0 ? 1.0 : (t[1] == 1 || t[1] == -1 ? E : E * E);
        sb += t[4] * e * sin(arg);
    }
    double LpR = Lp * DEG;
    sl += 3958.0 * sin(A1) + 1962.0 * sin(LpR - F) + 318.0 * sin(A2);
    sb += -2235.0 * sin(LpR) + 382.0 * sin(A3) + 175.0 * sin(A1 - F) + 175.0 * sin(A1 + F) + 127.0 * sin(LpR - Mm) - 115.0 * sin(LpR + Mm);
    double lamMoon = (Lp + sl * 1e-6 - 0.00478 * sin(om)) * DEG, betMoon = sb * 1e-6 * DEG;
    s->moonKm = 385000.56 + sr * 1e-3;
    double cm[3];
    ecliptic_to_equatorial(lamMoon, betMoon, eps, cm);
    ra_dec(cm, &s->moonRa, &s->moonDec);
    sky_celestial_to_earth(s, cm, s->moon);

    /* its phase (Meeus 48.2-48.3) and the brightness that goes with it: the
     * magnitude grows by 0.026 |i| + 4e-9 i^4 (i in degrees) from full */
    double cpsi = fmax(-1.0, fmin(1.0, cs[0] * cm[0] + cs[1] * cm[1] + cs[2] * cm[2]));
    double psi = acos(cpsi), Rk = R * AU_KM;
    s->moonPhase = atan2(Rk * sin(psi), s->moonKm - Rk * cpsi);
    s->moonLit = 0.5 * (1.0 + cos(s->moonPhase));
    double ideg = s->moonPhase / DEG;
    s->moonBright = pow(10.0, -0.4 * (0.026 * ideg + 4e-9 * ideg * ideg * ideg * ideg));
}

void sky_moon_topocentric(const SkyState *s, const double up[3], double out[3]) {
    double p[3], l = 0.0;
    for (int k = 0; k < 3; ++k) { p[k] = s->moon[k] * s->moonKm - up[k] * EARTH_KM; l += p[k] * p[k]; }
    l = sqrt(l);
    for (int k = 0; k < 3; ++k) out[k] = p[k] / l;
}

void sky_alt_az(const double d[3], const double up[3], double *alt, double *az) {
    double east[3] = { -up[1], up[0], 0.0 };   /* the pole (+Z) cross up */
    double el = sqrt(east[0] * east[0] + east[1] * east[1]);
    if (el < 1e-12) { east[0] = 0.0; east[1] = 1.0; el = 1.0; }   /* (at a pole, any) */
    east[0] /= el; east[1] /= el;
    double north[3] = { up[1] * east[2] - up[2] * east[1], up[2] * east[0] - up[0] * east[2], up[0] * east[1] - up[1] * east[0] };   /* up cross east */
    double u = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    double e = d[0] * east[0] + d[1] * east[1];
    double n = d[0] * north[0] + d[1] * north[1] + d[2] * north[2];
    *alt = asin(fmax(-1.0, fmin(1.0, u)));
    *az = atan2(e, n);
    if (*az < 0.0) *az += 2.0 * 3.14159265358979323846;
}

double sky_local_solar_hours(double jd, double lon_deg) {
    double h = fmod((jd + 0.5 - floor(jd + 0.5)) * 24.0 + lon_deg / 15.0, 24.0);
    return h < 0.0 ? h + 24.0 : h;
}

int sky_local_weekday(double jd, double lon_deg) {
    long d = (long)floor(jd + 0.5 + lon_deg / 360.0) % 7;   /* JD 0 began on a Monday (noon to noon: + 0.5) */
    return (int)(d < 0 ? d + 7 : d);
}

void sky_celestial_to_earth(const SkyState *s, const double c[3], double out[3]) {
    double g = s->gmst, cg = cos(g), sg = sin(g);
    out[0] = c[0] * cg + c[1] * sg;
    out[1] = -c[0] * sg + c[1] * cg;
    out[2] = c[2];
}
