// Planted violations: simulation code reading the wall clock and calling a
// CRT transcendental. Must be detected.
#include <chrono>
#include <cmath>

double Elapsed() {
  auto now = std::chrono::steady_clock::now();
  return std::sin(static_cast<double>(now.time_since_epoch().count()));
}
