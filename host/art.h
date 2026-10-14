/*
 * art.h - which picture belongs to which game, and turning it into what the cabinet draws.
 *
 * The pictures come from the libretro thumbnail server (https://thumbnails.libretro.com/), which is a plain
 * directory tree: one folder per system, and in it Named_Titles/ and Named_Snaps/ holding one PNG per game, named
 * after the game exactly as a No-Intro, Redump or MAME/FBNeo listing spells it, with the characters a filename
 * cannot hold (& * / : ` < > ? \ | ") replaced by an underscore. There is no search; the folder listing is the
 * index. So the job here is to take a game the library knows by some title and some file name and find the one
 * entry in that listing that is the same game, which is mostly string work:
 *
 *   FileSafe     the spelling the server uses for a title
 *   ParseListing the names in a folder listing page
 *   Matcher      the ladder from "exactly that" down to "same game, another region or revision"
 *   Repos        which folders to look in for a catalog system / ROM file
 *   FitPicture   where a picture of some shape goes on the cabinet's 4:3 canvas
 *   ComposeBGR   resample it onto that canvas, in the byte order the cabinet draws
 *
 * Portable C++17, no Windows headers, no network: the HTTP client and the PNG decoder are injected by the code that
 * uses this (art_store.h), so every rule here is testable on its own (art_test.cpp).
 */
#ifndef PHANTOM_ART_H
#define PHANTOM_ART_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phantom {
namespace art {

enum Kind { kTitle = 0, kSnap = 1, kKinds = 2 };

inline const char* KindDir(Kind k) { return k == kTitle ? "Named_Titles" : "Named_Snaps"; }
inline const char* KindName(Kind k) { return k == kTitle ? "title" : "snap"; }

// ---- names -------------------------------------------------------------------------------------------

// The server's spelling of a title as a file name.
inline std::string FileSafe(std::string_view t) {
    std::string out(t);
    for (char& c : out)
        if (std::strchr("&*/:`<>?\\|\"", c) && c) c = '_';
    return out;
}

inline bool IsAlnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// A title reduced to the part that survives spelling differences: lower case letters and digits only, with the
// words that different lists abbreviate or place differently folded together ("Revision" / "Rev", "Version" /
// "Ver", "and" / "&", and the article, which one list puts first and another last: "The House of the Dead" and
// "House of the Dead, The").
inline std::string Norm(std::string_view t) {
    std::string out, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (word == "revision") word = "rev";
        else if (word == "version") word = "ver";
        if (word != "the" && word != "and") out += word;
        word.clear();
    };
    for (char c : t) {
        if (IsAlnum((unsigned char)c)) word += Lower(c);
        else flush();
    }
    flush();
    return out;
}

// The title with every parenthesised or bracketed group taken out: "Super Mario World (USA) (Rev 1)" is
// "Super Mario World". What is left is the game; the groups say which release of it.
inline std::string BaseOf(std::string_view t) {
    std::string out;
    int depth = 0;
    for (char c : t) {
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        else if (!depth) {
            if (c == ' ' && (out.empty() || out.back() == ' ')) continue;   // one space between what is left of two groups
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The words inside the groups, lower case: {"usa", "rev", "1"}.
inline std::vector<std::string> GroupWords(std::string_view t) {
    std::vector<std::string> out;
    std::string word;
    int depth = 0;
    auto flush = [&]() {
        if (word.empty()) return;
        if (word == "revision") word = "rev";
        else if (word == "version") word = "ver";
        out.push_back(word);
        word.clear();
    };
    for (char c : t) {
        if (c == '(' || c == '[') { flush(); depth++; }
        else if (c == ')' || c == ']') { flush(); if (depth) depth--; }
        else if (depth && IsAlnum((unsigned char)c)) word += Lower(c);
        else flush();
    }
    flush();
    return out;
}

// ---- a folder listing -----------------------------------------------------------------------------------

inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string PercentDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && HexVal(s[i + 1]) >= 0 && HexVal(s[i + 2]) >= 0) {
            out += (char)(HexVal(s[i + 1]) * 16 + HexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

inline std::string HtmlUnescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            auto at = [&](const char* e) { return s.compare(i, std::strlen(e), e) == 0; };
            if (at("&amp;")) { out += '&'; i += 4; continue; }
            if (at("&lt;")) { out += '<'; i += 3; continue; }
            if (at("&gt;")) { out += '>'; i += 3; continue; }
            if (at("&quot;")) { out += '"'; i += 5; continue; }
            if (at("&#39;")) { out += '\''; i += 4; continue; }
        }
        out += s[i];
    }
    return out;
}

// The picture names in a directory listing page, without the ".png". An Apache index writes each as
// <a href="Name%20Here.png">; links that go elsewhere (sorting headers, the parent folder) are skipped.
inline std::vector<std::string> ParseListing(std::string_view html) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        p = html.find("<a href=\"", p);
        if (p == std::string_view::npos) break;
        p += 9;
        const size_t e = html.find('"', p);
        if (e == std::string_view::npos) break;
        std::string_view href = html.substr(p, e - p);
        p = e;
        if (href.empty() || href[0] == '?' || href[0] == '/' || href.find('?') != std::string_view::npos) continue;
        std::string name = PercentDecode(HtmlUnescape(href));
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
            out.push_back(name.substr(0, name.size() - 4));
    }
    return out;
}

