/*
 * game_index.h - the library as the cabinet browses it.
 *
 * The library used to be one JSON document: written whole by every scan, parsed whole on every
 * launch (a substring search through the text), and sent to the cabinet in a single datagram
 * capped at 60,000 bytes - about 325 games. That cannot survive a library of hundreds of
 * thousands of titles, so this is the replacement:
 *
 *   - Records live in one string pool plus a table of fixed-size structs, about 150 bytes a game,
 *     so 300,000 games is tens of megabytes, built with a few allocations rather than millions.
 *   - Every list the cabinet can ask for (one per system, and "all") is a sorted array of record
 *     numbers, built once when the scan publishes. The cabinet fetches a page of it at a time, so
 *     what it holds and what crosses the wire do not depend on the size of the library.
 *   - Each list carries a 27-entry letter table, so jumping to "S" is a lookup, not a search.
 *   - A published index is immutable and carries a version. The cabinet quotes the version in
 *     every request; if the library has changed in the meantime the host says STALE instead of
 *     returning rows from a list that has shifted under it - or launching the wrong game.
 *   - The index saves to and loads from one file in a single read, so the host can serve the
 *     last scan the moment it starts and rescan behind it.
 *
 * Portable C++17, no Windows headers: the same code is built by the daemon, by this directory's
 * test, and by the cabinet-side protocol test, so the wire format is exercised by both ends.
 *
 * Wire format (UDP, ASCII, one request one datagram; <list> is "*" for every game, or a system key):
 *   CATINFO                                 -> CATINFO:<ver>:<total>:<scanning 0|1>:<key>|<name>|<count>;...
 *   LETTERS:<ver>:<list>                    -> LETTERS:<ver>:<list>:<s0>,<s1>,...,<s27>
 *                                              s0 = first row filed under '#', s1..s26 under A..Z, s27 = total
 *   LIST:<ver>:<list>:<offset>:<count>      -> LIST:<ver>:<list>:<offset>:<n>:<total>\n then n rows of
 *                                              id TAB title TAB system TAB mode TAB year TAB maker LF
 *   LAUNCH:@<ver>:<list>:<index>[:kbd=N]    handled by the daemon (Resolve)
 *   anything quoting a version that is no longer current -> STALE:<current ver>
 */
#ifndef PHANTOM_GAME_INDEX_H
#define PHANTOM_GAME_INDEX_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phantom {

// ---- the order titles are listed in ----------------------------------------------------------
//
// The letter a title files under: 1..26 for A-Z whatever its case, 0 for everything else. The
// cabinet's jump-to-letter relies on titles that share a letter being adjacent, which a plain
// case-insensitive compare does not give: '{' and '~' sort after 'z' there, away from the rest of
// the non-letters. OrderKey puts every letter after every other character, so "#" comes first,
// then A to Z, and each letter's titles are one run.
//
// The cabinet carries the same two functions (phantom_cat.c) for the older catalog path; keep
// them in step.
inline int LetterOf(const char* t) {
    const char c = t ? t[0] : 0;
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
    return 0;
}

inline int OrderKey(unsigned char c) {
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    return (c >= 'a' && c <= 'z') ? 256 + c : c;
}

inline int TitleCmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ka = OrderKey((unsigned char)*a), kb = OrderKey((unsigned char)*b);
        if (ka != kb) return ka < kb ? -1 : 1;
        if (!*a) return 0;
    }
}

// ---- records ----------------------------------------------------------------------------------

enum : uint32_t {
    kRecPending  = 1,   // found on disk, not yet vetted (MAME is still being asked about it)
    kRecRejected = 2,   // vetted and not listable (an incomplete or non-runnable set)
};

struct Rec {
    uint32_t id, title, rom, mode, year, maker;   // offsets into Index::pool; 0 is ""
    uint32_t sys;                                 // index into Index::sys
    uint32_t flags;
    uint64_t size, mtime;                         // of the main file, so a rescan can tell it is unchanged
};
static_assert(sizeof(Rec) == 48, "Rec is written to the cache file as it is");

struct SysInfo {
    std::string key, name;
    std::string root;   // where this system's games were looked for; a changed setting invalidates its cached games
};

