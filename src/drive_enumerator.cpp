#include "drive_enumerator.h"
#include <ApfsLib/DeviceWinPhys.h>
#include <ApfsLib/GptPartitionMap.h>
#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/DiskStruct.h>
#include "hfsplus_reader.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

// GPT type GUID for HFS+ (Apple HFS) partition
// {48465300-0000-11AA-AA11-00306543ECAC}
// Stored in mixed-endian GPT format: first 3 groups LE, last 2 groups BE
static const uint8_t kGptTypeHFSPlus[16] = {
    0x00,0x53,0x46,0x48, 0x00,0x00, 0xAA,0x11, 0xAA,0x11, 0x00,0x30,0x65,0x43,0xEC,0xAC
};

// GPT type GUID for APFS container
// {7C3457EF-0000-11AA-AA11-00306543ECAC}
static const uint8_t kGptTypeAPFS[16] = {
    0xEF,0x57,0x34,0x7C, 0x00,0x00, 0xAA,0x11, 0xAA,0x11, 0x00,0x30,0x65,0x43,0xEC,0xAC
};

std::wstring DriveEnumerator::FormatSize(uint64_t bytes)
{
    const wchar_t *units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unit_idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit_idx < 4)
    {
        size /= 1024.0;
        unit_idx++;
    }
    std::wostringstream wss;
    wss << std::fixed << std::setprecision(1) << size << L" " << units[unit_idx];
    return wss.str();
}

static std::wstring Utf8ToWide(const std::string &str)
{
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring res(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &res[0], len);
    return res;
}

