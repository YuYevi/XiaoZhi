# BAJI Touch 1.85 LCD ML307

本板级来源于 `C:\Zero\BAJI工具\BAJI-V2.0.48` 中的 `main/boards/baji/BAJI`，目标为 ESP32-S3、360 × 360 ST77916 QSPI 液晶屏、ES8311 音频及 ML307 4G 模块。

板级组织参考本项目 Git 历史 d6e4a45 中的微雪 esp32-s3-touch-lcd-1.85：根目录放同名板级入口和配置，新增驱动及适配放 common。移植 BAJI 的硬件、按键、网络切换、电源及必要显示布局，复用小智原生字体、表情、主题和应用流程。服务器背景下载与轮播不在移植范围内。

已接入 CST836U 单点触控，参考同项目微雪 1.83 / 1.54 的方式复用现有 `esp_lcd_touch_cst816s` 和 `lvgl_port_add_touch()`，没有新增触控驱动文件。当前注册为 LVGL 指针输入设备，未绑定点击对话、滑动切换等应用操作。

## 硬件连接

具体配置以本目录 `config.h` 为准。默认使用 16 MB Flash、QIO/80 MHz、80 MHz 八线 PSRAM 和 `partitions/v2/16m.csv` 分区表。

| 功能 | ESP32-S3 引脚或接口 | 说明 |
| --- | --- | --- |
| 共享 I²C SDA / SCL | GPIO6 / GPIO7 | ES8311、TCA9554 与 CST836U，共用 I²C0 |
| 触控地址 / 中断 | `0x15` / GPIO17 | CST836U，低有效，启用中断脚上拉 |
| TCA9554 地址 | `0x20` | 扩展引脚见下表 |
| LCD QSPI 时钟 / CS | GPIO12 / GPIO10 | SPI2_HOST，ST77916，RGB565 |
| LCD QSPI D0 / D1 / D2 / D3 | GPIO11 / GPIO13 / GPIO14 / GPIO9 | 360 × 360，无坐标偏移 |
| LCD 背光 | GPIO15 | LEDC PWM，非反相 |
| LCD / 触控复位 | TCA9554 P7 | 共用复位；触控驱动配置为 NC，沿用 LCD 初始化前的复位 |
| I²S MCLK / BCLK / WS | GPIO45 / GPIO41 / GPIO46 | 默认输入、输出均为 24 kHz |
| I²S DOUT / DIN | GPIO42 / GPIO40 | 默认使用 ES8311 ADC 输入 |
| 可选外部数字麦克风 DIN | GPIO39 | `AUDIO_INPUT_USE_SILICON_MIC=1` 时使用，默认关闭 |
| 功放使能 | TCA9554 P5 | 由板级音频实现控制 |
| ML307 UART TX / RX / DTR | GPIO47 / GPIO48 / GPIO38 | TX/RX 均以 ESP32-S3 为参照 |
| BOOT 按键 | GPIO0 | 低电平有效 |
| 电源按键 | GPIO2 | 低电平有效，支持 RTC 唤醒 |
| 电源保持 | GPIO1 | 高电平保持供电 |
| USB 供电检测 | GPIO5 | 默认 GPIO 检测，高电平表示有 USB 电源 |
| 电池检测 | ADC1_CHANNEL_3（GPIO4） | 沿用源板级 ADC 与电量映射 |

| TCA9554 引脚 | 功能 |
| --- | --- |
| P0 | ML307 PWRON |
| P1 | ML307 RESET |
| P2 | 音量加按键，低电平有效 |
| P3 | 音量减按键，低电平有效 |
| P5 | 音频功放使能 |
| P6 | 运行指示灯，默认未启用自动闪烁 |
| P7 | LCD / 触控共用复位 |

## 按键与网络

未保存网络选择时默认使用 Wi-Fi；默认值由板级入口 `DualNetworkBoard(..., 0)` 的最后一个参数控制（`1` 为 4G，`0` 为 Wi-Fi）。已有选择保存在 NVS `network/type` 中，下次启动优先恢复。板级直接继承小智原生 `DualNetworkBoard`，由原生 `Ml307Board` 和 `78/esp-ml307` 组件完成模组检测、网络注册及 HTTP/MQTT/WebSocket 通信，使用原生 921600 波特率设置。

BAJI 仅保留 TCA9554 上的模组电源控制：启动原生联网前通过组件自带的 `AtUart` 检查 AT 通信是否可用，按需发送 PWRON 脉冲，避免将型号查询失败误判为关机、在 ESP32 软重启时误关模组。4G 启动时恢复旧固件可能遗留的飞行模式。临时探测对象先释放串口，再启动原生联网；Wi-Fi 模式关闭检测到的模组。

