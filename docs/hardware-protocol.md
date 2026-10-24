# 硬件对接协议（STM32 ↔ 服务端）

面向课程设计阶段：把 `LSM9DS0 九轴 + STM32F407 + WiFi/蓝牙模块` 接进现有系统。
**服务端与前端不需要再改代码**——设备只要按下面的报文上报，就会自动出现在网页的设备列表里。

---

## 1. 一句话流程

```
STM32 采集 (50Hz, int16 原始计数)
   └─► 每 100 个样本打包一次 JSON
        └─► HTTP POST http://<PC_IP>:8080/api/ingest
             └─► 服务端跑同一套 gait_algo（台阶/平台/楼层）
                  └─► 网页实时显示 + SQLite 落库（raw_samples / progress_snapshots / floor_events）
```

服务端**不在服务端做提示音**，而是把事件放在响应里返回，由端侧蜂鸣器/LED 决定怎么响
（这样"到达楼层""中间平台""每 15 级"的不同提示音是在设备上响的，符合课程设计"端侧计算与显示加分"的要求）。

---

## 2. 报文格式

### 请求

```http
POST /api/ingest HTTP/1.1
Host: 192.168.1.100:8080
Content-Type: application/json
X-Device-Key: <设备密钥，首次上报可省略>
Content-Length: ...

{
  "device_id": "STM32-01",
  "name": "真实终端01",
  "fs": 50,
  "unit": "LSB@2g/245dps",
  "user": "collector2",
  "samples": [
    [gx, gy, gz, ax, ay, az],
    ...                     // 一批 1~1000 个样本，建议 100 个（@50Hz = 2 s）
  ]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `device_id` | 是 | 设备唯一标识，建议 `STM32-01` 这种；出现在网页设备列表 |
| `samples` | 是 | 二维数组，每行 6 个**有符号 16 位整数**：`gx gy gz ax ay az` |
| `fs` | 否 | 采样率，默认 50（与数据集一致） |
| `unit` | 否 | 仅作记录，默认 `LSB@2g/245dps` |
| `name` | 否 | 显示名 |
| `user` | 否 | 绑定采集对象（`collector1` / `collector2`）。不填则该设备只有 admin/monitor 可见 |

> **关键约束：`samples` 必须是原始 LSB 计数，不要先换算成 g 再发。**
> 服务端按 `LSM9DS0 ±2g → 16384 LSB/g`、`±245dps → 114.29 LSB/(°/s)` 换算。
> 如果端上改了量程（比如 ±4g），要么改服务端 `gait_algo.c` 的 `acc_lsb_per_g`，要么在协议里加 `range` 字段——**不要两边都换**，否则数值会差 2 倍。

### 响应（HTTP 200）

```json
{
  "status": "ok",
  "device_id": "STM32-01",
  "device_key": "xb68ogum57ijktvpjoukp9ja",   // 仅首次上报时返回，务必存到 Flash/EEPROM
  "provisioned": true,                         // 仅首次上报时为 true
  "accepted": 100,
  "events": ["half", "steps15"],
  "current_floor": 2,
  "climbed_floors": 1,
  "steps": 22,
  "speed": 84.5,
  "half_flights": 2
}
```

`events` 语义（端侧据此驱动蜂鸣器/LED）：

| 事件 | 触发条件 | 建议端侧动作 |
|---|---|---|
| `half` | 到达中间平台（每 11 级台阶） | 短"滴"一声（提示音 1） |
| `floor` | 到达一个完整楼层（每 22 级台阶） | "滴滴"两声 + LED 闪 |
| `pause` | 检测到真实停顿（平台站定 ≥0.6 s） | 可不响，或柔和提示 |
| `steps15` | 累计每 15 级台阶 | **别的题目的遗留**，本课题要求是"每级楼梯一声"，可不实现；别在报告里写成本题要求 |
| `top` | 到达 99 层上限 | 长鸣 |

> **注意 1：`fs` 会参与换算。** 服务端按报文里的 `fs` 把"不应期 300 ms / 重力低通 /
> 滑动平均 100 ms"换算成样本数（`gait_apply_fs()`）。端侧定时器是 40 Hz 还是 100 Hz
> 都可以，但**报文里的 `fs` 必须是真的**——填错比不填更糟。
>
> **注意 2：`half` 只出现在"纯半层平台"。** 到层那一级台阶同时满足 `steps % 11 == 0` 和
> `steps % 22 == 0`，服务端只发 `floor`，不发 `half`。上 3 层楼一共 6 个梯段：
> `half` 3 次 + `floor` 3 次。响应里的 `half_flights` 是梯段总数（6），
> `/api/status` 另有 `mid_platforms` 表示真正的半层平台（3）。

### 错误码

| HTTP | `error` | 原因 | 端侧处理 |
|---|---|---|---|
| 403 | `invalid_device_key` | 设备已登记但密钥不对 | 停止上报并报警（或重新配网） |
| 400 | `missing_fields` / `invalid_json` | 报文缺字段/JSON 语法错 | 检查拼装，丢弃本批 |
| 503 | `no_free_channel` | 服务端设备通道满（默认 16） | 联系管理员 |
| 404 | — | 路径写错 | 检查 URL |

---

## 3. 鉴权流程（首次自动注册）

1. 设备第一次上报，**不带** `X-Device-Key` → 服务端自动登记并返回 `device_key`（24 字符）。
2. 端侧把 `device_key` 存进 Flash/EEPROM。
3. 之后每次上报都带 `X-Device-Key: <key>`；密钥不匹配返回 403。

> 现有实现里密钥是明文存 SQLite 与端侧 Flash 的，够课程演示用；要做得更严谨就换成"设备证书 + HMAC 签名"，那已经超出本课范围。

---

## 4. 端侧参考实现

### 4.1 ESP8266 / ESP32 直连（推荐，最简单）

```c
/* 每 100 个样本调用一次 */
static int16_t s_buf[100][6];
static int s_n = 0;

