#include "hfsplus_reader.h"
#include <ApfsLib/Util.h>

#include <algorithm>
#include <cstring>

namespace HfsPlus {

static const uint32_t kHFSExtentsFileID    = 3;
static const uint32_t kHFSCatalogFileID    = 4;
static const uint32_t kHFSAttributesFileID = 8;
static const uint8_t  kForkData            = 0x00;
static const uint8_t  kForkResource        = 0xFF;
// shortcut: one decompressed file is kept in memory, cap 1 GiB. Raise this if a real compressed file is larger.
static const uint64_t kMaxDecompBytes      = 1024ull * 1024ull * 1024ull;

static uint32_t LE32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::wstring Utf16BEToWide(const uint8_t *p, uint16_t length)
{
    std::wstring result;
    result.reserve(length);
    for (uint16_t i = 0; i < length; i++, p += 2) {
        wchar_t c = (wchar_t)(((uint16_t)p[0] << 8) | p[1]);
        if (c == L':') c = L'_';
        result.push_back(c);
    }
    return result;
}

static bool RecordBounds(const uint8_t *nd, uint32_t nodeSize, uint16_t index,
                         const uint8_t *&rec, uint16_t &keyLen, uint32_t &dataOff)
{
    if (nodeSize < (uint32_t)(index + 1) * 2) return false;
    uint32_t offIdx = nodeSize - (uint32_t)(index + 1) * 2;
    uint16_t recOff = BE16(nd + offIdx);
    if (recOff < 14 || (uint32_t)recOff + 2 > nodeSize) return false;
    keyLen = BE16(nd + recOff);
    if (keyLen < 6 || (uint32_t)recOff + 2u + keyLen > nodeSize) return false;
    rec = nd + recOff;
    dataOff = (uint32_t)recOff + 2u + keyLen;
    return true;
}

// Record key compared with a catalog search for (parent, empty name).
// <0 record is before the target, >0 record is after.
static int CmpCatalogParent(const uint8_t *rec, size_t avail, uint32_t parent)
{
    if (avail < 8) return 1;
    uint16_t keyLen = BE16(rec);
    if (keyLen < 6 || (size_t)2 + keyLen > avail) return 1;
    uint32_t pid = BE32(rec + 2);
    if (pid < parent) return -1;
    if (pid > parent) return 1;
    uint16_t nameLen = BE16(rec + 6);
    if ((size_t)8 + (size_t)nameLen * 2 > (size_t)2 + keyLen) return 1;
    return nameLen == 0 ? 0 : 1;
}

static int CmpExtentKey(const uint8_t *rec, size_t avail, uint8_t forkType, uint32_t fileID, uint32_t startBlock)
{
    if (avail < 12) return 1;
    uint16_t keyLen = BE16(rec);
    if (keyLen < 10) return 1;
    uint8_t ft = rec[2];
    uint32_t id = BE32(rec + 4);
    uint32_t st = BE32(rec + 8);
    if (ft < forkType) return -1;
    if (ft > forkType) return 1;
    if (id < fileID) return -1;
    if (id > fileID) return 1;
    if (st < startBlock) return -1;
    if (st > startBlock) return 1;
    return 0;
}

static int CmpAttrName(const uint8_t *rec, size_t avail, uint32_t fileID, const wchar_t *name, uint16_t nameLen)
{
    AttrKeyView key;
    if (!ParseHfsAttrKey(rec, avail, key)) return 1;
    if (key.fileID < fileID) return -1;
    if (key.fileID > fileID) return 1;
    uint16_t n = key.nameLen < nameLen ? key.nameLen : nameLen;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t c = BE16(key.name + i * 2);
        uint16_t w = (uint16_t)name[i];
        if (c < w) return -1;
        if (c > w) return 1;
    }
    if (key.nameLen < nameLen) return -1;
    if (key.nameLen > nameLen) return 1;
    return 0;
}

static int CmpAttrKey(const uint8_t *rec, size_t avail, uint32_t fileID, const wchar_t *name, uint16_t nameLen)
{
    int nameCmp = CmpAttrName(rec, avail, fileID, name, nameLen);
    if (nameCmp != 0) return nameCmp;
    AttrKeyView key;
    if (!ParseHfsAttrKey(rec, avail, key)) return 1;
    if (key.startBlock != 0) return 1;
    return 0;
}

