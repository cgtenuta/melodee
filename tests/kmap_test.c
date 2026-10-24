/* SPDX-License-Identifier: GPL-3.0-only */
/* KNOB MAP test (src/kmap.c) on the UI of ui_test.c: unassigned encoders keep their jobs; an assigned one edits its
 * parameter (a VA part's track parameter, a Prophet part's native one) on any page; persistence in the favorites row;
 * the sheet cycles the codes; a code of the wrong engine does nothing. */
#define UI_TEST_NO_MAIN
#include "ui_test.c"
static int bad;
static void ck(const char *w, int ok) { printf("kmap: %-80s %s\n", w, ok ? "ok" : "FAIL"); bad += !ok; }
static void kreset(void)
{
    memset(favorites.factory[13], 0, 32);
    kmap_ready = 0;
    kmap_load();
}
int main(void)
{
    int16_t before;
    uint8_t save[NPART][KMAP_SLOTS];
    ui_power_on_general();
    set_engine_of(&trk[0], ENGI_VA);
    trk[0].engine = trk[0].eng_req;
    kreset();
    ck("fresh: all slots unassigned", kmap_slot[0][0] == KMAP_NONE && kmap_slot[3][6] == KMAP_NONE);
    before = trk[0].p[P_E3];
    turn(EN_SELECT, 3);
    ck("unassigned SELECT does not touch FREQ", trk[0].p[P_E3] == before);
    kmap_set(0, 4, 3);                                  /* SELECT -> E3 (FREQ) */
    kmap_set(0, 0, 4);                                  /* KNOB 1 -> E4 (RES) */
    ck("stored in favorites row 13 with its tag", kmap_tag_ok() && favorites.factory[13][4] == 3 && favorites.factory[13][0] == 4);
    before = trk[0].p[P_E3];
    turn(EN_SELECT, 3);
    ck("assigned SELECT raises FREQ", trk[0].p[P_E3] == before + 3);
    before = trk[0].p[P_E4];
    turn(EN_K1, -2);
    ck("assigned KNOB 1 lowers RES", trk[0].p[P_E4] == before - 2);
    before = trk[0].p[P_E3];
    turn(EN_K2, 1);
    ck("unassigned KNOB 2 leaves FREQ alone", trk[0].p[P_E3] == before);
    memcpy(save, kmap_slot, sizeof save);
    memset(kmap_slot, 0x11, sizeof kmap_slot);
    kmap_ready = 0;
    kmap_load();
    ck("reload from the favorites row restores the map", memcmp(save, kmap_slot, sizeof save) == 0);
    ck("other parts' maps are untouched", kmap_slot[1][0] == KMAP_NONE && kmap_slot[2][4] == KMAP_NONE);
    memset(favorites.factory[13], 0xA5, 32);
    kmap_ready = 0;
    kmap_load();
    ck("untagged garbage reads as unassigned", kmap_slot[0][0] == KMAP_NONE);
    kreset();
    kmap_sheet_open();
    ck("sheet opens with 8 rows", pop.on == POP_SHEET && pop.rows == SHEET_KMAP && pop.n == 8u);
    turn(EN_K1, 1);
    ck("sheet: KNOB 1 assigns the next parameter to slot 1 (MODEL)", kmap_slot[0][0] == 0 && trk[0].p[P_E0] == ENG_VA.edit[0].def * 0 + trk[0].p[P_E0]);
    {
        char b[16];
        SHEET_KMAP[0].value(b);
        ck("sheet value shows the label", strcmp(b, "MODEL") == 0);
    }
    turn(EN_K1, -1);
    ck("sheet: KNOB 1 back to unassigned", kmap_slot[0][0] == KMAP_NONE);
    pop_close();
    set_engine_of(&trk[0], ENGI_PROPHET);
    trk[0].engine = trk[0].eng_req;
    kreset();
    kmap_set(0, 1, KMAP_P5 + P5_CUTOFF);
    {
        int before5 = p5_patch_of(&trk[0])->raw[P5_CUTOFF];
        turn(EN_K2, 5);
        ck("Prophet part: KNOB 2 on the native CUTOFF", p5_patch_of(&trk[0])->raw[P5_CUTOFF] == before5 + 5);
    }
    ck("code labels", strcmp(kmap_label(&trk[0], KMAP_P5 + P5_RELEASE_AMP), "A REL") == 0);
    set_engine_of(&trk[0], ENGI_VA);
    trk[0].engine = trk[0].eng_req;
    ck("a Prophet code on a VA part is not usable", !kmap_usable(&trk[0], KMAP_P5 + P5_CUTOFF));
    kmap_auto();
    ck("engine defaults fill KNOB 1..4, navigation encoders stay free", kmap_slot[0][0] != KMAP_NONE && kmap_slot[0][6] == KMAP_NONE);
    ui_power_on();
    four_synths();
    ck("FOUR SYNTHS: 4 tracks, Prophet + 3 VA models, LEN 64 each",
       trk_n() == 4u && trk[0].eng_req == ENGI_PROPHET && trk[1].eng_req == ENGI_VA && trk[2].eng_req == ENGI_VA &&
       trk[3].eng_req == ENGI_VA && trk[1].p[P_E0] == 0 && trk[2].p[P_E0] == 1 && trk[3].p[P_E0] == 2 &&
       trk[0].p[P_SLEN] == 64 && trk[3].p[P_SLEN] == 64);
    printf("%s\n", bad ? "kmap: FAILURES" : "kmap: ALL PASSED");
    return bad != 0;
}
