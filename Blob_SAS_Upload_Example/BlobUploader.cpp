#include "BlobUploader.h"

#include <iostream>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <cctype>

#include <azure/storage/blobs/blob_container_client.hpp>
#include <azure/storage/blobs/block_blob_client.hpp>

using namespace Azure::Storage::Blobs;

static bool IsHexPair(const char a, const char b)
{
    return std::isxdigit(static_cast<unsigned char>(a)) != 0
        && std::isxdigit(static_cast<unsigned char>(b)) != 0;
}

static std::string NormalizeSasUrl(const std::string& raw)
{
    std::string fixed;
    fixed.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '.' && (i + 2) < raw.size() && IsHexPair(raw[i + 1], raw[i + 2]))
        {
            fixed.push_back('%');
            fixed.push_back(raw[i + 1]);
            fixed.push_back(raw[i + 2]);
            i += 2;
            continue;
        }

        fixed.push_back(raw[i]);
    }

    return fixed;
}

BlobUploader::BlobUploader(const std::string sFolder)
{
    const std::string sasUrl = NormalizeSasUrl(sFolder);
    m_containerClient.reset(new BlobContainerClient(sasUrl));
}

BlobUploader::~BlobUploader() = default;

bool BlobUploader::uploadSingleFile(const std::string& filePath) const {
    std::string blobName = getBasename(filePath);

    try {
        if (!isFileValid(filePath)) {
            throw std::runtime_error("File does not exist or cannot be opened");
        }

        BlockBlobClient blobClient = m_containerClient->GetBlockBlobClient(blobName);

        Azure::Storage::Blobs::UploadBlockBlobFromOptions options;
        blobClient.UploadFrom(filePath, options);

        std::cout << "Uploaded \"" << filePath << "\" -> blob \"" << blobName << "\"\n";
        return true;
    }
    catch (const std::exception& error) {
        std::cerr << "Failed to upload \"" << filePath << "\": " << error.what() << "\n";
        return false;
    }
}

bool BlobUploader::isFileValid(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

std::string BlobUploader::getBasename(const std::string& path) {
    const char* lastSlash = std::strrchr(path.c_str(), '/');
#ifdef _WIN32
    const char* lastBackslash = std::strrchr(path.c_str(), '\\');
    if (!lastSlash || (lastBackslash && lastBackslash > lastSlash)) {
        lastSlash = lastBackslash;
    }
#endif
    return lastSlash ? std::string(lastSlash + 1) : path;
}

int main()
{
    // 1. Define a valid Blob Storage SAS URL (with container permissions)
    std::string sasUrl = "https://mystorageaccount.blob.core.windows.net/mycontainer?sv=2021-06-08&st=2026-01-01T00%3A00%3A00Z&se=2026-12-31T23%3A59%3A59Z&sr=c&sp=racwdl&sig=exampleSignature123";

    // 2. Specify the local file path you want to upload
    std::string localFilePath = "sample_data.txt";

    // Create a temporary dummy file to ensure the upload test succeeds
    std::ofstream dummyFile(localFilePath);
    if (dummyFile.is_open())
    {
        dummyFile << "Hello, world! This is a test payload for Azure Blob Storage.\n";
        dummyFile.close();
    }

    try
    {
        // 3. Instantiate the uploader using the SAS URL
        std::cout << "Initializing BlobUploader...\n";
        BlobUploader uploader(sasUrl);

        // 4. Attempt to upload the file
        std::cout << "Uploading file: " << localFilePath << "...\n";
        bool success = uploader.uploadSingleFile(localFilePath);

        if (success)
        {
            std::cout << "File upload completed successfully.\n";
            return 0;
        }
        else
        {
            std::cerr << "File upload failed.\n";
            return 1;
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Unhandled exception during upload: " << ex.what() << "\n";
        return 1;
    }
}