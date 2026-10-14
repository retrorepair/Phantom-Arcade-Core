/*
 * phantom.cpp - Phantom Arcade launcher: host link, catalog, input, and the glue that
 * puts it on the core's own screen.
 *
 * What this replaces
 * ------------------
 * Phantom Arcade originally ran as a framebuffer program under MiSTer's menu core,
 * started from Scripts. Picking a game forked a detached child, wrote "load_core
 * Groovy.rbf" to /dev/MiSTer_cmd, and exited so MiSTer would unblock and reprogram the
 * FPGA, while the child slept a configurable number of seconds and then told the PC to
 * start the emulator. That sleep was a guess about how long FPGA reconfiguration takes,
 * and the whole arrangement needed a script, a second .rbf and a core switch.
 *
 * Here the launcher lives inside the Groovy core's own HPS binary, so none of that
 * exists. The core is already loaded and already has a socket to the host. Selecting a
 * game sends one datagram. The host starts the emulator, its video arrives on the port
 * the core is already listening on, and groovy.cpp's existing setInit() path tears the
 * launcher down - the handover is driven by the first real frame instead of a timer.
 *
 * Threading: none. Everything here runs inside groovy_poll() on the HPS thread, so the
 * socket is non-blocking throughout and no call may wait on the network. groovy_poll()
 * is the same thread that services the video stream; a blocking recv here would stall
 * frames.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memmem */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>   /* KEY_* - phantom_keyboard sees raw event codes */

#include "phantom.h"
#include "phantom_ui.h"
#include "phantom_cat.h"
#include "../../user_io.h"

/* Hardware side, implemented in groovy.cpp. The launcher never touches SPI or the DDR
 * header itself; it asks for a canvas, draws, and asks for it to be shown. */
extern "C" {
uint32_t groovy_idle_canvas_bytes(void);
int      groovy_idle_begin(void);
void     groovy_idle_present(const uint8_t *src);
void     groovy_idle_end(void);
void     groovy_force_close(void);
int      groovy_ps2_inputs_enabled(void);
}

/* Overridable so tools/phantom_input_test.cpp can build this file off-target against a
 * scratch directory instead of a MiSTer's SD card. */
#ifndef PH_INI
#define PH_INI       "/media/fat/config/phantom.ini"
#endif
#ifndef PH_CACHE
#define PH_CACHE     "/media/fat/config/phantom_catalog.tsv"
#endif
/* An optional picture behind the whole launcher: a 720x480 binary PPM (P6). If the file is there it is used, every
 * time the launcher comes up; take it away and the launcher is as it always was. */
#ifndef PH_BG
#define PH_BG        "/media/fat/config/phantom_bg.ppm"
#endif

#define PH_DEF_PORT  1999

/* Timings, all milliseconds. */
#define T_DISCOVER_FAST  1500   /* while searching */
#define T_DISCOVER_SLOW 10000   /* after giving up, so a late host still gets found */
#define T_GIVE_UP       12000   /* searching -> offline */
#define T_PING           5000   /* liveness probe while online */
#define T_PING_LOST     16000   /* no reply for this long -> offline */
/* How long to sit on the loading screen before giving up. Generous on purpose: a
 * first RPCS3 boot compiles shaders and can run for minutes, and the screen shows a
 * running clock so the wait is visible rather than a guess. B cancels it sooner. */
#define T_LAUNCH_WAIT  180000
#define T_EXIT_HOLD      1200   /* Start+Select hold to kill the host emulator */
/* After the launcher takes the screen from a running stream, how long the launch button
 * must stay untouched before it will launch anything. See try_launch(). */
#define T_TAKEOVER_GUARD 1500

/* How long a session that has ended is waited on before the launcher takes the screen.
 * GRACE is always waited: a client that reconnects sends CMD_CLOSE and then CMD_INIT, and
 * has to be given time to do the second half. While the host says an emulator is still
 * running the wait continues, up to HOLD_CAP, asking every POLL. Overridable so the test
 * can run in seconds rather than half a minute. */
#ifndef T_CLOSE_GRACE
#define T_CLOSE_GRACE     2500
#endif
#ifndef T_CLOSE_HOLD_CAP
#define T_CLOSE_HOLD_CAP 20000
#endif
#ifndef T_CLOSE_POLL
#define T_CLOSE_POLL       400
#endif
#define T_STATUS_SHOW    6000   /* how long a transient message sits in the detail bar */

/* The library: how often the host is asked whether it has a new one, and how long it may stay silent before the
 * cabinet decides a different daemon has taken its place. Overridable for the same reason as the close timings. */
#ifndef T_INFO_POLL
#define T_INFO_POLL     10000   /* idle: a rescan is noticed within this */
#endif
#ifndef T_INFO_SCAN_POLL
#define T_INFO_SCAN_POLL 1000   /* while the host is still scanning */
#endif
#ifndef T_INFO_PROBE
#define T_INFO_PROBE    15000   /* a host that has never answered CATINFO is asked again this often, in case it was replaced */
#endif
#ifndef T_INFO_SILENT
#define T_INFO_SILENT    4000   /* a poll unanswered this long starts the retries that end in falling back */
#endif

/* One 480i field is ~16.7ms; there is no point building frames faster than the CRT
 * can show them, and every frame costs a full-canvas copy into DDR. */
#define PH_FRAME_MS        16

#define LOGP(...) do { printf("[PHANTOM] " __VA_ARGS__); fflush(stdout); } while (0)

static ph_ui     ui;
static pc_cat    cat;                /* the host's library, fetched a page at a time; see phantom_cat.h */
static int       cat_paged   = 0;    /* the host speaks the paged catalog and ui.src reads from `cat` */
static uint32_t  t_info_next = 0;    /* next CATINFO, to notice a rescan */
static uint32_t  t_info_last = 0;
static uint32_t  t_info_unanswered = 0;   /* when the oldest unanswered CATINFO poll went out; 0 = none */
static uint32_t  t_legacy_at = 0;    /* when to give up on CATINFO and ask the older way; 0 = not waiting */
static uint32_t  t_pump_next = 0;
static int       info_tries  = 0;
static int       info_answered = 0;   /* the host has answered CATINFO since it was last found */
static int       rescan_after_online = 0;   /* REFRESH asked for a rescan; send it once the host is found again */
static char      cat_host[64] = "";  /* whose library `cat` holds */
static uint8_t  *shadow      = 0;   /* cached-RAM canvas; see groovy_idle_present */
static uint32_t  t_presented = 0;
static int       inited      = 0;
static int       owns_screen = 0;
static int       idle_mode   = PHANTOM_IDLE_LAUNCHER;
static int       exit_hotkey = 1;

/* Pad edge state and auto-repeat. Up here rather than beside the input code because the
 * screen handover (idle_enter / idle_leave) has to be able to reset it. */
static uint32_t  prev_mask[4];
static uint32_t  rep_at[4];
static int       rep_dir[4];
static int       rep_n[4];
static uint32_t  repx_at[4];     /* the same for Left/Right, which page: held, it accelerates across the library */
static int       repx_dir[4];
static int       repx_n[4];

/* The launcher took the screen from a stream that was running, which is the one moment a
 * button being pressed was meant for a game. stream_was_live is set when a stream starts
 * and consumed by the next takeover; the guard then holds launches off until the launch
 * button has been left alone for T_TAKEOVER_GUARD. */
static int       stream_was_live = 0;
static int       guard_on        = 0;
static uint32_t  guard_until     = 0;

