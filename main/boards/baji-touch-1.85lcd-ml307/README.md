# BAJI Touch 1.85 智能手表

ESP32-S3 / 360 × 360 ST77916 / ES8311 / CST836U / ML307。板级硬件移植自 `C:\Zero\BAJI工具\BAJI-V2.0.48`；手表交互参考 `C:\Users\YuYevi\Desktop\Demo\智能手表`。板级入口组织沿用微雪 1.85 的方式，新增实现集中在 `common`。

## 页面与操作

| 页面 | 已实现内容 |
| --- | --- |
| 待机 | 3 张壁纸轮播与横滑切图、可开关的44px时钟和日期；轮播指示点位于顶部56px，上滑打开菜单，底部无白条 |
| 主菜单 | BAJI 风格纯深色底、大时间 / 小日期、粉色“专属陪伴”主卡片，下方应援灯 / 日历 / 闹钟 / 设置四张错落小卡片 |
| 控制中心 | 顶部下拉、上滑收起；WLAN / 移动数据 / 静音 / 息屏 / 省电 / 手电筒六张功能色图标卡，亮度和媒体音量滑杆 |
| AI 对话 | 用户提供的待机/说话双动画、顶部时间/会话状态在同一位置切换、半透明灰色字幕框、底部麦克风，接入原生小智会话 |
| 日历与日程 | 横向日期条、逐日事件标记与居中日程卡片；显示真实日程，使用 AI / MCP 新增、修改和删除 |
| 闹钟 | 最多 8 个，32px 时间、独立开关、左滑删除；滚轮编辑时分、星期按钮编辑重复规则，新建默认工作日 |
| 倒计时 | 顶部标签切换，分钟 / 秒滚轮、静态单进度环、暂停 / 继续 / 重置；1 小时以上使用时:分:秒，支持服务层的 24 小时上限 |
| 设置 | WLAN 列表及密码键盘、移动数据、显示设置、语言、重启检查更新、关于、重置手表数据 |
| 显示设置 | 自动熄屏时间、待机时钟与日期开关、常亮显示；返回上一级设置 |
| 应援灯 | 圆形纯色预览、原型选色和全屏颜色显示；只使用 LCD，没有虚构外圈 RGB 灯 |

息屏后触摸不亮屏，也不操作黑屏下的页面；短按实体电源键亮屏，第一次短按只负责唤醒。其他页面右滑返回；滚动列表与时间滚轮使用各自的滑动操作。

壁纸按用户提供的顺序为：日落户外、雨夜窗边、日光沙发，对应 `watch_wallpaper_0/1/2.rgb565`。三张源图均为360×360，保持原构图，不缩放、不裁剪；已覆盖原来的三张壁纸，生产资源目录不保留旧图或原图副本。

待机壁纸按原生360×360、RGB565像素显示，不再在加载时混入上下深色渐变。时间和日期只在各自文字范围内使用半透明黑底；关闭时钟与日期后也不会留下阴影。三张生产壁纸资源保持不变，不增加常驻图片或资源文件。

AI 背景使用用户提供的 `standby.mp4` 和 `speaking.mp4` 对应的原始 MJPEG，来源为 `C:\Users\YuYevi\Documents\吧唧\Work\Project\BAJI\main\display\SmartWatch_UI\images\mjpeg`。`C:\Users\YuYevi\Documents\WorkBuddy\2026-09-23-15-46-16\output\report.json` 记录了这两份原素材转为 MP4 的来源关系。本次直接将原始 JPEG 帧逐字节装包，避免 MJPEG→H.264→JPEG 的额外有损转换，不再烘入暗角、底部阴影，也不在页面上叠加大面积渐变。两段各4秒、40帧、360×360、10fps，无音轨；保持原构图和动作，不加入额外呼吸缩放。原生状态为 `kDeviceStateSpeaking` 时循环说话动画，空闲、连接、聆听及等待回复时循环待机动画。重复刷新同一状态不重置动画，切换时从对应片段首帧开始；中断与回复结束跟随原生会话状态变化。字体、图标与待机壁纸不变。

### 图片偏暗与画质修复（2026-09-28）

根因是显示和资源处理主动损失了原始画面，而非分辨率配置被缩小：

- 旧 `PrepareWallpaper()` 将壁纸顶部155行、底部108行与 `#09090f` 渐变混合，边缘覆盖率达到100%；即使关闭时钟也会执行。按圆屏可见像素重建，三张壁纸的平均亮度分别损失约28%、21%、27%。现加载缓存与生产 RGB565 像素一致。
- 旧生成器对每帧乘上径向暗角和底部65%黑色渐变，再编码为 JPEG q4。阴影已永久写入 v2～v4 资源，单改透明度或只烧固件不能恢复。现生产包使用80帧原始 JPEG，无二次编码；非原生 MJPEG 输入才使用单次 FFmpeg q2/4:2:0 转换，保留原来解码器支持的格式。
- 原始 RGB565、视频和屏幕均为360×360，图像控件不缩放。背光仍尊重用户设置（省电模式最高35%），未通过抬高背光或锐化掩盖资源问题。源素材分辨率与 LCD RGB565 色深仍是实际显示上限。

资源包升级为 v5，沿用 v4 的双片段布局，明确表示无预烘焙阴影。固件可读取 v1～v5；遇到 v2～v4 会在日志提示更新 assets。更新时必须同时写入本次应用和 `generated_assets.bin`，否则旧视频仍然带阴影；旧固件也无法读取 v5。升级本地数据无需清除 NVS。

为保证亮图上的可读性，待机时间、日期使用各自标签范围内的黑色半透明底，AI 返回键和字幕也只在控件范围内提供对比度。按用户确认，AI 对话页的小时间／会话状态恢复透明文字，不使用黑色底框。没有把大面积图片重新盖暗。

同日按用户确认移除 AI 时间／状态黑底后，已重新编译并仅将应用写入 COM6，通过写后哈希校验。应用为3,201,760字节，SHA-256 `ee7dd6206db8a1a08a6f960e2a49e491f8e4389d42a9a48b9ed745499f478e60`；图片资源未改动。25秒重启日志确认新的 ELF 标识 `7a3fb839b`、资源加载、Wi-Fi和服务连接及进入空闲状态正常。记录为 `build/display_quality_verify/chat-header-flash.log` 和 `chat-header-boot.log`；下方首次画质修复的固件大小及校验记录属于此前构建。

2026-09-28 按用户要求回退到“现在表盘UI使用的字体有哪些”对话时的界面：保留上述图片画质修复和 AI 顶部透明文字；撤销之后的原生字体替换及 AI 状态中英文映射。该时点 AI 小时间使用表盘 Segoe UI 14px，中文状态、用户转写及 AI 回复使用表盘 Noto Sans SC Regular 14px。重新编译的应用与该时点记录完全一致：3,201,760字节，SHA-256 `EE7DD6206DB8A1A08A6F960E2A49E491F8E4389D42A9A48B9ED745499F478E60`。仅将应用写回 COM6，写后哈希校验通过，图片资源未改动。25秒启动日志确认 ELF `7a3fb839b`、资源包70字体/138图标/80帧/10fps、Wi-Fi、`ota_0`、服务连接及进入空闲状态，无崩溃。记录为 `build/display_quality_verify/pre-font-rollback-build.log`、`pre-font-rollback-flash.log` 和 `pre-font-rollback-boot.log`；源码及固件备份在同目录 `pre-font-rollback-backup`。未目视检查实屏。

生成器回归测试：`python main/boards/baji-touch-1.85lcd-ml307/common/tools/test_watch_resources.py -v`，设置 `FFMPEG` 环境变量可运行实际转码测试。覆盖原 MJPEG 字节及顺序保留、明亮帧边缘亮度、尺寸与帧率约束、旧 v4→v5 字体图标保持及损坏帧表拒绝。修复前后背景对比、资源 SHA 和构建记录位于 `build/display_quality_verify`；这些背景图为软件重建，不是实机照片。

