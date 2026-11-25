#include "hfsplus_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fails = 0;

static void Expect(bool ok, const char *msg)
{
    if (!ok) {
        std::printf("FAIL %s\n", msg);
        g_fails++;
    }
}

static void TestVolumeForkOffsets()
{
    uint8_t hdr[512] = {};
    hdr[0] = 0x48;
    hdr[1] = 0x2B;
    hdr[42] = 0x10;          // blockSize 4096 at offset 40
    hdr[47] = 100;           // totalBlocks at offset 44
    hdr[278] = 0x10;         // catalog fork logicalSize 4096 at offset 272
    hdr[287] = 3;            // catalog totalBlocks
    hdr[291] = 100;          // first extent startBlock
    hdr[295] = 3;            // first extent blockCount

    uint32_t blockSize = 0;
    uint32_t totalBlocks = 0;
    HfsPlus::ForkData extents, catalog, attrs;
    Expect(HfsPlus::ParseHfsVolumeForks(hdr, sizeof(hdr), blockSize, totalBlocks, extents, catalog, attrs),
           "header parses");
    Expect(blockSize == 4096, "block size");
    Expect(totalBlocks == 100, "total blocks");
    Expect(catalog.logicalSize == 4096, "catalog logical size is at offset 272");
    Expect(catalog.totalBlocks == 3, "catalog block count");
    Expect(catalog.extents.size() == 1 && catalog.extents[0].startBlock == 100 && catalog.extents[0].blockCount == 3,
           "catalog extent");
    Expect(extents.logicalSize == 0 && attrs.logicalSize == 0, "other forks stay at their own offsets");
}

static void TestFileRecordOffsets()
{
    uint8_t rec[248] = {};
    rec[1] = 0x02;             // file record
    rec[4] = rec[5] = rec[6] = rec[7] = 0xFF; // reserved, must not be the file id
    rec[8] = 0x12;
    rec[9] = 0x34;
    rec[10] = 0x56;
    rec[11] = 0x78;            // fileID at offset 8
    rec[15] = 5;               // createDate
    rec[19] = 6;               // contentModDate
    rec[95] = 42;              // data fork logicalSize at offset 88
    rec[107] = 9;              // data extent start
    rec[111] = 1;              // data extent count

    HfsPlus::CatalogItem item = {};
    Expect(HfsPlus::ParseHfsFileRecord(rec, sizeof(rec), item), "file record parses");
    Expect(item.cnid == 0x12345678, "file id is at offset 8");
    Expect(item.createTime == 5 && item.modifyTime == 6, "file dates");
    Expect(item.dataSize == 42, "data fork starts at offset 88");
    Expect(item.dataFork.extents.size() == 1 && item.dataFork.extents[0].startBlock == 9, "data extent");
}

static void TestAttrKeyOffsets()
{
    const wchar_t *name = L"com.apple.decmpfs";
    const uint16_t nameLen = 17;
    uint8_t rec[64] = {};
    uint16_t keyLen = (uint16_t)(12 + nameLen * 2);
    rec[1] = (uint8_t)keyLen;
    rec[7] = 7;                // fileID at offset 4
    rec[13] = (uint8_t)nameLen; // name length at offset 12, not 10
    for (uint16_t i = 0; i < nameLen; i++)
        rec[15 + i * 2] = (uint8_t)name[i];

    HfsPlus::AttrKeyView key = {};
    Expect(HfsPlus::ParseHfsAttrKey(rec, sizeof(rec), key), "attr key parses");
    Expect(key.fileID == 7 && key.startBlock == 0, "attr file id");
    Expect(key.nameLen == nameLen, "attr name length is at offset 12");
    bool same = key.nameLen == nameLen;
    for (uint16_t i = 0; same && i < nameLen; i++)
        same = key.name[i * 2 + 1] == (uint8_t)name[i];
    Expect(same, "attr name is at offset 14");
}

static void W16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void W32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void W64(uint8_t *p, uint64_t v) { W32(p, (uint32_t)(v >> 32)); W32(p + 4, (uint32_t)v); }

