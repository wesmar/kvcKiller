#pragma once

// Loads the two kernel components used by the kill pipeline.
//
// Driver 1 exposes kernel memory primitives and is used to lower process
// protection. Driver 2 performs the actual process termination. Both images
// are extracted from the same cabinet resource and registered as independent
// demand-start services.

#include <windows.h>
#include "KvcContainers.h"

enum class DriverRole {
    PplUnprotector,
    ProcessTerminator
};

class DriverController {
public:
    static HANDLE LoadDriver(DriverRole role);
    static HANDLE GetDriverHandle(DriverRole role);
    static bool IsDriverLoaded(DriverRole role);

    static WString GetDriverPath(DriverRole role);

    // Closes both device handles, stops and removes both services, restores
    // temporary driver configuration, and deletes the staged driver files.
    static void CleanupAll();

private:
    static bool ExtractDriverFile(DriverRole role);
    static bool ConfigureDriver(DriverRole role);
    static void RestoreDriverConfiguration(DriverRole role);
    static void StopAndRemoveService(DriverRole role);
    static bool EnablePrivilege(LPCWSTR privilege);
};
