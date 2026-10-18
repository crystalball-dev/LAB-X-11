// The Zone Bus mapping: one named, zero-initialised block per host process, shared by every LAB X-16 DLL loaded in it.
// Kept in its own translation unit so <windows.h> never meets the JUCE headers.
#include "ZoneBus.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <cwchar>
#endif

namespace lab::host::bus
{
    SharedBlock* mapSharedBlock() noexcept
    {
       #if defined (_WIN32)
        // Mapped once per DLL and never unmapped. The handle stays open too: Windows removes a section's name when its
        // last handle closes, even while views still map it, and the next DLL would then create a block of its own
        // (found by Lx16BusHost: every module saw only itself).
        static SharedBlock* mapped = []() -> SharedBlock*
        {
            wchar_t name[96];
            std::swprintf (name, 96, L"Local\\LAB-X16-ZoneBus-%lu-v%u", (unsigned long) GetCurrentProcessId(), (unsigned) layoutVersion);
            HANDLE h = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) sizeof (SharedBlock), name);
            if (h == nullptr)
                return nullptr;
            void* view = MapViewOfFile (h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedBlock));
            if (view == nullptr)
                CloseHandle (h);
            return static_cast<SharedBlock*> (view);
        }();
        return mapped;
       #else
        static SharedBlock local {};
        return &local;
       #endif
    }
}
