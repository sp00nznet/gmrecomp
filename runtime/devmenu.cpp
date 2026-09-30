/* devmenu.cpp - the dev/cheat menu: an ImGui menu bar docked above the game.
 *
 * Generic across gmrecomp titles. It can poke every GML global and instance
 * variable directly because the recompiled game keeps them in plain C arrays
 * (gm_globals, Inst::vars). Game-specific cheats come from the game repo's
 * profile (devprofile.h). Layout and hotkeys: docs/devmenu.md.
 */
#include "devprofile.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL.h>
#include <sys/stat.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const char *gm_current_code;

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static int menu_h = 22;
static bool show_globals, show_insts, show_rng, show_controls, show_bbox, show_about;
static std::map<int, Val> frozen;            /* global id -> held value */
static int sel_inst = -1;
static int wait_key_for = -1;                /* game vk waiting for an extra keyboard key */
static int wait_pad_for = -1;                /* pad button waiting for a game key */
static bool turbo;
static char ini_path[1100];

/* ---------------- names ---------------- */

static const char *vk_name(int vk) {
    static char b[16];
    switch (vk) {
    case 0: return "-"; case 8: return "Backspace"; case 9: return "Tab"; case 13: return "Enter";
    case 16: return "Shift"; case 17: return "Ctrl"; case 18: return "Alt"; case 27: return "Esc";
    case 32: return "Space"; case 33: return "PgUp"; case 34: return "PgDn"; case 35: return "End";
    case 36: return "Home"; case 37: return "Left"; case 38: return "Up"; case 39: return "Right";
    case 40: return "Down"; case 45: return "Insert"; case 46: return "Delete";
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { b[0] = (char)vk; b[1] = 0; return b; }
    if (vk >= 112 && vk <= 123) { snprintf(b, sizeof b, "F%d", vk - 111); return b; }
    if (vk >= 96 && vk <= 105) { snprintf(b, sizeof b, "Num%d", vk - 96); return b; }
    snprintf(b, sizeof b, "vk%d", vk); return b;
}

static const char *pad_names[SDL_CONTROLLER_BUTTON_MAX] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "L-Stick", "R-Stick", "LB", "RB",
    "D-Up", "D-Down", "D-Left", "D-Right", "Misc", "Paddle1", "Paddle2", "Paddle3", "Paddle4", "Touchpad"
};

/* ---------------- helpers for profiles ---------------- */

static std::unordered_map<std::string, int> global_ids, object_ids;
static void build_maps(void) {
    if (!global_ids.empty()) return;
    for (int k = 0; k < gm_nvars; k++) if (gm_vars[k].itype == -5 && gm_vars[k].builtin < 0) global_ids[gm_vars[k].name] = gm_vars[k].id;
    for (int k = 0; k < gm_nobjects; k++) object_ids[gm_objects[k].name] = k;
}
int dev_global(const char *name) { build_maps(); auto it = global_ids.find(name); return it == global_ids.end() ? -1 : it->second; }
double dev_get(const char *g) { int id = dev_global(g); return id < 0 ? 0 : gm_real(gm_globals[id]); }
void dev_set(const char *g, double v) {
    int id = dev_global(g);
    if (id < 0) return;
    gm_globals[id] = R(v);
    auto it = frozen.find(id);
    if (it != frozen.end()) it->second = R(v);
}
bool dev_frozen(const char *g) { int id = dev_global(g); return id >= 0 && frozen.count(id); }
void dev_freeze(const char *g, bool on) {
    int id = dev_global(g);
    if (id < 0) return;
    if (on) frozen[id] = gm_globals[id]; else frozen.erase(id);
}
int dev_object(const char *name) { build_maps(); auto it = object_ids.find(name); return it == object_ids.end() ? -1 : it->second; }
Inst *dev_first(int obj) {
    for (int k = 0; k < gm_ninsts; k++) if (gm_is(gm_insts[k], obj)) return gm_insts[k];
    return nullptr;
}
bool dev_room_is(const char *name) { return gm_room >= 0 && !strcmp(gm_rooms[gm_room].name, name); }

