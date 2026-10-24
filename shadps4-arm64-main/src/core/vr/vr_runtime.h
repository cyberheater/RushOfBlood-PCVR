// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "common/types.h"

namespace Core::Vr {

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Quat {
    float x{};
    float y{};
    float z{};
    float w{1.0f};
};

struct Pose {
    Vec3 position;
    Quat orientation;
};

Quat Multiply(const Quat& a, const Quat& b);
Quat Conjugate(const Quat& q);
Quat Normalize(const Quat& q);
Vec3 Rotate(const Quat& q, const Vec3& v);
Quat FromYawPitch(float yaw, float pitch);
/// Yaw about +Y, then pitch about +X, then roll about +Z, in radians.
Quat FromYawPitchRoll(float yaw, float pitch, float roll);
/// Keeps only the rotation about the vertical axis.
Quat YawOnly(const Quat& q);

/// Per-eye field of view as tangents of the half angles. "out" is the temple side and "in" the
/// nose side, so one set of values describes both eyes (this is how libSceHmd reports it).
struct Fov {
    float tan_out{1.20743f};
    float tan_in{1.181346f};
    float tan_top{1.262872f};
    float tan_bottom{1.262872f};
};

/// A tracked device sample, expressed in PSVR tracker space: metres, +X right, +Y up and
/// -Z towards the camera the player is facing.
struct DeviceState {
    Pose pose;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    u64 sequence{}; ///< Host sample counter, 0 when the host never reported this device.
    bool tracked{};
};

/// What a title asks of the controller: its two rumble motors and the colour of its light.
struct PadFeedback {
    u8 small_motor{};
    u8 large_motor{};
    u8 red{};
    u8 green{};
    u8 blue{};

    bool operator==(const PadFeedback&) const = default;
};

/// The two PlayStation Move wands, played with the headset's own controllers
/// (SHADPS4_QUEST_MOVE=1). Wand 0 is the first the title opens: the right hand. Wand 1 is the left.
constexpr int NumMoveWands = 2;

/// Buttons of a Move wand, as libSceMove reports them (the layout of the PS3's libgem, which
/// libSceMove kept).
namespace MoveButtons {
constexpr u16 Select = 1u << 0;
constexpr u16 T = 1u << 1;
constexpr u16 Move = 1u << 2;
constexpr u16 Start = 1u << 3;
constexpr u16 Triangle = 1u << 4;
constexpr u16 Circle = 1u << 5;
constexpr u16 Cross = 1u << 6;
constexpr u16 Square = 1u << 7;
} // namespace MoveButtons

/// What a wand's buttons and trigger say, from the host.
struct MoveInput {
    u16 buttons{};
    u8 trigger{}; ///< 0 released to 255 fully pulled.
};

/// How the headset's controllers are turned into wands. The values are those the launcher
/// recommends; SHADPS4_QUEST_MOVE_PITCH (degrees), SHADPS4_QUEST_MOVE_SCALE and
/// SHADPS4_QUEST_MOVE_TURN (fractions) replace them.
struct MoveConfig {
    bool enabled{false};
    /// How far the wand is raised above the way the controller's grip points. A Touch
    /// controller's handle is tilted down from where the hand points.
    float pitch_degrees{35.0f};
    /// How far the wand moves when the hand does, measured from the head (1 = as far).
    float reach{0.55f};
    /// How far the wand turns when the hand does, measured from level and facing where the
    /// head faces (1 = as far).
    float turn{0.65f};
    /// The sphere a Move is tracked by sits at the end of the wand, ahead of the hand.
    float sphere_ahead{0.06f};
};

/// Everything the host needs to put one emulated HMD frame in front of the user's eyes.
struct PresentedFrame {
    u32 id{};         ///< Matches the marker the presenter stamps into the frame.
    Pose render_pose; ///< Head pose the guest rendered this frame with, in host space.
    Fov fov;          ///< Field of view the eye images were rendered with.
    u32 eye_width{};
    u32 eye_height{};
};

struct Config {
    bool headset_connected{false};
    float ipd{0.063f};
    Fov fov{};
    /// The field of view the title is told of is the headset's own (as the host found it), not
    /// a PlayStation VR's; fov_scale applies to either.
    bool fov_from_headset{false};
    float fov_scale{1.0f};
    /// Where the player's resting head position sits in tracker space. The PS Camera is the
    /// origin, so the player is placed a comfortable distance in front of it.
    Vec3 origin_offset{0.0f, 0.0f, 1.5f};
    /// Refresh rate the emulated headset panel runs at. The game renders 60 fps reprojected
    /// to 120 Hz.
    u32 refresh_rate{120};
    /// Sweeps the head when no host is feeding poses, so stereo output can be checked on a
    /// desktop build.
    bool demo_motion{false};
    /// Where a controller is held when the host cannot see it: relative to the player's head
    /// position, or to the resting head position when it does not follow the head. A gamepad has
    /// no positional tracking outside of the PS Camera, so this is the normal case. The default
    /// is an arm's reach in front of the chest, which is also where titles ask for the controller
    /// to be presented when they calibrate.
    Vec3 pad_offset{0.0f, -0.17f, -0.50f};
    bool pad_follows_head{true};
};

class Runtime {
public:
    static Runtime& Instance();

