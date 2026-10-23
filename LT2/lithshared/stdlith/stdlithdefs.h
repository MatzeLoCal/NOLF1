//------------------------------------------------------------------
//
//	FILE	  : StdLithDefs.h
//
//	PURPOSE	  : Defines types and stuff for StdLith files.
//
//	CREATED	  : September 7 1996
//
//	COPYRIGHT : Microsoft 1996 All Rights Reserved
//
//------------------------------------------------------------------


#ifndef __STDLITH_DEFS_H__
	#define __STDLITH_DEFS_H__


	// Includes....
	#include <stdio.h>
	#include <string.h>
	#include <assert.h>



	#ifndef BOOL
		#define BOOL	int
	#endif

	#ifndef DWORD
		// PORT(macOS/LP64): `long` was 32-bit on Win32 but is 64-bit here.
		// NB the `#ifndef` guard above never fires against a windows.h that
		// *typedefs* these names -- a typedef is not a macro -- so this
		// definition wins textually and must itself be the right width.
#if defined(__LP64__) || defined(_LP64)
		#define DWORD	unsigned int
#else
		#define DWORD	unsigned long
#endif
	#endif

	#ifndef SDWORD
		// PORT(macOS/LP64): `long` was 32-bit on Win32 but is 64-bit here.
		// NB the `#ifndef` guard above never fires against a windows.h that
		// *typedefs* these names -- a typedef is not a macro -- so this
		// definition wins textually and must itself be the right width.
#if defined(__LP64__) || defined(_LP64)
		#define SDWORD	int
#else
		#define SDWORD	long
#endif
	#endif

	#ifndef WORD
		#define WORD	unsigned short
	#endif

	#ifndef SWORD
		#define SWORD	short
	#endif

	#ifndef BYTE
		#define BYTE	unsigned char
	#endif

	#ifndef SBYTE
		#define SBYTE	char
	#endif

	#ifndef ASSERT
		#define ASSERT assert
	#endif

	#ifndef TRUE
		#define TRUE	1
	#endif

	#ifndef FALSE
		#define FALSE	0
	#endif

	#define	BAD_INDEX	((DWORD)-1)

/*	
	// Stdlith stuff uses these to allocate memory.
	void* dalloc(unsigned long size);
	void dfree(void *ptr);


	// You can use this macro if you don't want to define your own allocators.
	#define IMPLEMENT_DEFAULT_DALLOC() \
		void* dalloc(unsigned long size) {return malloc(size);}\
		void dfree(void *ptr) {if(ptr) free(ptr);}
*/

#endif  // __STDLITH_DEFS_H__