void dev_luck_prefer(const char *code, const char *const *objs, int n) {
    for (int s = 0; s < gm_nchoose_sites; s++) {
        if (!strstr(gm_choose_sites[s].code, code)) continue;
        int best = -1, rank = 1 << 30;
        for (int o = 0; o < gm_choose_sites[s].nopt; o++) {
            const char *lab = gm_choose_opts[gm_choose_sites[s].opt0 + o].label;
            if (!lab) continue;
            for (int r = 0; r < n; r++) if (!strcmp(lab, objs[r]) && r < rank) { rank = r; best = o; }
        }
        if (best >= 0) gm_choose_force[s] = best;
    }
}
int dev_sites(const char *code, const char *target, int *out, int max) {
    int n = 0;
    for (int s = 0; s < gm_nchoose_sites && n < max; s++)
        if (strstr(gm_choose_sites[s].code, code) && (!target || !strcmp(gm_choose_sites[s].target, target))) out[n++] = s;
    return n;
}
void dev_force(int site, int opt) { if (site >= 0 && site < gm_nchoose_sites) gm_choose_force[site] = opt < gm_choose_sites[site].nopt ? opt : -1; }
void dev_force_value(int s, int dir) {
    if (s < 0 || s >= gm_nchoose_sites) return;
    int best = -1;
    for (int o = 0; o < gm_choose_sites[s].nopt; o++) {
        double v = gm_choose_opts[gm_choose_sites[s].opt0 + o].value;
        if (v != v) continue;
        if (best < 0 || (dir > 0 ? v > gm_choose_opts[gm_choose_sites[s].opt0 + best].value
                                 : v < gm_choose_opts[gm_choose_sites[s].opt0 + best].value)) best = o;
    }
    if (best >= 0) gm_choose_force[s] = best;
}
void dev_luck_value(const char *code, int dir) {
    for (int s = 0; s < gm_nchoose_sites; s++) if (strstr(gm_choose_sites[s].code, code)) dev_force_value(s, dir);
}
void dev_luck_clear(const char *code) {
    for (int s = 0; s < gm_nchoose_sites; s++) if (strstr(gm_choose_sites[s].code, code)) gm_choose_force[s] = -1;
}

/* ---------------- savestates ---------------- */

static const char *slot_path(int s) {
    static char p[1100]; snprintf(p, sizeof p, "%s/state%d.gms", gm_save_dir(), s); return p;
}
static std::string slot_label(int s) {
    struct stat st;
    char b[64];
    if (stat(slot_path(s), &st) != 0) { snprintf(b, sizeof b, "Slot %d  (empty)", s); return b; }
    char t[32]; strftime(t, sizeof t, "%b %d %H:%M", localtime(&st.st_mtime));
    snprintf(b, sizeof b, "Slot %d  %s", s, t);
    return b;
}
static void save_slot(int s) { if (!gm_save_state(slot_path(s))) gm_log("savestate %d failed\n", s); }
static void load_slot(int s) { if (!gm_load_state(slot_path(s))) gm_log("loadstate %d failed\n", s); }

/* ---------------- init / events ---------------- */

void dev_init(SDL_Window *w, SDL_Renderer *r) {
    g_win = w; g_ren = r;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    snprintf(ini_path, sizeof ini_path, "%s/devmenu.ini", gm_save_dir());
    io.IniFilename = ini_path;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4;
    ImGui_ImplSDL2_InitForSDLRenderer(w, r);
    ImGui_ImplSDLRenderer2_Init(r);
}

void dev_shutdown(void) {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

int dev_menu_height(void) { return menu_h; }

bool dev_event(SDL_Event *e) {
    ImGui_ImplSDL2_ProcessEvent(e);
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        int vk = host_vk_from_sdl(e->key.keysym.sym);
        if (wait_key_for >= 0) {
            if (vk && vk != wait_key_for) host.key_map[vk] = wait_key_for;
            wait_key_for = -1; host_save_cfg(); return true;
        }
        if (wait_pad_for >= 0) {
            if (vk) host.pad_map[wait_pad_for] = vk;
            wait_pad_for = -1; host_save_cfg(); return true;
        }
        switch (e->key.keysym.sym) {
        case SDLK_F6: save_slot(0); return true;
        case SDLK_F7: load_slot(0); return true;
        case SDLK_F8: host.paused = !host.paused; return true;
        case SDLK_F9: host.paused = true; host.step_once = true; return true;
        case SDLK_TAB: turbo = true; return true;
        case SDLK_BACKQUOTE: show_globals = !show_globals; return true;
        }
    }
    if (e->type == SDL_KEYUP && e->key.keysym.sym == SDLK_TAB) { turbo = false; return true; }
    ImGuiIO &io = ImGui::GetIO();
    if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP || e->type == SDL_TEXTINPUT) && io.WantTextInput) return true;
    if (e->type >= SDL_MOUSEMOTION && e->type <= SDL_MOUSEWHEEL && io.WantCaptureMouse) return true;
    return false;
}

