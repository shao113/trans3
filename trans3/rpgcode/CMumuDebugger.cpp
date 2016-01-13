//MuMu Debugger

/*
	TODO:
		Reference parameters: m_calls[].refs
		Parameters of inherited methods: need to check inherits when finding methodIdx
		RPGCode() / inline

		Limit Cache size
		Should watches/breakpoints be static, or per instance?
		Switching between displayed files
		Show scope of resolved values


	Code Cleanup:
		Panels should be separate objects

	Features to consider:
		Performance Profiler
*/

#ifdef ENABLE_MUMU_DBG

#include "CMumuDebugger.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include "Shlwapi.h"
#include "CCursorMap.h"
#include "input/input.h"
#include "common/CFile.h"
#include "common/CAllocationHeap.h"
#include "common/paths.h"
#include "common/mbox.h"
#include "../tkCommon/images/FreeImage.h"

// Static member initializations:
std::vector<STRING> CMumuDebugger::s_cachedFilenames;
std::vector< std::vector<STRING> > CMumuDebugger::s_cachedText;
std::map<MACHINE_FUNC, STRING> CMumuDebugger::s_overloadableOps;
std::map<MACHINE_FUNC, STRING> CMumuDebugger::s_assignmentOps;
std::vector< std::set<int> > CMumuDebugger::s_breakpoints;
CMumuDebugger::Fonts CMumuDebugger::s_fonts;

bool CMumuDebugger::s_autoDisplay = false;

const COLORREF CMumuDebugger::kDefaultBorderColor	= RGB(0x00, 0x00, 0x00);
const COLORREF CMumuDebugger::kSelectedLineColor	= RGB(0xC2, 0x0C, 0x0C);
const COLORREF CMumuDebugger::kActiveLineColor		= RGB(0x10, 0x70, 0xFF);
const COLORREF CMumuDebugger::kBreakpointColor		= RGB(0xC2, 0x0C, 0x0C);

const double CMumuDebugger::kOverlayTranslucency	= 0.2;
const int CMumuDebugger::kCommandStripHeight		= 32;
const int CMumuDebugger::kInfoPanelWidth			= 180;
const int CMumuDebugger::kLineNumbersStripWidth		= 36;
const int CMumuDebugger::kMuPanelHeight				= 120;
const int CMumuDebugger::kFileStripHeight			= 18;
const int CMumuDebugger::kFileStripSpacing			= 16;
const int CMumuDebugger::kLineHeight				= 14;
const int CMumuDebugger::kMuLineHeight				= 12;
const int CMumuDebugger::kMuColumnSpacing			= 5;
const int CMumuDebugger::kMuTopMargin				= 2;
const int CMumuDebugger::kCodePanelLeftMargin		= 8;
const int CMumuDebugger::kTabSize					= 3;
const int CMumuDebugger::kMaxWatches				= 9;
const int CMumuDebugger::kInfoPanelLineHeight		= 19;
const int CMumuDebugger::kInfoPanelLeftMargin		= 8;
const int CMumuDebugger::kCommandWindowLineHeight	= 20;

const int CMumuDebugger::kMuColumnWidths[kMuColumnCount] =		{ 36,   97,    75,    104,   100,    18 };
const STRING CMumuDebugger::kMuColumnCaptions[kMuColumnCount] =	{ "#", "LIT", "NUM", "UDT", "FUNC", "P#" };

extern CDirectDraw *g_pDirectDraw;
extern int g_resX, g_resY;

namespace // Anonymous namespace
{
	// Returns a copy of a string, enclosed in quotes, with double quotes escaped as double-double
	// quotes (for CSV)
	inline STRING quoteString(STRING s)
	{
		STRING::size_type n = 0;
		while ((n = s.find(_T("\""), n)) != std::string::npos)
		{
			s.replace(n, 1, _T("\"\""));
			n += 2; 
		}
		s.push_back(_T('"'));
		s.insert(s.begin(), _T('"'));
		return s;
	}

	inline STRING getShortUnitDataType(UNIT_DATA_TYPE udt)
	{
		STRING ret;

		if (udt & UDT_UNSET) ret += "UNSET, ";
		if (udt & UDT_NUM) ret += "NUM, ";
		if (udt & UDT_LIT) ret += "LIT, ";
		if (udt & UDT_ID) ret += "ID, ";
		if (udt & UDT_FUNC) ret += "FUNC, ";
		if (udt & UDT_OPEN) ret += "OPEN, ";
		if (udt & UDT_CLOSE) ret += "CLOSE, ";
		if (udt & UDT_LINE) ret += "LINE, ";
		if (udt & UDT_OBJ) ret += "OBJ, ";
		if (udt & UDT_LABEL) ret += "LABEL, ";
		if (udt & UDT_PLUGIN) ret += "PLUGIN, ";

		return ret.empty() ? STRING() : ret.substr(0, ret.length() - 2);
	}

	// Check if a key reduces cleanly to a numerical value
	inline bool isNumerical(STRING key)
	{
		STRINGSTREAM ss;
		ss << atof(key.c_str());
		return key.length() && ss.str() == key;
	}

	inline bool isSymbol(TCHAR ch)
	{
		return isgraph(ch) && !isalnum(ch);
	}

	inline bool isOperator(const STRING &functionName)
	{
		return functionName.size() && 
			( isSymbol(*functionName.begin()) || isSymbol(*functionName.rbegin()) );
	}

	inline void line(int x1, int y1, int x2, int y2, COLORREF color, HDC hdc)
	{
		HPEN tmpPen = CreatePen(PS_SOLID, 1, color);
		HPEN oldPen = (HPEN)SelectObject(hdc, tmpPen);
		MoveToEx(hdc, x1, y1, NULL);
		LineTo(hdc, x2, y2);
		SelectObject(hdc, oldPen);
		DeleteObject((HGDIOBJ)tmpPen);
	}

	inline COLORREF hexToColor(const STRING &hex)
	{
		if (hex.length() < 6)
			return 0;

		return RGB(	strtol(hex.substr(0, 2).c_str(), NULL, 16),
					strtol(hex.substr(2, 2).c_str(), NULL, 16),
					strtol(hex.substr(4, 2).c_str(), NULL, 16));
	}

	inline void escapeAmpersandsInPlace(STRING &str)
	{
		for (STRING::iterator it = str.begin(); it != str.end(); ++it)
		{
			if (*it == _T('&'))
				it = str.insert(it, _T('&')) + 1;
		}
	}

	template <class T>
	void searchVariables(const T &list, const STRING &search, int maxResults, std::vector<STRING> &results)
	{
		int i = 0;
		for (T::const_iterator it = list.lower_bound(search);
			it != list.end();
			++it)
		{
			const STRING &name = it->first;
			if (name.substr(0, search.length()) == search)
			{
				results.push_back(name);
				if (++i == maxResults)
					return;
			}
			else
			{
				return; //<- Passed the point where names could start with the search value.
			}
		}
	}

} // Anonymous namespace

CMumuDebugger::VariableParser::VariableParser(CProgram &prg, const STRING &var)
:	m_program(prg)
{
	parse(var, true);
}

STRING CMumuDebugger::VariableParser::parse(const STRING &var, bool forced)
{
	if (var != m_input || forced)
	{
		m_input = var;
		m_iter = m_input.begin();
		m_memberSplice_iter = m_input.end();
		m_output = parseVariable();
	}
	return m_output;
}

void CMumuDebugger::VariableParser::skipWhitespace()
{
	for ( /**/ ; m_iter != m_input.end(); ++m_iter)
	{
		if (!isspace(*m_iter))
			return;
	}
}

// Doesn't prevent invalid numbers (multiple decimal points, etc.)
STRING CMumuDebugger::VariableParser::parseNumber()
{
	if (m_iter == m_input.end() || !isNumberFirstChar(*m_iter)) //<- Redundant, already done in parseKey()
		return STRING(); //<- Bad or incomplete input

	STRING::const_iterator start = m_iter;
	++m_iter;

	for ( /**/ ; m_iter != m_input.end(); ++m_iter)
	{
		if (!isNumberChar(*m_iter))
			break;
	}

	// Try to normalize format by converting from string to double and back again
	STRINGSTREAM ss;
	ss << atof(STRING(start, m_iter).c_str());
	return ss.str();
}

STRING CMumuDebugger::VariableParser::parseString(TCHAR quotChar)
{
	++m_iter; //<- Skip leading quote
	STRING::const_iterator start = m_iter;

	for ( /**/ ; m_iter != m_input.end(); ++m_iter)
	{
		if (*m_iter == quotChar)
		{
			++m_iter; //<- Skip closing quote
			return STRING(start, m_iter - 1); //<- Return parsed string without quotes
		}
	}
	// Reached end before closing quote; Return partial (for autocomplete)
	return STRING(start, m_input.end());
}

STRING CMumuDebugger::VariableParser::parseIdentifier()
{
	STRING::const_iterator start = m_iter;
	for ( /**/ ; m_iter != m_input.end(); ++m_iter)
	{
		if (!isIdentifierChar(*m_iter))
			break;
	}
	return lcase(STRING(start, m_iter));
}

