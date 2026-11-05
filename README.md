# H7-TOOL C++ MCP

通过 MCP（Model Context Protocol）将 H7-TOOL 调试器连接到 Codex 等客户端，提供 STM32 目标识别、内存读取、UART、CAN、I²C、SPI 和 SEGGER RTT 功能。

本目录从 `STM32H7-SmartCar/cpp` 提取，包含源码、第三方依赖、测试脚本和已编译的 Windows 程序，可独立于原仓库使用。当前版本为 **0.2.1**，默认目标配置为 **STM32H743**。

## 环境要求

- Windows；底层使用 Windows SetupAPI/HID。
- 使用硬件功能时，需要连接 H7-TOOL，并按实际目标及总线连接设备。
- 运行现成的 `bin/h7tool_mcp.exe` 无需 Python、Qt 或独立安装的 C++ 运行库；本次 MinGW 构建静态链接 C++ 运行库。
- 从源码构建需要 CMake 3.16 及以上和支持 C++17 的编译器。当前程序使用 MinGW 构建；项目也提供 MSVC 构建配置。
- Python 仅用于开发期协议测试，不参与 MCP 服务运行。

## 快速开始

在 PowerShell 中进入本目录：

```powershell
Set-Location G:\Desktop\h7tool-mcp
```

先执行不接触硬件的自测：

```powershell
.\bin\h7tool_mcp.exe --self-test
```

直接启动 MCP stdio 服务：

```powershell
.\bin\h7tool_mcp.exe --config .\config.json
```

启动后程序等待客户端通过标准输入发送 JSON-RPC 请求，没有交互界面。通常应由 MCP 客户端自动启动。

**请显式传入 `--config`。** 默认配置查找位置是可执行文件所在目录，而本独立包将配置放在根目录、程序放在 `bin` 目录。显式指定的配置文件不存在时，程序会报错。

## 接入 Codex

本机已经注册并启用名为 `h7tool-cpp` 的 MCP 服务，启动程序和配置均指向本目录。可查看配置：

```powershell
codex mcp get h7tool-cpp
```

在其他环境安装，或移动目录后重新注册：

```powershell
codex mcp add h7tool-cpp -- G:/Desktop/h7tool-mcp/bin/h7tool_mcp.exe --config G:/Desktop/h7tool-mcp/config.json
```

路径应替换为实际的绝对路径。客户端需要重新加载 MCP 配置；必要时重启 Codex。

移除客户端注册：

```powershell
codex mcp remove h7tool-cpp
```

该命令只移除注册，不删除本目录。

## 接入其他 MCP 客户端

对于支持 `mcpServers` 配置格式的客户端，可合并本目录的 `mcp.config.json`：

```json
{
  "mcpServers": {
    "h7tool-cpp": {
      "command": "G:/Desktop/h7tool-mcp/bin/h7tool_mcp.exe",
      "args": ["--config", "G:/Desktop/h7tool-mcp/config.json"]
    }
  }
}
```

服务通过 stdio 通信，支持逐行 JSON 和 `Content-Length` 帧，不需要监听网络端口。

## 可用工具

| 工具 | 用途 | 使用说明 |
| --- | --- | --- |
| `bridge_status` | 枚举 Windows HID 设备并显示桥接配置 | 用于检查设备发现与配置 |
| `tool_status` | 读取 H7-TOOL UID、版本、GPIO 和测量值 | 需要连接调试器 |
| `tool_registers` | 使用 function 03 读取寄存器 | 单次读取 1～60 个寄存器 |
| `read_memory` | 读取目标内存 | 默认上限 1024 字节 |
| `target_probe` | 读取目标探测信息 | 地址由目标配置指定 |
| `target_identity` | 读取目标身份信息 | 包括配置指定的 IDCODE、CPUID、UID |
| `uart_transact` | 配置 UART，可选发送并限时接收 | 支持 `dry_run` |
| `can_transact` | 执行有限次 CAN/CAN-FD 发送 | 支持 `dry_run` |
| `i2c_transact` | 地址扫描或一次写入/读取 | 支持 `dry_run` |
| `spi_transact` | 初始化 SPI、选择 CS 并写入/读取 | 支持 `dry_run` |
| `rtt_read` | 查找或指定 RTT 控制块并读取 up-buffer | `consume=true` 会更新目标 RAM 中的读偏移 |

