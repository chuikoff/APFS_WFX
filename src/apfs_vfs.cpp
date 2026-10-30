#include "apfs_vfs.h"
#include <ApfsLib/DiskStruct.h>
#include <ApfsLib/Decmpfs.h>

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <mutex>

#include <gdiplus.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include "res/resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

static std::once_flag g_gdiplusInitOnce;
static ULONG_PTR g_gdiplusToken = 0;

static void EnsureGdiplusInitialized()
{
    std::call_once(g_gdiplusInitOnce, []() {
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, nullptr);
    });
}

static bool GenerateThumbnailGdiplus(const void *data, size_t size, int width, int height, HBITMAP *outHBitmap)
{
    if (!data || size == 0 || width <= 0 || height <= 0 || !outHBitmap)
        return false;

    EnsureGdiplusInitialized();

    IStream *pStream = SHCreateMemStream(reinterpret_cast<const BYTE *>(data), (UINT)size);
    if (!pStream)
        return false;

    Gdiplus::Bitmap *pOrig = Gdiplus::Bitmap::FromStream(pStream);
    pStream->Release();

    if (!pOrig || pOrig->GetLastStatus() != Gdiplus::Ok)
    {
        delete pOrig;
        return false;
    }

    UINT origW = pOrig->GetWidth();
    UINT origH = pOrig->GetHeight();
    if (origW == 0 || origH == 0)
    {
        delete pOrig;
        return false;
    }

    // Preserve aspect ratio
    double scaleW = static_cast<double>(width) / static_cast<double>(origW);
    double scaleH = static_cast<double>(height) / static_cast<double>(origH);
    double scale = (scaleW < scaleH) ? scaleW : scaleH;
    if (scale > 1.0) scale = 1.0;

    int thumbW = static_cast<int>(origW * scale);
    int thumbH = static_cast<int>(origH * scale);
    if (thumbW < 1) thumbW = 1;
    if (thumbH < 1) thumbH = 1;

    Gdiplus::Bitmap thumb(thumbW, thumbH, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&thumb);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.Clear(Gdiplus::Color(255, 255, 255));
        g.DrawImage(pOrig, 0, 0, thumbW, thumbH);
    }
    delete pOrig;

    HBITMAP hBmp = nullptr;
    if (thumb.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hBmp) != Gdiplus::Ok || !hBmp)
    {
        return false;
    }

    *outHBitmap = hBmp;
    return true;
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

static std::string WideToUtf8(const std::wstring &wstr)
{
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string res(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &res[0], len, nullptr, nullptr);
    return res;
}

static FILETIME ApfsNsToFileTime(uint64_t ns)
{
    FILETIME ft = { 0, 0 };
    if (ns == 0) return ft;
    uint64_t intervals = 116444736000000000ULL + (ns / 100);
    ft.dwLowDateTime = (DWORD)(intervals & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(intervals >> 32);
    return ft;
}

static std::vector<std::wstring> SplitPath(const WCHAR *path)
{
    std::vector<std::wstring> parts;
    if (!path) return parts;

    std::wstring p(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');

    size_t start = 0;
    while (start < p.size() && p[start] == L'\\')
        start++;

    while (start < p.size())
    {
        size_t end = p.find(L'\\', start);
        if (end == std::wstring::npos)
        {
            parts.push_back(p.substr(start));
            break;
        }
        else
        {
            if (end > start)
                parts.push_back(p.substr(start, end - start));
            start = end + 1;
        }
    }
    return parts;
}

ApfsVFS &ApfsVFS::Instance()
{
    static ApfsVFS s_instance;
    return s_instance;
}

ApfsVFS::ApfsVFS()
    : m_hInstance(nullptr)
    , m_pluginNr(0)
    , m_progressProc(nullptr)
    , m_logProc(nullptr)
    , m_requestProc(nullptr)
    , m_drives_scanned(false)
    , m_access_denied_occurred(false)
{
}

ApfsVFS::~ApfsVFS()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open_volumes.clear();
}

void ApfsVFS::Init(int pluginNr, tProgressProcW pProg, tLogProcW pLog, tRequestProcW pReq)
{
    m_pluginNr = pluginNr;
    m_progressProc = pProg;
    m_logProc = pLog;
    m_requestProc = pReq;
}

void ApfsVFS::InvalidateDrives()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_drives_scanned = false;
    m_open_volumes.clear();
    m_open_hfs_volumes.clear();
}

