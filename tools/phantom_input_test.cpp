/*
 * phantom_input_test.cpp - who owns the pad, and when.
 *
 * Builds the real hps_linux/src/support/groovy/phantom.cpp off-target. The six hardware
 * hooks it needs from groovy.cpp are stubbed, and a UDP "host" on loopback stands in for
 * the PC daemon so a LAUNCH or KILL the launcher sends can be seen arriving. The pad is
 * fed through the same two-step the core uses in user_io.cpp: the launcher gets first
 * refusal, and only what it does not consume reaches the emulator.
 *
 * The question it answers is the one users keep asking: while a game is running, can the
 * launcher also be reacting to the pad? In steady state it cannot. The risk is the moment
 * the session ends - a stall, a client reconnect, the exit hotkey - when the launcher
 * takes the screen and the very next button press launches whatever row is highlighted.
 *
 *   wsl -- bash tools/run_phantom_input_test.sh
 *
 * Exit status is the number of failed checks.
 */
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <linux/input.h> /* KEY_* - phantom_keyboard takes raw event codes */
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "phantom.h"
#include "phantom_ui.h"
#include "user_io.h" /* JOY_* - the same bit values the core uses (found via -I hps_linux/src) */
#include "../host/game_index.h" /* the daemon's own index and reply code: the fake host below answers with it */

/* The Groovy core's ten pad positions in order - A, B, X, Y, L, R, Select, Start, L2, R2 - as bits
 * 4..13 of the mask. user_io.h's JOY_SELECT/JOY_START (0x40/0x80) are the older names for what are
 * X and Y here. */
#define GJ_L      0x0100
#define GJ_R      0x0200
#define GJ_L2     0x1000
#define GJ_R2     0x2000
#define GJ_X      0x0040
#define GJ_Y      0x0080
#define GJ_SELECT 0x0400
#define GJ_START  0x0800

/* Must match what phantom.cpp is built with (tools/run_phantom_input_test.sh passes the same
 * values to both), shortened so the session-end scenarios run in seconds. */
#ifndef T_CLOSE_GRACE
#define T_CLOSE_GRACE 2500
#endif
#ifndef T_CLOSE_HOLD_CAP
#define T_CLOSE_HOLD_CAP 30000
#endif
#ifndef T_CLOSE_POLL
#define T_CLOSE_POLL 400
#endif

#ifndef TEST_PORT
#define TEST_PORT 19999
#endif
#ifndef TEST_DIR
#define TEST_DIR "/tmp/phantom_input_test"
#endif

/* ---- the six hooks phantom.cpp expects groovy.cpp to provide ------------------------- */

static int g_forwarded = 0; /* pad events the launcher did NOT consume = events the emulator got */
static uint8_t g_canvas[720 * 480 * 3]; /* the last frame presented, BGR888 */
static int     g_presents = 0;
static std::atomic<int> g_closes{0};      /* times the core was told to really close the session */
static std::atomic<int> g_status_mode{0}; /* what the fake host says to STATUS: 0 nothing (an old
                                           * host), 1 RUNNING, 2 IDLE */

extern "C" {
uint32_t groovy_idle_canvas_bytes(void) { return 720u * 480u * 3u; }
int      groovy_idle_begin(void) { return 1; }
void     groovy_idle_present(const uint8_t *canvas)
{
    memcpy(g_canvas, canvas, sizeof(g_canvas)); /* what the launcher just drew, for the picture tests */
    g_presents++;
}
void     groovy_idle_end(void) {}
int      groovy_ps2_inputs_enabled(void) { return 0; }

/* In the core this is setClose(), which re-raises the idle screen - and with the launcher
 * as the idle screen, that is phantom_idle_enter(). Mirrored here because it is exactly the
 * path the exit hotkey takes. */
void groovy_force_close(void)
{
    g_closes++;
    phantom_idle_enter();
}
}

/* ---- a fake PC daemon ------------------------------------------------------------- */

static std::mutex               g_mx;
static std::vector<std::string> g_rx;
static std::atomic<bool>        g_stop{false};

/* What the fake host's library is, and how it behaves. The paged catalog is answered by the daemon's own code
 * (host/game_index.h), so each side of the wire is tested against the other's real implementation. */
static std::mutex                            g_ix_mx;
static std::shared_ptr<const phantom::Index> g_ix;
static std::atomic<int> g_host_paged{1}; /* 1: speaks the paged catalog; 0: an older daemon that only knows GET_CATALOG */
static std::atomic<int> g_drop_lists{0}; /* swallow this many LIST requests, to see them asked for again */
static std::atomic<int> g_scanning{0};
static std::atomic<int> g_fail_launch{0}; /* answer the next LAUNCH with a LAUNCHFAIL, as the PC does when an emulator will not start */

static std::shared_ptr<const phantom::Index> cur_ix()
{
    std::lock_guard<std::mutex> lk(g_ix_mx);
    return g_ix;
}
static void set_ix(std::shared_ptr<const phantom::Index> ix)
{
    std::lock_guard<std::mutex> lk(g_ix_mx);
    g_ix = ix;
}

