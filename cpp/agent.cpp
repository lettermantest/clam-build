#include "agent.h"

#include <windows.h>

#include "config.h"
#include "http.h"
#include "resources_data.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

namespace clam {

namespace {

constexpr const char* kApiBase = "https://integrate.api.nvidia.com/v1";
constexpr int kMaxSteps = 12;
constexpr size_t kReadLimit = 30000;

std::string read_text_file(const std::string& path, size_t limit, bool& truncated) {
    std::string out;
    if (!read_utf8_file(path, out, limit)) return std::string();
    truncated = out.size() > limit;
    if (truncated) out.resize(limit);
    return out;
}

std::string resolve_path(const std::string& root, const std::string& raw) {
    if (raw.empty()) return root;
    if (raw.size() > 1 && raw[1] == ':') return raw;  // absolute drive path
    if (raw[0] == '\\' || raw[0] == '/') return raw;
    if (raw.rfind("~/", 0) == 0) {
        const char* home = std::getenv("USERPROFILE");
        if (home) return std::string(home) + "\\" + raw.substr(2);
    }
    return join_path(root, raw);
}

void list_dir(const std::string& root, const std::string& dir, int depth, std::vector<std::string>& out) {
    if (depth <= 0 || out.size() >= 400) return;
    std::wstring pattern = utf8_to_wide(join_path(dir, "*"));
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::wstring, bool>> entries;
    do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.emplace_back(name, is_dir);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    std::sort(entries.begin(), entries.end());

    static const char* skip[] = {".git", "target", "node_modules", "vendor", ".venv", "venv", "__pycache__", ".clam"};
    for (const auto& [name, is_dir] : entries) {
        if (out.size() >= 400) return;
        std::string narrow = wide_to_utf8(name);
        if (!narrow.empty() && narrow[0] == '.') continue;
        if (is_dir) {
            bool skipped = false;
            for (const char* item : skip) {
                if (narrow == item) {
                    skipped = true;
                    break;
                }
            }
            if (skipped) continue;
        }
        std::string full = join_path(dir, narrow);
        std::string relative = full;
        if (full.rfind(root, 0) == 0) {
            relative = full.substr(root.size());
            while (!relative.empty() && (relative[0] == '\\' || relative[0] == '/')) relative.erase(0, 1);
        }
        if (is_dir) {
            out.push_back(relative + "/");
            list_dir(root, full, depth - 1, out);
        } else {
            out.push_back(relative);
        }
    }
}

void search_dir(const std::string& root, const std::string& dir, const std::string& needle,
                std::vector<std::string>& out) {
    if (out.size() >= 80) return;
    std::wstring pattern = utf8_to_wide(join_path(dir, "*"));
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (out.size() >= 80) break;
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::string narrow = wide_to_utf8(name);
        if (!narrow.empty() && narrow[0] == '.') continue;
        if (narrow == "target" || narrow == "node_modules" || narrow == "vendor") continue;
        std::string full = join_path(dir, narrow);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            search_dir(root, full, needle, out);
            continue;
        }
        if (data.nFileSizeHigh != 0 || data.nFileSizeLow > 2u * 1024u * 1024u) continue;
        bool truncated = false;
        std::string content = read_text_file(full, 2 * 1024 * 1024, truncated);
        if (content.empty()) continue;
        std::istringstream stream(content);
        std::string line;
        int line_number = 0;
        std::string relative = full;
        if (full.rfind(root, 0) == 0) {
            relative = full.substr(root.size());
            while (!relative.empty() && (relative[0] == '\\' || relative[0] == '/')) relative.erase(0, 1);
        }
        while (std::getline(stream, line)) {
            ++line_number;
            if (line.find(needle) != std::string::npos) {
                out.push_back(relative + ":" + std::to_string(line_number) + ": " + trim(line));
                if (out.size() >= 80) break;
            }
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

std::string run_shell(const std::string& root, const std::string& command) {
    std::string command_line = "cmd.exe /C " + command;
    FILE* pipe = _popen(command_line.c_str(), "r");
    if (!pipe) return "Error: could not start shell.";
    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
        if (output.size() > 200000) break;
    }
    int code = _pclose(pipe);
    if (output.size() > 8000) output = output.substr(0, 8000) + "\n[Output truncated]";
    (void)root;
    return "Exit code: " + std::to_string(code) + "\n" + output;
}

std::string web_search(const std::string& query, int limit) {
    std::string url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);
    HttpResult result = http_get(url, 20);
    if (!result.ok) {
        return "Error: web search failed (" + result.error +
               "). Set TAVILY_API_KEY or BRAVE_SEARCH_API_KEY for reliability.";
    }
    std::vector<std::string> found;
    size_t position = 0;
    while (found.size() < static_cast<size_t>(limit)) {
        size_t marker = result.body.find("result__a", position);
        if (marker == std::string::npos) break;
        size_t href = result.body.find("href=\"", marker);
        if (href == std::string::npos) break;
        size_t href_end = result.body.find('"', href + 6);
        if (href_end == std::string::npos) break;
        std::string link = result.body.substr(href + 6, href_end - href - 6);
        size_t title_start = result.body.find('>', marker);
        size_t title_end = result.body.find('<', title_start + 1);
        std::string title = title_start != std::string::npos && title_end != std::string::npos
                                ? result.body.substr(title_start + 1, title_end - title_start - 1)
                                : link;
        if (link.rfind("http", 0) == 0) {
            found.push_back("- " + title + "\n  " + link);
        }
        position = href_end;
    }
    if (found.empty()) return "No web results found for: " + query;
    std::string out = "Web results for: " + query + "\n\n";
    for (const auto& item : found) out += item + "\n";
    return out;
}

std::string fetch_url(const std::string& url) {
    HttpResult result = http_get(url, 25);
    if (!result.ok) return "Error fetching " + url + ": " + result.error;
    std::string text;
    bool inside = false;
    for (char c : result.body) {
        if (c == '<') inside = true;
        else if (c == '>') inside = false;
        else if (!inside) text.push_back(c);
    }
    std::string collapsed;
    bool space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
        } else {
            if (space && !collapsed.empty()) collapsed.push_back(' ');
            space = false;
            collapsed.push_back(c);
        }
    }
    if (collapsed.size() > 12000) collapsed = collapsed.substr(0, 12000) + "\n[Truncated]";
    return "Fetched " + url + "\n\n" + collapsed;
}