// Key to an array, basic support for string literals, numbers, and variables
STRING CMumuDebugger::VariableParser::parseKey()
{
	if (m_iter == m_input.end())
		return STRING(); //<- Bad or incomplete input

	if (*m_iter == _T('\'') || *m_iter == _T('"'))
		return parseString(*m_iter);

	if (isNumberFirstChar(*m_iter))
		return parseNumber();

	return resolveToString(parseVariable(true));
}

STRING CMumuDebugger::VariableParser::parseVariable(bool isKey, STRING objPrefix)
{
	skipWhitespace();

	if (m_iter != m_input.end() && isIdentifierFirstChar(*m_iter))
	{
		STRING id = objPrefix + parseIdentifier();
		skipWhitespace();

		// Plain identifier?
		if (m_iter == m_input.end() || (isKey && *m_iter == _T(']')))
			return id;

		// Array element?
		while (*m_iter == _T('['))
		{
			++m_iter; //<- Skip '['
			skipWhitespace();
			STRING key = parseKey();
			skipWhitespace();

			if (m_iter == m_input.end() || *m_iter != _T(']'))
				return id + _T('[') + key;	//<- Bad or incomplete input; Return partial

			++m_iter; //<- Skip ']'
			skipWhitespace();
			id += _T('[') + key + _T(']');

			// Allow for outer ']', e.g. x[y[z]]
			if (m_iter == m_input.end() || (isKey && *m_iter == _T(']')))
				return id;

			// Continue into the next iteration to check for additional dimensions, or fall
			// through to check for member access, e.g. x[y]->z
		}

		// Member access?
		if (*m_iter == _T('-'))
		{
			++m_iter; //<- Skip '-'
			if (m_iter != m_input.end() && (*m_iter == _T('>')))
			{
				++m_iter; //<- Skip '>'
				skipWhitespace();

				if (!isKey)
					m_memberSplice_iter = m_iter;

				// TODO: Should verify that this is actually an OBJ
				STRING obj = resolveToString(id) + _T("::");
				if (m_iter == m_input.end())
					return obj; //<- Bad or incomplete input; Return partial (for autocomplete)
				else
					return parseVariable(isKey, obj); 
			}
		}

		return id; //<- Bad or incomplete input; Return partial
	}

	return STRING(); //<- Bad or incomplete input
}

LPSTACK_FRAME CMumuDebugger::VariableParser::resolveToVariable(const STRING &formattedName) const
{
	// TODO: references, ...
	// TODO: check only globals when x::y format?
	std::list<std::map<STRING, STACK_FRAME> > *pLocalList = m_program.getLocals();

	// Check parameters
	if (pLocalList && !pLocalList->empty() && !m_program.m_calls.empty())
	{
		const int methodIdx = m_program.m_calls.back().methodIdx;
		if (methodIdx >= 0 && methodIdx < m_program.m_methods.size())
		{
			const tagNamedMethod &method = m_program.getMethod(methodIdx);
			std::map<STRING, STRING>::const_iterator res = method.paramNames.find(formattedName);
			
			if (res != method.paramNames.end())
			{
				// Search locals for the internal parameter name:
				std::map<STRING, STACK_FRAME> &locals = pLocalList->back();
				std::map<STRING, STACK_FRAME>::iterator resLocal = locals.find(res->second);
				if (resLocal != locals.end())
				{
					return &resLocal->second;
				}
			}
		}
	}

	// Check locals & instance variables:
	if (pLocalList && !pLocalList->empty())
	{
		std::map<STRING, STACK_FRAME> &locals = pLocalList->back();
		std::map<STRING, STACK_FRAME>::iterator res = locals.find(formattedName);
		if (res != locals.end())
		{
			return &res->second;
		}

		// Check instance variables belonging to local "this":
		res = locals.find("this");
		if (res != locals.end())
		{
			STRING instanceVar = res->second.getLit() + _T("::") + formattedName;
			std::map<STRING, CPtrData<STACK_FRAME> >::iterator res = m_program.m_heap.find(instanceVar);
			if (res != m_program.m_heap.end())
			{
				return res->second;
			}
		}
	}

	// Check globals:
	std::map<STRING, CPtrData<STACK_FRAME> >::iterator res = m_program.m_heap.find(formattedName);
	if (res != m_program.m_heap.end())
	{
		return res->second;
	}

	return NULL; //<- Could not resolve indicated variable.
}

STRING CMumuDebugger::VariableParser::resolveToString(const STRING &formattedName, bool verbose) const
{
	if (verbose)
		return resolveToVerboseString(formattedName);

	LPSTACK_FRAME var = resolveToVariable(formattedName);
	return var ? var->getLit() : STRING();
}

STRING CMumuDebugger::VariableParser::resolveToVerboseString(const STRING &formattedName) const
{
	LPSTACK_FRAME var = resolveToVariable(formattedName);
	STRINGSTREAM ss;

	if (!var)
		return _T("{NOT FOUND}");

	UNIT_DATA_TYPE udt = var->getType();

	if (udt & UDT_UNSET)
		ss << _T("{UNSET}");
	else if (udt & UDT_OBJ)
		ss << _T("{OBJ #") << var->getNum() << _T('}');
	else if (udt & UDT_LIT)
		ss << _T('"') << var->getLit() << _T('"');
	else if (udt & UDT_NUM)
		ss << var->getNum();
	else ss << _T("?: ") << var->getLit();

	return ss.str();
}

void CMumuDebugger::VariableParser::searchParameters(const STRING &search, int maxResults,
													 std::vector<STRING> &results) const
{
	if (m_program.m_calls.empty())
		return;

	const int methodIdx = m_program.m_calls.back().methodIdx;
	if (methodIdx >= 0 && methodIdx < m_program.m_methods.size())
	{
		const tagNamedMethod &method = m_program.getMethod(methodIdx);
		searchVariables(method.paramNames, search, maxResults, results);
	}
}

void CMumuDebugger::VariableParser::searchLocals(const STRING &search, int maxResults,
												 std::vector<STRING> &results) const
{
	std::list<std::map<STRING, STACK_FRAME> > *pLocalList = m_program.getLocals();
	if (!pLocalList || pLocalList->empty())
		return;

	searchVariables(pLocalList->back(), search, maxResults, results);
}

void CMumuDebugger::VariableParser::searchMembers(const STRING &search, int maxResults,
												  std::vector<STRING> &results) const
{
	std::list<std::map<STRING, STACK_FRAME> > *pLocalList = m_program.getLocals();
	if (!pLocalList || pLocalList->empty())
		return;

	const std::map<STRING, STACK_FRAME> &locals = pLocalList->back();
	std::map<STRING, STACK_FRAME>::const_iterator res = locals.find(_T("this"));
	if (res == locals.end())
		return;

	// Search the heap for matches belonging to local "this":
	std::vector<STRING> tmpResults;
	STRING prefix = res->second.getLit() + _T("::");
	searchGlobals(prefix + search, maxResults, tmpResults);

	// Strip the object part off of each result, and add the member to the actual results.
	int i = 0;
	for (std::vector<STRING>::const_iterator it = tmpResults.begin(); it != tmpResults.end(); ++it)
	{
		results.push_back(it->substr(prefix.length()));
		if (++i == maxResults)
			return;
	}

}

void CMumuDebugger::VariableParser::searchGlobals(const STRING &search, int maxResults,
												  std::vector<STRING> &results) const
{
	searchVariables(m_program.m_heap, search, maxResults, results);
}

std::vector<STRING> CMumuDebugger::VariableParser::getAutocomplete(const STRING &input, int maxResults) 
{
	std::vector<STRING> results;
	parse(input);

	if (m_output.empty())
		return results; //<- Return empty list

	searchParameters(m_output, maxResults, results);
	searchLocals(m_output, maxResults, results);
	searchMembers(m_output, maxResults, results);
	searchGlobals(m_output, maxResults, results);

	// Sort and truncate the combined results.
	std::sort(results.begin(), results.end());
	if (results.size() > maxResults)
		results.resize(maxResults);

	for (std::vector<STRING>::iterator it = results.begin(); it != results.end(); ++it)
	{
		// Quote literal keys
		// Note: String keys containing brackets will definitely trip this up.
		STRING::size_type pos = it->find(_T('['));
		while (pos != STRING::npos)
		{
			STRING::size_type keyStart = pos + 1;
			pos = it->find(_T(']'), keyStart);
			if (pos != STRING::npos)
			{
				STRING key = it->substr(keyStart, pos - keyStart);
				if (!isNumerical(key))
				{
					it->insert(pos, _T("\""));
					it->insert(keyStart, _T("\""));
					pos += 2;
				}
				pos = it->find(_T('['), pos);
			} 
		}

		// Replace "objID::" with "obj->" part from input string
		pos = it->find(_T("::"));
		if (pos != STRING::npos)
		{
			STRING::size_type bracketPos = it->find(_T('['));
			if (bracketPos == STRING::npos || pos < bracketPos)
				it->assign(getObjectPart() + it->substr(pos + 2));
		}
	}

	return results;
}

STRING CMumuDebugger::VariableParser::resolve(bool refresh, bool verbose)
{
	return resolve(STRING(), refresh, verbose);
}