/* A session that has ended and is being waited on (see phantom_session_ended). end_host is
 * what the host last said since the wait began: -1 nothing, 0 no game running, 1 running. */
static int       end_reason  = 0;
static uint32_t  end_since   = 0;
static uint32_t  end_poll_at = 0;
static int       end_host    = -1;

static int       sock = -1;
static int       cfg_port = PH_DEF_PORT;
static char      cfg_host[64] = "";      /* optional manual override */
static char      live_host[64] = "";     /* whoever actually answered */

static uint32_t  t_next_probe = 0;
static uint32_t  t_search_began = 0;
static uint32_t  t_last_reply = 0;
static uint32_t  t_launch_sent = 0;
static uint32_t  t_status_set = 0;   /* transient message in the detail bar */
static uint32_t  t_busy_tick = 0;    /* loading screen redraw, once a second */
static int       awaiting_video = 0;
static char      last_id[PH_ID_LEN] = "";

/* Exit-hotkey edge tracking, live only while a stream is running. */
static uint32_t  hk_since = 0;
static int       hk_fired = 0;

static void repeat_tick(void);   /* defined with the rest of the input handling */
static void send_to(const char *ip, const char *msg);   /* defined with the socket */

/* ---- clock -------------------------------------------------------------------- */

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ---- config ------------------------------------------------------------------- */

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

/* phantom.ini is Phantom Arcade's existing file and keeps its existing keys, so an
 * installation that already had one keeps working untouched. IDLE_SCREEN and
 * EXIT_HOTKEY are new and default to the launcher being on. */
/* The display's overscan margins, left/top/right/bottom, as saved by the screen-adjust page. */
static int safe_cfg[4] = { PH_SAFE_DEF_L, PH_SAFE_DEF_T, PH_SAFE_DEF_R, PH_SAFE_DEF_B };

/* How the picture behind the menu is shown: the brightness it is dimmed to, and how solid the panels are over it
 * (the list, which is the part to see through, and everything else). See phantom_ui.h. */
static int bg_dim = 50, bg_panels = PH_BG_PANEL_ALPHA, bg_list = PH_BG_LIST_ALPHA;

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load_config(void)
{
	FILE *f = fopen(PH_INI, "r");
	if (!f) return;

	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		trim(line);
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == ';' || *p == '[' || !*p) continue;

		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = p, *val = eq + 1;
		while (*val == ' ' || *val == '\t') val++;

		if (!strcmp(key, "PC_SERVER_IP"))
		{
			snprintf(cfg_host, sizeof(cfg_host), "%s", val);
		}
		else if (!strcmp(key, "UDP_PORT"))
		{
			int v = atoi(val);
			if (v > 0 && v < 65536) cfg_port = v;
		}
		else if (!strcmp(key, "IDLE_SCREEN"))
		{
			if (!strcasecmp(val, "logo"))     idle_mode = PHANTOM_IDLE_LOGO;
			else if (!strcasecmp(val, "off")) idle_mode = PHANTOM_IDLE_OFF;
			else                              idle_mode = PHANTOM_IDLE_LAUNCHER;
		}
		else if (!strcmp(key, "EXIT_HOTKEY"))
		{
			exit_hotkey = (!strcasecmp(val, "off") || !strcmp(val, "0")) ? 0 : 1;
		}
		else if (!strcmp(key, "SAFE_L")) safe_cfg[0] = atoi(val);
		else if (!strcmp(key, "SAFE_T")) safe_cfg[1] = atoi(val);
		else if (!strcmp(key, "SAFE_R")) safe_cfg[2] = atoi(val);
		else if (!strcmp(key, "SAFE_B")) safe_cfg[3] = atoi(val);
		else if (!strcmp(key, "BG_DIM"))    bg_dim    = clamp_int(atoi(val), 0, 100);
		else if (!strcmp(key, "BG_PANELS")) bg_panels = clamp_int(atoi(val), 1, 255);
		else if (!strcmp(key, "BG_LIST"))   bg_list   = clamp_int(atoi(val), 1, 255);
	}
	fclose(f);

	/* ph_ui_set_safe clamps and rounds, so a hand-edited value cannot break the layout. */
	ph_ui_set_safe(&ui, safe_cfg[0], safe_cfg[1], safe_cfg[2], safe_cfg[3]);
}

void phantom_save_config(void)
{
	FILE *f = fopen(PH_INI, "w");
	if (!f)
	{
		LOGP("cannot write %s: %s\n", PH_INI, strerror(errno));
		return;
	}
	fprintf(f, "[SERVER]\n");
	fprintf(f, "PC_SERVER_IP=%s\n", cfg_host);
	fprintf(f, "UDP_PORT=%d\n", cfg_port);
	fprintf(f, "\n[LAUNCHER]\n");
	fprintf(f, "IDLE_SCREEN=%s\n",
	        idle_mode == PHANTOM_IDLE_LOGO ? "logo" :
	        idle_mode == PHANTOM_IDLE_OFF  ? "off"  : "launcher");
	fprintf(f, "EXIT_HOTKEY=%s\n", exit_hotkey ? "on" : "off");
	fprintf(f, "SAFE_L=%d\nSAFE_T=%d\nSAFE_R=%d\nSAFE_B=%d\n", ui.safe.l, ui.safe.t, ui.safe.r, ui.safe.b);
	fprintf(f, "BG_DIM=%d\nBG_PANELS=%d\nBG_LIST=%d\n", bg_dim, bg_panels, bg_list);
	fclose(f);
}

/* ---- the picture behind the menu ----------------------------------------------- */

static uint8_t *bg_buf   = 0;
static time_t   bg_mtime = 0;
static long     bg_mtime_ns = 0;
static off_t    bg_size  = 0;

/* Pick up phantom_bg.ppm: loaded when it appears or changes, dropped when it goes. Called as the launcher comes up,
 * which is cheap (one stat) and means a new picture is seen the next time the menu returns, with nothing to restart. */
static void bg_load(void)
{
	struct stat st;
	if (stat(PH_BG, &st) != 0)
	{
		if (bg_buf)
		{
			free(bg_buf);
			bg_buf = 0;
			LOGP("background picture removed\n");
		}
		ui.bg = 0;
		ui.dirty = 1;
		return;
	}

	if (bg_buf && st.st_mtime == bg_mtime && st.st_mtim.tv_nsec == bg_mtime_ns && st.st_size == bg_size)
	{
		ui.bg = bg_buf;                  /* still the picture we hold; ph_ui_init may have cleared the pointer */
		ui.bg_alpha = bg_panels;
		ui.bg_list_alpha = bg_list;
		return;
	}

	FILE *f = fopen(PH_BG, "rb");
	if (!f) return;
	char magic[3] = { 0 };
	int w = 0, h = 0, mx = 0;
	if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &mx) != 4 || strcmp(magic, "P6") || w != PH_CANVAS_W || h != PH_CANVAS_H || mx != 255)
	{
		LOGP("%s is not a %dx%d binary PPM; ignored\n", PH_BG, PH_CANVAS_W, PH_CANVAS_H);
		fclose(f);
		return;
	}
	fgetc(f);                            /* the one whitespace after the header */

	const size_t n = (size_t)PH_CANVAS_W * PH_CANVAS_H * 3;
	uint8_t *p = (uint8_t *)malloc(n);
	if (!p || fread(p, 1, n, f) != n)
	{
		LOGP("could not read the picture in %s\n", PH_BG);
		free(p);
		fclose(f);
		return;
	}
	fclose(f);

	/* a PPM is R,G,B and the canvas is B,G,R */
	for (size_t i = 0; i < n; i += 3) { uint8_t r = p[i]; p[i] = p[i + 2]; p[i + 2] = r; }
	ph_ui_dim(p, p, n, bg_dim);

	free(bg_buf);
	bg_buf = p;
	bg_mtime = st.st_mtime;
	bg_mtime_ns = st.st_mtim.tv_nsec;
	bg_size = st.st_size;
	ui.bg = bg_buf;
	ui.bg_alpha = bg_panels;
	ui.bg_list_alpha = bg_list;
	ui.dirty = 1;
	LOGP("background picture loaded from %s (dimmed to %d%%)\n", PH_BG, bg_dim);
}

