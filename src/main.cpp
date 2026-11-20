// MouseBattery - show a wireless mouse battery level in the Windows notification area.
//
// Target device: MCHOSE G3 A / YJX-CHIP receiver (VID A8A5 or A8A4, PID 2255),
// HID usage page 0xFF01, usage 0x10.
//
// Protocol (reverse engineered by the yjx-sdk-mouse project):
//   host -> device: 65-byte output report, F0 30 01 00 ...
//   device -> host: input report containing FA ... D0 <level>
//
// The program is deliberately dependency-free: Win32 + HID + GDI+ only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidclass.h>
#include <winioctl.h>
#include <gdiplus.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

namespace {

constexpr wchar_t kAppName[] = L"鼠标电量";
constexpr wchar_t kWindowClass[] = L"MouseBatteryTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\MouseBatteryTray-8B1E4C2A";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MouseBattery";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kMessageBatteryUpdated = WM_APP + 2;
constexpr UINT kMessageShowInfo = WM_APP + 3;

constexpr UINT_PTR kMenuRefresh = 1001;
constexpr UINT_PTR kMenuAutoStart = 1002;
constexpr UINT_PTR kMenuInfo = 1003;
constexpr UINT_PTR kMenuLog = 1004;
constexpr UINT_PTR kMenuExit = 1005;

constexpr UINT_PTR kTimerTray = 1;
constexpr UINT_PTR kTimerWatchdog = 2;

constexpr UINT kPollMs = 30000;

struct BatterySnapshot {
    bool ok = false;
    bool stale = false;
    bool cached = false;
    bool wired = false;
    bool usb = false;
    int level = -1;
    std::wstring product;
    std::wstring error;
    SYSTEMTIME time{};
    ULONGLONG tick = 0;
};

HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_wakeEvent = nullptr;
std::thread g_worker;
std::mutex g_stateMutex;
BatterySnapshot g_state;
bool g_trayAdded = false;
bool g_lowBatteryNotified = false;
ULONGLONG g_lastWakeTick = 0;
UINT g_taskbarCreatedMessage = 0;
NOTIFYICONDATAW g_nid{};
std::wstring g_logPath;
std::mutex g_logMutex;
ULONG_PTR g_gdiplusToken = 0;

std::wstring FormatWin32Error(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, (LPWSTR)&buffer, 0, nullptr);
    std::wstring text;
    if (len && buffer) {
        text.assign(buffer, len);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = L"错误码 " + std::to_wstring(error);
    return text;
}

std::wstring LocalAppDataDirectory() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        result = path;
        CoTaskMemFree(path);
    } else {
        wchar_t buffer[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH)) result = buffer;
    }
    return result;
}

void LogInit() {
    std::wstring dir = LocalAppDataDirectory();
    if (dir.empty()) return;
    dir += L"\\MouseBattery";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    g_logPath = dir + L"\\MouseBattery.log";

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        if (size.QuadPart > 512 * 1024) DeleteFileW(g_logPath.c_str());
    }
}

void Log(const wchar_t* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) return;

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8") != 0 || !file) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(file, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);
    fwprintf(file, L"\n");
    fclose(file);
}

std::wstring FormatTime(const SYSTEMTIME& time) {
    if (time.wYear == 0) return L"—";
    wchar_t buffer[64]{};
    StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
                     time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

bool ParseTime(const std::wstring& text, SYSTEMTIME& time) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (swscanf_s(text.c_str(), L"%d-%d-%d %d:%d:%d", &year, &month, &day,
                  &hour, &minute, &second) != 6) {
        return false;
    }
    time = SYSTEMTIME{};
    time.wYear = (WORD)year;
    time.wMonth = (WORD)month;
    time.wDay = (WORD)day;
    time.wHour = (WORD)hour;
    time.wMinute = (WORD)minute;
    time.wSecond = (WORD)second;
    return true;
}

