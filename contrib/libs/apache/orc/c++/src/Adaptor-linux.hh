/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ADAPTER_HH
#define ADAPTER_HH

#define HAS_PREAD
#define HAS_STRPTIME
#define HAS_DIAGNOSTIC_PUSH
/* #undef HAS_DOUBLE_TO_STRING */
/* #undef HAS_INT64_TO_STRING */
#define HAS_PRE_1970
#define HAS_POST_2038
#define HAS_STD_ISNAN
#define HAS_BUILTIN_OVERFLOW_CHECK
/* #undef NEEDS_Z_PREFIX */

#include "orc/orc-config.hh"
#include <string>

#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define timegm(tm) _mkgmtime(tm)
#define gmtime_r(timep, result) (gmtime_s(result, timep) ? NULL : result)
#define asctime_r(tm, buf) (asctime_s(buf, 26, tm) ? NULL : buf)
#endif

#ifndef HAS_STRPTIME
  char* strptime(const char* buf, const char* format, struct tm* tm);
#endif

#ifndef HAS_PREAD
  ssize_t pread(int fd, void* buf, size_t count, off_t offset);
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define NO_SANITIZE_ATTR __attribute__((no_sanitize("signed-integer-overflow", "shift")))
#else
  #define NO_SANITIZE_ATTR
#endif

#ifdef HAS_DIAGNOSTIC_PUSH
  #ifdef __clang__
    #define DIAGNOSTIC_PUSH _Pragma("clang diagnostic push")
    #define DIAGNOSTIC_POP _Pragma("clang diagnostic pop")
  #elif defined(__GNUC__)
    #define DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
    #define DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")
  #elif defined(_MSC_VER)
    #define DIAGNOSTIC_PUSH __pragma(warning(push))
    #define DIAGNOSTIC_POP __pragma(warning(pop))
  #else
    #error("Unknown compiler")
  #endif
#else
  #define DIAGNOSTIC_PUSH
  #define DIAGNOSTIC_POP
#endif

#define PRAGMA(TXT) _Pragma(#TXT)

#if defined(_MSC_VER)
  // Handles both cl.exe and clang-cl.exe compilers
  #define DIAGNOSTIC_IGNORE(XXX) __pragma(warning(disable : XXX))
#elif defined(__clang__)
  #define DIAGNOSTIC_IGNORE(XXX) PRAGMA(clang diagnostic ignored XXX)
#elif defined(__GNUC__)
  #define DIAGNOSTIC_IGNORE(XXX) PRAGMA(GCC diagnostic ignored XXX)
#else
  #define DIAGNOSTIC_IGNORE(XXX)
#endif

#ifndef UINT32_MAX
  #define UINT32_MAX 0xffffffff
#endif

#ifndef INT64_MAX
  #define INT64_MAX 0x7fffffffffffffff
#endif

#ifndef INT64_MIN
  #define INT64_MIN (-0x7fffffffffffffff - 1)
#endif

#define GTEST_LANG_CXX11 0

#ifndef HAS_STD_ISNAN
  #include <math.h>
  #define std::isnan(XXX) isnan(XXX)
#else
  #include <cmath>
#endif

#include <mutex>

#ifdef NEEDS_Z_PREFIX
#define Z_PREFIX 1
#endif

namespace orc {
  std::string to_string(double val);
  std::string to_string(int64_t val);
  bool fileExists(const char* path);
}

#ifdef HAS_BUILTIN_OVERFLOW_CHECK
  #define multiplyExact !__builtin_mul_overflow
  #define addExact !__builtin_add_overflow
#else
namespace orc {
  /**
   * Compute value * repetitions, return false if overflow, return true otherwise
   * and save the result at the address pointed to by result
   * imitates the jdk Math.multiplyExact implementation
   * but this method makes the assumption that repetitions > 1
   */
  static bool multiplyExact(int64_t value, int64_t repetitions, int64_t* result) {
    int64_t r = value * repetitions;
    if (((value < 0 ? -value : value) | repetitions) >> 31 != 0 && r / repetitions != value) {
      return false;
    }
    *result = r;
    return true;
  }

  /**
   * imitates the jdk Math.addExact implementation
   */
  static bool addExact(int64_t sum, int64_t increment, int64_t* result) {
    int64_t r = sum + increment;
    if (((sum ^ r) & (increment ^ r)) < 0) {
      return false;
    }
    *result = r;
    return true;
  }
}
#endif

#endif /* ADAPTER_HH */
