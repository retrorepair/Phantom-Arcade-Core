// fbneo_sets.h - which games FBNeo supports, from FBNeo itself.
//
// `fcadefbneo.exe -listinfo` prints FBNeo's whole arcade game list as a ClrMamePro-style XML
// DAT to stdout (about 6,400 games, ~10 MB, in a second). The launcher uses it to list only
// the zips FBNeo can actually start - a folder of MAME sets holds plenty it cannot - and to
// name them. FBNeo refuses a BIOS set as a game, so those are marked and left out too.
//
// Header-only and free of the daemon's globals so host/fbneo_sets_test.cpp can exercise
// exactly what ships. The part that runs the emulator lives in the daemon.
#pragma once

#include <map>
#include <string>

struct FbneoSet {
    std::string description;   // FBNeo's own long name
    std::string year;
    std::string manufacturer;
    bool        isBios = false;
};

// Undo the five XML entities a DAT uses.
inline std::string FbneoXmlDecode(const std::string& in) {
    static const struct { const char* ent; char ch; } ents[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        bool hit = false;
        if (in[i] == '&') {
            for (const auto& e : ents) {
                const size_t n = std::char_traits<char>::length(e.ent);
                if (in.compare(i, n, e.ent) == 0) { out += e.ch; i += n; hit = true; break; }
            }
        }
        if (!hit) out += in[i++];
    }
    return out;
}

// The text between <tag> and </tag> inside [from, to), or "" if the tag is not there.
inline std::string FbneoTagText(const std::string& xml, size_t from, size_t to, const char* tag) {
    const std::string open  = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open, from);
    if (a == std::string::npos || a >= to) return std::string();
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos || b > to) return std::string();
    return FbneoXmlDecode(xml.substr(a + open.size(), b - (a + open.size())));
}

// Every <game name="..."> in the DAT, keyed by set name. A malformed or empty input yields an
// empty map, which the caller reads as "could not ask FBNeo" and does not filter on.
inline std::map<std::string, FbneoSet> ParseFbneoDat(const std::string& xml) {
    std::map<std::string, FbneoSet> sets;
    const std::string gameOpen = "<game ";
    size_t at = 0;
    while ((at = xml.find(gameOpen, at)) != std::string::npos) {
        // The attributes come in any order: FBNeo writes a BIOS as <game isbios="yes" name="neogeo">
        // and everything else as <game name="ssf2t" cloneof="ssf2">.
        const size_t tagEnd = xml.find('>', at);
        if (tagEnd == std::string::npos) break;
        const std::string attrs = xml.substr(at + gameOpen.size(), tagEnd - (at + gameOpen.size()));

        const size_t nameAt = attrs.find("name=\"");
        const size_t nameEnd = (nameAt == std::string::npos) ? std::string::npos : attrs.find('"', nameAt + 6);
        if (nameEnd == std::string::npos) { at = tagEnd; continue; }

        const size_t gameEnd = xml.find("</game>", tagEnd);
        const size_t limit = (gameEnd == std::string::npos) ? xml.size() : gameEnd;

        FbneoSet s;
        s.isBios       = attrs.find("isbios=\"yes\"") != std::string::npos;
        s.description  = FbneoTagText(xml, tagEnd, limit, "description");
        s.year         = FbneoTagText(xml, tagEnd, limit, "year");
        s.manufacturer = FbneoTagText(xml, tagEnd, limit, "manufacturer");

        sets[FbneoXmlDecode(attrs.substr(nameAt + 6, nameEnd - (nameAt + 6)))] = s;
        at = limit;
    }
    return sets;
}