/* ---- catalog cache ------------------------------------------------------------ */

/* The cache exists so the list is populated the instant the core comes up, before the
 * host has answered. It is a plain TSV rather than the host's JSON because it only has
 * to round-trip the fields the launcher actually shows. */
static void cache_save(void)
{
	FILE *f = fopen(PH_CACHE, "w");
	if (!f) return;
	fprintf(f, "# Phantom Arcade catalog cache - rewritten whenever the host sends a new one\n");
	for (int i = 0; i < ui.count; i++)
	{
		const ph_game *g = &ui.games[i];
		fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
		        g->id, g->title, g->system, g->sysname, g->mode, g->year, g->maker);
	}
	fclose(f);
}

/* The list is in title order whatever order the host or the cache gave it, because the
 * jump-to-letter buttons only mean something on a sorted list. Case-insensitive, with the id
 * to settle ties so the order is the same every time. */
static int cmp_game(const void *a, const void *b)
{
	const ph_game *x = (const ph_game *)a;
	const ph_game *y = (const ph_game *)b;
	int c = pc_title_cmp(x->title, y->title);   /* the host's order, so the letter jumps agree with it */
	return c ? c : strcmp(x->id, y->id);
}

static void cache_load(void)
{
	FILE *f = fopen(PH_CACHE, "r");
	if (!f) return;

	char line[768];
	ui.count = 0;
	while (fgets(line, sizeof(line), f) && ui.count < PH_MAX_GAMES)
	{
		if (line[0] == '#' || line[0] == '\n') continue;
		trim(line);

		char *fld[7] = { 0 };
		int n = 0;
		char *p = line;
		while (n < 7 && p)
		{
			fld[n++] = p;
			char *t = strchr(p, '\t');
			if (!t) break;
			*t = 0;
			p = t + 1;
		}
		if (n < 3 || !fld[0][0]) continue;

		ph_game *g = &ui.games[ui.count++];
		memset(g, 0, sizeof(*g));
		snprintf(g->id, sizeof(g->id), "%s", fld[0]);
		snprintf(g->title, sizeof(g->title), "%s", fld[1] ? fld[1] : "");
		snprintf(g->system, sizeof(g->system), "%s", fld[2] ? fld[2] : "");
		if (n > 3 && fld[3]) snprintf(g->sysname, sizeof(g->sysname), "%s", fld[3]);
		if (n > 4 && fld[4]) snprintf(g->mode, sizeof(g->mode), "%s", fld[4]);
		/* older caches stop at mode; the rest simply stay empty */
		if (n > 5 && fld[5]) snprintf(g->year, sizeof(g->year), "%s", fld[5]);
		if (n > 6 && fld[6]) snprintf(g->maker, sizeof(g->maker), "%s", fld[6]);
	}
	fclose(f);
	if (ui.count) qsort(ui.games, (size_t)ui.count, sizeof(ph_game), cmp_game);
	if (ui.count) LOGP("loaded %d cached titles\n", ui.count);
}

/* ---- minimal JSON field reader ------------------------------------------------ */

/* The host serves games_catalog.json verbatim in one datagram, capped at 60000 bytes
 * on its side. That means a large library can arrive truncated mid-record, so the
 * parser is written to stop cleanly at the first record it cannot complete rather than
 * trusting the document to be well formed. */
static const char *json_str(const char *obj, const char *end, const char *key, char *out, int outsz)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *k = obj;
	size_t patlen = strlen(pat);

	while (k < end)
	{
		const char *hit = (const char *)memmem(k, (size_t)(end - k), pat, patlen);
		if (!hit) return 0;
		const char *c = hit + patlen;
		while (c < end && (*c == ' ' || *c == '\t')) c++;
		if (c < end && *c == ':')
		{
			c++;
			while (c < end && (*c == ' ' || *c == '\t')) c++;
			if (c < end && *c == '"')
			{
				c++;
				int n = 0;
				while (c < end && *c != '"' && n < outsz - 1)
				{
					if (*c == '\\' && c + 1 < end) c++;   /* keep it simple: unescape one level */
					out[n++] = *c++;
				}
				if (c >= end) return 0;   /* truncated string: treat as no value */
				out[n] = 0;
				return c;
			}
		}
		k = hit + patlen;
	}
	return 0;
}

static void parse_catalog(const char *buf, int len)
{
	const char *end = buf + len;
	const char *p = buf;
	int n = 0;

	char keep[PH_ID_LEN];
	const ph_game *cur = ph_ui_selected(&ui);
	snprintf(keep, sizeof(keep), "%s", cur ? cur->id : "");

	ph_game tmp[PH_MAX_GAMES];

	while (n < PH_MAX_GAMES)
	{
		const char *rec = (const char *)memmem(p, (size_t)(end - p), "\"id\"", 4);
		if (!rec) break;

		/* Bound the record so a field lookup cannot wander into the next one. */
		const char *rec_end = (const char *)memmem(rec, (size_t)(end - rec), "}", 1);
		if (!rec_end) break;              /* truncated final record: drop it */

		ph_game *g = &tmp[n];
		memset(g, 0, sizeof(*g));

		if (!json_str(rec, rec_end, "id", g->id, sizeof(g->id)) || !g->id[0])
		{
			p = rec_end + 1;
			continue;
		}
		json_str(rec, rec_end, "title", g->title, sizeof(g->title));
		json_str(rec, rec_end, "system", g->system, sizeof(g->system));
		json_str(rec, rec_end, "systemName", g->sysname, sizeof(g->sysname));
		if (!json_str(rec, rec_end, "videoMode", g->mode, sizeof(g->mode)))
			json_str(rec, rec_end, "resolution", g->mode, sizeof(g->mode));
		json_str(rec, rec_end, "year", g->year, sizeof(g->year));
		json_str(rec, rec_end, "manufacturer", g->maker, sizeof(g->maker));

		if (!g->title[0]) snprintf(g->title, sizeof(g->title), "%s", g->id);
		if (!g->system[0]) snprintf(g->system, sizeof(g->system), "%s", "groovymame");

		n++;
		p = rec_end + 1;
	}

	/* An empty or unparseable reply must not wipe a good cached list: the host saying
	 * nothing useful is not the same as the host saying "you have no games". */
	if (n == 0)
	{
		LOGP("catalog reply had no usable records (%d bytes), keeping %d existing\n", len, ui.count);
		return;
	}

	qsort(tmp, (size_t)n, sizeof(ph_game), cmp_game);
	memcpy(ui.games, tmp, sizeof(ph_game) * (size_t)n);
	ui.count = n;
	ph_ui_reclamp(&ui, keep);
	cache_save();
	LOGP("catalog: %d titles from %s\n", n, live_host);
}

