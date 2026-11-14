#include "magpie_agent/magpie_client.hpp"
#include "magpie_agent/magpie_parser.hpp"
#include "magpie_agent/process_exec.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <omnidesk/plugin.hpp>
#include <omnidesk/plugin_abi.h>
#include <omnidesk/sdk.hpp>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define CHECK_TRUE(cond)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::cerr << "CHECK_TRUE failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                      << "\n";                                                                   \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        const auto& _va = (a);                                                                     \
        const auto& _vb = (b);                                                                     \
        if (!(_va == _vb)) {                                                                       \
            std::cerr << "CHECK_EQ failed at " << __FILE__ << ":" << __LINE__ << ": " << #a      \
                      << " (" << _va << ") != " << #b << " (" << _vb << ")\n";                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

void writeTextFile(const fs::path& p, const std::string& content, mode_t mode = 0644) {
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
    ofs.close();
    ::chmod(p.c_str(), mode);
}

std::string readTextFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// Minimal single-threaded loopback HTTP/1.1 test server with dynamic ephemeral port.
class LoopbackMockServer {
public:
    LoopbackMockServer() = default;
    ~LoopbackMockServer() { stop(); }

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct timeval tv {
            0, 100000
        };
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        running_.store(true);
        th_ = std::thread([this]() { serveLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            if (listen_fd_ >= 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            return;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (th_.joinable()) {
            th_.join();
        }
    }

    int port() const { return port_; }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::string expected_bearer = "secret-test-key-999";
    bool force_429 = false;
    nlohmann::json last_chat_body;
    std::string last_auth_header;

private:
    void serveLoop() {
        while (running_.load()) {
            sockaddr_in cli {};
            socklen_t clen = sizeof(cli);
            int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (fd < 0) {
                continue;
            }
            handleConnection(fd);
            ::close(fd);
        }
    }

    void handleConnection(int fd) {
        std::string req;
        char buf[4096];
        std::size_t header_end = std::string::npos;
        std::size_t content_length = 0;

        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            req.append(buf, static_cast<std::size_t>(n));
            if (header_end == std::string::npos) {
                header_end = req.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    std::string hdrs = req.substr(0, header_end);
                    std::istringstream iss(hdrs);
                    std::string line;
                    while (std::getline(iss, line)) {
                        std::string lower = line;
                        for (char& c : lower) {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        if (lower.rfind("content-length:", 0) == 0) {
                            content_length = static_cast<std::size_t>(
                                std::strtoul(lower.substr(15).c_str(), nullptr, 10));
                        }
                        if (lower.rfind("authorization:", 0) == 0) {
                            last_auth_header = line.substr(14);
                            while (!last_auth_header.empty() &&
                                   (last_auth_header.front() == ' ' || last_auth_header.back() == '\r')) {
                                if (last_auth_header.front() == ' ') {
                                    last_auth_header.erase(last_auth_header.begin());
                                }
                                if (!last_auth_header.empty() && last_auth_header.back() == '\r') {
                                    last_auth_header.pop_back();
                                }
                            }
                        }
                    }
                }
            }
            if (header_end != std::string::npos &&
                req.size() >= header_end + 4 + content_length) {
                break;
            }
        }

        if (header_end == std::string::npos) {
            return;
        }

        std::string first_line = req.substr(0, req.find("\r\n"));
        std::istringstream fls(first_line);
        std::string method;
        std::string path;
        fls >> method >> path;

        std::string body = req.substr(header_end + 4, content_length);
        int status = 200;
        nlohmann::json resp;

        if (!expected_bearer.empty() && last_auth_header != ("Bearer " + expected_bearer)) {
            status = 401;
            resp = {{"error", {{"message", "Invalid bearer " + last_auth_header}, {"code", "unauthorized"}}}};
        } else if (path == "/v1/models") {
            resp = {
                {"data",
                 {{{"id", "group/smart-split"},
                   {"display_name", "Smart Split"},
                   {"owned_by", "antigravity"},
                   {"reasoning", true},
                   {"reasoning_levels", {"low", "medium", "high", "xhigh"}},
                   {"service_tiers", {{{"id", "priority"}, {"name", "Fast"}}}},
                   {"context_length", 1000000},
                   {"max_output_tokens", 128000}},
                  {{"id", "codex/gpt-6-astra"},
                   {"display_name", "GPT-6-Astra"},
                   {"owned_by", "codex"},
                   {"reasoning", true},
                   {"reasoning_levels", {"low", "high", "xhigh"}},
                   {"context_length", 272000},
                   {"max_output_tokens", 128000}}}}};
        } else if (path == "/v1/magpie/quotas") {
            resp = {
                {"data",
                 {{{"provider", "codex"},
                   {"name", "Codex"},
                   {"kind", "subscription"},
                   {"plan", "plus"},
                   {"user", "tester@example.com"},
                   {"windows",
                    {{{"name", "5 hours"}, {"used", 25}, {"remaining", 75}, {"resetsAt", "2026-11-14T16:00:00Z"}}}}}}}};
        } else if (path == "/v1/chat/completions" && method == "POST") {
            if (force_429) {
                status = 429;
                resp = {{"error", {{"message", "quota limit exceeded for token " + expected_bearer}, {"code", "insufficient_quota"}}}};
            } else {
                auto parsed = nlohmann::json::parse(body, nullptr, false);
                last_chat_body = parsed;
                std::string user_prompt;
                if (parsed.contains("messages") && parsed["messages"].is_array() && !parsed["messages"].empty()) {
                    user_prompt = parsed["messages"].back().value("content", "");
                }

                if (parsed.value("stream", false)) {
                    const std::string hdr =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Connection: close\r\n\r\n";
                    if (::send(fd, hdr.data(), hdr.size(), MSG_NOSIGNAL) <= 0) {
                        return;
                    }
                    auto sendSse = [&](const std::string& line) -> bool {
                        std::string msg = "data: " + line + "\n\n";
                        return ::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) > 0;
                    };
                    if (!sendSse(R"({"id":"chatcmpl-stream-1","model":"codex/gpt-6-astra","choices":[{"index":0,"delta":{"content":"Hel"}}]})")) {
                        return;
                    }
                    if (user_prompt.find("__SLOW__") != std::string::npos) {
                        for (int i = 0; i < 40 && running_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            if (!sendSse(R"({"id":"chatcmpl-stream-1","model":"codex/gpt-6-astra","choices":[{"index":0,"delta":{"content":"."}}]})")) {
                                return;
                            }
                        }
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        if (!sendSse(R"({"id":"chatcmpl-stream-1","model":"codex/gpt-6-astra","choices":[{"index":0,"delta":{"content":"lo!"}}]})")) {
                            return;
                        }
                    }
                    if (!sendSse(R"({"id":"chatcmpl-stream-1","model":"codex/gpt-6-astra","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}})")) {
                        return;
                    }
                    sendSse("[DONE]");
                    return;
                }

                std::string reply_text = "MOCK_OK:" + parsed.value("model", "") + "|" + user_prompt;
                if (parsed.contains("response_format")) {
                    reply_text = R"({"status":"ok","summary":"structured reply"})";
                } else if (user_prompt.find("<target_text>") != std::string::npos) {
                    reply_text = "<translation>你好，世界</translation>";
                }

                resp = {
                    {"id", "chatcmpl-mock-1"},
                    {"model", parsed.value("model", "group/omnidesk-active")},
                    {"choices",
                     {{{"index", 0},
                       {"message", {{"role", "assistant"}, {"content", reply_text}}},
                       {"finish_reason", "stop"}}}},
                    {"usage", {{"prompt_tokens", 12}, {"completion_tokens", 6}, {"total_tokens", 18}}},
                };
            }
        } else {
            status = 404;
            resp = {{"error", {{"message", "unknown endpoint: " + path}}}};
        }

        std::string payload = resp.dump();
        std::ostringstream http;
        http << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Error") << "\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << payload.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << payload;
        std::string raw = http.str();
        ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread th_;
};

