/*
 * phantom_preview.c - render the Phantom Arcade launcher on the build machine.
 *
 * phantom_ui.c has no hardware or socket dependencies, so the exact code that runs on
 * the DE10-Nano can be compiled natively and dumped to a PPM. That makes the layout
 * reviewable without a cabinet in the loop, which is the only practical way to iterate
 * on a 720x480 interlaced screen.
 *
 *   gcc -O2 -I../hps_linux/src/support/groovy -o phantom_preview phantom_preview.c \
 *       ../hps_linux/src/support/groovy/phantom_ui.c
 *   ./phantom_preview out.ppm [catalog.tsv] [--busy] [--empty] [--offline] [--tab N]
 *       [--big N [--have LO,HI]] [--scanning] [--loading]
 *       [--bg picture.ppm [--dim PERCENT] [--panels ALPHA] [--list ALPHA]] [--settings [--setsel N]]
 *
 * --bg puts a 720x480 P6 picture behind the whole launcher, dimmed to --dim percent (default 50), with the
 * panels drawn over it at --panels opacity (0..255, default 200) and the game list at --list (default 102: 60% see-through).
 *
 * --big N shows a library of N titles the way a host-backed list does (no array behind it); --have LO,HI
 * says which rows have arrived, the rest drawing as placeholders.
 *
 * The catalog file is TSV: id, title, system, sysname, hsync_hz, vsync_centihz.
 * With no file a small fixture is used. That fixture is test data for looking at the
 * layout and is NOT what the core shows when no host is present - the core shows the
 * "no host found" state, because inventing a library the user does not own is exactly
 * the behaviour this integration set out to remove.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phantom_ui.h"

static ph_ui ui;
static uint8_t fb[PH_CANVAS_W * PH_CANVAS_H * 3];

static void add(const char *id, const char *title, const char *sys, const char *sysname,
                uint32_t hz, uint32_t chz)
{
	if (ui.count >= PH_MAX_GAMES) return;
	ph_game *g = &ui.games[ui.count++];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "%s", id);
	snprintf(g->title, sizeof(g->title), "%s", title);
	snprintf(g->system, sizeof(g->system), "%s", sys);
	snprintf(g->sysname, sizeof(g->sysname), "%s", sysname);
	g->hsync_hz = hz;
	g->vsync_chz = chz;
}

static void fixture(void)
{
	add("kinst", "Killer Instinct (v1.5, USA)", "groovymame", "Midway", 15734, 6000);
	add("sfiii3", "Street Fighter III: 3rd Strike (Euro 990512)", "groovymame", "Capcom CPS-3", 15600, 5963);
	add("umk3", "Ultimate Mortal Kombat 3", "groovymame", "Midway Wolf", 15200, 5471);
	add("garou", "Garou: Mark of the Wolves", "groovymame", "SNK Neo-Geo", 15625, 5918);
	add("mslug3", "Metal Slug 3", "groovymame", "SNK Neo-Geo", 15625, 5918);
	add("mvsc", "Marvel vs. Capcom", "groovymame", "Capcom CPS-2", 15600, 5963);
	add("dkong", "Donkey Kong", "groovymame", "Nintendo", 15441, 6061);
	add("vf4ft", "Virtua Fighter 4 Final Tuned", "flycast", "Sega NAOMI 2", 15734, 6000);
	add("cvs2", "Capcom vs. SNK 2", "flycast", "Sega NAOMI", 15734, 6000);
	add("ikaruga", "Ikaruga", "flycast", "Sega NAOMI", 15734, 6000);
	add("sf3aniv", "Street Fighter III Anniversary", "pcsx2", "PlayStation 2", 15734, 6000);
	add("arcana", "Arcana Heart", "pcsx2", "PlayStation 2", 15734, 6000);
	add("melee", "Super Smash Bros. Melee", "dolphin", "GameCube", 15734, 6000);
	add("fzerogx", "F-Zero GX", "dolphin", "GameCube", 15734, 6000);
}

static int  big_n = 0, have_lo = 0, have_hi = 1 << 30;
static ph_game big_ring[16];
static int  big_ring_i;

static int big_count(void *ctx, int tab)
{
	(void)ctx;
	return tab == PH_TAB_ALL ? big_n : 0;
}

static const ph_game *big_row(void *ctx, int tab, int i)
{
	(void)ctx;
	if (tab != PH_TAB_ALL || i < 0 || i >= big_n || i < have_lo || i >= have_hi) return 0;
	ph_game *g = &big_ring[big_ring_i++ & 15];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "big_%d", i);
	snprintf(g->title, sizeof(g->title), "%c%c Example Title Number %d", 'A' + (int)((long)i * 26 / big_n), 'a' + (i * 7) % 26, i);
	snprintf(g->system, sizeof(g->system), "pcsx2");
	snprintf(g->sysname, sizeof(g->sysname), "Sony PlayStation 2");
	snprintf(g->year, sizeof(g->year), "%d", 1990 + i % 30);
	snprintf(g->maker, sizeof(g->maker), "Some Maker Co.");
	snprintf(g->mode, sizeof(g->mode), "320x240 @ 60.00Hz");
	return g;
}

static int big_letter(void *ctx, int tab, int k)
{
	(void)ctx; (void)tab;
	return k >= 27 ? big_n : k == 0 ? 0 : (int)(((long)(k - 1) * big_n + 25) / 26);
}

/* A 720x480 binary PPM, as BGR888 in dst. */
static int load_ppm_bgr(const char *path, uint8_t *dst)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	int w = 0, h = 0, mx = 0;
	if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != PH_CANVAS_W || h != PH_CANVAS_H || mx != 255)
	{
		fclose(f);
		return 0;
	}
	fgetc(f);   /* the single whitespace after the header */
	size_t n = (size_t)w * h * 3;
	size_t got = fread(dst, 1, n, f);
	fclose(f);
	if (got != n) return 0;
	for (size_t i = 0; i < n; i += 3) { uint8_t r = dst[i]; dst[i] = dst[i + 2]; dst[i + 2] = r; }
	return 1;
}

