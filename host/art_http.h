/*
 * art_http.h - what the daemon says to the cabinet about artwork, with no sockets in it.
 *
 * Two surfaces, both small:
 *
 *   HTTP, on the port the catalog already uses (8088), because a picture is a megabyte and a datagram is not:
 *
 *     GET /art?v=<library version>&l=<list key>&i=<index in that list>&k=title|snap
 *
 *     200  the picture for that row, 720*480*3 bytes of B,G,R, ready to draw   (X-Phantom-Art: 720x480-bgr)
 *     202  not here yet, being fetched; ask again in a couple of seconds        (Retry-After: 2)
 *     404  there is no picture for it, and none is coming
 *     409  the library has changed since the cabinet's page was fetched; the row may be another game now
 *     400  the request does not make sense
 *
 *     The row is named the way a launch names it - version, list, position - because that is all the cabinet holds
 *     (it pages the list; it does not keep every id). A 200 or 202 also quietly starts fetching the rows around it,
 *     so scrolling finds the pictures already there.
 *
 *   UDP, on the daemon's usual port, for the settings menu and its numbers:
 *
 *     ART:INFO                  -> ART:INFO:<mask>:<titles>:<snaps>:<busy>:<online>:<bulk>:<done>:<total>
 *     ART:SET:<mask>            set what may be downloaded (bit 0 titles, bit 1 snaps), then answer as INFO
 *     ART:ALL:<1|0>             start / stop downloading pictures for the whole library, then answer as INFO
 *
 *     mask 0-3; titles/snaps are how many are on the PC; busy is queued + running fetches; online is whether the last
 *     request to the picture server worked; bulk is 1 while a whole-library download is running, with its progress.
 *
 * Portable, so art_http_test.cpp drives both against a real index and a fake picture server.
 */
#ifndef PHANTOM_ART_HTTP_H
#define PHANTOM_ART_HTTP_H

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "art_store.h"
#include "game_index.h"

namespace phantom {
namespace art {

struct HttpRequest {
    bool ok = false;
    std::string method, path, query;
};

// "GET /art?v=1&l=* HTTP/1.1\r\nHost: x\r\n\r\n" -> method GET, path /art, query v=1&l=*
inline HttpRequest ParseRequest(std::string_view head) {
    HttpRequest r;
    const size_t eol = head.find_first_of("\r\n");
    std::string_view line = head.substr(0, eol);
    const size_t a = line.find(' ');
    if (a == std::string_view::npos || a == 0) return r;
    const size_t b = line.find(' ', a + 1);
    std::string_view target = line.substr(a + 1, b == std::string_view::npos ? std::string_view::npos : b - a - 1);
    if (target.empty() || target[0] != '/') return r;
    r.method = std::string(line.substr(0, a));
    const size_t q = target.find('?');
    r.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos) r.query = std::string(target.substr(q + 1));
    r.ok = true;
    return r;
}

// The value of a query parameter, percent-decoded ("" when it is not there).
inline std::string QueryParam(std::string_view query, std::string_view key) {
    size_t p = 0;
    while (p <= query.size()) {
        size_t e = query.find('&', p);
        if (e == std::string_view::npos) e = query.size();
        std::string_view kv = query.substr(p, e - p);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == key) {
            std::string v(kv.substr(eq + 1));
            for (char& c : v)
                if (c == '+') c = ' ';
            return PercentDecode(v);
        }
        p = e + 1;
    }
    return std::string();
}

struct ArtReply {
    int code = 500;
    std::string reason = "Internal Server Error";
    std::string headers;            // extra header lines, each ending \r\n
    std::vector<uint8_t> body;
};

inline bool ToUint(const std::string& s, uint32_t& v) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (uint64_t)(c - '0');
    }
    if (n > 0xFFFFFFFFull) return false;
    v = (uint32_t)n;
    return true;
}

inline GameRef RefOf(const Index& ix, const Rec& r) {
    return GameRef{ ix.Id(r), ix.Title(r), r.sys < ix.sys.size() ? ix.sys[r.sys].key : std::string(), ix.Rom(r) };
}

