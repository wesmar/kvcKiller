#pragma once

// Reconstructs the driver components from the steganographic guest image resource
// and authentic system template.
// The returned entries are matched by their component names, not by position.

#include <windows.h>
#include "KvcContainers.h"

struct DriverPackageFile {
    AString name;
    Vector<BYTE> data;
};

bool ExtractDriverPackage(Vector<DriverPackageFile>& files);
