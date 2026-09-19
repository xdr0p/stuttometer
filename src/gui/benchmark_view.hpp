#pragma once

#include <windows.h>
#include <memory>
#include "stuttometer/session_benchmark.hpp"

namespace stuttometer::gui {

void ShowBenchmarkView(HWND parent_hwnd, std::shared_ptr<SessionBenchmark> benchmark, bool redact);

} // namespace stuttometer::gui
