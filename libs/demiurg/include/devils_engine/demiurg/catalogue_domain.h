#ifndef DEVILS_ENGINE_DEMIURG_CATALOGUE_DOMAIN_H
#define DEVILS_ENGINE_DEMIURG_CATALOGUE_DOMAIN_H

#include "devils_engine/catalogue/introspection.h"
#include "devils_engine/catalogue/logging.h"

namespace devils_engine {
namespace demiurg {

namespace catalogue_domains {
inline constexpr size_t demiurg = catalogue::log_domain::demiurg;
}

using catalogue_domain = catalogue::domain<catalogue_domains::demiurg>;

void install_catalogue_introspection() noexcept;

}
}

#endif
