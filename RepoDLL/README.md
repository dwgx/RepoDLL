# RepoDLL

基于 DirectX11 + ImGui 的游戏覆盖层 DLL，通过 Mono 反射与游戏交互。

## 功能
- DX11 Present/WndProc 钩子，创建 ImGui 覆盖层。
- 读取/设置本地玩家状态：位置、生命、体力、速度、跳跃、抓取等。
- 货币/推车价值写入，自动刷新相关 UI。
- 可选的卸载清理：释放 DX11 资源、恢复窗口过程、移除 MinHook。

## 构建
- 环境：Visual Studio 2022（MSVC 14.3+），x64。
- 依赖：项目自带 MinHook、ImGui（third_party）。
- 配置：打开 `RepoDLL.slnx`，选择 Debug/Release x64 编译生成 `x64/Debug|Release/RepoDLL.dll`。

## 使用
1. 将 DLL 注入目标进程（确保游戏为 DX11）。
2. Insert 键切换菜单。
3. 日志输出默认写入 `D:\Project\REPO_LOG.txt`，如需通用路径，可修改 `mono_bridge.cpp`/`dllmain.cpp` 的日志路径。

## 目录
- `dllmain.cpp`：入口与线程创建。
- `hook_dx11.*`：DXGI Present 钩子、ImGui 初始化/清理。
- `mono_bridge.*`：Mono 反射/字段解析、状态读写。
- `ui.*`：ImGui 覆盖层 UI。
- `third_party/`：MinHook、ImGui。
- `docs/`：功能点说明。

## 注意
- 日志路径可根据实际环境修改。
- 部分功能依赖游戏版本的托管字段/方法名，更新后需重新确认。
- 卸载时调用 `UnhookDx11` 可恢复窗口过程并释放 DX11 资源。