HfsPlusVolume::HfsPlusVolume()
    : m_open(false), m_caseSensitive(false), m_hDevice(INVALID_HANDLE_VALUE),
      m_volOff(0), m_sectorSz(512), m_blockSize(0), m_nodeSizeCatalog(0),
      m_decompCnid(0), m_decompValid(false)
{
    memset(&m_hdr, 0, sizeof(m_hdr));
}

HfsPlusVolume::~HfsPlusVolume()
{
    Close();
}

void HfsPlusVolume::Close()
{
    m_open = false;
    m_hDevice = INVALID_HANDLE_VALUE;
    m_nodeCache.clear();
    m_extentCache.clear();
    m_decompBuf.clear();
    m_decompValid = false;
}

bool HfsPlusVolume::RawRead(uint64_t byteOffset, void *buf, uint32_t len)
{
    if (m_hDevice == INVALID_HANDLE_VALUE || len == 0) return false;

    uint64_t absOffset = m_volOff + byteOffset;
    uint32_t sector = m_sectorSz;

    uint64_t alignedOffset = (absOffset / sector) * sector;
    uint32_t prefix = (uint32_t)(absOffset - alignedOffset);
    uint32_t totalNeeded = prefix + len;
    uint32_t alignedLen  = ((totalNeeded + sector - 1) / sector) * sector;

    std::vector<uint8_t> aligned(alignedLen);

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)alignedOffset;
    OVERLAPPED ov = {};
    ov.Offset     = li.LowPart;
    ov.OffsetHigh = li.HighPart;

    DWORD read = 0;
    if (!ReadFile(m_hDevice, aligned.data(), alignedLen, &read, &ov))
    {
        DWORD err = GetLastError();
        if (err != ERROR_HANDLE_EOF && err != ERROR_IO_PENDING)
            return false;
        if (err == ERROR_IO_PENDING)
            GetOverlappedResult(m_hDevice, &ov, &read, TRUE);
    }

    if (read < prefix + len) return false;
    memcpy(buf, aligned.data() + prefix, len);
    return true;
}

bool HfsPlusVolume::ReadBTreeNode(const ForkData &fork, uint32_t nodeIndex, uint32_t nodeSize,
                                   std::vector<uint8_t> &out, uint32_t ownerFileID)
{
    uint64_t cacheKey = 0;
    if (!fork.extents.empty() && ownerFileID != 0) {
        cacheKey = ((uint64_t)ownerFileID << 32) | nodeIndex;
        for (const auto &entry : m_nodeCache) {
            if (entry.first == cacheKey) {
                out = entry.second;
                return true;
            }
        }
    }

    out.resize(nodeSize);
    uint64_t byteOffset = (uint64_t)nodeIndex * nodeSize;
    uint64_t br = 0;
    uint8_t forkType = 0;
    if (!ReadForkData(fork, byteOffset, out.data(), nodeSize, br, ownerFileID, forkType) || br != nodeSize)
        return false;

    if (cacheKey != 0) {
        if (m_nodeCache.size() >= 64)
            m_nodeCache.erase(m_nodeCache.begin());
        m_nodeCache.push_back({ cacheKey, out });
    }
    return true;
}

bool HfsPlusVolume::ReadForkData(const ForkData &fork, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead,
                                 uint32_t fileID, uint8_t forkType)
{
    bytesRead = 0;
    if (!buf || len == 0) return true;

    std::vector<ExtentEntry> extents;
    if (!ResolveExtents(fork, fileID, forkType, extents))
        return false;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t remaining = len;
    uint64_t pos = 0;
    for (const auto &ext : extents) {
        uint64_t extBytes = (uint64_t)ext.blockCount * m_blockSize;
        if (extBytes == 0) continue;
        if (offset >= pos + extBytes) {
            pos += extBytes;
            continue;
        }
        while (remaining > 0 && offset < pos + extBytes) {
            uint64_t local = offset - pos;
            uint64_t diskOff = (uint64_t)ext.startBlock * m_blockSize + local;
            uint64_t can = extBytes - local;
            if (can > remaining) can = remaining;
            uint32_t chunk = (uint32_t)std::min<uint64_t>(can, 1024ull * 1024ull);
            if (!RawRead(diskOff, dst, chunk)) return false;
            dst += chunk;
            offset += chunk;
            bytesRead += chunk;
            remaining -= chunk;
        }
        pos += extBytes;
        if (remaining == 0) break;
    }
    return true;
}

