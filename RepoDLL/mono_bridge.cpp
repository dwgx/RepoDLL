#include "pch.h"

#include "mono_bridge.h"

#include <cstring>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <sstream>
#include <string>
#include <algorithm>
#include <cmath>
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <psapi.h>
#include <vector>
#include <functional>
#include <mutex>
#include <deque>
#include <atomic>
#include <filesystem>
#include "MinHook.h"

#include "config.h"

extern bool g_esp_enabled;
extern bool g_overlay_disabled;

bool g_native_highlight_failed = false;
bool g_native_highlight_active = false;
bool g_enemy_esp_disabled = false;
bool g_enemy_esp_enabled = false;
bool g_items_disabled = false;
bool g_enemy_cache_disabled = false;
bool g_item_esp_enabled = false;
int g_item_esp_cap = 65;
int g_enemy_esp_cap = 8;
bool g_session_master_patch_active = false;

namespace {
  using MonoDomain = void;
  using MonoAssembly = void;
  using MonoImage = void;
  using MonoClass = void;
  using MonoClassField = void;
  using MonoType = void;
  using MonoMethod = void;
  using MonoMethodSignature = void;
  using MonoObject = void;
  using MonoThread = void;
  using MonoVTable = void;

  struct MonoApi {
    HMODULE module = nullptr;
    MonoDomain* (__cdecl* mono_get_root_domain)() = nullptr;
    MonoThread* (__cdecl* mono_thread_attach)(MonoDomain*) = nullptr;
    void(__cdecl* mono_assembly_foreach)(void (*)(MonoAssembly*, void*), void*) = nullptr;
    MonoImage* (__cdecl* mono_assembly_get_image)(MonoAssembly*) = nullptr;
    const char* (__cdecl* mono_image_get_name)(MonoImage*) = nullptr;
    MonoClass* (__cdecl* mono_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
    MonoVTable* (__cdecl* mono_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
    MonoClassField* (__cdecl* mono_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
    MonoMethod* (__cdecl* mono_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
    MonoMethod* (__cdecl* mono_class_get_methods)(MonoClass*, void**) = nullptr;
    MonoClassField* (__cdecl* mono_class_get_fields)(MonoClass*, void**) = nullptr;
    MonoType* (__cdecl* mono_class_get_type)(MonoClass*) = nullptr;
    int(__cdecl* mono_class_is_subclass_of)(MonoClass*, MonoClass*, bool) = nullptr;
    MonoType* (__cdecl* mono_field_get_type)(MonoClassField*) = nullptr;
    int(__cdecl* mono_type_get_type)(MonoType*) = nullptr;
    const char* (__cdecl* mono_field_get_name)(MonoClassField*) = nullptr;
    MonoMethodSignature* (__cdecl* mono_method_signature)(MonoMethod*) = nullptr;
    uint32_t(__cdecl* mono_signature_get_param_count)(MonoMethodSignature*) = nullptr;
    MonoType* (__cdecl* mono_signature_get_return_type)(MonoMethodSignature*) = nullptr;
    MonoObject* (__cdecl* mono_type_get_object)(MonoDomain*, MonoType*) = nullptr;
    MonoObject* (__cdecl* mono_runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
    void(__cdecl* mono_field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void(__cdecl* mono_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    void(__cdecl* mono_field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    MonoClass* (__cdecl* mono_object_get_class)(MonoObject*) = nullptr;
    void* (__cdecl* mono_object_unbox)(MonoObject*) = nullptr;
    int(__cdecl* mono_field_get_offset)(MonoClassField*) = nullptr;
    MonoObject* (__cdecl* mono_string_new)(MonoDomain*, const char*) = nullptr;
    char* (__cdecl* mono_string_to_utf8)(MonoObject*) = nullptr;
    void(__cdecl* mono_free)(void*) = nullptr;
    void* (__cdecl* mono_compile_method)(MonoMethod*) = nullptr;
    const char* (__cdecl* mono_method_get_name)(MonoMethod*) = nullptr;
    uint32_t(__cdecl* mono_gchandle_new)(MonoObject*, int) = nullptr;
    void(__cdecl* mono_gchandle_free)(uint32_t) = nullptr;
    MonoObject* (__cdecl* mono_gchandle_get_target)(uint32_t) = nullptr;
  };

  struct MonoArray {
    void* vtable;
    void* synchronization;
    void* bounds;
    size_t max_length;
    void* vector[1];
  };

  // Helper: validate managed array before dereference
  inline bool IsValidArray(const MonoArray* arr) {
    return arr && arr->vector && arr->max_length > 0;
  }

  // Forward declaration for SafeInvoke (defined later).
  static MonoObject* SafeInvoke(MonoMethod* method, MonoObject* obj, void** args, const char* tag);
  // Forward declarations for enemy cache helpers / hooks
  static bool EnsureMinHookReady();
  static bool IsUnityNull(MonoObject* obj);
  static void EnemyCacheAdd(MonoObject* obj);
  static void EnemyCachePruneDead();
  static void __stdcall EnemyAwakeHook(MonoObject* self);
  static void InstallEnemyAwakeHook();
  static void __stdcall RoundDirectorUpdateHook(MonoObject* self);
  static void InstallRoundDirectorUpdateHook();
  static bool WriteFieldNumber(MonoObject* obj, MonoClassField* field, int value);
  static bool ReadFieldNumber(MonoObject* obj, MonoClassField* field, int& out_v);
  static bool ComputeRunCurrencyBeforeForTarget(int target_haul, int& out_before);
  struct CodePatchBackup;
  static bool PatchCodeToReturnInt(void* addr, int forced, CodePatchBackup& backup);
  static bool RestorePatchedCode(void* addr, const CodePatchBackup& backup);
  static bool SetRoundUpdateBypass(bool enable);
  static bool ApplySessionMasterPatches();
  static bool RestoreSessionMasterPatches();
#ifdef _MSC_VER
  static bool FieldGetBoolSafeSeh(MonoObject* obj, MonoClassField* field, bool& out_value);
#endif

  MonoApi g_mono;
  MonoDomain* g_domain = nullptr;
  DWORD g_attached_thread_id = 0;

  MonoImage* g_image = nullptr;
  MonoClass* g_game_director_class = nullptr;
  MonoVTable* g_game_director_vtable = nullptr;
  MonoClassField* g_game_director_instance_field = nullptr;
  MonoClassField* g_game_director_player_list_field = nullptr;
  MonoClassField* g_game_director_current_state_field = nullptr;

  MonoClass* g_player_avatar_class = nullptr;
  MonoVTable* g_player_avatar_vtable = nullptr;
  MonoClassField* g_player_avatar_is_local_field = nullptr;
  MonoClassField* g_player_avatar_instance_field = nullptr;
  MonoClassField* g_player_avatar_transform_field = nullptr;
  MonoClassField* g_player_avatar_health_field = nullptr;
  MonoClassField* g_player_avatar_steamid_field = nullptr;
  MonoClassField* g_player_avatar_physgrabber_field = nullptr;

  MonoClass* g_player_health_class = nullptr;
  MonoClassField* g_player_health_value_field = nullptr;
  MonoClassField* g_player_health_max_field = nullptr;

  // Stamina lives on PlayerController (EnergyCurrent/EnergyStart).
  MonoClass* g_player_controller_class = nullptr;
  MonoVTable* g_player_controller_vtable = nullptr;
  MonoClassField* g_player_controller_instance_field = nullptr;
  MonoClassField* g_player_controller_energy_current_field = nullptr;
  MonoClassField* g_player_controller_energy_start_field = nullptr;
  MonoClassField* g_player_controller_jump_extra_field = nullptr;
  MonoClassField* g_player_controller_override_speed_multiplier_field = nullptr;
  MonoClassField* g_player_controller_override_speed_timer_field = nullptr;
  MonoClassField* g_player_controller_jump_force_field = nullptr;

  // Camera
  MonoImage* g_unity_image = nullptr;
  MonoClass* g_unity_camera_class = nullptr;
  MonoMethod* g_unity_camera_get_main = nullptr;
  MonoMethod* g_unity_camera_get_projection_matrix = nullptr;
  MonoMethod* g_unity_camera_get_world_to_camera_matrix = nullptr;
  MonoMethod* g_unity_camera_get_transform = nullptr;
  MonoClass* g_component_class = nullptr;
  MonoMethod* g_component_get_transform = nullptr;
  MonoMethod* g_component_get_game_object = nullptr;

  MonoClass* g_game_object_class = nullptr;
  MonoMethod* g_game_object_get_component = nullptr;
  MonoMethod* g_game_object_get_component_in_parent = nullptr;
  MonoMethod* g_game_object_get_layer = nullptr;

  MonoClass* g_unity_object_class = nullptr;
  MonoMethod* g_unity_object_get_name = nullptr;
  MonoMethod* g_unity_object_op_eq = nullptr;

  MonoClass* g_transform_class = nullptr;
  MonoMethod* g_transform_get_local_to_world = nullptr;
  MonoMethod* g_transform_get_world_to_local = nullptr;

  MonoClass* g_physics_class = nullptr;
  MonoMethod* g_physics_overlap_sphere = nullptr;
  int g_physics_overlap_sphere_argc = 0;
  MonoClass* g_layer_mask_class = nullptr;
  MonoMethod* g_layer_mask_name_to_layer = nullptr;
  MonoClass* g_collider_class = nullptr;

  MonoClass* g_semi_func_class = nullptr;
  MonoMethod* g_player_avatar_local_method = nullptr;
  MonoMethod* g_player_get_all_method = nullptr;

  // Item system
  MonoClass* g_item_manager_class = nullptr;
  MonoVTable* g_item_manager_vtable = nullptr;
  MonoClassField* g_item_manager_instance_field = nullptr;
  MonoClassField* g_item_manager_item_volumes_field = nullptr;
  MonoClass* g_phys_grab_object_class = nullptr;
  MonoClassField* g_item_manager_spawned_items_field = nullptr;
  MonoMethod* g_item_manager_get_all_items = nullptr;
  int g_item_manager_get_all_items_argc = 0;

  // Alternative item population methods
  MonoMethod* g_semi_func_shop_populate = nullptr;
  MonoMethod* g_semi_func_truck_populate = nullptr;
  MonoMethod* g_find_objects_of_type_itemvolume = nullptr;
  MonoMethod* g_find_objects_of_type_itemvolume_include_inactive = nullptr;
  MonoMethod* g_object_find_objects_of_type = nullptr;                  // Object.FindObjectsOfType(Type)
  MonoMethod* g_object_find_objects_of_type_include_inactive = nullptr; // Object.FindObjectsOfType(Type,bool)
  MonoClass* g_resources_class = nullptr;
  MonoMethod* g_resources_find_objects_of_type_all = nullptr;
  MonoClass* g_item_volume_class = nullptr;
  MonoClassField* g_item_volume_item_attributes_field = nullptr;
  MonoClass* g_item_attributes_class = nullptr;
  MonoClassField* g_item_attributes_item_field = nullptr;
  MonoClassField* g_item_attributes_value_field = nullptr;
  MonoClassField* g_item_attributes_item_type_field = nullptr;
  MonoClassField* g_item_attributes_item_name_field = nullptr;
  MonoClassField* g_item_attributes_instance_name_field = nullptr;
  MonoObject* g_item_attributes_type_obj = nullptr;
  MonoClass* g_item_class = nullptr;
  MonoClassField* g_item_item_name_field = nullptr;

  MonoClass* g_valuable_object_class = nullptr;
  MonoClassField* g_valuable_object_value_field = nullptr;
  MonoObject* g_valuable_object_type_obj = nullptr;
  MonoClass* g_enemy_rigidbody_class = nullptr;
  std::vector<uint32_t> g_enemy_cache;  // GCHandles
  std::mutex g_enemy_cache_mutex;
  uint64_t g_enemy_cache_last_refresh = 0;
  const uint64_t k_enemy_cache_interval_ms = 2000;  // 2s low-frequency refresh
  MonoMethod* g_enemy_awake_method = nullptr;
  using EnemyAwakeFn = void(*)(MonoObject*);
  EnemyAwakeFn g_enemy_awake_orig = nullptr;
  bool g_enemy_awake_hooked = false;
  MonoMethod* g_round_director_update_method = nullptr;
  using RoundDirectorUpdateFn = void(*)(MonoObject*);
  RoundDirectorUpdateFn g_round_director_update_orig = nullptr;
  bool g_round_director_update_hooked = false;
  std::atomic<bool> g_force_round_haul_enabled{ false };
  std::atomic<int> g_force_round_haul_value{ 0 };
  std::atomic<int> g_force_round_haul_goal{ -1 };
  MonoClass* g_valuable_director_class = nullptr;
  MonoVTable* g_valuable_director_vtable = nullptr;
    MonoClassField* g_valuable_director_instance_field = nullptr;
    MonoClassField* g_valuable_director_list_field = nullptr;
    MonoClass* g_valuable_discover_class = nullptr;
    MonoVTable* g_valuable_discover_vtable = nullptr;
    MonoClassField* g_valuable_discover_instance_field = nullptr;
    MonoClassField* g_valuable_discover_hide_timer_field = nullptr;
    MonoClassField* g_valuable_discover_hide_alpha_field = nullptr;
    MonoMethod* g_valuable_discover_new_method = nullptr;
    MonoMethod* g_valuable_discover_hide_method = nullptr;

  // LightInteractableFadeRemove (in-game 1s highlight)
  MonoClass* g_light_fade_class = nullptr;
  MonoType* g_light_fade_type = nullptr;
  MonoClassField* g_light_fade_is_fading_field = nullptr;
  MonoClassField* g_light_fade_current_time_field = nullptr;
  MonoClassField* g_light_fade_fade_duration_field = nullptr;

  // Currency / movement / combat helpers
  MonoMethod* g_semi_func_stat_set_run_currency = nullptr;
  MonoMethod* g_semi_func_stat_get_run_currency = nullptr;
  MonoMethod* g_semi_func_is_not_master_client = nullptr;
  MonoMethod* g_semi_func_is_master_client = nullptr;
  MonoMethod* g_semi_func_is_master_client_or_singleplayer = nullptr;
  MonoMethod* g_semi_func_master_only_rpc = nullptr;
  MonoMethod* g_semi_func_owner_only_rpc = nullptr;
  MonoMethod* g_semi_func_master_and_owner_only_rpc = nullptr;
  MonoClass* g_photon_network_class = nullptr;
  MonoMethod* g_photon_network_get_is_master_client = nullptr;
  MonoMethod* g_photon_network_get_local_player = nullptr;
  MonoMethod* g_photon_network_get_master_client = nullptr;
  MonoMethod* g_photon_network_get_in_room = nullptr;
  MonoMethod* g_photon_network_get_level_loading_progress = nullptr;
  MonoClass* g_photon_player_class = nullptr;
  MonoMethod* g_photon_player_get_actor_number = nullptr;

  // Session/load transition guards
  MonoClass* g_run_manager_class = nullptr;
  MonoVTable* g_run_manager_vtable = nullptr;
  MonoClassField* g_run_manager_instance_field = nullptr;
  MonoClassField* g_run_manager_restarting_field = nullptr;
  MonoClassField* g_run_manager_wait_to_change_scene_field = nullptr;
  MonoClassField* g_run_manager_lobby_join_field = nullptr;
  MonoClass* g_level_generator_class = nullptr;
  MonoVTable* g_level_generator_vtable = nullptr;
  MonoClassField* g_level_generator_instance_field = nullptr;
  MonoClassField* g_level_generator_state_field = nullptr;
  MonoClassField* g_level_generator_generated_field = nullptr;

  // StatsManager
  MonoClass* g_stats_manager_class = nullptr;
  MonoVTable* g_stats_manager_vtable = nullptr;
  MonoClassField* g_stats_manager_instance_field = nullptr;
  MonoClassField* g_stats_manager_run_stats_field = nullptr;
  MonoMethod* g_dict_set_item = nullptr;

  // ShopManager (item volumes)
  MonoClass* g_shop_manager_class = nullptr;
  MonoVTable* g_shop_manager_vtable = nullptr;
  MonoClassField* g_shop_manager_instance_field = nullptr;
  MonoClassField* g_shop_secret_item_volumes_field = nullptr;

  MonoClass* g_pun_manager_class = nullptr;
  MonoMethod* g_pun_upgrade_extra_jump = nullptr;
  MonoMethod* g_pun_upgrade_grab_strength = nullptr;
  MonoMethod* g_pun_upgrade_throw_strength = nullptr;
  MonoMethod* g_pun_set_run_stat_set = nullptr;
  MonoClassField* g_pun_manager_instance_field = nullptr;
  MonoVTable* g_pun_manager_vtable = nullptr;

// Non-ASCII comment normalized.
  MonoClass* g_round_director_class = nullptr;
  MonoVTable* g_round_director_vtable = nullptr;
  MonoClassField* g_round_director_instance_field = nullptr;
  MonoClassField* g_round_current_haul_field = nullptr;
  MonoClassField* g_round_current_haul_max_field = nullptr;
  MonoClassField* g_round_haul_goal_field = nullptr;
  MonoClassField* g_round_total_haul_field = nullptr;
  MonoClassField* g_round_haul_goal_max_field = nullptr;
  MonoClassField* g_round_extraction_point_surplus_field = nullptr;
  MonoClassField* g_round_extraction_haul_goal_field = nullptr;
  MonoClassField* g_round_extraction_points_field = nullptr;
  MonoClassField* g_round_extraction_point_current_field = nullptr;
  MonoClassField* g_round_dollar_haul_list_field = nullptr;
  MonoClassField* g_round_extraction_points_completed_field = nullptr;
  MonoClassField* g_round_all_extraction_points_completed_field = nullptr;
  MonoClassField* g_round_stage_field = nullptr;  // Non-ASCII comment normalized.
  MonoMethod* g_round_set_current_haul_method = nullptr;
  MonoMethod* g_round_set_haul_goal_method = nullptr;
  MonoMethod* g_round_apply_haul_method = nullptr;
  MonoMethod* g_round_refresh_haul_method = nullptr;
  MonoMethod* g_player_controller_override_speed = nullptr;
  MonoMethod* g_player_controller_override_jump_cooldown = nullptr;

  MonoMethod* g_player_health_invincible_set = nullptr;

  // Currency UI
  MonoClass* g_currency_ui_class = nullptr;
  MonoMethod* g_currency_ui_fetch = nullptr;

  // Handcart
  MonoClass* g_phys_grab_cart_class = nullptr;
  MonoClassField* g_phys_grab_cart_haul_field = nullptr;
  MonoMethod* g_phys_grab_cart_set_haul_text = nullptr;

  // PhysGrabber
  MonoClass* g_phys_grabber_class = nullptr;
  MonoClassField* g_phys_grabber_range_field = nullptr;
  MonoClassField* g_phys_grabber_strength_field = nullptr;

  // Extraction point
  MonoClass* g_extraction_point_class = nullptr;
  MonoClassField* g_extraction_point_haul_goal_field = nullptr;
  MonoClassField* g_extraction_point_haul_current_field = nullptr;
  MonoClassField* g_extraction_point_extraction_haul_field = nullptr;
  MonoClassField* g_extraction_point_run_currency_before_field = nullptr;
  MonoMethod* g_extraction_point_set_current_haul_method = nullptr;
  MonoMethod* g_extraction_point_set_haul_goal_method = nullptr;
  MonoMethod* g_extraction_point_apply_haul_method = nullptr;
  MonoMethod* g_extraction_point_set_haul_text_method = nullptr;
  MonoMethod* g_extraction_point_refresh_method = nullptr;

  // Visual / gamma / post-processing
  // Pending cart value
  int g_pending_cart_value = 0;
  bool g_pending_cart_active = false;
  bool g_cart_apply_in_progress = false;

  // Shutdown guard
  bool g_shutting_down = false;
  constexpr bool k_enable_enemy_awake_hook = false;
  // Disabled by default: we use byte patch fallback for RoundDirector::Update to avoid hook misses.
  constexpr bool k_enable_round_update_hook = false;
  // Experimental method calls on RoundDirector/ExtractionPoint are crash-prone on some builds.
  // Keep disabled by default; rely on field writes for stability.
  constexpr bool k_enable_experimental_haul_method_calls = false;
  struct CodePatchBackup {
    std::array<uint8_t, 16> bytes{};
    size_t size{ 0 };
  };
  std::unordered_map<void*, CodePatchBackup> g_collector_getter_patches;
  std::unordered_map<void*, CodePatchBackup> g_session_master_backups;
  void* g_round_update_patch_addr = nullptr;
  CodePatchBackup g_round_update_patch_backup{};
  bool g_round_update_patch_active = false;

  void ClearEnemyCacheHandlesUnlocked() {
    if (!g_mono.mono_gchandle_free) {
      g_enemy_cache.clear();
      return;
    }
    for (uint32_t h : g_enemy_cache) {
      g_mono.mono_gchandle_free(h);
    }
    g_enemy_cache.clear();
  }

  void ClearEnemyCacheHandles() {
    std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
    ClearEnemyCacheHandlesUnlocked();
  }

  // ------------------------------------------------------------
  // Structured logging (thread-safe, low-noise, ring-buffered)
  // ------------------------------------------------------------
  enum class LogLevel : int { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3, kTrace = 4 };

  struct LogEntry {
    std::string ts;
    std::string stage;
    std::string msg;
    LogLevel level{ LogLevel::kInfo };
    uint32_t tid{ 0 };
  };

  constexpr size_t kLogRingCap = 256;
  std::string g_log_path;
  std::string DefaultLogPath() {
    char* buf = nullptr;
    size_t sz = 0;
    std::string base = "C:";
#if defined(_WIN32)
    if (_dupenv_s(&buf, &sz, "USERPROFILE") == 0 && buf) {
      base.assign(buf);
      free(buf);
    }
#else
    const char* user = std::getenv("HOME");
    if (user) base.assign(user);
#endif
    std::string path = base + "\\AppData\\LocalLow\\semiwork\\Repo\\repodll\\REPO_LOG.txt";
    return path;
  }
  void EnsureLogDir(const std::string& path) {
    try {
      std::filesystem::path p(path);
      std::filesystem::create_directories(p.parent_path());
    }
    catch (...) {
    }
  }
  void EnsureLogPath() {
    if (!g_log_path.empty()) return;
    g_log_path = DefaultLogPath();
    EnsureLogDir(g_log_path);
  }
  std::string CrashPath() {
    EnsureLogPath();
    std::filesystem::path p(g_log_path);
    return (p.parent_path() / "REPO_CRASH.txt").string();
  }

  std::mutex g_log_mutex;
  std::deque<LogEntry> g_log_ring;
  std::unordered_set<std::string> g_log_once;
  std::atomic<int> g_log_level(static_cast<int>(LogLevel::kInfo));
  std::atomic<bool> g_env_logged{false};
  std::atomic<const char*> g_crash_stage{"init"};
  std::atomic<bool> g_items_ready{false};
  std::atomic<bool> g_enemies_ready{false};
  std::atomic<uint64_t> g_items_last_crash_ms{0};

  std::string NowString() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_local = {};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%F %T") << "." << std::setw(3) << std::setfill('0')
      << ms.count();
    return oss.str();
  }

  void WriteLogLineUnlocked(const LogEntry& e) {
    EnsureLogPath();
    std::ofstream file(g_log_path, std::ios::app);
    if (!file) return;
    file << "[" << e.ts << "] "
         << "L" << static_cast<int>(e.level) << " "
         << "T" << e.tid << " "
         << "[" << e.stage << "] "
         << e.msg << "\n";
  }

  void AppendLogInternal(LogLevel lvl, const char* stage, const std::string& message) {
    if (static_cast<int>(lvl) > g_log_level.load(std::memory_order_relaxed)) return;
    LogEntry e{};
    e.ts = NowString();
    e.stage = stage ? stage : "general";
    e.msg = message;
    e.level = lvl;
#if defined(_WIN32)
    e.tid = GetCurrentThreadId();
#else
    e.tid = static_cast<uint32_t>(::getpid());
#endif
    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      g_log_ring.push_back(e);
      if (g_log_ring.size() > kLogRingCap) g_log_ring.pop_front();
      WriteLogLineUnlocked(e);
    }
  }

  void AppendLog(const std::string& message) {
    AppendLogInternal(LogLevel::kInfo, "legacy", message);
  }

  bool AppendLogOnce(const std::string& key, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_once.insert(key).second) {
      LogEntry e{NowString(), "legacy", message, LogLevel::kInfo,
#if defined(_WIN32)
        GetCurrentThreadId()
#else
        static_cast<uint32_t>(::getpid())
#endif
      };
      g_log_ring.push_back(e);
      if (g_log_ring.size() > kLogRingCap) g_log_ring.pop_front();
      WriteLogLineUnlocked(e);
      return true;
    }
    return false;
  }

  void SetLogLevel(LogLevel lvl) {
    g_log_level.store(static_cast<int>(lvl), std::memory_order_relaxed);
  }

  const std::string& InternalGetLogPath() {
    EnsureLogPath();
    return g_log_path;
  }

  void InternalSetLogPath(const std::string& path_utf8) {
    if (path_utf8.empty()) return;
    g_log_path = path_utf8;
    EnsureLogDir(g_log_path);
  }

  bool GetLogSnapshot(int max_lines, std::vector<std::string>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(g_log_mutex);
    int n = static_cast<int>(g_log_ring.size());
    if (n == 0) return true;
    int start = n > max_lines ? n - max_lines : 0;
    for (int i = start; i < n; ++i) {
      const auto& e = g_log_ring[i];
      std::ostringstream oss;
      oss << "[" << e.ts << "] "
        << "L" << static_cast<int>(e.level) << " "
        << "T" << e.tid << " "
        << "[" << e.stage << "] "
        << e.msg;
      out.push_back(oss.str());
    }
    return true;
  }

  // Crash report dumps last N log lines for diagnosis.
  void WriteCrashReport(const char* where, unsigned long code, EXCEPTION_POINTERS* info) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream f(CrashPath(), std::ios::app);
    if (!f) {
      AppendLogInternal(LogLevel::kError, "crash", "WriteCrashReport: cannot open crash file");
      return;
    }
    f << "=== Crash ===\n";
    f << "when: " << (where ? where : "<unknown>") << "\n";
    f << "code: 0x" << std::hex << code << std::dec << "\n";
    f << "addr: 0x"
      << (info && info->ExceptionRecord
            ? reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)
            : 0)
      << "\n";
    f << "thread: ";
#if defined(_WIN32)
    f << GetCurrentThreadId();
#else
    f << ::getpid();
#endif
    f << "\n";
    f << "stage: " << g_crash_stage.load(std::memory_order_relaxed) << "\n";
    f << "last_logs:\n";
    for (const auto& e : g_log_ring) {
      f << " [" << e.ts << "] L" << static_cast<int>(e.level) << " T" << e.tid
        << " [" << e.stage << "] " << e.msg << "\n";
    }
    f << "=== End Crash ===\n\n";
  }

  void LogEnvironmentOnce() {
    bool expected = false;
    if (!g_env_logged.compare_exchange_strong(expected, true)) return;
#if defined(_WIN32)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    std::ostringstream oss;
    oss << "Env: CPU=" << si.dwNumberOfProcessors
        << " PageSize=" << si.dwPageSize
        << " RAM=" << (ms.ullTotalPhys / (1024 * 1024)) << "MB"
        << " LogLevel=" << g_log_level.load();
    AppendLogInternal(LogLevel::kInfo, "env", oss.str());
#else
    AppendLogInternal(LogLevel::kInfo, "env", "Environment logging not implemented on this platform");
#endif
  }

  bool IsSubclassOf(MonoClass* klass, MonoClass* parent) {
    if (!klass || !parent || !g_mono.mono_class_is_subclass_of) return false;
    return g_mono.mono_class_is_subclass_of(klass, parent, false) != 0;
  }

  std::string MonoStringToUtf8(MonoObject* str) {
    if (!str || !g_mono.mono_string_to_utf8) {
      return {};
    }
    char* utf8 = g_mono.mono_string_to_utf8(str);
    if (!utf8) {
      return {};
    }
    std::string out(utf8);
    if (g_mono.mono_free) {
      g_mono.mono_free(utf8);
    }
    return out;
  }

  void PatchAntiMasterChecks() {
    static bool patched = false;
    static bool warned_once = false;
    if (patched) {
      return;
    }

    // Try direct module first
    HMODULE mod = GetModuleHandleW(L"Assembly-CSharp.dll");

    // If not loaded yet, skip to avoid heavy scans that can stutter; we'll patch after load.
    if (!mod) {
      if (!warned_once) {
        AppendLog("PatchAntiMasterChecks: Assembly-CSharp.dll not loaded yet");
        warned_once = true;
      }
      return;
    }
    warned_once = false;

    auto patch8 = [](uintptr_t addr, const char* tag) {
      uint8_t before[8]{};
      std::memcpy(before, reinterpret_cast<void*>(addr), 8);
      DWORD old = 0;
      if (VirtualProtect(reinterpret_cast<void*>(addr), 8, PAGE_EXECUTE_READWRITE, &old)) {
        std::memset(reinterpret_cast<void*>(addr), 0x00, 8);
        uint8_t after[8]{};
        std::memcpy(after, reinterpret_cast<void*>(addr), 8);
        DWORD dummy;
        VirtualProtect(reinterpret_cast<void*>(addr), 8, old, &dummy);
        std::ostringstream oss;
        oss << "PatchAntiMasterChecks: " << tag << " before=";
        for (int i = 0; i < 8; ++i) {
          oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(before[i]);
        }
        oss << " after=";
        for (int i = 0; i < 8; ++i) {
          oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(after[i]);
        }
        AppendLog(oss.str());
      }
      };

    uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    // RVA confirmed from IDA (Assembly-CSharp.dll)
    patch8(base + 0xA6D90, "ItemManager::GetAllItemVolumesInScene");
    patch8(base + 0xABDA0, "PunManager::ShopPopulateItemVolumes");
    patch8(base + 0xAC11F, "PunManager::TruckPopulateItemVolumes");

    std::ostringstream oss;
    oss << "PatchAntiMasterChecks: applied at base=0x" << std::hex << base;
    AppendLog(oss.str());
    patched = true;
  }

  template <typename T>
  bool ResolveProc(HMODULE module, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    return out != nullptr;
  }

  HMODULE FindMonoModule() {
    const char* candidates[] = {
        "mono-2.0-bdwgc.dll",
        "mono-2.0-sgen.dll",
        "mono.dll",
    };
    for (const char* name : candidates) {
      HMODULE module = GetModuleHandleA(name);
      if (module) {
        AppendLog(std::string("Found Mono module: ") + name);
        return module;
      }
    }
    AppendLog("Mono module not found");
    return nullptr;
  }

  bool ResolveMonoApi() {
    if (g_mono.module) {
      return true;
    }

    HMODULE module = FindMonoModule();
    if (!module) {
      return false;
    }

    MonoApi api = {};
    api.module = module;
    if (!ResolveProc(module, "mono_get_root_domain", api.mono_get_root_domain) ||
      !ResolveProc(module, "mono_thread_attach", api.mono_thread_attach) ||
      !ResolveProc(module, "mono_assembly_foreach", api.mono_assembly_foreach) ||
      !ResolveProc(module, "mono_assembly_get_image", api.mono_assembly_get_image) ||
      !ResolveProc(module, "mono_image_get_name", api.mono_image_get_name) ||
      !ResolveProc(module, "mono_class_from_name", api.mono_class_from_name) ||
      !ResolveProc(module, "mono_class_vtable", api.mono_class_vtable) ||
      !ResolveProc(module, "mono_class_get_field_from_name", api.mono_class_get_field_from_name) ||
      !ResolveProc(module, "mono_class_get_method_from_name", api.mono_class_get_method_from_name) ||
      !ResolveProc(module, "mono_class_get_methods", api.mono_class_get_methods) ||
      !ResolveProc(module, "mono_class_get_fields", api.mono_class_get_fields) ||
      !ResolveProc(module, "mono_class_get_type", api.mono_class_get_type) ||
      !ResolveProc(module, "mono_class_is_subclass_of", api.mono_class_is_subclass_of) ||
      !ResolveProc(module, "mono_type_get_object", api.mono_type_get_object) ||
      !ResolveProc(module, "mono_runtime_invoke", api.mono_runtime_invoke) ||
      !ResolveProc(module, "mono_field_get_value", api.mono_field_get_value) ||
      !ResolveProc(module, "mono_field_static_get_value", api.mono_field_static_get_value) ||
      !ResolveProc(module, "mono_field_set_value", api.mono_field_set_value) ||
      !ResolveProc(module, "mono_object_get_class", api.mono_object_get_class) ||
      !ResolveProc(module, "mono_object_unbox", api.mono_object_unbox) ||
      !ResolveProc(module, "mono_field_get_offset", api.mono_field_get_offset) ||
      !ResolveProc(module, "mono_string_new", api.mono_string_new) ||
      !ResolveProc(module, "mono_compile_method", api.mono_compile_method) ||
      !ResolveProc(module, "mono_method_get_name", api.mono_method_get_name) ||
      !ResolveProc(module, "mono_field_get_name", api.mono_field_get_name) ||
      !ResolveProc(module, "mono_gchandle_new", api.mono_gchandle_new) ||
      !ResolveProc(module, "mono_gchandle_free", api.mono_gchandle_free) ||
      !ResolveProc(module, "mono_gchandle_get_target", api.mono_gchandle_get_target)) {
      AppendLog("Failed to resolve one or more Mono exports");
      return false;
    }
    ResolveProc(module, "mono_field_get_type", api.mono_field_get_type);
    ResolveProc(module, "mono_type_get_type", api.mono_type_get_type);
    ResolveProc(module, "mono_method_signature", api.mono_method_signature);
    ResolveProc(module, "mono_signature_get_param_count", api.mono_signature_get_param_count);
    ResolveProc(module, "mono_signature_get_return_type", api.mono_signature_get_return_type);
    ResolveProc(module, "mono_string_to_utf8", api.mono_string_to_utf8);
    ResolveProc(module, "mono_free", api.mono_free);

    g_mono = api;
    AppendLog("Resolved Mono exports");
    return true;
  }


  bool EnsureThreadAttached() {
    if (!ResolveMonoApi()) {
      return false;
    }

    if (!g_domain) {
      g_domain = g_mono.mono_get_root_domain();
      if (!g_domain) {
        AppendLog("mono_get_root_domain returned null");
        return false;
      }
    }

    DWORD thread_id = GetCurrentThreadId();
    if (g_attached_thread_id != thread_id) {
      if (!g_mono.mono_thread_attach(g_domain)) {
        AppendLog("mono_thread_attach failed");
        return false;
      }
      g_attached_thread_id = thread_id;
      AppendLog("Attached thread to Mono domain");
    }
    return true;
  }

  struct AssemblySearch {
    const char* target = nullptr;
    MonoImage* image = nullptr;
  };

  void __cdecl OnAssembly(MonoAssembly* assembly, void* user_data) {
    auto* search = static_cast<AssemblySearch*>(user_data);
    if (!search || search->image) {
      return;
    }

    MonoImage* image = g_mono.mono_assembly_get_image(assembly);
    if (!image) {
      return;
    }

    const char* name = g_mono.mono_image_get_name(image);
    if (name && _stricmp(name, search->target) == 0) {
      search->image = image;
    }
  }

  MonoImage* FindImage() {
    if (!g_mono.mono_assembly_foreach) {
      return nullptr;
    }

    AssemblySearch search;
    search.target = config::kAssemblyName;
    g_mono.mono_assembly_foreach(OnAssembly, &search);
    if (!search.image) {
      AppendLog(std::string("Assembly not found: ") + config::kAssemblyName);
    }
    return search.image;
  }

  MonoImage* FindImageByName(const char* name) {
    if (!g_mono.mono_assembly_foreach) {
      return nullptr;
    }
    AssemblySearch search;
    search.target = name;
    g_mono.mono_assembly_foreach(OnAssembly, &search);
    if (!search.image) {
      AppendLog(std::string("Assembly not found: ") + name);
    }
    return search.image;
  }

  MonoClass* FindClassAnyAssembly(const char* ns, const char* name) {
    if (!g_mono.mono_assembly_foreach) {
      return nullptr;
    }
    struct SearchData {
      const char* ns;
      const char* name;
      MonoClass* cls;
    } data{ ns, name, nullptr };

    auto callback = [](MonoAssembly* assembly, void* user_data) {
      auto* d = static_cast<SearchData*>(user_data);
      if (d->cls) {
        return;
      }
      MonoImage* img = g_mono.mono_assembly_get_image(assembly);
      if (!img) return;
      MonoClass* cls = g_mono.mono_class_from_name(img, d->ns, d->name);
      if (cls) {
        d->cls = cls;
        const char* img_name = g_mono.mono_image_get_name(img);
        if (img_name) {
          AppendLog(std::string("Resolved ") + d->name + " from image: " + img_name);
        }
      }
      };

    // C-style function pointer adapter
    struct Thunk {
      static void __cdecl Call(MonoAssembly* assembly, void* user_data) {
        auto* d = static_cast<SearchData*>(user_data);
        if (!d || d->cls) return;
        MonoImage* img = g_mono.mono_assembly_get_image(assembly);
        if (!img) return;
        MonoClass* cls = g_mono.mono_class_from_name(img, d->ns, d->name);
        if (cls) {
          d->cls = cls;
          const char* img_name = g_mono.mono_image_get_name(img);
          if (img_name) {
            AppendLog(std::string("Resolved ") + d->name + " from image: " + img_name);
          }
        }
      }
    };

    g_mono.mono_assembly_foreach(Thunk::Call, &data);
    return data.cls;
  }

  void LogAssembliesOnce() {
    static bool logged = false;
    if (logged) return;
    logged = true;

    auto narrow = [](const wchar_t* ws) -> std::string {
      if (!ws) return {};
      int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
      if (len <= 0) return {};
      std::string out(static_cast<size_t>(len - 1), '\0');
      if (!out.empty()) {
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
      }
      return out;
      };

    auto log_module = [narrow](const wchar_t* name) {
      HMODULE mod = GetModuleHandleW(name);
      std::string narrow_name = narrow(name);
      if (narrow_name.empty()) {
        narrow_name = "<null>";
      }
      if (!mod) {
        AppendLog("Module not loaded: " + narrow_name);
        return;
      }
      std::ostringstream oss;
      oss << "Module " << narrow_name << " base=0x" << std::hex
        << reinterpret_cast<uintptr_t>(mod);
      AppendLog(oss.str());
      };

    log_module(L"Assembly-CSharp.dll");
    log_module(L"UnityEngine.CoreModule.dll");
    log_module(L"UnityEngine.dll");
  }

  bool CacheManagedRefs() {
    if (!EnsureThreadAttached()) {
      return false;
    }

    // Log assemblies on first call to help debugging
    LogAssembliesOnce();

    if (!g_image) {
      g_image = FindImage();
      if (!g_image) {
        AppendLog("Mono image is null");
        return false;
      }
    }

    // Apply runtime patch to bypass master-client checks for item population,
    // but only after Assembly-CSharp.dll is confirmed loaded.
    PatchAntiMasterChecks();

    if (!g_unity_image) {
      // Try multiple possible Unity assembly names
      const char* unity_names[] = {
          "UnityEngine.CoreModule",
          "UnityEngine",
          "UnityEngine.UI",
          "UnityEngine.PhysicsModule",
          "UnityEngine.InputLegacyModule"
      };
      for (const char* name : unity_names) {
        g_unity_image = FindImageByName(name);
        if (g_unity_image) {
          AppendLog(std::string("Found Unity image: ") + name);
          break;
        }
      }
      if (!g_unity_image) {
        AppendLog("Could not find any Unity image");
      }
    }

    if (!g_game_director_class) {
      g_game_director_class = g_mono.mono_class_from_name(
        g_image, config::kGameDirectorNamespace, config::kGameDirectorClass);
      if (!g_game_director_class) {
        AppendLog("Failed to resolve GameDirector class");
      }
    }
    if (g_game_director_class && !g_game_director_vtable) {
      g_game_director_vtable = g_mono.mono_class_vtable(g_domain, g_game_director_class);
    }
    if (g_game_director_class && !g_game_director_instance_field) {
      g_game_director_instance_field = g_mono.mono_class_get_field_from_name(
        g_game_director_class, config::kGameDirectorInstanceField);
      if (!g_game_director_instance_field) {
        AppendLog("Failed to resolve GameDirector::instance field");
      }
    }
    if (g_game_director_class && !g_game_director_player_list_field) {
      g_game_director_player_list_field = g_mono.mono_class_get_field_from_name(
        g_game_director_class, config::kGameDirectorPlayerListField);
      if (!g_game_director_player_list_field) {
        AppendLog("Failed to resolve GameDirector::PlayerList field");
      }
    }
    if (g_game_director_class && !g_game_director_current_state_field) {
      g_game_director_current_state_field = g_mono.mono_class_get_field_from_name(
        g_game_director_class, "currentState");
      if (g_game_director_current_state_field) {
        AppendLog("Resolved GameDirector::currentState field");
      }
    }

    if (!g_player_avatar_class) {
      g_player_avatar_class = g_mono.mono_class_from_name(
        g_image, config::kPlayerAvatarNamespace, config::kPlayerAvatarClass);
      if (!g_player_avatar_class) {
        AppendLog("Failed to resolve PlayerAvatar class");
      }
    }
    if (g_player_avatar_class && !g_player_avatar_vtable) {
      g_player_avatar_vtable = g_mono.mono_class_vtable(g_domain, g_player_avatar_class);
    }
    if (g_player_avatar_class && !g_player_avatar_is_local_field) {
      g_player_avatar_is_local_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarIsLocalField);
      if (!g_player_avatar_is_local_field) {
        AppendLog("Failed to resolve PlayerAvatar::isLocal field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_avatar_is_local_field);
        AppendLog("PlayerAvatar::isLocal offset = " + std::to_string(off));
      }
    }
    if (g_player_avatar_class && !g_player_avatar_transform_field) {
      g_player_avatar_transform_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarTransformField);
      if (!g_player_avatar_transform_field) {
        AppendLog("Failed to resolve PlayerAvatar::playerTransform field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_avatar_transform_field);
        AppendLog("PlayerAvatar::playerTransform offset = " + std::to_string(off));
      }
    }
    if (g_player_avatar_class && !g_player_avatar_health_field) {
      g_player_avatar_health_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarHealthField);
      if (!g_player_avatar_health_field) {
        AppendLog("Failed to resolve PlayerAvatar::playerHealth field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_avatar_health_field);
        AppendLog("PlayerAvatar::playerHealth offset = " + std::to_string(off));
      }
    }
    if (g_player_avatar_class && !g_player_avatar_instance_field) {
      const char* candidates[] = {
          config::kPlayerAvatarInstanceField,
          "Instance",
          "s_Instance",
          "m_Instance",
          "staticInstance",
      };
      for (const char* name : candidates) {
        g_player_avatar_instance_field =
          g_mono.mono_class_get_field_from_name(g_player_avatar_class, name);
        if (g_player_avatar_instance_field) {
          AppendLog(std::string("Resolved PlayerAvatar instance field as: ") + name);
          break;
        }
      }
      if (!g_player_avatar_instance_field) {
        AppendLog("Failed to resolve PlayerAvatar static instance field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_avatar_instance_field);
        AppendLog("PlayerAvatar::instance offset = " + std::to_string(off));
      }
    }
    if (g_player_avatar_class && !g_player_avatar_steamid_field) {
      g_player_avatar_steamid_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarSteamIdField);
      if (!g_player_avatar_steamid_field) {
        AppendLog("Failed to resolve PlayerAvatar::steamID field");
      }
    }
    if (g_player_avatar_class && !g_player_avatar_physgrabber_field) {
      g_player_avatar_physgrabber_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarPhysGrabberField);
      if (!g_player_avatar_physgrabber_field) {
        AppendLog("Failed to resolve PlayerAvatar::physGrabber field");
      }
    }

    if (!g_player_health_class) {
      g_player_health_class = g_mono.mono_class_from_name(
        g_image, config::kPlayerHealthNamespace, config::kPlayerHealthClass);
      if (!g_player_health_class) {
        AppendLog("Failed to resolve PlayerHealth class");
      }
    }
    if (g_player_health_class && !g_player_health_value_field) {
      g_player_health_value_field = g_mono.mono_class_get_field_from_name(
        g_player_health_class, config::kPlayerHealthValueField);
      if (!g_player_health_value_field) {
        AppendLog("Failed to resolve PlayerHealth::health field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_health_value_field);
        AppendLog("PlayerHealth::health offset = " + std::to_string(off));
      }
    }
    if (g_player_health_class && !g_player_health_max_field) {
      g_player_health_max_field = g_mono.mono_class_get_field_from_name(
        g_player_health_class, config::kPlayerHealthMaxField);
      if (!g_player_health_max_field) {
        AppendLog("Failed to resolve PlayerHealth::maxHealth field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_health_max_field);
        AppendLog("PlayerHealth::maxHealth offset = " + std::to_string(off));
      }
    }
    if (g_player_health_class && !g_player_health_invincible_set) {
      g_player_health_invincible_set = g_mono.mono_class_get_method_from_name(
        g_player_health_class, config::kPlayerHealthInvincibleSetMethod, 1);
      if (g_player_health_invincible_set) {
        AppendLog("Resolved PlayerHealth::InvincibleSet");
      }
      else {
        AppendLog("Failed to resolve PlayerHealth::InvincibleSet");
      }
    }

    // PlayerController for stamina (EnergyCurrent/EnergyStart).
    if (!g_player_controller_class) {
      g_player_controller_class = g_mono.mono_class_from_name(
        g_image, config::kPlayerControllerNamespace, config::kPlayerControllerClass);
      if (!g_player_controller_class) {
        AppendLog("Failed to resolve PlayerController class");
      }
    }
    if (g_player_controller_class && !g_player_controller_vtable) {
      g_player_controller_vtable = g_mono.mono_class_vtable(g_domain, g_player_controller_class);
    }
    if (g_player_controller_class && !g_player_controller_instance_field) {
      g_player_controller_instance_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerInstanceField);
    }
    if (g_player_controller_class && !g_player_controller_energy_current_field) {
      g_player_controller_energy_current_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerEnergyCurrentField);
      if (!g_player_controller_energy_current_field) {
        AppendLog("Failed to resolve PlayerController::EnergyCurrent field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_controller_energy_current_field);
        AppendLog("PlayerController::EnergyCurrent offset = " + std::to_string(off));
      }
    }
    if (g_player_controller_class && !g_player_controller_energy_start_field) {
      g_player_controller_energy_start_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerEnergyStartField);
      if (!g_player_controller_energy_start_field) {
        AppendLog("Failed to resolve PlayerController::EnergyStart field");
      }
      else if (g_mono.mono_field_get_offset) {
        int off = g_mono.mono_field_get_offset(g_player_controller_energy_start_field);
        AppendLog("PlayerController::EnergyStart offset = " + std::to_string(off));
      }
    }
    if (g_player_controller_class && !g_player_controller_jump_extra_field) {
      g_player_controller_jump_extra_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerJumpExtraField);
      if (g_player_controller_jump_extra_field) {
        AppendLog("Resolved PlayerController::JumpExtra field");
      }
      else {
        AppendLog("Failed to resolve PlayerController::JumpExtra field");
      }
    }
    if (g_player_controller_class && !g_player_controller_override_speed) {
      g_player_controller_override_speed = g_mono.mono_class_get_method_from_name(
        g_player_controller_class, config::kPlayerControllerOverrideSpeedMethod, 2);
      if (g_player_controller_override_speed) {
        AppendLog("Resolved PlayerController::OverrideSpeed");
      }
    }
    if (g_player_controller_class && !g_player_controller_override_speed_multiplier_field) {
      g_player_controller_override_speed_multiplier_field =
        g_mono.mono_class_get_field_from_name(
          g_player_controller_class, config::kPlayerControllerOverrideSpeedMultiplierField);
      if (g_player_controller_override_speed_multiplier_field) {
        AppendLog("Resolved PlayerController::overrideSpeedMultiplier field");
      }
    }
    if (g_player_controller_class && !g_player_controller_override_speed_timer_field) {
      g_player_controller_override_speed_timer_field =
        g_mono.mono_class_get_field_from_name(
          g_player_controller_class, config::kPlayerControllerOverrideSpeedTimerField);
      if (g_player_controller_override_speed_timer_field) {
        AppendLog("Resolved PlayerController::overrideSpeedTimer field");
      }
    }
    if (g_player_controller_class && !g_player_controller_jump_force_field) {
      g_player_controller_jump_force_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerJumpForceField);
      if (g_player_controller_jump_force_field) {
        AppendLog("Resolved PlayerController::JumpForce field");
      }
    }
    if (g_player_controller_class && !g_player_controller_override_jump_cooldown) {
      g_player_controller_override_jump_cooldown = g_mono.mono_class_get_method_from_name(
        g_player_controller_class, config::kPlayerControllerOverrideJumpCooldownMethod, 1);
      if (g_player_controller_override_jump_cooldown) {
        AppendLog("Resolved PlayerController::OverrideJumpCooldown");
      }
    }

