/* devmenu_stub.c - no-op dev menu for builds without ImGui. */
#include "gmhost.h"
void dev_init(SDL_Window *w, SDL_Renderer *r) { (void)w; (void)r; }
bool dev_event(SDL_Event *e) { (void)e; return false; }
void dev_before_step(void) {}
bool dev_click(double x, double y, int button, int key) { (void)x; (void)y; (void)button; (void)key; return false; }
bool dev_key(int vk, bool down) { (void)vk; (void)down; return false; }
void dev_after_step(void) {}
void dev_overlay(void) {}
void dev_frame(void) {}
int dev_menu_height(void) { return 0; }
void dev_shutdown(void) {}