| 操作 | 行为 |
| --- | --- |
| BOOT 单击 | 唤醒显示并切换对话状态；Wi-Fi 启动阶段可进入配网；4G 正在连接时显示等待提示 |
| 音量加单击 | 音量增加 10，最高 100 |
| 音量减单击 | 音量减少 10，最低 0 |
| 音量减长按 2 秒 | 静音 |
| 音量加长按 3 秒 | 显示 Wi-Fi/4G 切换提示；5 秒内再次短按音量加确认 |

网络切换调用原生 `SwitchNetworkType()`，保存网络选择后重启设备，不再执行运行时热切换。设备正在升级、激活等忙碌状态时会提示稍后切换。切换确认提示显示 2 秒，确认窗口仍为 5 秒。

## 显示与资源

与微雪 1.85 相同，使用小智组件提供的 `font_noto_sans_basic_16_4`、`font_material_symbols_16_4` 和 `noto-color-emoji_64`。运行字体和语音资源由原生构建流程生成，不再携带板级字体副本，也没有资源包字体替换脚本。

`common/baji_display.*` 继承 `SpiLcdDisplay`，先调用原生界面创建，再调整 BAJI 的中央网络/电池状态组、百分比、独立右侧静音标签及字幕上移。原生实现负责文字、表情、GIF、主题、时钟和低电提醒；纯充电页作为板级扩展保留。

触控注册也集中在已有 `common/baji_display.*` 中，正常启动在 LCD 初始化完成后调用 `InitializeTouch()`，纯充电路径不注册输入。LVGL 通过 GPIO17 中断读取触点，无额外轮询任务；坐标范围为 0–359，方向跟随 `DISPLAY_SWAP_XY` / `DISPLAY_MIRROR_X` / `DISPLAY_MIRROR_Y`。

