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

AI 背景使用用户提供的 `standby.mp4` 和 `speaking.mp4`，来源为 `C:\Users\YuYevi\Documents\WorkBuddy\2026-09-23-15-46-16\output`。两段各4秒、40帧、360×360、10fps，无音轨；保持原构图和动作，不加入额外呼吸缩放。原生状态为 `kDeviceStateSpeaking` 时循环说话动画，空闲、连接、聆听及等待回复时循环待机动画。重复刷新同一状态不重置动画，切换时从对应片段首帧开始；中断与回复结束跟随原生会话状态变化。旧人物动画已从资源包移除，字体、图标与待机壁纸不变。

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
| 电源 / 电池 | 保持 GPIO1、电源键 GPIO2、USB 检测 GPIO5、电池 ADC GPIO4 |

触控复用已有 CST816S 单点协议组件和 `lvgl_port_add_touch()`，没有新建触控驱动。LCD 初始化前已复位，触控驱动不再次拉低共用复位。触控方向跟随 `config.h` 的显示镜像设置。

按实物要求移除六轴传感器、抬腕唤醒和姿态校准，不再探测或轮询 IMU。手动唤醒只使用实体电源键；闹钟、倒计时与日程到点仍可亮屏提醒。旧存储中的抬腕标志被忽略，不清除已有闹钟或日程。

默认闲置 15 秒关闭背光、暂停壁纸轮播与语音唤醒。“设置 → 显示设置 → 自动熄屏”可选15秒、30秒、1分钟、2分钟、5分钟或永不，保存到原有手表设置并在重启后恢复；同页的常亮与“永不”使用同一字段。省电模式将亮度限制到35%，不覆盖所选熄屏时间。手电筒暂时使用100%亮度，退出恢复原设置，不把临时亮度写入NVS。已移除原先闲置300秒自动关机，插USB时也可息屏。AI对话、升级及新提醒期间保持亮屏；对话时主动选择电源菜单的息屏，会先结束对话再关闭背光。

1000 mAh 电池的 24 小时目标要求平均电池电流不超过约 41.7 mA，考虑容量余量还需更低。此版本没有完成电池侧电流和全天续航测试，不能以息屏功能替代续航验收。Wi-Fi 与 4G 必须分别测量；当前 ML307 的原生节能能力仍有限。

纯充电、USB 拔出下电、长按电源和 RTC 启动标记沿用已有 `power_boot` / `power_manager` 实现。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| 同名板级 `.cc` / `config.h` / `config.json` | 硬件注册、引脚、构建默认值和入口接线 |
| `common/board.cmake` | 递归注册分类目录中的板级源码，选择原生字体与表情，注册资源打包及启动 / LVGL 包装 |
| `common/hardware/baji_display.*` | LCD 与触控、原生显示接口适配、纯充电页面，以及本板 LVGL 任务栈配置 |
| `common/hardware/baji_audio_codec.*` | ES8311、TCA9554 功放和临时提醒音量 |
| `common/power/power_boot.cc` / `power_manager.*` | 开机门控、纯充电、RTC 标记与电池 / 电源管理 |
| `common/watch/watch_services.*` | 持久化、校时、闹钟 / 日程 / 倒计时调度与 MCP 工具 |
| `common/watch/watch_runtime.*` | 页面与硬件协调、息屏 / 唤醒、提醒声音与原生对话衔接 |
| `common/ui/watch_ui.*` | 统一持有页面状态，管理页面路由、生命周期、手势、定时刷新与异步结果 |
| `common/ui/pages/*.cc` | 每个页面独立实现布局、交互及页面专用刷新 |
| `common/ui/overlays/*.cc` | 状态栏、控制中心、弹窗、提醒、电源界面，分别独立实现 |
| `common/ui/watch_ui_widgets.cc` / `watch_ui_internal.h` | 共用控件、颜色、文字排版、日期格式与动画辅助；内部头只声明共享接口 |
| `common/ui/watch_resources.*` | 原型字体 / Lucide 图标加载、人物 JPEG 帧解码与播放，资源生命周期和 PSRAM 管理 |
| `common/tools/generate_watch_resources.py` | 生成单个资源包，支持保留字体/图标只替换双视频；完整生成时递归收集拆分页的文案 |
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

