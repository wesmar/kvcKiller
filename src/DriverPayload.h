#pragma once

// Extracts the driver files stored in the cabinet appended to the ICO resource.
// The returned entries are matched by their cabinet names, not by position.

#include <windows.h>
#include <string>
#include <vector>

struct DriverPayloadFile {
    std::string name;
    std::vector<BYTE> data;
};

bool ExtractDriverPayloads(std::vector<DriverPayloadFile>& files);
