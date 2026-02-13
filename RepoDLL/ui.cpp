#include "pch.h"

#include "ui.h"

#include "imgui.h"

#include "mono_bridge.h"
#include "hook_dx11.h"
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

#pragma execution_character_set("utf-8")

namespace {

struct EditableFields {
  float pos[3] = {0.f, 0.f, 0.f};
  int health = 0;
  int max_health = 0;
  float stamina = 0.f;
  float max_stamina = 0.f;
};

struct FieldLocks {
  bool position = false;
  bool health = false;
  bool stamina = false;
};

struct WeaponUiConfig {
  bool enabled = false;
  bool rapid_fire = false;
  bool magic_bullet = false;
  bool infinite_ammo = false;
  bool no_recoil = false;
  float rapid_fire_cooldown = 0.02f;
};

std::vector<PlayerState> g_cached_items;
std::vector<PlayerState> g_cached_weapons;
std::vector<PlayerState> g_cached_enemies;
Matrix4x4 g_cached_view{};
Matrix4x4 g_cached_proj{};
bool g_cached_mats_valid = false;
uint64_t g_last_matrix_update = 0;

struct SavedSettings {
  bool auto_refresh = false;
  bool auto_refresh_items = false;
  bool auto_refresh_enemies = false;
  bool item_esp = false;
  bool enemy_esp = false;
  bool native_highlight = false;
  bool no_fall = false;
  bool load_on_start = true;
  bool reset_each_round = false;
  int menu_toggle_vk = VK_INSERT;
  bool third_person_enabled = false;
  int third_person_toggle_vk = VK_F6;
  float third_person_distance = 2.8f;
  float third_person_height = 1.15f;
  float third_person_shoulder = 0.35f;
  float third_person_smooth = 2.0f;
  float speed_mult = 1.0f;
  std::string log_path;
};

std::string SettingsPath() {
  std::filesystem::path p(MonoGetLogPath());
  return (p.parent_path() / "repodll_settings.ini").string();
}

void ResetUiDefaults(bool &auto_refresh, bool &auto_refresh_items, bool &auto_refresh_enemies,
  bool &item_esp, bool &enemy_esp, bool &native_highlight, bool &no_fall,
  float &speed_mult, int &extra_jump_count, bool &infinite_jump_enabled,
  bool &god_mode_enabled) {
  auto_refresh = false;
  auto_refresh_items = false;
  auto_refresh_enemies = false;
  item_esp = false;
  enemy_esp = false;
  native_highlight = false;
  no_fall = false;
  speed_mult = 1.0f;
  extra_jump_count = 0;
  infinite_jump_enabled = false;
  god_mode_enabled = false;
}

void SaveSettings(const SavedSettings& s) {
  std::ofstream f(SettingsPath(), std::ios::trunc);
  if (!f) return;
  f << "auto_refresh=" << s.auto_refresh << "\n";
  f << "auto_refresh_items=" << s.auto_refresh_items << "\n";
  f << "auto_refresh_enemies=" << s.auto_refresh_enemies << "\n";
  f << "item_esp=" << s.item_esp << "\n";
  f << "enemy_esp=" << s.enemy_esp << "\n";
  f << "native_highlight=" << s.native_highlight << "\n";
  f << "no_fall=" << s.no_fall << "\n";
  f << "speed_mult=" << s.speed_mult << "\n";
  f << "load_on_start=" << s.load_on_start << "\n";
  f << "reset_each_round=" << s.reset_each_round << "\n";
  f << "menu_toggle_vk=" << s.menu_toggle_vk << "\n";
  f << "third_person_enabled=" << s.third_person_enabled << "\n";
  f << "third_person_toggle_vk=" << s.third_person_toggle_vk << "\n";
  f << "third_person_distance=" << s.third_person_distance << "\n";
  f << "third_person_height=" << s.third_person_height << "\n";
  f << "third_person_shoulder=" << s.third_person_shoulder << "\n";
  f << "third_person_smooth=" << s.third_person_smooth << "\n";
  f << "log_path=" << s.log_path << "\n";
}

bool LoadSettings(SavedSettings& out) {
  std::ifstream f(SettingsPath());
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    std::string k = line.substr(0, pos);
    std::string v = line.substr(pos + 1);
    auto to_bool = [](const std::string& s) { return s == "1" || s == "true" || s == "True"; };
    try {
      if (k == "auto_refresh") out.auto_refresh = to_bool(v);
      else if (k == "auto_refresh_items") out.auto_refresh_items = to_bool(v);
      else if (k == "auto_refresh_enemies") out.auto_refresh_enemies = to_bool(v);
      else if (k == "item_esp") out.item_esp = to_bool(v);
      else if (k == "enemy_esp") out.enemy_esp = to_bool(v);
      else if (k == "native_highlight") out.native_highlight = to_bool(v);
      else if (k == "no_fall") out.no_fall = to_bool(v);
      else if (k == "speed_mult") out.speed_mult = std::stof(v);
      else if (k == "load_on_start") out.load_on_start = to_bool(v);
      else if (k == "reset_each_round") out.reset_each_round = to_bool(v);
      else if (k == "menu_toggle_vk") out.menu_toggle_vk = std::stoi(v);
      else if (k == "third_person_enabled") out.third_person_enabled = to_bool(v);
      else if (k == "third_person_toggle_vk") out.third_person_toggle_vk = std::stoi(v);
      else if (k == "third_person_distance") out.third_person_distance = std::stof(v);
      else if (k == "third_person_height") out.third_person_height = std::stof(v);
      else if (k == "third_person_shoulder") out.third_person_shoulder = std::stof(v);
      else if (k == "third_person_smooth") out.third_person_smooth = std::stof(v);
      else if (k == "log_path") out.log_path = v;
    } catch (...) {
      continue;
    }
  }
  return true;
}

void ApplyOverlayStyleOnce() {
  static bool styled = false;
  if (styled) return;
  styled = true;

  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 3.0f;
  style.TabRounding = 5.0f;
  style.ItemSpacing = ImVec2(6.0f, 4.0f);

  ImVec4 accent = ImVec4(0.22f, 0.74f, 0.48f, 1.0f);
  ImVec4 accent_bg = ImVec4(accent.x, accent.y, accent.z, 0.16f);
  ImVec4 muted = ImVec4(0.24f, 0.34f, 0.42f, 1.0f);

  ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive] = accent_bg;
  ImGui::GetStyle().Colors[ImGuiCol_Header] = accent_bg;
  ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
  ImGui::GetStyle().Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
  ImGui::GetStyle().Colors[ImGuiCol_CheckMark] = accent;
  ImGui::GetStyle().Colors[ImGuiCol_SliderGrab] = accent;
  ImGui::GetStyle().Colors[ImGuiCol_Button] = accent_bg;
  ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
  ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
  ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered] = ImVec4(accent.x, accent.y, accent.z, 0.18f);
  ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = muted;
}

void SectionLabel(const char* label) {
  ImGui::Spacing();
  ImVec4 col = ImVec4(0.22f, 0.74f, 0.48f, 1.0f);
  ImGui::TextColored(col, "%s", label);
  // Animated underline for a bit of motion
  ImVec2 start = ImGui::GetItemRectMin();
  ImVec2 end = ImGui::GetItemRectMax();
  float t = ImGui::GetTime();
  float pulse = 0.5f + 0.5f * sinf(t * 3.0f);
  ImVec4 line_col = ImVec4(col.x, col.y, col.z, 0.35f + 0.35f * pulse);
  ImGui::GetWindowDrawList()->AddRectFilled(
    ImVec2(start.x, end.y + 2.0f),
    ImVec2(end.x, end.y + 6.0f),
    ImColor(line_col));
  ImGui::Separator();
}

std::string MenuHotkeyName(int vk) {
  if (vk == VK_INSERT) {
    return "INS";
  }
  if (vk <= 0 || vk > 0xFF) {
    return "Unknown";
  }
  UINT scan = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  const bool extended =
    vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN ||
    vk == VK_PRIOR || vk == VK_NEXT || vk == VK_END || vk == VK_HOME ||
    vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_NUMLOCK ||
    vk == VK_RMENU || vk == VK_RCONTROL;
  LONG lparam = static_cast<LONG>(scan << 16);
  if (extended) {
    lparam |= 1 << 24;
  }
  char name[64] = {};
  if (GetKeyNameTextA(lparam, name, static_cast<int>(sizeof(name))) > 0) {
    return name;
  }
  return std::string("VK_") + std::to_string(vk);
}

std::string DescribeGateReason(const std::string& reason) {
  if (reason.empty()) return "未知";
  if (reason == "managed_refs_unavailable") return "Mono引用未就绪";
  if (reason == "PhotonNetwork.InRoom=false") return "未进入房间（联机会话未建立）";
  if (reason == "PhotonNetwork.LevelLoadingProgress") return "关卡加载中";
  if (reason == "RunManager.restarting") return "RunManager 正在重开";
  if (reason == "RunManager.waitToChangeScene") return "等待切换地图";
  if (reason == "RunManager.lobbyJoin") return "正在加入大厅/会话";
  if (reason == "GameDirector.currentState=Load/EndWait") return "GameDirector 处于 Load/EndWait";
  if (reason == "LevelGenerator.Generated=false") return "地图尚未生成完成";
  if (reason == "LocalPlayerUnavailable") return "本地玩家对象未找到";
  if (reason == "LocalPlayerPositionUnavailable") return "本地玩家坐标未就绪";
  if (reason == "stable" || reason == "stable(InRoomUnknown)") return "稳定";
  return reason;
}
}  // namespace

const std::vector<PlayerState>& UiGetCachedItems() { return g_cached_items; }

const std::vector<PlayerState>& UiGetCachedEnemies() { return g_cached_enemies; }

bool UiGetCachedMatrices(Matrix4x4& view, Matrix4x4& proj) {
  if (!g_cached_mats_valid) return false;
  view = g_cached_view;
  proj = g_cached_proj;
  return true;
}

