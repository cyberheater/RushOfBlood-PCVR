// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include "common/types.h"

/// A per-frame performance trace, for finding out what holds a title's frame rate back.
///
/// SHADPS4_PERF_TRACE=1 writes two CSV files into the log folder, independently of the log
/// itself (which can stay off):
///   perf_frames.csv   one row for every frame the title presents: how long the frame took, the
///                     time spent in each part of its way to the display, the GPU time its work
///                     took, waits for the GPU, and the shaders and pipelines compiled for it.
///   perf_headset.csv  one row for every refresh of the headset's display: whether it got a new
///                     frame from the title or showed the last one again, and what it cost.
/// tools/perf-report.py turns them into a summary. Without the variable nothing is recorded
/// and every hook below returns at once.
namespace Common::PerfTrace {

/// True when SHADPS4_PERF_TRACE is set (and not "0").
bool Enabled();

/// Times summed per frame, in the order of the columns of perf_frames.csv.
enum class Time : u32 {
    Submitting,     ///< The title building the frame: first command list until the flip request.
    CatchUp,        ///< From there until the GPU thread had translated all of it.
    GpuThreadCpu,   ///< Processor time the GPU thread spent translating the frame.
    Queued,         ///< Frame ready until a display refresh took it up.
    GpuWait,        ///< From there until the GPU had finished drawing it.
    Latency,        ///< First command list of the frame until its flip.
    GpuTitle,       ///< GPU time of the title's work (and the VR post-processing).
    GpuPresent,     ///< GPU time of putting frames on the window.
    WaitThrottle,   ///< Emulator threads blocked because 8 submissions were in flight.
    WaitResult,     ///< Emulator threads blocked waiting for a GPU result (readbacks, Finish).
    ShaderCompile,  ///< Translating guest shaders to SPIR-V and making modules of them.
    PipelineCompile,///< The driver building pipelines from those modules.
    Count,
};

/// Counts per frame, after the times in perf_frames.csv.
enum class Count : u32 {
    Shaders,
    Pipelines,
    Passes,
    Draws,
    Dispatches,
    Submits,
    GpuTimed, ///< Submissions whose GPU time is in GpuTitle/GpuPresent this frame.
    Count,
};

void Add(Time what, std::chrono::nanoseconds time);
void Add(Count what, u64 amount = 1);

/// The title presented a frame: writes its row and starts the next.
void EndFrame();

/// One refresh of the headset's display (from the OpenXR thread).
struct HeadsetRefresh {
    double interval_ms;   ///< Since the previous refresh.
    double wait_ms;       ///< Spent in xrWaitFrame.
    double copy_ms;       ///< Handing a new frame over (0 when there was none).
    double end_ms;        ///< Spent in xrEndFrame.
    float display_hz;     ///< The rate the runtime says its display runs at.
    bool new_frame;       ///< A frame of the title's that had not been shown yet.
    bool shown;           ///< Something was shown at all (the runtime wanted it and had a frame).
};
void Headset(const HeadsetRefresh& refresh);

/// Processor time the calling thread has used so far (Windows: from its cycle count; 0 for the
/// first tenth of a second while the counter's frequency is measured).
std::chrono::nanoseconds ThreadCpuTime();

} // namespace Common::PerfTrace
