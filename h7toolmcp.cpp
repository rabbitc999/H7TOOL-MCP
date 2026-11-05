/** @file h7toolmcp.cpp
 * @brief Windows 原生 H7-TOOL MCP 服务入口与 USB HID 传输层。
 *
 * 数据路径：JSON-RPC -> H7ToolMcpServer -> HidTransport -> function 0x03 / 0x64 / 0x61。
 * 内部 C++ 类型/函数遵循 Qt 风格命名，但无需 Qt 库；外部协议名称保持兼容。
 * 高级参数/脚本/RTT 位于 h7tooladvanced.h；固定 Lua 模板位于 h7toolluatemplates.h。
 * 标准输出只用于 CLI 结果或 MCP 消息，诊断错误输出到 stderr。
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include "h7tooltypes.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>
using SteadyClock = std::chrono::steady_clock;
namespace fs = std::filesystem;
/**
 * @brief 格式化无符号数为带 0x 前缀的大写十六进制字符串。
 * @param value 数值。
 * @param width 最少十六进制位数；不会截断高位。
 * @return 格式化后的 ASCII 字符串。
 */
std::string formatHex(uint64_t value, int width)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return stream.str();
}
/**
 * @brief 将 Windows UTF-16 字符串转换为 UTF-8。
 * @param wideText 设备路径、序列号或产品名称。
 * @return UTF-8 字节；空输入返回空字符串。
 */
std::string toUtf8(const std::wstring& wideText)
{
    if (wideText.empty())
        return { };
    int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, wideText.data(), int(wideText.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8Text(byteCount, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideText.data(), int(wideText.size()), utf8Text.data(),
        byteCount, nullptr, nullptr);
    return utf8Text;
}
/**
 * @brief 去掉字符串两端的空白与 NUL，保留中间的二进制内容。
 * @param text 待裁剪的字符串副本。
 * @return 裁剪后的内容。
 */
std::string trimText(std::string text)
{
    auto firstOffset = text.find_first_not_of(" \r\n\t\0", 0, 5);
    if (firstOffset == std::string::npos)
        return { };
    auto lastOffset = text.find_last_not_of(" \r\n\t\0", std::string::npos, 5);
    return text.substr(firstOffset, lastOffset - firstOffset + 1);
}
/**
 * @brief 判断诊断标记是否作为完整输出行出现。
 * @param text 累积 Lua 输出，可含 CRLF 和前后空白。
 * @param marker 预期标记；短标记 RB/RE 也使用完整行匹配。
 * @return 找到匹配行时为 true。
 */
bool containsDiagnosticMarker(const std::string& text, const std::string& marker)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
        if (trimText(line) == marker)
            return true;
    return false;
}
/**
 * @brief 严格解析 32 位目标十六进制地址。
 * @param value 0x 开头、1..8 位十六进制的 JSON 字符串。
 * @return 地址值。
 * @throws BridgeError JSON 类型、格式或范围非法。
 */
uint32_t parseAddress(const Json& value)
{
    if (!value.is_string())
        throw BridgeError("address must be a hexadecimal string");
    auto text = value.get<std::string>();
    if (!std::regex_match(text, std::regex("0x[0-9a-fA-F]{1,8}")))
        throw BridgeError("address must be 0x followed by 1..8 hex digits");
    return uint32_t(std::stoull(text, nullptr, 16));
}
/**
 * @brief 读取有范围的 JSON 整数，拒绝布尔和浮点值。
 * @param arguments 参数对象。
 * @param key 外部协议字段名。
 * @param defaultValue 字段缺省值。
 * @param minimum 最小值（含）。
 * @param maximum 最大值（含）。
 * @return 已校验的 int 值。
 * @throws BridgeError 类型错误或超出范围。
 */
int boundedInteger(
    const Json& arguments, const char* key, int defaultValue, int minimum, int maximum)
{
    if (arguments.contains(key)
        && (!arguments[key].is_number_integer() || arguments[key].is_boolean()))
        throw BridgeError(std::string(key) + " must be an integer");
    auto value = arguments.contains(key) ? arguments[key].get<int64_t>() : defaultValue;
    if (value < minimum || value > maximum)
        throw BridgeError(std::string(key) + " must be " + std::to_string(minimum) + ".."
            + std::to_string(maximum));
    return int(value);
}
/**
 * @brief 计算 Modbus RTU CRC-16/IBM。
 * @param bytes 输入字节数组。
 * @param length 参与校验的前缀长度，调用者保证不超过数组长度。
 * @return 初值 0xFFFF、多项式 0xA001 的 CRC。
 */
uint16_t modbusCrc16(const ByteArray& bytes, size_t length)
{
    uint16_t checksum = 0xFFFF;
    for (size_t byteIndex = 0; byteIndex < length; ++byteIndex) {
        checksum ^= bytes[byteIndex];
        for (int bitIndex = 0; bitIndex < 8; ++bitIndex)
            checksum
                = (checksum & 1) ? uint16_t((checksum >> 1) ^ 0xA001) : uint16_t(checksum >> 1);
    }
    return checksum;
}
/**
 * @brief 追加低字节在前的 Modbus CRC。
 * @param frame 就地扩展的帧体；不包含 CRC 和 HID report ID。
 */
void appendModbusCrc(ByteArray& frame)
{
    auto checksum = modbusCrc16(frame, frame.size());
    frame.push_back(checksum & 255);
    frame.push_back(checksum >> 8);
}
/**
 * @brief 验证带填充 HID 载荷中的 RTU 帧前缀。
 * @param frame 可能含尾部填充的报告载荷。
 * @param length 真正帧长，含两个 CRC 字节。
 * @return 长度有效且 CRC 匹配时为 true；不抛异常。
 */
bool isValidModbusFrame(const ByteArray& frame, size_t length)
{
    return length >= 4 && length <= frame.size()
        && modbusCrc16(frame, length - 2) == (frame[length - 2] | (frame[length - 1] << 8));
}
/**
 * @brief 以网络字节序追加两个字节。
 * @param bytes 目标帧。
 * @param value 低 16 位数值；上层先校验地址和寄存器数量。
 */
void appendBigEndian16(ByteArray& bytes, uint32_t value)
{
    bytes.push_back((value >> 8) & 255);
    bytes.push_back(value & 255);
}
/**
 * @brief 以网络字节序追加四个字节。
 * @param bytes 目标帧。
 * @param value 32 位数值；用于 Lua 上传长度和偏移。
 */
