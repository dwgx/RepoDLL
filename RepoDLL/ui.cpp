#include "pch.h"

#include "ui.h"

#include "imgui.h"

#include "mono_bridge.h"
#include <cmath>
#include <algorithm>
#include <cfloat>

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
  ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.48f, 1.0f), "%s", label);
  ImGui::Separator();
}
}  // namespace

const std::vector<PlayerState>& UiGetCachedItems() {
  return g_cached_items;
}

const std::vector<PlayerState>& UiGetCachedEnemies() {
  return g_cached_enemies;
}

bool UiGetCachedMatrices(Matrix4x4& view, Matrix4x4& proj) {
  if (!g_cached_mats_valid) return false;
  view = g_cached_view;
  proj = g_cached_proj;
  return true;
}

void RenderOverlay(bool* menu_open) {
  const bool menu_visible = menu_open && *menu_open;
  static bool last_menu_open = false;

  static LocalPlayerInfo last_info;
  static bool last_ok = false;
  static uint64_t last_update = 0;
  static PlayerState last_state;
  static EditableFields edits;
  static FieldLocks locks;
  static bool lock_health = false;
  static bool lock_stamina = false;
  static bool inputs_synced = false;
  static bool auto_refresh = true;  // 默认开启自动刷新
  static bool auto_refresh_items = true;
  static bool auto_refresh_enemies = true;
  static bool native_highlight_enabled = false;
  static int native_highlight_state = 0;  // 0=Default, 1=Reminder, 2=Bad
  static int native_highlight_limit = 160;
  static uint64_t last_highlight_tick = 0;
  static uint64_t last_persist_tick = 0;
  static int last_highlight_count = 0;
  static int currency_edit = 999999;
  static float speed_mult = 3.0f;
  static int extra_jump_count = 9999;  // 固定大值用于无限跳跃
  static float jump_cooldown = 0.0f;
  static int grab_strength = 1000;
  static bool infinite_jump_enabled = true;
  static bool god_mode_enabled = false;
  static float jump_force = 20.0f;
  static float grab_range_field = 5.0f;
  static float grab_strength_field = 5.0f;
  static int current_currency = 0;
  static bool has_currency = false;
  static uint64_t last_user_edit = 0;

  static uint64_t last_items_update = 0;
  static uint64_t last_enemies_update = 0;

  const uint64_t now = GetTickCount64();
  const bool mono_ready = MonoInitialize();
  const bool user_editing = ImGui::IsAnyItemActive();
  if (user_editing) {
    last_user_edit = now;
  }
  const uint64_t edit_cooldown_ms = 800;
  const bool safe_to_refresh = !user_editing && (now - last_user_edit > edit_cooldown_ms);
  if (MonoIsShuttingDown()) {
    return;
  }
  if (mono_ready && auto_refresh && safe_to_refresh && now - last_update > 500) {
    last_ok = MonoGetLocalPlayer(last_info);
    if (last_ok) {
      MonoGetLocalPlayerState(last_state);
      has_currency = MonoGetRunCurrency(current_currency);
      MonoApplyPendingCartValue();
      inputs_synced = false;
    }
    last_update = now;
  }

  // 刷新物品 / 敌人列表（轻量定时）
  auto refresh_items = [&]() {
    if (!mono_ready) return;
    MonoListItems(g_cached_items);
    last_items_update = now;
  };
  auto refresh_enemies = [&]() {
    if (!mono_ready) return;
    MonoListEnemies(g_cached_enemies);
    last_enemies_update = now;
  };
  auto refresh_matrices = [&]() {
    if (!mono_ready) return;
    Matrix4x4 v{}, p{};
    if (MonoGetCameraMatrices(v, p)) {
      g_cached_view = v;
      g_cached_proj = p;
      g_cached_mats_valid = true;
      g_last_matrix_update = now;
    }
  };
  if (mono_ready && auto_refresh_items && now - last_items_update > 800) {
    refresh_items();
  }
  if (mono_ready && auto_refresh_enemies && now - last_enemies_update > 1000) {
    refresh_enemies();
  }
  if (mono_ready && now - g_last_matrix_update > 200) {
    refresh_matrices();
  }

  // Native in-game highlight (ValuableDiscover)
  if (mono_ready && g_esp_enabled && native_highlight_enabled &&
      MonoValueFieldsResolved() && MonoNativeHighlightAvailable()) {
    if (now - last_highlight_tick > 900) {
      int count = 0;
      if (MonoTriggerValuableDiscoverSafe(native_highlight_state, native_highlight_limit, count)) {
        last_highlight_count = count;
      }
      last_highlight_tick = now;
    }
    if (now - last_persist_tick > 200) {
      int count = 0;
      MonoApplyValuableDiscoverPersistenceSafe(true, 0.0f, count);
      last_persist_tick = now;
    }
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
            ImGui::Text("金钱/推车价值");
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
              MonoSetCartValue(currency_edit);
            }
            if (has_currency) {
              ImGui::SameLine();
              ImGui::TextDisabled("当前: %d", current_currency);
            }

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
              if (infinite_jump_enabled && extra_jump_count > 0) {
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

        // 物品 / ESP 页
        if (ImGui::BeginTabItem("物品/ESP")) {
          ImGui::Checkbox("启用ESP常亮", &g_esp_enabled);
          ImGui::SameLine();
          ImGui::Checkbox("原生高亮", &native_highlight_enabled);
          ImGui::SameLine();
          ImGui::Checkbox("自动刷新", &auto_refresh_items);
          ImGui::SameLine();
          if (ImGui::Button("刷新物品")) refresh_items();
          ImGui::SameLine();
          ImGui::TextDisabled("共 %d", static_cast<int>(g_cached_items.size()));

          ImGui::Spacing();
          ImGui::BeginGroup();
          ImGui::TextDisabled("原生高亮状态");
          ImGui::SliderInt("模式##native_state", &native_highlight_state, 0, 2);
          ImGui::SliderInt("最大数量##native_limit", &native_highlight_limit, 20, 512);
          ImGui::SameLine();
          ImGui::TextDisabled("最近触发: %d", last_highlight_count);
          ImGui::EndGroup();

          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable;
          ImVec2 table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 8.0f);
          if (ImGui::BeginTable("items_table_view", 6, flags, table_size)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("价值", ImGuiTableColumnFlags_WidthStretch, 1.0f);
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
              if (st.has_value) ImGui::Text("%d", st.value); else ImGui::TextDisabled("-");

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

        // 敌人页
        if (ImGui::BeginTabItem("敌人")) {
          ImGui::Checkbox("自动刷新", &auto_refresh_enemies);
          ImGui::SameLine();
          if (ImGui::Button("刷新敌人")) refresh_enemies();
          ImGui::SameLine();
          ImGui::TextDisabled("共 %d", static_cast<int>(g_cached_enemies.size()));

          ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY;
          ImVec2 table_size = ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y - 8.0f);
          if (ImGui::BeginTable("enemy_table_view", 5, flags, table_size)) {
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
