#include "magpie_agent/magpie_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace magpie_agent {

namespace {

std::string trimAscii(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// Matches `YYYY/MM/DD HH:MM:SS plugin` at the start of a trimmed line.
bool isMagpiePluginLogLine(const std::string& trimmed) {
    if (trimmed.size() < 26) {
        return false;
    }
    const bool date_ok = isDigit(trimmed[0]) && isDigit(trimmed[1]) && isDigit(trimmed[2]) &&
                         isDigit(trimmed[3]) && trimmed[4] == '/' && isDigit(trimmed[5]) &&
                         isDigit(trimmed[6]) && trimmed[7] == '/' && isDigit(trimmed[8]) &&
                         isDigit(trimmed[9]) && trimmed[10] == ' ' && isDigit(trimmed[11]) &&
                         isDigit(trimmed[12]) && trimmed[13] == ':' && isDigit(trimmed[14]) &&
                         isDigit(trimmed[15]) && trimmed[16] == ':' && isDigit(trimmed[17]) &&
                         isDigit(trimmed[18]) && trimmed[19] == ' ';
    if (!date_ok) {
        return false;
    }
    return trimmed.compare(20, 7, "plugin:") == 0 || trimmed.compare(20, 8, "plugin [") == 0;
}

// Splits a string wherever 2 or more consecutive ASCII spaces appear.
std::vector<std::string> splitByMultiSpaces(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        while (i < n && s[i] == ' ') {
            ++i;
        }
        if (i >= n) {
            break;
        }
        std::size_t start = i;
        std::size_t end = i;
        while (i < n) {
            if (s[i] == ' ' && (i + 1 < n && s[i + 1] == ' ')) {
                break;
            }
            if (s[i] != ' ') {
                end = i + 1;
            }
            ++i;
        }
        if (end > start) {
            out.push_back(s.substr(start, end - start));
        }
    }
    return out;
}

std::vector<std::string> splitOnDelimiter(const std::string& s, const std::string& delim) {
    std::vector<std::string> out;
    if (delim.empty()) {
        out.push_back(s);
        return out;
    }
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            std::string part = trimAscii(s.substr(start));
            if (!part.empty()) {
                out.push_back(part);
            }
            break;
        }
        std::string part = trimAscii(s.substr(start, pos - start));
        if (!part.empty()) {
            out.push_back(part);
        }
        start = pos + delim.size();
    }
    return out;
}

bool isValidEffortToken(const std::string& token) {
    return token == "none" || token == "minimal" || token == "low" || token == "medium" ||
           token == "high" || token == "xhigh" || token == "max" || token == "ultra";
}

bool looksLikeEffortList(const std::string& s) {
    auto parts = splitOnDelimiter(s, "/");
    if (parts.empty()) {
        return false;
    }
    for (const auto& p : parts) {
        if (!isValidEffortToken(p)) {
            return false;
        }
    }
    return true;
}

