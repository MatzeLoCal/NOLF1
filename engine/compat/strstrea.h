// strstrea.h -- pre-standard MSVC header for strstream.
// ButeMgr.h uses istrstream/ostrstream unqualified and assigns an istrstream*
// to an istream*, so these must be the genuine std types.
#ifndef __LTCOMPAT_STRSTREA_H__
#define __LTCOMPAT_STRSTREA_H__
#include <strstream>
#include "iostream.h"
using std::strstream; using std::istrstream; using std::ostrstream;
using std::strstreambuf;
#endif // __LTCOMPAT_STRSTREA_H__
