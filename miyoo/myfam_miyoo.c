/* SPDX-License-Identifier: MPL-2.0 */
/* Copyright (C) 2026 David Amo. My-FAM. */
/* My-FAM (by David Amo), on the Miyoo Mini Plus: the host simulator (host/plat_host.c, by way of host/rfm1.c's real-time entry points)
 * with the handheld's buttons, screen and sound in place of the FM-1's panel.
 *
 *   python3 tools/build.py fam --miyoo      ->  fam/build/miyoo/fm-fam      (CC, MIYOO_CFLAGS, MIYOO_LIBS)
 *
 * The screen: the FM-1's 240x240 at 2x on the left of the 640x480 panel, a side panel of 160 px on the right with the
 * layer that is active, the lights of the mode buttons and the keys, and the key cursor.
 *
 * The controls (layers: what a button does is decided when it goes down, so OCT- held before SELECT stays OCT-):
 *   BASE    Y X A B held + D-pad: knobs K1 K2 K3 K4 (right/up +, left/down -; up/down 4 clicks at a time)
 *           D-pad alone: left/right ALGORITHM, up/down SELECT (page)
 *           START PLAY   L1 OCT-   R1 OCT+   R2 REC
 *   MODES   SELECT held: up SEQ, right EDIT, down KICK, left LFO, X SCL, A FX, B PATTERNS, Y SAVE, L1 SYS,
 *           L2 / R2 PRESETS - / +
 *   KEYS    L2 held: D-pad moves the key cursor (left/right along a row, up the black keys, down the white keys),
 *           A plays the key while held, X taps it and moves on, B holds it (latched) until B again
 *   MENU    help (START there quits, saving; left/right: MASTER)
 *
 * The clock: the sound card's. The main loop runs the simulator one millisecond at a time while the audio ring has
 * room (rfm1.c's contract: tick, then frame every 16 ms or idle, then the audio that fell due), so My-FAM's time
 * is the sound's. Saved data: the 1 MiB flash image, in a file (MYFAM_SAVE, default ./my-fam.flash), written by a
 * thread of its own a little after the app last changed it, and on the way out.
 *
 * Backends: MIYOO_SDL (SDL 1.2: the Miyoo's, a desktop's for trying it) or the headless test runner (a script of
 * button presses, screenshots and a WAV: miyoo/tests). */
#define export_name(x) unused           /* (rfm1.c's WebAssembly exports: plain functions here) */
#include "rfm1.c"
#undef getenv                           /* (rfm1.c hides the environment from a host app: My-FAM reads its own) */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "font5x7.h"
#include <dirent.h>
#include <sys/syscall.h>

/* ------------------------------------------------------------ threads --- */
/* every thread of the process, by what started it (MAIN, SDL: SDL_Init's, AUDIO: SDL_OpenAudio's, SAVE), and its CPU
 * (the help shows them: which one is busy) */
enum { TH_MAIN, TH_SDL, TH_AUDIO, TH_SAVE, TH_BATT, TH_OTHER, TH_KINDS };
static const char *const TH_NAME[TH_KINDS] = {"MAIN", "SDL", "AUDIO", "SAVE", "BATT", "OTHER"};
#define TH_MAX 16
static struct { int tid, kind; unsigned long long ticks; } th[TH_MAX];
static volatile int saver_ready;
static int th_n, th_pct[TH_KINDS] = {-1, -1, -1, -1, -1, -1};
static pthread_mutex_t th_mx = PTHREAD_MUTEX_INITIALIZER;
static int th_find(int tid)
{
    for (int i = 0; i < th_n; i++)
        if (th[i].tid == tid)
            return i;
    return -1;
}
static void th_add(int tid, int kind)
{
    pthread_mutex_lock(&th_mx);
    if (th_find(tid) < 0 && th_n < TH_MAX)
        th[th_n].tid = tid, th[th_n].kind = kind, th[th_n].ticks = 0, th_n++;
    pthread_mutex_unlock(&th_mx);
}
static void th_self(int kind) { th_add((int)syscall(SYS_gettid), kind); }
static void th_scan(int kind)           /* the threads not seen yet: started by `kind` */
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
            th_add(atoi(e->d_name), kind);
    closedir(d);
}


#define SCR_W 640
#define SCR_H 480
#define PANEL_X 480
#define RATE 44100

/* --------------------------------------------------------------- buttons --- */
enum { MB_UP, MB_DOWN, MB_LEFT, MB_RIGHT, MB_A, MB_B, MB_X, MB_Y, MB_L1, MB_R1, MB_L2, MB_R2, MB_SELECT, MB_START,
       MB_MENU, MB_N };
static const char *const MB_NAMES[MB_N] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
                                           "SELECT", "START", "MENU"};
#define MB(b) (1u << (b))
#define MB_DPAD (MB(MB_UP) | MB(MB_DOWN) | MB(MB_LEFT) | MB(MB_RIGHT))
#define MB_FACE (MB(MB_A) | MB(MB_B) | MB(MB_X) | MB(MB_Y))

/* what a button does while it is down: an FM-1 button (label), a key, an encoder's clicks, or the front end's own */
enum { ACT_NONE, ACT_BTN, ACT_KEY, ACT_ENC, ACT_KNOB, ACT_CURSOR, ACT_KEYTAP, ACT_LATCH, ACT_HELP, ACT_QUIT, ACT_MASTER,
       ACT_IGNORE };
typedef struct {
    uint8_t act, arg;                   /* ACT_BTN: label; ACT_KEY: key; ACT_ENC: role; ACT_KNOB: face (0..3: K1..K4) */
    int8_t dir;                         /* clicks a step */
    uint32_t down_ms, next_ms;          /* repeats */
} mb_role_t;
static mb_role_t role[MB_N];
static uint32_t mb_held;
static uint32_t g_ms;                   /* the simulator's milliseconds: the front end's clock too */
static int help_on, quit_req, menu_alone;
static int cur_row, cur_idx;            /* the key cursor: row 0 the white keys (0..15), 1 the black keys (0..10) */
static int latched = -1;                /* the key B holds, -1 none */
static int master_pct = 80;
static uint32_t knob_flash_ms[4];       /* when each knob last moved (the side panel lights it) */
static const uint8_t WHITE[16] = PLAT_WHITE_KEYS, BLACK[11] = PLAT_BLACK_KEYS;

static int cur_key(void) { return cur_row ? BLACK[cur_idx] : WHITE[cur_idx]; }
static void btn_set(int label, int on)
{
    uint32_t bit = 1u << RF_BTN_ID[label];
    if (on) {
        rf_panel.buttons |= bit;
        rf_panel.buttons_tap |= bit;    /* (a press and a release between two ticks still counts) */
    } else
        rf_panel.buttons &= ~bit;
}
static uint32_t keys_by_role(void)
{
    uint32_t k = 0;
    for (int b = 0; b < MB_N; b++)
        if (role[b].act == ACT_KEY)
            k |= 1u << role[b].arg;
    if (latched >= 0)
        k |= 1u << latched;
    return k;
}
static void keys_publish(uint32_t tap)
{
    rf_panel.keys = keys_by_role();
    rf_panel.keys_tap |= tap;
}
static void enc_add(int r, int n) { rf_panel.enc[RF_ENC_ID[r]] += n; }
static void cursor_move(int b)
{
    if (b == MB_LEFT || b == MB_RIGHT) {
        int n = cur_row ? 11 : 16;
        cur_idx = (cur_idx + (b == MB_RIGHT ? 1 : n - 1)) % n;
    } else if (b == MB_UP && !cur_row) {
        /* the black key nearest the white one (to its right first) */
        int k = WHITE[cur_idx], best = 0;
        for (int i = 0; i < 11; i++)
            if (abs(BLACK[i] - k - 1) < abs(BLACK[best] - k - 1))
                best = i;
        cur_row = 1, cur_idx = best;
    } else if (b == MB_DOWN && cur_row) {
        int k = BLACK[cur_idx];
        for (int i = 0; i < 16; i++)
            if (WHITE[i] == k - 1)
                cur_idx = i;
        cur_row = 0;
    }
}
/* one step of a held button that repeats */
static void role_step(int b)
{
    mb_role_t *r = &role[b];
    if (r->act == ACT_ENC)
        enc_add(r->arg, r->dir);
    else if (r->act == ACT_KNOB) {
        enc_add(EN_K1 + r->arg, r->dir);
        knob_flash_ms[r->arg] = g_ms ? g_ms : 1;
    } else if (r->act == ACT_CURSOR)
        cursor_move(r->arg);
    else if (r->act == ACT_MASTER) {
        master_pct = clamp(master_pct + r->dir, 0, 100);
        rf_panel.master = master_pct * 1023 / 100;
    }
}
static int face_knob(void)              /* the knob of the face button held first, -1 none (Y X A B: K1..K4) */
{
    static const uint8_t F[4] = {MB_Y, MB_X, MB_A, MB_B};
    int best = -1;
    uint32_t t = 0;
    for (int i = 0; i < 4; i++)
        if ((mb_held & MB(F[i])) && role[F[i]].act == ACT_IGNORE && (best < 0 || role[F[i]].down_ms < t))
            best = i, t = role[F[i]].down_ms;
    return best;
}