bool HfsPlusVolume::ResolveExtents(const ForkData &base, uint32_t fileID, uint8_t forkType, std::vector<ExtentEntry> &out)
{
    out = base.extents;
    // shortcut: the extents file itself is read from its 8 header runs only.
    if (fileID == 0 || fileID == kHFSExtentsFileID || m_extentsFork.extents.empty())
        return true;

    uint64_t cacheKey = ((uint64_t)fileID << 8) | forkType;
    for (const auto &entry : m_extentCache) {
        if (entry.first == cacheKey) {
            out = entry.second;
            return true;
        }
    }

    uint32_t covered = 0;
    for (const auto &e : out) covered += e.blockCount;
    for (int guard = 0; covered < base.totalBlocks && guard < 4096; guard++) {
        std::vector<ExtentEntry> more;
        int found = FindOverflowExtents(forkType, fileID, covered, more);
        // A short map after a read error would stick until unmount and make later copies fail.
        if (found < 0)
            return false;
        if (found == 0 || more.empty())
            break;
        bool added = false;
        for (const auto &e : more) {
            if (e.blockCount == 0) continue;
            out.push_back(e);
            covered += e.blockCount;
            added = true;
        }
        if (!added) break;
    }

    if (m_extentCache.size() >= 64)
        m_extentCache.erase(m_extentCache.begin());
    m_extentCache.push_back({ cacheKey, out });
    return true;
}

int HfsPlusVolume::FindOverflowExtents(uint8_t forkType, uint32_t fileID, uint32_t startBlock, std::vector<ExtentEntry> &out)
{
    out.clear();
    BTHeaderRec hdr;
    uint32_t nodeSize = 0;
    if (!LoadBTreeHeader(m_extentsFork, hdr, nodeSize)) return -1;
    if (hdr.rootNode == 0) return 0;

    uint32_t leaf = 0;
    auto cmp = [&](const uint8_t *rec, size_t avail) {
        return CmpExtentKey(rec, avail, forkType, fileID, startBlock);
    };
    if (!DescendToLeaf(m_extentsFork, nodeSize, hdr.rootNode, kHFSExtentsFileID, cmp, leaf))
        return -1;

    std::vector<uint8_t> nodeBuf;
    if (!ReadBTreeNode(m_extentsFork, leaf, nodeSize, nodeBuf, kHFSExtentsFileID))
        return -1;

    uint16_t numRec = BE16(nodeBuf.data() + 10);
    for (uint16_t r = 0; r < numRec; r++) {
        const uint8_t *rec = nullptr;
        uint16_t keyLen = 0;
        uint32_t dataOff = 0;
        if (!RecordBounds(nodeBuf.data(), nodeSize, r, rec, keyLen, dataOff)) continue;
        if (CmpExtentKey(rec, nodeSize - (rec - nodeBuf.data()), forkType, fileID, startBlock) != 0)
            continue;
        if (dataOff + 64 > nodeSize) return -1;
        const uint8_t *p = nodeBuf.data() + dataOff;
        for (int i = 0; i < 8; i++, p += 8) {
            ExtentEntry e;
            e.startBlock = BE32(p);
            e.blockCount = BE32(p + 4);
            if (e.blockCount == 0) break;
            out.push_back(e);
        }
        return out.empty() ? 0 : 1;
    }
    return 0;
}

bool HfsPlusVolume::LoadBTreeHeader(const ForkData &fork, BTHeaderRec &hdr, uint32_t &nodeSize)
{
    std::vector<uint8_t> node0(512);
    uint64_t br = 0;
    // Node 0 sits at the start of the fork, inside the header's own extent list.
    if (!ReadForkData(fork, 0, node0.data(), 512, br, 0, 0) || br < 64)
        return false;

    const uint8_t *p = node0.data() + 14;
    hdr.treeDepth     = BE16(p);      p += 2;
    hdr.rootNode      = BE32(p);      p += 4;
    hdr.leafRecords   = BE32(p);      p += 4;
    hdr.firstLeafNode = BE32(p);      p += 4;
    hdr.lastLeafNode  = BE32(p);      p += 4;
    hdr.nodeSize      = BE16(p);      p += 2;
    hdr.maxKeyLength  = BE16(p);      p += 2;
    hdr.totalNodes    = BE32(p);      p += 4;
    hdr.freeNodes     = BE32(p);      p += 4;
    p += 2;
    hdr.clumpSize     = BE32(p);      p += 4;
    hdr.btreeType     = *p++;
    hdr.keyCompareType= *p++;
    hdr.attributes    = BE32(p);

    nodeSize = hdr.nodeSize;
    if (nodeSize == 0 || (nodeSize & (nodeSize - 1)) != 0) return false;
    if (nodeSize < 512 || nodeSize > 32768) return false;
    return true;
}