void RenderOverlay(bool* menu_open) {
  const bool menu_visible = menu_open && *menu_open;
  static bool last_menu_open = false;
  SetCrashStage("RenderOverlay:enter");

  static LocalPlayerInfo last_info;
  static bool last_ok = false;
  static uint64_t last_update = 0;
  static PlayerState last_state;
  static EditableFields edits;
  static FieldLocks locks;
  static bool lock_health = false;
  static bool lock_stamina = false;
  static bool inputs_synced = false;
  static bool auto_refresh = false;  // 默认关闭，需用户开启
  static bool auto_refresh_items = false;  // 保留设置兼容，不再驱动自动轮询
  static bool auto_refresh_enemies = false;
  static std::vector<PlayerState> squad_states;
  static uint64_t last_squad_update = 0;
  static int squad_selected_index = -1;
  static void* squad_selected_object = nullptr;
  static int squad_target_health = 100;
  static int squad_target_max_health = 100;
  static float squad_target_pos[3] = { 0.0f, 0.0f, 0.0f };
  static float squad_pull_offset[3] = { 0.8f, 0.0f, 0.8f };
  static int native_highlight_state = 0;  // 0=Default, 1=Reminder, 2=Bad
  static int native_highlight_limit = 160;
  static uint64_t last_highlight_tick = 0;
  static uint64_t last_persist_tick = 0;
  static int last_highlight_count = 0;
  static int currency_edit = 999999;
  static int round_current_edit = 0;
  static int round_goal_edit = 0;
  static bool round_lock_enabled = false;
  static int round_progress_completed_edit = 0;
  static int round_progress_total_edit = 0;
  static int round_stage_edit = 0;
  static bool round_progress_all_completed_edit = false;
  static float speed_mult = 1.0f;      // 默认与游戏一致，不主动改动
  static int extra_jump_count = 0;     // 初始 0，用户开启时如未设值会自动提升
  static float jump_cooldown = 0.0f;   // 仅在用户修改后生效
  static int grab_strength = 1000;
  static float third_person_distance = 2.8f;
  static float third_person_height = 1.15f;
  static float third_person_shoulder = 0.35f;
  static float third_person_smooth = 2.0f;
  static bool infinite_jump_enabled = false;  // 默认关闭，需手动开启
  static bool god_mode_enabled = false;
  static float jump_force = 20.0f;
  static float grab_range_field = 5.0f;
  static float grab_strength_field = 5.0f;
  static int current_currency = 0;
  static bool has_currency = false;
  static uint64_t last_user_edit = 0;
  static uint64_t last_enemies_update = 0;
  static bool no_fall_enabled = false;
  static bool include_local_squad = true;
  static std::unordered_map<void*, WeaponUiConfig> weapon_mod_configs;
  static std::unordered_map<void*, void*> weapon_identity_cache;
  static int weapon_selected_index = -1;
  static void* weapon_selected_object = nullptr;
  static void* current_weapon_object = nullptr;
  static int weapon_mod_applied_count = 0;
  static int weapon_mod_magic_active_count = 0;
  static bool weapon_mod_last_ok = false;
  static bool weapon_cache_dirty = true;
  static uint64_t item_snapshot_hash1 = 0;
  static uint64_t item_snapshot_hash2 = 0;
  static size_t item_snapshot_count = 0;
  static bool item_snapshot_ready = false;
  static RoundState cached_round_state{};
  static bool has_round_state = false;
  static RoundProgressState cached_round_progress{};
  static bool has_round_progress = false;
  static RunLevelInfo cached_level_info{};
  static bool has_level_info = false;
  static uint64_t last_round_update = 0;
  static uint64_t last_level_info_update = 0;
  static bool scan_pause_pending = false;
  static int scan_pause_stable_samples = 0;
  static std::vector<std::string> debug_logs;
  static std::vector<std::string> collector_scan_results;
  static char collector_scan_kw[64] = "haul";
  static int collector_force_value = 999999;
  static uint64_t last_log_update = 0;
  static int log_lines = 200;
  static SavedSettings saved{};
  static bool settings_loaded = false;
  static bool session_master_transitioning = false;
  static SessionRuntimeGate runtime_gate{};
  static bool runtime_gate_ok = false;
  static uint64_t last_runtime_gate_update = 0;
  static bool is_real_master = false;
  static uint64_t last_master_state_update = 0;
  static char log_path_buf[260] = {};

  const uint64_t now = GetTickCount64();
  const bool mono_ready = MonoInitialize();
  const bool user_editing = ImGui::IsAnyItemActive();
  if (user_editing) {
    last_user_edit = now;
  }
  if (!settings_loaded) {
    saved.log_path = MonoGetLogPath();
    LoadSettings(saved);
    SetMenuToggleVirtualKey(saved.menu_toggle_vk);
    SetThirdPersonToggleVirtualKey(saved.third_person_toggle_vk);
    third_person_distance = saved.third_person_distance;
    third_person_height = saved.third_person_height;
    third_person_shoulder = saved.third_person_shoulder;
    third_person_smooth = saved.third_person_smooth;
    SetThirdPersonEnabled(saved.load_on_start ? saved.third_person_enabled : false);
    if (!saved.log_path.empty()) {
      strncpy_s(log_path_buf, saved.log_path.c_str(), sizeof(log_path_buf) - 1);
      MonoSetLogPath(saved.log_path);
    }
    if (saved.load_on_start) {
      auto_refresh = saved.auto_refresh;
      auto_refresh_enemies = saved.auto_refresh_enemies;
      g_item_esp_enabled = saved.item_esp;
      g_enemy_esp_enabled = saved.enemy_esp;
      g_native_highlight_active = saved.native_highlight;
      no_fall_enabled = saved.no_fall;
      speed_mult = saved.speed_mult;
    }
    // Deprecated: auto reset on stage/scene change causes user settings to be lost.
    saved.reset_each_round = false;
    settings_loaded = true;
  }
  if (mono_ready) {
    if (!runtime_gate_ok || now - last_runtime_gate_update > 120) {
      runtime_gate_ok = MonoGetSessionRuntimeGate(runtime_gate);
      last_runtime_gate_update = now;
    }
    session_master_transitioning =
      !(runtime_gate_ok && runtime_gate.ok && !runtime_gate.transitioning);
    if (g_session_master_patch_active) {
      MonoSetSessionMaster(false);
    }
  }
  else {
    session_master_transitioning = true;
    runtime_gate_ok = false;
    runtime_gate = SessionRuntimeGate{};
  }
  const bool allow_read_player =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowReadPlayer) != 0;
  const bool allow_scan_items =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowScanItems) != 0;
  const bool allow_scan_enemies =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowScanEnemies) != 0;
  const bool allow_mutate_player =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowMutatePlayer) != 0;
  const bool allow_mutate_economy =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowMutateEconomy) != 0;
  const bool allow_mutate_round =
    runtime_gate_ok && (runtime_gate.allow_mask & kSessionAllowMutateRound) != 0;
  if (scan_pause_pending) {
    if (allow_scan_items || allow_scan_enemies) {
      ++scan_pause_stable_samples;
      if (scan_pause_stable_samples >= 2) {
        scan_pause_pending = false;
        scan_pause_stable_samples = 0;
      }
    }
    else {
      scan_pause_stable_samples = 0;
    }
  }
  std::string gate_reason_text;
  if (!mono_ready) {
    gate_reason_text = "Mono未就绪";
  } else if (!runtime_gate_ok) {
    gate_reason_text = "会话门控读取失败";
  } else if (!runtime_gate.ok) {
    gate_reason_text = runtime_gate.reason.empty()
      ? "会话门控未就绪"
      : DescribeGateReason(runtime_gate.reason);
  } else {
    gate_reason_text = runtime_gate.reason.empty()
      ? (runtime_gate.transitioning ? "会话切换中" : "稳定")
      : DescribeGateReason(runtime_gate.reason);
  }

  if (session_master_transitioning) {
    last_ok = false;
    has_currency = false;
    inputs_synced = false;
    g_cached_items.clear();
    g_cached_weapons.clear();
    g_cached_enemies.clear();
    weapon_identity_cache.clear();
    g_cached_mats_valid = false;
    weapon_selected_index = -1;
    weapon_selected_object = nullptr;
    current_weapon_object = nullptr;
    weapon_cache_dirty = true;
    item_snapshot_ready = false;
    item_snapshot_hash1 = 0;
    item_snapshot_hash2 = 0;
    item_snapshot_count = 0;
  }
  const uint64_t edit_cooldown_ms = 800;
  const bool safe_to_refresh = !user_editing && (now - last_user_edit > edit_cooldown_ms);
  if (MonoIsShuttingDown()) {
    return;
  }
  if (mono_ready && auto_refresh && safe_to_refresh && now - last_update > 500) {
    if (!session_master_transitioning && allow_read_player) {
      SetCrashStage("RenderOverlay:MonoGetLocalPlayer");
      last_ok = MonoGetLocalPlayer(last_info);
      if (last_ok) {
        SetCrashStage("RenderOverlay:MonoGetLocalPlayerState");
        MonoGetLocalPlayerState(last_state);
        SetCrashStage("RenderOverlay:MonoGetRunCurrency");
        has_currency = MonoGetRunCurrency(current_currency);
        SetCrashStage("RenderOverlay:MonoApplyPendingCartValue");
        MonoApplyPendingCartValue();
        inputs_synced = false;
      } else {
        has_currency = false;
      }
    } else {
      last_ok = false;
      has_currency = false;
    }
    last_update = now;
  }

  if (mono_ready && allow_read_player && now - last_master_state_update > 500) {
    is_real_master = MonoIsRealMasterClient();
    last_master_state_update = now;
  } else if (!mono_ready || !allow_read_player) {
    is_real_master = false;
  }

  // Round/haul state refresh (关卡收集阶段)
  if (mono_ready && allow_read_player && safe_to_refresh && now - last_round_update > 500) {
    RoundState rs{};
    RoundProgressState rps{};
    if (MonoGetRoundState(rs) && rs.ok) {
      has_round_state = true;
      cached_round_state = rs;
      // 同步输入框（如果当前没有正在编辑）
      if (!user_editing) {
        if (rs.current >= 0) round_current_edit = rs.current;
        if (rs.goal >= 0) round_goal_edit = rs.goal;
      }
    }
    else {
      has_round_state = false;
    }
    if (MonoGetRoundProgress(rps) && rps.ok) {
      has_round_progress = true;
      cached_round_progress = rps;
      if (!user_editing) {
        round_progress_completed_edit = rps.completed;
        round_progress_total_edit = rps.total;
        if (rps.stage >= 0) round_stage_edit = rps.stage;
        round_progress_all_completed_edit = rps.all_completed;
      }
    }
    else {
      has_round_progress = false;
    }
    last_round_update = now;
  } else if (!mono_ready || !allow_read_player) {
    has_round_state = false;
    has_round_progress = false;
  }

  if (mono_ready && safe_to_refresh && now - last_level_info_update > 500) {
    RunLevelInfo level_info{};
    if (MonoGetRunLevelInfo(level_info) && level_info.ok) {
      has_level_info = true;
      cached_level_info = level_info;
    }
    else {
      has_level_info = false;
      cached_level_info = RunLevelInfo{};
    }
    last_level_info_update = now;
  }

  auto build_item_snapshot = [&](const std::vector<PlayerState>& items,
                                 uint64_t& out_h1,
                                 uint64_t& out_h2,
                                 size_t& out_count) {
    std::vector<uintptr_t> objects;
    objects.reserve(items.size());
    for (const auto& st : items) {
      if (st.has_object && st.object) {
        objects.push_back(reinterpret_cast<uintptr_t>(st.object));
      }
    }
    std::sort(objects.begin(), objects.end());
    out_count = objects.size();
    uint64_t h1 = 1469598103934665603ull;
    uint64_t h2 = 1099511628211ull;
    for (uintptr_t p : objects) {
      h1 ^= static_cast<uint64_t>(p) + 0x9E3779B97F4A7C15ull + (h1 << 6) + (h1 >> 2);
      h1 *= 1099511628211ull;
      h2 += (static_cast<uint64_t>(p) * 11400714819323198485ull);
      h2 ^= (h2 >> 29);
    }
    out_h1 = h1;
    out_h2 = h2;
  };
  auto refresh_items = [&](bool manual_refresh = false) {
    if (!mono_ready) return;
    if (session_master_transitioning) return;
    if (!allow_scan_items) return;
    if (scan_pause_pending) return;
    if (g_items_disabled && !manual_refresh) return;
    SetCrashStage("RenderOverlay:MonoGetLocalPlayer(refresh_items)");
    last_ok = MonoGetLocalPlayer(last_info);
    if (!last_ok) return;
    SetCrashStage("RenderOverlay:MonoGetLocalPlayerState(refresh_items)");
    if (!MonoGetLocalPlayerState(last_state) || !last_state.has_position) {
      last_ok = false;
      return;
    }
    last_update = now;
    SetCrashStage("RenderOverlay:MonoListItems");
    std::vector<PlayerState> new_items;
    const bool listed = manual_refresh
      ? MonoManualRefreshItems(new_items)
      : MonoListItemsSafe(new_items);
    if (!listed) {
      return;
    }
    g_cached_items.swap(new_items);
    uint64_t new_h1 = 0;
    uint64_t new_h2 = 0;
    size_t new_count = 0;
    build_item_snapshot(g_cached_items, new_h1, new_h2, new_count);
    const bool items_changed =
      !item_snapshot_ready ||
      new_count != item_snapshot_count ||
      new_h1 != item_snapshot_hash1 ||
      new_h2 != item_snapshot_hash2;
    item_snapshot_hash1 = new_h1;
    item_snapshot_hash2 = new_h2;
    item_snapshot_count = new_count;
    item_snapshot_ready = true;
    if (items_changed) {
      weapon_cache_dirty = true;
    }
  };
  auto refresh_enemies = [&]() {
    if (!mono_ready) return;
    if (session_master_transitioning) return;
    if (!allow_scan_enemies) return;
    if (scan_pause_pending) return;
    if (g_enemy_esp_disabled) return;
    SetCrashStage("RenderOverlay:MonoGetLocalPlayer(refresh_enemies)");
    last_ok = MonoGetLocalPlayer(last_info);
    if (!last_ok) return;
    SetCrashStage("RenderOverlay:MonoGetLocalPlayerState(refresh_enemies)");
    if (!MonoGetLocalPlayerState(last_state) || !last_state.has_position) {
      last_ok = false;
      return;
    }
    last_update = now;
    SetCrashStage("RenderOverlay:MonoListEnemies");
    std::vector<PlayerState> next_enemies;
    if (MonoListEnemiesSafe(next_enemies)) {
      g_cached_enemies.swap(next_enemies);
    }
    last_enemies_update = now;
  };
  auto pause_scans_for_mutation = [&]() {
    scan_pause_pending = true;
    scan_pause_stable_samples = 0;
  };
  auto normalize_squad_selection = [&]() {
    if (squad_states.empty()) {
      squad_selected_index = -1;
      squad_selected_object = nullptr;
      return;
    }

    if (squad_selected_object) {
      for (size_t i = 0; i < squad_states.size(); ++i) {
        if (squad_states[i].has_object && squad_states[i].object == squad_selected_object) {
          squad_selected_index = static_cast<int>(i);
          return;
        }
      }
    }

    if (squad_selected_index >= 0 &&
      squad_selected_index < static_cast<int>(squad_states.size())) {
      if (squad_states[squad_selected_index].has_object) {
        squad_selected_object = squad_states[squad_selected_index].object;
      }
      return;
    }

    squad_selected_index = 0;
    squad_selected_object = squad_states[0].has_object ? squad_states[0].object : nullptr;
  };
  auto refresh_squad = [&]() {
    if (!mono_ready) return;
    if (!allow_read_player) return;
    MonoListPlayers(squad_states, include_local_squad);
    normalize_squad_selection();
    last_squad_update = now;
  };
  auto normalize_weapon_selection = [&]() {
    if (g_cached_weapons.empty()) {
      weapon_selected_index = -1;
      weapon_selected_object = nullptr;
      return;
    }
    if (weapon_selected_object) {
      for (size_t i = 0; i < g_cached_weapons.size(); ++i) {
        if (g_cached_weapons[i].has_object && g_cached_weapons[i].object == weapon_selected_object) {
          weapon_selected_index = static_cast<int>(i);
          return;
        }
      }
    }
    if (weapon_selected_index >= 0 &&
        weapon_selected_index < static_cast<int>(g_cached_weapons.size())) {
      weapon_selected_object = g_cached_weapons[weapon_selected_index].has_object
        ? g_cached_weapons[weapon_selected_index].object
        : nullptr;
      return;
    }
    weapon_selected_index = -1;
    weapon_selected_object = nullptr;
    for (size_t i = 0; i < g_cached_weapons.size(); ++i) {
      if (g_cached_weapons[i].has_object) {
        weapon_selected_index = static_cast<int>(i);
        weapon_selected_object = g_cached_weapons[i].object;
        break;
      }
    }
  };
  auto rebuild_weapon_cache = [&]() {
    g_cached_weapons.clear();
    std::unordered_map<void*, bool> weapon_lookup;
    std::unordered_map<void*, bool> present_lookup;
    for (const auto& st : g_cached_items) {
      if (!st.has_object || !st.object) continue;
      present_lookup[st.object] = true;
      void* weapon_obj = nullptr;
      auto id_it = weapon_identity_cache.find(st.object);
      if (id_it != weapon_identity_cache.end()) {
        weapon_obj = id_it->second;
      } else {
        void* resolved = nullptr;
        if (MonoResolveBulletWeaponObject(st.object, resolved) && resolved) {
          weapon_obj = resolved;
        }
        weapon_identity_cache[st.object] = weapon_obj;
      }
      if (!weapon_obj) continue;
      if (weapon_lookup.find(weapon_obj) != weapon_lookup.end()) continue;
      weapon_lookup[weapon_obj] = true;
      PlayerState weapon_state = st;
      weapon_state.object = weapon_obj;
      weapon_state.has_object = true;
      g_cached_weapons.push_back(weapon_state);
    }
    for (auto it = weapon_identity_cache.begin(); it != weapon_identity_cache.end();) {
      if (present_lookup.find(it->first) == present_lookup.end()) {
        it = weapon_identity_cache.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = weapon_mod_configs.begin(); it != weapon_mod_configs.end();) {
      if (weapon_lookup.find(it->first) == weapon_lookup.end()) {
        it = weapon_mod_configs.erase(it);
      } else {
        ++it;
      }
    }
    normalize_weapon_selection();
  };
  auto poll_current_weapon = [&]() {
    void* new_weapon_object = nullptr;
    if (mono_ready && !session_master_transitioning && allow_scan_items) {
      MonoGetLocalHeldBulletWeaponObject(new_weapon_object);
    }
    if (new_weapon_object != current_weapon_object) {
      current_weapon_object = new_weapon_object;
      weapon_cache_dirty = true;
    }
  };
  auto apply_weapon_mods = [&]() {
    std::vector<WeaponModEntry> entries;
    entries.reserve(weapon_mod_configs.size());
    std::unordered_map<void*, bool> scanned_lookup;
    for (const auto& st : g_cached_weapons) {
      if (st.has_object && st.object) {
        scanned_lookup[st.object] = true;
      }
    }
    for (const auto& kv : weapon_mod_configs) {
      if (!kv.first) continue;
      if (scanned_lookup.find(kv.first) == scanned_lookup.end()) continue;
      const WeaponUiConfig& cfg = kv.second;
      if (!cfg.enabled) continue;
      if (!cfg.rapid_fire && !cfg.magic_bullet && !cfg.infinite_ammo && !cfg.no_recoil) continue;
      WeaponModEntry entry{};
      entry.scanned_object = kv.first;
      entry.enabled = true;
      entry.rapid_fire = cfg.rapid_fire;
      entry.magic_bullet = cfg.magic_bullet;
      entry.infinite_ammo = cfg.infinite_ammo;
      entry.no_recoil = cfg.no_recoil;
      entry.rapid_fire_cooldown = cfg.rapid_fire_cooldown;
      entries.push_back(entry);
    }
    int applied_count = 0;
    int magic_active_count = 0;
    weapon_mod_last_ok = MonoApplyWeaponMods(entries, applied_count, magic_active_count);
    weapon_mod_applied_count = applied_count;
    weapon_mod_magic_active_count = magic_active_count;
  };
  auto get_weapon_name = [&](void* weapon_obj) -> const char* {
    if (!weapon_obj) return "<none>";
    for (const auto& st : g_cached_weapons) {
      if (st.has_object && st.object == weapon_obj) {
        return st.has_name ? st.name.c_str() : "<weapon>";
      }
    }
    return "<unknown>";
  };
  auto refresh_logs = [&]() {
    MonoGetLogs(log_lines, debug_logs);
    last_log_update = now;
  };
  auto refresh_matrices = [&]() {
    if (!mono_ready) return;
    if (!allow_read_player) return;
    if (!last_ok || !last_state.has_position) return;
    SetCrashStage("RenderOverlay:MonoGetCameraMatrices");
    Matrix4x4 v{}, p{};
    if (MonoGetCameraMatrices(v, p)) {
      g_cached_view = v;
      g_cached_proj = p;
      g_cached_mats_valid = true;
      g_last_matrix_update = now;
    }
  };
  // 物品列表不再自动巡检；仅手动刷新 + 会话切换重置
  if (mono_ready && auto_refresh_enemies && now - last_enemies_update > 1000) {
    refresh_enemies();
  }
  if (mono_ready && safe_to_refresh && now - last_squad_update > 500) {
    refresh_squad();
  }
  if (mono_ready && allow_read_player && now - g_last_matrix_update > 33) {
    refresh_matrices();
  }
  if (mono_ready && !session_master_transitioning) {
    poll_current_weapon();
  } else if (!mono_ready || session_master_transitioning) {
    if (current_weapon_object != nullptr) {
      current_weapon_object = nullptr;
      weapon_cache_dirty = true;
    }
  }
  if (weapon_cache_dirty) {
    rebuild_weapon_cache();
    weapon_cache_dirty = false;
  }
  normalize_weapon_selection();
  if (mono_ready && !session_master_transitioning) {
    apply_weapon_mods();
  } else {
    MonoResetWeaponMods();
    weapon_mod_last_ok = false;
    weapon_mod_applied_count = 0;
    weapon_mod_magic_active_count = 0;
  }
  const char* current_weapon_name = get_weapon_name(current_weapon_object);

  if (mono_ready && allow_mutate_round) {
    if (round_lock_enabled) {
      // Respect explicit zero values from UI; only fallback when user sets negative.
      int target_goal = round_goal_edit;
      if (target_goal < 0 && has_round_state && cached_round_state.goal > 0) {
        target_goal = cached_round_state.goal;
      }
      int target_cur = round_current_edit;
      if (target_cur < 0) {
        target_cur = (has_round_state && cached_round_state.current > 0)
          ? cached_round_state.current
          : ((target_goal > 0) ? target_goal : 0);
      }
      // Lock mode should be conservative by default: do not exceed goal unless user edits explicitly.
      if (target_goal > 0 && target_cur > target_goal) {
        target_cur = target_goal;
      }
      MonoSetRoundHaulOverride(true, target_cur, target_goal);
    }
    else {
      MonoSetRoundHaulOverride(false, 0, -1);
    }
  } else if (mono_ready && round_lock_enabled) {
    // Changing scene or restarting can leave stale lock values that auto-complete next round.
    round_lock_enabled = false;
    MonoSetRoundHaulOverride(false, 0, -1);
  }

  auto native_highlight = [&](uint64_t ts) -> bool {
    SetCrashStage("RenderOverlay:NativeHighlight");
    if (ts - last_highlight_tick > 900) {
      int count = 0;
      SetCrashStage("RenderOverlay:MonoTriggerValuableDiscover");
      if (MonoTriggerValuableDiscoverSafe(native_highlight_state, native_highlight_limit, count)) {
        last_highlight_count = count;
      }
      last_highlight_tick = ts;
    }
    if (ts - last_persist_tick > 200) {
      SetCrashStage("RenderOverlay:MonoApplyValuableDiscoverPersistence");
      int count = 0;
      MonoApplyValuableDiscoverPersistenceSafe(true, 0.0f, count);
      last_persist_tick = ts;
    }
    return true;
  };

  // Native in-game highlight (ValuableDiscover) with SEH guard
  if (mono_ready && allow_scan_items &&
    g_esp_enabled && g_native_highlight_active &&
    MonoValueFieldsResolved() && MonoNativeHighlightAvailable()) {
    native_highlight(now);
  }

  // Sync inputs once after we get a fresh state, but don't stomp user edits every frame.
  if (last_ok && !inputs_synced && safe_to_refresh) {
    if (last_state.has_position && !locks.position) {
      edits.pos[0] = last_state.x;
      edits.pos[1] = last_state.y;
      edits.pos[2] = last_state.z;
    }
    if (last_state.has_health && !locks.health) {
      edits.health = last_state.health;
      edits.max_health = last_state.max_health;
    }
    if (last_state.has_energy && !locks.stamina) {
      edits.stamina = last_state.energy;
      edits.max_stamina = last_state.max_energy;
    }
    inputs_synced = true;
  }

  if (menu_visible) {
    if (menu_open && !last_menu_open) {
      ImGui::SetNextWindowFocus();
    }
    ApplyOverlayStyleOnce();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp) {
      ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("RepoDLL", menu_open, win_flags)) {
      ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.48f, 1.0f), "RepoDLL 覆盖层");
      ImGui::SameLine();
      ImGui::Text("Mono: %s | 本地玩家: %s", mono_ready ? "就绪" : "未就绪",
        last_ok ? "找到" : "未找到");
      ImGui::Text("来源: %s | 指针: %p | isLocal: %s",
        last_info.via_player_list ? "GameDirector.PlayerList" : "SemiFunc.PlayerAvatarLocal",
        last_info.object, last_info.is_local ? "true" : "false");
      ImGui::Spacing();
      if (ImGui::BeginTabBar("repo_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        // 玩家页
        if (ImGui::BeginTabItem("玩家")) {
          ImGui::BeginGroup();
          ImGui::Checkbox("自动刷新", &auto_refresh);
          ImGui::SameLine();
          ImGui::BeginDisabled(!allow_read_player);
          if (ImGui::Button("刷新")) {
            last_ok = MonoGetLocalPlayer(last_info);
            if (last_ok) {
              MonoGetLocalPlayerState(last_state);
            }
            inputs_synced = false;
            last_update = now;
          }
          ImGui::EndDisabled();
          ImGui::EndGroup();
          if (!allow_read_player) {
            ImGui::TextDisabled("玩家读取受限: %s", gate_reason_text.c_str());
          }
          if (!allow_mutate_player) {
            ImGui::TextDisabled("玩家修改受限: %s", gate_reason_text.c_str());
          }

          SectionLabel("实时状态");
          ImGui::BeginGroup();
          ImGui::Text("位置: %.3f, %.3f, %.3f%s", last_state.x, last_state.y, last_state.z,
            last_state.has_position ? "" : " (无数据)");
          ImGui::Text("生命: %d / %d%s", last_state.health, last_state.max_health,
            last_state.has_health ? "" : " (无数据)");
          ImGui::Text("体力: %d / %d%s", last_state.energy, last_state.max_energy,
            last_state.has_energy ? "" : " (无数据)");
          ImGui::EndGroup();

          SectionLabel("编辑 / 应用");
          const bool can_local_position = allow_read_player && !session_master_transitioning;
          const bool can_local_mutate = allow_mutate_player;
          if (ImGui::BeginTable("edit_table", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.0f);

            // 位置
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("位置");
            ImGui::TableSetColumnIndex(1);
            bool disable_pos = locks.position || !last_state.has_position || !can_local_position;
            ImGui::Checkbox("锁定位置", &locks.position);
            ImGui::BeginDisabled(disable_pos);
            if (ImGui::InputFloat3("##position", edits.pos, "%.3f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoSetLocalPlayerPosition(edits.pos[0], edits.pos[1], edits.pos[2]);
            }
            ImGui::EndDisabled();

            // 生命
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("生命 / 最大生命");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!can_local_mutate);
            bool hp_apply = false;
            if (ImGui::InputInt("##health", &edits.health, 0, 0,
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              hp_apply = true;
            }
            if (ImGui::InputInt("##maxhealth", &edits.max_health, 0, 0,
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              hp_apply = true;
            }
            if (hp_apply) {
              MonoSetLocalPlayerHealth(edits.health, edits.max_health);
            }
            ImGui::SameLine();
            ImGui::Checkbox("锁定生命", &lock_health);
            ImGui::EndDisabled();

            // 体力
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("体力 / 最大体力");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!can_local_mutate);
            bool sta_apply = false;
            if (ImGui::InputFloat("##stamina", &edits.stamina, 0.1f, 1.0f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              sta_apply = true;
            }
            if (ImGui::InputFloat("##maxstamina", &edits.max_stamina, 0.1f, 1.0f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              sta_apply = true;
            }
            if (sta_apply) {
              MonoSetLocalPlayerEnergy(edits.stamina, edits.max_stamina);
            }
            ImGui::SameLine();
            ImGui::Checkbox("锁定体力", &lock_stamina);
            ImGui::EndDisabled();

            ImGui::EndTable();
          }
          if (!can_local_position) {
            ImGui::TextDisabled("本地坐标传送受限: %s", gate_reason_text.c_str());
          }

          SectionLabel("功能修改");
          ImGui::BeginDisabled(!allow_mutate_player);
          if (ImGui::BeginTable("mods_table", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.0f);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("防摔倒/击倒");
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("防摔倒", &no_fall_enabled);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("速度");
            ImGui::TableSetColumnIndex(1);
            bool speed_commit =
              ImGui::InputFloat("##speed", &speed_mult, 0.5f, 1.0f, "%.2f",
                ImGuiInputTextFlags_EnterReturnsTrue);
            speed_commit |= ImGui::IsItemDeactivatedAfterEdit();
            if (speed_commit) {
              MonoOverrideSpeed(speed_mult, 999999.0f);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("跳跃");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Checkbox("无限跳跃", &infinite_jump_enabled)) {
              if (infinite_jump_enabled) {
                if (extra_jump_count <= 0) extra_jump_count = 9999;
                MonoSetJumpExtraDirect(extra_jump_count);
              }
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("冷却");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::InputFloat("##jumpcd", &jump_cooldown, 0.1f, 0.5f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoOverrideJumpCooldown(jump_cooldown);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("抓取");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::InputInt("##grabforce", &grab_strength, 0, 0,
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoSetGrabStrength(grab_strength, grab_strength);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("抓取范围");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::InputFloat("##grabrangefield", &grab_range_field, 0.1f, 0.5f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoSetGrabRange(grab_range_field);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("抓取强度");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::InputFloat("##grabstrengthfield", &grab_strength_field, 0.1f, 0.5f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoSetGrabStrengthField(grab_strength_field);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("无敌");
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("无敌模式", &god_mode_enabled);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("跳跃力");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::InputFloat("##jumpforce", &jump_force, 0.5f, 1.0f, "%.2f",
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              MonoSetJumpForce(jump_force);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("其他");
            ImGui::TableSetColumnIndex(1);

            ImGui::EndTable();
          }
          ImGui::EndDisabled();
          ImGui::EndTabItem();
        }

        // 金钱管理页
        if (ImGui::BeginTabItem("金钱管理大师")) {
          SectionLabel("金钱/总价值联动");
          if (!allow_mutate_economy) {
            ImGui::TextDisabled("经济修改受限: %s", gate_reason_text.c_str());
          }
          if (!allow_mutate_round) {
            ImGui::TextDisabled("关卡收集修改受限: %s", gate_reason_text.c_str());
          }
          if (has_currency) {
            ImGui::TextDisabled("当前总金库: %d", current_currency);
          }
          else {
            ImGui::TextDisabled("当前总金库: 无数据");
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!allow_read_player);
          if (ImGui::Button("读取##econ_refresh_currency")) {
            has_currency = MonoGetRunCurrency(current_currency);
          }
          ImGui::EndDisabled();
          ImGui::BeginDisabled(!allow_mutate_economy);
          ImGui::InputInt("目标金额##econ_currency", &currency_edit, 0, 0);
          if (ImGui::Button("应用修改器数值##econ_apply_currency")) {
            pause_scans_for_mutation();
            MonoSetRunCurrency(currency_edit);
            has_currency = MonoGetRunCurrency(current_currency);
          }
          ImGui::SameLine();
          if (ImGui::Button("同步收集器(危险)##econ_apply_currency_cart")) {
            pause_scans_for_mutation();
            MonoSetCartValueSafe(currency_edit);
          }
          ImGui::EndDisabled();
          ImGui::TextDisabled("默认只改金钱；右侧按钮才会改收集器，可能导致直接通关。");

          SectionLabel("关卡状态");
          const char* current_level_name = cached_level_info.current_level.empty()
            ? "未知"
            : cached_level_info.current_level.c_str();
          const char* previous_level_name = cached_level_info.previous_level.empty()
            ? "未知"
            : cached_level_info.previous_level.c_str();
          if (has_level_info) {
            ImGui::TextDisabled("当前地图: %s", current_level_name);
            ImGui::TextDisabled("上一地图: %s", previous_level_name);
            ImGui::TextDisabled("状态: %s",
              cached_level_info.transitioning ? "切换中/加载中" : "稳定");
          }
          else if (session_master_transitioning) {
            ImGui::TextDisabled("状态: 切换中/加载中 (等待关卡初始化)");
          }
          else {
            ImGui::TextDisabled("状态: 无法读取关卡信息");
          }

          SectionLabel("关卡收集（局内）");
          int display_stage = cached_round_state.stage;
          if (display_stage < 0 && has_round_progress && cached_round_progress.completed >= 0) {
            display_stage = cached_round_progress.completed;
          }
          if (has_round_state) {
            ImGui::TextDisabled("当前: %d / 目标: %d / Max: %d / 阶段: %d",
              cached_round_state.current,
              cached_round_state.goal,
              cached_round_state.current_max,
              display_stage);
          }
          else {
            if (session_master_transitioning) {
              ImGui::TextDisabled("RoundDirector 暂无数据：关卡切换中/加载中");
            }
            else {
              ImGui::TextDisabled("RoundDirector 暂无数据：当前场景未初始化");
            }
          }
          ImGui::BeginDisabled(!allow_mutate_round);
          ImGui::InputInt("收集器当前##econ_haul_cur", &round_current_edit, 0, 0);
          ImGui::InputInt("收集器目标##econ_haul_goal", &round_goal_edit, 0, 0);
          if (ImGui::Button("当前=目标##econ_copy_goal")) {
            round_current_edit = round_goal_edit;
          }
          ImGui::SameLine();
          if (ImGui::Button("应用收集器数值##econ_apply_haul")) {
            pause_scans_for_mutation();
            MonoSetRoundStateSafe(round_current_edit, round_goal_edit, round_current_edit);
          }
          ImGui::Checkbox("锁定收集器(超级补丁大法)##econ_round_lock", &round_lock_enabled);
          ImGui::EndDisabled();
          ImGui::TextDisabled("说明: 应用收集器数值仅改 haul 字段，不再改进度完成字段。");
          ImGui::TextDisabled("说明: 开启后会拦截 RoundDirector.Update 的重算；切图/重启会自动关闭。");

          SectionLabel("关卡切换 / 进度");
          int display_progress_stage = cached_round_progress.stage;
          if (display_progress_stage < 0 && cached_round_progress.completed >= 0) {
            display_progress_stage = cached_round_progress.completed;
          }
          if (has_round_progress) {
            ImGui::TextDisabled("进度: %d / %d | allCompleted=%s | stage=%d",
              cached_round_progress.completed,
              cached_round_progress.total,
              cached_round_progress.all_completed ? "true" : "false",
              display_progress_stage);
          }
          else {
            if (session_master_transitioning) {
              ImGui::TextDisabled("进度字段暂无数据：关卡切换中/加载中");
            }
            else {
              ImGui::TextDisabled("进度字段暂无数据：当前场景未初始化");
            }
          }
          ImGui::BeginDisabled(!allow_mutate_round);
          ImGui::InputInt("已完成##econ_progress_done", &round_progress_completed_edit, 0, 0);
          ImGui::InputInt("总节点##econ_progress_total", &round_progress_total_edit, 0, 0);
          ImGui::InputInt("阶段##econ_progress_stage", &round_stage_edit, 0, 0);
          ImGui::Checkbox("全部完成##econ_progress_all", &round_progress_all_completed_edit);
          if (ImGui::Button("应用进度##econ_apply_progress")) {
            pause_scans_for_mutation();
            MonoSetRoundProgressSafe(
              round_progress_completed_edit,
              round_progress_total_edit,
              round_stage_edit,
              round_progress_all_completed_edit ? 1 : 0);
          }
          ImGui::SameLine();
          if (ImGui::Button("下一节点##econ_next_progress")) {
            pause_scans_for_mutation();
            ++round_progress_completed_edit;
            MonoSetRoundProgressSafe(
              round_progress_completed_edit,
              round_progress_total_edit,
              round_stage_edit,
              -1);
          }
          ImGui::EndDisabled();
          ImGui::BeginDisabled(!allow_read_player);
          if (ImGui::Button("读取全部状态##econ_refresh_all")) {
            has_currency = MonoGetRunCurrency(current_currency);
            RoundState rs{};
            if (MonoGetRoundState(rs) && rs.ok) {
              has_round_state = true;
              cached_round_state = rs;
            }
            RoundProgressState rps{};
            if (MonoGetRoundProgress(rps) && rps.ok) {
              has_round_progress = true;
              cached_round_progress = rps;
            }
          }
          ImGui::EndDisabled();
          ImGui::EndTabItem();
        }

        // 物品 / ESP 页
        if (ImGui::BeginTabItem("物品/ESP")) {
          ImGui::BeginGroup();
          if (ImGui::Checkbox("物品ESP", &g_item_esp_enabled)) {
            g_esp_enabled = g_item_esp_enabled || g_enemy_esp_enabled || g_native_highlight_active;
          }
          ImGui::SameLine();
          if (ImGui::Checkbox("原生高亮", &g_native_highlight_active)) {
            g_esp_enabled = g_item_esp_enabled || g_enemy_esp_enabled || g_native_highlight_active;
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!allow_scan_items);
          if (ImGui::Button("刷新物品")) refresh_items(true);
          ImGui::EndDisabled();
          ImGui::EndGroup();
          if (!allow_scan_items) {
            ImGui::TextDisabled("物品扫描受限: %s", gate_reason_text.c_str());
          }
          ImGui::SameLine();
          ImGui::TextDisabled("共 %d", static_cast<int>(g_cached_items.size()));
          if (MonoItemsDisabled()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1.0f), "物品扫描已自动关闭(崩溃保护)");
          }
          ImGui::BeginDisabled(!allow_scan_items);
          if (ImGui::Button("安全刷新一次")) {
            refresh_items(true);
          }
          ImGui::SameLine();
          if (ImGui::Button("重置物品禁用")) {
            MonoResetItemsDisabled();
          }
          ImGui::EndDisabled();
          ImGui::SliderInt("物品ESP上限", &g_item_esp_cap, 0, 1024);
          ImGui::SliderInt("敌人ESP上限", &g_enemy_esp_cap, 0, 512);

          ImGui::Spacing();
          ImGui::BeginGroup();
          int total_items = static_cast<int>(g_cached_items.size());
          ImGui::TextDisabled("原生高亮状态");
          ImGui::SliderInt("模式##native_state", &native_highlight_state, 0, 2);
          ImGui::SliderInt("最大数量##native_limit", &native_highlight_limit, 20, 512);
          ImGui::SameLine();
          ImGui::TextDisabled("当前检测: %d", total_items);
          ImGui::SameLine();
          ImGui::TextDisabled("最近触发: %d", last_highlight_count);
          ImGui::EndGroup();

          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;
          ImVec2 table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 8.0f);
          if (ImGui::BeginTable("items_table_view", 6, flags, table_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("物品昵称", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("价值($)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("距离", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("坐标", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();

            auto cat_name = [](PlayerState::Category c) {
              switch (c) {
              case PlayerState::Category::kValuable: return "Valuable";
              case PlayerState::Category::kPhysGrab: return "PhysGrab";
              case PlayerState::Category::kVolume: return "ItemVolume";
              case PlayerState::Category::kCollider: return "Collider";
              case PlayerState::Category::kEnemy: return "Enemy";
              default: return "Unknown";
              }
            };

            for (size_t i = 0; i < g_cached_items.size(); ++i) {
              const auto& st = g_cached_items[i];
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(st.has_name ? st.name.c_str() : "<unknown>");

              ImGui::TableSetColumnIndex(1);
              ImVec4 col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
              if (st.category == PlayerState::Category::kValuable) col = ImVec4(0.90f, 0.78f, 0.15f, 1.0f);
              else if (st.category == PlayerState::Category::kPhysGrab) col = ImVec4(0.30f, 0.82f, 0.52f, 1.0f);
              else if (st.category == PlayerState::Category::kEnemy) col = ImVec4(0.90f, 0.32f, 0.32f, 1.0f);
              ImGui::TextColored(col, "%s", cat_name(st.category));

              ImGui::TableSetColumnIndex(2);
              if (st.has_value) ImGui::Text("$%d", st.value);
              else if (st.has_item_type) ImGui::TextDisabled("type %d", st.item_type);
              else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(3);
              if (last_state.has_position && st.has_position) {
                float dx = st.x - last_state.x;
                float dy = st.y - last_state.y;
                float dz = st.z - last_state.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                ImGui::Text("%.1fm", dist);
              }
              else {
                ImGui::TextDisabled("-");
              }

              ImGui::TableSetColumnIndex(4);
              if (st.has_layer) ImGui::Text("%d", st.layer); else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(5);
              if (st.has_position) {
                ImGui::Text("%.2f, %.2f, %.2f", st.x, st.y, st.z);
              }
              else {
                ImGui::TextDisabled("无坐标");
              }
            }
            ImGui::EndTable();
          }
          ImGui::EndTabItem();
        }

        // 武器改造页
        if (ImGui::BeginTabItem("武器")) {
          ImGui::BeginDisabled(!allow_scan_items);
          if (ImGui::Button("刷新武器来源(物品扫描)")) {
            refresh_items(true);
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("已识别可发弹武器: %d", static_cast<int>(g_cached_weapons.size()));
          ImGui::TextDisabled("改造状态: %s | 已应用: %d | 穿墙激活: %d",
            weapon_mod_last_ok ? "运行中" : "未运行",
            weapon_mod_applied_count,
            weapon_mod_magic_active_count);
          ImGui::TextDisabled("当前手持武器: %s | obj=%p", current_weapon_name, current_weapon_object);
          ImGui::TextDisabled("筛选规则: 仅 ItemGun 且 bulletPrefab 有效、numberOfBullets > 0");
          ImGui::TextDisabled("重建触发: 物品对象集合变化 / 当前手持武器对象变化");

          ImGuiTableFlags weapon_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY;
          ImVec2 weapon_table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y * 0.45f);
          if (ImGui::BeginTable("weapon_table_view", 5, weapon_flags, weapon_table_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("武器");
            ImGui::TableSetupColumn("距离");
            ImGui::TableSetupColumn("Layer");
            ImGui::TableSetupColumn("价值");
            ImGui::TableSetupColumn("坐标");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < g_cached_weapons.size(); ++i) {
              const auto& st = g_cached_weapons[i];
              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              std::string display_name = st.has_name ? st.name : "<weapon>";
              const bool selected = st.has_object && weapon_selected_object == st.object;
              ImGui::PushID(static_cast<int>(i));
              if (ImGui::Selectable(display_name.c_str(), selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                weapon_selected_index = static_cast<int>(i);
                weapon_selected_object = st.has_object ? st.object : nullptr;
              }
              ImGui::PopID();

              ImGui::TableSetColumnIndex(1);
              if (last_state.has_position && st.has_position) {
                float dx = st.x - last_state.x;
                float dy = st.y - last_state.y;
                float dz = st.z - last_state.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                ImGui::Text("%.1fm", dist);
              } else {
                ImGui::TextDisabled("-");
              }

              ImGui::TableSetColumnIndex(2);
              if (st.has_layer) ImGui::Text("%d", st.layer); else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(3);
              if (st.has_value) ImGui::Text("$%d", st.value);
              else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(4);
              if (st.has_position) {
                ImGui::Text("%.2f, %.2f, %.2f", st.x, st.y, st.z);
              } else {
                ImGui::TextDisabled("无坐标");
              }
            }
            ImGui::EndTable();
          }

          normalize_weapon_selection();
          const PlayerState* selected_weapon_state = nullptr;
          if (weapon_selected_index >= 0 &&
            weapon_selected_index < static_cast<int>(g_cached_weapons.size())) {
            selected_weapon_state = &g_cached_weapons[weapon_selected_index];
          }

          SectionLabel("枪械功能改造");
          if (selected_weapon_state && selected_weapon_state->has_object) {
            void* selected_obj = selected_weapon_state->object;
            auto cfg_it = weapon_mod_configs.find(selected_obj);
            WeaponUiConfig cfg = (cfg_it != weapon_mod_configs.end()) ? cfg_it->second : WeaponUiConfig{};
            const char* selected_name =
              selected_weapon_state->has_name ? selected_weapon_state->name.c_str() : "<weapon>";
            ImGui::Text("当前武器: %s", selected_name);
            ImGui::SameLine();
            ImGui::TextDisabled("obj=%p", selected_obj);
            bool cfg_changed = false;
            cfg_changed = ImGui::Checkbox("启用该武器改造", &cfg.enabled) || cfg_changed;
            ImGui::BeginDisabled(!cfg.enabled);
            cfg_changed = ImGui::Checkbox("速射", &cfg.rapid_fire) || cfg_changed;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(170.0f);
            cfg_changed = ImGui::SliderFloat("速射间隔(s)", &cfg.rapid_fire_cooldown, 0.001f, 0.2f, "%.3f") || cfg_changed;
            cfg_changed = ImGui::Checkbox("魔法子弹", &cfg.magic_bullet) || cfg_changed;
            ImGui::SameLine();
            cfg_changed = ImGui::Checkbox("无限子弹", &cfg.infinite_ammo) || cfg_changed;
            ImGui::SameLine();
            cfg_changed = ImGui::Checkbox("无后座力", &cfg.no_recoil) || cfg_changed;
            ImGui::EndDisabled();
            if (cfg_changed) {
              const bool should_store =
                cfg.enabled || cfg.rapid_fire || cfg.magic_bullet || cfg.infinite_ammo || cfg.no_recoil;
              if (should_store) {
                weapon_mod_configs[selected_obj] = cfg;
              }
              else {
                weapon_mod_configs.erase(selected_obj);
              }
            }
            if (ImGui::Button("移除当前武器配置")) {
              weapon_mod_configs.erase(selected_obj);
            }
            ImGui::SameLine();
            if (ImGui::Button("清空全部武器配置")) {
              weapon_mod_configs.clear();
            }
          }
          else {
            ImGui::TextDisabled("先在上方武器表里选中一把武器。");
          }

          int configured_count = 0;
          for (const auto& kv : weapon_mod_configs) {
            const WeaponUiConfig& cfg = kv.second;
            if (cfg.enabled && (cfg.rapid_fire || cfg.magic_bullet || cfg.infinite_ammo || cfg.no_recoil)) {
              ++configured_count;
            }
          }
          ImGui::TextDisabled("已启用配置武器: %d", configured_count);
          ImGui::EndTabItem();
        }

        // 队友/复活
        if (ImGui::BeginTabItem("队友")) {
          ImGui::BeginDisabled(!allow_read_player);
          if (ImGui::Checkbox("包含自己", &include_local_squad)) {
            refresh_squad();
          }
          ImGui::SameLine();
          if (ImGui::Button("刷新队友")) refresh_squad();
          ImGui::EndDisabled();
          if (!allow_read_player) {
            ImGui::TextDisabled("队友读取受限: %s", gate_reason_text.c_str());
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!(is_real_master && allow_mutate_player));
          if (ImGui::Button("复活队友")) {
            MonoReviveAllPlayers(false);
            refresh_squad();
          }
          ImGui::SameLine();
          if (ImGui::Button("全体满血(含自己)")) {
            MonoReviveAllPlayers(true);
            refresh_squad();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("队友数: %d", static_cast<int>(squad_states.size()));
          ImGui::SameLine();
          ImGui::TextDisabled("你是房主: %s | 房主可写队友: %s",
            is_real_master ? "是" : "否",
            (is_real_master && allow_mutate_player) ? "是" : "否");

          normalize_squad_selection();
          const PlayerState* selected_state = nullptr;
          if (squad_selected_index >= 0 &&
            squad_selected_index < static_cast<int>(squad_states.size())) {
            selected_state = &squad_states[squad_selected_index];
          }

          if (selected_state) {
            std::string selected_name = selected_state->has_name ? selected_state->name : "<player>";
            ImGui::Text("当前选中: %s", selected_name.c_str());
            if (selected_state->has_steam_id) {
              ImGui::SameLine();
              ImGui::TextDisabled("steamID: %s", selected_state->steam_id.c_str());
            }

            if (selected_state->has_object) {
              void* selected_obj = selected_state->object;
              bool squad_need_refresh = false;
              const bool can_manage_squad =
                allow_mutate_player && (is_real_master || selected_state->is_local);
              ImGui::SetNextItemWidth(120.0f);
              ImGui::InputInt("目标生命##squad_hp", &squad_target_health, 1, 20);
              ImGui::SameLine();
              ImGui::SetNextItemWidth(120.0f);
              ImGui::InputInt("目标最大生命##squad_maxhp", &squad_target_max_health, 1, 20);
              if (squad_target_max_health < 1) squad_target_max_health = 1;
              if (squad_target_health < 0) squad_target_health = 0;
              if (squad_target_health > squad_target_max_health) {
                squad_target_health = squad_target_max_health;
              }

              ImGui::BeginDisabled(!can_manage_squad);
              if (ImGui::Button("设置选中生命")) {
                MonoSetPlayerAvatarHealth(
                  selected_obj, squad_target_health, squad_target_max_health);
                squad_need_refresh = true;
              }
              ImGui::SameLine();
              if (ImGui::Button("选中满血")) {
                MonoSetPlayerAvatarHealth(
                  selected_obj, squad_target_max_health, squad_target_max_health);
                squad_need_refresh = true;
              }

              ImGui::InputFloat3("目标坐标##squad_pos", squad_target_pos, "%.2f");
              if (ImGui::Button("传送选中到坐标")) {
                MonoSetPlayerAvatarPosition(
                  selected_obj, squad_target_pos[0], squad_target_pos[1], squad_target_pos[2]);
                squad_need_refresh = true;
              }
              ImGui::SameLine();
              ImGui::BeginDisabled(!last_state.has_position);
              if (ImGui::Button("拉到我身边")) {
                const float target_x = last_state.x + squad_pull_offset[0];
                const float target_y = last_state.y + squad_pull_offset[1];
                const float target_z = last_state.z + squad_pull_offset[2];
                MonoSetPlayerAvatarPosition(selected_obj, target_x, target_y, target_z);
                squad_need_refresh = true;
              }
              ImGui::EndDisabled();
              ImGui::EndDisabled();
              ImGui::SetNextItemWidth(260.0f);
              ImGui::InputFloat3("拉取偏移##squad_pull_offset", squad_pull_offset, "%.2f");
              if (!can_manage_squad) {
                ImGui::TextDisabled("非房主且非本地目标时，仅可查看。");
              }
              if (squad_need_refresh) {
                refresh_squad();
              }
            }
            else {
              ImGui::TextDisabled("选中对象无效，无法改状态。");
            }
          }
          else {
            ImGui::TextDisabled("未选中队友。先点击下方列表的一行。");
          }

          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY;
          ImVec2 table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 8.0f);
          if (ImGui::BeginTable("squad_table_view", 6, flags, table_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("生命", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("距离", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("坐标", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < squad_states.size(); ++i) {
              const auto& st = squad_states[i];
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              std::string display_name = st.has_name ? st.name : "<player>";
              if (st.is_local) {
                display_name = "[本地] " + display_name;
              }
              const bool selected = squad_selected_index == static_cast<int>(i);
              ImGui::PushID(static_cast<int>(i));
              if (ImGui::Selectable(display_name.c_str(), selected)) {
                squad_selected_index = static_cast<int>(i);
                squad_selected_object = st.has_object ? st.object : nullptr;
                if (st.has_health) {
                  squad_target_health = std::max(0, st.health);
                  squad_target_max_health = std::max(1, st.max_health);
                  if (squad_target_health > squad_target_max_health) {
                    squad_target_health = squad_target_max_health;
                  }
                }
                if (st.has_position) {
                  squad_target_pos[0] = st.x;
                  squad_target_pos[1] = st.y;
                  squad_target_pos[2] = st.z;
                }
              }
              ImGui::PopID();

              ImGui::TableSetColumnIndex(1);
              if (st.has_steam_id) ImGui::TextUnformatted(st.steam_id.c_str());
              else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(2);
              if (st.has_health) ImGui::Text("%d / %d", st.health, st.max_health);
              else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(3);
              bool downed = st.has_health && st.health <= 0;
              ImVec4 col = downed ? ImVec4(0.9f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
              ImGui::TextColored(col, "%s", downed ? "倒地" : "存活");

              ImGui::TableSetColumnIndex(4);
              if (last_state.has_position && st.has_position) {
                float dx = st.x - last_state.x;
                float dy = st.y - last_state.y;
                float dz = st.z - last_state.z;
                ImGui::Text("%.1fm", std::sqrt(dx * dx + dy * dy + dz * dz));
              } else {
                ImGui::TextDisabled("-");
              }

              ImGui::TableSetColumnIndex(5);
              if (st.has_position) {
                ImGui::Text("%.2f, %.2f, %.2f", st.x, st.y, st.z);
              } else {
                ImGui::TextDisabled("无坐标");
              }
            }
            ImGui::EndTable();
          }
          ImGui::EndTabItem();
        }

        // 调试
        if (ImGui::BeginTabItem("调试")) {
          SectionLabel("刷新/开关");
          ImGui::TextDisabled("物品禁用: %s", MonoItemsDisabled() ? "是" : "否");
          ImGui::SameLine();
          if (ImGui::Button("重置物品禁用")) {
            MonoResetItemsDisabled();
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!allow_scan_items);
          if (ImGui::Button("手动刷新物品")) {
            refresh_items(true);
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          ImGui::TextDisabled("敌人禁用: %s", g_enemy_esp_disabled ? "是" : "否");
          ImGui::SameLine();
          if (ImGui::Button("重置敌人禁用")) {
            MonoResetEnemiesDisabled();
          }

          SectionLabel("日志");
          ImGui::SliderInt("行数", &log_lines, 50, 400);
          ImGui::SameLine();
          if (ImGui::Button("刷新日志")) {
            refresh_logs();
          }
          if (debug_logs.empty() && (now - last_log_update > 2000)) {
            refresh_logs();
          }
          ImGui::BeginChild("log_view", ImVec2(0, ImGui::GetContentRegionAvail().y - 8.0f), true);
          for (const auto& line : debug_logs) {
            ImGui::TextUnformatted(line.c_str());
          }
          ImGui::EndChild();

          SectionLabel("收集器杀招扫描");
          ImGui::InputText("关键词", collector_scan_kw, sizeof(collector_scan_kw));
          if (ImGui::Button("扫描方法+机器码")) {
            collector_scan_results.clear();
            MonoScanMethodsWithBytes(collector_scan_kw, collector_scan_results);
          }
          ImGui::SameLine();
          if (ImGui::Button("探测函数返回值")) {
            collector_scan_results.clear();
            MonoProbeCollectorMethods(collector_scan_kw, collector_scan_results);
          }
          ImGui::SameLine();
          if (ImGui::Button("Dump收集字段")) {
            collector_scan_results.clear();
            MonoDumpCollectorNumericFields(collector_scan_results);
          }
          ImGui::SameLine();
          if (ImGui::Button("快速关键词")) {
            collector_scan_results.clear();
            static const char* kws[] = { "haul", "extract", "currency", "collector", "tax" };
            for (const char* kw : kws) {
              std::vector<std::string> tmp;
              if (MonoScanMethodsWithBytes(kw, tmp)) {
                collector_scan_results.push_back(std::string("== keyword: ") + kw + " ==");
                collector_scan_results.insert(collector_scan_results.end(), tmp.begin(), tmp.end());
              }
            }
          }
          ImGui::InputInt("强制值", &collector_force_value, 0, 0);
          if (ImGui::Button("杀招补丁: Getter=强制值")) {
            collector_scan_results.clear();
            MonoPatchCollectorGetters(collector_force_value, collector_scan_results);
          }
          ImGui::SameLine();
          if (ImGui::Button("恢复Getter补丁")) {
            collector_scan_results.clear();
            MonoRestoreCollectorGetterPatches(collector_scan_results);
          }
          ImGui::BeginChild("collector_scan_view", ImVec2(0, 180.0f), true);
          for (const auto& line : collector_scan_results) {
            ImGui::TextUnformatted(line.c_str());
          }
          ImGui::EndChild();

          ImGui::EndTabItem();
        }

        // 敌人页
        if (ImGui::BeginTabItem("敌人")) {
          ImGui::BeginGroup();
          if (ImGui::Checkbox("敌人ESP", &g_enemy_esp_enabled)) {
            g_esp_enabled = g_item_esp_enabled || g_enemy_esp_enabled || g_native_highlight_active;
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!allow_scan_enemies);
          ImGui::Checkbox("自动刷新", &auto_refresh_enemies);
          ImGui::SameLine();
          if (ImGui::Button("刷新敌人")) refresh_enemies();
          ImGui::EndDisabled();
          ImGui::EndGroup();
          if (!allow_scan_enemies) {
            ImGui::TextDisabled("敌人扫描受限: %s", gate_reason_text.c_str());
          }
          ImGui::SameLine();
          ImGui::TextDisabled("共 %d", static_cast<int>(g_cached_enemies.size()));
          if (g_enemy_esp_disabled) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1.0f), "敌人扫描已自动关闭(崩溃保护)");
          }

          // Child 内只绘制一次表格，避免底部再出现重复控件
          ImGui::BeginChild("enemy_table_child", ImVec2(0, ImGui::GetContentRegionAvail().y - 4.0f), true);
          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY;
          if (ImGui::BeginTable("enemy_table_view", 5, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("距离", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("坐标", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();

            for (const auto& st : g_cached_enemies) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s",
                st.has_name ? st.name.c_str() : "Enemy");
              ImGui::TableSetColumnIndex(1);
              if (last_state.has_position && st.has_position) {
                float dx = st.x - last_state.x;
                float dy = st.y - last_state.y;
                float dz = st.z - last_state.z;
                ImGui::Text("%.1fm", std::sqrt(dx * dx + dy * dy + dz * dz));
              }
              else {
                ImGui::TextDisabled("-");
              }
              ImGui::TableSetColumnIndex(2);
              if (st.has_layer) ImGui::Text("%d", st.layer); else ImGui::TextDisabled("-");
              ImGui::TableSetColumnIndex(3);
              if (st.has_position) {
                ImGui::Text("%.2f, %.2f, %.2f", st.x, st.y, st.z);
              }
              else {
                ImGui::TextDisabled("无坐标");
              }
              ImGui::TableSetColumnIndex(4);
              ImGui::Text("%s", "Hostile");
            }
            ImGui::EndTable();
          }
          ImGui::EndChild();

          ImGui::EndTabItem();
        }

        // 设置页（最右）
        if (ImGui::BeginTabItem("设置", nullptr, ImGuiTabItemFlags_Trailing)) {
          SectionLabel("日志 / 存档路径");
          ImGui::InputText("日志路径", log_path_buf, sizeof(log_path_buf));
          ImGui::SameLine();
          if (ImGui::Button("应用路径")) {
            MonoSetLogPath(log_path_buf);
            saved.log_path = log_path_buf;
          }

          SectionLabel("默认开关");
          ImGui::Checkbox("启动时加载上次参数", &saved.load_on_start);
          ImGui::SameLine();
          ImGui::TextDisabled("每局重置为默认: 已停用（防止切图丢设置）");
          ImGui::Checkbox("默认绘制覆盖层", &g_esp_enabled);
          ImGui::Checkbox("默认物品ESP", &g_item_esp_enabled);
          ImGui::SameLine();
          ImGui::Checkbox("默认敌人ESP", &g_enemy_esp_enabled);
          ImGui::Checkbox("默认原生高亮", &g_native_highlight_active);
          ImGui::SameLine();
          ImGui::Checkbox("默认防摔倒", &no_fall_enabled);
          ImGui::Checkbox("默认自动刷新玩家状态", &auto_refresh);
          ImGui::SameLine();
          ImGui::Checkbox("默认自动刷新敌人", &auto_refresh_enemies);
          ImGui::InputFloat("默认速度倍率", &speed_mult, 0.1f, 0.5f, "%.2f");
          g_esp_enabled = g_item_esp_enabled || g_enemy_esp_enabled || g_native_highlight_active;

          SectionLabel("菜单热键");
          const int menu_vk = GetMenuToggleVirtualKey();
          const std::string menu_vk_name = MenuHotkeyName(menu_vk);
          ImGui::Text("打开菜单键: %s", menu_vk_name.c_str());
          ImGui::SameLine();
          if (menu_vk == VK_INSERT) {
            ImGui::TextDisabled("(默认 INS)");
          }
          if (IsMenuToggleKeyCaptureActive()) {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.28f, 1.0f), "监听中... 按任意键 (Esc 取消)");
          } else {
            if (ImGui::Button("监听按键修改")) {
              BeginMenuToggleKeyCapture();
            }
            ImGui::SameLine();
            if (ImGui::Button("恢复默认 INS")) {
              SetMenuToggleVirtualKey(VK_INSERT);
            }
          }

          SectionLabel("第三人称");
          bool third_person_enabled = GetThirdPersonEnabled();
          if (ImGui::Checkbox("启用第三人称视角", &third_person_enabled)) {
            SetThirdPersonEnabled(third_person_enabled);
          }
          ImGui::TextDisabled("开启=第三人称，关闭=第一人称");
          ImGui::SliderFloat("第三人称后移距离", &third_person_distance, 0.5f, 8.0f, "%.2f");
          ImGui::SliderFloat("第三人称高度", &third_person_height, -0.5f, 3.0f, "%.2f");
          ImGui::SliderFloat("第三人称肩位偏移", &third_person_shoulder, -2.0f, 2.0f, "%.2f");
          ImGui::SliderFloat("第三人称平滑", &third_person_smooth, 0.2f, 15.0f, "%.2f");
          const int third_person_vk = GetThirdPersonToggleVirtualKey();
          const std::string third_person_vk_name = MenuHotkeyName(third_person_vk);
          ImGui::Text("第三人称切换键: %s", third_person_vk_name.c_str());
          ImGui::SameLine();
          if (third_person_vk == VK_F6) {
            ImGui::TextDisabled("(默认 F6)");
          }
          if (IsThirdPersonToggleKeyCaptureActive()) {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.28f, 1.0f), "监听中... 按任意键 (Esc 取消)");
          } else {
            if (ImGui::Button("监听第三人称按键")) {
              BeginThirdPersonToggleKeyCapture();
            }
            ImGui::SameLine();
            if (ImGui::Button("恢复默认 F6")) {
              SetThirdPersonToggleVirtualKey(VK_F6);
            }
          }

          SectionLabel("持久化操作");
            if (ImGui::Button("保存设置")) {
              saved.auto_refresh = auto_refresh;
              saved.auto_refresh_items = auto_refresh_items;
              saved.auto_refresh_enemies = auto_refresh_enemies;
              saved.item_esp = g_item_esp_enabled;
              saved.enemy_esp = g_enemy_esp_enabled;
              saved.native_highlight = g_native_highlight_active;
              saved.no_fall = no_fall_enabled;
              saved.speed_mult = speed_mult;
              saved.reset_each_round = false;
              saved.menu_toggle_vk = GetMenuToggleVirtualKey();
              saved.third_person_enabled = GetThirdPersonEnabled();
              saved.third_person_toggle_vk = GetThirdPersonToggleVirtualKey();
              saved.third_person_distance = third_person_distance;
              saved.third_person_height = third_person_height;
              saved.third_person_shoulder = third_person_shoulder;
              saved.third_person_smooth = third_person_smooth;
              saved.log_path = log_path_buf;
              SaveSettings(saved);
            }
          ImGui::SameLine();
          if (ImGui::Button("重新加载设置")) {
            if (LoadSettings(saved)) {
              auto_refresh = saved.auto_refresh;
              auto_refresh_items = saved.auto_refresh_items;
              auto_refresh_enemies = saved.auto_refresh_enemies;
              g_item_esp_enabled = saved.item_esp;
              g_enemy_esp_enabled = saved.enemy_esp;
                g_native_highlight_active = saved.native_highlight;
                no_fall_enabled = saved.no_fall;
                speed_mult = saved.speed_mult;
                saved.reset_each_round = false;
                SetMenuToggleVirtualKey(saved.menu_toggle_vk);
                SetThirdPersonToggleVirtualKey(saved.third_person_toggle_vk);
                SetThirdPersonEnabled(saved.third_person_enabled);
                third_person_distance = saved.third_person_distance;
                third_person_height = saved.third_person_height;
                third_person_shoulder = saved.third_person_shoulder;
                third_person_smooth = saved.third_person_smooth;
                g_esp_enabled = g_item_esp_enabled || g_enemy_esp_enabled || g_native_highlight_active;
                if (!saved.log_path.empty()) {
                  strncpy_s(log_path_buf, saved.log_path.c_str(), sizeof(log_path_buf) - 1);
                MonoSetLogPath(saved.log_path);
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("重置为默认")) {
            ResetUiDefaults(auto_refresh, auto_refresh_items, auto_refresh_enemies,
              g_item_esp_enabled, g_enemy_esp_enabled, g_native_highlight_active, no_fall_enabled,
              speed_mult, extra_jump_count, infinite_jump_enabled, god_mode_enabled);
            SetMenuToggleVirtualKey(VK_INSERT);
            SetThirdPersonToggleVirtualKey(VK_F6);
            SetThirdPersonEnabled(false);
            third_person_distance = 2.8f;
            third_person_height = 1.15f;
            third_person_shoulder = 0.35f;
            third_person_smooth = 2.0f;
            g_esp_enabled = false;
          }

          ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
      }
    }
    ImGui::End();
  }
  last_menu_open = menu_visible;

  static uint64_t maint_probe_last = 0;
  static bool maint_has_local_player = false;
  if (mono_ready && allow_read_player && now - maint_probe_last > 250) {
    LocalPlayerInfo probe_info{};
    maint_has_local_player = MonoGetLocalPlayer(probe_info);
    maint_probe_last = now;
  } else if (!mono_ready || !allow_read_player) {
    maint_has_local_player = false;
  }

  if (mono_ready && allow_read_player) {
    SetCrashStage("RenderOverlay:MonoSetThirdPerson");
    MonoSetThirdPerson(
      GetThirdPersonEnabled(),
      third_person_distance,
      third_person_height,
      third_person_shoulder,
      third_person_smooth);
    SetCrashStage("RenderOverlay:postMonoSetThirdPerson");
  }

  // Auto-maintenance toggles
  if (mono_ready && maint_has_local_player && allow_mutate_player) {
    if (no_fall_enabled) {
      MonoSetInvincible(2.0f);
      MonoOverrideJumpCooldown(0.0f);
    }
    if (infinite_jump_enabled && extra_jump_count > 0) {
      MonoSetJumpExtraDirect(extra_jump_count);
    }
    if (god_mode_enabled) {
      MonoSetInvincible(999999.0f);
    }
    if (lock_health && last_state.has_health) {
      int hp = edits.max_health > 0 ? edits.max_health : (last_state.max_health > 0 ? last_state.max_health : 999999);
      MonoSetLocalPlayerHealth(hp, hp);
    }
    if (lock_stamina && last_state.has_energy) {
      float sta = edits.max_stamina > 0 ? edits.max_stamina : (last_state.max_energy > 0 ? last_state.max_energy : 999999.0f);
      MonoSetLocalPlayerEnergy(sta, sta);
    }
  }
}
