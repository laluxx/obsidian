#pragma once

#include <stdbool.h>
#include "keychords.h"
#include "renderer.h"

void add_menu_init(void);
void add_menu_cleanup(void);

void add_menu_open(double mx, double my);
void add_menu_close(void);
bool add_menu_is_open(void);

// Mouse Input Routing
void add_menu_mouse_move(double mx, double my);
bool add_menu_mouse_button(int button, int action, double mx, double my);

void add_menu_render(void);