std::shared_ptr<CachedVolume> ApfsVFS::GetOrMountVolume(int drive_num, unsigned int vol_index, const std::string &vol_name)
{
    uint32_t key = (static_cast<uint32_t>(drive_num) << 16) | (vol_index & 0xFFFF);
    auto it = m_open_volumes.find(key);
    if (it != m_open_volumes.end() && it->second)
        return it->second;

    const ApfsDriveInfo *drive_info = nullptr;
    for (const auto &d : m_cached_drives)
    {
        if (d.drive_number == drive_num)
        {
            drive_info = &d;
            break;
        }
    }
    if (!drive_info)
        return nullptr;

    auto cv = std::make_shared<CachedVolume>();
    cv->drive_number = drive_num;
    cv->volume_index = vol_index;

    std::string dev_path = "\\\\.\\PhysicalDrive" + std::to_string(drive_num);
    cv->device = std::unique_ptr<DeviceWinPhys>(new DeviceWinPhys());
    if (!cv->device->Open(dev_path.c_str()))
        return nullptr;

    cv->container = std::unique_ptr<ApfsContainer>(new ApfsContainer(cv->device.get(), drive_info->apfs_offset, drive_info->apfs_size));
    if (!cv->container->Init())
        return nullptr;

    cv->volume = std::unique_ptr<ApfsVolume>(cv->container->GetVolume(vol_index));
    if (!cv->volume)
        return nullptr;

    cv->dir = std::unique_ptr<ApfsDir>(new ApfsDir(*cv->volume));

    m_open_volumes[key] = cv;
    return cv;
}

bool ApfsVFS::ResolvePath(CachedVolume &cv, const std::vector<std::string> &parts, uint64_t &out_inode, ApfsDir::Inode &out_ino_rec)
{
    if (!cv.dir) return false;

    uint64_t cur_inode = ROOT_DIR_INO_NUM; // Start at Inode 2 (real root directory)
    size_t start_idx = 0;

    if (!parts.empty())
    {
        if (parts[0] == "root")
        {
            ApfsDir::DirRec test_drec;
            if (!cv.dir->LookupName(test_drec, ROOT_DIR_INO_NUM, "root"))
            {
                cur_inode = ROOT_DIR_INO_NUM;
                start_idx = 1;
            }
        }
        else if (parts[0] == "private-dir")
        {
            cur_inode = PRIV_DIR_INO_NUM; // 3
            start_idx = 1;
        }
    }

    for (size_t i = start_idx; i < parts.size(); i++)
    {
        const std::string &part = parts[i];
        ApfsDir::DirRec drec;
        bool found = cv.dir->LookupName(drec, cur_inode, part.c_str());

        if (!found)
        {
            // Robust fallback: search directory listing directly
            std::vector<ApfsDir::DirRec> list;
            if (cv.dir->ListDirectory(list, cur_inode))
            {
                for (const auto &item : list)
                {
                    if (item.name == part || _stricmp(item.name.c_str(), part.c_str()) == 0)
                    {
                        drec = item;
                        found = true;
                        break;
                    }
                }
            }
        }

        if (!found)
            return false;

        cur_inode = drec.file_id;
    }

    out_inode = cur_inode;
    cv.dir->GetInode(out_ino_rec, cur_inode);
    return true;
}

