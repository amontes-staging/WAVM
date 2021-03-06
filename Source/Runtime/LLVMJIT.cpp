#include "Core/Core.h"
#include "Core/Platform.h"
#include "Runtime.h"
#include "AST/AST.h"
#include "AST/ASTExpressions.h"
#include "AST/ASTDispatch.h"
#include "Intrinsics.h"
#include "Core/MemoryArena.h"

#define WITH_FUNCTION_PROLOGUE_CHECK 0
#define WITH_FUNCTION_PREFIX_CHECK 1

#ifdef _WIN32
	#pragma warning(push)
	#pragma warning (disable:4267)
	#pragma warning (disable:4800)
	#pragma warning (disable:4291)
	#pragma warning (disable:4244)
	#pragma warning (disable:4351)
	#pragma warning (disable:4065)
	#pragma warning (disable:4624)
	#pragma warning (disable:4245)	// conversion from 'int' to 'unsigned int', signed/unsigned mismatch
#endif

#include "llvm/Analysis/Passes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Host.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Vectorize.h"
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>

#ifdef _WIN32
	#pragma warning(pop)
#endif

using namespace AST;

namespace LLVMJIT
{	
	llvm::LLVMContext& context = llvm::getGlobalContext();
	bool isInitialized = false;

	// Maps a type ID to the corresponding LLVM type.
	llvm::Type* llvmTypesByTypeId[(size_t)TypeId::num];
	
	// A dummy constant to use as the unique value inhabiting the void type.
	llvm::Constant* voidDummy = nullptr;

	// Zero constants of each type.
	llvm::Constant* typedZeroConstants[(size_t)TypeId::num];

	// All the modules that have been JITted.
	std::vector<struct JITModule*> jitModules;
	
	// Converts an AST type to a LLVM type.
	llvm::Type* asLLVMType(TypeId type) { return llvmTypesByTypeId[(uintptr_t)type]; }
	
	// Converts an AST function type to a LLVM type.
	llvm::FunctionType* asLLVMType(const FunctionType& functionType,bool addFunctionSignatureArg)
	{
		size_t numExtraLLVMArgs = addFunctionSignatureArg ? 1 : 0;
		auto llvmArgTypes = (llvm::Type**)alloca(sizeof(llvm::Type*) * (functionType.parameters.size() + numExtraLLVMArgs));
		uintptr_t llvmArgIndex = 0;
		if(addFunctionSignatureArg)
		{
			llvmArgTypes[llvmArgIndex++] = llvm::Type::getInt32Ty(context);
		}
		for(uintptr_t argIndex = 0;argIndex < functionType.parameters.size();++argIndex)
		{
			llvmArgTypes[llvmArgIndex++] = asLLVMType(functionType.parameters[argIndex]);
		}
		auto llvmReturnType = asLLVMType(functionType.returnType);
		return llvm::FunctionType::get(llvmReturnType,llvm::ArrayRef<llvm::Type*>(llvmArgTypes,functionType.parameters.size() + numExtraLLVMArgs),false);
	}

	// Converts an AST name to a LLVM name. Ensures that the name is not-null, and prefixes it to ensure it doesn't conflict with export names.
	llvm::Twine getLLVMName(const char* nullableName) { return nullableName ? (llvm::Twine('_') + llvm::Twine(nullableName)) : ""; }

	// Overloaded functions that compile a literal value to a LLVM constant of the right type.
	llvm::ConstantInt* compileLiteral(uint8 value) { return (llvm::ConstantInt*)llvm::ConstantInt::get(asLLVMType(TypeId::I8),llvm::APInt(8,(uint64)value,false)); }
	llvm::ConstantInt* compileLiteral(uint16 value) { return (llvm::ConstantInt*)llvm::ConstantInt::get(asLLVMType(TypeId::I16),llvm::APInt(16,(uint64)value,false)); }
	llvm::ConstantInt* compileLiteral(uint32 value) { return (llvm::ConstantInt*)llvm::ConstantInt::get(asLLVMType(TypeId::I32),llvm::APInt(32,(uint64)value,false)); }
	llvm::ConstantInt* compileLiteral(uint64 value) { return (llvm::ConstantInt*)llvm::ConstantInt::get(asLLVMType(TypeId::I64),llvm::APInt(64,(uint64)value,false)); }
	llvm::Constant* compileLiteral(float32 value) { return llvm::ConstantFP::get(context,llvm::APFloat(value)); }
	llvm::Constant* compileLiteral(float64 value) { return llvm::ConstantFP::get(context,llvm::APFloat(value)); }
	llvm::Constant* compileLiteral(bool value) { return llvm::ConstantInt::get(asLLVMType(TypeId::Bool),llvm::APInt(1,value ? 1 : 0,false)); }
	
	// Information about a JITed module.
	struct JITModule
	{
		const Module* astModule;
		llvm::Module* llvmModule;
		std::vector<llvm::Function*> functions;
		std::vector<llvm::GlobalVariable*> globalVariablePointers;
		std::vector<llvm::GlobalVariable*> functionImportPointers;
		std::vector<llvm::GlobalVariable*> functionTablePointers;
		llvm::Value* instanceMemoryBase;
		llvm::Value* instanceMemoryAddressMask;
		llvm::ExecutionEngine* executionEngine;

		JITModule(const Module* inASTModule)
		:	astModule(inASTModule)
		,	llvmModule(new llvm::Module("",context))
		,	instanceMemoryBase(nullptr)
		,	instanceMemoryAddressMask(nullptr)
		,	executionEngine(nullptr)
		{}
	};

	// The context used by functions involved in JITing a single AST function.
	struct JITFunctionContext
	{
		JITModule& jitModule;
		const Module* astModule;
		Function* astFunction;
		llvm::Function* llvmFunction;
		llvm::IRBuilder<> irBuilder;

		llvm::Value** localVariablePointers;

		llvm::BasicBlock* unreachableBlock;
		
		// An arena for allocations that can be discarded after compiling the function.
		Memory::ScopedArena scopedArena;

		// Information about an incoming edge to a branch target.
		struct BranchResult
		{
			llvm::BasicBlock* incomingBlock;
			llvm::Value* value;
			BranchResult* next;
		};

		// Information about an in-scope branch target.
		struct BranchContext
		{
			BranchTarget* branchTarget;
			llvm::BasicBlock* basicBlock;
			BranchContext* outerContext;
			BranchResult* results;
		};
	
		// A linked list of in-scope branch targets.
		BranchContext* branchContext;

		JITFunctionContext(JITModule& inJITModule,uintptr_t functionIndex)
		: jitModule(inJITModule)
		, astModule(inJITModule.astModule)
		, astFunction(astModule->functions[functionIndex])
		, llvmFunction(jitModule.functions[functionIndex])
		, irBuilder(context)
		, localVariablePointers(nullptr)
		, branchContext(nullptr)
		{
			unreachableBlock = llvm::BasicBlock::Create(context,"unreachable",llvmFunction);
		}

		void compile();
		
		// The result of the functions called by the AST dispatcher.
		// If the inner expression doesn't return control to the outer, then it will be null.
		typedef llvm::Value* DispatchResult;
		