static std::vector<uint8_t> CatKey(uint32_t parent, const wchar_t *name)
{
    uint16_t nlen = 0;
    while (name[nlen]) nlen++;
    std::vector<uint8_t> k(8u + (size_t)nlen * 2, 0);
    W16(k.data(), (uint16_t)(6 + nlen * 2));
    W32(k.data() + 2, parent);
    W16(k.data() + 6, nlen);
    for (uint16_t i = 0; i < nlen; i++)
        W16(k.data() + 8 + i * 2, (uint16_t)name[i]);
    return k;
}

static std::vector<uint8_t> FinishNode(uint32_t nodeSize, uint8_t kind, uint8_t height,
                                       const std::vector<std::vector<uint8_t>> &recs)
{
    std::vector<uint8_t> n(nodeSize, 0);
    n[8] = kind;
    n[9] = height;
    W16(n.data() + 10, (uint16_t)recs.size());
    uint32_t pos = 14;
    std::vector<uint16_t> offs;
    for (const auto &r : recs) {
        if (pos + r.size() + 2 * (recs.size() + 1) > nodeSize) {
            std::printf("FAIL node overflow\n");
            g_fails++;
            return n;
        }
        offs.push_back((uint16_t)pos);
        std::memcpy(n.data() + pos, r.data(), r.size());
        pos += (uint32_t)r.size();
    }
    offs.push_back((uint16_t)pos);
    for (size_t i = 0; i < offs.size(); i++)
        W16(n.data() + nodeSize - 2 * (i + 1), offs[i]);
    return n;
}

static std::vector<uint8_t> HeaderNode(uint32_t bytes, uint32_t root, uint16_t treeNodeSize)
{
    std::vector<uint8_t> n(bytes, 0);
    n[8] = 0x01;
    W16(n.data() + 14, 1);
    W32(n.data() + 16, root);
    W16(n.data() + 32, treeNodeSize);
    return n;
}

static std::vector<uint8_t> FileRecord(uint32_t parent, const wchar_t *name, uint32_t cnid,
                                       uint64_t logical, uint32_t blocks, uint32_t extStart, uint32_t extCount)
{
    auto rec = CatKey(parent, name);
    size_t body = rec.size();
    rec.resize(body + 248, 0);
    uint8_t *b = rec.data() + body;
    W16(b, 2);
    W32(b + 8, cnid);
    W64(b + 88, logical);
    W32(b + 100, blocks);
    if (extCount) {
        W32(b + 104, extStart);
        W32(b + 108, extCount);
    }
    return rec;
}

static std::vector<uint8_t> AttrKey(uint32_t fileID)
{
    const wchar_t *name = L"com.apple.decmpfs";
    std::vector<uint8_t> k(48, 0);
    W16(k.data(), 46);
    W32(k.data() + 4, fileID);
    W16(k.data() + 12, 17);
    for (uint16_t i = 0; i < 17; i++)
        W16(k.data() + 14 + i * 2, (uint16_t)name[i]);
    return k;
}

static std::vector<uint8_t> Decmpfs(const char *text)
{
    size_t n = std::strlen(text);
    std::vector<uint8_t> p(16 + 1 + n, 0);
    p[0] = 'f'; p[1] = 'p'; p[2] = 'm'; p[3] = 'c';
    p[4] = 9;
    p[8] = (uint8_t)n;
    p[16] = 0xCC;
    std::memcpy(p.data() + 17, text, n);
    return p;
}

static void Place(std::vector<uint8_t> &disk, uint32_t block, const std::vector<uint8_t> &node)
{
    size_t off = (size_t)block * 512;
    Expect(off + node.size() <= disk.size(), "node fits image");
    if (off + node.size() <= disk.size())
        std::memcpy(disk.data() + off, node.data(), node.size());
}

static const HfsPlus::CatalogItem *Pick(const std::vector<HfsPlus::CatalogItem> &items, const wchar_t *name)
{
    for (const auto &it : items)
        if (it.name == name) return &it;
    return nullptr;
}

