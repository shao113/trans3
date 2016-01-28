//MuMu Debugger

#ifdef ENABLE_MUMU_DBG

#ifndef _CMUMUDEBUGGER_H_
#define _CMUMUDEBUGGER_H_

#include <bitset>
#include <cassert>
#include "CProgram.h"

class CMumuDebugger
{
public:
	explicit CMumuDebugger(CProgram &prg);
	~CMumuDebugger();

	static int loadProgram(const STRING &filename = CProgram::m_parsing);
	static void clearBreakpoints();

	// Accessors for Watches
	int getMaxWatches() const { assert(m_watches.size() == kMaxWatches); return m_watches.size(); }
	void setWatch(int idx, const STRING &value) { if (idx < m_watches.size() && idx >= 0) m_watches[idx] = value; }
	STRING getWatch(int idx) const { return (idx < m_watches.size() && idx >= 0) ? m_watches[idx] : STRING(); }
	void addWatch(const STRING &value) { setWatch(getNextWatchSlot(), value); }
	void clearWatch(int idx) { if (idx < m_watches.size() && idx >= 0) m_watches[idx].clear(); }
	void clearWatches();

	void update(CONST_POS mu); // Called for each Machine Unit executed by CProgram::run() 

private:
	enum SteppingMode { smNone, smStepOut, smStepInto, smStepOver, smStepTo };
	enum PanelType { ptCode, ptLineNumbers, ptCommandStrip, ptInfo, ptMUnits, ptFileStrip };
	enum PanelBorder { pbNone = 0, pbTop = 1, pbRight = 2, pbBottom = 4, pbLeft = 8, pbAll = 15 };
	
	struct PanelStyle
	{
		PanelStyle() : backColor(0), foreColor(0), litColor(0), dimColor(0), rect(), font(NULL), 
			dimFont(NULL), altFont(NULL), rgn(NULL), border(pbNone), borderColor(kDefaultBorderColor)
		{
		}

		~PanelStyle()
		{
			if (rgn)
				DeleteObject(rgn);
		}

		void draw(CCanvas &cnv) const;

		void setBounds(int x1, int y1, int x2, int y2)
		{
			SetRect(&rect, x1, y1, x2, y2);
			rgn = CreateRectRgnIndirect(&rect);
		}

		COLORREF backColor, foreColor, litColor, dimColor, borderColor;
		RECT rect;
		HRGN rgn;
		HFONT font, dimFont, altFont;
		int border; //<- Bitwise combination of PanelBorder flags
	};

	// Pretty rigid, but should be enough for simple cases
	class VariableParser
	{
	public:
		// Parses a variable in the context of /prg/; alternatively, omit /var/ and call parse() later on
		VariableParser(CProgram &prg, const STRING &var = STRING());

		STRING getParsedValue() const { return m_output; }
		STRING getObjectPart() const { return STRING(m_input.begin(), m_memberSplice_iter); }
		STRING getMemberPart() const { return STRING(m_memberSplice_iter, m_input.end()); }

		// Returns a collection of up to /maxResults/ autocomplete suggestions for /input/
		std::vector<STRING> getAutocomplete(const STRING &input, int maxResults = 11);
		
		// Parses /var/; Set /forced/ to true to force re-evaluation within current context
		STRING parse(const STRING &var, bool forced = false);

		// Gets the value of a parsed variable within current context, parsing if not already done.
		// Precedence is the same as resolveToVariable(): Params/Refs, Locals, Members, Globals;
		//
		// Omit /var/ to resolve the last parsed value. 
		// /refresh/ forces re-evaluation; /verbose/ formats the result according to type
		STRING resolve(const STRING &var, bool refresh = false, bool verbose = false);
		STRING resolve(bool refresh = false, bool verbose = false);
	
	private:
		bool isIdentifierChar(TCHAR ch) const { return isalnum(ch) || ch == _T('_'); }
		bool isIdentifierFirstChar(TCHAR ch) const { return isalpha(ch) || ch == _T('_'); }
		// For simplicity, number parsing is pretty uh, generous right now.
		bool isNumberChar(TCHAR ch) const { return isNumberFirstChar(ch) || tolower(ch) == _T('e'); }
		bool isNumberFirstChar(TCHAR ch) const { return isdigit(ch) || ch == _T('.') || ch == _T('-') || ch == _T('+'); }

		void skipWhitespace();
		STRING parseKey();
		STRING parseString(TCHAR quotChar = _T('"'));
		STRING parseNumber();
		STRING parseIdentifier();
		STRING parseVariable(bool isKey = false, STRING objPrefix = STRING());

		// Resolve a variable within current context. Precedence: Params/Refs, Locals, Members, Globals;
		LPSTACK_FRAME resolveToVariable(const STRING &formattedName) const;
		STRING resolveToString(const STRING &formattedName, bool verbose = false) const;
		STRING resolveToVerboseString(const STRING &formattedName) const;

		void searchParameters(const STRING &search, int maxResults, std::vector<STRING> &results) const;
		void searchLocals(const STRING &search, int maxResults, std::vector<STRING> &results) const;
		void searchMembers(const STRING &search, int maxResults, std::vector<STRING> &results) const;
		void searchGlobals(const STRING &search, int maxResults, std::vector<STRING> &results) const;

		STRING m_input;								// The string currently being parsed
		STRING m_output;							// The parsed result
		STRING::const_iterator m_iter;				// Current position in string, while parsing
		STRING::const_iterator m_memberSplice_iter;	// For splicing input's object part into displayed output
		CProgram &m_program;
	};

	class Fonts
	{
	public:
		Fonts();
		~Fonts();

		HFONT create(LOGFONT *lf);
		HFONT get(int idx) const { return (idx < count() && idx >= 0) ? m_fonts[idx] : NULL; }
		int count() const { return m_fonts.size(); }