UART、CAN、I²C、SPI 的 `dry_run=true` 仅校验参数并生成 Lua 计划，不打开硬件。省略该参数或设为 `false` 时会实际配置接口，并可能发送数据。

具体参数、范围、协议限制和 RTT 消费语义见 [原始详细说明](README.zh-CN.txt)，客户端也可通过 `tools/list` 获取工具 schema。

## 配置说明

配置文件为根目录的 `config.json`：

| 配置项 | 当前值 | 含义 |
| --- | --- | --- |
| `adapter.type` | `h7tool_hid` | 使用原生 Windows HID 传输 |
| `adapter.vendor_id` | `49745`（`0xC251`） | USB VID |
| `adapter.product_id` | `61450`（`0xF00A`） | USB PID |
| `adapter.interface_number` | `2` | HID 接口号 |
| `adapter.timeout_ms` | `8000` | 适配器超时，单位毫秒 |
| `adapter.unit_id` | `1` | 协议单元 ID |
| `limits.max_read_memory_bytes` | `1024` | `read_memory` 单次读取上限 |
| `target.family` | `STM32H743` | 目标系列标记 |
| `target.idcode_address` | `0x5C001000` | IDCODE 地址 |
| `target.cpuid_address` | `0xE000ED00` | CPUID 地址 |
| `target.uid_address` | `0x1FF1E800` | UID 起始地址 |
| `target.uid_length` | `12` | UID 字节数 |
| `rtt.ranges` | 从 `0x24000000` 开始，长度 `524288` | 默认 RTT 搜索范围，共 512 KiB |

更换目标芯片时，应核对芯片手册并修改对应地址和内存范围。`family` 是配置标记，不代表自动识别成功。RTT 搜索范围还需覆盖固件实际放置控制块的 RAM 区域。

修改根目录配置后，使用显式 `--config` 的客户端无需复制配置文件；让服务重新启动即可读取新配置。

## 命令行示例

以下命令均在本目录执行。

枚举 HID 设备：

```powershell
.\bin\h7tool_mcp.exe --config .\config.json --list-hid-devices
```

读取调试器状态或探测目标（会访问硬件）：

```powershell
.\bin\h7tool_mcp.exe --config .\config.json --probe-h7tool
.\bin\h7tool_mcp.exe --config .\config.json --target-probe
```

通过支持正确传递原生程序 JSON 参数的 PowerShell 环境调用模拟事务：

```powershell
.\bin\h7tool_mcp.exe --config .\config.json --call uart_transact --arguments '{"dry_run":true,"send_text":"Hi"}'
.\bin\h7tool_mcp.exe --config .\config.json --call can_transact --arguments '{"dry_run":true,"frame_id":"0x321","data_hex":"01 02"}'
.\bin\h7tool_mcp.exe --config .\config.json --call i2c_transact --arguments '{"dry_run":true,"scan":true}'
.\bin\h7tool_mcp.exe --config .\config.json --call spi_transact --arguments '{"dry_run":true,"write_hex":"9F","read_length":3}'
```

若旧版 PowerShell 导致 JSON 引号丢失，可使用 MCP 客户端调用工具，或使用 Python `subprocess` 以参数列表传递 JSON。

## 从源码构建

在本目录执行：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

新程序生成在 `build/h7tool_mcp.exe`，CMake 同时将配置复制到 `build/config.json`。

当前客户端指向 `bin/h7tool_mcp.exe`。更新程序时，先停止使用该程序的 MCP 服务，再复制新产物：

```powershell
Copy-Item .\build\h7tool_mcp.exe .\bin\h7tool_mcp.exe -Force
```

也可以将客户端配置直接改为新生成的可执行文件路径。修改根目录配置不会自动同步到已有的 `build/config.json`；继续显式使用根目录配置，或重新运行 CMake 配置命令。

## 测试与验证

验证随包程序，无需连接硬件：

```powershell
.\bin\h7tool_mcp.exe --self-test
python .\verify_protocol.py --executable .\bin\h7tool_mcp.exe
```

