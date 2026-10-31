# 硬件联调七步清单

> 适用：软件设计已完成、第一次拿到 `STM32F407_V8` 开发板 + `HJ/Sensors V1` 九轴模块。
> 每一步都有**验收标准**，达不到就别往下走 —— 这样出问题能立刻定位在哪一层。

---

## 前置条件（开工前先确认）

| 项 | 怎么确认 |
|---|---|
| 板子能上电 | 按白色自锁开关，**`POWER LED`（L3）常亮** |
| `J6` 电源选择跳线正确 | 用适配器 → 帽子在 **`VIN`**；用 USB 供电 → 帽子在 **`VBUS`** |
| **一根 Mini-USB 数据线** | ⚠️ 最常见的坑：充电线（只有电源两芯）会让板子亮但电脑零反应 |
| 固件工程已编译 | `D:\STM32\stair_mcu\USER\stair_mcu.uvprojx`，`F7` 应 `0 Error(s)` |

**怎么判断线是不是数据线**：插到手机上，看有没有弹出「传输文件 / 仅充电」选项。只显示充电 = 充电线。

---

## 第 1 步：串口先通

```
USB 线 → 插板子左侧【中间】那个 Mini-USB 口（丝印 UART-USB）→ 另一头插电脑
```

**验收**：设备管理器 → 端口(COM 和 LPT) → 出现 **`USB-SERIAL CH340 (COM3)`**
→ **记下这个 COM 号**。

**打不开设备管理器时**，PowerShell 里查：

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name
```

**读串口**（任选一种）：

```powershell
# 方式 A：PowerShell（不装任何东西）
$p = New-Object System.IO.Ports.SerialPort COM3,115200,None,8,one
$p.Open()
while ($true) { $p.ReadLine() }
```

```
方式 B：串口助手（XCOM / SSCOM）
        115200 / 8 数据位 / 1 停止位 / 无校验
```

> **没有 CH340** → 线是充电线 / 插错口（插到 `JTAG` 或 `USB-Slave` 不会有 CH340）/ 驱动未装

---

## 第 2 步：Keil 下载（烧固件）

```
① USB 换插到左侧【最上面】那个 Mini-USB 口（丝印 JTAG）
② Keil 打开 D:\STM32\stair_mcu\USER\stair_mcu.uvprojx
③ 魔术棒(Alt+F7) → Debug 页 → Use: 选 ST-Link Debugger
④ 点 Settings → SW Device 里应出现 IDCODE（STM32F407）
⑤ 魔术棒 → Utilities 页 → 同样选 ST-Link Debugger
⑥ OK → 按 F8
```

> **三个小口的位置（从上到下）**：`JTAG` → `UART-USB` → `USB-Slave`

**验收**：`Build Output` 出现

```
Erase Done.
Programming Done.
Verify OK.
```

| 现象 | 原因 |
|---|---|
| `No ST-LINK detected` | 电脑没看到 ST-Link 硬件 → USB 线/口（不是芯片问题） |
| `No target connected` | ST-Link 找到了但连不上芯片 → 板子没上电 / SWD 线 |
| 弹窗问升级 ST-Link 固件 | **点"否"** —— 板载克隆 ST-Link 升级容易变砖 |

---

## 第 3 步：不接模块，先看串口启动是否正常

```
① USB 换回 UART-USB 口，串口助手保持打开
② 按一下板子的红色 RST 键
```

**验收**（**没接模块时应该是这样**）：

```
==== 楼梯层数计数（上楼）· 硬件采集端 ====
LSM9DS0  I2C: PB6=SCL  PB7=SDA   采样率 50 Hz
[ERR] 没找到九轴模块
      WHO_AM_I_G = 0xFF (期望 0xD4)
      WHO_AM_I_XM= 0xFF (期望 0x49)