/* ---- the paged library -------------------------------------------------------------- */

/* A host that speaks the paged catalog answers CATINFO. One that does not (an older daemon) says nothing, and
 * after a moment we ask the old way - the whole list in one datagram - and live with its limits. */
static void request_library(void)
{
	if (!live_host[0]) return;

	/* a different machine answering is a different library */
	if (strcmp(cat_host, live_host))
	{
		pc_reset(&cat);
		cat_paged = 0;
		memset(&ui.src, 0, sizeof(ui.src));
		snprintf(cat_host, sizeof(cat_host), "%s", live_host);
	}

	uint32_t t = now_ms();
	send_to(live_host, "CATINFO");
	t_info_last = t;
	t_info_next = t + 1000;
	info_tries = 1;
	info_answered = 0;
	t_info_unanswered = 0;
	if (!cat_paged)
	{
		t_legacy_at = t + 1500;
		ui.lib_loading = (ui.count == 0);
		ui.dirty = 1;
	}

	if (rescan_after_online)
	{
		rescan_after_online = 0;
		send_to(live_host, "RESCAN");
	}
}

static void on_cat_event(int ev)
{
	switch (ev)
	{
	case PC_INFO_NEW:
		if (!cat_paged) LOGP("host speaks the paged catalog\n");
		cat_paged = 1;
		info_answered = 1;
		t_legacy_at = 0;
		ui.src = pc_source(&cat);
		ui.lib_loading = 0;
		ui.lib_scanning = cat.scanning;
		t_info_next = now_ms() + (cat.scanning ? T_INFO_SCAN_POLL : T_INFO_POLL);
		t_info_unanswered = 0;
		ph_ui_reclamp(&ui, 0);
		ui.dirty = 1;
		LOGP("library v%u: %d titles in %d systems%s\n", (unsigned)cat.ver, cat.total, cat.nsys, cat.scanning ? " (host still scanning)" : "");
		break;
	case PC_INFO:
		info_answered = 1;
		ui.lib_loading = 0;
		t_info_unanswered = 0;
		t_info_next = now_ms() + (cat.scanning ? T_INFO_SCAN_POLL : T_INFO_POLL);
		if (ui.lib_scanning != cat.scanning)
		{
			ui.lib_scanning = cat.scanning;
			ui.dirty = 1;
		}
		break;
	case PC_PAGE_IN:
	case PC_LETTERS_IN:
		ui.dirty = 1;
		break;
	default:
		break;
	}
}

/* Keep the rows around the cursor on their way from the host. Cheap when nothing is missing, so it runs
 * on a short timer rather than every pass of the poll loop. */
static void pump_pages(uint32_t t)
{
	if (!cat_paged || !live_host[0] || !owns_screen || ui.view != PH_VIEW_LIST) return;

	static int last_sel = -1, last_scroll = -1, last_tab = -1, dir = 0;
	/* Moving to rows that are not here yet is the moment latency matters: ask at once. The timer only paces
	 * the housekeeping - retries, and the read-ahead once the cursor has stopped. */
	const int moved = (ui.sel != last_sel || ui.scroll != last_scroll || ui.tab != last_tab);
	if (!moved && (int32_t)(t - t_pump_next) < 0) return;
	t_pump_next = t + 30;

	if (ui.sel != last_sel)
	{
		dir = (ui.sel > last_sel) ? 1 : -1;
		last_sel = ui.sel;
	}
	last_scroll = ui.scroll;
	last_tab = ui.tab;

	int list = pc_list_for_tab(&cat, ui.tab);
	int rows = ph_ui_rows(&ui);
	char req[96];
	for (int i = 0; i < 6 && pc_next_request(&cat, t, list, ui.scroll, ui.scroll + rows - 1, dir, req, sizeof(req)); i++)
		send_to(live_host, req);
}

static void library_tick(uint32_t t)
{
	if (!live_host[0]) return;

	if (!info_answered)
	{
		if (info_tries < 4)
		{
			/* CATINFO may simply have been lost; a few tries before deciding the host is an older one */
			if ((int32_t)(t - t_info_next) >= 0)
			{
				send_to(live_host, "CATINFO");
				info_tries++;
				t_info_next = t + 1000;
			}
		}
		else if (cat_paged && (int32_t)(t - t_info_next) >= 0)
		{
			/* It answered CATINFO before and does not now: an older daemon has taken its place. What we hold
			 * is a library it cannot page, so drop it and ask the older way. */
			LOGP("host no longer answers CATINFO: back to the whole-catalog request\n");
			cat_paged = 0;
			pc_reset(&cat);
			memset(&ui.src, 0, sizeof(ui.src));
			ui.lib_scanning = 0;
			t_legacy_at = t;
			ui.dirty = 1;
		}
	}

	if (!cat_paged)
	{
		/* an older host never answers CATINFO: after a moment, ask for the whole list in one datagram */
		if (t_legacy_at && (int32_t)(t - t_legacy_at) >= 0)
		{
			t_legacy_at = 0;
			ui.lib_loading = 0;
			send_to(live_host, "GET_CATALOG");
			LOGP("no CATINFO answer: asking for the whole catalog\n");
		}
		/* ...and keep asking now and then. A host that is restarted does not look like a host that has just
		 * appeared, so nothing else would tell us that the daemon on the other end has been replaced by one
		 * that can page. An older daemon ignores the datagram, which costs nothing. */
		if (!info_answered && info_tries >= 4 && (int32_t)(t - t_info_next) >= 0)
		{
			send_to(live_host, "CATINFO");
			t_info_next = t + T_INFO_PROBE;
		}
		return;
	}
	if (!info_answered) return;

	/* notice a rescan on the host: often while it is still working, rarely otherwise */
	if ((cat.want_info && (int32_t)(t - t_info_last) >= 100) || (int32_t)(t - t_info_next) >= 0)
	{
		cat.want_info = 0;
		t_info_last = t;
		t_info_next = t + (cat.scanning ? T_INFO_SCAN_POLL : T_INFO_POLL);
		send_to(live_host, "CATINFO");
		if (!t_info_unanswered) t_info_unanswered = t;
	}

	/* A poll that is never answered means the daemon on the other end is not the one that was: start over, and the
	 * retries decide whether it is an older one. */
	if (t_info_unanswered && (int32_t)(t - t_info_unanswered) > T_INFO_SILENT)
	{
		t_info_unanswered = 0;
		info_answered = 0;
		info_tries = 1;
		t_info_next = t + 1000;
		return;
	}
	pump_pages(t);
}

/* ---- socket ------------------------------------------------------------------- */

static void sock_open(void)
{
	if (sock >= 0) return;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
	{
		LOGP("socket: %s\n", strerror(errno));
		return;
	}

	int on = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	fcntl(sock, F_SETFL, O_NONBLOCK);

	/* Ephemeral source port on purpose: Groovy already owns 32100/32101/32105 and the
	 * launcher must not collide with them or with a second instance. */
	struct sockaddr_in me;
	memset(&me, 0, sizeof(me));
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_ANY);
	me.sin_port = 0;
	if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0)
	{
		LOGP("bind: %s\n", strerror(errno));
		close(sock);
		sock = -1;
	}
}

static void send_to(const char *ip, const char *msg)
{
	if (sock < 0 || !ip || !ip[0]) return;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)cfg_port);
	a.sin_addr.s_addr = inet_addr(ip);
	sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof(a));
}

