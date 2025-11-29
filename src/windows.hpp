#pragma once
#include <Geode/Geode.hpp>

#if defined(_MSC_VER)
    #pragma pack(push, 1)
    #define PACKED
#else
    #define PACKED __attribute__((packed))
#endif

struct PACKED LinuxInputEvent {
    LARGE_INTEGER time;
    USHORT type;
    USHORT code;
    int value;
};

#if defined(_MSC_VER)
    #pragma pack(pop)
#endif

#undef PACKED

extern HANDLE hSharedMem;
extern HANDLE hMutex;
extern LPVOID pBuf;

extern bool linuxNative;

inline LARGE_INTEGER largeFromTimestamp(TimestampType t) {
    LARGE_INTEGER res;
    res.QuadPart = t;
    return res;
}

inline TimestampType timestampFromLarge(LARGE_INTEGER l) {
    return l.QuadPart;
}

constexpr size_t BUFFER_SIZE = 20;

void windowsSetup();
void linuxCheckInputs();
void inputThread();
