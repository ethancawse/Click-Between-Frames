#pragma once

#include <bitset>
#include <array>
#include <atomic>
#include <vector>
#include <cstddef>

#include <Geode/Geode.hpp>

using namespace geode::prelude;

using BindMask = std::array<std::bitset<256>, 6>;

using TimestampType = int64_t;
TimestampType getCurrentTimestamp();

enum GameAction : int {
	p1Jump = 0,
	p1Left = 1,
	p1Right = 2,
	p2Jump = 3,
	p2Left = 4,
	p2Right = 5
};

enum State : bool {
	Release = 0,
	Press = 1
};

struct InputEvent {
	TimestampType time;
	PlayerButton inputType;
	bool inputState;
	bool isPlayer1;
};

struct Step {
	InputEvent input;
	double deltaFactor;
	bool endStep;
};
extern std::vector<InputEvent> inputQueueCopy;
extern std::vector<Step> stepQueue;
extern size_t inputIdx;
extern size_t stepIdx;

extern std::atomic<bool> enableRightClick;
// true -> cbf disabled, confusing i know
extern std::atomic<bool> softToggle;

extern BindMask g_bindMaskA;
extern BindMask g_bindMaskB;
extern std::atomic<BindMask*> g_bindMask;

extern std::atomic<uint32_t> g_resetGen;

extern bool threadPriority;
extern bool disablePriorityBoost;
extern bool mmcssGames;

#if defined(GEODE_IS_WINDOWS)
// some windows only global variables
#include "windows.hpp"
#else
extern TimestampType pendingInputTimestamp;
#endif