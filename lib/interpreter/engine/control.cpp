// SPDX-License-Identifier: Apache-2.0
#include "common/value.h"
#include "interpreter/interpreter.h"

#include <algorithm>

namespace SSVM {
namespace Interpreter {

Expect<void>
Interpreter::runBlockOp(Runtime::StoreManager &StoreMgr,
                        const AST::BlockControlInstruction &Instr) {
  /// Get result type for arity.
  uint32_t Locals = 0, Arity = 0;
  if (std::holds_alternative<ValType>(Instr.getBlockType())) {
    Arity = (std::get<ValType>(Instr.getBlockType()) == ValType::None) ? 0 : 1;
  } else {
    /// Get function type at index x.
    const auto *ModInst = *StoreMgr.getModule(StackMgr.getModuleAddr());
    const auto *FuncType =
        *ModInst->getFuncType(std::get<uint32_t>(Instr.getBlockType()));
    Locals = FuncType->Params.size();
    Arity = FuncType->Returns.size();
  }

  /// Create Label{ nothing } and push.
  return enterBlock(Locals, Arity, nullptr, Instr.getBody());
}

Expect<void> Interpreter::runLoopOp(Runtime::StoreManager &StoreMgr,
                                    const AST::BlockControlInstruction &Instr) {
  /// Get result type for arity.
  uint32_t Arity = 0;
  if (std::holds_alternative<uint32_t>(Instr.getBlockType())) {
    /// Get function type at index x.
    const auto *ModInst = *StoreMgr.getModule(StackMgr.getModuleAddr());
    const auto *FuncType =
        *ModInst->getFuncType(std::get<uint32_t>(Instr.getBlockType()));
    Arity = FuncType->Params.size();
  }

  /// Create Label{ loop-instruction } and push.
  return enterBlock(Arity, Arity, &Instr, Instr.getBody());
}

Expect<void>
Interpreter::runIfElseOp(Runtime::StoreManager &StoreMgr,
                         const AST::IfElseControlInstruction &Instr) {
  /// Get condition.
  uint32_t Cond = retrieveValue<uint32_t>(StackMgr.pop());

  /// Get result type for arity.
  uint32_t Locals = 0, Arity = 0;
  if (std::holds_alternative<ValType>(Instr.getBlockType())) {
    Arity = (std::get<ValType>(Instr.getBlockType()) == ValType::None) ? 0 : 1;
  } else {
    /// Get function type at index x.
    const auto *ModInst = *StoreMgr.getModule(StackMgr.getModuleAddr());
    const auto *FuncType =
        *ModInst->getFuncType(std::get<uint32_t>(Instr.getBlockType()));
    Locals = FuncType->Params.size();
    Arity = FuncType->Returns.size();
  }

  /// If non-zero, run if-statement; else, run else-statement.
  if (Cond != 0) {
    const auto &IfStatement = Instr.getIfStatement();
    if (IfStatement.size() > 1) {
      /// At least an End instruction in IfStatement.
      return enterBlock(Locals, Arity, nullptr, IfStatement);
    }
  } else {
    const auto &ElseStatement = Instr.getElseStatement();
    if (ElseStatement.size() > 1) {
      /// At least an End instruction in ElseStatement.
      return enterBlock(Locals, Arity, nullptr, ElseStatement);
    }
  }
  return {};
}

Expect<void> Interpreter::runBrOp(Runtime::StoreManager &StoreMgr,
                                  const AST::BrControlInstruction &Instr) {
  return branchToLabel(StoreMgr, Instr.getLabelIndex());
}

Expect<void> Interpreter::runBrIfOp(Runtime::StoreManager &StoreMgr,
                                    const AST::BrControlInstruction &Instr) {
  if (retrieveValue<uint32_t>(StackMgr.pop()) != 0) {
    return runBrOp(StoreMgr, Instr);
  }
  return {};
}

Expect<void>
Interpreter::runBrTableOp(Runtime::StoreManager &StoreMgr,
                          const AST::BrTableControlInstruction &Instr) {
  /// Get value on top of stack.
  uint32_t Value = retrieveValue<uint32_t>(StackMgr.pop());

  /// Do branch.
  const auto &LabelTable = Instr.getLabelList();
  if (Value < LabelTable.size()) {
    return branchToLabel(StoreMgr, LabelTable[Value]);
  }
  return branchToLabel(StoreMgr, Instr.getLabelIndex());
}

Expect<void> Interpreter::runReturnOp() {
  StackMgr.popFrame();
  return {};
}

Expect<void> Interpreter::runCallOp(Runtime::StoreManager &StoreMgr,
                                    const AST::CallControlInstruction &Instr) {
  /// Get Function address.
  const auto *ModInst = *StoreMgr.getModule(StackMgr.getModuleAddr());
  const uint32_t FuncAddr = *ModInst->getFuncAddr(Instr.getTargetIndex());
  const auto *FuncInst = *StoreMgr.getFunction(FuncAddr);
  return enterFunction(StoreMgr, *FuncInst);
}

Expect<void>
Interpreter::runCallIndirectOp(Runtime::StoreManager &StoreMgr,
                               const AST::CallControlInstruction &Instr) {
  /// Get Table Instance
  const auto *TabInst = getTabInstByIdx(StoreMgr, Instr.getTableIndex());

  /// Get function type at index x.
  const auto *ModInst = *StoreMgr.getModule(StackMgr.getModuleAddr());
  const auto *TargetFuncType = *ModInst->getFuncType(Instr.getTargetIndex());

  /// Pop the value i32.const i from the Stack.
  uint32_t Idx = retrieveValue<uint32_t>(StackMgr.pop());

  /// If idx not small than tab.elem, trap.
  if (Idx >= TabInst->getSize()) {
    LOG(ERROR) << ErrCode::UndefinedElement;
    LOG(ERROR) << ErrInfo::InfoInstruction(Instr.getOpCode(), Instr.getOffset(),
                                           {Idx},
                                           {ValTypeFromType<uint32_t>()});
    return Unexpect(ErrCode::UndefinedElement);
  }

  /// Get function address.
  uint32_t FuncAddr;
  ValVariant Ref = *TabInst->getRefAddr(Idx);
  if (isNullRef(Ref)) {
    LOG(ERROR) << ErrInfo::InfoInstruction(Instr.getOpCode(), Instr.getOffset(),
                                           {Idx},
                                           {ValTypeFromType<uint32_t>()});
    LOG(ERROR) << ErrCode::UninitializedElement;
    return Unexpect(ErrCode::UninitializedElement);
  }
  FuncAddr = retrieveFuncIdx(Ref);

  /// Check function type.
  const auto *FuncInst = *StoreMgr.getFunction(FuncAddr);
  const auto &FuncType = FuncInst->getFuncType();
  if (*TargetFuncType != FuncType) {
    LOG(ERROR) << ErrCode::IndirectCallTypeMismatch;
    LOG(ERROR) << ErrInfo::InfoInstruction(Instr.getOpCode(), Instr.getOffset(),
                                           {Idx},
                                           {ValTypeFromType<uint32_t>()});
    LOG(ERROR) << ErrInfo::InfoMismatch(TargetFuncType->Params,
                                        TargetFuncType->Returns,
                                        FuncType.Params, FuncType.Returns);
    return Unexpect(ErrCode::IndirectCallTypeMismatch);
  }
  return enterFunction(StoreMgr, *FuncInst);
}

} // namespace Interpreter
} // namespace SSVM
