/*
 * Copyright 2018 Paul Dworzanski
 * Copyright 2016-2018 Alex Beregszaszi et al.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <memory>
#include <stack>

#include "wavm.h"

#define DLL_IMPORT // Needed by wavm on some platforms
#include "Inline/Serialization.h"
#include "IR/Module.h"
#include "IR/Validate.h"
#include "Runtime/Intrinsics.h"
#include "Runtime/Linker.h"
#include "Runtime/Runtime.h"
#include "WASM/WASM.h"

#include "debugging.h"
#include "eei.h"
#include "exceptions.h"

//for benchmarking
#include <fstream>      // std::fstream
#include <time.h>       // clock_t, clock

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"

using namespace std;

namespace hera {


class WavmEthereumInterface : public EthereumInterface {
public:
  explicit WavmEthereumInterface(
    evmc_context* _context,
    vector<uint8_t> const& _code,
    evmc_message const& _msg,
    ExecutionResult & _result,
    bool _meterGas
  ):
    EthereumInterface(_context, _code, _msg, _result, _meterGas)
  {}

  void setWasmMemory(Runtime::MemoryInstance* _wasmMemory) {
    m_wasmMemory = _wasmMemory;
  }

  void nullWasmMemory() {
    m_wasmMemory = nullptr;
  }

private:
  // These assume that m_wasmMemory was set prior to execution.
  size_t memorySize() const override { return Runtime::getMemoryNumPages(m_wasmMemory) * 65536; }
  void memorySet(size_t offset, uint8_t value) override { (Runtime::memoryArrayPtr<U8>(m_wasmMemory, offset, 1))[0] = value; }
  uint8_t memoryGet(size_t offset) override { return (Runtime::memoryArrayPtr<U8>(m_wasmMemory, offset, 1))[0]; }

  Runtime::MemoryInstance* m_wasmMemory;
};

unique_ptr<WasmEngine> WavmEngine::create()
{
  return unique_ptr<WasmEngine>{new WavmEngine};
}


namespace wavm_host_module {
  // first the ethereum interface(s), the top of the stack is used in host functions
  stack<WavmEthereumInterface*> interface;

  // set up for re-throwing exceptions, this is not clean, but Hera and WAVM are incompatible without some awkwardness 
  enum class HeraExceptionsEnum {None, InternalErrorException, VMTrap, ArgumentOutOfRange, OutOfGas, ContractValidationFailure, InvalidMemoryAccess, EndExecution, StaticModeViolation};
  HeraExceptionsEnum exceptionIdx = HeraExceptionsEnum::None;
  string exceptionString;


  // the host module is called 'ethereum'
  DEFINE_INTRINSIC_MODULE(ethereum)


  // host functions follow
  DEFINE_INTRINSIC_FUNCTION(ethereum, "useGas", void, useGas, I64 amount)
  {
    interface.top()->eeiUseGas(amount);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "getAddress", void, getAddress, U32 resultOffset)
  {
    interface.top()->eeiGetAddress(resultOffset);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "call", U32, call, I64 gas, U32 addressOffset, U32 valueOffset, U32 dataOffset, U32 dataLength)
  {
    return interface.top()->eeiCall(EthereumInterface::EEICallKind::Call, gas, addressOffset, valueOffset, dataOffset, dataLength);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "callDataCopy", void, callDataCopy, U32 resultOffset, U32 dataOffset, U32 length)
  {
    interface.top()->eeiCallDataCopy(resultOffset, dataOffset, length);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "getCallDataSize", U32, getCallDataSize)
  {
    return interface.top()->eeiGetCallDataSize();
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "getGasLeft", U64, getGasLeft)
  {
    return static_cast<U64>(interface.top()->eeiGetGasLeft());
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "storageStore", void, storageStore, U32 pathOffset, U32 valueOffset)
  {
    interface.top()->eeiStorageStore(pathOffset, valueOffset);
    // print stuff for debugging
    interface.top()->debugPrintMem(1, valueOffset, 32);
    interface.top()->debugPrintStorage(1, pathOffset);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "storageLoad", void, storageLoad, U32 pathOffset, U32 valueOffset)
  {
    interface.top()->eeiStorageLoad(pathOffset, valueOffset);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "codeCopy", void, codeCopy, U32 resultOffset, U32 codeOffset, U32 length)
  {
    interface.top()->eeiCodeCopy(resultOffset, codeOffset, length);
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "getCodeSize", U32, getCodeSize)
  {
    return interface.top()->eeiGetCodeSize();
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "finish", void, finish, U32 dataOffset, U32 length)
  {
    try {
      interface.top()->eeiFinish(dataOffset, length);
    } catch (InvalidMemoryAccess const& e) {
      HERA_DEBUG<<"caught Hera's InvalidMemoryAccess\n";
      // save exception so that we can throw it again later
      exceptionIdx = HeraExceptionsEnum::InvalidMemoryAccess;
      exceptionString = e.what();
      // clear pointer to memory and trigger WAVM halt so that WAVM cleans up and exits
      interface.top()->nullWasmMemory();
      throwException(Runtime::Exception::calledUnimplementedIntrinsicType); //or maybe reachedUnreachableType
    }
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "revert", void, revert, U32 dataOffset, U32 length)
  {
    try {
      interface.top()->eeiRevert(dataOffset, length);
    } catch (InvalidMemoryAccess const& e) {
      HERA_DEBUG<<"caught Hera's InvalidMemoryAccess\n";
      // save exception so that we can throw it again later
      exceptionIdx = HeraExceptionsEnum::InvalidMemoryAccess;
      exceptionString = e.what();
      // clear pointer to memory and trigger WAVM halt so that WAVM cleans up and exits
      interface.top()->nullWasmMemory();
      throwException(Runtime::Exception::calledUnimplementedIntrinsicType); //or maybe reachedUnreachableType
    }
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "getReturnDataSize", U32, getReturnDataSize)
  {
    return interface.top()->eeiGetReturnDataSize();
  }


  DEFINE_INTRINSIC_FUNCTION(ethereum, "returnDataCopy", void, returnDataCopy, U32 resultOffset, U32 dataOffset, U32 length)
  {
    return interface.top()->eeiReturnDataCopy(resultOffset, dataOffset, length);
  }


  // this is needed for resolving names of imported host functions
  struct HeraWavmResolver : Runtime::Resolver {
    Runtime::Compartment* compartment;
    HashMap<string, Runtime::ModuleInstance*> moduleNameToInstanceMap;

    HeraWavmResolver(Runtime::Compartment* inCompartment) : compartment(inCompartment) {}

    bool resolve(const string& moduleName,
      const string& exportName,
      IR::ObjectType type,
      Runtime::Object*& outObject) override
    {
      outObject = nullptr;
      auto namedInstance = moduleNameToInstanceMap.get(moduleName);
      if (namedInstance)
          outObject = Runtime::getInstanceExport(*namedInstance, exportName);
      return outObject != nullptr;
    }
  };
} // namespace wavm_host_module

ExecutionResult WavmEngine::execute(
  evmc_context* context,
  vector<uint8_t> const& code,
  vector<uint8_t> const& state_code,
  evmc_message const& msg,
  bool meterInterfaceGas
) {

  // clear stuff for re-throwing exception
  wavm_host_module::exceptionIdx = wavm_host_module::HeraExceptionsEnum::None;
  wavm_host_module::exceptionString = "";

  // hope and prayer that this cleans up memory leak for previous run
  Runtime::collectGarbage();

  // execute the contract
  ExecutionResult result = internalExecute(context, code, state_code, msg, meterInterfaceGas);

  // clean up this run, this is done here after leaving the scope of internalExecute()
  Runtime::collectGarbage();

  // re-throw exception if there was one
  if (wavm_host_module::exceptionIdx != wavm_host_module::HeraExceptionsEnum::None){
    wavm_host_module::HeraExceptionsEnum exceptionIdx = wavm_host_module::exceptionIdx;
    string exceptionString = wavm_host_module::exceptionString;
    wavm_host_module::exceptionIdx = wavm_host_module::HeraExceptionsEnum::None;
    wavm_host_module::exceptionString = "";
    switch (exceptionIdx){
      case wavm_host_module::HeraExceptionsEnum::InternalErrorException:
        ensureCondition(false, InternalErrorException, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::VMTrap:
        ensureCondition(false, VMTrap, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::ArgumentOutOfRange:
        ensureCondition(false, ArgumentOutOfRange, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::OutOfGas:
        ensureCondition(false, OutOfGas, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::ContractValidationFailure:
        ensureCondition(false, ContractValidationFailure, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::InvalidMemoryAccess:
        ensureCondition(false, InvalidMemoryAccess, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::EndExecution:
        ensureCondition(false, EndExecution, "hera exception re-thrown")
      case wavm_host_module::HeraExceptionsEnum::StaticModeViolation:
        ensureCondition(false, StaticModeViolation, "hera exception re-thrown")
      default:
        break;
    }
  }

  return result;
}

ExecutionResult WavmEngine::internalExecute(
  evmc_context* context,
  vector<uint8_t> const& code,
  vector<uint8_t> const& state_code,
  evmc_message const& msg,
  bool meterInterfaceGas
) {
  HERA_DEBUG << "Executing with wavm...\n";

  // benchmarking compiling
  clock_t t = clock();

  // set up a new ethereum interface just for this contract invocation
  ExecutionResult result;
  WavmEthereumInterface interface{context, state_code, msg, result, meterInterfaceGas};
  wavm_host_module::interface.push(&interface);

  // first parse module
  IR::Module moduleAST;
  try {
    // NOTE: this expects U8, which is a typedef over uint8_t
    Serialization::MemoryInputStream input(code.data(), code.size());
    WASM::serialize(input, moduleAST);
  } catch (Serialization::FatalSerializationException const& e) {
    ensureCondition(false, ContractValidationFailure, "Failed to deserialise contract: " + e.message);
  } catch (IR::ValidationException const& e) {
    ensureCondition(false, ContractValidationFailure, "Failed to validate contract: " + e.message);
  } catch (std::bad_alloc const&) {
    // Catching this here because apparently wavm doesn't necessarily checks bounds before allocation
    ensureCondition(false, ContractValidationFailure, "Bug in wavm: didn't check bounds before allocation");
  }

  // next set up the host module.
  // Note: in ewasm, we create a new VM for each call to a module, so we must instantiate a new host module for each of these VMs, this is inefficient, but OK for prototyping.
  // compartment is like the Wasm store, represents the VM, has lists of globals, memories, tables, and also has wavm's runtime stuff
  Runtime::GCPointer<Runtime::Compartment> compartment = Runtime::createCompartment();
  // context stores the compartment and some other stuff
  Runtime::GCPointer<Runtime::Context> wavm_context = Runtime::createContext(compartment);
  // instantiate host Module
  HashMap<string, Runtime::Object*> extraEthereumExports; //empty for current ewasm stuff
  Runtime::GCPointer<Runtime::ModuleInstance> ethereumHostModule = Intrinsics::instantiateModule(compartment, wavm_host_module::INTRINSIC_MODULE_REF(ethereum), "ethereum", extraEthereumExports);
  heraAssert(ethereumHostModule, "Failed to create host module.");
  // prepare contract module to resolve links against host module
  wavm_host_module::HeraWavmResolver resolver(compartment);
  resolver.moduleNameToInstanceMap.set("ethereum", ethereumHostModule);
  Runtime::LinkResult linkResult = Runtime::linkModule(moduleAST, resolver);
  heraAssert(linkResult.success, "Couldn't link contract against host module.");

  // instantiate contract module
  Runtime::GCPointer<Runtime::ModuleInstance> moduleInstance = Runtime::instantiateModule(compartment, moduleAST, move(linkResult.resolvedImports), "<ewasmcontract>");
  heraAssert(moduleInstance, "Couldn't instantiate contact module.");

  // get memory for easy access in host functions
  wavm_host_module::interface.top()->setWasmMemory(asMemory(Runtime::getInstanceExport(moduleInstance, "memory")));

  // invoke the main function
  Runtime::GCPointer<Runtime::FunctionInstance> mainFunction = asFunctionNullable(Runtime::getInstanceExport(moduleInstance, "main"));
  ensureCondition(mainFunction, ContractValidationFailure, "\"main\" not found");

  // done benchmarking compiling
  t = clock() - t;
  std::fstream fs;
  fs.open ("runtime_data_wavm_compile.csv", std::fstream::out | std::fstream::app);
  fs << ", " << ((float)t)/CLOCKS_PER_SEC;
  fs.close();

  // benchmarking runtime
  clock_t t2 = clock();

  // this is how WAVM's try/catch for exceptions
  Runtime::catchRuntimeExceptions(
    [&] {
      try {
        vector<IR::Value> invokeArgs;
        Runtime::invokeFunctionChecked(wavm_context, mainFunction, invokeArgs);
      } catch (EndExecution const&) {
        HERA_DEBUG<<"caught Hera's EndExecution\n";
        // This exception is ignored here because we consider it to be a success.
        // It is only a clutch for POSIX style exit()
      }
    },
    [&](Runtime::Exception&& exception) {
      HERA_DEBUG<<"caught WAVM's Runtime::Exception\n";
      // FIXME: decide if each of the exception fit into VMTrap/InternalError
      // ensureCondition(false, VMTrap, Runtime::describeException(exception));
      if (wavm_host_module::exceptionIdx == wavm_host_module::HeraExceptionsEnum::None)
        wavm_host_module::exceptionIdx = wavm_host_module::HeraExceptionsEnum::VMTrap;
    }
  );

  // done benchmarking invokation
  t2 = clock() - t2;
  std::fstream fs2;
  fs2.open ("runtime_data_wavm_invoke.csv", std::fstream::out | std::fstream::app);
  fs2 << ", " << ((float)t2)/CLOCKS_PER_SEC;
  fs2.close();

  // clean up
  wavm_host_module::interface.top()->nullWasmMemory();
  wavm_host_module::interface.pop();

  return result;
}

} // namespace hera