测试覆盖 CRC、初始化、通知、工具列表、错误响应、内存参数解析、JSON 行、帧格式、UTF-8、总线模拟计划、参数边界，以及 RTT 的内置回归场景。

2026-11-05 提取安装时，上述两项验证均通过，并确认注册了全部 11 个工具。本次没有执行硬件读写或总线收发；这些测试结果不表示已经验证实际接线和目标通信。

`verify_protocol.py` 另有可选的 `--reference` 参数，用于与原 Python 实现进行 Lua 生成结果比对；该参考实现未包含在本独立包中，默认验证不依赖它。

## 目录结构

```text
h7tool-mcp/
├── README.md                  本独立包说明
├── README.zh-CN.txt           原始详细说明
├── 独立版说明.txt             简要启动说明
├── LICENSE                    原仓库 GPLv3 许可证文本
├── bin/h7tool_mcp.exe          已编译的独立程序
├── config.json                目标与适配器配置
├── mcp.config.json            本机 MCP 客户端配置
├── mcp.config.example.json    原始配置模板
├── CMakeLists.txt             构建与测试入口
├── h7toolmcp.cpp              服务入口、JSON-RPC 和 HID 传输
├── h7tooltypes.h              共享类型
├── h7tooladvanced.h           总线与 RTT 实现
├── h7toolluatemplates.h        Lua 模板
├── h7toolschemas.h            高级工具 schema
├── h7tooltests.h              内置测试
├── advancedtools.json         高级工具定义数据
├── verify_protocol.py         stdio 协议验证脚本
├── Doxyfile                   原仓库 API 文档配置
└── third_party/
    ├── json.hpp               nlohmann/json v3.11.3
    └── LICENSE.nlohmann-json   第三方 MIT 许可证
```

`Doxyfile` 已适配本独立目录。在根目录运行 `doxygen Doxyfile`，或使用 CMake 的 `api_docs` 目标；HTML 输出到 `docs/html/`。

## 常见问题

- **客户端无法启动服务**：检查可执行文件和配置文件的绝对路径；移动文件夹后需更新客户端配置。
- **手动启动后没有输出**：不指定动作时是 stdio 服务，会等待客户端请求。可通过 `--self-test` 检查程序能否运行。
- **找不到调试器**：先运行 `--list-hid-devices`，检查 USB 连接、设备是否被系统识别，以及 VID、PID、接口号是否匹配。
- **目标识别或内存读取失败**：检查目标供电、调试接线和目标配置地址。
- **RTT 找不到控制块**：确认固件启用了 SEGGER RTT，并将搜索范围设为控制块实际所在的内存区域。
- **总线请求因脚本过长被拒绝**：HID 单帧允许的实际 Lua 脚本最多 1005 字节，转义可能增加长度，应缩短负载或拆分事务。

## 许可证

本包保留来源仓库的 [GNU GPL v3 许可证文本](LICENSE)。第三方 `nlohmann/json` 使用 MIT 许可证，详见 [第三方许可证](third_party/LICENSE.nlohmann-json)。复制或分发时请一并保留相关许可证。

## 0.2.1 修复记录（2026-11-05）

通过官方 PC 2.3.3 二进制静态分析、旧版公开源码和固件 2.33 实机 HID 抓包核对协议，修复 Lua 诊断间歇性返回不完整的问题：

- 只轮询、解析 Lua 打印通道 0，避免通道 2/3 的二进制状态和 UART 监视数据混入文本。
- 按响应头的 32 位大端长度重组跨 HID 报告的 function 0x61 响应，然后校验整帧 CRC。
- 每次请求 512 字节打印数据；仅裁掉声明帧之外的 HID 填充，不删除有效载荷的 NUL/FF。
- 增加分片、填充、CRC 损坏、32 位超长长度和二进制通道的回归用例。
- 修复独立目录下 Doxygen 输入/输出和 CMake 工作目录。

逆向证据见 `PROTOCOL-NOTES.md`。原程序与修改前源码保留在 `work/before-fix/`。正在运行的旧 MCP 进程需要重新加载，才会使用新程序。