static void mb_down(int b)
{
    mb_role_t *r = &role[b];
    int dpad = b <= MB_RIGHT;
    int sign = (b == MB_RIGHT || b == MB_UP) ? 1 : -1;
    mb_held |= MB(b);
    memset(r, 0, sizeof *r);
    r->down_ms = g_ms;
    r->next_ms = g_ms + 350u;
    if (help_on) {                      /* the help: START quits, left/right MASTER, MENU or B closes */
        if (b == MB_START)
            r->act = ACT_QUIT, quit_req = 1;
        else if (b == MB_LEFT || b == MB_RIGHT)
            r->act = ACT_MASTER, r->dir = (int8_t)(b == MB_RIGHT ? 5 : -5), role_step(b);
        else if (b == MB_MENU || b == MB_B)
            r->act = ACT_HELP, help_on = 0, menu_alone = 0;
        else
            r->act = ACT_IGNORE;
        return;
    }
    if (mb_held & MB(MB_MENU) && b != MB_MENU) {   /* MENU + D-pad: the system's (volume, brightness) */
        r->act = ACT_IGNORE;
        menu_alone = 0;
        return;
    }
    if (b == MB_MENU) {
        r->act = ACT_HELP;
        menu_alone = 1;
        return;
    }
    if (mb_held & MB(MB_SELECT) && b != MB_SELECT) {   /* MODES */
        static const int8_t MODE_BTN[MB_N] = {
            [MB_UP] = B_SEQ, [MB_RIGHT] = B_EDIT, [MB_DOWN] = B_ARP, [MB_LEFT] = B_LFO, [MB_X] = B_SEL, [MB_A] = B_FX,
            [MB_B] = B_GLO, [MB_Y] = B_SAVE, [MB_L1] = B_ENV, [MB_R1] = -1, [MB_L2] = -1, [MB_R2] = -1,
            [MB_START] = -1, [MB_SELECT] = -1, [MB_MENU] = -1};
        if (b == MB_L2 || b == MB_R2) {
            r->act = ACT_ENC, r->arg = EN_PRESET, r->dir = (int8_t)(b == MB_R2 ? 1 : -1);
            role_step(b);
        } else if (MODE_BTN[b] >= 0) {
            r->act = ACT_BTN, r->arg = (uint8_t)MODE_BTN[b];
            btn_set(r->arg, 1);
        } else
            r->act = ACT_IGNORE;
        return;
    }
    if (mb_held & MB(MB_L2) && b != MB_L2) {           /* KEYS */
        if (dpad) {
            r->act = ACT_CURSOR, r->arg = (uint8_t)b;
            cursor_move(b);
        } else if (b == MB_A) {
            r->act = ACT_KEY, r->arg = (uint8_t)cur_key();
            keys_publish(1u << r->arg);
        } else if (b == MB_X) {
            r->act = ACT_KEYTAP, r->arg = (uint8_t)cur_key();
            keys_publish(1u << r->arg);
            rf_panel.keys_tap |= 1u << r->arg;
        } else if (b == MB_B) {
            r->act = ACT_LATCH;
            latched = latched == cur_key() ? -1 : cur_key();
            keys_publish(latched >= 0 ? 1u << latched : 0u);
        } else
            goto base;                  /* L1 R1 R2 START: as in BASE (OCT+ and a key: the mutes) */
        return;
    }
base:
    switch (b) {
    case MB_START: r->act = ACT_BTN, r->arg = B_PLAY; btn_set(B_PLAY, 1); return;
    case MB_L1: r->act = ACT_BTN, r->arg = B_OCTDN; btn_set(B_OCTDN, 1); return;
    case MB_R1: r->act = ACT_BTN, r->arg = B_OCTUP; btn_set(B_OCTUP, 1); return;
    case MB_R2: r->act = ACT_BTN, r->arg = B_REC; btn_set(B_REC, 1); return;
    case MB_SELECT: case MB_L2: r->act = ACT_NONE; return;   /* (the layers' modifiers) */
    default: break;
    }
    if (dpad) {
        int k = face_knob();
        if (k >= 0) {
            r->act = ACT_KNOB, r->arg = (uint8_t)k;
            r->dir = (int8_t)(sign * (b == MB_UP || b == MB_DOWN ? 4 : 1));
        } else {
            r->act = ACT_ENC, r->arg = (uint8_t)(b == MB_LEFT || b == MB_RIGHT ? EN_ALGO : EN_SELECT);
            r->dir = (int8_t)(b == MB_LEFT || b == MB_RIGHT ? sign : -sign);   /* (down: the next page) */
        }
        role_step(b);
        return;
    }
    r->act = ACT_IGNORE;                /* a face button: a knob's shift, for the D-pad */
}
static void mb_up(int b)
{
    mb_role_t *r = &role[b];
    mb_held &= ~MB(b);
    if (r->act == ACT_BTN)
        btn_set(r->arg, 0);
    if (b == MB_MENU && r->act == ACT_HELP && menu_alone)
        help_on = 1;
    r->act = ACT_NONE;
    if (b == MB_A || b == MB_X)
        keys_publish(0);
}
static void mb_repeat(void)
{
    for (int b = 0; b < MB_N; b++) {
        mb_role_t *r = &role[b];
        if (!(mb_held & MB(b)) || (r->act != ACT_ENC && r->act != ACT_KNOB && r->act != ACT_CURSOR && r->act != ACT_MASTER))
            continue;
        while ((int32_t)(g_ms - r->next_ms) >= 0) {
            role_step(b);
            r->next_ms += g_ms - r->down_ms > 1500u ? 35u : 70u;   /* faster after 1.5 s */
        }
    }
}
static void mb_set(int b, int down)
{
    if (b < 0 || b >= MB_N || !!(mb_held & MB(b)) == !!down)
        return;
    if (down)
        mb_down(b);
    else
        mb_up(b);
}