HANDLE ApfsVFS::FindFirst(const WCHAR *path, WIN32_FIND_DATAW *findData)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!findData)
        return INVALID_HANDLE_VALUE;

    std::vector<std::wstring> parts = SplitPath(path);
    auto ctx = std::make_unique<FindContext>();
    ctx->current_index = 0;

    // ROOT: List physical APFS drives
    if (parts.empty())
    {
        m_cached_drives = DriveEnumerator::EnumerateApfsDrives(&m_access_denied_occurred);
        m_drives_scanned = true;

        if (m_cached_drives.empty())
        {
            WIN32_FIND_DATAW fd = {};
            fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
            if (m_access_denied_occurred)
            {
                wcscpy_s(fd.cFileName, L"[!] Run Total Commander as Administrator to access Physical Drives.txt");
            }
            else
            {
                wcscpy_s(fd.cFileName, L"[No Mac drives detected (APFS / HFS+) - Connect Mac USB drive and press Ctrl+R].txt");
            }
            ctx->entries.push_back(fd);
        }
        else
        {
            for (const auto &d : m_cached_drives)
            {
                WIN32_FIND_DATAW fd = {};
                fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                wcsncpy_s(fd.cFileName, d.display_name.c_str(), _TRUNCATE);
                ctx->entries.push_back(fd);
            }
        }
    }
    // LEVEL 1: List volumes in the chosen drive
    else if (parts.size() == 1)
    {
        const ApfsDriveInfo *found_drive = nullptr;
        for (const auto &d : m_cached_drives)
        {
            if (d.display_name == parts[0])
            {
                found_drive = &d;
                break;
            }
        }

        if (!found_drive)
        {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }

        for (const auto &v : found_drive->volumes)
        {
            WIN32_FIND_DATAW fd = {};
            fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            wcsncpy_s(fd.cFileName, v.volume_name_w.c_str(), _TRUNCATE);
            fd.nFileSizeLow = (DWORD)(v.total_bytes & 0xFFFFFFFF);
            fd.nFileSizeHigh = (DWORD)(v.total_bytes >> 32);
            ctx->entries.push_back(fd);
        }

        if (ctx->entries.empty())
        {
            WIN32_FIND_DATAW fd = {};
            fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY;
            wcscpy_s(fd.cFileName, L"[No volumes accessible].txt");
            ctx->entries.push_back(fd);
        }
    }
    // LEVEL 2+: Inside a volume (directory listing)
    else
    {
        const ApfsDriveInfo *found_drive = nullptr;
        for (const auto &d : m_cached_drives)
        {
            if (d.display_name == parts[0])
            {
                found_drive = &d;
                break;
            }
        }
        if (!found_drive)
        {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }

        const VolumeEntryInfo *found_vol = nullptr;
        for (const auto &v : found_drive->volumes)
        {
            if (v.volume_name_w == parts[1])
            {
                found_vol = &v;
                break;
            }
        }
        if (!found_vol)
        {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }

        if (found_vol->fs_type == FsType::HFSPlus)
        {
            auto chv = GetOrMountHfsVolume(found_drive->drive_number, found_vol->volume_offset, found_vol->sector_size);
            if (!chv || !chv->volume)
            {
                SetLastError(ERROR_NOT_READY);
                return INVALID_HANDLE_VALUE;
            }

            uint32_t parentCNID = HfsPlus::kHFSPlusRootFolderID;
            if (parts.size() > 2)
            {
                std::vector<std::wstring> subparts(parts.begin() + 2, parts.end());
                HfsPlus::CatalogItem dirItem;
                if (!ResolveHfsPath(*chv->volume, subparts, dirItem) || !dirItem.isDir)
                {
                    SetLastError(ERROR_PATH_NOT_FOUND);
                    return INVALID_HANDLE_VALUE;
                }
                parentCNID = dirItem.cnid;
            }

            std::vector<HfsPlus::CatalogItem> hfsItems;
            chv->volume->ListDirectory(parentCNID, hfsItems);

            for (const auto &hitem : hfsItems)
            {
                if (hitem.name.empty()) continue;
                WIN32_FIND_DATAW fd = {};
                wcsncpy_s(fd.cFileName, hitem.name.c_str(), _TRUNCATE);

                if (hitem.isDir)
                {
                    fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                }
                else
                {
                    fd.dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
                    uint64_t fsize = hitem.dataSize;
                    fd.nFileSizeLow  = (DWORD)(fsize & 0xFFFFFFFF);
                    fd.nFileSizeHigh = (DWORD)(fsize >> 32);
                }

                fd.ftCreationTime   = HfsPlus::HfsPlusTimeToFileTime(hitem.createTime);
                fd.ftLastWriteTime  = HfsPlus::HfsPlusTimeToFileTime(hitem.modifyTime);
                fd.ftLastAccessTime = fd.ftLastWriteTime;

                ctx->entries.push_back(fd);
            }
        }
        else
        {
            auto cv = GetOrMountVolume(found_drive->drive_number, found_vol->volume_index, found_vol->volume_name);
            if (!cv || !cv->dir)
            {
                SetLastError(ERROR_NOT_READY);
                return INVALID_HANDLE_VALUE;
            }

            std::vector<std::string> subpath;
            for (size_t i = 2; i < parts.size(); i++)
            {
                subpath.push_back(WideToUtf8(parts[i]));
            }

            uint64_t target_inode = ROOT_DIR_INO_NUM;
            ApfsDir::Inode target_rec;
            if (!subpath.empty())
            {
                if (!ResolvePath(*cv, subpath, target_inode, target_rec))
                {
                    SetLastError(ERROR_PATH_NOT_FOUND);
                    return INVALID_HANDLE_VALUE;
                }
            }
            else
            {
                // Volume root level: check if Inode 2 has entries
                std::vector<ApfsDir::DirRec> test_list;
                if (!cv->dir->ListDirectory(test_list, ROOT_DIR_INO_NUM) || test_list.empty())
                {
                    std::vector<ApfsDir::DirRec> parent_list;
                    if (cv->dir->ListDirectory(parent_list, ROOT_DIR_PARENT) && !parent_list.empty())
                    {
                        bool found_root = false;
                        for (const auto &p : parent_list)
                        {
                            if (p.name == "root")
                            {
                                target_inode = p.file_id;
                                found_root = true;
                                break;
                            }
                        }
                        if (!found_root)
                            target_inode = ROOT_DIR_PARENT;
                    }
                }
            }

            std::vector<ApfsDir::DirRec> dir_list;
            cv->dir->ListDirectory(dir_list, target_inode);

            for (const auto &item : dir_list)
            {
                if (item.name == "." || item.name == "..")
                    continue;

                WIN32_FIND_DATAW fd = {};
                std::wstring item_name_w = Utf8ToWide(item.name);
                wcsncpy_s(fd.cFileName, item_name_w.c_str(), _TRUNCATE);

                ApfsDir::Inode item_ino;
                bool has_ino = cv->dir->GetInode(item_ino, item.file_id);

                uint16_t drec_type = (item.flags & DREC_TYPE_MASK);
                bool is_dir = (drec_type == 4);
                if (has_ino && !is_dir)
                {
                    is_dir = ((item_ino.mode & 0170000) == 0040000);
                }

                if (has_ino)
                {
                    if (is_dir)
                    {
                        fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    }
                    else
                    {
                        fd.dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
                        uint64_t fsize = item_ino.uncompressed_size;
                        if ((item_ino.optional_present_flags & ApfsDir::Inode::INO_HAS_DSTREAM) && fsize == 0)
                        {
                            fsize = item_ino.ds_size;
                        }
                        fd.nFileSizeLow = (DWORD)(fsize & 0xFFFFFFFF);
                        fd.nFileSizeHigh = (DWORD)(fsize >> 32);
                    }

                    fd.ftCreationTime = ApfsNsToFileTime(item_ino.create_time);
                    fd.ftLastWriteTime = ApfsNsToFileTime(item_ino.mod_time);
                    fd.ftLastAccessTime = ApfsNsToFileTime(item_ino.access_time);
                }
                else
                {
                    if (is_dir)
                        fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    else
                        fd.dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
                }

                ctx->entries.push_back(fd);
            }
        }
    }

    if (ctx->entries.empty())
    {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    *findData = ctx->entries[0];
    ctx->current_index = 1;

    return reinterpret_cast<HANDLE>(ctx.release());
}

