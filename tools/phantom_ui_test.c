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
	add("d1", "Cube One", "dolphin", "X");   /* no GroovyNLC fork: must match no tab */

	ui.tab = PH_TAB_ALL;   CHECK(filtered_count() == 9, "ALL shows everything, got %d", filtered_count());
	ui.tab = PH_TAB_MAME;  CHECK(filtered_count() == 2, "MAME tab matches groovymame and mame, got %d", filtered_count());
	ui.tab = PH_TAB_FBNEO; CHECK(filtered_count() == 1, "FBNeo tab, got %d", filtered_count());
	ui.tab = PH_TAB_NAOMI; CHECK(filtered_count() == 1, "NAOMI tab, got %d", filtered_count());
	ui.tab = PH_TAB_PS2;   CHECK(filtered_count() == 1, "PS2 tab, got %d", filtered_count());
	ui.tab = PH_TAB_PS3;   CHECK(filtered_count() == 1, "PS3 tab, got %d", filtered_count());
	ui.tab = PH_TAB_XBOX;  CHECK(filtered_count() == 1, "Xbox tab, got %d", filtered_count());
	ui.tab = PH_TAB_RA;    CHECK(filtered_count() == 1, "RetroArch tab, got %d", filtered_count());

	/* A system no tab claims is still reachable under ALL rather than vanishing: the
	 * catalog is the host's to decide, and silently hiding rows would be worse than
	 * showing them in one place. */
	int idx[PH_MAX_GAMES];
	ui.tab = PH_TAB_ALL;
	int n = ph_ui_filtered(&ui, idx, PH_MAX_GAMES);
	int sawCube = 0;
	for (int i = 0; i < n; i++) if (!strcmp(ui.games[idx[i]].system, "dolphin")) sawCube = 1;
	CHECK(sawCube, "an unclaimed system still appears under ALL");

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
	while (ui.tab != PH_TAB_NAOMI) ph_ui_tab(&ui, 1);
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

int main(void)
{
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
