// Planted violation: a catch in a file not marked as a fault boundary.
void Call(void (*f)()) {
  try {
    f();
  } catch (...) {
  }
}
