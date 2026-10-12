#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <ctime>

#include <azure_c_shared_utility/threadapi.h>
#include <azure_c_shared_utility/platform.h>
#include <azureiot/iothub_device_client_ll.h>
#include <azureiot/iothub_client_options.h>
#include <azureiot/iothubtransportmqtt.h>

enum class IotUploadResult
{
    Success,
    NotInitialized,
    InvalidParameter,
    UploadFailed,
    Timeout
};

struct FileUploadOptions
{
    std::chrono::milliseconds timeout;
    int maxRetries;
    std::chrono::milliseconds retryBackoff;

    FileUploadOptions()
        : timeout(std::chrono::minutes(2)),
        maxRetries(3),
        retryBackoff(std::chrono::seconds(3))
    {
    }
};

class IotHubBlobUploader
{
public:
    IotHubBlobUploader()
        : deviceHandle_(nullptr)
    {
    }

    ~IotHubBlobUploader() noexcept
    {
        Cleanup();
    }

    IotHubBlobUploader(const IotHubBlobUploader&) = delete;
    IotHubBlobUploader& operator=(const IotHubBlobUploader&) = delete;

    IotHubBlobUploader(IotHubBlobUploader&& other) noexcept
        : deviceHandle_(other.deviceHandle_)
    {
        other.deviceHandle_ = nullptr;
    }

    IotHubBlobUploader& operator=(IotHubBlobUploader&& other) noexcept
    {
        if (this != &other)
        {
            Cleanup();
            deviceHandle_ = other.deviceHandle_;
            other.deviceHandle_ = nullptr;
        }
        return *this;
    }

    bool Initialize()
    {
        std::lock_guard<std::mutex> lock(clientMutex_);

        if (deviceHandle_ != nullptr)
        {
            return true;
        }

        InitLogging();

        const std::string connectionString = GetConnectionString();
        if (connectionString.empty())
        {
            Log("ERROR", "IOTHUB_DEVICE_CONNECTION_STRING is not set.");
            CloseLogging();
            return false;
        }

        if (platform_init() != 0)
        {
            Log("ERROR", "Failed to initialize network platform.");
            CloseLogging();
            return false;
        }

        deviceHandle_ = IoTHubDeviceClient_LL_CreateFromConnectionString(
            connectionString.c_str(),
            MQTT_Protocol);

        if (deviceHandle_ == nullptr)
        {
            Log("ERROR", "Failed to create IoT Hub device handle.");
            platform_deinit();
            CloseLogging();
            return false;
        }

        bool traceOn = false;
        (void)IoTHubDeviceClient_LL_SetOption(deviceHandle_, OPTION_LOG_TRACE, &traceOn);

        return true;
    }

    IotUploadResult UploadFile(
        const std::string& destinationFileName,
        const std::string& localFilePath,
        const FileUploadOptions& options = FileUploadOptions())
    {
        FILE* file = nullptr;
        if (fopen_s(&file, localFilePath.c_str(), "rb") != 0 || file == nullptr)
        {
            Log("ERROR", "Failed to open local file for uploading: %s", localFilePath.c_str());
            return IotUploadResult::InvalidParameter;
        }

        fseek(file, 0, SEEK_END);
        long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        if (fileSize < 0)
        {
            Log("ERROR", "Failed to determine file size for: %s", localFilePath.c_str());
            fclose(file);
            return IotUploadResult::InvalidParameter;
        }

        std::vector<unsigned char> fileBuffer(static_cast<size_t>(fileSize));
        if (fileSize > 0)
        {
            size_t bytesRead = fread(fileBuffer.data(), 1, static_cast<size_t>(fileSize), file);
            if (bytesRead != static_cast<size_t>(fileSize))
            {
                Log("ERROR", "Failed to read complete file content.");
                fclose(file);
                return IotUploadResult::InvalidParameter;
            }
        }
        fclose(file);

        Log("INFO", "Starting file upload: %s (%ld bytes) -> Blob: %s",
            localFilePath.c_str(), fileSize, destinationFileName.c_str());

        return UploadBuffer(destinationFileName, fileBuffer.data(), fileBuffer.size(), options);
    }

    IotUploadResult UploadBuffer(
        const std::string& destinationFileName,
        const uint8_t* buffer,
        size_t bufferSize,
        const FileUploadOptions& options = FileUploadOptions())
    {
        if (destinationFileName.empty())
        {
            Log("ERROR", "Destination file name cannot be empty.");
            return IotUploadResult::InvalidParameter;
        }

        if (buffer == nullptr && bufferSize > 0)
        {
            Log("ERROR", "Buffer pointer is null.");
            return IotUploadResult::InvalidParameter;
        }

        for (int attempt = 1; attempt <= options.maxRetries; ++attempt)
        {
            IotUploadResult result = ExecuteUploadSession(
                destinationFileName,
                buffer,
                bufferSize,
                options);

            if (result == IotUploadResult::Success)
            {
                return IotUploadResult::Success;
            }

            if (result == IotUploadResult::NotInitialized || result == IotUploadResult::InvalidParameter)
            {
                return result;
            }

            Log("WARN", "Upload attempt %d of %d failed. Retrying in %lld ms...",
                attempt, options.maxRetries, static_cast<long long>(options.retryBackoff.count()));

            if (attempt < options.maxRetries)
            {
                std::this_thread::sleep_for(options.retryBackoff);
            }
        }

        Log("ERROR", "Exhausted all %d upload attempts.", options.maxRetries);
        return IotUploadResult::UploadFailed;
    }