		// Inserts a branch, and returns the old basic block.
		llvm::BasicBlock* compileBranch(llvm::BasicBlock* dest)
		{
			auto exitBlock = irBuilder.GetInsertBlock();
			if(exitBlock == unreachableBlock) { return nullptr; }
			else
			{
				irBuilder.CreateBr(dest);
				return exitBlock;
			}
		}

		// Inserts a conditional branch, and returns the old basic block.
		llvm::BasicBlock* compileCondBranch(llvm::Value* condition,llvm::BasicBlock* trueDest,llvm::BasicBlock* falseDest)
		{
			auto exitBlock = irBuilder.GetInsertBlock();
			if(exitBlock == unreachableBlock) { return nullptr; }
			else
			{
				irBuilder.CreateCondBr(condition,trueDest,falseDest);
				return exitBlock;
			}
		}

		// Returns a LLVM intrinsic with the given id and argument types.
		DispatchResult getLLVMIntrinsic(const std::initializer_list<llvm::Type*>& argTypes,llvm::Intrinsic::ID id)
		{
			return llvm::Intrinsic::getDeclaration(jitModule.llvmModule,id,llvm::ArrayRef<llvm::Type*>(argTypes.begin(),argTypes.end()));
		}

		DispatchResult compileAddress(Expression<IntClass>* address,bool isFarAddress,TypeId memoryType)
		{
			// If the address is 32-bits, zext it to 64-bits.
			// This is crucial for security, as LLVM will otherwise implicitly sign extend it to 64-bits in the GEP below,
			// interpreting it as a signed offset and allowing access to memory outside the sandboxed memory range.
			auto byteIndex = isFarAddress ? dispatch(*this,address,TypeId::I64)
				: irBuilder.CreateZExt(dispatch(*this,address,TypeId::I32),llvm::Type::getInt64Ty(context));

			// Mask the index to the address-space size.
			auto maskedByteIndex = irBuilder.CreateAnd(byteIndex,jitModule.instanceMemoryAddressMask);

			// Cast the pointer to the appropriate type.
			auto bytePointer = irBuilder.CreateInBoundsGEP(jitModule.instanceMemoryBase,maskedByteIndex);
			return irBuilder.CreatePointerCast(bytePointer,asLLVMType(memoryType)->getPointerTo());
		}

		DispatchResult compileCall(const FunctionType& functionType,llvm::Value* function,UntypedExpression** args,bool isImport)
		{
			if(WITH_FUNCTION_PROLOGUE_CHECK && !isImport)
			{
				// Compile the parameter values for the call.
				auto llvmArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * (functionType.parameters.size() + 1));
				llvmArgs[0] = compileLiteral((uint32)0);
				for(size_t argIndex = 0;argIndex < functionType.parameters.size();++argIndex)
					{ llvmArgs[argIndex + 1] = dispatch(*this,args[argIndex],functionType.parameters[argIndex]); }
				// Create the call instruction.
				return irBuilder.CreateCall(function,llvm::ArrayRef<llvm::Value*>(llvmArgs,functionType.parameters.size() + 1));
			}
			else
			{
				// Compile the parameter values for the call.
				auto llvmArgs = (llvm::Value**)alloca(sizeof(llvm::Value*) * functionType.parameters.size());
				for(size_t argIndex = 0;argIndex < functionType.parameters.size();++argIndex)
					{ llvmArgs[argIndex] = dispatch(*this,args[argIndex],functionType.parameters[argIndex]); }
				// Create the call instruction.
				return irBuilder.CreateCall(function,llvm::ArrayRef<llvm::Value*>(llvmArgs,functionType.parameters.size()));
			}
		}
		
		template<typename Type> DispatchResult visitLiteral(const Literal<Type>* literal) { return compileLiteral(literal->value); }

		template<typename Class>
		DispatchResult visitError(TypeId type,const Error<Class>* error)
		{
			std::cerr << "Found error node while compiling:" << std::endl;
			std::cerr << error->message << std::endl;
			throw;
		}

		// Local/global get/set
		DispatchResult visitGetVariable(TypeId type,const GetVariable* getVariable,OpTypes<AnyClass>::getLocal)
		{
			assert(getVariable->variableIndex < astFunction->locals.size());
			return irBuilder.CreateLoad(localVariablePointers[getVariable->variableIndex]);
		}
		DispatchResult visitGetVariable(TypeId type,const GetVariable* getVariable,OpTypes<AnyClass>::getGlobal)
		{
			assert(getVariable->variableIndex < jitModule.globalVariablePointers.size());
			return irBuilder.CreateLoad(jitModule.globalVariablePointers[getVariable->variableIndex]);
		}
		DispatchResult visitSetVariable(const SetVariable* setVariable,OpTypes<AnyClass>::setLocal)
		{
			assert(setVariable->variableIndex < astFunction->locals.size());
			auto value = dispatch(*this,setVariable->value,astFunction->locals[setVariable->variableIndex].type);
			irBuilder.CreateStore(value,localVariablePointers[setVariable->variableIndex]);
			return value;
		}
		DispatchResult visitSetVariable(const SetVariable* setVariable,OpTypes<AnyClass>::setGlobal)
		{
			assert(setVariable->variableIndex < jitModule.globalVariablePointers.size());
			auto value = dispatch(*this,setVariable->value,astModule->globals[setVariable->variableIndex].type);
			irBuilder.CreateStore(value,jitModule.globalVariablePointers[setVariable->variableIndex]);
			return value;
		}

		// Memory load/store
		template<typename Class>
		DispatchResult visitLoad(TypeId type,const Load<Class>* load,typename OpTypes<AnyClass>::load)
		{
			assert(type == load->memoryType);
			return irBuilder.CreateLoad(compileAddress(load->address,load->isFarAddress,load->memoryType));
		}
		DispatchResult visitLoad(TypeId type,const Load<IntClass>* load,OpTypes<AnyClass>::load)
		{
			auto memoryValue = irBuilder.CreateLoad(compileAddress(load->address,load->isFarAddress,load->memoryType));
			assert(isTypeClass(load->memoryType,TypeClassId::Int));
			return type == load->memoryType ? memoryValue
				: irBuilder.CreateTrunc(memoryValue,asLLVMType(type));
		}
		DispatchResult visitLoad(TypeId type,const Load<IntClass>* load,OpTypes<IntClass>::loadZExt)
		{
			auto memoryValue = irBuilder.CreateLoad(compileAddress(load->address,load->isFarAddress,load->memoryType));
			return irBuilder.CreateZExt(memoryValue,asLLVMType(type));
		}
		DispatchResult visitLoad(TypeId type,const Load<IntClass>* load,OpTypes<IntClass>::loadSExt)
		{
			auto memoryValue = irBuilder.CreateLoad(compileAddress(load->address,load->isFarAddress,load->memoryType));
			return irBuilder.CreateSExt(memoryValue,asLLVMType(type));
		}
		template<typename Class>
		DispatchResult visitStore(const Store<Class>* store)
		{
			auto value = dispatch(*this,store->value);
			irBuilder.CreateStore(value,compileAddress(store->address,store->isFarAddress,store->memoryType));
			return value;
		}
		DispatchResult visitStore(const Store<IntClass>* store)
		{
			auto value = dispatch(*this,store->value);
			llvm::Value* memoryValue = value;
			if(store->value.type != store->memoryType)
			{
				assert(isTypeClass(store->memoryType,TypeClassId::Int));
				memoryValue = irBuilder.CreateTrunc(value,asLLVMType(store->memoryType));
			}
			irBuilder.CreateStore(memoryValue,compileAddress(store->address,store->isFarAddress,store->memoryType));
			return value;
		}

