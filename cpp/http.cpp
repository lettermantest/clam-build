#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

#include "json.h"

namespace clam {

namespace {

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

bool parse_url(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port,
               bool& secure) {
    std::wstring wide = utf8_to_wide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) {
        return false;
    }
    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    port = components.nPort;
    secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::string query_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length && buffer) {
        text = wide_to_utf8(std::wstring(buffer, length));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    LocalFree(buffer);
    if (text.empty()) text = "WinHTTP error " + std::to_string(code);
    return text;
}

std::string read_body(HINTERNET request) {
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) break;
        body.append(buffer.data(), read);
        if (body.size() > 8 * 1024 * 1024) break;  // safety cap
    }
    return body;
}

}  // namespace

HttpResult http_request(const std::string& method,
                        const std::string& url,
                        const std::string& body,
                        const std::string& extra_headers,
                        int timeout_seconds) {
    HttpResult result;

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = true;
    if (!parse_url(url, host, path, port, secure)) {
        result.error = "Invalid URL: " + url;
        return result;
    }

    InternetHandle session;
    session.handle = WinHttpOpen(L"ClamBuild/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        result.error = query_error_text(GetLastError());
        return result;
    }
    WinHttpSetTimeouts(session.handle, timeout_seconds * 1000, timeout_seconds * 1000,
                       timeout_seconds * 1000, timeout_seconds * 1000);

    InternetHandle connection;
    connection.handle = WinHttpConnect(session.handle, host.c_str(), port, 0);
    if (!connection.handle) {
        result.error = query_error_text(GetLastError());
        return result;
    }

    const std::wstring wide_method = utf8_to_wide(method);
    InternetHandle request;
    request.handle = WinHttpOpenRequest(connection.handle, wide_method.c_str(), path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request.handle) {
        result.error = query_error_text(GetLastError());
        return result;
    }

    std::wstring headers = utf8_to_wide(extra_headers);
    BOOL sent = WinHttpSendRequest(request.handle,
                                   headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : static_cast<DWORD>(-1),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        DWORD code = GetLastError();
        // Some servers reject chunked-style bodies; retry with a plain byte count header.
        if (code == ERROR_INVALID_PARAMETER || code == ERROR_WINHTTP_INVALID_SERVER_RESPONSE) {
            sent = WinHttpSendRequest(request.handle,
                                      headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                      headers.empty() ? 0 : static_cast<DWORD>(-1),
                                      body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                      static_cast<DWORD>(body.size()),
                                      static_cast<DWORD>(body.size()), 0);
        }
        if (!sent) {
            result.error = query_error_text(GetLastError());
            return result;
        }
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        result.error = query_error_text(GetLastError());
        return result;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

    result.status = static_cast<int>(status);
    result.body = read_body(request.handle);
    result.ok = status >= 200 && status < 300;
    if (!result.ok) {
        // Surface the API's JSON error message when present.
        std::string detail;
        if (auto parsed = Json::parse(result.body)) {
            const Json& root = *parsed;
            if (root.has("error")) {
                const Json& error = root["error"];
                detail = error.is_string() ? error.as_string() : error["message"].string_or("");
            }
            if (detail.empty() && root.has("detail")) detail = root["detail"].string_or("");
            if (detail.empty() && root.has("message")) detail = root["message"].string_or("");
        }
        result.error = "HTTP " + std::to_string(status);
        if (!detail.empty()) result.error += ": " + detail;
    }
    return result;
}

}  // namespace clam