// Answer GET /art?... against the current library.
inline ArtReply HandleArtRequest(Store& store, const std::shared_ptr<const Index>& ix, std::string_view query) {
    ArtReply out;
    uint32_t ver = 0, at = 0;
    const std::string kind = QueryParam(query, "k"), list = QueryParam(query, "l");
    if (!ToUint(QueryParam(query, "v"), ver) || !ToUint(QueryParam(query, "i"), at) || list.empty() ||
        (kind != "title" && kind != "snap")) {
        out.code = 400; out.reason = "Bad Request";
        return out;
    }
    if (!ix) { out.code = 503; out.reason = "Service Unavailable"; return out; }
    if (ix->ver != ver) {
        out.code = 409; out.reason = "Conflict";
        out.headers = "X-Phantom-Version: " + std::to_string(ix->ver) + "\r\n";
        return out;
    }
    const int li = ix->ListFor(list);
    const Rec* rec = ix->RecAt(li, at);
    if (!rec) { out.code = 404; out.reason = "Not Found"; return out; }

    const Kind k = kind == "title" ? kTitle : kSnap;
    std::vector<uint8_t> canvas;
    const Got got = store.Get(k, RefOf(*ix, *rec), &canvas);

    if (got != Got::None) {
        // The rows the cursor is about to reach: a few behind, more ahead (it mostly moves down).
        for (int d = -3; d <= 6; d++) {
            if (d == 0) continue;
            const int64_t j = (int64_t)at + d;
            if (j < 0) continue;
            const Rec* n = ix->RecAt(li, (size_t)j);
            if (n) store.Prefetch(k, RefOf(*ix, *n));
        }
    }

    switch (got) {
    case Got::Ready:
        out.code = 200; out.reason = "OK";
        out.headers = "Content-Type: application/octet-stream\r\nX-Phantom-Art: 720x480-bgr\r\n";
        out.body = std::move(canvas);
        break;
    case Got::Pending:
        out.code = 202; out.reason = "Accepted";
        out.headers = "Retry-After: 2\r\n";
        break;
    default:
        out.code = 404; out.reason = "Not Found";
        break;
    }
    return out;
}

// The status line and headers for a reply, ready to send ahead of its body.
inline std::string ReplyHead(const ArtReply& r) {
    return "HTTP/1.1 " + std::to_string(r.code) + " " + r.reason + "\r\n" + r.headers +
           "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
}

inline std::string InfoLine(Store& store) {
    const Stats s = store.GetStats();
    return "ART:INFO:" + std::to_string(s.fetch_mask) + ":" + std::to_string(s.have[kTitle]) + ":" + std::to_string(s.have[kSnap]) + ":" +
           std::to_string(s.queued + s.active) + ":" + (s.online ? "1" : "0") + ":" + (s.bulk_running ? "1" : "0") + ":" +
           std::to_string(s.bulk_done) + ":" + std::to_string(s.bulk_total);
}

// Answer an ART: datagram. Empty when it is not one (or not understood). `persist` is told the new mask when it
// changes, so the daemon can write it down.
inline std::string HandleArtCommand(Store& store, const std::shared_ptr<const Index>& ix, const std::string& msg,
                                    const std::function<void(int)>& persist) {
    if (msg.rfind("ART:", 0) != 0) return std::string();
    std::string m = msg;
    while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ')) m.pop_back();

    if (m == "ART:INFO") return InfoLine(store);

    if (m.rfind("ART:SET:", 0) == 0) {
        uint32_t mask = 0;
        if (!ToUint(m.substr(8), mask) || mask > 3) return "ART:BAD";
        store.SetFetchMask((int)mask);
        if (persist) persist((int)mask);
        return InfoLine(store);
    }

    if (m == "ART:ALL:0") {
        store.BulkStop();
        return InfoLine(store);
    }
    if (m == "ART:ALL:1") {
        if (!ix || ix->Total() == 0) return InfoLine(store);
        std::shared_ptr<const Index> hold = ix;   // the walk outlives this call; keep the library it walks
        store.BulkStart(
            [hold](size_t i, GameRef& g) {
                if (i >= hold->Total()) return false;
                g = RefOf(*hold, hold->recs[hold->lists[0][i]]);
                return true;
            },
            ix->Total());
        return InfoLine(store);
    }
    return std::string();
}

}  // namespace art
}  // namespace phantom

#endif
