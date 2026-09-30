/* gm_host.c - SDL2 host: window, drawing, audio, input, main loop.
 *
 *   game.exe [--game DIR] [--headless] [--record out.mp4] [--frames N]
 *            [--keys F:VK[:HOLD],...] [--shot F:out.png] [--seed N] [--room N]
 *            [--load save.gms] [--ui] [--click F:X,Y]
 *
 * Headless renders offscreen with the software renderer and never opens a
 * visible window, so it works over RDP (repo rules section 13). --keys taps a
 * GameMaker virtual key at frame F (held HOLD frames, default 2). --ui also
 * renders the dev menu offscreen, so --shot/--record capture the whole
 * window, and --click clicks window coordinates X,Y at frame F.
 */
#include "gmrt.h"
#include "gmhost.h"
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

bool gm_key_down[256], gm_key_pressed[256], gm_key_released[256];
int gm_draw_color = 0, gm_draw_font = -1;
double gm_draw_alpha = 1;
HostCfg host = { .speed = 1, .scale = 2, .sfx_volume = 1, .music_volume = 1, .smooth = 0 };
extern char gm_pending_load[1024];
void gm_seed(uint64_t s);

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *target;
static SDL_Texture **pages;
static uint8_t *datawin;
static char game_dir[1024];
static bool headless, ui;
static SDL_Texture *screen;     /* headless --ui: the whole window, offscreen */
static int cap_w, cap_h;        /* size of what --shot / --record capture */

const uint8_t *host_data(int off) { return datawin + off; }

/* ---------------- drawing ---------------- */

static SDL_Texture *page(int t) {
    if (t < 0 || t >= gm_ntextures) return NULL;
    if (!pages[t]) {
        SDL_RWops *rw = SDL_RWFromConstMem(datawin + gm_textures[t].off, gm_textures[t].len);
        pages[t] = IMG_LoadTexture_RW(ren, rw, 1);
        if (!pages[t]) { gm_log("texture %d: %s\n", t, IMG_GetError()); return NULL; }
        SDL_SetTextureBlendMode(pages[t], SDL_BLENDMODE_BLEND);
    }
    return pages[t];
}

static void tint(SDL_Texture *tex, int bgr, double alpha) {
    SDL_SetTextureColorMod(tex, bgr & 255, (bgr >> 8) & 255, (bgr >> 16) & 255);
    double a = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    SDL_SetTextureAlphaMod(tex, (Uint8)(a * 255 + 0.5));
}

static void draw_tpag(int tp, double x, double y, double ox, double oy, double xs, double ys,
                      double ang, int blend, double alpha) {
    if (tp < 0 || tp >= gm_ntpag) return;
    const GMTpag *t = &gm_tpag[tp];
    SDL_Texture *tex = page(t->tex);
    if (!tex) return;
    SDL_Rect src = { t->sx, t->sy, t->sw, t->sh };
    double ax = fabs(xs), ay = fabs(ys), lx = t->tx - ox, ly = t->ty - oy;
    SDL_FRect d;
    d.w = (float)(t->tw * ax); d.h = (float)(t->th * ay);
    d.x = (float)(x + (xs < 0 ? -(lx + t->tw) : lx) * ax);
    d.y = (float)(y + (ys < 0 ? -(ly + t->th) : ly) * ay);
    SDL_FPoint c = { (float)(x - d.x), (float)(y - d.y) };
    int flip = (xs < 0 ? SDL_FLIP_HORIZONTAL : 0) | (ys < 0 ? SDL_FLIP_VERTICAL : 0);
    tint(tex, blend, alpha);
    SDL_RenderCopyExF(ren, tex, &src, &d, -ang, &c, (SDL_RendererFlip)flip);
}

void host_draw_sprite(int spr, double sub, double x, double y, double xs, double ys, double ang, int blend, double alpha) {
    if (spr < 0 || spr >= gm_nsprites) return;
    const GMSprite *s = &gm_sprites[spr];
    if (!s->nframes) return;
    int f = ((int)floor(sub) % s->nframes + s->nframes) % s->nframes;
    draw_tpag(gm_sprite_frames[s->frame0 + f], x, y, s->ox, s->oy, xs, ys, ang, blend, alpha);
}