void upload_batch(void) {
    char json[4096];
    int len = snprintf(json, sizeof(json),
        "{\"device_id\":\"STM32-01\",\"fs\":50,\"samples\":[");
    for (int i = 0; i < s_n; ++i) {
        len += snprintf(json + len, sizeof(json) - len,
            "%s[%d,%d,%d,%d,%d,%d]", i ? "," : "",
            s_buf[i][0], s_buf[i][1], s_buf[i][2],
            s_buf[i][3], s_buf[i][4], s_buf[i][5]);
    }
    snprintf(json + len, sizeof(json) - len, "]}");

    /* AT 指令方式 */
    at_send("AT+CIPSTART=\"TCP\",\"192.168.1.100\",8080\r\n");
    at_send_fmt("AT+CIPSEND=%d\r\n", (int)strlen(json) + 120);
    /* 先发头部 */
    uart_puts("POST /api/ingest HTTP/1.1\r\nHost: 192.168.1.100:8080\r\n");
    uart_puts("Content-Type: application/json\r\n");
    uart_puts("Connection: close\r\n");
    uart_printf("X-Device-Key: %s\r\n", g_device_key);
    uart_printf("Content-Length: %d\r\n\r\n", (int)strlen(json));
    uart_puts(json);

    s_n = 0;
}
```

### 4.2 纯 STM32（无 WiFi，用串口转 WiFi 模块透传）

把上面那串 HTTP 文本直接写进串口即可（模块工作在 TCP Client 透传模式），
只要保证 `Content-Length` 与实际 body 字节数一致。

### 4.3 采样与打包建议

- **采样频率必须稳定在 50 Hz**：用定时器中断触发读取，不要用 `HAL_Delay`。
- 每 2 s 打包 100 个样本（一个 batch），网络抖动时不上报过快。
- 断网重传：本地用环形缓冲保存最近 500 个样本，失败后按 5 s 间隔重试 3 次。
- 时间戳：服务端以"收到时刻 + 采样间隔"推算样本时间；如果需要精确时间，可以在报文里加 `"ts": <unix 秒>`（当前版本预留，未使用）。

---

## 5. 验证命令（不用写代码就能测）

```powershell
# 1) 首次上报：返回 device_key
curl.exe -X POST http://127.0.0.1:8080/api/ingest ^
  -H "Content-Type: application/json" ^
  -d "{\"device_id\":\"STM32-99\",\"fs\":50,\"samples\":[[64876,161,64597,65338,14289,63033],[64885,173,64582,65472,14418,62487]]}"

# 2) 带密钥上报
curl.exe -X POST http://127.0.0.1:8080/api/ingest ^
  -H "Content-Type: application/json" -H "X-Device-Key: <上一步的 key>" ^
  -d "{\"device_id\":\"STM32-99\",\"fs\":50,\"samples\":[[...]]}"

