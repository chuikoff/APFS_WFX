#include "hfsplus_reader.h"
#include <algorithm>
#include <cassert>
#include <cstring>

// --- miniz for ZLIB decompression ---
#include "miniz.h"
// --- lzvn for LZVN decompression (from apfs-fuse-src) ---
extern "C" {
#include "apfs-fuse-src/3rdparty/lzfse/src/lzvn_decode_base.h"
}

namespace HfsPlus {

// ============================================================
// Helpers
// ============================================================

static std::wstring Utf16BEToWide(const uint8_t *p, uint16_t length)
{
    std::wstring result;
    result.reserve(length);
    for (uint16_t i = 0; i < length; i++, p += 2) {
        wchar_t c = (wchar_t)(((uint16_t)p[0] << 8) | p[1]);
        // HFS+ uses ':' as path separator; we map to '_' to avoid TC confusion
        if (c == L':') c = L'_';
        result.push_back(c);
    }
    return result;
}

static ForkData ParseForkStatic(const uint8_t *p)
{
    ForkData fd;
    fd.logicalSize  = BE64(p);
    // clumpSize = BE32(p+8)
    fd.totalBlocks  = BE32(p + 12);
    p += 16;
    for (int i = 0; i < 8; i++, p += 8) {
        ExtentEntry e;
        e.startBlock = BE32(p);
        e.blockCount = BE32(p + 4);
        if (e.blockCount == 0) break;
        fd.extents.push_back(e);
    }
    return fd;
}

// ============================================================
// HfsPlusVolume
// ============================================================

HfsPlusVolume::HfsPlusVolume()
    : m_open(false), m_hDevice(INVALID_HANDLE_VALUE),
      m_volOff(0), m_sectorSz(512), m_blockSize(0), m_nodeSizeCatalog(0)
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
}