BOOL ApfsVFS::FindNext(HANDLE hFind, WIN32_FIND_DATAW *findData)
{
    if (!hFind || hFind == INVALID_HANDLE_VALUE || !findData)
        return FALSE;

    auto ctx = reinterpret_cast<FindContext *>(hFind);
    if (ctx->current_index >= ctx->entries.size())
    {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }

    *findData = ctx->entries[ctx->current_index++];
    return TRUE;
}

int ApfsVFS::FindClose(HANDLE hFind)
{
    if (hFind && hFind != INVALID_HANDLE_VALUE)
    {
        auto ctx = reinterpret_cast<FindContext *>(hFind);
        delete ctx;
    }
    return 0;
}

int ApfsVFS::GetFile(const WCHAR *remoteName, const WCHAR *localName, int copyFlags, RemoteInfoStruct *remoteInfo)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::wstring> parts = SplitPath(remoteName);

    if (parts.size() < 3)
        return FS_FILE_NOTFOUND;

    // Find Drive
    const ApfsDriveInfo *found_drive = nullptr;
    for (const auto &d : m_cached_drives)
    {
        if (d.display_name == parts[0])
        {
            found_drive = &d;
            break;
        }
    }
    if (!found_drive)
        return FS_FILE_NOTFOUND;

    // Find Volume
    const VolumeEntryInfo *found_vol = nullptr;
    for (const auto &v : found_drive->volumes)
    {
        if (v.volume_name_w == parts[1])
        {
            found_vol = &v;
            break;
        }
    }
    if (!found_vol)
        return FS_FILE_NOTFOUND;

    // --- Route by filesystem type ---
    if (found_vol->fs_type == FsType::HFSPlus)
    {
        auto chv = GetOrMountHfsVolume(found_drive->drive_number, found_vol->volume_offset, found_vol->sector_size);
        if (!chv || !chv->volume)
            return FS_FILE_READERROR;

        std::vector<std::wstring> subpartsW(parts.begin() + 2, parts.end());
        HfsPlus::CatalogItem hfsItem;
        if (!ResolveHfsPath(*chv->volume, subpartsW, hfsItem) || hfsItem.isDir)
            return FS_FILE_NOTFOUND;

        DWORD creationDisp = (copyFlags & FS_COPYFLAGS_OVERWRITE) ? CREATE_ALWAYS : CREATE_NEW;
        HANDLE hLocal = CreateFileW(localName, GENERIC_WRITE, 0, nullptr, creationDisp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLocal == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            return (err == ERROR_FILE_EXISTS) ? FS_FILE_EXISTS : FS_FILE_WRITEERROR;
        }

        uint64_t total_size = hfsItem.dataSize;
        constexpr size_t CHUNK = 256 * 1024;
        std::vector<uint8_t> buf(CHUNK);
        uint64_t offset = 0;
        bool ok = true;

        while (offset < total_size)
        {
            uint64_t toRead = std::min<uint64_t>(CHUNK, total_size - offset);
            uint64_t bytesRead = 0;
            if (!chv->volume->ReadFileData(hfsItem, offset, buf.data(), toRead, bytesRead) || bytesRead == 0)
            {
                ok = false;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(hLocal, buf.data(), (DWORD)bytesRead, &written, nullptr) || written != bytesRead)
            {
                ok = false;
                break;
            }
            offset += bytesRead;

            if (m_progressProc && total_size > 0)
            {
                int pct = (int)((offset * 100) / total_size);
                if (m_progressProc(m_pluginNr, const_cast<WCHAR*>(remoteName), const_cast<WCHAR*>(localName), pct) != 0)
                {
                    CloseHandle(hLocal);
                    DeleteFileW(localName);
                    return FS_FILE_USERABORT;
                }
            }
        }

        FILETIME ftMod = HfsPlus::HfsPlusTimeToFileTime(hfsItem.modifyTime);
        FILETIME ftCre = HfsPlus::HfsPlusTimeToFileTime(hfsItem.createTime);
        SetFileTime(hLocal, &ftCre, nullptr, &ftMod);
        CloseHandle(hLocal);
        if (!ok) { DeleteFileW(localName); return FS_FILE_READERROR; }
        return FS_FILE_OK;
    }

    auto cv = GetOrMountVolume(found_drive->drive_number, found_vol->volume_index, found_vol->volume_name);
    if (!cv || !cv->dir)
        return FS_FILE_READERROR;

    std::vector<std::string> subpath;
    for (size_t i = 2; i < parts.size(); i++)
        subpath.push_back(WideToUtf8(parts[i]));

    uint64_t target_inode = 0;
    ApfsDir::Inode ino_rec;
    if (!ResolvePath(*cv, subpath, target_inode, ino_rec))
        return FS_FILE_NOTFOUND;

    DWORD creationDisposition = (copyFlags & FS_COPYFLAGS_OVERWRITE) ? CREATE_ALWAYS : CREATE_NEW;
    HANDLE hLocal = CreateFileW(localName, GENERIC_WRITE, 0, nullptr, creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLocal == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_EXISTS)
            return FS_FILE_EXISTS;
        return FS_FILE_WRITEERROR;
    }

    bool is_compressed = (ino_rec.bsd_flags & APFS_UF_COMPRESSED) != 0 || (ino_rec.internal_flags & INODE_HAS_UNCOMPRESSED_SIZE) != 0;

    if (is_compressed)
    {
        std::vector<uint8_t> attr;
        if (cv->dir->GetAttribute(attr, target_inode, "com.apple.decmpfs"))
        {
            std::vector<uint8_t> decomp;
            if (DecompressFile(*cv->dir, target_inode, decomp, attr))
            {
                DWORD written = 0;
                if (!decomp.empty())
                {
                    WriteFile(hLocal, decomp.data(), (DWORD)decomp.size(), &written, nullptr);
                }
                if (m_progressProc)
                    m_progressProc(m_pluginNr, const_cast<WCHAR*>(remoteName), const_cast<WCHAR*>(localName), 100);

                CloseHandle(hLocal);
                return FS_FILE_OK;
            }
        }
    }

    uint64_t total_size = ino_rec.uncompressed_size;
    if ((ino_rec.optional_present_flags & ApfsDir::Inode::INO_HAS_DSTREAM) && total_size == 0)
    {
        total_size = ino_rec.ds_size;
    }

    constexpr size_t CHUNK_SIZE = 256 * 1024;
    std::vector<uint8_t> buffer(CHUNK_SIZE);
    uint64_t offset = 0;

    while (offset < total_size)
    {
        size_t cur_chunk = (size_t)std::min<uint64_t>(CHUNK_SIZE, total_size - offset);
        if (!cv->dir->ReadFile(buffer.data(), ino_rec.private_id, offset, cur_chunk))
        {
            CloseHandle(hLocal);
            return FS_FILE_READERROR;
        }

        DWORD written = 0;
        if (!WriteFile(hLocal, buffer.data(), (DWORD)cur_chunk, &written, nullptr) || written != cur_chunk)
        {
            CloseHandle(hLocal);
            return FS_FILE_WRITEERROR;
        }

        offset += cur_chunk;

        if (m_progressProc && total_size > 0)
        {
            int percent = (int)((offset * 100) / total_size);
            if (m_progressProc(m_pluginNr, const_cast<WCHAR*>(remoteName), const_cast<WCHAR*>(localName), percent) != 0)
            {
                CloseHandle(hLocal);
                DeleteFileW(localName);
                return FS_FILE_USERABORT;
            }
        }
    }

    FILETIME ftMod = ApfsNsToFileTime(ino_rec.mod_time);
    FILETIME ftCreate = ApfsNsToFileTime(ino_rec.create_time);
    SetFileTime(hLocal, &ftCreate, nullptr, &ftMod);

    CloseHandle(hLocal);
    return FS_FILE_OK;
}