    if (!g_run_manager_class) {
      g_run_manager_class = g_mono.mono_class_from_name(g_image, "", "RunManager");
      if (!g_run_manager_class) {
        g_run_manager_class = FindClassAnyAssembly("", "RunManager");
      }
    }
    if (g_run_manager_class && !g_run_manager_vtable) {
      g_run_manager_vtable = g_mono.mono_class_vtable(g_domain, g_run_manager_class);
    }
    if (g_run_manager_class && !g_run_manager_instance_field) {
      g_run_manager_instance_field = g_mono.mono_class_get_field_from_name(
        g_run_manager_class, "instance");
      if (g_run_manager_instance_field) {
        AppendLog("Resolved RunManager::instance field");
      }
    }
    if (g_run_manager_class && !g_run_manager_restarting_field) {
      g_run_manager_restarting_field = g_mono.mono_class_get_field_from_name(
        g_run_manager_class, "restarting");
      if (g_run_manager_restarting_field) {
        AppendLog("Resolved RunManager::restarting field");
      }
    }
    if (g_run_manager_class && !g_run_manager_wait_to_change_scene_field) {
      g_run_manager_wait_to_change_scene_field = g_mono.mono_class_get_field_from_name(
        g_run_manager_class, "waitToChangeScene");
      if (g_run_manager_wait_to_change_scene_field) {
        AppendLog("Resolved RunManager::waitToChangeScene field");
      }
    }
    if (g_run_manager_class && !g_run_manager_lobby_join_field) {
      g_run_manager_lobby_join_field = g_mono.mono_class_get_field_from_name(
        g_run_manager_class, "lobbyJoin");
      if (g_run_manager_lobby_join_field) {
        AppendLog("Resolved RunManager::lobbyJoin field");
      }
    }

    if (!g_level_generator_class) {
      g_level_generator_class = g_mono.mono_class_from_name(g_image, "", "LevelGenerator");
      if (!g_level_generator_class) {
        g_level_generator_class = FindClassAnyAssembly("", "LevelGenerator");
      }
    }
    if (g_level_generator_class && !g_level_generator_vtable) {
      g_level_generator_vtable = g_mono.mono_class_vtable(g_domain, g_level_generator_class);
    }
    if (g_level_generator_class && !g_level_generator_instance_field) {
      g_level_generator_instance_field = g_mono.mono_class_get_field_from_name(
        g_level_generator_class, "Instance");
      if (g_level_generator_instance_field) {
        AppendLog("Resolved LevelGenerator::Instance field");
      }
    }
    if (g_level_generator_class && !g_level_generator_state_field) {
      g_level_generator_state_field = g_mono.mono_class_get_field_from_name(
        g_level_generator_class, "State");
      if (g_level_generator_state_field) {
        AppendLog("Resolved LevelGenerator::State field");
      }
    }
    if (g_level_generator_class && !g_level_generator_generated_field) {
      g_level_generator_generated_field = g_mono.mono_class_get_field_from_name(
        g_level_generator_class, "Generated");
      if (g_level_generator_generated_field) {
        AppendLog("Resolved LevelGenerator::Generated field");
      }
    }

