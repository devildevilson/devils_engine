#ifndef DEVILS_ENGINE_UTILS_PRNG_STRING_H
#define DEVILS_ENGINE_UTILS_PRNG_STRING_H

#include <cstdint>
#include <cstddef>
#include <string>
#include "dice.h"

namespace devils_engine {
namespace utils {

constexpr const char* full_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890";
constexpr const char* low_alphabet = "abcdefghijklmnopqrstuvwxyz1234567890";
constexpr const char* upper_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
constexpr const char* base16_alphabet = "abcdef1234567890";

template <typename T>
void strgen(const char* alphabet, const size_t alphabet_length, T &rnd, std::string &container) {
  for (auto &c : container) {
    const size_t index = utils::interval(alphabet_length, rnd);
    c = alphabet[index];
  }
}

template <typename T>
std::string strgen(const size_t length, const char* alphabet, const size_t alphabet_length, T &rnd) {
  std::string str(length, '\0');
  strgen(alphabet, alphabet_length, rnd, str);
  return str;
}

template <typename T>
std::string random_str_seed(const size_t length, T &rnd) {
  return strgen(length, base16_alphabet, strlen(base16_alphabet), rnd);
}

template <typename T>
std::string random_string(const size_t length, T &rnd) {
  return strgen(length, full_alphabet, strlen(full_alphabet), rnd);
}

template <typename T>
std::string random_lowercase(const size_t length, T &rnd) {
  return strgen(length, low_alphabet, strlen(low_alphabet), rnd);
}

template <typename T>
std::string random_uppercase(const size_t length, T &rnd) {
  return strgen(length, upper_alphabet, strlen(upper_alphabet), rnd);
}

template <typename T>
void random_str_seed(T &rnd, std::string &str) {
  strgen(base16_alphabet, strlen(base16_alphabet), rnd, str);
}

template <typename T>
void random_string(T &rnd, std::string &str) {
  strgen(length, full_alphabet, strlen(full_alphabet), rnd, str);
}

template <typename T>
void random_lowercase(T &rnd, std::string &str) {
  strgen(length, low_alphabet, strlen(low_alphabet), rnd, str);
}

template <typename T>
void random_uppercase(T &rnd, std::string &str) {
  strgen(length, upper_alphabet, strlen(upper_alphabet), rnd, str);
}

}
}

#endif
