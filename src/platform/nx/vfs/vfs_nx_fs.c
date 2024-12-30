/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include "../utils.h"
#include <errno.h>
#include <string.h>

/*
the read speed of transfers was really fast, even with small 16k buffers
however the write speed was very poor, around 1.5MiB/s...

i performed some benchmarks by writing an empty buffer to a file
using native fs calls:

# 64k write of a 400mb file will take 83s
# 128k write of a 400mb file will take 46s
# 256k write of a 400mb file will take 25s
# 512k write of a 400mb file will take 15s
# 1mb write of a 400mb file will take 11s
# 8mb write of a 400mb file will take 8s

as we can see, 1-8MB version is substantially faster!
for an application, we can easily afford 8MiB, so that's the end of that.

with a sys-mod however, we are not so lucky as we are very much limited
by memory!

i had an idea, what if instead of buffering the write, we set update the
file size using fsFileSetSize() and set it to a large number, say 1MiB,
then write to 64k of data, then when > 1MiB, resize it another 1Mib.

My hope with this was part of the bottleneck was the implicit calls to
fsFileSetSize due to using append when opening the file.
Below is my findings:

# 64k write of a 400mb with fsFileSetSize() at 1mb chunks takes 29s.
# 1mb write of a 400mb with fsFileSetSize() at 1mb chunks takes 12s.

As can be seen, the setsize() version performed 2.5x better than
the original version.
It's still slow, but thats the cost of being memory bound.

I was worried that calling setsize() with a large number for a small
file would cause slowdown, but this doesn't prove to be the case.

writing 400 64k files using setsize takes 9s, whereas without it takes
7s, this does show there is overhead, but not much.
*/

// given the above, in my testing, 8MiB was the fastest.
// anything above didn't improve anything, anything less slowed down.
#define NX_WRITE_CHUNK_SIZE (1024ULL*1024ULL*1ULL)

