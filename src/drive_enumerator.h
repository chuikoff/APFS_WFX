#pragma once

#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

struct VolumeEntryInfo {
    unsigned int volume_index;
    std::string volume_name;       // UTF-8
    std::wstring volume_name_w;    // Wide string for TC
    uint64_t total_bytes;
    uint64_t free_bytes;
};

struct ApfsDriveInfo {
    int drive_number;              // e.g. 1
    std::wstring device_path;      // L"\\\\.\\PhysicalDrive1"
    std::wstring display_name;     // L"PhysicalDrive1 [SanDisk Extreme 64GB]"
    std::string model_name;
    uint64_t drive_size;
    bool is_usb;
    bool has_apfs;
    
    uint64_t apfs_offset;
    uint64_t apfs_size;
    
    std::vector<VolumeEntryInfo> volumes;
};

class DriveEnumerator {
public:
    static std::vector<ApfsDriveInfo> EnumerateApfsDrives(bool *pAccessDeniedOccurred = nullptr);
    static bool InspectDrive(int drive_num, ApfsDriveInfo &info, bool &access_denied);
    static std::wstring FormatSize(uint64_t bytes);
};