constexpr int kLetterSlots = 28;   // '#', A..Z, then the total

class Index {
public:
    uint32_t ver = 0;
    std::vector<SysInfo> sys;
    std::string pool = std::string(1, '\0');
    std::vector<Rec> recs;
    std::vector<std::vector<uint32_t>> lists;           // [0] = every game, [1 + s] = system s; record numbers in title order
    std::vector<std::array<uint32_t, kLetterSlots>> letters;

    const char* Str(uint32_t off) const { return pool.data() + off; }
    const char* Id(const Rec& r) const { return Str(r.id); }
    const char* Title(const Rec& r) const { return Str(r.title); }
    const char* Rom(const Rec& r) const { return Str(r.rom); }

    size_t Total() const { return lists.empty() ? 0 : lists[0].size(); }

    // "*" is every game; otherwise a system key. -1 when there is no such list.
    int ListFor(std::string_view key) const {
        if (key == "*") return 0;
        for (size_t i = 0; i < sys.size(); i++)
            if (key == sys[i].key) return (int)i + 1;
        return -1;
    }

    const Rec* RecAt(int list, size_t i) const {
        if (list < 0 || (size_t)list >= lists.size() || i >= lists[list].size()) return nullptr;
        return &recs[lists[list][i]];
    }

    // The record behind a catalog id ("<system>_<set>"), for a launch that quotes one.
    const Rec* Find(const char* id) const {
        if (hash_.empty()) return nullptr;
        const size_t mask = hash_.size() - 1;
        for (size_t h = Hash(id) & mask;; h = (h + 1) & mask) {
            const uint32_t e = hash_[h];
            if (!e) return nullptr;
            if (!strcmp(Id(recs[e - 1]), id)) return &recs[e - 1];
        }
    }

    // Everything derived from recs and pool: the sorted lists, the letter tables, the id lookup.
    // Called once when an index is finished or loaded; the index is read-only after that.
    void Build() {
        const size_t n = recs.size();
        std::vector<uint32_t> all(n);
        for (size_t i = 0; i < n; i++) all[i] = (uint32_t)i;
        std::sort(all.begin(), all.end(), [&](uint32_t a, uint32_t b) {
            const int c = TitleCmp(Title(recs[a]), Title(recs[b]));
            if (c) return c < 0;
            const int d = strcmp(Id(recs[a]), Id(recs[b]));
            return d ? d < 0 : a < b;
        });

        lists.assign(sys.size() + 1, {});
        lists[0] = std::move(all);
        for (uint32_t r : lists[0])
            if (recs[r].sys < sys.size()) lists[1 + recs[r].sys].push_back(r);

        letters.assign(lists.size(), {});
        for (size_t l = 0; l < lists.size(); l++) {
            std::array<uint32_t, kLetterSlots>& s = letters[l];
            const uint32_t none = 0xFFFFFFFFu;
            s.fill(none);
            const std::vector<uint32_t>& v = lists[l];
            for (uint32_t i = 0; i < v.size(); i++) {
                const int k = LetterOf(Title(recs[v[i]]));
                if (s[k] == none) s[k] = i;
            }
            s[kLetterSlots - 1] = (uint32_t)v.size();
            for (int k = kLetterSlots - 2; k >= 0; k--)
                if (s[k] == none) s[k] = s[k + 1];
        }

        size_t cap = 16;
        while (cap < n * 2 + 2) cap <<= 1;
        hash_.assign(cap, 0);
        for (size_t i = 0; i < n; i++) {
            size_t h = Hash(Id(recs[i])) & (cap - 1);
            while (hash_[h]) {
                if (!strcmp(Id(recs[hash_[h] - 1]), Id(recs[i]))) { h = (size_t)-1; break; }   // first one wins
                h = (h + 1) & (cap - 1);
            }
            if (h != (size_t)-1) hash_[h] = (uint32_t)i + 1;
        }
    }

private:
    static size_t Hash(const char* s) {
        uint64_t h = 1469598103934665603ull;
        for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
        return (size_t)(h ^ (h >> 29));
    }
    std::vector<uint32_t> hash_;
};

