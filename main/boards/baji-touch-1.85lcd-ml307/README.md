# BAJI Touch 1.85 LCD ML307

本板级来源于 `C:\Zero\BAJI工具\BAJI-V2.0.48` 中的 `main/boards/baji/BAJI`，目标为 ESP32-S3、360 × 360 ST77916 QSPI 液晶屏、ES8311 音频及 ML307 4G 模块。

板级组织参考本项目 Git 历史 d6e4a45 中的微雪 esp32-s3-touch-lcd-1.85：根目录放同名板级入口和配置，新增驱动及适配放 common。移植 BAJI 的硬件、按键、网络切换、电源及必要显示布局，复用小智原生字体、表情、主题和应用流程。服务器背景下载与轮播不在移植范围内。

虽然板级名称含有 `touch`，当前实现没有触摸控制器驱动、LVGL 触控输入设备或触摸手势功能。源 BAJI 板级可见实现亦未提供对应触控接入，不能将本次移植视为已支持触摸操作。

## 硬件连接

具体配置以本目录 `config.h` 为准。默认使用 16 MB Flash、QIO/80 MHz、80 MHz 八线 PSRAM 和 `partitions/v2/16m.csv` 分区表。

| 功能 | ESP32-S3 引脚或接口 | 说明 |
| --- | --- | --- |
| 共享 I²C SDA / SCL | GPIO6 / GPIO7 | ES8311 与 TCA9554，共用 I²C0 |
| TCA9554 地址 | `0x20` | 扩展引脚见下表 |
| LCD QSPI 时钟 / CS | GPIO12 / GPIO10 | SPI2_HOST，ST77916，RGB565 |
| LCD QSPI D0 / D1 / D2 / D3 | GPIO11 / GPIO13 / GPIO14 / GPIO9 | 360 × 360，无坐标偏移 |
| LCD 背光 | GPIO15 | LEDC PWM，非反相 |
| LCD 复位 | TCA9554 P7 | 不向原生 GPIO API 传入虚拟 GPIO |
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
| P7 | LCD 复位 |

## 按键与网络

首次启动默认选择 Wi-Fi，沿用源 BAJI 的默认行为；切换后的网络选择保存在 NVS `network/type` 中，下次启动恢复。4G 模式通过本地 ML307 实现驱动模块，尝试配置 eDRX；实际支持情况由模块固件和网络决定。

| 操作 | 行为 |
| --- | --- |
| BOOT 单击 | 唤醒显示并切换对话状态；Wi-Fi 启动阶段可进入配网；4G 正在连接时显示等待提示 |
| 音量加单击 | 音量增加 10，最高 100 |
| 音量减单击 | 音量减少 10，最低 0 |
| 音量减长按 2 秒 | 静音 |
| 音量加长按 3 秒 | 显示 Wi-Fi/4G 切换提示；5 秒内再次短按音量加确认 |

网络切换会关闭旧协议和旧网络连接，再建立目标网络。设备正在升级、激活等忙碌状态时会提示稍后切换。切换确认提示显示 2 秒，确认窗口仍为 5 秒。

## 显示与资源

与微雪 1.85 相同，使用小智组件提供的 `font_noto_sans_basic_16_4`、`font_material_symbols_16_4` 和 `noto-color-emoji_64`。运行字体和语音资源由原生构建流程生成，不再携带板级字体副本，也没有资源包字体替换脚本。

`common/baji_display.*` 继承 `SpiLcdDisplay`，先调用原生界面创建，再调整 BAJI 的中央网络/电池状态组、百分比、独立右侧静音标签及字幕上移。原生实现负责文字、表情、GIF、主题、时钟和低电提醒；纯充电页作为板级扩展保留。

Wi-Fi 和电池继续使用 BAJI 原始位图，4G、静音及系统提示使用原生 Material 图标。BAJI 源 Wi-Fi 位图只有底部三行包含可见像素，这一资源特点保持原样。本版本采用原生字体，因此字形、文字度量和字体图标遵循当前小智，先前采用 Puhui/Font Awesome 的像素对比报告不作为本版本验证依据。

## 开关机、充电与节能

- 正常电源按键开机门限为持续 3 秒。深睡唤醒时计入触发唤醒的这次按压，无需再按一次；不足门限则继续深睡。
- 运行时长按电源键约 2.6 秒触发关机，源配置为 13 次、每次 200 ms 的采样。关机流程提示、熄屏、释放电源保持，并在松键后进入深睡眠。
- USB 上电及符合条件的 USB 深睡唤醒进入纯充电界面，背光为 5%。此路径不启动小智应用、Wi-Fi 或 4G；持续按住电源键 3 秒后重启进入正常应用。
- 纯充电时拔掉 USB 会下电，包括 USB 恰好在 LCD 初始化期间被拔掉的情况。带 USB 关机保存 RTC 标记；之后拔 USB 再按键唤醒时，首次长按仍进入正常开机门控。
- 电池供电时遵循原生节能设置及应用空闲判定：约 60 秒进入节能显示，背光降至 1%；约 300 秒请求关机。按键及应用唤醒可恢复显示，接入 USB 时暂停此自动节能计时。
- 若关机任务分配失败，复用已创建的电源定时器检查松键后进入深睡，不在共享 ESP_TIMER 回调中等待按键或获取 Board/UI 锁。

## 板级结构

共 14 个文件。参照微雪 1.85，同名板级 `.cc` 集中 I2C、TCA9554、QSPI、ST77916 和按键初始化，构造函数按顺序调用 `Initialize*`。正常启动与纯充电使用同一个显示工厂，共用总线和扩展芯片。

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
    ├── baji_ml307.h
    ├── power_boot.cc
    ├── power_manager.cc
    └── power_manager.h
```

`config.h` 按微雪的紧凑分组和对齐格式整理；原 BAJI 的 79 个宏完整保留，宏名和值没有替换成微雪参数。原有虚拟 GPIO 宏也保留在配置中，但实际驱动通过扩展芯片位掩码访问对应引脚。

| common 文件 | 职责 |
| --- | --- |
| `board.cmake` | 收集 common 源文件，使用原生字体资源，设置启动包装 |
| `baji_display.cc/.h` | 继承原生显示，调整板级布局并提供充电页 |
| `baji_185_status_icons.c` | BAJI 原始 Wi-Fi 与电池位图 |
| `baji_audio_codec.cc/.h` | ES8311、扩展 IO 功放使能及 BAJI 输入采样处理 |
| `baji_ml307.h` | ML307 启停、取消及事件转发；单个头文件内实现 |
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

2026-09-19 电源文件按职责拆分后已通过 ESP-IDF 5.5.4 / ESP32-S3 编译及合并镜像生成，应用大小 `0x2d5860`（2,971,744 字节），应用分区剩余约 28%。原生构建脚本可识别此板型；原 BAJI 配置宏、ST77916 初始化表以及调整前后的显示、按键、电源和调制解调器逻辑已核对。

本轮电源文件拆分的日志为同目录 `power-split-build.log`；`verification.json` 记录最终固件、分区、资源、入口包装和配置核对结果。拆分前后的函数与接口核对记录在 `build/baji-power-split-review/split-review.json`。显示代码未改动，继续使用 `build/baji-ui-review/native-smoke/` 的验证结果；旧 Puhui 版本的对比报告不代表当前使用原生字体的界面。

尚未进行真机烧录或功能验证；电源保持、屏幕批次、麦克风、SIM 卡注册、网络切换和节能行为需要在实际硬件上确认。