static std::string WideToUtf8(const std::wstring &str)
{
    if (str.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string res(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(), &res[0], len, nullptr, nullptr);
    return res;
}

static void CleanString(std::string &s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;
    if (start > 0)
        s = s.substr(start);
}

static void SanitizeName(std::wstring &name)
{
    for (auto &c : name)
    {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    }
}

#pragma pack(push, 1)
struct GptHeader {
    uint64_t signature;       // "EFI PART"
    uint32_t revision;
    uint32_t headerSize;
    uint32_t headerCRC32;
    uint32_t reserved;
    uint64_t myLBA;
    uint64_t alternateLBA;
    uint64_t firstUsableLBA;
    uint64_t lastUsableLBA;
    uint8_t  diskGUID[16];
    uint64_t partitionEntryLBA;
    uint32_t numPartitionEntries;
    uint32_t sizeOfPartitionEntry;
    uint32_t partitionEntryArrayCRC32;
};

struct GptEntry {
    uint8_t  partitionTypeGUID[16];
    uint8_t  uniquePartitionGUID[16];
    uint64_t startingLBA;
    uint64_t endingLBA;
    uint64_t attributes;
    uint16_t partitionName[36];
};

struct MbrPartitionEntry {
    uint8_t  bootIndicator;
    uint8_t  startingCHS[3];
    uint8_t  partitionType;
    uint8_t  endingCHS[3];
    uint32_t startingLBA;
    uint32_t totalSectors;
};
#pragma pack(pop)

static bool ReadSectors(HANDLE hDrive, uint64_t lba, uint32_t sectorSize, void* buf, uint32_t count)
{
    uint64_t offset = lba * sectorSize;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    OVERLAPPED ov = {};
    ov.Offset     = li.LowPart;
    ov.OffsetHigh = li.HighPart;
    DWORD read = 0;
    uint32_t totalBytes = sectorSize * count;
    if (!ReadFile(hDrive, buf, totalBytes, &read, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(hDrive, &ov, &read, TRUE);
        else
            return false;
    }
    return read == totalBytes;
}

struct HfsPlusPartInfo {
    uint64_t startLBA;
    uint64_t sizeLBA;
};

static std::vector<HfsPlusPartInfo> FindHFSPlusPartitions(HANDLE hDrive, uint32_t sectorSize, uint64_t driveSize)
{
    std::vector<HfsPlusPartInfo> result;

    // 1. Check MBR (LBA 0)
    std::vector<uint8_t> mbrBuf(sectorSize);
    if (ReadSectors(hDrive, 0, sectorSize, mbrBuf.data(), 1)) {
        if (mbrBuf[510] == 0x55 && mbrBuf[511] == 0xAA) {
            for (int i = 0; i < 4; i++) {
                const MbrPartitionEntry* p = reinterpret_cast<const MbrPartitionEntry*>(mbrBuf.data() + 446 + i * 16);
                if (p->partitionType == 0xAF && p->totalSectors > 0) {
                    HfsPlusPartInfo pi;
                    pi.startLBA = p->startingLBA;
                    pi.sizeLBA  = p->totalSectors;
                    result.push_back(pi);
                }
            }
        }
    }

    // 2. Check GPT (LBA 1)
    std::vector<uint8_t> hdrBuf(sectorSize);
    if (ReadSectors(hDrive, 1, sectorSize, hdrBuf.data(), 1)) {
        const GptHeader* gptHdr = reinterpret_cast<const GptHeader*>(hdrBuf.data());
        if (gptHdr->signature == 0x5452415020494645ULL) { // "EFI PART"
            uint32_t numEntries = gptHdr->numPartitionEntries;
            uint32_t entrySize  = gptHdr->sizeOfPartitionEntry;
            uint64_t entryLBA   = gptHdr->partitionEntryLBA;

            if (numEntries > 0 && entrySize >= sizeof(GptEntry) && numEntries <= 256) {
                uint32_t totalBytes = numEntries * entrySize;
                uint32_t sectors    = (totalBytes + sectorSize - 1) / sectorSize;
                std::vector<uint8_t> entriesBuf(sectors * sectorSize);
                if (ReadSectors(hDrive, entryLBA, sectorSize, entriesBuf.data(), sectors)) {
                    for (uint32_t i = 0; i < numEntries; i++) {
                        const GptEntry* entry = reinterpret_cast<const GptEntry*>(entriesBuf.data() + i * entrySize);
                        if (memcmp(entry->partitionTypeGUID, kGptTypeHFSPlus, 16) == 0) {
                            HfsPlusPartInfo pi;
                            pi.startLBA = entry->startingLBA;
                            pi.sizeLBA  = entry->endingLBA - entry->startingLBA + 1;
                            result.push_back(pi);
                        }
                    }
                }
            }
        }
    }

    // 3. If no partitions found, probe bare disk at offset 0
    if (result.empty() && driveSize > 0) {
        HfsPlus::HfsPlusVolume probeVol;
        if (probeVol.Open(hDrive, 0, driveSize, sectorSize)) {
            HfsPlusPartInfo pi;
            pi.startLBA = 0;
            pi.sizeLBA  = driveSize / sectorSize;
            result.push_back(pi);
        }
    }

    return result;
}

bool DriveEnumerator::InspectDrive(int drive_num, ApfsDriveInfo &info, bool &access_denied)
{
    access_denied = false;
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(drive_num);

    HANDLE hDrive = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDrive == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
        {
            access_denied = true;
        }
        return false;
    }

    uint64_t drive_size = 0;
    uint32_t sector_size = 512;

    BYTE geom_buf[1024] = { 0 };
    DWORD bytes_ret = 0;
    if (DeviceIoControl(hDrive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, geom_buf, sizeof(geom_buf), &bytes_ret, nullptr) && bytes_ret > 0)
    {
        const DISK_GEOMETRY_EX *geom = reinterpret_cast<const DISK_GEOMETRY_EX *>(geom_buf);
        drive_size = geom->DiskSize.QuadPart;
        sector_size = geom->Geometry.BytesPerSector ? geom->Geometry.BytesPerSector : 512;
    }
    else
    {
        GET_LENGTH_INFORMATION gli = {};
        if (DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gli, sizeof(gli), &bytes_ret, nullptr))
            drive_size = gli.Length.QuadPart;
        DISK_GEOMETRY dg = {};
        if (DeviceIoControl(hDrive, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &dg, sizeof(dg), &bytes_ret, nullptr))
            sector_size = dg.BytesPerSector ? dg.BytesPerSector : 512;
    }

    if (drive_size == 0)
    {
        CloseHandle(hDrive);
        return false;
    }

    // Query storage device descriptor
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE desc_buf[1024] = { 0 };
    bool is_usb = false;
    std::string model;

    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        desc_buf, sizeof(desc_buf), &bytes_ret, nullptr) && bytes_ret > 0)
    {
        const STORAGE_DEVICE_DESCRIPTOR *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(desc_buf);
        if (desc->BusType == BusTypeUsb)
            is_usb = true;

        std::string vendor, product;
        if (desc->VendorIdOffset != 0 && desc->VendorIdOffset < bytes_ret)
            vendor = reinterpret_cast<const char *>(desc_buf + desc->VendorIdOffset);
        if (desc->ProductIdOffset != 0 && desc->ProductIdOffset < bytes_ret)
            product = reinterpret_cast<const char *>(desc_buf + desc->ProductIdOffset);

        CleanString(vendor);
        CleanString(product);
        if (!vendor.empty() && !product.empty())
            model = vendor + " " + product;
        else if (!product.empty())
            model = product;
        else if (!vendor.empty())
            model = vendor;
    }

    // --- Scan for HFS+ partitions ---
    std::vector<HfsPlusPartInfo> hfsParts = FindHFSPlusPartitions(hDrive, sector_size, drive_size);
    std::vector<VolumeEntryInfo> hfsVolumes;
    for (const auto &part : hfsParts)
    {
        uint64_t partOffset = part.startLBA * sector_size;
        uint64_t partSize   = part.sizeLBA  * sector_size;

        HfsPlus::HfsPlusVolume hfsVol;
        if (hfsVol.Open(hDrive, partOffset, partSize, sector_size))
        {
            VolumeEntryInfo vol = {};
            vol.volume_index = 0; // will be assigned below
            vol.fs_type = FsType::HFSPlus;
            vol.volume_offset = partOffset;
            vol.sector_size = sector_size;
            vol.volume_name_w = hfsVol.GetVolumeName();
            if (vol.volume_name_w.empty()) vol.volume_name_w = L"HFS+ Volume";
            SanitizeName(vol.volume_name_w);
            vol.volume_name = WideToUtf8(vol.volume_name_w);
            vol.total_bytes = hfsVol.GetVolumeSize() ? hfsVol.GetVolumeSize() : partSize;
            vol.free_bytes  = 0;
            hfsVolumes.push_back(vol);
        }
    }

    CloseHandle(hDrive);

    // --- Scan for APFS ---
    std::string utf8_path = "\\\\.\\PhysicalDrive" + std::to_string(drive_num);
    DeviceWinPhys dev;
    bool dev_open = dev.Open(utf8_path.c_str());

    uint64_t apfs_offset = 0;
    uint64_t apfs_size = 0;
    bool found_apfs = false;

    if (dev_open)
    {
        GptPartitionMap gpt;
        if (gpt.LoadAndVerify(dev))
        {
            int part_idx = gpt.FindFirstAPFSPartition();
            if (part_idx != -1 && gpt.GetPartitionOffsetAndSize(part_idx, apfs_offset, apfs_size))
            {
                found_apfs = true;
            }
        }

        // Also check if whole disk is directly an APFS container
        if (!found_apfs)
        {
            uint8_t sb_buf[512] = { 0 };
            if (dev.Read(sb_buf, 0, sizeof(sb_buf)))
            {
                const nx_superblock_t *sb = reinterpret_cast<const nx_superblock_t *>(sb_buf);
                if (sb->nx_magic == NX_MAGIC)
                {
                    found_apfs = true;
                    apfs_offset = 0;
                    apfs_size = drive_size;
                }
            }
        }
    }

    if (!found_apfs && hfsVolumes.empty())
    {
        if (dev_open) dev.Close();
        return false;
    }

    info.drive_number = drive_num;
    info.device_path = path;
    info.model_name = model;
    info.drive_size = drive_size;
    info.is_usb = is_usb;
    info.has_apfs = found_apfs;
    info.has_hfsplus = !hfsVolumes.empty();
    info.apfs_offset = apfs_offset;
    info.apfs_size = apfs_size;

    std::wstring model_w = Utf8ToWide(model);
    if (model_w.empty())
        model_w = L"Drive";

    std::wstring display = L"Drive " + std::to_wstring(drive_num) + L" - " +
                          (is_usb ? L"[USB] " : L"") + model_w +
                          L" (" + FormatSize(drive_size) + L")";
    SanitizeName(display);
    info.display_name = display;

    // Discover APFS volumes
    if (found_apfs && dev_open)
    {
        ApfsContainer container(&dev, apfs_offset, apfs_size);
        if (container.Init())
        {
            for (unsigned int fsid = 0; fsid < NX_MAX_FILE_SYSTEMS; fsid++)
            {
                apfs_superblock_t apsb = {};
                if (container.GetVolumeInfo(fsid, apsb))
                {
                    VolumeEntryInfo vol = {};
                    vol.volume_index = (unsigned int)info.volumes.size();
                    vol.fs_type = FsType::APFS;
                    const char *name_ptr = reinterpret_cast<const char *>(apsb.apfs_volname);
                    if (name_ptr && name_ptr[0] != 0)
                        vol.volume_name = name_ptr;
                    else
                        vol.volume_name = "Volume_" + std::to_string(fsid);

                    vol.volume_name_w = Utf8ToWide(vol.volume_name);
                    SanitizeName(vol.volume_name_w);
                    vol.total_bytes = apfs_size;
                    vol.free_bytes = container.GetFreeBlocks() * container.GetBlocksize();
                    vol.volume_offset = apfs_offset;
                    vol.sector_size = sector_size;

                    info.volumes.push_back(vol);
                }
            }
        }
    }

    if (dev_open) dev.Close();

    // Add HFS+ volumes
    for (auto &hvol : hfsVolumes)
    {
        hvol.volume_index = (unsigned int)info.volumes.size();
        info.volumes.push_back(hvol);
    }

    return !info.volumes.empty();
}

std::vector<ApfsDriveInfo> DriveEnumerator::EnumerateApfsDrives(bool *pAccessDeniedOccurred)
{
    if (pAccessDeniedOccurred)
        *pAccessDeniedOccurred = false;

    std::vector<ApfsDriveInfo> result;
    bool any_access_denied = false;

    for (int i = 0; i < 32; i++)
    {
        bool access_denied = false;
        ApfsDriveInfo info = {};
        if (InspectDrive(i, info, access_denied))
        {
            result.push_back(info);
        }
        else if (access_denied)
        {
            any_access_denied = true;
        }
    }

    if (pAccessDeniedOccurred)
        *pAccessDeniedOccurred = any_access_denied;

    return result;
}
