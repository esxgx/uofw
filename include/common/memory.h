/* Copyright (C) 2011, 2012 The uOFW team
   See the file COPYING for copying permission.
*/

#ifndef COMMON_INCLUDED
# error "Only include common_imp.h or common_header.h!"
#endif

#define UCACHED(ptr) (void*)((u32)(void*)(ptr) & 0x1FFFFFFF)
#define KUNCACHED(ptr) (void*)(0xA0000000 | ((u32)(void*)(ptr) & 0x1FFFFFFF))
#define UUNCACHED(ptr) (void*)(0x40000000 | ((u32)(void*)(ptr) & 0x1FFFFFFF))

#define UPALIGN256(v) (((v) + 0xFF) & 0xFFFFFF00)
#define UPALIGN64(v) (((v) + 0x3F) & 0xFFFFFFC0)
#define UPALIGN4(v)     (((v) + 0x3) & 0xFFFFFFFC)   