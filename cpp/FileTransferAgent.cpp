/*
 * Copyright (c) 2018-2019 Snowflake Computing, Inc. All rights reserved.
 */

#include <memory>
#include <fstream>
#include <iostream>
#include <vector>
#include "FileTransferAgent.hpp"
#include "snowflake/SnowflakeTransferException.hpp"
#include "snowflake/IStatementPutGet.hpp"
#include "util/Base64.hpp"
#include "SnowflakeS3Client.hpp"
#include "StorageClientFactory.hpp"
#include "crypto/CipherStreamBuf.hpp"
#include "crypto/Cryptor.hpp"
#include "util/CompressionUtil.hpp"
#include "util/ThreadPool.hpp"
#include "EncryptionProvider.hpp"
#include "logger/SFLogger.hpp"
#include "snowflake/platform.h"

using ::std::string;
using ::std::vector;
using ::Snowflake::Client::RemoteStorageRequestOutcome;

namespace
{
  const std::string FILE_PROTOCOL = "file://";

  void replaceStrAll(std::string& stringToReplace,
                  std::string const& oldValue,
                  std::string const& newValue)
  {
    size_t oldValueLen = oldValue.length();
    size_t newValueLen = newValue.length();
    if (0 == oldValueLen)
    {
      return;
    }

    size_t index = 0;
    while (true) {
      /* Locate the substring to replace. */
      index = stringToReplace.find(oldValue, index);
      if (index == std::string::npos) break;

      /* Make the replacement. */
      stringToReplace.replace(index, oldValueLen, newValue);

      /* Advance index forward so the next iteration doesn't pick it up as well. */
      index += newValueLen;
    }
  }
}

Snowflake::Client::FileTransferAgent::FileTransferAgent(
  IStatementPutGet *statement,
  TransferConfig *transferConfig) :
  m_stmtPutGet(statement),
  m_FileMetadataInitializer(m_smallFilesMeta, m_largeFilesMeta),
  m_executionResults(nullptr),
  m_storageClient(nullptr),
  m_lastRefreshTokenSec(0),
  m_transferConfig(transferConfig),
  m_uploadStream(nullptr),
  m_uploadStreamSize(0)
{
  _mutex_init(&m_parallelTokRenewMutex);
}

Snowflake::Client::FileTransferAgent::~FileTransferAgent()
{
  reset();
  _mutex_term(&m_parallelTokRenewMutex);
}

void Snowflake::Client::FileTransferAgent::reset()
{
  if (m_executionResults != nullptr)
  {
    delete m_executionResults;
  }

  if (m_storageClient != nullptr)
  {
    delete m_storageClient;
  }

  m_largeFilesMeta.clear();
  m_smallFilesMeta.clear();
}

Snowflake::Client::ITransferResult *
Snowflake::Client::FileTransferAgent::execute(string *command)
{
  reset();

  // first parse command
  if (!m_stmtPutGet->parsePutGetCommand(command, &response))
  {
    throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
      "Failed to parse response.");
  }
  CXX_LOG_INFO("Parse response succeed");

  // init storage client
  m_storageClient = StorageClientFactory::getClient(&response.stageInfo,
                                                    (unsigned int) response.parallel,
                                                    response.threshold,
                                                    m_transferConfig,
                                                    m_stmtPutGet);

  // init file metadata
  initFileMetadata(command);

  switch (response.command)
  {
    case CommandType::UPLOAD:
      upload(command);
      break;

    case CommandType::DOWNLOAD:
      download(command);
      break;

    default:
      throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
        "Invalid command type.");
  }

  return m_executionResults;
}