enum FsError {
    FsError_PathNotFound = 0x202,
    FsError_PathAlreadyExists = 0x402,
    FsError_TargetLocked = 0xE02,
    FsError_UsableSpaceNotEnoughMmcCalibration = 0x4602,
    FsError_UsableSpaceNotEnoughMmcSafe = 0x4802,
    FsError_UsableSpaceNotEnoughMmcUser = 0x4A02,
    FsError_UsableSpaceNotEnoughMmcSystem = 0x4C02,
    FsError_UsableSpaceNotEnoughSdCard = 0x4E02,
    FsError_UnsupportedSdkVersion = 0x6402,
    FsError_MountNameAlreadyExists = 0x7802,
    FsError_PartitionNotFound = 0x7D202,
    FsError_TargetNotFound = 0x7D402,
    FsError_PortSdCardNoDevice = 0xFA202,
    FsError_GameCardCardNotInserted = 0x13B002,
    FsError_GameCardCardNotActivated = 0x13B402,
    FsError_GameCardInvalidSecureAccess = 0x13D802,
    FsError_GameCardInvalidNormalAccess = 0x13DA02,
    FsError_GameCardInvalidAccessAcrossMode = 0x13DC02,
    FsError_GameCardInitialDataMismatch = 0x13E002,
    FsError_GameCardInitialNotFilledWithZero = 0x13E202,
    FsError_GameCardKekIndexMismatch = 0x13E402,
    FsError_GameCardCardHeaderReadFailure = 0x13EE02,
    FsError_GameCardShouldTransitFromInitialToNormal = 0x145002,
    FsError_GameCardShouldTransitFromNormalModeToSecure = 0x145202,
    FsError_GameCardShouldTransitFromNormalModeToDebug = 0x145402,
    FsError_GameCardSendFirmwareFailure = 0x149402,
    FsError_GameCardReceiveCertificateFailure = 0x149A02,
    FsError_GameCardSendSocCertificateFailure = 0x14A002,
    FsError_GameCardReceiveRandomValueFailure = 0x14AA02,
    FsError_GameCardSendRandomValueFailure = 0x14AC02,
    FsError_GameCardReceiveDeviceChallengeFailure = 0x14B602,
    FsError_GameCardRespondDeviceChallengeFailure = 0x14B802,
    FsError_GameCardSendHostChallengeFailure = 0x14BA02,
    FsError_GameCardReceiveChallengeResponseFailure = 0x14BC02,
    FsError_GameCardChallengeAndResponseFailure = 0x14BE02,
    FsError_GameCardSplGenerateRandomBytesFailure = 0x14D802,
    FsError_GameCardReadRegisterFailure = 0x14DE02,
    FsError_GameCardWriteRegisterFailure = 0x14E002,
    FsError_GameCardEnableCardBusFailure = 0x14E202,
    FsError_GameCardGetCardHeaderFailure = 0x14E402,
    FsError_GameCardAsicStatusError = 0x14E602,
    FsError_GameCardChangeGcModeToSecureFailure = 0x14E802,
    FsError_GameCardChangeGcModeToDebugFailure = 0x14EA02,
    FsError_GameCardReadRmaInfoFailure = 0x14EC02,
    FsError_GameCardStateCardSecureModeRequired = 0x150802,
    FsError_GameCardStateCardDebugModeRequired = 0x150A02,
    FsError_GameCardCommandReadId1Failure = 0x155602,
    FsError_GameCardCommandReadId2Failure = 0x155802,
    FsError_GameCardCommandReadId3Failure = 0x155A02,
    FsError_GameCardCommandReadPageFailure = 0x155E02,
    FsError_GameCardCommandWritePageFailure = 0x156202,
    FsError_GameCardCommandRefreshFailure = 0x156402,
    FsError_GameCardCommandReadCrcFailure = 0x156C02,
    FsError_GameCardCommandEraseFailure = 0x156E02,
    FsError_GameCardCommandReadDevParamFailure = 0x157002,
    FsError_GameCardCommandWriteDevParamFailure = 0x157202,
    FsError_GameCardDebugCardReceivedIdMismatch = 0x16B002,
    FsError_GameCardDebugCardId1Mismatch = 0x16B202,
    FsError_GameCardDebugCardId2Mismatch = 0x16B402,
    FsError_GameCardFsCheckHandleInGetStatusFailure = 0x171402,
    FsError_GameCardFsCheckHandleInCreateReadOnlyFailure = 0x172002,
    FsError_GameCardFsCheckHandleInCreateSecureReadOnlyFailure = 0x172202,
    FsError_NotImplemented = 0x177202,
    FsError_AlreadyExists = 0x177602,
    FsError_OutOfRange = 0x177A02,
    FsError_AllocationMemoryFailedInFatFileSystemA = 0x190202,
    FsError_AllocationMemoryFailedInFatFileSystemB = 0x190402,
    FsError_AllocationMemoryFailedInFatFileSystemC = 0x190602,
    FsError_AllocationMemoryFailedInFatFileSystemD = 0x190802,
    FsError_AllocationMemoryFailedInFatFileSystemE = 0x190A02,
    FsError_AllocationMemoryFailedInFatFileSystemF = 0x190C02,
    FsError_AllocationMemoryFailedInFatFileSystemG = 0x190E02,
    FsError_AllocationMemoryFailedInFatFileSystemH = 0x191002,
    FsError_AllocationMemoryFailedInSdCardA = 0x195802,
    FsError_AllocationMemoryFailedInSdCardB = 0x195A02,
    FsError_AllocationMemoryFailedInSystemSaveDataA = 0x195C02,
    FsError_AllocationMemoryFailedInRomFsFileSystemA = 0x195E02,
    FsError_AllocationMemoryFailedInRomFsFileSystemB = 0x196002,
    FsError_AllocationMemoryFailedInRomFsFileSystemC = 0x196202,
    FsError_AllocationMemoryFailedInSdmmcStorageServiceA = 0x1A3E02,
    FsError_AllocationMemoryFailedInBuiltInStorageCreatorA = 0x1A4002,
    FsError_AllocationMemoryFailedInRegisterA = 0x1A4A02,
    FsError_IncorrectSaveDataFileSystemMagicCode = 0x21BC02,
    FsError_InvalidAcidFileSize = 0x234202,
    FsError_InvalidAcidSize = 0x234402,
    FsError_InvalidAcid = 0x234602,
    FsError_AcidVerificationFailed = 0x234802,
    FsError_InvalidNcaSignature = 0x234A02,
    FsError_NcaHeaderSignature1VerificationFailed = 0x234C02,
    FsError_NcaHeaderSignature2VerificationFailed = 0x234E02,
    FsError_NcaFsHeaderHashVerificationFailed = 0x235002,
    FsError_InvalidNcaKeyIndex = 0x235202,
    FsError_InvalidNcaFsHeaderEncryptionType = 0x235602,
    FsError_InvalidNcaPatchInfoIndirectSize = 0x235802,
    FsError_InvalidNcaPatchInfoAesCtrExSize = 0x235A02,
    FsError_InvalidNcaPatchInfoAesCtrExOffset = 0x235C02,
    FsError_InvalidNcaId = 0x235E02,
    FsError_InvalidNcaHeader = 0x236002,
    FsError_InvalidNcaFsHeader = 0x236202,
    FsError_InvalidHierarchicalSha256BlockSize = 0x236802,
    FsError_InvalidHierarchicalSha256LayerCount = 0x236A02,
    FsError_HierarchicalSha256BaseStorageTooLarge = 0x236C02,
    FsError_HierarchicalSha256HashVerificationFailed = 0x236E02,
    FsError_InvalidSha256PartitionHashTarget = 0x244402,
    FsError_Sha256PartitionHashVerificationFailed = 0x244602,
    FsError_PartitionSignatureVerificationFailed = 0x244802,
    FsError_Sha256PartitionSignatureVerificationFailed = 0x244A02,
    FsError_InvalidPartitionEntryOffset = 0x244C02,
    FsError_InvalidSha256PartitionMetaDataSize = 0x244E02,
    FsError_InvalidFatFileNumber = 0x249802,
    FsError_InvalidFatFormatBisUser = 0x249C02,
    FsError_InvalidFatFormatBisSystem = 0x249E02,
    FsError_InvalidFatFormatBisSafe = 0x24A002,
    FsError_InvalidFatFormatBisCalibration = 0x24A202,
    FsError_AesXtsFileSystemFileHeaderCorruptedOnFileOpen = 0x250E02,
    FsError_AesXtsFileSystemFileNoHeaderOnFileOpen = 0x251002,
    FsError_FatFsFormatUnsupportedSize = 0x280202,
    FsError_FatFsFormatInvalidBpb = 0x280402,
    FsError_FatFsFormatInvalidParameter = 0x280602,
    FsError_FatFsFormatIllegalSectorsA = 0x280802,
    FsError_FatFsFormatIllegalSectorsB = 0x280A02,
    FsError_FatFsFormatIllegalSectorsC = 0x280C02,
    FsError_FatFsFormatIllegalSectorsD = 0x280E02,
    FsError_UnexpectedInMountTableA = 0x296A02,
    FsError_TooLongPath = 0x2EE602,
    FsError_InvalidCharacter = 0x2EE802,
    FsError_InvalidPathFormat = 0x2EEA02,
    FsError_DirectoryUnobtainable = 0x2EEC02,
    FsError_InvalidOffset = 0x2F5A02,
    FsError_InvalidSize = 0x2F5C02,
    FsError_NullptrArgument = 0x2F5E02,
    FsError_InvalidAlignment = 0x2F6002,
    FsError_InvalidMountName = 0x2F6202,
    FsError_ExtensionSizeTooLarge = 0x2F6402,
    FsError_ExtensionSizeInvalid = 0x2F6602,
    FsError_FileExtensionWithoutOpenModeAllowAppend = 0x307202,
    FsError_UnsupportedCommitTarget = 0x313A02,
    FsError_UnsupportedSetSizeForNotResizableSubStorage = 0x313C02,
    FsError_UnsupportedSetSizeForResizableSubStorage = 0x313E02,
    FsError_UnsupportedSetSizeForMemoryStorage = 0x314002,
    FsError_UnsupportedOperateRangeForMemoryStorage = 0x314202,
    FsError_UnsupportedOperateRangeForFileStorage = 0x314402,
    FsError_UnsupportedOperateRangeForFileHandleStorage = 0x314602,
    FsError_UnsupportedOperateRangeForSwitchStorage = 0x314802,
    FsError_UnsupportedOperateRangeForStorageServiceObjectAdapter = 0x314A02,
    FsError_UnsupportedWriteForAesCtrCounterExtendedStorage = 0x314C02,
    FsError_UnsupportedSetSizeForAesCtrCounterExtendedStorage = 0x314E02,
    FsError_UnsupportedOperateRangeForAesCtrCounterExtendedStorage = 0x315002,
    FsError_UnsupportedWriteForAesCtrStorageExternal = 0x315202,
    FsError_UnsupportedSetSizeForAesCtrStorageExternal = 0x315402,
    FsError_UnsupportedSetSizeForAesCtrStorage = 0x315602,
    FsError_UnsupportedSetSizeForHierarchicalIntegrityVerificationStorage = 0x315802,
    FsError_UnsupportedOperateRangeForHierarchicalIntegrityVerificationStorage = 0x315A02,
    FsError_UnsupportedSetSizeForIntegrityVerificationStorage = 0x315C02,
    FsError_UnsupportedOperateRangeForWritableIntegrityVerificationStorage = 0x315E02,
    FsError_UnsupportedOperateRangeForIntegrityVerificationStorage = 0x316002,
    FsError_UnsupportedSetSizeForBlockCacheBufferedStorage = 0x316202,
    FsError_UnsupportedOperateRangeForWritableBlockCacheBufferedStorage = 0x316402,
    FsError_UnsupportedOperateRangeForBlockCacheBufferedStorage = 0x316602,
    FsError_UnsupportedWriteForIndirectStorage = 0x316802,
    FsError_UnsupportedSetSizeForIndirectStorage = 0x316A02,
    FsError_UnsupportedOperateRangeForIndirectStorage = 0x316C02,
    FsError_UnsupportedWriteForZeroStorage = 0x316E02,
    FsError_UnsupportedSetSizeForZeroStorage = 0x317002,
    FsError_UnsupportedSetSizeForHierarchicalSha256Storage = 0x317202,
    FsError_UnsupportedWriteForReadOnlyBlockCacheStorage = 0x317402,
    FsError_UnsupportedSetSizeForReadOnlyBlockCacheStorage = 0x317602,
    FsError_UnsupportedSetSizeForIntegrityRomFsStorage = 0x317802,
    FsError_UnsupportedSetSizeForDuplexStorage = 0x317A02,
    FsError_UnsupportedOperateRangeForDuplexStorage = 0x317C02,
    FsError_UnsupportedSetSizeForHierarchicalDuplexStorage = 0x317E02,
    FsError_UnsupportedGetSizeForRemapStorage = 0x318002,
    FsError_UnsupportedSetSizeForRemapStorage = 0x318202,
    FsError_UnsupportedOperateRangeForRemapStorage = 0x318402,
    FsError_UnsupportedSetSizeForIntegritySaveDataStorage = 0x318602,
    FsError_UnsupportedOperateRangeForIntegritySaveDataStorage = 0x318802,
    FsError_UnsupportedSetSizeForJournalIntegritySaveDataStorage = 0x318A02,
    FsError_UnsupportedOperateRangeForJournalIntegritySaveDataStorage = 0x318C02,
    FsError_UnsupportedGetSizeForJournalStorage = 0x318E02,
    FsError_UnsupportedSetSizeForJournalStorage = 0x319002,
    FsError_UnsupportedOperateRangeForJournalStorage = 0x319202,
    FsError_UnsupportedSetSizeForUnionStorage = 0x319402,
    FsError_UnsupportedSetSizeForAllocationTableStorage = 0x319602,
    FsError_UnsupportedReadForWriteOnlyGameCardStorage = 0x319802,
    FsError_UnsupportedSetSizeForWriteOnlyGameCardStorage = 0x319A02,
    FsError_UnsupportedWriteForReadOnlyGameCardStorage = 0x319C02,
    FsError_UnsupportedSetSizeForReadOnlyGameCardStorage = 0x319E02,
    FsError_UnsupportedOperateRangeForReadOnlyGameCardStorage = 0x31A002,
    FsError_UnsupportedSetSizeForSdmmcStorage = 0x31A202,
    FsError_UnsupportedOperateRangeForSdmmcStorage = 0x31A402,
    FsError_UnsupportedOperateRangeForFatFile = 0x31A602,
    FsError_UnsupportedOperateRangeForStorageFile = 0x31A802,
    FsError_UnsupportedSetSizeForInternalStorageConcatenationFile = 0x31AA02,
    FsError_UnsupportedOperateRangeForInternalStorageConcatenationFile = 0x31AC02,
    FsError_UnsupportedQueryEntryForConcatenationFileSystem = 0x31AE02,
    FsError_UnsupportedOperateRangeForConcatenationFile = 0x31B002,
    FsError_UnsupportedSetSizeForZeroBitmapFile = 0x31B202,
    FsError_UnsupportedOperateRangeForFileServiceObjectAdapter = 0x31B402,
    FsError_UnsupportedOperateRangeForAesXtsFile = 0x31B602,
    FsError_UnsupportedWriteForRomFsFileSystem = 0x31B802,
    FsError_UnsupportedCommitProvisionallyForRomFsFileSystem = 0x31BA02,
    FsError_UnsupportedGetTotalSpaceSizeForRomFsFileSystem = 0x31BC02,
    FsError_UnsupportedWriteForRomFsFile = 0x31BE02,
    FsError_UnsupportedOperateRangeForRomFsFile = 0x31C002,
    FsError_UnsupportedWriteForReadOnlyFileSystem = 0x31C202,
    FsError_UnsupportedCommitProvisionallyForReadOnlyFileSystem = 0x31C402,
    FsError_UnsupportedGetTotalSpaceSizeForReadOnlyFileSystem = 0x31C602,
    FsError_UnsupportedWriteForReadOnlyFile = 0x31C802,
    FsError_UnsupportedOperateRangeForReadOnlyFile = 0x31CA02,
    FsError_UnsupportedWriteForPartitionFileSystem = 0x31CC02,
    FsError_UnsupportedCommitProvisionallyForPartitionFileSystem = 0x31CE02,
    FsError_UnsupportedWriteForPartitionFile = 0x31D002,
    FsError_UnsupportedOperateRangeForPartitionFile = 0x31D202,
    FsError_UnsupportedOperateRangeForTmFileSystemFile = 0x31D402,
    FsError_UnsupportedWriteForSaveDataInternalStorageFileSystem = 0x31D602,
    FsError_UnsupportedCommitProvisionallyForApplicationTemporaryFileSystem = 0x31DC02,
    FsError_UnsupportedCommitProvisionallyForSaveDataFileSystem = 0x31DE02,
    FsError_UnsupportedCommitProvisionallyForDirectorySaveDataFileSystem = 0x31E002,
    FsError_UnsupportedWriteForZeroBitmapHashStorageFile = 0x31E202,
    FsError_UnsupportedSetSizeForZeroBitmapHashStorageFile = 0x31E402,
    FsError_NcaExternalKeyUnregisteredDeprecated = 0x326602,
    FsError_FileNotClosed = 0x326E02,
    FsError_DirectoryNotClosed = 0x327002,
    FsError_WriteModeFileNotClosed = 0x327202,
    FsError_AllocatorAlreadyRegistered = 0x327402,
    FsError_DefaultAllocatorAlreadyUsed = 0x327602,
    FsError_AllocatorAlignmentViolation = 0x327A02,
    FsError_UserNotExist = 0x328202,
    FsError_FileNotFound = 0x339402,
    FsError_DirectoryNotFound = 0x339602,
    FsError_MappingTableFull = 0x346402,
    FsError_OpenCountLimit = 0x346A02,
    FsError_MultiCommitFileSystemLimit = 0x346C02,
    FsError_MapFull = 0x353602,
    FsError_NotMounted = 0x35F202,
    FsError_DbmKeyNotFound = 0x3DBC02,
    FsError_DbmFileNotFound = 0x3DBE02,
    FsError_DbmDirectoryNotFound = 0x3DC002,
    FsError_DbmAlreadyExists = 0x3DC402,
    FsError_DbmKeyFull = 0x3DC602,
    FsError_DbmDirectoryEntryFull = 0x3DC802,
    FsError_DbmFileEntryFull = 0x3DCA02,
    FsError_DbmInvalidOperation = 0x3DD402,
};

