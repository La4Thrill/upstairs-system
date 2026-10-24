# SXL2 智能楼梯爬行监测系统（软件设计阶段 → 硬件阶段）

课题：**1、楼梯层数计数（上楼）· 九轴运动传感器**
数据：`上三层楼数据.txt`（2488 样本 @ **50 Hz**，约 49.8 s，上楼 3 层）

> 硬件阶段对接说明见 **[docs/hardware-protocol.md](docs/hardware-protocol.md)**（STM32 上报协议、鉴权、端侧参考代码）。

---

## 1. 现在能跑出什么

| 指标 | 旧版本 | 现在 |
|---|---|---|
| 上三层楼数据检出台阶数 | 465（每个样本都是"一步"） | **68 级** |
| 判出楼层 | 24 层 | **已爬 3 层 / 当前 4 层** |
| 步频 | 105 级/分（错 5 倍） | **82 级/分** |
| 前端波形量纲 | 原始计数（~9 万） | **g / (°/s)** |
| 设备接入 | 单终端（写死 `g_users[0]`） | **多终端/多设备通道**（模拟 2 台 + 真实设备任意接入） |
| 原始数据落库 | 表建了没用 | **`raw_samples` 批量入库**，可按设备回看 |
| 历史记录 | 每一步一行、所有用户混在一起 | **按设备、每 5 s 一条进度快照 + 楼层事件明细** |
| 权限 | 页面藏个按钮 | **user 只能看自己的设备**，越权返回 403 |

---

## 2. 系统架构

```
  模拟终端（多实例）                    服务端                              前端
┌────────────────────┐    ┌───────────────────────────────┐   ┌────────────────────┐
│ SIM-01 上楼(判层)   │───►│ device 通道表 g_devices[16]    │   │ 设备列表 / 切换     │
│ SIM-02 行走(只计步) │───►│  · 每台设备独立 GaitState      │──►│ 实时指标（按设备）  │
└────────────────────┘    │  · 独立波形缓冲 / 在线状态      │   │ 3 条实时波形        │
┌────────────────────┐    │                               │   │ 原始数据回看（库）  │
│ STM32 真实终端      │───►│ POST /api/ingest（设备密钥）    │   │ 历史记录（按设备）  │
│ WiFi + LSM9DS0     │    │                               │   │ 角色登录             │
└────────────────────┘    │ gait_algo（算法，可移植）       │   └────────────────────┘
                          │ audio_hint（事件→音色）         │
                          │ SQLite：6 张表                  │
                          └───────────────────────────────┘
```

**分层：** 数据源（simulator / ingest）→ 设备通道（`Device`）→ 算法（`gait_algo`）→ 存储（`db_manager`）→ 展示（`web_server` + 前端）。

关键点：**模拟终端和真实终端写的是同一个 `Device` 结构**，所以硬件接进来时前端、数据库、算法都不用改。

---

## 3. 算法（为什么能从 465 步变成 68 级台阶）

### 3.1 旧版本为什么错

1. 数据是**有符号 16 位原始计数**（`65338` 实际是 `-198`），旧代码用 `%f` 直接读成 65338，符号和量纲全丢 → `|a|` 常年在 9 万左右。
2. 台阶阈值是常数 `1200`（搭配 `80` 的增量门限），对 9 万的基线恒为真，`cooldown=2` 只能挡住 2 个样本 → 平均每 5.35 个样本就"走一步"，层数实际等于"样本数 / 107"。
3. 采样间隔写 100 ms，与数据集 50 Hz 不符 → 时间轴、步频错 5 倍。

### 3.2 处理链（`gait_algo.c`）

```
原始 LSB(int16)
  │  ①符号还原 + 量纲换算：/16384 → g（LSM9DS0 ±2g）；/114.29 → °/s（±245dps）
  ▼
加速度模长 |a| (g)
  │  ②去重力：一阶低通 α=0.03（@50Hz ≈0.7s），dyn = |a| − 重力
  ▼
动态加速度 dyn ──③5 点滑动平均
  │  ④台阶检测：迟滞 0.18g / 0.09g + 不应期 15 样本(300ms)
  ▼
台阶事件 ──►⑤台阶数 mod 11 → 中间平台(梯段)事件
        ──► 台阶数 mod 22 → 到达楼层事件
        ──► 台阶数 mod 15 → "每跳 15 梯"提示
  │  ⑥动态能量 EMA：RMS<0.14g 持续 0.6s → 平台(真停顿)事件
  ▼
事件 → 提示音（音色区分）+ /api/status + SQLite
```