void Snowflake::Client::FileTransferAgent::initFileMetadata(std::string *command)
{
  m_FileMetadataInitializer.setAutoCompress(response.autoCompress);
  m_FileMetadataInitializer.setSourceCompression(response.sourceCompression);
  m_FileMetadataInitializer.setEncryptionMaterials(&response.encryptionMaterials);

  // Upload data from stream in memory
  if ((m_uploadStream) && (CommandType::UPLOAD == response.command))
  {
    // Only single file uploading supported, the data must be pre-compressed with gzip
    if ((response.srcLocations.size() != 1) || response.autoCompress ||
        sf_strncasecmp(response.sourceCompression, "gzip", sizeof("gzip")))
    {
      CXX_LOG_FATAL(CXX_LOG_NS, "Invalid stream uploading.");
      throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
        "Invalid stream uploading.");
    }

    FileMetadata fileMeta;
    fileMeta.srcFileName = response.srcLocations.at(0);
    fileMeta.srcFileSize = m_uploadStreamSize;
    fileMeta.destFileName = fileMeta.srcFileName;
    fileMeta.sourceCompression = &FileCompressionType::GZIP;
    fileMeta.requireCompress = false;
    fileMeta.targetCompression = &FileCompressionType::GZIP;
    m_largeFilesMeta.push_back(fileMeta);

    return;
  }

  vector<string> *sourceLocations = &response.srcLocations;
  for (size_t i = 0; i < sourceLocations->size(); i++)
  {
    switch (response.command)
    {
      case CommandType::UPLOAD:
        m_FileMetadataInitializer.populateSrcLocUploadMetadata(
            sourceLocations->at(i), response.threshold);
        break;
      case CommandType::DOWNLOAD:
        {
        std::string presignedUrl =
          (m_storageClient->requirePresignedUrl() && (response.presignedUrls.size() > i)) ?
            response.presignedUrls.at(i) : "";
          RemoteStorageRequestOutcome outcome =
            m_FileMetadataInitializer.populateSrcLocDownloadMetadata(
              sourceLocations->at(i), &response.stageInfo.location,
              m_storageClient, &response.encryptionMaterials.at(i),
              presignedUrl);

          if (outcome == TOKEN_EXPIRED)
          {
            CXX_LOG_DEBUG("Token expired when getting download metadata");
            this->renewToken(command);
            i--;
          }
          break;
        }
      default:
        CXX_LOG_FATAL(CXX_LOG_NS, "Invalid command type");
        throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
                                         "Invalid command type.");
    }
  }
}

void Snowflake::Client::FileTransferAgent::upload(string *command)
{
  m_executionResults = new FileTransferExecutionResult(CommandType::UPLOAD,
    m_largeFilesMeta.size() + m_smallFilesMeta.size());

  if (m_largeFilesMeta.size() > 0)
  {
    for (size_t i=0; i<m_largeFilesMeta.size(); i++)
    {
      m_largeFilesMeta[i].overWrite = response.overwrite;
      m_executionResults->SetFileMetadata(&m_largeFilesMeta[i], i);

      if (m_storageClient->requirePresignedUrl())
      {
        getPresignedUrlForUploading(m_largeFilesMeta[i], *command);
      }
      RemoteStorageRequestOutcome outcome = uploadSingleFile(m_storageClient,
                                                 &m_largeFilesMeta[i], i);

      if (outcome == RemoteStorageRequestOutcome::TOKEN_EXPIRED)
      {
        renewToken(command);
        i--;
      }
    }
  }

  if (m_smallFilesMeta.size() > 0)
  {
    if (m_storageClient->requirePresignedUrl())
    {
      for (size_t i = 0; i < m_smallFilesMeta.size(); i++)
      {
        std::string localCommand = *command;
        getPresignedUrlForUploading(m_smallFilesMeta[i], localCommand);
      }
    }
    uploadFilesInParallel(command);
  }
}

void Snowflake::Client::FileTransferAgent::uploadFilesInParallel(std::string *command)
{
  Snowflake::Client::Util::ThreadPool tp((unsigned int)response.parallel);
  for (size_t i=0; i<m_smallFilesMeta.size(); i++)
  {
    size_t resultIndex = i + m_largeFilesMeta.size();
    FileMetadata * metadata = &m_smallFilesMeta[i];
    m_executionResults->SetFileMetadata(&m_smallFilesMeta[i], resultIndex);
    metadata->overWrite = response.overwrite;
    tp.AddJob([metadata, resultIndex, command, this]()->void {
        do
        {
          RemoteStorageRequestOutcome outcome = uploadSingleFile(m_storageClient, metadata,
                                                     resultIndex);
          if (outcome == RemoteStorageRequestOutcome::TOKEN_EXPIRED)
          {
            _mutex_lock(&m_parallelTokRenewMutex);
            this->renewToken(command);
            _mutex_unlock(&m_parallelTokRenewMutex);
          }
          else
          {
            break;
          }
        } while (true);
    });
  }

  // wait till all jobs have been finished
  tp.WaitAll();
}

