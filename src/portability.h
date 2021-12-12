#pragma once

#include <cstddef>

// detection for 64 bit
#if defined(__x86_64__) || defined(_M_X64)
#define FOLLY_X64 1
#else
#define FOLLY_X64 0
#endif

#if defined(__arm__)
#define FOLLY_ARM 1
#else
#define FOLLY_ARM 0
#endif

#if defined(__aarch64__)
#define FOLLY_AARCH64 1
#else
#define FOLLY_AARCH64 0
#endif

#if defined(__powerpc64__)
#define FOLLY_PPC64 1
#else
#define FOLLY_PPC64 0
#endif

#if defined(__s390x__)
#define FOLLY_S390X 1
#else
#define FOLLY_S390X 0
#endif

namespace myfolly {
constexpr bool kIsArchArm = (FOLLY_ARM == 1);
constexpr bool kIsArchAmd64 = (FOLLY_X64 == 1);
constexpr bool kIsArchAArch64 = (FOLLY_AARCH64 == 1);
constexpr bool kIsArchPPC64 = (FOLLY_PPC64 == 1);
constexpr bool kIsArchS390X = (FOLLY_S390X == 1);
} // namespace myfolly
