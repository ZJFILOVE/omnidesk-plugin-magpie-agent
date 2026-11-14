#pragma once

#include "magpie_agent/magpie_types.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace magpie_agent {

// Removes timestamped `YYYY/MM/DD HH:MM:SS plugin...` lines from Magpie CLI outputs.
std::string stripPluginLogLines(const std::string& text,
                                std::vector<std::string>* captured_logs = nullptr);

// Parses the JSON object or array embedded in CLI output after optional log lines.
nlohmann::json extractJsonAfterLogs(const std::string& text,
                                    std::string* error_out = nullptr,
                                    std::vector<std::string>* captured_logs = nullptr);

// Gateway status parsers.
GatewayStatus parseGatewayFileJson(const std::string& json_text,
                                   const std::string& config_path);
GatewayStatus parseStatusCliJson(const std::string& raw_output);

// Harness detection parser (`magpie ls`).
std::vector<HarnessInfo> parseHarnessesText(const std::string& raw_output);

// Models parsers (`GET /v1/models` and `magpie models`).
std::vector<ModelInfo> parseModelsHttpJson(const std::string& json_text);
std::vector<ModelInfo> parseModelsCliText(const std::string& raw_output);
std::vector<ModelInfo> mergeModels(const std::vector<ModelInfo>& http_models,
                                   const std::vector<ModelInfo>& cli_models);

// Groups parsers (`magpie groups` and `magpie group <id>`).
std::vector<GroupInfo> parseGroupsCliText(const std::string& raw_output);
std::optional<GroupInfo> parseGroupDetailText(const std::string& raw_output);

// Quotas parsers (`GET /v1/magpie/quotas` and `magpie quota --json`).
std::vector<QuotaEntry> parseQuotasJson(const std::string& raw_output,
                                        std::vector<std::string>* warnings = nullptr);

// Route validation & member spec construction.
bool isValidGroupId(const std::string& id);
bool isValidEffort(const std::string& effort);
bool isValidRoutingStrategy(const std::string& routing);
bool isValidStaysPolicy(const std::string& stays);

// Formats a member according to Magpie syntax: `provider/model[:effort][:fast][#<account>]` or `group/<id>`.
std::string buildMemberSpec(const std::string& model_or_spec,
                            const std::string& effort = "",
                            bool fast = false,
                            const std::string& account = "",
                            std::string* error_out = nullptr);

// Detects self-reference or cycles (up to depth 8) against existing Magpie groups.
bool wouldCreateGroupCycle(const std::string& target_group_id,
                           const std::vector<std::string>& members,
                           const std::vector<GroupInfo>& existing_groups,
                           std::string* cycle_reason = nullptr);

// Replaces any secret values in diagnostic strings with [REDACTED].
std::string redactSecrets(const std::string& text, const std::vector<std::string>& secrets);

// Default Magpie gateway config path (~/.config/magpie/gateway.json).
std::string defaultGatewayJsonPath();

} // namespace magpie_agent
