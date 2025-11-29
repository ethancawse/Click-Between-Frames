#include "includes.hpp"
#include "input_ring.hpp"
#include <geode.custom-keybinds/include/Keybinds.hpp>

#ifdef GEODE_IS_WINDOWS
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")
#endif

static std::array<uint8_t, 256> held = {};

TimestampType getCurrentTimestamp() {
	LARGE_INTEGER t;
	if (linuxNative) {
		// used instead of QPC to make it possible to convert between Linux and Windows timestamps
		GetSystemTimePreciseAsFileTime((FILETIME*)&t);
	} else {
		QueryPerformanceCounter(&t);
	}
	return t.QuadPart;
}

LPVOID pBuf;
HANDLE hSharedMem = NULL;
HANDLE hMutex = NULL;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	LARGE_INTEGER time;
	PlayerButton inputType;
	bool inputState;
	bool player1;

	switch (uMsg) {
		case WM_INPUT: {
		// Read RAWINPUT with a stack buffer first (fast path).
		RAWINPUT stackRaw{};
		UINT size = sizeof(stackRaw);

		// Use a reusable thread-local buffer only if needed.
		thread_local std::vector<BYTE> tlBuf;

		RAWINPUT* raw = nullptr;

		if (softToggle.load(std::memory_order_relaxed)) return 0;

		static thread_local uint32_t seenGen = 0;
		uint32_t gen = g_resetGen.load(std::memory_order_acquire);
		if (gen != seenGen) {
			held.fill(0);
			seenGen = gen;
		}

		UINT got = GetRawInputData(
			(HRAWINPUT)lParam,
			RID_INPUT,
			&stackRaw,
			&size,
			sizeof(RAWINPUTHEADER)
		);

		if (got == (UINT)-1) {
			// If stack buffer wasn't enough, size now contains required bytes.
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
				return 0;
			}

			tlBuf.resize(size);
			got = GetRawInputData(
				(HRAWINPUT)lParam,
				RID_INPUT,
				tlBuf.data(),
				&size,
				sizeof(RAWINPUTHEADER)
			);
			if (got == (UINT)-1) {
				return 0;
			}
			raw = reinterpret_cast<RAWINPUT*>(tlBuf.data());
		} else {
			raw = &stackRaw;
		}

		// Only handle keyboard + mouse here.
		switch (raw->header.dwType) {
		case RIM_TYPEMOUSE: {
			const USHORT flags = raw->data.mouse.usButtonFlags;

			// Only clicks matter for this mod. Ignore move/wheel/etc FAST.
			constexpr USHORT kInteresting =
				RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_1_UP |
				RI_MOUSE_BUTTON_2_DOWN | RI_MOUSE_BUTTON_2_UP;

			if ((flags & kInteresting) == 0) {
				return 0; // does NOT block vanilla mouse move/wheel; we just don't process it here
			}

			// Timestamp only for relevant mouse button events
			QueryPerformanceCounter(&time);

			player1 = true;
			inputType = PlayerButton::Jump;

			if (flags & RI_MOUSE_BUTTON_1_DOWN) inputState = Press;
			else if (flags & RI_MOUSE_BUTTON_1_UP) inputState = Release;
			else {
				// right click -> player 2 jump if enabled
				if (!enableRightClick.load()) return 0;

				player1 = false;
				if (flags & RI_MOUSE_BUTTON_2_DOWN) inputState = Press;
				else if (flags & RI_MOUSE_BUTTON_2_UP) inputState = Release;
				else return 0;

				queueInMainThread([inputState]() {
					keybinds::InvokeBindEvent("robtop.geometry-dash/jump-p2", inputState).post();
				});
			}

			break;
		}

		case RIM_TYPEKEYBOARD: {
			USHORT vkey = raw->data.keyboard.VKey;
			inputState = (raw->data.keyboard.Flags & RI_KEY_BREAK) ? Release : Press;

			if (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9) vkey -= 0x30;

			if (vkey >= 256) return 0;

			if (held[vkey]) {
				if (inputState == Press) return 0;
				held[vkey] = 0;
			} else {
				if (inputState == Press) held[vkey] = 1;
			}

			auto* mask = g_bindMask.load(std::memory_order_acquire);
			bool shouldEmplace = true;
			player1 = true;

			if ((*mask)[p1Jump].test(vkey)) inputType = PlayerButton::Jump;
			else if ((*mask)[p1Left].test(vkey)) inputType = PlayerButton::Left;
			else if ((*mask)[p1Right].test(vkey)) inputType = PlayerButton::Right;
			else {
				player1 = false;
				if ((*mask)[p2Jump].test(vkey)) inputType = PlayerButton::Jump;
				else if ((*mask)[p2Left].test(vkey)) inputType = PlayerButton::Left;
				else if ((*mask)[p2Right].test(vkey)) inputType = PlayerButton::Right;
				else shouldEmplace = false;
			}

		if (!shouldEmplace) return 0;

		QueryPerformanceCounter(&time);
		break;
	}

		default:
			return 0;
		}

		g_inputRing.try_push(InputEvent{
			timestampFromLarge(time),
			inputType,
			inputState,
			player1
		});
		

		return 0;
		}
	default:
		return DefWindowProcA(hwnd, uMsg, wParam, lParam);
	}
}

