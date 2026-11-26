#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

struct TempMemoryCleanup {
    std::optional<std::uint64_t> released_bytes;
    std::size_t directories_removed = 0;
    bool incomplete = false;
};

// Keep the returned file descriptor open while using the directory.
int register_temp_directory(const std::filesystem::path &directory);
std::optional<std::uint64_t> tmpfs_used_bytes(const std::filesystem::path &root);
TempMemoryCleanup clean_temp_memory(const std::filesystem::path &root);