static void probe(void)
{
	/* Broadcast finds a host nobody configured; the explicit unicast covers a network
	 * that drops broadcast, and a host named in phantom.ini. */
	send_to("255.255.255.255", "DISCOVER_PHANTOM");
	if (cfg_host[0]) send_to(cfg_host, "DISCOVER_PHANTOM");
	if (live_host[0]) send_to(live_host, "DISCOVER_PHANTOM");
}

static void drain(void)
{
	if (sock < 0) return;

	char buf[65536];
	for (;;)
	{
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0) break;
		buf[n] = 0;

		char ip[64];
		snprintf(ip, sizeof(ip), "%s", inet_ntoa(from.sin_addr));
		t_last_reply = now_ms();

		if (!strncmp(buf, "PHANTOM_HOST_ONLINE", 19) || !strcmp(buf, "PONG"))
		{
			int was = ui.host_state;
			snprintf(live_host, sizeof(live_host), "%s", ip);
			snprintf(ui.host_ip, sizeof(ui.host_ip), "%s", ip);
			ui.host_port = cfg_port;
			ui.host_state = PH_HOST_ONLINE;
			ui.dirty = 1;
			if (was != PH_HOST_ONLINE)
			{
				LOGP("host online at %s:%d\n", ip, cfg_port);
				request_library();
			}
		}
		else if (!strncmp(buf, "STATUS:", 7))
		{
			/* The answer to our STATUS poll. Only meaningful while a session end is being
			 * waited on; an old host never sends it, which reads as "no answer". */
			end_host = strstr(buf, "RUNNING") ? 1 : 0;
		}
		else if (!strncmp(buf, "ACK:LAUNCH:STALE", 16) || !strncmp(buf, "ACK:LAUNCH:NOTFOUND", 19) ||
		         !strncmp(buf, "ACK:LAUNCH:BAD", 14))
		{
			/* The host's library is not the one this row came from (it was rescanned since the page was
			 * fetched), so the position may now be a different game. Nothing was started; say so and
			 * fetch the new list rather than guess. */
			LOGP("host refused the launch: %s\n", buf);
			awaiting_video = 0;
			ui.view = PH_VIEW_LIST;
			snprintf(ui.status, sizeof(ui.status), "The library changed - choose again.");
			t_status_set = now_ms();
			cat.want_info = 1;
			ui.dirty = 1;
		}
		else if (!strncmp(buf, "LAUNCHFAIL:", 11))
		{
			/* The emulator would not start, stopped straight away, or is sitting behind an error dialog on the PC.
			 * Until the host said so, this screen waited three minutes on "starting the emulator". */
			LOGP("launch failed on the host: %s\n", buf + 11);
			awaiting_video = 0;
			ui.view = PH_VIEW_LIST;
			snprintf(ui.status, sizeof(ui.status), "%s", buf + 11);
			t_status_set = now_ms() + 5000;   /* an error is worth reading: about eleven seconds, not six */
			ui.dirty = 1;
		}
		else if (!strncmp(buf, "ACK:LAUNCH", 10))
		{
			LOGP("host acked launch\n");
			snprintf(ui.status, sizeof(ui.status), "Host is starting the emulator...");
			ui.dirty = 1;
		}
		else if (!strncmp(buf, "CATINFO:", 8) || !strncmp(buf, "LETTERS:", 8) || !strncmp(buf, "LIST:", 5) ||
		         !strncmp(buf, "STALE:", 6))
		{
			on_cat_event(pc_on_reply(&cat, buf, (int)n));
		}
		else if (!strncmp(buf, "RESCAN:", 7))
		{
			/* acknowledged; the host's CATINFO tells us when it is scanning and when it is done */
			t_info_next = now_ms() + 300;
		}
		else if (!strncmp(buf, "ACK:KILL", 8))
		{
			LOGP("host acked kill\n");
		}
		else if (!cat_paged && n > 8 && (buf[0] == '{' || strstr(buf, "\"id\"")))
		{
			ui.lib_loading = 0;
			parse_catalog(buf, (int)n);
			ui.dirty = 1;
		}
	}
}

/* ---- launcher actions --------------------------------------------------------- */

static void do_launch(void)
{
	const ph_game *g = ph_ui_selected(&ui);
	if (!g) return;

	if (ui.host_state != PH_HOST_ONLINE)
	{
		snprintf(ui.status, sizeof(ui.status), "No host - press REFRESH to search.");
		t_status_set = now_ms();
		ui.dirty = 1;
		return;
	}

	/* The :kbd= suffix tells the host whether this core is sending a keyboard, so it
	 * can decide whether to point the emulator at the MiSTer one. A host that does not
	 * understand the suffix takes everything up to the first colon as the id. */
	char msg[192];
	if (cat_paged)
	{
		/* By position, quoting the library's version: the cabinet holds a window of the list, not every id, and
		 * the host refuses (rather than starts the wrong game) if the library has been rescanned since. */
		int list = pc_list_for_tab(&cat, ui.tab);
		snprintf(msg, sizeof(msg), "LAUNCH:@%u:%s:%d:kbd=%d", (unsigned)cat.ver, pc_list_key(&cat, list), ui.sel,
		         groovy_ps2_inputs_enabled());
	}
	else
	{
		snprintf(msg, sizeof(msg), "LAUNCH:%s:kbd=%d", g->id, groovy_ps2_inputs_enabled());
	}
	send_to(live_host, msg);

	snprintf(last_id, sizeof(last_id), "%s", g->id);
	snprintf(ui.busy_title, sizeof(ui.busy_title), "%s", g->title);
	snprintf(ui.status, sizeof(ui.status), "Waiting for host video...");
	ui.view = PH_VIEW_BUSY;
	ui.busy_t0 = now_ms();
	ui.tick_ms = ui.busy_t0;
	t_busy_tick = 0;
	ui.dirty = 1;

	awaiting_video = 1;
	t_launch_sent = now_ms();
	LOGP("launch %s\n", g->id);
}

static int launch_guarded(void)
{
	if (!guard_on) return 0;
	if ((int32_t)(now_ms() - guard_until) >= 0)
	{
		guard_on = 0;
		return 0;
	}
	return 1;
}

/* Every launch request from the pad or the keyboard goes through here.
 *
 * While the guard is up the request is swallowed, and it pushes the guard out again, so
 * a button that is still being hit keeps the launcher shut and only a pause releases it.
 * Anything shorter than that and mashing a button through the handover launches a game
 * every second press. */
static void try_launch(void)
{
	if (launch_guarded())
	{
		guard_until = now_ms() + T_TAKEOVER_GUARD;
		return;
	}
	do_launch();
}

void phantom_send_kill(void)
{
	if (live_host[0]) send_to(live_host, "KILL");
	LOGP("kill sent\n");
}

/* "Stop the game and come back to the menu", as one action.
 *
 * Both halves are needed. Telling the host to kill the emulator leaves the core still
 * holding the session - GroovyMAME has no keepalive capability, so a process that was
 * terminated never sends CMD_CLOSE and the idle timeout is not allowed to reap it. The
 * CRT would keep showing the last frame for ever. Closing locally alone would be worse:
 * the launcher would return while the emulator carried on running on the PC, still
 * streaming into a core that had stopped listening.
 *
 * Order matters: the KILL goes first so the emulator is already on its way out by the
 * time the core stops accepting its frames. */
void phantom_return_to_launcher(void)
{
	phantom_send_kill();
	groovy_force_close();
	snprintf(ui.status, sizeof(ui.status), "Stopped. Back at the launcher.");
	t_status_set = now_ms();
	ui.dirty = 1;
	LOGP("returned to launcher on request\n");
}

