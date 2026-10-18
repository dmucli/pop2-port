/* Extras (the "Extras" menu): cheats, level selection, quick save slots and rewind,
 * skipping the copy protection question.
 *
 * Everything here sits outside the ported game code and is inactive in deterministic
 * runs (tools/check.sh, tools/equiv.sh), so the verification of the port is unaffected.
 *
 * Save states are taken at the start of a game frame (the call of the one-frame function
 * from the level's frame loop). Play is always reached through the same native call chain
 * (ResetGame, Start, RestartGame, the frame loop), none of which keeps a C local alive
 * across the frame call, so restoring the emulated memory, the CPU registers and the
 * runtime's host-side state brings the game back exactly, in any level and in a later run
 * (the stack depth and the host state's layout are checked). Saved states go to disk with
 * a history (states.c); the rewind ring stays in memory. */
#include "mac.h"
#include "../cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern maddr g_a5;
extern int runtime_deterministic;
#include "../platform/platform.h"
#include "states.h"
#include "qd.h"
void event_menu_command_key(uint8_t ch);
void event_clear_queue(void);
extern uint32_t qd_screen_dirty, qd_clut_version;

/* ---- game globals (A5-relative; see docs/notes/seg02b.md, seg04b.md) ---- */
#define A5(off)          (g_a5 + (uint32_t)(int32_t)(off))
#define G_CHEAT_MODE     A5(-0x2824)    /* word: the game's built-in cheat mode */
#define G_MINUTES_LEFT   A5(-0x4fce)    /* word */
#define G_FRAMES_LEFT    A5(-0x4fc6)    /* word: frames left in the current minute */
#define G_KID            A5(-0x50d2)    /* the kid's character record (0x3e bytes) */
#define G_CUR_CHAR       A5(-0x5056)    /* the character being animated */
#define CH_LIFE          0x1e           /* word: life points */
#define CH_MAXLIFE       0x20           /* word: maximum life points */
#define G_COPY_OK        A5(-0x56d8)    /* word: copy protection passed this session */

/* ---- settings ---- */
static const char *pref_name[MOD_COUNT] = {
    "Extras.CheatMode", "Extras.InfiniteTime", "Extras.InfiniteHealth", "Extras.Rewind",
    "Extras.SkipCopyProtection" };
static const int pref_default[MOD_COUNT] = { 0, 0, 0, 1, 1 };   /* rewind and skip copy protection on */
static int mod[MOD_COUNT], prefs_loaded;
static int cheat_forced;                 /* we set the game's cheat flag */
static int saved_minutes = -1;

static void load_prefs(void) {
    if (prefs_loaded) return;
    prefs_loaded = 1;
    for (int i = 0; i < MOD_COUNT; i++) mod[i] = macos_pref_int(pref_name[i], pref_default[i]);
}
static int active(void) {
    static int check = -1;
    if (check < 0) check = getenv("POP2_CHECK") != NULL;
    return !runtime_deterministic && !check && g_a5 && rd32(LM_CurrentA5);
}
int mods_get(int m) { load_prefs(); return m >= 0 && m < MOD_COUNT ? mod[m] : 0; }
static void ring_clear(void);
void mods_set(int m, int v) {
    load_prefs();
    if (m < 0 || m >= MOD_COUNT) return;
    mod[m] = v;
    macos_pref_set_int(pref_name[m], v);
    if (v && (m == MOD_CHEAT_MODE || m == MOD_INFINITE_TIME || m == MOD_INFINITE_HEALTH)) mods_assisted();
    if (m == MOD_INFINITE_TIME) saved_minutes = -1;
    if (m == MOD_REWIND && !v) ring_clear();
    if (m == MOD_CHEAT_MODE && active()) {   /* takes effect at once */
        if (v) { wr16(G_CHEAT_MODE, 1); cheat_forced = 1; }
        else if (cheat_forced) { wr16(G_CHEAT_MODE, 0); cheat_forced = 0; }
    }
}

