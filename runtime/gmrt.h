/* gmrt.h - gmrecomp runtime interface.
 *
 * The recompiler (tools/gmrecomp.py) emits one C function per GML code entry
 * plus constant tables; this header is the contract between that output and
 * the hand-written runtime. See docs/runtime.md for the event order and the
 * deliberate gaps.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STACK_MAX 64
#define ENV_MAX 8
#define NALARMS 12

enum { VT_REAL, VT_STR, VT_UNDEF, VT_ARRAY };
typedef struct GMArray GMArray;
typedef struct Val { int t; union { double r; const char *s; GMArray *a; }; } Val;
struct GMArray { int n; Val *v; };

static inline Val R(double d) { Val v; v.t = VT_REAL; v.r = d; return v; }
static inline Val S(const char *s) { Val v; v.t = VT_STR; v.s = s; return v; }
static inline Val U(void) { Val v; v.t = VT_UNDEF; v.r = 0; return v; }

typedef struct Inst {
    int id, obj;
    bool dead, active, visible, solid, persistent;
    double x, y, xprev, yprev, xstart, ystart;
    double hspeed, vspeed, speed, direction, friction, gravity, gravdir;
    int sprite, mask;
    double image_index, image_speed, xscale, yscale, angle, alpha;
    int blend;
    double depth;
    double alarm[NALARMS];
    Val *vars;
} Inst;

typedef struct { Inst *saved_self, *saved_other; int *ids; int n, k; } GMEnv;

/* ---- generated tables (tables.c) ---- */
typedef void (*GMFn)(Inst *, Inst *);
typedef struct { const char *name; GMFn fn; } GMCode;
typedef struct { const char *name; int itype, id, builtin; } GMVar;
typedef struct { uint16_t sx, sy, sw, sh, tx, ty, tw, th, bw, bh, tex; } GMTpag;
typedef struct { const char *name; int w, h, ox, oy, bl, bt, br, bb, frame0, nframes, nmasks; } GMSprite;
typedef struct { const char *name; int tpag; } GMBackground;
typedef struct { const char *name; int size, first, tpag; float sx, sy; int glyph0, nglyphs; } GMFont;
typedef struct { int ch, x, y, w, h, shift, offset; } GMGlyph;
typedef struct { const char *name; int sprite, visible, solid, depth, persistent, parent, mask, ev0, nev; } GMObject;
typedef struct { int type, sub, code; } GMEvent;
typedef struct { const char *name; int w, h, speed, persistent; unsigned color; int draw_color, code, inst0, ninst, bg0, nbg; } GMRoom;
typedef struct { int x, y, obj, id, code; float sx, sy; unsigned color; float rot; } GMRoomInst;
typedef struct { int fore, bg, x, y, tilex, tiley, hs, vs, stretch; } GMRoomBg;
typedef struct { const char *name, *file; int flags; float volume; int audio; } GMSound;
typedef struct { int off, len; } GMBlob;
typedef struct { const char *code; int addr, opt0, nopt; const char *target; } GMSite;  /* target: variable assigned, "" if none */
typedef struct { double value; const char *label; } GMSiteOpt;   /* value NaN = not a constant */

extern const char *gm_game_name, *gm_datawin;
extern int gm_window_w, gm_window_h;
extern const GMCode gm_code[]; extern const int gm_ncode;
extern const GMVar gm_vars[]; extern const int gm_nvars, gm_nglobals, gm_ninstvars;
extern const char *gm_builtin_names[];
extern const GMTpag gm_tpag[]; extern const int gm_ntpag;
extern const GMSprite gm_sprites[]; extern const int gm_nsprites; extern const int gm_sprite_frames[];
extern const int gm_sprite_mask_off[];
extern const GMBackground gm_backgrounds[]; extern const int gm_nbackgrounds;
extern const GMFont gm_fonts[]; extern const int gm_nfonts; extern const GMGlyph gm_glyphs[];
extern const GMObject gm_objects[]; extern const int gm_nobjects; extern const GMEvent gm_events[];
extern const GMRoom gm_rooms[]; extern const int gm_nrooms;
extern const GMRoomInst gm_room_insts[]; extern const GMRoomBg gm_room_bgs[];
extern const int gm_room_order[]; extern const int gm_nroom_order;
extern const GMSound gm_sounds[]; extern const int gm_nsounds;
extern const GMBlob gm_textures[]; extern const int gm_ntextures;
extern const GMBlob gm_audio[]; extern const int gm_naudio;
extern const GMSite gm_choose_sites[]; extern const int gm_nchoose_sites;
extern const GMSiteOpt gm_choose_opts[];

/* ---- value ops (gm_core.c) ---- */
double gm_real(Val v);
bool gm_truthy(Val v);
int gm_cmp(Val a, Val b);
Val gm_add(Val a, Val b); Val gm_sub(Val a, Val b); Val gm_mul(Val a, Val b);
Val gm_div(Val a, Val b); Val gm_idiv(Val a, Val b); Val gm_mod(Val a, Val b);
Val gm_and(Val a, Val b); Val gm_or(Val a, Val b); Val gm_xor(Val a, Val b);
Val gm_shl(Val a, Val b); Val gm_shr(Val a, Val b);
const char *gm_intern(const char *s);
const char *gm_tostr(Val v);

