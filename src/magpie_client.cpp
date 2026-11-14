#include "magpie_agent/magpie_client.hpp"
#include "magpie_agent/magpie_parser.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace magpie_agent {

namespace {

std::int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string readFileIfExists(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::string trimTrailingSlashes(const std::string& url) {
    std::string s = url;
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string extractJsonErrorMessage(const std::string& body) {
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return {};
    }
    if (j.contains("error")) {
        const auto& e = j["error"];
        if (e.is_string()) {
            return e.get<std::string>();
        }
        if (e.is_object()) {
            std::string msg = e.value("message", "");
            std::string code = e.value("code", "");
            if (!msg.empty() && !code.empty()) {
                return code + ": " + msg;
            }
            return !msg.empty() ? msg : code;
        }
    }
    return j.value("message", "");
}

std::string extractAssistantContent(const nlohmann::json& choice) {
    if (!choice.is_object() || !choice.contains("message")) {
        return {};
    }
    const auto& msg = choice["message"];
    if (!msg.is_object() || !msg.contains("content")) {
        return {};
    }
    const auto& c = msg["content"];
    if (c.is_string()) {
        return c.get<std::string>();
    }
    if (c.is_array()) {
        std::string combined;
        for (const auto& part : c) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                combined += part["text"].get<std::string>();
            }
        }
        return combined;
    }
    return {};
}

std::string domainAgentPersona(const std::string& domain) {
    if (domain == "media") {
        return "You are OmniDesk's Media Workbench AI assistant helping summarize recordings, inspect timestamps, and draft clean study notes.";
    }
    if (domain == "text") {
        return "You are OmniDesk's Text Workbench AI assistant helping analyze sentences, extract vocabulary/etymology, and refine prose.";
    }
    if (domain == "book") {
        return "You are OmniDesk's Book Reader AI assistant helping synthesize chapters, analyze difficult clauses, and explain terminology.";
    }
    return "You are OmniDesk's workspace AI assistant powered by Magpie multi-provider routing.";
}

std::string buildAgentActionInstruction(const std::string& action,
                                        const std::string& domain,
                                        const std::string& target_lang) {
    std::string base = domainAgentPersona(domain);
    if (action == "summarize") {
        return base + " Provide a structured, concise summary highlighting core thesis, key details, and actionable takeaways.";
    }
    if (action == "translate") {
        std::string lang = target_lang.empty() ? "zh-CN" : target_lang;
        return base + " Translate the user input faithfully into " + lang + " while preserving tone, formatting, and domain terminology.";
    }
    if (action == "etymology") {
        return base + " Explain word roots, affixes, cognates, nuance in context, and concise bilingual usage examples.";
    }
    if (action == "clause_parse") {
        return base + " Break down complex sentence structure into main clause, subordinate clauses, grammatical roles, and fluent translation.";
    }
    return base;
}

std::vector<std::string> parseFallbackList(const nlohmann::json& req) {
    std::vector<std::string> raw;
    if (!req.contains("fallbacks")) {
        return raw;
    }
    const auto& fbs = req["fallbacks"];
    if (fbs.is_array()) {
        for (const auto& item : fbs) {
            if (item.is_string()) {
                raw.push_back(item.get<std::string>());
            }
        }
    } else if (fbs.is_string()) {
        std::istringstream iss(fbs.get<std::string>());
        std::string tok;
        while (std::getline(iss, tok, ',')) {
            if (!tok.empty()) {
                raw.push_back(tok);
            }
        }
    }
    return raw;
}

std::string formatGlossarySection(const nlohmann::json& glossary) {
    if (glossary.is_object() && !glossary.empty()) {
        std::string out;
        for (auto it = glossary.begin(); it != glossary.end(); ++it) {
            if (it.value().is_string()) {
                out += "- " + it.key() + " => " + it.value().get<std::string>() + "\n";
            }
        }
        return out;
    }
    if (glossary.is_array() && !glossary.empty()) {
        std::string out;
        for (const auto& item : glossary) {
            if (item.is_object()) {
                std::string src = item.value("source", item.value("term", ""));
                std::string dst = item.value("target", item.value("translation", ""));
                if (!src.empty() && !dst.empty()) {
                    out += "- " + src + " => " + dst + "\n";
                }
            }
        }
        return out;
    }
    return {};
}

} // namespace

MagpieClient::MagpieClient(omnidesk::PluginContext* ctx,
                           std::shared_ptr<IHttpClient> http_client,
                           CommandRunnerFn command_runner,
                           std::string gateway_json_path_override)
    : ctx_(ctx),
      http_(http_client ? std::move(http_client) : std::make_shared<CurlHttpClient>()),
      cmd_runner_(command_runner ? command_runner : &runCommand),
      gateway_json_path_(std::move(gateway_json_path_override)),
      cancel_token_(std::make_shared<ProcessCancelToken>()) {}

MagpieClient::~MagpieClient() {
    shutdown();
}

void MagpieClient::initializeFast() {
    reloadConfig();
    GatewayStatus gw = probeGatewayStatus(false);

    std::lock_guard<std::mutex> lock(state_mu_);
    snapshot_.gateway = gw;
    snapshot_.route.summary = buildRouteSummary(snapshot_.route, snapshot_.gateway);
    snapshot_.updated_at_ms = nowEpochMs();
}

void MagpieClient::shutdown() {
    if (shutting_down_.exchange(true)) {
        return;
    }
    if (cancel_token_) {
        cancel_token_->cancel();
    }

    std::vector<WorkerSlot> to_join;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        to_join.swap(workers_);
    }
    for (auto& slot : to_join) {
        if (slot.thread.joinable()) {
            slot.thread.join();
        }
    }

    std::lock_guard<std::mutex> clock(ctx_mu_);
    ctx_ = nullptr;
}

void MagpieClient::spawnTrackedWorker(std::function<void()> fn) {
    if (shutting_down_.load()) {
        return;
    }
    std::vector<WorkerSlot> finished;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        if (shutting_down_.load()) {
            return;
        }
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->done && it->done->load()) {
                finished.push_back(std::move(*it));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
        auto done_flag = std::make_shared<std::atomic<bool>>(false);
        std::thread th([self = shared_from_this(), done_flag, task = std::move(fn)]() {
            struct Guard {
                std::shared_ptr<std::atomic<bool>> flag;
                ~Guard() {
                    if (flag) {
                        flag->store(true);
                    }
                }
            } guard{done_flag};
            if (!self->isShuttingDown() && task) {
                task();
            }
        });
        workers_.push_back(WorkerSlot{std::move(th), std::move(done_flag)});
    }
    for (auto& f : finished) {
        if (f.thread.joinable()) {
            f.thread.join();
        }
    }
}