# 3) 查看设备与状态（需要登录 token）
curl.exe http://127.0.0.1:8080/api/devices -H "X-Auth-Token: <token>"
curl.exe "http://127.0.0.1:8080/api/status?device_id=STM32-99" -H "X-Auth-Token: <token>"
```

PowerShell 里更方便的做法：

```powershell
$r = Invoke-RestMethod -Uri 'http://127.0.0.1:8080/api/ingest' -Method Post `
       -ContentType 'application/json' -Body $json
$r.device_key   # 第一次才有
```

---

## 6. 硬件阶段必须先确认的三件事

1. **陀螺到底出不出数。**
   现有数据集里三轴角速度偏置在 ±10 dps 规格内、但 σ 只有 0.061 °/s（详见 §7.3），
   所以算法只用加速度。真机上直接跑 `gyro_check.exe` 验证：

   ```powershell
   # 一边在串口打印 gx gy gz ax ay az（每行 6 个整数），一边验证
   $port = New-Object System.IO.Ports.SerialPort COM3,115200
   $port.Open()
   while ($true) { $port.ReadLine() } | .\gyro_check.exe - --every 200
   ```

   - 输出 `【陀螺无效】` → 先查量程配置（`CTRL_REG4`）、使能位（`CTRL_REG1`）、接线；
   - 输出 `【陀螺有效】` → 再做方向判别/平台转弯检测；
   - 服务端也会实时统计：`/api/status` 的 `gyro_std` / `gyro_valid`，网页上直接显示"陀螺：有效/无效"。
2. **量程与灵敏度。** LSM9DS0 加速度 ±2g / 角速度 ±245dps 对应 16384 LSB/g 与 114.29 LSB/dps。
   如果 CubeMX 里配了 ±4g，服务端换算会差 2 倍。
3. **采样率。** 定时器 20 ms 一次，实测用串口打印时间戳核对；50 Hz 是楼层判定的基准，
   偏到 40 Hz 就把 `SAMPLE_INTERVAL_MS` 和 `fs` 一起改。

---

## 7. LSM9DS0 配置对照（已与官方数据手册 DocID024763 Rev 2 核对）

> 这一节是拿到 `LSM9DS0.pdf` 之后核过的，可以直接写进设计报告的"传感器特性"章节。

### 7.1 两套寄存器映射（容易踩的第一个坑）

LSM9DS0 是"一颗芯片、两套地址"：

| 子系统 | 寄存器前缀 | I2C 从地址 | WHO_AM_I |
|---|---|---|---|
| 加速度计 + 磁力计 | `_XM`（如 `CTRL_REG1_XM`、`OUT_X_L_A`） | **0x1D**（SA0_XM=0）/ 0x1E（SA0_XM=1） | `WHO_AM_I_XM (0Fh)` |
| 陀螺仪 | `_G`（如 `CTRL_REG1_G`、`OUT_X_L_G`） | **0x6A**（SA0_G=0）/ 0x6B（SA0_G=1） | `WHO_AM_I_G (0Fh)` |

所以"读加速度"和"读陀螺"是两个不同的 I2C 从机地址；`CS_G` 和 `CS_XM` 两个片选都要拉高才是 I2C 模式。

### 7.2 量程与灵敏度（`gait_algo.c` 的默认值就对应第一行）

| 传感器 | 量程 | 灵敏度 | LSB / 物理单位 | 代码里对应的宏 |
|---|---|---|---|---|
| 加速度 | **±2 g** | **0.061 mg/LSB** | **≈16384 LSB/g** | `acc_lsb_per_g = 16384` ✅ |
| 加速度 | ±4 / ±6 / ±8 / ±16 g | 0.122 / 0.183 / 0.244 / 0.732 mg/LSB | 8192 / 5461 / 4098 / 1366 | 改量程就要改这个宏 |
| 陀螺 | **±245 dps** | **8.75 mdps/LSB** | **≈114.29 LSB/(°/s)** | `gyro_lsb_per_dps = 114.285` ✅ |
| 陀螺 | ±500 / ±2000 dps | 17.5 / 70 mdps/LSB | 57.14 / 14.29 | 改量程就要改这个宏 |
| 磁力计 | ±2 / ±4 / ±8 / ±12 gauss | 0.08 / 0.16 / 0.32 / 0.48 mgauss/LSB | — | 本课题未使用 |

### 7.3 陀螺"没出数"这件事，现在可以说清楚

数据手册给的零速率偏置（zero-rate level）规格：

| 量程 | 零速率偏置（典型） |
|---|---|
| ±245 dps | **±10 dps** |
| ±500 dps | ±15 dps |
| ±2000 dps | ±25 dps |

数据集实测角速度是 **-5.77 / +1.49 / -8.35 dps，σ = 0.061 dps**：