STRING CMumuDebugger::VariableParser::resolve(const STRING &var, bool refresh, bool verbose)
{
	// By default, resolve the previously parsed variable.
	return resolveToString(parse(var.empty() ? m_input : var, refresh), verbose);
}

void CMumuDebugger::PanelStyle::draw(CCanvas &cnv) const
{
	cnv.DrawFilledRect(rect.left, rect.top, rect.right-1, rect.bottom-1, backColor);

	if (border & pbTop)
		cnv.DrawLine(rect.left, rect.top, rect.right, rect.top, borderColor);
	if (border & pbBottom)
		cnv.DrawLine(rect.left, rect.bottom-1, rect.right, rect.bottom-1, borderColor);
	if (border & pbLeft)
		cnv.DrawLine(rect.left, rect.top, rect.left, rect.bottom, borderColor);
	if (border & pbRight)
		cnv.DrawLine(rect.right-1, rect.top, rect.right-1, rect.bottom, borderColor);
}

CMumuDebugger::Fonts::Fonts()
:	base(NULL), smaller(NULL), bold(NULL), large(NULL), arial(NULL), arialBold(NULL)
{
	LOGFONT lfBaseFont = {0};
	lfBaseFont.lfHeight = 12;
	strcpy(lfBaseFont.lfFaceName, "Lucida Console");

	LOGFONT lfSmallFont(lfBaseFont);
	lfSmallFont.lfHeight = 10;
	lfSmallFont.lfQuality = NONANTIALIASED_QUALITY;

	LOGFONT lfBoldFont(lfBaseFont);
	lfBoldFont.lfWeight = FW_SEMIBOLD;

	LOGFONT lfLargeFont(lfBaseFont);
	lfLargeFont.lfHeight = 14;

	LOGFONT lfArial = {0};
	lfArial.lfHeight = 13;
	strcpy(lfArial.lfFaceName, "Arial");

	LOGFONT lfArialBold = lfArial;
	lfArialBold.lfWeight = FW_SEMIBOLD; 

	base = create(&lfBaseFont);
	smaller = create(&lfSmallFont);
	bold = create(&lfBoldFont);
	large = create(&lfLargeFont);
	arial = create(&lfArial);
	arialBold = create(&lfArialBold);
}

CMumuDebugger::Fonts::~Fonts()
{
	for (vector<HFONT>::const_iterator it = m_fonts.begin(); it != m_fonts.end(); ++it)
		DeleteObject(*it);
}

HFONT CMumuDebugger::Fonts::create(LOGFONT *lf)
{
	HFONT res = CreateFontIndirect(lf);
	if (!res) res = (HFONT)GetStockObject(SYSTEM_FONT);
	m_fonts.push_back(res);
	return res;
}

CMumuDebugger::CMumuDebugger(CProgram& prg)
:	m_program(prg),
	m_mu(prg.m_units.begin()),
	m_steppingMode(smNone),
	m_fromUnit(prg.m_units.begin()),
	m_fromStackIndex(0),
	m_selectedLine(-1),
	m_fileIndex(-1),
	m_toLine(-1),
	m_toFileIndex(-1),
	m_watches(kMaxWatches),
	m_watchIndex(0),
	m_hidden(false),
	m_breakpointBrush(NULL),
	m_fnCaptions(kMaxFnKeys)
{
	m_canvas.CreateBlank(NULL, g_resX, g_resY, TRUE);
	m_canvas.ClearScreen(0);

	setupOps();
	setupViews();

	if (s_autoDisplay && prg.isReady())
		setStep(smStepInto);
}

CMumuDebugger::~CMumuDebugger()
{
	if (m_breakpointBrush)
		DeleteObject(m_breakpointBrush);
}

void CMumuDebugger::setupViews()
{
	//TODO: Move all the panel stuff to a Panel class; a View should be a collection of Panels

	// Code Panel
	m_panels[ptCode] = PanelStyle();
	m_panels[ptCode].backColor = RGB(0xF0, 0xF0, 0xF0);
	m_panels[ptCode].foreColor = RGB(0x00, 0x00, 0x00);
	m_panels[ptCode].font = s_fonts.base;

	// Line Numbers Strip
	m_panels[ptLineNumbers] = PanelStyle();
	m_panels[ptLineNumbers].backColor = RGB(0xCC, 0xCC, 0xCC);
	m_panels[ptLineNumbers].foreColor = RGB(0x00, 0x00, 0x00);
	m_panels[ptLineNumbers].font = s_fonts.base;

	// Command Strip
	m_panels[ptCommandStrip] = PanelStyle();
	m_panels[ptCommandStrip].backColor = RGB(0x28, 0x34, 0x7F);
	m_panels[ptCommandStrip].foreColor = RGB(0xFF, 0xFF, 0xFF);
	m_panels[ptCommandStrip].dimColor = RGB(0x91, 0x8B, 0xBB);
	m_panels[ptCommandStrip].litColor = RGB(0xBE, 0xBE, 0x80);
	m_panels[ptCommandStrip].font = s_fonts.smaller;
	m_panels[ptCommandStrip].altFont = s_fonts.large;

	// Info Panel
	m_panels[ptInfo] = PanelStyle();
	m_panels[ptInfo].backColor = RGB(0xFF, 0xFF, 0xFF);
	m_panels[ptInfo].foreColor = RGB(0x00, 0x00, 0x00);
	m_panels[ptInfo].litColor = RGB(0x30, 0x30, 0xD0);
	m_panels[ptInfo].font = s_fonts.base;
	m_panels[ptInfo].dimFont = s_fonts.arial;
	m_panels[ptInfo].altFont = s_fonts.arialBold;
	m_panels[ptInfo].border = pbLeft;

	// Machine Units Panel
	m_panels[ptMUnits] = PanelStyle();
	m_panels[ptMUnits].backColor = RGB(0xF1, 0xEF, 0xDC);
	m_panels[ptMUnits].foreColor = RGB(0x00, 0x00, 0x00);
	m_panels[ptMUnits].litColor = RGB(0xAA, 0x00, 0x00);
	m_panels[ptMUnits].dimColor = RGB(0xAA, 0xAA, 0xAA);
	m_panels[ptMUnits].borderColor = RGB(0xFF, 0xFF, 0xFF);
	m_panels[ptMUnits].font = s_fonts.arial;
	m_panels[ptMUnits].altFont = s_fonts.arialBold;

	// File Strip
	m_panels[ptFileStrip] = PanelStyle();
	m_panels[ptFileStrip].backColor = RGB(0x68, 0x77, 0xD0);
	m_panels[ptFileStrip].foreColor = RGB(0xFF, 0xFF, 0xFF);
	m_panels[ptFileStrip].dimColor = RGB(0xCE, 0xCC, 0xD8);
	m_panels[ptFileStrip].font = s_fonts.bold;
	m_panels[ptFileStrip].dimFont = s_fonts.base;
	m_panels[ptFileStrip].border = pbTop | pbBottom;

	// Calculate Rectangles
	m_panels[ptCommandStrip].setBounds(0, 0, g_resX, kCommandStripHeight);
	m_panels[ptCode].setBounds(kLineNumbersStripWidth, kCommandStripHeight, g_resX - kInfoPanelWidth, g_resY - kMuPanelHeight - kFileStripHeight);
	m_panels[ptLineNumbers].setBounds(0, m_panels[ptCode].rect.top, kLineNumbersStripWidth, m_panels[ptCode].rect.bottom);
	m_panels[ptInfo].setBounds(m_panels[ptCode].rect.right, m_panels[ptCode].rect.top, g_resX, g_resY);
	m_panels[ptFileStrip].setBounds(0, m_panels[ptCode].rect.bottom, m_panels[ptCode].rect.right, m_panels[ptCode].rect.bottom + kFileStripHeight);
	m_panels[ptMUnits].setBounds(0, m_panels[ptFileStrip].rect.bottom, m_panels[ptCode].rect.right, g_resY);

	m_breakpointBrush = CreateSolidBrush(kBreakpointColor);
	m_cnvLineOverlay.CreateBlank(NULL, m_panels[ptInfo].rect.left, kLineHeight, TRUE);

	// Set up Command Strip items
	m_fnEnabled.reset();
	m_fnEnabled.set(5 - 1);
	m_fnEnabled.set(6 - 1);
	m_fnEnabled.set(7 - 1);
	m_fnEnabled.set(8 - 1);
	m_fnEnabled.set(11 - 1);
	m_fnEnabled.set(12 - 1);
	m_fnCaptions[0] = "F1:\nHome View";
	m_fnCaptions[1] = "F2:\nPrev View";
	m_fnCaptions[2] = "F3:\nNext View";
	m_fnCaptions[3] = "F4:\nSwitch Ctrl";
	m_fnCaptions[4] = "F5:\nRun";
	m_fnCaptions[5] = "F6:\nStep Out";
	m_fnCaptions[6] = "F7:\nStep Over";
	m_fnCaptions[7] = "F8:\nStep Into";
	m_fnCaptions[8] = "F9:\nRun To";
	m_fnCaptions[9] = "F10:\n------";
	m_fnCaptions[10] = "F11:\nAction";
	m_fnCaptions[11] = "F12:\nHide MuMu";

	// Set up File Strip items
	m_filenames.push_back(removePath(m_program.m_fileName));

	for (std::vector<STRING>::const_iterator it = m_program.m_inclusions.begin();
		it != m_program.m_inclusions.end();
		++it)
	{
		m_filenames.push_back(removePath(*it));
	}

	std::replace(m_filenames.begin(), m_filenames.end(), STRING(""), STRING("<n/a>"));
}

