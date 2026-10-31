# 演示操作手册（答辩 / 给老师看）

> 一切都在 `D:\KS\upstairs-system` 里。按顺序做，每步都有"看到什么算成功"。

---

## 一、硬件准备（提前 5 分钟）

| 步骤 | 做什么 | 成功的标志 |
|---|---|---|
| 1 | 5V 适配器插进板子**黑色圆孔**（`POWER IN`） | — |
| 2 | 按一下白色自锁开关 | **`POWER LED`（L3）亮** |
| 3 | USB 线插板子**中间**那个 Mini-USB 口（`UART-USB`） | 电脑出现 `USB-SERIAL CH340 (COM3)` |
| 4 | 九轴模块已按线接好（4 根：GND / 3.3V / SCL=PB6 / SDA=PB7） | 模块不烫、线没松 |

> ⚠️ 三个小口顺序：**最上面 JTAG（烧录用）、中间 UART-USB（串口用）★、最下面 USB-Slave**
> ⚠️ 红色跳线帽保持在 **`VIN`** 那一侧（用适配器供电）

---

## 二、启动（3 个窗口，顺序别乱）

### 第 1 步：起服务端

**双击**：

```
D:\KS\upstairs-system\cmake-build-debug\server.exe
```

会弹出一个黑窗口，应该看到：

```
[BOOT] Stair IoT server starting...
[DB] Database ready (stair.db).
[SIM] 2 simulated terminals ready (SIM-01=upstairs3, SIM-02=dailywalk)
[WEB] listening on :8080
```

**看到 `listening on :8080` 就算成功。这个窗口不要关**（关了服务就停了）。

> 也可以用 CLion 点 ▶ Run（运行配置选 `server`），效果一样。

### 第 2 步：起串口桥

**双击**：

```
D:\KS\upstairs-system\tools\run-ingest.bat
```

会弹出一个窗口，应该看到：

```
使用 Python: C:\Users\...\python.exe
串口       : COM3
设备 ID    : STM32-01

[KEY] 使用上次保存的密钥：stm32d…
来源    : COM3 @ 115200
目标    : http://127.0.0.1:8080/api/ingest   设备 STM32-01   fs=50
--------------------------------------------------------------
  events=['pause'] floor=1 steps=0 climbed=0
  ...
```

**看到 `events=[...]` 一行行刷出来就算成功。这个窗口也不要关。**

### 第 3 步：打开网页

浏览器访问 → <http://127.0.0.1:8080>
登录（如果没自动登录）：**`admin` / `admin123`**

---

## 三、演示流程（建议按这个顺序讲）

### ① 先看设备列表

「设备 / 采集通道」应该有三行：

| 设备ID | 来源 | 状态 |
|---|---|---|
| SIM-01 | 模拟终端 | 离线 |
| SIM-02 | 模拟终端 | 离线 |
| **STM32-01** | **真实设备** | **在线** ← 实时的 |

### ② 数据集回放（证明算法对）

在「模拟端控制台」里：

```
目标终端：全部终端
加载场景：上三层楼数据（判层）
```

点 **「加载文件」** → 点 **「开始」**

然后「当前查看」下拉选 **`SIM-01`**，看「爬楼结果」：

| 指标 | 约 50 秒后 |
|---|---|
| 当前楼层 | **4** |
| 已爬楼层 | **3** |
| 总台阶数 | **68** |
| 步频 | **~82 级/分** |
| 中间平台数 | **3** |

> 这组数字来自老师的数据集，是"标准答案"。

### ③ 真机演示（证明能落地）

「当前查看」切到 **`STM32-01`**，然后把板子拿起来：

```
单手握板（屏幕朝外） → 走一段平路 → 上一层楼梯
```

**现场应该看到：**
- 页面上「总台阶数」一个数一个数往上涨
- 「当前楼层」从 1 变成 2
- 板子上的**蜂鸣器每级台阶响一声**，到一层时 LED1 翻转
- 下面「波形验证」三条曲线跟着动

### ④ 重置再走一遍（可选，很加分）

点「设备 / 采集通道」右上角的 **「重置当前设备」** → 台阶和楼层归零 → 再走一段。

对比"重置前 / 重置后"两张截图，能说明系统是可重复使用的。

### ⑤ 对比讲解

来回切换「当前查看」：`SIM-01`（数据集）↔ `STM32-01`（真机）

一句话总结：**"同一套算法，既能在老师的数据集上跑出 3 层，也能在真实传感器上跑出来。"**

---

## 四、当场出问题怎么办

| 现象 | 原因 | 处理 |
|---|---|---|
| 网页打不开 | 服务端没起 / 窗口被关 | 重新双击 `server.exe` |
| 设备列表没有 STM32-01 | 串口桥没起 | 重新双击 `run-ingest.bat` |
| 有 STM32-01 但显示"离线" | USB 松了 / 板子没电 | 看 `POWER LED`，重插 USB，等 3 秒点「刷新设备」 |
| 数据不动、台阶不涨 | 九轴模块线松了 | 断电重插模块的 4 根线 |
| 数字乱跳 / 一直归零 | 串口被别的程序占用 | 关掉任何 XCOM / SSCOM / 串口助手 |
| 想一键恢复 | 重启服务端 | 双击 `tools\reset-real-device.bat` |

**保命操作**：实在不行就全部关掉 → 先启 `server.exe` → 再启 `run-ingest.bat` → 刷新网页。30 秒能恢复。

---

## 五、结束怎么关

```
① 关掉串口桥窗口（直接点 ×）
② 关掉 server.exe 窗口
③ 拔 USB、关电源开关
```

顺序不重要，两个都能自愈（串口桥会自动重连）。

---

## 六、一句话版本（贴在显示器上）

```
1. 双击 cmake-build-debug\server.exe        → 等 "listening on :8080"
2. 双击 tools\run-ingest.bat                → 等 events=[...] 开始刷
3. 浏览器 127.0.0.1:8080 → admin/admin123
4. 「当前查看」选 STM32-01 → 拿板子走
```

---

## 附：常用命令（想用命令行的时候）

```powershell
# 起服务端
cd D:\KS\upstairs-system
.\cmake-build-debug\server.exe

# 起串口桥
cd D:\KS\upstairs-system
python tools\serial_ingest.py --port COM3 --device STM32-01 --user collector1

# 重置真实设备计数
curl -X POST http://127.0.0.1:8080/api/device/reset ^
     -H "Content-Type: application/json" -H "X-Auth-Token: <token>" ^
     -d "{\"device_id\":\"STM32-01\"}"

# 算法回归测试（8 条，其中真机 1 条）
.\cmake-build-debug\regress.exe testdata\expect.txt
```