`common/resources/` 只放需要烧入设备的 `.pack` / `.rgb565` 素材。资源加载器放 `ui/`、生成脚本放 `tools/`，避免源码与工具被原生资源打包器一并烧入设备。各分类目录共享 `common` 作为头文件搜索根，跨目录引用使用 `ui/`、`hardware/`、`power/`、`watch/` 前缀；板级构建入口仍为原有 `common/board.cmake`。

小智原生16px字体仍作为资源缺失时的回退。资源包保留已有 Segoe UI 拉丁字面；中文使用 Noto Sans SC Regular，主要文字14px、辅助文字至少12px，并保留1px字距，避免极小粗体中文笔画及相邻字符挤在一起。中文字模按整数目标像素直接生成，保留默认 FreeType hinting，不缩放、不锐化。字源复用已有 LVGL 组件内的 `NotoSansSC-Regular.ttf`，不新增生产 TTF；必要字形仍放在一个资源包中，普通编译直接使用该包。common 字集采用14px，加载器按包内覆盖最广的普通字重字体选择回退，兼容旧包。页面按本轮BAJI菜单的视觉规范重新排版，以实体屏清晰度和圆屏空间为准。

文字按行高校正基线。菜单、待机和时间工具使用项目已有 LVGL 字源 Montserrat Medium 的32/44px数字子集 `-:0123456789`；中文沿用 Noto 12/14/16px。布局使用明确行框，不让字体自然行高推动整组卡片下移。Lucide 图标使用已有原生尺寸，不通过整图缩放凑尺寸；顶部网络图标使用LVGL圆弧和圆角矩形。电池使用23×12外壳、贴合外壳的2×4端帽和19×8连续内槽，单块填充宽度按电量四舍五入到实际像素；四边计入边框后均留2px。内槽只有浅色底与完整填充，没有分块。新人物动画为待机40帧和说话40帧，设备按实际解码能力跳帧，不积压帧，退出 AI 页释放视频缓冲。

用户转写和 AI 回答各显示最新三行完整字幕，14px 字号、22px 行距，原消息全文保留。字幕卡固定236px宽，参考 BAJI 的灰色 `#6b7280`、40% 不透明度和圆胶囊外形。顶部仅有一个14px透明文字控件：空闲显示 `HH:MM`（未校时为 `--:--`），唤醒后同位置切换为原生连接、聆听或说话状态，恢复空闲立即显示时间；没有独立状态胶囊。返回按钮使用白色8%不透明度圆底与70%不透明度箭头。开关使用统一的父行右侧垂直居中布局：44×36触摸区内居中放置40×22轨道，16px圆点在轨道内居中。设置、显示设置、WLAN和闹钟六处开关均按容器高度定位，带开关的设置副标题限制为120px，动态SSID更新也保留该宽度并单行省略，避免文字覆盖开关。闹钟/倒计时Tab计入1px外边框后，选中胶囊上下及外侧均留3px，文字单独居中。没有增加实时视频模糊处理；控制中心、弹窗、电源或提醒覆盖对话时暂停视频与页面动画，退出后恢复。待机和倒计时不再运行持续装饰动画。

本板通过链接包装 `lvgl_port_init` 将 LVGL 任务栈至少配置为 16 KiB，保留原来的任务优先级、核绑定和内存类型。页面从触控回调中创建、销毁时需要额外的事件和布局调用栈；此前正式设备运行曾在退出 AI 后触发默认 7 KiB 栈溢出。配置限于本板，不修改小智的显示基类或 IDF 组件。

