#pragma once
#include <stdint.h>

/* Real signature from XNU's pexpert/pexpert/boot.h: returns int (0/1). */
extern "C" int PE_parse_boot_argn(const char* arg_string, void* arg_ptr, int max_arg_size);
