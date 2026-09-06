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
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include "config.h"

struct bar_buffer {
	struct wl_buffer *wl_buffer;
	void *shm_data;
	size_t size;
	int width;
	int height;
	bool busy;
};

#define MAX_FACES 4

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

	FT_Library ft;
	FT_Face faces[MAX_FACES];
	char *face_files[MAX_FACES];
	int num_faces;
	int pixel_size;
	bool dirty;
};

struct theme_colors {
	uint32_t bg;
	uint32_t fg;
	uint32_t active_ws;
	uint32_t inactive_ws;
	uint32_t left_fg;
	uint32_t left_pfx;
	uint32_t left_sfx;
	uint32_t mid_fg;
	uint32_t mid_pfx;
	uint32_t mid_sfx;
	uint32_t right_fg;
	uint32_t right_pfx;
	uint32_t right_sfx;
};

static struct theme_colors theme;

struct cmd_stream {
	pid_t pid;
	int fd;
	char line_buf[1024];
	size_t buf_pos;
};

static struct mintbar bar = {
	.ipc_fd = -1,
	.height = bar_height,
	.dirty = true,
};

static struct cmd_stream right_stream = { .pid = -1, .fd = -1 };
static struct cmd_stream left_stream  = { .pid = -1, .fd = -1 };
static struct cmd_stream mid_stream   = { .pid = -1, .fd = -1 };

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig) {
	(void)sig;
	running = 0;
}

static uint32_t parse_color_u32(const char *hex) {
	if (!hex) return 0xFFFFFFFF;
	if (*hex == '#') hex++;
	size_t len = strlen(hex);
	unsigned int val = 0;
	if (len == 6) {
		if (sscanf(hex, "%06x", &val) == 1) {
			return 0xFF000000 | val;
		}
	} else if (len == 8) {
		if (sscanf(hex, "%08x", &val) == 1) {
			uint32_t r = (val >> 24) & 0xff;
			uint32_t g = (val >> 16) & 0xff;
			uint32_t b = (val >> 8) & 0xff;
			uint32_t a = val & 0xff;
			return (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	return 0xFFFFFFFF;
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
		bar.dirty = true;
	}
	if (middle_block.type == BLOCK_WORKSPACES && strcmp(bar.middle_text, ws_str) != 0) {
		strcpy(bar.middle_text, ws_str);
		*changed = true;
		bar.dirty = true;
	}
	if (right_block.type == BLOCK_WORKSPACES && strcmp(bar.right_text, ws_str) != 0) {
		strcpy(bar.right_text, ws_str);
		*changed = true;
		bar.dirty = true;
	}
	if (left_block.type == BLOCK_TITLE && strcmp(bar.left_text, title) != 0) {
		strncpy(bar.left_text, title, sizeof(bar.left_text) - 1);
		bar.left_text[sizeof(bar.left_text) - 1] = '\0';
		*changed = true;
		bar.dirty = true;
	}
	if (middle_block.type == BLOCK_TITLE && strcmp(bar.middle_text, title) != 0) {
		strncpy(bar.middle_text, title, sizeof(bar.middle_text) - 1);
		bar.middle_text[sizeof(bar.middle_text) - 1] = '\0';
		*changed = true;
		bar.dirty = true;
	}
	if (right_block.type == BLOCK_TITLE && strcmp(bar.right_text, title) != 0) {
		strncpy(bar.right_text, title, sizeof(bar.right_text) - 1);
		bar.right_text[sizeof(bar.right_text) - 1] = '\0';
		*changed = true;
		bar.dirty = true;
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
                                    const char **out_pfx_color,
                                    const char **out_sfx_color,
                                    char *buf_pfx, size_t buf_pfx_size,
                                    char *buf_sfx, size_t buf_sfx_size) {
	*out_prefix = NULL;
	*out_suffix = NULL;
	*out_pfx_color = NULL;
	*out_sfx_color = NULL;

	if (blk->type == BLOCK_WORKSPACES) {
		*out_prefix = ws_prefix;
		*out_suffix = ws_suffix;
		*out_pfx_color = ws_prefix_color;
		*out_sfx_color = ws_suffix_color;
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
	if (blk->prefix_color) {
		*out_pfx_color = blk->prefix_color;
	}
	if (blk->suffix) {
		*out_suffix = blk->suffix;
	}
	if (blk->suffix_color) {
		*out_sfx_color = blk->suffix_color;
	}
}

static void init_theme(void) {
	theme.bg = parse_color_u32(color_bg);
	theme.fg = parse_color_u32(color_fg);
	theme.active_ws = parse_color_u32(color_active_ws);
	theme.inactive_ws = parse_color_u32(color_inactive_ws);

	const char *pfx = NULL, *sfx = NULL, *pcol = NULL, *scol = NULL;
	char bp[64], bs[64];

	get_block_prefix_suffix(&left_block, &pfx, &sfx, &pcol, &scol, bp, sizeof(bp), bs, sizeof(bs));
	theme.left_fg = parse_color_u32(left_block.color ? left_block.color : color_fg);
	theme.left_pfx = parse_color_u32(pcol ? pcol : (left_block.color ? left_block.color : color_fg));
	theme.left_sfx = parse_color_u32(scol ? scol : (left_block.color ? left_block.color : color_fg));

	get_block_prefix_suffix(&middle_block, &pfx, &sfx, &pcol, &scol, bp, sizeof(bp), bs, sizeof(bs));
	theme.mid_fg = parse_color_u32(middle_block.color ? middle_block.color : color_fg);
	theme.mid_pfx = parse_color_u32(pcol ? pcol : (middle_block.color ? middle_block.color : color_fg));
	theme.mid_sfx = parse_color_u32(scol ? scol : (middle_block.color ? middle_block.color : color_fg));

	get_block_prefix_suffix(&right_block, &pfx, &sfx, &pcol, &scol, bp, sizeof(bp), bs, sizeof(bs));
	theme.right_fg = parse_color_u32(right_block.color ? right_block.color : color_fg);
	theme.right_pfx = parse_color_u32(pcol ? pcol : (right_block.color ? right_block.color : color_fg));
	theme.right_sfx = parse_color_u32(scol ? scol : (right_block.color ? right_block.color : color_fg));
}

static uint32_t utf8_decode(const char **ptr) {
	const unsigned char *s = (const unsigned char *)*ptr;
	if (!*s) return 0;
	uint32_t cp = 0;
	if (*s < 0x80) {
		cp = *s++;
	} else if ((*s & 0xe0) == 0xc0) {
		cp = (*s++ & 0x1f) << 6;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f);
	} else if ((*s & 0xf0) == 0xe0) {
		cp = (*s++ & 0x0f) << 12;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f) << 6;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f);
	} else if ((*s & 0xf8) == 0xf0) {
		cp = (*s++ & 0x07) << 18;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f) << 12;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f) << 6;
		if ((*s & 0xc0) == 0x80) cp |= (*s++ & 0x3f);
	} else {
		s++;
	}
	*ptr = (const char *)s;
	return cp;
}

