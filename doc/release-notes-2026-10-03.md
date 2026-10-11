# mirv_pov — 2026-10-03

[English](#english) | [简体中文](#简体中文)

## English

Compatibility update for the October CS2 client update, based on HLAE 2.192.6.

### Changes

- Update the flash-render signature while retaining validation that both native render calls resolve to the same predicate.
- Update the player-voice handler signature for the new native function layout.
- Refresh the recorded buy-menu adapter, including its build identity, native entry points, UI globals and the verified `EventOpenBuyMenu` creator. Exact-build and schema checks remain enabled.
- Update the weapon pickup-prompt builder and caller signatures, retaining their native call-target validation.

### How to use

1. Install a complete official **HLAE 2.192.6** installation.
2. Download this release's **`AfxHookSource2-<commit>-windows-x64.zip`**. The matching `*-source.zip` is source code.
3. Close CS2 and back up `<HLAE>/x64/AfxHookSource2.dll`. Extract the binary ZIP into `<HLAE>/x64/`, replacing the DLL and retaining the bundled licenses. Keep the rest of HLAE, including its voice script. Do not extract into the CS2 directory.
4. Launch CS2 through HLAE, load a local demo, select the desired player's first-person view and enter `mirv_pov 1`.

Buy-menu replay is optional and disabled by default; enable it with `mirv_pov_buymenu 1`. Run `mirv_pov_debug_feature` to list independent effect controls. See the [POV guide](https://github.com/WangChuDi/advancedfx/blob/mirv-pov-20261003/README.md) for the other commands and defaults.

### Validation

Release x64 build and whitespace checks passed. All 82 POV source signatures match the installed modules; targeted static checks cover the flash call targets, voice message fields, buy-menu build/schema and event identity, and pickup-prompt call target. In-game playback and effect-toggle testing for this client update remain unverified.

The verified client.dll SHA-256 is `d7db25d48f1d10c5e0b0296e20ed803426eb9509da41760daeda39dd35ba89b9`. Future CS2 updates may require further adaptation.

### Corresponding source

Download the matching `AfxHookSource2-<commit>-source.zip` for corresponding source. The combined DLL containing `mirv_pov` is distributed under **AGPL-3.0-only**; separate AdvancedFX and third-party sources retain their original licenses.

## 简体中文

基于 HLAE 2.192.6，适配本次 10 月 CS2 客户端更新。

### 更新内容

- 更新闪光渲染特征码，保留两个原生调用点指向同一判定函数的校验。
- 更新玩家语音处理函数特征码，适配新的原生函数布局。
- 更新购买菜单的构建身份、原生入口、UI 全局地址和经事件注册表核验的打开事件创建函数，继续保留完整哈希与 schema 检查。
- 更新武器拾取提示的构建函数及调用点特征码，保留原生调用目标校验。

### 使用方法

安装完整的官方 **HLAE 2.192.6**，下载本 release 的 **`AfxHookSource2-<commit>-windows-x64.zip`**。关闭 CS2，备份 `<HLAE>/x64/AfxHookSource2.dll`，将二进制 ZIP 解压到 `<HLAE>/x64/` 替换 DLL，并保留随附许可证及 HLAE 其他文件。不要解压到 CS2 游戏目录。

通过 HLAE 启动 CS2，加载本地 Demo，切换至目标玩家的第一人称视角，执行 `mirv_pov 1`。购买菜单回放默认关闭，可用 `mirv_pov_buymenu 1` 开启；`mirv_pov_debug_feature` 可列出独立效果开关。其他命令见[中文说明](https://github.com/WangChuDi/advancedfx/blob/mirv-pov-20261003/README.zh-CN.md)。

### 验证范围

Release x64 编译及空白检查通过，82 条 POV 特征码均在当前安装模块中匹配，闪光调用关系、玩家语音字段、购买菜单构建/schema/事件身份和拾取提示调用目标的专项静态检查通过。本次客户端更新后的游戏内回放及效果开关测试尚未验证。

对应 client.dll SHA-256 为 `d7db25d48f1d10c5e0b0296e20ed803426eb9509da41760daeda39dd35ba89b9`；后续 CS2 更新可能需要重新适配。对应 `*-source.zip` 提供源码，含 `mirv_pov` 的组合 DLL 按 **AGPL-3.0-only** 分发，其他源码保留各自许可证。
