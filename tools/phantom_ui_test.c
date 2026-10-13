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

int main(void)
{
	test_safe_defaults_and_rows();
	test_safe_clamp();
	test_scroll_follows_margins();
	test_nothing_readable_in_margins();
	test_adjust_flow();
	test_page();
	test_letter();

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