static void add_face(FT_Library ft, const char *pattern_str, int pixel_size) {
	if (bar.num_faces >= MAX_FACES) return;
	FcPattern *pat = FcNameParse((const FcChar8 *)pattern_str);
	if (!pat) pat = FcPatternCreate();
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult res;
	FcPattern *match = FcFontMatch(NULL, pat, &res);
	if (!match) {
		FcPatternDestroy(pat);
		return;
	}
	FcChar8 *file = NULL;
	int index = 0;
	FcPatternGetString(match, FC_FILE, 0, &file);
	FcPatternGetInteger(match, FC_INDEX, 0, &index);

	if (file) {
		for (int i = 0; i < bar.num_faces; i++) {
			if (bar.face_files[i] && strcmp(bar.face_files[i], (const char *)file) == 0) {
				FcPatternDestroy(match);
				FcPatternDestroy(pat);
				return;
			}
		}

		FT_Face face;
		if (FT_New_Face(ft, (const char *)file, index, &face) == 0) {
			if (face->num_fixed_sizes > 0) {
				FT_Select_Size(face, 0);
			} else {
				FT_Set_Pixel_Sizes(face, 0, pixel_size);
			}
			bar.face_files[bar.num_faces] = strdup((const char *)file);
			bar.faces[bar.num_faces++] = face;
		}
	}
	FcPatternDestroy(match);
	FcPatternDestroy(pat);
}

