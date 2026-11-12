#pragma once
#include <stdint.h>

extern "C" bool PE_parse_boot_argn(const char* arg_string, void* arg_ptr, int max_arg_size);
