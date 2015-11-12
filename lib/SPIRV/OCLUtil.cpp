//===- OCLUtil.cpp - OCL Utilities ----------------------------------------===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
// This file implements OCL utility functions.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "oclutil"

#include "SPIRVInternal.h"
#include "OCLUtil.h"
#include "SPIRVEntry.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace SPIRV;

namespace OCLUtil {

cl::opt<enum SPIRAddressSpace>
ReservedIdAddrSpaceForOutput("spirv-reserved-id-addr-space",
    cl::desc("Addr space of reserved id for output"), cl::init(SPIRAS_Global));

///////////////////////////////////////////////////////////////////////////////
//
// Functions for getting builtin call info
//
///////////////////////////////////////////////////////////////////////////////
AtomicWorkItemFenceLiterals getAtomicWorkItemFenceLiterals(CallInst* CI) {
  return std::make_tuple(getArgAsInt(CI, 0),
    static_cast<OCLMemOrderKind>(getArgAsInt(CI, 1)),
    static_cast<OCLScopeKind>(getArgAsInt(CI, 2)));
}

size_t getAtomicBuiltinNumMemoryOrderArgs(StringRef Name) {
  if (Name.find("compare_exchange_strong") != StringRef::npos)
    return 2;
  return 1;
}

WorkGroupBarrierLiterals getWorkGroupBarrierLiterals(CallInst* CI){
  auto N = CI->getNumArgOperands();
  assert (N == 1 || N == 3);
  return std::make_tuple(getArgAsInt(CI, 0),
    N == 1 ? OCLMS_work_group : static_cast<OCLScopeKind>(getArgAsInt(CI, 1)),
    OCLMS_work_group);
}

unsigned
getExtOp(StringRef OrigName, const std::string &GivenDemangledName) {
  std::string DemangledName = GivenDemangledName;
  if (!oclIsBuiltin(OrigName, 20,
      DemangledName.empty() ? &DemangledName : nullptr))
    return ~0U;
  DEBUG(dbgs() << "getExtOp: demangled name: " << DemangledName << '\n');
  OCLExtOpKind EOC;
  bool Found = OCLExtOpMap::rfind(DemangledName, &EOC);
  if (!Found) {
    std::string Prefix = isLastFuncParamSigned(OrigName) ? "s_" : "u_";
    Found = OCLExtOpMap::rfind(Prefix + DemangledName, &EOC);
  }
  if (Found)
    return EOC;
  else
    return ~0U;
}

std::unique_ptr<SPIRVEntry>
getSPIRVInst(const OCLBuiltinTransInfo &Info) {
  Op OC = OpNop;
  unsigned ExtOp = ~0U;
  SPIRVEntry *Entry = nullptr;
  if (OCLSPIRVBuiltinMap::find(Info.UniqName, &OC))
    Entry = SPIRVEntry::create(OC);
  else if ((ExtOp = getExtOp(Info.MangledName, Info.UniqName)) != ~0U)
    Entry = static_cast<SPIRVEntry*>(
        SPIRVEntry::create_unique(SPIRVEIS_OpenCL, ExtOp).get());
  return std::unique_ptr<SPIRVEntry>(Entry);
}

///////////////////////////////////////////////////////////////////////////////
//
// Functions for getting module info
//
///////////////////////////////////////////////////////////////////////////////

unsigned getOCLVersion(Module *M) {
  NamedMDNode *NamedMD = M->getNamedMetadata(kSPIR2MD::OCLVer);
  if (!NamedMD)
    return 0;
  assert (NamedMD->getNumOperands() == 1 && "Invalid SPIR");
  MDNode *MD = NamedMD->getOperand(0);
  unsigned Major = getMDOperandAsInt(MD, 0);
  unsigned Minor = getMDOperandAsInt(MD, 1);
  return Major * 10 + Minor;
}

void
decodeMDNode(MDNode* N, unsigned& X, unsigned& Y, unsigned& Z) {
  if (N == NULL)
    return;
  X = getMDOperandAsInt(N, 1);
  Y = getMDOperandAsInt(N, 2);
  Z = getMDOperandAsInt(N, 3);
}

/// Encode LLVM type by SPIR-V execution mode VecTypeHint
unsigned
encodeVecTypeHint(Type *Ty){
  if (Ty->isHalfTy())
    return 4;
  if (Ty->isFloatTy())
    return 5;
  if (Ty->isDoubleTy())
    return 6;
  if (IntegerType* intTy = dyn_cast<IntegerType>(Ty)) {
    switch (intTy->getIntegerBitWidth()) {
    case 8:
      return 0;
    case 16:
      return 1;
    case 32:
      return 2;
    case 64:
      return 3;
    default:
      llvm_unreachable("invalid integer type");
    }
  }
  if (VectorType* VecTy = dyn_cast<VectorType>(Ty)) {
    Type* EleTy = VecTy->getElementType();
    unsigned Size = VecTy->getVectorNumElements();
    return Size << 16 | encodeVecTypeHint(EleTy);
  }
  llvm_unreachable("invalid type");
}

Type *
decodeVecTypeHint(LLVMContext &C, unsigned code) {
  unsigned VecWidth = code >> 16;
  unsigned Scalar = code & 0xFFFF;
  Type *ST = nullptr;
  switch(Scalar) {
  case 0:
  case 1:
  case 2:
  case 3:
    ST = IntegerType::get(C, 1 << (3 + Scalar));
    break;
  case 4:
    ST = Type::getHalfTy(C);
    break;
  case 5:
    ST = Type::getFloatTy(C);
    break;
  case 6:
    ST = Type::getDoubleTy(C);
    break;
  default:
    llvm_unreachable("Invalid vec type hint");
  }
  if (VecWidth < 1)
    return ST;
  return VectorType::get(ST, VecWidth);
}

unsigned
transVecTypeHint(MDNode* Node) {
  return encodeVecTypeHint(getMDOperandAsType(Node, 1));
}

SPIRAddressSpace
getOCLOpaqueTypeAddrSpace(Op OpCode) {
  switch (OpCode) {
  case OpTypePipe:
  case OpTypeQueue:
  case OpTypeEvent:
  case OpTypeDeviceEvent:
  case OpTypeSampler:
    return SPIRAS_Global;
  case OpTypeReserveId:
    return ReservedIdAddrSpaceForOutput;
  default:
    return SPIRAS_Private;
  }
}

class OCLBuiltinFuncMangleInfo:public SPIRV::BuiltinFuncMangleInfo {
public:
  OCLBuiltinFuncMangleInfo(){}
  void init(const std::string &UniqName) {
  UnmangledName = UniqName;
  size_t Pos = std::string::npos;

  if (UnmangledName.find("async_work_group") == 0) {
    addUnsignedArg(-1);
    setArgAttr(1, SPIR::ATTR_CONST);
  } else if (UnmangledName.find("write_imageui") == 0)
      addUnsignedArg(2);
  else if (UnmangledName == "prefetch") {
    addUnsignedArg(1);
    setArgAttr(0, SPIR::ATTR_CONST);
  }
  else if (UnmangledName.find("get_") == 0 ||
      UnmangledName.find("barrier") == 0 ||
      UnmangledName.find("work_group_barrier") == 0 ||
      UnmangledName == "nan" ||
      UnmangledName == "mem_fence" ||
      UnmangledName.find("shuffle") == 0){
    addUnsignedArg(-1);
    if (UnmangledName.find("get_fence") == 0){
      setArgAttr(0, SPIR::ATTR_CONST);
      addVoidPtrArg(0);
    }
  } else if (UnmangledName.find("atomic") == 0) {
    setArgAttr(0, SPIR::ATTR_VOLATILE);
    addAtomicArg(0);
    if (UnmangledName.find("atomic_umax") == 0 ||
        UnmangledName.find("atomic_umin") == 0) {
      addUnsignedArg(0);
      UnmangledName.erase(7, 1);
    } else if (UnmangledName.find("atomic_fetch_umin") == 0 ||
               UnmangledName.find("atomic_fetch_umax") == 0) {
      addUnsignedArg(0);
      UnmangledName.erase(13, 1);
    }
  } else if (UnmangledName.find("uconvert_") == 0) {
    addUnsignedArg(0);
    UnmangledName.erase(0, 1);
  } else if (UnmangledName.find("s_") == 0) {
    UnmangledName.erase(0, 2);
  } else if (UnmangledName.find("u_") == 0) {
    addUnsignedArg(-1);
    UnmangledName.erase(0, 2);
  } else if (UnmangledName == "capture_event_profiling_info") {
    addVoidPtrArg(2);
    setEnumArg(1, SPIR::PRIMITIVE_CLK_PROFILING_INFO);
  } else if (UnmangledName == "enqueue_kernel") {
    setEnumArg(1, SPIR::PRIMITIVE_KERNEL_ENQUEUE_FLAGS_T);
    addUnsignedArg(3);
  } else if (UnmangledName == "enqueue_marker") {
    setArgAttr(2, SPIR::ATTR_CONST);
    addUnsignedArg(1);
  } else if (UnmangledName.find("vload") == 0) {
    addUnsignedArg(0);
    setArgAttr(1, SPIR::ATTR_CONST);
  } else if (UnmangledName.find("vstore") == 0 ){
    addUnsignedArg(1);
  } else if (UnmangledName.find("ndrange_") == 0) {
    addUnsignedArg(-1);
    if (UnmangledName[8] == '2' || UnmangledName[8] == '3') {
      setArgAttr(-1, SPIR::ATTR_CONST);
    }
  } else if ((Pos = UnmangledName.find("umax")) != std::string::npos ||
             (Pos = UnmangledName.find("umin")) != std::string::npos) {
    addUnsignedArg(-1);
    UnmangledName.erase(Pos, 1);
  } else if (UnmangledName.find("broadcast") != std::string::npos)
    addUnsignedArg(-1);
  else if (UnmangledName.find(kOCLBuiltinName::SampledReadImage) == 0) {
    UnmangledName.erase(0, strlen(kOCLBuiltinName::Sampled));
    addSamplerArg(1);
  }
}
};

CallInst *
mutateCallInstOCL(Module *M, CallInst *CI,
    std::function<std::string (CallInst *, std::vector<Value *> &)>ArgMutate,
    AttributeSet *Attrs) {
  OCLBuiltinFuncMangleInfo BtnInfo;
  return mutateCallInst(M, CI, ArgMutate, &BtnInfo, Attrs);
}

Instruction *
mutateCallInstOCL(Module *M, CallInst *CI,
    std::function<std::string (CallInst *, std::vector<Value *> &,
        Type *&RetTy)> ArgMutate,
    std::function<Instruction *(CallInst *)> RetMutate,
    AttributeSet *Attrs) {
  OCLBuiltinFuncMangleInfo BtnInfo;
  return mutateCallInst(M, CI, ArgMutate, RetMutate, &BtnInfo, Attrs);
}

void
mutateFunctionOCL(Function *F,
    std::function<std::string (CallInst *, std::vector<Value *> &)>ArgMutate,
    AttributeSet *Attrs) {
  OCLBuiltinFuncMangleInfo BtnInfo;
  return mutateFunction(F, ArgMutate, &BtnInfo, Attrs, false);
}

} // namespace OCLUtil

void
llvm::MangleOpenCLBuiltin(const std::string &UniqName,
    ArrayRef<Type*> ArgTypes, std::string &MangledName) {
  OCLUtil::OCLBuiltinFuncMangleInfo BtnInfo;
  MangledName = SPIRV::mangleBuiltin(UniqName, ArgTypes, &BtnInfo);
}
