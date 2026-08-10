/* invaders.c -- a Space-Invaders-style intermission, hidden behind a
 * right-click on the clock dial's hub.
 *
 * Same embedding contract as lunarclock.c: a Tk-owned child HWND that
 * Direct2D paints, so the game never fights Tk's event loop. The whole
 * game -- simulation, rendering, input, sound, hi-score -- lives in this
 * one widget; the Tcl side only creates the window and binds Escape.
 *
 * Faithful in spirit, original in substance: the sprites, sounds, and
 * text here are drawn/synthesized from scratch (no Taito assets), and
 * the game calls itself INVADERS.
 *
 * Timing follows the dial's sweep pattern: a ~15 ms Tcl timer pumps a
 * fixed 60 Hz simulation clocked by QPC accumulation, so timer jitter
 * never becomes motion error. Input is polled per frame with
 * GetAsyncKeyState, gated on the toplevel owning focus AND Tk's own
 * focus being on this widget. The widget takes whatever rectangle the
 * dial had: the 224x256 field is uniform-scaled and letterboxed --
 * hard pixels at integer scales, evenness-preserving filtering at
 * fractional ones. The `test-step` subcommand advances the same
 * simulation deterministically (fixed RNG seed, synthetic input, no
 * wall clock, no sound, no hi-score writes) for the selftest and
 * staged screenshots; `hold` freezes a staged tableau outright.
 *
 * Sound is the chime engine's approach in miniature: tiny WAVs
 * synthesized once into memory, played fire-and-forget with PlaySound
 * (SND_ASYNC replaces the previous sound -- single-voice, newest wins,
 * exactly like 1978). The instrument outranks the toy: while a chime is
 * in flight (Lunar_ChimeBusy) the game stays silent rather than
 * truncating it.
 */
#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <d2d1.h>
#include <tcl.h>
#include <tk.h>
#include <tkPlatDecls.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_paths.h"   /* Lunar_AppDataPathW: hi-score persistence */

extern int Lunar_ChimeBusy(void);   /* lunarx.c: 1 while a chime plays */

/* ---- logical playfield ---------------------------------------------------
 * Classic proportions, 224x256 units, letterboxed and uniformly scaled to
 * the widget (the Tcl side sizes the window to exactly 2x for crisp
 * nearest-neighbour pixels). */
#define PF_W 224
#define PF_H 256

#define IV_ROWS 5
#define IV_COLS 11
#define IV_CELL 16
#define MAX_BOMBS 3
#define MAX_BOOMS 4
#define SHIELDS 4
#define SH_W 22
#define SH_H 16

#define FRAME_MS_NUM 1000        /* 60 Hz fixed timestep as a fraction */
#define FRAME_MS_DEN 60
#define PUMP_MS 15               /* Tcl timer cadence; sim catches up by QPC */

enum { KEY_LEFT = 1, KEY_RIGHT = 2, KEY_FIRE = 4, KEY_START = 8 };
typedef enum { MODE_ATTRACT, MODE_PLAYING, MODE_DYING, MODE_WAVE, MODE_OVER } InvMode;

typedef struct {
    float x, y;         /* top-left, logical units */
    int   alive;
} Bomb;

typedef struct {
    float x, y;
    int   ttl;          /* frames remaining */
    int   sprite;       /* which explosion bitmap */
} Boom;

typedef struct InvWidget {
    Tk_Window   tkwin;
    Tcl_Interp *interp;
    Tcl_Command command;
    int width, height;

    /* Direct2D (all target-bound resources recreated with the target) */
    ID2D1HwndRenderTarget *target;
    ID2D1SolidColorBrush  *brush;
    int targetWidth, targetHeight;
    D2D1_BITMAP_INTERPOLATION_MODE bmInterp; /* per-frame: crisp or even */
    ID2D1Bitmap *bmInv[3][2];      /* type x animation frame */
    ID2D1Bitmap *bmPlayer;
    ID2D1Bitmap *bmUfo;
    ID2D1Bitmap *bmBoom;
    ID2D1Bitmap *bmBomb[2];        /* zigzag animation frames */
    ID2D1Bitmap *bmShield[SHIELDS];
    int shieldDirty[SHIELDS];

    Tcl_TimerToken pump;           /* frame timer, NULL when idle */
    int redrawPending;
    int64_t lastQpc;               /* sim catch-up accumulator basis */
    int64_t accumMs1000;           /* accumulated ms x1000 (fixed point) */

    /* --- game state ------------------------------------------------- */
    InvMode mode;
    int  testMode;                 /* driven by test-step: no sound, no disk */
    int  held;                     /* staging tableau: sim suspended entirely */
    int  tkFocused;                /* Tk-level focus (set from Tcl bindings) */
    uint32_t rng;
    int  prevKeys;
    uint32_t frameCount;           /* free-running, animates the bombs */

    int  score, hiScore, hiDirty;
    int  lives, wave;

    unsigned char inv[IV_ROWS][IV_COLS];   /* alive flags */
    int   aliveCount;
    float fleetX, fleetY;
    int   fleetDir;                /* +1 right, -1 left */
    int   stepTimer;               /* frames until next fleet step */
    int   animFrame;               /* 0/1, toggles per step */
    int   marchNote;               /* 0..3 rotating bass note */

    float playerX;
    int   deathTimer;              /* MODE_DYING / MODE_WAVE countdown */

    float shotX, shotY; int shotAlive;
    Bomb  bombs[MAX_BOMBS];
    int   bombTimer;
    Boom  booms[MAX_BOOMS];

    float ufoX; int ufoAlive; int ufoDir; int ufoTimer; int ufoSndTimer;
    int frozen;                    /* focus lost mid-game: sim held, PAUSED shown */

    unsigned char shield[SHIELDS][SH_H][SH_W];
} InvWidget;

static ID2D1Factory *g_d2dInv;

/* ---- palette: the Lunar family on a dark field ---------------------------
 * COL_TEXTDIM exists because the light-page slate (#6B7177) drops to a
 * ~3.5:1 contrast on the ink field -- fine for furniture (bunkers, the
 * ground line), too dim for the small pixel font. */
#define COL_FIELD   RGB32(26, 26, 26)     /* INK */
#define COL_SPRITE  RGB32(242, 242, 242)  /* PAGE: the invaders */
#define COL_ACCENT  RGB32(220, 50, 47)    /* signature red: the player */
#define COL_SHIELD  RGB32(107, 113, 119)  /* muted slate: furniture */
#define COL_TEXTDIM RGB32(139, 145, 150)  /* readable slate for field text */
#define COL_WALL    RGB32(58, 62, 66)     /* the playfield's visible frame */
#define COL_AMBER   RGB32(184, 134, 11)   /* WARN: the saucer, the bombs */
#define RGB32(r, g, b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))

static D2D1_COLOR_F colorf(uint32_t argb) {
    D2D1_COLOR_F c = {
        ((argb >> 16) & 0xFF) / 255.0f,
        ((argb >> 8) & 0xFF) / 255.0f,
        (argb & 0xFF) / 255.0f,
        ((argb >> 24) & 0xFF) / 255.0f
    };
    return c;
}