// ---- finding a game in a listing --------------------------------------------------------------------------

class Matcher {
public:
    void Clear() { *this = Matcher(); }

    void Add(std::string stem) { stems_.push_back(std::move(stem)); }

    void Build() {
        exact_.clear();
        byNorm_.clear();
        byBase_.clear();
        for (uint32_t i = 0; i < stems_.size(); i++) {
            exact_.insert(stems_[i]);
            byNorm_.emplace(Norm(stems_[i]), i);
            byBase_[Norm(BaseOf(stems_[i]))].push_back(i);
        }
    }

    size_t Size() const { return stems_.size(); }

    // The listing's spelling of this game's picture, or "" when there is none. `title` is what the library shows;
    // `file` is the ROM's file name without its extension, which for a disc is the Redump / No-Intro name and is
    // often the better key. Either may be empty.
    std::string Find(std::string_view title, std::string_view file) const {
        const std::string_view names[2] = { title, file };

        // 1. the very same name
        for (std::string_view n : names) {
            if (n.empty()) continue;
            const std::string s = FileSafe(n);
            if (exact_.count(s)) return s;
        }
        // 2. the same name spelled a little differently (case, punctuation, "Rev" / "Revision", a moved "The")
        for (std::string_view n : names) {
            if (n.empty()) continue;
            auto it = byNorm_.find(Norm(n));
            if (it != byNorm_.end()) return stems_[it->second];
        }
        // 3. the same game, another release: the best of the entries that agree once the groups are taken off
        for (std::string_view n : names) {
            if (n.empty()) continue;
            const std::string base = Norm(BaseOf(n));
            if (base.size() < 2) continue;
            auto it = byBase_.find(base);
            if (it == byBase_.end()) continue;
            return stems_[Best(it->second, GroupWords(n))];
        }
        return std::string();
    }

private:
    // Which of several releases of the same game to show. The one whose groups say the same things as the
    // game's own wins; failing that, the release a player is most likely to know.
    uint32_t Best(const std::vector<uint32_t>& cands, const std::vector<std::string>& want) const {
        uint32_t best = cands[0];
        long bestScore = -1000000;
        for (uint32_t c : cands) {
            const std::vector<std::string> have = GroupWords(stems_[c]);
            long same = 0;
            for (const std::string& w : have)
                if (std::find(want.begin(), want.end(), w) != want.end()) same++;
            long score = same * 100 - (long)(have.size() - same) * 10 - (long)(want.size() - same);
            score = score * 10 - Region(have);
            if (score > bestScore || (score == bestScore && stems_[c] < stems_[best])) {
                best = c;
                bestScore = score;
            }
        }
        return best;
    }

    static int Region(const std::vector<std::string>& words) {
        static const char* order[] = { "world", "usa", "us", "europe", "euro", "japan", "asia" };
        for (int r = 0; r < 7; r++)
            for (const std::string& w : words)
                if (w == order[r]) return r;
        return 8;
    }

    std::vector<std::string> stems_;
    std::unordered_set<std::string> exact_;
    std::unordered_map<std::string, uint32_t> byNorm_;
    std::unordered_map<std::string, std::vector<uint32_t>> byBase_;
};

