# codex-deepseek-subagent-proxy-cpp

轻量 C++ 本地代理，修复 Codex Desktop / CLI 在 **DeepSeek 等非 OpenAI 模型** 下的子代理任务正文丢失问题。

> 搜索关键词：codex deepseek subagent、spawn_agent、followup_task、send_message、encrypted_content、multi_agent_v2、Responses API、子代理收不到任务、子代理任务正文丢失、本地代理、C++ 单文件、无需 Node。

根因：Codex Multi-Agent V2 会把 `spawn_agent` / `followup_task` / `send_message` 的任务文字放进 `agent_message` 的 `encrypted_content` 字段；DeepSeek Responses 端点不认这个字段并直接丢弃，于是子代理被创建了，但只收到空的 `Payload:`。本代理在本地把 `agent_message` 改写为普通 `message(role=user)` 明文，其他请求原样转发。

特点：

- 单文件 C++（Win32 + WinHTTP），零第三方依赖，`adapter.exe` 约 230 KB，运行内存约 15-18 MB
- 只监听 `127.0.0.1`，不存储、不接触你的 API key
- 一键部署脚本：自动编译、备份并修改配置、注册隐藏自启 + 看护、启动并自检
- 本仓库不包含任何用户的 `config.toml` / `models.json` 原文，只提供修改逻辑

## 问题原因与解决思路

```text
Codex (DeepSeek 主模型)
  └─ spawn_agent / followup_task / send_message
       └─ Codex Runtime 把任务封装成 agent_message + encrypted_content
            └─ 本地适配器 127.0.0.1:8787
                 ├─ 检测到 agent_message → 改写为明文 user message
                 └─ 转发到 https://api.deepseek.com (wire_api=responses)
                      └─ DeepSeek 子代理正常收到任务正文
```

## 目录

| 文件 | 作用 |
| --- | --- |
| `adapter.cpp` | 代理本体（proxy / watchdog / selftest / test 四种模式） |
| `build.ps1` | 用 MSVC 编译 `adapter.exe` |
| `setup.ps1` | 一键部署（推荐入口） |
| `uninstall.ps1` | 卸载并恢复配置备份 |
| `start.ps1` | 只负责启动适配器 |
| `tests/sample-agent-message.json` | 重写逻辑的测试样例 |

运行时还会生成两个只读日志：`adapter.log`（请求/改写记录）和 `tasks.log`（每一条派发给子代理的**明文任务全文**，便于核对主代理实际派发了什么任务）。

## 前置条件

1. Windows 10/11
2. 已经按 DeepSeek 官方文档把 Codex 主模型配为 DeepSeek：顶层 `model = "deepseek-..."`、`model_provider = "deepseek"`，`config.toml` 里有 `[model_providers.deepseek]` 段（含 `wire_api = "responses"` 和 API key）
3. 编译需要 VS Build Tools（“使用 C++ 的桌面开发”工作负载）；不想编译的话，从 Releases 下载最新 adapter 二进制（如 `adapter-v1.2.0.exe`），**改名为 `adapter.exe`** 放到本目录，再用 `setup.ps1 -SkipBuild`

## 一键部署

```powershell
powershell -ExecutionPolicy Bypass -File setup.ps1
```

脚本依次完成（每一步都会打印结果，可先加 `-DryRun` 预览）：

1. 若没有 `adapter.exe`，自动调用 `build.ps1` 编译
2. 备份 `~/.codex/config.toml`，只把 `[model_providers.deepseek]` 里的 `base_url` 改为 `http://127.0.0.1:8787/`，其余内容（包括 API key）原样保留
3. 备份 `~/.codex/models.json`，确保 deepseek 模型为 `multi_agent_version = "v2"`（否则桌面版不会注入 subagent 工具）
4. 注册登录自启（隐藏方式）和每分钟看护计划任务（无窗口），适配器崩溃后 1 分钟内自动拉起
5. 启动适配器并自检：本地 8787 端口监听 + `https://api.deepseek.com` 可达

部署后建议：**完全退出 Codex（所有窗口）→ 重新打开 → 新建任务**，确保 `models.json`、subagent 工具注入等改动完整生效。注：`base_url` 这类配置改动对旧会话通常也会生效，不必重启即可切换。

## 验证（smoke test）

在新任务里让主 Agent 派发一个不依赖上下文的子代理任务：

```text
请直接调用 spawn_agent，传入子代理的完整任务只有：
TOKEN=DS_OK_12345
只原样返回这个 TOKEN，不要解释。
```

然后检查 `adapter.log`，出现下面这行即代表链路打通：

