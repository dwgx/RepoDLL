#include "pch.h"

#include "ui.h"

#include "imgui.h"

#include "mono_bridge.h"
#include "hook_dx11.h"
#include <cmath>
#include <algorithm>
#include <cfloat>
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

std::vector<PlayerState> g_cached_items;
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
  bool reset_each_round = true;
  bool session_master_override = false;
  int menu_toggle_vk = VK_INSERT;
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
  f << "session_master_override=" << s.session_master_override << "\n";
  f << "menu_toggle_vk=" << s.menu_toggle_vk << "\n";
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
      else if (k == "session_master_override") out.session_master_override = to_bool(v);
      else if (k == "menu_toggle_vk") out.menu_toggle_vk = std::stoi(v);
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
  static bool auto_refresh_items = false;
  static bool auto_refresh_enemies = false;
  static std::vector<PlayerState> squad_states;
  static uint64_t last_squad_update = 0;
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
  static bool infinite_jump_enabled = false;  // 默认关闭，需手动开启
  static bool god_mode_enabled = false;
  static float jump_force = 20.0f;
  static float grab_range_field = 5.0f;
  static float grab_strength_field = 5.0f;
  static int current_currency = 0;
  static bool has_currency = false;
  static uint64_t last_user_edit = 0;
  static uint64_t last_items_update = 0;
  static uint64_t last_enemies_update = 0;
  static bool no_fall_enabled = false;
  static bool include_local_squad = true;
  static RoundState cached_round_state{};
  static bool has_round_state = false;
  static RoundProgressState cached_round_progress{};
  static bool has_round_progress = false;
  static uint64_t last_round_update = 0;
  static std::vector<std::string> debug_logs;
  static std::vector<std::string> collector_scan_results;
  static char collector_scan_kw[64] = "haul";
  static int collector_force_value = 999999;
  static uint64_t last_log_update = 0;
  static int log_lines = 200;
  static SavedSettings saved{};
  static bool settings_loaded = false;
  static bool reset_each_round = true;
  static bool session_master_override = false;
  static bool session_master_retry = false;
  static bool session_master_suspended = false;
  static int last_stage_seen = -999;
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
    if (!saved.log_path.empty()) {
      strncpy_s(log_path_buf, saved.log_path.c_str(), sizeof(log_path_buf) - 1);
      MonoSetLogPath(saved.log_path);
    }
    if (saved.load_on_start) {
      auto_refresh = saved.auto_refresh;
      auto_refresh_items = saved.auto_refresh_items;
      auto_refresh_enemies = saved.auto_refresh_enemies;
      g_item_esp_enabled = saved.item_esp;
      g_enemy_esp_enabled = saved.enemy_esp;
      g_native_highlight_active = saved.native_highlight;
      no_fall_enabled = saved.no_fall;
      speed_mult = saved.speed_mult;
      reset_each_round = saved.reset_each_round;
      session_master_override = saved.session_master_override;
      session_master_suspended = false;
      if (session_master_override) {
        session_master_retry = true;
      } else {
        MonoSetSessionMaster(false);
        session_master_retry = false;
      }
    }
    settings_loaded = true;
  }
  if (mono_ready) {
    const bool transitioning = session_master_override ? MonoIsSessionTransitioning() : false;
    if (session_master_override) {
      if (transitioning) {
        session_master_suspended = true;
        session_master_retry = true;
        if (g_session_master_patch_active) {
          MonoSetSessionMaster(false);
        }
      } else {
        if (session_master_suspended) {
          session_master_retry = true;
        }
        session_master_suspended = false;
        if (session_master_retry) {
          session_master_retry = !MonoSetSessionMaster(true);
        }
      }
    } else {
      session_master_suspended = false;
      session_master_retry = false;
      if (g_session_master_patch_active) {
        MonoSetSessionMaster(false);
      }
    }
  }
  const uint64_t edit_cooldown_ms = 800;
  const bool safe_to_refresh = !user_editing && (now - last_user_edit > edit_cooldown_ms);
  if (MonoIsShuttingDown()) {
    return;
  }
  if (mono_ready && auto_refresh && safe_to_refresh && now - last_update > 500) {
    SetCrashStage("RenderOverlay:MonoGetLocalPlayer");
    last_ok = MonoGetLocalPlayer(last_info);
    if (last_ok) {
      MonoGetLocalPlayerState(last_state);
      has_currency = MonoGetRunCurrency(current_currency);
      MonoApplyPendingCartValue();
      inputs_synced = false;
    }
    last_update = now;
  }

  // Round/haul state refresh (关卡收集阶段)
  if (mono_ready && safe_to_refresh && now - last_round_update > 500) {
    RoundState rs{};
    RoundProgressState rps{};
    if (MonoGetRoundState(rs) && rs.ok) {
      has_round_state = true;
      cached_round_state = rs;
      if (reset_each_round && last_stage_seen != -999 && rs.stage != last_stage_seen) {
        ResetUiDefaults(auto_refresh, auto_refresh_items, auto_refresh_enemies,
          g_item_esp_enabled, g_enemy_esp_enabled, g_native_highlight_active, no_fall_enabled,
          speed_mult, extra_jump_count, infinite_jump_enabled, god_mode_enabled);
      }
      last_stage_seen = rs.stage;
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
  }

  auto refresh_items = [&]() {
    if (!mono_ready) return;
    if (g_items_disabled) return;
    // 仅在有可靠本地玩家坐标时刷新，避免主菜单/加载场景扫物品导致崩溃
    if (!last_ok || !last_state.has_position) return;
    SetCrashStage("RenderOverlay:MonoListItems");
    MonoListItemsSafe(g_cached_items);
    last_items_update = now;
  };
  auto refresh_enemies = [&]() {
    if (!mono_ready) return;
    if (g_enemy_esp_disabled) return;
    if (!last_ok || !last_state.has_position) return;
    SetCrashStage("RenderOverlay:MonoListEnemies");
    MonoListEnemiesSafe(g_cached_enemies);
    last_enemies_update = now;
  };
  auto refresh_squad = [&]() {
    if (!mono_ready) return;
    MonoListPlayers(squad_states, include_local_squad);
    last_squad_update = now;
  };
  auto refresh_logs = [&]() {
    MonoGetLogs(log_lines, debug_logs);
    last_log_update = now;
  };
  auto refresh_matrices = [&]() {
    if (!mono_ready) return;
    SetCrashStage("RenderOverlay:MonoGetCameraMatrices");
    Matrix4x4 v{}, p{};
    if (MonoGetCameraMatrices(v, p)) {
      g_cached_view = v;
      g_cached_proj = p;
      g_cached_mats_valid = true;
      g_last_matrix_update = now;
    }
  };
  // 高频刷新：降低到约 60ms，保证快速转镜时缓存不滞后
  if (mono_ready && auto_refresh_items && now - last_items_update > 60) {
    refresh_items();
  }
  if (mono_ready && auto_refresh_enemies && now - last_enemies_update > 180) {
    refresh_enemies();
  }
  if (mono_ready && safe_to_refresh && now - last_squad_update > 500) {
    refresh_squad();
  }
  if (mono_ready && now - g_last_matrix_update > 33) {
    refresh_matrices();
  }

  if (mono_ready) {
    if (round_lock_enabled) {
      int target_goal = round_goal_edit > 0 ? round_goal_edit
        : ((has_round_state && cached_round_state.goal > 0) ? cached_round_state.goal : round_current_edit);
      int target_cur = round_current_edit > 0 ? round_current_edit : target_goal;
      MonoSetRoundHaulOverride(true, target_cur, target_goal);
    }
    else {
      MonoSetRoundHaulOverride(false, 0, -1);
    }
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
  if (mono_ready && g_esp_enabled && g_native_highlight_active &&
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
          if (ImGui::Button("刷新")) {
            last_ok = MonoGetLocalPlayer(last_info);
            if (last_ok) {
              MonoGetLocalPlayerState(last_state);
            }
            inputs_synced = false;
            last_update = now;
          }
          ImGui::EndGroup();

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
          if (ImGui::BeginTable("edit_table", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.0f);

            // 位置
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("位置");
            ImGui::TableSetColumnIndex(1);
            bool disable_pos = locks.position || !last_state.has_position;
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

            // 体力
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("体力 / 最大体力");
            ImGui::TableSetColumnIndex(1);
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

            ImGui::EndTable();
          }

          SectionLabel("功能修改");
          if (ImGui::BeginTable("mods_table", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.0f);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("金钱/总价值联动");
            ImGui::TableSetColumnIndex(1);
            bool money_commit = false;
            if (ImGui::InputInt("##money", &currency_edit, 0, 0,
              ImGuiInputTextFlags_EnterReturnsTrue) ||
              ImGui::IsItemDeactivatedAfterEdit()) {
              money_commit = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("应用")) {
              money_commit = true;
            }
              if (money_commit) {
                MonoSetRunCurrency(currency_edit);
                MonoSetCartValueSafe(currency_edit);
              }
              if (has_currency) {
                ImGui::SameLine();
                ImGui::TextDisabled("当前: %d (总金库)", current_currency);
              }

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::Text("Session master");
              ImGui::TableSetColumnIndex(1);
              if (ImGui::Checkbox("成为会话主人##session_master", &session_master_override)) {
                if (session_master_override) {
                  session_master_retry = true;
                  session_master_suspended = false;
                } else {
                  MonoSetSessionMaster(false);
                  session_master_retry = false;
                  session_master_suspended = false;
                }
              }
              ImGui::SameLine();
              if (session_master_suspended) {
                ImGui::TextDisabled("会话切换/加载中，已自动暂停");
              } else {
                ImGui::TextDisabled("绕过 SemiFunc 权限校验");
              }

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::Text("关卡收集 (局内)");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginGroup();
            if (has_round_state) {
              ImGui::TextDisabled("当前 %d / 目标 %d",
                cached_round_state.current,
                cached_round_state.goal);
              if (cached_round_state.current_max > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("Max %d", cached_round_state.current_max);
              }
              if (cached_round_state.stage >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("阶段 %d", cached_round_state.stage);
              }
              bool round_apply = false;
              if (ImGui::InputInt("当前值##haul_cur", &round_current_edit, 0, 0,
                ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit()) {
                round_apply = true;
              }
              if (ImGui::InputInt("目标值##haul_goal", &round_goal_edit, 0, 0,
                ImGuiInputTextFlags_EnterReturnsTrue) ||
                ImGui::IsItemDeactivatedAfterEdit()) {
                round_apply = false;  // only apply when user clicks
              }
              ImGui::SameLine();
            if (ImGui::Button("当前=目标##copy_goal")) {
              round_current_edit = round_goal_edit;
              round_apply = true;
            }
            if (round_apply) {
              MonoSetRoundStateSafe(round_current_edit, round_goal_edit, round_current_edit);
            }
            ImGui::Checkbox("超级补丁大法", &round_lock_enabled);
            ImGui::SameLine();
            ImGui::TextDisabled("ZW[ZIWEI]");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("防摔倒/击倒");
            ImGui::TableSetColumnIndex(1);
            ImGui::Checkbox("防摔倒", &no_fall_enabled);
            }
            else {
              ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1.0f), "未找到 RoundDirector (请在局内)");
            }
            ImGui::EndGroup();

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
          ImGui::EndTabItem();
        }

        // 经济 / 关卡页
        if (ImGui::BeginTabItem("经济/关卡")) {
          SectionLabel("总金库");
          if (has_currency) {
            ImGui::TextDisabled("当前总金库: %d", current_currency);
          }
          else {
            ImGui::TextDisabled("当前总金库: 无数据");
          }
          ImGui::SameLine();
          if (ImGui::Button("读取##econ_refresh_currency")) {
            has_currency = MonoGetRunCurrency(current_currency);
          }
          ImGui::InputInt("目标金额##econ_currency", &currency_edit, 0, 0);
          if (ImGui::Button("应用总金库##econ_apply_currency")) {
            MonoSetRunCurrency(currency_edit);
            has_currency = MonoGetRunCurrency(current_currency);
          }

          SectionLabel("收集器价格");
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
            ImGui::TextDisabled("RoundDirector 无数据（请在局内）");
          }
          ImGui::InputInt("收集器当前##econ_haul_cur", &round_current_edit, 0, 0);
          ImGui::InputInt("收集器目标##econ_haul_goal", &round_goal_edit, 0, 0);
          if (ImGui::Button("当前=目标##econ_copy_goal")) {
            round_current_edit = round_goal_edit;
          }
          ImGui::SameLine();
          if (ImGui::Button("应用收集器数值##econ_apply_haul")) {
            MonoSetRoundStateSafe(round_current_edit, round_goal_edit, round_current_edit);
          }
          ImGui::Checkbox("锁定收集器(超级补丁大法)##econ_round_lock", &round_lock_enabled);
          ImGui::TextDisabled("说明: 开启后会拦截 RoundDirector.Update 的重算，按你设定值显示。");

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
            ImGui::TextDisabled("进度字段无数据（请在局内）");
          }
          ImGui::InputInt("已完成##econ_progress_done", &round_progress_completed_edit, 0, 0);
          ImGui::InputInt("总节点##econ_progress_total", &round_progress_total_edit, 0, 0);
          ImGui::InputInt("阶段##econ_progress_stage", &round_stage_edit, 0, 0);
          ImGui::Checkbox("全部完成##econ_progress_all", &round_progress_all_completed_edit);
          if (ImGui::Button("应用进度##econ_apply_progress")) {
            MonoSetRoundProgressSafe(
              round_progress_completed_edit,
              round_progress_total_edit,
              round_stage_edit,
              round_progress_all_completed_edit ? 1 : 0);
          }
          ImGui::SameLine();
          if (ImGui::Button("下一节点##econ_next_progress")) {
            ++round_progress_completed_edit;
            MonoSetRoundProgressSafe(
              round_progress_completed_edit,
              round_progress_total_edit,
              round_stage_edit,
              -1);
          }
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
          ImGui::Checkbox("自动刷新", &auto_refresh_items);
          ImGui::SameLine();
          if (ImGui::Button("刷新物品")) refresh_items();
          ImGui::EndGroup();
          ImGui::SameLine();
          ImGui::TextDisabled("共 %d", static_cast<int>(g_cached_items.size()));
          if (MonoItemsDisabled()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.35f, 1.0f), "物品扫描已自动关闭(崩溃保护)");
          }
          if (ImGui::Button("安全刷新一次")) {
            MonoManualRefreshItems(g_cached_items);
            last_items_update = now;
          }
          ImGui::SameLine();
          if (ImGui::Button("重置物品禁用")) {
            MonoResetItemsDisabled();
          }
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

            for (const auto& st : g_cached_items) {
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

        // 队友/复活
        if (ImGui::BeginTabItem("队友")) {
          ImGui::Checkbox("包含自己", &include_local_squad);
          ImGui::SameLine();
          if (ImGui::Button("刷新队友")) refresh_squad();
          ImGui::SameLine();
          if (ImGui::Button("复活队友")) {
            MonoReviveAllPlayers(false);
            refresh_squad();
          }
          ImGui::SameLine();
          if (ImGui::Button("全体满血(含自己)")) {
            MonoReviveAllPlayers(true);
            refresh_squad();
          }
          ImGui::SameLine();
          ImGui::TextDisabled("队友数: %d", static_cast<int>(squad_states.size()));

          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY;
          ImVec2 table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 8.0f);
          if (ImGui::BeginTable("squad_table_view", 5, flags, table_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("生命", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("距离", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("坐标", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();

            for (const auto& st : squad_states) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(st.has_name ? st.name.c_str() : "<player>");

              ImGui::TableSetColumnIndex(1);
              if (st.has_health) ImGui::Text("%d / %d", st.health, st.max_health);
              else ImGui::TextDisabled("-");

              ImGui::TableSetColumnIndex(2);
              bool downed = st.has_health && st.health <= 0;
              ImVec4 col = downed ? ImVec4(0.9f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
              ImGui::TextColored(col, "%s", downed ? "倒地" : "存活");

              ImGui::TableSetColumnIndex(3);
              if (last_state.has_position && st.has_position) {
                float dx = st.x - last_state.x;
                float dy = st.y - last_state.y;
                float dz = st.z - last_state.z;
                ImGui::Text("%.1fm", std::sqrt(dx * dx + dy * dy + dz * dz));
              } else {
                ImGui::TextDisabled("-");
              }

              ImGui::TableSetColumnIndex(4);
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
          if (ImGui::Button("手动刷新物品")) {
            MonoManualRefreshItems(g_cached_items);
            last_items_update = now;
          }
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
          ImGui::Checkbox("自动刷新", &auto_refresh_enemies);
          ImGui::SameLine();
          if (ImGui::Button("刷新敌人")) refresh_enemies();
          ImGui::EndGroup();
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
          ImGui::Checkbox("每局重置为默认", &reset_each_round);
          ImGui::Checkbox("默认绘制覆盖层", &g_esp_enabled);
          ImGui::Checkbox("默认物品ESP", &g_item_esp_enabled);
          ImGui::SameLine();
          ImGui::Checkbox("默认敌人ESP", &g_enemy_esp_enabled);
          ImGui::Checkbox("默认原生高亮", &g_native_highlight_active);
          ImGui::SameLine();
          ImGui::Checkbox("默认防摔倒", &no_fall_enabled);
          ImGui::Checkbox("默认自动刷新玩家状态", &auto_refresh);
          ImGui::SameLine();
          ImGui::Checkbox("默认自动刷新物品", &auto_refresh_items);
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
              saved.reset_each_round = reset_each_round;
              saved.session_master_override = session_master_override;
              saved.menu_toggle_vk = GetMenuToggleVirtualKey();
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
                reset_each_round = saved.reset_each_round;
                session_master_override = saved.session_master_override;
                session_master_suspended = false;
                SetMenuToggleVirtualKey(saved.menu_toggle_vk);
                if (session_master_override) {
                  session_master_retry = true;
                } else {
                  MonoSetSessionMaster(false);
                  session_master_retry = false;
                }
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

  // Auto-maintenance toggles
  if (last_ok) {
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