static double base_speed = 1;
void dev_before_step(void) {
    host.speed = turbo ? base_speed * 4 : base_speed;
    for (auto &f : frozen) gm_globals[f.first] = f.second;
    profile_before_step();
}

void dev_after_step(void) { profile_after_step(); }
bool dev_click(double x, double y, int button, int key) { return profile_click(x, y, button, key); }
bool dev_key(int vk, bool down) { return profile_key(vk, down); }

/* ---------------- overlay ---------------- */

void dev_overlay(void) {
    if (!show_bbox) return;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i->dead || !i->active) continue;
        int s = i->mask >= 0 ? i->mask : i->sprite;
        if (s < 0) continue;
        const GMSprite *sp = &gm_sprites[s];
        float x0 = (float)(i->x + (sp->bl - sp->ox) * i->xscale), x1 = (float)(i->x + (sp->br + 1 - sp->ox) * i->xscale);
        float y0 = (float)(i->y + (sp->bt - sp->oy) * i->yscale), y1 = (float)(i->y + (sp->bb + 1 - sp->oy) * i->yscale);
        SDL_FRect r = { fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0) };
        SDL_SetRenderDrawColor(g_ren, i->solid ? 255 : 0, i->solid ? 64 : 255, 64, 255);
        SDL_RenderDrawRectF(g_ren, &r);
    }
}

/* ---------------- windows ---------------- */

static void edit_val(const char *id, Val *v, bool *changed) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (v->t == VT_REAL) {
        double d = v->r;
        if (ImGui::InputDouble("##v", &d, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) { *v = R(d); *changed = true; }
    } else if (v->t == VT_STR) {
        char b[256]; snprintf(b, sizeof b, "%s", v->s);
        if (ImGui::InputText("##v", b, sizeof b, ImGuiInputTextFlags_EnterReturnsTrue)) { *v = S(gm_intern(b)); *changed = true; }
    } else if (v->t == VT_ARRAY) {
        ImGui::TextDisabled("array[%d]", v->a->n);
    } else {
        ImGui::TextDisabled("unset");
    }
    ImGui::PopID();
}