    // Camera / matrices.
    if (!g_unity_camera_class) {
      // First try the Unity image we found
      if (g_unity_image) {
        g_unity_camera_class = g_mono.mono_class_from_name(
          g_unity_image, config::kUnityCameraNamespace, config::kUnityCameraClass);
      }
      // If not found, try the game assembly
      if (!g_unity_camera_class && g_image) {
        g_unity_camera_class = g_mono.mono_class_from_name(
          g_image, config::kUnityCameraNamespace, config::kUnityCameraClass);
      }
      // Last resort: search all assemblies
      if (!g_unity_camera_class) {
        g_unity_camera_class = FindClassAnyAssembly(config::kUnityCameraNamespace,
          config::kUnityCameraClass);
      }
      if (!g_unity_camera_class) {
        AppendLog("Failed to resolve UnityEngine.Camera class");
      }
      else {
        AppendLog("Successfully resolved UnityEngine.Camera class");
      }
    }
    if (g_unity_camera_class && !g_unity_camera_get_main) {
      g_unity_camera_get_main = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraMainMethod, 0);
      if (g_unity_camera_get_main) {
        AppendLog("Successfully resolved Camera::get_main method");
      }
      else {
        AppendLog("Failed to resolve Camera::get_main method");
      }
    }
    if (g_unity_camera_class && !g_unity_camera_get_projection_matrix) {
      g_unity_camera_get_projection_matrix = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraProjectionMatrixMethod, 0);
      if (g_unity_camera_get_projection_matrix) {
        AppendLog("Successfully resolved Camera::get_projectionMatrix method");
      }
      else {
        AppendLog("Failed to resolve Camera::get_projectionMatrix method");
      }
    }
    if (g_unity_camera_class && !g_unity_camera_get_world_to_camera_matrix) {
      g_unity_camera_get_world_to_camera_matrix = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraWorldToCameraMatrixMethod, 0);
      if (g_unity_camera_get_world_to_camera_matrix) {
        AppendLog("Successfully resolved Camera::get_worldToCameraMatrix method");
      }
      else {
        AppendLog("Failed to resolve Camera::get_worldToCameraMatrix method");
      }
    }
    if (g_unity_camera_class && !g_unity_camera_get_transform) {
      g_unity_camera_get_transform = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraGetTransformMethod, 0);
      if (g_unity_camera_get_transform) {
        AppendLogOnce("Camera_get_transform_ok", "Successfully resolved Camera::get_transform method");
      }
      else {
        AppendLogOnce("Camera_get_transform_fail", "Failed to resolve Camera::get_transform method");
      }
    }

    if (!g_component_class) {
      if (g_unity_image) {
        g_component_class =
          g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Component");
      }
      if (!g_component_class && g_image) {
        g_component_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "Component");
      }
      if (!g_component_class) {
        g_component_class = FindClassAnyAssembly("UnityEngine", "Component");
      }
      if (!g_component_class) {
        AppendLogOnce("Component_class_fail", "Failed to resolve UnityEngine.Component class");
      }
      else {
        AppendLogOnce("Component_class_ok", "Successfully resolved UnityEngine.Component class");
      }
    }
    if (g_component_class && !g_component_get_transform) {
      g_component_get_transform = g_mono.mono_class_get_method_from_name(
        g_component_class, config::kUnityCameraGetTransformMethod, 0);
      if (g_component_get_transform) {
        AppendLogOnce("Component_get_transform_ok", "Successfully resolved Component::get_transform method");
      }
      else {
        AppendLogOnce("Component_get_transform_fail", "Failed to resolve Component::get_transform method");
      }
    }
    if (g_component_class && !g_component_get_game_object) {
      g_component_get_game_object =
        g_mono.mono_class_get_method_from_name(g_component_class, "get_gameObject", 0);
      if (g_component_get_game_object) {
        AppendLogOnce("Component_get_gameObject_ok",
          "Successfully resolved Component::get_gameObject method");
      }
      else {
        AppendLogOnce("Component_get_gameObject_fail",
          "Failed to resolve Component::get_gameObject method");
      }
    }

    if (!g_game_object_class) {
      if (g_unity_image) {
        g_game_object_class =
          g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "GameObject");
      }
      if (!g_game_object_class && g_image) {
        g_game_object_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "GameObject");
      }
      if (!g_game_object_class) {
        g_game_object_class = FindClassAnyAssembly("UnityEngine", "GameObject");
      }
      if (!g_game_object_class) {
        AppendLogOnce("GameObject_class_fail", "Failed to resolve UnityEngine.GameObject class");
      }
      else {
        AppendLogOnce("GameObject_class_ok", "Successfully resolved UnityEngine.GameObject class");
      }
    }
    if (g_game_object_class && !g_game_object_get_component) {
      g_game_object_get_component =
        g_mono.mono_class_get_method_from_name(g_game_object_class, "GetComponent", 1);
      if (g_game_object_get_component) {
        AppendLogOnce("GameObject_get_component_ok", "Successfully resolved GameObject::GetComponent(Type)");
      }
      else {
        AppendLogOnce("GameObject_get_component_fail", "Failed to resolve GameObject::GetComponent(Type)");
      }
    }
    if (g_game_object_class && !g_game_object_get_component_in_parent) {
      g_game_object_get_component_in_parent =
        g_mono.mono_class_get_method_from_name(g_game_object_class, "GetComponentInParent", 1);
      if (g_game_object_get_component_in_parent) {
        AppendLogOnce("GameObject_get_component_parent_ok",
          "Successfully resolved GameObject::GetComponentInParent(Type)");
      }
      else {
        AppendLogOnce("GameObject_get_component_parent_fail",
          "Failed to resolve GameObject::GetComponentInParent(Type)");
      }
    }
    if (g_game_object_class && !g_game_object_get_layer) {
      g_game_object_get_layer =
        g_mono.mono_class_get_method_from_name(g_game_object_class, "get_layer", 0);
      if (g_game_object_get_layer) {
        AppendLogOnce("GameObject_get_layer_ok", "Successfully resolved GameObject::get_layer method");
      }
      else {
        AppendLogOnce("GameObject_get_layer_fail", "Failed to resolve GameObject::get_layer method");
      }
    }
    if (!g_unity_object_class) {
      if (g_unity_image) {
        g_unity_object_class =
          g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Object");
      }
      if (!g_unity_object_class && g_image) {
        g_unity_object_class =
          g_mono.mono_class_from_name(g_image, "UnityEngine", "Object");
      }
      if (!g_unity_object_class) {
        g_unity_object_class = FindClassAnyAssembly("UnityEngine", "Object");
      }
      if (g_unity_object_class) {
        AppendLog("Resolved UnityEngine.Object class");
      }
    }
    if (g_unity_object_class && !g_unity_object_get_name) {
      g_unity_object_get_name =
        g_mono.mono_class_get_method_from_name(g_unity_object_class, "get_name", 0);
      if (g_unity_object_get_name) {
        AppendLog("Resolved UnityEngine.Object::get_name");
      }
      else {
        AppendLog("Failed to resolve UnityEngine.Object::get_name");
      }
    }
    if (g_unity_object_class && !g_unity_object_op_eq) {
      g_unity_object_op_eq =
        g_mono.mono_class_get_method_from_name(g_unity_object_class, "op_Equality", 2);
      if (g_unity_object_op_eq) {
        AppendLog("Resolved UnityEngine.Object::op_Equality");
      }
      else {
        AppendLog("Failed to resolve UnityEngine.Object::op_Equality");
      }
    }

    if (!g_transform_class) {
      // First try the Unity image we found
      if (g_unity_image) {
        g_transform_class = g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Transform");
      }
      // If not found, try the game assembly
      if (!g_transform_class && g_image) {
        g_transform_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "Transform");
      }
      // Last resort: search all assemblies
      if (!g_transform_class) {
        g_transform_class = FindClassAnyAssembly("UnityEngine", "Transform");
      }
      if (!g_transform_class) {
        AppendLog("Failed to resolve UnityEngine.Transform");
      }
      else {
        AppendLog("Successfully resolved UnityEngine.Transform class");
      }
    }
    if (g_transform_class && !g_transform_get_local_to_world) {
      g_transform_get_local_to_world = g_mono.mono_class_get_method_from_name(
        g_transform_class, config::kTransformLocalToWorldMatrixMethod, 0);
      if (g_transform_get_local_to_world) {
        AppendLog("Successfully resolved Transform::get_localToWorldMatrix method");
      }
      else {
        AppendLog("Failed to resolve Transform::get_localToWorldMatrix method");
      }
    }
    if (g_transform_class && !g_transform_get_world_to_local) {
      g_transform_get_world_to_local = g_mono.mono_class_get_method_from_name(
        g_transform_class, config::kTransformWorldToLocalMatrixMethod, 0);
      if (g_transform_get_world_to_local) {
        AppendLog("Successfully resolved Transform::get_worldToLocalMatrix method");
      }
      else {
        AppendLog("Failed to resolve Transform::get_worldToLocalMatrix method");
      }
    }

    if (!g_physics_class) {
      if (g_unity_image) {
        g_physics_class = g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Physics");
      }
      if (!g_physics_class && g_image) {
        g_physics_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "Physics");
      }
      if (!g_physics_class) {
        g_physics_class = FindClassAnyAssembly("UnityEngine", "Physics");
      }
      if (g_physics_class) {
        AppendLog("Resolved Physics class");
      }
    }
    if (g_physics_class && !g_physics_overlap_sphere) {
      int arg_candidates[] = { 4, 3, 2 };
      for (int argc : arg_candidates) {
        g_physics_overlap_sphere =
          g_mono.mono_class_get_method_from_name(g_physics_class, "OverlapSphere", argc);
        if (g_physics_overlap_sphere) {
          g_physics_overlap_sphere_argc = argc;
          std::ostringstream oss;
          oss << "Resolved Physics::OverlapSphere argc=" << argc;
          AppendLog(oss.str());
          break;
        }
      }
    }
    if (!g_layer_mask_class) {
      if (g_unity_image) {
        g_layer_mask_class =
          g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "LayerMask");
      }
      if (!g_layer_mask_class && g_image) {
        g_layer_mask_class =
          g_mono.mono_class_from_name(g_image, "UnityEngine", "LayerMask");
      }
      if (!g_layer_mask_class) {
        g_layer_mask_class = FindClassAnyAssembly("UnityEngine", "LayerMask");
      }
      if (g_layer_mask_class) {
        AppendLog("Resolved LayerMask class");
      }
    }
    if (g_layer_mask_class && !g_layer_mask_name_to_layer) {
      g_layer_mask_name_to_layer =
        g_mono.mono_class_get_method_from_name(g_layer_mask_class, "NameToLayer", 1);
      if (g_layer_mask_name_to_layer) {
        AppendLog("Resolved LayerMask::NameToLayer");
      }
    }
    if (!g_collider_class) {
      if (g_unity_image) {
        g_collider_class =
          g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Collider");
      }
      if (!g_collider_class && g_image) {
        g_collider_class =
          g_mono.mono_class_from_name(g_image, "UnityEngine", "Collider");
      }
      if (!g_collider_class) {
        g_collider_class = FindClassAnyAssembly("UnityEngine", "Collider");
      }
      if (g_collider_class) {
        AppendLog("Resolved UnityEngine.Collider class");
      }
    }

    if (!g_semi_func_class) {
      g_semi_func_class = g_mono.mono_class_from_name(
        g_image, config::kSemiFuncNamespace, config::kSemiFuncClass);
    }
    if (g_semi_func_class && !g_player_get_all_method) {
      g_player_get_all_method = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, "PlayerGetAll", 0);
      if (!g_player_get_all_method) {
        AppendLog("Failed to resolve SemiFunc::PlayerGetAll");
      }
    }
    if (g_semi_func_class && !g_player_avatar_local_method) {
      g_player_avatar_local_method = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncLocalMethod, 0);
    }
    if (g_semi_func_class && !g_semi_func_stat_set_run_currency) {
      g_semi_func_stat_set_run_currency = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncStatSetRunCurrencyMethod, 1);
      if (g_semi_func_stat_set_run_currency) {
        AppendLog("Resolved SemiFunc::StatSetRunCurrency");
      }
      else {
        AppendLog("Failed to resolve SemiFunc::StatSetRunCurrency");
      }
    }
    if (g_semi_func_class && !g_semi_func_stat_get_run_currency) {
      g_semi_func_stat_get_run_currency = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncStatGetRunCurrencyMethod, 0);
      if (g_semi_func_stat_get_run_currency) {
        AppendLog("Resolved SemiFunc::StatGetRunCurrency");
      }
    }
    if (g_semi_func_class && !g_semi_func_is_not_master_client) {
      g_semi_func_is_not_master_client = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncIsNotMasterClientMethod, 0);
      if (g_semi_func_is_not_master_client) {
        AppendLog("Resolved SemiFunc::IsNotMasterClient");
      }
    }
    if (g_semi_func_class && !g_semi_func_is_master_client) {
      g_semi_func_is_master_client = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncIsMasterClientMethod, 0);
      if (g_semi_func_is_master_client) {
        AppendLog("Resolved SemiFunc::IsMasterClient");
      }
    }
    if (g_semi_func_class && !g_semi_func_is_master_client_or_singleplayer) {
      g_semi_func_is_master_client_or_singleplayer = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncIsMasterClientOrSingleplayerMethod, 0);
      if (g_semi_func_is_master_client_or_singleplayer) {
        AppendLog("Resolved SemiFunc::IsMasterClientOrSingleplayer");
      }
    }
    if (g_semi_func_class && !g_semi_func_master_only_rpc) {
      g_semi_func_master_only_rpc = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncMasterOnlyRPCMethod, 1);
      if (g_semi_func_master_only_rpc) {
        AppendLog("Resolved SemiFunc::MasterOnlyRPC");
      }
    }
    if (g_semi_func_class && !g_semi_func_owner_only_rpc) {
      g_semi_func_owner_only_rpc = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncOwnerOnlyRPCMethod, 2);
      if (g_semi_func_owner_only_rpc) {
        AppendLog("Resolved SemiFunc::OwnerOnlyRPC");
      }
    }
    if (g_semi_func_class && !g_semi_func_master_and_owner_only_rpc) {
      g_semi_func_master_and_owner_only_rpc = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, config::kSemiFuncMasterAndOwnerOnlyRPCMethod, 2);
      if (g_semi_func_master_and_owner_only_rpc) {
        AppendLog("Resolved SemiFunc::MasterAndOwnerOnlyRPC");
      }
    }
    if (!g_photon_network_class) {
      g_photon_network_class = FindClassAnyAssembly("Photon.Pun", "PhotonNetwork");
      if (g_photon_network_class) {
        AppendLog("Resolved PhotonNetwork class");
      }
    }
    if (g_photon_network_class && !g_photon_network_get_is_master_client) {
      g_photon_network_get_is_master_client =
        g_mono.mono_class_get_method_from_name(g_photon_network_class, "get_IsMasterClient", 0);
      if (g_photon_network_get_is_master_client) {
        AppendLog("Resolved PhotonNetwork::get_IsMasterClient");
      }
    }
    if (g_photon_network_class && !g_photon_network_get_local_player) {
      g_photon_network_get_local_player =
        g_mono.mono_class_get_method_from_name(g_photon_network_class, "get_LocalPlayer", 0);
      if (g_photon_network_get_local_player) {
        AppendLog("Resolved PhotonNetwork::get_LocalPlayer");
      }
    }
    if (g_photon_network_class && !g_photon_network_get_master_client) {
      g_photon_network_get_master_client =
        g_mono.mono_class_get_method_from_name(g_photon_network_class, "get_MasterClient", 0);
      if (g_photon_network_get_master_client) {
        AppendLog("Resolved PhotonNetwork::get_MasterClient");
      }
    }
    if (g_photon_network_class && !g_photon_network_get_in_room) {
      g_photon_network_get_in_room =
        g_mono.mono_class_get_method_from_name(g_photon_network_class, "get_InRoom", 0);
      if (g_photon_network_get_in_room) {
        AppendLog("Resolved PhotonNetwork::get_InRoom");
      }
    }
    if (g_photon_network_class && !g_photon_network_get_level_loading_progress) {
      g_photon_network_get_level_loading_progress =
        g_mono.mono_class_get_method_from_name(g_photon_network_class, "get_LevelLoadingProgress", 0);
      if (g_photon_network_get_level_loading_progress) {
        AppendLog("Resolved PhotonNetwork::get_LevelLoadingProgress");
      }
    }
    if (!g_photon_player_class) {
      g_photon_player_class = FindClassAnyAssembly("Photon.Realtime", "Player");
      if (g_photon_player_class) {
        AppendLog("Resolved Photon.Realtime.Player class");
      }
    }
    if (g_photon_player_class && !g_photon_player_get_actor_number) {
      g_photon_player_get_actor_number =
        g_mono.mono_class_get_method_from_name(g_photon_player_class, "get_ActorNumber", 0);
      if (g_photon_player_get_actor_number) {
        AppendLog("Resolved Photon.Realtime.Player::get_ActorNumber");
      }
    }

    // StatsManager for direct runStats edits
    if (!g_stats_manager_class) {
      g_stats_manager_class = g_mono.mono_class_from_name(
        g_image, config::kStatsManagerNamespace, config::kStatsManagerClass);
      if (!g_stats_manager_class) {
        g_stats_manager_class =
          FindClassAnyAssembly(config::kStatsManagerNamespace, config::kStatsManagerClass);
      }
      if (!g_stats_manager_class) {
        AppendLog("Failed to resolve StatsManager class");
      }
    }
    if (g_stats_manager_class && !g_stats_manager_vtable) {
      g_stats_manager_vtable = g_mono.mono_class_vtable(g_domain, g_stats_manager_class);
    }
    if (g_stats_manager_class && !g_stats_manager_instance_field) {
      g_stats_manager_instance_field = g_mono.mono_class_get_field_from_name(
        g_stats_manager_class, config::kStatsManagerInstanceField);
      if (!g_stats_manager_instance_field) {
        AppendLog("Failed to resolve StatsManager::instance field");
      }
    }
    if (g_stats_manager_class && !g_stats_manager_run_stats_field) {
      g_stats_manager_run_stats_field = g_mono.mono_class_get_field_from_name(
        g_stats_manager_class, config::kStatsManagerRunStatsField);
      if (!g_stats_manager_run_stats_field) {
        AppendLog("Failed to resolve StatsManager::runStats field");
      }
    }

    // Currency UI refresh
    if (!g_currency_ui_class && g_image) {
      g_currency_ui_class =
        g_mono.mono_class_from_name(g_image, "", config::kCurrencyUIClass);
      if (!g_currency_ui_class) {
        AppendLog("Failed to resolve CurrencyUI class");
      }
    }
    if (g_currency_ui_class && !g_currency_ui_fetch) {
      g_currency_ui_fetch = g_mono.mono_class_get_method_from_name(
        g_currency_ui_class, config::kCurrencyUIFetchMethod, 0);
      if (!g_currency_ui_fetch) {
        AppendLog("Failed to resolve CurrencyUI::FetchCurrency");
      }
    }

    // Handcart references
    if (!g_phys_grab_cart_class && g_image) {
      const char* namespaces[] = { "", "Game", "Physics", "Items", "Player" };
      const char* class_names[] = { config::kPhysGrabCartClass, "GrabCart", "HandCart" };
      for (const char* ns : namespaces) {
        for (const char* name : class_names) {
          g_phys_grab_cart_class = g_mono.mono_class_from_name(g_image, ns, name);
          if (g_phys_grab_cart_class) {
            std::ostringstream oss;
            oss << "Resolved PhysGrabCart class as: " << (ns ? ns : "") << "." << name;
            AppendLog(oss.str());
            break;
          }
        }
        if (g_phys_grab_cart_class) break;
      }
      if (!g_phys_grab_cart_class) {
        for (const char* name : class_names) {
          g_phys_grab_cart_class = FindClassAnyAssembly("", name);
          if (g_phys_grab_cart_class) {
            AppendLog(std::string("Resolved PhysGrabCart via FindClassAnyAssembly as: ") + name);
            break;
          }
        }
      }
    }
    if (g_phys_grab_cart_class && !g_phys_grab_cart_haul_field) {
      const char* field_names[] = {
          config::kPhysGrabCartHaulCurrentField, "haulCurrent", "currentHaul", "value", "haulValue",
          "cartValue" };
      for (const char* name : field_names) {
        g_phys_grab_cart_haul_field =
          g_mono.mono_class_get_field_from_name(g_phys_grab_cart_class, name);
        if (g_phys_grab_cart_haul_field) {
          std::ostringstream oss;
          oss << "Resolved PhysGrabCart haul field as: " << name;
          AppendLog(oss.str());
          break;
        }
      }
    }
    if (g_phys_grab_cart_class && !g_phys_grab_cart_set_haul_text) {
      g_phys_grab_cart_set_haul_text = g_mono.mono_class_get_method_from_name(
        g_phys_grab_cart_class, config::kPhysGrabCartSetHaulTextMethod, 0);
    }

    // PhysGrabber
    if (!g_phys_grabber_class && g_image) {
      g_phys_grabber_class = g_mono.mono_class_from_name(
        g_image, "", config::kPhysGrabberClass);
      if (!g_phys_grabber_class) {
        g_phys_grabber_class = FindClassAnyAssembly("", config::kPhysGrabberClass);
      }
      if (g_phys_grabber_class) {
        AppendLog("Resolved PhysGrabber class");
      }
    }
    if (g_phys_grabber_class && !g_phys_grabber_range_field) {
      g_phys_grabber_range_field = g_mono.mono_class_get_field_from_name(
        g_phys_grabber_class, config::kPhysGrabberGrabRangeField);
      if (g_phys_grabber_range_field) {
        AppendLog("Resolved PhysGrabber::grabRange field");
      }
    }
    if (g_phys_grabber_class && !g_phys_grabber_strength_field) {
      g_phys_grabber_strength_field = g_mono.mono_class_get_field_from_name(
        g_phys_grabber_class, config::kPhysGrabberGrabStrengthField);
      if (g_phys_grabber_strength_field) {
        AppendLog("Resolved PhysGrabber::grabStrength field");
      }
    }

    // ExtractionPoint
    if (!g_extraction_point_class && g_image) {
      g_extraction_point_class = g_mono.mono_class_from_name(
        g_image, "", config::kExtractionPointClass);
      if (!g_extraction_point_class) {
        g_extraction_point_class =
          FindClassAnyAssembly("", config::kExtractionPointClass);
      }
      if (g_extraction_point_class) {
        AppendLog("Resolved ExtractionPoint class");
      }
    }
    if (g_extraction_point_class && !g_extraction_point_haul_goal_field) {
      g_extraction_point_haul_goal_field = g_mono.mono_class_get_field_from_name(
        g_extraction_point_class, config::kExtractionPointHaulGoalField);
    }
    if (g_extraction_point_class && !g_extraction_point_haul_current_field) {
      g_extraction_point_haul_current_field = g_mono.mono_class_get_field_from_name(
        g_extraction_point_class, config::kExtractionPointHaulCurrentField);
    }
    if (g_extraction_point_class && !g_extraction_point_extraction_haul_field) {
      const char* names[] = { "extractionHaul", "haulExtracted", "extractedHaul" };
      for (const char* n : names) {
        g_extraction_point_extraction_haul_field =
          g_mono.mono_class_get_field_from_name(g_extraction_point_class, n);
        if (g_extraction_point_extraction_haul_field) {
          AppendLog(std::string("Resolved ExtractionPoint::extractionHaul as: ") + n);
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_run_currency_before_field) {
      const char* names[] = { "runCurrencyBefore", "currencyBefore", "runCurrencyStart" };
      for (const char* n : names) {
        g_extraction_point_run_currency_before_field =
          g_mono.mono_class_get_field_from_name(g_extraction_point_class, n);
        if (g_extraction_point_run_currency_before_field) {
          AppendLog(std::string("Resolved ExtractionPoint::runCurrencyBefore as: ") + n);
          break;
        }
      }
    }

    // PunManager methods (upgrades and strength)
    if (!g_pun_manager_class) {
      g_pun_manager_class = g_mono.mono_class_from_name(
        g_image, config::kPunManagerNamespace, config::kPunManagerClass);
      if (!g_pun_manager_class) {
        g_pun_manager_class =
          FindClassAnyAssembly(config::kPunManagerNamespace, config::kPunManagerClass);
      }
      if (g_pun_manager_class) {
        AppendLog("Resolved PunManager class");
      }
      else {
        AppendLog("Failed to resolve PunManager class");
      }
    }
    if (g_pun_manager_class && !g_pun_manager_vtable) {
      g_pun_manager_vtable = g_mono.mono_class_vtable(g_domain, g_pun_manager_class);
    }
    if (g_pun_manager_class && !g_pun_manager_instance_field) {
      const char* inst_names[] = { "instance", "Instance", "s_Instance", "m_Instance", "staticInstance" };
      for (const char* name : inst_names) {
        g_pun_manager_instance_field = g_mono.mono_class_get_field_from_name(g_pun_manager_class, name);
        if (g_pun_manager_instance_field) {
          AppendLog(std::string("Resolved PunManager instance field as: ") + name);
          break;
        }
      }
      if (!g_pun_manager_instance_field) {
        AppendLog("Failed to resolve PunManager::instance field");
      }
    }

    // RoundDirector
    if (!g_round_director_class && g_image) {
      g_round_director_class = g_mono.mono_class_from_name(g_image, "", "RoundDirector");
      if (!g_round_director_class) {
        g_round_director_class = FindClassAnyAssembly("", "RoundDirector");
      }
      if (g_round_director_class) {
        AppendLog("Resolved RoundDirector class");
      }
      else {
        AppendLogOnce("RoundDirector_class_missing", "Failed to resolve RoundDirector class");
      }
    }
    if (g_round_director_class && !g_round_director_vtable) {
      g_round_director_vtable = g_mono.mono_class_vtable(g_domain, g_round_director_class);
    }
    if (g_round_director_class && !g_round_director_instance_field) {
      const char* inst_names[] = { "instance", "Instance", "s_Instance", "m_Instance", "staticInstance" };
      for (const char* name : inst_names) {
        g_round_director_instance_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, name);
        if (g_round_director_instance_field) {
          AppendLog(std::string("Resolved RoundDirector instance field as: ") + name);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_current_haul_field) {
      const char* names[] = { "currentHaul", "haulCurrent", "currentValue" };
      for (const char* n : names) {
        g_round_current_haul_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_current_haul_field) {
          AppendLog(std::string("Resolved RoundDirector::currentHaul as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_current_haul_max_field) {
      const char* names[] = { "currentHaulMax", "haulCurrentMax", "haulMax" };
      for (const char* n : names) {
        g_round_current_haul_max_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_current_haul_max_field) {
          AppendLog(std::string("Resolved RoundDirector::currentHaulMax as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_haul_goal_field) {
      const char* names[] = { "haulGoal", "currentGoal", "targetHaul" };
      for (const char* n : names) {
        g_round_haul_goal_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_haul_goal_field) {
          AppendLog(std::string("Resolved RoundDirector::haulGoal as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_total_haul_field) {
      const char* names[] = { "totalHaul", "haulTotal", "runHaulTotal" };
      for (const char* n : names) {
        g_round_total_haul_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_total_haul_field) {
          AppendLog(std::string("Resolved RoundDirector::totalHaul as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_haul_goal_max_field) {
      const char* names[] = { "haulGoalMax", "goalMax", "maxHaulGoal" };
      for (const char* n : names) {
        g_round_haul_goal_max_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_haul_goal_max_field) {
          AppendLog(std::string("Resolved RoundDirector::haulGoalMax as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_extraction_point_surplus_field) {
      const char* names[] = { "extractionPointSurplus", "haulSurplus", "surplusHaul" };
      for (const char* n : names) {
        g_round_extraction_point_surplus_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_extraction_point_surplus_field) {
          AppendLog(std::string("Resolved RoundDirector::extractionPointSurplus as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_extraction_haul_goal_field) {
      const char* names[] = { "extractionHaulGoal", "extractionGoal", "haulGoalExtraction" };
      for (const char* n : names) {
        g_round_extraction_haul_goal_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_extraction_haul_goal_field) {
          AppendLog(std::string("Resolved RoundDirector::extractionHaulGoal as: ") + n);
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_set_current_haul_method) {
      const char* names[] = {
        "SetCurrentHaul", "SetHaulCurrent", "SetHaul", "SetExtractionHaul", "UpdateCurrentHaul",
        "UpdateHaul", "ApplyCurrentHaul", "SyncCurrentHaul"
      };
      for (const char* n : names) {
        g_extraction_point_set_current_haul_method =
          g_mono.mono_class_get_method_from_name(g_extraction_point_class, n, 1);
        if (g_extraction_point_set_current_haul_method) {
          AppendLog(std::string("Resolved ExtractionPoint current-haul method as: ") + n + "(1)");
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_set_haul_goal_method) {
      const char* names[] = {
        "SetHaulGoal", "SetCurrentGoal", "SetGoal", "UpdateHaulGoal", "SetExtractionGoal"
      };
      for (const char* n : names) {
        g_extraction_point_set_haul_goal_method =
          g_mono.mono_class_get_method_from_name(g_extraction_point_class, n, 1);
        if (g_extraction_point_set_haul_goal_method) {
          AppendLog(std::string("Resolved ExtractionPoint haul-goal method as: ") + n + "(1)");
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_apply_haul_method) {
      const char* names[] = {
        "SetHaul", "ApplyHaul", "SyncHaul", "SetHaulState", "SetCurrentHaulAndGoal"
      };
      for (const char* n : names) {
        g_extraction_point_apply_haul_method =
          g_mono.mono_class_get_method_from_name(g_extraction_point_class, n, 2);
        if (g_extraction_point_apply_haul_method) {
          AppendLog(std::string("Resolved ExtractionPoint apply-haul method as: ") + n + "(2)");
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_set_haul_text_method) {
      const char* names[] = {
        "SetHaulText", "UpdateHaulText", "RefreshHaulText", "UpdateCollectorText"
      };
      for (const char* n : names) {
        g_extraction_point_set_haul_text_method =
          g_mono.mono_class_get_method_from_name(g_extraction_point_class, n, 0);
        if (g_extraction_point_set_haul_text_method) {
          AppendLog(std::string("Resolved ExtractionPoint text method as: ") + n + "(0)");
          break;
        }
      }
    }
    if (g_extraction_point_class && !g_extraction_point_refresh_method) {
      const char* names[] = {
        "Refresh", "RefreshUI", "UpdateUI", "UpdateDisplay", "Update", "LateUpdate"
      };
      for (const char* n : names) {
        g_extraction_point_refresh_method =
          g_mono.mono_class_get_method_from_name(g_extraction_point_class, n, 0);
        if (g_extraction_point_refresh_method) {
          AppendLog(std::string("Resolved ExtractionPoint refresh method as: ") + n + "(0)");
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_extraction_points_field) {
      const char* names[] = { "extractionPoints", "extractionPointCount", "extractionTotal" };
      for (const char* n : names) {
        g_round_extraction_points_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_extraction_points_field) {
          AppendLog(std::string("Resolved RoundDirector::extractionPoints as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_extraction_point_current_field) {
      const char* names[] = { "extractionPointCurrent", "currentByExtractionPoint",
                              "extractionCurrentList" };
      for (const char* n : names) {
        g_round_extraction_point_current_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_extraction_point_current_field) {
          AppendLog(std::string("Resolved RoundDirector::extractionPointCurrent as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_dollar_haul_list_field) {
      const char* names[] = { "dollarHaulList", "haulDollarList", "dollarHauls" };
      for (const char* n : names) {
        g_round_dollar_haul_list_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_dollar_haul_list_field) {
          AppendLog(std::string("Resolved RoundDirector::dollarHaulList as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_extraction_points_completed_field) {
      const char* names[] = { "extractionPointsCompleted", "completedExtractionPoints",
                              "extractionCompleted" };
      for (const char* n : names) {
        g_round_extraction_points_completed_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_extraction_points_completed_field) {
          AppendLog(std::string("Resolved RoundDirector::extractionPointsCompleted as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_all_extraction_points_completed_field) {
      const char* names[] = { "allExtractionPointsCompleted", "allExtractionCompleted",
                              "allPointsCompleted" };
      for (const char* n : names) {
        g_round_all_extraction_points_completed_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_all_extraction_points_completed_field) {
          AppendLog(std::string("Resolved RoundDirector::allExtractionPointsCompleted as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_stage_field) {
      const char* names[] = { "currentStage", "stage", "round", "roundIndex" };
      for (const char* n : names) {
        g_round_stage_field =
          g_mono.mono_class_get_field_from_name(g_round_director_class, n);
        if (g_round_stage_field) {
          AppendLog(std::string("Resolved RoundDirector stage field as: ") + n);
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_set_current_haul_method) {
      const char* names[] = {
        "SetCurrentHaul", "SetHaulCurrent", "SetHaul", "SetExtractionHaul", "UpdateCurrentHaul",
        "UpdateHaul", "ApplyCurrentHaul", "SyncCurrentHaul"
      };
      for (const char* n : names) {
        g_round_set_current_haul_method =
          g_mono.mono_class_get_method_from_name(g_round_director_class, n, 1);
        if (g_round_set_current_haul_method) {
          AppendLog(std::string("Resolved RoundDirector current-haul method as: ") + n + "(1)");
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_set_haul_goal_method) {
      const char* names[] = {
        "SetHaulGoal", "SetCurrentGoal", "SetGoal", "UpdateHaulGoal", "SetExtractionGoal"
      };
      for (const char* n : names) {
        g_round_set_haul_goal_method =
          g_mono.mono_class_get_method_from_name(g_round_director_class, n, 1);
        if (g_round_set_haul_goal_method) {
          AppendLog(std::string("Resolved RoundDirector haul-goal method as: ") + n + "(1)");
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_apply_haul_method) {
      const char* names[] = { "SetHaul", "ApplyHaul", "SyncHaul", "SetHaulState", "SetRoundHaul" };
      for (const char* n : names) {
        g_round_apply_haul_method =
          g_mono.mono_class_get_method_from_name(g_round_director_class, n, 2);
        if (g_round_apply_haul_method) {
          AppendLog(std::string("Resolved RoundDirector apply-haul method as: ") + n + "(2)");
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_refresh_haul_method) {
      const char* names[] = {
        "Refresh", "RefreshUI", "UpdateUI", "UpdateHUD", "UpdateHaulText", "SetHaulText", "Update"
      };
      for (const char* n : names) {
        g_round_refresh_haul_method =
          g_mono.mono_class_get_method_from_name(g_round_director_class, n, 0);
        if (g_round_refresh_haul_method) {
          AppendLog(std::string("Resolved RoundDirector refresh method as: ") + n + "(0)");
          break;
        }
      }
    }
    if (g_round_director_class && !g_round_director_update_hooked) {
      InstallRoundDirectorUpdateHook();
    }
    if (g_pun_manager_class && !g_pun_upgrade_extra_jump) {
      for (int argc : {2, 1, 0}) {
        g_pun_upgrade_extra_jump = g_mono.mono_class_get_method_from_name(
          g_pun_manager_class, config::kPunManagerUpgradeExtraJumpMethod, argc);
        if (g_pun_upgrade_extra_jump) {
          AppendLog("Resolved PunManager::UpgradePlayerExtraJump argc=" + std::to_string(argc));
          break;
        }
      }
      if (g_pun_upgrade_extra_jump) {
        // already logged
      }
    }
    if (g_pun_manager_class && !g_pun_set_run_stat_set) {
      g_pun_set_run_stat_set = g_mono.mono_class_get_method_from_name(
        g_pun_manager_class, config::kPunManagerSetRunStatSetMethod, 2);
      if (g_pun_set_run_stat_set) {
        AppendLog("Resolved PunManager::SetRunStatSet");
      }
    }
    if (g_pun_manager_class && !g_pun_upgrade_grab_strength) {
      for (int argc : {2, 1, 0}) {
        g_pun_upgrade_grab_strength = g_mono.mono_class_get_method_from_name(
          g_pun_manager_class, config::kPunManagerUpgradeGrabStrengthMethod, argc);
        if (g_pun_upgrade_grab_strength) {
          AppendLog("Resolved PunManager::UpgradePlayerGrabStrength argc=" + std::to_string(argc));
          break;
        }
      }
      if (g_pun_upgrade_grab_strength) {
        // already logged
      }
    }
    if (g_pun_manager_class && !g_pun_upgrade_throw_strength) {
      for (int argc : {2, 1, 0}) {
        g_pun_upgrade_throw_strength = g_mono.mono_class_get_method_from_name(
          g_pun_manager_class, config::kPunManagerUpgradeThrowStrengthMethod, argc);
        if (g_pun_upgrade_throw_strength) {
          AppendLog("Resolved PunManager::UpgradePlayerThrowStrength argc=" + std::to_string(argc));
          break;
        }
      }
      if (g_pun_upgrade_throw_strength) {
        // already logged
      }
    }

    // Item system
    if (!g_item_manager_class) {
      g_item_manager_class = g_mono.mono_class_from_name(
        g_image, config::kItemManagerNamespace, config::kItemManagerClass);
      if (!g_item_manager_class) {
        g_item_manager_class = FindClassAnyAssembly(
          config::kItemManagerNamespace, config::kItemManagerClass);
      }
      if (!g_item_manager_class) {
        AppendLog("Failed to resolve ItemManager class");
      }
      else {
        AppendLog("Resolved ItemManager class");
      }
    }
    if (g_item_manager_class && !g_item_manager_vtable) {
      g_item_manager_vtable = g_mono.mono_class_vtable(g_domain, g_item_manager_class);
    }
    if (g_item_manager_class && !g_item_manager_instance_field) {
      g_item_manager_instance_field = g_mono.mono_class_get_field_from_name(
        g_item_manager_class, config::kItemManagerInstanceField);
      if (!g_item_manager_instance_field) {
        AppendLog("Failed to resolve ItemManager::instance field");
      }
    }
    if (g_item_manager_class && !g_item_manager_item_volumes_field) {
      g_item_manager_item_volumes_field =
        g_mono.mono_class_get_field_from_name(g_item_manager_class, "itemVolumes");
      if (!g_item_manager_item_volumes_field) {
        AppendLog("Failed to resolve ItemManager::itemVolumes field");
      }
    }
    // PhysGrabObject class (world items) for fallback scans.
    if (!g_phys_grab_object_class) {
      g_phys_grab_object_class = g_mono.mono_class_from_name(
        g_image, config::kPhysGrabObjectNamespace, config::kPhysGrabObjectClass);
      if (!g_phys_grab_object_class) {
        g_phys_grab_object_class =
          FindClassAnyAssembly(config::kPhysGrabObjectNamespace, config::kPhysGrabObjectClass);
      }
      if (!g_phys_grab_object_class) {
        AppendLogOnce("PhysGrabObject_class_missing", "Failed to resolve PhysGrabObject class");
      }
    }
    if (g_item_manager_class && !g_item_manager_spawned_items_field) {
      g_item_manager_spawned_items_field =
        g_mono.mono_class_get_field_from_name(g_item_manager_class, "spawnedItems");
    }

    // ShopManager (secret item volumes)
    if (!g_shop_manager_class && g_image) {
      g_shop_manager_class = g_mono.mono_class_from_name(g_image, "", "ShopManager");
      if (!g_shop_manager_class) {
        g_shop_manager_class = FindClassAnyAssembly("", "ShopManager");
      }
    }
    if (g_shop_manager_class && !g_shop_manager_vtable) {
      g_shop_manager_vtable = g_mono.mono_class_vtable(g_domain, g_shop_manager_class);
    }
    if (g_shop_manager_class && !g_shop_manager_instance_field) {
      g_shop_manager_instance_field =
        g_mono.mono_class_get_field_from_name(g_shop_manager_class, "instance");
    }
    if (g_shop_manager_class && !g_shop_secret_item_volumes_field) {
      g_shop_secret_item_volumes_field =
        g_mono.mono_class_get_field_from_name(g_shop_manager_class, "secretItemVolumes");
    }
    if (g_item_manager_class && !g_item_manager_get_all_items) {
      const char* candidates[] = {
          config::kItemManagerGetAllItemsMethod,
          "GetAllItemsInScene",
          "GetAllItems",
          "GetAllItemVolumes",
          "GetPurchasedItems",
      };
      const int arg_counts[] = { 0, 1 };
      for (const char* name : candidates) {
        for (int argc : arg_counts) {
          g_item_manager_get_all_items =
            g_mono.mono_class_get_method_from_name(g_item_manager_class, name, argc);
          if (g_item_manager_get_all_items) {
            g_item_manager_get_all_items_argc = argc;
            std::ostringstream oss;
            oss << "Resolved ItemManager method: " << name << " argc=" << argc;
            AppendLog(oss.str());
            break;
          }
        }
        if (g_item_manager_get_all_items) {
          break;
        }
      }
      if (!g_item_manager_get_all_items) {
        AppendLog("Failed to resolve ItemManager::GetAllItemVolumesInScene (or variants)");
      }
    }

    // Alternative item population methods in SemiFunc class
    if (g_semi_func_class && !g_semi_func_shop_populate) {
      g_semi_func_shop_populate = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, "ShopPopulateItemVolumes", 0);
      if (g_semi_func_shop_populate) {
        AppendLog("Resolved SemiFunc::ShopPopulateItemVolumes");
      }
    }
    if (g_semi_func_class && !g_semi_func_truck_populate) {
      g_semi_func_truck_populate = g_mono.mono_class_get_method_from_name(
        g_semi_func_class, "TruckPopulateItemVolumes", 0);
      if (g_semi_func_truck_populate) {
        AppendLog("Resolved SemiFunc::TruckPopulateItemVolumes");
      }
    }

    // Try to resolve FindObjectsOfType (generic) as fallback
    // First find Object class
    MonoClass* unity_object_class = nullptr;
    if (g_unity_image) {
      unity_object_class = g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Object");
    }
    if (!unity_object_class && g_image) {
      unity_object_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "Object");
    }
    if (!unity_object_class) {
      unity_object_class = FindClassAnyAssembly("UnityEngine", "Object");
    }

    if (unity_object_class) {
      if (!g_object_find_objects_of_type) {
        g_object_find_objects_of_type =
          g_mono.mono_class_get_method_from_name(unity_object_class, "FindObjectsOfType", 1);
        if (g_object_find_objects_of_type) {
          AppendLog("Resolved Object::FindObjectsOfType(Type) method");
        }
      }
      if (!g_object_find_objects_of_type_include_inactive) {
        g_object_find_objects_of_type_include_inactive =
          g_mono.mono_class_get_method_from_name(unity_object_class, "FindObjectsOfType", 2);
        if (g_object_find_objects_of_type_include_inactive) {
          AppendLog("Resolved Object::FindObjectsOfType(Type, bool) method");
        }
      }
      // Keep item-volume cached aliases (same signatures) for other call sites
      if (!g_find_objects_of_type_itemvolume) g_find_objects_of_type_itemvolume = g_object_find_objects_of_type;
      if (!g_find_objects_of_type_itemvolume_include_inactive)
        g_find_objects_of_type_itemvolume_include_inactive = g_object_find_objects_of_type_include_inactive;
    }

    // Resolve Resources::FindObjectsOfTypeAll(Type) for inactive objects
    if (!g_resources_find_objects_of_type_all) {
      if (!g_resources_class) {
        if (g_unity_image) {
          g_resources_class = g_mono.mono_class_from_name(g_unity_image, "UnityEngine", "Resources");
        }
        if (!g_resources_class && g_image) {
          g_resources_class = g_mono.mono_class_from_name(g_image, "UnityEngine", "Resources");
        }
        if (!g_resources_class) {
          g_resources_class = FindClassAnyAssembly("UnityEngine", "Resources");
        }
      }
      if (g_resources_class && !g_resources_find_objects_of_type_all) {
        g_resources_find_objects_of_type_all =
          g_mono.mono_class_get_method_from_name(g_resources_class, "FindObjectsOfTypeAll", 1);
        if (g_resources_find_objects_of_type_all) {
          AppendLog("Resolved Resources::FindObjectsOfTypeAll(Type) method");
        }
      }
    }

    // Resolve ItemVolume class for fallback enumerations
    if (!g_item_volume_class) {
      if (g_image) {
        g_item_volume_class = g_mono.mono_class_from_name(g_image, "", "ItemVolume");
      }
      if (!g_item_volume_class) {
        g_item_volume_class = FindClassAnyAssembly("", "ItemVolume");
      }
      if (g_item_volume_class) {
        AppendLog("Resolved ItemVolume class");
      }
      else {
        AppendLog("Failed to resolve ItemVolume class");
      }
    }

    // ItemAttributes (value/type) and ItemVolume->itemAttributes
    if (!g_item_attributes_class) {
      g_item_attributes_class = g_mono.mono_class_from_name(g_image, "", "ItemAttributes");
      if (!g_item_attributes_class) {
        g_item_attributes_class = FindClassAnyAssembly("", "ItemAttributes");
      }
      if (g_item_attributes_class) {
        AppendLog("Resolved ItemAttributes class");
      }
      else {
        AppendLogOnce("ItemAttributes_class_missing", "Failed to resolve ItemAttributes class");
      }
    }
    if (g_item_attributes_class && !g_item_attributes_value_field) {
      g_item_attributes_value_field =
        g_mono.mono_class_get_field_from_name(g_item_attributes_class, "value");
    }
    if (g_item_attributes_class && !g_item_attributes_item_field) {
      g_item_attributes_item_field =
        g_mono.mono_class_get_field_from_name(g_item_attributes_class, "item");
    }
    if (g_item_attributes_class && !g_item_attributes_item_type_field) {
      g_item_attributes_item_type_field =
        g_mono.mono_class_get_field_from_name(g_item_attributes_class, "itemType");
    }
    if (g_item_attributes_class && !g_item_attributes_item_name_field) {
      g_item_attributes_item_name_field =
        g_mono.mono_class_get_field_from_name(g_item_attributes_class, "itemName");
      if (g_item_attributes_item_name_field) {
        AppendLog("Resolved ItemAttributes::itemName field");
      }
    }
    if (g_item_attributes_class && !g_item_attributes_instance_name_field) {
      g_item_attributes_instance_name_field =
        g_mono.mono_class_get_field_from_name(g_item_attributes_class, "instanceName");
      if (g_item_attributes_instance_name_field) {
        AppendLog("Resolved ItemAttributes::instanceName field");
      }
    }
    if (g_item_attributes_class && !g_item_attributes_type_obj && g_mono.mono_class_get_type &&
      g_mono.mono_type_get_object) {
      MonoType* t = g_mono.mono_class_get_type(g_item_attributes_class);
      if (t) {
        g_item_attributes_type_obj = g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), t);
      }
    }
    if (g_item_volume_class && !g_item_volume_item_attributes_field) {
      g_item_volume_item_attributes_field =
        g_mono.mono_class_get_field_from_name(g_item_volume_class, "itemAttributes");
      if (!g_item_volume_item_attributes_field) {
        AppendLogOnce("ItemVolume_itemAttributes_missing", "ItemVolume::itemAttributes field not found");
      }
    }
    if (!g_item_class) {
      g_item_class = g_mono.mono_class_from_name(g_image, "", "Item");
      if (!g_item_class) {
        g_item_class = FindClassAnyAssembly("", "Item");
      }
      if (g_item_class) {
        AppendLog("Resolved Item class");
      }
    }
    if (g_item_class && !g_item_item_name_field) {
      g_item_item_name_field = g_mono.mono_class_get_field_from_name(g_item_class, "itemName");
      if (g_item_item_name_field) {
        AppendLog("Resolved Item::itemName field");
      }
    }
    if (!g_valuable_object_class) {
      g_valuable_object_class = g_mono.mono_class_from_name(
        g_image, config::kValuableObjectNamespace, config::kValuableObjectClass);
      if (!g_valuable_object_class) {
        g_valuable_object_class = FindClassAnyAssembly(
          config::kValuableObjectNamespace, config::kValuableObjectClass);
      }
      if (g_valuable_object_class) {
        AppendLog("Resolved ValuableObject class");
      }
    }
    if (g_valuable_object_class && !g_valuable_object_value_field) {
      const char* value_fields[] = { "dollarValueCurrent", "dollarValueOriginal", "value" };
      for (const char* n : value_fields) {
        g_valuable_object_value_field = g_mono.mono_class_get_field_from_name(g_valuable_object_class, n);
        if (g_valuable_object_value_field) {
          AppendLog(std::string("Resolved ValuableObject value field as: ") + n);
          break;
        }
      }
    }
    if (g_valuable_object_class && !g_valuable_object_type_obj &&
      g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
      MonoType* t = g_mono.mono_class_get_type(g_valuable_object_class);
      if (t) {
        g_valuable_object_type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), t);
      }
    }
    if (!g_valuable_director_class) {
      g_valuable_director_class = g_mono.mono_class_from_name(g_image, "", "ValuableDirector");
      if (!g_valuable_director_class) {
        g_valuable_director_class = FindClassAnyAssembly("", "ValuableDirector");
      }
      if (g_valuable_director_class) {
        AppendLog("Resolved ValuableDirector class");
      }
    }
    if (g_valuable_director_class && !g_valuable_director_vtable) {
      g_valuable_director_vtable = g_mono.mono_class_vtable(g_domain, g_valuable_director_class);
    }
    if (g_valuable_director_class && !g_valuable_director_instance_field) {
      g_valuable_director_instance_field =
        g_mono.mono_class_get_field_from_name(g_valuable_director_class, "instance");
    }
    if (g_valuable_director_class && !g_valuable_director_list_field) {
      g_valuable_director_list_field =
        g_mono.mono_class_get_field_from_name(g_valuable_director_class, "valuableList");
    }

    // ValuableDiscover (native highlight UI)
    if (!g_valuable_discover_class) {
      g_valuable_discover_class = g_mono.mono_class_from_name(g_image, "", "ValuableDiscover");
      if (!g_valuable_discover_class) {
        g_valuable_discover_class = FindClassAnyAssembly("", "ValuableDiscover");
      }
      if (g_valuable_discover_class) {
        AppendLog("Resolved ValuableDiscover class");
      }
    }
    if (g_valuable_discover_class && !g_valuable_discover_vtable) {
      g_valuable_discover_vtable = g_mono.mono_class_vtable(g_domain, g_valuable_discover_class);
    }
    if (g_valuable_discover_class && !g_valuable_discover_instance_field) {
      g_valuable_discover_instance_field =
        g_mono.mono_class_get_field_from_name(g_valuable_discover_class, "instance");
    }
    if (g_valuable_discover_class && !g_valuable_discover_new_method) {
      g_valuable_discover_new_method =
        g_mono.mono_class_get_method_from_name(g_valuable_discover_class, "New", 2);
    }
    if (g_valuable_discover_class && !g_valuable_discover_hide_method) {
      g_valuable_discover_hide_method =
        g_mono.mono_class_get_method_from_name(g_valuable_discover_class, "Hide", 0);
    }
    if (g_valuable_discover_class && !g_valuable_discover_hide_timer_field) {
      g_valuable_discover_hide_timer_field =
        g_mono.mono_class_get_field_from_name(g_valuable_discover_class, "hideTimer");
      g_valuable_discover_hide_alpha_field =
        g_mono.mono_class_get_field_from_name(g_valuable_discover_class, "hideAlpha");
    }

    // LightInteractableFadeRemove (prevent timed fade)
    if (!g_light_fade_class) {
      g_light_fade_class = g_mono.mono_class_from_name(g_image, "", "LightInteractableFadeRemove");
      if (!g_light_fade_class) {
        g_light_fade_class = FindClassAnyAssembly("", "LightInteractableFadeRemove");
      }
    }
    if (g_light_fade_class && !g_light_fade_is_fading_field) {
      g_light_fade_is_fading_field =
        g_mono.mono_class_get_field_from_name(g_light_fade_class, "isFading");
      g_light_fade_current_time_field =
        g_mono.mono_class_get_field_from_name(g_light_fade_class, "currentTime");
      g_light_fade_fade_duration_field =
        g_mono.mono_class_get_field_from_name(g_light_fade_class, "fadeDuration");
      if (!g_light_fade_type) {
        g_light_fade_type = g_mono.mono_class_get_type(g_light_fade_class);
      }
    }
    // Enemies
    if (!g_enemy_rigidbody_class) {
      g_enemy_rigidbody_class = g_mono.mono_class_from_name(
        g_image, "", "EnemyRigidbody");
      if (!g_enemy_rigidbody_class) {
        g_enemy_rigidbody_class = FindClassAnyAssembly("", "EnemyRigidbody");
      }
      if (g_enemy_rigidbody_class) {
        AppendLog("Resolved EnemyRigidbody class");
        if (k_enable_enemy_awake_hook) {
          InstallEnemyAwakeHook();
        } else {
          AppendLogOnce("EnemyAwake_hook_disabled",
                        "EnemyRigidbody::Awake hook disabled for stability");
        }
      }
      else {
        AppendLogOnce("EnemyRigidbody_class_missing", "Failed to resolve EnemyRigidbody class");
      }
    }

    return true;
  }

  MonoObject* FindLocalPlayerFromList() {
    if (!g_game_director_instance_field || !g_game_director_player_list_field) {
      AppendLogOnce("PlayerList_fields_missing",
        "Player list lookup skipped: GameDirector fields missing");
      return nullptr;
    }
    if (!g_player_avatar_is_local_field) {
      AppendLogOnce("PlayerList_isLocal_missing",
        "Player list lookup skipped: PlayerAvatar::isLocal missing");
      return nullptr;
    }

    MonoObject* director_instance = nullptr;
    if (!g_game_director_vtable) {
      AppendLogOnce("PlayerList_vtable_missing",
        "Player list lookup skipped: GameDirector vtable missing");
      return nullptr;
    }

    g_mono.mono_field_static_get_value(g_game_director_vtable, g_game_director_instance_field,
      &director_instance);
    if (!director_instance) {
      AppendLogOnce("GameDirector_instance_null", "GameDirector::instance is null");
      return nullptr;
    }

    MonoObject* player_list = nullptr;
    g_mono.mono_field_get_value(director_instance, g_game_director_player_list_field, &player_list);
    if (!player_list) {
      AppendLogOnce("PlayerList_null", "GameDirector::PlayerList is null");
      return nullptr;
    }

    MonoClass* list_class = g_mono.mono_object_get_class(player_list);
    if (!list_class) {
      AppendLogOnce("PlayerList_class_null", "PlayerList class is null");
      return nullptr;
    }

    MonoClassField* items_field = g_mono.mono_class_get_field_from_name(list_class, "_items");
    MonoClassField* size_field = g_mono.mono_class_get_field_from_name(list_class, "_size");
    if (!items_field || !size_field) {
      AppendLogOnce("PlayerList_fields_missing2", "PlayerList fields (_items/_size) not found");
      return nullptr;
    }

    int size = 0;
    MonoArray* items = nullptr;
    g_mono.mono_field_get_value(player_list, size_field, &size);
    g_mono.mono_field_get_value(player_list, items_field, &items);
    if (!items || size <= 0) {
      AppendLogOnce("PlayerList_empty", "PlayerList is empty or items array is null");
      return nullptr;
    }

    // Defensive bounds: clamp to prevent bad metadata from walking junk.
    int max_length = static_cast<int>(items->max_length);
    int limit = size;
    if (max_length <= 0) {
      AppendLogOnce("PlayerList_maxlen_invalid", "PlayerList items max_length invalid (<=0)");
      return nullptr;
    }
    if (max_length < limit) {
      limit = max_length;
    }
    if (limit > 512) {
      AppendLogOnce("PlayerList_limit_clamped",
        "PlayerList limit clamped to 512 to avoid overrun");
      limit = 512;
    }

    for (int i = 0; i < limit; ++i) {
      MonoObject* player = reinterpret_cast<MonoObject*>(items->vector[i]);
      if (!player) {
        continue;
      }

      MonoClass* player_class = g_mono.mono_object_get_class(player);
      if (!player_class) {
        continue;
      }
      if (g_player_avatar_class &&
          player_class != g_player_avatar_class &&
          !IsSubclassOf(player_class, g_player_avatar_class)) {
        continue;
      }

      bool is_local = false;
      bool read_ok = true;
#ifdef _MSC_VER
      read_ok = FieldGetBoolSafeSeh(player, g_player_avatar_is_local_field, is_local);
#else
      g_mono.mono_field_get_value(player, g_player_avatar_is_local_field, &is_local);
#endif
      if (!read_ok) {
        AppendLogOnce("PlayerList_isLocal_read_fault",
          "PlayerList scan: reading PlayerAvatar::isLocal triggered SEH; skipped entry");
        continue;
      }
      if (is_local) {
        AppendLogOnce("LocalPlayer_from_list", "Local player found via PlayerList");
        return player;
      }
    }

    AppendLogOnce("LocalPlayer_not_in_list", "Local player not found in PlayerList");
    return nullptr;
  }

  bool TryGetPositionFromTransform(MonoObject* transform_obj, PlayerState& out_state, bool log_fail = true) {
    if (!transform_obj || IsUnityNull(transform_obj)) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: transform_obj is null");
      return false;
    }

    MonoClass* transform_class = g_mono.mono_object_get_class(transform_obj);
    if (!transform_class) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: transform_class is null");
      return false;
    }

    MonoMethod* get_position = g_mono.mono_class_get_method_from_name(
      transform_class, config::kTransformGetPositionMethod, 0);
    if (!get_position) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: get_position method not found");
      return false;
    }

    MonoObject* exception = nullptr;
    MonoObject* position_obj =
      g_mono.mono_runtime_invoke(get_position, transform_obj, nullptr, &exception);
    if (exception || !position_obj) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: mono_runtime_invoke failed or returned null");
      return false;
    }

    if (g_mono.mono_object_unbox) {
      void* data = g_mono.mono_object_unbox(position_obj);
      if (data) {
        float tmp[3] = {};
        std::memcpy(tmp, data, sizeof(tmp));
        out_state.x = tmp[0];
        out_state.y = tmp[1];
        out_state.z = tmp[2];
        out_state.has_position = true;
        return true;
      }
    }

    MonoClass* vec_class = g_mono.mono_object_get_class(position_obj);
    if (!vec_class) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: vector class is null");
      return false;
    }
    static MonoClassField* x_field = nullptr;
    static MonoClassField* y_field = nullptr;
    static MonoClassField* z_field = nullptr;
    if (!x_field) x_field = g_mono.mono_class_get_field_from_name(vec_class, "x");
    if (!y_field) y_field = g_mono.mono_class_get_field_from_name(vec_class, "y");
    if (!z_field) z_field = g_mono.mono_class_get_field_from_name(vec_class, "z");
    if (!x_field || !y_field || !z_field) {
      if (log_fail) AppendLog("TryGetPositionFromTransform: vector field(s) missing");
      return false;
    }
    g_mono.mono_field_get_value(position_obj, x_field, &out_state.x);
    g_mono.mono_field_get_value(position_obj, y_field, &out_state.y);
    g_mono.mono_field_get_value(position_obj, z_field, &out_state.z);
    out_state.has_position = true;
    return true;
  }

  bool TryGetPosition(MonoObject* player_avatar_obj, PlayerState& out_state) {
    if (!player_avatar_obj) {
      AppendLog("TryGetPosition: player_avatar_obj is null");
      return false;
    }
    if (!g_player_avatar_transform_field) {
      AppendLog("TryGetPosition: transform field unresolved");
      return false;
    }

    MonoObject* transform_obj = nullptr;
    g_mono.mono_field_get_value(player_avatar_obj, g_player_avatar_transform_field, &transform_obj);
    if (!transform_obj) {
      AppendLog("TryGetPosition: transform_obj is null");
      return false;
    }

    // Reuse generic transform path.
    return TryGetPositionFromTransform(transform_obj, out_state);
  }

  bool UnityHelperReadEnemySnapshot(std::vector<PlayerState>& out_enemies) {
    (void)out_enemies;
    AppendLogOnce("UnityHelperReadEnemySnapshot_missing",
      "UnityHelperReadEnemySnapshot is not available (EnemyScanner data missing)");
    return false;
  }

  bool EnsureMinHookReady() {
    static bool inited = false;
    if (inited) return true;
    MH_STATUS st = MH_Initialize();
    if (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) {
      inited = true;
      return true;
    }
    AppendLog("MinHook initialize failed");
    return false;
  }

// Non-ASCII comment normalized.
  bool IsUnityNull(MonoObject* obj) {
    if (!obj) return true;
    if (!g_unity_object_op_eq) return false;  // Non-ASCII comment normalized.
    void* args[2] = { obj, nullptr };
    MonoObject* ret = SafeInvoke(g_unity_object_op_eq, nullptr, args, "Object.op_Equality");
    if (!ret || !g_mono.mono_object_unbox) return false;
    return *static_cast<bool*>(g_mono.mono_object_unbox(ret));
  }

  inline bool CaseContains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    std::string h = hay;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
  }

  void EnemyCacheAdd(MonoObject* obj) {
    if (!obj || !g_mono.mono_gchandle_new || !g_mono.mono_gchandle_get_target) return;
    std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
    for (uint32_t h : g_enemy_cache) {
      MonoObject* cur = g_mono.mono_gchandle_get_target(h);
      if (cur == obj) return;
    }
    if (g_enemy_cache.size() >= 2048) return;
    uint32_t handle = g_mono.mono_gchandle_new(obj, 0);
    if (handle) {
      g_enemy_cache.push_back(handle);
      g_enemy_cache_last_refresh = GetTickCount64();
      g_enemies_ready.store(true, std::memory_order_relaxed);
    }
  }

  void EnemyCachePruneDead() {
    if (!g_mono.mono_gchandle_get_target || !g_mono.mono_gchandle_free) return;
    std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
    auto it = g_enemy_cache.begin();
    while (it != g_enemy_cache.end()) {
      MonoObject* obj = g_mono.mono_gchandle_get_target(*it);
      if (!obj || IsUnityNull(obj)) {
        g_mono.mono_gchandle_free(*it);
        it = g_enemy_cache.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void __stdcall EnemyAwakeHook(MonoObject* self) {
    EnemyCacheAdd(self);
    if (g_enemy_awake_orig) {
      g_enemy_awake_orig(self);
    }
  }

  void InstallEnemyAwakeHook() {
    if (g_enemy_awake_hooked || !g_enemy_rigidbody_class || !g_mono.mono_class_get_method_from_name) return;
    if (!EnsureMinHookReady()) return;
    if (!g_enemy_awake_method) {
      g_enemy_awake_method = g_mono.mono_class_get_method_from_name(g_enemy_rigidbody_class, "Awake", 0);
      if (!g_enemy_awake_method) {
        AppendLogOnce("EnemyAwake_notfound", "EnemyRigidbody::Awake not found; cache hook skipped");
        return;
      }
    }
    void* native_ptr = g_mono.mono_compile_method ? g_mono.mono_compile_method(g_enemy_awake_method) : nullptr;
    if (!native_ptr) {
      AppendLogOnce("EnemyAwake_compile_fail", "mono_compile_method failed for EnemyRigidbody::Awake");
      return;
    }
    if (MH_CreateHook(native_ptr, reinterpret_cast<LPVOID>(EnemyAwakeHook),
      reinterpret_cast<LPVOID*>(&g_enemy_awake_orig)) != MH_OK) {
      AppendLogOnce("EnemyAwake_create_fail", "MH_CreateHook failed for EnemyRigidbody::Awake");
      return;
    }
    if (MH_EnableHook(native_ptr) != MH_OK) {
      AppendLogOnce("EnemyAwake_enable_fail", "MH_EnableHook failed for EnemyRigidbody::Awake");
      MH_RemoveHook(native_ptr);
      return;
    }
    g_enemy_awake_hooked = true;
    AppendLogOnce("EnemyAwake_hook_ok", "Installed hook for EnemyRigidbody::Awake");
  }

  void __stdcall RoundDirectorUpdateHook(MonoObject* self) {
    if (g_round_director_update_orig) {
      g_round_director_update_orig(self);
    }
    if (!self || !g_force_round_haul_enabled.load(std::memory_order_relaxed)) {
      return;
    }
    int cur = g_force_round_haul_value.load(std::memory_order_relaxed);
    int goal = g_force_round_haul_goal.load(std::memory_order_relaxed);
    if (cur < 0) cur = 0;
    if (goal < 0) goal = cur;

    WriteFieldNumber(self, g_round_current_haul_field, cur);
    WriteFieldNumber(self, g_round_current_haul_max_field, cur);
    WriteFieldNumber(self, g_round_haul_goal_field, goal);
    WriteFieldNumber(self, g_round_extraction_haul_goal_field, goal);
    int surplus = cur - goal;
    if (surplus < 0) surplus = 0;
    WriteFieldNumber(self, g_round_extraction_point_surplus_field, surplus);

    if (g_round_extraction_point_current_field && g_mono.mono_field_get_value &&
        g_mono.mono_object_get_class) {
      MonoObject* ep_obj = nullptr;
      g_mono.mono_field_get_value(self, g_round_extraction_point_current_field, &ep_obj);
      if (ep_obj) {
        MonoClass* ep_cls = g_mono.mono_object_get_class(ep_obj);
        if (ep_cls && g_extraction_point_class &&
            (ep_cls == g_extraction_point_class || IsSubclassOf(ep_cls, g_extraction_point_class))) {
          WriteFieldNumber(ep_obj, g_extraction_point_haul_current_field, cur);
          WriteFieldNumber(ep_obj, g_extraction_point_haul_goal_field, goal);
          WriteFieldNumber(ep_obj, g_extraction_point_extraction_haul_field, cur);
          if (g_extraction_point_run_currency_before_field) {
            int baseline = 0;
            if (ComputeRunCurrencyBeforeForTarget(cur, baseline)) {
              WriteFieldNumber(ep_obj, g_extraction_point_run_currency_before_field, baseline);
            }
          }
        }
      }
    }
  }

  void InstallRoundDirectorUpdateHook() {
    if (!k_enable_round_update_hook || g_round_director_update_hooked || !g_round_director_class ||
        !g_mono.mono_class_get_method_from_name) {
      return;
    }
    if (!EnsureMinHookReady()) return;
    if (!g_round_director_update_method) {
      g_round_director_update_method =
        g_mono.mono_class_get_method_from_name(g_round_director_class, "Update", 0);
      if (!g_round_director_update_method) {
        AppendLogOnce("RoundUpdate_hook_notfound", "RoundDirector::Update not found; haul force hook skipped");
        return;
      }
    }
    void* native_ptr =
      g_mono.mono_compile_method ? g_mono.mono_compile_method(g_round_director_update_method) : nullptr;
    if (!native_ptr) {
      AppendLogOnce("RoundUpdate_hook_compile_fail", "mono_compile_method failed for RoundDirector::Update");
      return;
    }
    if (MH_CreateHook(native_ptr, reinterpret_cast<LPVOID>(RoundDirectorUpdateHook),
                      reinterpret_cast<LPVOID*>(&g_round_director_update_orig)) != MH_OK) {
      AppendLogOnce("RoundUpdate_hook_create_fail", "MH_CreateHook failed for RoundDirector::Update");
      return;
    }
    if (MH_EnableHook(native_ptr) != MH_OK) {
      AppendLogOnce("RoundUpdate_hook_enable_fail", "MH_EnableHook failed for RoundDirector::Update");
      MH_RemoveHook(native_ptr);
      return;
    }
    g_round_director_update_hooked = true;
    AppendLogOnce("RoundUpdate_hook_ok", "Installed hook for RoundDirector::Update");
  }

// Non-ASCII comment normalized.
  bool AddEnemyFromObject(MonoObject* enemy_obj, std::vector<PlayerState>& out_enemies) {
    if (!enemy_obj) return false;
    MonoObject* tr = SafeInvoke(g_component_get_transform, enemy_obj, nullptr, "EnemyRigidbody.get_transform");
    if (!tr) return false;
    PlayerState st{};
    st.category = PlayerState::Category::kEnemy;
    if (!TryGetPositionFromTransform(tr, st, false)) return false;
    if (g_component_get_game_object) {
      MonoObject* go = SafeInvoke(g_component_get_game_object, enemy_obj, nullptr, "EnemyRigidbody.get_gameObject");
      if (go && g_unity_object_get_name) {
        MonoObject* name_obj = SafeInvoke(g_unity_object_get_name, go, nullptr, "GameObject.get_name");
        if (name_obj) {
          st.name = MonoStringToUtf8(name_obj);
          st.has_name = !st.name.empty();
        }
      }
      if (go && g_game_object_get_layer) {
        MonoObject* layer_obj = SafeInvoke(g_game_object_get_layer, go, nullptr, "GameObject.get_layer");
        if (layer_obj && g_mono.mono_object_unbox) {
          st.layer = *static_cast<int*>(g_mono.mono_object_unbox(layer_obj));
          st.has_layer = true;
        }
      }
    }
    out_enemies.push_back(st);
    return true;
  }

  bool ScanEnemiesDirect(std::vector<PlayerState>& out_enemies, int max_count) {
    if (!g_enemy_rigidbody_class) return false;
    if (!g_object_find_objects_of_type_include_inactive && !g_object_find_objects_of_type) {
      return false;
    }

    MonoType* enemy_type =
      g_mono.mono_class_get_type ? g_mono.mono_class_get_type(g_enemy_rigidbody_class) : nullptr;
    MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
    MonoObject* enemy_type_obj =
      (enemy_type && g_mono.mono_type_get_object) ? g_mono.mono_type_get_object(dom, enemy_type) : nullptr;
    if (!enemy_type_obj) return false;

    MonoObject* arr_obj = nullptr;
    if (g_object_find_objects_of_type_include_inactive) {
      bool include_inactive = true;
      void* args2[2] = { enemy_type_obj, &include_inactive };
      arr_obj = SafeInvoke(g_object_find_objects_of_type_include_inactive, nullptr, args2,
                           "Object.FindObjectsOfType(EnemyRigidbody, true)");
    }
    if (!arr_obj && g_object_find_objects_of_type) {
      void* args1[1] = { enemy_type_obj };
      arr_obj = SafeInvoke(g_object_find_objects_of_type, nullptr, args1,
                           "Object.FindObjectsOfType(EnemyRigidbody)");
    }

    MonoArray* arr = arr_obj ? reinterpret_cast<MonoArray*>(arr_obj) : nullptr;
    if (!IsValidArray(arr)) return false;

    int limit = static_cast<int>(arr->max_length);
    if (limit > max_count) limit = max_count;
    if (limit > 1024) limit = 1024;

    const size_t before = out_enemies.size();
    for (int i = 0; i < limit; ++i) {
      MonoObject* enemy_obj = reinterpret_cast<MonoObject*>(arr->vector[i]);
      if (!enemy_obj || IsUnityNull(enemy_obj)) continue;
      if (AddEnemyFromObject(enemy_obj, out_enemies) &&
          static_cast<int>(out_enemies.size() - before) >= max_count) {
        break;
      }
    }
    return out_enemies.size() > before;
  }

// Non-ASCII comment normalized.
  bool RefreshEnemyCache() {
    if (g_enemy_cache_disabled) return false;
    static uint64_t last_enemy_empty_ms = 0;
    uint64_t now_ms = GetTickCount64();
    if (!g_enemies_ready.load(std::memory_order_relaxed) && now_ms - last_enemy_empty_ms < 2000) {
      return false;  // Non-ASCII comment normalized.
    }
// Non-ASCII comment normalized.
    PlayerState lp{};
    if (!MonoGetLocalPlayerState(lp) || !lp.has_position) {
      return false;
    }
    EnemyCachePruneDead();
    {
      std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
      if (!g_enemy_cache.empty()) {
        g_enemy_cache_last_refresh = GetTickCount64();
        return true;  // Non-ASCII comment normalized.
      }
    }
    if (!g_enemy_rigidbody_class) {
      AppendLogOnce("EnemyCache_class_missing", "RefreshEnemyCache: EnemyRigidbody class unresolved");
      return false;
    }
    if (!g_object_find_objects_of_type_include_inactive && !g_object_find_objects_of_type) {
      AppendLogOnce("EnemyCache_find_missing", "RefreshEnemyCache: FindObjectsOfType methods unresolved");
      return false;
    }

    MonoType* enemy_type = g_mono.mono_class_get_type ? g_mono.mono_class_get_type(g_enemy_rigidbody_class) : nullptr;
    MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
    MonoObject* enemy_type_obj =
      (enemy_type && g_mono.mono_type_get_object) ? g_mono.mono_type_get_object(dom, enemy_type) : nullptr;
    if (!enemy_type_obj) {
      AppendLogOnce("EnemyCache_type_null", "RefreshEnemyCache: EnemyRigidbody type object null");
      return false;
    }

    MonoObject* arr_obj = nullptr;
    if (g_object_find_objects_of_type_include_inactive) {
      bool include_inactive = true;
      void* args2[2] = { enemy_type_obj, &include_inactive };
      arr_obj = SafeInvoke(g_object_find_objects_of_type_include_inactive, nullptr, args2,
        "Object.FindObjectsOfType(EnemyRigidbody, true)");
    }
    if (!arr_obj && g_object_find_objects_of_type) {
      void* args1[1] = { enemy_type_obj };
      arr_obj = SafeInvoke(g_object_find_objects_of_type, nullptr, args1,
        "Object.FindObjectsOfType(EnemyRigidbody)");
    }

    MonoArray* arr = arr_obj ? reinterpret_cast<MonoArray*>(arr_obj) : nullptr;
    if (!IsValidArray(arr)) {
      AppendLogOnce("EnemyCache_empty", "RefreshEnemyCache: FindObjectsOfType returned empty/invalid array");
      std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
      ClearEnemyCacheHandlesUnlocked();
      last_enemy_empty_ms = now_ms;
      return false;
    }

    int lim = static_cast<int>(arr->max_length);
    if (lim > 2048) lim = 2048;
    std::vector<uint32_t> tmp;
    tmp.reserve(lim);
    for (int i = 0; i < lim; ++i) {
      MonoObject* enemy_obj = reinterpret_cast<MonoObject*>(arr->vector[i]);
      if (!enemy_obj) continue;
      if (!g_mono.mono_gchandle_new) continue;
      uint32_t h = g_mono.mono_gchandle_new(enemy_obj, 0);
      if (h) tmp.push_back(h);
    }
    size_t cached_sz = 0;
    {
      std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
      ClearEnemyCacheHandlesUnlocked();
      g_enemy_cache.swap(tmp);
      cached_sz = g_enemy_cache.size();
    }
    g_enemy_cache_last_refresh = GetTickCount64();
    g_enemies_ready.store(true, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << "RefreshEnemyCache: cached " << cached_sz << " EnemyRigidbody objects";
    AppendLog(oss.str());
    return !g_enemy_cache.empty();
  }

#ifdef _MSC_VER
// Non-ASCII comment normalized.
  static bool RefreshEnemyCacheSafe() {
    __try {
      return RefreshEnemyCache();
    }
    __except (LogCrash("RefreshEnemyCacheSafe", GetExceptionCode(), GetExceptionInformation())) {
      g_enemy_cache_disabled = true;
      return false;
    }
  }
#else
  static bool RefreshEnemyCacheSafe() { return RefreshEnemyCache(); }
#endif

  // Safe invoke helper to guard mono_runtime_invoke with C++ exceptions only.
  static MonoObject* SafeInvoke(MonoMethod* method, MonoObject* obj, void** args, const char* tag) {
    if (!method || !g_mono.mono_runtime_invoke) return nullptr;
    try {
      MonoObject* exc = nullptr;
      MonoObject* ret = g_mono.mono_runtime_invoke(method, obj, args, &exc);
      if (exc) return nullptr;
      return ret;
    }
    catch (...) {
      AppendLogOnce(tag ? tag : "SafeInvoke", "SafeInvoke caught exception");
      return nullptr;
    }
  }

#ifdef _MSC_VER
  // Keep SEH in a tiny helper without C++ temporaries to avoid C2712.
  static bool FieldGetBoolSafeSeh(MonoObject* obj, MonoClassField* field, bool& out_value) {
    out_value = false;
    if (!obj || !field || !g_mono.mono_field_get_value) {
      return false;
    }
    __try {
      g_mono.mono_field_get_value(obj, field, &out_value);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      out_value = false;
      return false;
    }
  }
#endif

  bool TryGetHealth(MonoObject* player_avatar_obj, PlayerState& out_state) {
    if (!player_avatar_obj) {
      AppendLog("TryGetHealth: player_avatar_obj is null");
      return false;
    }
    if (!g_player_avatar_health_field) {
      AppendLog("TryGetHealth: health field unresolved");
      return false;
    }

    MonoObject* health_obj = nullptr;
    g_mono.mono_field_get_value(player_avatar_obj, g_player_avatar_health_field, &health_obj);
    if (!health_obj) {
      AppendLog("TryGetHealth: health_obj is null");
      return false;
    }

    MonoClass* health_class = g_mono.mono_object_get_class(health_obj);
    if (!health_class) {
      AppendLog("TryGetHealth: health_class is null");
      return false;
    }

    MonoClassField* health_field = g_player_health_value_field;
    MonoClassField* max_field = g_player_health_max_field;

    if (!health_field) {
      health_field = g_mono.mono_class_get_field_from_name(health_class, config::kPlayerHealthValueField);
      if (!health_field) {
        AppendLog("TryGetHealth: health field not found");
        return false;
      }
      g_player_health_value_field = health_field;
    }
    if (!max_field) {
      max_field = g_mono.mono_class_get_field_from_name(health_class, config::kPlayerHealthMaxField);
      if (!max_field) {
        AppendLog("TryGetHealth: maxHealth field not found");
        return false;
      }
      g_player_health_max_field = max_field;
    }

    g_mono.mono_field_get_value(health_obj, health_field, &out_state.health);
    g_mono.mono_field_get_value(health_obj, max_field, &out_state.max_health);
    out_state.has_health = true;
    return true;
  }

  bool TryGetEnergy(MonoObject* player_avatar_obj, PlayerState& out_state) {
    // Stamina lives on PlayerController static instance.
    if (!g_player_controller_instance_field || !g_player_controller_vtable) {
      AppendLog("TryGetEnergy: PlayerController fields not resolved");
      return false;
    }

    MonoObject* controller_instance = nullptr;
    g_mono.mono_field_static_get_value(
      g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
    if (!controller_instance) {
      AppendLog("TryGetEnergy: PlayerController::instance is null");
      return false;
    }

    if (g_player_controller_energy_current_field) {
      float cur = 0.0f;
      g_mono.mono_field_get_value(
        controller_instance, g_player_controller_energy_current_field, &cur);
      out_state.energy = cur;
    }
    if (g_player_controller_energy_start_field) {
      float start = 0.0f;
      g_mono.mono_field_get_value(
        controller_instance, g_player_controller_energy_start_field, &start);
      out_state.max_energy = start;
    }

    out_state.has_energy = true;
    return true;
  }
}  // namespace

const std::string& MonoGetLogPath() {
  return InternalGetLogPath();
}

void MonoSetLogPath(const std::string& path_utf8) {
  InternalSetLogPath(path_utf8);
}

void SetCrashStage(const char* stage) {
  g_crash_stage.store(stage ? stage : "null", std::memory_order_relaxed);
}

long LogCrash(const char* where, unsigned long code, EXCEPTION_POINTERS* info) {
  std::ostringstream oss;
  oss << "CRASH in " << (where ? where : "<unknown>")
    << " code=0x" << std::hex << code
    << " addr=0x"
    << std::hex
    << (info && info->ExceptionRecord ? reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) : 0)
    << " stage=" << g_crash_stage.load(std::memory_order_relaxed);
  AppendLogInternal(LogLevel::kError, "crash", oss.str());
  WriteCrashReport(where, code, info);
  return EXCEPTION_EXECUTE_HANDLER;
}

bool MonoInitialize() {
  if (g_shutting_down) return false;
  LogEnvironmentOnce();
  return EnsureThreadAttached();
}

bool MonoBeginShutdown() {
  g_shutting_down = true;
  g_pending_cart_active = false;
  g_cart_apply_in_progress = false;
  if (g_round_update_patch_active && g_round_update_patch_addr &&
      g_round_update_patch_backup.size > 0) {
    RestorePatchedCode(g_round_update_patch_addr, g_round_update_patch_backup);
    g_round_update_patch_active = false;
    g_round_update_patch_addr = nullptr;
    g_round_update_patch_backup = {};
  }
  ClearEnemyCacheHandles();
  return true;
}

bool MonoIsShuttingDown() {
  return g_shutting_down;
}

bool MonoGetLocalPlayerInternal(LocalPlayerInfo& out_info) {
  if (g_shutting_down) return false;
  out_info = {};
  if (!CacheManagedRefs()) {
    AppendLog("CacheManagedRefs failed");
    return false;
  }

  MonoObject* local_player = nullptr;
  bool via_list = false;

  // Prefer static instance first: stable and cheap.
  if (!local_player && g_player_avatar_instance_field && g_player_avatar_vtable) {
    g_mono.mono_field_static_get_value(
      g_player_avatar_vtable, g_player_avatar_instance_field, &local_player);
    if (local_player) {
      AppendLogOnce("localplayer_static_resolved",
        "Local player resolved via PlayerAvatar static instance");
    }
    else {
      AppendLogOnce("localplayer_static_null", "PlayerAvatar static instance is null");
    }
  }
  else if (!local_player) {
    if (!g_player_avatar_instance_field) {
      AppendLogOnce("localplayer_static_field_unresolved",
        "Skipping static instance lookup: instance field unresolved");
    }
    else if (!g_player_avatar_vtable) {
      AppendLogOnce("localplayer_static_vtable_unresolved",
        "Skipping static instance lookup: vtable unresolved");
    }
  }

  // Fallback: SemiFunc::PlayerAvatarLocal (returns static instance in this title).
  if (!local_player && g_player_avatar_local_method) {
    MonoObject* exception = nullptr;
    local_player = g_mono.mono_runtime_invoke(
      g_player_avatar_local_method, nullptr, nullptr, &exception);
    if (exception) {
      local_player = nullptr;
      AppendLogOnce("localplayer_method_exception",
        "SemiFunc::PlayerAvatarLocal threw an exception");
    }
    else if (local_player) {
      AppendLogOnce("localplayer_method_resolved",
        "Local player resolved via SemiFunc::PlayerAvatarLocal");
    }
  }
  else if (!local_player && !g_player_avatar_local_method) {
    AppendLogOnce("localplayer_method_unresolved",
      "Skipping SemiFunc::PlayerAvatarLocal: method unresolved");
  }

  // Last fallback: PlayerList traversal can be noisy/unstable in some sessions.
  if (!local_player) {
    local_player = FindLocalPlayerFromList();
    if (local_player) {
      via_list = true;
    }
  }

  if (!local_player) {
    AppendLogOnce("localplayer_failed", "Failed to resolve local player via all strategies");
    return false;
  }

  bool is_local = false;
  if (g_player_avatar_is_local_field) {
#ifdef _MSC_VER
    bool read_ok = FieldGetBoolSafeSeh(local_player, g_player_avatar_is_local_field, is_local);
    if (!read_ok) {
      AppendLogOnce("localplayer_isLocal_read_fault",
        "MonoGetLocalPlayer: reading isLocal triggered SEH");
      is_local = false;
    }
#else
    g_mono.mono_field_get_value(local_player, g_player_avatar_is_local_field, &is_local);
#endif
  }

  AppendLogOnce("localplayer_found",
    "Local player found via one of the strategies (logging once)");

  out_info.object = local_player;
  out_info.is_local = is_local;
  out_info.via_player_list = via_list;
  return true;
}

#ifdef _MSC_VER
// Dedicated SEH wrapper to avoid C2712 in high-level functions.
static bool MonoGetLocalPlayerInternalSafe(LocalPlayerInfo& out_info) {
  __try {
    return MonoGetLocalPlayerInternal(out_info);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    out_info = {};
    return false;
  }
}
#endif

bool MonoGetLocalPlayer(LocalPlayerInfo& out_info) {
#ifdef _MSC_VER
  bool ok = MonoGetLocalPlayerInternalSafe(out_info);
  if (!ok) {
    AppendLogOnce("MonoGetLocalPlayer_seh_guard",
      "MonoGetLocalPlayer caught SEH and returned false (guard active)");
  }
  return ok;
#else
  return MonoGetLocalPlayerInternal(out_info);
#endif
}

bool MonoGetPlayerStateFromAvatar(void* player_avatar_obj, PlayerState& out_state) {
  out_state = {};
  if (!CacheManagedRefs()) {
    AppendLog("MonoGetPlayerStateFromAvatar: CacheManagedRefs failed");
    return false;
  }
  if (!player_avatar_obj) {
    AppendLog("MonoGetPlayerStateFromAvatar: player_avatar_obj is null");
    return false;
  }

  bool ok_position = TryGetPosition(static_cast<MonoObject*>(player_avatar_obj), out_state);
  bool ok_health = TryGetHealth(static_cast<MonoObject*>(player_avatar_obj), out_state);
  bool ok_energy = TryGetEnergy(static_cast<MonoObject*>(player_avatar_obj), out_state);
  return ok_position || ok_health || ok_energy;
}

bool MonoGetLocalPlayerState(PlayerState& out_state) {
  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoGetLocalPlayerState: failed to get local player");
    out_state = {};
    return false;
  }
  return MonoGetPlayerStateFromAvatar(info.object, out_state);
}

bool MonoSetLocalPlayerPosition(float x, float y, float z) {
  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoSetLocalPlayerPosition: failed to get local player");
    return false;
  }
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetLocalPlayerPosition: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_avatar_transform_field) {
    AppendLog("MonoSetLocalPlayerPosition: transform field unresolved");
    return false;
  }

  MonoObject* transform_obj = nullptr;
  g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
    g_player_avatar_transform_field, &transform_obj);
  if (!transform_obj) {
    AppendLog("MonoSetLocalPlayerPosition: transform_obj is null");
    return false;
  }

  MonoClass* transform_class = g_mono.mono_object_get_class(transform_obj);
  if (!transform_class) {
    AppendLog("MonoSetLocalPlayerPosition: transform_class is null");
    return false;
  }

  MonoMethod* set_position = g_mono.mono_class_get_method_from_name(
    transform_class, config::kTransformSetPositionMethod, 1);
  if (!set_position) {
    AppendLog("MonoSetLocalPlayerPosition: set_position method not found");
    return false;
  }

  float vec[3] = { x, y, z };
  void* args[1] = { vec };
  MonoObject* exception = nullptr;
  g_mono.mono_runtime_invoke(set_position, transform_obj, args, &exception);
  if (exception) {
    AppendLog("MonoSetLocalPlayerPosition: mono_runtime_invoke set_position threw");
    return false;
  }
  {
    std::ostringstream oss;
    oss << "MonoSetLocalPlayerPosition: set to (" << x << "," << y << "," << z << ")";
    AppendLog(oss.str());
  }
  return true;
}

bool MonoSetLocalPlayerHealth(int health, int max_health) {
  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoSetLocalPlayerHealth: failed to get local player");
    return false;
  }
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetLocalPlayerHealth: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_avatar_health_field) {
    AppendLog("MonoSetLocalPlayerHealth: health field unresolved");
    return false;
  }
  if (!g_mono.mono_field_set_value) {
    AppendLog("MonoSetLocalPlayerHealth: mono_field_set_value not available");
    return false;
  }

  MonoObject* health_obj = nullptr;
  g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
    g_player_avatar_health_field, &health_obj);
  if (!health_obj) {
    AppendLog("MonoSetLocalPlayerHealth: health_obj is null");
    return false;
  }

  if (g_player_health_value_field) {
    g_mono.mono_field_set_value(health_obj, g_player_health_value_field, &health);
  }
  else {
    AppendLog("MonoSetLocalPlayerHealth: health value field unresolved");
  }
  if (g_player_health_max_field) {
    g_mono.mono_field_set_value(health_obj, g_player_health_max_field, &max_health);
  }
  else {
    AppendLog("MonoSetLocalPlayerHealth: maxHealth field unresolved");
  }
  return true;
}

bool MonoSetLocalPlayerEnergy(float energy, float max_energy) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetLocalPlayerEnergy: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable) {
    AppendLog("MonoSetLocalPlayerEnergy: PlayerController fields not resolved");
    return false;
  }
  if (!g_mono.mono_field_set_value) {
    AppendLog("MonoSetLocalPlayerEnergy: mono_field_set_value not available");
    return false;
  }

  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoSetLocalPlayerEnergy: PlayerController::instance is null");
    return false;
  }

  if (g_player_controller_energy_current_field) {
    g_mono.mono_field_set_value(controller_instance, g_player_controller_energy_current_field,
      &energy);
  }
  else {
    AppendLog("MonoSetLocalPlayerEnergy: EnergyCurrent field unresolved");
  }
  if (g_player_controller_energy_start_field) {
    g_mono.mono_field_set_value(controller_instance, g_player_controller_energy_start_field,
      &max_energy);
  }
  else {
    AppendLog("MonoSetLocalPlayerEnergy: EnergyStart field unresolved");
  }
  return true;
}

bool MonoSetJumpExtraDirect(int jump_count) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetJumpExtraDirect: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable ||
    !g_player_controller_jump_extra_field) {
    AppendLog("MonoSetJumpExtraDirect: required PlayerController refs unresolved");
    return false;
  }

  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoSetJumpExtraDirect: PlayerController instance is null");
    return false;
  }

  g_mono.mono_field_set_value(controller_instance, g_player_controller_jump_extra_field,
    &jump_count);
  return true;
}

bool ReadRunCurrency(int& out_value);

static MonoObject* GetPunManagerInstance() {
  if (!g_pun_manager_instance_field || !g_pun_manager_vtable) {
    return nullptr;
  }
  MonoObject* pun_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_pun_manager_vtable, g_pun_manager_instance_field, &pun_instance);
  return pun_instance;
}

static int GetPhotonPlayerActorNumber(MonoObject* player_obj) {
  if (!player_obj || !g_photon_player_get_actor_number ||
      !g_mono.mono_runtime_invoke || !g_mono.mono_object_unbox) {
    return -1;
  }
  MonoObject* exc = nullptr;
  MonoObject* boxed = g_mono.mono_runtime_invoke(
    g_photon_player_get_actor_number, player_obj, nullptr, &exc);
  if (exc || !boxed) {
    return -1;
  }
  void* raw = g_mono.mono_object_unbox(boxed);
  if (!raw) {
    return -1;
  }
  return *static_cast<int*>(raw);
}

static bool InvokeStaticBoolMethod(MonoMethod* method, bool& out_value) {
  out_value = false;
  if (!method || !g_mono.mono_runtime_invoke || !g_mono.mono_object_unbox) {
    return false;
  }
  MonoObject* exc = nullptr;
  MonoObject* boxed = g_mono.mono_runtime_invoke(method, nullptr, nullptr, &exc);
  if (exc || !boxed) {
    return false;
  }
  void* raw = g_mono.mono_object_unbox(boxed);
  if (!raw) {
    return false;
  }
  out_value = (*static_cast<uint8_t*>(raw) != 0);
  return true;
}

static bool InvokeStaticFloatMethod(MonoMethod* method, float& out_value) {
  out_value = 0.0f;
  if (!method || !g_mono.mono_runtime_invoke || !g_mono.mono_object_unbox) {
    return false;
  }
  MonoObject* exc = nullptr;
  MonoObject* boxed = g_mono.mono_runtime_invoke(method, nullptr, nullptr, &exc);
  if (exc || !boxed) {
    return false;
  }
  void* raw = g_mono.mono_object_unbox(boxed);
  if (!raw) {
    return false;
  }
  out_value = *static_cast<float*>(raw);
  return true;
}

static MonoObject* GetStaticSingletonInstance(MonoVTable* vtable, MonoClassField* field) {
  if (!vtable || !field || !g_mono.mono_field_static_get_value) {
    return nullptr;
  }
  MonoObject* instance = nullptr;
  g_mono.mono_field_static_get_value(vtable, field, &instance);
  return instance;
}

static bool ComputeSessionTransitionState(bool& out_transitioning, std::string& out_reason) {
  out_transitioning = true;
  out_reason = "managed_refs_unavailable";
  if (!EnsureThreadAttached() || !CacheManagedRefs()) {
    return false;
  }

  bool in_room = true;
  if (InvokeStaticBoolMethod(g_photon_network_get_in_room, in_room)) {
    if (!in_room) {
      out_transitioning = true;
      out_reason = "PhotonNetwork.InRoom=false";
      return true;
    }
  }

  float load_progress = 0.0f;
  if (InvokeStaticFloatMethod(g_photon_network_get_level_loading_progress, load_progress)) {
    if (load_progress > 0.001f && load_progress < 0.999f) {
      out_transitioning = true;
      out_reason = "PhotonNetwork.LevelLoadingProgress";
      return true;
    }
  }

  MonoObject* run_manager =
    GetStaticSingletonInstance(g_run_manager_vtable, g_run_manager_instance_field);
  if (!run_manager) {
    out_transitioning = true;
    out_reason = "RunManager.instance=null";
    return true;
  }

  int value = 0;
  if (g_run_manager_restarting_field &&
      ReadFieldNumber(run_manager, g_run_manager_restarting_field, value) && value != 0) {
    out_transitioning = true;
    out_reason = "RunManager.restarting";
    return true;
  }
  if (g_run_manager_wait_to_change_scene_field &&
      ReadFieldNumber(run_manager, g_run_manager_wait_to_change_scene_field, value) && value != 0) {
    out_transitioning = true;
    out_reason = "RunManager.waitToChangeScene";
    return true;
  }
  if (g_run_manager_lobby_join_field &&
      ReadFieldNumber(run_manager, g_run_manager_lobby_join_field, value) && value != 0) {
    out_transitioning = true;
    out_reason = "RunManager.lobbyJoin";
    return true;
  }

  MonoObject* game_director =
    GetStaticSingletonInstance(g_game_director_vtable, g_game_director_instance_field);
  if (!game_director) {
    out_transitioning = true;
    out_reason = "GameDirector.instance=null";
    return true;
  }

  constexpr int k_game_state_load = 0;
  constexpr int k_game_state_end_wait = 5;
  if (!g_game_director_current_state_field) {
    out_transitioning = true;
    out_reason = "GameDirector.currentState unresolved";
    return true;
  }
  if (ReadFieldNumber(game_director, g_game_director_current_state_field, value)) {
    if (value == k_game_state_load || value == k_game_state_end_wait) {
      out_transitioning = true;
      out_reason = "GameDirector.currentState=Load/EndWait";
      return true;
    }
  }

  MonoObject* level_generator =
    GetStaticSingletonInstance(g_level_generator_vtable, g_level_generator_instance_field);
  if (level_generator) {
    if (g_level_generator_generated_field &&
        ReadFieldNumber(level_generator, g_level_generator_generated_field, value) && value == 0) {
      out_transitioning = true;
      out_reason = "LevelGenerator.Generated=false";
      return true;
    }
    constexpr int k_level_state_done = 14;
    if (g_level_generator_state_field &&
        ReadFieldNumber(level_generator, g_level_generator_state_field, value) &&
        value != k_level_state_done) {
      out_transitioning = true;
      out_reason = "LevelGenerator.State!=Done";
      return true;
    }
  }

  out_transitioning = false;
  out_reason = "stable";
  return true;
}

static bool GetPhotonAuthoritySnapshot(bool& out_is_master_client,
                                       bool& out_local_equals_master,
                                       int& out_local_actor,
                                       int& out_master_actor) {
  out_is_master_client = false;
  out_local_equals_master = false;
  out_local_actor = -1;
  out_master_actor = -1;
  if (!g_mono.mono_runtime_invoke ||
      !g_photon_network_get_local_player ||
      !g_photon_network_get_master_client) {
    return false;
  }

  MonoObject* exc = nullptr;
  MonoObject* local_player = g_mono.mono_runtime_invoke(
    g_photon_network_get_local_player, nullptr, nullptr, &exc);
  if (exc) {
    local_player = nullptr;
  }

  exc = nullptr;
  MonoObject* master_player = g_mono.mono_runtime_invoke(
    g_photon_network_get_master_client, nullptr, nullptr, &exc);
  if (exc) {
    master_player = nullptr;
  }

  out_local_equals_master = local_player && master_player && local_player == master_player;
  out_local_actor = GetPhotonPlayerActorNumber(local_player);
  out_master_actor = GetPhotonPlayerActorNumber(master_player);

  if (g_photon_network_get_is_master_client && g_mono.mono_object_unbox) {
    exc = nullptr;
    MonoObject* boxed = g_mono.mono_runtime_invoke(
      g_photon_network_get_is_master_client, nullptr, nullptr, &exc);
    if (!exc && boxed) {
      void* raw = g_mono.mono_object_unbox(boxed);
      if (raw) {
        out_is_master_client = (*static_cast<uint8_t*>(raw) != 0);
      }
    }
  } else {
    out_is_master_client = out_local_equals_master;
  }

  return true;
}

static void LogPhotonAuthoritySnapshot(const char* tag) {
  bool is_master_client = false;
  bool local_equals_master = false;
  int local_actor = -1;
  int master_actor = -1;
  if (!GetPhotonAuthoritySnapshot(
        is_master_client, local_equals_master, local_actor, master_actor)) {
    AppendLog(std::string(tag) + ": Photon authority snapshot unavailable");
    return;
  }

  std::ostringstream oss;
  oss << tag
      << ": Photon isMasterClient=" << (is_master_client ? "true" : "false")
      << " localActor=" << local_actor
      << " masterActor=" << master_actor
      << " local==master=" << (local_equals_master ? "true" : "false");
  AppendLog(oss.str());

  if (!local_equals_master) {
    std::ostringstream key;
    key << "PhotonAuthorityMismatch_" << local_actor << "_" << master_actor;
    AppendLogOnce(key.str(),
      std::string(tag) +
      ": local player is not the real master; remote authoritative state may ignore local writes.");
  }
}

bool SetRunCurrencyViaPunManager(int amount) {
  if (!g_pun_set_run_stat_set || !g_pun_manager_instance_field || !g_pun_manager_vtable ||
    !g_mono.mono_string_new) {
    return false;
  }
  MonoObject* pun_instance = GetPunManagerInstance();
  if (!pun_instance) {
    AppendLog("SetRunCurrencyViaPunManager: PunManager::instance is null");
    return false;
  }
  MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
  MonoObject* key = g_mono.mono_string_new(dom, "currency");
  if (!key) {
    AppendLog("SetRunCurrencyViaPunManager: mono_string_new failed");
    return false;
  }
  void* args[2] = { key, &amount };
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_pun_set_run_stat_set, pun_instance, args, &exc);
  if (exc) {
    AppendLog("SetRunCurrencyViaPunManager: SetRunStatSet threw exception");
    return false;
  }
  AppendLog("SetRunCurrencyViaPunManager: SetRunStatSet succeeded");
  return true;
}

bool MonoIsSessionTransitioning() {
  bool transitioning = true;
  std::string reason;
  ComputeSessionTransitionState(transitioning, reason);

  static bool s_last_transitioning = true;
  static std::string s_last_reason;
  if (s_last_transitioning != transitioning || s_last_reason != reason) {
    std::ostringstream oss;
    oss << "SessionTransitionGuard: " << (transitioning ? "busy" : "stable")
        << " reason=" << reason;
    AppendLog(oss.str());
    s_last_transitioning = transitioning;
    s_last_reason = reason;
  }
  return transitioning;
}

bool MonoSetSessionMaster(bool enable) {
  if (enable && g_session_master_patch_active) {
    return true;
  }
  if (!enable && !g_session_master_patch_active) {
    return true;
  }
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetSessionMaster: CacheManagedRefs failed");
    return false;
  }
  std::ostringstream request_log;
  request_log << "MonoSetSessionMaster: request=" << (enable ? "enable" : "disable")
              << " active=" << (g_session_master_patch_active ? "yes" : "no");
  AppendLog(request_log.str());
  LogPhotonAuthoritySnapshot("MonoSetSessionMaster(before)");
  if (enable) {
    if (!ApplySessionMasterPatches()) {
      AppendLog("MonoSetSessionMaster: patch application failed");
      RestoreSessionMasterPatches();
      g_session_master_patch_active = false;
      return false;
    }
    g_session_master_patch_active = true;
    AppendLog("MonoSetSessionMaster: enabled");
    LogPhotonAuthoritySnapshot("MonoSetSessionMaster(enabled)");
    return true;
  }
  bool ok = RestoreSessionMasterPatches();
  g_session_master_patch_active = false;
  if (ok) {
    AppendLog("MonoSetSessionMaster: disabled");
  } else {
    AppendLog("MonoSetSessionMaster: disable attempted but restore failed");
  }
  LogPhotonAuthoritySnapshot("MonoSetSessionMaster(disabled)");
  return ok;
}

// RoundDirector helpers ----------------------------------------------------

static MonoObject* GetRoundDirectorInstance() {
  MonoObject* inst = nullptr;
  if (g_round_director_vtable && g_round_director_instance_field) {
    g_mono.mono_field_static_get_value(
      g_round_director_vtable, g_round_director_instance_field, &inst);
  }
  if (inst) return inst;

  // Fallback: FindObjectsOfType RoundDirector
  if (g_object_find_objects_of_type && g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* rd_type = g_mono.mono_class_get_type(g_round_director_class);
    MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
    MonoObject* type_obj =
      (rd_type && g_mono.mono_type_get_object) ? g_mono.mono_type_get_object(dom, rd_type) : nullptr;
    if (type_obj) {
      void* args[1] = { type_obj };
      MonoObject* arr_obj = SafeInvoke(g_object_find_objects_of_type, nullptr, args,
        "FindObjectsOfType(RoundDirector)");
      MonoArray* arr = arr_obj ? reinterpret_cast<MonoArray*>(arr_obj) : nullptr;
      if (arr && arr->max_length > 0) {
        inst = static_cast<MonoObject*>(arr->vector[0]);
      }
    }
  }
  return inst;
}

static MonoArray* FindObjectsOfTypeArray(MonoClass* klass, const char* tag) {
  if (!klass || !g_mono.mono_class_get_type || !g_mono.mono_type_get_object) {
    return nullptr;
  }
  MonoType* type = g_mono.mono_class_get_type(klass);
  if (!type) {
    return nullptr;
  }
  MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
  MonoObject* type_obj = g_mono.mono_type_get_object(dom, type);
  if (!type_obj) {
    return nullptr;
  }

  auto try_method = [&](MonoMethod* method, void** args, const char* name) -> MonoArray* {
    if (!method) {
      return nullptr;
    }
    MonoObject* arr_obj = SafeInvoke(method, nullptr, args, name);
    if (!arr_obj) {
      return nullptr;
    }
    MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
    if (!arr || arr->max_length == 0) {
      return nullptr;
    }
    return arr;
  };

  void* args_type[1] = { type_obj };
  if (MonoArray* arr = try_method(g_find_objects_of_type_itemvolume, args_type,
        "FindObjectsOfType(Type)")) {
    return arr;
  }

  bool include_inactive = true;
  void* args_type_inactive[2] = { type_obj, &include_inactive };
  if (MonoArray* arr = try_method(g_find_objects_of_type_itemvolume_include_inactive,
        args_type_inactive, "FindObjectsOfType(Type,bool)")) {
    return arr;
  }

  if (MonoArray* arr = try_method(g_resources_find_objects_of_type_all, args_type,
        "Resources.FindObjectsOfTypeAll(Type)")) {
    return arr;
  }
  if (MonoArray* arr = try_method(g_object_find_objects_of_type, args_type,
        "Object.FindObjectsOfType(Type)")) {
    return arr;
  }
  if (MonoArray* arr = try_method(g_object_find_objects_of_type_include_inactive,
        args_type_inactive, "Object.FindObjectsOfType(Type,bool)")) {
    return arr;
  }

  if (tag) {
    AppendLogOnce(std::string("find_type_array_missing_") + tag,
      std::string("FindObjectsOfTypeArray failed for ") + tag);
  }
  return nullptr;
}

enum MonoTypeCode : int {
  kMonoTypeBoolean = 0x02,
  kMonoTypeI1 = 0x04,
  kMonoTypeU1 = 0x05,
  kMonoTypeI2 = 0x06,
  kMonoTypeU2 = 0x07,
  kMonoTypeI4 = 0x08,
  kMonoTypeU4 = 0x09,
  kMonoTypeI8 = 0x0a,
  kMonoTypeU8 = 0x0b,
  kMonoTypeR4 = 0x0c,
  kMonoTypeR8 = 0x0d,
};

static int GetFieldTypeCode(MonoClassField* field) {
  if (!field || !g_mono.mono_field_get_type || !g_mono.mono_type_get_type) {
    return -1;
  }
  MonoType* ty = g_mono.mono_field_get_type(field);
  if (!ty) return -1;
  return g_mono.mono_type_get_type(ty);
}

static bool ReadFieldNumber(MonoObject* obj, MonoClassField* field, int& out_v) {
  out_v = 0;
  if (!obj || !field || !g_mono.mono_field_get_value) return false;
  const int type_code = GetFieldTypeCode(field);
  switch (type_code) {
    case kMonoTypeBoolean: {
      bool b = false;
      g_mono.mono_field_get_value(obj, field, &b);
      out_v = b ? 1 : 0;
      return true;
    }
    case kMonoTypeI1: {
      int8_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeU1: {
      uint8_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeI2: {
      int16_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeU2: {
      uint16_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeI8: {
      int64_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeU8: {
      uint64_t v = 0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = static_cast<int>(v);
      return true;
    }
    case kMonoTypeR4: {
      float v = 0.0f;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = (v >= 0.0f) ? static_cast<int>(v + 0.5f) : static_cast<int>(v - 0.5f);
      return true;
    }
    case kMonoTypeR8: {
      double v = 0.0;
      g_mono.mono_field_get_value(obj, field, &v);
      out_v = (v >= 0.0) ? static_cast<int>(v + 0.5) : static_cast<int>(v - 0.5);
      return true;
    }
    case kMonoTypeI4:
    case kMonoTypeU4:
    default: {
      int v = 0;
      float vf = 0.0f;
      g_mono.mono_field_get_value(obj, field, &v);
      g_mono.mono_field_get_value(obj, field, &vf);
      const bool int_ok = v > -200000000 && v < 200000000;
      const bool float_ok = (vf == vf) && (vf > -200000000.0f) && (vf < 200000000.0f);
      if (type_code == -1 && float_ok && !int_ok) {
        out_v = (vf >= 0.0f) ? static_cast<int>(vf + 0.5f) : static_cast<int>(vf - 0.5f);
      }
      else {
        out_v = v;
      }
      return true;
    }
  }
}

namespace {
static bool WriteFieldNumber(MonoObject* obj, MonoClassField* field, int value) {
  if (!obj || !field || !g_mono.mono_field_set_value) return false;
  const int type_code = GetFieldTypeCode(field);
  switch (type_code) {
    case kMonoTypeBoolean: {
      bool b = (value != 0);
      g_mono.mono_field_set_value(obj, field, &b);
      return true;
    }
    case kMonoTypeI1: {
      int8_t v = static_cast<int8_t>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeU1: {
      uint8_t v = static_cast<uint8_t>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeI2: {
      int16_t v = static_cast<int16_t>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeU2: {
      uint16_t v = static_cast<uint16_t>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeI8: {
      int64_t v = static_cast<int64_t>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeU8: {
      uint64_t v = static_cast<uint64_t>(value < 0 ? 0 : value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeR4: {
      float v = static_cast<float>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeR8: {
      double v = static_cast<double>(value);
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
    case kMonoTypeI4:
    case kMonoTypeU4:
    default: {
      int v = value;
      g_mono.mono_field_set_value(obj, field, &v);
      return true;
    }
  }
}
}  // namespace

static bool TryInvokeNoArgs(MonoMethod* method, MonoObject* target) {
  if (!method || !target || !g_mono.mono_runtime_invoke) return false;
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(method, target, nullptr, &exc);
  return exc == nullptr;
}

static bool TryInvokeOneNumberArg(MonoMethod* method, MonoObject* target, int v) {
  if (!method || !target || !g_mono.mono_runtime_invoke) return false;
  MonoObject* exc = nullptr;
  void* args[1] = { &v };
  g_mono.mono_runtime_invoke(method, target, args, &exc);
  if (!exc) return true;
  float fv = static_cast<float>(v);
  exc = nullptr;
  args[0] = &fv;
  g_mono.mono_runtime_invoke(method, target, args, &exc);
  return exc == nullptr;
}

static bool TryInvokeTwoNumberArgs(MonoMethod* method, MonoObject* target, int a, int b) {
  if (!method || !target || !g_mono.mono_runtime_invoke) return false;
  MonoObject* exc = nullptr;
  void* args_i[2] = { &a, &b };
  g_mono.mono_runtime_invoke(method, target, args_i, &exc);
  if (!exc) return true;
  float fa = static_cast<float>(a);
  float fb = static_cast<float>(b);
  exc = nullptr;
  void* args_f[2] = { &fa, &fb };
  g_mono.mono_runtime_invoke(method, target, args_f, &exc);
  return exc == nullptr;
}

namespace {
static bool ComputeRunCurrencyBeforeForTarget(int target_haul, int& out_before) {
  out_before = 0;
  int run_currency = 0;
  if (!MonoGetRunCurrency(run_currency) && !ReadRunCurrency(run_currency)) {
    return false;
  }
  int baseline = run_currency - target_haul;
  // Some game builds derive current haul as (runCurrency - runCurrencyBefore).
  // Allow negative baseline so target haul can be forced even when run currency is low.
  if (baseline < -1000000000) baseline = -1000000000;
  out_before = baseline;
  return true;
}
}  // namespace

static int SyncExtractionPointsHaul(int current, int goal) {
  if (!g_extraction_point_class || !g_extraction_point_haul_current_field) {
    return 0;
  }
  MonoArray* arr = FindObjectsOfTypeArray(g_extraction_point_class, "ExtractionPoint");
  if (!arr || arr->max_length == 0) {
    return 0;
  }
  int run_currency_before = 0;
  const bool has_run_currency_before = ComputeRunCurrencyBeforeForTarget(current, run_currency_before);
  int written = 0;
  int method_hits = 0;
  size_t len = arr->max_length;
  for (size_t i = 0; i < len; ++i) {
    MonoObject* ep = static_cast<MonoObject*>(arr->vector[i]);
    if (!ep) continue;
    WriteFieldNumber(ep, g_extraction_point_haul_current_field, current);
    if (goal >= 0 && g_extraction_point_haul_goal_field) {
      WriteFieldNumber(ep, g_extraction_point_haul_goal_field, goal);
    }
    if (g_extraction_point_extraction_haul_field) {
      WriteFieldNumber(ep, g_extraction_point_extraction_haul_field, current);
    }
    if (has_run_currency_before && g_extraction_point_run_currency_before_field) {
      WriteFieldNumber(ep, g_extraction_point_run_currency_before_field, run_currency_before);
    }

    if (k_enable_experimental_haul_method_calls) {
      int invoke_goal = goal >= 0 ? goal : current;
      if (TryInvokeOneNumberArg(g_extraction_point_set_current_haul_method, ep, current)) {
        ++method_hits;
      }
      if (TryInvokeOneNumberArg(g_extraction_point_set_haul_goal_method, ep, invoke_goal)) {
        ++method_hits;
      }
      if (TryInvokeTwoNumberArgs(g_extraction_point_apply_haul_method, ep, current, invoke_goal)) {
        ++method_hits;
      }
      if (TryInvokeNoArgs(g_extraction_point_set_haul_text_method, ep)) {
        ++method_hits;
      }
      if (TryInvokeNoArgs(g_extraction_point_refresh_method, ep)) {
        ++method_hits;
      }
    }
    ++written;
  }
  if (written > 0) {
    std::ostringstream oss;
    oss << "SyncExtractionPointsHaul: wrote " << written << " EPs cur=" << current
      << " goal=" << goal << " method_hits=" << method_hits;
    if (has_run_currency_before) {
      oss << " runCurrencyBefore=" << run_currency_before;
    }
    AppendLog(oss.str());
  }
  return written;
}

static int ReadListCount(MonoObject* list_obj) {
  if (!list_obj || !g_mono.mono_object_get_class) return 0;
  MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
  if (!list_class) return 0;
  MonoMethod* get_count = g_mono.mono_class_get_method_from_name(list_class, "get_Count", 0);
  if (!get_count || !g_mono.mono_runtime_invoke || !g_mono.mono_object_unbox) return 0;
  MonoObject* exc = nullptr;
  MonoObject* count_obj = g_mono.mono_runtime_invoke(get_count, list_obj, nullptr, &exc);
  if (exc || !count_obj) return 0;
  int count = *static_cast<int*>(g_mono.mono_object_unbox(count_obj));
  return count > 0 ? count : 0;
}

static bool ReadBoxedNumberGuess(MonoObject* boxed, double& out_v, bool& out_is_float) {
  out_v = 0.0;
  out_is_float = false;
  if (!boxed || !g_mono.mono_object_unbox) return false;
  void* p = g_mono.mono_object_unbox(boxed);
  if (!p) return false;
  int vi = *static_cast<int*>(p);
  float vf = *static_cast<float*>(p);

  const bool int_ok = vi > -50000000 && vi < 50000000;
  const bool float_ok = (vf == vf) && (vf > -50000000.0f) && (vf < 50000000.0f);

  if (!int_ok && float_ok) {
    out_v = static_cast<double>(vf);
    out_is_float = true;
    return true;
  }
  if (int_ok && (!float_ok || (vf > -1e-20f && vf < 1e-20f && vi != 0))) {
    out_v = static_cast<double>(vi);
    out_is_float = false;
    return true;
  }
  if (int_ok) {
    out_v = static_cast<double>(vi);
    out_is_float = false;
    return true;
  }
  if (float_ok) {
    out_v = static_cast<double>(vf);
    out_is_float = true;
    return true;
  }
  return false;
}

static bool ReadListTotalNumber(MonoObject* list_obj, int& out_total) {
  out_total = 0;
  if (!list_obj || !g_mono.mono_object_get_class || !g_mono.mono_runtime_invoke) return false;
  MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
  if (!list_class) return false;
  MonoMethod* get_item = g_mono.mono_class_get_method_from_name(list_class, "get_Item", 1);
  if (!get_item) return false;
  int count = ReadListCount(list_obj);
  if (count <= 0) return false;

  double sum = 0.0;
  int sampled = 0;
  for (int i = 0; i < count; ++i) {
    void* args[1] = { &i };
    MonoObject* exc = nullptr;
    MonoObject* item_obj = g_mono.mono_runtime_invoke(get_item, list_obj, args, &exc);
    if (exc || !item_obj) continue;
    double v = 0.0;
    bool is_float = false;
    if (ReadBoxedNumberGuess(item_obj, v, is_float)) {
      sum += v;
      ++sampled;
    }
  }
  if (sampled <= 0) return false;
  out_total = sum >= 0.0 ? static_cast<int>(sum + 0.5) : static_cast<int>(sum - 0.5);
  return true;
}

static int WriteListTotalNumber(MonoObject* list_obj, int total, const char* tag) {
  if (!list_obj || !g_mono.mono_object_get_class || !g_mono.mono_runtime_invoke) return 0;
  MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
  if (!list_class) return 0;
  MonoMethod* get_item = g_mono.mono_class_get_method_from_name(list_class, "get_Item", 1);
  MonoMethod* set_item = g_mono.mono_class_get_method_from_name(list_class, "set_Item", 2);
  if (!set_item) return 0;
  int count = ReadListCount(list_obj);
  if (count <= 0) return 0;

  bool use_float = false;
  if (get_item) {
    int idx0 = 0;
    void* args0[1] = { &idx0 };
    MonoObject* exc0 = nullptr;
    MonoObject* item0 = g_mono.mono_runtime_invoke(get_item, list_obj, args0, &exc0);
    if (!exc0 && item0) {
      double v0 = 0.0;
      bool is_float0 = false;
      if (ReadBoxedNumberGuess(item0, v0, is_float0)) {
        use_float = is_float0;
      }
    }
  }

  int base = total / count;
  int rem = total % count;
  if (rem < 0) rem = -rem;
  int written = 0;

  for (int i = 0; i < count; ++i) {
    int vi = base + ((i < rem) ? 1 : 0);
    MonoObject* exc = nullptr;
    void* args[2] = { &i, nullptr };

    if (!use_float) {
      args[1] = &vi;
      g_mono.mono_runtime_invoke(set_item, list_obj, args, &exc);
      if (exc) {
        float vf = static_cast<float>(vi);
        exc = nullptr;
        args[1] = &vf;
        g_mono.mono_runtime_invoke(set_item, list_obj, args, &exc);
        if (!exc) {
          use_float = true;
        }
      }
    }
    else {
      float vf = static_cast<float>(vi);
      args[1] = &vf;
      g_mono.mono_runtime_invoke(set_item, list_obj, args, &exc);
      if (exc) {
        exc = nullptr;
        args[1] = &vi;
        g_mono.mono_runtime_invoke(set_item, list_obj, args, &exc);
        if (!exc) {
          use_float = false;
        }
      }
    }
    if (!exc) {
      ++written;
    }
  }

  if (written > 0) {
    std::ostringstream oss;
    oss << "WriteListTotalNumber(" << (tag ? tag : "?") << "): count=" << count
        << " total=" << total << " mode=" << (use_float ? "float" : "int");
    AppendLog(oss.str());
  }
  return written;
}

bool MonoGetRoundState(RoundState& out_state) {
  out_state = {};
  if (!CacheManagedRefs()) return false;
  if (!g_round_director_class) return false;
  MonoObject* rd = GetRoundDirectorInstance();
  if (!rd) return false;

  auto read_int = [&](MonoClassField* f, int& dst) {
    return ReadFieldNumber(rd, f, dst);
  };

  read_int(g_round_current_haul_field, out_state.current);
  read_int(g_round_current_haul_max_field, out_state.current_max);
  read_int(g_round_haul_goal_field, out_state.goal);
  if (out_state.current <= 0) {
    MonoObject* list_obj = nullptr;
    if (g_round_extraction_point_current_field) {
      g_mono.mono_field_get_value(rd, g_round_extraction_point_current_field, &list_obj);
      int sum = 0;
      if (ReadListTotalNumber(list_obj, sum) && sum > out_state.current) {
        out_state.current = sum;
      }
    }
    list_obj = nullptr;
    if (g_round_dollar_haul_list_field) {
      g_mono.mono_field_get_value(rd, g_round_dollar_haul_list_field, &list_obj);
      int sum = 0;
      if (ReadListTotalNumber(list_obj, sum) && sum > out_state.current) {
        out_state.current = sum;
      }
    }
  }
  int stage_tmp = -1;
  if (read_int(g_round_stage_field, stage_tmp)) {
    out_state.stage = stage_tmp;
  }
  else {
    // Fallback: many builds do not expose a dedicated stage field.
    // Use completed extraction count so UI can display a meaningful phase.
    int completed_tmp = -1;
    if (read_int(g_round_extraction_points_completed_field, completed_tmp) && completed_tmp >= 0) {
      out_state.stage = completed_tmp;
    }
  }
  if (out_state.current_max <= 0 && out_state.goal > 0) {
    // Collector progress usually treats goal as the effective max target.
    out_state.current_max = out_state.goal;
  }
  out_state.ok = true;
  return true;
}

bool MonoSetRoundState(int current, int goal, int current_max) {
  if (!CacheManagedRefs()) return false;
  if (!g_round_director_class) return false;
  MonoObject* rd = GetRoundDirectorInstance();
  if (!rd) return false;

  auto read_int = [&](MonoClassField* f, int& dst) {
    return ReadFieldNumber(rd, f, dst);
  };
  auto write_int = [&](MonoClassField* f, int v) {
    WriteFieldNumber(rd, f, v);
  };

  int cur = current;
  int cur_max = current_max;
  int haul_goal = goal;
  if (cur < 0) {
    read_int(g_round_current_haul_field, cur);
  }
  if (haul_goal < 0) {
    read_int(g_round_haul_goal_field, haul_goal);
  }
  if (cur_max < 0) {
    if (!read_int(g_round_current_haul_max_field, cur_max)) {
      cur_max = cur;
    }
  }
  if (cur >= 0 && haul_goal > 0 && cur > haul_goal) {
    cur = haul_goal;
  }
  if (cur >= 0 && cur_max < cur) {
    cur_max = cur;
  }

  if (cur >= 0) write_int(g_round_current_haul_field, cur);
  if (cur_max >= 0) write_int(g_round_current_haul_max_field, cur_max);
  if (haul_goal >= 0) write_int(g_round_haul_goal_field, haul_goal);

  if (cur >= 0 && g_round_total_haul_field) {
    int total = cur;
    int prev_total = 0;
    if (read_int(g_round_total_haul_field, prev_total) && prev_total > total) {
      total = prev_total;
    }
    write_int(g_round_total_haul_field, total);
  }
  if (haul_goal >= 0 && g_round_haul_goal_max_field) {
    int goal_max = haul_goal;
    int prev_goal_max = 0;
    if (read_int(g_round_haul_goal_max_field, prev_goal_max) && prev_goal_max > goal_max) {
      goal_max = prev_goal_max;
    }
    write_int(g_round_haul_goal_max_field, goal_max);
  }
  if (haul_goal >= 0 && g_round_extraction_haul_goal_field) {
    write_int(g_round_extraction_haul_goal_field, haul_goal);
  }
  if (cur >= 0 && haul_goal >= 0 && g_round_extraction_point_surplus_field) {
    int surplus = cur - haul_goal;
    if (surplus < 0) surplus = 0;
    write_int(g_round_extraction_point_surplus_field, surplus);
  }
  if (cur >= 0) {
    int list_writes = 0;
    if (g_round_extraction_point_current_field) {
      MonoObject* list_obj = nullptr;
      g_mono.mono_field_get_value(rd, g_round_extraction_point_current_field, &list_obj);
      if (!list_obj) {
        AppendLogOnce("MonoSetRoundState_list_extraction_current_null",
                      "MonoSetRoundState: extractionPointCurrent list is null");
      }
      list_writes += WriteListTotalNumber(list_obj, cur, "extractionPointCurrent");
    }
    if (g_round_dollar_haul_list_field) {
      MonoObject* list_obj = nullptr;
      g_mono.mono_field_get_value(rd, g_round_dollar_haul_list_field, &list_obj);
      if (!list_obj) {
        AppendLogOnce("MonoSetRoundState_list_dollar_null",
                      "MonoSetRoundState: dollarHaulList list is null");
      }
      list_writes += WriteListTotalNumber(list_obj, cur, "dollarHaulList");
    }
    if (list_writes > 0) {
      std::ostringstream lss;
      lss << "MonoSetRoundState: list_writes=" << list_writes;
      AppendLog(lss.str());
    }
  }
  if (cur >= 0 && haul_goal > 0 && cur >= haul_goal) {
    int extraction_points = 0;
    if (!read_int(g_round_extraction_points_field, extraction_points) || extraction_points <= 0) {
      extraction_points = 2;
    }
    if (g_round_extraction_points_completed_field) {
      write_int(g_round_extraction_points_completed_field, extraction_points);
    }
    if (g_round_all_extraction_points_completed_field) {
      write_int(g_round_all_extraction_points_completed_field, 1);
    }
  }

  if (cur >= 0 && k_enable_experimental_haul_method_calls) {
    int method_hits = 0;
    if (TryInvokeOneNumberArg(g_round_set_current_haul_method, rd, cur)) {
      ++method_hits;
    }
    if (haul_goal >= 0 && TryInvokeOneNumberArg(g_round_set_haul_goal_method, rd, haul_goal)) {
      ++method_hits;
    }
    if (TryInvokeTwoNumberArgs(g_round_apply_haul_method, rd, cur, haul_goal >= 0 ? haul_goal : cur)) {
      ++method_hits;
    }
    if (TryInvokeNoArgs(g_round_refresh_haul_method, rd)) {
      ++method_hits;
    }
    if (method_hits > 0) {
      std::ostringstream mss;
      mss << "MonoSetRoundState: round_method_hits=" << method_hits;
      AppendLog(mss.str());
    }

    int ep_writes = SyncExtractionPointsHaul(cur, haul_goal >= 0 ? haul_goal : cur);
    if (ep_writes <= 0) {
      AppendLogOnce("MonoSetRoundState_ep_sync_zero",
                    "MonoSetRoundState: SyncExtractionPointsHaul wrote 0 (collector chain not found)");
    }
  }

  int verify_cur = -1;
  int verify_goal = -1;
  read_int(g_round_current_haul_field, verify_cur);
  read_int(g_round_haul_goal_field, verify_goal);
  std::ostringstream oss;
  oss << "MonoSetRoundState: cur=" << cur << " goal=" << haul_goal
      << " readback=" << verify_cur << "/" << verify_goal;
  AppendLog(oss.str());
  return true;
}

bool MonoSetRoundStateSafe(int current, int goal, int current_max) {
#ifdef _MSC_VER
  __try {
    return MonoSetRoundState(current, goal, current_max);
  }
  __except (LogCrash("MonoSetRoundStateSafe", GetExceptionCode(), GetExceptionInformation())) {
    return false;
  }
#else
  try {
    return MonoSetRoundState(current, goal, current_max);
  }
  catch (...) {
    return false;
  }
#endif
}

bool MonoGetRoundProgress(RoundProgressState& out_state) {
  out_state = {};
  if (!CacheManagedRefs()) return false;
  if (!g_round_director_class) return false;
  MonoObject* rd = GetRoundDirectorInstance();
  if (!rd) return false;

  int v = 0;
  if (g_round_extraction_points_completed_field &&
      ReadFieldNumber(rd, g_round_extraction_points_completed_field, v)) {
    out_state.completed = v;
    out_state.ok = true;
  }
  if (g_round_extraction_points_field &&
      ReadFieldNumber(rd, g_round_extraction_points_field, v)) {
    out_state.total = v;
    out_state.ok = true;
  }
  if (g_round_stage_field && ReadFieldNumber(rd, g_round_stage_field, v)) {
    out_state.stage = v;
  }
  if (g_round_all_extraction_points_completed_field &&
      ReadFieldNumber(rd, g_round_all_extraction_points_completed_field, v)) {
    out_state.all_completed = (v != 0);
    out_state.ok = true;
  }
  return out_state.ok;
}

bool MonoSetRoundProgress(int completed, int total, int stage, int all_completed) {
  if (!CacheManagedRefs()) return false;
  if (!g_round_director_class) return false;
  MonoObject* rd = GetRoundDirectorInstance();
  if (!rd) return false;

  bool wrote = false;
  int final_total = total;
  int final_completed = completed;

  if (final_total >= 0 && g_round_extraction_points_field) {
    if (final_total < 0) final_total = 0;
    WriteFieldNumber(rd, g_round_extraction_points_field, final_total);
    wrote = true;
  }
  if (final_completed >= 0 && g_round_extraction_points_completed_field) {
    if (final_completed < 0) final_completed = 0;
    if (final_total >= 0 && final_completed > final_total) {
      final_completed = final_total;
    }
    WriteFieldNumber(rd, g_round_extraction_points_completed_field, final_completed);
    wrote = true;
  }
  int final_all = all_completed;
  if (final_all < 0 && final_completed >= 0 && final_total >= 0) {
    final_all = (final_completed >= final_total) ? 1 : 0;
  }
  if (final_all >= 0 && g_round_all_extraction_points_completed_field) {
    WriteFieldNumber(rd, g_round_all_extraction_points_completed_field, final_all != 0 ? 1 : 0);
    wrote = true;
  }
  if (stage >= 0 && g_round_stage_field) {
    WriteFieldNumber(rd, g_round_stage_field, stage);
    wrote = true;
  }

  if (wrote) {
    std::ostringstream oss;
    oss << "MonoSetRoundProgress: completed=" << final_completed
        << " total=" << final_total
        << " stage=" << stage
        << " all=" << final_all;
    AppendLog(oss.str());
  }
  return wrote;
}

bool MonoSetRoundProgressSafe(int completed, int total, int stage, int all_completed) {
#ifdef _MSC_VER
  __try {
    return MonoSetRoundProgress(completed, total, stage, all_completed);
  }
  __except (LogCrash("MonoSetRoundProgressSafe", GetExceptionCode(), GetExceptionInformation())) {
    return false;
  }
#else
  try {
    return MonoSetRoundProgress(completed, total, stage, all_completed);
  }
  catch (...) {
    return false;
  }
#endif
}

void MonoSetRoundHaulOverride(bool enabled, int current, int goal) {
  g_force_round_haul_enabled.store(enabled, std::memory_order_relaxed);
  g_force_round_haul_value.store(current, std::memory_order_relaxed);
  g_force_round_haul_goal.store(goal, std::memory_order_relaxed);

  if (g_shutting_down) return;

  // Hard fallback: bypass RoundDirector::Update reset path when lock is enabled.
  static bool last_enabled = false;
  if (enabled != last_enabled || (enabled && !g_round_update_patch_active)) {
    SetRoundUpdateBypass(enabled);
    last_enabled = enabled;
  }
}

bool ReadRunCurrency(int& out_value) {
  if (!g_semi_func_stat_get_run_currency) return false;
  MonoObject* exc = nullptr;
  MonoObject* obj = g_mono.mono_runtime_invoke(g_semi_func_stat_get_run_currency, nullptr, nullptr,
    &exc);
  if (exc || !obj) {
    AppendLog("ReadRunCurrency: StatGetRunCurrency failed");
    return false;
  }
  out_value = *static_cast<int*>(g_mono.mono_object_unbox(obj));
  return true;
}

bool RefreshCurrencyUi(const char* tag) {
  if (!g_currency_ui_fetch) {
    AppendLog(std::string(tag) + ": CurrencyUI::FetchCurrency unresolved");
    return false;
  }
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_currency_ui_fetch, nullptr, nullptr, &exc);
  if (exc) {
    AppendLog(std::string(tag) + ": FetchCurrency threw exception");
    return false;
  }
  return true;
}

bool MonoGetRunCurrency(int& out_value) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoGetRunCurrency: CacheManagedRefs failed");
    return false;
  }
  if (ReadRunCurrency(out_value)) {
    return true;
  }
  // Fallback: try direct runStats dictionary
  if (!g_stats_manager_instance_field || !g_stats_manager_run_stats_field || !g_mono.mono_string_new) {
    return false;
  }
  MonoObject* stats_instance = nullptr;
  if (g_stats_manager_vtable) {
    g_mono.mono_field_static_get_value(g_stats_manager_vtable, g_stats_manager_instance_field,
      &stats_instance);
  }
  if (!stats_instance) {
    return false;
  }
  MonoObject* run_stats = nullptr;
  g_mono.mono_field_get_value(stats_instance, g_stats_manager_run_stats_field, &run_stats);
  if (!run_stats) {
    return false;
  }
  MonoClass* dict_class = g_mono.mono_object_get_class(run_stats);
  MonoMethod* get_item =
    g_mono.mono_class_get_method_from_name(dict_class, "get_Item", 1);
  if (!get_item) return false;
  MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
  MonoObject* key = g_mono.mono_string_new(dom, "currency");
  if (!key) return false;
  void* args[1] = { key };
  MonoObject* exc = nullptr;
  MonoObject* obj = g_mono.mono_runtime_invoke(get_item, run_stats, args, &exc);
  if (exc || !obj) return false;
  out_value = *static_cast<int*>(g_mono.mono_object_unbox(obj));
  return true;
}

bool SetRunCurrencyDirectDict(int amount) {
  if (!g_stats_manager_instance_field || !g_stats_manager_run_stats_field || !g_mono.mono_string_new) {
    AppendLog("SetRunCurrencyDirectDict: missing stats manager refs");
    return false;
  }
  MonoObject* stats_instance = nullptr;
  if (g_stats_manager_vtable) {
    g_mono.mono_field_static_get_value(g_stats_manager_vtable, g_stats_manager_instance_field,
      &stats_instance);
  }
  if (!stats_instance) {
    AppendLog("SetRunCurrencyDirectDict: StatsManager::instance is null");
    return false;
  }

  MonoObject* run_stats = nullptr;
  g_mono.mono_field_get_value(stats_instance, g_stats_manager_run_stats_field, &run_stats);
  if (!run_stats) {
    AppendLog("SetRunCurrencyDirectDict: runStats dict is null");
    return false;
  }

  MonoClass* dict_class = g_mono.mono_object_get_class(run_stats);
  MonoMethod* set_item = g_dict_set_item ? g_dict_set_item
    : g_mono.mono_class_get_method_from_name(dict_class, "set_Item", 2);
  if (!set_item) {
    AppendLog("SetRunCurrencyDirectDict: set_Item unresolved");
    return false;
  }
  if (!g_dict_set_item) {
    g_dict_set_item = set_item;
  }

  MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
  MonoObject* key = g_mono.mono_string_new(dom, "currency");
  if (!key) {
    AppendLog("SetRunCurrencyDirectDict: mono_string_new failed");
    return false;
  }
  void* args[2] = { key, &amount };
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(set_item, run_stats, args, &exc);
  if (exc) {
    AppendLog("SetRunCurrencyDirectDict: set_Item threw exception");
    return false;
  }

  AppendLog("SetRunCurrencyDirectDict: succeeded");
  return true;
}

bool MonoApplyPendingCartValue() {
  if (!g_pending_cart_active || g_cart_apply_in_progress) {
    return false;
  }
  g_cart_apply_in_progress = true;
  int val = g_pending_cart_value;
  // Avoid overriding pending flag inside MonoSetCartValue
  bool applied = false;
  // Temporarily store and restore state
  bool original_pending = g_pending_cart_active;
  g_pending_cart_active = false;
  applied = MonoSetCartValue(val);
  if (!applied) {
    // Restore pending if still not applied
    g_pending_cart_active = original_pending;
  }
  if (applied) {
    g_pending_cart_active = false;
  }
  g_cart_apply_in_progress = false;
  return applied;
}

bool MonoSetRunCurrency(int amount) {
  if (g_shutting_down) return false;
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetRunCurrency: CacheManagedRefs failed");
    return false;
  }
  if (g_session_master_patch_active) {
    LogPhotonAuthoritySnapshot("MonoSetRunCurrency");
  }
  if (!g_semi_func_stat_set_run_currency) {
    AppendLog("MonoSetRunCurrency: StatSetRunCurrency method unresolved");
  }

  bool success = false;
  if (g_semi_func_stat_set_run_currency) {
    void* args[1] = { &amount };
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_semi_func_stat_set_run_currency, nullptr, args, &exc);
    if (!exc) {
      success = true;
      AppendLog("MonoSetRunCurrency: used StatSetRunCurrency");
    }
  }

  if (!success && SetRunCurrencyViaPunManager(amount)) {
    success = true;
    AppendLog("MonoSetRunCurrency: used SetRunStatSet fallback");
  }
  if (!success && SetRunCurrencyDirectDict(amount)) {
    success = true;
    AppendLog("MonoSetRunCurrency: used runStats direct dict");
  }

  RefreshCurrencyUi("MonoSetRunCurrency");

  int current_currency = 0;
  if (ReadRunCurrency(current_currency)) {
    std::ostringstream oss;
    oss << "MonoSetRunCurrency: current currency=" << current_currency;
    AppendLog(oss.str());
  }
  else {
    AppendLog("MonoSetRunCurrency: ReadRunCurrency failed");
  }
  return true;
}

bool MonoOverrideSpeed(float multiplier, float duration_seconds) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoOverrideSpeed: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable) {
    AppendLog("MonoOverrideSpeed: PlayerController fields unresolved");
    return false;
  }

  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoOverrideSpeed: PlayerController::instance is null");
    return false;
  }

  if (g_player_controller_override_speed) {
    void* args[2] = { &multiplier, &duration_seconds };
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_player_controller_override_speed, controller_instance, args, &exc);
    if (exc) {
      AppendLog("MonoOverrideSpeed: OverrideSpeed threw exception");
      return false;
    }
    return true;
  }

  // Fallback: direct field set
  return MonoSetSpeedMultiplierDirect(multiplier, duration_seconds);
}

bool MonoUpgradeExtraJump(int count) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoUpgradeExtraJump: CacheManagedRefs failed");
    return false;
  }
  if (!g_pun_upgrade_extra_jump) {
    AppendLog("MonoUpgradeExtraJump: UpgradePlayerExtraJump unresolved");
    return false;
  }
  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoUpgradeExtraJump: failed to get local player");
    return false;
  }
  if (!g_player_avatar_steamid_field) {
    AppendLog("MonoUpgradeExtraJump: steamID field unresolved");
    return false;
  }
  void* steam_str = nullptr;
  g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
    g_player_avatar_steamid_field, &steam_str);
  if (!steam_str) {
    AppendLog("MonoUpgradeExtraJump: steamID is null");
    return false;
  }

  void* args[2] = { steam_str, &count };
  MonoObject* pun_instance = GetPunManagerInstance();
  if (!pun_instance) {
    AppendLog("MonoUpgradeExtraJump: PunManager::instance is null");
    return false;
  }
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_pun_upgrade_extra_jump, pun_instance, args, &exc);
  if (exc) {
    AppendLog("MonoUpgradeExtraJump: method threw exception");
    return false;
  }
  std::ostringstream ok_log;
  ok_log << "MonoUpgradeExtraJump: applied count=" << count;
  AppendLog(ok_log.str());
  return true;
}

bool MonoOverrideJumpCooldown(float seconds) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoOverrideJumpCooldown: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable ||
    !g_player_controller_override_jump_cooldown) {
    AppendLog("MonoOverrideJumpCooldown: PlayerController fields/method unresolved");
    return false;
  }
  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoOverrideJumpCooldown: PlayerController::instance is null");
    return false;
  }
  void* args[1] = { &seconds };
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(
    g_player_controller_override_jump_cooldown, controller_instance, args, &exc);
  if (exc) {
    AppendLog("MonoOverrideJumpCooldown: OverrideJumpCooldown threw exception");
    return false;
  }
  return true;
}

bool MonoSetInvincible(float duration_seconds) {
  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoSetInvincible: failed to get local player");
    return false;
  }
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetInvincible: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_avatar_health_field || !g_player_health_invincible_set) {
    AppendLog("MonoSetInvincible: required fields/method unresolved");
    return false;
  }

  MonoObject* health_obj = nullptr;
  g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
    g_player_avatar_health_field, &health_obj);
  if (!health_obj) {
    AppendLog("MonoSetInvincible: health_obj is null");
    return false;
  }

  void* args[1] = { &duration_seconds };
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_player_health_invincible_set, health_obj, args, &exc);
  if (exc) {
    AppendLog("MonoSetInvincible: InvincibleSet threw exception");
    return false;
  }
  return true;
}

bool MonoSetGrabStrength(int grab_strength, int throw_strength) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetGrabStrength: CacheManagedRefs failed");
    return false;
  }
  if (g_session_master_patch_active) {
    LogPhotonAuthoritySnapshot("MonoSetGrabStrength");
  }
  {
    std::ostringstream req;
    req << "MonoSetGrabStrength: request grab=" << grab_strength
        << " throw=" << throw_strength;
    AppendLog(req.str());
  }
  if (!g_pun_upgrade_grab_strength && !g_pun_upgrade_throw_strength) {
    static bool logged = false;
    if (!logged) {
      AppendLog("MonoSetGrabStrength: methods unresolved");
      logged = true;
    }
    // fall through to local field patch
  }

  LocalPlayerInfo info;
  if (!MonoGetLocalPlayer(info) || !info.object) {
    AppendLog("MonoSetGrabStrength: failed to get local player");
    return false;
  }
  if (!g_player_avatar_steamid_field) {
    AppendLog("MonoSetGrabStrength: steamID field unresolved");
    return false;
  }
  void* steam_str = nullptr;
  g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
    g_player_avatar_steamid_field, &steam_str);
  if (!steam_str) {
    AppendLog("MonoSetGrabStrength: steamID is null");
    return false;
  }

  bool ok = false;
  MonoObject* pun_instance = GetPunManagerInstance();
  if ((g_pun_upgrade_grab_strength || g_pun_upgrade_throw_strength) && !pun_instance) {
    AppendLog("MonoSetGrabStrength: PunManager::instance is null");
  }
  if (g_pun_upgrade_grab_strength) {
    void* args[2] = { steam_str, &grab_strength };
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_pun_upgrade_grab_strength, pun_instance, args, &exc);
    if (exc) {
      AppendLog("MonoSetGrabStrength: UpgradePlayerGrabStrength threw exception");
    } else {
      ok = true;
    }
  }
  if (g_pun_upgrade_throw_strength) {
    void* args[2] = { steam_str, &throw_strength };
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_pun_upgrade_throw_strength, pun_instance, args, &exc);
    if (exc) {
      AppendLog("MonoSetGrabStrength: UpgradePlayerThrowStrength threw exception");
    } else {
      ok = true;
    }
  }
// Non-ASCII comment normalized.
// Non-ASCII comment normalized.
  float desired = 0.2f * static_cast<float>(grab_strength);
  bool field_ok = MonoSetGrabStrengthField(desired);

  if (!ok) {
    AppendLog("MonoSetGrabStrength: RPC failed, applied local field fallback only");
  } else {
    AppendLog("MonoSetGrabStrength: PunManager upgrade invoke succeeded");
  }
  if (!field_ok) {
    AppendLog("MonoSetGrabStrength: local field fallback failed");
  }
  return true;
}

bool MonoSetGrabStrengthField(float strength) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetGrabStrengthField: CacheManagedRefs failed");
    return false;
  }
  MonoObject* grabber = nullptr;

  // Preferred: get via PlayerAvatar::physGrabber
  LocalPlayerInfo info;
  if (MonoGetLocalPlayer(info) && info.object && g_player_avatar_physgrabber_field) {
    g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
      g_player_avatar_physgrabber_field, &grabber);
  }

  // Fallback: FindObjectsOfType<PhysGrabber>
  if (!grabber && g_phys_grabber_class && g_find_objects_of_type_itemvolume &&
    g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* type = g_mono.mono_class_get_type(g_phys_grabber_class);
    if (type) {
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), type);
      void* args[1] = { type_obj };
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
        g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
      if (!exc && arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr && arr->max_length > 0) {
          grabber = static_cast<MonoObject*>(arr->vector[0]);
        }
      }
    }
  }

  if (!grabber || !g_phys_grabber_strength_field) {
    AppendLog("MonoSetGrabStrengthField: PhysGrabber unresolved");
    return false;
  }

  g_mono.mono_field_set_value(grabber, g_phys_grabber_strength_field, &strength);
  std::ostringstream oss;
  oss << "MonoSetGrabStrengthField: grabStrength=" << strength;
  AppendLog(oss.str());
  return true;
}

bool MonoSetGrabRange(float range) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetGrabRange: CacheManagedRefs failed");
    return false;
  }
  MonoObject* grabber = nullptr;

  LocalPlayerInfo info;
  if (MonoGetLocalPlayer(info) && info.object && g_player_avatar_physgrabber_field) {
    g_mono.mono_field_get_value(static_cast<MonoObject*>(info.object),
      g_player_avatar_physgrabber_field, &grabber);
  }

  if (!grabber && g_phys_grabber_class && g_find_objects_of_type_itemvolume &&
    g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* type = g_mono.mono_class_get_type(g_phys_grabber_class);
    if (type) {
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), type);
      void* args[1] = { type_obj };
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
        g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
      if (!exc && arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr && arr->max_length > 0) {
          grabber = static_cast<MonoObject*>(arr->vector[0]);
        }
      }
    }
  }

  if (!grabber || !g_phys_grabber_range_field) {
    AppendLog("MonoSetGrabRange: PhysGrabber unresolved");
    return false;
  }

  g_mono.mono_field_set_value(grabber, g_phys_grabber_range_field, &range);
  std::ostringstream oss;
  oss << "MonoSetGrabRange: grabRange=" << range;
  AppendLog(oss.str());
  return true;
}

bool MonoSetSpeedMultiplierDirect(float multiplier, float duration_seconds) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetSpeedMultiplierDirect: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable ||
    !g_player_controller_override_speed_multiplier_field ||
    !g_player_controller_override_speed_timer_field) {
    AppendLog("MonoSetSpeedMultiplierDirect: required fields unresolved");
    return false;
  }

  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoSetSpeedMultiplierDirect: PlayerController::instance is null");
    return false;
  }

  g_mono.mono_field_set_value(controller_instance,
    g_player_controller_override_speed_multiplier_field,
    &multiplier);
  g_mono.mono_field_set_value(controller_instance,
    g_player_controller_override_speed_timer_field,
    &duration_seconds);
  return true;
}

bool MonoSetJumpForce(float force) {
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetJumpForce: CacheManagedRefs failed");
    return false;
  }
  if (!g_player_controller_instance_field || !g_player_controller_vtable ||
    !g_player_controller_jump_force_field) {
    AppendLog("MonoSetJumpForce: required fields unresolved");
    return false;
  }
  MonoObject* controller_instance = nullptr;
  g_mono.mono_field_static_get_value(
    g_player_controller_vtable, g_player_controller_instance_field, &controller_instance);
  if (!controller_instance) {
    AppendLog("MonoSetJumpForce: PlayerController::instance is null");
    return false;
  }
  g_mono.mono_field_set_value(controller_instance, g_player_controller_jump_force_field, &force);
  return true;
}

