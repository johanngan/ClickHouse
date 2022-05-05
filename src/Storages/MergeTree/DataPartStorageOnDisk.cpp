#include <Storages/MergeTree/DataPartStorageOnDisk.h>
#include <Storages/MergeTree/MergeTreeDataPartChecksum.h>
#include <Disks/IVolume.h>
#include <Disks/TemporaryFileOnDisk.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <Common/logger_useful.h>
#include <Disks/IStoragePolicy.h>
#include <Backups/BackupEntryFromSmallFile.h>
#include <Backups/BackupEntryFromImmutableFile.h>
#include <Storages/MergeTree/localBackup.h>
#include <Disks/SingleDiskVolume.h>
#include <Interpreters/TransactionVersionMetadata.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int DIRECTORY_ALREADY_EXISTS;
    extern const int NOT_ENOUGH_SPACE;
}

DataPartStorageOnDisk::DataPartStorageOnDisk(VolumePtr volume_, std::string root_path_, std::string part_dir_)
    : volume(std::move(volume_)), root_path(std::move(root_path_)), part_dir(std::move(part_dir_))
{
}

void DataPartStorageOnDisk::setRelativePath(const std::string & path)
{
    part_dir = path;
}

std::string DataPartStorageOnDisk::getFullRelativePath() const
{
    return fs::path(root_path) / part_dir / "";
}

std::string DataPartStorageOnDisk::getFullPath() const
{
    return fs::path(volume->getDisk()->getPath()) / root_path / part_dir / "";
}

std::string DataPartStorageOnDisk::getFullRootPath() const
{
    return fs::path(volume->getDisk()->getPath()) / root_path / "";
}

std::string DataPartStorageOnDisk::getRelativePathForPrefix(Poco::Logger * log, const String & prefix, bool detached) const
{
    String res;

    auto full_relative_path = fs::path(root_path);
    if (detached)
        full_relative_path /= "detached";

    for (int try_no = 0; try_no < 10; ++try_no)
    {
        res = (prefix.empty() ? "" : prefix + "_") + part_dir + (try_no ? "_try" + DB::toString(try_no) : "");

        if (!volume->getDisk()->exists(full_relative_path / res))
            return res;

        LOG_WARNING(log, "Directory {} (to detach to) already exists. Will detach to directory with '_tryN' suffix.", res);
    }

    return res;
}

std::unique_ptr<ReadBufferFromFileBase> DataPartStorageOnDisk::readFile(
    const std::string & path,
    const ReadSettings & settings,
    std::optional<size_t> read_hint,
    std::optional<size_t> file_size) const
{
    return volume->getDisk()->readFile(fs::path(root_path) / part_dir / path, settings, read_hint, file_size);
}

bool DataPartStorageOnDisk::exists(const std::string & path) const
{
    return volume->getDisk()->exists(fs::path(root_path) / part_dir / path);
}

bool DataPartStorageOnDisk::isDirectory(const std::string & path) const
{
    return volume->getDisk()->isDirectory(fs::path(root_path) / part_dir / path);
}

bool DataPartStorageOnDisk::exists() const
{
    return volume->getDisk()->exists(fs::path(root_path) / part_dir);
}

Poco::Timestamp DataPartStorageOnDisk::getLastModified() const
{
    return volume->getDisk()->getLastModified(fs::path(root_path) / part_dir);
}

size_t DataPartStorageOnDisk::getFileSize(const String & path) const
{
    return volume->getDisk()->getFileSize(fs::path(root_path) / part_dir / path);
}

UInt32 DataPartStorageOnDisk::getRefCount(const String & path) const
{
    return volume->getDisk()->getRefCount(fs::path(root_path) / part_dir / path);
}

class DataPartStorageIteratorOnDisk final : public IDataPartStorageIterator
{
public:
    DataPartStorageIteratorOnDisk(DiskPtr disk_, DiskDirectoryIteratorPtr it_)
        : disk(std::move(disk_)), it(std::move(it_))
    {
    }

