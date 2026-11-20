# 鼠标电量（MouseBattery）

在 Windows 任务栏通知区（系统托盘）显示无线鼠标电量的小工具。纯 C++ / Win32 实现，不装驱动、不联网、不需要管理员权限。

针对 **MCHOSE G3 A**（YJX-CHIP 接收器，USB VID `A8A5`/`A8A4`，PID `2255`）实测可用。

![托盘图标](docs/tray-preview.png)

## 实际任务栏效果

Windows 11 + MCHOSE G3 A（2.4G 模式）实拍：

![任务栏中的电量图标](docs/taskbar.png)

## 功能

- 托盘图标直接显示电量数字，颜色随电量变化：绿色正常、橙色偏低、红色告急。
- 鼠标右键托盘图标：立即刷新 / 开机自启 / 设备信息 / 打开日志 / 退出。
- 左键单击：立即刷新。
- 每 30 秒尝试查询一次；设备主动上报电量时立即更新。
- 鼠标休眠或暂时读不到时，保留上次电量并显示为灰色，不会误报成 0%。
- 电量低于 15% 时弹出一次气泡提醒。
- 开机自启只写入当前用户的 `HKCU\...\Run`，随时可在菜单里取消。
- 支持多显示器 / 高 DPI；单实例运行。

## 使用方法

1. 双击 `dist\MouseBattery.exe`。
2. 托盘出现一个显示电量的方形图标。
3. 如果图标被 Windows 11 收进了 `^` 溢出区，把它从溢出区**拖到任务栏上**即可固定显示。
4. 右键图标可以刷新、设置开机自启或退出。

> 第一次启动时如果鼠标正在休眠，会先显示 `--` 或上次保存的电量；动一下鼠标，程序收到电量上报后就会更新。

## 电量是怎么读到的

这只鼠标不通过标准 HID Battery Usage 暴露电量，而是走厂商自定义 HID 配置接口：

- HID Usage Page `0xFF01`，Usage `0x10`，报告长度 65 字节。
- 主机发送 65 字节输出报告：`F0 30 01 00 ...`（命令 `0x30`，查询电量与 USB 状态）。
- 鼠标返回输入报告，其中包含 `0xFA` 状态上报；在 `0xFA` 报文里找到 `0xD0`，紧随其后的字节就是电量百分比。例如：

```text
00 AA FA AE 5A 38 01 01 01 D0 4F ...
                              ^^ 79%
```

程序同时兼容直接的 `0x30` 应答（电量在命令字节后第 7 个字节）。

参考实现与协议来源：

- [yjx-sdk-mouse](https://www.npmjs.com/package/yjx-sdk-mouse)（WebHID SDK，支持 VID `A8A5` / PID `2255`，通过 `0x30` 读取电量）
- [mkespn PROTOCOL.md](https://github.com/deepinthewoods/mkespn/blob/master/PROTOCOL.md)（YJX-CHIP 厂商通道帧格式）

## 编译

需要 Visual Studio（含“使用 C++ 的桌面开发”工作负载）和 Windows SDK。

```bat
build.bat
```

输出：`dist\MouseBattery.exe`。使用静态 CRT（`/MT`），目标机器不需要安装 VC++ 运行库。

## 命令行

```text
MouseBattery.exe                     正常启动托盘程序
MouseBattery.exe --diagnose [文件]   读取一次电量，写入文本后退出（默认 %TEMP%\MouseBattery_diagnose.txt）
MouseBattery.exe --help              显示帮助
```

`--diagnose` 适合排查问题，例如：

```powershell
.\dist\MouseBattery.exe --diagnose .\diag.txt
Get-Content .\diag.txt
```

## 文件

```text
MouseBattery/
├─ build.bat                 编译脚本
├─ src/main.cpp              全部源码
├─ dist/MouseBattery.exe     编译好的程序
├─ docs/tray-preview.png     托盘图标示意
├─ docs/taskbar.png          任务栏实际效果
├─ TUTORIAL.md               实现教程（从枚举 HID 到画托盘图标）
└─ README.md
```

运行日志：`%LOCALAPPDATA%\MouseBattery\MouseBattery.log`  
电量缓存：`%LOCALAPPDATA%\MouseBattery\state.ini`

## 常见问题

**托盘没有图标？**  
Windows 11 默认把新图标放进 `^` 溢出区。点开 `^`，把“鼠标电量”图标拖到任务栏上。

**一直显示 `--` 或“鼠标可能已休眠”？**  
确认鼠标底部的电源开关已打开、处于 2.4G 模式，并且接收器插在 USB 口上。动一下鼠标后点“立即刷新”。如果鼠标用的是蓝牙模式而不是接收器，这个程序读不到——它走的是接收器的厂商 HID 通道。

**为什么电量会在 76%～80% 之间跳？**  
鼠标用电量电压估算百分比，发射信号、开灯、充电都会让电压瞬间变化，读数就会跳 1～3 个点；这是正常现象。程序显示的是原始上报值，没有做平滑。想更稳定可以加中位数或迟滞滤波。

**会不会影响鼠标正常使用？**  
不会。程序只打开厂商配置接口做只读查询，不注入输入、不改 DPI / 按键 / 灯效。查询每 30 秒一次，开销可以忽略。

**会不会让鼠标更费电？**  
不会。查询本身很短；鼠标自己的休眠策略不变。程序会在鼠标休眠时保留上次电量。

**为什么不能像笔记本电池那样直接显示在任务栏上？**  
Windows 没有公开的“第三方外设电量”任务栏 API，厂商一般用托盘图标或自绘窗口。这个程序采用托盘图标，是无需驱动、最稳妥的方式。

## 隐私

- 不联网、不上传任何数据。
- 不安装服务/驱动。
- 只读写当前用户目录下的日志和缓存，以及可选的 `HKCU` 开机自启项。

## 已知限制

- 目前只适配 YJX-CHIP（VID `A8A5` / `A8A4`）这一类接收器；其他品牌鼠标的厂商协议不同。
- 鼠标完全关机或处于蓝牙模式时读不到电量，只能显示上次缓存值。
- 厂商驱动/控制软件如果独占打开配置接口，程序会提示“无法打开鼠标配置接口”；退出厂商软件即可。