static FT_Face get_face_for_char(uint32_t cp) {
	/* Ignore Unicode variation selectors and non-printable zero-width formatting characters */
	if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D || cp == 0x200B)
		return NULL;

	/* 1. Check already loaded faces */
	for (int i = 0; i < bar.num_faces; i++) {
		if (FT_Get_Char_Index(bar.faces[i], cp) != 0)
			return bar.faces[i];
	}

	/* 2. Dynamically query Fontconfig for a system font containing this codepoint */
	if (bar.num_faces < MAX_FACES) {
		FcCharSet *cs = FcCharSetCreate();
		if (cs) {
			FcCharSetAddChar(cs, cp);
			FcPattern *pat = FcPatternCreate();
			if (pat) {
				FcPatternAddCharSet(pat, FC_CHARSET, cs);
				FcConfigSubstitute(NULL, pat, FcMatchPattern);
				FcDefaultSubstitute(pat);
				FcResult res;
				FcPattern *match = FcFontMatch(NULL, pat, &res);
				if (match) {
					FcChar8 *file = NULL;
					int index = 0;
					FcPatternGetString(match, FC_FILE, 0, &file);
					FcPatternGetInteger(match, FC_INDEX, 0, &index);
					if (file) {
						bool exists = false;
						for (int i = 0; i < bar.num_faces; i++) {
							if (bar.face_files[i] && strcmp(bar.face_files[i], (const char *)file) == 0) {
								exists = true;
								break;
							}
						}
						if (!exists) {
							FT_Face face;
							if (FT_New_Face(bar.ft, (const char *)file, index, &face) == 0) {
								if (face->num_fixed_sizes > 0) {
									FT_Select_Size(face, 0);
								} else {
									FT_Set_Pixel_Sizes(face, 0, bar.pixel_size);
								}
								bar.face_files[bar.num_faces] = strdup((const char *)file);
								bar.faces[bar.num_faces++] = face;
								FcPatternDestroy(match);
								FcPatternDestroy(pat);
								FcCharSetDestroy(cs);
								if (FT_Get_Char_Index(face, cp) != 0)
									return face;
								return NULL;
							}
						}
					}
					FcPatternDestroy(match);
				}
				FcPatternDestroy(pat);
			}
			FcCharSetDestroy(cs);
		}
	}

	return NULL;
}

static bool init_font(const char *name) {
	if (!FcInit()) {
		fprintf(stderr, "Failed to initialize fontconfig\n");
		return false;
	}
	if (FT_Init_FreeType(&bar.ft)) {
		fprintf(stderr, "Failed to initialize FreeType\n");
		return false;
	}

	char buf[256];
	strncpy(buf, name, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	double req_size = -1.0;
	char *last_space = strrchr(buf, ' ');
	if (last_space && *(last_space + 1) >= '0' && *(last_space + 1) <= '9') {
		req_size = atof(last_space + 1);
		*last_space = '-';
	}
	if (req_size <= 0.0) req_size = 10.0;
	int pixel_size = (int)(req_size * 96.0 / 72.0 + 0.5);
	if (pixel_size <= 0) pixel_size = 13;

	bar.num_faces = 0;
	bar.pixel_size = pixel_size;
	add_face(bar.ft, buf, pixel_size);
	add_face(bar.ft, "emoji", pixel_size);

	return bar.num_faces > 0;
}

static void cleanup_font(void) {
	for (int i = 0; i < bar.num_faces; i++) {
		if (bar.faces[i]) {
			FT_Done_Face(bar.faces[i]);
			bar.faces[i] = NULL;
		}
		if (bar.face_files[i]) {
			free(bar.face_files[i]);
			bar.face_files[i] = NULL;
		}
	}
	bar.num_faces = 0;
	if (bar.ft) {
		FT_Done_FreeType(bar.ft);
		bar.ft = NULL;
	}
	FcFini();
}

/* Minimal inline color parser (suckless status2d: ^#RRGGBB^ or ^c#RRGGBB^, reset with ^d^ or ^^) */
static const char *parse_inline_color(const char *p, uint32_t base_color, uint32_t *out_color) {
	if (*p != '^') return NULL;
	if (p[1] == 'd' && p[2] == '^') {
		*out_color = base_color;
		return p + 3;
	}
	if (p[1] == '^') {
		*out_color = base_color;
		return p + 2;
	}
	const char *end = strchr(p + 1, '^');
	if (end) {
		const char *hex = p + 1;
		if (*hex == 'c') hex++;
		if (*hex == '#') {
			char col_buf[16];
			size_t len = (size_t)(end - hex);
			if (len < sizeof(col_buf)) {
				memcpy(col_buf, hex, len);
				col_buf[len] = '\0';
				*out_color = parse_color_u32(col_buf);
				return end + 1;
			}
		}
	}
	return NULL;
}

static int measure_text_width(FT_Face primary_face, const char *text) {
	if (!text) return 0;
	int width = 0;
	const char *p = text;
	int font_h = (primary_face->size->metrics.ascender - primary_face->size->metrics.descender) >> 6;
	if (font_h <= 0) font_h = 16;

	while (*p) {
		if (*p == '^') {
			uint32_t dummy = 0;
			const char *next = parse_inline_color(p, 0, &dummy);
			if (next) {
				p = next;
				continue;
			}
		}
		uint32_t cp = utf8_decode(&p);
		if (!cp) break;

		FT_Face face = get_face_for_char(cp);
		if (!face) continue;

		if (face->num_fixed_sizes > 0 || (face->face_flags & FT_FACE_FLAG_COLOR)) {
			width += font_h + 1;
		} else if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0) {
			width += (int)(face->glyph->advance.x >> 6);
		}
	}
	return width;
}