/* ------------------------------------------------------------ audio ring --- */
#define AR (1u << 14)                   /* frames (0.37 s) */
static int16_t aring[AR * 2];
static volatile uint32_t ar_w, ar_r;
static uint32_t target_frames = 2048;   /* how far ahead the simulator runs (MYFAM_AHEAD) */
static uint32_t ar_fill(void) { return __atomic_load_n(&ar_w, __ATOMIC_ACQUIRE) - __atomic_load_n(&ar_r, __ATOMIC_ACQUIRE); }
static void ar_pull(int16_t *out, uint32_t frames)      /* the sound card's side */
{
    uint32_t r = __atomic_load_n(&ar_r, __ATOMIC_RELAXED), w = __atomic_load_n(&ar_w, __ATOMIC_ACQUIRE), k;
    for (k = 0; k < frames && r != w; k++, r++) {
        out[2 * k] = aring[(r % AR) * 2];
        out[2 * k + 1] = aring[(r % AR) * 2 + 1];
    }
    for (; k < frames; k++)
        out[2 * k] = out[2 * k + 1] = 0;
    __atomic_store_n(&ar_r, r, __ATOMIC_RELEASE);
}
/* where the time goes (the help shows it): the app (tick, frame, idle), the sound, drawing, the screen copy */
enum { PR_APP, PR_SOUND, PR_DRAW, PR_SCREEN, PR_POLL, PR_SLEEP, PR_N };
static uint64_t pr_us[PR_N];
static int pr_pct[PR_N] = {-1, -1, -1, -1, -1, -1};
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
/* one millisecond of My-FAM: its tick, a frame or idle, the 44.1 frames that fell due */
static double frac;
static void sim_one_ms(void)
{
    int16_t buf[2 * 64];
    uint32_t n, w;
    uint64_t t0 = now_us(), t1, t2;
    g_ms++;
    mb_repeat();
    rf_tick(g_ms);
    if (g_ms % 16u == 0)
        rf_frame();
    else
        rf_idle();
    t1 = now_us();
    frac += RATE / 1000.0;
    n = (uint32_t)frac;
    frac -= n;
    rf_audio(buf, n);
    t2 = now_us();
    pr_us[PR_APP] += t1 - t0, pr_us[PR_SOUND] += t2 - t1;
    w = __atomic_load_n(&ar_w, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < n; k++, w++) {
        aring[(w % AR) * 2] = buf[2 * k];
        aring[(w % AR) * 2 + 1] = buf[2 * k + 1];
    }
    __atomic_store_n(&ar_w, w, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------ saving --- */
static const char *save_path = "my-fam.flash";
static uint8_t save_buf[RF_FLASH_SIZE];
static pthread_mutex_t save_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t save_cv = PTHREAD_COND_INITIALIZER;
static int save_pending, save_stop;
static uint32_t saved_dirty, seen_dirty, seen_dirty_ms;

static int write_file(const uint8_t *p, uint32_t n)
{
    char tmp[1024];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
    if (!(f = fopen(tmp, "wb")))
        return 0;
    int ok = fwrite(p, 1, n, f) == n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    return ok && rename(tmp, save_path) == 0;
}
static void *saver(void *arg)
{
    static uint8_t mine[RF_FLASH_SIZE];
    (void)arg;
    th_self(TH_SAVE);
    saver_ready = 1;
    pthread_mutex_lock(&save_mx);
    for (;;) {
        while (!save_pending && !save_stop)
            pthread_cond_wait(&save_cv, &save_mx);
        if (!save_pending && save_stop)
            break;
        memcpy(mine, save_buf, sizeof mine);
        save_pending = 0;
        pthread_mutex_unlock(&save_mx);
        if (!write_file(mine, sizeof mine))
            fprintf(stderr, "my-fam: could not write %s\n", save_path);
        pthread_mutex_lock(&save_mx);
    }
    pthread_mutex_unlock(&save_mx);
    return 0;
}
static void save_kick(void)             /* the image as it is now, to the thread */
{
    rf_flash_sync();
    pthread_mutex_lock(&save_mx);
    memcpy(save_buf, rf_image, sizeof save_buf);
    save_pending = 1;
    pthread_cond_signal(&save_cv);
    pthread_mutex_unlock(&save_mx);
    saved_dirty = rf_flash_dirty();
}
/* a save 2 s after the app last wrote its store or data (it writes them itself only when it is quiet) */
static void save_check(void)
{
    uint32_t d = rf_flash_dirty();
    if (d != seen_dirty)
        seen_dirty = d, seen_dirty_ms = g_ms;
    else if (d != saved_dirty && g_ms - seen_dirty_ms > 2000u)
        save_kick();
}
static void load_flash(void)
{
    static uint8_t img[RF_FLASH_SIZE];
    FILE *f = fopen(save_path, "rb");
    size_t n = 0;
    if (f) {
        n = fread(img, 1, sizeof img, f);
        fclose(f);
    }
    rf_flash_stage(n ? img : 0, (uint32_t)n);
}

/* ------------------------------------------------------------ CPU and RAM --- */
/* from /proc, once a second: the app's CPU (% of one core), the whole system's (all cores), the app's memory (RSS)
 * and the system's (in use, total) */
static int st_ncpu = 1, st_app_cpu = -1, st_sys_cpu = -1, st_app_mb = -1, st_used_mb = -1, st_total_mb = -1;
static uint32_t st_ms;
static unsigned long long st_app_ticks, st_sys_busy, st_sys_all;
/* (no scanf here: the Miyoo's C library and the newer scanf entry points do not get along: the numbers by hand) */
static size_t read_small(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t k = 0;
    if (f) {
        k = fread(buf, 1, n - 1, f);
        fclose(f);
    }
    buf[k] = 0;
    return k;
}
static unsigned long long num_at(const char **p)   /* the next number from *p on, 0 none */
{
    while (**p && (**p < '0' || **p > '9'))
        (*p)++;
    if (!**p)
        return 0;
    char *end;
    unsigned long long v = strtoull(*p, &end, 10);
    *p = end;
    return v;
}
static unsigned long long field_kb(const char *text, const char *key)   /* "Key:   123 kB" */
{
    const char *p = strstr(text, key);
    if (!p)
        return 0;
    p += strlen(key);
    return num_at(&p);
}
static void stats_update(void)
{
    static uint32_t last_ms;
    static int primed;
    static char buf[4096];
    unsigned long long ut = 0, stt = 0, v[8] = {0}, busy, all;
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    uint32_t dt = g_ms - last_ms;
    const char *p;
    if (read_small("/proc/self/stat", buf, sizeof buf) && (p = strrchr(buf, ')'))) {
        /* after the name: state, then fields 4..13, then utime (14) and stime (15) */
        p += 2;
        if (*p)
            p++;
        for (int i = 4; i <= 13; i++)
            (void)num_at(&p);
        ut = num_at(&p), stt = num_at(&p);
    }
    if (read_small("/proc/stat", buf, sizeof buf) && !strncmp(buf, "cpu ", 4)) {
        p = buf + 4;
        for (int i = 0; i < 8; i++)
            v[i] = num_at(&p);
    }
    /* the system's CPU: 100% less the idle time over the time that passed on the cores online. (Not the busy
     * counters: those are sampled at each timer tick, and a program that wakes on the timer, works under a
     * millisecond and sleeps again is never caught by them: SYS read 0%. The idle time is measured, not sampled.) */
    {
        int ncpu = 0;
        for (const char *q = buf; (q = strstr(q, "\ncpu")); q += 4)
            ncpu += q[4] >= '0' && q[4] <= '9';
        st_ncpu = ncpu ? ncpu : 1;
    }
    busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
    all = v[3] + v[4];                  /* (here: the idle time, idle + iowait) */
    {
        static uint64_t sys_wall;
        uint64_t w = now_us();
        if (primed && dt > 0 && hz > 0) {
            st_app_cpu = (int)((ut + stt - st_app_ticks) * 100000ull / (unsigned long long)hz / dt);
            if (sys_wall && w > sys_wall && all >= st_sys_all) {
                double cap = (double)(w - sys_wall) / 1e6 * st_ncpu, idle = (double)(all - st_sys_all) / (double)hz;
                st_sys_cpu = (int)(100.0 * (1.0 - idle / cap) + 0.5);
                st_sys_cpu = st_sys_cpu < 0 ? 0 : st_sys_cpu > 100 ? 100 : st_sys_cpu;
            }
        }
        sys_wall = w;
    }
    {   /* each thread's share of one core since the last reading */
        static uint64_t th_wall;
        uint64_t w = now_us();
        unsigned long long sum[TH_KINDS] = {0};
        char path[64];
        th_scan(TH_OTHER);
        pthread_mutex_lock(&th_mx);
        for (int i = 0; i < th_n; i++) {
            unsigned long long t = 0;
            snprintf(path, sizeof path, "/proc/self/task/%d/stat", th[i].tid);
            if (read_small(path, buf, sizeof buf) && (p = strrchr(buf, ')'))) {
                p += 2;
                if (*p)
                    p++;
                for (int k = 4; k <= 13; k++)
                    (void)num_at(&p);
                t = num_at(&p);
                t += num_at(&p);
            }
            if (t >= th[i].ticks)
                sum[th[i].kind] += t - th[i].ticks;
            th[i].ticks = t;
        }
        pthread_mutex_unlock(&th_mx);
        if (th_wall && w > th_wall && hz > 0)
            for (int k = 0; k < TH_KINDS; k++)
                th_pct[k] = (int)(sum[k] * 100000000ull / (unsigned long long)hz / (w - th_wall));
        th_wall = w;
    }
    st_app_ticks = ut + stt, st_sys_busy = busy, st_sys_all = all, last_ms = g_ms, primed = 1;
    {
        static uint64_t last_wall;
        uint64_t w = now_us();
        if (last_wall && w > last_wall)
            for (int i = 0; i < PR_N; i++)
                pr_pct[i] = (int)(pr_us[i] * 100u / (w - last_wall));
        memset(pr_us, 0, sizeof pr_us);
        last_wall = w;
    }
    if (read_small("/proc/self/statm", buf, sizeof buf)) {
        p = buf;
        (void)num_at(&p);
        st_app_mb = (int)(num_at(&p) * (unsigned long long)(page > 0 ? page : 4096) / (1024 * 1024));
    }
    if (read_small("/proc/meminfo", buf, sizeof buf)) {
        unsigned long long total = field_kb(buf, "MemTotal:"), avail = field_kb(buf, "MemAvailable:");
        if (!avail)
            avail = field_kb(buf, "MemFree:") + field_kb(buf, "Buffers:") + field_kb(buf, "Cached:");
        if (total)
            st_total_mb = (int)(total / 1024), st_used_mb = (int)((total - avail) / 1024);
    }
}

/* ------------------------------------------------------------ battery --- */
/* the Miyoo Mini Plus's battery, as Allium reads it: /customer/app/axp_test prints {"battery":N,...,"charging":N}
 * (3: charging). Every 30 s, in a thread of its own (the program takes a moment). -1: not known (another device). */
static volatile int batt_pct = -1, batt_charging;
static void *batt_thread(void *arg)
{
    (void)arg;
    th_self(TH_BATT);
    for (;;) {
        char out[256];
        size_t n = 0;
        FILE *f = access("/customer/app/axp_test", X_OK) == 0 ? popen("/customer/app/axp_test 2>/dev/null", "r") : 0;
        if (!f)
            break;
        n = fread(out, 1, sizeof out - 1, f);
        pclose(f);
        out[n] = 0;
        const char *p = strstr(out, "\"battery\"");
        if (p) {
            p += 9;
            batt_pct = (int)num_at(&p);
            if ((p = strstr(out, "\"charging\""))) {
                p += 10;
                batt_charging = num_at(&p) == 3;
            }
        }
        sleep(30);
    }
    return 0;
}
static int stats_off;                   /* MYFAM_STATS=0: no readings */
static void stats_tick(void)
{
    if (stats_off)
        return;
    if (st_ms && g_ms - st_ms < 1000u)
        return;
    st_ms = g_ms ? g_ms : 1;
    stats_update();
    if (ui.mode == M_SET)               /* (SYS: the new readings) */
        ui.d_vis = ui.d_foot = 1;
}
static void pct_str(char *s, size_t n, int v) { if (v < 0) snprintf(s, n, "--"); else snprintf(s, n, "%d%%", v); }


/* ------------------------------------------------------------ GLO's SYSTEM page --- */
/* (fam/app.c, built with MYFAM: GLO's fifth page draws with these, in the app's own canvas and fonts) */
static int g_video_w, g_video_h, g_video_bpp, g_audio_rate, g_audio_samples;
static void diag_line(int32_t y, const char *label, const char *val, uint16_t col)
{
    cv_text(6, y, &FONT_XS, label, CL_L);
    cv_text(62, y, &FONT_XS, val, col);
}
void myfam_diag_vis(void)
{
    char t[64], a[8], b[8];
    cv_text(6, 2, &FONT_T, "MY-FAM V" MYFAM_VERSION, CL_O);
    if (batt_pct >= 0) {                /* the battery: red at 15% and under (Allium's warning), "+" charging */
        snprintf(t, sizeof t, "BATTERY %d%%%s", batt_pct, batt_charging ? " +" : "");
        cv_text(236 - text_w(&FONT_T, t), 2, &FONT_T, t, batt_pct <= 15 && !batt_charging ? CL_R : CL_L);
    }
    pct_str(a, sizeof a, st_app_cpu), pct_str(b, sizeof b, st_sys_cpu);
    snprintf(t, sizeof t, "APP %s   SYSTEM %s (%d CORE%s)", a, b, st_ncpu, st_ncpu == 1 ? "" : "S");
    diag_line(20, "CPU", t, st_app_cpu >= 80 ? CL_R : C_WHITE);
    if (st_total_mb > 0)
        snprintf(t, sizeof t, "APP %dM   SYSTEM %d / %dM", st_app_mb, st_used_mb, st_total_mb);
    else
        snprintf(t, sizeof t, "--");
    diag_line(34, "RAM", t, C_WHITE);
    t[0] = 0;
    for (int k = 0; k < TH_KINDS; k++) {
        char one[16];
        if (k != TH_MAIN && th_pct[k] <= 0)
            continue;
        snprintf(one, sizeof one, "%s%s %d%%", t[0] ? "  " : "", TH_NAME[k], th_pct[k] < 0 ? 0 : th_pct[k]);
        strncat(t, one, sizeof t - strlen(t) - 1);
    }
    diag_line(48, "THREADS", t, C_WHITE);
    snprintf(t, sizeof t, "APP %d  SOUND %d  DRAW %d  SCREEN %d", pr_pct[PR_APP], pr_pct[PR_SOUND], pr_pct[PR_DRAW],
             pr_pct[PR_SCREEN]);
    diag_line(62, "TIME %", t, C_WHITE);
    snprintf(t, sizeof t, "POLL %d  SLEEP %d", pr_pct[PR_POLL], pr_pct[PR_SLEEP]);
    diag_line(76, "", t, C_WHITE);
    snprintf(t, sizeof t, "%dX%d %d BPP", g_video_w, g_video_h, g_video_bpp);
    diag_line(90, "SCREEN", t, C_WHITE);
    if (g_audio_rate)
        snprintf(t, sizeof t, "%d HZ  BUFFER %d  AHEAD %u", g_audio_rate, g_audio_samples, (unsigned)target_frames);
    else
        snprintf(t, sizeof t, "NONE");
    diag_line(104, "SOUND", t, g_audio_rate == RATE ? C_WHITE : CL_R);
}
void myfam_diag_cells(void *cells)
{
    fcell_t *c = cells;
    char t[16];
    pct_str(t, sizeof t, st_app_cpu);
    cell_set(&c[0], "CPU", t, st_app_cpu < 0 ? 0 : (st_app_cpu > 100 ? 127 : st_app_cpu * 127 / 100), 0);
    snprintf(t, sizeof t, "%dM", st_app_mb < 0 ? 0 : st_app_mb);
    cell_set(&c[1], "RAM", t, st_total_mb > 0 ? st_app_mb * 127 / st_total_mb : 0, 0);
    pct_str(t, sizeof t, st_sys_cpu);
    cell_set(&c[2], "SYS CPU", t, st_sys_cpu < 0 ? 0 : st_sys_cpu * 127 / 100, 0);
    snprintf(t, sizeof t, "%dM", st_total_mb > 0 ? st_total_mb - st_used_mb : 0);
    cell_set(&c[3], "FREE RAM", t, st_total_mb > 0 ? (st_total_mb - st_used_mb) * 127 / st_total_mb : 0, 0);
}

/* ------------------------------------------------------------ drawing --- */
static uint32_t scr[SCR_W * SCR_H];     /* XRGB8888 */
#define XRGB(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
static const uint32_t KNOB_RGB[4] = {XRGB(60, 220, 90), XRGB(40, 210, 230), XRGB(230, 70, 210), XRGB(255, 150, 40)};
static uint8_t glyphs[128][7];          /* rows, bit 4 the leftmost column */

static void font_init(void)
{
    for (size_t i = 0; i + 7 < sizeof FONT5X7 / sizeof FONT5X7[0]; i += 8) {
        uint8_t c = (uint8_t)FONT5X7[i][0];
        for (int y = 0; y < 7; y++) {
            uint8_t bits = 0;
            for (int x = 0; x < 5; x++)
                bits |= (uint8_t)((FONT5X7[i + 1 + y][x] == '#') << (4 - x));
            glyphs[c & 127][y] = bits;
        }
    }
}
static void fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < SCR_H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < SCR_W; i++)
            scr[j * SCR_W + i] = c;
}
static void frame_rect(int x, int y, int w, int h, uint32_t c)
{
    fill(x, y, w, 1, c), fill(x, y + h - 1, w, 1, c), fill(x, y, 1, h, c), fill(x + w - 1, y, 1, h, c);
}
/* text at scale s (each glyph 6*s wide); returns the x after it */
static int text(int x, int y, int s, uint32_t c, const char *str)
{
    for (; *str; str++, x += 6 * s) {
        uint8_t ch = (uint8_t)*str;
        if (ch >= 'a' && ch <= 'z')
            ch = (uint8_t)(ch - 32);
        for (int j = 0; j < 7; j++)
            for (int i = 0; i < 5; i++)
                if (glyphs[ch & 127][j] >> (4 - i) & 1)
                    fill(x + i * s, y + j * s, s, s, c);
    }
    return x;
}
static uint8_t led_of(int i)            /* plat's buttons 0..13, then keys */
{
    uint8_t q = rf_led_pos[i];
    return q == 0xFF ? 0 : rf_panel.led[q >> 2][q & 3u];
}
static uint32_t led_rgb(uint8_t v, uint32_t on)   /* a light at level v (0..255): from dark grey to its colour */
{
    uint32_t k = v < 12 ? 0 : v < 64 ? 90 : v < 128 ? 170 : 255, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t c = on >> sh & 255u;
        out |= (40u + (c > 40u ? (c - 40u) * k / 255u : 0u)) << sh;
    }
    return out;
}
static void draw_lcd(void)
{
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
            uint16_t p = rf_panel.lcd[y * 240 + x];
            p = (uint16_t)(p >> 8 | p << 8);                    /* (high byte first) */
            uint32_t r = (p >> 11 & 31u) * 255u / 31u, g = (p >> 5 & 63u) * 255u / 63u, b = (p & 31u) * 255u / 31u;
            uint32_t c = XRGB(r, g, b), *d = &scr[(2 * y) * SCR_W + 2 * x];
            d[0] = d[1] = d[SCR_W] = d[SCR_W + 1] = c;
        }
}
/* a mode button in the side panel (full width): its light, the Miyoo control, the name (2x) */
static void chip(int x, int y, const char *ctl, const char *name, int lbl)
{
    uint8_t v = lbl >= 0 ? led_of(lbl) : 0;
    int held = lbl >= 0 && (rf_panel.buttons >> RF_BTN_ID[lbl] & 1u);
    fill(x, y, 152, 22, held ? XRGB(90, 90, 110) : XRGB(28, 28, 34));
    fill(x + 3, y + 3, 5, 16, led_rgb(v, XRGB(255, 60, 60)));
    text(x + 12, y + 4, 2, XRGB(150, 150, 160), ctl);
    text(x + 44, y + 4, 2, XRGB(235, 235, 235), name);
}
static const char *layer_name(void)
{
    if (help_on)
        return "HELP";
    if (mb_held & MB(MB_SELECT))
        return "MODES";
    if (mb_held & MB(MB_L2))
        return "KEYS";
    return "BASE";
}
/* the keys: 16 white, the black ones over their gaps; lit as the FM-1 lights them, yellow while held, the cursor
 * under (white) or over (black) its key */
