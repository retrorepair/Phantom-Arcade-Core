/*
 * art_win.h - the two things art_store.h is handed: an HTTP client and a picture decoder, from what Windows ships.
 * WinHTTP for the download (it follows the system's proxy settings, does TLS, and needs nothing installed) and WIC
 * for the picture (PNG, and anything else the system can read). No third-party code.
 *
 * Link with winhttp.lib windowscodecs.lib ole32.lib.
 */
#ifndef PHANTOM_ART_WIN_H
#define PHANTOM_ART_WIN_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>

#include <mutex>
#include <string>
#include <vector>

#include "art_store.h"

namespace phantom {
namespace art {

// A window of the system's HTTP stack, kept for the life of the process: one session, many requests.
inline HINTERNET WinHttpSession() {
    static HINTERNET session = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        // Identify ourselves: the server is a volunteer-run public one, and a name is the courtesy it asks for.
        session = WinHttpOpen(L"PhantomArcade/1.0 (+https://github.com/retrorepair/Phantom-Arcade)",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"PhantomArcade/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) WinHttpSetTimeouts(session, 5000, 8000, 15000, 30000);
    });
    return session;
}

inline std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// GET a URL. Returns the HTTP status with the body in `body`, or -1 when the server could not be reached or the
// answer was too big to be a picture or a listing.
inline int HttpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    HINTERNET session = WinHttpSession();
    if (!session) return -1;

    const std::wstring wurl = WidenUtf8(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return -1;

    int status = -1;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                           : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                                WINHTTP_NO_HEADER_INDEX)) {
            status = (int)code;
            const size_t kMax = 64u << 20;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { status = -1; break; }
                if (avail == 0) break;
                const size_t at = body.size();
                if (at + avail > kMax) { status = -1; break; }
                body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, body.data() + at, avail, &got)) { status = -1; break; }
                body.resize(at + got);
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (status < 0) body.clear();
    return status;
}

template <class T>
struct ComRef {
    T* p = nullptr;
    ~ComRef() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator bool() const { return p != nullptr; }
};

// Decode a picture file (PNG, JPEG, BMP, whatever WIC reads) to B,G,R,A bytes.
inline bool DecodeImage(const std::vector<uint8_t>& file, int& w, int& h, std::vector<uint8_t>& bgra) {
    if (file.size() < 8) return false;
    // each thread that decodes needs COM; this is cheap after the first time and balanced on the way out
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool balance = SUCCEEDED(co);
    bool ok = false;
    {
        ComRef<IWICImagingFactory> factory;
        ComRef<IWICStream> stream;
        ComRef<IWICBitmapDecoder> decoder;
        ComRef<IWICBitmapFrameDecode> frame;
        ComRef<IWICFormatConverter> conv;
        UINT fw = 0, fh = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(file.data()), (DWORD)file.size())) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&fw, &fh)) && fw > 0 && fh > 0 && fw <= 8192 && fh <= 8192 &&
            SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            bgra.resize((size_t)fw * fh * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)bgra.size(), bgra.data()))) {
                w = (int)fw;
                h = (int)fh;
                ok = true;
            }
        }
    }
    if (balance) CoUninitialize();
    return ok;
}

// The real services: the system's HTTP client and the system's picture decoder.
inline Services WindowsServices() {
    Services s;
    s.get = [](const std::string& url, std::vector<uint8_t>& body) { return HttpGet(url, body); };
    s.decode = [](const std::vector<uint8_t>& f, int& w, int& h, std::vector<uint8_t>& px) { return DecodeImage(f, w, h, px); };
    s.politeness_ms = 150;
    return s;
}

}  // namespace art
}  // namespace phantom

#endif
