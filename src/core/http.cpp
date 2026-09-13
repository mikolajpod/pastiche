#include "http.hpp"

#include "fs.hpp"
#include "version.hpp"

#include <cstdio>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(HAVE_CURL)
#include <curl/curl.h>
#endif

namespace pastiche {

namespace {

const char* kUserAgent = "pastiche/" PASTICHE_VERSION;

std::string part_path(const std::string& dest) { return dest + ".part"; }

// Renames over an existing file; std::rename refuses to on Windows.
std::string move_into_place(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    if (MoveFileExW(utf8_to_wide(from).c_str(), utf8_to_wide(to).c_str(),
                    MOVEFILE_REPLACE_EXISTING)) {
        return {};
    }
    return "cannot rename " + from + " to " + to + " (error " +
           std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
#else
    if (std::rename(from.c_str(), to.c_str()) == 0) return {};
    return "cannot rename " + from + " to " + to;
#endif
}

} // namespace

#ifdef _WIN32

namespace {

// Closes a WinHTTP handle when it goes out of scope; the download path has
// several early returns and leaking a session on each would be easy.
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { if (h_) WinHttpCloseHandle(h_); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle& operator=(HINTERNET h)
    {
        if (h_) WinHttpCloseHandle(h_);
        h_ = h;
        return *this;
    }

    operator HINTERNET() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

std::string last_error(const char* what)
{
    return std::string(what) + " failed (error " +
           std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
}

} // namespace

bool http_available() { return true; }

HttpResult http_download(const std::string& url, const std::string& dest_path,
                         HttpProgress* progress, bool resume)
{
    HttpResult result;

    const std::wstring wurl = utf8_to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof host / sizeof host[0];
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = sizeof path / sizeof path[0];
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        result.error = "cannot parse URL: " + url;
        return result;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    WinHttpHandle session(WinHttpOpen(utf8_to_wide(kUserAgent).c_str(),
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        result.error = last_error("WinHttpOpen");
        return result;
    }

    WinHttpHandle connect(WinHttpConnect(session, host, parts.nPort, 0));
    if (!connect) {
        result.error = last_error("WinHttpConnect");
        return result;
    }

    const std::string partial = part_path(dest_path);
    uint64_t already_have = 0;
    if (resume && file_exists(partial)) already_have = file_size(partial);

    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        result.error = last_error("WinHttpOpenRequest");
        return result;
    }

    if (already_have > 0) {
        const std::wstring range = L"Range: bytes=" + std::to_wstring(already_have) + L"-";
        WinHttpAddRequestHeaders(request, range.c_str(), static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        result.error = last_error("WinHttpSendRequest");
        return result;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        result.error = last_error("WinHttpReceiveResponse");
        return result;
    }

    DWORD status = 0, status_size = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        result.error = last_error("WinHttpQueryHeaders");
        return result;
    }

    // 206 means the range was honoured; 200 with a range request means the
    // server ignored it and is sending the whole body, so the partial file has
    // to go or the two halves would be concatenated into garbage.
    bool append = false;
    if (status == 206 && already_have > 0) {
        append = true;
        result.resumed = true;
    } else if (status == 200) {
        already_have = 0;
    } else {
        result.error = "HTTP " + std::to_string(status) + " for " + url;
        if (status == 401 || status == 403) {
            result.error += " (the file may need a Hugging Face login or licence acceptance on the website)";
        }
        return result;
    }

    uint64_t content_length = 0;
    DWORD len_size = sizeof(wchar_t) * 64;
    std::vector<wchar_t> len_buf(64);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            len_buf.data(), &len_size, WINHTTP_NO_HEADER_INDEX)) {
        content_length = std::wcstoull(len_buf.data(), nullptr, 10);
    }
    const uint64_t total = content_length > 0 ? content_length + already_have : 0;

    const std::string dir = path_dirname(dest_path);
    if (!dir.empty() && !dir_exists(dir) && !make_dirs(dir)) {
        result.error = "cannot create directory " + dir;
        return result;
    }

    FILE* out = fopen_utf8(partial, append ? "ab" : "wb");
    if (!out) {
        result.error = "cannot open " + partial + " for writing";
        return result;
    }