int vfs_fs_set_errno(Result rc) {
    switch (rc) {
        case FsError_TargetLocked: errno = EBUSY; break;
        case FsError_PathNotFound: errno = ENOENT; break;
        case FsError_PathAlreadyExists: errno = EEXIST; break;
        case FsError_UsableSpaceNotEnoughMmcCalibration: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcSafe: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcUser: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughMmcSystem: errno = ENOSPC; break;
        case FsError_UsableSpaceNotEnoughSdCard: errno = ENOSPC; break;
        case FsError_OutOfRange: errno = ESPIPE; break;
        case FsError_TooLongPath: errno = ENAMETOOLONG; break;
        case FsError_UnsupportedWriteForReadOnlyFileSystem: errno = EROFS; break;
        default: errno = EIO; break;
    }
    return -1;
}

#if VFS_NX_BUFFER_IO
static Result flush_buffered_write(struct VfsFsFile* f) {
    Result rc;
    if (R_SUCCEEDED(rc = fsFileSetSize(&f->fd, f->off + f->buf_off))) {
        rc = fsFileWrite(&f->fd, f->off, f->buf, f->buf_off, FsWriteOption_None);
    }
    return rc;
}
#endif

static int fstat_internal(FsFileSystem* fs, FsFile* file, const char nxpath[FS_MAX_PATH], struct stat* st) {
    memset(st, 0, sizeof(*st));

    Result rc;
    s64 size;
    if (R_FAILED(rc = fsFileGetSize(file, &size))) {
        return vfs_fs_set_errno(rc);
    }

    st->st_nlink = 1;
    st->st_size = size;
    st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    FsTimeStampRaw timestamp;
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fs, nxpath, &timestamp))) {
        if (timestamp.is_valid) {
            st->st_ctime = timestamp.created;
            st->st_mtime = timestamp.modified;
            st->st_atime = timestamp.accessed;
        }
    }

    return 0;
}