// ---- which folders -------------------------------------------------------------------------------------------

inline std::string LowerStr(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = Lower(c);
    return o;
}

inline std::string ExtOf(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return std::string();
    return LowerStr(path.substr(dot));
}

// The file's name without folder or extension.
inline std::string StemOf(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    std::string_view f = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = f.rfind('.');
    if (dot != std::string_view::npos && dot > 0) f = f.substr(0, dot);
    return std::string(f);
}

// The server's folders to look in, best first, for a game of catalog system `sys` (the emulator's key) whose main
// file is `rom`. Empty when the server has nothing for that kind of game.
inline std::vector<std::string> Repos(std::string_view sys, std::string_view rom) {
    const std::string ext = ExtOf(rom);
    if (sys == "groovymame") return { "MAME" };
    if (sys == "fbneo") return { "FBNeo - Arcade Games" };
    if (sys == "pcsx2") return { "Sony - PlayStation 2" };
    if (sys == "rpcs3") return { "Sony - PlayStation 3" };
    if (sys == "xemu") return { "Microsoft - Xbox" };
    if (sys == "flycast") {
        // A zip is an arcade board's set; a disc image is a Dreamcast game.
        if (ext == ".zip" || ext == ".7z") return { "Sega - Naomi", "Sega - Atomiswave", "Sega - Naomi 2" };
        return { "Sega - Dreamcast" };
    }
    if (sys == "dolphin") {
        if (ext == ".wbfs" || ext == ".wia" || ext == ".rvz" || ext == ".wad") return { "Nintendo - Wii", "Nintendo - GameCube" };
        return { "Nintendo - GameCube", "Nintendo - Wii" };
    }
    if (sys == "retroarch") {
        struct E { const char* ext; const char* repo; };
        static const E byExt[] = {
            { ".sfc", "Nintendo - Super Nintendo Entertainment System" }, { ".smc", "Nintendo - Super Nintendo Entertainment System" },
            { ".nes", "Nintendo - Nintendo Entertainment System" },       { ".fds", "Nintendo - Family Computer Disk System" },
            { ".gb", "Nintendo - Game Boy" },                              { ".gbc", "Nintendo - Game Boy Color" },
            { ".gba", "Nintendo - Game Boy Advance" },                     { ".nds", "Nintendo - Nintendo DS" },
            { ".n64", "Nintendo - Nintendo 64" },                          { ".z64", "Nintendo - Nintendo 64" },
            { ".v64", "Nintendo - Nintendo 64" },
            { ".md", "Sega - Mega Drive - Genesis" },                      { ".gen", "Sega - Mega Drive - Genesis" },
            { ".smd", "Sega - Mega Drive - Genesis" },                     { ".sms", "Sega - Master System - Mark III" },
            { ".gg", "Sega - Game Gear" },                                 { ".32x", "Sega - 32X" },
            { ".pce", "NEC - PC Engine - TurboGrafx 16" },                 { ".a26", "Atari - 2600" },
            { ".a78", "Atari - 7800" },                                    { ".lnx", "Atari - Lynx" },
            { ".ngp", "SNK - Neo Geo Pocket" },                            { ".ngc", "SNK - Neo Geo Pocket Color" },
            { ".ws", "Bandai - WonderSwan" },                              { ".wsc", "Bandai - WonderSwan Color" },
        };
        for (const E& e : byExt)
            if (ext == e.ext) return { e.repo };
        // zip / 7z / chd / cue / bin / iso carry no hint of their own; a folder named for the system is the
        // convention, so read it from the path.
        struct F { const char* word; const char* repo; };
        static const F byFolder[] = {
            { "super nintendo", "Nintendo - Super Nintendo Entertainment System" }, { "snes", "Nintendo - Super Nintendo Entertainment System" },
            { "famicom", "Nintendo - Nintendo Entertainment System" },              { "nes", "Nintendo - Nintendo Entertainment System" },
            { "game boy advance", "Nintendo - Game Boy Advance" },                  { "gba", "Nintendo - Game Boy Advance" },
            { "game boy color", "Nintendo - Game Boy Color" },                      { "gbc", "Nintendo - Game Boy Color" },
            { "game boy", "Nintendo - Game Boy" },                                  { "nintendo 64", "Nintendo - Nintendo 64" },
            { "n64", "Nintendo - Nintendo 64" },                                    { "genesis", "Sega - Mega Drive - Genesis" },
            { "mega drive", "Sega - Mega Drive - Genesis" },                        { "megadrive", "Sega - Mega Drive - Genesis" },
            { "mega cd", "Sega - Mega-CD - Sega CD" },                              { "sega cd", "Sega - Mega-CD - Sega CD" },
            { "master system", "Sega - Master System - Mark III" },                 { "game gear", "Sega - Game Gear" },
            { "saturn", "Sega - Saturn" },                                          { "dreamcast", "Sega - Dreamcast" },
            { "playstation 2", "Sony - PlayStation 2" },                            { "ps2", "Sony - PlayStation 2" },
            { "psx", "Sony - PlayStation" },                                        { "playstation", "Sony - PlayStation" },
            { "ps1", "Sony - PlayStation" },                                        { "psp", "Sony - PlayStation Portable" },
            { "pc engine cd", "NEC - PC Engine CD - TurboGrafx-CD" },               { "pc engine", "NEC - PC Engine - TurboGrafx 16" },
            { "turbografx", "NEC - PC Engine - TurboGrafx 16" },                    { "neo geo", "SNK - Neo Geo" },
            { "3do", "The 3DO Company - 3DO" },                                     { "atari 2600", "Atari - 2600" },
        };
        const std::string low = LowerStr(rom);
        // the folder part only, so a game called "Saturn Bomberman" does not file itself under Saturn
        const size_t slash = low.find_last_of("/\\");
        const std::string dirs = slash == std::string::npos ? std::string() : low.substr(0, slash);
        for (const F& f : byFolder)
            if (dirs.find(f.word) != std::string::npos) return { f.repo };
        return {};
    }
    return {};   // 86Box machines and anything else: no pictures
}

