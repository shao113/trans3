/*
 ********************************************************************
 * The RPG Toolkit, Version 3
 * This file copyright (C) 2006  Colin James Fitzpatrick
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

#include "CProgram.h"
#include "../movement/CPlayer/CPlayer.h"
#include "../common/board.h"
#include "../audio/CAudioSegment.h"
#include "../misc/misc.h"
#include <sstream>

// These classes cannot have any of their own members, as their
// deconstructors are never called.

inline STRING getLit(const double num)
{
	// Just cast the number to a string.
	std::stringstream ss;
	ss << num;
	return ss.str();
}

// <layer, <x, y>>
std::pair<int, std::pair<int, int> > getPlayerLocation(int tag)
{
	extern std::vector<CPlayer *> g_players;
	extern LPBOARD g_pBoard;

	if (tag < 0 || tag >= g_players.size())
		return std::pair<int, std::pair<int, int> >(0, std::pair<int, int>(0, 0));

	CPlayer *pPlayer = g_players[tag];

	const SPRITE_POSITION s = g_players[tag]->getPosition();

	// Transform from pixel to board type (e.g. tile).
	int dx = int(s.x), dy = int(s.y);
	coords::pixelToTile(dx, dy, g_pBoard->coordType, false, g_pBoard->sizeX);

	return std::pair<int, std::pair<int, int> >(s.l, std::pair<int, int>(dx, dy));
}

// Reserved variable: int playerX[idx]
class CPlayerLocationX : public tagStackFrame
{
public:
	CPlayerLocationX(int idx) { num = idx; }
	double getNum() const { return getPlayerLocation(static_cast<int>(num)).second.first; }
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

// Reserved variable: int playerY[idx]
class CPlayerLocationY : public tagStackFrame
{
public:
	CPlayerLocationY(int idx) { num = idx; }
	double getNum() const { return getPlayerLocation(static_cast<int>(num)).second.second; }
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

// Reserved variable: int playerLayer[idx]
class CPlayerLocationZ : public tagStackFrame
{
public:
	CPlayerLocationZ(int idx) { num = idx; }
	double getNum() const { return getPlayerLocation(static_cast<int>(num)).first; }
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

// Reserved variable: string playerHandle[idx]
class CPlayerHandle : public tagStackFrame
{
public:
	CPlayerHandle(int idx) { num = idx; }
	double getNum() const { return atof(getLit().c_str()); }
	STRING getLit() const
	{
		extern std::vector<CPlayer *> g_players;
		int idx = static_cast<int>(num);
		return (idx >= 0 && idx < g_players.size()) ?
			g_players[idx]->name() : STRING();
	}
	UNIT_DATA_TYPE getType() const { return UDT_LIT; }
};

// Reserved variable: string music
class CPlayingMusic : public tagStackFrame
{
public:
	double getNum() const { return atof(getLit().c_str()); }
	STRING getLit() const
	{
		extern CAudioSegment *g_bkgMusic;
		return g_bkgMusic->getPlayingFile();
	}
	UNIT_DATA_TYPE getType() const { return UDT_LIT; }
};

// Reserved variable: int gameTime
class CGameTime : public tagStackFrame
{
public:
	double getNum() const
	{
		extern GAME_TIME g_gameTime;
		return g_gameTime.gameTime();
	}
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

// Reserved variable: string boardTitle[idx]
class CBoardTitle : public tagStackFrame
{
public:
	CBoardTitle(int idx) { num = idx; }
	STRING getLit() const
	{
		extern LPBOARD g_pBoard;
		int idx = static_cast<int>(num);
		return (idx >= 0 && idx < g_pBoard->layerTitles.size()) ?
			g_pBoard->layerTitles[idx] : STRING();
	}
	double getNum() const { return atof(getLit().c_str()); }
	UNIT_DATA_TYPE getType() const { return UDT_LIT; }
};

// Reserved variable: double constant[idx]
class CBoardConstant : public tagStackFrame
{
public:
	CBoardConstant(int idx) { num = idx; }
	STRING getLit() const
	{
		extern LPBOARD g_pBoard;
		int idx = static_cast<int>(num);
		return (idx >= 0 && idx < g_pBoard->constants.size()) ?
			g_pBoard->constants[idx] : STRING();
	}
	double getNum() const { return atof(getLit().c_str()); }
	UNIT_DATA_TYPE getType() const { return UDT_LIT; }
};

// Reserved variable: string boardBackground
class CBoardBackground : public tagStackFrame
{
public:
	STRING getLit() const
	{
		extern LPBOARD g_pBoard;
		return g_pBoard->battleBackground;
	}
	double getNum() const { return atof(getLit().c_str()); }
	UNIT_DATA_TYPE getType() const { return UDT_LIT; }
};

// Reserved variable: int boardSkill
class CBoardSkill : public tagStackFrame
{
public:
	double getNum() const
	{
		extern LPBOARD g_pBoard;
		return g_pBoard->battleSkill;
	}
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

// Reserved variable: int cnvRenderNow
class CCnvRenderNow : public tagStackFrame
{
public:
	double getNum() const
	{
		extern RENDER_OVERLAY g_renderNow;
		return double(int(g_renderNow.cnv));
	}
	STRING getLit() const { return ::getLit(getNum()); }
	UNIT_DATA_TYPE getType() const { return UDT_NUM; }
};

/*
 * Initialise virtual variables.
 */
void initVirtualVars()
{
	unsigned int i;
	for (i = 0; i < 5; ++i)
	{
		const STRING idx = getLit(i);

		// Player location virtual variables.
		CProgram::getGlobal(_T("playerx[") + idx + _T("]")) = new CPlayerLocationX(i);
		CProgram::getGlobal(_T("playery[") + idx + _T("]")) = new CPlayerLocationY(i);
		CProgram::getGlobal(_T("playerlayer[") + idx + _T("]")) = new CPlayerLocationZ(i);

		// Player handles.
		CProgram::getGlobal(_T("playerhandle[") + idx + _T("]")) = new CPlayerHandle(i);
	}

	// Board layer names.
	for (i = 1; i < 9; ++i)
	{
		CProgram::getGlobal(_T("boardtitle[") + getLit(i) + _T("]")) = new CBoardTitle(i);
	}

	// Board constants.
	for (i = 0; i < 11; ++i)
	{
		CProgram::getGlobal(_T("constant[") + getLit(i) + _T("]")) = new CBoardConstant(i);
	}

	// Playing music.
	CProgram::getGlobal(_T("music")) = new CPlayingMusic();

	// Seconds since game was started.
	CProgram::getGlobal(_T("gametime")) = new CGameTime();

	// Board fighting background.
	CProgram::getGlobal(_T("boardbackground")) = new CBoardBackground();

	// Board skill level.
	CProgram::getGlobal(_T("boardskill")) = new CBoardSkill();

	// cnvRenderNow overlay canvas.
	CProgram::getGlobal(_T("cnvrendernow")) = new CCnvRenderNow();
}
