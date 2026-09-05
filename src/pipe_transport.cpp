#include "hybrid_python/transport.hpp"

namespace hybrid_python::detail {
// The concrete POSIX descriptor transport lives with Runtime because it owns
// the child lifetime. This translation unit reserves the transport boundary for
// a later shared-memory implementation without coupling it to Python objects.
} // namespace hybrid_python::detail