void phantom_request_refresh(void)
{
	/* A paged host is asked to look for new and changed games once it has been found again; an older one has
	 * nothing to rescan, and simply sends its list again. */
	rescan_after_online = cat_paged;
	live_host[0] = 0;
	ui.host_state = PH_HOST_SEARCHING;
	t_search_began = now_ms();
	t_next_probe = 0;
	ui.dirty = 1;
	LOGP("refresh requested\n");
}

/* ---- lifecycle ---------------------------------------------------------------- */

void phantom_init(void)
{
	if (inited) return;
	inited = 1;

	ph_ui_init(&ui, 0, PH_CANVAS_W, PH_CANVAS_H);

	load_config();
	bg_load();
	snprintf(ui.host_ip, sizeof(ui.host_ip), "%s", cfg_host);
	ui.host_port = cfg_port;
	ui.host_state = PH_HOST_SEARCHING;

	pc_init(&cat);
	cache_load();
	ph_ui_reclamp(&ui, 0);

	sock_open();
	t_search_began = now_ms();
	t_next_probe = 0;

	LOGP("ready (idle=%s, port=%d, hotkey=%s)\n",
	     idle_mode == PHANTOM_IDLE_LOGO ? "logo" : "launcher", cfg_port, exit_hotkey ? "on" : "off");
}

void phantom_stop(void)
{
	if (sock >= 0) { close(sock); sock = -1; }
	if (shadow) { free(shadow); shadow = 0; }
	ui.fb = 0;
	owns_screen = 0;
	end_reason = 0;
	pc_reset(&cat);
	cat_paged = 0;
	cat_host[0] = 0;
	free(bg_buf);
	bg_buf = 0;
	inited = 0;
}

int phantom_idle_enter(void)
{
	if (!inited || idle_mode != PHANTOM_IDLE_LAUNCHER) return 0;

	if (!shadow)
	{
		shadow = (uint8_t *)calloc(1, groovy_idle_canvas_bytes());
		if (!shadow)
		{
			LOGP("cannot allocate the %u byte canvas\n", groovy_idle_canvas_bytes());
			return 0;
		}
	}

	if (!groovy_idle_begin())
	{
		LOGP("could not program the launcher video mode, falling back to the logo\n");
		return 0;
	}

	ui.fb = shadow;
	owns_screen = 1;
	awaiting_video = 0;
	bg_load();   /* a picture put on the card while a game was running is there when the menu comes back */

	/* Taking the screen from a running stream means whatever the user is doing with the
	 * pad right now is aimed at a game, not at this menu. A session can end under them
	 * for reasons they cannot see - a stalled emulator, a client reconnect, the exit
	 * hotkey - and a press that lands first would launch whatever row is highlighted,
	 * which the host answers by killing the game they were in. */
	memset(rep_dir, 0, sizeof(rep_dir));
	memset(repx_dir, 0, sizeof(repx_dir));
	end_reason = 0;   /* whatever was being waited on has been decided, one way or another */
	if (stream_was_live)
	{
		stream_was_live = 0;
		guard_on = 1;
		guard_until = now_ms() + T_TAKEOVER_GUARD;
		LOGP("took the screen from a running stream; launches held off for %dms\n", T_TAKEOVER_GUARD);
	}
	ui.view = PH_VIEW_LIST;
	ui.status[0] = 0;
	ui.tick_ms = now_ms();
	ui.dirty = 1;

	/* A fresh look for the host every time the screen comes back: the PC may have been
	 * switched on, or rebooted, while a stream was running. */
	if (ui.host_state != PH_HOST_ONLINE)
	{
		ui.host_state = PH_HOST_SEARCHING;
		t_search_began = now_ms();
	}
	t_next_probe = 0;

	ph_ui_render(&ui);
	groovy_idle_present(shadow);
	t_presented = now_ms();
	return 1;
}

void phantom_idle_leave(void)
{
	if (!owns_screen) return;
	owns_screen = 0;
	awaiting_video = 0;
	ui.view = PH_VIEW_LIST;
	stream_was_live = 1;
	guard_on = 0;
	memset(rep_dir, 0, sizeof(rep_dir));   /* a held stick must not keep scrolling a hidden list */
	memset(repx_dir, 0, sizeof(repx_dir));
	groovy_idle_end();
	LOGP("host video started, launcher released the screen\n");
}

int phantom_owns_screen(void)
{
	return owns_screen;
}

/* ---- ending a session without taking the screen ------------------------------------- */

int phantom_session_ended(int reason)
{
	/* Nothing to protect: the launcher is not the idle screen, or is already showing. */
	if (!inited || idle_mode != PHANTOM_IDLE_LAUNCHER || owns_screen) return 0;
	if (end_reason) return 2;

	end_reason  = reason;
	end_since   = now_ms();
	end_host    = -1;
	end_poll_at = end_since + T_CLOSE_POLL;
	send_to(live_host, "STATUS");

	LOGP("session ended (%s); holding the screen for at least %dms\n",
		 reason == PHANTOM_END_TIMEOUT ? "client went quiet" : "client closed", T_CLOSE_GRACE);
	return 1;
}

void phantom_session_alive(int how)
{
	if (!end_reason) return;
	if (how == PHANTOM_ALIVE_ACTIVITY && end_reason != PHANTOM_END_TIMEOUT) return;

	LOGP("client is back after %ums; the launcher never took the screen\n", (unsigned)(now_ms() - end_since));
	end_reason = 0;
}

/* Called from the idle poll, which runs whether or not the launcher is on screen. */
static void end_tick(void)
{
	if (!end_reason) return;

	uint32_t t    = now_ms();
	uint32_t held = t - end_since;
	const char *why = 0;

	if (held >= T_CLOSE_GRACE)
	{
		if (end_host == 1)
		{
			/* A game is alive but not streaming. Keep the last frame up rather than the
			 * menu - up to a point, so a hung emulator cannot hold the screen for ever. */
			if (held >= T_CLOSE_HOLD_CAP) why = "no stream for too long, although the host still reports a game";
		}
		else
		{
			why = (end_host == 0) ? "the host reports no game running" : "no answer from the host";
		}
	}

	if (why)
	{
		LOGP("taking the screen after %ums: %s\n", (unsigned)held, why);
		end_reason = 0;
		groovy_force_close();   /* setClose(): which raises the idle screen, so we arrive in idle_enter */
		return;
	}

	if ((int32_t)(t - end_poll_at) >= 0)
	{
		send_to(live_host, "STATUS");
		end_poll_at = t + T_CLOSE_POLL;
	}
}

