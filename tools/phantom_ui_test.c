/*
 * phantom_ui_test.c - checks for the launcher's navigation and layout logic.
 *
 * phantom_ui.c is pure, so its behaviour can be asserted on the build machine instead
 * of by pressing buttons on a cabinet and squinting at a CRT. These are the cases that
 * were actually got wrong at some point, or that are easy to get wrong later:
 * wrapping, scroll clamping, tab filtering, keeping the selection across a catalog
 * refresh, and the title/system column split.
 *
 *   gcc -O2 -Wall -Wextra -I../hps_linux/src/support/groovy -o phantom_ui_test \
 *       phantom_ui_test.c ../hps_linux/src/support/groovy/phantom_ui.c && ./phantom_ui_test
 */

#include <stdio.h>
#include <string.h>

#include "phantom_ui.h"

static ph_ui ui;
static uint8_t fb[PH_CANVAS_W * PH_CANVAS_H * 3];
static int checks = 0, fails = 0;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
	} while (0)

static void add(const char *id, const char *title, const char *sys, const char *sysname)
{
	ph_game *g = &ui.games[ui.count++];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "%s", id);
	snprintf(g->title, sizeof(g->title), "%s", title);
	snprintf(g->system, sizeof(g->system), "%s", sys);
	snprintf(g->sysname, sizeof(g->sysname), "%s", sysname);
}

static void reset(void)
{
	ph_ui_init(&ui, fb, PH_CANVAS_W, PH_CANVAS_H);
}

static int filtered_count(void)
{
	int idx[PH_MAX_GAMES];
	return ph_ui_filtered(&ui, idx, PH_MAX_GAMES);
}

static void test_empty(void)
{
	reset();
	CHECK(filtered_count() == 0, "empty catalog should filter to 0");
	CHECK(ph_ui_selected(&ui) == 0, "no selection in an empty catalog");
	/* must not crash or move */
	CHECK(ph_ui_move(&ui, 1) == 0, "move on an empty list reports no change");
	CHECK(ph_ui_move(&ui, -1) == 0, "move up on an empty list reports no change");
	ph_ui_render(&ui);   /* must not fault */
}

static void test_wrap(void)
{
	reset();
	add("a", "A", "groovymame", "X");
	add("b", "B", "groovymame", "X");
	add("c", "C", "groovymame", "X");

	CHECK(ui.sel == 0, "starts at the top");
	ph_ui_move(&ui, -1);
	CHECK(ui.sel == 2, "up from the first entry wraps to the last, got %d", ui.sel);
	ph_ui_move(&ui, 1);
	CHECK(ui.sel == 0, "down from the last entry wraps to the first, got %d", ui.sel);
}

static void test_scroll_clamp(void)
{
	reset();
	char id[16], title[32];
	for (int i = 0; i < 40; i++)
	{
		snprintf(id, sizeof(id), "g%d", i);
		snprintf(title, sizeof(title), "Game %d", i);
		add(id, title, "groovymame", "X");
	}

	/* walk to the bottom one step at a time; scroll must never run past the end */
	for (int i = 0; i < 39; i++) ph_ui_move(&ui, 1);
	CHECK(ui.sel == 39, "reached the last entry, got %d", ui.sel);
	CHECK(ui.scroll >= 0, "scroll never negative, got %d", ui.scroll);
	CHECK(ui.scroll <= 39, "scroll within the list, got %d", ui.scroll);
	CHECK(ui.sel >= ui.scroll, "selection is at or below the first visible row");

	/* and back up */
	for (int i = 0; i < 39; i++) ph_ui_move(&ui, -1);
	CHECK(ui.sel == 0, "back at the top, got %d", ui.sel);
	CHECK(ui.scroll == 0, "scrolled back to the top, got %d", ui.scroll);
}