/* Next level: the game's own Command-L, which works on any level in cheat mode. */
void mods_next_level(void) {
    if (!active()) { macos_beep(); return; }
    if (!mod[MOD_CHEAT_MODE]) mods_set(MOD_CHEAT_MODE, 1);
    wr16(G_CHEAT_MODE, 1);
    event_menu_command_key('l');
}

/* Go to a level: the game's own mechanism (as Command-L), applied at the start of a frame. */
#define G_LEVEL          A5(-0x4fd2)    /* word: the level being played */
#define G_NEXT_LEVEL     A5(-0x4f92)    /* word: the level to go to (== level while playing it) */
uint32_t nat_ResetSoundItem(int16_t p4);
static int want_level, in_level;   /* in_level: a level's frame loop is running */
static int want_time, want_health;

/* ---- statistics ---- */
static struct { int session_time, session_deaths; } session[15];
static int att_level, att_clean, att_start, kid_alive, att_skipped;
static int game_clock(void) { return (int16_t)rd16(A5(-0x4fce)) * 720 + (int16_t)rd16(A5(-0x4fc6)); }   /* 720 a minute */
static int any_cheat(void) { load_prefs(); return mod[MOD_CHEAT_MODE] || mod[MOD_INFINITE_TIME] || mod[MOD_INFINITE_HEALTH]; }
void mods_assisted(void) { att_clean = 0; }
int  mods_attempt_clean(void) { return att_clean && !any_cheat(); }
static void stat_add(const char *what, int level, int v) {
    char k[32]; snprintf(k, sizeof k, "Stats.L%d.%s", level, what);
    macos_pref_set_int(k, macos_pref_int(k, 0) + v);
}
void mods_level_stats(int level, LevelStats *out) {
    memset(out, 0, sizeof *out);
    if (level < 1 || level > 14) return;
    char k[32];
    snprintf(k, sizeof k, "Stats.L%d.Best", level); out->best = macos_pref_int(k, 0);
    snprintf(k, sizeof k, "Stats.L%d.Deaths", level); out->deaths = macos_pref_int(k, 0);
    snprintf(k, sizeof k, "Stats.L%d.Done", level); out->completions = macos_pref_int(k, 0);
    out->session_time = session[level].session_time;
    out->session_deaths = session[level].session_deaths;
}
static void selftest_title_load(void);
void mods_goto_level(int level) {
    if (!mods_playing() || level < 1 || level > 14) { macos_beep(); return; }
    want_level = level;
    mods_assisted();
    att_skipped = 1;                                    /* not finished: left */
}
int mods_current_level(void) { return mods_playing() ? (int16_t)rd16(G_LEVEL) : 0; }

/* Add time / health: applied at the start of a frame. Time as the game's own '+' cheat key
 * (which adds one minute) and shows the time left; health: one life point, or one more
 * flask when the meter is full (12 at most, the game's own limit). */
#define G_MSG_ID         A5(-0x4fcc)    /* word: message being displayed */
#define G_MSG_TIMER      A5(-0x4fca)    /* word */
#define G_SHOW_TIME      A5(-0x4fa2)    /* word: request to display the time left */
#define ADD_MINUTES      5
#define MAX_LIFE         12
void mods_add_time(void) { if (mods_playing()) { want_time++; mods_assisted(); } else macos_beep(); }
void mods_add_health(void) { if (mods_playing()) { want_health++; mods_assisted(); } else macos_beep(); }
static void apply_add_time(void) {
    int m = (int16_t)rd16(G_MINUTES_LEFT) + ADD_MINUTES * want_time;
    want_time = 0;
    if (m > 99) m = 99;
    wr16(G_MINUTES_LEFT, (uint16_t)m);
    if (saved_minutes >= 0) saved_minutes = m;
    wr16(G_MSG_ID, 0); wr16(G_MSG_TIMER, 0); wr16(G_SHOW_TIME, 1);
}
/* As a potion does: the game's pending change of strength (applied by ChangeMeters this
 * frame, which also redraws the meter; the game clears it before each frame, before this
 * runs). When the meter is full, the maximum goes up first. */