/* ---- sprites --------------------------------------------------------------
 * One uint16 row per scanline, MSB left. These are ORIGINAL designs made
 * for Lunar -- deliberately different silhouettes from the 1978 Taito
 * sprites (round-topped beetle with X-legs, joined-antenna moth, curtain-
 * legged jelly, twin-prong turret, finned slot-window saucer). Evoking
 * the genre is the point; copying the ROM art is not. */
static const int spriteW[3] = { 8, 11, 12 };
#define SPRITE_H 8

static const uint16_t SPR_A[2][SPRITE_H] = {   /* beetle */
    { 0x3C, 0x7E, 0xE7, 0xFF, 0x3C, 0x66, 0xC3, 0x81 },
    { 0x3C, 0x7E, 0xE7, 0xFF, 0x3C, 0x24, 0x99, 0x42 },
};
static const uint16_t SPR_B[2][SPRITE_H] = {   /* moth */
    { 0x202, 0x1FC, 0x3FE, 0x6FB, 0x7FF, 0x1FC, 0x246, 0x489 },
    { 0x202, 0x1FC, 0x3FE, 0x6FB, 0x7FF, 0x1FC, 0x28A, 0x111 },
};
static const uint16_t SPR_C[2][SPRITE_H] = {   /* jelly */
    { 0x1F8, 0x7FE, 0xDFB, 0xFFF, 0xFFF, 0x555, 0x924, 0x249 },
    { 0x1F8, 0x7FE, 0xDFB, 0xFFF, 0xFFF, 0xAAA, 0x492, 0x924 },
};
#define PLAYER_W 13
static const uint16_t SPR_PLAYER[SPRITE_H] = {   /* twin-prong turret */
    0x018C, 0x018C, 0x0FFE, 0x1FFF, 0x1FFF, 0x1BFB, 0x1FFF, 0x1FFF
};
#define UFO_W 16
#define UFO_H 7
static const uint16_t SPR_UFO[UFO_H] = {         /* finned saucer, slot windows */
    0x0FF0, 0x1FF8, 0x7FFE, 0xE7E7, 0xFFFF, 0x6006, 0x300C
};
#define BOOM_W 13
static const uint16_t SPR_BOOM[SPRITE_H] = {
    0x0888, 0x1111, 0x0A8A, 0x0104, 0x1803, 0x0104, 0x0A8A, 0x1111
};
#define BOMB_W 3
#define BOMB_H 6
static const uint16_t SPR_BOMB[2][BOMB_H] = {    /* zigzag squiggle */
    { 4, 2, 1, 2, 4, 2 },
    { 1, 2, 4, 2, 1, 2 },
};