static int draw_text(struct bar_buffer *buf, FT_Face primary_face,
                     const char *text, int x, int baseline_y, uint32_t base_color, int max_w) {
	if (!text || text[0] == '\0' || max_w == 0) return x;

	uint32_t *pixels = (uint32_t *)buf->shm_data;
	int buf_w = buf->width;
	int buf_h = buf->height;

	uint32_t color = base_color;
	uint8_t cr = (color >> 16) & 0xff;
	uint8_t cg = (color >> 8) & 0xff;
	uint8_t cb = color & 0xff;

	int start_x = x;
	const char *p = text;

	int ascender = primary_face->size->metrics.ascender >> 6;
	int descender = primary_face->size->metrics.descender >> 6;
	int font_h = ascender - descender;
	if (font_h <= 0) font_h = 16;

	while (*p) {
		if (*p == '^') {
			uint32_t new_col = color;
			const char *next = parse_inline_color(p, base_color, &new_col);
			if (next) {
				color = new_col;
				cr = (color >> 16) & 0xff;
				cg = (color >> 8) & 0xff;
				cb = color & 0xff;
				p = next;
				continue;
			}
		}

		uint32_t cp = utf8_decode(&p);
		if (!cp) break;

		FT_Face face = get_face_for_char(cp);
		if (!face) continue;

		FT_Error err = FT_Load_Char(face, cp, FT_LOAD_COLOR | FT_LOAD_RENDER);
		if (err != 0) continue;

		FT_GlyphSlot slot = face->glyph;

		if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
			int target_h = font_h;
			int target_w = font_h;

			if (max_w > 0 && (x + target_w - start_x > max_w)) {
				break;
			}

			int dst_x = x;
			int dst_y = baseline_y - ascender + (font_h - target_h) / 2;

			for (int dy = 0; dy < target_h; dy++) {
				int sy = dy * slot->bitmap.rows / target_h;
				for (int dx = 0; dx < target_w; dx++) {
					int sx = dx * slot->bitmap.width / target_w;
					int px = dst_x + dx;
					int py = dst_y + dy;
					if (px < 0 || px >= buf_w || py < 0 || py >= buf_h)
						continue;

					uint8_t *src = slot->bitmap.buffer + (sy * slot->bitmap.pitch + sx * 4);
					uint8_t b = src[0];
					uint8_t g = src[1];
					uint8_t r = src[2];
					uint8_t a = src[3];
					if (a == 0) continue;

					if (a == 255) {
						pixels[py * buf_w + px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
					} else {
						uint32_t bg = pixels[py * buf_w + px];
						uint8_t br = (bg >> 16) & 0xff;
						uint8_t bg_ = (bg >> 8) & 0xff;
						uint8_t bb = bg & 0xff;

						uint8_t out_r = (a * r + (255 - a) * br) / 255;
						uint8_t out_g = (a * g + (255 - a) * bg_) / 255;
						uint8_t out_b = (a * b + (255 - a) * bb) / 255;

						pixels[py * buf_w + px] = (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
					}
				}
			}
			x += target_w + 1;
		} else {
			int adv = (int)(slot->advance.x >> 6);
			if (max_w > 0 && (x + adv - start_x > max_w)) {
				break;
			}

			int gx = x + slot->bitmap_left;
			int gy = baseline_y - slot->bitmap_top;

			for (unsigned int row = 0; row < slot->bitmap.rows; row++) {
				for (unsigned int col = 0; col < slot->bitmap.width; col++) {
					int px = gx + (int)col;
					int py = gy + (int)row;
					if (px < 0 || px >= buf_w || py < 0 || py >= buf_h)
						continue;

					uint8_t alpha = 0;
					if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
						alpha = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
					} else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
						uint8_t byte = slot->bitmap.buffer[row * slot->bitmap.pitch + (col >> 3)];
						alpha = (byte & (0x80 >> (col & 7))) ? 255 : 0;
					}

					if (alpha == 0) continue;

					if (alpha == 255) {
						pixels[py * buf_w + px] = (0xFF << 24) | (cr << 16) | (cg << 8) | cb;
					} else {
						uint32_t bg = pixels[py * buf_w + px];
						uint8_t br = (bg >> 16) & 0xff;
						uint8_t bg_ = (bg >> 8) & 0xff;
						uint8_t bb = bg & 0xff;

						uint8_t r = (alpha * cr + (255 - alpha) * br) / 255;
						uint8_t g = (alpha * cg + (255 - alpha) * bg_) / 255;
						uint8_t b = (alpha * cb + (255 - alpha) * bb) / 255;

						pixels[py * buf_w + px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
					}
				}
			}
			x += adv;
		}
	}
	return x;
}