static void draw_keys(int y0, int h)
{
    int x0 = PANEL_X + 8, kw = 9, bh = h * 6 / 10;
    uint32_t held = rf_panel.keys;
    for (int i = 0; i < 16; i++) {
        int k = WHITE[i];
        uint8_t v = led_of(RF_NB + k);
        uint32_t c = held >> k & 1u ? XRGB(255, 230, 80) : v ? led_rgb(v, XRGB(255, 60, 60)) : XRGB(200, 200, 200);
        fill(x0 + i * kw, y0 + 8, kw - 1, h, c);
        if (!cur_row && cur_idx == i)
            fill(x0 + i * kw, y0 + 10 + h, kw - 1, 5, XRGB(255, 255, 255));
    }
    for (int i = 0; i < 11; i++) {
        int k = BLACK[i], w = 0;
        while (w < 15 && WHITE[w + 1] < k)
            w++;
        uint8_t v = led_of(RF_NB + k);
        uint32_t c = held >> k & 1u ? XRGB(255, 230, 80) : v ? led_rgb(v, XRGB(255, 60, 60)) : XRGB(20, 20, 20);
        int x = x0 + (w + 1) * kw - 4;
        fill(x - 1, y0 + 7, 8, bh + 1, XRGB(0, 0, 0));
        fill(x, y0 + 8, 6, bh - 1, c);
        if (cur_row && cur_idx == i)
            fill(x, y0 + 1, 6, 5, XRGB(255, 255, 255));
    }
}
/* the battery, in the panel's style: a grey outline, the charge in green (amber at 30%, red at 15% and under), a
 * yellow bolt while charging; nothing when it is not known (not a Miyoo Mini Plus) */
