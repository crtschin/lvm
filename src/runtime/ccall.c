/*-----------------------------------------------------------------------
  The Lazy Virtual Machine.

  Daan Leijen.

  Copyright 2001, Daan Leijen. All rights reserved. This file is
  distributed under the terms of the GNU Library General Public License.
-----------------------------------------------------------------------*/

/* $Id$ */

#include <string.h>
#include "mlvalues.h"
#include "fail.h"
#include "alloc.h"

#include "module.h"
#include "thread.h"
#include "ccall.h"

#define MAX_ARG 10

/*----------------------------------------------------------------------
  Define generic function types
----------------------------------------------------------------------*/
typedef long (CCALL   *fun_c0)(void);
typedef long (CCALL   *fun_c1)(long);
typedef long (CCALL   *fun_c2)(long,long);
typedef long (CCALL   *fun_c3)(long,long,long);
typedef long (CCALL   *fun_c4)(long,long,long,long);

#ifdef STDCALL
typedef long (STDCALL *fun_std0)(void);
typedef long (STDCALL *fun_std1)(long);
typedef long (STDCALL *fun_std2)(long,long);
typedef long (STDCALL *fun_std3)(long,long,long);
typedef long (STDCALL *fun_std4)(long,long,long,long);
#endif

/*----------------------------------------------------------------------
  Generic call_extern function
----------------------------------------------------------------------*/
static long long_val( char type, value v, const char* name );
static value val_long( char type, long l, const char* name );

value call_extern( value* sp, long arg_count, void* fun
                 , enum call_conv cconv
                 , value vtype, value vname )
{
  CAMLparam2(vtype,vname);
  const char* type;
  const char* name;
  long result = 0;
  long args[MAX_ARG];
  long i;

  Assert( Is_heap_val(vtype) && Tag_val(vtype) == String_tag );
  Assert( Is_heap_val(vname) && Tag_val(vname) == String_tag );
  type = String_val(vtype);
  name = String_val(vname);

  /* checks */
  if( (arg_count != (long)strlen(type)-1) )
    raise_internal("extern call \"%s\": type doesn't match number of arguments (\"%s\")", name, type );

  if (arg_count > MAX_ARG)
    raise_internal("extern call \"%s\": sorry, too many arguments (%i)", name, arg_count );

  /* check the return type */
  switch (type[0]) {
  case 'a':
  case 'b':
  case 'c':
  case 'i':
  case 'I':
  case 'l':
  case 'u':
  case 'U':
  case 'p':
  case 'z':
  case 'v':
  case '1':
  case '2':
  case '4':
#if (SIZEOF_LONG == 8)
  case '8':
#endif
            break;
  default:  raise_internal( "extern call \"%s\": invalid return type (%c)", name, type[0] );
  }


  if (arg_count == 1 && type[1] == 'v') {
    /* void function */
    if (!(Is_atom(sp[0]) && Tag_val(sp[0]) == 0)) {
      raise_internal( "extern call \"%s\": expecting () argument for void function", name );
    }
    arg_count = 0;
  }
  else {
    /* first do a collection if needed */
    for (i = 0; i < arg_count; i++) {
      if (type[i+1] == 'z' && Is_young(sp[i])) {
        empty_minor_heap();
        type = String_val(vtype); /* reload the strings, might be moved! */
        name = String_val(vname);
        break;
      }
    }
    /* convert arguments */
    for (i = 0; i < arg_count; i++) {
      args[i] = long_val(type[i+1],sp[i],name);
    }
  }


  /* do a generic C call */
  if (cconv == Call_c) {
    switch (arg_count) {
    case 0:   result = ((fun_c0)fun)(); break;
    case 1:   result = ((fun_c1)fun)(args[0]); break;
    case 2:   result = ((fun_c2)fun)(args[0],args[1]); break;
    case 3:   result = ((fun_c3)fun)(args[0],args[1],args[2]); break;
    case 4:   result = ((fun_c4)fun)(args[0],args[1],args[2], args[3]); break;
    default:  raise_internal( "extern call \"%s\": sorry, too many arguments (%i)", name, arg_count );
    }
  }
#ifdef STDCALL
  else if (cconv == Call_std) {
    switch (arg_count) {
    case 0:   result = ((fun_std0)fun)(); break;
    case 1:   result = ((fun_std1)fun)(args[0]); break;
    case 2:   result = ((fun_std2)fun)(args[0],args[1]); break;
    case 3:   result = ((fun_std3)fun)(args[0],args[1],args[2]); break;
    case 4:   result = ((fun_std4)fun)(args[0],args[1],args[2], args[3]); break;
    default:  raise_internal( "extern call \"%s\": sorry, too many arguments (%i)", name, arg_count );
    }
  }
#endif
  else {
    raise_internal( "extern call: unknown calling convention (%i)", cconv );
  }

  /* marshall the result */
  CAMLreturn(val_long( type[0], result, name ));
}

/*----------------------------------------------------------------------
  value conversion
----------------------------------------------------------------------*/
static long long_val( char type, value v, const char* name )
{
  /* first check the type */
  switch (type) {
  case 'a': return v;
  case 'z': if (!(Is_heap_val(v) && Tag_val(v) == String_tag )) {
              raise_internal( "extern call \"%s\": expecting string argument", name );
            }
            Assert (!Is_young(v));
            return (long)(String_val(v)); /* TODO: no compaction allowed for the duration of the call */

  case 'p': return v;
  case 'b': return Bool_val(v);
  case 'c':
  case 'i':
  case 'I':
  case 'l':
  case 'u':
  case 'U':
  case '1':
  case '2':
  case '4': break;

#if (SIZEOF_LONG == 8)
  case '8': break;
#endif



  default:  raise_internal( "extern call \"%s\": invalid argument type (%c)", name, type );
  }

  /* convert a value to a long */
  if (Is_long(v)) {
    return Long_val(v);
  } else if (Is_heap_val(v) && Tag_val(v) <= Con_max_tag) {
    con_tag_t tag;
    Con_tag_val(tag,v);
    return tag;
  } else {
    /* or should we just return it 'as is' ? */
    raise_internal( "extern call \"%s\": invalid argument value", name );
    return 0;
  }
}

static value val_long( char type, long l, const char* name )
{
  switch (type) {
  case 'a': return l;
  case 'b': return Val_bool(l);
  case 'c':
  case 'i':
  case 'I':
  case 'l': return Val_long(l);
  case 'u':
  case 'U': return Val_long(l);
  case 'p': return l;
  case 'z': return copy_string((char*)l); /* TODO: instead of copy, use a foreign pointer??, no, we can use "p" */
  case 'v': return Atom(0);
  case '1':
  case '2':
  case '4':
#if (SIZEOF_LONG == 8)
  case '8':
#endif
            return Val_long(l);
  default:  raise_internal( "extern call \"%s\": invalid return type (%c)", name, type );
            return Atom(0);
  }
}