static int draw_text_ellipsized(struct bar_buffer *buf, FT_Face primary_face,
                                const char *text, int x, int baseline_y, uint32_t color, int max_w) {
	if (!text || text[0] == '\0' || max_w <= 0) return x;

	int total_w = measure_text_width(primary_face, text);
	if (total_w <= max_w) {
		return draw_text(buf, primary_face, text, x, baseline_y, color, max_w);
	}

	int dots_w = measure_text_width(primary_face, "...");
	int avail_w = max_w - dots_w;
	if (avail_w <= 0) {
		return draw_text(buf, primary_face, "...", x, baseline_y, color, max_w);
	}

	int cur_x = draw_text(buf, primary_face, text, x, baseline_y, color, avail_w);
	return draw_text(buf, primary_face, "...", cur_x, baseline_y, color, -1);
}

static int get_block_width(FT_Face primary_face, const struct BarBlock *blk, const char *raw_text) {
	if (!raw_text || raw_text[0] == '\0') return 0;

	char pfx_buf[256] = "";
	char sfx_buf[256] = "";
	const char *prefix = NULL;
	const char *suffix = NULL;
	const char *pfx_col = NULL;
	const char *sfx_col = NULL;
	get_block_prefix_suffix(blk, &prefix, &suffix, &pfx_col, &sfx_col, pfx_buf, sizeof(pfx_buf), sfx_buf, sizeof(sfx_buf));

	int width = 0;
	if (prefix && prefix[0] != '\0') {
		width += measure_text_width(primary_face, prefix);
	}

	if (blk->type == BLOCK_WORKSPACES) {
		const char *p = raw_text;
		bool first = true;
		int sep_w = measure_text_width(primary_face, "  ");

		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			const char *tok_start = p;
			while (*p && *p != ' ') p++;
			int tok_len = (int)(p - tok_start);

			char tok[64];
			if (tok_len >= (int)sizeof(tok)) tok_len = sizeof(tok) - 1;
			memcpy(tok, tok_start, tok_len);
			tok[tok_len] = '\0';

			if (!first) width += sep_w;
			first = false;
			width += measure_text_width(primary_face, tok);
		}
	} else {
		width += measure_text_width(primary_face, raw_text);
	}

	if (suffix && suffix[0] != '\0') {
		width += measure_text_width(primary_face, suffix);
	}

	return width;
}