static void draw_battery(int x, int y)
{
    int p = batt_pct;
    if (p < 0)
        return;
    p = p > 100 ? 100 : p;
    uint32_t edge = XRGB(150, 150, 160);
    uint32_t c = batt_charging ? XRGB(60, 220, 90) : p <= 15 ? XRGB(240, 60, 50) : p <= 30 ? XRGB(255, 170, 40)
                                                                                         : XRGB(60, 220, 90);
    frame_rect(x, y, 30, 16, edge);
    fill(x + 30, y + 5, 3, 6, edge);
    fill(x + 2, y + 2, 26, 12, XRGB(14, 14, 18));
    fill(x + 2, y + 2, (26 * p + 50) / 100 < 2 ? 2 : (26 * p + 50) / 100, 12, c);
    if (batt_charging) {                /* a bolt, 2 px a dot */
        static const char *B[6] = {"..##.", ".##..", "#####", "..##.", ".##..", ".#..."};
        for (int j = 0; j < 6; j++)
            for (int i = 0; i < 5; i++)
                if (B[j][i] == '#')
                    fill(x + 10 + i * 2, y + 2 + j * 2, 2, 2, XRGB(255, 230, 80));
    }
}
static void draw_panel(void)
{
    char s[32];
    int x = PANEL_X, y;
    fill(x, 0, 160, SCR_H, XRGB(14, 14, 18));
    text(x + 8, 6, 2, XRGB(255, 140, 30), "MY-FAM");
    text(x + 84, 13, 1, XRGB(120, 120, 130), "V" MYFAM_VERSION);
    draw_battery(x + 116, 6);
    fill(x + 8, 26, 144, 24, XRGB(40, 40, 52));
    text(x + 14, 31, 2, XRGB(255, 255, 255), layer_name());
    /* PLAY and the shifts: START, L1 (OCT-), R1 (OCT+), R2 (REC) */
    {
        int pr = led_of(B_PLAY) > 40, pg = rf_panel.led_play_green > 40;
        y = 56;
        fill(x + 8, y, 144, 24, XRGB(28, 28, 34));
        fill(x + 12, y + 4, 16, 16, pg ? XRGB(40, 230, 70) : pr ? XRGB(240, 50, 40) : XRGB(50, 50, 50));
        text(x + 34, y + 5, 2, XRGB(235, 235, 235), "PLAY");
        y = 84;
        static const int8_t SH[3] = {B_OCTDN, B_OCTUP, B_REC};
        static const char *const SN[3] = {"L1", "R1", "R2"};
        for (int i = 0; i < 3; i++) {
            uint8_t v = led_of(SH[i]);
            int held = rf_panel.buttons >> RF_BTN_ID[SH[i]] & 1u;
            fill(x + 8 + i * 49, y, 46, 22, held ? XRGB(90, 90, 110) : XRGB(28, 28, 34));
            fill(x + 10 + i * 49, y + 3, 4, 16, led_rgb(v, i == 2 ? XRGB(255, 60, 60) : XRGB(255, 200, 60)));
            text(x + 18 + i * 49, y + 4, 2, XRGB(235, 235, 235), SN[i]);
        }
        text(x + 8, y + 26, 1, XRGB(130, 130, 140), "  OCT-   OCT+    REC");
    }
    y = 124;
    if (mb_held & MB(MB_SELECT)) {      /* MODES: the map, with the lights */
        static const struct { const char *c, *t; int8_t b; } M[9] = {
            {"\x01", "SEQ", B_SEQ}, {"\x04", "EDIT", B_EDIT}, {"\x02", "KICK", B_ARP}, {"\x03", "LFO", B_LFO},
            {"X", "SCL", B_SEL}, {"A", "FX", B_FX}, {"B", "PATTERNS", B_GLO}, {"Y", "SAVE", B_SAVE},
            {"L1", "SYS", B_ENV}};
        for (int i = 0; i < 9; i++)
            chip(x + 4, y - 2 + i * 24, M[i].c, M[i].t, M[i].b);
        text(x + 8, y + 216, 1, XRGB(170, 170, 180), "L2 / R2   PRESET - / +");
    } else if (mb_held & MB(MB_L2)) {   /* KEYS: the keyboard, big */
        if (latched >= 0)
            snprintf(s, sizeof s, "HOLD %d", latched + 1);
        else
            snprintf(s, sizeof s, "%s %d", cur_row ? "BLK" : "STEP", cur_idx + 1);
        text(x + 8, y, 2, latched >= 0 ? XRGB(255, 230, 80) : XRGB(255, 255, 255), s);
        draw_keys(y + 20, 70);
        text(x + 8, y + 108, 2, XRGB(200, 200, 210), "A PLAY");
        text(x + 8, y + 130, 2, XRGB(200, 200, 210), "X TAP \x04");
        text(x + 8, y + 152, 2, XRGB(200, 200, 210), "B HOLD");
    } else {                            /* BASE: the knobs, the encoders */
        static const char *F[4] = {"Y K1", "X K2", "A K3", "B K4"};
        for (int i = 0; i < 4; i++) {
            int kx = x + 8 + (i % 2) * 74, ky = y + (i / 2) * 32;
            int lit = knob_flash_ms[i] && g_ms - knob_flash_ms[i] < 200u;
            fill(kx, ky, 70, 28, lit ? KNOB_RGB[i] : XRGB(28, 28, 34));
            fill(kx, ky, 4, 28, KNOB_RGB[i]);
            text(kx + 10, ky + 7, 2, lit ? XRGB(0, 0, 0) : KNOB_RGB[i], F[i]);
        }
        text(x + 8, y + 70, 1, XRGB(130, 130, 140), "HOLD + D-PAD");
        text(x + 8, y + 90, 2, XRGB(200, 200, 210), "\x03\x04 ALGO");
        text(x + 8, y + 112, 2, XRGB(200, 200, 210), "\x01\x02 PAGE");
        text(x + 8, y + 140, 2, XRGB(170, 170, 180), "SEL MODES");
        text(x + 8, y + 162, 2, XRGB(170, 170, 180), "L2  KEYS");
    }
    /* the keys, always: the steps' lights */
    if (!(mb_held & MB(MB_L2))) {
        if (latched >= 0)
            snprintf(s, sizeof s, "HOLD %d", latched + 1);
        else
            snprintf(s, sizeof s, "%s %d", cur_row ? "BLK" : "STEP", cur_idx + 1);
        text(x + 8, 352, 2, latched >= 0 ? XRGB(255, 230, 80) : XRGB(170, 170, 180), s);
        draw_keys(370, 44);
    }
    text(x + 8, 458, 2, XRGB(110, 110, 120), "MENU HELP");
}
static const char *const HELP[] = {
    "MY-FAM V" MYFAM_VERSION " BY DAVID AMO, FOR MIYOO MINI PLUS",
    "",
    "BASE",
    " Y X A B + D-PAD   KNOBS K1 K2 K3 K4",
    "                   \x04\x01 +1/+4  \x03\x02 -1/-4",
    " D-PAD \x03\x04         ALGORITHM (CURSOR, TEMPO)",
    " D-PAD \x01\x02         SELECT (PAGE)",
    " START PLAY   L1 OCT-   R1 OCT+   R2 REC",
    "",
    "SELECT + (MODES)",
    " \x01 SEQ  \x04 EDIT  \x02 KICK  \x03 LFO",
    " X SCL  A FX  B PATTERNS  Y SAVE  L1 SYS",
    " L2 / R2   PRESETS - / +",
    "",
    "L2 + (KEYS)",
    " D-PAD MOVE THE CURSOR (\x01 BLACK, \x02 WHITE KEYS)",
    " A PLAY THE KEY   X TAP AND MOVE ON",
    " B HOLD THE KEY (KNOBS THEN EDIT THAT STEP)",
    "",
    "HOLD L1 OR R1 FIRST FOR THE OCT COMBOS",
    "MENU + \x03\x04 VOLUME   MENU + \x01\x02 BRIGHTNESS",
    "SELECT + L1: SYS (CPU, RAM, THREADS, BATTERY)",
};
static void draw_help(void)
{
    char s[64];
    fill(0, 0, SCR_W, SCR_H, XRGB(10, 10, 14));
    int y = 10;
    for (size_t i = 0; i < sizeof HELP / sizeof HELP[0]; i++, y += 19)
        text(12, y, 2, i == 0 ? XRGB(255, 140, 30) : HELP[i][0] != ' ' ? XRGB(120, 220, 255) : XRGB(230, 230, 230), HELP[i]);
    snprintf(s, sizeof s, "MASTER %d%%", master_pct);
    text(12, SCR_H - 22, 2, XRGB(255, 230, 80), s);
    text(12 + 13 * 12, SCR_H - 22, 2, XRGB(230, 230, 230), "\x03\x04 MASTER  START QUIT  MENU CLOSE");
}
static uint32_t drawn_sig = ~0u;
static uint32_t screen_sig(void)        /* what the picture depends on: redraw only when it changed */
{
    uint32_t h = rf_panel.lcd_writes * 2654435761u;
    const uint8_t *p = &rf_panel.led[0][0];
    for (size_t i = 0; i < sizeof rf_panel.led; i++)
        h = (h ^ p[i]) * 16777619u;
    h = (h ^ rf_panel.led_play_green) * 16777619u;
    h = (h ^ rf_panel.keys) * 16777619u;
    h = (h ^ rf_panel.buttons) * 16777619u;
    h = (h ^ (uint32_t)(help_on << 24 | cur_row << 20 | cur_idx << 12 | (latched & 0xFF) << 4 | master_pct)) * 16777619u;
    h = (h ^ mb_held) * 16777619u;
    h = (h ^ (uint32_t)(batt_pct << 1 | batt_charging)) * 16777619u;
    h = (h ^ (uint32_t)st_app_cpu) * 16777619u;
    h = (h ^ (uint32_t)st_sys_cpu) * 16777619u;
    h = (h ^ (uint32_t)st_app_mb << 16 ^ (uint32_t)st_used_mb) * 16777619u;
    for (int i = 0; i < 4; i++)
        h = (h ^ (knob_flash_ms[i] && g_ms - knob_flash_ms[i] < 200u)) * 16777619u;
    return h;
}
static uint32_t drawn_lcd = ~0u;
static int draw(int force)              /* 1: the picture changed (the FM-1's screen and the panel each only when theirs did) */
{
    uint32_t sig = screen_sig();
    uint64_t t0 = now_us();
    if (!force && sig == drawn_sig)
        return 0;
    drawn_sig = sig;
    if (help_on) {
        draw_help();
        drawn_lcd = ~0u;
    } else {
        if (force || rf_panel.lcd_writes != drawn_lcd)
            draw_lcd(), drawn_lcd = rf_panel.lcd_writes;
        draw_panel();
    }
    pr_us[PR_DRAW] += now_us() - t0;
    return 1;
}