void MagpieClient::reloadConfig() {
    nlohmann::json cfg = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> clock(ctx_mu_);
        if (ctx_) {
            cfg = ctx_->loadConfig();
        }
    }

    RuntimeSettings next;
    if (cfg.is_object()) {
        std::string bin = cfg.value("magpie_bin", "");
        if (!bin.empty()) {
            next.magpie_bin = bin;
        }
        next.gateway_url_override = trimTrailingSlashes(cfg.value("gateway_url", ""));

        std::string gid = cfg.value("route_group_id", "omnidesk-active");
        if (isValidGroupId(gid)) {
            next.route_group_id = gid;
        }
        std::string strat = cfg.value("routing_strategy", "order");
        if (isValidRoutingStrategy(strat)) {
            next.routing_strategy = strat;
        }
        std::string stays = cfg.value("stays_policy", "session");
        if (isValidStaysPolicy(stays)) {
            next.stays_policy = stays;
        }
        if (cfg.contains("request_timeout_ms")) {
            int parsed = 0;
            if (cfg["request_timeout_ms"].is_number_integer()) {
                parsed = cfg["request_timeout_ms"].get<int>();
            } else if (cfg["request_timeout_ms"].is_string()) {
                parsed = std::atoi(cfg["request_timeout_ms"].get<std::string>().c_str());
            }
            if (parsed >= 1000 && parsed <= 600000) {
                next.request_timeout_ms = parsed;
            }
        }
    }

    RouteSpec loaded_route;
    loaded_route.route_group_id = next.route_group_id;
    loaded_route.routing = next.routing_strategy;
    loaded_route.stays = next.stays_policy;

    if (cfg.is_object() && cfg.contains("active_route") && cfg["active_route"].is_object()) {
        const auto& r = cfg["active_route"];
        loaded_route.configured = r.value("configured", false);
        std::string r_gid = r.value("route_group_id", next.route_group_id);
        if (isValidGroupId(r_gid)) {
            loaded_route.route_group_id = r_gid;
        }
        loaded_route.model = r.value("model", "");
        loaded_route.effort = r.value("effort", "");
        loaded_route.fast = r.value("fast", false);
        loaded_route.account = r.value("account", "");
        loaded_route.primary_member = r.value("primary_member", "");
        loaded_route.routing = r.value("routing", next.routing_strategy);
        loaded_route.stays = r.value("stays", next.stays_policy);
        if (r.contains("fallbacks") && r["fallbacks"].is_array()) {
            for (const auto& fb : r["fallbacks"]) {
                if (fb.is_string()) {
                    loaded_route.fallbacks.push_back(fb.get<std::string>());
                }
            }
        }
        if (r.contains("members") && r["members"].is_array()) {
            for (const auto& m : r["members"]) {
                if (m.is_string()) {
                    loaded_route.members.push_back(m.get<std::string>());
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(state_mu_);
    settings_ = next;
    snapshot_.route = loaded_route;
    snapshot_.route.summary = buildRouteSummary(snapshot_.route, snapshot_.gateway);
}

MagpieClient::RuntimeSettings MagpieClient::currentSettings() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return settings_;
}

std::string MagpieClient::currentGatewayKey() const {
    std::lock_guard<std::mutex> clock(ctx_mu_);
    if (!ctx_) {
        return {};
    }
    return ctx_->getSecret("gateway_key").value_or("");
}

std::string MagpieClient::resolveMagpieExecutable(std::string* error_out) const {
    const auto s = currentSettings();
    std::string path = resolveExecutable(s.magpie_bin);
    if (path.empty() && error_out) {
        *error_out = "Magpie executable not found: '" + s.magpie_bin + "'";
    }
    return path;
}

std::string MagpieClient::resolveGatewayBaseUrl(const GatewayStatus& st) const {
    const auto s = currentSettings();
    if (!s.gateway_url_override.empty()) {
        return trimTrailingSlashes(s.gateway_url_override);
    }
    if (!st.url.empty()) {
        return trimTrailingSlashes(st.url);
    }
    return {};
}

std::string MagpieClient::buildRouteSummary(const RouteSpec& route, const GatewayStatus& st) const {
    const std::string gw_tag = st.running ? "gateway up" : "gateway offline";
    if (route.configured && !route.primary_member.empty()) {
        std::string s = route.primary_member;
        if (!route.fallbacks.empty()) {
            s += " (+" + std::to_string(route.fallbacks.size()) + " fallback)";
        }
        s += " · " + route.routing + "/" + route.stays;
        if (!st.running) {
            s += " [offline]";
        }
        return s;
    }
    return route.groupRef() + " (" + gw_tag + ")";
}

ProcessOutput MagpieClient::runMagpie(const std::vector<std::string>& subargs, int timeout_ms) const {
    std::string resolve_err;
    std::string exe = resolveMagpieExecutable(&resolve_err);
    if (exe.empty()) {
        ProcessOutput out;
        out.spawn_error = resolve_err;
        return out;
    }
    std::vector<std::string> argv;
    argv.reserve(1 + subargs.size());
    argv.push_back(exe);
    argv.insert(argv.end(), subargs.begin(), subargs.end());
    return cmd_runner_(argv, timeout_ms, cancel_token_);
}

GatewayStatus MagpieClient::probeGatewayStatus(bool check_http) {
    const auto s = currentSettings();
    GatewayStatus st;

    if (!s.gateway_url_override.empty()) {
        st.url = trimTrailingSlashes(s.gateway_url_override);
        st.source = "config_override";
    } else {
        const std::string gw_path =
            gateway_json_path_.empty() ? defaultGatewayJsonPath() : gateway_json_path_;
        const std::string raw = readFileIfExists(gw_path);
        if (!raw.empty()) {
            st = parseGatewayFileJson(raw, gw_path);
            st.running = !st.url.empty() && isPidAlive(st.pid);
        }
    }

    if (check_http) {
        if (st.url.empty()) {
            ProcessOutput cli = runMagpie({"status", "--json"}, 4000);
            if (cli.spawned && cli.exit_code == 0) {
                st = parseStatusCliJson(cli.combinedText());
            } else if (!cli.spawn_error.empty()) {
                st.error = cli.spawn_error;
            }
        }

        if (!st.url.empty()) {
            HttpRequestOptions opts;
            opts.connect_timeout_ms = 600;
            opts.timeout_ms = 1500;
            opts.bearer_token = currentGatewayKey();
            opts.cancel_flag = &shutting_down_;
            HttpResponse r = http_->get(st.url + "/v1/models", opts);
            if (r.ok) {
                st.running = true;
                st.models_reachable = true;
                st.error.clear();
            } else {
                st.models_reachable = false;
                if (st.source == "config_override") {
                    st.running = false;
                }
                if (st.error.empty()) {
                    st.error = redactSecrets(r.error, {opts.bearer_token});
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        snapshot_.gateway = st;
        snapshot_.route.summary = buildRouteSummary(snapshot_.route, snapshot_.gateway);
        snapshot_.updated_at_ms = nowEpochMs();
    }
    return st;
}

std::vector<HarnessInfo> MagpieClient::fetchHarnesses(std::vector<std::string>* warnings) {
    ProcessOutput out = runMagpie({"ls"}, 8000);
    if (!out.spawned || out.canceled || out.timed_out || out.exit_code != 0) {
        if (warnings) {
            std::string msg = !out.spawn_error.empty()
                                  ? out.spawn_error
                                  : ("magpie ls exited with code " + std::to_string(out.exit_code));
            warnings->push_back(msg);
        }
        return {};
    }
    auto harnesses = parseHarnessesText(out.combinedText());
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        snapshot_.harnesses = harnesses;
        snapshot_.updated_at_ms = nowEpochMs();
    }
    return harnesses;
}

std::vector<ModelInfo> MagpieClient::fetchModels(bool allow_cli_fallback,
                                                 std::vector<std::string>* warnings) {
    GatewayStatus gw = probeGatewayStatus(false);
    std::string base_url = resolveGatewayBaseUrl(gw);

    std::vector<ModelInfo> http_models;
    if (!base_url.empty()) {
        HttpRequestOptions opts;
        opts.connect_timeout_ms = 800;
        opts.timeout_ms = 3000;
        opts.bearer_token = currentGatewayKey();
        opts.cancel_flag = &shutting_down_;
        HttpResponse r = http_->get(base_url + "/v1/models", opts);
        if (r.ok) {
            http_models = parseModelsHttpJson(r.body);
            gw.running = true;
            gw.models_reachable = true;
        } else if (warnings && !r.error.empty()) {
            warnings->push_back("HTTP /v1/models failed: " + redactSecrets(r.error, {opts.bearer_token}));
        }
    }

    std::vector<ModelInfo> cli_models;
    if (allow_cli_fallback || http_models.empty()) {
        ProcessOutput out = runMagpie({"models"}, 8000);
        if (out.spawned && out.exit_code == 0) {
            cli_models = parseModelsCliText(out.combinedText());
        } else if (warnings && http_models.empty()) {
            std::string msg = !out.spawn_error.empty()
                                  ? out.spawn_error
                                  : ("magpie models failed (exit " + std::to_string(out.exit_code) + ")");
            warnings->push_back(msg);
        }
    }

    auto merged = mergeModels(http_models, cli_models);
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        snapshot_.gateway = gw;
        snapshot_.models = merged;
        snapshot_.updated_at_ms = nowEpochMs();
    }
    return merged;
}

std::vector<GroupInfo> MagpieClient::fetchGroups(bool include_details,
                                                 std::vector<std::string>* warnings) {
    ProcessOutput out = runMagpie({"groups"}, 8000);
    if (!out.spawned || out.canceled || out.timed_out || out.exit_code != 0) {
        if (warnings) {
            std::string msg = !out.spawn_error.empty()
                                  ? out.spawn_error
                                  : ("magpie groups failed (exit " + std::to_string(out.exit_code) + ")");
            warnings->push_back(msg);
        }
        return {};
    }
    auto groups = parseGroupsCliText(out.combinedText());
    if (include_details) {
        for (auto& g : groups) {
            if (shutting_down_.load()) {
                break;
            }
            if (auto detail = fetchGroupDetail(g.id)) {
                g = std::move(*detail);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        snapshot_.groups = groups;
        snapshot_.updated_at_ms = nowEpochMs();
    }
    return groups;
}

std::optional<GroupInfo> MagpieClient::fetchGroupDetail(const std::string& group_id,
                                                        std::string* error_out) {
    if (!isValidGroupId(group_id)) {
        if (error_out) {
            *error_out = "invalid group id: " + group_id;
        }
        return std::nullopt;
    }
    ProcessOutput out = runMagpie({"group", group_id}, 6000);
    if (!out.spawned || out.exit_code != 0) {
        if (error_out) {
            *error_out = !out.spawn_error.empty()
                             ? out.spawn_error
                             : stripPluginLogLines(out.combinedText());
        }
        return std::nullopt;
    }
    return parseGroupDetailText(out.combinedText());
}

std::vector<QuotaEntry> MagpieClient::fetchQuotas(std::vector<std::string>* warnings) {
    GatewayStatus gw = probeGatewayStatus(false);
    std::string base_url = resolveGatewayBaseUrl(gw);

    if (!base_url.empty()) {
        HttpRequestOptions opts;
        opts.connect_timeout_ms = 800;
        opts.timeout_ms = 3000;
        opts.bearer_token = currentGatewayKey();
        opts.cancel_flag = &shutting_down_;
        HttpResponse r = http_->get(base_url + "/v1/magpie/quotas", opts);
        if (r.ok) {
            auto quotas = parseQuotasJson(r.body, warnings);
            {
                std::lock_guard<std::mutex> lock(state_mu_);
                snapshot_.quotas = quotas;
                snapshot_.updated_at_ms = nowEpochMs();
            }
            return quotas;
        } else if (warnings && !r.error.empty()) {
            warnings->push_back("HTTP /v1/magpie/quotas failed: " +
                                redactSecrets(r.error, {opts.bearer_token}));
        }
    }

    ProcessOutput out = runMagpie({"quota", "--json"}, 10000);
    if (!out.spawned || out.exit_code != 0) {
        if (warnings) {
            std::string msg = !out.spawn_error.empty()
                                  ? out.spawn_error
                                  : ("magpie quota --json failed (exit " + std::to_string(out.exit_code) + ")");
            warnings->push_back(msg);
        }
        return {};
    }
    auto quotas = parseQuotasJson(out.combinedText(), warnings);
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        snapshot_.quotas = quotas;
        snapshot_.updated_at_ms = nowEpochMs();
    }
    return quotas;
}

RouterSnapshot MagpieClient::getCachedSnapshot() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return snapshot_;
}

RouterSnapshot MagpieClient::refreshAllSnapshot(bool probe_http_and_cli) {
    std::vector<std::string> warnings;
    GatewayStatus gw = probeGatewayStatus(probe_http_and_cli);
    if (probe_http_and_cli && !shutting_down_.load()) {
        fetchModels(true, &warnings);
    }
    if (probe_http_and_cli && !shutting_down_.load()) {
        fetchQuotas(&warnings);
    }
    if (probe_http_and_cli && !shutting_down_.load()) {
        fetchGroups(false, &warnings);
    }
    if (probe_http_and_cli && !shutting_down_.load()) {
        fetchHarnesses(&warnings);
    }
    std::lock_guard<std::mutex> lock(state_mu_);
    snapshot_.gateway = gw;
    snapshot_.warnings = std::move(warnings);
    snapshot_.route.summary = buildRouteSummary(snapshot_.route, snapshot_.gateway);
    snapshot_.updated_at_ms = nowEpochMs();
    return snapshot_;
}

omnidesk::CapabilityResult MagpieClient::makeError(const std::string& code,
                                                   const std::string& message,
                                                   const nlohmann::json& extra) {
    omnidesk::CapabilityResult res;
    res.ok = false;
    res.error = message;
    res.provider_plugin_id = "plugin-magpie-agent";
    res.data = extra.is_object() ? extra : nlohmann::json::object();
    res.data["error_code"] = code;
    return res;
}

omnidesk::CapabilityResult MagpieClient::applyRoute(const nlohmann::json& req) {
    if (!req.is_object()) {
        return makeError("invalid_argument", "set_route request must be a JSON object");
    }

    const auto s = currentSettings();
    std::string group_id = req.value("route_group_id", "");
    if (group_id.empty()) {
        group_id = s.route_group_id;
    }
    if (!isValidGroupId(group_id)) {
        return makeError("invalid_route_spec",
                         "route_group_id must match ^[a-z0-9][a-z0-9._-]*$ (got '" + group_id + "')");
    }

    std::string model = req.value("model", req.value("primary_model", req.value("active_model", "")));
    if (model.empty()) {
        return makeError("invalid_argument", "set_route requires non-empty 'model'");
    }

    std::string effort = req.value("effort", "");
    if (!isValidEffort(effort)) {
        return makeError("invalid_route_spec", "unsupported effort level: " + effort);
    }
    const bool fast = req.value("fast", false);
    const std::string account = req.value("account", "");

    std::string routing = req.value("routing", s.routing_strategy);
    if (!isValidRoutingStrategy(routing)) {
        return makeError("invalid_route_spec", "unsupported routing strategy: " + routing);
    }

    std::string stays = req.value("stays", s.stays_policy);
    if (!isValidStaysPolicy(stays)) {
        return makeError("invalid_route_spec", "unsupported stays policy: " + stays);
    }

    std::string spec_err;
    std::string primary_member = buildMemberSpec(model, effort, fast, account, &spec_err);
    if (primary_member.empty()) {
        return makeError("invalid_route_spec", spec_err);
    }

    std::vector<std::string> members;
    std::vector<std::string> normalized_fallbacks;
    std::unordered_set<std::string> seen;
    members.push_back(primary_member);
    seen.insert(primary_member);

    for (const auto& raw_fb : parseFallbackList(req)) {
        std::string fb_err;
        std::string norm = buildMemberSpec(raw_fb, "", false, "", &fb_err);
        if (norm.empty()) {
            return makeError("invalid_route_spec", "invalid fallback '" + raw_fb + "': " + fb_err);
        }
        if (seen.insert(norm).second) {
            normalized_fallbacks.push_back(norm);
            members.push_back(norm);
        }
    }

    bool any_group_member = false;
    for (const auto& m : members) {
        if (m.rfind("group/", 0) == 0 || m.rfind("auto/", 0) == 0) {
            any_group_member = true;
            break;
        }
    }

    std::vector<GroupInfo> existing_groups;
    if (any_group_member) {
        existing_groups = fetchGroups(false, nullptr);
    }
    std::string cycle_reason;
    if (wouldCreateGroupCycle(group_id, members, existing_groups, &cycle_reason)) {
        return makeError("group_cycle_detected",
                         "route would create a group cycle: " + cycle_reason);
    }

    std::string models_csv;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i > 0) {
            models_csv += ",";
        }
        models_csv += members[i];
    }

    const bool dry_run = req.value("dry_run", false);
    if (dry_run) {
        nlohmann::json cmds = nlohmann::json::array();
        cmds.push_back({"magpie", "group", "set", group_id, "models=" + models_csv,
                        "routing=" + routing, "stays=" + stays});
        if (routing == "manual" && primary_member.rfind("group/", 0) != 0 &&
            primary_member.rfind("auto/", 0) != 0) {
            cmds.push_back({"magpie", "group", "pick", group_id, primary_member});
        }
        omnidesk::CapabilityResult res;
        res.ok = true;
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {
            {"dry_run", true},
            {"route_group_id", group_id},
            {"group_ref", "group/" + group_id},
            {"primary_member", primary_member},
            {"fallbacks", normalized_fallbacks},
            {"members", members},
            {"routing", routing},
            {"stays", stays},
            {"planned_commands", cmds},
        };
        return res;
    }

    std::string exe_err;
    if (resolveMagpieExecutable(&exe_err).empty()) {
        return makeError("magpie_not_found", exe_err);
    }

    ProcessOutput probe = runMagpie({"group", group_id}, 6000);
    const bool group_exists = (probe.spawned && probe.exit_code == 0);

    ProcessOutput write_res;
    if (group_exists) {
        write_res = runMagpie(
            {"group", "set", group_id, "models=" + models_csv, "routing=" + routing, "stays=" + stays},
            10000);
    } else {
        write_res = runMagpie(
            {"group", "add", group_id, "models=" + models_csv, "routing=" + routing, "stays=" + stays},
            10000);
        if (write_res.spawned && write_res.exit_code == 0) {
            runMagpie({"group", "set", group_id, "name=OmniDesk Active Route"}, 6000);
        }
    }

    const std::string key = currentGatewayKey();
    if (!write_res.spawned || write_res.exit_code != 0) {
        std::string msg = !write_res.spawn_error.empty()
                              ? write_res.spawn_error
                              : stripPluginLogLines(write_res.combinedText());
        if (msg.empty()) {
            msg = "magpie group command failed with exit code " + std::to_string(write_res.exit_code);
        }
        return makeError("cli_failed", redactSecrets(msg, {key}));
    }

    if (routing == "manual" && primary_member.rfind("group/", 0) != 0 &&
        primary_member.rfind("auto/", 0) != 0) {
        ProcessOutput pick_res = runMagpie({"group", "pick", group_id, primary_member}, 6000);
        if (!pick_res.spawned || pick_res.exit_code != 0) {
            return makeError("cli_failed",
                             redactSecrets("magpie group pick failed: " +
                                               stripPluginLogLines(pick_res.combinedText()),
                                           {key}));
        }
    }

    RouteSpec next_route;
    next_route.configured = true;
    next_route.route_group_id = group_id;
    next_route.model = model;
    next_route.effort = effort;
    next_route.fast = fast;
    next_route.account = account;
    next_route.primary_member = primary_member;
    next_route.fallbacks = normalized_fallbacks;
    next_route.members = members;
    next_route.routing = routing;
    next_route.stays = stays;

    GatewayStatus gw = probeGatewayStatus(false);
    next_route.summary = buildRouteSummary(next_route, gw);

    {
        std::lock_guard<std::mutex> lock(state_mu_);
        settings_.route_group_id = group_id;
        settings_.routing_strategy = routing;
        settings_.stays_policy = stays;
        snapshot_.gateway = gw;
        snapshot_.route = next_route;
        snapshot_.updated_at_ms = nowEpochMs();
    }

    nlohmann::json payload;
    {
        std::lock_guard<std::mutex> clock(ctx_mu_);
        if (ctx_) {
            nlohmann::json cfg = ctx_->loadConfig();
            if (!cfg.is_object()) {
                cfg = nlohmann::json::object();
            }
            cfg["route_group_id"] = group_id;
            cfg["routing_strategy"] = routing;
            cfg["stays_policy"] = stays;
            cfg["active_route"] = {
                {"configured", true},
                {"route_group_id", group_id},
                {"model", model},
                {"effort", effort},
                {"fast", fast},
                {"account", account},
                {"primary_member", primary_member},
                {"fallbacks", normalized_fallbacks},
                {"members", members},
                {"routing", routing},
                {"stays", stays},
                {"summary", next_route.summary},
            };
            ctx_->saveConfig(cfg);
        }
    }

    payload = next_route.toJson(gw);
    // Publish without holding state_mu_ so subscribers can safely query current_route.
    {
        std::lock_guard<std::mutex> clock(ctx_mu_);
        if (ctx_) {
            ctx_->publish("entity.ai.route_changed", payload);
            ctx_->logInfo("Updated Magpie active route: " + next_route.summary);
        }
    }

    omnidesk::CapabilityResult res;
    res.ok = true;
    res.provider_plugin_id = "plugin-magpie-agent";
    res.data = std::move(payload);
    return res;
}

omnidesk::CapabilityResult MagpieClient::handleRouterRpc(const nlohmann::json& req) {
    const std::string action = req.is_object() ? req.value("action", "current_route") : "current_route";

    if (action == "current_route") {
        const bool do_refresh = req.is_object() && req.value("refresh", false);
        GatewayStatus gw = probeGatewayStatus(do_refresh);
        RouterSnapshot snap = getCachedSnapshot();
        snap.gateway = gw;
        snap.route.summary = buildRouteSummary(snap.route, gw);

        omnidesk::CapabilityResult res;
        res.ok = true;
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = snap.route.toJson(gw);
        return res;
    }

    if (action == "gateway_status") {
        const bool check_http = !req.is_object() || req.value("check_http", true);
        GatewayStatus gw = probeGatewayStatus(check_http);
        omnidesk::CapabilityResult res;
        res.ok = true;
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = gw.toJson();
        return res;
    }

    if (action == "list_harnesses") {
        std::vector<std::string> warnings;
        auto harnesses = fetchHarnesses(&warnings);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& h : harnesses) {
            arr.push_back(h.toJson());
        }
        omnidesk::CapabilityResult res;
        res.ok = warnings.empty() || !harnesses.empty();
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {
            {"harnesses", std::move(arr)},
            {"count", harnesses.size()},
            {"warnings", std::move(warnings)},
        };
        if (!res.ok) {
            res.error = res.data["warnings"].empty() ? "failed to list harnesses"
                                                     : res.data["warnings"][0].get<std::string>();
            res.data["error_code"] = "cli_failed";
        }
        return res;
    }

    if (action == "list_models") {
        const bool include_groups = !req.is_object() || req.value("include_groups", true);
        const std::string provider_filter = req.is_object() ? req.value("provider", "") : "";
        std::vector<std::string> warnings;
        auto models = fetchModels(true, &warnings);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : models) {
            if (!include_groups && m.is_group) {
                continue;
            }
            if (!provider_filter.empty() && m.provider != provider_filter) {
                continue;
            }
            arr.push_back(m.toJson());
        }

        omnidesk::CapabilityResult res;
        res.ok = !arr.empty() || warnings.empty();
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {
            {"models", std::move(arr)},
            {"count", models.size()},
            {"warnings", std::move(warnings)},
        };
        if (!res.ok) {
            res.error = res.data["warnings"][0].get<std::string>();
            res.data["error_code"] = "service_offline";
        }
        return res;
    }

    if (action == "list_groups") {
        const std::string group_id = req.is_object() ? req.value("group_id", "") : "";
        if (!group_id.empty()) {
            std::string err;
            auto detail = fetchGroupDetail(group_id, &err);
            if (!detail) {
                return makeError("not_found", err.empty() ? ("group not found: " + group_id) : err);
            }
            omnidesk::CapabilityResult res;
            res.ok = true;
            res.provider_plugin_id = "plugin-magpie-agent";
            res.data = {{"group", detail->toJson()}, {"groups", nlohmann::json::array({detail->toJson()})}};
            return res;
        }

        std::vector<std::string> warnings;
        auto groups = fetchGroups(req.is_object() && req.value("include_details", false), &warnings);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& g : groups) {
            arr.push_back(g.toJson());
        }
        omnidesk::CapabilityResult res;
        res.ok = warnings.empty() || !groups.empty();
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {{"groups", std::move(arr)}, {"warnings", std::move(warnings)}};
        if (!res.ok) {
            res.error = res.data["warnings"][0].get<std::string>();
            res.data["error_code"] = "cli_failed";
        }
        return res;
    }

    if (action == "list_quotas") {
        const std::string provider_filter = req.is_object() ? req.value("provider", "") : "";
        const std::string account_filter = req.is_object() ? req.value("account", "") : "";
        std::vector<std::string> warnings;
        auto quotas = fetchQuotas(&warnings);

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& q : quotas) {
            if (!provider_filter.empty() && q.provider != provider_filter) {
                continue;
            }
            if (!account_filter.empty() && q.user != account_filter && q.alias != account_filter) {
                continue;
            }
            arr.push_back(q.toJson());
        }
        omnidesk::CapabilityResult res;
        res.ok = !quotas.empty() || warnings.empty();
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {
            {"quotas", std::move(arr)},
            {"count", quotas.size()},
            {"warnings", std::move(warnings)},
        };
        if (!res.ok) {
            res.error = res.data["warnings"][0].get<std::string>();
            res.data["error_code"] = "service_offline";
        }
        return res;
    }

    if (action == "set_route" || action == "apply_route") {
        return applyRoute(req);
    }

    return makeError("invalid_argument", "unsupported llm.router.v1 action: " + action);
}

