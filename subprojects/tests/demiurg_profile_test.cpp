#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <devils_engine/demiurg/module_profile.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/modules_listing.h>
#include <devils_engine/demiurg/zip_module.h>
#include <devils_engine/utils/core.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

using namespace devils_engine;
namespace {
struct temporary_root {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "devils_engine_profile_test";
  temporary_root() {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~temporary_root() {
    std::filesystem::remove_all(path);
  }
};
void write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}
void make_zip(const std::filesystem::path& path) {
  void* writer = mz_zip_writer_create();
  REQUIRE(mz_zip_writer_open_file(writer, path.c_str(), 0, 0) == MZ_OK);
  for (const auto& [name, text] : std::vector<std::pair<std::string, std::string>>{{"data.txt", "hello"}, {"empty.txt", ""}}) {
    mz_zip_file info{};
    info.filename = name.c_str();
    info.compression_method = MZ_COMPRESS_METHOD_STORE;
    info.uncompressed_size = text.size();
    REQUIRE(mz_zip_writer_entry_open(writer, &info) == MZ_OK);
    if (!text.empty()) REQUIRE(mz_zip_writer_entry_write(writer, text.data(), int32_t(text.size())) == int32_t(text.size()));
    REQUIRE(mz_zip_writer_entry_close(writer) == MZ_OK);
  }
  REQUIRE(mz_zip_writer_close(writer) == MZ_OK);
  mz_zip_writer_delete(&writer);
}
} // namespace
TEST_CASE("TAVL module profiles are shared by listing and module system") {
  temporary_root root;
  std::filesystem::create_directories(root.path / "core");
  std::filesystem::create_directories(root.path / "other");
  demiurg::modules_listing listing(root.path.generic_string());
  REQUIRE_NOTHROW(listing.reload());
  REQUIRE(listing.entries().size() == 2);
  listing.save_list("selected", std::vector<std::string>{"other", "core/"});
  const auto profile = demiurg::read_module_profile(root.path / "selected.tavl");
  REQUIRE(profile.modules.size() == 2);
  CHECK(profile.modules[0].path == "other/");
  CHECK(profile.modules[1].path == "core/");
  const auto selected = listing.load_list("selected");
  REQUIRE(selected.entries.size() == 2);
  CHECK(selected.entries[0].ptr != nullptr);
  demiurg::module_system modules(root.path.generic_string());
  const auto loaded = modules.load_list("selected");
  REQUIRE(loaded.size() == 2);
  CHECK(loaded[0].path == profile.modules[0].path);
  modules.load_modules(loaded);
  REQUIRE(modules.loaded_modules().size() == 2);
  listing.reload();
  REQUIRE(listing.available_lists().size() == 1);
  CHECK_THROWS_AS(listing.save_list("bad", std::vector<std::string>{"missing"}), utils::critical_error);
  CHECK_THROWS_AS(listing.save_list("bad", std::vector<demiurg::modules_listing::view>{{"core", "", nullptr}}), utils::critical_error);
  write(root.path / "missing.tavl", "version=1\nmodules=[{path=missing}]");
  CHECK_THROWS_AS(listing.load_list("missing"), utils::critical_error);
}
TEST_CASE("profile codec validates version paths types duplicates and preserves escaped strings") {
  temporary_root root;
  const auto path = root.path / "profile.tavl";
  demiurg::module_profile input{1, {{"core/", "", "quote\" slash\\ newline\n"}}};
  demiurg::write_module_profile(path, input);
  CHECK(demiurg::read_module_profile(path).modules[0].file_date == input.modules[0].file_date);
  for (const auto& text : {"version=2\nmodules=[]", "modules=[]", "version=1", "version=1\nmodules=wrong", "version=1\nmodules=[{}]", "version=1\nmodules=[{path=\"../outside\"}]", "version=1\nmodules=[{path=core},{path=\"core/\"}]", "version=1\nmodules=[{path=core,hash=broken}]", "version=1\nversion=1\nmodules=[]"}) {
    write(path, text);
    CHECK_THROWS_AS(demiurg::read_module_profile(path), utils::critical_error);
  }
  write(root.path / "legacy.json", "[]");
  CHECK_THROWS_AS(demiurg::read_module_profile(root.path / "legacy.tavl"), utils::critical_error);
  CHECK_THROWS_AS(demiurg::module_profile_path(root.path, "../outside"), utils::critical_error);
  CHECK_THROWS_AS(demiurg::module_profile_path(root.path, "legacy.json"), utils::critical_error);
  CHECK_THROWS_AS(demiurg::write_module_profile(root.path / "bad.json", input), utils::critical_error);
  demiurg::write_module_profile(path, {1, {}});
  CHECK(demiurg::read_module_profile(path).modules.empty());
}
TEST_CASE("ZIP entry reads accept byte counts including empty and reject CRC errors") {
  temporary_root root;
  const auto path = root.path / "data.zip";
  make_zip(path);
  {
    demiurg::zip_module zip(path.generic_string(), "test");
    zip.open();
    for (int i = 0; i < 3; ++i) {
      std::string text;
      std::vector<char> chars;
      std::vector<uint8_t> bytes;
      zip.load_text("data.txt", text);
      zip.load_binary("data.txt", chars);
      zip.load_binary("data.txt", bytes);
      CHECK(text == "hello");
      CHECK(std::string(chars.begin(), chars.end()) == "hello");
      CHECK(std::string(bytes.begin(), bytes.end()) == "hello");
      zip.load_text("empty.txt", text);
      CHECK(text.empty());
      zip.load_binary("empty.txt", chars);
      CHECK(chars.empty());
      zip.load_binary("empty.txt", bytes);
      CHECK(bytes.empty());
      CHECK_THROWS_AS(zip.load_text("missing.txt", text), utils::critical_error);
    }
  }
  std::ifstream stream(path, std::ios::binary);
  std::string damaged((std::istreambuf_iterator<char>(stream)), {});
  stream.close();
  const auto position = damaged.find("hello");
  REQUIRE(position != std::string::npos);
  damaged[position] = 'x';
  write(path, damaged);
  demiurg::zip_module zip(path.generic_string(), "test");
  zip.open();
  std::string text;
  CHECK_THROWS_AS(zip.load_text("data.txt", text), utils::critical_error);
  CHECK_NOTHROW(zip.load_text("empty.txt", text));
}
