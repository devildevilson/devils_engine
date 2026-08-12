#include "atomic_file.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace devils_engine {
namespace file_io {
namespace {

constexpr std::intptr_t invalid_handle = -1;

std::error_code last_system_error() noexcept {
#if defined(_WIN32)
  return std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
  return std::error_code(errno, std::generic_category());
#endif
}

std::string temporary_path_for(const std::string_view target) {
  std::string temporary(target);
  temporary += ".tmp";
  return temporary;
}

std::expected<void, atomic_file_error> validate_target(
  const std::string& target,
  const std::string& temporary) noexcept {
  try {
    if (target.empty()) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::validate_target,
        std::make_error_code(std::errc::invalid_argument),
        target,
        temporary,
        false});
    }

    const fs::path target_path(target);
    const fs::path parent = target_path.has_parent_path() ? target_path.parent_path() : fs::path(".");
    std::error_code ec;
    const auto parent_status = fs::status(parent, ec);
    if (ec || !fs::is_directory(parent_status)) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::validate_target,
        ec ? ec : std::make_error_code(std::errc::not_a_directory),
        target,
        temporary,
        false});
    }

    const auto target_status = fs::symlink_status(target_path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::validate_target,
        ec,
        target,
        temporary,
        false});
    }
    if (!ec && fs::is_directory(target_status)) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::validate_target,
        std::make_error_code(std::errc::is_a_directory),
        target,
        temporary,
        false});
    }
    return {};
  } catch (const fs::filesystem_error& e) {
    return std::unexpected(atomic_file_error{
      atomic_file_stage::validate_target,
      e.code(),
      target,
      temporary,
      false});
  } catch (...) {
    return std::unexpected(atomic_file_error{
      atomic_file_stage::validate_target,
      std::make_error_code(std::errc::io_error),
      target,
      temporary,
      false});
  }
}

std::error_code remove_temporary(const std::string& temporary) noexcept {
  std::error_code ec;
  fs::remove(temporary, ec);
  return ec;
}

#if !defined(_WIN32)
std::error_code sync_parent_directory(const std::string& target) noexcept {
  const fs::path target_path(target);
  const fs::path parent = target_path.has_parent_path() ? target_path.parent_path() : fs::path(".");
  const int directory = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory < 0) {
    return last_system_error();
  }
  if (::fsync(directory) != 0) {
    const auto ec = last_system_error();
    ::close(directory);
    return ec;
  }
  if (::close(directory) != 0) {
    return last_system_error();
  }
  return {};
}
#endif

} // namespace

std::string_view atomic_file_stage_name(const atomic_file_stage stage) noexcept {
  switch (stage) {
    case atomic_file_stage::validate_target: return "validate_target";
    case atomic_file_stage::create_temporary: return "create_temporary";
    case atomic_file_stage::write: return "write";
    case atomic_file_stage::flush_file: return "flush_file";
    case atomic_file_stage::close_file: return "close_file";
    case atomic_file_stage::replace: return "replace";
    case atomic_file_stage::flush_directory: return "flush_directory";
    case atomic_file_stage::recover_temporary: return "recover_temporary";
    case atomic_file_stage::invalid_state: return "invalid_state";
  }
  return "unknown";
}

atomic_file_transaction::atomic_file_transaction(
  std::string target,
  std::string temporary,
  const std::intptr_t handle) noexcept
  : target_(std::move(target)),
    temporary_(std::move(temporary)),
    handle_(handle),
    active_(true) {}

atomic_file_transaction::~atomic_file_transaction() noexcept {
  abort();
}

atomic_file_transaction::atomic_file_transaction(atomic_file_transaction&& other) noexcept {
  *this = std::move(other);
}

atomic_file_transaction& atomic_file_transaction::operator=(atomic_file_transaction&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  abort();
  target_ = std::move(other.target_);
  temporary_ = std::move(other.temporary_);
  handle_ = std::exchange(other.handle_, invalid_handle);
  active_ = std::exchange(other.active_, false);
  committed_ = std::exchange(other.committed_, false);
  return *this;
}

