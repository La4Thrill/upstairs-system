# 原始数据记录与「是否爬了楼」的判断

> 对应课程设计要求：**"能够记录原始数据，从而能够判断是否爬了楼"**
> 全部为实测结果，数据来自 `STM32F407 + HJ/Sensors V1`。

---

## 一、为什么必须留原始数据

楼层数是一个**推导结果**，不是测量值。一旦出现"这次怎么多算了一层"，
只有原始六轴数据能回答：

- 是步伐没被检测到（阈值太高）？
- 还是平路走路的步子被算成了爬楼（没有楼梯判据，P0-1）？
- 还是传感器掉了/线松了（数据出现断档或 0）？

**没有原始数据，任何结论都只能靠猜。** 有了它，就能把同一段数据反复重放、调阈值、
跑回归，而且能证明"结论是可复现的"。

---

## 二、数据存在哪

### 表结构（`raw_samples`）

```sql
CREATE TABLE raw_samples (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT,
    ts        REAL,     -- 该样本的时间戳（秒）
    gx, gy, gz, ax, ay, az  INTEGER   -- 6 个 int16 原始值
);
```

**列顺序和老师的数据集完全一致**：角速度 x/y/z，加速度 x/y/z，单位是原始 LSB。

### 保留策略

| 项 | 值 | 说明 |
|---|---|---|
| 每台设备保留行数 | **90000**（`server.c` 的 `RAW_KEEP_ROWS`） | ≈ 30 分钟 @50 Hz |
| 保留天数 | **7 天**（`RETENTION_DAYS`） | 超期按天清理 |
| 清理时机 | 启动时 + `/api/maintenance/retention` | 循环覆盖最旧的数据 |

写库是**批量**的（`db_insert_raw_batch`，一批 100 行），不会一行一事务拖慢采集。

---

## 三、怎么导出

### 方式 A：网页按钮（推荐）

「设备 / 采集通道」右上角 → **「导出原始数据」**

作用于「当前查看」里选中的设备，浏览器直接下载：

```
STM32-01_raw_20261031154500.txt
```

并提示 `已导出 20000 行（约 400.0 秒 @50Hz）`。

### 方式 B：HTTP 接口

```
GET /api/export/raw?device_id=STM32-01[&limit=N]
Header: X-Auth-Token: <登录拿到的 token>
```

| 参数 | 说明 |
|---|---|
| `device_id` | 设备 ID；不填则用会话绑定的设备 |
| `limit` | 取最近多少行；不填默认 20000（≈400 s）。想看满 30 分钟传 `limit=90000` |

返回 `Content-Type: text/plain; charset=utf-8`，每行：

```
gx gy gz ax ay az
```

**和老师的数据集一个格式**，因此所有离线工具可以直接吃。

### 实测样例

```
HTTP 200   Content-Type=text/plain; charset=utf-8
落盘 572155 字节 / 20000 行 → 400.0 秒 @50Hz
服务端日志： [EXPORT] raw samples of STM32-01 (572155 bytes)
```

前 5 行：

```
947 249 -720 -1205 13316 7408
985 106 -744 -1337 13528 7384
886 -1 -819 -1435 13605 7355
749 0 -834 -1485 13528 7254
660 22 -711 -1527 13381 7215
```

---

## 四、怎么据此判断"是否爬了楼"

导出后用 `algo_check.exe`（和服务端/固件同一套算法）离线复算：

```powershell
cd D:\KS\upstairs-system
.\cmake-build-debug\algo_check.exe D:\DSH\export_test.txt 1
```

**实测输出：**

```
file      : export_test.txt
samples   : 19999  (400.0s @ 50Hz)
steps     : 29   (平均 4.4 级/分)
half      : 2 个梯段（其中中间平台 1 个，不含楼层平台）
floor     : 已上楼 1 层，当前 2 层（每层 22 级）
events    : 352
```

**判读方法：**

| 输出 | 含义 |
|---|---|
| `steps` 很小（< 22） | 没爬楼，或阈值偏高漏检 |
| `steps` ≥ 22 且 `floor` 递增 | 判出爬楼，`floor` 值就是要报的层数 |
| `steps` 很大但人明明没爬楼 | 平路/跑步/跳绳被误判（**P0-1 已知限制**） |
| `samples` 明显少于期望 | 中间掉过线（USB 松了），数据有断档 |

### 想一次看多个文件？

用回归工具，逐条对比期望值：

```powershell
.\cmake-build-debug\regress.exe testdata\expect.txt
```

---

## 五、和数据集对比的方法

老师的数据集和真机数据**格式相同**，可以直接放进同一个回归清单：

```
# testdata/expect.txt
3  ../data1.txt          # 老师的上三层楼数据 → 3 层
0  downstairs3.txt       # 下楼不应判楼层
1  walk_real.txt         # 真机走 1 层 → 1 层（本次新增）
```

当前结果（8 条）：

| 数据 | 台阶 | 判出楼层 | 期望 | 结论 |
|---|---|---|---|---|
| `../data1.txt`（上三层楼） | 68 | 3 | 3 | **PASS** |
| `walk_real.txt`（真机 1 层） | 39 | 1 | 1 | **PASS** |
| 下楼 / 跑步 / 跳绳 / 仰卧起坐 / 日常行走 / 混合 | — | 2~8 | 0/2 | FAIL（P0-1 已知限制） |

> 报告里不要写"楼层判断准确"，应写"**在上楼场景判层正确；非爬楼场景存在误判，
> 已在原始数据上定位到原因（缺少爬楼状态机）**"。

---

## 六、注意事项

1. **导出的是"最近 N 行"**，不是全部历史；更早的数据已被保留策略循环覆盖。
2. 默认 20000 行（≈400 s）；`limit=90000` 可拿到满 30 分钟。
3. **导出不影响采集**，也不清数据。
4. 只有 `source=dev` 的真实设备会写 `raw_samples`；模拟终端的数据来自 txt，不重复入库。
5. 采集期间**不要关掉串口桥窗口**，否则会出现数据断档（`samples` 少于期望值就是这个原因）。

---

## 七、相关代码位置

| 功能 | 位置 |
|---|---|
| 表结构 | `db_manager.c` `init_database()` |
| 批量写入 | `db_manager.c` `db_insert_raw_batch()` |
| 导出为文本 | `db_manager.c` `db_export_raw_text()` |
| 导出接口 | `web_server.c` `GET /api/export/raw` |
| 保留量 | `server.c` `RAW_KEEP_ROWS` |
| 网页按钮 | `index.html` `exportRaw()` |
| 离线复算 | `tools/algo_check.c` |
| 批量回归 | `tools/regress.c` |
