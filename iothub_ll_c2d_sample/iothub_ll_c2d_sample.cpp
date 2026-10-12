// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// CAVEAT: This sample demonstrates Azure IoT client concepts only and is not a guide to design principles or style.
// Return code and error checking are omitted for brevity. Please follow sound engineering practices
// when writing production code.

#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstdarg>

#include "iothub.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/shared_util_options.h"

#include "iothubtransportmqtt.h"

/* Device connection string, read from IOTHUB_DEVICE_CONNECTION_STRING (see README). */
static std::string GetConnectionString()
{
    char buffer[2048] = {};
    size_t written = 0;
    if (getenv_s(&written, buffer, sizeof(buffer), "IOTHUB_DEVICE_CONNECTION_STRING") == 0 && written > 0)
    {
        return std::string(buffer);
    }
    return std::string();
}

class IotHubC2DSample
{
public:
    int Run()
    {
        InitLogging();

        IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol = MQTT_Protocol;

        if (IoTHub_Init() != 0)
        {
            Log("ERROR", "Failed to initialize IoTHub SDK.");
            CloseLogging();
            return 1;
        }

        const std::string connectionString = GetConnectionString();
        if (connectionString.empty())
        {
            Log("ERROR", "IOTHUB_DEVICE_CONNECTION_STRING is not set.");
            IoTHub_Deinit();
            CloseLogging();
            return 1;
        }

        Log("INFO", "Creating IoTHub Device handle");
        IOTHUB_DEVICE_CLIENT_LL_HANDLE device_ll_handle = IoTHubDeviceClient_LL_CreateFromConnectionString(connectionString.c_str(), protocol);

        if (device_ll_handle == NULL)
        {
            Log("ERROR", "Failure creating IoTHub device. Hint: Check your connection string.");
            IoTHub_Deinit();
            CloseLogging();
            return 1;
        }

        bool traceOn = true;
        if (IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_LOG_TRACE, &traceOn) != IOTHUB_CLIENT_OK)
        {
            Log("ERROR", "Failed to enable SDK trace logging.");
            IoTHubDeviceClient_LL_Destroy(device_ll_handle);
            IoTHub_Deinit();
            CloseLogging();
            return 1;
        }

        bool urlDecodeOn = true;
        if (IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_AUTO_URL_ENCODE_DECODE, &urlDecodeOn) != IOTHUB_CLIENT_OK)
        {
            Log("ERROR", "Failed to enable auto URL encode/decode option.");
            IoTHubDeviceClient_LL_Destroy(device_ll_handle);
            IoTHub_Deinit();
            CloseLogging();
            return 1;
        }

        if (IoTHubDeviceClient_LL_SetMessageCallback(device_ll_handle, ReceiveMsgCallbackThunk, this) != IOTHUB_CLIENT_OK)
        {
            Log("ERROR", "IoTHubDeviceClient_LL_SetMessageCallback failed.");
            IoTHubDeviceClient_LL_Destroy(device_ll_handle);
            IoTHub_Deinit();
            CloseLogging();
            return 1;
        }

        Log("INFO", "Waiting for cloud-to-device messages");
        while (true)
        {
            IoTHubDeviceClient_LL_DoWork(device_ll_handle);
            ThreadAPI_Sleep(10);
        }

        IoTHubDeviceClient_LL_Destroy(device_ll_handle);
        IoTHub_Deinit();

        std::printf("Press any key to continue");
        (void)getchar();
        CloseLogging();
        return 0;
    }

private:
    size_t messageRecvCount_ = 0;

    static void Log(const char* level, const char* format, ...)
    {
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

        const char* path = "iothub_ll_c2d_sample.log";
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
        if (logFile == nullptr)
        {
            std::printf("[ERROR] Failed to open log file: %s\n", path);
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

    static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMsgCallbackThunk(IOTHUB_MESSAGE_HANDLE message, void* user_context)
    {
        return static_cast<IotHubC2DSample*>(user_context)->receive_msg_callback(message);
    }

    IOTHUBMESSAGE_DISPOSITION_RESULT receive_msg_callback(IOTHUB_MESSAGE_HANDLE message)
    {
        if (message == nullptr)
        {
            Log("ERROR", "Received null cloud-to-device message handle.");
            return IOTHUBMESSAGE_REJECTED;
        }

        const char* messageId;
        const char* correlationId;

        if ((messageId = IoTHubMessage_GetMessageId(message)) == NULL)
        {
            messageId = "<unavailable>";
        }

        if ((correlationId = IoTHubMessage_GetCorrelationId(message)) == NULL)
        {
            correlationId = "<unavailable>";
        }

        IOTHUBMESSAGE_CONTENT_TYPE content_type = IoTHubMessage_GetContentType(message);
        if (content_type == IOTHUBMESSAGE_BYTEARRAY)
        {
            const unsigned char* buff_msg;
            size_t buff_len;

            if (IoTHubMessage_GetByteArray(message, &buff_msg, &buff_len) != IOTHUB_MESSAGE_OK)
            {
                Log("ERROR", "Failure retrieving byte array message.");
            }
            else
            {
                Log("INFO", "Received Binary message");
                (void)std::printf("Message ID: %s\r\n Correlation ID: %s\r\n Data: <<<%.*s>>> & Size=%d\r\n", messageId, correlationId, (int)buff_len, buff_msg, (int)buff_len);
            }
        }
        else
        {
            const char* string_msg = IoTHubMessage_GetString(message);
            if (string_msg == NULL)
            {
                Log("ERROR", "Failure retrieving string message.");
            }
            else
            {
                Log("INFO", "Received String Message");
                (void)std::printf("Message ID: %s\r\n Correlation ID: %s\r\n Data: <<<%s>>>\r\n", messageId, correlationId, string_msg);
            }
        }

        const char* property_name = "property_name";
        const char* property_value = IoTHubMessage_GetProperty(message, property_name);
        if (property_value != NULL)
        {
            Log("INFO", "Message \"%s\" property:", property_name);
            (void)std::printf("\tKey: %s Value: %s\r\n", property_name, property_value);
        }

        MAP_HANDLE non_system_properties = IoTHubMessage_Properties(message);
        if (non_system_properties != NULL)
        {
            const char* const* keys;
            const char* const* values;
            size_t count;

            if (Map_GetInternals(non_system_properties, &keys, &values, &count) != MAP_OK)
            {
                Log("ERROR", "Failed retrieving message non-system properties.");
            }
            else
            {
                Log("INFO", "Message properties:");

                for (size_t i = 0; i < count; i++)
                {
                    (void)std::printf("\tKey: %s Value: %s\r\n", (char*)keys[i], (char*)values[i]);
                }
            }
        }

        messageRecvCount_++;
        Log("INFO", "Processed cloud-to-device messages: %zu", messageRecvCount_);

        return IOTHUBMESSAGE_ACCEPTED;
    }
};

int main(void)
{
    IotHubC2DSample sample;
    return sample.Run();
}