void appendBigEndian32(ByteArray& bytes, uint32_t value)
{
    bytes.push_back((value >> 24) & 255);
    bytes.push_back((value >> 16) & 255);
    bytes.push_back((value >> 8) & 255);
    bytes.push_back(value & 255);
}
/** @brief 唯一拥有 Windows HANDLE 的 RAII 容器；禁止复制，防止重复关闭。 */
struct WindowsHandle {
    HANDLE nativeHandle = INVALID_HANDLE_VALUE;
    /** @brief 接管给定句柄；INVALID_HANDLE_VALUE 表示未打开。 */
    explicit WindowsHandle(HANDLE x = INVALID_HANDLE_VALUE)
        : nativeHandle(x)
    {
    }
    /** @brief 若句柄有效则 CloseHandle；析构不抛异常。 */
    ~WindowsHandle()
    {
        if (nativeHandle != INVALID_HANDLE_VALUE && nativeHandle != nullptr)
            CloseHandle(nativeHandle);
    }
    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;
};
/** @brief USB HID 枚举快照，路径保持 UTF-16 以供 CreateFileW 使用。 */
struct HidDeviceInfo {
    std::wstring path;
    std::string serial, product;
    int interfaceNumber = -1;
    USHORT input = 0, output = 0;
};
/**
 * @brief 只读枚举指定 VID/PID 的 HID 接口与报告长度。
 * @param config 配置对象；adapter 默认为 C251:F00A。
 * @return 所有匹配接口，包含 MI_01/MI_02，不打开通信会话。
 * @throws BridgeError SetupAPI 枚举失败或配置不合法。
 * @note HID 预解析数据与设备信息列表均按 Windows API 要求释放。
 */
std::vector<HidDeviceInfo> enumerateHidDevices(const Json& config)
{
    auto a = config.value("adapter", Json::object());
    int vid = boundedInteger(a, "vendor_id", 0xC251, 0, 65535),
        pid = boundedInteger(a, "product_id", 0xF00A, 0, 65535);
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO info
        = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        throw BridgeError("Cannot enumerate HID devices");
    /** @brief 在任何退出路径释放 SetupAPI 设备信息列表。 */
    struct Cleanup {
        HDEVINFO h;
        /** @brief 析构时释放设备信息列表，不抛异常。 */
        ~Cleanup() { SetupDiDestroyDeviceInfoList(h); }
    } cleanup { info };
    std::vector<HidDeviceInfo> out;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA di { };
        di.cbSize = sizeof(di);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &g, i, &di)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            throw BridgeError("HID enumeration failed");
        }
        DWORD n = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &di, nullptr, 0, &n, nullptr);
        std::vector<unsigned char> buf(n);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &di, detail, n, nullptr, nullptr))
            continue;
        WindowsHandle h(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (h.nativeHandle == INVALID_HANDLE_VALUE)
            continue;
        HIDD_ATTRIBUTES attr { };
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(h.nativeHandle, &attr) || attr.VendorID != vid
            || attr.ProductID != pid)
            continue;
        HidDeviceInfo d;
        d.path = detail->DevicePath;
        std::string p = toUtf8(d.path);
        std::transform(
            p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::smatch m;
        if (std::regex_search(p, m, std::regex("mi_([0-9a-f]{2})")))
            d.interfaceNumber = std::stoi(m[1], nullptr, 16);
        wchar_t text[256] { };
        if (HidD_GetSerialNumberString(h.nativeHandle, text, sizeof(text)))
            d.serial = toUtf8(text);
        std::memset(text, 0, sizeof(text));
        if (HidD_GetProductString(h.nativeHandle, text, sizeof(text)))
            d.product = toUtf8(text);
        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(h.nativeHandle, &prep)) {
            HIDP_CAPS caps { };
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                d.input = caps.InputReportByteLength;
                d.output = caps.OutputReportByteLength;
            }
            HidD_FreePreparsedData(prep);
        }
        out.push_back(d);
    }
    return out;
}
/**
 * @brief 将原生设备信息转换为 MCP/CLI 的 JSON 列表。
 * @param config VID/PID 配置。
 * @return 包含 UTF-8 路径、序列号、产品名和报告长度的数组。
 */
