# RepoDLL 使用说明

## 1. 项目说明
- 本项目为 RepoDLL 注入模块与 UI 工具集。

## 2. 作者声明 / 免责声明
- 本仓库由我维护与整理，但原始历史源码来源复杂，原始作者信息并不完整。
- 原始源码版权不归我所有，与我个人无关；如涉及侵权请联系处理，我会配合删除相关内容。
- 本项目仅作学习与研究用途，使用行为及后果由使用者自行承担。

## 3. 快速使用
1. 使用 `x64` 配置编译 DLL（建议 `Release`）。
2. 进入游戏后将 DLL 注入到游戏进程。
3. 默认菜单开关键为 `INS`。
4. 菜单内可通过“监听按键修改”更换菜单按键。

## 4. 功能使用
- 玩家页：位置、生命、体力、移动相关修改。
- 经济/关卡：货币与关卡收集数值相关功能。
- ESP：物品与敌人显示，默认上限为物品 `65`、敌人 `8`。
- 队友页：支持队友列表、坐标与状态查看。
- 队友操作限制：传送/拉取/改队友状态仅在你是真实房主时可用，非房主只可查看。
- 稳定性策略：`Session master` 强制功能已停用，不再提供伪房主入口。

## 5. 日志与排查
- 日志目录：`C:\Users\username\AppData\LocalLow\semiwork\Repo\repodll`
- 常看文件：`REPO_LOG.txt`、`REPO_CRASH.txt`
- 建议关注关键字：`MonoGetLocalPlayer`、`MonoGetCameraMatrices`、`MonoSetGrabStrength`、`SessionTransitionGuard`

## 附录 A：联机通信方式说明
- 本游戏联机基于 `Photon PUN`（`Photon.Pun` + `Photon.Realtime`）。
- Session 本质是 Photon Room，权威角色是 `MasterClient`。
- 核心同步接口为 `PhotonView.RPC(...)` 与 `IPunObservable.OnPhotonSerializeView(...)`。
- 结论：本地篡改判定无法等价于真实切主，因此无法保证全房间全功能同步。