/* ------------------------------------------------------------ start, stop --- */
static void fe_init(void)
{
    const char *e;
    if ((e = getenv("MYFAM_SAVE")) && *e)
        save_path = e;
    if ((e = getenv("MYFAM_AHEAD")) && atoi(e) >= 256)
        target_frames = (uint32_t)atoi(e) < AR / 2 ? (uint32_t)atoi(e) : AR / 2;
    font_init();
    load_flash();
    rf_panel.master = master_pct * 1023 / 100;
    rf_init(0);
    saved_dirty = seen_dirty = rf_flash_dirty();
}
static void fe_quit_save(void)
{
    rf_flash_sync();
    if (rf_flash_dirty() != saved_dirty || access(save_path, F_OK) != 0) {
        memcpy(save_buf, rf_image, sizeof save_buf);
        if (!write_file(save_buf, sizeof save_buf))
            fprintf(stderr, "my-fam: could not write %s\n", save_path);
    }
}

#ifdef MIYOO_SDL
/* ============================================================ SDL 1.2 === */
#include <SDL/SDL.h>
#include <ucontext.h>
static volatile sig_atomic_t got_signal;
static void fe_log(const char *what)    /* (log.txt: how far the start got) */
{
    fprintf(stderr, "my-fam: %s\n", what);
    fflush(stderr);
}
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
#ifdef __arm__
    fprintf(stderr, "my-fam: crashed: signal %d at %p, pc %08lx lr %08lx\n", sig, si->si_addr,
            (unsigned long)uc->uc_mcontext.arm_pc, (unsigned long)uc->uc_mcontext.arm_lr);
