/*-----------------------------------------------------------------------
  The Lazy Virtual Machine.

  Daan Leijen.

  Copyright 2001, Daan Leijen. All rights reserved. This file is
  distributed under the terms of the GNU Library General Public License.
-----------------------------------------------------------------------*/

/* $Id$ */

#include <stdlib.h>
#include "mlvalues.h"
#include "fail.h"
#include "primfloat.h"

#ifdef HAS_FLOAT_H
#ifdef __MINGW32__
#include <../mingw/float.h>
#else
#include <float.h>
#endif
#endif

#ifdef HAS_IEEEFP_H
#include <ieeefp.h>
#endif

/*----------------------------------------------------------------------
-- portable functions
----------------------------------------------------------------------*/
#ifdef ARCH_ALIGN_DOUBLE

double Double_val(value val)
{
  union { value v[2]; double d; } buffer;

  Assert(sizeof(double) == 2 * sizeof(value));
  buffer.v[0] = Field(val, 0);
  buffer.v[1] = Field(val, 1);
  return buffer.d;
}

void Store_double_val(value val, double dbl)
{
  union { value v[2]; double d; } buffer;

  Assert(sizeof(double) == 2 * sizeof(value));
  buffer.d = dbl;
  Field(val, 0) = buffer.v[0];
  Field(val, 1) = buffer.v[1];
}

#endif

value copy_double(double d)
{
  value res;

#define Setup_for_gc
#define Restore_after_gc
  Alloc_small(res, Double_wosize, Double_tag);
#undef Setup_for_gc
#undef Restore_after_gc
  Store_double_val(res, d);
  return res;
}

float_t float_of_string( const char* s )
{
  return atof(s);
}


/*----------------------------------------------------------------------
-- Each platform defines an array that maps [fp_exception] to bit masks
----------------------------------------------------------------------*/
#define Fp_exn_count  (Fpe_denormal + 1)

static long fp_sticky_masks[Fp_exn_count];
static long fp_trap_masks[Fp_exn_count];
static long fp_round_masks[fp_round_count];

long  fp_sticky_mask( enum exn_arithmetic ex )
{
  if (ex < 0 || ex >= Fp_exn_count) {
    raise_invalid_argument( "fp_sticky_mask" ); return 0;
  }else 
    return fp_sticky_masks[ex];
}

long  fp_trap_mask( enum exn_arithmetic ex )
{
  if (ex < 0 || ex >= Fp_exn_count) {
    raise_invalid_argument( "fp_trap_mask" ); return 0;
  }else 
    return fp_trap_masks[ex];
}

long fp_round_mask( enum fp_round rnd )
{
  if (rnd < 0 || rnd >= fp_round_count) {
    raise_invalid_argument( "fp_round_mask" ); return 0;
  }else
    return fp_round_masks[rnd];
}

enum fp_round fp_round_unmask( long rnd )
{
  enum fp_round i;
  for( i = 0; i < fp_round_count; i++) {
    if (fp_round_masks[i] == rnd) return i;
  }
  return fp_round_near;
}


                              
/*----------------------------------------------------------------------
  The ieee conformance on i386 platforms is bizar:
  - Microsoft Visual C and Borland C provide no mechanism
    to set the sticky flags. On top of that, the MS functions are
    very inefficient since they convert all the bits to some other 
    (DEC alpha?) format.
  - Mingw32 does have a MS conformant [float.h] but it conflicts with
    the default GNU [float.h].
  - Cygwin does have [ieefp.h] but no library that implements it.
  - FreeBSD [fpsetsticky] clears all the sticky flags instead of setting them.
  - NetBSD [fpsetsticky] sets the control word! instead of the status word,
    effectively enabling SIG_FPE signals instead of setting sticky flags..

  Incredible, especially when we consider that the x87 was the driving
  force behind the IEEE 754 standard. We implement floating point support
  in assembly by default on the IA32 platforms. Surprisingly, it is 
  quite easy to do which makes one wonder why it the implementations are
  so diverse.
----------------------------------------------------------------------*/
#if defined(ARCH_IA32) 