int vfs_fs_internal_open(FsFileSystem* fs, struct VfsFsFile* f, const char nxpath[FS_MAX_PATH], enum FtpVfsOpenMode mode) {
    u32 open_mode;
    if (mode == FtpVfsOpenMode_READ) {
        open_mode = FsOpenMode_Read;
        f->is_write = false;
    } else {
        fsFsCreateFile(fs, nxpath, 0, 0);
        open_mode = FsOpenMode_Write;
        f->is_write = true;
    }

    Result rc;
    if (R_FAILED(rc = fsFsOpenFile(fs, nxpath, open_mode, &f->fd))) {
        return vfs_fs_set_errno(rc);
    }

    f->off = f->chunk_size = 0;
#if VFS_NX_BUFFER_IO
    f->buf_off = f->buf_size = 0;
#endif

    if (mode == FtpVfsOpenMode_WRITE) {
        if (R_FAILED(rc = fsFileSetSize(&f->fd, 0))) {
            goto fail_close;
        }
    } else if (mode == FtpVfsOpenMode_APPEND) {
        if (R_FAILED(rc = fsFileGetSize(&f->fd, &f->off))) {
            goto fail_close;
        }
        f->chunk_size = f->off;
    }

    f->is_valid = true;
    return 0;

fail_close:
    fsFileClose(&f->fd);
    return vfs_fs_set_errno(rc);
}