std::wstring StateFilePath() {
    std::wstring dir = LocalAppDataDirectory();
    if (dir.empty()) return L"";
    dir += L"\\MouseBattery";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\state.ini";
}

void SaveState(const BatterySnapshot& state) {
    if (!state.ok) return;
    const std::wstring path = StateFilePath();
    if (path.empty()) return;

    WritePrivateProfileStringW(L"state", L"level", std::to_wstring(state.level).c_str(), path.c_str());
    WritePrivateProfileStringW(L"state", L"wired", state.wired ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"state", L"product", state.product.c_str(), path.c_str());
    WritePrivateProfileStringW(L"state", L"time", FormatTime(state.time).c_str(), path.c_str());
}

bool LoadState(BatterySnapshot& state) {
    const std::wstring path = StateFilePath();
    if (path.empty()) return false;

    wchar_t buffer[512]{};
    GetPrivateProfileStringW(L"state", L"level", L"", buffer, ARRAYSIZE(buffer), path.c_str());
    if (buffer[0] == 0) return false;
    const int level = _wtoi(buffer);
    if (level < 0 || level > 100) return false;

    state = BatterySnapshot{};
    state.ok = true;
    state.stale = true;
    state.cached = true;
    state.level = level;
    state.wired = GetPrivateProfileIntW(L"state", L"wired", 0, path.c_str()) != 0;
    state.usb = state.wired;

    GetPrivateProfileStringW(L"state", L"product", L"", buffer, ARRAYSIZE(buffer), path.c_str());
    state.product = buffer;

    GetPrivateProfileStringW(L"state", L"time", L"", buffer, ARRAYSIZE(buffer), path.c_str());
    ParseTime(buffer, state.time);
    state.tick = GetTickCount64();
    return true;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH * 4);
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
        if (len == 0) return L"";
        if (len < buffer.size()) return std::wstring(buffer.data(), len);
        buffer.resize(buffer.size() * 2);
    }
}

// ---------------------------------------------------------------------------
// YJX HID battery reader
// ---------------------------------------------------------------------------

class YjxBattery {
  public:
    ~YjxBattery() { Close(); }

