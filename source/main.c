/*
 * STORMFIRE - Nintendo Wii platform layer (devkitPPC + libogc).
 *
 *   video : the game's triangle list is drawn by the Wii's GPU (GX): flat/gouraud
 *           shaded triangles with normal and additive blending, 60 fps, 16:9
 *           anamorphic (or letterboxed inside 4:3 - press MINUS)
 *   input : Wii Classic Controller (plugged into Wii Remote 1): buttons + left stick
 *   audio : synthesized effects played through ASND, pitch-shifted and panned per event;
 *           each level's music loops forever on voice 0 (which ASND hands out last)
 *
 * Everything game-related lives in the platform-independent C files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <asndlib.h>

#include "game.h"
#include "music.h"
#include "gfx.h"

#define FIFO_SIZE (512 * 1024)

/* extra bits the platform layer handles itself (above the game's BTN_* bits) */
#define X_HOME  (1u << 16)
#define X_MINUS (1u << 17)

static GXRModeObj *rmode;
static void       *xfb[2];
static int         fbi;
static int         wide = 1;           /* 1 = 16:9 anamorphic, 0 = letterboxed 16:9 inside a 4:3 picture */
static volatile int quit;
static Game       *game;

/* ------------------------------------------------------------------ */
/* video                                                               */
/* ------------------------------------------------------------------ */
static void set_view(void)
{
    float w = (float)rmode->fbWidth, h = (float)rmode->efbHeight;
    if (wide) {
        GX_SetViewport(0, 0, w, h, 0, 1);
        GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    } else {
        float bh = h * 0.75f, top = (h - bh) * 0.5f;           /* a 16:9 band inside the 4:3 frame */
        GX_SetViewport(0, top, w, bh, 0, 1);
        GX_SetScissor(0, (u32)top, rmode->fbWidth, (u32)bh);
    }
}

static void video_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);          /* 480p if progressive scan is on, else 480i */
    if ((rmode->viTVMode >> 2) == VI_PAL)           /* never run at 50 Hz: PAL60 keeps the game at 60 fps */
        rmode = &TVEurgb60Hz480IntDf;
    rmode->viWidth   = 678;                         /* fill a 16:9 screen edge to edge */
    rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - rmode->viWidth) / 2;

    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    void *fifo = memalign(32, FIFO_SIZE);
    memset(fifo, 0, FIFO_SIZE);
    GX_Init(fifo, FIFO_SIZE);

    GXColor black = {0, 0, 0, 255};
    GX_SetCopyClear(black, 0x00FFFFFF);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbh = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbh);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDither(GX_ENABLE);
    set_view();

    /* vertex format: 2D position (z = 0) + RGBA colour, sent directly */
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    /* no lighting, no textures: the vertex colour goes straight through */
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_InvVtxCache();
    GX_InvalidateTexAll();

    /* the virtual 854x480 screen */
    Mtx44 proj;
    guOrtho(proj, 0, VH, 0, VW, 0, 300);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx view;
    guMtxIdentity(view);
    guMtxTransApply(view, view, 0.0f, 0.0f, -50.0f);
    GX_LoadPosMtxImm(view, GX_PNMTX0);
}

/* send the frame's triangle list to the GPU */
static void draw_list(void)
{
    for (int s = 0; s < gfx_nseg; s++) {
        const GSeg *seg = &gfx_seg[s];
        if (seg->count < 3) continue;
        if (seg->mode == BL_ADD) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
        else                     GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        int i = seg->start, end = seg->start + seg->count;
        while (i + 2 < end) {
            int n = end - i;
            if (n > 65532) n = 65532;                  /* GX_Begin takes a 16-bit count */
            n -= n % 3;
            GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)n);
            for (int k = 0; k < n; k++) {
                const GVtx *v = &gfx_v[i + k];
                GX_Position3f32(v->x, v->y, 0.0f);
                GX_Color1u32(v->c);
            }
            GX_End();
            i += n;
        }
    }
}

