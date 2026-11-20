# MouseBattery 实现教程

这份教程按“数据怎么流动”的顺序讲，不要求你事先懂 HID。对着源码看效果最好。

整体数据流：

```text
鼠标 / 2.4G 接收器
        │  65 字节 HID 报告
        ▼
后台线程 YjxBattery：发查询、收上报、解析电量
        │  PostMessage(WM_APP+2)
        ▼
UI 线程：更新共享状态 → GDI+ 画图标 → Shell_NotifyIcon
        │
        ▼
Windows 任务栏通知区
```

对应源码：[src/main.cpp](src/main.cpp)

---

## 1. 为什么不用 Windows 自带的电池 API

Windows 有几套“电池”接口，但它们都不是给鼠标用的：

- `GetSystemPowerStatus`：只返回系统电池（笔记本电池）。
- `Windows.Devices.Power.Battery`：系统电池/聚合电池。
- 标准 HID Battery Usage（Usage Page `0x06`、Usage `0x20`）：规范存在，但这只鼠标没有暴露。

所以只能直接和鼠标接收器的**厂商自定义 HID 接口**通信。

---

## 2. 找到设备：SetupAPI + HID

### 2.1 枚举所有 HID 接口

关键 API：

```cpp
GUID hidGuid;
HidD_GetHidGuid(&hidGuid);

HDEVINFO devices = SetupDiGetClassDevsW(
    &hidGuid, nullptr, nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

// 逐个枚举
SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &ifd);

// 取设备路径 \\?\hid#vid_...
SetupDiGetDeviceInterfaceDetailW(...);
```

对每个路径用 `CreateFileW` 打开，注意共享模式：

```cpp
CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
```

### 2.2 读出 VID / PID / Usage

```cpp
HIDD_ATTRIBUTES attr{ sizeof(attr) };
HidD_GetAttributes(h, &attr);       // VendorID, ProductID

PHIDP_PREPARSED_DATA ppd = nullptr;
HidD_GetPreparsedData(h, &ppd);

HIDP_CAPS caps{};
HidP_GetCaps(ppd, &caps);           // UsagePage, Usage, 报告长度
```

这只鼠标的配置接口是：

```text
VID = 0xA8A5（有线款是 0xA8A4）
PID = 0x2255
UsagePage = 0xFF01
Usage     = 0x0010
InputReportByteLength  = 65
OutputReportByteLength = 65
FeatureReportByteLength = 0
```

匹配逻辑见 `FindDevice()`（`src/main.cpp:355` 附近）。

> 经验：不要只按 VID/PID 认设备，还要校验 UsagePage/Usage。同一个接收器会暴露鼠标、键盘、厂商配置等多个 HID 接口，只有 `FF01/10` 这个能读电量。

---

## 3. 协议：65 字节自定义报告

### 3.1 查询电量

主机发一个 65 字节 output report：

```text
F0 30 01 00 00 00 ... （其余补 0）
│  │  │  │  └─ LEN：本命令为 0
│  │  │  └──── CS：校验，本命令为 0
│  │  └─────── 设备类型：01 = 鼠标
│  └────────── 命令：0x30 = 获取电量 + USB 状态
└───────────── 帧头
```

代码在 `Send()`（`src/main.cpp:458`）：

```cpp
std::vector<BYTE> packet(65, 0);
packet[0] = 0xF0;
packet[1] = 0x30;
packet[2] = 0x01;

HidD_SetOutputReport(h, packet.data(), (DWORD)packet.size());
```

### 3.2 收到的报告

实际抓到的输入报告：

```text
00 AA FA AE 5A 38 01 01 01 D0 4F ...
                        ▲  ▲
                        │  └─ 79%
                        └──── 电量标记 D0
```

规则：

- 在前 8 个字节里找 `0xFA`（状态上报命令）。
- 从 `0xFA` 往后找 `0xD0`。
- `0xD0` 的下一个字节就是电量百分比（0–100）。

有些固件会直接回 `0x30` 命令，此时电量在命令字节后第 7 个字节。解析代码在 `ParseBattery()`（`src/main.cpp:539`）：

```cpp
if (data[i] == 0xFA) {
    for (size_t j = i + 1; j + 1 < size; ++j) {
        if (data[j] == 0xD0) {
            int value = data[j + 1];
            if (value <= 100) { level = value; return true; }
        }
    }
}
```

### 3.3 HID 不是 HTTP

这是初学者最容易踩的坑：

