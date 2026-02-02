// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"

#include "hook_dx11.h"
#include "mono_bridge.h"
#include "ui.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
void LogMessage(const char* tag) {
  std::string path = MonoGetLogPath();
  std::ofstream f(path, std::ios::app);
  if (!f) return;
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm_local{};
  localtime_s(&tm_local, &t);
  f << "[" << std::put_time(&tm_local, "%F %T") << "." << std::setw(3) << std::setfill('0')
    << ms.count() << "] " << (tag ? tag : "") << "\n";
}

LONG WINAPI RepoUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
  LogCrash("UnhandledException", info ? info->ExceptionRecord->ExceptionCode : 0, info);
  return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI MainThread(LPVOID module) {
  LogMessage("MainThread: begin");
  if (!HookDx11()) {
    LogMessage("MainThread: HookDx11 failed");
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
    return 0;
  }
  LogMessage("MainThread: hook installed, thread exiting");
  return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hModule);
      SetUnhandledExceptionFilter(RepoUnhandledExceptionFilter);
      HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
      if (thread) {
        CloseHandle(thread);
      } else {
        LogMessage("DllMain: CreateThread failed");
      }
      break;
    }
    case DLL_PROCESS_DETACH: {
      LogMessage("DllMain: detach");
      MonoBeginShutdown();
      UnhookDx11();
      break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    default:
      break;
  }
  return TRUE;
}