    /// Decides whether a virtual headset is plugged in for the title being booted.
    void Configure(bool psvr_supported, bool psvr_required);

    bool IsHeadsetConnected() const {
        return config.headset_connected;
    }
    const Config& GetConfig() const {
        return config;
    }

    /// Whether the headset is on the player's head. A host that can tell says so; a title asks
    /// (the headset has a sensor for it) and waits for the player while it is not.
    void SetHeadsetWorn(bool worn) {
        headset_worn.store(worn, std::memory_order_relaxed);
    }
    bool IsHeadsetWorn() const {
        return headset_worn.load(std::memory_order_relaxed);
    }

    // Host side. Poses arrive in host space: the headset's own, whose origin and heading are
    // wherever its system last put them. Where the player actually sits in it, and which way
    // they face, is the "seat" (see RecenterSeat); the title is given poses relative to that.
    void UpdateHead(const DeviceState& host_state);
    /// Declares the head's present position to be the resting one and the direction it looks
    /// in to be straight ahead. A title places its world around where the player sat when it
    /// last took stock (PlayStation VR: at its start, and when the player holds OPTIONS), and
    /// expects them to face the camera that tracks them; a player who has since moved, or who
    /// faces another way than the headset's system assumes, sits in the wrong spot of that
    /// world. Happens by itself for the first pose and when the title first asks for one.
    void RecenterSeat();
    /// RecenterSeat, and the title is told to take stock again, the way the console tells it
    /// when the player asks for the view to be reset.
    void RequestRecenter();
    /// For a host whose poses are counted from the seat already (scripted tests): the seat is
    /// the origin of its space and stays there until RecenterSeat is called.
    void FixSeat();
    void UpdatePad(const DeviceState& host_state);
    /// The host that said where the controller is and how it is turned (UpdatePad) no longer
    /// does: it is placed by what else is known of it again.
    void ReleasePad();
    /// For hosts that only know how the controller is turned (from its motion sensors) and not
    /// where it is. The runtime then places it with Config::pad_offset.
    void UpdatePadOrientation(const Quat& orientation, const Vec3& angular_velocity);
    /// The controller's own motion sensors, for hosts that have nothing better. Readings are in
    /// the controller's frame (+X right, +Y out of the face buttons, +Z towards the player), in
    /// rad/s and m/s² with gravity included, as SDL reports them. The runtime works out how the
    /// controller is turned from them.
    void UpdatePadGyro(const Vec3& angular_velocity);
    void UpdatePadAcceleration(const Vec3& acceleration);
    /// Declares the direction the controller points in right now to be straight ahead.
    void ResetPadYaw();
    /// Whether the controller's motion sensors ever said how it is held.
    bool PadMotionKnown() const {
        std::scoped_lock lock{mutex};
        return pad_attitude_valid;
    }