bool HfsPlusVolume::DescendToLeaf(const ForkData &fork, uint32_t nodeSize, uint32_t rootNode, uint32_t ownerFileID,
                                  const std::function<int(const uint8_t *rec, size_t avail)> &cmpKey,
                                  uint32_t &leafNode)
{
    uint32_t node = rootNode;
    if (node == 0) return false;

    for (int depth = 0; depth < 16; depth++) {
        std::vector<uint8_t> nodeBuf;
        if (!ReadBTreeNode(fork, node, nodeSize, nodeBuf, ownerFileID))
            return false;
        const uint8_t *nd = nodeBuf.data();
        uint8_t kind = nd[8];
        uint16_t numRec = BE16(nd + 10);
        if (numRec == 0) return false;

        if (kind == kBTLeafNode) {
            leafNode = node;
            return true;
        }
        if (kind != kBTIndexNode) return false;

        int chosen = -1;
        uint32_t child = 0;
        for (uint16_t r = 0; r < numRec; r++) {
            const uint8_t *rec = nullptr;
            uint16_t keyLen = 0;
            uint32_t dataOff = 0;
            if (!RecordBounds(nd, nodeSize, r, rec, keyLen, dataOff)) break;
            int cmp = cmpKey(rec, nodeSize - (size_t)(rec - nd));
            if (cmp > 0) break;
            if (dataOff + 4 > nodeSize) break;
            chosen = (int)r;
            child = BE32(nd + dataOff);
        }
        if (chosen < 0) {
            const uint8_t *rec = nullptr;
            uint16_t keyLen = 0;
            uint32_t dataOff = 0;
            if (!RecordBounds(nd, nodeSize, 0, rec, keyLen, dataOff) || dataOff + 4 > nodeSize)
                return false;
            child = BE32(nd + dataOff);
        }
        if (child == 0) return false;
        node = child;
    }
    return false;
}

ForkData HfsPlusVolume::ParseFork(const HFSPlusForkData &src)
{
    ForkData fd;
    ParseHfsFork(reinterpret_cast<const uint8_t *>(&src), sizeof(src), fd);
    return fd;
}

std::wstring HfsPlusVolume::ParseHFSPlusString(const uint8_t *p, uint16_t length)
{
    return Utf16BEToWide(p, length);
}

