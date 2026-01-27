#include "pch.h"

#include "mono_bridge.h"

#include <cstring>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cstdint>
#include <sstream>
#include <unordered_set>
#include <psapi.h>
#include <vector>
#include <functional>

#include "config.h"

namespace {
using MonoDomain = void;
using MonoAssembly = void;
using MonoImage = void;
using MonoClass = void;
using MonoClassField = void;
using MonoType = void;
using MonoMethod = void;
using MonoObject = void;
using MonoThread = void;
using MonoVTable = void;

struct MonoApi {
  HMODULE module = nullptr;
  MonoDomain* (__cdecl* mono_get_root_domain)() = nullptr;
  MonoThread* (__cdecl* mono_thread_attach)(MonoDomain*) = nullptr;
  void (__cdecl* mono_assembly_foreach)(void (*)(MonoAssembly*, void*), void*) = nullptr;
  MonoImage* (__cdecl* mono_assembly_get_image)(MonoAssembly*) = nullptr;
  const char* (__cdecl* mono_image_get_name)(MonoImage*) = nullptr;
  MonoClass* (__cdecl* mono_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
  MonoVTable* (__cdecl* mono_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
  MonoClassField* (__cdecl* mono_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
  MonoMethod* (__cdecl* mono_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
  MonoType* (__cdecl* mono_class_get_type)(MonoClass*) = nullptr;
  MonoObject* (__cdecl* mono_type_get_object)(MonoDomain*, MonoType*) = nullptr;
  MonoObject* (__cdecl* mono_runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
  void (__cdecl* mono_field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
  void (__cdecl* mono_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
  void (__cdecl* mono_field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;
  MonoClass* (__cdecl* mono_object_get_class)(MonoObject*) = nullptr;
  void* (__cdecl* mono_object_unbox)(MonoObject*) = nullptr;
  int (__cdecl* mono_field_get_offset)(MonoClassField*) = nullptr;
  MonoObject* (__cdecl* mono_string_new)(MonoDomain*, const char*) = nullptr;
};

struct MonoArray {
  void* obj;
  void* bounds;
  size_t max_length;
  void* vector[1];
};

MonoApi g_mono;
MonoDomain* g_domain = nullptr;
DWORD g_attached_thread_id = 0;

MonoImage* g_image = nullptr;
MonoClass* g_game_director_class = nullptr;
MonoVTable* g_game_director_vtable = nullptr;
MonoClassField* g_game_director_instance_field = nullptr;
MonoClassField* g_game_director_player_list_field = nullptr;

MonoClass* g_player_avatar_class = nullptr;
MonoVTable* g_player_avatar_vtable = nullptr;
MonoClassField* g_player_avatar_is_local_field = nullptr;
MonoClassField* g_player_avatar_instance_field = nullptr;
MonoClassField* g_player_avatar_transform_field = nullptr;
MonoClassField* g_player_avatar_health_field = nullptr;
MonoClassField* g_player_avatar_energy_field = nullptr;
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

MonoClass* g_game_object_class = nullptr;
MonoMethod* g_game_object_get_component = nullptr;

MonoClass* g_transform_class = nullptr;
MonoMethod* g_transform_get_local_to_world = nullptr;
MonoMethod* g_transform_get_world_to_local = nullptr;

MonoClass* g_semi_func_class = nullptr;
MonoMethod* g_player_avatar_local_method = nullptr;
MonoMethod* g_player_get_all_method = nullptr;

// Item system
MonoClass* g_item_manager_class = nullptr;
MonoVTable* g_item_manager_vtable = nullptr;
MonoClassField* g_item_manager_instance_field = nullptr;
MonoClassField* g_item_manager_item_volumes_field = nullptr;
MonoClassField* g_item_manager_spawned_items_field = nullptr;
MonoMethod* g_item_manager_get_all_items = nullptr;
int g_item_manager_get_all_items_argc = 0;
const char* g_item_manager_get_all_items_name = nullptr;

// Alternative item population methods
MonoMethod* g_semi_func_shop_populate = nullptr;
MonoMethod* g_semi_func_truck_populate = nullptr;
MonoMethod* g_find_objects_of_type_itemvolume = nullptr;
MonoMethod* g_find_objects_of_type_itemvolume_include_inactive = nullptr;
MonoClass* g_resources_class = nullptr;
MonoMethod* g_resources_find_objects_of_type_all = nullptr;
MonoClass* g_item_volume_class = nullptr;

MonoClass* g_valuable_object_class = nullptr;

// ItemTracker rendering (optional)
// Removed unused ItemTracker wiring

// Currency / movement / combat helpers
MonoMethod* g_semi_func_stat_set_run_currency = nullptr;
MonoMethod* g_semi_func_stat_get_run_currency = nullptr;

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

// Visual / gamma / post-processing
// Pending cart value
int g_pending_cart_value = 0;
bool g_pending_cart_active = false;
bool g_cart_apply_in_progress = false;

// Shutdown guard
bool g_shutting_down = false;

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

void AppendLog(const std::string& message) {
  static const char* kLogPath = "D:\\Project\\REPO_LOG.txt";
  std::ofstream file(kLogPath, std::ios::app);
  if (!file) {
    return;
  }
  file << "[" << NowString() << "] " << message << "\n";
}

bool AppendLogOnce(const std::string& key, const std::string& message) {
  static std::unordered_set<std::string> logged;
  if (logged.insert(key).second) {
    AppendLog(message);
    return true;
  }
  return false;
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

  // If not found, scan all modules to find the signature and patch by pattern.
  auto pattern_patch = []() {
    // Signature: 28 92 0F 00 06 2C 01 2A (call IsNotMasterClient; brfalse; ret)
    const uint8_t sig[] = {0x28, 0x92, 0x0F, 0x00, 0x06, 0x2C, 0x01, 0x2A};
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    size_t patched_count = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = start; addr < end && patched_count < 3;) {
      if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        addr += 0x1000;
        continue;
      }
      bool readable = (mbi.State == MEM_COMMIT) && (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READWRITE | PAGE_READONLY));
      if (readable && !(mbi.Protect & PAGE_GUARD)) {
        uint8_t* region = static_cast<uint8_t*>(mbi.AllocationBase);
        size_t region_size = mbi.RegionSize;
        // scan region
        for (size_t i = 0; i + sizeof(sig) <= region_size && patched_count < 3; ++i) {
          uint8_t* p = reinterpret_cast<uint8_t*>(addr) + i;
          if (memcmp(p, sig, sizeof(sig)) == 0) {
            DWORD old = 0;
            if (VirtualProtect(p, sizeof(sig), PAGE_EXECUTE_READWRITE, &old)) {
              std::memset(p, 0x00, sizeof(sig));  // NOP out
              DWORD dummy;
              VirtualProtect(p, sizeof(sig), old, &dummy);
              ++patched_count;
            }
          }
        }
      }
      addr += mbi.RegionSize;
    }
    if (patched_count > 0) {
      std::ostringstream oss;
      oss << "PatchAntiMasterChecks: pattern patched " << patched_count << " occurrences";
      AppendLog(oss.str());
      return true;
    }
    return false;
  };

  if (!mod) {
    if (!warned_once) {
      AppendLog("PatchAntiMasterChecks: Assembly-CSharp.dll not loaded yet");
      warned_once = true;
    }
    // Attempt pattern scan to patch anyway
    if (pattern_patch()) {
      patched = true;
      warned_once = false;
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
      !ResolveProc(module, "mono_class_get_type", api.mono_class_get_type) ||
      !ResolveProc(module, "mono_type_get_object", api.mono_type_get_object) ||
      !ResolveProc(module, "mono_runtime_invoke", api.mono_runtime_invoke) ||
      !ResolveProc(module, "mono_field_get_value", api.mono_field_get_value) ||
      !ResolveProc(module, "mono_field_static_get_value", api.mono_field_static_get_value) ||
      !ResolveProc(module, "mono_field_set_value", api.mono_field_set_value) ||
      !ResolveProc(module, "mono_object_get_class", api.mono_object_get_class) ||
      !ResolveProc(module, "mono_object_unbox", api.mono_object_unbox) ||
      !ResolveProc(module, "mono_field_get_offset", api.mono_field_get_offset) ||
      !ResolveProc(module, "mono_string_new", api.mono_string_new)) {
    AppendLog("Failed to resolve one or more Mono exports");
    return false;
  }

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
  } data{ns, name, nullptr};

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
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_avatar_is_local_field);
      AppendLog("PlayerAvatar::isLocal offset = " + std::to_string(off));
    }
  }
  if (g_player_avatar_class && !g_player_avatar_transform_field) {
    g_player_avatar_transform_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarTransformField);
    if (!g_player_avatar_transform_field) {
      AppendLog("Failed to resolve PlayerAvatar::playerTransform field");
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_avatar_transform_field);
      AppendLog("PlayerAvatar::playerTransform offset = " + std::to_string(off));
    }
  }
  if (g_player_avatar_class && !g_player_avatar_health_field) {
    g_player_avatar_health_field = g_mono.mono_class_get_field_from_name(
        g_player_avatar_class, config::kPlayerAvatarHealthField);
    if (!g_player_avatar_health_field) {
      AppendLog("Failed to resolve PlayerAvatar::playerHealth field");
    } else if (g_mono.mono_field_get_offset) {
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
    } else if (g_mono.mono_field_get_offset) {
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
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_health_value_field);
      AppendLog("PlayerHealth::health offset = " + std::to_string(off));
    }
  }
  if (g_player_health_class && !g_player_health_max_field) {
    g_player_health_max_field = g_mono.mono_class_get_field_from_name(
        g_player_health_class, config::kPlayerHealthMaxField);
    if (!g_player_health_max_field) {
      AppendLog("Failed to resolve PlayerHealth::maxHealth field");
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_health_max_field);
      AppendLog("PlayerHealth::maxHealth offset = " + std::to_string(off));
    }
  }
  if (g_player_health_class && !g_player_health_invincible_set) {
    g_player_health_invincible_set = g_mono.mono_class_get_method_from_name(
        g_player_health_class, config::kPlayerHealthInvincibleSetMethod, 1);
    if (g_player_health_invincible_set) {
      AppendLog("Resolved PlayerHealth::InvincibleSet");
    } else {
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
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_controller_energy_current_field);
      AppendLog("PlayerController::EnergyCurrent offset = " + std::to_string(off));
    }
  }
  if (g_player_controller_class && !g_player_controller_energy_start_field) {
    g_player_controller_energy_start_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerEnergyStartField);
    if (!g_player_controller_energy_start_field) {
      AppendLog("Failed to resolve PlayerController::EnergyStart field");
    } else if (g_mono.mono_field_get_offset) {
      int off = g_mono.mono_field_get_offset(g_player_controller_energy_start_field);
      AppendLog("PlayerController::EnergyStart offset = " + std::to_string(off));
    }
  }
  if (g_player_controller_class && !g_player_controller_jump_extra_field) {
    g_player_controller_jump_extra_field = g_mono.mono_class_get_field_from_name(
        g_player_controller_class, config::kPlayerControllerJumpExtraField);
    if (g_player_controller_jump_extra_field) {
      AppendLog("Resolved PlayerController::JumpExtra field");
    } else {
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
    } else {
      AppendLog("Successfully resolved UnityEngine.Camera class");
    }
  }
  if (g_unity_camera_class && !g_unity_camera_get_main) {
    g_unity_camera_get_main = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraMainMethod, 0);
    if (g_unity_camera_get_main) {
      AppendLog("Successfully resolved Camera::get_main method");
    } else {
      AppendLog("Failed to resolve Camera::get_main method");
    }
  }
  if (g_unity_camera_class && !g_unity_camera_get_projection_matrix) {
    g_unity_camera_get_projection_matrix = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraProjectionMatrixMethod, 0);
    if (g_unity_camera_get_projection_matrix) {
      AppendLog("Successfully resolved Camera::get_projectionMatrix method");
    } else {
      AppendLog("Failed to resolve Camera::get_projectionMatrix method");
    }
  }
  if (g_unity_camera_class && !g_unity_camera_get_world_to_camera_matrix) {
    g_unity_camera_get_world_to_camera_matrix = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraWorldToCameraMatrixMethod, 0);
    if (g_unity_camera_get_world_to_camera_matrix) {
      AppendLog("Successfully resolved Camera::get_worldToCameraMatrix method");
    } else {
      AppendLog("Failed to resolve Camera::get_worldToCameraMatrix method");
    }
  }
  if (g_unity_camera_class && !g_unity_camera_get_transform) {
    g_unity_camera_get_transform = g_mono.mono_class_get_method_from_name(
        g_unity_camera_class, config::kUnityCameraGetTransformMethod, 0);
    if (g_unity_camera_get_transform) {
      AppendLogOnce("Camera_get_transform_ok", "Successfully resolved Camera::get_transform method");
    } else {
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
    } else {
      AppendLogOnce("Component_class_ok", "Successfully resolved UnityEngine.Component class");
    }
  }
  if (g_component_class && !g_component_get_transform) {
    g_component_get_transform = g_mono.mono_class_get_method_from_name(
        g_component_class, config::kUnityCameraGetTransformMethod, 0);
    if (g_component_get_transform) {
      AppendLogOnce("Component_get_transform_ok", "Successfully resolved Component::get_transform method");
    } else {
      AppendLogOnce("Component_get_transform_fail", "Failed to resolve Component::get_transform method");
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
    } else {
      AppendLogOnce("GameObject_class_ok", "Successfully resolved UnityEngine.GameObject class");
    }
  }
  if (g_game_object_class && !g_game_object_get_component) {
    g_game_object_get_component =
        g_mono.mono_class_get_method_from_name(g_game_object_class, "GetComponent", 1);
    if (g_game_object_get_component) {
      AppendLogOnce("GameObject_get_component_ok", "Successfully resolved GameObject::GetComponent(Type)");
    } else {
      AppendLogOnce("GameObject_get_component_fail", "Failed to resolve GameObject::GetComponent(Type)");
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
    } else {
      AppendLog("Successfully resolved UnityEngine.Transform class");
    }
  }
  if (g_transform_class && !g_transform_get_local_to_world) {
    g_transform_get_local_to_world = g_mono.mono_class_get_method_from_name(
        g_transform_class, config::kTransformLocalToWorldMatrixMethod, 0);
    if (g_transform_get_local_to_world) {
      AppendLog("Successfully resolved Transform::get_localToWorldMatrix method");
    } else {
      AppendLog("Failed to resolve Transform::get_localToWorldMatrix method");
    }
  }
  if (g_transform_class && !g_transform_get_world_to_local) {
    g_transform_get_world_to_local = g_mono.mono_class_get_method_from_name(
        g_transform_class, config::kTransformWorldToLocalMatrixMethod, 0);
    if (g_transform_get_world_to_local) {
      AppendLog("Successfully resolved Transform::get_worldToLocalMatrix method");
    } else {
      AppendLog("Failed to resolve Transform::get_worldToLocalMatrix method");
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
    } else {
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
    const char* namespaces[] = {"", "Game", "Physics", "Items", "Player"};
    const char* class_names[] = {config::kPhysGrabCartClass, "GrabCart", "HandCart"};
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
        "cartValue"};
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
    } else {
      AppendLog("Failed to resolve PunManager class");
    }
  }
  if (g_pun_manager_class && !g_pun_manager_vtable) {
    g_pun_manager_vtable = g_mono.mono_class_vtable(g_domain, g_pun_manager_class);
  }
  if (g_pun_manager_class && !g_pun_manager_instance_field) {
    const char* inst_names[] = {"instance", "Instance", "s_Instance", "m_Instance", "staticInstance"};
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
    } else {
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
    const int arg_counts[] = {0, 1};
    for (const char* name : candidates) {
      for (int argc : arg_counts) {
        g_item_manager_get_all_items =
            g_mono.mono_class_get_method_from_name(g_item_manager_class, name, argc);
        if (g_item_manager_get_all_items) {
          g_item_manager_get_all_items_argc = argc;
          g_item_manager_get_all_items_name = name;
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

  // Try to resolve FindObjectsOfType (with/without includeInactive) as fallback
  if (!g_find_objects_of_type_itemvolume || !g_find_objects_of_type_itemvolume_include_inactive) {
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
      // Try to find FindObjectsOfType(Type)
      if (!g_find_objects_of_type_itemvolume) {
        g_find_objects_of_type_itemvolume = g_mono.mono_class_get_method_from_name(
            unity_object_class, "FindObjectsOfType", 1);
        if (g_find_objects_of_type_itemvolume) {
          AppendLog("Resolved Object::FindObjectsOfType(Type) method");
        }
      }
      // Try to find FindObjectsOfType(Type, bool includeInactive)
      if (!g_find_objects_of_type_itemvolume_include_inactive) {
        g_find_objects_of_type_itemvolume_include_inactive = g_mono.mono_class_get_method_from_name(
            unity_object_class, "FindObjectsOfType", 2);
        if (g_find_objects_of_type_itemvolume_include_inactive) {
          AppendLog("Resolved Object::FindObjectsOfType(Type, bool) method");
        }
      }
    }
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
    } else {
      AppendLog("Failed to resolve ItemVolume class");
    }
  }

  if (!g_valuable_object_class) {
    g_valuable_object_class = g_mono.mono_class_from_name(
        g_image, config::kValuableObjectNamespace, config::kValuableObjectClass);
    if (!g_valuable_object_class) {
      AppendLog("Failed to resolve ValuableObject class");
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

    bool is_local = false;
    g_mono.mono_field_get_value(player, g_player_avatar_is_local_field, &is_local);
    if (is_local) {
      AppendLogOnce("LocalPlayer_from_list", "Local player found via PlayerList");
      return player;
    }
  }

  AppendLogOnce("LocalPlayer_not_in_list", "Local player not found in PlayerList");
  return nullptr;
}

bool TryGetPositionFromTransform(MonoObject* transform_obj, PlayerState& out_state) {
  if (!transform_obj) {
    AppendLog("TryGetPositionFromTransform: transform_obj is null");
    return false;
  }

  MonoClass* transform_class = g_mono.mono_object_get_class(transform_obj);
  if (!transform_class) {
    AppendLog("TryGetPositionFromTransform: transform_class is null");
    return false;
  }

  MonoMethod* get_position = g_mono.mono_class_get_method_from_name(
      transform_class, config::kTransformGetPositionMethod, 0);
  if (!get_position) {
    AppendLog("TryGetPositionFromTransform: get_position method not found");
    return false;
  }

  MonoObject* exception = nullptr;
  MonoObject* position_obj =
      g_mono.mono_runtime_invoke(get_position, transform_obj, nullptr, &exception);
  if (exception || !position_obj) {
    AppendLog("TryGetPositionFromTransform: mono_runtime_invoke failed or returned null");
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
    AppendLog("TryGetPositionFromTransform: vector class is null");
    return false;
  }
  static MonoClassField* x_field = nullptr;
  static MonoClassField* y_field = nullptr;
  static MonoClassField* z_field = nullptr;
  if (!x_field) x_field = g_mono.mono_class_get_field_from_name(vec_class, "x");
  if (!y_field) y_field = g_mono.mono_class_get_field_from_name(vec_class, "y");
  if (!z_field) z_field = g_mono.mono_class_get_field_from_name(vec_class, "z");
  if (!x_field || !y_field || !z_field) {
    AppendLog("TryGetPositionFromTransform: vector field(s) missing");
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

long LogCrash(const char* where, unsigned long code, EXCEPTION_POINTERS* info) {
  std::ostringstream oss;
  oss << "CRASH in " << (where ? where : "<unknown>")
      << " code=0x" << std::hex << code
      << " addr=0x"
      << std::hex
      << (info && info->ExceptionRecord ? reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) : 0);
  AppendLog(oss.str());
  return EXCEPTION_EXECUTE_HANDLER;
}

bool MonoInitialize() {
  if (g_shutting_down) return false;
  return EnsureThreadAttached();
}

bool MonoBeginShutdown() {
  g_shutting_down = true;
  g_pending_cart_active = false;
  g_cart_apply_in_progress = false;
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
  bool via_static_instance = false;
  bool via_method = false;

  // Prefer PlayerList traversal (most reliable).
  local_player = FindLocalPlayerFromList();
  if (local_player) {
    via_list = true;
  }

  // Fallback: static instance field with multiple name candidates.
  if (!local_player && g_player_avatar_instance_field && g_player_avatar_vtable) {
    g_mono.mono_field_static_get_value(
      g_player_avatar_vtable, g_player_avatar_instance_field, &local_player);
      if (local_player) {
        via_static_instance = true;
        AppendLogOnce("localplayer_static_resolved",
                      "Local player resolved via PlayerAvatar static instance");
      } else {
        AppendLogOnce("localplayer_static_null", "PlayerAvatar static instance is null");
      }
    } else if (!local_player) {
      if (!g_player_avatar_instance_field) {
        AppendLogOnce("localplayer_static_field_unresolved",
                      "Skipping static instance lookup: instance field unresolved");
      } else if (!g_player_avatar_vtable) {
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
      } else if (local_player) {
        via_method = true;
        AppendLogOnce("localplayer_method_resolved",
                      "Local player resolved via SemiFunc::PlayerAvatarLocal");
      }
    } else if (!local_player && !g_player_avatar_local_method) {
      AppendLogOnce("localplayer_method_unresolved",
                    "Skipping SemiFunc::PlayerAvatarLocal: method unresolved");
    }

  if (!local_player) {
    AppendLogOnce("localplayer_failed", "Failed to resolve local player via all strategies");
    return false;
  }

  bool is_local = false;
  if (g_player_avatar_is_local_field) {
    g_mono.mono_field_get_value(local_player, g_player_avatar_is_local_field, &is_local);
  }

  AppendLogOnce("localplayer_found",
                "Local player found via one of the strategies (logging once)");

  out_info.object = local_player;
  out_info.is_local = is_local;
  out_info.via_player_list = via_list;
  return true;
}

bool MonoGetLocalPlayer(LocalPlayerInfo& out_info) {
  return MonoGetLocalPlayerInternal(out_info);
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

  float vec[3] = {x, y, z};
  void* args[1] = {vec};
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
  } else {
    AppendLog("MonoSetLocalPlayerHealth: health value field unresolved");
  }
  if (g_player_health_max_field) {
    g_mono.mono_field_set_value(health_obj, g_player_health_max_field, &max_health);
  } else {
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
  } else {
    AppendLog("MonoSetLocalPlayerEnergy: EnergyCurrent field unresolved");
  }
  if (g_player_controller_energy_start_field) {
    g_mono.mono_field_set_value(controller_instance, g_player_controller_energy_start_field,
                                &max_energy);
  } else {
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

bool SetRunCurrencyViaPunManager(int amount) {
  if (!g_pun_set_run_stat_set || !g_pun_manager_instance_field || !g_pun_manager_vtable ||
      !g_mono.mono_string_new) {
    return false;
  }
  MonoObject* pun_instance = nullptr;
  g_mono.mono_field_static_get_value(g_pun_manager_vtable, g_pun_manager_instance_field,
                                     &pun_instance);
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
  void* args[2] = {key, &amount};
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_pun_set_run_stat_set, pun_instance, args, &exc);
  if (exc) {
    AppendLog("SetRunCurrencyViaPunManager: SetRunStatSet threw exception");
    return false;
  }
  AppendLog("SetRunCurrencyViaPunManager: SetRunStatSet succeeded");
  return true;
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
  void* args[1] = {key};
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
  void* args[2] = {key, &amount};
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
  if (!g_semi_func_stat_set_run_currency) {
    AppendLog("MonoSetRunCurrency: StatSetRunCurrency method unresolved");
  }

  bool success = false;
  if (g_semi_func_stat_set_run_currency) {
    void* args[1] = {&amount};
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
  } else {
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
    void* args[2] = {&multiplier, &duration_seconds};
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

  void* args[2] = {steam_str, &count};
  MonoObject* exc = nullptr;
  g_mono.mono_runtime_invoke(g_pun_upgrade_extra_jump, nullptr, args, &exc);
  if (exc) {
    AppendLog("MonoUpgradeExtraJump: method threw exception");
    return false;
  }
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
  void* args[1] = {&seconds};
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

  void* args[1] = {&duration_seconds};
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
  if (!g_pun_upgrade_grab_strength && !g_pun_upgrade_throw_strength) {
    static bool logged = false;
    if (!logged) {
      AppendLog("MonoSetGrabStrength: methods unresolved");
      logged = true;
    }
    return false;
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
  if (g_pun_upgrade_grab_strength) {
    void* args[2] = {steam_str, &grab_strength};
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_pun_upgrade_grab_strength, nullptr, args, &exc);
    if (!exc) ok = true;
  }
  if (g_pun_upgrade_throw_strength) {
    void* args[2] = {steam_str, &throw_strength};
    MonoObject* exc = nullptr;
    g_mono.mono_runtime_invoke(g_pun_upgrade_throw_strength, nullptr, args, &exc);
    if (!exc) ok = true;
  }
  if (!ok) {
    AppendLog("MonoSetGrabStrength: failed");
  }
  return ok;
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
      void* args[1] = {type_obj};
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
      void* args[1] = {type_obj};
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
  if (!g_cart_apply_in_progress) {
    g_pending_cart_value = value;
    g_pending_cart_active = true;
  }
  bool currency_ok = false;
  if (g_semi_func_stat_set_run_currency) {
    void* args_fast[1] = {&value};
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
  if (currency_ok) {
    return true;
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
    } else {
      MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      void* args[1] = {type_obj};
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
          g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: FindObjectsOfType(Type) threw exception");
      } else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: FindObjectsOfType length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      } else {
        AppendLog("MonoSetCartValue: FindObjectsOfType returned null array");
      }
    }
  } else {
    AppendLogOnce("MonoSetCartValue_findobj_missing",
                  "MonoSetCartValue: FindObjectsOfType unavailable, will try ItemManager list");
  }

  // Fallback: FindObjectsOfType including inactive objects
  if (!cart && g_find_objects_of_type_itemvolume_include_inactive && g_mono.mono_class_get_type &&
      g_mono.mono_type_get_object) {
    MonoType* cart_type = g_mono.mono_class_get_type(g_phys_grab_cart_class);
    if (!cart_type) {
      AppendLog("MonoSetCartValue: cart_type null for FindObjectsOfType(Type,bool)");
    } else {
      MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      bool include_inactive = true;
      void* args[2] = {type_obj, &include_inactive};
      MonoObject* exc = nullptr;
      MonoObject* arr_obj = g_mono.mono_runtime_invoke(
          g_find_objects_of_type_itemvolume_include_inactive, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: FindObjectsOfType(Type,bool) threw exception");
      } else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: FindObjectsOfType(includeInactive) length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj_inactive", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      } else {
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
    } else {
      MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), cart_type);
      void* args[1] = {type_obj};
      MonoObject* exc = nullptr;
      MonoObject* arr_obj =
          g_mono.mono_runtime_invoke(g_resources_find_objects_of_type_all, nullptr, args, &exc);
      if (exc) {
        AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll threw exception");
      } else if (arr_obj) {
        MonoArray* arr = reinterpret_cast<MonoArray*>(arr_obj);
        if (arr) {
          std::ostringstream oss;
          oss << "MonoSetCartValue: Resources.FindObjectsOfTypeAll length=" << arr->max_length;
          AppendLogOnce("MonoSetCartValue_findobj_all", oss.str());
          if (arr->max_length > 0) {
            cart = static_cast<MonoObject*>(arr->vector[0]);
          }
        }
      } else {
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
        void* args[1] = {go_type_obj};
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(g_resources_find_objects_of_type_all, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll(GameObject) threw exception");
        } else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        } else {
          AppendLog("MonoSetCartValue: Resources.FindObjectsOfTypeAll(GameObject) returned null");
        }
      }

      if (!arr && g_find_objects_of_type_itemvolume_include_inactive && g_mono.mono_class_get_type &&
          g_mono.mono_type_get_object) {
        bool include_inactive = true;
        void* args[2] = {go_type_obj, &include_inactive};
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(
            g_find_objects_of_type_itemvolume_include_inactive, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject,bool) threw exception");
        } else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        } else {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject,bool) returned null");
        }
      }

      if (!arr && g_find_objects_of_type_itemvolume && g_mono.mono_class_get_type &&
          g_mono.mono_type_get_object) {
        void* args[1] = {go_type_obj};
        MonoObject* exc = nullptr;
        arr_obj = g_mono.mono_runtime_invoke(g_find_objects_of_type_itemvolume, nullptr, args, &exc);
        if (exc) {
          AppendLog("MonoSetCartValue: FindObjectsOfType(GameObject) threw exception");
        } else if (arr_obj) {
          arr = reinterpret_cast<MonoArray*>(arr_obj);
        } else {
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
        void* args[1] = {cart_type_obj};
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
      } else {
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
        } else {
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
      } else {
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
    } else {
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
        void* args[1] = {ep_type_obj};
        arr = try_invoke(g_resources_find_objects_of_type_all, args, "Resources.FindObjectsOfTypeAll(ExtractionPoint)");
      }
      if (!arr && g_find_objects_of_type_itemvolume_include_inactive) {
        bool include_inactive = true;
        void* args[2] = {ep_type_obj, &include_inactive};
        arr = try_invoke(g_find_objects_of_type_itemvolume_include_inactive, args,
                         "FindObjectsOfType(ExtractionPoint,bool)");
      }
      if (!arr && g_find_objects_of_type_itemvolume) {
        void* args[1] = {ep_type_obj};
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
      g_mono.mono_field_set_value(ep, g_extraction_point_haul_current_field, &value);
      if (g_extraction_point_haul_goal_field) {
        g_mono.mono_field_set_value(ep, g_extraction_point_haul_goal_field, &value);
      }
      AppendLog("MonoSetCartValue: wrote ExtractionPoint haul as fallback");
      return true;
    } else {
      AppendLog("MonoSetCartValue: no ExtractionPoint found for fallback");
    }
  }

  if (!cart) {
    AppendLog("MonoSetCartValue: no PhysGrabCart found (all strategies), pending");
    g_pending_cart_active = true;
    return true;
  }

  g_mono.mono_field_set_value(cart, g_phys_grab_cart_haul_field, &value);

  if (g_phys_grab_cart_set_haul_text) {
    MonoObject* exc2 = nullptr;
    g_mono.mono_runtime_invoke(g_phys_grab_cart_set_haul_text, cart, nullptr, &exc2);
  }
  g_pending_cart_active = false;
  return true;
}