bool looksLikeModelIdToken(const std::string& tok) {
    if (tok.empty() || tok.find(' ') != std::string::npos) {
        return false;
    }
    if (startsWith(tok, "http://") || startsWith(tok, "https://")) {
        return false;
    }
    std::size_t slash = tok.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= tok.size()) {
        return false;
    }
    if (tok.back() == ':') {
        return false;
    }
    for (std::size_t i = 0; i < slash; ++i) {
        unsigned char c = static_cast<unsigned char>(tok[i]);
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

std::string stripNotServedNote(const std::string& raw) {
    std::string s = trimAscii(raw);
    const std::string bullet = "\xe2\x97\x8f"; // ●
    if (startsWith(s, bullet)) {
        s = trimAscii(s.substr(bullet.size()));
    }
    if (s.size() >= bullet.size() &&
        s.compare(s.size() - bullet.size(), bullet.size(), bullet) == 0) {
        s = trimAscii(s.substr(0, s.size() - bullet.size()));
    }
    std::size_t paren = s.find(" (");
    if (paren != std::string::npos) {
        s = trimAscii(s.substr(0, paren));
    }
    return s;
}

std::vector<std::string> parseMembersColumn(const std::string& col, std::string* picked_out) {
    std::vector<std::string> members;
    const std::string arrow = " \xe2\x86\x92 "; // " → "
    const std::string dot = " \xc2\xb7 ";     // " · "
    const std::string bullet = "\xe2\x97\x8f"; // "●"

    std::vector<std::string> raw_parts;
    if (col.find(arrow) != std::string::npos) {
        raw_parts = splitOnDelimiter(col, arrow);
    } else if (col.find(dot) != std::string::npos) {
        raw_parts = splitOnDelimiter(col, dot);
    } else {
        raw_parts.push_back(col);
    }

    for (const auto& part : raw_parts) {
        bool has_bullet = (part.find(bullet) != std::string::npos);
        std::string cleaned = stripNotServedNote(part);
        if (!cleaned.empty()) {
            if (has_bullet && picked_out && picked_out->empty()) {
                *picked_out = cleaned;
            }
            members.push_back(cleaned);
        }
    }
    return members;
}

std::string stripSpecDecoratorsForCycle(const std::string& member) {
    std::string base = trimAscii(member);
    std::size_t hash_pos = base.find('#');
    if (hash_pos != std::string::npos) {
        base = base.substr(0, hash_pos);
    }
    if (startsWith(base, "group/") || startsWith(base, "auto/")) {
        std::size_t colon = base.find(':');
        if (colon != std::string::npos) {
            base = base.substr(0, colon);
        }
    }
    return base;
}

bool hasControlOrSpace(const std::string& s) {
    for (unsigned char c : s) {
        if (c <= 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

} // namespace

nlohmann::json GatewayStatus::toJson() const {
    return {
        {"running", running},
        {"models_reachable", models_reachable},
        {"url", url},
        {"addr", addr},
        {"pid", pid},
        {"version", version},
        {"started", started},
        {"source", source},
        {"config_file", config_file},
        {"error", error},
    };
}

nlohmann::json HarnessInfo::toJson() const {
    return {
        {"name", name},
        {"model_or_group", model_or_group},
        {"effort", effort},
        {"sign_in", sign_in},
        {"config_path", config_path},
        {"configured", configured},
    };
}

nlohmann::json ModelInfo::toJson() const {
    return {
        {"id", id},
        {"provider", provider},
        {"model_name", model_name},
        {"display_name", display_name},
        {"magpie_label", magpie_label},
        {"is_group", is_group},
        {"is_auto_group", is_auto_group},
        {"reasoning", reasoning},
        {"fast_supported", fast_supported},
        {"efforts", efforts},
        {"context_length", context_length},
        {"max_output_tokens", max_output_tokens},
        {"input_modalities", input_modalities},
        {"output_modalities", output_modalities},
        {"from_http", from_http},
        {"from_cli", from_cli},
    };
}

nlohmann::json GroupInfo::toJson() const {
    return {
        {"id", id},
        {"ref", ref},
        {"name", name},
        {"is_auto", is_auto},
        {"routing", routing},
        {"stays", stays},
        {"effort", effort},
        {"classifier", classifier},
        {"rules_count", rules_count},
        {"picked_model", picked_model},
        {"members", members},
        {"used_by", used_by},
        {"levels", levels},
    };
}

nlohmann::json QuotaWindow::toJson() const {
    nlohmann::json j = {
        {"name", name},
        {"used", used},
        {"remaining", remaining},
        {"resets_at", resets_at},
        {"display", display},
        {"pool", pool},
    };
    if (has_amount) {
        j["amount"] = amount;
        j["limit"] = limit;
    }
    return j;
}

nlohmann::json QuotaEntry::toJson() const {
    nlohmann::json win_arr = nlohmann::json::array();
    for (const auto& w : windows) {
        win_arr.push_back(w.toJson());
    }
    nlohmann::json j = {
        {"provider", provider},
        {"name", name},
        {"kind", kind},
        {"plan", plan},
        {"user", user},
        {"alias", alias},
        {"error", error},
        {"last", last},
        {"last_served_at", last_served_at},
        {"read_at", read_at},
        {"windows", win_arr},
    };
    if (has_balance) {
        j["balance"] = balance;
        j["balance_display"] = balance_display;
    }
    return j;
}

nlohmann::json RouteSpec::toJson(const GatewayStatus& gateway) const {
    return {
        {"configured", configured},
        {"provider", "magpie"},
        {"route_group_id", route_group_id},
        {"group_ref", groupRef()},
        {"active_model", model},
        {"primary_member", primary_member},
        {"fallbacks", fallbacks},
        {"members", members},
        {"routing", routing},
        {"stays", stays},
        {"effort", effort},
        {"fast", fast},
        {"account", account},
        {"summary", summary},
        {"gateway", gateway.toJson()},
    };
}

std::string stripPluginLogLines(const std::string& text, std::vector<std::string>* captured_logs) {
    std::istringstream iss(text);
    std::string line;
    std::string clean;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trimAscii(line);
        if (isMagpiePluginLogLine(trimmed)) {
            if (captured_logs) {
                captured_logs->push_back(trimmed);
            }
            continue;
        }
        clean += line;
        clean += '\n';
    }
    return clean;
}

nlohmann::json extractJsonAfterLogs(const std::string& text,
                                    std::string* error_out,
                                    std::vector<std::string>* captured_logs) {
    const std::string clean = stripPluginLogLines(text, captured_logs);
    const std::string trimmed = trimAscii(clean);
    if (trimmed.empty()) {
        if (error_out) {
            *error_out = "empty output when expecting JSON";
        }
        return {};
    }

    auto tryParseAt = [&](std::size_t offset) -> nlohmann::json {
        auto parsed = nlohmann::json::parse(trimmed.substr(offset), nullptr, false);
        if (parsed.is_discarded()) {
            return {};
        }
        return parsed;
    };

    if (trimmed[0] == '{' || trimmed[0] == '[') {
        auto direct = tryParseAt(0);
        if (!direct.is_null() && !direct.is_discarded()) {
            return direct;
        }
    }

    // Fallback: locate the first line beginning with '{' or '['.
    std::size_t pos = 0;
    while (pos < trimmed.size()) {
        std::size_t non_ws = pos;
        while (non_ws < trimmed.size() && (trimmed[non_ws] == ' ' || trimmed[non_ws] == '\t')) {
            ++non_ws;
        }
        if (non_ws < trimmed.size() && (trimmed[non_ws] == '{' || trimmed[non_ws] == '[')) {
            auto parsed = tryParseAt(non_ws);
            if (!parsed.is_null() && !parsed.is_discarded()) {
                return parsed;
            }
        }
        std::size_t nl = trimmed.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }

    if (error_out) {
        *error_out = "failed to parse JSON payload from output";
    }
    return {};
}

GatewayStatus parseGatewayFileJson(const std::string& json_text, const std::string& config_path) {
    GatewayStatus st;
    st.source = "gateway_json";
    st.config_file = config_path;
    auto j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        st.error = "invalid gateway.json format";
        return st;
    }
    st.addr = j.value("addr", "");
    st.pid = j.value("pid", 0);
    st.version = j.value("version", "");
    st.started = j.value("started", "");
    if (!st.addr.empty()) {
        if (startsWith(st.addr, "http://") || startsWith(st.addr, "https://")) {
            st.url = st.addr;
        } else {
            st.url = "http://" + st.addr;
        }
    }
    return st;
}

GatewayStatus parseStatusCliJson(const std::string& raw_output) {
    GatewayStatus st;
    st.source = "cli_status";
    std::string err;
    nlohmann::json j = extractJsonAfterLogs(raw_output, &err);
    if (j.is_null() || !j.is_object()) {
        st.error = err.empty() ? "invalid status JSON" : err;
        return st;
    }
    st.running = j.value("running", false);
    st.url = j.value("url", "");
    st.addr = j.value("addr", "");
    st.pid = j.value("pid", 0);
    st.version = j.value("version", "");
    st.started = j.value("started", "");
    st.config_file = j.value("file", "");
    if (st.url.empty() && !st.addr.empty()) {
        st.url = "http://" + st.addr;
    }
    return st;
}

std::vector<HarnessInfo> parseHarnessesText(const std::string& raw_output) {
    std::vector<HarnessInfo> harnesses;
    const std::string clean = stripPluginLogLines(raw_output);
    std::istringstream iss(clean);
    std::string line;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }
        auto cols = splitByMultiSpaces(trimmed);
        if (cols.size() < 2) {
            continue;
        }

        HarnessInfo h;
        h.raw_line = trimmed;
        h.name = cols[0];

        std::size_t end_idx = cols.size();
        if (end_idx >= 2 && (startsWith(cols[end_idx - 1], "~/") || startsWith(cols[end_idx - 1], "/"))) {
            h.config_path = cols[end_idx - 1];
            --end_idx;
        }

        const std::string middle_dot = "\xc2\xb7"; // "·"
        const std::string em_dash = "\xe2\x80\x94"; // "—"
        bool saw_model = false;
        for (std::size_t i = 1; i < end_idx; ++i) {
            const std::string& tok = cols[i];
            if (tok == middle_dot) {
                continue;
            }
            if (!saw_model) {
                h.model_or_group = tok;
                h.configured = (tok != em_dash && tok != "-");
                saw_model = true;
                continue;
            }
            if (startsWith(tok, "effort ")) {
                std::string v = trimAscii(tok.substr(7));
                if (v != em_dash && v != "-") {
                    h.effort = v;
                }
            } else if (startsWith(tok, "thinking ")) {
                std::string v = trimAscii(tok.substr(9));
                if (v != em_dash && v != "-") {
                    h.effort = v;
                }
            } else if (startsWith(tok, "sign-in ")) {
                h.sign_in = trimAscii(tok.substr(8));
            }
        }
        if (!h.name.empty() && !h.config_path.empty()) {
            harnesses.push_back(std::move(h));
        }
    }
    return harnesses;
}

