/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory
 *
 * qnx_micro_compat.h — C++23 backport cage.
 *
 * Every kernel type used in qnx-micro landed in C++23 or earlier:
 *
 *   std::expected<T,E>   C++23   (the entire error model)
 *   std::span<T>         C++20   (zero-copy buffer views)
 *   std::bitset<N>       C++98   (scheduler ready_mask_)
 *   std::array<T,N>      C++11   (fixed-capacity everything)
 *   std::optional<T>     C++17   (nullable handles)
 *   std::string_view     C++17   (PPS paths/keys)
 *   [[nodiscard]]        C++17   (every query)
 *   designated init      C++20   ({.id = x, .owner = y})
 *   std::ranges::*       C++20   (find_if, count_if, any_of)
 *   std::erase_if        C++20   (swap-free removal)
 *   auto -> T returns    C++14   (trailing return types)
 *
 * The ONLY C++26 dependency is `import std` and `export module`.
 * Include this header at the top of each .cppm when building
 * under C++23 (no modules). The logic is identical. Zero changes
 * to any algorithm, data structure, or API surface.
 *
 * Usage — each module wraps like this:
 *
 *   #ifndef QNX_MICRO_NO_MODULES
 *     export module qnx.types;
 *     import std;
 *   #else
 *     #pragma once
 *     #include "qnx_micro_compat.h"
 *     namespace qnx::types {
 *   #endif
 *
 *   // ... all kernel code, unchanged ...
 *
 *   #ifdef QNX_MICRO_NO_MODULES
 *     }  // namespace qnx::types
 *   #endif
 *
 * We chose C++26 modules because they express the DAG:
 *   types -> scheduler -> ipc -> memory -> driver -> pps -> kernel
 * Headers compile. Modules make the dependency graph a first-class
 * artifact the compiler enforces.
 */

#ifndef QNX_MICRO_COMPAT_H
#define QNX_MICRO_COMPAT_H

#if __cplusplus >= 202600L && defined(__cpp_modules)
  /* C++26 with module support — .cppm files work as-is. */
  /* This header is not needed; including it is harmless. */
#else
  /* C++23 fallback: headers replace modules. */

  #include <cstdint>
  #include <cstddef>
  #include <cstring>
  #include <array>
  #include <span>
  #include <bitset>
  #include <expected>
  #include <optional>
  #include <string_view>
  #include <string>
  #include <vector>
  #include <algorithm>
  #include <ranges>
  #include <functional>
  #include <numeric>

  #define QNX_MICRO_NO_MODULES 1

  /* On C++26, `export` makes symbols visible across module boundaries.
   * On C++23 with headers, everything is already visible via #include.
   * This macro silences `export` so the same source compiles both ways. */
  #ifndef export
    #define export
  #endif

#endif

#endif /* QNX_MICRO_COMPAT_H */