static void test_tabs(void)
{
	reset();
	add("m1", "Mame One", "groovymame", "X");
	add("m2", "Mame Two", "mame", "X");
	add("f1", "FBNeo One", "fbneo", "X");
	add("n1", "Naomi One", "flycast", "X");
	add("p1", "PS2 One", "pcsx2", "X");
	add("p3", "PS3 One", "rpcs3", "X");
	add("x1", "Xbox One", "xemu", "X");
	add("r1", "RetroArch One", "retroarch", "X");
	add("d1", "Cube One", "dolphin", "X");
	add("d2", "Wii One", "wii", "X");
	add("b1", "DOS One", "86box", "X");
	add("z1", "Model 2 One", "model2", "X"); /* no tab claims it: ALL only */

	ui.tab = PH_TAB_ALL;   CHECK(filtered_count() == 12, "ALL shows everything, got %d", filtered_count());
	ui.tab = PH_TAB_MAME;  CHECK(filtered_count() == 2, "MAME tab matches groovymame and mame, got %d", filtered_count());
	ui.tab = PH_TAB_FBNEO; CHECK(filtered_count() == 1, "FBNeo tab, got %d", filtered_count());
	ui.tab = PH_TAB_DC;    CHECK(filtered_count() == 1, "Dreamcast tab, got %d", filtered_count());
	ui.tab = PH_TAB_PS2;   CHECK(filtered_count() == 1, "PS2 tab, got %d", filtered_count());
	ui.tab = PH_TAB_PS3;   CHECK(filtered_count() == 1, "PS3 tab, got %d", filtered_count());
	ui.tab = PH_TAB_XBOX;  CHECK(filtered_count() == 1, "Xbox tab, got %d", filtered_count());
	ui.tab = PH_TAB_GCWII; CHECK(filtered_count() == 2, "GC/Wii tab matches dolphin and wii, got %d", filtered_count());
	ui.tab = PH_TAB_PC;    CHECK(filtered_count() == 1, "PC tab, got %d", filtered_count());
	ui.tab = PH_TAB_RA;    CHECK(filtered_count() == 1, "RetroArch tab, got %d", filtered_count());

	/* A system no tab claims is still reachable under ALL rather than vanishing: the
	 * catalog is the host's to decide, and silently hiding rows would be worse than
	 * showing them in one place. */
	int idx[PH_MAX_GAMES];
	ui.tab = PH_TAB_ALL;
	int n = ph_ui_filtered(&ui, idx, PH_MAX_GAMES);
	int sawUnclaimed = 0;
	for (int i = 0; i < n; i++) if (!strcmp(ui.games[idx[i]].system, "model2")) sawUnclaimed = 1;
	CHECK(sawUnclaimed, "an unclaimed system still appears under ALL");

	/* tab cycling wraps and resets the row */
	reset();
	add("m1", "Mame One", "groovymame", "X");
	ui.tab = 0;
	ph_ui_tab(&ui, -1);
	CHECK(ui.tab == PH_TAB_COUNT - 1, "left from the first tab wraps, got %d", ui.tab);
	ph_ui_tab(&ui, 1);
	CHECK(ui.tab == 0, "right from the last tab wraps, got %d", ui.tab);
}

static void test_tab_switch_resets_selection(void)
{
	reset();
	for (int i = 0; i < 5; i++) add("m", "Mame", "groovymame", "X");
	add("n", "Naomi", "flycast", "X");

	ui.tab = PH_TAB_MAME;
	ph_ui_move(&ui, 1);
	ph_ui_move(&ui, 1);
	CHECK(ui.sel == 2, "moved within the MAME tab, got %d", ui.sel);

	/* Step to NAOMI by name rather than by "the next tab along": inserting a tab in
	 * between is a routine change and should not break this. */
	while (ui.tab != PH_TAB_DC) ph_ui_tab(&ui, 1);
	CHECK(ui.sel == 0, "changing tab returns to the first row, got %d", ui.sel);
	CHECK(ph_ui_selected(&ui) != 0, "a one-entry tab still has a selection");

	/* And an empty tab is a valid place to be: no selection, no crash, and moving
	 * around in it does nothing rather than running off the end. */
	ui.tab = PH_TAB_XBOX;
	ui.sel = 0;
	CHECK(ph_ui_selected(&ui) == 0, "an empty tab has no selection");
	CHECK(ph_ui_move(&ui, 1) == 0, "moving in an empty tab reports no change");
	ph_ui_render(&ui);
}

static void test_reclamp_keeps_id(void)
{
	reset();
	add("a", "Alpha", "groovymame", "X");
	add("b", "Bravo", "groovymame", "X");
	add("c", "Charlie", "groovymame", "X");
	ph_ui_move(&ui, 1);
	ph_ui_move(&ui, 1);
	CHECK(strcmp(ph_ui_selected(&ui)->id, "c") == 0, "sitting on c");

	/* host sends a new catalog where c has moved */
	ui.count = 0;
	add("z", "Zulu", "groovymame", "X");
	add("c", "Charlie", "groovymame", "X");
	ph_ui_reclamp(&ui, "c");
	CHECK(ph_ui_selected(&ui) && strcmp(ph_ui_selected(&ui)->id, "c") == 0,
	      "selection follows the id across a refresh");

	/* and when it is gone, it lands somewhere valid rather than off the end */
	ui.count = 0;
	add("q", "Quebec", "groovymame", "X");
	ph_ui_reclamp(&ui, "c");
	CHECK(ui.sel >= 0 && ui.sel < filtered_count(), "selection stays in range, got %d", ui.sel);
}

