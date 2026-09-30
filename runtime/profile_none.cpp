/* profile_none.cpp - default profile: no game-specific cheats. */
#include "devprofile.h"
const char *profile_title = nullptr;
void profile_menu(void) {}
void profile_windows(void) {}
void profile_before_step(void) {}
void profile_after_step(void) {}
bool profile_click(double x, double y, int button, int key) { (void)x; (void)y; (void)button; (void)key; return false; }
bool profile_key(int vk, bool down) { (void)vk; (void)down; return false; }