### 3.3 参数标定依据

| 参数 | 取值 | 依据 |
|---|---|---|
| 采样率 fs | 50 Hz | `九轴数据文件/ReadMe.doc`：所有场景 50 Hz |
| 加速度灵敏度 | 16384 LSB/g | LSM9DS0 ±2g（0.061 mg/LSB） |
| 步态阈值 | 0.18 g（上楼）/ 0.10 g（平地行走） | 上楼动态幅度大；行走起伏小，同阈值会漏检（0.18g→57 级/分，0.10g→91 级/分） |
| 不应期 | 300 ms | 人正常步频上限约 3 步/s |
| 每半层台阶 | 11 级 | 68 级 ÷ 6 个梯段 ≈ 11.3 |
| 每层台阶数 | 22 级 | 68 级 ÷ 3 层 ≈ 22.7，与住宅 18~22 级/层吻合 |
| 楼层上限 | 99 层 | 课题要求 |

**阈值鲁棒性**（上三层楼数据，判层模式）：

| 步态阈值 (g) | 0.10 | 0.12 | 0.14 | 0.16 | 0.18 |
|---|---|---|---|---|---|
| 检出台阶 | 82 | 79 | 74 | 71 | 68 |
| 判出楼层 | **3** | **3** | **3** | **3** | **3** |

---

## 4. 设备与多终端

### 4.1 设备通道

`Device`（`global_vars.h`）一台设备一份：算法状态、波形缓冲、计数、在线状态、设备密钥、归属采集对象。
默认最多 16 台（`MAX_DEVICES`）。

### 4.2 默认模拟终端

| 终端 | 采集对象 | 默认场景 | 行为 |
|---|---|---|---|
| `SIM-01` | collector1 | `upstairs3` | 上楼，判层（0.18g） |
| `SIM-02` | collector2 | `dailywalk` | 日常行走，只计步（0.10g），验证"不误判楼层" |

两台终端各有独立线程、独立文件进度、独立计数——网页上能看到两台设备同时推进、数值互不影响。

场景可单独加载：`POST /api/sim/load` 带 `device_id`。

### 4.3 真实设备接入

见 `docs/hardware-protocol.md`。要点：

```http
POST /api/ingest
X-Device-Key: <首次上报时下发>
{"device_id":"STM32-01","fs":50,"samples":[[gx,gy,gz,ax,ay,az], ...]}
```

- 首次上报自动登记并下发密钥；
- 之后密钥不匹配返回 `403 invalid_device_key`；
- 响应里带 `events`（`half` / `floor` / `pause` / `steps15` / `top`），**提示音在端侧响**；
- 可带 `"user":"collector2"` 把设备绑定到采集对象。

---

## 5. API

| 接口 | 说明 |
|---|---|
| `POST /api/login` | 返回 `token`、`role`、`user_id`；后续请求带 `X-Auth-Token` |
| `GET /api/session` | 当前会话 |
| `GET /api/devices` | 设备列表（admin/monitor 全部；user 只有自己的） |
| `GET /api/status?device_id=` | 单设备实时状态 + 三条波形（accel/gyro/dyn） |
| `GET /api/history?device_id=&limit=` | 进度快照（时间/楼层/已爬/台阶/步频） |
| `GET /api/samples?device_id=&limit=` | 原始样本回看（`raw_samples` 表） |
| `POST /api/ingest` | 设备上报（`X-Device-Key` 鉴权） |
| `POST /api/sim/load` | `{profile, loop, device_id}`，admin/monitor |
| `POST /start` `/stop` `/reset` | 模拟终端控制，admin/monitor |
| `GET /api/collectors` | 采集对象列表 |

默认账号：`admin/admin123`、`monitor1/monitor123`、`collector1/collector123`、`collector2/collector123`。

---

## 6. 数据库（`stair.db`）

