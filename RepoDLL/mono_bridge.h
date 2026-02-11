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
  int item_type = -1;
  bool has_item_type = false;
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
bool MonoSetCartValueSafe(int value);
bool MonoSetGrabRange(float range);
bool MonoSetGrabStrengthField(float strength);

// Round/haul helpers (关卡收集阶段)
struct RoundState {
  bool ok{ false };
  int current{ 0 };
  int current_max{ 0 };
  int goal{ 0 };
  int stage{ -1 };  // 未解析则为 -1
};
struct RoundProgressState {
  bool ok{ false };
  int completed{ 0 };
  int total{ 0 };
  int stage{ -1 };
  bool all_completed{ false };
};
bool MonoGetRoundState(RoundState& out_state);
bool MonoSetRoundState(int current, int goal = -1, int current_max = -1);
bool MonoSetRoundStateSafe(int current, int goal = -1, int current_max = -1);
bool MonoGetRoundProgress(RoundProgressState& out_state);
bool MonoSetRoundProgress(int completed, int total, int stage = -1, int all_completed = -1);
bool MonoSetRoundProgressSafe(int completed, int total, int stage = -1, int all_completed = -1);
void MonoSetRoundHaulOverride(bool enabled, int current, int goal = -1);

// Enumerate all PlayerAvatar instances; fills out_states with any player that has a position.
bool MonoListPlayers(std::vector<PlayerState>& out_states, bool include_local);

// Enumerate items/valuables; fills out_items with positions (health/energy unused).
bool MonoListItems(std::vector<PlayerState>& out_items);
// SEH-safe wrapper for overlay thread
bool MonoListItemsSafe(std::vector<PlayerState>& out_items);

// Enumerate enemies (any hostile entity with transform).
bool MonoListEnemies(std::vector<PlayerState>& out_enemies);
// SEH-safe wrapper for overlay thread
bool MonoListEnemiesSafe(std::vector<PlayerState>& out_enemies);

// Diagnostics / manual controls
bool MonoItemsDisabled();
void MonoResetItemsDisabled();
bool MonoManualRefreshItems(std::vector<PlayerState>& out_items);
void MonoResetEnemiesDisabled();
bool MonoScanMethods(const char* keyword, std::vector<std::string>& out_results);
bool MonoScanMethodsWithBytes(const char* keyword, std::vector<std::string>& out_results);
bool MonoDumpCollectorNumericFields(std::vector<std::string>& out_results);
bool MonoProbeCollectorMethods(const char* keyword, std::vector<std::string>& out_results);
bool MonoPatchCollectorGetters(int forced_value, std::vector<std::string>& out_results);
bool MonoRestoreCollectorGetterPatches(std::vector<std::string>& out_results);
bool MonoReviveAllPlayers(bool include_local);
bool MonoGetLogs(int max_lines, std::vector<std::string>& out_logs);
const std::string& MonoGetLogPath();
void MonoSetLogPath(const std::string& path_utf8);

extern bool g_enemy_esp_disabled;
extern bool g_enemy_esp_enabled;
extern bool g_items_disabled;
extern bool g_enemy_cache_disabled;
extern bool g_item_esp_enabled;
extern int g_item_esp_cap;
extern int g_enemy_esp_cap;

// ESP开关（在 hook_dx11.cpp 定义）
extern bool g_esp_enabled;
// Overlay 状态（在 hook_dx11.cpp 定义）
extern bool g_overlay_disabled;