    /// What a player does with the controller about the view itself, where no application
    /// around the emulator sees to it: the first press of X once the headset shows the game
    /// takes where the head is then for the player's seat (they have settled, controller in
    /// hand); OPTIONS held for a second resets the view, as on a PlayStation VR, and so does
    /// the PS button. The buttons still reach the title.
    enum class PadButton { Cross, Options, Home };
    void EnableViewGestures(bool enabled);
    void NotePadButton(PadButton button, bool pressed);
    /// To be called often: holding a button takes time.
    void PollViewGestures();
    /// For hosts that see where the controller is held (the hands around it) but not how it is
    /// turned; the motion sensors keep providing that.
    void UpdatePadPosition(const Vec3& host_position, const Vec3& linear_velocity);
    /// The host lost sight of the controller: it stays where it was last seen, relative to
    /// the player (or where Config::pad_offset puts it if it never was seen).
    void ClearPadPosition();
    /// Replaces Config::pad_offset, and forgets where the controller was last seen.
    void SetPadOffset(const Vec3& offset);
    /// What the host can tell about the controller's heading (0 = straight ahead, positive to
    /// the left). The attitude worked out from the motion sensors is pulled towards it, which
    /// takes out the drift a gyroscope has about the vertical.
    void UpdatePadYawReference(float yaw);

    // The other direction: what the title wants the real controller to do.
    void SetPadVibration(u8 small_motor, u8 large_motor);
    void SetPadLight(u8 red, u8 green, u8 blue);
    void SetPadFeedbackListener(std::function<void(const PadFeedback&)> listener);
    void UpdateOptics(const Fov& fov, float ipd);
    /// What the host's headset shows of the world, both eyes together (the widest of the two to
    /// every side), as soon as it knows. Kept in the user folder for later starts.
    void NoteHeadsetFov(const Fov& fov);
    /// The field of view the title is told its headset has (sceHmdGetFieldOfView: titles ask
    /// once, when they open the headset). With Config::fov_from_headset that is the host
    /// headset's own, which is waited for a while if the host does not know it yet.
    Fov TitleFov();
    /// The host's display refreshed (see Protocol::Refresh), at `time` nanoseconds of the
    /// steady clock, or just now if that is 0.
    void NoteDisplayRefresh(float rate, u64 time);
    struct DisplayRefresh {
        u64 sequence{}; ///< How many the host has told of, 0 for none.
        std::chrono::steady_clock::time_point time;
        float rate{};
    };
    DisplayRefresh GetDisplayRefresh() const;
    /// How often the emulated headset refreshes: as often as the host's display while the host
    /// tells of its refreshes, as often as Config::refresh_rate says otherwise.
    float HeadsetRefreshRate() const;

    // The Move wands (see MoveConfig). Host side: where the controller of that hand is and what
    // its buttons say, in host space; LoseMove when the host no longer sees it.
    const MoveConfig& GetMoveConfig() const {
        return move_config;
    }
    bool MoveWandsEnabled() const {
        return move_config.enabled;
    }
    void UpdateMove(int wand, const DeviceState& host_state, const MoveInput& input);
    void UpdateMoveInput(int wand, const MoveInput& input);
    void LoseMove(int wand);
    /// What the title wants a wand to do: its rumble (0 to 255) and the colour of its sphere.
    void SetMoveVibration(int wand, u8 intensity);
    u8 GetMoveVibration(int wand) const;
    void SetMoveLight(int wand, u8 red, u8 green, u8 blue);

    // Guest side, tracker space.
    DeviceState GetHead();
    DeviceState GetPad();
    /// A wand's place and how it is turned, as the title is to see it (MoveConfig applied).
    /// `tracked` is false until the host has seen that controller, and once it has been out of
    /// sight for a moment (the wand then stays where it was last seen).
    DeviceState GetMove(int wand);
    MoveInput GetMoveInput(int wand) const;