Json hidDeviceList(const Json& config)
{
    Json r = Json::array();
    for (const auto& d : enumerateHidDevices(config))
        r.push_back({ { "path", toUtf8(d.path) }, { "serial_number", d.serial },
            { "product", d.product }, { "interface_number", d.interfaceNumber },
            { "input_report_bytes", d.input }, { "output_report_bytes", d.output } });
    return r;
}
/** @brief 重组 function 0x61 的跨 HID 报告响应，CRC 只覆盖实际帧。 */
class PrintFrameAssembler {
    ByteArray m_pending;
    size_t m_expected = 0;
public:
    ByteArray append(const ByteArray& report)
    {
        if (m_pending.empty()) {
            if (report.size() < 8 || report[0] != 1 || report[1] != 0x61)
                return {};
            m_expected = 10 + (size_t(report[4]) << 24) + (size_t(report[5]) << 16)
                + (size_t(report[6]) << 8) + report[7];
            if (m_expected > 4106)
                throw BridgeError("Print response exceeds 4096-byte payload");
        }
        size_t count = std::min(report.size(), m_expected - m_pending.size());
        m_pending.insert(m_pending.end(), report.begin(), report.begin() + count);
        if (m_pending.size() < m_expected)
            return {};
        ByteArray frame;
        frame.swap(m_pending);
        m_expected = 0;
        if (!isValidModbusFrame(frame, frame.size()))
            throw BridgeError("Invalid print response CRC");
        return frame;
    }
    bool pending() const { return !m_pending.empty(); }
};
/** @brief H7-TOOL MI_02 同步传输会话；以原生 OVERLAPPED 实现超时。 */
class HidTransport {
    WindowsHandle m_handle; ///< 当前通信接口句柄，析构时自动关闭。
    int m_timeoutMs; ///< 默认单次 HID I/O / 诊断等待预算（毫秒）。

public:
    /**
     * @brief 选择唯一 MI_02 接口并打开读写会话。
     * @param config VID/PID/接口/序列号/超时配置。
     * @throws BridgeError 无设备、多设备、报告长度不符或无法打开。
     */
    HidTransport(const Json& config)
        : m_timeoutMs(boundedInteger(
              config.value("adapter", Json::object()), "timeout_ms", 8000, 100, 60000))
    {
        auto a = config.value("adapter", Json::object());
        auto list = enumerateHidDevices(config);
        int iface = boundedInteger(a, "interface_number", 2, 0, 255);
        std::string serial = a.value("serial_number", std::string());
        list.erase(std::remove_if(list.begin(), list.end(),
                       [&](const HidDeviceInfo& d) {
                           return d.interfaceNumber != iface
                               || (!serial.empty() && serial != d.serial);
                       }),
            list.end());
        if (list.empty())
            throw BridgeError("H7-TOOL HID Communication interface not found; connect USB "
                              "and check interface_number");
        if (list.size() != 1)
            throw BridgeError("Multiple H7-TOOL devices: set adapter.serial_number");
        if (list[0].input != 1025 || list[0].output != 1025)
            throw BridgeError("Unsupported HID report length; expected 1025 bytes "
                              "including report ID");
        m_handle.nativeHandle = CreateFileW(list[0].path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
            nullptr);
        if (m_handle.nativeHandle == INVALID_HANDLE_VALUE)
            throw BridgeError(
                "Cannot open H7-TOOL HID; Windows error " + std::to_string(GetLastError()));
        HidD_FlushQueue(m_handle.nativeHandle);
    }
    HidTransport(const HidTransport&) = delete;
    /**
     * @brief 通过 OVERLAPPED 执行有超时的 HID 读写。
     * @param write true 写入、false 读取。
     * @param b 写入数据或预分配读取缓冲区。
     * @param ms 最长等待时间（毫秒）。
     * @return 实际传输的字节；读取超时返回空数组。
     * @throws BridgeError 写超时、短写或 Windows I/O 错误。
     * @note 取消后等待 I/O 完成，避免 OVERLAPPED 与缓冲区提前失效。
     */
    ByteArray performIo(bool write, ByteArray b, int ms)
    {
        WindowsHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.nativeHandle)
            throw BridgeError("Cannot create HID event");
        OVERLAPPED ov { };
        ov.hEvent = event.nativeHandle;
        DWORD transferred = 0;
        BOOL ok = write
            ? WriteFile(m_handle.nativeHandle, b.data(), DWORD(b.size()), &transferred, &ov)
            : ReadFile(m_handle.nativeHandle, b.data(), DWORD(b.size()), &transferred, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING)
                throw BridgeError("HID I/O failed: " + std::to_string(GetLastError()));
            DWORD wait = WaitForSingleObject(event.nativeHandle, DWORD(ms));
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(m_handle.nativeHandle, &ov);
                GetOverlappedResult(m_handle.nativeHandle, &ov, &transferred, TRUE);
                if (wait == WAIT_TIMEOUT && !write)
                    return { };
                throw BridgeError("HID I/O timeout or wait failure");
            }
            if (!GetOverlappedResult(m_handle.nativeHandle, &ov, &transferred, FALSE))
                throw BridgeError("HID completion failed");
        }
        if (write && transferred != b.size())
            throw BridgeError("HID short write");
        b.resize(transferred);
        return b;
    }
    /**
     * @brief 把协议帧封装为固定 1025 字节 HID 输出报告。
     * @param frame 最多 1024 字节的 RTU/Lua 帧，已带 CRC。
     * @throws BridgeError 帧过长或 HID 写入失败。
     * @note 首字节 report ID=0，其余未用字节填零。
     */
    void sendFrame(const ByteArray& frame)
    {
        if (frame.size() > 1024)
            throw BridgeError("Lua/frame exceeds one HID payload");
        ByteArray b(1025);
        std::copy(frame.begin(), frame.end(), b.begin() + 1);
        performIo(true, std::move(b), m_timeoutMs);
    }
    /**
     * @brief 读取报告并剥离 Windows 的 report ID 字节。
     * @param ms 接收等待时间（毫秒）。
     * @return 原始 1024 字节载荷，超时返回空数组。
     * @throws BridgeError report ID 不为 0 或读取失败。
     */
    ByteArray receiveReport(int ms)
    {
        auto b = performIo(false, ByteArray(1025), ms);
        if (!b.empty()) {
            if (b[0] != 0)
                throw BridgeError("Unexpected HID report ID");
            b.erase(b.begin());
        }
        return b;
    }
    /**
     * @brief 发送 function 03 并等待匹配的寄存器应答。
     * @param addr 16 位起始寄存器地址。
     * @param count 1..60 个寄存器，不允许跨越 0xFFFF。
     * @param unit Modbus unit ID，由配置先验证为 0..255。
     * @return 大端解码的无符号 16 位寄存器值。
     * @throws BridgeError Modbus 异常、非法范围或无有效应答。
     */
    std::vector<int> readHoldingRegisters(int addr, int count, int unit)
    {
        if (addr < 0 || addr > 65535 || count < 1 || count > 60 || addr + count > 65536)
            throw BridgeError("Register range invalid");
        ByteArray req { static_cast<unsigned char>(unit), 3 };
        appendBigEndian16(req, addr);
        appendBigEndian16(req, count);
        appendModbusCrc(req);
        sendFrame(req);
        auto end = SteadyClock::now() + std::chrono::milliseconds(m_timeoutMs);
        while (SteadyClock::now() < end) {
            auto b = receiveReport(100);
            if (b.size() < 5 || b[0] != unit)
                continue;
            size_t n = b[1] == 3 ? 5 + b[2] : 5;
            if (!isValidModbusFrame(b, n))
                continue;
            if (b[1] == 0x83)
                throw BridgeError("Modbus exception " + std::to_string(b[2]));
            if (b[1] != 3 || b[2] != count * 2)
                continue;
            std::vector<int> v;
            for (int i = 0; i < count; ++i)
                v.push_back((b[3 + 2 * i] << 8) | b[4 + 2 * i]);
            return v;
        }
        throw BridgeError("No matching HID register response; close vendor application "
                          "and retry");
    }
    /**
     * @brief 上传单报告 Lua 并轮询 Lua 打印通道 0，收集完整诊断输出。
     * @param script ASCII 脚本，必须含 begin/end 输出标记。
     * @param begin 起始标记。
     * @param endMarker 结束标记。
     * @param executionTimeoutMs 脚本执行预算；0 表示使用配置值。
     * @return 已裁剪的 UTF-8 原始输出。
     * @throws BridgeError 上传失败、无 ACK、输出超时或输出超过 1 MiB。
     * @note 必须同时收到 0x64 ACK 和完整标记，不把 ACK 当成执行成功。
     */
    std::string runLuaScript(std::string script, const std::string& begin,
        const std::string& endMarker, int executionTimeoutMs = 0)
    {
        if (script.find(begin) == std::string::npos || script.find(endMarker) == std::string::npos)
            throw BridgeError("Lua diagnostic markers missing");
        HidD_FlushQueue(m_handle.nativeHandle);
        script.push_back('\0');
        ByteArray req { 1, 0x64, 0, 0 };
        appendBigEndian32(req, uint32_t(script.size()));
        appendBigEndian32(req, 0);
        appendBigEndian32(req, uint32_t(script.size()));
        req.insert(req.end(), script.begin(), script.end());
        appendModbusCrc(req);
        sendFrame(req);
        bool ack = false, finished = false;
        std::string output;
        PrintFrameAssembler printFrames;
        auto deadline = SteadyClock::now()
            + std::chrono::milliseconds(executionTimeoutMs > 0 ? executionTimeoutMs : m_timeoutMs);
        auto next = SteadyClock::now();
        while (SteadyClock::now() < deadline) {
            if (!printFrames.pending() && SteadyClock::now() >= next) {
                ByteArray poll { 1, 0x61, 0, 0, 0, 0, 0x02, 0, 0,
                    0, 0, 0, 0, 0 };
                appendModbusCrc(poll);
                sendFrame(poll);
                next = SteadyClock::now() + std::chrono::milliseconds(20);
            }
            auto b = receiveReport(50);
            if (b.empty())
                continue;
            if (!printFrames.pending() && b.size() >= 19 && b[0] == 1
                && b[1] == 0x64 && isValidModbusFrame(b, 19) && b[2] == 0 && b[3] == 0) {
                ack = true;
            } else {
                auto frame = printFrames.append(b);
                // Only Lua channel 0 contains diagnostic text. Other channels carry binary state
                // and UART monitor traffic, which must never enter marker/field parsing.
                if (frame.empty() || frame[2] != 0 || frame[3] != 0)
                    continue;
                size_t len = (size_t(frame[6]) << 8) | frame[7];
                std::string chunk(frame.begin() + 8, frame.begin() + 8 + len);
                output += chunk;
                if (output.size() > 1024 * 1024)
                    throw BridgeError("Diagnostic output exceeds 1 MiB");
                finished = containsDiagnosticMarker(output, begin)
                    && containsDiagnosticMarker(output, endMarker);
            }
            if (ack && finished)
                return trimText(output);
        }
        if (!ack)
            throw BridgeError("No Lua acknowledgement; close vendor application and retry");
        throw BridgeError("Lua diagnostic output incomplete: " + output);
    }
};
/**
 * @brief 整理带偏移的目标内存输出并检查完整性。
 * @param raw Lua 原始输出。
 * @param addr 请求地址。
 * @param length 请求字节数。
 * @return raw/format/data；data.ok 仅在读取成功且数量精确匹配时为 true。
 * @throws BridgeError 字节格式错误、分段缺失或分段重叠。
 */
