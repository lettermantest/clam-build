// Settings, API key storage, plugins, and model helpers.
#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace clam {

constexpr const char* kDefaultModel = "nvidia/nemotron-3.5-lightning-30b-a3b";
inline const std::vector<std::string> kEffortLevels = {"low", "medium", "high", "max"};
constexpr int kMaxImagesPerMessage = 10;

struct Settings {
    std::string model = kDefaultModel;
    std::string effort = "medium";
    bool allow_all = true;
    std::string project;
    std::string theme = "black";
};

struct Plugin {
    std::string slug;
    std::string name;
    std::string description;
    std::string script;
    std::string path;
    bool enabled = false;
};

std::string config_dir();
std::string join_path(const std::string& a, const std::string& b);
bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
bool make_directories(const std::string& path);
std::string absolute_path(const std::string& path);
std::string current_directory();

// API key: stored with DPAPI on Windows.
bool load_api_key(std::string& out);
bool save_api_key(const std::string& key, std::string& error);

Settings load_settings();
bool save_settings(const Settings& settings);

int effort_tokens(const std::string& effort);
std::string effort_reasoning(const std::string& effort);
std::string next_effort(const std::string& effort);

bool is_code_model(const std::string& model);
bool model_supports_vision(const std::string& model);

std::string plugins_dir();
std::vector<Plugin> load_plugins();

// Data URL for an image file, used when attaching pictures to a request.
bool image_to_data_url(const std::string& path, std::string& out_url, std::string& error);

std::string base64_encode(const unsigned char* data, size_t length);

}  // namespace clam