void Snowflake::Client::FileTransferAgent::renewToken(std::string *command)
{
  long now = (long)time(NULL);

  // Call parse command only if last token renew time is more than 10 minutes
  // (10 minutes is just a random value I pick, to check if token has been
  //  refreshed "recently")
  // This check is only necessary for multiple thread scenario.
  // In a single thread case, if token is expired, then last time token is
  // refreshed is for sure 4 hour ago. However, in multi thread case, if token
  // is expired but last token update time is within 10 minutes, it is almost for
  // sure that some other thread has already renewed the token
  if (now - m_lastRefreshTokenSec > 10 * 60)
  {
    CXX_LOG_INFO("Renew aws token");
    if (!m_stmtPutGet->parsePutGetCommand(command, &response))
    {
      throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
                                       "Failed to parse response.");
    }
    m_storageClient = StorageClientFactory::getClient(&response.stageInfo,
                                                      (unsigned int) response.parallel,
                                                      response.threshold,
                                                      m_transferConfig);
    m_lastRefreshTokenSec = now;
  }
}

RemoteStorageRequestOutcome Snowflake::Client::FileTransferAgent::uploadSingleFile(
  IStorageClient *client,
  FileMetadata *fileMetadata,
  size_t resultIndex)
{
  // compress if required
  if (fileMetadata->requireCompress)
  {
    compressSourceFile(fileMetadata);
  } else
  {
    fileMetadata->srcFileToUpload = fileMetadata->srcFileName;
    fileMetadata->srcFileToUploadSize = fileMetadata->srcFileSize;
  }

  // calculate digest
  updateFileDigest(fileMetadata);

  m_FileMetadataInitializer.initEncryptionMetadata(fileMetadata);

  std::basic_iostream<char> *srcFileStream;
  ::std::fstream fs;

  if (m_uploadStream)
  {
    srcFileStream = m_uploadStream;
  }
  else
 {
    fs = ::std::fstream(fileMetadata->srcFileToUpload.c_str(),
      ::std::ios_base::in |
      ::std::ios_base::binary);
    srcFileStream = &fs;
  }

  Crypto::CipherIOStream inputEncryptStream(*srcFileStream,
    Crypto::CryptoOperation::ENCRYPT,
    fileMetadata->encryptionMetadata.fileKey,
    fileMetadata->encryptionMetadata.iv,
    FILE_ENCRYPTION_BLOCK_SIZE);

  // upload stream
  RemoteStorageRequestOutcome outcome = client->upload(fileMetadata,
                                                       &inputEncryptStream);
  if (fs.is_open())
  {
    fs.close();
  }

  if (fileMetadata->requireCompress)
  {
    //Remove the uniq temp directory created (force remove).
    sf_delete_uniq_dir_if_exists(fileMetadata->srcFileToUpload.c_str());
  }

  m_executionResults->SetTransferOutCome(outcome, resultIndex);
  return outcome;
}

