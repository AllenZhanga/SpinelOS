#pragma once

#include "app_state.hpp"

#include <string>

void push_store_catalog(AppState *state);
void start_store_action(AppState *state, const std::string &action, const std::string &payload);
void cancel_store_action(AppState *state);
void launch_store_app(AppState *state, const std::string &app_id);

// Testable Store helpers
inline bool is_supported_native_app_id(const std::string &app_id) {
    return app_id == "chrome" || app_id == "heroic" || app_id == "protonup" || app_id == "gravitymark" ||
           app_id == "libreoffice" || app_id == "printing" || app_id == "vscode" || app_id == "github-desktop" ||
           app_id == "docker" || app_id == "nodejs" || app_id == "python" || app_id == "ollama" ||
           app_id == "vllm";
}
inline std::string select_ollama_pacman_pkg(bool has_nvidia, bool has_amd) {
    if (has_nvidia) return "ollama-cuda";
    if (has_amd) return "ollama-rocm";
    return "ollama";
}

inline bool is_valid_store_service_unit(const std::string &unit) {
    return unit == "ollama" || unit == "vllm" || unit == "docker" || unit == "cups";
}

inline bool is_user_systemd_service_unit(const std::string &unit) {
    return unit == "vllm";
}

inline bool is_valid_https_url(const std::string &url) {
    if (url.rfind("https://", 0) != 0 || url.size() <= 8 || url.size() > 256) return false;
    for (char ch : url) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '\\') {
            return false;
        }
    }
    return true;
}