#if defined(_MSC_VER)
# define FLOAT_ASM_IA32
# define asm_fstcw(cw)    __asm{ fstcw cw }
# define asm_fldcw(cw)    __asm{ fldcw cw }
# define asm_fstsw(sw)    __asm{ fstsw sw }
# define asm_fldenv(env)  __asm{ fldenv env }
# define asm_fstenv(env)  __asm{ fstenv env }
#elif defined(__GNUC__)
# define FLOAT_ASM_IA32
# define asm_fstcw(cw)    __asm __volatile("fstcw %0" : "=m" (cw))
# define asm_fldcw(cw)    __asm __volatile("fldcw %0" : : "m" (cw))
# define asm_fstsw(sw)    __asm __volatile("fstsw %0" : "=m" (sw))
# define asm_fldenv(env)  __asm __volatile("fldenv %0" : : "m" (*(env)))
# define asm_fstenv(env)  __asm __volatile("fstenv %0" : "=m" (*(env)))
#else
  /* no assembler support, use default implementations */
#endif

#endif


/*----------------------------------------------------------------------
-- IEEE floating point on i386 platforms
----------------------------------------------------------------------*/
#if defined(FLOAT_ASM_IA32)

#define FP_STICKY_MASK 0x003f
#define FP_TRAP_MASK   0x003f
#define FP_ROUND_MASK  0x0c00

#define FP_STATUS_REG  1
#define FP_ENV_SIZE    7

#define FP_X_INV  0x01
#define FP_X_DNML 0x02
#define FP_X_DZ   0x04
#define FP_X_OFL  0x08
#define FP_X_UFL  0x10
#define FP_X_IMP  0x20

#define FP_RN     0x0000
#define FP_RM     0x0400
#define FP_RP     0x0800
#define FP_RZ     0x0c0

typedef unsigned int fp_reg;

static long fp_sticky_masks[Fp_exn_count] =
  { FP_X_INV, FP_X_DZ, FP_X_OFL, FP_X_UFL, FP_X_IMP, FP_X_DNML };

static long fp_trap_masks[Fp_exn_count] =
  { FP_X_INV, FP_X_DZ, FP_X_OFL, FP_X_UFL, FP_X_IMP, FP_X_DNML };

static long fp_round_masks[fp_round_count] =
  { FP_RN, FP_RP, FP_RM, FP_RZ };


long fp_get_sticky( void )
{
  volatile fp_reg sw;
  asm_fstsw(sw);
  return (sw & FP_STICKY_MASK);
}

long fp_set_sticky( long sticky )
{
  volatile fp_reg sw;
  volatile fp_reg env[FP_ENV_SIZE];
  asm_fstenv(env);
  sw                 = env[FP_STATUS_REG];
  env[FP_STATUS_REG] = (env[1] & ~FP_STICKY_MASK) | (sticky & FP_STICKY_MASK);
  asm_fldenv(env);
  return (sw & FP_STICKY_MASK);
}


static fp_reg fp_control( fp_reg control, fp_reg mask, fp_reg resmask ) 
{
  volatile fp_reg cw;
  volatile fp_reg cwnew;
  asm_fstcw(cw);
  if (mask != 0) {
    cwnew = (cw & ~mask) | (control & mask);
    asm_fldcw(cwnew);
  }
  return (cw & resmask);
}  

long fp_get_traps( void )
{
  return ~fp_control(0,0,FP_TRAP_MASK);
}

long fp_set_traps( long traps )
{
  return ~fp_control(~traps,FP_TRAP_MASK,FP_TRAP_MASK);
}

enum fp_round fp_get_round( void )
{
  return fp_round_unmask(fp_control(0,0,FP_ROUND_MASK));
}

enum fp_round fp_set_round( enum fp_round rnd )
{
  return fp_round_unmask(fp_control(fp_round_mask(rnd),FP_ROUND_MASK,FP_ROUND_MASK));
}

void fp_reset(void)
{
#ifdef HAS_CONTROLFP
  _fpreset();
#endif
}


/*----------------------------------------------------------------------
-- IEEE floating point on standard unix's
----------------------------------------------------------------------*/
#elif defined(HAS_IEEEFP_H)

#ifndef FP_X_DNML
#define FP_X_DNML 0
#endif

#ifndef FP_X_DZ
# ifdef FP_X_DX
#  define FP_X_DZ FP_X_DX
# else
#  define FP_X_DZ 0
# endif
#endif

static long fp_sticky_masks[Fp_exn_count] =
  { FP_X_INV, FP_X_DZ, FP_X_OFL, FP_X_UFL, FP_X_IMP, FP_X_DNML };

static long fp_trap_masks[Fp_exn_count] =
  { FP_X_INV, FP_X_DZ, FP_X_OFL, FP_X_UFL, FP_X_IMP, FP_X_DNML };

