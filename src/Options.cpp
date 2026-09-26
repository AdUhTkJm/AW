#include "aw/Options.h"

namespace aw {

Options g_options;

Options &options() noexcept {
  return g_options;
}

}  // namespace aw
