#ifndef BLOB_UPLOADER_H
#define BLOB_UPLOADER_H

#include <string>
#include <memory>

// Forward declaration to avoid pulling heavy Azure headers into the header file
namespace Azure {
    namespace Storage {
        namespace Blobs {
            class BlobContainerClient;
        }
    }
}

class BlobUploader {
public:
    /**
     * @brief Constructs the uploader using BLOB_CONTAINER_SAS_URL environment variable.
     * @throws std::runtime_error If the environment variable is missing or empty.
     */
    BlobUploader(const std::string sFolder);

    ~BlobUploader();

    /**
     * @brief Uploads a single file to Azure Blob Storage.
     * @param filePath Path to the local file to upload.
     * @return true if upload succeeded, false otherwise.
     */
    bool uploadSingleFile(const std::string& filePath) const;

private:
    std::unique_ptr<Azure::Storage::Blobs::BlobContainerClient> m_containerClient;

    static bool isFileValid(const std::string& path);
    static std::string getBasename(const std::string& path);
};

#endif // BLOB_UPLOADER_H