    void next() override { it->next(); }
    bool isValid() const override { return it->isValid(); }
    bool isFile() const override { return isValid() && disk->isFile(it->path()); }
    std::string name() const override { return it->name(); }

private:
    DiskPtr disk;
    DiskDirectoryIteratorPtr it;
};

DataPartStorageIteratorPtr DataPartStorageOnDisk::iterate() const
{
    return std::make_unique<DataPartStorageIteratorOnDisk>(
        volume->getDisk(),
        volume->getDisk()->iterateDirectory(fs::path(root_path) / part_dir));
}

DataPartStorageIteratorPtr DataPartStorageOnDisk::iterateDirectory(const String & path) const
{
    return std::make_unique<DataPartStorageIteratorOnDisk>(
        volume->getDisk(),
        volume->getDisk()->iterateDirectory(fs::path(root_path) / part_dir / path));
}

void DataPartStorageOnDisk::remove(
    bool can_remove_shared_data,
    const NameSet & names_not_to_remove,
    const MergeTreeDataPartChecksums & checksums,
    std::list<ProjectionChecksums> projections,
    Poco::Logger * log) const
{
    /// NOTE We rename part to delete_tmp_<relative_path> instead of delete_tmp_<name> to avoid race condition
    /// when we try to remove two parts with the same name, but different relative paths,
    /// for example all_1_2_1 (in Deleting state) and tmp_merge_all_1_2_1 (in Temporary state).
    fs::path from = fs::path(root_path) / part_dir;
    fs::path to = fs::path(root_path) / ("delete_tmp_" + part_dir);
    // TODO directory delete_tmp_<name> is never removed if server crashes before returning from this function

    auto disk = volume->getDisk();
    if (disk->exists(to))
    {
        LOG_WARNING(log, "Directory {} (to which part must be renamed before removing) already exists. Most likely this is due to unclean restart or race condition. Removing it.", fullPath(disk, to));
        try
        {
            disk->removeSharedRecursive(fs::path(to) / "", !can_remove_shared_data, names_not_to_remove);
        }
        catch (...)
        {
            LOG_ERROR(log, "Cannot recursively remove directory {}. Exception: {}", fullPath(disk, to), getCurrentExceptionMessage(false));
            throw;
        }
    }

    try
    {
        disk->moveDirectory(from, to);
    }
    catch (const fs::filesystem_error & e)
    {
        if (e.code() == std::errc::no_such_file_or_directory)
        {
            LOG_ERROR(log, "Directory {} (part to remove) doesn't exist or one of nested files has gone. Most likely this is due to manual removing. This should be discouraged. Ignoring.", fullPath(disk, to));
            return;
        }
        throw;
    }

    // Record existing projection directories so we don't remove them twice
    std::unordered_set<String> projection_directories;
    for (const auto & projection : projections)
    {
        std::string proj_dir_name = projection.name + ".proj";
        projection_directories.emplace(proj_dir_name);

        clearDirectory(
            fs::path(to) / proj_dir_name,
            can_remove_shared_data, names_not_to_remove, projection.checksums, {}, log, true);
    }

    clearDirectory(to, can_remove_shared_data, names_not_to_remove, checksums, projection_directories, log, false);
}

