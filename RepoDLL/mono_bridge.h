#pragma once

#include <cstdint>
#include <vector>
#include <string>

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
  int layer = -1;
  bool has_layer = false;
  std::string name;
  bool has_name = false;
  int value = 0;
  bool has_value = false;
  enum Category : int {
    kUnknown = 0,
    kValuable = 1,
    kPhysGrab = 2,
    kVolume = 3,
    kCollider = 4,
    kEnemy = 5,
  } category = kUnknown;
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

// Native in-game highlight (ValuableDiscover)
bool MonoValueFieldsResolved();
bool MonoTriggerValuableDiscover(int state, int max_items, int& out_count);
bool MonoApplyValuableDiscoverPersistence(bool enable, float wait_seconds, int& out_count);
// SEH 包装，防止崩溃
bool MonoTriggerValuableDiscoverSafe(int state, int max_items, int& out_count);
bool MonoApplyValuableDiscoverPersistenceSafe(bool enable, float wait_seconds, int& out_count);
bool MonoNativeHighlightAvailable();

extern bool g_native_highlight_failed;
extern bool g_native_highlight_active;

// 目前在 HookPresent / RenderOverlay 中设置，用于在崩溃日志里记录阶段
void SetCrashStage(const char* stage);

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

// Enumerate enemies (any hostile entity with transform).
bool MonoListEnemies(std::vector<PlayerState>& out_enemies);
// SEH-safe wrapper for overlay thread
bool MonoListEnemiesSafe(std::vector<PlayerState>& out_enemies);

extern bool g_enemy_esp_disabled;
extern bool g_enemy_esp_enabled;

// ESP开关（在 hook_dx11.cpp 定义）
extern bool g_esp_enabled;
// Overlay 状态（在 hook_dx11.cpp 定义）
extern bool g_overlay_disabled;
