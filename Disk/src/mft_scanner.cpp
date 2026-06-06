#include "mft_scanner.h"
#include <sstream>

bool IsNtfsVolume(const std::string& volumeRoot) {
    std::string root = volumeRoot;
    if (!root.empty() && root.back() != '\\') root += '\\';
    char fsName[32] = {};
    return GetVolumeInformationA(
        root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
        fsName, sizeof(fsName))
        && std::string(fsName) == "NTFS";
}

MftScanResult EnumerateMft(const std::string& volumeRoot, std::atomic<bool>& shouldStop) {
    MftScanResult result;

    std::string volPath = "\\\\.\\";
    if (volumeRoot.size() >= 2 && volumeRoot[1] == ':')
        volPath += volumeRoot.substr(0, 2);
    else
        volPath += volumeRoot;

    HANDLE hVol = CreateFileA(volPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hVol == INVALID_HANDLE_VALUE) {
        result.errorMsg = "Cannot open volume.";
        return result;
    }

    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;

    const DWORD BUF = 512 * 1024;
    std::vector<BYTE> buf(BUF);
    result.records.reserve(500000);

    DWORD bytesReturned = 0;
    while (!shouldStop) {
        BOOL ok = DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf.data(), BUF, &bytesReturned, nullptr);

        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                result.ok = true;
                break;
            }
            CloseHandle(hVol);
            return result;
        }

        DWORDLONG next = *reinterpret_cast<DWORDLONG*>(buf.data());
        if (next == 0) {
            result.ok = true;
            break;
        }
        med.StartFileReferenceNumber = next;

        BYTE* ptr = buf.data() + sizeof(DWORDLONG);
        BYTE* endPtr = buf.data() + bytesReturned;

        while (ptr < endPtr) {
            USN_RECORD_V2* rec = reinterpret_cast<USN_RECORD_V2*>(ptr);
            if (rec->RecordLength == 0) break;

            MftRecord mr;
            mr.fileRef = rec->FileReferenceNumber;
            mr.parentRef = rec->ParentFileReferenceNumber;
            mr.isDir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            mr.size = 0;

            WCHAR* namePtr = reinterpret_cast<WCHAR*>(reinterpret_cast<BYTE*>(rec) + rec->FileNameOffset);
            mr.name.assign(namePtr, rec->FileNameLength / sizeof(WCHAR));

            result.records.push_back(std::move(mr));
            ptr += rec->RecordLength;
        }
    }
    CloseHandle(hVol);
    return result;
}