void DataPartStorageOnDisk::clearDirectory(
    const std::string & dir,
    bool can_remove_shared_data,
    const NameSet & names_not_to_remove,
    const MergeTreeDataPartChecksums & checksums,
    const std::unordered_set<String> & skip_directories,
    Poco::Logger * log,
    bool is_projection) const
{
    auto disk = volume->getDisk();

    if (checksums.empty())
    {
        if (is_projection)
        {
            LOG_ERROR(
                log,
                "Cannot quickly remove directory {} by removing files; fallback to recursive removal. Reason: checksums.txt is missing",
                fullPath(disk, dir));
        }

        /// If the part is not completely written, we cannot use fast path by listing files.
        disk->removeSharedRecursive(fs::path(dir) / "", !can_remove_shared_data, names_not_to_remove);

        return;
    }

    try
    {
        /// Remove each expected file in directory, then remove directory itself.
        IDisk::RemoveBatchRequest request;

#if !defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-variable"
#endif
        for (const auto & [file, _] : checksums.files)
        {
            if (skip_directories.find(file) == skip_directories.end())
                request.emplace_back(fs::path(dir) / file);
        }
#if !defined(__clang__)
#    pragma GCC diagnostic pop
#endif

        for (const auto & file : {"checksums.txt", "columns.txt"})
            request.emplace_back(fs::path(dir) / file);

        request.emplace_back(fs::path(dir) / "default_compression_codec.txt", true);
        request.emplace_back(fs::path(dir) / "delete-on-destroy.txt", true);

        if (!is_projection)
            request.emplace_back(fs::path(dir) / "txn_version.txt", true);

        disk->removeSharedFiles(request, !can_remove_shared_data, names_not_to_remove);
        disk->removeDirectory(dir);
    }
    catch (...)
    {
        /// Recursive directory removal does many excessive "stat" syscalls under the hood.

        LOG_ERROR(log, "Cannot quickly remove directory {} by removing files; fallback to recursive removal. Reason: {}", fullPath(disk, dir), getCurrentExceptionMessage(false));

        disk->removeSharedRecursive(fs::path(dir) / "", !can_remove_shared_data, names_not_to_remove);
    }
}

DataPartStorageOnDisk::DisksSet::const_iterator DataPartStorageOnDisk::isStoredOnDisk(const DisksSet & disks) const
{
    return disks.find(volume->getDisk());
}

DataPartStoragePtr DataPartStorageOnDisk::getProjection(const std::string & name) const
{
    return std::make_shared<DataPartStorageOnDisk>(volume, std::string(fs::path(root_path) / part_dir), name);
}

DiskPtr DataPartStorageOnDisk::getDisk() const
{
    return volume->getDisk();
}

static UInt64 calculateTotalSizeOnDiskImpl(const DiskPtr & disk, const String & from)
{
    if (disk->isFile(from))
        return disk->getFileSize(from);
    std::vector<std::string> files;
    disk->listFiles(from, files);
    UInt64 res = 0;
    for (const auto & file : files)
        res += calculateTotalSizeOnDiskImpl(disk, fs::path(from) / file);
    return res;
}

UInt64 DataPartStorageOnDisk::calculateTotalSizeOnDisk() const
{
    return calculateTotalSizeOnDiskImpl(volume->getDisk(), fs::path(root_path) / part_dir);
}

bool DataPartStorageOnDisk::isStoredOnRemoteDisk() const
{
    return volume->getDisk()->isRemote();
}

bool DataPartStorageOnDisk::supportZeroCopyReplication() const
{
    return volume->getDisk()->supportZeroCopyReplication();
}

bool DataPartStorageOnDisk::supportParallelWrite() const
{
    return volume->getDisk()->supportParallelWrite();
}

bool DataPartStorageOnDisk::isBroken() const
{
    return volume->getDisk()->isBroken();
}

std::string DataPartStorageOnDisk::getDiskPathForLogs() const
{
    return volume->getDisk()->getPath();
}

void DataPartStorageOnDisk::writeChecksums(const MergeTreeDataPartChecksums & checksums, const WriteSettings & settings) const
{
    std::string path = fs::path(root_path) / part_dir / "checksums.txt";

    try
    {
        {
            auto out = volume->getDisk()->writeFile(path + ".tmp", 4096, WriteMode::Rewrite, settings);
            checksums.write(*out);
        }

        volume->getDisk()->moveFile(path + ".tmp", path);
    }
    catch (...)
    {
        try
        {
            if (volume->getDisk()->exists(path + ".tmp"))
                volume->getDisk()->removeFile(path + ".tmp");
        }
        catch (...)
        {
            tryLogCurrentException("DataPartStorageOnDisk");
        }

        throw;
    }
}

