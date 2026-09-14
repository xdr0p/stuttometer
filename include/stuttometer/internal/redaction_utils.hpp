#pragma once

#include "stuttometer/correlator.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <cctype>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace stuttometer {

inline std::string get_current_username() {
#ifdef _WIN32
    static const std::string cached_user = []() -> std::string {
        wchar_t buf[256]{};
        DWORD size = 256;
        if (GetUserNameW(buf, &size) && size > 1) {
            int needed = WideCharToMultiByte(CP_UTF8, 0, buf, size - 1, nullptr, 0, nullptr, nullptr);
            if (needed > 0) {
                std::string u(needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, buf, size - 1, u.data(), needed, nullptr, nullptr);
                return u;
            }
        }
        return {};
    }();
    return cached_user;
#else
    return {};
#endif
}

inline std::string redact_paths_and_usernames_in_text(std::string_view text) {
    if (text.empty()) return {};

    std::string result;
    result.reserve(text.size());

    const std::string username = get_current_username();

    size_t i = 0;
    while (i < text.size()) {
        // Check for drive letter path, e.g. "C:\" or "C:/"
        bool is_drive_path = (i + 2 < text.size() &&
                              (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1]))) &&
                              std::isalpha(static_cast<unsigned char>(text[i])) &&
                              text[i + 1] == ':' &&
                              (text[i + 2] == '\\' || text[i + 2] == '/'));

        // Check for UNC path, e.g. "\\server\share"
        bool is_unc_path = false;
        size_t unc_slash_pos = 0;
        if (i + 3 < text.size() &&
            text[i] == '\\' && text[i + 1] == '\\' &&
            (i == 0 || (!std::isalnum(static_cast<unsigned char>(text[i - 1])) && text[i - 1] != '\\'))) {
            size_t s = i + 2;
            while (s < text.size() && text[s] != '\\' && text[s] != '/' &&
                   text[s] != ' ' && text[s] != '\t' && text[s] != '\r' && text[s] != '\n' &&
                   text[s] != '"' && text[s] != '\'' && text[s] != '`') {
                ++s;
            }
            if (s > i + 2 && s < text.size() && (text[s] == '\\' || text[s] == '/')) {
                is_unc_path = true;
                unc_slash_pos = s;
            }
        }

        if (is_drive_path || is_unc_path) {
            char quote_char = 0;
            if (i > 0 && (text[i - 1] == '"' || text[i - 1] == '\'' || text[i - 1] == '`')) {
                if (text[i - 1] != '\'' || i == 1 || !std::isalnum(static_cast<unsigned char>(text[i - 2]))) {
                    quote_char = text[i - 1];
                }
            }

            size_t close_quote_pos = std::string_view::npos;
            if (quote_char != 0) {
                close_quote_pos = text.find(quote_char, i);
                if (close_quote_pos != std::string_view::npos) {
                    if (text.substr(i, close_quote_pos - i).find_first_of("\r\n") != std::string_view::npos) {
                        close_quote_pos = std::string_view::npos;
                    }
                }
            }

            if (quote_char != 0 && close_quote_pos != std::string_view::npos) {
                // Quoted Path: consume all characters until the matching closing quote
                result.append("[PATH_REDACTED]");
                i = close_quote_pos;
                continue;
            }

            if (quote_char != 0 && close_quote_pos == std::string_view::npos) {
                // Unterminated quote fallback: strip unclosed opening quote from result
                if (!result.empty() && result.back() == quote_char) {
                    result.pop_back();
                }
            }

            // Unquoted Path: scan until hard delimiter with balanced paren tracking
            const size_t min_end = is_drive_path ? (i + 3) : (unc_slash_pos + 1);
            size_t end = min_end;
            int paren_depth = 0;
            while (end < text.size()) {
                char c = text[end];
                if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\'' || c == '`' ||
                    c == ',' || c == ';' || c == ']' || c == '}' || c == '>' || c == '<') {
                    break;
                }
                if (c == '(') {
                    ++paren_depth;
                } else if (c == ')') {
                    if (paren_depth > 0) {
                        --paren_depth;
                    } else {
                        // Unbalanced closing paren opened before path
                        break;
                    }
                }
                ++end;
            }

            // Trailing punctuation & whitespace trimming
            while (end > min_end) {
                char last = text[end - 1];
                if (last == '.' || last == ':' || last == '!' || last == '?' ||
                    last == ' ' || last == '\t' || last == '\r' || last == '\n') {
                    --end;
                } else {
                    break;
                }
            }

            result.append("[PATH_REDACTED]");
            i = end;
            continue;
        }

        // Check for case-insensitive username match
        if (!username.empty() && (i + username.size() <= text.size())) {
            bool matches_user = true;
            for (size_t u = 0; u < username.size(); ++u) {
                if (std::tolower(static_cast<unsigned char>(text[i + u])) !=
                    std::tolower(static_cast<unsigned char>(username[u]))) {
                    matches_user = false;
                    break;
                }
            }
            if (matches_user) {
                // Verify word/path boundary before and after
                bool prev_ok = (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])));
                bool next_ok = (i + username.size() >= text.size() || !std::isalnum(static_cast<unsigned char>(text[i + username.size()])));
                if (prev_ok && next_ok) {
                    result.append("REDACTED");
                    i += username.size();
                    continue;
                }
            }
        }

        result.push_back(text[i]);
        ++i;
    }

    return result;
}

