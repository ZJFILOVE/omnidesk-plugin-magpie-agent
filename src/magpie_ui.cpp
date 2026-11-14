#include "magpie_agent/magpie_ui.hpp"

#include <atomic>
#include <borealis.hpp>
#include <iomanip>
#include <memory>
#include <omnidesk/ui_theme.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace magpie_agent {

namespace {

struct UiAliveFlag {
    std::atomic<bool> alive{true};
};

class SafeUiBox final : public brls::Box {
public:
    explicit SafeUiBox(brls::Axis axis = brls::Axis::COLUMN)
        : brls::Box(axis), alive_(std::make_shared<UiAliveFlag>()) {}

    ~SafeUiBox() override {
        if (alive_) {
            alive_->alive.store(false);
        }
    }

    std::shared_ptr<UiAliveFlag> aliveFlag() const { return alive_; }

private:
    std::shared_ptr<UiAliveFlag> alive_;
};

std::string formatQuotaSummaryLine(const QuotaEntry& q) {
    std::ostringstream oss;
    oss << (!q.provider.empty() ? q.provider : q.name);
    if (!q.user.empty()) {
        oss << " (" << q.user << ")";
    }
    if (!q.error.empty()) {
        oss << " - auth/error: " << q.error.substr(0, 48);
        return oss.str();
    }
    if (q.has_balance) {
        oss << " - balance " << std::fixed << std::setprecision(2) << q.balance;
    }
    if (!q.windows.empty()) {
        oss << ": ";
        std::size_t max_w = std::min<std::size_t>(q.windows.size(), 2);
        for (std::size_t i = 0; i < max_w; ++i) {
            if (i > 0) {
                oss << " | ";
            }
            const auto& w = q.windows[i];
            oss << w.name << " " << std::fixed << std::setprecision(0) << w.remaining << "% left";
            if (!w.display.empty()) {
                oss << " (" << w.display << ")";
            }
        }
    }
    return oss.str();
}

std::string formatHarnessesLine(const std::vector<HarnessInfo>& harnesses) {
    if (harnesses.empty()) {
        return "终端 Agent(只读): 点击“刷新”探测已安装工具";
    }
    std::ostringstream oss;
    oss << "终端 Agent(只读): ";
    std::size_t shown = 0;
    for (const auto& h : harnesses) {
        if (shown > 0) {
            oss << " · ";
        }
        oss << h.name << (h.configured ? " [✓]" : " [—]");
        if (++shown >= 5) {
            if (harnesses.size() > shown) {
                oss << " …";
            }
            break;
        }
    }
    return oss.str();
}

void clearContainerChildren(brls::Box* box) {
    if (!box) {
        return;
    }
    while (!box->getChildren().empty()) {
        box->removeView(box->getChildren().front(), true);
    }
}

struct SettingsDraft {
    std::vector<std::string> model_choices;
    std::size_t model_idx = 0;
    std::size_t fallback_idx = 0; // 0 => none; 1..N => model_choices[idx - 1]
    std::vector<std::string> effort_choices{"auto", "low", "medium", "high", "xhigh", "max", "none"};
    std::size_t effort_idx = 0;
    bool fast = false;
    std::vector<std::string> routing_choices{"order", "smart", "usage", "pace", "drain", "rotate", "manual"};
    std::size_t routing_idx = 0;
    std::vector<std::string> stays_choices{"session", "turn", "auto", "off", "strict"};
    std::size_t stays_idx = 0;
};

template <typename Vec>
std::size_t findIndexOrZero(const Vec& vec, const std::string& val) {
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (vec[i] == val) {
            return i;
        }
    }
    return 0;
}

} // namespace