void inputThread() {
	WNDCLASS wc = {};
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "CBF";

	RegisterClass(&wc);
	HWND hwnd = CreateWindow("CBF", "Raw Input Window", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
	if (!hwnd) {
		const DWORD err = GetLastError();
		log::error("Failed to create raw input window: {}", err);
		return;
	}

	RAWINPUTDEVICE dev[2];
	dev[0].usUsagePage = 0x01;        // generic desktop controls
	dev[0].usUsage = 0x02;            // mouse
	dev[0].dwFlags = RIDEV_INPUTSINK; // allow inputs without being in the foreground
	dev[0].hwndTarget = hwnd;         // raw input window

	dev[1].usUsagePage = 0x01;
	dev[1].usUsage = 0x06;            // keyboard
	dev[1].dwFlags = RIDEV_INPUTSINK;
	dev[1].hwndTarget = hwnd;

	if (!RegisterRawInputDevices(dev, 2, sizeof(dev[0]))) {
		log::error("Failed to register raw input devices");
		return;
	}

#ifdef GEODE_IS_WINDOWS
    HANDLE mmcssTask = nullptr;
    DWORD mmcssTaskIndex = 0;

    if (threadPriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

	if (mmcssGames) {
		mmcssTask = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex);
		if (mmcssTask) {
            AvSetMmThreadPriority(mmcssTask, AVRT_PRIORITY_HIGH);
        }
	}

	SetThreadPriorityBoost(GetCurrentThread(), disablePriorityBoost ? TRUE : FALSE);
#endif

	MSG msg;
	while (GetMessage(&msg, hwnd, 0, 0)) {
		DispatchMessage(&msg);
		while (softToggle.load()) { // reduce lag while mod is disabled
			Sleep(2000);
			while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)); // clear all pending messages
			held.fill(0);
		}
	}

#ifdef GEODE_IS_WINDOWS
    if (mmcssTask) {
        AvRevertMmThreadCharacteristics(mmcssTask);
    }
#endif
}

// notify the player if theres an issue with input on Linux
#include <Geode/modify/CreatorLayer.hpp>
class $modify(CreatorLayer) {
	bool init() {
		if (!CreatorLayer::init()) return false;

		if (linuxNative) {
			DWORD waitResult = WaitForSingleObject(hMutex, 5);
			if (waitResult == WAIT_OBJECT_0) {
				if (static_cast<LinuxInputEvent*>(pBuf)[0].type == 3 && !softToggle.load()) {
					log::error("Linux input failed");
					FLAlertLayer* popup = FLAlertLayer::create(
						"CBF Linux",
						"Failed to read input devices.\nOn most distributions, this can be resolved with the following command: <cr>sudo usermod -aG input $USER</c> (reboot afterward; this will make your system slightly less secure).\nIf the issue persists, please contact the mod developer.",
						"OK"
					);
					popup->m_scene = this;
					popup->show();
				}
				ReleaseMutex(hMutex);
			}
			else if (waitResult == WAIT_TIMEOUT) {
				log::error("Mutex stalling");
			}
			else {
				// log::error("CreatorLayer WaitForSingleObject failed: {}", GetLastError());
			}
		}
		return true;
	}
};

