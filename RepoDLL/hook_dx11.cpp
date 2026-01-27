#include "pch.h"

#include "hook_dx11.h"

#include <d3d11.h>
#include <dxgi.h>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "mono_bridge.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

PresentFn g_original_present = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_original_wndproc = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
void* g_present_fn = nullptr;
bool g_imgui_initialized = false;
bool g_menu_open = true;
bool g_overlay_disabled = false;
bool g_unhooked = false;

void LogMessage(const std::string& msg) {
  std::ofstream f("D:\\Project\\REPO_LOG.txt", std::ios::app);
  if (!f) return;
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm_local{};
  localtime_s(&tm_local, &t);
  f << "[" << std::put_time(&tm_local, "%F %T") << "." << std::setw(3) << std::setfill('0')
    << ms.count() << "] " << msg << "\n";
}

void SetupFonts(ImGuiIO& io) {
  io.Fonts->Clear();
  const char* font_path = "C:\\Windows\\Fonts\\msyh.ttc";  // 微软雅黑，覆盖中文显示
  ImFontConfig cfg;
  cfg.PixelSnapH = true;
  if (!io.Fonts->AddFontFromFileTTF(font_path, 18.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull())) {
    io.Fonts->AddFontDefault();  // 回退到默认字体
  }
  io.Fonts->Build();
}

void CreateRenderTarget(IDXGISwapChain* swap_chain) {
  ID3D11Texture2D* back_buffer = nullptr;
  if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) && back_buffer) {
    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
    back_buffer->Release();
  }
}

void CleanupRenderTarget() {
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
}

LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_KEYUP && wparam == VK_INSERT) {
    g_menu_open = !g_menu_open;
    return 0;
  }
  if (MonoIsShuttingDown()) {
    return CallWindowProc(g_original_wndproc, hwnd, msg, wparam, lparam);
  }

  if (g_menu_open) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
      return 1;
    }
    // Only swallow inputs when ImGui wants them, so game clicks still pass through.
    const bool want_mouse = io.WantCaptureMouse;
    const bool want_keyboard = io.WantCaptureKeyboard || io.WantTextInput;
    if (want_mouse) {
      switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
          return 1;
        default:
          break;
      }
    }
    if (want_keyboard) {
      switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
          return 1;
        default:
          break;
      }
    }
  }

  return CallWindowProc(g_original_wndproc, hwnd, msg, wparam, lparam);
}