std::vector<ModelInfo> parseModelsHttpJson(const std::string& json_text) {
    std::vector<ModelInfo> out;
    auto root = nlohmann::json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("data") || !root["data"].is_array()) {
        return out;
    }

    for (const auto& item : root["data"]) {
        if (!item.is_object()) {
            continue;
        }
        std::string id = item.value("id", "");
        if (id.empty()) {
            continue;
        }
        ModelInfo m;
        m.id = id;
        m.from_http = true;
        m.display_name = item.value("display_name", "");
        m.magpie_label = item.value("magpie_label", "");
        m.reasoning = item.value("reasoning", false);
        m.context_length = item.value("context_length", item.value("context_window", 0LL));
        m.max_output_tokens = item.value("max_output_tokens", 0LL);

        std::size_t slash = id.find('/');
        std::string prefix = (slash != std::string::npos) ? id.substr(0, slash) : "";
        m.model_name = (slash != std::string::npos) ? id.substr(slash + 1) : id;
        m.is_auto_group = (prefix == "auto");
        m.is_group = (prefix == "group" || prefix == "auto");
        if (m.is_group) {
            m.provider = item.value("owned_by", prefix);
        } else {
            m.provider = !prefix.empty() ? prefix : item.value("owned_by", "");
        }

        if (item.contains("reasoning_levels") && item["reasoning_levels"].is_array()) {
            for (const auto& lv : item["reasoning_levels"]) {
                if (lv.is_string()) {
                    m.efforts.push_back(lv.get<std::string>());
                }
            }
        } else if (item.contains("supported_reasoning_levels") &&
                   item["supported_reasoning_levels"].is_array()) {
            for (const auto& lv : item["supported_reasoning_levels"]) {
                if (lv.is_object() && lv.contains("effort") && lv["effort"].is_string()) {
                    m.efforts.push_back(lv["effort"].get<std::string>());
                }
            }
        }

        if (item.contains("service_tiers") && item["service_tiers"].is_array()) {
            for (const auto& tier : item["service_tiers"]) {
                if (tier.is_object()) {
                    const std::string tid = tier.value("id", "");
                    const std::string tname = tier.value("name", "");
                    if (tid == "priority" || tname == "Fast") {
                        m.fast_supported = true;
                    }
                }
            }
        }

        if (item.contains("modalities") && item["modalities"].is_object()) {
            const auto& mod = item["modalities"];
            if (mod.contains("input") && mod["input"].is_array()) {
                for (const auto& x : mod["input"]) {
                    if (x.is_string()) {
                        m.input_modalities.push_back(x.get<std::string>());
                    }
                }
            }
            if (mod.contains("output") && mod["output"].is_array()) {
                for (const auto& x : mod["output"]) {
                    if (x.is_string()) {
                        m.output_modalities.push_back(x.get<std::string>());
                    }
                }
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<ModelInfo> parseModelsCliText(const std::string& raw_output) {
    std::vector<ModelInfo> out;
    const std::string clean = stripPluginLogLines(raw_output);
    std::istringstream iss(clean);
    std::string line;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }
        auto cols = splitByMultiSpaces(trimmed);
        if (cols.empty() || !looksLikeModelIdToken(cols[0])) {
            continue;
        }
        ModelInfo m;
        m.id = cols[0];
        m.from_cli = true;
        std::size_t slash = m.id.find('/');
        std::string prefix = m.id.substr(0, slash);
        m.model_name = m.id.substr(slash + 1);
        m.provider = prefix;
        m.is_auto_group = (prefix == "auto");
        m.is_group = (prefix == "group" || prefix == "auto");

        if (cols.size() >= 2) {
            if (looksLikeEffortList(cols.back())) {
                m.efforts = splitOnDelimiter(cols.back(), "/");
                m.reasoning = !m.efforts.empty();
                if (cols.size() >= 3) {
                    m.display_name = cols[1];
                }
            } else {
                m.display_name = cols[1];
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<ModelInfo> mergeModels(const std::vector<ModelInfo>& http_models,
                                   const std::vector<ModelInfo>& cli_models) {
    std::vector<ModelInfo> merged = http_models;
    std::unordered_map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        by_id[merged[i].id] = i;
    }

    for (const auto& cm : cli_models) {
        auto it = by_id.find(cm.id);
        if (it == by_id.end()) {
            by_id[cm.id] = merged.size();
            merged.push_back(cm);
        } else {
            ModelInfo& dst = merged[it->second];
            dst.from_cli = true;
            if (dst.display_name.empty()) {
                dst.display_name = cm.display_name;
            }
            if (dst.efforts.empty() && !cm.efforts.empty()) {
                dst.efforts = cm.efforts;
                dst.reasoning = cm.reasoning;
            }
        }
    }
    return merged;
}

std::vector<GroupInfo> parseGroupsCliText(const std::string& raw_output) {
    std::vector<GroupInfo> groups;
    const std::string clean = stripPluginLogLines(raw_output);
    const std::string dot = " \xc2\xb7 ";     // " · "
    const std::string left_arrow = "\xe2\x86\x90 "; // "← "

    std::istringstream iss(clean);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }
        auto cols = splitByMultiSpaces(trimmed);
        std::size_t ref_idx = cols.size();
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (startsWith(cols[i], "group/") || startsWith(cols[i], "auto/")) {
                ref_idx = i;
                break;
            }
        }
        if (ref_idx >= cols.size()) {
            continue;
        }

        GroupInfo g;
        if (ref_idx > 0) {
            g.name = cols[0];
        }
        g.ref = cols[ref_idx];
        g.is_auto = startsWith(g.ref, "auto/");
        std::size_t slash = g.ref.find('/');
        g.id = (slash != std::string::npos) ? g.ref.substr(slash + 1) : g.ref;

        if (ref_idx + 1 < cols.size()) {
            auto meta_parts = splitOnDelimiter(cols[ref_idx + 1], dot);
            if (!meta_parts.empty()) {
                g.routing = meta_parts[0];
            }
            for (std::size_t i = 1; i < meta_parts.size(); ++i) {
                if (startsWith(meta_parts[i], "stays ")) {
                    g.stays = trimAscii(meta_parts[i].substr(6));
                } else {
                    std::size_t rules_pos = meta_parts[i].find(" rule");
                    if (rules_pos != std::string::npos) {
                        g.rules_count = std::atoi(meta_parts[i].substr(0, rules_pos).c_str());
                    }
                }
            }
        }

        if (ref_idx + 2 < cols.size() && !startsWith(cols[ref_idx + 2], left_arrow)) {
            g.members = parseMembersColumn(cols[ref_idx + 2], &g.picked_model);
        }

        for (std::size_t i = ref_idx + 2; i < cols.size(); ++i) {
            if (startsWith(cols[i], left_arrow)) {
                std::string hb = trimAscii(cols[i].substr(left_arrow.size()));
                g.used_by = splitOnDelimiter(hb, ",");
            }
        }
        groups.push_back(std::move(g));
    }
    return groups;
}

std::optional<GroupInfo> parseGroupDetailText(const std::string& raw_output) {
    const std::string clean = stripPluginLogLines(raw_output);
    std::istringstream iss(clean);
    std::string line;
    GroupInfo g;
    bool found_header = false;
    std::string current_section;

    auto parseMemberLine = [&](const std::string& body) {
        std::string s = trimAscii(body);
        if (s.empty() || !isDigit(s[0])) {
            return;
        }
        std::size_t sp = s.find(' ');
        if (sp == std::string::npos) {
            return;
        }
        std::string rest = trimAscii(s.substr(sp + 1));
        auto parts = splitByMultiSpaces(rest);
        if (parts.empty()) {
            return;
        }
        const std::string bullet = "\xe2\x97\x8f";
        bool has_bullet = (parts[0].find(bullet) != std::string::npos);
        std::string spec = stripNotServedNote(parts[0]);
        if (!spec.empty()) {
            if (has_bullet && g.picked_model.empty()) {
                g.picked_model = spec;
            }
            g.members.push_back(spec);
        }
    };

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }

        if (!found_header) {
            auto cols = splitByMultiSpaces(trimmed);
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (startsWith(cols[i], "group/") || startsWith(cols[i], "auto/")) {
                    found_header = true;
                    g.ref = cols[i];
                    g.is_auto = startsWith(g.ref, "auto/");
                    std::size_t slash = g.ref.find('/');
                    g.id = (slash != std::string::npos) ? g.ref.substr(slash + 1) : g.ref;
                    if (i > 0) {
                        g.name = cols[0];
                    }
                    break;
                }
            }
            continue;
        }

        if (startsWith(trimmed, "routing   ") || startsWith(trimmed, "routing\t") ||
            trimmed == "routing") {
            current_section = "routing";
            std::string rest = trimAscii(trimmed.substr(7));
            std::istringstream rss(rest);
            rss >> g.routing;
        } else if (startsWith(trimmed, "stays     ") || startsWith(trimmed, "stays ") ||
                   startsWith(trimmed, "stays\t")) {
            current_section = "stays";
            std::string rest = trimAscii(trimmed.substr(5));
            std::istringstream sss(rest);
            sss >> g.stays;
        } else if (startsWith(trimmed, "effort    ") || startsWith(trimmed, "effort ")) {
            current_section = "effort";
            std::string rest = trimAscii(trimmed.substr(6));
            std::istringstream ess(rest);
            ess >> g.effort;
        } else if (startsWith(trimmed, "classifier ")) {
            current_section = "classifier";
            auto cols = splitByMultiSpaces(trimAscii(trimmed.substr(10)));
            if (!cols.empty()) {
                g.classifier = cols[0];
            }
        } else if (startsWith(trimmed, "levels    ") || startsWith(trimmed, "levels ")) {
            current_section = "levels";
            auto cols = splitByMultiSpaces(trimAscii(trimmed.substr(6)));
            if (!cols.empty()) {
                g.levels = splitOnDelimiter(cols[0], ",");
            }
        } else if (startsWith(trimmed, "used by   ") || startsWith(trimmed, "used by ")) {
            current_section = "used_by";
            std::string rest = trimAscii(trimmed.substr(7));
            g.used_by = splitOnDelimiter(rest, ",");
        } else if (startsWith(trimmed, "models    ") || startsWith(trimmed, "models ")) {
            current_section = "models";
            parseMemberLine(trimmed.substr(6));
        } else if (startsWith(trimmed, "rules     ") || startsWith(trimmed, "rules ")) {
            current_section = "rules";
            ++g.rules_count;
        } else if (current_section == "models" && isDigit(trimmed[0])) {
            parseMemberLine(trimmed);
        } else if (current_section == "rules" && isDigit(trimmed[0])) {
            ++g.rules_count;
        } else {
            current_section.clear();
        }
    }

    if (!found_header) {
        return std::nullopt;
    }
    return g;
}

