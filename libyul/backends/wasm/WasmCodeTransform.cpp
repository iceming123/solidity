/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
* Common code generator for translating Yul / inline assembly to Wasm.
*/

#include <libyul/backends/wasm/WasmCodeTransform.h>

#include <libyul/optimiser/NameCollector.h>

#include <libyul/AsmData.h>
#include <libyul/Dialect.h>
#include <libyul/Utilities.h>
#include <libyul/Exceptions.h>

#include <liblangutil/Exceptions.h>

#include <optional>

using namespace std;
using namespace solidity;
using namespace solidity::yul;
using namespace solidity::util;

wasm::Module WasmCodeTransform::run(Dialect const& _dialect, yul::Block const& _ast)
{
	wasm::Module module;

	WasmCodeTransform transform(_dialect, _ast);

	for (auto const& statement: _ast.statements)
	{
		yulAssert(
			holds_alternative<yul::FunctionDefinition>(statement),
			"Expected only function definitions at the highest level."
		);
		if (holds_alternative<yul::FunctionDefinition>(statement))
			module.functions.emplace_back(transform.translateFunction(std::get<yul::FunctionDefinition>(statement)));
	}

	for (auto& imp: transform.m_functionsToImport)
		module.imports.emplace_back(std::move(imp.second));
	module.globals = transform.m_globalVariables;

	return module;
}

wasm::Expression WasmCodeTransform::generateMultiAssignment(
	vector<string> _variableNames,
	unique_ptr<wasm::Expression> _firstValue
)
{
	yulAssert(!_variableNames.empty(), "");
	wasm::LocalAssignment assignment{move(_variableNames.front()), std::move(_firstValue)};

	if (_variableNames.size() == 1)
		return { std::move(assignment) };

	allocateGlobals(_variableNames.size() - 1);

	wasm::Block block;
	block.statements.emplace_back(move(assignment));
	for (size_t i = 1; i < _variableNames.size(); ++i)
		block.statements.emplace_back(wasm::LocalAssignment{
			move(_variableNames.at(i)),
			make_unique<wasm::Expression>(wasm::GlobalVariable{m_globalVariables.at(i - 1).variableName})
		});
	return { std::move(block) };
}

wasm::Expression WasmCodeTransform::operator()(VariableDeclaration const& _varDecl)
{
	vector<string> variableNames;
	for (auto const& var: _varDecl.variables)
	{
		variableNames.emplace_back(var.name.str());
		m_localVariables.emplace_back(wasm::VariableDeclaration{variableNames.back()});
	}

	if (_varDecl.value)
		return generateMultiAssignment(move(variableNames), visit(*_varDecl.value));
	else
		return wasm::BuiltinCall{"nop", {}};
}

wasm::Expression WasmCodeTransform::operator()(Assignment const& _assignment)
{
	vector<string> variableNames;
	for (auto const& var: _assignment.variableNames)
		variableNames.emplace_back(var.name.str());
	return generateMultiAssignment(move(variableNames), visit(*_assignment.value));
}

wasm::Expression WasmCodeTransform::operator()(ExpressionStatement const& _statement)
{
	return visitReturnByValue(_statement.expression);
}

wasm::Expression WasmCodeTransform::operator()(FunctionCall const& _call)
{
	bool typeConversionNeeded = false;

	if (BuiltinFunction const* builtin = m_dialect.builtin(_call.functionName.name))
	{
		if (_call.functionName.name.str().substr(0, 4) == "eth.")
		{
			yulAssert(builtin->returns.size() <= 1, "");
			// Imported function, use regular call, but mark for import.
			if (!m_functionsToImport.count(builtin->name))
			{
				wasm::FunctionImport imp{
					"ethereum",
					builtin->name.str().substr(4),
					builtin->name.str(),
					{},
					builtin->returns.empty() ? nullopt : make_optional<string>(builtin->returns.front().str())
				};
				for (auto const& param: builtin->parameters)
					imp.paramTypes.emplace_back(param.str());
				m_functionsToImport[builtin->name] = std::move(imp);
			}
			typeConversionNeeded = true;
		}
		else if (builtin->literalArguments && contains(builtin->literalArguments.value(), true))
		{
			vector<wasm::Expression> literals;
			for (size_t i = 0; i < _call.arguments.size(); i++)
				if (builtin->literalArguments.value()[i])
					literals.emplace_back(wasm::StringLiteral{std::get<Literal>(_call.arguments[i]).value.str()});
				else
					literals.emplace_back(visitReturnByValue(_call.arguments[i]));

			return wasm::BuiltinCall{_call.functionName.name.str(), std::move(literals)};
		}
		else
		{
			wasm::BuiltinCall call{
				_call.functionName.name.str(),
				injectTypeConversionIfNeeded(visit(_call.arguments), builtin->parameters)
			};
			if (!builtin->returns.empty() && !builtin->returns.front().empty() && builtin->returns.front() != "i64"_yulstring)
			{
				yulAssert(builtin->returns.front() == "i32"_yulstring, "Invalid type " + builtin->returns.front().str());
				call = wasm::BuiltinCall{"i64.extend_i32_u", make_vector<wasm::Expression>(std::move(call))};
			}
			return {std::move(call)};
		}
	}

	// If this function returns multiple values, then the first one will
	// be returned in the expression itself and the others in global variables.
	// The values have to be used right away in an assignment or variable declaration,
	// so it is handled there.

	wasm::FunctionCall funCall{_call.functionName.name.str(), visit(_call.arguments)};
	if (typeConversionNeeded)
		// Inject type conversion if needed on the fly. This is just a temporary measure
		// and can be removed once we have proper types in Yul.
		return injectTypeConversionIfNeeded(std::move(funCall));
	else
		return {std::move(funCall)};
}