void DataPartStorageOnDisk::writeColumns(const NamesAndTypesList & columns, const WriteSettings & settings) const
{
    std::string path = fs::path(root_path) / part_dir / "columns.txt";

    try
    {
        {
            auto buf = volume->getDisk()->writeFile(path + ".tmp", 4096, WriteMode::Rewrite, settings);
            columns.writeText(*buf);
        }

        volume->getDisk()->moveFile(path + ".tmp", path);
    }
    catch (...)
    {
        try
        {
            if (volume->getDisk()->exists(path + ".tmp"))
                volume->getDisk()->removeFile(path + ".tmp");
        }
        catch (...)
        {
            tryLogCurrentException("DataPartStorageOnDisk");
        }

        throw;
    }
}

void DataPartStorageOnDisk::writeVersionMetadata(const VersionMetadata & version, bool fsync_part_dir) const
{
    std::string path = fs::path(root_path) / part_dir / "txn_version.txt";
    try
    {
        {
            /// TODO IDisk interface does not allow to open file with O_EXCL flag (for DiskLocal),
            /// so we create empty file at first (expecting that createFile throws if file already exists)
            /// and then overwrite it.
            auto buf = volume->getDisk()->writeFile(path + ".tmp", 4096);
            version.write(*buf);
            buf->finalize();
            buf->sync();
        }

        SyncGuardPtr sync_guard;
        if (fsync_part_dir)
            sync_guard = volume->getDisk()->getDirectorySyncGuard(getFullRelativePath());
        volume->getDisk()->replaceFile(path + ".tmp", path);

    }
    catch (...)
    {
        try
        {
            if (volume->getDisk()->exists(path + ".tmp"))
                volume->getDisk()->removeFile(path + ".tmp");
        }
        catch (...)
        {
            tryLogCurrentException("DataPartStorageOnDisk");
        }

        throw;
    }
}

void DataPartStorageOnDisk::appendCSNToVersionMetadata(const VersionMetadata & version, VersionMetadata::WhichCSN which_csn) const
{
    /// Small enough appends to file are usually atomic,
    /// so we append new metadata instead of rewriting file to reduce number of fsyncs.
    /// We don't need to do fsync when writing CSN, because in case of hard restart
    /// we will be able to restore CSN from transaction log in Keeper.

    std::string version_file_name = fs::path(root_path) / part_dir / "txn_version.txt";
    DiskPtr disk = volume->getDisk();
    auto out = disk->writeFile(version_file_name, 256, WriteMode::Append);
    version.writeCSN(*out, which_csn);
    out->finalize();
}

void DataPartStorageOnDisk::appendRemovalTIDToVersionMetadata(const VersionMetadata & version, bool clear) const
{
    String version_file_name = fs::path(root_path) / part_dir / "txn_version.txt";
    DiskPtr disk = volume->getDisk();
    auto out = disk->writeFile(version_file_name, 256, WriteMode::Append);
    version.writeRemovalTID(*out, clear);
    out->finalize();

    /// fsync is not required when we clearing removal TID, because after hard restart we will fix metadata
    if (!clear)
        out->sync();
}

static std::unique_ptr<ReadBufferFromFileBase> openForReading(const DiskPtr & disk, const String & path)
{
    size_t file_size = disk->getFileSize(path);
    return disk->readFile(path, ReadSettings().adjustBufferSize(file_size), file_size);
}