std::string run_capture(const std::string& command) {
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return "";
    std::string output;
    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    _pclose(pipe);
    return output;
}

std::string system_info() {
    std::string out;
    out += run_capture(
        "powershell.exe -NoProfile -Command \"$os=Get-CimInstance Win32_OperatingSystem; "
        "\\\"OS: $($os.Caption) $($os.Version)\\\"; \\\"$([math]::Round($os.TotalVisibleMemorySize/1MB,1)) GB RAM\\\"\"");
    out += run_capture(
        "powershell.exe -NoProfile -Command \"(Get-CimInstance Win32_Processor).Name\"");
    out += run_capture(
        "powershell.exe -NoProfile -Command \"(Get-CimInstance Win32_VideoController).Name\"");
    return trim(out);
}

std::string find_browser() {
    const char* candidates[] = {
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const char* candidate : candidates) {
        if (path_exists(candidate)) return candidate;
    }
    return std::string();
}

}  // namespace

std::string tools_json() { return kToolsJson; }
std::string system_prompt() { return kSystemPrompt; }

std::vector<std::string> fetch_models(const std::string& api_key, std::string& error) {
    std::vector<std::string> models;
    HttpResult result = http_request("GET", std::string(kApiBase) + "/models", std::string(),
                                     "Authorization: Bearer " + api_key + "\r\n", 25);
    if (!result.ok) {
        error = result.error;
        return models;
    }
    auto parsed = Json::parse(result.body, &error);
    if (!parsed) return models;
    const Json& data = (*parsed)["data"];
    for (size_t i = 0; i < data.array().size(); ++i) {
        const Json& entry = data.at(i);
        std::string id = entry["id"].string_or("");
        if (!id.empty() && is_code_model(id)) models.push_back(id);
    }
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    return models;
}

