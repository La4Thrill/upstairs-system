# 硬件阶段上手：从烧录到看见数据

> 目标读者：软件设计已经做完、现在第一次拿到 STM32 + LSM9DS0 的人。
> 本文只讲"怎么让板子把数据送到你已经做好的服务端"，**服务端代码一行都不用改**。

---

## 一、先分清：哪些要烧进去，哪些不用

| 组成 | 跑在哪 | 要不要烧录 | 谁写的 |
|---|---|---|---|
| `server.exe`（含网页、数据库、`/api/ingest`） | **你的电脑** | **不用烧，双击/CLion Run 就行** | 你已经做好的（软件设计阶段） |
| 端侧固件（I2C 读传感器、定时器采样、串口/网络发送） | **STM32F407** | **要烧**，本文第 3 节 | 硬件阶段要写的 |
| `gait_algo.c/h` | **两边都能跑** | 想拿"单片机侧算"的加分就编进固件 | 已写好，直接复制 |

一句话：**"烧录"指的是把端侧固件烧进 STM32；你电脑上的服务端永远不烧。**

---

## 二、整体链路（数据往哪走）

```
LSM9DS0 ──I2C──► STM32F407 ──┬─ 串口(115200) ──► PC: tools/serial_ingest.py ──┐
  陀螺 @0x6A                 │                                              ├─► POST /api/ingest
  加速度+磁 @0x1D            └─ WiFi(ESP8266) ──────────────────────────────┘        │
                                                                                    ▼
                                                              server.exe（你已做好的）
                                                                    │
                                                          SQLite + 网页 + 算法
```

**建议第一周先走"串口 + `serial_ingest.py`"这条路**：端侧只要 `printf` 六个数，不用碰 WiFi、不用拼 JSON，当天就能看到网页上的波形和楼层数。等算法调通了再换 WiFi。

---

## 三、烧录具体怎么操作

### 3.1 接线（SWD 四根线）

| ST-Link | 开发板 | 说明 |
|---|---|---|
| `3.3V` | `3.3V` | **板子已经单独供电时不要接**，避免对灌 |
| `GND` | `GND` | 必须接 |
| `SWDIO` | `SWDIO`（PA13） | 必须接 |
| `SWCLK` | `SWCLK`（PA14） | 必须接 |
| `RST`（可选） | `NRST` | 认不到芯片时接上，勾"Connect under reset" |

> 大多数实验室板子自带 ST-Link（USB 直接插板子就行），那就不用外接。

### 3.2 三条路，任选一条

| 工具 | 操作 | 适合 |
|---|---|---|
| **Keil MDK** | 打开工程 → `Options for Target` 选 ST-Link Debugger → 点 **Download（F8）** 或 **Load** | 实验室最常见的 SPL 工程 |
| **STM32CubeIDE** | 选中工程 → 点 **Debug（虫子图标）/ Run（绿色三角）** | CubeMX + HAL 工程 |
| **STM32CubeProgrammer** | 选 ST-LINK → `Connect` → 载入 `.hex`/`.bin` → `Download` | 只有 hex、不想开 IDE |

命令行也可以（装了 STM32CubeProgrammer 就有）：

```powershell
STM32_Programmer_CLI.exe -c port=SWD -w build\firmware.hex -v -rst
```

### 3.3 怎么算"烧成功"

```
Erase done.
Programming done.
Verify OK.                       ← 关键
Application running ...
```

看到 `Verify OK` 就说明**已经烧进去了**。板子复位后应该立刻按新程序跑（LED 变了 / 串口有输出）。

### 3.4 烧不进去的排查顺序

| 现象 | 原因 | 解决 |
|---|---|---|
| `No target connected` / `Cannot access target` | SWD 线接错、板子没供电 | 万用表量 3.3V；检查 SWDIO/SWCLK 有没有接反 |
| 能连上但 `Flash download failed` | 芯片读保护 / 选了错的型号 | CubeProgrammer 里先 `Full chip erase`；确认型号是 **STM32F407** |
| 程序烧进去了但不跑 | BOOT0 被拉高 | BOOT0 接 GND，按一下复位 |
| 烧完串口没输出 | 波特率、USART 引脚、或 `printf` 没重定向 | 先用最简单的 `HAL_UART_Transmit` 循环发一个字节试 |