// ---- building an index --------------------------------------------------------------------------

// Strings that go on the wire may not contain the protocol's delimiters, and a control character
// is never part of a title.
inline std::string Clean(std::string_view in) {
    std::string out(in);
    for (char& c : out)
        if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
    return out;
}

class IndexBuilder {
public:
    int AddSystem(const std::string& key, const std::string& name, const std::string& root) {
        SysInfo s;
        s.key = key;
        s.name = Clean(name);
        for (char& c : s.name) if (c == '|' || c == ';') c = ' ';
        s.root = root;
        sys_.push_back(std::move(s));
        return (int)sys_.size() - 1;
    }

    // Strings that repeat across thousands of games (a video mode, a year, a maker) are stored once.
    uint32_t Intern(std::string_view s) {
        if (s.empty()) return 0;
        // A copy first: the caller may be handing in a view of this builder's own pool, which Put can move.
        const std::string key = Clean(s);
        auto it = intern_.find(key);
        if (it != intern_.end()) return it->second;
        const uint32_t off = Put(key);
        intern_.emplace(key, off);
        return off;
    }

    uint32_t Put(std::string_view s) {
        if (s.empty()) return 0;
        const uint32_t off = (uint32_t)pool_.size();
        pool_.append(s.data(), s.size());
        pool_.push_back('\0');
        return off;
    }

    // Returns the record number, so a later pass can fill in what it learns (MAME's verdict).
    uint32_t Add(int sys, std::string_view id, std::string_view title, std::string_view rom,
                 std::string_view mode, std::string_view year, std::string_view maker,
                 uint64_t size, uint64_t mtime, uint32_t flags = 0) {
        Rec r;
        r.id = Put(Clean(id));
        r.title = Put(Clean(title));
        r.rom = Put(rom);
        r.mode = Intern(mode);
        r.year = Intern(year);
        r.maker = Intern(maker);
        r.sys = (uint32_t)sys;
        r.flags = flags;
        r.size = size;
        r.mtime = mtime;
        recs_.push_back(r);
        return (uint32_t)recs_.size() - 1;
    }

    // Keep a record from an earlier index as it was: its strings are copied across, and nothing about
    // the file is looked at again. This is what makes a rescan of an unchanged library cheap.
    uint32_t AddFrom(const Index& old, const Rec& o, int sys, uint32_t flags = 0) {
        return Add(sys, old.Id(o), old.Title(o), old.Rom(o), old.Str(o.mode), old.Str(o.year), old.Str(o.maker),
                   o.size, o.mtime, flags);
    }

    size_t Size() const { return recs_.size(); }
    Rec& At(uint32_t i) { return recs_[i]; }
    const Rec& At(uint32_t i) const { return recs_[i]; }
    const char* Str(uint32_t off) const { return pool_.data() + off; }
    void SetFlags(uint32_t i, uint32_t f) { recs_[i].flags = f; }
    void SetTitle(uint32_t i, std::string_view t) { recs_[i].title = Put(Clean(t)); }
    void SetMeta(uint32_t i, std::string_view mode, std::string_view year, std::string_view maker) {
        recs_[i].mode = Intern(mode);
        recs_[i].year = Intern(year);
        recs_[i].maker = Intern(maker);
    }