void host_draw_background(int bg, double x, double y, bool tilex, bool tiley, bool stretch) {
    if (bg < 0 || bg >= gm_nbackgrounds || gm_room < 0) return;
    int tp = gm_backgrounds[bg].tpag;
    if (tp < 0) return;
    const GMTpag *t = &gm_tpag[tp];
    const GMRoom *rm = &gm_rooms[gm_room];
    if (stretch) { draw_tpag(tp, 0, 0, 0, 0, (double)rm->w / t->bw, (double)rm->h / t->bh, 0, 0xFFFFFF, 1); return; }
    double x0 = x, y0 = y;
    if (tilex) while (x0 > 0) x0 -= t->bw;
    if (tiley) while (y0 > 0) y0 -= t->bh;
    for (double yy = y0; yy < (tiley ? rm->h : y0 + 1); yy += t->bh)
        for (double xx = x0; xx < (tilex ? rm->w : x0 + 1); xx += t->bw)
            draw_tpag(tp, xx, yy, 0, 0, 1, 1, 0, 0xFFFFFF, 1);
}

void host_clear(int c) {
    SDL_SetRenderDrawColor(ren, c & 255, (c >> 8) & 255, (c >> 16) & 255, 255);
    SDL_RenderClear(ren);
}

static const GMGlyph *glyph(const GMFont *f, int ch) {
    for (int k = 0; k < f->nglyphs; k++) if (gm_glyphs[f->glyph0 + k].ch == ch) return &gm_glyphs[f->glyph0 + k];
    return NULL;
}

/* GMS 1.x text: '#' is a newline, "\#" a literal '#'. */
void host_draw_text(double x, double y, const char *s) {
    if (!gm_nfonts) return;
    const GMFont *f = &gm_fonts[gm_draw_font >= 0 && gm_draw_font < gm_nfonts ? gm_draw_font : 0];
    if (f->tpag < 0) return;
    const GMTpag *t = &gm_tpag[f->tpag];
    SDL_Texture *tex = page(t->tex);
    if (!tex) return;
    int lh = 0;
    for (int k = 0; k < f->nglyphs; k++) if (gm_glyphs[f->glyph0 + k].h > lh) lh = gm_glyphs[f->glyph0 + k].h;
    tint(tex, gm_draw_color, gm_draw_alpha);
    double cx = x, cy = y;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int ch = *p;
        if (ch == '\\' && p[1] == '#') { ch = '#'; p++; }
        else if (ch == '#' || ch == '\n') { cx = x; cy += lh; continue; }
        else if (ch == '\r') continue;
        const GMGlyph *g = glyph(f, ch);
        if (!g) continue;
        SDL_Rect src = { t->sx + g->x, t->sy + g->y, g->w, g->h };
        SDL_FRect d = { (float)(cx + g->offset), (float)cy, (float)g->w, (float)g->h };
        SDL_RenderCopyF(ren, tex, &src, &d);
        cx += g->shift;
    }
}

/* ---------------- audio ---------------- */

typedef struct { Mix_Chunk *c; bool tried; double gain, gain_to, gain_step; int fade_left; } Snd;
static Snd *snds;
#define NCH 64
static int ch_snd[NCH], ch_handle[NCH], next_handle = 200000;

static bool is_music(int s) { return gm_sounds[s].audio < 0; }

