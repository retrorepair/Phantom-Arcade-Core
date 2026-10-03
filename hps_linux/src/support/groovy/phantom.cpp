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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>   /* KEY_* - phantom_keyboard sees raw event codes */

#include "phantom.h"
#include "phantom_ui.h"
#include "../../user_io.h"

/* Hardware side, implemented in groovy.cpp. The launcher never touches SPI or the DDR
 * header itself; it asks for a canvas, draws, and asks for it to be shown. */
extern "C" {
uint32_t groovy_idle_canvas_bytes(void);
int      groovy_idle_begin(void);
void     groovy_idle_present(const uint8_t *src);
void     groovy_idle_end(void);
}

#define PH_INI       "/media/fat/config/phantom.ini"
#define PH_CACHE     "/media/fat/config/phantom_catalog.tsv"

#define PH_DEF_PORT  1999

/* Timings, all milliseconds. */
#define T_DISCOVER_FAST  1500   /* while searching */
#define T_DISCOVER_SLOW 10000   /* after giving up, so a late host still gets found */
#define T_GIVE_UP       12000   /* searching -> offline */
#define T_PING           5000   /* liveness probe while online */
#define T_PING_LOST     16000   /* no reply for this long -> offline */
#define T_LAUNCH_WAIT   25000   /* no video after a LAUNCH -> back to the list */
#define T_EXIT_HOLD      1200   /* Start+Select hold to kill the host emulator */
#define T_STATUS_SHOW    6000   /* how long a transient message sits in the detail bar */

/* One 480i field is ~16.7ms; there is no point building frames faster than the CRT
 * can show them, and every frame costs a full-canvas copy into DDR. */
#define PH_FRAME_MS        16

#define LOGP(...) do { printf("[PHANTOM] " __VA_ARGS__); fflush(stdout); } while (0)

static ph_ui     ui;
static uint8_t  *shadow      = 0;   /* cached-RAM canvas; see groovy_idle_present */
static uint32_t  t_presented = 0;
static int       inited      = 0;
static int       owns_screen = 0;
static int       idle_mode   = PHANTOM_IDLE_LAUNCHER;
static int       exit_hotkey = 1;

static int       sock = -1;
static int       cfg_port = PH_DEF_PORT;
static char      cfg_host[64] = "";      /* optional manual override */
static char      live_host[64] = "";     /* whoever actually answered */

static uint32_t  t_next_probe = 0;
static uint32_t  t_search_began = 0;
static uint32_t  t_last_reply = 0;
static uint32_t  t_launch_sent = 0;
static uint32_t  t_status_set = 0;   /* transient message in the detail bar */
static int       awaiting_video = 0;
static char      last_id[PH_ID_LEN] = "";

/* Exit-hotkey edge tracking, live only while a stream is running. */
static uint32_t  hk_since = 0;
static int       hk_fired = 0;

static void repeat_tick(void);   /* defined with the rest of the input handling */

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
	}
	fclose(f);
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
	fclose(f);
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
		fprintf(f, "%s\t%s\t%s\t%s\t%s\n", g->id, g->title, g->system, g->sysname, g->mode);
	}
	fclose(f);
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

		char *fld[5] = { 0 };
		int n = 0;
		char *p = line;
		while (n < 5 && p)
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
	}
	fclose(f);
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

	memcpy(ui.games, tmp, sizeof(ph_game) * (size_t)n);
	ui.count = n;
	ph_ui_reclamp(&ui, keep);
	cache_save();
	LOGP("catalog: %d titles from %s\n", n, live_host);
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
				send_to(live_host, "GET_CATALOG");
			}
		}
		else if (!strncmp(buf, "ACK:LAUNCH", 10))
		{
			LOGP("host acked launch\n");
			snprintf(ui.status, sizeof(ui.status), "Host is starting the emulator...");
			ui.dirty = 1;
		}
		else if (!strncmp(buf, "ACK:KILL", 8))
		{
			LOGP("host acked kill\n");
		}
		else if (n > 8 && (buf[0] == '{' || strstr(buf, "\"id\"")))
		{
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

	char msg[160];
	snprintf(msg, sizeof(msg), "LAUNCH:%s", g->id);
	send_to(live_host, msg);

	snprintf(last_id, sizeof(last_id), "%s", g->id);
	snprintf(ui.busy_title, sizeof(ui.busy_title), "%s", g->title);
	snprintf(ui.status, sizeof(ui.status), "Waiting for host video...");
	ui.view = PH_VIEW_BUSY;
	ui.dirty = 1;

	awaiting_video = 1;
	t_launch_sent = now_ms();
	LOGP("launch %s\n", g->id);
}

