#include "Core/Core.h"
#include "Core/MemoryArena.h"
#include "Core/SExpressions.h"
#include "AST/AST.h"
#include "AST/ASTExpressions.h"
#include "AST/ASTDispatch.h"
#include "WebAssemblyTextSymbols.h"
#include "WebAssembly.h"

#include <map>

using namespace AST;

#ifdef _WIN32
	#pragma warning(disable:4702) // unreachable code
#endif

namespace WebAssemblyText
{
	typedef SExp::Node SNode;
	typedef SExp::NodeIt SNodeIt;
	
	// Describes a S-expression node briefly for parsing error messages.
	std::string describeSNode(SNode* node)
	{
		if(!node) { return "null"; }
		else
		{
			switch(node->type)
			{
			case SExp::NodeType::Tree: return "(" + describeSNode(node->children) + ")";
			case SExp::NodeType::Symbol: return wastSymbols[node->symbol];
			case SExp::NodeType::Int: return std::to_string(node->integer);
			case SExp::NodeType::Decimal: return std::to_string(node->decimal);
			case SExp::NodeType::Error: return node->error;
			case SExp::NodeType::String: return node->string;
			case SExp::NodeType::UnindexedSymbol: return node->string;
			default: throw;
			}
		}
	}

	// Creates and records an error with the given message and location (taken from nodeIt).
	template<typename Error> Error* recordError(std::vector<ErrorRecord*>& outErrors,SNodeIt nodeIt,std::string&& message)
	{
		auto locus = nodeIt.node ? nodeIt.node->startLocus : nodeIt.previousLocus;
		auto error = new Error(locus.describe() + ": " + message + " (S-expression node is " + describeSNode(nodeIt) + ")");
		outErrors.push_back(error);
		return error;
	}

	template<typename Error> Error* recordExcessInputError(std::vector<ErrorRecord*>& outErrors,SNodeIt nodeIt,const char* errorContext)
	{
		auto message = std::string("unexpected input following ") + errorContext;
		return recordError<Error>(outErrors,nodeIt,std::move(message));
	}

	// Parse a type from a S-expression symbol.
	bool parseType(SNodeIt& nodeIt,TypeId& outType)
	{
		if(nodeIt
			&& nodeIt->type == SExp::NodeType::Symbol
			&& nodeIt->symbol > (uintptr_t)Symbol::_typeBase
			&& nodeIt->symbol <= ((uintptr_t)Symbol::_typeBase + (uintptr_t)TypeId::max))
		{
			outType = (TypeId)(nodeIt->symbol - (uintptr_t)Symbol::_typeBase);
			++nodeIt;
			return true;
		}
		else { return false; }
	}