static void globals_window(void) {
    if (!show_globals) return;
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Globals", &show_globals)) { ImGui::End(); return; }
    static char filter[64];
    static bool only_set = true;
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##f", "filter", filter, sizeof filter);
    ImGui::SameLine(); ImGui::Checkbox("hide unset", &only_set);
    ImGui::TextDisabled("Enter commits an edit. Freeze holds a value every step.");
    if (ImGui::BeginTable("g", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("freeze", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("name"); ImGui::TableSetupColumn("value");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        std::vector<bool> seen(gm_nglobals + 1);
        for (int k = 0; k < gm_nvars; k++) {
            const GMVar *v = &gm_vars[k];
            if (v->itype != -5 || v->builtin >= 0 || v->id < 0 || v->id >= gm_nglobals || seen[v->id]) continue;
            seen[v->id] = true;
            if (filter[0] && !strstr(v->name, filter)) continue;
            if (only_set && gm_globals[v->id].t == VT_UNDEF) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool fz = frozen.count(v->id) > 0;
            ImGui::PushID(v->id);
            if (ImGui::Checkbox("##fz", &fz)) { if (fz) frozen[v->id] = gm_globals[v->id]; else frozen.erase(v->id); }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v->name);
            ImGui::TableNextColumn();
            bool ch = false;
            edit_val(v->name, &gm_globals[v->id], &ch);
            if (ch && fz) frozen[v->id] = gm_globals[v->id];
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void edit_d(const char *label, double *d) {
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble(label, d, 0, 0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue);
}

static void insts_window(void) {
    if (!show_insts) return;
    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Instances", &show_insts)) { ImGui::End(); return; }
    static char filter[64];
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##f", "filter", filter, sizeof filter);
    ImGui::SameLine(); ImGui::Text("%d instances in %s", gm_ninsts, gm_room >= 0 ? gm_rooms[gm_room].name : "-");
    ImGui::BeginChild("list", ImVec2(230, 0), ImGuiChildFlags_Borders);
    Inst *sel = nullptr;
    for (int k = 0; k < gm_ninsts; k++) {
        Inst *i = gm_insts[k];
        if (i->dead) continue;
        const char *n = gm_objects[i->obj].name;
        if (filter[0] && !strstr(n, filter)) continue;
        char b[128]; snprintf(b, sizeof b, "%s%d %s", i->active ? "" : "(off) ", i->id, n);
        if (ImGui::Selectable(b, sel_inst == i->id)) sel_inst = i->id;
        if (sel_inst == i->id) sel = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("insp");
    if (sel) {
        ImGui::Text("%s  (id %d)", gm_objects[sel->obj].name, sel->id);
        edit_d("x", &sel->x); ImGui::SameLine(); edit_d("y", &sel->y);
        edit_d("hspeed", &sel->hspeed); ImGui::SameLine(); edit_d("vspeed", &sel->vspeed);
        edit_d("image_index", &sel->image_index); ImGui::SameLine(); edit_d("image_speed", &sel->image_speed);
        edit_d("depth", &sel->depth);
        ImGui::Checkbox("visible", &sel->visible); ImGui::SameLine(); ImGui::Checkbox("solid", &sel->solid);
        ImGui::SameLine(); ImGui::Checkbox("active", &sel->active);
        ImGui::Text("sprite %s", sel->sprite >= 0 ? gm_sprites[sel->sprite].name : "-");
        if (ImGui::TreeNode("alarms")) {
            for (int a = 0; a < NALARMS; a++) { char l[16]; snprintf(l, sizeof l, "alarm[%d]", a); edit_d(l, &sel->alarm[a]); }
            ImGui::TreePop();
        }
        ImGui::SeparatorText("variables");
        std::vector<bool> seen(gm_ninstvars + 1);
        for (int k = 0; k < gm_nvars; k++) {
            const GMVar *v = &gm_vars[k];
            if (v->itype != -1 || v->builtin >= 0 || v->id < 0 || v->id >= gm_ninstvars || seen[v->id]) continue;
            seen[v->id] = true;
            if (sel->vars[v->id].t == VT_UNDEF) continue;
            ImGui::TextUnformatted(v->name); ImGui::SameLine(160);
            bool ch = false; edit_val(v->name, &sel->vars[v->id], &ch);
        }
        if (ImGui::Button("Destroy")) gm_destroy(sel);
    } else ImGui::TextDisabled("select an instance");
    ImGui::EndChild();
    ImGui::End();
}

static std::string opt_text(int s, int o) {
    const GMSiteOpt *op = &gm_choose_opts[gm_choose_sites[s].opt0 + o];
    char b[160];
    if (op->value != op->value) snprintf(b, sizeof b, "#%d", o + 1);
    else if (op->label) snprintf(b, sizeof b, "%g -> %s", op->value, op->label);
    else snprintf(b, sizeof b, "%g", op->value);
    return b;
}

static void rng_window(void) {
    if (!show_rng) return;
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Luck: every choose() in the game", &show_rng)) { ImGui::End(); return; }
    static char filter[64];
    static bool ran_only, forced_only;
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##f", "filter by code name", filter, sizeof filter);
    ImGui::SameLine(); ImGui::Checkbox("ran", &ran_only);
    ImGui::SameLine(); ImGui::Checkbox("forced", &forced_only);
    ImGui::SameLine(); if (ImGui::Button("All random")) for (int s = 0; s < gm_nchoose_sites; s++) gm_choose_force[s] = -1;
    if (ImGui::BeginTable("r", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("code"); ImGui::TableSetupColumn("force", ImGuiTableColumnFlags_WidthFixed, 230);
        ImGui::TableSetupColumn("ran", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("last", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int s = 0; s < gm_nchoose_sites; s++) {
            const char *code = gm_choose_sites[s].code;
            if (filter[0] && !strstr(code, filter)) continue;
            if (ran_only && !gm_choose_count[s]) continue;
            if (forced_only && gm_choose_force[s] < 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(strncmp(code, "gml_Object_", 11) ? code : code + 11);
            if (gm_choose_sites[s].target[0]) { ImGui::SameLine(); ImGui::TextDisabled("-> %s", gm_choose_sites[s].target); }
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            ImGui::SetNextItemWidth(-FLT_MIN);
            std::string cur = gm_choose_force[s] < 0 ? "random" : opt_text(s, gm_choose_force[s]);
            if (ImGui::BeginCombo("##c", cur.c_str())) {
                if (ImGui::Selectable("random", gm_choose_force[s] < 0)) gm_choose_force[s] = -1;
                for (int o = 0; o < gm_choose_sites[s].nopt; o++)
                    if (ImGui::Selectable(opt_text(s, o).c_str(), gm_choose_force[s] == o)) gm_choose_force[s] = o;
                ImGui::EndCombo();
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%d", gm_choose_count[s]);
            ImGui::TableNextColumn();
            if (gm_choose_last[s] >= 0) ImGui::TextUnformatted(opt_text(s, gm_choose_last[s]).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void controls_window(void) {
    if (!show_controls) return;
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Controls", &show_controls)) { ImGui::End(); return; }
    int npads = 0;
    for (int j = 0; j < SDL_NumJoysticks(); j++) npads += SDL_IsGameController(j);
    ImGui::Text("Gamepads connected: %d", npads);
    if (wait_key_for >= 0 || wait_pad_for >= 0) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press a key...");
    if (ImGui::Button("Reset to defaults")) { host_default_bindings(); host_save_cfg(); }

    ImGui::SeparatorText("Extra keyboard keys");
    ImGui::TextDisabled("Every key keeps its normal meaning; add more keys for a game key.");
    static std::vector<int> game_keys;
    if (game_keys.empty()) {
        bool used[256] = {};
        for (int k = 0; k < gm_nobjects; k++)
            for (int e = 0; e < gm_objects[k].nev; e++) {
                const GMEvent *ev = &gm_events[gm_objects[k].ev0 + e];
                if ((ev->type == EV_KEYPRESS || ev->type == EV_KEYRELEASE || ev->type == EV_KEYBOARD) && ev->sub > 1 && ev->sub < 256) used[ev->sub] = true;
            }
        for (int k = 0; k < 256; k++) if (used[k]) game_keys.push_back(k);
    }
    if (ImGui::BeginTable("k", 3, ImGuiTableFlags_RowBg)) {
        for (int vk : game_keys) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(vk_name(vk));
            ImGui::TableNextColumn();
            ImGui::PushID(vk);
            for (int k = 0; k < 256; k++) if (host.key_map[k] == vk) {
                ImGui::PushID(k);
                char b[32]; snprintf(b, sizeof b, "%s x", vk_name(k));
                if (ImGui::SmallButton(b)) { host.key_map[k] = 0; host_save_cfg(); }
                ImGui::SameLine(); ImGui::PopID();
            }
            ImGui::NewLine();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("+ key")) { wait_key_for = vk; wait_pad_for = -1; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Gamepad");
    ImGui::TextDisabled("Left stick works as the d-pad.");
    if (ImGui::BeginTable("p", 3, ImGuiTableFlags_RowBg)) {
        for (int b = 0; b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; b++) {
            if (b == SDL_CONTROLLER_BUTTON_GUIDE) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(pad_names[b]);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(vk_name(host.pad_map[b]));
            ImGui::TableNextColumn();
            ImGui::PushID(1000 + b);
            if (ImGui::SmallButton("set")) { wait_pad_for = b; wait_key_for = -1; }
            ImGui::SameLine(); if (ImGui::SmallButton("clear")) { host.pad_map[b] = 0; host_save_cfg(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

/* ---------------- menu bar ---------------- */

static void set_scale(int s) {
    host.scale = s;
    SDL_SetWindowSize(g_win, gm_window_w * s, gm_window_h * s + menu_h);
    host_save_cfg();
}

void dev_frame(void) {
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        menu_h = (int)ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::BeginMenu("Save state")) {
                for (int s = 0; s < 10; s++) if (ImGui::MenuItem(slot_label(s).c_str(), s == 0 ? "F6" : nullptr)) save_slot(s);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Load state")) {
                for (int s = 0; s < 10; s++) if (ImGui::MenuItem(slot_label(s).c_str(), s == 0 ? "F7" : nullptr)) load_slot(s);
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("States: %s", gm_save_dir());
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) gm_quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game")) {
            ImGui::MenuItem("Pause", "F8", &host.paused);
            if (ImGui::MenuItem("Step one frame", "F9")) { host.paused = true; host.step_once = true; }
            ImGui::SeparatorText("Speed (hold Tab for 4x)");
            const double speeds[] = { 0.25, 0.5, 1, 2, 4, 8 };
            for (double s : speeds) { char b[16]; snprintf(b, sizeof b, "%gx", s); if (ImGui::MenuItem(b, nullptr, base_speed == s)) base_speed = s; }
            ImGui::Separator();
            if (ImGui::MenuItem("Restart room")) gm_room_pending = gm_room;
            if (ImGui::BeginMenu("Go to room")) {
                for (int k = 0; k < gm_nroom_order; k++) {
                    int r = gm_room_order[k];
                    if (ImGui::MenuItem(gm_rooms[r].name, nullptr, r == gm_room)) gm_room_pending = r;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (profile_title && ImGui::BeginMenu(profile_title)) { profile_menu(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Luck")) {
            ImGui::MenuItem("Every choose() site...", nullptr, &show_rng);
            if (ImGui::MenuItem("Everything random again")) for (int s = 0; s < gm_nchoose_sites; s++) gm_choose_force[s] = -1;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Controls")) {
            ImGui::MenuItem("Keys and gamepad...", nullptr, &show_controls);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Graphics")) {
            for (int s = 1; s <= 4; s++) { char b[16]; snprintf(b, sizeof b, "%dx window", s); if (ImGui::MenuItem(b, nullptr, host.scale == s)) set_scale(s); }
            bool sm = host.smooth != 0;
            if (ImGui::MenuItem("Smooth scaling", nullptr, &sm)) { host.smooth = sm; host_save_cfg(); }
            bool fs = host_get_fullscreen();
            if (ImGui::MenuItem("Fullscreen", nullptr, &fs)) host_set_fullscreen(fs);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Audio")) {
            float sfx = (float)host.sfx_volume, mus = (float)host.music_volume;
            if (ImGui::SliderFloat("Sound effects", &sfx, 0, 1)) host.sfx_volume = sfx;
            if (ImGui::SliderFloat("Music", &mus, 0, 1)) host.music_volume = mus;
            bool m = host.mute != 0;
            if (ImGui::Checkbox("Mute", &m)) host.mute = m;
            if (ImGui::IsItemDeactivatedAfterEdit()) host_save_cfg();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Globals", "`", &show_globals);
            ImGui::MenuItem("Instances", nullptr, &show_insts);
            ImGui::MenuItem("Collision boxes", nullptr, &show_bbox);
            ImGui::Separator();
            ImGui::MenuItem("About", nullptr, &show_about);
            ImGui::EndMenu();
        }
        char st[160];
        snprintf(st, sizeof st, "%s%s%s", host.paused ? "PAUSED  " : "", turbo ? "TURBO  " : "",
                 gm_room >= 0 ? gm_rooms[gm_room].name : "");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(st).x - 12);
        ImGui::TextDisabled("%s", st);
        ImGui::EndMainMenuBar();
    }
    globals_window();
    insts_window();
    rng_window();
    controls_window();
    if (show_about) {
        ImGui::Begin("About", &show_about, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%s, statically recompiled by gmrecomp", gm_game_name);
        ImGui::Text("%d code entries, %d objects, %d rooms, %d choose() sites", gm_ncode, gm_nobjects, gm_nrooms, gm_nchoose_sites);
        ImGui::TextDisabled("F6/F7 quick save/load state, F8 pause, F9 step, Tab turbo, ` globals");
        ImGui::End();
    }
    profile_windows();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
}
