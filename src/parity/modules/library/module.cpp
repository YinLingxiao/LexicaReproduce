#include "parity/modules/modules.h"
namespace lexica::parity {
void registerLibrary(Router &) {
  // D registers the library-owned commands here; unbound commands fail with
  // NOT_IMPLEMENTED instead of reporting successful writes or fake statistics.
}
} // namespace lexica::parity
