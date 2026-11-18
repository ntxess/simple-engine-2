// Planted violation: a mutable static variable in simulation code (state
// shared by every World and thread). Must be detected.
#include <string>

int NextId() {
  static int next = 0;
  return ++next;
}

const std::string& Label() {
  static std::string label = "shared";
  return label;
}
