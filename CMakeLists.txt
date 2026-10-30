cmake_minimum_required(VERSION 3.15)
project(TotalCommanderApfsPlugin LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W3 /utf-8 /wd4996 /wd4244 /wd4267)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _DARWIN_USE_64_BIT_INODE _FILE_OFFSET_BITS=64 NOMINMAX)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/res
    ${CMAKE_CURRENT_SOURCE_DIR}/apfs-fuse-src
    ${CMAKE_CURRENT_SOURCE_DIR}/apfs-fuse-src/3rdparty/lzfse/src
    ${CMAKE_CURRENT_SOURCE_DIR}/miniz-src
    ${CMAKE_CURRENT_SOURCE_DIR}/bzip2-src
)

# 1. miniz (zlib replacement)
add_library(miniz STATIC
    miniz-src/miniz.c
    miniz-src/miniz_tdef.c
    miniz-src/miniz_tinfl.c
    miniz-src/miniz_zip.c
)

# 2. bzip2
add_library(bzip2 STATIC
    bzip2-src/blocksort.c
    bzip2-src/huffman.c
    bzip2-src/crctable.c
    bzip2-src/randtable.c
    bzip2-src/compress.c
    bzip2-src/decompress.c
    bzip2-src/bzlib.c
)

# 3. lzfse & lzvn
add_library(lzfse STATIC
    apfs-fuse-src/3rdparty/lzfse/src/lzfse_decode.c
    apfs-fuse-src/3rdparty/lzfse/src/lzfse_decode_base.c
    apfs-fuse-src/3rdparty/lzfse/src/lzfse_encode.c
    apfs-fuse-src/3rdparty/lzfse/src/lzfse_encode_base.c
    apfs-fuse-src/3rdparty/lzfse/src/lzfse_fse.c
    apfs-fuse-src/3rdparty/lzfse/src/lzvn_decode_base.c
    apfs-fuse-src/3rdparty/lzfse/src/lzvn_encode_base.c
)

# 4. Crypto
add_library(crypto STATIC
    apfs-fuse-src/Crypto/Aes.cpp
    apfs-fuse-src/Crypto/AesXts.cpp
    apfs-fuse-src/Crypto/Asn1Der.cpp
    apfs-fuse-src/Crypto/Crypto.cpp
    apfs-fuse-src/Crypto/Des.cpp
    apfs-fuse-src/Crypto/Sha1.cpp
    apfs-fuse-src/Crypto/Sha256.cpp
    apfs-fuse-src/Crypto/TripleDes.cpp
)

# 5. ApfsLib
add_library(apfslib STATIC
    apfs-fuse-src/ApfsLib/ApfsContainer.cpp
    apfs-fuse-src/ApfsLib/ApfsDir.cpp
    apfs-fuse-src/ApfsLib/ApfsNodeMapper.cpp
    apfs-fuse-src/ApfsLib/ApfsNodeMapperBTree.cpp
    apfs-fuse-src/ApfsLib/ApfsVolume.cpp
    apfs-fuse-src/ApfsLib/BlockDumper.cpp
    apfs-fuse-src/ApfsLib/BTree.cpp
    apfs-fuse-src/ApfsLib/CheckPointMap.cpp
    apfs-fuse-src/ApfsLib/Crc32.cpp
    apfs-fuse-src/ApfsLib/Decmpfs.cpp
    apfs-fuse-src/ApfsLib/Device.cpp
    apfs-fuse-src/ApfsLib/DeviceDMG.cpp
    apfs-fuse-src/ApfsLib/DeviceSparseImage.cpp
    apfs-fuse-src/ApfsLib/DeviceVDI.cpp
    apfs-fuse-src/ApfsLib/DeviceWinFile.cpp
    apfs-fuse-src/ApfsLib/DeviceWinPhys.cpp
    apfs-fuse-src/ApfsLib/DiskImageFile.cpp
    apfs-fuse-src/ApfsLib/GptPartitionMap.cpp
    apfs-fuse-src/ApfsLib/KeyMgmt.cpp
    apfs-fuse-src/ApfsLib/PList.cpp
    apfs-fuse-src/ApfsLib/Unicode.cpp
    apfs-fuse-src/ApfsLib/Util.cpp
)
target_link_libraries(apfslib miniz bzip2 lzfse crypto)

# 6. Total Commander Plugin DLL (.wfx64 or .wfx)
set(PLUGIN_SOURCES
    src/plugin_main.cpp
    src/apfs_vfs.cpp
    src/drive_enumerator.cpp
    src/hfsplus_reader.cpp
    src/apfs.def
    res/resource.rc
)

add_library(tc_apfs SHARED ${PLUGIN_SOURCES})
target_link_libraries(tc_apfs apfslib gdiplus shlwapi comctl32)

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLUGIN_EXTENSION "wfx64")
else()
    set(PLUGIN_EXTENSION "wfx")
endif()

set_target_properties(tc_apfs PROPERTIES
    OUTPUT_NAME "apfs"
    PREFIX ""
    SUFFIX ".${PLUGIN_EXTENSION}"
)