omnidesk::CapabilityResult MagpieClient::callChatCompletionsStream(
    const nlohmann::json& body,
    const std::string& route_used,
    int timeout_override_ms,
    const std::shared_ptr<omnidesk::CancelToken>& cancel,
    const omnidesk::StreamCallback& on_chunk) {
    if (shutting_down_.load() || (cancel && cancel->isCancelled())) {
        auto err = makeError("canceled", "request cancelled");
        err.cancelled = true;
        return err;
    }

    GatewayStatus gw = probeGatewayStatus(false);
    std::string base_url = resolveGatewayBaseUrl(gw);
    if (base_url.empty()) {
        gw = probeGatewayStatus(true);
        base_url = resolveGatewayBaseUrl(gw);
    }
    if (base_url.empty()) {
        return makeError("service_offline",
                         "Magpie gateway is not running or URL is unavailable (start Magpie or configure gateway_url).");
    }

    const auto s = currentSettings();
    const std::string key = currentGatewayKey();

    HttpRequestOptions opts;
    opts.connect_timeout_ms = 2000;
    opts.timeout_ms = timeout_override_ms > 0 ? timeout_override_ms : s.request_timeout_ms;
    opts.bearer_token = key;
    opts.cancel_flag = &shutting_down_;
    opts.cancel_token = cancel;

    const bool want_stream = body.value("stream", false);

    if (!want_stream) {
        HttpResponse http_res = http_->postJson(base_url + "/v1/chat/completions", body.dump(), opts);
        if (http_res.canceled || (cancel && cancel->isCancelled())) {
            auto err = makeError("canceled", "chat completion cancelled");
            err.cancelled = true;
            return err;
        }
        if (http_res.timed_out) {
            return makeError("timeout", "Magpie /v1/chat/completions timed out after " +
                                            std::to_string(opts.timeout_ms) + " ms");
        }
        if (http_res.connection_refused) {
            return makeError("service_offline",
                             redactSecrets("Magpie gateway unreachable at " + base_url + ": " + http_res.error, {key}));
        }
        if (!http_res.ok) {
            std::string api_err = extractJsonErrorMessage(http_res.body);
            std::string msg = !api_err.empty() ? api_err : http_res.error;
            msg = redactSecrets(msg, {key});

            if (http_res.status_code == 401 || http_res.status_code == 403) {
                return makeError("unauthorized", "Gateway authentication failed: " + msg,
                                 {{"http_status", http_res.status_code}});
            }
            if (http_res.status_code == 429) {
                bool is_quota = (msg.find("quota") != std::string::npos ||
                                 msg.find("limit") != std::string::npos);
                return makeError(is_quota ? "quota_exceeded" : "rate_limited", msg,
                                 {{"http_status", http_res.status_code}});
            }
            if (http_res.status_code == 404 && !getCachedSnapshot().route.configured &&
                route_used == "group/" + s.route_group_id) {
                return makeError("route_not_configured",
                                 "Route group '" + route_used +
                                     "' does not exist in Magpie yet. Configure a route via llm.router.v1 (set_route) or Plugin Settings.",
                                 {{"http_status", http_res.status_code}});
            }
            return makeError("upstream_error",
                             "Magpie HTTP " + std::to_string(http_res.status_code) + ": " + msg,
                             {{"http_status", http_res.status_code}});
        }

        auto parsed = nlohmann::json::parse(http_res.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return makeError("invalid_response", "Invalid JSON received from Magpie /v1/chat/completions");
        }
        if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty()) {
            std::string err_msg = extractJsonErrorMessage(http_res.body);
            return makeError("invalid_response",
                             err_msg.empty() ? "Magpie response contained no choices"
                                             : redactSecrets(err_msg, {key}));
        }

        const auto& choice0 = parsed["choices"][0];
        std::string reply = extractAssistantContent(choice0);
        std::string finish_reason = choice0.value("finish_reason", "");
        std::string served_model = parsed.value("model", route_used);
        nlohmann::json usage = parsed.contains("usage") ? parsed["usage"] : nlohmann::json::object();

        if (on_chunk && !reply.empty()) {
            on_chunk(omnidesk::StreamChunk{
                reply, {{"model", served_model}, {"route_used", route_used}}, "delta", false});
        }

        omnidesk::CapabilityResult res;
        res.ok = true;
        res.provider_plugin_id = "plugin-magpie-agent";
        res.data = {
            {"reply", reply},
            {"model", served_model},
            {"route_used", route_used},
            {"finish_reason", finish_reason},
            {"usage", usage},
            {"message", {{"role", "assistant"}, {"content", reply}}},
        };
        return res;
    }

    // Streaming path (`stream: true`): handle both `text/event-stream` and fallback JSON responses.
    omnidesk::SseParser sse;
    bool decided_sse = false;
    bool is_sse = false;
    std::string probe_prefix;
    std::string accumulated_reply;
    std::string served_model = route_used;
    std::string finish_reason;
    nlohmann::json usage = nlohmann::json::object();
    std::string stream_err;

    auto processSseEvent = [&](const std::string& /*event_name*/, const std::string& payload) {
        std::string trimmed = payload;
        while (!trimmed.empty() &&
               (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\r' ||
                trimmed.front() == '\n')) {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() &&
               (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r' ||
                trimmed.back() == '\n')) {
            trimmed.pop_back();
        }
        if (trimmed.empty() || trimmed == "[DONE]") {
            return;
        }
        auto j = nlohmann::json::parse(trimmed, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return;
        }
        if (j.contains("error")) {
            std::string e = extractJsonErrorMessage(trimmed);
            if (!e.empty()) {
                stream_err = redactSecrets(e, {key});
            }
            return;
        }
        if (j.contains("model") && j["model"].is_string()) {
            served_model = j["model"].get<std::string>();
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            usage = j["usage"];
        }
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& c0 = j["choices"][0];
            if (c0.contains("finish_reason") && c0["finish_reason"].is_string()) {
                finish_reason = c0["finish_reason"].get<std::string>();
            }
            std::string delta_text;
            if (c0.contains("delta") && c0["delta"].is_object() &&
                c0["delta"].contains("content") && c0["delta"]["content"].is_string()) {
                delta_text = c0["delta"]["content"].get<std::string>();
            } else if (c0.contains("text") && c0["text"].is_string()) {
                delta_text = c0["text"].get<std::string>();
            } else {
                delta_text = extractAssistantContent(c0);
            }
            if (!delta_text.empty()) {
                accumulated_reply += delta_text;
                if (on_chunk) {
                    on_chunk(omnidesk::StreamChunk{
                        delta_text,
                        {{"model", served_model}, {"route_used", route_used}},
                        "delta",
                        false});
                }
            }
        }
    };

    HttpResponse http_res = http_->postJsonStream(
        base_url + "/v1/chat/completions", body.dump(), opts,
        [&](const char* data, std::size_t size, const std::string& content_type) -> bool {
            if (shutting_down_.load() || (cancel && cancel->isCancelled())) {
                return false;
            }
            if (!decided_sse) {
                if (content_type.find("text/event-stream") != std::string::npos) {
                    decided_sse = true;
                    is_sse = true;
                } else if (content_type.find("application/json") != std::string::npos) {
                    decided_sse = true;
                    is_sse = false;
                } else {
                    probe_prefix.append(data, size);
                    std::size_t i = 0;
                    while (i < probe_prefix.size() &&
                           (probe_prefix[i] == ' ' || probe_prefix[i] == '\t' ||
                            probe_prefix[i] == '\r' || probe_prefix[i] == '\n')) {
                        ++i;
                    }
                    if (i < probe_prefix.size()) {
                        decided_sse = true;
                        is_sse = (probe_prefix.compare(i, 5, "data:") == 0 ||
                                  probe_prefix.compare(i, 6, "event:") == 0 ||
                                  probe_prefix[i] == ':');
                        if (is_sse) {
                            sse.feed(probe_prefix, processSseEvent);
                        }
                        probe_prefix.clear();
                    }
                    return true;
                }
            }
            if (is_sse) {
                sse.feed(std::string_view(data, size), processSseEvent);
            }
            return true;
        });

    if (is_sse) {
        sse.flush(processSseEvent);
    }

    if (http_res.canceled || (cancel && cancel->isCancelled())) {
        auto err = makeError("canceled", "chat completion cancelled");
        err.cancelled = true;
        return err;
    }
    if (http_res.timed_out) {
        return makeError("timeout", "Magpie /v1/chat/completions timed out after " +
                                        std::to_string(opts.timeout_ms) + " ms");
    }
    if (http_res.connection_refused) {
        return makeError("service_offline",
                         redactSecrets("Magpie gateway unreachable at " + base_url + ": " + http_res.error, {key}));
    }
    if (!http_res.ok) {
        std::string api_err = extractJsonErrorMessage(http_res.body);
        std::string msg = !api_err.empty() ? api_err : http_res.error;
        msg = redactSecrets(msg, {key});
        if (http_res.status_code == 401 || http_res.status_code == 403) {
            return makeError("unauthorized", "Gateway authentication failed: " + msg,
                             {{"http_status", http_res.status_code}});
        }
        if (http_res.status_code == 429) {
            bool is_quota = (msg.find("quota") != std::string::npos ||
                             msg.find("limit") != std::string::npos);
            return makeError(is_quota ? "quota_exceeded" : "rate_limited", msg,
                             {{"http_status", http_res.status_code}});
        }
        return makeError("upstream_error",
                         "Magpie HTTP " + std::to_string(http_res.status_code) + ": " + msg,
                         {{"http_status", http_res.status_code}});
    }
    if (!stream_err.empty()) {
        return makeError("upstream_error", stream_err);
    }

    if (!is_sse && !http_res.body.empty()) {
        auto parsed = nlohmann::json::parse(http_res.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() &&
            parsed.contains("choices") && parsed["choices"].is_array() && !parsed["choices"].empty()) {
            const auto& choice0 = parsed["choices"][0];
            accumulated_reply = extractAssistantContent(choice0);
            finish_reason = choice0.value("finish_reason", "");
            served_model = parsed.value("model", route_used);
            if (parsed.contains("usage") && parsed["usage"].is_object()) {
                usage = parsed["usage"];
            }
            if (on_chunk && !accumulated_reply.empty()) {
                on_chunk(omnidesk::StreamChunk{
                    accumulated_reply,
                    {{"model", served_model}, {"route_used", route_used}},
                    "delta",
                    false});
            }
        } else {
            return makeError("invalid_response", "Invalid JSON received from Magpie /v1/chat/completions");
        }
    }

    omnidesk::CapabilityResult res;
    res.ok = true;
    res.provider_plugin_id = "plugin-magpie-agent";
    res.data = {
        {"reply", accumulated_reply},
        {"model", served_model},
        {"route_used", route_used},
        {"finish_reason", finish_reason},
        {"usage", usage},
        {"message", {{"role", "assistant"}, {"content", accumulated_reply}}},
    };
    return res;
}