void CMumuDebugger::setupOps()
{
	if (s_assignmentOps.empty())
	{
		s_assignmentOps[operators::assign] = _T("=");
		s_assignmentOps[operators::xor_assign] = _T("`=");
		s_assignmentOps[operators::or_assign] = _T("|=");
		s_assignmentOps[operators::and_assign] = _T("&=");
		s_assignmentOps[operators::rs_assign] = _T(">>=");
		s_assignmentOps[operators::ls_assign] = _T("<<=");
		s_assignmentOps[operators::sub_assign] = _T("-=");
		s_assignmentOps[operators::add_assign] = _T("+=");
		s_assignmentOps[operators::mod_assign] = _T("%=");
		s_assignmentOps[operators::div_assign] = _T("/=");
		s_assignmentOps[operators::mul_assign] = _T("*=");
		s_assignmentOps[operators::pow_assign] = _T("^=");
		s_assignmentOps[operators::lor_assign] = _T("||=");
		s_assignmentOps[operators::land_assign] = _T("&&=");
		s_assignmentOps[operators::prefixIncrement] = _T("++");
		s_assignmentOps[operators::postfixIncrement] = _T("++");
		s_assignmentOps[operators::prefixDecrement] = _T("--");
		s_assignmentOps[operators::postfixDecrement] = _T("--");
	}
	if (s_overloadableOps.empty())
	{
		s_overloadableOps.insert(s_assignmentOps.begin(), s_assignmentOps.end());
		s_overloadableOps[operators::add] = _T("+");
		s_overloadableOps[operators::sub] = _T("-");
		s_overloadableOps[operators::mul] = _T("*");
		s_overloadableOps[operators::bor] = _T("|");
		s_overloadableOps[operators::bxor] = _T("`");
		s_overloadableOps[operators::band] = _T("&");
		s_overloadableOps[operators::lor] = _T("||");
		s_overloadableOps[operators::land] = _T("&&");
		s_overloadableOps[operators::ieq] = _T("~=");
		s_overloadableOps[operators::eq] = _T("==");
		s_overloadableOps[operators::gte] = _T(">=");
		s_overloadableOps[operators::lte] = _T("<=");
		s_overloadableOps[operators::gt] = _T(">");
		s_overloadableOps[operators::lt] = _T("<");
		s_overloadableOps[operators::rs] = _T(">>");
		s_overloadableOps[operators::ls] = _T("<<");
		s_overloadableOps[operators::mod] = _T("%");
		s_overloadableOps[operators::div] = _T("/");
		s_overloadableOps[operators::pow] = _T("^");
		s_overloadableOps[operators::unaryNegation] = _T("-");
		s_overloadableOps[operators::lnot] = _T("!");
		s_overloadableOps[operators::bnot] = _T("~");
		s_overloadableOps[operators::array] = _T("[]");
	}
}

int CMumuDebugger::loadProgram(const STRING &filename)
{
	std::vector<STRING>::const_iterator res = 
		std::find(s_cachedFilenames.begin(), s_cachedFilenames.end(), filename);

	if (res == s_cachedFilenames.end())
	{
		// Not in cache, so try to load it now.
		std::ifstream file(resolve(filename).c_str());
		if (!file.is_open())
			return -1;

		s_breakpoints.push_back(std::set<int>());
		s_cachedFilenames.push_back(filename);
		s_cachedText.push_back(vector<STRING>());
		// Compensate for the extra line inserted by CProgram::open()
		s_cachedText.back().push_back("// " + filename);

		STRING line;
		while (std::getline(file, line))
		{
			// Because DT_NOPREFIX is incompatible with DT_TABSTOP. (TODO: Do this during draw?)
			escapeAmpersandsInPlace(line);
			s_cachedText.back().push_back(line);
		}
		file.close();
		return s_cachedFilenames.size() - 1;
	}
	else
	{
		// Return cache index.
		return res - s_cachedFilenames.begin();
	}
}

bool CMumuDebugger::isOverloadedOp(CONST_POS mu)
{
	if ((mu->udt & UDT_FUNC) && s_overloadableOps.count(mu->func))
	{
		assert(m_program.m_stack.size() && m_program.m_stack.back().size() >= mu->params && mu->params > 0);
		// Read the lhs operand off the stack.
		LPSTACK_FRAME lhs = &m_program.m_stack.back().back() - (mu->params - 1);
		if (lhs->getType() & UDT_OBJ)
		{
			// Locate class.
			const unsigned int obj = static_cast<unsigned int>(lhs->getNum());
			const STRING &type = CProgram::m_objects[obj];
			std::map<STRING, tagClass>::iterator k = m_program.m_classes.find(type);
			if (k == m_program.m_classes.end())
			{
				return false;
			}

			// Check if class overloads the specified operator.
			const STRING method = _T("operator") + s_overloadableOps[mu->func];
			const CLASS_VISIBILITY cv = (m_program.m_calls.size() && 
				(CProgram::m_objects[m_program.m_calls.back().obj] == type)) ? CV_PRIVATE : CV_PUBLIC;
			if (k->second.locate(method, mu->params - 1, cv))
			{
				return true;
			}
		}
	}
	return false;
}


bool CMumuDebugger::isExecutionUnit(CONST_POS mu)
{
	if (mu->udt & UDT_FUNC)
	{
		STRING functionName = CProgram::getFunctionName(mu->func);

		return mu->func == CProgram::methodCall || s_assignmentOps.count(mu->func) || isOverloadedOp(mu) || 
			(!isOperator(functionName) && (mu->func != CProgram::skipMethod) && (mu->func != CProgram::skipClass));
	}
	return false;
}

void CMumuDebugger::drawCommandStrip()
{
	const PanelStyle &panel = m_panels[ptCommandStrip];
	const int totalWidth = panel.rect.right - panel.rect.left - 20;
	const int segWidth = totalWidth / kMaxFnKeys;

	panel.draw(m_canvas);
	HDC hdc = m_canvas.OpenDC();
	SetBkMode(hdc, TRANSPARENT);

	HPEN pen = CreatePen(PS_SOLID, 1, panel.dimColor);
	HPEN oldPen = (HPEN)SelectObject(hdc, pen);
	HFONT oldFont = (HFONT)SelectObject(hdc, panel.font);

	RECT r = { panel.rect.left + 5, panel.rect.top, panel.rect.left + segWidth, panel.rect.bottom };

	for (int i = 0; i < kMaxFnKeys; ++i)
	{
		MoveToEx(hdc, r.right, r.top, NULL);
		LineTo(hdc, r.right, r.bottom);
		SetTextColor(hdc, m_fnEnabled[i] ? panel.foreColor : panel.dimColor);
		DrawText(hdc, m_fnCaptions[i].c_str(), -1, &r, DT_LEFT | DT_WORDBREAK);
		OffsetRect(&r, segWidth, 0);
	}

	SelectObject(hdc, oldPen);
	SelectObject(hdc, oldFont);
	m_canvas.CloseDC(hdc);

	DeleteObject(pen);
}

void CMumuDebugger::fileStrip_arrangeFilenames()
{
	STRING current = removePath(s_cachedFilenames[m_mu->fileIndex]);
	std::list<STRING>::const_iterator res = std::find(m_filenames.begin(), m_filenames.end(), current);

	if (res == m_filenames.end())
	{
		// Insert the active program to the front of the list
		m_filenames.push_front(current); 
	}
	else
	{
		if (res != m_filenames.begin())
		{
			// Shift the active program to the front of the list
			m_filenames.push_front(*res); //<- doesn't invalidate iterator
			m_filenames.erase(res);
		}
	}
}

void CMumuDebugger::drawFileStrip()
{
	const int kLeftMargin = 3;
	m_panels[ptFileStrip].draw(m_canvas);

	if (m_mu->fileIndex >= 0 && m_mu->fileIndex < s_cachedFilenames.size())
	{
		HDC hdc = m_canvas.OpenDC();
		HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptFileStrip].font);
		SetTextColor(hdc, m_panels[ptFileStrip].foreColor);
		SetBkMode(hdc, TRANSPARENT);

		SIZE sz = {0};
		RECT r = m_panels[ptFileStrip].rect;
		r.left += kLeftMargin;

		// Draw the active filename first (in bold)
		fileStrip_arrangeFilenames();
		const STRING &front = m_filenames.front();
		DrawText(hdc, front.c_str(), front.length(), &r, DT_SINGLELINE | DT_VCENTER);
		GetTextExtentPoint32(hdc, front.c_str(), front.length(), &sz);
		r.left += sz.cx + kFileStripSpacing;

		// Draw the remaining filenames
		SelectObject(hdc, m_panels[ptFileStrip].dimFont);
		SetTextColor(hdc, m_panels[ptFileStrip].dimColor);

		for (std::list<STRING>::iterator it = ++m_filenames.begin();
			it != m_filenames.end() && r.left < m_panels[ptFileStrip].rect.right;
			++it)
		{
			DrawText(hdc, it->c_str(), it->length(), &r, DT_SINGLELINE | DT_VCENTER);
			GetTextExtentPoint32(hdc, it->c_str(), it->length(), &sz);
			r.left += sz.cx + kFileStripSpacing;
		}

		SelectObject(hdc, oldFont);
		m_canvas.CloseDC(hdc);
	}
}

