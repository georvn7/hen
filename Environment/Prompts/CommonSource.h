#ifdef COMPILE_TEST

#include <cstdio>
#define PRINT_TEST(format, ...) do { std::printf("function: %s - " format "\n", __func__, ##__VA_ARGS__);} while (0)

#else

#define PRINT_TEST(format, ...)

#endif
