// iostream.h -- pre-standard C++ header the original LT2 sources include.
// In the pre-standard world these names lived at global scope, and the original
// code (e.g. ButeMgr.h: `istream* m_pData;`) spells them unqualified. Lift the
// std ones to global scope so those sources compile as written.
#ifndef __LTCOMPAT_IOSTREAM_H__
#define __LTCOMPAT_IOSTREAM_H__
#include <iostream>
using std::istream;  using std::ostream;  using std::iostream;
using std::cin;      using std::cout;     using std::cerr;
using std::endl;     using std::ends;     using std::flush;
using std::ios;
#endif // __LTCOMPAT_IOSTREAM_H__
