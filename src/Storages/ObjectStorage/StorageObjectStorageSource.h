#pragma once
#include <optional>
#include <Common/re2.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/ClusterFunctionReadTask.h>
#include <IO/Archives/IArchiveReader.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Formats/IInputFormat.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/IObjectIterator.h>
#include <Formats/FormatParserSharedResources.h>
#include <Formats/FormatFilterInfo.h>
namespace DB
{

class SchemaCache;

class StorageObjectStorageSource : public ISource
{
    friend class ObjectStorageQueueSource;
public:
    using ObjectInfos = StorageObjectStorage::ObjectInfos;

    class ReadTaskIterator;
    class GlobIterator;
    class KeysIterator;
    class ArchiveIterator;

    StorageObjectStorageSource(
        String name_,
        ObjectStoragePtr object_storage_,
        StorageObjectStorageConfigurationPtr configuration,
        const ReadFromFormatInfo & info,
        const std::optional<FormatSettings> & format_settings_,
        ContextPtr context_,
        UInt64 max_block_size_,
        std::shared_ptr<IObjectIterator> file_iterator_,
        FormatParserSharedResourcesPtr parser_shared_resources_,
        FormatFilterInfoPtr format_filter_info_,
        bool need_only_count_);

    ~StorageObjectStorageSource() override;

    String getName() const override { return name; }

    Chunk generate() override;

    void onFinish() override { parser_shared_resources->finishStream(); }

    static std::shared_ptr<IObjectIterator> createFileIterator(
        StorageObjectStorageConfigurationPtr configuration,
        const StorageObjectStorageQuerySettings & query_settings,
        ObjectStoragePtr object_storage,
        bool distributed_processing,
        const ContextPtr & local_context,
        const ActionsDAG::Node * predicate,
        const ActionsDAG * filter_actions_dag,
        const NamesAndTypesList & virtual_columns,
        const NamesAndTypesList & hive_columns,
        ObjectInfos * read_keys,
        std::function<void(FileProgress)> file_progress_callback = {},
        bool ignore_archive_globs = false,
        bool skip_object_metadata = false);

    static std::string getUniqueStoragePathIdentifier(
        const StorageObjectStorageConfiguration & configuration,
        const ObjectInfo & object_info,
        bool include_connection_info = true);

    static std::unique_ptr<ReadBufferFromFileBase> createReadBuffer(
        ObjectInfo & object_info,
        const ObjectStoragePtr & object_storage,
        const ContextPtr & context_,
        const LoggerPtr & log,
        const std::optional<ReadSettings> & read_settings = std::nullopt);

protected:
    const String name;
    ObjectStoragePtr object_storage;
    const StorageObjectStorageConfigurationPtr configuration;
    const ContextPtr read_context;
    const std::optional<FormatSettings> format_settings;
    const UInt64 max_block_size;
    const bool need_only_count;
    FormatParserSharedResourcesPtr parser_shared_resources;
    FormatFilterInfoPtr format_filter_info;

    ReadFromFormatInfo read_from_format_info;
    const std::shared_ptr<ThreadPool> create_reader_pool;

    std::shared_ptr<IObjectIterator> file_iterator;
    SchemaCache & schema_cache;
    bool initialized = false;
    size_t total_rows_in_file = 0;
    LoggerPtr log = getLogger("StorageObjectStorageSource");

    struct ReaderHolder : private boost::noncopyable
    {
    public:
        ReaderHolder(
            ObjectInfoPtr object_info_,
            std::unique_ptr<ReadBuffer> read_buf_,
            std::shared_ptr<ISource> source_,
            std::unique_ptr<QueryPipeline> pipeline_,
            std::unique_ptr<PullingPipelineExecutor> reader_);

        ReaderHolder() = default;
        ReaderHolder(ReaderHolder && other) noexcept { *this = std::move(other); }
        ReaderHolder & operator=(ReaderHolder && other) noexcept;

        explicit operator bool() const { return reader != nullptr; }
        PullingPipelineExecutor * operator->() { return reader.get(); }
        const PullingPipelineExecutor * operator->() const { return reader.get(); }