void Snowflake::Client::FileTransferAgent::updateFileDigest(
  FileMetadata *fileMetadata)
{
  const int CHUNK_SIZE = 16 * 4 * 1024;

  std::basic_iostream<char> *srcFileStream;
  ::std::fstream fs;

  if (m_uploadStream)
  {
    srcFileStream = m_uploadStream;
  }
  else
  {
    fs = ::std::fstream(fileMetadata->srcFileToUpload,
      ::std::ios_base::in | ::std::ios_base::binary);
    srcFileStream = &fs;
  }

  Crypto::HashContext hashContext(Crypto::Cryptor::getInstance()
                                    .createHashContext(
                                      Crypto::CryptoHashFunc::SHA256));

  const size_t digestSize = Crypto::cryptoHashDigestSize(
    Crypto::CryptoHashFunc::SHA256);
  char digest[digestSize];

  hashContext.initialize();

  char sourceFileBuffer[CHUNK_SIZE];
  while (srcFileStream->read(sourceFileBuffer, CHUNK_SIZE))
  {
    hashContext.next(sourceFileBuffer, CHUNK_SIZE);
  }

  if (fs.is_open())
  {
    fs.close();
  }
  
  if (m_uploadStream)
  {
    m_uploadStream->clear();
    m_uploadStream->seekg(0, std::ios::beg);
  }

  hashContext.finalize(digest);

  const size_t digestEncodeSize = Util::Base64::encodedLength(digestSize);
  char digestEncode[digestEncodeSize];
  Util::Base64::encode(digest, digestSize, digestEncode);

  fileMetadata->sha256Digest = string(digestEncode, digestEncodeSize);
}

void Snowflake::Client::FileTransferAgent::compressSourceFile(
  FileMetadata *fileMetadata)
{
  CXX_LOG_DEBUG("Starting file compression");
  
  char tempDir[MAX_PATH]={0};
  sf_get_uniq_tmp_dir(tempDir);
  std::string stagingFile(tempDir);
  stagingFile += fileMetadata->destFileName;

  fileMetadata->srcFileToUpload = stagingFile;
  FILE *sourceFile = fopen(fileMetadata->srcFileName.c_str(), "r");
  FILE *destFile = fopen(fileMetadata->srcFileToUpload.c_str(), "w");

  int ret = Util::CompressionUtil::compressWithGzip(sourceFile, destFile,
                                          fileMetadata->srcFileToUploadSize);
  if (ret != 0)
  {
    CXX_LOG_ERROR("Failed to compress source file. Error code: %d", ret);
    throw SnowflakeTransferException(TransferError::COMPRESSION_ERROR, ret);
  }

  fclose(sourceFile);
  fclose(destFile);
}

void Snowflake::Client::FileTransferAgent::download(string *command)
{
  m_executionResults = new FileTransferExecutionResult(CommandType::DOWNLOAD,
    m_largeFilesMeta.size() + m_smallFilesMeta.size());

  int ret = sf_create_directory_if_not_exists((const char *)response.localLocation);
  if (ret != 0)
  {
    CXX_LOG_ERROR("Filed to create directory %s", response.localLocation);
    throw SnowflakeTransferException(TransferError::MKDIR_ERROR, 
      response.localLocation, ret);
  }

  if (m_largeFilesMeta.size() > 0)
  {
    for (size_t i=0; i<m_largeFilesMeta.size(); i++)
    {
      m_executionResults->SetFileMetadata(&m_largeFilesMeta[i], i);
      RemoteStorageRequestOutcome outcome = downloadSingleFile(m_storageClient,
                                                               &m_largeFilesMeta[i], i);

      if (outcome == RemoteStorageRequestOutcome::TOKEN_EXPIRED)
      {
        renewToken(command);
        i--;
      }
    }
  }

  if (m_smallFilesMeta.size() > 0)
  {
    downloadFilesInParallel(command);
  }
}

void Snowflake::Client::FileTransferAgent::downloadFilesInParallel(std::string *command)
{
  Snowflake::Client::Util::ThreadPool tp((unsigned int)response.parallel);
  for (size_t i=0; i<m_smallFilesMeta.size(); i++)
  {
    size_t resultIndex = i + m_largeFilesMeta.size();
    FileMetadata * metadata = &m_smallFilesMeta[i];
    m_executionResults->SetFileMetadata(&m_smallFilesMeta[i], resultIndex);
    tp.AddJob([metadata, resultIndex, command, this]()->void {
        do
        {
          RemoteStorageRequestOutcome outcome = downloadSingleFile(m_storageClient, metadata,
                                                                 resultIndex);
          if (outcome == RemoteStorageRequestOutcome::TOKEN_EXPIRED)
          {
            _mutex_lock(&m_parallelTokRenewMutex);
            this->renewToken(command);
            _mutex_unlock(&m_parallelTokRenewMutex);
          }
          else
          {
            break;
          }
        } while (true);
    });
  }

  // wait till all jobs have been finished
  tp.WaitAll();
}