---

## 四、五步走，每步都有验收标准

**别一口气写完再调**，按这个顺序，每步都有一个"看见就说明通了"的判据。

| 步骤 | 做什么 | 验收标准 | 大概耗时 |
|---|---|---|---|
| **① 点灯** | 一个 LED 1 Hz 闪 | 灯真的按 1 Hz 闪 | 10 min |
| **② 串口打印** | 每 500 ms `printf("hello\r\n")` | PC 串口助手能看到 hello | 20 min |
| **③ 读 WHO_AM_I** | I2C 读 `0x0F` | 陀螺回 **0xD4**、加速度+磁回 **0x49** | 1~2 h（最卡的就在这） |
| **④ 配量程 + 读六轴** | 初始化 + 每 20 ms 打印 `gx gy gz ax ay az` | 静置时 `ay ≈ +16000`、`|a|` 合理；拿起板子数会大幅变化 | 2 h |
| **⑤ 50 Hz 定时器 + 上报** | TIM 中断采样，串口打印；PC 侧跑 `serial_ingest.py` | **网页上出现设备、波形、楼梯计数** | 半天 |

第 ④ 步完成时，你就已经能拿着板和数据集对话了：

```powershell
# 串口打出来的六个数，直接喂给离线算法工具
$port = New-Object System.IO.Ports.SerialPort COM3,115200
$port.Open()
while ($true) { $port.ReadLine() } | .\cmake-build-debug\gyro_check.exe - --every 200
```

---

## 五、寄存器配置（已按数据手册 DocID024763 Rev 2 核对）

### 5.1 两个 I2C 地址（一颗芯片两套，最容易错的地方）

| 子系统 | I2C 7 位地址 | WHO_AM_I (0x0F) 应回 |
|---|---|---|
| 陀螺（`_G` 系列寄存器） | **0x6A**（SA0_G=0）/ 0x6B | **0xD4** |
| 加速度 + 磁力计（`_XM` 系列） | **0x1D**（SA0_XM=0）/ 0x1E | **0x49** |

> 前置条件：`CS_G` 与 `CS_XM` **都为高**（I2C 模式）。板上如果没上拉，要自己接 VDD。

### 5.2 初始化序列（写这 4 个寄存器就够）

| 设备 | 寄存器 | 值 | 含义（数据手册出处） |
|---|---|---|---|
| 陀螺 | `CTRL_REG1_G (0x20)` | **0x0F** | `DR=00 BW=00` → ODR **95 Hz**（Table 21）；`PD=1` 正常模式；`Zen=Xen=Yen=1` 三轴使能 |
| 陀螺 | `CTRL_REG4_G (0x23)` | **0x80** | `BDU=1`（读完高低字节前不更新）；`FS=00` → **±245 dps**（Table 30） |
| 加速度 | `CTRL_REG1_XM (0x20)` | **0x6F** | `AODR=0110` → **100 Hz**（Table 72）；`BDU=1`；`AZEN/AYEN/AXEN=1` |
| 加速度 | `CTRL_REG2_XM (0x21)` | **0xC0** | `ABW=11` → 抗混叠 **50 Hz**（Table 75）；`AFS=000` → **±2 g**（Table 76） |

对应灵敏度（`gait_algo.c` 里的两个宏就是这么来的）：

- 加速度 ±2 g → **0.061 mg/LSB** ≈ **16384 LSB/g**
- 陀螺 ±245 dps → **8.75 mdps/LSB** ≈ **114.29 LSB/(°/s)**

> 陀螺 ODR 只有 95/190/380/760 Hz 四档，没有 50 Hz，所以配 95 Hz、我们每 20 ms 读一次（Nyquist 够）。
> 加速度配 100 Hz 而不是 50 Hz，是为了避免"ODR 和采样率完全相等"时出现读到重复样本/漏样本。

