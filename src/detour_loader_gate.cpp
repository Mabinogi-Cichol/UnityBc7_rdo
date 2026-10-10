// No CRT, no allocation, no wait, no lookup: the import is resolved before this
// DLL's entry runs. LoadLibrary of this sibling DLL establishes the OS gate.
#include <windows.h>
extern "C" __declspec(dllimport) void WINAPI RgbmCommitFromLoaderGate();
extern "C" BOOL WINAPI RgbmLoaderGateEntry(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) RgbmCommitFromLoaderGate();
    return TRUE;
}
