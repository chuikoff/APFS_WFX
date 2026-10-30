#include "include/fsplugin.h"
#include "apfs_vfs.h"

#include <windows.h>
#include <string>

// DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        ApfsVFS::Instance().SetHInstance(hModule);
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

// -------------------------------------------------------------
// Total Commander WFX API exports
// -------------------------------------------------------------

extern "C" {

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen)
{
    if (DefRootName && maxlen > 0)
    {
        strncpy_s(DefRootName, maxlen, "Mac APFS Drives", _TRUNCATE);
    }
}

int __stdcall FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc pLogProc, tRequestProc pRequestProc)
{
    return 0;
}

int __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProc, tLogProcW pLogProc, tRequestProcW pRequestProc)
{
    ApfsVFS::Instance().Init(PluginNr, pProgressProc, pLogProc, pRequestProc);
    return 0;
}

HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW *FindData)
{
    return ApfsVFS::Instance().FindFirst(Path, FindData);
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA *FindData)
{
    if (!Path || !FindData)
        return INVALID_HANDLE_VALUE;

    int len = MultiByteToWideChar(CP_ACP, 0, Path, -1, nullptr, 0);
    std::wstring path_w(len, 0);
    MultiByteToWideChar(CP_ACP, 0, Path, -1, &path_w[0], len);

    WIN32_FIND_DATAW fdw = {};
    HANDLE h = ApfsVFS::Instance().FindFirst(path_w.c_str(), &fdw);
    if (h != INVALID_HANDLE_VALUE)
    {
        FindData->dwFileAttributes = fdw.dwFileAttributes;
        FindData->ftCreationTime = fdw.ftCreationTime;
        FindData->ftLastAccessTime = fdw.ftLastAccessTime;
        FindData->ftLastWriteTime = fdw.ftLastWriteTime;
        FindData->nFileSizeHigh = fdw.nFileSizeHigh;
        FindData->nFileSizeLow = fdw.nFileSizeLow;
        WideCharToMultiByte(CP_ACP, 0, fdw.cFileName, -1, FindData->cFileName, MAX_PATH, nullptr, nullptr);
    }
    return h;
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW *FindData)
{
    return ApfsVFS::Instance().FindNext(Hdl, FindData);
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA *FindData)
{
    WIN32_FIND_DATAW fdw = {};
    BOOL res = ApfsVFS::Instance().FindNext(Hdl, &fdw);
    if (res && FindData)
    {
        FindData->dwFileAttributes = fdw.dwFileAttributes;
        FindData->ftCreationTime = fdw.ftCreationTime;
        FindData->ftLastAccessTime = fdw.ftLastAccessTime;
        FindData->ftLastWriteTime = fdw.ftLastWriteTime;
        FindData->nFileSizeHigh = fdw.nFileSizeHigh;
        FindData->nFileSizeLow = fdw.nFileSizeLow;
        WideCharToMultiByte(CP_ACP, 0, fdw.cFileName, -1, FindData->cFileName, MAX_PATH, nullptr, nullptr);
    }
    return res;
}

int __stdcall FsFindClose(HANDLE Hdl)
{
    return ApfsVFS::Instance().FindClose(Hdl);
}

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* RemoteInfo)
{
    return ApfsVFS::Instance().GetFile(RemoteName, LocalName, CopyFlags, RemoteInfo);
}

int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* RemoteInfo)
{
    int rlen = MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, nullptr, 0);
    std::wstring rname_w(rlen, 0);
    MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, &rname_w[0], rlen);

    int llen = MultiByteToWideChar(CP_ACP, 0, LocalName, -1, nullptr, 0);
    std::wstring lname_w(llen, 0);
    MultiByteToWideChar(CP_ACP, 0, LocalName, -1, &lname_w[0], llen);

    return ApfsVFS::Instance().GetFile(rname_w.c_str(), lname_w.c_str(), CopyFlags, RemoteInfo);
}

int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb)
{
    return ApfsVFS::Instance().ExecuteFile(MainWin, RemoteName, Verb);
}

int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb)
{
    std::wstring rname_w;
    if (RemoteName)
    {
        int rlen = MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, nullptr, 0);
        rname_w.resize(rlen);
        MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, &rname_w[0], rlen);
    }
    std::wstring verb_w;
    if (Verb)
    {
        int vlen = MultiByteToWideChar(CP_ACP, 0, Verb, -1, nullptr, 0);
        verb_w.resize(vlen);
        MultiByteToWideChar(CP_ACP, 0, Verb, -1, &verb_w[0], vlen);
    }

    return ApfsVFS::Instance().ExecuteFile(MainWin, rname_w.c_str(), Verb ? verb_w.c_str() : nullptr);
}

void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int Starting, int Operation)
{
    if (Starting == FS_STATUS_START && Operation == FS_STATUS_OP_LIST)
    {
        if (RemoteDir && (wcscmp(RemoteDir, L"\\") == 0 || wcscmp(RemoteDir, L"/") == 0 || RemoteDir[0] == 0))
        {
            ApfsVFS::Instance().InvalidateDrives();
        }
    }
}

void __stdcall FsStatusInfo(char* RemoteDir, int Starting, int Operation)
{
    if (Starting == FS_STATUS_START && Operation == FS_STATUS_OP_LIST)
    {
        if (RemoteDir && (strcmp(RemoteDir, "\\") == 0 || strcmp(RemoteDir, "/") == 0 || RemoteDir[0] == 0))
        {
            ApfsVFS::Instance().InvalidateDrives();
        }
    }
}

int __stdcall FsGetPreviewBitmapW(WCHAR* RemoteName, int width, int height, HBITMAP* ReturnedBitmap)
{
    return ApfsVFS::Instance().GetPreviewBitmap(RemoteName, width, height, ReturnedBitmap);
}

int __stdcall FsGetPreviewBitmap(char* RemoteName, int width, int height, HBITMAP* ReturnedBitmap)
{
    if (!RemoteName || !ReturnedBitmap) return FS_BITMAP_NONE;
    int len = MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, nullptr, 0);
    std::wstring rname_w(len, 0);
    MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, &rname_w[0], len);
    return ApfsVFS::Instance().GetPreviewBitmap(rname_w.c_str(), width, height, ReturnedBitmap);
}

int __stdcall FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, HICON* TheIcon)
{
    return ApfsVFS::Instance().ExtractCustomIcon(RemoteName, ExtractFlags, TheIcon);
}

int __stdcall FsExtractCustomIcon(char* RemoteName, int ExtractFlags, HICON* TheIcon)
{
    if (!RemoteName) return ApfsVFS::Instance().ExtractCustomIcon(nullptr, ExtractFlags, TheIcon);
    int len = MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, nullptr, 0);
    std::wstring rname_w(len, 0);
    MultiByteToWideChar(CP_ACP, 0, RemoteName, -1, &rname_w[0], len);
    return ApfsVFS::Instance().ExtractCustomIcon(rname_w.c_str(), ExtractFlags, TheIcon);
}

} // extern "C"
