#include "ai/json_util.hpp"

#include <cctype>
#include <cstdlib>

namespace spinel::ai {

namespace {

void skip_ws(const std::string &s, size_t &pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
}

bool skip_json_value(const std::string &s, size_t &pos, std::string &raw_slice);

bool parse_scalar_or_raw(const std::string &s, size_t &pos, std::string &scalar, std::string &raw_slice) {
    skip_ws(s, pos);
    if (pos >= s.size()) return false;
    size_t start = pos;
    char c = s[pos];
    if (c == '"') {
        if (!json_parse_string(s, pos, scalar)) return false;
        raw_slice = s.substr(start, pos - start);
        return true;
    }
    if (c == '{' || c == '[') {
        if (!skip_json_value(s, pos, raw_slice)) return false;
        scalar = raw_slice;
        return true;
    }
    while (pos < s.size()) {
        char ch = s[pos];
        if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        ++pos;
    }
    if (pos == start) return false;
    raw_slice = s.substr(start, pos - start);
    scalar = (raw_slice == "null") ? "" : raw_slice;
    return true;
}

bool skip_json_value(const std::string &s, size_t &pos, std::string &raw_slice) {
    skip_ws(s, pos);
    if (pos >= s.size()) return false;
    size_t start = pos;
    char open = s[pos];
    if (open != '{' && open != '[') {
        std::string tmp;
        return parse_scalar_or_raw(s, pos, tmp, raw_slice);
    }
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    while (pos < s.size()) {
        char ch = s[pos++];
        if (in_str) {
            if (ch == '\\' && pos < s.size()) ++pos;
            else if (ch == '"') in_str = false;
        } else {
            if (ch == '"') in_str = true;
            else if (ch == open) ++depth;
            else if (ch == close) {
                --depth;
                if (depth == 0) {
                    raw_slice = s.substr(start, pos - start);
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

bool json_parse_string(const std::string &src, size_t &pos, std::string &out) {
    skip_ws(src, pos);
    if (pos >= src.size() || src[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < src.size()) {
        char c = src[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (pos >= src.size()) return false;
        char esc = src[pos++];
        switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (pos + 4 > src.size()) return false;
                unsigned int cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = src[pos + i];
                    if (!std::isxdigit(static_cast<unsigned char>(h))) return false;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                    else cp |= static_cast<unsigned int>(h - 'A' + 10);
                }
                pos += 4;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += esc; break;
        }
    }
    return false;
}

bool json_parse_object(
    const std::string &src,
    size_t &pos,
    const std::function<void(const std::string &key, const std::string &val, const std::string &raw)> &on_field) {
    skip_ws(src, pos);
    if (pos >= src.size() || src[pos] != '{') return false;
    ++pos;
    skip_ws(src, pos);
    if (pos < src.size() && src[pos] == '}') {
        ++pos;
        return true;
    }
    while (pos < src.size()) {
        std::string key;
        if (!json_parse_string(src, pos, key)) return false;
        skip_ws(src, pos);
        if (pos >= src.size() || src[pos] != ':') return false;
        ++pos;
        std::string val, raw;
        if (!parse_scalar_or_raw(src, pos, val, raw)) return false;
        if (on_field) on_field(key, val, raw);
        skip_ws(src, pos);
        if (pos >= src.size()) return false;
        if (src[pos] == ',') {
            ++pos;
            continue;
        }
        if (src[pos] == '}') {
            ++pos;
            return true;
        }
        return false;
    }
    return false;
}

bool json_parse_flat_object(const std::string &src, std::map<std::string, std::string> &out) {
    out.clear();
    return json_parse_object_str(src, [&](const std::string &k, const std::string &v, const std::string &) {
        out[k] = v;
    });
}

bool json_parse_array(const std::string &src,
                      size_t &pos,
                      const std::function<void(const std::string &raw_elem)> &on_elem) {
    skip_ws(src, pos);
    if (pos >= src.size() || src[pos] != '[') return false;
    ++pos;
    skip_ws(src, pos);
    if (pos < src.size() && src[pos] == ']') {
        ++pos;
        return true;
    }
    while (pos < src.size()) {
        std::string raw;
        if (!skip_json_value(src, pos, raw)) return false;
        if (on_elem) on_elem(raw);
        skip_ws(src, pos);
        if (pos >= src.size()) return false;
        if (src[pos] == ',') {
            ++pos;
            continue;
        }
        if (src[pos] == ']') {
            ++pos;
            return true;
        }
        return false;
    }
    return false;
}

bool json_parse_object_str(
    const std::string &src,
    const std::function<void(const std::string &key, const std::string &val, const std::string &raw)> &on_field) {
    size_t pos = 0;
    if (!json_parse_object(src, pos, on_field)) return false;
    skip_ws(src, pos);
    return pos == src.size();
}

bool json_parse_array_str(const std::string &src, const std::function<void(const std::string &raw_elem)> &on_elem) {
    size_t pos = 0;
    if (!json_parse_array(src, pos, on_elem)) return false;
    skip_ws(src, pos);
    return pos == src.size();
}

long long json_scalar_i64(const std::string &val, long long fallback) {
    if (val.empty()) return fallback;
    char *endp = nullptr;
    long long parsed = std::strtoll(val.c_str(), &endp, 10);
    return (endp != val.c_str() && *endp == '\0') ? parsed : fallback;
}

bool json_scalar_bool(const std::string &val, bool fallback) {
    if (val == "true" || val == "1") return true;
    if (val == "false" || val == "0") return false;
    return fallback;
}

}  // namespace spinel::ai