int testParsers() {
    using namespace magpie_agent;

    // 1. Log stripping + JSON extraction
    const std::string noisy_status = R"(2026/11/14 11:30:39 plugin: host starts with no proxy NODE_EXTRA_CA_CERTS=/etc/ssl/certs/ca-certificates.crt
2026/11/14 11:30:40 plugin [error]: @magpie-community/auth: signed out
{
  "running": true,
  "url": "http://127.0.0.1:3425",
  "addr": "127.0.0.1:3425",
  "pid": 969,
  "version": "0.1.4043",
  "started": "2026-11-14T01:19:37Z",
  "file": "/home/zhu/.config/magpie/gateway.json"
}
)";
    GatewayStatus st = parseStatusCliJson(noisy_status);
    CHECK_TRUE(st.running);
    CHECK_EQ(st.url, std::string("http://127.0.0.1:3425"));
    CHECK_EQ(st.pid, 969);
    CHECK_EQ(st.version, std::string("0.1.4043"));

    // 2. Harnesses (`magpie ls`)
    const std::string ls_sample = R"(2026/11/14 11:30:39 plugin: host starts with no proxy
  Codex             group/smart-split  ·  effort xhigh  ·  sign-in chatgpt  ~/.codex/config.toml
  Antigravity CLI   magpie/group/smart-split-classifier  ·  effort —        ~/.gemini/antigravity-cli/settings.json
  Antigravity       —  ·  effort —                                          ~/.antigravity/settings.json
  Pi                magpie/group/manual-gemini-flash  ·  thinking —         ~/.pi/agent/settings.json
  DeepSeek Harness  —  ·  thinking —                                        ~/.dsh/profiles/web/cordis.patch.yml
  ZCode             provider magpie                                         ~/.zcode/v2/config.json
)";
    auto harnesses = parseHarnessesText(ls_sample);
    CHECK_EQ(harnesses.size(), std::size_t{6});
    CHECK_EQ(harnesses[0].name, std::string("Codex"));
    CHECK_EQ(harnesses[0].model_or_group, std::string("group/smart-split"));
    CHECK_EQ(harnesses[0].effort, std::string("xhigh"));
    CHECK_EQ(harnesses[0].sign_in, std::string("chatgpt"));
    CHECK_TRUE(harnesses[0].configured);
    CHECK_EQ(harnesses[2].name, std::string("Antigravity"));
    CHECK_TRUE(!harnesses[2].configured);
    CHECK_EQ(harnesses[4].name, std::string("DeepSeek Harness"));
    CHECK_EQ(harnesses[5].name, std::string("ZCode"));
    CHECK_EQ(harnesses[5].model_or_group, std::string("provider magpie"));

    // 3. Models (`magpie models` including omitted display_name lines & footers)
    const std::string models_cli_sample = R"(2026/11/14 11:30:39 plugin: host starts with no proxy
  Routing groups
  group/smart-split               智能分流 · 同档额度优先  low/medium/high/xhigh/max/ultra
  auto/glm-5.3-flash              GLM-5.3-Flash  low/high/max
  Codex
  codex/gpt-6.1-sol               GPT-6.1-Sol  low/medium/high/xhigh/max/ultra
  ZCode
  zcode/GLM-5.3                   low/high/max
  zcode/GLM-5-Turbo               none/high
  Zhipu GLM: 11 more models in its list, not exposed (not picked): glm-4.5
    magpie provider models zhipu +<model> exposes one
  http://127.0.0.1:3425/v1
)";
    auto cli_models = parseModelsCliText(models_cli_sample);
    CHECK_EQ(cli_models.size(), std::size_t{5});
    CHECK_EQ(cli_models[0].id, std::string("group/smart-split"));
    CHECK_TRUE(cli_models[0].is_group);
    CHECK_EQ(cli_models[0].efforts.size(), std::size_t{6});
    CHECK_EQ(cli_models[3].id, std::string("zcode/GLM-5.3"));
    CHECK_EQ(cli_models[3].display_name, std::string(""));
    CHECK_EQ(cli_models[3].efforts.size(), std::size_t{3});

    // 4. Groups (`magpie groups` & `magpie group <id>`)
    const std::string groups_sample = R"(2026/11/14 11:30:39 plugin: host starts with no proxy
  智能分流 · 同档额度优先              group/smart-split               order · stays turn · 11 rules  group/quota-gemini-flash-first → group/quota-premium → group/quota-standard  ← Codex
  Claude Opus 5.5 found                auto/claude-opus-5-5            smart                          aihubmix/claude-opus-5-5 (not served: AiHubMix is off) · antigravity/claude-opus-5-5
  手动锁定 · Astra                     group/manual-astra              manual · stays session         ● codex/gpt-6-astra
)";
    auto groups = parseGroupsCliText(groups_sample);
    CHECK_EQ(groups.size(), std::size_t{3});
    CHECK_EQ(groups[0].id, std::string("smart-split"));
    CHECK_EQ(groups[0].routing, std::string("order"));
    CHECK_EQ(groups[0].stays, std::string("turn"));
    CHECK_EQ(groups[0].rules_count, 11);
    CHECK_EQ(groups[0].members.size(), std::size_t{3});
    CHECK_EQ(groups[0].used_by.size(), std::size_t{1});
    CHECK_EQ(groups[1].is_auto, true);
    CHECK_EQ(groups[1].members[0], std::string("aihubmix/claude-opus-5-5"));
    CHECK_EQ(groups[2].picked_model, std::string("codex/gpt-6-astra"));

    const std::string group_detail_sample = R"(2026/11/14 11:33:50 plugin: host starts
  手动锁定 · Astra  group/manual-astra
  routing   manual  every request to codex/gpt-6-astra
  stays     session
  models    1 codex/gpt-6-astra ●  Codex · GPT-6-Astra
            2 antigravity/claude-opus-5-5  Antigravity
)";
    auto detail = parseGroupDetailText(group_detail_sample);
    CHECK_TRUE(detail.has_value());
    CHECK_EQ(detail->id, std::string("manual-astra"));
    CHECK_EQ(detail->routing, std::string("manual"));
    CHECK_EQ(detail->stays, std::string("session"));
    CHECK_EQ(detail->picked_model, std::string("codex/gpt-6-astra"));
    CHECK_EQ(detail->members.size(), std::size_t{2});

    // 5. Quotas parsing (HTTP object wrapper & CLI array with plugin error logs)
    std::vector<std::string> warnings;
    const std::string quota_sample = R"(2026/11/14 11:30:40 plugin [error]: Antigravity signed out
{"data":[{"provider":"codex","name":"Codex","kind":"subscription","plan":"plus","user":"tester@example.com","windows":[{"name":"5 hours","used":100,"remaining":0,"resetsAt":"2026-11-14T13:01:56+08:00"}],"last":true},{"provider":"zcode","name":"ZCode","kind":"subscription","user":"ZCode","windows":[{"name":"MCP · Month","used":62,"remaining":38,"display":"62 / 100","amount":62,"limit":100}]}]})";
    auto quotas = parseQuotasJson(quota_sample, &warnings);
    CHECK_EQ(quotas.size(), std::size_t{2});
    CHECK_EQ(warnings.size(), std::size_t{1});
    CHECK_EQ(quotas[1].windows[0].amount, 62.0);
    CHECK_EQ(quotas[1].windows[0].limit, 100.0);

    return 0;
}

