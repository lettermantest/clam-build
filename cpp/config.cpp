#include "config.h"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace clam {

namespace {

std::string env(const char* name) {
    char buffer[1024];
    DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return std::string();
    return std::string(buffer, length);
}

std::string read_file(const std::string& path) {
    std::string content;
    read_utf8_file(path, content);
    return content;
}

bool write_file(const std::string& path, const std::string& content) {
    return write_utf8_file(path, content);
}

}  // namespace

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

bool path_exists(const std::string& path) {
    return GetFileAttributesW(utf8_to_wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::string& path) {
    DWORD attributes = GetFileAttributesW(utf8_to_wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_directories(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '\\' || path[i] == '/') {
            CreateDirectoryW(utf8_to_wide(current).c_str(), nullptr);
        }
    }
    CreateDirectoryW(utf8_to_wide(path).c_str(), nullptr);
    return is_directory(path);
}

std::string absolute_path(const std::string& path) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetFullPathNameW(utf8_to_wide(path).c_str(), MAX_PATH * 2, buffer, nullptr);
    if (length == 0) return path;
    return wide_to_utf8(std::wstring(buffer, length));
}

std::string current_directory() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetCurrentDirectoryW(MAX_PATH * 2, buffer);
    return wide_to_utf8(std::wstring(buffer, length));
}

std::string config_dir() {
    std::string appdata = env("APPDATA");
    if (appdata.empty()) {
        std::string profile = env("USERPROFILE");
        if (!profile.empty()) appdata = join_path(profile, "AppData\\Roaming");
    }
    if (appdata.empty()) appdata = current_directory();
    return join_path(appdata, "clam");
}

// Settings from the pre-rename build lived in %APPDATA%\\perx. Read them when
// the Clam Build files do not exist yet; the next save writes to the new folder.
std::string legacy_config_dir() {
    const std::string current = config_dir();
    const size_t slash = current.find_last_of("\\/");
    if (slash == std::string::npos) return current + "-legacy";
    return current.substr(0, slash + 1) + "perx";
}

std::string config_input_path(const char* name) {
    const std::string current = join_path(config_dir(), name);
    if (path_exists(current)) return current;
    const std::string legacy = join_path(legacy_config_dir(), name);
    if (path_exists(legacy)) return legacy;
    return current;
}

bool save_api_key_hex(const std::string& key, std::string& error);

bool load_api_key(std::string& out) {
    std::string from_env = env("NVIDIA_API_KEY");
    if (!from_env.empty()) {
        out = from_env;
        return true;
    }
    std::string path = config_input_path("api_key");
    if (!path_exists(path)) return false;
    std::string stored = trim(read_file(path));
    if (stored.empty()) return false;

    // The terminal build stores PowerShell's ConvertFrom-SecureString output, which is
    // the DPAPI blob hex encoded. Raw blobs are also accepted.
    std::vector<unsigned char> bytes;
    bool looks_hex = stored.size() % 2 == 0 && stored.size() >= 32;
    if (looks_hex) {
        for (char c : stored) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                looks_hex = false;
                break;
            }
        }
    }
    if (looks_hex) {
        bytes.reserve(stored.size() / 2);
        for (size_t i = 0; i + 1 < stored.size(); i += 2) {
            bytes.push_back(static_cast<unsigned char>(
                std::stoi(stored.substr(i, 2), nullptr, 16)));
        }
    } else {
        bytes.assign(stored.begin(), stored.end());
    }

    DATA_BLOB input{};
    input.pbData = bytes.data();
    input.cbData = static_cast<DWORD>(bytes.size());
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        std::string decoded(reinterpret_cast<char*>(output.pbData), output.cbData);
        DWORD length = output.cbData;
        LocalFree(output.pbData);

        // PowerShell writes UTF-16LE; normalize to UTF-8.
        bool utf16 = length >= 2 && length % 2 == 0;
        if (utf16) {
            for (DWORD i = 0; i < length; i += 2) {
                if (decoded[i + 1] != 0) {
                    utf16 = false;
                    break;
                }
            }
        }
        if (utf16) {
            decoded = wide_to_utf8(std::wstring(reinterpret_cast<const wchar_t*>(decoded.data()),
                                                length / 2));
        }
        out = trim(decoded);
        // Strip any stray NUL or BOM characters a shell may have wrapped around it.
        while (!out.empty() && (out.front() == '\0' || static_cast<unsigned char>(out.front()) == 0xEF)) {
            out.erase(out.begin());
        }
        while (!out.empty() && (out.back() == '\0' || static_cast<unsigned char>(out.back()) == 0xFE ||
                                static_cast<unsigned char>(out.back()) == 0xFF)) {
            out.pop_back();
        }
        return !out.empty();
    }
    // Last resort: a previously saved plaintext key.
    if (!looks_hex) {
        out = stored;
        return true;
    }
    return false;
}