wasm::Expression WasmCodeTransform::operator()(Identifier const& _identifier)
{
	return wasm::LocalVariable{_identifier.name.str()};
}

wasm::Expression WasmCodeTransform::operator()(Literal const& _literal)
{
	u256 value = valueOfLiteral(_literal);
	yulAssert(value <= numeric_limits<uint64_t>::max(), "Literal too large: " + value.str());
	return wasm::Literal{uint64_t(value)};
}

wasm::Expression WasmCodeTransform::operator()(If const& _if)
{
	// TODO converting i64 to i32 might not always be needed.

	vector<wasm::Expression> args;
	args.emplace_back(visitReturnByValue(*_if.condition));
	args.emplace_back(wasm::Literal{0});
	return wasm::If{
		make_unique<wasm::Expression>(wasm::BuiltinCall{"i64.ne", std::move(args)}),
		visit(_if.body.statements),
		{}
	};
}

wasm::Expression WasmCodeTransform::operator()(Switch const& _switch)
{
	wasm::Block block;
	string condition = m_nameDispenser.newName("condition"_yulstring).str();
	m_localVariables.emplace_back(wasm::VariableDeclaration{condition});
	block.statements.emplace_back(wasm::LocalAssignment{condition, visit(*_switch.expression)});

	vector<wasm::Expression>* currentBlock = &block.statements;
	for (size_t i = 0; i < _switch.cases.size(); ++i)
	{
		Case const& c = _switch.cases.at(i);
		if (c.value)
		{
			wasm::BuiltinCall comparison{"i64.eq", make_vector<wasm::Expression>(
				wasm::LocalVariable{condition},
				visitReturnByValue(*c.value)
			)};
			wasm::If ifStmnt{
				make_unique<wasm::Expression>(move(comparison)),
				visit(c.body.statements),
				{}
			};
			vector<wasm::Expression>* nextBlock = nullptr;
			if (i != _switch.cases.size() - 1)
			{
				ifStmnt.elseStatements = make_unique<vector<wasm::Expression>>();
				nextBlock = ifStmnt.elseStatements.get();
			}
			currentBlock->emplace_back(move(ifStmnt));
			currentBlock = nextBlock;
		}
		else
		{
			yulAssert(i == _switch.cases.size() - 1, "Default case must be last.");
			*currentBlock += visit(c.body.statements);
		}
	}
	return { std::move(block) };
}

wasm::Expression WasmCodeTransform::operator()(FunctionDefinition const&)
{
	yulAssert(false, "Should not have visited here.");
	return {};
}

wasm::Expression WasmCodeTransform::operator()(ForLoop const& _for)
{
	string breakLabel = newLabel();
	string continueLabel = newLabel();
	m_breakContinueLabelNames.push({breakLabel, continueLabel});

	wasm::Loop loop;
	loop.labelName = newLabel();
	loop.statements = visit(_for.pre.statements);
	loop.statements.emplace_back(wasm::BranchIf{wasm::Label{breakLabel}, make_unique<wasm::Expression>(
		wasm::BuiltinCall{"i64.eqz", make_vector<wasm::Expression>(
			visitReturnByValue(*_for.condition)
		)}
	)});
	loop.statements.emplace_back(wasm::Block{continueLabel, visit(_for.body.statements)});
	loop.statements += visit(_for.post.statements);
	loop.statements.emplace_back(wasm::Branch{wasm::Label{loop.labelName}});

	return { wasm::Block{breakLabel, make_vector<wasm::Expression>(move(loop))} };
}

wasm::Expression WasmCodeTransform::operator()(Break const&)
{
	return wasm::Branch{wasm::Label{m_breakContinueLabelNames.top().first}};
}

wasm::Expression WasmCodeTransform::operator()(Continue const&)
{
	return wasm::Branch{wasm::Label{m_breakContinueLabelNames.top().second}};
}