int testRouteBuilderAndCycles() {
    using namespace magpie_agent;

    std::string err;
    CHECK_EQ(buildMemberSpec("codex/gpt-6-astra", "high", true, "work", &err),
             std::string("codex/gpt-6-astra:high:fast#work"));
    // Vendor `:free` tag is preserved, effort & fast inserted cleanly before account
    CHECK_EQ(buildMemberSpec("openrouter/deepseek:free", "low", true, "acct1", &err),
             std::string("openrouter/deepseek:free:low:fast#acct1"));
    // Re-normalizing avoids duplicate :fast or out-of-order :effort after #account
    CHECK_EQ(buildMemberSpec("codex/gpt-6-astra:low:fast#home", "xhigh", false, "", &err),
             std::string("codex/gpt-6-astra:xhigh:fast#home"));
    // CLI flag injection rejected
    CHECK_EQ(buildMemberSpec("--help", "", false, "", &err), std::string(""));
    CHECK_TRUE(!err.empty());
    err.clear();
    CHECK_EQ(buildMemberSpec("group/smart-split", "high", true, "", &err), std::string(""));
    CHECK_TRUE(!err.empty());

    CHECK_TRUE(isValidGroupId("omnidesk-active"));
    CHECK_TRUE(!isValidGroupId("../bad"));
    CHECK_TRUE(!isValidGroupId("-flag"));

    std::vector<GroupInfo> existing;
    GroupInfo ga;
    ga.id = "group-a";
    ga.ref = "group/group-a";
    ga.members = {"group/group-b"};
    GroupInfo gb;
    gb.id = "group-b";
    gb.ref = "group/group-b";
    gb.members = {"group/omnidesk-active"};
    existing.push_back(ga);
    existing.push_back(gb);

    std::string cycle_reason;
    CHECK_TRUE(wouldCreateGroupCycle("omnidesk-active", {"group/omnidesk-active"}, existing, &cycle_reason));
    CHECK_TRUE(wouldCreateGroupCycle("omnidesk-active", {"group/group-a"}, existing, &cycle_reason));
    CHECK_TRUE(!wouldCreateGroupCycle("omnidesk-active", {"codex/gpt-6-astra"}, existing, &cycle_reason));

    return 0;
}