std::vector<QuotaEntry> parseQuotasJson(const std::string& raw_output,
                                        std::vector<std::string>* warnings) {
    std::vector<QuotaEntry> entries;
    std::string err;
    std::vector<std::string> logs;
    nlohmann::json root = extractJsonAfterLogs(raw_output, &err, &logs);
    if (warnings) {
        for (const auto& l : logs) {
            if (l.find("[error]") != std::string::npos) {
                warnings->push_back(l);
            }
        }
    }
    if (root.is_null()) {
        if (warnings && !err.empty()) {
            warnings->push_back(err);
        }
        return entries;
    }

    const nlohmann::json* arr = nullptr;
    if (root.is_array()) {
        arr = &root;
    } else if (root.is_object() && root.contains("data") && root["data"].is_array()) {
        arr = &root["data"];
    }
    if (!arr) {
        return entries;
    }

    for (const auto& item : *arr) {
        if (!item.is_object()) {
            continue;
        }
        QuotaEntry q;
        q.provider = item.value("provider", "");
        q.name = item.value("name", "");
        q.kind = item.value("kind", "");
        q.plan = item.value("plan", "");
        q.user = item.value("user", "");
        q.alias = item.value("alias", "");
        q.error = item.value("error", "");
        q.last = item.value("last", false);
        q.last_served_at = item.value("lastServedAt", "");
        q.read_at = item.value("readAt", "");

        if (item.contains("balance") && item["balance"].is_number()) {
            q.has_balance = true;
            q.balance = item["balance"].get<double>();
        }
        if (item.contains("balanceDisplay") && item["balanceDisplay"].is_string()) {
            q.balance_display = item["balanceDisplay"].get<std::string>();
        }

        if (item.contains("windows") && item["windows"].is_array()) {
            for (const auto& wj : item["windows"]) {
                if (!wj.is_object()) {
                    continue;
                }
                QuotaWindow w;
                w.name = wj.value("name", "");
                w.used = wj.value("used", 0.0);
                w.remaining = wj.value("remaining", 100.0);
                w.resets_at = wj.value("resetsAt", "");
                w.display = wj.value("display", "");
                w.pool = wj.value("pool", "");
                if (wj.contains("amount") && wj["amount"].is_number() &&
                    wj.contains("limit") && wj["limit"].is_number()) {
                    w.has_amount = true;
                    w.amount = wj["amount"].get<double>();
                    w.limit = wj["limit"].get<double>();
                }
                q.windows.push_back(std::move(w));
            }
        }
        entries.push_back(std::move(q));
    }
    return entries;
}