#define G_KID_HP_DELTA   A5(-0x50b0)    /* word: pending change of the kid's strength */
static void apply_add_health(void) {
    maddr kid = G_KID, cur = G_CUR_CHAR;
    int life = (int16_t)rd16(kid + CH_LIFE), max = (int16_t)rd16(kid + CH_MAXLIFE);
    int delta = (int16_t)rd16(G_KID_HP_DELTA);
    int target = life + delta + want_health;
    want_health = 0;
    if (life <= 0) return;                                     /* not for a dead kid */
    if (target > max) {
        max = target < MAX_LIFE ? target : MAX_LIFE;
        if (max > (int16_t)rd16(kid + CH_MAXLIFE)) wr16(kid + CH_MAXLIFE, (uint16_t)max);
        if (rd16(cur) == rd16(kid)) wr16(cur + CH_MAXLIFE, (uint16_t)max);
    }
    if (target > max) target = max;
    if (target > life + delta) wr16(G_KID_HP_DELTA, (uint16_t)(target - life));
    char t[40]; snprintf(t, sizeof t, "Health %d/%d", target, max);
    platform_toast(t);
}

/* ---- copy protection ----
 * CopyProtectionDialog asks only while its "passed" flag is clear (and sets it itself);
 * with the option on, the flag is set as soon as the game has initialised its globals. */
void mods_tick(void) {
    load_prefs();
    selftest_title_load();
    if (!mod[MOD_SKIP_COPYPROT] || !active()) return;
    extern uint64_t runtime_us(void);
    if (runtime_us() < 2000000) return;   /* after the THINK C start-up has set up the globals */
    if (rd16(G_COPY_OK) != 1) wr16(G_COPY_OK, 1);
}

/* debug (POP2_MODS_SELFTEST=6): load the newest save state from the title screen */
static void selftest_title_load(void) {
    static int done;
    const char *t = getenv("POP2_MODS_SELFTEST");
    extern uint64_t runtime_us(void);
    if (done || !t || *t != '6' || !active() || runtime_us() < 20000000) return;
    done = 1;
    fprintf(stderr, "[mods] at the title (in level %d), loading the newest state (%d on disk)\n", in_level, states_count());
    states_request_load(0);
}

/* ---- save states ---- */
#define PG   4096u
#define NPG  (MEM_SIZE / PG)
typedef struct Pg { int refs; uint8_t d[PG]; } Pg;
typedef struct Snap { int used; uint64_t level; Pg **pg; uint8_t *host; size_t host_len; } Snap;
#define RING 150                          /* frames of rewind (~15 s at the game's ~10 fps) */
static Snap ring[RING]; static int ring_head, ring_count;
static uint64_t level_id = 1;
static int rewind_held, want_save;

static void snap_free(Snap *s) {
    if (!s->used) return;
    for (unsigned p = 0; p < NPG; p++) if (s->pg[p] && --s->pg[p]->refs == 0) free(s->pg[p]);
    free(s->pg); free(s->host);
    memset(s, 0, sizeof *s);
}
static void ring_clear(void) { for (int i = 0; i < RING; i++) snap_free(&ring[i]); ring_head = ring_count = 0; }
static Snap *ring_newest(void) { return ring_count ? &ring[(ring_head + RING - 1) % RING] : NULL; }

/* host-side runtime state, in a fixed order */
void *mm_state(size_t *n); void *file_state(int i, size_t *n); void *loader_state(size_t *n);
void *native_state(size_t *n);
size_t time_state_size(void); void time_state_save(uint8_t *o); void time_state_load(const uint8_t *i);
size_t rsrc_state_size(void); void rsrc_state_save(uint8_t *o); int rsrc_state_load(const uint8_t *i);
static void *region(int i, size_t *n) {
    switch (i) {
        case 0: return mm_state(n);
        case 1: case 2: case 3: return file_state(i - 1, n);
        case 4: return loader_state(n);
        case 5: return native_state(n);
        default: *n = 0; return NULL;
    }
}
#define NREGIONS 6
#define NREGS 17                          /* D0-D7, A0-A7, SR */