bool HfsPlusVolume::Open(HANDLE hDevice, uint64_t volumeOffset, uint64_t volumeSize, uint32_t deviceSectorSize)
{
    (void)volumeSize;
    m_hDevice  = hDevice;
    m_volOff   = volumeOffset;
    m_sectorSz = deviceSectorSize ? deviceSectorSize : 512;
    m_caseSensitive = false;
    m_volumeName = L"HFS+ Volume";

    uint8_t hdrBuf[512] = {};
    if (!RawRead(1024, hdrBuf, 512)) return false;

    uint16_t sig = BE16(hdrBuf);

    if (sig == kHFSOldSigWord) {
        uint16_t embedSig = BE16(hdrBuf + 0x7C);
        if (embedSig != kHFSPlusSigWord && embedSig != kHFSXSigWord)
            return false;
        uint32_t alBlkSiz   = BE32(hdrBuf + 0x14);
        uint16_t alBlSt     = BE16(hdrBuf + 0x1C);
        uint16_t embedStart = BE16(hdrBuf + 0x7E);
        uint64_t embOff = (uint64_t)alBlSt * 512 + (uint64_t)embedStart * alBlkSiz;
        m_volOff += embOff;
        if (!RawRead(1024, hdrBuf, 512)) return false;
        sig = BE16(hdrBuf);
    }

    if (sig != kHFSPlusSigWord && sig != kHFSXSigWord) return false;

    uint32_t totalBlocks = 0;
    if (!ParseHfsVolumeForks(hdrBuf, sizeof(hdrBuf), m_blockSize, totalBlocks, m_extentsFork, m_catalogFork, m_attrFork))
        return false;
    m_hdr.blockSize = m_blockSize;
    m_hdr.totalBlocks = totalBlocks;
    m_hdr.signature = sig;

    if (m_blockSize == 0 || (m_blockSize & (m_blockSize - 1)) != 0) return false;
    if (m_catalogFork.totalBlocks == 0) return false;

    BTHeaderRec btHdr;
    uint32_t nodeSize = 0;
    if (!LoadBTreeHeader(m_catalogFork, btHdr, nodeSize)) return false;
    m_nodeSizeCatalog = nodeSize;
    m_caseSensitive = (sig == kHFSXSigWord && btHdr.keyCompareType == 0xBC);

    if (btHdr.rootNode != 0) {
        uint32_t leaf = 0;
        auto cmp = [](const uint8_t *rec, size_t avail) {
            return CmpCatalogParent(rec, avail, kHFSPlusRootFolderID);
        };
        if (DescendToLeaf(m_catalogFork, nodeSize, btHdr.rootNode, kHFSCatalogFileID, cmp, leaf)) {
            std::vector<uint8_t> nodeBuf;
            if (ReadBTreeNode(m_catalogFork, leaf, nodeSize, nodeBuf, kHFSCatalogFileID)) {
                uint16_t numRec = BE16(nodeBuf.data() + 10);
                for (uint16_t r = 0; r < numRec; r++) {
                    const uint8_t *rec = nullptr;
                    uint16_t keyLen = 0;
                    uint32_t dataOff = 0;
                    if (!RecordBounds(nodeBuf.data(), nodeSize, r, rec, keyLen, dataOff)) continue;
                    if (CmpCatalogParent(rec, nodeSize - (size_t)(rec - nodeBuf.data()), kHFSPlusRootFolderID) != 0)
                        continue;
                    if (dataOff + 10 > nodeSize) continue;
                    const uint8_t *d = nodeBuf.data() + dataOff;
                    int16_t recType = (int16_t)BE16(d);
                    if (recType != kHFSPlusFolderThreadRecord && recType != kHFSPlusFileThreadRecord)
                        continue;
                    uint16_t vNameLen = BE16(d + 8);
                    if (vNameLen > 0 && dataOff + 10u + (uint32_t)vNameLen * 2 <= nodeSize)
                        m_volumeName = Utf16BEToWide(d + 10, vNameLen);
                    break;
                }
            }
        }
    }

    m_open = true;
    return true;
}

bool HfsPlusVolume::ListDirectory(uint32_t parentCNID, std::vector<CatalogItem> &out)
{
    out.clear();
    if (!m_open) return false;

    BTHeaderRec btHdr;
    uint32_t nodeSize = 0;
    if (!LoadBTreeHeader(m_catalogFork, btHdr, nodeSize)) return false;
    m_nodeSizeCatalog = nodeSize;
    if (btHdr.rootNode == 0) return true;

    uint32_t leaf = 0;
    auto cmp = [&](const uint8_t *rec, size_t avail) {
        return CmpCatalogParent(rec, avail, parentCNID);
    };
    if (!DescendToLeaf(m_catalogFork, nodeSize, btHdr.rootNode, kHFSCatalogFileID, cmp, leaf))
        return false;

    uint32_t curNode = leaf;
    int guard = (int)btHdr.totalNodes + 2;
    while (curNode != 0 && guard-- > 0) {
        std::vector<uint8_t> nodeBuf;
        if (!ReadBTreeNode(m_catalogFork, curNode, nodeSize, nodeBuf, kHFSCatalogFileID))
            return false;

        const uint8_t *nd = nodeBuf.data();
        uint32_t fLink = BE32(nd);
        uint8_t kind = nd[8];
        uint16_t numRec = BE16(nd + 10);
        if (kind != kBTLeafNode) return false;

        for (uint16_t r = 0; r < numRec; r++) {
            const uint8_t *rec = nullptr;
            uint16_t keyLen = 0;
            uint32_t dataOff = 0;
            if (!RecordBounds(nd, nodeSize, r, rec, keyLen, dataOff)) continue;

            uint32_t pid = BE32(rec + 2);
            uint16_t nameLen = BE16(rec + 6);
            if (pid < parentCNID) continue;
            if (pid > parentCNID) return true;

            if (dataOff + 2 > nodeSize) continue;
            int16_t recType = (int16_t)BE16(nd + dataOff);
            if (recType != kHFSPlusFolderRecord && recType != kHFSPlusFileRecord) continue;

            CatalogItem item = {};
            item.parentCNID = pid;
            if (nameLen > 0 && (uint32_t)8 + (uint32_t)nameLen * 2 <= (uint32_t)2 + keyLen)
                item.name = ParseHFSPlusString(rec + 8, nameLen);

            if (recType == kHFSPlusFolderRecord) {
                if (dataOff + 20 > nodeSize) continue;
                const uint8_t *d = nd + dataOff;
                item.isDir = true;
                item.cnid       = BE32(d + 8);
                item.createTime = BE32(d + 12);
                item.modifyTime = BE32(d + 16);
            } else {
                size_t recLen = nodeSize - dataOff;
                if (!ParseHfsFileRecord(nd + dataOff, recLen, item)) continue;
                if (!LoadDecmpfsAttr(item.cnid, item))
                    return false;
                if (item.hasDecmpfs && item.decmpfsMethod != 0)
                    item.dataSize = item.decmpfsUnpackedSize;
            }
            out.push_back(std::move(item));
        }
        curNode = fLink;
    }
    return true;
}

