#pragma once

#include <windows.h>
#include <memory>
#include <functional>
#include "stuttometer/session_benchmark.hpp"

namespace stuttometer::gui {

void ShowBenchmarkView(
    HWND parent_hwnd,
    std::shared_ptr<SessionBenchmark> benchmark,
    bool redact,
    std::function<bool()> is_capturing_fn = nullptr
);

} // namespace stuttometer::gui