wasm::Expression WasmCodeTransform::operator()(Leave const&)
{
	yulAssert(!m_functionBodyLabel.empty(), "");
	return wasm::Branch{wasm::Label{m_functionBodyLabel}};
}

wasm::Expression WasmCodeTransform::operator()(Block const& _block)
{
	return wasm::Block{{}, visit(_block.statements)};
}

unique_ptr<wasm::Expression> WasmCodeTransform::visit(yul::Expression const& _expression)
{
	return make_unique<wasm::Expression>(std::visit(*this, _expression));
}

wasm::Expression WasmCodeTransform::visitReturnByValue(yul::Expression const& _expression)
{
	return std::visit(*this, _expression);
}

vector<wasm::Expression> WasmCodeTransform::visit(vector<yul::Expression> const& _expressions)
{
	vector<wasm::Expression> ret;
	for (auto const& e: _expressions)
		ret.emplace_back(visitReturnByValue(e));
	return ret;
}

wasm::Expression WasmCodeTransform::visit(yul::Statement const& _statement)
{
	return std::visit(*this, _statement);
}

vector<wasm::Expression> WasmCodeTransform::visit(vector<yul::Statement> const& _statements)
{
	vector<wasm::Expression> ret;
	for (auto const& s: _statements)
		ret.emplace_back(visit(s));
	return ret;
}

wasm::FunctionDefinition WasmCodeTransform::translateFunction(yul::FunctionDefinition const& _fun)
{
	wasm::FunctionDefinition fun;
	fun.name = _fun.name.str();
	for (auto const& param: _fun.parameters)
		fun.parameterNames.emplace_back(param.name.str());
	for (auto const& retParam: _fun.returnVariables)
		fun.locals.emplace_back(wasm::VariableDeclaration{retParam.name.str()});
	fun.returns = !_fun.returnVariables.empty();

	yulAssert(m_localVariables.empty(), "");
	yulAssert(m_functionBodyLabel.empty(), "");
	m_functionBodyLabel = newLabel();
	fun.body.emplace_back(wasm::Expression(wasm::Block{
		m_functionBodyLabel,
		visit(_fun.body.statements)
	}));
	fun.locals += m_localVariables;

	m_localVariables.clear();
	m_functionBodyLabel = {};

	if (!_fun.returnVariables.empty())
	{
		// First return variable is returned directly, the others are stored
		// in globals.
		allocateGlobals(_fun.returnVariables.size() - 1);
		for (size_t i = 1; i < _fun.returnVariables.size(); ++i)
			fun.body.emplace_back(wasm::GlobalAssignment{
				m_globalVariables.at(i - 1).variableName,
				make_unique<wasm::Expression>(wasm::LocalVariable{_fun.returnVariables.at(i).name.str()})
			});
		fun.body.emplace_back(wasm::LocalVariable{_fun.returnVariables.front().name.str()});
	}
	return fun;
}

wasm::Expression WasmCodeTransform::injectTypeConversionIfNeeded(wasm::FunctionCall _call) const
{
	wasm::FunctionImport const& import = m_functionsToImport.at(YulString{_call.functionName});
	for (size_t i = 0; i < _call.arguments.size(); ++i)
		if (import.paramTypes.at(i) == "i32")
			_call.arguments[i] = wasm::BuiltinCall{"i32.wrap_i64", make_vector<wasm::Expression>(std::move(_call.arguments[i]))};
		else
			yulAssert(import.paramTypes.at(i) == "i64", "Unknown type " + import.paramTypes.at(i));

	if (import.returnType && *import.returnType != "i64")
	{
		yulAssert(*import.returnType == "i32", "Invalid type " + *import.returnType);
		return wasm::BuiltinCall{"i64.extend_i32_u", make_vector<wasm::Expression>(std::move(_call))};
	}
	return {std::move(_call)};
}

vector<wasm::Expression> WasmCodeTransform::injectTypeConversionIfNeeded(
	vector<wasm::Expression> _arguments,
	vector<Type> const& _parameterTypes
) const
{
	for (size_t i = 0; i < _arguments.size(); ++i)
		if (_parameterTypes.at(i) == "i32"_yulstring)
			_arguments[i] = wasm::BuiltinCall{"i32.wrap_i64", make_vector<wasm::Expression>(std::move(_arguments[i]))};
		else
			yulAssert(
				_parameterTypes.at(i).empty() || _parameterTypes.at(i) == "i64"_yulstring,
				"Unknown type " + _parameterTypes.at(i).str()
			);

	return _arguments;
}

string WasmCodeTransform::newLabel()
{
	return m_nameDispenser.newName("label_"_yulstring).str();
}

void WasmCodeTransform::allocateGlobals(size_t _amount)
{
	while (m_globalVariables.size() < _amount)
		m_globalVariables.emplace_back(wasm::GlobalVariableDeclaration{
			m_nameDispenser.newName("global_"_yulstring).str()
		});
}