bool MonoSetCartValue(int value) {
  if (g_shutting_down) return false;
  if (!CacheManagedRefs()) {
    AppendLog("MonoSetCartValue: CacheManagedRefs failed");
    return false;
  }
  if (g_session_master_patch_active) {
    LogPhotonAuthoritySnapshot("MonoSetCartValue");
  }
  if (!g_cart_apply_in_progress) {
    g_pending_cart_value = value;
    g_pending_cart_active = true;
  }
  bool currency_ok = false;
  if (g_semi_func_stat_set_run_currency) {
    void* args_fast[1] = { &value };
    MonoObject* exc_fast = nullptr;
    g_mono.mono_runtime_invoke(g_semi_func_stat_set_run_currency, nullptr, args_fast, &exc_fast);
    if (!exc_fast) {
      currency_ok = true;
      AppendLog("MonoSetCartValue: currency via StatSetRunCurrency");
      RefreshCurrencyUi("MonoSetCartValue");
    }
  }
  if (!currency_ok && SetRunCurrencyViaPunManager(value)) {
    currency_ok = true;
    AppendLog("MonoSetCartValue: currency via SetRunStatSet");
    RefreshCurrencyUi("MonoSetCartValue");
  }
  if (!currency_ok && SetRunCurrencyDirectDict(value)) {
    currency_ok = true;
    AppendLog("MonoSetCartValue: currency via runStats");
    RefreshCurrencyUi("MonoSetCartValue");
  }

  bool haul_ok = false;
  int haul_goal = -1;
  int haul_current = value;
  int haul_current_max = value;
  RoundState rs{};
  if (MonoGetRoundState(rs) && rs.ok) {
    if (rs.goal >= 0) {
      haul_goal = rs.goal;
    }
    if (rs.current_max > haul_current_max) {
      haul_current_max = rs.current_max;
    }
  }
  if (haul_goal > 0 && haul_current > haul_goal) {
    haul_current = haul_goal;
  }
  if (haul_current < 0) {
    haul_current = 0;
  }
  int haul_ep_goal = haul_goal >= 0 ? haul_goal : haul_current;
  if (MonoSetRoundState(haul_current, haul_goal, haul_current_max)) {
    haul_ok = true;
  }
  {
    if (SyncExtractionPointsHaul(haul_current, haul_ep_goal) > 0) {
      haul_ok = true;
    }
  }
  {
    std::ostringstream oss;
    oss << "MonoSetCartValue: haul_apply cur=" << haul_current << " goal=" << haul_goal;
    AppendLog(oss.str());
  }
  if (haul_ok) {
    AppendLogOnce("MonoSetCartValue_haul_sync",
      "MonoSetCartValue: synced RoundDirector/ExtractionPoint haul values");
  }

  {
    std::ostringstream oss;
    oss << "MonoSetCartValue: begin class=" << g_phys_grab_cart_class
      << " haul_field=" << g_phys_grab_cart_haul_field;
    AppendLog(oss.str());
  }
  if (!g_phys_grab_cart_class || !g_phys_grab_cart_haul_field) {
    AppendLog("MonoSetCartValue: PhysGrabCart refs unresolved, retrying resolution");
    // Attempt resolution once more
    // (rely on CacheManagedRefs next call)
    return true;
  }
  MonoObject* cart = nullptr;

  // Try FindObjectsOfType<PhysGrabCart>
  if (g_find_objects_of_type_itemvolume && g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* cart_type = g_mono.mono_class_get_type(g_phys_grab_cart_class);
    if (!cart_type) {
      AppendLog("MonoSetCartValue: cart_type null for FindObjectsOfType(Type)");
    }
    else {
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      void* args[1] = { type_obj };
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
        g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: FindObjectsOfType(Type) threw exception");
      }
      else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: FindObjectsOfType length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      }
      else {
        AppendLog("MonoSetCartValue: FindObjectsOfType returned null array");
      }
    }
  }
  else {
    AppendLogOnce("MonoSetCartValue_findobj_missing",
      "MonoSetCartValue: FindObjectsOfType unavailable, will try ItemManager list");
  }

  // Fallback: FindObjectsOfType including inactive objects
  if (!cart && g_find_objects_of_type_itemvolume_include_inactive && g_mono.mono_class_get_type &&
    g_mono.mono_type_get_object) {
    MonoType* cart_type = g_mono.mono_class_get_type(g_phys_grab_cart_class);
    if (!cart_type) {
      AppendLog("MonoSetCartValue: cart_type null for FindObjectsOfType(Type,bool)");
    }
    else {
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      bool include_inactive = true;
      void* args[2] = { type_obj, &include_inactive };
      MonoObject* exc = nullptr;
      MonoObject* arr_obj = g_mono.mono_runtime_invoke(
        g_find_objects_of_type_itemvolume_include_inactive, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: FindObjectsOfType(Type,bool) threw exception");
      }
      else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: FindObjectsOfType(includeInactive) length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj_inactive", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      }
      else {
        AppendLog("MonoSetCartValue: FindObjectsOfType(Type,bool) returned null array");
      }
    }
  }

  // Fallback: Resources.FindObjectsOfTypeAll (includes inactive assets)
  if (!cart && g_resources_find_objects_of_type_all && g_mono.mono_class_get_type &&
    g_mono.mono_type_get_object) {
    MonoType* cart_type = g_mono.mono_class_get_type(g_phys_grab_cart_class);
    if (!cart_type) {
      AppendLog("MonoSetCartValue: cart_type null for Resources.FindObjectsOfTypeAll");
    }
    else {
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      void* args[1] = { type_obj };
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
        g_mono.mono_runtime_invoke(g_resources_find_objects_of_type_all, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll threw exception");
      }
      else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: Resources.FindObjectsOfTypeAll length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj_all", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      }
      else {
        AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll returned null array");
      }
    }
  }

  // Fallback: traverse all GameObjects and call GetComponent(PhysGrabCart)
  if (!cart) {
    auto find_cart_via_gameobjects = [&]() -> MonoObject* {
      if (!g_game_object_class || !g_game_object_get_component) {
        AppendLogOnce("MonoSetCartValue_go_missing", "MonoSetCartValue: GameObject class/GetComponent unresolved");
        return nullptr;
      }
      MonoType* go_type = g_mono.mono_class_get_type(g_game_object_class);
      if (!go_type) {
        AppendLog("MonoSetCartValue: GameObject type null");
        return nullptr;
      }
      MonoObject* go_type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), go_type);
      if (!go_type_obj) {
        AppendLog("MonoSetCartValue: GameObject type_obj null");
        return nullptr;
      }

      MonoObject* arr_obj = nullptr;
      MonoArray* arr = nullptr;

      if (g_resources_find_objects_of_type_all) {
        void* args[1] = { go_type_obj };
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(g_resources_find_objects_of_type_all, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll(GameObject) threw exception");
        }
        else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        }
        else {
          AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll(GameObject) returned null");
        }
      }

      if (!arr && g_find_objects_of_type_itemvolume_include_inactive && g_mono.mono_class_get_type &&
        g_mono.mono_type_get_object) {
        bool include_inactive = true;
        void* args[2] = { go_type_obj, &include_inactive };
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(
          g_find_objects_of_type_itemvolume_include_inactive, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject,bool) threw exception");
        }
        else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        }
        else {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject,bool) returned null");
        }
      }

      if (!arr && g_find_objects_of_type_itemvolume && g_mono.mono_class_get_type &&
        g_mono.mono_type_get_object) {
        void* args[1] = { go_type_obj };
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject) threw exception");
        }
        else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        }
        else {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject) returned null");
        }
      }

      if (!arr) {
        AppendLog("MonoSetCartValue: GameObject traversal has no source array");
        return nullptr;
      }

      int len = static_cast<int>(arr->max_length);
      {
        std::ostringstream oss;
        oss << "MonoSetCartValue: GameObject traversal length=" << len;
        AppendLogOnce("MonoSetCartValue_go_len", oss.str());
      }

      MonoType* cart_type = g_mono.mono_class_get_type(g_phys_grab_cart_class);
      if (!cart_type) {
        AppendLog("MonoSetCartValue: cart_type null for GameObject traversal");
        return nullptr;
      }
      MonoObject* cart_type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      if (!cart_type_obj) {
        AppendLog("MonoSetCartValue: cart_type_obj null for GameObject traversal");
        return nullptr;
      }

      size_t limit = arr ? arr->max_length : 0;
      for (size_t i = 0; i < limit; ++i) {
        MonoObject* go = static_cast<MonoObject*>(arr->vector[i]);
        if (!go) continue;
        void* args[1] = { cart_type_obj };
        MonoObject* exc = nullptr;
        MonoObject* comp =
          g_mono.mono_runtime_invoke(g_game_object_get_component, go, args, &exc);
        if (exc) {
          AppendLogOnce("MonoSetCartValue_go_getcomp_exc", "MonoSetCartValue: GameObject.GetComponent threw exception");
          continue;
        }
        if (comp) {
          AppendLog("MonoSetCartValue: found cart via GameObject traversal");
          return comp;
        }
      }

      AppendLog("MonoSetCartValue: GameObject traversal found none");
      return nullptr;
      };

    cart = find_cart_via_gameobjects();
  }

  // Fallback: scan ItemManager.itemVolumes for any PhysGrabCart instance
  if (!cart && g_item_manager_instance_field && g_item_manager_vtable &&
    g_item_manager_item_volumes_field) {
    MonoObject* manager = nullptr;
    g_mono.mono_field_static_get_value(
      g_item_manager_vtable, g_item_manager_instance_field, &manager);
    if (!manager) {
      AppendLog("MonoSetCartValue: ItemManager instance null");
    }

    auto read_list = [&](MonoObject* list_obj, MonoArray*& items, int& list_size) {
      items = nullptr;
      list_size = 0;
      if (!list_obj || !g_mono.mono_object_get_class) {
        return;
      }
      MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
      MonoClassField* items_field = g_mono.mono_class_get_field_from_name(list_class, "_items");
      MonoClassField* size_field = g_mono.mono_class_get_field_from_name(list_class, "_size");
      if (items_field && size_field) {
        g_mono.mono_field_get_value(list_obj, items_field, &items);
        g_mono.mono_field_get_value(list_obj, size_field, &list_size);
      }
      else {
        items = reinterpret_cast<MonoArray*>(list_obj);
        list_size = items ? static_cast<int>(items->max_length) : 0;
      }
      };

    MonoArray* items = nullptr;
    int list_size = 0;
    if (manager) {
      MonoObject* list_obj = nullptr;
      g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
      read_list(list_obj, items, list_size);

      if ((!items || list_size <= 0) && g_item_manager_get_all_items) {
        void* args[1] = {};
        bool arg_bool_false = false;
        if (g_item_manager_get_all_items_argc == 1) {
          args[0] = &arg_bool_false;
        }
        MonoObject* exc = nullptr;
        MonoObject* list_obj2 = g_mono.mono_runtime_invoke(
          g_item_manager_get_all_items, manager,
          g_item_manager_get_all_items_argc ? args : nullptr, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: ItemManager::GetAllItemVolumesInScene threw exception");
        }
        else {
          if (g_item_manager_item_volumes_field) {
            g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj2);
          }
          read_list(list_obj2, items, list_size);
        }
      }

      if (items) {
        std::ostringstream oss;
        oss << "MonoSetCartValue: ItemManager list size=" << list_size
          << " max_length=" << items->max_length;
        AppendLog(oss.str());
      }
      else {
        AppendLog("MonoSetCartValue: ItemManager items null");
      }

      size_t limit =
        items ? (items->max_length < static_cast<size_t>(list_size) ? items->max_length
          : static_cast<size_t>(list_size))
        : 0;
      for (size_t i = 0; i < limit; ++i) {
        MonoObject* candidate = static_cast<MonoObject*>(items->vector[i]);
        if (!candidate) continue;
        if (g_mono.mono_object_get_class(candidate) == g_phys_grab_cart_class) {
          cart = candidate;
          AppendLogOnce("MonoSetCartValue_item_list", "MonoSetCartValue: found cart via ItemManager list");
          break;
        }
      }
      if (!cart) {
        AppendLog("MonoSetCartValue: ItemManager list scanned, no PhysGrabCart match");
      }
    }
    else {
      AppendLogOnce("MonoSetCartValue_itemmanager_null", "MonoSetCartValue: ItemManager instance is null");
    }
  }

  // Final fallback: write ExtractionPoint haul values directly
  if (!cart && g_extraction_point_class && g_extraction_point_haul_current_field) {
    auto find_extraction_point = [&]() -> MonoObject* {
      MonoType* ep_type = g_mono.mono_class_get_type(g_extraction_point_class);
      if (!ep_type) {
        AppendLog("MonoSetCartValue: extraction_point type null");
        return nullptr;
      }
      MonoObject* ep_type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), ep_type);
      if (!ep_type_obj) {
        AppendLog("MonoSetCartValue: extraction_point type_obj null");
        return nullptr;
      }

      auto try_invoke = [&](MonoMethod* method, void** args, const char* tag) -> MonoArray* {
        if (!method) return nullptr;
        MonoObject* exc = nullptr;
        MonoObject* arr_obj = g_mono.mono_runtime_invoke(method, nullptr, args, &exc);
        if (exc) {
          AppendLog(std::string("MonoSetCartValue: ") + tag + " threw exception");
          return nullptr;
        }
        return reinterpret_cast<MonoArray*>(arr_obj);
        };

      MonoArray* arr = nullptr;
      if (g_resources_find_objects_of_type_all) {
        void* args[1] = { ep_type_obj };
        arr = try_invoke(g_resources_find_objects_of_type_all, args, "Resources.FindObjectsOfTypeAll(ExtractionPoint)");
      }
      if (!arr && g_find_objects_of_type_itemvolume_include_inactive) {
        bool include_inactive = true;
        void* args[2] = { ep_type_obj, &include_inactive };
        arr = try_invoke(g_find_objects_of_type_itemvolume_include_inactive, args,
          "FindObjectsOfType(ExtractionPoint,bool)");
      }
      if (!arr && g_find_objects_of_type_itemvolume) {
        void* args[1] = { ep_type_obj };
        arr = try_invoke(g_find_objects_of_type_itemvolume, args, "FindObjectsOfType(ExtractionPoint)");
      }

      int len = arr ? static_cast<int>(arr->max_length) : 0;
      std::ostringstream oss;
      oss << "MonoSetCartValue: ExtractionPoint search length=" << len;
      AppendLogOnce("MonoSetCartValue_ep_len", oss.str());
      if (arr && len > 0) {
        return static_cast<MonoObject*>(arr->vector[0]);
      }
      return nullptr;
      };

    MonoObject* ep = find_extraction_point();
    if (ep) {
      WriteFieldNumber(ep, g_extraction_point_haul_current_field, haul_current);
      if (g_extraction_point_haul_goal_field) {
        WriteFieldNumber(ep, g_extraction_point_haul_goal_field, haul_ep_goal);
      }
      if (g_extraction_point_extraction_haul_field) {
        WriteFieldNumber(ep, g_extraction_point_extraction_haul_field, haul_current);
      }
      if (g_extraction_point_run_currency_before_field) {
        int run_currency_before = 0;
        if (ComputeRunCurrencyBeforeForTarget(haul_current, run_currency_before)) {
          WriteFieldNumber(ep, g_extraction_point_run_currency_before_field, run_currency_before);
        }
      }
      if (k_enable_experimental_haul_method_calls) {
        int invoke_goal = haul_ep_goal >= 0 ? haul_ep_goal : haul_current;
        TryInvokeOneNumberArg(g_extraction_point_set_current_haul_method, ep, haul_current);
        TryInvokeOneNumberArg(g_extraction_point_set_haul_goal_method, ep, invoke_goal);
        TryInvokeTwoNumberArgs(g_extraction_point_apply_haul_method, ep, haul_current, invoke_goal);
        TryInvokeNoArgs(g_extraction_point_set_haul_text_method, ep);
        TryInvokeNoArgs(g_extraction_point_refresh_method, ep);
      }
      AppendLog("MonoSetCartValue: wrote ExtractionPoint haul as fallback");
      return true;
    }
    else {
      AppendLog("MonoSetCartValue: no ExtractionPoint found for fallback");
    }
  }

  if (!cart) {
    AppendLog("MonoSetCartValue: no PhysGrabCart found (all strategies), pending");
    g_pending_cart_active = true;
    return true;
  }

  WriteFieldNumber(cart, g_phys_grab_cart_haul_field, haul_current);

  if (g_phys_grab_cart_set_haul_text) {
    MonoObject* exc2 = nullptr;
    g_mono.mono_runtime_invoke(g_phys_grab_cart_set_haul_text, cart, nullptr, &exc2);
  }
  g_pending_cart_active = false;
  return true;
}

