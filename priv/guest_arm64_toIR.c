/* -*- mode: C; c-basic-offset: 3; -*- */

/*--------------------------------------------------------------------*/
/*--- begin                                     guest_arm64_toIR.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2013-2013 OpenWorks
      info@open-works.net

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   The GNU General Public License is contained in the file COPYING.
*/

//ZZ /* XXXX thumb to check:
//ZZ    that all cases where putIRegT writes r15, we generate a jump.
//ZZ 
//ZZ    All uses of newTemp assign to an IRTemp and not a UInt
//ZZ 
//ZZ    For all thumb loads and stores, including VFP ones, new-ITSTATE is
//ZZ    backed out before the memory op, and restored afterwards.  This
//ZZ    needs to happen even after we go uncond.  (and for sure it doesn't
//ZZ    happen for VFP loads/stores right now).
//ZZ 
//ZZ    VFP on thumb: check that we exclude all r13/r15 cases that we
//ZZ    should.
//ZZ 
//ZZ    XXXX thumb to do: improve the ITSTATE-zeroing optimisation by
//ZZ    taking into account the number of insns guarded by an IT.
//ZZ 
//ZZ    remove the nasty hack, in the spechelper, of looking for Or32(...,
//ZZ    0xE0) in as the first arg to armg_calculate_condition, and instead
//ZZ    use Slice44 as specified in comments in the spechelper.
//ZZ 
//ZZ    add specialisations for armg_calculate_flag_c and _v, as they
//ZZ    are moderately often needed in Thumb code.
//ZZ 
//ZZ    Correctness: ITSTATE handling in Thumb SVCs is wrong.
//ZZ 
//ZZ    Correctness (obscure): in m_transtab, when invalidating code
//ZZ    address ranges, invalidate up to 18 bytes after the end of the
//ZZ    range.  This is because the ITSTATE optimisation at the top of
//ZZ    _THUMB_WRK below analyses up to 18 bytes before the start of any
//ZZ    given instruction, and so might depend on the invalidated area.
//ZZ */
//ZZ 
//ZZ /* Limitations, etc
//ZZ 
//ZZ    - pretty dodgy exception semantics for {LD,ST}Mxx and {LD,ST}RD.
//ZZ      These instructions are non-restartable in the case where the
//ZZ      transfer(s) fault.
//ZZ 
//ZZ    - SWP: the restart jump back is Ijk_Boring; it should be
//ZZ      Ijk_NoRedir but that's expensive.  See comments on casLE() in
//ZZ      guest_x86_toIR.c.
//ZZ */

/* "Special" instructions.

   This instruction decoder can decode four special instructions
   which mean nothing natively (are no-ops as far as regs/mem are
   concerned) but have meaning for supporting Valgrind.  A special
   instruction is flagged by a 16-byte preamble:

      93CC0D8C 93CC358C 93CCCD8C 93CCF58C
      (ror x12, x12, #3;   ror x12, x12, #13
       ror x12, x12, #51;  ror x12, x12, #61)

   Following that, one of the following 3 are allowed
   (standard interpretation in parentheses):

      AA0A014A (orr x10,x10,x10)   X3 = client_request ( X4 )
      AA0B016B (orr x11,x11,x11)   X3 = guest_NRADDR
      AA0C018C (orr x12,x12,x12)   branch-and-link-to-noredir X8
      AA090129 (orr x9,x9,x9)      IR injection

   Any other bytes following the 16-byte preamble are illegal and
   constitute a failure in instruction decoding.  This all assumes
   that the preamble will never occur except in specific code
   fragments designed for Valgrind to catch.
*/

/* Translates ARM64 code to IR. */

#include "libvex_basictypes.h"
#include "libvex_ir.h"
#include "libvex.h"
#include "libvex_guest_arm64.h"

#include "main_util.h"
#include "main_globals.h"
#include "guest_generic_bb_to_IR.h"
#include "guest_arm64_defs.h"


/*------------------------------------------------------------*/
/*--- Globals                                              ---*/
/*------------------------------------------------------------*/

/* These are set at the start of the translation of a instruction, so
   that we don't have to pass them around endlessly.  CONST means does
   not change during translation of the instruction.
*/

/* CONST: is the host bigendian?  We need to know this in order to do
   sub-register accesses to the SIMD/FP registers correctly. */
static Bool host_is_bigendian;

/* CONST: The guest address for the instruction currently being
   translated.  */
static Addr64 guest_PC_curr_instr;

/* MOD: The IRSB* into which we're generating code. */
static IRSB* irsb;


/*------------------------------------------------------------*/
/*--- Debugging output                                     ---*/
/*------------------------------------------------------------*/

#define DIP(format, args...)           \
   if (vex_traceflags & VEX_TRACE_FE)  \
      vex_printf(format, ## args)

#define DIS(buf, format, args...)      \
   if (vex_traceflags & VEX_TRACE_FE)  \
      vex_sprintf(buf, format, ## args)


/*------------------------------------------------------------*/
/*--- Helper bits and pieces for deconstructing the        ---*/
/*--- arm insn stream.                                     ---*/
/*------------------------------------------------------------*/

/* Do a little-endian load of a 32-bit word, regardless of the
   endianness of the underlying host. */
static inline UInt getUIntLittleEndianly ( UChar* p )
{
   UInt w = 0;
   w = (w << 8) | p[3];
   w = (w << 8) | p[2];
   w = (w << 8) | p[1];
   w = (w << 8) | p[0];
   return w;
}

/* Sign extend a N-bit value up to 64 bits, by copying
   bit N-1 into all higher positions. */
static ULong sx_to_64 ( ULong x, UInt n )
{
   vassert(n > 1 && n < 64);
   Long r = (Long)x;
   r = (r << (64-n)) >> (64-n);
   return (ULong)r;
}

//ZZ /* Do a little-endian load of a 16-bit word, regardless of the
//ZZ    endianness of the underlying host. */
//ZZ static inline UShort getUShortLittleEndianly ( UChar* p )
//ZZ {
//ZZ    UShort w = 0;
//ZZ    w = (w << 8) | p[1];
//ZZ    w = (w << 8) | p[0];
//ZZ    return w;
//ZZ }
//ZZ 
//ZZ static UInt ROR32 ( UInt x, UInt sh ) {
//ZZ    vassert(sh >= 0 && sh < 32);
//ZZ    if (sh == 0)
//ZZ       return x;
//ZZ    else
//ZZ       return (x << (32-sh)) | (x >> sh);
//ZZ }
//ZZ 
//ZZ static Int popcount32 ( UInt x )
//ZZ {
//ZZ    Int res = 0, i;
//ZZ    for (i = 0; i < 32; i++) {
//ZZ       res += (x & 1);
//ZZ       x >>= 1;
//ZZ    }
//ZZ    return res;
//ZZ }
//ZZ 
//ZZ static UInt setbit32 ( UInt x, Int ix, UInt b )
//ZZ {
//ZZ    UInt mask = 1 << ix;
//ZZ    x &= ~mask;
//ZZ    x |= ((b << ix) & mask);
//ZZ    return x;
//ZZ }

#define BITS2(_b1,_b0)  \
   (((_b1) << 1) | (_b0))

#define BITS3(_b2,_b1,_b0)  \
  (((_b2) << 2) | ((_b1) << 1) | (_b0))

#define BITS4(_b3,_b2,_b1,_b0)  \
   (((_b3) << 3) | ((_b2) << 2) | ((_b1) << 1) | (_b0))

#define BITS8(_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0)  \
   ((BITS4((_b7),(_b6),(_b5),(_b4)) << 4)  \
    | BITS4((_b3),(_b2),(_b1),(_b0)))

#define BITS5(_b4,_b3,_b2,_b1,_b0)  \
   (BITS8(0,0,0,(_b4),(_b3),(_b2),(_b1),(_b0)))
#define BITS6(_b5,_b4,_b3,_b2,_b1,_b0)  \
   (BITS8(0,0,(_b5),(_b4),(_b3),(_b2),(_b1),(_b0)))
#define BITS7(_b6,_b5,_b4,_b3,_b2,_b1,_b0)  \
   (BITS8(0,(_b6),(_b5),(_b4),(_b3),(_b2),(_b1),(_b0)))

#define BITS9(_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0)  \
   (((_b8) << 8)  \
    | BITS8((_b7),(_b6),(_b5),(_b4),(_b3),(_b2),(_b1),(_b0)))

#define BITS10(_b9,_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0)  \
   (((_b9) << 9) | ((_b8) << 8)  \
    | BITS8((_b7),(_b6),(_b5),(_b4),(_b3),(_b2),(_b1),(_b0)))

#define BITS11(_b10,_b9,_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0)  \
   (((_b10) << 10)  \
    | BITS10(_b9,_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0))

#define BITS12(_b11, _b10,_b9,_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0) \
   (((_b11) << 11)  \
    | BITS11(_b10,_b9,_b8,_b7,_b6,_b5,_b4,_b3,_b2,_b1,_b0))

#define X00 BITS2(0,0)
#define X01 BITS2(0,1)
#define X10 BITS2(1,0)
#define X11 BITS2(1,1)

// produces _uint[_bMax:_bMin]
#define SLICE_UInt(_uint,_bMax,_bMin)  \
   (( ((UInt)(_uint)) >> (_bMin))  \
    & (UInt)((1ULL << ((_bMax) - (_bMin) + 1)) - 1ULL))


/*------------------------------------------------------------*/
/*--- Helper bits and pieces for creating IR fragments.    ---*/
/*------------------------------------------------------------*/

static IRExpr* mkV128 ( UShort w )
{
   return IRExpr_Const(IRConst_V128(w));
}

static IRExpr* mkU64 ( ULong i )
{
   return IRExpr_Const(IRConst_U64(i));
}

static IRExpr* mkU32 ( UInt i )
{
   return IRExpr_Const(IRConst_U32(i));
}

static IRExpr* mkU16 ( UInt i )
{
   vassert(i < 65536);
   return IRExpr_Const(IRConst_U16(i));
}

static IRExpr* mkU8 ( UInt i )
{
   vassert(i < 256);
   return IRExpr_Const(IRConst_U8( (UChar)i ));
}

static IRExpr* mkexpr ( IRTemp tmp )
{
   return IRExpr_RdTmp(tmp);
}

static IRExpr* unop ( IROp op, IRExpr* a )
{
   return IRExpr_Unop(op, a);
}

static IRExpr* binop ( IROp op, IRExpr* a1, IRExpr* a2 )
{
   return IRExpr_Binop(op, a1, a2);
}

static IRExpr* triop ( IROp op, IRExpr* a1, IRExpr* a2, IRExpr* a3 )
{
   return IRExpr_Triop(op, a1, a2, a3);
}

static IRExpr* loadLE ( IRType ty, IRExpr* addr )
{
   return IRExpr_Load(Iend_LE, ty, addr);
}

/* Add a statement to the list held by "irbb". */
static void stmt ( IRStmt* st )
{
   addStmtToIRSB( irsb, st );
}

static void assign ( IRTemp dst, IRExpr* e )
{
   stmt( IRStmt_WrTmp(dst, e) );
}

static void storeLE ( IRExpr* addr, IRExpr* data )
{
   stmt( IRStmt_Store(Iend_LE, addr, data) );
}

//ZZ static void storeGuardedLE ( IRExpr* addr, IRExpr* data, IRTemp guardT )
//ZZ {
//ZZ    if (guardT == IRTemp_INVALID) {
//ZZ       /* unconditional */
//ZZ       storeLE(addr, data);
//ZZ    } else {
//ZZ       stmt( IRStmt_StoreG(Iend_LE, addr, data,
//ZZ                           binop(Iop_CmpNE32, mkexpr(guardT), mkU32(0))) );
//ZZ    }
//ZZ }
//ZZ 
//ZZ static void loadGuardedLE ( IRTemp dst, IRLoadGOp cvt,
//ZZ                             IRExpr* addr, IRExpr* alt, 
//ZZ                             IRTemp guardT /* :: Ity_I32, 0 or 1 */ )
//ZZ {
//ZZ    if (guardT == IRTemp_INVALID) {
//ZZ       /* unconditional */
//ZZ       IRExpr* loaded = NULL;
//ZZ       switch (cvt) {
//ZZ          case ILGop_Ident32:
//ZZ             loaded = loadLE(Ity_I32, addr); break;
//ZZ          case ILGop_8Uto32:
//ZZ             loaded = unop(Iop_8Uto32, loadLE(Ity_I8, addr)); break;
//ZZ          case ILGop_8Sto32:
//ZZ             loaded = unop(Iop_8Sto32, loadLE(Ity_I8, addr)); break;
//ZZ          case ILGop_16Uto32:
//ZZ             loaded = unop(Iop_16Uto32, loadLE(Ity_I16, addr)); break;
//ZZ          case ILGop_16Sto32:
//ZZ             loaded = unop(Iop_16Sto32, loadLE(Ity_I16, addr)); break;
//ZZ          default:
//ZZ             vassert(0);
//ZZ       }
//ZZ       vassert(loaded != NULL);
//ZZ       assign(dst, loaded);
//ZZ    } else {
//ZZ       /* Generate a guarded load into 'dst', but apply 'cvt' to the
//ZZ          loaded data before putting the data in 'dst'.  If the load
//ZZ          does not take place, 'alt' is placed directly in 'dst'. */
//ZZ       stmt( IRStmt_LoadG(Iend_LE, cvt, dst, addr, alt,
//ZZ                          binop(Iop_CmpNE32, mkexpr(guardT), mkU32(0))) );
//ZZ    }
//ZZ }

/* Generate a new temporary of the given type. */
static IRTemp newTemp ( IRType ty )
{
   vassert(isPlausibleIRType(ty));
   return newIRTemp( irsb->tyenv, ty );
}

//ZZ /* Produces a value in 0 .. 3, which is encoded as per the type
//ZZ    IRRoundingMode. */
//ZZ static IRExpr* /* :: Ity_I32 */ get_FAKE_roundingmode ( void )
//ZZ {
//ZZ    return mkU32(Irrm_NEAREST);
//ZZ }
//ZZ 
//ZZ /* Generate an expression for SRC rotated right by ROT. */
//ZZ static IRExpr* genROR32( IRTemp src, Int rot )
//ZZ {
//ZZ    vassert(rot >= 0 && rot < 32);
//ZZ    if (rot == 0)
//ZZ       return mkexpr(src);
//ZZ    return
//ZZ       binop(Iop_Or32,
//ZZ             binop(Iop_Shl32, mkexpr(src), mkU8(32 - rot)),
//ZZ             binop(Iop_Shr32, mkexpr(src), mkU8(rot)));
//ZZ }
//ZZ 
//ZZ static IRExpr* mkU128 ( ULong i )
//ZZ {
//ZZ    return binop(Iop_64HLtoV128, mkU64(i), mkU64(i));
//ZZ }
//ZZ 
//ZZ /* Generate a 4-aligned version of the given expression if
//ZZ    the given condition is true.  Else return it unchanged. */
//ZZ static IRExpr* align4if ( IRExpr* e, Bool b )
//ZZ {
//ZZ    if (b)
//ZZ       return binop(Iop_And32, e, mkU32(~3));
//ZZ    else
//ZZ       return e;
//ZZ }

/* Other IR construction helpers. */
static IROp mkAND ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_And32;
      case Ity_I64: return Iop_And64;
      default: vpanic("mkAND");
   }
}

static IROp mkOR ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Or32;
      case Ity_I64: return Iop_Or64;
      default: vpanic("mkOR");
   }
}

static IROp mkXOR ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Xor32;
      case Ity_I64: return Iop_Xor64;
      default: vpanic("mkXOR");
   }
}

static IROp mkSHL ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Shl32;
      case Ity_I64: return Iop_Shl64;
      default: vpanic("mkSHL");
   }
}

static IROp mkSHR ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Shr32;
      case Ity_I64: return Iop_Shr64;
      default: vpanic("mkSHR");
   }
}

static IROp mkSAR ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Sar32;
      case Ity_I64: return Iop_Sar64;
      default: vpanic("mkSAR");
   }
}

static IROp mkNOT ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Not32;
      case Ity_I64: return Iop_Not64;
      default: vpanic("mkNOT");
   }
}

static IROp mkADD ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Add32;
      case Ity_I64: return Iop_Add64;
      default: vpanic("mkADD");
   }
}

static IROp mkSUB ( IRType ty ) {
   switch (ty) {
      case Ity_I32: return Iop_Sub32;
      case Ity_I64: return Iop_Sub64;
      default: vpanic("mkSUB");
   }
}

static IROp mkADDF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_AddF32;
      case Ity_F64: return Iop_AddF64;
      default: vpanic("mkADDF");
   }
}

static IROp mkSUBF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_SubF32;
      case Ity_F64: return Iop_SubF64;
      default: vpanic("mkSUBF");
   }
}

static IROp mkMULF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_MulF32;
      case Ity_F64: return Iop_MulF64;
      default: vpanic("mkMULF");
   }
}

static IROp mkDIVF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_DivF32;
      case Ity_F64: return Iop_DivF64;
      default: vpanic("mkMULF");
   }
}

static IROp mkNEGF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_NegF32;
      case Ity_F64: return Iop_NegF64;
      default: vpanic("mkNEGF");
   }
}

static IROp mkABSF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_AbsF32;
      case Ity_F64: return Iop_AbsF64;
      default: vpanic("mkNEGF");
   }
}

static IROp mkSQRTF ( IRType ty ) {
   switch (ty) {
      case Ity_F32: return Iop_SqrtF32;
      case Ity_F64: return Iop_SqrtF64;
      default: vpanic("mkNEGF");
   }
}

static IRExpr* mkU ( IRType ty, ULong imm ) {
   switch (ty) {
      case Ity_I32: return mkU32((UInt)(imm & 0xFFFFFFFFULL));
      case Ity_I64: return mkU64(imm);
      default: vpanic("mkU");
   }
}

/* Generate IR to create 'arg rotated right by imm', for sane values
   of 'ty' and 'imm'. */
static IRTemp mathROR ( IRType ty, IRTemp arg, UInt imm )
{
   UInt w = 0;
   if (ty == Ity_I64) {
      w = 64;
   } else {
      vassert(ty == Ity_I32);
      w = 32;
   }
   vassert(w != 0);
   vassert(imm < w);
   if (imm == 0) {
      return arg;
   }
   IRTemp res = newTemp(ty);
   assign(res, binop(mkOR(ty),
                     binop(mkSHL(ty), mkexpr(arg), mkU8(w - imm)),
                     binop(mkSHR(ty), mkexpr(arg), mkU8(imm)) ));
   return res;
}

/* Generate IR to set the returned temp to either all-zeroes or
   all ones, as a copy of arg<imm>. */
static IRTemp mathREPLICATE ( IRType ty, IRTemp arg, UInt imm )
{
   UInt w = 0;
   if (ty == Ity_I64) {
      w = 64;
   } else {
      vassert(ty == Ity_I32);
      w = 32;
   }
   vassert(w != 0);
   vassert(imm < w);
   IRTemp res = newTemp(ty);
   assign(res, binop(mkSAR(ty),
                     binop(mkSHL(ty), mkexpr(arg), mkU8(w - 1 - imm)),
                     mkU8(w - 1)));
   return res;
}

/* U-widen 8/16/32/64 bit int expr to 64. */
static IRExpr* widenUto64 ( IRType srcTy, IRExpr* e )
{
   switch (srcTy) {
      case Ity_I64: return e;
      case Ity_I32: return unop(Iop_32Uto64, e);
      case Ity_I16: return unop(Iop_16Uto64, e);
      case Ity_I8:  return unop(Iop_8Uto64, e);
      default: vpanic("widenUto64(arm64)");
   }
}

/* Narrow 64 bit int expr to 8/16/32/64.  Clearly only some
   of these combinations make sense. */
static IRExpr* narrowFrom64 ( IRType dstTy, IRExpr* e )
{
   switch (dstTy) {
      case Ity_I64: return e;
      case Ity_I32: return unop(Iop_64to32, e);
      case Ity_I16: return unop(Iop_64to16, e);
      case Ity_I8:  return unop(Iop_64to8, e);
      default: vpanic("narrowFrom64(arm64)");
   }
}


/*------------------------------------------------------------*/
/*--- Helpers for accessing guest registers.               ---*/
/*------------------------------------------------------------*/

#define OFFB_X0       offsetof(VexGuestARM64State,guest_X0)
#define OFFB_X1       offsetof(VexGuestARM64State,guest_X1)
#define OFFB_X2       offsetof(VexGuestARM64State,guest_X2)
#define OFFB_X3       offsetof(VexGuestARM64State,guest_X3)
#define OFFB_X4       offsetof(VexGuestARM64State,guest_X4)
#define OFFB_X5       offsetof(VexGuestARM64State,guest_X5)
#define OFFB_X6       offsetof(VexGuestARM64State,guest_X6)
#define OFFB_X7       offsetof(VexGuestARM64State,guest_X7)
#define OFFB_X8       offsetof(VexGuestARM64State,guest_X8)
#define OFFB_X9       offsetof(VexGuestARM64State,guest_X9)
#define OFFB_X10      offsetof(VexGuestARM64State,guest_X10)
#define OFFB_X11      offsetof(VexGuestARM64State,guest_X11)
#define OFFB_X12      offsetof(VexGuestARM64State,guest_X12)
#define OFFB_X13      offsetof(VexGuestARM64State,guest_X13)
#define OFFB_X14      offsetof(VexGuestARM64State,guest_X14)
#define OFFB_X15      offsetof(VexGuestARM64State,guest_X15)
#define OFFB_X16      offsetof(VexGuestARM64State,guest_X16)
#define OFFB_X17      offsetof(VexGuestARM64State,guest_X17)
#define OFFB_X18      offsetof(VexGuestARM64State,guest_X18)
#define OFFB_X19      offsetof(VexGuestARM64State,guest_X19)
#define OFFB_X20      offsetof(VexGuestARM64State,guest_X20)
#define OFFB_X21      offsetof(VexGuestARM64State,guest_X21)
#define OFFB_X22      offsetof(VexGuestARM64State,guest_X22)
#define OFFB_X23      offsetof(VexGuestARM64State,guest_X23)
#define OFFB_X24      offsetof(VexGuestARM64State,guest_X24)
#define OFFB_X25      offsetof(VexGuestARM64State,guest_X25)
#define OFFB_X26      offsetof(VexGuestARM64State,guest_X26)
#define OFFB_X27      offsetof(VexGuestARM64State,guest_X27)
#define OFFB_X28      offsetof(VexGuestARM64State,guest_X28)
#define OFFB_X29      offsetof(VexGuestARM64State,guest_X29)
#define OFFB_X30      offsetof(VexGuestARM64State,guest_X30)

#define OFFB_XSP      offsetof(VexGuestARM64State,guest_XSP)
#define OFFB_PC       offsetof(VexGuestARM64State,guest_PC)

#define OFFB_CC_OP    offsetof(VexGuestARM64State,guest_CC_OP)
#define OFFB_CC_DEP1  offsetof(VexGuestARM64State,guest_CC_DEP1)
#define OFFB_CC_DEP2  offsetof(VexGuestARM64State,guest_CC_DEP2)
#define OFFB_CC_NDEP  offsetof(VexGuestARM64State,guest_CC_NDEP)

#define OFFB_TPIDR_EL0 offsetof(VexGuestARM64State,guest_TPIDR_EL0)
#define OFFB_NRADDR   offsetof(VexGuestARM64State,guest_NRADDR)

#define OFFB_Q0       offsetof(VexGuestARM64State,guest_Q0)
#define OFFB_Q1       offsetof(VexGuestARM64State,guest_Q1)
#define OFFB_Q2       offsetof(VexGuestARM64State,guest_Q2)
#define OFFB_Q3       offsetof(VexGuestARM64State,guest_Q3)
#define OFFB_Q4       offsetof(VexGuestARM64State,guest_Q4)
#define OFFB_Q5       offsetof(VexGuestARM64State,guest_Q5)
#define OFFB_Q6       offsetof(VexGuestARM64State,guest_Q6)
#define OFFB_Q7       offsetof(VexGuestARM64State,guest_Q7)
#define OFFB_Q8       offsetof(VexGuestARM64State,guest_Q8)
#define OFFB_Q9       offsetof(VexGuestARM64State,guest_Q9)
#define OFFB_Q10      offsetof(VexGuestARM64State,guest_Q10)
#define OFFB_Q11      offsetof(VexGuestARM64State,guest_Q11)
#define OFFB_Q12      offsetof(VexGuestARM64State,guest_Q12)
#define OFFB_Q13      offsetof(VexGuestARM64State,guest_Q13)
#define OFFB_Q14      offsetof(VexGuestARM64State,guest_Q14)
#define OFFB_Q15      offsetof(VexGuestARM64State,guest_Q15)
#define OFFB_Q16      offsetof(VexGuestARM64State,guest_Q16)
#define OFFB_Q17      offsetof(VexGuestARM64State,guest_Q17)
#define OFFB_Q18      offsetof(VexGuestARM64State,guest_Q18)
#define OFFB_Q19      offsetof(VexGuestARM64State,guest_Q19)
#define OFFB_Q20      offsetof(VexGuestARM64State,guest_Q20)
#define OFFB_Q21      offsetof(VexGuestARM64State,guest_Q21)
#define OFFB_Q22      offsetof(VexGuestARM64State,guest_Q22)
#define OFFB_Q23      offsetof(VexGuestARM64State,guest_Q23)
#define OFFB_Q24      offsetof(VexGuestARM64State,guest_Q24)
#define OFFB_Q25      offsetof(VexGuestARM64State,guest_Q25)
#define OFFB_Q26      offsetof(VexGuestARM64State,guest_Q26)
#define OFFB_Q27      offsetof(VexGuestARM64State,guest_Q27)
#define OFFB_Q28      offsetof(VexGuestARM64State,guest_Q28)
#define OFFB_Q29      offsetof(VexGuestARM64State,guest_Q29)
#define OFFB_Q30      offsetof(VexGuestARM64State,guest_Q30)
#define OFFB_Q31      offsetof(VexGuestARM64State,guest_Q31)

#define OFFB_FPCR     offsetof(VexGuestARM64State,guest_FPCR)
#define OFFB_FPSR     offsetof(VexGuestARM64State,guest_FPSR)
//ZZ #define OFFB_TPIDRURO offsetof(VexGuestARMState,guest_TPIDRURO)
//ZZ #define OFFB_ITSTATE  offsetof(VexGuestARMState,guest_ITSTATE)
//ZZ #define OFFB_QFLAG32  offsetof(VexGuestARMState,guest_QFLAG32)
//ZZ #define OFFB_GEFLAG0  offsetof(VexGuestARMState,guest_GEFLAG0)
//ZZ #define OFFB_GEFLAG1  offsetof(VexGuestARMState,guest_GEFLAG1)
//ZZ #define OFFB_GEFLAG2  offsetof(VexGuestARMState,guest_GEFLAG2)
//ZZ #define OFFB_GEFLAG3  offsetof(VexGuestARMState,guest_GEFLAG3)

#define OFFB_CMSTART  offsetof(VexGuestARM64State,guest_CMSTART)
#define OFFB_CMLEN    offsetof(VexGuestARM64State,guest_CMLEN)


/* ---------------- Integer registers ---------------- */

static Int offsetIReg64 ( UInt iregNo )
{
   /* Do we care about endianness here?  We do if sub-parts of integer
      registers are accessed. */
   switch (iregNo) {
      case 0:  return OFFB_X0;
      case 1:  return OFFB_X1;
      case 2:  return OFFB_X2;
      case 3:  return OFFB_X3;
      case 4:  return OFFB_X4;
      case 5:  return OFFB_X5;
      case 6:  return OFFB_X6;
      case 7:  return OFFB_X7;
      case 8:  return OFFB_X8;
      case 9:  return OFFB_X9;
      case 10: return OFFB_X10;
      case 11: return OFFB_X11;
      case 12: return OFFB_X12;
      case 13: return OFFB_X13;
      case 14: return OFFB_X14;
      case 15: return OFFB_X15;
      case 16: return OFFB_X16;
      case 17: return OFFB_X17;
      case 18: return OFFB_X18;
      case 19: return OFFB_X19;
      case 20: return OFFB_X20;
      case 21: return OFFB_X21;
      case 22: return OFFB_X22;
      case 23: return OFFB_X23;
      case 24: return OFFB_X24;
      case 25: return OFFB_X25;
      case 26: return OFFB_X26;
      case 27: return OFFB_X27;
      case 28: return OFFB_X28;
      case 29: return OFFB_X29;
      case 30: return OFFB_X30;
      /* but not 31 */
      default: vassert(0);
   }
}

static Int offsetIReg64orSP ( UInt iregNo )
{
   return iregNo == 31  ? OFFB_XSP  : offsetIReg64(iregNo);
}

static const HChar* nameIReg64orZR ( UInt iregNo )
{
   vassert(iregNo < 32);
   static const HChar* names[32]
      = { "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7", 
          "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15", 
          "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", 
          "x24", "x25", "x26", "x27", "x28", "x29", "x30", "xzr" };
   return names[iregNo];
}

static const HChar* nameIReg64orSP ( UInt iregNo )
{
   if (iregNo == 31) {
      return "sp";
   }
   vassert(iregNo < 31);
   return nameIReg64orZR(iregNo);
}

static IRExpr* getIReg64orSP ( UInt iregNo )
{
   vassert(iregNo < 32);
   return IRExpr_Get( offsetIReg64orSP(iregNo), Ity_I64 );
}

static IRExpr* getIReg64orZR ( UInt iregNo )
{
   if (iregNo == 31) {
      return mkU64(0);
   }
   vassert(iregNo < 31);
   return IRExpr_Get( offsetIReg64orSP(iregNo), Ity_I64 );
}

static void putIReg64orSP ( UInt iregNo, IRExpr* e ) 
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I64);
   stmt( IRStmt_Put(offsetIReg64orSP(iregNo), e) );
}

static void putIReg64orZR ( UInt iregNo, IRExpr* e ) 
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I64);
   if (iregNo == 31) {
      return;
   }
   vassert(iregNo < 31);
   stmt( IRStmt_Put(offsetIReg64orSP(iregNo), e) );
}

static const HChar* nameIReg32orZR ( UInt iregNo )
{
   vassert(iregNo < 32);
   static const HChar* names[32]
      = { "w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7", 
          "w8",  "w9",  "w10", "w11", "w12", "w13", "w14", "w15", 
          "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23", 
          "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wzr" };
   return names[iregNo];
}

static const HChar* nameIReg32orSP ( UInt iregNo )
{
   if (iregNo == 31) {
      return "wsp";
   }
   vassert(iregNo < 31);
   return nameIReg32orZR(iregNo);
}

static IRExpr* getIReg32orSP ( UInt iregNo )
{
   vassert(iregNo < 32);
   return unop(Iop_64to32,
               IRExpr_Get( offsetIReg64orSP(iregNo), Ity_I64 ));
}

static IRExpr* getIReg32orZR ( UInt iregNo )
{
   if (iregNo == 31) {
      return mkU32(0);
   }
   vassert(iregNo < 31);
   return unop(Iop_64to32,
               IRExpr_Get( offsetIReg64orSP(iregNo), Ity_I64 ));
}

static void putIReg32orSP ( UInt iregNo, IRExpr* e ) 
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
   stmt( IRStmt_Put(offsetIReg64orSP(iregNo), unop(Iop_32Uto64, e)) );
}

static void putIReg32orZR ( UInt iregNo, IRExpr* e ) 
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
   if (iregNo == 31) {
      return;
   }
   vassert(iregNo < 31);
   stmt( IRStmt_Put(offsetIReg64orSP(iregNo), unop(Iop_32Uto64, e)) );
}

static const HChar* nameIRegOrSP ( Bool is64, UInt iregNo )
{
   vassert(is64 == True || is64 == False);
   return is64 ? nameIReg64orSP(iregNo) : nameIReg32orSP(iregNo);
}

static const HChar* nameIRegOrZR ( Bool is64, UInt iregNo )
{
   vassert(is64 == True || is64 == False);
   return is64 ? nameIReg64orZR(iregNo) : nameIReg32orZR(iregNo);
}

static IRExpr* getIRegOrZR ( Bool is64, UInt iregNo )
{
   vassert(is64 == True || is64 == False);
   return is64 ? getIReg64orZR(iregNo) : getIReg32orZR(iregNo);
}

static void putIRegOrZR ( Bool is64, UInt iregNo, IRExpr* e )
{
   vassert(is64 == True || is64 == False);
   if (is64) putIReg64orZR(iregNo, e); else putIReg32orZR(iregNo, e);
}

static void putPC ( IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I64);
   stmt( IRStmt_Put(OFFB_PC, e) );
}


/* ---------------- Vector (Q) registers ---------------- */

static Int offsetQReg128 ( UInt qregNo )
{
   /* We don't care about endianness at this point.  It only becomes
      relevant when dealing with sections of these registers.*/
   switch (qregNo) {
      case 0:  return OFFB_Q0;
      case 1:  return OFFB_Q1;
      case 2:  return OFFB_Q2;
      case 3:  return OFFB_Q3;
      case 4:  return OFFB_Q4;
      case 5:  return OFFB_Q5;
      case 6:  return OFFB_Q6;
      case 7:  return OFFB_Q7;
      case 8:  return OFFB_Q8;
      case 9:  return OFFB_Q9;
      case 10: return OFFB_Q10;
      case 11: return OFFB_Q11;
      case 12: return OFFB_Q12;
      case 13: return OFFB_Q13;
      case 14: return OFFB_Q14;
      case 15: return OFFB_Q15;
      case 16: return OFFB_Q16;
      case 17: return OFFB_Q17;
      case 18: return OFFB_Q18;
      case 19: return OFFB_Q19;
      case 20: return OFFB_Q20;
      case 21: return OFFB_Q21;
      case 22: return OFFB_Q22;
      case 23: return OFFB_Q23;
      case 24: return OFFB_Q24;
      case 25: return OFFB_Q25;
      case 26: return OFFB_Q26;
      case 27: return OFFB_Q27;
      case 28: return OFFB_Q28;
      case 29: return OFFB_Q29;
      case 30: return OFFB_Q30;
      case 31: return OFFB_Q31;
      default: vassert(0);
   }
}

/* Write to a complete Qreg. */
static void putQReg128 ( UInt qregNo, IRExpr* e )
{
   vassert(qregNo < 32);
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_V128);
   stmt( IRStmt_Put(offsetQReg128(qregNo), e) );
}

/* Read a complete Qreg. */
static IRExpr* getQReg128 ( UInt qregNo )
{
   vassert(qregNo < 32);
   return IRExpr_Get(offsetQReg128(qregNo), Ity_V128);
}

/* Produce the IR type for some sub-part of a vector.  For 32- and 64-
   bit sub-parts we can choose either integer or float types, and
   choose float on the basis that that is the common use case and so
   will give least interference with Put-to-Get forwarding later
   on. */
static IRType preferredVectorSubTypeFromSize ( UInt szB )
{
   switch (szB) {
      case 1:  return Ity_I8;
      case 2:  return Ity_I16;
      case 4:  return Ity_I32; //Ity_F32;
      case 8:  return Ity_F64;
      case 16: return Ity_V128;
      default: vassert(0);
   }
}

/* Find the offset of the laneNo'th lane of type laneTy in the given
   Qreg.  Since the host is little-endian, the least significant lane
   has the lowest offset. */
static Int offsetQRegLane ( UInt qregNo, IRType laneTy, UInt laneNo )
{
   vassert(!host_is_bigendian);
   Int base = offsetQReg128(qregNo);
   /* Since the host is little-endian, the least significant lane
      will be at the lowest address. */
   /* Restrict this to known types, so as to avoid silently accepting
      stupid types. */
   UInt laneSzB = 0;
   switch (laneTy) {
      case Ity_I8:                 laneSzB = 1;  break;
      case Ity_I16:                laneSzB = 2;  break;
      case Ity_F32: case Ity_I32:  laneSzB = 4;  break;
      case Ity_F64: case Ity_I64:  laneSzB = 8;  break;
      case Ity_V128:               laneSzB = 16; break;
      default: break;
   }
   vassert(laneSzB > 0);
   UInt minOff = laneNo * laneSzB;
   UInt maxOff = minOff + laneSzB - 1;
   vassert(maxOff < 16);
   return base + minOff;
}

/* Put to the least significant lane of a Qreg. */
static void putQRegLO ( UInt qregNo, IRExpr* e )
{
   IRType ty  = typeOfIRExpr(irsb->tyenv, e);
   Int    off = offsetQRegLane(qregNo, ty, 0);
   switch (ty) {
      case Ity_I8:  case Ity_I16: case Ity_I32: case Ity_I64:
      case Ity_F32: case Ity_F64: case Ity_V128:
         break;
      default:
         vassert(0); // Other cases are probably invalid
   }
   stmt(IRStmt_Put(off, e));
}

/* Get from the least significant lane of a Qreg. */
static IRExpr* getQRegLO ( UInt qregNo, IRType ty )
{
   Int off = offsetQRegLane(qregNo, ty, 0);
   switch (ty) {
      case Ity_I8:
      case Ity_I16:
      case Ity_I32: case Ity_I64:
      case Ity_F32: case Ity_F64: case Ity_V128:
         break;
      default:
         vassert(0); // Other cases are ATC
   }
   return IRExpr_Get(off, ty);
}

static const HChar* nameQRegLO ( UInt qregNo, IRType laneTy )
{
   static const HChar* namesQ[32]
      = { "q0",  "q1",  "q2",  "q3",  "q4",  "q5",  "q6",  "q7", 
          "q8",  "q9",  "q10", "q11", "q12", "q13", "q14", "q15", 
          "q16", "q17", "q18", "q19", "q20", "q21", "q22", "q23", 
          "q24", "q25", "q26", "q27", "q28", "q29", "q30", "q31" };
   static const HChar* namesD[32]
      = { "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7", 
          "d8",  "d9",  "d10", "d11", "d12", "d13", "d14", "d15", 
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", 
          "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31" };
   static const HChar* namesS[32]
      = { "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "s6",  "s7", 
          "s8",  "s9",  "s10", "s11", "s12", "s13", "s14", "s15", 
          "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23", 
          "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31" };
   static const HChar* namesH[32]
      = { "h0",  "h1",  "h2",  "h3",  "h4",  "h5",  "h6",  "h7", 
          "h8",  "h9",  "h10", "h11", "h12", "h13", "h14", "h15", 
          "h16", "h17", "h18", "h19", "h20", "h21", "h22", "h23", 
          "h24", "h25", "h26", "h27", "h28", "h29", "h30", "h31" };
   static const HChar* namesB[32]
      = { "b0",  "b1",  "b2",  "b3",  "b4",  "b5",  "b6",  "b7", 
          "b8",  "b9",  "b10", "b11", "b12", "b13", "b14", "b15", 
          "b16", "b17", "b18", "b19", "b20", "b21", "b22", "b23", 
          "b24", "b25", "b26", "b27", "b28", "b29", "b30", "b31" };
   vassert(qregNo < 32);
   switch (sizeofIRType(laneTy)) {
      case 1:  return namesB[qregNo];
      case 2:  return namesH[qregNo];
      case 4:  return namesS[qregNo];
      case 8:  return namesD[qregNo];
      case 16: return namesQ[qregNo];
      default: vassert(0);
   }
   /*NOTREACHED*/
}

static const HChar* nameQReg128 ( UInt qregNo )
{
   return nameQRegLO(qregNo, Ity_V128);
}

/* Find the offset of the most significant half (8 bytes) of the given
   Qreg.  This requires knowing the endianness of the host. */
static Int offsetQRegHI64 ( UInt qregNo )
{
   return offsetQRegLane(qregNo, Ity_I64, 1);
}

static IRExpr* getQRegHI64 ( UInt qregNo )
{
   return IRExpr_Get(offsetQRegHI64(qregNo), Ity_I64);
}

static void putQRegHI64 ( UInt qregNo, IRExpr* e )
{
   IRType ty  = typeOfIRExpr(irsb->tyenv, e);
   Int    off = offsetQRegHI64(qregNo);
   switch (ty) {
      case Ity_I64: case Ity_F64:
         break;
      default:
         vassert(0); // Other cases are plain wrong
   }
   stmt(IRStmt_Put(off, e));
}

/* Put to a specified lane of a Qreg. */
static void putQRegLane ( UInt qregNo, UInt laneNo, IRExpr* e )
{
   IRType laneTy  = typeOfIRExpr(irsb->tyenv, e);
   Int    off     = offsetQRegLane(qregNo, laneTy, laneNo);
   switch (laneTy) {
      case Ity_F64: case Ity_I64:
      case Ity_I32: case Ity_F32:
      case Ity_I16:
      case Ity_I8:
         break;
      default:
         vassert(0); // Other cases are ATC
   }
   stmt(IRStmt_Put(off, e));
}

/* Get from a specified lane of a Qreg. */
static IRExpr* getQRegLane ( UInt qregNo, UInt laneNo, IRType laneTy )
{
   Int off = offsetQRegLane(qregNo, laneTy, laneNo);
   switch (laneTy) {
      case Ity_I64: case Ity_I32: case Ity_I16: case Ity_I8:
      case Ity_F64: case Ity_F32:
         break;
      default:
         vassert(0); // Other cases are ATC
   }
   return IRExpr_Get(off, laneTy);
}


//ZZ /* ---------------- Misc registers ---------------- */
//ZZ 
//ZZ static void putMiscReg32 ( UInt    gsoffset, 
//ZZ                            IRExpr* e, /* :: Ity_I32 */
//ZZ                            IRTemp  guardT /* :: Ity_I32, 0 or 1 */)
//ZZ {
//ZZ    switch (gsoffset) {
//ZZ       case OFFB_FPSCR:   break;
//ZZ       case OFFB_QFLAG32: break;
//ZZ       case OFFB_GEFLAG0: break;
//ZZ       case OFFB_GEFLAG1: break;
//ZZ       case OFFB_GEFLAG2: break;
//ZZ       case OFFB_GEFLAG3: break;
//ZZ       default: vassert(0); /* awaiting more cases */
//ZZ    }
//ZZ    vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
//ZZ 
//ZZ    if (guardT == IRTemp_INVALID) {
//ZZ       /* unconditional write */
//ZZ       stmt(IRStmt_Put(gsoffset, e));
//ZZ    } else {
//ZZ       stmt(IRStmt_Put(
//ZZ          gsoffset,
//ZZ          IRExpr_ITE( binop(Iop_CmpNE32, mkexpr(guardT), mkU32(0)),
//ZZ                      e, IRExpr_Get(gsoffset, Ity_I32) )
//ZZ       ));
//ZZ    }
//ZZ }
//ZZ 
//ZZ static IRTemp get_ITSTATE ( void )
//ZZ {
//ZZ    ASSERT_IS_THUMB;
//ZZ    IRTemp t = newTemp(Ity_I32);
//ZZ    assign(t, IRExpr_Get( OFFB_ITSTATE, Ity_I32));
//ZZ    return t;
//ZZ }
//ZZ 
//ZZ static void put_ITSTATE ( IRTemp t )
//ZZ {
//ZZ    ASSERT_IS_THUMB;
//ZZ    stmt( IRStmt_Put( OFFB_ITSTATE, mkexpr(t)) );
//ZZ }
//ZZ 
//ZZ static IRTemp get_QFLAG32 ( void )
//ZZ {
//ZZ    IRTemp t = newTemp(Ity_I32);
//ZZ    assign(t, IRExpr_Get( OFFB_QFLAG32, Ity_I32));
//ZZ    return t;
//ZZ }
//ZZ 
//ZZ static void put_QFLAG32 ( IRTemp t, IRTemp condT )
//ZZ {
//ZZ    putMiscReg32( OFFB_QFLAG32, mkexpr(t), condT );
//ZZ }
//ZZ 
//ZZ /* Stickily set the 'Q' flag (APSR bit 27) of the APSR (Application Program
//ZZ    Status Register) to indicate that overflow or saturation occurred.
//ZZ    Nb: t must be zero to denote no saturation, and any nonzero
//ZZ    value to indicate saturation. */
//ZZ static void or_into_QFLAG32 ( IRExpr* e, IRTemp condT )
//ZZ {
//ZZ    IRTemp old = get_QFLAG32();
//ZZ    IRTemp nyu = newTemp(Ity_I32);
//ZZ    assign(nyu, binop(Iop_Or32, mkexpr(old), e) );
//ZZ    put_QFLAG32(nyu, condT);
//ZZ }


/* ---------------- FPCR stuff ---------------- */

/* Generate IR to get hold of the rounding mode bits in FPCR, and
   convert them to IR format.  Bind the final result to the
   returned temp. */
static IRTemp /* :: Ity_I32 */ mk_get_IR_rounding_mode ( void )
{
   /* The ARMvfp encoding for rounding mode bits is:
         00  to nearest
         01  to +infinity
         10  to -infinity
         11  to zero
      We need to convert that to the IR encoding:
         00  to nearest (the default)
         10  to +infinity
         01  to -infinity
         11  to zero
      Which can be done by swapping bits 0 and 1.
      The rmode bits are at 23:22 in FPSCR.
   */
   IRTemp armEncd = newTemp(Ity_I32);
   IRTemp swapped = newTemp(Ity_I32);
   /* Fish FPCR[23:22] out, and slide to bottom.  Doesn't matter that
      we don't zero out bits 24 and above, since the assignment to
      'swapped' will mask them out anyway. */
   assign(armEncd,
          binop(Iop_Shr32, IRExpr_Get(OFFB_FPCR, Ity_I32), mkU8(22)));
   /* Now swap them. */
   assign(swapped,
          binop(Iop_Or32,
                binop(Iop_And32,
                      binop(Iop_Shl32, mkexpr(armEncd), mkU8(1)),
                      mkU32(2)),
                binop(Iop_And32,
                      binop(Iop_Shr32, mkexpr(armEncd), mkU8(1)),
                      mkU32(1))
         ));
   return swapped;
}


/*------------------------------------------------------------*/
/*--- Helpers for flag handling and conditional insns      ---*/
/*------------------------------------------------------------*/

static const HChar* nameARM64Condcode ( ARM64Condcode cond )
{
   switch (cond) {
      case ARM64CondEQ:  return "eq";
      case ARM64CondNE:  return "ne";
      case ARM64CondCS:  return "cs";  // or 'hs'
      case ARM64CondCC:  return "cc";  // or 'lo'
      case ARM64CondMI:  return "mi";
      case ARM64CondPL:  return "pl";
      case ARM64CondVS:  return "vs";
      case ARM64CondVC:  return "vc";
      case ARM64CondHI:  return "hi";
      case ARM64CondLS:  return "ls";
      case ARM64CondGE:  return "ge";
      case ARM64CondLT:  return "lt";
      case ARM64CondGT:  return "gt";
      case ARM64CondLE:  return "le";
      case ARM64CondAL:  return "al";
      case ARM64CondNV:  return "nv";
      default: vpanic("name_ARM64Condcode");
   }
}

/* and a handy shorthand for it */
static const HChar* nameCC ( ARM64Condcode cond ) {
   return nameARM64Condcode(cond);
}


/* Build IR to calculate some particular condition from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression of type
   Ity_I64, suitable for narrowing.  Although the return type is
   Ity_I64, the returned value is either 0 or 1.  'cond' must be
   :: Ity_I64 and must denote the condition to compute in 
   bits 7:4, and be zero everywhere else.
*/
static IRExpr* mk_arm64g_calculate_condition_dyn ( IRExpr* cond )
{
   vassert(typeOfIRExpr(irsb->tyenv, cond) == Ity_I64);
   /* And 'cond' had better produce a value in which only bits 7:4 are
      nonzero.  However, obviously we can't assert for that. */

   /* So what we're constructing for the first argument is 
      "(cond << 4) | stored-operation".
      However, as per comments above, 'cond' must be supplied
      pre-shifted to this function.

      This pairing scheme requires that the ARM64_CC_OP_ values all fit
      in 4 bits.  Hence we are passing a (COND, OP) pair in the lowest
      8 bits of the first argument. */
   IRExpr** args
      = mkIRExprVec_4(
           binop(Iop_Or64, IRExpr_Get(OFFB_CC_OP, Ity_I64), cond),
           IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
           IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
           IRExpr_Get(OFFB_CC_NDEP, Ity_I64)
        );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/, 
           "arm64g_calculate_condition", &arm64g_calculate_condition,
           args
        );

   /* Exclude the requested condition, OP and NDEP from definedness
      checking.  We're only interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
   return call;
}


/* Build IR to calculate some particular condition from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression of type
   Ity_I64, suitable for narrowing.  Although the return type is
   Ity_I64, the returned value is either 0 or 1.
*/
static IRExpr* mk_arm64g_calculate_condition ( ARM64Condcode cond )
{
  /* First arg is "(cond << 4) | condition".  This requires that the
     ARM64_CC_OP_ values all fit in 4 bits.  Hence we are passing a
     (COND, OP) pair in the lowest 8 bits of the first argument. */
   vassert(cond >= 0 && cond <= 15);
   return mk_arm64g_calculate_condition_dyn( mkU64(cond << 4) );
}


/* Build IR to calculate just the carry flag from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression ::
   Ity_I64. */
static IRExpr* mk_arm64g_calculate_flag_c ( void )
{
   IRExpr** args
      = mkIRExprVec_4( IRExpr_Get(OFFB_CC_OP,   Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
                       IRExpr_Get(OFFB_CC_NDEP, Ity_I64) );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/,
           "arm64g_calculate_flag_c", &arm64g_calculate_flag_c,
           args
        );
   /* Exclude OP and NDEP from definedness checking.  We're only
      interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
   return call;
}


//ZZ /* Build IR to calculate just the overflow flag from stored
//ZZ    CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression ::
//ZZ    Ity_I32. */
//ZZ static IRExpr* mk_armg_calculate_flag_v ( void )
//ZZ {
//ZZ    IRExpr** args
//ZZ       = mkIRExprVec_4( IRExpr_Get(OFFB_CC_OP,   Ity_I32),
//ZZ                        IRExpr_Get(OFFB_CC_DEP1, Ity_I32),
//ZZ                        IRExpr_Get(OFFB_CC_DEP2, Ity_I32),
//ZZ                        IRExpr_Get(OFFB_CC_NDEP, Ity_I32) );
//ZZ    IRExpr* call
//ZZ       = mkIRExprCCall(
//ZZ            Ity_I32,
//ZZ            0/*regparm*/, 
//ZZ            "armg_calculate_flag_v", &armg_calculate_flag_v,
//ZZ            args
//ZZ         );
//ZZ    /* Exclude OP and NDEP from definedness checking.  We're only
//ZZ       interested in DEP1 and DEP2. */
//ZZ    call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
//ZZ    return call;
//ZZ }


/* Build IR to calculate N Z C V in bits 31:28 of the
   returned word. */
static IRExpr* mk_arm64g_calculate_flags_nzcv ( void )
{
   IRExpr** args
      = mkIRExprVec_4( IRExpr_Get(OFFB_CC_OP,   Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
                       IRExpr_Get(OFFB_CC_NDEP, Ity_I64) );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/, 
           "arm64g_calculate_flags_nzcv", &arm64g_calculate_flags_nzcv,
           args
        );
   /* Exclude OP and NDEP from definedness checking.  We're only
      interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
   return call;
}


/* Build IR to set the flags thunk, in the most general case. */
static
void setFlags_D1_D2_ND ( UInt cc_op,
                         IRTemp t_dep1, IRTemp t_dep2, IRTemp t_ndep )
{
   vassert(typeOfIRTemp(irsb->tyenv, t_dep1 == Ity_I64));
   vassert(typeOfIRTemp(irsb->tyenv, t_dep2 == Ity_I64));
   vassert(typeOfIRTemp(irsb->tyenv, t_ndep == Ity_I64));
   vassert(cc_op >= ARM64G_CC_OP_COPY && cc_op < ARM64G_CC_OP_NUMBER);
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(cc_op) ));
   stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(t_dep1) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkexpr(t_dep2) ));
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkexpr(t_ndep) ));
}

/* Build IR to set the flags thunk after ADD or SUB. */
static
void setFlags_ADD_SUB ( Bool is64, Bool isSUB, IRTemp argL, IRTemp argR )
{
   IRTemp argL64 = IRTemp_INVALID;
   IRTemp argR64 = IRTemp_INVALID;
   IRTemp z64    = newTemp(Ity_I64);
   if (is64) {
      argL64 = argL;
      argR64 = argR;
   } else {
      argL64 = newTemp(Ity_I64);
      argR64 = newTemp(Ity_I64);
      assign(argL64, unop(Iop_32Uto64, mkexpr(argL)));
      assign(argR64, unop(Iop_32Uto64, mkexpr(argR)));
   }
   assign(z64, mkU64(0));
   UInt cc_op = ARM64G_CC_OP_NUMBER;
   /**/ if ( isSUB &&  is64) { cc_op = ARM64G_CC_OP_SUB64; }
   else if ( isSUB && !is64) { cc_op = ARM64G_CC_OP_SUB32; }
   else if (!isSUB &&  is64) { cc_op = ARM64G_CC_OP_ADD64; }
   else if (!isSUB && !is64) { cc_op = ARM64G_CC_OP_ADD32; }
   else                      { vassert(0); }
   setFlags_D1_D2_ND(cc_op, argL64, argR64, z64);
}

/* Build IR to set the flags thunk after ADC or SBC. */
static
void setFlags_ADC_SBC ( Bool is64, Bool isSBC,
                        IRTemp argL, IRTemp argR, IRTemp oldC )
{
   IRTemp argL64 = IRTemp_INVALID;
   IRTemp argR64 = IRTemp_INVALID;
   IRTemp oldC64 = IRTemp_INVALID;
   if (is64) {
      argL64 = argL;
      argR64 = argR;
      oldC64 = oldC;
   } else {
      argL64 = newTemp(Ity_I64);
      argR64 = newTemp(Ity_I64);
      oldC64 = newTemp(Ity_I64);
      assign(argL64, unop(Iop_32Uto64, mkexpr(argL)));
      assign(argR64, unop(Iop_32Uto64, mkexpr(argR)));
      assign(oldC64, unop(Iop_32Uto64, mkexpr(oldC)));
   }
   UInt cc_op = ARM64G_CC_OP_NUMBER;
   /**/ if ( isSBC &&  is64) { cc_op = ARM64G_CC_OP_SBC64; }
   else if ( isSBC && !is64) { cc_op = ARM64G_CC_OP_SBC32; }
   else if (!isSBC &&  is64) { cc_op = ARM64G_CC_OP_ADC64; }
   else if (!isSBC && !is64) { cc_op = ARM64G_CC_OP_ADC32; }
   else                      { vassert(0); }
   setFlags_D1_D2_ND(cc_op, argL64, argR64, oldC64);
}

/* Build IR to set the flags thunk after ADD or SUB, if the given
   condition evaluates to True at run time.  If not, the flags are set
   to the specified NZCV value. */
static
void setFlags_ADD_SUB_conditionally (
        Bool is64, Bool isSUB,
        IRTemp cond, IRTemp argL, IRTemp argR, UInt nzcv
     )
{
   /* Generate IR as follows:
        CC_OP   = ITE(cond, OP_{ADD,SUB}{32,64}, OP_COPY)
        CC_DEP1 = ITE(cond, argL64, nzcv << 28)
        CC_DEP2 = ITE(cond, argR64, 0)
        CC_NDEP = 0
   */

   IRTemp z64 = newTemp(Ity_I64);
   assign(z64, mkU64(0));

   /* Establish the operation and operands for the True case. */
   IRTemp t_dep1 = IRTemp_INVALID;
   IRTemp t_dep2 = IRTemp_INVALID;
   UInt   t_op   = ARM64G_CC_OP_NUMBER;
   /**/ if ( isSUB &&  is64) { t_op = ARM64G_CC_OP_SUB64; }
   else if ( isSUB && !is64) { t_op = ARM64G_CC_OP_SUB32; }
   else if (!isSUB &&  is64) { t_op = ARM64G_CC_OP_ADD64; }
   else if (!isSUB && !is64) { t_op = ARM64G_CC_OP_ADD32; }
   else                      { vassert(0); }
   /* */
   if (is64) {
      t_dep1 = argL;
      t_dep2 = argR;
   } else {
      t_dep1 = newTemp(Ity_I64);
      t_dep2 = newTemp(Ity_I64);
      assign(t_dep1, unop(Iop_32Uto64, mkexpr(argL)));
      assign(t_dep2, unop(Iop_32Uto64, mkexpr(argR)));
   }

   /* Establish the operation and operands for the False case. */
   IRTemp f_dep1 = newTemp(Ity_I64);
   IRTemp f_dep2 = z64;
   UInt   f_op   = ARM64G_CC_OP_COPY;
   assign(f_dep1, mkU64(nzcv << 28));

   /* Final thunk values */
   IRTemp dep1 = newTemp(Ity_I64);
   IRTemp dep2 = newTemp(Ity_I64);
   IRTemp op   = newTemp(Ity_I64);

   assign(op,   IRExpr_ITE(mkexpr(cond), mkU64(t_op), mkU64(f_op)));
   assign(dep1, IRExpr_ITE(mkexpr(cond), mkexpr(t_dep1), mkexpr(f_dep1)));
   assign(dep2, IRExpr_ITE(mkexpr(cond), mkexpr(t_dep2), mkexpr(f_dep2)));

   /* finally .. */
   stmt( IRStmt_Put( OFFB_CC_OP,   mkexpr(op) ));
   stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(dep1) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkexpr(dep2) ));
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkexpr(z64) ));
}

/* Build IR to set the flags thunk after AND/OR/XOR or variants thereof. */
static
void setFlags_LOGIC ( Bool is64, IRTemp res )
{
   IRTemp res64 = IRTemp_INVALID;
   IRTemp z64   = newTemp(Ity_I64);
   UInt   cc_op = ARM64G_CC_OP_NUMBER;
   if (is64) {
      res64 = res;
      cc_op = ARM64G_CC_OP_LOGIC64;
   } else {
      res64 = newTemp(Ity_I64);
      assign(res64, unop(Iop_32Uto64, mkexpr(res)));
      cc_op = ARM64G_CC_OP_LOGIC32;
   }
   assign(z64, mkU64(0));
   setFlags_D1_D2_ND(cc_op, res64, z64, z64);
}

/* Build IR to set the flags thunk to a given NZCV value.  NZCV is
   located in bits 31:28 of the supplied value. */
static
void setFlags_COPY ( IRTemp nzcv_28x0 )
{
   IRTemp z64 = newTemp(Ity_I64);
   assign(z64, mkU64(0));
   setFlags_D1_D2_ND(ARM64G_CC_OP_COPY, nzcv_28x0, z64, z64);
}


//ZZ /* Minor variant of the above that sets NDEP to zero (if it
//ZZ    sets it at all) */
//ZZ static void setFlags_D1_D2 ( UInt cc_op, IRTemp t_dep1,
//ZZ                              IRTemp t_dep2,
//ZZ                              IRTemp guardT /* :: Ity_I32, 0 or 1 */ )
//ZZ {
//ZZ    IRTemp z32 = newTemp(Ity_I32);
//ZZ    assign( z32, mkU32(0) );
//ZZ    setFlags_D1_D2_ND( cc_op, t_dep1, t_dep2, z32, guardT );
//ZZ }
//ZZ 
//ZZ 
//ZZ /* Minor variant of the above that sets DEP2 to zero (if it
//ZZ    sets it at all) */
//ZZ static void setFlags_D1_ND ( UInt cc_op, IRTemp t_dep1,
//ZZ                              IRTemp t_ndep,
//ZZ                              IRTemp guardT /* :: Ity_I32, 0 or 1 */ )
//ZZ {
//ZZ    IRTemp z32 = newTemp(Ity_I32);
//ZZ    assign( z32, mkU32(0) );
//ZZ    setFlags_D1_D2_ND( cc_op, t_dep1, z32, t_ndep, guardT );
//ZZ }
//ZZ 
//ZZ 
//ZZ /* Minor variant of the above that sets DEP2 and NDEP to zero (if it
//ZZ    sets them at all) */
//ZZ static void setFlags_D1 ( UInt cc_op, IRTemp t_dep1,
//ZZ                           IRTemp guardT /* :: Ity_I32, 0 or 1 */ )
//ZZ {
//ZZ    IRTemp z32 = newTemp(Ity_I32);
//ZZ    assign( z32, mkU32(0) );
//ZZ    setFlags_D1_D2_ND( cc_op, t_dep1, z32, z32, guardT );
//ZZ }


/*------------------------------------------------------------*/
/*--- Misc math helpers                                    ---*/
/*------------------------------------------------------------*/

/* Generate IR for ((x & mask) >>u sh) | ((x << sh) & mask) */
static IRTemp math_SWAPHELPER ( IRTemp x, ULong mask, Int sh )
{
   IRTemp maskT = newTemp(Ity_I64);
   IRTemp res   = newTemp(Ity_I64);
   vassert(sh >= 1 && sh <= 63);
   assign(maskT, mkU64(mask));
   assign( res,
           binop(Iop_Or64,
                 binop(Iop_Shr64,
                       binop(Iop_And64,mkexpr(x),mkexpr(maskT)),
                       mkU8(sh)),
                 binop(Iop_And64,
                       binop(Iop_Shl64,mkexpr(x),mkU8(sh)),
                       mkexpr(maskT))
                 ) 
           );
   return res;
}

/* Generates byte swaps within 32-bit lanes. */
static IRTemp math_UINTSWAP64 ( IRTemp src )
{
   IRTemp res;
   res = math_SWAPHELPER(src, 0xFF00FF00FF00FF00ULL, 8);
   res = math_SWAPHELPER(res, 0xFFFF0000FFFF0000ULL, 16);
   return res;
}

/* Generates byte swaps within 16-bit lanes. */
static IRTemp math_USHORTSWAP64 ( IRTemp src )
{
   IRTemp res;
   res = math_SWAPHELPER(src, 0xFF00FF00FF00FF00ULL, 8);
   return res;
}

/* Generates a 64-bit byte swap. */
static IRTemp math_BYTESWAP64 ( IRTemp src )
{
   IRTemp res;
   res = math_SWAPHELPER(src, 0xFF00FF00FF00FF00ULL, 8);
   res = math_SWAPHELPER(res, 0xFFFF0000FFFF0000ULL, 16);
   res = math_SWAPHELPER(res, 0xFFFFFFFF00000000ULL, 32);
   return res;
}

/* Generates a 64-bit bit swap. */
static IRTemp math_BITSWAP64 ( IRTemp src )
{
   IRTemp res;
   res = math_SWAPHELPER(src, 0xAAAAAAAAAAAAAAAAULL, 1);
   res = math_SWAPHELPER(res, 0xCCCCCCCCCCCCCCCCULL, 2);
   res = math_SWAPHELPER(res, 0xF0F0F0F0F0F0F0F0ULL, 4);
   return math_BYTESWAP64(res);
}

/* Duplicates the bits at the bottom of the given word to fill the
   whole word.  src :: Ity_I64 is assumed to have zeroes everywhere
   except for the bottom bits. */
static IRTemp math_DUP_TO_64 ( IRTemp src, IRType srcTy )
{
   if (srcTy == Ity_I8) {
      IRTemp t16 = newTemp(Ity_I64);
      assign(t16, binop(Iop_Or64, mkexpr(src),
                                  binop(Iop_Shl64, mkexpr(src), mkU8(8))));
      IRTemp t32 = newTemp(Ity_I64);
      assign(t32, binop(Iop_Or64, mkexpr(t16),
                                  binop(Iop_Shl64, mkexpr(t16), mkU8(16))));
      IRTemp t64 = newTemp(Ity_I64);
      assign(t64, binop(Iop_Or64, mkexpr(t32),
                                  binop(Iop_Shl64, mkexpr(t32), mkU8(32))));
      return t64;
   }
   if (srcTy == Ity_I16) {
      IRTemp t32 = newTemp(Ity_I64);
      assign(t32, binop(Iop_Or64, mkexpr(src),
                                  binop(Iop_Shl64, mkexpr(src), mkU8(16))));
      IRTemp t64 = newTemp(Ity_I64);
      assign(t64, binop(Iop_Or64, mkexpr(t32),
                                  binop(Iop_Shl64, mkexpr(t32), mkU8(32))));
      return t64;
   }
   if (srcTy == Ity_I32) {
      IRTemp t64 = newTemp(Ity_I64);
      assign(t64, binop(Iop_Or64, mkexpr(src),
                                  binop(Iop_Shl64, mkexpr(src), mkU8(32))));
      return t64;
   }
   if (srcTy == Ity_I64) {
      return src;
   }
   vassert(0);
}


/* Duplicates the src element exactly so as to fill a V128 value. */
static IRTemp math_DUP_TO_V128 ( IRTemp src, IRType srcTy )
{
   IRTemp res = newTemp(Ity_V128);
   if (srcTy == Ity_F64) {
      IRTemp i64 = newTemp(Ity_I64);
      assign(i64, unop(Iop_ReinterpF64asI64, mkexpr(src)));
      assign(res, binop(Iop_64HLtoV128, mkexpr(i64), mkexpr(i64)));
      return res;
   }
   if (srcTy == Ity_F32) {
      IRTemp i64a = newTemp(Ity_I64);
      assign(i64a, unop(Iop_32Uto64, unop(Iop_ReinterpF32asI32, mkexpr(src))));
      IRTemp i64b = newTemp(Ity_I64);
      assign(i64b, binop(Iop_Or64, binop(Iop_Shl64, mkexpr(i64a), mkU8(32)),
                                   mkexpr(i64a)));
      assign(res, binop(Iop_64HLtoV128, mkexpr(i64b), mkexpr(i64b)));
      return res;
   }
   if (srcTy == Ity_I64) {
      assign(res, binop(Iop_64HLtoV128, mkexpr(src), mkexpr(src)));
      return res;
   }
   if (srcTy == Ity_I32 || srcTy == Ity_I16 || srcTy == Ity_I8) {
      IRTemp t1 = newTemp(Ity_I64);
      assign(t1, widenUto64(srcTy, mkexpr(src)));
      IRTemp t2 = math_DUP_TO_64(t1, srcTy);
      assign(res, binop(Iop_64HLtoV128, mkexpr(t2), mkexpr(t2)));
      return res;
   }
   vassert(0);
}


/*------------------------------------------------------------*/
/*--- FP comparison helpers                                ---*/
/*------------------------------------------------------------*/

/* irRes :: Ity_I32 holds a floating point comparison result encoded
   as an IRCmpF64Result.  Generate code to convert it to an
   ARM64-encoded (N,Z,C,V) group in the lowest 4 bits of an I64 value.
   Assign a new temp to hold that value, and return the temp. */
static
IRTemp mk_convert_IRCmpF64Result_to_NZCV ( IRTemp irRes32 )
{
   IRTemp ix       = newTemp(Ity_I64);
   IRTemp termL    = newTemp(Ity_I64);
   IRTemp termR    = newTemp(Ity_I64);
   IRTemp nzcv     = newTemp(Ity_I64);
   IRTemp irRes    = newTemp(Ity_I64);

   /* This is where the fun starts.  We have to convert 'irRes' from
      an IR-convention return result (IRCmpF64Result) to an
      ARM-encoded (N,Z,C,V) group.  The final result is in the bottom
      4 bits of 'nzcv'. */
   /* Map compare result from IR to ARM(nzcv) */
   /*
      FP cmp result | IR   | ARM(nzcv)
      --------------------------------
      UN              0x45   0011
      LT              0x01   1000
      GT              0x00   0010
      EQ              0x40   0110
   */
   /* Now since you're probably wondering WTF ..

      ix fishes the useful bits out of the IR value, bits 6 and 0, and
      places them side by side, giving a number which is 0, 1, 2 or 3.

      termL is a sequence cooked up by GNU superopt.  It converts ix
         into an almost correct value NZCV value (incredibly), except
         for the case of UN, where it produces 0100 instead of the
         required 0011.

      termR is therefore a correction term, also computed from ix.  It
         is 1 in the UN case and 0 for LT, GT and UN.  Hence, to get
         the final correct value, we subtract termR from termL.

      Don't take my word for it.  There's a test program at the bottom
      of guest_arm_toIR.c, to try this out with.
   */
   assign(irRes, unop(Iop_32Uto64, mkexpr(irRes32)));

   assign(
      ix,
      binop(Iop_Or64,
            binop(Iop_And64,
                  binop(Iop_Shr64, mkexpr(irRes), mkU8(5)),
                  mkU64(3)),
            binop(Iop_And64, mkexpr(irRes), mkU64(1))));

   assign(
      termL,
      binop(Iop_Add64,
            binop(Iop_Shr64,
                  binop(Iop_Sub64,
                        binop(Iop_Shl64,
                              binop(Iop_Xor64, mkexpr(ix), mkU64(1)),
                              mkU8(62)),
                        mkU64(1)),
                  mkU8(61)),
            mkU64(1)));

   assign(
      termR,
      binop(Iop_And64,
            binop(Iop_And64,
                  mkexpr(ix),
                  binop(Iop_Shr64, mkexpr(ix), mkU8(1))),
            mkU64(1)));

   assign(nzcv, binop(Iop_Sub64, mkexpr(termL), mkexpr(termR)));
   return nzcv;
}


/*------------------------------------------------------------*/
/*--- Data processing (immediate)                          ---*/
/*------------------------------------------------------------*/

/* Helper functions for supporting "DecodeBitMasks" */

static ULong dbm_ROR ( Int width, ULong x, Int rot )
{
   vassert(width > 0 && width <= 64);
   vassert(rot >= 0 && rot < width);
   if (rot == 0) return x;
   ULong res = x >> rot;
   res |= (x << (width - rot));
   if (width < 64)
     res &= ((1ULL << width) - 1);
   return res;
}

static ULong dbm_RepTo64( Int esize, ULong x )
{
   switch (esize) {
      case 64:
         return x;
      case 32:
         x &= 0xFFFFFFFF; x |= (x << 32);
         return x;
      case 16:
         x &= 0xFFFF; x |= (x << 16); x |= (x << 32);
         return x;
      case 8:
         x &= 0xFF; x |= (x << 8); x |= (x << 16); x |= (x << 32);
         return x;
      case 4:
         x &= 0xF; x |= (x << 4); x |= (x << 8);
         x |= (x << 16); x |= (x << 32);
         return x;
      case 2:
         x &= 0x3; x |= (x << 2); x |= (x << 4); x |= (x << 8);
         x |= (x << 16); x |= (x << 32);
         return x;
      default:
         break;
   }
   vpanic("dbm_RepTo64");
   /*NOTREACHED*/
   return 0;
}

static Int dbm_highestSetBit ( ULong x )
{
   Int i;
   for (i = 63; i >= 0; i--) {
      if (x & (1ULL << i))
         return i;
   }
   vassert(x == 0);
   return -1;
}

static
Bool dbm_DecodeBitMasks ( /*OUT*/ULong* wmask, /*OUT*/ULong* tmask, 
                          ULong immN, ULong imms, ULong immr, Bool immediate,
                          UInt M /*32 or 64*/)
{
   vassert(immN < (1ULL << 1));
   vassert(imms < (1ULL << 6));
   vassert(immr < (1ULL << 6));
   vassert(immediate == False || immediate == True);
   vassert(M == 32 || M == 64);

   Int len = dbm_highestSetBit( ((immN << 6) & 64) | ((~imms) & 63) );
   if (len < 1) { /* printf("fail1\n"); */ return False; }
   vassert(len <= 6);
   vassert(M >= (1 << len));

   vassert(len >= 1 && len <= 6);
   ULong levels = // (zeroes(6 - len) << (6-len)) | ones(len);
                  (1 << len) - 1;
   vassert(levels >= 1 && levels <= 63);

   if (immediate && ((imms & levels) == levels)) { 
      /* printf("fail2 imms %llu levels %llu len %d\n", imms, levels, len); */
      return False;
   }

   ULong S = imms & levels;
   ULong R = immr & levels;
   Int   diff = S - R;
   diff &= 63;
   Int esize = 1 << len;
   vassert(2 <= esize && esize <= 64);

   /* Be careful of these (1ULL << (S+1)) - 1 expressions, and the
      same below with d.  S can be 63 in which case we have an out of
      range and hence undefined shift. */
   vassert(S >= 0 && S <= 63);
   vassert(esize >= (S+1));
   ULong elem_s = // Zeroes(esize-(S+1)):Ones(S+1)
                  //(1ULL << (S+1)) - 1;
                  ((1ULL << S) - 1) + (1ULL << S);

   Int d = // diff<len-1:0>
           diff & ((1 << len)-1);
   vassert(esize >= (d+1));
   vassert(d >= 0 && d <= 63);

   ULong elem_d = // Zeroes(esize-(d+1)):Ones(d+1)
                  //(1ULL << (d+1)) - 1;
                  ((1ULL << d) - 1) + (1ULL << d);

   if (esize != 64) vassert(elem_s < (1ULL << esize));
   if (esize != 64) vassert(elem_d < (1ULL << esize));

   if (wmask) *wmask = dbm_RepTo64(esize, dbm_ROR(esize, elem_s, R));
   if (tmask) *tmask = dbm_RepTo64(esize, elem_d);

   return True;
}


static
Bool dis_ARM64_data_processing_immediate(/*MB_OUT*/DisResult* dres,
                                         UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))

   /* insn[28:23]
      10000x PC-rel addressing
      10001x Add/subtract (immediate)
      100100 Logical (immediate)
      100101 Move Wide (immediate)
      100110 Bitfield
      100111 Extract
   */

   /* ------------------ ADD/SUB{,S} imm12 ------------------ */
   if (INSN(28,24) == BITS5(1,0,0,0,1)) {
      Bool is64   = INSN(31,31) == 1;
      Bool isSub  = INSN(30,30) == 1;
      Bool setCC  = INSN(29,29) == 1;
      UInt sh     = INSN(23,22);
      UInt uimm12 = INSN(21,10);
      UInt nn     = INSN(9,5);
      UInt dd     = INSN(4,0);
      const HChar* nm = isSub ? "sub" : "add";
      if (sh >= 2) {
         /* Invalid; fall through */
      } else {
         vassert(sh <= 1);
         uimm12 <<= (12 * sh);
         if (is64) {
            IRTemp argL  = newTemp(Ity_I64);
            IRTemp argR  = newTemp(Ity_I64);
            IRTemp res   = newTemp(Ity_I64);
            assign(argL, getIReg64orSP(nn));
            assign(argR, mkU64(uimm12));
            assign(res,  binop(isSub ? Iop_Sub64 : Iop_Add64,
                               mkexpr(argL), mkexpr(argR)));
            if (setCC) {
               putIReg64orZR(dd, mkexpr(res));
               setFlags_ADD_SUB(True/*is64*/, isSub, argL, argR);
               DIP("%ss %s, %s, 0x%x\n",
                   nm, nameIReg64orZR(dd), nameIReg64orSP(nn), uimm12);
            } else {
               putIReg64orSP(dd, mkexpr(res));
               DIP("%s %s, %s, 0x%x\n",
                   nm, nameIReg64orSP(dd), nameIReg64orSP(nn), uimm12);
            }
         } else {
            IRTemp argL  = newTemp(Ity_I32);
            IRTemp argR  = newTemp(Ity_I32);
            IRTemp res   = newTemp(Ity_I32);
            assign(argL, getIReg32orSP(nn));
            assign(argR, mkU32(uimm12));
            assign(res,  binop(isSub ? Iop_Sub32 : Iop_Add32,
                               mkexpr(argL), mkexpr(argR)));
            if (setCC) {
               putIReg32orZR(dd, mkexpr(res));
               setFlags_ADD_SUB(False/*!is64*/, isSub, argL, argR);
               DIP("%ss %s, %s, 0x%x\n",
                   nm, nameIReg32orZR(dd), nameIReg32orSP(nn), uimm12);
            } else {
               putIReg32orSP(dd, mkexpr(res));
               DIP("%s %s, %s, 0x%x\n",
                   nm, nameIReg32orSP(dd), nameIReg32orSP(nn), uimm12);
            }
         }
         return True;
      }
   }

   /* -------------------- ADR/ADRP -------------------- */
   if (INSN(28,24) == BITS5(1,0,0,0,0)) {
      UInt  bP    = INSN(31,31);
      UInt  immLo = INSN(30,29);
      UInt  immHi = INSN(23,5);
      UInt  rD    = INSN(4,0);
      ULong uimm  = (immHi << 2) | immLo;
      ULong simm  = sx_to_64(uimm, 21);
      ULong val;
      if (bP) {
         val = (guest_PC_curr_instr & 0xFFFFFFFFFFFFF000ULL) + (simm << 12);
      } else {
         val = guest_PC_curr_instr + simm;
      }
      putIReg64orZR(rD, mkU64(val));
      DIP("adr%s %s, 0x%llx\n", bP ? "p" : "", nameIReg64orZR(rD), val);
      return True;
   }

   /* -------------------- LOGIC(imm) -------------------- */
   if (INSN(28,23) == BITS6(1,0,0,1,0,0)) {
      /* 31 30 28     22 21   15   9  4
         sf op 100100 N  immr imms Rn Rd
           op=00: AND  Rd|SP, Rn, #imm
           op=01: ORR  Rd|SP, Rn, #imm
           op=10: EOR  Rd|SP, Rn, #imm
           op=11: ANDS Rd|ZR, Rn, #imm
      */
      Bool  is64 = INSN(31,31) == 1;
      UInt  op   = INSN(30,29);
      UInt  N    = INSN(22,22);
      UInt  immR = INSN(21,16);
      UInt  immS = INSN(15,10);
      UInt  nn   = INSN(9,5);
      UInt  dd   = INSN(4,0);
      ULong imm  = 0;
      Bool  ok;
      if (N == 1 && !is64) 
         goto after_logic_imm; /* not allowed; fall through */
      ok = dbm_DecodeBitMasks(&imm, NULL,
                              N, immS, immR, True, is64 ? 64 : 32);
      if (!ok)
         goto after_logic_imm;

      const HChar* names[4] = { "and", "orr", "eor", "ands" };
      const IROp   ops64[4] = { Iop_And64, Iop_Or64, Iop_Xor64, Iop_And64 };
      const IROp   ops32[4] = { Iop_And32, Iop_Or32, Iop_Xor32, Iop_And32 };

      vassert(op < 4);
      if (is64) {
         IRExpr* argL = getIReg64orZR(nn);
         IRExpr* argR = mkU64(imm);
         IRTemp  res  = newTemp(Ity_I64);
         assign(res, binop(ops64[op], argL, argR));
         if (op < 3) {
            putIReg64orSP(dd, mkexpr(res));
            DIP("%s %s, %s, 0x%llx\n", names[op],
                nameIReg64orSP(dd), nameIReg64orZR(nn), imm);
         } else {
            putIReg64orZR(dd, mkexpr(res));
            setFlags_LOGIC(True/*is64*/, res);
            DIP("%s %s, %s, 0x%llx\n", names[op],
                nameIReg64orZR(dd), nameIReg64orZR(nn), imm);
         }
      } else {
         IRExpr* argL = getIReg32orZR(nn);
         IRExpr* argR = mkU32((UInt)imm);
         IRTemp  res  = newTemp(Ity_I32);
         assign(res, binop(ops32[op], argL, argR));
         if (op < 3) {
            putIReg32orSP(dd, mkexpr(res));
            DIP("%s %s, %s, 0x%x\n", names[op],
                nameIReg32orSP(dd), nameIReg32orZR(nn), (UInt)imm);
         } else {
            putIReg32orZR(dd, mkexpr(res));
            setFlags_LOGIC(False/*!is64*/, res);
            DIP("%s %s, %s, 0x%x\n", names[op],
                nameIReg32orZR(dd), nameIReg32orZR(nn), (UInt)imm);
         }
      }
      return True;
   }
   after_logic_imm:

   /* -------------------- MOV{Z,N,K} -------------------- */
   if (INSN(28,23) == BITS6(1,0,0,1,0,1)) {
      /* 31 30 28      22 20    4
         |  |  |       |  |     |
         sf 10 100 101 hw imm16 Rd   MOV(Z) Rd, (imm16 << (16*hw))
         sf 00 100 101 hw imm16 Rd   MOV(N) Rd, ~(imm16 << (16*hw))
         sf 11 100 101 hw imm16 Rd   MOV(K) Rd, (imm16 << (16*hw))
      */
      Bool is64   = INSN(31,31) == 1;
      UInt subopc = INSN(30,29);
      UInt hw     = INSN(22,21);
      UInt imm16  = INSN(20,5);
      UInt dd     = INSN(4,0);
      if (subopc == BITS2(0,1) || (!is64 && hw >= 2)) {
         /* invalid; fall through */
      } else {
         ULong imm64 = ((ULong)imm16) << (16 * hw);
         if (!is64)
            vassert(imm64 < 0x100000000ULL);
         switch (subopc) {
            case BITS2(1,0): // MOVZ
               putIRegOrZR(is64, dd, is64 ? mkU64(imm64) : mkU32((UInt)imm64));
               DIP("movz %s, 0x%llx\n", nameIRegOrZR(is64, dd), imm64);
               break;
            case BITS2(0,0): // MOVN
               imm64 = ~imm64;
               if (!is64)
                  imm64 &= 0xFFFFFFFFULL;
               putIRegOrZR(is64, dd, is64 ? mkU64(imm64) : mkU32((UInt)imm64));
               DIP("movn %s, 0x%llx\n", nameIRegOrZR(is64, dd), imm64);
               break;
            case BITS2(1,1): // MOVK
               /* This is more complex.  We are inserting a slice into
                  the destination register, so we need to have the old
                  value of it. */
               if (is64) {
                  IRTemp old = newTemp(Ity_I64);
                  assign(old, getIReg64orZR(dd));
                  ULong mask = 0xFFFFULL << (16 * hw);
                  IRExpr* res
                     = binop(Iop_Or64, 
                             binop(Iop_And64, mkexpr(old), mkU64(~mask)),
                             mkU64(imm64));
                  putIReg64orZR(dd, res);
                  DIP("movk %s, 0x%x, lsl %u\n",
                      nameIReg64orZR(dd), imm16, 16*hw);
               } else {
                  IRTemp old = newTemp(Ity_I32);
                  assign(old, getIReg32orZR(dd));
                  vassert(hw <= 1);
                  UInt mask = 0xFFFF << (16 * hw);
                  IRExpr* res
                     = binop(Iop_Or32, 
                             binop(Iop_And32, mkexpr(old), mkU32(~mask)),
                             mkU32((UInt)imm64));
                  putIReg32orZR(dd, res);
                  DIP("movk %s, 0x%x, lsl %u\n",
                      nameIReg32orZR(dd), imm16, 16*hw);
               }
               break;
            default:
               vassert(0);
         }
         return True;
      }
   }

   /* -------------------- {U,S,}BFM -------------------- */
   /*    30 28     22 21   15   9  4

      sf 10 100110 N  immr imms nn dd
         UBFM Wd, Wn, #immr, #imms   when sf=0, N=0, immr[5]=0, imms[5]=0
         UBFM Xd, Xn, #immr, #imms   when sf=1, N=1

      sf 00 100110 N  immr imms nn dd
         SBFM Wd, Wn, #immr, #imms   when sf=0, N=0, immr[5]=0, imms[5]=0
         SBFM Xd, Xn, #immr, #imms   when sf=1, N=1

      sf 01 100110 N  immr imms nn dd
         BFM Wd, Wn, #immr, #imms   when sf=0, N=0, immr[5]=0, imms[5]=0
         BFM Xd, Xn, #immr, #imms   when sf=1, N=1
   */
   if (INSN(28,23) == BITS6(1,0,0,1,1,0)) {
      UInt sf     = INSN(31,31);
      UInt opc    = INSN(30,29);
      UInt N      = INSN(22,22);
      UInt immR   = INSN(21,16);
      UInt immS   = INSN(15,10);
      UInt nn     = INSN(9,5);
      UInt dd     = INSN(4,0);
      Bool inZero = False;
      Bool extend = False;
      const HChar* nm = "???";
      /* skip invalid combinations */
      switch (opc) {
         case BITS2(0,0):
            inZero = True; extend = True; nm = "sbfm"; break;
         case BITS2(0,1):
            inZero = False; extend = False; nm = "bfm"; break;
         case BITS2(1,0):
            inZero = True; extend = False; nm = "ubfm"; break;
         case BITS2(1,1):
            goto after_bfm; /* invalid */
         default:
            vassert(0);
      }
      if (sf == 1 && N != 1) goto after_bfm;
      if (sf == 0 && (N != 0 || ((immR >> 5) & 1) != 0
                             || ((immS >> 5) & 1) != 0)) goto after_bfm;
      ULong wmask = 0, tmask = 0;
      Bool ok = dbm_DecodeBitMasks(&wmask, &tmask,
                                   N, immS, immR, False, sf == 1 ? 64 : 32);
      if (!ok) goto after_bfm; /* hmmm */

      Bool   is64 = sf == 1;
      IRType ty   = is64 ? Ity_I64 : Ity_I32;

      IRTemp dst = newTemp(ty);
      IRTemp src = newTemp(ty);
      IRTemp bot = newTemp(ty);
      IRTemp top = newTemp(ty);
      IRTemp res = newTemp(ty);
      assign(dst, inZero ? mkU(ty,0) : getIRegOrZR(is64, dd));
      assign(src, getIRegOrZR(is64, nn));
      /* perform bitfield move on low bits */
      assign(bot, binop(mkOR(ty),
                        binop(mkAND(ty), mkexpr(dst), mkU(ty, ~wmask)),
                        binop(mkAND(ty), mkexpr(mathROR(ty, src, immR)),
                                         mkU(ty, wmask))));
      /* determine extension bits (sign, zero or dest register) */
      assign(top, mkexpr(extend ? mathREPLICATE(ty, src, immS) : dst));
      /* combine extension bits and result bits */
      assign(res, binop(mkOR(ty),
                        binop(mkAND(ty), mkexpr(top), mkU(ty, ~tmask)),
                        binop(mkAND(ty), mkexpr(bot), mkU(ty, tmask))));
      putIRegOrZR(is64, dd, mkexpr(res));
      DIP("%s %s, %s, immR=%u, immS=%u\n",
          nm, nameIRegOrZR(is64, dd), nameIRegOrZR(is64, nn), immR, immS);
      return True;
   }
   after_bfm:

   /* ---------------------- EXTR ---------------------- */
   /*   30 28     22 20 15   9 4
      1 00 100111 10 m  imm6 n d  EXTR Xd, Xn, Xm, #imm6
      0 00 100111 00 m  imm6 n d  EXTR Wd, Wn, Wm, #imm6 when #imm6 < 32
   */
   if (INSN(30,23) == BITS8(0,0,1,0,0,1,1,1) && INSN(21,21) == 0) {
      Bool is64  = INSN(31,31) == 1;
      UInt mm    = INSN(20,16);
      UInt imm6  = INSN(15,10);
      UInt nn    = INSN(9,5);
      UInt dd    = INSN(4,0);
      Bool valid = True;
      if (INSN(31,31) != INSN(22,22))
        valid = False;
      if (!is64 && imm6 >= 32)
        valid = False;
      if (!valid) goto after_extr;
      IRType ty    = is64 ? Ity_I64 : Ity_I32;
      IRTemp srcHi = newTemp(ty);
      IRTemp srcLo = newTemp(ty);
      IRTemp res   = newTemp(ty);
      assign(srcHi, getIRegOrZR(is64, nn));
      assign(srcLo, getIRegOrZR(is64, mm));
      if (imm6 == 0) {
        assign(res, mkexpr(srcLo));
      } else {
        UInt szBits = 8 * sizeofIRType(ty);
        vassert(imm6 > 0 && imm6 < szBits);
        assign(res, binop(mkOR(ty),
                          binop(mkSHL(ty), mkexpr(srcHi), mkU8(szBits-imm6)),
                          binop(mkSHR(ty), mkexpr(srcLo), mkU8(imm6))));
      }
      putIRegOrZR(is64, dd, mkexpr(res));
      DIP("extr %s, %s, %s, #%u\n",
          nameIRegOrZR(is64,dd),
          nameIRegOrZR(is64,nn), nameIRegOrZR(is64,mm), imm6);
      return True;
   }
  after_extr:

   vex_printf("ARM64 front end: data_processing_immediate\n");
   return False;
#  undef INSN
}


/*------------------------------------------------------------*/
/*--- Data processing (register) instructions              ---*/
/*------------------------------------------------------------*/

static const HChar* nameSH ( UInt sh ) {
   switch (sh) {
      case 0: return "lsl";
      case 1: return "lsr";
      case 2: return "asr";
      case 3: return "ror";
      default: vassert(0);
   }
}

/* Generate IR to get a register value, possibly shifted by an
   immediate.  Returns either a 32- or 64-bit temporary holding the
   result.  After the shift, the value can optionally be NOT-ed 
   too.

   sh_how coding: 00=SHL, 01=SHR, 10=SAR, 11=ROR.  sh_amt may only be
   in the range 0 to (is64 ? 64 : 32)-1.  For some instructions, ROR
   isn't allowed, but it's the job of the caller to check that.
*/
static IRTemp getShiftedIRegOrZR ( Bool is64,
                                   UInt sh_how, UInt sh_amt, UInt regNo,
                                   Bool invert )
{
   vassert(sh_how < 4);
   vassert(sh_amt < (is64 ? 64 : 32));
   IRType ty = is64 ? Ity_I64 : Ity_I32;
   IRTemp t0 = newTemp(ty);
   assign(t0, getIRegOrZR(is64, regNo));
   IRTemp t1 = newTemp(ty);
   switch (sh_how) {
      case BITS2(0,0):
         assign(t1, binop(mkSHL(ty), mkexpr(t0), mkU8(sh_amt)));
         break;
      case BITS2(0,1):
         assign(t1, binop(mkSHR(ty), mkexpr(t0), mkU8(sh_amt)));
         break;
      case BITS2(1,0):
         assign(t1, binop(mkSAR(ty), mkexpr(t0), mkU8(sh_amt)));
         break;
      case BITS2(1,1):
         assign(t1, mkexpr(mathROR(ty, t0, sh_amt)));
         break;
      default:
         vassert(0);
   }
   if (invert) {
      IRTemp t2 = newTemp(ty);
      assign(t2, unop(mkNOT(ty), mkexpr(t1)));
      return t2;
   } else {
      return t1;
   }
}


static
Bool dis_ARM64_data_processing_register(/*MB_OUT*/DisResult* dres,
                                        UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))

   /* ------------------- ADD/SUB(reg) ------------------- */
   /* x==0 => 32 bit op      x==1 => 64 bit op
      sh: 00=LSL, 01=LSR, 10=ASR, 11=ROR(NOT ALLOWED)

      31 30 29 28    23 21 20 15   9  4
      |  |  |  |     |  |  |  |    |  |
      x  0  0  01011 sh 0  Rm imm6 Rn Rd   ADD  Rd,Rn, sh(Rm,imm6)
      x  0  1  01011 sh 0  Rm imm6 Rn Rd   ADDS Rd,Rn, sh(Rm,imm6)
      x  1  0  01011 sh 0  Rm imm6 Rn Rd   SUB  Rd,Rn, sh(Rm,imm6)
      x  1  1  01011 sh 0  Rm imm6 Rn Rd   SUBS Rd,Rn, sh(Rm,imm6)
   */
   if (INSN(28,24) == BITS5(0,1,0,1,1) && INSN(21,21) == 0) {
      UInt   bX    = INSN(31,31);
      UInt   bOP   = INSN(30,30); /* 0: ADD, 1: SUB */
      UInt   bS    = INSN(29, 29); /* set flags? */
      UInt   sh    = INSN(23,22);
      UInt   rM    = INSN(20,16);
      UInt   imm6  = INSN(15,10);
      UInt   rN    = INSN(9,5);
      UInt   rD    = INSN(4,0);
      Bool   isSUB = bOP == 1;
      Bool   is64  = bX == 1;
      IRType ty    = is64 ? Ity_I64 : Ity_I32;
      if ((!is64 && imm6 > 31) || sh == BITS2(1,1)) {
         /* invalid; fall through */
      } else {
         IRTemp argL = newTemp(ty);
         assign(argL, getIRegOrZR(is64, rN));
         IRTemp argR = getShiftedIRegOrZR(is64, sh, imm6, rM, False);
         IROp   op   = isSUB ? mkSUB(ty) : mkADD(ty);
         IRTemp res  = newTemp(ty);
         assign(res, binop(op, mkexpr(argL), mkexpr(argR)));
         if (rD != 31) putIRegOrZR(is64, rD, mkexpr(res));
         if (bS) {
            setFlags_ADD_SUB(is64, isSUB, argL, argR);
         }
         DIP("%s%s %s, %s, %s, %s #%u\n",
             bOP ? "sub" : "add", bS ? "s" : "",
             nameIRegOrZR(is64, rD), nameIRegOrZR(is64, rN),
             nameIRegOrZR(is64, rM), nameSH(sh), imm6);
         return True;
      }
   }

   /* ------------------- ADC/SBC(reg) ------------------- */
   /* x==0 => 32 bit op      x==1 => 64 bit op

      31 30 29 28    23 21 20 15     9  4
      |  |  |  |     |  |  |  |      |  |
      x  0  0  11010 00 0  Rm 000000 Rn Rd   ADC  Rd,Rn,Rm
      x  0  1  11010 00 0  Rm 000000 Rn Rd   ADCS Rd,Rn,Rm
      x  1  0  11010 00 0  Rm 000000 Rn Rd   SBC  Rd,Rn,Rm
      x  1  1  11010 00 0  Rm 000000 Rn Rd   SBCS Rd,Rn,Rm
   */

   if (INSN(28,21) == BITS8(1,1,0,1,0,0,0,0) && INSN(15,10) == 0 ) {
      UInt   bX    = INSN(31,31);
      UInt   bOP   = INSN(30,30); /* 0: ADC, 1: SBC */
      UInt   bS    = INSN(29,29); /* set flags */
      UInt   rM    = INSN(20,16);
      UInt   rN    = INSN(9,5);
      UInt   rD    = INSN(4,0);

      Bool   isSUB = bOP == 1;
      Bool   is64  = bX == 1;
      IRType ty    = is64 ? Ity_I64 : Ity_I32;

      IRTemp oldC = newTemp(ty);
      assign(oldC,
             is64 ? mk_arm64g_calculate_flag_c()
                  : unop(Iop_64to32, mk_arm64g_calculate_flag_c()) );

      IRTemp argL = newTemp(ty);
      assign(argL, getIRegOrZR(is64, rN));
      IRTemp argR = newTemp(ty);
      assign(argR, getIRegOrZR(is64, rM));

      IROp   op   = isSUB ? mkSUB(ty) : mkADD(ty);
      IRTemp res  = newTemp(ty);
      if (isSUB) {
         IRExpr* one = is64 ? mkU64(1) : mkU32(1);
         IROp xorOp = is64 ? Iop_Xor64 : Iop_Xor32;
         assign(res,
                binop(op,
                      binop(op, mkexpr(argL), mkexpr(argR)),
                      binop(xorOp, mkexpr(oldC), one)));
      } else {
         assign(res,
                binop(op,
                      binop(op, mkexpr(argL), mkexpr(argR)),
                      mkexpr(oldC)));
      }

      if (rD != 31) putIRegOrZR(is64, rD, mkexpr(res));

      if (bS) {
         setFlags_ADC_SBC(is64, isSUB, argL, argR, oldC);
      }

      DIP("%s%s %s, %s, %s\n",
          bOP ? "sbc" : "adc", bS ? "s" : "",
          nameIRegOrZR(is64, rD), nameIRegOrZR(is64, rN),
          nameIRegOrZR(is64, rM));
      return True;
   }

   /* -------------------- LOGIC(reg) -------------------- */   
   /* x==0 => 32 bit op      x==1 => 64 bit op
      N==0 => inv? is no-op (no inversion)
      N==1 => inv? is NOT
      sh: 00=LSL, 01=LSR, 10=ASR, 11=ROR

      31 30 28    23 21 20 15   9  4
      |  |  |     |  |  |  |    |  |
      x  00 01010 sh N  Rm imm6 Rn Rd  AND  Rd,Rn, inv?(sh(Rm,imm6))
      x  01 01010 sh N  Rm imm6 Rn Rd  ORR  Rd,Rn, inv?(sh(Rm,imm6))
      x  10 01010 sh N  Rm imm6 Rn Rd  EOR  Rd,Rn, inv?(sh(Rm,imm6))
      x  11 01010 sh N  Rm imm6 Rn Rd  ANDS Rd,Rn, inv?(sh(Rm,imm6))
      With N=1, the names are: BIC ORN EON BICS
   */
   if (INSN(28,24) == BITS5(0,1,0,1,0)) {
      UInt   bX   = INSN(31,31);
      UInt   sh   = INSN(23,22);
      UInt   bN   = INSN(21,21);
      UInt   rM   = INSN(20,16);
      UInt   imm6 = INSN(15,10);
      UInt   rN   = INSN(9,5);
      UInt   rD   = INSN(4,0);
      Bool   is64 = bX == 1;
      IRType ty   = is64 ? Ity_I64 : Ity_I32;
      if (!is64 && imm6 > 31) {
         /* invalid; fall though */
      } else {
         IRTemp argL = newTemp(ty);
         assign(argL, getIRegOrZR(is64, rN));
         IRTemp argR = getShiftedIRegOrZR(is64, sh, imm6, rM, bN == 1);
         IROp   op   = Iop_INVALID;
         switch (INSN(30,29)) {
            case BITS2(0,0): case BITS2(1,1): op = mkAND(ty); break;
            case BITS2(0,1):                  op = mkOR(ty);  break;
            case BITS2(1,0):                  op = mkXOR(ty); break;
            default: vassert(0);
         }
         IRTemp res = newTemp(ty);
         assign(res, binop(op, mkexpr(argL), mkexpr(argR)));
         if (INSN(30,29) == BITS2(1,1)) {
            setFlags_LOGIC(is64, res);
         }
         putIRegOrZR(is64, rD, mkexpr(res));

         static const HChar* names_op[8]
            = { "and", "orr", "eor", "ands", "bic", "orn", "eon", "bics" };
         vassert(((bN << 2) | INSN(30,29)) < 8);
         const HChar* nm_op = names_op[(bN << 2) | INSN(30,29)];
         /* Special-case the printing of "MOV" */
         if (rN == 31/*zr*/ && sh == 0/*LSL*/ && imm6 == 0 && bN == 0) {
            DIP("mov %s, %s\n", nameIRegOrZR(is64, rD),
                                nameIRegOrZR(is64, rM));
         } else {
            DIP("%s %s, %s, %s, %s #%u\n", nm_op,
                nameIRegOrZR(is64, rD), nameIRegOrZR(is64, rN),
                nameIRegOrZR(is64, rM), nameSH(sh), imm6);
         }
         return True;
      }
   }

   /* -------------------- {U,S}MULH -------------------- */   
   /* 31       23 22 20 15     9   4
      10011011 1  10 Rm 011111 Rn Rd   UMULH Xd,Xn,Xm
      10011011 0  10 Rm 011111 Rn Rd   SMULH Xd,Xn,Xm
   */
   if (INSN(31,24) == BITS8(1,0,0,1,1,0,1,1)
       && INSN(22,21) == BITS2(1,0) && INSN(15,10) == BITS6(0,1,1,1,1,1)) {
      Bool isU = INSN(23,23) == 1;
      UInt mm  = INSN(20,16);
      UInt nn  = INSN(9,5);
      UInt dd  = INSN(4,0);
      putIReg64orZR(dd, unop(Iop_128HIto64,
                             binop(isU ? Iop_MullU64 : Iop_MullS64,
                                   getIReg64orZR(nn), getIReg64orZR(mm))));
      DIP("%cmulh %s, %s, %s\n", 
          isU ? 'u' : 's',
          nameIReg64orZR(dd), nameIReg64orZR(nn), nameIReg64orZR(mm));
      return True;
   }

   /* -------------------- M{ADD,SUB} -------------------- */   
   /* 31 30           20 15 14 9 4
      sf 00 11011 000 m  0  a  n r   MADD Rd,Rn,Rm,Ra  d = a+m*n
      sf 00 11011 000 m  1  a  n r   MADD Rd,Rn,Rm,Ra  d = a-m*n
   */
   if (INSN(30,21) == BITS10(0,0,1,1,0,1,1,0,0,0)) {
      Bool is64  = INSN(31,31) == 1;
      UInt mm    = INSN(20,16);
      Bool isAdd = INSN(15,15) == 0;
      UInt aa    = INSN(14,10);
      UInt nn    = INSN(9,5);
      UInt dd    = INSN(4,0);
      if (is64) {
         putIReg64orZR(
            dd,
            binop(isAdd ? Iop_Add64 : Iop_Sub64,
                  getIReg64orZR(aa),
                  binop(Iop_Mul64, getIReg64orZR(mm), getIReg64orZR(nn))));
      } else {
         putIReg32orZR(
            dd,
            binop(isAdd ? Iop_Add32 : Iop_Sub32,
                  getIReg32orZR(aa),
                  binop(Iop_Mul32, getIReg32orZR(mm), getIReg32orZR(nn))));
      }
      DIP("%s %s, %s, %s, %s\n",
          isAdd ? "madd" : "msub",
          nameIRegOrZR(is64, dd), nameIRegOrZR(is64, nn),
          nameIRegOrZR(is64, mm), nameIRegOrZR(is64, aa));
      return True;
   }

   /* ---------------- CS{EL,INC,INV,NEG} ---------------- */   
   /* 31 30 28        20 15   11 9  4
      sf 00 1101 0100 mm cond 00 nn dd   CSEL  Rd,Rn,Rm
      sf 00 1101 0100 mm cond 01 nn dd   CSINC Rd,Rn,Rm
      sf 10 1101 0100 mm cond 00 nn dd   CSINV Rd,Rn,Rm
      sf 10 1101 0100 mm cond 01 nn dd   CSNEG Rd,Rn,Rm
      In all cases, the operation is: Rd = if cond then Rn else OP(Rm)
   */
   if (INSN(29,21) == BITS9(0, 1,1,0,1, 0,1,0,0) && INSN(11,11) == 0) {
      Bool    is64 = INSN(31,31) == 1;
      UInt    b30  = INSN(30,30);
      UInt    mm   = INSN(20,16);
      UInt    cond = INSN(15,12);
      UInt    b10  = INSN(10,10);
      UInt    nn   = INSN(9,5);
      UInt    dd   = INSN(4,0);
      UInt    op   = (b30 << 1) | b10; /* 00=id 01=inc 10=inv 11=neg */
      IRType  ty   = is64 ? Ity_I64 : Ity_I32;
      IRExpr* argL = getIRegOrZR(is64, nn);
      IRExpr* argR = getIRegOrZR(is64, mm);
      switch (op) {
         case BITS2(0,0):
            break;
         case BITS2(0,1):
            argR = binop(mkADD(ty), argR, mkU(ty,1));
            break;
         case BITS2(1,0):
            argR = unop(mkNOT(ty), argR);
            break;
         case BITS2(1,1):
            argR = binop(mkSUB(ty), mkU(ty,0), argR);
            break;
         default:
            vassert(0);
      }
      putIRegOrZR(
         is64, dd,
         IRExpr_ITE(unop(Iop_64to1, mk_arm64g_calculate_condition(cond)),
                    argL, argR)
      );
      const HChar* op_nm[4] = { "csel", "csinc", "csinv", "csneg" };
      DIP("%s %s, %s, %s, %s\n", op_nm[op],
          nameIRegOrZR(is64, dd), nameIRegOrZR(is64, nn),
          nameIRegOrZR(is64, mm), nameCC(cond));
      return True;
   }

   /* -------------- ADD/SUB(extended reg) -------------- */   
   /*     28         20 15  12   9 4
      000 01011 00 1 m  opt imm3 n d   ADD  Wd|SP, Wn|SP, Wm ext&lsld
      100 01011 00 1 m  opt imm3 n d   ADD  Xd|SP, Xn|SP, Rm ext&lsld

      001 01011 00 1 m  opt imm3 n d   ADDS Wd,    Wn|SP, Wm ext&lsld
      101 01011 00 1 m  opt imm3 n d   ADDS Xd,    Xn|SP, Rm ext&lsld

      010 01011 00 1 m  opt imm3 n d   SUB  Wd|SP, Wn|SP, Wm ext&lsld
      110 01011 00 1 m  opt imm3 n d   SUB  Xd|SP, Xn|SP, Rm ext&lsld

      011 01011 00 1 m  opt imm3 n d   SUBS Wd,    Wn|SP, Wm ext&lsld
      111 01011 00 1 m  opt imm3 n d   SUBS Xd,    Xn|SP, Rm ext&lsld

      The 'm' operand is extended per opt, thusly:

        000   Xm & 0xFF           UXTB
        001   Xm & 0xFFFF         UXTH
        010   Xm & (2^32)-1       UXTW
        011   Xm                  UXTX

        100   Xm sx from bit 7    SXTB
        101   Xm sx from bit 15   SXTH
        110   Xm sx from bit 31   SXTW
        111   Xm                  SXTX

      In the 64 bit case (bit31 == 1), UXTX and SXTX are the identity
      operation on Xm.  In the 32 bit case, UXTW, UXTX, SXTW and SXTX
      are the identity operation on Wm.

      After extension, the value is shifted left by imm3 bits, which
      may only be in the range 0 .. 4 inclusive.
   */
   if (INSN(28,21) == BITS8(0,1,0,1,1,0,0,1) && INSN(12,10) <= 4) {
      Bool is64  = INSN(31,31) == 1;
      Bool isSub = INSN(30,30) == 1;
      Bool setCC = INSN(29,29) == 1;
      UInt mm    = INSN(20,16);
      UInt opt   = INSN(15,13);
      UInt imm3  = INSN(12,10);
      UInt nn    = INSN(9,5);
      UInt dd    = INSN(4,0);
      const HChar* nameExt[8] = { "uxtb", "uxth", "uxtw", "uxtx",
                                  "sxtb", "sxth", "sxtw", "sxtx" };
      /* Do almost the same thing in the 32- and 64-bit cases. */
      IRTemp xN = newTemp(Ity_I64);
      IRTemp xM = newTemp(Ity_I64);
      assign(xN, getIReg64orSP(nn));
      assign(xM, getIReg64orZR(mm));
      IRExpr* xMw  = mkexpr(xM); /* "xM widened" */
      Int     shSX = 0;
      /* widen Xm .. */
      switch (opt) {
         case BITS3(0,0,0): // UXTB
            xMw = binop(Iop_And64, xMw, mkU64(0xFF)); break;
         case BITS3(0,0,1): // UXTH
            xMw = binop(Iop_And64, xMw, mkU64(0xFFFF)); break;
         case BITS3(0,1,0): // UXTW -- noop for the 32bit case
            if (is64) {
               xMw = unop(Iop_32Uto64, unop(Iop_64to32, xMw));
            }
            break;
         case BITS3(0,1,1): // UXTX -- always a noop
            break;
         case BITS3(1,0,0): // SXTB
            shSX = 56; goto sxTo64;
         case BITS3(1,0,1): // SXTH
            shSX = 48; goto sxTo64;
         case BITS3(1,1,0): // SXTW -- noop for the 32bit case
            if (is64) {
               shSX = 32; goto sxTo64;
            }
            break;
         case BITS3(1,1,1): // SXTX -- always a noop
            break;
         sxTo64:
            vassert(shSX >= 32);
            xMw = binop(Iop_Sar64, binop(Iop_Shl64, xMw, mkU8(shSX)),
                        mkU8(shSX));
            break;
         default:
            vassert(0);
      }
      /* and now shift */
      IRTemp argL = xN;
      IRTemp argR = newTemp(Ity_I64);
      assign(argR, binop(Iop_Shl64, xMw, mkU8(imm3)));
      IRTemp res = newTemp(Ity_I64);
      assign(res, binop(isSub ? Iop_Sub64 : Iop_Add64,
                        mkexpr(argL), mkexpr(argR)));
      if (is64) {
         if (setCC) {
            putIReg64orZR(dd, mkexpr(res));
            setFlags_ADD_SUB(True/*is64*/, isSub, argL, argR);
         } else {
            putIReg64orSP(dd, mkexpr(res));
         }
      } else {
         if (setCC) {
            IRTemp argL32 = newTemp(Ity_I32);
            IRTemp argR32 = newTemp(Ity_I32);
            putIReg32orZR(dd, unop(Iop_64to32, mkexpr(res)));
            assign(argL32, unop(Iop_64to32, mkexpr(argL)));
            assign(argR32, unop(Iop_64to32, mkexpr(argR)));
            setFlags_ADD_SUB(False/*!is64*/, isSub, argL32, argR32);
         } else {
            putIReg32orSP(dd, unop(Iop_64to32, mkexpr(res)));
         }
      }
      DIP("%s%s %s, %s, %s %s lsl %u\n",
          isSub ? "sub" : "add", setCC ? "s" : "",
          setCC ? nameIRegOrZR(is64, dd) : nameIRegOrSP(is64, dd),
          nameIRegOrSP(is64, nn), nameIRegOrSP(is64, mm),
          nameExt[opt], imm3);
      return True;
   }

   /* ---------------- CCMP/CCMN(imm) ---------------- */
   /* Bizarrely, these appear in the "data processing register"
      category, even though they are operations against an
      immediate. */
   /* 31   29        20   15   11 9    3
      sf 1 111010010 imm5 cond 10 Rn 0 nzcv   CCMP Rn, #imm5, #nzcv, cond
      sf 0 111010010 imm5 cond 10 Rn 0 nzcv   CCMN Rn, #imm5, #nzcv, cond

      Operation is:
         (CCMP) flags = if cond then flags-after-sub(Rn,imm5) else nzcv
         (CCMN) flags = if cond then flags-after-add(Rn,imm5) else nzcv
   */
   if (INSN(29,21) == BITS9(1,1,1,0,1,0,0,1,0)
       && INSN(11,10) == BITS2(1,0) && INSN(4,4) == 0) {
      Bool is64  = INSN(31,31) == 1;
      Bool isSUB = INSN(30,30) == 1;
      UInt imm5  = INSN(20,16);
      UInt cond  = INSN(15,12);
      UInt nn    = INSN(9,5);
      UInt nzcv  = INSN(3,0);

      IRTemp condT = newTemp(Ity_I1);
      assign(condT, unop(Iop_64to1, mk_arm64g_calculate_condition(cond)));

      IRType ty   = is64 ? Ity_I64 : Ity_I32;
      IRTemp argL = newTemp(ty);
      IRTemp argR = newTemp(ty);

      if (is64) {
         assign(argL, getIReg64orZR(nn));
         assign(argR, mkU64(imm5));
      } else {
         assign(argL, getIReg32orZR(nn));
         assign(argR, mkU32(imm5));
      }
      setFlags_ADD_SUB_conditionally(is64, isSUB, condT, argL, argR, nzcv);

      DIP("ccm%c %s, #%u, #%u, %s\n",
          isSUB ? 'p' : 'n', nameIRegOrZR(is64, nn),
          imm5, nzcv, nameCC(cond));
      return True;
   }

   /* ---------------- CCMP/CCMN(reg) ---------------- */
   /* 31   29        20 15   11 9    3
      sf 1 111010010 Rm cond 00 Rn 0 nzcv   CCMP Rn, Rm, #nzcv, cond
      sf 0 111010010 Rm cond 00 Rn 0 nzcv   CCMN Rn, Rm, #nzcv, cond
      Operation is:
         (CCMP) flags = if cond then flags-after-sub(Rn,Rm) else nzcv
         (CCMN) flags = if cond then flags-after-add(Rn,Rm) else nzcv
   */
   if (INSN(29,21) == BITS9(1,1,1,0,1,0,0,1,0)
       && INSN(11,10) == BITS2(0,0) && INSN(4,4) == 0) {
      Bool is64  = INSN(31,31) == 1;
      Bool isSUB = INSN(30,30) == 1;
      UInt mm    = INSN(20,16);
      UInt cond  = INSN(15,12);
      UInt nn    = INSN(9,5);
      UInt nzcv  = INSN(3,0);

      IRTemp condT = newTemp(Ity_I1);
      assign(condT, unop(Iop_64to1, mk_arm64g_calculate_condition(cond)));

      IRType ty   = is64 ? Ity_I64 : Ity_I32;
      IRTemp argL = newTemp(ty);
      IRTemp argR = newTemp(ty);

      if (is64) {
         assign(argL, getIReg64orZR(nn));
         assign(argR, getIReg64orZR(mm));
      } else {
         assign(argL, getIReg32orZR(nn));
         assign(argR, getIReg32orZR(mm));
      }
      setFlags_ADD_SUB_conditionally(is64, isSUB, condT, argL, argR, nzcv);

      DIP("ccm%c %s, %s, #%u, %s\n",
          isSUB ? 'p' : 'n', nameIRegOrZR(is64, nn),
          nameIRegOrZR(is64, mm), nzcv, nameCC(cond));
      return True;
   }


   /* -------------- REV/REV16/REV32/RBIT -------------- */   
   /* 31 30 28       20    15   11 9 4

      1  10 11010110 00000 0000 11 n d    (1) REV   Xd, Xn
      0  10 11010110 00000 0000 10 n d    (2) REV   Wd, Wn

      1  10 11010110 00000 0000 00 n d    (3) RBIT  Xd, Xn
      0  10 11010110 00000 0000 00 n d    (4) RBIT  Wd, Wn

      1  10 11010110 00000 0000 01 n d    (5) REV16 Xd, Xn
      0  10 11010110 00000 0000 01 n d    (6) REV16 Wd, Wn

      1  10 11010110 00000 0000 10 n d    (7) REV32 Xd, Xn
   */
   if (INSN(30,21) == BITS10(1,0,1,1,0,1,0,1,1,0)
       && INSN(20,12) == BITS9(0,0,0,0,0,0,0,0,0)) {
      UInt b31 = INSN(31,31);
      UInt opc = INSN(11,10);

      UInt ix = 0;
      /**/ if (b31 == 1 && opc == BITS2(1,1)) ix = 1; 
      else if (b31 == 0 && opc == BITS2(1,0)) ix = 2; 
      else if (b31 == 1 && opc == BITS2(0,0)) ix = 3; 
      else if (b31 == 0 && opc == BITS2(0,0)) ix = 4; 
      else if (b31 == 1 && opc == BITS2(0,1)) ix = 5; 
      else if (b31 == 0 && opc == BITS2(0,1)) ix = 6; 
      else if (b31 == 1 && opc == BITS2(1,0)) ix = 7; 
      if (ix >= 1 && ix <= 7) {
         Bool   is64  = ix == 1 || ix == 3 || ix == 5 || ix == 7;
         UInt   nn    = INSN(9,5);
         UInt   dd    = INSN(4,0);
         IRTemp src   = newTemp(Ity_I64);
         IRTemp dst   = IRTemp_INVALID;
         IRTemp (*math)(IRTemp) = NULL;
         switch (ix) {
            case 1: case 2: math = math_BYTESWAP64;   break;
            case 3: case 4: math = math_BITSWAP64;    break;
            case 5: case 6: math = math_USHORTSWAP64; break;
            case 7:         math = math_UINTSWAP64;   break;
            default: vassert(0);
         }
         const HChar* names[7]
           = { "rev", "rev", "rbit", "rbit", "rev16", "rev16", "rev32" };
         const HChar* nm = names[ix-1];
         vassert(math);
         if (ix == 6) {
            /* This has to be special cased, since the logic below doesn't
               handle it correctly. */
            assign(src, getIReg64orZR(nn));
            dst = math(src);
            putIReg64orZR(dd,
                          unop(Iop_32Uto64, unop(Iop_64to32, mkexpr(dst))));
         } else if (is64) {
            assign(src, getIReg64orZR(nn));
            dst = math(src);
            putIReg64orZR(dd, mkexpr(dst));
         } else {
            assign(src, binop(Iop_Shl64, getIReg64orZR(nn), mkU8(32)));
            dst = math(src);
            putIReg32orZR(dd, unop(Iop_64to32, mkexpr(dst)));
         }
         DIP("%s %s, %s\n", nm,
             nameIRegOrZR(is64,dd), nameIRegOrZR(is64,nn));
         return True;
      }
      /* else fall through */
   }

   /* -------------------- CLZ/CLS -------------------- */   
   /*    30 28   24   20    15      9 4
      sf 10 1101 0110 00000 00010 0 n d    CLZ Rd, Rn
      sf 10 1101 0110 00000 00010 1 n d    CLS Rd, Rn
   */
   if (INSN(30,21) == BITS10(1,0,1,1,0,1,0,1,1,0)
       && INSN(20,11) == BITS10(0,0,0,0,0,0,0,0,1,0)) {
      Bool   is64  = INSN(31,31) == 1;
      Bool   isCLS = INSN(10,10) == 1;
      UInt   nn    = INSN(9,5);
      UInt   dd    = INSN(4,0);
      IRTemp src   = newTemp(Ity_I64);
      IRTemp dst   = newTemp(Ity_I64);
      if (!isCLS) { // CLS not yet supported
         if (is64) {
            assign(src, getIReg64orZR(nn));
            assign(dst, IRExpr_ITE(binop(Iop_CmpEQ64, mkexpr(src), mkU64(0)),
                                   mkU64(64),
                                   unop(Iop_Clz64, mkexpr(src))));
            putIReg64orZR(dd, mkexpr(dst));
         } else {
            assign(src, binop(Iop_Shl64,
                              unop(Iop_32Uto64, getIReg32orZR(nn)), mkU8(32)));
            assign(dst, IRExpr_ITE(binop(Iop_CmpEQ64, mkexpr(src), mkU64(0)),
                                   mkU64(32),
                                   unop(Iop_Clz64, mkexpr(src))));
            putIReg32orZR(dd, unop(Iop_64to32, mkexpr(dst)));
         }
         DIP("cl%c %s, %s\n",
             isCLS ? 's' : 'z', nameIRegOrZR(is64, dd), nameIRegOrZR(is64, nn));
         return True;
      }
   }

   /* -------------------- LSLV/LSRV/ASRV -------------------- */   
   /*    30 28        20 15   11 9 4
      sf 00 1101 0110 m  0010 00 n d   LSLV Rd,Rn,Rm
      sf 00 1101 0110 m  0010 01 n d   LSRV Rd,Rn,Rm
      sf 00 1101 0110 m  0010 10 n d   ASRV Rd,Rn,Rm
   */
   if (INSN(30,21) == BITS10(0,0,1,1,0,1,0,1,1,0)
       && INSN(15,12) == BITS4(0,0,1,0) && INSN(11,10) < BITS2(1,1)) {
      Bool   is64 = INSN(31,31) == 1;
      UInt   mm   = INSN(20,16);
      UInt   op   = INSN(11,10);
      UInt   nn   = INSN(9,5);
      UInt   dd   = INSN(4,0);
      IRType ty   = is64 ? Ity_I64 : Ity_I32;
      IRTemp srcL = newTemp(ty);
      IRTemp srcR = newTemp(Ity_I8);
      IRTemp res  = newTemp(ty);
      IROp   iop  = Iop_INVALID;
      assign(srcL, getIRegOrZR(is64, nn));
      assign(srcR,
             unop(Iop_64to8,
                  binop(Iop_And64,
                        getIReg64orZR(mm), mkU64(is64 ? 63 : 31))));
      switch (op) {
         case BITS2(0,0): iop = mkSHL(ty); break;
         case BITS2(0,1): iop = mkSHR(ty); break;
         case BITS2(1,0): iop = mkSAR(ty); break;
         default: vassert(0);
      }
      assign(res, binop(iop, mkexpr(srcL), mkexpr(srcR)));
      putIRegOrZR(is64, dd, mkexpr(res));
      vassert(op < 3);
      const HChar* names[3] = { "lslv", "lsrv", "asrv" };
      DIP("%s %s, %s, %s\n",
          names[op], nameIRegOrZR(is64,dd),
                     nameIRegOrZR(is64,nn), nameIRegOrZR(is64,mm));
      return True;
   }

   /* -------------------- SDIV/UDIV -------------------- */   
   /*    30 28        20 15    10 9 4
      sf 00 1101 0110 m  00001  1 n d  SDIV Rd,Rn,Rm
      sf 00 1101 0110 m  00001  0 n d  UDIV Rd,Rn,Rm
   */
   if (INSN(30,21) == BITS10(0,0,1,1,0,1,0,1,1,0)
       && INSN(15,11) == BITS5(0,0,0,0,1)) {
      Bool is64 = INSN(31,31) == 1;
      UInt mm   = INSN(20,16);
      Bool isS  = INSN(10,10) == 1;
      UInt nn   = INSN(9,5);
      UInt dd   = INSN(4,0);
      if (isS) {
         putIRegOrZR(is64, dd, binop(is64 ? Iop_DivS64 : Iop_DivS32,
                                     getIRegOrZR(is64, nn),
                                     getIRegOrZR(is64, mm)));
      } else {
         putIRegOrZR(is64, dd, binop(is64 ? Iop_DivU64 : Iop_DivU32,
                                     getIRegOrZR(is64, nn),
                                     getIRegOrZR(is64, mm)));
      }
      DIP("%cdiv %s, %s, %s\n", isS ? 's' : 'u',
          nameIRegOrZR(is64, dd),
          nameIRegOrZR(is64, nn), nameIRegOrZR(is64, mm));
      return True;
   }

   /* ------------------ {S,U}M{ADD,SUB}L ------------------ */   
   /* 31        23  20 15 14 9 4
      1001 1011 101 m  0  a  n d   UMADDL Xd,Wn,Wm,Xa
      1001 1011 001 m  0  a  n d   SMADDL Xd,Wn,Wm,Xa
      1001 1011 101 m  1  a  n d   UMSUBL Xd,Wn,Wm,Xa
      1001 1011 001 m  1  a  n d   SMSUBL Xd,Wn,Wm,Xa
      with operation
         Xd = Xa +/- (Wn *u/s Wm)
   */
   if (INSN(31,24) == BITS8(1,0,0,1,1,0,1,1) && INSN(22,21) == BITS2(0,1)) {
      Bool   isU   = INSN(23,23) == 1;
      UInt   mm    = INSN(20,16);
      Bool   isAdd = INSN(15,15) == 0;
      UInt   aa    = INSN(14,10);
      UInt   nn    = INSN(9,5);
      UInt   dd    = INSN(4,0);
      IRTemp wN    = newTemp(Ity_I32);
      IRTemp wM    = newTemp(Ity_I32);
      IRTemp xA    = newTemp(Ity_I64);
      IRTemp muld  = newTemp(Ity_I64);
      IRTemp res   = newTemp(Ity_I64);
      assign(wN, getIReg32orZR(nn));
      assign(wM, getIReg32orZR(mm));
      assign(xA, getIReg64orZR(aa));
      assign(muld, binop(isU ? Iop_MullU32 : Iop_MullS32,
                         mkexpr(wN), mkexpr(wM)));
      assign(res, binop(isAdd ? Iop_Add64 : Iop_Sub64,
                        mkexpr(xA), mkexpr(muld)));
      putIReg64orZR(dd, mkexpr(res));
      DIP("%cm%sl %s, %s, %s, %s\n", isU ? 'u' : 's', isAdd ? "add" : "sub",
          nameIReg64orZR(dd), nameIReg32orZR(nn),
          nameIReg32orZR(mm), nameIReg64orZR(aa));
      return True;
   }
   vex_printf("ARM64 front end: data_processing_register\n");
   return False;
#  undef INSN
}


/*------------------------------------------------------------*/
/*--- Load and Store instructions                          ---*/
/*------------------------------------------------------------*/

/* Generate the EA for a "reg + reg" style amode.  This is done from
   parts of the insn, but for sanity checking sake it takes the whole
   insn.  This appears to depend on insn[15:12], with opt=insn[15:13]
   and S=insn[12]:

   The possible forms, along with their opt:S values, are:
      011:0   Xn|SP + Xm
      111:0   Xn|SP + Xm
      011:1   Xn|SP + Xm * transfer_szB
      111:1   Xn|SP + Xm * transfer_szB
      010:0   Xn|SP + 32Uto64(Wm)
      010:1   Xn|SP + 32Uto64(Wm) * transfer_szB
      110:0   Xn|SP + 32Sto64(Wm)
      110:1   Xn|SP + 32Sto64(Wm) * transfer_szB

   Rm is insn[20:16].  Rn is insn[9:5].  Rt is insn[4:0].  Log2 of
   the transfer size is insn[23,31,30].  For integer loads/stores,
   insn[23] is zero, hence szLg2 can be at most 3 in such cases.

   If the decoding fails, it returns IRTemp_INVALID.

   isInt is True iff this is decoding is for transfers to/from integer
   registers.  If False it is for transfers to/from vector registers.
*/
static IRTemp gen_indexed_EA ( /*OUT*/HChar* buf, UInt insn, Bool isInt )
{
   UInt    optS  = SLICE_UInt(insn, 15, 12);
   UInt    mm    = SLICE_UInt(insn, 20, 16);
   UInt    nn    = SLICE_UInt(insn, 9, 5);
   UInt    szLg2 = (isInt ? 0 : (SLICE_UInt(insn, 23, 23) << 2))
                   | SLICE_UInt(insn, 31, 30); // Log2 of the size

   buf[0] = 0;

   /* Sanity checks, that this really is a load/store insn. */
   if (SLICE_UInt(insn, 11, 10) != BITS2(1,0))
      goto fail;

   if (isInt
       && SLICE_UInt(insn, 29, 21) != BITS9(1,1,1,0,0,0,0,1,1)/*LDR*/
       && SLICE_UInt(insn, 29, 21) != BITS9(1,1,1,0,0,0,0,0,1)/*STR*/
       && SLICE_UInt(insn, 29, 21) != BITS9(1,1,1,0,0,0,1,0,1)/*LDRSbhw Xt*/
       && SLICE_UInt(insn, 29, 21) != BITS9(1,1,1,0,0,0,1,1,1))/*LDRSbhw Wt*/
      goto fail;

   if (!isInt
       && SLICE_UInt(insn, 29, 24) != BITS6(1,1,1,1,0,0)) /*LDR/STR*/
      goto fail;

   /* Throw out non-verified but possibly valid cases. */
   switch (szLg2) {
      case BITS3(0,0,0): break; //  8 bit, valid for both int and vec
      case BITS3(0,0,1): break; // 16 bit, valid for both int and vec
      case BITS3(0,1,0): break; // 32 bit, valid for both int and vec
      case BITS3(0,1,1): break; // 64 bit, valid for both int and vec
      case BITS3(1,0,0): // can only ever be valid for the vector case
                         if (isInt) goto fail; else goto fail;
      case BITS3(1,0,1): // these sizes are never valid
      case BITS3(1,1,0):
      case BITS3(1,1,1): goto fail;

      default: vassert(0);
   }

   IRExpr* rhs  = NULL;
   switch (optS) {
      case BITS4(1,1,1,0): goto fail; //ATC
      case BITS4(0,1,1,0):
         rhs = getIReg64orZR(mm);
         vex_sprintf(buf, "[%s, %s]",
                     nameIReg64orZR(nn), nameIReg64orZR(mm));
         break;
      case BITS4(1,1,1,1): goto fail; //ATC
      case BITS4(0,1,1,1):
         rhs = binop(Iop_Shl64, getIReg64orZR(mm), mkU8(szLg2));
         vex_sprintf(buf, "[%s, %s lsl %u]",
                     nameIReg64orZR(nn), nameIReg64orZR(mm), szLg2);
         break;
      case BITS4(0,1,0,0):
         rhs = unop(Iop_32Uto64, getIReg32orZR(mm));
         vex_sprintf(buf, "[%s, %s uxtx]",
                     nameIReg64orZR(nn), nameIReg32orZR(mm));
         break;
      case BITS4(0,1,0,1):
         rhs = binop(Iop_Shl64,
                     unop(Iop_32Uto64, getIReg32orZR(mm)), mkU8(szLg2));
         vex_sprintf(buf, "[%s, %s uxtx, lsl %u]",
                     nameIReg64orZR(nn), nameIReg32orZR(mm), szLg2);
         break;
      case BITS4(1,1,0,0):
         rhs = unop(Iop_32Sto64, getIReg32orZR(mm));
         vex_sprintf(buf, "[%s, %s sxtx]",
                     nameIReg64orZR(nn), nameIReg32orZR(mm));
         break;
      case BITS4(1,1,0,1):
         rhs = binop(Iop_Shl64,
                     unop(Iop_32Sto64, getIReg32orZR(mm)), mkU8(szLg2));
         vex_sprintf(buf, "[%s, %s sxtx, lsl %u]",
                     nameIReg64orZR(nn), nameIReg32orZR(mm), szLg2);
         break;
      default:
         /* The rest appear to be genuinely invalid */
         goto fail;
   }

   vassert(rhs);
   IRTemp res = newTemp(Ity_I64);
   assign(res, binop(Iop_Add64, getIReg64orSP(nn), rhs));
   return res;

  fail:
   vex_printf("gen_indexed_EA: unhandled case optS == 0x%x\n", optS);
   return IRTemp_INVALID;
}


/* Generate an 8/16/32/64 bit integer store to ADDR for the lowest
   bits of DATAE :: Ity_I64. */
static void gen_narrowing_store ( UInt szB, IRTemp addr, IRExpr* dataE )
{
   IRExpr* addrE = mkexpr(addr);
   switch (szB) {
      case 8:
         storeLE(addrE, dataE);
         break;
      case 4:
         storeLE(addrE, unop(Iop_64to32, dataE));
         break;
      case 2:
         storeLE(addrE, unop(Iop_64to16, dataE));
         break;
      case 1:
         storeLE(addrE, unop(Iop_64to8, dataE));
         break;
      default:
         vassert(0);
   }
}


/* Generate an 8/16/32/64 bit unsigned widening load from ADDR,
   placing the result in an Ity_I64 temporary. */
static IRTemp gen_zwidening_load ( UInt szB, IRTemp addr )
{
   IRTemp  res   = newTemp(Ity_I64);
   IRExpr* addrE = mkexpr(addr);
   switch (szB) {
      case 8:
         assign(res, loadLE(Ity_I64,addrE));
         break;
      case 4:
         assign(res, unop(Iop_32Uto64, loadLE(Ity_I32,addrE)));
         break;
      case 2:
         assign(res, unop(Iop_16Uto64, loadLE(Ity_I16,addrE)));
         break;
      case 1:
         assign(res, unop(Iop_8Uto64, loadLE(Ity_I8,addrE)));
         break;
      default:
         vassert(0);
   }
   return res;
}


/* Generate a "standard 7" name, from bitQ and size.  But also
   allow ".1d" since that's occasionally useful. */
static
const HChar* nameArr_Q_SZ ( UInt bitQ, UInt size )
{
   vassert(bitQ <= 1 && size <= 3);
   const HChar* nms[8]
      = { "8b", "4h", "2s", "1d", "16b", "8h", "4s", "2d" };
   UInt ix = (bitQ << 2) | size;
   vassert(ix < 8);
   return nms[ix];
}


static
Bool dis_ARM64_load_store(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))

   /* ------------ LDR,STR (immediate, uimm12) ----------- */
   /* uimm12 is scaled by the transfer size

      31 29  26    21    9  4
      |  |   |     |     |  |
      11 111 00100 imm12 nn tt    STR  Xt, [Xn|SP, #imm12 * 8]
      11 111 00101 imm12 nn tt    LDR  Xt, [Xn|SP, #imm12 * 8]

      10 111 00100 imm12 nn tt    STR  Wt, [Xn|SP, #imm12 * 4]
      10 111 00101 imm12 nn tt    LDR  Wt, [Xn|SP, #imm12 * 4]

      01 111 00100 imm12 nn tt    STRH Wt, [Xn|SP, #imm12 * 2]
      01 111 00101 imm12 nn tt    LDRH Wt, [Xn|SP, #imm12 * 2]

      00 111 00100 imm12 nn tt    STRB Wt, [Xn|SP, #imm12 * 1]
      00 111 00101 imm12 nn tt    LDRB Wt, [Xn|SP, #imm12 * 1]
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,1,0)) {
      UInt   szLg2 = INSN(31,30);
      UInt   szB   = 1 << szLg2;
      Bool   isLD  = INSN(22,22) == 1;
      UInt   offs  = INSN(21,10) * szB;
      UInt   nn    = INSN(9,5);
      UInt   tt    = INSN(4,0);
      IRTemp ta    = newTemp(Ity_I64);
      assign(ta, binop(Iop_Add64, getIReg64orSP(nn), mkU64(offs)));
      if (nn == 31) { /* FIXME generate stack alignment check */ }
      vassert(szLg2 < 4);
      if (isLD) {
         putIReg64orZR(tt, mkexpr(gen_zwidening_load(szB, ta)));
      } else {
         gen_narrowing_store(szB, ta, getIReg64orZR(tt));
      }
      const HChar* ld_name[4] = { "ldrb", "ldrh", "ldr", "ldr" };
      const HChar* st_name[4] = { "strb", "strh", "str", "str" };
      DIP("%s %s, [%s, #%u]\n", 
          (isLD ? ld_name : st_name)[szLg2], nameIRegOrZR(szB == 8, tt),
          nameIReg64orSP(nn), offs);
      return True;
   }

   /* ------------ LDUR,STUR (immediate, simm9) ----------- */
   /*
      31 29  26      20   11 9  4
      |  |   |       |    |  |  |
      (at-Rn-then-Rn=EA)  |  |  |
      sz 111 00000 0 imm9 01 Rn Rt   STR Rt, [Xn|SP], #simm9
      sz 111 00001 0 imm9 01 Rn Rt   LDR Rt, [Xn|SP], #simm9

      (at-EA-then-Rn=EA)
      sz 111 00000 0 imm9 11 Rn Rt   STR Rt, [Xn|SP, #simm9]!
      sz 111 00001 0 imm9 11 Rn Rt   LDR Rt, [Xn|SP, #simm9]!

      (at-EA)
      sz 111 00000 0 imm9 00 Rn Rt   STR Rt, [Xn|SP, #simm9]
      sz 111 00001 0 imm9 00 Rn Rt   LDR Rt, [Xn|SP, #simm9]

      simm9 is unscaled.

      The case 'wback && Rn == Rt && Rt != 31' is disallowed.  In the
      load case this is because would create two competing values for
      Rt.  In the store case the reason is unclear, but the spec
      disallows it anyway.

      Stores are narrowing, loads are unsigned widening.  sz encodes
      the transfer size in the normal way: 00=1, 01=2, 10=4, 11=8.
   */
   if ((INSN(29,21) & BITS9(1,1,1, 1,1,1,1,0, 1))
       == BITS9(1,1,1, 0,0,0,0,0, 0)) {
      UInt szLg2  = INSN(31,30);
      UInt szB    = 1 << szLg2;
      Bool isLoad = INSN(22,22) == 1;
      UInt imm9   = INSN(20,12);
      UInt nn     = INSN(9,5);
      UInt tt     = INSN(4,0);
      Bool wBack  = INSN(10,10) == 1;
      UInt how    = INSN(11,10);
      if (how == BITS2(1,0) || (wBack && nn == tt && tt != 31)) {
         /* undecodable; fall through */
      } else {
         if (nn == 31) { /* FIXME generate stack alignment check */ }

         // Compute the transfer address TA and the writeback address WA.
         IRTemp tRN = newTemp(Ity_I64);
         assign(tRN, getIReg64orSP(nn));
         IRTemp tEA = newTemp(Ity_I64);
         Long simm9 = (Long)sx_to_64(imm9, 9);
         assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm9)));

         IRTemp tTA = newTemp(Ity_I64);
         IRTemp tWA = newTemp(Ity_I64);
         switch (how) {
            case BITS2(0,1):
               assign(tTA, mkexpr(tRN)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(1,1):
               assign(tTA, mkexpr(tEA)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(0,0):
               assign(tTA, mkexpr(tEA)); /* tWA is unused */ break;
            default:
               vassert(0); /* NOTREACHED */
         }

         /* Normally rN would be updated after the transfer.  However, in
            the special case typifed by
               str x30, [sp,#-16]!
            it is necessary to update SP before the transfer, (1)
            because Memcheck will otherwise complain about a write
            below the stack pointer, and (2) because the segfault
            stack extension mechanism will otherwise extend the stack
            only down to SP before the instruction, which might not be
            far enough, if the -16 bit takes the actual access
            address to the next page.
         */
         Bool earlyWBack
           = wBack && simm9 < 0 && szB == 8
             && how == BITS2(1,1) && nn == 31 && !isLoad && tt != nn;

         if (wBack && earlyWBack)
            putIReg64orSP(nn, mkexpr(tEA));

         if (isLoad) {
            putIReg64orZR(tt, mkexpr(gen_zwidening_load(szB, tTA)));
         } else {
            gen_narrowing_store(szB, tTA, getIReg64orZR(tt));
         }

         if (wBack && !earlyWBack)
            putIReg64orSP(nn, mkexpr(tEA));

         const HChar* ld_name[4] = { "ldurb", "ldurh", "ldur", "ldur" };
         const HChar* st_name[4] = { "sturb", "sturh", "stur", "stur" };
         const HChar* fmt_str = NULL;
         switch (how) {
            case BITS2(0,1):
               fmt_str = "%s %s, [%s], #%lld (at-Rn-then-Rn=EA)\n";
               break;
            case BITS2(1,1):
               fmt_str = "%s %s, [%s, #%lld]! (at-EA-then-Rn=EA)\n";
               break;
            case BITS2(0,0):
               fmt_str = "%s %s, [%s, #%lld] (at-Rn)\n";
               break;
            default:
               vassert(0);
         }
         DIP(fmt_str, (isLoad ? ld_name : st_name)[szLg2],
                      nameIRegOrZR(szB == 8, tt),
                      nameIReg64orSP(nn), simm9);
         return True;
      }
   }

   /* -------- LDP,STP (immediate, simm7) (INT REGS) -------- */
   /* L==1 => mm==LD
      L==0 => mm==ST
      x==0 => 32 bit transfers, and zero extended loads
      x==1 => 64 bit transfers
      simm7 is scaled by the (single-register) transfer size

      (at-Rn-then-Rn=EA)
      x0 101 0001 L imm7 Rt2 Rn Rt1  mmP Rt1,Rt2, [Xn|SP], #imm
   
      (at-EA-then-Rn=EA)
      x0 101 0011 L imm7 Rt2 Rn Rt1  mmP Rt1,Rt2, [Xn|SP, #imm]!

      (at-EA)
      x0 101 0010 L imm7 Rt2 Rn Rt1  mmP Rt1,Rt2, [Xn|SP, #imm]
   */

   UInt insn_30_23 = INSN(30,23);
   if (insn_30_23 == BITS8(0,1,0,1,0,0,0,1) 
       || insn_30_23 == BITS8(0,1,0,1,0,0,1,1)
       || insn_30_23 == BITS8(0,1,0,1,0,0,1,0)) {
      UInt bL     = INSN(22,22);
      UInt bX     = INSN(31,31);
      UInt bWBack = INSN(23,23);
      UInt rT1    = INSN(4,0);
      UInt rN     = INSN(9,5);
      UInt rT2    = INSN(14,10);
      Long simm7  = (Long)sx_to_64(INSN(21,15), 7);
      if ((bWBack && (rT1 == rN || rT2 == rN) && rN != 31)
          || (bL && rT1 == rT2)) {
         /* undecodable; fall through */
      } else {
         if (rN == 31) { /* FIXME generate stack alignment check */ }

         // Compute the transfer address TA and the writeback address WA.
         IRTemp tRN = newTemp(Ity_I64);
         assign(tRN, getIReg64orSP(rN));
         IRTemp tEA = newTemp(Ity_I64);
         simm7 = (bX ? 8 : 4) * simm7;
         assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm7)));

         IRTemp tTA = newTemp(Ity_I64);
         IRTemp tWA = newTemp(Ity_I64);
         switch (INSN(24,23)) {
            case BITS2(0,1):
               assign(tTA, mkexpr(tRN)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(1,1):
               assign(tTA, mkexpr(tEA)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(1,0):
               assign(tTA, mkexpr(tEA)); /* tWA is unused */ break;
            default:
               vassert(0); /* NOTREACHED */
         }

         /* Normally rN would be updated after the transfer.  However, in
            the special case typifed by
               stp x29, x30, [sp,#-112]!
            it is necessary to update SP before the transfer, (1)
            because Memcheck will otherwise complain about a write
            below the stack pointer, and (2) because the segfault
            stack extension mechanism will otherwise extend the stack
            only down to SP before the instruction, which might not be
            far enough, if the -112 bit takes the actual access
            address to the next page.
         */
         Bool earlyWBack
           = bWBack && simm7 < 0
             && INSN(24,23) == BITS2(1,1) && rN == 31 && bL == 0;

         if (bWBack && earlyWBack)
            putIReg64orSP(rN, mkexpr(tEA));

         /**/ if (bL == 1 && bX == 1) {
            // 64 bit load
            putIReg64orZR(rT1, loadLE(Ity_I64,
                                      binop(Iop_Add64,mkexpr(tTA),mkU64(0))));
            putIReg64orZR(rT2, loadLE(Ity_I64, 
                                      binop(Iop_Add64,mkexpr(tTA),mkU64(8))));
         } else if (bL == 1 && bX == 0) {
            // 32 bit load
            putIReg32orZR(rT1, loadLE(Ity_I32,
                                      binop(Iop_Add64,mkexpr(tTA),mkU64(0))));
            putIReg32orZR(rT2, loadLE(Ity_I32, 
                                      binop(Iop_Add64,mkexpr(tTA),mkU64(4))));
         } else if (bL == 0 && bX == 1) {
            // 64 bit store
            storeLE(binop(Iop_Add64,mkexpr(tTA),mkU64(0)),
                    getIReg64orZR(rT1));
            storeLE(binop(Iop_Add64,mkexpr(tTA),mkU64(8)),
                    getIReg64orZR(rT2));
         } else {
            vassert(bL == 0 && bX == 0);
            // 32 bit store
            storeLE(binop(Iop_Add64,mkexpr(tTA),mkU64(0)),
                    getIReg32orZR(rT1));
            storeLE(binop(Iop_Add64,mkexpr(tTA),mkU64(4)),
                    getIReg32orZR(rT2));
         }

         if (bWBack && !earlyWBack)
            putIReg64orSP(rN, mkexpr(tEA));

         const HChar* fmt_str = NULL;
         switch (INSN(24,23)) {
            case BITS2(0,1):
               fmt_str = "%sp %s, %s, [%s], #%lld (at-Rn-then-Rn=EA)\n";
               break;
            case BITS2(1,1):
               fmt_str = "%sp %s, %s, [%s, #%lld]! (at-EA-then-Rn=EA)\n";
               break;
            case BITS2(1,0):
               fmt_str = "%sp %s, %s, [%s, #%lld] (at-Rn)\n";
               break;
            default:
               vassert(0);
         }
         DIP(fmt_str, bL == 0 ? "st" : "ld",
                      nameIRegOrZR(bX == 1, rT1),
                      nameIRegOrZR(bX == 1, rT2),
                      nameIReg64orSP(rN), simm7);
         return True;
      }
   }

   /* ---------------- LDR (literal, int reg) ---------------- */
   /* 31 29      23    4
      00 011 000 imm19 Rt   LDR   Wt, [PC + sxTo64(imm19 << 2)]
      01 011 000 imm19 Rt   LDR   Xt, [PC + sxTo64(imm19 << 2)]
      10 011 000 imm19 Rt   LDRSW Xt, [PC + sxTo64(imm19 << 2)]
      11 011 000 imm19 Rt   prefetch  [PC + sxTo64(imm19 << 2)]
      Just handles the first two cases for now.
   */
   if (INSN(29,24) == BITS6(0,1,1,0,0,0) && INSN(31,31) == 0) {
      UInt  imm19 = INSN(23,5);
      UInt  rT    = INSN(4,0);
      UInt  bX    = INSN(30,30);
      ULong ea    = guest_PC_curr_instr + sx_to_64(imm19 << 2, 21);
      if (bX) {
         putIReg64orZR(rT, loadLE(Ity_I64, mkU64(ea)));
      } else {
         putIReg32orZR(rT, loadLE(Ity_I32, mkU64(ea)));
      }
      DIP("ldr %s, 0x%llx (literal)\n", nameIRegOrZR(bX == 1, rT), ea);
      return True;
   }

   /* -------------- {LD,ST}R (integer register) --------------- */
   /* 31 29        20 15     12 11 9  4
      |  |         |  |      |  |  |  |
      11 111000011 Rm option S  10 Rn Rt  LDR  Xt, [Xn|SP, R<m>{ext/sh}]
      10 111000011 Rm option S  10 Rn Rt  LDR  Wt, [Xn|SP, R<m>{ext/sh}]
      01 111000011 Rm option S  10 Rn Rt  LDRH Wt, [Xn|SP, R<m>{ext/sh}]
      00 111000011 Rm option S  10 Rn Rt  LDRB Wt, [Xn|SP, R<m>{ext/sh}]

      11 111000001 Rm option S  10 Rn Rt  STR  Xt, [Xn|SP, R<m>{ext/sh}]
      10 111000001 Rm option S  10 Rn Rt  STR  Wt, [Xn|SP, R<m>{ext/sh}]
      01 111000001 Rm option S  10 Rn Rt  STRH Wt, [Xn|SP, R<m>{ext/sh}]
      00 111000001 Rm option S  10 Rn Rt  STRB Wt, [Xn|SP, R<m>{ext/sh}]
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,0,0)
       && INSN(21,21) == 1 && INSN(11,10) == BITS2(1,0)) {
      HChar  dis_buf[64];
      UInt   szLg2 = INSN(31,30);
      Bool   isLD  = INSN(22,22) == 1;
      UInt   tt    = INSN(4,0);
      IRTemp ea    = gen_indexed_EA(dis_buf, insn, True/*to/from int regs*/);
      if (ea != IRTemp_INVALID) {
         switch (szLg2) {
            case 3: /* 64 bit */
               if (isLD) {
                  putIReg64orZR(tt, loadLE(Ity_I64, mkexpr(ea)));
                  DIP("ldr %s, %s\n", nameIReg64orZR(tt), dis_buf);
               } else {
                  storeLE(mkexpr(ea), getIReg64orZR(tt));
                  DIP("str %s, %s\n", nameIReg64orZR(tt), dis_buf);
               }
               break;
            case 2: /* 32 bit */
               if (isLD) {
                  putIReg32orZR(tt, loadLE(Ity_I32, mkexpr(ea)));
                  DIP("ldr %s, %s\n", nameIReg32orZR(tt), dis_buf);
               } else {
                  storeLE(mkexpr(ea), getIReg32orZR(tt));
                  DIP("str %s, %s\n", nameIReg32orZR(tt), dis_buf);
               }
               break;
            case 1: /* 16 bit */
               if (isLD) {
                  putIReg64orZR(tt, unop(Iop_16Uto64,
                                         loadLE(Ity_I16, mkexpr(ea))));
                  DIP("ldruh %s, %s\n", nameIReg32orZR(tt), dis_buf);
               } else {
                  storeLE(mkexpr(ea), unop(Iop_64to16, getIReg64orZR(tt)));
                  DIP("strh %s, %s\n", nameIReg32orZR(tt), dis_buf);
               }
               break;
            case 0: /* 8 bit */
               if (isLD) {
                  putIReg64orZR(tt, unop(Iop_8Uto64,
                                         loadLE(Ity_I8, mkexpr(ea))));
                  DIP("ldrub %s, %s\n", nameIReg32orZR(tt), dis_buf);
               } else {
                  storeLE(mkexpr(ea), unop(Iop_64to8, getIReg64orZR(tt)));
                  DIP("strb %s, %s\n", nameIReg32orZR(tt), dis_buf);
               }
               break;
            default:
               vassert(0);
         }
         return True;
      }
   }

   /* -------------- LDRS{B,H,W} (uimm12) -------------- */
   /* 31 29  26  23 21    9 4
      10 111 001 10 imm12 n t   LDRSW Xt, [Xn|SP, #pimm12 * 4]
      01 111 001 1x imm12 n t   LDRSH Rt, [Xn|SP, #pimm12 * 2]
      00 111 001 1x imm12 n t   LDRSB Rt, [Xn|SP, #pimm12 * 1]
      where
         Rt is Wt when x==1, Xt when x==0
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,1,1)) {
      /* Further checks on bits 31:30 and 22 */
      Bool valid = False;
      switch ((INSN(31,30) << 1) | INSN(22,22)) {
         case BITS3(1,0,0):
         case BITS3(0,1,0): case BITS3(0,1,1):
         case BITS3(0,0,0): case BITS3(0,0,1):
            valid = True;
            break;
      }
      if (valid) {
         UInt    szLg2 = INSN(31,30);
         UInt    bitX  = INSN(22,22);
         UInt    imm12 = INSN(21,10);
         UInt    nn    = INSN(9,5);
         UInt    tt    = INSN(4,0);
         UInt    szB   = 1 << szLg2;
         IRExpr* ea    = binop(Iop_Add64,
                               getIReg64orSP(nn), mkU64(imm12 * szB));
         switch (szB) {
            case 4:
               vassert(bitX == 0);
               putIReg64orZR(tt, unop(Iop_32Sto64, loadLE(Ity_I32, ea)));
               DIP("ldrsw %s, [%s, #%u]\n", nameIReg64orZR(tt),
                   nameIReg64orSP(nn), imm12 * szB);
               break;
            case 2:
               if (bitX == 1) {
                  putIReg32orZR(tt, unop(Iop_16Sto32, loadLE(Ity_I16, ea)));
               } else {
                  putIReg64orZR(tt, unop(Iop_16Sto64, loadLE(Ity_I16, ea)));
               }
               DIP("ldrsh %s, [%s, #%u]\n",
                   nameIRegOrZR(bitX == 0, tt),
                   nameIReg64orSP(nn), imm12 * szB);
               break;
            case 1:
               if (bitX == 1) {
                  putIReg32orZR(tt, unop(Iop_8Sto32, loadLE(Ity_I8, ea)));
               } else {
                  putIReg64orZR(tt, unop(Iop_8Sto64, loadLE(Ity_I8, ea)));
               }
               DIP("ldrsb %s, [%s, #%u]\n",
                   nameIRegOrZR(bitX == 0, tt),
                   nameIReg64orSP(nn), imm12 * szB);
               break;
            default:
               vassert(0);
         }
         return True;
      }
      /* else fall through */
   }

   /* -------------- LDRS{B,H,W} (simm9, upd) -------------- */
   /* (at-Rn-then-Rn=EA)
      31 29      23 21 20   11 9 4
      00 111 000 1x 0  imm9 01 n t  LDRSB Rt, [Xn|SP], #simm9
      01 111 000 1x 0  imm9 01 n t  LDRSH Rt, [Xn|SP], #simm9
      10 111 000 10 0  imm9 01 n t  LDRSW Xt, [Xn|SP], #simm9

      (at-EA-then-Rn=EA)
      00 111 000 1x 0  imm9 11 n t  LDRSB Rt, [Xn|SP, #simm9]!
      01 111 000 1x 0  imm9 11 n t  LDRSH Rt, [Xn|SP, #simm9]!
      10 111 000 10 0  imm9 11 n t  LDRSW Xt, [Xn|SP, #simm9]!      
      where
         Rt is Wt when x==1, Xt when x==0
         transfer-at-Rn when [11]==0, at EA when [11]==1
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,0,1)
       && INSN(21,21) == 0 && INSN(10,10) == 1) {
      /* Further checks on bits 31:30 and 22 */
      Bool valid = False;
      switch ((INSN(31,30) << 1) | INSN(22,22)) {
         case BITS3(1,0,0):                    // LDRSW Xt
         case BITS3(0,1,0): case BITS3(0,1,1): // LDRSH Xt, Wt
         case BITS3(0,0,0): case BITS3(0,0,1): // LDRSB Xt, Wt
            valid = True;
            break;
      }
      if (valid) {
         UInt   szLg2 = INSN(31,30);
         UInt   imm9  = INSN(20,12);
         Bool   atRN  = INSN(11,11) == 0;
         UInt   nn    = INSN(9,5);
         UInt   tt    = INSN(4,0);
         IRTemp tRN   = newTemp(Ity_I64);
         IRTemp tEA   = newTemp(Ity_I64);
         IRTemp tTA   = IRTemp_INVALID;
         ULong  simm9 = sx_to_64(imm9, 9);
         Bool   is64  = INSN(22,22) == 0;
         assign(tRN, getIReg64orSP(nn));
         assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm9)));
         tTA = atRN ? tRN : tEA;
         HChar ch = '?';
         /* There are 5 cases: 
               byte     load,           SX to 64
               byte     load, SX to 32, ZX to 64
               halfword load,           SX to 64
               halfword load, SX to 32, ZX to 64
               word     load,           SX to 64
            The ifs below handle them in the listed order.
         */
         if (szLg2 == 0) {
            ch = 'b';
            if (is64) {
               putIReg64orZR(tt, unop(Iop_8Sto64,
                                      loadLE(Ity_I8, mkexpr(tTA))));
            } else {
               putIReg32orZR(tt, unop(Iop_8Sto32,
                                      loadLE(Ity_I8, mkexpr(tTA))));
            }
         }
         else if (szLg2 == 1) {
            ch = 'h';
            if (is64) {
               putIReg64orZR(tt, unop(Iop_16Sto64,
                                      loadLE(Ity_I16, mkexpr(tTA))));
            } else {
               putIReg32orZR(tt, unop(Iop_16Sto32,
                                      loadLE(Ity_I16, mkexpr(tTA))));
            }
         }
         else if (szLg2 == 2 && is64) {
            ch = 'w';
            putIReg64orZR(tt, unop(Iop_32Sto64,
                                   loadLE(Ity_I32, mkexpr(tTA))));
         }
         else {
            vassert(0);
         }
         putIReg64orSP(nn, mkexpr(tEA));
         DIP(atRN ? "ldrs%c %s, [%s], #%lld\n" : "ldrs%c %s, [%s, #%lld]!",
             ch, nameIRegOrZR(is64, tt), nameIReg64orSP(nn), simm9);
         return True;
      }
      /* else fall through */
   }

   /* -------------- LDRS{B,H,W} (simm9, noUpd) -------------- */
   /* 31 29      23 21 20   11 9 4
      00 111 000 1x 0  imm9 00 n t  LDURSB Rt, [Xn|SP, #simm9]
      01 111 000 1x 0  imm9 00 n t  LDURSH Rt, [Xn|SP, #simm9]
      10 111 000 10 0  imm9 00 n t  LDURSW Xt, [Xn|SP, #simm9]
      where
         Rt is Wt when x==1, Xt when x==0
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,0,1)
       && INSN(21,21) == 0 && INSN(11,10) == BITS2(0,0)) {
      /* Further checks on bits 31:30 and 22 */
      Bool valid = False;
      switch ((INSN(31,30) << 1) | INSN(22,22)) {
         case BITS3(1,0,0):                    // LDURSW Xt
         case BITS3(0,1,0): case BITS3(0,1,1): // LDURSH Xt, Wt
         case BITS3(0,0,0): case BITS3(0,0,1): // LDURSB Xt, Wt
            valid = True;
            break;
      }
      if (valid) {
         UInt   szLg2 = INSN(31,30);
         UInt   imm9  = INSN(20,12);
         UInt   nn    = INSN(9,5);
         UInt   tt    = INSN(4,0);
         IRTemp tRN   = newTemp(Ity_I64);
         IRTemp tEA   = newTemp(Ity_I64);
         ULong  simm9 = sx_to_64(imm9, 9);
         Bool   is64  = INSN(22,22) == 0;
         assign(tRN, getIReg64orSP(nn));
         assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm9)));
         HChar ch = '?';
         /* There are 5 cases: 
               byte     load,           SX to 64
               byte     load, SX to 32, ZX to 64
               halfword load,           SX to 64
               halfword load, SX to 32, ZX to 64
               word     load,           SX to 64
            The ifs below handle them in the listed order.
         */
         if (szLg2 == 0) {
            ch = 'b';
            if (is64) {
               putIReg64orZR(tt, unop(Iop_8Sto64,
                                      loadLE(Ity_I8, mkexpr(tEA))));
            } else {
               putIReg32orZR(tt, unop(Iop_8Sto32,
                                      loadLE(Ity_I8, mkexpr(tEA))));
            }
         }
         else if (szLg2 == 1) {
            ch = 'h';
            if (is64) {
               putIReg64orZR(tt, unop(Iop_16Sto64,
                                      loadLE(Ity_I16, mkexpr(tEA))));
            } else {
               putIReg32orZR(tt, unop(Iop_16Sto32,
                                      loadLE(Ity_I16, mkexpr(tEA))));
            }
         }
         else if (szLg2 == 2 && is64) {
            ch = 'w';
            putIReg64orZR(tt, unop(Iop_32Sto64,
                                   loadLE(Ity_I32, mkexpr(tEA))));
         }
         else {
            vassert(0);
         }
         DIP("ldurs%c %s, [%s, #%lld]",
             ch, nameIRegOrZR(is64, tt), nameIReg64orSP(nn), simm9);
         return True;
      }
      /* else fall through */
   }

   /* -------- LDP,STP (immediate, simm7) (FP&VEC) -------- */
   /* L==1    => mm==LD
      L==0    => mm==ST
      sz==00  => 32 bit (S) transfers
      sz==01  => 64 bit (D) transfers
      sz==10  => 128 bit (Q) transfers
      sz==11  isn't allowed
      simm7 is scaled by the (single-register) transfer size

      31 29       22 21   14 9 4
      sz 101 1001 L  imm7 t2 n t1   mmP SDQt1, SDQt2, [Xn|SP], #imm
      (at-Rn-then-Rn=EA)

      sz 101 1011 L  imm7 t2 n t1   mmP SDQt1, SDQt2, [Xn|SP, #imm]!
      (at-EA-then-Rn=EA)

      sz 101 1010 L  imm7 t2 n t1   mmP SDQt1, SDQt2, [Xn|SP, #imm]
      (at-EA)
   */

   UInt insn_29_23 = INSN(29,23);
   if (insn_29_23 == BITS7(1,0,1,1,0,0,1) 
       || insn_29_23 == BITS7(1,0,1,1,0,1,1)
       || insn_29_23 == BITS7(1,0,1,1,0,1,0)) {
      UInt szSlg2 = INSN(31,30); // log2 of the xfer size in 32-bit units
      Bool isLD   = INSN(22,22) == 1;
      Bool wBack  = INSN(23,23) == 1;
      Long simm7  = (Long)sx_to_64(INSN(21,15), 7);
      UInt tt2    = INSN(14,10);
      UInt nn     = INSN(9,5);
      UInt tt1    = INSN(4,0);
      if (szSlg2 == BITS2(1,1) || (isLD && tt1 == tt2)) {
         /* undecodable; fall through */
      } else {
         if (nn == 31) { /* FIXME generate stack alignment check */ }

         // Compute the transfer address TA and the writeback address WA.
         UInt   szB = 4 << szSlg2; /* szB is the per-register size */
         IRTemp tRN = newTemp(Ity_I64);
         assign(tRN, getIReg64orSP(nn));
         IRTemp tEA = newTemp(Ity_I64);
         simm7 = szB * simm7;
         assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm7)));

         IRTemp tTA = newTemp(Ity_I64);
         IRTemp tWA = newTemp(Ity_I64);
         switch (INSN(24,23)) {
            case BITS2(0,1):
               assign(tTA, mkexpr(tRN)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(1,1):
               assign(tTA, mkexpr(tEA)); assign(tWA, mkexpr(tEA)); break;
            case BITS2(1,0):
               assign(tTA, mkexpr(tEA)); /* tWA is unused */ break;
            default:
               vassert(0); /* NOTREACHED */
         }

         IRType ty = Ity_INVALID;
         switch (szB) {
            case 4:  ty = Ity_F32;  break;
            case 8:  ty = Ity_F64;  break;
            case 16: ty = Ity_V128; break;
            default: vassert(0);
         }

         /* Normally rN would be updated after the transfer.  However, in
            the special cases typifed by
               stp q0, q1, [sp,#-512]!
               stp d0, d1, [sp,#-512]!
               stp s0, s1, [sp,#-512]!
            it is necessary to update SP before the transfer, (1)
            because Memcheck will otherwise complain about a write
            below the stack pointer, and (2) because the segfault
            stack extension mechanism will otherwise extend the stack
            only down to SP before the instruction, which might not be
            far enough, if the -512 bit takes the actual access
            address to the next page.
         */
         Bool earlyWBack
           = wBack && simm7 < 0
             && INSN(24,23) == BITS2(1,1) && nn == 31 && !isLD;

         if (wBack && earlyWBack)
            putIReg64orSP(nn, mkexpr(tEA));

         if (isLD) {
            if (szB < 16) {
               putQReg128(tt1, mkV128(0x0000));
            }
            putQRegLO(tt1,
                      loadLE(ty, binop(Iop_Add64, mkexpr(tTA), mkU64(0))));
            if (szB < 16) {
               putQReg128(tt2, mkV128(0x0000));
            }
            putQRegLO(tt2,
                      loadLE(ty, binop(Iop_Add64, mkexpr(tTA), mkU64(szB))));
         } else {
            storeLE(binop(Iop_Add64, mkexpr(tTA), mkU64(0)),
                    getQRegLO(tt1, ty));
            storeLE(binop(Iop_Add64, mkexpr(tTA), mkU64(szB)),
                    getQRegLO(tt2, ty));
         }

         if (wBack && !earlyWBack)
            putIReg64orSP(nn, mkexpr(tEA));

         const HChar* fmt_str = NULL;
         switch (INSN(24,23)) {
            case BITS2(0,1):
               fmt_str = "%sp %s, %s, [%s], #%lld (at-Rn-then-Rn=EA)\n";
               break;
            case BITS2(1,1):
               fmt_str = "%sp %s, %s, [%s, #%lld]! (at-EA-then-Rn=EA)\n";
               break;
            case BITS2(1,0):
               fmt_str = "%sp %s, %s, [%s, #%lld] (at-Rn)\n";
               break;
            default:
               vassert(0);
         }
         DIP(fmt_str, isLD ? "ld" : "st",
                      nameQRegLO(tt1, ty), nameQRegLO(tt2, ty),
                      nameIReg64orSP(nn), simm7);
         return True;
      }
   }

   /* -------------- {LD,ST}R (vector register) --------------- */
   /* 31 29     23  20 15     12 11 9  4
      |  |      |   |  |      |  |  |  |
      00 111100 011 Rm option S  10 Rn Rt  LDR Bt, [Xn|SP, R<m>{ext/sh}]
      01 111100 011 Rm option S  10 Rn Rt  LDR Ht, [Xn|SP, R<m>{ext/sh}]
      10 111100 011 Rm option S  10 Rn Rt  LDR St, [Xn|SP, R<m>{ext/sh}]
      11 111100 011 Rm option S  10 Rn Rt  LDR Dt, [Xn|SP, R<m>{ext/sh}]
      00 111100 111 Rm option S  10 Rn Rt  LDR Qt, [Xn|SP, R<m>{ext/sh}]

      00 111100 001 Rm option S  10 Rn Rt  STR Bt, [Xn|SP, R<m>{ext/sh}]
      01 111100 001 Rm option S  10 Rn Rt  STR Ht, [Xn|SP, R<m>{ext/sh}]
      10 111100 001 Rm option S  10 Rn Rt  STR St, [Xn|SP, R<m>{ext/sh}]
      11 111100 001 Rm option S  10 Rn Rt  STR Dt, [Xn|SP, R<m>{ext/sh}]
      00 111100 101 Rm option S  10 Rn Rt  STR Qt, [Xn|SP, R<m>{ext/sh}]
   */
   if (INSN(29,24) == BITS6(1,1,1,1,0,0)
       && INSN(21,21) == 1 && INSN(11,10) == BITS2(1,0)) {
      HChar  dis_buf[64];
      UInt   szLg2 = (INSN(23,23) << 2) | INSN(31,30);
      Bool   isLD  = INSN(22,22) == 1;
      UInt   tt    = INSN(4,0);
      if (szLg2 >= 4) goto after_LDR_STR_vector_register;
      IRTemp ea    = gen_indexed_EA(dis_buf, insn, False/*to/from vec regs*/);
      if (ea == IRTemp_INVALID) goto after_LDR_STR_vector_register;
      switch (szLg2) {
         case 0: /* 8 bit */
            if (isLD) {
               putQReg128(tt, mkV128(0x0000));
               putQRegLO(tt, loadLE(Ity_I8, mkexpr(ea)));
               DIP("ldr %s, %s\n", nameQRegLO(tt, Ity_I8), dis_buf);
            } else {
               vassert(0); //ATC
               storeLE(mkexpr(ea), getQRegLO(tt, Ity_I8));
               DIP("str %s, %s\n", nameQRegLO(tt, Ity_I8), dis_buf);
            }
            break;
         case 1:
            if (isLD) {
               putQReg128(tt, mkV128(0x0000));
               putQRegLO(tt, loadLE(Ity_I16, mkexpr(ea)));
               DIP("ldr %s, %s\n", nameQRegLO(tt, Ity_I16), dis_buf);
            } else {
               vassert(0); //ATC
               storeLE(mkexpr(ea), getQRegLO(tt, Ity_I16));
               DIP("str %s, %s\n", nameQRegLO(tt, Ity_I16), dis_buf);
            }
            break;
         case 2: /* 32 bit */
            if (isLD) {
               putQReg128(tt, mkV128(0x0000));
               putQRegLO(tt, loadLE(Ity_I32, mkexpr(ea)));
               DIP("ldr %s, %s\n", nameQRegLO(tt, Ity_I32), dis_buf);
            } else {
               storeLE(mkexpr(ea), getQRegLO(tt, Ity_I32));
               DIP("str %s, %s\n", nameQRegLO(tt, Ity_I32), dis_buf);
            }
            break;
         case 3: /* 64 bit */
            if (isLD) {
               putQReg128(tt, mkV128(0x0000));
               putQRegLO(tt, loadLE(Ity_I64, mkexpr(ea)));
               DIP("ldr %s, %s\n", nameQRegLO(tt, Ity_I64), dis_buf);
            } else {
               storeLE(mkexpr(ea), getQRegLO(tt, Ity_I64));
               DIP("str %s, %s\n", nameQRegLO(tt, Ity_I64), dis_buf);
            }
            break;
         case 4:  return False; //ATC
         default: vassert(0);
      }
      return True;
   }
  after_LDR_STR_vector_register:

   /* ---------- LDRS{B,H,W} (integer register, SX) ---------- */
   /* 31 29      22 20 15  12 11 9  4
      |  |       |  |  |   |  |  |  |
      10 1110001 01 Rm opt S 10 Rn Rt    LDRSW Xt, [Xn|SP, R<m>{ext/sh}]

      01 1110001 01 Rm opt S 10 Rn Rt    LDRSH Xt, [Xn|SP, R<m>{ext/sh}]
      01 1110001 11 Rm opt S 10 Rn Rt    LDRSH Wt, [Xn|SP, R<m>{ext/sh}]

      00 1110001 01 Rm opt S 10 Rn Rt    LDRSB Xt, [Xn|SP, R<m>{ext/sh}]
      00 1110001 11 Rm opt S 10 Rn Rt    LDRSB Wt, [Xn|SP, R<m>{ext/sh}]
   */
   if (INSN(29,23) == BITS7(1,1,1,0,0,0,1)
       && INSN(21,21) == 1 && INSN(11,10) == BITS2(1,0)) {
      HChar  dis_buf[64];
      UInt   szLg2  = INSN(31,30);
      Bool   sxTo64 = INSN(22,22) == 0; // else sx to 32 and zx to 64
      UInt   tt     = INSN(4,0);
      if (szLg2 == 3) goto after_LDRS_integer_register;
      IRTemp ea     = gen_indexed_EA(dis_buf, insn, True/*to/from int regs*/);
      if (ea == IRTemp_INVALID) goto after_LDRS_integer_register;
      /* Enumerate the 5 variants explicitly. */
      if (szLg2 == 2/*32 bit*/ && sxTo64) {
         putIReg64orZR(tt, unop(Iop_32Sto64, loadLE(Ity_I32, mkexpr(ea))));
         DIP("ldrsw %s, %s\n", nameIReg64orZR(tt), dis_buf);
         return True;
      }
      else
      if (szLg2 == 1/*16 bit*/) {
         if (sxTo64) {
            putIReg64orZR(tt, unop(Iop_16Sto64, loadLE(Ity_I16, mkexpr(ea))));
            DIP("ldrsh %s, %s\n", nameIReg64orZR(tt), dis_buf);
         } else {
            putIReg32orZR(tt, unop(Iop_16Sto32, loadLE(Ity_I16, mkexpr(ea))));
            DIP("ldrsh %s, %s\n", nameIReg32orZR(tt), dis_buf);
         }
         return True;
      }
      else
      if (szLg2 == 0/*8 bit*/) {
         if (sxTo64) {
            putIReg64orZR(tt, unop(Iop_8Sto64, loadLE(Ity_I8, mkexpr(ea))));
            DIP("ldrsb %s, %s\n", nameIReg64orZR(tt), dis_buf);
         } else {
            putIReg32orZR(tt, unop(Iop_8Sto32, loadLE(Ity_I8, mkexpr(ea))));
            DIP("ldrsb %s, %s\n", nameIReg32orZR(tt), dis_buf);
         }
         return True;
      }
      /* else it's an invalid combination */
   }
  after_LDRS_integer_register:

   /* -------- LDR/STR (immediate, SIMD&FP, unsigned offset) -------- */
   /* This is the Unsigned offset variant only.  The Post-Index and
      Pre-Index variants are below.

      31 29      23 21    9 4
      00 111 101 01 imm12 n t   LDR Bt, [Xn|SP + imm12 * 1]
      01 111 101 01 imm12 n t   LDR Ht, [Xn|SP + imm12 * 2]
      10 111 101 01 imm12 n t   LDR St, [Xn|SP + imm12 * 4]
      11 111 101 01 imm12 n t   LDR Dt, [Xn|SP + imm12 * 8]
      00 111 101 11 imm12 n t   LDR Qt, [Xn|SP + imm12 * 16]

      00 111 101 00 imm12 n t   STR Bt, [Xn|SP + imm12 * 1]
      01 111 101 00 imm12 n t   STR Ht, [Xn|SP + imm12 * 2]
      10 111 101 00 imm12 n t   STR St, [Xn|SP + imm12 * 4]
      11 111 101 00 imm12 n t   STR Dt, [Xn|SP + imm12 * 8]
      00 111 101 10 imm12 n t   STR Qt, [Xn|SP + imm12 * 16]
   */
   if (INSN(29,24) == BITS6(1,1,1,1,0,1)
       && ((INSN(23,23) << 2) | INSN(31,30)) <= 4) {
      UInt   szLg2  = (INSN(23,23) << 2) | INSN(31,30);
      Bool   isLD   = INSN(22,22) == 1;
      UInt   pimm12 = INSN(21,10) << szLg2;
      UInt   nn     = INSN(9,5);
      UInt   tt     = INSN(4,0);
      IRTemp tEA    = newTemp(Ity_I64);
      IRType ty     = preferredVectorSubTypeFromSize(1 << szLg2);
      assign(tEA, binop(Iop_Add64, getIReg64orSP(nn), mkU64(pimm12)));
      if (isLD) {
         if (szLg2 < 4) {
            putQReg128(tt, mkV128(0x0000));
         }
         putQRegLO(tt, loadLE(ty, mkexpr(tEA)));
      } else {
         storeLE(mkexpr(tEA), getQRegLO(tt, ty));
      }
      DIP("%s %s, [%s, #%u]\n",
          isLD ? "ldr" : "str",
          nameQRegLO(tt, ty), nameIReg64orSP(nn), pimm12);
      return True;
   }

   /* -------- LDR/STR (immediate, SIMD&FP, pre/post index) -------- */
   /* These are the Post-Index and Pre-Index variants.

      31 29      23   20   11 9 4
      (at-Rn-then-Rn=EA)
      00 111 100 01 0 imm9 01 n t   LDR Bt, [Xn|SP], #simm
      01 111 100 01 0 imm9 01 n t   LDR Ht, [Xn|SP], #simm
      10 111 100 01 0 imm9 01 n t   LDR St, [Xn|SP], #simm
      11 111 100 01 0 imm9 01 n t   LDR Dt, [Xn|SP], #simm
      00 111 100 11 0 imm9 01 n t   LDR Qt, [Xn|SP], #simm

      (at-EA-then-Rn=EA)
      00 111 100 01 0 imm9 11 n t   LDR Bt, [Xn|SP, #simm]!
      01 111 100 01 0 imm9 11 n t   LDR Ht, [Xn|SP, #simm]!
      10 111 100 01 0 imm9 11 n t   LDR St, [Xn|SP, #simm]!
      11 111 100 01 0 imm9 11 n t   LDR Dt, [Xn|SP, #simm]!
      00 111 100 11 0 imm9 11 n t   LDR Qt, [Xn|SP, #simm]!

      Stores are the same except with bit 22 set to 0.
   */
   if (INSN(29,24) == BITS6(1,1,1,1,0,0)
       && ((INSN(23,23) << 2) | INSN(31,30)) <= 4
       && INSN(21,21) == 0 && INSN(10,10) == 1) {
      UInt   szLg2  = (INSN(23,23) << 2) | INSN(31,30);
      Bool   isLD   = INSN(22,22) == 1;
      UInt   imm9   = INSN(20,12);
      Bool   atRN   = INSN(11,11) == 0;
      UInt   nn     = INSN(9,5);
      UInt   tt     = INSN(4,0);
      IRTemp tRN    = newTemp(Ity_I64);
      IRTemp tEA    = newTemp(Ity_I64);
      IRTemp tTA    = IRTemp_INVALID;
      IRType ty     = preferredVectorSubTypeFromSize(1 << szLg2);
      ULong  simm9  = sx_to_64(imm9, 9);
      assign(tRN, getIReg64orSP(nn));
      assign(tEA, binop(Iop_Add64, mkexpr(tRN), mkU64(simm9)));
      tTA = atRN ? tRN : tEA;
      if (isLD) {
         if (szLg2 < 4) {
            putQReg128(tt, mkV128(0x0000));
         }
         putQRegLO(tt, loadLE(ty, mkexpr(tTA)));
      } else {
         storeLE(mkexpr(tTA), getQRegLO(tt, ty));
      }
      putIReg64orSP(nn, mkexpr(tEA));
      DIP(atRN ? "%s %s, [%s], #%lld\n" : "%s %s, [%s, #%lld]!\n",
          isLD ? "ldr" : "str",
          nameQRegLO(tt, ty), nameIReg64orSP(nn), simm9);
      return True;
   }

   /* -------- LDUR/STUR (unscaled offset, SIMD&FP) -------- */
   /* 31 29      23   20   11 9 4
      00 111 100 01 0 imm9 00 n t   LDR Bt, [Xn|SP, #simm]
      01 111 100 01 0 imm9 00 n t   LDR Ht, [Xn|SP, #simm]
      10 111 100 01 0 imm9 00 n t   LDR St, [Xn|SP, #simm]
      11 111 100 01 0 imm9 00 n t   LDR Dt, [Xn|SP, #simm]
      00 111 100 11 0 imm9 00 n t   LDR Qt, [Xn|SP, #simm]

      00 111 100 00 0 imm9 00 n t   STR Bt, [Xn|SP, #simm]
      01 111 100 00 0 imm9 00 n t   STR Ht, [Xn|SP, #simm]
      10 111 100 00 0 imm9 00 n t   STR St, [Xn|SP, #simm]
      11 111 100 00 0 imm9 00 n t   STR Dt, [Xn|SP, #simm]
      00 111 100 10 0 imm9 00 n t   STR Qt, [Xn|SP, #simm]
   */
   if (INSN(29,24) == BITS6(1,1,1,1,0,0)
       && ((INSN(23,23) << 2) | INSN(31,30)) <= 4
       && INSN(21,21) == 0 && INSN(11,10) == BITS2(0,0)) {
      UInt   szLg2  = (INSN(23,23) << 2) | INSN(31,30);
      Bool   isLD   = INSN(22,22) == 1;
      UInt   imm9   = INSN(20,12);
      UInt   nn     = INSN(9,5);
      UInt   tt     = INSN(4,0);
      ULong  simm9  = sx_to_64(imm9, 9);
      IRTemp tEA    = newTemp(Ity_I64);
      IRType ty     = preferredVectorSubTypeFromSize(1 << szLg2);
      assign(tEA, binop(Iop_Add64, getIReg64orSP(nn), mkU64(simm9)));
      if (isLD) {
         if (szLg2 < 4) {
            putQReg128(tt, mkV128(0x0000));
         }
         putQRegLO(tt, loadLE(ty, mkexpr(tEA)));
      } else {
         storeLE(mkexpr(tEA), getQRegLO(tt, ty));
      }
      DIP("%s %s, [%s, #%lld]\n",
          isLD ? "ldur" : "stur",
          nameQRegLO(tt, ty), nameIReg64orSP(nn), (Long)simm9);
      return True;
   }

   /* ---------------- LDR (literal, SIMD&FP) ---------------- */
   /* 31 29      23    4
      00 011 100 imm19 t    LDR St, [PC + sxTo64(imm19 << 2)]
      01 011 100 imm19 t    LDR Dt, [PC + sxTo64(imm19 << 2)]
      10 011 100 imm19 t    LDR Qt, [PC + sxTo64(imm19 << 2)]
   */
   if (INSN(29,24) == BITS6(0,1,1,1,0,0) && INSN(31,30) < BITS2(1,1)) {
      UInt   szB   = 4 << INSN(31,30);
      UInt   imm19 = INSN(23,5);
      UInt   tt    = INSN(4,0);
      ULong  ea    = guest_PC_curr_instr + sx_to_64(imm19 << 2, 21);
      IRType ty    = preferredVectorSubTypeFromSize(szB);
      putQReg128(tt, mkV128(0x0000));
      putQRegLO(tt, loadLE(ty, mkU64(ea)));
      DIP("ldr %s, 0x%llx (literal)\n", nameQRegLO(tt, ty), ea);
      return True;
   }

   /* ---------- LD1/ST1 (single structure, no offset) ---------- */
   /* 31        23
      0100 1100 0100 0000 0111 11 N T   LD1 {vT.2d},  [Xn|SP]
      0100 1100 0000 0000 0111 11 N T   ST1 {vT.2d},  [Xn|SP]
      0100 1100 0100 0000 0111 10 N T   LD1 {vT.4s},  [Xn|SP]
      0100 1100 0000 0000 0111 10 N T   ST1 {vT.4s},  [Xn|SP]
      0100 1100 0100 0000 0111 01 N T   LD1 {vT.8h},  [Xn|SP]
      0100 1100 0000 0000 0111 01 N T   ST1 {vT.8h},  [Xn|SP]
      0100 1100 0100 0000 0111 00 N T   LD1 {vT.16b}, [Xn|SP]
      0100 1100 0000 0000 0111 00 N T   ST1 {vT.16b}, [Xn|SP]
      FIXME does this assume that the host is little endian?
   */
   if (   (insn & 0xFFFFF000) == 0x4C407000 // LD1 cases
       || (insn & 0xFFFFF000) == 0x4C007000 // ST1 cases
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      const HChar* names[4] = { "2d", "4s", "8h", "16b" };
      const HChar* name = names[INSN(11,10)];
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      if (isLD) {
         putQReg128(vT, loadLE(Ity_V128, mkexpr(tEA)));
      } else {
         storeLE(mkexpr(tEA), getQReg128(vT));
      }
      DIP("%s {v%u.%s}, [%s]\n", isLD ? "ld1" : "st1",
          vT, name, nameIReg64orSP(rN));
      return True;
   }

   /* 31        23
      0000 1100 0100 0000 0111 11 N T   LD1 {vT.1d}, [Xn|SP]
      0000 1100 0000 0000 0111 11 N T   ST1 {vT.1d}, [Xn|SP]
      0000 1100 0100 0000 0111 10 N T   LD1 {vT.2s}, [Xn|SP]
      0000 1100 0000 0000 0111 10 N T   ST1 {vT.2s}, [Xn|SP]
      0000 1100 0100 0000 0111 01 N T   LD1 {vT.4h}, [Xn|SP]
      0000 1100 0000 0000 0111 01 N T   ST1 {vT.4h}, [Xn|SP]
      0000 1100 0100 0000 0111 00 N T   LD1 {vT.8b}, [Xn|SP]
      0000 1100 0000 0000 0111 00 N T   ST1 {vT.8b}, [Xn|SP]
      FIXME does this assume that the host is little endian?
   */
   if (   (insn & 0xFFFFF000) == 0x0C407000 // LD1 cases
       || (insn & 0xFFFFF000) == 0x0C007000 // ST1 cases
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      const HChar* names[4] = { "1d", "2s", "4h", "8b" };
      const HChar* name = names[INSN(11,10)];
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      if (isLD) {
         putQRegLane(vT, 0, loadLE(Ity_I64, mkexpr(tEA)));
         putQRegLane(vT, 1, mkU64(0));
      } else {
         storeLE(mkexpr(tEA), getQRegLane(vT, 0, Ity_I64));
      }
      DIP("%s {v%u.%s}, [%s]\n", isLD ? "ld1" : "st1",
          vT, name, nameIReg64orSP(rN));
      return True;
   }

   /* ---------- LD1/ST1 (single structure, post index) ---------- */
   /* 31        23
      0100 1100 1001 1111 0111 11 N T  ST1 {vT.2d},  [xN|SP], #16
      0100 1100 1101 1111 0111 11 N T  LD1 {vT.2d},  [xN|SP], #16
      0100 1100 1001 1111 0111 10 N T  ST1 {vT.4s},  [xN|SP], #16
      0100 1100 1101 1111 0111 10 N T  LD1 {vT.4s},  [xN|SP], #16
      0100 1100 1001 1111 0111 01 N T  ST1 {vT.8h},  [xN|SP], #16
      0100 1100 1101 1111 0111 01 N T  LD1 {vT.8h},  [xN|SP], #16
      0100 1100 1001 1111 0111 00 N T  ST1 {vT.16b}, [xN|SP], #16
      0100 1100 1101 1111 0111 00 N T  LD1 {vT.16b}, [xN|SP], #16
      Note that #16 is implied and cannot be any other value.
      FIXME does this assume that the host is little endian?
   */
   if (   (insn & 0xFFFFF000) == 0x4CDF7000 // LD1 cases
       || (insn & 0xFFFFF000) == 0x4C9F7000 // ST1 cases
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      const HChar* names[4] = { "2d", "4s", "8h", "16b" };
      const HChar* name = names[INSN(11,10)];
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      if (isLD) {
         putQReg128(vT, loadLE(Ity_V128, mkexpr(tEA)));
      } else {
         storeLE(mkexpr(tEA), getQReg128(vT));
      }
      putIReg64orSP(rN, binop(Iop_Add64, mkexpr(tEA), mkU64(16)));
      DIP("%s {v%u.%s}, [%s], #16\n", isLD ? "ld1" : "st1",
          vT, name, nameIReg64orSP(rN));
      return True;
   }

   /* 31        23
      0000 1100 1001 1111 0111 11 N T  ST1 {vT.1d}, [xN|SP], #8
      0000 1100 1101 1111 0111 11 N T  LD1 {vT.1d}, [xN|SP], #8
      0000 1100 1001 1111 0111 10 N T  ST1 {vT.2s}, [xN|SP], #8
      0000 1100 1101 1111 0111 10 N T  LD1 {vT.2s}, [xN|SP], #8
      0000 1100 1001 1111 0111 01 N T  ST1 {vT.4h}, [xN|SP], #8
      0000 1100 1101 1111 0111 01 N T  LD1 {vT.4h}, [xN|SP], #8
      0000 1100 1001 1111 0111 00 N T  ST1 {vT.8b}, [xN|SP], #8
      0000 1100 1101 1111 0111 00 N T  LD1 {vT.8b}, [xN|SP], #8
      Note that #8 is implied and cannot be any other value.
      FIXME does this assume that the host is little endian?
   */
   if (   (insn & 0xFFFFF000) == 0x0CDF7000 // LD1 cases
       || (insn & 0xFFFFF000) == 0x0C9F7000 // ST1 cases
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN  = INSN(9,5);
      UInt   vT  = INSN(4,0);
      IRTemp tEA = newTemp(Ity_I64);
      const HChar* names[4] = { "1d", "2s", "4h", "8b" };
      const HChar* name = names[INSN(11,10)];
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      if (isLD) {
         putQRegLane(vT, 0, loadLE(Ity_I64, mkexpr(tEA)));
         putQRegLane(vT, 1, mkU64(0));
      } else {
         storeLE(mkexpr(tEA), getQRegLane(vT, 0, Ity_I64));
      }
      putIReg64orSP(rN, binop(Iop_Add64, mkexpr(tEA), mkU64(8)));
      DIP("%s {v%u.%s}, [%s], #8\n",  isLD ? "ld1" : "st1",
          vT, name, nameIReg64orSP(rN));
      return True;
   }

   /* ---------- LD1R (single structure, replicate) ---------- */
   /* 31 29       22 20    15    11 9 4    
      0q 001 1010 10 00000 110 0 sz n t  LD1R Vt.T, [Xn|SP]
      0q 001 1011 10 m     110 0 sz n t  LD1R Vt.T, [Xn|SP], #sz (m=11111)
                                                           , Xm  (m!=11111)
   */
   if (INSN(31,31) == 0 && INSN(29,24) == BITS6(0,0,1,1,0,1)
       && INSN(22,21) == BITS2(1,0) && INSN(15,12) == BITS4(1,1,0,0)) {
      Bool   isQ  = INSN(30,30) == 1;
      Bool   isPX = INSN(23,23) == 1;
      UInt   mm   = INSN(20,16);
      UInt   sz   = INSN(11,10);
      UInt   nn   = INSN(9,5);
      UInt   tt   = INSN(4,0);
      IRType ty   = integerIRTypeOfSize(1 << sz);
      IRTemp tEA  = newTemp(Ity_I64);
      assign(tEA, getIReg64orSP(nn));
      if (nn == 31) { /* FIXME generate stack alignment check */ }
      IRTemp loaded = newTemp(ty);
      assign(loaded, loadLE(ty, mkexpr(tEA)));
      IRTemp dupd = math_DUP_TO_V128(loaded, ty);
      putQReg128(tt, isQ ? mkexpr(dupd)
                         : unop(Iop_ZeroHI64ofV128, mkexpr(dupd)));
      const HChar* arr = nameArr_Q_SZ(isQ ? 1 : 0, sz);
      /* Deal with the writeback, if any. */
      if (!isPX && mm == BITS5(0,0,0,0,0)) {
         /* No writeback. */
         DIP("ld1r v%u.%s, [%s]\n", tt, arr, nameIReg64orSP(nn));
         return True;
      }
      if (isPX) {
         putIReg64orSP(nn, binop(Iop_Add64, mkexpr(tEA), 
                                 mm == BITS5(1,1,1,1,1) ? mkU64(1 << sz)
                                                        : getIReg64orZR(mm)));
         if (mm == BITS5(1,1,1,1,1)) {
            DIP("ld1r v%u.%s, [%s], %s\n", tt, arr,
                nameIReg64orSP(nn), nameIReg64orZR(mm));
         } else {
            DIP("ld1r v%u.%s, [%s], #%u\n", tt, arr,
                nameIReg64orSP(nn), 1 << sz);
         }
         return True;
      }
      return False;
   }

   /* -------- LD2/ST2 (multi 2-elem structs, 2 regs, post index) -------- */
   /* Only a very few cases. */
   /* 31        23             11 9 4
      0100 1100 1101 1111 1000 11 n t  LD2 {Vt.2d, V(t+1)%32.2d}, [Xn|SP], #32
      0100 1100 1001 1111 1000 11 n t  ST2 {Vt.2d, V(t+1)%32.2d}, [Xn|SP], #32
      0100 1100 1101 1111 1000 10 n t  LD2 {Vt.4s, V(t+1)%32.4s}, [Xn|SP], #32
      0100 1100 1001 1111 1000 10 n t  ST2 {Vt.4s, V(t+1)%32.4s}, [Xn|SP], #32
   */
   if (   (insn & 0xFFFFFC00) == 0x4CDF8C00 // LD2 .2d
       || (insn & 0xFFFFFC00) == 0x4C9F8C00 // ST2 .2d
       || (insn & 0xFFFFFC00) == 0x4CDF8800 // LD2 .4s
       || (insn & 0xFFFFFC00) == 0x4C9F8800 // ST2 .4s
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      UInt   sz   = INSN(11,10);
      const HChar* name = "??";
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      IRExpr* tEA_0  = binop(Iop_Add64, mkexpr(tEA), mkU64(0));
      IRExpr* tEA_8  = binop(Iop_Add64, mkexpr(tEA), mkU64(8));
      IRExpr* tEA_16 = binop(Iop_Add64, mkexpr(tEA), mkU64(16));
      IRExpr* tEA_24 = binop(Iop_Add64, mkexpr(tEA), mkU64(24));
      if (sz == BITS2(1,1)) {
         name = "2d";
         if (isLD) {
            putQRegLane((vT+0) % 32, 0, loadLE(Ity_I64, tEA_0));
            putQRegLane((vT+0) % 32, 1, loadLE(Ity_I64, tEA_16));
            putQRegLane((vT+1) % 32, 0, loadLE(Ity_I64, tEA_8));
            putQRegLane((vT+1) % 32, 1, loadLE(Ity_I64, tEA_24));
         } else {
            storeLE(tEA_0,  getQRegLane((vT+0) % 32, 0, Ity_I64));
            storeLE(tEA_16, getQRegLane((vT+0) % 32, 1, Ity_I64));
            storeLE(tEA_8,  getQRegLane((vT+1) % 32, 0, Ity_I64));
            storeLE(tEA_24, getQRegLane((vT+1) % 32, 1, Ity_I64));
         }
      }
      else if (sz == BITS2(1,0)) {
         /* Uh, this is ugly.  TODO: better. */
         name = "4s";
         IRExpr* tEA_4  = binop(Iop_Add64, mkexpr(tEA), mkU64(4));
         IRExpr* tEA_12 = binop(Iop_Add64, mkexpr(tEA), mkU64(12));
         IRExpr* tEA_20 = binop(Iop_Add64, mkexpr(tEA), mkU64(20));
         IRExpr* tEA_28 = binop(Iop_Add64, mkexpr(tEA), mkU64(28));
         if (isLD) {
            putQRegLane((vT+0) % 32, 0, loadLE(Ity_I32, tEA_0));
            putQRegLane((vT+0) % 32, 1, loadLE(Ity_I32, tEA_8));
            putQRegLane((vT+0) % 32, 2, loadLE(Ity_I32, tEA_16));
            putQRegLane((vT+0) % 32, 3, loadLE(Ity_I32, tEA_24));
            putQRegLane((vT+1) % 32, 0, loadLE(Ity_I32, tEA_4));
            putQRegLane((vT+1) % 32, 1, loadLE(Ity_I32, tEA_12));
            putQRegLane((vT+1) % 32, 2, loadLE(Ity_I32, tEA_20));
            putQRegLane((vT+1) % 32, 3, loadLE(Ity_I32, tEA_28));
         } else {
            storeLE(tEA_0,  getQRegLane((vT+0) % 32, 0, Ity_I32));
            storeLE(tEA_8,  getQRegLane((vT+0) % 32, 1, Ity_I32));
            storeLE(tEA_16, getQRegLane((vT+0) % 32, 2, Ity_I32));
            storeLE(tEA_24, getQRegLane((vT+0) % 32, 3, Ity_I32));
            storeLE(tEA_4,  getQRegLane((vT+1) % 32, 0, Ity_I32));
            storeLE(tEA_12, getQRegLane((vT+1) % 32, 1, Ity_I32));
            storeLE(tEA_20, getQRegLane((vT+1) % 32, 2, Ity_I32));
            storeLE(tEA_28, getQRegLane((vT+1) % 32, 3, Ity_I32));
         }
      }
      else {
         vassert(0); // Can't happen.
      }
      putIReg64orSP(rN, binop(Iop_Add64, mkexpr(tEA), mkU64(32)));
      DIP("%s {v%u.%s, v%u.%s}, [%s], #32\n", isLD ? "ld2" : "st2",
          (vT+0) % 32, name, (vT+1) % 32, name, nameIReg64orSP(rN));
      return True;
   }

   /* -------- LD1/ST1 (multi 1-elem structs, 2 regs, no offset) -------- */
   /* Only a very few cases. */
   /* 31        23
      0100 1100 0100 0000 1010 00 n t  LD1 {Vt.16b, V(t+1)%32.16b}, [Xn|SP]
      0100 1100 0000 0000 1010 00 n t  ST1 {Vt.16b, V(t+1)%32.16b}, [Xn|SP]
   */
   if (   (insn & 0xFFFFFC00) == 0x4C40A000 // LD1
       || (insn & 0xFFFFFC00) == 0x4C00A000 // ST1
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      const HChar* name = "16b";
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      IRExpr* tEA_0  = binop(Iop_Add64, mkexpr(tEA), mkU64(0));
      IRExpr* tEA_16 = binop(Iop_Add64, mkexpr(tEA), mkU64(16));
      if (isLD) {
         putQReg128((vT+0) % 32, loadLE(Ity_V128, tEA_0));
         putQReg128((vT+1) % 32, loadLE(Ity_V128, tEA_16));
      } else {
         storeLE(tEA_0,  getQReg128((vT+0) % 32));
         storeLE(tEA_16, getQReg128((vT+1) % 32));
      }
      DIP("%s {v%u.%s, v%u.%s}, [%s], #32\n", isLD ? "ld1" : "st1",
          (vT+0) % 32, name, (vT+1) % 32, name, nameIReg64orSP(rN));
      return True;
   }

   /* -------- LD1/ST1 (multi 1-elem structs, 3 regs, no offset) -------- */
   /* Only a very few cases. */
   /* 31        23
      0100 1100 0100 0000 0110 00 n t  LD1 {Vt.16b .. V(t+2)%32.16b}, [Xn|SP]
      0100 1100 0000 0000 0110 00 n t  ST1 {Vt.16b .. V(t+2)%32.16b}, [Xn|SP]
   */
   if (   (insn & 0xFFFFFC00) == 0x4C406000 // LD1
       || (insn & 0xFFFFFC00) == 0x4C006000 // ST1
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      const HChar* name = "16b";
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      IRExpr* tEA_0  = binop(Iop_Add64, mkexpr(tEA), mkU64(0));
      IRExpr* tEA_16 = binop(Iop_Add64, mkexpr(tEA), mkU64(16));
      IRExpr* tEA_32 = binop(Iop_Add64, mkexpr(tEA), mkU64(32));
      if (isLD) {
         putQReg128((vT+0) % 32, loadLE(Ity_V128, tEA_0));
         putQReg128((vT+1) % 32, loadLE(Ity_V128, tEA_16));
         putQReg128((vT+2) % 32, loadLE(Ity_V128, tEA_32));
      } else {
         storeLE(tEA_0,  getQReg128((vT+0) % 32));
         storeLE(tEA_16, getQReg128((vT+1) % 32));
         storeLE(tEA_32, getQReg128((vT+2) % 32));
      }
      DIP("%s {v%u.%s, v%u.%s, v%u.%s}, [%s], #32\n",
          isLD ? "ld1" : "st1",
          (vT+0) % 32, name, (vT+1) % 32, name, (vT+2) % 32, name,
          nameIReg64orSP(rN));
      return True;
   }

   /* -------- LD3/ST3 (multi 3-elem structs, 3 regs, post index) -------- */
   /* Only a very few cases. */
   /* 31        23             11 9 4
      0100 1100 1101 1111 0100 11 n t  LD3 {Vt.2d .. V(t+2)%32.2d}, [Xn|SP], #48
      0100 1100 1001 1111 0100 11 n t  ST3 {Vt.2d .. V(t+2)%32.2d}, [Xn|SP], #48
   */
   if (   (insn & 0xFFFFFC00) == 0x4CDF4C00 // LD3 .2d
       || (insn & 0xFFFFFC00) == 0x4C9F4C00 // ST3 .2d
      ) {
      Bool   isLD = INSN(22,22) == 1;
      UInt   rN   = INSN(9,5);
      UInt   vT   = INSN(4,0);
      IRTemp tEA  = newTemp(Ity_I64);
      UInt   sz   = INSN(11,10);
      const HChar* name = "??";
      assign(tEA, getIReg64orSP(rN));
      if (rN == 31) { /* FIXME generate stack alignment check */ }
      IRExpr* tEA_0  = binop(Iop_Add64, mkexpr(tEA), mkU64(0));
      IRExpr* tEA_8  = binop(Iop_Add64, mkexpr(tEA), mkU64(8));
      IRExpr* tEA_16 = binop(Iop_Add64, mkexpr(tEA), mkU64(16));
      IRExpr* tEA_24 = binop(Iop_Add64, mkexpr(tEA), mkU64(24));
      IRExpr* tEA_32 = binop(Iop_Add64, mkexpr(tEA), mkU64(32));
      IRExpr* tEA_40 = binop(Iop_Add64, mkexpr(tEA), mkU64(40));
      if (sz == BITS2(1,1)) {
         name = "2d";
         if (isLD) {
            putQRegLane((vT+0) % 32, 0, loadLE(Ity_I64, tEA_0));
            putQRegLane((vT+0) % 32, 1, loadLE(Ity_I64, tEA_24));
            putQRegLane((vT+1) % 32, 0, loadLE(Ity_I64, tEA_8));
            putQRegLane((vT+1) % 32, 1, loadLE(Ity_I64, tEA_32));
            putQRegLane((vT+2) % 32, 0, loadLE(Ity_I64, tEA_16));
            putQRegLane((vT+2) % 32, 1, loadLE(Ity_I64, tEA_40));
         } else {
            storeLE(tEA_0,  getQRegLane((vT+0) % 32, 0, Ity_I64));
            storeLE(tEA_24, getQRegLane((vT+0) % 32, 1, Ity_I64));
            storeLE(tEA_8,  getQRegLane((vT+1) % 32, 0, Ity_I64));
            storeLE(tEA_32, getQRegLane((vT+1) % 32, 1, Ity_I64));
            storeLE(tEA_16, getQRegLane((vT+2) % 32, 0, Ity_I64));
            storeLE(tEA_40, getQRegLane((vT+2) % 32, 1, Ity_I64));
         }
      }
      else {
         vassert(0); // Can't happen.
      }
      putIReg64orSP(rN, binop(Iop_Add64, mkexpr(tEA), mkU64(48)));
      DIP("%s {v%u.%s, v%u.%s, v%u.%s}, [%s], #32\n",
          isLD ? "ld3" : "st3",
          (vT+0) % 32, name, (vT+1) % 32, name, (vT+2) % 32, name,
          nameIReg64orSP(rN));
      return True;
   }

   /* ------------------ LD{,A}X{R,RH,RB} ------------------ */
   /* ------------------ ST{,L}X{R,RH,RB} ------------------ */
   /* 31 29     23  20      14    9 4
      sz 001000 010 11111 0 11111 n t   LDX{R,RH,RB}  Rt, [Xn|SP]
      sz 001000 010 11111 1 11111 n t   LDAX{R,RH,RB} Rt, [Xn|SP]
      sz 001000 000 s     0 11111 n t   STX{R,RH,RB}  Ws, Rt, [Xn|SP]
      sz 001000 000 s     1 11111 n t   STLX{R,RH,RB} Ws, Rt, [Xn|SP]
   */
   if (INSN(29,23) == BITS7(0,0,1,0,0,0,0)
       && (INSN(23,21) & BITS3(1,0,1)) == BITS3(0,0,0)
       && INSN(14,10) == BITS5(1,1,1,1,1)) {
      UInt szBlg2     = INSN(31,30);
      Bool isLD       = INSN(22,22) == 1;
      Bool isAcqOrRel = INSN(15,15) == 1;
      UInt ss         = INSN(20,16);
      UInt nn         = INSN(9,5);
      UInt tt         = INSN(4,0);

      vassert(szBlg2 < 4);
      UInt   szB = 1 << szBlg2; /* 1, 2, 4 or 8 */
      IRType ty  = integerIRTypeOfSize(szB);
      const HChar* suffix[4] = { "rb", "rh", "r", "r" };

      IRTemp ea = newTemp(Ity_I64);
      assign(ea, getIReg64orSP(nn));
      /* FIXME generate check that ea is szB-aligned */

      if (isLD && ss == BITS5(1,1,1,1,1)) {
         IRTemp res = newTemp(ty);
         stmt(IRStmt_LLSC(Iend_LE, res, mkexpr(ea), NULL/*LL*/));
         putIReg64orZR(tt, widenUto64(ty, mkexpr(res)));
         if (isAcqOrRel) {
            stmt(IRStmt_MBE(Imbe_Fence));
         }
         DIP("ld%sx%s %s, [%s]\n", isAcqOrRel ? "a" : "", suffix[szBlg2],
             nameIRegOrZR(szB == 8, tt), nameIReg64orSP(nn));
         return True;
      }
      if (!isLD) {
         if (isAcqOrRel) {
            stmt(IRStmt_MBE(Imbe_Fence));
         }
         IRTemp  res  = newTemp(Ity_I1);
         IRExpr* data = narrowFrom64(ty, getIReg64orZR(tt));
         stmt(IRStmt_LLSC(Iend_LE, res, mkexpr(ea), data));
         /* IR semantics: res is 1 if store succeeds, 0 if it fails.
            Need to set rS to 1 on failure, 0 on success. */
         putIReg64orZR(ss, binop(Iop_Xor64, unop(Iop_1Uto64, mkexpr(res)),
                                            mkU64(1)));
         DIP("st%sx%s %s, %s, [%s]\n", isAcqOrRel ? "a" : "", suffix[szBlg2],
             nameIRegOrZR(False, ss),
             nameIRegOrZR(szB == 8, tt), nameIReg64orSP(nn));
         return True;
      }
      /* else fall through */
   }

   /* ------------------ LDA{R,RH,RB} ------------------ */
   /* ------------------ STL{R,RH,RB} ------------------ */
   /* 31 29     23  20      14    9 4
      sz 001000 110 11111 1 11111 n t   LDAR<sz> Rt, [Xn|SP]
      sz 001000 100 11111 1 11111 n t   STLR<sz> Rt, [Xn|SP]
   */
   if (INSN(29,23) == BITS7(0,0,1,0,0,0,1)
       && INSN(21,10) == BITS12(0,1,1,1,1,1,1,1,1,1,1,1)) {
      UInt szBlg2 = INSN(31,30);
      Bool isLD   = INSN(22,22) == 1;
      UInt nn     = INSN(9,5);
      UInt tt     = INSN(4,0);

      vassert(szBlg2 < 4);
      UInt   szB = 1 << szBlg2; /* 1, 2, 4 or 8 */
      IRType ty  = integerIRTypeOfSize(szB);
      const HChar* suffix[4] = { "rb", "rh", "r", "r" };

      IRTemp ea = newTemp(Ity_I64);
      assign(ea, getIReg64orSP(nn));
      /* FIXME generate check that ea is szB-aligned */

      if (isLD) {
         IRTemp res = newTemp(ty);
         assign(res, loadLE(ty, mkexpr(ea)));
         putIReg64orZR(tt, widenUto64(ty, mkexpr(res)));
         stmt(IRStmt_MBE(Imbe_Fence));
         DIP("lda%s %s, [%s]\n", suffix[szBlg2],
             nameIRegOrZR(szB == 8, tt), nameIReg64orSP(nn));
      } else {
         stmt(IRStmt_MBE(Imbe_Fence));
         IRExpr* data = narrowFrom64(ty, getIReg64orZR(tt));
         storeLE(mkexpr(ea), data);
         DIP("stl%s %s, [%s]\n", suffix[szBlg2],
             nameIRegOrZR(szB == 8, tt), nameIReg64orSP(nn));
      }
      return True;
   }

   vex_printf("ARM64 front end: load_store\n");
   return False;
#  undef INSN
}


/*------------------------------------------------------------*/
/*--- Control flow and misc instructions                   ---*/
/*------------------------------------------------------------*/

static
Bool dis_ARM64_branch_etc(/*MB_OUT*/DisResult* dres, UInt insn,
                          VexArchInfo* archinfo)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))

   /* ---------------------- B cond ----------------------- */
   /* 31        24    4 3
      0101010 0 imm19 0 cond */
   if (INSN(31,24) == BITS8(0,1,0,1,0,1,0,0) && INSN(4,4) == 0) {
      UInt  cond   = INSN(3,0);
      ULong uimm64 = INSN(23,5) << 2;
      Long  simm64 = (Long)sx_to_64(uimm64, 21);
      vassert(dres->whatNext    == Dis_Continue);
      vassert(dres->len         == 4);
      vassert(dres->continueAt  == 0);
      vassert(dres->jk_StopHere == Ijk_INVALID);
      stmt( IRStmt_Exit(unop(Iop_64to1, mk_arm64g_calculate_condition(cond)),
                        Ijk_Boring,
                        IRConst_U64(guest_PC_curr_instr + simm64),
                        OFFB_PC) );
      putPC(mkU64(guest_PC_curr_instr + 4));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_Boring;
      DIP("b.%s 0x%llx\n", nameCC(cond), guest_PC_curr_instr + simm64);
      return True;
   }

   /* -------------------- B{L} uncond -------------------- */
   if (INSN(30,26) == BITS5(0,0,1,0,1)) {
      /* 000101 imm26  B  (PC + sxTo64(imm26 << 2))
         100101 imm26  B  (PC + sxTo64(imm26 << 2))
      */
      UInt  bLink  = INSN(31,31);
      ULong uimm64 = INSN(25,0) << 2;
      Long  simm64 = (Long)sx_to_64(uimm64, 28);
      if (bLink) {
         putIReg64orSP(30, mkU64(guest_PC_curr_instr + 4));
      }
      putPC(mkU64(guest_PC_curr_instr + simm64));
      dres->whatNext = Dis_StopHere;
      dres->jk_StopHere = Ijk_Call;
      DIP("b%s 0x%llx\n", bLink == 1 ? "l" : "",
                          guest_PC_curr_instr + simm64);
      return True;
   }

   /* --------------------- B{L} reg --------------------- */
   /* 31      24 22 20    15     9  4
      1101011 00 10 11111 000000 nn 00000  RET  Rn
      1101011 00 01 11111 000000 nn 00000  CALL Rn
      1101011 00 00 11111 000000 nn 00000  JMP  Rn
   */
   if (INSN(31,23) == BITS9(1,1,0,1,0,1,1,0,0)
       && INSN(20,16) == BITS5(1,1,1,1,1)
       && INSN(15,10) == BITS6(0,0,0,0,0,0)
       && INSN(4,0) == BITS5(0,0,0,0,0)) {
      UInt branch_type = INSN(22,21);
      UInt nn          = INSN(9,5);
      if (branch_type == BITS2(1,0) /* RET */) {
         putPC(getIReg64orZR(nn));
         dres->whatNext = Dis_StopHere;
         dres->jk_StopHere = Ijk_Ret;
         DIP("ret %s\n", nameIReg64orZR(nn));
         return True;
      }
      if (branch_type == BITS2(0,1) /* CALL */) {
         IRTemp dst = newTemp(Ity_I64);
         assign(dst, getIReg64orZR(nn));
         putIReg64orSP(30, mkU64(guest_PC_curr_instr + 4));
         putPC(mkexpr(dst));
         dres->whatNext = Dis_StopHere;
         dres->jk_StopHere = Ijk_Call;
         DIP("blr %s\n", nameIReg64orZR(nn));
         return True;
      }
      if (branch_type == BITS2(0,0) /* JMP */) {
         putPC(getIReg64orZR(nn));
         dres->whatNext = Dis_StopHere;
         dres->jk_StopHere = Ijk_Boring;
         DIP("jmp %s\n", nameIReg64orZR(nn));
         return True;
      }
   }

   /* -------------------- CB{N}Z -------------------- */
   /* sf 011 010 1 imm19 Rt   CBNZ Xt|Wt, (PC + sxTo64(imm19 << 2))
      sf 011 010 0 imm19 Rt   CBZ  Xt|Wt, (PC + sxTo64(imm19 << 2))
   */
   if (INSN(30,25) == BITS6(0,1,1,0,1,0)) {
      Bool    is64   = INSN(31,31) == 1;
      Bool    bIfZ   = INSN(24,24) == 0;
      ULong   uimm64 = INSN(23,5) << 2;
      UInt    rT     = INSN(4,0);
      Long    simm64 = (Long)sx_to_64(uimm64, 21);
      IRExpr* cond   = NULL;
      if (is64) {
         cond = binop(bIfZ ? Iop_CmpEQ64 : Iop_CmpNE64,
                      getIReg64orZR(rT), mkU64(0));
      } else {
         cond = binop(bIfZ ? Iop_CmpEQ32 : Iop_CmpNE32,
                      getIReg32orZR(rT), mkU32(0));
      }
      stmt( IRStmt_Exit(cond,
                        Ijk_Boring,
                        IRConst_U64(guest_PC_curr_instr + simm64),
                        OFFB_PC) );
      putPC(mkU64(guest_PC_curr_instr + 4));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_Boring;
      DIP("cb%sz %s, 0x%llx\n",
          bIfZ ? "" : "n", nameIRegOrZR(is64, rT),
          guest_PC_curr_instr + simm64);
      return True;
   }

   /* -------------------- TB{N}Z -------------------- */
   /* 31 30      24 23  18  5 4
      b5 011 011 1  b40 imm14 t  TBNZ Xt, #(b5:b40), (PC + sxTo64(imm14 << 2))
      b5 011 011 0  b40 imm14 t  TBZ  Xt, #(b5:b40), (PC + sxTo64(imm14 << 2))
   */
   if (INSN(30,25) == BITS6(0,1,1,0,1,1)) {
      UInt    b5     = INSN(31,31);
      Bool    bIfZ   = INSN(24,24) == 0;
      UInt    b40    = INSN(23,19);
      UInt    imm14  = INSN(18,5);
      UInt    tt     = INSN(4,0);
      UInt    bitNo  = (b5 << 5) | b40;
      ULong   uimm64 = imm14 << 2;
      Long    simm64 = sx_to_64(uimm64, 16);
      IRExpr* cond 
         = binop(bIfZ ? Iop_CmpEQ64 : Iop_CmpNE64,
                 binop(Iop_And64,
                       binop(Iop_Shr64, getIReg64orZR(tt), mkU8(bitNo)),
                       mkU64(1)),
                 mkU64(0));
      stmt( IRStmt_Exit(cond,
                        Ijk_Boring,
                        IRConst_U64(guest_PC_curr_instr + simm64),
                        OFFB_PC) );
      putPC(mkU64(guest_PC_curr_instr + 4));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_Boring;
      DIP("tb%sz %s, #%u, 0x%llx\n",
          bIfZ ? "" : "n", nameIReg64orZR(tt), bitNo,
          guest_PC_curr_instr + simm64);
      return True;
   }

   /* -------------------- SVC -------------------- */
   /* 11010100 000 imm16 000 01
      Don't bother with anything except the imm16==0 case.
   */
   if (INSN(31,0) == 0xD4000001) {
      putPC(mkU64(guest_PC_curr_instr + 4));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_Sys_syscall;
      DIP("svc #0\n");
      return True;
   }

   /* ------------------ M{SR,RS} ------------------ */
   /* Only handles the case where the system register is TPIDR_EL0.
      0xD51BD0 010 Rt   MSR tpidr_el0, rT
      0xD53BD0 010 Rt   MRS rT, tpidr_el0
   */
   if (   (INSN(31,0) & 0xFFFFFFE0) == 0xD51BD040 /*MSR*/
       || (INSN(31,0) & 0xFFFFFFE0) == 0xD53BD040 /*MRS*/) {
      Bool toSys = INSN(21,21) == 0;
      UInt tt    = INSN(4,0);
      if (toSys) {
         stmt( IRStmt_Put( OFFB_TPIDR_EL0, getIReg64orZR(tt)) );
         DIP("msr tpidr_el0, %s\n", nameIReg64orZR(tt));
      } else {
         putIReg64orZR(tt, IRExpr_Get( OFFB_TPIDR_EL0, Ity_I64 ));
         DIP("mrs %s, tpidr_el0\n", nameIReg64orZR(tt));
      }
      return True;
   }
   /* Cases for FPCR 
      0xD51B44 000 Rt  MSR fpcr, rT
      0xD53B44 000 Rt  MSR rT, fpcr
   */
   if (   (INSN(31,0) & 0xFFFFFFE0) == 0xD51B4400 /*MSR*/
       || (INSN(31,0) & 0xFFFFFFE0) == 0xD53B4400 /*MRS*/) {
      Bool toSys = INSN(21,21) == 0;
      UInt tt    = INSN(4,0);
      if (toSys) {
         stmt( IRStmt_Put( OFFB_FPCR, getIReg32orZR(tt)) );
         DIP("msr fpcr, %s\n", nameIReg64orZR(tt));
      } else {
         putIReg32orZR(tt, IRExpr_Get(OFFB_FPCR, Ity_I32));
         DIP("mrs %s, fpcr\n", nameIReg64orZR(tt));
      }
      return True;
   }
   /* Cases for FPSR 
      0xD51B44 001 Rt  MSR fpsr, rT
      0xD53B44 001 Rt  MSR rT, fpsr
   */
   if (   (INSN(31,0) & 0xFFFFFFE0) == 0xD51B4420 /*MSR*/
       || (INSN(31,0) & 0xFFFFFFE0) == 0xD53B4420 /*MRS*/) {
      Bool toSys = INSN(21,21) == 0;
      UInt tt    = INSN(4,0);
      if (toSys) {
         stmt( IRStmt_Put( OFFB_FPSR, getIReg32orZR(tt)) );
         DIP("msr fpsr, %s\n", nameIReg64orZR(tt));
      } else {
         putIReg32orZR(tt, IRExpr_Get(OFFB_FPSR, Ity_I32));
         DIP("mrs %s, fpsr\n", nameIReg64orZR(tt));
      }
      return True;
   }
   /* Cases for NZCV
      D51B42 000 Rt  MSR nzcv, rT
      D53B42 000 Rt  MRS rT, nzcv
   */
   if (   (INSN(31,0) & 0xFFFFFFE0) == 0xD51B4200 /*MSR*/
       || (INSN(31,0) & 0xFFFFFFE0) == 0xD53B4200 /*MRS*/) {
      Bool  toSys = INSN(21,21) == 0;
      UInt  tt    = INSN(4,0);
      if (toSys) {
         IRTemp t = newTemp(Ity_I64);
         assign(t, binop(Iop_And64, getIReg64orZR(tt), mkU64(0xF0000000ULL)));
         setFlags_COPY(t);
         DIP("msr %s, nzcv\n", nameIReg32orZR(tt));
      } else {
         IRTemp res = newTemp(Ity_I64);
         assign(res, mk_arm64g_calculate_flags_nzcv());
         putIReg32orZR(tt, unop(Iop_64to32, mkexpr(res)));
         DIP("mrs %s, nzcv\n", nameIReg64orZR(tt));
      }
      return True;
   }
   /* Cases for DCZID_EL0
      Don't support arbitrary reads and writes to this register.  Just
      return the value 16, which indicates that the DC ZVA instruction
      is not permitted, so we don't have to emulate it.
      D5 3B 00 111 Rt  MRS rT, dczid_el0
   */
   if ((INSN(31,0) & 0xFFFFFFE0) == 0xD53B00E0) {
      UInt tt = INSN(4,0);
      putIReg64orZR(tt, mkU64(1<<4));
      DIP("mrs %s, dczid_el0 (FAKED)\n", nameIReg64orZR(tt));
      return True;
   }
   /* Cases for CTR_EL0
      We just handle reads, and make up a value from the D and I line
      sizes in the VexArchInfo we are given, and patch in the following
      fields that the Foundation model gives ("natively"):
      CWG = 0b0100, ERG = 0b0100, L1Ip = 0b11
      D5 3B 00 001 Rt  MRS rT, dczid_el0
   */
   if ((INSN(31,0) & 0xFFFFFFE0) == 0xD53B0020) {
      UInt tt = INSN(4,0);
      /* Need to generate a value from dMinLine_lg2_szB and
         dMinLine_lg2_szB.  The value in the register is in 32-bit
         units, so need to subtract 2 from the values in the
         VexArchInfo.  We can assume that the values here are valid --
         disInstr_ARM64 checks them -- so there's no need to deal with
         out-of-range cases. */
      vassert(archinfo->arm64_dMinLine_lg2_szB >= 2
              && archinfo->arm64_dMinLine_lg2_szB <= 17
              && archinfo->arm64_iMinLine_lg2_szB >= 2
              && archinfo->arm64_iMinLine_lg2_szB <= 17);
      UInt val
         = 0x8440c000 | ((0xF & (archinfo->arm64_dMinLine_lg2_szB - 2)) << 16)
                      | ((0xF & (archinfo->arm64_iMinLine_lg2_szB - 2)) << 0);
      putIReg64orZR(tt, mkU64(val));
      DIP("mrs %s, ctr_el0\n", nameIReg64orZR(tt));
      return True;
   }

   /* ------------------ IC_IVAU ------------------ */
   /* D5 0B 75 001 Rt  ic ivau, rT
   */
   if ((INSN(31,0) & 0xFFFFFFE0) == 0xD50B7520) {
      /* We will always be provided with a valid iMinLine value. */
      vassert(archinfo->arm64_iMinLine_lg2_szB >= 2
              && archinfo->arm64_iMinLine_lg2_szB <= 17);
      /* Round the requested address, in rT, down to the start of the
         containing block. */
      UInt   tt      = INSN(4,0);
      ULong  lineszB = 1ULL << archinfo->arm64_iMinLine_lg2_szB;
      IRTemp addr    = newTemp(Ity_I64);
      assign( addr, binop( Iop_And64,
                           getIReg64orZR(tt),
                           mkU64(~(lineszB - 1))) );
      /* Set the invalidation range, request exit-and-invalidate, with
         continuation at the next instruction. */
      stmt(IRStmt_Put(OFFB_CMSTART, mkexpr(addr)));
      stmt(IRStmt_Put(OFFB_CMLEN,   mkU64(lineszB)));
      /* be paranoid ... */
      stmt( IRStmt_MBE(Imbe_Fence) );
      putPC(mkU64( guest_PC_curr_instr + 4 ));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_InvalICache;
      DIP("ic ivau, %s\n", nameIReg64orZR(tt));
      return True;
   }

   /* ------------------ DC_CVAU ------------------ */
   /* D5 0B 7B 001 Rt  dc cvau, rT
   */
   if ((INSN(31,0) & 0xFFFFFFE0) == 0xD50B7B20) {
      /* Exactly the same scheme as for IC IVAU, except we observe the
         dMinLine size, and request an Ijk_FlushDCache instead of
         Ijk_InvalICache. */
      /* We will always be provided with a valid dMinLine value. */
      vassert(archinfo->arm64_dMinLine_lg2_szB >= 2
              && archinfo->arm64_dMinLine_lg2_szB <= 17);
      /* Round the requested address, in rT, down to the start of the
         containing block. */
      UInt   tt      = INSN(4,0);
      ULong  lineszB = 1ULL << archinfo->arm64_dMinLine_lg2_szB;
      IRTemp addr    = newTemp(Ity_I64);
      assign( addr, binop( Iop_And64,
                           getIReg64orZR(tt),
                           mkU64(~(lineszB - 1))) );
      /* Set the flush range, request exit-and-flush, with
         continuation at the next instruction. */
      stmt(IRStmt_Put(OFFB_CMSTART, mkexpr(addr)));
      stmt(IRStmt_Put(OFFB_CMLEN,   mkU64(lineszB)));
      /* be paranoid ... */
      stmt( IRStmt_MBE(Imbe_Fence) );
      putPC(mkU64( guest_PC_curr_instr + 4 ));
      dres->whatNext    = Dis_StopHere;
      dres->jk_StopHere = Ijk_FlushDCache;
      DIP("dc cvau, %s\n", nameIReg64orZR(tt));
      return True;
   }

   /* ------------------ ISB, DMB, DSB ------------------ */
   if (INSN(31,0) == 0xD5033FDF) {
      stmt(IRStmt_MBE(Imbe_Fence));
      DIP("isb\n");
      return True;
   }
   if (INSN(31,0) == 0xD5033BBF) {
      stmt(IRStmt_MBE(Imbe_Fence));
      DIP("dmb ish\n");
      return True;
   }
   if (INSN(31,0) == 0xD5033ABF) {
      stmt(IRStmt_MBE(Imbe_Fence));
      DIP("dmb ishst\n");
      return True;
   }
   if (INSN(31,0) == 0xD5033B9F) {
      stmt(IRStmt_MBE(Imbe_Fence));
      DIP("dsb ish\n");
      return True;
   }

   /* -------------------- NOP -------------------- */
   if (INSN(31,0) == 0xD503201F) {
      DIP("nop\n");
      return True;
   }

  //fail:
   vex_printf("ARM64 front end: branch_etc\n");
   return False;
#  undef INSN
}


/*------------------------------------------------------------*/
/*--- SIMD and FP instructions                             ---*/
/*------------------------------------------------------------*/

/* Some constructors for interleave/deinterleave expressions. */

static IRExpr* mk_CatEvenLanes64x2 ( IRTemp a10, IRTemp b10 ) {
   // returns a0 b0
   return binop(Iop_InterleaveLO64x2, mkexpr(a10), mkexpr(b10));
}

static IRExpr* mk_CatOddLanes64x2 ( IRTemp a10, IRTemp b10 ) {
   // returns a1 b1
   return binop(Iop_InterleaveHI64x2, mkexpr(a10), mkexpr(b10));
}

static IRExpr* mk_CatEvenLanes32x4 ( IRTemp a3210, IRTemp b3210 ) {
   // returns a2 a0 b2 b0
   return binop(Iop_CatEvenLanes32x4, mkexpr(a3210), mkexpr(b3210));
}

static IRExpr* mk_CatOddLanes32x4 ( IRTemp a3210, IRTemp b3210 ) {
   // returns a3 a1 b3 b1
   return binop(Iop_CatOddLanes32x4, mkexpr(a3210), mkexpr(b3210));
}

static IRExpr* mk_InterleaveLO32x4 ( IRTemp a3210, IRTemp b3210 ) {
   // returns a1 b1 a0 b0
   return binop(Iop_InterleaveLO32x4, mkexpr(a3210), mkexpr(b3210));
}

static IRExpr* mk_InterleaveHI32x4 ( IRTemp a3210, IRTemp b3210 ) {
   // returns a3 b3 a2 b2
   return binop(Iop_InterleaveHI32x4, mkexpr(a3210), mkexpr(b3210));
}

static IRExpr* mk_CatEvenLanes16x8 ( IRTemp a76543210, IRTemp b76543210 ) {
   // returns a6 a4 a2 a0 b6 b4 b2 b0
   return binop(Iop_CatEvenLanes16x8, mkexpr(a76543210), mkexpr(b76543210));
}

static IRExpr* mk_CatOddLanes16x8 ( IRTemp a76543210, IRTemp b76543210 ) {
   // returns a7 a5 a3 a1 b7 b5 b3 b1
   return binop(Iop_CatOddLanes16x8, mkexpr(a76543210), mkexpr(b76543210));
}

static IRExpr* mk_InterleaveLO16x8 ( IRTemp a76543210, IRTemp b76543210 ) {
   // returns a3 b3 a2 b2 a1 b1 a0 b0
   return binop(Iop_InterleaveLO16x8, mkexpr(a76543210), mkexpr(b76543210));
}

static IRExpr* mk_InterleaveHI16x8 ( IRTemp a76543210, IRTemp b76543210 ) {
   // returns a7 b7 a6 b6 a5 b5 a4 b4
   return binop(Iop_InterleaveHI16x8, mkexpr(a76543210), mkexpr(b76543210));
}

static IRExpr* mk_CatEvenLanes8x16 ( IRTemp aFEDCBA9876543210,
                                     IRTemp bFEDCBA9876543210 ) {
   // returns aE aC aA a8 a6 a4 a2 a0 bE bC bA b8 b6 b4 b2 b0
   return binop(Iop_CatEvenLanes8x16, mkexpr(aFEDCBA9876543210),
                                      mkexpr(bFEDCBA9876543210));
}

static IRExpr* mk_CatOddLanes8x16 ( IRTemp aFEDCBA9876543210,
                                    IRTemp bFEDCBA9876543210 ) {
   // returns aF aD aB a9 a7 a5 a3 a1 bF bD bB b9 b7 b5 b3 b1
   return binop(Iop_CatOddLanes8x16, mkexpr(aFEDCBA9876543210),
                                     mkexpr(bFEDCBA9876543210));
}

static IRExpr* mk_InterleaveLO8x16 ( IRTemp aFEDCBA9876543210,
                                     IRTemp bFEDCBA9876543210 ) {
   // returns a7 b7 a6 b6 a5 b5 a4 b4 a3 b3 a2 b2 a1 b1 a0 b0
   return binop(Iop_InterleaveLO8x16, mkexpr(aFEDCBA9876543210),
                                      mkexpr(bFEDCBA9876543210));
}

static IRExpr* mk_InterleaveHI8x16 ( IRTemp aFEDCBA9876543210,
                                     IRTemp bFEDCBA9876543210 ) {
   // returns aF bF aE bE aD bD aC bC aB bB aA bA a9 b9 a8 b8
   return binop(Iop_InterleaveHI8x16, mkexpr(aFEDCBA9876543210),
                                      mkexpr(bFEDCBA9876543210));
}

/* Generate N copies of |bit| in the bottom of a ULong. */
static ULong Replicate ( ULong bit, Int N )
{
   vassert(bit <= 1 && N >= 1 && N < 64);
   if (bit == 0) {
      return 0;
    } else {
      /* Careful.  This won't work for N == 64. */
      return (1ULL << N) - 1;
   }
}

static ULong Replicate32x2 ( ULong bits32 )
{
   vassert(0 == (bits32 & ~0xFFFFFFFFULL));
   return (bits32 << 32) | bits32;
}

static ULong Replicate16x4 ( ULong bits16 )
{
   vassert(0 == (bits16 & ~0xFFFFULL));
   return Replicate32x2((bits16 << 16) | bits16);
}

static ULong Replicate8x8 ( ULong bits8 )
{
   vassert(0 == (bits8 & ~0xFFULL));
   return Replicate16x4((bits8 << 8) | bits8);
}

/* Expand the VFPExpandImm-style encoding in the bottom 8 bits of
   |imm8| to either a 32-bit value if N is 32 or a 64 bit value if N
   is 64.  In the former case, the upper 32 bits of the returned value
   are guaranteed to be zero. */
static ULong VFPExpandImm ( ULong imm8, Int N )
{
   vassert(imm8 <= 0xFF);
   vassert(N == 32 || N == 64);
   Int E = ((N == 32) ? 8 : 11) - 2; // The spec incorrectly omits the -2.
   Int F = N - E - 1;
   ULong imm8_6 = (imm8 >> 6) & 1;
   /* sign: 1 bit */
   /* exp:  E bits */
   /* frac: F bits */
   ULong sign = (imm8 >> 7) & 1;
   ULong exp  = ((imm8_6 ^ 1) << (E-1)) | Replicate(imm8_6, E-1);
   ULong frac = ((imm8 & 63) << (F-6)) | Replicate(0, F-6);
   vassert(sign < (1ULL << 1));
   vassert(exp  < (1ULL << E));
   vassert(frac < (1ULL << F));
   vassert(1 + E + F == N);
   ULong res = (sign << (E+F)) | (exp << F) | frac;
   return res;
}

/* Expand an AdvSIMDExpandImm-style encoding into a 64-bit value.
   This might fail, as indicated by the returned Bool.  Page 2530 of
   the manual. */
static Bool AdvSIMDExpandImm ( /*OUT*/ULong* res,
                               UInt op, UInt cmode, UInt imm8 )
{
   vassert(op <= 1);
   vassert(cmode <= 15);
   vassert(imm8 <= 255);

   *res = 0; /* will overwrite iff returning True */

   ULong imm64    = 0;
   Bool  testimm8 = False;

   switch (cmode >> 1) {
      case 0:
         testimm8 = False; imm64 = Replicate32x2(imm8); break;
      case 1:
         testimm8 = True; imm64 = Replicate32x2(imm8 << 8); break;
      case 2:
         testimm8 = True; imm64 = Replicate32x2(imm8 << 16); break;
      case 3:
         testimm8 = True; imm64 = Replicate32x2(imm8 << 24); break;
      case 4:
          testimm8 = False; imm64 = Replicate16x4(imm8); break;
      case 5:
          testimm8 = True; imm64 = Replicate16x4(imm8 << 8); break;
      case 6:
          testimm8 = True;
          if ((cmode & 1) == 0)
              imm64 = Replicate32x2((imm8 << 8) | 0xFF);
          else
              imm64 = Replicate32x2((imm8 << 16) | 0xFFFF);
          break;
      case 7:
         testimm8 = False;
         if ((cmode & 1) == 0 && op == 0)
             imm64 = Replicate8x8(imm8);
         if ((cmode & 1) == 0 && op == 1) {
             imm64 = 0;   imm64 |= (imm8 & 0x80) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x40) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x20) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x10) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x08) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x04) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x02) ? 0xFF : 0x00;
             imm64 <<= 8; imm64 |= (imm8 & 0x01) ? 0xFF : 0x00;
         }
         if ((cmode & 1) == 1 && op == 0) {
            ULong imm8_7  = (imm8 >> 7) & 1;
            ULong imm8_6  = (imm8 >> 6) & 1;
            ULong imm8_50 = imm8 & 63;
            ULong imm32 = (imm8_7                 << (1 + 5 + 6 + 19))
                          | ((imm8_6 ^ 1)         << (5 + 6 + 19))
                          | (Replicate(imm8_6, 5) << (6 + 19))
                          | (imm8_50              << 19);
            imm64 = Replicate32x2(imm32);
         }
         if ((cmode & 1) == 1 && op == 1) {
            // imm64 = imm8<7>:NOT(imm8<6>)
            //                :Replicate(imm8<6>,8):imm8<5:0>:Zeros(48);
            ULong imm8_7  = (imm8 >> 7) & 1;
            ULong imm8_6  = (imm8 >> 6) & 1;
            ULong imm8_50 = imm8 & 63;
            imm64 = (imm8_7 << 63) | ((imm8_6 ^ 1) << 62)
                    | (Replicate(imm8_6, 8) << 54)
                    | (imm8_50 << 48);
         }
         break;
      default:
        vassert(0);
   }

   if (testimm8 && imm8 == 0)
      return False;

   *res = imm64;
   return True;
}

/* Help a bit for decoding laneage for vector operations that can be
   of the form 4x32, 2x64 or 2x32-and-zero-upper-half, as encoded by Q
   and SZ bits, typically for vector floating point. */
static Bool getLaneInfo_Q_SZ ( /*OUT*/IRType* tyI,  /*OUT*/IRType* tyF,
                               /*OUT*/UInt* nLanes, /*OUT*/Bool* zeroUpper,
                               /*OUT*/const HChar** arrSpec,
                               Bool bitQ, Bool bitSZ )
{
   vassert(bitQ == True || bitQ == False);
   vassert(bitSZ == True || bitSZ == False);
   if (bitQ && bitSZ) { // 2x64
      if (tyI)       *tyI       = Ity_I64;
      if (tyF)       *tyF       = Ity_F64;
      if (nLanes)    *nLanes    = 2;
      if (zeroUpper) *zeroUpper = False;
      if (arrSpec)   *arrSpec   = "2d";
      return True;
   }
   if (bitQ && !bitSZ) { // 4x32
      if (tyI)       *tyI       = Ity_I32;
      if (tyF)       *tyF       = Ity_F32;
      if (nLanes)    *nLanes    = 4;
      if (zeroUpper) *zeroUpper = False;
      if (arrSpec)   *arrSpec   = "4s";
      return True;
   }
   if (!bitQ && !bitSZ) { // 2x32
      if (tyI)       *tyI       = Ity_I32;
      if (tyF)       *tyF       = Ity_F32;
      if (nLanes)    *nLanes    = 2;
      if (zeroUpper) *zeroUpper = True;
      if (arrSpec)   *arrSpec   = "2s";
      return True;
   }
   // Else impliedly 1x64, which isn't allowed.
   return False;
}

/* Helper for decoding laneage for shift-style vector operations 
   that involve an immediate shift amount. */
static Bool getLaneInfo_IMMH_IMMB ( /*OUT*/UInt* shift, /*OUT*/UInt* szBlg2,
                                    UInt immh, UInt immb )
{
   vassert(immh < (1<<4));
   vassert(immb < (1<<3));
   UInt immhb = (immh << 3) | immb;
   if (immh & 8) {
      if (shift)  *shift  = 128 - immhb;
      if (szBlg2) *szBlg2 = 3;
      return True;
   }
   if (immh & 4) {
      if (shift)  *shift  = 64 - immhb;
      if (szBlg2) *szBlg2 = 2;
      return True;
   }
   if (immh & 2) {
      if (shift)  *shift  = 32 - immhb;
      if (szBlg2) *szBlg2 = 1;
      return True;
   }
   if (immh & 1) {
      if (shift)  *shift  = 16 - immhb;
      if (szBlg2) *szBlg2 = 0;
      return True;
   }
   return False;
}

/* Generate IR to fold all lanes of the V128 value in 'src' as
   characterised by the operator 'op', and return the result in the
   bottom bits of a V128, with all other bits set to zero. */
static IRTemp math_MINMAXV ( IRTemp src, IROp op )
{
   /* The basic idea is to use repeated applications of Iop_CatEven*
      and Iop_CatOdd* operators to 'src' so as to clone each lane into
      a complete vector.  Then fold all those vectors with 'op' and
      zero out all but the least significant lane. */
   switch (op) {
      case Iop_Min8Sx16: case Iop_Min8Ux16:
      case Iop_Max8Sx16: case Iop_Max8Ux16: case Iop_Add8x16: {
         /* NB: temp naming here is misleading -- the naming is for 8
            lanes of 16 bit, whereas what is being operated on is 16
            lanes of 8 bits. */
         IRTemp x76543210 = src;
         IRTemp x76547654 = newTemp(Ity_V128);
         IRTemp x32103210 = newTemp(Ity_V128);
         assign(x76547654, mk_CatOddLanes64x2 (x76543210, x76543210));
         assign(x32103210, mk_CatEvenLanes64x2(x76543210, x76543210));
         IRTemp x76767676 = newTemp(Ity_V128);
         IRTemp x54545454 = newTemp(Ity_V128);
         IRTemp x32323232 = newTemp(Ity_V128);
         IRTemp x10101010 = newTemp(Ity_V128);
         assign(x76767676, mk_CatOddLanes32x4 (x76547654, x76547654));
         assign(x54545454, mk_CatEvenLanes32x4(x76547654, x76547654));
         assign(x32323232, mk_CatOddLanes32x4 (x32103210, x32103210));
         assign(x10101010, mk_CatEvenLanes32x4(x32103210, x32103210));
         IRTemp x77777777 = newTemp(Ity_V128);
         IRTemp x66666666 = newTemp(Ity_V128);
         IRTemp x55555555 = newTemp(Ity_V128);
         IRTemp x44444444 = newTemp(Ity_V128);
         IRTemp x33333333 = newTemp(Ity_V128);
         IRTemp x22222222 = newTemp(Ity_V128);
         IRTemp x11111111 = newTemp(Ity_V128);
         IRTemp x00000000 = newTemp(Ity_V128);
         assign(x77777777, mk_CatOddLanes16x8 (x76767676, x76767676));
         assign(x66666666, mk_CatEvenLanes16x8(x76767676, x76767676));
         assign(x55555555, mk_CatOddLanes16x8 (x54545454, x54545454));
         assign(x44444444, mk_CatEvenLanes16x8(x54545454, x54545454));
         assign(x33333333, mk_CatOddLanes16x8 (x32323232, x32323232));
         assign(x22222222, mk_CatEvenLanes16x8(x32323232, x32323232));
         assign(x11111111, mk_CatOddLanes16x8 (x10101010, x10101010));
         assign(x00000000, mk_CatEvenLanes16x8(x10101010, x10101010));
         /* Naming not misleading after here. */
         IRTemp xAllF = newTemp(Ity_V128);
         IRTemp xAllE = newTemp(Ity_V128);
         IRTemp xAllD = newTemp(Ity_V128);
         IRTemp xAllC = newTemp(Ity_V128);
         IRTemp xAllB = newTemp(Ity_V128);
         IRTemp xAllA = newTemp(Ity_V128);
         IRTemp xAll9 = newTemp(Ity_V128);
         IRTemp xAll8 = newTemp(Ity_V128);
         IRTemp xAll7 = newTemp(Ity_V128);
         IRTemp xAll6 = newTemp(Ity_V128);
         IRTemp xAll5 = newTemp(Ity_V128);
         IRTemp xAll4 = newTemp(Ity_V128);
         IRTemp xAll3 = newTemp(Ity_V128);
         IRTemp xAll2 = newTemp(Ity_V128);
         IRTemp xAll1 = newTemp(Ity_V128);
         IRTemp xAll0 = newTemp(Ity_V128);
         assign(xAllF, mk_CatOddLanes8x16 (x77777777, x77777777));
         assign(xAllE, mk_CatEvenLanes8x16(x77777777, x77777777));
         assign(xAllD, mk_CatOddLanes8x16 (x66666666, x66666666));
         assign(xAllC, mk_CatEvenLanes8x16(x66666666, x66666666));
         assign(xAllB, mk_CatOddLanes8x16 (x55555555, x55555555));
         assign(xAllA, mk_CatEvenLanes8x16(x55555555, x55555555));
         assign(xAll9, mk_CatOddLanes8x16 (x44444444, x44444444));
         assign(xAll8, mk_CatEvenLanes8x16(x44444444, x44444444));
         assign(xAll7, mk_CatOddLanes8x16 (x33333333, x33333333));
         assign(xAll6, mk_CatEvenLanes8x16(x33333333, x33333333));
         assign(xAll5, mk_CatOddLanes8x16 (x22222222, x22222222));
         assign(xAll4, mk_CatEvenLanes8x16(x22222222, x22222222));
         assign(xAll3, mk_CatOddLanes8x16 (x11111111, x11111111));
         assign(xAll2, mk_CatEvenLanes8x16(x11111111, x11111111));
         assign(xAll1, mk_CatOddLanes8x16 (x00000000, x00000000));
         assign(xAll0, mk_CatEvenLanes8x16(x00000000, x00000000));
         IRTemp maxFE = newTemp(Ity_V128);
         IRTemp maxDC = newTemp(Ity_V128);
         IRTemp maxBA = newTemp(Ity_V128);
         IRTemp max98 = newTemp(Ity_V128);
         IRTemp max76 = newTemp(Ity_V128);
         IRTemp max54 = newTemp(Ity_V128);
         IRTemp max32 = newTemp(Ity_V128);
         IRTemp max10 = newTemp(Ity_V128);
         assign(maxFE, binop(op, mkexpr(xAllF), mkexpr(xAllE)));
         assign(maxDC, binop(op, mkexpr(xAllD), mkexpr(xAllC)));
         assign(maxBA, binop(op, mkexpr(xAllB), mkexpr(xAllA)));
         assign(max98, binop(op, mkexpr(xAll9), mkexpr(xAll8)));
         assign(max76, binop(op, mkexpr(xAll7), mkexpr(xAll6)));
         assign(max54, binop(op, mkexpr(xAll5), mkexpr(xAll4)));
         assign(max32, binop(op, mkexpr(xAll3), mkexpr(xAll2)));
         assign(max10, binop(op, mkexpr(xAll1), mkexpr(xAll0)));
         IRTemp maxFEDC = newTemp(Ity_V128);
         IRTemp maxBA98 = newTemp(Ity_V128);
         IRTemp max7654 = newTemp(Ity_V128);
         IRTemp max3210 = newTemp(Ity_V128);
         assign(maxFEDC, binop(op, mkexpr(maxFE), mkexpr(maxDC)));
         assign(maxBA98, binop(op, mkexpr(maxBA), mkexpr(max98)));
         assign(max7654, binop(op, mkexpr(max76), mkexpr(max54)));
         assign(max3210, binop(op, mkexpr(max32), mkexpr(max10)));
         IRTemp maxFEDCBA98 = newTemp(Ity_V128);
         IRTemp max76543210 = newTemp(Ity_V128);
         assign(maxFEDCBA98, binop(op, mkexpr(maxFEDC), mkexpr(maxBA98)));
         assign(max76543210, binop(op, mkexpr(max7654), mkexpr(max3210)));
         IRTemp maxAllLanes = newTemp(Ity_V128);
         assign(maxAllLanes, binop(op, mkexpr(maxFEDCBA98),
                                       mkexpr(max76543210)));
         IRTemp res = newTemp(Ity_V128);
         assign(res, unop(Iop_ZeroHI120ofV128, mkexpr(maxAllLanes)));
         return res;
      }
      case Iop_Min16Sx8: case Iop_Min16Ux8:
      case Iop_Max16Sx8: case Iop_Max16Ux8: case Iop_Add16x8: {
         IRTemp x76543210 = src;
         IRTemp x76547654 = newTemp(Ity_V128);
         IRTemp x32103210 = newTemp(Ity_V128);
         assign(x76547654, mk_CatOddLanes64x2 (x76543210, x76543210));
         assign(x32103210, mk_CatEvenLanes64x2(x76543210, x76543210));
         IRTemp x76767676 = newTemp(Ity_V128);
         IRTemp x54545454 = newTemp(Ity_V128);
         IRTemp x32323232 = newTemp(Ity_V128);
         IRTemp x10101010 = newTemp(Ity_V128);
         assign(x76767676, mk_CatOddLanes32x4 (x76547654, x76547654));
         assign(x54545454, mk_CatEvenLanes32x4(x76547654, x76547654));
         assign(x32323232, mk_CatOddLanes32x4 (x32103210, x32103210));
         assign(x10101010, mk_CatEvenLanes32x4(x32103210, x32103210));
         IRTemp x77777777 = newTemp(Ity_V128);
         IRTemp x66666666 = newTemp(Ity_V128);
         IRTemp x55555555 = newTemp(Ity_V128);
         IRTemp x44444444 = newTemp(Ity_V128);
         IRTemp x33333333 = newTemp(Ity_V128);
         IRTemp x22222222 = newTemp(Ity_V128);
         IRTemp x11111111 = newTemp(Ity_V128);
         IRTemp x00000000 = newTemp(Ity_V128);
         assign(x77777777, mk_CatOddLanes16x8 (x76767676, x76767676));
         assign(x66666666, mk_CatEvenLanes16x8(x76767676, x76767676));
         assign(x55555555, mk_CatOddLanes16x8 (x54545454, x54545454));
         assign(x44444444, mk_CatEvenLanes16x8(x54545454, x54545454));
         assign(x33333333, mk_CatOddLanes16x8 (x32323232, x32323232));
         assign(x22222222, mk_CatEvenLanes16x8(x32323232, x32323232));
         assign(x11111111, mk_CatOddLanes16x8 (x10101010, x10101010));
         assign(x00000000, mk_CatEvenLanes16x8(x10101010, x10101010));
         IRTemp max76 = newTemp(Ity_V128);
         IRTemp max54 = newTemp(Ity_V128);
         IRTemp max32 = newTemp(Ity_V128);
         IRTemp max10 = newTemp(Ity_V128);
         assign(max76, binop(op, mkexpr(x77777777), mkexpr(x66666666)));
         assign(max54, binop(op, mkexpr(x55555555), mkexpr(x44444444)));
         assign(max32, binop(op, mkexpr(x33333333), mkexpr(x22222222)));
         assign(max10, binop(op, mkexpr(x11111111), mkexpr(x00000000)));
         IRTemp max7654 = newTemp(Ity_V128);
         IRTemp max3210 = newTemp(Ity_V128);
         assign(max7654, binop(op, mkexpr(max76), mkexpr(max54)));
         assign(max3210, binop(op, mkexpr(max32), mkexpr(max10)));
         IRTemp max76543210 = newTemp(Ity_V128);
         assign(max76543210, binop(op, mkexpr(max7654), mkexpr(max3210)));
         IRTemp res = newTemp(Ity_V128);
         assign(res, unop(Iop_ZeroHI112ofV128, mkexpr(max76543210)));
         return res;
      }
      case Iop_Min32Sx4: case Iop_Min32Ux4:
      case Iop_Max32Sx4: case Iop_Max32Ux4: case Iop_Add32x4: {
         IRTemp x3210 = src;
         IRTemp x3232 = newTemp(Ity_V128);
         IRTemp x1010 = newTemp(Ity_V128);
         assign(x3232, mk_CatOddLanes64x2 (x3210, x3210));
         assign(x1010, mk_CatEvenLanes64x2(x3210, x3210));
         IRTemp x3333 = newTemp(Ity_V128);
         IRTemp x2222 = newTemp(Ity_V128);
         IRTemp x1111 = newTemp(Ity_V128);
         IRTemp x0000 = newTemp(Ity_V128);
         assign(x3333, mk_CatOddLanes32x4 (x3232, x3232));
         assign(x2222, mk_CatEvenLanes32x4(x3232, x3232));
         assign(x1111, mk_CatOddLanes32x4 (x1010, x1010));
         assign(x0000, mk_CatEvenLanes32x4(x1010, x1010));
         IRTemp max32 = newTemp(Ity_V128);
         IRTemp max10 = newTemp(Ity_V128);
         assign(max32, binop(op, mkexpr(x3333), mkexpr(x2222)));
         assign(max10, binop(op, mkexpr(x1111), mkexpr(x0000)));
         IRTemp max3210 = newTemp(Ity_V128);
         assign(max3210, binop(op, mkexpr(max32), mkexpr(max10)));
         IRTemp res = newTemp(Ity_V128);
         assign(res, unop(Iop_ZeroHI96ofV128, mkexpr(max3210)));
         return res;
      }
      default:
         vassert(0);
   }
}


/* Generate IR for TBL and TBX.  This deals with the 128 bit case
   only. */
static IRTemp math_TBL_TBX ( IRTemp tab[4], UInt len, IRTemp src,
                             IRTemp oor_values )
{
   vassert(len >= 0 && len <= 3);

   /* Generate some useful constants as concisely as possible. */
   IRTemp half15 = newTemp(Ity_I64);
   assign(half15, mkU64(0x0F0F0F0F0F0F0F0FULL));
   IRTemp half16 = newTemp(Ity_I64);
   assign(half16, mkU64(0x1010101010101010ULL));

   /* A zero vector */
   IRTemp allZero = newTemp(Ity_V128);
   assign(allZero, mkV128(0x0000));
   /* A vector containing 15 in each 8-bit lane */
   IRTemp all15 = newTemp(Ity_V128);
   assign(all15, binop(Iop_64HLtoV128, mkexpr(half15), mkexpr(half15)));
   /* A vector containing 16 in each 8-bit lane */
   IRTemp all16 = newTemp(Ity_V128);
   assign(all16, binop(Iop_64HLtoV128, mkexpr(half16), mkexpr(half16)));
   /* A vector containing 32 in each 8-bit lane */
   IRTemp all32 = newTemp(Ity_V128);
   assign(all32, binop(Iop_Add8x16, mkexpr(all16), mkexpr(all16)));
   /* A vector containing 48 in each 8-bit lane */
   IRTemp all48 = newTemp(Ity_V128);
   assign(all48, binop(Iop_Add8x16, mkexpr(all16), mkexpr(all32)));
   /* A vector containing 64 in each 8-bit lane */
   IRTemp all64 = newTemp(Ity_V128);
   assign(all64, binop(Iop_Add8x16, mkexpr(all32), mkexpr(all32)));

   /* Group the 16/32/48/64 vectors so as to be indexable. */
   IRTemp allXX[4] = { all16, all32, all48, all64 };

   /* Compute the result for each table vector, with zeroes in places
      where the index values are out of range, and OR them into the
      running vector. */
   IRTemp running_result = newTemp(Ity_V128);
   assign(running_result, mkV128(0));

   UInt tabent;
   for (tabent = 0; tabent <= len; tabent++) {
      vassert(tabent >= 0 && tabent < 4);
      IRTemp bias = newTemp(Ity_V128);
      assign(bias,
             mkexpr(tabent == 0 ? allZero : allXX[tabent-1]));
      IRTemp biased_indices = newTemp(Ity_V128);
      assign(biased_indices,
             binop(Iop_Sub8x16, mkexpr(src), mkexpr(bias)));
      IRTemp valid_mask = newTemp(Ity_V128);
      assign(valid_mask,
             binop(Iop_CmpGT8Ux16, mkexpr(all16), mkexpr(biased_indices)));
      IRTemp safe_biased_indices = newTemp(Ity_V128);
      assign(safe_biased_indices,
             binop(Iop_AndV128, mkexpr(biased_indices), mkexpr(all15)));
      IRTemp results_or_junk = newTemp(Ity_V128);
      assign(results_or_junk,
             binop(Iop_Perm8x16, mkexpr(tab[tabent]),
                                 mkexpr(safe_biased_indices)));
      IRTemp results_or_zero = newTemp(Ity_V128);
      assign(results_or_zero,
             binop(Iop_AndV128, mkexpr(results_or_junk), mkexpr(valid_mask)));
      /* And OR that into the running result. */
      IRTemp tmp = newTemp(Ity_V128);
      assign(tmp, binop(Iop_OrV128, mkexpr(results_or_zero),
                        mkexpr(running_result)));
      running_result = tmp;
   }

   /* So now running_result holds the overall result where the indices
      are in range, and zero in out-of-range lanes.  Now we need to
      compute an overall validity mask and use this to copy in the
      lanes in the oor_values for out of range indices.  This is
      unnecessary for TBL but will get folded out by iropt, so we lean
      on that and generate the same code for TBL and TBX here. */
   IRTemp overall_valid_mask = newTemp(Ity_V128);
   assign(overall_valid_mask,
          binop(Iop_CmpGT8Ux16, mkexpr(allXX[len]), mkexpr(src)));
   IRTemp result = newTemp(Ity_V128);
   assign(result,
          binop(Iop_OrV128,
                mkexpr(running_result),
                binop(Iop_AndV128,
                      mkexpr(oor_values),
                      unop(Iop_NotV128, mkexpr(overall_valid_mask)))));
   return result;      
}


/* Let |argL| and |argR| be V128 values, and let |opI64x2toV128| be
   an op which takes two I64s and produces a V128.  That is, a widening
   operator.  Generate IR which applies |opI64x2toV128| to either the
   lower (if |is2| is False) or upper (if |is2| is True) halves of
   |argL| and |argR|, and return the value in a new IRTemp.
*/
static
IRTemp math_BINARY_WIDENING_V128 ( Bool is2, IROp opI64x2toV128,
                                   IRExpr* argL, IRExpr* argR )
{
   IRTemp res   = newTemp(Ity_V128);
   IROp   slice = is2 ? Iop_V128HIto64 : Iop_V128to64;
   assign(res, binop(opI64x2toV128, unop(slice, argL),
                                    unop(slice, argR)));
   return res;
}


/* Let |new64| be a V128 in which only the lower 64 bits are interesting,
   and the upper can contain any value -- it is ignored.  If |is2| is False,
   generate IR to put |new64| in the lower half of vector reg |dd| and zero
   the upper half.  If |is2| is True, generate IR to put |new64| in the upper
   half of vector reg |dd| and leave the lower half unchanged.  This
   simulates the behaviour of the "foo/foo2" instructions in which the 
   destination is half the width of sources, for example addhn/addhn2.
*/
static
void putLO64andZUorPutHI64 ( Bool is2, UInt dd, IRTemp new64 )
{
   if (is2) {
      /* Get the old contents of Vdd, zero the upper half, and replace
         it with 'x'. */
      IRTemp t_zero_oldLO = newTemp(Ity_V128);
      assign(t_zero_oldLO, unop(Iop_ZeroHI64ofV128, getQReg128(dd)));
      IRTemp t_newHI_zero = newTemp(Ity_V128);
      assign(t_newHI_zero, binop(Iop_InterleaveLO64x2, mkexpr(new64),
                                                       mkV128(0x0000)));
      IRTemp res = newTemp(Ity_V128);
      assign(res, binop(Iop_OrV128, mkexpr(t_zero_oldLO),
                                    mkexpr(t_newHI_zero)));
      putQReg128(dd, mkexpr(res));
   } else {
      /* This is simple. */
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(new64)));
   }
}


static
Bool dis_AdvSIMD_EXT(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  29     23  21 20 15 14   10 9 4
      0 q 101110 op2 0  m  0  imm4 0  n d
      Decode fields: op2
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(29,24) != BITS6(1,0,1,1,1,0)
       || INSN(21,21) != 0 || INSN(15,15) != 0 || INSN(10,10) != 0) {
      return False;
   }
   UInt bitQ = INSN(30,30);
   UInt op2  = INSN(23,22);
   UInt mm   = INSN(20,16);
   UInt imm4 = INSN(14,11);
   UInt nn   = INSN(9,5);
   UInt dd   = INSN(4,0);

   if (op2 == BITS2(0,0)) {
      /* -------- 00: EXT 16b_16b_16b, 8b_8b_8b -------- */
      IRTemp sHi = newTemp(Ity_V128);
      IRTemp sLo = newTemp(Ity_V128);
      IRTemp res = newTemp(Ity_V128);
      assign(sHi, getQReg128(mm));
      assign(sLo, getQReg128(nn));
      if (bitQ == 1) {
         if (imm4 == 0) {
            assign(res, mkexpr(sLo));
         } else {
            vassert(imm4 <= 15);
            assign(res,
                   binop(Iop_OrV128,
                         binop(Iop_ShlV128, mkexpr(sHi), mkU8(8 * (16-imm4))),
                         binop(Iop_ShrV128, mkexpr(sLo), mkU8(8 * imm4))));
         }
         putQReg128(dd, mkexpr(res));
         DIP("ext v%u.16b, v%u.16b, v%u.16b, #%u\n", dd, nn, mm, imm4);
      } else {
         if (imm4 >= 8) return False;
         if (imm4 == 0) {
            assign(res, mkexpr(sLo));
         } else {
           assign(res,
                  binop(Iop_ShrV128,
                        binop(Iop_InterleaveLO64x2, mkexpr(sHi), mkexpr(sLo)),
                        mkU8(8 * imm4)));
         }
         putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
         DIP("ext v%u.8b, v%u.8b, v%u.8b, #%u\n", dd, nn, mm, imm4);
      }
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_TBL_TBX(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  29     23  21 20 15 14  12 11 9 4
      0 q 001110 op2 0  m  0  len op 00 n d
      Decode fields: op2,len,op
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(29,24) != BITS6(0,0,1,1,1,0)
       || INSN(21,21) != 0
       || INSN(15,15) != 0
       || INSN(11,10) != BITS2(0,0)) {
      return False;
   }
   UInt bitQ  = INSN(30,30);
   UInt op2   = INSN(23,22);
   UInt mm    = INSN(20,16);
   UInt len   = INSN(14,13);
   UInt bitOP = INSN(12,12);
   UInt nn    = INSN(9,5);
   UInt dd    = INSN(4,0);

   if (op2 == X00) {
      /* -------- 00,xx,0 TBL, xx register table -------- */
      /* -------- 00,xx,1 TBX, xx register table -------- */
      /* 31  28        20 15 14  12  9 4
         0q0 01110 000 m  0  len 000 n d  TBL Vd.Ta, {Vn .. V(n+len)%32}, Vm.Ta
         0q0 01110 000 m  0  len 100 n d  TBX Vd.Ta, {Vn .. V(n+len)%32}, Vm.Ta
         where Ta = 16b(q=1) or 8b(q=0)
      */
      Bool isQ   = bitQ == 1;
      Bool isTBX = bitOP == 1;
      /* The out-of-range values to use. */
      IRTemp oor_values = newTemp(Ity_V128);
      assign(oor_values, isTBX ? getQReg128(dd) : mkV128(0));
      /* src value */
      IRTemp src = newTemp(Ity_V128);
      assign(src, getQReg128(mm));
      /* The table values */
      IRTemp tab[4];
      UInt   i;
      for (i = 0; i <= len; i++) {
         vassert(i < 4);
         tab[i] = newTemp(Ity_V128);
         assign(tab[i], getQReg128((nn + i) % 32));
      }
      IRTemp res = math_TBL_TBX(tab, len, src, oor_values);
      putQReg128(dd, isQ ? mkexpr(res)
                         : unop(Iop_ZeroHI64ofV128, mkexpr(res)) );
      const HChar* Ta = isQ ? "16b" : "8b";
      const HChar* nm = isTBX ? "tbx" : "tbl";
      DIP("%s %s.%s, {v%d.16b .. v%d.16b}, %s.%s\n",
          nm, nameQReg128(dd), Ta, nn, (nn + len) % 32, nameQReg128(mm), Ta);
      return True;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_ZIP_UZP_TRN(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_across_lanes(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31    28    23   21    16     11 9 4
      0 q u 01110 size 11000 opcode 10 n d
      Decode fields: u,size,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,24) != BITS5(0,1,1,1,0)
       || INSN(21,17) != BITS5(1,1,0,0,0) || INSN(11,10) != BITS2(1,0)) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt opcode = INSN(16,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);

   UInt ix = 0;
   /**/ if (opcode == BITS5(0,1,0,1,0)) { ix = bitU == 0 ? 1 : 2; }
   else if (opcode == BITS5(1,1,0,1,0)) { ix = bitU == 0 ? 3 : 4; }
   else if (opcode == BITS5(1,1,0,1,1) && bitU == 0) { ix = 5; }
   /**/   
   if (ix != 0) {
      /* -------- 0,xx,01010: SMAXV -------- (1) */
      /* -------- 1,xx,01010: UMAXV -------- (2) */
      /* -------- 0,xx,11010: SMINV -------- (3) */
      /* -------- 1,xx,11010: UMINV -------- (4) */
      /* -------- 0,xx,11011: ADDV  -------- (5) */
      vassert(ix >= 1 && ix <= 5);
      if (size == X11) return False; // 1d,2d cases not allowed
      if (size == X10 && bitQ == 0) return False; // 2s case not allowed
      const IROp opMAXS[3]
         = { Iop_Max8Sx16, Iop_Max16Sx8, Iop_Max32Sx4 };
      const IROp opMAXU[3]
         = { Iop_Max8Ux16, Iop_Max16Ux8, Iop_Max32Ux4 };
      const IROp opMINS[3]
         = { Iop_Min8Sx16, Iop_Min16Sx8, Iop_Min32Sx4 };
      const IROp opMINU[3]
         = { Iop_Min8Ux16, Iop_Min16Ux8, Iop_Min32Ux4 };
      const IROp opADD[3]
         = { Iop_Add8x16,  Iop_Add16x8,  Iop_Add32x4 };
      vassert(size < 3);
      IROp op = Iop_INVALID;
      const HChar* nm = NULL;
      switch (ix) {
         case 1: op = opMAXS[size]; nm = "smaxv"; break;
         case 2: op = opMAXU[size]; nm = "umaxv"; break;
         case 3: op = opMINS[size]; nm = "sminv"; break;
         case 4: op = opMINU[size]; nm = "uminv"; break;
         case 5: op = opADD[size];  nm = "addv";  break;
         default: vassert(0);
      }
      vassert(op != Iop_INVALID && nm != NULL);
      IRTemp tN1 = newTemp(Ity_V128);
      assign(tN1, getQReg128(nn));
      /* If Q == 0, we're just folding lanes in the lower half of
         the value.  In which case, copy the lower half of the
         source into the upper half, so we can then treat it the
         same as the full width case.  Except for the addition case,
         in which we have to zero out the upper half. */
      IRTemp tN2 = newTemp(Ity_V128);
      assign(tN2, bitQ == 0 
                     ? (ix == 5 ? unop(Iop_ZeroHI64ofV128, mkexpr(tN1))
                                : mk_CatEvenLanes64x2(tN1,tN1))
                     : mkexpr(tN1));
      IRTemp res = math_MINMAXV(tN2, op);
      if (res == IRTemp_INVALID)
         return False; /* means math_MINMAXV
                          doesn't handle this case yet */
      putQReg128(dd, mkexpr(res));
      const IRType tys[3] = { Ity_I8, Ity_I16, Ity_I32 };
      IRType laneTy = tys[size];
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s, %s.%s\n", nm,
          nameQRegLO(dd, laneTy), nameQReg128(nn), arr);
      return True;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_copy(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31     28       20   15 14   10 9 4
      0 q op 01110000 imm5 0  imm4 1  n d
      Decode fields: q,op,imm4
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,21) != BITS8(0,1,1,1,0,0,0,0)
       || INSN(15,15) != 0 || INSN(10,10) != 1) {
      return False;
   }
   UInt bitQ  = INSN(30,30);
   UInt bitOP = INSN(29,29);
   UInt imm5  = INSN(20,16);
   UInt imm4  = INSN(14,11);
   UInt nn    = INSN(9,5);
   UInt dd    = INSN(4,0);

   /* -------- x,0,0000: DUP (element, vector) -------- */
   /* 31  28       20   15     9 4
      0q0 01110000 imm5 000001 n d  DUP Vd.T, Vn.Ts[index]
   */
   if (bitOP == 0 && imm4 == BITS4(0,0,0,0)) {
      Bool   isQ  = bitQ == 1;
      IRTemp w0   = newTemp(Ity_I64);
      const HChar* arT  = "??";
      const HChar* arTs = "??";
      IRType laneTy = Ity_INVALID;
      UInt   laneNo = 16; /* invalid */
      if (imm5 & 1) {
         arT    = isQ ? "16b" : "8b";
         arTs   = "b";
         laneNo = (imm5 >> 1) & 15;
         laneTy = Ity_I8;
         assign(w0, unop(Iop_8Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if (imm5 & 2) {
         arT    = isQ ? "8h" : "4h";
         arTs   = "h";
         laneNo = (imm5 >> 2) & 7;
         laneTy = Ity_I16;
         assign(w0, unop(Iop_16Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if (imm5 & 4) {
         arT    = isQ ? "4s" : "2s";
         arTs   = "s";
         laneNo = (imm5 >> 3) & 3;
         laneTy = Ity_I32;
         assign(w0, unop(Iop_32Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if ((imm5 & 8) && isQ) {
         arT  = "2d";
         arTs = "d";
         laneNo = (imm5 >> 4) & 1;
         laneTy = Ity_I64;
         assign(w0, getQRegLane(nn, laneNo, laneTy));
      }
      else {
         /* invalid; leave laneTy unchanged. */
      }
      /* */
      if (laneTy != Ity_INVALID) {
         vassert(laneNo < 16);
         IRTemp w1 = math_DUP_TO_64(w0, laneTy);
         putQReg128(dd, binop(Iop_64HLtoV128,
                              isQ ? mkexpr(w1) : mkU64(0), mkexpr(w1)));
         DIP("dup %s.%s, %s.%s[%u]\n",
             nameQReg128(dd), arT, nameQReg128(nn), arTs, laneNo);
         return True;
      }
      /* invalid */
      return False;
   }

   /* -------- x,0,0001: DUP (general, vector) -------- */
   /* 31  28       20   15       9 4
      0q0 01110000 imm5 0 0001 1 n d  DUP Vd.T, Rn
      Q=0 writes 64, Q=1 writes 128
      imm5: xxxx1  8B(q=0)      or 16b(q=1),     R=W
            xxx10  4H(q=0)      or 8H(q=1),      R=W
            xx100  2S(q=0)      or 4S(q=1),      R=W
            x1000  Invalid(q=0) or 2D(q=1),      R=X
            x0000  Invalid(q=0) or Invalid(q=1)
      Require op=0, imm4=0001
   */
   if (bitOP == 0 && imm4 == BITS4(0,0,0,1)) {
      Bool   isQ = bitQ == 1;
      IRTemp w0  = newTemp(Ity_I64);
      const HChar* arT = "??";
      IRType laneTy = Ity_INVALID;
      if (imm5 & 1) {
         arT    = isQ ? "16b" : "8b";
         laneTy = Ity_I8;
         assign(w0, unop(Iop_8Uto64, unop(Iop_64to8, getIReg64orZR(nn))));
      }
      else if (imm5 & 2) {
         arT    = isQ ? "8h" : "4h";
         laneTy = Ity_I16;
         assign(w0, unop(Iop_16Uto64, unop(Iop_64to16, getIReg64orZR(nn))));
      }
      else if (imm5 & 4) {
         arT    = isQ ? "4s" : "2s";
         laneTy = Ity_I32;
         assign(w0, unop(Iop_32Uto64, unop(Iop_64to32, getIReg64orZR(nn))));
      }
      else if ((imm5 & 8) && isQ) {
         arT    = "2d";
         laneTy = Ity_I64;
         assign(w0, getIReg64orZR(nn));
      }
      else {
         /* invalid; leave laneTy unchanged. */
      }
      /* */
      if (laneTy != Ity_INVALID) {
         IRTemp w1 = math_DUP_TO_64(w0, laneTy);
         putQReg128(dd, binop(Iop_64HLtoV128,
                              isQ ? mkexpr(w1) : mkU64(0), mkexpr(w1)));
         DIP("dup %s.%s, %s\n",
             nameQReg128(dd), arT, nameIRegOrZR(laneTy == Ity_I64, nn));
         return True;
      }
      /* invalid */
      return False;
   }

   /* -------- 1,0,0011: INS (general) -------- */
   /* 31  28       20   15     9 4
      010 01110000 imm5 000111 n d  INS Vd.Ts[ix], Rn
      where Ts,ix = case imm5 of xxxx1 -> B, xxxx
                                 xxx10 -> H, xxx
                                 xx100 -> S, xx
                                 x1000 -> D, x
   */
   if (bitQ == 1 && bitOP == 0 && imm4 == BITS4(0,0,1,1)) {
      HChar   ts     = '?';
      UInt    laneNo = 16;
      IRExpr* src    = NULL;
      if (imm5 & 1) {
         src    = unop(Iop_64to8, getIReg64orZR(nn));
         laneNo = (imm5 >> 1) & 15;
         ts     = 'b';
      }
      else if (imm5 & 2) {
         src    = unop(Iop_64to16, getIReg64orZR(nn));
         laneNo = (imm5 >> 2) & 7;
         ts     = 'h';
      }
      else if (imm5 & 4) {
         src    = unop(Iop_64to32, getIReg64orZR(nn));
         laneNo = (imm5 >> 3) & 3;
         ts     = 's';
      }
      else if (imm5 & 8) {
         src    = getIReg64orZR(nn);
         laneNo = (imm5 >> 4) & 1;
         ts     = 'd';
      }
      /* */
      if (src) {
         vassert(laneNo < 16);
         putQRegLane(dd, laneNo, src);
         DIP("ins %s.%c[%u], %s\n",
             nameQReg128(dd), ts, laneNo, nameIReg64orZR(nn));
         return True;
      }
      /* invalid */
      return False;
   }

   /* -------- x,0,0101: SMOV -------- */
   /* -------- x,0,0111: UMOV -------- */
   /* 31  28        20   15     9 4
      0q0 01110 000 imm5 001111 n d  UMOV Xd/Wd, Vn.Ts[index]
      0q0 01110 000 imm5 001011 n d  SMOV Xd/Wd, Vn.Ts[index]
      dest is Xd when q==1, Wd when q==0
      UMOV:
         Ts,index,ops = case q:imm5 of
                          0:xxxx1 -> B, xxxx, 8Uto64
                          1:xxxx1 -> invalid
                          0:xxx10 -> H, xxx,  16Uto64
                          1:xxx10 -> invalid
                          0:xx100 -> S, xx,   32Uto64
                          1:xx100 -> invalid
                          1:x1000 -> D, x,    copy64
                          other   -> invalid
      SMOV:
         Ts,index,ops = case q:imm5 of
                          0:xxxx1 -> B, xxxx, (32Uto64 . 8Sto32)
                          1:xxxx1 -> B, xxxx, 8Sto64
                          0:xxx10 -> H, xxx,  (32Uto64 . 16Sto32)
                          1:xxx10 -> H, xxx,  16Sto64
                          0:xx100 -> invalid
                          1:xx100 -> S, xx,   32Sto64
                          1:x1000 -> invalid
                          other   -> invalid
   */
   if (bitOP == 0 && (imm4 == BITS4(0,1,0,1) || imm4 == BITS4(0,1,1,1))) {
      Bool isU  = (imm4 & 2) == 2;
      const HChar* arTs = "??";
      UInt    laneNo = 16; /* invalid */
      // Setting 'res' to non-NULL determines valid/invalid
      IRExpr* res    = NULL;
      if (!bitQ && (imm5 & 1)) { // 0:xxxx1
         laneNo = (imm5 >> 1) & 15;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I8);
         res = isU ? unop(Iop_8Uto64, lane)
                   : unop(Iop_32Uto64, unop(Iop_8Sto32, lane));
         arTs = "b";
      }
      else if (bitQ && (imm5 & 1)) { // 1:xxxx1
         laneNo = (imm5 >> 1) & 15;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I8);
         res = isU ? NULL
                   : unop(Iop_8Sto64, lane);
         arTs = "b";
      }
      else if (!bitQ && (imm5 & 2)) { // 0:xxx10
         laneNo = (imm5 >> 2) & 7;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I16);
         res = isU ? unop(Iop_16Uto64, lane)
                   : unop(Iop_32Uto64, unop(Iop_16Sto32, lane));
         arTs = "h";
      }
      else if (bitQ && (imm5 & 2)) { // 1:xxx10
         laneNo = (imm5 >> 2) & 7;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I16);
         res = isU ? NULL
                   : unop(Iop_16Sto64, lane);
         arTs = "h";
      }
      else if (!bitQ && (imm5 & 4)) { // 0:xx100
         laneNo = (imm5 >> 3) & 3;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I32);
         res = isU ? unop(Iop_32Uto64, lane)
                   : NULL;
         arTs = "s";
      }
      else if (bitQ && (imm5 & 4)) { // 1:xxx10
         laneNo = (imm5 >> 3) & 3;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I32);
         res = isU ? NULL
                   : unop(Iop_32Sto64, lane);
         arTs = "s";
      }
      else if (bitQ && (imm5 & 8)) { // 1:x1000
         laneNo = (imm5 >> 4) & 1;
         IRExpr* lane = getQRegLane(nn, laneNo, Ity_I64);
         res = isU ? lane
                   : NULL;
         arTs = "d";
      }
      /* */
      if (res) {
         vassert(laneNo < 16);
         putIReg64orZR(dd, res);
         DIP("%cmov %s, %s.%s[%u]\n", isU ? 'u' : 's',
             nameIRegOrZR(bitQ == 1, dd),
             nameQReg128(nn), arTs, laneNo);
         return True;
      }
      /* invalid */
      return False;
   }

   /* -------- 1,1,xxxx: INS (element) -------- */
   /* 31  28       20     14   9 4
      011 01110000 imm5 0 imm4 n d  INS Vd.Ts[ix1], Vn.Ts[ix2]
      where Ts,ix1,ix2
               = case imm5 of xxxx1 -> B, xxxx, imm4[3:0]
                              xxx10 -> H, xxx,  imm4[3:1]
                              xx100 -> S, xx,   imm4[3:2]
                              x1000 -> D, x,    imm4[3:3]
   */
   if (bitQ == 1 && bitOP == 1) {
      HChar   ts  = '?';
      IRType  ity = Ity_INVALID;
      UInt    ix1 = 16;
      UInt    ix2 = 16;
      if (imm5 & 1) {
         ts  = 'b';
         ity = Ity_I8;
         ix1 = (imm5 >> 1) & 15;
         ix2 = (imm4 >> 0) & 15;
      }
      else if (imm5 & 2) {
         ts  = 'h';
         ity = Ity_I16;
         ix1 = (imm5 >> 2) & 7;
         ix2 = (imm4 >> 1) & 7;
      }
      else if (imm5 & 4) {
         ts  = 's';
         ity = Ity_I32;
         ix1 = (imm5 >> 3) & 3;
         ix2 = (imm4 >> 2) & 3;
      }
      else if (imm5 & 8) {
         ts  = 'd';
         ity = Ity_I64;
         ix1 = (imm5 >> 4) & 1;
         ix2 = (imm4 >> 3) & 1;
      }
      /* */
      if (ity != Ity_INVALID) {
         vassert(ix1 < 16);
         vassert(ix2 < 16);
         putQRegLane(dd, ix1, getQRegLane(nn, ix2, ity));
         DIP("ins %s.%c[%u], %s.%c[%u]\n",
             nameQReg128(dd), ts, ix1, nameQReg128(nn), ts, ix2);
         return True;
      }
      /* invalid */
      return False;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_modified_immediate(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31    28          18  15    11 9     4
      0q op 01111 00000 abc cmode 01 defgh d
      Decode fields: q,op,cmode
      Bit 11 is really "o2", but it is always zero.
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,19) != BITS10(0,1,1,1,1,0,0,0,0,0)
       || INSN(11,10) != BITS2(0,1)) {
      return False;
   }
   UInt bitQ     = INSN(30,30);
   UInt bitOP    = INSN(29,29);
   UInt cmode    = INSN(15,12);
   UInt abcdefgh = (INSN(18,16) << 5) | INSN(9,5);
   UInt dd       = INSN(4,0);

   ULong imm64lo  = 0;
   UInt  op_cmode = (bitOP << 4) | cmode;
   Bool  ok       = False;
   Bool  isORR    = False;
   Bool  isBIC    = False;
   Bool  isMOV    = False;
   Bool  isMVN    = False;
   Bool  isFMOV   = False;
   switch (op_cmode) {
      /* -------- x,0,0000 MOVI 32-bit shifted imm -------- */
      /* -------- x,0,0010 MOVI 32-bit shifted imm -------- */
      /* -------- x,0,0100 MOVI 32-bit shifted imm -------- */
      /* -------- x,0,0110 MOVI 32-bit shifted imm -------- */
      case BITS5(0,0,0,0,0): case BITS5(0,0,0,1,0):
      case BITS5(0,0,1,0,0): case BITS5(0,0,1,1,0): // 0:0xx0
         ok = True; isMOV = True; break;

      /* -------- x,0,0001 ORR (vector, immediate) 32-bit -------- */
      /* -------- x,0,0011 ORR (vector, immediate) 32-bit -------- */
      /* -------- x,0,0101 ORR (vector, immediate) 32-bit -------- */
      /* -------- x,0,0111 ORR (vector, immediate) 32-bit -------- */
      case BITS5(0,0,0,0,1): case BITS5(0,0,0,1,1):
      case BITS5(0,0,1,0,1): case BITS5(0,0,1,1,1): // 0:0xx1
         ok = True; isORR = True; break;

      /* -------- x,0,1000 MOVI 16-bit shifted imm -------- */
      /* -------- x,0,1010 MOVI 16-bit shifted imm -------- */
      case BITS5(0,1,0,0,0): case BITS5(0,1,0,1,0): // 0:10x0
         ok = True; isMOV = True; break;

      /* -------- x,0,1001 ORR (vector, immediate) 16-bit -------- */
      /* -------- x,0,1011 ORR (vector, immediate) 16-bit -------- */
      case BITS5(0,1,0,0,1): case BITS5(0,1,0,1,1): // 0:10x1
         ok = True; isORR = True; break;

      /* -------- x,0,1100 MOVI 32-bit shifting ones -------- */
      /* -------- x,0,1101 MOVI 32-bit shifting ones -------- */
      case BITS5(0,1,1,0,0): case BITS5(0,1,1,0,1): // 0:110x
         ok = True; isMOV = True; break;

      /* -------- x,0,1110 MOVI 8-bit -------- */
      case BITS5(0,1,1,1,0):
         ok = True; isMOV = True; break;

      /* FMOV (vector, immediate, single precision) */

      /* -------- x,1,0000 MVNI 32-bit shifted imm -------- */
      /* -------- x,1,0010 MVNI 32-bit shifted imm  -------- */
      /* -------- x,1,0100 MVNI 32-bit shifted imm  -------- */
      /* -------- x,1,0110 MVNI 32-bit shifted imm  -------- */
      case BITS5(1,0,0,0,0): case BITS5(1,0,0,1,0):
      case BITS5(1,0,1,0,0): case BITS5(1,0,1,1,0): // 1:0xx0
         ok = True; isMVN = True; break;

      /* -------- x,1,0001 BIC (vector, immediate) 32-bit -------- */
      /* -------- x,1,0011 BIC (vector, immediate) 32-bit -------- */
      /* -------- x,1,0101 BIC (vector, immediate) 32-bit -------- */
      /* -------- x,1,0111 BIC (vector, immediate) 32-bit -------- */
      case BITS5(1,0,0,0,1): case BITS5(1,0,0,1,1):
      case BITS5(1,0,1,0,1): case BITS5(1,0,1,1,1): // 1:0xx1
         ok = True; isBIC = True; break;

      /* -------- x,1,1000 MVNI 16-bit shifted imm -------- */
      /* -------- x,1,1010 MVNI 16-bit shifted imm -------- */
      case BITS5(1,1,0,0,0): case BITS5(1,1,0,1,0): // 1:10x0
         ok = True; isMVN = True; break;

      /* -------- x,1,1001 BIC (vector, immediate) 16-bit -------- */
      /* -------- x,1,1011 BIC (vector, immediate) 16-bit -------- */
      case BITS5(1,1,0,0,1): case BITS5(1,1,0,1,1): // 1:10x1
         ok = True; isBIC = True; break;

      /* -------- x,1,1100 MVNI 32-bit shifting ones -------- */
      /* -------- x,1,1101 MVNI 32-bit shifting ones -------- */
      case BITS5(1,1,1,0,0): case BITS5(1,1,1,0,1): // 1:110x
         ok = True; isMVN = True; break;

      /* -------- 0,1,1110 MOVI 64-bit scalar -------- */
      /* -------- 1,1,1110 MOVI 64-bit vector -------- */
      case BITS5(1,1,1,1,0):
         ok = True; isMOV = True; break;

      /* -------- 1,1,1111 FMOV (vector, immediate) -------- */
      case BITS5(1,1,1,1,1): // 1:1111
         ok = bitQ == 1; isFMOV = True; break;

      default:
        break;
   }
   if (ok) {
      vassert(1 == (isMOV ? 1 : 0) + (isMVN ? 1 : 0)
                   + (isORR ? 1 : 0) + (isBIC ? 1 : 0) + (isFMOV ? 1 : 0));
      ok = AdvSIMDExpandImm(&imm64lo, bitOP, cmode, abcdefgh);
   }
   if (ok) {
      if (isORR || isBIC) {
         ULong inv
            = isORR ? 0ULL : ~0ULL;
         IRExpr* immV128
            = binop(Iop_64HLtoV128, mkU64(inv ^ imm64lo), mkU64(inv ^ imm64lo));
         IRExpr* res
            = binop(isORR ? Iop_OrV128 : Iop_AndV128, getQReg128(dd), immV128);
         putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, res) : res);
         const HChar* nm = isORR ? "orr" : "bic";
         if (bitQ == 0) {
            putQReg128(dd, unop(Iop_ZeroHI64ofV128, res));
            DIP("%s %s.1d, %016llx\n", nm, nameQReg128(dd), imm64lo);
         } else {
            putQReg128(dd, res);
            DIP("%s %s.2d, #0x%016llx'%016llx\n", nm,
                nameQReg128(dd), imm64lo, imm64lo);
         }
      } 
      else if (isMOV || isMVN || isFMOV) {
         if (isMVN) imm64lo = ~imm64lo;
         ULong   imm64hi = bitQ == 0  ? 0  :  imm64lo;
         IRExpr* immV128 = binop(Iop_64HLtoV128, mkU64(imm64hi), mkU64(imm64lo));
         putQReg128(dd, immV128);
         DIP("mov %s, #0x%016llx'%016llx\n", nameQReg128(dd), imm64hi, imm64lo);
      }
      return True;
   }
   /* else fall through */

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_copy(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31    28       20   15 14   10 9 4
      01 op 11110000 imm5 0  imm4 1  n d
      Decode fields: op,imm4
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,30) != BITS2(0,1)
       || INSN(28,21) != BITS8(1,1,1,1,0,0,0,0)
       || INSN(15,15) != 0 || INSN(10,10) != 1) {
      return False;
   }
   UInt bitOP = INSN(29,29);
   UInt imm5  = INSN(20,16);
   UInt imm4  = INSN(14,11);
   UInt nn    = INSN(9,5);
   UInt dd    = INSN(4,0);

   if (bitOP == 0 && imm4 == BITS4(0,0,0,0)) {
      /* -------- 0,0000 DUP (element, scalar) -------- */
      IRTemp w0     = newTemp(Ity_I64);
      const HChar* arTs = "??";
      IRType laneTy = Ity_INVALID;
      UInt   laneNo = 16; /* invalid */
      if (imm5 & 1) {
         arTs   = "b";
         laneNo = (imm5 >> 1) & 15;
         laneTy = Ity_I8;
         assign(w0, unop(Iop_8Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if (imm5 & 2) {
         arTs   = "h";
         laneNo = (imm5 >> 2) & 7;
         laneTy = Ity_I16;
         assign(w0, unop(Iop_16Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if (imm5 & 4) {
         arTs   = "s";
         laneNo = (imm5 >> 3) & 3;
         laneTy = Ity_I32;
         assign(w0, unop(Iop_32Uto64, getQRegLane(nn, laneNo, laneTy)));
      }
      else if (imm5 & 8) {
         arTs   = "d";
         laneNo = (imm5 >> 4) & 1;
         laneTy = Ity_I64;
         assign(w0, getQRegLane(nn, laneNo, laneTy));
      }
      else {
         /* invalid; leave laneTy unchanged. */
      }
      /* */
      if (laneTy != Ity_INVALID) {
         vassert(laneNo < 16);
         putQReg128(dd, binop(Iop_64HLtoV128, mkU64(0), mkexpr(w0)));
         DIP("dup %s, %s.%s[%u]\n",
             nameQRegLO(dd, laneTy), nameQReg128(nn), arTs, laneNo);
         return True;
      }
      /* else fall through */
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_pairwise(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31   28    23 21    16     11 9 4
      01 u 11110 sz 11000 opcode 10 n d
      Decode fields: u,sz,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,30) != BITS2(0,1)
       || INSN(28,24) != BITS5(1,1,1,1,0)
       || INSN(21,17) != BITS5(1,1,0,0,0)
       || INSN(11,10) != BITS2(1,0)) {
      return False;
   }
   UInt bitU   = INSN(29,29);
   UInt sz     = INSN(23,22);
   UInt opcode = INSN(16,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);

   if (bitU == 0 && sz == X11 && opcode == BITS5(1,1,0,1,1)) {
      /* -------- 0,11,11011 ADDP d_2d -------- */
      IRTemp xy = newTemp(Ity_V128);
      IRTemp xx = newTemp(Ity_V128);
      assign(xy, getQReg128(nn));
      assign(xx, binop(Iop_InterleaveHI64x2, mkexpr(xy), mkexpr(xy)));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128,
                          binop(Iop_Add64x2, mkexpr(xy), mkexpr(xx))));
      DIP("addp d%u, %s.2d\n", dd, nameQReg128(nn));
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_shift_by_imm(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31   28     22   18   15     10 9 4
      01 u 111110 immh immb opcode 1  n d
      Decode fields: u,immh,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,30) != BITS2(0,1)
       || INSN(28,23) != BITS6(1,1,1,1,1,0) || INSN(10,10) != 1) {
      return False;
   }
   UInt bitU   = INSN(29,29);
   UInt immh   = INSN(22,19);
   UInt immb   = INSN(18,16);
   UInt opcode = INSN(15,11);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   UInt immhb  = (immh << 3) | immb;

   if (bitU == 1 && (immh & 8) == 8 && opcode == BITS5(0,0,0,0,0)) {
      /* -------- 1,1xxx,00000 SHR d_d_#imm -------- */
      UInt sh = 128 - immhb;
      vassert(sh >= 1 && sh <= 64);
      /* Don't generate an out of range IR shift */
      putQReg128(dd, sh == 64
                        ? mkV128(0x0000)
                        : unop(Iop_ZeroHI64ofV128,
                               binop(Iop_ShrN64x2, getQReg128(nn), mkU8(sh))));
      DIP("shr d%u, d%u, #%u\n", dd, nn, sh);
      return True;
   }

   if (bitU == 0 && (immh & 8) == 8 && opcode == BITS5(0,1,0,1,0)) {
      /* -------- 0,1xxx,01010 SHL d_d_#imm -------- */
      UInt sh = immhb - 64;
      vassert(sh >= 0 && sh < 64);
      putQReg128(dd, unop(Iop_ZeroHI64ofV128,
                          binop(Iop_ShlN64x2, getQReg128(nn), mkU8(sh))));
      DIP("shl d%u, d%u, #%u\n", dd, nn, sh);
      return True;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_three_different(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_three_same(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 29 28    23   21 20 15     10 9 4
      01 U  11110 size 1  m  opcode 1  n d
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,30) != BITS2(0,1)
       || INSN(28,24) != BITS5(1,1,1,1,0)
       || INSN(21,21) != 1
       || INSN(10,10) != 1) {
      return False;
   }
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt mm     = INSN(20,16);
   UInt opcode = INSN(15,11);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);

   if (size == X11 && opcode == BITS5(0,0,1,1,0)) {
      /* -------- 0,11,00110 CMGT d_d_d -------- */ // >s
      /* -------- 1,11,00110 CMHI d_d_d -------- */ // >u
      Bool    isGT = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isGT ? binop(Iop_CmpGT64Sx2, argL, argR)
                  : binop(Iop_CmpGT64Ux2, argL, argR));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      DIP("%s %s, %s, %s\n",isGT ? "cmgt" : "cmhi",
          nameQRegLO(dd, Ity_I64),
          nameQRegLO(nn, Ity_I64), nameQRegLO(mm, Ity_I64));
      return True;
   }

   if (size == X11 && opcode == BITS5(0,0,1,1,1)) {
      /* -------- 0,11,00111 CMGE d_d_d -------- */ // >=s
      /* -------- 1,11,00111 CMHS d_d_d -------- */ // >=u
      Bool    isGE = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isGE ? unop(Iop_NotV128, binop(Iop_CmpGT64Sx2, argR, argL))
                  : unop(Iop_NotV128, binop(Iop_CmpGT64Ux2, argR, argL)));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      DIP("%s %s, %s, %s\n", isGE ? "cmge" : "cmhs",
          nameQRegLO(dd, Ity_I64),
          nameQRegLO(nn, Ity_I64), nameQRegLO(mm, Ity_I64));
      return True;
   }

   if (size == X11 && opcode == BITS5(1,0,0,0,0)) {
      /* -------- 0,11,10000 ADD d_d_d -------- */
      /* -------- 1,11,10000 SUB d_d_d -------- */
      Bool   isSUB = bitU == 1;
      IRTemp res   = newTemp(Ity_I64);
      assign(res, binop(isSUB ? Iop_Sub64 : Iop_Add64,
                        getQRegLane(nn, 0, Ity_I64),
                        getQRegLane(mm, 0, Ity_I64)));
      putQRegLane(dd, 0, mkexpr(res));
      putQRegLane(dd, 1, mkU64(0));
      DIP("%s %s, %s, %s\n", isSUB ? "sub" : "add",
          nameQRegLO(dd, Ity_I64),
          nameQRegLO(nn, Ity_I64), nameQRegLO(mm, Ity_I64));
      return True;
   }

   if (size == X11 && opcode == BITS5(1,0,0,0,1)) {
      /* -------- 0,11,10001 CMTST d_d_d -------- */ // &, != 0
      /* -------- 1,11,10001 CMEQ  d_d_d -------- */ // ==
      Bool    isEQ = bitU == 1;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isEQ ? binop(Iop_CmpEQ64x2, argL, argR)
                  : unop(Iop_NotV128, binop(Iop_CmpEQ64x2,
                                            binop(Iop_AndV128, argL, argR), 
                                            mkV128(0x0000))));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      DIP("%s %s, %s, %s\n", isEQ ? "cmeq" : "cmtst",
          nameQRegLO(dd, Ity_I64),
          nameQRegLO(nn, Ity_I64), nameQRegLO(mm, Ity_I64));
      return True;
   }

   if (bitU == 1 && size >= X10 && opcode == BITS5(1,1,0,1,0)) {
      /* -------- 1,1x,11010 FABD d_d_d, s_s_s -------- */
      IRType ity = size == X11 ? Ity_F64 : Ity_F32;
      IRTemp res = newTemp(ity);
      assign(res, unop(mkABSF(ity),
                       triop(mkSUBF(ity),
                             mkexpr(mk_get_IR_rounding_mode()),
                             getQRegLO(nn,ity), getQRegLO(mm,ity))));
      putQReg128(dd, mkV128(0x0000));
      putQRegLO(dd, mkexpr(res));
      DIP("fabd %s, %s, %s\n",
          nameQRegLO(dd, ity), nameQRegLO(nn, ity), nameQRegLO(mm, ity));
      return True;
   }



   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_two_reg_misc(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 29 28    23   21    16     11 9 4
      01 U  11110 size 10000 opcode 10 n d
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,30) != BITS2(0,1)
       || INSN(28,24) != BITS5(1,1,1,1,0)
       || INSN(21,17) != BITS5(1,0,0,0,0)
       || INSN(11,10) != BITS2(1,0)) {
      return False;
   }
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt opcode = INSN(16,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);

   if (size == X11 && opcode == BITS5(0,1,0,0,0)) {
      /* -------- 0,11,01000: CMGT d_d_#0 -------- */ // >s 0
      /* -------- 1,11,01000: CMGE d_d_#0 -------- */ // >=s 0
      Bool    isGT = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = mkV128(0x0000);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res, isGT ? binop(Iop_CmpGT64Sx2, argL, argR)
                       : unop(Iop_NotV128, binop(Iop_CmpGT64Sx2, argR, argL)));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      DIP("cm%s d%u, d%u, #0\n", isGT ? "gt" : "ge", dd, nn);
      return True;
   }

   if (size == X11 && opcode == BITS5(0,1,0,0,1)) {
      /* -------- 0,11,01001: CMEQ d_d_#0 -------- */ // == 0
      /* -------- 1,11,01001: CMLE d_d_#0 -------- */ // <=s 0
      Bool    isEQ = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = mkV128(0x0000);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res, isEQ ? binop(Iop_CmpEQ64x2, argL, argR)
                       : unop(Iop_NotV128,
                              binop(Iop_CmpGT64Sx2, argL, argR)));
      putQReg128(dd, unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      DIP("cm%s d%u, d%u, #0\n", isEQ ? "eq" : "le", dd, nn);
      return True;
   }

   if (bitU == 0 && size == X11 && opcode == BITS5(0,1,0,1,0)) {
      /* -------- 0,11,01010: CMLT d_d_#0 -------- */ // <s 0
      putQReg128(dd, unop(Iop_ZeroHI64ofV128,
                          binop(Iop_CmpGT64Sx2, mkV128(0x0000),
                                                getQReg128(nn))));
      DIP("cm%s d%u, d%u, #0\n", "lt", dd, nn);
      return True;
   }

   if (bitU == 0 && size == X11 && opcode == BITS5(0,1,0,1,1)) {
      /* -------- 0,11,01011 ABS d_d -------- */
      putQReg128(dd, unop(Iop_ZeroHI64ofV128,
                          unop(Iop_Abs64x2, getQReg128(nn))));
      DIP("abs d%u, d%u\n", dd, nn);
      return True;
   }

   if (bitU == 1 && size == X11 && opcode == BITS5(0,1,0,1,1)) {
      /* -------- 1,11,01011 NEG d_d -------- */
      putQReg128(dd, unop(Iop_ZeroHI64ofV128,
                          binop(Iop_Sub64x2, mkV128(0x0000), getQReg128(nn))));
      DIP("neg d%u, d%u\n", dd, nn);
      return True;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_scalar_x_indexed_element(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_shift_by_immediate(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31    28     22   18   15     10 9 4
      0 q u 011110 immh immb opcode 1  n d
      Decode fields: u,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,23) != BITS6(0,1,1,1,1,0) || INSN(10,10) != 1) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt immh   = INSN(22,19);
   UInt immb   = INSN(18,16);
   UInt opcode = INSN(15,11);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);

   if (opcode == BITS5(0,0,0,0,0)) {
      /* -------- 0,00000 SSHR std7_std7_#imm -------- */
      /* -------- 1,00000 USHR std7_std7_#imm -------- */
      /* laneTy, shift = case immh:immb of
                         0001:xxx -> B, SHR:8-xxx
                         001x:xxx -> H, SHR:16-xxxx
                         01xx:xxx -> S, SHR:32-xxxxx
                         1xxx:xxx -> D, SHR:64-xxxxxx
                         other    -> invalid
      */
      const IROp opsSHRN[4]
         = { Iop_ShrN8x16, Iop_ShrN16x8, Iop_ShrN32x4, Iop_ShrN64x2 };
      const IROp opsSARN[4]
         = { Iop_SarN8x16, Iop_SarN16x8, Iop_SarN32x4, Iop_SarN64x2 };
      UInt size  = 0;
      UInt shift = 0;
      Bool isQ   = bitQ == 1;
      Bool isU   = bitU == 1;
      Bool ok    = getLaneInfo_IMMH_IMMB(&shift, &size, immh, immb);
      vassert(size >= 0 && size <= 3);
      if (ok && size < 4 && shift > 0 && shift < (8 << size)
          && !(size == 3/*64bit*/ && !isQ)) {
         IROp    op  = isU ? opsSHRN[size] : opsSARN[size];
         IRExpr* src = getQReg128(nn);
         IRExpr* res = binop(op, src, mkU8(shift));
         putQReg128(dd, isQ ? res : unop(Iop_ZeroHI64ofV128, res));
         HChar laneCh = "bhsd"[size];
         UInt  nLanes = (isQ ? 128 : 64) / (8 << size);
         const HChar* nm = isU ? "ushr" : "sshr";
         DIP("%s %s.%u%c, %s.%u%c, #%u\n", nm,
             nameQReg128(dd), nLanes, laneCh,
             nameQReg128(nn), nLanes, laneCh, shift);
         return True;
      }
      return False;
   }

   if (bitU == 0 && opcode == BITS5(0,1,0,1,0)) {
      /* -------- 0,01010 SHL std7_std7_#imm -------- */
      /* laneTy, shift = case immh:immb of
                         0001:xxx -> B, xxx
                         001x:xxx -> H, xxxx
                         01xx:xxx -> S, xxxxx
                         1xxx:xxx -> D, xxxxxx
                         other    -> invalid
      */
      const IROp opsSHLN[4]
         = { Iop_ShlN8x16, Iop_ShlN16x8, Iop_ShlN32x4, Iop_ShlN64x2 };
      UInt size  = 0;
      UInt shift = 0;
      Bool isQ   = bitQ == 1;
      Bool ok    = getLaneInfo_IMMH_IMMB(&shift, &size, immh, immb);
      vassert(size >= 0 && size <= 3);
      /* The shift encoding has opposite sign for the leftwards case.
         Adjust shift to compensate. */
      shift = (8 << size) - shift;
      if (ok && size < 4 && shift > 0 && shift < (8 << size)
          && !(size == 3/*64bit*/ && !isQ)) {
         IROp    op  = opsSHLN[size];
         IRExpr* src = getQReg128(nn);
         IRExpr* res = binop(op, src, mkU8(shift));
         putQReg128(dd, isQ ? res : unop(Iop_ZeroHI64ofV128, res));
         HChar laneCh = "bhsd"[size];
         UInt  nLanes = (isQ ? 128 : 64) / (8 << size);
         const HChar* nm = "shl";
         DIP("%s %s.%u%c, %s.%u%c, #%u\n", nm,
             nameQReg128(dd), nLanes, laneCh,
             nameQReg128(nn), nLanes, laneCh, shift);
         return True;
      }
      return False;
   }

   if (opcode == BITS5(1,0,1,0,0)) {
      /* -------- 0,10100 SSHLL{,2} #imm -------- */
      /* -------- 1,10100 USHLL{,2} #imm -------- */
      /* 31  28     22   18   15     9 4
         0q0 011110 immh immb 101001 n d  SSHLL Vd.Ta, Vn.Tb, #sh
         0q1 011110 immh immb 101001 n d  USHLL Vd.Ta, Vn.Tb, #sh
         where Ta,Tb,sh
           = case immh of 1xxx -> invalid
                          01xx -> 2d, 2s(q0)/4s(q1),  immh:immb - 32 (0..31)
                          001x -> 4s, 4h(q0)/8h(q1),  immh:immb - 16 (0..15)
                          0001 -> 8h, 8b(q0)/16b(q1), immh:immb - 8  (0..7)
                          0000 -> AdvSIMD modified immediate (???)
      */
      Bool    isQ   = bitQ == 1;
      Bool    isU   = bitU == 1;
      UInt    immhb = (immh << 3) | immb;
      IRTemp  src   = newTemp(Ity_V128);
      IRTemp  zero  = newTemp(Ity_V128);
      IRExpr* res   = NULL;
      UInt    sh    = 0;
      const HChar* ta = "??";
      const HChar* tb = "??";
      assign(src, getQReg128(nn));
      assign(zero, mkV128(0x0000));
      if (immh & 8) {
         /* invalid; don't assign to res */
      }
      else if (immh & 4) {
         sh = immhb - 32;
         vassert(sh < 32); /* so 32-sh is 1..32 */
         ta = "2d";
         tb = isQ ? "4s" : "2s";
         IRExpr* tmp = isQ ? mk_InterleaveHI32x4(src, zero) 
                           : mk_InterleaveLO32x4(src, zero);
         res = binop(isU ? Iop_ShrN64x2 : Iop_SarN64x2, tmp, mkU8(32-sh));
      }
      else if (immh & 2) {
         sh = immhb - 16;
         vassert(sh < 16); /* so 16-sh is 1..16 */
         ta = "4s";
         tb = isQ ? "8h" : "4h";
         IRExpr* tmp = isQ ? mk_InterleaveHI16x8(src, zero) 
                           : mk_InterleaveLO16x8(src, zero);
         res = binop(isU ? Iop_ShrN32x4 : Iop_SarN32x4, tmp, mkU8(16-sh));
      }
      else if (immh & 1) {
         sh = immhb - 8;
         vassert(sh < 8); /* so 8-sh is 1..8 */
         ta = "8h";
         tb = isQ ? "16b" : "8b";
         IRExpr* tmp = isQ ? mk_InterleaveHI8x16(src, zero) 
                           : mk_InterleaveLO8x16(src, zero);
         res = binop(isU ? Iop_ShrN16x8 : Iop_SarN16x8, tmp, mkU8(8-sh));
      } else {
         vassert(immh == 0);
         /* invalid; don't assign to res */
      }
      /* */
      if (res) {
         putQReg128(dd, res);
         DIP("%cshll%s %s.%s, %s.%s, #%d\n",
             isU ? 'u' : 's', isQ ? "2" : "",
             nameQReg128(dd), ta, nameQReg128(nn), tb, sh);
         return True;
      }
      return False;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_three_different(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 30 29 28    23   21 20 15     11 9 4
      0  Q  U  01110 size 1  m  opcode 00 n d
      Decode fields: u,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,24) != BITS5(0,1,1,1,0)
       || INSN(21,21) != 1
       || INSN(11,10) != BITS2(0,0)) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt mm     = INSN(20,16);
   UInt opcode = INSN(15,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);
   Bool is2    = bitQ == 1;

   if (opcode == BITS4(0,1,0,0) || opcode == BITS4(0,1,1,0)) {
      /* -------- 0,0100  ADDHN{2} -------- */
      /* -------- 1,0100 RADDHN{2} -------- */
      /* -------- 0,0110  SUBHN{2} -------- */
      /* -------- 1,0110 RSUBHN{2} -------- */
      /* Narrows, and size refers to the narrowed lanes. */
      if (size == X11) return False;
      vassert(size <= 2);
      const IROp opADD[3] = { Iop_Add16x8,  Iop_Add32x4,  Iop_Add64x2  };
      const IROp opSUB[3] = { Iop_Sub16x8,  Iop_Sub32x4,  Iop_Sub64x2  };
      const IROp opSHR[3] = { Iop_ShrN16x8, Iop_ShrN32x4, Iop_ShrN64x2 };
      const UInt shift[3] = { 8,            16,           32           };
      const IROp opCAT[3] = { Iop_CatEvenLanes8x16, Iop_CatEvenLanes16x8,
                              Iop_CatEvenLanes32x4 };
      Bool isADD = opcode == BITS4(0,1,0,0);
      Bool isR   = bitU == 1;
      /* Combined elements in wide lanes */
      IRTemp  wide  = newTemp(Ity_V128);
      IRExpr* wideE = binop(isADD ? opADD[size] : opSUB[size],
                            getQReg128(nn), getQReg128(mm));
      if (isR) {
         IRType ty  = Ity_INVALID;
         IRTemp rcS = IRTemp_INVALID;
         switch (size) {
            case X00: ty  = Ity_I16;
                      rcS = newTemp(ty); assign(rcS, mkU16(0x80)); break;
            case X01: ty  = Ity_I32;
                      rcS = newTemp(ty); assign(rcS, mkU32(0x8000)); break;
            case X10: ty  = Ity_I64;
                      rcS = newTemp(ty); assign(rcS, mkU64(0x80000000)); break;
            default:  vassert(0);
         }
         IRTemp rcV = math_DUP_TO_V128(rcS, ty);
         wideE = binop(opADD[size], wideE, mkexpr(rcV));
      }
      assign(wide, wideE);
      /* Top halves of elements, still in wide lanes */
      IRTemp shrd = newTemp(Ity_V128);
      assign(shrd, binop(opSHR[size], mkexpr(wide), mkU8(shift[size])));
      /* Elements now compacted into lower 64 bits */
      IRTemp new64 = newTemp(Ity_V128);
      assign(new64, binop(opCAT[size], mkexpr(shrd), mkexpr(shrd)));
      putLO64andZUorPutHI64(is2, dd, new64);
      const HChar* arrNarrow = nameArr_Q_SZ(bitQ, size);
      const HChar* arrWide   = nameArr_Q_SZ(1,    size+1);
      const HChar* nm = isADD ? (isR ? "raddhn" : "addhn")
                              : (isR ? "rsubhn" : "subhn");
      DIP("%s%s %s.%s, %s.%s, %s.%s\n", nm, is2 ? "2" : "",
          nameQReg128(dd), arrNarrow,
          nameQReg128(nn), arrWide, nameQReg128(mm), arrWide);
      return True;
   }

   if (bitU == 0 && opcode == BITS4(1,1,1,0)) {
      /* -------- 0,1110  PMULL{2} -------- */
      /* Narrows, and size refers to the narrowed lanes. */
      if (size != X00) return False;
      IRTemp res
         = math_BINARY_WIDENING_V128(is2, Iop_PolynomialMull8x8,
                                     getQReg128(nn), getQReg128(mm));
      putQReg128(dd, mkexpr(res));
      const HChar* arrNarrow = nameArr_Q_SZ(bitQ, size);
      const HChar* arrWide   = nameArr_Q_SZ(1,    size+1);
      DIP("%s%s %s.%s, %s.%s, %s.%s\n", "pmull", is2 ? "2" : "",
          nameQReg128(dd), arrNarrow,
          nameQReg128(nn), arrWide, nameQReg128(mm), arrWide);
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_three_same(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 30 29 28    23   21 20 15     10 9 4
      0  Q  U  01110 size 1  m  opcode 1  n d
      Decode fields: u,size,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,24) != BITS5(0,1,1,1,0)
       || INSN(21,21) != 1
       || INSN(10,10) != 1) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt mm     = INSN(20,16);
   UInt opcode = INSN(15,11);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);

   if (bitU == 0 && opcode == BITS5(0,0,0,1,1)) {
      /* -------- 0,00,00011 AND 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 0,01,00011 BIC 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 0,10,00011 ORR 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 0,10,00011 ORN 16b_16b_16b, 8b_8b_8b -------- */
      Bool   isQ    = bitQ == 1;
      Bool   isORR  = (size & 2) == 2;
      Bool   invert = (size & 1) == 1;
      IRTemp res    = newTemp(Ity_V128);
      assign(res, binop(isORR ? Iop_OrV128 : Iop_AndV128,
                        getQReg128(nn),
                        invert ? unop(Iop_NotV128, getQReg128(mm))
                               : getQReg128(mm)));
      putQReg128(dd, isQ ? mkexpr(res)
                         : unop(Iop_ZeroHI64ofV128, mkexpr(res)));
      const HChar* names[4] = { "and", "bic", "orr", "orn" };
      const HChar* ar = isQ ? "16b" : "8b";
      DIP("%s %s.%s, %s.%s, %s.%s\n", names[INSN(23,22)],
          nameQReg128(dd), ar, nameQReg128(nn), ar, nameQReg128(mm), ar);
      return True;
   }

   if (bitU == 1 && opcode == BITS5(0,0,0,1,1)) {
      /* -------- 1,00,00011 EOR 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 1,01,00011 BSL 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 1,10,00011 BIT 16b_16b_16b, 8b_8b_8b -------- */
      /* -------- 1,10,00011 BIF 16b_16b_16b, 8b_8b_8b -------- */
      Bool   isQ  = bitQ == 1;
      IRTemp argD = newTemp(Ity_V128);
      IRTemp argN = newTemp(Ity_V128);
      IRTemp argM = newTemp(Ity_V128);
      assign(argD, getQReg128(dd));
      assign(argN, getQReg128(nn));
      assign(argM, getQReg128(mm));
      const IROp opXOR = Iop_XorV128;
      const IROp opAND = Iop_AndV128;
      const IROp opNOT = Iop_NotV128;
      IRExpr* res = NULL;
      switch (size) {
         case BITS2(0,0): /* EOR */
            res = binop(opXOR, mkexpr(argM), mkexpr(argN));
            break;
         case BITS2(0,1): /* BSL */
            res = binop(opXOR, mkexpr(argM),
                               binop(opAND,
                                     binop(opXOR, mkexpr(argM), mkexpr(argN)),
                                     mkexpr(argD)));
            break;
         case BITS2(1,0): /* BIT */
            res = binop(opXOR, mkexpr(argD),
                               binop(opAND,
                                     binop(opXOR, mkexpr(argD), mkexpr(argN)),
                                     mkexpr(argM)));
            break;
         case BITS2(1,1): /* BIF */
            res = binop(opXOR, mkexpr(argD),
                               binop(opAND,
                                     binop(opXOR, mkexpr(argD), mkexpr(argN)),
                                     unop(opNOT, mkexpr(argM))));
            break;
         default:
            vassert(0);
      }
      vassert(res);
      putQReg128(dd, isQ ? res : unop(Iop_ZeroHI64ofV128, res));
      const HChar* nms[4] = { "eor", "bsl", "bit", "bif" };
      const HChar* arr = isQ ? "16b" : "8b";
      DIP("%s %s.%s, %s.%s, %s.%s\n", nms[size],
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(0,0,1,1,0)) {
      /* -------- 0,xx,00110 CMGT std7_std7_std7 -------- */ // >s
      /* -------- 1,xx,00110 CMHI std7_std7_std7 -------- */ // >u
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isGT = bitU == 0;
      const IROp opsGTS[4]
         = { Iop_CmpGT8Sx16, Iop_CmpGT16Sx8, Iop_CmpGT32Sx4, Iop_CmpGT64Sx2 };
      const IROp opsGTU[4]
         = { Iop_CmpGT8Ux16, Iop_CmpGT16Ux8, Iop_CmpGT32Ux4, Iop_CmpGT64Ux2 };
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isGT ? binop(opsGTS[size], argL, argR)
                  : binop(opsGTU[size], argL, argR));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* nm  = isGT ? "cmgt" : "cmhi";
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%s\n", nm,
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(0,0,1,1,1)) {
      /* -------- 0,xx,00111 CMGE std7_std7_std7 -------- */ // >=s
      /* -------- 1,xx,00111 CMHS std7_std7_std7 -------- */ // >=u
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isGE = bitU == 0;
      const IROp opsGTS[4]
         = { Iop_CmpGT8Sx16, Iop_CmpGT16Sx8, Iop_CmpGT32Sx4, Iop_CmpGT64Sx2 };
      const IROp opsGTU[4]
         = { Iop_CmpGT8Ux16, Iop_CmpGT16Ux8, Iop_CmpGT32Ux4, Iop_CmpGT64Ux2 };
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isGE ? unop(Iop_NotV128, binop(opsGTS[size], argR, argL))
                  : unop(Iop_NotV128, binop(opsGTU[size], argR, argL)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* nm  = isGE ? "cmge" : "cmhs";
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%s\n", nm,
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(0,1,1,0,0) || opcode == BITS5(0,1,1,0,1)) {
      /* -------- 0,xx,01100 SMAX std7_std7_std7 -------- */
      /* -------- 1,xx,01100 UMAX std7_std7_std7 -------- */
      /* -------- 0,xx,01101 SMIN std7_std7_std7 -------- */
      /* -------- 1,xx,01101 UMIN std7_std7_std7 -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isU   = bitU == 1;
      Bool isMAX = (opcode & 1) == 0;
      const IROp opMINS[4]
         = { Iop_Min8Sx16, Iop_Min16Sx8, Iop_Min32Sx4, Iop_Min64Sx2 };
      const IROp opMINU[4]
         = { Iop_Min8Ux16, Iop_Min16Ux8, Iop_Min32Ux4, Iop_Min64Ux2 };
      const IROp opMAXS[4]
         = { Iop_Max8Sx16, Iop_Max16Sx8, Iop_Max32Sx4, Iop_Max64Sx2 };
      const IROp opMAXU[4]
         = { Iop_Max8Ux16, Iop_Max16Ux8, Iop_Max32Ux4, Iop_Max64Ux2 };
      IROp op = isMAX ? (isU ? opMAXU[size] : opMAXS[size])
                      : (isU ? opMINU[size] : opMINS[size]);
      IRTemp t = newTemp(Ity_V128);
      assign(t, binop(op, getQReg128(nn), getQReg128(mm)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t))
                               : mkexpr(t));
      const HChar* nm = isMAX ? (isU ? "umax" : "smax")
                              : (isU ? "umin" : "smin");
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%s\n", nm,
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(1,0,0,0,0)) {
      /* -------- 0,xx,10000 ADD std7_std7_std7 -------- */
      /* -------- 1,xx,10000 SUB std7_std7_std7 -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isSUB = bitU == 1;
      const IROp opsADD[4]
         = { Iop_Add8x16, Iop_Add16x8, Iop_Add32x4, Iop_Add64x2 };
      const IROp opsSUB[4]
         = { Iop_Sub8x16, Iop_Sub16x8, Iop_Sub32x4, Iop_Sub64x2 };
      IROp   op = isSUB ? opsSUB[size] : opsADD[size];
      IRTemp t  = newTemp(Ity_V128);
      assign(t, binop(op, getQReg128(nn), getQReg128(mm)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t))
                               : mkexpr(t));
      const HChar* nm  = isSUB ? "sub" : "add";
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%s\n", nm,
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(1,0,0,0,1)) {
      /* -------- 0,xx,10001 CMTST std7_std7_std7 -------- */ // &, != 0
      /* -------- 1,xx,10001 CMEQ  std7_std7_std7 -------- */ // ==
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isEQ = bitU == 1;
      const IROp opsEQ[4]
         = { Iop_CmpEQ8x16,  Iop_CmpEQ16x8,  Iop_CmpEQ32x4,  Iop_CmpEQ64x2 };
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = getQReg128(mm);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res,
             isEQ ? binop(opsEQ[size], argL, argR)
                  : unop(Iop_NotV128, binop(opsEQ[size],
                                            binop(Iop_AndV128, argL, argR), 
                                            mkV128(0x0000))));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* nm  = isEQ ? "cmeq" : "cmtst";
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%s\n", nm,
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(1,0,0,1,0)) {
      /* -------- 0,xx,10010 MLA std7_std7_std7 -------- */
      /* -------- 1,xx,10010 MLS std7_std7_std7 -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isMLS = bitU == 1;
      const IROp opsADD[4]
         = { Iop_Add8x16, Iop_Add16x8, Iop_Add32x4, Iop_INVALID };
      const IROp opsSUB[4]
         = { Iop_Sub8x16, Iop_Sub16x8, Iop_Sub32x4, Iop_INVALID };
      const IROp opsMUL[4]
         = { Iop_Mul8x16, Iop_Mul16x8, Iop_Mul32x4, Iop_INVALID };
      IROp   opMUL    = opsMUL[size];
      IROp   opADDSUB = isMLS ? opsSUB[size] : opsADD[size];
      IRTemp res      = newTemp(Ity_V128);
      if (opMUL != Iop_INVALID && opADDSUB != Iop_INVALID) {
         assign(res, binop(opADDSUB,
                           getQReg128(dd),
                           binop(opMUL, getQReg128(nn), getQReg128(mm))));
         putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                                  : mkexpr(res));
         const HChar* arr = nameArr_Q_SZ(bitQ, size);
         DIP("%s %s.%s, %s.%s, %s.%s\n", isMLS ? "mls" : "mla",
             nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
         return True;
      }
      return False;
   }

   if (bitU == 0 && opcode == BITS5(1,0,1,1,1)) {
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      /* -------- 0,xx,10111 ADDP std7_std7_std7 -------- */
      const IROp opsADD[4]
         = { Iop_Add8x16, Iop_Add16x8, Iop_Add32x4, Iop_Add64x2 };
      const IROp opsCEV[4]
         = { Iop_CatEvenLanes8x16, Iop_CatEvenLanes16x8, Iop_CatEvenLanes32x4,
             Iop_InterleaveLO64x2 };
      const IROp opsCOD[4]
         = { Iop_CatOddLanes8x16, Iop_CatOddLanes16x8, Iop_CatOddLanes32x4,
             Iop_InterleaveHI64x2 };
      IRTemp vN = newTemp(Ity_V128);
      IRTemp vM = newTemp(Ity_V128);
      assign(vN, getQReg128(nn));
      assign(vM, getQReg128(mm));
      IRTemp res128 = newTemp(Ity_V128);
      assign(res128, binop(opsADD[size],
                           binop(opsCEV[size], mkexpr(vM), mkexpr(vN)),
                           binop(opsCOD[size], mkexpr(vM), mkexpr(vN))));
      /* In the half-width case, use CatEL32x4 to extract the half-width
         result from the full-width result. */
      IRExpr* res
         = bitQ == 0 ? unop(Iop_ZeroHI64ofV128,
                            binop(Iop_CatEvenLanes32x4, mkexpr(res128),
                                                        mkexpr(res128)))
                     : mkexpr(res128);
      putQReg128(dd, res);
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("addp %s.%s, %s.%s, %s.%s\n",
      nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (opcode == BITS5(1,0,0,1,1)) {
      /* -------- 0,xx,10011 MUL  std7_std7_std7 -------- */
      /* -------- 1,xx,10011 PMUL 16b_16b_16b, 8b_8b_8b -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isPMUL = bitU == 1;
      const IROp opsMUL[4]
         = { Iop_Mul8x16, Iop_Mul16x8, Iop_Mul32x4, Iop_INVALID };
      const IROp opsPMUL[4]
         = { Iop_PolynomialMul8x16, Iop_INVALID, Iop_INVALID, Iop_INVALID };
      IROp   opMUL = isPMUL ? opsPMUL[size] : opsMUL[size];
      IRTemp res   = newTemp(Ity_V128);
      if (opMUL != Iop_INVALID) {
         assign(res, binop(opMUL, getQReg128(nn), getQReg128(mm)));
         putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                                  : mkexpr(res));
         const HChar* arr = nameArr_Q_SZ(bitQ, size);
         DIP("%s %s.%s, %s.%s, %s.%s\n", isPMUL ? "pmul" : "mul",
             nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
         return True;
      }
      return False;
   }

   if (bitU == 0 && opcode == BITS5(1,1,0,0,1)) {
      /* -------- 0,0x,11001 FMLA 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      /* -------- 0,1x,11001 FMLS 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD   = (size & 1) == 1;
      Bool isSUB = (size & 2) == 2;
      if (bitQ == 0 && isD) return False; // implied 1d case
      IROp opADD = isD ? Iop_Add64Fx2 : Iop_Add32Fx4;
      IROp opSUB = isD ? Iop_Sub64Fx2 : Iop_Sub32Fx4;
      IROp opMUL = isD ? Iop_Mul64Fx2 : Iop_Mul32Fx4;
      IRTemp rm = mk_get_IR_rounding_mode();
      IRTemp t1 = newTemp(Ity_V128);
      IRTemp t2 = newTemp(Ity_V128);
      // FIXME: double rounding; use FMA primops instead
      assign(t1, triop(opMUL,
                       mkexpr(rm), getQReg128(nn), getQReg128(mm)));
      assign(t2, triop(isSUB ? opSUB : opADD,
                       mkexpr(rm), getQReg128(dd), mkexpr(t1)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t2))
                 : mkexpr(t2));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", isSUB ? "fmls" : "fmla",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 0 && opcode == BITS5(1,1,0,1,0)) {
      /* -------- 0,0x,11010 FADD 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      /* -------- 0,1x,11010 FSUB 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD   = (size & 1) == 1;
      Bool isSUB = (size & 2) == 2;
      if (bitQ == 0 && isD) return False; // implied 1d case
      const IROp ops[4]
         = { Iop_Add32Fx4, Iop_Add64Fx2, Iop_Sub32Fx4, Iop_Sub64Fx2 };
      IROp   op = ops[size];
      IRTemp rm = mk_get_IR_rounding_mode();
      IRTemp t1 = newTemp(Ity_V128);
      IRTemp t2 = newTemp(Ity_V128);
      assign(t1, triop(op, mkexpr(rm), getQReg128(nn), getQReg128(mm)));
      assign(t2, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                           : mkexpr(t1));
      putQReg128(dd, mkexpr(t2));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", isSUB ? "fsub" : "fadd",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 1 && size >= X10 && opcode == BITS5(1,1,0,1,0)) {
      /* -------- 1,1x,11010 FABD 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD = (size & 1) == 1;
      if (bitQ == 0 && isD) return False; // implied 1d case
      IROp   opSUB = isD ? Iop_Sub64Fx2 : Iop_Sub32Fx4;
      IROp   opABS = isD ? Iop_Abs64Fx2 : Iop_Abs32Fx4;
      IRTemp rm    = mk_get_IR_rounding_mode();
      IRTemp t1    = newTemp(Ity_V128);
      IRTemp t2    = newTemp(Ity_V128);
      // FIXME: use Abd primop instead?
      assign(t1, triop(opSUB,
                       mkexpr(rm), getQReg128(nn), getQReg128(mm)));
      assign(t2, unop(opABS, mkexpr(t1)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t2))
                               : mkexpr(t2));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("fabd %s.%s, %s.%s, %s.%s\n",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 1 && size <= X01 && opcode == BITS5(1,1,0,1,1)) {
      /* -------- 1,0x,11011 FMUL 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD = (size & 1) == 1;
      if (bitQ == 0 && isD) return False; // implied 1d case
      IRTemp rm = mk_get_IR_rounding_mode();
      IRTemp t1 = newTemp(Ity_V128);
      assign(t1, triop(isD ? Iop_Mul64Fx2 : Iop_Mul32Fx4,
                       mkexpr(rm), getQReg128(nn), getQReg128(mm)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                               : mkexpr(t1));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("fmul %s.%s, %s.%s, %s.%s\n",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (size <= X01 && opcode == BITS5(1,1,1,0,0)) {
      /* -------- 0,0x,11100 FCMEQ 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      /* -------- 1,0x,11100 FCMGE 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD = (size & 1) == 1;
      if (bitQ == 0 && isD) return False; // implied 1d case
      Bool   isGE  = bitU == 1;
      IROp   opCMP = isGE ? (isD ? Iop_CmpLE64Fx2 : Iop_CmpLE32Fx4)
                          : (isD ? Iop_CmpEQ64Fx2 : Iop_CmpEQ32Fx4);
      IRTemp t1    = newTemp(Ity_V128);
      assign(t1, isGE ? binop(opCMP, getQReg128(mm), getQReg128(nn)) // swapd
                      : binop(opCMP, getQReg128(nn), getQReg128(mm)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                               : mkexpr(t1));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", isGE ? "fcmge" : "fcmeq",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 1 && size >= X10 && opcode == BITS5(1,1,1,0,0)) {
      /* -------- 1,1x,11100 FCMGT 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD = (size & 1) == 1;
      if (bitQ == 0 && isD) return False; // implied 1d case
      IROp   opCMP = isD ? Iop_CmpLT64Fx2 : Iop_CmpLT32Fx4;
      IRTemp t1    = newTemp(Ity_V128);
      assign(t1, binop(opCMP, getQReg128(mm), getQReg128(nn))); // swapd
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                               : mkexpr(t1));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", "fcmgt",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 1 && opcode == BITS5(1,1,1,0,1)) {
      /* -------- 1,0x,11101 FACGE 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      /* -------- 1,1x,11101 FACGT 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD  = (size & 1) == 1;
      Bool isGT = (size & 2) == 2;
      if (bitQ == 0 && isD) return False; // implied 1d case
      IROp   opCMP = isGT ? (isD ? Iop_CmpLT64Fx2 : Iop_CmpLT32Fx4)
                          : (isD ? Iop_CmpLE64Fx2 : Iop_CmpLE32Fx4);
      IROp   opABS = isD ? Iop_Abs64Fx2 : Iop_Abs32Fx4;
      IRTemp t1    = newTemp(Ity_V128);
      assign(t1, binop(opCMP, unop(opABS, getQReg128(mm)),
                              unop(opABS, getQReg128(nn)))); // swapd
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                               : mkexpr(t1));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", isGT ? "facgt" : "facge",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   if (bitU == 1 && size <= X01 && opcode == BITS5(1,1,1,1,1)) {
      /* -------- 1,0x,11111 FDIV 2d_2d_2d, 4s_4s_4s, 2s_2s_2s -------- */
      Bool isD = (size & 1) == 1;
      if (bitQ == 0 && isD) return False; // implied 1d case
      vassert(size <= 1);
      const IROp ops[2] = { Iop_Div32Fx4, Iop_Div64Fx2 };
      IROp   op = ops[size];
      IRTemp rm = mk_get_IR_rounding_mode();
      IRTemp t1 = newTemp(Ity_V128);
      IRTemp t2 = newTemp(Ity_V128);
      assign(t1, triop(op, mkexpr(rm), getQReg128(nn), getQReg128(mm)));
      assign(t2, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(t1))
                           : mkexpr(t1));
      putQReg128(dd, mkexpr(t2));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s, %s.%s\n", "fdiv",
          nameQReg128(dd), arr, nameQReg128(nn), arr, nameQReg128(mm), arr);
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_two_reg_misc(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 30 29 28    23   21    16     11 9 4
      0  Q  U  01110 size 10000 opcode 10 n d
      Decode fields: U,size,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       || INSN(28,24) != BITS5(0,1,1,1,0)
       || INSN(21,17) != BITS5(1,0,0,0,0)
       || INSN(11,10) != BITS2(1,0)) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt opcode = INSN(16,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);

   if (opcode == BITS5(0,0,1,0,0)) {
      /* -------- 0,xx,00100: CLS std6_std6 -------- */
      /* -------- 1,xx,00100: CLZ std6_std6 -------- */
      if (size == X11) return False; // no 1d or 2d cases
      const IROp opsCLS[3] = { Iop_Cls8Sx16, Iop_Cls16Sx8, Iop_Cls32Sx4 };
      const IROp opsCLZ[3] = { Iop_Clz8Sx16, Iop_Clz16Sx8, Iop_Clz32Sx4 };
      Bool   isCLZ = bitU == 1;
      IRTemp res   = newTemp(Ity_V128);
      vassert(size <= 2);
      assign(res, unop(isCLZ ? opsCLZ[size] : opsCLS[size], getQReg128(nn)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s\n", isCLZ ? "clz" : "cls",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (size == X00 && opcode == BITS5(0,0,1,0,1)) {
      /* -------- 0,00,00101: CNT 16b_16b, 8b_8b -------- */
      /* -------- 1,00,00101: NOT 16b_16b, 8b_8b -------- */
      IRTemp res = newTemp(Ity_V128);
      assign(res, unop(bitU == 0 ? Iop_Cnt8x16 : Iop_NotV128, getQReg128(nn)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s\n", bitU == 0 ? "cnt" : "not",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (opcode == BITS5(0,1,0,0,0)) {
      /* -------- 0,xx,01000: CMGT std7_std7_#0 -------- */ // >s 0
      /* -------- 1,xx,01000: CMGE std7_std7_#0 -------- */ // >=s 0
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      const IROp opsGTS[4]
         = { Iop_CmpGT8Sx16, Iop_CmpGT16Sx8, Iop_CmpGT32Sx4, Iop_CmpGT64Sx2 };
      Bool    isGT = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = mkV128(0x0000);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res, isGT ? binop(opsGTS[size], argL, argR)
                       : unop(Iop_NotV128, binop(opsGTS[size], argR, argL)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("cm%s %s.%s, %s.%s, #0\n", isGT ? "gt" : "ge",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (opcode == BITS5(0,1,0,0,1)) {
      /* -------- 0,xx,01001: CMEQ std7_std7_#0 -------- */ // == 0
      /* -------- 1,xx,01001: CMLE std7_std7_#0 -------- */ // <=s 0
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      const IROp opsEQ[4]
         = { Iop_CmpEQ8x16,  Iop_CmpEQ16x8,  Iop_CmpEQ32x4,  Iop_CmpEQ64x2 };
      const IROp opsGTS[4]
         = { Iop_CmpGT8Sx16, Iop_CmpGT16Sx8, Iop_CmpGT32Sx4, Iop_CmpGT64Sx2 };
      Bool    isEQ = bitU == 0;
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = mkV128(0x0000);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res, isEQ ? binop(opsEQ[size], argL, argR)
                       : unop(Iop_NotV128,
                              binop(opsGTS[size], argL, argR)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("cm%s %s.%s, %s.%s, #0\n", isEQ ? "eq" : "le",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (bitU == 0 && opcode == BITS5(0,1,0,1,0)) {
      /* -------- 0,xx,01010: CMLT std7_std7_#0 -------- */ // <s 0
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      const IROp opsGTS[4]
         = { Iop_CmpGT8Sx16, Iop_CmpGT16Sx8, Iop_CmpGT32Sx4, Iop_CmpGT64Sx2 };
      IRExpr* argL = getQReg128(nn);
      IRExpr* argR = mkV128(0x0000);
      IRTemp  res  = newTemp(Ity_V128);
      assign(res, binop(opsGTS[size], argR, argL));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("cm%s %s.%s, %s.%s, #0\n", "lt",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (bitU == 0 && opcode == BITS5(0,1,0,1,1)) {
      /* -------- 0,xx,01011: ABS std7_std7 -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      const IROp opABS[4]
         = { Iop_Abs8x16, Iop_Abs16x8, Iop_Abs32x4, Iop_Abs64x2 };
      IRTemp res = newTemp(Ity_V128);
      assign(res, unop(opABS[size], getQReg128(nn)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("abs %s.%s, %s.%s\n", nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (bitU == 1 && opcode == BITS5(0,1,0,1,1)) {
      /* -------- 1,xx,01011: NEG std7_std7 -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      const IROp opSUB[4]
         = { Iop_Sub8x16, Iop_Sub16x8, Iop_Sub32x4, Iop_Sub64x2 };
      IRTemp res = newTemp(Ity_V128);
      assign(res, binop(opSUB[size], mkV128(0x0000), getQReg128(nn)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("neg %s.%s, %s.%s\n", nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (size >= X10 && opcode == BITS5(0,1,1,1,1)) {
      /* -------- 0,1x,01111: FABS 2d_2d, 4s_4s, 2s_2s -------- */
      /* -------- 1,1x,01111: FNEG 2d_2d, 4s_4s, 2s_2s -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool   isFNEG = bitU == 1;
      IROp   op     = isFNEG ? (size == X10 ? Iop_Neg32Fx4 : Iop_Neg64Fx2)
                             : (size == X10 ? Iop_Abs32Fx4 : Iop_Abs64Fx2);
      IRTemp res = newTemp(Ity_V128);
      assign(res, unop(op, getQReg128(nn)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = bitQ == 0 ? "2s" : (size == X11 ? "2d" : "4s");
      DIP("%s %s.%s, %s.%s\n", isFNEG ? "fneg" : "fabs",
          nameQReg128(dd), arr, nameQReg128(nn), arr);
      return True;
   }

   if (bitU == 0 && opcode == BITS5(1,0,0,1,0)) {
      /* -------- 0,xx,10010: XTN{,2} -------- */
      /* 31  28    23   21     15     9 4  XTN{,2} Vd.Tb, Vn.Ta
         0q0 01110 size 100001 001010 n d
      */
      Bool isQ  = bitQ == 1;
      IROp op   = Iop_INVALID;
      const HChar* tb = NULL;
      const HChar* ta = NULL;
      switch ((size << 1) | (isQ ? 1 : 0)) {
         case 0: tb = "8b";  ta = "8h"; op = Iop_NarrowUn16to8x8;  break;
         case 1: tb = "16b"; ta = "8h"; op = Iop_NarrowUn16to8x8;  break;
         case 2: tb = "4h";  ta = "4s"; op = Iop_NarrowUn32to16x4; break;
         case 3: tb = "8h";  ta = "4s"; op = Iop_NarrowUn32to16x4; break;
         case 4: tb = "2s";  ta = "2d"; op = Iop_NarrowUn64to32x2; break;
         case 5: tb = "4s";  ta = "2d"; op = Iop_NarrowUn64to32x2; break;
         case 6: break;
         case 7: break;
         default: vassert(0);
      }
      if (op != Iop_INVALID) {
         if (!isQ) {
            putQRegLane(dd, 1, mkU64(0));
         }
         putQRegLane(dd, isQ ? 1 : 0, unop(op, getQReg128(nn)));
         DIP("xtn%s %s.%s, %s.%s\n", isQ ? "2" : "",
             nameQReg128(dd), tb, nameQReg128(nn), ta);
         return True;
      }
      return False;
   }

   if (bitU == 0 && size == X01 && opcode == BITS5(1,0,1,1,0)) {
      /* -------- 0,01,10110: FCVTN 2s/4s_2d -------- */
      IRTemp  rm    = mk_get_IR_rounding_mode();
      IRExpr* srcLo = getQRegLane(nn, 0, Ity_F64);
      IRExpr* srcHi = getQRegLane(nn, 1, Ity_F64);
      putQRegLane(dd, 2 * bitQ + 0, binop(Iop_F64toF32, mkexpr(rm), srcLo));
      putQRegLane(dd, 2 * bitQ + 1, binop(Iop_F64toF32, mkexpr(rm), srcHi));
      if (bitQ == 0) {
         putQRegLane(dd, 1, mkU64(0));
      }
      DIP("fcvtn%s %s.%s, %s.2d\n", bitQ ? "2" : "",
          nameQReg128(dd), bitQ ? "4s" : "2s", nameQReg128(nn));
      return True;
   }

   if (size <= X01 && opcode == BITS5(1,1,1,0,1)) {
      /* -------- 0,0x,11101: SCVTF -------- */
      /* -------- 1,0x,11101: UCVTF -------- */
      /* 31  28      22 21       15     9 4
         0q0 01110 0 sz 1  00001 110110 n d  SCVTF Vd, Vn
         0q1 01110 0 sz 1  00001 110110 n d  UCVTF Vd, Vn
         with laneage:
         case sz:Q of 00 -> 2S, zero upper, 01 -> 4S, 10 -> illegal, 11 -> 2D
      */
      Bool isQ   = bitQ == 1;
      Bool isU   = bitU == 1;
      Bool isF64 = (size & 1) == 1;
      if (isQ || !isF64) {
         IRType tyF = Ity_INVALID, tyI = Ity_INVALID;
         UInt   nLanes = 0;
         Bool   zeroHI = False;
         const HChar* arrSpec = NULL;
         Bool   ok  = getLaneInfo_Q_SZ(&tyI, &tyF, &nLanes, &zeroHI, &arrSpec,
                                       isQ, isF64 );
         IROp   iop = isU ? (isF64 ? Iop_I64UtoF64 : Iop_I32UtoF32)
                          : (isF64 ? Iop_I64StoF64 : Iop_I32StoF32);
         IRTemp rm  = mk_get_IR_rounding_mode();
         UInt   i;
         vassert(ok); /* the 'if' above should ensure this */
         for (i = 0; i < nLanes; i++) {
            putQRegLane(dd, i,
                        binop(iop, mkexpr(rm), getQRegLane(nn, i, tyI)));
         }
         if (zeroHI) {
            putQRegLane(dd, 1, mkU64(0));
         }
         DIP("%ccvtf %s.%s, %s.%s\n", isU ? 'u' : 's',
             nameQReg128(dd), arrSpec, nameQReg128(nn), arrSpec);
         return True;
      }
      /* else fall through */
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_vector_x_indexed_elem(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31    28    23   21 20 19 15     11   9 4
      0 Q U 01111 size L  M  m  opcode H  0 n d
      Decode fields are: u,size,opcode
      M is really part of the mm register number.  Individual 
      cases need to inspect L and H though.
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,31) != 0
       && INSN(28,24) != BITS5(0,1,1,1,1) && INSN(10,10) !=0) {
      return False;
   }
   UInt bitQ   = INSN(30,30);
   UInt bitU   = INSN(29,29);
   UInt size   = INSN(23,22);
   UInt bitL   = INSN(21,21);
   UInt bitM   = INSN(20,20);
   UInt mmLO4  = INSN(19,16);
   UInt opcode = INSN(15,12);
   UInt bitH   = INSN(11,11);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);
   vassert(size < 4);
   vassert(bitH < 2 && bitM < 2 && bitL < 2);

   if (bitU == 0 && size >= X10 && opcode == BITS4(1,0,0,1)) {
      /* -------- 0,1x,1001 FMUL 2d_2d_d[], 4s_4s_s[], 2s_2s_s[] -------- */
      if (bitQ == 0 && size == X11) return False; // implied 1d case
      Bool isD = (size & 1) == 1;
      UInt index;
      if      (!isD)             index = (bitH << 1) | bitL;
      else if (isD && bitL == 0) index = bitH;
      else return False; // sz:L == x11 => unallocated encoding
      vassert(index < (isD ? 2 : 4));
      IRType ity  = isD ? Ity_F64 : Ity_F32;
      IRTemp elem = newTemp(ity);
      UInt   mm   = (bitM << 4) | mmLO4;
      assign(elem, getQRegLane(mm, index, ity));
      IRTemp dupd = math_DUP_TO_V128(elem, ity);
      IRTemp res  = newTemp(Ity_V128);
      assign(res, triop(isD ? Iop_Mul64Fx2 : Iop_Mul32Fx4,
                        mkexpr(mk_get_IR_rounding_mode()),
                        getQReg128(nn), mkexpr(dupd)));
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = bitQ == 0 ? "2s" : (isD ? "2d" : "4s");
      DIP("fmul %s.%s, %s.%s, %s.%c[%u]\n", nameQReg128(dd), arr,
          nameQReg128(nn), arr, nameQReg128(mm), isD ? 'd' : 's', index);
      return True;
   }

   if ((bitU == 1 && (opcode == BITS4(0,0,0,0) || opcode == BITS4(0,1,0,0)))
       || (bitU == 0 && opcode == BITS4(1,0,0,0))) {
      /* -------- 1,xx,0000 MLA s/h variants only -------- */
      /* -------- 1,xx,0100 MLS s/h variants only -------- */
      /* -------- 0,xx,1000 MUL s/h variants only -------- */
      Bool isMLA = opcode == BITS4(0,0,0,0);
      Bool isMLS = opcode == BITS4(0,1,0,0);
      UInt mm    = 32; // invalid
      UInt ix    = 16; // invalid
      switch (size) {
         case X00:
            return False; // b case is not allowed
         case X01:
            mm = mmLO4; ix = (bitH << 2) | (bitL << 1) | (bitM << 0); break;
         case X10:
            mm = (bitM << 4) | mmLO4; ix = (bitH << 1) | (bitL << 0); break;
         case X11:
            return False; // d case is not allowed
         default:
            vassert(0);
      }
      vassert(mm < 32 && ix < 16);
      IROp   opMUL = size == X01 ? Iop_Mul16x8 : Iop_Mul32x4;
      IROp   opADD = size == X01 ? Iop_Add16x8 : Iop_Add32x4;
      IROp   opSUB = size == X01 ? Iop_Sub16x8 : Iop_Sub32x4;
      IRType ity   = size == X01 ? Ity_I16 : Ity_I32;
      HChar  ch    = size == X01 ? 'h' : 's';
      IRTemp elemM = newTemp(ity);
      assign(elemM, getQRegLane(mm, ix, ity));
      IRTemp vecM  = math_DUP_TO_V128(elemM, ity);
      IRTemp vecD  = newTemp(Ity_V128);
      IRTemp vecN  = newTemp(Ity_V128);
      IRTemp res   = newTemp(Ity_V128);
      assign(vecD, getQReg128(dd));
      assign(vecN, getQReg128(nn));
      IRExpr* prod = binop(opMUL, mkexpr(vecN), mkexpr(vecM));
      if (isMLA || isMLS) {
         assign(res, binop(isMLA ? opADD : opSUB, mkexpr(vecD), prod));
      } else {
         assign(res, prod);
      }
      putQReg128(dd, bitQ == 0 ? unop(Iop_ZeroHI64ofV128, mkexpr(res))
                               : mkexpr(res));
      const HChar* arr = nameArr_Q_SZ(bitQ, size);
      DIP("%s %s.%s, %s.%s, %s.%c[%u]\n", isMLA ? "mla"
                                                : (isMLS ? "mls" : "mul"),
          nameQReg128(dd), arr,
          nameQReg128(nn), arr, nameQReg128(dd), ch, ix);
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_crypto_aes(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_crypto_three_reg_sha(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_crypto_two_reg_sha(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_compare(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  28    23 21 20 15 13   9 4
      000 11110 ty 1  m  op 1000 n opcode2
      The first 3 bits are really "M 0 S", but M and S are always zero.
      Decode fields are: ty,op,opcode2
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,24) != BITS8(0,0,0,1,1,1,1,0)
       || INSN(21,21) != 1 || INSN(13,10) != BITS4(1,0,0,0)) {
      return False;
   }
   UInt ty      = INSN(23,22);
   UInt mm      = INSN(20,16);
   UInt op      = INSN(15,14);
   UInt nn      = INSN(9,5);
   UInt opcode2 = INSN(4,0);
   vassert(ty < 4);

   if (ty <= X01 && op == X00
       && (opcode2 & BITS5(0,0,1,1,1)) == BITS5(0,0,0,0,0)) {
      /* -------- 0x,00,00000 FCMP  d_d,   s_s -------- */
      /* -------- 0x,00,01000 FCMP  d_#0, s_#0 -------- */
      /* -------- 0x,00,10000 FCMPE d_d,   s_s -------- */
      /* -------- 0x,00,11000 FCMPE d_#0, s_#0 -------- */
      /* 31        23   20    15      9 4
         000 11110 01 1     m 00 1000 n 10 000  FCMPE Dn, Dm
         000 11110 01 1 00000 00 1000 n 11 000  FCMPE Dn, #0.0
         000 11110 01 1     m 00 1000 n 00 000  FCMP  Dn, Dm
         000 11110 01 1 00000 00 1000 n 01 000  FCMP  Dn, #0.0

         000 11110 00 1     m 00 1000 n 10 000  FCMPE Sn, Sm
         000 11110 00 1 00000 00 1000 n 11 000  FCMPE Sn, #0.0
         000 11110 00 1     m 00 1000 n 00 000  FCMP  Sn, Sm
         000 11110 00 1 00000 00 1000 n 01 000  FCMP  Sn, #0.0

         FCMPE generates Invalid Operation exn if either arg is any kind
         of NaN.  FCMP generates Invalid Operation exn if either arg is a
         signalling NaN.  We ignore this detail here and produce the same
         IR for both.
      */
      Bool   isD     = (ty & 1) == 1;
      Bool   isCMPE  = (opcode2 & 16) == 16;
      Bool   cmpZero = (opcode2 & 8) == 8;
      IRType ity     = isD ? Ity_F64 : Ity_F32;
      Bool   valid   = True;
      if (cmpZero && mm != 0) valid = False;
      if (valid) {
         IRTemp argL  = newTemp(ity);
         IRTemp argR  = newTemp(ity);
         IRTemp irRes = newTemp(Ity_I32);
         assign(argL, getQRegLO(nn, ity));
         assign(argR,
                cmpZero 
                   ? (IRExpr_Const(isD ? IRConst_F64i(0) : IRConst_F32i(0)))
                   : getQRegLO(mm, ity));
         assign(irRes, binop(isD ? Iop_CmpF64 : Iop_CmpF32,
                             mkexpr(argL), mkexpr(argR)));
         IRTemp nzcv = mk_convert_IRCmpF64Result_to_NZCV(irRes);
         IRTemp nzcv_28x0 = newTemp(Ity_I64);
         assign(nzcv_28x0, binop(Iop_Shl64, mkexpr(nzcv), mkU8(28)));
         setFlags_COPY(nzcv_28x0);
         DIP("fcmp%s %s, %s\n", isCMPE ? "e" : "", nameQRegLO(nn, ity),
             cmpZero ? "#0.0" : nameQRegLO(mm, ity));
         return True;
      }
      return False;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_conditional_compare(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_conditional_select(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_data_proc_1_source(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  28    23 21 20     14    9 4
      000 11110 ty 1  opcode 10000 n d
      The first 3 bits are really "M 0 S", but M and S are always zero.
      Decode fields: ty,opcode
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,24) != BITS8(0,0,0,1,1,1,1,0)
       || INSN(21,21) != 1 || INSN(14,10) != BITS5(1,0,0,0,0)) {
      return False;
   }
   UInt ty     = INSN(23,22);
   UInt opcode = INSN(20,15);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);

   if (ty <= X01 && opcode <= BITS6(0,0,0,0,1,1)) {
      /* -------- 0x,000000: FMOV  d_d, s_s -------- */
      /* -------- 0x,000001: FABS  d_d, s_s -------- */
      /* -------- 0x,000010: FNEG  d_d, s_s -------- */
      /* -------- 0x,000011: FSQRT d_d, s_s -------- */
      IRType ity = ty == X01 ? Ity_F64 : Ity_F32;
      IRTemp src = newTemp(ity);
      IRTemp res = newTemp(ity);
      const HChar* nm = "??";
      assign(src, getQRegLO(nn, ity));
      switch (opcode) {
         case BITS6(0,0,0,0,0,0):
            nm = "fmov"; assign(res, mkexpr(src)); break;
         case BITS6(0,0,0,0,0,1):
            nm = "fabs"; assign(res, unop(mkABSF(ity), mkexpr(src))); break;
         case BITS6(0,0,0,0,1,0):
            nm = "fabs"; assign(res, unop(mkNEGF(ity), mkexpr(src))); break;
         case BITS6(0,0,0,0,1,1):
            nm = "fsqrt";
            assign(res, binop(mkSQRTF(ity), 
                              mkexpr(mk_get_IR_rounding_mode()),
                              mkexpr(src))); break;
         default:
            vassert(0);
      }
      putQReg128(dd, mkV128(0x0000));
      putQRegLO(dd, mkexpr(res));
      DIP("%s %s, %s\n", nm, nameQRegLO(dd, ity), nameQRegLO(nn, ity));
      return True;
   }

   if (   (ty == X11 && (opcode == BITS6(0,0,0,1,0,0) 
                         || opcode == BITS6(0,0,0,1,0,1)))
       || (ty == X00 && (opcode == BITS6(0,0,0,1,1,1) 
                         || opcode == BITS6(0,0,0,1,0,1)))
       || (ty == X01 && (opcode == BITS6(0,0,0,1,1,1) 
                         || opcode == BITS6(0,0,0,1,0,0)))) {
      /* -------- 11,000100: FCVT s_h -------- */
      /* -------- 11,000101: FCVT d_h -------- */
      /* -------- 00,000111: FCVT h_s -------- */
      /* -------- 00,000101: FCVT d_s -------- */
      /* -------- 01,000111: FCVT h_d -------- */
      /* -------- 01,000100: FCVT s_d -------- */
      /* 31        23 21    16 14    9 4
         000 11110 11 10001 00 10000 n d   FCVT Sd, Hn (unimp)
         --------- 11 ----- 01 ---------   FCVT Dd, Hn (unimp)
         --------- 00 ----- 11 ---------   FCVT Hd, Sn (unimp)
         --------- 00 ----- 01 ---------   FCVT Dd, Sn
         --------- 01 ----- 11 ---------   FCVT Hd, Dn (unimp)
         --------- 01 ----- 00 ---------   FCVT Sd, Dn
         Rounding, when dst is smaller than src, is per the FPCR.
      */
      UInt b2322 = ty;
      UInt b1615 = opcode & BITS2(1,1);
      if (b2322 == BITS2(0,0) && b1615 == BITS2(0,1)) {
         /* Convert S to D */
         IRTemp res = newTemp(Ity_F64);
         assign(res, unop(Iop_F32toF64, getQRegLO(nn, Ity_F32)));
         putQReg128(dd, mkV128(0x0000));
         putQRegLO(dd, mkexpr(res));
         DIP("fcvt %s, %s\n",
             nameQRegLO(dd, Ity_F64), nameQRegLO(nn, Ity_F32));
         return True;
      }
      if (b2322 == BITS2(0,1) && b1615 == BITS2(0,0)) {
         /* Convert D to S */
         IRTemp res = newTemp(Ity_F32);
         assign(res, binop(Iop_F64toF32, mkexpr(mk_get_IR_rounding_mode()),
                                         getQRegLO(nn, Ity_F64)));
         putQReg128(dd, mkV128(0x0000));
         putQRegLO(dd, mkexpr(res));
         DIP("fcvt %s, %s\n",
             nameQRegLO(dd, Ity_F32), nameQRegLO(nn, Ity_F64));
         return True;
      }
      /* else unhandled */
      return False;
   }

   if (ty <= X01
       && opcode >= BITS6(0,0,1,0,0,0) && opcode <= BITS6(0,0,1,1,1,1)
       && opcode != BITS6(0,0,1,1,0,1)) {
      /* -------- 0x,001000 FRINTN d_d, s_s -------- */
      /* -------- 0x,001001 FRINTP d_d, s_s -------- */
      /* -------- 0x,001010 FRINTM d_d, s_s -------- */
      /* -------- 0x,001011 FRINTZ d_d, s_s -------- */
      /* -------- 0x,001100 FRINTA d_d, s_s -------- */
      /* -------- 0x,001110 FRINTX d_d, s_s -------- */
      /* -------- 0x,001111 FRINTI d_d, s_s -------- */
      /* 31        23 21   17  14    9 4
         000 11110 0x 1001 111 10000 n d  FRINTI Fd, Fm (round per FPCR)
                           rm
         x==0 => S-registers, x==1 => D-registers
         rm (17:15) encodings:
            111 per FPCR  (FRINTI)
            001 +inf      (FRINTP)
            010 -inf      (FRINTM)
            011 zero      (FRINTZ)
            000 tieeven
            100 tieaway   (FRINTA) -- !! FIXME KLUDGED !!
            110 per FPCR + "exact = TRUE"
            101 unallocated
      */
      Bool    isD   = (ty & 1) == 1;
      UInt    rm    = opcode & BITS6(0,0,0,1,1,1);
      IRType  ity   = isD ? Ity_F64 : Ity_F32;
      IRExpr* irrmE = NULL;
      UChar   ch    = '?';
      switch (rm) {
         case BITS3(0,1,1): ch = 'z'; irrmE = mkU32(Irrm_ZERO); break;
         case BITS3(0,1,0): ch = 'm'; irrmE = mkU32(Irrm_NegINF); break;
         case BITS3(0,0,1): ch = 'p'; irrmE = mkU32(Irrm_PosINF); break;
         // The following is a kludge.  Should be: Irrm_NEAREST_TIE_AWAY_0
         case BITS3(1,0,0): ch = 'a'; irrmE = mkU32(Irrm_NEAREST); break;
         default: break;
      }
      if (irrmE) {
         IRTemp src = newTemp(ity);
         IRTemp dst = newTemp(ity);
         assign(src, getQRegLO(nn, ity));
         assign(dst, binop(isD ? Iop_RoundF64toInt : Iop_RoundF32toInt,
                           irrmE, mkexpr(src)));
         putQReg128(dd, mkV128(0x0000));
         putQRegLO(dd, mkexpr(dst));
         DIP("frint%c %s, %s\n",
             ch, nameQRegLO(dd, ity), nameQRegLO(nn, ity));
         return True;
      }
      return False;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_data_proc_2_source(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  28    23 21 20 15     11 9 4
      000 11110 ty 1  m  opcode 10 n d
      The first 3 bits are really "M 0 S", but M and S are always zero.
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,24) != BITS8(0,0,0,1,1,1,1,0)
       || INSN(21,21) != 1 || INSN(11,10) != BITS2(1,0)) {
      return False;
   }
   UInt ty     = INSN(23,22);
   UInt mm     = INSN(20,16);
   UInt opcode = INSN(15,12);
   UInt nn     = INSN(9,5);
   UInt dd     = INSN(4,0);

   if (ty <= X01 && opcode <= BITS4(0,0,1,1)) {
      /* ------- 0x,0000: FMUL d_d, s_s ------- */
      /* ------- 0x,0001: FDIV d_d, s_s ------- */
      /* ------- 0x,0010: FADD d_d, s_s ------- */
      /* ------- 0x,0011: FSUB d_d, s_s ------- */
      IRType ity = ty == X00 ? Ity_F32 : Ity_F64;
      IROp   iop = Iop_INVALID;
      const HChar* nm = "???";
      switch (opcode) {
         case BITS4(0,0,0,0): nm = "fmul";  iop = mkMULF(ity); break;
         case BITS4(0,0,0,1): nm = "fdiv";  iop = mkDIVF(ity); break;
         case BITS4(0,0,1,0): nm = "fadd";  iop = mkADDF(ity); break;
         case BITS4(0,0,1,1): nm = "fsub";  iop = mkSUBF(ity); break;
         default: vassert(0);
      }
      IRExpr* resE = triop(iop, mkexpr(mk_get_IR_rounding_mode()),
                           getQRegLO(nn, ity), getQRegLO(mm, ity));
      IRTemp  res  = newTemp(ity);
      assign(res, resE);
      putQReg128(dd, mkV128(0));
      putQRegLO(dd, mkexpr(res));
      DIP("%s %s, %s, %s\n",
          nm, nameQRegLO(dd, ity), nameQRegLO(nn, ity), nameQRegLO(mm, ity));
      return True;
   }

   if (ty <= X01 && opcode == BITS4(1,0,0,0)) {
      /* ------- 0x,1000: FNMUL d_d, s_s ------- */
      IRType ity  = ty == X00 ? Ity_F32 : Ity_F64;
      IROp   iop  = mkMULF(ity);
      IROp   iopn = mkNEGF(ity);
      const HChar* nm = "fnmul";
      IRExpr* resE = unop(iopn,
                          triop(iop, mkexpr(mk_get_IR_rounding_mode()),
                                getQRegLO(nn, ity), getQRegLO(mm, ity)));
      IRTemp  res  = newTemp(ity);
      assign(res, resE);
      putQReg128(dd, mkV128(0));
      putQRegLO(dd, mkexpr(res));
      DIP("%s %s, %s, %s\n",
          nm, nameQRegLO(dd, ity), nameQRegLO(nn, ity), nameQRegLO(mm, ity));
      return True;
   }

#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_data_proc_3_source(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  28    23 21 20 15 14 9 4
      000 11111 ty o1 m  o0 a  n d
      The first 3 bits are really "M 0 S", but M and S are always zero.
      Decode fields: ty,o1,o0
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,24) != BITS8(0,0,0,1,1,1,1,1)) {
      return False;
   }
   UInt ty    = INSN(23,22);
   UInt bitO1 = INSN(21,21);
   UInt mm    = INSN(20,16);
   UInt bitO0 = INSN(15,15);
   UInt aa    = INSN(14,10);
   UInt nn    = INSN(9,5);
   UInt dd    = INSN(4,0);
   vassert(ty < 4);

   if (ty <= X01) {
      /* -------- 0x,0,0 FMADD  d_d_d_d, s_s_s_s -------- */
      /* -------- 0x,0,1 FMSUB  d_d_d_d, s_s_s_s -------- */
      /* -------- 0x,1,0 FNMADD d_d_d_d, s_s_s_s -------- */
      /* -------- 0x,1,1 FNMSUB d_d_d_d, s_s_s_s -------- */
      /* -------------------- F{N}M{ADD,SUB} -------------------- */
      /* 31          22   20 15 14 9 4   ix
         000 11111 0 sz 0 m  0  a  n d   0   FMADD  Fd,Fn,Fm,Fa
         000 11111 0 sz 0 m  1  a  n d   1   FMSUB  Fd,Fn,Fm,Fa
         000 11111 0 sz 1 m  0  a  n d   2   FNMADD Fd,Fn,Fm,Fa
         000 11111 0 sz 1 m  1  a  n d   3   FNMSUB Fd,Fn,Fm,Fa
         where Fx=Dx when sz=1, Fx=Sx when sz=0

                  -----SPEC------    ----IMPL----
         fmadd       a +    n * m    a + n * m
         fmsub       a + (-n) * m    a - n * m
         fnmadd   (-a) + (-n) * m    -(a + n * m)
         fnmsub   (-a) +    n * m    -(a - n * m)
      */
      Bool    isD   = (ty & 1) == 1;
      UInt    ix    = (bitO1 << 1) | bitO0;
      IRType  ity   = isD ? Ity_F64 : Ity_F32;
      IROp    opADD = mkADDF(ity);
      IROp    opSUB = mkSUBF(ity);
      IROp    opMUL = mkMULF(ity);
      IROp    opNEG = mkNEGF(ity);
      IRTemp  res   = newTemp(ity);
      IRExpr* eA    = getQRegLO(aa, ity);
      IRExpr* eN    = getQRegLO(nn, ity);
      IRExpr* eM    = getQRegLO(mm, ity);
      IRExpr* rm    = mkexpr(mk_get_IR_rounding_mode());
      IRExpr* eNxM  = triop(opMUL, rm, eN, eM);
      switch (ix) {
         case 0:  assign(res, triop(opADD, rm, eA, eNxM)); break;
         case 1:  assign(res, triop(opSUB, rm, eA, eNxM)); break;
         case 2:  assign(res, unop(opNEG, triop(opADD, rm, eA, eNxM))); break;
         case 3:  assign(res, unop(opNEG, triop(opSUB, rm, eA, eNxM))); break;
         default: vassert(0);
      }
      putQReg128(dd, mkV128(0x0000));
      putQRegLO(dd, mkexpr(res));
      const HChar* names[4] = { "fmadd", "fmsub", "fnmadd", "fnmsub" };
      DIP("%s %s, %s, %s, %s\n",
          names[ix], nameQRegLO(dd, ity), nameQRegLO(nn, ity),
                     nameQRegLO(mm, ity), nameQRegLO(aa, ity));
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_immediate(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31  28    23 21 20   12  9    4
      000 11110 ty 1  imm8 100 imm5 d
      The first 3 bits are really "M 0 S", but M and S are always zero.
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(31,24) != BITS8(0,0,0,1,1,1,1,0)
       || INSN(21,21) != 1 || INSN(12,10) != BITS3(1,0,0)) {
      return False;
   }
   UInt ty     = INSN(23,22);
   UInt imm8   = INSN(20,13);
   UInt imm5   = INSN(9,5);
   UInt dd     = INSN(4,0);

   /* ------- 00,00000: FMOV s_imm ------- */
   /* ------- 01,00000: FMOV d_imm ------- */
   if (ty <= X01 && imm5 == BITS5(0,0,0,0,0)) {
      Bool  isD  = (ty & 1) == 1;
      ULong imm  = VFPExpandImm(imm8, isD ? 64 : 32);
      if (!isD) {
         vassert(0 == (imm & 0xFFFFFFFF00000000ULL));
      }
      putQReg128(dd, mkV128(0));
      putQRegLO(dd, isD ? mkU64(imm) : mkU32(imm & 0xFFFFFFFFULL));
      DIP("fmov %s, #0x%llx\n",
          nameQRegLO(dd, isD ? Ity_F64 : Ity_F32), imm);
      return True;
   }

   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_to_fixedp_conv(/*MB_OUT*/DisResult* dres, UInt insn)
{
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   return False;
#  undef INSN
}


static
Bool dis_AdvSIMD_fp_to_from_int_conv(/*MB_OUT*/DisResult* dres, UInt insn)
{
   /* 31 30 29 28    23   21 20    18     15     9 4
      sf  0  0 11110 type 1  rmode opcode 000000 n d
      The first 3 bits are really "sf 0 S", but S is always zero.
   */
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))
   if (INSN(30,29) != BITS2(0,0)
       || INSN(28,24) != BITS5(1,1,1,1,0)
       || INSN(21,21) != 1
       || INSN(15,10) != BITS6(0,0,0,0,0,0)) {
      return False;
   }
   UInt bitSF = INSN(31,31);
   UInt ty    = INSN(23,22); // type
   UInt rm    = INSN(20,19); // rmode
   UInt op    = INSN(18,16); // opcode
   UInt nn    = INSN(9,5);
   UInt dd    = INSN(4,0);

   // op = 000, 001
   /* -------- FCVT{N,P,M,Z}{S,U} (scalar, integer) -------- */
   /*    30       23   20 18  15     9 4
      sf 00 11110 0x 1 00 000 000000 n d  FCVTNS Rd, Fn (round to
      sf 00 11110 0x 1 00 001 000000 n d  FCVTNU Rd, Fn  nearest)
      ---------------- 01 --------------  FCVTP-------- (round to +inf)
      ---------------- 10 --------------  FCVTM-------- (round to -inf)
      ---------------- 11 --------------  FCVTZ-------- (round to zero)

      Rd is Xd when sf==1, Wd when sf==0
      Fn is Dn when x==1, Sn when x==0
      20:19 carry the rounding mode, using the same encoding as FPCR
   */
   if (ty <= X01 && (op == BITS3(0,0,0) || op == BITS3(0,0,1))) {
      Bool isI64 = bitSF == 1;
      Bool isF64 = (ty & 1) == 1;
      Bool isU   = (op & 1) == 1;
      /* Decide on the IR rounding mode to use. */
      IRRoundingMode irrm = 8; /*impossible*/
      HChar ch = '?';
      switch (rm) {
         case BITS2(0,0): ch = 'n'; irrm = Irrm_NEAREST; break;
         case BITS2(0,1): ch = 'p'; irrm = Irrm_PosINF; break;
         case BITS2(1,0): ch = 'm'; irrm = Irrm_NegINF; break;
         case BITS2(1,1): ch = 'z'; irrm = Irrm_ZERO; break;
         default: vassert(0);
      }
      vassert(irrm != 8);
      /* Decide on the conversion primop, based on the source size,
         dest size and signedness (8 possibilities).  Case coding:
            F32 ->s I32   0
            F32 ->u I32   1
            F32 ->s I64   2
            F32 ->u I64   3
            F64 ->s I32   4
            F64 ->u I32   5
            F64 ->s I64   6
            F64 ->u I64   7
      */
      UInt ix = (isF64 ? 4 : 0) | (isI64 ? 2 : 0) | (isU ? 1 : 0);
      vassert(ix < 8);
      const IROp iops[8] 
         = { Iop_F32toI32S, Iop_F32toI32U, Iop_F32toI64S, Iop_F32toI64U,
             Iop_F64toI32S, Iop_F64toI32U, Iop_F64toI64S, Iop_F64toI64U };
      IROp iop = iops[ix];
      // A bit of ATCery: bounce all cases we haven't seen an example of.
      if (/* F32toI32S */
             (iop == Iop_F32toI32S && irrm == Irrm_ZERO)   /* FCVTZS Wd,Sn */
          || (iop == Iop_F32toI32S && irrm == Irrm_NegINF) /* FCVTMS Wd,Sn */
          || (iop == Iop_F32toI32S && irrm == Irrm_PosINF) /* FCVTPS Wd,Sn */
          /* F32toI32U */
          || (iop == Iop_F32toI32U && irrm == Irrm_ZERO)   /* FCVTZU Wd,Sn */
          || (iop == Iop_F32toI32U && irrm == Irrm_NegINF) /* FCVTMU Wd,Sn */
          /* F32toI64S */
          || (iop == Iop_F32toI64S && irrm == Irrm_ZERO)   /* FCVTZS Xd,Sn */
          /* F32toI64U */
          || (iop == Iop_F32toI64U && irrm == Irrm_ZERO)   /* FCVTZU Xd,Sn */
          /* F64toI32S */
          || (iop == Iop_F64toI32S && irrm == Irrm_ZERO)   /* FCVTZS Wd,Dn */
          || (iop == Iop_F64toI32S && irrm == Irrm_NegINF) /* FCVTMS Wd,Dn */
          || (iop == Iop_F64toI32S && irrm == Irrm_PosINF) /* FCVTPS Wd,Dn */
          /* F64toI32U */
          || (iop == Iop_F64toI32U && irrm == Irrm_ZERO)   /* FCVTZU Wd,Dn */
          || (iop == Iop_F64toI32U && irrm == Irrm_NegINF) /* FCVTMU Wd,Dn */
          || (iop == Iop_F64toI32U && irrm == Irrm_PosINF) /* FCVTPU Wd,Dn */
          /* F64toI64S */
          || (iop == Iop_F64toI64S && irrm == Irrm_ZERO)   /* FCVTZS Xd,Dn */
          || (iop == Iop_F64toI64S && irrm == Irrm_NegINF) /* FCVTMS Xd,Dn */
          || (iop == Iop_F64toI64S && irrm == Irrm_PosINF) /* FCVTPS Xd,Dn */
          /* F64toI64U */
          || (iop == Iop_F64toI64U && irrm == Irrm_ZERO)   /* FCVTZU Xd,Dn */
          || (iop == Iop_F64toI64U && irrm == Irrm_PosINF) /* FCVTPU Xd,Dn */
         ) {
        /* validated */
      } else {
        return False;
      }
      IRType srcTy  = isF64 ? Ity_F64 : Ity_F32;
      IRType dstTy  = isI64 ? Ity_I64 : Ity_I32;
      IRTemp src    = newTemp(srcTy);
      IRTemp dst    = newTemp(dstTy);
      assign(src, getQRegLO(nn, srcTy));
      assign(dst, binop(iop, mkU32(irrm), mkexpr(src)));
      putIRegOrZR(isI64, dd, mkexpr(dst));
      DIP("fcvt%c%c %s, %s\n", ch, isU ? 'u' : 's',
          nameIRegOrZR(isI64, dd), nameQRegLO(nn, srcTy));
      return True;
   }

   // op = 010, 011
   /* -------------- {S,U}CVTF (scalar, integer) -------------- */
   /* (ix) sf  S 28    ty   rm op  15     9 4
      0    0 0 0 11110 00 1 00 010 000000 n d  SCVTF Sd, Wn
      1    0 0 0 11110 01 1 00 010 000000 n d  SCVTF Dd, Wn
      2    1 0 0 11110 00 1 00 010 000000 n d  SCVTF Sd, Xn
      3    1 0 0 11110 01 1 00 010 000000 n d  SCVTF Dd, Xn

      4    0 0 0 11110 00 1 00 011 000000 n d  UCVTF Sd, Wn
      5    0 0 0 11110 01 1 00 011 000000 n d  UCVTF Dd, Wn
      6    1 0 0 11110 00 1 00 011 000000 n d  UCVTF Sd, Xn
      7    1 0 0 11110 01 1 00 011 000000 n d  UCVTF Dd, Xn

      These are signed/unsigned conversion from integer registers to
      FP registers, all 4 32/64-bit combinations, rounded per FPCR.
   */
   if (ty <= X01 && rm == X00 && (op == BITS3(0,1,0) || op == BITS3(0,1,1))) {
      Bool isI64 = bitSF == 1;
      Bool isF64 = (ty & 1) == 1;
      Bool isU   = (op & 1) == 1;
      UInt ix    = (isU ? 4 : 0) | (isI64 ? 2 : 0) | (isF64 ? 1 : 0);
      const IROp ops[8]
        = { Iop_I32StoF32, Iop_I32StoF64, Iop_I64StoF32, Iop_I64StoF64,
            Iop_I32UtoF32, Iop_I32UtoF64, Iop_I64UtoF32, Iop_I64UtoF64 };
      IRExpr* src = getIRegOrZR(isI64, nn);
      IRExpr* res = (isF64 && !isI64) 
                       ? unop(ops[ix], src)
                       : binop(ops[ix], mkexpr(mk_get_IR_rounding_mode()), src);
      putQReg128(dd, mkV128(0));
      putQRegLO(dd, res);
      DIP("%ccvtf %s, %s\n",
          isU ? 'u' : 's', nameQRegLO(dd, isF64 ? Ity_F64 : Ity_F32), 
          nameIRegOrZR(isI64, nn));
      return True;
   }

   // op = 110, 111
   /* -------- FMOV (general) -------- */
   /* case sf  S       ty   rm op  15     9 4
       (1) 0 0 0 11110 00 1 00 111 000000 n d     FMOV Sd,      Wn
       (2) 1 0 0 11110 01 1 00 111 000000 n d     FMOV Dd,      Xn
       (3) 1 0 0 11110 10 1 01 111 000000 n d     FMOV Vd.D[1], Xn

       (4) 0 0 0 11110 00 1 00 110 000000 n d     FMOV Wd, Sn
       (5) 1 0 0 11110 01 1 00 110 000000 n d     FMOV Xd, Dn
       (6) 1 0 0 11110 10 1 01 110 000000 n d     FMOV Xd, Vn.D[1]
   */
   if (1) {
      UInt ix = 0; // case
      if (bitSF == 0) {
         if (ty == BITS2(0,0) && rm == BITS2(0,0) && op == BITS3(1,1,1))
            ix = 1;
         else
         if (ty == BITS2(0,0) && rm == BITS2(0,0) && op == BITS3(1,1,0))
            ix = 4;
      } else {
         vassert(bitSF == 1);
         if (ty == BITS2(0,1) && rm == BITS2(0,0) && op == BITS3(1,1,1))
            ix = 2;
         else
         if (ty == BITS2(0,1) && rm == BITS2(0,0) && op == BITS3(1,1,0))
            ix = 5;
         else
         if (ty == BITS2(1,0) && rm == BITS2(0,1) && op == BITS3(1,1,1))
            ix = 3;
         else
         if (ty == BITS2(1,0) && rm == BITS2(0,1) && op == BITS3(1,1,0))
            ix = 6;
      }
      if (ix > 0) {
         switch (ix) {
            case 1:
               putQReg128(dd, mkV128(0));
               putQRegLO(dd, getIReg32orZR(nn));
               DIP("fmov s%u, w%u\n", dd, nn);
               break;
            case 2:
               putQReg128(dd, mkV128(0));
               putQRegLO(dd, getIReg64orZR(nn));
               DIP("fmov d%u, x%u\n", dd, nn);
               break;
            case 3:
               putQRegHI64(dd, getIReg64orZR(nn));
               DIP("fmov v%u.d[1], x%u\n", dd, nn);
               break;
            case 4:
               putIReg32orZR(dd, getQRegLO(nn, Ity_I32));
               DIP("fmov w%u, s%u\n", dd, nn);
               break;
            case 5:
               putIReg64orZR(dd, getQRegLO(nn, Ity_I64));
               DIP("fmov x%u, d%u\n", dd, nn);
               break;
            case 6:
               putIReg64orZR(dd, getQRegHI64(nn));
               DIP("fmov x%u, v%u.d[1]\n", dd, nn);
               break;
            default:
               vassert(0);
         }
         return True;
      }
      /* undecodable; fall through */
   }

   return False;
#  undef INSN
}


static
Bool dis_ARM64_simd_and_fp(/*MB_OUT*/DisResult* dres, UInt insn)
{
   Bool ok;
   ok = dis_AdvSIMD_EXT(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_TBL_TBX(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_ZIP_UZP_TRN(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_across_lanes(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_copy(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_modified_immediate(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_copy(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_pairwise(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_shift_by_imm(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_three_different(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_three_same(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_two_reg_misc(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_scalar_x_indexed_element(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_shift_by_immediate(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_three_different(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_three_same(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_two_reg_misc(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_vector_x_indexed_elem(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_crypto_aes(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_crypto_three_reg_sha(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_crypto_two_reg_sha(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_compare(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_conditional_compare(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_conditional_select(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_data_proc_1_source(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_data_proc_2_source(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_data_proc_3_source(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_immediate(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_to_fixedp_conv(dres, insn);
   if (UNLIKELY(ok)) return True;
   ok = dis_AdvSIMD_fp_to_from_int_conv(dres, insn);
   if (UNLIKELY(ok)) return True;
   return False;
}


/*------------------------------------------------------------*/
/*--- Disassemble a single ARM64 instruction               ---*/
/*------------------------------------------------------------*/

/* Disassemble a single ARM64 instruction into IR.  The instruction
   has is located at |guest_instr| and has guest IP of
   |guest_PC_curr_instr|, which will have been set before the call
   here.  Returns True iff the instruction was decoded, in which case
   *dres will be set accordingly, or False, in which case *dres should
   be ignored by the caller. */

static
Bool disInstr_ARM64_WRK (
        /*MB_OUT*/DisResult* dres,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        UChar*       guest_instr,
        VexArchInfo* archinfo,
        VexAbiInfo*  abiinfo
     )
{
   // A macro to fish bits out of 'insn'.
#  define INSN(_bMax,_bMin)  SLICE_UInt(insn, (_bMax), (_bMin))

//ZZ    DisResult dres;
//ZZ    UInt      insn;
//ZZ    //Bool      allow_VFP = False;
//ZZ    //UInt      hwcaps = archinfo->hwcaps;
//ZZ    IRTemp    condT; /* :: Ity_I32 */
//ZZ    UInt      summary;
//ZZ    HChar     dis_buf[128];  // big enough to hold LDMIA etc text
//ZZ 
//ZZ    /* What insn variants are we supporting today? */
//ZZ    //allow_VFP  = (0 != (hwcaps & VEX_HWCAPS_ARM_VFP));
//ZZ    // etc etc

   /* Set result defaults. */
   dres->whatNext    = Dis_Continue;
   dres->len         = 4;
   dres->continueAt  = 0;
   dres->jk_StopHere = Ijk_INVALID;

   /* At least this is simple on ARM64: insns are all 4 bytes long, and
      4-aligned.  So just fish the whole thing out of memory right now
      and have done. */
   UInt insn = getUIntLittleEndianly( guest_instr );

   if (0) vex_printf("insn: 0x%x\n", insn);

   DIP("\t(arm64) 0x%llx:  ", (ULong)guest_PC_curr_instr);

   vassert(0 == (guest_PC_curr_instr & 3ULL));

   /* ----------------------------------------------------------- */

   /* Spot "Special" instructions (see comment at top of file). */
   {
      UChar* code = (UChar*)guest_instr;
      /* Spot the 16-byte preamble: 
            93CC0D8C   ror x12, x12, #3
            93CC358C   ror x12, x12, #13
            93CCCD8C   ror x12, x12, #51
            93CCF58C   ror x12, x12, #61
      */
      UInt word1 = 0x93CC0D8C;
      UInt word2 = 0x93CC358C;
      UInt word3 = 0x93CCCD8C;
      UInt word4 = 0x93CCF58C;
      if (getUIntLittleEndianly(code+ 0) == word1 &&
          getUIntLittleEndianly(code+ 4) == word2 &&
          getUIntLittleEndianly(code+ 8) == word3 &&
          getUIntLittleEndianly(code+12) == word4) {
         /* Got a "Special" instruction preamble.  Which one is it? */
         if (getUIntLittleEndianly(code+16) == 0xAA0A014A
                                               /* orr x10,x10,x10 */) {
            /* X3 = client_request ( X4 ) */
            DIP("x3 = client_request ( x4 )\n");
            putPC(mkU64( guest_PC_curr_instr + 20 ));
            dres->jk_StopHere = Ijk_ClientReq;
            dres->whatNext    = Dis_StopHere;
            return True;
         }
         else
         if (getUIntLittleEndianly(code+16) == 0xAA0B016B
                                               /* orr x11,x11,x11 */) {
            /* X3 = guest_NRADDR */
            DIP("x3 = guest_NRADDR\n");
            dres->len = 20;
            putIReg64orZR(3, IRExpr_Get( OFFB_NRADDR, Ity_I64 ));
            return True;
         }
         else
         if (getUIntLittleEndianly(code+16) == 0xAA0C018C
                                               /* orr x12,x12,x12 */) {
            /*  branch-and-link-to-noredir X8 */
            DIP("branch-and-link-to-noredir x8\n");
            putIReg64orZR(30, mkU64(guest_PC_curr_instr + 20));
            putPC(getIReg64orZR(8));
            dres->jk_StopHere = Ijk_NoRedir;
            dres->whatNext    = Dis_StopHere;
            return True;
         }
         else
         if (getUIntLittleEndianly(code+16) == 0xAA090129
                                               /* orr x9,x9,x9 */) {
            /* IR injection */
            DIP("IR injection\n");
            vex_inject_ir(irsb, Iend_LE);
            // Invalidate the current insn. The reason is that the IRop we're
            // injecting here can change. In which case the translation has to
            // be redone. For ease of handling, we simply invalidate all the
            // time.
            stmt(IRStmt_Put(OFFB_CMSTART, mkU64(guest_PC_curr_instr)));
            stmt(IRStmt_Put(OFFB_CMLEN,   mkU64(20)));
            putPC(mkU64( guest_PC_curr_instr + 20 ));
            dres->whatNext    = Dis_StopHere;
            dres->jk_StopHere = Ijk_InvalICache;
            return True;
         }
         /* We don't know what it is. */
         return False;
         /*NOTREACHED*/
      }
   }

   /* ----------------------------------------------------------- */

   /* Main ARM64 instruction decoder starts here. */

   Bool ok = False;

   /* insn[28:25] determines the top-level grouping, so let's start
      off with that.

      For all of these dis_ARM64_ functions, we pass *dres with the
      normal default results "insn OK, 4 bytes long, keep decoding" so
      they don't need to change it.  However, decodes of control-flow
      insns may cause *dres to change.
   */
   switch (INSN(28,25)) {
      case BITS4(1,0,0,0): case BITS4(1,0,0,1):
         // Data processing - immediate
         ok = dis_ARM64_data_processing_immediate(dres, insn);
         break;
      case BITS4(1,0,1,0): case BITS4(1,0,1,1):
         // Branch, exception generation and system instructions
         ok = dis_ARM64_branch_etc(dres, insn, archinfo);
         break;
      case BITS4(0,1,0,0): case BITS4(0,1,1,0):
      case BITS4(1,1,0,0): case BITS4(1,1,1,0):
         // Loads and stores
         ok = dis_ARM64_load_store(dres, insn);
         break;
      case BITS4(0,1,0,1): case BITS4(1,1,0,1):
         // Data processing - register
         ok = dis_ARM64_data_processing_register(dres, insn);
         break;
      case BITS4(0,1,1,1): case BITS4(1,1,1,1): 
         // Data processing - SIMD and floating point
         ok = dis_ARM64_simd_and_fp(dres, insn);
         break;
      case BITS4(0,0,0,0): case BITS4(0,0,0,1):
      case BITS4(0,0,1,0): case BITS4(0,0,1,1):
         // UNALLOCATED
         break;
      default:
         vassert(0); /* Can't happen */
   }

   /* If the next-level down decoders failed, make sure |dres| didn't
      get changed. */
   if (!ok) {
      vassert(dres->whatNext    == Dis_Continue);
      vassert(dres->len         == 4);
      vassert(dres->continueAt  == 0);
      vassert(dres->jk_StopHere == Ijk_INVALID);
   }

   return ok;

#  undef INSN
}


/*------------------------------------------------------------*/
/*--- Top-level fn                                         ---*/
/*------------------------------------------------------------*/

/* Disassemble a single instruction into IR.  The instruction
   is located in host memory at &guest_code[delta]. */

DisResult disInstr_ARM64 ( IRSB*        irsb_IN,
                           Bool         (*resteerOkFn) ( void*, Addr64 ),
                           Bool         resteerCisOk,
                           void*        callback_opaque,
                           UChar*       guest_code_IN,
                           Long         delta_IN,
                           Addr64       guest_IP,
                           VexArch      guest_arch,
                           VexArchInfo* archinfo,
                           VexAbiInfo*  abiinfo,
                           Bool         host_bigendian_IN,
                           Bool         sigill_diag_IN )
{
   DisResult dres;
   vex_bzero(&dres, sizeof(dres));

   /* Set globals (see top of this file) */
   vassert(guest_arch == VexArchARM64);

   irsb                = irsb_IN;
   host_is_bigendian   = host_bigendian_IN;
   guest_PC_curr_instr = (Addr64)guest_IP;

   /* Sanity checks */
   /* (x::UInt - 2) <= 15   ===   x >= 2 && x <= 17 (I hope) */
   vassert((archinfo->arm64_dMinLine_lg2_szB - 2) <= 15);
   vassert((archinfo->arm64_iMinLine_lg2_szB - 2) <= 15);

   /* Try to decode */
   Bool ok = disInstr_ARM64_WRK( &dres,
                                 resteerOkFn, resteerCisOk, callback_opaque,
                                 (UChar*)&guest_code_IN[delta_IN],
                                 archinfo, abiinfo );
   if (ok) {
      /* All decode successes end up here. */
      vassert(dres.len == 4 || dres.len == 20);
      switch (dres.whatNext) {
         case Dis_Continue:
            putPC( mkU64(dres.len + guest_PC_curr_instr) );
            break;
         case Dis_ResteerU:
         case Dis_ResteerC:
            putPC(mkU64(dres.continueAt));
            break;
         case Dis_StopHere:
            break;
         default:
            vassert(0);
      }
      DIP("\n");
   } else {
      /* All decode failures end up here. */
      if (sigill_diag_IN) {
         Int   i, j;
         UChar buf[64];
         UInt  insn
                  = getUIntLittleEndianly( (UChar*)&guest_code_IN[delta_IN] );
         vex_bzero(buf, sizeof(buf));
         for (i = j = 0; i < 32; i++) {
            if (i > 0) {
              if ((i & 7) == 0) buf[j++] = ' ';
              else if ((i & 3) == 0) buf[j++] = '\'';
            }
            buf[j++] = (insn & (1<<(31-i))) ? '1' : '0';
         }
         vex_printf("disInstr(arm64): unhandled instruction 0x%08x\n", insn);
         vex_printf("disInstr(arm64): %s\n", buf);
      }

      /* Tell the dispatcher that this insn cannot be decoded, and so
         has not been executed, and (is currently) the next to be
         executed.  PC should be up-to-date since it is made so at the
         start of each insn, but nevertheless be paranoid and update
         it again right now. */
      putPC( mkU64(guest_PC_curr_instr) );
      dres.whatNext    = Dis_StopHere;
      dres.len         = 0;
      dres.continueAt  = 0;
      dres.jk_StopHere = Ijk_NoDecode;
   }
   return dres;
}


/*--------------------------------------------------------------------*/
/*--- end                                       guest_arm64_toIR.c ---*/
/*--------------------------------------------------------------------*/
