//------------------------------------------------------------------
//
//	FILE	  : AbstractIO.h
//
//	PURPOSE	  : Defines the CAbstractIO class.
//
//	CREATED	  : 1st May 1996
//
//	COPYRIGHT : Microsoft 1996 All Rights Reserved
//
//------------------------------------------------------------------

#ifndef __ABSTRACTIO_H__
	#define __ABSTRACTIO_H__


	#include "stdlithdefs.h"
	#include "lithexception.h"
	#include "memory.h"


	// The exception that will be thrown on a read or write fail.
	typedef enum
	{

		MoWriteError=0,
		MoReadError=1,
		MoSeekError=2
	
	} LithIOExceptionType;


	#define LITHIO_EXCEPTION		10
	
	class CLithIOException : public CLithException
	{
		public:

			CLithIOException()
			{
				SetExceptionType( LITHIO_EXCEPTION );
			}
			
			CLithIOException( LithIOExceptionType code )
			{
				SetExceptionType( LITHIO_EXCEPTION );
				m_Code=code;
			}

			LithIOExceptionType		m_Code;

	};



	class CAbstractIO
	{
		public:

			// Member functions

									CAbstractIO();
									~CAbstractIO();

			virtual BOOL			Open( const char *pFilename, const char *pAccess )	{ return TRUE; }
			virtual void			Close()												{}


			void					SetUserData1( DWORD data )	{ m_UserData1 = data; }
			DWORD					GetUserData1()				{ return m_UserData1; }


			void					EnableExceptions( BOOL bEnable );
			BOOL					IsExceptionsEnabled();

			// Functions to toWrite data
			virtual BOOL			Write( void *pBlock, DWORD blockSize )=0;
			CAbstractIO&	operator << ( DWORD toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( SDWORD toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( WORD toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( SWORD toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( BYTE toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( SBYTE toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( float toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			CAbstractIO&	operator << ( double toWrite )	{ Write(&toWrite, sizeof(toWrite)); return *this; }
			// PORT(macOS/LP64): on Win32 SDWORD was `long` and `int` was `int` --
			// two DISTINCT types that both happened to be 32-bit, so these were
			// separate overloads with identical bodies. With SDWORD correctly
			// 32-bit here it IS `int`, so the pair collapses into a redeclaration.
			// Dropping the redundant one is behaviour-preserving: SDWORD's
			// overload handles `int` and writes the same 4 bytes.
#if !defined(__LP64__) && !defined(_LP64)
			CAbstractIO&	operator << ( int toWrite )		{ Write(&toWrite, sizeof(toWrite)); return *this; }
#endif

			// Functions to read data
			virtual BOOL			Read( void *pBlock, DWORD blockSize )=0;
			CAbstractIO&	operator >> ( DWORD &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( SDWORD &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( WORD &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( SWORD &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( BYTE &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( SBYTE &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( float &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			CAbstractIO&	operator >> ( double &toRead )	{ Read(&toRead, sizeof(toRead)); return *this; }
			// PORT(macOS/LP64): on Win32 SDWORD was `long` and `int` was `int` --
			// two DISTINCT types that both happened to be 32-bit, so these were
			// separate overloads with identical bodies. With SDWORD correctly
			// 32-bit here it IS `int`, so the pair collapses into a redeclaration.
			// Dropping the redundant one is behaviour-preserving: SDWORD's
			// overload handles `int` and writes the same 4 bytes.
#if !defined(__LP64__) && !defined(_LP64)
			CAbstractIO&	operator >> ( int &toRead )		{ Read(&toRead, sizeof(toRead)); return *this; }
#endif

			BOOL					WriteString( char *pStr );
			BOOL					ReadString( char *pStr, DWORD maxLen );

			BOOL					ReadTextString(char *pStr, DWORD maxLen);

			virtual DWORD			GetCurPos()=0;
			virtual DWORD			GetLen()=0;

			virtual BOOL			SeekTo( DWORD pos )=0;

		
		protected:

			// User data stuff.
			DWORD					m_UserData1;

			// Tells whether or not it should throw exceptions.
			BOOL					m_bExceptionsEnabled;

			// Throws an exception if they're enabled.
			void					MaybeThrowIOException( LithIOExceptionType code );

	};


#endif