static void TestVolumeBehaviors()
{
    const uint32_t kBlocks = 51;
    std::vector<uint8_t> disk((size_t)kBlocks * 512, 0);
    uint8_t *hdr = disk.data() + 1024;
    W16(hdr, 0x482B);
    W32(hdr + 40, 512);
    W32(hdr + 44, 64);
    // Extents fork: header node at block 24, leaf node lives at block 200 (past EOF on purpose).
    W64(hdr + 192, 1024);
    W32(hdr + 204, 2);
    W32(hdr + 208, 24);
    W32(hdr + 212, 1);
    W32(hdr + 216, 200);
    W32(hdr + 220, 1);
    W64(hdr + 272, 6144);
    W32(hdr + 284, 12);
    W32(hdr + 288, 8);
    W32(hdr + 292, 12);
    W64(hdr + 352, 1024);
    W32(hdr + 364, 2);
    W32(hdr + 368, 32);
    W32(hdr + 372, 2);

    auto threadKey = CatKey(2, L"");
    threadKey.resize(threadKey.size() + 18, 0);
    uint8_t *tb = threadKey.data() + (threadKey.size() - 18);
    W16(tb, 3);
    W32(tb + 4, 1);
    W16(tb + 8, 4);
    W16(tb + 10, L'T'); W16(tb + 12, L'e'); W16(tb + 14, L's'); W16(tb + 16, L't');

    std::vector<std::vector<uint8_t>> leafRecs;
    leafRecs.push_back(threadKey);
    leafRecs.push_back(FileRecord(2, L"afork", 19, 0, 0, 0, 0));
    leafRecs.push_back(FileRecord(2, L"bad", 20, 0, 0, 0, 0));
    leafRecs.push_back(FileRecord(2, L"big", 17, 1024, 2, 40, 1));
    leafRecs.push_back(FileRecord(2, L"cmp", 18, 0, 0, 0, 0));
    leafRecs.push_back(FileRecord(2, L"plain", 16, 5, 1, 42, 1));

    auto index0 = CatKey(2, L"");
    index0.resize(index0.size() + 4, 0);
    W32(index0.data() + index0.size() - 4, 2);
    auto index1 = CatKey(9, L"");
    index1.resize(index1.size() + 4, 0);
    W32(index1.data() + index1.size() - 4, 99);

    Place(disk, 8, HeaderNode(2048, 1, 2048));
    Place(disk, 12, FinishNode(2048, 0x00, 2, { index0, index1 }));
    Place(disk, 16, FinishNode(2048, 0xFF, 1, leafRecs));
    Place(disk, 24, HeaderNode(512, 1, 512));
    Place(disk, 32, HeaderNode(512, 1, 512));

    auto blob = Decmpfs("hi");
    auto inlineRec = AttrKey(18);
    size_t inlineBody = inlineRec.size();
    inlineRec.resize(inlineBody + 16 + blob.size(), 0);
    W32(inlineRec.data() + inlineBody, 0x10);
    W32(inlineRec.data() + inlineBody + 12, (uint32_t)blob.size());
    std::memcpy(inlineRec.data() + inlineBody + 16, blob.data(), blob.size());

    auto forkBlob = Decmpfs("yz");
    auto forkRec = AttrKey(19);
    forkRec.resize(forkRec.size() + 88, 0);
    uint8_t *fb = forkRec.data() + (forkRec.size() - 88);
    W32(fb, 0x20);
    W64(fb + 8, forkBlob.size());
    W32(fb + 20, 1);
    W32(fb + 24, 50);
    W32(fb + 28, 1);

    auto badRec = AttrKey(20);
    badRec.resize(badRec.size() + 16, 0);
    W32(badRec.data() + badRec.size() - 16, 0x99);

    Place(disk, 33, FinishNode(512, 0xFF, 1, { inlineRec, forkRec, badRec }));
    std::memset(disk.data() + 40 * 512, 'A', 512);
    std::memset(disk.data() + 41 * 512, 'B', 512);
    std::memcpy(disk.data() + 42 * 512, "HELLO", 5);
    std::memcpy(disk.data() + 50 * 512, forkBlob.data(), forkBlob.size());

    wchar_t tmp[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    Expect(tn > 0 && tn < MAX_PATH, "temp path");
    if (tn == 0 || tn >= MAX_PATH) return;
    std::wstring path = std::wstring(tmp) + L"hfs_layout_vol.bin";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    Expect(h != INVALID_HANDLE_VALUE, "create image");
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    BOOL wok = WriteFile(h, disk.data(), (DWORD)disk.size(), &written, nullptr);
    Expect(wok && written == disk.size(), "write image");

    HfsPlus::HfsPlusVolume vol;
    bool opened = vol.Open(h, 0, disk.size(), 512);
    Expect(opened, "volume opens");
    std::vector<HfsPlus::CatalogItem> items;
    if (opened)
        Expect(vol.ListDirectory(2, items), "list root");

    const HfsPlus::CatalogItem *plain = Pick(items, L"plain");
    const HfsPlus::CatalogItem *cmp = Pick(items, L"cmp");
    const HfsPlus::CatalogItem *afork = Pick(items, L"afork");
    const HfsPlus::CatalogItem *bad = Pick(items, L"bad");
    const HfsPlus::CatalogItem *big = Pick(items, L"big");
    Expect(plain && cmp && afork && bad && big, "five files listed");

    if (plain) {
        uint8_t got[5] = {};
        uint64_t br = 0;
        Expect(plain->dataSize == 5 && vol.ReadFileData(*plain, 0, got, 5, br) && br == 5 &&
               std::memcmp(got, "HELLO", 5) == 0, "plain file");
    }
    if (cmp) {
        uint8_t got[2] = {};
        uint64_t br = 0;
        Expect(cmp->hasDecmpfs && cmp->dataSize == 2 && vol.ReadFileData(*cmp, 0, got, 2, br) &&
               br == 2 && got[0] == 'h' && got[1] == 'i', "inline decmpfs");
    }
    if (afork) {
        uint8_t got[2] = {};
        uint64_t br = 0;
        Expect(afork->hasDecmpfs && afork->dataSize == 2 && vol.ReadFileData(*afork, 0, got, 2, br) &&
               br == 2 && got[0] == 'y' && got[1] == 'z', "attribute fork decmpfs");
    }
    if (bad) {
        uint8_t b = 0;
        uint64_t br = 0;
        Expect(bad->hasDecmpfs && bad->decmpfsMethod == 0 && !vol.ReadFileData(*bad, 0, &b, 1, br),
               "unknown decmpfs does not fall through to the data fork");
    }
    if (big) {
        std::vector<uint8_t> got(1024);
        uint64_t br = 0;
        Expect(!vol.ReadFileData(*big, 0, got.data(), got.size(), br), "extents read error is not a short success");

        std::vector<uint8_t> leaf = FinishNode(512, 0xFF, 1, { [&]() {
            std::vector<uint8_t> rec(76, 0);
            W16(rec.data(), 10);
            W32(rec.data() + 4, 17);
            W32(rec.data() + 8, 1);
            W32(rec.data() + 12, 41);
            W32(rec.data() + 16, 1);
            return rec;
        }() });
        LARGE_INTEGER pos;
        pos.QuadPart = 201ll * 512;
        Expect(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && SetEndOfFile(h), "extend image");
        pos.QuadPart = 200ll * 512;
        DWORD lw = 0;
        Expect(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
               WriteFile(h, leaf.data(), (DWORD)leaf.size(), &lw, nullptr) && lw == leaf.size(),
               "write extents leaf");

        br = 0;
        bool ok = vol.ReadFileData(*big, 0, got.data(), got.size(), br);
        bool bytes = br == 1024;
        for (int i = 0; bytes && i < 512; i++) bytes = got[i] == 'A' && got[512 + i] == 'B';
        Expect(ok && bytes, "overflow extent is read after the failed attempt");
    }

    vol.Close();
    CloseHandle(h);
    DeleteFileW(path.c_str());
}

int main()
{
    TestVolumeForkOffsets();
    TestFileRecordOffsets();
    TestAttrKeyOffsets();
    TestVolumeBehaviors();
    if (g_fails) {
        std::printf("%d failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
