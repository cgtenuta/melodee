/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* KNOB MAP: any parameter on any of the seven encoders, per part.
 *
 * Each part (1..4) keeps a map of 7 slots: KNOB 1..4, SELECT, PRESET, ALGORITHM. A slot holds a code:
 *   0xFF       unassigned: the encoder does its normal job (nothing changes until a slot is assigned)
 *   0..31      KMAP_T[code]: one of the part's own track parameters (the engine's E0..E7, the envelope, LFO depths...)
 *   32..119    32 + id: the Prophet's native panel parameter id (P5_PANEL), for a Prophet part only
 * An assigned encoder edits that parameter of the SELECTED part on every page (not while a menu, sheet or question
 * is open). The codes are stored, so the list is append-only.
 *
 * The map lives with the favorites (favorites.c, row 13, retired SLICE's: bytes 0..27, tag 'K' 'M' 'P' 1 in 28..31),
 * so the settings record keeps its layout. Set it from the action sheet (OCT+ held on a sound page, "Knob map"):
 * KNOB 2 the slot, KNOB 1 its parameter, OCT+ clears the slot. */
#define KMAP_SLOTS 7u
#define KMAP_NONE 0xFFu
#define KMAP_P5 32u
#define KMAP_P5_END 120u
static const uint8_t KMAP_T[32] = {
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
    P_ATK, P_DEC, P_SUS, P_REL, P_LEVEL, P_PAN,
    P_ED_FLT, P_ED_PIT, P_ED_SHP,
    P_LRATE, P_LD_PIT, P_LD_FLT, P_LD_SHP, P_LD_AMP,
    P_GLIDE, P_DETUNE, P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL, P_LEVEL,   /* (24..31 spare: = LEVEL) */
};
#define KMAP_T_USED 24u
static uint8_t kmap_slot[NPART][KMAP_SLOTS];
static uint8_t kmap_ready;
static const uint8_t KMAP_ROLE[KMAP_SLOTS] = {EN_K1, EN_K2, EN_K3, EN_K4, EN_SELECT, EN_PRESET, EN_ALGO};

static int kmap_tag_ok(void)
{
    const uint8_t *r = favorites.factory[13];
    return r[28] == 'K' && r[29] == 'M' && r[30] == 'P' && r[31] == 1;
}
static void kmap_load(void)
{
    uint32_t p, k;
    if (kmap_ready)
        return;
    kmap_ready = 1;
    memset(kmap_slot, KMAP_NONE, sizeof kmap_slot);
    if (!kmap_tag_ok())
        return;
    for (p = 0; p < NPART; p++)
        for (k = 0; k < KMAP_SLOTS; k++) {
            uint8_t c = favorites.factory[13][p * KMAP_SLOTS + k];
            kmap_slot[p][k] = (c < KMAP_T_USED || (c >= KMAP_P5 && c < KMAP_P5_END)) ? c : KMAP_NONE;
        }
}
static void kmap_store(void)
{
    uint32_t p, k;
    uint8_t *r = favorites.factory[13];
    for (p = 0; p < NPART; p++)
        for (k = 0; k < KMAP_SLOTS; k++)
            r[p * KMAP_SLOTS + k] = kmap_slot[p][k];
    r[28] = 'K'; r[29] = 'M'; r[30] = 'P'; r[31] = 1;
    settings_save();
}
static uint32_t kmap_part(void) { return song.sel % NPART; }

/* a code usable on the part now? (a Prophet id needs a Prophet part with a panel label) */
static int kmap_usable(const track_t *t, uint32_t c)
{
    if (c < KMAP_T_USED)
        return 1;
    if (c >= KMAP_P5 && c < KMAP_P5_END) {
        uint32_t id = c - KMAP_P5;
        return t->eng_req == ENGI_PROPHET && id < NELEM(P5_PANEL) && P5_PANEL[id].label && P5_PANEL[id].fmt != F_ONOFF && id != P5_UNISON &&
               id != P5_UNISON_COUNT && id != P5_BEND && !p5_keyed_tune(t, id);
    }
    return 0;
}
static const param_desc_t *kmap_desc(const track_t *t, uint32_t c)
{
    if (c < KMAP_T_USED)
        return param_desc_of(t->eng_req % NENGINES, KMAP_T[c]);
    return &P5_PANEL[c - KMAP_P5];
}
static const char *kmap_label(const track_t *t, uint32_t c)
{
    if (c == KMAP_NONE)
        return "-";
    if (c < KMAP_T_USED && ENGINES[t->eng_req % NENGINES]->desc && KMAP_T[c] >= P_E0 && KMAP_T[c] <= P_E7) {
        const param_desc_t *d = ENGINES[t->eng_req % NENGINES]->desc(t, KMAP_T[c] - P_E0);
        if (d)
            return d->label;
    }
    if (c >= KMAP_P5) {                                  /* the envelopes' twins, told apart */
        static const struct { uint8_t id; const char *l; } TW[] = {
            {P5_ATTACK_FILTER, "F ATK"}, {P5_DECAY_FILTER, "F DEC"}, {P5_SUSTAIN_FILTER, "F SUS"}, {P5_RELEASE_FILTER, "F REL"},
            {P5_ATTACK_AMP, "A ATK"}, {P5_DECAY_AMP, "A DEC"}, {P5_SUSTAIN_AMP, "A SUS"}, {P5_RELEASE_AMP, "A REL"},
            {P5_ENV_FILTER, "F ENV"}, {P5_POLY_ENV, "PM ENV"}, {P5_POLY_B, "PM OSCB"}, {P5_LEVEL_A, "MIX A"}, {P5_LEVEL_B, "MIX B"}};
        uint32_t i;
        for (i = 0; i < NELEM(TW); i++)
            if (TW[i].id == c - KMAP_P5)
                return TW[i].l;
    }
    return kmap_desc(t, c)->label;
}

/* the next / previous usable code after c (none is between the ends): dir > 0 / < 0 */
static uint8_t kmap_next(const track_t *t, uint8_t c, int dir)
{
    int32_t i = c == KMAP_NONE ? -1 : (int32_t)c, n;
    for (n = 0; n < (int32_t)KMAP_P5_END + 1; n++) {
        i += dir > 0 ? 1 : -1;
        if (i < 0 || i >= (int32_t)KMAP_P5_END)
            return KMAP_NONE;
        if (kmap_usable(t, (uint32_t)i))
            return (uint8_t)i;
    }
    return KMAP_NONE;
}
static void kmap_set(uint32_t part, uint32_t slot, uint8_t c)
{
    kmap_load();
    kmap_slot[part % NPART][slot % KMAP_SLOTS] = c;
    kmap_store();
}
static void kmap_auto(void)                              /* the engine's four HOME knobs on KNOB 1..4, the rest cleared
                                                         * (SELECT, PRESET and ALGORITHM keep their navigation jobs) */
{
    const track_t *t = TSEL;
    uint32_t k, c, p = kmap_part();
    kmap_load();
    for (k = 0; k < KMAP_SLOTS; k++)
        kmap_slot[p][k] = KMAP_NONE;
    if (t->eng_req == ENGI_PROPHET) {                    /* cutoff, resonance, filter env amount, release */
        static const uint8_t PRO[4] = {P5_CUTOFF, P5_RESONANCE, P5_ENV_FILTER, P5_RELEASE_AMP};
        for (k = 0; k < 4u; k++)
            if (kmap_usable(t, KMAP_P5 + PRO[k]))
                kmap_slot[p][k] = (uint8_t)(KMAP_P5 + PRO[k]);
    } else {
        for (k = 0; k < 4u; k++)
            for (c = 0; c < KMAP_T_USED; c++)
                if (KMAP_T[c] == ENGINES[t->eng_req % NENGINES]->knob[k]) {
                    kmap_slot[p][k] = (uint8_t)c;
                    break;
                }
    }
    kmap_store();
}

/* a detent of slot's encoder: 1 = taken. Not while a menu / sheet / question is up (the caller checks) */
static int kmap_turn(uint32_t slot, int32_t s)
{
    track_t *t = TSEL;
    uint8_t c;
    const param_desc_t *d;
    int32_t v, nv;
    char b[24], u[12];
    kmap_load();
    c = kmap_slot[kmap_part()][slot];
    if (c == KMAP_NONE || !kmap_usable(t, c))
        return 0;
    d = kmap_desc(t, c);
    if (c < KMAP_T_USED) {
        int16_t *vp = &t->p[KMAP_T[c]];
        if (d->max == d->min)
            return 1;
        v = *vp;
        nv = enum_step(d, v, clamp(v + accel(KMAP_ROLE[slot], s, desc_range(d)), d->min, d->max));
        *vp = (int16_t)nv;
        if (!momentary.active && !designer_on()) {
            motion_capture(t, KMAP_T[c], *vp);
            if (motion_param(KMAP_T[c]))
                motion_unplayed();
        }
    } else {
        uint32_t id = c - KMAP_P5;
        uint8_t old[MO_NATIVE_N];
        uint32_t f = motion_guard();
        v = p5_patch_of(t)->raw[id];
        nv = clamp(v + accel(KMAP_ROLE[slot], s, desc_range(d)), d->min, d->max);
        motion_native_peek(t, old);
        p5_edit_value(t, id, p5_panel_raw(t, id, nv));
        native_motion(old);
        motion_unguard(f);
    }
    str_cpy(b, kmap_label(t, c), 10);
    ui_message(b);
    (void)u;
    return 1;
}
/* every assigned encoder this frame; returns the slots taken (bit mask) */
static uint32_t kmap_encoders(void)
{
    uint32_t k, taken = 0;
    int32_t s;
    kmap_load();
    for (k = 0; k < KMAP_SLOTS; k++) {
        if (kmap_slot[kmap_part()][k] == KMAP_NONE || !kmap_usable(TSEL, kmap_slot[kmap_part()][k]))
            continue;
        s = panel_enc(KMAP_ROLE[k]);
        if (s && kmap_turn(k, s))
            taken |= 1u << k;
    }
    return taken;
}

/* ------------------------------------------------------------ the sheet --- */
#define KMAP_ROW(n) \
    static void kmap_v##n(char *b) { str_cpy(b, kmap_label(TSEL, kmap_slot[kmap_part()][n]), 10); } \
    static void kmap_t##n(int32_t s) { kmap_load(); kmap_set(kmap_part(), n, kmap_next(TSEL, kmap_slot[kmap_part()][n], s)); ui.force = 1; } \
    static void kmap_c##n(void) { kmap_set(kmap_part(), n, KMAP_NONE); ui.force = 1; }
KMAP_ROW(0) KMAP_ROW(1) KMAP_ROW(2) KMAP_ROW(3) KMAP_ROW(4) KMAP_ROW(5) KMAP_ROW(6)
static const sheet_row_t SHEET_KMAP[] = {
    {"Knob 1", kmap_v0, kmap_t0, kmap_c0, 0}, {"Knob 2", kmap_v1, kmap_t1, kmap_c1, 0},
    {"Knob 3", kmap_v2, kmap_t2, kmap_c2, 0}, {"Knob 4", kmap_v3, kmap_t3, kmap_c3, 0},
    {"Select", kmap_v4, kmap_t4, kmap_c4, 0},  {"Preset", kmap_v5, kmap_t5, kmap_c5, 0},
    {"Algorithm", kmap_v6, kmap_t6, kmap_c6, 0}, {"Engine defaults", 0, 0, kmap_auto, 0}};

/* FOUR SYNTHS: tracks 1..4 as a Prophet-5, a Juno-6, a UB-Xa and a JP-8080 sound (each with its own 64-step
 * sequence: LEN 64; the steps themselves stay as they are). Presets: Prophet 1 (the first factory program), VA
 * J6 BRASS, UB STRNG, JP SAW (VA_PRESETS order) */
static void four_synths(void)
{
    static const uint8_t ENG[4] = {ENGI_PROPHET, ENGI_VA, ENGI_VA, ENGI_VA}, PRE[4] = {1, 0, 3, 6};
    uint32_t k;
    if (chain_busy()) {
        ui_message("STOP TO EDIT");
        return;
    }
    browse_commit();
    momentary_restore();
    for (k = trk_n(); k < 4u; k++) {
        track_blank(k);
        song.ntrk = (uint8_t)(k + 1u);
    }
    for (k = 0; k < 4u; k++) {
        engine_load_of(&trk[k], ENG[k], PRE[k]);
        trk[k].p[P_SLEN] = NSTEP;
    }
    track_select(0);
    ui.force = 1;
    ui_message("FOUR SYNTHS");
}
static void kmap_sheet_open(void)
{
    char t[24] = "Knob map P";
    kmap_load();
    t[10] = (char)('1' + kmap_part());
    t[11] = 0;
    sheet_open(t, "turn K1: param", SHEET_KMAP, NELEM(SHEET_KMAP));
}
