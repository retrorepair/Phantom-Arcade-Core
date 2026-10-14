/*
 * art_store.h - the pictures on disk, fetched in the background, and handed out ready to draw.
 *
 * The cabinet asks for the picture of the game under its cursor. Everything slow about answering that - a
 * folder listing of thousands of names the first time, a download, a decode - happens on worker threads here, and
 * the request itself only ever looks at a file and a few tables, so it can be answered inside the HTTP handler
 * that asked. A request for a picture that is not there yet starts the fetch and says "pending"; the cabinet asks
 * again a moment later.
 *
 * What is kept, under the root folder:
 *
 *   lists/<system>__<kind>.txt     the names in a server folder, one per line (refreshed after two weeks)
 *   pic/<kind>/<system>/<id>.png   the picture, as the server sent it, filed under the library's own id for the game
 *   pic/<kind>/<system>/<id>.none  the server has no picture for this game (asked again after a month)
 *
 * Filing by the game's id rather than the server's name means a lookup never needs the listing: it is one file
 * check. Two releases of a game that map to one picture are stored twice, a few kilobytes each.
 *
 * Failure is split in two on purpose. "There is no such picture" (no match in the listing, or a 404) is recorded on
 * disk, so it is not asked again every time the cursor passes. "The server could not be reached" is not: it is
 * remembered in memory with a back-off, gives up quietly after three tries so the cabinet stops waiting, and is
 * forgotten after ten minutes or on restart.
 *
 * The server is a free public one: requests are spaced out (Services::politeness_ms between a worker's downloads)
 * and there are three workers at most, so even a download of a whole library is a trickle.
 *
 * The HTTP client and the picture decoder are injected (Services), so all of this is tested with fakes
 * (art_store_test.cpp) and the real ones (WinHTTP, WIC) live in art_win.h.
 */
#ifndef PHANTOM_ART_STORE_H
#define PHANTOM_ART_STORE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "art.h"

namespace phantom {
namespace art {

// The part of a catalog entry the store needs.
struct GameRef {
    std::string id;      // the library's id, "<system>_<set or file>"
    std::string title;
    std::string sys;     // the emulator's key, "fbneo", "pcsx2"...
    std::string rom;     // the main file's path, which says disc vs. cartridge vs. set
};

struct Services {
    // GET a URL. Returns the HTTP status with the body in `body`, or -1 when the server could not be reached.
    std::function<int(const std::string& url, std::vector<uint8_t>& body)> get;
    // Decode a picture file to B,G,R,A bytes. False when the bytes are not a picture.
    std::function<bool(const std::vector<uint8_t>& file, int& w, int& h, std::vector<uint8_t>& bgra)> decode;
    int politeness_ms = 150;
};

enum class Got { Ready, Pending, None };

struct Stats {
    int fetch_mask = 0;
    int have[kKinds] = { 0, 0 };
    int queued = 0, active = 0;
    int online = 2;   // 1 the picture server answered last time, 0 it did not, 2 not asked yet (everything came from disk)
    bool bulk_running = false;
    size_t bulk_done = 0, bulk_total = 0;
};

class Store {
public:
    Store(std::filesystem::path root, Services svc, int workers = 3) : root_(std::move(root)), svc_(std::move(svc)) {
        std::error_code ec;
        std::filesystem::create_directories(root_ / "lists", ec);
        for (int k = 0; k < kKinds; k++) have_[k] = CountPictures((Kind)k);
        for (int i = 0; i < workers; i++) workers_.emplace_back([this] { Work(); });
    }

    ~Store() {
        BulkStop();
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) t.join();
    }

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // Which kinds may be downloaded: bit 0 titles, bit 1 snaps. Pictures already on disk are served whatever this is.
    void SetFetchMask(int mask) { mask_ = mask & 3; }
    int FetchMask() const { return mask_; }

    // The picture for this game, on the 720x480 canvas. Ready fills `canvas`; Pending means a fetch is on its way
    // (ask again); None means there is no picture and none is coming.
    Got Get(Kind k, const GameRef& g, std::vector<uint8_t>* canvas) {
        const std::filesystem::path p = PicPath(k, g, ".png");
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            if (Compose(p, canvas)) return Got::Ready;
            std::filesystem::remove(p, ec);   // not a picture after all; fetch it again
        }
        const Got s = NoFile(k, g);
        if (s != Got::Pending) return s;
        Enqueue(k, g, true);
        return Got::Pending;
    }

