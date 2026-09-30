/* devprofile.h - what a game repo implements to add its own cheats, and the
 * helpers it gets to do it. The toolkit menu is generic; anything that knows
 * a game's variable names lives in the game repo's profile (repo rules: game
 * hacks stay out of the toolkit). runtime/profile_none.cpp is the default.
 */
#pragma once
#include "gmrt.h"
#include "gmhost.h"

extern "C" {
extern const char *profile_title;       /* name of the game's cheat menu */
void profile_menu(void);                /* ImGui items inside that menu */
void profile_windows(void);             /* extra ImGui windows, every frame */
void profile_before_step(void);         /* runs before every game step */
void profile_after_step(void);          /* runs after every game step */
}

/* helpers (devmenu.cpp) */
int dev_global(const char *name);               /* global id, -1 if unknown */
double dev_get(const char *global);             /* 0 if unknown */
void dev_set(const char *global, double v);
bool dev_frozen(const char *global);
void dev_freeze(const char *global, bool on);   /* hold at its current value */
int dev_object(const char *name);               /* object index, -1 if unknown */
Inst *dev_first(int obj);                       /* first live instance or NULL */
bool dev_room_is(const char *name);

/* Luck: steer choose() sites whose code entry name contains `code`.
 * prefer: options whose outcome label is earlier in `objs` win.
 * value:  +1 = largest value, -1 = smallest. clear: back to random. */
void dev_luck_prefer(const char *code, const char *const *objs, int n);
void dev_luck_value(const char *code, int dir);
void dev_luck_clear(const char *code);
/* finer control: sites in `code` assigning variable `target` (NULL = any) */
int dev_sites(const char *code, const char *target, int *out, int max);
void dev_force(int site, int opt);              /* -1 = random */
void dev_force_value(int site, int dir);        /* +1 largest, -1 smallest */
