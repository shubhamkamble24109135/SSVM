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
  struct PackedOpCode {
    constexpr PackedOpCode(OpCode C, uint32_t O) : Code(C), Offset(O) {}
    OpCode Code;
    uint32_t Offset;
  };

  FunctionInstance() = delete;
  /// Constructor for native function.
  FunctionInstance(const uint32_t ModAddr, const FType &Type,
                   Span<const std::pair<uint32_t, ValType>> Locs,
                   const Runtime::Instance::ModuleInstance &ModInst,
                   const AST::InstrVec &Expr) noexcept
      : ModuleAddr(ModAddr), FuncType(Type),
        Data(std::in_place_type_t<WasmFunction>(), Locs, Type, ModInst, Expr) {}
  /// Constructor for compiled function.
  FunctionInstance(const uint32_t ModAddr, const FType &Type,
                   DLSymbol<CompiledFunction> S) noexcept
      : ModuleAddr(ModAddr), FuncType(Type),
        Data(std::in_place_type_t<DLSymbol<CompiledFunction>>(), std::move(S)) {
  }
  /// Constructor for host function. Module address will not be used.
  FunctionInstance(std::unique_ptr<HostFunctionBase> &&Func) noexcept
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

  /// Getter of function body OpCodes.
  Span<const PackedOpCode> getOpCodes() const {
    return std::get_if<WasmFunction>(&Data)->OpCodes;
  }

  /// Getter of function body Intermediates.
  Span<const ValVariant> getIntermediates() const {
    return std::get_if<WasmFunction>(&Data)->Intermediates;
  }

  /// Getter of symbol
  const auto getSymbol() const noexcept {
    return *std::get_if<DLSymbol<CompiledFunction>>(&Data);
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
    std::vector<PackedOpCode> OpCodes;
    std::vector<ValVariant> Intermediates;
    WasmFunction(Span<const std::pair<uint32_t, ValType>> Locs,
                 const FType &Type,
                 const Runtime::Instance::ModuleInstance &ModInst,
                 const AST::InstrVec &Expr) noexcept;
  };
  /// @}

  uint32_t ModuleAddr;

  const FType &FuncType;

  std::variant<WasmFunction, std::unique_ptr<HostFunctionBase>,
               DLSymbol<CompiledFunction>>
      Data;
};

} // namespace Instance
} // namespace Runtime
} // namespace SSVM