- **不要假设“发一条命令就一定立刻回一条”**。设备可能延迟、合并、主动上报，甚至一次回多帧。
- 正确做法是：后台线程持续 `ReadFile`，收到什么解析什么；查询只是“催一下”。
- `ReadFile` 用 overlapped + 超时；超时后 `CancelIoEx`，下一轮继续读。
- 鼠标休眠时读不到是正常的，保留上次值即可。

### 3.4 这套协议是怎么找到的

不是猜出来的，步骤大致是：

1. 写一个 HID 枚举程序，打印所有接口的 VID/PID、UsagePage/Usage、报告长度。
2. 看到 `FF01/10` 这个非标准接口，确认它不是鼠标/键盘数据口，而是厂商配置口。
3. 找同芯片的公开 SDK/开源实现（本项目参考了 `yjx-sdk-mouse`），拿到候选命令 `0x30`。
4. 用 65 字节 buffer 发 `F0 30 01 ...`，把返回的 65 字节按十六进制打印。
5. 抓到 `00 AA FA ... D0 4F`，对照 SDK 的 `D0` 解析逻辑，确认 `4F` 是电量。
6. 只使用只读查询，不碰 DPI/灯效/恢复出厂等写命令，避免改坏鼠标配置。

---

## 4. 把数字画进任务栏

### 4.1 托盘图标 API

```cpp
NOTIFYICONDATAW nid{};
nid.cbSize = sizeof(nid);
nid.hWnd = hwnd;                  // 一个隐藏窗口
nid.uID = 1;
nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
nid.uCallbackMessage = WM_APP + 1;

Shell_NotifyIconW(NIM_ADD, &nid);
```

之后：

- 更新图标/提示：`NIM_MODIFY`
- 退出：`NIM_DELETE`
- 鼠标事件会以 `WM_APP+1` 发到隐藏窗口，`lParam` 是 `WM_LBUTTONUP` / `WM_RBUTTONUP` 等。

窗口过程见 `WindowProc()`（`src/main.cpp:989`），右键菜单见 `ShowTrayMenu()`（`src/main.cpp:801`）。

### 4.2 自己画图标

Windows 托盘图标就是一个 `HICON`。我们用 GDI+ 画一张 32 位 ARGB 位图：

1. 深色圆角方块；
2. 中间白色电量数字；
3. 底部一条彩色条：绿 / 橙 / 红；
4. `Bitmap::GetHBITMAP` 拿到 HBITMAP；
5. `CreateIconIndirect` 变成 HICON。

代码在 `MakeBatteryIcon()`（`src/main.cpp:586`）。核心：

```cpp
Bitmap bitmap(size, size, PixelFormat32bppARGB);
Graphics graphics(&bitmap);
// ... 画圆角矩形、文字、底部条 ...

HBITMAP color = nullptr;
bitmap.GetHBITMAP(Color(0, 0, 0, 0), &color);

ICONINFO info{};
info.fIcon = TRUE;
info.hbmColor = color;
info.hbmMask = mask;              // 全 0，让 alpha 通道生效
HICON icon = CreateIconIndirect(&info);
```

尺寸用 `GetSystemMetrics(SM_CXSMICON)`，这样 100% / 150% / 200% 缩放都清晰。

### 4.3 Windows 11 的溢出区

Windows 11 默认把新图标放进 `^` 溢出区。`NIF_STATE` 清 `NIS_HIDDEN` 在部分版本上不够，程序里也做了尝试，但最终是否显示由 Shell 决定。

最可靠的办法：点开 `^`，把图标拖到任务栏。  
（本机已经手动固定过一次；如果以后移动了 exe 路径，需要重新拖。）

---

## 5. 线程、缓存和生命周期

### 5.1 两个线程

- **UI 线程**：消息循环、托盘图标、右键菜单、弹窗。
- **后台线程**：打开 HID、发查询、持续读报告。

后台线程不能直接摸 UI，解析到电量后：

```cpp
PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
```

UI 线程收到后重新画图标。共享状态用 `std::mutex` 保护。

### 5.2 后台循环

`WorkerMain()`（`src/main.cpp:865`）的逻辑：

```text
打开设备
loop:
    每 30 秒发一次 0x30 查询
    ReadFile 等 1 秒
    收到报告 → 解析 → 更新状态 → 写缓存
    90 秒没有新数据 → 标记为“上次电量”
    设备拔掉/读错误 → 关闭，重新枚举
```

### 5.3 缓存

成功读到电量后写入：

```text
%LOCALAPPDATA%\MouseBattery\state.ini
```