    uint64_t received = already_have;
    std::vector<uint8_t> buf(64 * 1024);
    bool cancelled = false;
    std::string io_error;

    if (progress && !progress->on_progress(received, total)) cancelled = true;

    while (!cancelled) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            io_error = last_error("WinHttpQueryDataAvailable");
            break;
        }
        if (available == 0) break;  // body complete

        while (available > 0 && !cancelled) {
            const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buf.size()));
            DWORD got = 0;
            if (!WinHttpReadData(request, buf.data(), want, &got)) {
                io_error = last_error("WinHttpReadData");
                break;
            }
            if (got == 0) { available = 0; break; }
            if (std::fwrite(buf.data(), 1, got, out) != got) {
                io_error = "write error on " + partial + " (disk full?)";
                break;
            }
            received += got;
            available -= got;
            if (progress && !progress->on_progress(received, total)) cancelled = true;
        }
        if (!io_error.empty()) break;
    }

    std::fclose(out);

    if (!io_error.empty()) {
        result.error = io_error;
        return result;
    }
    if (cancelled) {
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    }
    if (total > 0 && received != total) {
        result.error = "truncated download: got " + std::to_string(received) + " of " +
                       std::to_string(total) + " bytes";
        return result;
    }

    const std::string rename_error = move_into_place(partial, dest_path);
    if (!rename_error.empty()) {
        result.error = rename_error;
        return result;
    }

    result.ok = true;
    result.bytes = received;
    return result;
}

#elif defined(HAVE_CURL)

namespace {

struct CurlSink {
    FILE* out = nullptr;
    HttpProgress* progress = nullptr;
    uint64_t received = 0;
    uint64_t total = 0;
    bool cancelled = false;
    std::string io_error;
};

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    CurlSink* sink = static_cast<CurlSink*>(userdata);
    const size_t bytes = size * nmemb;
    if (std::fwrite(ptr, 1, bytes, sink->out) != bytes) {
        sink->io_error = "write error (disk full?)";
        return 0;
    }
    sink->received += bytes;
    if (sink->progress && !sink->progress->on_progress(sink->received, sink->total)) {
        sink->cancelled = true;
        return 0;
    }
    return bytes;
}

} // namespace

bool http_available() { return true; }

HttpResult http_download(const std::string& url, const std::string& dest_path,
                         HttpProgress* progress, bool resume)
{
    HttpResult result;

    const std::string partial = part_path(dest_path);
    uint64_t already_have = 0;
    if (resume && file_exists(partial)) already_have = file_size(partial);

    const std::string dir = path_dirname(dest_path);
    if (!dir.empty() && !dir_exists(dir) && !make_dirs(dir)) {
        result.error = "cannot create directory " + dir;
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    FILE* out = fopen_utf8(partial, already_have > 0 ? "ab" : "wb");
    if (!out) {
        curl_easy_cleanup(curl);
        result.error = "cannot open " + partial + " for writing";
        return result;
    }

    CurlSink sink;
    sink.out = out;
    sink.progress = progress;
    sink.received = already_have;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    if (already_have > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(already_have));
        result.resumed = true;
    }

    const CURLcode code = curl_easy_perform(curl);
    std::fclose(out);
    curl_easy_cleanup(curl);

    if (sink.cancelled) {
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    }
    if (!sink.io_error.empty()) {
        result.error = sink.io_error;
        return result;
    }
    if (code != CURLE_OK) {
        result.error = std::string("download failed: ") + curl_easy_strerror(code);
        return result;
    }

    const std::string rename_error = move_into_place(partial, dest_path);
    if (!rename_error.empty()) {
        result.error = rename_error;
        return result;
    }

    result.ok = true;
    result.bytes = sink.received;
    return result;
}

#else

bool http_available() { return false; }

HttpResult http_download(const std::string&, const std::string&, HttpProgress*, bool)
{
    HttpResult result;
    result.error = "this build has no HTTP support: install libcurl and reconfigure, "
                   "or fetch the file manually and place it in the models directory";
    return result;
}

#endif

} // namespace pastiche