2026-09-28 已经用户确认烧录至 COM6。设备 Flash 的应用（3,201,776字节）、assets（6,794,384字节）和分区表均通过 esptool 直接哈希比对；assets 内 v5 包及三张壁纸与生产资源逐字节一致。OTA 状态区已由启动流程从初始空白改为 `ota_seq=1, ota_state=2 (VALID)`，因此不能要求它在启动后仍与 `ota_data_initial.bin` 相同。重启采集25秒，确认 ESP-IDF 5.5.4、新资源70字体/138图标/80帧/10fps、背光100%、运行分区 `ota_0`、Wi-Fi和服务连接成功，无崩溃或资源加载失败。日志仍有4G模块串口波特率检测超时；本轮使用Wi-Fi，未验证4G。实屏亮度、色彩和真实AI会话播放流畅度仍需现场验收。设备验证记录为 `build/display_quality_verify/device-verification.json`，启动日志为同目录 `device-boot.log`。

待机轮播指示点沿用 BAJI 的顶部居中位置：距顶56px，活动项16×6、其余6×6、间距6px。活动项纯白，其他项白色不透明度160/255，外侧增加1px半透明黑色轮廓，改善浅色照片上的辨识度；不增加底板或大范围辉光。

顶部状态栏参考 BAJI 的轻薄黑色半透明底和紧凑排列，独立使用 LVGL 绘制。距顶16px、高26px，网络图标在16px槽位按实际像素绘制：Wi-Fi为两道圆弧加圆点，移动数据为三根递增圆角条；两种网络离线时均变暗并显示斜线，当前数据不提供信号强度等级。百分比使用原生12px/600并固定宽度，避免数值位数变化时整组抖动。电池内部为一整块连续填充，宽度随实际电量按比例变化，没有分格和分隔线；0%为空，非零电量至少显示1px，未知电量显示空槽和 `--%`。低电量沿用红/黄色，充电为绿色并显示闪电。圆弧视觉参考为 [Tabler Wi-Fi 2](https://tabler.io/icons?icon=wifi-2)，网络和电池均按圆屏像素尺寸独立实现，没有新增图标库或图片资源。

“设置 → 显示设置 → 时钟与日期”一起控制待机页的时间和日期，保存到已有的 `show_clock` 字段，重启后保留。菜单和 AI 页的时间显示独立。待机底部静态白条及上滑过程中的进度条均已删除，上滑菜单、横滑壁纸继续可用。

菜单独立实现于 `common/ui/pages/menu_page.cc`，视觉参考 `C:\Users\YuYevi\Documents\吧唧\Work\Project\BAJI` 的 Home 页面。198px 宽布局使用 198×66 主卡与 118/74px 交错小卡，背景为纯 `#09090f`，小卡文字左对齐，渐变只用于图标底座。保留当前五个功能入口，未引入参考项目的全局 UI、导航或资源实现。按压只缩放卡片底板，文字和图标保持原生尺寸；移出、取消、息屏和打开覆盖层时复位，点击在当前触摸事件处理结束后切页。菜单不再生成原先 360×360 柔光背景纹理。

其余页面沿用同一套视觉规范：`#09090f` 平面底色、柔和功能色、16px 页标题、14px 主要文字和12px辅助文字；滚动列表主要宽240px，底部动作收进圆屏安全区。待机、对话、日期、闹钟、时间编辑、设置、WLAN 键盘、应援灯、控制中心和确认/提示/提醒/电源弹层均已重排。普通按钮只改变底色，菜单和快捷按钮只缩背景底板；文字与图标不缩放。移除大范围光晕、倒计时水波和待机中央手势圆环。普通按钮独立识别拖动取消，覆盖层内拖动松手不会误确认；闹钟从开关区域起手左滑也会正常吸附。

| 按键 | 行为 |
| --- | --- |
| 电源键单击 | 息屏时唤醒；亮屏时返回或关闭电源菜单 |
| 电源键双击 | 亮屏时在 320 ms 内连续两次短按，打开 AI 对话 |
| 电源键长按 3 秒 | 显示原型中的关机 / 重启 / 息屏菜单；松手不再触发单击 |
| 电源键持续长按 8 秒 | 强制关机；正常关机状态开机仍需长按约 3 秒 |
| BOOT 单击 / 双击 | 仅亮屏时返回 / 打开 AI；Wi-Fi 启动阶段沿用进入配网的行为 |
| 音量加 / 减单击 | 仅亮屏时调整媒体音量，每次 10 |
| 音量减长按 2 秒 | 仅亮屏时媒体静音 |
| 音量加长按 3 秒 | 仅亮屏时提示切换 Wi-Fi / 4G，5 秒内再次短按音量加确认 |

网络切换会保存选择并重启，复用原生 `DualNetworkBoard`、`Ml307Board` 和 `78/esp-ml307`。未保存网络选择时，板级构造函数最后一个参数 `0` 表示 Wi-Fi，改为 `1` 表示 4G；已保存的 `network/type` 优先。没有板级自定义 ML307 驱动。

再次点击已开启的 WLAN / 移动数据开关可关闭全部网络，确认后重启生效。离线启动不运行原生联网任务，关闭 ML307 电源，保留本地手表功能；恢复任一种网络同样重启。开关状态存于 `watch/net_off`，不会删除原 Wi-Fi 凭证或更改默认网络类型。

WLAN 设置页使用实际连接状态和 SSID。当前已连接时只列出实际热点，附近网络刷新暂不可用：实机发现直接扫描会与原生扫描事件处理争用结果，并触发原生重新连接，因此已移除这条冲突路径。保持连接的扫描扩展尚待用户批准，没有擅自修改原生组件，也不自动断开现有连接。配网模式仍复用原生热点的扫描缓存、连接验证与保存接口，通过本机 Wi-Fi HTTP 通道访问。连接成功以实际 SSID / IP 为准，失败可重试；原生配网的限制为 SSID 最长 31 字节、密码最长 63 字节。原型演示 SSID、示例闹钟和日程不会写入设备。重置手表数据清除手表设置、闹钟和日程，保留 Wi-Fi 配置。

## 时间、提醒与存储

- 使用系统时间，联网按原生服务器流程校时。原生时间戳已经加过时区，手表不再次加 8 小时。
- 无外置 RTC，断电后必须重新校时；未校时不会把错误日期当成有效闹钟时间。不支持关机响铃，纯充电界面不运行提醒服务。
- 最多保存 8 个闹钟、16 条日程；写入独立 `watch` NVS 命名空间，用双副本和校验保护保存。写入失败会返回错误，不擦除全局 NVS。
- 重复闹钟按星期触发，时间回拨或重启不重复同一次提醒；错过不超过 5 分钟可补响，更早的提醒不突然补响。
- 倒计时与“稍后 5 分钟”使用单调计时，不受校时影响，但不跨断电保存。
- 勿扰让日程静默，闹钟与倒计时仍遵循独立提醒音量。提醒最多连续响 60 秒，然后允许正常息屏，未确认的卡片保留到下次亮屏。
- 提醒复用原生音频服务和提示音，不增加 I²S 播放管线。临时提醒音量不覆盖保存的媒体音量。
- 当前 `BAJI_WATCH_LOCAL_AUDIO=0` 保持原生源码不变：对话期间先显示提醒，原生会话空闲后再响铃。可靠抢占 AI 音频需要另行批准的 `Application` 本地音频接口补丁；不能只把宏改为 1。

注册的 MCP 工具：`self.watch.get_status`、`save_alarm`、`delete_alarm`、`save_event`、`delete_event`、`countdown`（后五项均以 `self.watch.` 为前缀）。AI 调用实际保存成功后才可报告已设置，能否被服务器模型选用还取决于当前服务器配置。

## 硬件与节能

| 功能 | 接口 |
| --- | --- |
| 共享 I²C SDA / SCL | GPIO6 / GPIO7，I²C0 |
| CST836U | 地址 0x15，INT GPIO17，与 LCD 共用 TCA9554 P7 复位 |
| TCA9554 | 地址 0x20；P0 模组 PWRON、P1 模组复位、P2 / P3 音量按键、P5 功放、P6 指示灯、P7 LCD / 触控复位 |
| ST77916 QSPI | CLK 12、CS 10、D0 11、D1 13、D2 14、D3 9，背光 GPIO15 |
| ES8311 I²S | MCLK 45、BCLK 41、WS 46、DOUT 42、DIN 40 |
| ML307 UART | ESP TX 47、RX 48、DTR 38 |
| 电源 / 电池 | 保持 GPIO1、电源键 GPIO2、USB 检测 GPIO5、电池 ADC GPIO4、充电 STAT GPIO16 |

触控复用已有 CST816S 单点协议组件和 `lvgl_port_add_touch()`，没有新建触控驱动。LCD 初始化前已复位，触控驱动不再次拉低共用复位。触控方向跟随 `config.h` 的显示镜像设置。

按实物要求移除六轴传感器、抬腕唤醒和姿态校准，不再探测或轮询 IMU。手动唤醒只使用实体电源键；闹钟、倒计时与日程到点仍可亮屏提醒。旧存储中的抬腕标志被忽略，不清除已有闹钟或日程。

默认闲置 15 秒关闭背光、暂停壁纸轮播与语音唤醒。“设置 → 显示设置 → 自动熄屏”可选15秒、30秒、1分钟、2分钟、5分钟或永不，保存到原有手表设置并在重启后恢复；同页的常亮与“永不”使用同一字段。省电模式将亮度限制到35%，不覆盖所选熄屏时间。手电筒暂时使用100%亮度，退出恢复原设置，不把临时亮度写入NVS。已移除原先闲置300秒自动关机，插USB时也可息屏。AI对话、升级及新提醒期间保持亮屏；对话时主动选择电源菜单的息屏，会先结束对话再关闭背光。

1000 mAh 电池的 24 小时目标要求平均电池电流不超过约 41.7 mA，考虑容量余量还需更低。此版本没有完成电池侧电流和全天续航测试，不能以息屏功能替代续航验收。Wi-Fi 与 4G 必须分别测量；当前 ML307 的原生节能能力仍有限。

关机纯充电页只显示白色电池闪电图标和充电状态，不显示百分比；确认充满后显示“充电完成”。图标保留 BAJI 的 30px、4bit 抗锯齿字形，启动页和纯充电页均读取手表已保存的中英文设置。USB 拔出下电、长按电源和 RTC 启动标记沿用板级 `power_boot` / `power_manager` 实现。

未接 USB 时，原始电池电压连续不高于 3.4V 达 15 秒，或不高于 3.3V 达 3 秒，会立即熄灭背光、关闭功放并下电；低压区每秒采样，电量估算仍约每 5 秒更新。再次开机先测电压，低于约 3.5V 不亮屏；低压关机后，插电状态须达到 3.75V 并稳定 15 秒才允许长按开机，拔线开机则须达到 3.55V，避免充电抬压后反复启动和屏闪。阈值基于端电压，不能代替电池保护板；实际低压触发点和负载压降仍需实物放电验证。

## 电量估算与验证

原算法将未经校准的 ADC 原始值 1970..2430 等间距换算为 0..100%，每分钟只读取一次，并在插拔 USB 时混入不同供电状态的历史样本。因此充电导致的端电压抬升会被误认为容量增加，锂电池电压平台也会造成各区间掉电速度异常。GPIO5 只表示 USB 输入存在，不能表示实际充电或充满。

原理图中 R8/R13 均为 200kΩ、C7 为 100nF，GPIO4 读取 VBAT 的一半；实现使用 ESP-IDF ADC 曲线校准后乘以 2，16 次采样去除两端各 4 次，取中间 8 次均值，每 5 秒采样并滤波。USB 和 GPIO16 的充电状态去抖，插拔后稳定 30 秒再学习电压阶跃补偿。容量按真实经过时间渐进校正，放电时不会因瞬时电压回升而涨电，持续低电压会加快下降与告警；采样失效超过 60 秒显示未知，不使用虚构的默认百分比。

估算曲线参考乐鑫 `adc_battery_estimation` 的 model 2，端点按 703048 资料设为 3.0V / 4.2V。这是通用的电压估算，不是该电芯的实测容量曲线。仍在恒压充电时最高显示 99%。充电阶段的正负阶跃补偿只随电压实际向 4.2V 推进而减小，不再单凭时间衰减；若插线稳定电压已达到 4180mV，则保留补偿直到 STAT 确认满电，避免在缺少可用电压跨度时将短时重插误算为新增容量。停止充电时取消正向扣压；USB 和 STAT 阶段变化均处理端电压阶跃，避免将正常停充回落重复换算为电量损失。

满电拔线后的放电阶段使用容量锚点衔接：切换稳定 30 秒后，将切换前可信容量 `anchor_soc` 与负载电压对应的曲线值 `anchor_curve` 配对。用反函数恢复负载锚点电压 A 和容量对应的参考电压 O，补偿为 `clamp(O-A, -200, 200)` mV；后续用 `VoltagePercent(当前电压 + 补偿 × clamp((当前电压-3400)/(A-3400), 0, 1))` 计算目标，限制在 0 到 `anchor_soc`。这替代了原来的百分比比率缩放，避免将满电端点移到约 4.05V 的较陡曲线段；补偿随电压下降连续减小，到 3400mV 归零，不随时间自行衰减。A 或 O 不高于 3400mV 时走低电量保守分支，避免极小分母或负斜率。满电切换保留 100%；普通切换取模型目标与精确显示值的较小值，避免显示滞后或反复插拔抬高容量。供电变化最多只解释 200mV 的压降，超出部分仍扣减锚点容量，不能把异常大压降锁成满电。低压告警与归零仍有效；同一放电阶段重启原样恢复锚点，不把关机期间的真实压降重新吸收入供电补偿。

整数显示在内部容量不低于 10% 时向上取整（0.001% 容差），让 100% 与后续档位一样覆盖完整约 1% 区间；之前四舍五入使 100% 只覆盖 99.5～100 的半个区间。低于 10% 仍按原来的保守四舍五入，不延迟零电量显示；USB 未确认满电仍最高 99%。显示取整不写回容量模型，不按固定时间掉电，也不定时锁住 100%。沿用同一锚点格式的检查点，旧精确容量不会在放电中因新模型上涨，但整数显示可能因新取整规则比旧版高 1%。

满电判定结合充电历史与 STAT：近期充电电压至少 4140mV 持续30秒，或者在近恒压时观察到明确的充电→停充边沿，然后停充稳定120秒、电压不低于 `max(4000mV, 充电峰值-180mV)`，才确认充电结束。启动或新插线时已停充、未观察到主动充电的情况，至少4100mV稳定300秒后可推断满电，并在日志标记 `inferred=1`。这些电压门限只用于验证 STAT 是否合理，不单凭4.2V认满。

ETA6098 的充电电压范围为4.16～4.24V，停充后要下降160mV才自动重充。因此已确认满电后，在同一次USB连接中保留满电状态，允许电压松弛和自动补充电；只有拔线，或者电压低于3980mV持续120秒才退出。若低到3400mV则缩短为15秒，同时记录供电检查提示，不因USB存在永久固定100%。顶部图标继续在USB接入期间显示绿色闪电，包括满电。正常运行与纯充电页面共用监测模块。

RTC/NVS 检查点版本为 `0x42415404`，以 0.001% 的定点精度保存显示容量、模型目标及锚点的容量和曲线值，同时保存电压采样时的 USB、实际充电阶段与已确认满电状态，并保存插线显示保护及其电压基线；版本 `0x42415403` 会迁移并保留原模型和锚点，新字段从未启用开始，更早版本不恢复。软件重启还保留近期恒压证据，冷启动不沿用无法确定时间的未完成证据。充满状态变化、显示保护变化、每 10 分钟及正常关机时保存，并在 USB 边沿 35 秒后补存已稳定的锚点；恢复满电不再被普通放电曲线误判为无效。

ETA6098 官方手册确认 STAT 充电时拉低、结束时高阻，但没有给出完整的故障状态表。因此上述满电条件属于软件推断，无法识别所有高电压停充故障。芯片典型终止电流为 130mA，而电池组资料的容量测试条件要求恒压至 20mA；芯片结束充电不等于达到规格测试下的全部 1000mAh，不能通过修改显示算法补足这项硬件差异。

每分钟的 `BajiBattery` 日志包含 `VBAT`、补偿 `correction`、电压估算目标 `target`、显示电量 `SOC`、`valid`、`USB`、实际充电 `active_charge`、`full`、异常低压 `supply_low` 以及 `anchor_soc` / `anchor_curve`；容量值输出 3 位小数。`gpio_usb` / `gpio_stat` 是引脚原始电平，STAT 低电平对应 `active_charge=1`。满电状态转换时额外输出 `Charge cycle` 日志。模块另用约 1.5KiB RAM 循环保存本次放电最近 64 条每分钟或 USB 边沿轨迹；新一次拔线才清空旧轨迹，插着 USB 的每分钟常规日志不占用轨迹槽位。重新接入 USB 10 秒后输出 `Offline t=... VBAT=... SOC=... target=...`，此后每 60 秒重放一次，重试至重接后 610 秒，避免串口监视晚打开而漏收；回放后仍保留数据，不增加 Flash 写入，断电或重启不保留此 RAM 轨迹。64 位毫秒时间戳拆成十进制字符串再以 `%s` 输出，兼容 newlib nano 且不在运行 49 天后溢出。上板先用万用表对比电池端电压与 VBAT，再检查插拔、停充、回充及重启。纯充电模式默认只输出错误日志。

当前硬件没有电流采样或专用电量计，电池资料也没有 OCV/SOC 实测曲线，无法仅靠电压证明真实容量精度或保证每 1% 对应相同使用时长。30 秒稳定窗口也不能完全分离后续极化恢复与真实消耗；容量锚点修复的是已复现的模型假掉电，不能代替电芯标定。精度验收仍需完整充放电记录：分别记录熄屏、亮屏、Wi-Fi/4G 和对话负载，测量电池侧电流并积分容量，在多个剩余容量点静置后测电压，再替换 `BatteryEstimator::VoltagePercent()` 的曲线。只有曲线标定及实机测试完成后，才能报告容量误差或续航结论。

2026-09-26 首轮验证记录于 `build_boot_verify/battery_verification.json`，该轮仅监视180秒，未覆盖长时间充电结束后的回落。后续COM6现场捕获到 `VBAT=4148mV correction=117mV target=83.1% SOC=86% USB=1 STAT=0 full=0`，一分钟后电压4146mV、显示85%。这直接证实旧补偿在停止充电后仍重复扣压，且4150mV固定门限阻止判满；主机模型亦复现4200mV/99%停止充电回落4140mV后变89%。上一轮针对该充电周期修正，旧短时验证不能作为长时充电通过的证据。万用表对比及完整容量标定仍需实测。

上一轮主机回归已通过，包含99→89复现、正常回充、短时顶充、已满电重插、阶段重启、无效采样及真实USB低压；用 `g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 -static` 编译测试。ESP-IDF固件编译通过，应用大小3,191,328字节，SHA-256为 `8ee21cfb30ad9f5bb0fb7de1d53789977f45c6c5e768cdc4aecd351d82ddbcf0`。COM6烧录并校验后立即连续监视620秒：317秒确认已停充满电，361秒显示100%，此后至601秒的5次读数均为100%、补偿0、USB存在，电压4141～4144mV，无电池采样错误或崩溃。该轮实机仅覆盖插着 USB 的停充保持状态，没有覆盖满电拔线放电；自动回充切换与完整充电周期由主机模型覆盖，尚未完成整轮实物放电再充电。记录位于 `build_boot_verify/battery_charge_cycle_verification.json`、`battery_charge_cycle_serial.log`；ML307波特率探测超时仍单独列为已有网络问题。

本轮满电拔线问题已在旧模型精确复现：已确认满电的 4142mV 对应普通曲线仅 95.915%，拔线取消满电状态后目标出现断层，随后负电压补偿又按 300 秒时间常数自行衰减，180 秒显示跟随将错误目标表现为连续掉电。即使拔线后电压固定为 4050mV、没有继续下降，520 秒后仍显示 90.14%，最终目标为 85.61%，与报告的约 8 分 40 秒到 90% 高度吻合。新实现取消随时间消失的补偿，并通过上述容量锚点将满电状态连续衔接到放电曲线。

本轮完整主机回归通过：满电从 4142mV 拔线后分别保持 4142/4100/4050mV 一小时不再自行掉电，覆盖 520 秒回归、0/5/25/60 秒阶段重启、冷启动真实压降保留、重复插拔不增容、充电恒压不假掉电、异常大压降不锁满，以及持续真实放电至 0%；此前充电、噪声和无效采样等回归仍通过。首次拔线测试固件编译成功，应用大小为 `0x30bd50`（3,194,192）字节，应用分区剩余约 23%，SHA-256 为 `525046421CCF88538CA491E31835CC4C8C39920528F20B11A6879033935C1A20`，编译日志为 `build_boot_verify/battery_unplug_build.log`。这是该次测试版本的历史标识，后续日志修正后的固件校验值另行记录。

首次测试固件通过 COM6 烧录及写入校验，并立即监视：启动后 158 秒观察到充电转停充，278 秒由实际充电历史确认满电（`inferred=0`），301 秒显示 100%（内部 99.868%，VBAT=4146mV，补偿为 0）。未观察到电池采样错误或固件崩溃。延长监视、关闭重开旧串口连接期间出现两次重启，满电检查点成功恢复；监视脚本随后采用 ESP-IDF 官方无复位打开顺序及 Windows `usbser.sys` 修补，重开后运行时间连续、容量保持 100%。这些读数属于插着 USB 的满电保持，不能作为拔线后不掉电的证据。

第一次实物拔线测试由用户确认全程亮屏停留在待机页、无操作，约 10 分钟后重接 COM6。该版离线轨迹的 `%lld` 与已启用的 newlib nano 格式化器不兼容，显示为 `t=ldms` 并导致后续参数错位；SOC/target 的 double 数据恰好以两个 32 位整数保留在后面的字段中。按 IEEE754 小端重组后，得到以下逐条数据；每一组均恢复为原先保存的 0.001% 精度，最后一组与重插后正常日志的 `95.016% / 93.848%` 完全吻合。恢复脚本及明细为 `build_boot_verify/recover_battery_trace.py`、`battery_unplug_trace_recovered.json`。启动时的 `-1% / -1%` 无效记录不列入放电样本。

| 轨迹顺序（不是精确分钟） | SOC | 模型 target |
| --- | --- | --- |
| 拔线基准 | 100.000% | 100.000% |
| 1 | 99.970% | 99.698% |
| 2 | 99.661% | 98.172% |
| 3 | 99.038% | 97.385% |
| 4 | 98.472% | 96.231% |
| 5 | 97.759% | 96.020% |
| 6 | 97.244% | 95.206% |
| 7 | 96.707% | 95.042% |
| 8 | 96.172% | 94.705% |
| 9 | 95.648% | 93.962% |
| 10 | 95.224% | 93.960% |
| 重插边沿 | 95.016% | 93.848% |

这次确认的是约 10 分钟显示由 100% 逐渐降到约 95%，不是全程保持 100%。时间戳和电压的原始位已被错误浮点格式化舍弃，无法从该日志精确恢复；日志行外的时间是回放时刻，也不能冒充各条采样时刻。按正常日志中独立记录的锚点 `100 / 91.204` 反算，对应的滤波电压约从 4096mV 降至 4050mV，但这只是模型反算，不是独立恢复的 ADC 实测值。用户旧报告为 520 秒掉到 90%，本次约 600 秒掉到 95.016%；测试时长和旧负载条件不完全一致，不能据此给出容量精度改善比例。当前电压法仍不能分离极化松弛和实际消耗，完整容量标定尚未完成。

恢复数据中重插边沿的模型目标与后续正常日志相同，没有观察到重插瞬间额外的目标下降。`SetPower()` 在取切换前目标时，`usb_` 仍是拔线状态，`TargetPercent()` 使用原放电锚点。

第二次测试使用修正为 32 位 `%u` 时间戳的固件，历史应用 SHA-256 为 `76D26B47E79D7B1B0BCF7608531FB64FD53EC8FDA384179B9C4A3E343903688F`，大小 3,194,224 字节，设备 ELF 标识为 `a53a3ba7e`。用户观察到这次拔线约 10 分钟的屏幕终值为 **95%**。该次 1200 秒串口监视已在重插前到时退出，重开时仅收到重插事件的残留片段，没有捕获到正常 `Offline` 回放。重插运行时间为 2137476ms，首次完整机读电量日志为 2222706ms，已经相隔 85.23 秒，此时 `SOC=94.136% target=92.881% VBAT=4194mV USB=1 active_charge=1`。94.136% 属于再次充电期间，与用户观察的拔线终点 95% 是不同时刻的数据，不能混为同一个值；也无法补回第二次缺失的逐分钟轨迹。记录在 `build_boot_verify/battery_second_run_capture_audit.json`。

最终轨迹修正版已通过当前源码的电量主机回归、ESP-IDF 编译及 COM6 烧录校验。应用大小 3,194,464 字节，分区剩余约 23%，SHA-256 为 `65864f59b7603a2f8698b8b748616a7a698acc92f33cf6b353568ea10f33c3ad`，设备 ELF 标识为 `7088119ff`。编译日志为 `build_boot_verify/battery_trace_replay_build.log`；烧录后立即监视启动。先前重复增加的重插分支已撤回，最终估算逻辑与两次测试所用逻辑等价；本次新增行为是保留与重试回放记录。没有要求第三次 10 分钟测试，重试回放尚未在实物重新拔插后验收。

最终固件烧录后 100 秒串口监视已完成：61 秒读数为 `VBAT=4147mV SOC=94.933% target=96.243% USB=1 active_charge=0 full=0`，正在恢复停充后的容量及满电确认；未观察到电池采样错误或崩溃，已有 ML307 波特率探测超时仍存在。监视结束已释放 COM6。

完整历史验证记录和串口原文位于 `build_boot_verify/battery_unplug_verification.json`、`battery_device_serial.log`。两次约 10 分钟测试的显示终点均为 95% 左右，但仍无法将电压极化恢复和真实容量消耗分离；完整容量精度与续航验收仍待电池电流测量及电芯曲线标定。

2026-09-26 高电量显示复查：原比率模型把负载锚点约 4096mV 后降至 4050mV 映射为约 93.9%；采用上述电压域补偿后，同样输入的目标约 96.3%。这只是模型对比，电压仍来源于旧日志反算，不能视为新固件实测容量。除了负载/极化回落未完全分离，原四舍五入还导致满电首格先天只有后续档位一半宽；本轮同时修正这两项。没有加入额外电压保留量或固定 100% 保持时间。

本轮最终主机回归通过（`g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 -static`），包含合成高段压降回放、完整百分比量化、正负偏移及低电锚点扫描、恒压保持、阶段重启、反复插拔、3400mV 安全边界和已有充电周期用例。合成回放以人为设定的 60 秒间隔输入旧轨迹反算电压，不能冒充恢复的真实采样时间。ESP-IDF 编译成功，应用大小 3,194,880 字节，分区剩余约 23%，SHA-256 为 `afc3155658baf12217a67e4c0be57de9e0e9e88231cac74f0bc0b53dceaa6cbb`。构建日志为 `build_boot_verify/battery_voltage_anchor_build.log`；本轮只修改板级估算器、既有回归测试和本文档。完整放电、80%～70% 区间与实际耗电仍需实测，不能保证每 1% 的使用时长完全相同。

2026-09-26 插线退格修复：插入 USB 后，旧实现会让精确显示值继续向较低的电压模型目标收敛，因而出现 93%→92% 或 97%→96%；即使插线后电压稳定也能复现，不能由此判断真实净放电。现实现为插线前显示建立临时显示下限，容量模型仍独立保留较低目标；稳定充电时不再补扣这段插线前的显示差额。保护在拔线、持续明显掉压、临界低压或模型追上时解除，并将“保护仍有效及其电压基线”随检查点保存，避免掉压后重启再次冻结高显示。参考电压在插线稳定后建立，低于参考超过 25mV 持续 120 秒会解除保护，ADC 缺测时间不计入；STAT 停充/恢复不会重新建立已经解除的保护。主机回归新增覆盖 93/97% 插线、STAT 切换、短暂/持续 50mV 掉压、ADC 中断、重启和快速插拔，最终套件已实际运行通过。

同日新固件实物复测：用户在拔线前确认屏幕已到 100%，拔线后保持待机亮屏、无操作；重新插线前目视为 97%。COM6 离线轨迹记录拔线至重插 629.020 秒（10 分 29 秒），滤波 VBAT 从 4113mV 降至 4051mV，内部 SOC 从 99.987% 降至 96.477%，按固件取整对应 100%→97%，与目视终值一致。100% 在拔线后 3 分 29 秒的分钟采样仍可见，4 分 29 秒采样已为 99%；其精确切换时刻未记录。末端重插边沿虽然标记 `USB=1`，但 `voltage_usb=0`，保留的仍是插线前的放电测量；重插后的 4196mV/96.317% 属于充电阶段，不参与放电结论。轨迹中电压与 SOC 连续下降、`valid=1`，无重启或明显采样突跳。相比此前约 10 分钟屏显 100%→95%，这次高电量显示下降较缓，但测试条件未严格受控；这一段轨迹不能证明真实容量、各档续航均匀性或 24 小时目标，仍需完整放电及电池电流测量。原始记录位于 `build_boot_verify/battery_device_serial.log` 的 `2026-09-26 20:41:46 COM6` 段。

重插后继续监视 180 秒，完整收到三次相同的离线回放，串口正常退出并释放 COM6。重新充电期间 `active_charge=1`、`valid=1`、`supply_low=0`，VBAT 为 4194～4197mV；这是旧固件复测中内部 SOC 从重插瞬间 96.477% 收敛至 95.945% 的历史记录，不能代表本轮修复后的行为。修复后，正常插线会保留插线前的可见百分比，容量模型仍独立收敛；只有拔线、持续真实掉压或模型追上时才解除保护。机器可读旧记录仍位于 `build_boot_verify/battery_high_soc_verification.json`。

本轮最终验证：主机回归套件实际运行通过；ESP-IDF 应用大小 `0x30c670`（3,196,528 字节），分区剩余 23%，SHA-256 为 `3199B8B188AB8C6516386EF1CB72257FE91A7805D2A7C14DB46795F1235A09A4`。固件已写入 COM6 并校验，随后连续监视 180 秒；启动、ADC 校准、触控注册和电源唤醒正常，未出现崩溃或电池采样错误。插着 USB 且停充时，约 61 秒和 121 秒分别记录 `VBAT=4141/4142mV`、`SOC=100%`、`full=1`、`correction=0`，没有显示退格。该次实物验证覆盖停充保持，非满电 93% 插线场景仍需用户在合适电量下实际插拔确认。

同日待机耗电复查：用户对比设备的硬件与测试条件不确定，“每 7 分钟掉 1%”只能作为体验目标。按标称 1000mAh 折算，它要求平均电池电流约 86mA、约 11.7 小时满放；不能据此调整本板电压曲线或用定时器强制限制百分比。当前 CPU 固定 240MHz、PM 未启用，亮屏普通模式保留唤醒词采集，默认亮度 75%；本轮未改这些设置，也未再改电量估算器。串口没有电池电流计数据，无法确认实际容量与高电量段估算误差的比例。

修复两处明确的板级待机问题：原生音频为保持双工录音而保留输出通道，之前本板让 LM4890 功放也持续开启；现在复用 WatchRuntime 轮询，在最后一次 PCM 写入完成后空闲 15 秒仅关闭功放，保持采集和时钟。按 TI LM4890 数据手册的最大 220ms 唤醒时间，下一次播放预留 250ms 稳定时间后再提交首块 PCM；可能增加相应首句等待，15 秒保持避免正常语句间隔或每 3 秒闹钟提示频繁启停。另修复壁纸轮播忽略已保存间隔、始终每 3 秒整页重绘的问题，现按 `wallpaper_interval_seconds` 运行，默认 10 秒，0 禁止自动轮播，手动滑动保留。相关修改仅在 `common/hardware/baji_audio_codec.*`、`common/watch/watch_runtime.cc` 和 `common/ui/watch_ui.cc`。

省电修正版 ESP-IDF 编译、COM6 烧录校验通过，应用大小 3,195,520 字节，SHA-256 为 `bc17841b005ae724908023b4c6ac3dc071bc356ab42d13a07a84698a8f60552a`，设备 ELF 标识 `e508c4736`。启动后 15156ms 日志显示功放开启，31636ms 显示 `Amplifier idle; duplex clocks retained`，证明待机关闭路径执行。模拟/数字麦克风两分支主机测试均编译成功，但测试程序执行被 Windows 应用程序控制策略阻止，不能称主机运行测试通过；实物首字是否完整、切换噗声和节省电流仍需听音及电流实测。功放手册静态电流是数毫安量级，不能将本项修复表述为已实现 7 分钟/1%。构建日志为 `build_boot_verify/battery_idle_power_build.log`。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| 同名板级 `.cc` / `config.h` / `config.json` | 硬件注册、引脚、构建默认值和入口接线 |
| `common/board.cmake` | 递归注册分类目录中的板级源码，选择原生字体与表情，注册资源打包及启动 / LVGL 包装 |
| `common/hardware/baji_display.*` | LCD 与触控、原生显示接口适配、纯充电页面，以及本板 LVGL 任务栈配置 |
| `common/hardware/baji_audio_codec.*` | ES8311、TCA9554 功放、播放前稳定等待、功放空闲休眠和临时提醒音量 |
| `common/power/power_boot.cc` / `power_manager.*` | 开机门控、纯充电、RTC 标记、电源键与独立电池任务 |
| `common/power/battery_monitor.*` | 校准 ADC 采样、USB/STAT 状态、统一快照及 RTC/NVS 电量检查点 |
| `common/power/battery_estimator.h` | 电压容量曲线、滤波、插拔补偿、渐进校正及满电判定；不依赖硬件接口 |
| `common/power/low_voltage_protection.h` | 原始电压的持续低压判定和重启门槛 |
| `common/resources/charging_icon.h` | 纯充电页的白色抗锯齿电池闪电字形 |
| `common/watch/watch_services.*` | 持久化、校时、闹钟 / 日程 / 倒计时调度与 MCP 工具 |
| `common/watch/watch_runtime.*` | 页面与硬件协调、息屏 / 唤醒、提醒声音与原生对话衔接 |
| `common/ui/watch_ui.*` | 统一持有页面状态，管理页面路由、生命周期、手势、定时刷新与异步结果 |
| `common/ui/pages/*.cc` | 每个页面独立实现布局、交互及页面专用刷新 |
| `common/ui/overlays/*.cc` | 状态栏、控制中心、弹窗、提醒、电源界面，分别独立实现 |
| `common/ui/watch_ui_widgets.cc` / `watch_ui_internal.h` | 共用控件、颜色、文字排版、日期格式与动画辅助；内部头只声明共享接口 |
| `common/ui/watch_resources.*` | 原型字体 / Lucide 图标加载、人物 JPEG 帧解码与播放，资源生命周期和 PSRAM 管理 |
| `common/tools/generate_watch_resources.py` | 生成单个资源包，支持保留字体/图标只替换双视频；完整生成时递归收集拆分页的文案 |
| `common/tools/test_battery_estimator.cpp` | 主机电量回归测试，不编入固件，覆盖插拔、噪声、恢复、恒压及低电状态 |
| `common/resources/watch_ui.pack` | 字体子集、原型图标、人物动画集中在一个包内 |
| `common/resources/*.rgb565` | 用户提供的 3 张 360 × 360 JPG 转换为壁纸，每张 12 字节 LVGL9 头和 RGB565 像素 |

页面文件与 `WatchUi::Page` 一一对应：

| 页面 | `common/ui/pages/` 下的文件 |
| --- | --- |
| 待机 | `standby_page.cc` |
| 主菜单 | `menu_page.cc` |
| AI 对话 | `chat_page.cc` |
| 日历与日程 | `calendar_page.cc` |
| 闹钟列表 | `alarms_page.cc` |
| 闹钟编辑 | `alarm_editor_page.cc` |
| 倒计时 | `countdown_page.cc` |
| 设置 | `settings_page.cc` |
| 显示设置 | `display_settings_page.cc` |
| WLAN | `network_page.cc` |
| 应援灯 | `lightstick_page.cc` |

所有页面都是独立编译单元，共用同一个 `WatchUi` 对象；页面间通过管理器的 `Navigate()` / `GoBack()` 跳转，由 `Render()` 统一分派并清理旧控件。异步任务、菜单延迟导航和动画的生命周期仍由该对象管理。新增页面时，在 `watch_ui.h` 的 `Page` 与私有方法中声明，在 `watch_ui.cc` 的路由中登记，并在 `pages/` 添加对应实现；同类控件优先复用公共组件。

设置、闹钟保存和倒计时操作仍在原生应用任务中执行；提交期间由 LVGL 每20ms检查一次完成标志，完成即更新界面并暂停检查，不再等待每秒一次的设备状态刷新。20ms是结果检查间隔，并非实测触控到显示的总延迟。倒计时完成回调只重建一次页面；已经位于 AI 页面时，原生连接、聆听和说话状态变化复用现有控件与视频缓冲，避免反复销毁和重新解码。

页面复查后进一步修复：日历在首次校时、时间失效或跨自然日时更新日期和日程；同页数据刷新保留闹钟、设置、显示设置、日程与网络列表的滚动位置，主动切页和切换日期则从顶部开始。语言切换使用同一异步保存路径，保存成功后立即刷新。WLAN 密码输入、删除、空格和显隐切换只更新现有控件；较长的明文显示末尾，保证能看到新输入字符，切换键盘层仍重建键帽。

`common/resources/` 只放需要烧入设备的 `.pack` / `.rgb565` 素材。资源加载器放 `ui/`、生成脚本放 `tools/`，避免源码与工具被原生资源打包器一并烧入设备。各分类目录共享 `common` 作为头文件搜索根，跨目录引用使用 `ui/`、`hardware/`、`power/`、`watch/` 前缀；板级构建入口仍为原有 `common/board.cmake`。

AI 页小时间使用表盘 Segoe UI 14px；会话状态、用户转写及 AI 回复的中文使用表盘 Noto Sans SC Regular 14px。小时间／状态保持透明背景，用户转写为浅粉色，AI 回复为白色；小智原生字体仅在表盘资源缺失时作为回退。资源包保留已有 Segoe UI 拉丁字面；中文主要文字14px、辅助文字至少12px，并保留1px字距，避免极小粗体中文笔画及相邻字符挤在一起。中文字模按整数目标像素直接生成，保留默认 FreeType hinting，不缩放、不锐化。字源复用已有 LVGL 组件内的 `NotoSansSC-Regular.ttf`，不新增生产 TTF；必要字形仍放在一个资源包中，普通编译直接使用该包。common 字集采用14px，加载器按包内覆盖最广的普通字重字体选择回退，兼容旧包。页面按本轮BAJI菜单的视觉规范重新排版，以实体屏清晰度和圆屏空间为准。

AI 状态在显示时读取表盘的运行时语言设置，将原生 `STANDBY/CONNECTING/LISTENING/SPEAKING` 映射为对应中英文；英文沿用小智文案 `Standby/Connecting.../Listening.../Speaking...`。原始状态字符串继续用于内部会话判断，未知动态消息不翻译。翻译在显示缓存比较前执行，使仅切换语言时也能刷新。空闲仍显示小时间，保留状态先到、消息后到时的兜底顺序；不改变用户转写、AI 回复、字体或布局。

该语言修复已编译并烧录COM6，同时按用户选择使用当前原字体资源。应用3,201,888字节，SHA-256 `7e34b90607dc2b2a4f910d83ffd81e9be73c8ecc9c550cf876ea719eab400138`；assets SHA-256 `9ecab54caa64120b3f8281a26046e6814dbad4f81d80929fc62ddc5ec7bc3fe5`。四条英文状态字宽50/79/64/64px，均在原160px框内。两段烧录均通过哈希校验，25秒启动日志确认新ELF、资源加载、联网及待命。`build/chat-language-fix/`保存源码检查、构建、烧录及启动记录；尚未以实屏照片验证语言切换后的视觉结果。

文字按行高校正基线。菜单、待机和时间工具使用项目已有 LVGL 字源 Montserrat Medium 的32/44px数字子集 `-:0123456789`；中文沿用 Noto 12/14/16px。布局使用明确行框，不让字体自然行高推动整组卡片下移。Lucide 图标使用已有原生尺寸，不通过整图缩放凑尺寸；顶部网络图标使用LVGL圆弧和圆角矩形。电池使用23×12外壳、贴合外壳的2×4端帽和19×8连续内槽，单块填充宽度按电量四舍五入到实际像素；四边计入边框后均留2px。内槽只有浅色底与完整填充，没有分块。新人物动画为待机40帧和说话40帧，设备按实际解码能力跳帧，不积压帧，退出 AI 页释放视频缓冲。

用户转写和 AI 回答各显示最新三行完整字幕，14px字号、22px行距，原消息全文保留；用户转写保留浅粉色，AI 回复保留白色。字幕卡固定236px宽，用户文字、AI回复及等待提示的背景与顶部状态栏一致，采用64/255背景不透明度的轻透效果和圆胶囊外形；文字自身不透明度保持原值。顶部时间／状态使用表盘14px透明文字控件，沿用现有160px宽度、22px行框位置和居中布局：空闲显示 `HH:MM`（未校时为 `--:--`），唤醒后同位置切换为原生连接、聆听或说话状态，恢复空闲立即显示时间。AI 页返回按钮使用160/255不透明度黑色圆底与全白箭头，按下时底色不透明度为192/255；其他页保留白色8%不透明度圆底与70%不透明度箭头。开关使用统一的父行右侧垂直居中布局：44×36触摸区内居中放置40×22轨道，16px圆点在轨道内居中。设置、显示设置、WLAN和闹钟六处开关均按容器高度定位，带开关的设置副标题限制为120px，动态SSID更新也保留该宽度并单行省略，避免文字覆盖开关。闹钟/倒计时Tab计入1px外边框后，选中胶囊上下及外侧均留3px，文字单独居中。没有增加实时视频模糊处理；控制中心、弹窗、电源或提醒覆盖对话时暂停视频与页面动画，退出后恢复。待机和倒计时不再运行持续装饰动画。

本板通过链接包装 `lvgl_port_init` 将 LVGL 任务栈至少配置为 16 KiB，保留原来的任务优先级、核绑定和内存类型。页面从触控回调中创建、销毁时需要额外的事件和布局调用栈；此前正式设备运行曾在退出 AI 后触发默认 7 KiB 栈溢出。配置限于本板，不修改小智的显示基类或 IDF 组件。

字体方案参考 [openvela 的 FreeType 渲染](https://github.com/open-vela/apps_graphics_lvgl/blob/dev/src/libs/freetype/lv_freetype_image.c)、[OpenHarmony UI 字体度量与缓存](https://github.com/openharmony/graphic_ui/blob/master/frameworks/font/ui_font_vector.cpp)、[LVGL 的 14px 中文示例](https://github.com/lvgl/lvgl/blob/7cf49a06ce30036906721766e31061ad833973d3/src/font/lv_font_source_han_sans_sc_14_cjk.c)，并对比了本机 Noto / 微软雅黑、字号、字重及 hinting；没有复制它们的字体引擎或修改小智原生代码。公开框架不等同于特定小米 / 华为量产手表的完整实现。此前研究产物曾位于 `build/watch-font-research/`；该临时目录目前已不存在，不能作为本轮验证证据。

视频逐帧从当前 assets 取出 JPEG 并复制到暂存区，不常驻整段压缩视频。两段动画共用解码器、双 RGB565 画布及单帧暂存，切换不重新分配内存。字体索引放在 PSRAM。覆盖层或息屏期间只记录片段变化，恢复时显示所选片段；升级期间停止读取资源，资源失效后保持最后一帧并等待重新加载。

旧资源包 v2～v4 将固定暗角和底部阴影合成进人物视频；v5 保留原始画面亮度。v3 扩展字体尺寸和字重字段；v4 在48字节头后增加两段帧数，v5 沿用该布局，帧表按待机、说话排序。当前加载器兼容 v1～v5，各版本均不再额外叠加全屏遮罩，但旧资源里已烘入的阴影只能通过更新资源消除。新 v5 包必须同时使用支持 v5 的固件。此前 COM77 使用旧24fps视频、在 Wi-Fi 空闲时测得约11.7fps提交帧率；当前使用源视频原生10fps，该历史记录不代表新视频与 AI 听说并行的实测性能。

壁纸和资源包随默认 assets 分区打包，更新本固件必须同时烧录 `generated_assets.bin`。字体、图标及壁纸加载后保留有效副本，避免服务器重载 assets 导致旧指针失效。服务器资源更新若不包含本板资源，重启后将回退，服务器包需要保留本板文件。浏览器的子像素字体、CSS 模糊和 LCD RGB565 输出存在渲染差异，不能用编译通过代替视觉验收。

## 编译与验证

使用 ESP-IDF 5.5.4、ESP32-S3、16 MB Flash、八线 PSRAM，分区为 `partitions/v2/16m.csv`。本次图像修复使用独立构建目录 `build/display_quality_firmware`；原 `build/` 存在 CMake 生成器缓存冲突，旧构建产物予以保留。

```powershell
idf.py -B build/display_quality_firmware build
```

2026-09-23 壁纸横纹、交互响应及其余页面复查修复已完成编译验证：`build/watch-response-firmware/xiaozhi.bin` 为3,176,384字节，应用分区剩余约23%；同目录 `generated_assets.bin` 为6,038,558字节，8 MiB 资源分区剩余2,350,050字节。当时 app SHA-256 为 `56d901a5d333267e1eb5c139da0d3a891cecdb0a5bd8424b650651e45750155f`。

当前资源包为 v5，共4,753,645字节，包含70个字体规格、138个图标规格及两段各40帧的动画，SHA-256 为 `90c3af9c5749677aafc5cd88c29ad2825a46d69f5531ab9a36a191cf79c0781e`。本次图像修复保留了全部26,863个字形和138个图标，仅替换视频为原始未压暗 JPEG。旧 v4 包大小3,997,819字节、SHA `695e9c556c16fad3b26ea3f099c5c3477551ec1347353aba75cd203329a4d0a8`，以及上述2026-09-23构建产物仅作历史记录，不可用于本次更新。

后续仅换动画可运行 `common/tools/generate_watch_resources.py --replace-videos-in <现有资源包> --standby-video <待机MJPEG> --speaking-video <说话MJPEG> --work-dir <构建临时目录>`（脚本路径相对于本板目录），默认源帧率和播放帧率均为10fps，输出到本板 `common/resources/watch_ui.pack`。原生360×360 MJPEG逐帧复制；其他帧率的原生流需同时指定 `--mjpeg-source-fps` 与 `--video-fps`，不静默增删帧。MP4等视频另加 `--ffmpeg <FFmpeg路径>` 进行单次转换。此模式保留已有字模和图标，不依赖原型工程、Node 或字体重建；两段视频均需明确提供。普通 `idf.py build` 会把更新后的资源包重新打入 assets。

以下为2026-09-23等此前工作的历史记录，路径、截图与数值不作为本次无阴影 v5 修复的验收证据。

`build/watch-detail-fix/resource-build/manifest.json` 与 `independent-validation.json` 记录两段来源、原尺寸/帧率、80帧 baseline JPEG 解码、旧帧移除、字模/图标不变、v4 重建一致性及8类损坏输入拒绝检查。`build/watch-video-review/player_review.cc` 直接包含生产播放器，通过57项主机检查，覆盖分段循环、重复状态、暂停切换/恢复、缓冲复用、升级保护和分配/解码失败清理；仅平台接口为替身，不代表 LCD 或真实 AI 会话验收。

`build/watch-detail-fix/ai-video-states.png` 为当时待机/说话首帧与中间帧的离线页面拼图；`ai-standby.png`、`ai-speaking.png` 分别展示两种状态，`chat-background-current.jpg` 当时更新为 v4 包待机首帧。这些是实际资源与页面代码组合的近似预览，不是实机截图。

当时构建日志为 `build/watch-detail-fix/build.log`；同目录 `build-verification.json` 记录该轮最终源文件、app、assets 和资源包的 SHA，核对全部26个板级 C++ 源文件对应对象、预览版本一致及板级链接包装保留。UI 摘要覆盖 `ui/` 下全部22个源码/头文件，包含有序相对路径及文件字节，避免只校验管理器而遗漏独立页面。此前分类重构保留了原72个方法，拆出的4个刷新/动画方法按原调用顺序还原后，C++ token 与拆分前一致；其历史记录位于 `build/watch-file-refactor/extraction.json`。

当时条纹与响应检查记录于 `build/watch-detail-fix/wallpaper-shading-verification.json` 和 `ui-responsiveness-verification.json`。主机执行该轮 C++ 壁纸处理函数，平坦测试顶部155行从33个亮度行均值变为155个连续递增行均值；异步完成测试覆盖无一秒刷新时的结果消费、失败重试、离页、销毁和倒计时单次重建，聊天测试确认连续状态通知不重建页面。`wallpaper-circular-shading-comparison.png` 与 `wallpaper-flat-gradient-comparison.png` 展示该轮修复前后对比，属于离线验证，不代表实屏显示或端到端响应时间已验收。

`report.json` 记录6个离线代表状态及补充预览的字模、图标和圆屏布局检查，0项问题。开关40×22轨道内的16px圆点上下各留3px，圆点位于开/关端时也各留3px。旧 `build/watch-menu-redesign`、`build/watch-pages-polish` 临时验证目录目前已不存在，不作为2026-09-28图像修复的验证材料。

其余页面复查覆盖11个页面与5个覆盖层的源码；`build/watch-detail-fix/all-page-audit/` 增补26个中英文代表状态的离线预览，包括菜单、闹钟、编辑、倒计时、日历、WLAN与键盘、控制中心、电源、提醒和确认弹窗，静态几何检查0项问题。`current-image-inventory.json` 列出当时两套预览的44张图及校验值，旧 `standby.png`、`ai-idle/listening/reply.png` 等不在该清单内的历史图不作为该轮验收证据。预览未执行原生 LVGL 滚轮、动态滚动或 LCD 刷新，不能据此断言所有实屏状态都正常。`ui-page-refresh-verification.json` 记录实际 C++ 方法的校时/跨日刷新、语言保存完成、同页滚动保持和聊天回归检查；平台接口使用主机替身。

`build/watch-page-audit/wifi-input-verification.json` 记录真实密码输入方法的主机验证：逐字/删除保留控件、显示末尾但提交完整密码、隐藏模式、63字节上限与多字节字符删除、错误提示清除、连接按钮状态和离页保护均通过。未记录或读取设备的真实 Wi-Fi 密码。

`build/watch-detail-fix/wallpaper-carousel.png` 展示三张新壁纸的圆屏待机效果；`wallpaper-replacement.json` 记录源图、替换前后壁纸及资源包SHA，当时已核对更新后的资源包只包含三个壁纸条目且内容与该轮资源文件完全一致。资源更新需烧录 `generated_assets.bin` 并重启，单独烧录应用不会更新壁纸。

`build/watch-detail-fix/control-alignment.png` 展示闹钟/倒计时两种Tab选中态及44/60/64px行内的开关对齐。几何检查包含父行、触摸区、轨道和圆点的中心，同时检查副标题与开关的边界；图为离线近似预览，不代表实机触控已验收。

`build/watch-detail-fix/overview.png` 展示设置入口、显示设置开/关两态、自动熄屏选项，以及待机显示/隐藏时钟日期。预览使用实际资源包的字模、图标和壁纸，明确标注为离线组合图，不是 LVGL 帧缓冲或 LCD 照片。它用于检查文字布局与基本配色，未对实物触控、阴影合成、刷新率或面板波纹作已通过结论。

`build/watch-detail-fix/status-variants.png` 对比深浅背景上的12种状态，包含满电、75%/50%/25%连续填充、空电量、低电红色、未知电量、充电和网络离线。百分比最大“100%”原生字模宽29px，32px文字区域无溢出；连续电量条上下留白各2px，没有内部格间距。与总览一样，此图为离线资源预览，圆弧由预览脚本近似绘制。 `battery-continuous.png` 展示当时一体填充的电池样式；`status-adjustment.png` 与 `wifi-proportion-review.png` 保留之前尺寸和Wi-Fi比例调整的历史对比。

状态栏网络标记固定为三段外观；Wi-Fi恢复紧凑的90°扇形轮廓，弧半径10/5px、线宽2px、角度225–315°，圆点为同心2×2px；通过收小内弧保留3px弧间距，圆头端部也分离，移动信号条为3×4/8/12px。充电图标使用已有11px资源，百分比为12px/600字模。代码交叉审查核对连续填充宽度及0%/1%/100%边界、未知电量、充电/低电量优先级、离线斜线的点数组生命周期，以及电量条计入边框后的对称留白。正常状态栏宽108px、充电时124px，在360px圆屏顶部安全区域内。主机原生渲染执行仍未验证，不能以静态审查代替实屏检查。

2026-09-23 上述页面修复当时未烧录设备，该记录不描述2026-09-28图像修复的烧录状态。当时列出的实物验收项目包括壁纸显示、开关及保存操作响应、AI 听说切换流畅度、状态栏清晰度、连续电量条及充电/低电提示。显示设置的重启恢复、菜单触控、电源键唤醒、真实 AI 听说、4G 和续航仍需对应的实物验收，历史离线检查不能替代这些结果。

## 字体许可

`watch_ui.pack` 中的 Noto Sans SC 中文子集和 Montserrat Medium 菜单时钟子集遵循以下 SIL Open Font License 1.1；其余资源沿用各自许可。两套字源均复用已有 LVGL 组件内的字体文件。

```text
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'
Copyright 2011 The Montserrat Project Authors (https://github.com/JulietaUla/Montserrat)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
