#include "includes.hpp"
#include "input_ring.hpp"

#include <limits>
#include <bitset>
#include <array>
#include <atomic>

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <tulip/TulipHook.hpp>

constexpr double SMALLEST_FLOAT = std::numeric_limits<float>::min();

constexpr InputEvent EMPTY_INPUT = InputEvent {
	.time = 0,
	.inputType = PlayerButton::Jump,
	.inputState = false,
	.isPlayer1 = false,
};
constexpr Step EMPTY_STEP = Step {
	.input = EMPTY_INPUT,
	.deltaFactor = 1.0,
	.endStep = true,
};

//std::deque<struct InputEvent> inputQueue;
std::vector<InputEvent> inputQueueCopy;
std::vector<Step> stepQueue;
size_t inputIdx = 0;
size_t stepIdx = 0;

std::atomic<bool> softToggle;
std::atomic<uint32_t> g_resetGen{0};

InputEvent nextInput = EMPTY_INPUT;

TimestampType lastFrameTime;
TimestampType currentFrameTime;

bool firstFrame = true; // necessary to prevent accidental inputs at the start of the level or when unpausing
bool skipUpdate = true; // true -> dont split steps during PlayerObject::update()
bool enableInput = false;
bool linuxNative = false;
bool lateCutoff; // false -> ignore inputs that happen after the start of the frame; true -> check for inputs at the latest possible moment

//std::array<std::unordered_set<size_t>, 6> inputBinds;

BindMask g_bindMaskA{};
BindMask g_bindMaskB{};
std::atomic<BindMask*> g_bindMask{ &g_bindMaskA };

//std::mutex inputQueueLock;
//std::mutex keybindsLock;

std::atomic<bool> enableRightClick;
bool threadPriority;
bool disablePriorityBoost = false;
bool mmcssGames = true;

static std::atomic<uint64_t> g_lastDropped{0};

inline bool stepsEmpty() { return stepIdx >= stepQueue.size(); }
inline Step const& stepsFront() { return stepQueue[stepIdx]; }
inline void stepsPopFront() { if (!stepsEmpty()) ++stepIdx; }

inline void resetInputState() {
    firstFrame = true;
    skipUpdate = true;
    enableInput = true;

    nextInput = EMPTY_INPUT;

    inputQueueCopy.clear();
    stepQueue.clear();

    inputIdx = 0;
    stepIdx = 0;

    g_inputRing.clear();
	g_lastDropped.store(g_inputRing.dropped(), std::memory_order_relaxed);
	g_resetGen.fetch_add(1, std::memory_order_release);
}

void buildStepQueue(int stepCount) {
	nextInput = EMPTY_INPUT;

	stepQueue.clear();
	inputQueueCopy.clear();
	stepIdx = 0;
	inputIdx = 0;

	stepQueue.reserve(stepCount * 2 + 8);
	inputQueueCopy.reserve(256);

#ifdef GEODE_IS_WINDOWS
	if (linuxNative) linuxCheckInputs();
#endif

	currentFrameTime = getCurrentTimestamp();

	if (lateCutoff) {
        InputEvent ev;
        while (g_inputRing.try_pop(ev)) {
            inputQueueCopy.emplace_back(std::move(ev));
        }
    } else {
        const InputEvent* peek = nullptr;
        InputEvent ev;
        while (g_inputRing.try_peek_ptr(peek) && peek->time <= currentFrameTime) {
            (void)g_inputRing.try_pop(ev);
            inputQueueCopy.emplace_back(std::move(ev));
        }
    }

	skipUpdate = false;
	if (firstFrame) {
		skipUpdate = true;
		firstFrame = false;
		lastFrameTime = currentFrameTime;

		if (!lateCutoff) {
			inputQueueCopy.clear();
			stepQueue.clear();
			inputIdx = 0;
			stepIdx = 0;
		}
		return;
	}

	TimestampType deltaTime = currentFrameTime - lastFrameTime;

	if (deltaTime <= 0 || stepCount <= 0) {
		stepQueue.emplace_back(Step{ EMPTY_INPUT, 1.0, true });
		lastFrameTime = currentFrameTime;
		return;
	}

	for (int i = 0; i < stepCount; ++i) {
		const TimestampType stepStart =
			lastFrameTime + (deltaTime * i) / stepCount;

		const TimestampType stepEnd =
			lastFrameTime + (deltaTime * (i + 1)) / stepCount;

		TimestampType stepLen = stepEnd - stepStart;
		if (stepLen <= 0) stepLen = 1; // paranoia; usually never hits

		double elapsedTime = 0.0;

		while (inputIdx < inputQueueCopy.size()) {
			auto& front = inputQueueCopy[inputIdx];
			if (front.time < stepEnd) {
				TimestampType t = front.time;
				if (t < stepStart) t = stepStart;

				const double inputTime = std::clamp(
					static_cast<double>(t - stepStart) / static_cast<double>(stepLen),
					0.0,
					1.0
				);

				stepQueue.emplace_back(Step{
					std::move(front),
					std::clamp(inputTime - elapsedTime, SMALLEST_FLOAT, 1.0),
					false
				});

				++inputIdx;
				elapsedTime = inputTime;
			} else break;
		}

		stepQueue.emplace_back(Step{
			EMPTY_INPUT,
			std::max(SMALLEST_FLOAT, 1.0 - elapsedTime),
			true
		});
	}

	lastFrameTime = currentFrameTime;
}