/* 5x7 pixel font, A-Z then 0-9 (one byte per row, low 5 bits, MSB left). */
static const unsigned char FONT[36][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x1E,0x11,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    {0x1F,0x10,0x1E,0x10,0x10,0x10,0x1F}, {0x1F,0x10,0x1E,0x10,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x1F,0x11,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};

/* ---- deterministic RNG (xorshift32, seeded per game) ---------------------- */
static uint32_t rng_next(InvWidget *g) {
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x ? x : 0x1978u;
    return g->rng;
}
static int rng_range(InvWidget *g, int n) { return (int)(rng_next(g) % (uint32_t)n); }

/* ---- sound ----------------------------------------------------------------
 * Synthesized once into static WAV buffers; single-voice PlaySound with
 * newest-wins replacement, silent while a chime is in flight and always
 * silent under test-step. */
#define SFX_RATE 22050
enum { SFX_SHOT, SFX_HIT, SFX_STEP0, SFX_STEP1, SFX_STEP2, SFX_STEP3,
       SFX_DEATH, SFX_UFO, SFX_UFOHIT, SFX_COUNT };
static unsigned char *g_sfx[SFX_COUNT];
static int g_sfxReady = 0;

static void wav_hdr(unsigned char *p, uint32_t dataBytes) {
    memcpy(p, "RIFF", 4); p += 4;
    uint32_t riff = 36 + dataBytes;
    p[0]=riff&0xFF; p[1]=(riff>>8)&0xFF; p[2]=(riff>>16)&0xFF; p[3]=(riff>>24)&0xFF; p += 4;
    memcpy(p, "WAVEfmt ", 8); p += 8;
    const unsigned char fmt[20] = {16,0,0,0, 1,0, 1,0,
        SFX_RATE&0xFF,(SFX_RATE>>8)&0xFF,0,0,
        (SFX_RATE*2)&0xFF,((SFX_RATE*2)>>8)&0xFF,((SFX_RATE*2)>>16)&0xFF,0,
        2,0, 16,0};
    memcpy(p, fmt, 20); p += 20;
    memcpy(p, "data", 4); p += 4;
    p[0]=dataBytes&0xFF; p[1]=(dataBytes>>8)&0xFF; p[2]=(dataBytes>>16)&0xFF; p[3]=(dataBytes>>24)&0xFF;
}

/* freqA->freqB square sweep (freqB==freqA gives a plain square); noise when
 * freqA==0. Linear decay envelope. */
static unsigned char *synth(int ms, float freqA, float freqB, float amp) {
    int frames = SFX_RATE * ms / 1000;
    unsigned char *buf = HeapAlloc(GetProcessHeap(), 0, 44 + (size_t)frames * 2);
    if (!buf) return NULL;
    wav_hdr(buf, (uint32_t)frames * 2);
    unsigned char *p = buf + 44;
    uint32_t noise = 0x1978u;
    float phase = 0.0f;
    for (int i = 0; i < frames; i++) {
        float t = (float)i / (float)frames;
        float env = 1.0f - t;
        float s;
        if (freqA <= 0.0f) {                       /* noise burst */
            noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
            s = ((noise & 0xFFFF) / 32768.0f) - 1.0f;
        } else {
            float f = freqA + (freqB - freqA) * t;
            phase += f / SFX_RATE;
            s = (fmodf(phase, 1.0f) < 0.5f) ? 1.0f : -1.0f;
        }
        int v = (int)(s * env * amp * 32767.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        p[0] = (unsigned char)(v & 0xFF);
        p[1] = (unsigned char)((v >> 8) & 0xFF);
        p += 2;
    }
    return buf;
}

static void sfx_init(void) {
    if (g_sfxReady) return;
    g_sfx[SFX_SHOT]   = synth(70, 1400.0f, 300.0f, 0.16f);
    g_sfx[SFX_HIT]    = synth(70, 0.0f, 0.0f, 0.18f);
    g_sfx[SFX_STEP0]  = synth(50, 130.0f, 130.0f, 0.20f);
    g_sfx[SFX_STEP1]  = synth(50, 116.0f, 116.0f, 0.20f);
    g_sfx[SFX_STEP2]  = synth(50, 104.0f, 104.0f, 0.20f);
    g_sfx[SFX_STEP3]  = synth(50, 92.0f, 92.0f, 0.20f);
    g_sfx[SFX_DEATH]  = synth(450, 0.0f, 0.0f, 0.22f);
    g_sfx[SFX_UFO]    = synth(160, 340.0f, 520.0f, 0.10f);
    g_sfx[SFX_UFOHIT] = synth(220, 250.0f, 1200.0f, 0.18f);
    g_sfxReady = 1;
}

static void sfx_play(InvWidget *g, int which) {
    if (g->testMode || !g_sfxReady || !g_sfx[which]) return;
    if (Lunar_ChimeBusy()) return;    /* the instrument outranks the toy */
    PlaySoundW((LPCWSTR)g_sfx[which], NULL,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* ---- hi-score persistence (%APPDATA%\Lunar\invaders.dat) ------------------ */
static int hiscore_load(void) {
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"invaders.dat")) return 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    char buf[32] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    (void)n;
    int v = atoi(buf);
    return (v > 0 && v < 1000000) ? v : 0;
}

static void hiscore_save(InvWidget *g) {
    if (g->testMode || !g->hiDirty) return;
    wchar_t path[MAX_PATH];
    if (!Lunar_AppDataPathW(path, MAX_PATH, L"invaders.dat")) return;
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d", g->hiScore);
    if (n <= 0) return;
    /* the same atomic replace every Lunar data file uses: this runs on
     * teardown paths where a torn write would eat the record */
    if (Lunar_WriteFileAtomicW(path, buf, (size_t)n)) g->hiDirty = 0;
}

/* ---- game reset ----------------------------------------------------------- */
static void fleet_reset(InvWidget *g) {
    for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++)
            g->inv[r][c] = 1;
    g->aliveCount = IV_ROWS * IV_COLS;
    g->fleetX = 24.0f;
    int drop = (g->wave - 1) * 8;
    if (drop > 64) drop = 64;
    g->fleetY = 40.0f + (float)drop;
    g->fleetDir = 1;
    g->stepTimer = g->aliveCount;
    g->animFrame = 0;
    g->shotAlive = 0;
    for (int i = 0; i < MAX_BOMBS; i++) g->bombs[i].alive = 0;
    for (int i = 0; i < MAX_BOOMS; i++) g->booms[i].ttl = 0;
    g->bombTimer = 90;
    g->ufoAlive = 0;
    g->ufoTimer = 1400 + rng_range(g, 600);
}

static void shields_reset(InvWidget *g) {
    for (int s = 0; s < SHIELDS; s++) {
        for (int y = 0; y < SH_H; y++)
            for (int x = 0; x < SH_W; x++) {
                int on = 1;
                /* stepped top corners */
                if (y < 4 && (x < 4 - y || x >= SH_W - (4 - y))) on = 0;
                /* bottom notch */
                if (y >= SH_H - 5 && x >= 8 && x < SH_W - 8) on = 0;
                g->shield[s][y][x] = (unsigned char)on;
            }
        g->shieldDirty[s] = 1;
    }
}

static void game_reset(InvWidget *g) {
    g->score = 0;
    g->lives = 3;
    g->wave = 1;
    g->rng = 0x1978u;      /* deterministic: every game marches the same */
    g->playerX = (PF_W - PLAYER_W) / 2.0f;
    fleet_reset(g);
    shields_reset(g);
    g->mode = MODE_ATTRACT;
    g->marchNote = 0;
}

/* ---- geometry helpers ------------------------------------------------------ */
/* 4 bunkers of 22 in a 224 field: 32-unit outer margins, 24-unit gaps --
 * exactly symmetric, so the formation centers on the field. */
static float shield_x(int i) { return 32.0f + 46.0f * (float)i; }
#define SHIELD_Y 192.0f
#define PLAYER_Y 232.0f

static int inv_type(int row) { return row == 0 ? 0 : (row <= 2 ? 1 : 2); }
static int inv_points(int row) { return row == 0 ? 30 : (row <= 2 ? 20 : 10); }

static float inv_x(InvWidget *g, int r, int c) {
    return g->fleetX + (float)c * IV_CELL
         + (float)(IV_CELL - spriteW[inv_type(r)]) * 0.5f;
}
static float inv_y(InvWidget *g, int r) {
    return g->fleetY + (float)r * IV_CELL;
}

static int aabb(float ax, float ay, float aw, float ah,
                float bx, float by, float bw, float bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Erode a shield around an impact cell: a 3x3 core plus deterministic
 * speckle, the honest look of taking fire. */
static void shield_damage(InvWidget *g, int s, int cx, int cy) {
    for (int dy = -1; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= SH_W || y < 0 || y >= SH_H) continue;
            if (dx >= -1 && dx <= 1 && dy >= 0 && dy <= 1) {
                g->shield[s][y][x] = 0;
            } else if (rng_range(g, 3) == 0) {
                g->shield[s][y][x] = 0;
            }
        }
    g->shieldDirty[s] = 1;
}

/* One cell probe. Returns 1 when a solid cell absorbed the hit. */
static int shield_probe(InvWidget *g, float x, float y, int dirDown) {
    for (int s = 0; s < SHIELDS; s++) {
        float sx = shield_x(s);
        if (x < sx || x >= sx + SH_W) continue;
        if (y < SHIELD_Y || y >= SHIELD_Y + SH_H) continue;
        int cx = (int)(x - sx);
        int cy = (int)(y - SHIELD_Y);
        if (cx < 0 || cx >= SH_W || cy < 0 || cy >= SH_H) continue;
        if (!g->shield[s][cy][cx]) continue;
        shield_damage(g, s, cx, cy + (dirDown ? 0 : -1));
        return 1;
    }
    return 0;
}

/* Bullet/bomb vs shields, SWEPT over every cell the projectile crossed
 * this frame -- point-sampling at the projectile's fixed per-frame phase
 * would leave most shield rows untestable and let shots tunnel through
 * drawn-solid pixels (spawn y and speed are both constants, so the
 * sampled rows repeat forever). span covers the distance travelled;
 * halfW covers the projectile's width beyond its centerline. */
static int shield_absorb(InvWidget *g, float x, float y, float span,
                         float halfW, int dirDown) {
    float dir = dirDown ? 1.0f : -1.0f;
    for (float k = 0.0f; k <= span; k += 1.0f) {
        float yy = y - dir * k;   /* walk back along the path travelled */
        for (float dx = -halfW; dx <= halfW; dx += 1.0f) {
            if (shield_probe(g, x + dx, yy, dirDown)) return 1;
        }
    }
    return 0;
}

static void boom_at(InvWidget *g, float x, float y) {
    for (int i = 0; i < MAX_BOOMS; i++) {
        if (g->booms[i].ttl <= 0) {
            g->booms[i].x = x;
            g->booms[i].y = y;
            g->booms[i].ttl = 14;
            return;
        }
    }
}

/* ---- one fixed 60 Hz simulation step -------------------------------------- */
static void player_hit(InvWidget *g) {
    g->lives--;
    g->mode = MODE_DYING;
    g->deathTimer = 70;
    boom_at(g, g->playerX + PLAYER_W * 0.5f - BOOM_W * 0.5f, PLAYER_Y);
    for (int i = 0; i < MAX_BOMBS; i++) g->bombs[i].alive = 0;
    g->shotAlive = 0;
    sfx_play(g, SFX_DEATH);
}

static void fleet_step(InvWidget *g) {
    /* Edge test against the NEXT position of the outermost alive columns. */
    float minX = PF_W, maxX = 0.0f;
    for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++) {
            if (!g->inv[r][c]) continue;
            float x = inv_x(g, r, c);
            if (x < minX) minX = x;
            if (x + spriteW[inv_type(r)] > maxX) maxX = x + spriteW[inv_type(r)];
        }
    float dx = 4.0f * (float)g->fleetDir;
    if (minX + dx < 6.0f || maxX + dx > PF_W - 6.0f) {
        g->fleetDir = -g->fleetDir;
        g->fleetY += 8.0f;
    } else {
        g->fleetX += dx;
    }
    g->animFrame ^= 1;
    sfx_play(g, SFX_STEP0 + g->marchNote);
    g->marchNote = (g->marchNote + 1) & 3;

    /* Marching through a shield grinds it away. */
    for (int r = 0; r < IV_ROWS; r++)
        for (int c = 0; c < IV_COLS; c++) {
            if (!g->inv[r][c]) continue;
            float iy = inv_y(g, r);
            if (iy + SPRITE_H < SHIELD_Y || iy > SHIELD_Y + SH_H) continue;
            float ix = inv_x(g, r, c);
            for (int s = 0; s < SHIELDS; s++) {
                float sx = shield_x(s);
                if (ix + spriteW[inv_type(r)] < sx || ix > sx + SH_W) continue;
                for (int px = 0; px < spriteW[inv_type(r)]; px++) {
                    int cx = (int)(ix + px - sx);
                    if (cx < 0 || cx >= SH_W) continue;
                    for (int py = 0; py < SPRITE_H; py++) {
                        int cy = (int)(iy + py - SHIELD_Y);
                        if (cy >= 0 && cy < SH_H && g->shield[s][cy][cx]) {
                            g->shield[s][cy][cx] = 0;
                            g->shieldDirty[s] = 1;
                        }
                    }
                }
            }
        }

    /* Landing ends the game -- the fleet reached the cannon's row. */
    for (int r = IV_ROWS - 1; r >= 0; r--)
        for (int c = 0; c < IV_COLS; c++)
            if (g->inv[r][c] && inv_y(g, r) + SPRITE_H >= PLAYER_Y) {
                g->lives = 0;
                g->mode = MODE_OVER;
                g->ufoAlive = 0;   /* nothing hovers over GAME OVER */
                if (g->score > g->hiScore) { g->hiScore = g->score; g->hiDirty = 1; }
                hiscore_save(g);
                return;
            }
}

