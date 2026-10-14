/*
 * library_scan.h - find the games, fast, and only do the slow part for what is new.
 *
 * What was slow, in the scan this replaces, and what replaces it:
 *
 *   - Every file went through std::filesystem, and each game through fs::relative (which resolves
 *     both paths on disk) and a string search of the extension list. -> rom_enum.h, one pass, and
 *     a hash lookup for the extension.
 *   - Everything ran on the window's thread, so the manager froze for the whole scan. -> the caller
 *     runs this on a worker; it reports progress and publishes as it goes.
 *   - Every scan re-read every PS3 PARAM.SFO and Xbox disc, and asked MAME about every set again.
 *     -> a game whose file has the same size and modification time as in the last scan keeps what
 *     was learned about it; only new and changed files are examined.
 *   - MAME was given every set name on one command line. Windows stops at about 32,000 characters,
 *     so a library of a few thousand sets failed outright - and the failure was swallowed, so MAME
 *     filtering silently switched itself off. -> sets are asked about in batches that fit.
 *   - Nothing could be shown until the whole scan finished. -> the games that need no vetting are
 *     published as soon as they are found, and MAME sets join as each batch is answered.
 *
 * A set MAME calls unrunnable is not remembered as a record (the index lists only what can be
 * played), so there is a second, small file: the stems MAME rejected and a fingerprint of the MAME
 * folder and executable when it did. If nothing under that folder has changed, the verdicts are
 * still true and MAME is not asked again; the moment anything changes, a missing parent could have
 * been added, so everything not already known good is asked again.
 *
 * The parts that need the daemon's own code (reading PARAM.SFO, XBE certificates, running MAME) come
 * in through ScanServices, which is also what lets library_scan_test.cpp run the real scan with
 * stand-ins and count how often each is called.
 */
#ifndef PHANTOM_LIBRARY_SCAN_H
#define PHANTOM_LIBRARY_SCAN_H

#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "fbneo_sets.h"
#include "game_index.h"
#include "rom_enum.h"

namespace phantom {

struct ScanTarget {
    std::string key, name, videoMode;       // "groovymame", "GroovyMAME Arcade", "15kHz Native"
    std::vector<std::wstring> roots;        // the first is the configured folder; any others are scanned as well
    std::wstring exts;                      // ".zip,.7z,.chd", lower case
};

struct Derived {
    std::string title, year, maker;
};

struct MameVerdict {
    bool runnable = true;
    std::string mode, year, maker;
};

struct ScanServices {
    const std::map<std::string, std::string>* mameTitles = nullptr;     // MAME's own names, by set
    const std::map<std::string, FbneoSet>* fbneoSets = nullptr;         // what FBNeo can start; null or empty = do not filter
    std::function<std::string(const std::wstring&)> ps3Title;           // PARAM.SFO name for a game file, or ""
    std::function<Derived(const std::wstring&)> xboxMeta;               // from the disc's own executable
    std::function<std::string(const std::string&)> fold;                // UTF-8 down to what the cabinet's font can draw
    std::function<void(const std::vector<std::string>&, std::vector<MameVerdict>&)> mameVerify;   // one batch of set names
    std::string mameFingerprint;                                        // identifies the MAME build the verdicts came from
};

struct ScanOptions {
    bool full = false;                       // ignore everything learned last time
    int threads = 4;
    std::filesystem::path rejectsFile;       // empty = MAME's rejections are not remembered
    const std::atomic<bool>* stop = nullptr; // set to abandon the scan between batches
};

struct ScanStats {
    uint64_t files = 0, listed = 0, reused = 0, derived = 0, pkgs = 0, fbneoSkipped = 0;
    uint64_t mameAsked = 0, mameRejected = 0, mameRemembered = 0;
    double enumMs = 0, deriveMs = 0, verifyMs = 0, totalMs = 0;
};

using PublishFn = std::function<void(std::shared_ptr<Index>, bool final)>;
using ProgressFn = std::function<void(const std::string&)>;

namespace scan_detail {

inline std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

inline std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

inline bool IEndsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() > n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

inline std::unordered_set<std::wstring> ExtSet(const std::wstring& exts) {
    std::unordered_set<std::wstring> out;
    size_t at = 0;
    while (at <= exts.size()) {
        const size_t comma = exts.find(L',', at);
        std::wstring e = exts.substr(at, (comma == std::wstring::npos ? exts.size() : comma) - at);
        if (!e.empty()) out.insert(Lower(e));
        if (comma == std::wstring::npos) break;
        at = comma + 1;
    }
    return out;
}

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 1099511628211ull;
}

template <class Fn>
void ParallelFor(size_t n, int threads, Fn&& fn) {
    std::atomic<size_t> next(0);
    auto work = [&] {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= n) break;
            fn(i);
        }
    };
    if (threads < 1) threads = 1;
    if ((size_t)threads > n) threads = (int)std::max<size_t>(1, n);
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; i++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

