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
static const char *ws_prefix       = "";   /* Default text before workspaces (e.g. "WS: " or "[") */
static const char *ws_prefix_color = NULL; /* Default color for ws_prefix (NULL = color_fg) */
static const char *ws_suffix       = "";   /* Default text after workspaces (e.g. " |" or "]") */
static const char *ws_suffix_color = NULL; /* Default color for ws_suffix (NULL = color_fg) */

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
	const char *command_or_text; /* Used for BLOCK_COMMAND, BLOCK_STATIC */
	const char *color;           /* Custom text color, or NULL to use color_fg */
	const char *prefix;          /* Text before block */
	const char *prefix_color;    /* Color for prefix (NULL to use color) */
	const char *suffix;          /* Text after block */
	const char *suffix_color;    /* Color for suffix (NULL to use color) */
};
#define custom_color color

/*
 * Configuration of the three bar sections:
 * Left:   Workspaces, with active workspace in brackets
 * Middle: Active window title
 * Right:  Output of the command "ssstatus"
 *
 * Each block can specify prefix, prefix_color, suffix, and suffix_color.
 * For minimal inline coloring in commands or text, suckless status2d syntax
 * is supported: ^#RRGGBB^text^d^ or ^c#RRGGBB^text^d^ (^d^ or ^^ resets color).
 */
static const struct BarBlock left_block   = { .type = BLOCK_WORKSPACES };
static const struct BarBlock middle_block = { .type = BLOCK_TITLE };
static const struct BarBlock right_block  = { .type = BLOCK_COMMAND, .command_or_text = "ssstatus" };

#endif /* BAR_CONFIG_H */
