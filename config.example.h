/*
 * WiFi Configuration Example
 * 
 * Copy this file to config.h and update with your WiFi credentials
 * This file is ignored by Git to keep your credentials safe
 */

#ifndef CONFIG_H
#define CONFIG_H

// WiFi Settings
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// Camera Settings
#define CAMERA_MODEL_AI_THINKER // Default camera model

// Optional: Custom hostname
// #define HOSTNAME "esp32cam-gallery"

// Optional: Static IP configuration
// #define USE_STATIC_IP
// #ifdef USE_STATIC_IP
//   IPAddress local_IP(192, 168, 1, 100);
//   IPAddress gateway(192, 168, 1, 1);
//   IPAddress subnet(255, 255, 255, 0);
// #endif

// Image capture settings
#define CAPTURE_INTERVAL_MS 1000  // Capture interval in milliseconds
#define JPEG_QUALITY 15           // JPEG quality (10-63, lower = better)

// SD Card settings
#define SD_CARD_MOUNT_POINT "/sdcard"
#define SD_CARD_1BIT_MODE true    // Use 1-bit mode for better compatibility

#endif // CONFIG_H
