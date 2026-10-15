/// repl_panel.h — Monad REPL window hosted by the reusable editor UI system
#ifndef OBSIDIAN_REPL_PANEL_H
#define OBSIDIAN_REPL_PANEL_H
#include "font.h"
void obs_repl_panel_init(void);
void obs_repl_panel_cleanup(void);
void obs_repl_panel_toggle(void);
void obs_repl_panel_update(float dt, double mx, double my);
bool obs_repl_panel_mouse_move(double mx, double my);
bool obs_repl_panel_mouse_button(int button, int action, double mx, double my);
void obs_repl_panel_render(Font *font);
bool obs_repl_panel_active(void);
#endif