int vfs_fs_internal_read(struct VfsFsFile* f, void* buf, size_t size) {
    Result rc;

#if VFS_NX_BUFFER_IO
    if (f->buf_off == f->buf_size) {
        u64 bytes_read;
        if (R_FAILED(rc = fsFileRead(&f->fd, f->off, f->buf, sizeof(f->buf), FsReadOption_None, &bytes_read))) {
            return vfs_fs_set_errno(rc);
        }

        f->buf_off = 0;
        f->buf_size = bytes_read;
    }

    if (!f->buf_size) {
        return 0;
    }

    size = size < f->buf_size - f->buf_off ? size : f->buf_size - f->buf_off;
    memcpy(buf, f->buf + f->buf_off, size);
    f->off += size;
    f->buf_off += size;
    return size;
#else
    u64 bytes_read;
    if (R_FAILED(rc = fsFileRead(&f->fd, f->off, buf, size, FsReadOption_None, &bytes_read))) {
        return vfs_fs_set_errno(rc);
    }
    f->off += bytes_read;
    return bytes_read;
#endif
}

int vfs_fs_internal_write(struct VfsFsFile* f, const void* buf, size_t size) {
    Result rc;

#if VFS_NX_BUFFER_IO
    const size_t ret = size;
    while (size) {
        if (f->buf_off + size > sizeof(f->buf)) {
            const u64 sz = sizeof(f->buf) - f->buf_off;
            memcpy(f->buf + f->buf_off, buf, sz);
            f->buf_off += sz;

            if (R_FAILED(rc = flush_buffered_write(f))) {
                return vfs_fs_set_errno(rc);
            }

            buf += sz;
            size -= sz;
            f->off += f->buf_off;
            f->buf_off = 0;
        } else {
            memcpy(f->buf + f->buf_off, buf, size);
            f->buf_off += size;
            size = 0;
        }
    }

    return ret;
#else
    if (f->chunk_size < f->off + size) {
        f->chunk_size += NX_WRITE_CHUNK_SIZE;
        if (R_FAILED(rc = fsFileSetSize(&f->fd, f->chunk_size))) {
            return vfs_fs_set_errno(rc);
        }
    }

    if (R_FAILED(rc = fsFileWrite(&f->fd, f->off, buf, size, FsWriteOption_None))) {
        return vfs_fs_set_errno(rc);
    }

    f->off += size;
    return size;
#endif
}