- 三个轴的偏置**全部落在 ±10 dps 规格内** → 说明**量程和换算系数是对的**（114.29 LSB/dps 没问题）；
- 但 σ 只有 0.061 dps，而爬三层楼这 50 s 里器件必然出现 20~50 dps 的转动 → **这 50 秒陀螺没有测到真实转动**；
- 结论：**不是换算错，是数据源/使能的问题**。硬件阶段按这个顺序查：
  1. `CTRL_REG1_G (20h)` 的 `Xen / Yen / Zen`（bit 2:0）是否置 1，`DR / BW` 是否配成 50 Hz；
  2. `CTRL_REG4_G (23h)` 的 `FS1:0`：`00` = ±245 dps；
  3. 是否误把 `CS_G` 接地进了 SPI 模式，或 I2C 地址用错（陀螺是 0x6A，不是 0x1D）；
  4. 拿板子真转一下，跑 `gyro_check.exe` 看 σ 会不会跳到 5 dps 以上。

### 7.4 ⚠️ 别抄 `传感器实验指导书.doc` 里"实验三十/三十一"的换算

实验指导书里的九轴部分是 **MPU6050 + HMC5883L**，不是 LSM9DS0，两处数值对不上：

| 项 | 指导书（MPU6050） | 本课题（LSM9DS0） | 差多少 |
|---|---|---|---|
| 陀螺量程 | `*2000*Pi/180/32767` → ±2000 dps | ±245 dps | **换算差 8.2 倍** |
| 加速度 | `/32767*2*9.8` → ±2 g | 0.061 mg/LSB → ±2 g | 一致（都是 16384 LSB/g） |
| 磁力计 | `*1.3/2048` | 0.08 mgauss/LSB | 不同 |
| 数据帧顺序 | **acc(x,y,z) → gyro(x,y,z) → mag(x,y,z)**，帧头 `88AF1C`，每帧 64 个 hex 字符 | 数据集 txt 是 `gx gy gz ax ay az` | **顺序相反** |

**结论：加速度可以照抄（都是 ±2g / 16384），陀螺绝对不能照抄**——照抄的话所有角速度会差 8 倍多。
数据集 txt 的表头 `gx gy gz ax ay az` 与数据本身是自洽的（前三列变化极小 = 陀螺偏置，后三列大幅起伏 = 加速度），
所以现有代码的读法是**对的**。

### 7.5 指导书里现成能用的四块东西

`传感器实验指导书.doc` 后半段是"算法"章节，正好是本课题需要的，可直接引用/复用：

| 指导书实验 | 内容 | 在本课题里的用处 |
|---|---|---|
| 实验三十二 | **中值滤波**（`GetMedianNum`） | 预处理备选方案，可与现有"低通 + 滑动平均"做对比 |
| 实验三十三 | **低通滤波**（FIR + 巴特沃兹） | 现有 `dyn` 的去噪就是低通，可引它作为理论依据 |
| 实验三十四 | **手势数据截取**：用**前 10 点方差** + 阈值判断"有动作 / 无动作" | ⭐ **这就是 P0-1"非爬楼判别"的现成思路**：先用方差/RMS 阈值把"静止/非运动"和"运动"分开，再谈楼梯结构 |
| 实验三十 / 三十五 | **四元数姿态解算**（`IMUupdate`，Kp=200 / Ki=0.008 / halfT）+ 手势特征提取 | ⭐ 硬件阶段做"竖直加速度投影 / 单步位移"可直接复用这套 AHRS 代码，而且是老师教材里的，天然被认可 |

> 把"实验三十四的方差阈值"搬过来，就能补上现在缺的"非爬楼期间提示音 2"，
> 这条正好是课题的**发挥要求**，有教材依据，报告里好写。

---

## 8. 端侧也算一遍的加分项

`gait_algo.c/h` 不依赖任何平台代码（无 malloc、无文件 IO、只用 `math.h` 的 `sqrtf`），
可以直接加进 STM32 工程，与服务端跑同一套算法：

```c
GaitConfig cfg;
gait_default_config(&cfg);
GaitState st;
gait_init(&st, &cfg);

/* 每采到一个样本 */
uint32_t ev = gait_feed(&st, gx, gy, gz, ax, ay, az, NULL, NULL, NULL);
if (ev & GAIT_EV_FLOOR) { Beep_Floor(); OLED_ShowFloor(st.floor); }
```

这样"数据在单片机侧实现计算和显示"就成立了（`泛在物联网模块课程设计安排.pptx` 第 18 页第 4 条要求）。