#else
    (void)uc;
    fprintf(stderr, "my-fam: crashed: signal %d at %p\n", sig, si->si_addr);
#endif
    fflush(stderr);
    _exit(128 + sig);
}
static void on_signal(int s) { (void)s; got_signal = 1; }
static void audio_cb(void *u, Uint8 *stream, int len) { (void)u; ar_pull((int16_t *)stream, (uint32_t)len / 4u); }
static int sdl_button(SDLKey k)
{
    switch (k) {                        /* the Miyoo Mini's keys (a keyboard's arrows and these letters on a desktop) */
    case SDLK_UP: return MB_UP;
    case SDLK_DOWN: return MB_DOWN;
    case SDLK_LEFT: return MB_LEFT;
    case SDLK_RIGHT: return MB_RIGHT;
    case SDLK_SPACE: return MB_A;
    case SDLK_LCTRL: return MB_B;
    case SDLK_LSHIFT: return MB_X;
    case SDLK_LALT: return MB_Y;
    case SDLK_e: return MB_L1;
    case SDLK_t: return MB_R1;
    case SDLK_TAB: return MB_L2;
    case SDLK_BACKSPACE: return MB_R2;
    case SDLK_RCTRL: return MB_SELECT;
    case SDLK_RETURN: return MB_START;
    case SDLK_ESCAPE: return MB_MENU;
    default: return -1;
    }
}
static void blit(SDL_Surface *s, int rot180)
{
    SDL_PixelFormat *f = s->format;
    int xrgb = f->BytesPerPixel == 4 && f->Rshift == 16 && f->Gshift == 8 && f->Bshift == 0;
    int rgb565 = f->BytesPerPixel == 2 && f->Rshift == 11 && f->Gshift == 5 && f->Bshift == 0;
    if (SDL_MUSTLOCK(s))
        SDL_LockSurface(s);
    int w = s->w < SCR_W ? s->w : SCR_W, h = s->h < SCR_H ? s->h : SCR_H;
    int ox = (s->w - w) / 2, oy = (s->h - h) / 2;      /* (a bigger panel: the picture in its middle) */
    for (int y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)s->pixels + (size_t)(y + oy) * s->pitch;
        const uint32_t *src = rot180 ? &scr[(SCR_H - 1 - y) * SCR_W] : &scr[y * SCR_W];
        if (xrgb && !rot180) {          /* the usual case: a row as it is */
            memcpy((uint32_t *)row + ox, src, (size_t)w * 4u);
            continue;
        }
        for (int x = 0; x < w; x++) {
            uint32_t c = rot180 ? src[SCR_W - 1 - x] : src[x];
            if (xrgb)
                ((uint32_t *)row)[x + ox] = c;
            else if (rgb565)
                ((uint16_t *)row)[x + ox] = (uint16_t)((c >> 8 & 0xF800u) | (c >> 5 & 0x07E0u) | (c >> 3 & 0x001Fu));
            else {
                Uint32 px = SDL_MapRGB(f, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c);
                if (f->BytesPerPixel == 4)
                    ((uint32_t *)row)[x + ox] = px;
                else
                    ((uint16_t *)row)[x + ox] = (uint16_t)px;
            }
        }
    }
    if (SDL_MUSTLOCK(s))
        SDL_UnlockSurface(s);
    SDL_Flip(s);
}
int main(int argc, char **argv)
{
    SDL_AudioSpec want, have;
    const char *e;
    /* the picture: Allium's own SDL (/customer/lib) does not turn it, so My-FAM does (MYFAM_ROTATE=0: not) */
    int rot = !((e = getenv("MYFAM_ROTATE")) && atoi(e) == 0);
    if (argc > 1)
        setenv("MYFAM_SAVE", argv[1], 1);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_crash;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, 0), sigaction(SIGBUS, &sa, 0), sigaction(SIGILL, &sa, 0), sigaction(SIGFPE, &sa, 0);
        sigaction(SIGABRT, &sa, 0);
    }
    stats_off = (e = getenv("MYFAM_STATS")) && atoi(e) == 0;
#ifdef __arm__
    {   /* the VFP's flush-to-zero and default NaN: denormal floats (the tails of decays) at full speed */
        unsigned int fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr |= (1u << 24) | (1u << 25);
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    }
#endif
    fe_log("start My-FAM v" MYFAM_VERSION " by David Amo");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "my-fam: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    th_self(TH_MAIN);
    th_scan(TH_SDL);
    fe_log("SDL ok");
    SDL_ShowCursor(SDL_DISABLE);
    SDL_Surface *video = SDL_SetVideoMode(SCR_W, SCR_H, 32, SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!video)
        video = SDL_SetVideoMode(SCR_W, SCR_H, 16, SDL_SWSURFACE);
    if (!video) {
        fprintf(stderr, "my-fam: SDL_SetVideoMode: %s\n", SDL_GetError());
        return 1;
    }
    {
        char m[96];
        snprintf(m, sizeof m, "video %dx%d %d bpp", video->w, video->h, video->format->BytesPerPixel * 8);
        g_video_w = video->w, g_video_h = video->h, g_video_bpp = video->format->BytesPerPixel * 8;
        fe_log(m);
    }
    SDL_WM_SetCaption("My-FAM", "My-FAM");
    fe_init();
    fe_log("My-FAM ready");
    pthread_t th;
    pthread_create(&th, 0, saver, 0);
    memset(&want, 0, sizeof want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;                /* (Allium's audioserver, through SDL's OSS driver: 2048) */
    want.callback = audio_cb;
    while (!saver_ready && SDL_GetTicks() < 3000u)   /* (the saver has said which thread it is) */
        SDL_Delay(1);
    th_scan(TH_SDL);                    /* (whatever SDL_SetVideoMode started) */
    int open_rc = SDL_OpenAudio(&want, &have);
    th_scan(TH_AUDIO);
    if (open_rc < 0)
        fprintf(stderr, "my-fam: no sound: %s\n", SDL_GetError());
    else {
        if (have.freq != RATE)
            fprintf(stderr, "my-fam: the sound runs at %d Hz, not %d: off pitch\n", have.freq, RATE);
        if (target_frames < (uint32_t)have.samples + 2048u)
            target_frames = (uint32_t)have.samples + 2048u;   /* (the buffer, and 46 ms more) */
        g_audio_rate = have.freq, g_audio_samples = have.samples;
        SDL_PauseAudio(0);
        SDL_Delay(50);
        th_scan(TH_AUDIO);
        fe_log("sound ok");
    }
    uint32_t last_draw = 0, last_real = SDL_GetTicks();
    uint32_t frame_ms = (e = getenv("MYFAM_FPS")) && atoi(e) > 0 ? 1000u / (uint32_t)atoi(e) : 33u;   /* 30 fps */
    {                                   /* (after the audio thread was named) */
        pthread_t bt;
        if (pthread_create(&bt, 0, batt_thread, 0) == 0)
            pthread_detach(bt);
    }
    fe_log("running");
    while (!quit_req && !got_signal) {
        SDL_Event ev;
        uint64_t tp = now_us();
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit_req = 1;
            else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
                mb_set(sdl_button(ev.key.keysym.sym), ev.type == SDL_KEYDOWN);
        }
        pr_us[PR_POLL] += now_us() - tp;
        /* the simulator ahead of the sound card; without sound, by the wall clock */
        uint32_t now = SDL_GetTicks(), budget = 60;
        if (have.freq) {
            while (ar_fill() < target_frames && budget--)
                sim_one_ms();
        } else {
            while (last_real != now && budget--)
                sim_one_ms(), last_real++;
            last_real = now;
            ar_r = ar_w;
        }
        save_check();
        stats_tick();
        if (now - last_draw >= frame_ms) {
            last_draw = now;
            if (draw(0)) {
                uint64_t t0 = now_us();
                blit(video, rot);
                pr_us[PR_SCREEN] += now_us() - t0;
            }
        }
        {   /* a real sleep (the kernel's, not SDL_Delay: on the Miyoo that one may spin) */
            uint64_t ts = now_us();
            struct timespec d = {0, 2000000};
            nanosleep(&d, 0);
            pr_us[PR_SLEEP] += now_us() - ts;
        }
    }
    SDL_PauseAudio(1);
    SDL_CloseAudio();
    pthread_mutex_lock(&save_mx);
    save_stop = 1;
    pthread_cond_signal(&save_cv);
    pthread_mutex_unlock(&save_mx);
    pthread_join(th, 0);
    fe_quit_save();
    fe_log("saved, bye");
    fill(0, 0, SCR_W, SCR_H, 0);
    blit(video, rot);
    SDL_Quit();
    return 0;
}