    bool Open() { return EnsureOpen(); }
    bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }
    bool RequestBattery() { return Send(0x30, false); }
    bool ReadReport(std::vector<BYTE>& report, DWORD timeoutMs) { return Read(report, timeoutMs); }
    bool HadReadError() const { return readError_; }
    const std::wstring& Product() const { return product_; }
    const std::wstring& Error() const { return lastError_; }
    bool Wired() const { return wired_; }

    bool Query(BatterySnapshot& out) {
        out = BatterySnapshot{};
        out.tick = GetTickCount64();
        GetLocalTime(&out.time);

        if (!EnsureOpen()) {
            out.error = lastError_;
            return false;
        }

        out.product = product_;
        out.wired = wired_;
        out.usb = wired_;

        int level = -1;
        bool usb = wired_;
        bool sent = false;

        // The mouse understands two checksum variants. Try the current one first,
        // then the old one. A sleeping mouse simply won't answer; the caller keeps
        // the last known value and retries later.
        for (int attempt = 0; attempt < 2 && level < 0; ++attempt) {
            if (!Send(0x30, attempt == 1)) break;
            sent = true;
            const DWORD waitMs = attempt == 0 ? 3000 : 2200;
            const ULONGLONG deadline = GetTickCount64() + waitMs;

            while (GetTickCount64() < deadline) {
                DWORD remaining = (DWORD)(deadline - GetTickCount64());
                if (remaining < 120) remaining = 120;
                std::vector<BYTE> report;
                if (!Read(report, remaining)) break;
                if (ParseBattery(report.data(), report.size(), level, usb)) {
                    out.ok = true;
                    out.level = level;
                    out.usb = wired_ || usb;
                    out.error.clear();
                    Log(L"battery: level=%d%%, product=%ls, wired=%d", level, product_.c_str(), wired_ ? 1 : 0);
                    return true;
                }
            }
            if (attempt == 0) Sleep(120);
        }

        out.error = sent ? L"鼠标无响应（可能正在休眠）" : L"无法向鼠标发送查询";
        Log(L"battery query failed: %ls", out.error.c_str());
        Close();
        return false;
    }

    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::wstring product_;
    std::wstring lastError_;
    bool wired_ = false;
    bool readError_ = false;
    USHORT outputLength_ = 65;
    USHORT inputLength_ = 65;

    bool EnsureOpen() {
        if (h_ != INVALID_HANDLE_VALUE) return true;

        if (!FindDevice()) {
            if (lastError_.empty()) {
                lastError_ = L"未找到鼠标接收器，请确认鼠标已开机、接收器已插入";
            }
            return false;
        }

        h_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            lastError_ = L"无法打开鼠标配置接口：" + FormatWin32Error(GetLastError());
            Log(L"CreateFile failed: %ls", lastError_.c_str());
            return false;
        }

        lastError_.clear();
        Log(L"opened HID: %ls (out=%u in=%u)", path_.c_str(), outputLength_, inputLength_);
        return true;
    }

    bool FindDevice() {
        GUID hidGuid{};
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO devices = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) {
            lastError_ = L"枚举 HID 设备失败：" + FormatWin32Error(GetLastError());
            return false;
        }

        bool found = false;
        std::wstring fallbackPath;
        std::wstring fallbackProduct;
        bool fallbackWired = false;
        USHORT fallbackOut = 65;
        USHORT fallbackIn = 65;

        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA interfaceData{};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &interfaceData)) break;

            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
            if (required == 0) continue;

            std::vector<BYTE> detailBuffer(required);
            auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA deviceInfo{};
            deviceInfo.cbSize = sizeof(deviceInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required,
                                                  nullptr, &deviceInfo)) {
                continue;
            }

            HANDLE probe = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) {
                probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            }
            if (probe == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            if (HidD_GetAttributes(probe, &attributes)) {
                const bool knownVendor = attributes.VendorID == 0xA8A5 || attributes.VendorID == 0xA8A4;
                if (knownVendor) {
                    PHIDP_PREPARSED_DATA preparsed = nullptr;
                    if (HidD_GetPreparsedData(probe, &preparsed)) {
                        HIDP_CAPS caps{};
                        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                            caps.UsagePage == 0xFF01 && caps.Usage == 0x0010) {
                            wchar_t product[256]{};
                            HidD_GetProductString(probe, product, sizeof(product));
                            std::wstring productText = product;
                            if (productText.empty()) productText = L"YJX 无线鼠标";

                            USHORT outLen = caps.OutputReportByteLength ? caps.OutputReportByteLength : 65;
                            USHORT inLen = caps.InputReportByteLength ? caps.InputReportByteLength : 65;

                            if (productText.find(L"MCHOSE") != std::wstring::npos) {
                                path_ = detail->DevicePath;
                                product_ = productText;
                                wired_ = attributes.VendorID == 0xA8A4;
                                outputLength_ = outLen;
                                inputLength_ = inLen;
                                found = true;
                            } else if (fallbackPath.empty()) {
                                fallbackPath = detail->DevicePath;
                                fallbackProduct = productText;
                                fallbackWired = attributes.VendorID == 0xA8A4;
                                fallbackOut = outLen;
                                fallbackIn = inLen;
                            }
                        }
                        HidD_FreePreparsedData(preparsed);
                    }
                }
            }
            CloseHandle(probe);
            if (found) break;
        }

        SetupDiDestroyDeviceInfoList(devices);

        if (!found && !fallbackPath.empty()) {
            path_ = fallbackPath;
            product_ = fallbackProduct;
            wired_ = fallbackWired;
            outputLength_ = fallbackOut;
            inputLength_ = fallbackIn;
            found = true;
        }

        if (!found) {
            lastError_ = L"未找到兼容的鼠标（VID A8A5/A8A4，UsagePage FF01）";
        }
        return found;
    }

    bool Send(BYTE command, bool legacyChecksum) {
        if (h_ == INVALID_HANDLE_VALUE) return false;

        const DWORD length = outputLength_ >= 3 ? outputLength_ : 65;
        std::vector<BYTE> packet(length, 0);
        packet[0] = 0xF0;
        packet[1] = command;
        packet[2] = 0x01;
        if (legacyChecksum) {
            BYTE checksum = 0;
            for (DWORD i = 1; i < length; ++i) {
                if (i != 3) checksum = (BYTE)(checksum + packet[i]);
            }
            packet[3] = checksum;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        // HidD_SetOutputReport is the most forgiving path on Windows and is what
        // the vendor tools use. WriteFile (interrupt OUT) is kept as a fallback.
        SetLastError(0);
        BOOL result = HidD_SetOutputReport(h_, packet.data(), length);
        DWORD error = GetLastError();
        if (!result) {
            DWORD written = 0;
            result = WriteFile(h_, packet.data(), length, &written, &overlapped);
            DWORD writeError = GetLastError();
            if (!result && writeError == ERROR_IO_PENDING) {
                if (WaitForSingleObject(overlapped.hEvent, 2000) == WAIT_OBJECT_0) {
                    result = GetOverlappedResult(h_, &overlapped, &written, FALSE);
                }
                writeError = GetLastError();
            }
            Log(L"send: SetOutputReport failed (%lu), WriteFile result=%d error=%lu",
                error, result ? 1 : 0, writeError);
        }
        CloseHandle(overlapped.hEvent);
        return result != FALSE;
    }

    bool Read(std::vector<BYTE>& report, DWORD timeoutMs) {
        if (h_ == INVALID_HANDLE_VALUE) return false;

        readError_ = false;
        const DWORD length = inputLength_ >= 2 ? inputLength_ : 65;
        report.assign(length, 0);

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD read = 0;
        BOOL result = ReadFile(h_, report.data(), length, &read, &overlapped);
        DWORD error = GetLastError();
        if (!result && error == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (wait == WAIT_OBJECT_0) {
                result = GetOverlappedResult(h_, &overlapped, &read, FALSE);
            } else {
                CancelIoEx(h_, &overlapped);
                WaitForSingleObject(overlapped.hEvent, 100);
                result = FALSE;
                error = ERROR_TIMEOUT;
            }
        }
        CloseHandle(overlapped.hEvent);

        if (!result || read == 0) {
            if (!result && error != ERROR_TIMEOUT && error != ERROR_OPERATION_ABORTED) {
                readError_ = true;
            }
            report.clear();
            return false;
        }
        report.resize(read);
        return true;
    }

  public:
    static bool ParseBattery(const BYTE* data, size_t size, int& level, bool& usb) {
        if (!data || size < 4) return false;
        // Reports have been observed as:
        //   00 AA FA AE ... D0 50 ...   (0xFA push/status, D0 followed by level)
        //   F0 30 01 CS ... 50 01 ...   (direct 0x30 reply, level at +7)
        // Tolerate a leading report-id byte and the 0xAA response marker.
        for (size_t i = 0; i < size && i < 8; ++i) {
            if (data[i] == 0xFA) {
                for (size_t j = i + 1; j + 1 < size; ++j) {
                    if (data[j] == 0xD0) {
                        const int value = data[j + 1];
                        if (value >= 0 && value <= 100) {
                            level = value;
                            usb = false;
                            return true;
                        }
                    }
                }
            }
            if (data[i] == 0x30) {
                if (i + 8 < size) {
                    const int value = data[i + 7];
                    if (value >= 0 && value <= 100) {
                        level = value;
                        usb = data[i + 8] == 1;
                        return true;
                    }
                }
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Tray icon drawing
// ---------------------------------------------------------------------------

void AddRoundedRect(GraphicsPath& path, REAL x, REAL y, REAL width, REAL height, REAL radius) {
    const REAL diameter = radius * 2.0f;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

HICON MakeBatteryIcon(const BatterySnapshot& state) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    if (size > 64) size = 64;

    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
    graphics.Clear(Color(0, 0, 0, 0));

    const REAL scale = (REAL)size / 16.0f;
    const REAL inset = 0.45f * scale;
    GraphicsPath background;
    AddRoundedRect(background, inset, inset, (REAL)size - inset * 2.0f,
                   (REAL)size - inset * 2.0f, 3.4f * scale);

    SolidBrush backgroundBrush(Color(235, 27, 29, 34));
    graphics.FillPath(&backgroundBrush, &background);
    Pen borderPen(Color(95, 255, 255, 255), 0.75f * scale);
    graphics.DrawPath(&borderPen, &background);

    const bool hasLevel = state.ok && state.level >= 0 && state.level <= 100;
    wchar_t text[8]{};
    if (hasLevel) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%d", state.level);
    } else {
        StringCchCopyW(text, ARRAYSIZE(text), L"--");
    }

    Color accent;
    if (!hasLevel) {
        accent = Color(255, 145, 148, 156);
    } else if (state.stale) {
        accent = Color(255, 130, 134, 142);
    } else if (state.level <= 15) {
        accent = Color(255, 235, 72, 72);
    } else if (state.level <= 40) {
        accent = Color(255, 242, 164, 48);
    } else {
        accent = Color(255, 62, 202, 112);
    }

    if (hasLevel) {
        SolidBrush barBrush(accent);
        graphics.FillRectangle(&barBrush, 2.8f * scale, 13.15f * scale, 10.4f * scale, 1.55f * scale);
    }

    const bool threeDigits = hasLevel && state.level >= 100;
    const REAL fontSize = (threeDigits ? 7.7f : 10.3f) * scale;
    Font font(L"Segoe UI", fontSize, FontStyleBold, UnitPixel);
    SolidBrush textBrush(hasLevel ? Color(255, 255, 255, 255) : Color(255, 190, 193, 200));
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF textRect(0.0f, -0.35f * scale, (REAL)size, 13.2f * scale);
    graphics.DrawString(text, -1, &font, textRect, &format, &textBrush);

    HBITMAP colorBitmap = nullptr;
    if (bitmap.GetHBITMAP(Color(0, 0, 0, 0), &colorBitmap) != Ok || !colorBitmap) {
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    const int maskStride = ((size + 31) / 32) * 4;
    std::vector<BYTE> maskBits((size_t)maskStride * size, 0);
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = maskBitmap;
    info.hbmColor = colorBitmap;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);

    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

std::wstring MakeTooltip(const BatterySnapshot& state) {
    std::wstring text = state.product.empty() ? kAppName : state.product;
    if (state.ok) {
        text += L"  电量 " + std::to_wstring(state.level) + L"%";
        if (state.stale) {
            text += state.cached ? L"（上次保存）" : L"（鼠标可能休眠）";
        } else {
            text += state.wired ? L"  USB 有线" : L"  2.4G 无线";
        }
    } else {
        text += L"  电量未知";
        if (!state.error.empty()) text += L"：" + state.error;
    }
    text += L"\n右键菜单：立即刷新 / 开机自启 / 退出";
    return text;
}

// ---------------------------------------------------------------------------
// Tray / window plumbing
// ---------------------------------------------------------------------------

bool IsAutoStartEnabled() {
    wchar_t value[MAX_PATH * 4]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ,
                        nullptr, value, &size) == ERROR_SUCCESS;
}

void SetAutoStartEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enabled) {
        std::wstring command = L"\"" + ExecutablePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void ShowInfoDialog() {
    BatterySnapshot state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }

    std::wstring text;
    text += L"设备：" + (state.product.empty() ? std::wstring(L"未识别") : state.product) + L"\r\n";
    if (state.ok) {
        text += L"电量：" + std::to_wstring(state.level) + L"%\r\n";
        text += std::wstring(L"连接：") + (state.wired ? L"USB 有线" : L"2.4G 无线") + L"\r\n";
        if (state.stale) {
            text += state.cached ? L"状态：上次保存的电量，正在刷新…\r\n"
                                 : L"状态：长时间未更新（鼠标可能已休眠）\r\n";
        } else {
            text += L"状态：正常\r\n";
        }
        text += L"更新时间：" + FormatTime(state.time) + L"\r\n";
    } else {
        text += L"电量：尚未读取到\r\n";
        if (!state.error.empty()) text += L"原因：" + state.error + L"\r\n";
    }
    text += L"\r\n程序每 30 秒自动刷新；鼠标休眠时会保留上次电量。\r\n";
    text += L"如果托盘图标被藏在 ^ 溢出区，可以把它拖到任务栏上。";

    MessageBoxW(nullptr, text.c_str(), L"鼠标电量",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

void OpenLogFile() {
    Log(L"log opened from tray menu");
    if (!g_logPath.empty()) {
        ShellExecuteW(nullptr, L"open", L"notepad.exe", g_logPath.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void UpdateTrayIcon(bool add) {
    BatterySnapshot state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }

    HICON icon = MakeBatteryIcon(state);
    NOTIFYICONDATAW data = g_nid;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    if (add) {
        // Ask the shell not to hide the icon in the overflow area. Windows 11 may
        // still put new icons there, but this is the documented request to show it.
        data.uFlags |= NIF_STATE;
        data.dwState = 0;
        data.dwStateMask = NIS_HIDDEN;
    }
    data.hIcon = icon;
    StringCchCopyNW(data.szTip, ARRAYSIZE(data.szTip), MakeTooltip(state).c_str(),
                    ARRAYSIZE(data.szTip) - 1);

    const BOOL ok = Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data);
    if (ok) {
        HICON previous = g_nid.hIcon;
        g_nid.hIcon = icon;
        g_trayAdded = true;
        if (previous) DestroyIcon(previous);
    } else {
        DestroyIcon(icon);
        if (add) {
            g_trayAdded = false;
            SetTimer(g_hwnd, kTimerTray, 2000, nullptr);
        }
    }
}

void RemoveTrayIcon() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
}

void TriggerRefresh() {
    g_lastWakeTick = GetTickCount64();
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool autoStart = IsAutoStartEnabled();
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"立即刷新(&R)");
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0), kMenuAutoStart, L"开机自启(&A)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuInfo, L"设备信息(&I)");
    AppendMenuW(menu, MF_STRING, kMenuLog, L"打开日志(&L)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出(&X)");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_hwnd);
    // Right/bottom align keeps the menu inside the screen when the taskbar is
    // on the right edge (vertical taskbar) or at the bottom.
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                     point.x, point.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void NotifyLowBattery() {
    BatterySnapshot state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }
    if (!state.ok || state.stale) return;

    if (state.level > 20) {
        g_lowBatteryNotified = false;
        return;
    }
    if (state.level > 15 || g_lowBatteryNotified) return;

    g_lowBatteryNotified = true;
    NOTIFYICONDATAW data = g_nid;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    data.uTimeout = 10000;
    StringCchCopyW(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), L"鼠标电量低");
    std::wstring message = L"鼠标剩余电量 " + std::to_wstring(state.level) + L"，请及时充电。";
    StringCchCopyW(data.szInfo, ARRAYSIZE(data.szInfo), message.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void OnBatteryUpdated() {
    UpdateTrayIcon(false);
    NotifyLowBattery();
}

void PublishState(const BatterySnapshot& state) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state = state;
    }
    if (g_hwnd && IsWindow(g_hwnd)) {
        PostMessageW(g_hwnd, kMessageBatteryUpdated, 0, 0);
    }
}

