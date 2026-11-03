/**
 * @file        web/web_platform.cpp
 * @brief       [web port] Platform services GTA IV's glue gets per platform
 *
 * Each host supplies these from its own files (Game Center and music on
 * Apple, JNI on Android, the hub on the PS5). The browser has none of them
 * yet, so they report "not available": no achievements service, no user
 * music, no microphone, and no community multiplayer backend (that one needs
 * libcurl; a browser build would talk to the relay over WebSockets).
 *
 * @license     BSD 3-Clause License
 */

#include <rex/cvar.h>

#include "achievement_bridge_gc.h"
#include "input/user_music_player.h"
#include "network/community_multiplayer.h"
#include "network/gta4_microphone_permission.h"

namespace gta4::game_center {

void Initialize() {}

void SubmitAchievement(uint32_t xbox_id) {
  (void)xbox_id;
}

}  // namespace gta4::game_center

namespace gta4::input {

struct UserMusicPlayer::Impl {};

UserMusicPlayer::UserMusicPlayer(std::filesystem::path music_root) {
  (void)music_root;
}

UserMusicPlayer::~UserMusicPlayer() = default;

bool UserMusicPlayer::Next() {
  return false;
}

bool UserMusicPlayer::Previous() {
  return false;
}

bool UserMusicPlayer::Stop() {
  return false;
}

bool UserMusicPlayer::HasTracks() const {
  return false;
}

void PublishUserMusicPlayer(UserMusicPlayer* player) {
  (void)player;
}

bool IsUserMusicAvailable() {
  return false;
}

bool RequestUserMusicNext() {
  return false;
}

bool RequestUserMusicPrevious() {
  return false;
}

bool RequestUserMusicStop() {
  return false;
}

}  // namespace gta4::input

namespace gta4::voice {

MicrophonePermissionStatus GetMicrophonePermissionStatus() noexcept {
  return MicrophonePermissionStatus::kRestricted;
}

void RequestMicrophonePermission(std::function<void()> completion) {
  if (completion) {
    completion();
  }
}

}  // namespace gta4::voice

namespace LibertyRecomp::Network {

rex::system::xam::LiveBackendServices CreateCommunityMultiplayerBackend(
    const rex::system::xam::LiveConfig& config, const rex::system::xam::LiveIdentity& identity) {
  (void)config;
  (void)identity;
  return {};
}

}  // namespace LibertyRecomp::Network

// The FSR 1 presenter that owns this setting is not built without a renderer.
REXCVAR_DEFINE_DOUBLE(present_fsr_sharpness_reduction, 0.2, "Display",
                      "FSR 1 sharpness reduction (unused on the web until a presenter exists)");

// The recompiled code's multiply-adds (PPC fmadd and kin: std::fma, some
// fifteen thousand sites). WebAssembly has no fused multiply-add, so the C
// library's fma is a software routine exact to the last bit - tens of
// nanoseconds a call, and the game makes millions a second: a third of the
// audio thread and a sizeable part of the others on a slow CPU. These take
// the place of the library's (the linker uses an object's definition before
// an archive's): a multiply and an add, unfused. For the single-precision
// instructions most of the game uses, inputs are floats whose product a
// double holds exactly, so the result rounds as the fused one would; the
// vector multiply-adds (simde) were already unfused here.
#pragma clang fp contract(off)
extern "C" double fma(double a, double b, double c) {
  return a * b + c;
}
extern "C" float fmaf(float a, float b, float c) {
  return float(double(a) * double(b) + double(c));
}
#pragma clang fp contract(on)