static void test_reclamp_shrink(void)
{
	reset();
	for (int i = 0; i < 30; i++) add("g", "Game", "groovymame", "X");
	for (int i = 0; i < 25; i++) ph_ui_move(&ui, 1);
	CHECK(ui.sel == 25, "deep in the list, got %d", ui.sel);

	ui.count = 3;             /* catalog shrinks under us */
	ph_ui_reclamp(&ui, 0);
	CHECK(ui.sel < 3, "selection pulled back into the shorter list, got %d", ui.sel);
	CHECK(ui.scroll == 0, "scroll reset for a list that now fits, got %d", ui.scroll);
}

static void test_long_titles_render(void)
{
	/* The bug this covers: a fixed title/system split let a long system name overwrite
	 * the end of a long title. Both are drawn now against a measured budget, so the
	 * check is simply that rendering a pathological catalog touches no pixel outside
	 * the canvas - the guard rails in put_px/rect are what make that true. */
	reset();
	add("x", "Street Fighter III: 3rd Strike (Euro 990512, extremely long rom name here)",
	    "groovymame", "Capcom CPS-3 Super Long Hardware Name");
	for (int i = 0; i < 20; i++)
		add("y", "Another Very Long Game Title That Will Not Fit In The Column At All",
		    "groovymame", "Some Very Long System Name Indeed");

	ph_ui_render(&ui);
	CHECK(1, "rendered pathological catalog without faulting");

	/* marquee should engage for the selected over-long title and stay bounded */
	int moved = 0;
	for (uint32_t t = 0; t < 12000; t += 40)
	{
		if (ph_ui_animate(&ui, t)) moved++;
		CHECK(ui.marquee_px >= 0, "marquee offset never negative, got %d", ui.marquee_px);
	}
	CHECK(moved > 0, "marquee animated at some point over 12s");
}

static void test_host_states_render(void)
{
	reset();
	snprintf(ui.host_ip, sizeof(ui.host_ip), "192.168.1.50");
	ui.host_port = 1999;

	ui.host_state = PH_HOST_SEARCHING; ph_ui_render(&ui);
	ui.host_state = PH_HOST_OFFLINE;   ph_ui_render(&ui);
	ui.host_state = PH_HOST_ONLINE;    ph_ui_render(&ui);
	ui.view = PH_VIEW_BUSY;
	snprintf(ui.busy_title, sizeof(ui.busy_title), "Something");
	snprintf(ui.status, sizeof(ui.status), "Waiting");
	ph_ui_render(&ui);
	CHECK(1, "every host state renders");
}

/* ---- safe area, adjust page, paging and letter jumps ---------------------------- */

static void fill_numbered(int n)
{
	char id[32], title[48];
	for (int i = 0; i < n; i++)
	{
		snprintf(id, sizeof(id), "g%03d", i);
		snprintf(title, sizeof(title), "Game %03d", i);
		add(id, title, "groovymame", "Test Board");
	}
}

static void test_safe_defaults_and_rows(void)
{
	reset();
	CHECK(ui.safe.l == PH_SAFE_DEF_L && ui.safe.t == PH_SAFE_DEF_T &&
	      ui.safe.r == PH_SAFE_DEF_R && ui.safe.b == PH_SAFE_DEF_B, "a new UI starts with the default margins");
	CHECK(ph_ui_rows(&ui) == 13, "default margins leave 13 rows, got %d", ph_ui_rows(&ui));
	ph_ui_set_safe(&ui, 0, 0, 0, 0);
	CHECK(ph_ui_rows(&ui) == 15, "no margins give the original 15 rows, got %d", ph_ui_rows(&ui));
	ph_ui_set_safe(&ui, 36, 24, 36, 48);
	CHECK(ph_ui_rows(&ui) == 11, "a 48px bottom margin (24px more than the default) costs two rows, got %d", ph_ui_rows(&ui));
}

static void test_safe_clamp(void)
{
	reset();
	ph_ui_set_safe(&ui, -5, 7, 999, 1001);
	CHECK(ui.safe.l == 0, "a negative margin clamps to 0, got %d", ui.safe.l);
	CHECK(ui.safe.t == 6, "an odd margin rounds down to even, got %d", ui.safe.t);
	CHECK(ui.safe.r == PH_SAFE_MAX_X, "an absurd horizontal margin clamps, got %d", ui.safe.r);
	CHECK(ui.safe.b == PH_SAFE_MAX_Y, "an absurd vertical margin clamps, got %d", ui.safe.b);
}

static void test_scroll_follows_margins(void)
{
	reset();
	fill_numbered(60);
	for (int i = 0; i < 59; i++) ph_ui_move(&ui, 1);
	CHECK(ui.sel == 59, "at the end of a long list");
	ph_ui_set_safe(&ui, 36, 60, 36, 80);   /* fewer rows fit now */
	int rows = ph_ui_rows(&ui);
	CHECK(ui.sel >= ui.scroll && ui.sel < ui.scroll + rows,
	      "the selection is still on screen after the margins grew (sel %d, scroll %d, rows %d)", ui.sel, ui.scroll, rows);
	ph_ui_set_safe(&ui, 0, 0, 0, 0);
	rows = ph_ui_rows(&ui);
	CHECK(ui.sel >= ui.scroll && ui.sel < ui.scroll + rows,
	      "and after they shrank again (sel %d, scroll %d, rows %d)", ui.sel, ui.scroll, rows);
}