Json parseMemoryOutput(const std::string& raw, uint32_t addr, int length)
{
    std::istringstream lines(raw);
    std::string line;
    std::map<int, std::vector<std::string>> chunks;
    Json data = { { "address", formatHex(addr) }, { "length", length } };
    while (std::getline(lines, line)) {
        line = trimText(line);
        if (line.rfind("read=", 0) == 0) {
            try {
                data["read"] = std::stoi(line.substr(5));
            } catch (...) {
                data["read"] = line.substr(5);
            }
        }
        std::smatch m;
        if (std::regex_match(line, m, std::regex("data\\[([0-9]+)\\]=(.*)"))) {
            std::istringstream bytes(m[2]);
            std::string v;
            int o = std::stoi(m[1]);
            while (bytes >> v) {
                if (!std::regex_match(v, std::regex("[0-9A-Fa-f]{2}")))
                    throw BridgeError("Invalid memory hex output");
                chunks[o].push_back(v);
            }
        }
    }
    Json bytes = Json::array();
    std::string joined;
    int offset = 0;
    for (auto& kv : chunks) {
        if (kv.first != offset)
            throw BridgeError("Missing/overlapping memory chunk");
        for (auto& v : kv.second) {
            if (!joined.empty())
                joined += ' ';
            joined += v;
            bytes.push_back(v);
            ++offset;
        }
    }
    data["data_bytes"] = bytes;
    data["data_hex"] = joined;
    data["ok"] = data.value("read", Json()) == 1 && offset == length;
    return { { "raw", raw }, { "format", "h7tool_read_memory" }, { "data", data } };
}
/**
 * @brief 构造 pg_init/pg_read_mem 的只读目标内存脚本。
 * @param addr 已校验的目标地址。
 * @param length 已校验的读取长度。
 * @return 含成对 H7TOOL_MEM 标记及 16 字节打印分段的 Lua。
 * @note pg_init 可以没有返回值，必须先保存局部变量再转换字符串。
 */
std::string memoryReadScript(uint32_t addr, int length)
{
    std::ostringstream s;
    s << "print(\"H7TOOL_MEM_BEGIN\")\nlocal A=" << formatHex(addr) << "\nlocal L=" << length
      << R"lua(
if pg_init then local r=pg_init() print("pg_init="..tostring(r)) end
print(string.format("address=0x%08X",A))
print("length="..L)
if pg_read_mem then
 local ok,b=pg_read_mem(A,L)
 print("read="..ok)
 if ok==1 and b then
  for o=1,#b,16 do
   local r=""
   for i=o,math.min(o+15,#b) do r=r..string.format("%02X ",string.byte(b,i)) end
   print(string.format("data[%d]=",o-1)..r)
  end
 end
else print("read=unavailable") end
print("H7TOOL_MEM_END")
)lua";
    return s.str();
}
#include "h7tooladvanced.h"
#include "h7toolschemas.h"
#include "h7tooltests.h"

Json toolDefinitions();
/**
 * @brief 检查工具存在、必填字段和未知参数。
 * @param name 外部 MCP 工具名。
 * @param args JSON 参数对象。
 * @throws BridgeError 未知工具、缺少必填参数或额外字段。
 * @note 类型、取值范围和跨字段约束由各工具生成器进一步验证。
 */