    // Start fetching a picture nobody has asked for yet (the neighbours of the one on screen).
    void Prefetch(Kind k, const GameRef& g) {
        std::error_code ec;
        if (std::filesystem::exists(PicPath(k, g, ".png"), ec)) return;
        if (NoFile(k, g) != Got::Pending) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() > 96) return;   // the cursor is moving faster than the network; the ones in front matter
        }
        Enqueue(k, g, false);
    }

    Stats GetStats() {
        Stats s;
        s.fetch_mask = mask_;
        std::lock_guard<std::mutex> lk(m_);
        for (int k = 0; k < kKinds; k++) s.have[k] = have_[k];
        s.queued = (int)q_.size();
        s.active = (int)active_.size();
        s.online = online_;
        s.bulk_running = bulk_running_;
        s.bulk_done = bulk_done_;
        s.bulk_total = bulk_total_;
        return s;
    }

    // Fetch pictures for a whole library, at the lowest priority and without flooding the queue. `at(i, out)`
    // fills out with game i and returns false past the end. Which kinds is the current fetch mask.
    bool BulkStart(std::function<bool(size_t, GameRef&)> at, size_t total) {
        BulkStop();
        {
            std::lock_guard<std::mutex> lk(m_);
            bulk_stop_ = false;
            bulk_running_ = true;
            bulk_done_ = 0;
            bulk_total_ = total;
        }
        bulk_ = std::thread([this, at, total] {
            for (size_t i = 0; i < total; i++) {
                GameRef g;
                if (!at(i, g)) break;
                for (int k = 0; k < kKinds; k++) {
                    if (!(mask_ & (1 << k))) continue;
                    std::error_code ec;
                    if (std::filesystem::exists(PicPath((Kind)k, g, ".png"), ec)) continue;
                    if (NoFile((Kind)k, g) != Got::Pending) continue;
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        while (!bulk_stop_ && q_.size() >= 8) bulk_cv_.wait_for(lk, std::chrono::milliseconds(200));
                        if (bulk_stop_) break;
                    }
                    Enqueue((Kind)k, g, false);
                }
                std::lock_guard<std::mutex> lk(m_);
                if (bulk_stop_) break;
                bulk_done_ = i + 1;
            }
            {
                std::lock_guard<std::mutex> lk(m_);
                bulk_running_ = false;
            }
            idle_cv_.notify_all();
        });
        return true;
    }

    void BulkStop() {
        {
            std::lock_guard<std::mutex> lk(m_);
            bulk_stop_ = true;
        }
        bulk_cv_.notify_all();
        if (bulk_.joinable()) bulk_.join();
    }

    // Tests: wait until nothing is queued or running (or the time is up). True when idle.
    bool WaitIdle(int ms) {
        std::unique_lock<std::mutex> lk(m_);
        return idle_cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return q_.empty() && active_.empty() && !bulk_running_; });
    }