字体方案参考 [openvela 的 FreeType 渲染](https://github.com/open-vela/apps_graphics_lvgl/blob/dev/src/libs/freetype/lv_freetype_image.c)、[OpenHarmony UI 字体度量与缓存](https://github.com/openharmony/graphic_ui/blob/master/frameworks/font/ui_font_vector.cpp)、[LVGL 的 14px 中文示例](https://github.com/lvgl/lvgl/blob/7cf49a06ce30036906721766e31061ad833973d3/src/font/lv_font_source_han_sans_sc_14_cjk.c)，并对比了本机 Noto / 微软雅黑、字号、字重及 hinting；没有复制它们的字体引擎或修改小智原生代码。公开框架不等同于特定小米 / 华为量产手表的完整实现。此前研究产物曾位于 `build/watch-font-research/`；该临时目录目前已不存在，不能作为本轮验证证据。

视频逐帧从当前 assets 取出 JPEG 并复制到暂存区，不常驻整段压缩视频。两段动画共用解码器、双 RGB565 画布及单帧暂存，切换不重新分配内存。字体索引放在 PSRAM。覆盖层或息屏期间只记录片段变化，恢复时显示所选片段；升级期间停止读取资源，资源失效后保持最后一帧并等待重新加载。

资源包 v2 及以后将固定暗角和底部阴影合成进人物视频，减少逐帧透明混合；加载旧 v1 包时仍绘制对应遮罩，避免漏画或重复叠加。v3 扩展字体尺寸和字重字段；v4 在48字节头后增加两段帧数，帧表按待机、说话排序，当前加载器兼容 v1 / v2 / v3 / v4。新 v4 包必须同时使用本版固件。此前 COM77 使用旧24fps视频、在 Wi-Fi 空闲时测得约11.7fps提交帧率；本次使用源视频原生10fps，该历史记录不代表新视频与 AI 听说并行的实测性能。

壁纸和资源包随默认 assets 分区打包，更新本固件必须同时烧录 `generated_assets.bin`。字体、图标及壁纸加载后保留有效副本，避免服务器重载 assets 导致旧指针失效。服务器资源更新若不包含本板资源，重启后将回退，服务器包需要保留本板文件。浏览器的子像素字体、CSS 模糊和 LCD RGB565 输出存在渲染差异，不能用编译通过代替视觉验收。

## 编译与验证

使用 ESP-IDF 5.5.4、ESP32-S3、16 MB Flash、八线 PSRAM，分区为 `partitions/v2/16m.csv`。当前构建目录为 `build/`。

```powershell
idf.py build
```

2026-09-23 文件分类与独立页面重构已完成编译验证：`build/xiaozhi.bin` 为3,174,768字节，应用分区剩余约23%；`build/generated_assets.bin` 为6,038,558字节，8 MiB 资源分区剩余2,350,050字节。当前 app SHA-256 为 `000fe7d2a906a6e12621d4df7f45b9983102c28332e7d4c8994495f324b4de96`。

资源包沿用 v4，共3,997,819字节，包含70个字体规格、138个图标规格及两段各40帧的动画，SHA-256 为 `695e9c556c16fad3b26ea3f099c5c3477551ec1347353aba75cd203329a4d0a8`。之前的动画替换保留了70个字体的26,863个字形和138个图标，并移除了旧169帧。此次目录与页面重构没有改动资源内容；重新生成的 assets 与拆分前逐字节一致，SHA-256 为 `28cabd74c1064c7c48f2486035d79b08cb86b0a650480a40cadbc916f495db52`。源码调整限定在本板目录，未修改小智原生代码。

后续仅换动画可运行 `common/tools/generate_watch_resources.py --replace-videos-in <现有资源包> --standby-video <待机MP4> --speaking-video <说话MP4> --ffmpeg <FFmpeg路径> --work-dir <构建临时目录>`（脚本路径相对于本板目录），默认10fps并输出到本板 `common/resources/watch_ui.pack`。此模式保留已有字模和图标，不依赖原型工程、Node 或字体重建；两段视频均需明确提供。普通 `idf.py build` 会把更新后的资源包重新打入 assets。

`build/watch-detail-fix/resource-build/manifest.json` 与 `independent-validation.json` 记录两段来源、原尺寸/帧率、80帧 baseline JPEG 解码、旧帧移除、字模/图标不变、v4 重建一致性及8类损坏输入拒绝检查。`build/watch-video-review/player_review.cc` 直接包含生产播放器，通过57项主机检查，覆盖分段循环、重复状态、暂停切换/恢复、缓冲复用、升级保护和分配/解码失败清理；仅平台接口为替身，不代表 LCD 或真实 AI 会话验收。

`build/watch-detail-fix/ai-video-states.png` 为新待机/说话首帧与中间帧的离线页面拼图；`ai-standby.png`、`ai-speaking.png` 分别展示两种状态，`chat-background-current.jpg` 已更新为新包待机首帧。这些是实际资源与页面代码组合的近似预览，不是实机截图。

本次构建日志为 `build/watch-detail-fix/build.log`；同目录 `build-verification.json` 记录最终源文件、app、assets 和资源包的 SHA，核对全部26个板级 C++ 源文件对应对象、预览版本一致及板级链接包装保留。UI 摘要覆盖 `ui/` 下全部22个源码/头文件，包含有序相对路径及文件字节，避免只校验管理器而遗漏独立页面。原72个方法全部保留且无重复；拆出的4个刷新/动画方法按原调用顺序还原后，C++ token 与拆分前一致，页面布局、手势和异步生命周期保持。`build/watch-file-refactor/extraction.json` 记录方法归属与拆分前资源 SHA。生成器的 `--help`、Python 编译检查及新根目录/文案扫描验证通过。

`report.json` 记录6个离线代表状态及补充预览的字模、图标和圆屏布局检查，0项问题。开关40×22轨道内的16px圆点上下各留3px，圆点位于开/关端时也各留3px。旧 `build/watch-menu-redesign`、`build/watch-pages-polish` 临时验证目录目前已不存在，不作为本次验证材料。

`build/watch-detail-fix/wallpaper-carousel.png` 展示三张新壁纸的圆屏待机效果；`wallpaper-replacement.json` 记录源图、替换前后壁纸及资源包SHA，已核对新包只包含三个壁纸条目且内容与当前资源文件完全一致。资源更新需烧录 `generated_assets.bin` 并重启，单独烧录应用不会更新壁纸。

`build/watch-detail-fix/control-alignment.png` 展示闹钟/倒计时两种Tab选中态及44/60/64px行内的开关对齐。几何检查包含父行、触摸区、轨道和圆点的中心，同时检查副标题与开关的边界；图为离线近似预览，不代表实机触控已验收。

`build/watch-detail-fix/overview.png` 展示设置入口、显示设置开/关两态、自动熄屏选项，以及待机显示/隐藏时钟日期。预览使用实际资源包的字模、图标和壁纸，明确标注为离线组合图，不是 LVGL 帧缓冲或 LCD 照片。它用于检查文字布局与基本配色，未对实物触控、阴影合成、刷新率或面板波纹作已通过结论。

`build/watch-detail-fix/status-variants.png` 对比深浅背景上的12种状态，包含满电、75%/50%/25%连续填充、空电量、低电红色、未知电量、充电和网络离线。百分比最大“100%”原生字模宽29px，32px文字区域无溢出；连续电量条上下留白各2px，没有内部格间距。与总览一样，此图为离线资源预览，圆弧由预览脚本近似绘制。 `battery-continuous.png` 展示当前一体填充的电池样式；`status-adjustment.png` 与 `wifi-proportion-review.png` 保留之前尺寸和Wi-Fi比例调整的历史对比。

状态栏网络标记固定为三段外观；Wi-Fi恢复紧凑的90°扇形轮廓，弧半径10/5px、线宽2px、角度225–315°，圆点为同心2×2px；通过收小内弧保留3px弧间距，圆头端部也分离，移动信号条为3×4/8/12px。充电图标使用已有11px资源，百分比为12px/600字模。代码交叉审查核对连续填充宽度及0%/1%/100%边界、未知电量、充电/低电量优先级、离线斜线的点数组生命周期，以及电量条计入边框后的对称留白。正常状态栏宽108px、充电时124px，在360px圆屏顶部安全区域内。主机原生渲染执行仍未验证，不能以静态审查代替实屏检查。

本次没有烧录设备。上板后需检查状态栏图标/百分比清晰度、连续电量条及充电/低电提示、深浅壁纸上的对比度。显示设置的重启恢复、菜单触控、电源键唤醒、真实 AI 听说、4G 和续航仍需实物验收。

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
