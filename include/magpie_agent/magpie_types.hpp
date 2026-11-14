#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace magpie_agent {

struct GatewayStatus {
    bool running = false;
    bool models_reachable = false;
    std::string url;
    std::string addr;
    int pid = 0;
    std::string version;
    std::string started;
    std::string source = "unknown";
    std::string config_file;
    std::string error;

    nlohmann::json toJson() const;
};

struct HarnessInfo {
    std::string name;
    std::string model_or_group;
    std::string effort;
    std::string sign_in;
    std::string config_path;
    bool configured = false;
    std::string raw_line;

    nlohmann::json toJson() const;
};

struct ModelInfo {
    std::string id;
    std::string provider;
    std::string model_name;
    std::string display_name;
    std::string magpie_label;
    bool is_group = false;
    bool is_auto_group = false;
    bool reasoning = false;
    bool fast_supported = false;
    std::vector<std::string> efforts;
    std::int64_t context_length = 0;
    std::int64_t max_output_tokens = 0;
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;
    bool from_http = false;
    bool from_cli = false;

    nlohmann::json toJson() const;
};

struct GroupInfo {
    std::string id;
    std::string ref;
    std::string name;
    bool is_auto = false;
    std::string routing;
    std::string stays;
    std::string effort;
    std::string classifier;
    int rules_count = 0;
    std::string picked_model;
    std::vector<std::string> members;
    std::vector<std::string> used_by;
    std::vector<std::string> levels;

    nlohmann::json toJson() const;
};

struct QuotaWindow {
    std::string name;
    double used = 0.0;
    double remaining = 100.0;
    std::string resets_at;
    std::string display;
    std::string pool;
    double amount = 0.0;
    double limit = 0.0;
    bool has_amount = false;

    nlohmann::json toJson() const;
};

struct QuotaEntry {
    std::string provider;
    std::string name;
    std::string kind;
    std::string plan;
    std::string user;
    std::string alias;
    std::string error;
    bool last = false;
    std::string last_served_at;
    std::string read_at;
    bool has_balance = false;
    double balance = 0.0;
    std::string balance_display;
    std::vector<QuotaWindow> windows;

    nlohmann::json toJson() const;
};

struct RouteSpec {
    bool configured = false;
    std::string route_group_id = "omnidesk-active";
    std::string model;
    std::string effort;
    bool fast = false;
    std::string account;
    std::string primary_member;
    std::vector<std::string> fallbacks;
    std::vector<std::string> members;
    std::string routing = "order";
    std::string stays = "session";
    std::string summary;

    std::string groupRef() const { return "group/" + route_group_id; }
    nlohmann::json toJson(const GatewayStatus& gateway) const;
};

struct RouterSnapshot {
    GatewayStatus gateway;
    std::vector<HarnessInfo> harnesses;
    std::vector<ModelInfo> models;
    std::vector<GroupInfo> groups;
    std::vector<QuotaEntry> quotas;
    RouteSpec route;
    std::vector<std::string> warnings;
    std::int64_t updated_at_ms = 0;
};

} // namespace magpie_agent
