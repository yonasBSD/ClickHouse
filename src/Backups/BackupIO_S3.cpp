#include <Backups/BackupIO_S3.h>

#if USE_AWS_S3
#include <Core/Settings.h>
#include <Common/threadPoolCallbackRunner.h>
#include <Interpreters/Context.h>
#include <IO/SharedThreadPools.h>
#include <IO/ReadBufferFromS3.h>
#include <IO/WriteBufferFromS3.h>
#include <IO/HTTPHeaderEntries.h>
#include <IO/S3/copyS3File.h>
#include <IO/S3/deleteFileFromS3.h>
#include <IO/S3/Client.h>
#include <IO/S3/Credentials.h>
#include <Disks/IDisk.h>

#include <Poco/Util/AbstractConfiguration.h>

#include <aws/core/auth/AWSCredentials.h>

#include <filesystem>


namespace fs = std::filesystem;

namespace DB
{

namespace Setting
{
    extern const SettingsUInt64 backup_restore_s3_retry_attempts;
    extern const SettingsBool enable_s3_requests_logging;
    extern const SettingsBool s3_disable_checksum;
    extern const SettingsUInt64 s3_max_connections;
    extern const SettingsUInt64 s3_max_redirects;
    extern const SettingsBool s3_slow_all_threads_after_network_error;
}

namespace S3AuthSetting
{
    extern const S3AuthSettingsString access_key_id;
    extern const S3AuthSettingsUInt64 expiration_window_seconds;
    extern const S3AuthSettingsBool no_sign_request;
    extern const S3AuthSettingsString region;
    extern const S3AuthSettingsString secret_access_key;
    extern const S3AuthSettingsString server_side_encryption_customer_key_base64;
    extern const S3AuthSettingsBool use_environment_credentials;
    extern const S3AuthSettingsBool use_insecure_imds_request;