/* the host-side state: the core (registers, runtime tables, timers), then the resource
 * references (in-memory states only: states on disk keep those by file, see states.c) */
static size_t core_size(void) {
    size_t len = NREGS * 4 + time_state_size();
    for (int i = 0; i < NREGIONS; i++) { size_t n; region(i, &n); len += n; }
    return len;
}
static size_t host_size(void) { return core_size() + rsrc_state_size(); }
static void core_fill(uint8_t *o) {
    for (int r = 0; r < 16; r++) { put32(o, cpu_get(r)); o += 4; }
    put32(o, cpu_get(R_SR)); o += 4;
    for (int i = 0; i < NREGIONS; i++) { size_t n; void *p = region(i, &n); memcpy(o, p, n); o += n; }
    time_state_save(o);
}
static void host_fill(uint8_t *o) { core_fill(o); rsrc_state_save(o + core_size()); }
static void host_save(Snap *s) {
    s->host_len = host_size();
    s->host = malloc(s->host_len);
    host_fill(s->host);
}
static void core_load(const uint8_t *in) {
    for (int r = 0; r < 16; r++) cpu_set(r, be32(in + 4 * r));
    cpu_set(R_SR, be32(in + 64));
    in += NREGS * 4;
    for (int i = 0; i < NREGIONS; i++) { size_t n; void *p = region(i, &n); memcpy(p, in, n); in += n; }
    time_state_load(in);
}
static void host_load(const uint8_t *in) { core_load(in); rsrc_state_load(in + core_size()); }

static void snap_take(Snap *s, const Snap *base) {
    s->used = 1; s->level = level_id;
    s->pg = malloc(NPG * sizeof(Pg *));
    for (unsigned p = 0; p < NPG; p++) {
        const uint8_t *cur = M + (size_t)p * PG;
        if (base && base->pg[p] && !memcmp(base->pg[p]->d, cur, PG)) { s->pg[p] = base->pg[p]; s->pg[p]->refs++; continue; }
        Pg *n = malloc(sizeof(Pg)); n->refs = 1; memcpy(n->d, cur, PG); s->pg[p] = n;
    }
    host_save(s);
}
/* same point of the frame loop (stack depth), same set of resources: else refuse */
static int restorable(const uint8_t *host, size_t len) {
    return len == host_size() && be32(host + 15 * 4) == cpu_get(R_SP);
}
static void restore_done(const uint8_t *keys) {
    memcpy(M + LM_KeyMap, keys, 16);                    /* the keys held now stay held */
    event_clear_queue();
    qd_screen_dirty++; qd_clut_version++;               /* redraw with the restored colours */
}
static int snap_restore(const Snap *s) {
    if (!s->used || s->level != level_id || !restorable(s->host, s->host_len)) return 0;
    uint8_t keys[16];
    memcpy(keys, M + LM_KeyMap, 16);
    for (unsigned p = 0; p < NPG; p++) memcpy(M + (size_t)p * PG, s->pg[p]->d, PG);
    host_load(s->host);
    restore_done(keys);
    return 1;
}

/* for states.c: the core host-side state, and a restore from a flat memory image (the
 * caller then restores the resource references and open files its own way) */
uint8_t *mods_core_blob(size_t *len) {
    *len = core_size();
    uint8_t *b = malloc(*len);
    core_fill(b);
    return b;
}
int mods_core_restorable(const uint8_t *core, size_t len) {
    return len == core_size() && be32(core + 15 * 4) == cpu_get(R_SP);
}
void mods_restore_flat(const uint8_t *mem, const uint8_t *core) {
    uint8_t keys[16];
    memcpy(keys, M + LM_KeyMap, 16);
    memcpy(M, mem, MEM_SIZE);
    core_load(core);
    restore_done(keys);
}
int mods_minutes_left(void) { return (int16_t)rd16(G_MINUTES_LEFT); }