static void MarkDecmpfsBroken(CatalogItem &item)
{
    item.hasDecmpfs = true;
    item.decmpfsMethod = 0;
    item.decmpfsUnpackedSize = 0;
    item.decmpfsAttrData.clear();
}

static bool ParseDecmpfsBlob(const uint8_t *ad, uint32_t size, CatalogItem &item)
{
    if (!ad || size < 16 || LE32(ad) != 0x636D7066) return false;
    item.decmpfsMethod = LE32(ad + 4);
    item.decmpfsUnpackedSize = (uint64_t)LE32(ad + 8) | ((uint64_t)LE32(ad + 12) << 32);
    item.hasDecmpfs = true;
    item.decmpfsAttrData.clear();
    if (size > 16)
        item.decmpfsAttrData.assign(ad + 16, ad + size);
    return true;
}

bool HfsPlusVolume::LoadDecmpfsAttr(uint32_t cnid, CatalogItem &item)
{
    item.hasDecmpfs = false;
    item.decmpfsMethod = 0;
    item.decmpfsUnpackedSize = 0;
    item.decmpfsAttrData.clear();
    // No attributes file means nothing is compressed. A file that claims blocks but has
    // no extents cannot be read; failing the listing beats copying compressed files as empty.
    if (m_attrFork.totalBlocks == 0)
        return true;
    if (m_attrFork.extents.empty())
        return false;

    BTHeaderRec btHdr;
    uint32_t nodeSize = 0;
    if (!LoadBTreeHeader(m_attrFork, btHdr, nodeSize)) return false;
    if (btHdr.rootNode == 0) return true;

    static const wchar_t kDecmpfs[] = L"com.apple.decmpfs";
    static const uint16_t kDecmpfsLen = 17;

    uint32_t leaf = 0;
    auto cmp = [&](const uint8_t *rec, size_t avail) {
        return CmpAttrKey(rec, avail, cnid, kDecmpfs, kDecmpfsLen);
    };
    if (!DescendToLeaf(m_attrFork, nodeSize, btHdr.rootNode, kHFSAttributesFileID, cmp, leaf))
        return false;

    bool haveFork = false;
    ForkData fork;

    uint32_t curNode = leaf;
    int guard = 4;
    while (curNode != 0 && guard-- > 0) {
        std::vector<uint8_t> nodeBuf;
        if (!ReadBTreeNode(m_attrFork, curNode, nodeSize, nodeBuf, kHFSAttributesFileID))
            return false;
        const uint8_t *nd = nodeBuf.data();
        uint32_t fLink = BE32(nd);
        uint8_t kind = nd[8];
        uint16_t numRec = BE16(nd + 10);
        if (kind != kBTLeafNode) return false;

        for (uint16_t r = 0; r < numRec; r++) {
            const uint8_t *rec = nullptr;
            uint16_t keyLen = 0;
            uint32_t dataOff = 0;
            if (!RecordBounds(nd, nodeSize, r, rec, keyLen, dataOff)) continue;
            int nameCmp = CmpAttrName(rec, nodeSize - (size_t)(rec - nd), cnid, kDecmpfs, kDecmpfsLen);
            if (nameCmp < 0) continue;
            if (nameCmp > 0) {
                curNode = 0;
                break;
            }

            AttrKeyView key;
            if (!ParseHfsAttrKey(rec, nodeSize - (size_t)(rec - nd), key)) {
                MarkDecmpfsBroken(item);
                return true;
            }
            uint16_t nextOff = (r + 1 < numRec)
                ? BE16(nd + nodeSize - (uint32_t)(r + 2) * 2)
                : (uint16_t)(nodeSize - 2u * (numRec + 1));
            if (nextOff > nodeSize || dataOff + 4 > nextOff) {
                MarkDecmpfsBroken(item);
                return true;
            }
            uint32_t body = nextOff - dataOff;
            uint32_t attrRecType = BE32(nd + dataOff);

            if (attrRecType == 0x10 && key.startBlock == 0) {
                if (body < 16) { MarkDecmpfsBroken(item); return true; }
                uint32_t attrSize = BE32(nd + dataOff + 12);
                if (attrSize < 16 || attrSize > body - 16) { MarkDecmpfsBroken(item); return true; }
                if (!ParseDecmpfsBlob(nd + dataOff + 16, attrSize, item))
                    MarkDecmpfsBroken(item);
                return true;
            }

            // shortcut: type 0x30 attribute extent records are not followed. A fork that needs them fails the copy.
            if (attrRecType == 0x20 && key.startBlock == 0 && body >= 88 &&
                ParseHfsFork(nd + dataOff + 8, body - 8, fork)) {
                haveFork = true;
                curNode = 0;
                break;
            }

            MarkDecmpfsBroken(item);
            return true;
        }
        if (curNode != 0)
            curNode = fLink;
    }

    if (!haveFork)
        return true;

    if (fork.logicalSize == 0 || fork.logicalSize > kMaxDecompBytes) {
        MarkDecmpfsBroken(item);
        return true;
    }
    try {
        std::vector<uint8_t> bytes((size_t)fork.logicalSize);
        uint64_t br = 0;
        if (!ReadForkData(fork, 0, bytes.data(), bytes.size(), br, 0, 0))
            return false;
        if (br != bytes.size() || !ParseDecmpfsBlob(bytes.data(), (uint32_t)bytes.size(), item))
            MarkDecmpfsBroken(item);
    } catch (const std::bad_alloc &) {
        MarkDecmpfsBroken(item);
    }
    return true;
}

