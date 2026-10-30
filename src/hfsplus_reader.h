#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// ============================================================
// HFS+ (Mac OS Extended) native reader
// Based on Apple TN1150 "HFS Plus Volume Format"
// Supports: HFS+ (H+, version 4) and HFSX (HX, version 5)
// Embedded HFS+ inside old HFS wrapper volumes
// Decmpfs transparent decompression: ZLIB-attr, LZVN-attr, COPY-attr
// ============================================================

namespace HfsPlus {

// --- Big-endian helpers ---
inline uint16_t BE16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t BE32(const uint8_t *p) { return (uint32_t)(p[0]<<24|p[1]<<16|p[2]<<8|p[3]); }
inline uint64_t BE64(const uint8_t *p) {
    return ((uint64_t)BE32(p) << 32) | BE32(p+4);
}

// --- HFS+ Constants ---
static const uint16_t kHFSPlusSigWord  = 0x482B; // H+
static const uint16_t kHFSXSigWord     = 0x4858; // HX
static const uint16_t kHFSOldSigWord   = 0x4244; // BD  (old HFS)
static const uint32_t kHFSPlusRootFolderID = 2;

// --- Catalog record types ---
static const int16_t kHFSPlusFolderRecord     = 1;
static const int16_t kHFSPlusFileRecord       = 2;
static const int16_t kHFSPlusFolderThreadRecord = 3;
static const int16_t kHFSPlusFileThreadRecord = 4;

// --- BTree node kinds ---
static const uint8_t kBTLeafNode   = 0xFF;
static const uint8_t kBTIndexNode  = 0x00;
static const uint8_t kBTHeaderNode = 0x01;
static const uint8_t kBTMapNode    = 0x02;

// --- Volume flags ---
static const uint32_t kHFSVolumeJournaledBit = 13;

// --- HFS+ fork data (stored big-endian) ---
#pragma pack(push,1)
struct HFSPlusExtentDescriptor {
    uint32_t startBlock;
    uint32_t blockCount;
};

struct HFSPlusForkData {
    uint64_t logicalSize;
    uint32_t clumpSize;
    uint32_t totalBlocks;
    HFSPlusExtentDescriptor extents[8];
};

struct HFSPlusVolumeHeader {
    uint16_t signature;       // 0x482B (H+) or 0x4858 (HX)
    uint16_t version;         // 4 or 5
    uint32_t attributes;
    uint32_t lastMountedVersion;
    uint32_t journalInfoBlock;
    uint32_t createDate;
    uint32_t modifyDate;
    uint32_t backupDate;
    uint32_t checkedDate;
    uint32_t fileCount;
    uint32_t folderCount;
    uint32_t blockSize;
    uint32_t totalBlocks;
    uint32_t freeBlocks;
    uint32_t nextAllocation;
    uint32_t rsrcClumpSize;
    uint32_t dataClumpSize;
    uint32_t nextCatalogID;  // HFSCatalogNodeID
    uint32_t writeCount;
    uint64_t encodingsBitmap;
    uint32_t finderInfo[8];
    HFSPlusForkData allocationFile;
    HFSPlusForkData extentsFile;
    HFSPlusForkData catalogFile;
    HFSPlusForkData attributesFile;
    HFSPlusForkData startupFile;
};

struct BTNodeDescriptor {
    uint32_t fLink;
    uint32_t bLink;
    int8_t   kind;
    uint8_t  height;
    uint16_t numRecords;
    uint16_t reserved;
};

struct BTHeaderRec {
    uint16_t treeDepth;
    uint32_t rootNode;
    uint32_t leafRecords;
    uint32_t firstLeafNode;
    uint32_t lastLeafNode;
    uint16_t nodeSize;
    uint16_t maxKeyLength;
    uint32_t totalNodes;
    uint32_t freeNodes;
    uint16_t reserved1;
    uint32_t clumpSize;
    uint8_t  btreeType;
    uint8_t  keyCompareType;
    uint32_t attributes;
    uint32_t reserved3[16];
};
#pragma pack(pop)

// --- Parsed structures (host byte order) ---

struct ExtentEntry {
    uint32_t startBlock;
    uint32_t blockCount;
};

struct ForkData {
    uint64_t logicalSize;
    uint32_t totalBlocks;
    std::vector<ExtentEntry> extents;