void phantom_idle_poll(void)
{
	if (!inited) return;

	uint32_t t = now_ms();

	/* The network half runs even when the launcher is not on screen, so that the
	 * catalog and the host's state are already current the moment a stream ends. */
	drain();
	end_tick();

	int searching = (ui.host_state != PH_HOST_ONLINE);
	uint32_t interval = T_DISCOVER_FAST;
	if (ui.host_state == PH_HOST_OFFLINE) interval = T_DISCOVER_SLOW;

	if (searching)
	{
		if (!t_next_probe || (int32_t)(t - t_next_probe) >= 0)
		{
			probe();
			t_next_probe = t + interval;
		}
		if (ui.host_state == PH_HOST_SEARCHING && (int32_t)(t - t_search_began) > T_GIVE_UP)
		{
			ui.host_state = PH_HOST_OFFLINE;
			ui.dirty = 1;
			LOGP("no host answered in %dms\n", T_GIVE_UP);
		}
	}
	else
	{
		if (!t_next_probe || (int32_t)(t - t_next_probe) >= 0)
		{
			send_to(live_host, "PING");
			t_next_probe = t + T_PING;
		}
		if ((int32_t)(t - t_last_reply) > T_PING_LOST)
		{
			ui.host_state = PH_HOST_OFFLINE;
			ui.dirty = 1;
			LOGP("host stopped answering\n");
		}
	}

	/* The library: notice a rescan, and keep the rows around the cursor arriving. */
	if (ui.host_state == PH_HOST_ONLINE) library_tick(t);

	if (!owns_screen) return;

	/* A launch that never produced video. Saying so and going back to the list beats
	 * sitting on a splash screen forever, which is what the original's fixed delay
	 * did whenever the host was slower than the guess. */
	if (awaiting_video && (int32_t)(t - t_launch_sent) > T_LAUNCH_WAIT)
	{
		awaiting_video = 0;
		ui.view = PH_VIEW_LIST;
		snprintf(ui.status, sizeof(ui.status), "No video from host - is the emulator set up?");
		t_status_set = t;
		ui.dirty = 1;
		LOGP("launch of %s produced no video within %dms\n", last_id, T_LAUNCH_WAIT);
	}

	/* retire a transient message */
	if (ui.status[0] && ui.view != PH_VIEW_BUSY && (int32_t)(t - t_status_set) > T_STATUS_SHOW)
	{
		ui.status[0] = 0;
		ui.dirty = 1;
	}

	repeat_tick();

	/* Go quiet once a launch is out. The busy screen's sweep asks for a redraw every
	 * tick, and each one is a megabyte of uncached writes into the very DDR region the
	 * arriving video stream is about to fill - about 60MB/s of contention during the
	 * exact handover it is decorating. The screen is still drawn once, when the view
	 * changes; it simply stops animating until the host either sends video or the
	 * launch times out. A moving progress bar is not worth competing with the thing
	 * the user actually asked for. */
	if (awaiting_video)
	{
		/* Loading screen: one redraw a second, enough for the clock and the activity
		 * strip, and cheap enough not to compete with the video about to arrive. */
		if (!t_busy_tick || (int32_t)(t - t_busy_tick) >= 1000)
		{
			t_busy_tick = t;
			/* ph_ui_animate is what normally advances the clock, and it is skipped
			 * here, so the elapsed counter has to be told the time directly. */
			ui.tick_ms = t;
			ui.dirty = 1;
		}
	}
	else if (ph_ui_animate(&ui, t)) ui.dirty = 1;

	/* Frame pacing. groovy_poll() spins as fast as it can, and without this even an
	 * idle list rebuilt and recopied a megabyte thousands of times a second. Capped at
	 * the 480i field rate, which is as often as the CRT can show a new frame anyway. */
	if (ui.dirty && (int32_t)(t - t_presented) >= PH_FRAME_MS)
	{
		ph_ui_render(&ui);
		groovy_idle_present(shadow);
		t_presented = t;
	}
}

/* ---- input -------------------------------------------------------------------- */

/* The core names the pad's ten positions A, B, X, Y, L, R, Select, Start, L2, R2, in that order
 * (input.cpp, groovy_btns), a pad is mapped to them in that order, and position n arrives as bit
 * 4+n of the mask. user_io.h still carries the Minimig-era names for the first of those bits -
 * JOY_SELECT is 0x40 and JOY_START is 0x80 - but here those are X and Y. The launcher used them
 * as Select and Start, so "refresh" was really the Y button, and the exit hotkey was X and Y held
 * together: two face buttons a fighting game uses. These are the real ones. */
#define GJ_A       0x0010
#define GJ_B       0x0020
#define GJ_X       0x0040
#define GJ_Y       0x0080
#define GJ_L       0x0100
#define GJ_R       0x0200
#define GJ_SELECT  0x0400
#define GJ_START   0x0800
#define GJ_L2      0x1000
#define GJ_R2      0x2000

/* Edge detection against the previous mask: user_io_digital_joystick() is called on
 * every change, and a held direction must not free-run the list. Auto-repeat is
 * deliberate and matches an arcade stick: a pause, then acceleration. (State: see the
 * declarations at the top of the file.) */

#define PH_REP_DELAY  420
#define PH_REP_FAST   110
#define PH_REP_FLOOR   45

/* Left/Right, held: pages, then more pages at a time (see repeat_tick). */
#define REPX_DELAY      450
#define REPX_FAST       220
#define REPX_FLOOR       80
#define REPX_MAX_PAGES   40

static int nav_from(uint32_t mask, int *dx, int *dy)
{
	*dx = 0;
	*dy = 0;
	if (mask & JOY_UP)    *dy = -1;
	if (mask & JOY_DOWN)  *dy =  1;
	if (mask & JOY_LEFT)  *dx = -1;
	if (mask & JOY_RIGHT) *dx =  1;
	return (*dx || *dy);
}

int phantom_joystick(unsigned char joy, uint32_t mask)
{
	if (joy > 3) return 0;

	/* Tracked whether or not the launcher is on screen. This used to sit below the
	 * ownership check, so while a game ran it went stale: the launcher took the screen
	 * holding the pad state from the last time it had been looked at, and every button
	 * already held in the game read as freshly pressed. */
	uint32_t prev = prev_mask[joy];
	prev_mask[joy] = mask;

	if (!owns_screen) return 0;

	uint32_t pressed = mask & ~prev;
	const int adjusting = (ui.view == PH_VIEW_ADJUST);

	int dx, dy;
	int held = nav_from(mask, &dx, &dy);

	/* A fresh direction press acts now and arms its repeat. Up/Down and Left/Right are separate: holding one
	 * while tapping the other must not cancel it. */
	int pdx, pdy;
	nav_from(pressed, &pdx, &pdy);
	if (pdy)
	{
		if (adjusting) ph_ui_adjust_move(&ui, -pdy * 2);   /* up shrinks the box, down grows it */
		else           ph_ui_move(&ui, pdy);
		rep_at[joy] = now_ms() + PH_REP_DELAY;
		rep_dir[joy] = pdy;
		rep_n[joy] = 0;
	}
	if (pdx)
	{
		if (adjusting) ph_ui_adjust_edge(&ui, pdx);
		else           ph_ui_page(&ui, pdx);
		repx_at[joy] = now_ms() + REPX_DELAY;
		repx_dir[joy] = adjusting ? 0 : pdx;               /* choosing an edge does not repeat */
		repx_n[joy] = 0;
	}
	if (!dy) rep_dir[joy] = 0;
	if (!dx) repx_dir[joy] = 0;
	(void)held;

	if (adjusting)
	{
		if (pressed & GJ_A)      { ph_ui_adjust_end(&ui, 1); phantom_save_config(); }
		else if (pressed & GJ_B) { ph_ui_adjust_end(&ui, 0); }
		else if (pressed & GJ_Y) { ph_ui_adjust_reset(&ui); }
		return 1;
	}

	if (ui.view == PH_VIEW_LIST)
	{
		if (pressed & GJ_L)  ph_ui_tab(&ui, -1);
		if (pressed & GJ_R)  ph_ui_tab(&ui,  1);
		if (pressed & GJ_L2) ph_ui_letter(&ui, -1);
		if (pressed & GJ_R2) ph_ui_letter(&ui,  1);
	}

	if (pressed & GJ_A)
	{
		if (ui.view == PH_VIEW_BUSY)
		{
			/* let the user back out of a launch that is going nowhere */
			ui.view = PH_VIEW_LIST;
			awaiting_video = 0;
			ui.dirty = 1;
		}
		else
		{
			try_launch();
		}
		return 1;
	}

	if (pressed & GJ_Y)
	{
		phantom_request_refresh();
		return 1;
	}

	if ((pressed & GJ_SELECT) && ui.view == PH_VIEW_LIST)
	{
		ph_ui_adjust_begin(&ui);
		return 1;
	}

	if (pressed & GJ_B)
	{
		if (ui.view == PH_VIEW_BUSY)
		{
			ui.view = PH_VIEW_LIST;
			awaiting_video = 0;
			ui.dirty = 1;
		}
		return 1;
	}

	/* Every event is consumed while the launcher is up: if any of it also reached the
	 * host, browsing the menu would be typing into whatever the PC has focused. */
	return 1;
}