static bool FillStored(std::vector<uint8_t> &out, uint64_t usize, const uint8_t *src, size_t srcLen)
{
    if (srcLen != usize + 1) return false;
    out.assign(src + 1, src + srcLen);
    return true;
}

static bool DecompressAttrBytes(uint32_t algo, uint64_t usize, const std::vector<uint8_t> &srcIn, std::vector<uint8_t> &out)
{
    if (srcIn.empty() || usize > kMaxDecompBytes) return false;
    const uint8_t *src = srcIn.data();
    size_t srcLen = srcIn.size();
    out.clear();

    if (algo == 3) {
        if (src[0] == 0xFF) return FillStored(out, usize, src, srcLen);
        if (src[0] != 0x78) return false;
        out.resize((size_t)usize);
        return DecompressZLib(out.data(), out.size(), src, srcLen) == usize;
    }
    if (algo == 7) {
        if (src[0] == 0x06) return FillStored(out, usize, src, srcLen);
        out.resize((size_t)usize);
        return DecompressLZVN(out.data(), out.size(), src, srcLen) == usize;
    }
    if (algo == 9)
        return FillStored(out, usize, src, srcLen);
    if (algo == 11) {
        out.resize((size_t)usize);
        return DecompressLZFSE(out.data(), out.size(), src, srcLen) == usize;
    }
    if (algo == 13) {
        if (src[0] == 0xFF) return FillStored(out, usize, src, srcLen);
        out.resize((size_t)usize);
        return DecompressLZBITMAP(out.data(), out.size(), src, srcLen) == usize;
    }
    return false;
}