static int render_block(struct bar_buffer *buf, FT_Face primary_face, const struct BarBlock *blk,
                        const char *raw_text, int x, int baseline_y, int max_w,
                        uint32_t pfx_color, uint32_t fg_color, uint32_t sfx_color) {
	if (!raw_text || raw_text[0] == '\0' || max_w == 0) return x;

	char pfx_buf[256] = "";
	char sfx_buf[256] = "";
	const char *prefix = NULL;
	const char *suffix = NULL;
	const char *pfx_col = NULL;
	const char *sfx_col = NULL;
	get_block_prefix_suffix(blk, &prefix, &suffix, &pfx_col, &sfx_col, pfx_buf, sizeof(pfx_buf), sfx_buf, sizeof(sfx_buf));

	if (blk->type == BLOCK_WORKSPACES) {
		if (prefix && prefix[0] != '\0') {
			x = draw_text(buf, primary_face, prefix, x, baseline_y, pfx_color, max_w);
		}

		const char *p = raw_text;
		bool first = true;

		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			const char *tok_start = p;
			while (*p && *p != ' ') p++;
			int tok_len = (int)(p - tok_start);

			char tok[64];
			if (tok_len >= (int)sizeof(tok)) tok_len = sizeof(tok) - 1;
			memcpy(tok, tok_start, tok_len);
			tok[tok_len] = '\0';

			if (!first) {
				x = draw_text(buf, primary_face, "  ", x, baseline_y, theme.inactive_ws, max_w);
			}
			first = false;

			uint32_t col = (tok[0] == '[' && tok[tok_len - 1] == ']') ? theme.active_ws : theme.inactive_ws;
			x = draw_text(buf, primary_face, tok, x, baseline_y, col, max_w);
		}

		if (suffix && suffix[0] != '\0') {
			x = draw_text(buf, primary_face, suffix, x, baseline_y, sfx_color, max_w);
		}
	} else {
		int pfx_w = (prefix && prefix[0] != '\0') ? measure_text_width(primary_face, prefix) : 0;
		int sfx_w = (suffix && suffix[0] != '\0') ? measure_text_width(primary_face, suffix) : 0;

		if (prefix && prefix[0] != '\0') {
			x = draw_text(buf, primary_face, prefix, x, baseline_y, pfx_color, max_w);
		}

		if (raw_text && raw_text[0] != '\0') {
			if (max_w > 0) {
				int text_max_w = max_w - pfx_w - sfx_w;
				if (text_max_w < 0) text_max_w = 0;
				x = draw_text_ellipsized(buf, primary_face, raw_text, x, baseline_y, fg_color, text_max_w);
			} else {
				x = draw_text(buf, primary_face, raw_text, x, baseline_y, fg_color, -1);
			}
		}

		if (suffix && suffix[0] != '\0') {
			x = draw_text(buf, primary_face, suffix, x, baseline_y, sfx_color, max_w);
		}
	}
	return x;
}

/* Rendering */
static void render_bar(void) {
	if (!bar.configured || bar.width <= 0 || bar.height <= 0 || bar.num_faces == 0) {
		return;
	}

	struct bar_buffer *buf = get_next_buffer(bar.width, bar.height);
	if (!buf) return;

	uint32_t *pixels = (uint32_t *)buf->shm_data;

	/* Clear background with precomputed color */
	for (int i = 0; i < bar.width * bar.height; i++) {
		pixels[i] = theme.bg;
	}

	FT_Face primary_face = bar.faces[0];
	int ascender = primary_face->size->metrics.ascender >> 6;
	int descender = primary_face->size->metrics.descender >> 6;
	int font_h = ascender - descender;
	int baseline_y = (bar.height - font_h) / 2 + ascender;

	/* 1. Render Left Block directly (returns ending x, eliminating extra measurement pass) */
	int left_end_x = render_block(buf, primary_face, &left_block, bar.left_text, padding_x, baseline_y, -1,
	                              theme.left_pfx, theme.left_fg, theme.left_sfx);
	int left_w = left_end_x - padding_x;

	/* 2. Render Right Block */
	int right_w = get_block_width(primary_face, &right_block, bar.right_text);
	int right_x = bar.width - right_w - padding_x;
	if (right_x < padding_x + left_w + 20) {
		right_x = padding_x + left_w + 20;
	}
	render_block(buf, primary_face, &right_block, bar.right_text, right_x, baseline_y, -1,
	             theme.right_pfx, theme.right_fg, theme.right_sfx);

	/* 3. Render Middle Block (centered, ellipsized if needed) */
	int max_mid_w = right_x - (padding_x + left_w + 20);
	if (max_mid_w > 0) {
		int mid_w = get_block_width(primary_face, &middle_block, bar.middle_text);
		int mid_x = (bar.width - mid_w) / 2;
		if (mid_x < padding_x + left_w + 10) {
			mid_x = padding_x + left_w + 10;
		}
		render_block(buf, primary_face, &middle_block, bar.middle_text, mid_x, baseline_y, max_mid_w,
		             theme.mid_pfx, theme.mid_fg, theme.mid_sfx);
	}

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

	/* Initialize theme colors */
	init_theme();

	/* Initialize font */
	if (!init_font(font_name)) {
		fprintf(stderr, "Failed to load font: %s\n", font_name);
	}

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

		if (changed || bar.dirty) {
			render_bar();
			bar.dirty = false;
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
	cleanup_font();
	wl_display_disconnect(bar.display);
	return 0;
}
