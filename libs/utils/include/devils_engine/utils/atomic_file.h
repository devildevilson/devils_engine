#ifndef DEVILS_ENGINE_UTILS_ATOMIC_FILE_H
#define DEVILS_ENGINE_UTILS_ATOMIC_FILE_H

// Durable same-directory file replacement with an explicit commit boundary.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace devils_engine {
namespace file_io {

enum class atomic_file_stage : uint8_t {
  validate_target,
  create_temporary,
  write,
  flush_file,
  close_file,
  replace,
  flush_directory,
  recover_temporary,
  invalid_state
};

std::string_view atomic_file_stage_name(atomic_file_stage stage) noexcept;

struct atomic_file_error {
  atomic_file_stage stage = atomic_file_stage::validate_target;
  std::error_code code;
  std::string target;
  std::string temporary;
  // A directory-sync failure can be reported after the atomic rename already committed.
  bool replacement_committed = false;
};

class atomic_file_transaction {
public:
  atomic_file_transaction() noexcept = default;
  ~atomic_file_transaction() noexcept;

  atomic_file_transaction(const atomic_file_transaction&) = delete;
  atomic_file_transaction& operator=(const atomic_file_transaction&) = delete;
  atomic_file_transaction(atomic_file_transaction&& other) noexcept;
  atomic_file_transaction& operator=(atomic_file_transaction&& other) noexcept;

  static std::expected<atomic_file_transaction, atomic_file_error> begin(std::string target) noexcept;

  std::expected<void, atomic_file_error> write(std::span<const std::byte> bytes) noexcept;
  std::expected<void, atomic_file_error> write(std::span<const uint8_t> bytes) noexcept;
  std::expected<void, atomic_file_error> write(std::string_view bytes) noexcept;
  std::expected<void, atomic_file_error> commit() noexcept;
  void abort() noexcept;

  bool active() const noexcept;
  bool committed() const noexcept;
  std::string_view target_path() const noexcept;
  std::string_view temporary_path() const noexcept;

private:
  explicit atomic_file_transaction(std::string target, std::string temporary, std::intptr_t handle) noexcept;

  atomic_file_error error(atomic_file_stage stage, std::error_code code, bool replacement_committed = false) const noexcept;
  void close_handle() noexcept;

  std::string target_;
  std::string temporary_;
  std::intptr_t handle_ = -1;
  bool active_ = false;
  bool committed_ = false;
};

std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  std::span<const std::byte> bytes) noexcept;
std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  std::span<const uint8_t> bytes) noexcept;
std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  std::string_view bytes) noexcept;

// Call only while no writer for target is active. Returns true when a stale sidecar was removed.
std::expected<bool, atomic_file_error> recover_atomic_file(std::string target) noexcept;

} // namespace file_io
} // namespace devils_engine

#endif