void CMumuDebugger::codePanel_drawBreakpoints(int firstLine, int lastLine, HDC hdc)
{
	SelectClipRgn(hdc, NULL);
	const std::set<int> &breakpointLines = s_breakpoints[m_fileIndex];

	for (int i = firstLine; i <= lastLine; ++i)
	{
		if (breakpointLines.count(i))
		{
			const int y = m_panels[ptCode].rect.top + (i - firstLine + 1) * kLineHeight;

			RECT r = {
				m_panels[ptLineNumbers].rect.left,
				y - 2,
				m_panels[ptCode].rect.right,
				y + 0
			};

			if (y < m_panels[ptCode].rect.bottom)
			{
				FillRect(hdc, &r, m_breakpointBrush);
				SetRect(&r, r.left + 2, y - 10, r.left + 6, y - 6);
				FillRect(hdc, &r, m_breakpointBrush);
			}
		}
	}
}

void CMumuDebugger::codePanel_drawHighlights(int firstLine, int lastLine)
{
	// Highlight the selected line.
	int y = m_panels[ptCode].rect.top + (m_selectedLine - firstLine) * kLineHeight;
	m_cnvLineOverlay.ClearScreen(kSelectedLineColor);
	m_cnvLineOverlay.BltTranslucent(&m_canvas, 0, y, kOverlayTranslucency, -1, -1);

	if (m_selectedLine != m_mu->line && m_fileIndex == m_mu->fileIndex && m_mu->line >= firstLine && 
		m_mu->line <= lastLine)
	{
		// Highlight the line attached to the active MU
		y = m_panels[ptCode].rect.top + (m_mu->line - firstLine) * kLineHeight;

		if (y + kLineHeight < m_panels[ptCode].rect.bottom)
		{
			m_cnvLineOverlay.ClearScreen(kActiveLineColor);
			m_cnvLineOverlay.BltTranslucent(&m_canvas, 0, y, kOverlayTranslucency, -1, -1);
		}
	}
}

void CMumuDebugger::codePanel_drawLineNumbers(int firstLine, int lastLine, HDC hdc)
{
	HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptLineNumbers].font);
	SelectClipRgn(hdc, m_panels[ptLineNumbers].rgn);
	SetTextColor(hdc, m_panels[ptLineNumbers].foreColor);

	RECT r = {
		m_panels[ptLineNumbers].rect.left, 
		m_panels[ptLineNumbers].rect.top, 
		m_panels[ptLineNumbers].rect.right,
		m_panels[ptLineNumbers].rect.top + kLineHeight
	};

	for (int i = firstLine; i <= lastLine; ++i)
	{
		TCHAR buf[12];
		_itoa_s(i, buf, 12, 10);
		DrawText(hdc, buf, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
		OffsetRect(&r, 0, kLineHeight);
	}

	SelectObject(hdc, oldFont);
}

void CMumuDebugger::codePanel_drawCodeText(int firstLine, int lastLine, HDC hdc)
{
	HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptCode].font);
	SelectClipRgn(hdc, m_panels[ptCode].rgn);
	SetTextColor(hdc, m_panels[ptCode].foreColor);

	RECT r = {
		m_panels[ptCode].rect.left + kCodePanelLeftMargin, 
		m_panels[ptCode].rect.top, 
		m_panels[ptCode].rect.right,
		m_panels[ptCode].rect.top + kLineHeight, 
	};

	for (int i = firstLine; i <= lastLine; ++i)
	{
		DrawText(hdc, s_cachedText[m_fileIndex][i].c_str(), -1, &r, 
			DT_VCENTER | DT_SINGLELINE | DT_EXPANDTABS | DT_TABSTOP | (kTabSize << 8));
		OffsetRect(&r, 0, kLineHeight);
	}

	SelectObject(hdc, oldFont);
}

void CMumuDebugger::drawCode()
{
	m_panels[ptLineNumbers].draw(m_canvas);
	m_panels[ptCode].draw(m_canvas);

	int firstLine = 0, lastLine = 0;
	bool invalid = 
		m_fileIndex < 0 || m_selectedLine < 1 || m_selectedLine >= s_cachedText[m_fileIndex].size();
	
	HDC hdc = m_canvas.OpenDC();
	HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptCode].font);
	SetBkMode(hdc, TRANSPARENT);
	SetTextColor(hdc, m_panels[ptCode].foreColor);

	if (invalid)
	{
		DrawText(hdc, "(Unable to locate source.)", -1, &m_panels[ptCode].rect, 
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	else
	{
		int visibleLines = (m_panels[ptCode].rect.bottom - m_panels[ptCode].rect.top) / kLineHeight;
		firstLine = max(1, m_selectedLine - visibleLines / 2);
		lastLine = (m_selectedLine < visibleLines / 2) ?
			min(s_cachedText[m_fileIndex].size() - 1, visibleLines) :
			min(s_cachedText[m_fileIndex].size() - 1, m_selectedLine + visibleLines / 2);

		codePanel_drawCodeText(firstLine, lastLine, hdc);
		codePanel_drawLineNumbers(firstLine, lastLine, hdc);
		codePanel_drawBreakpoints(firstLine, lastLine, hdc);
	}

	SelectClipRgn(hdc, NULL);
	SelectObject(hdc, oldFont);
	m_canvas.CloseDC(hdc);

	if (!invalid)
		codePanel_drawHighlights(firstLine, lastLine);
}

void CMumuDebugger::mUnitsPanel_populateRow(MACHINE_UNITS::size_type idx,
											STRING (&cells)[kMuColumnCount],
											std::bitset<kMuColumnCount> &dimmed)
{
	const int kMaxLiteralLength = 50;
	const int kMaxNumPrecision = 6;
	const MACHINE_UNIT &mu = m_program.m_units[idx];

	TCHAR buf[12];
	MACHINE_UNITS::difference_type activeIdx = m_mu - m_program.m_units.begin();
	dimmed.reset();

	// 0:"#", 1:"LIT", 2:"NUM", 3:"UDT", 4:"FUNC", 5:"P#"
	_itoa_s(idx, buf, 12, 10);
	cells[0] = buf;
	if (idx == activeIdx) cells[0].insert(0, "*");

	cells[1] = mu.lit.substr(0, kMaxLiteralLength);

	STRINGSTREAM ss;
	ss << std::setprecision(kMaxNumPrecision) << mu.num;
	cells[2] = ss.str();
	dimmed[2] = !(mu.udt & UDT_NUM) && !(mu.udt & UDT_CLOSE) && !(mu.udt & UDT_OPEN);

	cells[3] = getShortUnitDataType(mu.udt);
	cells[4] = CProgram::getFunctionName(mu.func);

	_itoa_s(mu.params, buf, 12, 10);
	cells[5] = buf;
	dimmed[5] = !(mu.udt & UDT_FUNC);
}

void CMumuDebugger::mUnitsPanel_drawColumns(HDC hdc)
{
	const PanelStyle &panel = m_panels[ptMUnits];

	RECT r = {
		panel.rect.left,
		panel.rect.top + kMuTopMargin,
		panel.rect.left - kMuColumnSpacing,
		panel.rect.top + kMuTopMargin + kMuLineHeight
	};

	// Draw a line under fixed header
	MoveToEx(hdc, panel.rect.left, r.bottom, NULL);
	LineTo(hdc, panel.rect.right, r.bottom);

	// Draw column headers + lines
	HFONT oldFont = (HFONT)SelectObject(hdc, panel.altFont);
	SetTextColor(hdc, panel.foreColor);
	for (int i = 0; i < kMuColumnCount; ++i)
	{
		r.left = r.right + kMuColumnSpacing;
		r.right = r.left + kMuColumnWidths[i];
		DrawText(hdc, kMuColumnCaptions[i].c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		// Draw a line between each column
		MoveToEx(hdc, r.right + kMuColumnSpacing / 2, panel.rect.top, NULL);
		LineTo(hdc, r.right + kMuColumnSpacing / 2, panel.rect.bottom);
	}
	SelectObject(hdc, oldFont);
}

void CMumuDebugger::mUnitsPanel_drawRows(HDC hdc)
{
	const PanelStyle &panel = m_panels[ptMUnits];
	HFONT oldFont = (HFONT)SelectObject(hdc, panel.font);

	RECT r = {
		panel.rect.left,
		panel.rect.top + kMuTopMargin,
		panel.rect.left - kMuColumnSpacing,
		panel.rect.top + kMuTopMargin + kMuLineHeight
	};

	MACHINE_UNITS::difference_type activeIdx = m_mu - m_program.m_units.begin();
	int visibleLines = (panel.rect.bottom - (panel.rect.top + kMuTopMargin)) / kMuLineHeight - 1;
	int firstIdx = max(0, activeIdx - visibleLines / 2);
	int lastIdx = (activeIdx < visibleLines / 2) ?
		min(m_program.m_units.size() - 1, visibleLines) :
		min(m_program.m_units.size() - 1, activeIdx + visibleLines / 2);

	for (MACHINE_UNITS::size_type i = firstIdx; i <= lastIdx; ++i)
	{
		OffsetRect(&r, 0, kMuLineHeight);
		r.left = panel.rect.left;
		r.right = r.left - kMuColumnSpacing;

		// Collect row data... 
		STRING cells[kMuColumnCount];
		std::bitset<kMuColumnCount> dimmed;
		mUnitsPanel_populateRow(i, cells, dimmed);

		// Draw a line under each row
		MoveToEx(hdc, panel.rect.left, r.bottom, NULL);
		LineTo(hdc, panel.rect.right, r.bottom);

		// Draw row cells
		for (int j = 0; j < kMuColumnCount; ++j)
		{
			if (dimmed[j])
				SetTextColor(hdc, panel.dimColor);
			else
				SetTextColor(hdc, i == activeIdx ? panel.litColor : panel.foreColor);

			r.left = r.right + kMuColumnSpacing;
			r.right = r.left + kMuColumnWidths[j];
			DrawText(hdc, cells[j].c_str(), -1, &r,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_MODIFYSTRING | DT_END_ELLIPSIS);
		}
	}
	SelectObject(hdc, oldFont);
}

void CMumuDebugger::drawMUnits()
{
	m_panels[ptMUnits].draw(m_canvas);

	HDC hdc = m_canvas.OpenDC();
	SelectClipRgn(hdc, m_panels[ptMUnits].rgn);
	SetBkMode(hdc, TRANSPARENT);

	HPEN gridPen = CreatePen(PS_SOLID, 1, m_panels[ptMUnits].borderColor);
	HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
	
	mUnitsPanel_drawColumns(hdc);
	mUnitsPanel_drawRows(hdc);

	SelectObject(hdc, oldPen);
	DeleteObject((HGDIOBJ)gridPen);
	SelectClipRgn(hdc, NULL);
	m_canvas.CloseDC(hdc);
}

void CMumuDebugger::infoPanel_drawWatchItem(std::vector<STRING>::size_type idx, RECT r, HDC hdc)
{
	const bool kVerbose = true;
	//HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptInfo].altFont);
	SetTextColor(hdc, m_panels[ptInfo].foreColor);
	STRING watch = getWatch(idx);
	STRINGSTREAM ss;
	ss << (idx + 1) << _T(". ") << watch;
	DrawText(hdc, ss.str().c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER);
	OffsetRect(&r, 0, kInfoPanelLineHeight);
	//SelectObject(hdc, oldFont);

	if (watch.length())
	{
		VariableParser vp(m_program, watch);
		SetTextColor(hdc, m_panels[ptInfo].litColor);
		STRING s = _T("   ") + vp.resolve(false, kVerbose);
		DrawText(hdc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER);
	}
}

void CMumuDebugger::infoPanel_drawWatchList(HDC hdc)
{
	RECT r = {
		m_panels[ptInfo].rect.left, 
		m_panels[ptInfo].rect.top, 
		m_panels[ptInfo].rect.right,
		m_panels[ptInfo].rect.top + kInfoPanelLineHeight, 
	};

	HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptInfo].font);
	SetTextColor(hdc, m_panels[ptInfo].foreColor);
	DrawText(hdc, "Watch List", -1, &r, DT_SINGLELINE | DT_CENTER | DT_BOTTOM);
	line(r.left, r.bottom, r.right, r.bottom, m_panels[ptInfo].borderColor, hdc);
	r.left += kInfoPanelLeftMargin;

	for (std::vector<STRING>::size_type i = 0; i != getMaxWatches(); ++i)
	{
		OffsetRect(&r, 0, kInfoPanelLineHeight * 2);
		infoPanel_drawWatchItem(i, r, hdc);
	}
	SelectObject(hdc, oldFont);
}

