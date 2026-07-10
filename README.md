# RepoDLL

**DirectX 11 / ImGui 游戏覆盖层注入 DLL — Unity/Mono 游戏内存修改与 ESP 工具**

A DirectX 11 overlay DLL that hooks into Unity/Mono games, providing real-time memory editing, ESP rendering, and player tweaks through an ImGui interface.

一个注入 Unity/Mono 游戏进程的 DX11 覆盖层 DLL，通过 ImGui 界面提供实时内存读写、ESP 透视和玩家属性修改功能。早期逆向实验项目，留着当成长记录。

---

## Overview / 概述

RepoDLL is a Windows DLL that injects into a Unity/Mono game process, hooks the DirectX 11 swap chain, and renders a Dear ImGui overlay on top of the game. It bridges into the Mono runtime to inspect and modify managed game objects at runtime — reading and writing player state, item data, enemy positions, and economy values.

RepoDLL 是一个 Windows 动态链接库，注入到 Unity/Mono 游戏进程后，hook DirectX 11 交换链并在游戏画面上渲染 Dear ImGui 覆盖层。通过 Mono 运行时桥接，运行时解析和修改托管游戏对象——读写玩家状态、物品数据、敌人位置和经济数值。

The Mono class/field names in the code (`GameDirector`, `SemiFunc`, `PlayerAvatar`, `PhysGrabber`, `PhysGrabCart`, `ExtractionPoint`, `PunManager`) reference a specific Unity game built on `Assembly-CSharp`.

<!-- TODO: confirm the exact target game/version this was written against. -->

---

## Features / 功能

### Hook & Overlay / Hook 与覆盖层

- DirectX 11 Present/ResizeBuffers hooking via MinHook
- ImGui overlay rendered inside the game each frame
- Toggle key: `Insert` (rebindable at runtime)
- WndProc subclassing for input capture

### Mono Bridge / Mono 桥接

- Runtime resolution of managed classes, fields, and methods from `Assembly-CSharp`
- SEH-guarded wrappers to reduce crashes on the render thread
- Method scanning by keyword, field dumping, log capture

### ESP / 透视

- World-to-screen rendering for players, items/valuables, and enemies
- Camera view/projection matrices pulled from Unity's `Camera`
- Native in-game highlight hooks (`ValuableDiscover`) with optional persistence

### Player Tweaks / 玩家修改

- Position, health/max health, energy (stamina) read/write
- Movement speed override
- Extra jump, jump cooldown/force adjustment
- Invincibility
- Grab range/strength modification
- Third-person camera toggle (`F6`)

### Economy & Session / 经济与会话

- Run currency get/set
- Cart value, round/haul progress state
- Session/master-client gating for conditional reads/mutations

### Diagnostics / 诊断

- Method scanning by keyword
- Field dumping
- Runtime log with configurable path

---

## Tech Stack / 技术栈

| Component | Detail |
|-----------|--------|
| Language | C++20 (`stdcpp20`), C sources for MinHook |
| Build | MSBuild / Visual Studio (`RepoDLL.slnx`) |
| Toolset | PlatformToolset v145, Windows SDK 10.0 |
| Configs | Debug/Release × x64/Win32 |
| Graphics | `d3d11.lib`, `dxgi.lib` |
| ImGui | Dear ImGui (vendored, DX11 + Win32 backends) |
| Hooking | MinHook (vendored) |
| License | MIT |

---

## Project Structure / 项目结构

```
RepoDLL/
├─ RepoDLL.slnx                 # Visual Studio solution
├─ LICENSE                      # MIT
└─ RepoDLL/
   ├─ RepoDLL.vcxproj           # MSBuild project (DLL output)
   ├─ dllmain.cpp               # DLL entry; spawns MainThread, installs hooks
   ├─ hook_dx11.cpp / .h        # DX11 Present/ResizeBuffers hooks, WndProc, ImGui init, WorldToScreen
   ├─ mono_bridge.cpp / .h      # Mono runtime bridge: resolve classes/fields/methods, read/write state
   ├─ ui.cpp / .h               # ImGui overlay UI, ESP caches, settings
   ├─ config.h                  # Target Mono class/field/method name constants
   ├─ framework.h / pch.h/.cpp  # Windows/precompiled-header plumbing
   └─ third_party/
      ├─ imgui/                 # Dear ImGui + dx11/win32 backends
      └─ minhook/               # MinHook hooking library
```

---

## How It Works / 工作原理

1. `DllMain` on `DLL_PROCESS_ATTACH` installs an unhandled-exception filter and starts `MainThread`.
2. `MainThread` calls `HookDx11()`, using MinHook to hook DXGI swap chain's `Present` and `ResizeBuffers`.
3. Inside hooked `Present`, ImGui initializes against the game's D3D11 device; the overlay renders each frame. Game window's `WndProc` is subclassed for input.
4. `mono_bridge.cpp` locates the Mono runtime, resolves managed types/fields/methods from `config.h`, reads/writes game state. `ui.cpp` renders values and edit controls.
5. On `DLL_PROCESS_DETACH`, `MonoBeginShutdown()` and `UnhookDx11()` clean up.

---

## Getting Started / 快速开始

### Prerequisites / 前置条件

- Windows, DirectX 11 capable GPU
- Visual Studio 2022+ with **Desktop development with C++** workload and Windows 10/11 SDK (PlatformToolset v145)

<!-- TODO: confirm the minimum Visual Studio version that ships PlatformToolset v145. -->

### Build / 构建

1. Open `RepoDLL.slnx` in Visual Studio.
2. Select configuration/platform (e.g. `Release | x64`).
3. Build. Output: `RepoDLL.dll`.

ImGui and MinHook are vendored — no package restore needed.

### Run / 运行

This builds a DLL. You inject it into the target game process. The injection method is not included in this repo.

这个项目编译出一个 DLL，你需要自行注入到目标游戏进程。注入器不包含在本仓库中。

- Overlay toggle: `Insert`
- Third-person toggle: `F6`

<!-- TODO: confirm the intended injection method, as it is not part of this repo. -->

---

## Configuration / 配置

- **Target names / 目标名称:** `config.h` holds managed class, field, and method names (e.g. `kAssemblyName = "Assembly-CSharp"`). Adjust if targeting a different build.
- **Log file / 日志:** Runtime log path via `MonoGetLogPath()` / `MonoSetLogPath()`.
- **Runtime toggles / 运行时开关:** ESP options, auto-refresh, toggle keys — all managed in the ImGui UI.

---

## Status / 状态

Legacy experimental project. 早期逆向实验，代码保留原样作为技术研究记录。功能不完整，有硬编码的目标假设，可能需要适配才能在你的环境里跑起来。

---

## License / 许可证

MIT License. Copyright (c) 2026 dwgx. See [LICENSE](./LICENSE).

Vendored dependencies (Dear ImGui, MinHook) under their own respective licenses.
