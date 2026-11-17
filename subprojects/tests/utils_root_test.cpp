#include <filesystem>
#include <string>

#include <devils_engine/utils/root.h>
#include <doctest/doctest.h>

using devils_engine::utils::path_refusal;
using devils_engine::utils::root;

namespace {
root temp_root() {
  auto opened = root::open(std::filesystem::temp_directory_path().string());
  REQUIRE(opened.has_value());
  return *opened;
}
} // namespace

TEST_CASE("root opens only an absolute existing directory [utils][root]") {
  const auto base = std::filesystem::temp_directory_path();
  const auto opened = root::open(base.string());
  REQUIRE(opened.has_value());
  CHECK(opened->path().back() == '/');
  CHECK(opened->path().find('\\') == std::string::npos);

  CHECK(root::open("relative/folder").error() == path_refusal::root_not_absolute);
  CHECK(root::open("").error() == path_refusal::root_not_absolute);
  CHECK(root::open((base / "devils_root_test_missing_folder").string()).error() == path_refusal::root_not_directory);
}

TEST_CASE("root resolves relative paths inside itself [utils][root]") {
  const auto r = temp_root();
  CHECK(r.resolve("settings.tavl").value() == r.path() + "settings.tavl");
  CHECK(r.resolve("cache/painter/spirv").value() == r.path() + "cache/painter/spirv");
  CHECK(r.resolve("cache\\painter\\spirv").value() == r.path() + "cache/painter/spirv");
  CHECK(r.resolve("./a//b/./c").value() == r.path() + "a/b/c");
  CHECK(r.resolve("a/b/../c").value() == r.path() + "a/c");
  CHECK(r.resolve("resources/").value() == r.path() + "resources/");
  CHECK(r.resolve("").value() == r.path());
  CHECK(r.resolve(".").value() == r.path());
  CHECK(r.resolve("a/..").value() == r.path());
}

TEST_CASE("root refuses absolute paths of every platform on every platform [utils][root]") {
  const auto r = temp_root();
  for (const char* path : {"/etc/passwd", "\\Windows\\system32", "//server/share", "\\\\server\\share",
                           "C:\\Users\\x", "C:/Users/x", "c:x", "Z:"}) {
    CAPTURE(path);
    CHECK(r.resolve(path).error() == path_refusal::absolute);
  }
}

TEST_CASE("root refuses leaving itself and non-portable names [utils][root]") {
  const auto r = temp_root();
  CHECK(r.resolve("..").error() == path_refusal::escapes_root);
  CHECK(r.resolve("../x").error() == path_refusal::escapes_root);
  CHECK(r.resolve("a/../../x").error() == path_refusal::escapes_root);
  CHECK(r.resolve("..\\x").error() == path_refusal::escapes_root);
  CHECK(r.resolve("file.txt:stream").error() == path_refusal::invalid);
  CHECK(r.resolve(std::string("a\0b", 3)).error() == path_refusal::invalid);
}
