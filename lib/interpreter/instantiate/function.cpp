// SPDX-License-Identifier: Apache-2.0
#include "runtime/instance/function.h"
#include "ast/section.h"
#include "interpreter/interpreter.h"
#include "runtime/instance/module.h"

namespace SSVM {
namespace Runtime {
namespace Instance {

namespace {

class Compiler {
public:
  Compiler(const Runtime::Instance::ModuleInstance &M,
           std::vector<FunctionInstance::PackedOpCode> &O,
           std::vector<ValVariant> &I)
      : ModInst(M), OpCodes(O), Intermediates(I) {}
  void compileFunction(const FType &Type, const AST::InstrVec &Instrs) {
    const auto Local = Type.Params.size();
    const auto Arity = Type.Returns.size();
    enterBlock(Local, Arity);
    compile(Instrs);
    leaveBlock(OpCodes.size(), Intermediates.size());
  }
  void compile(const AST::InstrVec &Instrs) {
    for (const auto &InstrPtr : Instrs) {
      const auto &Instr = *InstrPtr;
      OpCodes.emplace_back(Instr.getOpCode(), Instr.getOffset());
      AST::dispatchInstruction(
          Instr.getOpCode(), [this, &Instr](const auto &&Arg) {
            using InstrT = typename std::decay_t<decltype(Arg)>::type;
            if constexpr (std::is_void_v<InstrT>) {
              /// OpCode was checked in validator
              __builtin_unreachable();
            } else {
              /// Make the instruction node according to Code.
              compile(static_cast<const InstrT &>(Instr));
            }
          });
    }
  }
  void compile(const AST::ControlInstruction &Instr) {
    if (Instr.getOpCode() == OpCode::Return) {
      addJumps(Labels.size() - 1);
    }
  }
  void compile(const AST::BlockControlInstruction &Instr) {
    if (Instr.getOpCode() == OpCode::Loop) {
      compileLoopOp(Instr);
    } else if (Instr.getOpCode() == OpCode::Block) {
      compileBlockOp(Instr);
    } else {
      __builtin_unreachable();
    }
  }
  void compileBlockOp(const AST::BlockControlInstruction &Instr) {
    const auto [Local, Arity] = getLocalArity(Instr.getBlockType());
    enterBlock(Local, Arity);
    compileBlock(Instr.getBody());
    leaveBlock(OpCodes.size(), Intermediates.size());
  }
  void compileLoopOp(const AST::BlockControlInstruction &Instr) {
    const auto [Local, Arity] = getLocalArity(Instr.getBlockType());
    enterBlock(Arity, Arity);
    uint32_t PC = OpCodes.size();
    uint32_t Imm = Intermediates.size();
    compileBlock(Instr.getBody());
    leaveBlock(PC, Imm);
  }
  void compile(const AST::IfElseControlInstruction &Instr) {
    const auto [Local, Arity] = getLocalArity(Instr.getBlockType());
    enterBlock(Local, Arity);
    addJumps(0);
    compileBlock(Instr.getIfStatement());
    leaveBlock(OpCodes.size(), Intermediates.size());
    if (const auto &Else = Instr.getElseStatement(); !Else.empty()) {
      OpCodes.emplace_back(OpCode::Else, Instr.getOffset());
      enterBlock(Local, Arity);
      addJumps(0);
      compileBlock(Else);
      leaveBlock(OpCodes.size(), Intermediates.size());
    }
  }
  void compile(const AST::BrControlInstruction &Instr) {
    addJumps(Instr.getLabelIndex());
  }
  void compile(const AST::BrTableControlInstruction &Instr) {
    const auto LabelList = Instr.getLabelList();
    Intermediates.push_back(uint32_t(LabelList.size()));
    for (uint32_t Index : LabelList) {
      addJumps(Index);
    }
    addJumps(Instr.getLabelIndex());
  }
  void compile(const AST::CallControlInstruction &Instr) {
    if (Instr.getOpCode() == OpCode::Call_indirect) {
      Intermediates.push_back(uint32_t(Instr.getTableIndex()));
    }
    Intermediates.push_back(uint32_t(Instr.getTargetIndex()));
  }
  void compile(const AST::ReferenceInstruction &Instr) {
    switch (Instr.getOpCode()) {
    case OpCode::Ref__null:
      Intermediates.push_back(uint32_t(uint8_t(Instr.getReferenceType())));
      return;
    case OpCode::Ref__is_null:
      return;
    case OpCode::Ref__func:
      Intermediates.push_back(uint32_t(Instr.getTargetIndex()));
      return;
    default:
      __builtin_unreachable();
    }
  }
  void compile(const AST::ParametricInstruction &Instr) {}
  void compile(const AST::VariableInstruction &Instr) {
    Intermediates.push_back(Instr.getVariableIndex());
  }
  void compile(const AST::TableInstruction &Instr) {
    switch (Instr.getOpCode()) {
    case OpCode::Elem__drop:
      Intermediates.push_back(uint32_t(Instr.getElemIndex()));
      return;
    case OpCode::Table__get:
    case OpCode::Table__set:
    case OpCode::Table__grow:
    case OpCode::Table__size:
    case OpCode::Table__fill:
      Intermediates.push_back(uint32_t(Instr.getTargetIndex()));
      return;
    case OpCode::Table__init:
      Intermediates.push_back(uint32_t(Instr.getTargetIndex()));
      Intermediates.push_back(uint32_t(Instr.getElemIndex()));
      return;
    case OpCode::Table__copy:
      Intermediates.push_back(uint32_t(Instr.getTargetIndex()));
      Intermediates.push_back(uint32_t(Instr.getSourceIndex()));
      return;
    default:
      __builtin_unreachable();
    }
  }
  void compile(const AST::MemoryInstruction &Instr) {
    switch (Instr.getOpCode()) {
    case OpCode::I32__load:
    case OpCode::I64__load:
    case OpCode::F32__load:
    case OpCode::F64__load:
    case OpCode::I32__load8_s:
    case OpCode::I32__load8_u:
    case OpCode::I32__load16_s:
    case OpCode::I32__load16_u:
    case OpCode::I64__load8_s:
    case OpCode::I64__load8_u:
    case OpCode::I64__load16_s:
    case OpCode::I64__load16_u:
    case OpCode::I64__load32_s:
    case OpCode::I64__load32_u:
    case OpCode::I32__store:
    case OpCode::I64__store:
    case OpCode::F32__store:
    case OpCode::F64__store:
    case OpCode::I32__store8:
    case OpCode::I32__store16:
    case OpCode::I64__store8:
    case OpCode::I64__store16:
    case OpCode::I64__store32:
      Intermediates.push_back(uint32_t(Instr.getMemoryOffset()));
      return;
    case OpCode::Memory__grow:
    case OpCode::Memory__size:
    case OpCode::Memory__copy:
    case OpCode::Memory__fill:
      return;
    case OpCode::Memory__init:
    case OpCode::Data__drop:
      Intermediates.push_back(uint32_t(Instr.getDataIndex()));
      return;
    default:
      __builtin_unreachable();
    }
  }
  void compile(const AST::ConstInstruction &Instr) {
    Intermediates.push_back(Instr.getConstValue());
  }
  void compile(const AST::UnaryNumericInstruction &Instr) {}
  void compile(const AST::BinaryNumericInstruction &Instr) {}
  void compileBlock(const AST::InstrVec &Instrs) {
    auto CurrPC = OpCodes.size();
    auto CurrImm = Intermediates.size();
    auto &NextPC = Intermediates.emplace_back(UINT32_C(0));
    auto &NextImm = Intermediates.emplace_back(UINT32_C(0));
    compile(Instrs);
    NextPC = OpCodes.size() - CurrPC;
    NextImm = Intermediates.size() - CurrImm;
  }
  std::pair<uint32_t, uint32_t> getLocalArity(const BlockType &Type) {
    if (std::holds_alternative<ValType>(Type)) {
      return {0, std::get<ValType>(Type) == ValType::None ? 0 : 1};
    } else if (std::holds_alternative<uint32_t>(Type)) {
      const auto &FuncType = **ModInst.getFuncType(std::get<uint32_t>(Type));
      return {FuncType.Params.size(), FuncType.Returns.size()};
    } else {
      __builtin_unreachable();
    }
  }
  void addJumps(uint32_t Index) {
    assert(Index < Jumps.size());
    const uint32_t PC = OpCodes.size();
    const uint32_t Imm = Intermediates.size();
    auto &Label = Labels[Labels.size() - 1 - Index];
    Label.Jumps.emplace_back(Intermediates.size());
    Intermediates.emplace_back(Label.Arity);
    Intermediates.emplace_back(StackSize - Label.StackSize);
    Intermediates.emplace_back(-PC); // PCOff
    Intermediates.emplace_back(-Imm); // ImmOff
  }
  void enterBlock(uint32_t Local, uint32_t Arity) {
    Labels.emplace_back(Local, Arity);
  }
  void leaveBlock(uint32_t PC, uint32_t Imm) {
    auto &Label = Labels.back();
    Intermediates.emplace_back(Label.Arity);
    for (const uint32_t Pos : Label.Jumps) {
      Intermediates[Pos + 2] += PC;
      Intermediates[Pos + 3] += Imm;
    }
    Labels.pop_back();
  }

private:
  struct Label {
    bool Unreachable = false;
    uint32_t StackSize;
    uint32_t Local;
    uint32_t Arity;
    std::vector<uint32_t> Jumps;
    Label(uint32_t S, uint32_t L, uint32_t A) noexcept
        : StackSize(S), Local(L), Arity(A) {}
  };
  const Runtime::Instance::ModuleInstance &ModInst;
  uint32_t StackSize = 0;
  std::vector<FunctionInstance::PackedOpCode> &OpCodes;
  std::vector<ValVariant> &Intermediates;
  std::vector<Label> Labels;
};

} // namespace

/// Compile instructions. See "include/interpreter/interpreter.h".
FunctionInstance::WasmFunction::WasmFunction(
    Span<const std::pair<uint32_t, ValType>> Locs, const FType &Type,
    const Runtime::Instance::ModuleInstance &ModInst,
    const AST::InstrVec &Expr) noexcept
    : Locals(Locs.begin(), Locs.end()) {
  Compiler C(ModInst, OpCodes, Intermediates);
  C.compileFunction(Type, Expr);
}
} // namespace Instance
} // namespace Runtime

namespace Interpreter {

/// Instantiate function instance. See "include/interpreter/interpreter.h".
Expect<void> Interpreter::instantiate(
    Runtime::StoreManager &StoreMgr, Runtime::Instance::ModuleInstance &ModInst,
    const AST::FunctionSection &FuncSec, const AST::CodeSection &CodeSec) {

  /// Get the function type indices.
  auto TypeIdxs = FuncSec.getContent();
  auto CodeSegs = CodeSec.getContent();

  /// Iterate through code segments to make function instances.
  for (uint32_t I = 0; I < CodeSegs.size(); ++I) {
    /// Make a new function instance.
    auto *FuncType = *ModInst.getFuncType(TypeIdxs[I]);
    std::unique_ptr<Runtime::Instance::FunctionInstance> NewFuncInst;
    if (auto Symbol = CodeSegs[I]->getSymbol()) {
      NewFuncInst = std::make_unique<Runtime::Instance::FunctionInstance>(
          ModInst.Addr, *FuncType, std::move(Symbol));
    } else {
      NewFuncInst = std::make_unique<Runtime::Instance::FunctionInstance>(
          ModInst.Addr, *FuncType, CodeSegs[I]->getLocals(), ModInst,
          CodeSegs[I]->getInstrs());
    }

    /// Insert function instance to store manager.
    uint32_t NewFuncInstAddr;
    if (InsMode == InstantiateMode::Instantiate) {
      NewFuncInstAddr = StoreMgr.pushFunction(std::move(NewFuncInst));
    } else {
      NewFuncInstAddr = StoreMgr.importFunction(std::move(NewFuncInst));
    }
    ModInst.addFuncAddr(NewFuncInstAddr);
  }
  return {};
}

} // namespace Interpreter
} // namespace SSVM
