/*
 * mintbar configuration
 *
 * Customize appearance, colors, intervals, and the left, middle,
 * and right blocks below.
 */

#ifndef BAR_CONFIG_H
#define BAR_CONFIG_H

#include <stdbool.h>

/* Bar geometry & positioning */
static const int bar_height = 24;
static const bool bar_bottom = false; /* false = top of screen, true = bottom */

/* Typography */
static const char *font_name = "monospace 10";

/* Colors (Hex: #RGB, #RRGGBB, or #RRGGBBAA) */
static const char *color_bg          = "#1e1e2e"; /* Bar background */
static const char *color_fg          = "#cdd6f4"; /* Default text color */
static const char *color_active_ws   = "#89b4fa"; /* Active workspace highlight */
static const char *color_inactive_ws = "#6c7086"; /* Inactive workspace color */

/* Workspace options */
static const char *ws_prefix = ""; /* Default text/markup before workspaces (e.g. "WS: " or "[") */
static const char *ws_suffix = ""; /* Default text/markup after workspaces (e.g. " |" or "]") */

/* Update intervals (milliseconds) */
static const int status_interval_ms = 1000; /* Interval for BLOCK_COMMAND execution */

/* Spacing */
static const int padding_x = 10; /* Horizontal padding from bar edges (pixels) */

/*
 * Block types:
 *   BLOCK_WORKSPACES - Receives live MinT IPC workspace state [active]
 *   BLOCK_TITLE      - Receives live MinT IPC active window title
 *   BLOCK_COMMAND    - Execute an external shell command (e.g. ssstatus)
 *   BLOCK_STATIC     - Display a fixed text string
 */
enum block_type {
	BLOCK_WORKSPACES,
	BLOCK_WORKSPACE = BLOCK_WORKSPACES, /* Alias */
	BLOCK_TITLE,
	BLOCK_COMMAND,
	BLOCK_STATIC,
};

struct BarBlock {
	enum block_type type;
	const char *command_or_text; /* Used for BLOCK_COMMAND, BLOCK_STATIC, or format/prefix for BLOCK_WORKSPACES */
	const char *custom_color;    /* Custom text color, or NULL to use color_fg */
	const char *prefix;          /* Text/markup before block (or NULL to use ws_prefix for workspaces) */
	const char *suffix;          /* Text/markup after block (or NULL to use ws_suffix for workspaces) */
};

/*
 * Configuration of the three bar sections:
 * Left:   Workspaces, with active workspace in brackets
 * Middle: Active window title
 * Right:  Output of the command "ssstatus"
 *
 * For BLOCK_WORKSPACES, text before and after the workspaces can be customized
 * either globally via ws_prefix and ws_suffix, or per-block via prefix and suffix:
 *   e.g. { BLOCK_WORKSPACES, NULL, NULL, "WS: ", " |" }
 * Pango markup is supported in prefix and suffix strings (e.g. "<span foreground='#89b4fa'>WS:</span> ").
 */
static const struct BarBlock left_block   = { BLOCK_WORKSPACES, NULL, NULL, NULL, "<span foreground='#50FA7B'>   [MinTwm]</span>"};
static const struct BarBlock middle_block = { BLOCK_TITLE,      NULL, NULL, NULL, NULL };
static const struct BarBlock right_block  = { BLOCK_COMMAND,    "ssstatus", NULL, NULL, NULL };

#endif /* BAR_CONFIG_H */