/* ---- variables / instances ---- */
Val gm_get(Inst *self, Inst *other, int inst, int var);
void gm_set(Inst *self, Inst *other, int inst, int var, Val v);
Val gm_aget(Inst *self, Inst *other, Val inst, int var, Val idx);
void gm_aset(Inst *self, Inst *other, Val inst, int var, Val idx, Val v);
int gm_inst_of(Val v);
bool gm_env_push(GMEnv *e, Inst **self, Inst **other, Val target);
bool gm_env_next(GMEnv *e, Inst **self);
void gm_env_pop(GMEnv *e, Inst **self, Inst **other);
void gm_env_unwind(GMEnv *e, int ep, Inst **self, Inst **other);
Val gm_choose(int site, Val *a, int n);

extern Val *gm_globals;
extern Inst **gm_insts; extern int gm_ninsts;
extern int gm_room, gm_room_pending;
extern int gm_room_speed;
extern bool gm_quit;
Inst *gm_find(int id);
Inst *gm_create(double x, double y, int obj, Inst *creator);
void gm_destroy(Inst *i);
void gm_change(Inst *i, int obj, bool perf);
void gm_event(Inst *i, int type, int sub, Inst *other);
bool gm_has_event(int obj, int type, int sub);
bool gm_is(Inst *i, int target);            /* does instance i match object/id target */
bool gm_collide(Inst *a, double ax, double ay, Inst *b);
void gm_init(void);
extern void (*gm_premove_hook)(void);      /* after Step events, before speeds move instances */
void gm_frame(void);                        /* one game step (no drawing) */
void gm_draw(void);                         /* draw the room into the game target */
bool gm_save_state(const char *path);
bool gm_load_state(const char *path);
const char *gm_save_dir(void);

/* RNG control for the dev menu: per-site forced pick, -1 = random */
extern int gm_choose_force[];               /* sized gm_nchoose_sites */
extern int gm_choose_last[];                /* last picked index per site, -1 never */
extern int gm_choose_count[];               /* times each site ran */

/* ---- host (gm_host.c) ---- */
extern bool gm_key_down[256], gm_key_pressed[256], gm_key_released[256];
extern int gm_draw_color, gm_draw_font;
extern double gm_draw_alpha;
void host_draw_sprite(int spr, double sub, double x, double y, double xs, double ys, double ang, int blend, double alpha);
void host_draw_text(double x, double y, const char *s);
void host_draw_background(int bg, double x, double y, bool tilex, bool tiley, bool stretch);
void host_clear(int color);
int host_sound_play(int snd, bool loop);
void host_sound_stop(int snd_or_handle);
void host_sound_stop_all(void);
bool host_sound_playing(int snd_or_handle);
void host_sound_gain(int snd_or_handle, double vol, double ms);
void host_set_fullscreen(bool on);
bool host_get_fullscreen(void);
const uint8_t *host_data(int off);          /* pointer into data.win */

/* ---- builtin functions (gm_funcs.c) ---- */
#define GMF(name) Val gmf_##name(Inst *self, Inst *other, Val *a, int n)
GMF(instance_destroy); GMF(audio_play_sound); GMF(instance_change); GMF(room_goto);
GMF(audio_stop_sound); GMF(instance_create); GMF(instance_deactivate_object); GMF(action_end_sound);
GMF(action_sound); GMF(action_set_alarm); GMF(sound_loop); GMF(room_goto_next); GMF(file_exists);
GMF(sound_stop_all); GMF(keyboard_check_pressed); GMF(instance_exists); GMF(game_load); GMF(game_end);
GMF(instance_activate_object); GMF(room_goto_previous); GMF(steam_get_achievement);
GMF(steam_set_achievement); GMF(draw_set_font); GMF(action_create_object); GMF(keyboard_check_released);
GMF(draw_set_color); GMF(draw_text); GMF(draw_sprite); GMF(audio_is_playing); GMF(action_snapshot);
GMF(audio_sound_gain); GMF(sprite_exists); GMF(game_save); GMF(file_delete); GMF(window_get_fullscreen);
GMF(window_set_fullscreen); GMF(action_kill_object); GMF(instance_nearest); GMF(place_meeting);
GMF(action_change_object); GMF(action_set_vspeed); GMF(action_set_hspeed); GMF(keyboard_check);
GMF(place_free); GMF(action_sprite_set);

enum { EV_CREATE, EV_DESTROY, EV_ALARM, EV_STEP, EV_COLLISION, EV_KEYBOARD, EV_MOUSE, EV_OTHER,
       EV_DRAW, EV_KEYPRESS, EV_KEYRELEASE, EV_TRIGGER };

void gm_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