bool MonoSetCartValueSafe(int value) {
#ifdef _MSC_VER
  __try {
    return MonoSetCartValue(value);
  }
  __except (LogCrash("MonoSetCartValueSafe", GetExceptionCode(), GetExceptionInformation())) {
    return false;
  }
#else
  try {
    return MonoSetCartValue(value);
  }
  catch (...) {
    return false;
  }
#endif
}

// Non-ASCII comment normalized.
int EnumerateListObjects(MonoObject* list_obj, const std::function<bool(MonoObject*)>& on_elem) {
  if (!list_obj || !on_elem) return 0;
  MonoClass* cls = g_mono.mono_object_get_class(list_obj);
  if (!cls) return 0;
// Non-ASCII comment normalized.
  static MonoClass* cached_list_cls = nullptr;
  static MonoMethod* cached_get_enum = nullptr;
  if (cls != cached_list_cls) {
    cached_get_enum = g_mono.mono_class_get_method_from_name(cls, "GetEnumerator", 0);
    cached_list_cls = cls;
  }
  MonoMethod* get_enum = cached_get_enum;
  if (!get_enum) return 0;

  MonoObject* exc_enum = nullptr;
  MonoObject* enumerator = g_mono.mono_runtime_invoke(get_enum, list_obj, nullptr, &exc_enum);
  if (exc_enum || !enumerator) return 0;
  MonoClass* enum_class = g_mono.mono_object_get_class(enumerator);
  if (!enum_class) return 0;

  static MonoClass* cached_enum_cls = nullptr;
  static MonoMethod* cached_move_next = nullptr;
  static MonoMethod* cached_get_current = nullptr;
  if (enum_class != cached_enum_cls) {
    cached_move_next = g_mono.mono_class_get_method_from_name(enum_class, "MoveNext", 0);
    cached_get_current = g_mono.mono_class_get_method_from_name(enum_class, "get_Current", 0);
    cached_enum_cls = enum_class;
  }
  MonoMethod* move_next = cached_move_next;
  MonoMethod* get_current = cached_get_current;
  if (!move_next || !get_current) return 0;

  int count = 0;
  while (true) {
    MonoObject* exc_move = nullptr;
    MonoObject* move_obj =
      g_mono.mono_runtime_invoke(move_next, enumerator, nullptr, &exc_move);
    if (exc_move || !move_obj) break;
    bool has_next = false;
    if (g_mono.mono_object_unbox) {
      has_next = *static_cast<bool*>(g_mono.mono_object_unbox(move_obj));
    }
    if (!has_next) break;
    MonoObject* exc_cur = nullptr;
    MonoObject* cur = g_mono.mono_runtime_invoke(get_current, enumerator, nullptr, &exc_cur);
    if (exc_cur || !cur) continue;
    if (on_elem(cur)) {
      ++count;
    }
  }
  return count;
}

