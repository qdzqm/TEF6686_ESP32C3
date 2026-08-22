<img width="837" height="625" alt="TEF6686_ESP32C3" src="https://github.com/user-attachments/assets/49c40df3-86ea-4bf5-95fd-ff13d17d7c1f" />

# TEF6686_ESP32C3 数字收音机

拼多多收音机，大家自己搜索，原程序操作不太方便，用AI编写了一个操作方便的版本。
基于 **ESP32-C3** 与 **NXP TEF6686** 的便携式数字收音机，配备 **ILI9341 (320x240, SPI)** 彩色屏幕与旋转编码器交互界面。

## 功能特性

- **三波段接收**
  - **FM**：87.5 – 108.0 MHz（0.1 MHz 步进，支持 SEEK 100kHz / SEEK 50kHz）
  - **MW / AM**：531 – 1710 kHz（9 kHz / 1 kHz 步进）
  - **SW 短波**：2300 – 27000 kHz（5 kHz / 500 kHz 步进）
- **双模式调谐**
  - **手动调台**：旋转编码器逐级步进微调
  - **自动搜台（SEEK）**：一键向上/向下搜索电台并落台
- **清晰界面**：大号六段数码字体显示频率，顶部波段高亮、底部功能标签（SEEK/TUNE 及步进），实时信号强度与立体声/单声道指示。
- **操作交互**：旋转编码器调整，按键支持**单击**（切换步进/模式）、**双击**（重设频率）、**长按**（切换波段）。

## 硬件要求

| 部件         | 说明                              |
|--------------|-----------------------------------|
| 主控         | ESP32-C3（WeAct Studio Core Board）|
| 收音机前端   | NXP TEF6686（含 9.216 MHz 晶振）  |
| 显示屏       | ILI9341 SPI 320×240               |
| 状态存储     | I2C EEPROM（如 AT24C64/AT24C32）  |
| 输入         | 旋转编码器 + 按键                 |

## 引脚分配

| 功能     | ESP32-C3 GPIO |
|----------|---------------|
| 编码器 A | GPIO3         |
| 编码器 B | GPIO1         |
| 按键     | GPIO2         |
| TFT RST  | GPIO20        |
| TFT DC   | GPIO21        |
| TFT CS   | GPIO7         |
| 收音机电源使能 | GPIO0    |
| I2C (SDA/SCL) | TEF6686 与 EEPROM 共用总线 |

> 具体引脚请以 `radio.ino` 顶部的 `#define` 为准。

## 状态记忆（磨损均衡存储）

- 开机自动恢复**上次**保存的状态：`当前波段 / 频率 / 步进 / 模式 / 底栏选中项`。
- 存储使用 I2C **EEPROM**（代码针对 24C64 设计），并实现**均匀磨损（wear leveling）**：
  - 整片 8192 字节划分为 128 个槽，数据**轮询写入下一个槽**，避免反复写同一地址而造成提前报废。
  - 每个槽带魔数、版本、**顺序号**与**校验和**；上电时自动选择校验通过且"最新"的记录，遇到写入一半断电的损坏槽会自动跳过。
- 为避免手动调台时频繁擦写：**手动调台 / 搜台落台后需频率稳定 10 秒**才写入 EEPROM 一次；波段切换、步进切换等明确操作则立即保存。

## 目录结构

```
.
├── platformio.ini          # PlatformIO 工程配置
├── src/
│   ├── radio.ino           # 主程序：界面、主逻辑、状态
│   ├── StateStore.h        # EEPROM 磨损均衡状态存储
│   ├── TEF6686.cpp/.h       # 收音机 API 封装
│   ├── Tuner_*.cpp/.h       # TEF6686 驱动 / 接口 / 补丁
│   └── Tuner_Patch_*.h      # 收音机固件补丁（p224 / p209 版本）
└── backup/                  # 历史备份版本
```

## 编译与烧录

使用 **PlatformIO**：

```bash
# 编译
pio run

# 烧录（替换为实际串口）
pio run --target upload --upload-port COM6

# 串口监视（115200 波特）
pio device monitor -p COM6 -b 115200
```

> 若启动后无声音/信号：优先检查 TEF6686 模块的 **9.216 MHz 晶振**是否起振、供电是否充足、以及 I2C 走线/上拉。

## 参考

- [NXP TEF6686 数据手册](https://www.nxp.com/)
- [ESP32-C3](https://www.espressif.com/en/products/socs/esp32-c3)