int vfs_fs_internal_seek(struct VfsFsFile* f, size_t off) {
#if VFS_NX_BUFFER_IO
    if (!f->is_write) {
        f->buf_off -= f->off - off;
    }
#endif
    f->off = off;
    return 0;
}

int vfs_fs_internal_close(struct VfsFsFile* f) {
    if (!vfs_fs_internal_isfile_open(f)) {
        return -1;
    }

#if VFS_NX_BUFFER_IO
    if (f->buf_off) {
        flush_buffered_write(f);
    }
#else
    if (f->chunk_size) {
        fsFileSetSize(&f->fd, f->off); // shrink
    }
#endif

    fsFileClose(&f->fd);
    f->is_valid = false;
    return 0;
}

int vfs_fs_internal_isfile_open(struct VfsFsFile* f) {
    return f->is_valid;
}

int vfs_fs_internal_opendir(FsFileSystem* fs, struct VfsFsDir* f, const char nxpath[FS_MAX_PATH]) {
    Result rc;
    if (R_FAILED(rc = fsFsOpenDirectory(fs, nxpath, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &f->dir))) {
        return vfs_fs_set_errno(rc);
    }

    f->is_valid = 1;
    return 0;
}

const char* vfs_fs_internal_readdir(struct VfsFsDir* f, struct VfsFsDirEntry* entry) {
    Result rc;
    s64 total_entries;
    if (R_FAILED(rc = fsDirRead(&f->dir, &total_entries, 1, &entry->buf))) {
        vfs_fs_set_errno(rc);
        return NULL;
    }

    if (total_entries <= 0) {
        return NULL;
    }

    return entry->buf.name;
}