    // The listable records as a finished, sorted index. Does not disturb the builder, so a long scan
    // can publish what it has so far and carry on.
    std::shared_ptr<Index> Finish(uint32_t ver) const {
        auto ix = std::make_shared<Index>();
        ix->ver = ver;
        ix->sys = sys_;

        size_t listed = 0;
        for (const Rec& r : recs_) if (!(r.flags & (kRecPending | kRecRejected))) listed++;
        ix->recs.reserve(listed);
        ix->pool.reserve(pool_.size() / 2 + 16);

        std::unordered_map<uint32_t, uint32_t> shared;   // old offset -> new, for the interned strings
        auto copy = [&](uint32_t off) -> uint32_t {
            if (!off) return 0;
            const uint32_t at = (uint32_t)ix->pool.size();
            ix->pool.append(pool_.data() + off);
            ix->pool.push_back('\0');
            return at;
        };
        auto copy_shared = [&](uint32_t off) -> uint32_t {
            if (!off) return 0;
            auto it = shared.find(off);
            if (it != shared.end()) return it->second;
            const uint32_t at = copy(off);
            shared.emplace(off, at);
            return at;
        };

        for (const Rec& r : recs_) {
            if (r.flags & (kRecPending | kRecRejected)) continue;
            Rec n = r;
            n.id = copy(r.id);
            n.title = copy(r.title);
            n.rom = copy(r.rom);
            n.mode = copy_shared(r.mode);
            n.year = copy_shared(r.year);
            n.maker = copy_shared(r.maker);
            n.flags = 0;
            ix->recs.push_back(n);
        }
        ix->Build();
        return ix;
    }

private:
    std::vector<SysInfo> sys_;
    std::string pool_ = std::string(1, '\0');
    std::vector<Rec> recs_;
    std::unordered_map<std::string, uint32_t> intern_;
};

// True when two indexes list the same games, in the same order, with the same details. A rescan that
// finds nothing new is then not published: publishing bumps the version, and a cabinet that is browsing
// would throw away every page it had fetched for a library that had not changed.
inline bool IndexEquivalent(const Index& a, const Index& b) {
    if (a.sys.size() != b.sys.size() || a.recs.size() != b.recs.size()) return false;
    for (size_t i = 0; i < a.sys.size(); i++)
        if (a.sys[i].key != b.sys[i].key || a.sys[i].name != b.sys[i].name || a.sys[i].root != b.sys[i].root) return false;
    for (size_t i = 0; i < a.lists[0].size(); i++) {
        const Rec& x = a.recs[a.lists[0][i]];
        const Rec& y = b.recs[b.lists[0][i]];
        // size and modification time count: they are how the next scan decides a file is unchanged, so an index
        // that kept stale ones would make every later scan think the file had just changed
        if (x.sys != y.sys || x.size != y.size || x.mtime != y.mtime ||
            strcmp(a.Id(x), b.Id(y)) || strcmp(a.Title(x), b.Title(y)) || strcmp(a.Rom(x), b.Rom(y)) ||
            strcmp(a.Str(x.mode), b.Str(y.mode)) || strcmp(a.Str(x.year), b.Str(y.year)) || strcmp(a.Str(x.maker), b.Str(y.maker)))
            return false;
    }
    return true;
}

// ---- the cache file ----------------------------------------------------------------------------
//
// Records, pool and the system table, nothing derived: the lists are rebuilt on load, which
// costs a fraction of a second for 300,000 games and keeps the format from depending on how they
// are sorted. Written to a temporary and renamed, so a crash mid-save leaves the old file.

namespace detail {
inline void PutU32(std::string& o, uint32_t v) { o.append((const char*)&v, 4); }
inline void PutStr(std::string& o, const std::string& s) { PutU32(o, (uint32_t)s.size()); o += s; }
}

inline bool SaveIndex(const Index& ix, const std::filesystem::path& file) {
    std::string head;
    head.append("PAIX", 4);
    detail::PutU32(head, 1);
    detail::PutU32(head, (uint32_t)ix.sys.size());
    detail::PutU32(head, (uint32_t)ix.recs.size());
    const uint64_t pool = ix.pool.size();
    head.append((const char*)&pool, 8);
    for (const SysInfo& s : ix.sys) {
        detail::PutStr(head, s.key);
        detail::PutStr(head, s.name);
        detail::PutStr(head, s.root);
    }

    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(head.data(), (std::streamsize)head.size());
        f.write((const char*)ix.recs.data(), (std::streamsize)(ix.recs.size() * sizeof(Rec)));
        f.write(ix.pool.data(), (std::streamsize)ix.pool.size());
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);   // replaces an existing file on Windows and POSIX alike
    return !ec;
}