RemoteStorageRequestOutcome Snowflake::Client::FileTransferAgent::downloadSingleFile(
  IStorageClient *client,
  FileMetadata *fileMetadata,
  size_t resultIndex)
{
   fileMetadata->destPath = std::string(response.localLocation) + PATH_SEP +
    fileMetadata->destFileName;

  std::basic_fstream<char> dstFile(fileMetadata->destPath.c_str(),
                                std::ios_base::out | std::ios_base::binary );
  if( ! dstFile.is_open())
  {
      CXX_LOG_DEBUG("Could not open file to downoad: %s", std::strerror(errno));
  }

  Crypto::CipherIOStream decryptOutputStream(
                               dstFile,
                               Crypto::CryptoOperation::DECRYPT,
                               fileMetadata->encryptionMetadata.fileKey,
                               fileMetadata->encryptionMetadata.iv,
                               FILE_ENCRYPTION_BLOCK_SIZE);

  RemoteStorageRequestOutcome outcome = client->download(fileMetadata,
                                                         &decryptOutputStream);
  dstFile.close();

  m_executionResults->SetTransferOutCome(outcome, resultIndex);
  return outcome;
}

void Snowflake::Client::FileTransferAgent::getPresignedUrlForUploading(
                               FileMetadata& fileMetadata,
                               const std::string& command)
{
  // need to replace file://mypath/myfile?.csv with file://mypath/myfile1.csv.gz
  std::string presignedUrlCommand = command;
  std::string localFilePath = getLocalFilePathFromCommand(command, false);
  replaceStrAll(presignedUrlCommand, localFilePath, fileMetadata.destFileName);
  // then hand that to GS to get the actual presigned URL we'll use
  PutGetParseResponse rsp;
  if (!m_stmtPutGet->parsePutGetCommand(&presignedUrlCommand, &rsp))
  {
    throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
      "Failed to parse response.");
  }
  CXX_LOG_INFO("Parse response succeed");

  fileMetadata.presignedUrl = rsp.stageInfo.presignedUrl;
}

std::string Snowflake::Client::FileTransferAgent::getLocalFilePathFromCommand(
                               std::string const& command,
                               bool unescape)
{
  if (std::string::npos == command.find(FILE_PROTOCOL))
  {
    CXX_LOG_ERROR("file:// prefix not found in command %s", command.c_str());
    throw SnowflakeTransferException(TransferError::INTERNAL_ERROR,
      "file:// prefix not found in command.");
  }

  int localFilePathBeginIdx = command.find(FILE_PROTOCOL) +
    FILE_PROTOCOL.length();
  bool isLocalFilePathQuoted =
    (localFilePathBeginIdx > FILE_PROTOCOL.length()) &&
    (command.at(localFilePathBeginIdx - 1 - FILE_PROTOCOL.length()) == '\'');

  // the ending index is exclusive
  int localFilePathEndIdx = 0;
  std::string localFilePath = "";

  if (isLocalFilePathQuoted)
  {
    // look for the matching quote
    localFilePathEndIdx = command.find("'", localFilePathBeginIdx);
    if (localFilePathEndIdx > localFilePathBeginIdx)
    {
      localFilePath = command.substr(localFilePathBeginIdx,
        localFilePathEndIdx - localFilePathBeginIdx);
    }
    // unescape backslashes to match the file name from GS
    if (unescape)
    {
      replaceStrAll(localFilePath, "\\\\\\\\", "\\\\");
    }
  }
  else
  {
    // look for the first space or new line or semi colon
    localFilePathEndIdx = command.find_first_of(" \n;", localFilePathBeginIdx);

    if (localFilePathEndIdx > localFilePathBeginIdx)
    {
      localFilePath = command.substr(localFilePathBeginIdx,
        localFilePathEndIdx - localFilePathBeginIdx);
    }
    else if (std::string::npos == localFilePathEndIdx)
    {
      localFilePath = command.substr(localFilePathBeginIdx);
    }
  }

  return localFilePath;
}
