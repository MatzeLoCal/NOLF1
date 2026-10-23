/****************************************************************************
;
;	MODULE:		LITHTYPES (.H)
;
;	PURPOSE:	basic data types
;
;	HISTORY:	04/19/98 [blb] 
;
;	NOTICE:		Copyright (c) 1998, MONOLITH, Inc.
;
****************************************************************************/

#ifndef LITHTYPES_H
#define LITHTYPES_H

// get sometypes from windows if we are a windows app
#ifdef _WINDOWS
//#include "windows.h"
#endif

// 8 bit
#ifndef BYTE
typedef unsigned char 		BYTE;
#endif
#ifndef UINT8
typedef unsigned char 		UINT8;
#endif
#ifndef INT8
typedef signed char 		INT8;
#endif

// 16 bit
#ifndef WORD
typedef unsigned short int 	WORD;
#endif
#ifndef UINT16
typedef unsigned short int 	UINT16;
#endif

#ifndef INT16
typedef signed short int 	INT16;
#endif

// 32 bit
#ifndef DWORD
// PORT(macOS/LP64): `unsigned long` was 32-bit on Win32 -- and the authored
// intent here is explicitly 32 bits -- but it is 64-bit under LP64.
// NB the `#ifndef DWORD` guard around this NEVER fires: DWORD is a typedef,
// not a macro, so this redefines it even when a windows.h is in scope.
#if defined(__LP64__) || defined(_LP64)
typedef unsigned int	DWORD;
#else
typedef unsigned long int	DWORD;
#endif
#endif

#ifndef UINT32
typedef unsigned int 		UINT32;
#endif
#ifndef INT32
typedef int 				INT32;
#endif

// boolean
#ifndef BOOL
typedef int BOOL;
#endif
#ifndef TRUE
# define TRUE    1
#endif
#ifndef FALSE
# define FALSE   0
#endif

// NULL
#ifndef NULL
#define NULL    0
#endif

// ASSERT
#ifndef ASSERT
#ifdef _DEBUG
#include "assert.h"
#define	ASSERT(exp)		assert(exp)
#else
#define ASSERT(exp)		/* */
#endif
#endif

#endif