		DispatchResult visitCall(TypeId type,const Call* call,OpTypes<AnyClass>::callDirect)
		{
			auto astFunction = astModule->functions[call->functionIndex];
			assert(astFunction->type.returnType == type);
			return compileCall(astFunction->type,jitModule.functions[call->functionIndex],call->parameters,false);
		}
		DispatchResult visitCall(TypeId type,const Call* call,OpTypes<AnyClass>::callImport)
		{
			auto astFunctionImport = astModule->functionImports[call->functionIndex];
			assert(astFunctionImport.type.returnType == type);
			auto function = jitModule.functionImportPointers[call->functionIndex];
			return compileCall(astFunctionImport.type,function,call->parameters,true);
		}
		DispatchResult visitCallIndirect(TypeId type,const CallIndirect* callIndirect)
		{
			assert(callIndirect->tableIndex < astModule->functionTables.size());
			auto functionTablePointer = jitModule.functionTablePointers[callIndirect->tableIndex];
			auto astFunctionTable = astModule->functionTables[callIndirect->tableIndex];
			assert(astFunctionTable.type.returnType == type);
			assert(astFunctionTable.numFunctions > 0);

			// Compile the function index and mask it to be within the function table's bounds (which are already verified to be 2^N).
			auto functionIndex = dispatch(*this,callIndirect->functionIndex,TypeId::I32);
			auto functionIndexMask = compileLiteral((uint32)astFunctionTable.numFunctions-1);
			auto maskedFunctionIndex = irBuilder.CreateAnd(functionIndex,functionIndexMask);

			// Get a pointer to the function pointer in the function table using the masked function index.
			llvm::Value* gepIndices[2] = {compileLiteral((uint32)0),maskedFunctionIndex};

			// Load the function pointer from the table and call it.
			auto function = irBuilder.CreateLoad(irBuilder.CreateInBoundsGEP(functionTablePointer,gepIndices));

			#if WITH_FUNCTION_PREFIX_CHECK
				// Look up an I32 prefix stored with the function, and if it's not zero, call a random function of the right type instead.
				auto prefixPointer = irBuilder.CreateBitCast(function,llvm::Type::getInt32Ty(context)->getPointerTo());
				auto functionType = irBuilder.CreateLoad(irBuilder.CreateInBoundsGEP(prefixPointer,{compileLiteral((uint64)-1)}));
				auto safeFunction = irBuilder.CreateSelect(
					irBuilder.CreateICmpEQ(functionType,compileLiteral((uint32)0)),
					function,
					jitModule.functions[astFunctionTable.functionIndices[0]]
					);
			#else
				auto safeFunction = function;
			#endif

			return compileCall(astFunctionTable.type,safeFunction,callIndirect->parameters,false);
		}
		
