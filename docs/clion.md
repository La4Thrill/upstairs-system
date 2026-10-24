# 在 CLion 里跑这个项目

> 已验证环境：CLion 2025.3 + 自带 CMake/Ninja + MinGW-w64 gcc 8.1（`F:\BaiduNetdiskDownload\mingw64`）。
> CMake 全量编译通过，`server` / `algo_check` / `gyro_check` / `selftest` 四个目标都能出。

---

## 一、打开工程

1. CLion → **File → Open**，选**仓库根目录**（有 `CMakeLists.txt` 的那一层，例如 `...\upstairs-system`）。
   ⚠️ 不要选它的上一级，否则 CMake 找不到工程。
2. 右下角提示 **Load CMake Project** → 点 **Trust Project / Load**。
3. 底部 **CMake** 面板应显示配置成功，展开后能看到 4 个 target：

```
server          ← 主程序（后端 + 网页 + 数据库）
algo_check      ← 算法回归工具（命令行）
gyro_check      ← 陀螺验证工具（命令行）
selftest        ← 自检（SHA-256 / 口令 / 步态）
```

---

## 二、确认工具链（只需要做一次）

**Settings → Build, Execution, Deployment → Toolchains**

- 如果列表里已有 `MinGW`（路径指向 `...\mingw64\bin`）→ 把它拖到第一位设为默认；
- 没有就点 `+` → **MinGW** → 填：

| 项 | 值 |
|---|---|
| Toolset | `F:\BaiduNetdiskDownload\mingw64\mingw64`（选到含有 `bin` 的那一层） |
| C Compiler | 自动填入 `...\bin\gcc.exe` |
| C++ Compiler | `...\bin\g++.exe` |
| Make | `...\bin\mingw32-make.exe` |
| Debugger | `...\bin\gdb.exe` |

> 用 Visual Studio 工具链也能编（CMakeLists 里对 MSVC 加了 `/utf-8`，也不会再去链接 `m`）。

改完工具链后在 CMake 面板点 **Reload CMake Project**。

---

## 三、跑主程序（演示用）

1. 右上角运行配置下拉框选 **server**（CLion 会自动为每个 target 生成一个配置）；
2. 点绿色 ▶ **Run**（或 Shift+F10）；
3. 控制台出现：

```
[BOOT] Stair IoT server starting...
[INIT] Initializing global variables...
[DB] Database ready (stair.db).
[BOOT] retention: raw=0 snapshots=0 events=0 sessions=0 (raw rows now 0)
[SIM] 2 simulated terminals ready (SIM-01=upstairs3, SIM-02=dailywalk)
[WEB] listening on :8080
```

4. 浏览器打开 **<http://127.0.0.1:8080/>**；
5. 登录 `admin` / `admin123`；
6. 设备列表里选 **SIM-01** → 加载"上三层楼数据" → **开始**；
7. 约 50 秒后停在 **已爬 3 层 / 68 级台阶**；切到 **SIM-02** 可以看到同一时间"只计步、不判层"。

> **关于工作目录**：以前在 CLion 里跑会报"html file not found"，因为默认工作目录是
> `cmake-build-debug/`。现在程序会按"工作目录 → 可执行文件目录 → 编译期写入的工程根目录"
> 依次找 `data1.txt` / `index.html`，所以**不需要改任何 Run 配置**。
> 万一还是 404，控制台会打印 `[PATH] 找不到资源文件 ... 已尝试:` 加候选路径，照着改即可。

### 想固定工作目录（可选）

**Run → Edit Configurations… → 选中 server → Working directory** 改成工程根目录（`$CMakeCurrentProjectBinaryDir$/..` 或直接填绝对路径）。
好处是 `stair.db` 也生成在你能预期的地方；不改也能跑。

---

## 四、调试（答辩时很好用）