omnidesk::CapabilityResult MagpieClient::handleAgentRpc(const nlohmann::json& req) {
    return handleAgentRpcStream(req, nullptr, nullptr);
}

omnidesk::CapabilityResult MagpieClient::handleAgentRpcStream(
    const nlohmann::json& req,
    const std::shared_ptr<omnidesk::CancelToken>& cancel,
    const omnidesk::StreamCallback& on_chunk) {
    if (!req.is_object()) {
        return makeError("invalid_argument", "ai.agent.v1 request must be a JSON object");
    }

    const std::string action = req.value("action", "chat");
    const std::string domain = req.value("domain", "core");
    const std::string prompt = req.value("prompt", "");
    const std::string system_override = req.value("system_prompt", "");
    const std::string target_lang = req.value("target_lang", "zh-CN");

    const bool has_messages = req.contains("messages") && req["messages"].is_array() && !req["messages"].empty();
    if (prompt.empty() && !has_messages) {
        return makeError("invalid_argument", "ai.agent.v1 requires non-empty 'prompt' or 'messages'");
    }

    const auto s = currentSettings();
    std::string route_used = req.value("route_override", "");
    if (route_used.empty()) {
        route_used = "group/" + s.route_group_id;
    }

    std::string sys_prompt = !system_override.empty()
                                 ? system_override
                                 : buildAgentActionInstruction(action, domain, target_lang);
    if (req.contains("context") && !req["context"].is_null()) {
        std::string ctx_str;
        if (req["context"].is_string()) {
            ctx_str = req["context"].get<std::string>();
        } else {
            ctx_str = req["context"].dump(2);
        }
        if (!ctx_str.empty()) {
            sys_prompt += "\n\nWorkspace Context:\n" + ctx_str;
        }
    }

    nlohmann::json messages = nlohmann::json::array();
    messages.push_back({{"role", "system"}, {"content", sys_prompt}});
    if (has_messages) {
        for (const auto& m : req["messages"]) {
            if (m.is_object() && m.contains("role") && m.contains("content")) {
                messages.push_back(m);
            }
        }
    }
    if (!prompt.empty()) {
        messages.push_back({{"role", "user"}, {"content", prompt}});
    }

    nlohmann::json body = {
        {"model", route_used},
        {"messages", std::move(messages)},
        {"stream", req.value("stream", false)},
    };
    if (req.contains("temperature") && req["temperature"].is_number()) {
        body["temperature"] = req["temperature"];
    }
    if (req.contains("max_tokens") && req["max_tokens"].is_number_integer()) {
        body["max_tokens"] = req["max_tokens"];
    }
    if (req.contains("reasoning_effort") && req["reasoning_effort"].is_string() &&
        !req["reasoning_effort"].get<std::string>().empty()) {
        body["reasoning_effort"] = req["reasoning_effort"];
    }

    return callChatCompletionsStream(body, route_used, req.value("timeout_ms", 0), cancel, on_chunk);
}

