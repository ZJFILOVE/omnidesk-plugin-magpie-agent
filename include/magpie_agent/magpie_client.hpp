#pragma once

#include "magpie_agent/http_client.hpp"
#include "magpie_agent/magpie_types.hpp"
#include "magpie_agent/process_exec.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <omnidesk/plugin.hpp>
#include <string>
#include <thread>
#include <vector>

namespace magpie_agent {

class MagpieClient : public std::enable_shared_from_this<MagpieClient> {
public:
    MagpieClient(omnidesk::PluginContext* ctx,
                 std::shared_ptr<IHttpClient> http_client = nullptr,
                 CommandRunnerFn command_runner = nullptr,
                 std::string gateway_json_path_override = "");
    ~MagpieClient();

    // Non-blocking initialization from persisted config and local gateway.json (< 5 ms).
    void initializeFast();

    // Cancels active CLI/HTTP operations, joins background workers, and detaches context.
    void shutdown();

    bool isShuttingDown() const { return shutting_down_.load(); }

    // Reloads runtime settings from PluginContext config & SecretVault.
    void reloadConfig();

    // Read operations (thread-safe).
    GatewayStatus probeGatewayStatus(bool check_http = false);
    std::vector<HarnessInfo> fetchHarnesses(std::vector<std::string>* warnings = nullptr);
    std::vector<ModelInfo> fetchModels(bool allow_cli_fallback = true,
                                       std::vector<std::string>* warnings = nullptr);
    std::vector<GroupInfo> fetchGroups(bool include_details = false,
                                       std::vector<std::string>* warnings = nullptr);
    std::optional<GroupInfo> fetchGroupDetail(const std::string& group_id,
                                              std::string* error_out = nullptr);
    std::vector<QuotaEntry> fetchQuotas(std::vector<std::string>* warnings = nullptr);

    RouterSnapshot getCachedSnapshot() const;
    RouterSnapshot refreshAllSnapshot(bool probe_http_and_cli = true);

    // Launches a tracked worker thread safely joined during shutdown().
    void spawnTrackedWorker(std::function<void()> fn);

    // Capability RPC entrypoints (synchronous & streaming/cancellable).
    omnidesk::CapabilityResult handleRouterRpc(const nlohmann::json& req);
    omnidesk::CapabilityResult handleAgentRpc(const nlohmann::json& req);
    omnidesk::CapabilityResult handleChatRpc(const nlohmann::json& req);
    omnidesk::CapabilityResult handleTranslateRpc(const nlohmann::json& req);

    omnidesk::CapabilityResult handleAgentRpcStream(const nlohmann::json& req,
                                                    const std::shared_ptr<omnidesk::CancelToken>& cancel,
                                                    const omnidesk::StreamCallback& on_chunk);
    omnidesk::CapabilityResult handleChatRpcStream(const nlohmann::json& req,
                                                   const std::shared_ptr<omnidesk::CancelToken>& cancel,
                                                   const omnidesk::StreamCallback& on_chunk);
    omnidesk::CapabilityResult handleTranslateRpcStream(const nlohmann::json& req,
                                                        const std::shared_ptr<omnidesk::CancelToken>& cancel,
                                                        const omnidesk::StreamCallback& on_chunk);

    // Health check payload for onHealthCheck().
    nlohmann::json healthCheckJson() const;

    // Applies route to Magpie group, saves ConfigStore, and emits entity.ai.route_changed.
    omnidesk::CapabilityResult applyRoute(const nlohmann::json& req);

private:
    struct RuntimeSettings {
        std::string magpie_bin = "magpie";
        std::string gateway_url_override;
        std::string route_group_id = "omnidesk-active";
        std::string routing_strategy = "order";
        std::string stays_policy = "session";
        int request_timeout_ms = 60000;
    };

    RuntimeSettings currentSettings() const;
    std::string currentGatewayKey() const;
    std::string resolveMagpieExecutable(std::string* error_out) const;
    std::string resolveGatewayBaseUrl(const GatewayStatus& st) const;
    std::string buildRouteSummary(const RouteSpec& route, const GatewayStatus& st) const;

    ProcessOutput runMagpie(const std::vector<std::string>& subargs, int timeout_ms) const;

    omnidesk::CapabilityResult callChatCompletionsStream(
        const nlohmann::json& body,
        const std::string& route_used,
        int timeout_override_ms,
        const std::shared_ptr<omnidesk::CancelToken>& cancel,
        const omnidesk::StreamCallback& on_chunk);

    static omnidesk::CapabilityResult makeError(const std::string& code,
                                                const std::string& message,
                                                const nlohmann::json& extra = nlohmann::json::object());

    omnidesk::PluginContext* ctx_ = nullptr; // Guarded by ctx_mu_ (cleared in shutdown)
    mutable std::mutex ctx_mu_;

    std::shared_ptr<IHttpClient> http_;
    CommandRunnerFn cmd_runner_;
    std::string gateway_json_path_;

    std::atomic<bool> shutting_down_{false};
    std::shared_ptr<ProcessCancelToken> cancel_token_;

    mutable std::mutex state_mu_;
    RuntimeSettings settings_;
    RouterSnapshot snapshot_;

    struct WorkerSlot {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::mutex workers_mu_;
    std::vector<WorkerSlot> workers_;
};

} // namespace magpie_agent