		HFONT base, smaller, bold, large, arial, arialBold;
	private:
		std::vector<HFONT> m_fonts;
	};

	static const int kMaxFnKeys = 12;
	static const int kMuColumnCount = 6;
	static const int kCommandStripHeight, kInfoPanelWidth, kLineNumbersStripWidth;
	static const int kMuPanelHeight, kFileStripHeight, kMuColumnSpacing, kMuTopMargin;
	static const int kLineHeight, kMuLineHeight, kCodePanelLeftMargin, kFileStripSpacing, kTabSize;
	static const int kInfoPanelLineHeight, kInfoPanelLeftMargin, kMaxWatches, kCommandWindowLineHeight;
	
	static const COLORREF kDefaultBorderColor, kSelectedLineColor, kActiveLineColor, kBreakpointColor;
	static const double kOverlayTranslucency;

	static const int kMuColumnWidths[kMuColumnCount];
	static const STRING kMuColumnCaptions[kMuColumnCount];
	
	void display(CONST_POS mu);					// Suspends program execution and displays MuMu screen
	void drawView();							// Draws the panels associated with the current view
	void refreshScreen();						// Draws backbuffer to screen
	void handleInput();
	void scrollCode(int delta);
	void showActionMenu();
	void showWatchPrompt(int watchIdx);
	void showMessage(const STRING &message);
	void drawCommandWindow(int margin = 100, int borderSize = 4);
	void processStyleDirective(const STRING &cmd);
	void processDirective(const STRING &cmd);
	void setStep(SteppingMode mode, int file = -1, int line = -1);
	int getNextWatchSlot();						// Gets a free watch slot; wraps around if none are free.
	void disable();								// Hides the debugger for the remainder of the active program,
												// and then "detaches" the debugger from future programs
	// Draw the various sections to the backbuffer
	void drawCommandStrip();
	void drawCode();
	void drawFileStrip();
	void drawMUnits();
	void drawInfo();

	// This stuff should be moved into individual Panel classes
	void watchPrompt_drawInput(const STRING &text, const STRING &defaultText, const STRING &parsed, RECT r, HDC hdc);
	void watchPrompt_drawAutocomplete(const std::vector<STRING> &results, RECT r, HDC hdc);
	void watchPrompt_drawAutocompleteItem(const STRING &text, RECT r, HDC hdc);
	void infoPanel_drawWatchItem(std::vector<STRING>::size_type idx, RECT r, HDC hdc);
	void infoPanel_drawWatchList(HDC hdc);
	void infoPanel_drawStackTrace(HDC hdc);
	void codePanel_drawLineNumbers(int firstLine, int lastLine, HDC hdc);
	void codePanel_drawCodeText(int firstLine, int lastLine, HDC hdc);
	void codePanel_drawBreakpoints(int firstLine, int lastLine, HDC hdc);
	void codePanel_drawHighlights(int firstLine, int lastLine);
	void mUnitsPanel_populateRow(MACHINE_UNITS::size_type idx, STRING (&cells)[kMuColumnCount],
		std::bitset<kMuColumnCount> &dimmed);
	void mUnitsPanel_drawColumns(HDC hdc);
	void mUnitsPanel_drawRows(HDC hdc);
	void fileStrip_arrangeFilenames();

	STRING getDumpDir(const STRING &subdir = STRING());
	bool dumpAll();
	bool dumpMachineUnits();
	bool dumpVariables();
	bool dumpLocalVariables();
	bool dumpGlobalVariables();
	bool dumpCanvases();
	bool dumpCanvas(const CCanvas &cnv, const STRING &filename);

	void setupOps();   // Initializes the operator lookup lists
	void setupViews(); // Initializes all views and their associated panels

	bool isOverloadedOp(CONST_POS mu);  // Is this MU a call to an overloaded operator?
	bool isExecutionUnit(CONST_POS mu);	// Can we break on this MU? (Function call, assignment, etc.)

	static std::map<MACHINE_FUNC, STRING> s_assignmentOps;
	static std::map<MACHINE_FUNC, STRING> s_overloadableOps;
	static std::vector<STRING> s_cachedFilenames;
	static std::vector< std::vector<STRING> > s_cachedText;
	static Fonts s_fonts;
	static bool s_autoDisplay;				// Should MuMu automatically display for (future) programs?

	// Sets of breakpoint line numbers for each cache index
	static std::vector< std::set<int> > s_breakpoints;

	CProgram &m_program;					// Program the debugger is attached to
	CONST_POS m_mu;							// Active Machine Unit
	SteppingMode m_steppingMode;			// Current Stepping Mode
	CONST_POS m_fromUnit;					// MU to start stepping from
	int m_fromStackIndex;					// Stack index to start stepping from
	std::list<STRING> m_filenames;			// Filenames displayed on the File Strip
	int m_selectedLine;						// Selected line of displayed file
	int m_fileIndex;						// Index of displayed file
	int m_toLine, m_toFileIndex;			// Target for StepTo
	std::vector<STRING> m_watches;			// Names of variables to watch
	int m_watchIndex;						// Index of next Watch slot to assign
	bool m_hidden;							// Controls whether this instance should be hidden

	CCanvas m_canvas;						// Backbuffer
	CCanvas m_cnvLineOverlay;				// Translucent overlay used to highlight current line
	HBRUSH m_breakpointBrush;				// Brush used to designate breakpoints
	std::map<PanelType, PanelStyle> m_panels; // Ruins my neat comment alignment
	std::bitset<kMaxFnKeys> m_fnEnabled;	// Availability of function key shortcuts
	std::vector<STRING> m_fnCaptions;		// Command Strip Captions for each function key

};

#endif

#endif