    extern const S3AuthSettingsString role_arn;
    extern const S3AuthSettingsString role_session_name;
    extern const S3AuthSettingsString http_client;
    extern const S3AuthSettingsString service_account;
    extern const S3AuthSettingsString metadata_service;
    extern const S3AuthSettingsString request_token_path;
}

namespace S3RequestSetting
{
    extern const S3RequestSettingsBool allow_native_copy;
    extern const S3RequestSettingsString storage_class_name;
    extern const S3RequestSettingsUInt64 http_max_fields;
    extern const S3RequestSettingsUInt64 http_max_field_name_size;
    extern const S3RequestSettingsUInt64 http_max_field_value_size;
}

namespace ErrorCodes
{
    extern const int S3_ERROR;
    extern const int LOGICAL_ERROR;
}

namespace
{
    std::shared_ptr<S3::Client> makeS3Client(
        const S3::URI & s3_uri,
        const String & access_key_id,
        const String & secret_access_key,
        String role_arn,
        String role_session_name,
        const S3Settings & settings,
        const ContextPtr & context)
    {
        Aws::Auth::AWSCredentials credentials(access_key_id, secret_access_key);
        HTTPHeaderEntries headers;
        if (access_key_id.empty())
        {
            credentials = Aws::Auth::AWSCredentials(settings.auth_settings[S3AuthSetting::access_key_id], settings.auth_settings[S3AuthSetting::secret_access_key]);
            headers = settings.auth_settings.headers;
        }

        const auto & request_settings = settings.request_settings;
        const Settings & global_settings = context->getGlobalContext()->getSettingsRef();
        const Settings & local_settings = context->getSettingsRef();

        if (role_arn.empty())
        {
            role_arn = settings.auth_settings[S3AuthSetting::role_arn];
            role_session_name = settings.auth_settings[S3AuthSetting::role_session_name];
        }

        S3::PocoHTTPClientConfiguration client_configuration = S3::ClientFactory::instance().createClientConfiguration(
            settings.auth_settings[S3AuthSetting::region],
            context->getRemoteHostFilter(),
            static_cast<unsigned>(local_settings[Setting::s3_max_redirects]),
            static_cast<unsigned>(local_settings[Setting::backup_restore_s3_retry_attempts]),
            local_settings[Setting::s3_slow_all_threads_after_network_error],
            local_settings[Setting::enable_s3_requests_logging],
            /* for_disk_s3 = */ false,
            request_settings.get_request_throttler,
            request_settings.put_request_throttler,
            s3_uri.uri.getScheme());

        client_configuration.endpointOverride = s3_uri.endpoint;
        client_configuration.maxConnections = static_cast<unsigned>(global_settings[Setting::s3_max_connections]);
        /// Increase connect timeout
        client_configuration.connectTimeoutMs = 10 * 1000;
        /// Requests in backups can be extremely long, set to one hour
        client_configuration.requestTimeoutMs = 60 * 60 * 1000;
        client_configuration.http_keep_alive_timeout = S3::DEFAULT_KEEP_ALIVE_TIMEOUT;
        client_configuration.http_keep_alive_max_requests = S3::DEFAULT_KEEP_ALIVE_MAX_REQUESTS;
        client_configuration.http_max_fields = request_settings[S3RequestSetting::http_max_fields];
        client_configuration.http_max_field_name_size = request_settings[S3RequestSetting::http_max_field_name_size];
        client_configuration.http_max_field_value_size = request_settings[S3RequestSetting::http_max_field_value_size];

        client_configuration.http_client = settings.auth_settings[S3AuthSetting::http_client];
        client_configuration.service_account = settings.auth_settings[S3AuthSetting::service_account];
        client_configuration.metadata_service = settings.auth_settings[S3AuthSetting::metadata_service];
        client_configuration.request_token_path = settings.auth_settings[S3AuthSetting::request_token_path];

        S3::ClientSettings client_settings{
            .use_virtual_addressing = s3_uri.is_virtual_hosted_style,
            .disable_checksum = local_settings[Setting::s3_disable_checksum],
            .gcs_issue_compose_request = context->getConfigRef().getBool("s3.gcs_issue_compose_request", false),
            .is_s3express_bucket = S3::isS3ExpressEndpoint(s3_uri.endpoint),
        };

        return S3::ClientFactory::instance().create(
            client_configuration,
            client_settings,
            credentials.GetAWSAccessKeyId(),
            credentials.GetAWSSecretKey(),
            settings.auth_settings[S3AuthSetting::server_side_encryption_customer_key_base64],
            settings.auth_settings.server_side_encryption_kms_config,
            std::move(headers),
            S3::CredentialsConfiguration
            {
                settings.auth_settings[S3AuthSetting::use_environment_credentials],
                settings.auth_settings[S3AuthSetting::use_insecure_imds_request],
                settings.auth_settings[S3AuthSetting::expiration_window_seconds],
                settings.auth_settings[S3AuthSetting::no_sign_request],
                std::move(role_arn),
                std::move(role_session_name),
                /*sts_endpoint_override=*/""
            });
    }