bool save_api_key_hex(const std::string& key, std::string& error) {
    make_directories(config_dir());
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(key.data()));
    input.cbData = static_cast<DWORD>(key.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Clam Build API key", nullptr, nullptr, nullptr, 0, &output)) {
        error = "Windows DPAPI could not encrypt the key.";
        return false;
    }
    std::string hex;
    hex.reserve(static_cast<size_t>(output.cbData) * 2);
    static const char* digits = "0123456789ABCDEF";
    for (DWORD i = 0; i < output.cbData; ++i) {
        unsigned char c = output.pbData[i];
        hex.push_back(digits[c >> 4]);
        hex.push_back(digits[c & 0x0F]);
    }
    LocalFree(output.pbData);
    std::string path = join_path(config_dir(), "api_key");
    if (!write_file(path, hex)) {
        error = "Could not write " + path;
        return false;
    }
    return true;
}

bool save_api_key(const std::string& key, std::string& error) {
    if (key.empty()) {
        error = "API key cannot be empty.";
        return false;
    }
    return save_api_key_hex(key, error);
}

Settings load_settings() {
    Settings settings;
    std::string path = config_input_path("settings.json");
    std::string raw = read_file(path);
    if (raw.empty()) return settings;
    auto parsed = Json::parse(raw);
    if (!parsed) return settings;
    const Json& json = *parsed;
    if (json.has("model")) settings.model = json["model"].string_or(settings.model);
    if (json.has("effort")) settings.effort = json["effort"].string_or(settings.effort);
    if (json.has("allow_all")) settings.allow_all = json["allow_all"].as_bool(true);
    if (json.has("project")) settings.project = json["project"].string_or("");
    if (json.has("theme")) settings.theme = json["theme"].string_or("black");
    if (std::find(kEffortLevels.begin(), kEffortLevels.end(), settings.effort) == kEffortLevels.end()) {
        settings.effort = "medium";
    }
    if (settings.model.empty()) settings.model = kDefaultModel;
    return settings;
}

bool save_settings(const Settings& settings) {
    make_directories(config_dir());
    Json json = Json::make_object();
    json.set("model", Json(settings.model));
    json.set("effort", Json(settings.effort));
    json.set("allow_all", Json(settings.allow_all));
    json.set("project", Json(settings.project));
    json.set("theme", Json(settings.theme));
    return write_file(join_path(config_dir(), "settings.json"), json.dump(2));
}

int effort_tokens(const std::string& effort) {
    if (effort == "low") return 2048;
    if (effort == "high") return 8192;
    if (effort == "max") return 16384;
    return 4096;
}

std::string effort_reasoning(const std::string& effort) {
    if (effort == "low") return "low";
    if (effort == "medium") return "medium";
    return "high";
}

std::string next_effort(const std::string& effort) {
    auto it = std::find(kEffortLevels.begin(), kEffortLevels.end(), effort);
    if (it == kEffortLevels.end()) return "medium";
    size_t index = static_cast<size_t>(std::distance(kEffortLevels.begin(), it));
    return kEffortLevels[(index + 1) % kEffortLevels.size()];
}

bool is_code_model(const std::string& model) {
    std::string lower = to_lower(model);
    static const char* excluded[] = {
        "embed", "rerank", "retriever", "reward", "classifier", "detector", "diffusion",
        "stable-diffusion", "sdxl", "flux", "text-to-image", "text-to-video", "image-to-image",
        "upscale", "parakeet", "canary", "riva", "speech", "audio", "asr", "tts", "nvclip",
        "guard", "safety", "palmyra-med", "palmyra-fin", "riva-translate", "nemotron-parse",
        "deplot", "studiovoice", "magpie-tts", "synthetic-video",
    };
    for (const char* needle : excluded) {
        if (lower.find(needle) != std::string::npos) return false;
    }
    return true;
}