下次启动先读缓存，图标立刻有数字；再后台刷新。这样鼠标休眠时也不会显示 `--`。

### 5.4 单实例

```cpp
CreateMutexW(nullptr, TRUE, L"Local\\MouseBatteryTray-8B1E4C2A");
```

如果已存在，第二个进程直接退出，避免两个图标。

### 5.5 开机自启

只写当前用户：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
    MouseBattery = "D:\...\MouseBattery.exe"
```

不需要管理员权限，也不写系统目录。

---

## 6. 编译

```bat
build.bat
```

核心命令：

```bat
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MT /W3 ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
   src\main.cpp /Fe:dist\MouseBattery.exe ^
   /link /SUBSYSTEM:WINDOWS comctl32.lib
```

链接的库：

```text
user32 gdi32 shell32 advapi32 setupapi hid gdiplus shlwapi ole32 comctl32
```

`/MT` 是静态 CRT，别人拿到 exe 不需要装 VC++ 运行库。

调试：

```powershell
.\dist\MouseBattery.exe --diagnose .\diag.txt
Get-Content .\diag.txt
```

日志：

```text
%LOCALAPPDATA%\MouseBattery\MouseBattery.log
```

---

## 7. 为什么电量会变？

这是正常现象，不是程序算错。

### 7.1 鼠标用的是“电压估算”

大多数无线鼠标没有库仑计，只有电池电压检测。固件按电压查表估算百分比：

- 鼠标发射 2.4G 信号、LED 亮起时，电池内阻造成**电压瞬间下降**，百分比可能掉 1–3%。
- 停止发射/进入空闲后，电压恢复，百分比又升回去。
- 充电时充电电压把读数抬高，接近满电时会在 78%–82% 之间来回。
- 温度、电池老化也会让曲线漂移。

所以你看到 `76 → 80 → 79 → 80` 这种几秒到几分钟的跳动，是电量计在“电压”和“百分比”之间换算的结果。

### 7.2 为什么日志里一分钟出现好几次？

程序有两条数据来源：

1. 每 30 秒主动发一次 `0x30` 查询；
2. 鼠标自己会主动推 `0xFA` 状态帧（唤醒、充电状态变化、连接变化、电量变化时）。

后台线程一直在读，**收到一帧就更新一次**，所以日志里会出现成对的 80%、80%。日志记录的是“收到的原始帧”，不是“每分钟只采样一次”。

### 7.3 为什么不是严格 30 秒一条？

`0x30` 只是“催一下”，这只鼠标经常不直接回 `0x30`，而是过一会儿推 `0xFA`。所以实际更新节奏是“事件驱动 + 心跳查询”，不是固定采样率。

### 7.4 想让它稳定一点？

现在的程序**故意显示原始值**，没有做滤波。如果你希望图标不要跳，可以加一层很简单的处理：

- **中位数滤波**：保留最近 3 次读数，显示中间值；
- **迟滞**：新值和当前显示值差小于 2% 就不更新；
- **连续确认**：连续 2 次读到同一值才更新。

代价是显示会“慢半拍”。需要的话可以在 `WorkerMain()` 解析出 `level` 后、`PublishState()` 之前加几行。

---

## 8. 你可以继续扩展的方向

- 支持更多厂商协议（Logitech HID++、MCHOSE 自己的 0x11/0x13 协议等）。
- 支持蓝牙 BLE 鼠标的 GATT Battery Service（`0x180F` / `0x2A19`）。
- 把电量历史写进 CSV，画一张曲线。
- 低电量时播放提示音/发 Toast 通知。
- 给图标加充电闪电标记（需要协议里确认充电状态字节）。

---

## 9. 关键文件索引

| 内容 | 位置 |
|---|---|
| 设备枚举 / 匹配 | `src/main.cpp:355` `FindDevice()` |
| 发查询 | `src/main.cpp:458` `Send()` |
| 读报告 | `src/main.cpp:500` `Read()` |
| 解析电量 | `src/main.cpp:539` `ParseBattery()` |
| 画图标 | `src/main.cpp:586` `MakeBatteryIcon()` |
| 更新托盘 | `src/main.cpp:749` `UpdateTrayIcon()` |
| 右键菜单 | `src/main.cpp:801` `ShowTrayMenu()` |
| 后台循环 | `src/main.cpp:865` `WorkerMain()` |
| 窗口过程 | `src/main.cpp:989` `WindowProc()` |
| 程序入口 | `src/main.cpp:1125` `wWinMain()` |
