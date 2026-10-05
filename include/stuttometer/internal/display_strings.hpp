#pragma once

#include <string_view>

namespace stuttometer::display {

// Returns friendly UTF-8 display name for a raw trigger reason string/key.
// Unknown keys are returned unchanged.
[[nodiscard]] std::string_view trigger_reason_display(std::string_view raw) noexcept;

// Returns friendly UTF-8 display name for a raw trigger source string/key.
// Unknown keys are returned unchanged.
[[nodiscard]] std::string_view trigger_source_display(std::string_view raw) noexcept;

// Returns friendly UTF-8 display name for a raw hypothesis identifier.
// Unknown keys are returned unchanged.
[[nodiscard]] std::string_view hypothesis_display(std::string_view raw) noexcept;

} // namespace stuttometer::display
