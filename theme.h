#pragma once

#include <stdbool.h>
#include "common.h"

typedef struct {
    char *name;
    Color bg;
    Color cursor;
    Color text;
    Color minibuffer;
    Color modeline_border;
    Color show_paren_match;
    Color isearch_highlight;
    Color minibuffer_prompt;
    Color region_fg;
    Color region_bg;
    Color type;
    Color string;
    Color number;
    Color function;
    Color preprocessor;
    Color variable;
    Color keyword;
    Color comment;
    Color null;
    Color negation;
    Color success;
    Color warning;
    Color error;
    Color diff_hl_change;
    Color diff_hl_insert;
    Color diff_hl_change_cursor;
    Color diff_hl_insert_cursor;
    Color diff_hl_bg;
    Color diff_hl_change_bg;
    Color fringe_bg;
    Color fringe_fg;
    Color header_line;
    Color rainbow_delimiters_base_face;
    Color rainbow_delimiters_depth_1_face;
    Color rainbow_delimiters_depth_2_face;
    Color rainbow_delimiters_depth_3_face;
    Color rainbow_delimiters_depth_4_face;
    Color rainbow_delimiters_depth_5_face;
    Color rainbow_delimiters_depth_6_face;

    Color diredfl_dir_heading;
    Color diredfl_dir_priv;
    Color diredfl_read_priv;
    Color diredfl_write_priv;
    Color diredfl_exec_priv;
    Color diredfl_no_priv;
    Color diredfl_number;
    Color diredfl_date_time;
    Color diredfl_dir_name;
    Color diredfl_file_suffix;

    Color button_fg;
    Color button_bg;

    Color window_divider;

    Color mode_line_active_fg;
    Color mode_line_active_bg;
    Color mode_line_inactive_fg;
    Color mode_line_inactive_bg;

    Color vertico_current;

    Color orderless_match_face_0_bg;
    Color orderless_match_face_0_fg;
    Color orderless_match_face_1_bg;
    Color orderless_match_face_1_fg;
    Color orderless_match_face_2_bg;
    Color orderless_match_face_2_fg;
    Color orderless_match_face_3_bg;
    Color orderless_match_face_3_fg;

    Color font_lock_builtin_face_bg;
    Color font_lock_builtin_face_fg;

} Theme;


void initThemes();
const Theme* getCurrentTheme();
void nextTheme();
void previousTheme();
void switchToTheme(int index);
void loadThemeByName(const char *themeName);

const char* getCurrentThemeName();
int getCurrentThemeIndex();
int getThemeCount();

#define CT (*getCurrentTheme())