void linuxCheckInputs() {
    DWORD waitResult = WaitForSingleObject(hMutex, 1);
    if (waitResult == WAIT_OBJECT_0) {
        LinuxInputEvent* events = static_cast<LinuxInputEvent*>(pBuf);

        auto* mask = g_bindMask.load(std::memory_order_acquire);

        for (int i = 0; i < BUFFER_SIZE; i++) {
            if (events[i].type == 0) break;

            InputEvent input;
            bool player1 = true;

            USHORT scanCode = events[i].code;

            if (scanCode == 0x3110) { // left click
                input.inputType = PlayerButton::Jump;
            }
            else if (scanCode == 0x3111) { // right click
                if (!enableRightClick.load()) continue;
                input.inputType = PlayerButton::Jump;
                player1 = false;
            }
            else {
                USHORT keyCode = MapVirtualKeyExA(scanCode, MAPVK_VSC_TO_VK, GetKeyboardLayout(0));
                if (keyCode >= 256) continue; // bitset is 0..255

                if ((*mask)[p1Jump].test(keyCode)) input.inputType = PlayerButton::Jump;
                else if ((*mask)[p1Left].test(keyCode)) input.inputType = PlayerButton::Left;
                else if ((*mask)[p1Right].test(keyCode)) input.inputType = PlayerButton::Right;
                else {
                    player1 = false;
                    if ((*mask)[p2Jump].test(keyCode)) input.inputType = PlayerButton::Jump;
                    else if ((*mask)[p2Left].test(keyCode)) input.inputType = PlayerButton::Left;
                    else if ((*mask)[p2Right].test(keyCode)) input.inputType = PlayerButton::Right;
                    else continue;
                }
            }

            input.inputState = events[i].value;
            input.time = timestampFromLarge(events[i].time);
            input.isPlayer1 = player1;

            g_inputRing.try_push(std::move(input));
        }

        ZeroMemory(events, sizeof(LinuxInputEvent[BUFFER_SIZE]));
        ReleaseMutex(hMutex);
    }
    else if (waitResult != WAIT_TIMEOUT) {
        log::error("WaitForSingleObject failed: {}", GetLastError());
    }
}

void windowsSetup() {
	HANDLE gdMutex;

	HMODULE ntdll = GetModuleHandle("ntdll.dll");
	typedef void (*wine_get_host_version)(const char **sysname, const char **release);
	wine_get_host_version wghv = (wine_get_host_version)GetProcAddress(ntdll, "wine_get_host_version");
	if (wghv) { // if this function exists, the user is on Wine
		const char* sysname;
		const char* release;
		wghv(&sysname, &release);

		std::string sys = sysname;
		log::info("Wine {}", sys);

		if (sys == "Linux") Mod::get()->setSavedValue<bool>("you-must-be-on-linux-to-change-this", true);
		if (sys == "Linux" && Mod::get()->getSettingValue<bool>("wine-workaround")) { // background raw keyboard input doesn't work in Wine
			linuxNative = true;
			log::info("Linux native");

			hSharedMem = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LinuxInputEvent[BUFFER_SIZE]), "LinuxSharedMemory");
			if (hSharedMem == NULL) {
				log::error("Failed to create file mapping: {}", GetLastError());
				return;
			}

			pBuf = MapViewOfFile(hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinuxInputEvent[BUFFER_SIZE]));
			if (pBuf == NULL) {
				log::error("Failed to map view of file: {}", GetLastError());
				CloseHandle(hSharedMem);
				return;
			}

			hMutex = CreateMutex(NULL, FALSE, "CBFLinuxMutex"); // used to gate access to the shared memory buffer for inputs
			if (hMutex == NULL) {
				log::error("Failed to create shared memory mutex: {}", GetLastError());
				CloseHandle(hSharedMem);
				return;
			}

			gdMutex = CreateMutex(NULL, TRUE, "CBFWatchdogMutex"); // will be released when gd closes
			if (gdMutex == NULL) {
				log::error("Failed to create watchdog mutex: {}", GetLastError());
				CloseHandle(hMutex);
				CloseHandle(hSharedMem);
				return;
			}

			SECURITY_ATTRIBUTES sa;
			sa.nLength = sizeof(SECURITY_ATTRIBUTES);
			sa.bInheritHandle = TRUE;
			sa.lpSecurityDescriptor = NULL;

			STARTUPINFO si;
			PROCESS_INFORMATION pi;
			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			ZeroMemory(&pi, sizeof(pi));

			std::string path = CCFileUtils::get()->fullPathForFilename("linux-input.so"_spr, true);

			if (!CreateProcess(path.c_str(), NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
				log::error("Failed to launch Linux input program: {}", GetLastError());
				CloseHandle(hMutex);
				CloseHandle(gdMutex);
				CloseHandle(hSharedMem);
				return;
			}
		}
	}

	if (!linuxNative) {
		std::thread(inputThread).detach();
	}
}
