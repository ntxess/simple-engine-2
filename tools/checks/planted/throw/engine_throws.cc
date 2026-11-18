// Planted violation: engine code throwing. Must be detected.
#include <stdexcept>

void Fail() { throw std::runtime_error("engine code must not throw"); }