brls::View* createMagpieDockWidget(const std::shared_ptr<MagpieClient>& client) {
    auto* root = new SafeUiBox(brls::Axis::COLUMN);
    root->setPadding(12, 12, 12, 12);
    root->setCornerRadius(10);
    root->setBackgroundColor(omnidesk::ui::color("desktop/soft"));
    root->setBorderColor(omnidesk::ui::color("desktop/line"));
    root->setBorderThickness(1.0f);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginBottom(6);

    auto* title_lbl = omnidesk::ui::makeLabel("Magpie AI 路由与额度", 14.0f, "brls/text");
    auto* gw_pill = omnidesk::ui::makeLabel("Gateway", 11.5f, "font/grey");
    header->addView(title_lbl);
    header->addView(gw_pill);
    root->addView(header);

    auto* route_lbl = omnidesk::ui::makeLabel("", 12.5f, "brls/text");
    route_lbl->setMarginBottom(4);
    root->addView(route_lbl);

    auto* harness_lbl = omnidesk::ui::makeLabel("", 11.5f, "font/grey");
    harness_lbl->setMarginBottom(6);
    root->addView(harness_lbl);

    auto* quota_box = new brls::Box(brls::Axis::COLUMN);
    quota_box->setMarginBottom(8);
    root->addView(quota_box);

    auto* btn_row = new brls::Box(brls::Axis::ROW);
    btn_row->setAlignItems(brls::AlignItems::CENTER);

    auto renderSnapshot = [route_lbl, gw_pill, harness_lbl, quota_box](const RouterSnapshot& snap) {
        if (snap.gateway.running) {
            gw_pill->setText("在线 · " + (!snap.gateway.addr.empty() ? snap.gateway.addr : snap.gateway.url));
            gw_pill->setTextColor(omnidesk::ui::color("status/ok_fg"));
        } else {
            gw_pill->setText("Gateway 离线");
            gw_pill->setTextColor(omnidesk::ui::color("status/err_fg"));
        }

        route_lbl->setText("当前路由: " + snap.route.summary);
        harness_lbl->setText(formatHarnessesLine(snap.harnesses));

        clearContainerChildren(quota_box);
        if (snap.quotas.empty()) {
            auto* empty_q = omnidesk::ui::makeLabel("额度快照: 点击“刷新状态”获取实时额度", 11.5f, "font/grey");
            quota_box->addView(empty_q);
        } else {
            std::size_t shown = 0;
            for (const auto& q : snap.quotas) {
                if (q.windows.empty() && !q.has_balance && !q.error.empty()) {
                    continue; // Skip expired accounts when healthy ones are shown first
                }
                auto* row = omnidesk::ui::makeLabel(formatQuotaSummaryLine(q), 11.5f, "font/grey");
                row->setMarginBottom(2);
                quota_box->addView(row);
                if (++shown >= 3) {
                    break;
                }
            }
            if (shown == 0) {
                auto* row = omnidesk::ui::makeLabel(formatQuotaSummaryLine(snap.quotas.front()),
                                                    11.5f, "status/warn_fg");
                quota_box->addView(row);
            }
        }
    };

    if (client) {
        renderSnapshot(client->getCachedSnapshot());
    }

    auto alive = root->aliveFlag();
    std::weak_ptr<MagpieClient> weak_client = client;

    auto triggerRefresh = [alive, weak_client, renderSnapshot, route_lbl]() {
        auto locked = weak_client.lock();
        if (!locked || locked->isShuttingDown()) {
            return;
        }
        route_lbl->setText("正在同步 Magpie 状态与额度…");
        locked->spawnTrackedWorker([locked, alive, renderSnapshot]() {
            RouterSnapshot snap = locked->refreshAllSnapshot(true);
            brls::sync([locked, alive, renderSnapshot, snap]() {
                if (!alive->alive.load() || locked->isShuttingDown()) {
                    return;
                }
                renderSnapshot(snap);
            });
        });
    };

    auto* refresh_btn = omnidesk::ui::makeButton("刷新状态", triggerRefresh, false);
    refresh_btn->setMarginRight(8);
    btn_row->addView(refresh_btn);

    auto* preset_btn = omnidesk::ui::makeButton(
        "同步默认策略 (order)",
        [alive, weak_client, renderSnapshot, route_lbl]() {
            auto locked = weak_client.lock();
            if (!locked || locked->isShuttingDown()) {
                return;
            }
            RouterSnapshot cur = locked->getCachedSnapshot();
            if (cur.route.model.empty() && cur.models.empty()) {
                route_lbl->setText("请先点击“刷新状态”加载可用模型后再设置路由");
                return;
            }
            const std::string chosen =
                !cur.route.model.empty() ? cur.route.model : cur.models.front().id;
            route_lbl->setText("正在写入 omnidesk-active: " + chosen);
            locked->spawnTrackedWorker([locked, alive, renderSnapshot, chosen]() {
                locked->applyRoute({
                    {"model", chosen},
                    {"routing", "order"},
                    {"stays", "session"},
                });
                RouterSnapshot next = locked->refreshAllSnapshot(false);
                brls::sync([locked, alive, renderSnapshot, next]() {
                    if (!alive->alive.load() || locked->isShuttingDown()) {
                        return;
                    }
                    renderSnapshot(next);
                });
            });
        },
        true);
    btn_row->addView(preset_btn);
    root->addView(btn_row);

    return root;
}

