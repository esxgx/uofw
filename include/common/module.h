/* Copyright (C) 2011, 2012 The uOFW team
   See the file COPYING for copying permission.
*/

#ifndef COMMON_H
# error "Only include common.h!"
#endif

#define PSP_SDK_VERSION(ver) const int module_sdk_version = ver

#define PSP_MODULE_BOOTSTART(name) int module_start(int arglen, void *argp) __attribute__((alias(name))); \
int module_bootstart(int arglen, void *argp) __attribute__((alias(name)))
#define PSP_MODULE_REBOOT_BEFORE(name) int module_reboot_before(void) __attribute__((alias(name)))
#define PSP_MODULE_REBOOT_PHASE(name) int module_reboot_phase(void) __attribute__((alias(name)))
#define PSP_MODULE_STOP(name) int module_stop(void) __attribute__((alias(name)))

