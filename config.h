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
	BLOCK_TITLE,
	BLOCK_COMMAND,
	BLOCK_STATIC,
};

struct BarBlock {
	enum block_type type;
	const char *command_or_text; /* Used for BLOCK_COMMAND or BLOCK_STATIC */
	const char *custom_color;    /* Custom text color, or NULL to use color_fg */
};

/*
 * Configuration of the three bar sections:
 * Left:   Workspaces, with active workspace in brackets
 * Middle: Active window title
 * Right:  Output of the command "ssstatus"
 */
static const struct BarBlock left_block   = { BLOCK_WORKSPACES, NULL, NULL };
static const struct BarBlock middle_block = { BLOCK_TITLE,      NULL, NULL };
static const struct BarBlock right_block  = { BLOCK_COMMAND,    "ssstatus", NULL };

#endif /* BAR_CONFIG_H */
