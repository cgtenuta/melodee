/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* VA: three virtual-analog voice models in one engine, picked by MODEL (E0):
 *   JUNO  Roland Juno-6 style: DCO saw + pulse (PWM) + sub square + noise, a one-pole HPF, a 24 dB
 *         resonant low-pass, and a BBD-style chorus (CHOR, in post()).
 *   UBXA  Oberheim Xpander/Matrix-style "UB-Xa" voice: two VCOs (DETU, MIX), cross-modulation of the first
 *         by the second (XMOD), and a 2-pole SEM-style state-variable filter morphing LP -> notch -> HP (MORPH).
 *   JP80  Roland JP-8080 style: a 7-saw supersaw (DETU spread, MIX of the side voices), a HPF, and a 12 or
 *         24 dB low-pass (POLE).
 * Amplitude and filter envelopes, LFO, glide and the matrix are the track's (voice.c): this engine renders
 * the oscillators and filter in float and returns Q15-scaled samples through voice_amp().
 * These are sound-alike models of the architectures, not circuit emulations.
 *
 * Per-part state is va_part_t (resource RES_ENGINE0 + part, engines.c): VA_POLY voices of oscillator phases
 * and filter state, and the chorus' delay line. */
#include "x0x/fastmath.h"
#define VA_POLY 6u
#define VA_DL 1024u
#define VA_PI 3.14159265f
static const char *const N_VA_MODEL[] = {"JUNO", "UBXA", "JP80"};
enum { VA_JUNO, VA_UBXA, VA_JP80 };

typedef struct {
    float ph[7];                 /* oscillator phases 0..1 */
    float z[4];                  /* filter integrators */
    float hp;                    /* HPF one-pole state */
    uint32_t rng;
    uint8_t live;
} va_voice_t;
typedef struct {
    va_voice_t v[VA_POLY];
    float dl[VA_DL];
    uint32_t wi;
    float lfo;
} va_part_t;
static va_part_t *va_part(uint32_t part);

