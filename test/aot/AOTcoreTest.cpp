// SPDX-License-Identifier: Apache-2.0
//===-- ssvm/test/aot/AOTcoreTest.cpp - Wasm test suites ------------------===//
//
// Part of the SSVM Project.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains tests of Wasm test suites extracted by wast2json.
/// Test Suits: https://github.com/WebAssembly/spec/tree/master/test/core
/// wast2json: https://webassembly.github.io/wabt/doc/wast2json.1.html
///
//===----------------------------------------------------------------------===//

#include "../spec/spectest.h"
#include "aot/compiler.h"
#include "common/filesystem.h"
#include "common/log.h"
#include "validator/validator.h"
#include "vm/configure.h"
#include "vm/vm.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::literals;
using namespace SSVM;
static SpecTest T(std::filesystem::u8path("../spec/testSuites"sv));

/// Parameterized testing class.
class CoreTest : public testing::TestWithParam<std::string> {};

TEST_P(CoreTest, TestSuites) {
  const auto kWorkerSize = std::thread::hardware_concurrency();
  struct alignas(32) WorkerData {
    std::packaged_task<size_t(size_t)> TaskReady{[](size_t I) { return I; }};
    std::packaged_task<Expect<void>(SSVM::VM::VM &)> NormalTask;
    std::packaged_task<Expect<std::vector<ValVariant>>(SSVM::VM::VM &)>
        ExecuteTask;
  };
  const auto [Proposal, PConf, UnitName] = T.resolve(GetParam());

  std::vector<WorkerData> WData(kWorkerSize);
  std::vector<std::thread> Workers;
  Workers.reserve(kWorkerSize);
  for (size_t I = 0; I < kWorkerSize; ++I) {
    Workers.emplace_back([PConf = std::cref(PConf), &W = WData[I]]() {
      SSVM::VM::Configure Conf;
      SSVM::VM::VM VM(PConf, Conf);
      SSVM::SpecTestModule SpecTestMod;
      VM.registerModule(SpecTestMod);

      while (true) {
        auto TaskReady = W.TaskReady.get_future();
        const auto Command = TaskReady.get();
        W.TaskReady.reset();
        switch (Command) {
        case 0:
          return;
        case 1:
          W.NormalTask(VM);
          break;
        case 2:
          W.ExecuteTask(VM);
          break;
        }
      }
    });
  }

  auto Compile = [&, PConf = std::cref(PConf)](
                     const std::string &Filename) -> Expect<std::string> {
    SSVM::Loader::Loader Loader(PConf);
    SSVM::Validator::Validator ValidatorEngine(PConf);
    SSVM::AOT::Compiler Compiler;
    Compiler.setOptimizationLevel(SSVM::AOT::Compiler::OptimizationLevel::O0);
    Compiler.setDumpIR(true);
    auto Path = std::filesystem::u8path(Filename);
    Path.replace_extension(std::filesystem::u8path(".so"sv));
    const auto SOPath = Path.u8string();
    auto Data = *Loader.loadFile(Filename);
    auto Module = *Loader.parseModule(Data);
    if (auto Res = ValidatorEngine.validate(*Module); !Res) {
      return Unexpect(Res);
    }
    if (auto Res = Compiler.compile(Data, *Module, SOPath); !Res) {
      return Unexpect(Res);
    }
    return SOPath;
  };
  auto CallVM =
      [&WData](
          std::function<Expect<void>(SSVM::VM::VM &)> Task) -> Expect<void> {
    std::vector<std::future<Expect<void>>> Futures;
    Futures.reserve(WData.size());
    for (auto &W : WData) {
      W.NormalTask = std::packaged_task<Expect<void>(SSVM::VM::VM &)>(Task);
      Futures.emplace_back(W.NormalTask.get_future());
    }
    for (auto &W : WData) {
      W.TaskReady(1);
    }
    for (auto &Future : Futures) {
      Future.wait();
    }
    std::vector<Expect<void>> Results;
    Results.reserve(WData.size());
    for (auto &Future : Futures) {
      Results.push_back(Future.get());
    }
    return std::move(Results[0]);
  };
  auto ExecuteVM =
      [&WData](
          std::function<Expect<std::vector<ValVariant>>(SSVM::VM::VM &)> Task)
      -> Expect<std::vector<ValVariant>> {
    std::vector<std::future<Expect<std::vector<ValVariant>>>> Futures;
    Futures.reserve(WData.size());
    for (auto &W : WData) {
      W.ExecuteTask =
          std::packaged_task<Expect<std::vector<ValVariant>>(SSVM::VM::VM &)>(
              Task);
      Futures.emplace_back(W.ExecuteTask.get_future());
    }
    for (auto &W : WData) {
      W.TaskReady(2);
    }
    for (auto &Future : Futures) {
      Future.wait();
    }
    std::vector<Expect<std::vector<ValVariant>>> Results;
    Results.reserve(WData.size());
    for (auto &Future : Futures) {
      Results.push_back(Future.get());
    }
    return std::move(Results[0]);
  };
  T.onModule = [&CallVM, &Compile](const std::string &ModName,
                                   const std::string &Filename) {
    return Compile(Filename).and_then(
        [&CallVM, &ModName](const std::string &SOFilename) {
          return CallVM([&ModName, &SOFilename](SSVM::VM::VM &VM) {
            if (!ModName.empty()) {
              return VM.registerModule(ModName, SOFilename);
            } else {
              return VM.loadWasm(SOFilename)
                  .and_then([&VM]() { return VM.validate(); })
                  .and_then([&VM]() { return VM.instantiate(); });
            }
          });
        });
  };
  T.onValidate = [&CallVM, &Compile](const std::string &Filename) {
    return Compile(Filename).and_then([&CallVM](const std::string &SOFilename) {
      return CallVM([&SOFilename](SSVM::VM::VM &VM) {
        return VM.loadWasm(SOFilename).and_then([&VM]() {
          return VM.validate();
        });
      });
    });
  };
  T.onInstantiate = [&CallVM, &Compile](const std::string &Filename) {
    return Compile(Filename).and_then([&CallVM](const std::string &SOFilename) {
      return CallVM([&SOFilename](SSVM::VM::VM &VM) {
        return VM.loadWasm(SOFilename)
            .and_then([&VM]() { return VM.validate(); })
            .and_then([&VM]() { return VM.instantiate(); });
      });
    });
  };
  /// Helper function to call functions.
  T.onInvoke = [&ExecuteVM](const std::string &ModName,
                            const std::string &Field,
                            const std::vector<ValVariant> &Params) {
    return ExecuteVM([&ModName, &Field, &Params](SSVM::VM::VM &VM) {
      if (!ModName.empty()) {
        /// Invoke function of named module. Named modules are registered in
        /// Store Manager.
        return VM.execute(ModName, Field, Params);
      } else {
        /// Invoke function of anonymous module. Anonymous modules are
        /// instantiated in VM.
        return VM.execute(Field, Params);
      }
    });
  };
  /// Helper function to get values.
  T.onGet = [&ExecuteVM](const std::string &ModName, const std::string &Field) {
    return ExecuteVM([&ModName, &Field](
                         SSVM::VM::VM &VM) -> Expect<std::vector<ValVariant>> {
      /// Get module instance.
      auto &Store = VM.getStoreManager();
      SSVM::Runtime::Instance::ModuleInstance *ModInst = nullptr;
      if (ModName.empty()) {
        ModInst = *Store.getActiveModule();
      } else {
        if (auto Res = Store.findModule(ModName)) {
          ModInst = *Res;
        } else {
          return Unexpect(Res);
        }
      }

      /// Get global instance.
      auto &Globs = ModInst->getGlobalExports();
      if (Globs.find(Field) == Globs.cend()) {
        return Unexpect(ErrCode::IncompatibleImportType);
      }
      uint32_t GlobAddr = Globs.find(Field)->second;
      auto *GlobInst = *Store.getGlobal(GlobAddr);

      return std::vector<SSVM::ValVariant>{GlobInst->getValue()};
    });
  };
  T.onCompare =
      [](const std::vector<std::pair<std::string, std::string>> &Expected,
         const std::vector<ValVariant> &Got) -> bool {
    if (Expected.size() != Got.size()) {
      return false;
    }
    for (size_t I = 0; I < Expected.size(); ++I) {
      const auto &[Type, E] = Expected[I];
      const auto &G = Got[I];
      /// Handle NaN case
      if (E.substr(0, 4) == "nan:"sv) {
        /// TODO: nan:canonical and nan:arithmetic
        if (Type == "f32"sv) {
          const float F = std::get<float>(G);
          if (!std::isnan(F)) {
            return false;
          }
        } else if (Type == "f64"sv) {
          const double D = std::get<double>(G);
          if (!std::isnan(D)) {
            return false;
          }
        }
      } else if (Type == "i32"sv || Type == "f32"sv) {
        const uint32_t V1 = uint32_t(std::stoul(E));
        const uint32_t V2 = std::get<uint32_t>(G);
        if (V1 != V2) {
          return false;
        }
      } else if (Type == "i64"sv || Type == "f64"sv) {
        const uint64_t V2 = uint64_t(std::stoull(E));
        const uint64_t V1 = std::get<uint64_t>(G);
        if (V1 != V2) {
          return false;
        }
      } else if (Type == "funcref"sv) {
        /// Handle reference value case
        if (E == "null"sv) {
          return SSVM::isNullRef(G);
        } else {
          if (SSVM::isNullRef(G)) {
            return false;
          }
          uint32_t V1 = SSVM::retrieveFuncIdx(G);
          uint32_t V2 = static_cast<uint32_t>(std::stoul(E));
          if (V1 != V2) {
            return false;
          }
        }
      } else if (Type == "externref"sv) {
        /// Handle reference value case
        if (E == "null"sv) {
          return SSVM::isNullRef(G);
        } else {
          if (SSVM::isNullRef(G)) {
            return false;
          }
          /// The added 0x1 uint32_t prefix in externref index case will be
          /// discarded
          uint32_t V1 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
              &SSVM::retrieveExternRef<uint32_t>(G)));
          uint32_t V2 = static_cast<uint32_t>(std::stoul(E));
          if (V1 != V2) {
            return false;
          }
        }
      } else {
        assert(false);
      }
    }
    return true;
  };
  T.onStringContains = [](const std::string &Expected,
                          const std::string &Got) -> bool {
    if (Expected.rfind(Got, 0) != 0) {
      std::cout << "   ##### expected text : " << Expected << '\n';
      std::cout << "   ######## error text : " << Got << '\n';
      return false;
    }
    return true;
  };

  T.run(Proposal, UnitName);

  for (auto &W : WData) {
    W.TaskReady(0);
  }
  for (size_t I = 0; I < kWorkerSize; ++I) {
    Workers[I].join();
  }
}

/// Initiate test suite.
INSTANTIATE_TEST_SUITE_P(TestUnit, CoreTest, testing::ValuesIn(T.enumerate()));
} // namespace

GTEST_API_ int main(int argc, char **argv) {
  SSVM::Log::setErrorLoggingLevel();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