void validateToolArguments(const std::string& name, const Json& args)
{
    for (const auto& tool : toolDefinitions()) {
        if (tool["name"] != name)
            continue;
        const auto& schema = tool["inputSchema"];
        for (const auto& required : schema.value("required", Json::array()))
            if (!args.contains(required.get<std::string>()))
                throw BridgeError("Missing required argument: " + required.get<std::string>());
        for (auto it = args.begin(); it != args.end(); ++it)
            if (!schema["properties"].contains(it.key()))
                throw BridgeError("Unknown argument: " + it.key());
        return;
    }
    throw BridgeError("Unknown tool: " + name);
}
/** @brief 持有配置的 MCP 工具服务；不常驻打开硬件，按调用创建会话。 */
class H7ToolMcpServer {
    Json m_config; ///< 配置快照；工具调用期间不修改。
    fs::path m_configPath; ///< 用于状态输出的配置文件绝对/显式路径。

public:
    /**
     * @brief 保存配置与路径，确认适配器是 h7tool_hid。
     * @param config 配置对象的所有权转移到服务。
     * @param configPath 用于 bridge_status 显示。
     * @throws BridgeError 根类型或适配器类型不兼容。
     */
    H7ToolMcpServer(Json config, fs::path configPath)
        : m_config(std::move(config))
        , m_configPath(std::move(configPath))
    {
        if (!m_config.is_object())
            throw BridgeError("Config root must be an object");
        if (m_config.value("adapter", Json::object()).value("type", std::string("h7tool_hid"))
            != "h7tool_hid")
            throw BridgeError("This C++ version supports adapter.type=h7tool_hid");
    }
    /**
     * @brief 执行核心或高级 MCP 工具，统一处理参数和结果。
     * @param name MCP 工具名。
     * @param args 调用参数。
     * @return 结构化工具结果；外层 JSON-RPC 包装由 dispatchRequest 完成。
     * @throws BridgeError 硬件错误或参数错误。
     * @note dry_run 采用延迟创建传输，不打开设备；RTT 共享句柄并分块读取。
     */
    Json callTool(const std::string& name, const Json& args)
    {
        if (!args.is_object())
            throw BridgeError("arguments must be an object");
        validateToolArguments(name, args);
        if (name == "bridge_status")
            return { { "server", { { "name", "h7tool-mcp-cpp" }, { "version", "0.2.1" } } },
                { "adapter", "h7tool_hid" }, { "config_path", m_configPath.u8string() },
                { "h7tool_hid_devices", hidDeviceList(m_config) } };
        if (name == "uart_transact" || name == "can_transact" || name == "i2c_transact"
            || name == "spi_transact" || name == "rtt_read") {
            const int timeoutMs = boundedInteger(
                m_config.value("adapter", Json::object()), "timeout_ms", 8000, 100, 60000);
            // lazy transport: dry_run 和参数校验失败时绝不打开 HID。
            std::unique_ptr<HidTransport> transport;
            auto runLua = [&](const H7ToolAdvanced::LuaRequest& request) {
                if (!transport)
                    transport = std::make_unique<HidTransport>(m_config);
                return transport->runLuaScript(
                    request.script, request.beginMarker, request.endMarker, request.timeoutMs);
            };
            auto readMemory = [&](uint32_t targetAddress, int length) {
                H7ToolAdvanced::LuaRequest request { memoryReadScript(targetAddress, length),
                    "H7TOOL_MEM_BEGIN", "H7TOOL_MEM_END", "generated/read_memory.lua", timeoutMs };
                auto raw = runLua(request);
                auto parsed = parseMemoryOutput(raw, targetAddress, length);
                if (!parsed["data"].value("ok", false))
                    throw BridgeError("RTT target memory read failed");
                return H7ToolAdvanced::parseHexBytes(parsed["data"]["data_hex"]);
            };
            if (name == "rtt_read")
                return H7ToolAdvanced::readRtt(args, m_config, runLua, readMemory);
            auto request = H7ToolAdvanced::transactionRequest(name, args, timeoutMs);
            if (H7ToolAdvanced::booleanArgument(args, "dry_run"))
                return { { "dry_run", true }, { "tool", name }, { "script", request.script },
                    { "script_bytes", request.script.size() },
                    { "timeout_ms", request.timeoutMs } };
            return H7ToolAdvanced::transactionResult(name, request, runLua(request));
        }
        if (name != "tool_status" && name != "tool_registers" && name != "read_memory"
            && name != "target_probe" && name != "target_identity")
            throw BridgeError("Unknown tool: " + name);
        int unit = boundedInteger(m_config.value("adapter", Json::object()), "unit_id", 1, 0, 255);
        if (name == "tool_registers") {
            auto addr = parseAddress(args.at("address"));
            if (addr > 65535)
                throw BridgeError("Register address must be 16 bit");
            int count = boundedInteger(args, "count", 1, 1, 60);
            HidTransport h(m_config);
            auto values = h.readHoldingRegisters(int(addr), count, unit);
            Json hx = Json::array();
            for (int v : values)
                hx.push_back(formatHex(v, 4));
            return { { "transport", "h7tool_hid/function_03/read_holding_registers" },
                { "address", formatHex(addr, 4) }, { "count", count }, { "values_u16", values },
                { "values_hex", hx } };
        }
        if (name == "tool_status") {
            HidTransport h(m_config);
            auto id = h.readHoldingRegisters(0, 12, unit), a = h.readHoldingRegisters(12, 20, unit);
            Json measurements = Json::object();
            const char* names[] = { "ch1_v", "ch2_v", "high_side_v", "high_side_a", "tvcc_v",
                "tvcc_a", "ntc_ohm", "ntc_c", "usb_5v", "external_power_v" };
            for (int i = 0; i < 10; ++i) {
                uint32_t bits = (uint32_t(a[2 * i]) << 16) | uint32_t(a[2 * i + 1]);
                float f;
                std::memcpy(&f, &bits, 4);
                measurements[names[i]] = std::isfinite(f) ? Json(f) : Json();
            }
            std::string uid;
            for (int i = 0; i < 6; i += 2)
                uid += formatHex(id[i + 1], 4).substr(2) + formatHex(id[i], 4).substr(2);
            Json data
                = { { "register_map", "V2.33 compatible; verify semantics for other firmware" },
                      { "device_id_hex", uid }, { "hardware_model", id[6] },
                      { "app_version_raw", formatHex(id[7], 4) },
                      { "app_version",
                          std::to_string(id[7] >> 8) + "." + std::to_string((id[7] & 255) >> 4)
                              + std::to_string(id[7] & 15) },
                      { "gpio_inputs_bits", formatHex((uint32_t(id[8]) << 16) | id[9]) },
                      { "gpio_outputs_bits", formatHex((uint32_t(id[10]) << 16) | id[11]) },
                      { "measurements", measurements } };
            return { { "transport", "h7tool_hid/function_03/read_holding_registers" },
                { "data", data } };
        }
        if (name == "read_memory") {
            auto addr = parseAddress(args.at("address"));
            int max = boundedInteger(
                m_config.value("limits", Json::object()), "max_read_memory_bytes", 1024, 1, 65536);
            int length = boundedInteger(args, "length", 1, 1, max);
            if (uint64_t(addr) + uint64_t(length) > 0x100000000ULL)
                throw BridgeError("Memory range exceeds 32-bit address space");
            HidTransport h(m_config);
            auto raw = h.runLuaScript(
                memoryReadScript(addr, length), "H7TOOL_MEM_BEGIN", "H7TOOL_MEM_END");
            return { { "transport", "h7tool_hid/function_64_lua + function_61_print_poll" },
                { "script", "generated/read_memory.lua" },
                { "result", parseMemoryOutput(raw, addr, length) } };
        }
        auto t = m_config.value("target", Json::object());
        auto idaddr = parseAddress(t.value("idcode_address", Json("0x5C001000"))),
             cpuaddr = parseAddress(t.value("cpuid_address", Json("0xE000ED00"))),
             uidaddr = parseAddress(t.value("uid_address", Json("0x1FF1E800")));
        int uidlen = boundedInteger(t, "uid_length", 12, 1, 32);
        if (uint64_t(uidaddr) + uidlen > 0x100000000ULL || idaddr > 0xFFFFFFFC
            || cpuaddr > 0xFFFFFFFC)
            throw BridgeError("Target probe range exceeds address space");
        std::ostringstream script;
        script << R"lua(print("H7TOOL_TARGET_BEGIN")
if pg_init then local r=pg_init() print("pg_init="..tostring(r)) end
local function word(k,a)
 local ok,b=pg_read_mem(a,4)
 if ok==1 and b and #b==4 then
  local v=string.byte(b,1)+string.byte(b,2)*256+string.byte(b,3)*65536+string.byte(b,4)*16777216
  print(string.format("%s=0x%08X",k,v))
 else print(k.."=unavailable") end
end
)lua" << "word(\"idcode\","
               << formatHex(idaddr) << ")\nword(\"cpuid\"," << formatHex(cpuaddr)
               << ")\nlocal ok,b=pg_read_mem(" << formatHex(uidaddr) << "," << uidlen << R"lua()
print("uid_read="..ok)
if ok==1 and b then
 local r=""
 for i=1,#b do r=r..string.format("%02X ",string.byte(b,i)) end
 print("uid="..r)
end
print("H7TOOL_TARGET_END")
)lua";
        HidTransport h(m_config);
        auto raw = h.runLuaScript(script.str(), "H7TOOL_TARGET_BEGIN", "H7TOOL_TARGET_END");
        Json data = { { "family", t.value("family", std::string("STM32H7")) },
            { "uid_address", formatHex(uidaddr) }, { "uid_length", uidlen } };
        std::istringstream lines(raw);
        std::string line;
        while (std::getline(lines, line)) {
            auto p = line.find('=');
            if (p == std::string::npos)
                continue;
            auto k = trimText(line.substr(0, p)), v = trimText(line.substr(p + 1));
            if (k == "idcode" || k == "cpuid")
                data[k] = v;
            if (k == "pg_init")
                data[k] = v;
            if (k == "uid_read") {
                try {
                    data[k] = (std::stod(v) == 1.0 ? 1 : 0);
                } catch (...) {
                    data[k] = 0;
                }
            }
            if (k == "uid") {
                data["uid_hex"] = v;
                data["uid_bytes"] = Json::array();
                std::istringstream bytes(v);
                std::string b;
                while (bytes >> b)
                    data["uid_bytes"].push_back(b);
            }
        }
        bool connected = data.value("uid_read", 0) == 1;
        for (auto k : { "idcode", "cpuid" }) {
            auto v = data.value(k, std::string());
            if (v.size() == 10 && v != "0x00000000" && v != "0xFFFFFFFF")
                connected = true;
        }
        data["connected"] = connected;
        return { { "transport", "h7tool_hid/function_64_lua + function_61_print_poll" },
            { "script", "generated/target_probe.lua" },
            { "result",
                { { "raw", raw }, { "format", "h7tool_target_probe" }, { "data", data } } } };
    }
};
/**
 * @brief 构造六个核心工具与五个高级工具的 schema。
 * @return 与既有 MCP 工具名兼容的 11 项数组。
 * @note 类型校验、范围校验仍在执行路径实施，不能只依赖客户端 schema。
 */
