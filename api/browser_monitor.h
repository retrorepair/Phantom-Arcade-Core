#ifndef GM_BROWSER_MONITOR_H
#define GM_BROWSER_MONITOR_H

// Optional, dependency-free browser monitor for the GroovyMiSTer sender.
// Enable with GM_BROWSER_MONITOR=1 before starting the emulator/client.
// Serves the pre-compression emulator frame on http://<PC-IP>:8765/.
// This is deliberately read-only: it does not touch CRT timing, UDP, inputs,
// frame counters, modelines, or game-launch logic.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
 #include <winsock2.h>
 #include <ws2tcpip.h>
 typedef SOCKET gm_bm_socket;
 #define GM_BM_CLOSE closesocket
 #define GM_BM_INVALID INVALID_SOCKET
#else
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <unistd.h>
 typedef int gm_bm_socket;
 #define GM_BM_CLOSE close
 #define GM_BM_INVALID (-1)
#endif

class GMBrowserMonitor {
public:
    static GMBrowserMonitor& instance() {
        static GMBrowserMonitor monitor;
        return monitor;
    }

    void publish(const uint8_t* pixels, int width, int height, int mode) {
        if (!pixels || width <= 0 || height <= 0) return;
        if (!enabled()) return;
        // The browser is a preview, not part of the CRT timing path. Cap copies to
        // 20 fps so enabling it cannot add a full-frame memcpy on every 60 Hz blit.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPublish < std::chrono::milliseconds(50)) return;
        lastPublish = now;
        const int bpp = mode == 1 ? 4 : mode == 2 ? 2 : 3;
        const size_t bytes = (size_t)width * (size_t)height * (size_t)bpp;
        if (bytes > 720u * 576u * 4u) return;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            raw.assign(pixels, pixels + bytes);
            frameWidth = width;
            frameHeight = height;
            frameMode = mode;
            ++frameSequence;
        }
        startOnce();
    }