    Aws::Vector<Aws::S3::Model::Object> listObjects(S3::Client & client, const S3::URI & s3_uri, const String & file_name)
    {
        S3::ListObjectsRequest request;
        request.SetBucket(s3_uri.bucket);
        request.SetPrefix(fs::path{s3_uri.key} / file_name);
        request.SetMaxKeys(1);
        auto outcome = client.ListObjects(request);
        if (!outcome.IsSuccess())
            throw S3Exception(outcome.GetError().GetMessage(), outcome.GetError().GetErrorType());
        return outcome.GetResult().GetContents();
    }
}


BackupReaderS3::BackupReaderS3(
    const S3::URI & s3_uri_,
    const String & access_key_id_,
    const String & secret_access_key_,
    const String & role_arn,
    const String & role_session_name,
    bool allow_s3_native_copy,
    const ReadSettings & read_settings_,
    const WriteSettings & write_settings_,
    const ContextPtr & context_,
    bool is_internal_backup)
    : BackupReaderDefault(read_settings_, write_settings_, getLogger("BackupReaderS3"))
    , s3_uri(s3_uri_)
    , data_source_description{DataSourceType::ObjectStorage, ObjectStorageType::S3, MetadataStorageType::None, s3_uri.endpoint, false, false, ""}
{
    s3_settings.loadFromConfig(context_->getConfigRef(), "s3", context_->getSettingsRef());

    if (auto endpoint_settings = context_->getStorageS3Settings().getSettings(
            s3_uri.uri.toString(), context_->getUserName(), /*ignore_user=*/is_internal_backup))
    {
        s3_settings.updateIfChanged(*endpoint_settings);
    }

    s3_settings.request_settings.updateFromSettings(context_->getSettingsRef(), /* if_changed */true);
    s3_settings.request_settings[S3RequestSetting::allow_native_copy] = allow_s3_native_copy;

    client = makeS3Client(s3_uri_, access_key_id_, secret_access_key_, role_arn, role_session_name, s3_settings, context_);

    if (auto blob_storage_system_log = context_->getBlobStorageLog())
        blob_storage_log = std::make_shared<BlobStorageLogWriter>(blob_storage_system_log);
}

BackupReaderS3::~BackupReaderS3() = default;

bool BackupReaderS3::fileExists(const String & file_name)
{
    return !listObjects(*client, s3_uri, file_name).empty();
}

UInt64 BackupReaderS3::getFileSize(const String & file_name)
{
    auto objects = listObjects(*client, s3_uri, file_name);
    if (objects.empty())
        throw Exception(ErrorCodes::S3_ERROR, "Object {} must exist", file_name);
    return objects[0].GetSize();
}

std::unique_ptr<ReadBufferFromFileBase> BackupReaderS3::readFile(const String & file_name)
{
    return std::make_unique<ReadBufferFromS3>(
        client, s3_uri.bucket, fs::path(s3_uri.key) / file_name, s3_uri.version_id, s3_settings.request_settings, read_settings);
}

void BackupReaderS3::copyFileToDisk(const String & path_in_backup, size_t file_size, bool encrypted_in_backup,
                                    DiskPtr destination_disk, const String & destination_path, WriteMode write_mode)
{
    /// Use the native copy as a more optimal way to copy a file from S3 to S3 if it's possible.
    /// We don't check for `has_throttling` here because the native copy almost doesn't use network.
    auto destination_data_source_description = destination_disk->getDataSourceDescription();
    if (destination_data_source_description.sameKind(data_source_description)
        && (destination_data_source_description.is_encrypted == encrypted_in_backup))
    {
        LOG_TRACE(log, "Copying {} from S3 to disk {}", path_in_backup, destination_disk->getName());
        auto write_blob_function = [&](const Strings & blob_path, WriteMode mode, const std::optional<ObjectAttributes> & object_attributes) -> size_t
        {
            /// Object storage always uses mode `Rewrite` because it simulates append using metadata and different files.
            if (blob_path.size() != 2 || mode != WriteMode::Rewrite)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Blob writing function called with unexpected blob_path.size={} or mode={}",
                                blob_path.size(), mode);

            copyS3File(
                client,
                s3_uri.bucket,
                fs::path(s3_uri.key) / path_in_backup,
                0,
                file_size,
                /* dest_s3_client= */ destination_disk->getS3StorageClient(),
                /* dest_bucket= */ blob_path[1],
                /* dest_key= */ blob_path[0],
                s3_settings.request_settings,
                read_settings,
                blob_storage_log,
                threadPoolCallbackRunnerUnsafe<void>(getBackupsIOThreadPool().get(), "BackupReaderS3"),
                [&, this] { return readFile(path_in_backup); },
                object_attributes);

            return file_size;
        };

        destination_disk->writeFileUsingBlobWritingFunction(destination_path, write_mode, write_blob_function);
        return; /// copied!
    }

