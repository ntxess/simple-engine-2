// Copyright 2026 The simple-engine-2 Authors.
// Planted violations: non-Google naming, an implicit constructor and `long`.
// Must be detected.
namespace se {

class bad_widget {
 public:
  bad_widget(int value) : value(value) {}
  int value;
};

long make_widget_count() { return bad_widget(3).value; }

}  // namespace se