std::expected<atomic_file_transaction, atomic_file_error> atomic_file_transaction::begin(std::string target) noexcept {
  try {
    auto temporary = temporary_path_for(target);
    if (auto valid = validate_target(target, temporary); !valid) {
      return std::unexpected(std::move(valid.error()));
    }

#if defined(_WIN32)
    const auto handle = CreateFileW(
      fs::path(temporary).c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::create_temporary,
        last_system_error(),
        std::move(target),
        std::move(temporary),
        false});
    }
    return atomic_file_transaction(
      std::move(target),
      std::move(temporary),
      reinterpret_cast<std::intptr_t>(handle));
#else
    const int handle = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (handle < 0) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::create_temporary,
        last_system_error(),
        std::move(target),
        std::move(temporary),
        false});
    }

    struct stat target_status {};
    if (::stat(target.c_str(), &target_status) == 0) {
      (void)::fchmod(handle, target_status.st_mode & 07777);
    }
    return atomic_file_transaction(std::move(target), std::move(temporary), handle);
#endif
  } catch (...) {
    return std::unexpected(atomic_file_error{
      atomic_file_stage::create_temporary,
      std::make_error_code(std::errc::not_enough_memory),
      std::move(target),
      {},
      false});
  }
}

atomic_file_error atomic_file_transaction::error(
  const atomic_file_stage stage,
  std::error_code code,
  const bool replacement_committed) const noexcept {
  try {
    return atomic_file_error{stage, std::move(code), target_, temporary_, replacement_committed};
  } catch (...) {
    return atomic_file_error{stage, std::make_error_code(std::errc::not_enough_memory), {}, {}, replacement_committed};
  }
}

std::expected<void, atomic_file_error> atomic_file_transaction::write(
  const std::span<const std::byte> bytes) noexcept {
  if (!active_ || handle_ == invalid_handle) {
    return std::unexpected(error(
      atomic_file_stage::invalid_state,
      std::make_error_code(std::errc::operation_not_permitted),
      committed_));
  }

  size_t written = 0;
  while (written < bytes.size()) {
#if defined(_WIN32)
    const size_t remaining = bytes.size() - written;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD current = 0;
    const auto handle = reinterpret_cast<HANDLE>(handle_);
    if (!WriteFile(handle, bytes.data() + written, request, &current, nullptr)) {
      return std::unexpected(error(atomic_file_stage::write, last_system_error()));
    }
    if (current == 0) {
      return std::unexpected(error(atomic_file_stage::write, std::make_error_code(std::errc::io_error)));
    }
    written += current;
#else
    const size_t remaining = bytes.size() - written;
    const size_t request = std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t current = ::write(
      static_cast<int>(handle_),
      bytes.data() + written,
      request);
    if (current < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(error(atomic_file_stage::write, last_system_error()));
    }
    if (current == 0) {
      return std::unexpected(error(atomic_file_stage::write, std::make_error_code(std::errc::io_error)));
    }
    written += static_cast<size_t>(current);
#endif
  }
  return {};
}

std::expected<void, atomic_file_error> atomic_file_transaction::write(const std::span<const uint8_t> bytes) noexcept {
  return write(std::as_bytes(bytes));
}