	// Parse an integer from a S-expression node.
	bool parseInt(SNodeIt& nodeIt,int64& outInt)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::Int) { outInt = nodeIt->integer; ++nodeIt; return true; }
		else { return false; }
	}

	// Parse a 64-bit float32 from a S-expression node.
	bool parseFloat64(SNodeIt& nodeIt,float64& outDouble)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::Decimal) { outDouble = nodeIt->decimal; ++nodeIt; return true; }
		else if(nodeIt && nodeIt->type == SExp::NodeType::Int) { outDouble = (float64)nodeIt->integer; ++nodeIt; return true; }
		else { return false; }
	}

	// Parse a string from a S-expression node. Upon success, the string is copied into the provided memory arena.
	bool parseString(SNodeIt& nodeIt,const char*& outString,size_t& outStringLength,Memory::Arena& arena)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::String)
		{
			outString = arena.copyToArena(nodeIt->string,nodeIt->stringLength + 1);
			outStringLength = nodeIt->stringLength;
			++nodeIt;
			return true;
		}
		else { return false; }
	}
	
	// Parse a S-expression tree node. Upon success, outChildIt is set to the node's first child.
	bool parseTreeNode(SNodeIt nodeIt,SNodeIt& outChildIt)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::Tree) { outChildIt = nodeIt.getChildIt(); return true; }
		else { return false; }
	}

	// Parse a S-expression symbol node. Upon success, outSymbol is set to the parsed symbol.
	bool parseSymbol(SNodeIt& nodeIt,Symbol& outSymbol)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::Symbol) { outSymbol = (Symbol)nodeIt->symbol; ++nodeIt; return true; }
		else { return false; }
	}

	// Parse a S-expression tree node whose first child is an symbol. Sets outChildren to the first child after the symbol on success.
	bool parseTaggedNode(SNodeIt nodeIt,Symbol tagSymbol,SNodeIt& outChildIt)
	{
		Symbol symbol;
		return parseTreeNode(nodeIt,outChildIt) && parseSymbol(outChildIt,symbol) && symbol == tagSymbol;
	}

	// Tries to parse a name from a SExp node (a string symbol starting with a $).
	// On success, returns true, sets outString to the name's string, and advances node to the sibling following the name.
	bool parseName(SNodeIt& nodeIt,const char*& outString)
	{
		if(nodeIt && nodeIt->type == SExp::NodeType::UnindexedSymbol && nodeIt->string[0] == '$')
		{
			outString = nodeIt->string + 1;
			++nodeIt;
			return true;
		}
		else
		{
			return false;
		}
	}
	
	// Parse a variable from the child nodes of a local, global, or param node. Names are copied into the provided memory arena.
	// Format is (name type) | type+
	size_t parseVariables(SNodeIt& childNodeIt,std::vector<Variable>& outVariables,std::vector<ErrorRecord*>& outErrors,Memory::Arena& arena)
	{
		const char* name;
		if(parseName(childNodeIt,name))
		{
			TypeId type;
			if(!parseType(childNodeIt,type)) { recordError<ErrorRecord>(outErrors,childNodeIt,"expected type"); return 0; }
			auto nameCopy = arena.copyToArena(name,strlen(name)+1);
			outVariables.emplace_back(std::move(Variable({type,nameCopy})));
			return 1;
		}
		else
		{
			size_t numVariables = 0;
			while(childNodeIt)
			{
				TypeId type;
				if(!parseType(childNodeIt,type)) { recordError<ErrorRecord>(outErrors,childNodeIt,"expected type"); return numVariables; }
 				outVariables.emplace_back(std::move(Variable({type,nullptr})));
				++numVariables;
			}
			return numVariables;
		}
	}
	
	// Parse a name or an index.
	// If a name is parsed that is contained in nameToIndex, the index of the name is assigned to outIndex and true is returned.
	// If an index is parsed that is between 0 and numValidIndices, the index is assigned to outIndex and true is returned.
	bool parseNameOrIndex(SNodeIt& nodeIt,const std::map<std::string,uintptr_t>& nameToIndex,size_t numValidIndices,uintptr_t& outIndex)
	{
		const char* name;
		int64 parsedInt;
		if(parseInt(nodeIt,parsedInt) && parsedInt >= 0 && (uintptr_t)parsedInt < numValidIndices) { outIndex = (uintptr_t)parsedInt; return true; }
		else if(parseName(nodeIt,name))
		{
			auto it = nameToIndex.find(name);
			if(it != nameToIndex.end()) { outIndex = it->second; return true; }
			else { return false; }
		}
		else { return false; }
	}

	// Builds a map from name to index from an array of variables.
	void buildVariableNameToIndexMapMap(const std::vector<Variable>& variables,std::map<std::string,uintptr_t>& outNameToIndexMap,std::vector<ErrorRecord*>& outErrors)
	{
		for(uintptr_t variableIndex = 0;variableIndex < variables.size();++variableIndex)
		{
			const auto& variable = variables[variableIndex];
			if(variable.name != nullptr)
			{
				if(outNameToIndexMap.count(variable.name)) { recordError<ErrorRecord>(outErrors,SNodeIt(nullptr),"duplicate variable name"); }
				else { outNameToIndexMap[variable.name] = variableIndex; }
			}
		}
	}
	
	// Context that is shared between functions parsing a module.
	struct ModuleContext
	{
		Module* module;
		std::map<std::string,uintptr_t> functionNameToIndexMap;
		std::map<std::string,uintptr_t> globalNameToIndexMap;
		std::map<std::string,uintptr_t> functionTableNameToIndexMap;
		std::map<std::string,uintptr_t> functionImportNameToIndexMap;
		std::vector<ErrorRecord*>& outErrors;

		ModuleContext(Module* inModule,std::vector<ErrorRecord*>& inOutErrors): module(inModule), outErrors(inOutErrors) {}

		Module* parse(SNodeIt moduleNode);
	};

	// Context that is shared between functions parsing a function.
	struct FunctionContext
	{
		FunctionContext(ModuleContext& inModuleContext,Function* inFunction)
		:	arena(inModuleContext.module->arena)
		,	outErrors(inModuleContext.outErrors)
		,	moduleContext(inModuleContext)
		,	function(inFunction)
		{
			// Build a map from local/parameter names to indices.
			buildVariableNameToIndexMapMap(function->locals,localNameToIndexMap,outErrors);
		}
		
		// Parses an expression of a specific type that's not known at compile time. Returns an UntypedExpression because the type is known to the caller.
		UntypedExpression* parseTypedExpression(TypeId type,SNodeIt& nodeIt,const char* errorContext)
		{
			switch(type)
			{
			#define AST_TYPE(typeName,className,...) case TypeId::typeName: return parseTypedExpression<className##Class>(type,nodeIt,errorContext);
			ENUM_AST_TYPES(AST_TYPE,_)
			#undef AST_TYPE
			default: throw;
			};
		}
		
		// Parses a sequence of expressions, with the final node being of a specific type.
		UntypedExpression* parseExpressionSequence(TypeId type,SNodeIt nodeIt,const char* context)
		{
			switch(type)
			{
			#define AST_TYPE(typeName,className,...) case TypeId::typeName: return parseExpressionSequence<className##Class>(type,nodeIt,context);
			ENUM_AST_TYPES(AST_TYPE,_)
			#undef AST_TYPE
			default: throw;
			}
		}

	private:
		Memory::Arena& arena;
		std::vector<ErrorRecord*>& outErrors;
		ModuleContext& moduleContext;
		Function* function;
		std::map<std::string,uintptr_t> localNameToIndexMap;
		std::map<std::string,BranchTarget*> labelToBranchTargetMap;

		std::vector<BranchTarget*> scopedBranchTargets;
	
		// Parses a non-parametric expression. These are expressions whose result type is defined by the opcode.
		TypedExpression parseNonParametricExpression(SNodeIt parentNodeIt)
		{
			SNodeIt nodeIt;
			Symbol tag;
			if(parseTreeNode(parentNodeIt,nodeIt) && parseSymbol(nodeIt,tag))
			{
				TypeId opType;
				switch(tag)
				{
				default: return TypedExpression();
				#define DEFINE_UNTYPED_OP(symbol) \
					throw; case Symbol::_##symbol: opType = TypeId::None;
				#define DISPATCH_TYPED_OP(opTypeName,opClassName,symbol) \
					throw; case Symbol::_##symbol##_##opTypeName: opType = TypeId::opTypeName; goto symbol##opClassName##Label;
				#define DEFINE_TYPED_OP(opClass,symbol) \
					ENUM_AST_TYPES_##opClass(DISPATCH_TYPED_OP,symbol) \
					throw; symbol##opClass##Label:
				#define DEFINE_TYPED_OP_FOR_TYPE(opTypeName,symbol) \
					throw; case Symbol::_##symbol##_##opTypeName:
				#define DEFINE_BITYPED_OP(leftTypeName,rightTypeName,symbol) \
					throw; case Symbol::_##symbol##_##leftTypeName##_##rightTypeName:

				#define DEFINE_UNARY_OP(class,symbol,opcode) DEFINE_TYPED_OP(class,symbol) { return parseUnaryExpression<class##Class>(opType,class##Op::opcode,nodeIt); }
				#define DEFINE_BINARY_OP(class,symbol,opcode) DEFINE_TYPED_OP(class,symbol) { return parseBinaryExpression<class##Class>(opType,class##Op::opcode,nodeIt); }
				#define DEFINE_COMPARE_OP(class,symbol,opcode) DEFINE_TYPED_OP(class,symbol) { return parseComparisonExpression(opType,BoolOp::opcode,nodeIt); }
				#define DEFINE_CAST_OP(destType,sourceType,symbol,opcode) DEFINE_BITYPED_OP(destType,sourceType,symbol) \
					{ return parseCastExpression<destType##Type::Class>(destType##Type::Op::opcode,TypeId::sourceType,TypeId::destType,nodeIt); }

				DEFINE_UNTYPED_OP(nop)			{ return TypedExpression(Nop::get(),TypeId::Void); }

				DEFINE_TYPED_OP(Int,const)
				{
					int64 integer;
					if(!parseInt(nodeIt,integer)) { return TypedExpression(recordError<Error<IntClass>>(outErrors,nodeIt,"const: expected integer"),opType); }
					switch(opType)
					{
					case TypeId::I8: return TypedExpression(requireFullMatch(nodeIt,"const.i8",new(arena)Literal<I8Type>((uint8)integer)),TypeId::I8);
					case TypeId::I16: return TypedExpression(requireFullMatch(nodeIt,"const.i16",new(arena)Literal<I16Type>((uint16)integer)),TypeId::I16);
					case TypeId::I32: return TypedExpression(requireFullMatch(nodeIt,"const.i32",new(arena)Literal<I32Type>((uint32)integer)),TypeId::I32);
					case TypeId::I64: return TypedExpression(requireFullMatch(nodeIt,"const.i64",new(arena)Literal<I64Type>((uint64)integer)),TypeId::I64);
					default: throw;
					}
				}
				DEFINE_TYPED_OP(Float,const)
				{
					float64 doubleValue;
					if(!parseFloat64(nodeIt,doubleValue)) { return TypedExpression(recordError<Error<FloatClass>>(outErrors,nodeIt,"const: expected decimal"),opType); }
					switch(opType)
					{
					case TypeId::F32: return TypedExpression(requireFullMatch(nodeIt,"const.f32",new(arena)Literal<F32Type>((float32)doubleValue)),TypeId::F32);
					case TypeId::F64: return TypedExpression(requireFullMatch(nodeIt,"const.f64",new(arena)Literal<F64Type>(doubleValue)),TypeId::F64);
					default: throw;
					}
				}
				
				#define DEFINE_LOAD_OP(class,valueType,memoryType,loadSymbol,loadOp) DEFINE_TYPED_OP_FOR_TYPE(valueType,loadSymbol)	\
					{ return parseLoadExpression<class##Class>(TypeId::valueType,TypeId::memoryType,class##Op::loadOp,false,true,nodeIt); }
				#define DEFINE_STORE_OP(class,valueType,memoryType,storeSymbol) DEFINE_TYPED_OP_FOR_TYPE(valueType,storeSymbol) \
					{ return parseStoreExpression<class##Class>(TypeId::valueType,TypeId::memoryType,false,true,nodeIt); }
				#define DEFINE_MEMORY_OP(class,valueType,memoryType,loadSymbol,storeSymbol,loadOp) \
					DEFINE_LOAD_OP(class,valueType,memoryType,loadSymbol,loadOp) \
					DEFINE_STORE_OP(class,valueType,memoryType,storeSymbol)
					
				DEFINE_LOAD_OP(Int,I32,I8,load8_s,loadSExt)
				DEFINE_LOAD_OP(Int,I32,I8,load8_u,loadZExt)
				DEFINE_LOAD_OP(Int,I32,I16,load16_s,loadSExt)
				DEFINE_LOAD_OP(Int,I32,I16,load16_u,loadZExt)
				DEFINE_STORE_OP(Int,I32,I8,store8)
				DEFINE_STORE_OP(Int,I32,I16,store16)
				DEFINE_MEMORY_OP(Int,I32,I32,load,store,load)
				DEFINE_MEMORY_OP(Int,I64,I64,load,store,load)
				DEFINE_MEMORY_OP(Float,F32,F32,load,store,load)
				DEFINE_MEMORY_OP(Float,F64,F64,load,store,load)

				DEFINE_UNARY_OP(Int,neg,neg)
				DEFINE_UNARY_OP(Int,abs,abs)
				DEFINE_UNARY_OP(Int,not,bitwiseNot)
				DEFINE_UNARY_OP(Int,clz,clz)
				DEFINE_UNARY_OP(Int,ctz,ctz)
				DEFINE_UNARY_OP(Int,popcnt,popcnt)
				DEFINE_BINARY_OP(Int,add,add)
				DEFINE_BINARY_OP(Int,sub,sub)
				DEFINE_BINARY_OP(Int,mul,mul)
				DEFINE_BINARY_OP(Int,div_s,divs)
				DEFINE_BINARY_OP(Int,div_u,divu)
				DEFINE_BINARY_OP(Int,rem_s,rems)
				DEFINE_BINARY_OP(Int,rem_u,remu)
				DEFINE_BINARY_OP(Int,and,bitwiseAnd)
				DEFINE_BINARY_OP(Int,or,bitwiseOr)
				DEFINE_BINARY_OP(Int,xor,bitwiseXor)
				DEFINE_BINARY_OP(Int,shl,shl)
				DEFINE_BINARY_OP(Int,shr_s,shrSExt)
				DEFINE_BINARY_OP(Int,shr_u,shrZExt)

				DEFINE_UNARY_OP(Float,neg,neg)
				DEFINE_UNARY_OP(Float,abs,abs)
				DEFINE_UNARY_OP(Float,ceil,ceil)
				DEFINE_UNARY_OP(Float,floor,floor)
				DEFINE_UNARY_OP(Float,trunc,trunc)
				DEFINE_UNARY_OP(Float,nearest,nearestInt)
				DEFINE_BINARY_OP(Float,add,add)
				DEFINE_BINARY_OP(Float,sub,sub)
				DEFINE_BINARY_OP(Float,mul,mul)
				DEFINE_BINARY_OP(Float,div,div)
				DEFINE_BINARY_OP(Float,rem,rem)
				DEFINE_BINARY_OP(Float,copysign,copySign)
				DEFINE_BINARY_OP(Float,min,min)
				DEFINE_BINARY_OP(Float,max,max)
				DEFINE_UNARY_OP(Float,sqrt,sqrt)

				DEFINE_UNARY_OP(Bool,not,bitwiseNot)
				DEFINE_BINARY_OP(Bool,and,bitwiseAnd)
				DEFINE_BINARY_OP(Bool,or,bitwiseOr)

				DEFINE_COMPARE_OP(Int,eq,eq) DEFINE_COMPARE_OP(Int,ne,ne)
				DEFINE_COMPARE_OP(Float,eq,eq) DEFINE_COMPARE_OP(Float,ne,ne)
				DEFINE_COMPARE_OP(Bool,eq,eq) DEFINE_COMPARE_OP(Bool,ne,ne)
				DEFINE_COMPARE_OP(Int,lt_s,lts)
				DEFINE_COMPARE_OP(Int,lt_u,ltu)
				DEFINE_COMPARE_OP(Int,le_s,les)
				DEFINE_COMPARE_OP(Int,le_u,leu)
				DEFINE_COMPARE_OP(Int,gt_s,gts)
				DEFINE_COMPARE_OP(Int,gt_u,gtu)
				DEFINE_COMPARE_OP(Int,ge_s,ges)
				DEFINE_COMPARE_OP(Int,ge_u,geu)
				DEFINE_COMPARE_OP(Float,lt,lt)
				DEFINE_COMPARE_OP(Float,le,le)
				DEFINE_COMPARE_OP(Float,gt,gt)
				DEFINE_COMPARE_OP(Float,ge,ge)
				
				DEFINE_CAST_OP(I8,I16,wrap,wrap)
				DEFINE_CAST_OP(I8,I32,wrap,wrap)
				DEFINE_CAST_OP(I8,I64,wrap,wrap)
				DEFINE_CAST_OP(I16,I32,wrap,wrap)
				DEFINE_CAST_OP(I16,I64,wrap,wrap)
				DEFINE_CAST_OP(I32,I64,wrap,wrap)

				DEFINE_CAST_OP(I64,I8,extend_s,sext)
				DEFINE_CAST_OP(I64,I16,extend_s,sext)
				DEFINE_CAST_OP(I64,I32,extend_s,sext)
				DEFINE_CAST_OP(I32,I8,extend_s,sext)
				DEFINE_CAST_OP(I32,I16,extend_s,sext)
				DEFINE_CAST_OP(I16,I8,extend_s,sext)

				DEFINE_CAST_OP(I64,I8,extend_u,zext)
				DEFINE_CAST_OP(I64,I16,extend_u,zext)
				DEFINE_CAST_OP(I64,I32,extend_u,zext)
				DEFINE_CAST_OP(I32,I8,extend_u,zext)
				DEFINE_CAST_OP(I32,I16,extend_u,zext)
				DEFINE_CAST_OP(I16,I8,extend_u,zext)

				DEFINE_CAST_OP(I32,F64,trunc_s,truncSignedFloat)
				DEFINE_CAST_OP(I32,F64,trunc_u,truncUnsignedFloat)
				DEFINE_CAST_OP(I32,F32,trunc_s,truncSignedFloat)
				DEFINE_CAST_OP(I32,F32,trunc_u,truncUnsignedFloat)
				DEFINE_CAST_OP(I64,F64,trunc_s,truncSignedFloat)
				DEFINE_CAST_OP(I64,F64,trunc_u,truncUnsignedFloat)
				DEFINE_CAST_OP(I64,F32,trunc_s,truncSignedFloat)
				DEFINE_CAST_OP(I64,F32,trunc_u,truncUnsignedFloat)

				DEFINE_CAST_OP(F64,I8,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F64,I16,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F64,I32,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F64,I64,convert_s,convertSignedInt)

				DEFINE_CAST_OP(F32,I8,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F32,I16,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F32,I32,convert_s,convertSignedInt)
				DEFINE_CAST_OP(F32,I64,convert_s,convertSignedInt)

				DEFINE_CAST_OP(F64,I8,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F64,I16,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F64,I32,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F64,I64,convert_u,convertUnsignedInt)

				DEFINE_CAST_OP(F32,I8,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F32,I16,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F32,I32,convert_u,convertUnsignedInt)
				DEFINE_CAST_OP(F32,I64,convert_u,convertUnsignedInt)

				DEFINE_CAST_OP(F32,F64,demote,demote)
				DEFINE_CAST_OP(F64,F32,promote,promote)

				DEFINE_CAST_OP(F64,I64,reinterpret,reinterpretInt)
				DEFINE_CAST_OP(F32,I32,reinterpret,reinterpretInt)
				DEFINE_CAST_OP(I64,F64,reinterpret,reinterpretFloat)
				DEFINE_CAST_OP(I32,F32,reinterpret,reinterpretFloat)

				DEFINE_CAST_OP(I8,Bool,reinterpret,reinterpretBool)
				DEFINE_CAST_OP(I16,Bool,reinterpret,reinterpretBool)
				DEFINE_CAST_OP(I32,Bool,reinterpret,reinterpretBool)
				DEFINE_CAST_OP(I64,Bool,reinterpret,reinterpretBool)

				#undef DEFINE_UNTYPED_OP
				#undef DISPATCH_TYPED_OP
				#undef DEFINE_TYPED_OP
				#undef DISPATCH_BITYPED_OP
				#undef DEFINE_BITYPED_OP
				}
			}
			
			return TypedExpression();
		}

		// Parse a parametric expression of a specific type.
		// These are different from the non-parametric expressions because the opcodes are valid in any type context,
		// so it needs to know the context to construct the correct class of expression.
		template<typename Class> typename Class::ClassExpression* parseParametricExpression(TypeId resultType,SNodeIt parentNodeIt)
		{
			SNodeIt nodeIt;
			Symbol tag;
			if(parseTreeNode(parentNodeIt,nodeIt) && parseSymbol(nodeIt,tag))
			{
				TypeId opType;
				switch(tag)
				{
				default: return nullptr;
				#define DEFINE_PARAMETRIC_UNTYPED_OP(symbol) \
					throw; case Symbol::_##symbol: opType = TypeId::None;
				#define DISPATCH_PARAMETRIC_TYPED_OP(opTypeName,opClassName,symbol) \
					throw; case Symbol::_##symbol##_##opTypeName: opType = TypeId::opTypeName; goto symbol##opClassName##Label;
				#define DEFINE_PARAMETRIC_TYPED_OP(opClass,symbol) \
					ENUM_AST_TYPES_##opClass(DISPATCH_PARAMETRIC_TYPED_OP,symbol) \
					throw; symbol##opClass##Label:

				DEFINE_PARAMETRIC_TYPED_OP(Int,switch)
				{
					// Parse an optional label name for the switch end branch target.
					const char* labelName;
					bool hasEndLabel = parseName(nodeIt,labelName);
					auto endTarget = new(arena)BranchTarget(resultType);
					if(hasEndLabel && labelToBranchTargetMap.count(labelName)) { return recordError<Error<Class>>(outErrors,nodeIt,"switch: break label name shadows outer label"); }
					
					// Parse the switch key.
					auto keyType = opType;
					auto key = parseTypedExpression<IntClass>(keyType,nodeIt,"switch key");

					// Add the switch's label to the in-scope labels.
					if(hasEndLabel) { labelToBranchTargetMap[labelName] = endTarget; }
					
					// Count the number of switch cases.
					size_t numArms = 0;
					for(auto caseCountIt = nodeIt;caseCountIt;++caseCountIt)
					{
						SNodeIt childNodeIt;
						if(parseTaggedNode(caseCountIt,Symbol::_case,childNodeIt)) { ++numArms; }
						else { break; }
					}

					// Parse the switch cases.
					SwitchArm* arms = new(arena)SwitchArm[numArms + 1];
					uintptr_t armIndex = 0;
					for(;nodeIt;++nodeIt)
					{
						SNodeIt childNodeIt;
						if(parseTaggedNode(nodeIt,Symbol::_case,childNodeIt))
						{
							// Parse the key for this case.
							int64 keyValue;
							if(!parseInt(childNodeIt,keyValue)) { return recordError<Error<Class>>(outErrors,childNodeIt,"switch: missing integer case key"); }
							arms[armIndex].key = (uint64)keyValue;

							// Count the number of operations in the case, and whether it ends with a fallthrough symbol.
							// If there are no operations or a fallthrough symbol, it should fallthrough to the next case.
							size_t numOps = 0;
							bool shouldFallthrough = true;
							for(auto fallthroughNodeIt = childNodeIt;fallthroughNodeIt;++fallthroughNodeIt)
							{
								if(fallthroughNodeIt->type == SExp::NodeType::Symbol && fallthroughNodeIt->symbol == (uintptr_t)Symbol::_fallthrough)
								{
									shouldFallthrough = true;
									// The fallthrough symbol should be the last sibling.
									if(fallthroughNodeIt->nextSibling) { return recordError<Error<Class>>(outErrors,fallthroughNodeIt,"switch: expected fallthrough to be final symbol in S-expression"); }
									break;
								}
								else { ++numOps; shouldFallthrough = false; }
							}

							// Parse the case's expression.
							if(shouldFallthrough)
							{
								// Fallthrough cases expect void expression[s].
								arms[armIndex].value = parseExpressionSequence<VoidClass>(TypeId::Void,childNodeIt,"switch case body",numOps);
							}
							else
							{
								// Non-fallthrough cases expect an expression of the switch's result type.
								auto armValue = parseExpressionSequence<Class>(resultType,childNodeIt,"switch case body",numOps);

								// Branch to the switch end.
								// If the switch result type is void, we can't put anything in the branch node's value, so we have to use a sequence node.
								if(resultType != TypeId::Void) { arms[armIndex].value = new(arena)Branch<VoidClass>(endTarget,armValue); }
								else { arms[armIndex].value = new(arena)Sequence<VoidClass>(as<VoidClass>(armValue),new(arena)Branch<VoidClass>(endTarget,nullptr)); }
							}
							++armIndex;
						}
						else { break; }
					}
					assert(armIndex == numArms);

					// Parse the default expression.
					arms[numArms].key = 0;
					arms[numArms].value = parseTypedExpression<Class>(resultType,nodeIt,"switch default value");
					
					// Remove the switch end target from the in-scope branch targets.
					if(hasEndLabel) { labelToBranchTargetMap.erase(labelName); }

					// Create the Switch node.
					auto result = new(arena)Switch<Class>(TypedExpression(key,keyType),numArms,numArms+1,arms,endTarget);
					return requireFullMatch(nodeIt,"switch",result);
				}

				DEFINE_PARAMETRIC_UNTYPED_OP(if)
				{
					// Parse the if condition and then-expression.
					auto condition = parseTypedExpression<BoolClass>(TypeId::Bool,nodeIt,"if condition");
					auto thenExpression = parseTypedExpression<Class>(resultType,nodeIt,"if then");

					// Parse an optional else-expression. If there isn't one, use a nop.
					Expression<Class>* elseExpression;
					if(nodeIt) { elseExpression = parseTypedExpression<Class>(resultType,nodeIt,"if else"); }
					else if(resultType == TypeId::Void) { elseExpression = as<Class>(Nop::get()); }
					else { elseExpression = recordError<Error<Class>>(outErrors,nodeIt,"if without else used as value"); }

					// Construct the IfElse node.
					return requireFullMatch(nodeIt,"if",new(arena)IfElse<Class>(condition,thenExpression,elseExpression));
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(loop)
				{
					auto breakTarget = new(arena) BranchTarget(resultType);
					auto continueTarget = new(arena) BranchTarget(TypeId::Void);
					
					// Parse a label names for the break and continue branch targets.
					const char* breakLabelName;
					const char* continueLabelName;
					bool hasBreakLabel = parseName(nodeIt,breakLabelName);
					bool hasContinueLabel = parseName(nodeIt,continueLabelName);
					if(hasBreakLabel)
					{
						if(labelToBranchTargetMap.count(breakLabelName)) { return recordError<Error<Class>>(outErrors,nodeIt,"loop: break label name shadows outer label"); }
						labelToBranchTargetMap[breakLabelName] = breakTarget;
					}
					if(hasContinueLabel)
					{
						if(labelToBranchTargetMap.count(continueLabelName)) { return recordError<Error<Class>>(outErrors,nodeIt,"loop: continue label name shadows outer label"); }
						labelToBranchTargetMap[continueLabelName] = continueTarget;
					}

					// Parse the loop body.
					auto expression = parseExpressionSequence<VoidClass>(TypeId::Void,nodeIt,"loop body");
					
					if(hasBreakLabel) { labelToBranchTargetMap.erase(breakLabelName); }
					if(hasContinueLabel) { labelToBranchTargetMap.erase(continueLabelName); }

					// Create the Loop node.
					return new(arena)Loop<Class>(expression,breakTarget,continueTarget);
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(break)
				{
					// Parse the name or index of the target label.
					const char* name;
					int64 parsedInt;
					BranchTarget* branchTarget = nullptr;
					if(parseInt(nodeIt,parsedInt) && parsedInt >= 0 && (uintptr_t)parsedInt < scopedBranchTargets.size())
					{
						branchTarget = scopedBranchTargets[scopedBranchTargets.size() - 1 - (uintptr_t)parsedInt];
					}
					else if(parseName(nodeIt,name))
					{
						auto it = labelToBranchTargetMap.find(name);
						if(it != labelToBranchTargetMap.end()) { branchTarget = it->second; }
					}
					else
					{
						// If no name or index, use the top of the target stack.
						branchTarget = scopedBranchTargets.back();
					}
					if(!branchTarget)
					{
						return recordError<Error<Class>>(outErrors,nodeIt,"break: expected label name or index");
					}

					// If the branch target's type isn't void, parse an expression for the branch's value.
					auto value = branchTarget->type == TypeId::Void ? nullptr
						: parseTypedExpression(branchTarget->type,nodeIt,"break value");

					// Create the Branch node.
					return requireFullMatch(nodeIt,"break",new(arena)Branch<Class>(branchTarget,value));
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(return)
				{
					// If the function's return type isn't void, parse an expression for the return value.
					auto returnType = function->type.returnType;
					auto valueExpression = returnType == TypeId::Void ? nullptr
						: parseTypedExpression(returnType,nodeIt,"return value");
					
					// Create the Return node.
					return requireFullMatch(nodeIt,"return",new(arena)Return<Class>(valueExpression));
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(call)
				{
					// Parse the function name or index to call.
					uintptr_t functionIndex;
					if(!parseNameOrIndex(nodeIt,moduleContext.functionNameToIndexMap,moduleContext.module->functions.size(),functionIndex))
					{
						return recordError<Error<Class>>(outErrors,nodeIt,"call: expected function name or index");
					}

					// Parse the call's parameters.
					auto callFunction = moduleContext.module->functions[functionIndex];
					auto parameters = new(arena)UntypedExpression*[callFunction->type.parameters.size()];
					for(uintptr_t parameterIndex = 0;parameterIndex < callFunction->type.parameters.size();++parameterIndex)
					{
						auto parameterType = callFunction->type.parameters[parameterIndex];
						auto parameterValue = parseTypedExpression(parameterType,nodeIt,"call parameter");
						parameters[parameterIndex] = parameterValue;
					}

					// Create the Call node.
					auto call = new(arena)Call(AnyOp::callDirect,getPrimaryTypeClass(callFunction->type.returnType),functionIndex,parameters);

					// Validate the function return type against the result type of this call.
					auto result = coerceExpression(Class(),resultType,TypedExpression(call,callFunction->type.returnType),parentNodeIt,"call return value");
					return requireFullMatch(nodeIt,"call",result);
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(call_import)
				{
					// Parse the import name or index to call.
					uintptr_t importIndex;
					if(!parseNameOrIndex(nodeIt,moduleContext.functionImportNameToIndexMap,moduleContext.module->functionImports.size(),importIndex))
					{
						return recordError<Error<Class>>(outErrors,nodeIt,"call_import: expected function import name or index");
					}

					// Parse the call's parameters.
					auto functionImport = moduleContext.module->functionImports[importIndex];
					auto parameters = new(arena)UntypedExpression*[functionImport.type.parameters.size()];
					for(uintptr_t parameterIndex = 0;parameterIndex < functionImport.type.parameters.size();++parameterIndex)
					{
						auto parameterType = functionImport.type.parameters[parameterIndex];
						auto parameterValue = parseTypedExpression(parameterType,nodeIt,"call_import parameter");
						parameters[parameterIndex] = parameterValue;
					}

					// Create the Call node.
					auto call = new(arena)Call(AnyOp::callImport,getPrimaryTypeClass(functionImport.type.returnType),importIndex,parameters);
					
					// Validate the function return type against the result type of this call.
					auto result = coerceExpression(Class(),resultType,TypedExpression(call,functionImport.type.returnType),parentNodeIt,"call_import return value");
					return requireFullMatch(nodeIt,"call",result);
				}
				DEFINE_PARAMETRIC_UNTYPED_OP(call_indirect)
				{
					// Parse the table name or index.
					uintptr_t tableIndex;
					if(!parseNameOrIndex(nodeIt,moduleContext.functionTableNameToIndexMap,moduleContext.module->functionTables.size(),tableIndex))
					{
						return recordError<Error<Class>>(outErrors,nodeIt,"call_indirect: expected function table index");
					}

					// Parse the function index.
					auto functionIndex = parseTypedExpression<IntClass>(TypeId::I32,nodeIt,"call_indirect function");

					// Parse the call's parameters.
					auto functionTable = moduleContext.module->functionTables[tableIndex];
					auto parameters = arena.allocate<UntypedExpression*>(functionTable.type.parameters.size());
					for(uintptr_t parameterIndex = 0;parameterIndex < functionTable.type.parameters.size();++parameterIndex)
					{
						auto parameterType = functionTable.type.parameters[parameterIndex];
						auto parameterValue = parseTypedExpression(parameterType,nodeIt,"call_indirect parameter");
						parameters[parameterIndex] = parameterValue;
					}

					// Create the CallIndirect node.
					auto call = new(arena)CallIndirect(getPrimaryTypeClass(functionTable.type.returnType),tableIndex,functionIndex,parameters);
					
					// Validate the function return type against the result type of this call.
					auto result = coerceExpression(Class(),resultType,TypedExpression(call,functionTable.type.returnType),parentNodeIt,"call_indirect return value");
					return requireFullMatch(nodeIt,"call_indirect",result);
				}

				DEFINE_PARAMETRIC_UNTYPED_OP(label)
				{
					// Parse an optional name for the label.
					const char* labelName;
					bool hasName = parseName(nodeIt,labelName);
					if(hasName && labelToBranchTargetMap.count(labelName)) { return recordError<Error<Class>>(outErrors,nodeIt,"label: name shadows outer label"); }

					// Create a branch target for the label.
					auto branchTarget = new(arena)BranchTarget(resultType);

					// Add the target to the in-scope branch targets.
					if(hasName) { labelToBranchTargetMap[labelName] = branchTarget; }
					scopedBranchTargets.push_back(branchTarget);

					// Parse the label body.
					auto expression = parseExpressionSequence<Class>(resultType,nodeIt,"label body");

					// Remove the target from the in-scope branch targets.
					scopedBranchTargets.pop_back();
					if(hasName) { labelToBranchTargetMap.erase(labelName); }
					
					// Create the Label node.
					return new(arena)Label<Class>(branchTarget,expression);
				}

				DEFINE_PARAMETRIC_UNTYPED_OP(block)
				{ return parseExpressionSequence<Class>(resultType,nodeIt,"block body"); }
				DEFINE_PARAMETRIC_UNTYPED_OP(get_local)
				{ return parseGetVariable<Class>(resultType,AnyOp::getLocal,localNameToIndexMap,function->locals,nodeIt); }
				DEFINE_PARAMETRIC_UNTYPED_OP(load_global)
				{ return parseGetVariable<Class>(resultType,AnyOp::getGlobal,moduleContext.globalNameToIndexMap,moduleContext.module->globals,nodeIt); }
				DEFINE_PARAMETRIC_UNTYPED_OP(set_local)
				{ return parseSetVariable<Class>(resultType,AnyOp::setLocal,localNameToIndexMap,function->locals,nodeIt); }
				DEFINE_PARAMETRIC_UNTYPED_OP(store_global)
				{ return parseSetVariable<Class>(resultType,AnyOp::setGlobal,moduleContext.globalNameToIndexMap,moduleContext.module->globals,nodeIt); }

				#undef DEFINE_PARAMETRIC_UNTYPED_OP
				#undef DISPATCH_PARAMETRIC_TYPED_OP
				#undef DEFINE_PARAMETRIC_TYPED_OP
				}
			}

			return nullptr;
		}
		
		// Record a type error.
		template<typename Class>
		Error<Class>* typeError(TypeId type,TypedExpression typedExpression,SNodeIt nodeIt,const char* errorContext)
		{
			auto message =
				std::string("type error: expecting a ") + getTypeName(type)
				+ " " + errorContext
				+ " but found " + getTypeName(typedExpression.type);
			return recordError<Error<Class>>(outErrors,nodeIt,std::move(message));
		}

		// By default coerceExpression results in a type error, but is overloaded below for specific classes.
		template<typename Class>
		typename Class::ClassExpression* coerceExpression(Class,TypeId resultType,TypedExpression typedExpression,SNodeIt nodeIt,const char* errorContext)
		{
			if(resultType == typedExpression.type) { return as<Class>(typedExpression.expression); }
			else { return typeError<Class>(resultType,typedExpression,nodeIt,errorContext); }
		}

		// coerceExpression for BoolClass will try to coerce integers.
		BoolClass::ClassExpression* coerceExpression(BoolClass,TypeId resultType,TypedExpression typedExpression,SNodeIt nodeIt,const char* errorContext)
		{
			assert(resultType == TypeId::Bool);
			if(resultType == typedExpression.type) { return as<BoolClass>(typedExpression); }
			else if(isTypeClass(typedExpression.type,TypeClassId::Int))
			{
				// Create a literal zero of the appropriate type.
				IntExpression* zero;
				switch(typedExpression.type)
				{
				case TypeId::I8: zero = new(arena) Literal<I8Type>(0); break;
				case TypeId::I16: zero = new(arena) Literal<I16Type>(0); break;
				case TypeId::I32: zero = new(arena) Literal<I32Type>(0); break;
				case TypeId::I64: zero = new(arena) Literal<I64Type>(0); break;
				default: throw;
				}
				// Coerce the integer to a boolean by testing if the integer != 0.
				return new(arena) Comparison(BoolOp::ne,typedExpression.type,typedExpression.expression,zero);
			}
			else { return typeError<BoolClass>(resultType,typedExpression,nodeIt,errorContext); }
		}

		// coerceExpression for VoidClass will wrap any other typed expression in a DiscardResult node.
		VoidClass::ClassExpression* coerceExpression(VoidClass,TypeId resultType,TypedExpression typedExpression,SNodeIt nodeIt,const char* errorContext)
		{
			if(resultType == typedExpression.type) { return as<VoidClass>(typedExpression); }
			else
			{
				assert(typedExpression.type != TypeId::Void);
				return new(arena) DiscardResult(typedExpression);
			}
		}
		
		// coerceExpression for IntClass will try to coerce bools.
		IntClass::ClassExpression* coerceExpression(IntClass,TypeId resultType,TypedExpression typedExpression,SNodeIt nodeIt,const char* errorContext)
		{
			assert(isTypeClass(resultType,TypeClassId::Int));
			if(resultType == typedExpression.type) { return as<IntClass>(typedExpression); }
			else if(isTypeClass(typedExpression.type,TypeClassId::Bool))
			{
				// Reinterpret the bool as an integer.
				return new(arena) Cast<IntClass>(IntOp::reinterpretBool,typedExpression);
			}
			else { return typeError<IntClass>(resultType,typedExpression,nodeIt,errorContext); }
		}

		// Parses expressions of a specific type.
		template<typename Class>
		typename Class::ClassExpression* parseTypedExpression(TypeId type,SNodeIt& nodeIt,const char* errorContext)
		{
			// If the node is a S-expression error node, translate it to an AST error node.
			if(nodeIt && nodeIt->type == SExp::NodeType::Error)
			{
				auto messageLength = strlen(nodeIt->error) + 1;
				auto messageCopy = new char[messageLength];
				memcpy(messageCopy,nodeIt->error,messageLength + 1);
				return recordError<Error<Class>>(outErrors,nodeIt,messageCopy);
			}
			else
			{
				// Try to parse a non-parametric expression.
				auto nonParametricExpression = parseNonParametricExpression(nodeIt);
				if(nonParametricExpression)
				{
					// If successful, then advance to the next node, and coerce the expression to the expected type.
					auto result = coerceExpression(Class(),type,nonParametricExpression,nodeIt,errorContext);
					++nodeIt;
					return result;
				}
				else 
				{
					// Try to parse a parametric expression.
					auto parametricExpression = parseParametricExpression<Class>(type,nodeIt);
					
					if(parametricExpression) { ++nodeIt; return parametricExpression; }
					else
					{
						// Failed to parse an expression.
						auto error = recordError<Error<Class>>(outErrors,nodeIt,std::move(std::string("expected ")
							+ getTypeName(type)
							+ " expression for "
							+ errorContext
							));
						++nodeIt;
						return error;
					}
				}
			}
		}

		// Used to verify that after a SExpr was parsed into an AST node, that all children of the SExpr were consumed.
		// If node is null, then returns result. Otherwise produces an error that there was unexpected input.
		template<typename Class>
		Expression<Class>* requireFullMatch(SNodeIt nodeIt,const char* errorContext,Expression<Class>* result)
		{
			if(!nodeIt) { return result; }
			else { return recordExcessInputError<Error<Class>>(outErrors,nodeIt,errorContext); }
		}
		
		// Parse an expression from a sequence of S-expression children. A specified number of siblings following nodeIt are used.
		// Multiple children are turned into a block node, with all but the last expression yielding a void type.
		template<typename Class>
		typename Class::ClassExpression* parseExpressionSequence(TypeId type,SNodeIt nodeIt,const char* errorContext,size_t numOps)
		{
			if(numOps == 0) { return as<Class>(Nop::get()); }
			else if(numOps == 1) { return parseTypedExpression<Class>(type,nodeIt,errorContext); }
			else
			{
				// Parse the void expressions.
				VoidExpression* result = nullptr;
				for(uintptr_t expressionIndex = 0;expressionIndex < numOps - 1;++expressionIndex)
				{
					auto expression = parseTypedExpression<VoidClass>(TypeId::Void,nodeIt,errorContext);
					if(result) { result = new(arena) Sequence<VoidClass>(result,expression); }
					else { result = expression; }
				}
				// Parse the result expression.
				return new(arena) Sequence<Class>(
					result,
					parseTypedExpression<Class>(type,nodeIt,errorContext)
					);
			}
		}

		// Parse an expression from a sequence of S-expression children. All siblings following nodeIt are used.
		// Multiple children are turned into a block node, with all but the last expression yielding a void type.
		template<typename Class>
		typename Class::ClassExpression* parseExpressionSequence(TypeId type,SNodeIt nodeIt,const char* context)
		{
			size_t numOps = 0;
			for(auto countNodeIt = nodeIt;countNodeIt;++countNodeIt) {++numOps;}
			if(!numOps)
			{
				return recordError<Error<Class>>(outErrors,nodeIt,"missing expression");
			}
			return parseExpressionSequence<Class>(type,nodeIt,context,numOps);
		}

		// Parse a comparison operation.
		TypedExpression parseComparisonExpression(TypeId opType,BoolOp op,SNodeIt childNodeIt)
		{
			auto leftOperand = parseTypedExpression(opType,childNodeIt,"comparison left operand");
			auto rightOperand = parseTypedExpression(opType,childNodeIt,"comparison right operand");
			auto result = new(arena) Comparison(op,opType,leftOperand,rightOperand);
			return TypedExpression(requireFullMatch(childNodeIt,getOpName(op),result),TypeId::Bool);
		}

		// Parse a binary operation.
		template<typename Class>
		TypedExpression parseBinaryExpression(TypeId opType,typename Class::Op op,SNodeIt operandNodeIt)
		{
			auto left = parseTypedExpression<Class>(opType,operandNodeIt,"binary left operand");
			auto right = parseTypedExpression<Class>(opType,operandNodeIt,"binary right operand");
			auto result = new(arena) Binary<Class>(op,left,right);
			return TypedExpression(requireFullMatch(operandNodeIt,getOpName(op),result),opType);
		}
		
		// Parse an unary operation.
		template<typename Class>
		TypedExpression parseUnaryExpression(TypeId opType,typename Class::Op op,SNodeIt operandNodeIt)
		{
			auto operand = parseTypedExpression<Class>(opType,operandNodeIt,"unary operand");
			auto result = new(arena) Unary<Class>(op,operand);
			return TypedExpression(requireFullMatch(operandNodeIt,getOpName(op),result),opType);
		}

		// Parse a memory load operation.
		template<typename Class>
		TypedExpression parseLoadExpression(TypeId resultType,TypeId memoryType,typename Class::Op loadOp,bool isFarAddress,bool isAligned,SNodeIt nodeIt)
		{
			if(!isTypeClass(memoryType,Class::id))
				{ return TypedExpression(recordError<Error<Class>>(outErrors,nodeIt,"load: memory type must be same type class as result"),resultType); }
			
			auto address = parseTypedExpression<IntClass>(isFarAddress ? TypeId::I64 : TypeId::I32,nodeIt,"load address");

			auto result = new(arena) Load<Class>(loadOp,isFarAddress,isAligned,address,memoryType);
			return TypedExpression(requireFullMatch(nodeIt,"load",result),resultType);
		}

		// Parse a memory store operation.
		template<typename OperandClass>
		TypedExpression parseStoreExpression(TypeId valueType,TypeId memoryType,bool isFarAddress,bool isAligned,SNodeIt nodeIt)
		{
			if(!isTypeClass(memoryType,OperandClass::id))
				{ return TypedExpression(recordError<Error<VoidClass>>(outErrors,nodeIt,"store: memory type must be same type class as result"),TypeId::Void); }
			
			auto address = parseTypedExpression<IntClass>(isFarAddress ? TypeId::I64 : TypeId::I32,nodeIt,"store address");
			auto value = parseTypedExpression<OperandClass>(valueType,nodeIt,"store value");
			auto result = new(arena) Store<OperandClass>(isFarAddress,isAligned,address,TypedExpression(value,valueType),memoryType);
			return TypedExpression(requireFullMatch(nodeIt,"store",result),valueType);
		}
		
		// Parse a cast operation.
		template<typename Class>
		TypedExpression parseCastExpression(typename Class::Op op,TypeId sourceType,TypeId destType,SNodeIt nodeIt)
		{
			auto source = parseTypedExpression(sourceType,nodeIt,"cast source");
			auto result = new(arena) Cast<Class>(op,TypedExpression(source,sourceType));
			return TypedExpression(requireFullMatch(nodeIt,getOpName(op),result),destType);
		}
		
		// Parses a load from a local or global variable.
		template<typename Class>
		typename Class::ClassExpression* parseGetVariable(TypeId resultType,AnyOp op,const std::map<std::string,uintptr_t>& nameToIndexMap,const std::vector<Variable>& variables,SNodeIt nodeIt)
		{
			uintptr_t variableIndex;
			if(!parseNameOrIndex(nodeIt,nameToIndexMap,variables.size(),variableIndex))
			{
				auto message = op == AnyOp::getLocal ? "get_local: expected local name or index" : "load_global: expected global name or index";
				return recordError<Error<Class>>(outErrors,nodeIt,std::move(message));
			}
			auto variableType = variables[variableIndex].type;
			auto load = new(arena) GetVariable(op,getPrimaryTypeClass(variableType),variableIndex);
			auto result = coerceExpression(Class(),resultType,TypedExpression(load,variableType),nodeIt,"variable");
			return requireFullMatch(nodeIt,getOpName(op),result);
		}

		// Parses a store to a local or global variable.
		template<typename Class>
		typename Class::ClassExpression* parseSetVariable(TypeId resultType,AnyOp op,const std::map<std::string,uintptr_t>& nameToIndexMap,const std::vector<Variable>& variables,SNodeIt nodeIt)
		{
			uintptr_t variableIndex;
			if(!parseNameOrIndex(nodeIt,nameToIndexMap,variables.size(),variableIndex))
			{
				auto message = op == AnyOp::setLocal ? "set_local: expected local name or index" : "store_global: expected global name or index";
				return recordError<Error<Class>>(outErrors,nodeIt,message);
			}
			auto variableType = variables[variableIndex].type;
			auto valueExpression = parseTypedExpression(variableType,nodeIt,"store value");
			auto store = new(arena) SetVariable(op,getPrimaryTypeClass(variableType),valueExpression,variableIndex);
			auto result = coerceExpression(Class(),resultType,TypedExpression(store,variableType),nodeIt,"variable");
			return requireFullMatch(nodeIt,getOpName(op),result);
		}
	};

	Module* ModuleContext::parse(SNodeIt firstModuleChildNode)
	{
		// Do a first pass that only parses declarations before parsing definitions.
		bool hasMemoryNode = false;
		for(auto nodeIt = firstModuleChildNode;nodeIt;++nodeIt)
		{
			SNodeIt childNodeIt;
			if(parseTaggedNode(nodeIt,Symbol::_func,childNodeIt))
			{
				auto function = new(module->arena) Function();
				auto functionIndex = module->functions.size();
				module->functions.push_back(function);

				// Parse an optional function name.
				const char* functionName;
				if(parseName(childNodeIt,functionName))
				{
					function->name = module->arena.copyToArena(functionName,strlen(functionName)+1);
					if(functionNameToIndexMap.count(functionName)) { recordError<ErrorRecord>(outErrors,childNodeIt,"duplicate function name"); }
					else { functionNameToIndexMap[functionName] = functionIndex; }
				}

				bool hasResult = false;
				for(;childNodeIt;++childNodeIt)
				{
					SNodeIt innerChildNodeIt;
					if(parseTaggedNode(childNodeIt,Symbol::_result,innerChildNodeIt))
					{
						// Parse a result declaration.
						if(hasResult) { recordError<ErrorRecord>(outErrors,childNodeIt,"duplicate result declaration"); continue; }
						if(!parseType(innerChildNodeIt,function->type.returnType)) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"expected type"); continue; }
						hasResult = true;
						if(innerChildNodeIt) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"unexpected input following result declaration"); continue; }
					}
					else if(parseTaggedNode(childNodeIt,Symbol::_param,innerChildNodeIt))
					{
						// Parse a parameter declaration.
						const uintptr_t baseLocalIndex = function->locals.size();
						const size_t numParameters = parseVariables(innerChildNodeIt,function->locals,outErrors,module->arena);
						for(uintptr_t parameterIndex = 0;parameterIndex < numParameters;++parameterIndex)
						{
							function->parameterLocalIndices.push_back(baseLocalIndex + parameterIndex);
							function->type.parameters.push_back(function->locals[baseLocalIndex + parameterIndex].type);
						}
						if(innerChildNodeIt) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"unexpected input following parameter declaration"); continue; }
					}
					else if(parseTaggedNode(childNodeIt,Symbol::_local,innerChildNodeIt))
					{
						// Parse a local declaration.
						parseVariables(innerChildNodeIt,function->locals,outErrors,module->arena);
						if(innerChildNodeIt) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"unexpected input following local declaration"); continue; }
					}
					else { break; } // Stop parsing when we reach the first func child that isn't a param, result, or local.
				}
			}
			else if(parseTaggedNode(nodeIt,Symbol::_import,childNodeIt))
			{
				auto importIndex = module->functionImports.size();

				// Parse an optional import name used within the module.
				const char* importInternalName;
				if(parseName(childNodeIt,importInternalName))
				{
					importInternalName = module->arena.copyToArena(importInternalName,strlen(importInternalName) + 1);
					if(functionImportNameToIndexMap.count(importInternalName)) { recordError<ErrorRecord>(outErrors,SNodeIt(nullptr),"duplicate variable name"); }
					else { functionImportNameToIndexMap[importInternalName] = importIndex; }
				}

				// Parse a mandatory import string.
				const char* importExternalName;
				size_t importExternalNameLength;
				if(!parseString(childNodeIt,importExternalName,importExternalNameLength,module->arena))
				{ recordError<ErrorRecord>(outErrors,childNodeIt,"expected import name string"); continue; }
				
				// If there's a non-function type following the import string, it's a variable import.
				TypeId variableType;
				if(parseType(childNodeIt,variableType))
				{
					// Create the import.
					auto globalIndex = module->globals.size();
					module->globals.push_back({variableType,importInternalName});
					module->variableImports.push_back({variableType,importExternalName,globalIndex});
				}
				else
				{
					// Parse the import's parameter and result declarations.
					std::vector<Variable> parameters;
					TypeId returnType = TypeId::Void;
					bool hasResult = false;
					for(;childNodeIt;++childNodeIt)
					{
						SNodeIt innerChildNodeIt;
						if(parseTaggedNode(childNodeIt,Symbol::_result,innerChildNodeIt))
						{
							// Parse a result declaration.
							if(hasResult) { recordError<ErrorRecord>(outErrors,childNodeIt,"duplicate result declaration"); continue; }
							if(!parseType(innerChildNodeIt,returnType)) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"expected type"); continue; }
							hasResult = true;
							if(innerChildNodeIt) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"unexpected input following result declaration"); continue; }
						}
						else if(parseTaggedNode(childNodeIt,Symbol::_param,innerChildNodeIt))
						{
							// Parse a parameter declaration.
							parseVariables(innerChildNodeIt,parameters,outErrors,module->arena);
							if(innerChildNodeIt) { recordError<ErrorRecord>(outErrors,innerChildNodeIt,"unexpected input following parameter declaration"); continue; }
						}
						else
						{
							recordError<ErrorRecord>(outErrors,innerChildNodeIt,"expected param or result declaration");
						}
					}
				
					// Create the import.
					std::vector<TypeId> parameterTypes;
					for(auto parameter : parameters) { parameterTypes.push_back(parameter.type); }
					module->functionImports.push_back({FunctionType(returnType,parameterTypes),importExternalName});
				}

				if(childNodeIt) { recordError<ErrorRecord>(outErrors,childNodeIt,"unexpected input following import declaration"); continue; }
			}
			else if(parseTaggedNode(nodeIt,Symbol::_global,childNodeIt))
			{
				// Parse a global declaration.
				parseVariables(childNodeIt,module->globals,outErrors,module->arena);
				if(childNodeIt) { recordError<ErrorRecord>(outErrors,childNodeIt,"unexpected input following global declaration"); continue; }
			}
			else if(parseTaggedNode(nodeIt,Symbol::_memory,childNodeIt))
			{
				// Parse a memory declaration.
				if(hasMemoryNode) { recordError<ErrorRecord>(outErrors,nodeIt,"duplicate memory declaration"); continue; }
				hasMemoryNode = true;

				// Parse the initial and maximum number of bytes.
				// If one number is found, it is taken to be both the initial and max.
				int64 initialNumBytes;
				int64 maxNumBytes;
				if(!parseInt(childNodeIt,initialNumBytes))
					{ recordError<ErrorRecord>(outErrors,childNodeIt,"expected initial memory size integer"); continue; }
				if(!parseInt(childNodeIt,maxNumBytes))
					{ maxNumBytes = initialNumBytes; }
				if(module->maxNumBytesMemory > (1ull<<32))
					{ recordError<ErrorRecord>(outErrors,childNodeIt,"maximum memory size must be <=2^32 bytes"); continue; }
				if(module->initialNumBytesMemory > module->maxNumBytesMemory)
					{ recordError<ErrorRecord>(outErrors,childNodeIt,"initial memory size must be <= maximum memory size"); continue; }
				module->initialNumBytesMemory = (uint64) initialNumBytes;
				module->maxNumBytesMemory = (uint64) maxNumBytes;
				
				// Parse the memory segments.
				for(;childNodeIt;++childNodeIt)
				{
					SNodeIt segmentChildNodeIt;
					int64 baseAddress;
					const char* dataString;
					size_t dataLength;
					if(!parseTaggedNode(childNodeIt,Symbol::_segment,segmentChildNodeIt))
						{ recordError<ErrorRecord>(outErrors,segmentChildNodeIt,"expected segment declaration"); continue; }
					if(!parseInt(segmentChildNodeIt,baseAddress))
						{ recordError<ErrorRecord>(outErrors,segmentChildNodeIt,"expected segment base address integer"); continue; }
					if(!parseString(segmentChildNodeIt,dataString,dataLength,module->arena))
						{ recordError<ErrorRecord>(outErrors,segmentChildNodeIt,"expected segment data string"); continue; }
					if(	(uint64)baseAddress + dataLength < (uint64)baseAddress // Check for integer overflow in baseAddress+dataLength.
					||	(uint64)baseAddress + dataLength > module->initialNumBytesMemory)
						{ recordError<ErrorRecord>(outErrors,segmentChildNodeIt,"data segment bounds aren't contained by initial memory size"); continue; }
					module->dataSegments.push_back({(uint64)baseAddress,dataLength,(uint8*)dataString});
				}

				if(childNodeIt) { recordError<ErrorRecord>(outErrors,childNodeIt,"unexpected input following memory declaration"); continue; }
			}
			else if(!parseTaggedNode(nodeIt,Symbol::_export,childNodeIt) && !parseTaggedNode(nodeIt,Symbol::_table,childNodeIt))
				{ recordError<ErrorRecord>(outErrors,nodeIt,"unrecognized declaration"); continue; }
		}

		for(auto nodeIt = firstModuleChildNode;nodeIt;++nodeIt)
		{
			SNodeIt childNodeIt;
			if(parseTaggedNode(nodeIt,Symbol::_table,childNodeIt))
			{
				// Count the number of functions in the table.
				size_t numFunctions = 0;
				for(auto countNodeIt = childNodeIt;countNodeIt;++countNodeIt)
				{ ++numFunctions; }

				FunctionType functionType;
				auto functionIndices = new(module->arena) uintptr_t[numFunctions];
				if(!numFunctions) { recordError<ErrorRecord>(outErrors,nodeIt,"function table must contain atleast 1 function"); }
				else
				{
					// Parse the function indices or names.
					for(uintptr_t index = 0;index < numFunctions;++index)
					{
						uintptr_t functionIndex;
						if(!parseNameOrIndex(childNodeIt,functionNameToIndexMap,module->functions.size(),functionIndex))
							{ functionIndices[index] = 0; recordError<ErrorRecord>(outErrors,childNodeIt,"expected function name or index"); }
						else if((uintptr_t)functionIndex >= module->functions.size())
							{ functionIndices[index] = 0; recordError<ErrorRecord>(outErrors,childNodeIt,"invalid function index"); }
						else { functionIndices[index] = (uintptr_t)functionIndex; }
					}

					// Check that all the functions have the same type.
					functionType = module->functions[functionIndices[0]]->type;
					for(uintptr_t index = 0;index < numFunctions;++index)
					{
						if(module->functions[functionIndices[index]]->type != functionType)
							{ recordError<ErrorRecord>(outErrors,nodeIt,"function table must only contain functions of a single type"); }
					}
				}
				module->functionTables.push_back({functionType,functionIndices,numFunctions});
			}
		}

		// Build a global name to index map.
		buildVariableNameToIndexMapMap(module->globals,globalNameToIndexMap,outErrors);

		// Do a second pass that parses definitions as well.
		intptr_t currentFunctionIndex = 0;
		for(auto nodeIt = firstModuleChildNode;nodeIt;++nodeIt)
		{
			SNodeIt childNodeIt;
			if(parseTaggedNode(nodeIt,Symbol::_func,childNodeIt))
			{
				// Process nodes until the first node that isn't the function name, a param, local, or result.
				const char* functionName;
				parseName(childNodeIt,functionName);

				for(;childNodeIt;++childNodeIt)
				{
					SNodeIt childChildNodeIt;
					if(	!parseTaggedNode(childNodeIt,Symbol::_local,childChildNodeIt)
					&&	!parseTaggedNode(childNodeIt,Symbol::_param,childChildNodeIt)
					&&	!parseTaggedNode(childNodeIt,Symbol::_result,childChildNodeIt))
					{ break; }
				};

				// Parse the function's body.
				auto function = module->functions[currentFunctionIndex++];
				FunctionContext functionContext(*this,function);
				function->expression = functionContext.parseExpressionSequence(function->type.returnType,childNodeIt,"function body");
			}
			else if(parseTaggedNode(nodeIt,Symbol::_export,childNodeIt))
			{
				// Parse an export definition.
				const char* exportName;
				size_t nameLength;
				if(!parseString(childNodeIt,exportName,nameLength,module->arena))
					{ recordError<ErrorRecord>(outErrors,childNodeIt,"expected export name string"); continue; }
				uintptr_t functionIndex;
				if(!parseNameOrIndex(childNodeIt,functionNameToIndexMap,module->functions.size(),functionIndex))
					{ recordError<ErrorRecord>(outErrors,childNodeIt,"expected function name or index"); continue; }
				module->exportNameToFunctionIndexMap[exportName] = functionIndex;
				if(childNodeIt) { recordError<ErrorRecord>(outErrors,childNodeIt,"unexpected input following export declaration"); continue; }
			}
		}
		
		return module;
	}

	// Parses a module from a WAST string.
	bool parse(const char* string,File& outFile)
	{
		const SExp::SymbolIndexMap& symbolIndexMap = getWASTSymbolIndexMap();
		
		// Parse S-expressions from the string.
		Memory::ScopedArena scopedArena;
		auto rootNode = SExp::parse(string,scopedArena,symbolIndexMap);
		
		// Parse modules from S-expressions.
		for(auto rootNodeIt = SNodeIt(rootNode);rootNodeIt;++rootNodeIt)
		{
			SNodeIt childNodeIt;
			if(parseTaggedNode(rootNodeIt,Symbol::_module,childNodeIt))
			{
				// Parse a module definition.
				outFile.modules.push_back(ModuleContext(new Module(),outFile.errors).parse(childNodeIt));
			}
		}
		
		// Parse the assertions.
		for(auto rootNodeIt = SNodeIt(rootNode);rootNodeIt;++rootNodeIt)
		{
			SNodeIt childNodeIt;
			if(parseTaggedNode(rootNodeIt,Symbol::_assert_eq,childNodeIt))
			{
				SNodeIt invokeChildIt;
				if(!parseTaggedNode(childNodeIt++,Symbol::_invoke,invokeChildIt))
					{ recordError<ErrorRecord>(outFile.errors,childNodeIt,"expected invoke expression"); continue; }

				// Parse the export name to invoke.
				const char* invokeExportName;
				size_t invokeExportNameLength;
				SNodeIt savedExportNameIt = invokeChildIt;
				if(!parseString(invokeChildIt,invokeExportName,invokeExportNameLength,scopedArena))
					{ recordError<ErrorRecord>(outFile.errors,invokeChildIt,"expected export name string"); continue; }

				// Find the named export in one of the modules.
				Module* exportModule = nullptr;
				uintptr_t exportFunctionIndex = 0;
				for(auto module : outFile.modules)
				{
					auto exportIt = module->exportNameToFunctionIndexMap.find(invokeExportName);
					if(exportIt != module->exportNameToFunctionIndexMap.end())
					{
						exportModule = module;
						exportFunctionIndex = exportIt->second;
						break;
					}
				}
				if(!exportModule) { recordError<ErrorRecord>(outFile.errors,savedExportNameIt,"couldn't find export with this name"); continue; }

				// Set up a dummy module, function, and parsing context to parse the invoke parameters in.
				Module* dummyModule = new AST::Module;
				Function dummyFunction;
				ModuleContext dummyModuleContext(dummyModule,outFile.errors);
				FunctionContext dummyFunctionContext(dummyModuleContext,&dummyFunction);

				// Parse the invoke's parameters.
				auto function = exportModule->functions[exportFunctionIndex];
				std::vector<TypedExpression> parameters(function->type.parameters.size());
				for(uintptr_t parameterIndex = 0;parameterIndex < function->type.parameters.size();++parameterIndex)
				{
					auto parameterType = function->type.parameters[parameterIndex];
					auto parameterValue = dummyFunctionContext.parseTypedExpression(parameterType,invokeChildIt,"invoke parameter");
					parameters[parameterIndex] = TypedExpression(parameterValue,parameterType);
				}
				
				// Verify that all of the invoke's parameters were matched.
				if(invokeChildIt) { recordExcessInputError<ErrorRecord>(outFile.errors,invokeChildIt,"invoke parameters"); continue; }

				// Parse the expected value of the invoke.
				auto returnType = function->type.returnType;
				auto value = TypedExpression(dummyFunctionContext.parseTypedExpression(returnType,childNodeIt,"assert_eq reference value"),returnType);
				
				// Verify that all of the invoke's parameters were matched.
				if(childNodeIt) { recordExcessInputError<ErrorRecord>(outFile.errors,childNodeIt,"assert_eq expected value"); continue; }

				outFile.assertEqs.push_back({dummyModule,exportModule,exportFunctionIndex,std::move(parameters),value,rootNodeIt->startLocus});
			}
		}

		return !outFile.errors.size();
	}
}