/*
return the first step in the queue,
also check if an input happened on the previous step, if so run handleButton.
tbh this doesnt need to be a separate function from the PlayerObject::update() hook
*/
Step popStepQueue() {
	if (stepIdx >= stepQueue.size()) return EMPTY_STEP;

	Step front = std::move(stepQueue[stepIdx++]);

	if (nextInput.time != 0) {
		if (auto* playLayer = PlayLayer::get()) {
			enableInput = true;
			playLayer->handleButton(nextInput.inputState, (int)nextInput.inputType, nextInput.isPlayer1);
			enableInput = false;
		}
	}

	nextInput = front.input;

	return front;
}

#ifdef GEODE_IS_WINDOWS
#include <geode.custom-keybinds/include/Keybinds.hpp>
/*
send list of keybinds to the input thread
*/
void updateKeybinds() {
    BindMask tmp;
    for (auto& b : tmp) b.reset();

    std::vector<geode::Ref<keybinds::Bind>> v;
    enableRightClick.store(Mod::get()->getSettingValue<bool>("right-click"));

	static std::atomic<bool> s_warnedBadHash{false};
	size_t skipped = 0;

    auto addBinds = [&](int action, const char* id) {
        v = keybinds::BindManager::get()->getBindsFor(id);
        for (auto& bind : v) {
            auto hk = static_cast<size_t>(bind->getHash());
            if (hk < 256) tmp[action].set(hk);
			else skipped++;
        }
    };

    addBinds(p1Jump,  "robtop.geometry-dash/jump-p1");
    addBinds(p1Left,  "robtop.geometry-dash/move-left-p1");
    addBinds(p1Right, "robtop.geometry-dash/move-right-p1");
    addBinds(p2Jump,  "robtop.geometry-dash/jump-p2");
    addBinds(p2Left,  "robtop.geometry-dash/move-left-p2");
    addBinds(p2Right, "robtop.geometry-dash/move-right-p2");

	if (skipped && !s_warnedBadHash.exchange(true)) {
    	log::warn("CBF: ignored {} keybind hashes >= 256 (those binds won't work with current mask).", skipped);
	}

    // publish swap (no mutex needed)
    BindMask* cur = g_bindMask.load(std::memory_order_relaxed);
    BindMask* next = (cur == &g_bindMaskA) ? &g_bindMaskB : &g_bindMaskA;
    *next = std::move(tmp);
    g_bindMask.store(next, std::memory_order_release);
}
#endif

/*
rewritten PlayerObject::resetCollisionLog() since it's inlined in GD 2.2074 on Windows
*/
void decomp_resetCollisionLog(PlayerObject* p) {
	p->m_collisionLogTop->removeAllObjects();
    p->m_collisionLogBottom->removeAllObjects();
    p->m_collisionLogLeft->removeAllObjects();
    p->m_collisionLogRight->removeAllObjects();
	p->m_lastCollisionLeft = -1;
	p->m_lastCollisionRight = -1;
	p->m_lastCollisionBottom = -1;
	p->m_lastCollisionTop = -1;
}

double averageDelta = 0.0;

bool physicsBypass;
bool legacyBypass;

