# RepoDLL 说明（联机 / 同步 / 稳定性）

## 1. 这个游戏的联机通信方式
- 联机框架是 `Photon PUN`（`Photon.Pun` + `Photon.Realtime`）。
- Session 本质是 Photon Room。
- 核心同步通过：
  - `PhotonView.RPC(...)`
  - `IPunObservable.OnPhotonSerializeView(...)`
- 权限判断由 `SemiFunc` 系列方法统一控制（`IsMasterClient*`, `MasterOnlyRPC`, `OwnerOnlyRPC` 等）。

## 2. 为什么“本地强制变房主”无法真正全局同步
- 本地 patch 只能修改本进程判定，不能改变房间内其他客户端/服务端的权威状态。
- Photon 的真实房主（`MasterClient`）不是靠本地 bool 改出来的，需要网络层真实切主成功。
- 当强制把 `OwnerOnlyRPC/MasterOnlyRPC` 一类检查改成恒 true 时，会破坏原本的所有权约束，常见后果：
  - 抓取状态错乱
  - 物体交互异常
  - 看起来“本地成功”，但队友端不同步

## 3. 本次修复与策略调整
- `Session master` 功能已默认停用（稳定性优先）。
- 不再使用高风险的 RPC 权限强制路径来“伪装房主”。
- 抓力修改改为本地字段路径优先，避免触发联机所有权冲突。
- 场景切换/回主菜单期间，减少高频写入与相机矩阵拉取，降低崩溃概率。
- 退出卸载时会主动恢复残留 patch，避免脏状态带出。

## 4. 已知限制
- 非真实房主情况下，涉及权威端写入的功能（尤其网络物理/抓取/部分货币链路）仍可能被房主状态覆盖。
- 这属于联机模型限制，不是单个函数名 patch 就能彻底解决。

## 5. 日志排查建议
- 日志目录：`C:\Users\dwgx1\AppData\LocalLow\semiwork\Repo\repodll`
- 重点看：
  - `SessionTransitionGuard: ...`
  - `MonoGetLocalPlayer ...`
  - `MonoGetCameraMatrices ...`
  - `MonoSetGrabStrength ...`

如果后续要恢复“会话主人”实验功能，建议单独开测试分支，不要在日常联机版本启用。
