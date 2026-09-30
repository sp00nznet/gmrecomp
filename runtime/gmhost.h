/* gmhost.h - host settings shared between gm_host.c and the dev menu. */
#pragma once
#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HostCfg {
    double speed;                           /* game speed multiplier */
    bool paused, step_once;
    int scale, smooth, mute;
    double sfx_volume, music_volume;
    int pad_map[SDL_CONTROLLER_BUTTON_MAX]; /* button -> GameMaker vk */
    int key_map[256];                       /* vk -> vk remap, 0 = unchanged */
} HostCfg;
extern HostCfg host;

void host_vk(int vk, bool down);
int host_vk_from_sdl(SDL_Keycode k);
void host_save_cfg(void);
void host_default_bindings(void);
void host_click(double x, double y, int button); /* room coords; SDL_BUTTON_* */
void host_key_seq(int vk, int delay, int hold);  /* press vk after `delay` steps for `hold` steps */

/* dev menu (devmenu.cpp) */
void dev_init(SDL_Window *w, SDL_Renderer *r);
bool dev_event(SDL_Event *e);     /* true = consumed, don't pass to the game */
bool dev_click(double x, double y, int button, int key);  /* profile sees a click first; key = what it would tap */
bool dev_key(int vk, bool down);                         /* profile sees keys first; true = swallowed */
void dev_before_step(void);       /* freezes / forced values, run before each step */
void dev_after_step(void);
void dev_overlay(void);           /* drawn into the game target (collision boxes) */
void dev_frame(void);             /* ImGui menu bar + windows */
int dev_menu_height(void);
void dev_shutdown(void);

#ifdef __cplusplus
}
#endif
