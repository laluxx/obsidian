/// add_menu — editor subsystem
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "keychords.h"
#include "renderer.h"

/// Public interface

void add_menu_init(void);
void add_menu_cleanup(void);

void add_menu_open(double mx, double my);
void add_menu_close(void);
bool add_menu_is_open(void);
int32_t add_menu_visible_count(void);
int32_t add_menu_selection_index(void);
bool add_menu_set_selection_index(int32_t index);

// Mouse Input Routing
void add_menu_mouse_move(double mx, double my);
bool add_menu_mouse_button(int button, int action, double mx, double my);

void add_menu_render(void);