复用依据：[CST836U 公开示例](https://github.com/noerbany/CST836U_library/blob/main/cst836u.cpp) 使用 I²C 地址 `0x15`、点数寄存器 `0x02`、坐标寄存器 `0x03`–`0x06`，以及低有效 IRQ，与现有 CST816S 组件的单点协议相同。此依据来自社区示例；实际触点和屏幕方向仍需上板验证。若启动时遇到 `CST816S: Read ID failed`，可在 `menuconfig → Component config → ESP LCD TOUCH - CST816S` 启用 `Disable reading ID during initialization`（`CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`）；部分同系列芯片仅在触摸事件后回应 I²C。

Wi-Fi 和电池继续使用 BAJI 原始位图，4G、静音及系统提示使用原生 Material 图标。BAJI 源 Wi-Fi 位图只有底部三行包含可见像素，这一资源特点保持原样。本版本采用原生字体，因此字形、文字度量和字体图标遵循当前小智，先前采用 Puhui/Font Awesome 的像素对比报告不作为本版本验证依据。

## 开关机、充电与节能

- 正常电源按键开机门限为持续 3 秒。深睡唤醒时计入触发唤醒的这次按压，无需再按一次；不足门限则继续深睡。
- 运行时长按电源键约 2.6 秒触发关机，源配置为 13 次、每次 200 ms 的采样。关机流程提示、熄屏、释放电源保持，并在松键后进入深睡眠。
- USB 上电及符合条件的 USB 深睡唤醒进入纯充电界面，背光为 5%。此路径不启动小智应用、Wi-Fi 或 4G；持续按住电源键 3 秒后重启进入正常应用。
- 纯充电时拔掉 USB 会下电，包括 USB 恰好在 LCD 初始化期间被拔掉的情况。带 USB 关机保存 RTC 标记；之后拔 USB 再按键唤醒时，首次长按仍进入正常开机门控。
- 电池供电时遵循原生节能设置及应用空闲判定：约 60 秒进入节能显示，背光降至 1%；约 300 秒请求关机。按键及应用唤醒可恢复显示，接入 USB 时暂停此自动节能计时。
- 若关机任务分配失败，复用已创建的电源定时器检查松键后进入深睡，不在共享 ESP_TIMER 回调中等待按键或获取 Board/UI 锁。

## 板级结构

共 13 个文件。参照微雪 1.85，同名板级 `.cc` 集中 I2C、TCA9554、QSPI、ST77916 和按键初始化，构造函数按顺序调用 `Initialize*`。正常启动与纯充电使用同一个显示工厂，共用总线和扩展芯片。

```text
baji-touch-1.85lcd-ml307/
├── baji-touch-1.85lcd-ml307.cc
├── config.h
├── config.json
├── README.md
└── common/
    ├── board.cmake
    ├── baji_audio_codec.cc
    ├── baji_audio_codec.h
    ├── baji_display.cc
    ├── baji_display.h
    ├── baji_185_status_icons.c
    ├── power_boot.cc
    ├── power_manager.cc
    └── power_manager.h
```

`config.h` 按微雪的紧凑分组和对齐格式整理；原 BAJI 的 79 个宏完整保留，宏名和值没有替换成微雪参数。原有虚拟 GPIO 宏也保留在配置中，但实际驱动通过扩展芯片位掩码访问对应引脚。原生 `Ml307Board` 不读取 BAJI 的 eDRX 宏，本版本不主动配置 eDRX。

| common 文件 | 职责 |
| --- | --- |
| `board.cmake` | 收集 common 源文件，使用原生字体资源，设置启动包装 |
| `baji_display.cc/.h` | 继承原生显示，调整板级布局、提供充电页并注册触控输入 |
| `baji_185_status_icons.c` | BAJI 原始 Wi-Fi 与电池位图 |
| `baji_audio_codec.cc/.h` | ES8311、扩展 IO 功放使能及 BAJI 输入采样处理 |
| `power_manager.h` | 电源管理类、回调和共享 RTC 状态接口声明 |
| `power_manager.cc` | 运行期电量、USB 状态、关机及共享 RTC 标记实现 |
| `power_boot.cc` | 应用启动前的开机门控、纯充电流程及 `__wrap_app_main` |

电源入口通过 `--wrap=app_main` 在原生应用启动前判断纯充电模式，正常启动调用原生入口。原生文件仅保留 `main/Kconfig.projbuild` 和 `main/CMakeLists.txt` 的板型注册项，没有修改原生 C/C++ 实现。

## 编译

在已激活 ESP-IDF 5.5.4 的 PowerShell 中，从项目根目录执行通用构建流程：

```powershell
python scripts/build.py baji-touch-1.85lcd-ml307
```

此命令使用项目根配置。要使用独立编译目录，保留当前根目录 `sdkconfig`，执行：

```powershell
$bajiBuild = (Join-Path $PWD 'build/baji-touch-1.85lcd-ml307').Replace('\', '/')
New-Item -ItemType Directory -Force $bajiBuild | Out-Null
$bajiConfig = Get-Content 'main/boards/baji-touch-1.85lcd-ml307/config.json' -Raw | ConvertFrom-Json
$bajiConfig.builds[0].sdkconfig_append | Set-Content "$bajiBuild/board-config.defaults" -Encoding utf8
idf.py -B $bajiBuild -DIDF_TARGET=esp32s3 "-DSDKCONFIG=$bajiBuild/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;$bajiBuild/board-config.defaults" -DBOARD_NAME=baji-touch-1.85lcd-ml307 merge-bin
```

板级编译参数以 `config.json` 为唯一来源，不再另存重复的 `sdkconfig.defaults` 或专用构建脚本。临时配置生成在 build 目录。后续可使用相同命令，将最后的 `merge-bin` 改成 `build` 或 `reconfigure`。

产物位于 `build/baji-touch-1.85lcd-ml307/`：`xiaozhi.bin` 为应用，`generated_assets.bin` 为原生字体/表情/语音资源，`merged-binary.bin` 为包含引导程序、分区表、OTA 数据、应用和资源的合并镜像，烧录偏移 `0x0`。

2026-09-20 触控接入使用当前有效的根目录 `build/` 完成编译和合并镜像生成，应用大小 `0x2d6670`（2,975,344 字节），应用分区剩余 28%。本轮日志位于 `build/touch-build.log`、`build/touch-flash.log` 和 `build/touch-serial.log`，验证记录为 `build/touch-verification.json`。COM13 启动日志确认 I²C 读取成功及 `CST836U touch registered: I2C 0x15, INT GPIO17`；实际触点坐标和方向尚未手动验证。

2026-09-20 改用原生 `DualNetworkBoard` 后已通过 ESP-IDF 5.5.4 / ESP32-S3 编译及合并镜像生成，应用大小 `0x2d44a0`（2,966,688 字节），应用分区剩余约 28%。构建日志为同目录 `native-network-build.log`，`native-network-verification.json` 记录固件哈希与合并镜像各分区的核对结果。

已通过 COM13 烧录并立即采集启动串口，运行中的 ELF 哈希与本次构建一致。原生 `Ml307Board` 以 921600 波特率识别 ML307C，注册网络并取得 PDP IP；但原生 `Ml307Http` 访问小智 HTTPS OTA 服务仍出现 `Connection abnormal disconnection`，设备尚未完成服务器连接，不能将注册成功视为整机联网通过。烧录与串口日志分别为同目录 `native-network-flash.log`、`native-network-serial.log`。

本次实机检查覆盖启动与 4G 注册，没有完成 Wi-Fi/4G 按键切换、断电冷启动或语音对话验证。显示、音频与电源实现未作调整；已有显示验证结果位于 `build/baji-ui-review/native-smoke/`。