Json toolDefinitions()
{
    Json list = Json::array();
    auto add = [&](const char* n, const char* d, Json props, Json req = Json::array()) {
        list.push_back({ { "name", n }, { "description", d },
            { "inputSchema",
                { { "type", "object" }, { "properties", props }, { "required", req },
                    { "additionalProperties", false } } } });
    };
    add("bridge_status", "Enumerate native Windows HID devices and show bridge configuration",
        Json::object());
    add("tool_status",
        "Read H7-TOOL identity, firmware version and measurements via Modbus "
        "function 03",
        Json::object());
    add("tool_registers", "Read 1..60 tool holding registers",
        { { "address", { { "type", "string" }, { "pattern", "^0x[0-9A-Fa-f]{1,4}$" } } },
            { "count", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 60 } } } },
        Json::array({ "address", "count" }));
    add("read_memory",
        "Read bounded target memory using pg_read_mem; pg_init may initialize "
        "the debug interface",
        { { "address", { { "type", "string" }, { "pattern", "^0x[0-9A-Fa-f]{1,8}$" } } },
            { "length", { { "type", "integer" }, { "minimum", 1 } } } },
        Json::array({ "address", "length" }));
    add("target_probe",
        "Probe configured target addresses; defaults assume STM32H7. Initializes "
        "the debug interface",
        Json::object());
    add("target_identity", "Read configured target IDCODE, CPUID and UID (STM32H7 by default)",
        Json::object());
    for (const auto& definition : Json::parse(H7ToolSchemas::advancedTools))
        list.push_back(definition);
    return list;
}
/**
 * @brief 包装 MCP text content，保持内嵌 JSON 可解析。
 * @param data 要序列化的 JSON。
 * @param error 工具执行是否失败。
 * @return content/isError 对象。
 * @note 无效 UTF-8 文本采用替换字符，原始总线字节仍由十六进制字段保存。
 */