private:
    GMBrowserMonitor() : started(false), frameWidth(0), frameHeight(0), frameMode(0), frameSequence(0), lastPublish(std::chrono::steady_clock::now() - std::chrono::seconds(1)) {}
    GMBrowserMonitor(const GMBrowserMonitor&) = delete;
    GMBrowserMonitor& operator=(const GMBrowserMonitor&) = delete;

    static bool enabled() {
        const char* value = getenv("GM_BROWSER_MONITOR");
        return value && value[0] == '1';
    }

    void startOnce() {
        bool expected = false;
        if (started.compare_exchange_strong(expected, true)) {
            std::thread([this]() { serve(); }).detach();
        }
    }

    static bool sendAll(gm_bm_socket s, const char* data, size_t size) {
        while (size) {
#ifdef _WIN32
            int n = send(s, data, (int)std::min<size_t>(size, 1u << 20), 0);
#else
            #ifdef MSG_NOSIGNAL
            ssize_t n = send(s, data, size, MSG_NOSIGNAL);
            #else
            ssize_t n = send(s, data, size, 0);
            #endif
#endif
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static void put16(std::vector<uint8_t>& out, size_t p, uint16_t v) {
        out[p] = (uint8_t)v; out[p+1] = (uint8_t)(v >> 8);
    }
    static void put32(std::vector<uint8_t>& out, size_t p, uint32_t v) {
        out[p] = (uint8_t)v; out[p+1] = (uint8_t)(v >> 8);
        out[p+2] = (uint8_t)(v >> 16); out[p+3] = (uint8_t)(v >> 24);
    }

    std::vector<uint8_t> makeBmp(std::vector<uint8_t>& src, int w, int h, int mode) {
        const size_t outBytes = (size_t)w * (size_t)h * 4;
        std::vector<uint8_t> bmp(54 + outBytes, 0);
        bmp[0] = 'B'; bmp[1] = 'M';
        put32(bmp, 2, (uint32_t)bmp.size());
        put32(bmp, 10, 54);
        put32(bmp, 14, 40);
        put32(bmp, 18, (uint32_t)w);
        put32(bmp, 22, (uint32_t)h);
        put16(bmp, 26, 1);
        put16(bmp, 28, 32);
        put32(bmp, 34, (uint32_t)outBytes);
        const int inBpp = mode == 1 ? 4 : mode == 2 ? 2 : 3;
        // BMP rows are bottom-up and pixels are BGRA. Source is RGB/RGBA or RGB565.
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = src.data() + (size_t)y * w * inBpp;
            uint8_t* dst = bmp.data() + 54 + (size_t)(h - 1 - y) * w * 4;
            for (int x = 0; x < w; ++x) {
                uint8_t r, g, b;
                if (mode == 2) {
                    uint16_t p = (uint16_t)row[x*2] | ((uint16_t)row[x*2+1] << 8);
                    r = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                    g = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                    b = (uint8_t)((p & 31) * 255 / 31);
                } else {
                    r = row[x*inBpp + 0];
                    g = row[x*inBpp + 1];
                    b = row[x*inBpp + 2];
                }
                dst[x*4+0] = b; dst[x*4+1] = g; dst[x*4+2] = r;
                dst[x*4+3] = 255;
            }
        }
        return bmp;
    }

    void serveClient(gm_bm_socket client) {
        char request[2048] = {};
#ifdef _WIN32
        int got = recv(client, request, (int)sizeof(request)-1, 0);
#else
        ssize_t got = recv(client, request, sizeof(request)-1, 0);
#endif
        if (got <= 0) { GM_BM_CLOSE(client); return; }
        request[got] = '\0';
        if (strncmp(request, "GET /frame.bmp ", 15) == 0) {
            std::vector<uint8_t> snapshot;
            int w, h, mode;
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                snapshot = raw; w = frameWidth; h = frameHeight; mode = frameMode;
            }
            if (w <= 0 || h <= 0 || snapshot.empty()) {
                const char* body = "No frame received yet";
                char head[160];
                int n = snprintf(head, sizeof(head), "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)strlen(body));
                sendAll(client, head, (size_t)n); sendAll(client, body, strlen(body));
            } else {
                std::vector<uint8_t> bmp = makeBmp(snapshot, w, h, mode);
                char head[256];
                int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nCache-Control: no-store, no-cache, must-revalidate\r\nConnection: close\r\n\r\n", (unsigned)bmp.size());
                if (sendAll(client, head, (size_t)n)) sendAll(client, (const char*)bmp.data(), bmp.size());
            }
        } else if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /? ", 7) == 0) {
            static const char page[] =
                "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>GroovyMiSTer Browser Monitor</title><style>"
                "*{box-sizing:border-box}body{margin:0;background:#111;color:#eee;font:14px system-ui;text-align:center}"
                "header{padding:12px;background:#202020;display:flex;justify-content:space-between;gap:12px}"
                "main{height:calc(100vh - 48px);display:flex;align-items:center;justify-content:center;padding:8px}"
                "canvas{max-width:100%;max-height:100%;object-fit:contain;image-rendering:pixelated;background:#000}"
                "small{color:#aaa}</style></head><body><header><b>GroovyMiSTer · Browser Monitor</b><small id='status'>Connecting…</small></header>"
                "<main><canvas id='screen'></canvas></main><script>"
                "const c=document.getElementById('screen'),x=c.getContext('2d',{alpha:false}),s=document.getElementById('status');"
                "let busy=false,last=0;async function tick(){if(busy)return;busy=true;try{"
                "const r=await fetch('/frame.bmp?t='+Date.now(),{cache:'no-store'});if(!r.ok)throw Error('Waiting for first frame');"
                "const b=await r.blob(),u=URL.createObjectURL(b),im=new Image();await new Promise((ok,no)=>{im.onload=ok;im.onerror=no;im.src=u});"
                "if(c.width!==im.naturalWidth||c.height!==im.naturalHeight){c.width=im.naturalWidth;c.height=im.naturalHeight}"
                "x.drawImage(im,0,0);URL.revokeObjectURL(u);last=Date.now();s.textContent=c.width+'×'+c.height+' · live';"
                "}catch(e){s.textContent=e.message||'Reconnecting'}finally{busy=false;setTimeout(tick,50)}}tick();</script></body></html>";
            char head[192];
            int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)(sizeof(page)-1));
            if (sendAll(client, head, (size_t)n)) sendAll(client, page, sizeof(page)-1);
        } else {
            const char* body = "Not found";
            const char* head = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\n";
            sendAll(client, head, strlen(head)); sendAll(client, body, strlen(body));
        }
        GM_BM_CLOSE(client);
    }

    void serve() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return;
#endif
        gm_bm_socket server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == GM_BM_INVALID) return;
        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8765);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(server, 4) != 0) {
            GM_BM_CLOSE(server);
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }
        for (;;) {
            gm_bm_socket client = accept(server, nullptr, nullptr);
            if (client == GM_BM_INVALID) continue;
            serveClient(client);
        }
    }

    std::atomic<bool> started;
    std::mutex frameMutex;
    std::vector<uint8_t> raw;
    int frameWidth, frameHeight, frameMode;
    uint64_t frameSequence;
    std::chrono::steady_clock::time_point lastPublish;
};

inline void gm_browser_monitor_publish(const char* pixels, int width, int height, int mode) {
    GMBrowserMonitor::instance().publish((const uint8_t*)pixels, width, height, mode);
}
#endif