ToolResult execute_tool(const std::string& name,
                        const Json& arguments,
                        const std::string& project_root,
                        const std::string& api_key) {
    ToolResult result;
    (void)api_key;

    if (name == "list_files") {
        std::string dir = resolve_path(project_root,
                                       arguments["path"].string_or("."));
        int depth = arguments["depth"].as_int(2);
        if (depth < 1) depth = 1;
        if (depth > 4) depth = 4;
        std::vector<std::string> lines;
        list_dir(project_root, dir, depth, lines);
        if (lines.empty()) {
            result.output = "(empty directory)";
        } else {
            for (const auto& line : lines) result.output += line + "\n";
        }
        return result;
    }

    if (name == "read_file" || name == "view_image") {
        std::string path = resolve_path(project_root, arguments["path"].string_or(""));
        std::string ext = to_lower(path.substr(path.find_last_of('.') + 1));
        bool image = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
                     ext == "webp" || ext == "bmp";
        if (!path_exists(path)) {
            result.output = "Error: file not found: " + path;
            return result;
        }
        if (image) {
            result.output = "Image: " + path;
            result.images.push_back(path);
            return result;
        }
        bool truncated = false;
        std::string content = read_text_file(path, kReadLimit, truncated);
        if (content.empty() && !truncated) {
            result.output = "Error: could not read " + path;
            return result;
        }
        result.output = content;
        if (truncated) result.output += "\n[File truncated at 30,000 bytes]";
        return result;
    }

    if (name == "write_file") {
        std::string path = resolve_path(project_root, arguments["path"].string_or(""));
        std::string content = arguments["content"].string_or("");
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) make_directories(path.substr(0, slash));
        if (!write_utf8_file(path, content)) {
            result.output = "Error: could not write " + path;
            return result;
        }
        result.output = "Wrote " + path + " (" + format_bytes(content.size()) + ")";
        return result;
    }

    if (name == "search_text") {
        std::string query = arguments["query"].string_or("");
        std::string dir = resolve_path(project_root, arguments["path"].string_or("."));
        std::vector<std::string> hits;
        search_dir(project_root, dir, query, hits);
        if (hits.empty()) {
            result.output = "No matches found.";
        } else {
            for (const auto& hit : hits) result.output += hit + "\n";
        }
        return result;
    }

    if (name == "run_command") {
        result.output = run_shell(project_root, arguments["command"].string_or(""));
        return result;
    }

    if (name == "web_search") {
        result.output = web_search(arguments["query"].string_or(""),
                                   arguments["max_results"].as_int(5));
        return result;
    }

    if (name == "fetch_url") {
        result.output = fetch_url(arguments["url"].string_or(""));
        return result;
    }

    if (name == "system_info") {
        result.output = system_info();
        return result;
    }

    if (name == "visual_test") {
        std::string target = arguments["url"].string_or("");
        std::string url = target;
        if (url.rfind("http", 0) != 0) url = "file:///" + resolve_path(project_root, target);
        std::replace(url.begin(), url.end(), '\\', '/');
        std::string shot_dir = join_path(project_root, ".clam\\screenshots");
        make_directories(shot_dir);
        std::string shot = join_path(shot_dir, "shot.png");
        std::string browser = find_browser();
        if (browser.empty()) {
            result.output = "Error: Chrome or Edge not found for visual_test.";
            return result;
        }
        std::string command = "\"" + browser + "\" --headless=new --disable-gpu --hide-scrollbars "
                              "--window-size=1280,800 --screenshot=\"" + shot + "\" \"" + url + "\"";
        system(command.c_str());
        if (path_exists(shot)) {
            result.output = "Screenshot saved: " + shot;
            result.images.push_back(shot);
        } else {
            result.output = "Error: screenshot failed.";
        }
        return result;
    }

    if (name == "arduino_boards") {
        result.output = run_capture("arduino-cli board list 2>&1");
        if (trim(result.output).empty()) {
            result.output = "arduino-cli not installed. Use arduino_setup or install it from arduino.github.io/arduino-cli.";
        }
        return result;
    }

    if (name == "arduino_upload") {
        std::string sketch = resolve_path(project_root, arguments["sketch_path"].string_or(""));
        std::string fqbn = arguments["fqbn"].string_or("");
        std::string port = arguments["port"].string_or("");
        bool compile_only = arguments["compile_only"].as_bool(false);
        std::string command = "arduino-cli ";
        if (compile_only || port.empty()) {
            command += "compile --fqbn " + fqbn + " \"" + sketch + "\"";
        } else {
            command += "upload -p " + port + " --fqbn " + fqbn + " \"" + sketch + "\"";
        }
        result.output = run_capture((command + " 2>&1").c_str());
        return result;
    }

    if (name == "arduino_setup") {
        result.output = "Open Settings in Clam Build to install Arduino CLI and board cores.";
        return result;
    }

    if (name == "serial_read") {
        result.output = "Serial reading is available from the terminal build of Clam.";
        return result;
    }

    result.output = "Unknown tool: " + name;
    return result;
}

