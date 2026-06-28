#include "SettingsStore.hpp"
#include "PythonRunner.hpp"
#include "SshClient.hpp"
#include "Tab5KeyboardInput.hpp"
#include "TerminalBuffer.hpp"
#include "TerminalEmulator.hpp"
#include "WifiProfiles.hpp"
#include "fonts/TerminusBitmap.hpp"

#include <FS.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>
#include <lgfx/v1/misc/DataWrapper.hpp>
#if ENABLE_USB_HOST_KEYBOARD
#include <diskio_impl.h>
#include <ff.h>
#include <tusb.h>
#endif
#if ENABLE_BLE_HID_KEYBOARD
#include <BLESecurity.h>
#endif

namespace {
AppConfig config;
SettingsStore settings;
WifiProfiles wifiProfiles;
SshClient ssh;
PythonRunner python;
Tab5KeyboardInput keyboard;
TerminalBuffer terminal(2500);
TerminalEmulator vt;
M5Canvas screenSprite(&M5.Display);
bool screenSpriteReady = false;

void saveBlePairingFromKeyboard();
bool removeBleDeviceConfig(int index, String& result);

enum class Screen : uint8_t {
    Terminal,
    WifiList,
    WifiEdit,
    WifiScan,
    SshList,
    SshEdit,
    FontList,
    ConfigEdit,
    ImageViewer,
};

enum class WifiConnectState : uint8_t {
    Idle,
    Connecting,
    Connected,
    Failed,
};

enum class VirtualVolume : uint8_t {
    Root,
    Sd,
    Usb,
    Flash,
    Invalid,
};

struct VirtualPath {
    VirtualVolume volume{VirtualVolume::Invalid};
    String virtualPath{"/"};
    String localPath{"/"};
};

struct Rect {
    int x;
    int y;
    int w;
    int h;

    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct ScannedNetwork {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t auth;
};

Screen screen = Screen::Terminal;
size_t activeWifi = 0;
size_t activeSsh = 0;
size_t editIndex = 0;
uint8_t editField = 0;
size_t editCursor = 0;
bool editIsNew = false;
uint32_t lastDraw = 0;
bool dirty = true;
bool headerDirty = false;
String statusLine = "booting";
std::vector<ScannedNetwork> scannedNetworks;
size_t settingScrollOffset = 0;
bool keyboardMenuMode = false;
size_t focusedHeaderButton = 0;
size_t focusedContentItem = 0;
size_t blePairTarget = 0;
bool wifiScanActive = false;
WifiConnectState wifiState = WifiConnectState::Idle;
volatile bool wifiDisabled = false;
size_t wifiProfileIndex = 0;
uint32_t wifiAttemptStart = 0;
uint32_t wifiRetryAt = 0;
uint32_t wifiAttemptTimeoutMs = 20000;
int16_t wifiLastRetrySecond = -1;
uint8_t wifiLastDisconnectReason = 0;
String wifiLastDisconnectName;
String wifiLastFailureText;
String wifiStatusText = "Wi-Fi idle";
String wifiWorkerSsid;
String wifiWorkerPassword;
volatile bool wifiWorkerBusy = false;
volatile bool wifiWorkerDone = false;
bool wifiDirectBeginPending = false;
bool wifiPinsConfigured = false;
bool autoSshConnectPending = false;
volatile bool sshConnectJobRunning = false;
volatile bool sshConnectJobDone = false;
bool sshConnectJobOk = false;
String sshConnectJobError;
SshProfile sshConnectJobProfile;
int sshConnectJobColumns = 100;
int sshConnectJobRows = 32;
TaskHandle_t sshConnectTaskHandle = nullptr;
volatile bool storageBridgeJobRunning = false;
TaskHandle_t storageBridgeTaskHandle = nullptr;
bool timeSyncStarted = false;
bool timeSynced = false;
uint32_t lastTimeSyncAttempt = 0;
bool sdReady = false;
bool sdInitAttempted = false;
String sdLastError = "not initialized";
String sdCwd = "/";
#if ENABLE_USB_HOST_KEYBOARD
bool usbMscMounted = false;
bool usbMscPresent = false;
bool usbDevicePresent = false;
uint32_t usbLastEventMs = 0;
uint8_t usbMscDevAddr = 0;
uint8_t usbMscLun = 0;
BYTE usbMscPdrv = FF_DRV_NOT_USED;
FATFS usbMscFatfs{};
String usbMscStatus = "not mounted";
volatile bool usbMscDone = false;
volatile bool usbMscOk = false;
uint8_t* usbMscSectorBuffer = nullptr;
TwoWire* tab5SystemI2c = &Wire1;
bool tab5SystemI2cStarted = false;
bool tab5Usb5vEnabled = false;
String tab5UsbPowerStatus = "not initialized";
String usbHostStatus = "no device";
#endif
struct SdModeEntry {
    String path;
    uint16_t mode;
};
std::vector<SdModeEntry> sdModes;
bool sdModesLoaded = false;
struct LsOptions {
    bool longFormat{false};
    bool all{false};
    bool human{false};
    String path;
};
String serialCommand;
bool serialCliCapture = false;
String commandLine;
size_t commandCursor = 0;
std::vector<String> commandHistory;
size_t commandHistoryIndex = 0;
bool pythonReplMode = false;
int touchScrollRemainderY = 0;
bool touchScrollActive = false;
bool remoteLineMode = false;
uint32_t lastCursorBlink = 0;
bool cursorVisible = true;
uint32_t lastSshReceive = 0;
bool storageBridgeRunning = false;
String storageBridgeLine;
uint32_t storageBridgeLastStart = 0;
String sshRemoteHome;
bool imageOverlayActive = false;
bool imageOverlayDrawn = false;
String imageViewerPath;
String imageViewerVolume = "sd";
String imageViewerMode = "fit";
String imageViewerStatus;
RTC_DATA_ATTR uint32_t crashStageMagic = 0;
RTC_DATA_ATTR char crashStage[64] = "";

constexpr bool ForceFixedWifiForTest = false;
constexpr const char* FixedWifiSsid = "";
constexpr const char* FixedWifiPassword = "";
constexpr const char* PythonPrompt = ">>> ";
constexpr int SD_SPI_CS_PIN = 42;
constexpr int SD_SPI_SCK_PIN = 43;
constexpr int SD_SPI_MOSI_PIN = 44;
constexpr int SD_SPI_MISO_PIN = 39;

constexpr int HeaderH = 44;
constexpr int HeaderTouchH = HeaderH * 3;
constexpr int ContentTopGap = 56;

constexpr Rect BtnTerminal{4, 4, 72, 36};
constexpr Rect BtnWifi{82, 4, 72, 36};
constexpr Rect BtnSsh{160, 4, 72, 36};
constexpr Rect BtnFont{238, 4, 72, 36};
constexpr Rect BtnConfig{316, 4, 72, 36};
constexpr Rect BtnConnect{402, 4, 80, 36};
constexpr Rect BtnAdd{402, 4, 72, 36};
constexpr Rect BtnMinus{480, 4, 72, 36};
constexpr Rect BtnPlus{558, 4, 72, 36};
constexpr Rect BtnSave{636, 4, 72, 36};
constexpr Rect BtnDelete{714, 4, 72, 36};
constexpr Rect BodyBtn1{8, HeaderH + ContentTopGap, 112, 48};
constexpr Rect BodyBtn2{132, HeaderH + ContentTopGap, 112, 48};
constexpr Rect BodyBtn3{256, HeaderH + ContentTopGap, 150, 48};
constexpr Rect BodyBtn4{418, HeaderH + ContentTopGap, 112, 48};
constexpr Rect FontMinusBtn{8, HeaderH + ContentTopGap, 112, 48};
constexpr Rect FontPlusBtn{132, HeaderH + ContentTopGap, 112, 48};
constexpr Rect FontSaveBtn{256, HeaderH + ContentTopGap, 150, 48};

bool connectSshProfile(const SshProfile& profile);
bool parseSshCommand(const String& line, SshProfile& profile, String& error);
void inheritSavedSshCredentials(SshProfile& profile);
void connectActiveSsh();
void pollSshConnectJob();
bool parseTrailingIndex(const String& command, size_t prefixLen, size_t& index);
void configureTerminal();
void draw();
bool saveConfig();
bool sendSshText(const String& text);
void appendStatus(const String& message);
void handleAction(const KeyAction& action);
bool showImageCommand(const String& args, String& message);
bool handleSshOscMessage(const String& osc);
void resetStorageBridgeState();
void startStorageBridge();
void startStorageBridgeAsync();
VirtualPath resolveVirtualPath(const String& input);
void pollStorageBridge();
void startTimeSync(bool force = false);
void setWifiStatus(const String& message);
void startWifiReconnect(uint32_t timeoutMs = 20000);
void stopWifiRuntime();
void enableWifiRuntime(uint32_t timeoutMs = 20000);
bool drawFallbackUnicodeGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg, uint16_t bg);
void drawHeader();
void drawSettingsTitle(const char* title);
void setUiFont();
String normalizeSdPath(const String& input);
String basenameOnly(const String& path);
bool ensureSdReady();
int hexValue(char c);
#if ENABLE_USB_HOST_KEYBOARD
bool ensureUsbReady(bool quiet = false);
String usbFatPath(const String& path);
#endif
extern "C" bool tab5_python_gfx_command(const char* command);
extern "C" int tab5_python_gfx_width();
extern "C" int tab5_python_gfx_height();

bool headerButtonContains(const Rect& r, int px, int py)
{
    return px >= r.x && px < r.x + r.w && py >= 0 && py < HeaderTouchH;
}

uint16_t gfxColor(uint32_t rgb)
{
    return lgfx::color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

String gfxTakeToken(String& rest)
{
    rest.trim();
    int split = rest.indexOf(' ');
    if (split < 0) {
        String token = rest;
        rest = "";
        return token;
    }
    String token = rest.substring(0, split);
    rest = rest.substring(split + 1);
    return token;
}

int gfxToInt(String& rest, int fallback = 0)
{
    String token = gfxTakeToken(rest);
    if (!token.length()) {
        return fallback;
    }
    return static_cast<int>(strtol(token.c_str(), nullptr, 0));
}

uint8_t gfxHexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}

bool gfxBitAt(const String& bits, int index)
{
    int nibbleIndex = index >> 2;
    if (nibbleIndex < 0 || nibbleIndex >= static_cast<int>(bits.length())) {
        return false;
    }
    uint8_t nibble = gfxHexNibble(bits[nibbleIndex]);
    int shift = 3 - (index & 3);
    return ((nibble >> shift) & 1) != 0;
}

int gfxY(int y)
{
    return HeaderH + y;
}

#if ENABLE_USB_HOST_KEYBOARD
class UsbFatDataWrapper : public lgfx::DataWrapper {
public:
    bool open(const char* path) override
    {
        close();
        if (f_open(&_file, path, FA_READ) != FR_OK) {
            _opened = false;
            _position = 0;
            _size = 0;
            return false;
        }
        _opened = true;
        _position = 0;
        _size = f_size(&_file);
        return true;
    }

    int read(uint8_t* buf, uint32_t len) override
    {
        if (!_opened || !buf || !len) {
            return 0;
        }
        UINT readBytes = 0;
        if (f_read(&_file, buf, len, &readBytes) != FR_OK) {
            return 0;
        }
        _position += readBytes;
        return static_cast<int>(readBytes);
    }

    int read(uint8_t* buf, uint32_t maximumLen, uint32_t requiredLen) override
    {
        uint32_t len = maximumLen;
        if (_size >= _position && len > _size - _position) {
            len = _size - _position;
        }
        if (len < requiredLen) {
            len = requiredLen;
        }
        return read(buf, len);
    }

    void skip(int32_t offset) override
    {
        if (!_opened) {
            return;
        }
        int64_t next = static_cast<int64_t>(_position) + offset;
        if (next < 0) {
            next = 0;
        }
        seek(static_cast<uint32_t>(next));
    }

    bool seek(uint32_t offset) override
    {
        if (!_opened) {
            return false;
        }
        if (f_lseek(&_file, offset) != FR_OK) {
            return false;
        }
        _position = offset;
        return true;
    }

    void close() override
    {
        if (_opened) {
            f_close(&_file);
            _opened = false;
        }
    }

    int32_t tell() override
    {
        return static_cast<int32_t>(_position);
    }

private:
    FIL _file{};
    bool _opened{false};
    uint32_t _position{0};
    uint32_t _size{0};
};
#endif

int terminalTop()
{
    return (screen == Screen::Terminal && ssh.connected()) ? 0 : HeaderH;
}

bool terminalUsesHeader()
{
    return !(screen == Screen::Terminal && ssh.connected());
}

String imageExtension(const String& path)
{
    int dot = path.lastIndexOf('.');
    if (dot < 0 || dot + 1 >= static_cast<int>(path.length())) {
        return "";
    }
    String ext = path.substring(dot + 1);
    ext.toLowerCase();
    return ext;
}

struct ImageInfo {
    int width{0};
    int height{0};
};

uint16_t readBe16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t readBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint32_t readLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool parseImageInfo(const uint8_t* data, size_t size, const String& ext, ImageInfo& info)
{
    if (!data || size < 10) {
        return false;
    }
    if (ext == "png") {
        static const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (size >= 24 && memcmp(data, sig, sizeof(sig)) == 0) {
            info.width = static_cast<int>(readBe32(data + 16));
            info.height = static_cast<int>(readBe32(data + 20));
            return info.width > 0 && info.height > 0;
        }
        return false;
    }
    if (ext == "bmp") {
        if (size >= 26 && data[0] == 'B' && data[1] == 'M') {
            info.width = static_cast<int>(readLe32(data + 18));
            int32_t signedHeight = static_cast<int32_t>(readLe32(data + 22));
            info.height = signedHeight < 0 ? -signedHeight : signedHeight;
            return info.width > 0 && info.height > 0;
        }
        return false;
    }
    if (ext == "jpg" || ext == "jpeg") {
        if (size < 4 || data[0] != 0xff || data[1] != 0xd8) {
            return false;
        }
        size_t pos = 2;
        while (pos + 9 < size) {
            while (pos < size && data[pos] != 0xff) {
                ++pos;
            }
            while (pos < size && data[pos] == 0xff) {
                ++pos;
            }
            if (pos >= size) {
                break;
            }
            uint8_t marker = data[pos++];
            if (marker == 0xd9 || marker == 0xda) {
                break;
            }
            if (pos + 2 > size) {
                break;
            }
            uint16_t segmentLen = readBe16(data + pos);
            if (segmentLen < 2 || pos + segmentLen > size) {
                break;
            }
            bool sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                       (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
            if (sof && segmentLen >= 7) {
                info.height = static_cast<int>(readBe16(data + pos + 3));
                info.width = static_cast<int>(readBe16(data + pos + 5));
                return info.width > 0 && info.height > 0;
            }
            pos += segmentLen;
        }
    }
    return false;
}

template <typename Gfx>
void fillCheckerboardOn(Gfx& gfx, int x, int y, int w, int h)
{
    constexpr int cell = 12;
    for (int yy = 0; yy < h; yy += cell) {
        for (int xx = 0; xx < w; xx += cell) {
            uint16_t color = (((xx / cell) + (yy / cell)) & 1) ? TFT_DARKGREY : TFT_BLACK;
            gfx.fillRect(x + xx, y + yy, min(cell, w - xx), min(cell, h - yy), color);
        }
    }
}

void fillCheckerboard(int x, int y, int w, int h)
{
    fillCheckerboardOn(screenSprite, x, y, w, h);
}

uint8_t* allocateImageBuffer(size_t size)
{
    if (!size) {
        return nullptr;
    }
    uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    return data;
}

template <typename Gfx>
bool drawImageBytesOn(Gfx& gfx, const String& ext, uint8_t* data, size_t size, int x, int y, int maxW, int maxH, float scale)
{
    if (!data || !size || size > UINT32_MAX) {
        return false;
    }
    uint32_t length = static_cast<uint32_t>(size);
    if (ext == "jpg" || ext == "jpeg") {
        return gfx.drawJpg(data, length, x, y, maxW, maxH, 0, 0, scale, scale);
    }
    if (ext == "png") {
        gfx.releasePngMemory();
        bool ok = gfx.drawPng(data, length, x, y, maxW, maxH, 0, 0, scale, scale);
        gfx.releasePngMemory();
        return ok;
    }
    if (ext == "bmp") {
        return gfx.drawBmp(data, length, x, y, maxW, maxH, 0, 0, scale, scale);
    }
    return false;
}

bool readSdFileToBuffer(const String& path, uint8_t*& data, size_t& size)
{
    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }
    size = file.size();
    data = allocateImageBuffer(size);
    if (!data) {
        file.close();
        return false;
    }
    size_t readBytes = file.read(data, size);
    file.close();
    if (readBytes != size) {
        free(data);
        data = nullptr;
        size = 0;
        return false;
    }
    return true;
}

bool readImageFromSd(const String& path, uint8_t*& data, size_t& size, ImageInfo& info)
{
    String ext = imageExtension(path);
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "bmp") {
        return false;
    }
    bool ok = readSdFileToBuffer(path, data, size);
    if (ok) {
        parseImageInfo(data, size, ext, info);
    }
    return ok;
}

bool readFlashFileToBuffer(const String& path, uint8_t*& data, size_t& size)
{
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return false;
    }
    size = file.size();
    data = allocateImageBuffer(size);
    if (!data) {
        file.close();
        return false;
    }
    size_t readBytes = file.read(data, size);
    file.close();
    if (readBytes != size) {
        free(data);
        data = nullptr;
        size = 0;
        return false;
    }
    return true;
}

bool readImageFromFlash(const String& path, uint8_t*& data, size_t& size, ImageInfo& info)
{
    String ext = imageExtension(path);
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "bmp") {
        return false;
    }
    bool ok = readFlashFileToBuffer(path, data, size);
    if (ok) {
        parseImageInfo(data, size, ext, info);
    }
    return ok;
}

#if ENABLE_USB_HOST_KEYBOARD
bool readUsbFileToBuffer(const String& path, uint8_t*& data, size_t& size)
{
    FIL file{};
    if (f_open(&file, usbFatPath(path).c_str(), FA_READ) != FR_OK) {
        return false;
    }
    FSIZE_t fileSize = f_size(&file);
    if (fileSize == 0 || fileSize > UINT32_MAX) {
        f_close(&file);
        return false;
    }
    size = static_cast<size_t>(fileSize);
    data = allocateImageBuffer(size);
    if (!data) {
        f_close(&file);
        return false;
    }
    UINT readBytes = 0;
    bool readOk = f_read(&file, data, size, &readBytes) == FR_OK && readBytes == size;
    f_close(&file);
    if (!readOk) {
        free(data);
        data = nullptr;
        size = 0;
    }
    return readOk;
}

bool readImageFromUsb(const String& path, uint8_t*& data, size_t& size, ImageInfo& info)
{
    String ext = imageExtension(path);
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "bmp") {
        return false;
    }
    bool ok = readUsbFileToBuffer(path, data, size);
    if (!ok) {
        return false;
    }
    parseImageInfo(data, size, ext, info);
    return true;
}
#endif

bool loadImageViewerData(uint8_t*& data, size_t& dataSize, ImageInfo& info)
{
    if (imageViewerVolume == "sd" || imageViewerVolume == "microsd") {
        return ensureSdReady() && readImageFromSd(imageViewerPath, data, dataSize, info);
    }
    if (imageViewerVolume == "flash") {
        return readImageFromFlash(imageViewerPath, data, dataSize, info);
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (imageViewerVolume == "usb") {
        return ensureUsbReady() && readImageFromUsb(imageViewerPath, data, dataSize, info);
    }
#endif
    return false;
}

template <typename Gfx>
bool drawImageInRectOn(Gfx& gfx, int areaX, int areaY, int areaW, int areaH)
{
    areaW = max<int>(1, areaW);
    areaH = max<int>(1, areaH);
    fillCheckerboardOn(gfx, areaX, areaY, areaW, areaH);
    gfx.setClipRect(areaX, areaY, areaW, areaH);

    uint8_t* data = nullptr;
    size_t dataSize = 0;
    ImageInfo info;
    bool loaded = loadImageViewerData(data, dataSize, info);
    if (!loaded) {
        gfx.clearClipRect();
        imageViewerStatus = String("image failed load ") + imageViewerVolume + ":" + imageViewerPath;
        return false;
    }

    String ext = imageExtension(imageViewerPath);
    float scale = 1.0f;
    if (imageViewerMode == "half") {
        scale = 0.5f;
    } else if (imageViewerMode == "quarter") {
        scale = 0.25f;
    } else if (imageViewerMode != "center" && info.width > 0 && info.height > 0) {
        float sx = static_cast<float>(areaW) / static_cast<float>(info.width);
        float sy = static_cast<float>(areaH) / static_cast<float>(info.height);
        scale = min(sx, sy);
        scale = max(0.05f, min(scale, 16.0f));
    }

    int drawW = info.width > 0 ? max<int>(1, static_cast<int>(info.width * scale + 0.5f)) : areaW;
    int drawH = info.height > 0 ? max<int>(1, static_cast<int>(info.height * scale + 0.5f)) : areaH;
    int x = areaX + max<int>(0, (areaW - drawW) / 2);
    int y = areaY + max<int>(0, (areaH - drawH) / 2);
    int maxW = areaW;
    int maxH = areaH;

    bool ok = drawImageBytesOn(gfx, ext, data, dataSize, x, y, maxW, maxH, scale);
    free(data);
    gfx.clearClipRect();

    String dimensions;
    if (info.width > 0 && info.height > 0) {
        dimensions = String(" ") + info.width + "x" + info.height + " -> " + drawW + "x" + drawH;
    }
    imageViewerStatus = ok ? String("image shown ") + imageViewerVolume + ":" + imageViewerPath + dimensions
                           : String("image failed draw ") + imageViewerVolume + ":" + imageViewerPath + dimensions;
    return ok;
}

bool drawImageInRect(int areaX, int areaY, int areaW, int areaH)
{
    if (!screenSpriteReady) {
        imageViewerStatus = "image: display not ready";
        return false;
    }
    return drawImageInRectOn(screenSprite, areaX, areaY, areaW, areaH);
}

void imageOverlayGeometry(int& winX, int& winY, int& winW, int& winH, int& imageX, int& imageY, int& imageW, int& imageH)
{
    const int marginX = max<int>(18, screenSprite.width() / 16);
    const int marginY = max<int>(18, screenSprite.height() / 12);
    winX = marginX;
    winY = marginY;
    winW = max<int>(160, screenSprite.width() - marginX * 2);
    winH = max<int>(120, screenSprite.height() - marginY * 2);
    constexpr int titleH = 28;
    constexpr int footerH = 22;
    imageX = winX + 10;
    imageY = winY + titleH + 8;
    imageW = winW - 20;
    imageH = winH - titleH - footerH - 16;
}

void drawImageOverlay(bool renderImage = true)
{
    int winX = 0;
    int winY = 0;
    int winW = 0;
    int winH = 0;
    int imageX = 0;
    int imageY = 0;
    int imageW = 0;
    int imageH = 0;
    imageOverlayGeometry(winX, winY, winW, winH, imageX, imageY, imageW, imageH);
    const int titleH = 28;
    const int footerH = 22;

    screenSprite.fillRect(winX + 5, winY + 5, winW, winH, TFT_BLACK);
    screenSprite.fillRect(winX, winY, winW, winH, TFT_NAVY);
    screenSprite.drawRect(winX, winY, winW, winH, TFT_CYAN);
    screenSprite.drawRect(winX + 1, winY + 1, winW - 2, winH - 2, TFT_DARKGREY);

    setUiFont();
    String title = String("Image: ") + imageViewerVolume + ":" + imageViewerPath;
    if (title.length() > 80) {
        title = title.substring(0, 77) + "...";
    }
    screenSprite.setTextColor(TFT_WHITE, TFT_NAVY);
    screenSprite.drawString(title, winX + 10, winY + 7);
    screenSprite.drawFastHLine(winX + 1, winY + titleH, winW - 2, TFT_DARKGREY);

    bool ok = false;
    if (renderImage) {
        ok = drawImageInRect(imageX, imageY, imageW, imageH);
    } else {
        fillCheckerboard(imageX, imageY, imageW, imageH);
        ok = imageViewerStatus.startsWith("image shown");
    }

    setUiFont();
    screenSprite.setTextColor(ok ? TFT_GREEN : TFT_RED, TFT_NAVY);
    String status = ok ? "Esc: close" : String("failed: ") + imageViewerStatus;
    if (status.length() > 78) {
        status = status.substring(0, 75) + "...";
    }
    screenSprite.fillRect(winX + 2, winY + winH - footerH, winW - 4, footerH - 2, TFT_NAVY);
    screenSprite.drawString(status, winX + 10, winY + winH - footerH + 4);
}

void drawImageOverlayDirect()
{
    int winX = 0;
    int winY = 0;
    int winW = 0;
    int winH = 0;
    int imageX = 0;
    int imageY = 0;
    int imageW = 0;
    int imageH = 0;
    imageOverlayGeometry(winX, winY, winW, winH, imageX, imageY, imageW, imageH);
    bool ok = drawImageInRectOn(M5.Display, imageX, imageY, imageW, imageH);

    constexpr int footerH = 22;
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(ok ? TFT_GREEN : TFT_RED, TFT_NAVY);
    String status = ok ? "Esc: close" : String("failed: ") + imageViewerStatus;
    if (status.length() > 78) {
        status = status.substring(0, 75) + "...";
    }
    M5.Display.fillRect(winX + 2, winY + winH - footerH, winW - 4, footerH - 2, TFT_NAVY);
    M5.Display.drawString(status, winX + 10, winY + winH - footerH + 4);
}

String normalizeImagePathSpec(const String& spec, String& volume)
{
    String text = spec;
    text.trim();
    int sep = text.indexOf(':');
    if (sep >= 0) {
        volume = text.substring(0, sep);
        volume.toLowerCase();
        return normalizeSdPath(text.substring(sep + 1));
    }
    VirtualPath virtualPath = resolveVirtualPath(text);
    if (virtualPath.volume == VirtualVolume::Sd) {
        volume = "sd";
        return normalizeSdPath(virtualPath.localPath);
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (virtualPath.volume == VirtualVolume::Usb) {
        volume = "usb";
        return normalizeSdPath(virtualPath.localPath);
    }
#endif
    if (virtualPath.volume == VirtualVolume::Flash) {
        volume = "flash";
        return normalizeSdPath(virtualPath.localPath);
    }
    volume = "sd";
    return normalizeSdPath(text);
}

bool showImageCommand(const String& args, String& message)
{
    String rest = args;
    rest.trim();
    if (!rest.length()) {
        message = "usage: image <path|sd:/path|usb:/path> [fit|center|half|quarter]";
        return false;
    }
    String pathToken = gfxTakeToken(rest);
    String volume;
    String path = normalizeImagePathSpec(pathToken, volume);
    String mode = gfxTakeToken(rest);
    mode.toLowerCase();
    if (!mode.length()) {
        mode = "fit";
    }
    if (!(mode == "fit" || mode == "center" || mode == "half" || mode == "quarter")) {
        message = "image: mode must be fit, center, half, or quarter";
        return false;
    }
    String ext = imageExtension(path);
    if (!(ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp")) {
        message = "image: supported extensions are .jpg, .jpeg, .png, .bmp";
        return false;
    }
    if (volume == "sd" || volume == "microsd") {
        if (!ensureSdReady()) {
            message = String("image: sd ") + sdLastError;
            return false;
        }
        if (!SD.exists(path)) {
            message = String("image: not found sd:") + path;
            return false;
        }
    }
#if ENABLE_USB_HOST_KEYBOARD
    else if (volume == "usb") {
        if (!ensureUsbReady()) {
            message = String("image: usb ") + usbMscStatus;
            return false;
        }
        FILINFO info{};
        if (f_stat(usbFatPath(path).c_str(), &info) != FR_OK || (info.fattrib & AM_DIR)) {
            message = String("image: not found usb:") + path;
            return false;
        }
    }
#endif
    else {
        message = String("image: unsupported volume ") + volume;
        return false;
    }
    imageViewerVolume = volume;
    imageViewerPath = path;
    imageViewerMode = mode;
    imageOverlayActive = true;
    imageOverlayDrawn = false;
    keyboardMenuMode = false;
    dirty = true;
    message = String("image: showing ") + volume + ":" + path;
    return true;
}

String decodeHexString(const String& hex)
{
    String out;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        int hi = hexValue(hex[i]);
        int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return "";
        }
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

String basenameOfPath(const String& path)
{
    int slash = path.lastIndexOf('/');
    if (slash < 0 || slash + 1 >= static_cast<int>(path.length())) {
        return path.length() ? path : "image";
    }
    return path.substring(slash + 1);
}

String safeCacheName(const String& remotePath)
{
    String name = basenameOfPath(remotePath);
    if (!name.length() || name == "." || name == "..") {
        name = "image";
    }
    String safe;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    if (!safe.length()) {
        safe = "image";
    }
    return safe;
}

bool mapRemoteMountedImagePath(const String& remotePath, String& volume, String& localPath)
{
    struct Prefix {
        const char* suffix;
        const char* volume;
    };
    const Prefix prefixes[] = {
        {"/.tab5/mnt/sd", "sd"},
        {"/sd", "sd"},
        {"/.tab5/mnt/usb", "usb"},
        {"/usb", "usb"},
    };
    if (!sshRemoteHome.length()) {
        return false;
    }
    for (const auto& prefix : prefixes) {
        String root = sshRemoteHome + prefix.suffix;
        if (remotePath == root || remotePath.startsWith(root + "/")) {
            String sub = remotePath.substring(root.length());
            if (!sub.length()) {
                sub = "/";
            }
            volume = prefix.volume;
            localPath = normalizeSdPath(sub);
            return true;
        }
    }
    return false;
}

bool showRemoteImagePath(const String& remotePath, const String& mode, String& message)
{
    String volume;
    String localPath;
    if (mapRemoteMountedImagePath(remotePath, volume, localPath)) {
        return showImageCommand(volume + ":" + localPath + " " + mode, message);
    }
    if (!ensureSdReady()) {
        message = String("remote image: sd ") + sdLastError;
        return false;
    }
    String ext = imageExtension(remotePath);
    if (!(ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp")) {
        message = "remote image: supported extensions are .jpg, .jpeg, .png, .bmp";
        return false;
    }
    SD.mkdir("/.tab5-cache");
    SD.mkdir("/.tab5-cache/images");
    String local = String("/.tab5-cache/images/") + millis() + "-" + safeCacheName(remotePath);
    String error;
    appendStatus(String("Image: downloading ") + basenameOfPath(remotePath));
    if (activeSsh >= config.ssh.size() ||
        !ssh.scpDownload(config.ssh[activeSsh], remotePath, SD, local, error)) {
        message = String("remote image: scp failed: ") + error;
        return false;
    }
    return showImageCommand(String("sd:") + local + " " + mode, message);
}

bool handleSshOscMessage(const String& osc)
{
    if (!osc.startsWith("777;tab5-image;")) {
        return false;
    }
    String pathHex;
    String mode = "fit";
    size_t start = strlen("777;tab5-image;");
    while (start <= osc.length()) {
        int sep = osc.indexOf(';', start);
        size_t end = sep < 0 ? osc.length() : static_cast<size_t>(sep);
        String part = osc.substring(start, end);
        int eq = part.indexOf('=');
        if (eq > 0) {
            String key = part.substring(0, eq);
            String value = part.substring(eq + 1);
            if (key == "pathhex") {
                pathHex = value;
            } else if (key == "mode") {
                mode = value;
                mode.toLowerCase();
            }
        }
        if (sep < 0) {
            break;
        }
        start = static_cast<size_t>(sep) + 1;
    }
    if (!(mode == "fit" || mode == "center" || mode == "half" || mode == "quarter")) {
        mode = "fit";
    }
    String remotePath = decodeHexString(pathHex);
    if (!remotePath.length()) {
        appendStatus("Image: invalid remote request");
        return true;
    }
    String message;
    showRemoteImagePath(remotePath, mode, message);
    appendStatus(message);
    dirty = true;
    return true;
}

bool pollPythonAbortInput()
{
    M5.update();
    keyboard.update();
    while (keyboard.available()) {
        KeyAction action = keyboard.read();
        if (action.type != KeyActionType::Text || !action.text.length()) {
            continue;
        }
        char c = action.text[0];
        if (c == 0x03 || c == 'q' || c == 'Q') {
            return true;
        }
    }
    return false;
}

extern "C" int tab5_python_gfx_width()
{
    return screenSpriteReady ? screenSprite.width() : M5.Display.width();
}

extern "C" int tab5_python_gfx_height()
{
    int h = screenSpriteReady ? screenSprite.height() : M5.Display.height();
    return max<int>(1, h - HeaderH);
}

extern "C" bool tab5_python_gfx_command(const char* command)
{
    if (!command || !screenSpriteReady) {
        return true;
    }
    String rest(command);
    String op = gfxTakeToken(rest);
    op.toLowerCase();
    const int canvasW = tab5_python_gfx_width();
    const int canvasH = tab5_python_gfx_height();
    screen = Screen::Terminal;
    keyboardMenuMode = false;

    if (op == "clear") {
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0));
        screenSprite.fillRect(0, HeaderH, canvasW, canvasH, gfxColor(color));
        return true;
    }
    if (op == "present") {
        drawHeader();
        screenSprite.pushSprite(0, 0);
        dirty = false;
        headerDirty = false;
        if (pollPythonAbortInput()) {
            appendStatus("python: interrupted");
            return false;
        }
        return true;
    }
    if (op == "px") {
        int x = gfxToInt(rest);
        int y = gfxToInt(rest);
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        if (x >= 0 && x < canvasW && y >= 0 && y < canvasH) {
            screenSprite.drawPixel(x, gfxY(y), gfxColor(color));
        }
        return true;
    }
    if (op == "line") {
        int x0 = gfxToInt(rest);
        int y0 = gfxToInt(rest);
        int x1 = gfxToInt(rest);
        int y1 = gfxToInt(rest);
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        screenSprite.drawLine(x0, gfxY(y0), x1, gfxY(y1), gfxColor(color));
        return true;
    }
    if (op == "rect") {
        int x = gfxToInt(rest);
        int y = gfxToInt(rest);
        int w = gfxToInt(rest);
        int h = gfxToInt(rest);
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        bool fill = gfxToInt(rest, 1) != 0;
        if (fill) {
            screenSprite.fillRect(x, gfxY(y), w, h, gfxColor(color));
        } else {
            screenSprite.drawRect(x, gfxY(y), w, h, gfxColor(color));
        }
        return true;
    }
    if (op == "circle") {
        int x = gfxToInt(rest);
        int y = gfxToInt(rest);
        int r = gfxToInt(rest);
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        bool fill = gfxToInt(rest, 1) != 0;
        if (fill) {
            screenSprite.fillCircle(x, gfxY(y), r, gfxColor(color));
        } else {
            screenSprite.drawCircle(x, gfxY(y), r, gfxColor(color));
        }
        return true;
    }
    if (op == "mono") {
        int x0 = gfxToInt(rest);
        int y0 = gfxToInt(rest);
        int cols = gfxToInt(rest);
        int rows = gfxToInt(rest);
        int cell = gfxToInt(rest);
        uint32_t fg = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        uint32_t bg = static_cast<uint32_t>(gfxToInt(rest, 0));
        rest.trim();
        if (cols <= 0 || rows <= 0 || cell <= 0 || cols > 128 || rows > 128) {
            return true;
        }
        uint16_t fg565 = gfxColor(fg);
        uint16_t bg565 = gfxColor(bg);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                bool on = gfxBitAt(rest, y * cols + x);
                screenSprite.fillRect(x0 + x * cell + 1, gfxY(y0 + y * cell + 1),
                                      max(1, cell - 2), max(1, cell - 2), on ? fg565 : bg565);
            }
        }
        return true;
    }
    if (op == "text") {
        int x = gfxToInt(rest);
        int y = gfxToInt(rest);
        uint32_t color = static_cast<uint32_t>(gfxToInt(rest, 0xffffff));
        screenSprite.setFont(&fonts::AsciiFont8x16);
        screenSprite.setTextSize(1);
        screenSprite.setTextColor(gfxColor(color), TFT_BLACK);
        screenSprite.drawString(rest, x, gfxY(y));
        return true;
    }
    return true;
}

const Rect* headerButtonAt(size_t index)
{
    size_t i = 0;
    if (index == i++) return &BtnTerminal;
    if (index == i++) return &BtnWifi;
    if (index == i++) return &BtnSsh;
    if (index == i++) return &BtnFont;
    if (index == i++) return &BtnConfig;

    if (screen == Screen::Terminal) {
        if (index == i++) return &BtnConnect;
    } else if (screen == Screen::WifiEdit) {
        if (index == i++) return &BtnConnect;
        if (index == i++) return &BtnSave;
        if (index == i++) return &BtnDelete;
    } else if (screen == Screen::SshEdit) {
        if (index == i++) return &BtnSave;
        if (index == i++) return &BtnDelete;
    } else if (screen == Screen::ConfigEdit) {
        if (index == i++) return &BtnSave;
    }
    return nullptr;
}

size_t headerButtonCount()
{
    size_t count = 0;
    while (headerButtonAt(count) != nullptr) {
        ++count;
    }
    return count;
}

void clampFocusedHeaderButton()
{
    const size_t count = headerButtonCount();
    if (!count) {
        focusedHeaderButton = 0;
    } else if (focusedHeaderButton >= count) {
        focusedHeaderButton = count - 1;
    }
}

void focusCurrentScreenButton()
{
    if (screen == Screen::WifiList || screen == Screen::WifiEdit || screen == Screen::WifiScan) {
        focusedHeaderButton = 1;
    } else if (screen == Screen::SshList || screen == Screen::SshEdit) {
        focusedHeaderButton = 2;
    } else if (screen == Screen::FontList) {
        focusedHeaderButton = 3;
    } else if (screen == Screen::ConfigEdit) {
        focusedHeaderButton = 4;
    } else {
        focusedHeaderButton = 0;
    }
    clampFocusedHeaderButton();
}

struct TerminalFontOption {
    const char* id;
    const char* label;
    const lgfx::IFont* japaneseFont;
    uint8_t cellW;
    uint8_t cellH;
    uint8_t settingsLineHeight;
    uint8_t defaultLineStep;
    uint8_t legacyLineStep;
    uint8_t previousLineStep;
};

constexpr TerminalFontOption TerminalFonts[] = {
    {"mono9", "Terminus 8x16", &fonts::lgfxJapanGothic_16, 8, 16, 18, 18, 12, 20},
    {"mono12", "Terminus 10x20", &fonts::lgfxJapanGothic_20, 10, 20, 22, 22, 15, 26},
    {"mono18", "Terminus 14x28", &fonts::lgfxJapanGothic_28, 14, 28, 30, 30, 23, 38},
    {"mono24", "Terminus 18x36", &fonts::lgfxJapanGothic_36, 18, 36, 38, 38, 30, 50},
};

const TerminalFontOption& terminalFont()
{
    for (const auto& option : TerminalFonts) {
        if (config.keyboard.terminalFont == option.id) {
            return option;
        }
    }
    return TerminalFonts[1];
}

uint8_t terminalLineStep()
{
    uint8_t step = config.keyboard.terminalLineStep;
    uint8_t minStep = static_cast<uint8_t>(max<int>(12, terminalFont().settingsLineHeight + 2));
    if (step < minStep) {
        step = static_cast<uint8_t>(max<int>(terminalFont().defaultLineStep, minStep));
    }
    return step;
}

void setUiFont()
{
    screenSprite.setFont(&fonts::AsciiFont8x16);
    screenSprite.setTextSize(1);
}

void setTerminalFont()
{
    screenSprite.setFont(terminalFont().japaneseFont);
    screenSprite.setTextSize(1);
}

int terminalCellWidth()
{
    return terminalFont().cellW;
}

int terminalFontHeight(bool japanese)
{
    if (!japanese) {
        return terminalFont().cellH;
    }
    screenSprite.setFont(terminalFont().japaneseFont);
    screenSprite.setTextSize(1);
    return screenSprite.fontHeight();
}

const TerminusBitmap::Font& terminalBitmapFont()
{
    return TerminusBitmap::fontForHeight(terminalFont().cellH);
}

bool fontSupportsCodepoint(const lgfx::IFont* font, uint32_t cp)
{
    if (!font || cp > 0xFFFF) {
        return false;
    }
    lgfx::FontMetrics metrics;
    font->getDefaultMetric(&metrics);
    return font->updateFontMetric(&metrics, static_cast<uint16_t>(cp));
}

const lgfx::IFont* fontForTerminalCodepoint(uint32_t cp)
{
    struct FontCacheEntry {
        uint32_t cp;
        const lgfx::IFont* fallback;
        const lgfx::IFont* resolved;
        bool valid;
    };
    static FontCacheEntry cache[96] = {};

    const lgfx::IFont* fallback = terminalFont().japaneseFont;
    FontCacheEntry& entry = cache[cp % (sizeof(cache) / sizeof(cache[0]))];
    if (entry.valid && entry.cp == cp && entry.fallback == fallback) {
        return entry.resolved;
    }

    const lgfx::IFont* resolved = nullptr;
    if (fontSupportsCodepoint(fallback, cp)) {
        resolved = fallback;
    }

    entry = {cp, fallback, resolved, true};
    return resolved;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

uint16_t terminalColor(uint32_t color, bool bold)
{
    static const uint16_t normal[] = {
        TFT_BLACK, TFT_MAROON, TFT_DARKGREEN, TFT_OLIVE,
        TFT_NAVY, TFT_PURPLE, TFT_DARKCYAN, TFT_LIGHTGREY,
        TFT_DARKGREY, TFT_RED, TFT_GREEN, TFT_YELLOW,
        TFT_BLUE, TFT_MAGENTA, TFT_CYAN, TFT_WHITE
    };
    if (color & 0x01000000UL) {
        return static_cast<uint16_t>(color & 0xFFFF);
    }
    if (color < 16) {
        uint8_t index = static_cast<uint8_t>(color & 0x0F);
        if (bold && index < 8) {
            index += 8;
        }
        return normal[index];
    }
    if (color >= 16 && color <= 231) {
        uint32_t c = color - 16;
        uint8_t r = static_cast<uint8_t>(c / 36);
        uint8_t g = static_cast<uint8_t>((c / 6) % 6);
        uint8_t b = static_cast<uint8_t>(c % 6);
        auto level = [](uint8_t v) -> uint8_t {
            return v == 0 ? 0 : static_cast<uint8_t>(55 + v * 40);
        };
        return rgb565(level(r), level(g), level(b));
    }
    if (color >= 232 && color <= 255) {
        uint8_t level = static_cast<uint8_t>(8 + (color - 232) * 10);
        return rgb565(level, level, level);
    }
    uint8_t index = static_cast<uint8_t>(color & 0x0F);
    if (bold && index < 8) {
        index += 8;
    }
    return normal[index];
}

void migrateLegacyLineStep()
{
    bool knownFont = false;
    for (const auto& option : TerminalFonts) {
        if (config.keyboard.terminalFont == option.id) {
            knownFont = true;
            break;
        }
    }
    if (!knownFont || config.keyboard.terminalFont.startsWith("efont")) {
        config.keyboard.terminalFont = "mono12";
    }
    const auto& font = terminalFont();
    int minStep = max(terminalFontHeight(false), terminalFontHeight(true));
    if (config.keyboard.terminalLineStep == font.legacyLineStep ||
        config.keyboard.terminalLineStep == font.previousLineStep ||
        config.keyboard.terminalLineStep < minStep) {
        config.keyboard.terminalLineStep = static_cast<uint8_t>(max<int>(font.defaultLineStep, minStep));
    }
}

void setSettingsFontForLine(const String& line = "")
{
    for (size_t i = 0; i < line.length(); ++i) {
        if (static_cast<uint8_t>(line[i]) >= 0x80) {
            screenSprite.setFont(terminalFont().japaneseFont);
            screenSprite.setTextSize(1);
            return;
        }
    }
    screenSprite.setFont(&fonts::AsciiFont8x16);
    screenSprite.setTextSize(1);
}

int settingRowH()
{
    return max<int>(44, terminalFont().settingsLineHeight + 12);
}

int settingListTop()
{
    if (screen == Screen::WifiEdit || screen == Screen::SshEdit || screen == Screen::ConfigEdit) {
        return HeaderH + 38;
    }
    return BodyBtn1.y + BodyBtn1.h + 18;
}

void drawSettingsTitle(const char* title)
{
    setSettingsFontForLine();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    screenSprite.drawString(title, 8, HeaderH + 8);
}

uint8_t utf8CharLength(uint8_t c)
{
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

uint32_t utf8Codepoint(const String& text)
{
    if (!text.length()) return 0;
    const uint8_t c0 = static_cast<uint8_t>(text[0]);
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && text.length() >= 2) {
        return ((c0 & 0x1F) << 6) | (static_cast<uint8_t>(text[1]) & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && text.length() >= 3) {
        return ((c0 & 0x0F) << 12) |
               ((static_cast<uint8_t>(text[1]) & 0x3F) << 6) |
               (static_cast<uint8_t>(text[2]) & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0 && text.length() >= 4) {
        return ((c0 & 0x07) << 18) |
               ((static_cast<uint8_t>(text[1]) & 0x3F) << 12) |
               ((static_cast<uint8_t>(text[2]) & 0x3F) << 6) |
               (static_cast<uint8_t>(text[3]) & 0x3F);
    }
    return c0;
}

uint32_t utf8CodepointAt(const String& text, size_t index)
{
    if (index >= text.length()) return 0;
    const uint8_t c0 = static_cast<uint8_t>(text[index]);
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && index + 1 < text.length()) {
        return ((c0 & 0x1F) << 6) | (static_cast<uint8_t>(text[index + 1]) & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && index + 2 < text.length()) {
        return ((c0 & 0x0F) << 12) |
               ((static_cast<uint8_t>(text[index + 1]) & 0x3F) << 6) |
               (static_cast<uint8_t>(text[index + 2]) & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0 && index + 3 < text.length()) {
        return ((c0 & 0x07) << 18) |
               ((static_cast<uint8_t>(text[index + 1]) & 0x3F) << 12) |
               ((static_cast<uint8_t>(text[index + 2]) & 0x3F) << 6) |
               (static_cast<uint8_t>(text[index + 3]) & 0x3F);
    }
    return c0;
}

bool isJapaneseTerminalCodepoint(uint32_t cp)
{
    return (cp >= 0x3000 && cp <= 0x30FF) ||
           (cp >= 0x31F0 && cp <= 0x31FF) ||
           (cp >= 0x3400 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

int terminalCodepointWidth(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 0x80 || TerminusBitmap::hasGlyph(terminalBitmapFont(), cp)) {
        return terminalCellWidth();
    }
    if (isJapaneseTerminalCodepoint(cp)) {
        return terminalCellWidth() * 2;
    }
    return terminalCellWidth();
}

int terminalTextWidth(const String& text)
{
    int width = 0;
    for (size_t i = 0; i < text.length();) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        uint32_t cp = utf8CodepointAt(text, i);
        width += terminalCodepointWidth(cp);
        i += utf8CharLength(c);
    }
    return width;
}

bool drawTerminusGlyph(uint32_t cp, int x, int lineTop, int lineStep, uint16_t fg)
{
    const auto& font = terminalBitmapFont();
    if (!TerminusBitmap::hasGlyph(font, cp)) {
        return false;
    }
    int y = lineTop + max<int>(0, (lineStep - font.targetH) / 2);
    return TerminusBitmap::drawGlyph(screenSprite, font, cp, x, y, fg);
}

int drawTerminalText(const String& text, int x, int lineTop, int lineStep, uint16_t fg, uint16_t bg)
{
    int cursorX = x;
    for (size_t i = 0; i < text.length();) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        size_t next = i + utf8CharLength(c);
        String glyph = text.substring(i, next);
        uint32_t cp = utf8CodepointAt(text, i);
        if (drawTerminusGlyph(cp, cursorX, lineTop, lineStep, fg)) {
            cursorX += terminalCellWidth();
        } else if (isJapaneseTerminalCodepoint(cp)) {
            screenSprite.setFont(terminalFont().japaneseFont);
            screenSprite.setTextSize(1);
            screenSprite.setTextColor(fg, bg);
            int textY = lineTop + max<int>(0, (lineStep - screenSprite.fontHeight()) / 2);
            screenSprite.drawString(glyph, cursorX, textY);
            cursorX += terminalCellWidth() * 2;
        } else if (drawFallbackUnicodeGlyph(cp, cursorX, lineTop, terminalCellWidth(), lineStep, fg, bg)) {
            cursorX += terminalCellWidth();
        } else {
            screenSprite.setFont(terminalFont().japaneseFont);
            screenSprite.setTextSize(1);
            screenSprite.setTextColor(fg, bg);
            int textY = lineTop + max<int>(0, (lineStep - screenSprite.fontHeight()) / 2);
            screenSprite.drawString(glyph, cursorX, textY);
            cursorX += terminalCellWidth();
        }
        i = next;
    }
    return cursorX - x;
}

void drawTightJapaneseRun(const String& text, int x, int y, int w, int lineStep, uint16_t fg, uint16_t bg)
{
    screenSprite.setClipRect(x, y, w, lineStep);
    screenSprite.setFont(terminalFont().japaneseFont);
    screenSprite.setTextSize(1);
    screenSprite.setTextColor(fg, bg);
    const int textY = y + max<int>(0, (lineStep - screenSprite.fontHeight()) / 2);
    const int tighten = max<int>(1, terminalCellWidth() / 8);
    int cursorX = x;
    for (size_t i = 0; i < text.length();) {
        size_t next = i + utf8CharLength(static_cast<uint8_t>(text[i]));
        String glyph = text.substring(i, next);
        screenSprite.drawString(glyph, cursorX, textY);
        cursorX += max<int>(1, screenSprite.textWidth(glyph) - tighten);
        i = next;
    }
    screenSprite.clearClipRect();
}

bool drawBlockGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg, uint16_t bg)
{
    if (cp == 0x2588 || cp == 0x2589 || cp == 0x258A || cp == 0x258B ||
        cp == 0x258C || cp == 0x258D || cp == 0x258E || cp == 0x258F) {
        int fillW = w;
        if (cp >= 0x2589) {
            fillW = max<int>(1, (w * static_cast<int>(0x2590 - cp)) / 8);
        }
        screenSprite.fillRect(x, y, fillW, h, fg);
        return true;
    }
    if (cp == 0x2580) {
        screenSprite.fillRect(x, y, w, max<int>(1, h / 2), fg);
        return true;
    }
    if (cp == 0x2584) {
        int half = max<int>(1, h / 2);
        screenSprite.fillRect(x, y + h - half, w, half, fg);
        return true;
    }
    if (cp == 0x258C) {
        screenSprite.fillRect(x, y, max<int>(1, w / 2), h, fg);
        return true;
    }
    if (cp == 0x2590) {
        screenSprite.fillRect(x + w / 2, y, max<int>(1, w - w / 2), h, fg);
        return true;
    }
    if (cp >= 0x2596 && cp <= 0x259F) {
        const int leftW = max<int>(1, w / 2);
        const int rightX = x + leftW;
        const int rightW = max<int>(1, w - leftW);
        const int topH = max<int>(1, h / 2);
        const int bottomY = y + topH;
        const int bottomH = max<int>(1, h - topH);
        auto quad = [&](bool tl, bool tr, bool bl, bool br) {
            if (tl) screenSprite.fillRect(x, y, leftW, topH, fg);
            if (tr) screenSprite.fillRect(rightX, y, rightW, topH, fg);
            if (bl) screenSprite.fillRect(x, bottomY, leftW, bottomH, fg);
            if (br) screenSprite.fillRect(rightX, bottomY, rightW, bottomH, fg);
        };
        switch (cp) {
            case 0x2596: quad(false, false, true, false); break;
            case 0x2597: quad(false, false, false, true); break;
            case 0x2598: quad(true, false, false, false); break;
            case 0x2599: quad(true, false, true, true); break;
            case 0x259A: quad(true, false, false, true); break;
            case 0x259B: quad(true, true, true, false); break;
            case 0x259C: quad(true, true, false, true); break;
            case 0x259D: quad(false, true, false, false); break;
            case 0x259E: quad(false, true, true, false); break;
            case 0x259F: quad(false, true, true, true); break;
        }
        return true;
    }
    if (cp == 0x2591 || cp == 0x2592 || cp == 0x2593) {
        int step = cp == 0x2591 ? 4 : (cp == 0x2592 ? 3 : 2);
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = (yy % step); xx < w; xx += step) {
                screenSprite.drawPixel(x + xx, y + yy, fg);
            }
        }
        return true;
    }
    (void)bg;
    return false;
}

bool drawBoxGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg)
{
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int thick = max<int>(1, min<int>(w, h) / 8);
    auto hline = [&](int x1, int x2) {
        screenSprite.fillRect(min(x1, x2), cy - thick / 2, abs(x2 - x1) + 1, thick, fg);
    };
    auto vline = [&](int y1, int y2) {
        screenSprite.fillRect(cx - thick / 2, min(y1, y2), thick, abs(y2 - y1) + 1, fg);
    };
    auto junction = [&](bool left, bool right, bool up, bool down) {
        if (left) hline(x, cx);
        if (right) hline(cx, x + w - 1);
        if (up) vline(y, cy);
        if (down) vline(cy, y + h - 1);
    };
    auto smoothCorner = [&](bool right, bool down) {
        const int insetX = max<int>(1, w / 4);
        const int insetY = max<int>(1, h / 4);
        const int x1 = right ? cx + insetX / 2 : cx - insetX / 2;
        const int x2 = right ? cx + insetX : cx - insetX;
        const int y1 = down ? cy + insetY / 2 : cy - insetY / 2;
        const int y2 = down ? cy + insetY : cy - insetY;
        screenSprite.fillRect(min(cx, x1), min(cy, y1), max<int>(1, abs(x1 - cx) + thick), thick, fg);
        screenSprite.fillRect(min(x1, x2), min(y1, y2), thick, max<int>(1, abs(y2 - y1) + thick), fg);
    };

    switch (cp) {
        case 0x2500: hline(x, x + w - 1); return true;
        case 0x2502: vline(y, y + h - 1); return true;
        case 0x256D: hline(cx, x + w - 1); vline(cy, y + h - 1); smoothCorner(true, true); return true;
        case 0x256E: hline(x, cx); vline(cy, y + h - 1); smoothCorner(false, true); return true;
        case 0x2570: hline(cx, x + w - 1); vline(y, cy); smoothCorner(true, false); return true;
        case 0x256F: hline(x, cx); vline(y, cy); smoothCorner(false, false); return true;
        case 0x250C: hline(cx, x + w - 1); vline(cy, y + h - 1); return true;
        case 0x2510: hline(x, cx); vline(cy, y + h - 1); return true;
        case 0x2514: hline(cx, x + w - 1); vline(y, cy); return true;
        case 0x2518: hline(x, cx); vline(y, cy); return true;
        case 0x251C: junction(false, true, true, true); return true;
        case 0x2524: junction(true, false, true, true); return true;
        case 0x252C: junction(true, true, false, true); return true;
        case 0x2534: junction(true, true, true, false); return true;
        case 0x253C: junction(true, true, true, true); return true;
        case 0x251D:
        case 0x251E:
        case 0x251F:
        case 0x2520:
        case 0x2521:
        case 0x2522:
        case 0x2523:
            junction(false, true, true, true); return true;
        case 0x2525:
        case 0x2526:
        case 0x2527:
        case 0x2528:
        case 0x2529:
        case 0x252A:
        case 0x252B:
            junction(true, false, true, true); return true;
        case 0x252D:
        case 0x252E:
        case 0x252F:
        case 0x2530:
        case 0x2531:
        case 0x2532:
        case 0x2533:
            junction(true, true, false, true); return true;
        case 0x2535:
        case 0x2536:
        case 0x2537:
        case 0x2538:
        case 0x2539:
        case 0x253A:
        case 0x253B:
            junction(true, true, true, false); return true;
        case 0x253D:
        case 0x253E:
        case 0x253F:
        case 0x2540:
        case 0x2541:
        case 0x2542:
        case 0x2543:
        case 0x2544:
        case 0x2545:
        case 0x2546:
        case 0x2547:
        case 0x2548:
        case 0x2549:
        case 0x254A:
        case 0x254B:
            junction(true, true, true, true); return true;
        case 0x2550:
            screenSprite.fillRect(x, cy - thick, w, thick, fg);
            screenSprite.fillRect(x, cy + thick, w, thick, fg);
            return true;
        case 0x2551:
            screenSprite.fillRect(cx - thick, y, thick, h, fg);
            screenSprite.fillRect(cx + thick, y, thick, h, fg);
            return true;
        case 0x2552:
        case 0x2553:
        case 0x2554:
            junction(false, true, false, true); return true;
        case 0x2555:
        case 0x2556:
        case 0x2557:
            junction(true, false, false, true); return true;
        case 0x2558:
        case 0x2559:
        case 0x255A:
            junction(false, true, true, false); return true;
        case 0x255B:
        case 0x255C:
        case 0x255D:
            junction(true, false, true, false); return true;
        case 0x255E:
        case 0x255F:
        case 0x2560:
            junction(false, true, true, true); return true;
        case 0x2561:
        case 0x2562:
        case 0x2563:
            junction(true, false, true, true); return true;
        case 0x2564:
        case 0x2565:
        case 0x2566:
            junction(true, true, false, true); return true;
        case 0x2567:
        case 0x2568:
        case 0x2569:
            junction(true, true, true, false); return true;
        case 0x256A:
        case 0x256B:
        case 0x256C:
            junction(true, true, true, true); return true;
        case 0x2571:
            screenSprite.drawLine(x, y + h - 1, x + w - 1, y, fg);
            return true;
        case 0x2572:
            screenSprite.drawLine(x, y, x + w - 1, y + h - 1, fg);
            return true;
        case 0x2573:
            screenSprite.drawLine(x, y + h - 1, x + w - 1, y, fg);
            screenSprite.drawLine(x, y, x + w - 1, y + h - 1, fg);
            return true;
        case 0x2574: hline(x, cx); return true;
        case 0x2575: vline(y, cy); return true;
        case 0x2576: hline(cx, x + w - 1); return true;
        case 0x2577: vline(cy, y + h - 1); return true;
        default:
            return false;
    }
}

bool isUnicodeBlank(uint32_t cp)
{
    return cp == 0x00A0 || cp == 0x1680 || cp == 0x180E ||
           (cp >= 0x2000 && cp <= 0x200F) ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF;
}

bool isFallbackSymbolRange(uint32_t cp)
{
    return (cp >= 0x2190 && cp <= 0x21FF) || // arrows
           (cp >= 0x2300 && cp <= 0x23FF) || // technical symbols
           (cp >= 0x2460 && cp <= 0x24FF) || // enclosed alphanumerics
           (cp >= 0x25A0 && cp <= 0x25FF) || // geometric shapes
           (cp >= 0x2600 && cp <= 0x27BF) || // misc symbols and dingbats
           (cp >= 0x27C0 && cp <= 0x27FF) || // supplemental arrows and brackets
           (cp >= 0x2800 && cp <= 0x28FF) || // braille patterns
           (cp >= 0xE000 && cp <= 0xF8FF) || // private-use icons, including Nerd Font
           (cp >= 0x1F000 && cp <= 0x1FAFF); // emoji and symbol planes
}

bool drawBrailleGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg)
{
    if (cp < 0x2800 || cp > 0x28FF) {
        return false;
    }
    const uint8_t bits = static_cast<uint8_t>(cp - 0x2800);
    const int dot = max<int>(1, min<int>(w, h) / 5);
    const int left = x + max<int>(1, w / 4 - dot / 2);
    const int right = x + max<int>(1, (w * 3) / 4 - dot / 2);
    const int top = y + max<int>(1, h / 8);
    const int gap = max<int>(dot + 1, (h - dot - 2) / 4);
    auto drawDot = [&](uint8_t bit, int dx, int dy) {
        if (bits & bit) {
            screenSprite.fillCircle(dx + dot / 2, dy + dot / 2, max<int>(1, dot / 2), fg);
        }
    };
    drawDot(0x01, left, top);
    drawDot(0x02, left, top + gap);
    drawDot(0x04, left, top + gap * 2);
    drawDot(0x40, left, top + gap * 3);
    drawDot(0x08, right, top);
    drawDot(0x10, right, top + gap);
    drawDot(0x20, right, top + gap * 2);
    drawDot(0x80, right, top + gap * 3);
    return true;
}

bool drawPowerlineGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg, uint16_t bg)
{
    switch (cp) {
        case 0xE0B0:
        case 0xE0B1:
            screenSprite.fillTriangle(x, y, x, y + h - 1, x + w - 1, y + h / 2, fg);
            if (cp == 0xE0B1) {
                screenSprite.fillTriangle(x + 2, y + 3, x + 2, y + h - 4, x + w - 4, y + h / 2, bg);
            }
            return true;
        case 0xE0B2:
        case 0xE0B3:
            screenSprite.fillTriangle(x + w - 1, y, x + w - 1, y + h - 1, x, y + h / 2, fg);
            if (cp == 0xE0B3) {
                screenSprite.fillTriangle(x + w - 3, y + 3, x + w - 3, y + h - 4, x + 3, y + h / 2, bg);
            }
            return true;
        default:
            return false;
    }
}

bool drawFallbackUnicodeGlyph(uint32_t cp, int x, int y, int w, int h, uint16_t fg, uint16_t bg)
{
    if (isUnicodeBlank(cp)) {
        return true;
    }
    if (drawBrailleGlyph(cp, x, y, w, h, fg) || drawPowerlineGlyph(cp, x, y, w, h, fg, bg)) {
        return true;
    }
    if (!isFallbackSymbolRange(cp)) {
        return false;
    }

    const int pad = max<int>(1, min<int>(w, h) / 8);
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int r = max<int>(2, min<int>(w, h) / 3);

    if (cp >= 0x2190 && cp <= 0x21FF) {
        bool drawn = false;
        if (cp == 0x2190 || cp == 0x2194) {
            screenSprite.drawLine(x + pad, cy, x + w - pad - 1, cy, fg);
            screenSprite.fillTriangle(x + pad, cy, x + pad + r / 2, cy - r / 2, x + pad + r / 2, cy + r / 2, fg);
            drawn = true;
        }
        if (cp == 0x2192 || cp == 0x2194 || cp == 0x21D2) {
            screenSprite.drawLine(x + pad, cy, x + w - pad - 1, cy, fg);
            screenSprite.fillTriangle(x + w - pad - 1, cy, x + w - pad - r / 2, cy - r / 2,
                                      x + w - pad - r / 2, cy + r / 2, fg);
            drawn = true;
        }
        if (cp == 0x2191 || cp == 0x2195) {
            screenSprite.drawLine(cx, y + pad, cx, y + h - pad - 1, fg);
            screenSprite.fillTriangle(cx, y + pad, cx - r / 2, y + pad + r / 2, cx + r / 2, y + pad + r / 2, fg);
            drawn = true;
        }
        if (cp == 0x2193 || cp == 0x2195) {
            screenSprite.drawLine(cx, y + pad, cx, y + h - pad - 1, fg);
            screenSprite.fillTriangle(cx, y + h - pad - 1, cx - r / 2, y + h - pad - r / 2,
                                      cx + r / 2, y + h - pad - r / 2, fg);
            drawn = true;
        }
        if (cp == 0x21B5) {
            screenSprite.drawLine(x + w - pad - 1, y + pad, x + w - pad - 1, cy, fg);
            screenSprite.drawLine(x + pad, cy, x + w - pad - 1, cy, fg);
            screenSprite.fillTriangle(x + pad, cy, x + pad + r / 2, cy - r / 2, x + pad + r / 2, cy + r / 2, fg);
            drawn = true;
        }
        if (!drawn) {
            screenSprite.drawLine(x + pad, cy, x + w - pad - 1, cy, fg);
            screenSprite.fillTriangle(x + w - pad - 1, cy, x + w - pad - r / 2, cy - r / 2,
                                      x + w - pad - r / 2, cy + r / 2, fg);
        }
        return true;
    }

    if (cp == 0x25CF || cp == 0x26AB || cp == 0x26AA || cp == 0x1F534 || cp == 0x1F535) {
        screenSprite.fillCircle(cx, cy, r, fg);
        return true;
    }
    if (cp == 0x25CB || cp == 0x25EF) {
        screenSprite.drawCircle(cx, cy, r, fg);
        return true;
    }
    if (cp == 0x25A0 || cp == 0x25A1 || cp == 0x25AA || cp == 0x25AB) {
        if (cp == 0x25A0 || cp == 0x25AA) {
            screenSprite.fillRect(x + pad, y + pad, w - pad * 2, h - pad * 2, fg);
        } else {
            screenSprite.drawRect(x + pad, y + pad, w - pad * 2, h - pad * 2, fg);
        }
        return true;
    }
    if (cp == 0x25B6 || cp == 0x25B8 || cp == 0x25BA || cp == 0x25C0 || cp == 0x25C2 || cp == 0x25C4) {
        if (cp == 0x25C0 || cp == 0x25C2 || cp == 0x25C4) {
            screenSprite.fillTriangle(x + pad, cy, x + w - pad - 1, y + pad, x + w - pad - 1, y + h - pad - 1, fg);
        } else {
            screenSprite.fillTriangle(x + w - pad - 1, cy, x + pad, y + pad, x + pad, y + h - pad - 1, fg);
        }
        return true;
    }
    if (cp == 0x276E || cp == 0x276F || cp == 0x276C || cp == 0x276D ||
        cp == 0x2770 || cp == 0x2771 || cp == 0x2772 || cp == 0x2773 ||
        cp == 0x2039 || cp == 0x203A) {
        const bool left = cp == 0x276E || cp == 0x276C || cp == 0x2770 || cp == 0x2772 || cp == 0x2039;
        const int thick = max<int>(1, min<int>(w, h) / 8);
        const int topY = y + pad;
        const int bottomY = y + h - pad - 1;
        const int innerX = left ? x + pad : x + w - pad - 1;
        const int outerX = left ? x + w - pad - 1 : x + pad;
        for (int t = 0; t < thick; ++t) {
            screenSprite.drawLine(outerX + (left ? -t : t), topY, innerX + (left ? t : -t), cy, fg);
            screenSprite.drawLine(innerX + (left ? t : -t), cy, outerX + (left ? -t : t), bottomY, fg);
        }
        return true;
    }
    if (cp == 0x2768 || cp == 0x2769 || cp == 0x276A || cp == 0x276B || cp == 0x2774 || cp == 0x2775) {
        const bool left = cp == 0x2768 || cp == 0x276A || cp == 0x2774;
        const int thick = max<int>(1, min<int>(w, h) / 10);
        const int topY = y + pad;
        const int bottomY = y + h - pad - 1;
        const int midY = cy;
        if (cp == 0x2774 || cp == 0x2775) {
            const int outerX = left ? x + w - pad - 1 : x + pad;
            const int innerX = left ? x + pad : x + w - pad - 1;
            for (int t = 0; t < thick; ++t) {
                screenSprite.drawLine(outerX, topY, innerX, topY + (midY - topY) / 2, fg);
                screenSprite.drawLine(innerX, topY + (midY - topY) / 2, innerX, midY - t, fg);
                screenSprite.drawLine(innerX, midY + t, innerX, midY + (bottomY - midY) / 2, fg);
                screenSprite.drawLine(innerX, midY + (bottomY - midY) / 2, outerX, bottomY, fg);
            }
        } else {
            const int outerX = left ? x + w - pad - 1 : x + pad;
            const int innerX = left ? x + pad : x + w - pad - 1;
            for (int t = 0; t < thick; ++t) {
                screenSprite.drawLine(outerX, topY, innerX + (left ? t : -t), midY, fg);
                screenSprite.drawLine(innerX + (left ? t : -t), midY, outerX, bottomY, fg);
            }
        }
        return true;
    }
    if ((cp >= 0x2794 && cp <= 0x27BF) || cp == 0x27F6 || cp == 0x27F8 || cp == 0x27FA) {
        const int tailX = x + pad;
        const int headX = x + w - pad - 1;
        const int headW = max<int>(3, w / 3);
        const int thick = max<int>(1, min<int>(w, h) / 8);
        screenSprite.fillRect(tailX, cy - thick / 2, max<int>(1, headX - tailX - headW / 2), thick, fg);
        screenSprite.fillTriangle(headX, cy, headX - headW, y + pad, headX - headW, y + h - pad - 1, fg);
        if (cp == 0x27F8 || cp == 0x27FA) {
            screenSprite.fillRect(tailX, cy + thick + 1, max<int>(1, headX - tailX - headW / 2), thick, fg);
        }
        return true;
    }

    if (cp >= 0x1F000 && cp <= 0x1FAFF) {
        screenSprite.drawRoundRect(x + pad, y + pad, w - pad * 2, h - pad * 2, max<int>(2, r / 2), fg);
        screenSprite.fillCircle(x + w / 3, y + h / 3, max<int>(1, r / 5), fg);
        screenSprite.fillCircle(x + (w * 2) / 3, y + h / 3, max<int>(1, r / 5), fg);
        screenSprite.drawLine(x + w / 3, y + (h * 2) / 3, x + (w * 2) / 3, y + (h * 2) / 3, fg);
        return true;
    }

    screenSprite.drawRect(x + pad, y + pad, w - pad * 2, h - pad * 2, fg);
    screenSprite.drawLine(x + pad, y + pad, x + w - pad - 1, y + h - pad - 1, fg);
    screenSprite.drawLine(x + w - pad - 1, y + pad, x + pad, y + h - pad - 1, fg);
    (void)bg;
    return true;
}

void drawMixedTerminalLine(const String& line, int x, int lineTop, int lineHeight)
{
    drawTerminalText(line, x, lineTop, lineHeight, TFT_GREEN, TFT_BLACK);
}

void drawVtTerminal()
{
    static bool lastViewingHistory = false;
    static size_t lastScrollbackOffset = 0;
    static bool lastCursorDrawn = false;
    static size_t lastCursorCol = 0;
    static size_t lastCursorRow = 0;
    const int cellW = terminalCellWidth();
    const int lineStep = terminalLineStep();
    const int top = terminalTop();
    const int gridRight = 4 + static_cast<int>(vt.columns()) * cellW;
    const int gridBottom = top + static_cast<int>(vt.rows()) * lineStep;
    const bool viewingHistory = vt.scrollbackOffset() > 0;
    const bool drawCursor = !viewingHistory && cursorVisible && vt.cursorVisible();
    int scrollDelta = 0;
    bool canShiftRows = false;

    if (viewingHistory && lastViewingHistory) {
        scrollDelta = static_cast<int>(vt.scrollbackOffset()) - static_cast<int>(lastScrollbackOffset);
        canShiftRows = scrollDelta != 0 && abs(scrollDelta) < static_cast<int>(vt.rows());
    }
    if (!viewingHistory && lastViewingHistory) {
        vt.markAllDirty();
    }

    if (gridRight < screenSprite.width()) {
        screenSprite.fillRect(gridRight, top, screenSprite.width() - gridRight,
                              max<int>(0, min<int>(screenSprite.height(), gridBottom) - top), TFT_BLACK);
    }
    if (gridBottom < screenSprite.height()) {
        screenSprite.fillRect(0, gridBottom, screenSprite.width(), screenSprite.height() - gridBottom, TFT_BLACK);
    }

    if (canShiftRows) {
        const int bodyH = max<int>(0, min<int>(screenSprite.height(), gridBottom) - top);
        screenSprite.setClipRect(0, top, screenSprite.width(), bodyH);
        screenSprite.scroll(0, scrollDelta * lineStep);
        screenSprite.clearClipRect();
    }

    auto drawRow = [&](size_t row, bool force) {
        int y = top + static_cast<int>(row) * lineStep;
        if (y >= screenSprite.height()) {
            return;
        }
        for (size_t col = 0; col < vt.columns();) {
            int x = 4 + static_cast<int>(col) * cellW;
            if (x >= screenSprite.width()) {
                break;
            }
            const auto& cell = vt.displayCell(col, row);
            bool cursor = drawCursor && col == vt.cursorColumn() && row == vt.cursorRow();
            bool staleCursor = lastCursorDrawn && col == lastCursorCol && row == lastCursorRow && !cursor;
            if (!force && !cell.dirty && !cursor && !staleCursor) {
                ++col;
                continue;
            }
            bool inverse = cell.inverse ^ cursor;
            uint16_t fg = terminalColor(cell.fg, cell.bold);
            uint16_t bg = terminalColor(cell.bg, false);
            if (inverse) {
                std::swap(fg, bg);
            }
            if (cell.continuation && !cursor) {
                ++col;
                continue;
            }
            const int drawW = cell.wide ? min<int>(cellW * 2, screenSprite.width() - x) : cellW;

            if (cell.wide && !cursor && cell.ch != " " && isJapaneseTerminalCodepoint(utf8Codepoint(cell.ch))) {
                size_t endCol = col;
                String runText;
                bool runDirty = false;
                while (endCol < vt.columns()) {
                    const auto& runCell = vt.displayCell(endCol, row);
                    bool runCursor = !viewingHistory && cursorVisible && endCol == vt.cursorColumn() &&
                                     row == vt.cursorRow();
                    if (runCursor || !runCell.wide || runCell.continuation || runCell.ch == " " ||
                        runCell.fg != cell.fg || runCell.bg != cell.bg || runCell.bold != cell.bold ||
                        runCell.inverse != cell.inverse || !isJapaneseTerminalCodepoint(utf8Codepoint(runCell.ch))) {
                        break;
                    }
                    runText += runCell.ch;
                    runDirty = runDirty || runCell.dirty;
                    endCol += 2;
                }
                if (endCol > col) {
                    if (force || runDirty) {
                        const int runW = min<int>((endCol - col) * cellW, screenSprite.width() - x);
                        screenSprite.fillRect(x, y, runW, lineStep, bg);
                        drawTightJapaneseRun(runText, x, y, runW, lineStep, fg, bg);
                    }
                    col = endCol;
                    continue;
                }
            }

            screenSprite.fillRect(x, y, drawW, lineStep, bg);
            if (cell.ch != " ") {
                uint32_t cp = utf8Codepoint(cell.ch);
                if (drawTerminusGlyph(cp, x, y, lineStep, fg)) {
                    ++col;
                    continue;
                }
                const lgfx::IFont* glyphFont = fontForTerminalCodepoint(cp);
                if (!glyphFont) {
                    if (drawBlockGlyph(cp, x, y, drawW, lineStep, fg, bg)) {
                        ++col;
                        continue;
                    }
                    if (drawBoxGlyph(cp, x, y, drawW, lineStep, fg)) {
                        ++col;
                        continue;
                    }
                    if (drawFallbackUnicodeGlyph(cp, x, y, drawW, lineStep, fg, bg)) {
                        ++col;
                        continue;
                    }
                    glyphFont = terminalFont().japaneseFont;
                }
                screenSprite.setFont(glyphFont);
                screenSprite.setTextSize(1);
                screenSprite.setTextColor(fg, bg);
                int textY = y + max<int>(0, (lineStep - screenSprite.fontHeight()) / 2);
                screenSprite.drawString(cell.ch, x, textY);
            }
            ++col;
        }
    };

    if (canShiftRows) {
        const int exposed = abs(scrollDelta);
        if (scrollDelta > 0) {
            for (int row = 0; row < exposed; ++row) {
                drawRow(static_cast<size_t>(row), true);
            }
        } else {
            const int rows = static_cast<int>(vt.rows());
            for (int row = max(0, rows - exposed); row < rows; ++row) {
                drawRow(static_cast<size_t>(row), true);
            }
        }
    } else {
        const bool force = viewingHistory;
        for (size_t row = 0; row < vt.rows(); ++row) {
            drawRow(row, force);
        }
    }
    setTerminalFont();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    vt.clearDirty();
    lastViewingHistory = viewingHistory;
    lastScrollbackOffset = vt.scrollbackOffset();
    lastCursorDrawn = drawCursor;
    lastCursorCol = vt.cursorColumn();
    lastCursorRow = vt.cursorRow();
}

void resetCommandEditor()
{
    commandLine = "";
    commandCursor = 0;
    commandHistoryIndex = commandHistory.size();
    remoteLineMode = false;
    cursorVisible = true;
    lastCursorBlink = millis();
}

void clampCommandCursor()
{
    if (commandCursor > commandLine.length()) {
        commandCursor = commandLine.length();
    }
}

void insertCommandText(const String& text)
{
    if (!text.length()) {
        return;
    }
    commandLine = commandLine.substring(0, commandCursor) + text + commandLine.substring(commandCursor);
    commandCursor += text.length();
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
}

void backspaceCommandText()
{
    if (commandCursor == 0 || !commandLine.length()) {
        return;
    }
    size_t pos = commandCursor - 1;
    while (pos > 0 && (static_cast<uint8_t>(commandLine[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    commandLine = commandLine.substring(0, pos) + commandLine.substring(commandCursor);
    commandCursor = pos;
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
}

void moveCommandCursor(int delta)
{
    if (delta < 0) {
        if (commandCursor == 0) {
            return;
        }
        --commandCursor;
        while (commandCursor > 0 && (static_cast<uint8_t>(commandLine[commandCursor]) & 0xC0) == 0x80) {
            --commandCursor;
        }
    } else if (delta > 0 && commandCursor < commandLine.length()) {
        commandCursor += utf8CharLength(static_cast<uint8_t>(commandLine[commandCursor]));
        clampCommandCursor();
    }
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
}

void browseCommandHistory(int delta)
{
    if (commandHistory.empty()) {
        return;
    }
    if (delta < 0) {
        if (commandHistoryIndex == 0) {
            return;
        }
        --commandHistoryIndex;
    } else {
        if (commandHistoryIndex >= commandHistory.size()) {
            return;
        }
        ++commandHistoryIndex;
    }
    if (commandHistoryIndex < commandHistory.size()) {
        commandLine = commandHistory[commandHistoryIndex];
    } else {
        commandLine = "";
    }
    commandCursor = commandLine.length();
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
}

void rememberCommandHistory(const String& line)
{
    if (!line.length()) {
        return;
    }
    if (commandHistory.empty() || commandHistory.back() != line) {
        commandHistory.push_back(line);
        if (commandHistory.size() > 50) {
            commandHistory.erase(commandHistory.begin());
        }
    }
    commandHistoryIndex = commandHistory.size();
}

void appendCliLine(const String& line)
{
    if (serialCliCapture) {
        Serial.print("CLI ");
        Serial.println(line);
    }
    terminal.append(line);
    terminal.append("\n");
}

void appendPythonCliLine(const String& line)
{
    appendCliLine(line);
}

void serialPythonLine(const String& line)
{
    Serial.println(line);
}

void appendCliHelp()
{
    appendCliLine("Tab5 CLI commands:");
    appendCliLine("  help, man <command>, clear, status, history");
    appendCliLine("  uname, whoami, hostname, date, uptime, echo");
    appendCliLine("  wifi status, wifi list, wifi off, wifi on, ip addr");
    appendCliLine("  ssh list, ssh connect <index>, ssh disconnect");
    appendCliLine("  ssh user@host[:port] [password]");
    appendCliLine("  sd status, df, ls [-lah] [path], cat <path>, chmod <mode> <path>");
    appendCliLine("  mkdir <path>, rmdir <path>, rm <path>, cp <src> <dst>");
    appendCliLine("  sd write <path> <text>");
    appendCliLine("  scp get <remote> <sd-local>, scp put <sd-local> <remote>");
    appendCliLine("  image <sd:/path|usb:/path> [fit|center|half|quarter]");
    appendCliLine("  python, python <sd.py> [args...], python -c <statement>");
    appendCliLine("  ble status, ble devices, ble enable, ble disable, ble scan, ble gapauto");
    appendCliLine("  ble pair <index>, ble disconnect [index|all], ble forget [index|all]");
}

void appendCliManEntry(const char* name, const char* synopsis, const char* description)
{
    appendCliLine(String("NAME"));
    appendCliLine(String("  ") + name);
    appendCliLine(String("SYNOPSIS"));
    appendCliLine(String("  ") + synopsis);
    appendCliLine(String("DESCRIPTION"));
    appendCliLine(String("  ") + description);
}

bool appendCliMan(const String& topic)
{
    String key = topic;
    key.trim();
    key.toLowerCase();
    while (key.indexOf("  ") >= 0) {
        key.replace("  ", " ");
    }
    if (!key.length() || key == "help" || key == "man") {
        appendCliManEntry("man", "man <command>", "Show help for Tab5 CLI built-in commands.");
        appendCliLine("Try: man clear, man date, man wifi, man ssh, man scp, man python");
        return true;
    }
    if (key == "clear" || key == "cls" || key == "reset") {
        appendCliManEntry("clear", "clear", "Clear the Tab5 CLI screen buffer.");
        return true;
    }
    if (key == "status") {
        appendCliManEntry("status", "status", "Show Wi-Fi, SSH, device, time, and active profile status.");
        return true;
    }
    if (key == "history") {
        appendCliManEntry("history", "history", "Show local Tab5 CLI command history.");
        return true;
    }
    if (key == "whoami" || key == "hostname") {
        appendCliManEntry(key.c_str(), key.c_str(), "Print the configured Tab5 device name.");
        return true;
    }
    if (key == "uname") {
        appendCliManEntry("uname", "uname [-a]", "Print Tab5 CLI firmware and platform information.");
        return true;
    }
    if (key == "date") {
        appendCliManEntry("date", "date", "Print NTP-synced local time using the configured region and UTC offset.");
        return true;
    }
    if (key == "time" || key == "time sync" || key == "ntp" || key == "ntp sync") {
        appendCliManEntry("time sync", "time sync | ntp sync", "Request network time synchronization over Wi-Fi.");
        return true;
    }
    if (key == "uptime") {
        appendCliManEntry("uptime", "uptime", "Print time elapsed since the firmware booted.");
        return true;
    }
    if (key == "echo") {
        appendCliManEntry("echo", "echo <text>", "Print text back to the Tab5 CLI screen.");
        return true;
    }
    if (key == "wifi" || key == "wifi status") {
        appendCliManEntry("wifi status", "wifi status | wifi off | wifi on", "Show or change Wi-Fi runtime state.");
        appendCliLine("EXAMPLES");
        appendCliLine("  wifi status");
        appendCliLine("  wifi off");
        appendCliLine("  wifi on");
        appendCliLine("SEE ALSO");
        appendCliLine("  wifi list");
        return true;
    }
    if (key == "wifi off") {
        appendCliManEntry("wifi off", "wifi off", "Turn off Wi-Fi runtime and stop retry/reconnect attempts.");
        return true;
    }
    if (key == "wifi on") {
        appendCliManEntry("wifi on", "wifi on", "Turn Wi-Fi runtime back on and start reconnecting to the active profile.");
        return true;
    }
    if (key == "wifi list") {
        appendCliManEntry("wifi list", "wifi list", "List saved Wi-Fi profiles and the active profile marker.");
        return true;
    }
    if (key == "ip" || key == "ip addr" || key == "ip a" || key == "ifconfig") {
        appendCliManEntry("ip addr", "ip addr | ip a | ifconfig", "Show the Tab5 wlan0 address and Wi-Fi state.");
        return true;
    }
    if (key == "ssh" || key == "ssh list") {
        appendCliManEntry("ssh list", "ssh list", "List saved SSH profiles and the active profile marker.");
        appendCliLine("EXAMPLES");
        appendCliLine("  ssh list");
        appendCliLine("  ssh connect 0");
        appendCliLine("  ssh demo@192.0.2.10");
        appendCliLine("SEE ALSO");
        appendCliLine("  ssh connect, ssh disconnect");
        return true;
    }
    if (key == "ssh connect") {
        appendCliManEntry("ssh connect", "ssh connect <index>", "Connect to a saved SSH profile by list index.");
        return true;
    }
    if (key == "ssh disconnect") {
        appendCliManEntry("ssh disconnect", "ssh disconnect", "Disconnect the active SSH session.");
        return true;
    }
    if (key == "ssh direct" || key == "ssh user@host" || key == "ssh user@host[:port]") {
        appendCliManEntry("ssh direct", "ssh user@host[:port] [password]", "Connect without a saved profile.");
        appendCliLine("EXAMPLES");
        appendCliLine("  ssh demo@192.0.2.10");
        appendCliLine("  ssh demo@192.0.2.10:22");
        return true;
    }
    if (key == "scp" || key == "scp get" || key == "scp put") {
        appendCliManEntry("scp", "scp get <remote> <sd-local> [profile] | scp put <sd-local> <remote> [profile]",
                          "Copy files between the active SSH profile and the Tab5 microSD card.");
        appendCliLine("Direct endpoints are supported: user@host:/path.");
        appendCliLine("DIRECTION");
        appendCliLine("  get: SSH server -> Tab5 microSD");
        appendCliLine("  put: Tab5 microSD -> SSH server");
        appendCliLine("EXAMPLES");
        appendCliLine("  scp get demo@192.0.2.10:/home/demo/test.py /test.py");
        appendCliLine("  scp put /test.py demo@192.0.2.10:/home/demo/test.py");
        appendCliLine("  scp get /home/demo/test.py /test.py 0");
        appendCliLine("  scp put /test.py /home/demo/test.py 0");
        appendCliLine("CHECK");
        appendCliLine("  ls -l /");
        appendCliLine("  cat /test.py");
        appendCliLine("  python /test.py");
        return true;
    }
    if (key == "image" || key == "img" || key == "view") {
        appendCliManEntry("image", "image <path|sd:/path|usb:/path> [fit|center|half|quarter]",
                          "Display a JPG, PNG, or BMP file on the Tab5 using M5GFX.");
        appendCliLine("EXAMPLES");
        appendCliLine("  image /photo.jpg");
        appendCliLine("  image sd:/photo.png fit");
        appendCliLine("  image usb:/wall.bmp half");
        return true;
    }
    if (key == "ble" || key == "ble status" || key == "ble devices" || key == "ble paired" ||
        key == "ble gapstatus" || key == "ble enable" || key == "ble disable" || key == "ble scan" ||
        key == "ble gapauto" || key == "ble scanpair" || key == "ble pair" || key == "ble disconnect" ||
        key == "ble forget") {
        appendCliManEntry("ble",
                          "ble status | ble devices | ble scan | ble gapauto | ble disconnect [index|all] | ble forget [index|all]",
                          "Manage BLE HID keyboards through the Tab5 ESP32-C6 wireless coprocessor.");
        appendCliLine("EXAMPLES");
        appendCliLine("  ble enable");
        appendCliLine("  ble status");
        appendCliLine("  ble devices");
        appendCliLine("  ble gapstatus");
        appendCliLine("  ble scan");
        appendCliLine("  ble pair 0");
        appendCliLine("  ble gapauto");
        appendCliLine("  ble disconnect all");
        appendCliLine("  ble forget 0");
        appendCliLine("NOTES");
        appendCliLine("  Put the keyboard in pairing mode before ble scan.");
        appendCliLine("  ble gapauto scans, connects, subscribes HID input, and stores the device.");
        appendCliLine("  ble devices shows saved devices and active runtime connections.");
        appendCliLine("  ble gapstatus shows low-level connection, service, and notification state.");
        return true;
    }
    if (key == "python" || key == "python file" || key == "python -c") {
        appendCliManEntry("python", "python | python <sd.py> [args...] | python -c <statement>",
                          "Run the built-in MicroPython-compatible subset with SD scripts and GPIO helpers.");
        appendCliLine("Examples:");
        appendCliLine("  python");
        appendCliLine("  python test.py 50");
        appendCliLine("  python /scripts/blink.py");
        appendCliLine("  python -c print('hello')");
        return true;
    }
    if (key == "sd" || key == "sd status") {
        appendCliManEntry("sd status", "sd status | df | sd df", "Show microSD mount state, size, used, available space, and current directory.");
        appendCliLine("EXAMPLES");
        appendCliLine("  sd status");
        appendCliLine("  df");
        appendCliLine("SEE ALSO");
        appendCliLine("  sd ls, sd cat, sd write, sd append, mkdir, rmdir, sd rm");
        return true;
    }
    if (key == "df" || key == "sd df" || key == "free" || key == "sd free") {
        appendCliManEntry("df", "df | sd df", "Show microSD size, used space, available space, and mount point.");
        appendCliLine("EXAMPLES");
        appendCliLine("  df");
        appendCliLine("  sd df");
        return true;
    }
    if (key == "ls" || key == "dir" || key == "sd ls") {
        appendCliManEntry("sd ls", "ls [-lah] [path] | sd ls [-lah] [path]", "List files on the Tab5 microSD card.");
        appendCliLine("Options: -l long format, -a show dotfiles, -h human-readable sizes.");
        appendCliLine("EXAMPLES");
        appendCliLine("  ls");
        appendCliLine("  ls -lah /");
        appendCliLine("  sd ls /scripts");
        return true;
    }
    if (key == "chmod" || key == "sd chmod") {
        appendCliManEntry("chmod", "chmod <mode> <path> | sd chmod <mode> <path>", "Set Tab5 virtual permission bits for SD files.");
        appendCliLine("EXAMPLES");
        appendCliLine("  chmod 644 /test.py");
        appendCliLine("  chmod 755 /scripts");
        return true;
    }
    if (key == "cd" || key == "pwd" || key == "cat" || key == "sd cat") {
        appendCliManEntry(key.c_str(), key.c_str(), "Operate on the Tab5 microSD card.");
        appendCliLine("Examples: pwd, cd /scripts, cat boot.py");
        return true;
    }
    if (key == "sd write" || key == "sd append") {
        appendCliManEntry(key.c_str(), "sd write <path> <text> | sd append <path> <text>", "Write or append one line of text to a file on microSD.");
        appendCliLine("EXAMPLES");
        appendCliLine("  sd write /hello.py print(123)");
        appendCliLine("  sd append /notes.txt more text");
        return true;
    }
    if (key == "mkdir" || key == "sd mkdir") {
        appendCliManEntry("mkdir", "mkdir <path> | sd mkdir <path>", "Create a directory on microSD.");
        appendCliLine("EXAMPLES");
        appendCliLine("  mkdir /scripts");
        appendCliLine("  sd mkdir /logs");
        return true;
    }
    if (key == "rmdir" || key == "sd rmdir") {
        appendCliManEntry("rmdir", "rmdir <path> | sd rmdir <path>", "Remove an empty directory from microSD.");
        appendCliLine("EXAMPLES");
        appendCliLine("  rmdir /scripts");
        appendCliLine("  sd rmdir /logs");
        return true;
    }
    if (key == "rm" || key == "sd rm") {
        appendCliManEntry("rm", "sd rm <path>", "Remove a file from microSD.");
        appendCliLine("EXAMPLES");
        appendCliLine("  sd rm /old.py");
        return true;
    }
    appendCliLine(String("No manual entry for ") + topic);
    appendCliLine("type 'help' to list Tab5 CLI commands");
    return true;
}

void appendWifiList()
{
    if (config.wifi.empty()) {
        appendCliLine("no Wi-Fi profiles");
        return;
    }
    for (size_t i = 0; i < config.wifi.size(); ++i) {
        appendCliLine(String(i == activeWifi ? "* " : "  ") + i + " " + config.wifi[i].name + " " + config.wifi[i].ssid);
    }
}

void appendSshList()
{
    if (config.ssh.empty()) {
        appendCliLine("no SSH profiles");
        return;
    }
    for (size_t i = 0; i < config.ssh.size(); ++i) {
        const auto& p = config.ssh[i];
        appendCliLine(String(i == activeSsh ? "* " : "  ") + i + " " + p.name + " " + p.user + "@" + p.host + ":" + p.port);
    }
}

String normalizeSdPath(const String& input)
{
    String path = input;
    path.trim();
    if (!path.length()) {
        path = sdCwd;
        if (path == "/sd") {
            path = "/";
        } else if (path.startsWith("/sd/")) {
            path = path.substring(3);
        } else if (path.startsWith("/usb") || path.startsWith("/flash")) {
            path = "/";
        }
    } else if (!path.startsWith("/")) {
        path = sdCwd;
        if (path == "/sd") {
            path = "/";
        } else if (path.startsWith("/sd/")) {
            path = path.substring(3);
        } else if (path.startsWith("/usb") || path.startsWith("/flash")) {
            path = "/";
        }
        if (!path.endsWith("/")) {
            path += "/";
        }
        path += input;
    } else if (path == "/sd") {
        path = "/";
    } else if (path.startsWith("/sd/")) {
        path = path.substring(3);
    }
    while (path.indexOf("//") >= 0) {
        path.replace("//", "/");
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    std::vector<String> parts;
    int start = 1;
    while (start <= static_cast<int>(path.length())) {
        int slash = path.indexOf('/', start);
        String part = slash >= 0 ? path.substring(start, slash) : path.substring(start);
        if (part.length() && part != ".") {
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else {
                parts.push_back(part);
            }
        }
        if (slash < 0) {
            break;
        }
        start = slash + 1;
    }

    String normalized = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            normalized += "/";
        }
        normalized += parts[i];
    }
    return normalized;
}

String normalizeVirtualPathText(const String& input)
{
    String path = input;
    path.trim();
    if (!path.length()) {
        path = sdCwd;
    } else if (!path.startsWith("/")) {
        path = sdCwd;
        if (!path.endsWith("/")) {
            path += "/";
        }
        path += input;
    }
    while (path.indexOf("//") >= 0) {
        path.replace("//", "/");
    }
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    std::vector<String> parts;
    int start = 1;
    while (start <= static_cast<int>(path.length())) {
        int slash = path.indexOf('/', start);
        String part = slash >= 0 ? path.substring(start, slash) : path.substring(start);
        if (part.length() && part != ".") {
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else {
                parts.push_back(part);
            }
        }
        if (slash < 0) {
            break;
        }
        start = slash + 1;
    }

    String normalized = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            normalized += "/";
        }
        normalized += parts[i];
    }
    return normalized;
}

VirtualPath resolveVirtualPath(const String& input)
{
    VirtualPath result;
    result.virtualPath = normalizeVirtualPathText(input);
    if (result.virtualPath == "/") {
        result.volume = VirtualVolume::Root;
        result.localPath = "/";
    } else if (result.virtualPath == "/sd" || result.virtualPath.startsWith("/sd/")) {
        result.volume = VirtualVolume::Sd;
        result.localPath = result.virtualPath.length() == 3 ? "/" : result.virtualPath.substring(3);
    } else if (result.virtualPath == "/usb" || result.virtualPath.startsWith("/usb/")) {
        result.volume = VirtualVolume::Usb;
        result.localPath = result.virtualPath.length() == 4 ? "/" : result.virtualPath.substring(4);
    } else if (result.virtualPath == "/flash" || result.virtualPath.startsWith("/flash/")) {
        result.volume = VirtualVolume::Flash;
        result.localPath = result.virtualPath.length() == 6 ? "/" : result.virtualPath.substring(6);
    } else {
        result.volume = VirtualVolume::Invalid;
    }
    if (!result.localPath.length()) {
        result.localPath = "/";
    }
    return result;
}

String localShellPrompt()
{
    String path = normalizeVirtualPathText(sdCwd);
    if (path.length() > 1 && path.endsWith("/")) {
        path.remove(path.length() - 1);
    }
    return String("tab5:") + path + "$ ";
}

void splitPythonScriptCommand(const String& rest, String& pathArg, String& args)
{
    String trimmed = rest;
    trimmed.trim();
    int split = trimmed.indexOf(' ');
    if (split < 0) {
        pathArg = trimmed;
        args = "";
        return;
    }
    pathArg = trimmed.substring(0, split);
    args = trimmed.substring(split + 1);
    args.trim();
}

bool ensureSdReady()
{
    if (sdReady) {
        return true;
    }
    if (!sdInitAttempted) {
        sdInitAttempted = true;
        SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
        if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
            sdReady = true;
            sdLastError = "";
            return true;
        }
        sdLastError = "SD card not detected";
    }
    return false;
}

#if ENABLE_USB_HOST_KEYBOARD
bool usbMscComplete(uint8_t, const tuh_msc_complete_data_t* data)
{
    usbMscOk = data && data->csw && data->csw->status == 0;
    usbMscDone = true;
    return true;
}

bool waitUsbMsc(uint32_t timeoutMs = 3000)
{
    uint32_t start = millis();
    while (!usbMscDone && millis() - start < timeoutMs) {
        tuh_task();
        delay(1);
    }
    return usbMscDone && usbMscOk;
}

DSTATUS usbMscDiskStatus(BYTE)
{
    return usbMscPresent && tuh_msc_mounted(usbMscDevAddr) ? 0 : STA_NOINIT;
}

DSTATUS usbMscDiskInit(BYTE pdrv)
{
    return usbMscDiskStatus(pdrv);
}

DRESULT usbMscDiskRead(BYTE, BYTE* buff, DWORD sector, UINT count)
{
    if (!usbMscPresent || !tuh_msc_mounted(usbMscDevAddr) || !buff || !count) {
        return RES_ERROR;
    }
    uint32_t blockSize = tuh_msc_get_block_size(usbMscDevAddr, usbMscLun);
    if (!blockSize || blockSize > 4096) {
        return RES_ERROR;
    }
    if (!usbMscSectorBuffer) {
        usbMscSectorBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(32, 4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (!usbMscSectorBuffer) {
        return RES_ERROR;
    }
    for (UINT i = 0; i < count; ++i) {
        usbMscDone = false;
        usbMscOk = false;
        if (!tuh_msc_read10(usbMscDevAddr, usbMscLun, usbMscSectorBuffer, sector + i, 1, usbMscComplete, 0) ||
            !waitUsbMsc()) {
            return RES_ERROR;
        }
        memcpy(buff + i * blockSize, usbMscSectorBuffer, blockSize);
    }
    return RES_OK;
}

DRESULT usbMscDiskWrite(BYTE, const BYTE* buff, DWORD sector, UINT count)
{
    if (!usbMscPresent || !tuh_msc_mounted(usbMscDevAddr) || !buff || !count) {
        return RES_ERROR;
    }
    uint32_t blockSize = tuh_msc_get_block_size(usbMscDevAddr, usbMscLun);
    if (!blockSize || blockSize > 4096) {
        return RES_ERROR;
    }
    if (!usbMscSectorBuffer) {
        usbMscSectorBuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(32, 4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (!usbMscSectorBuffer) {
        return RES_ERROR;
    }
    for (UINT i = 0; i < count; ++i) {
        memcpy(usbMscSectorBuffer, buff + i * blockSize, blockSize);
        usbMscDone = false;
        usbMscOk = false;
        if (!tuh_msc_write10(usbMscDevAddr, usbMscLun, usbMscSectorBuffer, sector + i, 1, usbMscComplete, 0) ||
            !waitUsbMsc()) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT usbMscDiskIoctl(BYTE, BYTE cmd, void* buff)
{
    if (!usbMscPresent || !tuh_msc_mounted(usbMscDevAddr)) {
        return RES_ERROR;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *static_cast<DWORD*>(buff) = tuh_msc_get_block_count(usbMscDevAddr, usbMscLun);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *static_cast<WORD*>(buff) = static_cast<WORD>(tuh_msc_get_block_size(usbMscDevAddr, usbMscLun));
        return RES_OK;
    case GET_BLOCK_SIZE:
        *static_cast<DWORD*>(buff) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

const ff_diskio_impl_t UsbMscDisk = {
    .init = usbMscDiskInit,
    .status = usbMscDiskStatus,
    .read = usbMscDiskRead,
    .write = usbMscDiskWrite,
    .ioctl = usbMscDiskIoctl,
};

String usbFatPath(const String& path)
{
    String normalized = path;
    if (!normalized.startsWith("/")) {
        normalized = "/" + normalized;
    }
    return String(static_cast<unsigned>(usbMscPdrv)) + ":" + normalized;
}

bool usbDirectoryIsEmpty(const String& path)
{
    FF_DIR dir{};
    FILINFO info{};
    if (f_opendir(&dir, usbFatPath(path).c_str()) != FR_OK) {
        return false;
    }
    bool empty = true;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        String name(info.fname);
        if (name != "." && name != "..") {
            empty = false;
            break;
        }
    }
    f_closedir(&dir);
    return empty;
}

bool removeUsbDirectoryEntry(const String& path)
{
    String normalized = normalizeSdPath(path);
    if (normalized == "/") {
        return false;
    }
    FILINFO info{};
    if (f_stat(usbFatPath(normalized).c_str(), &info) != FR_OK || !(info.fattrib & AM_DIR)) {
        return false;
    }
    if (!usbDirectoryIsEmpty(normalized)) {
        return false;
    }
    String base = basenameOnly(normalized);
    for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        String target = String("/.tab5-deleted-") + millis() + "-" + attempt + "-" + base;
        if (f_stat(usbFatPath(target).c_str(), &info) == FR_OK) {
            continue;
        }
        if (f_rename(usbFatPath(normalized).c_str(), usbFatPath(target).c_str()) == FR_OK) {
            return true;
        }
    }
    return false;
}

bool tab5EgpioWriteBit(uint8_t address, uint8_t reg, uint8_t bit, bool value)
{
    tab5SystemI2c->beginTransmission(address);
    tab5SystemI2c->write(reg);
    if (tab5SystemI2c->endTransmission(false) != 0) {
        return false;
    }
    uint8_t read = tab5SystemI2c->requestFrom(address, static_cast<uint8_t>(1));
    if (read != 1 || !tab5SystemI2c->available()) {
        return false;
    }
    uint8_t current = tab5SystemI2c->read();
    uint8_t updated = value ? (current | (1U << bit)) : (current & ~(1U << bit));
    tab5SystemI2c->beginTransmission(address);
    tab5SystemI2c->write(reg);
    tab5SystemI2c->write(updated);
    return tab5SystemI2c->endTransmission() == 0;
}

bool tab5Usb5vSet(bool enabled)
{
    constexpr uint8_t E2Address = 0x44;
    constexpr uint8_t IoDirection = 0x03;
    constexpr uint8_t OutputState = 0x05;
    constexpr uint8_t OutputImpedance = 0x07;
    constexpr uint8_t Usb5vPin = 3;
    if (!tab5SystemI2cStarted) {
        tab5SystemI2cStarted = tab5SystemI2c->begin(31, 32, 400000);
    }
    if (!tab5SystemI2cStarted) {
        tab5UsbPowerStatus = "system i2c start failed";
        return false;
    }
    if (!tab5EgpioWriteBit(E2Address, OutputImpedance, Usb5vPin, false) ||
        !tab5EgpioWriteBit(E2Address, OutputState, Usb5vPin, enabled) ||
        !tab5EgpioWriteBit(E2Address, IoDirection, Usb5vPin, true)) {
        tab5UsbPowerStatus = enabled ? "E2 usb 5v enable failed" : "E2 usb 5v disable failed";
        tab5Usb5vEnabled = false;
        return false;
    }
    tab5Usb5vEnabled = enabled;
    tab5UsbPowerStatus = enabled ? "USB-A 5V on" : "USB-A 5V off";
    return true;
}

bool tab5Usb5vOn()
{
    return tab5Usb5vSet(true);
}

bool ensureUsbReady(bool quiet)
{
    if (!usbMscPresent) {
        tuh_task();
    }
    if (usbMscMounted) {
        return true;
    }
    if (!usbMscPresent && tab5Usb5vEnabled) {
        uint32_t start = millis();
        while (!usbMscPresent && millis() - start < 2500) {
            tuh_task();
            delay(5);
        }
    }
    if (usbMscPresent && tuh_msc_mounted(usbMscDevAddr)) {
        if (usbMscPdrv == FF_DRV_NOT_USED && ff_diskio_get_drive(&usbMscPdrv) != ESP_OK) {
            usbMscStatus = "no free fatfs drive";
            if (!quiet) {
                Serial.printf("ERR volume usb %s\r\n", usbMscStatus.c_str());
            }
            return false;
        }
        ff_diskio_register(usbMscPdrv, &UsbMscDisk);
        uint32_t start = millis();
        while (!tuh_msc_ready(usbMscDevAddr) && millis() - start < 3000) {
            tuh_task();
            delay(10);
        }
        bool unitReady = false;
        start = millis();
        while (millis() - start < 5000) {
            if (tuh_msc_ready(usbMscDevAddr)) {
                usbMscDone = false;
                usbMscOk = false;
                if (tuh_msc_test_unit_ready(usbMscDevAddr, usbMscLun, usbMscComplete, 0) && waitUsbMsc(1000)) {
                    unitReady = true;
                    break;
                }
            }
            delay(100);
        }
        if (!unitReady) {
            usbMscStatus = "scsi unit not ready";
            if (!quiet) {
                Serial.printf("ERR volume usb %s\r\n", usbMscStatus.c_str());
            }
            return false;
        }
        FRESULT result = f_mount(&usbMscFatfs, (String(static_cast<unsigned>(usbMscPdrv)) + ":").c_str(), 1);
        if (result == FR_OK) {
            usbMscMounted = true;
            usbMscStatus = String("ready dev=") + usbMscDevAddr +
                           " blocks=" + tuh_msc_get_block_count(usbMscDevAddr, usbMscLun) +
                           " blockSize=" + tuh_msc_get_block_size(usbMscDevAddr, usbMscLun);
            return true;
        }
        usbMscStatus = String("fatfs mount failed ") + static_cast<int>(result);
    }
    if (!quiet) {
        Serial.printf("ERR volume usb %s\r\n", usbMscStatus.c_str());
    }
    return false;
}

extern "C" void tuh_msc_mount_cb(uint8_t dev_addr)
{
    usbLastEventMs = millis();
    usbMscDevAddr = dev_addr;
    usbMscLun = 0;
    usbDevicePresent = true;
    usbMscPresent = true;
    usbMscMounted = false;
    usbHostStatus = String("msc dev=") + dev_addr;
    usbMscStatus = String("present dev=") + dev_addr;
    Serial.printf("[tab5] USB MSC mounted callback dev=%u\r\n", static_cast<unsigned>(dev_addr));
}

extern "C" void tuh_msc_umount_cb(uint8_t)
{
    usbLastEventMs = millis();
    if (usbMscPdrv != FF_DRV_NOT_USED) {
        f_mount(nullptr, (String(static_cast<unsigned>(usbMscPdrv)) + ":").c_str(), 0);
        ff_diskio_unregister(usbMscPdrv);
        usbMscPdrv = FF_DRV_NOT_USED;
    }
    usbMscMounted = false;
    usbMscPresent = false;
    usbMscDevAddr = 0;
    usbHostStatus = "no msc";
    usbMscStatus = "not mounted";
    Serial.println("[tab5] USB MSC unmounted callback");
}

extern "C" void tuh_mount_cb(uint8_t dev_addr)
{
    usbLastEventMs = millis();
    usbDevicePresent = true;
    usbHostStatus = String("device dev=") + dev_addr;
    Serial.printf("[tab5] USB device mounted callback dev=%u\r\n", static_cast<unsigned>(dev_addr));
}

extern "C" void tuh_umount_cb(uint8_t dev_addr)
{
    usbLastEventMs = millis();
    usbDevicePresent = false;
    usbMscPresent = false;
    usbMscMounted = false;
    usbHostStatus = "no device";
    usbMscStatus = "not mounted";
    Serial.printf("[tab5] USB device unmounted callback dev=%u\r\n", static_cast<unsigned>(dev_addr));
}
#endif

String formatBytes(uint64_t bytes)
{
    if (bytes < 1024) {
        return String(static_cast<unsigned long>(bytes)) + " B";
    }
    if (bytes < 1024ULL * 1024ULL) {
        return String(static_cast<double>(bytes) / 1024.0, 1) + " KB";
    }
    return String(static_cast<double>(bytes) / (1024.0 * 1024.0), 1) + " MB";
}

String formatLsTime(time_t t)
{
    if (t <= 0) {
        return "Jan  1  1980";
    }
    struct tm tmValue;
    localtime_r(&t, &tmValue);
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %2d %02d:%02d",
             months[constrain(tmValue.tm_mon, 0, 11)], tmValue.tm_mday,
             tmValue.tm_hour, tmValue.tm_min);
    return String(buf);
}

String joinSdPath(const String& dir, const String& name)
{
    if (dir == "/") {
        return "/" + name;
    }
    return dir + "/" + name;
}

uint16_t defaultSdMode(bool directory)
{
    return directory ? 0755 : 0644;
}

int findSdModeIndex(const String& path)
{
    for (size_t i = 0; i < sdModes.size(); ++i) {
        if (sdModes[i].path == path) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void loadSdModes()
{
    if (sdModesLoaded || !sdReady) {
        return;
    }
    sdModesLoaded = true;
    sdModes.clear();
    File file = SD.open("/.tab5perms", FILE_READ);
    if (!file) {
        return;
    }
    String line;
    while (file.available()) {
        char c = static_cast<char>(file.read());
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            line += c;
            continue;
        }
        int split = line.indexOf(' ');
        if (split > 0) {
            SdModeEntry entry;
            entry.mode = static_cast<uint16_t>(strtoul(line.substring(0, split).c_str(), nullptr, 8) & 0777);
            entry.path = line.substring(split + 1);
            if (entry.path.length()) {
                sdModes.push_back(entry);
            }
        }
        line = "";
    }
    file.close();
}

void saveSdModes()
{
    if (!sdReady) {
        return;
    }
    if (SD.exists("/.tab5perms")) {
        SD.remove("/.tab5perms");
    }
    File file = SD.open("/.tab5perms", FILE_APPEND);
    if (!file) {
        return;
    }
    char modeText[8];
    for (const auto& entry : sdModes) {
        snprintf(modeText, sizeof(modeText), "%03o", entry.mode & 0777);
        file.print(modeText);
        file.print(" ");
        file.print(entry.path);
        file.print("\n");
    }
    file.close();
}

uint16_t sdModeForPath(const String& path, bool directory)
{
    loadSdModes();
    int index = findSdModeIndex(path);
    return index >= 0 ? sdModes[index].mode : defaultSdMode(directory);
}

String modeString(uint16_t mode, bool directory)
{
    String out = directory ? "d" : "-";
    const uint16_t bits[] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    const char chars[] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (size_t i = 0; i < 9; ++i) {
        out += (mode & bits[i]) ? chars[i] : '-';
    }
    return out;
}

bool parseOctalMode(const String& text, uint16_t& mode)
{
    if (!text.length() || text.length() > 4) {
        return false;
    }
    uint16_t value = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c < '0' || c > '7') {
            return false;
        }
        value = static_cast<uint16_t>((value << 3) + (c - '0'));
    }
    mode = value & 0777;
    return true;
}

void setSdModeForPath(const String& path, uint16_t mode)
{
    loadSdModes();
    int index = findSdModeIndex(path);
    if (index >= 0) {
        sdModes[index].mode = mode;
    } else {
        sdModes.push_back({path, mode});
    }
    saveSdModes();
}

bool sdPathHasWritePermission(const String& path)
{
    File file = SD.open(path, FILE_READ);
    bool directory = file && file.isDirectory();
    if (file) {
        file.close();
    }
    return (sdModeForPath(path, directory) & 0200) != 0;
}

bool sdPathHasExecutePermission(const String& path)
{
    return (sdModeForPath(path, true) & 0100) != 0;
}

bool sdPathHasReadPermission(const String& path)
{
    File file = SD.open(path, FILE_READ);
    bool directory = file && file.isDirectory();
    if (file) {
        file.close();
    }
    return (sdModeForPath(path, directory) & 0400) != 0;
}

bool parseLsOptions(const String& input, LsOptions& options, String& error)
{
    options = LsOptions{};
    error = "";
    String rest = input;
    rest.trim();
    while (rest.length()) {
        int split = rest.indexOf(' ');
        String token = split < 0 ? rest : rest.substring(0, split);
        rest = split < 0 ? "" : rest.substring(split + 1);
        rest.trim();
        if (!token.length()) {
            continue;
        }
        if (token == "--") {
            if (rest.length()) {
                if (options.path.length()) {
                    error = "ls: multiple paths are not supported";
                    return false;
                }
                options.path = rest;
            }
            break;
        }
        if (token.length() > 1 && token[0] == '-') {
            for (size_t i = 1; i < token.length(); ++i) {
                char opt = token[i];
                if (opt == 'l') {
                    options.longFormat = true;
                } else if (opt == 'a') {
                    options.all = true;
                } else if (opt == 'h') {
                    options.human = true;
                } else {
                    error = String("ls: unsupported option -- ") + opt;
                    return false;
                }
            }
            continue;
        }
        if (options.path.length()) {
            error = "ls: multiple paths are not supported";
            return false;
        }
        options.path = token;
    }
    return true;
}

String basenameOnly(const String& path);

bool globPatternMatches(const String& pattern, const String& name)
{
    size_t p = 0;
    size_t n = 0;
    int star = -1;
    size_t match = 0;
    while (n < name.length()) {
        if (p < pattern.length() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.length() && pattern[p] == '*') {
            star = static_cast<int>(p++);
            match = n;
        } else if (star >= 0) {
            p = static_cast<size_t>(star + 1);
            n = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.length() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.length();
}

bool pathHasGlob(const String& path)
{
    return path.indexOf('*') >= 0 || path.indexOf('?') >= 0;
}

String pathDirPart(const String& path)
{
    int slash = path.lastIndexOf('/');
    if (slash <= 0) {
        return "/";
    }
    return path.substring(0, slash);
}

String pathBasePart(const String& path)
{
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

String lsSizeText(uint64_t size, const LsOptions& options)
{
    char text[24];
    if (options.human) {
        snprintf(text, sizeof(text), "%8s", formatBytes(size).c_str());
    } else {
        snprintf(text, sizeof(text), "%8llu", static_cast<unsigned long long>(size));
    }
    return String(text);
}

String lsDisplayLine(File& file, const String& name, const String& path, const LsOptions& options)
{
    if (!options.longFormat) {
        return name + (file.isDirectory() ? "/" : "");
    }
    return modeString(sdModeForPath(path, file.isDirectory()), file.isDirectory()) +
           " 1 tab5 tab5 " + lsSizeText(file.isDirectory() ? 0 : file.size(), options) + " " +
           formatLsTime(file.getLastWrite()) + " " +
           name + (file.isDirectory() ? "/" : "");
}

void sortLsNames(std::vector<String>& names)
{
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) {
        return strcmp(a.c_str(), b.c_str()) < 0;
    });
}

std::vector<String> collectLsNames(File& root, const String& path, const LsOptions& options)
{
    std::vector<String> names;
    File file = root.openNextFile();
    while (file) {
        String displayName = basenameOnly(file.name());
        if (options.all || !displayName.startsWith(".")) {
            names.push_back(displayName + (file.isDirectory() ? "/" : ""));
        }
        file.close();
        file = root.openNextFile();
    }
    sortLsNames(names);
    return names;
}

String lsColumnLine(const std::vector<String>& names, size_t first, size_t rowCount, size_t columnWidth, size_t columns)
{
    String line;
    for (size_t col = 0; col < columns; ++col) {
        size_t index = first + col * rowCount;
        if (index >= names.size()) {
            continue;
        }
        String name = names[index];
        line += name;
        if (col + 1 < columns) {
            size_t pad = columnWidth > name.length() ? columnWidth - name.length() : 2;
            while (pad--) {
                line += ' ';
            }
        }
    }
    while (line.endsWith(" ")) {
        line.remove(line.length() - 1);
    }
    return line;
}

void appendLsColumns(const std::vector<String>& names, size_t terminalWidth)
{
    if (names.empty()) {
        return;
    }
    size_t maxLen = 0;
    for (const String& name : names) {
        maxLen = std::max(maxLen, name.length());
    }
    size_t columnWidth = maxLen + 2;
    size_t columns = std::max<size_t>(1, terminalWidth / columnWidth);
    columns = std::min(columns, names.size());
    size_t rows = (names.size() + columns - 1) / columns;
    for (size_t row = 0; row < rows; ++row) {
        appendCliLine(lsColumnLine(names, row, rows, columnWidth, columns));
    }
}

void serialPrintLsColumns(const std::vector<String>& names, size_t terminalWidth)
{
    if (names.empty()) {
        return;
    }
    size_t maxLen = 0;
    for (const String& name : names) {
        maxLen = std::max(maxLen, name.length());
    }
    size_t columnWidth = maxLen + 2;
    size_t columns = std::max<size_t>(1, terminalWidth / columnWidth);
    columns = std::min(columns, names.size());
    size_t rows = (names.size() + columns - 1) / columns;
    for (size_t row = 0; row < rows; ++row) {
        Serial.println(lsColumnLine(names, row, rows, columnWidth, columns));
    }
}

void appendSdStatus()
{
    if (!ensureSdReady()) {
        appendCliLine(String("sd=not ready: ") + sdLastError);
        return;
    }
    uint8_t type = SD.cardType();
    String typeName = "unknown";
    if (type == CARD_MMC) typeName = "MMC";
    else if (type == CARD_SD) typeName = "SDSC";
    else if (type == CARD_SDHC) typeName = "SDHC";
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t avail = total > used ? total - used : 0;
    appendCliLine(String("sd=ready type=") + typeName +
                  " card=" + formatBytes(SD.cardSize()) +
                  " size=" + formatBytes(total) +
                  " used=" + formatBytes(used) +
                  " avail=" + formatBytes(avail));
    appendCliLine(String("cwd=") + sdCwd);
}

void appendSdDf()
{
    if (!ensureSdReady()) {
        appendCliLine(String("sd: ") + sdLastError);
        return;
    }
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t avail = total > used ? total - used : 0;
    uint32_t usePct = total ? static_cast<uint32_t>((used * 100ULL + total - 1) / total) : 0;
    appendCliLine("Filesystem      Size  Used Avail Use% Mounted on");
    appendCliLine(String("microSD         ") + formatBytes(total) + "  " +
                  formatBytes(used) + "  " + formatBytes(avail) + "  " +
                  usePct + "% /sd");
}

void appendSdList(const String& inputPath)
{
    if (!ensureSdReady()) {
        appendCliLine(String("sd: ") + sdLastError);
        return;
    }
    LsOptions options;
    String error;
    if (!parseLsOptions(inputPath, options, error)) {
        appendCliLine(error);
        return;
    }
    String path = normalizeSdPath(options.path);
    if (pathHasGlob(path)) {
        String dirPath = pathDirPart(path);
        String pattern = pathBasePart(path);
        File dir = SD.open(dirPath, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            appendCliLine(String("ls: cannot access '") + path + "': No such file or directory");
            if (dir) {
                dir.close();
            }
            return;
        }
        std::vector<String> names;
        size_t matched = 0;
        File file = dir.openNextFile();
        while (file) {
            String displayName = basenameOnly(file.name());
            if ((options.all || !displayName.startsWith(".")) && globPatternMatches(pattern, displayName)) {
                ++matched;
                if (options.longFormat) {
                    appendCliLine(lsDisplayLine(file, displayName, joinSdPath(dirPath, displayName), options));
                } else {
                    names.push_back(displayName + (file.isDirectory() ? "/" : ""));
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();
        if (!matched) {
            appendCliLine(String("ls: cannot access '") + path + "': No such file or directory");
            return;
        }
        if (!options.longFormat) {
            sortLsNames(names);
            appendLsColumns(names, std::max<size_t>(20, vt.columns()));
        }
        return;
    }
    File root = SD.open(path, FILE_READ);
    if (!root) {
        appendCliLine(String("sd ls: cannot open ") + path);
        return;
    }
    if (root.isDirectory() && !sdPathHasExecutePermission(path)) {
        appendCliLine(String("ls: cannot open directory '") + path + "': Permission denied");
        root.close();
        return;
    }
    if (!root.isDirectory()) {
        appendCliLine(lsDisplayLine(root, basenameOnly(path), path, options));
        root.close();
        return;
    }
    if (!options.longFormat) {
        std::vector<String> names = collectLsNames(root, path, options);
        appendLsColumns(names, std::max<size_t>(20, vt.columns()));
        root.close();
        return;
    }
    if (options.longFormat) {
        appendCliLine(String("total ") + formatBytes(root.size()) + "  " + path);
    }
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        String displayName = basenameOnly(name);
        if (!options.all && displayName.startsWith(".")) {
            file.close();
            file = root.openNextFile();
            continue;
        }
        appendCliLine(lsDisplayLine(file, displayName, joinSdPath(path, displayName), options));
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

void appendVirtualRootList(const LsOptions& options)
{
    if (options.longFormat) {
        appendCliLine("total 0  /");
        appendCliLine("dr-xr-xr-x 1 root root        0 Jan  1  1980 sd");
        appendCliLine("dr-xr-xr-x 1 root root        0 Jan  1  1980 usb");
        appendCliLine("dr-xr-xr-x 1 root root        0 Jan  1  1980 flash");
        return;
    }
    std::vector<String> names = {"sd/", "usb/", "flash/"};
    appendLsColumns(names, std::max<size_t>(20, vt.columns()));
}

#if ENABLE_USB_HOST_KEYBOARD
void appendUsbList(const String& path, const LsOptions& options)
{
    if (!ensureUsbReady()) {
        if (path == "/") {
            if (options.longFormat) {
                appendCliLine("total 0  /usb");
            }
            appendCliLine(String("usb: ") + usbMscStatus);
            return;
        }
        appendCliLine(String("usb: ") + usbMscStatus);
        return;
    }
    if (pathHasGlob(path)) {
        String dirPath = pathDirPart(path);
        String pattern = pathBasePart(path);
        FF_DIR dir{};
        FILINFO info{};
        if (f_opendir(&dir, usbFatPath(dirPath).c_str()) != FR_OK) {
            appendCliLine(String("ls: cannot access '/usb") + path + "': No such file or directory");
            return;
        }
        std::vector<String> names;
        size_t matched = 0;
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            String name(info.fname);
            if (!options.all && name.startsWith(".")) {
                continue;
            }
            if (!globPatternMatches(pattern, name)) {
                continue;
            }
            ++matched;
            bool dirEntry = (info.fattrib & AM_DIR) != 0;
            if (options.longFormat) {
                appendCliLine(String(dirEntry ? "drwxr-xr-x" : "-rw-r--r--") + " 1 tab5 tab5 " +
                              lsSizeText(dirEntry ? 0 : info.fsize, options) + " Jan  1  1980 " + name);
            } else {
                names.push_back(name + (dirEntry ? "/" : ""));
            }
        }
        f_closedir(&dir);
        if (!matched) {
            appendCliLine(String("ls: cannot access '/usb") + path + "': No such file or directory");
            return;
        }
        if (!options.longFormat) {
            sortLsNames(names);
            appendLsColumns(names, std::max<size_t>(20, vt.columns()));
        }
        return;
    }
    FILINFO info{};
    if (path != "/" && f_stat(usbFatPath(path).c_str(), &info) != FR_OK) {
        appendCliLine(String("usb ls: cannot open /usb") + (path == "/" ? "" : path));
        return;
    }
    if (path != "/" && !(info.fattrib & AM_DIR)) {
        appendCliLine(String((info.fattrib & AM_RDO) ? "-r--r--r--" : "-rw-r--r--") +
                      " 1 tab5 tab5 " + lsSizeText(info.fsize, options) + " Jan  1  1980 " + basenameOnly(path));
        return;
    }
    FF_DIR dir{};
    if (f_opendir(&dir, usbFatPath(path).c_str()) != FR_OK) {
        appendCliLine(String("usb ls: cannot open /usb") + (path == "/" ? "" : path));
        return;
    }
    if (options.longFormat) {
        appendCliLine(String("total 0  /usb") + (path == "/" ? "" : path));
    }
    std::vector<String> names;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        String name(info.fname);
        if (!options.all && name.startsWith(".")) {
            continue;
        }
        bool dirEntry = (info.fattrib & AM_DIR) != 0;
        if (options.longFormat) {
            appendCliLine(String(dirEntry ? "drwxr-xr-x" : "-rw-r--r--") + " 1 tab5 tab5 " +
                          lsSizeText(dirEntry ? 0 : info.fsize, options) + " Jan  1  1980 " + name);
        } else {
            names.push_back(name + (dirEntry ? "/" : ""));
        }
    }
    f_closedir(&dir);
    if (!options.longFormat) {
        appendLsColumns(names, std::max<size_t>(20, vt.columns()));
    }
}
#endif

void appendFlashList(const String& path, const LsOptions& options)
{
    if (pathHasGlob(path)) {
        String dirPath = pathDirPart(path);
        String pattern = pathBasePart(path);
        File dir = LittleFS.open(dirPath, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            appendCliLine(String("ls: cannot access '/flash") + path + "': No such file or directory");
            if (dir) {
                dir.close();
            }
            return;
        }
        std::vector<String> names;
        size_t matched = 0;
        File file = dir.openNextFile();
        while (file) {
            String displayName = basenameOnly(file.name());
            if ((options.all || !displayName.startsWith(".")) && globPatternMatches(pattern, displayName)) {
                ++matched;
                if (options.longFormat) {
                    appendCliLine(lsDisplayLine(file, displayName, joinSdPath(dirPath, displayName), options));
                } else {
                    names.push_back(displayName + (file.isDirectory() ? "/" : ""));
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();
        if (!matched) {
            appendCliLine(String("ls: cannot access '/flash") + path + "': No such file or directory");
            return;
        }
        if (!options.longFormat) {
            sortLsNames(names);
            appendLsColumns(names, std::max<size_t>(20, vt.columns()));
        }
        return;
    }
    File root = LittleFS.open(path, FILE_READ);
    if (!root) {
        appendCliLine(String("flash ls: cannot open /flash") + (path == "/" ? "" : path));
        return;
    }
    if (!root.isDirectory()) {
        appendCliLine(lsDisplayLine(root, basenameOnly(path), path, options));
        root.close();
        return;
    }
    if (options.longFormat) {
        appendCliLine(String("total ") + formatBytes(root.size()) + "  /flash" + (path == "/" ? "" : path));
    }
    std::vector<String> names;
    File file = root.openNextFile();
    while (file) {
        String displayName = basenameOnly(file.name());
        if (!options.all && displayName.startsWith(".")) {
            file.close();
            file = root.openNextFile();
            continue;
        }
        if (options.longFormat) {
            appendCliLine(lsDisplayLine(file, displayName, path, options));
        } else {
            names.push_back(displayName + (file.isDirectory() ? "/" : ""));
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    if (!options.longFormat) {
        appendLsColumns(names, std::max<size_t>(20, vt.columns()));
    }
}

void appendVirtualList(const String& inputPath)
{
    LsOptions options;
    String error;
    if (!parseLsOptions(inputPath, options, error)) {
        appendCliLine(error);
        return;
    }
    VirtualPath path = resolveVirtualPath(options.path);
    if (path.volume == VirtualVolume::Invalid && pathHasGlob(options.path) && !options.path.startsWith("/")) {
        path = resolveVirtualPath(String("/sd/") + options.path);
    }
    switch (path.volume) {
    case VirtualVolume::Root:
        appendVirtualRootList(options);
        break;
    case VirtualVolume::Sd:
    {
        String args;
        if (options.longFormat || options.all || options.human) {
            args += "-";
            if (options.longFormat) args += "l";
            if (options.all) args += "a";
            if (options.human) args += "h";
            args += " ";
        }
        args += path.localPath;
        appendSdList(args);
        break;
    }
#if ENABLE_USB_HOST_KEYBOARD
    case VirtualVolume::Usb:
        appendUsbList(path.localPath, options);
        break;
#endif
    case VirtualVolume::Flash:
        appendFlashList(path.localPath, options);
        break;
    default:
        appendCliLine(String("ls: cannot access '") + path.virtualPath + "': No such file or directory");
        break;
    }
}

void appendSdCat(const String& inputPath)
{
    if (!ensureSdReady()) {
        appendCliLine(String("sd: ") + sdLastError);
        return;
    }
    String path = normalizeSdPath(inputPath);
    if (!sdPathHasReadPermission(path)) {
        appendCliLine(String("sd cat: permission denied: ") + path);
        return;
    }
    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        appendCliLine(String("sd cat: cannot open ") + path);
        return;
    }
    String line;
    size_t printed = 0;
    while (file.available() && printed < 12000) {
        char c = static_cast<char>(file.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            appendCliLine(line);
            line = "";
        } else {
            line += c;
            if (line.length() >= 160) {
                appendCliLine(line);
                line = "";
            }
        }
        ++printed;
    }
    if (line.length()) {
        appendCliLine(line);
    }
    if (file.available()) {
        appendCliLine("... truncated");
    }
    file.close();
}

void appendFileText(File& file)
{
    String line;
    size_t printed = 0;
    while (file.available() && printed < 12000) {
        char c = static_cast<char>(file.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            appendCliLine(line);
            line = "";
        } else {
            line += c;
            if (line.length() >= 160) {
                appendCliLine(line);
                line = "";
            }
        }
        ++printed;
    }
    if (line.length()) {
        appendCliLine(line);
    }
    if (file.available()) {
        appendCliLine("... truncated");
    }
}

#if ENABLE_USB_HOST_KEYBOARD
void appendUsbCat(const String& path)
{
    if (!ensureUsbReady()) {
        appendCliLine(String("usb: ") + usbMscStatus);
        return;
    }
    FIL file{};
    if (f_open(&file, usbFatPath(path).c_str(), FA_READ) != FR_OK) {
        appendCliLine(String("usb cat: cannot open /usb") + (path == "/" ? "" : path));
        return;
    }
    char buffer[96];
    size_t printed = 0;
    String line;
    while (printed < 12000) {
        UINT readBytes = 0;
        if (f_read(&file, buffer, sizeof(buffer), &readBytes) != FR_OK || readBytes == 0) {
            break;
        }
        for (UINT i = 0; i < readBytes && printed < 12000; ++i, ++printed) {
            char c = buffer[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                appendCliLine(line);
                line = "";
            } else {
                line += c;
                if (line.length() >= 160) {
                    appendCliLine(line);
                    line = "";
                }
            }
        }
    }
    if (line.length()) {
        appendCliLine(line);
    }
    f_close(&file);
}
#endif

void appendFlashCat(const String& path)
{
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        appendCliLine(String("flash cat: cannot open /flash") + (path == "/" ? "" : path));
        return;
    }
    appendFileText(file);
    file.close();
}

void appendVirtualCat(const String& inputPath)
{
    VirtualPath path = resolveVirtualPath(inputPath);
    if (path.volume == VirtualVolume::Sd) {
        appendSdCat(path.localPath);
#if ENABLE_USB_HOST_KEYBOARD
    } else if (path.volume == VirtualVolume::Usb) {
        appendUsbCat(path.localPath);
#endif
    } else if (path.volume == VirtualVolume::Flash) {
        appendFlashCat(path.localPath);
    } else {
        appendCliLine(String("cat: cannot open '") + path.virtualPath + "'");
    }
}

String basenameOnly(const String& path)
{
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

String parentSdPath(const String& path)
{
    if (path == "/" || !path.length()) {
        return "/";
    }
    int slash = path.lastIndexOf('/');
    if (slash <= 0) {
        return "/";
    }
    return path.substring(0, slash);
}

bool makeSdDirectory(const String& inputPath, String& message)
{
    if (!ensureSdReady()) {
        message = String("sd: ") + sdLastError;
        return false;
    }
    String path = normalizeSdPath(inputPath);
    if (path == "/") {
        message = "mkdir: cannot create directory '/': File exists";
        return false;
    }
    if (SD.exists(path)) {
        message = String("mkdir: cannot create directory '") + path + "': File exists";
        return false;
    }
    String parent = parentSdPath(path);
    if (!sdPathHasWritePermission(parent) || !sdPathHasExecutePermission(parent)) {
        message = String("mkdir: cannot create directory '") + path + "': Permission denied";
        return false;
    }
    if (!SD.mkdir(path)) {
        message = String("mkdir: cannot create directory '") + path + "': failed";
        return false;
    }
    message = String("created ") + path;
    return true;
}

bool removeSdDirectory(const String& inputPath, String& message)
{
    if (!ensureSdReady()) {
        message = String("sd: ") + sdLastError;
        return false;
    }
    String path = normalizeSdPath(inputPath);
    if (path == "/") {
        message = "rmdir: failed to remove '/': Invalid argument";
        return false;
    }
    File dir = SD.open(path, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        message = String("rmdir: failed to remove '") + path + "': Not a directory";
        return false;
    }
    File child = dir.openNextFile();
    if (child) {
        child.close();
        dir.close();
        message = String("rmdir: failed to remove '") + path + "': Directory not empty";
        return false;
    }
    dir.close();
    String parent = parentSdPath(path);
    if (!sdPathHasWritePermission(parent) || !sdPathHasExecutePermission(parent)) {
        message = String("rmdir: failed to remove '") + path + "': Permission denied";
        return false;
    }
    if (!SD.rmdir(path)) {
        message = String("rmdir: failed to remove '") + path + "'";
        return false;
    }
    int modeIndex = findSdModeIndex(path);
    if (modeIndex >= 0) {
        sdModes.erase(sdModes.begin() + modeIndex);
        saveSdModes();
    }
    message = String("removed directory ") + path;
    return true;
}

String commonPrefix(const std::vector<String>& values)
{
    if (values.empty()) {
        return "";
    }
    String prefix = values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        while (prefix.length() && !values[i].startsWith(prefix)) {
            prefix.remove(prefix.length() - 1);
        }
    }
    return prefix;
}

const char* const LocalCliCommands[] = {
    "ble",      "cat",     "cd",       "chmod",    "clear",   "date",   "df",
    "dir",      "echo",    "help",     "history",  "hostname","ifconfig","image",
    "ip",       "ls",      "man",      "mkdir",    "ntp",     "pwd",    "python",
    "reboot",   "restart", "rm",       "rmdir",    "scp",     "sd",     "ssh",
    "status",   "sync",    "time",     "uname",    "uptime",  "wifi",   "whoami",
};

bool completeLocalCommandAtCursor()
{
    if (pythonReplMode) {
        return false;
    }
    clampCommandCursor();
    String before = commandLine.substring(0, commandCursor);
    if (before.indexOf(' ') >= 0 || before.indexOf('\t') >= 0) {
        return false;
    }
    String prefix = before;
    prefix.toLowerCase();

    std::vector<String> matches;
    for (const char* command : LocalCliCommands) {
        String value(command);
        if (value.startsWith(prefix)) {
            matches.push_back(value);
        }
    }
    if (matches.empty()) {
        appendCliLine("complete: no command match");
        return true;
    }

    String replacement = matches.size() == 1 ? matches[0] + " " : commonPrefix(matches);
    if (replacement.length() > prefix.length()) {
        commandLine = replacement + commandLine.substring(commandCursor);
        commandCursor = replacement.length();
    }
    if (matches.size() > 1) {
        appendCliLine("");
        for (const auto& match : matches) {
            appendCliLine(match);
        }
    }
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
    return true;
}

bool commandAllowsPathCompletion(const String& beforeToken)
{
    if (pythonReplMode) {
        return false;
    }
    String text = beforeToken;
    text.trim();
    text.toLowerCase();
    if (!text.length()) {
        return false;
    }
    int firstSpace = text.indexOf(' ');
    String first = firstSpace >= 0 ? text.substring(0, firstSpace) : text;
    if (first == "chmod" || first == "python" || first == "cat" || first == "cd" ||
        first == "ls" || first == "dir" || first == "mkdir" || first == "rmdir" ||
        first == "image") {
        return true;
    }
    if (text.startsWith("sd chmod ")) {
        return true;
    }
    return text == "ls" || text == "dir" || text == "cat" || text == "cd" || text == "python" ||
           text == "chmod" || text == "mkdir" || text == "rmdir" || text == "image" || text.endsWith(" chmod") ||
           text == "sd ls" || text == "sd dir" || text == "sd cat" || text == "sd cd" ||
           text == "sd rm" || text == "sd mkdir" || text == "sd rmdir" || text == "scp put";
}

bool completeSdPathAtCursor()
{
    clampCommandCursor();
    String before = commandLine.substring(0, commandCursor);
    int tokenStart = before.lastIndexOf(' ');
    String beforeToken = tokenStart >= 0 ? before.substring(0, tokenStart) : "";
    String token = tokenStart >= 0 ? before.substring(tokenStart + 1) : before;
    if (!commandAllowsPathCompletion(beforeToken)) {
        return false;
    }

    int slash = token.lastIndexOf('/');
    String dirPart = slash >= 0 ? token.substring(0, slash + 1) : "";
    String prefix = slash >= 0 ? token.substring(slash + 1) : token;
    VirtualPath virtualDir = resolveVirtualPath(dirPart.length() ? dirPart : ".");

    std::vector<String> matches;
    if (virtualDir.volume == VirtualVolume::Root) {
        static const char* roots[] = {"sd/", "usb/", "flash/"};
        for (const char* root : roots) {
            String name(root);
            if (name.startsWith(prefix)) {
                matches.push_back(name);
            }
        }
    } else if (virtualDir.volume == VirtualVolume::Sd) {
        if (!ensureSdReady()) {
            appendCliLine(String("sd: ") + sdLastError);
            return true;
        }
        File dir = SD.open(virtualDir.localPath, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            appendCliLine(String("complete: not a directory: ") + virtualDir.virtualPath);
            return true;
        }
        File file = dir.openNextFile();
        while (file) {
            String name = basenameOnly(file.name());
            if (name.startsWith(prefix)) {
                matches.push_back(name + (file.isDirectory() ? "/" : ""));
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();
#if ENABLE_USB_HOST_KEYBOARD
    } else if (virtualDir.volume == VirtualVolume::Usb) {
        if (!ensureUsbReady()) {
            appendCliLine(String("usb: ") + usbMscStatus);
            return true;
        }
        FF_DIR dir{};
        FILINFO info{};
        if (f_opendir(&dir, usbFatPath(virtualDir.localPath).c_str()) != FR_OK) {
            appendCliLine(String("complete: not a directory: ") + virtualDir.virtualPath);
            return true;
        }
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            String name(info.fname);
            if (name.startsWith(prefix)) {
                matches.push_back(name + ((info.fattrib & AM_DIR) ? "/" : ""));
            }
        }
        f_closedir(&dir);
#endif
    } else if (virtualDir.volume == VirtualVolume::Flash) {
        File dir = LittleFS.open(virtualDir.localPath, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            appendCliLine(String("complete: not a directory: ") + virtualDir.virtualPath);
            return true;
        }
        File file = dir.openNextFile();
        while (file) {
            String name = basenameOnly(file.name());
            if (name.startsWith(prefix)) {
                matches.push_back(name + (file.isDirectory() ? "/" : ""));
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();
    } else {
        appendCliLine(String("complete: not a directory: ") + virtualDir.virtualPath);
        return true;
    }

    if (matches.empty()) {
        appendCliLine("complete: no match");
        return true;
    }

    String replacementSuffix;
    if (matches.size() == 1) {
        replacementSuffix = matches[0];
    } else {
        replacementSuffix = commonPrefix(matches);
        appendCliLine("");
        for (const auto& match : matches) {
            appendCliLine(match);
        }
    }
    if (replacementSuffix.length() > prefix.length()) {
        String replacement = dirPart + replacementSuffix;
        size_t start = tokenStart >= 0 ? static_cast<size_t>(tokenStart + 1) : 0;
        commandLine = commandLine.substring(0, start) + replacement + commandLine.substring(commandCursor);
        commandCursor = start + replacement.length();
    }
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
    return true;
}

bool writeSdText(const String& inputPath, const String& text, bool append)
{
    if (!ensureSdReady()) {
        appendCliLine(String("sd: ") + sdLastError);
        return false;
    }
    String path = normalizeSdPath(inputPath);
    if (SD.exists(path) && !sdPathHasWritePermission(path)) {
        appendCliLine(String("sd write: permission denied: ") + path);
        return false;
    }
    if (!append && SD.exists(path)) {
        SD.remove(path);
    }
    File file = SD.open(path, FILE_APPEND);
    if (!file) {
        appendCliLine(String("sd write: cannot open ") + path);
        return false;
    }
    file.print(text);
    file.print("\n");
    file.close();
    appendCliLine(String(append ? "appended " : "wrote ") + path);
    return true;
}

bool makeVirtualDirectory(const String& inputPath, String& message)
{
    VirtualPath path = resolveVirtualPath(inputPath);
    if (path.volume == VirtualVolume::Root || path.volume == VirtualVolume::Invalid) {
        message = String("mkdir: cannot create directory '") + path.virtualPath + "': Read-only file system";
        return false;
    }
    if (path.volume == VirtualVolume::Sd) {
        return makeSdDirectory(path.localPath, message);
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        if (!ensureUsbReady()) {
            message = String("usb: ") + usbMscStatus;
            return false;
        }
        FRESULT result = f_mkdir(usbFatPath(path.localPath).c_str());
        message = result == FR_OK ? String("created ") + path.virtualPath
                                  : String("mkdir: cannot create directory '") + path.virtualPath + "'";
        return result == FR_OK;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        if (LittleFS.exists(path.localPath)) {
            message = String("mkdir: cannot create directory '") + path.virtualPath + "': File exists";
            return false;
        }
        bool ok = LittleFS.mkdir(path.localPath);
        message = ok ? String("created ") + path.virtualPath
                     : String("mkdir: cannot create directory '") + path.virtualPath + "'";
        return ok;
    }
    message = "mkdir: unsupported volume";
    return false;
}

bool removeVirtualDirectory(const String& inputPath, String& message)
{
    VirtualPath path = resolveVirtualPath(inputPath);
    if (path.volume == VirtualVolume::Root || path.volume == VirtualVolume::Invalid) {
        message = String("rmdir: failed to remove '") + path.virtualPath + "': Read-only file system";
        return false;
    }
    if (path.volume == VirtualVolume::Sd) {
        return removeSdDirectory(path.localPath, message);
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        if (!ensureUsbReady()) {
            message = String("usb: ") + usbMscStatus;
            return false;
        }
        bool ok = removeUsbDirectoryEntry(path.localPath);
        message = ok ? String("removed directory ") + path.virtualPath
                     : String("rmdir: failed to remove '") + path.virtualPath + "'";
        return ok;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        bool ok = LittleFS.rmdir(path.localPath);
        message = ok ? String("removed directory ") + path.virtualPath
                     : String("rmdir: failed to remove '") + path.virtualPath + "'";
        return ok;
    }
    message = "rmdir: unsupported volume";
    return false;
}

bool removeVirtualFile(const String& inputPath, String& message)
{
    VirtualPath path = resolveVirtualPath(inputPath);
    if (path.volume == VirtualVolume::Root || path.volume == VirtualVolume::Invalid) {
        message = String("rm: cannot remove '") + path.virtualPath + "': Read-only file system";
        return false;
    }
    if (path.volume == VirtualVolume::Sd) {
        if (!ensureSdReady()) {
            message = String("sd: ") + sdLastError;
            return false;
        }
        if (SD.exists(path.localPath) && !sdPathHasWritePermission(path.localPath)) {
            message = String("sd rm: permission denied: ") + path.virtualPath;
            return false;
        }
        bool ok = SD.remove(path.localPath);
        message = ok ? String("removed ") + path.virtualPath : String("rm failed: ") + path.virtualPath;
        return ok;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        if (!ensureUsbReady()) {
            message = String("usb: ") + usbMscStatus;
            return false;
        }
        FRESULT result = f_unlink(usbFatPath(path.localPath).c_str());
        message = result == FR_OK ? String("removed ") + path.virtualPath : String("rm failed: ") + path.virtualPath;
        return result == FR_OK;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        bool ok = LittleFS.remove(path.localPath);
        message = ok ? String("removed ") + path.virtualPath : String("rm failed: ") + path.virtualPath;
        return ok;
    }
    message = "rm: unsupported volume";
    return false;
}

bool virtualPathIsDirectory(const VirtualPath& path, bool& directory)
{
    directory = false;
    if (path.volume == VirtualVolume::Sd) {
        if (!ensureSdReady()) {
            return false;
        }
        File file = SD.open(path.localPath, FILE_READ);
        if (!file) {
            return false;
        }
        directory = file.isDirectory();
        file.close();
        return true;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        if (!ensureUsbReady()) {
            return false;
        }
        FILINFO info{};
        if (f_stat(usbFatPath(path.localPath).c_str(), &info) != FR_OK) {
            return false;
        }
        directory = (info.fattrib & AM_DIR) != 0;
        return true;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        File file = LittleFS.open(path.localPath, FILE_READ);
        if (!file) {
            return false;
        }
        directory = file.isDirectory();
        file.close();
        return true;
    }
    return false;
}

bool virtualReadChunk(const VirtualPath& path, uint32_t offset, uint8_t* buffer, size_t capacity, size_t& readBytes)
{
    readBytes = 0;
    if (path.volume == VirtualVolume::Sd) {
        File file = SD.open(path.localPath, FILE_READ);
        if (!file || file.isDirectory()) {
            return false;
        }
        file.seek(offset);
        readBytes = file.read(buffer, capacity);
        file.close();
        return true;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        FIL file;
        if (f_open(&file, usbFatPath(path.localPath).c_str(), FA_READ) != FR_OK) {
            return false;
        }
        if (f_lseek(&file, offset) != FR_OK) {
            f_close(&file);
            return false;
        }
        UINT bytes = 0;
        FRESULT result = f_read(&file, buffer, capacity, &bytes);
        f_close(&file);
        readBytes = bytes;
        return result == FR_OK;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        File file = LittleFS.open(path.localPath, FILE_READ);
        if (!file || file.isDirectory()) {
            return false;
        }
        file.seek(offset);
        readBytes = file.read(buffer, capacity);
        file.close();
        return true;
    }
    return false;
}

bool virtualWriteChunk(const VirtualPath& path, uint32_t offset, const uint8_t* buffer, size_t length)
{
    if (path.volume == VirtualVolume::Sd) {
        if (offset == 0 && SD.exists(path.localPath)) {
            SD.remove(path.localPath);
        }
        File file = SD.open(path.localPath, FILE_APPEND);
        if (!file) {
            return false;
        }
        if (offset > 0) {
            file.seek(offset);
        }
        size_t written = file.write(buffer, length);
        file.close();
        return written == length;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (path.volume == VirtualVolume::Usb) {
        FIL file;
        BYTE mode = FA_WRITE | FA_OPEN_ALWAYS;
        if (f_open(&file, usbFatPath(path.localPath).c_str(), mode) != FR_OK) {
            return false;
        }
        FRESULT result = offset == 0 ? f_truncate(&file) : f_lseek(&file, offset);
        if (result != FR_OK) {
            f_close(&file);
            return false;
        }
        UINT written = 0;
        result = f_write(&file, buffer, length, &written);
        f_close(&file);
        return result == FR_OK && written == length;
    }
#endif
    if (path.volume == VirtualVolume::Flash) {
        if (offset == 0 && LittleFS.exists(path.localPath)) {
            LittleFS.remove(path.localPath);
        }
        File file = LittleFS.open(path.localPath, FILE_APPEND);
        if (!file) {
            return false;
        }
        if (offset > 0) {
            file.seek(offset);
        }
        size_t written = file.write(buffer, length);
        file.close();
        return written == length;
    }
    return false;
}

bool copyVirtualFile(const String& sourceInput, const String& destInput, String& message)
{
    VirtualPath source = resolveVirtualPath(sourceInput);
    VirtualPath dest = resolveVirtualPath(destInput);
    if (source.volume == VirtualVolume::Root || source.volume == VirtualVolume::Invalid) {
        message = String("cp: cannot stat '") + sourceInput + "'";
        return false;
    }
    if (dest.volume == VirtualVolume::Root || dest.volume == VirtualVolume::Invalid) {
        message = String("cp: cannot create '") + destInput + "'";
        return false;
    }
    bool sourceDir = false;
    if (!virtualPathIsDirectory(source, sourceDir)) {
        message = String("cp: cannot stat '") + source.virtualPath + "'";
        return false;
    }
    if (sourceDir) {
        message = String("cp: omitting directory '") + source.virtualPath + "'";
        return false;
    }
    bool destDir = false;
    if (virtualPathIsDirectory(dest, destDir) && destDir) {
        dest = resolveVirtualPath(joinSdPath(dest.virtualPath, basenameOnly(source.virtualPath)));
    }
    uint8_t buffer[512];
    uint32_t offset = 0;
    for (;;) {
        size_t readBytes = 0;
        if (!virtualReadChunk(source, offset, buffer, sizeof(buffer), readBytes)) {
            message = String("cp: failed to read '") + source.virtualPath + "'";
            return false;
        }
        if (readBytes == 0) {
            break;
        }
        if (!virtualWriteChunk(dest, offset, buffer, readBytes)) {
            message = String("cp: failed to write '") + dest.virtualPath + "'";
            return false;
        }
        offset += readBytes;
    }
    if (offset == 0) {
        if (!virtualWriteChunk(dest, 0, buffer, 0)) {
            message = String("cp: failed to write '") + dest.virtualPath + "'";
            return false;
        }
    }
    message = String("copied ") + source.virtualPath + " -> " + dest.virtualPath;
    return true;
}

bool handleSdCliCommand(const String& command, const String& lower)
{
    if (lower == "sd" || lower == "sd status") {
        appendSdStatus();
        return true;
    }
    if (lower == "df" || lower == "sd df" || lower == "sd free" || lower == "free") {
        appendSdDf();
        return true;
    }
    if (lower == "pwd") {
        appendCliLine(sdCwd);
        return true;
    }
    if (lower == "sd pwd") {
        appendCliLine(resolveVirtualPath(sdCwd).volume == VirtualVolume::Sd ? resolveVirtualPath(sdCwd).localPath : "/");
        return true;
    }
    if (lower == "ls" || lower == "dir") {
        appendVirtualList("");
        return true;
    }
    if (lower == "sd ls" || lower == "sd dir") {
        appendSdList("");
        return true;
    }
    if (lower.startsWith("ls ") || lower.startsWith("dir ")) {
        appendVirtualList(command.substring(command.indexOf(' ') + 1));
        return true;
    }
    if (lower.startsWith("sd ls ") || lower.startsWith("sd dir ")) {
        appendSdList(command.substring(command.indexOf(' ', 3) + 1));
        return true;
    }
    if (lower.startsWith("cd ")) {
        String arg = command.substring(3);
        VirtualPath path = resolveVirtualPath(arg);
        if (path.volume == VirtualVolume::Root) {
            sdCwd = "/";
            appendCliLine(sdCwd);
            return true;
        }
        if (path.volume == VirtualVolume::Invalid) {
            appendCliLine(String("cd: no such directory: ") + path.virtualPath);
            return true;
        }
        if (path.volume == VirtualVolume::Sd) {
            if (!ensureSdReady()) {
                appendCliLine(String("sd: ") + sdLastError);
                return true;
            }
            File dir = SD.open(path.localPath, FILE_READ);
            if (!dir || !dir.isDirectory()) {
                appendCliLine(String("cd: not a directory: ") + path.virtualPath);
            } else if (!sdPathHasExecutePermission(path.localPath)) {
                appendCliLine(String("cd: permission denied: ") + path.virtualPath);
            } else {
                sdCwd = path.virtualPath;
                appendCliLine(sdCwd);
            }
            if (dir) {
                dir.close();
            }
            return true;
        }
#if ENABLE_USB_HOST_KEYBOARD
        if (path.volume == VirtualVolume::Usb) {
            if (path.localPath == "/") {
                sdCwd = path.virtualPath;
                appendCliLine(sdCwd);
                return true;
            }
            if (!ensureUsbReady()) {
                appendCliLine(String("usb: ") + usbMscStatus);
                return true;
            }
            FILINFO info{};
            if (f_stat(usbFatPath(path.localPath).c_str(), &info) != FR_OK || !(info.fattrib & AM_DIR)) {
                appendCliLine(String("cd: not a directory: ") + path.virtualPath);
            } else {
                sdCwd = path.virtualPath;
                appendCliLine(sdCwd);
            }
            return true;
        }
#endif
        if (path.volume == VirtualVolume::Flash) {
            File dir = LittleFS.open(path.localPath, FILE_READ);
            if (!dir || !dir.isDirectory()) {
                appendCliLine(String("cd: not a directory: ") + path.virtualPath);
            } else {
                sdCwd = path.virtualPath;
                appendCliLine(sdCwd);
            }
            if (dir) {
                dir.close();
            }
            return true;
        }
        return true;
    }
    if (lower.startsWith("sd cd ")) {
        String arg = command.substring(6);
        String path = normalizeSdPath(arg);
        if (!ensureSdReady()) {
            appendCliLine(String("sd: ") + sdLastError);
            return true;
        }
        File dir = SD.open(path, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            appendCliLine(String("sd cd: not a directory: ") + path);
        } else if (!sdPathHasExecutePermission(path)) {
            appendCliLine(String("sd cd: permission denied: ") + path);
        } else {
            sdCwd = String("/sd") + (path == "/" ? "" : path);
            appendCliLine(path);
        }
        return true;
    }
    if (lower.startsWith("cat ")) {
        appendVirtualCat(command.substring(4));
        return true;
    }
    if (lower.startsWith("sd cat ")) {
        appendSdCat(command.substring(7));
        return true;
    }
    if (lower.startsWith("sd write ") || lower.startsWith("sd append ")) {
        bool append = lower.startsWith("sd append ");
        size_t baseLen = append ? strlen("sd append ") : strlen("sd write ");
        String rest = command.substring(baseLen);
        rest.trim();
        int split = rest.indexOf(' ');
        if (split <= 0) {
            appendCliLine(String("usage: ") + (append ? "sd append" : "sd write") + " <path> <text>");
            return true;
        }
        writeSdText(rest.substring(0, split), rest.substring(split + 1), append);
        return true;
    }
    if (lower.startsWith("mkdir ")) {
        String message;
        makeVirtualDirectory(command.substring(strlen("mkdir ")), message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("sd mkdir ")) {
        String message;
        makeSdDirectory(command.substring(strlen("sd mkdir ")), message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("rmdir ")) {
        String message;
        removeVirtualDirectory(command.substring(strlen("rmdir ")), message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("sd rmdir ")) {
        String message;
        removeSdDirectory(command.substring(strlen("sd rmdir ")), message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("rm ")) {
        String message;
        removeVirtualFile(command.substring(strlen("rm ")), message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("cp ")) {
        String rest = command.substring(strlen("cp "));
        rest.trim();
        int split = rest.indexOf(' ');
        if (split <= 0) {
            appendCliLine("usage: cp <source> <dest>");
            return true;
        }
        String source = rest.substring(0, split);
        String dest = rest.substring(split + 1);
        dest.trim();
        if (!dest.length()) {
            appendCliLine("usage: cp <source> <dest>");
            return true;
        }
        String message;
        copyVirtualFile(source, dest, message);
        appendCliLine(message);
        return true;
    }
    if (lower.startsWith("sd rm ")) {
        if (!ensureSdReady()) {
            appendCliLine(String("sd: ") + sdLastError);
            return true;
        }
        String path = normalizeSdPath(command.substring(strlen("sd rm ")));
        if (SD.exists(path) && !sdPathHasWritePermission(path)) {
            appendCliLine(String("sd rm: permission denied: ") + path);
            return true;
        }
        appendCliLine(SD.remove(path) ? String("removed ") + path : String("sd rm failed: ") + path);
        return true;
    }
    if (lower.startsWith("chmod ") || lower.startsWith("sd chmod ")) {
        String rest = lower.startsWith("sd chmod ") ? command.substring(strlen("sd chmod ")) : command.substring(strlen("chmod "));
        rest.trim();
        int split = rest.indexOf(' ');
        if (split <= 0) {
            appendCliLine("chmod: usage: chmod <mode> <path>");
            return true;
        }
        String modeText = rest.substring(0, split);
        String path = normalizeSdPath(rest.substring(split + 1));
        if (!ensureSdReady()) {
            appendCliLine(String("sd: ") + sdLastError);
            return true;
        }
        if (!SD.exists(path)) {
            appendCliLine(String("chmod: cannot access '") + path + "'");
            return true;
        }
        uint16_t mode = 0;
        if (!parseOctalMode(modeText, mode)) {
            appendCliLine("chmod: invalid mode");
            return true;
        }
        setSdModeForPath(path, mode);
        appendCliLine(String("mode ") + modeText + " " + path);
        return true;
    }
    return false;
}

bool parseScpProfileIndex(String& rest, size_t& index)
{
    rest.trim();
    int split = rest.lastIndexOf(' ');
    if (split < 0) {
        index = activeSsh;
        return activeSsh < config.ssh.size();
    }
    String tail = rest.substring(split + 1);
    for (size_t i = 0; i < tail.length(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tail[i]))) {
            index = activeSsh;
            return activeSsh < config.ssh.size();
        }
    }
    size_t parsed = static_cast<size_t>(tail.toInt());
    if (parsed >= config.ssh.size()) {
        return false;
    }
    rest = rest.substring(0, split);
    rest.trim();
    index = parsed;
    return true;
}

bool parseDirectScpEndpoint(const String& endpoint, SshProfile& profile, String& remotePath)
{
    int at = endpoint.indexOf('@');
    int colon = endpoint.indexOf(':', at + 1);
    if (at <= 0 || colon <= at + 1 || colon >= static_cast<int>(endpoint.length()) - 1) {
        return false;
    }
    profile = SshProfile{};
    profile.user = endpoint.substring(0, at);
    profile.host = endpoint.substring(at + 1, colon);
    profile.port = 22;
    profile.terminal = "xterm-256color";
    remotePath = endpoint.substring(colon + 1);
    profile.name = String("direct ") + profile.user + "@" + profile.host;
    inheritSavedSshCredentials(profile);
    return profile.user.length() && profile.host.length() && remotePath.length();
}

bool splitScpPaths(String rest, String& first, String& second, String& trailing)
{
    rest.trim();
    int split = rest.indexOf(' ');
    if (split <= 0) {
        return false;
    }
    first = rest.substring(0, split);
    rest = rest.substring(split + 1);
    rest.trim();
    split = rest.indexOf(' ');
    if (split < 0) {
        second = rest;
        trailing = "";
    } else {
        second = rest.substring(0, split);
        trailing = rest.substring(split + 1);
        trailing.trim();
    }
    first.trim();
    second.trim();
    return first.length() && second.length();
}

bool handleScpCliCommand(const String& command, const String& lower)
{
    if (lower == "scp" || lower == "scp help") {
        appendCliLine("usage:");
        appendCliLine("  scp get <remote> <sd-local> [profile-index]");
        appendCliLine("  scp put <sd-local> <remote> [profile-index]");
        appendCliLine("  scp get user@host:/remote <sd-local> [password]");
        appendCliLine("  scp put <sd-local> user@host:/remote [password]");
        return true;
    }
    const bool isGet = lower.startsWith("scp get ");
    const bool isPut = lower.startsWith("scp put ");
    if (!isGet && !isPut) {
        return false;
    }
    if (!ensureSdReady()) {
        appendCliLine(String("sd: ") + sdLastError);
        return true;
    }
    String rest = command.substring(isGet ? strlen("scp get ") : strlen("scp put "));
    String first;
    String second;
    String trailing;
    if (!splitScpPaths(rest, first, second, trailing)) {
        appendCliLine(String("usage: scp ") + (isGet ? "get" : "put") +
                      (isGet ? " <remote> <sd-local> [profile-index]" : " <sd-local> <remote> [profile-index]"));
        return true;
    }

    SshProfile profile;
    String remotePath;
    String localPath;
    const bool direct = isGet ? parseDirectScpEndpoint(first, profile, remotePath)
                              : parseDirectScpEndpoint(second, profile, remotePath);
    if (direct) {
        if (trailing.length()) {
            profile.password = trailing;
        }
        localPath = isGet ? second : first;
    } else {
        if (config.ssh.empty()) {
            appendCliLine("scp: no SSH profiles");
            return true;
        }
        rest = command.substring(isGet ? strlen("scp get ") : strlen("scp put "));
        size_t profileIndex = activeSsh;
        if (!parseScpProfileIndex(rest, profileIndex)) {
            appendCliLine("scp: invalid profile index");
            return true;
        }
        if (!splitScpPaths(rest, first, second, trailing)) {
            appendCliLine("scp: missing paths");
            return true;
        }
        profile = config.ssh[profileIndex];
        remotePath = isGet ? first : second;
        localPath = isGet ? second : first;
    }

    String error;
    appendCliLine(String("scp ") + (isGet ? "get " : "put ") + profile.user + "@" + profile.host);
    bool ok = false;
    if (isGet) {
        ok = ssh.scpDownload(profile, remotePath, SD, normalizeSdPath(localPath), error);
    } else {
        ok = ssh.scpUpload(profile, SD, normalizeSdPath(localPath), remotePath, error);
    }
    appendCliLine(ok ? "scp: done" : String("scp: failed: ") + error);
    return true;
}

bool handleBleCliCommand(const String&, const String& lower)
{
    if (lower == "ble" || lower == "ble status") {
        appendCliLine(keyboard.bleStatus());
        return true;
    }
    if (lower == "ble devices" || lower == "ble paired") {
        appendCliLine(keyboard.bleDevicesStatus());
        return true;
    }
    if (lower == "ble gapstatus") {
        appendCliLine(keyboard.bleGapStatus());
        return true;
    }
    if (lower == "ble scan") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleScan(result);
        appendCliLine(String(ok ? "ble scan: " : "ble scan failed: ") + result);
        for (size_t i = 0; i < keyboard.bleScanCount(); ++i) {
            appendCliLine(String("  ") + keyboard.bleScanEntry(i));
        }
        return true;
    }
    if (lower == "ble gapauto" || lower == "ble scanpair") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = lower == "ble gapauto" ? keyboard.bleGapScanAndSubscribeHid(result)
                                         : keyboard.bleScanAndPairFirst(result);
        if (ok) {
            saveBlePairingFromKeyboard();
        }
        appendCliLine(String(ok ? "ble pair: " : "ble pair failed: ") + result);
        return true;
    }
    if (lower.startsWith("ble pair ")) {
        String indexText = lower.substring(strlen("ble pair "));
        indexText.trim();
        String result;
        bool ok = keyboard.blePair(static_cast<size_t>(indexText.toInt()), result);
        if (ok) {
            saveBlePairingFromKeyboard();
        }
        appendCliLine(String(ok ? "ble pair: " : "ble pair failed: ") + result);
        return true;
    }
    if (lower == "ble disconnect" || lower.startsWith("ble disconnect ")) {
        String arg = lower == "ble disconnect" ? "all" : lower.substring(strlen("ble disconnect "));
        arg.trim();
        String result;
        bool ok = keyboard.bleDisconnect(arg == "all" ? -1 : arg.toInt(), result);
        appendCliLine(String(ok ? "ble disconnect: " : "ble disconnect failed: ") + result);
        return true;
    }
    if (lower == "ble forget" || lower.startsWith("ble forget ")) {
        String arg = lower == "ble forget" ? "all" : lower.substring(strlen("ble forget "));
        arg.trim();
        String result;
        bool ok = arg == "all" ? keyboard.bleForget(result) : keyboard.bleDisconnect(arg.toInt(), result);
        String configResult;
        bool configOk = removeBleDeviceConfig(arg == "all" ? -1 : arg.toInt(), configResult);
        appendCliLine(String((ok && configOk) ? "ble forget: " : "ble forget failed: ") + configResult + "; " + result);
        return true;
    }
    if (lower == "ble enable" || lower == "ble disable") {
        config.keyboard.bleKeyboardEnabled = lower == "ble enable";
        keyboard.configure(config.keyboard);
        saveConfig();
        appendCliLine(keyboard.bleStatus());
        return true;
    }
    return false;
}

bool handlePythonCliCommand(const String& command, const String& lower)
{
    if (lower == "python help" || lower == "python --help" || lower == "python -h") {
        appendCliLine("python commands:");
        appendCliLine("  python");
        appendCliLine("  python <sd.py> [args...]");
        appendCliLine("  python -c <statement>");
        appendCliLine("  python --reset");
        appendCliLine("Runs the embedded MicroPython VM; scripts are loaded from microSD.");
        return true;
    }
    if (lower == "python") {
        pythonReplMode = true;
        appendCliLine("MicroPython REPL. Type exit() to return.");
        return true;
    }
    if (lower == "python --reset") {
        python.reset();
        appendCliLine("python: state reset");
        return true;
    }
    if (lower.startsWith("python -c ")) {
        String statement = command.substring(strlen("python -c "));
        if (!python.runLine(statement, appendPythonCliLine)) {
            appendCliLine(String("python: ") + python.lastError());
        }
        return true;
    }
    if (lower.startsWith("python ")) {
        if (!ensureSdReady()) {
            appendCliLine(String("sd: ") + sdLastError);
            return true;
        }
        String pathArg;
        String args;
        splitPythonScriptCommand(command.substring(strlen("python ")), pathArg, args);
        String path = normalizeSdPath(pathArg);
        if (!sdPathHasReadPermission(path)) {
            appendCliLine(String("python: permission denied: ") + path);
            return true;
        }
        appendCliLine(args.length() ? String("python ") + path + " " + args : String("python ") + path);
        uint32_t start = millis();
        bool ok = python.runFile(SD, path, args, appendPythonCliLine);
        appendCliLine(ok ? String("python: done in ") + (millis() - start) + " ms"
                         : String("python: failed: ") + python.lastError());
        return true;
    }
    return false;
}

String formattedDateTime()
{
    time_t now = time(nullptr);
    if (now < 1700000000) {
        return "";
    }
    now += static_cast<time_t>(config.system.utcOffsetMinutes) * 60;
    struct tm tmLocal;
    gmtime_r(&now, &tmLocal);
    const int offsetHours = config.system.utcOffsetMinutes / 60;
    const int offsetMinutes = abs(config.system.utcOffsetMinutes % 60);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC%+03d:%02d",
             tmLocal.tm_year + 1900,
             tmLocal.tm_mon + 1,
             tmLocal.tm_mday,
             tmLocal.tm_hour,
             tmLocal.tm_min,
             tmLocal.tm_sec,
             offsetHours,
             offsetMinutes);
    return String(buffer);
}

void startTimeSync(bool force)
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!force && timeSyncStarted && millis() - lastTimeSyncAttempt < 3600000UL) {
        return;
    }
    timeSyncStarted = true;
    timeSynced = false;
    lastTimeSyncAttempt = millis();
    configTime(0, 0, config.system.ntpServer.c_str(), "time.nict.jp", "pool.ntp.org");
    setWifiStatus(String("Time sync: ") + config.system.ntpServer);
}

void pollTimeSync()
{
    if (!timeSyncStarted || timeSynced) {
        return;
    }
    if (time(nullptr) >= 1700000000) {
        timeSynced = true;
        appendCliLine(String("[time] ") + formattedDateTime() + " " + config.system.region);
    }
}

bool handleTab5CliCommand(const String& line)
{
    String command = line;
    command.trim();
    String lower = command;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        appendCliHelp();
        return true;
    }
    if (lower == "man") {
        appendCliHelp();
        appendCliLine("Usage: man <command>");
        return true;
    }
    if (lower.startsWith("man ")) {
        return appendCliMan(command.substring(4));
    }
    if (lower == "clear" || lower == "cls" || lower == "reset") {
        terminal.clear();
        return true;
    }
    if (lower == "status") {
        appendCliLine(String("screen=Tab5 CLI wifi=") + wifiStatusText);
        appendCliLine(String("ip=") + WiFi.localIP().toString() + " ssid=" + WiFi.SSID());
        appendCliLine(String("ssh=") + (ssh.connected() ? "connected" : "disconnected"));
        appendCliLine(String("device=") + config.system.deviceName + " region=" + config.system.region);
        appendCliLine(String("time=") + (timeSynced ? formattedDateTime() : "not synced"));
        appendCliLine(String("keymap=") + config.keyboard.layout + " keyboard=" + keyboard.status());
        appendCliLine(String("ble=") + keyboard.bleStatus());
        appendCliLine(String("sd=") + (sdReady ? "ready" : sdLastError));
        appendCliLine(String("activeWifi=") + activeWifi + " activeSsh=" + activeSsh);
        return true;
    }
    if (lower == "history") {
        for (size_t i = 0; i < commandHistory.size(); ++i) {
            appendCliLine(String(i + 1) + "  " + commandHistory[i]);
        }
        return true;
    }
    if (lower == "whoami") {
        appendCliLine(config.system.deviceName);
        return true;
    }
    if (lower == "hostname") {
        appendCliLine(config.system.deviceName);
        return true;
    }
    if (lower == "uname" || lower == "uname -a") {
        appendCliLine("Tab5 CLI tab5 0.1.0 esp32p4 arduino");
        return true;
    }
    if (lower == "date") {
        String text = formattedDateTime();
        if (text.length()) {
            appendCliLine(text + " " + config.system.region);
        } else {
            appendCliLine("time not synced; connect Wi-Fi or run 'time sync'");
            startTimeSync(true);
        }
        return true;
    }
    if (lower == "time sync" || lower == "ntp sync") {
        startTimeSync(true);
        appendCliLine("time sync requested");
        return true;
    }
    if (lower == "uptime") {
        uint32_t seconds = millis() / 1000;
        appendCliLine(String("up ") + (seconds / 3600) + "h " + ((seconds / 60) % 60) + "m " + (seconds % 60) + "s");
        return true;
    }
    if (handleSdCliCommand(command, lower)) {
        return true;
    }
    if (handleScpCliCommand(command, lower)) {
        return true;
    }
    if (lower == "image" || lower == "img" || lower == "view" ||
        lower.startsWith("image ") || lower.startsWith("img ") || lower.startsWith("view ")) {
        size_t prefixLen = lower.startsWith("image") ? strlen("image") : (lower.startsWith("view") ? strlen("view") : strlen("img"));
        String message;
        showImageCommand(command.substring(prefixLen), message);
        appendCliLine(message);
        return true;
    }
    if (handlePythonCliCommand(command, lower)) {
        return true;
    }
    if (handleBleCliCommand(command, lower)) {
        return true;
    }
    if (lower == "wifi status") {
        appendCliLine(wifiStatusText);
        appendCliLine(String("wl=") + static_cast<int>(WiFi.status()) + " ip=" + WiFi.localIP().toString() + " ssid=" + WiFi.SSID());
        return true;
    }
    if (lower == "wifi off") {
        stopWifiRuntime();
        appendCliLine("Wi-Fi off");
        return true;
    }
    if (lower == "wifi on") {
        enableWifiRuntime(20000);
        appendCliLine("Wi-Fi on");
        return true;
    }
    if (lower == "wifi list") {
        appendWifiList();
        return true;
    }
    if (lower == "ip addr" || lower == "ip a" || lower == "ifconfig") {
        appendCliLine("wlan0:");
        appendCliLine(String("  inet ") + WiFi.localIP().toString());
        appendCliLine(String("  ssid ") + WiFi.SSID());
        appendCliLine(String("  status ") + static_cast<int>(WiFi.status()));
        return true;
    }
    if (lower == "ssh list") {
        appendSshList();
        return true;
    }
    if (lower == "ssh disconnect") {
        resetStorageBridgeState();
        ssh.disconnect();
        resetCommandEditor();
        configureTerminal();
        appendCliLine("SSH disconnected");
        return true;
    }
    if (lower.startsWith("ssh connect")) {
        size_t index = 0;
        String trailing = command.substring(strlen("ssh connect"));
        trailing.trim();
        if (!trailing.length() && !config.ssh.empty()) {
            connectActiveSsh();
        } else if (parseTrailingIndex(command, strlen("ssh connect"), index) && index < config.ssh.size()) {
            activeSsh = index;
            saveConfig();
            connectActiveSsh();
        } else {
            appendCliLine("usage: ssh connect <index>");
        }
        return true;
    }
    if (lower.startsWith("echo ")) {
        appendCliLine(command.substring(5));
        return true;
    }
    return false;
}

void executeLocalCommand()
{
    String line = commandLine;
    line.trim();
    terminal.append(String(pythonReplMode ? PythonPrompt : localShellPrompt()) + line + "\n");
    rememberCommandHistory(line);
    resetCommandEditor();

    if (pythonReplMode) {
        if (line == "exit()" || line == "quit()" || line == "exit" || line == "quit") {
            pythonReplMode = false;
            appendCliLine("python: exit");
        } else if (line.length() && !python.runLine(line, appendPythonCliLine)) {
            appendCliLine(String("python: ") + python.lastError());
        }
        dirty = true;
        return;
    }

    if (!line.length()) {
        dirty = true;
        return;
    }

    if (handleTab5CliCommand(line)) {
        dirty = true;
        return;
    }

    SshProfile directProfile;
    String error;
    if (parseSshCommand(line, directProfile, error)) {
        connectSshProfile(directProfile);
    } else {
        appendCliLine(String(line) + ": command not found");
        appendCliLine("type 'help' for Tab5 CLI commands, or use ssh user@host[:port]");
    }
    dirty = true;
}

bool sendSshText(const String& text)
{
    if (!ssh.connected()) {
        return false;
    }
    return ssh.write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

void resetStorageBridgeState()
{
    storageBridgeRunning = false;
    storageBridgeLine = "";
}

void replaceRemoteCommandLine(const String& line)
{
    if (!ssh.connected()) {
        return;
    }
    String payload;
    payload += static_cast<char>(0x01);  // Ctrl-A: beginning of line in readline shells.
    payload += static_cast<char>(0x0B);  // Ctrl-K: kill to end of line.
    payload += line;
    sendSshText(payload);
    commandLine = line;
    commandCursor = commandLine.length();
}

bool browseRemoteCommandHistory(int delta)
{
    if (vt.alternateScreen() || commandHistory.empty()) {
        return false;
    }
    if (delta < 0) {
        if (commandHistoryIndex == 0) {
            return true;
        }
        --commandHistoryIndex;
    } else {
        if (commandHistoryIndex >= commandHistory.size()) {
            return true;
        }
        ++commandHistoryIndex;
    }
    replaceRemoteCommandLine(commandHistoryIndex < commandHistory.size() ? commandHistory[commandHistoryIndex] : "");
    return true;
}

String terminalLineText(size_t row, size_t endCol)
{
    String line;
    const size_t limit = std::min(endCol, vt.columns());
    for (size_t col = 0; col < limit; ++col) {
        const auto& cell = vt.cell(col, row);
        if (cell.continuation) {
            continue;
        }
        line += cell.ch.length() ? cell.ch : " ";
    }
    while (line.endsWith(" ")) {
        line.remove(line.length() - 1);
    }
    return line;
}

String commandFromRemoteTerminalLine()
{
    if (vt.alternateScreen() || !vt.columns() || !vt.rows()) {
        return commandLine;
    }
    String line = terminalLineText(vt.cursorRow(), vt.cursorColumn());
    if (!line.length()) {
        return commandLine;
    }
    int promptEnd = -1;
    const char* markers[] = {"$ ", "# ", "% ", "> "};
    for (const char* marker : markers) {
        int index = line.lastIndexOf(marker);
        if (index >= 0) {
            promptEnd = std::max(promptEnd, index + static_cast<int>(strlen(marker)));
        }
    }
    String restored = promptEnd >= 0 ? line.substring(promptEnd) : line;
    restored.trim();
    return restored.length() ? restored : commandLine;
}

void trackRemoteCommandText(const String& text)
{
    if (vt.alternateScreen()) {
        return;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c == '\r' || c == '\n') {
            String line = commandFromRemoteTerminalLine();
            line.trim();
            rememberCommandHistory(line);
            resetCommandEditor();
        } else if (c == 0x08 || c == 0x7F) {
            backspaceCommandText();
        } else if (c == '\t') {
            commandLine = commandFromRemoteTerminalLine();
            commandCursor = commandLine.length();
            commandHistoryIndex = commandHistory.size();
        } else if (std::isprint(static_cast<unsigned char>(c))) {
            insertCommandText(String(c));
        }
    }
}

int settingTextY(int rowY)
{
    return rowY + max<int>(6, (settingRowH() - terminalFont().settingsLineHeight) / 2);
}

size_t visibleSettingRows()
{
    const int available = max<int>(1, screenSprite.height() - settingListTop());
    return max<size_t>(1, available / settingRowH());
}

size_t maxSettingOffset(size_t total)
{
    const size_t visible = visibleSettingRows();
    return total > visible ? total - visible : 0;
}

void clampSettingScroll(size_t total)
{
    settingScrollOffset = min(settingScrollOffset, maxSettingOffset(total));
}

void scrollSettingList(int delta, size_t total)
{
    const size_t maxOffset = maxSettingOffset(total);
    int next = static_cast<int>(settingScrollOffset) + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > static_cast<int>(maxOffset)) {
        next = static_cast<int>(maxOffset);
    }
    settingScrollOffset = static_cast<size_t>(next);
    dirty = true;
}

void drawButton(const Rect& r, const char* label, uint16_t fg = TFT_WHITE, uint16_t bg = TFT_DARKGREY)
{
    screenSprite.fillRect(r.x, r.y, r.w, r.h, bg);
    screenSprite.drawRect(r.x, r.y, r.w, r.h, TFT_LIGHTGREY);
    screenSprite.setTextColor(fg, bg);
    int textY = r.y + max<int>(2, (r.h - screenSprite.fontHeight()) / 2);
    screenSprite.drawString(label, r.x + 6, textY);
}

void drawFocusedBodyButton(const Rect& r, const char* label, size_t focusIndex, uint16_t bg = TFT_DARKGREY)
{
    bool focused = !keyboardMenuMode && focusedContentItem == focusIndex;
    drawButton(r, label, TFT_WHITE, focused ? TFT_NAVY : bg);
    if (focused) {
        screenSprite.drawRect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, TFT_CYAN);
    }
}

void drawHeaderFocus()
{
    if (!keyboardMenuMode) {
        return;
    }
    clampFocusedHeaderButton();
    const Rect* r = headerButtonAt(focusedHeaderButton);
    if (!r) {
        return;
    }
    screenSprite.drawRect(r->x - 2, r->y - 2, r->w + 4, r->h + 4, TFT_YELLOW);
    screenSprite.drawRect(r->x - 3, r->y - 3, r->w + 6, r->h + 6, TFT_YELLOW);
}

void drawHeader()
{
    screenSprite.fillRect(0, 0, screenSprite.width(), HeaderH, TFT_DARKGREY);
    setUiFont();
    drawButton(BtnTerminal, "TERM");
    drawButton(BtnWifi, "WIFI");
    drawButton(BtnSsh, "SSH");
    drawButton(BtnFont, "FONT");
    drawButton(BtnConfig, "CONF");

    if (screen == Screen::Terminal) {
        drawButton(BtnConnect, ssh.connected() ? "DISC" : "CONN", TFT_WHITE, ssh.connected() ? TFT_MAROON : TFT_DARKGREEN);
    } else if (screen == Screen::WifiEdit) {
        drawButton(BtnConnect, "CONN", TFT_WHITE, TFT_DARKGREEN);
        drawButton(BtnSave, "SAVE", TFT_WHITE, TFT_DARKGREEN);
        drawButton(BtnDelete, "DEL", TFT_WHITE, TFT_MAROON);
    } else if (screen == Screen::SshEdit) {
        drawButton(BtnSave, "SAVE", TFT_WHITE, TFT_DARKGREEN);
        drawButton(BtnDelete, "DEL", TFT_WHITE, TFT_MAROON);
    } else if (screen == Screen::ConfigEdit) {
        drawButton(BtnSave, "SAVE", TFT_WHITE, TFT_DARKGREEN);
    }

    screenSprite.setTextColor(TFT_WHITE, TFT_DARKGREY);
    String wifiStateText = "WiFi down";
    if (wifiDisabled) {
        wifiStateText = "WiFi off";
    } else if (WiFi.status() == WL_CONNECTED) {
        wifiStateText = "WiFi ok";
    } else if (wifiState == WifiConnectState::Connecting) {
        wifiStateText = "WiFi conn";
    } else if (wifiState == WifiConnectState::Failed && wifiRetryAt) {
        wifiStateText = "WiFi retry";
    }
    String status = wifiStateText + (ssh.connected() ? "  SSH ok" : "  SSH down");
    if (screen == Screen::FontList) {
        status = String("Font: ") + terminalFont().label + " line " + terminalLineStep();
    } else if (screen == Screen::ConfigEdit) {
        status = String(config.system.deviceName) + " " + config.system.region;
    } else if (screen == Screen::Terminal && ssh.connected() && vt.scrollbackOffset() > 0) {
        status = String("Scroll +") + vt.scrollbackOffset();
    }
    if (status.length() > 42) {
        status = status.substring(0, 42);
    }
    int statusX = screenSprite.width() - screenSprite.textWidth(status) - 8;
    statusX = max<int>(540, statusX);
    screenSprite.drawString(status, statusX, 14);
    drawHeaderFocus();
}

void configureTerminal()
{
    const int top = ssh.connected() ? 0 : HeaderH;
    const size_t columns = max<int>(20, (screenSprite.width() - 8) / terminalCellWidth());
    size_t rows = max<int>(5, (screenSprite.height() - top - 4) / terminalLineStep());
    terminal.setViewport(columns, rows);
    vt.resize(columns, rows);
    vt.markAllDirty();
    dirty = true;
    if (ssh.connected()) {
        ssh.resizePty(static_cast<int>(columns), static_cast<int>(rows));
    }
}

void appendStatus(const String& message)
{
    terminal.append("[tab5] ");
    terminal.append(message);
    terminal.append("\n");
    Serial.print("[tab5] ");
    Serial.println(message);
    statusLine = message;
    dirty = true;
}

bool saveConfig()
{
    config.activeWifi = activeWifi;
    config.activeSsh = activeSsh;
    if (!settings.save(config)) {
        appendStatus(String("Save failed: ") + settings.lastError());
        return false;
    }
    appendStatus("Profiles saved");
    return true;
}

void setWifiStatus(const String& message)
{
    wifiStatusText = message;
    headerDirty = true;
}

void appendWifiProgress(const String& message)
{
    appendStatus(message);
}

String wifiDisconnectSummary()
{
    String status = String("st") + static_cast<int>(WiFi.status());
    String passLen = ForceFixedWifiForTest ? String(" p") + String(FixedWifiPassword).length()
                                           : (wifiProfileIndex < config.wifi.size()
                                                  ? String(" p") + config.wifi[wifiProfileIndex].password.length()
                                                  : "");
    if (!wifiLastDisconnectReason) {
        return String("timeout ") + status + passLen;
    }
    String summary = String("r") + wifiLastDisconnectReason;
    if (wifiLastDisconnectName.length()) {
        summary += " ";
        summary += wifiLastDisconnectName;
    }
    summary += " ";
    summary += status;
    summary += passLen;
    return summary;
}

void setWifiFailureStatus(const String& prefix)
{
    wifiLastFailureText = prefix + " " + wifiDisconnectSummary();
    setWifiStatus(wifiLastFailureText);
    appendWifiProgress(wifiLastFailureText);
}

void stopWifiRuntime()
{
    wifiDisabled = true;
    wifiState = WifiConnectState::Idle;
    wifiRetryAt = 0;
    wifiLastRetrySecond = -1;
    wifiDirectBeginPending = false;
    wifiWorkerDone = false;
    wifiWorkerSsid = "";
    wifiWorkerPassword = "";
    WiFi.scanDelete();
    wifiScanActive = false;
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(true, false);
    }
    setWifiStatus("Wi-Fi off");
    appendWifiProgress("Wi-Fi off");
}

void enableWifiRuntime(uint32_t timeoutMs)
{
    wifiDisabled = false;
    setWifiStatus("Wi-Fi on");
    appendWifiProgress("Wi-Fi on");
    startWifiReconnect(timeoutMs);
}

void handleWifiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        return;
    }
    wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
    const char* name = WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(wifiLastDisconnectReason));
    wifiLastDisconnectName = name ? name : "";
    Serial.printf("Wi-Fi disconnected reason=%u %s\n", wifiLastDisconnectReason, wifiLastDisconnectName.c_str());
}

void configureTab5WifiPins()
{
    if (wifiPinsConfigured) {
        return;
    }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (M5.getBoard() == m5::board_t::board_M5Tab5) {
        WiFi.setPins(GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_11, GPIO_NUM_10,
                     GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_15);
    }
#endif
    wifiPinsConfigured = true;
}

void beginWifiNow(const String& ssid, const String& password)
{
    if (wifiDisabled) {
        wifiWorkerDone = true;
        return;
    }
    configureTab5WifiPins();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(false);
    delay(20);
    if (!wifiDisabled) {
        WiFi.begin(ssid.c_str(), password.c_str());
    }
    wifiWorkerDone = true;
}

bool startWifiBeginWorker(const String& ssid, const String& password)
{
    if (wifiDisabled) {
        return false;
    }
    wifiWorkerSsid = ssid;
    wifiWorkerPassword = password;
    wifiWorkerDone = false;
    wifiWorkerBusy = false;
    wifiDirectBeginPending = true;
    return true;
}

void setCrashStage(const char* stage)
{
    crashStageMagic = 0x54414235;
    strlcpy(crashStage, stage, sizeof(crashStage));
}

void adjustTerminalLineStep(int delta)
{
    int next = static_cast<int>(config.keyboard.terminalLineStep) + delta;
    int minStep = max(12, max(terminalFontHeight(false), terminalFontHeight(true)));
    next = constrain(next, minStep, 80);
    if (next == config.keyboard.terminalLineStep) {
        return;
    }
    config.keyboard.terminalLineStep = static_cast<uint8_t>(next);
    configureTerminal();
    saveConfig();
    dirty = true;
}

void beginWifiAttempt(size_t index)
{
    wifiLastRetrySecond = -1;
    if (wifiDisabled) {
        wifiState = WifiConnectState::Idle;
        wifiRetryAt = 0;
        autoSshConnectPending = false;
        setWifiStatus("Wi-Fi off");
        return;
    }
    if (!ForceFixedWifiForTest && index >= config.wifi.size()) {
        wifiState = WifiConnectState::Failed;
        wifiRetryAt = millis() + 10000;
        autoSshConnectPending = false;
        String reason = wifiLastFailureText.length() ? wifiLastFailureText : String("WiFi fail ") + wifiDisconnectSummary();
        setWifiStatus(reason + "; retry 10s");
        appendWifiProgress(reason + "; retry 10s");
        return;
    }

    wifiProfileIndex = index;
    wifiState = WifiConnectState::Connecting;
    wifiAttemptStart = millis();
    wifiLastDisconnectReason = 0;
    wifiLastDisconnectName = "";
    wifiLastFailureText = "";
    String ssid = ForceFixedWifiForTest ? String(FixedWifiSsid) : config.wifi[wifiProfileIndex].ssid;
    String password = ForceFixedWifiForTest ? String(FixedWifiPassword) : config.wifi[wifiProfileIndex].password;
    appendWifiProgress(String("Wi-Fi begin: ") + ssid);
    if (!startWifiBeginWorker(ssid, password)) {
        setWifiFailureStatus(String("WiFi start fail ") + ssid);
        beginWifiAttempt(wifiProfileIndex + 1);
        return;
    }
    setWifiStatus(String("Wi-Fi direct: ") + ssid + " p" + password.length());
}

void startWifiReconnect(uint32_t timeoutMs)
{
    wifiAttemptTimeoutMs = timeoutMs;
    wifiLastRetrySecond = -1;
    if (wifiDisabled) {
        wifiState = WifiConnectState::Idle;
        wifiRetryAt = 0;
        autoSshConnectPending = false;
        setWifiStatus("Wi-Fi off");
        appendWifiProgress("Wi-Fi off");
        return;
    }
    if (!ForceFixedWifiForTest && config.wifi.empty()) {
        wifiState = WifiConnectState::Failed;
        wifiRetryAt = 0;
        autoSshConnectPending = false;
        setWifiStatus("Wi-Fi fail: no profile");
        appendWifiProgress("Wi-Fi fail: no profile");
        return;
    }
    if (!ForceFixedWifiForTest && activeWifi >= config.wifi.size()) {
        activeWifi = 0;
    }
    autoSshConnectPending = !config.ssh.empty();
    setWifiStatus(ForceFixedWifiForTest ? "Wi-Fi direct test" : "Wi-Fi reconnect");
    appendWifiProgress(ForceFixedWifiForTest ? "Wi-Fi direct test" : "Wi-Fi reconnect");
    beginWifiAttempt(ForceFixedWifiForTest ? 0 : activeWifi);
}

void pollWifi()
{
    if (wifiDisabled) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(true, false);
        }
        return;
    }
    if (wifiScanActive) {
        return;
    }

    if (wifiDirectBeginPending) {
        wifiDirectBeginPending = false;
        beginWifiNow(wifiWorkerSsid, wifiWorkerPassword);
    }

    if (wifiState == WifiConnectState::Connecting) {
        if (wifiWorkerDone) {
            wifiWorkerDone = false;
            String ssid = ForceFixedWifiForTest ? String(FixedWifiSsid) : config.wifi[wifiProfileIndex].ssid;
            setWifiStatus(String("Wi-Fi try: ") + ssid);
            appendWifiProgress(String("Wi-Fi try: ") + ssid);
        }
        if (WiFi.status() == WL_CONNECTED) {
            wifiState = WifiConnectState::Connected;
            setWifiStatus(String("Wi-Fi connected: ") + WiFi.SSID() + " " + WiFi.localIP().toString());
            appendWifiProgress(wifiStatusText);
            startTimeSync(false);
            if (autoSshConnectPending && !ssh.connected() && !config.ssh.empty()) {
                autoSshConnectPending = false;
                connectActiveSsh();
            }
            return;
        }
        if (millis() - wifiAttemptStart >= wifiAttemptTimeoutMs) {
            if (wifiWorkerBusy) {
                String ssid = ForceFixedWifiForTest ? String(FixedWifiSsid) : config.wifi[wifiProfileIndex].ssid;
                setWifiStatus(String("Wi-Fi start busy: ") + ssid);
                appendWifiProgress(String("Wi-Fi start busy: ") + ssid);
                wifiAttemptStart = millis();
                return;
            }
            String ssid = ForceFixedWifiForTest ? String(FixedWifiSsid) : config.wifi[wifiProfileIndex].ssid;
            setWifiFailureStatus(String("WiFi fail ") + ssid);
            if (ForceFixedWifiForTest) {
                wifiState = WifiConnectState::Failed;
                wifiRetryAt = millis() + 10000;
                autoSshConnectPending = false;
                return;
            }
            beginWifiAttempt(wifiProfileIndex + 1);
        }
        return;
    }

    if (wifiState == WifiConnectState::Connected && WiFi.status() != WL_CONNECTED) {
        wifiState = WifiConnectState::Failed;
        wifiRetryAt = millis() + 3000;
        setWifiStatus("Wi-Fi lost; reconnect");
        appendWifiProgress("Wi-Fi lost; reconnect");
        return;
    }

    if (wifiState == WifiConnectState::Failed && config.wifi.size() && wifiRetryAt) {
        uint32_t now = millis();
        if (now >= wifiRetryAt) {
            wifiRetryAt = 0;
            wifiLastRetrySecond = -1;
            startWifiReconnect(wifiAttemptTimeoutMs);
        } else {
            int16_t remaining = static_cast<int16_t>((wifiRetryAt - now + 999) / 1000);
            if (remaining != wifiLastRetrySecond) {
                wifiLastRetrySecond = remaining;
                String reason = wifiLastFailureText.length() ? wifiLastFailureText : String("WiFi fail ") + wifiDisconnectSummary();
                setWifiStatus(reason + String("; retry ") + remaining + "s");
                appendWifiProgress(reason + String("; retry ") + remaining + "s");
            }
        }
    }
}

void connectActiveSsh()
{
    if (config.ssh.empty()) {
        appendStatus("No SSH profiles");
        autoSshConnectPending = false;
        screen = Screen::Terminal;
        return;
    }
    if (sshConnectJobRunning) {
        appendStatus("SSH connect already running");
        screen = Screen::Terminal;
        return;
    }
    if (activeSsh >= config.ssh.size()) {
        activeSsh = 0;
    }
    resetStorageBridgeState();
    ssh.disconnect();
    screen = Screen::Terminal;
    dirty = true;
    appendStatus(String("Connecting SSH: ") + config.ssh[activeSsh].host);
    setCrashStage("ssh.connect");
    sshConnectJobProfile = config.ssh[activeSsh];
    sshConnectJobColumns = static_cast<int>(vt.columns());
    sshConnectJobRows = static_cast<int>(vt.rows());
    sshConnectJobError = "";
    sshConnectJobOk = false;
    sshConnectJobDone = false;
    sshConnectJobRunning = true;
    BaseType_t created = xTaskCreatePinnedToCore(
        [](void*) {
            String err;
            bool ok = ssh.connect(sshConnectJobProfile, err, sshConnectJobColumns, sshConnectJobRows);
            sshConnectJobError = err;
            sshConnectJobOk = ok;
            sshConnectJobDone = true;
            sshConnectJobRunning = false;
            sshConnectTaskHandle = nullptr;
            vTaskDelete(nullptr);
        },
        "ssh_connect",
        16384,
        nullptr,
        1,
        &sshConnectTaskHandle,
        0);
    if (created != pdPASS) {
        sshConnectTaskHandle = nullptr;
        sshConnectJobRunning = false;
        sshConnectJobDone = false;
        sshConnectJobOk = false;
        setCrashStage("ssh.failed");
        appendStatus("SSH failed: task create");
    }
}

void pollSshConnectJob()
{
    if (!sshConnectJobDone) {
        return;
    }
    sshConnectJobDone = false;
    screen = Screen::Terminal;
    if (sshConnectJobOk) {
        setCrashStage("ssh.connected");
        resetCommandEditor();
        vt.reset();
        vt.scrollbackToBottom();
        configureTerminal();
        appendStatus("SSH connected");
        startStorageBridgeAsync();
    } else {
        setCrashStage("ssh.failed");
        appendStatus(String("SSH failed: ") + sshConnectJobError);
        resetCommandEditor();
        configureTerminal();
    }
    dirty = true;
}

bool connectSshProfile(const SshProfile& profile)
{
    resetStorageBridgeState();
    ssh.disconnect();
    String err;
    screen = Screen::Terminal;
    dirty = true;
    appendStatus(String("Connecting SSH: ") + profile.user + "@" + profile.host);
    draw();
    setCrashStage("ssh.connect.direct");
    if (ssh.connect(profile, err, static_cast<int>(vt.columns()), static_cast<int>(vt.rows()))) {
        setCrashStage("ssh.connected");
        resetCommandEditor();
        vt.reset();
        vt.scrollbackToBottom();
        configureTerminal();
        appendStatus("SSH connected");
        startStorageBridge();
        screen = Screen::Terminal;
        return true;
    }
    setCrashStage("ssh.failed");
    appendStatus(String("SSH failed: ") + err);
    return false;
}

void inheritSavedSshCredentials(SshProfile& profile)
{
    for (const auto& saved : config.ssh) {
        if (saved.host == profile.host && saved.user == profile.user && saved.port == profile.port) {
            profile.password = saved.password;
            profile.terminal = saved.terminal.length() ? saved.terminal : "xterm-256color";
            return;
        }
    }
    for (const auto& saved : config.ssh) {
        if (saved.host == profile.host && saved.user == profile.user) {
            profile.password = saved.password;
            profile.terminal = saved.terminal.length() ? saved.terminal : "xterm-256color";
            return;
        }
    }
}

bool parseSshCommand(const String& line, SshProfile& profile, String& error)
{
    String rest = line;
    rest.trim();
    if (!rest.startsWith("ssh ")) {
        error = "Command not found";
        return false;
    }
    rest = rest.substring(4);
    rest.trim();
    if (!rest.length()) {
        error = "Usage: ssh [-p port] user@host";
        return false;
    }

    profile = SshProfile{};
    profile.port = 22;
    profile.terminal = "xterm-256color";

    int pIndex = rest.indexOf("-p ");
    if (pIndex >= 0) {
        String before = rest.substring(0, pIndex);
        String after = rest.substring(pIndex + 3);
        before.trim();
        after.trim();
        int nextSpace = after.indexOf(' ');
        String portText = nextSpace >= 0 ? after.substring(0, nextSpace) : after;
        if (!portText.length() || portText.toInt() <= 0 || portText.toInt() > 65535) {
            error = "Invalid SSH port";
            return false;
        }
        profile.port = static_cast<uint16_t>(portText.toInt());
        String remaining = nextSpace >= 0 ? after.substring(nextSpace + 1) : "";
        remaining.trim();
        rest = before.length() ? before : remaining;
        rest.trim();
    }

    int space = rest.indexOf(' ');
    if (space >= 0) {
        rest = rest.substring(0, space);
    }
    int at = rest.indexOf('@');
    if (at <= 0 || at >= static_cast<int>(rest.length()) - 1) {
        error = "Usage: ssh [-p port] user@host";
        return false;
    }
    profile.user = rest.substring(0, at);
    profile.host = rest.substring(at + 1);
    int colon = profile.host.lastIndexOf(':');
    if (colon > 0 && colon < static_cast<int>(profile.host.length()) - 1) {
        String portText = profile.host.substring(colon + 1);
        bool numeric = true;
        for (size_t i = 0; i < portText.length(); ++i) {
            numeric = numeric && std::isdigit(static_cast<unsigned char>(portText[i]));
        }
        if (numeric) {
            int port = portText.toInt();
            if (port <= 0 || port > 65535) {
                error = "Invalid SSH port";
                return false;
            }
            profile.port = static_cast<uint16_t>(port);
            profile.host = profile.host.substring(0, colon);
        }
    }
    profile.name = String("direct ") + profile.user + "@" + profile.host;
    inheritSavedSshCredentials(profile);
    return true;
}

String safeValue(const String& value, bool secret = false)
{
    if (!secret) {
        return value;
    }
    String masked;
    for (size_t i = 0; i < value.length(); ++i) {
        masked += '*';
    }
    return masked;
}

String wifiFieldValue(uint8_t field)
{
    if (editIndex >= config.wifi.size()) {
        return "";
    }
    const auto& p = config.wifi[editIndex];
    if (field == 0) return p.name;
    if (field == 1) return p.ssid;
    return p.password;
}

void setWifiFieldValue(uint8_t field, const String& value)
{
    if (editIndex >= config.wifi.size()) {
        return;
    }
    auto& p = config.wifi[editIndex];
    if (field == 0) p.name = value;
    if (field == 1) p.ssid = value;
    if (field == 2) p.password = value;
}

String sshFieldValue(uint8_t field)
{
    if (editIndex >= config.ssh.size()) {
        return "";
    }
    const auto& p = config.ssh[editIndex];
    if (field == 0) return p.name;
    if (field == 1) return p.host;
    if (field == 2) return String(p.port);
    if (field == 3) return p.user;
    if (field == 4) return p.password;
    return p.terminal;
}

void setSshFieldValue(uint8_t field, const String& value)
{
    if (editIndex >= config.ssh.size()) {
        return;
    }
    auto& p = config.ssh[editIndex];
    if (field == 0) p.name = value;
    if (field == 1) p.host = value;
    if (field == 2) p.port = static_cast<uint16_t>(constrain(value.toInt(), 1, 65535));
    if (field == 3) p.user = value;
    if (field == 4) p.password = value;
    if (field == 5) p.terminal = value.length() ? value : "xterm-256color";
}

String configFieldValue(uint8_t field)
{
    if (field == 0) return config.system.deviceName;
    if (field == 1) return config.system.region;
    if (field == 2) return String(config.system.utcOffsetMinutes);
    if (field == 3) return config.system.ntpServer;
    if (field == 4) return config.keyboard.layout;
    if (field == 5) return config.keyboard.bleKeyboardEnabled ? "on" : "off";
    if (field == 6) return config.keyboard.bleKeyboardName;
    if (field == 7) return config.keyboard.bleKeyboardAddress;
    if (field == 8) {
        String entry = keyboard.bleScanEntry(blePairTarget);
        return entry.length() ? entry : String(blePairTarget);
    }
    if (field == 9) return "scan now";
    if (field == 10) return "pair selected";
    return "forget paired";
}

void setConfigFieldValue(uint8_t field, const String& value)
{
    if (field == 0) config.system.deviceName = value.length() ? value : "tab5";
    if (field == 1) config.system.region = value.length() ? value : "Asia/Tokyo";
    if (field == 2) config.system.utcOffsetMinutes = static_cast<int16_t>(constrain(value.toInt(), -720, 840));
    if (field == 3) config.system.ntpServer = value.length() ? value : "pool.ntp.org";
    if (field == 4) {
        String layout = value;
        layout.toLowerCase();
        config.keyboard.layout = layout == "jp" ? "jp" : "us";
    }
    if (field == 5) {
        String enabled = value;
        enabled.toLowerCase();
        config.keyboard.bleKeyboardEnabled = enabled == "on" || enabled == "1" || enabled == "yes" || enabled == "true";
    }
    if (field == 6) config.keyboard.bleKeyboardName = value;
    if (field == 7) config.keyboard.bleKeyboardAddress = value;
    if (field == 8) blePairTarget = static_cast<size_t>(max<int>(0, value.toInt()));
}

uint8_t editFieldCount()
{
    if (screen == Screen::WifiEdit) return 3;
    if (screen == Screen::SshEdit) return 6;
    if (screen == Screen::ConfigEdit) return 12;
    return 0;
}

String currentEditFieldValue()
{
    if (screen == Screen::WifiEdit) return wifiFieldValue(editField);
    if (screen == Screen::SshEdit) return sshFieldValue(editField);
    if (screen == Screen::ConfigEdit) return configFieldValue(editField);
    return "";
}

void setCurrentEditFieldValue(const String& value)
{
    if (screen == Screen::WifiEdit) setWifiFieldValue(editField, value);
    if (screen == Screen::SshEdit) setSshFieldValue(editField, value);
    if (screen == Screen::ConfigEdit) setConfigFieldValue(editField, value);
}

void saveBlePairingFromKeyboard()
{
    String address = keyboard.bleAddress();
    if (!address.length()) {
        return;
    }
    BleHidProfile* existing = nullptr;
    for (auto& profile : config.keyboard.bleDevices) {
        if (profile.address == address) {
            existing = &profile;
            break;
        }
    }
    if (!existing && keyboard.bleName().length()) {
        for (auto& profile : config.keyboard.bleDevices) {
            if (profile.name == keyboard.bleName() &&
                (profile.kind == keyboard.bleKind() || (!profile.kind.length() && keyboard.bleKind() == "keyboard"))) {
                existing = &profile;
                break;
            }
        }
    }
    if (!existing) {
        config.keyboard.bleDevices.push_back(BleHidProfile{});
        existing = &config.keyboard.bleDevices.back();
    }
    existing->name = keyboard.bleName().length() ? keyboard.bleName() : "BLE HID";
    existing->address = address;
    existing->addressType = keyboard.bleAddressType();
    existing->kind = keyboard.bleKind().length() ? keyboard.bleKind() : "keyboard";
    existing->enabled = true;
    for (size_t i = 0; i < config.keyboard.bleDevices.size(); ++i) {
        if (&config.keyboard.bleDevices[i] == existing) {
            config.keyboard.activeBle = i;
            break;
        }
    }
    config.keyboard.bleKeyboardName = existing->name;
    config.keyboard.bleKeyboardAddress = existing->address;
    saveConfig();
    keyboard.configure(config.keyboard);
}

bool removeBleDeviceConfig(int index, String& result)
{
    if (index < 0) {
        config.keyboard.bleDevices.clear();
        config.keyboard.activeBle = 0;
        config.keyboard.bleKeyboardName = "";
        config.keyboard.bleKeyboardAddress = "";
        keyboard.configure(config.keyboard);
        saveConfig();
        result = "BLE device list cleared";
        return true;
    }
    if (static_cast<size_t>(index) >= config.keyboard.bleDevices.size()) {
        result = "invalid BLE device index";
        return false;
    }
    config.keyboard.bleDevices.erase(config.keyboard.bleDevices.begin() + index);
    if (config.keyboard.activeBle >= config.keyboard.bleDevices.size()) {
        config.keyboard.activeBle = config.keyboard.bleDevices.empty() ? 0 : config.keyboard.bleDevices.size() - 1;
    }
    if (config.keyboard.bleDevices.size()) {
        const auto& active = config.keyboard.bleDevices[config.keyboard.activeBle];
        config.keyboard.bleKeyboardName = active.name;
        config.keyboard.bleKeyboardAddress = active.address;
    } else {
        config.keyboard.bleKeyboardName = "";
        config.keyboard.bleKeyboardAddress = "";
    }
    keyboard.configure(config.keyboard);
    saveConfig();
    result = "BLE device removed";
    return true;
}

void executeConfigBleAction(uint8_t field)
{
    if (field == 5) {
        config.keyboard.bleKeyboardEnabled = !config.keyboard.bleKeyboardEnabled;
        keyboard.configure(config.keyboard);
        saveConfig();
        appendStatus(String("BLE keyboard ") + (config.keyboard.bleKeyboardEnabled ? "enabled" : "disabled"));
    } else if (field == 8) {
        size_t count = keyboard.bleScanCount();
        blePairTarget = count ? (blePairTarget + 1) % count : 0;
        appendStatus(String("BLE target ") + blePairTarget);
    } else if (field == 9) {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleScan(result);
        blePairTarget = 0;
        appendStatus(String(ok ? "BLE scan: " : "BLE scan failed: ") + result);
    } else if (field == 10) {
        String result;
        bool ok = keyboard.blePair(blePairTarget, result);
        if (ok) {
            saveBlePairingFromKeyboard();
        }
        appendStatus(String(ok ? "BLE pair: " : "BLE pair failed: ") + result);
    } else if (field == 11) {
        String result;
        bool ok = keyboard.bleForget(result);
        String configResult;
        removeBleDeviceConfig(-1, configResult);
        appendStatus(String(ok ? "BLE forget: " : "BLE forget failed: ") + result);
    }
    editCursor = configFieldValue(editField).length();
    dirty = true;
}

bool isChoiceEditField()
{
    return screen == Screen::ConfigEdit && (editField == 4 || editField == 5 || editField >= 8);
}

void toggleChoiceEditField()
{
    if (!isChoiceEditField()) {
        return;
    }
    if (editField == 4) {
        config.keyboard.layout = config.keyboard.layout == "jp" ? "us" : "jp";
        editCursor = config.keyboard.layout.length();
    } else if (editField >= 5) {
        executeConfigBleAction(editField);
        return;
    }
    dirty = true;
}

void clampEditCursor()
{
    String value = currentEditFieldValue();
    if (editCursor > value.length()) {
        editCursor = value.length();
    }
}

void setEditCursorToEnd()
{
    editCursor = currentEditFieldValue().length();
}

void moveEditCursor(int delta)
{
    clampEditCursor();
    String value = currentEditFieldValue();
    if (delta < 0) {
        if (!editCursor) return;
        --editCursor;
        while (editCursor > 0 && (static_cast<uint8_t>(value[editCursor]) & 0xC0) == 0x80) {
            --editCursor;
        }
    } else if (delta > 0 && editCursor < value.length()) {
        editCursor += utf8CharLength(static_cast<uint8_t>(value[editCursor]));
        if (editCursor > value.length()) editCursor = value.length();
    }
    cursorVisible = true;
    lastCursorBlink = millis();
    dirty = true;
}

void drawTerminal()
{
    if (ssh.connected()) {
        drawVtTerminal();
        return;
    }
    setTerminalFont();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    const int lineStep = terminalLineStep();
    const int top = terminalTop();
    const bool drawEditor = terminal.atBottom();
    const size_t editorRow = drawEditor ? terminal.inputViewportRow() : terminal.viewportRows();
    for (size_t row = 0; row < terminal.viewportRows(); ++row) {
        String line = terminal.lineAt(row);
        if (drawEditor && row == editorRow) {
            int lineTop = top + static_cast<int>(row) * lineStep;
            clampCommandCursor();
            String prefix = String(pythonReplMode ? PythonPrompt : localShellPrompt()) + commandLine.substring(0, commandCursor);
            String cursorGlyph = " ";
            String suffix = "";
            if (commandCursor < commandLine.length()) {
                size_t next = commandCursor + utf8CharLength(static_cast<uint8_t>(commandLine[commandCursor]));
                cursorGlyph = commandLine.substring(commandCursor, next);
                suffix = commandLine.substring(next);
            }

            size_t visibleStart = 0;
            while (visibleStart < prefix.length() &&
                   terminalTextWidth(prefix.substring(visibleStart)) > screenSprite.width() - 16) {
                ++visibleStart;
                while (visibleStart < prefix.length() &&
                       (static_cast<uint8_t>(prefix[visibleStart]) & 0xC0) == 0x80) {
                    ++visibleStart;
                }
            }

            String visiblePrefix = prefix.substring(visibleStart);
            int x = 4;
            x += drawTerminalText(visiblePrefix, x, lineTop, lineStep, TFT_GREEN, TFT_BLACK);
            if (cursorVisible) {
                int cursorW = max<int>(terminalCellWidth(), terminalTextWidth(cursorGlyph));
                screenSprite.fillRect(x, lineTop, cursorW, lineStep, TFT_GREEN);
                drawTerminalText(cursorGlyph, x, lineTop, lineStep, TFT_BLACK, TFT_GREEN);
                x += cursorW;
            } else {
                x += drawTerminalText(cursorGlyph, x, lineTop, lineStep, TFT_GREEN, TFT_BLACK);
            }
            if (suffix.length()) {
                drawTerminalText(suffix, x, lineTop, lineStep, TFT_GREEN, TFT_BLACK);
            }
            continue;
        }
        if (line.length() > 0) {
            drawMixedTerminalLine(line, 4, top + static_cast<int>(row) * lineStep, lineStep);
        }
    }
}

void drawWifiList()
{
    clampSettingScroll(config.wifi.size());
    drawSettingsTitle("Wi-Fi profiles");
    setSettingsFontForLine();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    drawFocusedBodyButton(BodyBtn1, "SCAN", 0);
    drawFocusedBodyButton(BodyBtn2, "ADD", 1);
    drawFocusedBodyButton(BodyBtn3, "CONNECT", 2, TFT_DARKGREEN);
    drawFocusedBodyButton(BodyBtn4, wifiDisabled ? "ON" : "OFF", 3, wifiDisabled ? TFT_DARKGREEN : TFT_MAROON);
    for (size_t row = 0; row < visibleSettingRows(); ++row) {
        size_t i = settingScrollOffset + row;
        if (i >= config.wifi.size()) {
            break;
        }
        int y = settingListTop() + static_cast<int>(row) * settingRowH();
        bool selected = !keyboardMenuMode && focusedContentItem == i + 4;
        uint16_t bg = selected ? TFT_NAVY : TFT_BLACK;
        screenSprite.fillRect(4, y, screenSprite.width() - 8, settingRowH() - 4, bg);
        screenSprite.drawRect(4, y, screenSprite.width() - 8, settingRowH() - 4, selected ? TFT_CYAN : TFT_DARKGREY);
        String line = String(i == activeWifi ? "* " : "  ") + config.wifi[i].name + " | " + config.wifi[i].ssid;
        setSettingsFontForLine(line);
        screenSprite.setTextColor(TFT_WHITE, bg);
        screenSprite.drawString(line, 12, settingTextY(y));
        setSettingsFontForLine();
        screenSprite.setTextColor(TFT_WHITE, bg);
        screenSprite.drawString(i == activeWifi ? "ACTIVE" : "SELECT", screenSprite.width() - 170, settingTextY(y));
    }
}

void drawWifiScan()
{
    clampSettingScroll(scannedNetworks.size());
    drawSettingsTitle("Wi-Fi scan");
    setSettingsFontForLine();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    drawFocusedBodyButton(BodyBtn1, "RESCAN", 0);
    drawFocusedBodyButton(BodyBtn2, "BACK", 1);
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    String title = wifiScanActive ? "Wi-Fi Scan: running..." : String("Wi-Fi Scan: ") + scannedNetworks.size() + " SSIDs";
    screenSprite.drawString(title, 256, HeaderH + 8);
    for (size_t row = 0; row < visibleSettingRows(); ++row) {
        size_t i = settingScrollOffset + row;
        if (i >= scannedNetworks.size()) {
            break;
        }
        int y = settingListTop() + static_cast<int>(row) * settingRowH();
        bool selected = !keyboardMenuMode && focusedContentItem == i + 2;
        uint16_t bg = selected ? TFT_NAVY : TFT_BLACK;
        screenSprite.fillRect(4, y, screenSprite.width() - 8, settingRowH() - 4, bg);
        screenSprite.drawRect(4, y, screenSprite.width() - 8, settingRowH() - 4, selected ? TFT_CYAN : TFT_DARKGREY);
        String security = scannedNetworks[i].auth == WIFI_AUTH_OPEN ? "open" : "secured";
        String line = scannedNetworks[i].ssid + " | " + scannedNetworks[i].rssi + " dBm | " + security;
        setSettingsFontForLine(line);
        screenSprite.setTextColor(TFT_WHITE, bg);
        screenSprite.drawString(line, 12, settingTextY(y));
    }
}

void drawSshList()
{
    clampSettingScroll(config.ssh.size());
    drawSettingsTitle("SSH profiles");
    setSettingsFontForLine();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    drawFocusedBodyButton(BodyBtn1, "ADD", 0);
    drawFocusedBodyButton(BodyBtn2, "EDIT", 1);
    drawFocusedBodyButton(BodyBtn3, "CONNECT", 2, TFT_DARKGREEN);
    for (size_t row = 0; row < visibleSettingRows(); ++row) {
        size_t i = settingScrollOffset + row;
        if (i >= config.ssh.size()) {
            break;
        }
        int y = settingListTop() + static_cast<int>(row) * settingRowH();
        bool selected = !keyboardMenuMode && focusedContentItem == i + 3;
        uint16_t bg = selected ? TFT_NAVY : TFT_BLACK;
        screenSprite.fillRect(4, y, screenSprite.width() - 8, settingRowH() - 4, bg);
        screenSprite.drawRect(4, y, screenSprite.width() - 8, settingRowH() - 4, selected ? TFT_CYAN : TFT_DARKGREY);
        String line = String(i == activeSsh ? "* " : "  ") + config.ssh[i].name + " | " + config.ssh[i].user + "@" +
                      config.ssh[i].host + ":" + config.ssh[i].port;
        setSettingsFontForLine(line);
        screenSprite.setTextColor(TFT_WHITE, bg);
        screenSprite.drawString(line, 12, settingTextY(y));
        setSettingsFontForLine();
        screenSprite.setTextColor(TFT_WHITE, bg);
        screenSprite.drawString(i == activeSsh ? "ACTIVE" : "SELECT", screenSprite.width() - 170, settingTextY(y));
    }
}

void drawEditFields(const char* title, const char* const* labels, uint8_t count, bool sshFields)
{
    drawSettingsTitle(title);
    for (uint8_t i = 0; i < count; ++i) {
        int y = settingListTop() + i * settingRowH();
        uint16_t bg = i == editField ? TFT_NAVY : TFT_BLACK;
        screenSprite.fillRect(4, y, screenSprite.width() - 8, settingRowH() - 4, bg);
        screenSprite.drawRect(4, y, screenSprite.width() - 8, settingRowH() - 4, i == editField ? TFT_CYAN : TFT_DARKGREY);
        screenSprite.setTextColor(TFT_WHITE, bg);
        setSettingsFontForLine(labels[i]);
        screenSprite.drawString(labels[i], 12, settingTextY(y));
        String rawValue;
        if (screen == Screen::WifiEdit) rawValue = wifiFieldValue(i);
        else if (screen == Screen::SshEdit) rawValue = sshFieldValue(i);
        else rawValue = configFieldValue(i);
        bool secret = (screen == Screen::SshEdit && i == 4) || (screen == Screen::WifiEdit && i == 2);
        bool choiceField = screen == Screen::ConfigEdit && (i == 4 || i == 5 || i >= 8);
        String value = safeValue(rawValue, secret);
        size_t cursor = (i == editField && !choiceField) ? min(editCursor, rawValue.length()) : rawValue.length();
        if (secret) cursor = min(cursor, value.length());
        String prefix = value.substring(0, cursor);
        String cursorGlyph = " ";
        String suffix = "";
        if (cursor < value.length()) {
            size_t next = cursor + utf8CharLength(static_cast<uint8_t>(value[cursor]));
            cursorGlyph = value.substring(cursor, next);
            suffix = value.substring(next);
        }
        while (prefix.length() && screenSprite.textWidth(prefix + cursorGlyph + suffix) > screenSprite.width() - 195) {
            uint8_t len = utf8CharLength(static_cast<uint8_t>(prefix[0]));
            prefix = prefix.substring(len);
        }
        setSettingsFontForLine(value);
        int x = 180;
        int textY = settingTextY(y);
        screenSprite.drawString(prefix, x, textY);
        x += screenSprite.textWidth(prefix);
        if (i == editField && !choiceField && cursorVisible) {
            int cursorW = max<int>(terminalCellWidth(), screenSprite.textWidth(cursorGlyph));
            screenSprite.fillRect(x, y + 3, cursorW, settingRowH() - 10, TFT_GREEN);
            screenSprite.setTextColor(TFT_BLACK, TFT_GREEN);
            screenSprite.drawString(cursorGlyph, x, textY);
            screenSprite.setTextColor(TFT_WHITE, bg);
            x += cursorW;
        } else {
            screenSprite.drawString(cursorGlyph, x, textY);
            x += screenSprite.textWidth(cursorGlyph);
        }
        if (suffix.length()) {
            screenSprite.drawString(suffix, x, textY);
        }
    }
}

void drawFontList()
{
    drawSettingsTitle("Font settings");
    setSettingsFontForLine();
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    drawFocusedBodyButton(FontMinusBtn, "-", 0);
    drawFocusedBodyButton(FontPlusBtn, "+", 1);
    drawFocusedBodyButton(FontSaveBtn, "SAVE", 2, TFT_DARKGREEN);
    screenSprite.setTextColor(TFT_GREEN, TFT_BLACK);
    screenSprite.drawString("Terminal font", 428, HeaderH + 16);
    screenSprite.drawString(String("Line step: ") + terminalLineStep() + " px", 428, HeaderH + 40);

    constexpr size_t fontCount = sizeof(TerminalFonts) / sizeof(TerminalFonts[0]);
    for (size_t i = 0; i < fontCount; ++i) {
        int y = settingListTop() + static_cast<int>(i) * settingRowH();
        bool selected = config.keyboard.terminalFont == TerminalFonts[i].id;
        bool focused = !keyboardMenuMode && focusedContentItem == i + 3;
        uint16_t bg = selected ? TFT_NAVY : TFT_BLACK;
        screenSprite.fillRect(4, y, screenSprite.width() - 8, settingRowH() - 4, bg);
        screenSprite.drawRect(4, y, screenSprite.width() - 8, settingRowH() - 4, focused ? TFT_CYAN : TFT_DARKGREY);
        screenSprite.setTextColor(TFT_WHITE, bg);
        String line = String(selected ? "* " : "  ") + TerminalFonts[i].label + "  default line " +
                      TerminalFonts[i].defaultLineStep + " px";
        setSettingsFontForLine(line);
        screenSprite.drawString(line, 12, settingTextY(y));
    }
}

void draw()
{
    screenSprite.startWrite();
    if (!(screen == Screen::Terminal && ssh.connected())) {
        screenSprite.fillScreen(TFT_BLACK);
    }
    setUiFont();
    if (terminalUsesHeader()) {
        drawHeader();
    }

    if (screen == Screen::Terminal) {
        drawTerminal();
    } else if (screen == Screen::WifiList) {
        drawWifiList();
    } else if (screen == Screen::WifiScan) {
        drawWifiScan();
    } else if (screen == Screen::SshList) {
        drawSshList();
    } else if (screen == Screen::WifiEdit) {
        static const char* const labels[] = {"Name", "SSID", "Password"};
        drawEditFields("Edit Wi-Fi", labels, 3, false);
    } else if (screen == Screen::SshEdit) {
        static const char* const labels[] = {"Name", "Host", "Port", "User", "Password", "Term"};
        drawEditFields("Edit SSH", labels, 6, true);
    } else if (screen == Screen::ConfigEdit) {
        static const char* const labels[] = {"Device",     "Region",   "UTC min",    "NTP",
                                             "Keymap",     "BLE KB",   "BLE Name",   "BLE Addr",
                                             "BLE Target", "BLE Scan", "BLE Pair",   "BLE Forget"};
        drawEditFields("Config", labels, 12, false);
    } else if (screen == Screen::FontList) {
        drawFontList();
    }
    screenSprite.endWrite();

    if (imageOverlayActive) {
        drawImageOverlay(false);
    }

    screenSprite.pushSprite(0, 0);
    if (imageOverlayActive) {
        drawImageOverlayDirect();
        imageOverlayDrawn = true;
    }
    dirty = false;
    headerDirty = false;
}

void drawHeaderOnly()
{
    screenSprite.startWrite();
    drawHeader();
    screenSprite.endWrite();
    screenSprite.pushSprite(0, 0);
    headerDirty = false;
}

void beginWifiEdit(size_t index, bool isNew)
{
    editIndex = index;
    editField = 0;
    editIsNew = isNew;
    screen = Screen::WifiEdit;
    setEditCursorToEnd();
    dirty = true;
}

void beginSshEdit(size_t index, bool isNew)
{
    editIndex = index;
    editField = 0;
    editIsNew = isNew;
    screen = Screen::SshEdit;
    setEditCursorToEnd();
    dirty = true;
}

void addProfile()
{
    if (screen == Screen::WifiList) {
        WifiProfile p;
        p.name = String("wifi-") + (config.wifi.size() + 1);
        p.ssid = "";
        p.password = "";
        config.wifi.push_back(p);
        activeWifi = config.wifi.size() - 1;
        beginWifiEdit(activeWifi, true);
    } else if (screen == Screen::SshList) {
        SshProfile p;
        p.name = String("ssh-") + (config.ssh.size() + 1);
        p.host = "";
        p.port = 22;
        p.user = "";
        p.password = "";
        p.terminal = "xterm-256color";
        config.ssh.push_back(p);
        activeSsh = config.ssh.size() - 1;
        beginSshEdit(activeSsh, true);
    }
}

int findWifiProfileBySsid(const String& ssid)
{
    for (size_t i = 0; i < config.wifi.size(); ++i) {
        if (config.wifi[i].ssid == ssid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool scannedSsidExists(const String& ssid)
{
    for (const auto& network : scannedNetworks) {
        if (network.ssid == ssid) {
            return true;
        }
    }
    return false;
}

void scanWifiNetworks()
{
    if (wifiDisabled) {
        setWifiStatus("Wi-Fi off");
        appendStatus("Wi-Fi is off; press ON first");
        dirty = true;
        return;
    }
    if (wifiScanActive) {
        setWifiStatus("Wi-Fi scan running");
        dirty = true;
        return;
    }

    statusLine = "Scanning Wi-Fi...";
    setWifiStatus("Wi-Fi scanning");
    scannedNetworks.clear();
    screen = Screen::WifiScan;
    settingScrollOffset = 0;
    focusedContentItem = 0;
    wifiScanActive = true;
    wifiState = WifiConnectState::Idle;
    wifiRetryAt = 0;
    dirty = true;

    configureTab5WifiPins();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    WiFi.scanDelete();
    if (!WiFi.scanNetworks(true, true)) {
        wifiScanActive = false;
        appendStatus("Wi-Fi scan start failed");
        screen = Screen::WifiList;
    }
}

void finishWifiScan(int count)
{
    wifiScanActive = false;
    scannedNetworks.clear();
    if (count < 0) {
        appendStatus("Wi-Fi scan failed");
        screen = Screen::WifiList;
        return;
    }

    for (int i = 0; i < count; ++i) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length() || scannedSsidExists(ssid)) {
            continue;
        }
        scannedNetworks.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i)});
    }
    WiFi.scanDelete();

    std::sort(scannedNetworks.begin(), scannedNetworks.end(), [](const ScannedNetwork& a, const ScannedNetwork& b) {
        return a.rssi > b.rssi;
    });

    statusLine = String("Wi-Fi scan: ") + scannedNetworks.size() + " SSIDs";
    setWifiStatus(statusLine);
    screen = Screen::WifiScan;
    settingScrollOffset = 0;
    focusedContentItem = 2;
    dirty = true;
}

void pollWifiScan()
{
    if (!wifiScanActive) {
        return;
    }

    int count = WiFi.scanComplete();
    if (count == -1) {
        return;
    }
    finishWifiScan(count);
}

void selectScannedWifi(size_t index)
{
    if (index >= scannedNetworks.size()) {
        return;
    }

    int existing = findWifiProfileBySsid(scannedNetworks[index].ssid);
    if (existing >= 0) {
        activeWifi = static_cast<size_t>(existing);
        beginWifiEdit(activeWifi, false);
    } else {
        WifiProfile p;
        p.ssid = scannedNetworks[index].ssid;
        p.name = p.ssid.length() ? p.ssid : String("wifi-") + (config.wifi.size() + 1);
        p.password = "";
        config.wifi.push_back(p);
        activeWifi = config.wifi.size() - 1;
        beginWifiEdit(activeWifi, true);
    }
    editField = 2;
    dirty = true;
}

void deleteEditingProfile()
{
    if (screen == Screen::WifiEdit && editIndex < config.wifi.size()) {
        config.wifi.erase(config.wifi.begin() + editIndex);
        if (activeWifi >= config.wifi.size()) {
            activeWifi = 0;
        }
        saveConfig();
        screen = Screen::WifiList;
    } else if (screen == Screen::SshEdit && editIndex < config.ssh.size()) {
        config.ssh.erase(config.ssh.begin() + editIndex);
        if (activeSsh >= config.ssh.size()) {
            activeSsh = 0;
        }
        saveConfig();
        screen = Screen::SshList;
    }
    dirty = true;
}

void saveEditingProfile()
{
    (void)editIsNew;
    if (saveConfig()) {
        if (screen == Screen::WifiEdit) {
            screen = Screen::WifiList;
        } else if (screen == Screen::SshEdit) {
            screen = Screen::SshList;
        } else if (screen == Screen::ConfigEdit) {
            startTimeSync(true);
            keyboard.configure(config.keyboard);
            screen = Screen::Terminal;
        }
    }
    dirty = true;
}

bool handleHeaderTouch(int x, int y)
{
    keyboardMenuMode = false;
    if (headerButtonContains(BtnTerminal, x, y)) {
        screen = Screen::Terminal;
        dirty = true;
    } else if (headerButtonContains(BtnWifi, x, y)) {
        screen = Screen::WifiList;
        settingScrollOffset = 0;
        focusedContentItem = 0;
        dirty = true;
    } else if (headerButtonContains(BtnSsh, x, y)) {
        screen = Screen::SshList;
        settingScrollOffset = 0;
        focusedContentItem = 0;
        dirty = true;
    } else if (headerButtonContains(BtnFont, x, y)) {
        screen = Screen::FontList;
        settingScrollOffset = 0;
        focusedContentItem = 0;
        dirty = true;
    } else if (headerButtonContains(BtnConfig, x, y)) {
        screen = Screen::ConfigEdit;
        editField = 0;
        setEditCursorToEnd();
        dirty = true;
    } else if (screen == Screen::Terminal && headerButtonContains(BtnConnect, x, y)) {
        if (ssh.connected()) {
            resetStorageBridgeState();
            ssh.disconnect();
            resetCommandEditor();
            configureTerminal();
            appendStatus("SSH disconnected");
        } else {
            connectActiveSsh();
        }
    } else if (screen == Screen::WifiEdit && headerButtonContains(BtnConnect, x, y)) {
        if (saveConfig()) {
            startWifiReconnect(20000);
        }
        dirty = true;
    } else if ((screen == Screen::WifiEdit || screen == Screen::SshEdit || screen == Screen::ConfigEdit) &&
               headerButtonContains(BtnSave, x, y)) {
        saveEditingProfile();
    } else if ((screen == Screen::WifiEdit || screen == Screen::SshEdit) && headerButtonContains(BtnDelete, x, y)) {
        deleteEditingProfile();
    } else {
        return false;
    }
    return true;
}

void executeFocusedHeaderButton()
{
    clampFocusedHeaderButton();
    const Rect* r = headerButtonAt(focusedHeaderButton);
    if (!r) {
        return;
    }
    handleHeaderTouch(r->x + r->w / 2, HeaderH / 2);
    keyboardMenuMode = false;
    clampFocusedHeaderButton();
    dirty = true;
}

bool isLeftKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && action.text == "\x1B[D";
}

bool isRightKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && action.text == "\x1B[C";
}

bool isUpKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && action.text == "\x1B[A";
}

bool isDownKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && action.text == "\x1B[B";
}

bool isEnterKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && (action.text == "\r" || action.text == "\n");
}

bool isTabKey(const KeyAction& action)
{
    return action.type == KeyActionType::Text && action.text == "\t";
}

bool handleKeyboardMenuAction(const KeyAction& action)
{
    if (action.type == KeyActionType::Menu) {
        if (!keyboardMenuMode) {
            focusCurrentScreenButton();
            keyboardMenuMode = true;
        } else {
            keyboardMenuMode = false;
        }
        dirty = true;
        return true;
    }

    if (!keyboardMenuMode) {
        return false;
    }

    const size_t count = headerButtonCount();
    if (!count) {
        return true;
    }
    if (isLeftKey(action) || isUpKey(action)) {
        focusedHeaderButton = focusedHeaderButton == 0 ? count - 1 : focusedHeaderButton - 1;
        dirty = true;
        return true;
    }
    if (isRightKey(action) || isDownKey(action) || isTabKey(action)) {
        focusedHeaderButton = (focusedHeaderButton + 1) % count;
        dirty = true;
        return true;
    }
    if (action.type == KeyActionType::Scroll) {
        focusedHeaderButton = action.value > 0 ? (focusedHeaderButton == 0 ? count - 1 : focusedHeaderButton - 1)
                                               : (focusedHeaderButton + 1) % count;
        dirty = true;
        return true;
    }
    if (isEnterKey(action)) {
        executeFocusedHeaderButton();
        return true;
    }
    return true;
}

void handleListTouch(int x, int y)
{
    if (screen == Screen::WifiList && y < settingListTop()) {
        if (BodyBtn1.contains(x, y)) {
            focusedContentItem = 0;
            scanWifiNetworks();
        } else if (BodyBtn2.contains(x, y)) {
            focusedContentItem = 1;
            addProfile();
        } else if (BodyBtn3.contains(x, y)) {
            focusedContentItem = 2;
            saveConfig();
            startWifiReconnect(20000);
        } else if (BodyBtn4.contains(x, y)) {
            focusedContentItem = 3;
            if (wifiDisabled) {
                enableWifiRuntime(20000);
            } else {
                stopWifiRuntime();
            }
            dirty = true;
        }
        return;
    }

    if (screen == Screen::WifiScan && y < settingListTop()) {
        if (BodyBtn1.contains(x, y)) {
            focusedContentItem = 0;
            scanWifiNetworks();
        } else if (BodyBtn2.contains(x, y)) {
            focusedContentItem = 1;
            screen = Screen::WifiList;
            settingScrollOffset = 0;
            dirty = true;
        }
        return;
    }

    if (screen == Screen::SshList && y < settingListTop()) {
        if (BodyBtn1.contains(x, y)) {
            focusedContentItem = 0;
            addProfile();
        } else if (BodyBtn2.contains(x, y)) {
            focusedContentItem = 1;
            if (activeSsh < config.ssh.size()) {
                beginSshEdit(activeSsh, false);
            }
        } else if (BodyBtn3.contains(x, y)) {
            focusedContentItem = 2;
            connectActiveSsh();
        }
        return;
    }

    if (screen == Screen::FontList) {
        if (y < settingListTop()) {
            if (FontMinusBtn.contains(x, y)) {
                focusedContentItem = 0;
                adjustTerminalLineStep(-1);
            } else if (FontPlusBtn.contains(x, y)) {
                focusedContentItem = 1;
                adjustTerminalLineStep(1);
            } else if (FontSaveBtn.contains(x, y)) {
                focusedContentItem = 2;
                configureTerminal();
                saveConfig();
            }
            return;
        }
        constexpr size_t fontCount = sizeof(TerminalFonts) / sizeof(TerminalFonts[0]);
        const int rowH = settingRowH();
        size_t index = static_cast<size_t>((y - settingListTop()) / rowH);
        if (index < fontCount) {
            focusedContentItem = index + 3;
            config.keyboard.terminalFont = TerminalFonts[index].id;
            config.keyboard.terminalLineStep =
                max<uint8_t>(TerminalFonts[index].defaultLineStep, max(terminalFontHeight(false), terminalFontHeight(true)));
            configureTerminal();
            saveConfig();
            dirty = true;
        }
        return;
    }

    if (y < settingListTop()) {
        return;
    }
    size_t index = settingScrollOffset + static_cast<size_t>((y - settingListTop()) / settingRowH());
    if (screen == Screen::WifiList && index < config.wifi.size()) {
        if (x > M5.Display.width() - 220) {
            activeWifi = index;
            appendStatus(String("Active Wi-Fi: ") + config.wifi[activeWifi].name);
            saveConfig();
        } else {
            focusedContentItem = index + 4;
            activeWifi = index;
            appendStatus(String("Active Wi-Fi: ") + config.wifi[activeWifi].name);
            saveConfig();
            dirty = true;
        }
    } else if (screen == Screen::WifiScan && index < scannedNetworks.size()) {
        focusedContentItem = index + 2;
        selectScannedWifi(index);
    } else if (screen == Screen::SshList && index < config.ssh.size()) {
        if (x > M5.Display.width() - 220) {
            activeSsh = index;
            appendStatus(String("Active SSH: ") + config.ssh[activeSsh].name);
            saveConfig();
        } else {
            focusedContentItem = index + 3;
            activeSsh = index;
            appendStatus(String("Active SSH: ") + config.ssh[activeSsh].name);
            saveConfig();
            dirty = true;
        }
    }
}

void editFocusedSshProfile()
{
    if (focusedContentItem >= 3) {
        size_t index = focusedContentItem - 3;
        if (index < config.ssh.size()) {
            beginSshEdit(index, false);
            return;
        }
    }
    if (activeSsh < config.ssh.size()) {
        beginSshEdit(activeSsh, false);
    }
}

void selectFocusedWifiProfile()
{
    if (focusedContentItem < 4) {
        return;
    }
    size_t index = focusedContentItem - 4;
    if (index < config.wifi.size()) {
        activeWifi = index;
        appendStatus(String("Active Wi-Fi: ") + config.wifi[activeWifi].name);
        saveConfig();
        dirty = true;
    }
}

void selectFocusedSshProfile()
{
    if (focusedContentItem < 3) {
        return;
    }
    size_t index = focusedContentItem - 3;
    if (index < config.ssh.size()) {
        activeSsh = index;
        appendStatus(String("Active SSH: ") + config.ssh[activeSsh].name);
        saveConfig();
        dirty = true;
    }
}

void handleEditTouch(int, int y)
{
    if (y < settingListTop()) {
        return;
    }
    uint8_t field = static_cast<uint8_t>((y - settingListTop()) / settingRowH());
    uint8_t maxField = editFieldCount();
    if (field < maxField) {
        editField = field;
        setEditCursorToEnd();
        if (isChoiceEditField()) {
            toggleChoiceEditField();
        }
        dirty = true;
    }
}

void handleTouch()
{
    auto touch = M5.Touch.getDetail();
    if (screen == Screen::Terminal) {
        if (touch.wasPressed()) {
            touchScrollRemainderY = 0;
            touchScrollActive = false;
        }
        if (touch.isFlicking() || touch.isDragging()) {
            touchScrollRemainderY += touch.deltaY();
            const int step = max<int>(1, terminalLineStep());
            int lines = touchScrollRemainderY / step;
            if (lines) {
                if (ssh.connected()) {
                    vt.scrollback(lines);
                } else {
                    terminal.scroll(lines);
                }
                touchScrollRemainderY -= lines * step;
                touchScrollActive = true;
                dirty = true;
                return;
            }
            if (touchScrollActive) {
                return;
            }
        }
        if (touch.wasReleased()) {
            bool consumed = touchScrollActive;
            touchScrollRemainderY = 0;
            touchScrollActive = false;
            if (consumed) {
                return;
            }
        }
        if (touch.wasFlicked() || touch.wasDragged()) {
            touchScrollRemainderY = 0;
            if (touchScrollActive) {
                touchScrollActive = false;
                return;
            }
        }
    } else {
        touchScrollRemainderY = 0;
        touchScrollActive = false;
    }

    if (screen == Screen::Terminal && touch.wasFlicked() && !touchScrollActive) {
        const int dy = touch.distanceY();
        if (abs(dy) > 30) {
            int lines = max<int>(3, abs(dy) / max<int>(1, terminalLineStep()));
            if (ssh.connected()) {
                vt.scrollback(dy > 0 ? lines : -lines);
            } else {
                terminal.scroll(dy > 0 ? lines : -lines);
            }
            dirty = true;
            return;
        }
    }

    if (!touch.wasClicked()) {
        return;
    }

    int x = touch.x;
    int y = touch.y;
    const int activeHeaderTouchH = screen == Screen::Terminal ? HeaderTouchH : HeaderH + 20;
    if (y < activeHeaderTouchH && handleHeaderTouch(x, y)) {
        return;
    } else if (screen == Screen::WifiList || screen == Screen::WifiScan || screen == Screen::SshList ||
               screen == Screen::FontList) {
        handleListTouch(x, y);
    } else if (screen == Screen::WifiEdit || screen == Screen::SshEdit || screen == Screen::ConfigEdit) {
        handleEditTouch(x, y);
    }
}

void editAppendChar(char c)
{
    if (screen != Screen::WifiEdit && screen != Screen::SshEdit && screen != Screen::ConfigEdit) {
        return;
    }

    const uint8_t maxField = editFieldCount();
    if (c == '\r' || c == '\n') {
        if (isChoiceEditField()) {
            toggleChoiceEditField();
            return;
        }
        saveEditingProfile();
        return;
    }
    if (c == '\t') {
        editField = (editField + 1) % maxField;
        setEditCursorToEnd();
        dirty = true;
        return;
    }
    if (isChoiceEditField()) {
        if (c == ' ' || c == '+' || c == '-') {
            toggleChoiceEditField();
        }
        return;
    }

    String value = currentEditFieldValue();
    clampEditCursor();
    if (c == 0x08 || c == 0x7F) {
        if (editCursor > 0) {
            size_t pos = editCursor - 1;
            while (pos > 0 && (static_cast<uint8_t>(value[pos]) & 0xC0) == 0x80) {
                --pos;
            }
            value = value.substring(0, pos) + value.substring(editCursor);
            editCursor = pos;
        }
    } else if (std::isprint(static_cast<unsigned char>(c))) {
        bool numeric = (screen == Screen::SshEdit && editField == 2) || (screen == Screen::ConfigEdit && editField == 2);
        bool allowed = !numeric || std::isdigit(static_cast<unsigned char>(c)) ||
                       (screen == Screen::ConfigEdit && editField == 2 && c == '-' && editCursor == 0);
        if (allowed) {
            value = value.substring(0, editCursor) + String(c) + value.substring(editCursor);
            ++editCursor;
        }
    }

    setCurrentEditFieldValue(value);
    dirty = true;
}

void moveEditField(int delta)
{
    if (screen != Screen::WifiEdit && screen != Screen::SshEdit && screen != Screen::ConfigEdit) {
        return;
    }
    const uint8_t maxField = editFieldCount();
    int next = static_cast<int>(editField) + delta;
    if (next < 0) {
        next = maxField - 1;
    } else if (next >= maxField) {
        next = 0;
    }
    editField = static_cast<uint8_t>(next);
    setEditCursorToEnd();
    dirty = true;
}

size_t wifiContentCount()
{
    if (screen == Screen::WifiList) {
        return 4 + config.wifi.size();
    }
    if (screen == Screen::WifiScan) {
        return 2 + scannedNetworks.size();
    }
    if (screen == Screen::SshList) {
        return 3 + config.ssh.size();
    }
    if (screen == Screen::FontList) {
        return 3 + (sizeof(TerminalFonts) / sizeof(TerminalFonts[0]));
    }
    return 0;
}

void clampWifiContentFocus()
{
    size_t count = wifiContentCount();
    if (!count) {
        focusedContentItem = 0;
        return;
    }
    if (focusedContentItem >= count) {
        focusedContentItem = count - 1;
    }
}

void ensureWifiFocusedRowVisible()
{
    size_t firstRowFocus = screen == Screen::WifiList ? 4 : (screen == Screen::WifiScan ? 2 : 3);
    if (focusedContentItem < firstRowFocus) {
        return;
    }
    size_t rowIndex = focusedContentItem - firstRowFocus;
    size_t visible = visibleSettingRows();
    if (rowIndex < settingScrollOffset) {
        settingScrollOffset = rowIndex;
    } else if (rowIndex >= settingScrollOffset + visible) {
        settingScrollOffset = rowIndex - visible + 1;
    }
}

void moveWifiContentFocus(int delta)
{
    size_t count = wifiContentCount();
    if (!count) {
        return;
    }
    int next = static_cast<int>(focusedContentItem) + delta;
    if (next < 0) {
        next = count - 1;
    } else if (next >= static_cast<int>(count)) {
        next = 0;
    }
    focusedContentItem = static_cast<size_t>(next);
    ensureWifiFocusedRowVisible();
    dirty = true;
}

void executeWifiContentFocus()
{
    clampWifiContentFocus();
    if (screen == Screen::WifiList) {
        if (focusedContentItem == 0) {
            scanWifiNetworks();
        } else if (focusedContentItem == 1) {
            addProfile();
        } else if (focusedContentItem == 2) {
            saveConfig();
            startWifiReconnect(20000);
        } else if (focusedContentItem == 3) {
            if (wifiDisabled) {
                enableWifiRuntime(20000);
            } else {
                stopWifiRuntime();
            }
            dirty = true;
        } else {
            selectFocusedWifiProfile();
        }
    } else if (screen == Screen::WifiScan) {
        if (focusedContentItem == 0) {
            scanWifiNetworks();
        } else if (focusedContentItem == 1) {
            screen = Screen::WifiList;
            focusedContentItem = 0;
            settingScrollOffset = 0;
            dirty = true;
        } else {
            size_t index = focusedContentItem - 2;
            if (index < scannedNetworks.size()) {
                selectScannedWifi(index);
            }
        }
    } else if (screen == Screen::SshList) {
        if (focusedContentItem == 0) {
            addProfile();
        } else if (focusedContentItem == 1) {
            editFocusedSshProfile();
        } else if (focusedContentItem == 2) {
            connectActiveSsh();
        } else {
            selectFocusedSshProfile();
        }
    } else if (screen == Screen::FontList) {
        if (focusedContentItem == 0) {
            adjustTerminalLineStep(-1);
        } else if (focusedContentItem == 1) {
            adjustTerminalLineStep(1);
        } else if (focusedContentItem == 2) {
            configureTerminal();
            saveConfig();
        } else {
            constexpr size_t fontCount = sizeof(TerminalFonts) / sizeof(TerminalFonts[0]);
            size_t index = focusedContentItem - 3;
            if (index < fontCount) {
                config.keyboard.terminalFont = TerminalFonts[index].id;
                config.keyboard.terminalLineStep =
                    max<uint8_t>(TerminalFonts[index].defaultLineStep, max(terminalFontHeight(false), terminalFontHeight(true)));
                configureTerminal();
                saveConfig();
                dirty = true;
            }
        }
    }
}

bool handleWifiContentAction(const KeyAction& action)
{
    if (keyboardMenuMode) {
        return false;
    }
    if (screen != Screen::WifiList && screen != Screen::WifiScan && screen != Screen::SshList &&
        screen != Screen::FontList) {
        return false;
    }
    clampWifiContentFocus();
    if (isTabKey(action) || isRightKey(action) || isDownKey(action)) {
        moveWifiContentFocus(1);
        return true;
    }
    if (isLeftKey(action) || isUpKey(action)) {
        moveWifiContentFocus(-1);
        return true;
    }
    if (isEnterKey(action)) {
        executeWifiContentFocus();
        return true;
    }
    if (action.type == KeyActionType::Scroll) {
        moveWifiContentFocus(action.value > 0 ? -1 : 1);
        return true;
    }
    return false;
}

void handleDisconnectedTerminalText(const KeyAction& action)
{
    if (isEnterKey(action)) {
        executeLocalCommand();
        return;
    }
    if (isLeftKey(action)) {
        moveCommandCursor(-1);
        return;
    }
    if (isRightKey(action)) {
        moveCommandCursor(1);
        return;
    }
    if (isUpKey(action)) {
        browseCommandHistory(-1);
        return;
    }
    if (isDownKey(action)) {
        browseCommandHistory(1);
        return;
    }
    if (isTabKey(action)) {
        if (completeSdPathAtCursor()) {
            return;
        }
        if (completeLocalCommandAtCursor()) {
            return;
        }
        return;
    }

    for (size_t i = 0; i < action.text.length(); ++i) {
        char c = action.text[i];
        if (c == 0x08 || c == 0x7F) {
            backspaceCommandText();
        } else if (std::isprint(static_cast<unsigned char>(c))) {
            insertCommandText(String(c));
        }
    }
}

void handleTerminalAction(const KeyAction& action)
{
    switch (action.type) {
        case KeyActionType::Text:
            if (ssh.connected()) {
                if (isUpKey(action) && browseRemoteCommandHistory(-1)) {
                    dirty = true;
                    break;
                }
                if (isDownKey(action) && browseRemoteCommandHistory(1)) {
                    dirty = true;
                    break;
                }
                vt.scrollbackToBottom();
                sendSshText(action.text);
                trackRemoteCommandText(action.text);
                cursorVisible = true;
                lastCursorBlink = millis();
                vt.markCursorDirty();
            } else {
                handleDisconnectedTerminalText(action);
            }
            dirty = true;
            break;
        case KeyActionType::Scroll:
            if (ssh.connected()) {
                vt.scrollback(action.value);
            } else {
                terminal.scroll(action.value);
            }
            dirty = true;
            break;
        case KeyActionType::ConnectNext:
            if (!config.ssh.empty()) {
                activeSsh = (activeSsh + 1) % config.ssh.size();
                connectActiveSsh();
            }
            break;
        case KeyActionType::ConnectPrevious:
            if (!config.ssh.empty()) {
                activeSsh = activeSsh == 0 ? config.ssh.size() - 1 : activeSsh - 1;
                connectActiveSsh();
            }
            break;
        case KeyActionType::Menu:
            screen = Screen::WifiList;
            dirty = true;
            break;
        default:
            break;
    }
}

void handleAction(const KeyAction& action)
{
    if (imageOverlayActive) {
        if (action.type == KeyActionType::Menu ||
            (action.type == KeyActionType::Text && action.text == String(static_cast<char>(0x1B)))) {
            imageOverlayActive = false;
            imageOverlayDrawn = false;
            if (ssh.connected()) {
                vt.markAllDirty();
            }
            dirty = true;
        }
        return;
    }

    if (action.type == KeyActionType::Menu && screen == Screen::Terminal && ssh.connected()) {
        sendSshText(String(static_cast<char>(0x1B)));
        dirty = true;
        return;
    }

    if (action.type == KeyActionType::Menu) {
        handleKeyboardMenuAction(action);
        return;
    }

    if (keyboardMenuMode) {
        handleKeyboardMenuAction(action);
        return;
    }

    if (screen == Screen::FontList && action.type == KeyActionType::Text) {
        if (action.text == "-") {
            adjustTerminalLineStep(-1);
            return;
        }
        if (action.text == "+") {
            adjustTerminalLineStep(1);
            return;
        }
    }

    if ((screen == Screen::WifiList || screen == Screen::WifiScan || screen == Screen::SshList ||
         screen == Screen::FontList) &&
        action.type != KeyActionType::Menu) {
        if (handleWifiContentAction(action)) {
            return;
        }
    }

    if (handleWifiContentAction(action)) {
        return;
    }

    if (screen == Screen::WifiEdit || screen == Screen::SshEdit || screen == Screen::ConfigEdit) {
        if (isTabKey(action) || isDownKey(action)) {
            moveEditField(1);
        } else if (isUpKey(action)) {
            moveEditField(-1);
        } else if (isLeftKey(action)) {
            if (isChoiceEditField()) toggleChoiceEditField();
            else moveEditCursor(-1);
        } else if (isRightKey(action)) {
            if (isChoiceEditField()) toggleChoiceEditField();
            else moveEditCursor(1);
        } else if (action.type == KeyActionType::Text) {
            for (size_t i = 0; i < action.text.length(); ++i) {
                editAppendChar(action.text[i]);
            }
        } else if (action.type == KeyActionType::Menu) {
            if (screen == Screen::WifiEdit) screen = Screen::WifiList;
            else if (screen == Screen::SshEdit) screen = Screen::SshList;
            else screen = Screen::Terminal;
            dirty = true;
        }
        return;
    }

    if (screen == Screen::Terminal) {
        handleTerminalAction(action);
    } else if (action.type == KeyActionType::Text && (isUpKey(action) || isLeftKey(action))) {
        if (screen == Screen::WifiList) {
            scrollSettingList(-1, config.wifi.size());
        } else if (screen == Screen::WifiScan) {
            scrollSettingList(-1, scannedNetworks.size());
        } else if (screen == Screen::SshList) {
            scrollSettingList(-1, config.ssh.size());
        }
    } else if (action.type == KeyActionType::Text && (isDownKey(action) || isRightKey(action) || isTabKey(action))) {
        if (screen == Screen::WifiList) {
            scrollSettingList(1, config.wifi.size());
        } else if (screen == Screen::WifiScan) {
            scrollSettingList(1, scannedNetworks.size());
        } else if (screen == Screen::SshList) {
            scrollSettingList(1, config.ssh.size());
        }
    } else if (action.type == KeyActionType::Scroll) {
        if (screen == Screen::WifiList) {
            scrollSettingList(action.value, config.wifi.size());
        } else if (screen == Screen::WifiScan) {
            scrollSettingList(action.value, scannedNetworks.size());
        } else if (screen == Screen::SshList) {
            scrollSettingList(action.value, config.ssh.size());
        }
    } else if (action.type == KeyActionType::Menu) {
        screen = screen == Screen::WifiScan ? Screen::WifiList : Screen::Terminal;
        settingScrollOffset = 0;
        dirty = true;
    }
}

void pollSsh()
{
    if (!ssh.connected()) {
        return;
    }
    char buffer[512];
    bool received = false;
    uint32_t start = millis();
    for (uint8_t reads = 0; reads < 8 && millis() - start < 8; ++reads) {
        setCrashStage("ssh.read");
        int n = ssh.read(buffer, sizeof(buffer));
        if (n > 0) {
            setCrashStage("ssh.append");
            vt.write(buffer, static_cast<size_t>(n));
            while (vt.hasOscMessage()) {
                handleSshOscMessage(vt.popOscMessage());
            }
            received = true;
            lastSshReceive = millis();
            continue;
        }
        if (n < 0) {
            setCrashStage("ssh.read.error");
            resetStorageBridgeState();
            ssh.disconnect();
            resetCommandEditor();
            configureTerminal();
            appendStatus("SSH disconnected by remote");
            setCrashStage("loop");
            return;
        }
        break;
    }
    if (received) {
        dirty = true;
    }
    setCrashStage("loop");
}

void serialPrintHelp()
{
    Serial.println("Tab5 SSH serial API");
    Serial.println("  help");
    Serial.println("  status");
    Serial.println("  crash");
    Serial.println("  sd status");
    Serial.println("  sd df");
    Serial.println("  sd ls [-lah] [path]");
    Serial.println("  sd cat <path>");
    Serial.println("  sd mkdir <path>");
    Serial.println("  sd rmdir <path>");
    Serial.println("  sd write <path> <text>");
    Serial.println("  sd append <path> <text>");
    Serial.println("  sd chmod <mode> <path>");
    Serial.println("  usb power|status|rescan");
    Serial.println("  fs volumes");
    Serial.println("  fs stat <sd:/path|usb:/path>");
    Serial.println("  fs list <sd:/path|usb:/path>");
    Serial.println("  fs read <sd:/path> <offset> <size>");
    Serial.println("  fs write <sd:/path> <offset> <hex>");
    Serial.println("  fs mkdir|rmdir|rm <sd:/path>");
    Serial.println("  wifi status");
    Serial.println("  wifi off");
    Serial.println("  wifi on");
    Serial.println("  ssh list");
    Serial.println("  ssh active <index>");
    Serial.println("  ssh connect [index]");
    Serial.println("  ssh send <text>");
    Serial.println("  ssh raw <hex bytes>");
    Serial.println("  ssh disconnect");
    Serial.println("  scp get <remote> <sd-local> [profile-index]");
    Serial.println("  scp put <sd-local> <remote> [profile-index]");
    Serial.println("  scp get user@host:/remote <sd-local> [password]");
    Serial.println("  scp put <sd-local> user@host:/remote [password]");
    Serial.println("  image <sd:/path|usb:/path> [fit|center|half|quarter]");
    Serial.println("  ble status|devices|paired|enable|disable|scan|scanraw|list|disconnect [index|all]|type <own> <peer>|auth <none|bond|scbond>|force <on|off>|params <si> <sw> <min> <max> <lat> <to>|gaptest [index]|gapstatus|gapscan|gapauto|pair [index]|scanpair [index]|forget [index|all]");
    Serial.println("  python -c <statement>");
    Serial.println("  python <sd.py> [args...]");
    Serial.println("  python --reset");
    Serial.println("  term dump");
}

void serialPrintStatus()
{
    Serial.printf("screen=%u wifi=%s wl=%d ssh=%s activeWifi=%u activeSsh=%u keymap=%s sd=%s keyboard=%s ble=%s stage=%s\r\n",
                  static_cast<unsigned>(screen),
                  wifiStatusText.c_str(),
                  static_cast<int>(WiFi.status()),
                  ssh.connected() ? "connected" : "disconnected",
                  static_cast<unsigned>(activeWifi),
                  static_cast<unsigned>(activeSsh),
                  config.keyboard.layout.c_str(),
                  sdReady ? "ready" : sdLastError.c_str(),
                  keyboard.status().c_str(),
                  keyboard.bleStatus().c_str(),
                  crashStage);
}

void serialPrintSdStatus()
{
    if (!ensureSdReady()) {
        Serial.printf("sd=not ready error=%s\r\n", sdLastError.c_str());
        return;
    }
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t avail = total > used ? total - used : 0;
    Serial.printf("sd=ready type=%u card=%llu size=%llu used=%llu avail=%llu cwd=%s\r\n",
                  static_cast<unsigned>(SD.cardType()),
                  static_cast<unsigned long long>(SD.cardSize()),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(used),
                  static_cast<unsigned long long>(avail),
                  sdCwd.c_str());
}

void serialPrintSdDf()
{
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t avail = total > used ? total - used : 0;
    uint32_t usePct = total ? static_cast<uint32_t>((used * 100ULL + total - 1) / total) : 0;
    Serial.println("Filesystem      Size  Used Avail Use% Mounted on");
    Serial.printf("microSD         %s  %s  %s  %u%% /sd\r\n",
                  formatBytes(total).c_str(),
                  formatBytes(used).c_str(),
                  formatBytes(avail).c_str(),
                  static_cast<unsigned>(usePct));
}

void serialPrintSdList(const String& inputPath)
{
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    LsOptions options;
    String error;
    if (!parseLsOptions(inputPath, options, error)) {
        Serial.println(error);
        return;
    }
    String path = normalizeSdPath(options.path);
    File root = SD.open(path, FILE_READ);
    if (!root) {
        Serial.printf("ERR cannot open %s\r\n", path.c_str());
        return;
    }
    if (root.isDirectory() && !sdPathHasExecutePermission(path)) {
        Serial.printf("ERR permission denied %s\r\n", path.c_str());
        root.close();
        return;
    }
    if (!root.isDirectory()) {
        Serial.println(lsDisplayLine(root, basenameOnly(path), path, options));
        root.close();
        return;
    }
    if (!options.longFormat) {
        std::vector<String> names = collectLsNames(root, path, options);
        serialPrintLsColumns(names, 80);
        root.close();
        return;
    }
    if (options.longFormat) {
        Serial.printf("total %s  %s\r\n", formatBytes(root.size()).c_str(), path.c_str());
    }
    File file = root.openNextFile();
    while (file) {
        String displayName = basenameOnly(file.name());
        if (!options.all && displayName.startsWith(".")) {
            file.close();
            file = root.openNextFile();
            continue;
        }
        Serial.println(lsDisplayLine(file, displayName, joinSdPath(path, displayName), options));
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

void serialPrintSdCat(const String& inputPath)
{
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    String path = normalizeSdPath(inputPath);
    if (!sdPathHasReadPermission(path)) {
        Serial.printf("ERR permission denied %s\r\n", path.c_str());
        return;
    }
    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        Serial.printf("ERR cannot open %s\r\n", path.c_str());
        return;
    }
    Serial.printf("BEGIN %s %u\r\n", path.c_str(), static_cast<unsigned>(file.size()));
    size_t sent = 0;
    while (file.available() && sent < 4096) {
        Serial.write(file.read());
        ++sent;
    }
    file.close();
    Serial.println();
    Serial.println(sent >= 4096 ? "END truncated" : "END");
}

void serialWriteSdText(const String& command, bool append)
{
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    size_t prefix = append ? strlen("sd append ") : strlen("sd write ");
    String rest = command.substring(prefix);
    rest.trim();
    int split = rest.indexOf(' ');
    if (split <= 0) {
        Serial.println("ERR usage");
        return;
    }
    String path = normalizeSdPath(rest.substring(0, split));
    String text = rest.substring(split + 1);
    if (SD.exists(path) && !sdPathHasWritePermission(path)) {
        Serial.printf("ERR permission denied %s\r\n", path.c_str());
        return;
    }
    if (!append && SD.exists(path)) {
        SD.remove(path);
    }
    File file = SD.open(path, FILE_APPEND);
    if (!file) {
        Serial.printf("ERR cannot open %s\r\n", path.c_str());
        return;
    }
    file.print(text);
    file.print("\n");
    file.close();
    Serial.printf("OK %s %s\r\n", append ? "appended" : "wrote", path.c_str());
}

void serialMakeSdDirectory(const String& inputPath)
{
    String message;
    bool ok = makeSdDirectory(inputPath, message);
    Serial.printf("%s %s\r\n", ok ? "OK" : "ERR", message.c_str());
}

void serialRemoveSdDirectory(const String& inputPath)
{
    String message;
    bool ok = removeSdDirectory(inputPath, message);
    Serial.printf("%s %s\r\n", ok ? "OK" : "ERR", message.c_str());
}

void serialChmodSd(const String& command)
{
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    String rest = command.substring(strlen("sd chmod "));
    rest.trim();
    int split = rest.indexOf(' ');
    if (split <= 0) {
        Serial.println("ERR usage");
        return;
    }
    uint16_t mode = 0;
    String modeText = rest.substring(0, split);
    if (!parseOctalMode(modeText, mode)) {
        Serial.println("ERR invalid mode");
        return;
    }
    String path = normalizeSdPath(rest.substring(split + 1));
    if (!SD.exists(path)) {
        Serial.printf("ERR cannot access %s\r\n", path.c_str());
        return;
    }
    setSdModeForPath(path, mode);
    Serial.printf("OK mode %s %s\r\n", modeText.c_str(), path.c_str());
}

String normalizeFsPath(const String& spec, String& volume)
{
    String text = spec;
    text.trim();
    int sep = text.indexOf(':');
    if (sep >= 0) {
        volume = text.substring(0, sep);
        volume.toLowerCase();
        return normalizeSdPath(text.substring(sep + 1));
    }
    volume = "sd";
    return normalizeSdPath(text);
}

bool ensureFsVolume(const String& volume)
{
    if (volume == "sd" || volume == "microsd") {
        return ensureSdReady();
    }
    if (volume == "flash") {
        return true;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        return ensureUsbReady();
    }
#endif
    Serial.printf("ERR volume %s not mounted\r\n", volume.c_str());
    return false;
}

char hexDigit(uint8_t value)
{
    value &= 0x0F;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void serialFsVolumes()
{
    if (ensureSdReady()) {
        Serial.printf("VOL sd ready size=%llu used=%llu\r\n",
                      static_cast<unsigned long long>(SD.totalBytes()),
                      static_cast<unsigned long long>(SD.usedBytes()));
    } else {
        Serial.printf("VOL sd error=%s\r\n", sdLastError.c_str());
    }
#if ENABLE_USB_HOST_KEYBOARD
    ensureUsbReady(true);
    if (usbMscMounted) {
        Serial.printf("VOL usb ready %s\r\n", usbMscStatus.c_str());
    } else if (usbMscPresent) {
        Serial.printf("VOL usb present mount=/usb status=%s\r\n", usbMscStatus.c_str());
    } else {
        Serial.printf("VOL usb mount=/usb error=%s\r\n", usbMscStatus.c_str());
    }
#else
    Serial.println("VOL usb error=usb mass-storage host disabled");
#endif
    Serial.println("OK");
}

#if ENABLE_USB_HOST_KEYBOARD
void serialUsbStat(const String& path)
{
    FILINFO info{};
    FRESULT result = f_stat(usbFatPath(path).c_str(), &info);
    if (result != FR_OK) {
        Serial.printf("ERR not found usb:%s\r\n", path.c_str());
        return;
    }
    bool directory = (info.fattrib & AM_DIR) != 0;
    Serial.printf("OK type=%s size=%lu mode=%03o mtime=0 path=usb:%s\r\n",
                  directory ? "dir" : "file",
                  static_cast<unsigned long>(directory ? 0 : info.fsize),
                  directory ? 0755 : 0644,
                  path.c_str());
}

void serialUsbList(const String& path)
{
    FF_DIR dir;
    FRESULT result = f_opendir(&dir, usbFatPath(path).c_str());
    if (result != FR_OK) {
        serialUsbStat(path);
        return;
    }
    for (;;) {
        FILINFO info{};
        result = f_readdir(&dir, &info);
        if (result != FR_OK || info.fname[0] == 0) {
            break;
        }
        bool directory = (info.fattrib & AM_DIR) != 0;
        Serial.printf("ITEM %s %lu %03o 0 %s\r\n",
                      directory ? "dir" : "file",
                      static_cast<unsigned long>(directory ? 0 : info.fsize),
                      directory ? 0755 : 0644,
                      info.fname);
    }
    f_closedir(&dir);
    Serial.println("OK");
}

void serialUsbRead(const String& path, uint32_t offset, uint32_t requested)
{
    FIL file;
    FRESULT result = f_open(&file, usbFatPath(path).c_str(), FA_READ);
    if (result != FR_OK) {
        Serial.printf("ERR cannot read usb:%s\r\n", path.c_str());
        return;
    }
    f_lseek(&file, offset);
    requested = min<uint32_t>(requested, 512);
    uint32_t count = 0;
    Serial.print("DATA ");
    while (count < requested) {
        uint8_t b = 0;
        UINT readBytes = 0;
        result = f_read(&file, &b, 1, &readBytes);
        if (result != FR_OK || readBytes == 0) {
            break;
        }
        Serial.write(hexDigit(b >> 4));
        Serial.write(hexDigit(b));
        ++count;
    }
    f_close(&file);
    Serial.printf("\r\nOK size=%lu\r\n", static_cast<unsigned long>(count));
}

void serialUsbWrite(const String& path, uint32_t offset, const String& hex)
{
    FIL file;
    BYTE mode = FA_WRITE | FA_OPEN_ALWAYS;
    FRESULT result = f_open(&file, usbFatPath(path).c_str(), mode);
    if (result != FR_OK) {
        Serial.printf("ERR cannot write usb:%s\r\n", path.c_str());
        return;
    }
    if (offset == 0) {
        f_truncate(&file);
    } else {
        f_lseek(&file, offset);
    }
    uint32_t written = 0;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        int hi = hexValue(hex[i]);
        int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            f_close(&file);
            Serial.println("ERR invalid hex");
            return;
        }
        uint8_t b = static_cast<uint8_t>((hi << 4) | lo);
        UINT wrote = 0;
        result = f_write(&file, &b, 1, &wrote);
        if (result != FR_OK || wrote != 1) {
            f_close(&file);
            Serial.println("ERR usb write failed");
            return;
        }
        ++written;
    }
    f_close(&file);
    Serial.printf("OK written=%lu path=usb:%s\r\n", static_cast<unsigned long>(written), path.c_str());
}
#endif

void serialFsStat(const String& spec)
{
    String volume;
    String path = normalizeFsPath(spec, volume);
    if (!ensureFsVolume(volume)) {
        return;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        serialUsbStat(path);
        return;
    }
#endif
    fs::FS* fs = volume == "flash" ? static_cast<fs::FS*>(&LittleFS) : static_cast<fs::FS*>(&SD);
    File file = fs->open(path, FILE_READ);
    if (!file) {
        Serial.printf("ERR not found %s:%s\r\n", volume.c_str(), path.c_str());
        return;
    }
    bool directory = file.isDirectory();
    Serial.printf("OK type=%s size=%llu mode=%03o mtime=%lu path=%s:%s\r\n",
                  directory ? "dir" : "file",
                  static_cast<unsigned long long>(directory ? 0 : file.size()),
                  static_cast<unsigned>(volume == "flash" ? (directory ? 0755 : 0644) : sdModeForPath(path, directory)),
                  static_cast<unsigned long>(file.getLastWrite()),
                  volume.c_str(),
                  path.c_str());
    file.close();
}

void serialFsList(const String& spec)
{
    String volume;
    String path = normalizeFsPath(spec, volume);
    if (!ensureFsVolume(volume)) {
        return;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        serialUsbList(path);
        return;
    }
#endif
    fs::FS* fs = volume == "flash" ? static_cast<fs::FS*>(&LittleFS) : static_cast<fs::FS*>(&SD);
    File root = fs->open(path, FILE_READ);
    if (!root) {
        Serial.printf("ERR not found %s:%s\r\n", volume.c_str(), path.c_str());
        return;
    }
    if (!root.isDirectory()) {
        Serial.printf("ITEM file %llu %03o %lu %s\r\n",
                      static_cast<unsigned long long>(root.size()),
                      static_cast<unsigned>(volume == "flash" ? 0644 : sdModeForPath(path, false)),
                      static_cast<unsigned long>(root.getLastWrite()),
                      basenameOnly(path).c_str());
        Serial.println("OK");
        root.close();
        return;
    }
    File file = root.openNextFile();
    while (file) {
        String name = basenameOnly(file.name());
        String childPath = joinSdPath(path, name);
        bool directory = file.isDirectory();
        Serial.printf("ITEM %s %llu %03o %lu %s\r\n",
                      directory ? "dir" : "file",
                      static_cast<unsigned long long>(directory ? 0 : file.size()),
                      static_cast<unsigned>(volume == "flash" ? (directory ? 0755 : 0644) : sdModeForPath(childPath, directory)),
                      static_cast<unsigned long>(file.getLastWrite()),
                      name.c_str());
        file.close();
        file = root.openNextFile();
    }
    root.close();
    Serial.println("OK");
}

void serialFsRead(const String& args)
{
    String rest = args;
    rest.trim();
    int first = rest.indexOf(' ');
    int second = first < 0 ? -1 : rest.indexOf(' ', first + 1);
    if (first <= 0 || second <= first) {
        Serial.println("ERR usage fs read <volume:path> <offset> <size>");
        return;
    }
    String volume;
    String path = normalizeFsPath(rest.substring(0, first), volume);
    uint32_t offset = static_cast<uint32_t>(strtoul(rest.substring(first + 1, second).c_str(), nullptr, 10));
    uint32_t requested = static_cast<uint32_t>(strtoul(rest.substring(second + 1).c_str(), nullptr, 10));
    requested = min<uint32_t>(requested, 512);
    if (!ensureFsVolume(volume)) {
        return;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        serialUsbRead(path, offset, requested);
        return;
    }
#endif
    fs::FS* fs = volume == "flash" ? static_cast<fs::FS*>(&LittleFS) : static_cast<fs::FS*>(&SD);
    File file = fs->open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        Serial.printf("ERR cannot read %s:%s\r\n", volume.c_str(), path.c_str());
        return;
    }
    file.seek(offset);
    uint32_t count = 0;
    Serial.print("DATA ");
    while (file.available() && count < requested) {
        uint8_t b = static_cast<uint8_t>(file.read());
        Serial.write(hexDigit(b >> 4));
        Serial.write(hexDigit(b));
        ++count;
    }
    file.close();
    Serial.printf("\r\nOK size=%lu\r\n", static_cast<unsigned long>(count));
}

void serialFsWrite(const String& args)
{
    String rest = args;
    rest.trim();
    int first = rest.indexOf(' ');
    int second = first < 0 ? -1 : rest.indexOf(' ', first + 1);
    if (first <= 0) {
        Serial.println("ERR usage fs write <volume:path> <offset> <hex>");
        return;
    }
    String volume;
    String path = normalizeFsPath(rest.substring(0, first), volume);
    String offsetText = second > first ? rest.substring(first + 1, second) : rest.substring(first + 1);
    uint32_t offset = static_cast<uint32_t>(strtoul(offsetText.c_str(), nullptr, 10));
    String hex = second > first ? rest.substring(second + 1) : "";
    hex.trim();
    if (!ensureFsVolume(volume)) {
        return;
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        serialUsbWrite(path, offset, hex);
        return;
    }
#endif
    fs::FS* fs = volume == "flash" ? static_cast<fs::FS*>(&LittleFS) : static_cast<fs::FS*>(&SD);
    if (offset == 0 && fs->exists(path)) {
        fs->remove(path);
    }
    File file = fs->open(path, FILE_APPEND);
    if (!file) {
        Serial.printf("ERR cannot write %s:%s\r\n", volume.c_str(), path.c_str());
        return;
    }
    if (offset > 0) {
        file.seek(offset);
    }
    uint32_t written = 0;
    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        int hi = hexValue(hex[i]);
        int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            file.close();
            Serial.println("ERR invalid hex");
            return;
        }
        file.write(static_cast<uint8_t>((hi << 4) | lo));
        ++written;
    }
    file.close();
    Serial.printf("OK written=%lu path=%s:%s\r\n", static_cast<unsigned long>(written), volume.c_str(), path.c_str());
}

void serialFsCommand(const String& command)
{
    if (command == "fs volumes") {
        serialFsVolumes();
    } else if (command.startsWith("fs stat ")) {
        serialFsStat(command.substring(strlen("fs stat ")));
    } else if (command.startsWith("fs list ")) {
        serialFsList(command.substring(strlen("fs list ")));
    } else if (command.startsWith("fs read ")) {
        serialFsRead(command.substring(strlen("fs read ")));
    } else if (command.startsWith("fs write ")) {
        serialFsWrite(command.substring(strlen("fs write ")));
    } else if (command.startsWith("fs mkdir ")) {
        String volume;
        String path = normalizeFsPath(command.substring(strlen("fs mkdir ")), volume);
        if (!ensureFsVolume(volume)) return;
#if ENABLE_USB_HOST_KEYBOARD
        if (volume == "usb") {
            Serial.println(f_mkdir(usbFatPath(path).c_str()) == FR_OK ? "OK" : "ERR mkdir failed");
            return;
        }
#endif
        Serial.println(SD.mkdir(path) ? "OK" : "ERR mkdir failed");
    } else if (command.startsWith("fs rmdir ")) {
        String volume;
        String path = normalizeFsPath(command.substring(strlen("fs rmdir ")), volume);
        if (!ensureFsVolume(volume)) return;
#if ENABLE_USB_HOST_KEYBOARD
        if (volume == "usb") {
            Serial.println(removeUsbDirectoryEntry(path) ? "OK" : "ERR rmdir failed");
            return;
        }
#endif
        Serial.println(SD.rmdir(path) ? "OK" : "ERR rmdir failed");
    } else if (command.startsWith("fs rm ")) {
        String volume;
        String path = normalizeFsPath(command.substring(strlen("fs rm ")), volume);
        if (!ensureFsVolume(volume)) return;
#if ENABLE_USB_HOST_KEYBOARD
        if (volume == "usb") {
            Serial.println(f_unlink(usbFatPath(path).c_str()) == FR_OK ? "OK" : "ERR rm failed");
            return;
        }
#endif
        Serial.println(SD.remove(path) ? "OK" : "ERR rm failed");
    } else {
        Serial.println("ERR usage fs volumes|stat|list|read|write|mkdir|rmdir|rm");
    }
}

String hexEncodeText(const String& text)
{
    String out;
    out.reserve(text.length() * 2);
    for (size_t i = 0; i < text.length(); ++i) {
        uint8_t b = static_cast<uint8_t>(text[i]);
        out += hexDigit(b >> 4);
        out += hexDigit(b);
    }
    return out;
}

void appendHexBytes(String& out, const uint8_t* bytes, size_t length)
{
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        uint8_t b = bytes[i];
        out += digits[b >> 4];
        out += digits[b & 0x0F];
    }
}

bool decodeHexChunk(const String& hex, size_t& index, uint8_t* buffer, size_t capacity, size_t& length)
{
    length = 0;
    if (((hex.length() - index) & 1) != 0) {
        return false;
    }
    while (index + 1 < hex.length() && length < capacity) {
        int hi = hexValue(hex[index]);
        int lo = hexValue(hex[index + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        buffer[length++] = static_cast<uint8_t>((hi << 4) | lo);
        index += 2;
    }
    return true;
}

String hexDecodeText(const String& text)
{
    String out;
    out.reserve(text.length() / 2);
    for (size_t i = 0; i + 1 < text.length(); i += 2) {
        int hi = hexValue(text[i]);
        int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return "";
        }
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

void bridgeReply(const String& id, const String& payload)
{
    String line = "RSP\t" + id + "\t" + payload + "\n";
    if (!ssh.writeBridge(reinterpret_cast<const uint8_t*>(line.c_str()), line.length())) {
        storageBridgeRunning = false;
        storageBridgeLine = "";
    }
}

void bridgeError(const String& id, int code)
{
    bridgeReply(id, String("ERR\t") + code);
}

bool bridgeEnsureVolume(const String& volume)
{
    if (volume == "sd" || volume == "microsd") {
        return ensureSdReady();
    }
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        return ensureUsbReady();
    }
#endif
    return false;
}

void bridgeHandleRequest(const String& line)
{
    std::vector<String> parts;
    int start = 0;
    for (;;) {
        int tab = line.indexOf('\t', start);
        if (tab < 0) {
            parts.push_back(line.substring(start));
            break;
        }
        parts.push_back(line.substring(start, tab));
        start = tab + 1;
    }
    if (parts.size() < 5 || parts[0] != "REQ") {
        return;
    }
    const String& id = parts[1];
    const String& op = parts[2];
    String volume = parts[3];
    volume.toLowerCase();
    String path = normalizeSdPath(hexDecodeText(parts[4]));
#if ENABLE_USB_HOST_KEYBOARD
    if (volume == "usb") {
        if (!bridgeEnsureVolume(volume)) {
            if (path == "/" && op == "stat") {
                bridgeReply(id, "OK\tSTAT\tdir\t0\t0555\t0");
            } else if (path == "/" && op == "list") {
                bridgeReply(id, "OK\tLIST\t");
            } else {
                bridgeError(id, 5);
            }
            return;
        }
        String fatPath = usbFatPath(path);
        if (op == "stat") {
            FILINFO info{};
            FRESULT result = f_stat(fatPath.c_str(), &info);
            if (result != FR_OK) {
                bridgeError(id, 2);
                return;
            }
            bool directory = (info.fattrib & AM_DIR) != 0;
            bridgeReply(id, String("OK\tSTAT\t") + (directory ? "dir" : "file") + "\t" +
                              String(static_cast<unsigned long>(directory ? 0 : info.fsize)) + "\t" +
                              String(directory ? 0755 : 0644, 8) + "\t0");
        } else if (op == "list") {
            FF_DIR dir;
            FRESULT result = f_opendir(&dir, fatPath.c_str());
            if (result != FR_OK) {
                bridgeError(id, 2);
                return;
            }
            String payload = "OK\tLIST\t";
            bool first = true;
            for (;;) {
                FILINFO info{};
                result = f_readdir(&dir, &info);
                if (result != FR_OK || info.fname[0] == 0) {
                    break;
                }
                bool directory = (info.fattrib & AM_DIR) != 0;
                if (!first) {
                    payload += "|";
                }
                first = false;
                payload += hexEncodeText(info.fname);
                payload += ",";
                payload += directory ? "dir" : "file";
                payload += ",";
                payload += String(static_cast<unsigned long>(directory ? 0 : info.fsize));
                payload += ",";
                payload += String(directory ? 0755 : 0644, 8);
                payload += ",0";
            }
            f_closedir(&dir);
            bridgeReply(id, payload);
        } else if (op == "read") {
            if (parts.size() < 7) {
                bridgeError(id, 22);
                return;
            }
            uint32_t offset = static_cast<uint32_t>(strtoul(parts[5].c_str(), nullptr, 10));
            uint32_t requested = min<uint32_t>(static_cast<uint32_t>(strtoul(parts[6].c_str(), nullptr, 10)), 4096);
            FIL file;
            if (f_open(&file, fatPath.c_str(), FA_READ) != FR_OK) {
                bridgeError(id, 2);
                return;
            }
            f_lseek(&file, offset);
            String data;
            data.reserve(requested * 2);
            uint8_t buffer[512];
            uint32_t remaining = requested;
            while (remaining > 0) {
                UINT readBytes = 0;
                UINT chunk = static_cast<UINT>(min<uint32_t>(remaining, sizeof(buffer)));
                if (f_read(&file, buffer, chunk, &readBytes) != FR_OK || readBytes == 0) {
                    break;
                }
                appendHexBytes(data, buffer, readBytes);
                remaining -= readBytes;
            }
            f_close(&file);
            bridgeReply(id, String("OK\tDATA\t") + data);
        } else if (op == "write") {
            if (parts.size() < 7) {
                bridgeError(id, 22);
                return;
            }
            uint32_t offset = static_cast<uint32_t>(strtoul(parts[5].c_str(), nullptr, 10));
            if (offset == 0) {
                f_unlink(fatPath.c_str());
            }
            FIL file;
            if (f_open(&file, fatPath.c_str(), FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) {
                bridgeError(id, 5);
                return;
            }
            f_lseek(&file, offset);
            String hex = parts[6];
            uint8_t buffer[512];
            size_t index = 0;
            while (index < hex.length()) {
                size_t length = 0;
                if (!decodeHexChunk(hex, index, buffer, sizeof(buffer), length)) {
                    f_close(&file);
                    bridgeError(id, 22);
                    return;
                }
                UINT wrote = 0;
                if (length && (f_write(&file, buffer, length, &wrote) != FR_OK || wrote != length)) {
                    f_close(&file);
                    bridgeError(id, 5);
                    return;
                }
            }
            f_close(&file);
            bridgeReply(id, "OK\tDONE");
        } else if (op == "truncate") {
            if (parts.size() < 6) {
                bridgeError(id, 22);
                return;
            }
            size_t size = static_cast<size_t>(strtoul(parts[5].c_str(), nullptr, 10));
            if (size == 0) {
                f_unlink(fatPath.c_str());
                FIL file;
                FRESULT result = f_open(&file, fatPath.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
                f_close(&file);
                result == FR_OK ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
            } else {
                bridgeReply(id, "OK\tDONE");
            }
        } else if (op == "mkdir") {
            f_mkdir(fatPath.c_str()) == FR_OK ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
        } else if (op == "rmdir") {
            removeUsbDirectoryEntry(path) ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
        } else if (op == "unlink") {
            f_unlink(fatPath.c_str()) == FR_OK ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
        } else if (op == "rename") {
            if (parts.size() < 6) {
                bridgeError(id, 22);
                return;
            }
            String to = usbFatPath(normalizeSdPath(hexDecodeText(parts[5])));
            f_rename(fatPath.c_str(), to.c_str()) == FR_OK ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
        } else {
            bridgeError(id, 38);
        }
        return;
    }
#endif
    if (!bridgeEnsureVolume(volume)) {
        bridgeError(id, 5);
        return;
    }
    if (op == "stat") {
        File file = SD.open(path, FILE_READ);
        if (!file) {
            bridgeError(id, 2);
            return;
        }
        bool directory = file.isDirectory();
        String sizeText = String(static_cast<unsigned long>(directory ? 0 : file.size()));
        String mtimeText = String(static_cast<unsigned long>(file.getLastWrite()));
        bridgeReply(id, String("OK\tSTAT\t") + (directory ? "dir" : "file") + "\t" +
                          sizeText + "\t" +
                          String(sdModeForPath(path, directory), 8) + "\t" +
                          mtimeText);
        file.close();
    } else if (op == "list") {
        File root = SD.open(path, FILE_READ);
        if (!root) {
            bridgeError(id, 2);
            return;
        }
        if (!root.isDirectory()) {
            root.close();
            bridgeError(id, 20);
            return;
        }
        String payload = "OK\tLIST\t";
        File file = root.openNextFile();
        bool first = true;
        while (file) {
            String name = basenameOnly(file.name());
            String childPath = joinSdPath(path, name);
            bool directory = file.isDirectory();
            if (!first) {
                payload += "|";
            }
            first = false;
            payload += hexEncodeText(name);
            payload += ",";
            payload += directory ? "dir" : "file";
            payload += ",";
            payload += String(static_cast<unsigned long>(directory ? 0 : file.size()));
            payload += ",";
            payload += String(sdModeForPath(childPath, directory), 8);
            payload += ",";
            payload += String(static_cast<unsigned long>(file.getLastWrite()));
            file.close();
            file = root.openNextFile();
        }
        root.close();
        bridgeReply(id, payload);
    } else if (op == "read") {
        if (parts.size() < 7) {
            bridgeError(id, 22);
            return;
        }
        uint32_t offset = static_cast<uint32_t>(strtoul(parts[5].c_str(), nullptr, 10));
        uint32_t requested = min<uint32_t>(static_cast<uint32_t>(strtoul(parts[6].c_str(), nullptr, 10)), 4096);
        File file = SD.open(path, FILE_READ);
        if (!file || file.isDirectory()) {
            bridgeError(id, 2);
            return;
        }
        file.seek(offset);
        String data;
        data.reserve(requested * 2);
        uint8_t buffer[512];
        uint32_t remaining = requested;
        while (file.available() && remaining > 0) {
            size_t readBytes = file.read(buffer, min<uint32_t>(remaining, sizeof(buffer)));
            if (!readBytes) {
                break;
            }
            appendHexBytes(data, buffer, readBytes);
            remaining -= readBytes;
        }
        file.close();
        bridgeReply(id, String("OK\tDATA\t") + data);
    } else if (op == "write") {
        if (parts.size() < 7) {
            bridgeError(id, 22);
            return;
        }
        uint32_t offset = static_cast<uint32_t>(strtoul(parts[5].c_str(), nullptr, 10));
        String hex = parts[6];
        if (offset == 0 && SD.exists(path)) {
            SD.remove(path);
        }
        File file = SD.open(path, offset == 0 ? FILE_WRITE : "r+");
        if (!file) {
            bridgeError(id, 5);
            return;
        }
        if (offset > 0) {
            file.seek(offset);
        }
        uint8_t buffer[512];
        size_t index = 0;
        while (index < hex.length()) {
            size_t length = 0;
            if (!decodeHexChunk(hex, index, buffer, sizeof(buffer), length)) {
                file.close();
                bridgeError(id, 22);
                return;
            }
            if (length && file.write(buffer, length) != length) {
                file.close();
                bridgeError(id, 5);
                return;
            }
        }
        file.close();
        bridgeReply(id, "OK\tDONE");
    } else if (op == "truncate") {
        if (parts.size() < 6) {
            bridgeError(id, 22);
            return;
        }
        size_t size = static_cast<size_t>(strtoul(parts[5].c_str(), nullptr, 10));
        if (size == 0) {
            if (SD.exists(path)) {
                SD.remove(path);
            }
            File file = SD.open(path, FILE_WRITE);
            bool ok = static_cast<bool>(file);
            file.close();
            ok ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
        } else {
            bridgeReply(id, "OK\tDONE");
        }
    } else if (op == "mkdir") {
        SD.mkdir(path) ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
    } else if (op == "rmdir") {
        SD.rmdir(path) ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
    } else if (op == "unlink") {
        SD.remove(path) ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
    } else if (op == "rename") {
        if (parts.size() < 6) {
            bridgeError(id, 22);
            return;
        }
        String to = normalizeSdPath(hexDecodeText(parts[5]));
        SD.rename(path, to) ? bridgeReply(id, "OK\tDONE") : bridgeError(id, 5);
    } else {
        bridgeError(id, 38);
    }
}

void pollStorageBridge()
{
    if (!storageBridgeRunning) {
        return;
    }
    char buffer[512];
    int n = ssh.readBridge(buffer, sizeof(buffer));
    if (n < 0) {
        if (ssh.connected()) {
            return;
        }
        storageBridgeRunning = false;
        appendStatus("Tab5 storage bridge stopped");
        return;
    }
    for (int i = 0; i < n; ++i) {
        char c = buffer[i];
        if (c == '\n') {
            if (storageBridgeLine.length()) {
                bridgeHandleRequest(storageBridgeLine);
            }
            storageBridgeLine = "";
        } else if (c != '\r') {
            if (storageBridgeLine.length() < 8192) {
                storageBridgeLine += c;
            } else {
                storageBridgeLine = "";
            }
        }
    }
}

bool deployServerSetupScript(String& error)
{
    String output;
    const char* script = R"SH(mkdir -p "$HOME/.tab5/bin" &&
cat > "$HOME/.tab5/bin/tab5-server-setup.sh" <<'TAB5_SETUP_EOF'
#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run with sudo: sudo sh ~/.tab5/bin/tab5-server-setup.sh" >&2
  exit 1
fi

if command -v apt-get >/dev/null 2>&1; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-pip fuse3 python3-fusepy libfuse2t64 || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-pip fuse3 python3-fusepy libfuse2 || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-pip fuse3
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y python3 python3-pip fuse3 fuse-libs
elif command -v yum >/dev/null 2>&1; then
  yum install -y python3 python3-pip fuse fuse-libs
elif command -v apk >/dev/null 2>&1; then
  apk add --no-cache python3 py3-pip fuse3 fuse
else
  echo "Unsupported package manager. Install python3, pip, FUSE, and fusepy." >&2
fi

python3 -c 'import fuse' >/dev/null 2>&1 || \
  python3 -m pip install --break-system-packages fusepy || \
  python3 -m pip install fusepy

if [ ! -e /dev/fuse ]; then
  echo "warning: /dev/fuse is missing. Enable FUSE on this server." >&2
fi

echo "Tab5 server setup complete. Reconnect from Tab5 after this finishes."
TAB5_SETUP_EOF
chmod +x "$HOME/.tab5/bin/tab5-server-setup.sh")SH";
    return ssh.execCommand(script, output, error, 8000);
}

bool deployStorageSetupScript(String& error)
{
    String output;
    const char* script = R"SH(mkdir -p "$HOME/.tab5/bin" &&
cat > "$HOME/.tab5/bin/tab5-storage-setup.sh" <<'TAB5_STORAGE_SETUP_EOF'
#!/bin/sh

pkill -9 -f '[t]ab5-fuse-launch.sh' >/dev/null 2>&1 || true
pkill -9 -f '[t]ab5-fuse-server.py' >/dev/null 2>&1 || true
pkill -9 -f '[s]h -c rm -f .*tab5-fuse-server.py' >/dev/null 2>&1 || true
pkill -9 -f '[c]at .*/.tab5/rpc.in' >/dev/null 2>&1 || true
pkill -9 -f '[c]at .*/.tab5/rpc.out' >/dev/null 2>&1 || true

umount_one() {
  if command -v timeout >/dev/null 2>&1; then
    timeout 2 "$@" >/dev/null 2>&1 || true
  else
    "$@" >/dev/null 2>&1 || true
  fi
}

if command -v fusermount3 >/dev/null 2>&1; then
  umount_one fusermount3 -uz "$HOME/.tab5/mnt"
  umount_one fusermount3 -uz "$HOME/sd"
  umount_one fusermount3 -uz "$HOME/usb"
else
  umount_one fusermount -uz "$HOME/.tab5/mnt"
  umount_one fusermount -uz "$HOME/sd"
  umount_one fusermount -uz "$HOME/usb"
fi

mkdir -p "$HOME/.tab5/bin" "$HOME/.tab5/mnt" || exit 1
if [ -e "$HOME/sd" ] && [ ! -L "$HOME/sd" ]; then
  mv "$HOME/sd" "$HOME/.tab5/sd.local.$(date +%s)"
fi
ln -sfn "$HOME/.tab5/mnt/sd" "$HOME/sd" || exit 1
if [ -e "$HOME/usb" ] && [ ! -L "$HOME/usb" ]; then
  mv "$HOME/usb" "$HOME/.tab5/usb.local.$(date +%s)"
fi
ln -sfn "$HOME/.tab5/mnt/usb" "$HOME/usb" || exit 1
rm -f "$HOME/.tab5/rpc.in" "$HOME/.tab5/rpc.out"

python3 -c 'import fuse' >/dev/null 2>&1 || \
  (command -v timeout >/dev/null 2>&1 && timeout 10 python3 -m pip install --user fusepy >/dev/null 2>&1) || \
  (command -v timeout >/dev/null 2>&1 && timeout 10 python3 -m pip install --break-system-packages --user fusepy >/dev/null 2>&1) || true

echo ok
TAB5_STORAGE_SETUP_EOF
chmod +x "$HOME/.tab5/bin/tab5-storage-setup.sh")SH";
    return ssh.execCommand(script, output, error, 8000);
}

void appendServerSetupHint(const String& reason, bool scriptReady)
{
    if (reason.length()) {
        appendStatus(String("Tab5 server setup required: ") + reason);
    } else {
        appendStatus("Tab5 server setup required");
    }
    if (scriptReady) {
        appendStatus("Run on SSH server:");
        appendStatus("sudo sh ~/.tab5/bin/tab5-server-setup.sh");
    } else {
        appendStatus("Install on SSH server: python3, pip, FUSE, fusepy");
    }
    appendStatus("Then reconnect from Tab5: connect");
}

void startStorageBridge()
{
    if (storageBridgeRunning || !ssh.connected()) {
        return;
    }
    if (millis() - storageBridgeLastStart < 3000) {
        return;
    }
    storageBridgeLastStart = millis();
    String output;
    String error;
    appendStatus("Tab5 storage: preparing server mount");
    if (ssh.execCommand("printf %s \"$HOME\"", output, error, 5000)) {
        output.trim();
        sshRemoteHome = output;
    }
    const bool setupScriptReady = deployServerSetupScript(error);
    if (!setupScriptReady) {
        appendStatus(String("Tab5 server setup script deploy failed: ") + error);
    }
    if (!deployStorageSetupScript(error) ||
        !ssh.execCommand("sh \"$HOME/.tab5/bin/tab5-storage-setup.sh\"", output, error, 35000)) {
        appendStatus(String("Tab5 storage setup failed: ") + error);
        appendServerSetupHint("Python/FUSE preparation failed", setupScriptReady);
        return;
    }
    const char* imageScript = R"SH(cat > "$HOME/.tab5/bin/tab5-image" <<'TAB5_IMAGE_EOF'
#!/bin/sh
if [ $# -lt 1 ]; then
  echo "usage: image <file> [fit|center|half|quarter]" >&2
  exit 2
fi
target=$1
mode=${2:-fit}
case "$mode" in
  fit|center|half|quarter) ;;
  *) mode=fit ;;
esac
case "$target" in
  ~/*) target="$HOME/${target#~/}" ;;
esac
dir=$(dirname -- "$target") || exit 1
base=$(basename -- "$target") || exit 1
if ! cd -P -- "$dir" 2>/dev/null; then
  echo "image: cannot access $target" >&2
  exit 1
fi
abs="$PWD/$base"
if [ ! -f "$abs" ]; then
  echo "image: not a file: $target" >&2
  exit 1
fi
hex=$(python3 -c 'import sys; print(sys.argv[1].encode("utf-8").hex())' "$abs") || exit 1
printf '\033]777;tab5-image;pathhex=%s;mode=%s\a\n' "$hex" "$mode"
TAB5_IMAGE_EOF
chmod +x "$HOME/.tab5/bin/tab5-image" &&
ln -sfn "$HOME/.tab5/bin/tab5-image" "$HOME/.tab5/bin/image" &&
mkdir -p "$HOME/.local/bin" &&
ln -sfn "$HOME/.tab5/bin/tab5-image" "$HOME/.local/bin/tab5-image" &&
ln -sfn "$HOME/.tab5/bin/tab5-image" "$HOME/.local/bin/image")SH";
    if (!ssh.execCommand(imageScript, output, error, 8000)) {
        appendStatus(String("Tab5 image command deploy failed: ") + error);
    }
    if (!ssh.scpUpload(config.ssh[activeSsh], LittleFS, "/tab5-fuse-server.py", ".tab5/bin/tab5-fuse-server.py", error)) {
        String check;
        String checkError;
        if (!ssh.execCommand("test -s \"$HOME/.tab5/bin/tab5-fuse-server.py\" && echo ok",
                             check, checkError, 5000) ||
            check.indexOf("ok") < 0) {
            appendStatus(String("Tab5 storage deploy failed: ") + error);
            return;
        }
        appendStatus(String("Tab5 storage deploy skipped; using existing helper: ") + error);
    }
    const char* launcherScript = R"SH(cat > "$HOME/.tab5/bin/tab5-fuse-launch.sh" <<'TAB5_FUSE_LAUNCH_EOF'
#!/bin/sh
base="$HOME/.tab5"
rpc_in="$base/rpc.in"
rpc_out="$base/rpc.out"
rm -f "$rpc_in" "$rpc_out"
mkfifo "$rpc_in" "$rpc_out" || exit 1
cleanup() {
  [ -n "${py_pid:-}" ] && kill "$py_pid" >/dev/null 2>&1 || true
  [ -n "${out_pid:-}" ] && kill "$out_pid" >/dev/null 2>&1 || true
  rm -f "$rpc_in" "$rpc_out"
}
trap cleanup EXIT INT TERM
python3 "$base/bin/tab5-fuse-server.py" --volume all --mount "$base/mnt" --rpc-in "$rpc_in" --rpc-out "$rpc_out" &
py_pid=$!
cat "$rpc_out" &
out_pid=$!
cat > "$rpc_in"
TAB5_FUSE_LAUNCH_EOF
chmod +x "$HOME/.tab5/bin/tab5-fuse-launch.sh")SH";
    if (!ssh.execCommand(launcherScript, output, error, 8000)) {
        appendStatus(String("Tab5 storage launcher deploy failed: ") + error);
        return;
    }
    const String command = "sh \"$HOME/.tab5/bin/tab5-fuse-launch.sh\"";
    if (!ssh.startBridge(command, error)) {
        appendStatus(String("Tab5 storage bridge failed: ") + error);
        appendServerSetupHint("Python/FUSE bridge could not start", setupScriptReady);
        return;
    }
    storageBridgeLine = "";
    storageBridgeRunning = true;
    appendStatus("Tab5 storage mounted at ~/sd and ~/usb");
}

void startStorageBridgeAsync()
{
    if (storageBridgeRunning || storageBridgeJobRunning || !ssh.connected()) {
        return;
    }
    appendStatus("Tab5 storage: starting background mount");
    storageBridgeJobRunning = true;
    BaseType_t created = xTaskCreatePinnedToCore(
        [](void*) {
            startStorageBridge();
            storageBridgeJobRunning = false;
            storageBridgeTaskHandle = nullptr;
            vTaskDelete(nullptr);
        },
        "storage_bridge",
        16384,
        nullptr,
        1,
        &storageBridgeTaskHandle,
        0);
    if (created != pdPASS) {
        storageBridgeTaskHandle = nullptr;
        storageBridgeJobRunning = false;
        appendStatus("Tab5 storage: background task failed");
    }
}

void serialRunScpCommand(const String& command)
{
    String lower = command;
    lower.toLowerCase();
    const bool isGet = lower.startsWith("scp get ");
    const bool isPut = lower.startsWith("scp put ");
    if (!isGet && !isPut) {
        Serial.println("ERR usage");
        return;
    }
    if (!ensureSdReady()) {
        Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
        return;
    }
    String rest = command.substring(isGet ? strlen("scp get ") : strlen("scp put "));
    String first;
    String second;
    String trailing;
    if (!splitScpPaths(rest, first, second, trailing)) {
        Serial.println("ERR missing paths");
        return;
    }

    SshProfile profile;
    String remotePath;
    String localPath;
    const bool direct = isGet ? parseDirectScpEndpoint(first, profile, remotePath)
                              : parseDirectScpEndpoint(second, profile, remotePath);
    if (direct) {
        if (trailing.length()) {
            profile.password = trailing;
        }
        localPath = isGet ? second : first;
    } else {
        if (config.ssh.empty()) {
            Serial.println("ERR no SSH profiles");
            return;
        }
        rest = command.substring(isGet ? strlen("scp get ") : strlen("scp put "));
        size_t profileIndex = activeSsh;
        if (!parseScpProfileIndex(rest, profileIndex)) {
            Serial.println("ERR invalid profile index");
            return;
        }
        if (!splitScpPaths(rest, first, second, trailing)) {
            Serial.println("ERR missing paths");
            return;
        }
        profile = config.ssh[profileIndex];
        remotePath = isGet ? first : second;
        localPath = isGet ? second : first;
    }
    String error;
    bool ok = isGet
        ? ssh.scpDownload(profile, remotePath, SD, normalizeSdPath(localPath), error)
        : ssh.scpUpload(profile, SD, normalizeSdPath(localPath), remotePath, error);
    Serial.println(ok ? "OK scp done" : String("ERR scp failed: ") + error);
}

void serialRunBleCommand(const String& command)
{
    String lower = command;
    lower.toLowerCase();
    if (lower == "ble status") {
        Serial.println(keyboard.bleStatus());
    } else if (lower == "ble devices" || lower == "ble paired") {
        Serial.println(String("OK ") + keyboard.bleDevicesStatus());
    } else if (lower == "ble enable" || lower == "ble disable") {
        config.keyboard.bleKeyboardEnabled = lower == "ble enable";
        keyboard.configure(config.keyboard);
        saveConfig();
        Serial.println(keyboard.bleStatus());
    } else if (lower == "ble scan") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleScan(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
        for (size_t i = 0; i < keyboard.bleScanCount(); ++i) {
            Serial.println(String("ITEM ") + keyboard.bleScanEntry(i));
        }
    } else if (lower == "ble scanraw") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleScanRaw(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble list") {
        Serial.printf("OK count=%u\r\n", static_cast<unsigned>(keyboard.bleScanCount()));
        for (size_t i = 0; i < keyboard.bleScanCount(); ++i) {
            Serial.println(String("ITEM ") + keyboard.bleScanEntry(i));
        }
    } else if (lower == "ble disconnect" || lower.startsWith("ble disconnect ")) {
        String arg = lower == "ble disconnect" ? "all" : lower.substring(strlen("ble disconnect "));
        arg.trim();
        String result;
        bool ok = keyboard.bleDisconnect(arg == "all" ? -1 : arg.toInt(), result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower.startsWith("ble type ")) {
        String args = lower.substring(strlen("ble type "));
        args.trim();
        int space = args.indexOf(' ');
        if (space < 0) {
            Serial.println("ERR usage: ble type <own 0-3> <peer 0-3|255>");
            return;
        }
        uint8_t ownType = static_cast<uint8_t>(args.substring(0, space).toInt());
        uint8_t peerType = static_cast<uint8_t>(args.substring(space + 1).toInt());
        keyboard.bleSetConnectTypes(ownType, peerType);
        Serial.printf("OK ble type own=%u peer=%u\r\n", ownType, peerType);
    } else if (lower.startsWith("ble auth ")) {
        String mode = lower.substring(strlen("ble auth "));
        mode.trim();
        if (mode == "none") {
            keyboard.bleSetSecurity(ESP_LE_AUTH_NO_BOND, false);
            Serial.println("OK ble auth none force=off");
        } else if (mode == "bond") {
            keyboard.bleSetSecurity(ESP_LE_AUTH_BOND, false);
            Serial.println("OK ble auth bond force=off");
        } else if (mode == "scbond") {
            keyboard.bleSetSecurity(ESP_LE_AUTH_REQ_SC_BOND, false);
            Serial.println("OK ble auth scbond force=off");
        } else {
            Serial.println("ERR usage: ble auth <none|bond|scbond>");
        }
    } else if (lower.startsWith("ble force ")) {
        String mode = lower.substring(strlen("ble force "));
        mode.trim();
        if (mode == "on") {
            keyboard.bleSetSecurity(ESP_LE_AUTH_BOND, true);
            Serial.println("OK ble force on auth=bond");
        } else if (mode == "off") {
            keyboard.bleSetSecurity(ESP_LE_AUTH_NO_BOND, false);
            Serial.println("OK ble force off auth=none");
        } else {
            Serial.println("ERR usage: ble force <on|off>");
        }
    } else if (lower == "ble gaptest" || lower.startsWith("ble gaptest ")) {
        String indexText = lower == "ble gaptest" ? "0" : lower.substring(strlen("ble gaptest "));
        indexText.trim();
        String result;
        bool ok = keyboard.bleGapTest(static_cast<size_t>(indexText.toInt()), result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble gapstatus") {
        Serial.println(String("OK ") + keyboard.bleGapStatus());
    } else if (lower == "ble gapclose") {
        String result;
        bool ok = keyboard.bleGapClose(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble gapsecure") {
        String result;
        bool ok = keyboard.bleGapSecure(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble gapservices") {
        String result;
        bool ok = keyboard.bleGapListServices(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble gaphid") {
        String result;
        bool ok = keyboard.bleGapSubscribeHid(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower.startsWith("ble params ")) {
        String args = lower.substring(strlen("ble params "));
        args.trim();
        uint16_t values[6]{};
        bool okArgs = true;
        for (int i = 0; i < 6; ++i) {
            int space = args.indexOf(' ');
            String token = space < 0 ? args : args.substring(0, space);
            token.trim();
            if (!token.length()) {
                okArgs = false;
                break;
            }
            values[i] = static_cast<uint16_t>(token.toInt());
            args = space < 0 ? "" : args.substring(space + 1);
            args.trim();
        }
        if (!okArgs) {
            Serial.println("ERR usage: ble params <scan_itvl> <scan_window> <min> <max> <latency> <timeout>");
            return;
        }
        keyboard.bleSetGapParams(values[0], values[1], values[2], values[3], values[4], values[5]);
        Serial.printf("OK ble params si=%u sw=%u min=%u max=%u lat=%u to=%u\r\n", values[0], values[1], values[2],
                      values[3], values[4], values[5]);
    } else if (lower == "ble gapscan") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleGapScanAndTest(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble gapauto") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleGapScanAndSubscribeHid(result);
        if (ok) {
            saveBlePairingFromKeyboard();
        }
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble arduinotest") {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String result;
        bool ok = keyboard.bleArduinoClientTest(result);
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble pair" || lower.startsWith("ble pair ")) {
        String indexText = lower == "ble pair" ? "0" : lower.substring(strlen("ble pair "));
        indexText.trim();
        String result;
        bool ok = keyboard.blePair(static_cast<size_t>(indexText.toInt()), result);
        if (ok) {
            saveBlePairingFromKeyboard();
        }
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower == "ble scanpair" || lower.startsWith("ble scanpair ")) {
        config.keyboard.bleKeyboardEnabled = true;
        keyboard.configure(config.keyboard);
        saveConfig();
        String pairResult;
        bool pairOk = keyboard.bleScanAndPairFirst(pairResult);
        for (size_t i = 0; i < keyboard.bleScanCount(); ++i) {
            Serial.println(String("ITEM ") + keyboard.bleScanEntry(i));
        }
        if (pairOk) {
            saveBlePairingFromKeyboard();
        }
        Serial.println(String(pairOk ? "OK pair " : "ERR pair ") + pairResult);
    } else if (lower == "ble forget") {
        String result;
        bool ok = keyboard.bleForget(result);
        removeBleDeviceConfig(-1, result);
        saveConfig();
        Serial.println(String(ok ? "OK " : "ERR ") + result);
    } else if (lower.startsWith("ble forget ")) {
        String arg = lower.substring(strlen("ble forget "));
        arg.trim();
        String disconnectResult;
        bool disconnectOk = arg == "all" ? keyboard.bleDisconnect(-1, disconnectResult)
                                         : keyboard.bleDisconnect(arg.toInt(), disconnectResult);
        String result;
        bool ok = removeBleDeviceConfig(arg == "all" ? -1 : arg.toInt(), result);
        Serial.println(String((ok && disconnectOk) ? "OK " : "ERR ") + result + "; " + disconnectResult);
    } else {
        Serial.println("ERR usage");
    }
}

void serialRunPythonCommand(const String& command)
{
    String lower = command;
    lower.toLowerCase();
    if (lower == "python --reset") {
        python.reset();
        Serial.println("OK python state reset");
    } else if (lower.startsWith("python -c ")) {
        String statement = command.substring(strlen("python -c "));
        bool ok = python.runLine(statement, serialPythonLine);
        Serial.println(ok ? "OK python -c" : String("ERR python ") + python.lastError());
    } else if (lower.startsWith("python ")) {
        if (!ensureSdReady()) {
            Serial.printf("ERR sd %s\r\n", sdLastError.c_str());
            return;
        }
        String pathArg;
        String args;
        splitPythonScriptCommand(command.substring(strlen("python ")), pathArg, args);
        String path = normalizeSdPath(pathArg);
        if (!sdPathHasReadPermission(path)) {
            Serial.printf("ERR python permission denied %s\r\n", path.c_str());
            return;
        }
        uint32_t start = millis();
        bool ok = python.runFile(SD, path, args, serialPythonLine);
        Serial.println(ok ? String("OK python done ") + (millis() - start) + " ms"
                          : String("ERR python ") + python.lastError());
    } else if (lower == "python") {
        Serial.println("ERR python REPL is available from the Tab5 CLI");
    } else {
        Serial.println("ERR usage");
    }
}

void serialPrintSshProfiles()
{
    Serial.printf("ssh profiles: %u\r\n", static_cast<unsigned>(config.ssh.size()));
    for (size_t i = 0; i < config.ssh.size(); ++i) {
        const auto& p = config.ssh[i];
        Serial.printf("%c %u: %s %s@%s:%u term=%s\r\n",
                      i == activeSsh ? '*' : ' ',
                      static_cast<unsigned>(i),
                      p.name.c_str(),
                      p.user.c_str(),
                      p.host.c_str(),
                      static_cast<unsigned>(p.port),
                      p.terminal.c_str());
    }
}

void serialDumpTerminal()
{
    Serial.printf("term cols=%u rows=%u cursor=%u,%u alt=%u\r\n",
                  static_cast<unsigned>(vt.columns()),
                  static_cast<unsigned>(vt.rows()),
                  static_cast<unsigned>(vt.cursorColumn()),
                  static_cast<unsigned>(vt.cursorRow()),
                  vt.alternateScreen() ? 1 : 0);
    for (size_t row = 0; row < vt.rows(); ++row) {
        String line;
        line.reserve(vt.columns());
        for (size_t col = 0; col < vt.columns(); ++col) {
            String ch = vt.cell(col, row).ch;
            if (ch.length() == 1 && static_cast<uint8_t>(ch[0]) >= 0x20 && static_cast<uint8_t>(ch[0]) < 0x7F) {
                line += ch;
            } else if (ch == " ") {
                line += ' ';
            } else {
                line += '?';
            }
        }
        while (line.endsWith(" ")) {
            line.remove(line.length() - 1);
        }
        Serial.printf("%02u|%s\r\n", static_cast<unsigned>(row), line.c_str());
    }
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool sendRawHex(const String& hex)
{
    uint8_t bytes[128];
    size_t count = 0;
    int high = -1;
    for (size_t i = 0; i < hex.length(); ++i) {
        int v = hexNibble(hex[i]);
        if (v < 0) {
            continue;
        }
        if (high < 0) {
            high = v;
        } else {
            if (count >= sizeof(bytes)) {
                return false;
            }
            bytes[count++] = static_cast<uint8_t>((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0 || count == 0 || !ssh.connected()) {
        return false;
    }
    return ssh.write(bytes, count);
}

std::vector<String> splitSerialArgs(const String& text)
{
    std::vector<String> args;
    String current;
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            if (current.length()) {
                args.push_back(current);
                current = "";
            }
        } else {
            current += c;
        }
    }
    if (current.length()) {
        args.push_back(current);
    }
    return args;
}

bool parseTrailingIndex(const String& command, size_t prefixLen, size_t& index)
{
    String rest = command.substring(prefixLen);
    rest.trim();
    if (!rest.length()) {
        return false;
    }
    for (size_t i = 0; i < rest.length(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(rest[i]))) {
            return false;
        }
    }
    index = static_cast<size_t>(rest.toInt());
    return true;
}

void handleSerialCommand(String command)
{
    command.trim();
    if (!command.length()) {
        return;
    }
    Serial.print("> ");
    if (command.startsWith("wifi set ") || command.startsWith("ssh set ")) {
        Serial.println("[redacted setup command]");
    } else {
        Serial.println(command);
    }

    if (command == "help" || command == "?") {
        serialPrintHelp();
    } else if (command == "status") {
        serialPrintStatus();
    } else if (command == "crash") {
        Serial.printf("resetStageMagic=0x%08x stage=%s\r\n", static_cast<unsigned>(crashStageMagic), crashStage);
    } else if (command == "sd status") {
        serialPrintSdStatus();
    } else if (command == "sd df" || command == "df") {
        serialPrintSdDf();
    } else if (command == "sd ls" || command.startsWith("sd ls ")) {
        serialPrintSdList(command.length() > strlen("sd ls") ? command.substring(strlen("sd ls ")) : "");
    } else if (command.startsWith("sd cat ")) {
        serialPrintSdCat(command.substring(strlen("sd cat ")));
    } else if (command.startsWith("sd mkdir ")) {
        serialMakeSdDirectory(command.substring(strlen("sd mkdir ")));
    } else if (command.startsWith("sd rmdir ")) {
        serialRemoveSdDirectory(command.substring(strlen("sd rmdir ")));
    } else if (command.startsWith("sd write ")) {
        serialWriteSdText(command, false);
    } else if (command.startsWith("sd append ")) {
        serialWriteSdText(command, true);
    } else if (command.startsWith("sd chmod ")) {
        serialChmodSd(command);
#if ENABLE_USB_HOST_KEYBOARD
    } else if (command == "usb power") {
        bool ok = tab5Usb5vOn();
        Serial.printf("%s %s\r\n", ok ? "OK" : "ERR", tab5UsbPowerStatus.c_str());
    } else if (command == "usb status") {
        Serial.printf("usb5v=%u device=%u msc=%u mounted=%u host=%s status=%s\r\n",
                      tab5Usb5vEnabled ? 1 : 0,
                      usbDevicePresent ? 1 : 0,
                      usbMscPresent ? 1 : 0,
                      usbMscMounted ? 1 : 0,
                      usbHostStatus.c_str(),
                      usbMscStatus.c_str());
        Serial.println("OK");
    } else if (command == "usb rescan") {
        tab5Usb5vSet(false);
        delay(500);
        bool powerOk = tab5Usb5vOn();
        uint32_t start = millis();
        while (millis() - start < 5000) {
            tuh_task();
            delay(5);
        }
        bool mountOk = ensureUsbReady(true);
        Serial.printf("%s power=%s device=%u msc=%u mounted=%u host=%s status=%s\r\n",
                      mountOk ? "OK" : "ERR",
                      powerOk ? "on" : "failed",
                      usbDevicePresent ? 1 : 0,
                      usbMscPresent ? 1 : 0,
                      usbMscMounted ? 1 : 0,
                      usbHostStatus.c_str(),
                      usbMscStatus.c_str());
#endif
    } else if (command.startsWith("cli ")) {
        String line = command.substring(strlen("cli "));
        serialCliCapture = true;
        bool handled = handleTab5CliCommand(line);
        serialCliCapture = false;
        dirty = true;
        Serial.println(handled ? "OK cli" : "ERR cli command not found");
    } else if (command == "fs volumes" || command.startsWith("fs ")) {
        serialFsCommand(command);
    } else if (command == "image" || command.startsWith("image ") ||
               command == "img" || command.startsWith("img ") ||
               command == "view" || command.startsWith("view ")) {
        String lower = command;
        lower.toLowerCase();
        size_t prefixLen = lower.startsWith("image") ? strlen("image") : (lower.startsWith("view") ? strlen("view") : strlen("img"));
        String message;
        bool ok = showImageCommand(command.substring(prefixLen), message);
        if (ok) {
            draw();
            Serial.printf("%s %s\r\n", imageViewerStatus.startsWith("image shown") ? "OK" : "ERR", imageViewerStatus.c_str());
        } else {
            Serial.printf("ERR %s\r\n", message.c_str());
        }
    } else if (command == "wifi status") {
        Serial.printf("wifiStatus=%s wl=%d ip=%s ssid=%s\r\n",
                      wifiStatusText.c_str(),
                      static_cast<int>(WiFi.status()),
                      WiFi.localIP().toString().c_str(),
                      WiFi.SSID().c_str());
    } else if (command.startsWith("wifi set ")) {
        std::vector<String> args = splitSerialArgs(command.substring(strlen("wifi set ")));
        if (args.size() < 2) {
            Serial.println("ERR usage wifi set <ssid> <password>");
            return;
        }
        WifiProfile profile;
        profile.name = args[0];
        profile.ssid = args[0];
        profile.password = args[1];
        if (config.wifi.empty()) {
            config.wifi.push_back(profile);
        } else {
            config.wifi[0] = profile;
        }
        activeWifi = 0;
        saveConfig();
        enableWifiRuntime(20000);
        Serial.printf("OK wifi profile saved ssid=%s\r\n", profile.ssid.c_str());
    } else if (command == "wifi off") {
        stopWifiRuntime();
        Serial.println("OK wifi off");
    } else if (command == "wifi on") {
        enableWifiRuntime(20000);
        Serial.println("OK wifi on");
    } else if (command == "ssh list") {
        serialPrintSshProfiles();
    } else if (command.startsWith("ssh set ")) {
        std::vector<String> args = splitSerialArgs(command.substring(strlen("ssh set ")));
        if (args.size() < 3) {
            Serial.println("ERR usage ssh set <host> <user> <password> [port]");
            return;
        }
        SshProfile profile;
        profile.name = args[1] + "@" + args[0];
        profile.host = args[0];
        profile.user = args[1];
        profile.password = args[2];
        profile.port = args.size() >= 4 ? static_cast<uint16_t>(args[3].toInt()) : 22;
        profile.terminal = "xterm-256color";
        if (config.ssh.empty()) {
            config.ssh.push_back(profile);
        } else {
            config.ssh[0] = profile;
        }
        activeSsh = 0;
        saveConfig();
        Serial.printf("OK ssh profile saved host=%s user=%s port=%u\r\n",
                      profile.host.c_str(),
                      profile.user.c_str(),
                      static_cast<unsigned>(profile.port));
    } else if (command == "term dump") {
        serialDumpTerminal();
    } else if (command.startsWith("ssh active")) {
        size_t index = 0;
        if (!parseTrailingIndex(command, strlen("ssh active"), index) || index >= config.ssh.size()) {
            Serial.println("ERR invalid ssh index");
            return;
        }
        activeSsh = index;
        saveConfig();
        Serial.printf("OK activeSsh=%u\r\n", static_cast<unsigned>(activeSsh));
    } else if (command == "ssh connect" || command.startsWith("ssh connect ")) {
        size_t index = 0;
        if (parseTrailingIndex(command, strlen("ssh connect"), index)) {
            if (index >= config.ssh.size()) {
                Serial.println("ERR invalid ssh index");
                return;
            }
            activeSsh = index;
            saveConfig();
        }
        Serial.printf("OK connecting activeSsh=%u\r\n", static_cast<unsigned>(activeSsh));
        connectActiveSsh();
        serialPrintStatus();
    } else if (command.startsWith("ssh send ")) {
        if (!ssh.connected()) {
            Serial.println("ERR ssh disconnected");
            return;
        }
        String text = command.substring(strlen("ssh send "));
        String payload = text + "\n";
        bool ok = ssh.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
        Serial.println(ok ? "OK sent" : "ERR ssh write failed");
    } else if (command.startsWith("ssh raw ")) {
        String hex = command.substring(strlen("ssh raw "));
        Serial.println(sendRawHex(hex) ? "OK raw sent" : "ERR raw send failed");
    } else if (command == "ssh disconnect") {
        resetStorageBridgeState();
        ssh.disconnect();
        resetCommandEditor();
        configureTerminal();
        appendStatus("SSH disconnected");
        Serial.println("OK");
    } else if (command.startsWith("scp ")) {
        serialRunScpCommand(command);
    } else if (command == "ble" || command.startsWith("ble ")) {
        serialRunBleCommand(command);
    } else if (command == "python" || command == "python --reset" || command.startsWith("python -c ") ||
               command.startsWith("python ")) {
        serialRunPythonCommand(command);
    } else {
        Serial.println("ERR unknown command; type help");
    }
}

void pollSerialApi()
{
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n') {
            handleSerialCommand(serialCommand);
            serialCommand = "";
        } else if (c == '\b' || c == 0x7F) {
            if (serialCommand.length()) {
                serialCommand.remove(serialCommand.length() - 1);
            }
        } else if (std::isprint(static_cast<unsigned char>(c))) {
            if (serialCommand.length() < 1400) {
                serialCommand += c;
            }
        }
    }
}

void initScreenSprite()
{
    screenSprite.setPsram(true);
    screenSprite.setColorDepth(8);
    screenSpriteReady = screenSprite.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
    if (!screenSpriteReady) {
        screenSprite.setColorDepth(4);
        screenSpriteReady = screenSprite.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
    }
    if (!screenSpriteReady) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("Sprite alloc failed", 8, 8);
        while (true) {
            delay(1000);
        }
    }
    Serial.printf("[tab5] [boot] sprite depth=%d size=%ldx%ld\r\n",
                  static_cast<int>(screenSprite.getColorDepth()),
                  static_cast<long>(screenSprite.width()),
                  static_cast<long>(screenSprite.height()));
}

}

void tab5SetCrashStage(const char* stage)
{
    setCrashStage(stage);
}

void tab5SshProgress(const char* stage)
{
    static uint32_t lastProgressDraw = 0;
    if (sshConnectJobRunning || storageBridgeJobRunning) {
        statusLine = String("SSH: ") + stage;
        Serial.print("[tab5] ");
        Serial.println(statusLine);
        dirty = true;
        return;
    }
    appendStatus(String("SSH: ") + stage);
    uint32_t now = millis();
    if (now - lastProgressDraw >= 300 || strcmp(stage, "ssh_connect") == 0 || strcmp(stage, "ssh_ready") == 0) {
        lastProgressDraw = now;
        draw();
    }
}

void setup()
{
    setCrashStage("setup.start");
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    M5.Display.setRotation(3);
    M5.Display.setBrightness(100);
    initScreenSprite();
    configureTerminal();

    Serial.begin(115200);
    WiFi.onEvent(handleWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    terminal.append("Tab5 CLI\n");
    appendStatus("[boot] display ready");
    esp_reset_reason_t resetReason = esp_reset_reason();
    appendStatus(String("[boot] reset reason: ") + static_cast<int>(resetReason));
    if (crashStageMagic == 0x54414235 && strlen(crashStage)) {
        appendStatus(String("[boot] previous stage: ") + crashStage);
    }

    if (!settings.begin()) {
        appendStatus(String("[boot] ") + settings.lastError());
    } else if (!settings.load(config)) {
        appendStatus(String("[boot] ") + settings.lastError());
    } else {
        migrateLegacyLineStep();
        activeWifi = config.activeWifi;
        activeSsh = config.activeSsh;
        appendStatus(String("[boot] profiles loaded: Wi-Fi ") + config.wifi.size() + ", SSH " + config.ssh.size());
    }

#if ENABLE_USB_HOST_KEYBOARD
    if (tab5Usb5vOn()) {
        appendStatus(String("[boot] ") + tab5UsbPowerStatus);
        delay(250);
    } else {
        appendStatus(String("[boot] USB-A power: ") + tab5UsbPowerStatus);
    }
#endif
    keyboard.configure(config.keyboard);
    keyboard.begin();
    appendStatus(String("[boot] ") + keyboard.status());
    if (ensureSdReady()) {
        appendStatus(String("[boot] SD ready: ") + formatBytes(SD.cardSize()));
    } else {
        appendStatus(String("[boot] ") + sdLastError);
    }
#if ENABLE_USB_HOST_KEYBOARD
    appendStatus(String("[boot] USB: ") + usbMscStatus);
#endif
    configureTerminal();

    draw();
    setCrashStage("loop");
    appendStatus("[boot] starting Wi-Fi reconnect");
    startWifiReconnect(20000);
}

void loop()
{
    M5.update();
    pollSerialApi();
    handleTouch();
    keyboard.update();
    while (keyboard.available()) {
        handleAction(keyboard.read());
    }
    pollWifi();
    pollWifiScan();
    pollTimeSync();
    pollSshConnectJob();
    if (!sshConnectJobRunning && !storageBridgeJobRunning) {
        pollSsh();
    }
    if (!sshConnectJobRunning && !storageBridgeJobRunning) {
        pollStorageBridge();
    }
    if (ssh.connected() && !storageBridgeRunning && !storageBridgeJobRunning &&
        millis() - storageBridgeLastStart > 10000) {
        startStorageBridgeAsync();
    }

    if (!imageOverlayActive &&
        (screen == Screen::Terminal || screen == Screen::WifiEdit || screen == Screen::SshEdit ||
         screen == Screen::ConfigEdit) &&
        millis() - lastCursorBlink >= 500) {
        lastCursorBlink = millis();
        if (ssh.connected()) {
            vt.markCursorDirty();
        }
        cursorVisible = !cursorVisible;
        if (ssh.connected()) {
            vt.markCursorDirty();
        }
        dirty = true;
    }

    uint32_t drawInterval = 5;
    if (ssh.connected() && millis() - lastSshReceive < 25 && millis() - lastDraw < 250) {
        drawInterval = 25;
    }

    if (imageOverlayActive && imageOverlayDrawn) {
        dirty = false;
        headerDirty = false;
    } else if (dirty && millis() - lastDraw > drawInterval) {
        lastDraw = millis();
        draw();
    } else if (headerDirty && millis() - lastDraw > drawInterval) {
        lastDraw = millis();
        drawHeaderOnly();
    }
    delay(2);
}
