/* gm_funcs.c - the GML built-in functions bytecode-15 games call.
 *
 * Only what the lifted games reference is here; the recompiler writes the list
 * a game needs to <gen>/funcs.txt and a missing one is a link error, which is
 * the point: no silent stubs. Arguments arrive with a[0] = first argument.
 */
#include "gmrt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define A(k) (k < n ? gm_real(a[k]) : 0)
#define AI(k) ((int)A(k))
#define ZERO R(0)

static Inst *target1(Inst *self, int t) {
    if (t == -1) return self;
    for (int k = 0; k < gm_ninsts; k++) if (gm_is(gm_insts[k], t)) return gm_insts[k];
    return NULL;
}

GMF(instance_destroy) { (void)other; (void)a; (void)n; gm_destroy(self); return ZERO; }
GMF(action_kill_object) { (void)other; (void)a; (void)n; gm_destroy(self); return ZERO; }

GMF(instance_create) { (void)other; Inst *i = gm_create(A(0), A(1), AI(2), self); return R(i ? i->id : -4); }
GMF(action_create_object) { (void)other; Inst *i = gm_create(A(1), A(2), AI(0), self); return R(i ? i->id : -4); }

GMF(instance_change) { (void)other; gm_change(self, AI(0), n > 1 && gm_truthy(a[1])); return ZERO; }
GMF(action_change_object) { (void)other; gm_change(self, AI(0), n > 1 && gm_truthy(a[1])); return ZERO; }

GMF(instance_exists) {
    (void)self; (void)other;
    for (int k = 0; k < gm_ninsts; k++) if (gm_is(gm_insts[k], AI(0))) return R(1);
    return ZERO;
}

static void set_active(int t, bool on) {
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i->dead) continue;
        bool match = t == -3 || (t >= 100000 ? i->id == t : t >= 0 && i->obj == t);
        if (match) i->active = on;
    }
}
GMF(instance_deactivate_object) { (void)self; (void)other; set_active(AI(0), false); return ZERO; }
GMF(instance_activate_object) { (void)self; (void)other; set_active(AI(0), true); return ZERO; }

GMF(instance_nearest) {
    (void)self; (void)other;
    double x = A(0), y = A(1), best = 1e300; int id = -4;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (!gm_is(i, AI(2))) continue;
        double d = hypot(i->x - x, i->y - y);
        if (d < best) { best = d; id = i->id; }
    }
    return R(id);
}

GMF(place_meeting) {
    (void)other;
    if (!self) return ZERO;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i != self && gm_is(i, AI(2)) && gm_collide(self, A(0), A(1), i)) return R(1);
    }
    return ZERO;
}

GMF(place_free) {
    (void)other;
    if (!self) return R(1);
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i != self && i->solid && gm_is(i, -3) && gm_collide(self, A(0), A(1), i)) return ZERO;
    }
    return R(1);
}

GMF(action_set_alarm) { (void)other; if (self && AI(1) >= 0 && AI(1) < NALARMS) self->alarm[AI(1)] = A(0); return ZERO; }
GMF(action_set_hspeed) {
    (void)other; if (!self) return ZERO;
    self->hspeed = A(0); self->speed = hypot(self->hspeed, self->vspeed);
    if (self->speed) self->direction = fmod(atan2(-self->vspeed, self->hspeed) * 57.29577951308232 + 360, 360);
    return ZERO;
}
GMF(action_set_vspeed) {
    (void)other; if (!self) return ZERO;
    self->vspeed = A(0); self->speed = hypot(self->hspeed, self->vspeed);
    if (self->speed) self->direction = fmod(atan2(-self->vspeed, self->hspeed) * 57.29577951308232 + 360, 360);
    return ZERO;
}
GMF(action_sprite_set) {
    (void)other; if (!self) return ZERO;
    self->sprite = AI(0);
    if (AI(1) != -1) self->image_index = A(1);
    self->image_speed = A(2);
    return ZERO;
}

GMF(room_goto) { (void)self; (void)other; gm_room_pending = AI(0); return ZERO; }
static int room_pos(void) { for (int k = 0; k < gm_nroom_order; k++) if (gm_room_order[k] == gm_room) return k; return -1; }
GMF(room_goto_next) {
    (void)self; (void)other; (void)a; (void)n;
    int p = room_pos();
    if (p >= 0 && p + 1 < gm_nroom_order) gm_room_pending = gm_room_order[p + 1];
    return ZERO;
}
GMF(room_goto_previous) {
    (void)self; (void)other; (void)a; (void)n;
    int p = room_pos();
    if (p > 0) gm_room_pending = gm_room_order[p - 1];
    return ZERO;
}

