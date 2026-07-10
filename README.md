# RepoDLL

> DirectX 11 / ImGui overlay DLL for Unity/Mono games — hook, inspect, modify.
> 基于 DirectX 11 / ImGui 的游戏覆盖层 DLL，针对 Unity/Mono 游戏的注入、检视与修改工具。

## Overview / 概述

RepoDLL is a Windows DLL that injects into a Unity/Mono game process, hooks its DirectX 11 swap chain, and renders a Dear ImGui overlay on top of the game. Through a Mono runtime bridge, it resolves managed classes and fields at runtime, reads game state (player, items, enemies), and writes values back — position, health, speed, currency, you name it.

The Mono class/field names in the code (`GameDirector`, `SemiFunc`, `PlayerAvatar`, `PhysGrabber`, `PhysGrabCart`, `ExtractionPoint`, `PunManager`) point to a specific Unity game built on `Assembly-CSharp`.

<!-- TODO: confirm the exact target game/version this was written against. -->

---

RepoDLL 是一个 Windows 动态库，注入 Unity/Mono 游戏进程后 hook DirectX 11 交换链，在游戏画面上绘制 Dear ImGui 覆盖层。通过 Mono 运行时桥接，它在运行时解析托管类和字段，读取游戏状态（玩家、物品、敌人），也能直接写入修改值——坐标、血量、速度、货币，随便改。

代码中引用的 Mono 类/字段名（`GameDirector`、`SemiFunc`、`PlayerAvatar`、`PhysGrabber`、`PhysGrabCart`、`ExtractionPoint`、`PunManager`）指向一个基于 `Assembly-CSharp` 构建的特定 Unity 游戏。

<!-- TODO: 确认目标游戏/版本 -->

## Features / 功能

- **DX11 Present/ResizeBuffers hooking** — MinHook 拦截 DXGI 交换链，ImGui 直接画在游戏帧上
- **可切换的菜单覆盖层** — 默认 `Insert` 键呼出，运行时可改键
- **Mono 桥接** — 运行时从 `Assembly-CSharp` 解析托管类、字段、方法
- **玩家状态读写** — 坐标、血量/最大血量、体力
- **ESP / 世界坐标到屏幕坐标渲染** — 玩家、物品/贵重品、敌人，从 Unity Camera 拉取 View/Projection 矩阵
- **物品和敌人枚举** — SEH 保护的 "Safe" 包装器，减少渲染线程崩溃
- **原生高亮 hook** — `ValuableDiscover` 高亮，可选持久模式
- **玩家 tweaks** — 移动速度覆写、多段跳、跳跃冷却/力度、无敌、抓取范围/力度、第三人称相机
- **经济/关卡辅助** — 运行货币读写、推车价值、回合/搬运进度状态
- **房间/主机权限判断** — 根据房间和游戏状态条件性地允许读取或修改
- **诊断工具** — 按关键词扫描方法、字段 dump、日志捕获、可配置日志路径

很多功能是实验性的，带运行时检查保护。粗糙但真实，留着当成长记录。

## Tech Stack / 技术栈

| Component | Detail |
|-----------|--------|
| Language | C++ (C++20, `stdcpp20`)，MinHook 部分为 C |
| Build | MSBuild, Visual Studio solution (`RepoDLL.slnx`) |
| Toolset | PlatformToolset v145, Windows 10 SDK |
| Configs | Debug/Release x x64/Win32 |
| Graphics | `d3d11.lib`, `dxgi.lib` |
| Dependencies | Dear ImGui (DX11 + Win32 backends), MinHook — 均为 vendored |

## Project Structure / 项目结构

```
RepoDLL/
├─ RepoDLL.slnx                 # Visual Studio solution
├─ LICENSE                      # MIT
└─ RepoDLL/
   ├─ RepoDLL.vcxproj           # MSBuild project (DLL output)
   ├─ dllmain.cpp               # DLL 入口，启动 MainThread，装载 hooks
   ├─ hook_dx11.cpp / .h        # DX11 Present/ResizeBuffers hook, WndProc, ImGui 初始化, WorldToScreen
   ├─ mono_bridge.cpp / .h      # Mono 运行时桥接：解析类/字段/方法，读写游戏状态
   ├─ ui.cpp / .h               # ImGui 覆盖层 UI, ESP 缓存, 设置面板
   ├─ config.h                  # 目标 Mono 类/字段/方法名常量
   ├─ framework.h / pch.h/.cpp  # Windows/预编译头
   └─ third_party/
      ├─ imgui/                 # Dear ImGui + dx11/win32 backends
      └─ minhook/               # MinHook
```

## How It Works / 工作原理

1. `DllMain` 在 `DLL_PROCESS_ATTACH` 时安装异常过滤器，启动 `MainThread`。
2. `MainThread` 调用 `HookDx11()`，通过 MinHook 拦截 DXGI 交换链的 `Present`（和 `ResizeBuffers`）。
3. 在 hooked 的 `Present` 中初始化 ImGui，每帧绘制覆盖层；同时子类化游戏窗口的 `WndProc` 处理输入。
4. `mono_bridge.cpp` 定位 Mono 运行时，解析 `config.h` 中命名的托管类型/字段/方法，读写游戏状态；`ui.cpp` 渲染这些值和编辑控件。
5. `DLL_PROCESS_DETACH` 时调用 `MonoBeginShutdown()` 和 `UnhookDx11()` 清理。

## Getting Started / 快速开始

### Prerequisites / 前置条件

- Windows + DirectX 11 显卡
- Visual Studio 2022+，安装 **Desktop development with C++** 工作负载和 Windows 10/11 SDK（PlatformToolset v145）

<!-- TODO: confirm the minimum Visual Studio version that ships PlatformToolset v145. -->

### Build / 构建

1. 打开 `RepoDLL.slnx`
2. 选择配置/平台（如 `Release | x64`）
3. 构建解决方案，输出 `RepoDLL.dll`

ImGui 和 MinHook 已 vendored 在 `third_party/`，不需要额外的包管理器。

### Run / 运行

这个项目输出的是 DLL，需要注入到目标游戏进程中。注入器不包含在本仓库内。

<!-- TODO: confirm the intended injection method, as it is not part of this repo. -->

- 默认覆盖层切换键：`Insert`
- 默认第三人称切换键：`F6`

## Configuration / 配置

- **目标名称：** `RepoDLL/config.h` 存放桥接解析的托管类/字段/方法名（如 `kAssemblyName = "Assembly-CSharp"`）。目标游戏符号不同时改这里。
- **日志文件：** 运行时日志路径通过 `MonoGetLogPath()` / `MonoSetLogPath()` 控制。
- **运行时开关：** 覆盖层/第三人称切换键、ESP 选项、自动刷新等在 ImGui UI (`ui.cpp`) 中管理。

## Status / 状态

历史项目整理 + 技术研究记录。早期实验，功能不完整，有目标游戏特定的假设，代码可能需要适配才能在你的环境中编译运行。留着当参考。

Legacy / archival. An early experiment — incomplete features, target-specific assumptions, code that may need adaptation to build or run in your environment. Kept as reference.

## License / 许可证

MIT License. Copyright (c) 2026 dwgx. See [LICENSE](./LICENSE).

`third_party/` 下的 Dear ImGui 和 MinHook 遵循各自的许可证。