void WorkerMain() {
    YjxBattery device;
    ULONGLONG lastRequest = 0;
    ULONGLONG lastData = GetTickCount64();
    ULONGLONG lastOpenErrorPost = 0;
    bool stale = false;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        if (!device.IsOpen()) {
            if (!device.Open()) {
                const ULONGLONG now = GetTickCount64();
                if (now - lastOpenErrorPost > 30000) {
                    lastOpenErrorPost = now;
                    BatterySnapshot state;
                    state.tick = now;
                    GetLocalTime(&state.time);
                    state.error = device.Error().empty() ? L"未找到鼠标接收器" : device.Error();
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        if (g_state.ok) {
                            g_state.stale = true;
                            g_state.error = state.error;
                        } else {
                            g_state = state;
                        }
                    }
                    if (g_hwnd && IsWindow(g_hwnd)) {
                        PostMessageW(g_hwnd, kMessageBatteryUpdated, 0, 0);
                    }
                }
                WaitForSingleObject(g_stopEvent, 5000);
                continue;
            }
            lastRequest = 0;
            lastData = GetTickCount64();
            stale = false;
            lastOpenErrorPost = 0;
        }

        if (WaitForSingleObject(g_wakeEvent, 0) == WAIT_OBJECT_0) {
            ResetEvent(g_wakeEvent);
            lastRequest = 0;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastRequest >= kPollMs) {
            if (!device.RequestBattery()) {
                device.Close();
                WaitForSingleObject(g_stopEvent, 1000);
                continue;
            }
            lastRequest = now;
        }

        std::vector<BYTE> report;
        if (device.ReadReport(report, 1000)) {
            int level = -1;
            bool usb = false;
            if (YjxBattery::ParseBattery(report.data(), report.size(), level, usb)) {
                BatterySnapshot state;
                state.ok = true;
                state.level = level;
                state.wired = device.Wired();
                state.usb = device.Wired() || usb;
                state.product = device.Product();
                state.tick = now;
                GetLocalTime(&state.time);
                PublishState(state);
                SaveState(state);
                Log(L"battery update: level=%d%%, product=%ls", level, state.product.c_str());
                lastData = now;
                stale = false;
            }
        } else if (device.HadReadError()) {
            device.Close();
            WaitForSingleObject(g_stopEvent, 1000);
            continue;
        }

        if (!stale && GetTickCount64() - lastData > 90000) {
            stale = true;
            BatterySnapshot state;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                state = g_state;
            }
            if (state.ok) {
                state.stale = true;
                state.cached = false;
                state.error = L"鼠标可能已休眠，显示的是上次电量";
                state.tick = GetTickCount64();
                PublishState(state);
            }
        }
    }

    device.Close();
}

