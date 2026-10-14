/*
 * phantom_render_bench.c - what a frame of the launcher costs, flat and over a picture.
 *
 * The picture mode blends every panel with what is already on the canvas, which is more work than flat fills, and
 * the HPS's A9 shares its attention with the video stream. This renders the real phantom_ui.c a few hundred times and
 * reports milliseconds per frame, so the cost is a number rather than a worry. Build it for whatever it will run on:
 *
 *   gcc -O3 -I../hps_linux/src/support/groovy -o bench phantom_render_bench.c ../hps_linux/src/support/groovy/phantom_ui.c
 *   arm-none-linux-gnueabihf-gcc -O3 -mcpu=cortex-a9 -mfpu=neon ...   (the cabinet's flags; see hps_linux/src/Makefile)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "phantom_ui.h"

static ph_ui ui;
static uint8_t fb[PH_CANVAS_W * PH_CANVAS_H * 3];
static uint8_t pic[PH_CANVAS_W * PH_CANVAS_H * 3], dim[PH_CANVAS_W * PH_CANVAS_H * 3];

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double time_frames(int n)
{
	double t0 = now_ms();
	for (int i = 0; i < n; i++)
	{
		ui.dirty = 1;
		ui.sel = i % 50;
		ph_ui_render(&ui);
	}
	return (now_ms() - t0) / n;
}

int main(void)
{
	ph_ui_init(&ui, fb, PH_CANVAS_W, PH_CANVAS_H);
	ui.host_state = PH_HOST_ONLINE;
	for (int i = 0; i < 300; i++)
	{
		ph_game *g = &ui.games[ui.count++];
		memset(g, 0, sizeof(*g));
		snprintf(g->id, sizeof(g->id), "g%d", i);
		snprintf(g->title, sizeof(g->title), "Example Game Title Number %d (World)", i);
		snprintf(g->system, sizeof(g->system), "groovymame");
		snprintf(g->sysname, sizeof(g->sysname), "GroovyMAME Arcade");
	}

	/* something with detail in it, so the blend is not working on a flat colour */
	for (size_t i = 0; i < sizeof(pic); i++) pic[i] = (uint8_t)((i * 2654435761u) >> 24);
	ph_ui_dim(dim, pic, sizeof(dim), 50);

	time_frames(20);   /* warm the caches */
	double flat = time_frames(300);
	ui.bg = dim;
	ui.bg_alpha = PH_BG_PANEL_ALPHA;
	ui.bg_list_alpha = PH_BG_LIST_ALPHA;
	time_frames(20);
	double over = time_frames(300);

	printf("flat background:      %.2f ms/frame\n", flat);
	printf("over a picture:       %.2f ms/frame  (%.1fx)\n", over, over / flat);
	printf("the 480i field period is 16.7 ms\n");
	return 0;
}
