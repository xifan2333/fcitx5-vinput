#include "common/config_path.h"

#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

#include "common/path_utils.h"
#include "common/file_utils.h"

using json = nlohmann::json;

namespace vinput::config {

static const std::string kExtraPrefix = "extra.";

// Parse a dotpath like "extra.core.active_model" → vector ["core", "active_model"]
// Returns false if prefix is not "extra."
static bool ParseExtraDotpath(const std::string& dotpath, std::vector<std::string>* keys, std::string* error) {
    if (dotpath.substr(0, kExtraPrefix.size()) != kExtraPrefix) {
        if (error) *error = "Only 'extra.*' dotpaths are supported. Got: " + dotpath;
        return false;
    }
    std::string rest = dotpath.substr(kExtraPrefix.size());
    if (rest.empty()) {
        if (error) *error = "Empty path after 'extra.'";
        return false;
    }
    keys->clear();
    std::istringstream ss(rest);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (token.empty()) {
            if (error) *error = "Empty segment in dotpath: " + dotpath;
            return false;
        }
        keys->push_back(token);
    }
    return true;
}

static json LoadRawJson(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return json::object();
    try {
        json j;
        f >> j;
        return j;
    } catch (...) {
        return json::object();
    }
}

bool GetConfigValue(const std::string& dotpath, std::string* value, std::string* error) {
    std::vector<std::string> keys;
    if (!ParseExtraDotpath(dotpath, &keys, error)) return false;

    auto path = vinput::path::CoreConfigPath();
    json j = LoadRawJson(path);

    const json* cur = &j;
    for (const auto& key : keys) {
        if (!cur->is_object() || !cur->contains(key)) {
            if (error) *error = "Key not found: " + key;
            return false;
        }
        cur = &(*cur)[key];
    }

    if (cur->is_string()) {
        *value = cur->get<std::string>();
    } else if (cur->is_boolean()) {
        *value = cur->get<bool>() ? "true" : "false";
    } else if (cur->is_number_integer()) {
        *value = std::to_string(cur->get<int64_t>());
    } else if (cur->is_number_float()) {
        *value = std::to_string(cur->get<double>());
    } else {
        *value = cur->dump();
    }
    return true;
}

bool SetConfigValue(const std::string& dotpath, const std::string& value, std::string* error) {
    std::vector<std::string> keys;
    if (!ParseExtraDotpath(dotpath, &keys, error)) return false;

    auto path = vinput::path::CoreConfigPath();
    json j = LoadRawJson(path);

    // Navigate to parent and set the leaf
    json* cur = &j;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& key = keys[i];
        if (!cur->is_object()) *cur = json::object();
        if (!cur->contains(key) || !(*cur)[key].is_object()) {
            (*cur)[key] = json::object();
        }
        cur = &(*cur)[key];
    }

    const std::string& leaf = keys.back();
    // Type inference: bool, int, or string
    if (value == "true") {
        (*cur)[leaf] = true;
    } else if (value == "false") {
        (*cur)[leaf] = false;
    } else {
        bool all_digits = !value.empty();
        for (char c : value) {
            if (c < '0' || c > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            try {
                (*cur)[leaf] = std::stoi(value);
            } catch (...) {
                (*cur)[leaf] = value;
            }
        } else {
            (*cur)[leaf] = value;
        }
    }

    std::string err;
    if (!vinput::file::EnsureParentDirectory(path, &err)) {
        if (error) *error = "Failed to create config directory: " + err;
        return false;
    }

    std::string content = j.dump(4) + "\n";
    if (!vinput::file::AtomicWriteTextFile(path, content, &err)) {
        if (error) *error = "Failed to write config: " + err;
        return false;
    }
    return true;
}

std::filesystem::path GetEditTarget(const std::string& target) {
    if (target == "extra") {
        return vinput::path::CoreConfigPath();
    }
    // "fcitx" → ~/.config/fcitx5/conf/vinput.conf
    auto home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / ".config" / "fcitx5" / "conf" / "vinput.conf";
}

} // namespace vinput::config
