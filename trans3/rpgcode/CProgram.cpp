/*
 ********************************************************************
 * The RPG Toolkit, Version 3
 * This file copyright (C) 2006, 2007  Colin James Fitzpatrick
 ********************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Creating a game EXE using the Make EXE feature creates a 
 * derivative version of trans3 that includes the game's files. 
 * Therefore the EXE must be licensed under the GPL. However, as a 
 * special exception, you are permitted to license EXEs made with 
 * this feature under whatever terms you like, so long as 
 * Corresponding Source, as defined in the GPL, of the version 
 * of trans3 used to make the EXE is available separately under 
 * terms compatible with the Licence of this software and that you 
 * do not charge, aside from any price of the game EXE, to obtain 
 * these components.
 * 
 * If you publish a modified version of this Program, you may delete
 * these exceptions from its distribution terms, or you may choose
 * to propagate them.
 */

/*
 * RPGCode parser and interpreter.
 */

#include "CProgram.h"
#include "COptimiser.h"
#include "CVariant.h"
#include "CGarbageCollector.h"
#include "../plugins/plugins.h"
#include "../plugins/constants.h"
#include "../common/mbox.h"
#include "../common/paths.h"
#include "../common/CFile.h"
#include "../input/input.h"
#include "../../tkCommon/strings.h"
#include <malloc.h>
#include <math.h>
#include <algorithm> 
#include "assert.h"

#ifdef ENABLE_MUMU_DBG
#include "CMumuDebugger.h"
bool CProgram::m_enableMumu = true;
#endif

// Static member initialization.
std::vector<tagNamedMethod> tagNamedMethod::m_methods;
std::map<STRING, MACHINE_FUNC> CProgram::m_functions;
LPMACHINE_UNITS CProgram::m_pyyUnits = NULL;
std::deque<MACHINE_UNITS> CProgram::m_yyFors;
std::map<STRING, CPtrData<STACK_FRAME> > CProgram::m_heap;
std::deque<int> CProgram::m_params;
std::map<STRING, CLASS> *CProgram::m_pClasses = NULL;
std::map<unsigned int, STRING> CProgram::m_objects;
std::vector<unsigned int> *CProgram::m_pLines = NULL;
std::vector<STRING> *CProgram::m_pInclusions = NULL;
std::vector<IPlugin *> CProgram::m_plugins;
std::map<STRING, STACK_FRAME> CProgram::m_constants;
std::map<STRING, STRING> CProgram::m_redirects;
std::set<CThread *> CThread::m_threads;
STRING CProgram::m_parsing;
unsigned long CProgram::m_runningPrograms = 0;
EXCEPTION_TYPE CProgram::m_debugLevel = E_WARNING;	// Show all error messages by default.

// Critical section for garbage collection.
LPCRITICAL_SECTION g_mutex = NULL;

static std::map<STRING, CProgram> g_cache; // Program cache.
typedef std::map<STRING, CProgram>::iterator CACHE_ITR;

/*
 * *************************************************************************
 * Global Scope
 * *************************************************************************
 */

// Parsing error handler.
int yyerror(const char *error)
{
	extern unsigned int g_lines;
	TCHAR str[255];
	// No +1 because the first line is a hacky bug fix.
	// See CProgram::open().
	_itot(g_lines, str, 10);
#ifndef _UNICODE
	STRING strError = error;
#else
	STRING strError = getUnicodeString(std::string(error));
#endif
	strError[0] = toupper(strError[0]);
	CProgram::debugger(CProgram::m_parsing + _T("\r\nLine ") + str + _T(": ") + strError + _T("."));
	return 0;
}

namespace // Anonymous namespace
{
	inline STRING getUnitDataType(UNIT_DATA_TYPE udt)
	{
		STRING ret;

		if (udt & UDT_UNSET) ret += "UDT_UNSET, ";
		if (udt & UDT_NUM) ret += "UDT_NUM, ";
		if (udt & UDT_LIT) ret += "UDT_LIT, ";
		if (udt & UDT_ID) ret += "UDT_ID, ";
		if (udt & UDT_FUNC) ret += "UDT_FUNC, ";
		if (udt & UDT_OPEN) ret += "UDT_OPEN, ";
		if (udt & UDT_CLOSE) ret += "UDT_CLOSE, ";
		if (udt & UDT_LINE) ret += "UDT_LINE, ";
		if (udt & UDT_OBJ) ret += "UDT_OBJ, ";
		if (udt & UDT_LABEL) ret += "UDT_LABEL, ";
		if (udt & UDT_PLUGIN) ret += "UDT_PLUGIN, ";

		return (!ret.empty()) ? ret.substr(0, ret.length() - 2) : STRING();
	}

	// Serialise a stack frame.
	inline void serialiseStackFrame(CFile &stream, const STACK_FRAME &sf)
	{
		// Do not bother writing 'tag'; it is only for virtual
		// variables and they cannot be serialised anyway.

		// The member 'prg' is also not written because it is
		// just a pointer to the current program.
		stream << sf.num << sf.lit << int(sf.udt);
	}

	inline void reconstructStackFrame(CFile &stream, STACK_FRAME &sf)
	{
		int udt = 0;
		stream >> sf.num >> sf.lit >> udt;
		sf.udt = UNIT_DATA_TYPE(udt);
	}

	// Read a string.
	inline STRING freadString(FILE *file)
	{
		STRING ret;
		TCHAR c = _T('\0');
		while (fread(&c, sizeof(TCHAR), 1, file) != 0)
		{
			if (c == _T('\0')) break;
			ret += c;
		}
		return ret;
	}

	// Walks up the unit stack to the head of the starting unit's parameter list.
	// /pos/ is modified to point to the new position.
	// Returns the distance from the starting position.
	inline int gotoInsertionPoint(CONST_POS &pos, const MACHINE_UNITS &units)
	{
		int k = 0;
		for (int j = pos->params; j > 0; j--, k++)
		{
			if (pos == units.begin())
				throw CError(_T("Out of range while searching for insertion point.")); 
			--pos;
			j += pos->params;
		}
		return k;
	}
} // Anonymous namespace


#define YYSTACKSIZE 50000
#define YYSTYPE CVariant
#include "y.tab.c"

// opr - the overloaded operator to check for
// call[0] must be an object.
inline bool checkOverloadedOperator(const STRING opr, CALL_DATA &call)
{
	const unsigned int obj = static_cast<unsigned int>(call[0].getNum());
	std::map<unsigned int, STRING>::const_iterator res = CProgram::m_objects.find(obj);
	//assert(res != CProgram::m_objects.end());
	if (res == CProgram::m_objects.end())
		return false;

	const STRING &type = res->second;
	std::map<STRING, tagClass>::iterator k = call.prg->m_classes.find(type);
	assert(k != call.prg->m_classes.end());
	if (k == call.prg->m_classes.end())
		return false;

	// Check if class overloads the specified operator.
	const STRING method = _T("operator") + opr;
	CLASS_VISIBILITY cv = CV_PUBLIC;

	if (call.prg->m_calls.size())
	{
		const unsigned int callerObj = call.prg->m_calls.back().obj;
		std::map<unsigned int, STRING>::const_iterator res = CProgram::m_objects.find(callerObj);
		if (res != CProgram::m_objects.end() && res->second == type)
		{
			cv = CV_PRIVATE;
		}
	}

	if (!k->second.locate(method, call.params - 1, cv))
		return false;

	STACK_FRAME &fra = call.ret();
	fra.udt = UDT_OBJ;
	fra.lit = method;
	call.prg->m_stack[call.prg->m_stackIndex].push_back(call.prg);
	call.p = &call.prg->m_stack[call.prg->m_stackIndex].back() - (++call.params);
	CProgram::methodCall(call);
	return true;
}