static void sim_step(InvWidget *g, int keys) {
    int pressed = keys & ~g->prevKeys;
    g->prevKeys = keys;
    g->frameCount++;

    /* explosions decay in EVERY mode -- a boom must never freeze on the
     * wave pause or the game-over screen */
    for (int i = 0; i < MAX_BOOMS; i++)
        if (g->booms[i].ttl > 0) g->booms[i].ttl--;

    switch (g->mode) {
    case MODE_ATTRACT:
        if (pressed & KEY_START) {
            g->score = 0; g->lives = 3; g->wave = 1;
            g->rng = 0x1978u;
            g->playerX = (PF_W - PLAYER_W) / 2.0f;
            fleet_reset(g);
            shields_reset(g);
            g->mode = MODE_PLAYING;
        }
        return;
    case MODE_OVER:
        if (pressed & KEY_START) { game_reset(g); }
        return;
    case MODE_WAVE:
        if (--g->deathTimer <= 0) {
            g->wave++;
            fleet_reset(g);
            g->mode = MODE_PLAYING;
        }
        return;
    case MODE_DYING:
        if (--g->deathTimer <= 0) {
            if (g->lives <= 0) {
                g->mode = MODE_OVER;
                if (g->score > g->hiScore) { g->hiScore = g->score; g->hiDirty = 1; }
                hiscore_save(g);
            } else {
                g->playerX = (PF_W - PLAYER_W) / 2.0f;
                g->mode = MODE_PLAYING;
            }
        }
        return;
    case MODE_PLAYING:
        break;
    }

    /* player */
    if (keys & KEY_LEFT)  g->playerX -= 1.25f;
    if (keys & KEY_RIGHT) g->playerX += 1.25f;
    if (g->playerX < 6.0f) g->playerX = 6.0f;
    if (g->playerX > PF_W - 6.0f - PLAYER_W) g->playerX = PF_W - 6.0f - PLAYER_W;
    if ((keys & KEY_FIRE) && !g->shotAlive) {
        g->shotAlive = 1;
        g->shotX = g->playerX + PLAYER_W * 0.5f;
        g->shotY = PLAYER_Y - 4.0f;
        sfx_play(g, SFX_SHOT);
    }

    /* fleet cadence: one step every aliveCount frames (min 3) -- the fewer
     * they are, the faster they come, and the march music with them */
    if (--g->stepTimer <= 0) {
        fleet_step(g);
        if (g->mode != MODE_PLAYING) return;   /* landed */
        g->stepTimer = g->aliveCount < 3 ? 3 : g->aliveCount;
    }

    /* player shot */
    if (g->shotAlive) {
        g->shotY -= 5.0f;
        if (g->shotY < 12.0f) g->shotAlive = 0;
        else if (shield_absorb(g, g->shotX, g->shotY, 5.0f, 0.0f, 0)) g->shotAlive = 0;
        else {
            /* saucer? */
            if (g->ufoAlive &&
                aabb(g->shotX - 0.5f, g->shotY, 1, 4, g->ufoX, 18, UFO_W, UFO_H)) {
                static const int prize[4] = { 50, 100, 150, 300 };
                g->score += prize[rng_range(g, 4)];
                if (g->score > g->hiScore) { g->hiScore = g->score; g->hiDirty = 1; }
                g->ufoAlive = 0;
                g->shotAlive = 0;
                boom_at(g, g->ufoX + UFO_W * 0.5f - BOOM_W * 0.5f, 18.0f);
                sfx_play(g, SFX_UFOHIT);
            }
            /* invaders: scan bottom-up so the closest one takes the hit */
            for (int r = IV_ROWS - 1; r >= 0 && g->shotAlive; r--)
                for (int c = 0; c < IV_COLS && g->shotAlive; c++) {
                    if (!g->inv[r][c]) continue;
                    float ix = inv_x(g, r, c), iy = inv_y(g, r);
                    if (aabb(g->shotX - 0.5f, g->shotY, 1, 4,
                             ix, iy, (float)spriteW[inv_type(r)], SPRITE_H)) {
                        g->inv[r][c] = 0;
                        g->aliveCount--;
                        g->score += inv_points(r);
                        if (g->score > g->hiScore) { g->hiScore = g->score; g->hiDirty = 1; }
                        g->shotAlive = 0;
                        boom_at(g, ix + spriteW[inv_type(r)] * 0.5f - BOOM_W * 0.5f, iy);
                        sfx_play(g, SFX_HIT);
                    }
                }
        }
    }

    /* wave cleared */
    if (g->aliveCount == 0) {
        g->mode = MODE_WAVE;
        g->deathTimer = 90;
        g->ufoAlive = 0;   /* never a saucer parked over the pause */
        return;
    }

    /* bombs: dropped from the lowest alive invader of a random column */
    if (--g->bombTimer <= 0) {
        g->bombTimer = 24 + rng_range(g, 70) + g->aliveCount;
        int tries = 4;
        while (tries--) {
            int c = rng_range(g, IV_COLS);
            for (int r = IV_ROWS - 1; r >= 0; r--) {
                if (!g->inv[r][c]) continue;
                for (int i = 0; i < MAX_BOMBS; i++) {
                    if (!g->bombs[i].alive) {
                        g->bombs[i].alive = 1;
                        g->bombs[i].x = inv_x(g, r, c) + spriteW[inv_type(r)] * 0.5f;
                        g->bombs[i].y = inv_y(g, r) + SPRITE_H;
                        i = MAX_BOMBS; tries = 0;
                    }
                }
                break;
            }
        }
    }
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!g->bombs[i].alive) continue;
        g->bombs[i].y += 2.0f;
        float bx = g->bombs[i].x, by = g->bombs[i].y;
        if (by > PF_H - 10.0f) { g->bombs[i].alive = 0; continue; }
        if (shield_absorb(g, bx, by + 4.0f, 2.0f, 1.0f, 1)) { g->bombs[i].alive = 0; continue; }
        /* a bomb and the player's shot can cancel each other */
        if (g->shotAlive &&
            aabb(bx - 1.5f, by, 3, 6, g->shotX - 0.5f, g->shotY, 1, 4)) {
            g->bombs[i].alive = 0;
            g->shotAlive = 0;
            boom_at(g, bx - BOOM_W * 0.5f, by);
            continue;
        }
        if (aabb(bx - 1.5f, by, 3, 6, g->playerX, PLAYER_Y, PLAYER_W, SPRITE_H)) {
            g->bombs[i].alive = 0;
            player_hit(g);
            return;
        }
    }

    /* saucer */
    if (g->ufoAlive) {
        g->ufoX += 0.75f * (float)g->ufoDir;
        if (g->ufoX < -(float)UFO_W || g->ufoX > (float)PF_W) g->ufoAlive = 0;
        if (--g->ufoSndTimer <= 0) { sfx_play(g, SFX_UFO); g->ufoSndTimer = 14; }
    } else if (g->aliveCount >= 8 && --g->ufoTimer <= 0) {
        g->ufoAlive = 1;
        g->ufoDir = rng_range(g, 2) ? 1 : -1;
        g->ufoX = g->ufoDir > 0 ? -(float)UFO_W : (float)PF_W;
        g->ufoTimer = 1400 + rng_range(g, 600);
        g->ufoSndTimer = 1;
    }

    for (int i = 0; i < MAX_BOOMS; i++)
        if (g->booms[i].ttl > 0) g->booms[i].ttl--;
}