		template<typename Class>
		DispatchResult visitSwitch(TypeId type,const Switch<Class>* switchExpression)
		{
			auto value = dispatch(*this,switchExpression->key);		

			// Create the basic blocks for each arm of the switch so they can be forward referenced by fallthrough branches.
			auto armEntryBlocks = new(scopedArena) llvm::BasicBlock*[switchExpression->numArms];
			for(uint32 armIndex = 0;armIndex < switchExpression->numArms;++armIndex)
			{ armEntryBlocks[armIndex] = llvm::BasicBlock::Create(context,"switchArm",llvmFunction); }

			// Create and link the context for this switch's branch target into the list of in-scope contexts.
			auto successorBlock = llvm::BasicBlock::Create(context,"switchSucc",llvmFunction);
			auto outerBranchContext = branchContext;
			BranchContext endBranchContext = {switchExpression->endTarget,successorBlock,outerBranchContext,nullptr};
			branchContext = &endBranchContext;
			assert(switchExpression->endTarget->type == type);

			// Compile each arm of the switch.
			assert(switchExpression->numArms > 0);
			assert(switchExpression->defaultArmIndex < switchExpression->numArms);
			auto defaultBlock = armEntryBlocks[switchExpression->defaultArmIndex];
			auto switchInstruction = irBuilder.CreateSwitch(value,defaultBlock,(uint32)switchExpression->numArms - 1);
			for(uint32 armIndex = 0;armIndex < switchExpression->numArms;++armIndex)
			{
				const SwitchArm& arm = switchExpression->arms[armIndex];
				if(armIndex != switchExpression->defaultArmIndex)
				{
					llvm::ConstantInt* armKey;
					switch(switchExpression->key.type)
					{
					case TypeId::I8: armKey = compileLiteral((uint8)arm.key); break;
					case TypeId::I16: armKey = compileLiteral((uint16)arm.key); break;
					case TypeId::I32: armKey = compileLiteral((uint32)arm.key); break;
					case TypeId::I64: armKey = compileLiteral((uint64)arm.key); break;
					default: throw;
					}
					switchInstruction->addCase(armKey,armEntryBlocks[armIndex]);
				}

				irBuilder.SetInsertPoint(armEntryBlocks[armIndex]);
				assert(arm.value);
				if(armIndex + 1 == switchExpression->numArms)
				{
					// The final arm is an expression of the same type as the switch.
					auto value = dispatch(*this,arm.value,type);
					auto exitBlock = compileBranch(successorBlock);
					if(type != TypeId::Void && exitBlock)
					{ endBranchContext.results = new(scopedArena) BranchResult {exitBlock,value,endBranchContext.results}; }
				}
				else
				{
					// The other arms yield void.
					dispatch(*this,arm.value,TypeId::Void);
					compileBranch(armEntryBlocks[armIndex + 1]);
				}
			}

			// Remove the switch's branch target from the in-scope context list.
			assert(branchContext == &endBranchContext);
			branchContext = outerBranchContext;

			irBuilder.SetInsertPoint(successorBlock);
			if(type == TypeId::Void) { return voidDummy; }
			else
			{
				// Create a phi node that merges the results from all the branches out of the switch.
				auto phi = irBuilder.CreatePHI(asLLVMType(type),(uint32)switchExpression->numArms);
				for(auto result = endBranchContext.results;result;result = result->next) { phi->addIncoming(result->value,result->incomingBlock); }
				return phi;
			}
		}
		template<typename Class>
		DispatchResult visitIfElse(TypeId type,const IfElse<Class>* ifElse)
		{
			auto condition = dispatch(*this,ifElse->condition,TypeId::Bool);

			auto trueBlock = llvm::BasicBlock::Create(context,"ifThen",llvmFunction);
			auto falseBlock = llvm::BasicBlock::Create(context,"ifElse",llvmFunction);
			auto successorBlock = llvm::BasicBlock::Create(context,"ifSucc",llvmFunction);

			compileCondBranch(condition,trueBlock,falseBlock);

			irBuilder.SetInsertPoint(trueBlock);
			auto trueValue = dispatch(*this,ifElse->thenExpression,type);
			auto trueExitBlock = compileBranch(successorBlock);

			irBuilder.SetInsertPoint(falseBlock);
			auto falseValue = dispatch(*this,ifElse->elseExpression,type);
			auto falseExitBlock = compileBranch(successorBlock);

			irBuilder.SetInsertPoint(successorBlock);
			if(type == TypeId::Void) { return voidDummy; }
			else
			{
				auto phi = irBuilder.CreatePHI(trueValue->getType(),2);
				if(trueExitBlock) { phi->addIncoming(trueValue,trueExitBlock); }
				if(falseExitBlock) { phi->addIncoming(falseValue,falseExitBlock); }
				return phi;
			}
		}
		template<typename Class>
		DispatchResult visitLabel(TypeId type,const Label<Class>* label)
		{
			auto labelBlock = llvm::BasicBlock::Create(context,"label",llvmFunction);
			auto successorBlock = llvm::BasicBlock::Create(context,"labelSucc",llvmFunction);
			
			compileBranch(labelBlock);
			irBuilder.SetInsertPoint(labelBlock);

			// Create and link the context for this label's branch target into the list of in-scope contexts.
			auto outerBranchContext = branchContext;
			BranchContext endBranchContext = {label->endTarget,successorBlock,outerBranchContext,nullptr};
			branchContext = &endBranchContext;
			
			// Compile the label's value.
			auto value = dispatch(*this,label->expression,type);

			// Remove the label's branch target from the in-scope context list.
			assert(branchContext == &endBranchContext);
			branchContext = outerBranchContext;

			// Branch to the successor block.
			auto exitBlock = compileBranch(successorBlock);
			irBuilder.SetInsertPoint(successorBlock);

			// Create a phi node that merges all the possible values yielded by the label into one.
			if(type == TypeId::Void) { return voidDummy; }
			else
			{
				auto phi = irBuilder.CreatePHI(asLLVMType(type),2);
				if(exitBlock) { phi->addIncoming(value,exitBlock); }
				for(auto result = endBranchContext.results;result;result = result->next) { phi->addIncoming(result->value,result->incomingBlock); }
				return phi;
			}
		}
		template<typename Class>
		DispatchResult visitSequence(TypeId type,const Sequence<Class>* seq)
		{
			dispatch(*this,seq->voidExpression);
			return dispatch(*this,seq->resultExpression,type);
		}
		template<typename Class>
		DispatchResult visitReturn(TypeId type,const Return<Class>* ret)
		{
			auto returnValue = astFunction->type.returnType == TypeId::Void ? nullptr
				: dispatch(*this,ret->value,astFunction->type.returnType);

			if(irBuilder.GetInsertBlock() != unreachableBlock)
			{
				if(astFunction->type.returnType == TypeId::Void) { irBuilder.CreateRetVoid(); }
				else { irBuilder.CreateRet(returnValue); }
			
				// Set the insert point to the unreachable block.
				irBuilder.SetInsertPoint(unreachableBlock);
			}

			return typedZeroConstants[(size_t)type];
		}
		template<typename Class>
		DispatchResult visitLoop(TypeId type,const Loop<Class>* loop)
		{
			auto loopBlock = llvm::BasicBlock::Create(context,"loop",llvmFunction);
			auto successorBlock = llvm::BasicBlock::Create(context,"succ",llvmFunction);
			
			// Create and link the contexts for this label's branch targets into the list of in-scope contexts.
			auto outerBranchContext = branchContext;
			BranchContext continueBranchContext = {loop->continueTarget,loopBlock,outerBranchContext,nullptr};
			BranchContext breakBranchContext = {loop->breakTarget,successorBlock,&continueBranchContext,nullptr};
			branchContext = &breakBranchContext;
			
			compileBranch(loopBlock);

			irBuilder.SetInsertPoint(loopBlock);
			dispatch(*this,loop->expression);
			compileBranch(loopBlock);
			
			// Remove the loop's branch targets from the in-scope context list.
			assert(branchContext == &breakBranchContext);
			branchContext = outerBranchContext;

			irBuilder.SetInsertPoint(successorBlock);
			if(type == TypeId::Void) { return voidDummy; }
			else
			{
				auto phi = irBuilder.CreatePHI(asLLVMType(type),1);
				for(auto result = breakBranchContext.results;result;result = result->next) { phi->addIncoming(result->value,result->incomingBlock); }
				return phi;
			}
		}
		template<typename Class>
		DispatchResult visitBranch(TypeId type,const Branch<Class>* branch)
		{
			// Find the branch target context for this branch's target.
			auto targetContext = branchContext;
			while(targetContext && targetContext->branchTarget != branch->branchTarget) { targetContext = targetContext->outerContext; }
			if(!targetContext) { throw; }
			
			// If the branch target has a non-void type, compile the branch's value.
			auto value = branch->branchTarget->type == TypeId::Void ? voidDummy
				: dispatch(*this,branch->value,targetContext->branchTarget->type);
			
			// Insert the branch instruction.
			auto exitBlock = compileBranch(targetContext->basicBlock);
			
			// Add the branch's value to the list of incoming values for the branch target.
			if(exitBlock) { targetContext->results = new(scopedArena) BranchResult {exitBlock,value,targetContext->results}; }

			// Set the insert point to the unreachable block.
			irBuilder.SetInsertPoint(unreachableBlock);

			return typedZeroConstants[(size_t)type];
		}

		DispatchResult visitNop(const Nop*)
		{
			return voidDummy;
		}
		DispatchResult visitDiscardResult(const DiscardResult* discardResult)
		{
			dispatch(*this,discardResult->expression);
			return voidDummy;
		}
		
		DispatchResult compileIntrinsic(llvm::Intrinsic::ID intrinsicId,llvm::Value* firstOperand)
		{
			auto operandType = firstOperand->getType();
			auto intrinsic = getLLVMIntrinsic({operandType},intrinsicId);
			return irBuilder.CreateCall(intrinsic,firstOperand);
		}
		DispatchResult compileIntrinsic(llvm::Intrinsic::ID intrinsicId,llvm::Value* firstOperand,llvm::Value* secondOperand)
		{
			auto intrinsic = getLLVMIntrinsic({firstOperand->getType(),secondOperand->getType()},intrinsicId);
			return irBuilder.CreateCall(intrinsic,llvm::ArrayRef<llvm::Value*>({firstOperand,secondOperand}));
		}
		
		llvm::Value* compileIntAbs(llvm::Value* operand)
		{
			auto mask = irBuilder.CreateAShr(operand,operand->getType()->getScalarSizeInBits() - 1);
			return irBuilder.CreateXor(irBuilder.CreateAdd(operand,mask),mask);
		}

		template<typename Class,typename OpAsType> DispatchResult visitUnary(TypeId type,const Unary<Class>* unary,OpAsType);
		template<typename Class,typename OpAsType> DispatchResult visitBinary(TypeId type,const Binary<Class>* binary,OpAsType);
		template<typename Class,typename OpAsType> DispatchResult visitCast(TypeId type,const Cast<Class>* cast,OpAsType);