HRESULT __stdcall HookPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
  if (!g_original_present) {
    LogMessage("HookPresent: original_present null, skipping");
    return DXGI_ERROR_INVALID_CALL;
  }
  static bool logged_present_once = false;
  if (!logged_present_once) {
    LogMessage("HookPresent: entered");
    logged_present_once = true;
  }
  if (MonoIsShuttingDown()) {
    return g_original_present(swap_chain, sync_interval, flags);
  }
  if (!g_imgui_initialized) {
    if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                                        reinterpret_cast<void**>(&g_device))) &&
        g_device) {
      g_device->GetImmediateContext(&g_context);
      DXGI_SWAP_CHAIN_DESC desc = {};
      swap_chain->GetDesc(&desc);
      g_hwnd = desc.OutputWindow;
      g_original_wndproc = reinterpret_cast<WNDPROC>(
          SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));

      CreateRenderTarget(swap_chain);

      ImGui::CreateContext();
      ImGuiIO& io = ImGui::GetIO();
      SetupFonts(io);
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
      ImGui::StyleColorsDark();
      ImGui_ImplWin32_Init(g_hwnd);
      ImGui_ImplDX11_Init(g_device, g_context);

      g_imgui_initialized = true;
    }
  }

  if (g_imgui_initialized) {
    if (g_overlay_disabled) {
      return g_original_present(swap_chain, sync_interval, flags);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 用于异常追踪的阶段标记
    const char* stage = "begin";
    try {
      stage = "RenderOverlay";
      RenderOverlay(&g_menu_open);

      stage = "ImGui::Render";
      ImGui::Render();
    } catch (...) {
      ImGuiContext* ctx = ImGui::GetCurrentContext();
      if (ctx && ctx->WithinFrameScope) {
        ImGui::EndFrame();
      }
      g_overlay_disabled = true;
      ImGui_ImplDX11_Shutdown();
      ImGui_ImplWin32_Shutdown();
      ImGui::DestroyContext();
      g_imgui_initialized = false;
      CleanupRenderTarget();
      std::string err = std::string("HookPresent: exception at stage ") + stage;
      LogMessage(err);
      return g_original_present(swap_chain, sync_interval, flags);
    }

    if (!g_rtv) {
      CreateRenderTarget(swap_chain);
    }
    if (g_rtv) {
      g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }

  return g_original_present(swap_chain, sync_interval, flags);
}

bool CreateDx11Hook() {
  LogMessage("CreateDx11Hook: begin");
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"RepoDLL_DX11";

  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogMessage("CreateDx11Hook: RegisterClassExW failed");
    return false;
  }

  HWND hwnd = CreateWindowW(wc.lpszClassName, L"RepoDLL_DX11", WS_OVERLAPPEDWINDOW,
                            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    LogMessage("CreateDx11Hook: CreateWindowW failed");
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 1;
  sd.BufferDesc.Width = 2;
  sd.BufferDesc.Height = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swap_chain = nullptr;
  D3D_FEATURE_LEVEL feature_level;

  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             nullptr, 0, D3D11_SDK_VERSION, &sd, &swap_chain,
                                             &device, &feature_level, &context);
  if (FAILED(hr)) {
    std::ostringstream oss;
    oss << "CreateDx11Hook: D3D11CreateDeviceAndSwapChain failed hr=0x" << std::hex << hr;
    LogMessage(oss.str());
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  void** vtable = *reinterpret_cast<void***>(swap_chain);
  void* present = vtable[8];
  if (!present) {
    LogMessage("CreateDx11Hook: present vtable null");
  }
  g_present_fn = present;

  swap_chain->Release();
  context->Release();
  device->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);

  if (MH_Initialize() != MH_OK) {
    LogMessage("CreateDx11Hook: MH_Initialize failed");
    return false;
  }

  if (MH_CreateHook(present, HookPresent, reinterpret_cast<void**>(&g_original_present)) !=
      MH_OK) {
    LogMessage("CreateDx11Hook: MH_CreateHook failed");
    MH_Uninitialize();
    return false;
  }

  if (MH_EnableHook(present) != MH_OK) {
    LogMessage("CreateDx11Hook: MH_EnableHook failed");
    MH_RemoveHook(present);
    MH_Uninitialize();
    return false;
  }

  LogMessage("CreateDx11Hook: success");
  return true;
}
}  // namespace

bool HookDx11() {
  LogMessage("HookDx11: initializing");
  if (!CreateDx11Hook()) {
    LogMessage("HookDx11: CreateDx11Hook failed");
    return false;
  }
  LogMessage("HookDx11: hook installed");
  return true;
}

void UnhookDx11() {
  if (g_unhooked) return;
  g_unhooked = true;

  LogMessage("UnhookDx11: begin");

  if (g_imgui_initialized) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui_initialized = false;
  }

  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
  if (g_context) {
    g_context->Release();
    g_context = nullptr;
  }
  if (g_device) {
    g_device->Release();
    g_device = nullptr;
  }

  if (g_hwnd && g_original_wndproc) {
    SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
    g_original_wndproc = nullptr;
    g_hwnd = nullptr;
  }

  if (g_present_fn && g_original_present) {
    MH_DisableHook(g_present_fn);
    MH_RemoveHook(g_present_fn);
    g_present_fn = nullptr;
  }
  MH_Uninitialize();

  g_original_present = nullptr;
  LogMessage("UnhookDx11: done");
}
