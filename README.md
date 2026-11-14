# OmniDesk Plugin #1 — `plugin-magpie-agent`

Magpie AI Agent Router (`plugin-magpie-agent`) connects **OmniDesk** to the local [Magpie](https://github.com/usemagpie) multi-provider AI gateway and group router via OmniDesk Plugin C ABI v1.

## Capabilities Provided

| Capability | Purpose | Default Backing |
|---|---|---|
| `llm.router.v1` | Read Magpie gateway status, detected harnesses (read-only), models, routing groups, quotas; write OmniDesk-only route group & emit `entity.ai.route_changed` | `~/.config/magpie/gateway.json`, `/v1/models`, `/v1/magpie/quotas`, `magpie` CLI |
| `ai.agent.v1` | Domain-aware companion assistant (`chat`, `summarize`, `translate`, `etymology`, `clause_parse`) across `core`, `media`, `text`, `book` | `POST /v1/chat/completions` (`model = "group/omnidesk-active"` or `route_override`) |
| `llm.chat.v1` | OpenAI-compatible multi-turn completions, `reasoning_effort`, and `response_format` JSON extraction | `POST /v1/chat/completions` |
| `llm.translate.v1` | Context-window & mandatory glossary translation preserving formatting | `POST /v1/chat/completions` |

## Architecture & Isolation Guarantees

- **Dedicated Route Group Isolation**: OmniDesk route selections are pushed exclusively to a dedicated Magpie routing group (`group/omnidesk-active` by default) via `magpie group add/set/pick`. Global terminal agent configurations (`codex`, `claude`, `pi`, `antigravity-cli`, etc.) are **read-only** via `magpie ls` and are never modified.
- **No Proxy Hijacking**: Loopback gateway requests (`127.0.0.1`) set `CURLOPT_NOPROXY = "*"` so desktop proxy settings do not break local gateway calls.
- **Non-Blocking Lifecycle**: `onLoad()` reads local configuration and `gateway.json` in milliseconds without blocking UI startup; UI dock/settings refreshes run asynchronously and join cleanly on `onUnload()`.
- **Secret Hygiene**: Optional `gateway_key` is read solely from OmniDesk `SecretVault` and redacted from logs and error payloads.

## Build & Test

```bash
cmake -S . -B build -DOmniDeskSDK_DIR=/tmp/omnidesk-build/sdk-cmake
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Install into OmniDesk

```bash
omnidesk-pm install /path/to/omnidesk-plugin-magpie-agent
omnidesk-pm verify plugin-magpie-agent
```
