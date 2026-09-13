#pragma once

// Minimal HTTPS download client for fetching model weights (D19).
//
// Windows uses WinHTTP, which ships with the system, so the downloader adds no
// dependency to the release. Linux uses libcurl when it was found at configure
// time; without it the call fails with a clear message rather than silently
// doing nothing.
#include <cstdint>
#include <string>

namespace pastiche {

// Reports download progress and doubles as the cancellation channel.
class HttpProgress {
public:
    virtual ~HttpProgress() = default;

    // `total` is 0 when the server does not say how large the body is.
    // Returning false aborts the transfer; the partial file is kept so the
    // next attempt can resume from it.
    virtual bool on_progress(uint64_t received, uint64_t total) = 0;
};

struct HttpResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    uint64_t bytes = 0;     // size of the completed file
    bool resumed = false;   // whether an existing partial file was continued
};

// Downloads `url` into `dest_path`.
//
// The body goes to `dest_path + ".part"` and is renamed only once the transfer
// completes, so an interrupted download can never look like a finished one. If
// a .part file is already there and `resume` is set, the request asks for the
// remaining byte range; servers that ignore it and send the whole body again
// are handled by restarting the file.
//
// `progress` may be null. Redirects are followed, which Hugging Face needs
// since it hands out CDN URLs.
HttpResult http_download(const std::string& url, const std::string& dest_path,
                         HttpProgress* progress, bool resume = true);

// True when this build can actually download anything.
bool http_available();

} // namespace pastiche
