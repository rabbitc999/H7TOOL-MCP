H7-TOOL 原生 C++ MCP v0.2.0

本项目运行于 Windows，使用 C++17、Windows SetupAPI/HID 和 nlohmann/json。
采用 Qt 风格命名和排版，不需要链接 Qt，也不依赖 Python/hidapi。
内部：类型 UpperCamelCase、函数/变量 lowerCamelCase、私有成员 m_ 前缀。
外部：MCP 工具名、JSON 键名和固件 Lua API 保留原协议名称。

源码结构
  h7toolmcp.cpp：入口、MCP JSON-RPC、核心工具、Windows HID 传输。
  h7tooltypes.h：共享类型与通用函数声明。
  h7tooladvanced.h：高级参数校验、Lua 请求生成、结果解析、RTT 读取与消费。
  h7toolluatemplates.h：编译时嵌入的 Lua 模板，保持原 Python API 和执行顺序。
  h7toolschemas.h：编译时嵌入的五个高级工具 schema。
  h7tooltests.h：不触碰硬件的高级回归测试，RTT 使用可注入假传输。
  verify_protocol.py：开发期 stdio 测试与原 Python 生成器一致性测试。
函数均附中文 Doxygen 文档；重点解释参数、返回值、错误、超时、CRC、缓冲区和副作用。

构建与测试（在 仓库根目录 下）
  cmake -S cpp -B cpp/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
  cmake --build cpp/build -j 2
  ctest --test-dir cpp/build --output-on-failure
  python cpp/verify_protocol.py
MinGW 版本静态链接 C++ 运行库，exe 只需 Windows 系统 DLL。
Python 仅用于开发验证；C++ 服务运行和 --self-test 不需要 Python。
可用 Visual Studio 编译器配置 CMake；MSVC 构建未经本机实测。

生成函数 API 文档
  doxygen cpp/Doxyfile
  打开 cpp/docs/html/index.html。
文档配置启用 private 成员，以便查看句柄生命周期与内部协议约定。

启动 MCP
  .\cpp\build\h7tool_mcp.exe
不传动作时运行 stdio 服务，支持逐行 JSON 和 Content-Length 帧。
连接配置见 mcp.config.json，合并 mcpServers 条目到客户端配置。
本次没有自动修改 Codex 的用户配置。
默认从 exe 同目录加载 config.json，CMake 配置阶段复制配置到 build。
编辑 cpp/config.json 后，请重跑 cmake 配置或显式使用 --config cpp/config.json。
显式 --config 指向不存在的文件会报错。

可用工具（11 个）
  bridge_status：枚举设备和显示桥接配置。
  tool_status：读取调试器 UID、版本、GPIO、测量值。
  tool_registers：function 03 读取 1..60 个寄存器。
  read_memory：按 limits.max_read_memory_bytes 限制读取目标内存，默认 1024 字节。
  target_probe / target_identity：读取配置指定的 IDCODE、CPUID、UID。
  uart_transact：配置 UART，可选发送，再接收限定长度/时间的数据。
  can_transact：CAN/CAN-FD 有限次发送。
  i2c_transact：地址扫描或一次写/读事务。
  spi_transact：初始化 SPI，选择 CS，写入后读取。
  rtt_read：扫描或指定 SEGGER RTT 控制块，读取指定 up-buffer，可选消费。
原 Python 的器件库、Lua 草稿管理、烧录预检等仍未迁移。

高级参数与语义
UART
  channel=1，范围 1..8；UART2 的 D0/D1 使用 channel=7，按原实现配置 GPIO。
  baudrate=115200，范围 1200..3000000；parity=0/1/2；data_bits=7/8；stop_bits=1/2。
  send_text 或 send_hex 二选一，原始字节上限 512。
  rx_length=256，范围 0..1024；timeout_ms=500，范围 1..10000。
  固件函数保留 uart_recive 的原拼写。接收字节保存在 rx_bytes/rx_hex。
  rx_text 由十六进制重建，避免打印中的换行、等号和 NUL 破坏行解析。
CAN
  mode=0，范围 0/1/2；mode=2 使用原 Python 的 CAN-FD 路径。
  fifo_len=8，范围 0..64；bitrate=500000，范围 1000..8000000。
  data_bitrate 默认等于 bitrate；frame_id="0x321"。
  extended=false：ID <=0x7FF；extended=true：ID <=0x1FFFFFFF。
  data_text 或 data_hex 二选一；普通模式最多 8 字节，FD 最多 64 字节。
  count=1，范围 1..100；interval_ms=0，范围 0..10000。
  remote=false；远程帧不允许数据，FD 不允许远程帧。
  tx_count 表示固件发送 API 调用计数，不代表远端接收/ACK 已确认。
