
/****************************************************************************
 ** input_map.h *************************************************************
 ****************************************************************************
 *
 * input_map.h - button namespace derived from Linux input layer
 *
 * Copyright (C) 2008 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

/**
 * @file input_map.h
 * @brief  Button namespace derived from Linux input layer.
 * @ingroup private_api
 */

#ifndef INPUT_MAP_H
#define INPUT_MAP_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#if defined __linux__
#include <linux/input.h>
#include <linux/uinput.h>
#endif

#if defined __linux__
typedef __u16 linux_input_code;
#else
typedef unsigned short linux_input_code;
#endif

int get_input_code(const char* name, linux_input_code* code);
void fprint_namespace(FILE* f);
int is_in_namespace(const char* name);

#ifdef	__cplusplus
}
#endif

#endif /* INPUT_MAP_H */
