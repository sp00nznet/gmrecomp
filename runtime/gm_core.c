/* gm_core.c - values, instances, events, rooms and savestates.
 *
 * Mirrors the GameMaker Studio 1.4 runner's observable behaviour for what
 * bytecode-15 games use. The step order is in docs/runtime.md; keep the two in
 * step when either changes.
 */
#include "gmrt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* Order must match BUILTINS in tools/gmrecomp.py. */
enum { B_X, B_Y, B_XPREV, B_YPREV, B_XSTART, B_YSTART, B_HSPEED, B_VSPEED, B_SPEED, B_DIRECTION,
       B_FRICTION, B_GRAVITY, B_GRAVDIR, B_SPRITE, B_IMAGE_INDEX, B_IMAGE_SPEED, B_XSCALE, B_YSCALE,
       B_ANGLE, B_ALPHA, B_BLEND, B_IMAGE_NUMBER, B_DEPTH, B_VISIBLE, B_SOLID, B_PERSISTENT, B_MASK,
       B_OBJECT_INDEX, B_ID, B_ALARM, B_ROOM, B_ROOM_SPEED, B_ROOM_WIDTH, B_ROOM_HEIGHT,
       B_KEYBOARD_KEY, B_KEYBOARD_LASTKEY, B_MOUSE_X, B_MOUSE_Y, B_FPS, B_CURRENT_TIME,
       B_INSTANCE_COUNT, B_BBOX_LEFT, B_BBOX_RIGHT, B_BBOX_TOP, B_BBOX_BOTTOM, B_SPRITE_WIDTH,
       B_SPRITE_HEIGHT, B_SCORE, B_LIVES, B_HEALTH, B_ROOM_FIRST, B_ROOM_LAST, B_ARG_RELATIVE };

Val *gm_globals;
Inst **gm_insts; int gm_ninsts; static int cap_insts;
int gm_room = -1, gm_room_pending = -1, gm_room_speed = 30;
bool gm_quit;
double gm_mouse_x, gm_mouse_y;
void (*gm_premove_hook)(void);
static int next_id = 100001;
static double g_score, g_lives = -1, g_health = 100;
int gm_choose_force[1024], gm_choose_last[1024], gm_choose_count[1024];
static uint64_t rng_state;

void gm_log(const char *fmt, ...) {
    static FILE *lf;
    if (!lf) lf = fopen("gmrecomp.log", "w");
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (lf) { va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap); fflush(lf); }
}

/* ---------------- values ---------------- */

/* ponytail: runtime strings are interned forever; bounded by distinct strings
 * the game builds (score readouts). Upgrade to refcounting if a game churns. */