// nullptr when the file is missing, from another format, or cut short.
inline std::shared_ptr<Index> LoadIndex(const std::filesystem::path& file, uint32_t ver) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) return nullptr;
    const std::streamoff total = f.tellg();
    if (total < 24) return nullptr;
    f.seekg(0);

    char magic[4];
    uint32_t format = 0, nsys = 0, nrecs = 0;
    uint64_t pool = 0;
    f.read(magic, 4);
    f.read((char*)&format, 4);
    f.read((char*)&nsys, 4);
    f.read((char*)&nrecs, 4);
    f.read((char*)&pool, 8);
    if (!f || memcmp(magic, "PAIX", 4) != 0 || format != 1 || nsys > 64) return nullptr;

    auto ix = std::make_shared<Index>();
    ix->ver = ver;
    auto getstr = [&](std::string& s) {
        uint32_t len = 0;
        f.read((char*)&len, 4);
        if (!f || len > (1u << 20)) return false;
        s.resize(len);
        f.read(&s[0], len);
        return (bool)f;
    };
    for (uint32_t i = 0; i < nsys; i++) {
        SysInfo s;
        if (!getstr(s.key) || !getstr(s.name) || !getstr(s.root)) return nullptr;
        ix->sys.push_back(std::move(s));
    }

    const std::streamoff here = f.tellg();
    if (here < 0 || (uint64_t)(total - here) != (uint64_t)nrecs * sizeof(Rec) + pool || pool < 1) return nullptr;

    ix->recs.resize(nrecs);
    f.read((char*)ix->recs.data(), (std::streamsize)((uint64_t)nrecs * sizeof(Rec)));
    ix->pool.resize((size_t)pool);
    f.read(&ix->pool[0], (std::streamsize)pool);
    if (!f) return nullptr;

    // A cache is trusted for its contents, not its offsets: refuse one whose offsets point outside the pool.
    for (const Rec& r : ix->recs) {
        const uint32_t offs[6] = { r.id, r.title, r.rom, r.mode, r.year, r.maker };
        for (uint32_t o : offs)
            if (o >= pool) return nullptr;
        if (r.sys >= nsys) return nullptr;
    }
    if (ix->pool.back() != '\0') return nullptr;
    ix->Build();
    return ix;
}

// ---- answering the cabinet ------------------------------------------------------------------------

inline std::string CatInfoReply(const Index& ix, bool scanning) {
    std::string out = "CATINFO:" + std::to_string(ix.ver) + ":" + std::to_string(ix.Total()) + ":" +
                      (scanning ? "1" : "0") + ":";
    for (size_t s = 0; s < ix.sys.size(); s++) {
        if (s) out += ";";
        out += ix.sys[s].key + "|" + ix.sys[s].name + "|" + std::to_string(ix.lists[1 + s].size());
    }
    return out;
}

inline std::string LettersReply(const Index& ix, const std::string& list) {
    const int l = ix.ListFor(list);
    std::string out = "LETTERS:" + std::to_string(ix.ver) + ":" + list + ":";
    for (int k = 0; k < kLetterSlots; k++) {
        if (k) out += ",";
        out += std::to_string(l < 0 ? 0u : ix.letters[l][k]);
    }
    return out;
}

// As many of the requested rows as fit in maxBytes. A request past the end gets zero rows, not an error.
inline std::string ListReply(const Index& ix, const std::string& list, size_t offset, size_t count, size_t maxBytes) {
    const int l = ix.ListFor(list);
    const size_t total = l < 0 ? 0 : ix.lists[l].size();

    std::string rows;
    size_t n = 0;
    for (; n < count && offset + n < total; n++) {
        const Rec& r = ix.recs[ix.lists[l][offset + n]];
        std::string row;
        row.reserve(128);
        row += ix.Id(r);
        row += '\t';
        row += ix.Title(r);
        row += '\t';
        row += ix.sys[r.sys].key;
        row += '\t';
        row += ix.Str(r.mode);
        row += '\t';
        row += ix.Str(r.year);
        row += '\t';
        row += ix.Str(r.maker);
        row += '\n';
        if (n > 0 && rows.size() + row.size() > maxBytes) break;
        rows += row;
    }
    return "LIST:" + std::to_string(ix.ver) + ":" + list + ":" + std::to_string(offset) + ":" + std::to_string(n) +
           ":" + std::to_string(total) + "\n" + rows;
}

