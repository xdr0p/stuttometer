#include "stuttometer/gui_config.hpp"
#include "stuttometer/trigger_engine.hpp"

namespace stuttometer {

void apply_detection_preset(DetectionPreset p, TriggerConfig& trig, GuiConfig& gui) noexcept {
    gui.detection_preset = p;
    switch (p) {
        case DetectionPreset::BALANCED:
            trig.pacing_profile = PacingProfile::AUTO_ADAPTIVE;
            trig.min_report_severity = ReportSeverity::WARNING;
            trig.judder_min_alternations = 5;
            trig.judder_swing_ratio = 0.50;
            trig.dwm_min_missed_vblanks = 2;
            trig.kernel_frame_stall_min_missed_vblanks = 2;
            break;
        case DetectionPreset::COMPETITIVE:
            trig.pacing_profile = PacingProfile::AUTO_ADAPTIVE;
            trig.min_report_severity = ReportSeverity::WARNING;
            trig.judder_min_alternations = 3;
            trig.judder_swing_ratio = 0.35;
            trig.dwm_min_missed_vblanks = 1;
            trig.kernel_frame_stall_min_missed_vblanks = 1;
            break;
        case DetectionPreset::CONSERVATIVE:
            trig.pacing_profile = PacingProfile::CONSERVATIVE;
            trig.min_report_severity = ReportSeverity::DANGER;
            trig.judder_min_alternations = 8;
            trig.judder_swing_ratio = 0.60;
            trig.dwm_min_missed_vblanks = 3;
            trig.kernel_frame_stall_min_missed_vblanks = 3;
            break;
        case DetectionPreset::FORENSIC:
            trig.pacing_profile = PacingProfile::AUTO_ADAPTIVE;
            trig.min_report_severity = ReportSeverity::ALL;
            trig.judder_min_alternations = 3;
            trig.judder_swing_ratio = 0.35;
            trig.dwm_min_missed_vblanks = 1;
            trig.kernel_frame_stall_min_missed_vblanks = 1;
            break;
        case DetectionPreset::CUSTOM:
        default:
            break;
    }
    gui.pacing_profile = trig.pacing_profile;
    gui.judder_swing_ratio = trig.judder_swing_ratio;
}

void apply_detection_preset(DetectionPreset p, TriggerConfig& trig) noexcept {
    GuiConfig dummy;
    apply_detection_preset(p, trig, dummy);
}

} // namespace stuttometer