bool MonoListPlayers(std::vector<PlayerState>& out_states, bool include_local) {
  out_states.clear();
  if (!CacheManagedRefs()) {
    AppendLog("MonoListPlayers: CacheManagedRefs failed");
    return false;
  }

  MonoObject* list_obj = nullptr;

  // First try GameDirector.PlayerList
  if (g_game_director_instance_field && g_game_director_player_list_field) {
    MonoObject* director = nullptr;
    g_mono.mono_field_static_get_value(g_game_director_vtable, g_game_director_instance_field,
      &director);
    if (director) {
      g_mono.mono_field_get_value(director, g_game_director_player_list_field, &list_obj);
    }
  }

  // Fallback to SemiFunc::PlayerGetAll
  if (!list_obj && g_player_get_all_method) {
    MonoObject* exception = nullptr;
    list_obj = g_mono.mono_runtime_invoke(g_player_get_all_method, nullptr, nullptr, &exception);
    if (exception) {
      list_obj = nullptr;
      AppendLog("MonoListPlayers: PlayerGetAll threw an exception");
    }
  }

  if (!list_obj) {
    AppendLog("MonoListPlayers: no player list object");
    return false;
  }

  MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
  static MonoClassField* items_field = nullptr;
  static MonoClassField* size_field = nullptr;
  if (!items_field || !size_field) {
    items_field = g_mono.mono_class_get_field_from_name(list_class, "_items");
    size_field = g_mono.mono_class_get_field_from_name(list_class, "_size");
  }

  if (!items_field || !size_field) {
    AppendLog("MonoListPlayers: failed to resolve List fields");
    return false;
  }

  MonoArray* items = nullptr;
  int list_size = 0;
  g_mono.mono_field_get_value(list_obj, items_field, &items);
  g_mono.mono_field_get_value(list_obj, size_field, &list_size);
  if (!items || list_size <= 0 || items->max_length <= 0) {
    int enumerated = EnumerateListObjects(
      list_obj, [&](MonoObject* player) -> bool {
        if (!player) return false;
        bool is_local = false;
        if (g_player_avatar_is_local_field) {
          g_mono.mono_field_get_value(player, g_player_avatar_is_local_field, &is_local);
        }
        if (!include_local && is_local) {
          return false;
        }
        PlayerState state{};
        state.is_local = is_local;
        if (MonoGetPlayerStateFromAvatar(player, state) && state.has_position) {
          out_states.push_back(state);
          return true;
        }
        return false;
      });

// Non-ASCII comment normalized.
    if (out_states.empty() && g_player_avatar_instance_field && g_player_avatar_vtable) {
      MonoObject* inst = nullptr;
      g_mono.mono_field_static_get_value(g_player_avatar_vtable, g_player_avatar_instance_field,
        &inst);
      if (inst) {
        PlayerState state{};
        state.is_local = true;
        if (MonoGetPlayerStateFromAvatar(inst, state) && state.has_position) {
          out_states.push_back(state);
        }
      }
    }

    static uint64_t last_log = 0;
    uint64_t now_ms = GetTickCount64();
    if (enumerated <= 0 && now_ms - last_log > 2000) {
      AppendLog("MonoListPlayers: list empty or max_length invalid");
      last_log = now_ms;
    }
    return true;
  }

  for (int i = 0; i < list_size && i < static_cast<int>(items->max_length); ++i) {
    MonoObject* player = static_cast<MonoObject*>(items->vector[i]);
    if (!player) continue;

    bool is_local = false;
    if (g_player_avatar_is_local_field) {
      g_mono.mono_field_get_value(player, g_player_avatar_is_local_field, &is_local);
    }
    if (!include_local && is_local) {
      continue;
    }

    PlayerState state{};
    state.is_local = is_local;
    if (MonoGetPlayerStateFromAvatar(player, state) && state.has_position) {
      out_states.push_back(state);
    }
  }

  return true;
}

bool MonoListItems(std::vector<PlayerState>& out_items) {
  try {
    if (g_shutting_down) return false;
    static uint64_t last_items_empty_ms = 0;
    uint64_t now_ms = GetTickCount64();
// Non-ASCII comment normalized.
    if (g_items_disabled && now_ms - g_items_last_crash_ms.load(std::memory_order_relaxed) > 3000) {
      g_items_disabled = false;
    }
    if (!g_items_ready.load(std::memory_order_relaxed) && now_ms - last_items_empty_ms < 1500) {
      return false;  // Non-ASCII comment normalized.
    }
    out_items.clear();
    if (!CacheManagedRefs()) {
      AppendLog("MonoListItems: CacheManagedRefs failed");
      return false;
    }
    if (!g_component_get_transform) {
      AppendLog("MonoListItems: Component::get_transform unresolved");
      return false;
    }

    auto DisableFadeOnObject = [&](MonoObject* go_obj) {
      if (!g_esp_enabled) return;  // Non-ASCII comment normalized.
      if (!go_obj || !g_light_fade_class || !g_light_fade_type || !g_game_object_get_component ||
        !g_mono.mono_type_get_object || !g_mono.mono_runtime_invoke) {
        return;
      }
      MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
      MonoObject* type_obj = g_mono.mono_type_get_object(dom, g_light_fade_type);
      if (!type_obj) return;
      void* args[1] = { type_obj };
      MonoObject* exc = nullptr;
      MonoObject* comp =
        g_mono.mono_runtime_invoke(g_game_object_get_component, go_obj, args, &exc);
      if (exc || !comp) return;

      if (g_light_fade_is_fading_field) {
        bool f = false;
        g_mono.mono_field_set_value(comp, g_light_fade_is_fading_field, &f);
      }
      if (g_light_fade_current_time_field) {
        float zero = 0.0f;
        g_mono.mono_field_set_value(comp, g_light_fade_current_time_field, &zero);
      }
      if (g_light_fade_fade_duration_field) {
        float big = 1e9f;
        g_mono.mono_field_set_value(comp, g_light_fade_fade_duration_field, &big);
      }
      };

    auto read_mono_string_field = [&](MonoObject* obj, MonoClassField* field) -> std::string {
      if (!obj || !field) return {};
      MonoObject* str_obj = nullptr;
      g_mono.mono_field_get_value(obj, field, &str_obj);
      if (!str_obj) return {};
      return MonoStringToUtf8(str_obj);
    };

    auto apply_item_attributes = [&](MonoObject* attr_obj, PlayerState& st) {
      if (!attr_obj) return;
      if (g_item_attributes_value_field && !st.has_value) {
        int v = 0;
        g_mono.mono_field_get_value(attr_obj, g_item_attributes_value_field, &v);
        st.value = v;
        st.has_value = true;
      }
      if (g_item_attributes_item_type_field && !st.has_item_type) {
        int t = -1;
        g_mono.mono_field_get_value(attr_obj, g_item_attributes_item_type_field, &t);
        st.item_type = t;
        st.has_item_type = true;
      }
      std::string item_name = read_mono_string_field(attr_obj, g_item_attributes_item_name_field);
      std::string instance_name = read_mono_string_field(attr_obj, g_item_attributes_instance_name_field);
      std::string preferred;
      if (!item_name.empty()) {
        preferred = item_name;
      } else if (!instance_name.empty()) {
        preferred = instance_name;
        size_t slash = preferred.find('/');
        if (slash != std::string::npos) {
          preferred = preferred.substr(0, slash);
        }
      } else if (g_item_attributes_item_field && g_item_item_name_field) {
        MonoObject* item_obj = nullptr;
        g_mono.mono_field_get_value(attr_obj, g_item_attributes_item_field, &item_obj);
        preferred = read_mono_string_field(item_obj, g_item_item_name_field);
      }
      if (!preferred.empty()) {
        st.name = preferred;
        st.has_name = true;
      }
      st.category = PlayerState::Category::kValuable;
    };

    auto apply_valuable_object = [&](MonoObject* valuable_obj, PlayerState& st) {
      if (!valuable_obj) return;
      if (g_valuable_object_value_field && !st.has_value) {
        float v = 0.0f;
        g_mono.mono_field_get_value(valuable_obj, g_valuable_object_value_field, &v);
        st.value = static_cast<int>(std::lround(v));
        st.has_value = true;
      }
      st.category = PlayerState::Category::kValuable;
    };

    auto populate_meta = [&](MonoObject* item_obj, PlayerState& st) {
      if (!item_obj) return;
      MonoObject* exc_go = nullptr;
      MonoObject* go_obj = g_component_get_game_object
        ? g_mono.mono_runtime_invoke(g_component_get_game_object, item_obj, nullptr, &exc_go)
        : nullptr;
      if (!exc_go && go_obj && !IsUnityNull(go_obj)) {
        DisableFadeOnObject(go_obj);
        if (g_game_object_get_layer) {
          MonoObject* exc_layer = nullptr;
          MonoObject* layer_obj =
            g_mono.mono_runtime_invoke(g_game_object_get_layer, go_obj, nullptr, &exc_layer);
          if (!exc_layer && layer_obj && g_mono.mono_object_unbox) {
            st.layer = *static_cast<int*>(g_mono.mono_object_unbox(layer_obj));
            st.has_layer = true;
          }
        }
        if (g_unity_object_get_name) {
          MonoObject* exc_name = nullptr;
          MonoObject* name_obj =
            g_mono.mono_runtime_invoke(g_unity_object_get_name, go_obj, nullptr, &exc_name);
          if (!exc_name && name_obj) {
            st.name = MonoStringToUtf8(name_obj);
            st.has_name = !st.name.empty();
          }
        }
      }

      MonoClass* cls = g_mono.mono_object_get_class(item_obj);
      if (!cls) return;

      if (g_item_volume_class && IsSubclassOf(cls, g_item_volume_class) && g_item_volume_item_attributes_field) {
        MonoObject* attr_obj = nullptr;
        g_mono.mono_field_get_value(item_obj, g_item_volume_item_attributes_field, &attr_obj);
        apply_item_attributes(attr_obj, st);
      }

      if (go_obj && g_game_object_get_component && g_item_attributes_type_obj &&
        (!st.has_value || !st.has_item_type || !st.has_name)) {
        void* args_attr[1] = { g_item_attributes_type_obj };
        MonoObject* attr_obj = SafeInvoke(g_game_object_get_component, go_obj, args_attr,
          "GameObject.GetComponent(ItemAttributes)");
        if (!attr_obj && g_game_object_get_component_in_parent) {
          attr_obj = SafeInvoke(g_game_object_get_component_in_parent, go_obj, args_attr,
            "GameObject.GetComponentInParent(ItemAttributes)");
        }
        apply_item_attributes(attr_obj, st);
      }

      MonoObject* valuable_obj = nullptr;
      if (g_valuable_object_class && IsSubclassOf(cls, g_valuable_object_class)) {
        valuable_obj = item_obj;
      } else if (go_obj && g_valuable_object_type_obj && g_game_object_get_component) {
        void* args_val[1] = { g_valuable_object_type_obj };
        valuable_obj = SafeInvoke(g_game_object_get_component, go_obj, args_val,
          "GameObject.GetComponent(ValuableObject)");
        if (!valuable_obj && g_game_object_get_component_in_parent) {
          valuable_obj = SafeInvoke(g_game_object_get_component_in_parent, go_obj, args_val,
            "GameObject.GetComponentInParent(ValuableObject)");
        }
      }
      apply_valuable_object(valuable_obj, st);

      if (st.has_value) {
        st.category = PlayerState::Category::kValuable;
      } else if (g_phys_grab_object_class && IsSubclassOf(cls, g_phys_grab_object_class)) {
        st.category = PlayerState::Category::kPhysGrab;
      } else if (g_item_volume_class && IsSubclassOf(cls, g_item_volume_class)) {
        st.category = PlayerState::Category::kVolume;
      } else {
        st.category = PlayerState::Category::kUnknown;
      }
    };

    auto collect_from_overlap_sphere = [&]() -> bool {
      static bool logged_overlap_start = false;
      if (!logged_overlap_start) {
        std::ostringstream oss;
        oss << "MonoListItems: OverlapSphere strategy invoked (argc=" << g_physics_overlap_sphere_argc
          << ", transform=" << (g_component_get_transform ? "yes" : "no") << ")";
        AppendLog(oss.str());
        logged_overlap_start = true;
      }
      if (!g_physics_overlap_sphere) {
        AppendLogOnce("MonoListItems_overlap_missing_physics",
          "MonoListItems: OverlapSphere method unresolved");
        return false;
      }
      if (!g_component_get_transform) {
        AppendLogOnce("MonoListItems_overlap_missing_transform",
          "MonoListItems: Component::get_transform unresolved");
        return false;
      }

      float center[3] = { 0.0f, 0.0f, 0.0f };
      PlayerState local_state{};
      if (MonoGetLocalPlayerState(local_state) && local_state.has_position) {
        center[0] = local_state.x;
        center[1] = local_state.y;
        center[2] = local_state.z;
      }

      float radius = 500.0f;
      int query = 2;  // QueryTriggerInteraction.Collide
      int named_layer = -1;
      int layer_mask = -1;
      if (g_layer_mask_name_to_layer && g_mono.mono_string_new) {
        MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
        MonoObject* layer_name = g_mono.mono_string_new(dom, "Interactable");
        if (layer_name) {
          void* layer_args[1] = { layer_name };
          MonoObject* exc_layer = nullptr;
          MonoObject* layer_obj =
            g_mono.mono_runtime_invoke(g_layer_mask_name_to_layer, nullptr, layer_args, &exc_layer);
          if (!exc_layer && layer_obj && g_mono.mono_object_unbox) {
            named_layer = *static_cast<int*>(g_mono.mono_object_unbox(layer_obj));
          }
        }
      }
      if (named_layer < 0) {
        named_layer = 16;
      }
      layer_mask = (named_layer >= 0) ? (1 << named_layer) : -1;

      MonoObject* arr_obj = nullptr;
      void* args4[4] = { center, &radius, &layer_mask, &query };
      void* args3[3] = { center, &radius, &layer_mask };
      void* args2[2] = { center, &radius };
      switch (g_physics_overlap_sphere_argc) {
      case 4:
        arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args4, "OverlapSphere4");
        break;
      case 3:
        arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args3, "OverlapSphere3");
        break;
      case 2:
        arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args2, "OverlapSphere2");
        break;
      default:
        return false;
      }
      if (!arr_obj) {
        AppendLogOnce("MonoListItems_overlap_null",
          "MonoListItems: OverlapSphere threw or returned null");
        return false;
      }

      auto parse_array = [&](MonoObject* obj, MonoArray*& out, int& count) {
        out = obj ? reinterpret_cast<MonoArray*>(obj) : nullptr;
        count = out ? static_cast<int>(out->max_length) : 0;
        return IsValidArray(out);
      };

      MonoArray* colliders = nullptr;
      int collider_count = 0;
      bool ok = parse_array(arr_obj, colliders, collider_count);

