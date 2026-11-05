#pragma once
#include "h7tooltypes.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <set>
#include "h7toolluatemplates.h"

/** @file h7tooladvanced.h
 * @brief UART / CAN / I2C / SPI / RTT 的纯 C++ 参数校验、Lua 生成与结果解析。
 *
 * 命名遵循 Qt 常用约定：类型 UpperCamelCase，函数及局部变量 lowerCamelCase。
 * Json 键和 Lua 固件函数属于外部协议，保留 Python 版本的 snake_case 名称。
 * 传输通过回调注入，便于在不连接设备时测试 RTT 环形缓冲区与错误处理。
 */
namespace H7ToolAdvanced {

/** @brief 已校验的 Lua 请求；脚本始终包含成对输出标记。
 * timeoutMs 是脚本执行时间预算，不是 UART 每次接收等待时间。
 */
struct LuaRequest {
    std::string script;
    std::string beginMarker;
    std::string endMarker;
    std::string scriptLabel;
    int timeoutMs = 8000;
};
/** @brief 同步执行诊断脚本并返回原始 UTF-8 输出；失败必须抛 BridgeError。 */
using LuaRunner = std::function<std::string(const LuaRequest&)>;
/** @brief 精确读取指定目标地址；回调返回字节数必须等于请求长度。 */
using MemoryReader = std::function<ByteArray(uint32_t, int)>;

/**
 * @brief 读取严格布尔参数，避免字符串 "false" 被当作 true。
 * @param arguments MCP 参数对象。
 * @param key 外部协议参数名。
 * @param defaultValue 参数未出现时使用的值。
 * @return 已校验的布尔值。
 * @throws BridgeError 参数不是 JSON boolean。
 */
inline bool booleanArgument(const Json& arguments, const char* key, bool defaultValue = false)
{
    if (!arguments.contains(key))
        return defaultValue;
    if (!arguments[key].is_boolean())
        throw BridgeError(std::string(key) + " must be boolean");
    return arguments[key].get<bool>();
}

/** @brief 读取字符串参数；空字符串有效，其他 JSON 类型会报错。 */
inline std::string stringArgument(const Json& arguments, const char* key)
{
    if (!arguments.contains(key))
        return { };
    if (!arguments[key].is_string())
        throw BridgeError(std::string(key) + " must be string");
    return arguments[key].get<std::string>();
}

/**
 * @brief 解析十进制整数或 0x 十六进制字符串，适用于 CAN ID 和 RTT 范围。
 * @param value 输入值；布尔、浮点、负数和带尾随字符的字符串均无效。
 * @param maximum 允许的最大无符号值。
 * @return 已校验的整数。
 * @throws BridgeError 类型、格式或范围错误。
 */
inline uint32_t unsignedArgument(const Json& value, uint32_t maximum = UINT32_MAX)
{
    uint64_t parsedValue = 0;
    if (value.is_number_unsigned())
        parsedValue = value.get<uint64_t>();
    else if (value.is_number_integer()) {
        auto signedValue = value.get<int64_t>();
        if (signedValue < 0)
            throw BridgeError("Unsigned argument cannot be negative");
        parsedValue = uint64_t(signedValue);
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (!std::regex_match(text, std::regex("(?:0x[0-9a-fA-F]+|[0-9]+)")))
            throw BridgeError("Expected decimal or 0x hexadecimal integer");
        try {
            parsedValue = std::stoull(text, nullptr, text.rfind("0x", 0) == 0 ? 16 : 10);
        } catch (...) {
            throw BridgeError("Unsigned argument overflow");
        }
    } else
        throw BridgeError("Expected integer or hexadecimal string");
    if (parsedValue > maximum)
        throw BridgeError("Unsigned argument exceeds allowed range");
    return uint32_t(parsedValue);
}

/**
 * @brief 严格解析字节串；接受连续十六进制及空白分隔形式。
 * @param text 如 "48 37 0D 0A" 或 "48370D0A"。
 * @return 原始字节数组。
 * @throws BridgeError 出现非十六进制字符或奇数个半字节。
 * @note 不静默剔除任意字符，防止用户输入错误转换为另一条硬件命令。
 */
inline ByteArray parseHexBytes(const std::string& text)
{
    std::string digits;
    for (unsigned char character : text) {
        if (std::isspace(character))
            continue;
        if (!std::isxdigit(character))
            throw BridgeError("Payload contains non-hexadecimal character");
        digits += char(character);
    }
    if (digits.size() % 2)
        throw BridgeError("Hex payload requires an even number of digits");
    ByteArray bytes;
    for (size_t offset = 0; offset < digits.size(); offset += 2)
        bytes.push_back(
            static_cast<unsigned char>(std::stoul(digits.substr(offset, 2), nullptr, 16)));
    return bytes;
}

/** @brief 将字节输出为大写、空格分隔的十六进制字符串，不依赖本地编码。 */
inline std::string bytesToHex(const ByteArray& bytes)
{
    std::string text;
    for (auto byte : bytes) {
        if (!text.empty())
            text += ' ';
        text += formatHex(byte, 2).substr(2);
    }
    return text;
}

/**
 * @brief 将任意字节转换为可安全序列化的 UTF-8 文本。
 * @param bytes 原始总线/RTT 字节，允许 NUL 和非 UTF-8 字节。
 * @return 无效编码以 U+FFFD 替换；精确字节仍由 *_hex / *_bytes 保留。
 * @note 先在字符串层使用 JSON 替换模式，再解码，以保持 CLI 和 MCP 行为一致。
 */
inline std::string decodedUtf8Text(const ByteArray& bytes)
{
    const std::string text(bytes.begin(), bytes.end());
    return Json::parse(Json(text).dump(-1, ' ', false, Json::error_handler_t::replace))
        .get<std::string>();
}

/**
 * @brief 从互斥的 text/hex 参数获得发送字节，限制 UTF-8 编码后的实际字节数。
 * @param arguments MCP 参数。
 * @param textKey 文本字段名。
 * @param hexKey 十六进制字段名。
 * @param maximumBytes 允许的最大负载。
 * @return 发送字节；两个参数都缺省时返回空数组。
 * @throws BridgeError 类型错误、两个字段同时非空或负载过长。
 */
inline ByteArray payloadBytes(
    const Json& arguments, const char* textKey, const char* hexKey, size_t maximumBytes)
{
    auto text = stringArgument(arguments, textKey);
    auto hexText = stringArgument(arguments, hexKey);
    if (!text.empty() && !hexText.empty())
        throw BridgeError("Provide only one text/hex payload");
    auto bytes = hexText.empty() ? ByteArray(text.begin(), text.end()) : parseHexBytes(hexText);
    if (bytes.size() > maximumBytes)
        throw BridgeError("Payload exceeds " + std::to_string(maximumBytes) + " bytes");
    return bytes;
}

/**
 * @brief 将任意字节转义成 ASCII Lua 字符串字面量。
 * @return 带双引号的字符串；非打印字节采用固定三位十进制转义。
 * @note 固定三位可避免后续数字被拼入转义值；用户内容不能注入 Lua 语句。
 */
inline std::string luaStringLiteral(const ByteArray& bytes)
{
    std::string text = "\"";
    for (unsigned char byte : bytes) {
        if (byte == '"' || byte == '\\') {
            text += '\\';
            text += char(byte);
        } else if (byte >= 32 && byte <= 126)
            text += char(byte);
        else {
            text += '\\';
            text += char('0' + byte / 100);
            text += char('0' + (byte / 10) % 10);
            text += char('0' + byte % 10);
        }
    }
    return text + '"';
}

/**
 * @brief 单次扫描替换编译时 Lua 模板，并移除每行缩进。
 * @param source 固定模板，含 ${name} 占位符。
 * @param values 已校验并正确转义的替换内容。
 * @return 可直接发送的 ASCII Lua 脚本。
 * @throws BridgeError 模板含未提供的占位符。
 * @note 不重复扫描插入值，用户负载中的 ${...} 保持原样。
 */
inline std::string renderTemplate(
    const std::string& source, const std::map<std::string, std::string>& values)
{
    std::string rendered;
    for (size_t offset = 0; offset < source.size();) {
        if (source.compare(offset, 2, "${") == 0) {
            auto endOffset = source.find('}', offset + 2);
            if (endOffset == std::string::npos)
                throw BridgeError("Unterminated Lua template placeholder");
            auto key = source.substr(offset + 2, endOffset - offset - 2);
            auto value = values.find(key);
            if (value == values.end())
                throw BridgeError("Missing Lua template value: " + key);
            rendered += value->second;
            offset = endOffset + 1;
        } else
            rendered += source[offset++];
    }
    // 只去掉行首行尾空白，不合并语句、不修改字符串内部的空白。
    std::istringstream lines(rendered);
    std::string line;
    std::string script;
    while (std::getline(lines, line)) {
        auto compactLine = trimText(line);
        if (!compactLine.empty())
            script += compactLine + '\n';
    }
    return script;
}

/**
 * @brief 检查单报告 Lua 限制和执行预算，必须在打开 HID 前调用。
 * @note 0x64 帧头 16 字节、终止 NUL 1 字节、CRC 2 字节，共占 19 字节。
 *  固件分段上传未经验证，本实现遵守原 Python 单报告限制，不静默截断负载。
 */
inline void validateLuaRequest(const LuaRequest& request)
{
    if (request.script.size() + 19 > 1024)
        throw BridgeError("Generated Lua needs " + std::to_string(request.script.size() + 19)
            + " bytes; HID payload limit is 1024. Shorten payload/search ranges.");
    if (request.timeoutMs < 100 || request.timeoutMs > 60000)
        throw BridgeError("Lua execution budget must be 100..60000 ms; reduce count/interval");
    if (request.script.find(request.beginMarker) == std::string::npos
        || request.script.find(request.endMarker) == std::string::npos)
        throw BridgeError("Lua template diagnostic markers missing");
}

/**
 * @brief 校验并构造 UART、CAN、I2C 或 SPI 单次事务。
 * @param toolName MCP 工具名（外部名称保持 snake_case）。
 * @param arguments MCP 参数对象。
 * @param baseTimeoutMs adapter.timeout_ms 的缺省执行预算。
 * @return 可供 dry_run 检查或直接执行的请求。
 * @throws BridgeError 参数非法、生成帧过长或事务预计执行时间超过 60 秒。
 * @note CAN 等待时间由 count/interval 推导；UART 预算包含接收超时与输出轮询余量。
 */
inline LuaRequest transactionRequest(
    const std::string& toolName, const Json& arguments, int baseTimeoutMs)
{
    LuaRequest request;
    request.timeoutMs = baseTimeoutMs;
    std::map<std::string, std::string> values;
    auto integer = [&](const char* key, int defaultValue, int minimum, int maximum) {
        int value = boundedInteger(arguments, key, defaultValue, minimum, maximum);
        values[key] = std::to_string(value);
        return value;
    };
    std::string source;
    if (toolName == "uart_transact") {
        integer("channel", 1, 1, 8);
        integer("baudrate", 115200, 1200, 3000000);
        integer("parity", 0, 0, 2);
        integer("data_bits", 8, 7, 8);
        integer("stop_bits", 1, 1, 2);
        integer("rx_length", 256, 0, 1024);
        int receiveTimeout = integer("timeout_ms", 500, 1, 10000);
        request.timeoutMs = std::max(baseTimeoutMs, receiveTimeout + 1500);
        values["tx_literal"]
            = luaStringLiteral(payloadBytes(arguments, "send_text", "send_hex", 512));
        source = H7ToolLuaTemplates::uartTransaction;
        request.beginMarker = "H7TOOL_UART_BEGIN";
        request.endMarker = "H7TOOL_UART_END";
    } else if (toolName == "can_transact") {
        int mode = integer("mode", 0, 0, 2);
        integer("fifo_len", 8, 0, 64);
        int bitrate = integer("bitrate", 500000, 1000, 8000000);
        integer("data_bitrate", bitrate, 1000, 8000000);
        bool extended = booleanArgument(arguments, "extended"),
             remote = booleanArgument(arguments, "remote");
        uint32_t frameId = unsignedArgument(
            arguments.value("frame_id", Json("0x321")), extended ? 0x1FFFFFFF : 0x7FF);
        values["frame_id"] = formatHex(frameId).substr(2);
        values["extended"] = extended ? "1" : "0";
        values["remote"] = remote ? "1" : "0";
        int count = integer("count", 1, 1, 100), interval = integer("interval_ms", 0, 0, 10000);
        int64_t duration = int64_t(count - 1) * interval + 1500;
        if (duration > 60000)
            throw BridgeError("CAN count/interval exceeds 60-second execution budget");
        request.timeoutMs = std::max(baseTimeoutMs, int(duration));
        auto bytes = payloadBytes(arguments, "data_text", "data_hex", mode == 2 ? 64 : 8);
        if (remote && !bytes.empty())
            throw BridgeError("Remote CAN frame cannot contain a data payload");
        if (mode == 2 && remote)
            throw BridgeError("CAN-FD does not support remote frames");
        values["tx_literal"] = luaStringLiteral(bytes);
        values["tx_hex"] = bytesToHex(bytes);
        source = H7ToolLuaTemplates::canTransaction;
        request.beginMarker = "H7TOOL_CAN_BEGIN";
        request.endMarker = "H7TOOL_CAN_END";
    } else if (toolName == "i2c_transact") {
        integer("clock_hz", 100000, 1000, 1000000);
        auto busAddress = unsignedArgument(arguments.value("address", Json("0x50")), 0x7F);
        values["address"] = formatHex(busAddress, 2).substr(2);
        int readLength = integer("read_length", 0, 0, 256);
        auto bytes = payloadBytes(arguments, "write_text", "write_hex", 256);
        bool scan = booleanArgument(arguments, "scan");
        if (scan && (!bytes.empty() || readLength))
            throw BridgeError("I2C scan cannot be combined with write/read payload");
        values["write_literal"] = luaStringLiteral(bytes);
        source = scan ? H7ToolLuaTemplates::i2cScan : H7ToolLuaTemplates::i2cTransaction;
        request.beginMarker = "H7TOOL_I2C_BEGIN";
        request.endMarker = "H7TOOL_I2C_END";
    } else if (toolName == "spi_transact") {
        integer("freq_id", 0, 0, 7);
        integer("phase", 0, 0, 1);
        integer("polarity", 0, 0, 1);
        integer("cs", 0, 0, 1);
        integer("read_length", 0, 0, 256);
        int delayUs = integer("delay_us", 0, 0, 1000000);
        request.timeoutMs = std::max(baseTimeoutMs, delayUs / 1000 + 1500);
        auto bytes = payloadBytes(arguments, "write_text", "write_hex", 256);
        values["write_literal"] = luaStringLiteral(bytes);
        values["tx_hex"] = bytesToHex(bytes);
        source = H7ToolLuaTemplates::spiTransaction;
        request.beginMarker = "H7TOOL_SPI_BEGIN";
        request.endMarker = "H7TOOL_SPI_END";
    } else
        throw BridgeError("Unsupported transaction: " + toolName);
    request.scriptLabel = "generated/" + toolName + ".lua";
    request.script = renderTemplate(source, values);
    validateLuaRequest(request);
    return request;
}

/** @brief 解析固件输出中的整数（兼容 1.0）及 0x 数字；非数值保持字符串。 */
inline Json parseLuaNumber(const std::string& text)
{
    if (std::regex_match(text, std::regex("0x[0-9a-fA-F]{1,8}")))
        return unsignedArgument(text);
    if (std::regex_match(text, std::regex("-?[0-9]+(?:\\.0+)?"))) {
        try {
            return int64_t(std::stoll(text));
        } catch (...) {
        }
    }
    return text;
}

/**
 * @brief 解析 key=value 诊断输出并识别 Lua/固件错误。
 * @param raw 保留原始输出，方便定位错误。
 * @return data 对象，含字段值、errors 和 ordered ACK/设备地址列表。
 * @note RX 文本由十六进制字节重建，避免换行、NUL、等号破坏文本行解析。
 */
inline Json parseDiagnosticFields(const std::string& raw)
{
    Json data = Json::object(), errors = Json::array(), acknowledgements = Json::array(),
         foundAddresses = Json::array();
    std::istringstream lines(raw);
    std::string line;
    std::map<int, Json> ackByIndex;
    while (std::getline(lines, line)) {
        line = trimText(line);
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        if (lower.find("parameter error") != std::string::npos || lower.rfind("error :", 0) == 0
            || lower.find("[string ") != std::string::npos)
            errors.push_back(line);
        auto equalOffset = line.find('=');
        if (equalOffset == std::string::npos)
            continue;
        std::string key = trimText(line.substr(0, equalOffset)),
                    value = trimText(line.substr(equalOffset + 1));
        std::smatch match;
        if (std::regex_search(key, match, std::regex("([A-Za-z_][A-Za-z0-9_\\[\\]]*)$")))
            key = match[1];
        if (key == "found" && value.rfind("0x", 0) == 0)
            foundAddresses.push_back(formatHex(unsignedArgument(value), 2));
        else if (std::regex_match(key, match, std::regex("ack\\[([0-9]+)\\]")))
            ackByIndex[std::stoi(match[1])] = parseLuaNumber(value);
        else if (key == "rx_hex" || key == "tx_hex" || key == "rtt_hex") {
            auto bytes = parseHexBytes(value);
            data[key] = bytesToHex(bytes);
            auto prefix = key.substr(0, key.size() - 4);
            Json byteStrings = Json::array();
            for (auto byte : bytes)
                byteStrings.push_back(formatHex(byte, 2).substr(2));
            data[prefix + "_bytes"] = byteStrings;
            // nlohmann 的替换模式仅处理文本中的无效 UTF-8；原始字节仍由 *_bytes 完整保留。
            if (prefix != "tx")
                data[prefix + "_text"] = decodedUtf8Text(bytes);
        } else if (key != "rx_text")
            data[key] = parseLuaNumber(value);
    }
    for (const auto& entry : ackByIndex)
        acknowledgements.push_back(entry.second);
    if (!errors.empty())
        data["errors"] = errors;
    if (!acknowledgements.empty())
        data["acks"] = acknowledgements;
    if (!foundAddresses.empty())
        data["found_addresses"] = foundAddresses;
    return data;
}

/**
 * @brief 生成与 Python 返回形状兼容的事务结果，校验报告字节数和固件错误。
 * @param toolName 事务名称。
 * @param request 执行的请求。
 * @param raw 固件输出。
 * @return transport/script/result；result.data.ok 表示接口报告成功，而非外部设备端到端确认。
 * @note CAN 发送计数只是固件 API 的调用计数，不代表远端 ACK；与原实现一致。
 */
inline Json transactionResult(
    const std::string& toolName, const LuaRequest& request, const std::string& raw)
{
    auto data = parseDiagnosticFields(raw);
    bool ok = !data.contains("errors");
    if (toolName == "uart_transact")
        ok = ok && data.value("configured", Json()) == 1 && data.contains("rx_len");
    else if (toolName == "can_transact")
        ok = ok && data.value("opened", Json()) == 1 && data.contains("tx_count");
    else
        ok = ok && data.value("initialized", Json()) == 1;
    if (toolName == "i2c_transact" && data.contains("acks"))
        for (const auto& ack : data["acks"])
            ok = ok && ack == 0;
    const char* lengthKey = toolName == "uart_transact" ? "rx_len" : "read_len";
    if (toolName != "can_transact" && !data.contains("scan_count")) {
        if (!data.contains(lengthKey) || !data[lengthKey].is_number_integer())
            ok = false;
        else {
            if (data[lengthKey] == 0 && !data.contains("rx_bytes")) {
                data["rx_bytes"] = Json::array();
                data["rx_hex"] = "";
                data["rx_text"] = "";
            }
            ok = ok && data.contains("rx_bytes") && data[lengthKey] == data["rx_bytes"].size();
        }
    }
    data["ok"] = ok;
    return { { "transport", "h7tool_hid/function_64_lua + function_61_print_poll" },
        { "script", request.scriptLabel },
        { "result", { { "raw", raw }, { "format", "h7tool_" + toolName }, { "data", data } } } };
}

/** @brief 已校验的 RTT 搜索与读取参数；consume 缺省 false，普通读取不修改偏移。 */
struct RttParameters {
    uint32_t controlBlockAddress = 0;
    bool hasAddress = false;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    int channel = 0;
    int maximumBytes = 512;
    bool consume = false;
};

/**
 * @brief 校验 RTT 地址、范围、通道和读取大小。
 * @param arguments 调用参数。
 * @param config 可提供 rtt.ranges；否则使用原 Python 的三个默认 RAM 范围。
 * @return 参数对象；所有地址加长度在 uint64_t 中检查，防止 32 位溢出。
 * @throws BridgeError 越界、空范围、不合法类型或超过 16 个搜索范围。
 */
inline RttParameters rttParameters(const Json& arguments, const Json& config)
{
    RttParameters parameters;
    parameters.channel = boundedInteger(arguments, "channel", 0, 0, 15);
    parameters.maximumBytes = boundedInteger(arguments, "max_bytes", 512, 1, 2048);
    parameters.consume = booleanArgument(arguments, "consume");
    if (arguments.contains("address")) {
        parameters.controlBlockAddress = parseAddress(arguments["address"]);
        parameters.hasAddress = true;
    }
    Json ranges = arguments.value(
        "ranges", config.value("rtt", Json::object()).value("ranges", Json::array()));
    if (!ranges.is_array())
        throw BridgeError("RTT ranges must be an array");
    if (!parameters.hasAddress && ranges.empty())
        ranges = Json::array({ { { "address", "0x20000000" }, { "length", 0x40000 } },
            { { "address", "0x24000000" }, { "length", 0x80000 } },
            { { "address", "0x30000000" }, { "length", 0x40000 } } });
    if (ranges.size() > 16)
        throw BridgeError("RTT accepts at most 16 search ranges");
    for (const auto& range : ranges) {
        if (!range.is_object() || !range.contains("address") || !range.contains("length"))
            throw BridgeError("RTT range requires address and length");
        auto startAddress = unsignedArgument(range["address"]),
             length = unsignedArgument(range["length"], 0x200000);
        if (!length || uint64_t(startAddress) + length > 0x100000000ULL)
            throw BridgeError("RTT search range exceeds address space");
        parameters.ranges.emplace_back(startAddress, length);
    }
    if (parameters.hasAddress
        && uint64_t(parameters.controlBlockAddress) + 24 + uint64_t(parameters.channel + 1) * 24
            > 0x100000000ULL)
        throw BridgeError("RTT descriptor exceeds address space");
    return parameters;
}

/**
 * @brief 计算 RTT 环形缓冲区的可读部分，明确区分尾段与回绕段。
 * @param bufferSize 缓冲区大小。
 * @param writeOffset 生产者写偏移。
 * @param readOffset 消费者读偏移。
 * @param maximumBytes 本次读取上限。
 * @return available/readLength/firstLength/secondLength/newReadOffset。
 * @throws BridgeError 零大小或任一偏移越界；损坏描述符不会触发内存读取/写入。
 */
inline Json ringReadPlan(
    uint32_t bufferSize, uint32_t writeOffset, uint32_t readOffset, int maximumBytes)
{
    if (!bufferSize || writeOffset >= bufferSize || readOffset >= bufferSize)
        throw BridgeError("Invalid RTT ring descriptor");
    uint32_t available = writeOffset >= readOffset ? writeOffset - readOffset
                                                   : bufferSize - readOffset + writeOffset;
    uint32_t readLength = std::min(available, uint32_t(maximumBytes));
    uint32_t firstLength = std::min(readLength, bufferSize - readOffset);
    return { { "available", available }, { "readLength", readLength },
        { "firstLength", firstLength }, { "secondLength", readLength - firstLength },
        { "newReadOffset", uint32_t((uint64_t(readOffset) + readLength) % bufferSize) } };
}

/**
 * @brief 扫描 RTT 控制块、读取所选 up-buffer，并在明确请求时消费数据。
 * @param arguments MCP 参数。
 * @param config 搜索范围与时间预算配置。
 * @param runLua 同步 Lua 传输回调。
 * @param readMemory 精确读取字节的回调；内部按 512 字节分块，支持 2048 字节上限。
 * @return Python 兼容结构，含 descriptor/memory_reads/result.data。
 * @throws BridgeError 描述符损坏、内存读不完整、环形缓冲区溢出或消费写入失败。
 * @note 未发现 RTT 时 found=0、ok=false；不捏造空串成功。
 * @note consume=true 会更新目标 RAM 中的 RdOff；先校验并完整读取所有分段，成功后才提交。
 */
inline Json readRtt(const Json& arguments, const Json& config, const LuaRunner& runLua,
    const MemoryReader& readMemory)
{
    auto parameters = rttParameters(arguments, config);
    int timeoutMs
        = boundedInteger(config.value("adapter", Json::object()), "timeout_ms", 8000, 100, 60000);
    uint32_t controlBlock = parameters.controlBlockAddress;
    Json memoryReads = Json::array();
    auto execute = [&](const char* source, const std::map<std::string, std::string>& values,
                       const char* begin, const char* end, const char* label) {
        LuaRequest request { renderTemplate(source, values), begin, end, label, timeoutMs };
        validateLuaRequest(request);
        return runLua(request);
    };
    auto output = [&](const std::string& raw, Json data, Json descriptor = Json()) {
        return Json { { "transport", "h7tool_hid/function_64_lua + function_61_print_poll" },
            { "script", "generated/rtt_read.lua" }, { "descriptor", descriptor },
            { "memory_reads", memoryReads },
            { "result", { { "raw", raw }, { "format", "h7tool_rtt_read" }, { "data", data } } } };
    };
    if (!parameters.hasAddress) {
        std::string rangeList;
        for (const auto& range : parameters.ranges) {
            if (!rangeList.empty())
                rangeList += ',';
            rangeList += "{" + formatHex(range.first) + "," + formatHex(range.second) + "}";
        }
        auto raw = execute(H7ToolLuaTemplates::rttScan, { { "lua_ranges", rangeList } },
            "H7TOOL_RTT_SCAN_BEGIN", "H7TOOL_RTT_SCAN_END", "generated/rtt_scan.lua");
        auto scanData = parseDiagnosticFields(raw);
        if (scanData.contains("errors"))
            throw BridgeError("RTT scan reported Lua error: " + raw);
        if (scanData.value("found", Json()) != 1)
            return output(raw,
                { { "found", 0 }, { "ok", false }, { "read_len", 0 }, { "rtt_hex", "" },
                    { "rtt_bytes", Json::array() }, { "rtt_text", "" } });
        if (!scanData.contains("control_block"))
            throw BridgeError("RTT scan missing control block address");
        controlBlock = unsignedArgument(scanData["control_block"]);
    }
    // 24-byte header + channel * 24-byte descriptor；读偏移在描述符 +16。
    uint64_t descriptorAddress = uint64_t(controlBlock) + 24 + uint64_t(parameters.channel) * 24;
    if (descriptorAddress + 24 > 0x100000000ULL)
        throw BridgeError("RTT descriptor address overflow");
    auto raw = execute(H7ToolLuaTemplates::rttDescriptor,
        { { "control_block_address", formatHex(controlBlock).substr(2) },
            { "channel", std::to_string(parameters.channel) } },
        "RB", "RE", "generated/rtt_descriptor.lua");
    auto data = parseDiagnosticFields(raw);
    Json descriptor = { { "raw", raw }, { "data", data } };
    if (data.contains("errors"))
        throw BridgeError("RTT descriptor reported Lua error: " + raw);
    data["control_block"] = formatHex(controlBlock);
    data["consume"] = parameters.consume ? 1 : 0;
    data["consume_written"] = 0;
    if (data.value("found", Json()) != 1) {
        data["rtt_bytes"] = Json::array();
        data["rtt_hex"] = "";
        data["rtt_text"] = "";
        data["ok"] = false;
        return output(raw, data, descriptor);
    }
    auto requiredValue = [&](const char* key) {
        if (!data.contains(key))
            throw BridgeError(std::string("RTT descriptor missing ") + key);
        return unsignedArgument(data[key]);
    };
    uint32_t upBuffers = requiredValue("max_up_buffers");
    if (!upBuffers || parameters.channel >= int64_t(upBuffers))
        throw BridgeError("RTT channel outside available up buffers");
    uint32_t bufferAddress = requiredValue("buffer_address"),
             bufferSize = requiredValue("buffer_size"), writeOffset = requiredValue("write_offset"),
             readOffset = requiredValue("read_offset");
    if (!bufferAddress || uint64_t(bufferAddress) + bufferSize > 0x100000000ULL)
        throw BridgeError("RTT buffer address invalid");
    auto plan = ringReadPlan(bufferSize, writeOffset, readOffset, parameters.maximumBytes);
    ByteArray bytes;
    auto appendPart = [&](uint32_t startAddress, uint32_t length) {
        for (uint32_t offset = 0; offset < length;) {
            int chunkLength = int(std::min(uint32_t(512), length - offset));
            auto chunk = readMemory(startAddress + offset, chunkLength);
            if (chunk.size() != size_t(chunkLength))
                throw BridgeError("RTT memory read incomplete; read offset was not changed");
            memoryReads.push_back({ { "address", formatHex(startAddress + offset) },
                { "length", chunkLength }, { "data_hex", bytesToHex(chunk) } });
            bytes.insert(bytes.end(), chunk.begin(), chunk.end());
            offset += uint32_t(chunkLength);
        }
    };
    appendPart(bufferAddress + readOffset, plan["firstLength"].get<uint32_t>());
    appendPart(bufferAddress, plan["secondLength"].get<uint32_t>());
    data["available"] = plan["available"];
    data["read_len"] = bytes.size();
    data["new_read_offset"] = plan["newReadOffset"];
    data["rtt_hex"] = bytesToHex(bytes);
    data["rtt_text"] = decodedUtf8Text(bytes);
    data["rtt_bytes"] = Json::array();
    for (auto byte : bytes)
        data["rtt_bytes"].push_back(formatHex(byte, 2).substr(2));
    if (parameters.consume && !bytes.empty()) {
        // 再读当前 RdOff，防止与另一个 RTT 客户端并发消费时覆盖其进度。
        auto currentOffset = readMemory(uint32_t(descriptorAddress + 16), 4);
        if (currentOffset.size() != 4)
            throw BridgeError("Cannot validate RTT read offset before consume");
        uint32_t actualOffset = uint32_t(currentOffset[0]) | (uint32_t(currentOffset[1]) << 8)
            | (uint32_t(currentOffset[2]) << 16) | (uint32_t(currentOffset[3]) << 24);
        if (actualOffset != readOffset)
            throw BridgeError("RTT read offset changed concurrently; consume aborted");
        auto consumeRaw = execute(H7ToolLuaTemplates::rttConsume,
            { { "consume_address", formatHex(descriptorAddress + 16).substr(2) },
                { "new_read_offset", plan["newReadOffset"].dump() } },
            "H7TOOL_RTT_CONSUME_BEGIN", "H7TOOL_RTT_CONSUME_END", "generated/rtt_consume.lua");
        auto consumeData = parseDiagnosticFields(consumeRaw);
        if (consumeData.contains("errors") || consumeData.value("consume_written", Json()) != 1)
            throw BridgeError("RTT consume did not execute");
        auto committedOffset = readMemory(uint32_t(descriptorAddress + 16), 4);
        uint32_t expectedOffset = plan["newReadOffset"].get<uint32_t>();
        if (committedOffset.size() != 4
            || (uint32_t(committedOffset[0]) | (uint32_t(committedOffset[1]) << 8)
                   | (uint32_t(committedOffset[2]) << 16) | (uint32_t(committedOffset[3]) << 24))
                != expectedOffset)
            throw BridgeError("RTT consume write could not be verified");
        data["consume_written"] = 1;
    }
    data["ok"] = true;
    return output(raw, data, descriptor);
}
} // namespace H7ToolAdvanced