static bool DecompressRsrcBytes(uint32_t algo, uint64_t usize, const std::vector<uint8_t> &rsrc, std::vector<uint8_t> &out)
{
    if (usize == 0 || usize > kMaxDecompBytes || rsrc.empty()) return false;
    uint32_t blocks = (uint32_t)((usize + 0xFFFF) >> 16);
    out.assign((size_t)usize, 0);

    if (algo == 4) {
        if (rsrc.size() < 16) return false;
        uint32_t dataOff = BE32(rsrc.data());
        if ((uint64_t)dataOff + 4 > rsrc.size()) return false;
        const uint8_t *base = rsrc.data() + dataOff + 4;
        size_t baseLen = rsrc.size() - (dataOff + 4);
        if (baseLen < 4) return false;
        uint32_t entries = LE32(base);
        if (entries != blocks) return false;
        if (baseLen < 4ull + (uint64_t)entries * 8) return false;
        for (uint32_t k = 0; k < entries; k++) {
            uint32_t off = LE32(base + 4 + k * 8);
            uint32_t sz = LE32(base + 8 + k * 8);
            if (sz == 0 || (uint64_t)off + sz > baseLen) return false;
            const uint8_t *src = base + off;
            uint64_t expect64 = usize - (uint64_t)k * 0x10000;
            if (expect64 > 0x10000) expect64 = 0x10000;
            size_t expect = (size_t)expect64;
            uint8_t *dst = out.data() + (size_t)k * 0x10000;
            size_t n = 0;
            if (src[0] == 0x78)
                n = DecompressZLib(dst, 0x10000, src, sz);
            else if ((src[0] & 0x0F) == 0x0F && sz > 1 && sz - 1 == expect) {
                memcpy(dst, src + 1, expect);
                n = expect;
            } else
                return false;
            if (n != expect) return false;
        }
        return true;
    }

    if (rsrc.size() < (uint64_t)(blocks + 1) * 4) return false;
    for (uint32_t k = 0; k < blocks; k++) {
        uint32_t a = LE32(rsrc.data() + k * 4);
        uint32_t b = LE32(rsrc.data() + (k + 1) * 4);
        if (b < a || b > rsrc.size() || a == b) return false;
        const uint8_t *src = rsrc.data() + a;
        size_t sz = (size_t)(b - a);
        uint64_t expect64 = usize - (uint64_t)k * 0x10000;
        if (expect64 > 0x10000) expect64 = 0x10000;
        size_t expect = (size_t)expect64;
        uint8_t *dst = out.data() + (size_t)k * 0x10000;
        size_t n = 0;
        if (algo == 8) {
            if (src[0] == 0x06 && sz > 1) {
                if (sz - 1 != expect) return false;
                memcpy(dst, src + 1, expect);
                n = expect;
            } else
                n = DecompressLZVN(dst, expect, src, sz);
        } else if (algo == 10) {
            if (sz < 2 || sz - 1 != expect) return false;
            memcpy(dst, src + 1, expect);
            n = expect;
        } else if (algo == 12) {
            n = DecompressLZFSE(dst, expect, src, sz);
        } else if (algo == 14) {
            if (src[0] == 0xFF && sz > 1) {
                if (sz - 1 != expect) return false;
                memcpy(dst, src + 1, expect);
                n = expect;
            } else
                n = DecompressLZBITMAP(dst, expect, src, sz);
        } else
            return false;
        if (n != expect) return false;
    }
    return true;
}

bool HfsPlusVolume::DecompressItem(const CatalogItem &item, std::vector<uint8_t> &out)
{
    uint32_t algo = item.decmpfsMethod;
    uint64_t usize = item.decmpfsUnpackedSize;
    if (usize > kMaxDecompBytes) return false;

    try {
        if (algo == 4 || algo == 8 || algo == 10 || algo == 12 || algo == 14) {
            if (item.resourceFork.logicalSize == 0 || item.resourceFork.logicalSize > kMaxDecompBytes)
                return false;
            std::vector<uint8_t> rsrc((size_t)item.resourceFork.logicalSize);
            uint64_t br = 0;
            if (!ReadForkData(item.resourceFork, 0, rsrc.data(), rsrc.size(), br, item.cnid, kForkResource) || br != rsrc.size())
                return false;
            return DecompressRsrcBytes(algo, usize, rsrc, out);
        }
        return DecompressAttrBytes(algo, usize, item.decmpfsAttrData, out);
    } catch (const std::bad_alloc &) {
        return false;
    }
}

bool HfsPlusVolume::ReadFileData(const CatalogItem &item, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead)
{
    bytesRead = 0;
    if (!m_open || !buf || len == 0) return false;
    if (item.isDir) return false;

    if (item.hasDecmpfs) {
        if (!m_decompValid || m_decompCnid != item.cnid) {
            std::vector<uint8_t> decoded;
            if (!DecompressItem(item, decoded)) return false;
            m_decompBuf.swap(decoded);
            m_decompCnid = item.cnid;
            m_decompValid = true;
        }
        if (offset >= m_decompBuf.size()) return true;
        uint64_t can = m_decompBuf.size() - offset;
        if (can > len) can = len;
        memcpy(buf, m_decompBuf.data() + offset, (size_t)can);
        bytesRead = can;
        return true;
    }

    if (item.dataFork.totalBlocks == 0 && item.dataFork.logicalSize == 0) return true;
    return ReadForkData(item.dataFork, offset, buf, len, bytesRead, item.cnid, kForkData);
}

} // namespace HfsPlus
