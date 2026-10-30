#include "drive_enumerator.h"
#include <ApfsLib/DeviceWinPhys.h>
#include <ApfsLib/GptPartitionMap.h>
#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/DiskStruct.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>

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

    CloseHandle(hDrive);

    // Open through DeviceWinPhys for APFS analysis
    std::string utf8_path = "\\\\.\\PhysicalDrive" + std::to_string(drive_num);
    DeviceWinPhys dev;
    if (!dev.Open(utf8_path.c_str()))
        return false;

    uint64_t apfs_offset = 0;
    uint64_t apfs_size = 0;
    bool found_apfs = false;

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

    if (!found_apfs)
    {
        dev.Close();
        return false;
    }

    // Attempt to initialize APFS container
    ApfsContainer container(&dev, apfs_offset, apfs_size);
    if (!container.Init())
    {
        dev.Close();
        return false;
    }

    info.drive_number = drive_num;
    info.device_path = path;
    info.model_name = model;
    info.drive_size = drive_size;
    info.is_usb = is_usb;
    info.has_apfs = true;
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

    // Discover volumes
    for (unsigned int fsid = 0; fsid < NX_MAX_FILE_SYSTEMS; fsid++)
    {
        apfs_superblock_t apsb = {};
        if (container.GetVolumeInfo(fsid, apsb))
        {
            VolumeEntryInfo vol = {};
            vol.volume_index = fsid;
            const char *name_ptr = reinterpret_cast<const char *>(apsb.apfs_volname);
            if (name_ptr && name_ptr[0] != 0)
                vol.volume_name = name_ptr;
            else
                vol.volume_name = "Volume_" + std::to_string(fsid);

            vol.volume_name_w = Utf8ToWide(vol.volume_name);
            SanitizeName(vol.volume_name_w);
            vol.total_bytes = apfs_size;
            vol.free_bytes = container.GetFreeBlocks() * container.GetBlocksize();

            info.volumes.push_back(vol);
        }
    }

    dev.Close();
    return true;
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