void OnWatchdogTimer() {
    if (!g_trayAdded) {
        UpdateTrayIcon(true);
        return;
    }

    BatterySnapshot state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastWakeTick < 20000) return;

    LASTINPUTINFO input{};
    input.cbSize = sizeof(input);
    if (!GetLastInputInfo(&input)) return;
    const DWORD idle = GetTickCount() - input.dwTime;
    if (idle > 5000) return;

    const ULONGLONG age = state.tick ? now - state.tick : now;
    if (age > 60000) TriggerRefresh();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        g_trayAdded = false;
        UpdateTrayIcon(true);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuRefresh:
            TriggerRefresh();
            break;
        case kMenuAutoStart:
            SetAutoStartEnabled(!IsAutoStartEnabled());
            break;
        case kMenuInfo:
            ShowInfoDialog();
            break;
        case kMenuLog:
            OpenLogFile();
            break;
        case kMenuExit:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case kMessageBatteryUpdated:
        OnBatteryUpdated();
        return 0;

    case kMessageShowInfo:
        ShowInfoDialog();
        return 0;

    case WM_TIMER:
        if (wParam == kTimerTray) {
            KillTimer(hwnd, kTimerTray);
            if (!g_trayAdded) UpdateTrayIcon(true);
        } else if (wParam == kTimerWatchdog) {
            OnWatchdogTimer();
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            TriggerRefresh();
        }
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerWatchdog);
        KillTimer(hwnd, kTimerTray);
        RemoveTrayIcon();
        if (g_stopEvent) SetEvent(g_stopEvent);
        if (g_worker.joinable()) g_worker.join();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    // With the classic NOTIFYICON_VERSION, the callback message is delivered with
    // lParam set to the mouse message.
    if (message == kTrayMessage) {
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            TriggerRefresh();
            break;
        case WM_LBUTTONDBLCLK:
            ShowInfoDialog();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        default:
            break;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int RunDiagnose(const std::wstring& outputPath) {
    YjxBattery device;
    BatterySnapshot snapshot;
    const bool ok = device.Query(snapshot);

    std::wstring path = outputPath;
    if (path.empty()) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        path = temp;
        path += L"MouseBattery_diagnose.txt";
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"ok=%d\r\n", ok ? 1 : 0);
        fwprintf(file, L"product=%ls\r\n", snapshot.product.c_str());
        fwprintf(file, L"level=%d\r\n", snapshot.level);
        fwprintf(file, L"wired=%d\r\n", snapshot.wired ? 1 : 0);
        fwprintf(file, L"error=%ls\r\n", snapshot.error.c_str());
        fwprintf(file, L"time=%ls\r\n", FormatTime(snapshot.time).c_str());
        fclose(file);
    }

    Log(L"diagnose: ok=%d level=%d product=%ls error=%ls", ok ? 1 : 0, snapshot.level,
        snapshot.product.c_str(), snapshot.error.c_str());
    return ok ? 0 : 2;
}