bool isValidGroupId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    unsigned char c0 = static_cast<unsigned char>(id[0]);
    if (!std::isalnum(c0)) {
        return false;
    }
    for (unsigned char c : id) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    return id.find("..") == std::string::npos;
}

bool isValidEffort(const std::string& effort) {
    return effort.empty() || effort == "agent" || effort == "auto" || isValidEffortToken(effort);
}

bool isValidRoutingStrategy(const std::string& routing) {
    return routing == "smart" || routing == "order" || routing == "drain" ||
           routing == "rotate" || routing == "usage" || routing == "pace" ||
           routing == "weight" || routing == "manual" || routing == "fast" ||
           routing == "cheap" || routing == "expiring";
}

bool isValidStaysPolicy(const std::string& stays) {
    return stays == "auto" || stays == "session" || stays == "turn" ||
           stays == "off" || stays == "strict";
}

std::string buildMemberSpec(const std::string& model_or_spec,
                            const std::string& effort,
                            bool fast,
                            const std::string& account,
                            std::string* error_out) {
    auto fail = [&](const std::string& msg) -> std::string {
        if (error_out) {
            *error_out = msg;
        }
        return {};
    };

    std::string raw = trimAscii(model_or_spec);
    if (raw.empty()) {
        return fail("model specification must not be empty");
    }
    if (hasControlOrSpace(raw) || raw.find(',') != std::string::npos || raw[0] == '-') {
        return fail("invalid characters in model specification: " + raw);
    }
    if (raw.find('=') != std::string::npos) {
        return fail("model specification must not contain '='");
    }

    std::string effective_account = trimAscii(account);
    if (!effective_account.empty() &&
        (hasControlOrSpace(effective_account) || effective_account.find(',') != std::string::npos ||
         effective_account.find('#') != std::string::npos)) {
        return fail("invalid account selector");
    }

    std::string base = raw;
    std::size_t hash_pos = base.find('#');
    if (hash_pos != std::string::npos) {
        std::string embedded_acct = base.substr(hash_pos + 1);
        base = base.substr(0, hash_pos);
        if (embedded_acct.empty()) {
            return fail("empty #account suffix in model specification");
        }
        if (effective_account.empty()) {
            effective_account = embedded_acct;
        }
    }

    const bool is_group_ref = startsWith(base, "group/") || startsWith(base, "auto/");
    if (is_group_ref) {
        if (!effective_account.empty() || fast || (!effort.empty() && effort != "auto" && effort != "agent")) {
            return fail("nested group/ or auto/ references do not accept :effort, :fast, or #account suffixes");
        }
        return base;
    }

    bool embedded_fast = false;
    if (base.size() > 5 && base.compare(base.size() - 5, 5, ":fast") == 0) {
        embedded_fast = true;
        base = base.substr(0, base.size() - 5);
    }

    std::string embedded_effort;
    std::size_t last_colon = base.rfind(':');
    if (last_colon != std::string::npos) {
        std::string suffix = base.substr(last_colon + 1);
        // Only treat suffix as Magpie reasoning override when it matches a valid effort level,
        // preserving vendor tags such as `:free`, `:7b`, `:0`, or `low+high`.
        if (isValidEffortToken(suffix)) {
            embedded_effort = suffix;
            base = base.substr(0, last_colon);
        }
    }

    std::string effective_effort = trimAscii(effort);
    if (effective_effort == "auto" || effective_effort == "agent") {
        effective_effort.clear();
    } else if (effective_effort.empty()) {
        effective_effort = embedded_effort;
    } else if (!isValidEffortToken(effective_effort)) {
        return fail("unsupported reasoning effort: " + effective_effort);
    }

    if (base.empty() || base.back() == '/') {
        return fail("invalid provider/model identifier: " + raw);
    }

    std::string out = base;
    if (!effective_effort.empty()) {
        out += ":" + effective_effort;
    }
    if (fast || embedded_fast) {
        out += ":fast";
    }
    if (!effective_account.empty()) {
        out += "#" + effective_account;
    }
    return out;
}