		#define IMPLEMENT_UNARY_OP(class,op,llvmOp) \
			DispatchResult visitUnary(TypeId type,const Unary<class>* unary,OpTypes<class>::op) \
			{ \
				auto operand = dispatch(*this,unary->operand,type); \
				return llvmOp; \
			}
		#define IMPLEMENT_BINARY_OP(class,op,llvmOp) \
			DispatchResult visitBinary(TypeId type,const Binary<class>* binary,OpTypes<class>::op) \
			{ \
				auto left = dispatch(*this,binary->left,type); \
				auto right = dispatch(*this,binary->right,type); \
				return llvmOp; \
			}
		#define IMPLEMENT_CAST_OP(class,op,llvmOp) \
			DispatchResult visitCast(TypeId type,const Cast<class>* cast,OpTypes<class>::op) \
			{ \
				auto source = dispatch(*this,cast->source); \
				auto destType = asLLVMType(type); \
				return llvmOp; \
			}
		#define IMPLEMENT_COMPARE_OP(op,llvmOp) \
			DispatchResult visitComparison(const Comparison* compare,OpTypes<BoolClass>::op) \
			{ \
				auto left = dispatch(*this,compare->left,compare->operandType); \
				auto right = dispatch(*this,compare->right,compare->operandType); \
				return llvmOp; \
			}