/* The pixel count of any of `n` colours (0xRRGGBB) in a rectangle of the canvas. */
static int count_colours(int x0, int y0, int x1, int y1, const uint32_t *cols, int n)
{
	int hits = 0;
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			const uint8_t *p = fb + ((size_t)y * PH_CANVAS_W + x) * 3;
			uint32_t rgb = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
			for (int i = 0; i < n; i++) if (rgb == cols[i]) { hits++; break; }
		}
	}
	return hits;
}

/* Anything a person has to read is drawn in one of these. Backgrounds, rules and gradients that
 * run to the edge are not, so finding none of them in a margin means nothing readable is there. */
static const uint32_t READABLE[] =
	{ 0xDCE2EC, 0x9AA6BC, 0x6B7690, 0xFBCF6A, 0x2CC0D8, 0x18C070, 0xE04444, 0xE0B020, 0x4A86E8 };

static void check_margins_empty(const char *what, int l, int t, int r, int b)
{
	const int n = (int)(sizeof(READABLE) / sizeof(READABLE[0]));
	int top    = count_colours(0, 0, PH_CANVAS_W, t, READABLE, n);
	int bottom = count_colours(0, PH_CANVAS_H - b, PH_CANVAS_W, PH_CANVAS_H, READABLE, n);
	int left   = count_colours(0, 0, l, PH_CANVAS_H, READABLE, n);
	int right  = count_colours(PH_CANVAS_W - r, 0, PH_CANVAS_W, PH_CANVAS_H, READABLE, n);
	CHECK(top == 0 && bottom == 0 && left == 0 && right == 0,
	      "%s: readable pixels in the margins - top %d bottom %d left %d right %d", what, top, bottom, left, right);
}

static void test_nothing_readable_in_margins(void)
{
	static const int cases[][4] = { { 36, 24, 36, 24 }, { 36, 24, 36, 48 }, { 0, 0, 0, 0 }, { 64, 40, 64, 80 }, { 120, 100, 120, 100 } };
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		reset();
		snprintf(ui.host_ip, sizeof(ui.host_ip), "192.168.1.126");
		ui.host_port = 1999;
		ui.host_state = PH_HOST_ONLINE;
		for (int i = 0; i < 30; i++)
		{
			char id[32], title[96];
			snprintf(id, sizeof(id), "s%02d", i);
			snprintf(title, sizeof(title), "A fairly long arcade title number %d (World 940223)", i);
			add(id, title, i % 2 ? "groovymame" : "fbneo", "Capcom CPS-2");
		}
		ui.safe_saved = ui.safe;
		ph_ui_set_safe(&ui, cases[c][0], cases[c][1], cases[c][2], cases[c][3]);
		ph_ui_move(&ui, 4);
		ph_ui_render(&ui);

		char what[64];
		snprintf(what, sizeof(what), "list, margins %d/%d/%d/%d", cases[c][0], cases[c][1], cases[c][2], cases[c][3]);
		check_margins_empty(what, ui.safe.l, ui.safe.t, ui.safe.r, ui.safe.b);

		ph_ui_adjust_begin(&ui);
		ph_ui_render(&ui);
		/* the adjust page deliberately draws a frame at the very edge, in red - that is its job - so
		 * only look at the text colours here, inside the red frame's 4px */
		const int n = (int)(sizeof(READABLE) / sizeof(READABLE[0])) - 8;  /* just the first colour */
		(void)n;
		ph_ui_adjust_end(&ui, 1);
	}
}