bool wouldCreateGroupCycle(const std::string& target_group_id,
                           const std::vector<std::string>& members,
                           const std::vector<GroupInfo>& existing_groups,
                           std::string* cycle_reason) {
    const std::string target_ref = "group/" + target_group_id;
    const std::string auto_ref = "auto/" + target_group_id;

    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (const auto& g : existing_groups) {
        if (g.id == target_group_id) {
            continue;
        }
        std::vector<std::string> child_refs;
        for (const auto& m : g.members) {
            std::string clean = stripSpecDecoratorsForCycle(m);
            if (startsWith(clean, "group/") || startsWith(clean, "auto/")) {
                child_refs.push_back(clean);
            }
        }
        adj[g.ref] = std::move(child_refs);
    }

    struct Node {
        std::string ref;
        int depth;
        std::string path;
    };

    std::queue<Node> q;
    std::unordered_set<std::string> visited;

    for (const auto& m : members) {
        std::string clean = stripSpecDecoratorsForCycle(m);
        if (!startsWith(clean, "group/") && !startsWith(clean, "auto/")) {
            continue;
        }
        if (clean == target_ref || clean == auto_ref) {
            if (cycle_reason) {
                *cycle_reason = target_ref + " -> " + clean;
            }
            return true;
        }
        if (visited.insert(clean).second) {
            q.push({clean, 1, target_ref + " -> " + clean});
        }
    }

    while (!q.empty()) {
        Node cur = std::move(q.front());
        q.pop();

        if (cur.depth > 8) {
            if (cycle_reason) {
                *cycle_reason = "group nesting exceeds Magpie depth limit (8): " + cur.path;
            }
            return true;
        }

        auto it = adj.find(cur.ref);
        if (it == adj.end()) {
            continue;
        }
        for (const auto& next_ref : it->second) {
            if (next_ref == target_ref || next_ref == auto_ref) {
                if (cycle_reason) {
                    *cycle_reason = cur.path + " -> " + next_ref;
                }
                return true;
            }
            if (visited.insert(next_ref).second) {
                q.push({next_ref, cur.depth + 1, cur.path + " -> " + next_ref});
            }
        }
    }
    return false;
}

std::string redactSecrets(const std::string& text, const std::vector<std::string>& secrets) {
    std::string out = text;
    for (const auto& sec : secrets) {
        if (sec.empty()) {
            continue;
        }
        std::size_t pos = 0;
        while ((pos = out.find(sec, pos)) != std::string::npos) {
            out.replace(pos, sec.size(), "[REDACTED]");
            pos += 10;
        }
    }
    return out;
}

std::string defaultGatewayJsonPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/magpie/gateway.json";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/magpie/gateway.json";
    }
    return {};
}

} // namespace magpie_agent