namespace {

Json message_to_json(const Message& message) {
    Json object = Json::make_object();
    switch (message.role) {
        case Role::User: {
            object.set("role", Json("user"));
            if (message.images.empty()) {
                object.set("content", Json(message.text));
            } else {
                Json parts = Json::make_array();
                Json text_part = Json::make_object();
                text_part.set("type", Json("text"));
                text_part.set("text", Json(message.text));
                parts.push_back(text_part);
                for (const auto& image : message.images) {
                    std::string url;
                    std::string error;
                    if (!image_to_data_url(image, url, error)) continue;
                    Json image_part = Json::make_object();
                    image_part.set("type", Json("image_url"));
                    Json inner = Json::make_object();
                    inner.set("url", Json(url));
                    image_part.set("image_url", inner);
                    parts.push_back(image_part);
                }
                object.set("content", parts);
            }
            break;
        }
        case Role::Assistant: {
            object.set("role", Json("assistant"));
            object.set("content", Json(message.text));
            if (!message.calls.empty()) {
                Json calls = Json::make_array();
                for (const auto& call : message.calls) {
                    Json entry = Json::make_object();
                    entry.set("id", Json(call.id));
                    entry.set("type", Json("function"));
                    Json function = Json::make_object();
                    function.set("name", Json(call.name));
                    function.set("arguments", Json(call.arguments));
                    entry.set("function", function);
                    calls.push_back(entry);
                }
                object.set("tool_calls", calls);
            }
            break;
        }
        case Role::Tool: {
            object.set("role", Json("tool"));
            object.set("tool_call_id", Json(message.tool_call_id));
            object.set("name", Json(message.tool_name));
            object.set("content", Json(message.text));
            break;
        }
    }
    return object;
}

struct ModelReply {
    bool ok = false;
    std::string error;
    std::string content;
    std::vector<ToolCall> calls;
    std::string reasoning;
};

bool has_leaked_tool_markup(const std::string& text) {
    return text.find("<tool_call") != std::string::npos ||
           text.find("</tool_call>") != std::string::npos;
}

// Some models occasionally print their tool call as text instead of using the
// tool API. Drop that block before it can appear in the transcript.
std::string strip_leaked_tool_markup(const std::string& text) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t begin = text.find("<tool_call", pos);
        if (begin == std::string::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, begin - pos));
        const size_t end = text.find("</tool_call>", begin);
        if (end == std::string::npos) {
            pos = text.size();
        } else {
            pos = end + std::string("</tool_call>").size();
        }
    }
    return trim(out);
}