    void Cleanup() noexcept
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (deviceHandle_ != nullptr)
        {
            IoTHubDeviceClient_LL_Destroy(deviceHandle_);
            deviceHandle_ = nullptr;
            platform_deinit();
            CloseLogging();
        }
    }

private:
    IotUploadResult ExecuteUploadSession(
        const std::string& destinationFileName,
        const uint8_t* buffer,
        size_t bufferSize,
        const FileUploadOptions& options)
    {
        std::lock_guard<std::mutex> lock(clientMutex_);

        if (deviceHandle_ == nullptr)
        {
            Log("ERROR", "Client is not initialized.");
            return IotUploadResult::NotInitialized;
        }

        IOTHUB_CLIENT_RESULT status = IoTHubDeviceClient_LL_UploadToBlob(
            deviceHandle_,
            destinationFileName.c_str(),
            buffer,
            bufferSize);

        if (status != IOTHUB_CLIENT_OK)
        {
            Log("ERROR", "Failed to upload buffer to blob. SDK Code: %d", static_cast<int>(status));
            return IotUploadResult::UploadFailed;
        }

        Log("INFO", "Blob upload completed successfully.");
        return IotUploadResult::Success;
    }

    static void Log(const char* level, const char* format, ...)
    {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);

        std::printf("[%s] ", level);
        va_list args;
        va_start(args, format);
        va_list fileArgs;
        va_copy(fileArgs, args);
        (void)std::vprintf(format, args);

        FILE* logFile = GetLogFile();
        if (logFile != nullptr)
        {
            std::fprintf(logFile, "[%s] ", level);
            (void)std::vfprintf(logFile, format, fileArgs);
            std::fprintf(logFile, "\n");
            std::fflush(logFile);
        }

        va_end(fileArgs);
        va_end(args);
        std::printf("\n");
    }

    static void InitLogging()
    {
        FILE*& logFile = GetLogFile();
        if (logFile != nullptr)
        {
            return;
        }

        const char* path = "iothub_blob_upload.log";
        char pathBuffer[512] = {};
        size_t requiredSize = 0;

        if (getenv_s(&requiredSize, nullptr, 0, "IOTHUB_LOG_FILE") == 0 && requiredSize > 0 && requiredSize <= sizeof(pathBuffer))
        {
            size_t written = 0;
            if (getenv_s(&written, pathBuffer, sizeof(pathBuffer), "IOTHUB_LOG_FILE") == 0 && written > 0)
            {
                path = pathBuffer;
            }
        }

        FILE* openedLogFile = nullptr;
        if (fopen_s(&openedLogFile, path, "a") == 0)
        {
            logFile = openedLogFile;
        }
    }

    static void CloseLogging()
    {
        FILE*& logFile = GetLogFile();
        if (logFile != nullptr)
        {
            std::fclose(logFile);
            logFile = nullptr;
        }
    }

    static FILE*& GetLogFile()
    {
        static FILE* logFile = nullptr;
        return logFile;
    }

    static std::string GetConnectionString()
    {
        char envConnectionString[2048] = {};
        size_t requiredSize = 0;

        if (getenv_s(&requiredSize, nullptr, 0, "IOTHUB_DEVICE_CONNECTION_STRING") == 0 && requiredSize > 0 && requiredSize <= sizeof(envConnectionString))
        {
            size_t written = 0;
            if (getenv_s(&written, envConnectionString, sizeof(envConnectionString), "IOTHUB_DEVICE_CONNECTION_STRING") == 0 && written > 0)
            {
                return std::string(envConnectionString);
            }
        }

        return std::string();
    }

    std::mutex clientMutex_;
    IOTHUB_DEVICE_CLIENT_LL_HANDLE deviceHandle_;
};

static std::string GenerateTimestampedFileName(const std::string& prefix, const std::string& extension)
{
    auto now = std::chrono::system_clock::now();
    std::time_t timeT = std::chrono::system_clock::to_time_t(now);

    std::tm tmBuffer{};
#if defined(_WIN32)
    localtime_s(&tmBuffer, &timeT);
#else
    localtime_r(&timeT, &tmBuffer);
#endif

    std::ostringstream oss;
    oss << prefix << "_"
        << std::put_time(&tmBuffer, "%Y%m%d_%H%M%S")
        << extension;

    return oss.str();
}

int main()
{
    IotHubBlobUploader uploader;

    if (!uploader.Initialize())
    {
        std::printf("Uploader initialization failed.\n");
        return 1;
    }

    FileUploadOptions options;
    options.timeout = std::chrono::minutes(2);

    std::string destinationBlob = GenerateTimestampedFileName("diagnostics/ram_dump", ".txt");
    std::string sampleData = "Log Entry Line 1\nLog Entry Line 2\nTelemetry payload dump...";

    std::printf("Uploading to destination blob: %s\n", destinationBlob.c_str());

    IotUploadResult bufferResult = uploader.UploadBuffer(
        destinationBlob,
        reinterpret_cast<const uint8_t*>(sampleData.data()),
        sampleData.size(),
        options);

    if (bufferResult == IotUploadResult::Success)
    {
        std::printf("RAM buffer upload completed successfully.\n");
        return 0;
    }

    std::printf("File upload failed. Status code: %d\n", static_cast<int>(bufferResult));
    return 1;
}