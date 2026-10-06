# ESP32-C3 TEF6686 收音机（中断输入分支）

基于 WeAct Studio ESP32-C3 核心板与 NXP TEF6686（Lithio）车规收音芯片的
多波段收音机固件。本分支将旋钮编码器与按键改为**中断方式**处理，并把搜台
重写为**非阻塞状态机**，搜台过程中界面与按键全程保持响应。

> 分支：`feature/interrupt-encoder-button`
> 基线：`e706893`（第一次提交，基本功能）
> 开发环境：Linux + VSCode / PlatformIO，由 Codex 辅助编码。

## 功能

- 三个波段：FM 87.5–108 MHz、MW 531–1710 kHz、SW 2.3–27 MHz。
- 旋转编码器调谐 / 搜台，一个按键完成模式切换、波段切换与重新应用频率。
- ILI9341 320×240 TFT 显示：大字号频率、信号强度条、立体声状态。
- 搜台非阻塞：搜台中可随时转动旋钮或按下按键立即中止。
- 串口调试输出（115200）：启动信息、搜台开始 / 步进 / 完成 / 中止。

## 硬件与接线

| 模块 | 引脚（ESP32-C3） | 说明 |
| --- | --- | --- |
| 编码器 A 相 | GPIO3 | CHANGE 中断 |
| 编码器 B 相 | GPIO1 | CHANGE 中断 |
| 按键 | GPIO2 | FALLING 中断，内部上拉 |
| TFT RST | GPIO20 | 硬件 SPI |
| TFT DC | GPIO21 | 硬件 SPI |
| TFT CS | GPIO7 | 硬件 SPI |
| TEF6686 | I2C 地址 0x64 | 时钟 400 kHz，另有硬件 SPI 驱动屏幕 |

## 操作方式

- **旋转编码器**：
  - 搜台模式：转动超过 4 格触发一次向上 / 向下自动搜台，找到电台后锁定。
  - 调谐模式：转动超过 4 格，频率按当前步进增减一次。
- **单击**：在当前波段的三种模式间循环。
  - FM：`SEEK_100K` → `SEEK_50K` → `TUNE`
  - MW：`TUNE_9K` → `TUNE_1K` → `SEEK`
  - SW：`TUNE_5K` → `TUNE_500K` → `SEEK`
- **双击**：按当前模式重新应用当前频率。
- **长按（>0.8s）**：FM → MW → SW 循环切换波段。
- **搜台过程中**：转动旋钮或按下按键立即中止搜台；其中按键中止会吞掉
  本次按压，不会切换搜索 / 调谐模式。

## 技术设计

### 中断输入

- 编码器 A、B 两相均注册 CHANGE 中断，中断内做 5ms 软件防抖，只有有效
  边沿才更新计数，避免机械抖动误触发。
- 按键注册 FALLING（按下沿）中断，搜台中按下即刻发出中止请求，无需等待
  松手和双击判定窗口。
- 搜台启动设有“武装”（armed）机制：触发搜台的同一次旋转的残余边沿，在
  旋钮静止满 `SEEK_ENCODER_GRACE`（100ms）之前被忽略；武装之后新的旋转
  或按键才能中止搜台，解决了搜台刚启动就被误中止的问题。

### 非阻塞搜台

原先 `FMSeek / MWSeek / SWSeek` 的 `while (true)` 阻塞循环合并为一个
`SeekJob` 状态机，每次 `loop` 通过 `seekTick()` 只推进一步：

`频率步进(20) → 等待信号稳定并检测(30) → 结果判定(40) → 锁定频率(50)`

MW 搜台前先将频率对齐 9kHz 频道栅格，步进由 1kHz 改为 9kHz，扫完整个
中波波段由约 2 分钟缩短到约 15 秒。

### 其他修复

- `Radio_SetFreq` 边界钳制改用**目标波段**范围校验（原来用旧波段，跨波段
  切换会被钳到错误频率），移除了 MW/SW “调两次”的变通写法。
- 区域配置由 `Radio_EUR`（六波段表）改为 `Radio_CHN`（FM/MW/SW 三波段），
  与项目 `MaxBandNum = 3` 的波段定义一致。
- 切换波段时强制重绘，避免 FM 与 SW 数值相同导致显示缓存误命中。
- `init()` 改为有界轮询，`Tuner_Init()` 失败重试加上限，`setVolume` 参数
  类型修正为 `int16_t`。

### 已移除

- WiFi 连接与 NTP 授时（原实现连不上 WiFi 会永久阻塞启动）。
- RDS 解析相关代码，芯片初始化表中一并关闭 RDS。
- 不再使用的阻塞式 seek/tune 死代码。

## 工程结构

```
src/
├── radio.ino                 主程序：界面、交互、搜台状态机
├── TEF6686.cpp / .h          TEF6686 C++ 封装
├── Tuner_Api.cpp / .h        上层 Radio API
├── Tuner_Drv_Lithio.cpp / .h 底层 I2C 命令驱动
├── Tuner_Interface.cpp / .h  I2C 总线与固件 patch 加载
└── Tuner_Patch_*.h           芯片固件 blob
```

## 编译与烧录

本工程使用 PlatformIO（目标板 `weactstudio_esp32c3coreboard`，Arduino 框架）：

```bash
pio run            # 编译
pio run -t upload  # 烧录
pio device monitor # 串口监视器（115200）
```

显示依赖库 `gavinlyonsrepo/ILI9341_LTSM`（已在 `platformio.ini` 声明，
编译时自动下载）。

## 说明

- MW 对天线敏感，若中波搜台异常先检查天线连接与摆放。
- 若觉得搜台中止的武装时间不合适，可调整 `src/radio.ino` 中的
  `SEEK_ENCODER_GRACE`。
