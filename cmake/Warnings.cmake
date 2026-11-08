# Interface targets every engine module links privately.
#
#   se_warnings     the warning set, optionally as errors.
#   se_determinism  floating-point flags for simulation code: no contraction
#                   into FMA and no fast-math (docs/design/mt-design.md
#                   change 20).

add_library(se_warnings INTERFACE)
add_library(se_determinism INTERFACE)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(se_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus /Zc:inline
    /w14265  # class has virtual functions, but destructor is not virtual
    /w14287  # unsigned/negative constant mismatch
    /we4289  # loop control variable used outside the for-loop scope
    /w14296  # expression is always true/false
    /w14555  # expression has no effect
    /w14640  # thread-unsafe static member initialisation
    /w14826  # sign-extending conversion
    /w14905  # wide string literal cast to LPSTR
    /w14906  # string literal cast to LPWSTR
    /w14928  # illegal copy-initialisation
  )
  if(SE_WARNINGS_AS_ERRORS)
    target_compile_options(se_warnings INTERFACE /WX)
  endif()
  target_compile_options(se_determinism INTERFACE /fp:precise)
elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")  # clang-cl
  target_compile_options(se_warnings INTERFACE
    /W4 /permissive- /utf-8 -Wshadow -Wthread-safety
    -Wno-c++98-compat -Wno-c++98-compat-pedantic)
  if(SE_THREAD_SAFETY_ERRORS)
    target_compile_options(se_warnings INTERFACE -Werror=thread-safety)
  endif()
  if(SE_WARNINGS_AS_ERRORS)
    target_compile_options(se_warnings INTERFACE /WX)
  endif()
  # clang's default floating-point model is already "precise"; adding
  # /fp:precise as well only makes clang warn that -ffp-contract=off
  # overrides it.
  target_compile_options(se_determinism INTERFACE /clang:-ffp-contract=off)
else()  # GCC and Clang
  target_compile_options(se_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(se_warnings INTERFACE -Wthread-safety)
    if(SE_THREAD_SAFETY_ERRORS)
      target_compile_options(se_warnings INTERFACE -Werror=thread-safety)
    endif()
  endif()
  if(SE_WARNINGS_AS_ERRORS)
    target_compile_options(se_warnings INTERFACE -Werror)
  endif()
  target_compile_options(se_determinism INTERFACE
    -ffp-contract=off -fno-fast-math)
endif()
