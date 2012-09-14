/* Copyright (C) 2011, 2012 The uOFW team
   See the file COPYING for copying permission.
*/

#include <stdarg.h>
#include "common_header.h"
#include "loadcore.h"

typedef struct {
    s32 unk0; //0
    s32 numExportLibs; //4 -- number of sysmem's export libraries - set in SysMemInit (from utopia)
    SceResidentLibraryEntryTable *exportLib[8]; //8 --array of sysmem's export tables set in SysMemInit (from utopia)
    u32 loadCoreAddr; // 40 -- allocated in SysMemInit (from utopia)
    s32 userLibStart; // 44 -- offset in export_lib at which user libraries begin - set in SysMemInit (from utopia)
    s32 unk48; //48
    s32 unk52; //52
    s32 unk56;//56
    s32 unk60; //60
    SceStubLibraryEntryTable *loadCoreImportTables; //64 -- loadcore stubs - set in kactivate before booting loadcore (from utopia)
    u32 loadCoreImportTablesSize; //68 -- total size of stubs - set in kactivate before booting loadcore (from utopia)
    s32 init_thread_stack; //72 -- allocated in SysMemInit (from utopia)
    SceLoadCoreExecFileInfo *sysMemExecInfo; //76 -- set in kactivate before booting loadcore (from utopia)
    SceLoadCoreExecFileInfo *loadCoreExecInfo; //80 -- set in kactivate before booting loadcore (from utopia)
    s32 (*CompareSubType)(u32 tag); //84
    u32 (*CompareLatestSubType)(u32 tag); //88
    s32 (*SetMaskFunction)(u32 unk1, vs32 *addr); //92
    void (*kprintf_handler)(u8 *format, void *arg1, void *arg2, void *arg3, void *arg4, void *arg5, void *arg6, void *arg7); //96 -- set by sysmem (from utopia)
    s32 (*GetLengthFunction)(u8 *file, u32 size, u32 *newSize); //100 -- set in kactivate before booting loadcore (from utopia)
    s32 (*PrepareGetLengthFunction)(u8 *buf, u32 size); //104
    SceResidentLibraryEntryTable *exportEntryTables[]; //108 
} SysMemThreadConfig;

typedef struct {
    SceSize size;
    u32 startAddr;
    u32 memSize;
    u32 attr;
} SceSysmemPartitionInfo;

s32 sceKernelQueryMemoryPartitionInfo(u32 pid, SceSysmemPartitionInfo *info);

typedef struct {   
    int id;
    int (*func)(void *, int, int funcid, void *args);
} SceSysmemUIDLookupFunction;

typedef struct SceSysmemUIDControlBlock {   
    struct SceSysmemUIDControlBlock *parent; // 0
    struct SceSysmemUIDControlBlock *nextChild; // 4
    struct SceSysmemUIDControlBlock *type; // 8
    SceUID UID; // 12
    char *name; // 16
    unsigned char unk; // 20
    unsigned char size; // size in words
    short attribute; // 22
    struct SceSysmemUIDControlBlock *nextEntry; // 24
    struct SceSysmemUIDControlBlock *inherited; // 28
    SceSysmemUIDLookupFunction *func_table; // 32
} __attribute__((packed)) SceSysmemUIDControlBlock;

typedef struct {   
    int id;
    int (*func)();
} SceSysmemUIDLookupFunc;

SceSysmemUIDControlBlock *sceKernelCreateUIDType(const char *name, int attr, SceSysmemUIDLookupFunc *funcs, int unk, SceSysmemUIDControlBlock **type);
SceUID sceKernelCreateUID(SceSysmemUIDControlBlock *type, const char *name, short attr, SceSysmemUIDControlBlock **block);
int sceKernelDeleteUID(SceUID uid);
int sceKernelRenameUID(SceUID uid, const char *name);
int sceKernelGetUIDcontrolBlock(SceUID uid, SceSysmemUIDControlBlock **block);
s32 sceKernelGetUIDcontrolBlockWithType(SceUID uid, SceSysmemUIDControlBlock* type, SceSysmemUIDControlBlock** block);
void sceKernelCallUIDObjCommonFunction(SceSysmemUIDControlBlock *cb, SceSysmemUIDControlBlock *uidWithFunc, s32 funcId, va_list ap);

SceUID sceKernelCreateHeap(SceUID partitionid, SceSize size, int unk, const char *name);
void *sceKernelAllocHeapMemory(SceUID heapid, SceSize size);
int sceKernelFreeHeapMemory(SceUID heapid, void *block);
int sceKernelDeleteHeap(SceUID heapid);
SceSize sceKernelHeapTotalFreeSize(SceUID heapid);

void *sceKernelGetUsersystemLibWork(void);
void *sceKernelGetGameInfo(void);
void *sceKernelGetAWeDramSaveAddr(void);
int sceKernelGetMEeDramSaveAddr(void);
int sceKernelGetMEeDramSaveSize(void);

void *sceKernelMemcpy(void *dst, const void *src, u32 n);
void *sceKernelMemmove(void *dst, const void *src, u32 n);
void *sceKernelMemset32(void *buf, int c, int size);
void *sceKernelMemset(void *buf, int c, u32 size);

int sceKernelGetCompiledSdkVersion(void);
int sceKernelGetModel(void);