namespace detail {
// Split "a:b:c" at the first `max-1` colons; the last field keeps the rest.
inline std::vector<std::string> Split(const std::string& s, size_t from, int max) {
    std::vector<std::string> out;
    size_t at = from;
    while ((int)out.size() < max - 1) {
        const size_t c = s.find(':', at);
        if (c == std::string::npos) break;
        out.push_back(s.substr(at, c - at));
        at = c + 1;
    }
    out.push_back(s.substr(at));
    return out;
}
inline bool ToU32(const std::string& s, uint32_t& v) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t x = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        x = x * 10 + (uint64_t)(c - '0');
    }
    if (x > 0xFFFFFFFFull) return false;
    v = (uint32_t)x;
    return true;
}
}

// CATINFO, LETTERS and LIST. Returns true when `msg` was one of them and `reply` holds the answer.
// Everything else (LAUNCH, KILL, STATUS...) is the daemon's own business.
inline bool AnswerCatalogRequest(const std::shared_ptr<const Index>& ix, bool scanning, const std::string& msg,
                                 std::string& reply, size_t maxBytes = 7000) {
    if (msg == "CATINFO") {
        reply = ix ? CatInfoReply(*ix, scanning) : "CATINFO:0:0:" + std::string(scanning ? "1" : "0") + ":";
        return true;
    }

    const bool isLetters = msg.rfind("LETTERS:", 0) == 0;
    const bool isList = msg.rfind("LIST:", 0) == 0;
    if (!isLetters && !isList) return false;

    const std::vector<std::string> f = detail::Split(msg, isLetters ? 8 : 5, isLetters ? 2 : 4);
    uint32_t ver = 0, offset = 0, count = 0;
    if (f.size() != (isLetters ? 2u : 4u) || !detail::ToU32(f[0], ver) ||
        (isList && (!detail::ToU32(f[2], offset) || !detail::ToU32(f[3], count)))) {
        reply = "ERR:BAD_REQUEST";
        return true;
    }
    if (!ix || ix->ver != ver) {
        reply = "STALE:" + std::to_string(ix ? ix->ver : 0);
        return true;
    }
    reply = isLetters ? LettersReply(*ix, f[1]) : ListReply(*ix, f[1], offset, std::min<uint32_t>(count, 128), maxBytes);
    return true;
}

// ---- the older cabinet path ---------------------------------------------------------------------
//
// A cabinet that predates the paged protocol sends GET_CATALOG and reads a JSON document out of one
// datagram, 60,000 bytes at most and 500 games at most. Rather than send it the front of a long
// list, which hides every emulator after the first, it gets the small systems first and the big arcade
// ones after, each in title order, stopping at a whole record.
inline std::string CompactCatalogJson(const Index& ix, size_t maxBytes = 59000, size_t maxGames = 500) {
    auto esc = [](const char* s) {
        std::string o;
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') o += '\\';
            o += *s;
        }
        return o;
    };

    std::string out = "{\"games\":[";
    size_t written = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t s = 0; s < ix.sys.size(); s++) {
            const int rank = ix.sys[s].key == "fbneo" ? 2 : ix.sys[s].key == "groovymame" ? 1 : 0;
            if (rank != pass) continue;
            for (uint32_t ri : ix.lists[1 + s]) {
                const Rec& r = ix.recs[ri];
                std::string rec = std::string("{\"id\":\"") + esc(ix.Id(r)) + "\",\"title\":\"" + esc(ix.Title(r)) +
                                  "\",\"system\":\"" + esc(ix.sys[s].key.c_str()) + "\",\"systemName\":\"" +
                                  esc(ix.sys[s].name.c_str()) + "\",\"videoMode\":\"" + esc(ix.Str(r.mode)) +
                                  "\",\"year\":\"" + esc(ix.Str(r.year)) + "\",\"manufacturer\":\"" +
                                  esc(ix.Str(r.maker)) + "\"}";
                const size_t add = rec.size() + (written ? 1 : 0);
                if (written >= maxGames || out.size() + add + 3 > maxBytes) return out + "]}";
                if (written) out += ",";
                out += rec;
                written++;
            }
        }
    }
    return out + "]}";
}

}  // namespace phantom

#endif