bool ApfsVFS::ReadFileData(const WCHAR *remoteName, std::vector<uint8_t> &out_data, size_t max_bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::wstring> parts = SplitPath(remoteName);
    if (parts.size() < 3)
        return false;

    const ApfsDriveInfo *found_drive = nullptr;
    for (const auto &d : m_cached_drives)
    {
        if (d.display_name == parts[0])
        {
            found_drive = &d;
            break;
        }
    }
    if (!found_drive)
        return false;

    const VolumeEntryInfo *found_vol = nullptr;
    for (const auto &v : found_drive->volumes)
    {
        if (v.volume_name_w == parts[1])
        {
            found_vol = &v;
            break;
        }
    }
    if (!found_vol)
        return false;

    // --- Route by filesystem type ---
    if (found_vol->fs_type == FsType::HFSPlus)
    {
        auto chv = GetOrMountHfsVolume(found_drive->drive_number, found_vol->volume_offset, found_vol->sector_size);
        if (!chv || !chv->volume) return false;

        std::vector<std::wstring> subpartsW(parts.begin() + 2, parts.end());
        HfsPlus::CatalogItem hfsItem;
        if (!ResolveHfsPath(*chv->volume, subpartsW, hfsItem) || hfsItem.isDir) return false;

        uint64_t fsize = hfsItem.dataSize;
        if (fsize == 0 || fsize > max_bytes) return false;
        out_data.resize((size_t)fsize);
        uint64_t bytesRead = 0;
        bool ok = chv->volume->ReadFileData(hfsItem, 0, out_data.data(), fsize, bytesRead);
        if (!ok || bytesRead != fsize) { out_data.clear(); return false; }
        return true;
    }

    auto cv = GetOrMountVolume(found_drive->drive_number, found_vol->volume_index, found_vol->volume_name);
    if (!cv || !cv->dir)
        return false;

    std::vector<std::string> subpath;
    for (size_t i = 2; i < parts.size(); i++)
        subpath.push_back(WideToUtf8(parts[i]));

    uint64_t target_inode = 0;
    ApfsDir::Inode ino_rec;
    if (!ResolvePath(*cv, subpath, target_inode, ino_rec))
        return false;

    if ((ino_rec.mode & 0170000) == 0040000)
        return false;

    bool is_compressed = (ino_rec.bsd_flags & APFS_UF_COMPRESSED) != 0 || (ino_rec.internal_flags & INODE_HAS_UNCOMPRESSED_SIZE) != 0;
    if (is_compressed)
    {
        std::vector<uint8_t> attr;
        if (cv->dir->GetAttribute(attr, target_inode, "com.apple.decmpfs"))
        {
            if (DecompressFile(*cv->dir, target_inode, out_data, attr))
            {
                return true;
            }
        }
        return false;
    }

    uint64_t total_size = ino_rec.uncompressed_size;
    if ((ino_rec.optional_present_flags & ApfsDir::Inode::INO_HAS_DSTREAM) && total_size == 0)
    {
        total_size = ino_rec.ds_size;
    }

    if (total_size == 0 || total_size > max_bytes)
        return false;

    out_data.resize(static_cast<size_t>(total_size));
    return cv->dir->ReadFile(out_data.data(), ino_rec.private_id, 0, static_cast<size_t>(total_size));
}

