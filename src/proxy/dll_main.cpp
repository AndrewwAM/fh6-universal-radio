// version.dll proxy: forwards every export to the real system DLL via PE
// forwarders (declared in version.def), and spawns the bridge on
// DLL_PROCESS_ATTACH so the loader is never blocked on FMOD discovery
// or HTTP startup.

#include <windows.h>

namespace fh6 {
void run_bridge(HMODULE self) noexcept;
} // namespace fh6

namespace {
DWORD WINAPI bridge_thread(LPVOID self) {
    fh6::run_bridge(static_cast<HMODULE>(self));
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) noexcept {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, bridge_thread, hModule, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