inline std::string redact_id_in_text(std::string_view text, uint32_t id) {
    if (id == 0 || text.empty()) return std::string(text);
    const std::string id_str = std::to_string(id);
    std::string result;
    result.reserve(text.size());

    size_t pos = 0;
    while (pos < text.size()) {
        size_t match_pos = text.find(id_str, pos);
        if (match_pos == std::string_view::npos) {
            result.append(text.substr(pos));
            break;
        }

        // Check previous context: must not be a hex/dec digit, 0x prefix, '.' preceded by a digit,
        // or a unary minus for a negative number (e.g. "-12ms" or "offset: -12")
        bool prev_ok = true;
        if (match_pos > 0) {
            unsigned char prev_c = static_cast<unsigned char>(text[match_pos - 1]);
            if (std::isxdigit(prev_c)) {
                prev_ok = false;
            } else if (prev_c == '.' && match_pos > 1 && std::isdigit(static_cast<unsigned char>(text[match_pos - 2]))) {
                prev_ok = false;
            } else if ((prev_c == 'x' || prev_c == 'X') && match_pos > 1 && text[match_pos - 2] == '0') {
                prev_ok = false;
            } else if (prev_c == '-') {
                // If '-' is a unary minus (preceded by start of string, whitespace, or non-alphanumeric punctuation),
                // treat as a negative numerical value, not a PID/TID. If preceded by alphanumeric (e.g. "Thread-12"),
                // it is a hyphenated ID and should be redacted.
                if (match_pos == 1) {
                    prev_ok = false;
                } else {
                    unsigned char before_minus = static_cast<unsigned char>(text[match_pos - 2]);
                    if (!std::isalnum(before_minus)) {
                        prev_ok = false;
                    }
                }
            }
        }

        // Check next context: must not be a hex/dec digit and must not be '.' followed by a digit (e.g. 12.5)
        const size_t next_idx = match_pos + id_str.size();
        bool next_ok = true;
        if (next_idx < text.size()) {
            unsigned char next_c = static_cast<unsigned char>(text[next_idx]);
            if (std::isxdigit(next_c)) {
                next_ok = false;
            } else if (next_c == '.' && next_idx + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[next_idx + 1]))) {
                next_ok = false;
            }
        }

        if (prev_ok && next_ok) {
            result.append(text.substr(pos, match_pos - pos));
            result.append("REDACTED");
            pos = match_pos + id_str.size();
        } else {
            result.append(text.substr(pos, (match_pos + 1) - pos));
            pos = match_pos + 1;
        }
    }
    return result;
}

inline std::string get_redacted_module_name(std::string_view module_name, bool redact) {
    if (!redact) return std::string(module_name);
    if (module_name.empty()) return {};

    bool is_sys = false;
    if (module_name.size() >= 4) {
        auto ends_with_ci = [](std::string_view str, std::string_view suffix) {
            if (str.size() < suffix.size()) return false;
            size_t offset = str.size() - suffix.size();
            for (size_t i = 0; i < suffix.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(str[offset + i])) !=
                    std::tolower(static_cast<unsigned char>(suffix[i]))) {
                    return false;
                }
            }
            return true;
        };
        is_sys = ends_with_ci(module_name, ".sys");
    }
    return is_sys ? "driver_REDACTED.sys" : "module_REDACTED";
}

inline void extract_ids_from_text(std::string_view text, std::vector<uint32_t>& out_ids) {
    if (text.empty()) return;
    std::string lower_text;
    lower_text.resize(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        lower_text[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    }

    const std::string_view markers[] = { "tid", "pid", "thread" };
    for (const auto& marker : markers) {
        size_t pos = 0;
        while ((pos = lower_text.find(marker, pos)) != std::string::npos) {
            // Check boundary before marker (must not be alphanumeric)
            if (pos > 0 && std::isalnum(static_cast<unsigned char>(lower_text[pos - 1]))) {
                pos += marker.size();
                continue;
            }
            pos += marker.size();
            // Require a separator after marker
            if (pos >= lower_text.size() ||
                (lower_text[pos] != ' ' && lower_text[pos] != ':' && lower_text[pos] != '=' && lower_text[pos] != '#')) {
                continue;
            }
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == ':' || text[pos] == '=' || text[pos] == '#')) {
                ++pos;
            }
            if (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                uint64_t val = 0;
                while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                    val = val * 10 + (text[pos] - '0');
                    if (val > 0xFFFFFFFFULL) break;
                    ++pos;
                }
                if (val > 0 && val <= 0xFFFFFFFFULL) {
                    out_ids.push_back(static_cast<uint32_t>(val));
                }
            }
        }
    }
}

inline std::string redact_text_with_ids(std::string_view text, const std::vector<uint32_t>& ids_to_redact) {
    std::string s = redact_paths_and_usernames_in_text(text);
    for (uint32_t id : ids_to_redact) {
        if (id != 0) {
            s = redact_id_in_text(s, id);
        }
    }
    std::vector<uint32_t> text_ids;
    extract_ids_from_text(s, text_ids);
    for (uint32_t id : text_ids) {
        if (id != 0) {
            s = redact_id_in_text(s, id);
        }
    }
    return s;
}

inline std::vector<uint32_t> collect_report_ids(const DiagnosticReport& report) {
    std::vector<uint32_t> ids;
    if (report.trigger.target_tid != 0) ids.push_back(report.trigger.target_tid);
    if (report.trigger.target_pid != 0) ids.push_back(report.trigger.target_pid);
    if (report.attribution_pid != 0) ids.push_back(report.attribution_pid);
    for (const auto& diag : report.diagnoses) {
        for (const auto& ev : diag.evidence) {
            if (ev.secondary_tid != 0) ids.push_back(ev.secondary_tid);
            if (ev.secondary_pid != 0) ids.push_back(ev.secondary_pid);
            if (ev.pid != 0) ids.push_back(ev.pid);
        }
    }
    return ids;
}

} // namespace stuttometer