static Mix_Chunk *snd_chunk(int s) {
    Snd *d = &snds[s];
    if (d->c || d->tried) return d->c;
    d->tried = true;
    if (gm_sounds[s].audio >= 0 && gm_sounds[s].audio < gm_naudio) {
        const GMBlob *b = &gm_audio[gm_sounds[s].audio];
        d->c = Mix_LoadWAV_RW(SDL_RWFromConstMem(datawin + b->off, b->len), 1);
    } else {
        /* ponytail: streamed music is decoded whole; drop idle ones so only a
         * few tracks sit in memory. Switch to Mix_Music if a game layers music. */
        for (int k = 0; k < gm_nsounds; k++) {
            if (k == s || !snds[k].c || !is_music(k) || host_sound_playing(k)) continue;
            Mix_FreeChunk(snds[k].c); snds[k].c = NULL; snds[k].tried = false;
        }
        char p[1200]; snprintf(p, sizeof p, "%s/%s", game_dir, gm_sounds[s].file);
        d->c = Mix_LoadWAV(p);
    }
    if (!d->c) gm_log("sound %s: %s\n", gm_sounds[s].name, Mix_GetError());
    return d->c;
}

static void ch_volume(int ch) {
    int s = ch_snd[ch];
    if (s < 0) return;
    double v = gm_sounds[s].volume * snds[s].gain * (is_music(s) ? host.music_volume : host.sfx_volume);
    if (host.mute) v = 0;
    Mix_Volume(ch, (int)(fmin(fmax(v, 0), 1) * MIX_MAX_VOLUME));
}

int host_sound_play(int s, bool loop) {
    if (s < 0 || s >= gm_nsounds) return -1;
    Mix_Chunk *c = snd_chunk(s);
    if (!c) return -1;
    int ch = Mix_PlayChannel(-1, c, loop ? -1 : 0);
    if (ch < 0 || ch >= NCH) return -1;
    ch_snd[ch] = s; ch_handle[ch] = next_handle++;
    ch_volume(ch);
    return ch_handle[ch];
}

static bool ch_match(int ch, int x) { return ch_snd[ch] >= 0 && (ch_snd[ch] == x || ch_handle[ch] == x); }

void host_sound_stop(int x) { for (int c = 0; c < NCH; c++) if (ch_match(c, x) && Mix_Playing(c)) { Mix_HaltChannel(c); ch_snd[c] = -1; } }
void host_sound_stop_all(void) { Mix_HaltChannel(-1); for (int c = 0; c < NCH; c++) ch_snd[c] = -1; }
bool host_sound_playing(int x) { for (int c = 0; c < NCH; c++) if (ch_match(c, x) && Mix_Playing(c)) return true; return false; }

void host_sound_gain(int x, double vol, double ms) {
    int s = x;
    if (x >= 200000) { s = -1; for (int c = 0; c < NCH; c++) if (ch_handle[c] == x) s = ch_snd[c]; }
    if (s < 0 || s >= gm_nsounds) return;
    int frames = (int)(ms / 1000.0 * gm_room_speed);
    if (frames <= 0) { snds[s].gain = vol; snds[s].fade_left = 0; }
    else { snds[s].gain_to = vol; snds[s].gain_step = (vol - snds[s].gain) / frames; snds[s].fade_left = frames; }
}

static void audio_step(void) {
    for (int s = 0; s < gm_nsounds; s++) {
        Snd *d = &snds[s];
        if (d->fade_left > 0 && --d->fade_left == 0) d->gain = d->gain_to;
        else if (d->fade_left > 0) d->gain += d->gain_step;
    }
    for (int c = 0; c < NCH; c++) ch_volume(c);
}

/* ---------------- window ---------------- */

