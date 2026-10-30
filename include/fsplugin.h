#pragma once
#include <windows.h>

// Return values for FsGetFile
#define FS_FILE_OK                  0
#define FS_FILE_EXISTS              1
#define FS_FILE_NOTFOUND            2
#define FS_FILE_READERROR           3
#define FS_FILE_WRITEERROR          4
#define FS_FILE_USERABORT           5
#define FS_FILE_NOTSUPPORTED        6
#define FS_FILE_EXISTSRESUMEALLOWED 7

// Return values for FsExecuteFile
#define FS_EXEC_OK                 0
#define FS_EXEC_ERROR              1
#define FS_EXEC_YOURSELF          -1
#define FS_EXEC_SYMLINK           -2

// Flags for FsGetFile / FsPutFile
#define FS_COPYFLAGS_OVERWRITE           1
#define FS_COPYFLAGS_RESUME              2
#define FS_COPYFLAGS_MOVE                4
#define FS_COPYFLAGS_EXISTS_SAMECASE     8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 16

// Request types for tRequestProc
#define RT_Other               0
#define RT_UserName            1
#define RT_Password            2
#define RT_Account             3
#define RT_UserNameFirewall    4
#define RT_PasswordFirewall    5
#define RT_TargetDir           6
#define RT_URL                 7
#define RT_MsgOK               8
#define RT_MsgYesNo            9
#define RT_MsgOKCancel        10

// Message types for tLogProc
#define MSGTYPE_CONNECT         1
#define MSGTYPE_DISCONNECT      2
#define MSGTYPE_DETAILS         3
#define MSGTYPE_TRANSFERCOMPLETE 4
#define MSGTYPE_CONNECTCOMPLETE 5
#define MSGTYPE_IMPORTANTERROR  6
#define MSGTYPE_PANICERROR      7

// Flags for FsStatusInfo
#define FS_STATUS_START         0
#define FS_STATUS_END           1

#define FS_STATUS_OP_LIST               1
#define FS_STATUS_OP_GET_SINGLE         2
#define FS_STATUS_OP_GET_MULTI          3
#define FS_STATUS_OP_PUT_SINGLE         4
#define FS_STATUS_OP_PUT_MULTI          5
#define FS_STATUS_OP_RENMOV_SINGLE      6
#define FS_STATUS_OP_RENMOV_MULTI       7
#define FS_STATUS_OP_DELETE             8
#define FS_STATUS_OP_ATTRIB             9
#define FS_STATUS_OP_MKDIR             10
#define FS_STATUS_OP_EXEC              11
#define FS_STATUS_OP_CALCSIZE          12
#define FS_STATUS_OP_SEARCH            13
#define FS_STATUS_OP_SEARCH_TEXT       14
#define FS_STATUS_OP_SYNC_SEARCH       15
#define FS_STATUS_OP_SYNC_GET          16
#define FS_STATUS_OP_SYNC_PUT          17
#define FS_STATUS_OP_SYNC_DELETE       18
#define FS_STATUS_OP_GET_MULTI_THREAD  19
#define FS_STATUS_OP_PUT_MULTI_THREAD  20

// Flags for FsExtractCustomIcon
#define FS_ICONFLAG_SMALL 1
#define FS_ICONFLAG_BACKGROUND 2

#define FS_ICON_USEDEFAULT 0
#define FS_ICON_EXTRACTED 1
#define FS_ICON_EXTRACTED_DESTROY 2
#define FS_ICON_DELAYED 3

// Return values for FsGetPreviewBitmap
#define FS_BITMAP_NONE 0
#define FS_BITMAP_EXTRACTED 1
#define FS_BITMAP_EXTRACT_YOURSELF 2
#define FS_BITMAP_EXTRACT_YOURSELF_ANDDELETE 3
#define FS_BITMAP_CACHE 256

typedef struct {
    DWORD SizeLow;
    DWORD SizeHigh;
    FILETIME LastWriteTime;
    DWORD Attr;
} RemoteInfoStruct;

// Callback function prototypes
typedef int (__stdcall *tProgressProc)(int PluginNr, char* SourceName, char* TargetName, int PercentDone);
typedef int (__stdcall *tProgressProcW)(int PluginNr, WCHAR* SourceName, WCHAR* TargetName, int PercentDone);
typedef void (__stdcall *tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef void (__stdcall *tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);
typedef BOOL (__stdcall *tRequestProc)(int PluginNr, int RequestType, char* CustomTitle, char* CustomText, char* ReturnedText, int maxlen);
typedef BOOL (__stdcall *tRequestProcW)(int PluginNr, int RequestType, WCHAR* CustomTitle, WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);
