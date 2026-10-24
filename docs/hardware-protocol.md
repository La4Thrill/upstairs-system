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
| `half` | 到达中间平台（每 11 级台阶） | 短"滴"一声 |
| `floor` | 到达一个完整楼层（每 22 级台阶） | "滴滴"两声 + LED 闪 |
| `pause` | 检测到真实停顿（平台站定 ≥0.6 s） | 可不响，或柔和提示 |
| `steps15` | 累计每 15 级台阶 | 轻提示音（课题要求） |
| `top` | 到达 99 层上限 | 长鸣 |

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
   现有数据集里三轴角速度的标准差都 < 0.1 °/s（近似常量偏置，见 README），
   所以算法只用加速度。真机上先用串口把 `gx gy gz` 打出来晃动一下板子，
   确认数值有变化，再谈"上下楼方向判别""平台转弯检测"。
2. **量程与灵敏度。** LSM9DS0 加速度 ±2g / 角速度 ±245dps 对应 16384 LSB/g 与 114.29 LSB/dps。
   如果 CubeMX 里配了 ±4g，服务端换算会差 2 倍。
3. **采样率。** 定时器 20 ms 一次，实测用串口打印时间戳核对；50 Hz 是楼层判定的基准，
   偏到 40 Hz 就把 `SAMPLE_INTERVAL_MS` 和 `fs` 一起改。

---

## 7. 端侧也算一遍的加分项

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