/* ---- Direct2D assets -------------------------------------------------------- */
static ID2D1Bitmap *make_bitmap(InvWidget *g, const uint16_t *rows,
                                int w, int h, uint32_t argb) {
    uint32_t pixels[16 * 16];
    if (w > 16 || h > 16) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            pixels[y * w + x] = (rows[y] >> (w - 1 - x)) & 1 ? argb : 0;
    D2D1_SIZE_U size = { (UINT32)w, (UINT32)h };
    D2D1_BITMAP_PROPERTIES props = {
        { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
        96.0f, 96.0f
    };
    ID2D1Bitmap *bm = NULL;
    if (FAILED(ID2D1RenderTarget_CreateBitmap((ID2D1RenderTarget *)g->target,
            size, pixels, (UINT32)(w * 4), &props, &bm))) return NULL;
    return bm;
}

static void shield_bitmap_update(InvWidget *g, int s) {
    if (!g->bmShield[s]) return;
    uint32_t pixels[SH_W * SH_H];
    for (int y = 0; y < SH_H; y++)
        for (int x = 0; x < SH_W; x++)
            pixels[y * SH_W + x] = g->shield[s][y][x] ? COL_SHIELD : 0;
    D2D1_RECT_U box = { 0, 0, SH_W, SH_H };
    ID2D1Bitmap_CopyFromMemory(g->bmShield[s], &box, pixels, SH_W * 4);
    g->shieldDirty[s] = 0;
}

static void discard_assets(InvWidget *g) {
    for (int t = 0; t < 3; t++)
        for (int f = 0; f < 2; f++)
            if (g->bmInv[t][f]) { ID2D1Bitmap_Release(g->bmInv[t][f]); g->bmInv[t][f] = NULL; }
    if (g->bmPlayer) { ID2D1Bitmap_Release(g->bmPlayer); g->bmPlayer = NULL; }
    if (g->bmUfo)    { ID2D1Bitmap_Release(g->bmUfo);    g->bmUfo = NULL; }
    if (g->bmBoom)   { ID2D1Bitmap_Release(g->bmBoom);   g->bmBoom = NULL; }
    for (int f = 0; f < 2; f++)
        if (g->bmBomb[f]) { ID2D1Bitmap_Release(g->bmBomb[f]); g->bmBomb[f] = NULL; }
    for (int s = 0; s < SHIELDS; s++)
        if (g->bmShield[s]) { ID2D1Bitmap_Release(g->bmShield[s]); g->bmShield[s] = NULL; }
}

static int create_assets(InvWidget *g) {
    g->bmInv[0][0] = make_bitmap(g, SPR_A[0], spriteW[0], SPRITE_H, COL_SPRITE);
    g->bmInv[0][1] = make_bitmap(g, SPR_A[1], spriteW[0], SPRITE_H, COL_SPRITE);
    g->bmInv[1][0] = make_bitmap(g, SPR_B[0], spriteW[1], SPRITE_H, COL_SPRITE);
    g->bmInv[1][1] = make_bitmap(g, SPR_B[1], spriteW[1], SPRITE_H, COL_SPRITE);
    g->bmInv[2][0] = make_bitmap(g, SPR_C[0], spriteW[2], SPRITE_H, COL_SPRITE);
    g->bmInv[2][1] = make_bitmap(g, SPR_C[1], spriteW[2], SPRITE_H, COL_SPRITE);
    g->bmPlayer = make_bitmap(g, SPR_PLAYER, PLAYER_W, SPRITE_H, COL_ACCENT);
    g->bmUfo    = make_bitmap(g, SPR_UFO, UFO_W, UFO_H, COL_AMBER);
    g->bmBoom   = make_bitmap(g, SPR_BOOM, BOOM_W, SPRITE_H, COL_SPRITE);
    g->bmBomb[0] = make_bitmap(g, SPR_BOMB[0], BOMB_W, BOMB_H, COL_AMBER);
    g->bmBomb[1] = make_bitmap(g, SPR_BOMB[1], BOMB_W, BOMB_H, COL_AMBER);
    for (int s = 0; s < SHIELDS; s++) {
        D2D1_SIZE_U size = { SH_W, SH_H };
        D2D1_BITMAP_PROPERTIES props = {
            { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            96.0f, 96.0f
        };
        if (FAILED(ID2D1RenderTarget_CreateBitmap((ID2D1RenderTarget *)g->target,
                size, NULL, 0, &props, &g->bmShield[s]))) return 0;
        g->shieldDirty[s] = 1;
    }
    if (!(g->bmPlayer && g->bmUfo && g->bmBoom)) return 0;
    for (int t = 0; t < 3; t++)
        for (int f = 0; f < 2; f++)
            if (!g->bmInv[t][f]) return 0;   /* frame 1 too, or the fleet
                                              * blinks invisible every
                                              * other march step */
    return g->bmBomb[0] && g->bmBomb[1];
}

static void discard_target(InvWidget *g) {
    discard_assets(g);
    if (g->brush)  { ID2D1SolidColorBrush_Release(g->brush); g->brush = NULL; }
    if (g->target) { ID2D1HwndRenderTarget_Release(g->target); g->target = NULL; }
    g->targetWidth = g->targetHeight = 0;
}

static HRESULT create_target(InvWidget *g, HWND hwnd, int width, int height) {
    if (!g_d2dInv) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       &IID_ID2D1Factory, NULL, (void **)&g_d2dInv);
        if (FAILED(hr)) return hr;
    }
    D2D1_RENDER_TARGET_PROPERTIES properties = {
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED },
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    };
    D2D1_SIZE_U size = { (UINT32)width, (UINT32)height };
    /* IMMEDIATELY: pacing comes from the frame timer; two vsync-waiting
     * targets (dial + game) must not stack their waits on Tk's thread. */
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties = {
        hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY
    };
    HRESULT hr = ID2D1Factory_CreateHwndRenderTarget(g_d2dInv, &properties,
                                                     &hwndProperties, &g->target);
    if (FAILED(hr)) return hr;
    D2D1_COLOR_F initial = colorf(COL_SPRITE);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g->target, &initial,
                                                     NULL, &g->brush);
    if (SUCCEEDED(hr) && !create_assets(g)) hr = E_FAIL;
    if (FAILED(hr)) { discard_target(g); return hr; }
    g->targetWidth = width;
    g->targetHeight = height;
    return S_OK;
}

