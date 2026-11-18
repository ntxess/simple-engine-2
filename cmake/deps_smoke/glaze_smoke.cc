// Copyright 2026 The simple-engine-2 Authors.
// glaze reflection compile-cost probe: 20 aggregates written to JSON and
// BEVE and read back.

#include <cstdint>
#include <string>
#include <vector>

#include "glaze/glaze.hpp"

namespace probe {

struct Component0 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component1 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component2 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component3 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component4 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component5 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component6 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component7 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component8 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component9 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component10 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component11 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component12 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component13 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component14 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component15 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component16 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component17 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component18 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct Component19 {
  float x = 0;
  float y = 0;
  std::int32_t id = 0;
  std::string name;
  std::vector<float> values;
};

struct All {
  Component0 c0;
  Component1 c1;
  Component2 c2;
  Component3 c3;
  Component4 c4;
  Component5 c5;
  Component6 c6;
  Component7 c7;
  Component8 c8;
  Component9 c9;
  Component10 c10;
  Component11 c11;
  Component12 c12;
  Component13 c13;
  Component14 c14;
  Component15 c15;
  Component16 c16;
  Component17 c17;
  Component18 c18;
  Component19 c19;
};

}  // namespace probe

int main() {
  probe::All all{};
  std::string json;
  if (glz::write_json(all, json)) return 1;
  probe::All back{};
  if (glz::read_json(back, json)) return 2;
  std::vector<std::byte> beve;
  if (glz::write_beve(all, beve)) return 3;
  return 0;
}