/*
determine the number of physics steps that happen on each frame,
need to rewrite the vanilla formula bc otherwise you'd have to use inline assembly to get the step count
*/
int calculateStepCount(float delta, float timewarp, bool forceVanilla) {
	if (!physicsBypass || forceVanilla) { // vanilla 2.2
		return std::round(std::max(1.0, ((delta * 60.0) / std::min(1.0f, timewarp)) * 4.0)); // not sure if this is different from `(delta * 240) / timewarp` bc of float precision
	}
	else if (legacyBypass) { // 2.1 physics bypass (same as vanilla 2.1)
		return std::round(std::max(4.0, delta * 240.0) / std::min(1.0f, timewarp));
	}
	else { // sorta just 2.2 + physics bypass but it doesnt allow below 240 steps/sec, also it smooths things out a bit when lagging
		double animationInterval = CCDirector::sharedDirector()->getAnimationInterval();
		averageDelta = (0.05 * delta) + (0.95 * averageDelta); // exponential moving average to detect lag/external fps caps
		if (averageDelta > animationInterval * 10) averageDelta = animationInterval * 10; // dont let averageDelta get too high

		bool laggingOneFrame = animationInterval < delta - (1.0 / 240.0); // more than 1 step of lag on a single frame
		bool laggingManyFrames = averageDelta - animationInterval > 0.0005; // average lag is >0.5ms

		if (!laggingOneFrame && !laggingManyFrames) { // no stepcount variance when not lagging
			return std::round(std::ceil((animationInterval * 240.0) - 0.0001) / std::min(1.0f, timewarp));
		}
		else if (!laggingOneFrame) { // consistently low fps
			return std::round(std::ceil(averageDelta * 240.0) / std::min(1.0f, timewarp));
		}
		else { // need to catch up badly
			return std::round(std::ceil(delta * 240.0) / std::min(1.0f, timewarp));
		}
	}
}

bool safeMode;

class $modify(PlayLayer) {
#ifdef GEODE_IS_WINDOWS
	// update keybinds when you enter a level
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		updateKeybinds();
		return PlayLayer::init(level, useReplay, dontCreateObjects);
	}
#endif

	// disable progress in safe mode
	void levelComplete() {
		bool testMode = this->m_isTestMode;
		if (safeMode && !softToggle.load()) this->m_isTestMode = true;

		PlayLayer::levelComplete();

		this->m_isTestMode = testMode;
	}

	// disable new best popup in safe mode
	void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
		if (!safeMode || softToggle.load()) PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
	}
};

bool mouseFix;

void onFramePrePoll() {
	const uint64_t d = g_inputRing.dropped();
    const uint64_t prev = g_lastDropped.load(std::memory_order_relaxed);

	if (d != prev) {
		g_lastDropped.store(d, std::memory_order_relaxed);

		resetInputState();

		log::warn("CBF: input ring dropped events, total = {}", d);
	}

    PlayLayer* playLayer = PlayLayer::get();
    CCNode* par;

    if (softToggle.load()
    #ifdef GEODE_IS_WINDOWS
        || !GetFocus()
    #endif
        || !playLayer
        || !(par = playLayer->getParent())
        || (par->getChildByType<PauseLayer>(0))
        || (playLayer->getChildByType<EndLevelLayer>(0)))
    {
        resetInputState();
    }

    #ifdef GEODE_IS_WINDOWS
    if (mouseFix && !skipUpdate) {
        MSG msg;
        while (PeekMessage(&msg, NULL, WM_MOUSEMOVE,   WM_MOUSEMOVE,   PM_REMOVE)) {}
		while (PeekMessage(&msg, NULL, WM_NCMOUSEMOVE, WM_NCMOUSEMOVE, PM_REMOVE)) {}
    }
    #endif
}

#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCEGLView.hpp>
class $modify(CCEGLView) {
    void pollEvents() {
        onFramePrePoll();
        CCEGLView::pollEvents();
    }
};
#else
#include <Geode/modify/CCScheduler.hpp>
class $modify(CCScheduler) {
	void update(float dt) {
		onFramePrePoll();
		CCScheduler::update(dt);
	}
};
#endif

int stepCount;
bool clickOnSteps = false;