I2C
  clock_hz=100000，范围 1000..1000000；scan=false；address="0x50"，7 位地址。
  write_text 或 write_hex 二选一，最多 256 字节；read_length=0，范围 0..256。
  scan=true 时不接受写/读负载，扫描地址 1..127，保留原 Python 行为。
  ACK=0 为应答，任一 NACK 使 data.ok=false。
SPI
  freq_id=0，范围 0..7；phase/polarity/cs=0，均允许 0/1。
  write_text 或 write_hex 二选一，最多 256 字节；read_length=0，最多 256。
  delay_us=0，范围 0..1000000。
  按原实现配置两个 CS GPIO，事务中选定 CS 拉低，完成后拉高。

四种总线工具支持 dry_run=true
  只验证参数并返回实际 Lua、脚本字节数和执行超时；不打开硬件。
  参数和负载会先校验；数值字段拒绝 bool/float，布尔字段拒绝字符串。
  十六进制负载只接受 hex 和空白，不静默删除任意非 hex 字符。
  用户数据先转义成 Lua 字符串，模板只做一次替换，防止脚本注入。
  dry_run=false 或不传该参数时实际执行；这些工具会配置接口并可能发送数据。

HID 单帧与时间预算
  HID 输入/输出是 1025 字节，含 report ID 0；有效帧上限 1024 字节。
  Lua 帧固定开销 19 字节，因此实际脚本必须 <=1005 字节。
  转义后的负载会增加脚本长度；上述原始负载上限不代表所有内容都能放入一帧。
  保留原 Python 单帧上传协议；过长脚本在打开 HID 前明确报错，不截断。
  UART 预算至少为接收超时+1500 ms；CAN 至少为 (count-1)*interval_ms+1500 ms。
  执行预算最大 60000 ms；超过此限制的组合在执行前报错。
  Lua 必须同时收到上传 ACK 和完整行起止标记，才返回完整结果。

RTT
  address 可显式指定控制块；否则扫描 ranges 或 config.rtt.ranges。
  ranges 最多 16 项，每项 length<=2 MiB；地址+长度不得超出 32 位地址空间。
  当前 STM32F4 配置搜索 0x20000000 开始的 128 KiB SRAM。
  channel=0，范围 0..15；max_bytes=512，范围 1..2048；consume=false。
  扫描保留 9 字节尾部以匹配跨块的 "SEGGER RTT" 字符串。
  控制块头 24 字节，每个 up-buffer 描述符 24 字节；RdOff 位于描述符+16。
  环形回绕分为尾段/头段，每次内存读取最多 512 字节。
  大小、指针或偏移损坏会报错，读取不足不会推进 RdOff。
  consume=true 会修改目标 RAM 中的读偏移：完整读取后复核原偏移，写入并回读验证。
  并发检查和写入不是原子操作；避免多个消费客户端同时控制同一通道。
  未找到控制块返回 found=0、ok=false；只有固件启用 RTT 后才能验证真实日志。

示例（PowerShell）
  .\cpp\build\h7tool_mcp.exe --probe-h7tool
  .\cpp\build\h7tool_mcp.exe --target-probe
  .\cpp\build\h7tool_mcp.exe --call uart_transact --arguments '{"dry_run":true,"send_text":"Hi"}'
  .\cpp\build\h7tool_mcp.exe --call uart_transact --arguments '{"channel":1,"rx_length":16,"timeout_ms":50}'
  .\cpp\build\h7tool_mcp.exe --call can_transact --arguments '{"dry_run":true,"frame_id":"0x321","data_hex":"01 02"}'
  .\cpp\build\h7tool_mcp.exe --call i2c_transact --arguments '{"dry_run":true,"scan":true}'
  .\cpp\build\h7tool_mcp.exe --call spi_transact --arguments '{"dry_run":true,"write_hex":"9F","read_length":3}'
  .\cpp\build\h7tool_mcp.exe --call rtt_read --arguments '{"address":"0x20000000","consume":false}'

验证记录（2026-10-08）
  H7-TOOL APP=2.33，序列号=002B00223131510339333437。
  目标 IDCODE=0x101F6413，CPUID=0x410FC241；配置选择 STM32F405/407/415/417 系列。
  UID=49 00 33 00 11 51 33 34 37 38 32 35；具体料号需结合芯片标记确认。
  UART1 实机配置与短时接收正常；未发送数据，窗口内 rx_len=0。
  RTT 实机描述符读取正常，0x20000000 未发现控制块。
  CAN/I2C/SPI 未做外部总线实机收发，需要对应设备或回环接线。
  本地测试覆盖帧/编码/协议、参数校验、固件错误、RTT 回绕/分块/消费/并发变化。
  Lua 生成结果与原 Python 的 UART/CAN/I2C/SPI 生成器逐项比对。
  JSON 依赖 v3.11.3 附 MIT 许可证；third_party 文件不做 Qt 命名改造。
