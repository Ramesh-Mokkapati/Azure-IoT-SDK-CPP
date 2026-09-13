# Azure IoT SDK C++ Samples

This repository provides sample C++ implementations demonstrating how to connect devices to Azure IoT Hub using the Azure IoT C SDK. The projects are configured for **Visual Studio 2022** on **Windows** using `vcpkg` for dependency management.

---

## 📦 Project Modules

* **`iothub_ll_telemetry_sample`**: Demonstrates sending device-to-cloud (D2C) telemetry messages to Azure IoT Hub using the lower-layer (`LL`) non-blocking API.
* **`iothub_ll_c2d_sample`**: Demonstrates receiving cloud-to-device (C2D) messages and processing commands sent from Azure IoT Hub.
* **`iothub_blob_upload_sample`**: Demonstrates uploading local files/blobs from an IoT device to Azure Blob Storage via IoT Hub endpoints.
* **`Blob_SAS_Upload_Example`**: Demonstrates SAS-based blob upload using Azure Storage C++ SDK components.

---

## 🧩 Runtime DLL Copy (Windows)

Projects are configured to copy required Azure runtime DLLs into `$(OutDir)` after build.

- `Debug` configurations copy from `$(VcpkgRoot)\installed\$(VcpkgTriplet)\debug\bin\azure-*.dll`
- `Release` configurations copy from `$(VcpkgRoot)\installed\$(VcpkgTriplet)\bin\azure-*.dll`

This prevents missing-DLL runtime errors when launching executables from the build output folder.

---

## 🔐 Environment Configuration

Instead of hardcoding credentials in source files, configure your device connection string using the `IOTHUB_DEVICE_CONNECTION_STRING` environment variable:

### Windows (PowerShell)
```powershell
$env:IOTHUB_DEVICE_CONNECTION_STRING="HostName=<YourHubName>.azure-devices.net;DeviceId=<YourDeviceId>;SharedAccessKey=<YourDeviceKey>"