static std::shared_ptr<phantom::Index> make_small(unsigned ver)
{
    phantom::IndexBuilder b;
    int d = b.AddSystem("dolphin", "Nintendo GameCube / Wii", "/r/d");
    int x = b.AddSystem("xemu", "Microsoft Xbox", "/r/x");
    int m = b.AddSystem("groovymame", "GroovyMAME Arcade", "/r/m");
    b.Add(d, "dolphin_Galaxy", "Super Mario Galaxy", "/r/d/g.iso", "", "", "", 1, 1);
    b.Add(x, "xemu_Halo2", "Halo 2", "/r/x/h.iso", "", "", "", 1, 1);
    b.Add(m, "groovymame_sf2", "Street Fighter II", "/r/m/sf2.zip", "", "", "", 1, 1);
    return b.Finish(ver);
}

/* n games over three systems, titles spread across every letter plus some digits first. */
static std::shared_ptr<phantom::Index> make_big(unsigned n, unsigned ver)
{
    phantom::IndexBuilder b;
    int m = b.AddSystem("groovymame", "GroovyMAME Arcade", "/r/m");
    int d = b.AddSystem("dolphin", "Nintendo GameCube / Wii", "/r/d");
    int x = b.AddSystem("xemu", "Microsoft Xbox", "/r/x");
    for (unsigned i = 0; i < n; i++) {
        const int         s = (i % 10) < 6 ? m : (i % 10) < 9 ? d : x;
        const char *const key = s == m ? "groovymame" : s == d ? "dolphin" : "xemu";
        char title[64];
        if (i % 40 == 0)
            std::snprintf(title, sizeof(title), "%u Game", i);
        else
            std::snprintf(title, sizeof(title), "%c%c Title %u", 'A' + (int) ((i * 7) % 26), 'a' + (int) ((i * 11) % 26), i);
        char id[64];
        std::snprintf(id, sizeof(id), "%s_set%u", key, i);
        b.Add(s, id, title, "/r/x.zip", "15kHz Native", "1990", "Capcom", i, i);
    }
    return b.Finish(ver);
}

static const char *CATALOG =
    "{\"games\":["
    "{\"id\":\"dolphin_Galaxy\",\"title\":\"Super Mario Galaxy\",\"system\":\"dolphin\"},"
    "{\"id\":\"xemu_Halo2\",\"title\":\"Halo 2\",\"system\":\"xemu\"},"
    "{\"id\":\"groovymame_sf2\",\"title\":\"Street Fighter II\",\"system\":\"groovymame\"}"
    "]}";

static void host_thread(int fd)
{
    char buf[2048];
    while (!g_stop) {
        sockaddr_in from {};
        socklen_t   fl = sizeof(from);
        ssize_t     n  = recvfrom(fd, buf, sizeof(buf) - 1, 0, (sockaddr *) &from, &fl);
        if (n <= 0)
            continue; /* receive timeout */
        buf[n] = 0;
        std::string m(buf);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_rx.push_back(m);
        }
        if (m == "DISCOVER_PHANTOM" || m == "PING") {
            const char *r = "PHANTOM_HOST_ONLINE";
            sendto(fd, r, strlen(r), 0, (sockaddr *) &from, fl);
        } else if (m == "GET_CATALOG") {
            std::string body = g_host_paged ? phantom::CompactCatalogJson(*cur_ix()) : std::string(CATALOG);
            sendto(fd, body.data(), body.size(), 0, (sockaddr *) &from, fl);
        } else if (g_host_paged && (m == "CATINFO" || m.rfind("LETTERS:", 0) == 0 || m.rfind("LIST:", 0) == 0)) {
            if (m.rfind("LIST:", 0) == 0 && g_drop_lists > 0) {
                g_drop_lists--;
                continue;
            }
            std::string r;
            if (phantom::AnswerCatalogRequest(cur_ix(), g_scanning != 0, m, r))
                sendto(fd, r.data(), r.size(), 0, (sockaddr *) &from, fl);
        } else if (g_host_paged && m == "RESCAN") {
            const char *r = "RESCAN:OK";
            sendto(fd, r, strlen(r), 0, (sockaddr *) &from, fl);
        } else if (g_fail_launch > 0 && m.rfind("LAUNCH:", 0) == 0) {
            g_fail_launch--;
            const char *r = "ACK:LAUNCH:OK:x";
            sendto(fd, r, strlen(r), 0, (sockaddr *) &from, fl);
            usleep(120000); /* the PC takes a moment to find out */
            const char *bad = "LAUNCHFAIL:RPCS3 is showing an error window on the PC: RPCS3: Fatal Error";
            sendto(fd, bad, strlen(bad), 0, (sockaddr *) &from, fl);
        } else if (g_host_paged && m.rfind("LAUNCH:@", 0) == 0) {
            /* what the daemon does: refuse a position quoted against a library that is no longer current */
            unsigned ver = 0;
            std::string r = "ACK:LAUNCH:OK:x";
            if (std::sscanf(m.c_str(), "LAUNCH:@%u:", &ver) != 1 || ver != cur_ix()->ver)
                r = "ACK:LAUNCH:STALE:" + std::to_string(cur_ix()->ver);
            sendto(fd, r.data(), r.size(), 0, (sockaddr *) &from, fl);
        } else if (m == "STATUS") {
            int         mode = g_status_mode;
            const char *r    = mode == 1 ? "STATUS:RUNNING" : mode == 2 ? "STATUS:IDLE" : nullptr;
            if (r)
                sendto(fd, r, strlen(r), 0, (sockaddr *) &from, fl);
        }
    }
}