/* ---- drawing ----------------------------------------------------------------- */
static void draw_bitmap(InvWidget *g, ID2D1Bitmap *bm, float x, float y,
                        float w, float h) {
    if (!bm) return;
    D2D1_RECT_F dst = { x, y, x + w, y + h };
    ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)g->target, bm, &dst, 1.0f,
        g->bmInterp, NULL);
}

static void fill_rect(InvWidget *g, float x, float y, float w, float h,
                      uint32_t argb) {
    D2D1_COLOR_F c = colorf(argb);
    ID2D1SolidColorBrush_SetColor(g->brush, &c);
    D2D1_RECT_F r = { x, y, x + w, y + h };
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)g->target, &r,
                                    (ID2D1Brush *)g->brush);
}

static void draw_text_s(InvWidget *g, const char *s, float x, float y,
                        float scale, uint32_t argb) {
    for (; *s; s++, x += 6.0f * scale) {
        int idx;
        if (*s >= 'A' && *s <= 'Z') idx = *s - 'A';
        else if (*s >= '0' && *s <= '9') idx = 26 + (*s - '0');
        else continue;   /* space and anything else advances silently */
        for (int row = 0; row < 7; row++) {
            unsigned char bits = FONT[idx][row];
            for (int col = 0; col < 5; col++)
                if ((bits >> (4 - col)) & 1)
                    fill_rect(g, x + (float)col * scale, y + (float)row * scale,
                              scale, scale, argb);
        }
    }
}

static void draw_text(InvWidget *g, const char *s, float x, float y,
                      uint32_t argb) {
    draw_text_s(g, s, x, y, 1.0f, argb);
}

static float text_w_s(const char *s, float scale) {
    return ((float)strlen(s) * 6.0f - 1.0f) * scale;
}
static float text_w(const char *s) { return text_w_s(s, 1.0f); }
static void draw_text_center_s(InvWidget *g, const char *s, float y,
                               float scale, uint32_t argb) {
    draw_text_s(g, s, ((float)PF_W - text_w_s(s, scale)) * 0.5f, y, scale, argb);
}
static void draw_text_center(InvWidget *g, const char *s, float y, uint32_t argb) {
    draw_text_center_s(g, s, y, 1.0f, argb);
}

static void pump_manage(InvWidget *g);