- 在 `gait_algo.c` 的 `gait_feed()` 里打断点（`st->steps++` 那一行）；
- 启动 server → 页面点"开始" → 断点会**每秒命中 50 次**，看 `st->steps`、`st->floor`、`f`（滤波后的动态加速度）怎么变化；
- 想看得舒服一点：条件断点 `st->steps % 11 == 0`，只在平台/楼层事件处停下。

---

## 五、跑那三个命令行工具

最简单的做法是直接用底部 **Terminal**（工作目录已经是工程根）：

```powershell
# 自检（16 项断言，不依赖服务端）
.\cmake-build-debug\selftest.exe "D:\数据文件\数据文件\九轴数据文件\上三层楼数据.txt"

# 算法回归（参数：文件 是否判层 阈值）
.\cmake-build-debug\algo_check.exe "D:\数据文件\数据文件\九轴数据文件\上三层楼数据.txt" 1 0.18

# 陀螺验证
.\cmake-build-debug\gyro_check.exe "D:\数据文件\数据文件\九轴数据文件\上三层楼数据.txt"
```

（把 `cmake-build-debug` 换成你实际的构建目录名，Release 配置下是 `cmake-build-release`。）

想用 CLion 的 Run 按钮跑工具：

1. **Run → Edit Configurations… → + → CMake Application**（新版本叫 *CMake*）；
2. **Target** 选 `selftest`；
3. **Program arguments** 填数据文件路径（**用引号括起来**，路径里有中文和空格）：

```
"D:\数据文件\数据文件\九轴数据文件\上三层楼数据.txt"
```

4. 保存运行即可，输出在 Run 窗口。

---

## 六、常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `as: unknown option -- gdwarf-5`（构建失败） | CLion 自带 gcc 13 默认发 `--gdwarf-5`，但 PATH 上的 binutils 较旧（2.30 只认 `--gdwarf-2`）。仓库已在 `CMakeLists.txt` 里对 MinGW 统一加 `-gdwarf-2`，**pull 最新代码即可**；或改用另一套自带 binutils 的 MinGW（如 `F:\BaiduNetdiskDownload\mingw64`） |
| `bind failed on port 8080` 或页面打不开 | 8080 被占用（比如上次没关掉的 server，或 CLion 里还跑着一个实例）。**老版本这里有个坑**：Windows 的 `SO_REUSEADDR` 允许第二个实例也绑上 8080，两个进程静默共享端口，网页数字会时对时错。现在已改用 `SO_EXCLUSIVEADDRUSE`，第二个实例会明确报 `bind failed`。解决：Terminal 里 `netstat -ano \| findstr :8080` 找到 PID，`taskkill /PID <pid> /F` |
| 网页数字时对时错、两个终端互相干扰 | 检查是不是开了两个 `server` 进程：`Get-Process server`（PowerShell）。老版本会静默双绑 8080，现在会直接报 `bind failed` |
| 页面 404 / `html file not found` | 看控制台 `[PATH]` 那几行，按它列的候选路径检查；或者把 Working directory 设成工程根 |
| 启动后显示"已停止"、行数不涨 | 没点"开始"；或数据文件没找到（同上） |
| 控制台中文乱码 | **Settings → Editor → File Encodings** 全部设 UTF-8；Run 配置勾上 *Emulate terminal in output console* |
| 跑模拟时有系统"叮"声 | 正常：提示音（中间平台/到达楼层/每 15 级）在服务端 PC 上响 |
| CMake 报找不到编译器 | Toolchains 里没配 MinGW，见第二节 |
| 改了 `CMakeLists.txt` 没生效 | 在 CMake 面板点 **Reload CMake Project** |

---

## 七、提交代码前

CLion 生成的 `cmake-build-debug/` 已经在 `.gitignore` 里（`cmake-build-*/`、`*.db`、`*.db-wal`、`*.db-shm` 都不会提交）。
用 CLion 的 Git 面板 Commit 时确认一下没把这些勾上就行。