### 5.3 读数据

| 数据 | 起始寄存器 | 长度 | 备注 |
|---|---|---|---|
| 陀螺 XYZ | `OUT_X_L_G (0x28)` | 6 字节 | 自动递增：地址要写 **`0x28 | 0x80`** |
| 加速度 XYZ | `OUT_X_L_A (0x28)` | 6 字节 | 自动递增：地址要写 **`0x28 | 0x80`** |

拼成有符号 16 位：`int16_t v = (int16_t)((uint16_t)hi << 8 | lo);`
（和数据集 txt 里的数一致——`65338` 其实就是 `-198`。）

---

## 六、端侧最小代码骨架

### 6.1 HAL（CubeMX 工程）

```c
#define GYRO_ADDR   0x6A
#define XM_ADDR     0x1D

static uint8_t who_g, who_xm;

static void lsm_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    HAL_I2C_Mem_Write(&hi2c1, addr << 1, reg, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

static void lsm_read(uint8_t addr, uint8_t reg, uint8_t* buf, uint16_t len)
{
    HAL_I2C_Mem_Read(&hi2c1, addr << 1, reg, I2C_MEMADD_SIZE_8BIT, buf, len, 100);
}

/* 返回 0 表示芯片都在线 */
int lsm9ds0_init(void)
{
    lsm_read(GYRO_ADDR, 0x0F, &who_g, 1);      /* WHO_AM_I_G  -> 0xD4 */
    lsm_read(XM_ADDR,   0x0F, &who_xm, 1);     /* WHO_AM_I_XM -> 0x49 */
    if (who_g != 0xD4 || who_xm != 0x49) {
        return -1;
    }

    lsm_write(XM_ADDR,   0x20, 0x6F);          /* 100 Hz + BDU + 三轴使能 */
    lsm_write(XM_ADDR,   0x21, 0xC0);          /* ABW 50Hz, ±2 g         */
    lsm_write(GYRO_ADDR, 0x23, 0x80);          /* BDU, ±245 dps          */
    lsm_write(GYRO_ADDR, 0x20, 0x0F);          /* 95 Hz, 正常模式, 三轴  */
    return 0;
}

/* 读一组六轴原始值 */
void lsm9ds0_read(int16_t out[6])
{
    uint8_t b[6];
    lsm_read(GYRO_ADDR, 0x28 | 0x80, b, 6);
    out[0] = (int16_t)((uint16_t)b[1] << 8 | b[0]);   /* gx */
    out[1] = (int16_t)((uint16_t)b[3] << 8 | b[2]);   /* gy */
    out[2] = (int16_t)((uint16_t)b[5] << 8 | b[4]);   /* gz */

    lsm_read(XM_ADDR, 0x28 | 0x80, b, 6);
    out[3] = (int16_t)((uint16_t)b[1] << 8 | b[0]);   /* ax */
    out[4] = (int16_t)((uint16_t)b[3] << 8 | b[2]);   /* ay */
    out[5] = (int16_t)((uint16_t)b[5] << 8 | b[4]);   /* az */
}
```

### 6.2 标准外设库（正点原子风格的 SPL 工程）

把上面两个 I2C 函数换成 SPL 版本，寄存器逻辑完全一样：

```c
uint8_t lsm_read_reg(uint8_t addr, uint8_t reg)
{
    I2C_GenerateSTART(I2C1, ENABLE);
    while (!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));
    I2C_Send7bitAddress(I2C1, addr << 1, I2C_Direction_Transmitter);
    while (!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));
    I2C_SendData(I2C1, reg);
    while (!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));
    I2C_GenerateSTART(I2C1, ENABLE);
    while (!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));
    I2C_Send7bitAddress(I2C1, addr << 1, I2C_Direction_Receiver);
    while (!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED));
    I2C_AcknowledgeConfig(I2C1, DISABLE);
    I2C_GenerateSTOP(I2C1, ENABLE);
    while (I2C_GetFlagStatus(I2C1, I2C_FLAG_RXNE) == RESET);
    return I2C_ReceiveData(I2C1);
}
```