static void inv_redraw(void *clientData) {
    InvWidget *g = clientData;
    g->redrawPending = 0;
    pump_manage(g);
    if (!g->tkwin || !Tk_IsMapped(g->tkwin)) return;
    Tk_MakeWindowExist(g->tkwin);
    HWND hwnd = Tk_GetHWND(Tk_WindowId(g->tkwin));
    if (!IsWindow(hwnd)) return;

    RECT client;
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width < 1 || height < 1) return;
    if (g->target && (g->targetWidth != width || g->targetHeight != height))
        discard_target(g);
    if (!g->target && FAILED(create_target(g, hwnd, width, height))) return;

    for (int s = 0; s < SHIELDS; s++)
        if (g->shieldDirty[s]) shield_bitmap_update(g, s);

    ID2D1RenderTarget *target = (ID2D1RenderTarget *)g->target;
    ID2D1RenderTarget_BeginDraw(target);
    D2D1_COLOR_F field = colorf(COL_FIELD);
    ID2D1RenderTarget_Clear(target, &field);
    float sx = (float)width / (float)PF_W;
    float sy = (float)height / (float)PF_H;
    float sc = sx < sy ? sx : sy;
    /* Integer scale: hard pixels (aliased, nearest). Fractional scale:
     * uneven nearest-neighbour cells make identical sprites render
     * unequal, so trade crispness for evenness (AA + linear) there. */
    int crisp = sc >= 1.0f && fabsf(sc - floorf(sc + 0.5f)) < 0.01f;
    g->bmInterp = crisp ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                      : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    ID2D1RenderTarget_SetAntialiasMode(target,
        crisp ? D2D1_ANTIALIAS_MODE_ALIASED : D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F m = { 0 };
    m._11 = sc; m._22 = sc;
    m._31 = ((float)width - (float)PF_W * sc) * 0.5f;
    m._32 = ((float)height - (float)PF_H * sc) * 0.5f;
    ID2D1RenderTarget_SetTransform(target, &m);

    /* the playfield's frame: the walls the fleet bounces off, made visible */
    fill_rect(g, 0.0f, 0.0f, (float)PF_W, 1.0f, COL_WALL);
    fill_rect(g, 0.0f, (float)PF_H - 1.0f, (float)PF_W, 1.0f, COL_WALL);
    fill_rect(g, 0.0f, 0.0f, 1.0f, (float)PF_H, COL_WALL);
    fill_rect(g, (float)PF_W - 1.0f, 0.0f, 1.0f, (float)PF_H, COL_WALL);

    /* HUD on a single grid: everything flush to x=6 / PF_W-6 */
    char line[48];
    snprintf(line, sizeof line, "SCORE %04d", g->score);
    draw_text(g, line, 6.0f, 5.0f, COL_SPRITE);
    snprintf(line, sizeof line, "HI %04d", g->hiScore);
    draw_text(g, line, (float)PF_W - 6.0f - text_w(line), 5.0f, COL_TEXTDIM);

    if (g->mode == MODE_ATTRACT) {
        draw_text_center_s(g, "INVADERS", 76.0f, 2.0f, COL_SPRITE);
        draw_text_center(g, "A LUNAR INTERMISSION", 100.0f, COL_TEXTDIM);
        draw_text_center(g, "PRESS ENTER", 138.0f, COL_ACCENT);
        draw_text_center(g, "ARROWS MOVE  SPACE FIRES", 156.0f, COL_TEXTDIM);
        draw_text_center(g, "ESC RETURNS TO THE CLOCK", 168.0f, COL_TEXTDIM);
    } else {
        /* fleet */
        for (int r = 0; r < IV_ROWS; r++) {
            int t = inv_type(r);
            for (int c = 0; c < IV_COLS; c++)
                if (g->inv[r][c])
                    draw_bitmap(g, g->bmInv[t][g->animFrame],
                                inv_x(g, r, c), inv_y(g, r),
                                (float)spriteW[t], (float)SPRITE_H);
        }
        /* saucer */
        if (g->ufoAlive)
            draw_bitmap(g, g->bmUfo, g->ufoX, 18.0f, (float)UFO_W, (float)UFO_H);
        /* shields */
        for (int s = 0; s < SHIELDS; s++)
            draw_bitmap(g, g->bmShield[s], shield_x(s), SHIELD_Y,
                        (float)SH_W, (float)SH_H);
        /* player (hidden while dying -- the explosion stands in) */
        if (g->mode != MODE_DYING)
            draw_bitmap(g, g->bmPlayer, g->playerX, PLAYER_Y,
                        (float)PLAYER_W, (float)SPRITE_H);
        /* projectiles */
        if (g->shotAlive)
            fill_rect(g, g->shotX - 0.5f, g->shotY, 1.0f, 4.0f, COL_SPRITE);
        for (int i = 0; i < MAX_BOMBS; i++)
            if (g->bombs[i].alive)
                draw_bitmap(g, g->bmBomb[(g->frameCount >> 3) & 1],
                            g->bombs[i].x - 1.5f, g->bombs[i].y,
                            (float)BOMB_W, (float)BOMB_H);
        /* explosions */
        for (int i = 0; i < MAX_BOOMS; i++)
            if (g->booms[i].ttl > 0)
                draw_bitmap(g, g->bmBoom, g->booms[i].x, g->booms[i].y,
                            (float)BOOM_W, (float)SPRITE_H);
        /* lives + wave, flush to the same x=6 / PF_W-6 grid as the HUD */
        for (int i = 0; i < g->lives - 1 && i < 4; i++)
            draw_bitmap(g, g->bmPlayer, 6.0f + (float)i * 16.0f, PF_H - 12.0f,
                        (float)PLAYER_W * 0.8f, (float)SPRITE_H * 0.8f);
        snprintf(line, sizeof line, "W%d", g->wave);
        draw_text(g, line, (float)PF_W - 6.0f - text_w(line), PF_H - 12.0f,
                  COL_TEXTDIM);
        fill_rect(g, 1.0f, PF_H - 15.0f, (float)PF_W - 2.0f, 1.0f, COL_SHIELD);

        if (g->mode == MODE_OVER) {
            draw_text_center_s(g, "GAME OVER", 104.0f, 2.0f, COL_ACCENT);
            draw_text_center(g, "PRESS ENTER", 130.0f, COL_TEXTDIM);
        } else if (g->frozen && !g->held) {
            draw_text_center(g, "PAUSED", 120.0f, COL_AMBER);
        }
    }

    D2D1_MATRIX_3X2_F identity = { 0 };
    identity._11 = 1.0f; identity._22 = 1.0f;
    ID2D1RenderTarget_SetTransform(target, &identity);
    HRESULT hr = ID2D1RenderTarget_EndDraw(target, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        discard_target(g);
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void request_redraw(InvWidget *g) {
    if (!g->redrawPending) {
        g->redrawPending = 1;
        Tcl_DoWhenIdle(inv_redraw, g);
    }
}

/* ---- the frame pump ----------------------------------------------------------
 * QPC-accumulated fixed timestep: the timer's jitter changes how many sim
 * steps run per pump, never how far the world advances per step. */
static int64_t inv_qpc_freq(void) {
    static int64_t f = 0;
    if (!f) { LARGE_INTEGER li; QueryPerformanceFrequency(&li); f = li.QuadPart ? li.QuadPart : 1; }
    return f;
}

static int game_focused(InvWidget *g) {
    /* Two gates: the toplevel must own the Win32 focus AND Tk's internal
     * focus must be on this widget (Tk parks Win32 focus on the toplevel
     * wrapper, so the root check alone would keep feeding the game while
     * e.g. the status-bar gear button is the focused widget and Space
     * would both fire and press the button). tkFocused is maintained by
     * <FocusIn>/<FocusOut> bindings on the Tcl side. */
    if (!g->tkFocused) return 0;
    HWND hwnd = Tk_GetHWND(Tk_WindowId(g->tkwin));
    HWND focusRoot = GetAncestor(GetFocus(), GA_ROOT);
    return focusRoot && focusRoot == GetAncestor(hwnd, GA_ROOT);
}

static int live_keys(InvWidget *g) {
    /* only when the game's toplevel owns the focus -- a background easter
     * egg must never eat keystrokes */
    if (!game_focused(g)) return 0;
    int keys = 0;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)  keys |= KEY_LEFT;
    if (GetAsyncKeyState('A') & 0x8000)      keys |= KEY_LEFT;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) keys |= KEY_RIGHT;
    if (GetAsyncKeyState('D') & 0x8000)      keys |= KEY_RIGHT;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) keys |= KEY_FIRE;
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) keys |= KEY_START;
    return keys;
}

static void pump_tick(void *clientData) {
    InvWidget *g = clientData;
    g->pump = NULL;
    if (!g->tkwin || !Tk_IsMapped(g->tkwin)) {
        g->lastQpc = 0;   /* a later remap must not fast-forward the gap */
        return;
    }

    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    int64_t now = li.QuadPart;
    if (g->lastQpc) {
        int64_t elapsed1000 = (now - g->lastQpc) * 1000000 / inv_qpc_freq();
        if (elapsed1000 < 0) elapsed1000 = 0;
        if (elapsed1000 > 250000) elapsed1000 = 250000;   /* stall cap: 250 ms */
        g->accumMs1000 += elapsed1000;
    }
    g->lastQpc = now;

    /* A held tableau (screenshot staging) never simulates at all. */
    if (g->held) {
        g->accumMs1000 = 0;
        request_redraw(g);
        return;
    }

    /* Focus lost mid-game: hold the simulation (an unattended intermission
     * must not lose lives in the background) and drop the accumulated debt
     * so refocusing never fast-forwards a burst of catch-up frames. */
    g->frozen = (g->mode == MODE_PLAYING && !game_focused(g));
    if (g->frozen) {
        g->accumMs1000 = 0;
    } else {
        const int64_t stepMs1000 = 1000000 * FRAME_MS_NUM / (FRAME_MS_DEN * 1000);
        int keys = live_keys(g);
        int steps = 0;
        while (g->accumMs1000 >= stepMs1000 && steps < 5) {
            sim_step(g, keys);
            g->accumMs1000 -= stepMs1000;
            steps++;
        }
        if (steps == 5) g->accumMs1000 = 0;   /* fell behind: drop the debt */
    }

    request_redraw(g);   /* re-arms the pump via pump_manage */
}