| 表 | 用途 | 写入时机 |
|---|---|---|
| `users` | 采集对象 | 启动时种子（collector1/2） |
| `auth_accounts` | 登录账号（user 角色关联 `users.id`） | 启动时 `INSERT OR IGNORE`（**不再每次 DROP**） |
| `devices` | 设备登记：密钥、来源、归属、最后上报、累计样本 | 设备首次出现/每次上报 |
| `raw_samples` | 原始九轴样本（6 列 int16 + 时间戳） | 每 100 个样本批量事务写入 |
| `progress_snapshots` | 进度快照（楼层/已爬/台阶/步频） | 每 5 s + 到达楼层 |
| `floor_events` | 楼层/平台事件明细 | 到达楼层 / 中间平台 / 封顶 |

> 旧的 `climbing.db` 不再使用（旧表结构与新模型不匹配），首次运行会新建 `stair.db`。

---

## 7. 构建与运行

### 7.1 CMake

```powershell
cmake -S . -B build
cmake --build build --config Release -j 4
.\build\Release\server.exe
```

### 7.2 直接用 gcc（最快验证）

```powershell
gcc -std=c99 -O2 -o server.exe server.c globals.c web_server.c simulator.c gait_algo.c `
    db_manager.c audio_hint.c cjson.c sqlite3.c -lws2_32 -lwinmm -lm
.\server.exe
```

打开 <http://127.0.0.1:8080/>，用 `admin/admin123` 登录 → 设备列表里能看到 SIM-01 / SIM-02 →
选中设备 → 加载场景 → 开始。约 50 s 后 SIM-01 停在"已爬 3 层"，SIM-02 持续只计步。

---

## 8. 算法回归工具

```powershell
gcc -std=c99 -O2 -o algo_check.exe tools\algo_check.c gait_algo.c -lm
.\algo_check.exe "E:\数据文件\数据文件\九轴数据文件\上三层楼数据.txt" 1 0.18
```

```
  [  20.9s] FLOOR     floor=2  steps=22  |a|=1.20g dyn=0.31g
  [  42.2s] PAUSE     平台停顿 steps=59
samples   : 2488  (49.8s @ 50Hz)
steps     : 68   (平均 82.0 级/分)
floor     : 已上楼 3 层，当前 4 层（每层 22 级）
```

参数：`<数据文件> [1/0 是否判层] [步态阈值，可选]`。

---

## 9. 已知限制 / 下一步

1. **陀螺数据不可用**：数据集三轴角速度标准差 < 0.1 °/s（近似常量偏置），所以现在只用了加速度，
   上下楼方向判别做不了。硬件阶段必须先确认 LSM9DS0 陀螺出数（见协议文档第 6 节）。
2. **密码仍是明文**：`auth_accounts.password` 直接存明文，注册/改密接口也没有。课程演示够用，
   要加固就上"盐 + SHA-256"。
3. **会话在内存里**：服务重启后所有人要重新登录；没有登出接口。
4. **`raw_samples` 会一直增长**：50 Hz × 2 台 ≈ 36 万行/小时，演示没问题，
   长期跑需要加保留策略（比如只留最近 7 天）。
5. **`sqlite3.c` 仍在仓库里**（9.4 MB）：为了让 `gcc 一行命令` 就能编，先留着；换 vcpkg/系统库后可以删。
6. 模拟终端的"文件缺失/格式错"只会静默停止，页面上没有明确报错。

---

## 10. 文件说明

| 文件 | 作用 |
|---|---|
| `server.c` | 进程入口、定时器精度、设备恢复、线程启动 |
| `simulator.c` | 模拟终端（多实例）：文件源、场景、发送计数 |
| `gait_algo.c/h` | **算法层**（台上/台下通用，可编进 STM32） |
| `device_*`（在 `globals.c`） | 设备通道注册表 |
| `audio_hint.c/h` | 事件 → 提示音（音色按事件区分） |
| `db_manager.c` | SQLite 建表与读写（6 张表） |
| `web_server.c` | HTTP 服务、Token 会话、`/api/ingest`、接口 |
| `globals.c` / `global_vars.h` | `Device` 结构与全局状态 |
| `index.html` / `terminal.html` | 监控页（设备列表/切换）/ 模拟端独立页 |
| `tools/algo_check.c` | 算法回归与参数标定工具 |
| `docs/hardware-protocol.md` | **硬件对接协议**（STM32/ESP8266） |