    /// Fallback to copy through buffers.
    BackupReaderDefault::copyFileToDisk(path_in_backup, file_size, encrypted_in_backup, destination_disk, destination_path, write_mode);
}


BackupWriterS3::BackupWriterS3(
    const S3::URI & s3_uri_,
    const String & access_key_id_,
    const String & secret_access_key_,
    const String & role_arn,
    const String & role_session_name,
    bool allow_s3_native_copy,
    const String & storage_class_name,
    const ReadSettings & read_settings_,
    const WriteSettings & write_settings_,
    const ContextPtr & context_,
    bool is_internal_backup)
    : BackupWriterDefault(read_settings_, write_settings_, getLogger("BackupWriterS3"))
    , s3_uri(s3_uri_)
    , data_source_description{DataSourceType::ObjectStorage, ObjectStorageType::S3, MetadataStorageType::None, s3_uri.endpoint, false, false, ""}
    , s3_capabilities(getCapabilitiesFromConfig(context_->getConfigRef(), "s3"))
{
    s3_settings.loadFromConfig(context_->getConfigRef(), "s3", context_->getSettingsRef());

    if (auto endpoint_settings = context_->getStorageS3Settings().getSettings(
            s3_uri.uri.toString(), context_->getUserName(), /*ignore_user=*/is_internal_backup))
    {
        s3_settings.updateIfChanged(*endpoint_settings);
    }

    s3_settings.request_settings.updateFromSettings(context_->getSettingsRef(), /* if_changed */true);
    s3_settings.request_settings[S3RequestSetting::allow_native_copy] = allow_s3_native_copy;
    s3_settings.request_settings[S3RequestSetting::storage_class_name] = storage_class_name;

    client = makeS3Client(s3_uri_, access_key_id_, secret_access_key_, role_arn, role_session_name, s3_settings, context_);

    if (auto blob_storage_system_log = context_->getBlobStorageLog())
    {
        blob_storage_log = std::make_shared<BlobStorageLogWriter>(blob_storage_system_log);
        if (context_->hasQueryContext())
            blob_storage_log->query_id = context_->getQueryContext()->getCurrentQueryId();
    }
}

void BackupWriterS3::copyFileFromDisk(const String & path_in_backup, DiskPtr src_disk, const String & src_path,
                                      bool copy_encrypted, UInt64 start_pos, UInt64 length)
{
    /// Use the native copy as a more optimal way to copy a file from S3 to S3 if it's possible.
    /// We don't check for `has_throttling` here because the native copy almost doesn't use network.
    auto source_data_source_description = src_disk->getDataSourceDescription();
    if (source_data_source_description.sameKind(data_source_description) && (source_data_source_description.is_encrypted == copy_encrypted))
    {
        /// getBlobPath() can return more than 3 elements if the file is stored as multiple objects in S3 bucket.
        /// In this case we can't use the native copy.
        if (auto blob_path = src_disk->getBlobPath(src_path); blob_path.size() == 2)
        {
            LOG_TRACE(log, "Copying file {} from disk {} to S3", src_path, src_disk->getName());
            copyS3File(
                src_disk->getS3StorageClient(),
                /* src_bucket */ blob_path[1],
                /* src_key= */ blob_path[0],
                start_pos,
                length,
                /* dest_s3_client= */ client,
                /* dest_bucket= */ s3_uri.bucket,
                /* dest_key= */ fs::path(s3_uri.key) / path_in_backup,
                s3_settings.request_settings,
                read_settings,
                blob_storage_log,
                threadPoolCallbackRunnerUnsafe<void>(getBackupsIOThreadPool().get(), "BackupWriterS3"),
                [&]
                {
                    LOG_TRACE(log, "Falling back to copy file {} from disk {} to S3 through buffers", src_path, src_disk->getName());

                    if (copy_encrypted)
                        return src_disk->readEncryptedFile(src_path, read_settings);

                    return src_disk->readFile(src_path, read_settings);
                });
            return; /// copied!
        }
    }

    /// Fallback to copy through buffers.
    BackupWriterDefault::copyFileFromDisk(path_in_backup, src_disk, src_path, copy_encrypted, start_pos, length);
}

