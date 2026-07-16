// gui/gui_pipeline.h — pipeline runner for the WebView GUI.
//
// Wraps the same stage sequence as main.cpp's run_pipeline, but reports
// progress/log/status through a JSON event sink instead of stdout, and
// supports cooperative cancellation (between stages).
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace recomp::gui {

struct GuiPipelineParams {
    std::string job_id;
    std::string iso_path;
    std::string output_dir;
    std::string profile_name;
    std::string sdk_path;         // "" = auto-detect
    std::string sdk_source_path;  // "" = none (lite mode)
    bool clean = false;
};

/// Serialized-JSON event sink. Called from the pipeline worker thread; the
/// host marshals to the UI thread. Event shapes match the UI bridge:
///   {event:"job", id, kind:"log",      t, level, msg}
///   {event:"job", id, kind:"progress", progress, stageId}
///   {event:"job", id, kind:"status",   status, error?, deployDir?}
using GuiEventSink = std::function<void(const std::string& json)>;

/// Runs the full pipeline. Blocking — call on a worker thread.
/// `cancel` is polled between stages.
void RunGuiPipeline(const GuiPipelineParams& params,
                    const std::atomic<bool>& cancel,
                    const GuiEventSink& sink);

} // namespace recomp::gui
