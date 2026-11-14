#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <omnidesk/plugin.hpp>
#include <omnidesk/plugin_abi.h>
#include <omnidesk/sdk.hpp>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const char* env_so = std::getenv("OMNIDESK_PLUGIN_SO");
    std::string so_path = (argc > 1) ? argv[1] : (env_so ? env_so : "./libplugin_magpie_agent.so");

    void* handle = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "dlopen failed for " << so_path << ": " << ::dlerror() << "\n";
        return 1;
    }

    auto abi_info_fn = reinterpret_cast<OmniDeskAbiInfoFn>(
        ::dlsym(handle, "omnidesk_plugin_abi_info"));
    auto create_fn = reinterpret_cast<OmniDeskPluginCreateFn>(
        ::dlsym(handle, "omnidesk_plugin_create"));
    auto destroy_fn = reinterpret_cast<OmniDeskPluginDestroyFn>(
        ::dlsym(handle, "omnidesk_plugin_destroy"));

    if (!abi_info_fn || !create_fn || !destroy_fn) {
        std::cerr << "Missing required OmniDesk C ABI v1 symbols\n";
        ::dlclose(handle);
        return 1;
    }

    const OmniDeskPluginInfo* info = abi_info_fn();
    if (!info || info->abi_version != OMNIDESK_PLUGIN_ABI_VERSION ||
        info->struct_size < sizeof(OmniDeskPluginInfo)) {
        std::cerr << "Invalid OmniDeskPluginInfo ABI header\n";
        ::dlclose(handle);
        return 1;
    }

    if (!info->id || !info->kind || !info->domain || !info->sdk_version ||
        std::string(info->id) != "plugin-magpie-agent" ||
        std::string(info->kind) != "provider" || std::string(info->domain) != "core" ||
        std::string(info->sdk_version) != OMNIDESK_SDK_VERSION_STRING) {
        std::cerr << "Unexpected OmniDeskPluginInfo metadata\n";
        ::dlclose(handle);
        return 1;
    }

    const fs::path temp_root =
        fs::temp_directory_path() / ("omnidesk-magpie-abi-" + std::to_string(::getpid()));
    fs::create_directories(temp_root / "data");
    fs::create_directories(temp_root / "config");

    omnidesk::CapabilityRegistry caps;
    omnidesk::EntityBus bus;
    omnidesk::DomainRegistry domains;
    omnidesk::ConfigStore config_store(temp_root / "config");
    omnidesk::SecretVault vault(temp_root / "config");

    omnidesk::PluginContext ctx(info->id,
                                fs::path(so_path).parent_path(),
                                temp_root / "data",
                                caps,
                                bus,
                                domains,
                                config_store,
                                vault);

    auto* plugin = static_cast<omnidesk::IPlugin*>(create_fn());
    if (!plugin) {
        std::cerr << "omnidesk_plugin_create returned nullptr\n";
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bool loaded = plugin->onLoad(ctx);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    if (!loaded) {
        std::cerr << "onLoad returned false\n";
        destroy_fn(plugin);
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }
    if (load_ms > 500) {
        std::cerr << "onLoad exceeded non-blocking budget: " << load_ms << " ms\n";
        plugin->onUnload();
        destroy_fn(plugin);
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }

    for (const char* cap : {"llm.router.v1", "ai.agent.v1", "llm.chat.v1", "llm.translate.v1"}) {
        if (!caps.has(cap)) {
            std::cerr << "Missing registered capability: " << cap << "\n";
            plugin->onUnload();
            destroy_fn(plugin);
            ::dlclose(handle);
            fs::remove_all(temp_root);
            return 1;
        }
    }

    if (domains.dockWidgets(omnidesk::ContentDomain::Core).empty()) {
        std::cerr << "Expected F8 dock widget to be registered\n";
        plugin->onUnload();
        destroy_fn(plugin);
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }

    auto hc = plugin->onHealthCheck();
    if (!hc.value("ok", false)) {
        std::cerr << "onHealthCheck failed: " << hc.dump() << "\n";
        plugin->onUnload();
        destroy_fn(plugin);
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }

    auto cur = caps.call("llm.router.v1", {{"action", "current_route"}});
    if (!cur.ok || !cur.data.contains("summary") || !cur.data["summary"].is_string()) {
        std::cerr << "llm.router.v1 current_route contract check failed: " << cur.error << "\n";
        plugin->onUnload();
        destroy_fn(plugin);
        ::dlclose(handle);
        fs::remove_all(temp_root);
        return 1;
    }

    plugin->onUnload();
    destroy_fn(plugin);
    ::dlclose(handle);
    fs::remove_all(temp_root);

    std::cout << "test_abi passed (onLoad " << load_ms << " ms)\n";
    return 0;
}
