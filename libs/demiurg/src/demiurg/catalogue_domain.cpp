#include "catalogue_domain.h"

#include "devils_engine/catalogue/logging.h"

namespace devils_engine {
namespace demiurg {

void install_catalogue_introspection() noexcept {
  static const catalogue::introspection cfg{
    catalogue::introspection_mode::off,
    catalogue::log_domain::demiurg,
    nullptr
  };
  catalogue_domain::set_introspection(&cfg);
}

}
}