void mods_set_rewind_held(int held) { rewind_held = held; }
void mods_quick_save(void) {
    if (mods_playing()) want_save = 1;
    else { macos_beep(); platform_toast("Save states are made during play"); }
}
void mods_quick_load(void) {
    if (states_count() > 0) states_request_load(0);
    else { macos_beep(); platform_toast("No save state yet"); }
}

/* the level's frame loop starts: save states from another level are no longer valid */
void mods_level_start(void) {
    in_level = 1; level_id++; ring_clear(); saved_minutes = -1;
    att_level = (int16_t)rd16(G_LEVEL); att_clean = 1; att_start = game_clock(); kid_alive = 1; att_skipped = 0;
    if (!runtime_deterministic) {
        extern uint64_t runtime_us(void);
        fprintf(stderr, "[game] level %d%s starts (%llu s)\n", (int16_t)rd16(G_LEVEL), rd16(A5(-0x4ffe)) ? " (attract mode)" : "",
                (unsigned long long)(runtime_us() / 1000000));
    }
}

/* a level is being played: its frame loop is running (menus pause the game, so this is
 * not a matter of time since the last frame) */
#define G_DEMO           A5(-0x4ffe)    /* word: a recorded game is playing (attract mode) */
int mods_playing(void) { return active() && in_level && rd16(G_DEMO) == 0; }
int mods_in_level(void) { return active() && in_level; }
void mods_level_end(void) {
    in_level = 0; want_level = 0; want_time = want_health = 0;
    int lv = (int16_t)rd16(G_LEVEL), next = (int16_t)rd16(G_NEXT_LEVEL);
    if (!runtime_deterministic && rd16(G_DEMO) == 0 && lv == att_level && lv >= 1 && lv <= 14) {
        int secs = (att_start - game_clock()) / 12;
        if (secs > 0) session[lv].session_time += secs;
        if (!att_skipped && !cheat_forced && (next == lv + 1 || (lv == 14 && next > 14))) {   /* the level was finished */
            stat_add("Done", lv, 1);
            char k[32]; snprintf(k, sizeof k, "Stats.L%d.Best", lv);
            int best = macos_pref_int(k, 0);
            if (mods_attempt_clean() && secs > 0 && (!best || secs < best)) {
                macos_pref_set_int(k, secs);
                fprintf(stderr, "[stats] level %d: best time %d:%02d\n", lv, secs / 60, secs % 60);
            }
        }
    }
    if (!runtime_deterministic) fprintf(stderr, "[game] level %d ends, next %d\n", (int16_t)rd16(G_LEVEL), (int16_t)rd16(G_NEXT_LEVEL));
}

/* debug: the whole emulated memory but the keys held (FNV-1a) */
static uint64_t mem_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < MEM_SIZE; i++) {
        if (i - LM_KeyMap < 16) continue;
        h = (h ^ M[i]) * 1099511628211ull;
    }
    return h;
}

