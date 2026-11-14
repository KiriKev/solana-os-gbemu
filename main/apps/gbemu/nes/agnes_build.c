// Builds the vendored agnes.c unchanged. Two of its range checks on uint8_t
// values are always true/false, which -Wtype-limits (an error here) reports.
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "agnes.c"
