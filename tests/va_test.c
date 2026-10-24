/* SPDX-License-Identifier: GPL-3.0-only */
/* VA engine test (src/eng_va.c) through hostsim.c, as noise_test.c.  build/host/va_test
 * 1. pitch: every model at A3 with the filter open: autocorrelation period within 1 % of 220 Hz
 * 2. cutoff: the high-frequency energy rises with FREQ (each model)
 * 3. every preset: audible (rms), finite, |peak| <= 2 x VOICE_FS, |mean| small
 * 4. stability: RES 127 over FREQ / notes / models: finite and bounded
 * 5. determinism: the same note from silence renders the same samples
 * 6. cost: ns a sample and voice per model, against the Prophet */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <time.h>

static int bad;
static void check(const char *w, int ok) { printf("va: %-84s %s\n", w, ok ? "ok" : "FAIL"); bad += !ok; }

static uint32_t eng_of(const engine_t *x) { uint32_t e; for (e = 0; e < NENGINES; e++) if (ENGINES[e] == x) return e; return 0; }

static void setup(uint32_t eng, uint32_t pi, const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], eng, pi);
    if (e) for (i = 0; i < 8u; i++) trk[0].p[P_E0 + i] = e[i];
    trk[0].p[P_ATK] = 0; trk[0].p[P_SUS] = 127; trk[0].p[P_REL] = 10;
    trk[0].p[P_ED_FLT] = 0; trk[0].p[P_VOICE] = V_POLY;
}
static void render(uint32_t note, double *y, uint32_t n, uint32_t nnotes)
{
    static int32_t b[CTL];
    uint32_t i, k, skip = FS / 10u / CTL;
    for (k = 0; k < nnotes; k++) trk_note_on(&trk[0], note + k * 4u, 100);
    for (i = 0; i < skip; i++) track_render(&trk[0], b, CTL);
    for (i = 0; i < n; i += CTL) {
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++) y[i + k] = b[k] / 32768.0;
    }
}
static double rms(const double *y, uint32_t n) { double s = 0; for (uint32_t i = 0; i < n; i++) s += y[i] * y[i]; return sqrt(s / n); }
static double hf(const double *y, uint32_t n) { double s = 0; for (uint32_t i = 2; i < n; i++) { double d = y[i] - 2 * y[i-1] + y[i-2]; s += d * d; } return sqrt(s / n); }
static double period_hz(const double *y, uint32_t n)
{
    uint32_t lo = FS / 1000u, hi = FS / 100u, lag, best = lo; double bv = -1e9;
    for (lag = lo; lag < hi; lag++) { double s = 0; for (uint32_t i = 0; i + lag < n; i++) s += y[i] * y[i + lag]; if (s > bv) { bv = s; best = lag; } }
    return (double)FS / best;
}
#define N 16384
static double Y[N];
static const int16_t BASE[3][8] = {   /* MODEL E1 E2 FREQ RES E5 E6 NOIS */
    {0, 64, 0, 127, 0, 0, 0, 0}, {1, 0, 0, 127, 0, 0, 0, 0}, {2, 0, 0, 127, 0, 0, 1, 0}};
static const char *MN[3] = {"JUNO", "UBXA", "JP80"};