void CMumuDebugger::infoPanel_drawStackTrace(HDC hdc)
{
	const int kLineHeight = 13, kLevels = 3;

	RECT r = {
		m_panels[ptInfo].rect.left, 
		m_panels[ptInfo].rect.bottom - 4 - kLineHeight, 
		m_panels[ptInfo].rect.right,
		m_panels[ptInfo].rect.bottom - 4, 
	};

	if (!m_program.m_calls.empty())
	{
		HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptInfo].dimFont);
		SetTextColor(hdc, m_panels[ptInfo].foreColor);

		std::vector<CALL_FRAME>::size_type start = max(0, int(m_program.m_calls.size()) - kLevels);

		for (std::vector<CALL_FRAME>::size_type i = start;
			i != m_program.m_calls.size();
			++i)
		{
			STRINGSTREAM ss;
			ss << (i + 1) << _T(": ");

			const int methodIdx = m_program.m_calls[i].methodIdx;
			if (methodIdx >= 0 && methodIdx < m_program.m_methods.size())
				ss << m_program.getMethod(methodIdx).name << _T("()");
			else
				ss << _T("n/a");
				
			if (i == m_program.m_calls.size() - 1)
			{
				HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptInfo].altFont);
				DrawText(hdc, ss.str().c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
				SelectObject(hdc, oldFont);
			}
			else
			{
				DrawText(hdc, ss.str().c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
			}
			OffsetRect(&r, 0, -kLineHeight);
		}
		SelectObject(hdc, oldFont);
	}
}

void CMumuDebugger::drawInfo()
{
	m_panels[ptInfo].draw(m_canvas);

	HDC hdc = m_canvas.OpenDC();
	SetBkMode(hdc, TRANSPARENT);

	infoPanel_drawWatchList(hdc);
	infoPanel_drawStackTrace(hdc);

	m_canvas.CloseDC(hdc);
}

void CMumuDebugger::drawView()
{
	m_canvas.ClearScreen(0);

	for (std::map<PanelType, PanelStyle>::const_iterator it = m_panels.begin(); it != m_panels.end(); ++it)
	{
		it->second.draw(m_canvas);
	}

	drawCommandStrip();
	drawCode();
	drawFileStrip();
	drawMUnits();
	drawInfo();

	refreshScreen();
}

void CMumuDebugger::refreshScreen()
{
	g_pDirectDraw->DrawCanvas(&m_canvas, 0, 0, SRCCOPY);
	g_pDirectDraw->Refresh();
}

void CMumuDebugger::clearWatches()
{
	for (std::vector<STRING>::iterator it = m_watches.begin(); it != m_watches.end(); ++it)
	{
		it->clear();
	}
	m_watchIndex = 0;
}

void CMumuDebugger::showMessage(const STRING &message)
{
	messageBox(message);
}