static bool fullscreen;
void host_set_fullscreen(bool on) {
    fullscreen = on;
    if (win && !headless) SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
bool host_get_fullscreen(void) { return fullscreen; }

const char *gm_save_dir(void) {
    static char d[1024];
    if (!d[0]) {
        const char *base = getenv("LOCALAPPDATA");
        snprintf(d, sizeof d, "%s/gmrecomp", base ? base : ".");
        _mkdir(d);
        snprintf(d, sizeof d, "%s/gmrecomp/%s", base ? base : ".", gm_game_name);
        _mkdir(d);
    }
    return d;
}

/* ---------------- input ----------------
 * Every SDL key maps to the GameMaker virtual key of the same meaning; the
 * dev menu's bindings add extra sources (other keys, gamepad buttons). */

int host_vk_from_sdl(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= '0' && k <= '9') return k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 112 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 97 + (k - SDLK_KP_1);
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13; case SDLK_ESCAPE: return 27; case SDLK_SPACE: return 32;
    case SDLK_LEFT: return 37; case SDLK_UP: return 38; case SDLK_RIGHT: return 39; case SDLK_DOWN: return 40;
    case SDLK_BACKSPACE: return 8; case SDLK_TAB: return 9; case SDLK_LSHIFT: case SDLK_RSHIFT: return 16;
    case SDLK_LCTRL: case SDLK_RCTRL: return 17; case SDLK_LALT: case SDLK_RALT: return 18;
    case SDLK_DELETE: return 46; case SDLK_INSERT: return 45; case SDLK_HOME: return 36; case SDLK_END: return 35;
    case SDLK_PAGEUP: return 33; case SDLK_PAGEDOWN: return 34; case SDLK_KP_0: return 96;
    }
    return 0;
}

/* sources are counted so two inputs bound to one key don't release each other */
static int src_count[256];
void host_vk(int vk, bool down) {
    if (vk <= 0 || vk > 255) return;
    if (down) { if (src_count[vk]++ == 0) { gm_key_down[vk] = true; gm_key_pressed[vk] = true; } }
    else if (src_count[vk] > 0 && --src_count[vk] == 0) { gm_key_down[vk] = false; gm_key_released[vk] = true; }
}

static SDL_GameController *pad;
static bool axis_state[4];
static void pad_axis(int axis, int value) {
    /* left stick acts as the d-pad */
    const int dead = 16000;
    if (axis == SDL_CONTROLLER_AXIS_LEFTX) {
        bool l = value < -dead, r = value > dead;
        if (l != axis_state[0]) { axis_state[0] = l; host_vk(host.pad_map[SDL_CONTROLLER_BUTTON_DPAD_LEFT], l); }
        if (r != axis_state[1]) { axis_state[1] = r; host_vk(host.pad_map[SDL_CONTROLLER_BUTTON_DPAD_RIGHT], r); }
    } else if (axis == SDL_CONTROLLER_AXIS_LEFTY) {
        bool u = value < -dead, d = value > dead;
        if (u != axis_state[2]) { axis_state[2] = u; host_vk(host.pad_map[SDL_CONTROLLER_BUTTON_DPAD_UP], u); }
        if (d != axis_state[3]) { axis_state[3] = d; host_vk(host.pad_map[SDL_CONTROLLER_BUTTON_DPAD_DOWN], d); }
    }
}

