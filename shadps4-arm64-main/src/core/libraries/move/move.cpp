// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/vr/vr_runtime.h"

// With SHADPS4_QUEST_MOVE=1 the headset's own controllers play as two PlayStation Move wands
// (see Core::Vr::OpenXrHost::UpdateMoves): the title opens them here, reads their buttons and
// motion sensors here, and asks libSceVrTracker where they are. Without it no wand is ever
// connected, as before.

namespace Libraries::Move {

namespace {

constexpr s32 FirstHandle = 0x30b0000;

struct Wand {
    s32 handle{-1};
    std::atomic<u32> count{};
};

bool g_library_initialized = false;
std::mutex g_mutex;
// Even when no controllers are connected, a proper handle is returned: libSceVrTracker
// requires it to be different from those of other devices.
s32 g_next_handle = FirstHandle;
std::array<Wand, Core::Vr::NumMoveWands> g_wands;

bool WandsEnabled() {
    return Core::Vr::Runtime::Instance().MoveWandsEnabled();
}

/// The wand a handle stands for, or -1 for one that is open but has no wand behind it.
int FindWand(s32 handle) {
    std::scoped_lock lock{g_mutex};
    for (int i = 0; i < static_cast<int>(g_wands.size()); ++i) {
        if (g_wands[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

/// What the wand's motion sensors would read, from how the host says it is turned and moves:
/// gravity (in g, as the console reports it) and the turning rate (rad/s), in the wand's own
/// frame (+X right, +Y up, -Z towards the sphere when it points ahead).
void FillSensors(const Core::Vr::DeviceState& state, OrbisMoveData& data) {
    using namespace Core::Vr;
    const Quat to_wand = Conjugate(state.pose.orientation);
    const Vec3 up = Rotate(to_wand, {0.0f, 1.0f, 0.0f});
    data.accelerometer[0] = up.x;
    data.accelerometer[1] = up.y;
    data.accelerometer[2] = up.z;
    const Vec3 spin = Rotate(to_wand, state.angular_velocity);
    data.gyro[0] = spin.x;
    data.gyro[1] = spin.y;
    data.gyro[2] = spin.z;
}

void FillState(int wand, OrbisMoveData& data) {
    auto& runtime = Core::Vr::Runtime::Instance();
    data = {};
    const Core::Vr::DeviceState state = runtime.GetMove(wand);
    if (state.tracked) {
        FillSensors(state, data);
    } else {
        // Lying still, pointing ahead.
        data.accelerometer[1] = 1.0f;
    }
    const Core::Vr::MoveInput input = runtime.GetMoveInput(wand);
    data.button_data.button_data = input.buttons;
    data.button_data.trigger_data = input.trigger;
    data.timestamp = static_cast<s64>(Libraries::Kernel::sceKernelGetProcessTime());
    data.count = static_cast<s32>(g_wands[wand].count.fetch_add(1) + 1);
    data.temperature = 30.0f;
}

} // namespace

int WandForHandle(s32 handle) {
    if (!WandsEnabled()) {
        return -1;
    }
    return FindWand(handle);
}

s32 PS4_SYSV_ABI sceMoveInit() {
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    if (WandsEnabled()) {
        LOG_INFO(Lib_Move, "Move wands: the headset's controllers (right = first, left = second)");
    } else {
        LOG_WARNING(Lib_Move, "Move controllers are not supported yet (SHADPS4_QUEST_MOVE=1 "
                              "plays them with the headset's controllers)");
    }
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(Libraries::UserService::OrbisUserServiceUserId user_id, s32 type,
                             s32 index) {
    LOG_DEBUG(Lib_Move, "called, user_id = {}, type = {}, index = {}", user_id, type, index);
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (index < 0 && WandsEnabled()) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    std::scoped_lock lock{g_mutex};
    g_next_handle += 0x100;
    const s32 handle = g_next_handle;
    if (WandsEnabled() && index < static_cast<s32>(g_wands.size())) {
        if (g_wands[index].handle != -1) {
            return ORBIS_MOVE_ERROR_ALREADY_OPENED;
        }
        g_wands[index].handle = handle;
        LOG_INFO(Lib_Move, "Move wand {} opened as handle {:#x}", index + 1, handle);
    }
    return handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    if (WandForHandle(handle) < 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // The sphere of a Move is 45 mm across.
    info->sphere_radius = 0.0225f;
    info->accelerometer_offset[0] = 0.0f;
    info->accelerometer_offset[1] = 0.0f;
    info->accelerometer_offset[2] = 0.0f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const int wand = WandForHandle(handle);
    if (wand < 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    FillState(wand, *data);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                        s32* out_count) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const int wand = WandForHandle(handle);
    if (wand < 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // The host gives one state a frame: that is the only one there is since `timestamp`.
    FillState(wand, data[0]);
    *out_count = 1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, void* data) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    LOG_TRACE(Lib_Move, "called, intensity = {}", intensity);
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const int wand = WandForHandle(handle);
    if (wand < 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    Core::Vr::Runtime::Instance().SetMoveVibration(wand, intensity);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const int wand = WandForHandle(handle);
    if (wand < 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    Core::Vr::Runtime::Instance().SetMoveLight(wand, red, green, blue);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    LOG_TRACE(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    // Returns success, even if no controllers are connected.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const int wand = FindWand(handle);
    if (wand >= 0) {
        Core::Vr::Runtime::Instance().SetMoveVibration(wand, 0);
        std::scoped_lock lock{g_mutex};
        g_wands[wand].handle = -1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    LOG_DEBUG(Lib_Move, "called");
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    {
        std::scoped_lock lock{g_mutex};
        for (int i = 0; i < static_cast<int>(g_wands.size()); ++i) {
            if (g_wands[i].handle != -1) {
                Core::Vr::Runtime::Instance().SetMoveVibration(i, 0);
            }
            g_wands[i].handle = -1;
        }
    }
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