int testIntegrationWithPluginSo() {
    LoopbackMockServer server;
    CHECK_TRUE(server.start());

    const fs::path temp_root =
        fs::temp_directory_path() / ("omnidesk-magpie-int-" + std::to_string(::getpid()));
    fs::remove_all(temp_root);
    fs::create_directories(temp_root / "data");
    fs::create_directories(temp_root / "config" / "magpie");
    fs::create_directories(temp_root / "config" / "plugins");

    const fs::path cli_log = temp_root / "magpie_cli_calls.log";
    const fs::path group_created_marker = temp_root / "group_created.flag";
    const fs::path mock_magpie = temp_root / "mock_magpie.sh";

    std::ostringstream script;
    script << "#!/usr/bin/env bash\n"
           << "echo \"$*\" >> \"" << cli_log.string() << "\"\n"
           << "case \"$1\" in\n"
           << "  ls)\n"
           << "    echo '2026/11/14 11:30:39 plugin: ready'\n"
           << "    echo '  Codex  group/smart-split  ·  effort xhigh  ·  sign-in chatgpt  ~/.codex/config.toml'\n"
           << "    exit 0\n"
           << "    ;;\n"
           << "  models)\n"
           << "    echo '  group/smart-split  Smart Split  low/high'\n"
           << "    echo '  codex/gpt-6-astra  GPT-6-Astra  low/high/xhigh'\n"
           << "    exit 0\n"
           << "    ;;\n"
           << "  groups)\n"
           << "    echo '  Smart Split  group/smart-split  order · stays turn  codex/gpt-6-astra  ← Codex'\n"
           << "    exit 0\n"
           << "    ;;\n"
           << "  group)\n"
           << "    if [ \"$2\" = \"add\" ]; then\n"
           << "      touch \"" << group_created_marker.string() << "\"\n"
           << "      exit 0\n"
           << "    elif [ \"$2\" = \"set\" ] || [ \"$2\" = \"pick\" ]; then\n"
           << "      exit 0\n"
           << "    elif [ -f \"" << group_created_marker.string() << "\" ]; then\n"
           << "      echo '  OmniDesk Active  group/omnidesk-active'\n"
           << "      echo '  routing   order'\n"
           << "      echo '  stays     session'\n"
           << "      echo '  models    1 codex/gpt-6-astra  Codex'\n"
           << "      exit 0\n"
           << "    else\n"
           << "      exit 1\n"
           << "    fi\n"
           << "    ;;\n"
           << "  status)\n"
           << "    echo '{\"running\":true,\"url\":\"" << server.baseUrl() << "\",\"pid\":" << ::getpid() << "}'\n"
           << "    exit 0\n"
           << "    ;;\n"
           << "esac\n"
           << "exit 0\n";
    writeTextFile(mock_magpie, script.str(), 0755);

    const fs::path gw_json = temp_root / "config" / "magpie" / "gateway.json";
    writeTextFile(gw_json,
                  nlohmann::json({{"addr", "127.0.0.1:" + std::to_string(server.port())},
                                  {"pid", ::getpid()},
                                  {"version", "0.1.4043-mock"}})
                      .dump());

    omnidesk::CapabilityRegistry caps;
    omnidesk::EntityBus bus;
    omnidesk::DomainRegistry domains;
    omnidesk::ConfigStore config_store(temp_root / "config");
    omnidesk::SecretVault vault(temp_root / "config");

    const std::string so_path = MAGPIE_PLUGIN_SO_PATH;
    void* handle = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK_TRUE(handle != nullptr);

    auto create_fn = reinterpret_cast<OmniDeskPluginCreateFn>(::dlsym(handle, "omnidesk_plugin_create"));
    auto destroy_fn = reinterpret_cast<OmniDeskPluginDestroyFn>(::dlsym(handle, "omnidesk_plugin_destroy"));
    CHECK_TRUE(create_fn != nullptr && destroy_fn != nullptr);

    omnidesk::PluginContext ctx("plugin-magpie-agent",
                                fs::path(so_path).parent_path(),
                                temp_root / "data",
                                caps,
                                bus,
                                domains,
                                config_store,
                                vault);

    ctx.saveConfig({{"magpie_bin", mock_magpie.string()},
                    {"gateway_url", server.baseUrl()},
                    {"route_group_id", "omnidesk-active"},
                    {"routing_strategy", "order"},
                    {"stays_policy", "session"}});
    ctx.setSecret("gateway_key", "secret-test-key-999");

    int route_events = 0;
    nlohmann::json last_event_payload;
    bus.subscribe("entity.ai.route_changed", "test-listener",
                  [&](const std::string& /*topic*/, const nlohmann::json& payload) {
                      ++route_events;
                      last_event_payload = payload;
                  });

    auto* plugin = static_cast<omnidesk::IPlugin*>(create_fn());
    CHECK_TRUE(plugin != nullptr);
    CHECK_TRUE(plugin->onLoad(ctx));

    // Read-only router queries
    auto r_gw = caps.call("llm.router.v1", {{"action", "gateway_status"}});
    CHECK_TRUE(r_gw.ok);
    CHECK_TRUE(r_gw.data.value("running", false));
    CHECK_TRUE(r_gw.data.value("models_reachable", false));

    auto r_ls = caps.call("llm.router.v1", {{"action", "list_harnesses"}});
    CHECK_TRUE(r_ls.ok);
    CHECK_EQ(r_ls.data["harnesses"].size(), std::size_t{1});

    auto r_models = caps.call("llm.router.v1", {{"action", "list_models"}});
    CHECK_TRUE(r_models.ok);
    CHECK_EQ(r_models.data["models"].size(), std::size_t{2});

    auto r_quotas = caps.call("llm.router.v1", {{"action", "list_quotas"}});
    CHECK_TRUE(r_quotas.ok);
    CHECK_EQ(r_quotas.data["quotas"].size(), std::size_t{1});

    // Cycle rejection & dry-run check
    auto r_cycle = caps.call("llm.router.v1", {{"action", "set_route"}, {"model", "group/omnidesk-active"}});
    CHECK_TRUE(!r_cycle.ok);
    CHECK_EQ(r_cycle.data.value("error_code", ""), std::string("group_cycle_detected"));

    auto r_dry = caps.call("llm.router.v1",
                           {{"action", "set_route"},
                            {"model", "codex/gpt-6-astra"},
                            {"effort", "high"},
                            {"dry_run", true}});
    CHECK_TRUE(r_dry.ok);
    CHECK_TRUE(r_dry.data.value("dry_run", false));

    // Write-back 1: creates new group (`group add`) + publishes event + persists ConfigStore
    auto r_set1 = caps.call("llm.router.v1",
                            {{"action", "set_route"},
                             {"model", "codex/gpt-6-astra"},
                             {"effort", "high"},
                             {"fast", true},
                             {"fallbacks", {"group/smart-split"}},
                             {"routing", "order"},
                             {"stays", "session"}});
    CHECK_TRUE(r_set1.ok);
    CHECK_EQ(r_set1.data.value("primary_member", ""), std::string("codex/gpt-6-astra:high:fast"));
    CHECK_EQ(route_events, 1);
    CHECK_EQ(last_event_payload.value("group_ref", ""), std::string("group/omnidesk-active"));

    // Write-back 2: updates existing group (`group set`) + `manual` triggers `group pick`
    auto r_set2 = caps.call("llm.router.v1",
                            {{"action", "set_route"},
                             {"model", "codex/gpt-6-astra"},
                             {"routing", "manual"}});
    CHECK_TRUE(r_set2.ok);
    CHECK_EQ(route_events, 2);

    // Ensure ConfigStore never stored SecretVault secrets
    const std::string saved_cfg_text =
        readTextFile(temp_root / "config" / "plugins" / "plugin-magpie-agent.json");
    CHECK_TRUE(saved_cfg_text.find("secret-test-key-999") == std::string::npos);
    CHECK_TRUE(saved_cfg_text.find("codex/gpt-6-astra") != std::string::npos);

    // Inference RPC 1: ai.agent.v1
    auto r_agent = caps.call("ai.agent.v1",
                             {{"action", "summarize"},
                              {"domain", "book"},
                              {"context", "Chapter 3 notes"},
                              {"prompt", "Summarize core thesis"}});
    CHECK_TRUE(r_agent.ok);
    CHECK_EQ(r_agent.data.value("route_used", ""), std::string("group/omnidesk-active"));
    CHECK_TRUE(r_agent.data.value("reply", "").find("Summarize core thesis") != std::string::npos);
    CHECK_EQ(server.last_chat_body.value("model", ""), std::string("group/omnidesk-active"));

    // Inference RPC 2: llm.chat.v1 structured JSON + route_override
    auto r_chat = caps.call("llm.chat.v1",
                            {{"route_override", "codex/gpt-6-astra"},
                             {"reasoning_effort", "medium"},
                             {"response_format", {{"type", "json_object"}}},
                             {"messages", {{{"role", "user"}, {"content", "Give JSON"}}}}});
    CHECK_TRUE(r_chat.ok);
    CHECK_EQ(r_chat.data.value("route_used", ""), std::string("codex/gpt-6-astra"));
    CHECK_TRUE(r_chat.data.contains("parsed_json"));
    CHECK_EQ(r_chat.data["parsed_json"].value("status", ""), std::string("ok"));
    CHECK_EQ(server.last_chat_body.value("reasoning_effort", ""), std::string("medium"));

    // Inference RPC 3: llm.translate.v1 with context & glossary
    auto r_trans = caps.call("llm.translate.v1",
                             {{"text", "Hello world"},
                              {"source_lang", "en"},
                              {"target_lang", "zh-CN"},
                              {"context_before", "Intro paragraph."},
                              {"glossary", {{"OmniDesk", "全能桌面"}}}});
    CHECK_TRUE(r_trans.ok);
    CHECK_EQ(r_trans.data.value("translation", ""), std::string("你好，世界"));

    // Inference RPC 4: streaming SSE via callStream + CancelToken cooperative abort
    {
        std::vector<std::string> deltas;
        bool saw_done = false;
        auto r_stream = caps.callStream(
            "llm.chat.v1",
            {{"prompt", "stream hi"}, {"stream", true}},
            [&](const omnidesk::StreamChunk& chunk) {
                if (!chunk.delta.empty()) {
                    deltas.push_back(chunk.delta);
                }
                if (chunk.done) {
                    saw_done = true;
                }
            });
        CHECK_TRUE(r_stream.ok);
        CHECK_EQ(r_stream.data.value("reply", ""), std::string("Hello!"));
        CHECK_EQ(deltas.size(), std::size_t(2));
        CHECK_EQ(deltas[0], std::string("Hel"));
        CHECK_EQ(deltas[1], std::string("lo!"));
        CHECK_TRUE(saw_done);
    }
    {
        auto cancel_tok = omnidesk::CancelToken::create();
        std::thread killer([cancel_tok]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancel_tok->cancel();
        });
        auto t0 = std::chrono::steady_clock::now();
        bool saw_cancel_done = false;
        auto r_cancel = caps.callStream(
            "llm.chat.v1",
            {{"prompt", "__SLOW__"}, {"stream", true}},
            [&](const omnidesk::StreamChunk& chunk) {
                if (chunk.done) {
                    saw_cancel_done = true;
                }
            },
            cancel_tok);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        killer.join();
        CHECK_TRUE(!r_cancel.ok);
        CHECK_TRUE(r_cancel.cancelled);
        CHECK_TRUE(saw_cancel_done);
        CHECK_TRUE(elapsed_ms < 1500);
    }

    // Verify 429 quota_exceeded maps cleanly AND redacts gateway_key from error message
    server.force_429 = true;
    auto r_err = caps.call("llm.chat.v1", {{"prompt", "hi"}});
    CHECK_TRUE(!r_err.ok);
    CHECK_EQ(r_err.data.value("error_code", ""), std::string("quota_exceeded"));
    CHECK_TRUE(r_err.error.find("secret-test-key-999") == std::string::npos);
    CHECK_TRUE(r_err.error.find("[REDACTED]") != std::string::npos);

    // Inspect mock CLI invocation log to guarantee zero terminal harness writes
    const std::string cli_calls = readTextFile(cli_log);
    CHECK_TRUE(cli_calls.find("group add omnidesk-active") != std::string::npos);
    CHECK_TRUE(cli_calls.find("group set omnidesk-active") != std::string::npos);
    CHECK_TRUE(cli_calls.find("group pick omnidesk-active") != std::string::npos);
    CHECK_TRUE(cli_calls.find("codex") == std::string::npos || cli_calls.find("models=codex/") != std::string::npos);

    plugin->onUnload();
    destroy_fn(plugin);
    ::dlclose(handle);
    server.stop();
    fs::remove_all(temp_root);
    return 0;
}

} // namespace

int main() {
    if (testParsers() != 0) {
        return 1;
    }
    if (testRouteBuilderAndCycles() != 0) {
        return 1;
    }
    if (testIntegrationWithPluginSo() != 0) {
        return 1;
    }
    std::cout << "test_magpie_plugin: all parser, cycle, routing, RPC, and security tests passed\n";
    return 0;
}