// Drops empty assistant turns. Models refuse to answer when a prior assistant
// message has no content, which is what produced the "(empty response)" bug.
std::vector<Message> sanitize_history(const std::vector<Message>& history) {
    std::vector<Message> cleaned;
    cleaned.reserve(history.size());
    for (const auto& message : history) {
        if (message.role == Role::Assistant && trim(message.text).empty() && message.calls.empty()) {
            continue;
        }
        if (message.role == Role::Tool && trim(message.text).empty()) {
            Message copy = message;
            copy.text = "(tool returned no output)";
            cleaned.push_back(copy);
            continue;
        }
        cleaned.push_back(message);
    }
    return cleaned;
}

ModelReply call_model(const std::string& api_key,
                      const std::string& model,
                      const std::string& effort,
                      const std::vector<Message>& history,
                      bool add_nudge,
                      int max_tokens,
                      const std::string& reasoning_effort,
                      bool include_tools = true) {
    ModelReply reply;
    Json messages = Json::make_array();
    Json system = Json::make_object();
    system.set("role", Json("system"));
    system.set("content", Json(system_prompt()));
    messages.push_back(system);
    for (const auto& message : sanitize_history(history)) {
        messages.push_back(message_to_json(message));
    }
    if (add_nudge) {
        Json nudge = Json::make_object();
        nudge.set("role", Json("user"));
        nudge.set("content", Json("Your previous reply was empty. Respond now with the final answer in normal text."));
        messages.push_back(nudge);
    }

    Json body = Json::make_object();
    body.set("model", Json(model));
    body.set("messages", messages);
    if (include_tools) {
        auto tools = Json::parse(tools_json());
        if (tools) body.set("tools", *tools);
        body.set("tool_choice", Json("auto"));
        body.set("parallel_tool_calls", Json(false));
    }
    body.set("temperature", Json(0.2));
    body.set("max_tokens", Json(max_tokens > 0 ? max_tokens : effort_tokens(effort)));
    body.set("reasoning_effort", Json(reasoning_effort.empty() ? effort_reasoning(effort) : reasoning_effort));

    HttpResult result;
    // Transient socket/DNS blips should not surface as an empty answer.
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = http_post_json(std::string(kApiBase) + "/chat/completions", body.dump(),
                                "Authorization: Bearer " + api_key);
        if (result.ok) break;
        const bool retryable = result.status == 0 || result.status == 429 || result.status >= 500;
        if (!retryable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(600 * (attempt + 1)));
    }
    if (!result.ok) {
        reply.error = result.error;
        return reply;
    }
    std::string parse_error;
    auto parsed = Json::parse(result.body, &parse_error);
    if (!parsed) {
        reply.error = "Invalid API response: " + parse_error;
        return reply;
    }
    const Json& choices = (*parsed)["choices"];
    if (choices.array().empty()) {
        reply.error = "NVIDIA returned no choices.";
        return reply;
    }
    const Json& message = choices.at(0)["message"];
    reply.content = trim(message["content"].string_or(""));
    reply.reasoning = message["reasoning_content"].string_or("");
    const Json& calls = message["tool_calls"];
    for (size_t i = 0; i < calls.array().size(); ++i) {
        const Json& call = calls.at(i);
        ToolCall parsed_call;
        parsed_call.id = call["id"].string_or("call-" + std::to_string(i));
        parsed_call.name = call["function"]["name"].string_or("");
        parsed_call.arguments = call["function"]["arguments"].string_or("{}");
        if (parsed_call.arguments.empty()) parsed_call.arguments = "{}";
        reply.calls.push_back(parsed_call);
    }
    reply.ok = true;
    return reply;
}

}  // namespace

