// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/fs.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

namespace se::fs {
namespace {

std::span<const std::byte> AsBytes(std::string_view text) {
  return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

void RemoveTree(const std::string& path) {
  auto info = Stat(path);
  REQUIRE(info);
  if (info->type == PathType::kDirectory) {
    auto entries = ListDirectory(path);
    REQUIRE(entries);
    for (const std::string& entry : *entries) RemoveTree(Join(path, entry));
  }
  REQUIRE(Remove(path));
}

// A fresh directory under the test output directory.
std::string FreshDir(std::string_view name) {
  const std::string dir = Join(SE_TEST_OUT_DIR, name);
  if (Exists(dir)) RemoveTree(dir);
  REQUIRE(CreateDirectories(dir));
  return dir;
}

TEST_CASE("Normalize folds separators, dots and parents", "[fs]") {
  REQUIRE(Normalize("a\\b//c/./d") == "a/b/c/d");
  REQUIRE(Normalize("a/b/../c") == "a/c");
  REQUIRE(Normalize("../a/../../b") == "../../b");
  REQUIRE(Normalize("/a/../../b/") == "/b");
  REQUIRE(Normalize("C:\\x\\..\\y\\") == "C:/y");
  REQUIRE(Normalize("C:") == "C:");
  REQUIRE(Normalize("/") == "/");
  REQUIRE(Normalize("a/..") == ".");
  REQUIRE(Normalize("//server/share/x") == "//server/share/x");
  REQUIRE(Normalize("").empty());
}

TEST_CASE("Join, IsAbsolute and Parent", "[fs]") {
  REQUIRE(Join("a/b", "c/d") == "a/b/c/d");
  REQUIRE(Join("a/b/", "../c") == "a/c");
  REQUIRE(Join("a", "/abs") == "/abs");
  REQUIRE(Join("a", "D:/abs") == "D:/abs");
  REQUIRE(Join("", "x") == "x");
  REQUIRE(Join("x", "") == "x");
  CHECK(IsAbsolute("/x"));
  CHECK(IsAbsolute("\\x"));
  CHECK(IsAbsolute("C:/x"));
  CHECK_FALSE(IsAbsolute("C:x"));
  CHECK_FALSE(IsAbsolute("x/y"));
  REQUIRE(Parent("a/b/c") == "a/b");
  REQUIRE(Parent("/x") == "/");
  REQUIRE(Parent("C:/x") == "C:/");
  REQUIRE(Parent("c").empty());
}

TEST_CASE("Files are written, read, renamed, listed and removed", "[fs]") {
  const std::string dir = FreshDir("fs_basic");
  const std::string a = Join(dir, "a.txt");
  const std::string b = Join(dir, "b.txt");
  REQUIRE(WriteFile(a, AsBytes("hello")));
  auto info = Stat(a);
  REQUIRE(info);
  CHECK(info->type == PathType::kFile);
  REQUIRE(info->size == 5);
  auto text = ReadTextFile(a);
  REQUIRE(text);
  REQUIRE(*text == "hello");

  REQUIRE(Rename(a, b));
  CHECK_FALSE(Exists(a));
  CHECK(Exists(b));
  REQUIRE(CreateDirectories(Join(dir, "sub/deeper")));
  auto entries = ListDirectory(dir);
  REQUIRE(entries);
  REQUIRE(*entries == std::vector<std::string>{"b.txt", "sub"});

  REQUIRE(Remove(b));
  CHECK_FALSE(Exists(b));
  // Removing what is not there is not an error.
  CHECK(Remove(b));
  auto dir_info = Stat(Join(dir, "sub"));
  REQUIRE(dir_info);
  CHECK(dir_info->type == PathType::kDirectory);
}

TEST_CASE("Errors name the path", "[fs]") {
  const std::string missing = Join(SE_TEST_OUT_DIR, "no/such/file.txt");
  auto read = ReadFile(missing);
  REQUIRE_FALSE(read);
  CHECK(read.error().path == missing);
  CHECK(read.error().code == ErrorCode::kNotFound);
  auto listed = ListDirectory(Join(SE_TEST_OUT_DIR, "no/such/dir"));
  REQUIRE_FALSE(listed);
  CHECK_FALSE(listed.error().path.empty());
  auto info = Stat(missing);
  REQUIRE(info);
  CHECK(info->type == PathType::kNone);
}

TEST_CASE("WriteFileAtomic replaces the file and leaves no temporary", "[fs]") {
  const std::string dir = FreshDir("fs_atomic");
  const std::string path = Join(dir, "recovery.bin");
  REQUIRE(WriteFileAtomic(path, AsBytes("first")));
  REQUIRE(WriteFileAtomic(path, AsBytes("second, longer")));
  auto text = ReadTextFile(path);
  REQUIRE(text);
  REQUIRE(*text == "second, longer");
  REQUIRE(WriteFileAtomic(path, {}));
  auto empty = ReadFile(path);
  REQUIRE(empty);
  CHECK(empty->empty());
  auto entries = ListDirectory(dir);
  REQUIRE(entries);
  REQUIRE(*entries == std::vector<std::string>{"recovery.bin"});
}

TEST_CASE("WriteFileAtomic into a missing directory fails cleanly", "[fs]") {
  const std::string path = Join(SE_TEST_OUT_DIR, "fs_missing_dir/x/y.bin");
  auto written = WriteFileAtomic(path, AsBytes("data"));
  REQUIRE_FALSE(written);
  CHECK_FALSE(written.error().path.empty());
  CHECK_FALSE(Exists(Join(SE_TEST_OUT_DIR, "fs_missing_dir")));
}

TEST_CASE("The base path and a root above it are found", "[fs]") {
  auto base = BasePath();
  REQUIRE(base);
  CHECK_FALSE(base->empty());
  REQUIRE(base->back() == '/');
  CHECK(IsAbsolute(*base));
  const std::string dir = FreshDir("fs_root");
  REQUIRE(CreateDirectories(Join(dir, "x/y/z")));
  REQUIRE(WriteFile(Join(dir, "marker.txt"), AsBytes("m")));
  auto root = FindRootContaining(Join(dir, "x/y/z"), "marker.txt");
  REQUIRE(root);
  CHECK(*root == Normalize(dir));
  auto none = FindRootContaining(Join(dir, "x/y/z"), "no-such-marker", 2);
  REQUIRE_FALSE(none);
  CHECK(none.error().code == ErrorCode::kNotFound);
}

}  // namespace
}  // namespace se::fs
