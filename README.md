# ESP32-C6 Matter Delay Switch

A smart delay switch based on ESP32-C6 using the ESP-Matter SDK, compatible with Apple Home, Google Home, and Amazon Alexa.

## 🚀 Features
- **Matter over Wi-Fi**: Seamless integration with major smart home ecosystems.
- **Programmable Delay**: Automatic off-timer (configurable).
- **Physical Control**: Button toggle with hardware debouncing.
- **Deep Factory Reset**: Long-press (5s) to wipe NVS and Storage partitions, enabling re-commissioning.
- **Apple Home Optimized**: Uses `OnOffLight` device type for maximum compatibility.

## 🛠 Hardware Configuration
- **MCU**: ESP32-C6-WROOM-1 (N4)
- **Relay**: GPIO4 (Active Low)
- **Status LED**: GPIO8 (Active High)
- **User Button**: GPIO9 (Pull-up, Active Low)

## 📚 Technical Experience & Lessons Learned

### 1. Build Environment Challenges
- **Python Version Conflict**: ESP-IDF v5.1 is incompatible with Python 3.11+. 
    - **Solution**: Forced the use of **Python 3.9.6** via a system shim in `~/bin/python3`.
- **C++ Standard Mismatch**: Matter SDK v1.2/1.3 has issues with C++20 (e.g., recursive operator `==` in `TCPEndPoint.h`).
    - **Solution**: Locked the project to **C++17** globally in `CMakeLists.txt` to ensure stability.
- **MbedTLS HKDF**: Certain Matter operations require HKDF support in MbedTLS.
    - **Solution**: Manually enabled `MBEDTLS_HKDF_C` in `mbedtls_config.h`.

### 2. Runtime Stability & Memory
- **Tmr Svc Stack Overflow**: Triggered when calling `attribute::update()` inside a FreeRTOS timer callback.
    - **Reason**: The Timer Service task has a very small default stack size; Matter's attribute update logic is too "heavy" for it.
    - **Solution**: Moved all button handling to a dedicated **Worker Task** (`btn_worker`) with a 4KB stack, using a Binary Semaphore for interrupt signaling.

### 3. Factory Reset & Commissioning
- **The "Persistence" Bug**: Standard `nvs_flash_erase()` was insufficient to reset the device for re-commissioning.
    - **Reason**: Matter stores Fabric credentials in a separate FAT partition named `storage`. If this isn't wiped, the device believes it's already commissioned and disables BLE advertising.
    - **Solution**: Implemented a deep reset that locates the `storage` partition via `esp_partition_find_first` and erases it using `esp_partition_erase_range`.

## 🏗 Build & Flash
```bash
# 1. Setup environment (assuming ESP-IDF and ESP-Matter are installed)
. $IDF_PATH/export.sh
. $ESP_MATTER_PATH/export.sh

# 2. Build and Flash
idf.py build flash monitor
```

## 📂 Project Structure
- `main/app_main.cpp`: Core logic, Matter data model, and hardware drivers.
- `partitions.csv`: Custom partition table to accommodate Matter binary size.
- `sdkconfig.defaults`: Essential IDF configurations.