// 安全遍历 List<T>：当 _items 为空或 max_length 异常时，通过枚举器逐项取值。
int EnumerateListObjects(MonoObject* list_obj, const std::function<bool(MonoObject*)>& on_elem) {
  if (!list_obj || !on_elem) return 0;
  MonoClass* cls = g_mono.mono_object_get_class(list_obj);
  if (!cls) return 0;
  // Cache per list/enumerator class to减少反射开销
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

    // 兜底：静态实例也尝试一次，避免列表异常导致全空
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
  if (g_shutting_down) return false;
  out_items.clear();
  if (!CacheManagedRefs()) {
    AppendLog("MonoListItems: CacheManagedRefs failed");
    return false;
  }
  if (!g_component_get_transform) {
    AppendLog("MonoListItems: Component::get_transform unresolved");
    return false;
  }

  MonoObject* manager = nullptr;
  if (g_item_manager_instance_field && g_item_manager_vtable) {
    g_mono.mono_field_static_get_value(
        g_item_manager_vtable, g_item_manager_instance_field, &manager);
  } else {
    AppendLog("MonoListItems: ItemManager instance field/vtable unresolved");
  }
  static bool logged_no_items = false;

  // --- 已验证补丁：直接字段 + GetAllItemVolumesInScene ---
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

  // --- 备选数据源：Populate + FindObjectsOfType<ItemVolume>() ---
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
        void* args[1] = {type_obj};
        list_obj = g_mono.mono_runtime_invoke(
            g_find_objects_of_type_itemvolume, nullptr, args, &exception);
        if (exception) {
          AppendLog("MonoListItems: FindObjectsOfType threw exception");
          list_obj = nullptr;
        } else if (list_obj) {
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
    } else {
      items = reinterpret_cast<MonoArray*>(list_obj);
      list_size = items ? static_cast<int>(items->max_length) : 0;
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
    return false;
  }

  MonoArray* items = nullptr;
  int list_size = 0;
  read_list(list_obj, items, list_size);

  // 如果拿到了对象但长度为 0，强制调用一次 GetAllItemVolumesInScene 并重复读取。
  static bool forced_get_all_once = false;
  if (list_size == 0 && manager && g_item_manager_get_all_items && !forced_get_all_once) {
    MonoObject* exc_force = nullptr;
    g_mono.mono_runtime_invoke(g_item_manager_get_all_items, manager, nullptr, &exc_force);
    if (exc_force) {
      AppendLog("MonoListItems: forced GetAllItemVolumesInScene threw exception");
    } else {
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

  // Rescue: 如果 list_size>0 但 items 为空，直接用 FindObjectsOfType 拿 Array。
  if (list_size > 0 && (!items || (items && items->max_length == 0)) &&
      g_find_objects_of_type_itemvolume && g_item_volume_class &&
      g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* iv_type = g_mono.mono_class_get_type(g_item_volume_class);
    if (iv_type) {
      MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), iv_type);
      if (type_obj) {
        void* f_args[1] = {type_obj};
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

  // 兜底：依然没有 items 就再尝试一次 FindObjectsOfType。
  if ((!items || list_size <= 0) && g_find_objects_of_type_itemvolume && g_item_volume_class &&
      g_mono.mono_class_get_type && g_mono.mono_type_get_object) {
    MonoType* iv_type = g_mono.mono_class_get_type(g_item_volume_class);
    if (iv_type) {
      MonoObject* type_obj =
          g_mono.mono_type_get_object(g_domain ? g_domain : g_mono.mono_get_root_domain(), iv_type);
      if (type_obj) {
        void* f_args[1] = {type_obj};
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

  if (!items || list_size <= 0 || (items && items->max_length == 0)) {
    int enumerated = EnumerateListObjects(
        list_obj, [&](MonoObject* item_obj) -> bool {
          if (!item_obj) return false;
          MonoObject* exception2 = nullptr;
          MonoObject* transform_obj =
              g_mono.mono_runtime_invoke(g_component_get_transform, item_obj, nullptr, &exception2);
          if (exception2 || !transform_obj) {
            return false;
          }
          PlayerState st{};
          if (TryGetPositionFromTransform(transform_obj, st) && st.has_position) {
            out_items.push_back(st);
            return true;
          }
          return false;
        });
    if (enumerated <= 0 && !logged_no_items) {
      std::ostringstream oss;
      oss << "MonoListItems: list_size=" << list_size
          << " max_length=" << (items ? items->max_length : 0) << " (enumerator fallback empty)";
      AppendLog(oss.str());
      logged_no_items = true;
    }
    return true;
  }

  static bool logged_list_count = false;
  if (!logged_list_count) {
    std::ostringstream oss;
    oss << "MonoListItems: Successfully got item list, size=" << list_size
        << " max_length=" << (items ? items->max_length : 0);
    AppendLog(oss.str());
    logged_list_count = true;
  }

  int limit = list_size;
  int max_len = (items && items->max_length > 0) ? static_cast<int>(items->max_length) : list_size;
  if (max_len > 0 && limit > max_len) {
    limit = max_len;
  }
  if (limit > 1024) {
    limit = 1024;
  }

  int valid_count = 0;
  for (int i = 0; i < limit; ++i) {
    MonoObject* item = static_cast<MonoObject*>(items->vector[i]);
    if (!item) continue;

    MonoObject* exception2 = nullptr;
    MonoObject* transform_obj =
        g_mono.mono_runtime_invoke(g_component_get_transform, item, nullptr, &exception2);
    if (exception2 || !transform_obj) {
      continue;
    }

    PlayerState st{};
    if (TryGetPositionFromTransform(transform_obj, st) && st.has_position) {
      out_items.push_back(st);
      valid_count++;
    }
  }

  if (valid_count > 0 && !logged_list_count) {
    std::ostringstream oss;
    oss << "MonoListItems: Found " << valid_count << " valid items with positions";
    AppendLog(oss.str());
  }

  return true;
}

bool MonoGetCameraMatrices(Matrix4x4& view, Matrix4x4& projection) {
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
}