int ApfsVFS::GetPreviewBitmap(const WCHAR *remoteName, int width, int height, HBITMAP *returnedBitmap)
{
    if (!remoteName || !returnedBitmap || width <= 0 || height <= 0)
        return FS_BITMAP_NONE;

    *returnedBitmap = nullptr;

    std::wstring rname(remoteName);
    size_t dot = rname.rfind(L'.');
    if (dot == std::wstring::npos)
        return FS_BITMAP_NONE;

    std::wstring ext = rname.substr(dot);
    for (auto &c : ext) c = towlower(c);

    // Formats supported for fast direct in-memory decode via GDI+
    static const std::unordered_set<std::wstring> gdiplus_exts = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".ico", L".tif", L".tiff"
    };

    // Extended formats supported by Total Commander Lister plugins / Shell (HEIC, WebP, PDF, Video, etc.)
    static const std::unordered_set<std::wstring> extract_exts = {
        L".heic", L".heif", L".webp", L".avif", L".psd", L".raw", L".cr2", L".nef",
        L".arw", L".dng", L".svg", L".pdf",
        L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".flv", L".webm"
    };

    bool is_gdi = (gdiplus_exts.find(ext) != gdiplus_exts.end());
    bool is_ext = (extract_exts.find(ext) != extract_exts.end());

    if (!is_gdi && !is_ext)
        return FS_BITMAP_NONE;

    if (is_gdi)
    {
        std::vector<uint8_t> data;
        // Limit to 30 MB for in-memory thumbnailing
        if (ReadFileData(remoteName, data, 30 * 1024 * 1024))
        {
            HBITMAP hBmp = nullptr;
            if (GenerateThumbnailGdiplus(data.data(), data.size(), width, height, &hBmp))
            {
                *returnedBitmap = hBmp;
                return FS_BITMAP_EXTRACTED | FS_BITMAP_CACHE;
            }
        }
    }

    // For HEIC, video, or if in-memory GDI+ fails: let Total Commander extract and use its thumbnailers
    return FS_BITMAP_EXTRACT_YOURSELF_ANDDELETE;
}