class $modify(GJBaseGameLayer) {
	static void onModify(auto& self) {
		(void) self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
		(void) self.setHookPriority("GJBaseGameLayer::getModifiedDelta", Priority::VeryEarly);
	}

	// disable regular inputs while CBF is active
	void handleButton(bool down, int button, bool isPlayer1) {
		if (enableInput) GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	// either use the modified delta to calculate the step count, or use the actual delta if physics bypass is enabled
	float calculateSteps(float modifiedDelta) {
		PlayLayer* pl = PlayLayer::get();
		if (pl) {
			const float timewarp = pl->m_gameState.m_timeWarp;
			if (physicsBypass && (!firstFrame || softToggle.load())) modifiedDelta = CCDirector::sharedDirector()->getActualDeltaTime() * timewarp;

			stepCount = calculateStepCount(modifiedDelta, timewarp, false);

			if (pl->m_playerDied || GameManager::sharedState()->getEditorLayer() || softToggle.load()) {
				resetInputState();
			}
			else if (modifiedDelta > 0.0) buildStepQueue(stepCount);
			else skipUpdate = true;
		}
		else if (physicsBypass) stepCount = calculateStepCount(modifiedDelta, this->m_gameState.m_timeWarp, true); // disable physics bypass outside levels

		return modifiedDelta;
	}

	void processCommands(float p0) {
		if (clickOnSteps && stepIdx < stepQueue.size()) {
			Step step;
			do step = popStepQueue(); while ((stepIdx < stepQueue.size()) && !step.endStep); // process 1 step (or more if theres an input)
		}
		GJBaseGameLayer::processCommands(p0);
	}

	float getModifiedDelta(float delta) {
		return calculateSteps(GJBaseGameLayer::getModifiedDelta(delta));
	}

#ifdef GEODE_IS_MACOS
	// getModifiedDelta is inlined, hook update directly instead
	void update(float delta) {
		if (this->m_started) {
			float timewarp = std::max(this->m_gameState.m_timeWarp, 1.0f) / 240.0f;
			calculateSteps(roundf((this->m_extraDelta + (m_resumeTimer <= 0 ? delta : 0.0)) / timewarp) * timewarp);
		}

		GJBaseGameLayer::update(delta);
	}
#endif
};

CCPoint p1Pos = { 0.f, 0.f };
CCPoint p2Pos = { 0.f, 0.f };

float rotationDelta;
float shipRotDelta = 0.0f;
bool inputThisStep = false;
bool p1Split = false;
bool p2Split = false;
bool midStep = false;

class $modify(PlayerObject) {
	// split a single step based on the entries in stepQueue
	void update(float stepDelta) {
		PlayLayer* pl = PlayLayer::get();
		if (!skipUpdate) enableInput = false;
		
		if (pl && this != pl->m_player1 || midStep) { // do all of the logic during the P1 update for simplicity
			if (midStep || !inputThisStep || this != pl->m_player2) PlayerObject::update(stepDelta);
			return; 
		}

		inputThisStep = (stepIdx < stepQueue.size()) ? !stepQueue[stepIdx].endStep : false;
		if ((stepIdx < stepQueue.size()) && !inputThisStep && !clickOnSteps) {
			++stepIdx;
		}
		
		if (skipUpdate
			|| !pl
			|| !inputThisStep
			|| clickOnSteps)
		{
			p1Split = false;
			p2Split = false;
			inputThisStep = false;
			PlayerObject::update(stepDelta);
			return;
		}

		PlayerObject* p2 = pl->m_player2;
		bool isDual = pl->m_gameState.m_isDualMode;
		bool p1StartedOnGround = this->m_isOnGround;
		bool p2StartedOnGround = p2->m_isOnGround;

		bool p1NotBuffering = p1StartedOnGround
			|| this->m_touchingRings->count()
			|| this->m_isDashing
			|| (this->m_isDart || this->m_isBird || this->m_isShip || this->m_isSwing);

		bool p2NotBuffering = p2StartedOnGround
			|| p2->m_touchingRings->count()
			|| p2->m_isDashing
			|| (p2->m_isDart || p2->m_isBird || p2->m_isShip || p2->m_isSwing);

		p1Pos = PlayerObject::getPosition(); // save for later to prevent desync with move triggers & some other issues
		p2Pos = p2->getPosition();

		p1Split = p1NotBuffering;
		p2Split = p2NotBuffering && isDual;
		
		Step step;
		bool firstLoop = true;
		midStep = true;

		do {
			step = popStepQueue();
			const float substepDelta = stepDelta * step.deltaFactor;
			rotationDelta = substepDelta;
			
			if (p1Split) {
				PlayerObject::update(substepDelta);
				if (!step.endStep) {
					if (firstLoop && ((this->m_yVelocity < 0) ^ this->m_isUpsideDown)) this->m_isOnGround = p1StartedOnGround; // this fixes delayed inputs on platforms moving down for some reason
					if (!this->m_isOnSlope || this->m_isDart) pl->checkCollisions(this, 0.0f, true); // moving platforms will launch u really high if this is anything other than 0.0, idk why
					else pl->checkCollisions(this, stepDelta, true); // slopes will launch you really high if the 2nd argument is lower than like 0.01, idk why
					PlayerObject::updateRotation(substepDelta);
					decomp_resetCollisionLog(this); // necessary for wave
				}
			}
			else if (step.endStep) PlayerObject::update(stepDelta); // revert to click-on-steps mode when buffering to reduce bugs

			if (p2Split) {
				p2->update(substepDelta);
				if (!step.endStep) {
					if (firstLoop && ((p2->m_yVelocity < 0) ^ p2->m_isUpsideDown)) p2->m_isOnGround = p2StartedOnGround;
					if (!p2->m_isOnSlope || p2->m_isDart) pl->checkCollisions(p2, 0.0f, true);
					else pl->checkCollisions(p2, stepDelta, true);
					p2->updateRotation(substepDelta);
					decomp_resetCollisionLog(p2);
				}
			}
			else if (step.endStep) p2->update(stepDelta);

			firstLoop = false;
		} while (!step.endStep);

		midStep = false;
	}

