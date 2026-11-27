#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spinel::ai {

/// Parse a JSON string starting at `pos` (advances `pos` past closing quote).
bool json_parse_string(const std::string &src, size_t &pos, std::string &out);

/// Parse a JSON object starting at `pos` and invoke `on_field(key, unquoted_or_raw, raw_slice)`.
bool json_parse_object(
    const std::string &src,
    size_t &pos,
    const std::function<void(const std::string &key, const std::string &val, const std::string &raw)> &on_field);

/// Parse a flat JSON object into a string map (scalars unquoted, objects/arrays preserved as raw JSON).
bool json_parse_flat_object(const std::string &src, std::map<std::string, std::string> &out);

/// Parse a JSON array starting at `pos` and invoke `on_elem(raw_slice)` for each element.
bool json_parse_array(const std::string &src,
                      size_t &pos,
                      const std::function<void(const std::string &raw_elem)> &on_elem);

/// Extract key/value string fields from a JSON object inside a slice (whole string).
bool json_parse_object_str(
    const std::string &src,
    const std::function<void(const std::string &key, const std::string &val, const std::string &raw)> &on_field);

/// Extract raw elements from a JSON array string (whole string).
bool json_parse_array_str(const std::string &src, const std::function<void(const std::string &raw_elem)> &on_elem);

/// Parse integer/boolean JSON scalar strings safely.
long long json_scalar_i64(const std::string &val, long long fallback = 0);
bool json_scalar_bool(const std::string &val, bool fallback = false);

}  // namespace spinel::ai