int vfs_fs_internal_dirlstat(FsFileSystem* fs, struct VfsFsDir* f, const struct VfsFsDirEntry* entry, const char nxpath[FS_MAX_PATH], struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (entry->buf.type == FsDirEntryType_File) {
        st->st_size = entry->buf.file_size;
        st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        FsTimeStampRaw timestamp;
        if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fs, nxpath, &timestamp))) {
            if (timestamp.is_valid) {
                st->st_ctime = timestamp.created;
                st->st_mtime = timestamp.modified;
                st->st_atime = timestamp.accessed;
            }
        }
    } else {
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    }

    return 0;
}

int vfs_fs_internal_closedir(struct VfsFsDir* f) {
    if (!vfs_fs_internal_isdir_open(f)) {
        return -1;
    }

    fsDirClose(&f->dir);
    f->is_valid = false;
    return 0;
}

int vfs_fs_internal_isdir_open(struct VfsFsDir* f) {
    return f->is_valid;
}

int vfs_fs_internal_stat(FsFileSystem* fs, const char nxpath[FS_MAX_PATH], struct stat* st) {
    memset(st, 0, sizeof(*st));

    Result rc;
    FsDirEntryType type;
    if (R_FAILED(rc = fsFsGetEntryType(fs, nxpath, &type))) {
        return vfs_fs_set_errno(rc);
    }

    if (type == FsDirEntryType_File) {
        FsFile file;
        if (R_FAILED(rc = fsFsOpenFile(fs, nxpath, FsOpenMode_Read, &file))) {
            return vfs_fs_set_errno(rc);
        }

        const int rci = fstat_internal(fs, &file, nxpath, st);
        fsFileClose(&file);
        return rci;
    } else {
        st->st_nlink = 1;
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    }

    return 0;
}

int vfs_fs_internal_lstat(FsFileSystem* fs, const char nxpath[FS_MAX_PATH], struct stat* st) {
    return vfs_fs_internal_stat(fs, nxpath, st);
}

int vfs_fs_internal_mkdir(FsFileSystem* fs, const char nxpath[FS_MAX_PATH]) {
    Result rc;
    if (R_FAILED(rc = fsFsCreateDirectory(fs, nxpath))) {
        return vfs_fs_set_errno(rc);
    }

    return 0;
}

int vfs_fs_internal_unlink(FsFileSystem* fs, const char nxpath[FS_MAX_PATH]) {
    Result rc;
    if (R_FAILED(rc = fsFsDeleteFile(fs, nxpath))) {
        return vfs_fs_set_errno(rc);
    }

    return 0;
}

int vfs_fs_internal_rmdir(FsFileSystem* fs, const char nxpath[FS_MAX_PATH]) {
    Result rc;
    if (R_FAILED(rc = fsFsDeleteDirectory(fs, nxpath))) {
        return vfs_fs_set_errno(rc);
    }

    return 0;
}

