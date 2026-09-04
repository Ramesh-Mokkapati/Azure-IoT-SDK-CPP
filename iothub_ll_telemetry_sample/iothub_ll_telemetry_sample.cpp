#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>

#include <azure_c_shared_utility/threadapi.h>
#include <azure_c_shared_utility/platform.h>
#include <azureiot/iothub_device_client_ll.h>
#include <azureiot/iothub_client_options.h>
#include <azureiot/iothub_message.h>
#include <azureiot/iothubtransportmqtt.h>

enum class IotSendResult
{
    Success,
    NotInitialized,
    InvalidPayload,
    MessageCreateFailed,
    SetPropertiesFailed,
    EnqueueFailed,
    Timeout,
    DeliveryFailed
};

struct SendOptions
{
    std::chrono::milliseconds timeout;
    int maxRetries;
    std::chrono::milliseconds retryBackoff;

    SendOptions()
        : timeout(std::chrono::seconds(20)),
        maxRetries(3),
        retryBackoff(std::chrono::seconds(2))
    {
    }
};

class IotHubTelemetryClient
{
public:
    IotHubTelemetryClient()
        : deviceHandle_(nullptr)
    {
    }

    ~IotHubTelemetryClient() noexcept
    {
        Cleanup();
    }

    IotHubTelemetryClient(const IotHubTelemetryClient&) = delete;
    IotHubTelemetryClient& operator=(const IotHubTelemetryClient&) = delete;

    IotHubTelemetryClient(IotHubTelemetryClient&& other) noexcept
        : deviceHandle_(other.deviceHandle_)
    {
        other.deviceHandle_ = nullptr;
    }