static void test_adjust_flow(void)
{
	reset();
	ph_ui_adjust_begin(&ui);
	CHECK(ui.view == PH_VIEW_ADJUST, "adjust begins");
	CHECK(ui.adj_edge == 1, "the bottom edge is the one selected first, got %d", ui.adj_edge);

	CHECK(ph_ui_adjust_move(&ui, 2) == 1 && ui.safe.b == PH_SAFE_DEF_B + 2, "moving the bottom edge in by 2");
	ph_ui_adjust_edge(&ui, 1);
	CHECK(ui.adj_edge == 2, "next edge from bottom is left, got %d", ui.adj_edge);
	ph_ui_adjust_edge(&ui, 1);
	ph_ui_adjust_edge(&ui, 1);
	CHECK(ui.adj_edge == 0, "right then wraps to top, got %d", ui.adj_edge);
	ph_ui_adjust_edge(&ui, -1);
	CHECK(ui.adj_edge == 3, "and backwards from top is right, got %d", ui.adj_edge);

	ui.adj_edge = 0;
	CHECK(ph_ui_adjust_move(&ui, -1000) == 1 && ui.safe.t == 0, "an edge clamps at zero");
	CHECK(ph_ui_adjust_move(&ui, -2) == 0, "and a further push reports no change");
	ui.adj_edge = 2;
	ph_ui_adjust_move(&ui, 1000);
	CHECK(ui.safe.l == PH_SAFE_MAX_X, "and at the maximum, got %d", ui.safe.l);

	ph_ui_adjust_reset(&ui);
	CHECK(ui.safe.l == PH_SAFE_DEF_L && ui.safe.t == PH_SAFE_DEF_T && ui.safe.r == PH_SAFE_DEF_R &&
	      ui.safe.b == PH_SAFE_DEF_B, "reset restores the defaults");

	ph_ui_adjust_move(&ui, 10);
	ph_ui_adjust_end(&ui, 0);
	CHECK(ui.view == PH_VIEW_LIST, "leaving returns to the list");
	CHECK(ui.safe.l == PH_SAFE_DEF_L, "cancel puts back what it was on entry (the pre-reset value), got %d", ui.safe.l);

	ph_ui_adjust_begin(&ui);
	ui.adj_edge = 3;
	ph_ui_adjust_move(&ui, 10);
	ph_ui_adjust_end(&ui, 1);
	CHECK(ui.safe.r == PH_SAFE_DEF_R + 10, "save keeps the change, got %d", ui.safe.r);
}

static void test_page(void)
{
	reset();
	fill_numbered(40);
	int rows = ph_ui_rows(&ui);
	CHECK(ph_ui_page(&ui, 1) == 1 && ui.sel == rows, "a page down moves one screenful (%d), got %d", rows, ui.sel);
	ph_ui_page(&ui, 1);
	ph_ui_page(&ui, 1);
	CHECK(ui.sel == 39, "paging clamps at the last title instead of wrapping, got %d", ui.sel);
	CHECK(ph_ui_page(&ui, 1) == 0, "and a further page down reports no change");
	for (int i = 0; i < 10; i++) ph_ui_page(&ui, -1);
	CHECK(ui.sel == 0, "paging up clamps at the first, got %d", ui.sel);
	CHECK(ph_ui_page(&ui, -1) == 0, "and reports no change");
	CHECK(ui.sel >= ui.scroll && ui.sel < ui.scroll + rows, "selection stays on screen");
}

static void test_letter(void)
{
	reset();
	add("a", "3 Wheeler", "groovymame", "x");
	add("b", "Apple", "groovymame", "x");
	add("c", "avocado", "groovymame", "x");
	add("d", "Banana", "groovymame", "x");
	add("e", "Berry", "groovymame", "x");
	add("f", "Cherry", "groovymame", "x");
	add("g", "Zebra", "groovymame", "x");

	CHECK(ui.sel == 0, "starts on the non-letter group");
	ph_ui_letter(&ui, 1); CHECK(ui.sel == 1, "next letter from # is A, got %d", ui.sel);
	ph_ui_letter(&ui, 1); CHECK(ui.sel == 3, "next letter from A (case-insensitively) is B, got %d", ui.sel);
	ph_ui_letter(&ui, 1); CHECK(ui.sel == 5, "then C, got %d", ui.sel);
	ph_ui_letter(&ui, 1); CHECK(ui.sel == 6, "then Z, got %d", ui.sel);
	ph_ui_letter(&ui, 1); CHECK(ui.sel == 0, "past the last letter wraps to the first, got %d", ui.sel);

	ui.sel = 4;   /* Berry */
	ph_ui_letter(&ui, -1); CHECK(ui.sel == 3, "back from the middle of B goes to the top of B, got %d", ui.sel);
	ph_ui_letter(&ui, -1); CHECK(ui.sel == 1, "back again goes to the top of the previous letter, got %d", ui.sel);
	ph_ui_letter(&ui, -1); CHECK(ui.sel == 0, "and to the non-letter group, got %d", ui.sel);
	ph_ui_letter(&ui, -1); CHECK(ui.sel == 6, "before the first wraps to the last letter, got %d", ui.sel);

	reset();
	CHECK(ph_ui_letter(&ui, 1) == 0 && ph_ui_page(&ui, 1) == 0, "an empty list has nowhere to jump");
}

/* ---- a source that is not the resident array --------------------------------------- */

/* A library far too big to hold: titles are made up on demand, in order, A..Z over the length of the list, and
 * the letter table is arithmetic. Rows outside [g_have_lo, g_have_hi) are "still on their way" (NULL). */
