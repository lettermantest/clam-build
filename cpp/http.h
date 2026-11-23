// WinHTTP-based HTTPS client. Uses only APIs shipped with Windows.
#pragma once

#include <string>

namespace clam {

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

// Performs a blocking request. `extra_headers` are sent as-is (CRLF separated).
HttpResult http_request(const std::string& method,
                        const std::string& url,
                        const std::string& body,
                        const std::string& extra_headers,
                        int timeout_seconds = 180);

inline HttpResult http_get(const std::string& url, int timeout_seconds = 30) {
    return http_request("GET", url, std::string(), std::string(), timeout_seconds);
}

inline HttpResult http_post_json(const std::string& url,
                                 const std::string& json,
                                 const std::string& authorization,
                                 int timeout_seconds = 180) {
    std::string headers = "Content-Type: application/json\r\n";
    if (!authorization.empty()) headers += authorization + "\r\n";
    return http_request("POST", url, json, headers, timeout_seconds);
}

}  // namespace clam