	// this function was chosen to update m_lastPosition in just because it's called right at the end of the vanilla physics step loop
	void updateRotation(float t) {
		PlayLayer* pl = PlayLayer::get();
		
		if (pl && this == pl->m_player1 && p1Split && !midStep) {
			PlayerObject::updateRotation(rotationDelta); // perform the remaining rotation that was left incomplete in the PlayerObject::update() hook
			this->m_lastPosition = p1Pos; // move triggers & spider get confused without this (iirc)
		}
		else if (pl && this == pl->m_player2 && p2Split && !midStep) {
			PlayerObject::updateRotation(rotationDelta);
			this->m_lastPosition = p2Pos;
		}
		else PlayerObject::updateRotation(t);

		if (physicsBypass && pl && !midStep) { // fix percent calculation with physics bypass on 2.2 levels
			pl->m_gameState.m_currentProgress = static_cast<int>(pl->m_gameState.m_levelTime * 240.0);
		}
	}

	#ifdef GEODE_IS_WINDOWS
	void updateShipRotation(float t) {
		PlayLayer* pl = PlayLayer::get();

		if (pl && (this == pl->m_player1 || this == pl->m_player2) && (physicsBypass || inputThisStep)) {
			shipRotDelta = t;
			PlayerObject::updateShipRotation(1.0/1024); // necessary to use a really small deltatime to get around an oversight in rob's math
			shipRotDelta = 0.0f;
		}
		else PlayerObject::updateShipRotation(t);
	}
	#endif
};

/*
CBF/PB endscreen watermark
*/
class $modify(EndLevelLayer) {
	void customSetup() {
		EndLevelLayer::customSetup();

		if (!softToggle.load() || physicsBypass) {
			std::string text;

			if ((softToggle.load() || clickOnSteps) && physicsBypass) text = "PB";
			else if (physicsBypass) text = "CBF+PB";
			else if (!clickOnSteps && !softToggle.load()) text = "CBF";
			else return;

			cocos2d::CCSize size = cocos2d::CCDirector::sharedDirector()->getWinSize();
			CCLabelBMFont* indicator = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");

			indicator->setPosition({ size.width, size.height });
			indicator->setAnchorPoint({ 1.0f, 1.0f });
			indicator->setOpacity(30);
			indicator->setScale(0.2f);

			this->addChild(indicator);
		}
	}
};

/*
dont submit to leaderboards for rated levels
*/
class $modify(GJGameLevel) {
	void savePercentage(int percent, bool p1, int clicks, int attempts, bool valid) {
		valid = (
			softToggle.load() && !physicsBypass	
			|| clickOnSteps && !physicsBypass
			|| this->m_stars == 0
		);

		if (!safeMode || softToggle.load()) GJGameLevel::savePercentage(percent, p1, clicks, attempts, valid);
	}
};

float Slerp2D(float p0, float p1, float p2) {
	auto orig = reinterpret_cast<float (*)(float, float, float)>(geode::base::get() + 0x71ec0);
	if (shipRotDelta != 0.0f) { // only do anything if Slerp2D is called within PlayerObject::updateShipRotation()
		shipRotDelta *= p2 * 1024; // p2 is 1/1024 scaled by a constant factor, we need to multiply shipRotDelta by that factor
		return orig(p0, p1, shipRotDelta);
	}
	else return orig(p0, p1, p2);
}

void togglePhysicsBypass(bool enable) {
#ifdef GEODE_IS_WINDOWS
	void* addr = reinterpret_cast<void*>(geode::base::get() + 0x2322ca);

	static Patch* pbPatch = nullptr;
	if (!pbPatch) {
		geode::ByteVector bytes = { 0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0, 0x44, 0x8b, 0x19 }; // could be 1 instruction if i was less lazy
		int* stepAddr = &stepCount;
		for (int i = 0; i < 8; i++) { // for each byte in stepAddr
			bytes[i + 2] = ((char*)&stepAddr)[i]; // replace the zeroes with the address of stepCount
		}
		log::info("Physics bypass patch: {} at {}", bytes, addr);
		pbPatch = Mod::get()->patch(addr, bytes).unwrap();
	}

	if (enable) {
		(void) pbPatch->enable();
	} 
	else  {
		(void) pbPatch->disable();
	}

	physicsBypass = enable;
#endif
}

void toggleMod(bool disable) {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID64)
	void* addr = reinterpret_cast<void*>(geode::base::get() + GEODE_WINDOWS(0x607230) GEODE_ANDROID64(0x5c00d0));