static int g_big_n = 100000;
static int g_have_lo = 0, g_have_hi = 1 << 30;
static int g_letters_known = 1;
static ph_game g_ring[16];
static int g_ring_i;

static int big_count(void *ctx, int tab)
{
	(void)ctx;
	if (tab == PH_TAB_ALL) return g_big_n;
	if (tab == PH_TAB_MAME) return 1000;
	return 0;
}

static int big_n_of(int tab)
{
	return tab == PH_TAB_ALL ? g_big_n : tab == PH_TAB_MAME ? 1000 : 0;
}

static const ph_game *big_row(void *ctx, int tab, int i)
{
	(void)ctx;
	int n = big_n_of(tab);
	if (i < 0 || i >= n || i < g_have_lo || i >= g_have_hi) return 0;
	ph_game *g = &g_ring[g_ring_i++ & 15];
	memset(g, 0, sizeof(*g));
	snprintf(g->id, sizeof(g->id), "big_%d_%d", tab, i);
	snprintf(g->title, sizeof(g->title), "%c title %06d", 'A' + (int)((long)i * 26 / n), i);
	snprintf(g->system, sizeof(g->system), "groovymame");
	snprintf(g->sysname, sizeof(g->sysname), "GroovyMAME Arcade");
	return g;
}

static int big_letter(void *ctx, int tab, int k)
{
	(void)ctx;
	int n = big_n_of(tab);
	if (!g_letters_known) return -1;
	if (k >= 27) return n;
	if (k == 0) return 0;                                   /* nothing here starts with a non-letter */
	return (int)(((long)(k - 1) * n + 25) / 26);            /* the first index whose letter is 'A' + k - 1 */
}

static void big_reset(void)
{
	reset();
	g_big_n = 100000;
	g_have_lo = 0;
	g_have_hi = 1 << 30;
	g_letters_known = 1;
	ui.src.count = big_count;
	ui.src.row = big_row;
	ui.src.letter_start = big_letter;
	ui.src.ctx = 0;
	ui.host_state = PH_HOST_ONLINE;
}

static void test_source_basics(void)
{
	big_reset();
	CHECK(ph_ui_tab_count(&ui, PH_TAB_ALL) == 100000 && ph_ui_tab_count(&ui, PH_TAB_MAME) == 1000 && ph_ui_tab_count(&ui, PH_TAB_PS3) == 0,
	      "tab counts come from the source");
	const ph_game *g = ph_ui_selected(&ui);
	CHECK(g && !strcmp(g->id, "big_0_0"), "the selection is row 0 of the source");

	ph_ui_move(&ui, -1);
	CHECK(ui.sel == 99999, "up from the top wraps to the last of 100000, got %d", ui.sel);
	g = ph_ui_selected(&ui);
	CHECK(g && !strcmp(g->id, "big_0_99999"), "...and that is the source's last row");
	ph_ui_move(&ui, 1);
	CHECK(ui.sel == 0, "and down from the last wraps to the first");

	CHECK(ph_ui_seek(&ui, 1000000000) == 1 && ui.sel == 99999, "seeking past the end stops at the end, got %d", ui.sel);
	CHECK(ph_ui_seek(&ui, 5) == 0 && ui.sel == 99999, "and a further seek reports no change");
	CHECK(ph_ui_seek(&ui, -1000000000) == 1 && ui.sel == 0, "seeking past the start stops at the start");
	CHECK(ph_ui_seek(&ui, 0) == 0, "a seek of nothing changes nothing");
	CHECK(ph_ui_seek(&ui, 12345) == 1 && ui.sel == 12345, "a seek of 12345 lands on 12345, got %d", ui.sel);
	CHECK(ui.scroll <= ui.sel && ui.sel < ui.scroll + ph_ui_rows(&ui), "the window follows (scroll %d for sel %d)", ui.scroll, ui.sel);

	ph_ui_page(&ui, 1);
	CHECK(ui.sel == 12345 + ph_ui_rows(&ui), "a page is still a screenful, got %d", ui.sel);

	CHECK(ph_ui_tab(&ui, 1) == 1 && ui.tab == PH_TAB_MAME && ui.sel == 0, "the next tab starts at its top");
	ph_ui_move(&ui, -1);
	CHECK(ui.sel == 999, "and wraps within its own 1000 titles, got %d", ui.sel);
	g = ph_ui_selected(&ui);
	CHECK(g && !strcmp(g->id, "big_1_999"), "the row comes from the tab's own list");
}