GMF(keyboard_check) { (void)self; (void)other; int k = AI(0) & 255; return R(k == 1 ? 0 : gm_key_down[k]); }
GMF(keyboard_check_pressed) { (void)self; (void)other; return R(gm_key_pressed[AI(0) & 255]); }
GMF(keyboard_check_released) { (void)self; (void)other; return R(gm_key_released[AI(0) & 255]); }

/* audio: both the new audio_* API and the legacy sound_* / action_sound one */
GMF(audio_play_sound) { (void)self; (void)other; return R(host_sound_play(AI(0), n > 2 && gm_truthy(a[2]))); }
GMF(audio_stop_sound) { (void)self; (void)other; host_sound_stop(AI(0)); return ZERO; }
GMF(audio_is_playing) { (void)self; (void)other; return R(host_sound_playing(AI(0))); }
GMF(audio_sound_gain) { (void)self; (void)other; host_sound_gain(AI(0), A(1), A(2)); return ZERO; }
GMF(sound_loop) { (void)self; (void)other; host_sound_stop(AI(0)); host_sound_play(AI(0), true); return ZERO; }
GMF(sound_stop_all) { (void)self; (void)other; (void)a; (void)n; host_sound_stop_all(); return ZERO; }
GMF(action_sound) {
    (void)self; (void)other;
    /* legacy sounds restart rather than stack */
    host_sound_stop(AI(0)); host_sound_play(AI(0), n > 1 && gm_truthy(a[1]));
    return ZERO;
}
GMF(action_end_sound) { (void)self; (void)other; host_sound_stop(AI(0)); return ZERO; }

GMF(draw_set_font) { (void)self; (void)other; gm_draw_font = AI(0); return ZERO; }
GMF(draw_set_color) { (void)self; (void)other; gm_draw_color = AI(0); return ZERO; }
GMF(draw_text) { (void)self; (void)other; host_draw_text(A(0), A(1), n > 2 ? gm_tostr(a[2]) : ""); return ZERO; }
GMF(draw_sprite) {
    (void)other;
    double sub = A(1);
    if (sub < 0 && self) sub = self->image_index;
    host_draw_sprite(AI(0), sub, A(2), A(3), 1, 1, 0, 0xFFFFFF, gm_draw_alpha);
    return ZERO;
}
GMF(sprite_exists) { (void)self; (void)other; return R(AI(0) >= 0 && AI(0) < gm_nsprites); }

static const char *save_path(Val v) {
    static char p[1024];
    snprintf(p, sizeof p, "%s/%s", gm_save_dir(), gm_tostr(v));
    return p;
}
GMF(file_exists) { (void)self; (void)other; FILE *f = n ? fopen(save_path(a[0]), "rb") : NULL; if (f) fclose(f); return R(f != NULL); }
GMF(file_delete) { (void)self; (void)other; return R(n && remove(save_path(a[0])) == 0); }
GMF(game_save) { (void)self; (void)other; return R(n && gm_save_state(save_path(a[0]))); }
/* game_load swaps the whole world, so it waits until the current step ends */
char gm_pending_load[1024];
GMF(game_load) { (void)self; (void)other; if (n) snprintf(gm_pending_load, sizeof gm_pending_load, "%s", save_path(a[0])); return ZERO; }
GMF(game_end) { (void)self; (void)other; (void)a; (void)n; gm_quit = true; return ZERO; }

GMF(window_get_fullscreen) { (void)self; (void)other; (void)a; (void)n; return R(host_get_fullscreen()); }
GMF(window_set_fullscreen) { (void)self; (void)other; host_set_fullscreen(n && gm_truthy(a[0])); return ZERO; }

/* No Steam here: achievements report locked and unlocks are only logged. */
GMF(steam_get_achievement) { (void)self; (void)other; (void)a; (void)n; return ZERO; }
GMF(steam_set_achievement) { (void)self; (void)other; if (n) gm_log("achievement: %s\n", gm_tostr(a[0])); return ZERO; }
GMF(action_snapshot) { (void)self; (void)other; (void)a; (void)n; return ZERO; }