int vfs_fs_internal_rename(FsFileSystem* fs, const char nxpath_src[FS_MAX_PATH], const char nxpath_dst[FS_MAX_PATH]) {
    Result rc;
    FsDirEntryType type;
    if (R_FAILED(rc = fsFsGetEntryType(fs, nxpath_src, &type))) {
        return vfs_fs_set_errno(rc);
    }

    if (type == FsDirEntryType_File) {
        if (R_FAILED(rc = fsFsRenameFile(fs, nxpath_src, nxpath_dst))) {
            return vfs_fs_set_errno(rc);
        }
    } else {
        if (R_FAILED(rc = fsFsRenameDirectory(fs, nxpath_src, nxpath_dst))) {
            return vfs_fs_set_errno(rc);
        }
    }

    return 0;
}

////////////////////

static int vfs_fs_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    struct VfsFsFile* f = user;
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_open(fs, f, nxpath, mode);
}

static int vfs_fs_read(void* user, void* buf, size_t size) {
    struct VfsFsFile* f = user;
    return vfs_fs_internal_read(f, buf, size);
}

static int vfs_fs_write(void* user, const void* buf, size_t size) {
    struct VfsFsFile* f = user;
    return vfs_fs_internal_write(f, buf, size);
}

static int vfs_fs_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsFsFile* f = user;
    return vfs_fs_internal_seek(f, off);
}

static int vfs_fs_isfile_open(void* user) {
    struct VfsFsFile* f = user;
    return vfs_fs_internal_isfile_open(f);
}

static int vfs_fs_close(void* user) {
    struct VfsFsFile* f = user;
    if (!vfs_fs_isfile_open(f)) {
        return -1;
    }
    return vfs_fs_internal_close(f);
}

static int vfs_fs_opendir(void* user, const char* path) {
    struct VfsFsDir* f = user;
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_opendir(fs, f, nxpath);
}

const char* vfs_fs_readdir(void* user, void* user_entry) {
    struct VfsFsDir* f = user;
    struct VfsFsDirEntry* entry = user_entry;
    return vfs_fs_internal_readdir(f, entry);
}

static int vfs_fs_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    struct VfsFsDir* f = user;
    const struct VfsFsDirEntry* entry = user_entry;
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_dirlstat(fs, f, entry, nxpath, st);
}

static int vfs_fs_isdir_open(void* user) {
    struct VfsFsDir* f = user;
    return vfs_fs_internal_isdir_open(f);
}

static int vfs_fs_closedir(void* user) {
    struct VfsFsDir* f = user;
    return vfs_fs_internal_closedir(f);
}

static int vfs_fs_stat(const char* path, struct stat* st) {
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_stat(fs, nxpath, st);
}

static int vfs_fs_mkdir(const char* path) {
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_mkdir(fs, nxpath);
}

static int vfs_fs_unlink(const char* path) {
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_unlink(fs, nxpath);
}

static int vfs_fs_rmdir(const char* path) {
    FsFileSystem* fs = NULL;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return -1;
    }
    return vfs_fs_internal_rmdir(fs, nxpath);
}

static int vfs_fs_rename(const char* src, const char* dst) {
    FsFileSystem* fs = NULL;
    FsFileSystem* fs_dst = NULL;
    char nxpath_src[FS_MAX_PATH];
    char nxpath_dst[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(src, &fs, nxpath_src) || fsdev_wrapTranslatePath(dst, &fs_dst, nxpath_dst)) {
        return -1;
    }

    // cannot rename across different fs.
    if (fs != fs_dst) {
        errno = EXDEV; // this will do for the error.
        return -1;
    }

    return vfs_fs_internal_rename(fs, nxpath_src, nxpath_dst);
}

const FtpVfs g_vfs_fs = {
    .open = vfs_fs_open,
    .read = vfs_fs_read,
    .write = vfs_fs_write,
    .seek = vfs_fs_seek,
    .close = vfs_fs_close,
    .isfile_open = vfs_fs_isfile_open,
    .opendir = vfs_fs_opendir,
    .readdir = vfs_fs_readdir,
    .dirlstat = vfs_fs_dirlstat,
    .closedir = vfs_fs_closedir,
    .isdir_open = vfs_fs_isdir_open,
    .stat = vfs_fs_stat,
    .lstat = vfs_fs_stat,
    .mkdir = vfs_fs_mkdir,
    .unlink = vfs_fs_unlink,
    .rmdir = vfs_fs_rmdir,
    .rename = vfs_fs_rename,
};