void DataPartStorageOnDisk::loadVersionMetadata(VersionMetadata & version, Poco::Logger * log) const
{
    std::string version_file_name = fs::path(root_path) / part_dir / "txn_version.txt";
    String tmp_version_file_name = version_file_name + ".tmp";
    DiskPtr disk = volume->getDisk();

    auto remove_tmp_file = [&]()
    {
        auto last_modified = disk->getLastModified(tmp_version_file_name);
        auto buf = openForReading(disk, tmp_version_file_name);
        String content;
        readStringUntilEOF(content, *buf);
        LOG_WARNING(log, "Found file {} that was last modified on {}, has size {} and the following content: {}",
                    tmp_version_file_name, last_modified.epochTime(), content.size(), content);
        disk->removeFile(tmp_version_file_name);
    };

    if (disk->exists(version_file_name))
    {
        auto buf = openForReading(disk, version_file_name);
        version.read(*buf);
        if (disk->exists(tmp_version_file_name))
            remove_tmp_file();
        return;
    }

    /// Four (?) cases are possible:
    /// 1. Part was created without transactions.
    /// 2. Version metadata file was not renamed from *.tmp on part creation.
    /// 3. Version metadata were written to *.tmp file, but hard restart happened before fsync.
    /// 4. Fsyncs in storeVersionMetadata() work incorrectly.

    if (!disk->exists(tmp_version_file_name))
    {
        /// Case 1.
        /// We do not have version metadata and transactions history for old parts,
        /// so let's consider that such parts were created by some ancient transaction
        /// and were committed with some prehistoric CSN.
        /// NOTE It might be Case 3, but version metadata file is written on part creation before other files,
        /// so it's not Case 3 if part is not broken.
        version.setCreationTID(Tx::PrehistoricTID, nullptr);
        version.creation_csn = Tx::PrehistoricCSN;
        return;
    }

    /// Case 2.
    /// Content of *.tmp file may be broken, just use fake TID.
    /// Transaction was not committed if *.tmp file was not renamed, so we should complete rollback by removing part.
    version.setCreationTID(Tx::DummyTID, nullptr);
    version.creation_csn = Tx::RolledBackCSN;
    remove_tmp_file();
}

void DataPartStorageOnDisk::writeDeleteOnDestroyMarker(Poco::Logger * log) const
{
    String marker_path = fs::path(root_path) / part_dir / "delete-on-destroy.txt";
    auto disk = volume->getDisk();
    try
    {
        volume->getDisk()->createFile(marker_path);
    }
    catch (Poco::Exception & e)
    {
        LOG_ERROR(log, "{} (while creating DeleteOnDestroy marker: {})", e.what(), backQuote(fullPath(disk, marker_path)));
    }
}

void DataPartStorageOnDisk::removeDeleteOnDestroyMarker() const
{
    std::string delete_on_destroy_file_name = fs::path(root_path) / part_dir / "delete-on-destroy.txt";
    volume->getDisk()->removeFileIfExists(delete_on_destroy_file_name);
}

void DataPartStorageOnDisk::removeVersionMetadata() const
{
    std::string version_file_name = fs::path(root_path) / part_dir / "txn_version.txt";
    volume->getDisk()->removeFileIfExists(version_file_name);
}

void DataPartStorageOnDisk::checkConsistency(const MergeTreeDataPartChecksums & checksums) const
{
    checksums.checkSizes(volume->getDisk(), getFullRelativePath());
}

ReservationPtr DataPartStorageOnDisk::reserve(UInt64 bytes) const
{
    auto res = volume->reserve(bytes);
    if (!res)
        throw Exception(ErrorCodes::NOT_ENOUGH_SPACE, "Cannot reserve {}, not enough space", ReadableSize(bytes));

    return res;
}

ReservationPtr DataPartStorageOnDisk::tryReserve(UInt64 bytes) const
{
    return volume->reserve(bytes);
}