//`
#define CHECK_OVERLOADED_OPERATOR(opr, fail) \
	if (call[0].getType() & UDT_OBJ) \
	{ \
		if (checkOverloadedOperator(_T(#opr), call)) \
			return; \
		else \
		{ \
			if (fail) \
				throw CError(_T("No overloaded operator ") _T(#opr) _T(" found!")); \
		} \
	}

/*
 * *************************************************************************
 * CProgram
 * *************************************************************************
 */

// Copy constructor for CProgram.
CProgram::CProgram(const CProgram &rhs)
{
	CGarbageCollector::getInstance().addProgram(this);
	*this = rhs;
}

CProgram::CProgram(tagBoardProgram *pBrdProgram):
		m_pBoardPrg(pBrdProgram)
{
	CGarbageCollector::getInstance().addProgram(this);
}

CProgram::CProgram(const STRING file, tagBoardProgram *pBrdProgram):
		m_pBoardPrg(pBrdProgram)
{
	CGarbageCollector::getInstance().addProgram(this);
	open(file);
}

CProgram::~CProgram()
{
	try
	{
		CGarbageCollector::getInstance().removeProgram(this);
	}
	catch (...)
	{
		// The above will fail if we are exiting.
	}
}

// Assignment operator.
CProgram &CProgram::operator=(const CProgram &rhs)
{
	// Just copy over most of the members.
	m_stack = rhs.m_stack;
	m_stackIndex = rhs.m_stackIndex;
	m_locals = rhs.m_locals;
	m_calls = rhs.m_calls;
	m_classes = rhs.m_classes;
	m_lines = rhs.m_lines;
	//m_pBoardPrg = rhs.m_pBoardPrg; // Do not copy this.
	m_units = rhs.m_units;
	m_i = rhs.m_i;
	m_methods = rhs.m_methods;
	m_inclusions = rhs.m_inclusions;
	m_fileName = rhs.m_fileName;

	return *this;
}

// Show the debugger.
void CProgram::debugger(const STRING str)
{
	messageBox(_T("RPGCode Error\n\n") + str);
}

// Free loaded plugins.
void CProgram::freePlugins()
{
	std::vector<IPlugin *>::iterator i = m_plugins.begin();
	for (; i != m_plugins.end(); ++i)
	{
		(*i)->terminate();
		delete *i;
	}
	m_plugins.clear();
}

// Estimate the line a unit is part of. Doesn't work at all for
// compiled code, but this is okay because compiled code is
// presumably free of compile-time errors. Has unavoidable
// errors with for loops. but an error in the third section
// of a for loop is so rare that this shouldn't be a big issue.
// Finally, included files with loose code will not work.
unsigned int CProgram::getLine(CONST_POS pos) const
{
	unsigned int i = pos - m_units.begin();
	std::vector<unsigned int>::const_iterator j = m_lines.begin();
	for (; j != m_lines.end(); ++j)
	{
		// No +1 because the first line is a hacky bug fix.
		// See CProgram::open().
		if (*j > i) return (j - m_lines.begin());
	}
	return m_lines.size() - 1;
}

// Get the name of an instance variable in the heap.
inline std::pair<bool, STRING> CProgram::getInstanceVar(const STRING &name) const
{
	if (!m_calls.size())
	{
		return std::pair<bool, STRING>(false, STRING());
	}

	const unsigned int obj = m_calls.back().obj;
	if (!obj)
	{
		return std::pair<bool, STRING>(false, STRING());
	}

	std::map<unsigned int, STRING>::const_iterator obj_it = m_objects.find(obj);
	if (obj_it != m_objects.end())
	{
		std::map<STRING, CLASS>::const_iterator cls_it = m_classes.find(obj_it->second);
		if ((cls_it != m_classes.end()) && cls_it->second.memberExists(name, CV_PRIVATE))
		{
			TCHAR str[33];
			_itot(obj, str, 10);
			return std::pair<bool, STRING>(true, STRING(str) + _T("::") + name);
		}
	}

	return std::pair<bool, STRING>(false, STRING());
}

// Get a variable.
LPSTACK_FRAME CProgram::getVar(const STRING &name, unsigned int *pFrame, STRING *pName)
{
	if (name[0] == _T(':'))
	{
		STRING var = name.substr(1);
		if (pName) *pName = var;
		return m_heap[var];
	}
	if (name[0] == _T(' '))
	{
		const unsigned int i = (unsigned int)name[1];
		REFERENCE_MAP &r = m_calls.back().refs;
		REFERENCE_MAP::iterator j = r.find(i);
		if (j != r.end())
		{
			return j->second.first;
		}
	}
	// TBD: This should be done at compile-time!
	const std::pair<bool, STRING> res = getInstanceVar(name);
	if (res.first)
	{
		const STRING &qualified = res.second;
		if (pName) *pName = qualified;
		return m_heap[qualified];
	}
	return (this->*m_pResolveFunc)(name, pFrame);
}

// Prefer the global scope when resolving a variable.
LPSTACK_FRAME CProgram::resolveVarGlobal(const STRING &name, unsigned int *pFrame)
{
	std::list<std::map<STRING, STACK_FRAME> > *pLocalList = getLocals();
	std::map<STRING, STACK_FRAME> *pLocals = &pLocalList->back();

	std::map<STRING, STACK_FRAME>::iterator res = pLocals->find(name);

	if (res != pLocals->end())
	{
		if (pFrame) *pFrame = pLocalList->size();
		return &res->second;
	}
	return m_heap[name];
}

// Prefer the local scope when resolving a variable.
LPSTACK_FRAME CProgram::resolveVarLocal(const STRING &name, unsigned int *pFrame)
{
	std::map<STRING, CPtrData<STACK_FRAME> >::iterator i = m_heap.find(name);
	if (i != m_heap.end())
	{
		return i->second;
	}
	std::list<std::map<STRING, STACK_FRAME> > *pLocals = getLocals();
	if (pFrame) *pFrame = pLocals->size();
	return &pLocals->back()[name];
}

// Remove a redirect from the list.
void CProgram::removeRedirect(CONST STRING str)
{
	std::map<STRING, STRING>::iterator i = m_redirects.begin();
	for (; i != m_redirects.end(); ++i)
	{
		if (i->first == str)
		{
			m_redirects.erase(i);
			return;
		}
	}
}

// Get a function's name.
STRING CProgram::getFunctionName(const MACHINE_FUNC func)
{
	std::map<STRING, MACHINE_FUNC>::const_iterator i = m_functions.begin();
	for (; i != m_functions.end(); ++i)
	{
		if (i->second == func) break;
	}
	return ((i != m_functions.end()) ? i->first : STRING());
}

// Add a function to the global namespace.
void CProgram::addFunction(const STRING &name, const MACHINE_FUNC func)
{
	TCHAR *const str = _tcslwr(_tcsdup(name.c_str()));
	m_functions.insert(std::map<STRING, MACHINE_FUNC>::value_type(str, func));
	free(str);
}

// Free a variable.
void CProgram::freeVar(const STRING &var)
{
	std::map<STRING, STACK_FRAME> *pLocals = &getLocals()->back();
	if (pLocals->erase(var))
	{
		return;
	}
	m_heap.erase(var);
}

// Free an object.
void CProgram::freeObject(unsigned int obj)
{
	std::map<unsigned int, STRING>::const_iterator res = m_objects.find(obj);
	assert(res != m_objects.end() && m_classes.find(res->second) != m_classes.end());
	if (res != m_objects.end())
	{
		TCHAR str[20];
		_itot_s(obj, str, 20, 10);

		//` Remove any heap vars that are in the expected format. Doesn't explicitly check the members
		//  list, but that doesn't appear to be needed. This way, array elements will get destroyed too,
		//  which are typically going to be the bulk of heavier objects.
		STRING prefix = STRING(str) + _T("::");
		std::map<STRING, CPtrData<STACK_FRAME> >::const_iterator it = m_heap.lower_bound(prefix);
		while (it != m_heap.end() && it->first.substr(0, prefix.length()) == prefix)
		{
			m_heap.erase(it++); //<- Increments before invalidating iterator.
		}

		m_objects.erase(obj);
	}
}

// Handle a method call.
void CProgram::methodCall(CALL_DATA &call)
{
	std::map<STRING, STACK_FRAME> local;

	CALL_FRAME fr;
	fr.obj = 0;

	// Will be > 0 because the first line is at least a skipMethod.
	fr.errorReturn = 0;

	// Last parameter is the UDT_UNSET (terminator)
	// Second to the last is the method/function call (type UDT_ID and UDT_OBJ)
	STACK_FRAME &fra = call[call.params - 1];

	bool bNoRet = false;

	// Look at the num member as though it were two longs.
	const double metadata = fra.num;
	long *const pLong = (long *)&metadata;

	bool bObjectCall = false;

	if (fra.udt & UDT_OBJ)
	{
		// Call to a class function.

		// Find the parameter containing the object.
		LPSTACK_FRAME objp = &call[0];

		//` CHECKME: objp is now fixed in position at call[0], across both methods and constructors.
		//  Needs thorough testing - Could have some unforeseen consequences (construction order?)

		unsigned int obj = static_cast<unsigned int>(objp->getNum());
		std::map<unsigned int, STRING>::const_iterator obj_it = CProgram::m_objects.find(obj);
		if ((~objp->getType() & UDT_OBJ) || (~objp->getType() & UDT_NUM) || obj_it == CProgram::m_objects.end())
		{
			throw CError(objp->lit + _T(" is an invalid object."));
		}

		const STRING &type = obj_it->second;
		std::map<STRING, CLASS>::iterator cls_it = call.prg->m_classes.find(type);
		if (cls_it == call.prg->m_classes.end())
		{
			throw CError(_T("Could not find class ") + type + _T("."));
		}

		bool bRelease = false;
		if (fra.lit == _T("release"))
		{
			bRelease = true;
			fra.lit = _T("~") + type;
		}

		assert(call.prg->m_calls.empty() || call.prg->m_calls.back().obj == 0 || 
			CProgram::m_objects.count(call.prg->m_calls.back().obj));

		const CLASS_VISIBILITY cv = (call.prg->m_calls.size() && 
			(CProgram::m_objects[call.prg->m_calls.back().obj] == type)) ? CV_PRIVATE : CV_PUBLIC;
		LPNAMED_METHOD p = cls_it->second.locate(fra.lit, call.params - 2, cv);
		if (!p)
		{
			if (!bRelease)
			{
				STRINGSTREAM ss;
				ss	<< _T("Class ") << cls_it->first << _T(" has no accessible ") << fra.lit
					<< _T(" method with a parameter count of ") << (call.params - 2) << _T(".");
				throw CError(ss.str());
			}
			else
			{
				call.prg->freeObject(obj);	//` freeObject was only being called when no user-defined
											//  d-tor was available. See releaseObj for possible fix.
				return;
			}
		}

		if (type == fra.lit)
		{
			call.prg->m_stack[call.prg->m_stackIndex].back() = objp->getValue();
			bNoRet = true;
		}

		pLong[0] = p->i;
		pLong[1] = p->byref;

		STACK_FRAME &lvar = local[_T("this")];
		lvar.udt = UNIT_DATA_TYPE(UDT_OBJ | UDT_NUM);
		lvar.num = fr.obj = obj;

		// The parameters are offset because of the object pointer.
		bObjectCall = true;
#ifndef ENABLE_MUMU_DBG
	}
#else
		if (m_enableMumu)
		{
			fr.methodIdx = call.prg->findMethod(type + _T("::") + fra.lit, call.params - 2);

			if (fr.methodIdx == -1)
			{
				// Not found, so search inherited classes
				const CLASS &cls = cls_it->second;
				for (std::deque<STRING>::const_iterator inherits_it = cls.inherits.begin();
					fr.methodIdx == -1 && inherits_it != cls.inherits.end();
					++inherits_it)
				{
					fr.methodIdx = call.prg->findMethod(*inherits_it + _T("::") + fra.lit, call.params - 2);
				}
			}
		}
	}
	else if (m_enableMumu)
	{
		fr.methodIdx = call.prg->findMethod(fra.lit, call.params - 1);
	}
	assert(!m_enableMumu || fr.methodIdx != -1);
#endif

	//`(snip) Implicit "this->" now handled at compile-time; See parseFile().
	// Could be improved by precomputing some stuff ahead of time (method location, etc.)

	// Add each parameter's value to the new local heap, excluding the last parameter (descriptor) and,
	// for object calls, the first parameter (objp).
	for (unsigned int i = bObjectCall; i < (call.params - 1); ++i)
	{
		TCHAR pos = call.params - i - 1;

		if (pLong[1] & (1 << (pos - 1)))
		{
			STRING var = call[i].lit;
			if (var[0] == ' ')
			{
				REFERENCE_MAP &res = call.prg->m_calls.back().refs;
				REFERENCE_MAP::iterator j = res.find((unsigned int)var[1]);
				if (j != res.end())
				{
					fr.refs.insert(*j);
					continue;
				}
			}
			unsigned int frame = 0;
			LPSTACK_FRAME pStackFrame = fra.prg->getVar(var, &frame, &var);
			fr.refs[pos] = REFERENCE(pStackFrame, REFERENCE_DESC(frame, var));
		}
		else
		{
			local[STRING(_T(" ")) + pos] = call[i].getValue();
		}
	}

	// Make sure this method has actually been resolved.
	if (metadata == -1)
	{
		STRING errorText = _T("Could not find method ") + fra.lit + _T(".");
		if (call.prg->findMethod(fra.lit) != -1)
		{
			errorText += _T(" Did you forget a parameter?");
		}
		throw CError(errorText);
	}

	// Push a new local heap onto the stack of heaps for this method.
	call.prg->getLocals()->push_back(local);

	// Record the current position in the program.
	fr.i = call.prg->m_i - call.prg->m_units.begin();

	// Push a new stack onto the stack of stacks for this method.
	// It should be a copy of the stack being used at the time of
	// this call so that the remainder of tagMachineUnit::execute()
	// is able to pop off the parameters for this method without
	// crashing.

	//` Intended:
	//std::vector<STACK_FRAME> tmp = call.prg->m_stack.back();
	//call.prg->m_stack.push_back(tmp);

	//` CHECKME
	//` Since params will simply be erased following methodCall(),
	//` just fill with dummy values (for a minor speed boost)
	call.prg->m_stack.push_back(
		std::vector<STACK_FRAME>(call.prg->m_stack.back().size(), STACK_FRAME(call.prg)));

	// Pop the parameters of this method off the current stack, as
	// they will not be popped off by tagMachineUnit::execute().
	call.prg->m_stack[call.prg->m_stackIndex].erase(
		call.prg->m_stack[call.prg->m_stackIndex].end() - call.params - 1, 
		call.prg->m_stack[call.prg->m_stackIndex].end() - 1);

	if (bNoRet)
	{
		fr.bReturn = true;
	}
	else if (call.prg->m_i->udt & UDT_LINE)
	{
		// This call is the last thing on a line, so its
		// value is not being used. Do not bother setting
		// up a return value.
		fr.bReturn = false;
		fr.p = NULL;
	}
	else
	{
		// Set the return pointer to the stack frame
		// that this call would use to return.
		fr.bReturn = true;
		fr.p = NULL;
		fr.p = &call.prg->m_stack[call.prg->m_stackIndex].back();
		fr.p->udt = UNIT_DATA_TYPE(UDT_NUM | UDT_UNSET);
		fr.p->num = 0.0;
	}

	// Set the stack pointer to this method's stack.
	call.prg->m_stackIndex = call.prg->m_stack.size() - 1;
	assert(call.prg->m_stackIndex > 0);

	// Jump to the first line of this method.
	assert(pLong[0] < call.prg->m_units.size());
	call.prg->m_i = call.prg->m_units.begin() + pLong[0];

	// Record the last line of this method.
	fr.j = static_cast<unsigned int>(call.prg->m_i->num);

	// Push this call onto the call stack.
	call.prg->m_calls.push_back(fr);
}

// Handle a plugin call.
void CProgram::pluginCall(CALL_DATA &call)
{
	extern CProgram *g_prg;

	// Get the plugin.
	STACK_FRAME &fra = call[call.params - 1];
	IPlugin *pPlugin = m_plugins[static_cast<unsigned int>(fra.num)];

	// Prepare the command line.
	STRING line = fra.lit + _T('(');
	for (unsigned int i = 0; i < (call.params - 1); ++i)
	{
		STACK_FRAME &param = call[i];
		if (param.udt & UDT_NUM)
		{
			line += param.getLit();
		}
		else if (param.udt & UDT_LIT)
		{
			line += _T('"') + param.lit + _T('"');
		}
		else if (param.udt & UDT_ID)
		{
			line += param.lit + ((param.getType() & UDT_NUM) ? _T('!') : _T('$'));
		}
		if (i != call.params - 2)
		{
			line += _T(',');
		}
	}
	line += _T(')');

	// Call the function.
	CProgram *const prg = g_prg;
	g_prg = call.prg;
	int dt = PLUG_DT_VOID; STRING lit; double num = 0.0;
	pPlugin->execute(line, dt, lit, num, !(call.prg->m_i->udt & UDT_LINE));
	g_prg = prg;

	if (dt == PLUG_DT_NUM)
	{
		call.ret().udt = UDT_NUM;
		call.ret().num = num;
	}
	else if (dt == PLUG_DT_LIT)
	{
		call.ret().udt = UDT_LIT;
		call.ret().lit = lit;
	}
}

inline void CProgram::returnFromMethod(const STACK_FRAME &value)
{
	if (!m_calls.size()) return;
	LPSTACK_FRAME pValue = m_calls.back().p;
	if (pValue)
	{
		*pValue = value;
	}
	m_i = m_units.begin() + m_calls.back().j - 1;
}

void CProgram::returnVal(CALL_DATA &call)
{
	call.prg->returnFromMethod(call[0].getValue());
}

// Pretty much the same as returnValue(), except the return value
// is the parameter, not the value of the parameter.
void CProgram::returnReference(CALL_DATA &call)
{
	// TBD: Do this at compile-time!
	const std::pair<bool, STRING> res = call.prg->getInstanceVar(call[0].lit);
	if (res.first)
	{
		call[0].lit = _T(':') + res.second;
	}
	call.prg->returnFromMethod(call[0]);
}

// Open an RPGCode program.
bool CProgram::open(const STRING fileName)
{
	// Attempt to locate this program in the cache.
	CACHE_ITR itr = g_cache.find(fileName);
	if (itr != g_cache.end())
	{
		*this = itr->second;
		prime();
		return true;
	}

	FILE *file = fopen(resolve(fileName).c_str(), _T("rb"));
	if (!file) return false;

	m_fileName = fileName;

	// Get the length of the file.
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	if (length == 0)
	{
		// It is unlikely that the file is completely blank,
		// but this avoids a crash in case it is.
		fclose(file);
		prime();
		g_cache[fileName] = *this;
		return true;
	}

	const STRING parsing = m_parsing;
	m_parsing = fileName;
	fseek(file, 0, SEEK_SET);

	// Programs starting with include, redirect and possibly
	// other things crash. As a quick solution, we add "1",
	// a line that does nothing, to the start of each file.

	char *const str = (char *const)malloc(sizeof(char) * (length + 3));
	fread(str + 2, sizeof(char), length, file);

	str[0] = '1';		// Arbitrary first line.
	str[1] = '\n';

	// Add a blank line to the end of the file to prevent
	// an error from occurring when the file has no final
	// blank line.
	str[length + 2] = '\n';

	fclose(file);					// Close the original file...
	file = createTemporaryFile();	// ...and create another.

	// Write the updated version to our temp file.
	fwrite(str, sizeof(char), length + 3, file);
	free(str);

	// And parse the file.
	fseek(file, 0, SEEK_SET);
	parseFile(file);
	m_parsing = parsing;

	fclose(file);

	prime();

	// Store this program in the cache.
	g_cache[fileName] = *this;

	return true;
}

// Prime the program.
void CProgram::prime()
{
	m_stack.clear();
	m_stack.push_back(std::vector<STACK_FRAME>());
	m_stackIndex = 0;
	m_calls.clear();
	m_locals.clear();
	m_locals.push_back(std::map<STRING, STACK_FRAME>());
	m_i = m_units.begin();
	// Prefer the global scope when resolving a variable by default.
	m_pResolveFunc = &CProgram::resolveVarGlobal;
}

// Load the program from a string.
bool CProgram::loadFromString(const STRING &str)
{
	// tmpfile() is capped to TMP_MAX calls. tmpnam() is slow.
    char tempfile[] = _T("_tmpprg");
 	  	 
    FILE *p = fopen(tempfile, _T("wb+"));

	if (!p) return false;

	fputs(str.c_str(), p);
	fseek(p, 0, SEEK_SET);
	parseFile(p);
	fclose(p);

	remove(tempfile);
	prime();
	return true;
}

// Parse a file.
void CProgram::parseFile(FILE *pFile)
{
	// Step 1:
	//   - Run the file though the YACC generated parser,
	//     producing machine units. See yacc.txt.
	extern void resetState();
	resetState();
	m_inclusions.clear();
	m_units.clear();
	m_methods.clear();
	m_classes.clear();
	m_pClasses = &m_classes;
	m_pyyUnits = &m_units;
	yyin = pFile;
	g_lines = 0;
	m_lines.clear();
	m_pLines = &m_lines;
	m_pInclusions = &m_inclusions;
	NAMED_METHOD::m_methods.clear();

	// We are parsing a new file.
	YY_NEW_FILE;
	yyparse();

#ifdef ENABLE_MUMU_DBG
	g_lines = 0;
	g_mumuProgramIdx = -1;
#endif

	m_methods = NAMED_METHOD::m_methods;
	m_yyFors.clear();

	// Step 2:
	//   - Include requested files.
	{
		std::vector<STRING> inclusions = m_inclusions;
		std::vector<STRING>::const_iterator i = inclusions.begin();

		// If this is a child program, include its parent.
		// dynamic_cast<> returns null if the cast is unsafe.
		CProgramChild *pChild = dynamic_cast<CProgramChild *>(this);
		if (pChild)
		{
			include(pChild->getParent());
		}

		extern STRING g_projectPath;
		for (; i != inclusions.end(); ++i)
		{
			include(CProgram(g_projectPath + PRG_PATH + *i));
		}
	}

	// Step 3:
	//   - TBD: Handle member references within class methods.
	//   - Record class members.
	//   - Detect class factory references.
	//   - Backward compatibility: "end" => "end()"
	for (std::map<STRING, CLASS>::iterator j = m_classes.begin(); j != m_classes.end(); ++j)
	{
		std::deque<STRING> immediate = j->second.inherits;
		for (std::deque<STRING>::iterator k = immediate.begin(); k != immediate.end(); ++k)
		{
			if (!m_classes.count(*k))
			{
				debugger(_T("Could not find ") + j->first + _T("'s base class ") + *k + _T("."));
				if ((k = immediate.erase(k)) == immediate.end()) break;
				--k;
			}
			else
			{
				j->second.inherit(m_classes[*k]);
			}
		}
	}

	LPCLASS pClass = NULL;
	CLASS_VISIBILITY vis = CV_PRIVATE;
	STRING methodName;

	int depth = 0, classDepth = -1, methodDepth = -1;

	for (POS i = m_units.begin(); i != m_units.end(); ++i)
	{
		if (i->udt & UDT_OPEN)
		{
			++depth;
			POS previous = i - 1;

			if (previous->udt & UDT_FUNC)
			{
				if (previous->func == skipClass)
				{
					classDepth = depth;
					pClass = &m_classes[previous->lit];
					vis = CLASS_VISIBILITY(int(previous->num));
				}
				else if (previous->func == skipMethod)
				{
					methodDepth = depth;
					methodName = previous->lit;
					assert(findMethod(methodName) != -1);
				}
			}
		}
		else if (i->udt & UDT_CLOSE)
		{
			--depth;
			if (depth < classDepth)
			{
				classDepth = -1;
				pClass = NULL;
			}
			if (depth < methodDepth)
			{
				methodDepth = -1;
				methodName.clear();
			}
		}

		if ((i->udt & UDT_ID) && (i->udt & UDT_LINE) && ((i == m_units.begin()) || ((i - 1)->udt & UDT_LINE)) && 
			((i == m_units.end()) || ((i + 1)->udt & UDT_LINE)))
		{
			if (depth == classDepth)
			{
				if (i->udt & UDT_NUM)
				{
					// Visibility specifier.
					vis = CLASS_VISIBILITY(int(i->num));
				}
				else
				{
					// Historical member declaration.
					pClass->members.push_back(std::pair<STRING, CLASS_VISIBILITY>(i->lit, vis));
				}
				i = m_units.erase(i) - 1;
			}
			else
			{
				// Convert such lines as "end" to "end()". This could
				// potentially do unwanted things, but it is required
				// for backward compatibility.
				if (m_functions.count(i->lit))
				{
					i->params = 0;
					i->func = m_functions[i->lit];
				}
				else
				{
					MACHINE_UNIT mu;
					mu.udt = UDT_ID;
					mu.lit = i->lit;
					mu.num = -1;
#ifdef ENABLE_MUMU_DBG
					mu.line = i->line - 1;
					mu.fileIndex = i->fileIndex;
#endif
					i->params = 1;
					i->func = methodCall;
					i = m_units.insert(i, mu) + 1;
				}
				i->udt = UNIT_DATA_TYPE(UDT_FUNC | UDT_LINE);
			}
		}

		if (!(i->udt & UDT_FUNC)) continue;

		if (i->func == methodCall)
		{
			POS previous = i - 1;
			if (previous->udt & UDT_OBJ) continue;
			if (m_classes.count(previous->lit))
			{
				LPCLASS pCls = &m_classes[previous->lit];
				LPNAMED_METHOD pCtor = pCls->locate(previous->lit, i->params - 1, 
					(pCls == pClass) ? CV_PRIVATE : CV_PUBLIC);
				if ((i->params == 1) || pCtor)
				{
					i->func = classFactory;
					if (pCtor)
					{
						// A user-defined constructor is available, so we need to set up a methodCall MU
						// for it, which will adopt the old parameters plus the classFactory MU (objp).
						//`Experimental: For consistency across all call types, objp is now relocated to
						// the head of the parameter list.

						int paramCount = i->params;
						MACHINE_UNIT descriptor = *previous;
						MACHINE_UNIT objp = *i;
						objp.params = 1; //<- classFactory has 1 parameter

						// Insert the new methodCall and its descriptor:
						{
							MACHINE_UNIT mu;
							mu.udt = UNIT_DATA_TYPE(UDT_ID | UDT_OBJ);
							mu.lit = previous->lit;
#ifdef ENABLE_MUMU_DBG
							mu.line = i->line;
							mu.fileIndex = i->fileIndex;
#endif
							i = m_units.insert(i + 1, mu) - 1;
						}
						{
							MACHINE_UNIT mu;
							mu.udt = UDT_FUNC;
							mu.func = methodCall;
							mu.params = paramCount + 1; //<- Extra parameter (objp)
#ifdef ENABLE_MUMU_DBG
							mu.line = i->line;
							mu.fileIndex = i->fileIndex;
#endif
							i = m_units.insert(i + 2, mu) - 2;
						}

						// Insert NEW classFactory and its descriptor ahead of the other parameters:
						int k = gotoInsertionPoint(i, m_units);
						i = m_units.insert(i, objp);
						i = m_units.insert(i, descriptor) + (k + 2); //<- Jump back to OLD classFactory
						assert(i->func == classFactory);

						// Now erase the OLD classFactory and its descriptor:
						i = m_units.erase(i - 1, i + 1) + 1;
						assert(i->func == methodCall);
					}
				}
				else
				{
					STRINGSTREAM ss;
					ss	<< _T("Near line ") << getLine(i) << _T(": No accessible constructor for ")
						<< previous->lit << _T(" has a parameter count of ") << (i->params - 1) << _T(".");
					debugger(ss.str());
				}
			}
			else
			{
				//`Expand any implicit "this->" method references. (CHECKME)
				// Note: Introduces a subtle inconsistency vs explicit this, where a base class
				// method calls an inherited method which doesn't also exist in the base class.
				if (methodName.length())
				{
					STRING::size_type pos = methodName.find(_T("::"));
					if (pos != STRING::npos)
					{
						STRING className = methodName.substr(0, pos);
						std::map<STRING, CLASS>::iterator res = m_classes.find(className);
						assert(res != m_classes.end() && !(previous->udt & UDT_OBJ));

						if (res != m_classes.end() &&
							res->second.locate(previous->lit, i->params - 1, CV_PRIVATE))
						{
							// Method exists in caller class, so insert "this->"
							MACHINE_UNIT mu;
							mu.udt = UDT_ID;
							mu.lit = _T("this");
#ifdef ENABLE_MUMU_DBG
							mu.line = i->line;
							mu.fileIndex = i->fileIndex;
#endif

							int k = gotoInsertionPoint(i, m_units);
							previous->udt = UNIT_DATA_TYPE(UDT_ID | UDT_OBJ);
							i = m_units.insert(i, mu) + (k + 1); //<- Jump back to starting point
							assert(i->func == methodCall);
							i->params++;
						}
					}
				}
			}
		}
		else if ((i->func == operators::array) && (depth == classDepth))
		{
			POS unit = i - 2;
			pClass->members.push_back(std::pair<STRING, CLASS_VISIBILITY>(unit->lit, vis));
		}

	}

	// Update curly brace pairing and method locations.
	if (depth = updateLocations(m_units.begin()))
	{
		MACHINE_UNIT mu;
		mu.udt = UNIT_DATA_TYPE(UDT_CLOSE | UDT_LINE);
		for (unsigned int i = 0; i < depth; ++i)
		{
			TCHAR str[255];
			_itot(getLine(m_units.begin() + matchBrace(m_units.insert(m_units.end(), mu))) + 1, str, 10);
			debugger(m_fileName + STRING(_T("\nNear line ")) + str + _T(": Unmatched curly brace."));
		}
	}

#ifdef ENABLE_MUMU_DBG
	// Post-process to fix broken line numbers (TODO: Fix in grammar.)
	for (POS it = m_units.begin(); it != m_units.end(); ++it)
	{
		if (!(it->udt & UDT_LINE))
		{
			int line = it->line;
			for (  ; it != m_units.end(); ++it)
			{
				it->line = line;
				if (it->udt & UDT_LINE)
					break;
			}
			if (it == m_units.end())
				break;
		}
	}
#endif

	// Inline requested methods.
	if (COptimiser(*this).inlineExpand())
	{
		// Some methods were inlined, so we need to update locations.
		updateLocations(m_units.begin());
	}

	// Resolve function calls.
	resolveFunctions();
}

// Match all curly braces and update method locations.
unsigned int CProgram::updateLocations(POS i)
{
	unsigned int depth = 0;
	for (; i != m_units.end(); ++i)
	{
		if (i->udt & UDT_FUNC)
		{
			if (i->func == skipMethod)
			{
				LPNAMED_METHOD p = NAMED_METHOD::locate(i->lit,
					static_cast<unsigned int>(i->num), false, *this);
				if (p)
				{
					p->i = i - m_units.begin() + 1;
				}
				//` (snip) class methods are now updated after this loop
			}
			else if (i->func == methodCall)
			{
				POS unit = i - 1;
				if (unit->udt & UDT_OBJ) continue;
				unit->num = -1;
			}
		}
		else if (i->udt & UDT_OPEN)
		{
			++depth;
		}
		else if (i->udt & UDT_CLOSE)
		{
			--depth;
			matchBrace(i);
		}
	}

	//` forward locations from m_methods to each class method, taking inheritance order into account
	//` checkme: ensure inherits are always stored in proper order
	for (std::map<STRING, tagClass>::iterator classIter = m_classes.begin();
		classIter != m_classes.end(); ++classIter)
	{
		for (ClassMethods::iterator methodIter = classIter->second.methods.begin();
			methodIter != classIter->second.methods.end(); ++methodIter)
		{
			bool success = false;
			LPNAMED_METHOD method = NAMED_METHOD::locate(classIter->first + "::" + methodIter->first.name,
				methodIter->first.params, false, *this);

			if (method)
			{
				methodIter->first.i = method->i;
				continue;
			}
			else
			{
				//` not in immediate class, so check inherited classes in order, breaking at first match
				for (std::deque<STRING>::const_iterator inheritsIter = classIter->second.inherits.begin();
					inheritsIter != classIter->second.inherits.end(); ++inheritsIter)
				{
					method = NAMED_METHOD::locate(*inheritsIter + "::" + methodIter->first.name,
						methodIter->first.params, false, *this);

					if (method)
					{
						methodIter->first.i = method->i;
						success = true;
						break;
					}
				}
			}

			if (!success)
			{
				//`todo: resolve gracefully; relink to empty body?
				STRING errorText = "[CProgram::updateLocations] Could not locate " +
					classIter->first + "::" + methodIter->first.name;		
				CProgram::debugger(errorText);	
				throw CError(errorText);
			}
		}
	}

	return depth;
}

// Resolve all currently unresolved functions.
void CProgram::resolveFunctions()
{
	for (POS i = m_units.begin(); i != m_units.end(); ++i)
	{
		if ((i->udt & UDT_FUNC) && (i->func == methodCall))
		{
			POS unit = i - 1;
			if ((unit->udt & UDT_OBJ) || (unit->num != -1)) continue;
			LPNAMED_METHOD p = NAMED_METHOD::locate(unit->lit, i->params - 1, false, *this);
			if (p)
			{
				unit->udt = UDT_NUM;
				long *pLong = (long *)&unit->num;
				pLong[0] = p->i;
				pLong[1] = p->byref;
			}
			else if (resolvePluginCall(&*unit))
			{
				// Found it in a plugin.
				i->func = pluginCall;
			}
		}
	}
}

// Resolve a plugin call.
bool CProgram::resolvePluginCall(LPMACHINE_UNIT pUnit)
{
	// Get lowercase name.
	TCHAR *const lwr = _tcslwr(_tcsdup(pUnit->lit.c_str()));
	const STRING name = lwr;
	free(lwr);

	// Query plugins.
	std::vector<IPlugin *>::iterator i = m_plugins.begin();
	for (; i != m_plugins.end(); ++i)
	{
		if ((*i)->plugType(PT_RPGCODE) && (*i)->query(name))
		{
			// Refer to the plugin by its index in the list
			// of plugins.
			pUnit->udt = UDT_PLUGIN;
			pUnit->num = i - m_plugins.begin();
			pUnit->lit = name;
			return true;
		}
	}

	// It wasn't a plugin call.
	return false;
}

// Match a curly brace pair.
unsigned int CProgram::matchBrace(POS i)
{
	POS cur = i;
	int depth = 0;
	for (; i != m_units.begin(); --i)
	{
		if ((i->udt & UDT_OPEN) && (++depth == 0))
		{
			i->num = cur - m_units.begin();
			unsigned long *const pLines = (unsigned long *)&cur->num;
			pLines[0] = i - m_units.begin();
			for (; i != m_units.begin(); --i)
			{
				if ((i->udt & UDT_LINE) && (++depth == 3)) break;
			}
			pLines[1] = i - m_units.begin() + 1;
			if (i == m_units.begin()) --pLines[1];
			return pLines[0];
		}
		else if (i->udt & UDT_CLOSE) --depth;
	}
	return 0;
}

// Include a file.
void CProgram::include(const CProgram prg)
{
	{
		std::map<STRING, tagClass>::const_iterator i = prg.m_classes.begin();
		for (; i != prg.m_classes.end(); ++i)
		{
			m_classes.insert(*i);
		}
	}

	std::vector<NAMED_METHOD>::const_iterator i = prg.m_methods.begin();
	for (; i != prg.m_methods.end(); ++i)
	{
		if (NAMED_METHOD::locate(i->name, i->params, false, *this))
		{
			// Duplicate method.
			/**if (m_debugLevel >= E_ERROR)
			{
				debugger(_T("Included file contains method that is already defined: ") + i->name + _T("()"));
			}**/
			continue;
		}

		if (i->i >= prg.m_units.size())
		{
			CProgram::debugger("CHECKME bad method location: " + i->name + "()\n in " + prg.m_fileName + "\n");
			continue;
		}

		m_methods.push_back(*i);
		int depth = 0;

		CONST_POS j = prg.m_units.begin() + i->i - 1;
		do
		{
			m_units.push_back(*j);
			if (j->udt & UDT_OPEN) ++depth;
			else if ((j->udt & UDT_CLOSE) && !--depth) break;
		} while (++j != prg.m_units.end());
	}
}

// Run a program file if it exists, otherwise treat as inline RPGCode
// - Checks for empty string
void CProgram::verifyAndRun(const STRING &text)
{
	extern STRING g_projectPath;

	if (!text.empty())
	{
		if (CFile::fileExists(g_projectPath + PRG_PATH + text))
		{
			CProgram(g_projectPath + PRG_PATH + text).run();
		}
		else
		{
			if (text.length() > 4 && text.substr(text.length() - 4, 4) == ".prg")
				messageBox("Warning:\n\nInline RPGCode looks like a filename, but that file doesn't exist!\n: " + text);
			CProgram p;
			p.loadFromString(text);
			p.run();
		}
	}
}

// Serialise the current state.
void CProgram::serialiseState(CFile &stream) const
{
	// Write the index of the current stack frame.
	if (m_stackIndex < 0)
	{
		stream << -1;
		CProgram::debugger("[CProgram::serialiseState] Invalid stack frame!");
		return;
	}
	else
	{
		stream << m_stackIndex;
	}

	// Data stack.
	{
		stream << int(m_stack.size());	// Number of stack frames.
		STACK_ITR i = m_stack.begin();
		for (; i != m_stack.end(); ++i)
		{
			stream << int(i->size());	// Number of items in this frame.

			std::vector<STACK_FRAME>::const_iterator j = i->begin();
			for (; j != i->end(); ++j)
			{
				// Write each stack frame item.
				serialiseStackFrame(stream, *j);
			}
		}
	}

	// Local variables.
	{
		stream << int(m_locals.size());
		std::list<std::map<STRING, STACK_FRAME> >::const_iterator i = m_locals.begin();
		for (; i != m_locals.end(); ++i)
		{
			stream << int(i->size());

			std::map<STRING, STACK_FRAME>::const_iterator j = i->begin();
			for (; j != i->end(); ++j)
			{
				stream << j->first;
				serialiseStackFrame(stream, j->second);
			}
		}
	}

	// Call stack.
	{
		stream << int(m_calls.size());
		std::vector<CALL_FRAME>::const_iterator i = m_calls.begin();
		for (; i != m_calls.end(); ++i)
		{
			stream << int(i->bReturn ? 1 : 0);
			stream << i->i << i->j << i->obj;
			stream << int(i->refs.size());
			REFERENCE_MAP::const_iterator j = i->refs.begin();
			for (; j != i->refs.end(); ++j)
			{
				stream << j->first;
				const REFERENCE_DESC &r = j->second.second;
				stream << r.first << r.second;
			}
		}
	}

	// Program position.
	stream << int(m_i - m_units.begin());

	// Default scope resolution (can be changed by autolocal()).
	stream << int((m_pResolveFunc == &CProgram::resolveVarGlobal) ? 0 : 1);

	// List of inclusions.
	{
		stream << int(m_inclusions.size());
		std::vector<STRING>::const_iterator i = m_inclusions.begin();
		for (; i != m_inclusions.end(); ++i)
		{
			stream << *i;
		}
	}
}

// Reconstruct a previously serialised state.
void CProgram::reconstructState(CFile &stream)
{
	int stackIdx = -1;
	stream >> stackIdx;
	if (stackIdx < 0)
	{
		// The current stack frame is invalid.
		CProgram::debugger("[CProgram::reconstructState] Invalid stack frame!");
		return;
	}

	// Data stack.
	{
		m_stack.clear();

		int stackSize = 0;
		stream >> stackSize;
		for (unsigned int i = 0; i < stackSize; ++i)
		{
			m_stack.push_back(std::vector<STACK_FRAME>());

			int frameSize = 0;
			stream >> frameSize;

			std::vector<STACK_FRAME> &frame = m_stack.back();

			for (unsigned int j = 0; j < frameSize; ++j)
			{
				STACK_FRAME sf;
				reconstructStackFrame(stream, sf);
				sf.prg = this;
				frame.push_back(sf);
			}
		}
	}

	m_stackIndex = stackIdx;

	// Local variables.
	{
		m_locals.clear();

		int stackSize = 0;
		stream >> stackSize;
		for (unsigned int i = 0; i < stackSize; ++i)
		{
			m_locals.push_back(std::map<STRING, STACK_FRAME>());

			int frameSize = 0;
			stream >> frameSize;

			std::map<STRING, STACK_FRAME> &frame = m_locals.back();

			for (unsigned int j = 0; j < frameSize; ++j)
			{
				STRING str; STACK_FRAME sf;
				stream >> str;
				reconstructStackFrame(stream, sf);
				sf.prg = this;
				frame[str] = sf;
			}
		}
	}

	// Call stack.
	{
		m_calls.clear();

		int stackSize = 0;
		stream >> stackSize;
		for (unsigned int i = 0, j = stackSize + 1; i < stackSize; ++i, --j)
		{
			CALL_FRAME cf; int b = 0;
			stream >> b;
			cf.bReturn = bool(b);
			stream >> cf.i;
			stream >> cf.j;
			stream >> cf.obj;
			cf.p = &(m_stack.end() - j)->back();
			int count;
			stream >> count;
			{
				for (int j = 0; j < count; ++j)
				{
					unsigned int idx;
					stream >> idx;
					REFERENCE_DESC desc;
					stream >> desc.first;
					stream >> desc.second;
					cf.refs[idx] = REFERENCE(NULL, desc);
				}
			}

			{
				REFERENCE_MAP::iterator j = cf.refs.begin();
				for (; j != cf.refs.end(); ++j)
				{
					REFERENCE &ref = j->second;

					const unsigned int frame = ref.second.first;
					if (frame == 0)
					{
						ref.first = m_heap[ref.second.second];
						continue;
					}
					std::list<std::map<STRING, STACK_FRAME> >::iterator itr = getLocals()->begin();
					for (int i = 0; i < frame - 1; ++i) ++itr;
					ref.first = &itr->find(ref.second.second)->second;
				}
			}

			m_calls.push_back(cf);
		}
	}

	// Program position.
	{
		int ppos = 0;
		stream >> ppos;
		m_i = m_units.begin() + ppos;
	}

	// Default scope.
	{
		int scope = 0;
		stream >> scope;
		m_pResolveFunc = scope ? &CProgram::resolveVarLocal : &CProgram::resolveVarGlobal;
	}

	// List of inclusions.
	{
		m_inclusions.clear();
		int includes = 0;
		stream >> includes;
		for (int i = 0; i < includes; ++i)
		{
			STRING str;
			stream >> str;
			m_inclusions.push_back(str);
		}
	}
}

int CProgram::findMethod(const STRING &name, int params)
{
	//` todo: pre-sort & use std; note m_methods is modified by runtime inclusions
	for (std::vector<NAMED_METHOD>::size_type i = 0; i != m_methods.size(); ++i)
	{
		if (params != -1 && m_methods[i].params != params)
			continue;
		if (m_methods[i].name == name)
			return i;
	}
	return -1;
}

NAMED_METHOD& CProgram::getMethod(int idx)
{
	assert(idx >= 0 && idx < m_methods.size());
	return m_methods[idx];
}

// Run an RPGCode program.
STACK_FRAME CProgram::run()
{
	extern void programInit(), programFinish();

	if (!isReady())
		return STACK_FRAME();

	++m_runningPrograms;
	programInit();

#ifdef ENABLE_MUMU_DBG
	if (m_enableMumu)
	{
		CMumuDebugger mumu(*this);
		
		for (m_i = m_units.begin(); m_i != m_units.end(); ++m_i)
		{
			mumu.update(m_i);
			m_i->execute(this);
			processEvent();
		}
	}
	else
	{
		for (m_i = m_units.begin(); m_i != m_units.end(); ++m_i)
		{
			m_i->execute(this);
			processEvent();
		}
	}
#else
	for (m_i = m_units.begin(); m_i != m_units.end(); ++m_i)
	{
		m_i->execute(this);
		processEvent();
	}
#endif

	programFinish();
	--m_runningPrograms;

	// Obtain the final return value.
	const STACK_FRAME ret = 
		m_stack[m_stackIndex].size() ? m_stack[m_stackIndex].back() : STACK_FRAME();

	// Clear the stack.
	m_stack[m_stackIndex].clear();

	// Return the last value.
	return ret;
}

// Jump to a label.
bool CProgram::jump(const STRING label)
{
	CONST_POS i = m_units.begin();
	for (; i != m_units.end(); ++i)
	{
		if ((i->udt & UDT_LINE) && (i->udt & UDT_LABEL) && (_tcsicmp(i->lit.c_str(), label.c_str()) == 0))
		{
			m_i = i;
			return true;
		}
	}
	return false;
}

// Return from an error handler to the next statement
// after the one where the error occurred.
void CProgram::resumeFromErrorHandler()
{
	if (!m_calls.size())
	{
		throw CError("Invalid outside functions.");
	}
	unsigned int &err = m_calls.back().errorReturn;
	if (err == 0)
	{
		throw CError("An error handler has not been invoked.");
	}
	m_i = m_units.begin() + err;
	err = 0;
}

// Handle an error occurring at the current line.
void CProgram::handleError(CException *p)
{
	if (m_calls.size())
	{
		CALL_FRAME &frame = *&m_calls.back();
		STRING &handler = frame.errorHandler;
		if (!handler.empty())
		{
			// Note: a single space is a hard-coded convention
			//		 indicating "on error resume next".
			if (handler == " ")
			{
				// Hide the error.
				return;
			}

			// Find the beginning of the next statement.
			CONST_POS i;
			for (i = m_i; i != m_units.end(); ++i)
			{
				if (i->udt & UDT_LINE) break;
			}
			frame.errorReturn = i - m_units.begin();

			// Try to jump to the label specified.
			if (!jump(handler))
			{
				CError exp = _T("An error occurred, but the handler could not be invoked because label \"") + handler 
					+ _T("\" was not found.");
				handler = _T("");
				handleError(&exp);
				// Swallow the original error deliberately to avoid confusion.
			}

			return;
		}
	}

	if (p && (m_debugLevel < p->getType())) return;

	STRINGSTREAM ss;
	ss	<< m_fileName
		<< _T("\nNear line ")
		<< getLine(m_i)
		<< _T(": ")
		<< (p ? p->getMessage() : _T("Unexpected error."));
	debugger(ss.str());
}

// Set the label to jump to in case of error.
void CProgram::setErrorHandler(const STRING handler)
{
	if (m_calls.size() == 0)
	{
		throw CError("An error handler cannot be set outside of a function.");
	}
	m_calls.back().errorHandler = handler;
}

// If...else control structure.
void CProgram::conditional(CALL_DATA &call)
{
	if (call[0].getNum()) return;
	int i = (int)(call.prg->m_i + 1)->num;
	CONST_POS close = call.prg->m_units.begin() + i;
	if (close == call.prg->m_units.end() - 1)
	{
		call.prg->m_i = close;
		return;
	}
	if ((close != call.prg->m_units.end()) && ((close + 1)->udt & UDT_FUNC) && ((close + 1)->func == skipElse))
	{
		// Set the current unit to the else so that it is not
		// executed in CProgram::run(). Execution would cause
		// the else clause to be skipped.
		call.prg->m_i = close + 1;
	}
	else
	{
		call.prg->m_i = close;
	}
}

// Skip an else block.
void CProgram::skipElse(CALL_DATA &call)
{
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// Skip a method block.
void CProgram::skipMethod(CALL_DATA &call)
{
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// Skip a class block.
void CProgram::skipClass(CALL_DATA &call)
{
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// While loop.
void CProgram::whileLoop(CALL_DATA &call)
{
	if (call[0].getNum()) return;
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// Until loop.
void CProgram::untilLoop(CALL_DATA &call)
{
	if (!call[0].getNum()) return;
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// For loop.
void CProgram::forLoop(CALL_DATA &call)
{
	if (call[0].getNum()) return;
	call.prg->m_i = call.prg->m_units.begin() + (int)(call.prg->m_i + 1)->num;
}

// Create an object.
void CProgram::classFactory(CALL_DATA &call)
{
	const STRING &cls = call[0].lit;

	unsigned int obj = m_objects.size() + 1;
	while (m_objects.count(obj)) ++obj;
	m_objects.insert(std::map<unsigned int, STRING>::value_type(obj, cls));

	call.ret().udt = UNIT_DATA_TYPE(UDT_OBJ | UDT_NUM);
	call.ret().num = obj;
}

// Null Operation
void CProgram::nullOp(CALL_DATA &call)
{

}

void CProgram::releaseObj(CALL_DATA &call)
{
	assert(call.prg->m_calls.size() && call.prg->m_calls.back().obj);
	if (call.prg->m_calls.size() && call.prg->m_calls.back().obj)
		call.prg->freeObject(call.prg->m_calls.back().obj);
}


void CProgram::verifyType(CALL_DATA &call)
{
	const STRING &cls = call[1].lit;
	if (call.prg->m_classes.find(cls) == call.prg->m_classes.end())
	{
		throw CError("Could not find class referenced in parameter list: " + cls);
	}

	STACK_FRAME &frame = *call.prg->getVar(call[0].lit);
	if (!(frame.udt & UDT_OBJ))
	{
		throw CError("The method requires a parameter of type " + cls + ".");
	}

	const unsigned int obj = static_cast<unsigned int>(frame.num);
	std::map<unsigned int, STRING>::const_iterator obj_it = CProgram::m_objects.find(obj);
	if (obj_it == CProgram::m_objects.end())
	{
		throw CError("Invalid object.");
	}

	const STRING &type = obj_it->second;
	if (type == cls) return;

	assert(call.prg->m_classes.count(type));
	LPCLASS pClass = &call.prg->m_classes[type];

	std::deque<STRING>::const_iterator j = pClass->inherits.begin();
	for (; j != pClass->inherits.end(); ++j)
	{
		if (*j == cls) return;
	}

	throw CError("The method requires a parameter of type " + cls + ".");
}

void CProgram::runtimeInclusion(CALL_DATA &call)
{
	extern STRING g_projectPath;

	// Qualify the file name.
	const STRING file = g_projectPath + PRG_PATH + call[0].getLit();

	std::vector<STRING>::const_iterator i = call.prg->m_inclusions.begin();
	for (; i != call.prg->m_inclusions.end(); ++i)
	{
		if (*i == file)
		{
			// Silently fail for backward compatibility.
			return;
		}
	}

	CProgram inclusion;
	if (!inclusion.open(file))
	{
		throw CError(_T("Runtime inclusion: could not find ") + call[0].getLit() + _T("."));
	}

	// Add the file to the list of inclusions.
	call.prg->m_inclusions.push_back(file);

	// CProgram::include() will modify m_units, which will invalidate m_i,
	// so we save the value of m_i relative to m_units.begin() here.
	MACHINE_UNITS::difference_type pos = call.prg->m_i - call.prg->m_units.begin();
	MACHINE_UNITS::size_type size = call.prg->m_units.size();

	call.prg->include(inclusion);

	// Restore the position.
	call.prg->m_i = call.prg->m_units.begin() + pos;

	// And update references to the code that we just injected into the program.
	call.prg->updateLocations(call.prg->m_units.begin() + size);
	call.prg->resolveFunctions();
}

void CProgram::initialize()
{
	// Special.
	addFunction(_T(" null"), NULL);

	// Operators.
	addFunction(_T("+"), operators::add);
	addFunction(_T("-"), operators::sub);
	addFunction(_T("*"), operators::mul);
	addFunction(_T("|"), operators::bor);
	addFunction(_T("`"), operators::bxor);
	addFunction(_T("&"), operators::band);
	addFunction(_T("||"), operators::lor);
	addFunction(_T("&&"), operators::land);
	addFunction(_T("!="), operators::ieq);
	addFunction(_T("=="), operators::eq);
	addFunction(_T(">="), operators::gte);
	addFunction(_T("<="), operators::lte);
	addFunction(_T(">"), operators::gt);
	addFunction(_T("<"), operators::lt);
	addFunction(_T(">>"), operators::rs);
	addFunction(_T("<<"), operators::ls);
	addFunction(_T("%"), operators::mod);
	addFunction(_T("/"), operators::div);
	addFunction(_T("^"), operators::pow);
	addFunction(_T("="), operators::assign);
	addFunction(_T("`="), operators::xor_assign);
	addFunction(_T("|="), operators::or_assign);
	addFunction(_T("&="), operators::and_assign);
	addFunction(_T(">>="), operators::rs_assign);
	addFunction(_T("<<="), operators::ls_assign);
	addFunction(_T("-="), operators::sub_assign);
	addFunction(_T("+="), operators::add_assign);
	addFunction(_T("%="), operators::mod_assign);
	addFunction(_T("/="), operators::div_assign);
	addFunction(_T("*="), operators::mul_assign);
	addFunction(_T("^="), operators::pow_assign);
	addFunction(_T("||="), operators::lor_assign);
	addFunction(_T("&&="), operators::land_assign);
	addFunction(_T("[]"), operators::array);
	addFunction(_T("++i"), operators::prefixIncrement);
	addFunction(_T("i++"), operators::postfixIncrement);
	addFunction(_T("--i"), operators::prefixDecrement);
	addFunction(_T("i--"), operators::postfixDecrement);
	addFunction(_T("-i"), operators::unaryNegation);
	addFunction(_T("!"), operators::lnot);
	addFunction(_T("?:"), operators::tertiary);
	addFunction(_T("->"), operators::member);

	// Reserved.
	addFunction(_T("method a"), skipMethod);
	addFunction(_T("method b"), methodCall);
	addFunction(_T(" plugin"), pluginCall);
	addFunction(_T("class a"), skipClass);
	addFunction(_T("class b"), classFactory);
	addFunction(_T("if"), conditional);
	addFunction(_T("else"), skipElse);
	addFunction(_T("elseif"), elseIf);
	addFunction(_T("while"), whileLoop);
	addFunction(_T("until"), untilLoop);
	addFunction(_T("for"), forLoop);
	addFunction(_T(" rtinclude"), runtimeInclusion);
	addFunction(_T(" verifyType"), verifyType);
	addFunction(_T(" returnVal"), returnVal);
	addFunction(_T(" returnReference"), returnReference);
	addFunction(_T("null op"), nullOp);
	addFunction(_T(" releaseObj"), releaseObj);

	// Get the mutex for program execution.
	g_mutex = CGarbageCollector::getInstance().getMutex();
}

/*
 * *************************************************************************
 * CThread
 * *************************************************************************
 */

// Protected constructor.
CThread::CThread(const STRING str):
CProgram(),
m_bSleeping(false) 
{
	extern STRING g_projectPath;
	m_fileName = g_projectPath + PRG_PATH + str;
	if (CFile::fileExists(m_fileName))
	{
		open(m_fileName);
	}
	else
	{
		if (str.length() > 4 && str.substr(str.length() - 4, 4) == ".prg")
			messageBox("Warning:\n\nInline RPGCode looks like a filename, but that file doesn't exist!\n: " + str);
				
		m_fileName = str;
		loadFromString(str);
	}
}

// Create a thread.
CThread *CThread::create(const STRING str)
{
	CThread *p = new CThread(str);
	m_threads.insert(p);
	return p;
}

// Destroy a thread.
void CThread::destroy(CThread *p)
{
	std::set<CThread *>::iterator i = m_threads.find(p);
	if (i != m_threads.end())
	{
		m_threads.erase(i);
		delete p;
	}
}

// Destroy all threads.
void CThread::destroyAll()
{
	std::set<CThread *>::iterator i = m_threads.begin();
	for (; i != m_threads.end(); ++i)
	{
		delete *i;
	}
	m_threads.clear();
}

// Multitask now.
void CThread::multitask(const unsigned int units)
{
	std::set<CThread *>::iterator i = m_threads.begin();
	for (; i != m_threads.end(); ++i)
	{
		(*i)->execute(units);
	}
}

// Put a thread to sleep.
// 0 milliseconds = indefinite sleep
void CThread::sleep(const unsigned long milliseconds)
{
	m_bSleeping = true;
	m_sleepDuration = milliseconds;
	m_sleepBegin = GetTickCount();
}

// Is a thread sleeping?
bool CThread::isSleeping() const
{
	if (!m_bSleeping) return false;

	if (m_sleepDuration && (GetTickCount() - m_sleepBegin >= m_sleepDuration))
	{
		m_bSleeping = false;
		return false;
	}
	return true;
}

// Check how much sleep is remaining.
unsigned long CThread::sleepRemaining() const
{
	if (!isSleeping() || !m_sleepDuration) return 0;
	return (m_sleepDuration - (GetTickCount() - m_sleepBegin));
}

// Execute n units from a program.
bool CThread::execute(const unsigned int units)
{
	unsigned int i = 0;
	while ((m_i != m_units.end()) && (i++ < units) && !isSleeping())
	{
		m_i->execute(this);
		++m_i; 
	}
	return true;
}

/*
 * *************************************************************************
 * tagClass
 * *************************************************************************
 */

tagNamedMethod *tagClass::locate(const STRING &name, const int params, const CLASS_VISIBILITY vis)
{
	std::deque<std::pair<NAMED_METHOD, CLASS_VISIBILITY> >::iterator i = methods.begin();
	for (; i != methods.end(); ++i)
	{
		if ((i->second >= vis) && (i->first.name == name) && (i->first.params == params))
		{
			return &i->first;
		}
	}
	return NULL;
}

// Check whether the class has a member.
bool tagClass::memberExists(const STRING &name, const CLASS_VISIBILITY vis) const
{
	std::deque<std::pair<STRING, CLASS_VISIBILITY> >::const_iterator i = members.begin();
	for (; i != members.end(); ++i)
	{
		if ((i->second >= vis) && (i->first == name)) return true;
	}
	return false;
}

// Inherit a class.
void tagClass::inherit(const tagClass &cls)
{
	//` Inherit any member variables that don't already exist
	for (std::deque<std::pair<STRING, CLASS_VISIBILITY> >::const_iterator i = cls.members.begin();
		i != cls.members.end(); ++i)
	{
		if (!memberExists(i->first, CV_PRIVATE))
		{
			members.push_back(*i);
		}
	}

	//`Inherit any member functions that don't already exist
	for (std::deque<std::pair<NAMED_METHOD, CLASS_VISIBILITY> >::const_iterator j = cls.methods.begin();
		j != cls.methods.end(); ++j)
	{
		if (!locate(j->first.name, j->first.params, CV_PRIVATE))
		{
			methods.push_back(*j);
		}
	}

	//`Inherit any parent classes
	for (std::deque<STRING>::const_iterator k = cls.inherits.begin(); k != cls.inherits.end(); ++k)
	{
		inherits.push_back(*k);
	}
}

/*
 * *************************************************************************
 * tagMachineUnit
 * *************************************************************************
 */

// Show the contents of the instruction unit.
/*inline */void tagMachineUnit::show() const
{
	STRINGSTREAM ss;

	ss			<< "Lit: " << getAsciiString(lit)
				<< "\nNum: " << num
				<< "\nType: " << getUnitDataType(udt)
				<< "\nFunc: " << getAsciiString(CProgram::getFunctionName(func))
				<< "\nParams: " << params;
				//<< "\n\n";

	CProgram::debugger(ss.str());
}

// Execute an instruction unit.
void tagMachineUnit::execute(CProgram *prg) const
{
	EnterCriticalSection(g_mutex);
	if (udt & UDT_FUNC)
	{
		prg->m_stack[prg->m_stackIndex].push_back(prg);
		if (func)
		{
			try
			{
				assert(prg->m_stack[prg->m_stackIndex].size() > params);
				CALL_DATA call = {params, &prg->m_stack[prg->m_stackIndex].back() - params, prg};
				func(call);
			}
			catch (CException exp)
			{
				prg->handleError(&exp);
			}
			catch (...)
			{
				prg->handleError(NULL);
			}
		}

		//` methodCall() preliminarily pops the parameters off the initial stack, before 
		//` pushing the new stack.
		//` It attempts to duplicate the back() stack, so that erase() can act on it
		//` without overflowing, but that was not working as intended. (VC2008)

		assert(prg->m_stack[prg->m_stackIndex].size() > params);

		prg->m_stack[prg->m_stackIndex].erase(
			prg->m_stack[prg->m_stackIndex].end() - params - 1, 
			prg->m_stack[prg->m_stackIndex].end() - 1);

	}
	else if (udt & UDT_CLOSE)
	{
		/**
		 * Hacky code here. The num member is actually storing
		 * two longs in the double. The first long is the unit
		 * which holds the opening brace, and the second long
		 * is the unit which holds the beginning of the first
		 * statement before the opening brace.
		 */
		const unsigned long *const pLines = (unsigned long *)&num;
		CONST_POS open = prg->m_units.begin() + pLines[0];
		if ((open != prg->m_units.end()) && ((open - 1)->udt & UDT_FUNC))
		{
			const MACHINE_FUNC &func = (open - 1)->func;
			if ((func == CProgram::whileLoop) || (func == CProgram::forLoop) || (func == CProgram::untilLoop))
			{
				prg->m_i = prg->m_units.begin() + (pLines[1] > 0 ? pLines[1] : 1) - 1;
			}
			else if (func == CProgram::skipMethod)
			{
				const bool bReturn = prg->m_calls.back().bReturn;
				prg->m_i = prg->m_units.begin() + prg->m_calls.back().i;
				prg->m_calls.pop_back();
				prg->m_stack.pop_back();
				prg->m_stackIndex = prg->m_stack.size() - 1;
				prg->getLocals()->pop_back();
				if (bReturn)
				{
					LeaveCriticalSection(g_mutex);
					return;
				}
			}
			else if (((func == CProgram::conditional) || (func == CProgram::elseIf)) && (prg->m_i != prg->m_units.end()))
			{
				CONST_POS i = prg->m_i;
				while (++i != prg->m_units.end())
				{
					if ((i->udt & UDT_FUNC) && (i->udt & UDT_LINE)) break;
				}
				if ((i != prg->m_units.end()) && (i->func == CProgram::elseIf))
				{
					prg->m_i = prg->m_units.begin() + int((i + 1)->num) - 1;
				}
			}
			else
			{
				extern void multiRunBegin(CALL_DATA &params), multiRunEnd(CProgram *prg);
				if (func == multiRunBegin)
				{
					multiRunEnd(prg);
				}
			}
		}
	}
	else if (!(udt & UDT_OPEN))
	{
		STACK_FRAME fr(prg);
		fr.lit = lit;
		fr.num = num;
		fr.udt = udt;
		prg->m_stack[prg->m_stackIndex].push_back(fr);
	}
	if (udt & UDT_LINE)
	{
		prg->m_stack[prg->m_stackIndex].clear();
	}
	LeaveCriticalSection(g_mutex);
}

/*
 * *************************************************************************
 * tagNamedMethod
 * *************************************************************************
 */

// Locate a named method.
tagNamedMethod *tagNamedMethod::locate(const STRING &name, const int params, const bool bMethod, CProgram &prg)
{
	std::vector<NAMED_METHOD>::iterator i = prg.m_methods.begin();
	for (; i != prg.m_methods.end(); ++i)
	{
		if ((i->name == name) && (i->params == params) && (bMethod || (i->i != 0xffffff)))
		{
			return &*i;
		}
	}
	return NULL;
}

tagNamedMethod *tagNamedMethod::locate(const STRING &name, const int params, const bool bMethod)
{
	std::vector<NAMED_METHOD>::iterator i = m_methods.begin();
	for (; i != m_methods.end(); ++i)
	{
		if ((i->name == name) && (i->params == params) && (bMethod || (i->i != 0xffffff)))
		{
			return &*i;
		}
	}
	return NULL;
}

/*
 * *************************************************************************
 * tagStackFrame
 * *************************************************************************
 */

// Get the numerical value from a stack frame.
double tagStackFrame::getNum() const
{
	if (udt & UDT_ID)
	{
		return prg->getVar(lit)->getNum();
	}
	else if (udt & UDT_LIT)
	{
		return atof(lit.c_str());
	}
	return num;
}

// Get the boolean value from a stack frame.
bool tagStackFrame::getBool() const
{
	if (getType() & UDT_LIT)
	{
		return (_tcsicmp(getLit().c_str(), _T("off")) != 0);
	}
	return (getNum() != 0.0);
}

// Get the literal value from a stack frame.
STRING tagStackFrame::getLit() const
{
	if (udt & UDT_ID)
	{
		return prg->getVar(lit)->getLit();
#if !defined(_DEBUG) && defined(_MSC_VER)
		// Without the following line, VC++ will crash
		// in release mode. I'll be damned if I know why.
		const STRING str;
#endif
	}
	else if (udt & UDT_NUM)
	{
		if (udt & UDT_UNSET)
		{
			// Return an empty string rather than "0" if
			// the variable hasn't been set.
			return STRING();
		}

		// Convert the number to a string.
		STRINGSTREAM ss;
		ss << num;
		return ss.str();
	}
	return lit;
}

// Get the type of data in a stack frame.
UNIT_DATA_TYPE tagStackFrame::getType() const
{
	if (udt & UDT_ID)
	{
		return prg->getVar(lit)->getType();
	}
	return udt;
}

// Get the value in a stack frame.
tagStackFrame tagStackFrame::getValue() const
{
	if (udt & UDT_ID)
	{
		return prg->getVar(lit)->getValue();
	}

	// Ensure proper copying of virtual vars by using
	// getLit(), getNum(), and getType().
	tagStackFrame sf;
	sf.udt = getType();
	if (sf.udt & UDT_LIT)
	{
		sf.lit = getLit();
	}
	else
	{
		sf.num = getNum();
	}
	sf.prg = prg;
	//sf.tag = 0;
	return sf;
}

/*
 * *************************************************************************
 * Operators
 * *************************************************************************
 */

void operators::add(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(+, true);
	if ((call[0].getType() & UDT_NUM) && (call[1].getType() & UDT_NUM))
	{
		call.ret().num = call[0].getNum() + call[1].getNum();
		call.ret().udt = UDT_NUM;
	}
	else
	{
		call.ret().lit = call[0].getLit() + call[1].getLit();
		call.ret().udt = UDT_LIT;
	}
}

void operators::sub(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(-, true);
	call.ret().num = call[0].getNum() - call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::mul(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(*, true);
	call.ret().num = call[0].getNum() * call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::bor(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(|, true);
	call.ret().num = int(call[0].getNum()) | int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::bxor(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(`, true);
	call.ret().num = int(call[0].getNum()) ^ int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::band(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(&, true);
	call.ret().num = int(call[0].getNum()) & int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::lor(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(||, true);
	call.ret().num = call[0].getNum() || call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::land(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(&&, true);
	call.ret().num = call[0].getNum() && call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::ieq(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(~=, false);
	if ((call[0].getType() & UDT_NUM) && (call[1].getType() & UDT_NUM))
	{
		call.ret().num = (call[0].getNum() != call[1].getNum());
	}
	else
	{
		call.ret().num = (call[0].getLit() != call[1].getLit());
	}
	call.ret().udt = UDT_NUM;
}

void operators::eq(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(==, false);
	if ((call[0].getType() & UDT_NUM) && (call[1].getType() & UDT_NUM))
	{
		call.ret().num = (call[0].getNum() == call[1].getNum());
	}
	else
	{
		call.ret().num = (call[0].getLit() == call[1].getLit());
	}
	call.ret().udt = UDT_NUM;
}

void operators::gte(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(>=, true);
	call.ret().num = call[0].getNum() >= call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::lte(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(<=, true);
	call.ret().num = call[0].getNum() <= call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::gt(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(>, true);
	call.ret().num = call[0].getNum() > call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::lt(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(<, true);
	call.ret().num = call[0].getNum() < call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::rs(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(>>, true);
	call.ret().num = int(call[0].getNum()) >> int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::ls(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(<<, true);
	call.ret().num = int(call[0].getNum()) << int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::mod(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(%, true);
	call.ret().num = int(call[0].getNum()) % int(call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::div(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(/, true);
	call.ret().num = call[0].getNum() / call[1].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::pow(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(^, true);
	call.ret().num = ::pow(call[0].getNum(), call[1].getNum());
	call.ret().udt = UDT_NUM;
}

void operators::assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(=, false);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	*call.prg->getVar(call.ret().lit) = call[1].getValue();
}

void operators::xor_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(`=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) ^ int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::or_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(|=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) | int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::and_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(&=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) & int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::rs_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(>>=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) >> int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::ls_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(<<=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) << int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::sub_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(-=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() - call[1].getNum();
	var.udt = UDT_NUM;
}

void operators::add_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(+=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	if ((call[0].getType() & UDT_NUM) && (call[1].getType() & UDT_NUM))
	{
		var.num = call[0].getNum() + call[1].getNum();
		var.udt = UDT_NUM;
	}
	else
	{
		var.lit = call[0].getLit() + call[1].getLit();
		var.udt = UDT_LIT;
	}
}

void operators::mod_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(%=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = int(call[0].getNum()) % int(call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::div_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(/=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() / call[1].getNum();
	var.udt = UDT_NUM;
}

void operators::mul_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(*=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() * call[1].getNum();
	var.udt = UDT_NUM;
}

void operators::pow_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(^=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = ::pow(call[0].getNum(), call[1].getNum());
	var.udt = UDT_NUM;
}

void operators::lor_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(||=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() || call[1].getNum();
	var.udt = UDT_NUM;
}

void operators::land_assign(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(&&=, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() && call[1].getNum();
	var.udt = UDT_NUM;
}

void operators::prefixIncrement(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(++, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = call[0].getNum() + 1;
	var.udt = UDT_NUM;
}

void operators::postfixIncrement(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(++, true);
	call.ret().udt = UDT_NUM;
	call.ret().num = call[0].getNum();

	STACK_FRAME &var = *call.prg->getVar(call[0].lit);
	var.num = var.getNum() + 1;
	var.udt = UDT_NUM;
}

void operators::prefixDecrement(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(--, true);
	call.ret().udt = UDT_ID;
	call.ret().lit = call[0].lit;
	STACK_FRAME &var = *call.prg->getVar(call.ret().lit);
	var.num = var.getNum() - 1;
	var.udt = UDT_NUM;
}

void operators::postfixDecrement(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(--, true);
	call.ret().udt = UDT_NUM;
	call.ret().num = call[0].getNum();

	STACK_FRAME &var = *call.prg->getVar(call[0].lit);
	var.num = var.getNum() - 1;
	var.udt = UDT_NUM;
}

void operators::unaryNegation(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(-, true);
	call.ret().num = -call[0].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::lnot(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(!, true);
	call.ret().num = !call[0].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::bnot(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR(~, true);
	call.ret().num = ~(int)call[0].getNum();
	call.ret().udt = UDT_NUM;
}

void operators::tertiary(CALL_DATA &call)
{
	call.ret() = call[0].getNum() ? call[1].getValue() : call[2].getValue();
}

void operators::member(CALL_DATA &call)
{
	unsigned int obj = static_cast<unsigned int>(call[0].getNum());
	std::map<unsigned int, STRING>::const_iterator obj_it = CProgram::m_objects.find(obj);

	if (!(call[0].getType() & UDT_OBJ) || obj_it == CProgram::m_objects.end())
	{
		throw CError(_T("Invalid object."));
	}

	const STRING &type = obj_it->second;
	std::map<STRING, CLASS>::const_iterator cls_it = call.prg->m_classes.find(type);

	if (cls_it == call.prg->m_classes.end())
	{
		throw CError(_T("Could not find class ") + type + _T("."));
	}

	assert(call.prg->m_calls.empty() || call.prg->m_calls.back().obj == 0 || 
		CProgram::m_objects.count(call.prg->m_calls.back().obj));

	const CLASS_VISIBILITY cv = (call.prg->m_calls.size() && 
		(CProgram::m_objects[call.prg->m_calls.back().obj] == type)) ? CV_PRIVATE : CV_PUBLIC;
	const STRING &mem = call[1].lit;
	if (!cls_it->second.memberExists(mem, cv))
	{
		throw CError(_T("Class ") + type + _T(" has no accessible ") + mem + _T(" member."));
	}

	TCHAR str[33];
	_itot(obj, str, 10);
	call.ret().udt = UDT_ID;
	call.ret().lit = _T(":") + STRING(str) + _T("::") + mem;
}

void operators::array(CALL_DATA &call)
{
	CHECK_OVERLOADED_OPERATOR([], false);

	call.ret().udt = UDT_ID;
	// TBD: This should be done at compile-time.
	const std::pair<bool, STRING> res = call.prg->getInstanceVar(call[0].lit);
	const STRING prefix = (res.first ? (_T(':') + res.second) : call[0].lit);
	call.ret().lit = prefix + _T('[') + call[1].getLit() + _T(']');
}