const char *gm_intern(const char *s) {
    static const char **tab; static unsigned cap, cnt;
    if (cnt * 2 >= cap) {
        unsigned nc = cap ? cap * 2 : 1024; const char **nt = calloc(nc, sizeof *nt);
        for (unsigned i = 0; i < cap; i++) if (tab[i]) {
            unsigned h = 2166136261u; for (const char *p = tab[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            while (nt[h & (nc - 1)]) h++;
            nt[h & (nc - 1)] = tab[i];
        }
        free(tab); tab = nt; cap = nc;
    }
    unsigned h = 2166136261u; for (const char *p = s; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (;; h++) {
        const char *e = tab[h & (cap - 1)];
        if (!e) break;
        if (!strcmp(e, s)) return e;
    }
    char *d = _strdup(s); tab[h & (cap - 1)] = d; cnt++;
    return d;
}

double gm_real(Val v) { return v.t == VT_REAL ? v.r : 0; }
bool gm_truthy(Val v) { return v.t == VT_REAL && v.r > 0.5; }

const char *gm_tostr(Val v) {
    char b[64];
    if (v.t == VT_STR) return v.s;
    if (v.t != VT_REAL) return "undefined";
    if (v.r == floor(v.r) && fabs(v.r) < 1e15) snprintf(b, sizeof b, "%.0f", v.r);
    else snprintf(b, sizeof b, "%.2f", v.r);
    return gm_intern(b);
}

int gm_cmp(Val a, Val b) {
    if (a.t == VT_STR && b.t == VT_STR) { int c = strcmp(a.s, b.s); return (c > 0) - (c < 0); }
    if (a.t == VT_STR) return 1;
    if (b.t == VT_STR) return -1;
    double d = gm_real(a) - gm_real(b);
    if (fabs(d) < 1e-5) return 0;       /* GMS 1.4 math_get_epsilon default */
    return d < 0 ? -1 : 1;
}

Val gm_add(Val a, Val b) {
    if (a.t == VT_STR && b.t == VT_STR) {
        size_t la = strlen(a.s), lb = strlen(b.s);
        char *t = malloc(la + lb + 1); memcpy(t, a.s, la); memcpy(t + la, b.s, lb + 1);
        const char *r = gm_intern(t); free(t); return S(r);
    }
    return R(gm_real(a) + gm_real(b));
}
Val gm_sub(Val a, Val b) { return R(gm_real(a) - gm_real(b)); }
Val gm_mul(Val a, Val b) { return R(gm_real(a) * gm_real(b)); }
Val gm_div(Val a, Val b) { double d = gm_real(b); return R(d == 0 ? 0 : gm_real(a) / d); }
Val gm_idiv(Val a, Val b) { double d = gm_real(b); return R(d == 0 ? 0 : trunc(gm_real(a) / d)); }
Val gm_mod(Val a, Val b) { double d = gm_real(b); return R(d == 0 ? 0 : fmod(gm_real(a), d)); }
#define I64(v) ((int64_t)gm_real(v))
Val gm_and(Val a, Val b) { return R((double)(I64(a) & I64(b))); }
Val gm_or(Val a, Val b) { return R((double)(I64(a) | I64(b))); }
Val gm_xor(Val a, Val b) { return R((double)(I64(a) ^ I64(b))); }
Val gm_shl(Val a, Val b) { return R((double)(I64(a) << I64(b))); }
Val gm_shr(Val a, Val b) { return R((double)(I64(a) >> I64(b))); }

/* ---------------- RNG ---------------- */

static uint32_t rnd32(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

Val gm_choose(int site, Val *a, int n) {
    if (n <= 0) return R(0);
    int k = gm_choose_force[site];
    if (k < 0 || k >= n) k = (int)(rnd32() % (unsigned)n);
    gm_choose_last[site] = k; gm_choose_count[site]++;
    return a[k];
}

/* ---------------- instances ---------------- */

Inst *gm_find(int id) {
    for (int k = 0; k < gm_ninsts; k++) if (gm_insts[k]->id == id && !gm_insts[k]->dead) return gm_insts[k];
    return NULL;
}

static bool obj_is(int obj, int target) {
    for (int o = obj; o >= 0; o = gm_objects[o].parent) if (o == target) return true;
    return false;
}

bool gm_is(Inst *i, int target) {
    if (i->dead || !i->active) return false;
    if (target == -3) return true;
    if (target >= 100000) return i->id == target;
    if (target >= 0) return obj_is(i->obj, target);
    return false;
}

static Inst *resolve1(Inst *self, Inst *other, int t) {
    if (t == -1) return self;
    if (t == -2) return other;
    if (t == -4) return NULL;
    for (int k = 0; k < gm_ninsts; k++) if (gm_is(gm_insts[k], t)) return gm_insts[k];
    return NULL;
}

static void set_hv(Inst *i) {
    i->speed = hypot(i->hspeed, i->vspeed);
    if (i->speed != 0) {
        double d = atan2(-i->vspeed, i->hspeed) * 180 / 3.14159265358979;
        i->direction = d < 0 ? d + 360 : d;
    }
}
static void set_sd(Inst *i) {
    double r = i->direction * 3.14159265358979 / 180;
    i->hspeed = i->speed * cos(r); i->vspeed = -i->speed * sin(r);
    if (fabs(i->hspeed) < 1e-10) i->hspeed = 0;
    if (fabs(i->vspeed) < 1e-10) i->vspeed = 0;
}

static int spr_frames(int s) { return s >= 0 && s < gm_nsprites ? gm_sprites[s].nframes : 0; }

static void bbox(Inst *i, double *l, double *t, double *r, double *b) {
    int s = i->mask >= 0 ? i->mask : i->sprite;
    const GMSprite *sp = &gm_sprites[s];
    double x0 = i->x + (sp->bl - sp->ox) * i->xscale, x1 = i->x + (sp->br + 1 - sp->ox) * i->xscale;
    double y0 = i->y + (sp->bt - sp->oy) * i->yscale, y1 = i->y + (sp->bb + 1 - sp->oy) * i->yscale;
    *l = fmin(x0, x1); *r = fmax(x0, x1); *t = fmin(y0, y1); *b = fmax(y0, y1);
}

static Val bi_get(Inst *i, int b, int idx) {
    static Inst dummy;
    if (!i) i = &dummy;
    switch (b) {
    case B_X: return R(i->x); case B_Y: return R(i->y);
    case B_XPREV: return R(i->xprev); case B_YPREV: return R(i->yprev);
    case B_XSTART: return R(i->xstart); case B_YSTART: return R(i->ystart);
    case B_HSPEED: return R(i->hspeed); case B_VSPEED: return R(i->vspeed);
    case B_SPEED: return R(i->speed); case B_DIRECTION: return R(i->direction);
    case B_FRICTION: return R(i->friction); case B_GRAVITY: return R(i->gravity);
    case B_GRAVDIR: return R(i->gravdir); case B_SPRITE: return R(i->sprite);
    case B_IMAGE_INDEX: return R(i->image_index); case B_IMAGE_SPEED: return R(i->image_speed);
    case B_XSCALE: return R(i->xscale); case B_YSCALE: return R(i->yscale);
    case B_ANGLE: return R(i->angle); case B_ALPHA: return R(i->alpha); case B_BLEND: return R(i->blend);
    case B_IMAGE_NUMBER: return R(spr_frames(i->sprite));
    case B_DEPTH: return R(i->depth); case B_VISIBLE: return R(i->visible);
    case B_SOLID: return R(i->solid); case B_PERSISTENT: return R(i->persistent);
    case B_MASK: return R(i->mask); case B_OBJECT_INDEX: return R(i->obj); case B_ID: return R(i->id);
    case B_ALARM: return R(idx >= 0 && idx < NALARMS ? i->alarm[idx] : -1);
    case B_ROOM: return R(gm_room); case B_ROOM_SPEED: return R(gm_room_speed);
    case B_ROOM_WIDTH: return R(gm_rooms[gm_room].w); case B_ROOM_HEIGHT: return R(gm_rooms[gm_room].h);
    case B_INSTANCE_COUNT: { int c = 0; for (int k = 0; k < gm_ninsts; k++) c += !gm_insts[k]->dead && gm_insts[k]->active; return R(c); }
    case B_MOUSE_X: return R(gm_mouse_x); case B_MOUSE_Y: return R(gm_mouse_y);
    case B_CURRENT_TIME: return R((double)clock() * 1000 / CLOCKS_PER_SEC);
    case B_FPS: return R(gm_room_speed);
    case B_BBOX_LEFT: case B_BBOX_RIGHT: case B_BBOX_TOP: case B_BBOX_BOTTOM: {
        if (i->sprite < 0 && i->mask < 0) return R(i->x);
        double l, t, r, bb; bbox(i, &l, &t, &r, &bb);
        return R(b == B_BBOX_LEFT ? l : b == B_BBOX_RIGHT ? r - 1 : b == B_BBOX_TOP ? t : bb - 1);
    }
    case B_SPRITE_WIDTH: return R(i->sprite >= 0 ? gm_sprites[i->sprite].w * i->xscale : 0);
    case B_SPRITE_HEIGHT: return R(i->sprite >= 0 ? gm_sprites[i->sprite].h * i->yscale : 0);
    case B_SCORE: return R(g_score); case B_LIVES: return R(g_lives); case B_HEALTH: return R(g_health);
    case B_ROOM_FIRST: return R(gm_room_order[0]); case B_ROOM_LAST: return R(gm_room_order[gm_nroom_order - 1]);
    }
    return R(0);
}

static void bi_set(Inst *i, int b, int idx, Val v) {
    double d = gm_real(v);
    if (b == B_ROOM) { gm_room_pending = (int)d; return; }
    if (b == B_ROOM_SPEED) { gm_room_speed = (int)d; return; }
    if (b == B_SCORE) { g_score = d; return; }
    if (b == B_LIVES) { g_lives = d; return; }
    if (b == B_HEALTH) { g_health = d; return; }
    if (!i) return;
    switch (b) {
    case B_X: i->x = d; break; case B_Y: i->y = d; break;
    case B_XPREV: i->xprev = d; break; case B_YPREV: i->yprev = d; break;
    case B_XSTART: i->xstart = d; break; case B_YSTART: i->ystart = d; break;
    case B_HSPEED: i->hspeed = d; set_hv(i); break; case B_VSPEED: i->vspeed = d; set_hv(i); break;
    case B_SPEED: i->speed = d; set_sd(i); break; case B_DIRECTION: i->direction = fmod(fmod(d, 360) + 360, 360); set_sd(i); break;
    case B_FRICTION: i->friction = d; break; case B_GRAVITY: i->gravity = d; break;
    case B_GRAVDIR: i->gravdir = d; break; case B_SPRITE: i->sprite = (int)d; break;
    case B_IMAGE_INDEX: i->image_index = d; break; case B_IMAGE_SPEED: i->image_speed = d; break;
    case B_XSCALE: i->xscale = d; break; case B_YSCALE: i->yscale = d; break;
    case B_ANGLE: i->angle = d; break; case B_ALPHA: i->alpha = d; break; case B_BLEND: i->blend = (int)d; break;
    case B_DEPTH: i->depth = d; break; case B_VISIBLE: i->visible = d > 0.5; break;
    case B_SOLID: i->solid = d > 0.5; break; case B_PERSISTENT: i->persistent = d > 0.5; break;
    case B_MASK: i->mask = (int)d; break;
    case B_ALARM: if (idx >= 0 && idx < NALARMS) i->alarm[idx] = d; break;
    default: gm_log("write to read-only builtin %s\n", gm_builtin_names[b]);
    }
}

static int warned_unset;
Val gm_get(Inst *self, Inst *other, int inst, int var) {
    const GMVar *v = &gm_vars[var];
    if (v->builtin >= 0) return bi_get(resolve1(self, other, inst == -5 ? -1 : inst), v->builtin, 0);
    if (inst == -5) return gm_globals[v->id];
    Inst *t = resolve1(self, other, inst);
    if (!t) {
        if (warned_unset++ < 20) gm_log("read %s from missing instance %d\n", v->name, inst);
        return R(0);
    }
    Val r = t->vars[v->id];
    return r.t == VT_UNDEF ? R(0) : r;
}

void gm_set(Inst *self, Inst *other, int inst, int var, Val val) {
    const GMVar *v = &gm_vars[var];
    if (inst == -5 && v->builtin < 0) { gm_globals[v->id] = val; return; }
    if (inst == -5) inst = -1;
    if (inst == -1 || inst == -2) {
        Inst *t = inst == -1 ? self : other;
        if (!t) return;
        if (v->builtin >= 0) bi_set(t, v->builtin, 0, val); else t->vars[v->id] = val;
        return;
    }
    if (v->builtin >= 0 && (v->builtin == B_ROOM || v->builtin == B_SCORE)) { bi_set(NULL, v->builtin, 0, val); return; }
    /* obj.var = x assigns every instance of obj */
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *t = gm_insts[k];
        if (!gm_is(t, inst)) continue;
        if (v->builtin >= 0) bi_set(t, v->builtin, 0, val); else t->vars[v->id] = val;
    }
}

int gm_inst_of(Val v) { return (int)gm_real(v); }

Val gm_aget(Inst *self, Inst *other, Val inst, int var, Val idx) {
    const GMVar *v = &gm_vars[var];
    int it = gm_inst_of(inst), ix = (int)gm_real(idx);
    Inst *t = it == -5 ? NULL : resolve1(self, other, it);
    if (v->builtin >= 0) return bi_get(t, v->builtin, ix);
    Val a = it == -5 ? gm_globals[v->id] : t ? t->vars[v->id] : U();
    if (a.t != VT_ARRAY || ix < 0 || ix >= a.a->n) return R(0);
    return a.a->v[ix];
}

static void arr_store(Val *slot, int ix, Val val) {
    if (ix < 0) return;
    if (slot->t != VT_ARRAY) { GMArray *a = calloc(1, sizeof *a); slot->t = VT_ARRAY; slot->a = a; }
    GMArray *a = slot->a;
    if (ix >= a->n) {
        a->v = realloc(a->v, (ix + 1) * sizeof(Val));
        for (int k = a->n; k <= ix; k++) a->v[k] = R(0);
        a->n = ix + 1;
    }
    a->v[ix] = val;
}

void gm_aset(Inst *self, Inst *other, Val inst, int var, Val idx, Val val) {
    const GMVar *v = &gm_vars[var];
    int it = gm_inst_of(inst), ix = (int)gm_real(idx);
    if (it == -5 && v->builtin < 0) { arr_store(&gm_globals[v->id], ix, val); return; }
    for (int k = -1; k < gm_ninsts; k++) {
        Inst *t;
        if (it == -1 || it == -2 || it == -5) { if (k >= 0) break; t = it == -2 ? other : self; }
        else { if (k < 0) continue; t = gm_insts[k]; if (!gm_is(t, it)) continue; }
        if (!t) continue;
        if (v->builtin >= 0) bi_set(t, v->builtin, ix, val); else arr_store(&t->vars[v->id], ix, val);
    }
}

/* ---------------- with() ---------------- */

bool gm_env_push(GMEnv *e, Inst **self, Inst **other, Val target) {
    int t = gm_inst_of(target);
    e->n = 0; e->k = 0; e->ids = NULL;
    if (t == -1 || t == -2) {
        Inst *i = t == -1 ? *self : *other;
        if (!i) return false;
        e->ids = malloc(sizeof(int)); e->ids[0] = i->id; e->n = 1;
    } else {
        e->ids = malloc(sizeof(int) * (gm_ninsts + 1));
        for (int k = 0; k < gm_ninsts; k++) if (gm_is(gm_insts[k], t)) e->ids[e->n++] = gm_insts[k]->id;
    }
    if (!e->n) { free(e->ids); return false; }
    e->saved_self = *self; e->saved_other = *other;
    *other = *self; *self = gm_find(e->ids[0]);
    return true;
}

bool gm_env_next(GMEnv *e, Inst **self) {
    while (++e->k < e->n) {
        Inst *i = gm_find(e->ids[e->k]);
        if (i && i->active) { *self = i; return true; }
    }
    return false;
}

void gm_env_pop(GMEnv *e, Inst **self, Inst **other) {
    *self = e->saved_self; *other = e->saved_other; free(e->ids); e->ids = NULL;
}

void gm_env_unwind(GMEnv *e, int ep, Inst **self, Inst **other) {
    while (ep > 0) gm_env_pop(&e[--ep], self, other);
}

/* ---------------- events ---------------- */

static int find_event(int obj, int type, int sub) {
    for (int o = obj; o >= 0; o = gm_objects[o].parent) {
        const GMObject *ob = &gm_objects[o];
        for (int k = 0; k < ob->nev; k++) {
            const GMEvent *ev = &gm_events[ob->ev0 + k];
            if (ev->type == type && ev->sub == sub) return ev->code;
        }
    }
    return -1;
}

bool gm_has_event(int obj, int type, int sub) { return find_event(obj, type, sub) >= 0; }

const char *gm_current_code;
void gm_event(Inst *i, int type, int sub, Inst *other) {
    int c = find_event(i->obj, type, sub);
    if (c < 0 || !gm_code[c].fn) return;
    const char *prev = gm_current_code; gm_current_code = gm_code[c].name;
    gm_code[c].fn(i, other ? other : i);
    gm_current_code = prev;
}

static Inst *alloc_inst(int obj, int id, double x, double y) {
    Inst *i = calloc(1, sizeof *i);
    const GMObject *o = &gm_objects[obj];
    i->id = id; i->obj = obj; i->active = true;
    i->x = i->xprev = i->xstart = x; i->y = i->yprev = i->ystart = y;
    i->sprite = o->sprite; i->mask = o->mask; i->visible = o->visible; i->solid = o->solid;
    i->persistent = o->persistent; i->depth = o->depth;
    i->image_speed = 1; i->xscale = i->yscale = 1; i->alpha = 1; i->blend = 0xFFFFFF;
    i->gravdir = 270;
    for (int k = 0; k < NALARMS; k++) i->alarm[k] = -1;
    i->vars = malloc(sizeof(Val) * (gm_ninstvars ? gm_ninstvars : 1));
    for (int k = 0; k < gm_ninstvars; k++) i->vars[k] = U();
    if (gm_ninsts == cap_insts) { cap_insts = cap_insts ? cap_insts * 2 : 256; gm_insts = realloc(gm_insts, cap_insts * sizeof *gm_insts); }
    gm_insts[gm_ninsts++] = i;
    return i;
}

Inst *gm_create(double x, double y, int obj, Inst *creator) {
    if (obj < 0 || obj >= gm_nobjects) { gm_log("instance_create: bad object %d\n", obj); return NULL; }
    Inst *i = alloc_inst(obj, next_id++, x, y);
    gm_event(i, EV_CREATE, 0, creator ? creator : i);
    return i;
}

void gm_destroy(Inst *i) {
    if (!i || i->dead) return;
    gm_event(i, EV_DESTROY, 0, i);
    i->dead = true;
}

void gm_change(Inst *i, int obj, bool perf) {
    if (!i || obj < 0 || obj >= gm_nobjects) return;
    if (perf) gm_event(i, EV_DESTROY, 0, i);
    const GMObject *o = &gm_objects[obj];
    i->obj = obj; i->sprite = o->sprite; i->mask = o->mask; i->visible = o->visible;
    i->solid = o->solid; i->persistent = o->persistent; i->depth = o->depth;
    if (perf) gm_event(i, EV_CREATE, 0, i);
}

static void free_inst(Inst *i) {
    free(i->vars); free(i);
}

static void compact(bool room_change) {
    int w = 0;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i->dead || (room_change && !i->persistent)) free_inst(i);
        else gm_insts[w++] = i;
    }
    gm_ninsts = w;
}

/* snapshot iteration: instances created during the pass wait for the next one */
#define EACH(i) for (int k_ = 0, n_ = gm_ninsts; k_ < n_; k_++) for (Inst *i = gm_insts[k_]; i && !i->dead && i->active; i = NULL)

static void room_enter(int r) {
    if (r < 0 || r >= gm_nrooms) { gm_log("room_goto: bad room %d\n", r); return; }
    bool first = gm_room < 0;
    if (!first) EACH(i) gm_event(i, EV_OTHER, 5, i);        /* room end */
    compact(true);
    gm_room = r; gm_room_speed = gm_rooms[r].speed;
    if (gm_rooms[r].persistent) gm_log("room %s is persistent; not supported, reloading fresh\n", gm_rooms[r].name);
    const GMRoom *rm = &gm_rooms[r];
    int created0 = gm_ninsts;
    for (int k = 0; k < rm->ninst; k++) {
        const GMRoomInst *ri = &gm_room_insts[rm->inst0 + k];
        if (ri->obj < 0 || gm_find(ri->id)) continue;
        Inst *i = alloc_inst(ri->obj, ri->id, ri->x, ri->y);
        i->xscale = ri->sx; i->yscale = ri->sy; i->angle = ri->rot;
        i->blend = ri->color & 0xFFFFFF; i->alpha = ((ri->color >> 24) & 0xFF) / 255.0;
        if (ri->id >= next_id) next_id = ri->id + 1;
    }
    for (int k = created0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        gm_event(i, EV_CREATE, 0, i);
        const GMRoomInst *ri = NULL;
        for (int q = 0; q < rm->ninst; q++) if (gm_room_insts[rm->inst0 + q].id == i->id) ri = &gm_room_insts[rm->inst0 + q];
        if (ri && ri->code >= 0 && gm_code[ri->code].fn) gm_code[ri->code].fn(i, i);
    }
    if (rm->code >= 0 && gm_code[rm->code].fn) gm_code[rm->code].fn(NULL, NULL);
    if (first) EACH(i) gm_event(i, EV_OTHER, 2, i);         /* game start */
    EACH(i) gm_event(i, EV_OTHER, 4, i);                    /* room start */
}

void gm_init(void) {
    gm_globals = malloc(sizeof(Val) * (gm_nglobals ? gm_nglobals : 1));
    for (int k = 0; k < gm_nglobals; k++) gm_globals[k] = U();
    for (int k = 0; k < gm_nchoose_sites; k++) { gm_choose_force[k] = -1; gm_choose_last[k] = -1; }
    if (!rng_state) rng_state = (uint64_t)time(NULL) * 2654435761u | 1;
    for (int r = 0; r < gm_nrooms; r++)
        for (int k = 0; k < gm_rooms[r].ninst; k++)
            if (gm_room_insts[gm_rooms[r].inst0 + k].id >= next_id) next_id = gm_room_insts[gm_rooms[r].inst0 + k].id + 1;
    gm_room_pending = gm_room_order[0];
}

void gm_seed(uint64_t s) { rng_state = s * 2654435761u | 1; }

/* ---------------- collision ---------------- */

static bool mask_bit(Inst *i, double px, double py) {
    int s = i->mask >= 0 ? i->mask : i->sprite;
    const GMSprite *sp = &gm_sprites[s];
    if (!sp->nmasks || gm_sprite_mask_off[s] < 0) return true;
    double lx = (px - i->x) / i->xscale + sp->ox, ly = (py - i->y) / i->yscale + sp->oy;
    int ix = (int)floor(lx), iy = (int)floor(ly);
    if (ix < 0 || iy < 0 || ix >= sp->w || iy >= sp->h) return false;
    int f = sp->nmasks > 1 ? ((int)floor(i->image_index) % sp->nmasks + sp->nmasks) % sp->nmasks : 0;
    int stride = (sp->w + 7) / 8;
    const uint8_t *m = host_data(gm_sprite_mask_off[s]) + (size_t)f * stride * sp->h;
    return (m[iy * stride + ix / 8] >> (7 - (ix & 7))) & 1;
}

void gm_bbox(Inst *i, double *l, double *t, double *r, double *b) { bbox(i, l, t, r, b); }

bool gm_point_in(Inst *i, double x, double y) {
    if (i->dead || !i->active || (i->sprite < 0 && i->mask < 0)) return false;
    double l, t, r, b; bbox(i, &l, &t, &r, &b);
    return x >= l && x < r && y >= t && y < b && mask_bit(i, x, y);
}

/* ponytail: bbox + precise masks, image_angle ignored; add rotation if a game spins colliders */
bool gm_collide(Inst *a, double ax, double ay, Inst *b) {
    if (a == b || (a->sprite < 0 && a->mask < 0) || (b->sprite < 0 && b->mask < 0)) return false;
    double ox = a->x, oy = a->y; a->x = ax; a->y = ay;
    double al, at, ar, ab, bl, bt, br, bb;
    bbox(a, &al, &at, &ar, &ab); bbox(b, &bl, &bt, &br, &bb);
    bool hit = false;
    double l = fmax(al, bl), r = fmin(ar, br), t = fmax(at, bt), btm = fmin(ab, bb);
    if (l < r && t < btm) {
        for (double y = floor(t); y < btm && !hit; y++)
            for (double x = floor(l); x < r; x++)
                if (mask_bit(a, x + 0.5, y + 0.5) && mask_bit(b, x + 0.5, y + 0.5)) { hit = true; break; }
    }
    a->x = ox; a->y = oy;
    return hit;
}

/* ---------------- frame ---------------- */

void gm_frame(void) {
    if (gm_room_pending >= 0) { int r = gm_room_pending; gm_room_pending = -1; room_enter(r); }
    EACH(i) { i->xprev = i->x; i->yprev = i->y; }
    EACH(i) gm_event(i, EV_STEP, 1, i);                      /* begin step */
    EACH(i) for (int a = 0; a < NALARMS && !i->dead; a++)
        if (i->alarm[a] > 0 && (i->alarm[a] -= 1) <= 0) { i->alarm[a] = -1; gm_event(i, EV_ALARM, a, i); }
    for (int k = 0; k < 256; k++) if (gm_key_down[k]) EACH(i) gm_event(i, EV_KEYBOARD, k, i);
    bool anyp = false, anyr = false;
    for (int k = 2; k < 256; k++) {
        if (gm_key_pressed[k]) { anyp = true; EACH(i) gm_event(i, EV_KEYPRESS, k, i); }
        if (gm_key_released[k]) { anyr = true; EACH(i) gm_event(i, EV_KEYRELEASE, k, i); }
    }
    if (anyp) EACH(i) gm_event(i, EV_KEYPRESS, 1, i);
    if (anyr) EACH(i) gm_event(i, EV_KEYRELEASE, 1, i);
    EACH(i) gm_event(i, EV_STEP, 0, i);
    if (gm_premove_hook) gm_premove_hook();
    EACH(i) {
        if (i->friction != 0 && i->speed != 0) {
            double s = fabs(i->speed) - i->friction; i->speed = s > 0 ? copysign(s, i->speed) : 0; set_sd(i);
        }
        if (i->gravity != 0) {
            double r = i->gravdir * 3.14159265358979 / 180;
            i->hspeed += i->gravity * cos(r); i->vspeed -= i->gravity * sin(r); set_hv(i);
        }
        i->x += i->hspeed; i->y += i->vspeed;
    }
    EACH(i) {
        const GMObject *o = &gm_objects[i->obj];
        for (int e = 0; e < o->nev && !i->dead; e++) {
            const GMEvent *ev = &gm_events[o->ev0 + e];
            if (ev->type != EV_COLLISION) continue;
            for (int q = 0; q < gm_ninsts && !i->dead; q++) {
                Inst *j = gm_insts[q];
                if (!gm_is(j, ev->sub) || !gm_collide(i, i->x, i->y, j)) continue;
                if (i->solid || j->solid) { i->x = i->xprev; i->y = i->yprev; }
                gm_event(i, EV_COLLISION, ev->sub, j);
            }
        }
    }
    EACH(i) gm_event(i, EV_STEP, 2, i);                      /* end step */
    EACH(i) {
        int n = spr_frames(i->sprite);
        if (n <= 0 || i->image_speed == 0) continue;
        i->image_index += i->image_speed;
        if (i->image_index >= n) { i->image_index -= n; gm_event(i, EV_OTHER, 7, i); }
        else if (i->image_index < 0) { i->image_index += n; gm_event(i, EV_OTHER, 7, i); }
    }
    compact(false);
}

static int cmp_depth(const void *a, const void *b) {
    const Inst *x = *(Inst *const *)a, *y = *(Inst *const *)b;
    if (x->depth != y->depth) return x->depth < y->depth ? 1 : -1;
    return x->id - y->id;
}

void gm_draw(void) {
    if (gm_room < 0) return;
    const GMRoom *rm = &gm_rooms[gm_room];
    if (rm->draw_color) host_clear(rm->color & 0xFFFFFF);
    for (int f = 0; f < 2; f++) {
        if (f == 1) {
            Inst **order = malloc(sizeof(Inst *) * (gm_ninsts + 1)); int n = 0;
            for (int k = 0; k < gm_ninsts; k++) {
                Inst *i = gm_insts[k];
                if (!i->dead && i->active && i->visible) order[n++] = i;
            }
            qsort(order, n, sizeof *order, cmp_depth);
            for (int k = 0; k < n; k++) {
                Inst *i = order[k];
                if (i->dead) continue;
                if (gm_has_event(i->obj, EV_DRAW, 0)) gm_event(i, EV_DRAW, 0, i);
                else if (i->sprite >= 0)
                    host_draw_sprite(i->sprite, i->image_index, i->x, i->y, i->xscale, i->yscale, i->angle, i->blend, i->alpha);
            }
            free(order);
        }
        for (int k = 0; k < rm->nbg; k++) {
            const GMRoomBg *b = &gm_room_bgs[rm->bg0 + k];
            if (b->fore == (f == 1)) host_draw_background(b->bg, b->x, b->y, b->tilex, b->tiley, b->stretch);
        }
    }
}

/* ---------------- savestates ----------------
 * Used for game_save/game_load and the dev menu's slots. Our own format, not
 * the YoYo runner's: the original's saves don't carry across. */

static void wval(FILE *f, Val v) {
    fwrite(&v.t, 4, 1, f);
    if (v.t == VT_REAL) fwrite(&v.r, 8, 1, f);
    else if (v.t == VT_STR) { int n = (int)strlen(v.s); fwrite(&n, 4, 1, f); fwrite(v.s, 1, n, f); }
    else if (v.t == VT_ARRAY) { fwrite(&v.a->n, 4, 1, f); for (int k = 0; k < v.a->n; k++) wval(f, v.a->v[k]); }
}
static Val rval(FILE *f) {
    Val v = U(); fread(&v.t, 4, 1, f);
    if (v.t == VT_REAL) fread(&v.r, 8, 1, f);
    else if (v.t == VT_STR) {
        int n = 0; fread(&n, 4, 1, f); char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0;
        v.s = gm_intern(b); free(b);
    } else if (v.t == VT_ARRAY) {
        GMArray *a = calloc(1, sizeof *a); fread(&a->n, 4, 1, f);
        a->v = malloc(sizeof(Val) * (a->n ? a->n : 1));
        for (int k = 0; k < a->n; k++) a->v[k] = rval(f);
        v.a = a;
    } else v.t = VT_UNDEF;
    return v;
}

#define SAVE_MAGIC 0x31534d47   /* "GMS1" */
bool gm_save_state(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int hdr[6] = { SAVE_MAGIC, gm_nglobals, gm_ninstvars, gm_room, next_id, 0 };
    int live = 0; for (int k = 0; k < gm_ninsts; k++) live += !gm_insts[k]->dead;
    hdr[5] = live;
    fwrite(hdr, 4, 6, f);
    fwrite(&g_score, 8, 1, f); fwrite(&g_lives, 8, 1, f); fwrite(&g_health, 8, 1, f);
    for (int k = 0; k < gm_nglobals; k++) wval(f, gm_globals[k]);
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i->dead) continue;
        Val *vars = i->vars; i->vars = NULL;
        fwrite(i, sizeof *i, 1, f);
        i->vars = vars;
        for (int q = 0; q < gm_ninstvars; q++) wval(f, vars[q]);
    }
    fclose(f);
    return true;
}

bool gm_load_state(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    int hdr[6];
    if (fread(hdr, 4, 6, f) != 6 || hdr[0] != SAVE_MAGIC || hdr[1] != gm_nglobals || hdr[2] != gm_ninstvars) {
        fclose(f); gm_log("load %s: not a save for this game\n", path); return false;
    }
    for (int k = 0; k < gm_ninsts; k++) free_inst(gm_insts[k]);
    gm_ninsts = 0;
    gm_room = hdr[3]; next_id = hdr[4]; gm_room_pending = -1;
    gm_room_speed = gm_rooms[gm_room].speed;
    fread(&g_score, 8, 1, f); fread(&g_lives, 8, 1, f); fread(&g_health, 8, 1, f);
    for (int k = 0; k < gm_nglobals; k++) gm_globals[k] = rval(f);
    for (int n = 0; n < hdr[5]; n++) {
        Inst *i = alloc_inst(0, 0, 0, 0);
        Val *vars = i->vars;
        fread(i, sizeof *i, 1, f);
        i->vars = vars;
        for (int q = 0; q < gm_ninstvars; q++) vars[q] = rval(f);
    }
    fclose(f);
    return true;
}