void DataPartStorageOnDisk::rename(const String & new_relative_path, Poco::Logger * log, bool remove_new_dir_if_exists, bool fsync_part_dir)
{
    if (!exists())
        throw Exception(
            ErrorCodes::FILE_DOESNT_EXIST,
            "Part directory {} doesn't exist. Most likely it is a logical error.",
            std::string(fs::path(volume->getDisk()->getPath()) / root_path / part_dir));

    auto new_path = fs::path(root_path) / new_relative_path;
    if (!new_path.has_filename())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Cannot rename from {} to {}. Destination should not contain trailing slash",
            getFullRelativePath(),
            new_relative_path);

    /// Why "" ?
    String to = fs::path(root_path) / new_relative_path / "";

    if (volume->getDisk()->exists(to))
    {
        if (remove_new_dir_if_exists)
        {
            Names files;
            volume->getDisk()->listFiles(to, files);

            LOG_WARNING(log,
                "Part directory {} already exists and contains {} files. Removing it.",
                fullPath(volume->getDisk(), to), files.size());

            volume->getDisk()->removeRecursive(to);
        }
        else
        {
            throw Exception(
                ErrorCodes::DIRECTORY_ALREADY_EXISTS,
                "Part directory {} already exists",
                fullPath(volume->getDisk(), to));
        }
    }

    // metadata_manager->deleteAll(true);
    // metadata_manager->assertAllDeleted(true);

    String from = getFullRelativePath();

    /// Why?
    volume->getDisk()->setLastModified(from, Poco::Timestamp::fromEpochTime(time(nullptr)));
    volume->getDisk()->moveDirectory(from, to);
    part_dir = new_path.filename();
    root_path = new_path.remove_filename();
    // metadata_manager->updateAll(true);

    SyncGuardPtr sync_guard;
    if (fsync_part_dir)
        sync_guard = volume->getDisk()->getDirectorySyncGuard(getFullRelativePath());
}

void DataPartStorageOnDisk::changeRootPath(const std::string & from_root, const std::string & to_root)
{
    /// This is a very dumb implementation, here for root path like
    /// "some/current/path/to/part" and change like
    /// "some/current" -> "other/different", we just replace prefix to make new root like
    /// "other/different/path/to/part".
    /// Here we expect that actual move was done by somebody else.

    size_t prefix_size = from_root.size();
    if (prefix_size > 0 && from_root.back() == '/')
        --prefix_size;

    if (prefix_size > root_path.size()
        || std::string_view(from_root).substr(0, prefix_size) !=  std::string_view(root_path).substr(0, prefix_size))
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Cannot change part root to {} because it is not a prefix of current root {}",
            from_root, root_path);

    size_t dst_size = to_root.size();
    if (dst_size > 0 && to_root.back() == '/')
        --dst_size;

    root_path = to_root.substr(0, dst_size) + root_path.substr(prefix_size);
}

bool DataPartStorageOnDisk::shallParticipateInMerges(const IStoragePolicy & storage_policy) const
{
    /// `IMergeTreeDataPart::volume` describes space where current part belongs, and holds
    /// `SingleDiskVolume` object which does not contain up-to-date settings of corresponding volume.
    /// Therefore we shall obtain volume from storage policy.
    auto volume_ptr = storage_policy.getVolume(storage_policy.getVolumeIndexByDisk(volume->getDisk()));

    return !volume_ptr->areMergesAvoided();
}

size_t DataPartStorageOnDisk::getVolumeIndex(const IStoragePolicy & storage_policy) const
{
    return storage_policy.getVolumeIndexByDisk(volume->getDisk());
}

String DataPartStorageOnDisk::getUniqueId() const
{
    auto disk = volume->getDisk();
    if (!disk->supportZeroCopyReplication())
        throw Exception(fmt::format("Disk {} doesn't support zero-copy replication", disk->getName()), ErrorCodes::LOGICAL_ERROR);

    return disk->getUniqueId(fs::path(getFullRelativePath()) / "checksums.txt");
}

std::string DataPartStorageOnDisk::getName() const
{
    return volume->getDisk()->getName();
}

std::string DataPartStorageOnDisk::getDiskType() const
{
    return toString(volume->getDisk()->getType());
}