#else
/* ============================================================ the test runner === */
/* fmfam_test SCRIPT OUTDIR: lines of
 *   wait MS | down BTN | up BTN | tap BTN [MS] | shot FILE.png | wav FILE.wav | wavend | save | expect help 0|1
 * BTN: UP DOWN LEFT RIGHT A B X Y L1 R1 L2 R2 SELECT START MENU */
#include <zlib.h>
static FILE *wav;
static uint32_t wav_frames;
static const char *outdir = ".";
static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4u;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    rf_put32(h + 4, 36u + data);
    rf_put32(h + 16, 16), h[20] = 1, h[21] = 0, h[22] = 2, h[23] = 0;
    rf_put32(h + 24, RATE), rf_put32(h + 28, RATE * 4u), h[32] = 4, h[33] = 0, h[34] = 16, h[35] = 0;
    memcpy(h + 36, "data", 4), rf_put32(h + 40, data);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}
static void png_out(const char *path)
{
    static uint8_t raw[SCR_H * (1 + SCR_W * 3)], z[SCR_H * (1 + SCR_W * 3) + 1024];
    uLongf zn = sizeof z;
    for (int y = 0; y < SCR_H; y++) {
        uint8_t *r = raw + y * (1 + SCR_W * 3);
        r[0] = 0;
        for (int x = 0; x < SCR_W; x++) {
            uint32_t c = scr[y * SCR_W + x];
            r[1 + 3 * x] = (uint8_t)(c >> 16), r[2 + 3 * x] = (uint8_t)(c >> 8), r[3 + 3 * x] = (uint8_t)c;
        }
    }
    compress2(z, &zn, raw, sizeof raw, 6);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint8_t ihdr[13] = {0, 0, SCR_W >> 8, SCR_W & 255, 0, 0, SCR_H >> 8, SCR_H & 255, 8, 2, 0, 0, 0};
    const uint8_t sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    struct { const char *t; const uint8_t *d; uint32_t n; } ch[3] = {{"IHDR", ihdr, 13}, {"IDAT", z, (uint32_t)zn}, {"IEND", 0, 0}};
    for (int i = 0; i < 3; i++) {
        uint8_t b[4] = {(uint8_t)(ch[i].n >> 24), (uint8_t)(ch[i].n >> 16), (uint8_t)(ch[i].n >> 8), (uint8_t)ch[i].n};
        fwrite(b, 1, 4, f);
        fwrite(ch[i].t, 1, 4, f);
        if (ch[i].n)
            fwrite(ch[i].d, 1, ch[i].n, f);
        uLong c = crc32(0, (const Bytef *)ch[i].t, 4);
        if (ch[i].n)
            c = crc32(c, ch[i].d, ch[i].n);
        uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(cb, 1, 4, f);
    }
    fclose(f);
}
static void fe_run(uint32_t ms)
{
    int16_t buf[2 * 64];
    while (ms--) {
        sim_one_ms();
        uint32_t n;
        while ((n = ar_fill()) > 0) {
            if (n > 64)
                n = 64;
            ar_pull(buf, n);
            if (wav) {
                fwrite(buf, 4, n, wav);
                wav_frames += n;
            }
        }
        save_check();
        stats_tick();
    }
}
static int btn_by_name(const char *s)
{
    for (int b = 0; b < MB_N; b++)
        if (!strcmp(s, MB_NAMES[b]))
            return b;
    return -1;
}
int main(int argc, char **argv)
{
    char line[256], a[64], b[192], path[512];
    int fails = 0, lineno = 0;
    FILE *sc;
    if (argc < 3 || !(sc = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: fmfam_test SCRIPT OUTDIR\n");
        return 2;
    }
    outdir = argv[2];
    {   /* MYFAM_TEST_BATT=PCT[,CHARGING]: a battery for the screenshots (there is none on a PC) */
        const char *e = getenv("MYFAM_TEST_BATT");
        if (e && *e) {
            batt_pct = atoi(e);
            batt_charging = strchr(e, ',') && atoi(strchr(e, ',') + 1);
        }
    }
    fe_init();
    pthread_t th;
    pthread_create(&th, 0, saver, 0);
    while (fgets(line, sizeof line, sc)) {
        int n = 0, v;
        lineno++;
        b[0] = 0;
        if (sscanf(line, "%63s %191[^\n]", a, b) < 1 || a[0] == '#')
            continue;
        if (!strcmp(a, "wait"))
            fe_run((uint32_t)atoi(b));
        else if (!strcmp(a, "down") || !strcmp(a, "up")) {
            if (btn_by_name(b) < 0)
                printf("FAIL line %d: no button %s\n", lineno, b), fails++;
            mb_set(btn_by_name(b), a[0] == 'd');
            fe_run(1);
        } else if (!strcmp(a, "tap")) {
            char nm[32];
            int hold = 40;
            sscanf(b, "%31s %d", nm, &hold);
            if (btn_by_name(nm) < 0)
                printf("FAIL line %d: no button %s\n", lineno, nm), fails++;
            mb_set(btn_by_name(nm), 1);
            fe_run((uint32_t)hold);
            mb_set(btn_by_name(nm), 0);
            fe_run(40);
        } else if (!strcmp(a, "shot")) {
            snprintf(path, sizeof path, "%s/%s", outdir, b);
            draw(1);
            png_out(path);
        } else if (!strcmp(a, "wav")) {
            snprintf(path, sizeof path, "%s/%s", outdir, b);
            if ((wav = fopen(path, "wb+")))
                wav_hdr(wav, 0), wav_frames = 0;
        } else if (!strcmp(a, "wavend")) {
            if (wav)
                wav_hdr(wav, wav_frames), fclose(wav), wav = 0;
        } else if (!strcmp(a, "save"))
            save_kick();
        else if (!strcmp(a, "expect") && sscanf(b, "%31s %d", a, &v) == 2) {
            int got = 0;
            if (!strcmp(a, "help"))
                got = help_on;
            else if (!strcmp(a, "latched"))
                got = latched;
            else if (!strcmp(a, "cursor"))
                got = cur_row * 100 + cur_idx;
            else if (!strcmp(a, "master"))
                got = master_pct;
            else if (!strcmp(a, "quit"))
                got = quit_req;
            else if (!app_expect(a, &got)) {
                printf("FAIL line %d: unknown expect %s\n", lineno, a);
                fails++;
                continue;
            }
            if (got != v) {
                printf("FAIL line %d: %s = %d, expected %d\n", lineno, a, got, v);
                fails++;
            }
        } else
            printf("FAIL line %d: %s?\n", lineno, a), fails++;
        (void)n;
    }
    if (wav)
        wav_hdr(wav, wav_frames), fclose(wav);
    pthread_mutex_lock(&save_mx);
    save_stop = 1;
    pthread_cond_signal(&save_cv);
    pthread_mutex_unlock(&save_mx);
    pthread_join(th, 0);
    fe_quit_save();
    printf("%s: %d failure(s)\n", argv[1], fails);
    return fails != 0;
}
#endif