// Non-ASCII comment normalized.
      if (!ok || collider_count <= 0) {
        int layer_mask_all = -1;
        void* args4b[4] = { center, &radius, &layer_mask_all, &query };
        void* args3b[3] = { center, &radius, &layer_mask_all };
        void* args2b[2] = { center, &radius };
        MonoObject* arr_obj_fallback = nullptr;
        switch (g_physics_overlap_sphere_argc) {
        case 4: arr_obj_fallback = SafeInvoke(g_physics_overlap_sphere, nullptr, args4b, "OverlapSphere4_all"); break;
        case 3: arr_obj_fallback = SafeInvoke(g_physics_overlap_sphere, nullptr, args3b, "OverlapSphere3_all"); break;
        case 2: arr_obj_fallback = SafeInvoke(g_physics_overlap_sphere, nullptr, args2b, "OverlapSphere2_all"); break;
        default: break;
        }
        ok = parse_array(arr_obj_fallback, colliders, collider_count);
      }

      if (!ok || collider_count <= 0) {
        AppendLogOnce("MonoListItems_overlap_empty",
          "MonoListItems: OverlapSphere returned empty array");
        return false;
      }

      static bool logged_header = false;
      if (!logged_header) {
        AppendLog("Got Collider array from Physics.OverlapSphere");
        std::ostringstream oss;
        oss << "Physics.OverlapSphere collider count=" << collider_count
          << " layerMask=" << layer_mask << " namedLayer=" << named_layer;
        AppendLog(oss.str());
        int preview = collider_count < 5 ? collider_count : 5;
        for (int i = 0; i < preview; ++i) {
          MonoObject* collider = static_cast<MonoObject*>(colliders->vector[i]);
          if (!collider || IsUnityNull(collider)) continue;
          int go_layer = -1;
          std::string go_name;
          MonoObject* game_obj = SafeInvoke(g_component_get_game_object, collider, nullptr, "Collider.preview_get_gameObject");
          if (game_obj && !IsUnityNull(game_obj) && g_game_object_get_layer) {
            MonoObject* layer_obj = SafeInvoke(g_game_object_get_layer, game_obj, nullptr, "Collider.preview_get_layer");
            if (layer_obj && g_mono.mono_object_unbox) {
              go_layer = *static_cast<int*>(g_mono.mono_object_unbox(layer_obj));
            }
          }
          if (game_obj && !IsUnityNull(game_obj) && g_unity_object_get_name) {
            MonoObject* name_obj = SafeInvoke(g_unity_object_get_name, game_obj, nullptr, "Collider.preview_get_name");
            if (name_obj) go_name = MonoStringToUtf8(name_obj);
          }
          std::ostringstream oss2;
          oss2 << "  Collider[" << i << "] layer=" << go_layer
            << " name=" << (go_name.empty() ? "<none>" : go_name);
          AppendLog(oss2.str());
        }
        logged_header = true;
      }

      int limit = collider_count;
      if (limit > 1024) {
        limit = 1024;
      }

      int valid = 0;
      for (int i = 0; i < limit; ++i) {
        MonoObject* collider = static_cast<MonoObject*>(colliders->vector[i]);
        if (!collider || IsUnityNull(collider)) continue;

        MonoObject* transform_obj = SafeInvoke(g_component_get_transform, collider, nullptr, "Collider.get_transform");
        if (!transform_obj || IsUnityNull(transform_obj)) continue;

        PlayerState st{};
        populate_meta(collider, st);
        if (TryGetPositionFromTransform(transform_obj, st, false) && st.has_position) {
          out_items.push_back(st);
          valid++;
        }
      }

      if (valid > 0) {
        static bool logged_valid = false;
        if (!logged_valid) {
          std::ostringstream oss;
          oss << "MonoListItems: Found " << valid
            << " valid items with positions (OverlapSphere)";
          AppendLog(oss.str());
          logged_valid = true;
        }
        g_items_ready.store(true, std::memory_order_relaxed);
        return true;
      }
      return false;
      };

    if (collect_from_overlap_sphere()) {
      return true;
    }

    MonoObject* manager = nullptr;
    if (g_item_manager_instance_field && g_item_manager_vtable) {
      g_mono.mono_field_static_get_value(
        g_item_manager_vtable, g_item_manager_instance_field, &manager);
    }
    else {
      AppendLog("MonoListItems: ItemManager instance field/vtable unresolved");
    }
    static bool logged_no_items = false;

// Non-ASCII comment normalized.
    auto fetch_verified_list = [&]() -> MonoObject* {
      MonoObject* list_obj = nullptr;
      if (manager && g_item_manager_item_volumes_field) {
        g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
        if (list_obj) {
          AppendLogOnce("MonoListItems_strategy1",
            "Got item list from ItemManager::itemVolumes field");
          return list_obj;
        }
      }

      if (manager && g_item_manager_get_all_items) {
        void* args1[1] = {};
        bool arg_bool_false = false;
        if (g_item_manager_get_all_items_argc == 1) {
          args1[0] = &arg_bool_false;
        }

        MonoObject* exception = nullptr;
        list_obj = g_mono.mono_runtime_invoke(
          g_item_manager_get_all_items, manager, g_item_manager_get_all_items_argc ? args1 : nullptr,
          &exception);
        if (exception) {
          AppendLog("MonoListItems: GetAllItemVolumesInScene threw exception");
        }

        if (list_obj) {
          AppendLogOnce("MonoListItems_strategy2", "Got item list from GetAllItemVolumesInScene");
          if (manager && g_item_manager_item_volumes_field) {
            g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
          }
        }
      }
      return list_obj;
      };

// Non-ASCII comment normalized.
    auto fetch_fallback_list = [&]() -> MonoObject* {
      MonoObject* list_obj = nullptr;
      MonoObject* exception = nullptr;

      if (g_semi_func_shop_populate) {
        MonoObject* result =
          g_mono.mono_runtime_invoke(g_semi_func_shop_populate, nullptr, nullptr, &exception);
        if (exception) {
          exception = nullptr;
        }
        if (result) {
          AppendLogOnce("MonoListItems_strategy3a", "Called SemiFunc::ShopPopulateItemVolumes");
        }
      }

      if (manager && g_item_manager_item_volumes_field) {
        g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
      }

      if (!list_obj && g_semi_func_truck_populate) {
        MonoObject* result =
          g_mono.mono_runtime_invoke(g_semi_func_truck_populate, nullptr, nullptr, &exception);
        if (exception) {
          exception = nullptr;
        }
        if (result) {
          AppendLogOnce("MonoListItems_strategy3b", "Called SemiFunc::TruckPopulateItemVolumes");
        }
      }

      if (!list_obj && manager && g_item_manager_item_volumes_field) {
        g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
      }

      if (!list_obj && g_find_objects_of_type_itemvolume && g_item_volume_class &&
        g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
        MonoType* iv_type = g_mono.mono_class_get_type(g_item_volume_class);
        if (iv_type) {
          MonoObject* type_obj = g_mono.mono_type_get_object(
            g_domain ? g_domain : g_mono.mono_get_root_domain(), iv_type);
          void* args[1] = { type_obj };
          list_obj = g_mono.mono_runtime_invoke(
            g_find_objects_of_type_itemvolume, nullptr, args, &exception);
          if (exception) {
            AppendLog("MonoListItems: FindObjectsOfType threw exception");
            list_obj = nullptr;
          }
          else if (list_obj) {
            AppendLogOnce("MonoListItems_strategy4",
              "Got item list from FindObjectsOfType<ItemVolume>()");
          }
        }
      }
      return list_obj;
      };

    auto read_list = [&](MonoObject* list_obj, MonoArray*& items, int& list_size) {
      items = nullptr;
      list_size = 0;
      if (!list_obj) {
        return;
      }

      MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
      MonoClassField* items_field = g_mono.mono_class_get_field_from_name(list_class, "_items");
      MonoClassField* size_field = g_mono.mono_class_get_field_from_name(list_class, "_size");

      if (items_field && size_field) {
        g_mono.mono_field_get_value(list_obj, items_field, &items);
        g_mono.mono_field_get_value(list_obj, size_field, &list_size);
      }
      else {
        items = reinterpret_cast<MonoArray*>(list_obj);
        list_size = items ? static_cast<int>(items->max_length) : 0;
      }

      // Guard against invalid arrays
      if (!IsValidArray(items)) {
        items = nullptr;
        list_size = 0;
        return;
      }

      if (list_size > static_cast<int>(items->max_length)) {
        list_size = static_cast<int>(items->max_length);
      }
    };

    MonoObject* list_obj = fetch_verified_list();
    if (!list_obj) {
      list_obj = fetch_fallback_list();
    }

    // If still empty, try to force populate once (avoid hammering every frame)
    static bool forced_populate_once = false;
    if (!list_obj && !forced_populate_once) {
      forced_populate_once = true;
      if (g_semi_func_shop_populate) {
        MonoObject* exception = nullptr;
        g_mono.mono_runtime_invoke(g_semi_func_shop_populate, nullptr, nullptr, &exception);
      }
      if (g_semi_func_truck_populate) {
        MonoObject* exception = nullptr;
        g_mono.mono_runtime_invoke(g_semi_func_truck_populate, nullptr, nullptr, &exception);
      }
      list_obj = fetch_verified_list();
    }

    if (!list_obj) {
      if (!logged_no_items) {
        AppendLog("MonoListItems: All strategies failed to get item list");
        logged_no_items = true;
      }
      last_items_empty_ms = now_ms;
      return false;
    }

    MonoArray* items = nullptr;
    int list_size = 0;
      read_list(list_obj, items, list_size);

// Non-ASCII comment normalized.
      static bool forced_get_all_once = false;
      if (list_size == 0 && manager && g_item_manager_get_all_items && !forced_get_all_once) {
      MonoObject* exc_force = nullptr;
      g_mono.mono_runtime_invoke(g_item_manager_get_all_items, manager, nullptr, &exc_force);
      if (exc_force) {
        AppendLog("MonoListItems: forced GetAllItemVolumesInScene threw exception");
      }
      else {
        AppendLogOnce("MonoListItems_force_call", "Forced GetAllItemVolumesInScene due to empty list");
      }
      forced_get_all_once = true;

      if (g_semi_func_shop_populate) {
        MonoObject* exc_pop = nullptr;
        g_mono.mono_runtime_invoke(g_semi_func_shop_populate, nullptr, nullptr, &exc_pop);
        if (!exc_pop) {
          AppendLogOnce("MonoListItems_force_shop", "Forced SemiFunc::ShopPopulateItemVolumes");
        }
      }
      if (g_semi_func_truck_populate) {
        MonoObject* exc_pop = nullptr;
        g_mono.mono_runtime_invoke(g_semi_func_truck_populate, nullptr, nullptr, &exc_pop);
        if (!exc_pop) {
          AppendLogOnce("MonoListItems_force_truck", "Forced SemiFunc::TruckPopulateItemVolumes");
        }
      }

      list_obj = nullptr;
      g_mono.mono_field_get_value(manager, g_item_manager_item_volumes_field, &list_obj);
      read_list(list_obj, items, list_size);

      static bool logged_after_force = false;
      if (items && !logged_after_force) {
        std::ostringstream oss;
        oss << "MonoListItems: after force, size=" << list_size
          << " max_length=" << items->max_length;
        AppendLog(oss.str());
        logged_after_force = true;
      }
    }

// Non-ASCII comment normalized.
    if (list_size > 0 && (!items || (items && items->max_length == 0)) &&
      g_find_objects_of_type_itemvolume && g_item_volume_class &&
      g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
      MonoType* iv_type = g_mono.mono_class_get_type(g_item_volume_class);
      if (iv_type) {
        MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), iv_type);
        if (type_obj) {
          void* f_args[1] = { type_obj };
          MonoObject* exc = nullptr;
          MonoObject* arr_obj = g_mono.mono_runtime_invoke(
            g_find_objects_of_type_itemvolume, nullptr, f_args, &exc);
          if (!exc && arr_obj) {
            items = reinterpret_cast<MonoArray*>(arr_obj);
            list_size = items ? static_cast<int>(items->max_length) : 0;
            static bool logged_rescue = false;
            if (!logged_rescue) {
              std::ostringstream oss;
              oss << "MonoListItems: rescue FindObjectsOfType size=" << list_size;
              AppendLog(oss.str());
              logged_rescue = true;
            }
          }
        }
      }
    }