brls::View* createMagpieSettingsView(const std::shared_ptr<MagpieClient>& client) {
    auto* root = new SafeUiBox(brls::Axis::COLUMN);
    root->setMarginTop(8);

    auto draft = std::make_shared<SettingsDraft>();
    auto alive = root->aliveFlag();
    std::weak_ptr<MagpieClient> weak_client = client;

    // 1. Gateway status card
    auto* gw_card = omnidesk::ui::makeCard("desktop/surface");
    gw_card->setMarginBottom(10);
    auto* gw_title = omnidesk::ui::makeLabel("Magpie Gateway 与当前状态", 15.0f, "brls/text");
    gw_title->setMarginBottom(4);
    auto* gw_info = omnidesk::ui::makeLabel("", 12.5f, "font/grey");
    gw_info->setMarginBottom(8);
    gw_card->addView(gw_title);
    gw_card->addView(gw_info);

    // 2. Route configurator card
    auto* route_card = omnidesk::ui::makeCard("desktop/surface");
    route_card->setMarginBottom(10);
    auto* route_title = omnidesk::ui::makeLabel("OmniDesk 专用路由组配置 (无终端污染)", 15.0f, "brls/text");
    route_title->setMarginBottom(4);
    auto* route_status = omnidesk::ui::makeLabel("", 12.5f, "font/grey");
    route_status->setMarginBottom(8);
    route_card->addView(route_title);
    route_card->addView(route_status);

    auto* primary_lbl = omnidesk::ui::makeLabel("", 13.0f, "brls/text");
    auto* fallback_lbl = omnidesk::ui::makeLabel("", 12.5f, "font/grey");
    auto* policy_lbl = omnidesk::ui::makeLabel("", 12.5f, "font/grey");
    primary_lbl->setMarginBottom(4);
    fallback_lbl->setMarginBottom(4);
    policy_lbl->setMarginBottom(8);
    route_card->addView(primary_lbl);
    route_card->addView(fallback_lbl);
    route_card->addView(policy_lbl);

    auto* controls_row1 = new brls::Box(brls::Axis::ROW);
    controls_row1->setMarginBottom(6);
    auto* controls_row2 = new brls::Box(brls::Axis::ROW);
    controls_row2->setMarginBottom(8);
    auto* feedback_lbl = omnidesk::ui::makeLabel("", 12.0f, "font/grey");

    // 3. Quotas & Harnesses card
    auto* quota_card = omnidesk::ui::makeCard("desktop/surface");
    quota_card->setMarginBottom(10);
    auto* quota_title = omnidesk::ui::makeLabel("实时订阅额度与终端 Agent 检测 (只读)", 15.0f, "brls/text");
    quota_title->setMarginBottom(6);
    quota_card->addView(quota_title);
    auto* quota_list = new brls::Box(brls::Axis::COLUMN);
    quota_card->addView(quota_list);

    auto syncDraftLabels = [draft, primary_lbl, fallback_lbl, policy_lbl]() {
        std::string m = draft->model_choices.empty()
                            ? "(未加载，请先点击刷新)"
                            : draft->model_choices[draft->model_idx % draft->model_choices.size()];
        std::string fb = "(无备选)";
        if (draft->fallback_idx > 0 && !draft->model_choices.empty()) {
            fb = draft->model_choices[(draft->fallback_idx - 1) % draft->model_choices.size()];
        }
        primary_lbl->setText("主模型 / 组: " + m);
        fallback_lbl->setText("备选 Fallback: " + fb);
        std::ostringstream oss;
        oss << "推理强度: " << draft->effort_choices[draft->effort_idx % draft->effort_choices.size()]
            << "  ·  Fast: " << (draft->fast ? "开" : "关")
            << "  ·  策略: " << draft->routing_choices[draft->routing_idx % draft->routing_choices.size()]
            << "  ·  粘滞: " << draft->stays_choices[draft->stays_idx % draft->stays_choices.size()];
        policy_lbl->setText(oss.str());
    };

    auto applySnapshotToView = [draft, gw_info, route_status, quota_list, syncDraftLabels](const RouterSnapshot& snap) {
        std::ostringstream gss;
        gss << (snap.gateway.running ? "状态: 运行中" : "状态: 未运行 / 不可达");
        if (!snap.gateway.url.empty()) {
            gss << "  ·  " << snap.gateway.url;
        }
        if (snap.gateway.pid > 0) {
            gss << "  ·  PID " << snap.gateway.pid;
        }
        if (!snap.gateway.error.empty()) {
            gss << " (" << snap.gateway.error << ")";
        }
        gw_info->setText(gss.str());
        route_status->setText("当前激活: " + snap.route.summary + "  (目标组: " + snap.route.groupRef() + ")");

        std::vector<std::string> candidates;
        for (const auto& m : snap.models) {
            if (m.id == snap.route.groupRef()) {
                continue; // Never allow selecting self in draft UI
            }
            candidates.push_back(m.id);
        }
        if (!candidates.empty()) {
            draft->model_choices = std::move(candidates);
            if (!snap.route.model.empty()) {
                draft->model_idx = findIndexOrZero(draft->model_choices, snap.route.model);
            } else {
                draft->model_idx %= draft->model_choices.size();
            }
            if (!snap.route.fallbacks.empty()) {
                std::size_t idx = findIndexOrZero(draft->model_choices, snap.route.fallbacks.front());
                draft->fallback_idx = (draft->model_choices[idx] == snap.route.fallbacks.front()) ? (idx + 1) : 0;
            }
        }
        draft->routing_idx = findIndexOrZero(draft->routing_choices, snap.route.routing);
        draft->stays_idx = findIndexOrZero(draft->stays_choices, snap.route.stays);
        if (!snap.route.effort.empty()) {
            draft->effort_idx = findIndexOrZero(draft->effort_choices, snap.route.effort);
        }
        draft->fast = snap.route.fast;
        syncDraftLabels();

        clearContainerChildren(quota_list);
        if (!snap.harnesses.empty()) {
            auto* h_lbl = omnidesk::ui::makeLabel(formatHarnessesLine(snap.harnesses), 12.0f, "brls/text");
            h_lbl->setMarginBottom(6);
            quota_list->addView(h_lbl);
        }
        if (snap.quotas.empty()) {
            quota_list->addView(omnidesk::ui::makeLabel("暂无额度数据，请点击“刷新全部数据”。", 12.0f, "font/grey"));
        } else {
            std::size_t max_rows = std::min<std::size_t>(snap.quotas.size(), 8);
            for (std::size_t i = 0; i < max_rows; ++i) {
                quota_list->addView(
                    omnidesk::ui::makeLabel(formatQuotaSummaryLine(snap.quotas[i]), 12.0f, "font/grey"));
            }
        }
    };

    if (client) {
        applySnapshotToView(client->getCachedSnapshot());
    } else {
        syncDraftLabels();
    }

    auto* btn_refresh = omnidesk::ui::makeButton(
        "刷新全部数据",
        [alive, weak_client, feedback_lbl, applySnapshotToView]() {
            auto locked = weak_client.lock();
            if (!locked || locked->isShuttingDown()) {
                return;
            }
            feedback_lbl->setText("正在后台查询 Magpie CLI 与 Gateway…");
            locked->spawnTrackedWorker([locked, alive, feedback_lbl, applySnapshotToView]() {
                RouterSnapshot next = locked->refreshAllSnapshot(true);
                brls::sync([locked, alive, feedback_lbl, applySnapshotToView, next]() {
                    if (!alive->alive.load() || locked->isShuttingDown()) {
                        return;
                    }
                    applySnapshotToView(next);
                    if (next.warnings.empty()) {
                        feedback_lbl->setText("同步完成: 发现 " + std::to_string(next.models.size()) +
                                              " 个模型, " + std::to_string(next.quotas.size()) + " 条额度记录");
                    } else {
                        feedback_lbl->setText("同步完成(含告警): " + next.warnings.front());
                    }
                });
            });
        },
        false);
    gw_card->addView(btn_refresh);

    auto* btn_model = omnidesk::ui::makeButton(
        "切换主模型",
        [draft, syncDraftLabels]() {
            if (!draft->model_choices.empty()) {
                draft->model_idx = (draft->model_idx + 1) % draft->model_choices.size();
            }
            syncDraftLabels();
        },
        false);
    btn_model->setMarginRight(6);

    auto* btn_fallback = omnidesk::ui::makeButton(
        "切换备选",
        [draft, syncDraftLabels]() {
            if (!draft->model_choices.empty()) {
                draft->fallback_idx = (draft->fallback_idx + 1) % (draft->model_choices.size() + 1);
            }
            syncDraftLabels();
        },
        false);
    btn_fallback->setMarginRight(6);

    auto* btn_effort = omnidesk::ui::makeButton(
        "切换推理强度",
        [draft, syncDraftLabels]() {
            draft->effort_idx = (draft->effort_idx + 1) % draft->effort_choices.size();
            syncDraftLabels();
        },
        false);
    controls_row1->addView(btn_model);
    controls_row1->addView(btn_fallback);
    controls_row1->addView(btn_effort);

    auto* btn_fast = omnidesk::ui::makeButton(
        "切换 Fast",
        [draft, syncDraftLabels]() {
            draft->fast = !draft->fast;
            syncDraftLabels();
        },
        false);
    btn_fast->setMarginRight(6);

    auto* btn_routing = omnidesk::ui::makeButton(
        "切换路由策略",
        [draft, syncDraftLabels]() {
            draft->routing_idx = (draft->routing_idx + 1) % draft->routing_choices.size();
            syncDraftLabels();
        },
        false);
    btn_routing->setMarginRight(6);

    auto* btn_stays = omnidesk::ui::makeButton(
        "切换粘滞",
        [draft, syncDraftLabels]() {
            draft->stays_idx = (draft->stays_idx + 1) % draft->stays_choices.size();
            syncDraftLabels();
        },
        false);
    controls_row2->addView(btn_fast);
    controls_row2->addView(btn_routing);
    controls_row2->addView(btn_stays);

    auto* btn_apply = omnidesk::ui::makeButton(
        "写入并激活 Magpie 路由组",
        [alive, weak_client, draft, feedback_lbl, applySnapshotToView]() {
            auto locked = weak_client.lock();
            if (!locked || locked->isShuttingDown()) {
                return;
            }
            if (draft->model_choices.empty()) {
                feedback_lbl->setText("错误: 未检测到可用模型，请先点击“刷新全部数据”。");
                return;
            }
            const std::string chosen = draft->model_choices[draft->model_idx % draft->model_choices.size()];
            nlohmann::json fbs = nlohmann::json::array();
            if (draft->fallback_idx > 0) {
                std::string fb = draft->model_choices[(draft->fallback_idx - 1) % draft->model_choices.size()];
                if (fb != chosen) {
                    fbs.push_back(fb);
                }
            }
            const std::string effort = draft->effort_choices[draft->effort_idx % draft->effort_choices.size()];
            const std::string routing = draft->routing_choices[draft->routing_idx % draft->routing_choices.size()];
            const std::string stays = draft->stays_choices[draft->stays_idx % draft->stays_choices.size()];

            nlohmann::json req = {
                {"action", "set_route"},
                {"model", chosen},
                {"fallbacks", fbs},
                {"effort", effort == "auto" ? "" : effort},
                {"fast", draft->fast},
                {"routing", routing},
                {"stays", stays},
            };

            feedback_lbl->setText("正在回写 Magpie 路由组…");
            locked->spawnTrackedWorker([locked, alive, req, feedback_lbl, applySnapshotToView]() {
                auto r = locked->applyRoute(req);
                RouterSnapshot next = locked->getCachedSnapshot();
                brls::sync([locked, alive, r, next, feedback_lbl, applySnapshotToView]() {
                    if (!alive->alive.load() || locked->isShuttingDown()) {
                        return;
                    }
                    applySnapshotToView(next);
                    if (r.ok) {
                        feedback_lbl->setText("已生效: " + r.data.value("summary", "updated"));
                    } else {
                        feedback_lbl->setText("写入失败: " + r.error);
                    }
                });
            });
        },
        true);
    btn_apply->setMarginBottom(6);

    route_card->addView(controls_row1);
    route_card->addView(controls_row2);
    route_card->addView(btn_apply);
    route_card->addView(feedback_lbl);

    root->addView(gw_card);
    root->addView(route_card);
    root->addView(quota_card);
    return root;
}

} // namespace magpie_agent