static const param_desc_t VA_E1[3] = {{"PW", F_PCT, 0, 127, 64, 0, 0}, {"DETU", F_PCT, 0, 127, 20, 0, 0}, {"DETU", F_PCT, 0, 127, 40, 0, 0}};
static const param_desc_t VA_E2[3] = {{"SUB", F_PCT, 0, 127, 0, 0, 0}, {"MIX", F_PCT, 0, 127, 64, 0, 0}, {"MIX", F_PCT, 0, 127, 80, 0, 0}};
static const param_desc_t VA_E5[3] = {{"HPF", F_PCT, 0, 127, 0, 0, 0}, {"XMOD", F_PCT, 0, 127, 0, 0, 0}, {"HPF", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t VA_E6[3] = {{"CHOR", F_PCT, 0, 127, 0, 0, 0}, {"MORP", F_PCT, 0, 127, 0, 0, 0}, {"POLE", F_ONOFF, 0, 1, 1, 0, 0}};

static const param_desc_t *va_desc(const track_t *t, uint32_t k)
{
    uint32_t m = (uint32_t)t->p[P_E0];
    if (m > VA_JP80)
        m = VA_JP80;
    switch (k) {
    case 1: return &VA_E1[m];
    case 2: return &VA_E2[m];
    case 5: return &VA_E5[m];
    case 6: return &VA_E6[m];
    default: return 0;
    }
}

static inline float va_rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
static inline float va_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static inline float va_saw(float ph, float dt) { return 2.0f * ph - 1.0f - va_blep(ph, dt); }
static inline float va_pulse(float ph, float dt, float pw)
{
    float p2 = ph + 1.0f - pw, y = ph < pw ? 1.0f : -1.0f;
    if (p2 >= 1.0f)
        p2 -= 1.0f;
    return y + va_blep(ph, dt) - va_blep(p2, dt);
}
static inline float va_soft(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
static inline float va_adv(float *ph, float dt)
{
    float p = *ph + dt;
    if (p >= 1.0f)
        p -= 1.0f;
    *ph = p;
    return p;
}

static void va_note_on(track_t *t, voice_t *v)
{
    va_part_t *P = va_part((uint32_t)(t - trk));
    va_voice_t *x;
    uint32_t i, seed;
    if (!P)
        return;
    x = &P->v[(uint32_t)(v - t->v) % VA_POLY];
    seed = (v->note + 1u) * 0x9E3779B9u ^ (uint32_t)v->fine * 0x85EBCA6Bu;
    if (!x->live) {                                  /* from silence: fixed phases (same sound each note) */
        for (i = 0; i < 7u; i++)
            x->ph[i] = (float)((seed >> (i * 3u)) & 255u) * (1.0f / 256.0f);
        x->z[0] = x->z[1] = x->z[2] = x->z[3] = x->hp = 0.0f;
    }
    x->rng = seed ? seed : 1u;
    x->live = 1;
}

static void va_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    va_part_t *P = va_part((uint32_t)(t - trk));
    va_voice_t *x;
    uint32_t model = (uint32_t)p[P_E0], i, j;
    float hz, dt, g, G, G2, G4, kres, hpa, pw, mixa, noisa, sub, detu, xmod, morph, side, cutidx, cuthz;
    float ph2_dt;
    int pole4 = 1;
    if (!P)
        return;
    if (model > VA_JP80)
        model = VA_JP80;
    x = &P->v[(uint32_t)(v - t->v) % VA_POLY];
    hz = 440.0f * fm_exp2f(((float)m->pitch16 * (1.0f / 16.0f) - 69.0f) * (1.0f / 12.0f));
    hz *= 1.0f + (float)m->fine * (1.0f / 4096.0f);
    dt = hz * (1.0f / (float)FS);
    if (dt > 0.45f)
        dt = 0.45f;
    cutidx = clamp((p[P_E3] << 8) + m->cutoff, 0, 127 << 8) * (1.0f / 256.0f);
    cuthz = 20.0f * fm_exp2f(cutidx * (9.8f / 127.0f));
    g = fm_tanf(cuthz * (VA_PI / (float)FS) > 1.5f ? 1.5f : cuthz * (VA_PI / (float)FS));
    G = g / (1.0f + g);
    G2 = G * G;
    G4 = G2 * G2;
    kres = (float)p[P_E4] * (model == VA_UBXA ? 0.9f / 127.0f : 3.9f / 127.0f);   /* ladder k, SVF damping 1-.. */
    pw = 0.5f;
    mixa = (float)p[P_E2] * (1.0f / 127.0f);
    noisa = (float)p[P_E7] * (1.0f / 127.0f);
    hpa = 0.0f;
    sub = 0.0f;
    detu = (float)p[P_E1] * (1.0f / 127.0f);
    xmod = morph = side = 0.0f;
    ph2_dt = dt;
    if (model == VA_JUNO) {
        pw = 0.05f + 0.45f * detu;                      /* E1 = PW: 5 % .. 50 % */
        sub = mixa;
        hpa = (float)p[P_E5] * (1.0f / 127.0f);
    } else if (model == VA_UBXA) {
        ph2_dt = dt * fm_exp2f(detu * (1.0f / 12.0f));       /* osc 2 up to a semitone sharp */
        xmod = (float)p[P_E5] * (1.0f / 127.0f);
        morph = (float)p[P_E6] * (2.0f / 127.0f);
    } else {
        hpa = (float)p[P_E5] * (1.0f / 127.0f);
        pole4 = p[P_E6] != 0;
        side = mixa;
    }
    {
        float hpc = 1.0f - fm_expf(-VA_PI * 2.0f * (30.0f * fm_exp2f(hpa * 5.0f)) * (1.0f / (float)FS));   /* 30 Hz .. 1 kHz */
        float u, y, sb;
        static const float SS[7] = {0.0f, -0.11002313f, 0.11002313f, -0.06288439f, 0.06288439f, -0.01952356f, 0.01952356f};
        for (i = 0; i < n; i++) {
            float o;
            if (model == VA_JUNO) {
                float sw = va_saw(x->ph[0], dt), pu = va_pulse(x->ph[0], dt, pw) - (2.0f * pw - 1.0f);   /* no DC */
                float su = x->ph[0] < 0.5f ? 1.0f : -1.0f;
                o = 0.5f * sw + 0.5f * pu + sub * 0.5f * su + noisa * va_rnd(&x->rng);
                va_adv(&x->ph[0], dt);
            } else if (model == VA_UBXA) {
                float o2 = va_saw(x->ph[1], ph2_dt), a;
                a = x->ph[0] + xmod * 0.15f * o2;
                a -= (float)(int32_t)a;                         /* (a >= 0: a floor) */
                if (a < 0.0f)
                    a += 1.0f;
                o = (1.0f - 0.5f * mixa) * va_saw(a, dt) + mixa * 0.7f * o2 + noisa * va_rnd(&x->rng);
                va_adv(&x->ph[0], dt);
                va_adv(&x->ph[1], ph2_dt);
            } else {
                float acc = 0.0f, sp = detu * 0.02f;     /* spread up to +-2 % per step */
                for (j = 0; j < 7u; j++) {
                    float dj = dt * (1.0f + SS[j] * sp * 9.0f), w;
                    w = va_saw(x->ph[j], dj);
                    acc += (j == 0 ? (1.0f - 0.4f * side) : 0.35f * side) * w;
                    va_adv(&x->ph[j], dj);
                }
                o = acc * 0.6f + noisa * va_rnd(&x->rng);
            }
            if (hpa > 0.0f && model != VA_UBXA) {        /* HPF: input minus a one-pole low-pass */
                x->hp += hpc * (o - x->hp);
                o -= x->hp;
            }
            if (model == VA_UBXA) {                     /* SEM state variable */
                float R = 1.0f - kres, hpv, bpv, lpv, d = 1.0f / (1.0f + 2.0f * R * g + g * g);
                hpv = (va_soft(o * 0.9f) - (2.0f * R + g) * x->z[0] - x->z[1]) * d;
                bpv = g * hpv + x->z[0];
                lpv = g * bpv + x->z[1];
                x->z[0] = g * hpv + bpv;
                x->z[1] = g * bpv + lpv;
                if (morph <= 1.0f)
                    y = lpv + morph * hpv;                       /* LP -> LP + HP = notch */
                else
                    y = (2.0f - morph) * (lpv + hpv) + (morph - 1.0f) * hpv;
            } else {                                    /* TPT ladder with the feedback solved */
                float b0 = (1.0f - G) * x->z[0], b1 = (1.0f - G) * x->z[1], b2 = (1.0f - G) * x->z[2], b3 = (1.0f - G) * x->z[3];
                float Gk = pole4 ? G4 : G2;
                float S = pole4 ? (G2 * G * b0 + G2 * b1 + G * b2 + b3) : (G * b0 + b1);
                float vv;
                u = va_soft((o - kres * S) / (1.0f + kres * Gk));
                sb = u;
                for (j = 0; j < (pole4 ? 4u : 2u); j++) {
                    vv = G * (sb - x->z[j]);
                    y = vv + x->z[j];
                    x->z[j] = y + vv;
                    sb = y;
                }
                y = sb;
            }
            out[i] += voice_amp((int32_t)clamp((int32_t)(y * 14000.0f), -32767, 32767) , m, i);
        }
    }
}

/* Juno chorus: a modulated delay mixed with the dry signal (CHOR = depth) */
static void va_post(track_t *t, int32_t *out, uint32_t n, uint32_t nr)
{
    const int16_t *p = t->p;
    va_part_t *P = va_part((uint32_t)(t - trk));
    uint32_t i;
    float depth = (float)p[P_E6] * (1.0f / 127.0f);
    (void)nr;
    if (!P || (uint32_t)p[P_E0] != VA_JUNO)
        return;
    for (i = 0; i < n; i++) {
        float in = (float)out[i], d, f, a, b;
        uint32_t k;
        P->lfo += 0.6f * (1.0f / (float)FS);
        if (P->lfo >= 1.0f)
            P->lfo -= 1.0f;
        P->dl[P->wi % VA_DL] = in;
        d = (float)(0.0035 * FS) * (1.0f + 0.5f * depth * fm_sinf(2.0f * VA_PI * P->lfo));
        k = (uint32_t)d;
        f = d - (float)k;
        a = P->dl[(P->wi - k) % VA_DL];
        b = P->dl[(P->wi - k - 1u) % VA_DL];
        out[i] = (int32_t)(in + depth * 0.7f * (a + (b - a) * f));
        P->wi++;
    }
}

static uint32_t va_units(const track_t *t) { return (uint32_t)t->p[P_E0] == VA_JP80 ? 3u : 2u; }

/* {MODEL, E1, E2, FREQ, RES, E5, E6, NOIS} */
static const preset_t VA_PRESETS[] = {
    {"J6 BRASS", {VA_JUNO, 40, 30, 70, 20, 10, 80, 0}, {5, 60, 90, 40}, 30, 0},
    {"J6 PAD", {VA_JUNO, 70, 10, 55, 10, 0, 100, 5}, {60, 70, 100, 80}, 20, 0},
    {"J6 BASS", {VA_JUNO, 20, 100, 45, 40, 15, 0, 0}, {0, 50, 70, 20}, 40, 1},
    {"UB STRNG", {VA_UBXA, 30, 80, 80, 20, 5, 0, 0}, {40, 60, 100, 60}, 10, 0},
    {"UB SWEEP", {VA_UBXA, 50, 64, 40, 60, 40, 30, 0}, {10, 80, 80, 50}, 50, 0},
    {"UB BRASS", {VA_UBXA, 20, 100, 60, 30, 30, 0, 0}, {8, 60, 90, 30}, 40, 0},
    {"JP SAW", {VA_JP80, 80, 100, 100, 10, 0, 1, 0}, {2, 60, 110, 40}, 10, 0},
    {"JP LEAD", {VA_JP80, 50, 70, 90, 30, 20, 1, 0}, {0, 50, 100, 30}, 15, 1},
    {"JP TRANC", {VA_JP80, 100, 127, 70, 40, 30, 0, 0}, {0, 30, 60, 20}, 60, 0},
};

static const engine_t ENG_VA = {
    .name = "VA",
    .page_title = {"OSC", "FILT"},
    .edit = {
        {"MODEL", F_ENUM, 0, 2, 0, N_VA_MODEL, 0},
        {"PW", F_PCT, 0, 127, 64, 0},
        {"SUB", F_PCT, 0, 127, 0, 0},
        {"FREQ", F_CUTOFF, 0, 127, 80, 0},
        {"RES", F_PCT, 0, 127, 20, 0},
        {"HPF", F_PCT, 0, 127, 0, 0},
        {"CHOR", F_PCT, 0, 127, 0, 0},
        {"NOIS", F_PCT, 0, 127, 0, 0},
    },
    .presets = VA_PRESETS,
    .npresets = NELEM(VA_PRESETS),
    .note_on = va_note_on,
    .render = va_render,
    .knob = {P_E3, P_E4, P_E1, P_REL},
    .poly = VA_POLY,
    .keep = 0x0F,
    .desc = va_desc,
    .post = va_post,
    .units = va_units,
};