int main(void)
{
    uint32_t m, pi, eng = eng_of(&ENG_VA);
    char w[200];
    check("engine registered and selectable", eng == ENGI_VA && eng_ok(eng));
    for (m = 0; m < 3u; m++) {
        double f;
        setup(eng, 0, BASE[m]);
        render(57, Y, N, 1);
        f = period_hz(Y, N);
        snprintf(w, sizeof w, "%s pitch A3: %.1f Hz (want 220 +-1%%)", MN[m], f);
        check(w, fabs(f / 220.0 - 1.0) < 0.01);
    }
    for (m = 0; m < 3u; m++) {
        int16_t e[8]; double h1, h2;
        memcpy(e, BASE[m], sizeof e); e[3] = 30; setup(eng, 0, e); render(48, Y, N, 1); h1 = hf(Y, N) / (rms(Y, N) + 1e-9);
        e[3] = 110; setup(eng, 0, e); render(48, Y, N, 1); h2 = hf(Y, N) / (rms(Y, N) + 1e-9);
        snprintf(w, sizeof w, "%s cutoff: brightness %.3f at FREQ 30 < %.3f at 110", MN[m], h1, h2);
        check(w, h2 > h1 * 1.5);
    }
    for (pi = 0; pi < ENG_VA.npresets; pi++) {
        double mean = 0, pk = 0, r; uint32_t i; int fin = 1;
        setup(eng, pi, 0);
        render(48, Y, N, 3);
        for (i = 0; i < N; i++) { if (!isfinite(Y[i])) fin = 0; mean += Y[i]; if (fabs(Y[i]) > pk) pk = fabs(Y[i]); }
        mean /= N; r = rms(Y, N);
        snprintf(w, sizeof w, "preset %-9s rms %.3f peak %.3f mean %.4f", ENG_VA.presets[pi].name, r, pk, mean);
        check(w, fin && r > 0.02 && pk <= 2.0 * 24000 / 32768.0 * 3 && fabs(mean) < 0.03);
    }
    {
        int ok = 1, fr, no; uint32_t mm;
        for (mm = 0; mm < 3u; mm++) for (fr = 0; fr <= 127; fr += 21) for (no = 24; no <= 108; no += 28) {
            int16_t e[8]; uint32_t i; memcpy(e, BASE[mm], sizeof e); e[3] = fr; e[4] = 127; e[7] = 127; e[1] = 127; e[2] = 127; e[5] = 127; e[6] = 127;
            if (mm == 2) e[6] = 1;
            setup(eng, 0, e); render(no, Y, 4096, 4);
            for (i = 0; i < 4096; i++) if (!isfinite(Y[i]) || fabs(Y[i]) > 8.0) ok = 0;
        }
        check("stability: RES/NOIS/E1/E2 at 127 across FREQ, notes, models", ok);
    }
    {
        static double A[4096]; uint32_t i; int same = 1;
        setup(eng, 6, 0); render(60, A, 4096, 1);
        setup(eng, 6, 0); render(60, Y, 4096, 1);
        for (i = 0; i < 4096; i++) if (A[i] != Y[i]) same = 0;
        check("determinism: a note from silence renders the same", same);
    }
    {
        static int32_t b[CTL]; uint32_t k, i, nb = 600; struct timespec a, c; double ns[3], ps;
        for (m = 0; m < 4u; m++) {
            uint32_t en = m < 3u ? eng : ENGI_PROPHET;
            setup(en, 0, m < 3u ? BASE[m] : 0);
            if (m < 3u) trk[0].p[P_E3] = 90;
            for (k = 0; k < 4u; k++) trk_note_on(&trk[0], 48 + k * 4u, 100);
            for (i = 0; i < 40u; i++) track_render(&trk[0], b, CTL);
            clock_gettime(CLOCK_MONOTONIC, &a);
            for (i = 0; i < nb; i++) track_render(&trk[0], b, CTL);
            clock_gettime(CLOCK_MONOTONIC, &c);
            ps = ((c.tv_sec - a.tv_sec) * 1e9 + (c.tv_nsec - a.tv_nsec)) / (nb * (double)CTL * 4);
            if (m < 3u) ns[m] = ps; else {
                snprintf(w, sizeof w, "cost ns/sample/voice: JUNO %.0f UBXA %.0f JP80 %.0f, Prophet %.0f", ns[0], ns[1], ns[2], ps);
                check(w, ns[0] < ps * 2 && ns[1] < ps * 2 && ns[2] < ps * 3);
            }
        }
    }
    printf("%s\n", bad ? "va: FAILURES" : "va: ALL PASSED");
    return bad != 0;
}