    /// Converts a tracker-space pose handed back by the guest into host space.
    Pose ToHostSpace(const Pose& tracker_pose) const;

    u32 NextFrameId() {
        return ++frame_counter;
    }
    void SetFrameListener(std::function<void(const PresentedFrame&)> listener);
    void NotifyFramePresented(const PresentedFrame& frame);

private:
    Runtime();

    DeviceState DemoHead() const;
    void RecenterSeatLocked();
    void PlaceHead();
    Vec3 PositionToTracker(const Vec3& host) const;
    Vec3 DirectionToTracker(const Vec3& host) const;

    Config config;
    std::atomic<bool> headset_worn{true};
    std::atomic<bool> view_gestures{};
    std::mutex gesture_mutex;
    bool gesture_seat_taken{};
    bool gesture_options_down{};
    bool gesture_options_fired{};
    std::chrono::steady_clock::time_point gesture_options_since;
    mutable std::mutex mutex;
    DeviceState head;
    // What the host last said of the head, in its own space, and the seat in that space.
    DeviceState host_head;
    // Where the head was a moment ago, and how fast that says it moves.
    Vec3 head_seen_position;
    std::chrono::steady_clock::time_point head_seen_time;
    bool head_seen{};
    Vec3 head_speed;
    Vec3 seat_position;
    Quat seat_yaw;
    bool seat_valid{};
    // The seat before the last reset of the view, for the frames that were drawn from it and
    // are only shown after.
    Vec3 previous_seat_position;
    Quat previous_seat_yaw;
    std::chrono::steady_clock::time_point seat_changed;
    bool title_asked{};
    DeviceState pad;
    bool pad_position_tracked{};
    // Smoothed point the untracked controller hangs off.
    Vec3 pad_anchor;
    bool pad_anchor_valid{};
    std::chrono::steady_clock::time_point pad_anchor_time;
    // Attitude estimated from the controller's motion sensors.
    Quat pad_attitude;
    Vec3 pad_acceleration;
    bool pad_acceleration_valid{};
    bool pad_attitude_valid{};
    std::chrono::steady_clock::time_point pad_motion_time;
    // Where the host last saw the controller, in tracker space.
    Vec3 pad_seen_position;
    Vec3 pad_seen_velocity;
    bool pad_seen{};
    // Where it was last seen relative to the anchor, to stay there once out of sight.
    Vec3 pad_seen_offset;
    bool pad_seen_offset_valid{};
    std::chrono::steady_clock::time_point pad_seen_time;
    // Where the controller is reported to be; follows the above without jumping.
    Vec3 pad_shown_position;
    bool pad_shown_valid{};
    std::chrono::steady_clock::time_point pad_shown_time;
    float pad_yaw_reference{};
    bool pad_yaw_reference_valid{};
    std::chrono::steady_clock::time_point pad_yaw_reference_time;
    DisplayRefresh display_refresh;
    // The host headset's field of view, once known.
    std::condition_variable headset_fov_known;
    Fov headset_fov;
    bool has_headset_fov{};
    // The Move wands, guarded by `mutex`.
    struct MoveWand {
        DeviceState host; ///< As the host last gave it, host space.
        MoveInput input;
        bool seen{};  ///< Ever.
        bool lost{};  ///< Out of the host's sight since it was last seen.
        std::chrono::steady_clock::time_point seen_time;
        u8 vibration{};
        u8 light[3]{};
    };
    MoveConfig move_config;
    MoveWand move_wands[NumMoveWands];
    PadFeedback pad_feedback;
    std::function<void(const PadFeedback&)> pad_feedback_listener;
    std::function<void(const PresentedFrame&)> frame_listener;
    u32 frame_counter{};
};

} // namespace Core::Vr
