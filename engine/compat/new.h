// new.h -- pre-standard MSVC header the original LT2 sources include.
// The standard spelling is <new>; this forwards so the originals compile
// unmodified.
#ifndef __LTCOMPAT_NEW_H__
#define __LTCOMPAT_NEW_H__
#ifdef __cplusplus
  #include <new>
  using std::set_new_handler;
  using std::bad_alloc;
  using std::nothrow;
  using std::nothrow_t;
  // MSVC's _set_new_handler / _PNH spelling.
  typedef int (*_PNH)(size_t);
#endif
#endif // __LTCOMPAT_NEW_H__