void run_turn(const std::string& api_key,
              const std::string& model,
              const std::string& effort,
              const std::string& project_root,
              std::vector<Message> history,
              const std::atomic<bool>& cancel,
              const AgentCallbacks& callbacks,
              std::vector<Message>* final_history) {
    auto status = [&](const std::string& text) {
        if (callbacks.on_status) callbacks.on_status(text);
    };
    auto fail = [&](const std::string& error) {
        if (callbacks.on_error) callbacks.on_error(error);
    };

    for (int step = 0; step < kMaxSteps; ++step) {
        if (cancel.load()) break;
        status("thinking…");
        ModelReply reply = call_model(api_key, model, effort, history, false, 0, std::string());
        if (reply.ok && !trim(reply.reasoning).empty() && callbacks.on_thinking) {
            callbacks.on_thinking(trim(reply.reasoning));
        }
        if (!reply.ok) {
            fail(reply.error);
            break;
        }

        if (reply.calls.empty() && has_leaked_tool_markup(reply.content)) {
            status("retrying malformed tool call…");
            std::vector<Message> corrective = history;
            Message nudge;
            nudge.role = Role::User;
            nudge.text = "Do not print tool-call XML, tool markup, or full source files. "
                         "Use the tools through the API to do the work, then reply with a "
                         "concise summary. Only show code when the user explicitly asks for it.";
            corrective.push_back(nudge);
            ModelReply retry = call_model(api_key, model, effort, corrective, false,
                                          std::max(effort_tokens(effort), 8192), "low");
            if (retry.ok) {
                reply = std::move(retry);
            }
            if (reply.calls.empty()) {
                reply.content = strip_leaked_tool_markup(reply.content);
            }
        }

        if (reply.calls.empty()) {
            std::string answer = trim(reply.content);
            if (answer.empty()) {
                status("retrying empty reply\u2026");
                ModelReply retry = call_model(api_key, model, effort, history, true,
                                              std::max(effort_tokens(effort), 8192), "low");
                if (retry.ok) answer = trim(retry.content);
            }
            if (answer.empty()) {
                status("retrying without tools\u2026");
                ModelReply retry = call_model(api_key, model, effort, history, true, 4096, "low",
                                              false);
                if (retry.ok) answer = trim(retry.content);
            }
            if (answer.empty()) {
                status("retrying with more tokens\u2026");
                ModelReply retry = call_model(api_key, model, effort, history, true,
                                              std::max(effort_tokens(effort), 8192) * 2, "medium",
                                              false);
                if (retry.ok) answer = trim(retry.content);
            }
            if (answer.empty()) {
                // Fall back to whatever reasoning text the model produced so the user
                // is never left with a silent failure.
                if (!trim(reply.reasoning).empty()) {
                    answer = "(the model only produced reasoning)\n\n" + trim(reply.reasoning);
                }
            }
            if (answer.empty()) {
                fail("The model returned no text. Try Effort: low, or pick another model.");
            } else {
                Message final_message;
                final_message.role = Role::Assistant;
                final_message.text = answer;
                history.push_back(final_message);
                if (callbacks.on_answer) callbacks.on_answer(answer);
            }
            if (final_history) *final_history = history;
            if (callbacks.on_done) callbacks.on_done();
            return;
        }

        Message assistant;
        assistant.role = Role::Assistant;
        assistant.text = reply.content;
        assistant.calls = reply.calls;
        history.push_back(assistant);

        for (const auto& call : reply.calls) {
            if (cancel.load()) break;
            status("running " + call.name + "…");
            std::string parse_error;
            auto arguments = Json::parse(call.arguments, &parse_error);
            Json args = arguments ? *arguments : Json::make_object();
            ToolResult result = execute_tool(call.name, args, project_root, api_key);
            if (callbacks.on_tool) callbacks.on_tool(call.name, result);

            Message tool;
            tool.role = Role::Tool;
            tool.tool_call_id = call.id;
            tool.tool_name = call.name;
            tool.text = result.output;
            history.push_back(tool);
        }
    }

    if (final_history) *final_history = history;
    if (callbacks.on_done) callbacks.on_done();
}

}  // namespace clam
