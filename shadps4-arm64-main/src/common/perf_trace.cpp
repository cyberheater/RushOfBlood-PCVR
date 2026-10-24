// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include "common/path_util.h"
#include "common/perf_trace.h"
#include "common/rdtsc.h"

#ifndef _WIN32
extern char** environ;
#endif

namespace Common::PerfTrace {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t NumTimes = static_cast<size_t>(Time::Count);
constexpr size_t NumCounts = static_cast<size_t>(Count::Count);

constexpr const char* TimeColumns[NumTimes] = {
    "submit_ms",   "catchup_ms",   "gputhread_cpu_ms", "queued_ms",
    "gpuwait_ms",  "latency_ms",   "gpu_ms",           "gpu_present_ms",
    "wait_throttle_ms", "wait_result_ms", "shader_ms", "pipeline_ms",
};
constexpr const char* CountColumns[NumCounts] = {
    "shaders", "pipelines", "passes", "draws", "dispatches", "submits", "gpu_timed",
};

std::array<std::atomic<u64>, NumTimes> g_times{};
std::array<std::atomic<u64>, NumCounts> g_counts{};

/// One CSV file, opened the first time a row is written to it.
class CsvFile {
public:
    explicit CsvFile(const char* name_) : name{name_} {}

    ~CsvFile() {
        std::scoped_lock lock{mutex};
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    /// Calls `write(FILE*)` for a row, under the file's lock. `header(FILE*)` writes the
    /// lines at the top the first time.
    template <typename Header, typename Write>
    void Row(Header&& header, Write&& write) {
        std::scoped_lock lock{mutex};
        if (failed) {
            return;
        }
        if (file == nullptr && !Open(header)) {
            return;
        }
        write(file);
        // Buffered, but never more than about two seconds behind (in case the emulator is
        // closed without its destructors running).
        const auto now = Clock::now();
        if (now - last_flush > std::chrono::seconds{2}) {
            std::fflush(file);
            last_flush = now;
        }
    }

private:
    template <typename Header>
    bool Open(Header&& header) {
        std::error_code error;
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        std::filesystem::create_directories(dir, error);
        const auto path = dir / name;
#ifdef _WIN32
        file = _wfopen(path.c_str(), L"w");
#else
        file = std::fopen(path.c_str(), "w");
#endif
        if (file == nullptr) {
            failed = true;
            return false;
        }
        std::setvbuf(file, nullptr, _IOFBF, 1 << 16);
        header(file);
        last_flush = Clock::now();
        return true;
    }

    const char* name;
    std::mutex mutex;
    std::FILE* file{};
    bool failed{};
    Clock::time_point last_flush{};
};

CsvFile g_frames_file{"perf_frames.csv"};
CsvFile g_headset_file{"perf_headset.csv"};

const Clock::time_point g_start = Clock::now();

double SecondsSinceStart(Clock::time_point now) {
    return std::chrono::duration<double>(now - g_start).count();
}

/// The settings the emulator was started with that matter for performance: every SHADPS4_
/// variable, as a comment line.
void WriteEnvironment(std::FILE* file) {
#ifdef _WIN32
    char** env = _environ;
#else
    char** env = environ;
#endif
    std::fputs("# environment:", file);
    for (; env != nullptr && *env != nullptr; ++env) {
        if (std::strncmp(*env, "SHADPS4_", 8) == 0) {
            std::fprintf(file, " %s", *env);
        }
    }
    std::fputc('\n', file);
}

void WriteStarted(std::FILE* file, const char* what) {
    const std::time_t now = std::time(nullptr);
    char when[64]{};
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(file, "# RushVR performance trace (%s), started %s\n", what, when);
    WriteEnvironment(file);
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_PERF_TRACE");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void Add(Time what, std::chrono::nanoseconds time) {
    if (!Enabled() || time.count() <= 0) {
        return;
    }
    g_times[static_cast<size_t>(what)].fetch_add(static_cast<u64>(time.count()),
                                                 std::memory_order_relaxed);
}

void Add(Count what, u64 amount) {
    if (!Enabled()) {
        return;
    }
    g_counts[static_cast<size_t>(what)].fetch_add(amount, std::memory_order_relaxed);
}

void EndFrame() {
    if (!Enabled()) {
        return;
    }
    static u64 frame = 0;
    static Clock::time_point last;
    const auto now = Clock::now();
    const double frame_ms =
        last == Clock::time_point{} ? 0.0
                                    : std::chrono::duration<double, std::milli>(now - last).count();
    last = now;

    std::array<u64, NumTimes> times;
    for (size_t i = 0; i < NumTimes; ++i) {
        times[i] = g_times[i].exchange(0, std::memory_order_relaxed);
    }
    std::array<u64, NumCounts> counts;
    for (size_t i = 0; i < NumCounts; ++i) {
        counts[i] = g_counts[i].exchange(0, std::memory_order_relaxed);
    }

    g_frames_file.Row(
        [](std::FILE* file) {
            WriteStarted(file, "one row per frame the title presents");
            std::fputs("# GPU times come from timestamps read back once the GPU has finished, "
                       "so they lag their frame by one to three rows.\n",
                       file);
            std::fputs("frame,t_s,frame_ms", file);
            for (const char* column : TimeColumns) {
                std::fprintf(file, ",%s", column);
            }
            for (const char* column : CountColumns) {
                std::fprintf(file, ",%s", column);
            }
            std::fputc('\n', file);
        },
        [&](std::FILE* file) {
            std::fprintf(file, "%llu,%.4f,%.3f", static_cast<unsigned long long>(frame),
                         SecondsSinceStart(now), frame_ms);
            for (const u64 ns : times) {
                std::fprintf(file, ",%.3f", static_cast<double>(ns) / 1e6);
            }
            for (const u64 count : counts) {
                std::fprintf(file, ",%llu", static_cast<unsigned long long>(count));
            }
            std::fputc('\n', file);
        });
    ++frame;
}

void Headset(const HeadsetRefresh& refresh) {
    if (!Enabled()) {
        return;
    }
    const auto now = Clock::now();
    g_headset_file.Row(
        [](std::FILE* file) {
            WriteStarted(file, "one row per refresh of the headset's display");
            std::fputs("t_s,interval_ms,wait_ms,copy_ms,end_ms,display_hz,new_frame,shown\n",
                       file);
        },
        [&](std::FILE* file) {
            std::fprintf(file, "%.4f,%.3f,%.3f,%.3f,%.3f,%.1f,%d,%d\n", SecondsSinceStart(now),
                         refresh.interval_ms, refresh.wait_ms, refresh.copy_ms, refresh.end_ms,
                         refresh.display_hz, refresh.new_frame ? 1 : 0, refresh.shown ? 1 : 0);
        });
}

std::chrono::nanoseconds ThreadCpuTime() {
#ifdef _WIN32
    // GetThreadTimes only moves on in steps of the scheduler's tick (about 15.6 ms), far too
    // coarse for a frame. The thread's cycle count is exact; it counts at the rate of the
    // processor's time stamp counter, which is measured once, off this thread.
    static std::atomic<u64> tsc_hz{0};
    static std::once_flag measure;
    std::call_once(measure, [] {
        std::thread([] { tsc_hz.store(Common::EstimateRDTSCFrequency()); }).detach();
    });
    const u64 hz = tsc_hz.load(std::memory_order_relaxed);
    ULONG64 cycles = 0;
    if (hz == 0 || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        return std::chrono::nanoseconds{0};
    }
    return std::chrono::nanoseconds{
        static_cast<s64>(static_cast<double>(cycles) * 1e9 / static_cast<double>(hz))};
#else
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec};
#endif
}

} // namespace Common::PerfTrace
