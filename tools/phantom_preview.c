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
	uint32_t t = 0;

	for (int i = 2; i < argc; i++)
	{
		if (!strcmp(argv[i], "--busy")) busy = 1;
		else if (!strcmp(argv[i], "--empty")) empty = 1;
		else if (!strcmp(argv[i], "--offline")) offline = 1;
		else if (!strcmp(argv[i], "--tab") && i + 1 < argc) tab = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--sel") && i + 1 < argc) sel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--time") && i + 1 < argc) t = (uint32_t)strtoul(argv[++i], 0, 10);
		else if (argv[i][0] != '-') tsv = argv[i];
	}

	ph_ui_init(&ui, fb, PH_CANVAS_W, PH_CANVAS_H);
	snprintf(ui.host_ip, sizeof(ui.host_ip), "192.168.1.126");
	ui.host_port = 1999;
	ui.host_state = offline ? PH_HOST_OFFLINE : PH_HOST_ONLINE;

	if (!empty)
	{
		if (tsv) load_tsv(tsv); else fixture();
	}

	ui.tab = tab;
	ui.sel = sel;
	ph_ui_reclamp(&ui, 0);
	ph_ui_animate(&ui, t);

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