static void load_tsv(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		if (line[0] == '#' || line[0] == '\n') continue;
		char *fld[6] = {0};
		int n = 0;
		char *p = line;
		while (n < 6 && p)
		{
			fld[n++] = p;
			char *t = strchr(p, '\t');
			if (!t) break;
			*t = 0;
			p = t + 1;
		}
		for (int i = 0; i < n; i++)
		{
			char *nl = strpbrk(fld[i], "\r\n");
			if (nl) *nl = 0;
		}
		if (n < 3) continue;
		add(fld[0], fld[1], fld[2], n > 3 ? fld[3] : "",
		    n > 4 ? (uint32_t)strtoul(fld[4], 0, 10) : 0,
		    n > 5 ? (uint32_t)strtoul(fld[5], 0, 10) : 0);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "phantom_preview.ppm";
	const char *tsv = 0;
	int busy = 0, empty = 0, offline = 0, tab = 0, sel = 0;
	int settings = 0, setsel = 0;
	int adjust = 0, edge = 1, sl = -1, st = -1, sr = -1, sb = -1, scanning = 0, loading = 0;
	const char *bgfile = 0;
	int dim = 50, panels = PH_BG_PANEL_ALPHA, listalpha = PH_BG_LIST_ALPHA;
	uint32_t t = 0;

	for (int i = 2; i < argc; i++)
	{
		if (!strcmp(argv[i], "--busy")) busy = 1;
		else if (!strcmp(argv[i], "--empty")) empty = 1;
		else if (!strcmp(argv[i], "--offline")) offline = 1;
		else if (!strcmp(argv[i], "--tab") && i + 1 < argc) tab = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--sel") && i + 1 < argc) sel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--time") && i + 1 < argc) t = (uint32_t)strtoul(argv[++i], 0, 10);
		else if (!strcmp(argv[i], "--adjust")) adjust = 1;
		else if (!strcmp(argv[i], "--settings")) settings = 1;
		else if (!strcmp(argv[i], "--setsel") && i + 1 < argc) setsel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--big") && i + 1 < argc) big_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--have") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &have_lo, &have_hi);
		else if (!strcmp(argv[i], "--bg") && i + 1 < argc) bgfile = argv[++i];
		else if (!strcmp(argv[i], "--dim") && i + 1 < argc) dim = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--panels") && i + 1 < argc) panels = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--list") && i + 1 < argc) listalpha = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scanning")) scanning = 1;
		else if (!strcmp(argv[i], "--loading")) loading = 1;
		else if (!strcmp(argv[i], "--edge") && i + 1 < argc) edge = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--safe") && i + 1 < argc) sscanf(argv[++i], "%d,%d,%d,%d", &sl, &st, &sr, &sb);
		else if (argv[i][0] != '-') tsv = argv[i];
	}

	ph_ui_init(&ui, fb, PH_CANVAS_W, PH_CANVAS_H);
	snprintf(ui.host_ip, sizeof(ui.host_ip), "192.168.1.126");
	ui.host_port = 1999;
	ui.host_state = offline ? PH_HOST_OFFLINE : PH_HOST_ONLINE;

	if (big_n > 0)
	{
		ui.src.count = big_count;
		ui.src.row = big_row;
		ui.src.letter_start = big_letter;
	}
	else if (!empty)
	{
		if (tsv) load_tsv(tsv); else fixture();
	}
	ui.lib_scanning = scanning;
	ui.lib_loading = loading;

	static uint8_t bg_raw[PH_CANVAS_W * PH_CANVAS_H * 3], bg_dim[PH_CANVAS_W * PH_CANVAS_H * 3];
	if (bgfile)
	{
		if (!load_ppm_bgr(bgfile, bg_raw)) { fprintf(stderr, "cannot read a 720x480 P6 picture from %s\n", bgfile); return 1; }
		ph_ui_dim(bg_dim, bg_raw, sizeof(bg_dim), dim);
		ui.bg = bg_dim;
		ui.bg_alpha = panels;
		ui.bg_list_alpha = listalpha;
	}

	ui.tab = tab;
	ui.sel = sel;
	if (sl >= 0) ph_ui_set_safe(&ui, sl, st, sr, sb);
	if (adjust) { ph_ui_adjust_begin(&ui); ui.adj_edge = edge; }
	ph_ui_reclamp(&ui, 0);
	ph_ui_animate(&ui, t);

	if (settings)
	{
		/* the lines the cabinet's menu has, with plausible values */
		static const struct { const char *l, *v; int k; } rows[] = {
			{ "Picture behind the menu", "Title screens", PH_SET_VALUE },
			{ "Download pictures", "Both", PH_SET_VALUE },
			{ "Download for every game", "Start", PH_SET_ACTION },
			{ "Picture brightness", "50%", PH_SET_VALUE },
			{ "Game list see-through", "60%", PH_SET_VALUE },
			{ "Screen edges", "Adjust...", PH_SET_ACTION },
			{ "Pictures on the PC", "1210 titles, 1198 screenshots", PH_SET_INFO },
			{ "Pictures come from", "thumbnails.libretro.com", PH_SET_INFO },
		};
		for (int i = 0; i < 8; i++)
		{
			snprintf(ui.set_rows[i].label, sizeof(ui.set_rows[i].label), "%s", rows[i].l);
			snprintf(ui.set_rows[i].value, sizeof(ui.set_rows[i].value), "%s", rows[i].v);
			ui.set_rows[i].kind = rows[i].k;
		}
		ui.set_count = 8;
		snprintf(ui.set_note, sizeof(ui.set_note), "%s", "Which picture sits behind the menu: the game's title screen or a screenshot of it.");
		ph_ui_settings_begin(&ui);
		if (setsel > 0) ui.set_sel = setsel;
	}

	if (busy)
	{
		ui.view = PH_VIEW_BUSY;
		const ph_game *g = ph_ui_selected(&ui);
		snprintf(ui.busy_title, sizeof(ui.busy_title), "%s", g ? g->title : "-");
		snprintf(ui.status, sizeof(ui.status), "Waiting for host video...");
		/* --time is how long the launch has been waiting, so the elapsed clock and
		 * the activity strip can be looked at at any point in the wait. */
		ui.busy_t0 = 1;
		ui.tick_ms = 1 + t;
	}

	ph_ui_render(&ui);

	FILE *f = fopen(out, "wb");
	if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
	fprintf(f, "P6\n%d %d\n255\n", PH_CANVAS_W, PH_CANVAS_H);
	/* framebuffer is B,G,R; PPM wants R,G,B */
	for (long i = 0; i < (long)PH_CANVAS_W * PH_CANVAS_H; i++)
	{
		fputc(fb[i * 3 + 2], f);
		fputc(fb[i * 3 + 1], f);
		fputc(fb[i * 3 + 0], f);
	}
	fclose(f);
	fprintf(stderr, "wrote %s (%dx%d), %d titles, tab %d\n", out, PH_CANVAS_W, PH_CANVAS_H, ui.count, ui.tab);
	return 0;
}