void DataPartStorageOnDisk::backup(
    TemporaryFilesOnDisks & temp_dirs,
    const MergeTreeDataPartChecksums & checksums,
    const NameSet & files_without_checksums,
    BackupEntries & backup_entries) const
{
    auto disk = volume->getDisk();

    auto temp_dir_it = temp_dirs.find(disk);
    if (temp_dir_it == temp_dirs.end())
        temp_dir_it = temp_dirs.emplace(disk, std::make_shared<TemporaryFileOnDisk>(disk, "tmp/backup_")).first;
    auto temp_dir_owner = temp_dir_it->second;
    fs::path temp_dir = temp_dir_owner->getPath();

    fs::path temp_part_dir = temp_dir / part_dir;
    disk->createDirectories(temp_part_dir);

    for (const auto & [filepath, checksum] : checksums.files)
    {
        String relative_filepath = fs::path(part_dir) / filepath;
        String full_filepath = fs::path(root_path) / part_dir / filepath;
        String hardlink_filepath = temp_part_dir / filepath;
        disk->createHardLink(full_filepath, hardlink_filepath);
        UInt128 file_hash{checksum.file_hash.first, checksum.file_hash.second};
        backup_entries.emplace_back(
            relative_filepath,
            std::make_unique<BackupEntryFromImmutableFile>(disk, hardlink_filepath, checksum.file_size, file_hash, temp_dir_owner));
    }

    for (const auto & filepath : files_without_checksums)
    {
        String relative_filepath = fs::path(part_dir) / filepath;
        String full_filepath = fs::path(root_path) / part_dir / filepath;
        backup_entries.emplace_back(relative_filepath, std::make_unique<BackupEntryFromSmallFile>(disk, full_filepath));
    }
}


DataPartStoragePtr DataPartStorageOnDisk::freeze(
    const std::string & to,
    const std::string & dir_path,
    bool make_source_readonly,
    std::function<void(const DiskPtr &)> save_metadata_callback,
    bool copy_instead_of_hardlink) const
{
    auto disk = volume->getDisk();
    disk->createDirectories(to);

    localBackup(disk, getFullRelativePath(), fs::path(to) / dir_path, make_source_readonly, {}, copy_instead_of_hardlink);

    if (save_metadata_callback)
        save_metadata_callback(disk);

    disk->removeFileIfExists(fs::path(to) / dir_path / "delete-on-destroy.txt");
    disk->removeFileIfExists(fs::path(to) / dir_path / "txn_version.txt");

    auto single_disk_volume = std::make_shared<SingleDiskVolume>(disk->getName(), disk, 0);
    return std::make_shared<DataPartStorageOnDisk>(single_disk_volume, to, dir_path);
}

 DataPartStoragePtr DataPartStorageOnDisk::clone(
    const std::string & to,
    const std::string & dir_path,
    Poco::Logger * log) const
{
    auto disk = volume->getDisk();
    String path_to_clone = fs::path(to) / dir_path / "";

    if (disk->exists(path_to_clone))
    {
        LOG_WARNING(log, "Path {} already exists. Will remove it and clone again.", fullPath(disk, path_to_clone));
        disk->removeRecursive(path_to_clone);
    }
    disk->createDirectories(to);
    volume->getDisk()->copy(getFullRelativePath(), disk, path_to_clone);
    volume->getDisk()->removeFileIfExists(fs::path(path_to_clone) / "delete-on-destroy.txt");

    auto single_disk_volume = std::make_shared<SingleDiskVolume>(disk->getName(), disk, 0);
    return std::make_shared<DataPartStorageOnDisk>(single_disk_volume, to, dir_path);
}


DataPartStorageBuilderOnDisk::DataPartStorageBuilderOnDisk(VolumePtr volume_, std::string root_path_, std::string part_dir_)
    : volume(std::move(volume_)), root_path(std::move(root_path_)), part_dir(std::move(part_dir_))
{
}