struct Cand {
    std::string stem, rom;      // UTF-8; rom has forward slashes
    std::wstring full;          // only kept for the systems whose metadata is read from the file
    uint64_t size = 0, mtime = 0;
};

struct Local {
    std::vector<Cand> cands;
    uint64_t fingerprint = 0, pkgs = 0;
};

}  // namespace scan_detail

inline double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

inline ScanStats RunScan(const std::vector<ScanTarget>& targets, const ScanServices& svc, const ScanOptions& opt,
                         const std::shared_ptr<const Index>& old, const PublishFn& publish, const ProgressFn& progress) {
    using namespace scan_detail;
    ScanStats st;
    const auto tStart = std::chrono::steady_clock::now();
    auto say = [&](const std::string& s) { if (progress) progress(s); };
    auto stopped = [&] { return opt.stop && opt.stop->load(); };
    auto fold = [&](const std::string& s) { return svc.fold ? svc.fold(s) : s; };

    IndexBuilder b;
    for (const ScanTarget& t : targets) b.AddSystem(t.key, t.name, t.roots.empty() ? std::string() : Utf8(t.roots[0]));

    // What the last scan knew, found again by file path. Nothing is reused for a system whose folder
    // setting has changed, and nothing at all for a full rescan.
    std::unordered_map<std::string_view, uint32_t> oldByRom;
    std::unordered_set<std::string> oldSysUsable;
    if (old && !opt.full) {
        for (size_t i = 0; i < targets.size(); i++)
            for (const SysInfo& s : old->sys)
                if (s.key == targets[i].key && !targets[i].roots.empty() && s.root == Utf8(targets[i].roots[0]))
                    oldSysUsable.insert(s.key);
        oldByRom.reserve(old->recs.size() * 2);
        for (uint32_t i = 0; i < old->recs.size(); i++) oldByRom.emplace(old->Rom(old->recs[i]), i);
    }
    auto reusable = [&](const std::string& key, const Cand& c) -> const Rec* {
        if (!oldSysUsable.count(key)) return nullptr;
        auto it = oldByRom.find(c.rom);
        if (it == oldByRom.end()) return nullptr;
        const Rec& r = old->recs[it->second];
        return (old->sys[r.sys].key == key && r.size == c.size && r.mtime == c.mtime) ? &r : nullptr;
    };

    // MAME's remembered rejections.
    std::unordered_set<std::string> rejects;
    std::string rejectsFp;
    uint64_t mameTreeFp = 0;
    bool haveMameTree = false;

    std::vector<uint32_t> pendingRec;
    std::vector<std::string> pendingStem;

    struct DeriveJob { uint32_t rec; const Cand* cand; bool xbox; };
    std::vector<DeriveJob> jobs;
    std::vector<std::vector<Cand>> kept(targets.size());   // candidates must outlive the record loop for the jobs

    const auto tEnum = std::chrono::steady_clock::now();
    for (size_t ti = 0; ti < targets.size(); ti++) {
        const ScanTarget& t = targets[ti];
        const bool isMame = t.key == "groovymame", isFb = t.key == "fbneo", isPs3 = t.key == "rpcs3",
                   isXbox = t.key == "xemu", is86 = t.key == "86box";
        const std::unordered_set<std::wstring> exts = ExtSet(t.exts);
        const int threads = std::max(1, opt.threads);
        std::vector<Local> locals((size_t)threads);

        for (const std::wstring& root : t.roots) {
            if (root.empty() || GetFileAttributesW(LongPath(root).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            say("Scanning " + t.name + "...");

            std::wstring r = root;
            while (r.size() > 1 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
            const size_t rootLen = r.size();

            EnumStats es = EnumTree(root, 5, threads, [&](int w, const std::wstring& dir, const WIN32_FIND_DATAW& fd, int) {
                Local& L = locals[(size_t)w];
                const wchar_t* nm = fd.cFileName;
                const uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                const uint64_t mtime = FileTimeToU64(fd.ftLastWriteTime);

                if (isMame) {   // every file under the MAME folder, wanted or not, goes into the fingerprint
                    uint64_t h = 1469598103934665603ull;
                    for (const wchar_t* p = nm; *p; p++) h = Mix(h, (uint64_t)*p);
                    for (size_t i = rootLen; i < dir.size(); i++) h = Mix(h, (uint64_t)dir[i]);
                    L.fingerprint += Mix(Mix(h, size), mtime);   // a sum, so the order the threads finish in does not matter
                }

                const wchar_t* dot = wcsrchr(nm, L'.');
                std::wstring ext = dot ? Lower(dot) : std::wstring();
                // PS3 games arrive as .pkg installers, which RPCS3 installs rather than runs, and one title is a
                // dozen of them (base, patches, DLC). They are counted, not listed; the launchable form is EBOOT.BIN.
                if (isPs3 && ext == L".pkg") { L.pkgs++; return; }

                bool wanted = !ext.empty() && exts.count(ext);
                const bool eboot = isPs3 && _wcsicmp(nm, L"eboot.bin") == 0;
                if (eboot) wanted = true;
                if (!wanted) return;

                Cand c;
                c.size = size;
                c.mtime = mtime;
                std::wstring wstem(nm, dot ? (size_t)(dot - nm) : wcslen(nm));

                std::wstring rel = dir.size() > rootLen ? dir.substr(rootLen + 1) : std::wstring();
                if (isMame || isFb) {
                    // A set is a name, not a file: its CHDs sit in a folder called after it (roms/sfiii3/cap-33s-1.chd),
                    // so a nested file belongs to the folder and its own stem would invent games.
                    if (!rel.empty()) {
                        const size_t sl = rel.find(L'\\');
                        wstem = rel.substr(0, sl);
                    }
                } else if (is86) {
                    // A machine per folder, and the file every one has is 86box.cfg: the folder is the name.
                    const size_t sl = dir.find_last_of(L'\\');
                    if (sl != std::wstring::npos && sl + 1 < dir.size()) wstem = dir.substr(sl + 1);
                }
                if (eboot) {
                    // A generic filename says nothing: name it after the folder, stepping over a disc's layout directories.
                    std::wstring p = dir;
                    for (int up = 0; up < 3 && !p.empty(); up++) {
                        const size_t sl = p.find_last_of(L'\\');
                        const std::wstring n = sl == std::wstring::npos ? p : p.substr(sl + 1);
                        const std::wstring nl = Lower(n);
                        if (nl != L"usrdir" && nl != L"ps3_game" && !n.empty()) { wstem = n; break; }
                        if (sl == std::wstring::npos) break;
                        p = p.substr(0, sl);
                    }
                }
                c.stem = Utf8(wstem);
                // ".nkit" and ".decrypted" are container formats, not part of a title ("Game.nkit.iso").
                for (const char* sfx : { ".nkit", ".nkit.gcz", ".decrypted" })
                    if (IEndsWith(c.stem, sfx)) { c.stem.erase(c.stem.size() - strlen(sfx)); break; }

                c.rom = Utf8(dir + L"\\" + nm);
                for (char& ch : c.rom) if (ch == '\\') ch = '/';
                if (isPs3 || isXbox) c.full = dir + L"\\" + nm;
                L.cands.push_back(std::move(c));
            });
            st.files += es.files;
        }

        // one entry per game: the file path settles which when two files claim the same name, so the answer
        // does not depend on which thread got there first
        //
        // For the arcade systems a name that differs only by case is the same set - Windows does not tell
        // roms/kinst.zip from roms/Kinst/kinst.chd's folder, and neither does MAME - and the lower-case one is
        // the set's real name.
        const bool arcade = (t.key == "groovymame" || t.key == "fbneo");
        auto isLower = [](const std::string& s) {
            for (char ch : s) if (ch >= 'A' && ch <= 'Z') return false;
            return true;
        };
        //
        // For everything else, when one game has several files - Crazy Taxi as a .7z beside a folder holding its .cue and
        // .bin - the one the emulator can open directly is the one to launch. An archive loses to a loose disc image or
        // descriptor: Flycast, for one, stops without a word on a .7z of a disc, and the old scan only ever got this right
        // by the accident of the order the folder was read in.
        auto archiveRank = [](const std::string& rom) {
            const size_t dot = rom.rfind('.');
            if (dot == std::string::npos) return 0;
            std::string e = rom.substr(dot);
            for (char& ch : e) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            return (e == ".zip" || e == ".7z" || e == ".rar") ? 1 : 0;
        };
        std::unordered_map<std::string, size_t> best;
        std::vector<Cand>& all = kept[ti];
        for (Local& L : locals) {
            st.pkgs += L.pkgs;
            if (isMame) { mameTreeFp += L.fingerprint; haveMameTree = true; }
            for (Cand& c : L.cands) {
                std::string key = c.stem;
                if (arcade) for (char& ch : key) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
                auto it = best.find(key);
                if (it == best.end()) { best.emplace(key, all.size()); all.push_back(std::move(c)); continue; }
                Cand& cur = all[it->second];
                bool better;
                if (arcade && isLower(c.stem) != isLower(cur.stem)) better = isLower(c.stem);
                else if (!arcade && archiveRank(c.rom) != archiveRank(cur.rom)) better = archiveRank(c.rom) < archiveRank(cur.rom);
                else better = c.rom < cur.rom;
                if (better) cur = std::move(c);
            }
        }
    }
    st.enumMs = MsSince(tEnum);

    if (haveMameTree && !opt.full && !opt.rejectsFile.empty()) {
        std::ifstream f(opt.rejectsFile, std::ios::binary);
        std::string line;
        if (f && std::getline(f, line) && line.rfind("FP\t", 0) == 0) {
            rejectsFp = line.substr(3);
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (!line.empty()) rejects.insert(line);
            }
        }
    }
    char fpbuf[64];
    snprintf(fpbuf, sizeof(fpbuf), "%016llx", (unsigned long long)mameTreeFp);
    const std::string currentFp = std::string(fpbuf) + "/" + svc.mameFingerprint;
    const bool rejectsTrusted = haveMameTree && !rejects.empty() && rejectsFp == currentFp;

    // ---- turn candidates into records ----
    const auto tDerive = std::chrono::steady_clock::now();
    auto mameTitle = [&](const std::string& stem) -> const std::string* {
        if (!svc.mameTitles) return nullptr;
        auto it = svc.mameTitles->find(stem);
        return it == svc.mameTitles->end() ? nullptr : &it->second;
    };

    for (size_t ti = 0; ti < targets.size(); ti++) {
        const ScanTarget& t = targets[ti];
        const bool isMame = t.key == "groovymame", isFb = t.key == "fbneo", isPs3 = t.key == "rpcs3", isXbox = t.key == "xemu";
        const bool filterFb = isFb && svc.fbneoSets && !svc.fbneoSets->empty();

        for (const Cand& c : kept[ti]) {
            const std::string id = t.key + "_" + c.stem;
            const Rec* prev = reusable(t.key, c);

            if (isMame) {
                const std::string* mt = mameTitle(c.stem);
                const std::string title = fold(mt ? *mt : c.stem);
                if (prev) {   // what MAME told us about this set last time still holds
                    b.Add((int)ti, id, title, c.rom, old->Str(prev->mode), old->Str(prev->year), old->Str(prev->maker), c.size, c.mtime);
                    st.reused++;
                } else if (rejectsTrusted && rejects.count(c.stem)) {
                    st.mameRemembered++;
                } else {
                    const uint32_t r = b.Add((int)ti, id, title, c.rom, t.videoMode, "", "", c.size, c.mtime, kRecPending);
                    pendingRec.push_back(r);
                    pendingStem.push_back(c.stem);
                }
                continue;
            }

            if (isFb) {
                // FBNeo names its sets the way MAME does, so MAME's list gives its zips real titles too; a set MAME
                // has no entry for takes FBNeo's own description, and failing that keeps the zip's name.
                const FbneoSet* fs = nullptr;
                if (filterFb) {
                    auto it = svc.fbneoSets->find(c.stem);
                    if (it == svc.fbneoSets->end() || it->second.isBios) { st.fbneoSkipped++; continue; }
                    fs = &it->second;
                }
                const std::string* mt = mameTitle(c.stem);
                std::string title = mt ? *mt : (fs && !fs->description.empty() ? fs->description : c.stem);
                b.Add((int)ti, id, fold(title), c.rom, t.videoMode, fs ? fold(fs->year) : "", fs ? fold(fs->manufacturer) : "",
                      c.size, c.mtime);
                continue;
            }

            if (isPs3 || isXbox) {
                if (prev) {
                    b.AddFrom(*old, *prev, (int)ti);
                    st.reused++;
                } else {
                    const uint32_t r = b.Add((int)ti, id, fold(c.stem), c.rom, t.videoMode, "", "", c.size, c.mtime);
                    jobs.push_back({ r, &c, isXbox });
                }
                continue;
            }

            b.Add((int)ti, id, fold(c.stem), c.rom, t.videoMode, "", "", c.size, c.mtime);
        }
    }

    // Reading a file's own metadata is I/O-bound and independent per file.
    if (!jobs.empty()) {
        say("Reading " + std::to_string(jobs.size()) + " new game(s)...");
        std::vector<Derived> got(jobs.size());
        ParallelFor(jobs.size(), std::max(1, opt.threads) * 2, [&](size_t i) {
            const DeriveJob& j = jobs[i];
            if (j.xbox) { if (svc.xboxMeta) got[i] = svc.xboxMeta(j.cand->full); }
            else if (svc.ps3Title) got[i].title = svc.ps3Title(j.cand->full);
        });
        for (size_t i = 0; i < jobs.size(); i++) {
            if (!got[i].title.empty()) b.SetTitle(jobs[i].rec, fold(got[i].title));
            if (!got[i].year.empty() || !got[i].maker.empty())
                b.SetMeta(jobs[i].rec, b.Str(b.At(jobs[i].rec).mode), fold(got[i].year), fold(got[i].maker));
        }
        st.derived += jobs.size();
    }
    st.deriveMs = MsSince(tDerive);

    // With no way to ask MAME its sets are listed as found, not held back for an answer that cannot come.
    if (!svc.mameVerify) {
        for (uint32_t r : pendingRec) b.SetFlags(r, 0);
        pendingRec.clear();
        pendingStem.clear();
    }

    // Everything that needs no vetting is listed now; MAME sets join as they are answered.
    if (publish) publish(b.Finish(0), pendingRec.empty());

    // ---- ask MAME, a batch at a time ----
    const auto tVerify = std::chrono::steady_clock::now();
    std::vector<std::string> newRejects;
    bool verifyComplete = true;
    if (!pendingRec.empty()) {
        const size_t kMaxChars = 24000, kMaxNames = 4000;
        auto lastPublish = std::chrono::steady_clock::now();
        size_t i = 0;
        const size_t batches = [&] {
            size_t n = 0, chars = 0, cnt = 0;
            for (const std::string& s : pendingStem) {
                if (cnt && (chars + s.size() + 1 > kMaxChars || cnt >= kMaxNames)) { n++; chars = 0; cnt = 0; }
                chars += s.size() + 1;
                cnt++;
            }
            return n + (cnt ? 1 : 0);
        }();
        size_t batchNo = 0;
        while (i < pendingRec.size()) {
            if (stopped()) { verifyComplete = false; break; }
            size_t chars = 0, j = i;
            std::vector<std::string> names;
            while (j < pendingRec.size() && names.size() < kMaxNames && (names.empty() || chars + pendingStem[j].size() + 1 <= kMaxChars)) {
                names.push_back(pendingStem[j]);
                chars += pendingStem[j].size() + 1;
                j++;
            }
            say("Checking " + std::to_string(pendingRec.size()) + " new arcade set(s) with MAME (batch " +
                std::to_string(++batchNo) + " of " + std::to_string(batches) + ")...");

            std::vector<MameVerdict> verdicts;
            svc.mameVerify(names, verdicts);
            const bool answered = verdicts.size() == names.size();
            for (size_t k = 0; k < names.size(); k++) {
                const uint32_t rec = pendingRec[i + k];
                if (answered && !verdicts[k].runnable) {
                    b.SetFlags(rec, kRecRejected);
                    newRejects.push_back(names[k]);
                    st.mameRejected++;
                } else {
                    // An unanswered batch lists its sets rather than hiding them: a MAME that cannot be asked must not
                    // silently empty somebody's library.
                    if (answered) b.SetMeta(rec, verdicts[k].mode.empty() ? b.Str(b.At(rec).mode) : verdicts[k].mode,
                                            verdicts[k].year, verdicts[k].maker);
                    b.SetFlags(rec, 0);
                }
            }
            if (!answered) verifyComplete = false;
            st.mameAsked += names.size();
            i = j;

            if (i < pendingRec.size() && MsSince(lastPublish) >= 2500) {
                if (publish) publish(b.Finish(0), false);
                lastPublish = std::chrono::steady_clock::now();
            }
        }
    }
    st.verifyMs = MsSince(tVerify);

    if (verifyComplete && haveMameTree && !opt.rejectsFile.empty()) {
        std::set<std::string> all(newRejects.begin(), newRejects.end());
        if (rejectsTrusted) all.insert(rejects.begin(), rejects.end());
        std::ofstream f(opt.rejectsFile, std::ios::binary | std::ios::trunc);
        if (f) {
            f << "FP\t" << currentFp << "\n";
            for (const std::string& s : all) f << s << "\n";
        }
    }

    if (!stopped() && publish && !pendingRec.empty()) publish(b.Finish(0), true);

    for (size_t s = 0; s < b.Size(); s++) if (!(b.At((uint32_t)s).flags & (kRecPending | kRecRejected))) st.listed++;
    st.totalMs = MsSince(tStart);
    return st;
}

}  // namespace phantom

#endif