void CMumuDebugger::watchPrompt_drawInput(const STRING &text, const STRING &defaultText,
										  const STRING &parsed, RECT r, HDC hdc)
{
	const bool kShowParsed = true;
	if (text.empty())
	{
		SetTextColor(hdc, m_panels[ptCommandStrip].dimColor);
		DrawText(hdc, defaultText.c_str(), -1, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE );
	}
	else
	{
		SetTextColor(hdc, m_panels[ptCommandStrip].foreColor);
		DrawText(hdc, text.c_str(), -1, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE );

		if (kShowParsed)
		{
			SetTextColor(hdc, m_panels[ptCommandStrip].dimColor);
			DrawText(hdc, parsed.c_str(), -1, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
		}
	}
}

void CMumuDebugger::watchPrompt_drawAutocomplete(const std::vector<STRING> &results, RECT r, HDC hdc)
{
	for (std::vector<STRING>::const_iterator it = results.begin(); it != results.end(); ++it)
	{
		watchPrompt_drawAutocompleteItem(*it, r, hdc);
		OffsetRect(&r, 0, kCommandWindowLineHeight);
	}
}

void CMumuDebugger::watchPrompt_drawAutocompleteItem(const STRING &text, RECT r, HDC hdc)
{
	SetTextColor(hdc, m_panels[ptCommandStrip].dimColor);
	DrawText(hdc, text.c_str(), -1, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
	SetTextColor(hdc, m_panels[ptCommandStrip].litColor);
	r.left += 200;
	DrawText(hdc, VariableParser(m_program, text).resolve(false, true).c_str(), -1, &r,
		DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_MODIFYSTRING | DT_END_ELLIPSIS);
}

void CMumuDebugger::showWatchPrompt(int watchIdx)
{
	const int kMargin = 100, kTextMarginSize = 14;
	const int kTextOffset = kMargin + kTextMarginSize;

	if (watchIdx >= getMaxWatches())
		return;

	VariableParser parser(m_program);
	STRING input = getWatch(watchIdx);

	TCHAR buf[12];
	_itoa_s(watchIdx + 1, buf, 12, 10);
	STRING defaultText = "Input watch #" + STRING(buf); 
	
	while (true)
	{
		int maxResults = max(1, (g_resY - kTextOffset * 2) / kCommandWindowLineHeight - 1); 
		std::vector<STRING> results = parser.getAutocomplete(input, maxResults);

		drawCommandWindow(kMargin);
		RECT r = { kTextOffset, kTextOffset, g_resX - kTextOffset, kTextOffset + kCommandWindowLineHeight };
		m_canvas.DrawLine(r.left, r.bottom, r.right, r.bottom, m_panels[ptCommandStrip].dimColor);

		HDC hdc = m_canvas.OpenDC();
		HFONT oldFont = (HFONT)SelectObject(hdc, m_panels[ptCommandStrip].altFont);
		SetBkMode(hdc, TRANSPARENT);

		watchPrompt_drawInput(input, defaultText, parser.parse(input), r, hdc);
		OffsetRect(&r, 0, kCommandWindowLineHeight);
		watchPrompt_drawAutocomplete(results, r, hdc);

		SelectObject(hdc, oldFont);
		m_canvas.CloseDC(hdc);
		refreshScreen();

		//TODO: Use up/down to choose entry
		STRING key = waitForKey(false);
		if (key.length() == 1 && isprint(key[0]))
		{
			input.push_back(key[0]);
		}
		else if (input.length() && key == _T("BACKSPACE"))
		{
			input.resize(input.length() - 1);
		}
		else if (results.size() && key == _T("TAB"))
		{
			input = results.front();
		}
		else if (key == _T("ENTER"))
		{
			setWatch(watchIdx, input);
			break;
		}
		else if (key == _T("DELETE"))
		{
			clearWatch(watchIdx);
			break;
		}
		else if (key == _T("ESC"))
		{
			break;
		}	
	}

	drawView();
}

void CMumuDebugger::drawCommandWindow(int margin, int borderSize)
{
	int innerMargin = margin + borderSize;
	m_canvas.DrawFilledRect(margin, margin, g_resX - margin, g_resY - margin, m_panels[ptCommandStrip].backColor);
	m_canvas.DrawRect(margin, margin, g_resX - margin, g_resY - margin, m_panels[ptCommandStrip].dimColor);
	m_canvas.DrawRect(innerMargin, innerMargin, g_resX - innerMargin, g_resY - innerMargin, m_panels[ptCommandStrip].dimColor);
}

void CMumuDebugger::showActionMenu()
{
	const int kMargin = 100, kTextOffset = 20;
	const int kTextMargin = kMargin + kTextOffset;

	drawCommandWindow(kMargin);
	std::vector<STRING> options;
	CCursorMap map;

	options.push_back("Cancel");
	options.push_back("Throw Exception");
	options.push_back("Dump Machine Units (CSV)");
	options.push_back("Dump Variables (CSV)");
	options.push_back("Dump Canvases");
	options.push_back("Dump All");
	options.push_back("Clear Breakpoints");
	options.push_back("Clear Watches");
	options.push_back("Disable MuMu");

	for (std::vector<STRING>::size_type i = 0; i < options.size(); ++i)
	{
		m_canvas.DrawTextA(kTextMargin, kTextMargin + kCommandWindowLineHeight * i, options[i], 
			"Lucida Console", 14, m_panels[ptCommandStrip].foreColor);
		map.add(kTextMargin, kTextMargin + kCommandWindowLineHeight * i + 7);
	}
	refreshScreen();

	int res = map.run();
	switch (res)
	{
	case 0: // Cancel
		break;
	case 1: // Throw Exception
		DebugBreak();
		break;
	case 2:
		showMessage(dumpMachineUnits() ? "DUMP OK" : "FAILED");
		break;
	case 3:
		showMessage(dumpVariables() ? "DUMP OK" : "FAILED");
		break;
	case 4:
		showMessage(dumpCanvases() ? "DUMP OK" : "FAILED");
		break;
	case 5:
		showMessage(dumpAll() ? "DUMP OK" : "FAILED");
		break;
	case 6:
		clearBreakpoints(); //<- (for each cached file)
		break;
	case 7:
		clearWatches();
		break;
	case 8: // Disable MuMu (Note: will be hidden, but not fully disabled until next program)
		disable();
		break;
	}
	drawView();
}

void CMumuDebugger::clearBreakpoints()
{
	for (std::vector< std::set<int> >::iterator it = s_breakpoints.begin();
		it != s_breakpoints.end();
		++it)
	{
		it->clear();
	}
}

STRING CMumuDebugger::getDumpDir(const STRING &subdir)
{
	extern STRING g_projectPath;
	STRING path = g_projectPath + _T("_MUMU\\");
	if (!PathIsDirectory(path.c_str()))
	{
		CreateDirectory(path.c_str(), NULL);
	}
	if (!subdir.empty())
	{
		path += subdir + _T("\\");
		if (!PathIsDirectory(path.c_str()))
		{
			CreateDirectory(path.c_str(), NULL);
		}
	}
	return path;
}

bool CMumuDebugger::dumpAll()
{
	return dumpMachineUnits() && dumpVariables() && dumpCanvases();
}

bool CMumuDebugger::dumpCanvas(const CCanvas &cnv, const STRING &filename)
{
	// TODO: Error-handling
	bool res = false;
	int width = cnv.GetWidth(), height = cnv.GetHeight();
	
	HDC cnvDC = cnv.OpenDC();
	HDC tmpDC = CreateCompatibleDC(cnvDC);
	HBITMAP tmpBmp = CreateCompatibleBitmap(cnvDC, width, height);
	HBITMAP oldBmp = (HBITMAP)SelectObject(tmpDC, tmpBmp);
	BitBlt(tmpDC, 0, 0, width, height, cnvDC, 0, 0, SRCCOPY);
	SelectObject(tmpDC, oldBmp); //<- Must de-select tmpBmp before passing to GetDIBits()
	cnv.CloseDC(cnvDC);

	FIBITMAP *dib = FreeImage_Allocate(width, height, 24);
	GetDIBits(tmpDC, tmpBmp, 0, height, FreeImage_GetBits(dib), FreeImage_GetInfo(dib), DIB_RGB_COLORS);
	res = FreeImage_Save(FIF_PNG, dib, filename.c_str());
	FreeImage_Unload(dib);

	DeleteObject(tmpBmp);
	DeleteDC(tmpDC);
	return res;
}

bool CMumuDebugger::dumpCanvases()
{
	extern CAllocationHeap<CCanvas> g_canvases;
	extern CCanvas *g_cnvRpgCode;
	std::set<CCanvas*> &canvases = g_canvases.getContents();
	STRING path = getDumpDir("Canvas");

	int ct = 0;
	for (std::set<CCanvas*>::iterator it = canvases.begin(); it != canvases.end(); ++it)
	{
		TCHAR buf[12];
		_itoa_s(ct, buf, 12, 10);
		if (dumpCanvas(**it, path + "cnv-" + buf + ".png"))
			ct++;
	}

	if (g_cnvRpgCode)
		dumpCanvas(*g_cnvRpgCode, path + "cnv-screen.png");

	return ct == canvases.size();
}

bool CMumuDebugger::dumpVariables()
{
	return dumpLocalVariables() && dumpGlobalVariables();
}

bool CMumuDebugger::dumpLocalVariables()
{
	extern STRING g_projectPath;
	STRING filename = getDumpDir() + _T("locals-dump.csv");
	CFile file(filename, OF_CREATE | OF_WRITE);

	if (!file.isOpen())
		return false;

	STRINGSTREAM ss;
	ss << _T("NAME,VALUE,UDT,L#\n");

	std::list< std::map<STRING, STACK_FRAME> > *pLocalList = m_program.getLocals();
	if (pLocalList)
	{
		int ct = 0;
		for (std::list< std::map<STRING, STACK_FRAME> >::const_iterator i = pLocalList->begin();
			i != pLocalList->end();
			++i)
		{
			for (std::map<STRING, STACK_FRAME>::const_iterator j = i->begin(); j != i->end(); ++j)
			{
				ss << quoteString(j->first) << _T(",");

				if (j->second.getType() & UDT_LIT)
					ss << quoteString(j->second.getLit()) << _T(",");
				else
					ss << j->second.getNum() << _T(",");
	 
				ss << quoteString(getShortUnitDataType(j->second.getType())) << _T(",") << ct << _T("\n");
			}
			ct++;
		}
	}

	file << ss.str();
	return true;
}

bool CMumuDebugger::dumpGlobalVariables()
{
	extern STRING g_projectPath;
	STRING filename = getDumpDir() + _T("globals-dump.csv");
	CFile file(filename, OF_CREATE | OF_WRITE);

	if (!file.isOpen())
		return false;

	STRINGSTREAM ss;
	ss << _T("NAME,VALUE,UDT\n");

	for (std::map<STRING, CPtrData<STACK_FRAME> >::iterator it = m_program.m_heap.begin();
		it != m_program.m_heap.end();
		++it)
	{
		ss << quoteString(it->first) << _T(",");
		
		if (it->second->getType() & UDT_LIT)
			ss << quoteString(it->second->getLit()) << _T(",");
		else
			ss << it->second->getNum() << _T(",");

		ss << quoteString(getShortUnitDataType(it->second->getType())) << _T("\n");
	}

	file << ss.str();
	return true;
}

bool CMumuDebugger::dumpMachineUnits()
{
	extern STRING g_projectPath;
	STRING filename = removePath(m_program.m_fileName);

	if (filename.empty())
	{
		TCHAR buf[12];
		_itoa_s(rand() % 0xFFFF, buf, 12, 16);
		filename = STRING(_T("tmp")) + buf;
	}

	filename = getDumpDir() + filename + _T("-dump.csv");
	CFile file(filename, OF_CREATE | OF_WRITE);

	if (!file.isOpen())
		return false;

	STRINGSTREAM ss;
	ss << _T("#,FILE#,LINE#,LIT,NUM,UDT,FUNC,PARAMS,P0,P1\n");
		
	for (MACHINE_UNITS::size_type i = 0; i < m_program.m_units.size(); ++i)
	{
		const unsigned long *const pLines = (unsigned long *)&m_program.m_units[i].num;

		ss	<< i << _T(",")
			<< m_program.m_units[i].fileIndex << _T(",")
			<< m_program.m_units[i].line << _T(",")
			<< quoteString(m_program.m_units[i].lit) << _T(",")
			<< m_program.m_units[i].num << _T(",")
			<< quoteString(getShortUnitDataType(m_program.m_units[i].udt)) << _T(",")
			<< quoteString(CProgram::getFunctionName(m_program.m_units[i].func)) << _T(",")
			<< m_program.m_units[i].params << _T(",")
			<< pLines[0] << _T(",")
			<< pLines[1] << _T("\n");
	}

	file << ss.str();
	return true;
}

void CMumuDebugger::scrollCode(int delta)
{
	if (delta > 0)
		m_selectedLine = min(m_selectedLine + delta, s_cachedText[m_fileIndex].size() - 1);
	else
		m_selectedLine = max(m_selectedLine + delta, 1);

	bool enableRunTo = m_selectedLine != m_mu->line || m_fileIndex != m_mu->fileIndex;
	if (m_fnEnabled[9-1] != enableRunTo)
	{
		m_fnEnabled[9-1] = enableRunTo;
		drawCommandStrip();
	}
	drawCode();
	refreshScreen();
}

int CMumuDebugger::getNextWatchSlot()
{
	for (std::vector<STRING>::size_type i = 0; i != m_watches.size(); ++i)
	{
		if (m_watches[i].empty())
			return i;
	}
	// When all slots are filled, use a wrapping index:
	int res = m_watchIndex++;
	m_watchIndex %= m_watches.size();
	return res;
}

void CMumuDebugger::disable()
{
	CProgram::m_enableMumu = false;
	m_hidden = true;
	setStep(smNone);
	clearBreakpoints();
}

void CMumuDebugger::setStep(SteppingMode mode, int file, int line)
{
	assert(m_mu != m_program.m_units.end());

	m_steppingMode = mode;
	m_fromUnit = m_mu;
	m_fromStackIndex = m_program.m_stackIndex;
	m_toFileIndex = file;
	m_toLine = line;
}

void CMumuDebugger::handleInput()
{
	while (true)
	{
		const STRING key = waitForKey(false);

		if (key.length() == 1 && key[0] >= _T('1') && key[0] <= _T('9'))
		{
			showWatchPrompt(key[0] - _T('1'));
		}

		if (key == _T("0") || key == _T("`"))
			showWatchPrompt(getNextWatchSlot());

		// Not implemented
		if (key == "F1" && m_fnEnabled[1-1]) { }
		if (key == "F2" && m_fnEnabled[2-1]) { }
		if (key == "F3" && m_fnEnabled[3-1]) { }
		if (key == "F4" && m_fnEnabled[4-1]) { }

		// Run
		if (key == "F5" && m_fnEnabled[5-1])
		{
			setStep(smNone);
			break;
		}

		// Step Out
		if (key == "F6" && m_fnEnabled[6-1])
		{
			setStep(smStepOut);
			break;
		}

		// Step Over
		if (key == "F7" && m_fnEnabled[7-1])
		{
			setStep(smStepOver);
			break;
		}

		// Step Into
		if ((key == "F8" || key == "TAB") && m_fnEnabled[8-1])
		{
			setStep(smStepInto);
			break;
		}

		// Run To selected line
		if ((key == "F9" || key == "ENTER") && m_fnEnabled[9-1])
		{
			setStep(smStepTo, m_fileIndex, m_selectedLine);
			break;
		}

		// Windows hogs F10, and it's not worth a workaround. Using SPACE instead!
		//if (key == "F10" && m_fnEnabled[10-1]) { }

		if (key == "F11" && m_fnEnabled[11-1])
		{
			showActionMenu();
			if (m_hidden) break;
		}

		// Run
		if (key == "F12" && m_fnEnabled[12-1])
		{
			m_hidden = true;
			setStep(smNone);
			break;
		}

		// Toggle Breakpoint for selected line
		if (key == " ")
		{
			if (m_fileIndex >= 0 && m_selectedLine > 0)
			{
				assert(m_fileIndex < s_breakpoints.size());
				std::set<int> &breakpointLines = s_breakpoints[m_fileIndex];

				if (breakpointLines.count(m_selectedLine))
					breakpointLines.erase(m_selectedLine);
				else
					breakpointLines.insert(m_selectedLine);

				drawCode();
				refreshScreen();
			}
		}

		// Scrolling
		if (m_fileIndex >= 0)
		{
			if (key == "DOWN")
				scrollCode(+1);
			else if (key == "UP")
				scrollCode(-1);
			else if (key == "PGDN")
				scrollCode(+15);
			else if (key == "PGUP")
				scrollCode(-15);
			else if (key == "HOME")
				scrollCode(-999000); // LOL
			else if (key == "END")
				scrollCode(+999000);
		}
	} // while (true)
}

void CMumuDebugger::display(CONST_POS mu)
{
	if (m_hidden) return;

	m_mu = mu;
	m_fileIndex = m_mu->fileIndex;
	m_selectedLine = m_mu->line;

	drawView();
	handleInput();
}

void CMumuDebugger::processStyleDirective(const STRING &cmd)
{
	// ".style:p,c,rrggbb"  or  ".style:p,f,#"
	if (cmd.length() < 12) return;

	PanelType panel = static_cast<PanelType>(atoi(cmd.substr(7, 1).c_str()));
	if (m_panels.count(panel))
	{
		int which = atoi(cmd.substr(9, 1).c_str());
		STRING value = cmd.substr(11);
		if (value.length() == 6)
		{
			COLORREF color = hexToColor(value);
			switch (which)
			{
				case 0: m_panels[panel].backColor = color; break;
				case 1: m_panels[panel].foreColor = color; break;
				case 2: m_panels[panel].litColor = color; break;
				case 3: m_panels[panel].dimColor = color; break;
				case 4: m_panels[panel].borderColor = color; break;
			}
		}
		else
		{
			int fontIdx = atoi(cmd.substr(11).c_str());
			if (fontIdx >= 0 && fontIdx < s_fonts.count())
			{
				switch (which)
				{
					case 0: m_panels[panel].font = s_fonts.get(fontIdx); break;
					case 1: m_panels[panel].altFont = s_fonts.get(fontIdx); break;
					case 2: m_panels[panel].dimFont = s_fonts.get(fontIdx); break;
				}
			}
		}
	}
}

void CMumuDebugger::processDirective(const STRING &cmd)
{
	STRING s = lcase(cmd);
	if (s == _T(".pause")) 
	{
		setStep(smStepInto); //<- Trigger Breakpoint on next executable unit
	}
	else if (s == _T(".auto"))
	{
		// Automatically display for all programs
		s_autoDisplay = true;
		setStep(smStepInto);
	}
	else if (s.length() >= 8 && s.substr(0, 7) == _T(".watch:"))
	{
		addWatch(s.substr(7, STRING::npos));
		// TODO: Skip duplicates?
		// TODO: For full compatibility, array string keys need to retain their case
	}
	else if (s == _T(".disable"))
	{
		// MuMu will be hidden immediately, then disabled for future programs
		disable();
	}
	else if (s.length() >= 12 && s.substr(0, 7) == _T(".style:"))
	{
		// Customize style (somewhat)
		processStyleDirective(s);
	}
}

void CMumuDebugger::update(CONST_POS mu)
{
	// Check if the current unit is a MuMu directive. 
	if (mu->udt == (UDT_LIT|UDT_LINE) && lcase(mu->lit.substr(0, 5)) == _T("mumu."))
	{
		processDirective(mu->lit.substr(4));
	}

	if (mu->fileIndex >= 0 && 
		!s_breakpoints[mu->fileIndex].empty() &&		//<- Redundant, so as to short-circuit on most common case
		s_breakpoints[mu->fileIndex].count(mu->line) &&	//   and cut down on the higher complexity count() calls
		isExecutionUnit(mu)) //<- Also redundant! But we're about to pause, anyway...
	{
		m_steppingMode = smStepInto; //<-- Trigger on next executable unit (which is now!)
	}

	if (m_steppingMode != smNone && isExecutionUnit(mu))
	{
		if ((m_steppingMode == smStepInto) ||
			(m_steppingMode == smStepOver && mu->line != m_fromUnit->line && m_program.m_stackIndex <= m_fromStackIndex) ||
			(m_steppingMode == smStepOut && m_program.m_stackIndex < m_fromStackIndex) ||
			(m_steppingMode == smStepTo && mu->line == m_toLine && mu->fileIndex == m_fileIndex))
		{
			m_steppingMode = smNone;
			display(mu);
		}
	}
}

#endif
