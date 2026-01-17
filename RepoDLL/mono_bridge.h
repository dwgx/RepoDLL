#pragma once

#include <cstdint>
#include <vector>

struct LocalPlayerInfo {
  void* object = nullptr;
  bool is_local = false;
  bool via_player_list = false;
};

struct PlayerState {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool is_local = false;
  int health = 0;
  int max_health = 0;
  float energy = 0.0f;
  float max_energy = 0.0f;
  bool has_position = false;
  bool has_health = false;
  bool has_energy = false;
};

struct Matrix4x4 {
  float m[16]{};
};

bool MonoInitialize();
bool MonoGetLocalPlayer(LocalPlayerInfo& out_info);
bool MonoGetLocalPlayerState(PlayerState& out_state);
bool MonoGetPlayerStateFromAvatar(void* player_avatar_obj, PlayerState& out_state);
bool MonoBeginShutdown();
bool MonoIsShuttingDown();
long LogCrash(const char* where, unsigned long code, struct _EXCEPTION_POINTERS* info);
bool MonoSetLocalPlayerPosition(float x, float y, float z);
bool MonoSetLocalPlayerHealth(int health, int max_health);
bool MonoSetLocalPlayerEnergy(float energy, float max_energy);
bool MonoGetCameraMatrices(Matrix4x4& view, Matrix4x4& projection);

bool MonoSetRunCurrency(int amount);
bool MonoGetRunCurrency(int& out_amount);
bool MonoApplyPendingCartValue();
bool MonoOverrideSpeed(float multiplier, float duration_seconds);
bool MonoUpgradeExtraJump(int count);
bool MonoOverrideJumpCooldown(float seconds);
bool MonoSetInvincible(float duration_seconds);
bool MonoSetGrabStrength(int grab_strength, int throw_strength);
bool MonoSetJumpExtraDirect(int jump_count);
bool MonoSetSpeedMultiplierDirect(float multiplier, float duration_seconds);
bool MonoSetJumpForce(float force);
bool MonoSetCartValue(int value);
bool MonoSetGrabRange(float range);
bool MonoSetGrabStrengthField(float strength);

// Enumerate all PlayerAvatar instances; fills out_states with any player that has a position.
bool MonoListPlayers(std::vector<PlayerState>& out_states, bool include_local);

// Enumerate items/valuables; fills out_items with positions (health/energy unused).
bool MonoListItems(std::vector<PlayerState>& out_items);