void EnableDpiAwareness() {
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setContext = reinterpret_cast<SetDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext) {
        setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    EnableDpiAwareness();
    LogInit();
    Log(L"MouseBattery starting, exe=%ls", ExecutablePath().c_str());

    GdiplusStartupInput gdiplusInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Ok) {
        g_gdiplusToken = 0;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool diagnose = false;
    bool showHelp = false;
    std::wstring diagnosePath;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--diagnose") == 0) {
                diagnose = true;
                if (i + 1 < argc) diagnosePath = argv[i + 1];
            } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
                showHelp = true;
            }
        }
        LocalFree(argv);
    }

    if (showHelp) {
        MessageBoxW(nullptr,
                    L"MouseBattery - 在任务栏通知区显示鼠标电量\n\n"
                    L"直接双击运行即可。\n"
                    L"右键托盘图标：立即刷新 / 开机自启 / 设备信息 / 退出。\n\n"
                    L"命令行：\n"
                    L"  MouseBattery.exe --diagnose [输出文件]\n"
                    L"      读取一次电量并写入文本文件后退出。",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    if (diagnose) {
        const int result = RunDiagnose(diagnosePath);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return result;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing) PostMessageW(existing, kMessageShowInfo, 0, 0);
        CloseHandle(mutex);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass)) {
        MessageBoxW(nullptr, L"注册窗口类失败。", kAppName, MB_OK | MB_ICONERROR);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"创建托盘窗口失败。", kAppName, MB_OK | MB_ICONERROR);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = nullptr;
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"鼠标电量：正在读取…");

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_wakeEvent) {
        MessageBoxW(nullptr, L"创建同步事件失败。", kAppName, MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    {
        BatterySnapshot cached;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (LoadState(cached)) {
            g_state = cached;
            Log(L"loaded cached battery: level=%d%%, product=%ls", cached.level, cached.product.c_str());
        } else {
            g_state.error = L"正在读取…";
        }
    }
    UpdateTrayIcon(true);
    SetTimer(hwnd, kTimerWatchdog, 5000, nullptr);

    g_worker = std::thread(WorkerMain);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_worker.joinable()) g_worker.join();
    if (g_stopEvent) CloseHandle(g_stopEvent);
    if (g_wakeEvent) CloseHandle(g_wakeEvent);
    if (mutex) CloseHandle(mutex);
    if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
    Log(L"MouseBattery exited");
    return (int)message.wParam;
}
