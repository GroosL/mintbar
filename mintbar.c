#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo.h>
#include <pango/pangocairo.h>
#include <glib.h>

#include "config.h"

struct bar_buffer {
	struct wl_buffer *wl_buffer;
	void *shm_data;
	size_t size;
	int width;
	int height;
	bool busy;
};

struct mintbar {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_output *output;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	int width;
	int height;
	bool configured;

	struct bar_buffer buffers[2];

	char left_text[512];
	char middle_text[512];
	char right_text[512];

	int ipc_fd;
};

struct cmd_stream {
	pid_t pid;
	int fd;
	char line_buf[1024];
	size_t buf_pos;
};

static struct mintbar bar = {
	.ipc_fd = -1,
	.height = bar_height,
};

static struct cmd_stream right_stream = { .pid = -1, .fd = -1 };
static struct cmd_stream left_stream  = { .pid = -1, .fd = -1 };
static struct cmd_stream mid_stream   = { .pid = -1, .fd = -1 };

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig) {
	(void)sig;
	running = 0;
}

static void parse_color(const char *hex, double *r, double *g, double *b, double *a) {
	*r = *g = *b = 0.0;
	*a = 1.0;
	if (!hex) return;
	if (*hex == '#') hex++;
	size_t len = strlen(hex);
	unsigned int val = 0;
	if (len == 6) {
		if (sscanf(hex, "%06x", &val) == 1) {
			*r = ((val >> 16) & 0xff) / 255.0;
			*g = ((val >> 8) & 0xff) / 255.0;
			*b = (val & 0xff) / 255.0;
		}
	} else if (len == 8) {
		if (sscanf(hex, "%08x", &val) == 1) {
			*r = ((val >> 24) & 0xff) / 255.0;
			*g = ((val >> 16) & 0xff) / 255.0;
			*b = ((val >> 8) & 0xff) / 255.0;
			*a = (val & 0xff) / 255.0;
		}
	}
}

/* Command stream management */
static void close_cmd_stream(struct cmd_stream *s) {
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	if (s->pid > 0) {
		kill(s->pid, SIGTERM);
		waitpid(s->pid, NULL, WNOHANG);
		s->pid = -1;
	}
	s->buf_pos = 0;
}

static void start_cmd_stream(struct cmd_stream *s, const char *cmd) {
	if (!cmd || cmd[0] == '\0') return;

	if (s->fd >= 0) {
		int status;
		pid_t r = waitpid(s->pid, &status, WNOHANG);
		if (r == 0) {
			return; /* Still running */
		}
		close_cmd_stream(s);
	}

	int pfd[2];
	if (pipe(pfd) < 0) return;

	pid_t pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);

		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(1);
	} else if (pid > 0) {
		close(pfd[1]);
		int flags = fcntl(pfd[0], F_GETFL, 0);
		fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
		s->fd = pfd[0];
		s->pid = pid;
		s->buf_pos = 0;
	} else {
		close(pfd[0]);
		close(pfd[1]);
	}
}

static bool read_cmd_stream(struct cmd_stream *s, char *out, size_t out_size) {
	if (s->fd < 0) return false;

	char tmp[1024];
	ssize_t n;
	bool got_line = false;

	while ((n = read(s->fd, tmp, sizeof(tmp))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = tmp[i];
			if (c == '\n' || c == '\r') {
				if (s->buf_pos > 0) {
					s->line_buf[s->buf_pos] = '\0';
					strncpy(out, s->line_buf, out_size - 1);
					out[out_size - 1] = '\0';
					s->buf_pos = 0;
					got_line = true;
				}
			} else {
				if (s->buf_pos < sizeof(s->line_buf) - 1) {
					s->line_buf[s->buf_pos++] = c;
				}
			}
		}
	}

	if (n == 0) {
		if (s->buf_pos > 0) {
			s->line_buf[s->buf_pos] = '\0';
			strncpy(out, s->line_buf, out_size - 1);
			out[out_size - 1] = '\0';
			s->buf_pos = 0;
			got_line = true;
		}
		close_cmd_stream(s);
	}

	return got_line;
}

