#pragma once

#include <stdbool.h>
#include "common.h"

typedef struct {
    char *name;
    Color bg;
    Color cursor;
    Color marked_cursor;
    Color text;
    Color minibuffer;
    Color modeline;
    Color modeline_inactive;
    Color modeline_highlight;
    Color modeline_border;
    Color show_paren_match;
    Color isearch_highlight;
    Color minibuffer_prompt;
    Color region;
    Color region_fg;
    Color message;
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
    Color fringe;
    Color diff_hl_change;
    Color diff_hl_insert;
    Color diff_hl_change_cursor;
    Color diff_hl_insert_cursor;
    Color diff_hl_bg;
    Color diff_hl_change_bg;
    Color clock;
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

    Color vertico_current;

    Color orderless_match_face_0_bg;
    Color orderless_match_face_0_fg;
    Color orderless_match_face_1_bg;
    Color orderless_match_face_1_fg;
    Color orderless_match_face_2_bg;
    Color orderless_match_face_2_fg;
    Color orderless_match_face_3_bg;
    Color orderless_match_face_3_fg;
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