// ---- URLs -----------------------------------------------------------------------------------------------------------

inline std::string UrlEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (IsAlnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

inline const char* ArtBase() { return "https://thumbnails.libretro.com/"; }

inline std::string ListingUrl(std::string_view repo, Kind k) {
    return std::string(ArtBase()) + UrlEncode(repo) + "/" + KindDir(k) + "/";
}

inline std::string PictureUrl(std::string_view repo, Kind k, std::string_view stem) {
    return ListingUrl(repo, k) + UrlEncode(stem) + ".png";
}

// ---- the picture on the canvas ------------------------------------------------------------------------------

constexpr int kCanvasW = 720;
constexpr int kCanvasH = 480;

struct Rect { int x, y, w, h; };

// Where a picture of sw x sh goes on the cabinet's canvas. The canvas is 720x480 pixels shown on a 4:3 tube, so
// its pixels are not square, and the pictures a game's emulator saves are the raw frame - 256x224, 320x240,
// 384x224 - which that same tube would have stretched to 4:3. So a picture up to 7:4 wide fills the canvas, as it
// did on the real machine (384x224 is the widest arcade frame, 12:7). Wider than that (a 16:9 PS3 title) is
// letter-boxed at its own shape, and a tall one (a
// vertical arcade board) is pillar-boxed at its own shape; both are centred. Sizes and offsets are even, so a
// picture edge never lands on one field of the interlaced scan only.
inline Rect FitPicture(int sw, int sh) {
    if (sw <= 0 || sh <= 0) return { 0, 0, kCanvasW, kCanvasH };
    const double a = (double)sw / (double)sh;
    int w = kCanvasW, h = kCanvasH;
    if (a > 1.75) h = (int)std::lround(640.0 / a);
    else if (a < 1.0) w = (int)std::lround(540.0 * a);
    w = (std::max)(2, (std::min)(kCanvasW, w & ~1));
    h = (std::max)(2, (std::min)(kCanvasH, h & ~1));
    return { ((kCanvasW - w) / 2) & ~1, ((kCanvasH - h) / 2) & ~1, w, h };
}

// Per destination pixel, the source pixels that make it and how much each counts. Shrinking averages the span a
// destination pixel covers; enlarging blends the two nearest, so a 256-wide picture on 720 columns is smooth
// rather than blocky.
struct Taps {
    std::vector<int> first, count;
    std::vector<float> weight;   // count[i] weights starting at offset[i]
    std::vector<int> offset;
};

inline Taps MakeTaps(int srcN, int dstN) {
    Taps t;
    t.first.resize(dstN);
    t.count.resize(dstN);
    t.offset.resize(dstN);
    const double scale = (double)srcN / (double)dstN;
    for (int d = 0; d < dstN; d++) {
        t.offset[d] = (int)t.weight.size();
        if (scale > 1.0) {
            const double lo = d * scale, hi = (d + 1) * scale;
            const int s0 = (int)std::floor(lo), s1 = (std::min)(srcN - 1, (int)std::ceil(hi) - 1);
            t.first[d] = s0;
            t.count[d] = s1 - s0 + 1;
            double sum = 0;
            for (int s = s0; s <= s1; s++) {
                const double w = (std::min)(hi, (double)(s + 1)) - (std::max)(lo, (double)s);
                t.weight.push_back((float)w);
                sum += w;
            }
            for (int i = 0; i < t.count[d]; i++) t.weight[t.offset[d] + i] = (float)(t.weight[t.offset[d] + i] / sum);
        } else {
            const double c = (d + 0.5) * scale - 0.5;
            int s0 = (int)std::floor(c);
            const double f = c - s0;
            int s1 = s0 + 1;
            s0 = (std::max)(0, (std::min)(srcN - 1, s0));
            s1 = (std::max)(0, (std::min)(srcN - 1, s1));
            if (s0 == s1) { t.first[d] = s0; t.count[d] = 1; t.weight.push_back(1.0f); }
            else { t.first[d] = s0; t.count[d] = 2; t.weight.push_back((float)(1.0 - f)); t.weight.push_back((float)f); }
        }
    }
    return t;
}

// Resample a picture (B,G,R,A bytes, as the Windows imaging code hands it over) onto the 720x480 canvas as
// B,G,R, black behind and around it. `out` must hold kCanvasW * kCanvasH * 3 bytes.
inline void ComposeBGR(const uint8_t* bgra, int sw, int sh, uint8_t* out) {
    std::memset(out, 0, (size_t)kCanvasW * kCanvasH * 3);
    if (!bgra || sw <= 0 || sh <= 0) return;
    const Rect r = FitPicture(sw, sh);
    const Taps tx = MakeTaps(sw, r.w), ty = MakeTaps(sh, r.h);

    // across first (sh rows of r.w pixels), then down; float keeps the two passes from compounding rounding
    std::vector<float> mid((size_t)sh * r.w * 3);
    for (int y = 0; y < sh; y++) {
        const uint8_t* row = bgra + (size_t)y * sw * 4;
        float* m = &mid[(size_t)y * r.w * 3];
        for (int x = 0; x < r.w; x++) {
            float c0 = 0, c1 = 0, c2 = 0;
            for (int i = 0; i < tx.count[x]; i++) {
                const uint8_t* p = row + (size_t)(tx.first[x] + i) * 4;
                const float w = tx.weight[tx.offset[x] + i] * (p[3] / 255.0f);   // a see-through pixel shows black
                c0 += p[0] * w; c1 += p[1] * w; c2 += p[2] * w;
            }
            m[x * 3] = c0; m[x * 3 + 1] = c1; m[x * 3 + 2] = c2;
        }
    }
    for (int y = 0; y < r.h; y++) {
        uint8_t* o = out + ((size_t)(r.y + y) * kCanvasW + r.x) * 3;
        for (int x = 0; x < r.w; x++) {
            float c0 = 0, c1 = 0, c2 = 0;
            for (int i = 0; i < ty.count[y]; i++) {
                const float* m = &mid[((size_t)(ty.first[y] + i) * r.w + x) * 3];
                const float w = ty.weight[ty.offset[y] + i];
                c0 += m[0] * w; c1 += m[1] * w; c2 += m[2] * w;
            }
            o[x * 3] = (uint8_t)(std::min)(255.0f, c0 + 0.5f);
            o[x * 3 + 1] = (uint8_t)(std::min)(255.0f, c1 + 0.5f);
            o[x * 3 + 2] = (uint8_t)(std::min)(255.0f, c2 + 0.5f);
        }
    }
}

}  // namespace art
}  // namespace phantom

#endif
