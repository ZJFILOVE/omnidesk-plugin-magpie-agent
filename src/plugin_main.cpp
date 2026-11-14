#include "magpie_agent/magpie_client.hpp"
#include "magpie_agent/magpie_ui.hpp"

#include <memory>
#include <omnidesk/plugin_abi.h>
#include <omnidesk/sdk.hpp>

namespace {

omnidesk::CapabilityResult unloadedError() {
    omnidesk::CapabilityResult r;
    r.ok = false;
    r.data = {{"error_code", "unloaded"}};
    r.error = "plugin-magpie-agent unloaded";
    r.provider_plugin_id = "plugin-magpie-agent";
    return r;
}

} // namespace

class MagpieAgentPlugin final : public omnidesk::IPlugin {
public:
    bool onLoad(omnidesk::PluginContext& ctx) override {
        ctx_ = &ctx;
        client_ = std::make_shared<magpie_agent::MagpieClient>(&ctx);
        client_->initializeFast();

        auto weak_client = std::weak_ptr<magpie_agent::MagpieClient>(client_);

        ctx.registerCapability(
            "llm.router.v1",
            [weak_client](const nlohmann::json& req) -> omnidesk::CapabilityResult {
                auto c = weak_client.lock();
                return c ? c->handleRouterRpc(req) : unloadedError();
            },
            {
                {"provider", "magpie"},
                {"actions", {"current_route", "gateway_status", "list_harnesses", "list_models", "list_groups", "list_quotas", "set_route"}},
                {"isolation", "dedicated_group_no_harness_mutation"},
            },
            100);

        ctx.registerStreamingCapability(
            "ai.agent.v1",
            [weak_client](const nlohmann::json& req,
                          const std::shared_ptr<omnidesk::CancelToken>& cancel,
                          const omnidesk::StreamCallback& on_chunk) -> omnidesk::CapabilityResult {
                auto c = weak_client.lock();
                return c ? c->handleAgentRpcStream(req, cancel, on_chunk) : unloadedError();
            },
            {
                {"provider", "magpie"},
                {"actions", {"chat", "summarize", "translate", "etymology", "clause_parse", "custom"}},
                {"domains", {"core", "media", "text", "book"}},
                {"streaming", true},
            },
            100);

        ctx.registerStreamingCapability(
            "llm.chat.v1",
            [weak_client](const nlohmann::json& req,
                          const std::shared_ptr<omnidesk::CancelToken>& cancel,
                          const omnidesk::StreamCallback& on_chunk) -> omnidesk::CapabilityResult {
                auto c = weak_client.lock();
                return c ? c->handleChatRpcStream(req, cancel, on_chunk) : unloadedError();
            },
            {{"provider", "magpie"}, {"openai_compatible", true}, {"streaming", true}},
            100);

        ctx.registerStreamingCapability(
            "llm.translate.v1",
            [weak_client](const nlohmann::json& req,
                          const std::shared_ptr<omnidesk::CancelToken>& cancel,
                          const omnidesk::StreamCallback& on_chunk) -> omnidesk::CapabilityResult {
                auto c = weak_client.lock();
                return c ? c->handleTranslateRpcStream(req, cancel, on_chunk) : unloadedError();
            },
            {{"provider", "magpie"}, {"glossary_lock", true}, {"context_window", true}, {"streaming", true}},
            100);

        ctx.registerDockWidget(omnidesk::DockWidgetSpec{
            "magpie.route_dock",
            "plugin-magpie-agent",
            "Magpie 路由与额度",
            {
                omnidesk::ContentDomain::Core,
                omnidesk::ContentDomain::Media,
                omnidesk::ContentDomain::Text,
                omnidesk::ContentDomain::Book,
            },
            [weak_client]() -> brls::View* {
                auto c = weak_client.lock();
                return magpie_agent::createMagpieDockWidget(c);
            }});

        ctx.logInfo("plugin-magpie-agent initialized (non-blocking fast path + SSE streaming)");
        return true;
    }

    void onUnload() override {
        if (ctx_) {
            ctx_->capabilities().unregisterPlugin(ctx_->pluginId());
            ctx_->domains().unregisterPlugin(ctx_->pluginId());
            ctx_ = nullptr;
        }
        if (client_) {
            client_->shutdown();
            client_.reset();
        }
    }

    nlohmann::json onHealthCheck() const override {
        if (!client_) {
            return {{"ok", false}, {"error", "not initialized"}};
        }
        return client_->healthCheckJson();
    }

    brls::View* createSettingsView(omnidesk::PluginContext& ctx) override {
        if (!client_) {
            return nullptr;
        }
        client_->reloadConfig();
        (void)ctx;
        return magpie_agent::createMagpieSettingsView(client_);
    }

private:
    omnidesk::PluginContext* ctx_ = nullptr;
    std::shared_ptr<magpie_agent::MagpieClient> client_;
};

OMNIDESK_DECLARE_PLUGIN(MagpieAgentPlugin,
                        "plugin-magpie-agent",
                        "Magpie AI Agent Router",
                        "0.1.0",
                        "provider",
                        "core")
