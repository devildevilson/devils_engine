#ifndef DEVILS_ENGINE_UTILS_DETERMINISTIC_SORT_H
#define DEVILS_ENGINE_UTILS_DETERMINISTIC_SORT_H

// Adapted from Jolt Physics 5.6.0, Jolt/Core/InsertionSort.h and QuickSort.h.
// SPDX-FileCopyrightText: 2022 Jorrit Rouwe
// SPDX-License-Identifier: MIT
//
// The algorithm is owned here because canonical engine order must not depend on
// which standard-library implementation happens to provide std::sort.

#include <functional>
#include <iterator>
#include <utility>

namespace devils_engine::utils {

namespace deterministic_sort_detail {

template <typename Iterator, typename Compare>
inline void insertion_sort(Iterator begin, Iterator end, Compare compare) {
  if (begin == end) return;

  for (Iterator i = begin + 1; i != end; ++i) {
    auto value = std::move(*i);
    if (compare(value, *begin)) {
      Iterator previous;
      for (Iterator j = i; j != begin; j = previous) {
        previous = j - 1;
        *j = std::move(*previous);
      }
      *begin = std::move(value);
    } else {
      Iterator j = i;
      for (Iterator previous = j - 1; compare(value, *previous); j = previous, --previous)
        *j = std::move(*previous);
      *j = std::move(value);
    }
  }
}

template <typename Iterator, typename Compare>
inline void median_of_three(Iterator first, Iterator middle, Iterator last, Compare compare) {
  if (compare(*middle, *first)) std::swap(*first, *middle);
  if (compare(*last, *first)) std::swap(*first, *last);
  if (compare(*last, *middle)) std::swap(*middle, *last);
}

template <typename Iterator, typename Compare>
inline void ninther(Iterator first, Iterator middle, Iterator last, Compare compare) {
  const auto distance = (last - first) >> 3;
  const auto twice = distance << 1;
  Iterator middle_first = first + distance;
  median_of_three(first, middle_first, first + twice, compare);
  median_of_three(middle - distance, middle, middle + distance, compare);
  Iterator middle_last = last - distance;
  median_of_three(last - twice, middle_last, last, compare);
  median_of_three(middle_first, middle, middle_last, compare);
}

} // namespace deterministic_sort_detail

// Deterministic for the same input bytes and comparator. Like std::sort this is
// not stable: if equivalent elements need a canonical order, the comparator
// must include an explicit tie-breaker.
template <typename Iterator, typename Compare>
inline void deterministic_sort(Iterator begin, Iterator end, Compare compare) {
  for (;;) {
    const auto count = end - begin;
    if (count < 2) return;
    if (count <= 32) {
      deterministic_sort_detail::insertion_sort(begin, end, compare);
      return;
    }

    Iterator pivot_iterator = begin + ((count - 1) >> 1);
    deterministic_sort_detail::ninther(begin, pivot_iterator, end - 1, compare);
    auto pivot = *pivot_iterator;
    Iterator left = begin;
    Iterator right = end;
    for (;;) {
      while (compare(*left, pivot)) ++left;
      do {
        --right;
      } while (compare(pivot, *right));
      if (left >= right) break;
      std::swap(*left, *right);
      ++left;
    }
    ++right;

    if (right - begin < end - right) {
      deterministic_sort(begin, right, compare);
      begin = right;
    } else {
      deterministic_sort(right, end, compare);
      end = right;
    }
  }
}

template <typename Iterator>
inline void deterministic_sort(Iterator begin, Iterator end) {
  deterministic_sort(begin, end, std::less<>{});
}

} // namespace devils_engine::utils

#endif
