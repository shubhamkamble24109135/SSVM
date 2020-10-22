// SPDX-License-Identifier: Apache-2.0
//===-- ssvm/runtime/instance/function.h - Function Instance definition ---===//
//
// Part of the SSVM Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the function instance definition in store manager.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "ast/instruction.h"
#include "module.h"
#include "runtime/hostfunc.h"

#include <memory>
#include <string>
#include <vector>

namespace SSVM {
namespace Runtime {
namespace Instance {

class FunctionInstance {
public:
  using CompiledFunction = void;

  FunctionInstance() = delete;
  /// Constructor for native function.
  FunctionInstance(const uint32_t ModAddr, const FType &Type,
                   Span<const std::pair<uint32_t, ValType>> Locs,
                   const AST::InstrVec &Expr)
      : ModuleAddr(ModAddr), FuncType(Type),
        Data(std::in_place_type_t<WasmFunction>(), Locs, Expr) {}
  /// Constructor for host function. Module address will not be used.
  FunctionInstance(std::unique_ptr<HostFunctionBase> &&Func)
      : ModuleAddr(0), FuncType(Func->getFuncType()),
        Data(std::in_place_type_t<std::unique_ptr<HostFunctionBase>>(),
             std::move(Func)) {}
  virtual ~FunctionInstance() = default;

  /// Getter of checking is host function.
  bool isHostFunction() const {
    return std::holds_alternative<std::unique_ptr<HostFunctionBase>>(Data);
  }

  /// Getter of module address of this function instance.
  uint32_t getModuleAddr() const { return ModuleAddr; }

  /// Setter of module address of this function instance.
  void setModuleAddr(const uint32_t Addr) { ModuleAddr = Addr; }

  /// Getter of function type.
  const FType &getFuncType() const { return FuncType; }

  /// Getter of function body instrs.
  Span<const std::pair<uint32_t, ValType>> getLocals() const {
    return std::get_if<WasmFunction>(&Data)->Locals;
  }

  /// Getter of function body instrs.
  const AST::InstrVec &getInstrs() const {
    return std::get_if<WasmFunction>(&Data)->Instrs;
  }

  /// Getter of symbol
  const auto getSymbol() const noexcept {
    return std::get_if<WasmFunction>(&Data)->Symbol;
  }
  /// Setter of symbol
  void setSymbol(DLSymbol<CompiledFunction> S) noexcept {
    std::get_if<WasmFunction>(&Data)->Symbol = std::move(S);
  }

  /// Getter of host function.
  HostFunctionBase &getHostFunc() const {
    return *std::get_if<std::unique_ptr<HostFunctionBase>>(&Data)->get();
  }

private:
  /// \name Data of function instance for native function.
  /// @{
  struct WasmFunction {
    std::vector<std::pair<uint32_t, ValType>> Locals;
    AST::InstrVec Instrs;
    DLSymbol<CompiledFunction> Symbol;
    WasmFunction(Span<const std::pair<uint32_t, ValType>> Locs,
                 const AST::InstrVec &Expr)
        : Locals(Locs.begin(), Locs.end()) {
      /// Copy instructions
      for (auto &It : Expr) {
        if (auto Res = makeInstructionNode(*It.get())) {
          Instrs.push_back(std::move(*Res));
        }
      }
    }
  };
  /// @}

  uint32_t ModuleAddr;

  const FType &FuncType;

  std::variant<WasmFunction, std::unique_ptr<HostFunctionBase>> Data;
};

} // namespace Instance
} // namespace Runtime
} // namespace SSVM