/* IPC helper */
static int bar_ipc_connect(void) {
	if (bar.ipc_fd >= 0) {
		return bar.ipc_fd;
	}

	const char *sock_path = getenv("MINT_IPC_SOCKET");
	if (!sock_path || sock_path[0] == '\0') {
		sock_path = getenv("TINYWL_IPC_SOCKET");
	}
	char default_path[108];
	if (!sock_path || sock_path[0] == '\0') {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (!rt) rt = "/tmp";
		snprintf(default_path, sizeof(default_path), "%s/mint-ipc.sock", rt);
		sock_path = default_path;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	if (write(fd, "subscribe\n", 10) != 10) {
		close(fd);
		return -1;
	}

	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	bar.ipc_fd = fd;
	return bar.ipc_fd;
}

static void update_state_blocks(unsigned int ws, const char *title, bool *changed) {
	char ws_str[128] = "";
	char *p = ws_str;
	for (unsigned int i = 1; i <= 9; i++) {
		if (i == ws) {
			p += snprintf(p, sizeof(ws_str) - (p - ws_str), "[%u] ", i);
		} else {
			p += snprintf(p, sizeof(ws_str) - (p - ws_str), "%u ", i);
		}
	}
	if (p > ws_str && *(p - 1) == ' ') *(p - 1) = '\0';

	if (left_block.type == BLOCK_WORKSPACES && strcmp(bar.left_text, ws_str) != 0) {
		strcpy(bar.left_text, ws_str);
		*changed = true;
	}
	if (middle_block.type == BLOCK_WORKSPACES && strcmp(bar.middle_text, ws_str) != 0) {
		strcpy(bar.middle_text, ws_str);
		*changed = true;
	}
	if (right_block.type == BLOCK_WORKSPACES && strcmp(bar.right_text, ws_str) != 0) {
		strcpy(bar.right_text, ws_str);
		*changed = true;
	}
	if (left_block.type == BLOCK_TITLE && strcmp(bar.left_text, title) != 0) {
		strncpy(bar.left_text, title, sizeof(bar.left_text) - 1);
		bar.left_text[sizeof(bar.left_text) - 1] = '\0';
		*changed = true;
	}
	if (middle_block.type == BLOCK_TITLE && strcmp(bar.middle_text, title) != 0) {
		strncpy(bar.middle_text, title, sizeof(bar.middle_text) - 1);
		bar.middle_text[sizeof(bar.middle_text) - 1] = '\0';
		*changed = true;
	}
	if (right_block.type == BLOCK_TITLE && strcmp(bar.right_text, title) != 0) {
		strncpy(bar.right_text, title, sizeof(bar.right_text) - 1);
		bar.right_text[sizeof(bar.right_text) - 1] = '\0';
		*changed = true;
	}
}

static char ipc_buf[512];
static size_t ipc_buf_len = 0;

static void read_ipc_events(bool *changed) {
	if (bar.ipc_fd < 0) return;

	char tmp[512];
	ssize_t n;
	while ((n = read(bar.ipc_fd, tmp, sizeof(tmp))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = tmp[i];
			if (c == '\n' || c == '\r') {
				ipc_buf[ipc_buf_len] = '\0';
				if (strncmp(ipc_buf, "STATE ", 6) == 0) {
					char *ptr = ipc_buf + 6;
					while (*ptr == ' ') ptr++;
					unsigned int ws = (unsigned int)atoi(ptr);
					while (*ptr >= '0' && *ptr <= '9') ptr++;
					while (*ptr == ' ') ptr++;
					update_state_blocks(ws, ptr, changed);
				}
				ipc_buf_len = 0;
			} else if (ipc_buf_len < sizeof(ipc_buf) - 1) {
				ipc_buf[ipc_buf_len++] = c;
			}
		}
	}

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		close(bar.ipc_fd);
		bar.ipc_fd = -1;
		ipc_buf_len = 0;
	}
}

/* SHM buffer creation */
static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
	(void)wl_buffer;
	struct bar_buffer *buf = data;
	buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static int create_shm_file(off_t size) {
	int fd = memfd_create("mintbar-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		char template[] = "/tmp/mintbar-shm-XXXXXX";
		fd = mkstemp(template);
		if (fd >= 0) {
			unlink(template);
		}
	}
	if (fd < 0) return -1;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct bar_buffer *get_next_buffer(int width, int height) {
	int stride = width * 4;
	size_t size = stride * height;

	for (int i = 0; i < 2; i++) {
		struct bar_buffer *buf = &bar.buffers[i];
		if (!buf->busy) {
			if (buf->width != width || buf->height != height) {
				if (buf->wl_buffer) {
					wl_buffer_destroy(buf->wl_buffer);
					buf->wl_buffer = NULL;
				}
				if (buf->shm_data) {
					munmap(buf->shm_data, buf->size);
					buf->shm_data = NULL;
				}

				int fd = create_shm_file(size);
				if (fd < 0) return NULL;

				buf->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				if (buf->shm_data == MAP_FAILED) {
					close(fd);
					return NULL;
				}

				struct wl_shm_pool *pool = wl_shm_create_pool(bar.shm, fd, size);
				buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
				wl_shm_pool_destroy(pool);
				close(fd);

				wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, buf);
				buf->size = size;
				buf->width = width;
				buf->height = height;
			}
			buf->busy = true;
			return buf;
		}
	}
	return NULL;
}