```text
rewrote agent message: POST /responses
```

也可随时手动自检：

```powershell
adapter.exe selftest
```

结果写在 `selftest.log`：本地端口监听 + 上游可达即 OK。

## 任务可见性

Codex V2 的子代理任务在界面上显示为空 `Payload`，是因为任务正文被封装进了 `encrypted_content`，界面不渲染。适配器会在改写时把明文任务追加到本目录的 `tasks.log`，格式为：

```text
2026-08-12T12:00:00.000 task to <子代理任务名>:
Message Type: NEW_TASK
Task name: <子代理任务名>
Sender: <父代理名>
Payload:

<这里就是完整的任务正文>
---
```

用任何文本编辑器打开即可看到主代理发给子代理的完整输入。

## 给 Codex Agent 的部署说明

使用者可以让自己的 Codex Agent 直接执行（逻辑都在本仓库内，Agent 可读取）：

1. 先 `setup.ps1 -DryRun` 预览改动
2. 执行 `setup.ps1`
3. 检查 `selftest.log` / `adapter.log`
4. 提醒用户完全退出并重启 Codex、新建任务
5. 派发 marker 子代理任务验证 `rewrote agent message` 是否出现

注：`base_url` 改动对旧会话通常也会生效；完全重启主要用于确保 `models.json`（subagent 工具注入）等改动完整生效。

更详细的约束见 [AGENTS.md](AGENTS.md)。

## 卸载

```powershell
powershell -ExecutionPolicy Bypass -File uninstall.ps1
```

默认会停掉本目录启动的适配器、移除自启和看护任务，并从备份恢复 `config.toml` / `models.json`。加 `-KeepConfig` 则只停服务、不改配置。

## 参数

| 脚本 | 参数 | 说明 |
| --- | --- | --- |
| `setup.ps1` | `-DryRun` | 只打印将要做的改动，不写任何文件 |
| | `-SkipBuild` | 使用已有的 `adapter.exe`，不编译 |
| | `-NoAutostart` | 不注册自启和看护任务 |
| | `-CodexHome <path>` | 指定 `~/.codex` 位置（默认 `$env:USERPROFILE\.codex`） |
| `uninstall.ps1` | `-KeepConfig` | 不恢复配置，只停服务 |
| `adapter.exe` | `[port]` | 默认 8787，可指定其他端口（如 `adapter.exe 8799`） |
| | `watchdog [port]` | 看护模式：端口未监听则启动代理 |
| | `selftest [port]` | 自检：本地端口 + 上游可达性，写入 `selftest.log` |
| | `test in.json out.json` | 离线测试重写逻辑，不联网 |

## 安全说明

- 仓库里没有任何用户的 `config.toml` / `models.json` 原文、API key 或私密路径；脚本只在运行者自己的机器上读写这些文件
- 适配器仅绑定 `127.0.0.1`，仅转发到 `https://api.deepseek.com`，不经过任何第三方
- 只改写 `agent_message` / `encrypted_content`，其余请求原样转发
- `setup.ps1` / `uninstall.ps1` 都先备份再修改，可用 `-DryRun` 预览

## 常见问题

- **端口被占用**：默认 8787；改 `adapter.cpp` 里的默认端口并重编译，或启动时传 `adapter.exe 8799`（同时把 `setup.ps1` 里的 `$adapterUrl` 同步改成对应端口）
- **没有编译环境**：从 Releases 下载最新 adapter 二进制，改名为 `adapter.exe` 放到本目录，然后 `setup.ps1 -SkipBuild`
- **子代理还是收不到任务**：确认 `adapter.log` 有 `rewrote agent message`；确认完全退出并重启了 Codex；确认用的是新建任务
- **会弹窗吗**：不会。适配器和看护都是无窗口的 GUI 子系统程序，自启用隐藏 VBS

## 参考与致谢

- [openai/codex#36586](https://github.com/openai/codex/issues/36586)：`encrypted_content` 被非 OpenAI provider 丢弃的官方 issue
- [openai/codex#37237](https://github.com/openai/codex/issues/37237)、[#36387](https://github.com/openai/codex/issues/36387)：同类复现
- [hairyf/codex-deepseek-subagent-proxy](https://github.com/hairyf/codex-deepseek-subagent-proxy)：Node 版同思路实现
- [CCanxue/codex-deepseek-subagent-fix](https://github.com/CCanxue/codex-deepseek-subagent-fix)、[asnndpro/Codex-DeepSeek-Subagent](https://github.com/asnndpro/Codex-DeepSeek-Subagent)：补丁版与复现材料

## License

MIT
