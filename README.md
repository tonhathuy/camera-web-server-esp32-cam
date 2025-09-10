# ESP32-CAM Web Gallery Server

🚀 **A complete ESP32-CAM solution with web-based image gallery and WiFi remote access**

## Features ✨

- 📷 **Automatic image capture** - Takes photos every second and saves to SD card
- 🖼️ **Web-based gallery** - View all captured images through a beautiful web interface
- 🌐 **WiFi remote access** - No need to remove SD card, access everything via web browser
- 📱 **Mobile responsive** - Works perfectly on phones, tablets, and desktop
- 🚀 **Optimized performance** - Fast loading thumbnails with lazy loading
- 🔧 **Debug tools** - Built-in SD card status and file management
- 📺 **Live streaming** - Real-time camera feed
- 🎯 **Easy setup** - Just configure WiFi and upload!

## Hardware Requirements 🛠️

- **ESP32-CAM module** (AI-Thinker or compatible)
- **SD card** (Class 10 recommended)
- **External antenna** (optional, for better WiFi range)
- **FTDI programmer** or **ESP32-CAM-MB** for uploading code

## Pin Configuration 📋

This project is configured for **AI-Thinker ESP32-CAM** by default:

| Function | GPIO Pin |
|----------|----------|
| Camera Data | GPIO 4-15 |
| SD Card | GPIO 2, 14, 15, 13 |
| LED Flash | GPIO 4 |
| External LED | GPIO 33 |

## Quick Start 🚀

### 1. Hardware Setup
1. Insert SD card into ESP32-CAM
2. Connect FTDI programmer (or use ESP32-CAM-MB)
3. Power on the module

### 2. Software Setup
1. **Install Arduino IDE** with ESP32 support
2. **Install required libraries**:
   ```
   ESP32 Board Package (v3.2.0+)
   ```
3. **Configure WiFi credentials** in `CameraWebServer.ino`:
   ```cpp
   const char *ssid = "YOUR_WIFI_NAME";
   const char *password = "YOUR_WIFI_PASSWORD";
   ```

### 3. Upload and Run
1. Select **"AI Thinker ESP32-CAM"** board
2. Set **partition scheme** to "Huge APP (3MB No OTA/1MB SPIFFS)"
3. Upload the code
4. Open Serial Monitor to see the IP address
5. Access `http://[ESP32_IP]/` in your browser

## Web Interface 🌐

### Home Page (`/`)
- 📷 Live stream access
- 📸 Manual photo capture
- 🖼️ Gallery access
- 🔧 Debug tools

### Gallery (`/gallery`)
- Grid view of all captured images
- Optimized thumbnail loading
- Click to view full resolution
- Mobile-friendly responsive design

### Debug Tools (`/debug`)
- SD card status information
- File listing with sizes
- Storage diagnostics

## API Endpoints 📡

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Home page with navigation |
| `/gallery` | GET | Image gallery interface |
| `/image/filename.jpg` | GET | Serve individual images |
| `/stream` | GET | Live camera stream |
| `/capture` | GET | Take single photo |
| `/debug` | GET | SD card debug info |
| `/status` | GET | Camera status (JSON) |
| `/control` | GET | Camera control parameters |

## Camera Configuration ⚙️

### Image Quality Settings
```cpp
config.jpeg_quality = 15;  // Lower = better quality (10-63)
config.frame_size = FRAMESIZE_QXGA;  // 2048x1536
```

### Supported Frame Sizes
- `FRAMESIZE_QXGA` (2048x1536) - Default
- `FRAMESIZE_UXGA` (1600x1200)
- `FRAMESIZE_SVGA` (800x600)
- `FRAMESIZE_VGA` (640x480)

## Performance Optimizations 🚀

### Memory Management
- **Stack size increased** to 8192 bytes to prevent overflow
- **Chunked HTTP responses** for large files
- **Lazy loading** for gallery images
- **Efficient buffer management**

### Network Optimization
- **Content-Length headers** for better browser handling
- **Cache headers** for image caching
- **CORS enabled** for cross-origin requests
- **Error handling** with custom 404 handler

## File Structure 📁

```
CameraWebServer/
├── CameraWebServer.ino      # Main Arduino sketch
├── app_httpd.cpp           # Web server implementation
├── camera_pins.h           # Pin configurations
├── camera_index.h          # Web interface HTML
├── partitions.csv          # ESP32 partition table
├── ci.json                # Configuration file
└── README.md              # This documentation
```

## Troubleshooting 🔧

### Common Issues

**1. Camera initialization failed**
- Check camera module connection
- Verify power supply (5V recommended)
- Try different camera module

**2. SD card not detected**
- Use Class 10 SD card (32GB max for FAT32)
- Check SD card formatting (FAT32)
- Verify SD card connections

**3. WiFi connection issues**
- Double-check SSID and password
- Ensure 2.4GHz WiFi network
- Check signal strength

**4. Out of memory errors**
- Reduce JPEG quality (increase number 10→15)
- Lower frame size if needed
- Check available heap memory

### Serial Monitor Debug
Enable debug output to see detailed logs:
```cpp
Serial.setDebugOutput(true);
```

## Advanced Features 🔥

### Thumbnail System
- Automatic thumbnail generation via CSS
- Fast gallery loading
- Progressive image enhancement

### Error Recovery
- Automatic retry on SD card errors
- Graceful handling of camera failures
- Memory leak prevention

### Security Features
- CORS headers for safe web access
- Input validation for all endpoints
- Safe file path handling

## Performance Benchmarks 📊

| Metric | Value |
|--------|-------|
| Image capture rate | 1 fps |
| Gallery load time | 5-10 seconds (50 images) |
| Single image load | 2-3 seconds |
| Memory usage | ~180KB heap |
| SD card write speed | ~500KB/s |

## Contributing 🤝

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## License 📄

This project is open source and available under the MIT License.

## Support 💡

For questions and support:
- Check the **Issues** section
- Review **troubleshooting** guide above
- Test with **debug tools** (`/debug` endpoint)

## Changelog 📝

### v1.0.0 (Current)
- ✅ Complete ESP32-CAM web gallery implementation
- ✅ Optimized thumbnail loading system
- ✅ Mobile-responsive design
- ✅ Advanced error handling
- ✅ Performance optimizations
- ✅ Comprehensive documentation

---

**Made with ❤️ for the ESP32-CAM community**

*Happy coding! 🚀*
