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
#include "user_io.h" /* JOY_* - the same bit values the core uses (found via -I hps_linux/src) */

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
static std::atomic<int> g_closes{0};      /* times the core was told to really close the session */
static std::atomic<int> g_status_mode{0}; /* what the fake host says to STATUS: 0 nothing (an old
                                           * host), 1 RUNNING, 2 IDLE */

extern "C" {
uint32_t groovy_idle_canvas_bytes(void) { return 720u * 480u * 3u; }
int      groovy_idle_begin(void) { return 1; }
void     groovy_idle_present(const uint8_t *) {}
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
            sendto(fd, CATALOG, strlen(CATALOG), 0, (sockaddr *) &from, fl);
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
    pad(JOY_START | JOY_SELECT);
    pump(1300);
    pad(JOY_START | JOY_SELECT | JOY_BTN1); /* the third event is what completes the hold */
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

    g_stop = true;
    host.join();
    std::printf("\n%d check(s) failed\n", g_fail);
    return g_fail;
}
