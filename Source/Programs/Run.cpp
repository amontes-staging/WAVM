#include "Core/Core.h"
#include "Core/MemoryArena.h"
#include "CLI.h"
#include "Core/Platform.h"
#include "AST/AST.h"
#include "Runtime/Runtime.h"

struct Void {};

template<typename NativeValue> struct NativeToASTType;
template<> struct NativeToASTType<uint8> { typedef AST::I8Type ASTType; };
template<> struct NativeToASTType<int8> { typedef AST::I8Type ASTType; };
template<> struct NativeToASTType<uint16> { typedef AST::I16Type ASTType; };
template<> struct NativeToASTType<int16> { typedef AST::I16Type ASTType; };
template<> struct NativeToASTType<uint32> { typedef AST::I32Type ASTType; };
template<> struct NativeToASTType<int32> { typedef AST::I32Type ASTType; };
template<> struct NativeToASTType<uint64> { typedef AST::I64Type ASTType; };
template<> struct NativeToASTType<int64> { typedef AST::I64Type ASTType; };
template<> struct NativeToASTType<float32> { typedef AST::F32Type ASTType; };
template<> struct NativeToASTType<float64> { typedef AST::F64Type ASTType; };
template<> struct NativeToASTType<bool> { typedef AST::BoolType ASTType; };
template<> struct NativeToASTType<Void> { typedef AST::VoidType ASTType; };

template<typename... Args>
bool validateArgTypes(const AST::FunctionType& functionType,uintptr_t argIndex,Args...)
{
	return argIndex == functionType.parameters.size();
}

template<typename Arg0,typename... Args>
bool validateArgTypes(const AST::FunctionType& functionType,uintptr_t argIndex,Arg0,Args...)
{
	return validateArgTypes<Args...>(functionType,argIndex + 1)
		&& functionType.parameters[argIndex] == NativeToASTType<Arg0>::ASTType::id;
}

template<typename Return,typename... Args>
bool callModuleFunction(const AST::Module* module,const char* functionName,Return& outReturn,Args... args)
{
	// Look up the function specified on the command line in the module.
	auto exportIt = module->exportNameToFunctionIndexMap.find(functionName);
	if(exportIt == module->exportNameToFunctionIndexMap.end())
	{
		std::cerr << "module doesn't contain named export " << functionName << std::endl;
		return false;
	}
	
	auto function = module->functions[exportIt->second];
	if(!validateArgTypes(function->type,0,args...) || function->type.returnType != NativeToASTType<Return>::ASTType::id)
	{
		std::cerr << "exported function " << functionName << " isn't expected type" << std::endl;
		return false;
	}

	void* functionPtr = Runtime::getFunctionPointer(module,exportIt->second);
	assert(functionPtr);

	// Call the generated machine code for the function.
	try
	{
		// Call the function specified on the command-line.
		outReturn = ((Return(*)(Args...))functionPtr)(args...);
		return true;
	}
	catch(...)
	{
		std::cout << functionName << " threw exception." << std::endl;
		return false;
	}
}

bool initModuleRuntime(const AST::Module* module)
{
	std::cout << "Loaded module uses " << (module->arena.getTotalAllocatedBytes() / 1024) << "KB" << std::endl;

	// Allocate address-space for the VM.
	if(!Runtime::initInstanceMemory(module->maxNumBytesMemory))
	{
		std::cerr << "Couldn't initialize address-space for module instance (" << module->maxNumBytesMemory/1024 << "KB requested)" << std::endl;
		return false;
	}

	// Initialize the module's requested initial memory.
	if(Runtime::vmSbrk((int32)module->initialNumBytesMemory) != 0)
	{
		std::cerr << "Failed to commit the requested initial memory for module instance (" << module->initialNumBytesMemory/1024 << "KB requested)" << std::endl;
		return false;
	}

	// Copy the module's data segments into VM memory.
	if(module->initialNumBytesMemory >= (1ull<<32)) { throw; }
	for(auto dataSegment : module->dataSegments)
	{
		if(dataSegment.baseAddress + dataSegment.numBytes > module->initialNumBytesMemory)
		{
			std::cerr << "Module data segment exceeds initial memory allocation" << std::endl;
			return false;
		}
		memcpy(Runtime::instanceMemoryBase + dataSegment.baseAddress,dataSegment.data,dataSegment.numBytes);
	}

	// Generate machine code for the module.
	if(!Runtime::compileModule(module))
	{
		std::cerr << "Couldn't compile module." << std::endl;
		return false;
	}

	// Initialize the Emscripten intrinsics.
	Runtime::initEmscriptenIntrinsics();
	
	Void result;
	callModuleFunction(module,"__GLOBAL__sub_I_iostream_cpp",result);
	return true;
}

int main(int argc,char** argv)
{
	AST::Module* module = nullptr;
	const char* functionName;
	if(argc == 4 && !strcmp(argv[1],"-text"))
	{
		WebAssemblyText::File wastFile;
		if(loadTextModule(argv[2],wastFile)) { module = wastFile.modules[0]; }
		else { return -1; }
		functionName = argv[3];
	}
	else if(argc == 5 && !strcmp(argv[1],"-binary"))
	{
		module = loadBinaryModule(argv[2],argv[3]);
		functionName = argv[4];
	}
	else
	{
		std::cerr <<  "Usage: Run -binary in.wasm in.js.mem functionname" << std::endl;
		std::cerr <<  "       Run -text in.wast functionname" << std::endl;
		return -1;
	}
	
	if(!module) { return -1; }

	if(!initModuleRuntime(module)) { return -1; }

	uint32 returnCode;
	Core::Timer executionTime;
	if(!callModuleFunction(module,functionName,returnCode)) { return -1; }
	executionTime.stop();

	std::cout << "Program returned: " << returnCode << std::endl;
	std::cout << "Execution time: " << executionTime.getMilliseconds() << "ms" << std::endl;

	return 0;
}