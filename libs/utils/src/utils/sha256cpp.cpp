#include "sha256.h"
#include "sha256cpp.h"

namespace devils_engine {
namespace utils {

SHA256::SHA256() noexcept {
  sha256_init(&buff);
}
    
void SHA256::update(const void* data, const size_t size) {
  sha256_update(&buff, data, size);
}
    
std::string SHA256::hash() {
  char hash[64];
  sha256_finalize(&buff);
  sha256_read_hex(&buff, hash);
  sha256_init(&buff);
  return std::string(hash, 64);
}

digest SHA256::finalize() {
  digest bin;
  sha256_finalize(&buff);
  sha256_read(&buff, bin.data());
  sha256_init(&buff);
  return bin;
}
    
std::string SHA256::easy(const std::string_view &str) {
  char hash[64];
  sha256_easy_hash_hex(str.data(), str.length(), hash);
  return std::string(hash, 64);
}

std::string SHA256::easy(const void* data, const size_t size) {
  char hash[64];
  sha256_easy_hash_hex(data, size, hash);
  return std::string(hash, 64);
}

}
}