omnidesk::CapabilityResult MagpieClient::handleChatRpc(const nlohmann::json& req) {
    return handleChatRpcStream(req, nullptr, nullptr);
}

omnidesk::CapabilityResult MagpieClient::handleChatRpcStream(
    const nlohmann::json& req,
    const std::shared_ptr<omnidesk::CancelToken>& cancel,
    const omnidesk::StreamCallback& on_chunk) {
    if (!req.is_object()) {
        return makeError("invalid_argument", "llm.chat.v1 request must be a JSON object");
    }

    nlohmann::json messages = nlohmann::json::array();
    if (req.contains("system_prompt") && req["system_prompt"].is_string() &&
        !req["system_prompt"].get<std::string>().empty()) {
        messages.push_back({{"role", "system"}, {"content", req["system_prompt"]}});
    }
    if (req.contains("messages") && req["messages"].is_array()) {
        for (const auto& m : req["messages"]) {
            if (m.is_object() && m.contains("role") && m.contains("content")) {
                messages.push_back(m);
            }
        }
    } else if (req.contains("prompt") && req["prompt"].is_string() &&
               !req["prompt"].get<std::string>().empty()) {
        messages.push_back({{"role", "user"}, {"content", req["prompt"]}});
    }

    if (messages.empty()) {
        return makeError("invalid_argument", "llm.chat.v1 requires non-empty 'messages' or 'prompt'");
    }

    const auto s = currentSettings();
    std::string route_used = req.value("route_override", "");
    if (route_used.empty()) {
        route_used = "group/" + s.route_group_id;
    }

    const bool has_response_format = req.contains("response_format") && req["response_format"].is_object();
    nlohmann::json body = {
        {"model", route_used},
        {"messages", std::move(messages)},
        {"stream", !has_response_format && req.value("stream", false)},
    };
    if (req.contains("temperature") && req["temperature"].is_number()) {
        body["temperature"] = req["temperature"];
    }
    if (req.contains("max_tokens") && req["max_tokens"].is_number_integer()) {
        body["max_tokens"] = req["max_tokens"];
    }
    if (req.contains("reasoning_effort") && req["reasoning_effort"].is_string() &&
        !req["reasoning_effort"].get<std::string>().empty()) {
        body["reasoning_effort"] = req["reasoning_effort"];
    }
    if (has_response_format) {
        body["response_format"] = req["response_format"];
    }

    auto res = callChatCompletionsStream(body, route_used, req.value("timeout_ms", 0), cancel, on_chunk);
    if (res.ok && has_response_format && res.data.contains("reply") &&
        res.data["reply"].is_string()) {
        auto parsed = nlohmann::json::parse(res.data["reply"].get<std::string>(), nullptr, false);
        if (!parsed.is_discarded()) {
            res.data["parsed_json"] = std::move(parsed);
        }
    }
    return res;
}