static void present(void)
{
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb[fbi], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(xfb[fbi]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fbi ^= 1;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */
static float stick_axis(int pos, int center, int lo, int hi)
{
    float d = (float)(pos - center);
    float range = d >= 0 ? (float)(hi - center) : (float)(center - lo);
    if (range < 4.0f) return 0.0f;
    float v = d / range;
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

static u32 read_input(Input *in)
{
    WPAD_ScanPads();
    u32 h = WPAD_ButtonsHeld(0), b = 0;
    if (h & WPAD_CLASSIC_BUTTON_UP)    b |= BTN_UP;
    if (h & WPAD_CLASSIC_BUTTON_DOWN)  b |= BTN_DOWN;
    if (h & WPAD_CLASSIC_BUTTON_LEFT)  b |= BTN_LEFT;
    if (h & WPAD_CLASSIC_BUTTON_RIGHT) b |= BTN_RIGHT;
    if (h & WPAD_CLASSIC_BUTTON_A)     b |= BTN_A;
    if (h & WPAD_CLASSIC_BUTTON_B)     b |= BTN_B;
    if (h & WPAD_CLASSIC_BUTTON_X)     b |= BTN_X;
    if (h & WPAD_CLASSIC_BUTTON_Y)     b |= BTN_Y;
    if (h & WPAD_CLASSIC_BUTTON_FULL_L) b |= BTN_L;      /* L1 / R1: choose tower */
    if (h & WPAD_CLASSIC_BUTTON_FULL_R) b |= BTN_R;
    if (h & WPAD_CLASSIC_BUTTON_ZL)     b |= BTN_ZL;     /* L2 / R2: zoom */
    if (h & WPAD_CLASSIC_BUTTON_ZR)     b |= BTN_ZR;
    if (h & WPAD_CLASSIC_BUTTON_PLUS)  b |= BTN_PLUS;
    if (h & WPAD_CLASSIC_BUTTON_MINUS) b |= X_MINUS;
    if ((h & WPAD_CLASSIC_BUTTON_HOME) || (h & WPAD_BUTTON_HOME)) b |= X_HOME;

    in->sx = in->sy = 0.0f;
    WPADData *wd = WPAD_Data(0);
    if (wd && wd->exp.type == WPAD_EXP_CLASSIC) {
        const joystick_t *js = &wd->exp.classic.ljs;
        float x = stick_axis(js->pos.x, js->center.x, js->min.x, js->max.x);
        float y = -stick_axis(js->pos.y, js->center.y, js->min.y, js->max.y);   /* stick up = screen up */
        if (x == 0.0f && y == 0.0f && js->mag > 0.0f && js->mag <= 1.5f) {        /* fall back to angle/magnitude */
            float a = js->ang * 3.14159265f / 180.0f;
            x = sinf(a) * js->mag; y = -cosf(a) * js->mag;
        }
        float m = sqrtf(x * x + y * y);
        if (m < 0.2f || !isfinite(m)) { x = y = 0.0f; }                          /* dead zone */
        else { float k = (m > 1.0f ? 1.0f : m); k = (k - 0.2f) / 0.8f / m; x *= k; y *= k; }
        in->sx = x; in->sy = y;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */
#define MUSIC_VOICE 0

static void play_sfx(int id, int pitch, float pan)
{
    s32 v = ASND_GetFirstUnusedVoice();
    if (v < 0 || v == MUSIC_VOICE) return;      /* all voices busy (voice 0 is the music) */
    int vol = audio_volume(id);
    int vl = (int)((float)vol * (pan > 0 ? 1.0f - pan * 0.7f : 1.0f));
    int vr = (int)((float)vol * (pan < 0 ? 1.0f + pan * 0.7f : 1.0f));
    s32 rate = SFX_RATE * pitch / 128;
    if (rate > MAX_PITCH) rate = MAX_PITCH;
    ASND_SetVoice(v, VOICE_MONO_16BIT, rate, 0, (void *)audio_data(id), audio_len(id) * 2, vl, vr, NULL);
}

static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; quit = 1; }
static void on_power(void) { quit = 1; }

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    video_init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC);   /* make sure the extension (sticks) is reported */
    ASND_Init();
    ASND_Pause(0);
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);

    game = (Game *)memalign(32, GAME_SIZE);
    game_init(game);                           /* also synthesizes the sound effects */
    for (int id = 0; id < SFX_COUNT; id++)     /* ASND reads samples by DMA: flush the cache */
        DCFlushRange((void *)audio_data(id), (u32)audio_len(id) * 2);
    for (int t = 0; t < MUS_TRACKS; t++)
        DCFlushRange((void *)music_data(t), (u32)music_len(t) * 2);
    int playing = -1;

    const float dt = 1.0f / 60.0f;              /* the game simulates in fixed 1/60 s steps */
    u32 prev = 0;
    u64 last = gettime();

    while (!quit) {
        Input in;
        u32 held = read_input(&in);
        u32 pressed = held & ~prev;
        prev = held;

        if (held & X_HOME) break;                                   /* back to the Homebrew Channel / menu */
        if (pressed & X_MINUS) { wide = !wide; set_view(); }        /* 16:9 <-> letterboxed for a 4:3 TV */

        /* VIDEO_WaitVSync() locks us to 60 fps. If a frame ever runs long, run extra
         * simulation steps so the game keeps its speed instead of slowing down. */
        u64 now = gettime();
        float elapsed = (float)diff_usec(last, now) * 1e-6f;
        last = now;
        int steps = (int)(elapsed / dt + 0.5f);
        if (steps < 1) steps = 1;
        if (steps > 3) steps = 3;
        steps *= game_speed(game);              /* fast forward runs two steps per frame */
        for (int s = 0; s < steps; s++) {
            in.held = held & 0xFFFFu;
            in.pressed = s == 0 ? (pressed & 0xFFFFu) : 0;
            game_update(game, &in, dt);
        }

        int id, pitch;
        float pan;
        while ((id = game_pop_sound(game, &pitch, &pan)) >= 0) play_sfx(id, pitch, pan);

        int want = game_music(game);           /* switch the music loop when the level (or the setting) changes */
        if (want != playing) {
            ASND_StopVoice(MUSIC_VOICE);
            if (want >= 0)
                ASND_SetInfiniteVoice(MUSIC_VOICE, VOICE_MONO_16BIT, MUS_RATE, 0, (void *)music_data(want),
                                      music_len(want) * 2, music_volume(want), music_volume(want));
            playing = want;
        }

        game_render(game);
        draw_list();
        present();
    }

    ASND_End();
    return 0;
}