        ObjectInfoPtr getObjectInfo() const { return object_info; }
        const IInputFormat * getInputFormat() const { return dynamic_cast<const IInputFormat *>(source.get()); }

    private:
        ObjectInfoPtr object_info;
        std::unique_ptr<ReadBuffer> read_buf;
        std::shared_ptr<ISource> source;
        std::unique_ptr<QueryPipeline> pipeline;
        std::unique_ptr<PullingPipelineExecutor> reader;
    };

    ReaderHolder reader;
    ThreadPoolCallbackRunnerUnsafe<ReaderHolder> create_reader_scheduler;
    std::future<ReaderHolder> reader_future;

    /// Recreate ReadBuffer and Pipeline for each file.
    static ReaderHolder createReader(
        size_t processor,
        const std::shared_ptr<IObjectIterator> & file_iterator,
        const StorageObjectStorageConfigurationPtr & configuration,
        const ObjectStoragePtr & object_storage,
        ReadFromFormatInfo & read_from_format_info,
        const std::optional<FormatSettings> & format_settings,
        const ContextPtr & context_,
        SchemaCache * schema_cache,
        const LoggerPtr & log,
        size_t max_block_size,
        FormatParserSharedResourcesPtr parser_shared_resources,
        FormatFilterInfoPtr format_filter_info,
        bool need_only_count);

    ReaderHolder createReader();

    std::future<ReaderHolder> createReaderAsync();

    void addNumRowsToCache(const ObjectInfo & object_info, size_t num_rows);
    void lazyInitialize();
};

class StorageObjectStorageSource::ReadTaskIterator : public IObjectIterator, private WithContext
{
public:
    ReadTaskIterator(
        const ClusterFunctionReadTaskCallback & callback_,
        size_t max_threads_count,
        bool is_archive_,
        ObjectStoragePtr object_storage_,
        ContextPtr context_);

    ObjectInfoPtr next(size_t) override;

    size_t estimatedKeysCount() override { return buffer.size(); }

private:
    ObjectInfoPtr createObjectInfoInArchive(const std::string & path_to_archive, const std::string & path_in_archive);

    ClusterFunctionReadTaskCallback callback;
    ObjectInfos buffer;
    std::atomic_size_t index = 0;
    bool is_archive;
    ObjectStoragePtr object_storage;
    /// path_to_archive -> archive reader.
    std::unordered_map<std::string, std::shared_ptr<IArchiveReader>> archive_readers;
    std::mutex archive_readers_mutex;
    LoggerPtr log = getLogger("ReadTaskIterator");
};

class StorageObjectStorageSource::GlobIterator : public IObjectIterator, WithContext
{
public:
    GlobIterator(
        ObjectStoragePtr object_storage_,
        StorageObjectStorageConfigurationPtr configuration_,
        const ActionsDAG::Node * predicate,
        const NamesAndTypesList & virtual_columns_,
        const NamesAndTypesList & hive_columns_,
        ContextPtr context_,
        ObjectInfos * read_keys_,
        size_t list_object_keys_size,
        bool throw_on_zero_files_match_,
        std::function<void(FileProgress)> file_progress_callback_ = {});

    ~GlobIterator() override = default;

    ObjectInfoPtr next(size_t processor) override;

    size_t estimatedKeysCount() override;

private:
    ObjectInfoPtr nextUnlocked(size_t processor);
    void createFilterAST(const String & any_key);
    void fillBufferForKey(const std::string & uri_key);

    const ObjectStoragePtr object_storage;
    const StorageObjectStorageConfigurationPtr configuration;
    const NamesAndTypesList virtual_columns;
    const NamesAndTypesList hive_columns;
    const bool throw_on_zero_files_match;
    const LoggerPtr log;

    size_t index = 0;

    ObjectInfos object_infos;
    ObjectInfos * read_keys;
    ExpressionActionsPtr filter_expr;
    ObjectStorageIteratorPtr object_storage_iterator;
    bool recursive{false};
    std::vector<String> expanded_keys;
    std::vector<String>::iterator expanded_keys_iter;