private:
    struct Job {
        Kind k;
        GameRef g;
        std::string key;
    };

    struct Listing {
        std::mutex m;
        bool loaded = false;
        Matcher matcher;
    };

    struct Failure {
        int count = 0;
        std::chrono::steady_clock::time_point until;
    };

    static constexpr int kGiveUpAfter = 3;
    static constexpr int kForgetFailuresSec = 600;
    static constexpr int kListingMaxAgeDays = 14;
    static constexpr int kNoneMaxAgeDays = 30;

    std::filesystem::path PicPath(Kind k, const GameRef& g, const char* ext) const {
        return root_ / "pic" / KindName(k) / FileSafe(g.sys) / (FileSafe(g.id) + ext);
    }

    static std::string Key(Kind k, const GameRef& g) { return std::string(KindName(k)) + "|" + g.id; }

    int CountPictures(Kind k) const {
        int n = 0;
        std::error_code ec;
        const std::filesystem::path d = root_ / "pic" / KindName(k);
        if (!std::filesystem::exists(d, ec)) return 0;
        for (std::filesystem::recursive_directory_iterator it(d, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".png") n++;
        return n;
    }

    static bool OlderThanDays(const std::filesystem::path& p, int days) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return true;
        return std::filesystem::file_time_type::clock::now() - t > std::chrono::hours(24 * days);
    }

    // What there is to say about a picture that is not on disk: None if it is known not to exist, or may not be
    // fetched, or has failed too often; Pending if it could be fetched.
    Got NoFile(Kind k, const GameRef& g) {
        std::error_code ec;
        const std::filesystem::path none = PicPath(k, g, ".none");
        if (std::filesystem::exists(none, ec) && !OlderThanDays(none, kNoneMaxAgeDays)) return Got::None;
        if (!(mask_ & (1 << k))) return Got::None;
        if (Repos(g.sys, g.rom).empty()) return Got::None;
        std::lock_guard<std::mutex> lk(m_);
        auto f = fails_.find(Key(k, g));
        if (f != fails_.end() && f->second.count >= kGiveUpAfter) {
            if (std::chrono::steady_clock::now() - f->second.until < std::chrono::seconds(kForgetFailuresSec)) return Got::None;
            fails_.erase(f);
        }
        return Got::Pending;
    }

    void Enqueue(Kind k, const GameRef& g, bool urgent) {
        const std::string key = Key(k, g);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (active_.count(key)) return;
            auto f = fails_.find(key);
            if (f != fails_.end() && std::chrono::steady_clock::now() < f->second.until && f->second.count < kGiveUpAfter) return;   // backing off
            auto it = qpos_.find(key);
            if (it != qpos_.end()) {
                if (urgent) q_.splice(q_.begin(), q_, it->second);   // already waiting: the cursor is on it now
                return;
            }
            Job j{ k, g, key };
            if (urgent) { q_.push_front(std::move(j)); qpos_[key] = q_.begin(); }
            else { q_.push_back(std::move(j)); qpos_[key] = std::prev(q_.end()); }
        }
        cv_.notify_one();
    }

    // The matcher for a server folder, from disk if it is recent, else from the server. Null when neither works.
    std::shared_ptr<Listing> GetListing(const std::string& repo, Kind k, bool* unreachable) {
        const std::string name = FileSafe(repo) + "__" + KindName(k);
        std::shared_ptr<Listing> l;
        {
            std::lock_guard<std::mutex> lk(m_);
            std::shared_ptr<Listing>& slot = listings_[name];
            if (!slot) slot = std::make_shared<Listing>();
            l = slot;
        }
        std::lock_guard<std::mutex> lk(l->m);
        if (l->loaded) return l;

        const std::filesystem::path file = root_ / "lists" / (name + ".txt");
        std::error_code ec;
        const bool have = std::filesystem::exists(file, ec);
        if (have && !OlderThanDays(file, kListingMaxAgeDays) && LoadNames(file, &l->matcher)) { l->loaded = true; return l; }

        std::vector<uint8_t> body;
        const int status = svc_.get(ListingUrl(repo, k), body);
        if (status == 200) {
            const std::string_view page((const char*)body.data(), body.size());
            const std::vector<std::string> names = ParseListing(page);
            // A folder with nothing in it is a real answer (the server has no PS3 title screens), told apart from a
            // captive-portal or error page by being a directory index.
            if (!names.empty() || page.find("Index of") != std::string_view::npos) {
                SetOnline(true);
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                for (const std::string& n : names) out << n << "\n";
                out.close();
                l->matcher.Clear();
                for (const std::string& n : names) l->matcher.Add(n);
                l->matcher.Build();
                l->loaded = true;
                return l;
            }
        } else if (status == 404) {
            // The server has no such folder: nothing to find, and nothing to retry. An empty listing answers for it.
            SetOnline(true);
            l->matcher.Build();
            l->loaded = true;
            return l;
        }
        if (status < 0) SetOnline(false);
        if (have && LoadNames(file, &l->matcher)) { l->loaded = true; return l; }   // stale beats nothing
        *unreachable = true;
        return nullptr;
    }

    static bool LoadNames(const std::filesystem::path& file, Matcher* m) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        m->Clear();
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) m->Add(line);
        }
        m->Build();
        return true;   // an empty file is a folder with no pictures in it
    }

    void SetOnline(bool v) {
        std::lock_guard<std::mutex> lk(m_);
        online_ = v ? 1 : 0;
    }

    bool Compose(const std::filesystem::path& p, std::vector<uint8_t>* canvas) {
        std::string key = p.string();
        std::error_code ec;
        const auto mt = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
        key += "@" + std::to_string((long long)mt);
        {
            std::lock_guard<std::mutex> lk(cache_m_);
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                if (canvas) *canvas = it->second;
                return true;
            }
        }
        std::ifstream in(p, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int w = 0, h = 0;
        std::vector<uint8_t> bgra;
        if (file.empty() || !svc_.decode(file, w, h, bgra) || w <= 0 || h <= 0 || bgra.size() < (size_t)w * h * 4) return false;
        std::vector<uint8_t> out((size_t)kCanvasW * kCanvasH * 3);
        ComposeBGR(bgra.data(), w, h, out.data());
        {
            std::lock_guard<std::mutex> lk(cache_m_);
            if (cache_order_.size() >= 12) {
                cache_.erase(cache_order_.front());
                cache_order_.pop_front();
            }
            cache_[key] = out;
            cache_order_.push_back(key);
        }
        if (canvas) *canvas = std::move(out);
        return true;
    }

    void MarkNone(Kind k, const GameRef& g) {
        const std::filesystem::path p = PicPath(k, g, ".none");
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary | std::ios::trunc).put('\n');
    }

    bool SavePicture(Kind k, const GameRef& g, const std::vector<uint8_t>& body) {
        const std::filesystem::path p = PicPath(k, g, ".png");
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        const std::filesystem::path tmp = p.string() + ".part";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write((const char*)body.data(), (std::streamsize)body.size());
            if (!out) { std::filesystem::remove(tmp, ec); return false; }
        }
        std::filesystem::rename(tmp, p, ec);
        if (ec) { std::filesystem::remove(tmp, ec); return false; }
        std::filesystem::remove(PicPath(k, g, ".none"), ec);
        return true;
    }

    enum class Outcome { Saved, NoPicture, Unreachable };

    Outcome Fetch(Kind k, const GameRef& g) {
        const std::vector<std::string> repos = Repos(g.sys, g.rom);
        const std::string file = StemOf(g.rom);
        bool unreachable = false;
        for (const std::string& repo : repos) {
            bool down = false;
            std::shared_ptr<Listing> l = GetListing(repo, k, &down);
            if (!l) { unreachable |= down; continue; }
            const std::string stem = l->matcher.Find(g.title, file);
            if (stem.empty()) continue;

            std::vector<uint8_t> body;
            const int status = svc_.get(PictureUrl(repo, k, stem), body);
            if (svc_.politeness_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(svc_.politeness_ms));
            if (status == 200) {
                int w = 0, h = 0;
                std::vector<uint8_t> px;
                SetOnline(true);
                if (!svc_.decode(body, w, h, px) || w <= 0 || h <= 0) continue;   // not a picture; try the next folder
                if (!SavePicture(k, g, body)) { unreachable = true; continue; }
                return Outcome::Saved;
            }
            if (status == 404) { SetOnline(true); continue; }
            if (status < 0) SetOnline(false);
            unreachable = true;
        }
        return unreachable ? Outcome::Unreachable : Outcome::NoPicture;
    }

    void Work() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return quit_ || !q_.empty(); });
                if (quit_) return;
                j = std::move(q_.front());
                q_.pop_front();
                qpos_.erase(j.key);
                active_.insert(j.key);
            }
            bulk_cv_.notify_all();

            const Outcome o = Fetch(j.k, j.g);

            {
                std::lock_guard<std::mutex> lk(m_);
                active_.erase(j.key);
                if (o == Outcome::Saved) {
                    have_[j.k]++;
                    fails_.erase(j.key);
                } else if (o == Outcome::Unreachable) {
                    Failure& f = fails_[j.key];
                    f.count++;
                    f.until = std::chrono::steady_clock::now() + std::chrono::seconds((std::min)(60, 5 * f.count));
                }
            }
            if (o == Outcome::NoPicture) MarkNone(j.k, j.g);
            bulk_cv_.notify_all();
            idle_cv_.notify_all();
        }
    }

    std::filesystem::path root_;
    Services svc_;
    std::atomic<int> mask_{ 3 };

    std::mutex m_;
    std::condition_variable cv_, idle_cv_, bulk_cv_;
    std::list<Job> q_;
    std::unordered_map<std::string, std::list<Job>::iterator> qpos_;
    std::unordered_set<std::string> active_;
    std::unordered_map<std::string, Failure> fails_;
    std::map<std::string, std::shared_ptr<Listing>> listings_;
    int have_[kKinds] = { 0, 0 };
    int online_ = 2;
    bool quit_ = false;
    bool bulk_stop_ = false, bulk_running_ = false;
    size_t bulk_done_ = 0, bulk_total_ = 0;
    std::thread bulk_;
    std::vector<std::thread> workers_;

    std::mutex cache_m_;
    std::unordered_map<std::string, std::vector<uint8_t>> cache_;
    std::deque<std::string> cache_order_;
};

}  // namespace art
}  // namespace phantom

#endif
