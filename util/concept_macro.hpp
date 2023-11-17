#ifndef CM_UTIL_CONCEPTS_MACRO_HPP
#define CM_UTIL_CONCEPTS_MACRO_HPP

// a macro header to reduce the burden of writing constraints
// also a common place to handle the differences between C++17 and C++20

#include <concepts>

#if (__cplusplus < 202002L)
  // C++17 with -fconcepts in GCC
  #define C_DERIVE(c, b) (std::is_base_of_v<b, c> && std::is_convertible_v<const volatile c*, const volatile b*>)
#else
  // C++20
  #define C_DERIVE(c, b) std::derived_from<c, b>
#endif

#define C_VOID(c)        std::is_void_v<c>
#define C_SAME(lhs, rhs) std::is_same_v<lhs, rhs>
#define C_DERIVE_OR_VOID(c, b) (C_DERIVE(c, b) || C_VOID(c))
#define C_DERIVE2(c, b1, b2) (C_DERIVE(c, b1) && C_DERIVE(c, b2))
#define C_DERIVE3(c, b1, b2, b3) (C_DERIVE2(c, b1, b2) && C_DERIVE(c, b3))

#endif
