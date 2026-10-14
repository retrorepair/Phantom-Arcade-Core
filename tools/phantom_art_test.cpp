/*
 * phantom_art_test.cpp - the cabinet's picture client (phantom_art.c) against a fake host.
 *
 * The client is a non-blocking HTTP client that is driven by calling pa_tick() with the time. Here a thread plays the
 * host on loopback with a different behaviour per row (the position in the query): a picture, "fetching" a few times
 * first, no picture, a changed library, a picture sent a few bytes at a time, one cut off half way, a host that says
 * nothing. The clock the client sees runs ten times faster than the real one, so its eight-second timeouts take under
 * a second to test.
 *
 * Checked: nothing is asked for until the cursor has rested; a row the cursor has left is never asked for; every
 * answer (200, 202, 404, 409) is handled; a picture that arrives in pieces is whole; a transfer that fails is retried
 * and then given up on rather than hammered; a stall is noticed; changing the wanted row or the host drops what was in
 * flight; and bad answers (not HTTP, no end to the headers, the wrong length) are refused. Built with the address and
 * undefined-behaviour sanitizers (tools/run_phantom_art_test.sh).
 *
 * Exit status is the number of failed checks.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include "phantom_art.h"
}

static int checks = 0, fails = 0;
#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

/* ---- the fake host ------------------------------------------------------------------------------ */

enum Mode { M_OK, M_PENDING_THEN_OK, M_NONE, M_STALE, M_DRIBBLE, M_CUT_THEN_OK, M_SILENT, M_CLOSE, M_GARBAGE, M_NOEND, M_SHORT, M_BAD400 };

struct Host
{
    int lsock = -1;
    int port = 0;
    std::thread th;
    std::atomic<bool> quit{ false };
    std::mutex m;
    std::map<int, Mode> modes;          // row index -> behaviour
    std::map<int, int> hits;            // row index -> requests seen
    std::vector<int> order;             // rows in the order they were asked for
    std::vector<int> silent_fds;

    void start()
    {
        lsock = socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(lsock, (sockaddr *)&a, sizeof(a));
        socklen_t l = sizeof(a);
        getsockname(lsock, (sockaddr *)&a, &l);
        port = ntohs(a.sin_port);
        listen(lsock, 16);
        th = std::thread([this] { loop(); });
    }

    void stop()
    {
        quit = true;
        shutdown(lsock, SHUT_RDWR);
        close(lsock);
        if (th.joinable()) th.join();
        for (int fd : silent_fds) close(fd);
    }

    int seen(int row) { std::lock_guard<std::mutex> lk(m); return hits[row]; }
    size_t total() { std::lock_guard<std::mutex> lk(m); return order.size(); }

    static void send_all(int fd, const void *p, size_t n)
    {
        const char *c = (const char *)p;
        while (n)
        {
            ssize_t k = send(fd, c, n, MSG_NOSIGNAL);
            if (k <= 0) return;
            c += k;
            n -= (size_t)k;
        }
    }

    static void send_picture(int fd, uint8_t shade, bool dribble, bool cut)
    {
        static const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Phantom-Art: 720x480-bgr\r\nContent-Length: 1036800\r\nConnection: close\r\n\r\n";
        std::vector<uint8_t> body(PA_BYTES, shade);
        if (dribble)
        {
            for (size_t i = 0; i + 1 <= sizeof(head) - 1; i += 7)
            {
                send_all(fd, head + i, std::min<size_t>(7, sizeof(head) - 1 - i));
                usleep(300);
            }
            for (size_t off = 0; off < body.size(); off += 150000)
            {
                send_all(fd, body.data() + off, std::min<size_t>(150000, body.size() - off));
                usleep(500);
            }
            return;
        }
        send_all(fd, head, sizeof(head) - 1);
        send_all(fd, body.data(), cut ? body.size() / 2 : body.size());
    }