    IotHubTelemetryClient& operator=(IotHubTelemetryClient&& other) noexcept
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
            Log("ERROR", "Connection string is empty.");
            CloseLogging();
            return false;
        }

        if (platform_init() != 0)
        {
            Log("ERROR", "Failed to initialize underlying network platform.");
            CloseLogging();
            return false;
        }

        deviceHandle_ = IoTHubDeviceClient_LL_CreateFromConnectionString(
            connectionString.c_str(),
            MQTT_Protocol);

        if (deviceHandle_ == nullptr)
        {
            Log("ERROR", "Failed to create IoT Hub client handle.");
            platform_deinit();
            CloseLogging();
            return false;
        }

        bool traceOn = false;
        (void)IoTHubDeviceClient_LL_SetOption(deviceHandle_, OPTION_LOG_TRACE, &traceOn);

        if (IoTHubDeviceClient_LL_SetRetryPolicy(
            deviceHandle_,
            IOTHUB_CLIENT_RETRY_EXPONENTIAL_BACKOFF_WITH_JITTER,
            0) != IOTHUB_CLIENT_OK)
        {
            Log("WARN", "Failed to set SDK transport retry policy; proceeding with defaults.");
        }

        return true;
    }

    IotSendResult SendMessage(const std::string& jsonPayload, const SendOptions& options = SendOptions())
    {
        if (jsonPayload.empty())
        {
            Log("ERROR", "Payload cannot be empty.");
            return IotSendResult::InvalidPayload;
        }

        for (int attempt = 1; attempt <= options.maxRetries; ++attempt)
        {
            IotSendResult result = SendMessageInternal(jsonPayload, options.timeout);

            if (result == IotSendResult::Success)
            {
                return IotSendResult::Success;
            }

            if (result == IotSendResult::NotInitialized || result == IotSendResult::InvalidPayload)
            {
                return result;
            }

            Log("WARN", "Send attempt %d of %d failed. Retrying in %lld ms...",
                attempt, options.maxRetries, static_cast<long long>(options.retryBackoff.count()));

            if (attempt < options.maxRetries)
            {
                std::this_thread::sleep_for(options.retryBackoff);
            }
        }

        Log("ERROR", "Exhausted all %d send attempts.", options.maxRetries);
        return IotSendResult::DeliveryFailed;
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
    struct MessageContext
    {
        std::atomic<bool> confirmationReceived;
        std::atomic<bool> delivered;

        MessageContext()
            : confirmationReceived(false),
            delivered(false)
        {
        }
    };

    IotSendResult SendMessageInternal(const std::string& jsonPayload, std::chrono::milliseconds timeout)
    {
        std::lock_guard<std::mutex> lock(clientMutex_);

        if (deviceHandle_ == nullptr)
        {
            Log("ERROR", "Client handle is not initialized. Call Initialize() first.");
            return IotSendResult::NotInitialized;
        }

        IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(jsonPayload.c_str());
        if (messageHandle == nullptr)
        {
            Log("ERROR", "Failed to allocate IoT Hub message handle.");
            return IotSendResult::MessageCreateFailed;
        }

        if (IoTHubMessage_SetContentTypeSystemProperty(messageHandle, "application/json") != IOTHUB_MESSAGE_OK ||
            IoTHubMessage_SetContentEncodingSystemProperty(messageHandle, "utf-8") != IOTHUB_MESSAGE_OK)
        {
            Log("ERROR", "Failed to assign system properties to message.");
            IoTHubMessage_Destroy(messageHandle);
            return IotSendResult::SetPropertiesFailed;
        }

        MessageContext context;

        if (IoTHubDeviceClient_LL_SendEventAsync(deviceHandle_, messageHandle, SendConfirmationCallback, &context) != IOTHUB_CLIENT_OK)
        {
            Log("ERROR", "Failed to submit message to SDK transport queue.");
            IoTHubMessage_Destroy(messageHandle);
            return IotSendResult::EnqueueFailed;
        }

        Log("INFO", "Dispatched message to queue: %s", jsonPayload.c_str());

        const auto startTime = std::chrono::steady_clock::now();
        const auto pollInterval = std::chrono::milliseconds(50);

        while (!context.confirmationReceived.load())
        {
            IoTHubDeviceClient_LL_DoWork(deviceHandle_);

            if (std::chrono::steady_clock::now() - startTime >= timeout)
            {
                break;
            }

            std::this_thread::sleep_for(pollInterval);
        }

        IoTHubMessage_Destroy(messageHandle);

        if (!context.confirmationReceived.load())
        {
            Log("ERROR", "Timed out waiting for ACK from IoT Hub.");
            return IotSendResult::Timeout;
        }

        return context.delivered.load() ? IotSendResult::Success : IotSendResult::DeliveryFailed;
    }

    static void SendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
    {
        auto* context = static_cast<MessageContext*>(userContextCallback);
        if (context == nullptr)
        {
            return;
        }

        context->delivered.store(result == IOTHUB_CLIENT_CONFIRMATION_OK);
        context->confirmationReceived.store(true);

        if (result == IOTHUB_CLIENT_CONFIRMATION_OK)
        {
            Log("INFO", "Message ACK received from IoT Hub.");
        }
        else
        {
            Log("ERROR", "Message NAK received. Failure status code: %d", static_cast<int>(result));
        }
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

        const char* path = "iothub_telemetry.log";
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
        else
        {
            std::printf("[ERROR] Failed to open log file stream: %s\n", path);
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

        return "HostName=xxx-xxx-xxx.azure-devices.net;DeviceId=xxx-xxx-xxx;SharedAccessKey=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    }

    std::mutex clientMutex_;
    IOTHUB_DEVICE_CLIENT_LL_HANDLE deviceHandle_;
};

int main()
{
    IotHubTelemetryClient client;

    if (!client.Initialize())
    {
        std::printf("Initialization failed. Terminating.\n");
        return 1;
    }

    SendOptions options;
    options.maxRetries = 3;
    options.retryBackoff = std::chrono::seconds(2);
    options.timeout = std::chrono::seconds(15);

    const std::string payload = "{\"temperature\": 24.8, \"humidity\": 52.3}";

    IotSendResult result = client.SendMessage(payload, options);

    if (result == IotSendResult::Success)
    {
        std::printf("Telemetry event dispatched successfully.\n");
        return 0;
    }

    std::printf("Failed to send telemetry event. Error code: %d\n", static_cast<int>(result));
    return 1;
}