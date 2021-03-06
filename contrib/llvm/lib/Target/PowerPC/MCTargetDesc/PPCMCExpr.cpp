//===-- PPCMCExpr.cpp - PPC specific MC expression classes ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PPCMCExpr.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectStreamer.h"

using namespace llvm;

#define DEBUG_TYPE "ppcmcexpr"

const PPCMCExpr*
PPCMCExpr::Create(VariantKind Kind, const MCExpr *Expr,
                  bool isDarwin, MCContext &Ctx) {
  return new (Ctx) PPCMCExpr(Kind, Expr, isDarwin);
}

void PPCMCExpr::PrintImpl(raw_ostream &OS) const {
  if (isDarwinSyntax()) {
    switch (Kind) {
    default: llvm_unreachable("Invalid kind!");
    case VK_PPC_LO: OS << "lo16"; break;
    case VK_PPC_HI: OS << "hi16"; break;
    case VK_PPC_HA: OS << "ha16"; break;
    }

    OS << '(';
    getSubExpr()->print(OS);
    OS << ')';
  } else {
    getSubExpr()->print(OS);

    switch (Kind) {
    default: llvm_unreachable("Invalid kind!");
    case VK_PPC_LO: OS << "@l"; break;
    case VK_PPC_HI: OS << "@h"; break;
    case VK_PPC_HA: OS << "@ha"; break;
    case VK_PPC_HIGHER: OS << "@higher"; break;
    case VK_PPC_HIGHERA: OS << "@highera"; break;
    case VK_PPC_HIGHEST: OS << "@highest"; break;
    case VK_PPC_HIGHESTA: OS << "@highesta"; break;
    }
  }
}

bool
PPCMCExpr::EvaluateAsRelocatableImpl(MCValue &Res,
                                     const MCAsmLayout *Layout) const {
  MCValue Value;

  if (!getSubExpr()->EvaluateAsRelocatable(Value, Layout))
    return false;

  if (Value.isAbsolute()) {
    int64_t Result = Value.getConstant();
    switch (Kind) {
      default:
        llvm_unreachable("Invalid kind!");
      case VK_PPC_LO:
        Result = Result & 0xffff;
        break;
      case VK_PPC_HI:
        Result = (Result >> 16) & 0xffff;
        break;
      case VK_PPC_HA:
        Result = ((Result + 0x8000) >> 16) & 0xffff;
        break;
      case VK_PPC_HIGHER:
        Result = (Result >> 32) & 0xffff;
        break;
      case VK_PPC_HIGHERA:
        Result = ((Result + 0x8000) >> 32) & 0xffff;
        break;
      case VK_PPC_HIGHEST:
        Result = (Result >> 48) & 0xffff;
        break;
      case VK_PPC_HIGHESTA:
        Result = ((Result + 0x8000) >> 48) & 0xffff;
        break;
    }
    Res = MCValue::get(Result);
  } else {
    if (!Layout)
      return false;

    MCContext &Context = Layout->getAssembler().getContext();
    const MCSymbolRefExpr *Sym = Value.getSymA();
    MCSymbolRefExpr::VariantKind Modifier = Sym->getKind();
    if (Modifier != MCSymbolRefExpr::VK_None)
      return false;
    switch (Kind) {
      default:
        llvm_unreachable("Invalid kind!");
      case VK_PPC_LO:
        Modifier = MCSymbolRefExpr::VK_PPC_LO;
        break;
      case VK_PPC_HI:
        Modifier = MCSymbolRefExpr::VK_PPC_HI;
        break;
      case VK_PPC_HA:
        Modifier = MCSymbolRefExpr::VK_PPC_HA;
        break;
      case VK_PPC_HIGHERA:
        Modifier = MCSymbolRefExpr::VK_PPC_HIGHERA;
        break;
      case VK_PPC_HIGHER:
        Modifier = MCSymbolRefExpr::VK_PPC_HIGHER;
        break;
      case VK_PPC_HIGHEST:
        Modifier = MCSymbolRefExpr::VK_PPC_HIGHEST;
        break;
      case VK_PPC_HIGHESTA:
        Modifier = MCSymbolRefExpr::VK_PPC_HIGHESTA;
        break;
    }
    Sym = MCSymbolRefExpr::Create(&Sym->getSymbol(), Modifier, Context);
    Res = MCValue::get(Sym, Value.getSymB(), Value.getConstant());
  }

  return true;
}

void PPCMCExpr::visitUsedExpr(MCStreamer &Streamer) const {
  Streamer.visitUsedExpr(*getSubExpr());
}