		IMPLEMENT_UNARY_OP(IntClass,neg,irBuilder.CreateNeg(operand))
		IMPLEMENT_UNARY_OP(IntClass,abs,compileIntAbs(operand))
		IMPLEMENT_UNARY_OP(IntClass,bitwiseNot,irBuilder.CreateNot(operand))
		IMPLEMENT_UNARY_OP(IntClass,clz,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::ctlz),llvm::ArrayRef<llvm::Value*>({operand,compileLiteral(false)})))
		IMPLEMENT_UNARY_OP(IntClass,ctz,irBuilder.CreateCall(getLLVMIntrinsic({operand->getType()},llvm::Intrinsic::cttz),llvm::ArrayRef<llvm::Value*>({operand,compileLiteral(false)})))
		IMPLEMENT_UNARY_OP(IntClass,popcnt,compileIntrinsic(llvm::Intrinsic::ctpop,operand))
		IMPLEMENT_BINARY_OP(IntClass,add,irBuilder.CreateAdd(left,right))
		IMPLEMENT_BINARY_OP(IntClass,sub,irBuilder.CreateSub(left,right))
		IMPLEMENT_BINARY_OP(IntClass,mul,irBuilder.CreateMul(left,right))
		IMPLEMENT_BINARY_OP(IntClass,divs,irBuilder.CreateSDiv(left,right))
		IMPLEMENT_BINARY_OP(IntClass,divu,irBuilder.CreateUDiv(left,right))
		IMPLEMENT_BINARY_OP(IntClass,rems,irBuilder.CreateSRem(left,right))
		IMPLEMENT_BINARY_OP(IntClass,remu,irBuilder.CreateURem(left,right))
		IMPLEMENT_BINARY_OP(IntClass,bitwiseAnd,irBuilder.CreateAnd(left,right))
		IMPLEMENT_BINARY_OP(IntClass,bitwiseOr,irBuilder.CreateOr(left,right))
		IMPLEMENT_BINARY_OP(IntClass,bitwiseXor,irBuilder.CreateXor(left,right))
		IMPLEMENT_BINARY_OP(IntClass,shl,irBuilder.CreateShl(left,right))
		IMPLEMENT_BINARY_OP(IntClass,shrSExt,irBuilder.CreateAShr(left,right))
		IMPLEMENT_BINARY_OP(IntClass,shrZExt,irBuilder.CreateLShr(left,right))
		IMPLEMENT_CAST_OP(IntClass,wrap,irBuilder.CreateTrunc(source,destType))
		IMPLEMENT_CAST_OP(IntClass,truncSignedFloat,irBuilder.CreateFPToSI(source,destType))
		IMPLEMENT_CAST_OP(IntClass,truncUnsignedFloat,irBuilder.CreateFPToUI(source,destType))
		IMPLEMENT_CAST_OP(IntClass,sext,irBuilder.CreateSExt(source,destType))
		IMPLEMENT_CAST_OP(IntClass,zext,irBuilder.CreateZExt(source,destType))
		IMPLEMENT_CAST_OP(IntClass,reinterpretFloat,irBuilder.CreateBitCast(source,destType))
		IMPLEMENT_CAST_OP(IntClass,reinterpretBool,irBuilder.CreateZExt(source,destType))
		
		IMPLEMENT_UNARY_OP(FloatClass,neg,irBuilder.CreateFNeg(operand))
		IMPLEMENT_UNARY_OP(FloatClass,abs,compileIntrinsic(llvm::Intrinsic::fabs,operand))
		IMPLEMENT_UNARY_OP(FloatClass,ceil,compileIntrinsic(llvm::Intrinsic::ceil,operand))
		IMPLEMENT_UNARY_OP(FloatClass,floor,compileIntrinsic(llvm::Intrinsic::floor,operand))
		IMPLEMENT_UNARY_OP(FloatClass,trunc,compileIntrinsic(llvm::Intrinsic::trunc,operand))
		IMPLEMENT_UNARY_OP(FloatClass,nearestInt,compileIntrinsic(llvm::Intrinsic::nearbyint,operand))
		IMPLEMENT_UNARY_OP(FloatClass,sqrt,compileIntrinsic(llvm::Intrinsic::sqrt,operand))
		IMPLEMENT_BINARY_OP(FloatClass,add,irBuilder.CreateFAdd(left,right))
		IMPLEMENT_BINARY_OP(FloatClass,sub,irBuilder.CreateFSub(left,right))
		IMPLEMENT_BINARY_OP(FloatClass,mul,irBuilder.CreateFMul(left,right))
		IMPLEMENT_BINARY_OP(FloatClass,div,irBuilder.CreateFDiv(left,right))
		IMPLEMENT_BINARY_OP(FloatClass,rem,irBuilder.CreateFRem(left,right))
		IMPLEMENT_BINARY_OP(FloatClass,min,compileIntrinsic(llvm::Intrinsic::minnum,left,right))
		IMPLEMENT_BINARY_OP(FloatClass,max,compileIntrinsic(llvm::Intrinsic::maxnum,left,right))
		IMPLEMENT_BINARY_OP(FloatClass,copySign,compileIntrinsic(llvm::Intrinsic::copysign,left,right))
		IMPLEMENT_CAST_OP(FloatClass,convertSignedInt,irBuilder.CreateSIToFP(source,destType))
		IMPLEMENT_CAST_OP(FloatClass,convertUnsignedInt,irBuilder.CreateUIToFP(source,destType))
		IMPLEMENT_CAST_OP(FloatClass,promote,irBuilder.CreateFPExt(source,destType))
		IMPLEMENT_CAST_OP(FloatClass,demote,irBuilder.CreateFPTrunc(source,destType))
		IMPLEMENT_CAST_OP(FloatClass,reinterpretInt,irBuilder.CreateBitCast(source,destType))

		IMPLEMENT_UNARY_OP(BoolClass,bitwiseNot,irBuilder.CreateNot(operand))
		IMPLEMENT_BINARY_OP(BoolClass,bitwiseAnd,irBuilder.CreateAnd(left,right))
		IMPLEMENT_BINARY_OP(BoolClass,bitwiseOr,irBuilder.CreateOr(left,right))

		IMPLEMENT_COMPARE_OP(eq,isTypeClass(compare->operandType,TypeClassId::Float) ? irBuilder.CreateFCmpUEQ(left,right) : irBuilder.CreateICmpEQ(left,right))
		IMPLEMENT_COMPARE_OP(ne,isTypeClass(compare->operandType,TypeClassId::Float) ? irBuilder.CreateFCmpUNE(left,right) : irBuilder.CreateICmpNE(left,right))
		IMPLEMENT_COMPARE_OP(lt,irBuilder.CreateFCmpULT(left,right))
		IMPLEMENT_COMPARE_OP(lts,irBuilder.CreateICmpSLT(left,right))
		IMPLEMENT_COMPARE_OP(ltu,irBuilder.CreateICmpULT(left,right))
		IMPLEMENT_COMPARE_OP(le,irBuilder.CreateFCmpULE(left,right))
		IMPLEMENT_COMPARE_OP(les,irBuilder.CreateICmpSLE(left,right))
		IMPLEMENT_COMPARE_OP(leu,irBuilder.CreateICmpULE(left,right))
		IMPLEMENT_COMPARE_OP(gt,irBuilder.CreateFCmpUGT(left,right))
		IMPLEMENT_COMPARE_OP(gts,irBuilder.CreateICmpSGT(left,right))
		IMPLEMENT_COMPARE_OP(gtu,irBuilder.CreateICmpUGT(left,right))
		IMPLEMENT_COMPARE_OP(ge,irBuilder.CreateFCmpUGE(left,right))
		IMPLEMENT_COMPARE_OP(ges,irBuilder.CreateICmpSGE(left,right))
		IMPLEMENT_COMPARE_OP(geu,irBuilder.CreateICmpUGE(left,right))

		#undef IMPLEMENT_UNARY_OP
		#undef IMPLEMENT_BINARY_OP
		#undef IMPLEMENT_CAST_OP
		#undef IMPLEMENT_COMPARE_OP
	};


	struct MCJITMemoryManager : public llvm::SectionMemoryManager
	{
		// Called by MCJIT to resolve symbols by name.
		virtual uint64 getSymbolAddress(const std::string& name)
		{
			// We assume JITModule::finalize has validated the types of any imports against the intrinsic functions available.
			const Intrinsics::Function* intrinsicFunction = Intrinsics::findFunction(name.c_str());
			if(intrinsicFunction) { return reinterpret_cast<uint64>(intrinsicFunction->value); }
			
			const Intrinsics::Value* intrinsicValue = Intrinsics::findValue(name.c_str());
			if(intrinsicValue) { return reinterpret_cast<uint64>(intrinsicValue->value); }

			// Allow __chkstk through.
			if(name == "__chkstk") { return llvm::SectionMemoryManager::getSymbolAddress(name); }

			std::cerr << "getSymbolAddress: " << name << " not found" << std::endl;
			throw;
		}
	};
	
	void JITFunctionContext::compile()
	{
		// Create an initial basic block for the function.
		auto entryBasicBlock = llvm::BasicBlock::Create(context,"entry",llvmFunction);
		irBuilder.SetInsertPoint(entryBasicBlock);
		
		// Create allocas for all the locals and initialize them to zero.
		localVariablePointers = new(scopedArena) llvm::Value*[astFunction->locals.size()];
		for(uintptr_t localIndex = 0;localIndex < astFunction->locals.size();++localIndex)
		{
			auto localVariable = astFunction->locals[localIndex];
			localVariablePointers[localIndex] = irBuilder.CreateAlloca(asLLVMType(localVariable.type),nullptr,getLLVMName(localVariable.name));
			switch(localVariable.type)
			{
			case TypeId::I8: irBuilder.CreateStore(compileLiteral((uint8)0),localVariablePointers[localIndex]); break;
			case TypeId::I16: irBuilder.CreateStore(compileLiteral((uint16)0),localVariablePointers[localIndex]); break;
			case TypeId::I32: irBuilder.CreateStore(compileLiteral((uint32)0),localVariablePointers[localIndex]); break;
			case TypeId::I64: irBuilder.CreateStore(compileLiteral((uint64)0),localVariablePointers[localIndex]); break;
			case TypeId::F32: irBuilder.CreateStore(compileLiteral((float32)0.0f),localVariablePointers[localIndex]); break;
			case TypeId::F64: irBuilder.CreateStore(compileLiteral((float64)0.0),localVariablePointers[localIndex]); break;
			case TypeId::Bool: irBuilder.CreateStore(compileLiteral((bool)false),localVariablePointers[localIndex]); break;
			default: throw;
			}
		}

		// Move the function arguments into the corresponding local variable allocas.
		uintptr_t parameterIndex = 0;
		auto llvmArgIt = llvmFunction->arg_begin();
		if(WITH_FUNCTION_PROLOGUE_CHECK && llvmFunction->getLinkage() == llvm::Function::PrivateLinkage)
		{
			++llvmArgIt;
		}
		for(;llvmArgIt != llvmFunction->arg_end();++parameterIndex,++llvmArgIt)
		{
			auto localIndex = astFunction->parameterLocalIndices[parameterIndex];
			irBuilder.CreateStore(llvmArgIt,localVariablePointers[localIndex]);
		}
		
		if(WITH_FUNCTION_PROLOGUE_CHECK && llvmFunction->getLinkage() == llvm::Function::PrivateLinkage)
		{
			auto signatureCheckFailBlock = llvm::BasicBlock::Create(context,"signatureCheckFail",llvmFunction);
			auto signatureCheckSuccBlock = llvm::BasicBlock::Create(context,"signatureCheckSucc",llvmFunction);
			irBuilder.CreateCondBr(
				irBuilder.CreateICmpEQ(llvmFunction->arg_begin(),compileLiteral((uint32)0)),
				signatureCheckSuccBlock,
				signatureCheckFailBlock
				);

			irBuilder.SetInsertPoint(signatureCheckFailBlock);
			irBuilder.CreateCall(getLLVMIntrinsic({},llvm::Intrinsic::trap));
			irBuilder.CreateUnreachable();

			irBuilder.SetInsertPoint(signatureCheckSuccBlock);
		}

		// Traverse the function's expressions.
		auto value = dispatch(*this,astFunction->expression,astFunction->type.returnType);

		// If the final value of the function is reachable, return it.
		if(irBuilder.GetInsertBlock() != unreachableBlock)
		{
			if(astFunction->type.returnType == TypeId::Void) { irBuilder.CreateRetVoid(); }
			else { irBuilder.CreateRet(value); }
		}

		// Delete the unreachable block.
		unreachableBlock->eraseFromParent();
		unreachableBlock = nullptr;
	}
	
	static void init()
	{
		isInitialized = true;

		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		llvm::InitializeNativeTargetAsmParser();

		llvmTypesByTypeId[(size_t)TypeId::None] = nullptr;
		llvmTypesByTypeId[(size_t)TypeId::I8] = llvm::Type::getInt8Ty(context);
		llvmTypesByTypeId[(size_t)TypeId::I16] = llvm::Type::getInt16Ty(context);
		llvmTypesByTypeId[(size_t)TypeId::I32] = llvm::Type::getInt32Ty(context);
		llvmTypesByTypeId[(size_t)TypeId::I64] = llvm::Type::getInt64Ty(context);
		llvmTypesByTypeId[(size_t)TypeId::F32] = llvm::Type::getFloatTy(context);
		llvmTypesByTypeId[(size_t)TypeId::F64] = llvm::Type::getDoubleTy(context);
		llvmTypesByTypeId[(size_t)TypeId::Bool] = llvm::Type::getInt1Ty(context);
		llvmTypesByTypeId[(size_t)TypeId::Void] = llvm::Type::getVoidTy(context);
		
		// Create a null pointer constant to use as the void dummy value.
		voidDummy = llvm::Constant::getIntegerValue(llvm::Type::getInt8Ty(context),llvm::APInt(64,0));
		
		// Create zero constants of each type.
		typedZeroConstants[(size_t)TypeId::None] = nullptr;
		typedZeroConstants[(size_t)TypeId::I8] = compileLiteral((uint8)0);
		typedZeroConstants[(size_t)TypeId::I16] = compileLiteral((uint16)0);
		typedZeroConstants[(size_t)TypeId::I32] = compileLiteral((uint32)0);
		typedZeroConstants[(size_t)TypeId::I64] = compileLiteral((uint64)0);
		typedZeroConstants[(size_t)TypeId::F32] = compileLiteral((float32)0.0f);
		typedZeroConstants[(size_t)TypeId::F64] = compileLiteral((float64)0.0);
		typedZeroConstants[(size_t)TypeId::Bool] = compileLiteral(false);
		typedZeroConstants[(size_t)TypeId::Void] = voidDummy;
	}

	bool compileModule(const Module* astModule)
	{
		if(!isInitialized)
		{
			init();
		}

		// Create a JIT module.
		Core::Timer llvmGenTimer;
		JITModule* jitModule = new JITModule(astModule);
		jitModules.push_back(jitModule);

		// Create literals for the virtual memory base and mask.
		jitModule->instanceMemoryBase = llvm::Constant::getIntegerValue(llvm::Type::getInt8PtrTy(context),llvm::APInt(64,reinterpret_cast<uintptr_t>(Runtime::instanceMemoryBase)));
		auto instanceMemoryAddressMask = Runtime::instanceAddressSpaceMaxBytes - 1;
		jitModule->instanceMemoryAddressMask = sizeof(uintptr_t) == 8 ? compileLiteral((uint64)instanceMemoryAddressMask) : compileLiteral((uint32)instanceMemoryAddressMask);
		
		// Build a reverse map from function index to export.
		std::map<uintptr_t,const char*> functionIndexToExportNameMap;
		for(auto exportIt : astModule->exportNameToFunctionIndexMap)
		{
			assert(exportIt.second < astModule->functions.size());
			functionIndexToExportNameMap[exportIt.second] = exportIt.first;
		}

		// Create the LLVM functions.
		jitModule->functions.resize(astModule->functions.size());
		for(uintptr_t functionIndex = 0;functionIndex < astModule->functions.size();++functionIndex)
		{
			auto astFunction = astModule->functions[functionIndex];
			auto exportIt = functionIndexToExportNameMap.find(functionIndex);
			bool isExported = exportIt != functionIndexToExportNameMap.end();
			auto functionName = isExported ? llvm::Twine(exportIt->second) : getLLVMName(astFunction->name);
			auto linkage = isExported ? llvm::Function::ExternalLinkage : llvm::Function::PrivateLinkage;
			auto llvmFunctionType = asLLVMType(astFunction->type,WITH_FUNCTION_PROLOGUE_CHECK && !isExported);
			jitModule->functions[functionIndex] = llvm::Function::Create(llvmFunctionType,linkage,functionName,jitModule->llvmModule);
			#if WITH_FUNCTION_PREFIX_CHECK
				jitModule->functions[functionIndex]->setPrefixData(compileLiteral((uint32)0));
			#endif
		}

		// Create the global variables.
		jitModule->globalVariablePointers.resize(astModule->globals.size());
		for(uintptr_t importIndex = 0;importIndex < jitModule->globalVariablePointers.size();++importIndex)
		{
			auto globalVariable = astModule->globals[importIndex];
			llvm::Constant* initializer = typedZeroConstants[(size_t)globalVariable.type];
			jitModule->globalVariablePointers[importIndex] = new llvm::GlobalVariable(*jitModule->llvmModule,asLLVMType(globalVariable.type),false,llvm::GlobalValue::PrivateLinkage,initializer,getLLVMName(globalVariable.name));
		}
		
		// Set imported global variables to have external linkage and give them the unmangled import name.
		for(uintptr_t variableImportIndex = 0;variableImportIndex < astModule->variableImports.size();++variableImportIndex)
		{
			auto variableImport = astModule->variableImports[variableImportIndex];
			jitModule->globalVariablePointers[variableImport.globalIndex]->setName(variableImport.name);
			jitModule->globalVariablePointers[variableImport.globalIndex]->setLinkage(llvm::GlobalValue::ExternalLinkage);
			jitModule->globalVariablePointers[variableImport.globalIndex]->setInitializer(nullptr);
		}

		// Create the function import globals.
		jitModule->functionImportPointers.resize(astModule->functionImports.size());
		for(uintptr_t importIndex = 0;importIndex < jitModule->functionImportPointers.size();++importIndex)
		{
			auto functionImport = astModule->functionImports[importIndex];
			jitModule->functionImportPointers[importIndex] = new llvm::GlobalVariable(*jitModule->llvmModule,asLLVMType(functionImport.type,false),true,llvm::GlobalValue::ExternalLinkage,nullptr,functionImport.name);
		}

		// Create the function table globals.
		jitModule->functionTablePointers.resize(astModule->functionTables.size());
		for(uintptr_t tableIndex = 0;tableIndex < astModule->functionTables.size();++tableIndex)
		{
			auto astFunctionTable = astModule->functionTables[tableIndex];
			std::vector<llvm::Constant*> llvmFunctionTableElements;
			llvmFunctionTableElements.resize(astFunctionTable.numFunctions);
			for(uint32 functionIndex = 0;functionIndex < astFunctionTable.numFunctions;++functionIndex)
			{
				assert(astFunctionTable.functionIndices[functionIndex] < jitModule->functions.size());
				llvmFunctionTableElements[functionIndex] = jitModule->functions[astFunctionTable.functionIndices[functionIndex]];
			}
			// Verify that the number of elements is a power of two, so we can use bitwise and to prevent out-of-bounds accesses.
			assert((astFunctionTable.numFunctions & (astFunctionTable.numFunctions-1)) == 0);

			// Create a LLVM global variable that holds the array of function pointers.
			auto llvmFunctionTablePointerType = llvm::ArrayType::get(asLLVMType(astFunctionTable.type,WITH_FUNCTION_PROLOGUE_CHECK)->getPointerTo(),llvmFunctionTableElements.size());
			auto llvmFunctionTablePointer = new llvm::GlobalVariable(
				*jitModule->llvmModule,llvmFunctionTablePointerType,true,llvm::GlobalValue::PrivateLinkage,
				llvm::ConstantArray::get(llvmFunctionTablePointerType,llvmFunctionTableElements)
				);
			jitModule->functionTablePointers[tableIndex] = llvmFunctionTablePointer;
		}

		// Compile each function in the module.
		for(uintptr_t functionIndex = 0;functionIndex < astModule->functions.size();++functionIndex)
		{
			JITFunctionContext(*jitModule,functionIndex).compile();
		}
		//std::cout << "Generated LLVM code for module in " << llvmGenTimer.getMilliseconds() << "ms" << std::endl;
		
		// Work around a problem with LLVM generating a COFF file that MCJIT can't parse. Adding -elf to the target triple forces it to use ELF instead of COFF.
		// This also works around a _ being prepended to the symbol before getSymbolAddress is called on MacOS.
        jitModule->llvmModule->setTargetTriple(llvm::sys::getProcessTriple() + "-elf");

		// Create the MCJIT execution engine for this module.
		std::string errStr;
		jitModule->executionEngine = llvm::EngineBuilder(std::unique_ptr<llvm::Module>(jitModule->llvmModule))
			.setErrorStr(&errStr)
			.setOptLevel(llvm::CodeGenOpt::Aggressive)
			.setMCJITMemoryManager(std::unique_ptr<llvm::RTDyldMemoryManager>(new MCJITMemoryManager()))
			.create();
		if(!jitModule->executionEngine)
		{
			std::cerr << "Could not create ExecutionEngine: " << errStr << std::endl;
			return false;
		}
		jitModule->llvmModule->setDataLayout(*jitModule->executionEngine->getDataLayout());

		// Look up intrinsic functions that match the name+type of functions imported by the module, and bind them to the global variable used by the module to access the import.
		bool missingImport = false;
		for(uintptr_t functionImportIndex = 0;functionImportIndex < astModule->functionImports.size();++functionImportIndex)
		{
			auto functionImport = astModule->functionImports[functionImportIndex];
			const Intrinsics::Function* intrinsicFunction = Intrinsics::findFunction(functionImport.name);
			void* functionPointer = intrinsicFunction && intrinsicFunction->type == functionImport.type ? intrinsicFunction->value : nullptr;
			if(!functionPointer)
			{
				std::cerr << "Missing imported function " << functionImport.name << " : (";
				for(auto argIt = functionImport.type.parameters.begin();argIt != functionImport.type.parameters.end();++argIt)
				{
					if(argIt != functionImport.type.parameters.begin()) { std::cerr << ","; }
					std::cerr << getTypeName(*argIt);
				}
				std::cerr << ") -> " << getTypeName(functionImport.type.returnType) << std::endl;
				missingImport = true;
			}
		}

		// Look up intrinsic values that match the name+type of values imported by the module, and bind them to the global variable used by the module to access the import.
		for(uintptr_t variableImportIndex = 0;variableImportIndex < astModule->variableImports.size();++variableImportIndex)
		{
			auto variableImport = astModule->variableImports[variableImportIndex];
			const Intrinsics::Value* intrinsicValue = Intrinsics::findValue(variableImport.name);
			if(intrinsicValue && variableImport.type != intrinsicValue->type) { intrinsicValue = NULL; }
			if(!intrinsicValue)
			{
				std::cerr << "Missing imported variable " << variableImport.name << " : " << getTypeName(variableImport.type) << std::endl;
				missingImport = true;
			}
			else { jitModule->executionEngine->addGlobalMapping(jitModule->globalVariablePointers[variableImport.globalIndex],intrinsicValue->value); }
		}

		// Fail if there were any missing imports.
		if(missingImport) { return false; }

		// Verify the module.
		#ifdef _DEBUG
			std::string verifyOutputString;
			llvm::raw_string_ostream verifyOutputStream(verifyOutputString);
			if(llvm::verifyModule(*jitModule->llvmModule,&verifyOutputStream))
			{
				std::error_code errorCode;
				llvm::raw_fd_ostream dumpFileStream(llvm::StringRef("llvmDump.ll"),errorCode,llvm::sys::fs::OpenFlags::F_Text);
				jitModule->llvmModule->print(dumpFileStream,nullptr);
				std::cerr << "LLVM verification errors:\n" << verifyOutputStream.str() << std::endl;
				return false;
			}
		#endif

		// Run some optimization on the module's functions.		
		Core::Timer optimizationTimer;
		llvm::legacy::PassManager passManager;
		passManager.add(llvm::createFunctionInliningPass(2,0));
		//passManager.add(llvm::createPartialInliningPass());
		passManager.add(llvm::createGlobalDCEPass());
		passManager.run(*jitModule->llvmModule);

		auto fpm = new llvm::legacy::FunctionPassManager(jitModule->llvmModule);
		fpm->add(llvm::createPromoteMemoryToRegisterPass());
		fpm->add(llvm::createBasicAliasAnalysisPass());
		fpm->add(llvm::createInstructionCombiningPass());
		fpm->add(llvm::createReassociatePass());
		fpm->add(llvm::createGVNPass());
		fpm->add(llvm::createLICMPass());
		fpm->add(llvm::createLoopVectorizePass());
		fpm->add(llvm::createSLPVectorizerPass());
		fpm->add(llvm::createBBVectorizePass());
		fpm->add(llvm::createLoopUnrollPass());
		fpm->add(llvm::createCFGSimplificationPass());
		fpm->add(llvm::createConstantPropagationPass());
		fpm->add(llvm::createDeadInstEliminationPass());
		fpm->add(llvm::createDeadCodeEliminationPass());
		fpm->add(llvm::createDeadStoreEliminationPass());
		fpm->add(llvm::createAggressiveDCEPass());
		fpm->add(llvm::createBitTrackingDCEPass());
		fpm->add(llvm::createInductiveRangeCheckEliminationPass());
		fpm->add(llvm::createIndVarSimplifyPass());
		fpm->add(llvm::createLoopStrengthReducePass());
		fpm->add(llvm::createLoopRotatePass());
		fpm->add(llvm::createLoopIdiomPass());
		fpm->add(llvm::createJumpThreadingPass());
		fpm->add(llvm::createMemCpyOptPass());
		fpm->add(llvm::createConstantHoistingPass());
		fpm->doInitialization();
		for(auto functionIt = jitModule->llvmModule->begin();functionIt != jitModule->llvmModule->end();++functionIt)
		{ fpm->run(*functionIt); }
		delete fpm;
		
		std::error_code errorCode;
		llvm::raw_fd_ostream dumpFileStream(llvm::StringRef("llvmOptimizedDump.ll"),errorCode,llvm::sys::fs::OpenFlags::F_Text);
		jitModule->llvmModule->print(dumpFileStream,nullptr);

		//std::cout << "Optimized LLVM code in " << optimizationTimer.getMilliseconds() << "ms" << std::endl;

		// Generate native machine code.
		Core::Timer machineCodeTimer;
		jitModule->executionEngine->finalizeObject();
		//std::cout << "Generated machine code in " << machineCodeTimer.getMilliseconds() << "ms" << std::endl;

		return true;
	}
}

namespace Runtime
{
	bool compileModule(const Module* astModule)
	{
		return LLVMJIT::compileModule(astModule);
	}

	void* getFunctionPointer(const Module* module,uintptr_t functionIndex)
	{
		for(auto jitModule : LLVMJIT::jitModules)
		{
			if(jitModule->astModule == module)
			{
				return (void*)jitModule->executionEngine->getFunctionAddress(jitModule->functions[functionIndex]->getName());
			}
		}
		return nullptr;
	}
}