// Non-ASCII comment normalized.
    if ((!items || list_size <= 0) && g_find_objects_of_type_itemvolume && g_item_volume_class &&
      g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
      MonoType* iv_type = g_mono.mono_class_get_type(g_item_volume_class);
      if (iv_type) {
        MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), iv_type);
        if (type_obj) {
          void* f_args[1] = { type_obj };
          MonoObject* exc = nullptr;
          MonoObject* arr_obj = g_mono.mono_runtime_invoke(
            g_find_objects_of_type_itemvolume, nullptr, f_args, &exc);
          if (!exc && arr_obj) {
            items = reinterpret_cast<MonoArray*>(arr_obj);
            list_size = items ? static_cast<int>(items->max_length) : 0;
            static bool logged_fallback = false;
            if (!logged_fallback) {
              std::ostringstream oss;
              oss << "MonoListItems: FindObjectsOfType fallback size=" << list_size;
              AppendLog(oss.str());
              logged_fallback = true;
            }
          }
        }
      }
    }

    // Extra fallback: spawnedItems list (ItemAttributes) for count only.
    if ((!items || list_size <= 0) && manager && g_item_manager_spawned_items_field) {
      MonoObject* spawned_list = nullptr;
      g_mono.mono_field_get_value(manager, g_item_manager_spawned_items_field, &spawned_list);
      MonoArray* spawned = nullptr;
      int spawned_size = 0;
      read_list(spawned_list, spawned, spawned_size);
      if (spawned && spawned_size > 0) {
        items = spawned;
        list_size = spawned_size;
        AppendLogOnce("MonoListItems_spawned", "MonoListItems: using spawnedItems list");
      }
    }

    // Extra fallback: ShopManager::secretItemVolumes
    if ((!items || list_size <= 0) && g_shop_manager_class && g_shop_manager_instance_field &&
      g_shop_secret_item_volumes_field) {
      MonoObject* shop_manager = nullptr;
      if (g_shop_manager_vtable) {
        g_mono.mono_field_static_get_value(g_shop_manager_vtable, g_shop_manager_instance_field,
          &shop_manager);
      }
      if (shop_manager) {
        MonoObject* dict_obj = nullptr;
        g_mono.mono_field_get_value(shop_manager, g_shop_secret_item_volumes_field, &dict_obj);
        if (dict_obj) {
          MonoClass* dict_class = g_mono.mono_object_get_class(dict_obj);
          MonoMethod* get_values = g_mono.mono_class_get_method_from_name(dict_class, "get_Values", 0);
          if (get_values) {
            MonoObject* exc_vals = nullptr;
            MonoObject* values = g_mono.mono_runtime_invoke(get_values, dict_obj, nullptr, &exc_vals);
            if (!exc_vals && values) {
              MonoClass* values_class = g_mono.mono_object_get_class(values);
              MonoMethod* get_enum =
                g_mono.mono_class_get_method_from_name(values_class, "GetEnumerator", 0);
              if (get_enum) {
                MonoObject* exc_enum = nullptr;
                MonoObject* enumerator =
                  g_mono.mono_runtime_invoke(get_enum, values, nullptr, &exc_enum);
                if (!exc_enum && enumerator) {
                  MonoClass* enum_class = g_mono.mono_object_get_class(enumerator);
                  MonoMethod* move_next =
                    g_mono.mono_class_get_method_from_name(enum_class, "MoveNext", 0);
                  MonoMethod* get_current =
                    g_mono.mono_class_get_method_from_name(enum_class, "get_Current", 0);
                  MonoClassField* kv_value_field = nullptr;
                  if (move_next && get_current) {
                    while (true) {
                      MonoObject* exc_move = nullptr;
                      MonoObject* move_obj =
                        g_mono.mono_runtime_invoke(move_next, enumerator, nullptr, &exc_move);
                      if (exc_move || !move_obj) break;
                      bool has_next = *static_cast<bool*>(g_mono.mono_object_unbox(move_obj));
                      if (!has_next) break;
                      MonoObject* exc_cur = nullptr;
                      MonoObject* kv = g_mono.mono_runtime_invoke(get_current, enumerator, nullptr,
                        &exc_cur);
                      if (exc_cur || !kv) continue;
                      if (!kv_value_field) {
                        MonoClass* kv_class = g_mono.mono_object_get_class(kv);
                        kv_value_field = g_mono.mono_class_get_field_from_name(kv_class, "value");
                      }
                      if (!kv_value_field) continue;
                      MonoObject* list_obj2 = nullptr;
                      g_mono.mono_field_get_value(kv, kv_value_field, &list_obj2);
                      MonoArray* arr2 = nullptr;
                      int size2 = 0;
                      read_list(list_obj2, arr2, size2);
                      if (arr2 && size2 > 0) {
                        items = arr2;
                        list_size = size2;
                        AppendLogOnce("MonoListItems_shopmgr",
                          "MonoListItems: using ShopManager::secretItemVolumes");
                        break;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

// Non-ASCII comment normalized.
    if (!items || list_size <= 0 || (items && items->max_length <= 0)) {
      if (!logged_no_items) {
        std::ostringstream oss;
        oss << "MonoListItems: list_size=" << list_size
          << " max_length=" << (items ? items->max_length : 0) << " (empty; skipping scan)";
        AppendLog(oss.str());
        logged_no_items = true;
      }
      last_items_empty_ms = now_ms;
      return false;
    }

    static bool logged_list_count = false;
    if (!logged_list_count) {
      std::ostringstream oss;
      oss << "MonoListItems: Successfully got item list, size=" << list_size
        << " max_length=" << (items ? items->max_length : 0);
      AppendLog(oss.str());
      logged_list_count = true;
    }
    g_items_ready.store(true, std::memory_order_relaxed);

    int limit = list_size;
    int max_len = (items && items->max_length > 0) ? static_cast<int>(items->max_length) : list_size;
// Non-ASCII comment normalized.
    if (max_len > 0 && limit > max_len) limit = max_len;
    if (limit > 1024) {
      limit = 1024;
    }

    int valid_count = 0;
    for (int i = 0; i < limit; ++i) {
      MonoObject* item = static_cast<MonoObject*>(items->vector[i]);
      if (!item || IsUnityNull(item)) continue;

      MonoObject* exception2 = nullptr;
      MonoObject* transform_obj =
        g_mono.mono_runtime_invoke(g_component_get_transform, item, nullptr, &exception2);
      if (exception2 || !transform_obj || IsUnityNull(transform_obj)) {
        continue;
      }

      PlayerState st{};
      populate_meta(item, st);
      if (TryGetPositionFromTransform(transform_obj, st, false) && st.has_position) {
        out_items.push_back(st);
        valid_count++;
      }
    }

    // Extra sweep: use authoritative scans for PhysGrabObject and ValuableObject.
    auto sweep_type_positions = [&](MonoClass* target_cls, const char* tag) -> int {
      if (!target_cls || !g_mono.mono_class_get_type || !g_mono.mono_type_get_object) return 0;
      MonoType* t = g_mono.mono_class_get_type(target_cls);
      if (!t) return 0;
      MonoObject* type_obj =
        g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), t);
      if (!type_obj) return 0;

      MonoArray* arr = nullptr;
      // try Object.FindObjectsOfType(Type,bool)
      if (g_find_objects_of_type_itemvolume_include_inactive) {
        bool include_inactive = true;
        void* args2[2] = { type_obj, &include_inactive };
        MonoObject* exc = nullptr;
        MonoObject* arr_obj = g_mono.mono_runtime_invoke(
          g_find_objects_of_type_itemvolume_include_inactive, nullptr, args2, &exc);
        if (!exc && arr_obj) arr = reinterpret_cast<MonoArray*>(arr_obj);
      }
      // try Object.FindObjectsOfType(Type)
      if (!arr && g_find_objects_of_type_itemvolume) {
        void* args1[1] = { type_obj };
        MonoObject* exc = nullptr;
        MonoObject* arr_obj =
          g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args1, &exc);
        if (!exc && arr_obj) arr = reinterpret_cast<MonoArray*>(arr_obj);
      }
      // try Resources.FindObjectsOfTypeAll(Type)
      if (!arr && g_resources_find_objects_of_type_all) {
        void* args1b[1] = { type_obj };
        MonoObject* exc = nullptr;
        MonoObject* arr_obj = g_mono.mono_runtime_invoke(
          g_resources_find_objects_of_type_all, nullptr, args1b, &exc);
        if (!exc && arr_obj) arr = reinterpret_cast<MonoArray*>(arr_obj);
      }

      int added = 0;
      if (IsValidArray(arr)) {
        int cnt = static_cast<int>(arr->max_length);
        std::ostringstream oss;
        oss << "MonoListItems: sweep " << tag << " count=" << cnt;
        AppendLogOnce(std::string("MonoListItems_sweep_") + tag, oss.str());
        int lim = cnt > 1024 ? 1024 : cnt;
        for (int i = 0; i < lim; ++i) {
          MonoObject* obj = static_cast<MonoObject*>(arr->vector[i]);
          if (!obj || IsUnityNull(obj)) continue;
          MonoObject* exc_t = nullptr;
          MonoObject* tr = g_mono.mono_runtime_invoke(g_component_get_transform, obj, nullptr, &exc_t);
          if (exc_t || !tr || IsUnityNull(tr)) continue;
          PlayerState st{};
          populate_meta(obj, st);
          if (TryGetPositionFromTransform(tr, st, false) && st.has_position) {
            out_items.push_back(st);
            ++added;
          }
        }
      }
      return added;
      };

    if (valid_count == 0) {
      valid_count += sweep_type_positions(g_phys_grab_object_class, "PhysGrabObject");
    }
    if (valid_count == 0) {
      valid_count += sweep_type_positions(g_valuable_object_class, "ValuableObject");
    }

    if (valid_count > 0 && !logged_list_count) {
      std::ostringstream oss;
      oss << "MonoListItems: Found " << valid_count << " valid items with positions";
      AppendLog(oss.str());
    }

    return true;
  }
  catch (...) {
    AppendLog("MonoListItems: exception caught");
    return false;
  }
}

// Thunk with SEH kept minimal to satisfy MSVC "object unwinding" restriction.
// Avoid local objects with destructors inside the __try block.
#ifdef _MSC_VER
__declspec(noinline) static bool __stdcall MonoListItemsSehThunk(std::vector<PlayerState>* out_items) {
  __try {
    return MonoListItems(*out_items);
  }
  __except (LogCrash("MonoListItemsSafe", GetExceptionCode(), GetExceptionInformation())) {
    return false;
  }
}
#endif

bool MonoListItemsSafe(std::vector<PlayerState>& out_items) {
#ifdef _MSC_VER
  bool ok = MonoListItemsSehThunk(&out_items);
  if (!ok) {
    g_items_disabled = true;
    g_items_last_crash_ms.store(GetTickCount64(), std::memory_order_relaxed);
    AppendLogOnce("MonoListItems_disabled", "MonoListItemsSafe crashed; auto item refresh disabled");
  }
  return ok;
#else
  try {
    return MonoListItems(out_items);
  }
  catch (...) {
    g_items_disabled = true;
    g_items_last_crash_ms.store(GetTickCount64(), std::memory_order_relaxed);
    AppendLogOnce("MonoListItems_disabled", "MonoListItemsSafe crashed; auto item refresh disabled");
    return false;
  }
#endif
}

bool MonoItemsDisabled() { return g_items_disabled; }

void MonoResetItemsDisabled() {
  g_items_disabled = false;
  g_items_ready.store(false, std::memory_order_relaxed);
}

bool MonoManualRefreshItems(std::vector<PlayerState>& out_items) {
  MonoResetItemsDisabled();
  return MonoListItemsSafe(out_items);
}

void MonoResetEnemiesDisabled() {
  g_enemy_esp_disabled = false;
  g_enemy_cache_disabled = false;
  g_enemies_ready.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
    ClearEnemyCacheHandlesUnlocked();
  }
}

bool MonoListEnemies(std::vector<PlayerState>& out_enemies) {
  try {
    if (g_shutting_down) return false;
    out_enemies.clear();
    if (!CacheManagedRefs()) {
      AppendLog("MonoListEnemies: CacheManagedRefs failed");
      return false;
    }
    RoundState rs{};
    if (!MonoGetRoundState(rs) || !rs.ok) {
      AppendLogOnce("MonoListEnemies_skip_noround", "MonoListEnemies: RoundDirector not ready; skip scan");
      return false;
    }
    if (!g_enemy_rigidbody_class) {
      AppendLogOnce("MonoListEnemies_no_class", "MonoListEnemies: EnemyRigidbody class unresolved");
      return false;
    }
    if (!g_component_get_transform || !g_component_get_game_object || !g_game_object_get_component) {
      AppendLogOnce("MonoListEnemies_no_method", "MonoListEnemies: transform/gameObject/GetComponent unresolved");
      return false;
    }

    uint64_t now = GetTickCount64();
    EnemyCachePruneDead();

    std::vector<uint32_t> cache_snapshot;
    {
      std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
      cache_snapshot = g_enemy_cache;
    }

    int cached_added = 0;
    for (uint32_t h : cache_snapshot) {
      MonoObject* enemy_obj = g_mono.mono_gchandle_get_target ? g_mono.mono_gchandle_get_target(h) : nullptr;
      if (enemy_obj && AddEnemyFromObject(enemy_obj, out_enemies)) {
        ++cached_added;
        if (cached_added >= 512) break;
      }
    }
    if (cached_added > 0) {
      AppendLogOnce("MonoListEnemies_cache", "MonoListEnemies: populated from cached EnemyRigidbody list");
      return true;
    }

    if (!g_enemy_cache_disabled) {
      if (RefreshEnemyCacheSafe()) {
        EnemyCachePruneDead();
        std::lock_guard<std::mutex> lock(g_enemy_cache_mutex);
        cache_snapshot = g_enemy_cache;
      }
    }
    else {
      AppendLogOnce("MonoListEnemies_cache_disabled", "MonoListEnemies: enemy cache disabled or refresh failed");
    }

    for (uint32_t h : cache_snapshot) {
      MonoObject* enemy_obj = g_mono.mono_gchandle_get_target ? g_mono.mono_gchandle_get_target(h) : nullptr;
      if (enemy_obj && AddEnemyFromObject(enemy_obj, out_enemies)) {
        ++cached_added;
        if (cached_added >= 512) break;
      }
    }
    if (cached_added > 0) {
      AppendLogOnce("MonoListEnemies_cache_refill", "MonoListEnemies: cache refilled via FindObjectsOfType");
      return true;
    }

    static uint64_t s_last_direct_enemy_scan_ms = 0;
    if (now - s_last_direct_enemy_scan_ms >= 250) {
      s_last_direct_enemy_scan_ms = now;
      if (ScanEnemiesDirect(out_enemies, 512)) {
        AppendLogOnce("MonoListEnemies_direct_scan", "MonoListEnemies: populated via direct FindObjectsOfType scan");
        return true;
      }
    }

    AppendLogOnce("MonoListEnemies_skip_overlap", "MonoListEnemies: cache empty, skip global scan");
    return true;
  }
  catch (...) {
    AppendLog("MonoListEnemies: exception caught");
    g_enemy_esp_disabled = true;
    return false;
  }
}

bool MonoListEnemiesSafe(std::vector<PlayerState>& out_enemies) {
#ifdef _MSC_VER
  __try {
    return MonoListEnemies(out_enemies);
  }
  __except (LogCrash("MonoListEnemiesSafe", GetExceptionCode(), GetExceptionInformation())) {
    g_enemy_esp_disabled = true;
    g_enemy_cache_disabled = true;
    return false;
  }
#else
  try {
    return MonoListEnemies(out_enemies);
  }
  catch (...) {
    g_enemy_esp_disabled = true;
    return false;
  }
#endif
}

static bool ReadCodeBytesSafe(const void* addr, uint8_t* out, size_t size) {
  if (!addr || !out || size == 0) return false;
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
  const DWORD p = mbi.Protect & 0xff;
  const bool executable =
    p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
    p == PAGE_EXECUTE_WRITECOPY;
  if (!executable) return false;
#endif
#ifdef _MSC_VER
  __try {
    std::memcpy(out, addr, size);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  std::memcpy(out, addr, size);
  return true;
#endif
}

static std::string HexBytes(const uint8_t* data, size_t n) {
  std::ostringstream oss;
  for (size_t i = 0; i < n; ++i) {
    if (i) oss << " ";
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
  }
  return oss.str();
}

static bool IsNumericOrBoolTypeCode(int tc) {
  switch (tc) {
    case kMonoTypeBoolean:
    case kMonoTypeI1:
    case kMonoTypeU1:
    case kMonoTypeI2:
    case kMonoTypeU2:
    case kMonoTypeI4:
    case kMonoTypeU4:
    case kMonoTypeI8:
    case kMonoTypeU8:
    case kMonoTypeR4:
    case kMonoTypeR8:
      return true;
    default:
      return false;
  }
}

static bool IsIntLikeTypeCode(int tc) {
  switch (tc) {
    case kMonoTypeBoolean:
    case kMonoTypeI1:
    case kMonoTypeU1:
    case kMonoTypeI2:
    case kMonoTypeU2:
    case kMonoTypeI4:
    case kMonoTypeU4:
      return true;
    default:
      return false;
  }
}

static bool IsCollectorKeywordHit(const std::string& name) {
  return CaseContains(name, "haul") || CaseContains(name, "extract") || CaseContains(name, "currency") ||
         CaseContains(name, "dollar") || CaseContains(name, "collector") || CaseContains(name, "tax");
}

static int GetMethodReturnTypeCode(MonoMethod* m) {
  if (!m || !g_mono.mono_method_signature || !g_mono.mono_signature_get_return_type ||
      !g_mono.mono_type_get_type) {
    return -1;
  }
  MonoMethodSignature* sig = g_mono.mono_method_signature(m);
  MonoType* ret_ty = sig ? g_mono.mono_signature_get_return_type(sig) : nullptr;
  return ret_ty ? g_mono.mono_type_get_type(ret_ty) : -1;
}

static int GetMethodParamCount(MonoMethod* m) {
  if (!m || !g_mono.mono_method_signature || !g_mono.mono_signature_get_param_count) {
    return -1;
  }
  MonoMethodSignature* sig = g_mono.mono_method_signature(m);
  return sig ? static_cast<int>(g_mono.mono_signature_get_param_count(sig)) : -1;
}

static bool ReadBoxedNumberByType(MonoObject* boxed, int type_code, double& out_v) {
  out_v = 0.0;
  if (!boxed || !g_mono.mono_object_unbox) return false;
  void* p = g_mono.mono_object_unbox(boxed);
  if (!p) return false;
  switch (type_code) {
    case kMonoTypeBoolean: out_v = *static_cast<bool*>(p) ? 1.0 : 0.0; return true;
    case kMonoTypeI1: out_v = static_cast<double>(*static_cast<int8_t*>(p)); return true;
    case kMonoTypeU1: out_v = static_cast<double>(*static_cast<uint8_t*>(p)); return true;
    case kMonoTypeI2: out_v = static_cast<double>(*static_cast<int16_t*>(p)); return true;
    case kMonoTypeU2: out_v = static_cast<double>(*static_cast<uint16_t*>(p)); return true;
    case kMonoTypeI4: out_v = static_cast<double>(*static_cast<int32_t*>(p)); return true;
    case kMonoTypeU4: out_v = static_cast<double>(*static_cast<uint32_t*>(p)); return true;
    case kMonoTypeI8: out_v = static_cast<double>(*static_cast<int64_t*>(p)); return true;
    case kMonoTypeU8: out_v = static_cast<double>(*static_cast<uint64_t*>(p)); return true;
    case kMonoTypeR4: out_v = static_cast<double>(*static_cast<float*>(p)); return true;
    case kMonoTypeR8: out_v = *static_cast<double*>(p); return true;
    default: {
      bool is_float = false;
      return ReadBoxedNumberGuess(boxed, out_v, is_float);
    }
  }
}

static bool InvokeNoArgMethodNumber(MonoMethod* m, MonoObject* instance_obj, int return_type_code,
                                    int& out_value, bool& out_used_static) {
  out_value = 0;
  out_used_static = false;
  if (!m || !g_mono.mono_runtime_invoke) return false;
  auto try_invoke = [&](MonoObject* obj, bool used_static, int& dst) -> bool {
    MonoObject* exc = nullptr;
    MonoObject* ret = g_mono.mono_runtime_invoke(m, obj, nullptr, &exc);
    if (exc || !ret) return false;
    double v = 0.0;
    if (!ReadBoxedNumberByType(ret, return_type_code, v)) return false;
    dst = v >= 0.0 ? static_cast<int>(v + 0.5) : static_cast<int>(v - 0.5);
    out_used_static = used_static;
    return true;
  };
  if (instance_obj && try_invoke(instance_obj, false, out_value)) {
    return true;
  }
  return try_invoke(nullptr, true, out_value);
}

static const char* MonoTypeCodeName(int tc) {
  switch (tc) {
    case kMonoTypeBoolean: return "bool";
    case kMonoTypeI1: return "i1";
    case kMonoTypeU1: return "u1";
    case kMonoTypeI2: return "i2";
    case kMonoTypeU2: return "u2";
    case kMonoTypeI4: return "i4";
    case kMonoTypeU4: return "u4";
    case kMonoTypeI8: return "i8";
    case kMonoTypeU8: return "u8";
    case kMonoTypeR4: return "r4";
    case kMonoTypeR8: return "r8";
    default: return "unk";
  }
}

namespace {
static bool PatchCodeToReturnInt(void* addr, int forced, CodePatchBackup& backup) {
  if (!addr) return false;
  uint8_t patch[8]{};
  patch[0] = 0xB8;  // mov eax, imm32
  std::memcpy(&patch[1], &forced, sizeof(int));
  patch[5] = 0xC3;  // ret
  patch[6] = 0x90;
  patch[7] = 0x90;

  DWORD old_protect = 0;
  if (!VirtualProtect(addr, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }
#ifdef _MSC_VER
  __try {
    std::memcpy(backup.bytes.data(), addr, sizeof(patch));
    backup.size = sizeof(patch);
    std::memcpy(addr, patch, sizeof(patch));
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD tmp = 0;
    VirtualProtect(addr, sizeof(patch), old_protect, &tmp);
    return false;
  }
#else
  std::memcpy(backup.bytes.data(), addr, sizeof(patch));
  backup.size = sizeof(patch);
  std::memcpy(addr, patch, sizeof(patch));
#endif
  FlushInstructionCache(GetCurrentProcess(), addr, sizeof(patch));
  DWORD tmp = 0;
  VirtualProtect(addr, sizeof(patch), old_protect, &tmp);
  return true;
}

static bool RestorePatchedCode(void* addr, const CodePatchBackup& backup) {
  if (!addr || backup.size == 0 || backup.size > backup.bytes.size()) return false;
  DWORD old_protect = 0;
  if (!VirtualProtect(addr, backup.size, PAGE_EXECUTE_READWRITE, &old_protect)) {
    return false;
  }
#ifdef _MSC_VER
  __try {
    std::memcpy(addr, backup.bytes.data(), backup.size);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD tmp = 0;
    VirtualProtect(addr, backup.size, old_protect, &tmp);
    return false;
  }
#else
  std::memcpy(addr, backup.bytes.data(), backup.size);
#endif
  FlushInstructionCache(GetCurrentProcess(), addr, backup.size);
  DWORD tmp = 0;
  VirtualProtect(addr, backup.size, old_protect, &tmp);
  return true;
}

struct SessionMasterPatchTarget {
  MonoMethod** method_ptr;
  int forced_value;
  const char* tag;
};
static const SessionMasterPatchTarget k_session_master_patch_targets[] = {
  { &g_semi_func_is_not_master_client, 0, "SemiFunc::IsNotMasterClient" },
  { &g_semi_func_is_master_client, 1, "SemiFunc::IsMasterClient" },
  { &g_semi_func_is_master_client_or_singleplayer, 1, "SemiFunc::IsMasterClientOrSingleplayer" },
  { &g_photon_network_get_is_master_client, 1, "PhotonNetwork::get_IsMasterClient" },
  { &g_semi_func_master_only_rpc, 1, "SemiFunc::MasterOnlyRPC" },
  { &g_semi_func_owner_only_rpc, 1, "SemiFunc::OwnerOnlyRPC" },
  { &g_semi_func_master_and_owner_only_rpc, 1, "SemiFunc::MasterAndOwnerOnlyRPC" },
};

static bool ApplySessionMasterPatches() {
  if (!g_mono.mono_compile_method) {
    AppendLog("SessionMasterPatch: mono_compile_method unresolved");
    return false;
  }
  bool patched = false;
  for (const auto& target : k_session_master_patch_targets) {
    if (!target.method_ptr || !*target.method_ptr) {
      continue;
    }
    void* code = g_mono.mono_compile_method(*target.method_ptr);
    if (!code || g_session_master_backups.find(code) != g_session_master_backups.end()) {
      continue;
    }
    CodePatchBackup backup{};
    if (!PatchCodeToReturnInt(code, target.forced_value, backup)) {
      continue;
    }
    g_session_master_backups.emplace(code, backup);
    patched = true;
    std::ostringstream oss;
    oss << "SessionMasterPatch: " << target.tag << " => " << target.forced_value;
    AppendLog(oss.str());
  }
  return patched;
}

static bool RestoreSessionMasterPatches() {
  bool any_restored = false;
  for (auto it = g_session_master_backups.begin(); it != g_session_master_backups.end();) {
    if (RestorePatchedCode(it->first, it->second)) {
      any_restored = true;
      it = g_session_master_backups.erase(it);
    } else {
      ++it;
    }
  }
  return g_session_master_backups.empty() || any_restored;
}

static bool SetRoundUpdateBypass(bool enable) {
  if (!g_mono.mono_class_get_method_from_name || !g_mono.mono_compile_method) {
    return false;
  }
  if (!EnsureThreadAttached() || !CacheManagedRefs()) {
    return false;
  }
  if (!g_round_director_class) {
    return false;
  }
  if (!g_round_director_update_method) {
    g_round_director_update_method =
      g_mono.mono_class_get_method_from_name(g_round_director_class, "Update", 0);
    if (!g_round_director_update_method) {
      AppendLogOnce("RoundUpdate_patch_no_method", "RoundDirector::Update not found for byte patch");
      return false;
    }
  }

  void* addr = g_mono.mono_compile_method(g_round_director_update_method);
  if (!addr) {
    AppendLogOnce("RoundUpdate_patch_no_code", "mono_compile_method failed for RoundDirector::Update patch");
    return false;
  }

  if (enable) {
    if (g_round_update_patch_active && g_round_update_patch_addr == addr) {
      return true;
    }
    if (g_round_update_patch_active && g_round_update_patch_addr &&
        g_round_update_patch_backup.size > 0) {
      RestorePatchedCode(g_round_update_patch_addr, g_round_update_patch_backup);
      g_round_update_patch_active = false;
      g_round_update_patch_addr = nullptr;
      g_round_update_patch_backup = {};
    }

    CodePatchBackup backup{};
    if (!PatchCodeToReturnInt(addr, 0, backup)) {
      AppendLogOnce("RoundUpdate_patch_apply_fail", "Failed to patch RoundDirector::Update");
      return false;
    }
    g_round_update_patch_addr = addr;
    g_round_update_patch_backup = backup;
    g_round_update_patch_active = true;
    AppendLogOnce("RoundUpdate_patch_enabled", "RoundDirector::Update patched to immediate return");
    return true;
  }

  if (!g_round_update_patch_active) {
    return true;
  }

  bool ok = true;
  if (g_round_update_patch_addr && g_round_update_patch_backup.size > 0) {
    ok = RestorePatchedCode(g_round_update_patch_addr, g_round_update_patch_backup);
  }
  g_round_update_patch_active = false;
  g_round_update_patch_addr = nullptr;
  g_round_update_patch_backup = {};
  AppendLogOnce("RoundUpdate_patch_disabled", "RoundDirector::Update patch restored");
  return ok;
}
}  // namespace

bool MonoScanMethods(const char* keyword, std::vector<std::string>& out_results) {
  out_results.clear();
  if (!keyword || !*keyword) return false;
  if (!EnsureThreadAttached() || !CacheManagedRefs()) return false;
  if (!g_mono.mono_class_get_methods || !g_mono.mono_method_get_name) return false;

  struct TargetCls {
    MonoClass* cls;
    const char* name;
  };
  TargetCls targets[] = {
    { g_player_health_class, "PlayerHealth" },
    { g_player_avatar_class, "PlayerAvatar" },
    { g_player_controller_class, "PlayerController" },
    { g_game_director_class, "GameDirector" },
    { g_round_director_class, "RoundDirector" },
    { g_extraction_point_class, "ExtractionPoint" },
    { g_phys_grab_cart_class, "PhysGrabCart" },
    { g_pun_manager_class, "PunManager" },
    { g_phys_grabber_class, "PhysGrabber" },
    { g_enemy_rigidbody_class, "EnemyRigidbody" },
  };

  int total_hits = 0;
  std::string needle(keyword);
  for (const auto& t : targets) {
    if (!t.cls) continue;
    void* iter = nullptr;
    MonoMethod* m = nullptr;
    while ((m = g_mono.mono_class_get_methods(t.cls, &iter)) != nullptr) {
      const char* nm = g_mono.mono_method_get_name ? g_mono.mono_method_get_name(m) : nullptr;
      if (!nm) continue;
      if (CaseContains(nm, needle)) {
        std::ostringstream oss;
        oss << t.name << "::" << nm;
        out_results.push_back(oss.str());
        ++total_hits;
      }
      if (total_hits > 512) break;  // Non-ASCII comment normalized.
    }
    if (total_hits > 512) break;
  }

  std::ostringstream log;
  log << "MethodScan '" << needle << "': hits=" << total_hits;
  AppendLog(log.str());
  return total_hits > 0;
}

bool MonoScanMethodsWithBytes(const char* keyword, std::vector<std::string>& out_results) {
  out_results.clear();
  if (!keyword || !*keyword) return false;
  if (!EnsureThreadAttached() || !CacheManagedRefs()) return false;
  if (!g_mono.mono_class_get_methods || !g_mono.mono_method_get_name || !g_mono.mono_compile_method) {
    return false;
  }

  struct TargetCls {
    MonoClass* cls;
    const char* name;
  };
  TargetCls targets[] = {
    { g_round_director_class, "RoundDirector" },
    { g_extraction_point_class, "ExtractionPoint" },
    { g_phys_grab_cart_class, "PhysGrabCart" },
    { g_currency_ui_class, "CurrencyUI" },
    { g_pun_manager_class, "PunManager" },
    { g_player_controller_class, "PlayerController" },
  };

  std::string needle(keyword);
  int hits = 0;
  for (const auto& t : targets) {
    if (!t.cls) continue;
    void* iter = nullptr;
    MonoMethod* m = nullptr;
    while ((m = g_mono.mono_class_get_methods(t.cls, &iter)) != nullptr) {
      const char* nm = g_mono.mono_method_get_name(m);
      if (!nm || !CaseContains(nm, needle)) continue;

      void* code = g_mono.mono_compile_method(m);
      std::ostringstream oss;
      oss << t.name << "::" << nm;
      if (code) {
        uint8_t first[16]{};
        if (ReadCodeBytesSafe(code, first, sizeof(first))) {
          oss << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(code)
              << " bytes=" << HexBytes(first, sizeof(first));
        }
        else {
          oss << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(code) << " bytes=<unreadable>";
        }
      }
      else {
        oss << " addr=<jit-null>";
      }
      out_results.push_back(oss.str());
      ++hits;
      if (hits > 1024) break;
    }
    if (hits > 1024) break;
  }
  std::ostringstream log;
  log << "MethodScanBytes '" << keyword << "': hits=" << hits;
  AppendLog(log.str());
  return hits > 0;
}

bool MonoDumpCollectorNumericFields(std::vector<std::string>& out_results) {
  out_results.clear();
  if (!EnsureThreadAttached() || !CacheManagedRefs()) return false;
  if (!g_mono.mono_class_get_fields || !g_mono.mono_field_get_name) return false;

  auto dump_instance_fields = [&](MonoObject* obj, MonoClass* cls, const char* tag, int limit) -> int {
    if (!obj || !cls) return 0;
    int dumped = 0;
    void* iter = nullptr;
    MonoClassField* f = nullptr;
    while ((f = g_mono.mono_class_get_fields(cls, &iter)) != nullptr) {
      const int tc = GetFieldTypeCode(f);
      if (!IsNumericOrBoolTypeCode(tc)) continue;
      int v = 0;
      if (!ReadFieldNumber(obj, f, v)) continue;
      const char* fn = g_mono.mono_field_get_name(f);
      if (!fn) continue;
      std::ostringstream oss;
      oss << tag << "." << fn << "=" << v;
      out_results.push_back(oss.str());
      ++dumped;
      if (dumped >= limit) break;
    }
    return dumped;
  };

  int total = 0;
  MonoObject* rd = GetRoundDirectorInstance();
  if (rd && g_round_director_class) {
    total += dump_instance_fields(rd, g_round_director_class, "RoundDirector", 256);
  }
  if (g_round_extraction_point_current_field) {
    MonoObject* list_obj = nullptr;
    if (rd) g_mono.mono_field_get_value(rd, g_round_extraction_point_current_field, &list_obj);
    int sum = 0;
    if (ReadListTotalNumber(list_obj, sum)) {
      out_results.push_back(std::string("RoundDirector.extractionPointCurrent.sum=") + std::to_string(sum));
      ++total;
    }
  }
  if (g_round_dollar_haul_list_field) {
    MonoObject* list_obj = nullptr;
    if (rd) g_mono.mono_field_get_value(rd, g_round_dollar_haul_list_field, &list_obj);
    int sum = 0;
    if (ReadListTotalNumber(list_obj, sum)) {
      out_results.push_back(std::string("RoundDirector.dollarHaulList.sum=") + std::to_string(sum));
      ++total;
    }
  }

  MonoArray* eps = FindObjectsOfTypeArray(g_extraction_point_class, "ExtractionPointDump");
  if (eps && eps->max_length > 0) {
    const int n = static_cast<int>(eps->max_length > 3 ? 3 : eps->max_length);
    for (int i = 0; i < n; ++i) {
      MonoObject* ep = static_cast<MonoObject*>(eps->vector[i]);
      if (!ep) continue;
      std::ostringstream tag;
      tag << "ExtractionPoint[" << i << "]";
      std::string tag_str = tag.str();
      total += dump_instance_fields(ep, g_extraction_point_class, tag_str.c_str(), 128);
    }
  }

  std::ostringstream log;
  log << "CollectorFieldDump: lines=" << total;
  AppendLog(log.str());
  return total > 0;
}

bool MonoProbeCollectorMethods(const char* keyword, std::vector<std::string>& out_results) {
  out_results.clear();
  if (!keyword || !*keyword) return false;
  if (!EnsureThreadAttached() || !CacheManagedRefs()) return false;
  if (!g_mono.mono_class_get_methods || !g_mono.mono_method_get_name || !g_mono.mono_compile_method) {
    return false;
  }

  MonoObject* rd = GetRoundDirectorInstance();
  MonoObject* ep0 = nullptr;
  MonoObject* cart0 = nullptr;
  if (MonoArray* eps = FindObjectsOfTypeArray(g_extraction_point_class, "ProbeExtractionPoint")) {
    if (eps->max_length > 0) ep0 = static_cast<MonoObject*>(eps->vector[0]);
  }
  if (MonoArray* carts = FindObjectsOfTypeArray(g_phys_grab_cart_class, "ProbePhysGrabCart")) {
    if (carts->max_length > 0) cart0 = static_cast<MonoObject*>(carts->vector[0]);
  }

  struct TargetCls {
    MonoClass* cls;
    const char* name;
    MonoObject* instance;
  };
  TargetCls targets[] = {
    { g_round_director_class, "RoundDirector", rd },
    { g_extraction_point_class, "ExtractionPoint", ep0 },
    { g_phys_grab_cart_class, "PhysGrabCart", cart0 },
    { g_currency_ui_class, "CurrencyUI", nullptr },
    { g_pun_manager_class, "PunManager", nullptr },
  };

  const std::string needle(keyword);
  int hits = 0;
  for (const auto& t : targets) {
    if (!t.cls) continue;
    void* iter = nullptr;
    MonoMethod* m = nullptr;
    while ((m = g_mono.mono_class_get_methods(t.cls, &iter)) != nullptr) {
      const char* nm_raw = g_mono.mono_method_get_name(m);
      if (!nm_raw) continue;
      std::string nm(nm_raw);
      if (!CaseContains(nm, needle)) continue;

      const int argc = GetMethodParamCount(m);
      if (argc >= 0 && argc != 0) continue;
      const int rtc = GetMethodReturnTypeCode(m);
      if (rtc >= 0 && !IsNumericOrBoolTypeCode(rtc)) continue;

      int value = 0;
      bool used_static = false;
      const bool ok = InvokeNoArgMethodNumber(m, t.instance, rtc, value, used_static);
      void* code = g_mono.mono_compile_method(m);
      std::ostringstream oss;
      oss << t.name << "::" << nm << " argc=" << (argc >= 0 ? argc : -1)
          << " ret=" << MonoTypeCodeName(rtc)
          << " invoke=" << (ok ? "ok" : "fail");
      if (ok) {
        oss << " value=" << value << (used_static ? " [static]" : " [instance]");
      }
      if (code) {
        uint8_t first[16]{};
        if (ReadCodeBytesSafe(code, first, sizeof(first))) {
          oss << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(code)
              << " bytes=" << HexBytes(first, sizeof(first));
        }
        else {
          oss << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(code) << " bytes=<unreadable>";
        }
      }
      out_results.push_back(oss.str());
      ++hits;
      if (hits >= 1024) break;
    }
    if (hits >= 1024) break;
  }

  std::ostringstream log;
  log << "ProbeCollectorMethods '" << keyword << "': lines=" << hits;
  AppendLog(log.str());
  return hits > 0;
}

bool MonoPatchCollectorGetters(int forced_value, std::vector<std::string>& out_results) {
  out_results.clear();
  if (!EnsureThreadAttached() || !CacheManagedRefs()) return false;
  if (!g_mono.mono_class_get_methods || !g_mono.mono_method_get_name || !g_mono.mono_compile_method) {
    return false;
  }

  struct TargetCls {
    MonoClass* cls;
    const char* name;
  };
  TargetCls targets[] = {
    { g_round_director_class, "RoundDirector" },
    { g_extraction_point_class, "ExtractionPoint" },
    { g_phys_grab_cart_class, "PhysGrabCart" },
    { g_currency_ui_class, "CurrencyUI" },
  };

  int patched = 0;
  for (const auto& t : targets) {
    if (!t.cls) continue;
    void* iter = nullptr;
    MonoMethod* m = nullptr;
    while ((m = g_mono.mono_class_get_methods(t.cls, &iter)) != nullptr) {
      const char* nm_raw = g_mono.mono_method_get_name(m);
      if (!nm_raw) continue;
      std::string nm(nm_raw);
      if (!IsCollectorKeywordHit(nm)) continue;
      if (CaseContains(nm, "set") || CaseContains(nm, "update") || CaseContains(nm, "refresh") ||
          CaseContains(nm, "awake") || CaseContains(nm, "late")) {
        continue;
      }

      const int argc = GetMethodParamCount(m);
      if (argc > 2) continue;

      const int tc = GetMethodReturnTypeCode(m);
      if (tc >= 0 && !IsIntLikeTypeCode(tc)) {
        continue;
      }

      void* code = g_mono.mono_compile_method(m);
      if (!code) continue;
      if (g_collector_getter_patches.find(code) != g_collector_getter_patches.end()) {
        continue;
      }
      CodePatchBackup backup{};
      if (!PatchCodeToReturnInt(code, forced_value, backup)) {
        continue;
      }
      g_collector_getter_patches.emplace(code, backup);
      std::ostringstream oss;
      oss << "patched " << t.name << "::" << nm << " addr=0x" << std::hex
          << reinterpret_cast<uintptr_t>(code) << " => " << std::dec << forced_value;
      out_results.push_back(oss.str());
      ++patched;
      if (patched >= 64) break;
    }
    if (patched >= 64) break;
  }

  std::ostringstream log;
  log << "PatchCollectorGetters: patched=" << patched << " forced=" << forced_value;
  AppendLog(log.str());
  return patched > 0;
}

bool MonoRestoreCollectorGetterPatches(std::vector<std::string>& out_results) {
  out_results.clear();
  int restored = 0;
  for (auto it = g_collector_getter_patches.begin(); it != g_collector_getter_patches.end(); ++it) {
    void* addr = it->first;
    const CodePatchBackup& backup = it->second;
    if (RestorePatchedCode(addr, backup)) {
      std::ostringstream oss;
      oss << "restored addr=0x" << std::hex << reinterpret_cast<uintptr_t>(addr);
      out_results.push_back(oss.str());
      ++restored;
    }
  }
  g_collector_getter_patches.clear();

  std::ostringstream log;
  log << "RestoreCollectorGetterPatches: restored=" << restored;
  AppendLog(log.str());
  return restored > 0;
}

bool MonoGetLogs(int max_lines, std::vector<std::string>& out_logs) {
  return GetLogSnapshot(max_lines > 0 ? max_lines : 0, out_logs);
}

bool MonoReviveAllPlayers(bool include_local) {
  try {
    if (!CacheManagedRefs()) return false;
    if (!g_player_avatar_is_local_field || !g_player_avatar_health_field) return false;

// Non-ASCII comment normalized.
    MonoObject* list_obj = nullptr;
    if (g_game_director_instance_field && g_game_director_player_list_field && g_game_director_vtable) {
      MonoObject* director = nullptr;
      g_mono.mono_field_static_get_value(g_game_director_vtable, g_game_director_instance_field, &director);
      if (director) {
        g_mono.mono_field_get_value(director, g_game_director_player_list_field, &list_obj);
      }
    }
    if (!list_obj && g_player_get_all_method) {
      MonoObject* exc = nullptr;
      list_obj = g_mono.mono_runtime_invoke(g_player_get_all_method, nullptr, nullptr, &exc);
    }
    if (!list_obj) {
      AppendLog("MonoReviveAllPlayers: no player list object");
      return false;
    }

    MonoClass* list_class = g_mono.mono_object_get_class(list_obj);
    static MonoClassField* items_field = nullptr;
    static MonoClassField* size_field = nullptr;
    if (!items_field || !size_field) {
      items_field = g_mono.mono_class_get_field_from_name(list_class, "_items");
      size_field = g_mono.mono_class_get_field_from_name(list_class, "_size");
    }

    auto revive_avatar = [&](MonoObject* avatar) -> bool {
      if (!avatar) return false;
      bool is_local = false;
      g_mono.mono_field_get_value(avatar, g_player_avatar_is_local_field, &is_local);
      if (!include_local && is_local) return false;

      MonoObject* health_obj = nullptr;
      g_mono.mono_field_get_value(avatar, g_player_avatar_health_field, &health_obj);
      if (!health_obj) return false;

      int max_hp = 100;
      if (g_player_health_max_field) {
        g_mono.mono_field_get_value(health_obj, g_player_health_max_field, &max_hp);
      }
      if (max_hp <= 0) max_hp = 100;

      if (g_player_health_value_field) {
        g_mono.mono_field_set_value(health_obj, g_player_health_value_field, &max_hp);
      }
      if (g_player_health_max_field) {
        g_mono.mono_field_set_value(health_obj, g_player_health_max_field, &max_hp);
      }
// Non-ASCII comment normalized.
      if (g_player_health_invincible_set) {
        float dur = 5.0f;
        MonoObject* exc = nullptr;
        void* args[1] = { &dur };
        g_mono.mono_runtime_invoke(g_player_health_invincible_set, health_obj, args, &exc);
      }
      return true;
    };

    int revived = 0;
    MonoArray* items = nullptr;
    int list_size = 0;
    if (items_field && size_field) {
      g_mono.mono_field_get_value(list_obj, items_field, &items);
      g_mono.mono_field_get_value(list_obj, size_field, &list_size);
    }
    if (items && items->vector && items->max_length > 0 && list_size > 0) {
      int lim = list_size;
      if (lim > static_cast<int>(items->max_length)) lim = static_cast<int>(items->max_length);
      for (int i = 0; i < lim; ++i) {
        MonoObject* avatar = static_cast<MonoObject*>(items->vector[i]);
        if (revive_avatar(avatar)) ++revived;
      }
    }
    else {
// Non-ASCII comment normalized.
      EnumerateListObjects(list_obj, [&](MonoObject* avatar) -> bool {
        if (revive_avatar(avatar)) {
          ++revived;
          return true;
        }
        return false;
      });
    }

    std::ostringstream oss;
    oss << "MonoReviveAllPlayers: revived " << revived << " players"
      << (include_local ? " (include local)" : "");
    AppendLog(oss.str());
    return revived > 0;
  }
  catch (...) {
    return false;
  }
}

bool MonoGetCameraMatrices(Matrix4x4& view, Matrix4x4& projection) {
  try {
    view = {};
    projection = {};
    if (!CacheManagedRefs()) {
      AppendLog("MonoGetCameraMatrices: CacheManagedRefs failed");
      return false;
    }

    // Log which camera methods are available (once)
    static bool logged_method_status = false;
    if (!logged_method_status) {
      std::ostringstream oss;
      oss << "Camera methods status: ";
      oss << "get_main=" << (g_unity_camera_get_main ? "yes" : "no") << ", ";
      oss << "get_projectionMatrix=" << (g_unity_camera_get_projection_matrix ? "yes" : "no") << ", ";
      oss << "get_worldToCameraMatrix=" << (g_unity_camera_get_world_to_camera_matrix ? "yes" : "no") << ", ";
      oss << "get_transform=" << (g_unity_camera_get_transform ? "yes" : "no") << ", ";
      oss << "component_get_transform=" << (g_component_get_transform ? "yes" : "no");
      AppendLog(oss.str());
      logged_method_status = true;
    }

    static bool logged_transform_status = false;
    if (!logged_transform_status) {
      std::ostringstream oss;
      oss << "Transform methods status: ";
      oss << "get_worldToLocalMatrix=" << (g_transform_get_world_to_local ? "yes" : "no") << ", ";
      oss << "get_localToWorldMatrix=" << (g_transform_get_local_to_world ? "yes" : "no");
      AppendLog(oss.str());
      logged_transform_status = true;
    }

    if (!g_unity_camera_get_main || !g_unity_camera_get_projection_matrix) {
      AppendLog("MonoGetCameraMatrices: camera methods not resolved");
      return false;
    }

    MonoObject* exception = nullptr;
    MonoObject* main_camera =
      g_mono.mono_runtime_invoke(g_unity_camera_get_main, nullptr, nullptr, &exception);
    if (exception || !main_camera) {
      AppendLog("MonoGetCameraMatrices: get_main failed");
      return false;
    }

    MonoObject* proj_obj = g_mono.mono_runtime_invoke(
      g_unity_camera_get_projection_matrix, main_camera, nullptr, &exception);
    if (exception || !proj_obj) {
      AppendLog("MonoGetCameraMatrices: get_projectionMatrix failed");
      return false;
    }

    MonoObject* view_obj = nullptr;

    // Try worldToCameraMatrix first
    if (g_unity_camera_get_world_to_camera_matrix) {
      view_obj = g_mono.mono_runtime_invoke(
        g_unity_camera_get_world_to_camera_matrix, main_camera, nullptr, &exception);
      if (exception) {
        exception = nullptr;
        view_obj = nullptr;
      }
    }

    // Fallback to Transform::get_worldToLocalMatrix
    if (!view_obj && g_transform_get_world_to_local) {
      MonoObject* transform_obj = nullptr;

      if (g_unity_camera_get_transform) {
        transform_obj = g_mono.mono_runtime_invoke(
          g_unity_camera_get_transform, main_camera, nullptr, &exception);
        if (exception) {
          exception = nullptr;
          transform_obj = nullptr;
        }
      }

      if (!transform_obj && g_component_get_transform) {
        transform_obj = g_mono.mono_runtime_invoke(
          g_component_get_transform, main_camera, nullptr, &exception);
        if (exception) {
          exception = nullptr;
          transform_obj = nullptr;
        }
      }

      if (transform_obj) {
        view_obj = g_mono.mono_runtime_invoke(
          g_transform_get_world_to_local, transform_obj, nullptr, &exception);
        if (exception) {
          exception = nullptr;
          view_obj = nullptr;
        }
      }
    }

    if (!view_obj) {
      AppendLog("MonoGetCameraMatrices: failed to get view matrix via any method");
      return false;
    }

    void* proj_data = g_mono.mono_object_unbox ? g_mono.mono_object_unbox(proj_obj) : nullptr;
    void* view_data = g_mono.mono_object_unbox ? g_mono.mono_object_unbox(view_obj) : nullptr;
    if (!proj_data) {
      AppendLog("MonoGetCameraMatrices: failed to unbox projection matrix");
      return false;
    }
    if (!view_data) {
      AppendLog("MonoGetCameraMatrices: failed to unbox view matrix");
      return false;
    }

    std::memcpy(projection.m, proj_data, sizeof(projection.m));
    std::memcpy(view.m, view_data, sizeof(view.m));

    static bool logged_matrices_once = false;
    if (!logged_matrices_once) {
      std::ostringstream oss1;
      oss1 << "Projection matrix [0-3]: ";
      for (int i = 0; i < 4; ++i) oss1 << projection.m[i] << " ";
      AppendLog(oss1.str());
      std::ostringstream oss2;
      oss2 << "View matrix [0-3]: ";
      for (int i = 0; i < 4; ++i) oss2 << view.m[i] << " ";
      AppendLog(oss2.str());
      logged_matrices_once = true;
    }

    return true;
  } catch (...) {
    AppendLog("MonoGetCameraMatrices: exception caught");
    return false;
  }
}

// ---------------------------------------------------------------------------
// Valuable discover helpers (native highlight)
bool MonoValueFieldsResolved() {
  return g_valuable_discover_class && g_valuable_discover_new_method && g_valuable_discover_instance_field;
}

bool MonoTriggerValuableDiscover(int state, int max_items, int& out_count) {
  out_count = 0;
  if (g_shutting_down) return false;
  if (!CacheManagedRefs()) return false;
  if (!g_valuable_discover_class || !g_valuable_discover_new_method || !g_valuable_discover_instance_field) {
    AppendLogOnce("ValuableDiscover_missing", "ValuableDiscover: class/method/instance unresolved");
    return false;
  }
  if (!g_phys_grab_object_class || !g_game_object_get_component || !g_component_get_game_object) {
    AppendLogOnce("ValuableDiscover_missing_pgo", "ValuableDiscover: PhysGrabObject/GameObject::GetComponent missing");
    return false;
  }

  MonoObject* discover_instance = nullptr;
  if (g_valuable_discover_vtable && g_valuable_discover_instance_field) {
    g_mono.mono_field_static_get_value(
      g_valuable_discover_vtable, g_valuable_discover_instance_field, &discover_instance);
  }
  if (!discover_instance) {
    AppendLogOnce("ValuableDiscover_no_instance", "ValuableDiscover: instance is null");
    return false;
  }

  MonoType* pgo_type = g_mono.mono_class_get_type ? g_mono.mono_class_get_type(g_phys_grab_object_class) : nullptr;
  MonoDomain* dom = g_domain ? g_domain : g_mono.mono_get_root_domain();
  MonoObject* pgo_type_obj = (pgo_type && g_mono.mono_type_get_object) ? g_mono.mono_type_get_object(dom, pgo_type) : nullptr;

  // Prevent runaway loops on large maps: cap objects and time budget
  const int kObjCap = max_items > 0 ? std::min(max_items, 512) : 512;
  const uint64_t start_ms = GetTickCount64();
  const uint64_t kBudgetMs = 30;  // keep under ~30ms to avoid hitching Present thread

  auto call_new_on_array = [&](MonoArray* arr) {
    if (!IsValidArray(arr)) return 0;
    int limit = static_cast<int>(arr->max_length);
    if (max_items > 0 && limit > max_items) limit = max_items;
    if (limit > kObjCap) limit = kObjCap;
    int hit = 0;
    for (int i = 0; i < limit; ++i) {
      if (GetTickCount64() - start_ms > kBudgetMs) {
        AppendLogInternal(LogLevel::kWarn, "valuable",
          "ValuableDiscover.New exceeded time budget, early-exit");
        break;
      }
      MonoObject* obj = static_cast<MonoObject*>(arr->vector[i]);
      if (!obj) continue;
      void* new_args[2] = { obj, &state };
      MonoObject* ret = SafeInvoke(g_valuable_discover_new_method, discover_instance, new_args, "ValuableDiscover.New");
      if (ret) ++hit;
    }
    return hit;
  };

// Non-ASCII comment normalized.
  int total_hit = 0;
  if (g_object_find_objects_of_type_include_inactive && pgo_type_obj) {
    bool include_inactive = true;
    void* args2[2] = { pgo_type_obj, &include_inactive };  // bool includeInactive=true
    MonoObject* arr_obj =
      SafeInvoke(g_object_find_objects_of_type_include_inactive, nullptr, args2, "FindObjectsOfType(PhysGrabObject, true)");
    total_hit += call_new_on_array(reinterpret_cast<MonoArray*>(arr_obj));
  }

// Non-ASCII comment normalized.
  if (total_hit == 0 && g_physics_overlap_sphere && g_component_get_transform) {
    float center[3] = { 0.0f, 0.0f, 0.0f };
    PlayerState local_state{};
    if (MonoGetLocalPlayerState(local_state) && local_state.has_position) {
      center[0] = local_state.x;
      center[1] = local_state.y;
      center[2] = local_state.z;
    }
    float radius = 500.0f;
    int query = 2;
    int layer_mask = -1;
    MonoObject* arr_obj = nullptr;
    MonoObject* exc = nullptr;
    void* args4[4] = { center, &radius, &layer_mask, &query };
    void* args3[3] = { center, &radius, &layer_mask };
    void* args2b[2] = { center, &radius };
    switch (g_physics_overlap_sphere_argc) {
    case 4: arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args4, "Physics.OverlapSphere"); break;
    case 3: arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args3, "Physics.OverlapSphere3"); break;
    case 2: arr_obj = SafeInvoke(g_physics_overlap_sphere, nullptr, args2b, "Physics.OverlapSphere2"); break;
    default: break;
    }
    if (!exc && arr_obj && pgo_type_obj) {
      MonoArray* colliders = reinterpret_cast<MonoArray*>(arr_obj);
      if (!IsValidArray(colliders)) {
        out_count = total_hit;
        return total_hit > 0;
      }
      int limit = static_cast<int>(colliders->max_length);
      if (limit > 4096) limit = 4096;
      for (int i = 0; i < limit; ++i) {
        if (GetTickCount64() - start_ms > kBudgetMs) {
          AppendLogInternal(LogLevel::kWarn, "valuable",
            "ValuableDiscover overlap-sphere exceeded time budget, early-exit");
          break;
        }
        MonoObject* collider = static_cast<MonoObject*>(colliders->vector[i]);
        if (!collider) continue;
        MonoObject* game_obj =
          g_component_get_game_object
          ? SafeInvoke(g_component_get_game_object, collider, nullptr, "Collider.get_gameObject")
          : nullptr;
        if (!game_obj) continue;
        void* comp_args[1] = { pgo_type_obj };
        MonoObject* phys_grab =
          SafeInvoke(g_game_object_get_component, game_obj, comp_args, "GameObject.GetComponent(PhysGrabObject)");
        if (!phys_grab) continue;
        void* new_args[2] = { phys_grab, &state };
        MonoObject* ret = SafeInvoke(g_valuable_discover_new_method, discover_instance, new_args, "ValuableDiscover.New");
        if (ret) ++total_hit;
      }
    }
  }

  out_count = total_hit;
  return total_hit > 0;
}

bool MonoApplyValuableDiscoverPersistence(bool enable, float wait_seconds, int& out_count) {
  out_count = 0;
  if (g_shutting_down) return false;
  if (!CacheManagedRefs()) return false;
  if (!enable) return true;

  MonoObject* discover_instance = nullptr;
  if (g_valuable_discover_vtable && g_valuable_discover_instance_field) {
    g_mono.mono_field_static_get_value(
      g_valuable_discover_vtable, g_valuable_discover_instance_field, &discover_instance);
  }
  if (!discover_instance) return false;

  if (g_valuable_discover_hide_timer_field) {
    float zero = 0.0f;
    g_mono.mono_field_set_value(discover_instance, g_valuable_discover_hide_timer_field, &zero);
  }
  if (g_valuable_discover_hide_alpha_field) {
    float one = 1.0f;
    g_mono.mono_field_set_value(discover_instance, g_valuable_discover_hide_alpha_field, &one);
  }
  return true;
}

// Non-ASCII comment normalized.
bool MonoTriggerValuableDiscoverSafe(int state, int max_items, int& out_count) {
  out_count = 0;
  static uint64_t s_last_exception_ms = 0;
  static int s_exception_burst = 0;
  try {
    bool ok = MonoTriggerValuableDiscover(state, max_items, out_count);
    if (ok) {
      s_exception_burst = 0;
    }
    return ok;
  }
  catch (...) {
    const uint64_t now = GetTickCount64();
    if (now - s_last_exception_ms > 5000) {
      s_exception_burst = 0;
    }
    s_last_exception_ms = now;
    ++s_exception_burst;

    g_native_highlight_failed = true;
    g_native_highlight_active = false;
    {
      std::ostringstream oss;
      oss << "MonoTriggerValuableDiscoverSafe: exception, native highlight disabled"
          << " burst=" << s_exception_burst;
      AppendLogInternal(LogLevel::kError, "valuable", oss.str());
    }

    // Repeated faults in a short window likely indicate deeper corruption.
    // Keep previous hard-fallback behavior only for burst failures.
    if (s_exception_burst >= 3) {
      g_overlay_disabled = true;
      AppendLogInternal(LogLevel::kError, "valuable",
                        "MonoTriggerValuableDiscoverSafe: repeated exceptions, overlay disabled");
    }
    return false;
  }
}

bool MonoApplyValuableDiscoverPersistenceSafe(bool enable, float wait_seconds, int& out_count) {
  out_count = 0;
  try {
    return MonoApplyValuableDiscoverPersistence(enable, wait_seconds, out_count);
  }
  catch (...) {
    g_native_highlight_failed = true;
    g_native_highlight_active = false;
    AppendLogInternal(LogLevel::kWarn, "valuable",
                      "MonoApplyValuableDiscoverPersistenceSafe: exception, native highlight disabled");
    return false;
  }
}

bool MonoNativeHighlightAvailable() {
  return !g_native_highlight_failed;
}