bool model_supports_vision(const std::string& model) {
    std::string lower = to_lower(model);
    static const char* markers[] = {
        "vision", "-vl", "-vlm", "vila", "llava", "fuyu", "paligemma", "kosmos-2", "neva",
        "molmo", "internvl", "minicpm-v", "smolvlm", "llama-4", "gemma-3", "mistral-small-3",
        "qwen2.5-vl", "qwen2-vl", "phi-3-vision", "phi-3.5-vision", "phi-4-multimodal", "omni",
    };
    for (const char* marker : markers) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

std::string plugins_dir() {
    const std::string current = join_path(config_dir(), "plugins");
    if (is_directory(current)) return current;
    const std::string legacy = join_path(legacy_config_dir(), "plugins");
    if (is_directory(legacy)) return legacy;
    return current;
}

std::vector<Plugin> load_plugins() {
    std::vector<Plugin> plugins;
    std::string dir = plugins_dir();
    if (!is_directory(dir)) return plugins;
    WIN32_FIND_DATAW data{};
    std::wstring pattern = utf8_to_wide(join_path(dir, "*.luau"));
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return plugins;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Plugin plugin;
        plugin.path = join_path(dir, wide_to_utf8(data.cFileName));
        std::string file_name = wide_to_utf8(data.cFileName);
        plugin.slug = file_name.substr(0, file_name.find_last_of('.'));
        plugin.script = read_file(plugin.path);
        plugin.name = plugin.slug;
        plugin.description = "Clam Build plugin";
        for (const std::string& line : [&] {
                 std::vector<std::string> lines;
                 std::istringstream stream(plugin.script);
                 std::string buffer;
                 while (std::getline(stream, buffer)) lines.push_back(buffer);
                 return lines;
             }()) {
            std::string trimmed = trim(line);
            if (trimmed.rfind("name", 0) == 0 || trimmed.rfind("local name", 0) == 0) {
                size_t quote = trimmed.find('"');
                if (quote != std::string::npos) {
                    size_t end = trimmed.find('"', quote + 1);
                    if (end != std::string::npos) plugin.name = trimmed.substr(quote + 1, end - quote - 1);
                }
            } else if (trimmed.rfind("description", 0) == 0 || trimmed.rfind("local description", 0) == 0) {
                size_t quote = trimmed.find('"');
                if (quote != std::string::npos) {
                    size_t end = trimmed.find('"', quote + 1);
                    if (end != std::string::npos) plugin.description = trimmed.substr(quote + 1, end - quote - 1);
                }
            }
        }
        plugins.push_back(plugin);
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    std::sort(plugins.begin(), plugins.end(),
              [](const Plugin& a, const Plugin& b) { return a.slug < b.slug; });
    return plugins;
}

std::string base64_encode(const unsigned char* data, size_t length) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((length + 2) / 3 * 4);
    for (size_t i = 0; i < length; i += 3) {
        unsigned int chunk = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < length) chunk |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < length) chunk |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(table[(chunk >> 18) & 63]);
        out.push_back(table[(chunk >> 12) & 63]);
        out.push_back(i + 1 < length ? table[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < length ? table[chunk & 63] : '=');
    }
    return out;
}

bool image_to_data_url(const std::string& path, std::string& out_url, std::string& error) {
    std::string bytes;
    if (!read_utf8_file(path, bytes)) {
        error = "Cannot open " + path;
        return false;
    }
    if (bytes.empty()) {
        error = "Empty image: " + path;
        return false;
    }
    if (bytes.size() > 8 * 1024 * 1024) {
        error = "Image larger than 8 MB: " + path;
        return false;
    }
    std::string ext = to_lower(path.substr(path.find_last_of('.') + 1));
    std::string mime = "image/png";
    if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
    else if (ext == "gif") mime = "image/gif";
    else if (ext == "webp") mime = "image/webp";
    else if (ext == "bmp") mime = "image/bmp";
    out_url = "data:" + mime + ";base64," +
              base64_encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return true;
}

}  // namespace clam