static int count_rx(const char *prefix)
{
    std::lock_guard<std::mutex> lk(g_mx);
    int                         n = 0;
    for (auto &s : g_rx)
        if (s.rfind(prefix, 0) == 0)
            n++;
    return n;
}

static std::string last_rx(const char *prefix)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto it = g_rx.rbegin(); it != g_rx.rend(); ++it)
        if (it->rfind(prefix, 0) == 0)
            return *it;
    return "";
}

/* ---- driving the launcher --------------------------------------------------------- */

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* The launcher's network half and its timers only advance inside phantom_idle_poll(). */
static void pump(int ms)
{
    for (int t = 0; t < ms; t += 2) {
        phantom_idle_poll();
        sleep_ms(2);
    }
}

/* The routing from user_io_digital_joystick(), verbatim in shape. */
static int pad(uint32_t mask)
{
    int consumed = phantom_joystick(0, mask);
    if (!consumed) {
        phantom_exit_hotkey(0, mask);
        g_forwarded++;
    }
    return consumed;
}

static void tap(uint32_t btn, int gap_ms = 60)
{
    pad(btn);
    pump(gap_ms);
    pad(0);
    pump(gap_ms);
}

static int g_fail = 0;
static void check(bool ok, const char *fmt, ...)
{
    char    msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok)
        g_fail++;
}

static void section(const char *s) { std::printf("\n%s\n", s); }

/* Where a paged launch would go: press A, read the LAUNCH it sent, press B to leave the loading screen. */
struct Pos {
    bool        ok  = false;
    unsigned    ver = 0;
    std::string list;
    int         index = -1;
};
static Pos launch_here()
{
    int n0 = count_rx("LAUNCH:@");
    tap(JOY_BTN1, 40);
    Pos p;
    if (count_rx("LAUNCH:@") == n0)
        return p;
    char     list[64] = "";
    unsigned ver      = 0;
    int      idx      = -1;
    if (std::sscanf(last_rx("LAUNCH:@").c_str(), "LAUNCH:@%u:%63[^:]:%d", &ver, list, &idx) == 3) {
        p.ok    = true;
        p.ver   = ver;
        p.list  = list;
        p.index = idx;
    }
    tap(JOY_BTN2, 40); /* B: back out of the loading screen */
    return p;
}

/* Does the launcher's settings file contain this line? */
static bool ini_has(const char *line)
{
    FILE *f = fopen(TEST_DIR "/phantom.ini", "r");
    if (!f)
        return false;
    char buf[256];
    bool hit = false;
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = 0;
        if (!strcmp(buf, line))
            hit = true;
    }
    fclose(f);
    return hit;
}

/* ---- scenarios -------------------------------------------------------------------- */