std::unique_ptr<ReadBufferFromFileBase> DataPartStorageBuilderOnDisk::readFile(
    const std::string & path,
    const ReadSettings & settings,
    std::optional<size_t> read_hint,
    std::optional<size_t> file_size) const
{
    return volume->getDisk()->readFile(fs::path(root_path) / part_dir / path, settings, read_hint, file_size);
}

std::unique_ptr<WriteBufferFromFileBase> DataPartStorageBuilderOnDisk::writeFile(
    const String & path,
    size_t buf_size,
    const WriteSettings & settings)
{
    return volume->getDisk()->writeFile(fs::path(root_path) / part_dir / path, buf_size, WriteMode::Rewrite, settings);
}

void DataPartStorageBuilderOnDisk::removeFile(const String & path)
{
    return volume->getDisk()->removeFile(fs::path(root_path) / part_dir / path);
}

void DataPartStorageBuilderOnDisk::removeRecursive()
{
    volume->getDisk()->removeRecursive(fs::path(root_path) / part_dir);
}

void DataPartStorageBuilderOnDisk::removeSharedRecursive(bool keep_in_remote_fs)
{
    volume->getDisk()->removeSharedRecursive(fs::path(root_path) / part_dir, keep_in_remote_fs, {});
}

SyncGuardPtr DataPartStorageBuilderOnDisk::getDirectorySyncGuard() const
{
    return volume->getDisk()->getDirectorySyncGuard(fs::path(root_path) / part_dir);
}

void DataPartStorageBuilderOnDisk::createHardLinkFrom(const IDataPartStorage & source, const std::string & from, const std::string & to) const
{
    const auto * source_on_disk = typeid_cast<const DataPartStorageOnDisk *>(&source);
    if (!source_on_disk)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Cannot create hardlink from different storage. Expected DataPartStorageOnDisk, got {}",
            typeid(source).name());

    volume->getDisk()->createHardLink(
        fs::path(source_on_disk->getFullRelativePath()) / from,
        fs::path(root_path) / part_dir / to);
}

bool DataPartStorageBuilderOnDisk::exists() const
{
    return volume->getDisk()->exists(fs::path(root_path) / part_dir);
}


bool DataPartStorageBuilderOnDisk::exists(const std::string & path) const
{
    return volume->getDisk()->exists(fs::path(root_path) / part_dir / path);
}

std::string DataPartStorageBuilderOnDisk::getFullPath() const
{
    return fs::path(volume->getDisk()->getPath()) / root_path / part_dir;
}

std::string DataPartStorageBuilderOnDisk::getFullRelativePath() const
{
    return fs::path(root_path) / part_dir;
}

void DataPartStorageBuilderOnDisk::createDirectories()
{
    return volume->getDisk()->createDirectories(fs::path(root_path) / part_dir);
}

void DataPartStorageBuilderOnDisk::createProjection(const std::string & name)
{
    return volume->getDisk()->createDirectory(fs::path(root_path) / part_dir / name);
}

ReservationPtr DataPartStorageBuilderOnDisk::reserve(UInt64 bytes)
{
    auto res = volume->reserve(bytes);
    if (!res)
        throw Exception(ErrorCodes::NOT_ENOUGH_SPACE, "Cannot reserve {}, not enough space", ReadableSize(bytes));

    return res;
}

DataPartStorageBuilderPtr DataPartStorageBuilderOnDisk::getProjection(const std::string & name) const
{
    return std::make_shared<DataPartStorageBuilderOnDisk>(volume, std::string(fs::path(root_path) / part_dir), name);
}

DataPartStoragePtr DataPartStorageBuilderOnDisk::getStorage() const
{
    return std::make_shared<DataPartStorageOnDisk>(volume, root_path, part_dir);
}

void DataPartStorageBuilderOnDisk::setRelativePath(const std::string & path)
{
    part_dir = path;
}

}