    std::unique_ptr<re2::RE2> matcher;

    bool is_finished = false;
    bool first_iteration = true;
    std::mutex next_mutex;
    const ContextPtr local_context;

    std::function<void(FileProgress)> file_progress_callback;
};

class StorageObjectStorageSource::KeysIterator : public IObjectIterator
{
public:
    KeysIterator(
        const Strings & keys_,
        ObjectStoragePtr object_storage_,
        const NamesAndTypesList & virtual_columns_,
        ObjectInfos * read_keys_,
        bool ignore_non_existent_files_,
        bool skip_object_metadata_,
        std::function<void(FileProgress)> file_progress_callback = {});

    ~KeysIterator() override = default;

    ObjectInfoPtr next(size_t processor) override;

    size_t estimatedKeysCount() override { return keys.size(); }

private:
    const ObjectStoragePtr object_storage;
    const NamesAndTypesList virtual_columns;
    const std::function<void(FileProgress)> file_progress_callback;
    const std::vector<String> keys;
    std::atomic<size_t> index = 0;
    bool ignore_non_existent_files;
    bool skip_object_metadata;
};

/*
 * An archives iterator.
 * Allows to iterate files inside one or many archives.
 * `archives_iterator` is an iterator which iterates over different archives.
 * There are two ways to read files in archives:
 * 1. When we want to read one concete file in each archive.
 *    In this case we go through all archives, check if this certain file
 *    exists within this archive and read it if it exists.
 * 2. When we have a certain pattern of files we want to read in each archive.
 *    For this purpose we create a filter defined as IArchiveReader::NameFilter.
 */
class StorageObjectStorageSource::ArchiveIterator : public IObjectIterator, private WithContext
{
public:
    explicit ArchiveIterator(
        ObjectStoragePtr object_storage_,
        StorageObjectStorageConfigurationPtr configuration_,
        std::unique_ptr<IObjectIterator> archives_iterator_,
        ContextPtr context_,
        ObjectInfos * read_keys_,
        bool ignore_archive_globs_ = false);

    ObjectInfoPtr next(size_t processor) override;

    size_t estimatedKeysCount() override;

    struct ObjectInfoInArchive : public ObjectInfo
    {
        ObjectInfoInArchive(
            ObjectInfoPtr archive_object_,
            const std::string & path_in_archive_,
            std::shared_ptr<IArchiveReader> archive_reader_,
            IArchiveReader::FileInfo && file_info_);

        std::string getFileName() const override
        {
            return path_in_archive;
        }

        std::string getPath() const override
        {
            return archive_object->getPath() + "::" + path_in_archive;
        }

        std::string getPathToArchive() const override
        {
            return archive_object->getPath();
        }

        bool isArchive() const override { return true; }

        size_t fileSizeInArchive() const override { return file_info.uncompressed_size; }

        const ObjectInfoPtr archive_object;
        const std::string path_in_archive;
        const std::shared_ptr<IArchiveReader> archive_reader;
        const IArchiveReader::FileInfo file_info;
    };

private:
    std::shared_ptr<IArchiveReader> createArchiveReader(ObjectInfoPtr object_info) const;

    const ObjectStoragePtr object_storage;
    const bool is_path_in_archive_with_globs;
    /// Iterator which iterates through different archives.
    const std::unique_ptr<IObjectIterator> archives_iterator;
    /// Used when files inside archive are defined with a glob
    const IArchiveReader::NameFilter filter = {};
    const LoggerPtr log;
    /// Current file inside the archive.
    std::string path_in_archive = {};
    /// Read keys of files inside archives.
    ObjectInfos * read_keys;
    /// Object pointing to archive (NOT path within archive).
    ObjectInfoPtr archive_object;
    /// Reader of the archive.
    std::shared_ptr<IArchiveReader> archive_reader;
    /// File enumerator inside the archive.
    std::unique_ptr<IArchiveReader::FileEnumerator> file_enumerator;

    bool ignore_archive_globs;

    std::mutex next_mutex;
};

}