static void pump_manage(InvWidget *g) {
    int want = g->tkwin && Tk_IsMapped(g->tkwin);
    if (want && !g->pump) {
        g->pump = Tcl_CreateTimerHandler(PUMP_MS, pump_tick, g);
    } else if (!want && g->pump) {
        Tcl_DeleteTimerHandler(g->pump);
        g->pump = NULL;
        g->lastQpc = 0;
    }
}

static void inv_event(void *clientData, XEvent *eventPtr) {
    InvWidget *g = clientData;
    if (eventPtr->type == Expose || eventPtr->type == ConfigureNotify) {
        request_redraw(g);
    } else if (eventPtr->type == DestroyNotify) {
        g->tkwin = NULL;
        if (g->command) Tcl_DeleteCommandFromToken(g->interp, g->command);
    }
}

static void inv_command_deleted(void *clientData) {
    InvWidget *g = clientData;
    hiscore_save(g);
    if (g->pump) { Tcl_DeleteTimerHandler(g->pump); g->pump = NULL; }
    if (g->redrawPending) Tcl_CancelIdleCall(inv_redraw, g);
    if (g->tkwin) {
        Tk_DeleteEventHandler(g->tkwin, ExposureMask | StructureNotifyMask,
                              inv_event, g);
        Tk_DestroyWindow(g->tkwin);
        g->tkwin = NULL;
    }
    discard_target(g);
    ckfree(g);
}

static const char *mode_name(InvMode m) {
    switch (m) {
        case MODE_ATTRACT: return "attract";
        case MODE_PLAYING: return "playing";
        case MODE_DYING:   return "dying";
        case MODE_WAVE:    return "wave";
        case MODE_OVER:    return "over";
    }
    return "?";
}

static int inv_widget_command(void *clientData, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[]) {
    InvWidget *g = clientData;
    static const char *usage =
        "state | redraw | test-step steps keys | hold | focused 0|1";
    if (objc < 2) { Tcl_WrongNumArgs(interp, 1, objv, usage); return TCL_ERROR; }
    const char *sub = Tcl_GetString(objv[1]);
    if (strcmp(sub, "redraw") == 0 && objc == 2) {
        request_redraw(g);
        return TCL_OK;
    }
    if (strcmp(sub, "hold") == 0 && objc == 2) {
        /* freeze the staged tableau: no simulation, no PAUSED banner */
        g->held = 1;
        request_redraw(g);
        return TCL_OK;
    }
    if (strcmp(sub, "focused") == 0 && objc == 3) {
        int f;
        if (Tcl_GetBooleanFromObj(interp, objv[2], &f) != TCL_OK) return TCL_ERROR;
        g->tkFocused = f;
        return TCL_OK;
    }
    if (strcmp(sub, "state") == 0 && objc == 2) {
        Tcl_Obj *d = Tcl_NewDictObj();
#define IPUT(k, v) Tcl_DictObjPut(interp, d, Tcl_NewStringObj((k), -1), (v))
        IPUT("mode",  Tcl_NewStringObj(mode_name(g->mode), -1));
        IPUT("score", Tcl_NewIntObj(g->score));
        IPUT("hiscore", Tcl_NewIntObj(g->hiScore));
        IPUT("lives", Tcl_NewIntObj(g->lives));
        IPUT("wave",  Tcl_NewIntObj(g->wave));
        IPUT("alive", Tcl_NewIntObj(g->aliveCount));
#undef IPUT
        Tcl_SetObjResult(interp, d);
        return TCL_OK;
    }
    if (strcmp(sub, "test-step") == 0 && objc == 4) {
        int steps, keys;
        if (Tcl_GetIntFromObj(interp, objv[2], &steps) != TCL_OK ||
            Tcl_GetIntFromObj(interp, objv[3], &keys) != TCL_OK) return TCL_ERROR;
        if (steps < 0 || steps > 100000) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("steps out of range", -1));
            return TCL_ERROR;
        }
        g->testMode = 1;   /* silent + no disk writes from here on */
        for (int i = 0; i < steps; i++) sim_step(g, keys);
        request_redraw(g);
        return TCL_OK;
    }
    Tcl_WrongNumArgs(interp, 1, objv, usage);
    return TCL_ERROR;
}

static int inv_create_command(void *clientData, Tcl_Interp *interp,
                              int objc, Tcl_Obj *const objv[]) {
    Tk_Window mainWindow = clientData;
    if (objc < 2 || (objc - 2) % 2 != 0) {
        Tcl_WrongNumArgs(interp, 1, objv, "pathName ?-width pixels -height pixels?");
        return TCL_ERROR;
    }
    int width = PF_W * 2, height = PF_H * 2;
    for (int i = 2; i < objc; i += 2) {
        const char *option = Tcl_GetString(objv[i]);
        if (strcmp(option, "-width") == 0) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &width) != TCL_OK) return TCL_ERROR;
        } else if (strcmp(option, "-height") == 0) {
            if (Tcl_GetIntFromObj(interp, objv[i + 1], &height) != TCL_OK) return TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option %s", option));
            return TCL_ERROR;
        }
    }
    if (width < PF_W || height < PF_H) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("invaders widget below native size", -1));
        return TCL_ERROR;
    }
    Tk_Window window = Tk_CreateWindowFromPath(interp, mainWindow,
                                               Tcl_GetString(objv[1]), NULL);
    if (!window) return TCL_ERROR;
    InvWidget *g = ckalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->tkwin = window;
    g->interp = interp;
    g->width = width;
    g->height = height;
    g->hiScore = hiscore_load();
    game_reset(g);
    sfx_init();
    Tk_SetClass(window, "LunarInvaders");
    Tk_GeometryRequest(window, width, height);
    Tk_CreateEventHandler(window, ExposureMask | StructureNotifyMask,
                          inv_event, g);
    g->command = Tcl_CreateObjCommand(interp, Tk_PathName(window),
                                      inv_widget_command, g,
                                      inv_command_deleted);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(Tk_PathName(window), -1));
    return TCL_OK;
}

int LunarInvaders_Init(Tcl_Interp *interp) {
    if (Tk_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
    /* Global widget-class command like lunarclock: never inside ::lunar,
     * where it would shadow same-named Tcl commands for the namespace. */
    Tcl_CreateObjCommand(interp, "lunarinvaders", inv_create_command,
                         Tk_MainWindow(interp), NULL);
    return TCL_OK;
}