    void serve(int fd)
    {
        char buf[2048];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(fd); return; }
        buf[n] = 0;
        int row = -1;
        if (const char *q = strstr(buf, "i=")) row = atoi(q + 2);
        Mode mode;
        int count;
        {
            std::lock_guard<std::mutex> lk(m);
            order.push_back(row);
            count = ++hits[row];
            mode = modes.count(row) ? modes[row] : M_OK;
        }
        const uint8_t shade = (uint8_t)(row & 0xFF);
        switch (mode)
        {
        case M_OK:      send_picture(fd, shade, false, false); break;
        case M_DRIBBLE: send_picture(fd, shade, true, false); break;
        case M_PENDING_THEN_OK:
            if (count <= 2) { static const char r[] = "HTTP/1.1 202 Accepted\r\nRetry-After: 2\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); }
            else send_picture(fd, shade, false, false);
            break;
        case M_NONE:  { static const char r[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); break; }
        case M_STALE: { static const char r[] = "HTTP/1.1 409 Conflict\r\nX-Phantom-Version: 9\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); break; }
        case M_BAD400:{ static const char r[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); break; }
        case M_CUT_THEN_OK:
            send_picture(fd, shade, false, count == 1);
            break;
        case M_SILENT: { std::lock_guard<std::mutex> lk(m); silent_fds.push_back(fd); return; }   // kept open, never answered
        case M_CLOSE:   break;
        case M_GARBAGE: { static const char r[] = "this is not http at all\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); break; }
        case M_NOEND:   { std::string r(1500, 'x'); send_all(fd, r.data(), r.size()); break; }
        case M_SHORT:   { static const char r[] = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n"; send_all(fd, r, sizeof(r) - 1); std::string b(1000, 'z'); send_all(fd, b.data(), b.size()); break; }
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    void loop()
    {
        while (!quit)
        {
            sockaddr_in c{};
            socklen_t cl = sizeof(c);
            int fd = accept(lsock, (sockaddr *)&c, &cl);
            if (fd < 0) { if (quit) return; continue; }
            std::thread([this, fd] { serve(fd); }).detach();
        }
    }
};

/* ---- driving the client on a fast clock ------------------------------------------------------------ */

struct Clock
{
    uint32_t now = 1000;
    void step(pa_art *a, int fake_ms, int *changed = nullptr)
    {
        for (int t = 0; t < fake_ms; t += 10)
        {
            now += 10;
            const int c = pa_tick(a, now);
            if (changed) *changed += c;
            usleep(1000);   // one real millisecond per ten simulated
        }
    }

    // Run until the predicate holds or `limit` simulated milliseconds pass; returns simulated ms used, or -1.
    template <class F> int until(pa_art *a, int limit, F pred)
    {
        for (int t = 0; t < limit; t += 10)
        {
            if (pred()) return t;
            step(a, 10);
        }
        return pred() ? limit : -1;
    }
};

static std::string q(int row) { return "v=1&l=*&i=" + std::to_string(row) + "&k=title"; }
static std::string key(int row) { return "1:*:" + std::to_string(row) + ":title"; }

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v) return false;
    return true;
}

static void want(pa_art *a, Clock &c, int row) { pa_want(a, key(row).c_str(), q(row).c_str(), c.now); }

static void test_basic()
{
    Host h;
    h.start();
    pa_art a;
    pa_init(&a);
    pa_set_host(&a, "127.0.0.1", h.port);
    Clock c;

    CHECK(pa_get_status(&a) == PA_NOTHING && !pa_picture(&a), "starts with nothing");

    // nothing is asked for until the cursor has rested
    want(&a, c, 1);
    c.step(&a, 200);
    CHECK(h.total() == 0, "no request inside the debounce (%zu)", h.total());
    int ready = c.until(&a, 3000, [&] { return pa_get_status(&a) == PA_READY; });
    CHECK(ready >= 0, "the picture arrives");
    CHECK(h.seen(1) == 1, "after one request (%d)", h.seen(1));
    CHECK(pa_picture(&a) && all_bytes(pa_picture(&a), PA_BYTES, 1), "and is whole");
    CHECK(!strcmp(pa_picture_key(&a), key(1).c_str()), "filed under its row");
    CHECK(a.pics == 1, "counted");

    // the same row again changes nothing
    want(&a, c, 1);
    c.step(&a, 800);
    CHECK(h.seen(1) == 1 && pa_get_status(&a) == PA_READY, "asking again for the held row does not fetch it again");

    // a row the cursor passes through is never asked for
    want(&a, c, 2);
    c.step(&a, 100);
    want(&a, c, 3);
    c.step(&a, 100);
    want(&a, c, 4);
    c.until(&a, 3000, [&] { return pa_get_status(&a) == PA_READY && !strcmp(pa_picture_key(&a), key(4).c_str()); });
    CHECK(h.seen(2) == 0 && h.seen(3) == 0 && h.seen(4) == 1, "only the row it stopped on was asked for (%d %d %d)", h.seen(2), h.seen(3), h.seen(4));
    CHECK(all_bytes(pa_picture(&a), PA_BYTES, 4), "and it is that row's picture");

    // going back to a row whose picture is the one held needs no request
    const size_t t0 = h.total();
    want(&a, c, 4);
    CHECK(h.total() == t0, "same row");

    // nothing wanted: the picture stays, the status is NOTHING
    pa_want(&a, nullptr, nullptr, c.now);
    CHECK(pa_get_status(&a) == PA_NOTHING && pa_picture(&a) && all_bytes(pa_picture(&a), PA_BYTES, 4), "unwanted: status NOTHING, the picture is kept");

    pa_free(&a);
    h.stop();
}

static void test_answers()
{
    Host h;
    h.modes[10] = M_PENDING_THEN_OK;
    h.modes[11] = M_NONE;
    h.modes[12] = M_STALE;
    h.modes[13] = M_DRIBBLE;
    h.modes[14] = M_CUT_THEN_OK;
    h.start();
    pa_art a;
    pa_init(&a);
    pa_set_host(&a, "127.0.0.1", h.port);
    Clock c;

    // 202 twice, then 200
    want(&a, c, 10);
    bool saw_pending = false;
    int t = 0;
    while (t < 20000 && pa_get_status(&a) != PA_READY)
    {
        c.step(&a, 10);
        t += 10;
        saw_pending |= (pa_get_status(&a) == PA_PENDING);
    }
    CHECK(saw_pending, "'fetching' is reported while the host works");
    CHECK(pa_get_status(&a) == PA_READY && h.seen(10) == 3, "and the third ask gets the picture (%d asks)", h.seen(10));
    CHECK(all_bytes(pa_picture(&a), PA_BYTES, 10), "whole");

    // 404 keeps the earlier picture, and says NONE
    want(&a, c, 11);
    c.until(&a, 3000, [&] { return pa_get_status(&a) == PA_NONE; });
    CHECK(pa_get_status(&a) == PA_NONE, "no picture for it");
    CHECK(pa_picture(&a) && !strcmp(pa_picture_key(&a), key(10).c_str()), "the picture from the row before is still there");
    c.step(&a, 3000);
    CHECK(h.seen(11) == 1, "and it is not asked about again (%d)", h.seen(11));

    // 409: the library changed
    want(&a, c, 12);
    c.until(&a, 3000, [&] { return h.seen(12) >= 1; });
    c.step(&a, 300);
    CHECK(pa_take_stale(&a) == 1, "409 sets the stale flag");
    CHECK(pa_take_stale(&a) == 0, "and taking it clears it");
    c.step(&a, 3000);
    CHECK(h.seen(12) == 1 && pa_get_status(&a) == PA_NOTHING, "a stale row is not hammered");

    // a picture sent in pieces
    want(&a, c, 13);
    c.until(&a, 10000, [&] { return pa_get_status(&a) == PA_READY && !strcmp(pa_picture_key(&a), key(13).c_str()); });
    CHECK(pa_get_status(&a) == PA_READY && all_bytes(pa_picture(&a), PA_BYTES, 13), "headers and body in dribbles still make a whole picture");

    // cut off half way, then complete on the retry
    want(&a, c, 14);
    c.until(&a, 20000, [&] { return pa_get_status(&a) == PA_READY && !strcmp(pa_picture_key(&a), key(14).c_str()); });
    CHECK(h.seen(14) == 2, "a truncated transfer is retried (%d asks)", h.seen(14));
    CHECK(pa_get_status(&a) == PA_READY && all_bytes(pa_picture(&a), PA_BYTES, 14), "and the retry is whole");

    pa_free(&a);
    h.stop();
}

static void test_failures()
{
    Host h;
    h.modes[20] = M_CLOSE;
    h.modes[21] = M_GARBAGE;
    h.modes[22] = M_NOEND;
    h.modes[23] = M_SHORT;
    h.modes[24] = M_SILENT;
    h.modes[25] = M_BAD400;
    h.start();
    pa_art a;
    pa_init(&a);
    pa_set_host(&a, "127.0.0.1", h.port);
    Clock c;

    for (int row : { 20, 21, 22, 23, 25 })
    {
        want(&a, c, row);
        c.step(&a, 20000);
        CHECK(h.seen(row) == 4, "row %d: a bad answer is tried %d times, a second apart, and then slows down (4 expected)", row, h.seen(row));
        CHECK(pa_get_status(&a) != PA_READY, "row %d: and is not taken for a picture", row);
        CHECK(!pa_picture(&a), "row %d: no picture appeared", row);
    }

    // ...and goes on asking, slowly: a host that was only restarting is found again without the cursor having to move
    want(&a, c, 20);
    c.step(&a, 40000);
    CHECK(h.seen(20) >= 9 && h.seen(20) <= 11, "a failing row is asked for again every 15 s once the quick tries are spent (%d asks: 4 before, 4 quick + 2 slow now)", h.seen(20));
    h.modes[20] = M_OK;
    c.until(&a, 20000, [&] { return pa_get_status(&a) == PA_READY; });
    CHECK(pa_get_status(&a) == PA_READY && all_bytes(pa_picture(&a), PA_BYTES, 20), "and when the host comes right the picture arrives, with no cursor movement");

    // the rest of this test wants a client that has never had a picture
    pa_free(&a);
    pa_init(&a);
    pa_set_host(&a, "127.0.0.1", h.port);

    // a host that accepts and says nothing: the stall is noticed, by the clock the client is given
    want(&a, c, 24);
    int t = c.until(&a, 40000, [&] { return h.seen(24) >= 2; });
    CHECK(t >= 0, "a silent host is given up on and the row asked for again");
    CHECK(t >= 8000, "but not before the timeout (%d ms)", t);

    // nothing listening at all
    pa_set_host(&a, "127.0.0.1", 1);   // privileged port, closed
    want(&a, c, 30);
    c.step(&a, 20000);
    CHECK(!pa_picture(&a) && pa_get_status(&a) != PA_READY, "a closed port gives no picture and no spin");

    // a host that is not an address
    pa_set_host(&a, "not-an-ip", 8088);
    want(&a, c, 31);
    c.step(&a, 2000);
    CHECK(!pa_picture(&a), "a bad host is ignored");
    pa_set_host(&a, "", 8088);
    want(&a, c, 32);
    c.step(&a, 2000);
    CHECK(!pa_picture(&a), "no host is ignored");

    pa_free(&a);
    h.stop();
}

static void test_drops()
{
    Host h;
    h.modes[40] = M_SILENT;
    h.start();
    pa_art a;
    pa_init(&a);
    pa_set_host(&a, "127.0.0.1", h.port);
    Clock c;

    // moving on while a transfer is in flight drops it, and the new row is what arrives
    want(&a, c, 40);
    c.until(&a, 3000, [&] { return h.seen(40) >= 1; });
    want(&a, c, 41);
    c.until(&a, 4000, [&] { return pa_get_status(&a) == PA_READY; });
    CHECK(pa_get_status(&a) == PA_READY && !strcmp(pa_picture_key(&a), key(41).c_str()), "the row the cursor moved to is what arrives");

    // not wanting anything cancels
    want(&a, c, 40);
    c.until(&a, 3000, [&] { return h.seen(40) >= 2; });
    pa_want(&a, nullptr, nullptr, c.now);
    const size_t t0 = h.total();
    c.step(&a, 20000);
    CHECK(h.total() == t0, "cancelling stops the asking");

    // changing the host drops the old one and asks the new, at once
    Host h2;
    h2.start();
    want(&a, c, 42);
    c.step(&a, 100);
    pa_set_host(&a, "127.0.0.1", h2.port);
    c.until(&a, 4000, [&] { return pa_get_status(&a) == PA_READY && !strcmp(pa_picture_key(&a), key(42).c_str()); });
    CHECK(h2.seen(42) == 1 && h.seen(42) == 0, "the new host was asked, the old one never (%d, %d)", h2.seen(42), h.seen(42));

    // the same host again changes nothing
    const size_t t1 = h2.total();
    pa_set_host(&a, "127.0.0.1", h2.port);
    c.step(&a, 1000);
    CHECK(h2.total() == t1, "setting the same host is a no-op");

    pa_free(&a);
    h2.stop();
    h.stop();
}

int main()
{
    test_basic();
    test_answers();
    test_failures();
    test_drops();
    std::printf("%d check(s), %d failed\n", checks, fails);
    return fails;
}
