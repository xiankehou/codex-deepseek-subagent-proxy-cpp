# 给 Codex Agent 的部署说明（AGENTS.md）

本仓库用于修复 DeepSeek 等非 OpenAI 模型下，Codex Multi-Agent V2 子代理收不到任务正文的问题。

## 问题原因

Codex V2 把 `spawn_agent` / `followup_task` / `send_message` 的任务文字封装进 `agent_message` 的 `encrypted_content`；DeepSeek Responses 端点不认该字段并直接丢弃，于是子代理被创建，但只收到空的 `Payload`。

## 解决思路

本地代理（`adapter.exe`）监听 `127.0.0.1:8787`，把 `agent_message` 改写成普通明文 `message(role=user)` 后转发到 `https://api.deepseek.com`，其余请求原样转发。

## 方案（一键部署）

1. 预览：`powershell -ExecutionPolicy Bypass -File setup.ps1 -DryRun`
2. 执行：`powershell -ExecutionPolicy Bypass -File setup.ps1`
3. 检查 `selftest.log` 和 `adapter.log`
4. 提醒用户完全退出 Codex（所有窗口）→ 重新打开 → 新建任务
5. 派发 marker 子代理任务做 smoke test，确认 `adapter.log` 出现 `rewrote agent message: POST /responses`

注：`base_url` 改动对旧会话通常也会生效；重启主要用于确保 models.json / 工具注入等改动完整生效。

详细参数与手动步骤见 README.md。

## 注意事项

- 修改配置前会先备份；建议先用 `-DryRun` 预览
- 适配器无窗口；`tasks.log` 记录了派发给子代理的明文任务，便于核对
- 排障速查：

| 现象 | 排查 |
| --- | --- |
| `selftest.log` 显示 `not_listening` | 适配器没起来，看 `adapter.log` / 手动 `start.ps1` |
| `selftest.log` 显示 upstream 不可达 | 网络/代理问题，与适配器无关 |
| 子代理启动但收不到任务 | 看 `adapter.log` 有没有 `rewrote agent message`；确认新任务 + 完全重启 |
| 完全没 subagent 工具 | 确认 models.json 中 deepseek 模型为 `multi_agent_version` v2，并重启 Codex |