bool HfsPlusVolume::RawRead(uint64_t byteOffset, void *buf, uint32_t len)
{
    if (m_hDevice == INVALID_HANDLE_VALUE || len == 0) return false;

    uint64_t absOffset = m_volOff + byteOffset;
    uint32_t sector = m_sectorSz;

    // Align down to sector boundary
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

bool HfsPlusVolume::Open(HANDLE hDevice, uint64_t volumeOffset, uint64_t volumeSize, uint32_t deviceSectorSize)
{
    m_hDevice  = hDevice;
    m_volOff   = volumeOffset;
    m_sectorSz = deviceSectorSize ? deviceSectorSize : 512;

    // HFS+ volume header is at byte offset 1024 from volume start
    uint8_t hdrBuf[512] = {};
    if (!RawRead(1024, hdrBuf, 512)) return false;

    uint16_t sig = BE16(hdrBuf);

    // Old HFS wrapper: check for embedded HFS+ at drEmbedExtent
    if (sig == kHFSOldSigWord) {
        // drEmbedSigWord at offset 0x7C, drEmbedExtent at 0x7E
        uint16_t embedSig = BE16(hdrBuf + 0x7C);
        if (embedSig != kHFSPlusSigWord && embedSig != kHFSXSigWord)
            return false;
        // drAlBlkSiz at 0x14, drAlBlSt at 0x1C
        uint32_t alBlkSiz  = BE32(hdrBuf + 0x14);
        uint16_t alBlSt    = BE16(hdrBuf + 0x1C);
        uint16_t embedStart= BE16(hdrBuf + 0x7E);
        // Compute real HFS+ offset
        uint64_t embOff = (uint64_t)alBlSt * 512 + (uint64_t)embedStart * alBlkSiz;
        m_volOff += embOff;
        // Re-read volume header
        if (!RawRead(1024, hdrBuf, 512)) return false;
        sig = BE16(hdrBuf);
    }

    if (sig != kHFSPlusSigWord && sig != kHFSXSigWord) return false;

    // Verify first 1024 bytes are zero (boot blocks reserved area)
    // (we skip this check to be lenient with real disks)

    // Parse volume header (big-endian layout matches struct if we read manually)
    const uint8_t *p = hdrBuf;
    m_hdr.signature            = BE16(p);      p += 2;
    m_hdr.version              = BE16(p);      p += 2;
    m_hdr.attributes           = BE32(p);      p += 4;
    m_hdr.lastMountedVersion   = BE32(p);      p += 4;
    m_hdr.journalInfoBlock     = BE32(p);      p += 4;
    m_hdr.createDate           = BE32(p);      p += 4;
    m_hdr.modifyDate           = BE32(p);      p += 4;
    m_hdr.backupDate           = BE32(p);      p += 4;
    m_hdr.checkedDate          = BE32(p);      p += 4;
    m_hdr.fileCount            = BE32(p);      p += 4;
    m_hdr.folderCount          = BE32(p);      p += 4;
    m_hdr.blockSize            = BE32(p);      p += 4;
    m_hdr.totalBlocks          = BE32(p);      p += 4;
    m_hdr.freeBlocks           = BE32(p);      p += 4;
    // Skip nextAllocation .. finderInfo (5*4 + 8 + 8*4 = 72 bytes)
    p += 72;
    // allocationFile fork: 80 bytes
    p += 80;
    // extentsFile fork
    m_extentsFork = ParseForkStatic(p); p += 80;
    // catalogFile fork
    m_catalogFork = ParseForkStatic(p); p += 80;
    // attributesFile fork
    m_attrFork    = ParseForkStatic(p); p += 80;

    m_blockSize = m_hdr.blockSize;
    if (m_blockSize == 0 || (m_blockSize & (m_blockSize-1)) != 0) return false;
    if (m_catalogFork.totalBlocks == 0) return false;

    // Load BTree header to get node size
    BTHeaderRec btHdr;
    uint32_t nodeSize;
    if (!LoadBTreeHeader(m_catalogFork, btHdr, nodeSize)) return false;
    m_nodeSizeCatalog = nodeSize;

    // Read volume name from catalog root folder thread record
    // CNID 1 = special root parent; thread record for CNID 2 has the name
    m_volumeName = L"HFS+ Volume";
    {
        std::vector<CatalogItem> rootItems;
        // List items with parentCNID = 1 (root parent) to get root folder thread
        // Actually: the root folder's thread record is keyed by (1, "")
        // For simplicity we'll do a full scan of leaf nodes looking for CNID=1 thread
        // This is the approach used for correctness on all sizes
        uint32_t firstLeaf = btHdr.firstLeafNode;
        uint32_t curNode = firstLeaf;
        bool nameFound = false;
        int maxNodes = 5; // only scan first few nodes for the name
        while (curNode != 0 && maxNodes-- > 0 && !nameFound) {
            std::vector<uint8_t> nodeBuf;
            if (!ReadBTreeNode(m_catalogFork, curNode, nodeSize, nodeBuf)) break;
            const uint8_t *nd = nodeBuf.data();
            uint32_t fLink = BE32(nd);
            uint8_t  kind  = nd[8];
            uint16_t numRec= BE16(nd + 10);
            if (kind != (uint8_t)kBTLeafNode) { curNode = fLink; continue; }

            // Offsets stored at end of node (big-endian uint16)
            for (uint16_t r = 0; r < numRec && !nameFound; r++) {
                uint32_t offIdx = nodeSize - (r+1)*2;
                if (offIdx + 2 > nodeSize) break;
                uint16_t recOff = BE16(nd + offIdx);
                if (recOff < 14 || recOff >= nodeSize) continue;
                const uint8_t *rec = nd + recOff;
                // Catalog key: keyLength(2) + parentID(4) + nodeName(2+2*len)
                uint16_t keyLen = BE16(rec);
                if (keyLen < 6) continue;
                uint32_t parentID = BE32(rec + 2);
                uint16_t nameLen  = BE16(rec + 6);
                uint32_t recDataOff = recOff + 2 + keyLen;
                if (recDataOff + 2 > nodeSize) continue;
                int16_t recType = (int16_t)BE16(nd + recDataOff);
                if (parentID == 1 && nameLen == 0 && recType == kHFSPlusFolderThreadRecord) {
                    // Thread record: data = type(2) + reserved(6) + parentID(4) + name(2+2*n)
                    if (recDataOff + 14 > nodeSize) continue;
                    uint16_t vNameLen = BE16(nd + recDataOff + 12);
                    if (vNameLen > 0 && recDataOff + 14 + vNameLen*2 <= nodeSize) {
                        m_volumeName = Utf16BEToWide(nd + recDataOff + 14, vNameLen);
                        nameFound = true;
                    }
                }
            }
            curNode = fLink;
        }
    }

    m_open = true;
    return true;
}

ForkData HfsPlusVolume::ParseFork(const HFSPlusForkData &src)
{
    return ParseForkStatic(reinterpret_cast<const uint8_t*>(&src));
}

bool HfsPlusVolume::ReadBTreeNode(const ForkData &fork, uint32_t nodeIndex, uint32_t nodeSize,
                                   std::vector<uint8_t> &out)
{
    out.resize(nodeSize);
    // nodeIndex * nodeSize = byte offset within fork
    uint64_t byteOffset = (uint64_t)nodeIndex * nodeSize;
    uint64_t br = 0;
    return ReadForkData(fork, byteOffset, out.data(), nodeSize, br);
}

// Overload for ReadBTreeNode that doesn't use the last-bytes-read trick:
static bool ReadBTreeNodeStatic(HfsPlusVolume *vol, const ForkData &fork,
                                 uint32_t nodeIndex, uint32_t nodeSize,
                                 std::vector<uint8_t> &out)
{
    out.resize(nodeSize);
    uint64_t byteOffset = (uint64_t)nodeIndex * nodeSize;
    uint64_t br = 0;
    return vol->ReadForkData(fork, byteOffset, out.data(), nodeSize, br);
}

bool HfsPlusVolume::ReadForkData(const ForkData &fork, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead)
{
    bytesRead = 0;
    if (!buf || len == 0) return true;

    uint8_t *dst = (uint8_t*)buf;
    uint64_t remaining = len;

    // Walk extents until we reach 'offset' bytes in
    uint64_t extentBase = 0;
    for (const auto &ext : fork.extents) {
        uint64_t extBytes = (uint64_t)ext.blockCount * m_blockSize;
        if (offset >= extentBase + extBytes) {
            extentBase += extBytes;
            continue;
        }
        // We're in this extent
        uint64_t localOff = offset - extentBase;
        uint64_t diskOff = (uint64_t)ext.startBlock * m_blockSize + localOff;
        uint64_t canRead = extBytes - localOff;
        if (canRead > remaining) canRead = remaining;

        if (!RawRead(diskOff, dst, (uint32_t)canRead)) return false;

        dst      += canRead;
        offset   += canRead;
        bytesRead+= canRead;
        remaining-= canRead;
        extentBase += extBytes;
        if (remaining == 0) break;
    }
    return true;
}

bool HfsPlusVolume::LoadBTreeHeader(const ForkData &fork, BTHeaderRec &hdr, uint32_t &nodeSize)
{
    // Node 0 is the header node; its header record starts at byte 14
    std::vector<uint8_t> node0(512);
    uint64_t br = 0;
    if (!ReadForkData(fork, 0, node0.data(), 512, br)) return false;

    // nodeSize is at offset 14+2+4+4+4+4+2 = 14+20 = 34 within header record
    // BTHeaderRec starts at offset 14 (after BTNodeDescriptor)
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
    p += 2; // reserved1
    hdr.clumpSize     = BE32(p);      p += 4;
    hdr.btreeType     = *p++;
    hdr.keyCompareType= *p++;
    hdr.attributes    = BE32(p);

    nodeSize = hdr.nodeSize;
    if (nodeSize == 0 || (nodeSize & (nodeSize-1)) != 0) return false;
    if (nodeSize < 512 || nodeSize > 32768) return false;
    return true;
}

std::wstring HfsPlusVolume::ParseHFSPlusString(const uint8_t *p, uint16_t length)
{
    return Utf16BEToWide(p, length);
}

bool HfsPlusVolume::ListDirectory(uint32_t parentCNID, std::vector<CatalogItem> &out)
{
    if (!m_open) return false;

    BTHeaderRec btHdr;
    uint32_t nodeSize = m_nodeSizeCatalog;
    if (nodeSize == 0) {
        if (!LoadBTreeHeader(m_catalogFork, btHdr, nodeSize)) return false;
        m_nodeSizeCatalog = nodeSize;
    } else {
        // Reload header anyway
        LoadBTreeHeader(m_catalogFork, btHdr, nodeSize);
    }

    // Traverse all leaf nodes
    uint32_t curNode = btHdr.firstLeafNode;
    int maxIter = (int)btHdr.totalNodes + 2;

    while (curNode != 0 && maxIter-- > 0) {
        std::vector<uint8_t> nodeBuf;
        if (!ReadBTreeNodeStatic(this, m_catalogFork, curNode, nodeSize, nodeBuf)) break;

        const uint8_t *nd = nodeBuf.data();
        uint32_t fLink   = BE32(nd);
        uint8_t  kind    = nd[8];
        uint16_t numRec  = BE16(nd + 10);

        if (kind != (uint8_t)kBTLeafNode) { curNode = fLink; continue; }

        for (uint16_t r = 0; r < numRec; r++) {
            if (nodeSize < (uint32_t)(r+1)*2 + 1) break;
            uint32_t offTableIdx = nodeSize - (uint32_t)(r+1)*2;
            uint16_t recOff = BE16(nd + offTableIdx);
            if (recOff < 14 || (uint32_t)recOff + 8 > nodeSize) continue;

            const uint8_t *rec = nd + recOff;
            uint16_t keyLen  = BE16(rec);
            if (keyLen < 6 || (uint32_t)recOff + 2 + keyLen + 2 > nodeSize) continue;

            uint32_t pid     = BE32(rec + 2);
            uint16_t nameLen = BE16(rec + 6);

            if (pid != parentCNID) continue;

            uint32_t dataOff = recOff + 2 + keyLen;
            if (dataOff + 2 > nodeSize) continue;

            int16_t recType = (int16_t)BE16(nd + dataOff);
            if (recType != kHFSPlusFolderRecord && recType != kHFSPlusFileRecord) continue;

            CatalogItem item = {};
            item.parentCNID = pid;
            item.isDir = (recType == kHFSPlusFolderRecord);

            if (nameLen > 0 && (uint32_t)recOff + 8 + (uint32_t)nameLen*2 <= nodeSize) {
                item.name = ParseHFSPlusString(rec + 8, nameLen);
            }

            const uint8_t *d = nd + dataOff + 2; // skip recType

            if (recType == kHFSPlusFolderRecord) {
                // flags(2) + valence(4) + folderID(4) + createDate(4) + ...
                item.cnid       = BE32(d + 6);
                item.createTime = BE32(d + 10);
                item.modifyTime = BE32(d + 14);
            } else {
                // flags(2) + reserved(2) + fileID(4) + createDate(4) + modifyDate(4) + ...
                item.cnid       = BE32(d + 4);
                item.createTime = BE32(d + 8);
                item.modifyTime = BE32(d + 12);
                // data fork is at offset 88 from start of file record (after type field)
                // File record layout: type(2) + flags(2) + reserved(4) + fileID(4) +
                //   createDate(4) + contentModDate(4) + attributeModDate(4) + accessDate(4) + backupDate(4)
                //   + permissions(16) + userInfo(16) + finderInfo(16) + textEncoding(4) + reserved(4)
                //   = 2+2+4+4 = 12, then dates 5*4=20, perms 16, ui+fi=32, enc+res=8 => total 88
                // Then dataFork(80) + resourceFork(80)
                if (dataOff + 2 + 88 + 80 <= nodeSize) {
                    item.dataFork     = ParseForkStatic(nd + dataOff + 2 + 88);
                    item.resourceFork = ParseForkStatic(nd + dataOff + 2 + 88 + 80);
                }
                item.dataSize = item.dataFork.logicalSize;
            }

            // Try decmpfs attribute for compressed files
            if (!item.isDir) {
                LoadDecmpfsAttr(item.cnid, item);
                if (item.hasDecmpfs) {
                    item.dataSize = item.decmpfsUnpackedSize;
                }
            }

            out.push_back(std::move(item));
        }

        curNode = fLink;
    }
    return true;
}

bool HfsPlusVolume::LoadDecmpfsAttr(uint32_t cnid, CatalogItem &item)
{
    item.hasDecmpfs = false;
    if (m_attrFork.totalBlocks == 0) return false;

    BTHeaderRec btHdr;
    uint32_t nodeSize;
    if (!LoadBTreeHeader(m_attrFork, btHdr, nodeSize)) return false;
    if (btHdr.firstLeafNode == 0) return false;

    uint32_t curNode = btHdr.firstLeafNode;
    int maxIter = (int)btHdr.totalNodes + 2;

    // Attribute key: keyLength(2) + pad(2) + fileID(4) + startBlock(4) + attrName(2+2*n)
    // We search for fileID == cnid and attrName == "com.apple.decmpfs"
    static const wchar_t kDecmpfs[] = L"com.apple.decmpfs";
    static const uint16_t kDecmpfsLen = 17;

    while (curNode != 0 && maxIter-- > 0) {
        std::vector<uint8_t> nodeBuf;
        if (!ReadBTreeNodeStatic(this, m_attrFork, curNode, nodeSize, nodeBuf)) break;

        const uint8_t *nd = nodeBuf.data();
        uint32_t fLink   = BE32(nd);
        uint8_t  kind    = nd[8];
        uint16_t numRec  = BE16(nd + 10);

        if (kind != (uint8_t)kBTLeafNode) { curNode = fLink; continue; }

        for (uint16_t r = 0; r < numRec; r++) {
            if (nodeSize < (uint32_t)(r+1)*2 + 1) break;
            uint32_t offTableIdx = nodeSize - (uint32_t)(r+1)*2;
            uint16_t recOff = BE16(nd + offTableIdx);
            if (recOff < 14 || (uint32_t)recOff + 12 > nodeSize) continue;

            const uint8_t *rec = nd + recOff;
            uint16_t keyLen = BE16(rec);
            if (keyLen < 10) continue;

            uint32_t fileID = BE32(rec + 4);
            if (fileID != cnid) continue;

            uint16_t attrNameLen = BE16(rec + 10);
            if (attrNameLen != kDecmpfsLen) continue;
            if ((uint32_t)recOff + 2 + keyLen > nodeSize) continue;

            // Compare attribute name (UTF-16 BE)
            bool nameMatch = true;
            for (int i = 0; i < kDecmpfsLen && nameMatch; i++) {
                uint16_t c = BE16(rec + 12 + i*2);
                if (c != (uint16_t)kDecmpfs[i]) nameMatch = false;
            }
            if (!nameMatch) continue;

            // Found! Parse attribute record data
            uint32_t dataOff = recOff + 2 + keyLen;
            if (dataOff + 4 > nodeSize) continue;

            // Attribute record: type(4) + reserved(12) + attrData...
            // type 0x10 = inline data, type 0x20 = fork data
            uint32_t attrRecType = BE32(nd + dataOff);
            if (attrRecType == 0x10) {
                // Inline: type(4) + reserved(12) = 16 bytes header, then data
                uint32_t inlineDataOff = dataOff + 16;
                if (inlineDataOff + 16 > nodeSize) continue;

                const uint8_t *ad = nd + inlineDataOff;
                // decmpfs header: magic "fpmc" (little-endian) + method(4) + unpackedSize(8)
                uint32_t magic = ad[0]|(uint32_t)ad[1]<<8|(uint32_t)ad[2]<<16|(uint32_t)ad[3]<<24;
                if (magic != 0x636D7066) continue; // "fpmc" LE

                item.decmpfsMethod      = ad[4]|(uint32_t)ad[5]<<8|(uint32_t)ad[6]<<16|(uint32_t)ad[7]<<24;
                item.decmpfsUnpackedSize= (uint64_t)(ad[8]|(uint32_t)ad[9]<<8|(uint32_t)ad[10]<<16|(uint32_t)ad[11]<<24) |
                                          ((uint64_t)(ad[12]|(uint32_t)ad[13]<<8|(uint32_t)ad[14]<<16|(uint32_t)ad[15]<<24) << 32);
                item.hasDecmpfs = true;

                // Copy remaining attribute bytes (compressed data)
                uint32_t attrDataEnd = 0;
                if (r + 1 < numRec) {
                    uint16_t nextOff = BE16(nd + nodeSize - (r+2)*2);
                    if (nextOff > inlineDataOff) attrDataEnd = nextOff;
                }
                if (attrDataEnd == 0) attrDataEnd = nodeSize - 2*(numRec+1);
                uint32_t attrPayloadSize = (attrDataEnd > inlineDataOff + 16) ?
                                           (attrDataEnd - inlineDataOff - 16) : 0;
                if (attrPayloadSize > 0 && attrPayloadSize < 64*1024) {
                    item.decmpfsAttrData.assign(ad + 16, ad + 16 + attrPayloadSize);
                }
            }
            return true;
        }

        curNode = fLink;
    }
    return false;
}

bool HfsPlusVolume::ReadFileData(const CatalogItem &item, uint64_t offset, void *buf, uint64_t len, uint64_t &bytesRead)
{
    bytesRead = 0;
    if (!m_open || !buf || len == 0) return false;

    if (item.isDir) return false;

    // If compressed via decmpfs attribute-inline, decompress in memory
    if (item.hasDecmpfs) {
        uint32_t method = item.decmpfsMethod;
        uint64_t unpackedSize = item.decmpfsUnpackedSize;

        // Method 9 = COPY (uncompressed, stored inline after header)
        if (method == 9 || method == 1) {
            // Data is raw in decmpfsAttrData
            if (offset >= (uint64_t)item.decmpfsAttrData.size()) return true;
            uint64_t canRead = (uint64_t)item.decmpfsAttrData.size() - offset;
            if (canRead > len) canRead = len;
            memcpy(buf, item.decmpfsAttrData.data() + offset, (size_t)canRead);
            bytesRead = canRead;
            return true;
        }

        // Method 3 = ZLIB inline attr
        if (method == 3 && !item.decmpfsAttrData.empty()) {
            const uint8_t *src = item.decmpfsAttrData.data();
            size_t srcLen = item.decmpfsAttrData.size();

            // First byte 0x0F means uncompressed
            if (src[0] == 0x0F && srcLen > 1) {
                uint64_t dataLen = srcLen - 1;
                if (offset >= dataLen) return true;
                uint64_t canRead = dataLen - offset;
                if (canRead > len) canRead = len;
                memcpy(buf, src + 1 + offset, (size_t)canRead);
                bytesRead = canRead;
                return true;
            }

            // Decompress with zlib
            std::vector<uint8_t> decompressed((size_t)unpackedSize);
            mz_ulong destLen = (mz_ulong)unpackedSize;
            int ret = mz_uncompress(decompressed.data(), &destLen, src, (mz_ulong)srcLen);
            if (ret != MZ_OK) return false;

            if (offset >= destLen) return true;
            uint64_t canRead = destLen - offset;
            if (canRead > len) canRead = len;
            memcpy(buf, decompressed.data() + offset, (size_t)canRead);
            bytesRead = canRead;
            return true;
        }

        // Method 7 = LZVN inline attr
        if (method == 7 && !item.decmpfsAttrData.empty()) {
            const uint8_t *src = item.decmpfsAttrData.data();
            size_t srcLen = item.decmpfsAttrData.size();

            // First byte 0x06 means uncompressed
            if (src[0] == 0x06 && srcLen > 1) {
                uint64_t dataLen = srcLen - 1;
                if (offset >= dataLen) return true;
                uint64_t canRead = dataLen - offset;
                if (canRead > len) canRead = len;
                memcpy(buf, src + 1 + offset, (size_t)canRead);
                bytesRead = canRead;
                return true;
            }

            std::vector<uint8_t> decompressed((size_t)unpackedSize + 8);
            lzvn_decoder_state state;
            memset(&state, 0, sizeof(state));
            state.src = src;
            state.src_end = src + srcLen;
            state.dst = decompressed.data();
            state.dst_begin = decompressed.data();
            state.dst_end = decompressed.data() + unpackedSize + 8;
            state.dst_current = decompressed.data();
            lzvn_decode(&state);
            size_t decoded = (size_t)(state.dst - decompressed.data());

            if (offset >= decoded) return true;
            uint64_t canRead = decoded - offset;
            if (canRead > len) canRead = len;
            memcpy(buf, decompressed.data() + offset, (size_t)canRead);
            bytesRead = canRead;
            return true;
        }

        // For resource-fork compression methods (4=ZLIB-rsrc, 8=LZVN-rsrc, 12=LZFSE-rsrc):
        // Fall through to read data fork as raw (the compressed data is in the resource fork)
        // For now just read data fork raw for unsupported methods
    }

    // Plain read from data fork
    if (item.dataFork.totalBlocks == 0 && item.dataFork.logicalSize == 0) return true;
    return ReadForkData(item.dataFork, offset, buf, len, bytesRead);
}

} // namespace HfsPlus
