/* Copyright (C) 2011, 2012 The uOFW team
   See the file COPYING for copying permission.
*/

#include "common.h"

typedef struct {   
    int size;
    int (*ops[])();
} SceKernelDeci2Ops;

int sceKernelDeci2pRegisterOperations(void *op);
void *sceKernelDeci2pReferOperations();
void Kprintf(const char *format, ...);
int sceKernelDipsw(int);

int sceKernelDebugWrite(SceUID fd, const void *data, SceSize size);
int sceKernelDebugRead(SceUID fd, const void *data, SceSize size);
int sceKernelDebugEcho(void);