int main()
{
    mkdir(TEST_DIR, 0755);
    {
        FILE *f = fopen(TEST_DIR "/phantom.ini", "w");
        std::fprintf(f, "[SERVER]\nPC_SERVER_IP=127.0.0.1\nUDP_PORT=%d\n[LAUNCHER]\nEXIT_HOTKEY=on\n", TEST_PORT);
        fclose(f);
        unlink(TEST_DIR "/cache.tsv");
    }

    int hfd = socket(AF_INET, SOCK_DGRAM, 0);
    {
        sockaddr_in a {};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(TEST_PORT);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (bind(hfd, (sockaddr *) &a, sizeof(a)) < 0) {
            std::perror("bind fake host");
            return 99;
        }
        timeval tv { 0, 50000 };
        setsockopt(hfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    set_ix(make_small(41));
    std::thread host(host_thread, hfd);

    phantom_init();
    phantom_idle_enter(); /* first idle screen after boot: no stream has run yet */
    pump(600);            /* find the host, fetch the catalog */

    section("setup");
    check(phantom_owns_screen() == 1, "the launcher owns the screen at boot");
    check(phantom_title_count() == 3, "catalog fetched from the host (%d titles)", phantom_title_count());

    /* ---------------------------------------------------------------------------- */
    section("1. a deliberate press at the launcher launches the highlighted game");
    tap(JOY_BTN1);
    check(count_rx("LAUNCH:") == 1, "one LAUNCH sent (%s)", last_rx("LAUNCH:").c_str());
    phantom_idle_leave(); /* the host's video arrives */
    pump(100);

    /* ---------------------------------------------------------------------------- */
    section("2. while a game streams, does the launcher also react to the pad?");
    int launches_before = count_rx("LAUNCH:");
    int fwd_before      = g_forwarded;
    int consumed        = 0;
    for (int i = 0; i < 40; i++) {
        consumed += pad(JOY_BTN1);
        consumed += pad(0);
        consumed += pad(JOY_BTN2 | JOY_RIGHT);
        consumed += pad(0);
    }
    check(consumed == 0, "launcher consumed %d of 160 events (all must reach the emulator)", consumed);
    check(g_forwarded - fwd_before == 160, "emulator received %d of 160 events", g_forwarded - fwd_before);
    check(count_rx("LAUNCH:") == launches_before, "no LAUNCH while streaming (%d new)", count_rx("LAUNCH:") - launches_before);

    /* ---------------------------------------------------------------------------- */
    section("3. a button already HELD when the session ends must not count as a new press");
    launches_before = count_rx("LAUNCH:");
    pad(JOY_BTN1); /* user is holding attack in-game */
    pump(80);
    phantom_idle_enter(); /* the session closes under them: stall timeout or client reconnect */
    pump(40);
    pad(JOY_BTN1 | JOY_BTN3); /* any other change on the pad, BTN1 never released. A neutral
                               * button: RIGHT would change tab and leave later cases somewhere else. */
    pump(80);
    check(count_rx("LAUNCH:") == launches_before,
          "held BTN1 did not launch anything (%d unexpected LAUNCH)", count_rx("LAUNCH:") - launches_before);
    pad(0);
    pump(100);
    phantom_idle_leave();
    pump(50);

    /* The takeover guard would also hide a stale-edge bug in the case above, so prove the
     * edge tracking by itself: let the guard expire, with the button still held throughout. */
    section("3b. ...and not even once the guard has expired: it is still the same, unreleased press");
    launches_before = count_rx("LAUNCH:");
    pad(JOY_BTN1);
    pump(80);
    phantom_idle_enter();
    pump(1800); /* longer than the guard */
    pad(JOY_BTN1 | JOY_BTN3);
    pump(80);
    check(count_rx("LAUNCH:") == launches_before,
          "a button held since before the takeover launched %d games (must be 0)",
          count_rx("LAUNCH:") - launches_before);
    pad(0);
    pump(100);
    phantom_idle_leave();
    pump(50);

    /* ---------------------------------------------------------------------------- */
    section("4. mashing a button straight through a session close");
    launches_before = count_rx("LAUNCH:");
    for (int i = 0; i < 6; i++)
        tap(JOY_BTN1, 50); /* mashing in-game */
    phantom_idle_enter();  /* the session closes mid-mash */
    int mashed_after = 0;
    for (int i = 0; i < 10; i++) { /* ~1.2s more of the same, now at the launcher */
        tap(JOY_BTN1, 60);
        mashed_after++;
    }
    check(count_rx("LAUNCH:") == launches_before,
          "%d mashed presses after the close launched %d games (must be 0)", mashed_after,
          count_rx("LAUNCH:") - launches_before);

    /* ...and the launcher must still be usable once the user has actually stopped. */
    pump(2200);
    tap(JOY_BTN1);
    check(count_rx("LAUNCH:") == launches_before + 1, "a deliberate press after a pause still launches (%d)",
          count_rx("LAUNCH:") - launches_before);
    phantom_idle_leave();
    pump(50);

    /* ---------------------------------------------------------------------------- */
    section("5. the exit hotkey (Start+Select held) kills the game; what comes after?");
    int kills_before = count_rx("KILL");
    launches_before  = count_rx("LAUNCH:");
    pad(GJ_START | GJ_SELECT);
    pump(1300);
    pad(GJ_START | GJ_SELECT | JOY_BTN1); /* the third event is what completes the hold */
    pump(50);
    check(count_rx("KILL") == kills_before + 1, "hotkey sent KILL (%d)", count_rx("KILL") - kills_before);
    check(phantom_owns_screen() == 1, "launcher took the screen back");
    pad(0);
    pump(40);
    for (int i = 0; i < 8; i++)
        tap(JOY_BTN1, 60);
    check(count_rx("LAUNCH:") == launches_before,
          "mashing after the hotkey launched %d games (must be 0)", count_rx("LAUNCH:") - launches_before);

    /* ---------------------------------------------------------------------------- */
    section("5b. X and Y held together are NOT the exit hotkey (they used to be)");
    phantom_idle_leave(); /* a game is streaming */
    pump(100);
    kills_before = count_rx("KILL");
    pad(GJ_X | GJ_Y);
    pump(1600); /* longer than the 1.2s hold */
    pad(GJ_X | GJ_Y | JOY_BTN1);
    pump(50);
    pad(0);
    pump(40);
    check(count_rx("KILL") == kills_before, "holding X+Y for 1.6s sent %d KILLs (must be 0)", count_rx("KILL") - kills_before);
    check(phantom_owns_screen() == 0, "and the game kept the screen");

    /* ---------------------------------------------------------------------------- */
    section("5c. Y refreshes; Select opens the screen-adjust page, which saves, cancels and resets");
    phantom_idle_enter();
    pump(2200); /* past the takeover guard */

    int rescan0 = count_rx("RESCAN"), info0 = count_rx("CATINFO");
    tap(GJ_Y);
    pump(400);
    check(count_rx("RESCAN") > rescan0, "Y asked the host to look for new and changed games");
    check(count_rx("CATINFO") > info0, "...and for its library again");

    launches_before = count_rx("LAUNCH:");
    tap(GJ_SELECT); /* adjust page opens with the bottom edge selected */
    for (int i = 0; i < 6; i++)
        tap(JOY_UP); /* up shrinks the box: +2 each */
    tap(JOY_BTN1);   /* A saves */
    check(count_rx("LAUNCH:") == launches_before, "A on the adjust page saved rather than launching a game");
    check(ini_has("SAFE_B=36"), "six presses of Up moved the bottom margin from 24 to 36 and saved it");
    check(ini_has("SAFE_T=24") && ini_has("SAFE_L=36") && ini_has("SAFE_R=36"), "the other edges are untouched");

    tap(GJ_SELECT);
    for (int i = 0; i < 5; i++)
        tap(JOY_UP);
    tap(JOY_BTN2); /* B cancels */
    check(ini_has("SAFE_B=36"), "cancelling leaves the saved margins as they were");

    tap(GJ_SELECT);
    tap(JOY_RIGHT); /* next edge: left */
    for (int i = 0; i < 4; i++)
        tap(JOY_UP);
    tap(JOY_BTN1);
    check(ini_has("SAFE_L=44") && ini_has("SAFE_B=36"), "Right chose the left edge; 4 x Up made it 44 (bottom stays 36)");

    tap(GJ_SELECT);
    tap(GJ_Y); /* Y resets to the defaults */
    tap(JOY_BTN1);
    check(ini_has("SAFE_L=36") && ini_has("SAFE_T=24") && ini_has("SAFE_R=36") && ini_has("SAFE_B=24"),
          "Y then A restores the default margins");
    check(count_rx("LAUNCH:") == launches_before, "none of that launched a game");

    /* ---------------------------------------------------------------------------- */
    section("6. the keyboard takes the same road: Enter/Space launch, WASD navigate");
    phantom_idle_leave();
    pump(100);
    int kbd_consumed = 0;
    for (int i = 0; i < 20; i++)
        kbd_consumed += phantom_keyboard(KEY_ENTER, 1) + phantom_keyboard(KEY_ENTER, 0);
    check(kbd_consumed == 0, "while streaming the launcher consumed %d of 40 key events", kbd_consumed);

    launches_before = count_rx("LAUNCH:");
    phantom_idle_enter(); /* the session closes */
    for (int i = 0; i < 12; i++) {
        phantom_keyboard(KEY_SPACE, 1);
        phantom_keyboard(KEY_SPACE, 0);
        pump(60);
    }
    check(count_rx("LAUNCH:") == launches_before,
          "mashing Space through the takeover launched %d games (must be 0)",
          count_rx("LAUNCH:") - launches_before);
    pump(2200);
    phantom_keyboard(KEY_ENTER, 1);
    phantom_keyboard(KEY_ENTER, 0);
    pump(50);
    check(count_rx("LAUNCH:") == launches_before + 1, "Enter after a pause still launches (%d)",
          count_rx("LAUNCH:") - launches_before);

    /* ---------------------------------------------------------------------------- */
    section("7. a session that ends is waited on - the launcher must not take the screen from a live game");
    const int GRACE = T_CLOSE_GRACE, CAP = T_CLOSE_HOLD_CAP, POLL = T_CLOSE_POLL;
    std::printf("  (grace %dms, cap %dms, poll %dms)\n", GRACE, CAP, POLL);
    phantom_idle_leave(); /* a game is streaming */
    pump(100);

    /* 7a: the case that matters. A client reconnects: CMD_CLOSE, then CMD_INIT a moment later. */
    g_status_mode = 1;
    int closes0   = g_closes;
    int asks0     = count_rx("STATUS");
    check(phantom_session_ended(PHANTOM_END_CLOSE) == 1, "a close while streaming is taken over by the launcher");
    check(phantom_session_ended(PHANTOM_END_CLOSE) == 2, "a repeat just reports it is already waiting");
    pump(GRACE / 2);
    check(phantom_owns_screen() == 0 && g_closes == closes0, "nothing taken at half the grace");
    phantom_session_alive(PHANTOM_ALIVE_INIT); /* the client's CMD_INIT */
    pump(GRACE * 2);
    check(phantom_owns_screen() == 0 && g_closes == closes0,
          "a client that re-inits is never interrupted, however long we then wait (screen=%d closes=%d)",
          phantom_owns_screen(), (int) g_closes - closes0);
    check(count_rx("STATUS") > asks0, "the host was asked whether a game is running (%d asks)",
          count_rx("STATUS") - asks0);

    /* 7b: no reconnect, but the host says the game is alive. Hold - up to the cap. */
    g_status_mode = 1;
    closes0       = g_closes;
    phantom_session_ended(PHANTOM_END_CLOSE);
    pump(GRACE + 600);
    check(phantom_owns_screen() == 0 && g_closes == closes0,
          "game reported running: still holding the last frame %dms in, past the grace", GRACE + 600);
    pump(CAP - (GRACE + 600) + 600);
    check(phantom_owns_screen() == 1 && g_closes == closes0 + 1,
          "...but not for ever: a hung emulator cannot hold the screen past the cap");
    phantom_idle_leave();
    pump(100);

    /* 7c: the host says nothing is running (a normal quit). Only the grace is waited. */
    g_status_mode = 2;
    closes0       = g_closes;
    phantom_session_ended(PHANTOM_END_CLOSE);
    pump(GRACE - 150);
    check(phantom_owns_screen() == 0 && g_closes == closes0, "host idle: still waiting just inside the grace");
    pump(400);
    check(phantom_owns_screen() == 1 && g_closes == closes0 + 1, "host idle: the launcher takes the screen after the grace");
    phantom_idle_leave();
    pump(100);

    /* 7d: an old host never answers STATUS at all. Same as idle: the grace, then close. */
    g_status_mode = 0;
    closes0       = g_closes;
    phantom_session_ended(PHANTOM_END_CLOSE);
    pump(GRACE - 150);
    check(phantom_owns_screen() == 0, "no answer from the host: still waiting just inside the grace");
    pump(400);
    check(phantom_owns_screen() == 1 && g_closes == closes0 + 1, "no answer from the host: closes after the grace");
    phantom_idle_leave();
    pump(100);

    /* 7e: the game ends while we are holding for it. */
    g_status_mode = 1;
    closes0       = g_closes;
    phantom_session_ended(PHANTOM_END_CLOSE);
    pump(GRACE + 400);
    check(phantom_owns_screen() == 0, "held while the host reports a game");
    g_status_mode = 2; /* it quits */
    pump(POLL * 2 + 100);
    check(phantom_owns_screen() == 1 && g_closes == closes0 + 1,
          "the launcher appears within a poll or two of the game actually ending");
    phantom_idle_leave();
    pump(100);

    /* 7f: the two reasons are cancelled differently. */
    g_status_mode = 2;
    closes0       = g_closes;
    phantom_session_ended(PHANTOM_END_TIMEOUT); /* a stall... */
    phantom_session_alive(PHANTOM_ALIVE_ACTIVITY); /* ...that ended on its own */
    pump(GRACE + 400);
    check(phantom_owns_screen() == 0 && g_closes == closes0, "a stalled client that resumes cancels the wait");
    phantom_session_ended(PHANTOM_END_CLOSE);
    phantom_session_alive(PHANTOM_ALIVE_ACTIVITY); /* a stray datagram after CMD_CLOSE */
    pump(GRACE + 400);
    check(phantom_owns_screen() == 1 && g_closes == closes0 + 1,
          "...but a stray datagram after CMD_CLOSE does not cancel the close");

    /* 7g: nothing to protect once the launcher is up. */
    check(phantom_session_ended(PHANTOM_END_CLOSE) == 0, "with the launcher already showing, the caller just closes");

    /* ---------------------------------------------------------------------------- */
    section("8. a library of 50,000 titles, fetched a page at a time");
    phantom_idle_leave();   /* put a game on, and take the screen back, to start from a known place */
    pump(100);
    phantom_idle_enter();
    pump(2200);             /* past the takeover guard */

    ph_ui rows_probe;
    ph_ui_init(&rows_probe, 0, PH_CANVAS_W, PH_CANVAS_H);
    const int ROWS = ph_ui_rows(&rows_probe);
    check(ROWS > 5, "the list shows %d rows", ROWS);

    const unsigned BIGN = 50000;
    auto big = make_big(BIGN, 777);
    set_ix(big);
    tap(GJ_Y);
    pump(700);
    check(phantom_title_count() == (int) BIGN, "the cabinet reports %d titles (expected %u)", phantom_title_count(), BIGN);

    /* bring the cursor to the first title of the All tab */
    Pos p = launch_here();
    for (int i = 0; i < 12 && p.ok && p.list != "*"; i++) {
        tap(GJ_R);
        pump(60);
        p = launch_here();
    }
    tap(GJ_R);
    pump(60);
    tap(GJ_L);
    pump(150);
    p = launch_here();
    check(p.ok && p.ver == 777 && p.list == "*" && p.index == 0, "the first launch quotes version %u, list '%s', index %d", p.ver, p.list.c_str(), p.index);

    for (int i = 0; i < 3; i++) tap(JOY_DOWN, 30);
    p = launch_here();
    check(p.ok && p.index == 3, "three Downs: index %d", p.index);

    tap(JOY_RIGHT, 30);
    pump(100);
    p = launch_here();
    check(p.ok && p.index == 3 + ROWS, "one page right: index %d (expected %d)", p.index, 3 + ROWS);

    /* letters: R2 goes to the start of the next letter that has titles; the host's table says where */
    {
        const auto &starts = big->letters[0];
        int next = -1;
        for (int k = 0; k < 27; k++)
            if (starts[k] < starts[k + 1] && (int) starts[k] > p.index) { next = (int) starts[k]; break; }
        tap(GJ_R2, 30);
        pump(100);
        Pos q = launch_here();
        check(q.ok && q.index == next, "R2: index %d (the host's table says the next letter starts at %d)", q.index, next);

        tap(GJ_R2, 30);
        pump(100);
        Pos r = launch_here();
        check(r.ok && r.index > q.index, "R2 again goes on: %d", r.index);
        tap(GJ_L2, 30);
        pump(100);
        Pos s = launch_here();
        check(s.ok && s.index == q.index, "L2 at the top of a letter goes to the top of the one before it (%d, expected %d)", s.index, q.index);
    }

    /* holding Right crosses the library at an increasing pace, and the rows are there when it stops */
    {
        Pos before = launch_here();
        int lists0 = count_rx("LIST:");
        pad(JOY_RIGHT);
        pump(2500);
        pad(0);
        pump(150);
        Pos after = launch_here();
        int moved = after.index - before.index;
        std::printf("  (holding Right for 2.5 s moved %d titles and asked for %d pages)\n", moved, count_rx("LIST:") - lists0);
        check(after.ok, "the rows were already there when the cursor stopped");
        check(moved > 40 * ROWS, "holding Right for 2.5 s moved only %d titles", moved);
        check(count_rx("LIST:") - lists0 < 400, "that many titles cost %d page requests", count_rx("LIST:") - lists0);
    }

    /* a tab is a different list on the host */
    tap(GJ_R, 30);
    pump(100);
    p = launch_here();
    check(p.ok && p.list == "groovymame" && p.index == 0, "next tab: list '%s', index %d", p.list.c_str(), p.index);

    /* a page that never arrives is asked for again */
    {
        g_drop_lists = 3;
        tap(GJ_R2, 30);
        pump(80);
        int n0 = count_rx("LAUNCH:@");
        tap(JOY_BTN1, 30);
        check(count_rx("LAUNCH:@") == n0, "while its page is missing, A does not launch an unseen row");
        pump(900);
        Pos q = launch_here();
        check(q.ok, "the page was asked for again and arrived");
    }

    /* the host rescans: the next thing the cabinet asks is refused, and it picks up the new library */
    {
        auto big2 = make_big(49000, 888);
        set_ix(big2);
        /* pages already held keep showing; the cabinet finds out when it next asks for one it does not have */
        tap(GJ_R2, 30);
        tap(GJ_R2, 30);
        pump(600);
        check(phantom_title_count() == 49000, "after the host's rescan the cabinet reports %d titles", phantom_title_count());
        Pos q = launch_here();
        check(q.ok && q.ver == 888, "...and launches quote the new version (%u)", q.ver);
    }

    /* ...and a launch against a library that changed under it is refused, not misdirected */
    {
        auto big3 = make_big(48000, 999);
        set_ix(big3);
        int n0 = count_rx("LAUNCH:@");
        tap(JOY_BTN1, 30);
        pump(300);
        check(count_rx("LAUNCH:@") == n0 + 1, "the launch was sent (quoting the old version)");
        check(phantom_title_count() == 48000, "the refusal made the cabinet fetch the new library (%d titles)", phantom_title_count());
        Pos q = launch_here();
        check(q.ok && q.ver == 999, "it is back at the list, ready to launch against version 999 (got %u)", q.ver);
    }

    /* a host that does not know the paged catalog is still served, the older way */
    {
        g_host_paged = 0;
        int cat0 = count_rx("GET_CATALOG");
        tap(GJ_Y);
        pump(5500);
        check(count_rx("GET_CATALOG") > cat0, "an older host was asked for its whole catalog");
        check(phantom_title_count() == 3, "...and its %d titles are shown", phantom_title_count());
        int l0 = count_rx("LAUNCH:");
        tap(JOY_BTN1, 30);
        check(count_rx("LAUNCH:") == l0 + 1 && last_rx("LAUNCH:").rfind("LAUNCH:@", 0) != 0,
              "launching from it uses the id, not a position (%s)", last_rx("LAUNCH:").c_str());
        tap(JOY_BTN2, 30); /* B: back out of the loading screen */
        g_host_paged = 1;
    }

    /* The daemon is restarted as a newer build, with nobody pressing anything: the cabinet is still online the
     * whole time, so nothing looks like "a host has appeared". It has to notice for itself. */
    {
        pump(4000); /* the probe interval is shortened for the test */
        check(phantom_title_count() == 48000, "a host that learned to page was noticed without a refresh (%d titles)", phantom_title_count());
        Pos q = launch_here();
        check(q.ok && q.ver == 999, "and launches are by position again (version %u)", q.ver);
    }

    /* ...and the other way: a paged host replaced, silently, by an older one. */
    {
        g_host_paged = 0;
        pump(9000);
        check(phantom_title_count() == 3, "a host that stopped answering CATINFO was given up on (%d titles)", phantom_title_count());
        g_host_paged = 1;
        pump(4000);
        check(phantom_title_count() == 48000, "and found again when it speaks the paged catalog (%d titles)", phantom_title_count());
    }

    /* ---------------------------------------------------------------------------- */
    section("9. a picture behind the menu");
    {
        auto write_ppm = [](const char *path, int r, int g, int b, int w = 720, int h = 480) {
            FILE *f = fopen(path, "wb");
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) {
                fputc(r, f);
                fputc(g, f);
                fputc(b, f);
            }
            fclose(f);
        };
        auto margin = [&](int &b, int &g, int &r) { /* a pixel in the left margin: no panel covers it */
            const uint8_t *p = &g_canvas[(300 * 720 + 4) * 3];
            b = p[0]; g = p[1]; r = p[2];
        };
        const char *bgpath = TEST_DIR "/bg.ppm";
        unlink(bgpath);

        phantom_idle_leave();
        pump(50);
        phantom_idle_enter();
        int b, g, r;
        margin(b, g, r);
        check(b == 0x12 && g == 0x0C && r == 0x0B, "with no picture the menu is on its flat background (%02X %02X %02X)", b, g, r);

        phantom_idle_leave();
        write_ppm(bgpath, 200, 100, 40);
        pump(50);
        phantom_idle_enter();
        margin(b, g, r);
        check(b == 20 && g == 50 && r == 100, "a picture appears behind the menu, dimmed to half (B%d G%d R%d, expected 20 50 100)", b, g, r);

        /* a picture swapped for another is picked up the next time the menu comes back, with nothing restarted */
        phantom_idle_leave();
        write_ppm(bgpath, 10, 240, 120);
        pump(50);
        phantom_idle_enter();
        margin(b, g, r);
        check(b == 60 && g == 120 && r == 5, "a replaced picture is picked up (B%d G%d R%d, expected 60 120 5)", b, g, r);

        /* a file that is not a 720x480 PPM is ignored, and the one already held stays */
        phantom_idle_leave();
        write_ppm(bgpath, 1, 2, 3, 100, 100);
        pump(50);
        phantom_idle_enter();
        margin(b, g, r);
        check(b == 60 && g == 120 && r == 5, "a picture of the wrong size is ignored and the one already held stays (B%d G%d R%d)", b, g, r);
        check(phantom_owns_screen() == 1, "and the launcher is up (%d)", phantom_owns_screen());

        /* take the picture away: the menu is as it always was */
        phantom_idle_leave();
        unlink(bgpath);
        pump(50);
        phantom_idle_enter();
        margin(b, g, r);
        check(b == 0x12 && g == 0x0C && r == 0x0B, "without the file the menu is flat again (%02X %02X %02X)", b, g, r);

        /* and the menu is still usable with a picture behind it */
        write_ppm(bgpath, 90, 90, 90);
        phantom_idle_leave();
        pump(50);
        phantom_idle_enter();
        pump(2200);
        int l0 = count_rx("LAUNCH:");
        tap(JOY_BTN1, 30);
        check(count_rx("LAUNCH:") == l0 + 1, "launching works with a picture behind the menu");
        tap(JOY_BTN2, 30);
        unlink(bgpath);
    }

    /* ---------------------------------------------------------------------------- */
    section("10. a launch the PC could not carry out is reported, not waited on");
    {
        phantom_idle_leave();
        pump(50);
        phantom_idle_enter();
        pump(2200); /* past the takeover guard */
        int n0 = count_rx("LAUNCH:");
        g_fail_launch = 1;
        tap(JOY_BTN1, 30); /* A: launch; the host says OK, then that it could not */
        check(count_rx("LAUNCH:") == n0 + 1, "the launch was sent");
        pump(500);
        /* back at the list: pressing A launches again (on the loading screen it would only cancel) */
        int n1 = count_rx("LAUNCH:");
        tap(JOY_BTN1, 30);
        check(count_rx("LAUNCH:") == n1 + 1, "after the failure report the launcher is back at the list, not still waiting");
        tap(JOY_BTN2, 30);
    }

    g_stop = true;
    host.join();
    std::printf("\n%d check(s) failed\n", g_fail);
    return g_fail;
}