void phantom_send_kill(void)
{
	if (live_host[0]) send_to(live_host, "KILL");
	LOGP("kill sent\n");
}

void phantom_request_refresh(void)
{
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
	snprintf(ui.host_ip, sizeof(ui.host_ip), "%s", cfg_host);
	ui.host_port = cfg_port;
	ui.host_state = PH_HOST_SEARCHING;

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
	groovy_idle_end();
	LOGP("host video started, launcher released the screen\n");
}

int phantom_owns_screen(void)
{
	return owns_screen;
}

void phantom_idle_poll(void)
{
	if (!inited) return;

	uint32_t t = now_ms();

	/* The network half runs even when the launcher is not on screen, so that the
	 * catalog and the host's state are already current the moment a stream ends. */
	drain();

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
	if (!awaiting_video && ph_ui_animate(&ui, t)) ui.dirty = 1;

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

/* Edge detection against the previous mask: user_io_digital_joystick() is called on
 * every change, and a held direction must not free-run the list. Auto-repeat is
 * deliberate and matches an arcade stick: a pause, then acceleration. */
static uint32_t prev_mask[4];
static uint32_t rep_at[4];
static int      rep_dir[4];
static int      rep_n[4];

#define REP_DELAY  420
#define REP_FAST   110
#define REP_FLOOR   45

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
	if (!owns_screen) return 0;
	if (joy > 3) return 0;

	uint32_t prev = prev_mask[joy];
	prev_mask[joy] = mask;
	uint32_t pressed = mask & ~prev;

	int dx, dy;
	int held = nav_from(mask, &dx, &dy);

	/* fresh direction press -> act now, arm the repeat */
	int pdx, pdy;
	if (nav_from(pressed, &pdx, &pdy))
	{
		if (pdy) ph_ui_move(&ui, pdy);
		if (pdx) ph_ui_tab(&ui, pdx);
		rep_at[joy] = now_ms() + REP_DELAY;
		rep_dir[joy] = pdy ? pdy : 0;
		rep_n[joy] = 0;
	}
	else if (!held)
	{
		rep_dir[joy] = 0;
	}

	if (pressed & JOY_BTN1)
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
			do_launch();
		}
		return 1;
	}

	/* JOY_START is JOY_BTN4 (user_io.h), so this is the one Start/BTN4 binding. */
	if (pressed & JOY_START)
	{
		phantom_request_refresh();
		if (live_host[0]) send_to(live_host, "GET_CATALOG");
		return 1;
	}

	if (pressed & JOY_BTN2)
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
		if (!rep_dir[j]) continue;
		if ((int32_t)(t - rep_at[j]) < 0) continue;

		ph_ui_move(&ui, rep_dir[j]);
		rep_n[j]++;
		uint32_t gap = REP_FAST - (uint32_t)rep_n[j] * 6;
		if ((int32_t)gap < REP_FLOOR) gap = REP_FLOOR;
		rep_at[j] = t + gap;
	}
}

int phantom_keyboard(uint16_t key, int press)
{
	if (!owns_screen) return 0;
	if (!press) return 1;

	/* These are Linux input event codes, not HID usages: input.cpp reads /dev/input
	 * and passes ev->code straight through (user_io_kbd(ev->code, ev->value)). The
	 * translation to PS/2 happens after this point, in get_ps2_code(). */
	switch (key)
	{
	case KEY_UP:
	case KEY_W:     ph_ui_move(&ui, -1); break;
	case KEY_DOWN:
	case KEY_S:     ph_ui_move(&ui,  1); break;
	case KEY_LEFT:
	case KEY_A:     ph_ui_tab(&ui, -1); break;
	case KEY_RIGHT:
	case KEY_D:
	case KEY_TAB:   ph_ui_tab(&ui,  1); break;

	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_SPACE: do_launch(); break;

	case KEY_ESC:
	case KEY_BACKSPACE:
		if (ui.view == PH_VIEW_BUSY) { ui.view = PH_VIEW_LIST; awaiting_video = 0; ui.dirty = 1; }
		break;

	case KEY_F5:
		phantom_request_refresh();
		if (live_host[0]) send_to(live_host, "GET_CATALOG");
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
	int combo = (mask & JOY_START) && (mask & JOY_SELECT);
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
		phantom_send_kill();
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

int phantom_title_count(void) { return ui.count; }