static void get_block_prefix_suffix(const struct BarBlock *blk,
                                    const char **out_prefix,
                                    const char **out_suffix,
                                    char *buf_pfx, size_t buf_pfx_size,
                                    char *buf_sfx, size_t buf_sfx_size) {
	*out_prefix = NULL;
	*out_suffix = NULL;

	if (blk->type == BLOCK_WORKSPACES) {
		*out_prefix = ws_prefix;
		*out_suffix = ws_suffix;
	}

	if (blk->command_or_text && blk->type == BLOCK_WORKSPACES) {
		const char *fmt = strstr(blk->command_or_text, "%s");
		if (fmt) {
			size_t pfx_len = (size_t)(fmt - blk->command_or_text);
			if (pfx_len >= buf_pfx_size) pfx_len = buf_pfx_size - 1;
			strncpy(buf_pfx, blk->command_or_text, pfx_len);
			buf_pfx[pfx_len] = '\0';
			*out_prefix = buf_pfx;

			strncpy(buf_sfx, fmt + 2, buf_sfx_size - 1);
			buf_sfx[buf_sfx_size - 1] = '\0';
			*out_suffix = buf_sfx;
		} else {
			*out_prefix = blk->command_or_text;
		}
	}

	if (blk->prefix) {
		*out_prefix = blk->prefix;
	}
	if (blk->suffix) {
		*out_suffix = blk->suffix;
	}
}

static void append_markup_segment(char *out, size_t out_size, const char *str, const char *color) {
	if (!str || str[0] == '\0') return;

	char *to_insert = NULL;
	if (pango_parse_markup(str, -1, 0, NULL, NULL, NULL, NULL)) {
		to_insert = g_strdup(str);
	} else {
		to_insert = g_markup_escape_text(str, -1);
	}

	char *wrapped = g_strdup_printf("<span foreground=\"%s\">%s</span>",
		color ? color : color_fg, to_insert);
	g_free(to_insert);

	size_t cur_len = strlen(out);
	if (cur_len < out_size - 1) {
		strncat(out, wrapped, out_size - cur_len - 1);
	}
	g_free(wrapped);
}

/* Workspaces markup formatter: highlights active workspace inside [] and applies prefix/suffix */
static void format_workspaces_markup(const struct BarBlock *blk, const char *raw, char *out, size_t out_size) {
	out[0] = '\0';
	if (!raw || raw[0] == '\0') return;

	char pfx_buf[256] = "";
	char sfx_buf[256] = "";
	const char *prefix = NULL;
	const char *suffix = NULL;
	get_block_prefix_suffix(blk, &prefix, &suffix, pfx_buf, sizeof(pfx_buf), sfx_buf, sizeof(sfx_buf));
	const char *fg_color = (blk && blk->custom_color) ? blk->custom_color : color_fg;

	if (prefix && prefix[0] != '\0') {
		append_markup_segment(out, out_size, prefix, fg_color);
	}

	char copy[512];
	strncpy(copy, raw, sizeof(copy) - 1);
	copy[sizeof(copy) - 1] = '\0';

	char *saveptr = NULL;
	char *token = strtok_r(copy, " ", &saveptr);
	bool first = true;

	while (token) {
		char part[256];
		if (!first) {
			strncat(out, "  ", out_size - strlen(out) - 1);
		}
		first = false;

		if (token[0] == '[' && token[strlen(token) - 1] == ']') {
			snprintf(part, sizeof(part), "<span foreground=\"%s\" weight=\"bold\">%s</span>",
				color_active_ws, token);
		} else {
			snprintf(part, sizeof(part), "<span foreground=\"%s\">%s</span>",
				color_inactive_ws, token);
		}
		strncat(out, part, out_size - strlen(out) - 1);
		token = strtok_r(NULL, " ", &saveptr);
	}

	if (suffix && suffix[0] != '\0') {
		append_markup_segment(out, out_size, suffix, fg_color);
	}
}

