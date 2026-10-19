#ifndef MEASUREMENT_OUTPUT_H
#define MEASUREMENT_OUTPUT_H
#include <stdio.h>
int measurement_test_printf(const char *format, ...);
#define printf measurement_test_printf
#endif