Json toolResult(const Json& data, bool error = false)
{
    return { { "content",
                 Json::array({ { { "type", "text" },
                     { "text", data.dump(2, ' ', false, Json::error_handler_t::replace) } } }) },
        { "isError", error } };
}
/**
 * @brief 创建标准 JSON-RPC 错误应答。
 * @param id 客户端请求 ID，解析失败时为 null。
 * @param code JSON-RPC 错误码。
 * @param msg 用户可读的错误原因。
 * @return 完整 JSON-RPC error 对象。
 */
Json jsonRpcError(Json id, int code, const std::string& msg)
{
    return { { "jsonrpc", "2.0" }, { "id", id },
        { "error", { { "code", code }, { "message", msg } } } };
}
/**
 * @brief 分派 initialize/ping/tools 请求，区分协议错误和工具错误。
 * @param server 已配置的工具服务。
 * @param r 解析后的请求。
 * @return 应答对象；合法通知返回 null，不输出响应。
 * @note 工具异常转换为 isError=true，协议格式错误使用 JSON-RPC error。
 */
Json dispatchRequest(H7ToolMcpServer& server, const Json& r)
{
    if (!r.is_object() || r.value("jsonrpc", Json()) != "2.0" || !r.contains("method")
        || !r["method"].is_string())
        return jsonRpcError(
            r.is_object() ? r.value("id", Json()) : Json(), -32600, "Invalid Request");
    if (!r.contains("id"))
        return Json();
    auto id = r["id"];
    if (!id.is_null() && !id.is_string() && !id.is_number_integer())
        return jsonRpcError(Json(), -32600, "Invalid request id");
    auto method = r["method"].get<std::string>();
    Json p = r.value("params", Json::object());
    if (!p.is_object())
        return jsonRpcError(id, -32602, "params must be an object");
    Json out;
    try {
        if (method == "initialize") {
            auto version = p.value("protocolVersion", std::string("2024-11-05"));
            if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18")
                version = "2024-11-05";
            out = { { "protocolVersion", version },
                { "capabilities", { { "tools", { { "listChanged", false } } } } },
                { "serverInfo", { { "name", "h7tool-mcp-cpp" }, { "version", "0.2.1" } } } };
        } else if (method == "ping")
            out = Json::object();
        else if (method == "tools/list")
            out = { { "tools", toolDefinitions() } };
        else if (method == "tools/call") {
            if (!p.contains("name") || !p["name"].is_string()
                || !p.value("arguments", Json::object()).is_object())
                return jsonRpcError(
                    id, -32602, "tools/call requires string name and object arguments");
            try {
                out = toolResult(server.callTool(p["name"], p.value("arguments", Json::object())));
            } catch (const std::exception& e) {
                out = toolResult({ { "error", e.what() } }, true);
            }
        } else
            return jsonRpcError(id, -32601, "Method not found: " + method);
    } catch (const std::exception& e) {
        return jsonRpcError(id, -32602, e.what());
    }
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", out } };
}
/**
 * @brief 同步处理逐行 JSON 或 Content-Length MCP 消息。
 * @param server 工具服务。
 * @return 正常 EOF 返回 0。
 * @note stdin/stdout 设为二进制，帧长度按 UTF-8 字节计算，单消息最多 1 MiB。
 */
int serveStdio(H7ToolMcpServer& server)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (trimText(line).empty())
            continue;
        bool framed = false;
        std::string body = line;
        Json response;
        try {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.rfind("content-length:", 0) == 0) {
                framed = true;
                auto lenText = trimText(line.substr(15));
                if (!std::regex_match(lenText, std::regex("[0-9]+")))
                    throw BridgeError("Invalid Content-Length");
                auto n = std::stoull(lenText);
                if (n > 1024 * 1024)
                    throw BridgeError("Message exceeds 1 MiB");
                bool separator = false;
                while (std::getline(std::cin, line)) {
                    if (trimText(line).empty()) {
                        separator = true;
                        break;
                    }
                }
                if (!separator)
                    throw BridgeError("Missing header separator");
                body.resize(size_t(n));
                std::cin.read(body.data(), std::streamsize(n));
                if (std::cin.gcount() != std::streamsize(n))
                    throw BridgeError("Truncated framed message");
            }
            if (body.size() > 1024 * 1024)
                throw BridgeError("Message exceeds 1 MiB");
            response = dispatchRequest(server, Json::parse(body));
        } catch (const std::exception& e) {
            response = jsonRpcError(Json(), -32700, e.what());
        }
        if (!response.is_null()) {
            auto text = response.dump(-1, ' ', false, Json::error_handler_t::replace);
            if (framed)
                std::cout << "Content-Length: " << text.size() << "\r\n\r\n";
            std::cout << text;
            if (!framed)
                std::cout << '\n';
            std::cout.flush();
        }
        if (!std::cin)
            break;
    }
    return 0;
}
/**
 * @brief 运行纯本地核心及高级回归测试。
 * @return 所有断言通过时返回 0。
 * @throws BridgeError 任一断言失败。
 * @note 不打开硬件；高级测试使用回调验证 RTT 回绕、分块及消费顺序。
 */
