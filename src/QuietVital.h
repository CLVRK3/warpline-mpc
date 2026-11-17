// QuietVital.h - silence warnings that Vital's own sources raise under the SDK's -Wall -Wextra,
// so the build output stays readable. Include first in every .cpp of this plugin.
#pragma once
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunknown-warning-option"
#endif
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wdangling-reference"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-private-field"
  #pragma clang diagnostic ignored "-Wimplicit-const-int-float-conversion"
  #pragma clang diagnostic ignored "-Wvla-cxx-extension"
  #pragma clang diagnostic ignored "-Wtautological-overlap-compare"
  #pragma clang diagnostic ignored "-Wbitwise-instead-of-logical"
#endif
