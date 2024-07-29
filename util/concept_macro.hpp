#ifndef CM_UTIL_CONCEPTS_MACRO_HPP
#define CM_UTIL_CONCEPTS_MACRO_HPP

// a macro header to reduce the burden of writing constraints
// also a common place to handle the differences between C++17 and C++20

#if (__cplusplus < 202002L)
  // C++17 with -fconcepts in GCC
  #include <type_traits>
  template <typename Derived, typename... Bases>
  constexpr bool C_DERIVE = (std::is_base_of_v<Bases, Derived> && ...) && 
                            (std::is_convertible_v<const volatile Derived*, const volatile Bases*> && ...);
#else
  // C++20
  #include <concepts>
  template <typename Derived, typename... Bases>
  constexpr bool C_DERIVE = (std::derived_from<Derived, Bases> && ...);
#endif

template<typename c>
constexpr bool C_VOID = std::is_void_v<c>;

template<typename lhs, typename rhs>
constexpr bool C_SAME = std::is_same_v<lhs, rhs>;

template <typename Derived, typename... Bases>
constexpr bool C_DERIVE_OR_VOID = C_DERIVE<Derived, Bases...> || C_VOID<Derived>;

#endif