static void setup_block_layout(PangoLayout *layout, const struct BarBlock *blk,
                               const char *raw_text, cairo_t *cr,
                               double default_fg_r, double default_fg_g,
                               double default_fg_b, double default_fg_a) {
	if (blk->custom_color) {
		double r, g, b, a;
		parse_color(blk->custom_color, &r, &g, &b, &a);
		cairo_set_source_rgba(cr, r, g, b, a);
	} else {
		cairo_set_source_rgba(cr, default_fg_r, default_fg_g, default_fg_b, default_fg_a);
	}

	if (blk->type == BLOCK_WORKSPACES) {
		char markup[2048];
		format_workspaces_markup(blk, raw_text, markup, sizeof(markup));
		pango_layout_set_markup(layout, markup, -1);
	} else {
		char pfx_buf[256] = "";
		char sfx_buf[256] = "";
		const char *prefix = NULL;
		const char *suffix = NULL;
		get_block_prefix_suffix(blk, &prefix, &suffix, pfx_buf, sizeof(pfx_buf), sfx_buf, sizeof(sfx_buf));

		if ((prefix && prefix[0] != '\0') || (suffix && suffix[0] != '\0')) {
			char buf[1024];
			snprintf(buf, sizeof(buf), "%s%s%s",
				prefix ? prefix : "",
				raw_text ? raw_text : "",
				suffix ? suffix : "");
			pango_layout_set_text(layout, buf, -1);
		} else {
			pango_layout_set_text(layout, raw_text ? raw_text : "", -1);
		}
	}
}

/* Rendering */
static void render_bar(void) {
	if (!bar.configured || bar.width <= 0 || bar.height <= 0) {
		return;
	}

	struct bar_buffer *buf = get_next_buffer(bar.width, bar.height);
	if (!buf) return;

	cairo_surface_t *cairo_surf = cairo_image_surface_create_for_data(
		buf->shm_data, CAIRO_FORMAT_ARGB32, bar.width, bar.height, bar.width * 4);
	cairo_t *cr = cairo_create(cairo_surf);

	/* Clear background */
	double bg_r, bg_g, bg_b, bg_a;
	parse_color(color_bg, &bg_r, &bg_g, &bg_b, &bg_a);
	cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
	cairo_paint(cr);

	/* Default text color */
	double fg_r, fg_g, fg_b, fg_a;
	parse_color(color_fg, &fg_r, &fg_g, &fg_b, &fg_a);

	PangoFontDescription *font_desc = pango_font_description_from_string(font_name);

	int left_w = 0, left_h = 0;
	int right_w = 0, right_h = 0;
	int mid_w = 0, mid_h = 0;

	/* 1. Render Left Block with dedicated layout */
	PangoLayout *layout_left = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout_left, font_desc);
	setup_block_layout(layout_left, &left_block, bar.left_text, cr, fg_r, fg_g, fg_b, fg_a);
	pango_layout_get_pixel_size(layout_left, &left_w, &left_h);
	cairo_move_to(cr, padding_x, (bar.height - left_h) / 2);
	pango_cairo_show_layout(cr, layout_left);
	g_object_unref(layout_left);

	/* 2. Render Right Block with dedicated layout */
	PangoLayout *layout_right = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout_right, font_desc);
	setup_block_layout(layout_right, &right_block, bar.right_text, cr, fg_r, fg_g, fg_b, fg_a);
	pango_layout_get_pixel_size(layout_right, &right_w, &right_h);

	int right_x = bar.width - right_w - padding_x;
	if (right_x < padding_x + left_w + 20) {
		right_x = padding_x + left_w + 20;
	}
	cairo_move_to(cr, right_x, (bar.height - right_h) / 2);
	pango_cairo_show_layout(cr, layout_right);
	g_object_unref(layout_right);

	/* 3. Render Middle Block with dedicated layout (centered, ellipsized if needed) */
	PangoLayout *layout_mid = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout_mid, font_desc);
	setup_block_layout(layout_mid, &middle_block, bar.middle_text, cr, fg_r, fg_g, fg_b, fg_a);
	pango_layout_set_ellipsize(layout_mid, PANGO_ELLIPSIZE_END);

	int max_mid_w = right_x - (padding_x + left_w + 20);
	if (max_mid_w > 0) {
		pango_layout_set_width(layout_mid, max_mid_w * PANGO_SCALE);
		pango_layout_get_pixel_size(layout_mid, &mid_w, &mid_h);

		int mid_x = (bar.width - mid_w) / 2;
		if (mid_x < padding_x + left_w + 10) {
			mid_x = padding_x + left_w + 10;
		}
		cairo_move_to(cr, mid_x, (bar.height - mid_h) / 2);
		pango_cairo_show_layout(cr, layout_mid);
	}
	g_object_unref(layout_mid);

	pango_font_description_free(font_desc);
	cairo_destroy(cr);
	cairo_surface_destroy(cairo_surf);

	wl_surface_attach(bar.surface, buf->wl_buffer, 0, 0);
	wl_surface_damage_buffer(bar.surface, 0, 0, bar.width, bar.height);
	wl_surface_commit(bar.surface);
	wl_display_flush(bar.display);
}