void host_default_bindings(void) {
    static const int pad_default[SDL_CONTROLLER_BUTTON_MAX] = {
        [SDL_CONTROLLER_BUTTON_A] = 13, [SDL_CONTROLLER_BUTTON_B] = 27, [SDL_CONTROLLER_BUTTON_X] = 32,
        [SDL_CONTROLLER_BUTTON_Y] = 69, [SDL_CONTROLLER_BUTTON_START] = 13, [SDL_CONTROLLER_BUTTON_BACK] = 27,
        [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 81, [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 69,
        [SDL_CONTROLLER_BUTTON_DPAD_UP] = 38, [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = 40,
        [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = 37, [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = 39,
    };
    memcpy(host.pad_map, pad_default, sizeof pad_default);
    for (int k = 0; k < 256; k++) host.key_map[k] = 0;
}

/* ---------------- mouse ----------------
 * GMS 1.4 games often have no mouse code at all: their buttons are objects
 * that listen for a key. A left click taps the key of the topmost visible
 * instance under the cursor that has KeyPress/KeyRelease events; a button
 * that takes Left and Right (a "< option >" selector) gets Left or Right from
 * its outer thirds. A click nothing claims goes to the profile (dev_click),
 * then becomes Enter (left) or Esc (right). docs/runtime.md, "Mouse". */

/* timed key presses: clicks, and profile macros (host_key_seq) */
typedef struct { int vk, at, hold; } Seq;
static Seq seqs[64]; static int nseqs;
void host_key_seq(int vk, int delay, int hold) {
    if (vk <= 0 || vk > 255 || nseqs == 64) return;
    seqs[nseqs++] = (Seq){ vk, delay, hold < 1 ? 1 : hold };
    if (delay == 0) { host_vk(vk, true); seqs[nseqs - 1].at = -1; }
}
static void tap(int vk) { host_key_seq(vk, 0, 3); }
static void seq_step(void) {
    for (int k = 0; k < nseqs; k++) {
        Seq *q = &seqs[k];
        if (q->at > 0 && --q->at == 0) { host_vk(q->vk, true); q->at = -1; continue; }
        if (q->at < 0 && --q->hold == 0) { host_vk(q->vk, false); seqs[k--] = seqs[--nseqs]; }
    }
}

static int obj_keys(int obj, bool *keys) {
    int n = 0;
    for (int o = obj; o >= 0; o = gm_objects[o].parent)
        for (int e = 0; e < gm_objects[o].nev; e++) {
            const GMEvent *ev = &gm_events[gm_objects[o].ev0 + e];
            if ((ev->type == EV_KEYPRESS || ev->type == EV_KEYRELEASE) && ev->sub > 1 && ev->sub < 256 && !keys[ev->sub]) {
                keys[ev->sub] = true; n++;
            }
        }
    return n;
}

int host_click_key(double x, double y) {
    Inst *best = NULL;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        /* the box, not the precise mask: a text button's mask is only its letters */
        if (i->dead || !i->active || !i->visible || (i->sprite < 0 && i->mask < 0)) continue;
        bool keys[256] = { 0 };
        if (!obj_keys(i->obj, keys)) continue;
        double l, t, r, b; gm_bbox(i, &l, &t, &r, &b);
        if (x < l || x >= r || y < t || y >= b) continue;
        if (!best || i->depth < best->depth || (i->depth == best->depth && i->id > best->id)) best = i;
    }
    if (!best) return 0;
    bool keys[256] = { 0 };
    int n = obj_keys(best->obj, keys);
    double l, t, r, b; gm_bbox(best, &l, &t, &r, &b);
    if (keys[37] && keys[39] && r > l) { double f = (x - l) / (r - l); if (f < 1.0 / 3) return 37; if (f > 2.0 / 3) return 39; }
    if (keys[38] && keys[40] && b > t) { double f = (y - t) / (b - t); if (f < 1.0 / 3) return 38; if (f > 2.0 / 3) return 40; }
    if (keys[13]) return 13;
    if (keys[32]) return 32;
    for (int k = 0; k < 256; k++) if (keys[k] && n) return k;
    return 0;
}

void host_click(double x, double y, int button) {
    int k = button == SDL_BUTTON_LEFT ? host_click_key(x, y) : 0;
    if (dev_click(x, y, button, k)) return;
    if (k) { tap(k); return; }
    tap(button == SDL_BUTTON_LEFT ? 13 : button == SDL_BUTTON_RIGHT ? 27 : 0);
}

static SDL_Rect game_dst;               /* where the game frame sits in the window */
static bool to_room(int wx, int wy, double *x, double *y) {
    int ww, wh, ow = 0, oh = 0; SDL_GetWindowSize(win, &ww, &wh);
    if (!screen) SDL_GetRendererOutputSize(ren, &ow, &oh);
    double sx = ow && ww ? (double)ow / ww : 1, sy = oh && wh ? (double)oh / wh : 1;
    if (!game_dst.w || !game_dst.h) return false;
    *x = (wx * sx - game_dst.x) * gm_window_w / game_dst.w;
    *y = (wy * sy - game_dst.y) * gm_window_h / game_dst.h;
    return *x >= 0 && *y >= 0 && *x < gm_window_w && *y < gm_window_h;
}

/* ---------------- config ---------------- */

static const char *cfg_path(void) {
    static char p[1100]; snprintf(p, sizeof p, "%s/host.cfg", gm_save_dir()); return p;
}
void host_save_cfg(void) {
    FILE *f = fopen(cfg_path(), "w");
    if (!f) return;
    fprintf(f, "scale %d\nsmooth %d\nsfx %g\nmusic %g\nmute %d\n", host.scale, host.smooth, host.sfx_volume, host.music_volume, host.mute);
    for (int k = 0; k < SDL_CONTROLLER_BUTTON_MAX; k++) fprintf(f, "pad %d %d\n", k, host.pad_map[k]);
    for (int k = 0; k < 256; k++) if (host.key_map[k]) fprintf(f, "key %d %d\n", k, host.key_map[k]);
    fclose(f);
}
static void load_cfg(void) {
    FILE *f = fopen(cfg_path(), "r");
    if (!f) return;
    char w[32]; double v; int a, b;
    while (fscanf(f, "%31s", w) == 1) {
        if (!strcmp(w, "pad") || !strcmp(w, "key")) {
            if (fscanf(f, "%d %d", &a, &b) != 2) break;
            if (w[0] == 'p' && a >= 0 && a < SDL_CONTROLLER_BUTTON_MAX) host.pad_map[a] = b;
            if (w[0] == 'k' && a >= 0 && a < 256) host.key_map[a] = b;
            continue;
        }
        if (fscanf(f, "%lf", &v) != 1) break;
        if (!strcmp(w, "scale")) host.scale = (int)v; else if (!strcmp(w, "smooth")) host.smooth = (int)v;
        else if (!strcmp(w, "sfx")) host.sfx_volume = v; else if (!strcmp(w, "music")) host.music_volume = v;
        else if (!strcmp(w, "mute")) host.mute = (int)v;
    }
    fclose(f);
}

/* ---------------- main ---------------- */

typedef struct { int frame, vk, hold; } KeyEv;

static void load_datawin(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", game_dir, gm_datawin);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s - pass --game <folder with %s>\n", p, gm_datawin); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    datawin = malloc(n);
    if (fread(datawin, 1, n, f) != (size_t)n) { fprintf(stderr, "short read on %s\n", p); exit(1); }
    fclose(f);
}

static void save_png(const char *path) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, cap_w, cap_h, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_SetRenderTarget(ren, screen ? screen : target);
    SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch);
    IMG_SavePNG(s, path);
    SDL_FreeSurface(s);
}

int main(int argc, char **argv) {
    const char *record = NULL, *keys = NULL, *load = NULL;
    int frames = 0, start_room = -1;
    char shots[16][260]; int shot_frame[16], nshots = 0;
    int clicks[32][3], nclicks = 0;
    snprintf(game_dir, sizeof game_dir, "game");
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "--game") && k + 1 < argc) snprintf(game_dir, sizeof game_dir, "%s", argv[++k]);
        else if (!strcmp(argv[k], "--headless")) headless = true;
        else if (!strcmp(argv[k], "--record") && k + 1 < argc) record = argv[++k];
        else if (!strcmp(argv[k], "--frames") && k + 1 < argc) frames = atoi(argv[++k]);
        else if (!strcmp(argv[k], "--keys") && k + 1 < argc) keys = argv[++k];
        else if (!strcmp(argv[k], "--seed") && k + 1 < argc) gm_seed(strtoull(argv[++k], NULL, 10));
        else if (!strcmp(argv[k], "--room") && k + 1 < argc) start_room = atoi(argv[++k]);
        else if (!strcmp(argv[k], "--load") && k + 1 < argc) load = argv[++k];
        else if (!strcmp(argv[k], "--ui")) ui = true;
        else if (!strcmp(argv[k], "--click") && k + 1 < argc && nclicks < 32) {
            if (sscanf(argv[++k], "%d:%d,%d", &clicks[nclicks][0], &clicks[nclicks][1], &clicks[nclicks][2]) == 3) nclicks++;
        }
        else if (!strcmp(argv[k], "--shot") && k + 1 < argc && nshots < 16) {
            const char *a = argv[++k], *c = strchr(a, ':');
            if (c) { shot_frame[nshots] = atoi(a); snprintf(shots[nshots++], 260, "%s", c + 1); }
        } else { fprintf(stderr, "unknown argument %s\n", argv[k]); return 2; }
    }
    if (headless && !frames) frames = 600;
    KeyEv kev[512]; int nkev = 0;
    for (const char *p = keys; p && *p && nkev < 512;) {
        KeyEv e = { 0, 0, 2 };
        int got = sscanf(p, "%d:%d:%d", &e.frame, &e.vk, &e.hold);
        if (got >= 2) kev[nkev++] = e;
        p = strchr(p, ','); if (p) p++;
    }

    load_datawin();
    if (headless) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    IMG_Init(IMG_INIT_PNG);
    if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) != 0) gm_log("audio: %s\n", Mix_GetError());
    Mix_AllocateChannels(NCH);
    for (int c = 0; c < NCH; c++) ch_snd[c] = -1;
    snds = calloc(gm_nsounds + 1, sizeof *snds);
    for (int s = 0; s < gm_nsounds; s++) snds[s].gain = 1;
    host_default_bindings();
    load_cfg();

    bool menu = !headless || ui;
    int menu_h = menu ? 22 : 0;
    win = SDL_CreateWindow(gm_game_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           gm_window_w * host.scale, gm_window_h * host.scale + menu_h,
                           headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, headless ? SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE
                                               : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, gm_window_w, gm_window_h);
    pages = calloc(gm_ntextures + 1, sizeof *pages);
    game_dst = (SDL_Rect){ 0, menu_h, gm_window_w * host.scale, gm_window_h * host.scale };
    if (menu) dev_init(win, ren);
    cap_w = gm_window_w; cap_h = gm_window_h;
    if (headless && ui) {
        cap_w = gm_window_w * host.scale; cap_h = gm_window_h * host.scale + menu_h;
        screen = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, cap_w, cap_h);
    }

    FILE *ff = NULL;
    uint8_t *px = NULL;
    if (record) {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
                 "-c:v libx264 -pix_fmt yuv420p \"%s\"", cap_w, cap_h, gm_rooms[gm_room_order[0]].speed, record);
        ff = _popen(cmd, "wb");
        px = malloc((size_t)cap_w * cap_h * 4);
    }

    gm_init();
    if (start_room >= 0) gm_room_pending = start_room;
    if (load && !gm_load_state(load)) gm_log("could not load %s\n", load);

    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    double acc = 0;
    int frame = 0;
    while (!gm_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) gm_quit = true;
            if (menu && dev_event(&ev)) continue;
            switch (ev.type) {
            case SDL_KEYDOWN: case SDL_KEYUP: {
                if (ev.key.repeat) break;
                int vk = host_vk_from_sdl(ev.key.keysym.sym);
                if (vk && host.key_map[vk]) vk = host.key_map[vk];
                if (!dev_key(vk, ev.type == SDL_KEYDOWN)) host_vk(vk, ev.type == SDL_KEYDOWN);
                break;
            }
            case SDL_CONTROLLERDEVICEADDED: if (!pad) pad = SDL_GameControllerOpen(ev.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) { SDL_GameControllerClose(pad); pad = NULL; }
                break;
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                if (ev.cbutton.button < SDL_CONTROLLER_BUTTON_MAX) {
                    int vk = host.pad_map[ev.cbutton.button]; bool dn = ev.type == SDL_CONTROLLERBUTTONDOWN;
                    if (!dev_key(vk, dn)) host_vk(vk, dn);
                }
                break;
            case SDL_CONTROLLERAXISMOTION: pad_axis(ev.caxis.axis, ev.caxis.value); break;
            case SDL_MOUSEMOTION: { double x, y; if (to_room(ev.motion.x, ev.motion.y, &x, &y)) { gm_mouse_x = x; gm_mouse_y = y; } break; }
            case SDL_MOUSEBUTTONDOWN: {
                double x, y;
                if (to_room(ev.button.x, ev.button.y, &x, &y)) { gm_mouse_x = x; gm_mouse_y = y; host_click(x, y, ev.button.button); }
                break;
            }
            }
        }

        int steps;
        if (headless) steps = 1;
        else {
            Uint64 now = SDL_GetPerformanceCounter();
            acc += (double)(now - last) / freq; last = now;
            double dt = 1.0 / (gm_room_speed > 0 ? gm_room_speed : 30);
            steps = 0;
            while (acc >= dt / host.speed && steps < 8) { acc -= dt / host.speed; steps++; }
            if (steps == 8) acc = 0;
            if (host.paused) { steps = host.step_once ? 1 : 0; host.step_once = false; }
        }
        for (int s = 0; s < steps && !gm_quit; s++) {
            for (int k = 0; k < nkev; k++) {
                if (kev[k].frame == frame) host_vk(kev[k].vk, true);
                if (kev[k].frame + kev[k].hold == frame) host_vk(kev[k].vk, false);
            }
            for (int k = 0; k < nclicks; k++) {
                int d = frame - clicks[k][0];
                if (d < 0 || d > 3 || d == 1) continue;
                SDL_Event e; memset(&e, 0, sizeof e);
                if (d == 0) { e.type = SDL_MOUSEMOTION; e.motion.windowID = SDL_GetWindowID(win); e.motion.x = clicks[k][1]; e.motion.y = clicks[k][2]; }
                else {
                    e.type = d == 2 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP; e.button.windowID = SDL_GetWindowID(win);
                    e.button.button = SDL_BUTTON_LEFT; e.button.state = d == 2 ? SDL_PRESSED : SDL_RELEASED;
                    e.button.clicks = 1; e.button.x = clicks[k][1]; e.button.y = clicks[k][2];
                }
                SDL_PushEvent(&e);
            }
            seq_step();
            dev_before_step();
            gm_frame();
            dev_after_step();
            if (gm_pending_load[0]) { gm_load_state(gm_pending_load); gm_pending_load[0] = 0; }
            audio_step();
            memset(gm_key_pressed, 0, sizeof gm_key_pressed);
            memset(gm_key_released, 0, sizeof gm_key_released);
            frame++;
        }

        SDL_SetRenderTarget(ren, target);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        gm_draw();
        if (menu) dev_overlay();
        SDL_SetRenderTarget(ren, screen);
        if (menu) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            int w = cap_w, h = cap_h;
            if (!screen) SDL_GetRendererOutputSize(ren, &w, &h);
            int mh = fullscreen ? 0 : dev_menu_height();
            double sc = fmin((double)w / gm_window_w, (double)(h - mh) / gm_window_h);
            SDL_Rect d = { (int)((w - gm_window_w * sc) / 2), mh + (int)((h - mh - gm_window_h * sc) / 2),
                           (int)(gm_window_w * sc), (int)(gm_window_h * sc) };
            game_dst = d;
            SDL_SetTextureScaleMode(target, host.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            SDL_RenderCopy(ren, target, NULL, &d);
            dev_frame();
        }
        if (ff && steps) {
            SDL_SetRenderTarget(ren, screen ? screen : target);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGBA32, px, cap_w * 4);
            fwrite(px, 1, (size_t)cap_w * cap_h * 4, ff);
        }
        for (int k = 0; k < nshots; k++) if (shot_frame[k] == frame) save_png(shots[k]);
        SDL_SetRenderTarget(ren, NULL);
        if (!headless) SDL_RenderPresent(ren);
        if (frames && frame >= frames) break;
    }
    if (ff) _pclose(ff);
    if (!headless) host_save_cfg();
    if (menu) dev_shutdown();
    gm_log("exit after %d frames in room %s\n", frame, gm_room >= 0 ? gm_rooms[gm_room].name : "?");
    Mix_CloseAudio();
    SDL_Quit();
    return 0;
}
