/*
* Copyright (c) 2019. tangzx(love.tangzx@qq.com)
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "emmy_debugger/emmy_debugger.h"
#include <algorithm>
#include <sstream>
#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/hook_state.h"
#include "emmy_debugger/emmy_helper.h"

#define CACHE_TABLE_NAME "_emmy_cache_table_"

int cacheId = 1;

void HookLua(lua_State* L, lua_Debug* ar)
{
	EmmyFacade::Get().Hook(L, ar);
}

Debugger::Debugger(lua_State* L):
	L(L),
	hookState(nullptr),
	running(false),
	skipHook(false),
	blocking(false)
{
}

Debugger::~Debugger()
{
}

void Debugger::Start()
{
	skipHook = false;
	blocking = false;
	running = true;
	doStringList.clear();
	RemoveAllBreakpoints();

	// ��EmmyFacade �̳жϵ�
	const auto& breakPoints = EmmyFacade::Get().GetBreakPoints();
	this->breakPoints = breakPoints;

	RefreshLineSet();
}

void Debugger::Attach(bool doHelperCode)
{
	if (!running)
		return;


	// execute helper code
	if (doHelperCode && !EmmyFacade::Get().GetHelperCode().empty())
	{
		ExecuteOnLuaThread([this](lua_State* L)
		{
			const int t = lua_gettop(L);
			const int r = luaL_loadstring(L, EmmyFacade::Get().GetHelperCode().c_str());
			if (r == LUA_OK)
			{
				if (lua_pcall(L, 0, 0, 0) != LUA_OK)
				{
					std::string msg = lua_tostring(L, -1);
					printf("msg: %s", msg.c_str());
				}
			}
			lua_settop(L, t);
		});
	}
	// todo: just set hook when break point added.
	UpdateHook(LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET);

	// todo: hook co
	// auto root = G(L)->allgc;
}

void Debugger::Detach()
{
	// const auto it = states.find(L);
	// if (it != states.end())
	// {
	// 	states.erase(it);
	// }
}

void Debugger::Hook(lua_Debug* ar)
{
	if (skipHook)
	{
		return;
	}
	if (getDebugEvent(ar) == LUA_HOOKLINE)
	{
		if (luaThreadExecutors.empty() == false)
		{
			std::unique_lock<std::mutex> lock(mutexLuaThread);
			for (auto& executor : luaThreadExecutors)
			{
				ExecuteWithSkipHook(executor);
			}
			luaThreadExecutors.clear();
		}
		auto bp = FindBreakPoint(ar);
		if (bp && ProcessBreakPoint(bp))
		{
			HandleBreak();
			return;
		}
		if (hookState)
		{
			hookState->ProcessHook(shared_from_this(), L, ar);
		}
	}
}

void Debugger::Stop()
{
	running = false;
	skipHook = true;
	blocking = false;

	UpdateHook(0);

	hookState = nullptr;

	// clear lua thread executors
	std::unique_lock<std::mutex> luaThreadLock(mutexLuaThread);
	luaThreadExecutors.clear();

	ExitDebugMode();
}

bool Debugger::IsRunning() const
{
	return running;
}

bool Debugger::GetStacks(std::vector<std::shared_ptr<Stack>>& stacks, StackAllocatorCB alloc)
{
	int level = 0;
	while (true)
	{
		lua_Debug ar{};
		if (!lua_getstack(L, level, &ar))
		{
			break;
		}
		if (!lua_getinfo(L, "nSlu", &ar))
		{
			continue;
		}
		auto stack = alloc();
		stack->file = GetFile(&ar);
		stack->functionName = getDebugName(&ar) == nullptr ? "" : getDebugName(&ar);
		stack->level = level;
		stack->line = getDebugCurrentLine(&ar);
		stacks.push_back(stack);
		// get variables
		{
			for (int i = 1;; i++)
			{
				const char* name = lua_getlocal(L, &ar, i);
				if (name == nullptr)
				{
					break;
				}
				if (name[0] == '(')
				{
					lua_pop(L, 1);
					continue;
				}

				// add local variable
				auto var = stack->CreateVariable();
				var->name = name;
				GetVariable(var, -1, 1);
				lua_pop(L, 1);
				stack->localVariables.push_back(var);
			}

			if (lua_getinfo(L, "f", &ar))
			{
				const int fIdx = lua_gettop(L);
				for (int i = 1;; i++)
				{
					const char* name = lua_getupvalue(L, fIdx, i);
					if (!name)
					{
						break;
					}

					// add up variable
					auto var = stack->CreateVariable();
					var->name = name;
					GetVariable(var, -1, 1);
					lua_pop(L, 1);
					stack->upvalueVariables.push_back(var);
				}
				// pop function
				lua_pop(L, 1);
			}
		}

		level++;
	}
	return false;
}

bool CallMetaFunction(lua_State* L, int valueIndex, const char* method, int numResults, int& result)
{
	if (lua_getmetatable(L, valueIndex))
	{
		const int metaIndex = lua_gettop(L);
		if (!lua_isnil(L, metaIndex))
		{
			lua_pushstring(L, method);
			lua_rawget(L, metaIndex);
			if (lua_isnil(L, -1))
			{
				// The meta-method doesn't exist.
				lua_pop(L, 1);
				lua_remove(L, metaIndex);
				return false;
			}
			lua_pushvalue(L, valueIndex);
			result = lua_pcall(L, 1, numResults, 0);
		}
		lua_remove(L, metaIndex);
		return true;
	}
	return false;
}

std::string ToPointer(lua_State* L, int index)
{
	const void* pointer = lua_topointer(L, index);
	std::stringstream ss;
	ss << lua_typename(L, lua_type(L, index)) << "(0x" << std::hex << pointer << ")";
	return ss.str();
}

void Debugger::GetVariable(std::shared_ptr<Variable> variable, int index, int depth, bool queryHelper)
{
	const int t1 = lua_gettop(L);
	index = lua_absindex(L, index);
	CacheValue(index, variable);
	const int type = lua_type(L, index);
	const char* typeName = lua_typename(L, type);
	variable->valueTypeName = typeName;
	variable->valueType = type;
	if (queryHelper && (type == LUA_TTABLE || type == LUA_TUSERDATA))
	{
		if (query_variable(L, variable, typeName, index, depth))
		{
			return;
		}
	}
	switch (type)
	{
	case LUA_TNIL:
		{
			variable->value = "nil";
			break;
		}
	case LUA_TNUMBER:
		{
			variable->value = lua_tostring(L, index);
			break;
		}
	case LUA_TBOOLEAN:
		{
			const bool v = lua_toboolean(L, index);
			variable->value = v ? "true" : "false";
			break;
		}
	case LUA_TSTRING:
		{
			variable->value = lua_tostring(L, index);
			break;
		}
	case LUA_TUSERDATA:
		{
			auto* string = lua_tostring(L, index);
			if (string == nullptr)
			{
				int result;
				if (CallMetaFunction(L, t1, "__tostring", 1, result) && result == 0)
				{
					string = lua_tostring(L, -1);
					lua_pop(L, 1);
				}
			}
			if (string)
			{
				variable->value = string;
			}
			else
			{
				variable->value = ToPointer(L, index);
			}
			if (depth > 1)
			{
				if (lua_getmetatable(L, index))
				{
					GetVariable(variable, -1, depth);
					lua_pop(L, 1); //pop meta
				}
			}
			break;
		}
	case LUA_TFUNCTION:
	case LUA_TLIGHTUSERDATA:
	case LUA_TTHREAD:
		{
			variable->value = ToPointer(L, index);
			break;
		}
	case LUA_TTABLE:
		{
			int tableSize = 0;
			const void* tableAddr = lua_topointer(L, index);
			lua_pushnil(L);
			while (lua_next(L, index))
			{
				// k: -2, v: -1
				if (depth > 1)
				{
					//todo: use allocator
					auto v = std::make_shared<Variable>();
					const auto t = lua_type(L, -2);
					v->nameType = t;
					if (t == LUA_TSTRING || t == LUA_TNUMBER || t == LUA_TBOOLEAN)
					{
						lua_pushvalue(L, -2); // avoid error: "invalid key to 'next'" ???
						v->name = lua_tostring(L, -1);
						lua_pop(L, 1);
					}
					else
					{
						v->name = ToPointer(L, -2);
					}
					GetVariable(v, -1, depth - 1);
					variable->children.push_back(v);
				}
				lua_pop(L, 1);
				tableSize++;
			}


			if (lua_getmetatable(L, index))
			{
				// metatable
				auto metatable = std::make_shared<Variable>();
				metatable->name = "metatable";
				metatable->nameType = LUA_TSTRING;

				GetVariable(metatable, -1, 1);
				variable->children.push_back(metatable);

				//__index
				{
					lua_getfield(L, -1, "__index");
					if (!lua_isnil(L, -1))
					{
						auto v = std::make_shared<Variable>();
						GetVariable(v, -1, 1);
						if (depth > 1)
						{
							for (auto child : v->children)
							{
								variable->children.push_back(child->Clone());
							}
						}
						tableSize += v->children.size();
					}
					lua_pop(L, 1);
				}

				// metatable
				lua_pop(L, 1);
			}

			std::stringstream ss;
			ss << "table(0x" << std::hex << tableAddr << std::dec << ", size = " << tableSize << ")";
			variable->value = ss.str();
			break;
		}
	}
	const int t2 = lua_gettop(L);
	assert(t2 == t1);
}

void Debugger::CacheValue(int valueIndex, std::shared_ptr<Variable> variable) const
{
	const int type = lua_type(L, valueIndex);
	if (type == LUA_TUSERDATA || type == LUA_TTABLE)
	{
		const int id = cacheId++;
		variable->cacheId = id;
		const int t1 = lua_gettop(L);
		lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME); // 1: cacheTable|nil
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1); //
			lua_newtable(L); // 1: {}
			lua_setfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME); //
			lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME); // 1: cacheTable
		}
		lua_pushvalue(L, valueIndex); // 1: cacheTable, 2: value
		char Key[10];
		sprintf(Key, "%d", id);
		lua_setfield(L, -2, Key); // 1: cacheTable
		lua_settop(L, t1);
	}
}

void Debugger::ClearCache() const
{
	lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);
	if (!lua_isnil(L, -1))
	{
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME);
	}
	lua_pop(L, 1);
	
}

void Debugger::DoAction(DebugAction action)
{
	switch (action)
	{
	case DebugAction::Break:
		SetHookState(EmmyFacade::Get().stateBreak);
		break;
	case DebugAction::Continue:
		SetHookState(EmmyFacade::Get().stateContinue);
		break;
	case DebugAction::StepOver:
		SetHookState(EmmyFacade::Get().stateStepOver);
		break;
	case DebugAction::StepIn:
		SetHookState(EmmyFacade::Get().stateStepIn);
		break;
	case DebugAction::Stop:
		SetHookState(EmmyFacade::Get().stateStop);
		break;
	default: break;
	}
}

void Debugger::UpdateHook(int mask)
{
	if (mask == 0)
		lua_sethook(L, nullptr, mask, 0);
	else
		lua_sethook(L, HookLua, mask, 0);
}

// _G.emmy.fixPath = function(path) return (newPath) end
int FixPath(lua_State* L)
{
	const auto path = lua_tostring(L, 1);
	lua_getglobal(L, "emmy");
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, "fixPath");
		if (lua_isfunction(L, -1))
		{
			lua_pushstring(L, path);
			lua_call(L, 1, 1);
			return 1;
		}
	}
	return 0;
}

std::string Debugger::GetFile(lua_Debug* ar) const
{
	assert(L);
	const char* file = getDebugSource(ar);
	if (getDebugCurrentLine(ar) < 0)
		return file;
	if (strlen(file) > 0 && file[0] == '@')
		file++;
	lua_pushcclosure(L, FixPath, 0);
	lua_pushstring(L, file);
	const int result = lua_pcall(L, 1, 1, 0);
	if (result == LUA_OK)
	{
		const auto p = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (p)
		{
			return p;
		}
	}
	// todo: handle error
	return file;
}

void Debugger::HandleBreak()
{
	// to be on the safe side, hook it again
	UpdateHook(LUA_MASKCALL | LUA_MASKLINE | LUA_MASKRET);
	EmmyFacade::Get().OnBreak(L);
	EnterDebugMode();
}

// host thread
void Debugger::EnterDebugMode()
{
	std::unique_lock<std::mutex> lock(mutexRun);
	blocking = true;
	while (true)
	{
		std::unique_lock<std::mutex> lockEval(mutexEval);
		if (evalQueue.empty() && blocking)
		{
			lockEval.unlock();
			cvRun.wait(lock);
			lockEval.lock();
		}
		if (!evalQueue.empty())
		{
			const auto evalContext = evalQueue.front();
			evalQueue.pop();
			lockEval.unlock();
			const bool skip = skipHook;
			skipHook = true;
			evalContext->success = DoEval(evalContext);
			skipHook = skip;
			EmmyFacade::Get().OnEvalResult(evalContext);
			continue;
		}
		break;
	}
	ClearCache();
	EmmyFacade::Get().ResetBreakedDebugger();
}

void Debugger::ExitDebugMode()
{
	blocking = false;
	cvRun.notify_all();
}

void Debugger::AddBreakPoint(std::shared_ptr<BreakPoint> bp)
{
	std::lock_guard<std::mutex> lock(mutexBP);
	breakPoints.push_back(bp);
	RefreshLineSet();
}

void Debugger::RemoveBreakPoint(const std::string& file, int line)
{
	std::string lowerCaseFile = file;
	std::transform(file.begin(), file.end(), lowerCaseFile.begin(), tolower);
	std::lock_guard<std::mutex> lock(mutexBP);
	auto it = breakPoints.begin();
	while (it != breakPoints.end())
	{
		const auto bp = *it;
		if (bp->file == lowerCaseFile && bp->line == line)
		{
			breakPoints.erase(it);
			break;
		}
		++it;
	}
	RefreshLineSet();
}

void Debugger::RemoveAllBreakpoints()
{
	lineSet.clear();
	breakPoints.clear();
}

int EnvIndexFunction(lua_State* L)
{
	const int locals = lua_upvalueindex(1);
	const int upvalues = lua_upvalueindex(2);
	const char* name = lua_tostring(L, 2);
	// up value
	lua_getfield(L, upvalues, name);
	if (lua_isnil(L, -1) == 0)
	{
		return 1;
	}
	lua_pop(L, 1);
	// local value
	lua_getfield(L, locals, name);
	if (lua_isnil(L, -1) == 0)
	{
		return 1;
	}
	lua_pop(L, 1);
	// _ENV
	lua_getfield(L, upvalues, "_ENV");
	if (lua_istable(L, -1))
	{
		lua_getfield(L, -1, name); // _ENV[name]
		if (lua_isnil(L, -1) == 0)
		{
			return 1;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	// global
	lua_getglobal(L, name);
	if (lua_isnil(L, -1) == 0)
	{
		return 1;
	}
	lua_pop(L, 1);
	return 0;
}

bool Debugger::CreateEnv(lua_State* L, int stackLevel)
{
	//assert(L);
	//const auto L = L;

	lua_Debug ar{};
	if (!lua_getstack(L, stackLevel, &ar))
	{
		return false;
	}
	if (!lua_getinfo(L, "nSlu", &ar))
	{
		return false;
	}

	lua_newtable(L);
	const int env = lua_gettop(L);
	lua_newtable(L);
	const int envMetatable = lua_gettop(L);
	lua_newtable(L);
	const int locals = lua_gettop(L);
	lua_newtable(L);
	const int upvalues = lua_gettop(L);

	int idx = 1;
	// local values
	while (true)
	{
		const char* name = lua_getlocal(L, &ar, idx++);
		if (name == nullptr)
			break;
		if (name[0] == '(')
		{
			lua_pop(L, 1);
			continue;
		}
		lua_setfield(L, locals, name);
	}
	// up values
	if (lua_getinfo(L, "f", &ar))
	{
		const int fIdx = lua_gettop(L);
		idx = 1;
		while (true)
		{
			const char* name = lua_getupvalue(L, fIdx, idx++);
			if (name == nullptr)
				break;
			lua_setfield(L, upvalues, name);
		}
		lua_pop(L, 1);
	}
	int top = lua_gettop(L);
	assert(top == upvalues);

	// index function
	// up value: locals, upvalues
	lua_pushcclosure(L, EnvIndexFunction, 2);

	// envMetatable.__index = EnvIndexFunction
	lua_setfield(L, envMetatable, "__index");
	// setmetatable(env, envMetatable)
	lua_setmetatable(L, env);

	top = lua_gettop(L);
	assert(top == env);
	return true;
}

bool Debugger::ProcessBreakPoint(std::shared_ptr<BreakPoint> bp)
{
	if (!bp->condition.empty())
	{
		auto ctx = std::make_shared<EvalContext>();
		ctx->expr = bp->condition;
		bool suc = DoEval(ctx);
		return suc && ctx->result->valueType == LUA_TBOOLEAN && ctx->result->value == "true";
	}
	if (!bp->logMessage.empty())
	{
		EmmyFacade::Get().SendLog(LogType::Info, bp->logMessage.c_str());
		return false;
	}
	if (!bp->hitCondition.empty())
	{
		bp->hitCount++;
		//TODO check hit condition
		return false;
	}
	return true;
}

void Debugger::SetHookState(std::shared_ptr<HookState> newState)
{
	hookState = nullptr;
	if (newState->Start(shared_from_this(), L))
	{
		hookState = newState;
	}
}

int Debugger::GetStackLevel(bool skipC) const
{
	int level = 0, i = 0;
	lua_Debug ar{};
	while (lua_getstack(L, i, &ar))
	{
		lua_getinfo(L, "l", &ar);
		if (getDebugCurrentLine(&ar) >= 0 || !skipC)
			level++;
		i++;
	}
	return level;
}

void Debugger::AsyncDoString(const std::string& code)
{
	doStringList.emplace_back(code);
}

void Debugger::CheckDoString()
{
	if (!doStringList.empty())
	{
		const auto skip = skipHook;
		skipHook = true;
		const int t = lua_gettop(L);
		for (const auto& code : doStringList)
		{
			const int r = luaL_loadstring(L, code.c_str());
			if (r == LUA_OK)
			{
				lua_pcall(L, 0, 0, 0);
			}
			lua_settop(L, t);
		}
		skipHook = skip;
		assert(lua_gettop(L) == t);
		doStringList.clear();
	}
}

// message thread
bool Debugger::Eval(std::shared_ptr<EvalContext> evalContext, bool force)
{
	if (force)
		return DoEval(evalContext);
	if (!blocking)
		return false;
	std::unique_lock<std::mutex> lock(mutexEval);
	evalQueue.push(evalContext);
	lock.unlock();
	cvRun.notify_all();
	return true;
}

// host thread
bool Debugger::DoEval(std::shared_ptr<EvalContext> evalContext)
{
	assert(L);
	assert(evalContext);
	//auto* const L = L;
	// From "cacheId"
	if (evalContext->cacheId > 0)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, CACHE_TABLE_NAME); // 1: cacheTable|nil
		if (lua_type(L, -1) == LUA_TTABLE)
		{
			lua_getfield(L, -1, std::to_string(evalContext->cacheId).c_str()); // 1: cacheTable, 2: value
			GetVariable(evalContext->result, -1, evalContext->depth);
			lua_pop(L, 2);
			return true;
		}
		lua_pop(L, 1);
	}
	// LOAD AS "return expr"
	std::string statement = "return ";
	statement.append(evalContext->expr);
	int r = luaL_loadstring(L, statement.c_str());
	if (r == LUA_ERRSYNTAX)
	{
		evalContext->error = "syntax err: ";
		evalContext->error.append(evalContext->expr);
		return false;
	}
	// call
	const int fIdx = lua_gettop(L);
	// create env
	if (!CreateEnv(L, evalContext->stackLevel))
		return false;
	// setup env
#ifndef EMMY_USE_LUA_SOURCE
	lua_setfenv(L, fIdx);
#elif EMMY_LUA_51
    lua_setfenv(L, fIdx);
#else //52 & 53
    lua_setupvalue(L, fIdx, 1);
#endif
	assert(lua_gettop(L) == fIdx);
	// call function() return expr end
	r = lua_pcall(L, 0, 1, 0);
	if (r == LUA_OK)
	{
		evalContext->result->name = evalContext->expr;
		GetVariable(evalContext->result, -1, evalContext->depth);
		lua_pop(L, 1);
		return true;
	}
	if (r == LUA_ERRRUN)
	{
		evalContext->error = lua_tostring(L, -1);
	}

	return false;
}

std::shared_ptr<BreakPoint> Debugger::FindBreakPoint(lua_Debug* ar)
{
	const int cl = getDebugCurrentLine(ar);
	if (cl >= 0 && lineSet.find(cl) != lineSet.end())
	{
		lua_getinfo(L, "S", ar);
		const auto file = GetFile(ar);
		return FindBreakPoint(file, cl);
	}
	return nullptr;
}

std::shared_ptr<BreakPoint> Debugger::FindBreakPoint(const std::string& file, int line)
{
	std::lock_guard<std::mutex> lock(mutexBP);
	std::vector<std::string> pathParts;
	std::string lowerCaseFile = file;
	std::transform(file.begin(), file.end(), lowerCaseFile.begin(), tolower);
	ParsePathParts(lowerCaseFile, pathParts);
	auto it = breakPoints.begin();
	while (it != breakPoints.end())
	{
		auto const bp = *it;
		if (bp->line == line)
		{
			// full match: bp(a/b/c), file(a/b/c)
			if (bp->file == lowerCaseFile)
			{
				return *it;
			}
			// fuzz match: bp(x/a/b/c), file(a/b/c)
			if (bp->pathParts.size() >= pathParts.size() && MatchFileName(pathParts.back(), bp->pathParts.back()))
			{
				bool match = true;
				for (size_t i = 1; i < pathParts.size(); i++)
				{
					const auto p = *(bp->pathParts.end() - i - 1);
					const auto f = *(pathParts.end() - i - 1);
					if (p != f)
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					return bp;
				}
			}
		}
		++it;
	}

	return nullptr;
}

bool Debugger::MatchFileName(const std::string& chunkName, const std::string& fileName) const
{
	if (chunkName == fileName)
		return true;
	// abc == abc.lua
	for (const auto& ext : EmmyFacade::Get().GetExtNames())
	{
		if (chunkName + ext == fileName)
		{
			return true;
		}
	}
	return false;
}


void Debugger::RefreshLineSet()
{
	lineSet.clear();
	for (auto bp : breakPoints)
	{
		lineSet.insert(bp->line);
	}
}

void Debugger::ExecuteWithSkipHook(const Executor& exec)
{
	const bool skip = skipHook;
	skipHook = true;
	exec(L);
	skipHook = skip;
}

void Debugger::ExecuteOnLuaThread(const Executor& exec)
{
	std::unique_lock<std::mutex> lock(mutexLuaThread);
	luaThreadExecutors.push_back(exec);
}