### 6.3 串口输出（第 ④ 步就能用）

```c
/* 50 Hz 定时器中断里置标志，主循环打印，别在中断里 printf */
if (s_tick20ms) {
    s_tick20ms = 0;
    int16_t v[6];
    lsm9ds0_read(v);
    printf("%d %d %d %d %d %d\r\n", v[0], v[1], v[2], v[3], v[4], v[5]);
}
```

> `printf` 要能出串口，Keil 里要重定向 `fputc`，GCC 里通常需要 `-u _printf_float` 之类的链接选项（这里不用打印浮点，省事）。

---

## 七、把数据送进你已有的系统（三选一）

### 方案 A（推荐，第一天就能跑通）：串口 + `tools/serial_ingest.py`

端侧只管 `printf` 六个数，PC 侧：

```powershell
# 先起服务端
.\cmake-build-debug\server.exe

# 另开一个窗口
pip install pyserial
python tools\serial_ingest.py --port COM3 --baud 115200 --device STM32-01 --user collector1
```

网页上立刻能看到 `STM32-01` 这台"真实设备"的实时波形、台阶数、楼层。

> 不接板子也能验证这条链路：
> `Get-Content 上三层楼数据.txt | python tools\serial_ingest.py --port -`
> 应该输出"已爬 3 层 / 68 级台阶 / 中间平台 3"。

### 方案 B：ESP8266 直连（最终形态）

AT 指令发 HTTP POST，报文格式见 [`hardware-protocol.md` 第 2 节](hardware-protocol.md#2-报文格式)。
注意 `fs` 必须填端侧真实采样率——服务端会按它换算不应期。

### 方案 C：蓝牙连手机

同上，只是把 HTTP 换成手机侧 App 转发；课程里如果只做网页版，可以先不折腾。

---

## 八、服务端这边你已经不用改

已经具备、直接可用：

| 能力 | 接口 / 位置 |
|---|---|
| 设备自动登记 + 密钥 | 第一次 POST `/api/ingest` 就返回 `device_key` |
| 多终端接入 | 最多 16 台（`MAX_DEVICES`），每台独立算法状态 |
| 绑定采集对象 | 报文里带 `"user":"collector1"` |
| 原始数据落库 | `raw_samples` 表，网页可回看 |
| 实时波形 / 楼层 / 中间平台 | 网页 + `GET /api/status` |
| 算法可移植到端侧 | `gait_algo.c` 无 malloc、无平台依赖 |

---

## 九、常见坑（按踩到的概率排序）

1. **I2C 没上拉 / 片选没拉高** → 扫描不到设备。先量 `CS_G`、`CS_XM` 是不是高电平，`SCL`/`SDA` 有没有 4.7k 上拉。
2. **地址搞混**：陀螺 **0x6A**、加速度+磁 **0x1D**，不是同一个！读加速度却用 0x6A 是最常见的错。
3. **`HAL_I2C_*` 的地址参数要左移一位**（`addr << 1`），而 `HAL_I2C_IsDeviceReady` 也是 8 位地址。
4. **量程配错一倍**：CubeMX 里配了 ±4 g，`acc_lsb_per_g` 就得从 16384 改成 8192，否则幅度全错。
5. **采样率不是真 50 Hz**：用 `HAL_Delay(20)` 会漂；要用定时器中断。而且报文里的 `fs` 必须是真的（服务端按它换算不应期）。
6. **多字节读忘了 `| 0x80`**：LSM9DS0 的自动递增要置地址最高位，否则六个字节读回来是一样的。
7. **在中断里 `printf`**：串口阻塞会把定时器节奏拖垮。中断里只置标志、存缓冲，主循环再打印。
8. **手持姿势变了**：数据集是"手持开发板"录的，换成腰绑/口袋必须重标阈值（见 [`mounting.md`](mounting.md)）。
9. **同时开了两个 `server.exe`**：老版本会静默双绑 8080（已在 `a984682` 修掉，现在会明确报 `bind failed`）。