/* Called from phantom_idle_poll via the animate path so a held stick keeps moving
 * without needing new input events. */
static void repeat_tick(void)
{
	uint32_t t = now_ms();
	for (int j = 0; j < 4; j++)
	{
		if (rep_dir[j] && (int32_t)(t - rep_at[j]) >= 0)
		{
			if (ui.view == PH_VIEW_ADJUST) ph_ui_adjust_move(&ui, -rep_dir[j] * 2);
			else                           ph_ui_move(&ui, rep_dir[j]);
			rep_n[j]++;
			uint32_t gap = PH_REP_FAST - (uint32_t)rep_n[j] * 6;
			if ((int32_t)gap < PH_REP_FLOOR) gap = PH_REP_FLOOR;
			rep_at[j] = t + gap;
		}

		if (repx_dir[j] && ui.view == PH_VIEW_LIST && (int32_t)(t - repx_at[j]) >= 0)
		{
			/* Holding Left/Right pages, and the longer it is held the further each step goes: a page, then
			 * several, up to REPX_MAX_PAGES. Across a library of hundreds of thousands a page at a time is a
			 * long afternoon; this crosses a letter's worth in a couple of seconds and still lets go within a
			 * page of where you meant. */
			int pages = 1 + repx_n[j] / 2;
			if (pages > REPX_MAX_PAGES) pages = REPX_MAX_PAGES;
			ph_ui_seek(&ui, repx_dir[j] * pages * ph_ui_rows(&ui));
			repx_n[j]++;
			int gap = REPX_FAST - repx_n[j] * 10;
			if (gap < REPX_FLOOR) gap = REPX_FLOOR;
			repx_at[j] = t + (uint32_t)gap;
		}
	}
}

int phantom_keyboard(uint16_t key, int press)
{
	if (!owns_screen) return 0;
	if (!press) return 1;

	/* These are Linux input event codes, not HID usages: input.cpp reads /dev/input
	 * and passes ev->code straight through (user_io_kbd(ev->code, ev->value)). The
	 * translation to PS/2 happens after this point, in get_ps2_code(). */
	if (ui.view == PH_VIEW_ADJUST)
	{
		switch (key)
		{
		case KEY_LEFT:  ph_ui_adjust_edge(&ui, -1); break;
		case KEY_RIGHT: ph_ui_adjust_edge(&ui,  1); break;
		case KEY_UP:    ph_ui_adjust_move(&ui,  2); break;
		case KEY_DOWN:  ph_ui_adjust_move(&ui, -2); break;
		case KEY_ENTER:
		case KEY_KPENTER:
		case KEY_SPACE: ph_ui_adjust_end(&ui, 1); phantom_save_config(); break;
		case KEY_ESC:
		case KEY_BACKSPACE: ph_ui_adjust_end(&ui, 0); break;
		case KEY_R:     ph_ui_adjust_reset(&ui); break;
		default: break;
		}
		return 1;
	}

	switch (key)
	{
	case KEY_UP:
	case KEY_W:     ph_ui_move(&ui, -1); break;
	case KEY_DOWN:
	case KEY_S:     ph_ui_move(&ui,  1); break;
	case KEY_LEFT:
	case KEY_PAGEUP:   ph_ui_page(&ui, -1); break;
	case KEY_RIGHT:
	case KEY_PAGEDOWN: ph_ui_page(&ui,  1); break;
	case KEY_A:     ph_ui_page(&ui, -1); break;
	case KEY_D:     ph_ui_page(&ui,  1); break;
	case KEY_Q:     ph_ui_tab(&ui, -1); break;
	case KEY_E:
	case KEY_TAB:   ph_ui_tab(&ui,  1); break;
	case KEY_COMMA: ph_ui_letter(&ui, -1); break;
	case KEY_DOT:   ph_ui_letter(&ui,  1); break;

	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_SPACE: try_launch(); break;

	case KEY_ESC:
	case KEY_BACKSPACE:
		if (ui.view == PH_VIEW_BUSY) { ui.view = PH_VIEW_LIST; awaiting_video = 0; ui.dirty = 1; }
		break;

	case KEY_F2:
		if (ui.view == PH_VIEW_LIST) ph_ui_adjust_begin(&ui);
		break;

	case KEY_F5:
		phantom_request_refresh();
		break;

	default: break;
	}
	return 1;
}

int phantom_exit_hotkey(unsigned char joy, uint32_t mask)
{
	if (!exit_hotkey || owns_screen) return 0;
	if (joy > 3) return 0;

	/* Start+Select held together. The hold is what keeps this from firing during a
	 * game that legitimately uses both buttons. */
	int combo = (mask & GJ_START) && (mask & GJ_SELECT);
	uint32_t t = now_ms();

	if (!combo)
	{
		hk_since = 0;
		hk_fired = 0;
		return 0;
	}
	if (!hk_since)
	{
		hk_since = t;
		return 0;
	}
	if (!hk_fired && (int32_t)(t - hk_since) >= T_EXIT_HOLD)
	{
		hk_fired = 1;
		phantom_return_to_launcher();
		return 1;
	}
	return 0;
}

/* ---- OSD accessors ------------------------------------------------------------ */

int phantom_get_idle_mode(void) { return idle_mode; }

void phantom_set_idle_mode(int mode)
{
	if (mode < PHANTOM_IDLE_LAUNCHER || mode > PHANTOM_IDLE_OFF) mode = PHANTOM_IDLE_LAUNCHER;
	idle_mode = mode;
	phantom_save_config();
}

int phantom_get_exit_hotkey(void) { return exit_hotkey; }

void phantom_set_exit_hotkey(int on)
{
	exit_hotkey = on ? 1 : 0;
	phantom_save_config();
}

const char *phantom_host_text(void)
{
	static char s[96];
	if (ui.host_state == PH_HOST_ONLINE) snprintf(s, sizeof(s), "%s:%d", live_host, cfg_port);
	else if (cfg_host[0])                snprintf(s, sizeof(s), "%s:%d (set)", cfg_host, cfg_port);
	else                                 snprintf(s, sizeof(s), "auto-discover :%d", cfg_port);
	return s;
}

const char *phantom_state_text(void)
{
	switch (ui.host_state)
	{
	case PH_HOST_ONLINE:  return "online";
	case PH_HOST_OFFLINE: return "not found";
	default:              return "searching";
	}
}

int phantom_title_count(void) { return cat_paged ? cat.total : ui.count; }