void BackupWriterS3::copyFile(const String & destination, const String & source, size_t size)
{
    LOG_TRACE(log, "Copying file inside backup from {} to {}", source, destination);

    const auto source_key = fs::path(s3_uri.key) / source;
    copyS3File(
        client,
        /* src_bucket */ s3_uri.bucket,
        /* src_key= */ source_key,
        0,
        size,
        /* dest_s3_client= */ client,
        /* dest_bucket= */ s3_uri.bucket,
        /* dest_key= */ fs::path(s3_uri.key) / destination,
        s3_settings.request_settings,
        read_settings,
        blob_storage_log,
        threadPoolCallbackRunnerUnsafe<void>(getBackupsIOThreadPool().get(), "BackupWriterS3"),
        [&, this]
        {
            LOG_TRACE(log, "Falling back to copy file inside backup from {} to {} through direct buffers", source, destination);
            return std::make_unique<ReadBufferFromS3>(
                client, s3_uri.bucket, source_key, s3_uri.version_id, s3_settings.request_settings, read_settings);
        });
}

void BackupWriterS3::copyDataToFile(const String & path_in_backup, const CreateReadBufferFunction & create_read_buffer, UInt64 start_pos, UInt64 length)
{
    copyDataToS3File(create_read_buffer, start_pos, length, client, s3_uri.bucket, fs::path(s3_uri.key) / path_in_backup,
                     s3_settings.request_settings, blob_storage_log,
                     threadPoolCallbackRunnerUnsafe<void>(getBackupsIOThreadPool().get(), "BackupWriterS3"));
}

BackupWriterS3::~BackupWriterS3() = default;

bool BackupWriterS3::fileExists(const String & file_name)
{
    return !listObjects(*client, s3_uri, file_name).empty();
}

UInt64 BackupWriterS3::getFileSize(const String & file_name)
{
    auto objects = listObjects(*client, s3_uri, file_name);
    if (objects.empty())
        throw Exception(ErrorCodes::S3_ERROR, "Object {} must exist", file_name);
    return objects[0].GetSize();
}

std::unique_ptr<ReadBuffer> BackupWriterS3::readFile(const String & file_name, size_t expected_file_size)
{
    return std::make_unique<ReadBufferFromS3>(
            client, s3_uri.bucket, fs::path(s3_uri.key) / file_name, s3_uri.version_id, s3_settings.request_settings, read_settings,
            false, 0, 0, false, expected_file_size);
}

std::unique_ptr<WriteBuffer> BackupWriterS3::writeFile(const String & file_name)
{
    return std::make_unique<WriteBufferFromS3>(
        client,
        s3_uri.bucket,
        fs::path(s3_uri.key) / file_name,
        DBMS_DEFAULT_BUFFER_SIZE,
        s3_settings.request_settings,
        blob_storage_log,
        std::nullopt,
        threadPoolCallbackRunnerUnsafe<void>(getBackupsIOThreadPool().get(), "BackupWriterS3"),
        write_settings);
}

void BackupWriterS3::removeFile(const String & file_name)
{
    deleteFileFromS3(client, s3_uri.bucket, fs::path(s3_uri.key) / file_name, /* if_exists = */ true,
                     blob_storage_log);
}

void BackupWriterS3::removeFiles(const Strings & file_names)
{
    Strings keys;
    keys.reserve(file_names.size());
    for (const String & file_name : file_names)
        keys.push_back(fs::path(s3_uri.key) / file_name);

    /// One call of DeleteObjects() cannot remove more than 1000 keys.
    size_t batch_size = 1000;

    deleteFilesFromS3(client, s3_uri.bucket, keys, /* if_exists = */ true,
                      s3_capabilities, batch_size, blob_storage_log);
}

}

#endif
