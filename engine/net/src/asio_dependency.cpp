#include <asio/version.hpp>

// This translation unit is intentionally implementation-free. It keeps the
// standalone Asio package, exported CMake target, include path, and pinned API
// version under every strict compiler lane before the transport sources land.
static_assert(ASIO_VERSION == 103200, "standalone Asio 1.32.0 is required");
