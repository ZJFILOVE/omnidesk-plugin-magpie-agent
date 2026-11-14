#pragma once

#include "magpie_agent/magpie_client.hpp"

#include <memory>

namespace brls {
class View;
}

namespace magpie_agent {

// Compact widget mounted inside OmniDesk's F8 AI Companion Dock across domains.
brls::View* createMagpieDockWidget(const std::shared_ptr<MagpieClient>& client);

// Full configuration, quota, harness (read-only), and routing control view inside Plugin Store settings.
brls::View* createMagpieSettingsView(const std::shared_ptr<MagpieClient>& client);

} // namespace magpie_agent