static HRESULT CALLBACK TaskDialogCallbackProc(HWND hwnd, UINT uNotification, WPARAM wParam, LPARAM lParam, LONG_PTR dwRefData)
{
    if (uNotification == TDN_HYPERLINK_CLICKED)
    {
        LPCWSTR url = reinterpret_cast<LPCWSTR>(lParam);
        ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

std::string ApfsVFS::GetAboutText()
{
    return
        "======================================================================\r\n"
        " Total Commander macOS File System Plugin (WFX)\r\n"
        " Поддержка: Apple APFS & HFS+ (Mac OS Extended)\r\n"
        " Версия: 1.2.0 (x64 / x86)\r\n"
        " Автор: chuikoff\r\n"
        " GitHub: https://github.com/chuikoff/APFS_WFX\r\n"
        "======================================================================\r\n\r\n"
        "Плагин предназначен для прямого просмотра, чтения и извлечения файлов\r\n"
        "с физических накопителей (USB-флешек, внешних SSD/HDD), отформатированных\r\n"
        "в файловых системах Apple APFS (macOS High Sierra — Sequoia) и\r\n"
        "HFS+ / Mac OS Extended (macOS Yosemite — Sequoia).\r\n\r\n"
        "Ключевые возможности:\r\n"
        "- Прямой доступ к физическим дискам Windows (\\\\.\\PhysicalDriveX).\r\n"
        "- Автоматический поиск GPT и MBR разделов с APFS и HFS+.\r\n"
        "- Поддержка томов APFS и HFS+ (включая каталоги B-Tree).\r\n"
        "- Прозрачная декомпрессия сжатых файлов macOS Decmpfs (LZVN, LZFSE, ZLIB).\r\n"
        "- Корректное отображение русских и Unicode-имен файлов.\r\n"
        "- Поддержка быстрого просмотра эскизов (Thumbnails / Ctrl+Shift+F1).\r\n"
        "- Режим работы строго Только для чтения (Read-Only) — безопасно для данных.\r\n\r\n"
        "Горячие клавиши Total Commander:\r\n"
        "- F3: Быстрый просмотр файла (Lister)\r\n"
        "- F5: Копирование файлов/папок на диск Windows\r\n"
        "- Enter: Открытие файла в ассоциированной программе Windows\r\n"
        "- Ctrl+R: Обновление списка подключенных дисков\r\n"
        "- Alt+Enter: Свойства плагина / Окно \"О программе\"\r\n\r\n"
        "======================================================================\r\n";
}

void ApfsVFS::ShowAboutDialog(HWND hWnd)
{
    typedef HRESULT (WINAPI *pfnTaskDialogIndirect)(const TASKDIALOGCONFIG *pTaskConfig, int *pnButton, int *pnRadioButton, BOOL *pfVerificationFlagChecked);

    HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");
    pfnTaskDialogIndirect pTaskDialogIndirect = hComCtl ? reinterpret_cast<pfnTaskDialogIndirect>(GetProcAddress(hComCtl, "TaskDialogIndirect")) : nullptr;

    const TASKDIALOG_BUTTON customButtons[] = {
        { 1001, L"Открыть GitHub репозиторий\nhttps://github.com/chuikoff/APFS_WFX" }
    };

    if (pTaskDialogIndirect)
    {
        TASKDIALOGCONFIG tc = { sizeof(tc) };
        tc.hwndParent = hWnd;
        tc.hInstance = m_hInstance;
        tc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = L"О плагине — macOS File System Plugin";
        tc.pszMainInstruction = L"Total Commander macOS File System Plugin (APFS & HFS+)";
        tc.pszContent =
            L"Версия: 1.2.0 (x64 / x86)\n"
            L"Разработчик: chuikoff\n"
            L"GitHub: <a href=\"https://github.com/chuikoff/APFS_WFX\">https://github.com/chuikoff/APFS_WFX</a>\n\n"
            L"Возможности:\n"
            L"• Прямой доступ к физическим накопителям и USB-дискам macOS\n"
            L"• Распознавание разделов Apple APFS и HFS+ (Mac OS Extended)\n"
            L"• Поддержка нескольких томов (Macintosh HD, Data, Recovery и др.)\n"
            L"• Декомпрессия сжатых файлов Apple Decmpfs (LZVN, LZFSE, ZLIB)\n"
            L"• Быстрый просмотр эскизов для фото (GDI+) и медиафайлов\n"
            L"• Режим строго только для чтения (Read-Only) — безопасность данных\n\n"
            L"Примечание: для прямого доступа к секторам дисков запускайте Total Commander от имени Администратора.";

        tc.pButtons = customButtons;
        tc.cButtons = 1;
        tc.dwCommonButtons = TDCBF_OK_BUTTON;
        tc.pfCallback = TaskDialogCallbackProc;

        HICON hIcon = (HICON)LoadImageW(m_hInstance, MAKEINTRESOURCEW(IDI_APFS_ICON), IMAGE_ICON, 32, 32, LR_SHARED);
        if (hIcon)
        {
            tc.dwFlags |= TDF_USE_HICON_MAIN;
            tc.hMainIcon = hIcon;
        }
        else
        {
            tc.pszMainIcon = TD_INFORMATION_ICON;
        }

        int buttonPressed = 0;
        HRESULT hr = pTaskDialogIndirect(&tc, &buttonPressed, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            if (buttonPressed == 1001)
            {
                ShellExecuteW(hWnd, L"open", L"https://github.com/chuikoff/APFS_WFX", nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (hComCtl) FreeLibrary(hComCtl);
            return;
        }
    }

    if (hComCtl) FreeLibrary(hComCtl);

    // Fallback MessageBox
    std::wstring msg =
        L"Total Commander macOS File System Plugin v1.2.0\n"
        L"Поддержка: Apple APFS & HFS+ (Mac OS Extended)\n"
        L"Разработчик: chuikoff\n"
        L"GitHub: https://github.com/chuikoff/APFS_WFX\n\n"
        L"Прямой доступ к физическим накопителям и USB-дискам macOS.\n"
        L"Режим: Только для чтения (Read-Only).\n"
        L"Требуются права Администратора для доступа к PhysicalDrive.\n\n"
        L"Нажмите ОК для перехода на страницу GitHub.";
    int res = MessageBoxW(hWnd, msg.c_str(), L"О плагине macOS File System", MB_OKCANCEL | MB_ICONINFORMATION);
    if (res == IDOK)
    {
        ShellExecuteW(hWnd, L"open", L"https://github.com/chuikoff/APFS_WFX", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

int ApfsVFS::ExecuteFile(HWND hWnd, const WCHAR *remoteName, const WCHAR *verb)
{
    if (verb && _wcsicmp(verb, L"properties") == 0)
    {
        ShowAboutDialog(hWnd);
        return FS_EXEC_OK;
    }

    if (!verb || _wcsicmp(verb, L"open") == 0)
    {
        return FS_EXEC_YOURSELF;
    }

    return FS_EXEC_ERROR;
}

int ApfsVFS::ExtractCustomIcon(const WCHAR *remoteName, int extractFlags, HICON *theIcon)
{
    if (!theIcon) return FS_ICON_USEDEFAULT;

    bool usePluginIcon = false;
    if (!remoteName || remoteName[0] == 0 || wcscmp(remoteName, L"\\") == 0 || wcscmp(remoteName, L"/") == 0)
    {
        usePluginIcon = true;
    }
    else if (wcsstr(remoteName, L"О программе") != nullptr || wcsstr(remoteName, L"About") != nullptr)
    {
        usePluginIcon = true;
    }
    else
    {
        std::vector<std::wstring> parts = SplitPath(remoteName);
        if (parts.size() <= 2)
        {
            usePluginIcon = true; // Physical drive (level 1) or volume (level 2)
        }
    }

    if (usePluginIcon)
    {
        int size = (extractFlags & FS_ICONFLAG_SMALL) ? 16 : 32;
        HICON hIcon = (HICON)LoadImageW(m_hInstance, MAKEINTRESOURCEW(IDI_APFS_ICON), IMAGE_ICON, size, size, LR_SHARED);
        if (hIcon)
        {
            *theIcon = hIcon;
            return FS_ICON_EXTRACTED; // Icon loaded with LR_SHARED, TC must not destroy it
        }
    }

    return FS_ICON_USEDEFAULT;
}

// ============================================================
// HFS+ Volume Mount & Path Resolution
// ============================================================

std::shared_ptr<CachedHfsVolume> ApfsVFS::GetOrMountHfsVolume(int drive_num, uint64_t vol_offset, uint32_t sector_size)
{
    uint64_t key = (uint64_t)drive_num * 10000000ULL + vol_offset / (1024 * 1024);
    auto it = m_open_hfs_volumes.find(key);
    if (it != m_open_hfs_volumes.end() && it->second && it->second->volume && it->second->volume->IsOpen())
        return it->second;

    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(drive_num);
    HANDLE hRaw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hRaw == INVALID_HANDLE_VALUE) return nullptr;

    auto chv = std::make_shared<CachedHfsVolume>();
    chv->hDevice = hRaw;
    chv->drive_number = drive_num;
    chv->volume_offset = vol_offset;
    chv->sector_size = sector_size;
    chv->volume = std::make_unique<HfsPlus::HfsPlusVolume>();

    if (!chv->volume->Open(hRaw, vol_offset, 0, sector_size))
    {
        return nullptr;
    }

    m_open_hfs_volumes[key] = chv;
    return chv;
}

bool ApfsVFS::FindHfsCatalogItem(HfsPlus::HfsPlusVolume &vol, uint32_t parentCNID, const std::wstring &name, HfsPlus::CatalogItem &out)
{
    std::vector<HfsPlus::CatalogItem> items;
    if (!vol.ListDirectory(parentCNID, items)) return false;
    for (auto &item : items) {
        if (_wcsicmp(item.name.c_str(), name.c_str()) == 0) {
            out = item;
            return true;
        }
    }
    return false;
}

bool ApfsVFS::ResolveHfsPath(HfsPlus::HfsPlusVolume &vol, const std::vector<std::wstring> &parts, HfsPlus::CatalogItem &out_item)
{
    uint32_t curCNID = HfsPlus::kHFSPlusRootFolderID;
    HfsPlus::CatalogItem cur = {};
    cur.isDir = true;
    cur.cnid = curCNID;

    for (size_t i = 0; i < parts.size(); i++) {
        HfsPlus::CatalogItem child;
        if (!FindHfsCatalogItem(vol, curCNID, parts[i], child))
            return false;
        cur = child;
        curCNID = cur.cnid;
    }
    out_item = cur;
    return true;
}