```

同时 **LED0（PF9）以 5 Hz 快闪**。

> 这一步过了，说明**烧录、串口、电源、板子**四样都没问题。后面只剩接线。

---

## 第 4 步：接九轴模块

接线表见 **[wiring-lsm9ds0.md](wiring-lsm9ds0.md)**（含模块 14 针全表、板子接点位置、上电前万用表检查）。

速查版：

| 模块针 | 丝印 | 接到 |
|---|---|---|
| 1 / 9 | `GND` | 板 GND（电源开关下方 3 针排针的**第 2 针**） |
| 2 / 3 / 8 | `CS_G` / `CS_XM` / `3.3V` | 板 3.3V（同一排针的**第 3 针**） |
| 6 | `SCL` | **PB6** |
| 7 | `SDA` | **PB7** |
| 4,5,10~14 | `SDO_G` `SDO_XM` `DEN` `INT` `DRDY` `INT1M` `INT2M` | 空着 |

**验收**：按 `RST` 复位后串口变成

```
[OK] WHO_AM_I_G=0xD4  WHO_AM_I_XM=0x49
[OK] 开始采集：每 20ms 输出一行  gx gy gz ax ay az
64876 161 64597 65338 14289 63033
```

LED0 变成**常亮**（不再快闪）。

> ⚠️ 模块 `3.3V` 接板子 **5V** 会立刻烧模块。上电前必须用万用表确认。

---

## 第 5 步：读数是否合理（静置测试）

板子平放桌上别动，看串口六个数：

| 现象 | 判断 |
|---|---|
| 加速度某一路稳定在 **±16000 附近**，另两路接近 0 | ✅ 量程换算正确 |
| 陀螺三路约 **−660 / +170 / −954** 且基本不动 | ✅ 正常（零偏置，数据集里也是这样） |
| 六个数都在乱跳 | ❌ 线接触不良或模块坏 |
| 全是 0 | ❌ I2C 没读到数据 |

然后**拿起板子走两步**，加速度三路应明显大幅变化。

---

## 第 6 步：端到端 —— 数据进系统

**PC 侧开两个窗口：**

```powershell
# 窗口 1：服务端
cd D:\KS\upstairs-system
.\cmake-build-debug\server.exe

# 窗口 2：串口桥
cd D:\KS\upstairs-system
pip install pyserial
python tools\serial_ingest.py --port COM3 --device STM32-01 --user collector1
```

浏览器打开 <http://127.0.0.1:8080>，登录 `admin` / `admin123`。

**验收**：
- 设备列表出现 **`STM32-01`（真实设备）**
- 能看到 **三条实时波形**
- 拿板子走楼梯，页面上的**台阶数、楼层数实时增长**
- 页面显示 `陀螺：无效（σ≈0.06 °/s）` ← **正常且预期**，和数据集一致

---

## 第 7 步：真楼梯标定

按数据集采集时的姿势（[mounting.md](mounting.md) 图 1：单手握板、屏幕朝外），从 1 楼爬到 4 楼。

| 指标 | 数据集参考值 | 你的真机结果 |
|---|---|---|
| 上 3 层楼梯 | 68 级 | ? |
| 判出楼层 | 3 层 | ? |
| 步频 | 82 级/分 | ? |
| 陀螺 σ | ≈0.06（无效） | ? |

**判断：**
- 楼层对（3 层）→ 算法迁移成功，可以写报告
- 级数差很多（<50 或 >90）→ 调 `gait_algo.c` 里的 `cfg.step_thr_g`
- 陀螺 σ 上到 5 以上 → 好消息，可以做方向判别（见 `hardware-protocol.md` §8）

---

## 附录 A：卡住对照表

| 现象 | 大概率原因 |
|---|---|
| 板子不亮 | `J6` 跳线位置错 / 电源开关没按 / 适配器没插紧 |
| 板子亮但电脑零设备 | **USB 线是充电线**（最常见） / 插到 `USB HOST` 大口头 |
| 设备管理器没有 CH340 | 插错口（要 `UART-USB`）/ 线 / 驱动 |
| Keil `No ST-LINK detected` | 没插 `JTAG` 口 / 线是充电线 |
| 串口无输出 | 没烧进去 / 波特率不是 115200 / COM 号错 |
| 串口有 banner 但 `0xFF` | 模块没接 / `CS_G` `CS_XM` 没到 3.3V |
| 数据全 0 | SCL/SDA 接反 |
| 网页没数据 | `serial_ingest.py` 的 COM 号 / server 没起 |

## 附录 B：已经踩过的坑（省得重踩）

1. **USB 线是充电线** —— 板子能亮，电脑零反应。判据：插手机能不能传文件。
2. **大 USB 口是 `USB HOST`**（插 U 盘用），接电脑没反应。要接电脑用三个**小口**。
3. **`J6` 跳线插在 `VBUS` 时，插适配器不通电** —— 要换到 `VIN`。
4. **Keil 默认调试器是 ULINK2**，必须手动改成 `ST-Link Debugger`（`Debug` 和 `Utilities` 两页都要改）。
5. **正点原子模板的 `stm32f4xx.h` 在 `USER\` 里**，不是 `FWLIB\inc\`。新建工程漏拷会导致 `u8/u16/u32` 全部未定义（几百个错）。
6. **Keil 弹窗问升级 ST-Link 固件 → 点否**。
7. **Windows 下 `SO_REUSEADDR` 允许两个 `server.exe` 静默绑同一端口** —— 本项目已在 `a984682` 修复（改用 `SO_EXCLUSIVEADDRUSE`）。