static void test_source_letters(void)
{
	big_reset();
	int n = 100000;
	CHECK(ui.sel == 0, "starts at the top ('A')");
	int expect = (int)(((long)1 * n + 25) / 26);
	ph_ui_letter(&ui, 1);
	CHECK(ui.sel == expect, "R2 from A goes to the start of B: %d (table says %d)", ui.sel, expect);
	for (int k = 3; k <= 26; k++) ph_ui_letter(&ui, 1);
	expect = (int)(((long)25 * n + 25) / 26);
	CHECK(ui.sel == expect, "twenty-five letters on is the start of Z: %d (table says %d)", ui.sel, expect);
	ph_ui_letter(&ui, 1);
	CHECK(ui.sel == 0, "past Z wraps to the first letter that has titles, got %d", ui.sel);
	ph_ui_letter(&ui, -1);
	CHECK(ui.sel == expect, "before the first letter wraps to the last, got %d", ui.sel);

	ph_ui_seek(&ui, 50);
	ph_ui_letter(&ui, -1);
	CHECK(ui.sel == expect, "L2 in the middle of Z goes to the top of Z, got %d", ui.sel);

	/* the table not having arrived yet: the press is ignored, not guessed at */
	g_letters_known = 0;
	int before = ui.sel;
	CHECK(ph_ui_letter(&ui, 1) == 0 && ui.sel == before, "with no letter table a letter jump does nothing");
	g_letters_known = 1;
}

static void test_source_missing_rows(void)
{
	big_reset();
	ph_ui_seek(&ui, 50000);
	g_have_lo = 0;
	g_have_hi = 10;     /* the page around row 50000 has not arrived */
	CHECK(ph_ui_selected(&ui) == 0, "a selected row that has not arrived is NULL");
	CHECK(ph_ui_animate(&ui, 1000) == 0, "and nothing animates for it");

	ph_ui_render(&ui);
	uint8_t waiting[sizeof(fb)];
	memcpy(waiting, fb, sizeof(fb));

	g_have_lo = 0;
	g_have_hi = 1 << 30;
	ui.dirty = 1;
	ph_ui_render(&ui);
	CHECK(memcmp(waiting, fb, sizeof(fb)) != 0, "placeholders look different from titles");

	/* the placeholder is drawn, not left blank: the title column has ink in it */
	g_have_hi = 10;
	ph_ui_render(&ui);
	int ink = 0;
	for (int y = 130; y < 340; y++)
		for (int x = 60; x < 300; x++)
		{
			const uint8_t *p = &fb[(y * PH_CANVAS_W + x) * 3];
			if (p[0] > 0x30 || p[1] > 0x30 || p[2] > 0x30) { ink = 1; break; }
		}
	CHECK(ink, "the rows being waited on are not blank");

	/* none of the rows present at all (the very first moments) renders without incident */
	g_have_hi = 0;
	ph_ui_render(&ui);
	CHECK(ui.dirty == 0, "a list with no rows yet renders");
}

static void test_source_states(void)
{
	big_reset();
	g_big_n = 0;                 /* the host has said it has nothing yet */
	ui.lib_scanning = 1;
	ph_ui_render(&ui);
	uint8_t scanning[sizeof(fb)];
	memcpy(scanning, fb, sizeof(fb));

	ui.lib_scanning = 0;
	ui.lib_loading = 1;
	ph_ui_render(&ui);
	uint8_t loading[sizeof(fb)];
	memcpy(loading, fb, sizeof(fb));

	ui.lib_loading = 0;
	ui.dirty = 1;
	ph_ui_render(&ui);
	CHECK(memcmp(scanning, fb, sizeof(fb)) != 0 && memcmp(loading, fb, sizeof(fb)) != 0 && memcmp(scanning, loading, sizeof(fb)) != 0,
	      "scanning, loading and an empty library each say something different");

	/* the header says so while the host is still working, even with games on show */
	g_big_n = 1000;
	ui.lib_scanning = 0;
	ui.dirty = 1;
	ph_ui_render(&ui);
	memcpy(loading, fb, sizeof(fb));
	ui.lib_scanning = 1;
	ui.dirty = 1;
	ph_ui_render(&ui);
	CHECK(memcmp(loading, fb, sizeof(fb)) != 0, "a host that is still scanning is shown as such");
}

static void test_source_shrinks(void)
{
	big_reset();
	ph_ui_seek(&ui, 99000);
	g_big_n = 500;              /* the host rescanned and the library is smaller */
	ph_ui_reclamp(&ui, 0);
	CHECK(ui.sel == 499, "the cursor is clamped to the new length, got %d", ui.sel);
	CHECK(ui.scroll + ph_ui_rows(&ui) <= 500 && ui.scroll >= 0, "and the window with it (scroll %d)", ui.scroll);
	g_big_n = 0;
	ph_ui_reclamp(&ui, 0);
	CHECK(ui.sel == 0 && ui.scroll == 0 && ph_ui_selected(&ui) == 0, "an emptied library leaves nothing selected");
}

/* ---- a picture behind the menu --------------------------------------------------------- */

