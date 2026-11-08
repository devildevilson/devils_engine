#pragma once

#include <string>
#include <string_view>

namespace devils_engine::utils {
// пригодятся юникод строки
std::string cast(const std::wstring& str) noexcept;
std::string cast(const std::u16string_view& str) noexcept;
std::string cast(const std::u32string_view& str) noexcept;
std::wstring cast(const std::string& str) noexcept;
std::u16string cast16(const std::string_view& str) noexcept;
std::u32string cast32(const std::string_view& str) noexcept;

} // namespace devils_engine::utils
