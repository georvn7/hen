#ifdef COMPILE_TEST

#include <cstdio>
#define PRINT_TEST(format, ...) std::fprintf(stderr, format, ##__VA_ARGS__)

#else

#define PRINT_TEST(format, ...)

#endif