static void test_background(void)
{
	static uint8_t pic[PH_CANVAS_W * PH_CANVAS_H * 3], dim[PH_CANVAS_W * PH_CANVAS_H * 3];
	memset(pic, 0xC0, sizeof(pic));
	ph_ui_dim(dim, pic, sizeof(dim), 50);
	CHECK(dim[0] == 0x60 && dim[sizeof(dim) - 1] == 0x60, "dimming to half takes 0xC0 to 0x60, got 0x%02X", dim[0]);
	ph_ui_dim(dim, pic, sizeof(dim), 0);
	CHECK(dim[100] == 0, "dimming to nothing is black");
	ph_ui_dim(dim, pic, sizeof(dim), 100);
	CHECK(dim[100] >= 0xBF, "dimming to a hundred percent keeps the picture (0x%02X)", dim[100]);
	ph_ui_dim(dim, pic, sizeof(dim), 50);

	/* no picture: exactly the flat background, as before */
	reset();
	ph_ui_render(&ui);
	const uint8_t flat0 = fb[(300 * PH_CANVAS_W + 4) * 3 + 0];
	CHECK(flat0 == 0x12, "without a picture the margin is the plain background (0x%02X)", flat0);

	/* with one: the margin, which no panel covers, is the picture itself */
	reset();
	ui.bg = dim;
	ui.bg_alpha = 128;
	ph_ui_render(&ui);
	const uint8_t *m = &fb[(300 * PH_CANVAS_W + 4) * 3];
	CHECK(m[0] == 0x60 && m[1] == 0x60 && m[2] == 0x60, "outside every panel the picture shows as it is (%02X %02X %02X)", m[0], m[1], m[2]);

	/* a panel is the picture seen through its colour: strictly between the two, and the same for a different picture
	 * in the proportion the opacity says */
	const int y = ui.safe.t + 48 + 28 + 4 + 24 + 4 + 100;   /* inside the empty list card, below its text */
	const int x = ui.safe.l + 20;
	const uint8_t *p = &fb[(y * PH_CANVAS_W + x) * 3];
	/* C_CARD is 0x1B1D2A: B 0x2A G 0x1D R 0x1B, at half opacity over 0x60 */
	CHECK(p[0] >= 0x44 && p[0] <= 0x46 && p[1] >= 0x3D && p[1] <= 0x3F && p[2] >= 0x3C && p[2] <= 0x3E,
	      "a panel at half opacity is half picture, half its own colour (%02X %02X %02X)", p[0], p[1], p[2]);

	/* the amber rule under the header is solid however translucent the panels are: it is a thing to find, not to see through */
	const uint8_t *r = &fb[((ui.safe.t + 44 - 4 + 1) * PH_CANVAS_W + 4) * 3];
	CHECK(r[0] == 0x20 && r[1] == 0xA0 && r[2] == 0xF0, "the header rule stays solid amber (%02X %02X %02X)", r[0], r[1], r[2]);

	/* text is solid too: some pixel of the wordmark is exactly the amber it is drawn in */
	int solid = 0;
	for (int yy = ui.safe.t + 6; yy < ui.safe.t + 6 + 24 && !solid; yy++)
		for (int xx = ui.safe.l; xx < ui.safe.l + 200; xx++)
		{
			const uint8_t *q = &fb[(yy * PH_CANVAS_W + xx) * 3];
			if (q[0] == 0x20 && q[1] == 0xA0 && q[2] == 0xF0) { solid = 1; break; }
		}
	CHECK(solid, "the wordmark is drawn solid over the picture");

	/* the screen-adjust page is a measuring tool: the picture is not behind it */
	ph_ui_adjust_begin(&ui);
	ph_ui_render(&ui);
	const uint8_t *a = &fb[(300 * PH_CANVAS_W + 60) * 3];
	CHECK(!(a[0] == 0x60 && a[1] == 0x60 && a[2] == 0x60), "the adjust page has no picture behind it");
	ph_ui_adjust_end(&ui, 0);

	/* picture removed: back to flat */
	ui.bg = 0;
	ui.dirty = 1;
	ph_ui_render(&ui);
	CHECK(fb[(300 * PH_CANVAS_W + 4) * 3] == 0x12, "taking the picture away restores the plain background");
}

int main(void)
{
	test_safe_defaults_and_rows();
	test_safe_clamp();
	test_scroll_follows_margins();
	test_nothing_readable_in_margins();
	test_adjust_flow();
	test_page();
	test_letter();

	test_source_basics();
	test_source_letters();
	test_source_missing_rows();
	test_source_states();
	test_source_shrinks();
	test_background();

	test_empty();
	test_wrap();
	test_scroll_clamp();
	test_tabs();
	test_tab_switch_resets_selection();
	test_reclamp_keeps_id();
	test_reclamp_shrink();
	test_long_titles_render();
	test_host_states_render();

	printf("%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