std::expected<void, atomic_file_error> atomic_file_transaction::write(const std::string_view bytes) noexcept {
  return write(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::expected<void, atomic_file_error> atomic_file_transaction::commit() noexcept {
  if (!active_ || handle_ == invalid_handle) {
    return std::unexpected(error(
      atomic_file_stage::invalid_state,
      std::make_error_code(std::errc::operation_not_permitted),
      committed_));
  }

#if defined(_WIN32)
  const auto handle = reinterpret_cast<HANDLE>(handle_);
  if (!FlushFileBuffers(handle)) {
    return std::unexpected(error(atomic_file_stage::flush_file, last_system_error()));
  }
  if (!CloseHandle(handle)) {
    handle_ = invalid_handle;
    return std::unexpected(error(atomic_file_stage::close_file, last_system_error()));
  }
  handle_ = invalid_handle;
  if (!MoveFileExW(
        fs::path(temporary_).c_str(),
        fs::path(target_).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected(error(atomic_file_stage::replace, last_system_error()));
  }
#else
  if (::fsync(static_cast<int>(handle_)) != 0) {
    return std::unexpected(error(atomic_file_stage::flush_file, last_system_error()));
  }
  const int handle = static_cast<int>(std::exchange(handle_, invalid_handle));
  if (::close(handle) != 0) {
    return std::unexpected(error(atomic_file_stage::close_file, last_system_error()));
  }
  if (::rename(temporary_.c_str(), target_.c_str()) != 0) {
    return std::unexpected(error(atomic_file_stage::replace, last_system_error()));
  }
#endif

  active_ = false;
  committed_ = true;

#if !defined(_WIN32)
  if (auto ec = sync_parent_directory(target_); ec) {
    return std::unexpected(error(atomic_file_stage::flush_directory, ec, true));
  }
#endif
  return {};
}

void atomic_file_transaction::close_handle() noexcept {
  if (handle_ == invalid_handle) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = invalid_handle;
}

void atomic_file_transaction::abort() noexcept {
  if (!active_) {
    return;
  }
  close_handle();
  (void)remove_temporary(temporary_);
  active_ = false;
}

bool atomic_file_transaction::active() const noexcept {
  return active_;
}

bool atomic_file_transaction::committed() const noexcept {
  return committed_;
}

std::string_view atomic_file_transaction::target_path() const noexcept {
  return target_;
}

std::string_view atomic_file_transaction::temporary_path() const noexcept {
  return temporary_;
}

std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  const std::span<const std::byte> bytes) noexcept {
  auto transaction = atomic_file_transaction::begin(std::move(target));
  if (!transaction) {
    return std::unexpected(std::move(transaction.error()));
  }
  if (auto result = transaction->write(bytes); !result) {
    return result;
  }
  return transaction->commit();
}

std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  const std::span<const uint8_t> bytes) noexcept {
  return atomic_replace(std::move(target), std::as_bytes(bytes));
}

std::expected<void, atomic_file_error> atomic_replace(
  std::string target,
  const std::string_view bytes) noexcept {
  return atomic_replace(std::move(target), std::as_bytes(std::span(bytes.data(), bytes.size())));
}

std::expected<bool, atomic_file_error> recover_atomic_file(std::string target) noexcept {
  try {
    const auto temporary = temporary_path_for(target);
    if (auto valid = validate_target(target, temporary); !valid) {
      return std::unexpected(std::move(valid.error()));
    }
    std::error_code ec;
    const auto temporary_status = fs::symlink_status(temporary, ec);
    if (ec) {
      if (ec == std::errc::no_such_file_or_directory) {
        return false;
      }
      return std::unexpected(atomic_file_error{
        atomic_file_stage::recover_temporary,
        ec,
        std::move(target),
        temporary,
        false});
    }
    if (!fs::exists(temporary_status)) {
      return false;
    }
    if (fs::is_directory(temporary_status)) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::recover_temporary,
        std::make_error_code(std::errc::is_a_directory),
        std::move(target),
        temporary,
        false});
    }
    if (!fs::remove(temporary, ec) || ec) {
      return std::unexpected(atomic_file_error{
        atomic_file_stage::recover_temporary,
        ec ? ec : std::make_error_code(std::errc::operation_not_permitted),
        std::move(target),
        temporary,
        false});
    }
    return true;
  } catch (const fs::filesystem_error& e) {
    return std::unexpected(atomic_file_error{
      atomic_file_stage::recover_temporary,
      e.code(),
      std::move(target),
      {},
      false});
  } catch (...) {
    return std::unexpected(atomic_file_error{
      atomic_file_stage::recover_temporary,
      std::make_error_code(std::errc::not_enough_memory),
      std::move(target),
      {},
      false});
  }
}

} // namespace file_io
} // namespace devils_engine