int runSelfTests()
{
    if (containsDiagnosticMarker("RB\nTHERE\n", "RE"))
        throw BridgeError("Short Lua marker accepted inside another line");
    if (!containsDiagnosticMarker(" RB\r\nRE\n", "RE"))
        throw BridgeError("Complete Lua marker was not recognized");
    // A print frame may span reports; the last report includes padding outside its CRC.
    ByteArray printFrame { 1, 0x61, 0, 0, 0, 0, 0x08, 0x00 };
    for (size_t i = 0; i < 2048; ++i)
        printFrame.push_back(static_cast<unsigned char>(i));
    appendModbusCrc(printFrame);
    PrintFrameAssembler assembler;
    for (size_t offset = 0; offset < printFrame.size(); offset += 1024) {
        auto count = std::min(size_t(1024), printFrame.size() - offset);
        ByteArray report(1024, 0xFF);
        std::copy_n(printFrame.begin() + offset, count, report.begin());
        auto complete = assembler.append(report);
        if (offset + count < printFrame.size()) {
            if (!complete.empty() || !assembler.pending())
                throw BridgeError("Fragmented print response completed prematurely");
        } else if (complete != printFrame || assembler.pending())
            throw BridgeError("Print response reassembly corrupted binary data");
    }
    auto corruptPrint = printFrame;
    corruptPrint[20] ^= 1;
    bool crcRejected = false;
    try { assembler.append(corruptPrint); }
    catch (const BridgeError&) { crcRejected = true; }
    if (!crcRejected)
        throw BridgeError("Corrupt print response accepted");
    bool sizeRejected = false;
    try { assembler.append(ByteArray { 1, 0x61, 0, 0, 0, 1, 0, 0 }); }
    catch (const BridgeError&) { sizeRejected = true; }
    if (!sizeRejected)
        throw BridgeError("32-bit print length was truncated");
    // Channels 2/3 can contain NUL/FF status bytes; preserve their frames for filtering.
    ByteArray statusFrame { 1, 0x61, 0, 2, 0, 0, 0, 2, 0, 0xFF };
    appendModbusCrc(statusFrame);
    if (assembler.append(statusFrame) != statusFrame)
        throw BridgeError("Print channel identity or payload changed");
    runAdvancedSelfTests();
    auto check = [](bool b, const char* msg) {
        if (!b)
            throw BridgeError(msg);
    };
    ByteArray known { 1, 3, 0, 0, 0, 10 };
    appendModbusCrc(known);
    check(known[6] == 0xC5 && known[7] == 0xCD, "CRC vector failed");
    check(isValidModbusFrame(known, 8), "CRC validation failed");
    known[0] ^= 1;
    check(!isValidModbusFrame(known, 8), "CRC corruption accepted");
    H7ToolMcpServer s(Json::object(), { });
    auto r = dispatchRequest(s,
        { { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" },
            { "params", { { "protocolVersion", "2025-06-18" } } } });
    check(r["result"]["protocolVersion"] == "2025-06-18", "Initialize failed");
    check(toolDefinitions().size() == 11, "Tool list failed");
    check(dispatchRequest(s, { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } })
              .is_null(),
        "Notification must be silent");
    check(dispatchRequest(
              s, { { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "bad" } })["error"]["code"]
            == -32601,
        "Unknown method handling failed");
    check(dispatchRequest(s,
              { { "jsonrpc", "2.0" }, { "id", 3 }, { "method", "tools/call" },
                  { "params",
                      { { "name", "read_memory" },
                          { "arguments",
                              { { "address", "0x100000000" },
                                  { "length", 1 } } } } } })["result"]["isError"]
            == true,
        "Invalid address accepted");
    auto m = parseMemoryOutput("read=1\ndata[16]=11 12\ndata[0]=01 02 03 04 05 06 07 "
                               "08 09 0A 0B 0C 0D 0E 0F 10",
        0, 18);
    check(m["data"]["ok"] == true, "Memory chunk sorting failed");
    bool rejected = false;
    try {
        parseMemoryOutput("read=1\ndata[1]=FF", 0, 1);
    } catch (...) {
        rejected = true;
    }
    check(rejected, "Memory gap accepted");
    std::cout << "{\"ok\":true,\"checks\":\"CRC, initialize, notifications, tool "
                 "list, errors, memory parser, advanced buses and RTT\"}\n";
    return 0;
}
/**
 * @brief 解析命令行，加载配置并启动 CLI 或 MCP stdio 服务。
 * @param argc 参数数量。
 * @param argv UTF-8 命令行参数。
 * @return 成功为 0，配置/工具/协议入口异常为 2。
 * @note 未指定动作时运行 MCP；--self-test 不加载配置、不接触设备。
 */
int main(int argc, char** argv)
{
    try {
        wchar_t executablePath[32768] { };
        if (!GetModuleFileNameW(nullptr, executablePath, 32768))
            throw BridgeError("Cannot locate executable");
        fs::path configPath = fs::path(executablePath).parent_path() / "config.json";
        bool explicitConfig = false;
        std::string action, tool, argumentsText = "{}", luaPath;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--config" && i + 1 < argc) {
                configPath = fs::u8path(argv[++i]);
                explicitConfig = true;
            } else if (option == "--lua-file" && i + 1 < argc) {
                action = option;
                luaPath = argv[++i];
            } else if (option == "--call" && i + 1 < argc) {
                action = option;
                tool = argv[++i];
            } else if (option == "--arguments" && i + 1 < argc)
                argumentsText = argv[++i];
            else if (option == "--list-hid-devices" || option == "--probe-h7tool"
                || option == "--target-probe" || option == "--self-test")
                action = option;
            else if (option == "--help") {
                std::cout << "h7tool_mcp [--config PATH] [--list-hid-devices | "
                             "--probe-h7tool | --target-probe | --call TOOL "
                             "--arguments JSON | --lua-file PATH | --self-test]\nWithout an action: MCP "
                             "stdio server (JSON lines or Content-Length).\n";
                return 0;
            } else
                throw BridgeError("Unknown/incomplete option: " + option);
        }
        if (action == "--self-test")
            return runSelfTests();
        Json config = Json::object();
        if (explicitConfig && !fs::exists(configPath))
            throw BridgeError("Config file does not exist: " + configPath.u8string());
        if (fs::exists(configPath)) {
            std::ifstream file(configPath, std::ios::binary);
            if (!file)
                throw BridgeError("Cannot read config");
            file >> config;
        }
        H7ToolMcpServer server(config, configPath);
        if (action == "--list-hid-devices")
            std::cout << hidDeviceList(config).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        else if (action == "--probe-h7tool")
            std::cout << server.callTool("tool_status", Json::object()).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        else if (action == "--target-probe")
            std::cout << server.callTool("target_probe", Json::object()).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        else if (action == "--lua-file") {
            // 本地显式CLI调试入口，不作为自动发现的MCP工具暴露。
            // Lua可能修改目标状态，因此不在读取文件时隐式执行，也不代替调用者的授权范围判断。
            std::ifstream file(fs::u8path(luaPath), std::ios::binary);
            if (!file) throw BridgeError("Cannot read Lua file");
            const std::string script((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (script.empty() || script.size() > 1005) throw BridgeError("Lua file must contain 1..1005 bytes");
            if (script.find('\0') != std::string::npos) throw BridgeError("Lua file contains NUL");
            HidTransport transport(config);
            std::cout << Json({ { "raw", transport.runLuaScript(script, "H7_TEST_BEGIN", "H7_TEST_END") },
                               { "script", luaPath } }).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        }
        else if (action == "--call")
            std::cout << server.callTool(tool, Json::parse(argumentsText)).dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        else
            return serveStdio(server);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << Json({ { "error", exception.what() } }).dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
        return 2;
    }
}