static long fp_round_masks[fp_round_count] =
  { FP_RN, FP_RP, FP_RM, FP_RZ };

void fp_reset( void )
{
  return;
}

/* sticky */
long fp_get_sticky(void)
{
  return fpgetsticky();
}

long fp_set_sticky( long sticky )
{ 
  return fpsetsticky( sticky );
}

/* traps */
long fp_get_traps(void)
{
  return fpgetmask();
}

long fp_set_traps( long traps )
{ 
  return fpsetmask(traps);
}

/* rounding */
enum fp_round fp_get_round( void )
{
  return fp_round_unmask( fpgetround() );
}

enum fp_round fp_set_round( enum fp_round rnd )
{
  return fp_round_unmask( fpsetround(fp_round_mask(rnd)) );
}


/*----------------------------------------------------------------------
-- IEEE floating point on Visual C++/Mingw32/Borland systems
----------------------------------------------------------------------*/
#elif defined(HAS_CONTROLFP)

static long fp_sticky_masks[Fp_exn_count] =
  { _EM_INVALID, _EM_ZERODIVIDE, _EM_OVERFLOW, _EM_UNDERFLOW, _EM_INEXACT, _EM_DENORMAL };

static long fp_trap_masks[Fp_exn_count] =
  { _EM_INVALID, _EM_ZERODIVIDE, _EM_OVERFLOW, _EM_UNDERFLOW, _EM_INEXACT, _EM_DENORMAL };

static long fp_round_masks[fp_round_count] =
  { _RC_NEAR, _RC_UP, _RC_DOWN, _RC_CHOP };

/* management */
void fp_reset( void )
{
  _fpreset();
}

/* rounding */
enum fp_round fp_get_round( void )
{
  return fp_round_unmask( _controlfp(0,0) & _MCW_RC );
}

enum fp_round fp_set_round( enum fp_round rnd )
{
  return fp_round_unmask( _controlfp(fp_round_mask(rnd),_MCW_RC) & _MCW_RC );
}

/* traps */
long fp_get_traps( void )
{
  return ~(_controlfp(0,0) & _MCW_EM);
}

long fp_set_traps( long traps )
{
  return ~(_controlfp(~traps, _MCW_EM) & _MCW_EM);
}

/* sticky */
long fp_get_sticky( void )
{
  return (_statusfp() & _MCW_EM);
}

/* sticky bits can only be set with assembly code :-( */
long fp_set_sticky( long sticky )
{
  raise_arithmetic_exn( Fpe_unemulated );
  return fp_get_sticky();
}


/*----------------------------------------------------------------------
-- IEEE floating point unsupported
----------------------------------------------------------------------*/
#else
static long fp_sticky_masks[Fp_exn_count] =
  { 0,0,0,0,0,0 };

static long fp_trap_masks[Fp_exn_count] =
  { 0,0,0,0,0,0 };

static long fp_round_masks[fp_round_count] =
  { 0,0,0,0 };

void fp_reset( void )
{
  return;
}

/* sticky */
long fp_get_sticky(void)
{
  raise_arithmetic_exn( Fpe_unemulated );
  return 0;
}

long fp_set_sticky( long sticky )
{ 
  raise_arithmetic_exn( Fpe_unemulated );
  return 0;
}

/* traps */
long fp_get_traps(void)
{
  raise_arithmetic_exn( Fpe_unemulated );
  return 0;
}

long fp_set_traps( long traps )
{ 
  raise_arithmetic_exn( Fpe_unemulated );
  return 0;
}

/* rounding */
enum fp_round fp_get_round( void )
{
  raise_arithmetic_exn( Fpe_unemulated );
  return fp_round_near;
}

enum fp_round fp_set_round( enum fp_round rnd )
{
  raise_arithmetic_exn( Fpe_unemulated );
  return fp_round_near;
}

#endif

/*
int main( int argc, char** argv )
{
  double x,y;
  long f1,f2,f3;
  fp_set_traps( fp_get_traps() | fp_trap_mask(fp_ex_inexact) );
  x = 1.0;
  y = 0.1;
  x = y + x;
  f1 = fp_get_sticky();
  f2 = fp_set_sticky( 0 );
  f3 = fp_get_sticky(); 
  printf( "%g, %x %x %x\n", x, f1, f2, f3 );
  
  return 0;
}
*/