/* start of a game frame */
void mods_frame(void) {
    load_prefs();
    { static int once; if (!once++ && getenv("POP2_MODS_SELFTEST")) fprintf(stderr, "[mods] first frame, active %d\n", active()); }
    if (!active()) return;
    if (mod[MOD_CHEAT_MODE] && rd16(G_CHEAT_MODE) == 0) { wr16(G_CHEAT_MODE, 1); cheat_forced = 1; }
    if (mod[MOD_INFINITE_TIME]) {
        int m = (int16_t)rd16(G_MINUTES_LEFT);
        if (saved_minutes < 0 || m > saved_minutes) saved_minutes = m;   /* (cheat keys add time) */
        else if (m < saved_minutes) { wr16(G_MINUTES_LEFT, (uint16_t)saved_minutes); wr16(G_FRAMES_LEFT, 0x2cf); }
    }
    if (mod[MOD_INFINITE_HEALTH]) {
        maddr kid = G_KID, cur = G_CUR_CHAR;
        int16_t max = (int16_t)rd16(kid + CH_MAXLIFE);
        if (max > 0 && (int16_t)rd16(kid + CH_LIFE) > 0 && (int16_t)rd16(kid + CH_LIFE) < max) wr16(kid + CH_LIFE, (uint16_t)max);
        if (rd16(cur) == rd16(kid) && (int16_t)rd16(cur + CH_LIFE) > 0 && (int16_t)rd16(cur + CH_LIFE) < max)
            wr16(cur + CH_LIFE, (uint16_t)max);
    }
    if (mods_playing()) {                                /* deaths, for the statistics */
        int life = (int16_t)rd16(G_KID + CH_LIFE);
        if (kid_alive && life <= 0 && att_level >= 1 && att_level <= 14) {
            stat_add("Deaths", att_level, 1);
            session[att_level].session_deaths++;
        }
        kid_alive = life > 0;
    }
    static int selftest = -1, frame_no;
    if (selftest < 0) selftest = getenv("POP2_MODS_SELFTEST") != NULL;
    if (selftest) {   /* debug: quick save, load, rewind, with checks */
        frame_no++;
        static uint32_t sum_at_save; static maddr kid_at_save;
        uint32_t sum = 0; for (unsigned i = 0; i < 0x40000; i++) sum = sum * 31 + M[0x100000 + i];
        if (*getenv("POP2_MODS_SELFTEST") == '3') {   /* level selector: visit every level */
            static int lv_frames, target = -1;
            if (target < 0) target = getenv("POP2_MODS_LEVEL") ? atoi(getenv("POP2_MODS_LEVEL")) : 1;
            if (++lv_frames == 30) {
                fprintf(stderr, "[mods] in level %d, going to %d\n", (int16_t)rd16(A5(-0x4fd2)), target);
                mods_goto_level(target++); lv_frames = -100000;
            }
            if (rd16(A5(-0x4fd2)) == target - 1 && lv_frames < 0) lv_frames = 0;
            goto selftest_done;
        }
        if (*getenv("POP2_MODS_SELFTEST") == '2') {   /* cheats */
            if (frame_no == 1) { mod[MOD_INFINITE_TIME] = mod[MOD_INFINITE_HEALTH] = 1; }
            if (frame_no % 40 == 0) fprintf(stderr, "[mods] frame %d%s level %d minutes %d frames %d life %d/%d\n", frame_no,
                rd16(G_DEMO) ? " (demo)" : "",
                (int16_t)rd16(A5(-0x4fd2)), (int16_t)rd16(G_MINUTES_LEFT), (int16_t)rd16(G_FRAMES_LEFT),
                (int16_t)rd16(G_KID + CH_LIFE), (int16_t)rd16(G_KID + CH_MAXLIFE));
            if (frame_no == 60) { fprintf(stderr, "[mods] next level\n"); mods_next_level(); }
            goto selftest_done;
        }
        char t = *getenv("POP2_MODS_SELFTEST");
        if (t == '8') {   /* overlay pictures (POP2_OVERLAY_SHOTS) */
            extern void platform_overlay_shot(const char *name);
            if (frame_no == 20) want_save = 1;
            if (frame_no == 22) platform_overlay_shot("toast_saved");
            if (frame_no == 30) mods_add_health();
            if (frame_no == 32) platform_overlay_shot("toast_health");
            if (frame_no == 40) rewind_held = 1;
            if (frame_no == 44) platform_overlay_shot("rewind");
            if (frame_no == 48) rewind_held = 0;
            if (frame_no == 60) { extern void overlay_menu_request(void); overlay_menu_request(); }
            if (frame_no == 70) { fprintf(stderr, "[mods] overlay pictures done\n"); fflush(stderr); _exit(0); }
            goto selftest_done;
        }
        if (t == '9') {   /* add health when hurt; follow the counters */
            static int shot_at = -1, added_at = -1, prev = -1;
            int adding = added_at < 0 && (int16_t)rd16(G_KID + CH_LIFE) > 0 && (int16_t)rd16(G_KID + CH_LIFE) < (int16_t)rd16(G_KID + CH_MAXLIFE);
            if (frame_no % 4 == 0 && frame_no < 2000) {
                char path[256]; snprintf(path, sizeof path, "%s/f%05d_%d.ppm", getenv("POP2_SHOTDIR"), frame_no, (int16_t)rd16(G_KID + CH_LIFE));
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", qd_screen_w, qd_screen_h);
                    for (int i = 0; i < qd_screen_w * qd_screen_h; i++) {
                        uint32_t c = qd_device_rgb(rd8(SCREEN_BASE + (uint32_t)i));
                        uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
                        fwrite(px, 1, 3, f);
                    }
                    fclose(f);
                }
            }
            int life = (int16_t)rd16(G_KID + CH_LIFE), max = (int16_t)rd16(G_KID + CH_MAXLIFE);
            int cur = (int16_t)rd16(G_CUR_CHAR + CH_LIFE), hp = (int16_t)rd16(A5(-0x50b4)), delta = (int16_t)rd16(A5(-0x50b0));
            if (life != prev || (added_at >= 0 && frame_no - added_at < 6))
                fprintf(stderr, "[mods] frame %d: kid life %d/%d, current char life %d (char %d, kid %d), delta %d\n",
                        frame_no, life, max, cur, rd16(G_CUR_CHAR), rd16(G_KID), delta), (void)hp;
            prev = life;
            if (added_at < 0 && life > 0 && life < max) { fprintf(stderr, "[mods] hurt: add one health\n"); added_at = frame_no; shot_at = frame_no; }
            if (added_at >= 0 && frame_no == added_at + 40) { fprintf(stderr, "[mods] frame %d: add one health\n", frame_no); mods_add_health(); }
            if (added_at >= 0 && frame_no == added_at + 80) { fprintf(stderr, "[mods] frame %d: add one health (twice)\n", frame_no); mods_add_health(); mods_add_health(); }
            if (frame_no > 4000) _exit(0);
            goto selftest_done;
        }
        if (t == '7') {   /* add time and health */
            if (frame_no == 30 || frame_no == 32 || frame_no == 60 || frame_no == 62)
                fprintf(stderr, "[mods] frame %d: minutes %d, life %d/%d\n", frame_no, (int16_t)rd16(G_MINUTES_LEFT),
                        (int16_t)rd16(G_KID + CH_LIFE), (int16_t)rd16(G_KID + CH_MAXLIFE));
            if (frame_no == 31) mods_add_time();
            if (frame_no == 61) { mods_add_health(); mods_add_health(); mods_add_health(); }
            goto selftest_done;
        }
        if (t == '4' || t == '5' || t == '6') {   /* save states across levels (4) and across runs (5: loads the newest) */
            static int lv_frames, phase, saved_level;
            lv_frames++;
            int lv = (int16_t)rd16(G_LEVEL);
            if (t == '4' && phase == 0 && frame_no == 30) {
                want_save = 1; sum_at_save = sum; kid_at_save = rd32(G_KID + 2); saved_level = lv;
                fprintf(stderr, "[mods] save in level %d sum %08x\n", lv, sum);
                phase = 1;
            } else if (t == '4' && phase == 1 && frame_no == 40) {
                fprintf(stderr, "[mods] going to level %d\n", lv + 1);
                mods_goto_level(lv + 1); phase = 2; lv_frames = -100000;
            } else if (t == '4' && phase == 2 && lv == saved_level + 1 && lv_frames < 0) lv_frames = 0;
            else if (t == '4' && phase == 2 && lv_frames == 30) {
                fprintf(stderr, "[mods] in level %d, loading the state\n", lv);
                states_request_load(0); phase = 3; lv_frames = 0;
            } else if (t == '5' && phase == 0 && frame_no == 30) {
                fprintf(stderr, "[mods] in level %d, loading the newest state (%d on disk)\n", lv, states_count());
                states_request_load(0); phase = 3; lv_frames = 0;
            } else if (t == '6' && phase == 0) {
                fprintf(stderr, "[mods] first frame of play in level %d\n", lv);
                phase = 3; lv_frames = 0;
            } else if (phase == 3 && lv_frames == 1) {
                fprintf(stderr, "[mods] after load: level %d sum %08x%s, kid %s\n", lv, sum,
                        t == '4' ? (sum == sum_at_save ? " (same)" : " (DIFFERENT)") : "",
                        t == '4' ? (rd32(G_KID + 2) == kid_at_save ? "same" : "DIFFERENT") : "-");
                phase = 4;
            } else if (phase == 4 && lv_frames % 50 == 0)
                fprintf(stderr, "[mods] frame %d after load: level %d, kid at %08x, life %d, minutes %d\n", lv_frames, lv,
                        rd32(G_KID + 2), (int16_t)rd16(G_KID + CH_LIFE), (int16_t)rd16(G_MINUTES_LEFT));
            goto selftest_done;
        }
        if (frame_no == 30) { want_save = 1; sum_at_save = sum; kid_at_save = rd32(G_KID + 2); }
        if (frame_no == 90) states_request_load(0);
        if (frame_no == 91) fprintf(stderr, "[mods] after load: globals %s, kid %s\n",
                                    sum == sum_at_save ? "same" : "DIFFERENT", rd32(G_KID + 2) == kid_at_save ? "same" : "DIFFERENT");
        if (frame_no == 150) rewind_held = 1;
        if (frame_no == 180) { rewind_held = 0; fprintf(stderr, "[mods] rewound, ring %d\n", ring_count); }
        if (frame_no % 50 == 0) fprintf(stderr, "[mods] frame %d level %d ring %d\n", frame_no, (int16_t)rd16(A5(-0x4fd2)), ring_count);
    }
    selftest_done:
    if (want_time) apply_add_time();
    if (want_health) apply_add_health();
    if (want_level) {
        int l = want_level; want_level = 0;
        char t[24]; snprintf(t, sizeof t, "Level %d", l);
        platform_toast(t);
        if (rd16(G_NEXT_LEVEL) == rd16(G_LEVEL)) {   /* not already leaving the level */
            wr16(G_NEXT_LEVEL, (uint16_t)l);
            nat_ResetSoundItem(0);                    /* as Command-L: stop the level's sounds */
        }
        return;
    }
    if (states_load_pending()) {
        if (!states_load_now()) { macos_beep(); platform_toast("This save state cannot be loaded"); }
        else {
            ring_clear();
            att_level = (int16_t)rd16(G_LEVEL); att_start = game_clock(); att_clean = 0;
            kid_alive = (int16_t)rd16(G_KID + CH_LIFE) > 0;
            platform_toast("State loaded");
        }
        if (selftest) fprintf(stderr, "[mods] memory hash after load %016llx\n", (unsigned long long)mem_hash());
        return;
    }
    if (want_save) {
        want_save = 0;
        if (selftest) fprintf(stderr, "[mods] memory hash at save %016llx\n", (unsigned long long)mem_hash());
        if (rd16(G_DEMO) == 0) { states_save_now(mods_current_level(), mods_minutes_left()); platform_toast("State saved"); }
    }
    platform_rewinding(rewind_held && mod[MOD_REWIND]);
    if (rewind_held && mod[MOD_REWIND]) {
        if (ring_count <= 1) return;                     /* nothing older */
        ring_head = (ring_head + RING - 1) % RING; ring_count--;
        snap_free(&ring[ring_head]);                     /* drop the newest */
        if (!snap_restore(ring_newest())) { ring_clear(); macos_beep(); }
        else { mods_assisted(); kid_alive = (int16_t)rd16(G_KID + CH_LIFE) > 0; }
        return;
    }
    if (mod[MOD_REWIND]) {
        Snap *base = ring_newest();
        if (ring_count == RING) { snap_free(&ring[ring_head]); ring_count--; }
        Snap tmp = {0};
        snap_take(&tmp, base);
        ring[ring_head] = tmp;
        ring_head = (ring_head + 1) % RING; ring_count++;
    }
}