omnidesk::CapabilityResult MagpieClient::handleTranslateRpc(const nlohmann::json& req) {
    return handleTranslateRpcStream(req, nullptr, nullptr);
}

omnidesk::CapabilityResult MagpieClient::handleTranslateRpcStream(
    const nlohmann::json& req,
    const std::shared_ptr<omnidesk::CancelToken>& cancel,
    const omnidesk::StreamCallback& on_chunk) {
    if (!req.is_object()) {
        return makeError("invalid_argument", "llm.translate.v1 request must be a JSON object");
    }

    const std::string text = req.value("text", "");
    if (text.empty()) {
        return makeError("invalid_argument", "llm.translate.v1 requires non-empty 'text'");
    }

    const std::string source_lang = req.value("source_lang", "auto");
    const std::string target_lang = req.value("target_lang", "zh-CN");
    const std::string ctx_before = req.value("context_before", "");
    const std::string ctx_after = req.value("context_after", "");
    const bool preserve_formatting = req.value("preserve_formatting", true);
    const bool bilingual_notes = req.value("bilingual_notes", false);

    const auto s = currentSettings();
    std::string route_used = req.value("route_override", "");
    if (route_used.empty()) {
        route_used = "group/" + s.route_group_id;
    }

    std::string sys = "You are a precise literary and technical translator.";
    if (preserve_formatting) {
        sys += " Preserve paragraph breaks, inline markup, code/identifiers, and punctuation balance.";
    }
    if (!bilingual_notes) {
        sys += " Output ONLY the translated text inside <translation>...</translation> tags, omitting surrounding context.";
    } else {
        sys += " Output <translation>...</translation> followed by concise <notes>...</notes> for key terms.";
    }

    std::string glossary_str;
    if (req.contains("glossary")) {
        glossary_str = formatGlossarySection(req["glossary"]);
    }
    if (!glossary_str.empty()) {
        sys += "\n\nMandatory Glossary (must follow exact translations):\n" + glossary_str;
    }

    std::ostringstream user;
    user << "Source language: " << source_lang << "\nTarget language: " << target_lang << "\n";
    if (!ctx_before.empty()) {
        user << "<context_before>\n" << ctx_before << "\n</context_before>\n";
    }
    user << "<target_text>\n" << text << "\n</target_text>\n";
    if (!ctx_after.empty()) {
        user << "<context_after>\n" << ctx_after << "\n</context_after>\n";
    }

    nlohmann::json body = {
        {"model", route_used},
        {"messages",
         nlohmann::json::array({
             {{"role", "system"}, {"content", sys}},
             {{"role", "user"}, {"content", user.str()}},
         })},
        {"stream", req.value("stream", false)},
    };
    if (req.contains("temperature") && req["temperature"].is_number()) {
        body["temperature"] = req["temperature"];
    } else {
        body["temperature"] = 0.2;
    }
    if (req.contains("max_tokens") && req["max_tokens"].is_number_integer()) {
        body["max_tokens"] = req["max_tokens"];
    }
    if (req.contains("reasoning_effort") && req["reasoning_effort"].is_string() &&
        !req["reasoning_effort"].get<std::string>().empty()) {
        body["reasoning_effort"] = req["reasoning_effort"];
    }

    auto res = callChatCompletionsStream(body, route_used, req.value("timeout_ms", 0), cancel, on_chunk);
    if (!res.ok) {
        return res;
    }

    std::string raw_reply = res.data.value("reply", "");
    std::string translation = raw_reply;
    std::string notes;

    auto extractTag = [&](const std::string& tag) -> std::string {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        std::size_t b = raw_reply.find(open);
        if (b == std::string::npos) {
            return {};
        }
        b += open.size();
        std::size_t e = raw_reply.find(close, b);
        if (e == std::string::npos) {
            return {};
        }
        std::string inner = raw_reply.substr(b, e - b);
        while (!inner.empty() && (inner.front() == '\n' || inner.front() == '\r')) {
            inner.erase(inner.begin());
        }
        while (!inner.empty() && (inner.back() == '\n' || inner.back() == '\r')) {
            inner.pop_back();
        }
        return inner;
    };

    std::string tagged = extractTag("translation");
    if (!tagged.empty()) {
        translation = tagged;
    }
    if (bilingual_notes) {
        notes = extractTag("notes");
    }

    res.data["translation"] = translation;
    res.data["source_lang"] = source_lang;
    res.data["target_lang"] = target_lang;
    if (!notes.empty()) {
        res.data["notes"] = notes;
    }
    return res;
}

nlohmann::json MagpieClient::healthCheckJson() const {
    const auto s = currentSettings();
    const auto snap = getCachedSnapshot();
    const std::string exe = resolveExecutable(s.magpie_bin);
    return {
        {"ok", true},
        {"magpie_found", !exe.empty()},
        {"magpie_bin", exe.empty() ? s.magpie_bin : exe},
        {"gateway_running", snap.gateway.running},
        {"gateway_url", resolveGatewayBaseUrl(snap.gateway)},
        {"route_group", "group/" + s.route_group_id},
        {"route_configured", snap.route.configured},
        {"route_summary", snap.route.summary},
    };
}

} // namespace magpie_agent
