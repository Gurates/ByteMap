#pragma once
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <atomic>

struct MftRecord {
    DWORDLONG fileRef;
    DWORDLONG parentRef;
    bool isDir;
    LONGLONG size;
    std::wstring name;
};

struct MftScanResult {
    bool ok = false;
    std::string errorMsg;
    std::vector<MftRecord> records;
};

bool IsNtfsVolume(const std::string& volumeRoot);

MftScanResult EnumerateMft(const std::string& volumeRoot, std::atomic<bool>& shouldStop);