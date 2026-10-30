#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>

#include "drive_enumerator.h"
#include <ApfsLib/DeviceWinPhys.h>
#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/ApfsVolume.h>
#include <ApfsLib/ApfsDir.h>
#include "include/fsplugin.h"

struct FindContext {
    std::vector<WIN32_FIND_DATAW> entries;
    size_t current_index;
};

struct CachedVolume {
    std::unique_ptr<DeviceWinPhys> device;
    std::unique_ptr<ApfsContainer> container;
    std::unique_ptr<ApfsVolume> volume;
    std::unique_ptr<ApfsDir> dir;
    int drive_number;
    unsigned int volume_index;
};

class ApfsVFS {
public:
    static ApfsVFS &Instance();

    void Init(int pluginNr, tProgressProcW pProg, tLogProcW pLog, tRequestProcW pReq);

    HANDLE FindFirst(const WCHAR *path, WIN32_FIND_DATAW *findData);
    BOOL FindNext(HANDLE hFind, WIN32_FIND_DATAW *findData);
    int FindClose(HANDLE hFind);

    int GetFile(const WCHAR *remoteName, const WCHAR *localName, int copyFlags, RemoteInfoStruct *remoteInfo);
    int ExecuteFile(HWND hWnd, const WCHAR *remoteName, const WCHAR *verb);

    int GetPreviewBitmap(const WCHAR *remoteName, int width, int height, HBITMAP *returnedBitmap);
    bool ReadFileData(const WCHAR *remoteName, std::vector<uint8_t> &out_data, size_t max_bytes);

    int ExtractCustomIcon(const WCHAR *remoteName, int extractFlags, HICON *theIcon);
    void ShowAboutDialog(HWND hWnd);
    static std::string GetAboutText();

    void SetHInstance(HINSTANCE hInst) { m_hInstance = hInst; }
    HINSTANCE GetHInstance() const { return m_hInstance; }

    void InvalidateDrives();

private:
    ApfsVFS();
    ~ApfsVFS();

    std::shared_ptr<CachedVolume> GetOrMountVolume(int drive_num, unsigned int vol_index, const std::string &vol_name);
    bool ResolvePath(CachedVolume &cv, const std::vector<std::string> &parts, uint64_t &out_inode, ApfsDir::Inode &out_ino_rec);

    HINSTANCE m_hInstance;
    int m_pluginNr;
    tProgressProcW m_progressProc;
    tLogProcW m_logProc;
    tRequestProcW m_requestProc;

    std::mutex m_mutex;
    std::vector<ApfsDriveInfo> m_cached_drives;
    bool m_drives_scanned;
    bool m_access_denied_occurred;

    // Cache key: (drive_number << 16) | volume_index
    std::map<uint32_t, std::shared_ptr<CachedVolume>> m_open_volumes;
};