    bool isEmpty() const { return logicalSize == 0 && totalBlocks == 0; }
};

// HFS+ time: seconds since 1904-01-01 00:00:00
inline FILETIME HfsPlusTimeToFileTime(uint32_t hfsTime) {
    // HFS+ epoch: Jan 1, 1904 = Unix -2082844800 seconds
    // FILETIME epoch: Jan 1, 1601 = Unix -11644473600 seconds
    // Offset from FILETIME to HFS+: 9561628800 seconds
    uint64_t secs = (uint64_t)hfsTime + 9561628800ULL;
    uint64_t ft = secs * 10000000ULL;
    FILETIME result;
    result.dwLowDateTime  = (DWORD)(ft & 0xFFFFFFFF);
    result.dwHighDateTime = (DWORD)(ft >> 32);
    return result;
}

// Parsed catalog item
struct CatalogItem {
    uint32_t   cnid;         // catalog node ID
    uint32_t   parentCNID;
    std::wstring name;       // Unicode filename
    bool       isDir;
    uint64_t   dataSize;     // logical size of data fork
    uint32_t   createTime;
    uint32_t   modifyTime;
    ForkData   dataFork;
    ForkData   resourceFork;
    // Decmpfs compressed data (attribute-inline)
    bool       hasDecmpfs;
    uint32_t   decmpfsMethod;
    uint64_t   decmpfsUnpackedSize;
    std::vector<uint8_t> decmpfsAttrData; // raw attribute bytes after header
};

// ============================================================
// HfsPlusVolume — main reader class
// ============================================================
class HfsPlusVolume {
public:
    HfsPlusVolume();
    ~HfsPlusVolume();

    // Open from a physical device handle + byte offset of the HFS+ volume start
    bool Open(HANDLE hDevice, uint64_t volumeOffset, uint64_t volumeSize, uint32_t deviceSectorSize = 512);
    void Close();

    bool IsOpen() const { return m_open; }
    std::wstring GetVolumeName() const { return m_volumeName; }
    uint64_t     GetVolumeSize() const { return (uint64_t)m_hdr.totalBlocks * m_hdr.blockSize; }

    // List directory contents (parentCNID = 2 for root)
    bool ListDirectory(uint32_t parentCNID, std::vector<CatalogItem> &out);

    // Read file data into caller-supplied buffer
    bool ReadFileData(const CatalogItem &item, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead);

    // Read raw extents data (helper for partial reads)
    bool ReadForkData(const ForkData &fork, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead);

private:
    bool     m_open;
    HANDLE   m_hDevice;
    uint64_t m_volOff;   // byte offset of volume start on device
    uint32_t m_sectorSz;
    HFSPlusVolumeHeader m_hdr;
    std::wstring m_volumeName;
    ForkData m_catalogFork;
    ForkData m_attrFork;
    ForkData m_extentsFork;
    uint32_t m_blockSize;
    uint32_t m_nodeSizeCatalog;

    // Read raw bytes from device (sector-aligned internally)
    bool RawRead(uint64_t byteOffset, void *buf, uint32_t len);

    // Parse ForkData from big-endian HFSPlusForkData
    ForkData ParseFork(const HFSPlusForkData &src);

    // BTree helpers
    bool LoadBTreeHeader(const ForkData &fork, BTHeaderRec &hdr, uint32_t &nodeSize);
    bool ReadBTreeNode(const ForkData &fork, uint32_t nodeIndex, uint32_t nodeSize, std::vector<uint8_t> &out);

    std::wstring ParseHFSPlusString(const uint8_t *p, uint16_t length);

    // Attributes B-Tree lookup for decmpfs
    bool LoadDecmpfsAttr(uint32_t cnid, CatalogItem &item);
};

} // namespace HfsPlus