	static Patch* modPatch = nullptr;
	if (!modPatch) modPatch = Mod::get()->patch(addr, { 0x29, 0x5c, 0x4f, 0x3f }).unwrap();

	if (disable) {
		(void) modPatch->disable();
	} else {
		(void) modPatch->enable();
	}
#endif

	softToggle.store(disable);
}

$on_mod(Loaded) {
	Mod::get()->setSavedValue<bool>("is-linux", false);

	toggleMod(Mod::get()->getSettingValue<bool>("soft-toggle"));
	listenForSettingChanges("soft-toggle", toggleMod);

	togglePhysicsBypass(Mod::get()->getSettingValue<bool>("physics-bypass"));
	listenForSettingChanges("physics-bypass", togglePhysicsBypass);

	legacyBypass = Mod::get()->getSettingValue<std::string>("bypass-mode") == "2.1";
	listenForSettingChanges("bypass-mode", +[](std::string mode) {
		legacyBypass = mode == "2.1";
	});

	safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
	listenForSettingChanges("safe-mode", +[](bool enable) {
		safeMode = enable;
	});

	clickOnSteps = Mod::get()->getSettingValue<bool>("click-on-steps");
	listenForSettingChanges("click-on-steps", +[](bool enable) {
		clickOnSteps = enable;
	});

	mouseFix = Mod::get()->getSettingValue<bool>("mouse-fix");
	listenForSettingChanges("mouse-fix", +[](bool enable) {
		mouseFix = enable;
	});

	lateCutoff = Mod::get()->getSettingValue<bool>("late-cutoff");
	listenForSettingChanges("late-cutoff", +[](bool enable) {
		lateCutoff = enable;
	});

	threadPriority = Mod::get()->getSettingValue<bool>("thread-priority");

	mmcssGames = Mod::get()->getSettingValue<bool>("mmcss-games");

	disablePriorityBoost = Mod::get()->getSettingValue<bool>("thread-priority-boost-disable");

#ifdef GEODE_IS_WINDOWS
	(void) Mod::get()->hook(
		reinterpret_cast<void*>(geode::base::get() + 0x71ec0),
		Slerp2D,
		"Slerp2D",
		tulip::hook::TulipConvention::Default
	);

	windowsSetup();
#endif
}