static void update_block_immediate(const struct BarBlock *blk, char *dst, size_t dst_size) {
	switch (blk->type) {
	case BLOCK_WORKSPACES:
	case BLOCK_TITLE:
		/* Populated via IPC push subscription */
		break;
	case BLOCK_STATIC:
		if (blk->command_or_text) {
			strncpy(dst, blk->command_or_text, dst_size - 1);
			dst[dst_size - 1] = '\0';
		} else {
			dst[0] = '\0';
		}
		break;
	case BLOCK_COMMAND:
		break;
	}
}

/* Layer surface listeners */
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	(void)data;
	if (width > 0) {
		bar.width = width;
	}
	if (height > 0) {
		bar.height = height;
	}
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	bar.configured = true;
	render_bar();
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	(void)data;
	(void)surface;
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/* Registry listeners */
static void registry_global(void *data, struct wl_registry *registry,
		uint32_t id, const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		bar.compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		bar.shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		bar.layer_shell = wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (!bar.output) {
			bar.output = wl_registry_bind(registry, id, &wl_output_interface, 1);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
	(void)data;
	(void)registry;
	(void)id;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	bar.display = wl_display_connect(NULL);
	if (!bar.display) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		return 1;
	}

	bar.registry = wl_display_get_registry(bar.display);
	wl_registry_add_listener(bar.registry, &registry_listener, NULL);
	wl_display_roundtrip(bar.display);

	if (!bar.compositor || !bar.shm || !bar.layer_shell) {
		fprintf(stderr, "Missing required Wayland interfaces (compositor, shm, or layer_shell)\n");
		wl_display_disconnect(bar.display);
		return 1;
	}

	bar.surface = wl_compositor_create_surface(bar.compositor);
	bar.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		bar.layer_shell, bar.surface, bar.output,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "mintbar");

	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	anchor |= bar_bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;

	zwlr_layer_surface_v1_set_anchor(bar.layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(bar.layer_surface, 0, bar.height);
	zwlr_layer_surface_v1_set_exclusive_zone(bar.layer_surface, bar.height);

	zwlr_layer_surface_v1_add_listener(bar.layer_surface, &layer_surface_listener, NULL);
	wl_surface_commit(bar.surface);
	wl_display_roundtrip(bar.display);

	int wl_fd = wl_display_get_fd(bar.display);

	struct timespec last_status;
	clock_gettime(CLOCK_MONOTONIC, &last_status);

	/* Start initial command streams */
	if (right_block.type == BLOCK_COMMAND) {
		start_cmd_stream(&right_stream, right_block.command_or_text);
	}
	if (left_block.type == BLOCK_COMMAND) {
		start_cmd_stream(&left_stream, left_block.command_or_text);
	}
	if (middle_block.type == BLOCK_COMMAND) {
		start_cmd_stream(&mid_stream, middle_block.command_or_text);
	}

	/* Connect to IPC and subscribe to events */
	bar_ipc_connect();
	bool changed = false;
	read_ipc_events(&changed);

	/* Initial data fetch */
	update_block_immediate(&left_block, bar.left_text, sizeof(bar.left_text));
	update_block_immediate(&middle_block, bar.middle_text, sizeof(bar.middle_text));
	update_block_immediate(&right_block, bar.right_text, sizeof(bar.right_text));
	render_bar();

	while (running) {
		while (wl_display_prepare_read(bar.display) != 0) {
			wl_display_dispatch_pending(bar.display);
		}
		wl_display_flush(bar.display);

		/* Reconnect IPC if disconnected */
		if (bar.ipc_fd < 0) {
			bar_ipc_connect();
		}

		struct pollfd fds[5];
		int nfds = 0;

		fds[nfds].fd = wl_fd;
		fds[nfds].events = POLLIN;
		int wl_idx = nfds++;

		int ipc_idx = -1;
		if (bar.ipc_fd >= 0) {
			fds[nfds].fd = bar.ipc_fd;
			fds[nfds].events = POLLIN;
			ipc_idx = nfds++;
		}

		int r_idx = -1;
		if (right_stream.fd >= 0) {
			fds[nfds].fd = right_stream.fd;
			fds[nfds].events = POLLIN;
			r_idx = nfds++;
		}
		int l_idx = -1;
		if (left_stream.fd >= 0) {
			fds[nfds].fd = left_stream.fd;
			fds[nfds].events = POLLIN;
			l_idx = nfds++;
		}
		int m_idx = -1;
		if (mid_stream.fd >= 0) {
			fds[nfds].fd = mid_stream.fd;
			fds[nfds].events = POLLIN;
			m_idx = nfds++;
		}

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		long elapsed_status_ms = (now.tv_sec - last_status.tv_sec) * 1000 +
			(now.tv_nsec - last_status.tv_nsec) / 1000000;
		int timeout = status_interval_ms - (int)elapsed_status_ms;
		if (timeout < 0) timeout = 0;

		int ret = poll(fds, nfds, timeout);

		if (ret > 0 && (fds[wl_idx].revents & POLLIN)) {
			wl_display_read_events(bar.display);
		} else {
			wl_display_cancel_read(bar.display);
		}

		wl_display_dispatch_pending(bar.display);

		changed = false;

		/* Read from IPC (instant workspace and title updates) */
		if (ipc_idx >= 0 && (fds[ipc_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
			read_ipc_events(&changed);
		}

		/* Read from command streams if output ready */
		if (r_idx >= 0 && (fds[r_idx].revents & (POLLIN | POLLHUP))) {
			char line[512];
			if (read_cmd_stream(&right_stream, line, sizeof(line))) {
				if (strcmp(line, bar.right_text) != 0) {
					strcpy(bar.right_text, line);
					changed = true;
				}
			}
		}
		if (l_idx >= 0 && (fds[l_idx].revents & (POLLIN | POLLHUP))) {
			char line[512];
			if (read_cmd_stream(&left_stream, line, sizeof(line))) {
				if (strcmp(line, bar.left_text) != 0) {
					strcpy(bar.left_text, line);
					changed = true;
				}
			}
		}
		if (m_idx >= 0 && (fds[m_idx].revents & (POLLIN | POLLHUP))) {
			char line[512];
			if (read_cmd_stream(&mid_stream, line, sizeof(line))) {
				if (strcmp(line, bar.middle_text) != 0) {
					strcpy(bar.middle_text, line);
					changed = true;
				}
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_status_ms = (now.tv_sec - last_status.tv_sec) * 1000 +
			(now.tv_nsec - last_status.tv_nsec) / 1000000;

		/* Check/restart command streams if needed */
		if (elapsed_status_ms >= status_interval_ms) {
			last_status = now;

			if (right_block.type == BLOCK_COMMAND) {
				start_cmd_stream(&right_stream, right_block.command_or_text);
			}
			if (left_block.type == BLOCK_COMMAND) {
				start_cmd_stream(&left_stream, left_block.command_or_text);
			}
			if (middle_block.type == BLOCK_COMMAND) {
				start_cmd_stream(&mid_stream, middle_block.command_or_text);
			}
		}

		if (changed) {
			render_bar();
		}
	}

	close_cmd_stream(&right_stream);
	close_cmd_stream(&left_stream);
	close_cmd_stream(&mid_stream);

	if (bar.ipc_fd >= 0) {
		close(bar.ipc_fd);
	}
	if (bar.layer_surface) {
		zwlr_layer_surface_v1_destroy(bar.layer_surface);
	}
	if (bar.surface) {
		wl_surface_destroy(bar.surface);
	}
	for (int i = 0; i < 2; i++) {
		if (bar.buffers[i].wl_buffer) {
			wl_buffer_destroy(bar.buffers[i].wl_buffer);
		}
		if (bar.buffers[i].shm_data) {
			munmap(bar.buffers[i].shm_data, bar.buffers[i].size);
		}
	}
	wl_display_disconnect(bar.display);
	return 0;
}
