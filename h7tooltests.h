#pragma once
#include "h7tooladvanced.h"
/** @file h7tooltests.h
 * @brief 不触碰硬件的高级功能回归测试；注入假传输验证环形缓冲区和消费顺序。
 */

/**
 * @brief 执行高级事务生成、解析与 RTT 边界测试。
 * @throws BridgeError 任一断言失败；由 --self-test 和 CTest 调用。
 * @note 这些测试验证可观测行为，不会发送任何 CAN/I2C/SPI 命令到实机。
 */
inline void runAdvancedSelfTests()
{
    using namespace H7ToolAdvanced;
    auto verify = [](bool condition, const char* message) {
        if (!condition)
            throw BridgeError(message);
    };
    auto rejects = [&](const std::function<void()>& operation, const char* message) {
        bool rejected = false;
        try {
            operation();
        } catch (const std::exception&) {
            rejected = true;
        }
        verify(rejected, message);
    };
    for (const auto& name : { "uart_transact", "can_transact", "i2c_transact", "spi_transact" }) {
        auto request = transactionRequest(name, Json::object(), 8000);
        verify(request.script.size() + 19 <= 1024, "Default bus request exceeds HID report");
        verify(request.script.find("${") == std::string::npos, "Unresolved template placeholder");
    }
    verify(decodedUtf8Text(ByteArray { 0x41, 0xFF, 0x42 })
            == std::string("A\xEF\xBF\xBD"
                           "B"),
        "Invalid UTF-8 byte replacement failed");
    auto bytes = parseHexBytes("00 22 5c ff");
    verify(luaStringLiteral(bytes) == "\"\\000\\\"\\\\\\255\"", "Lua byte escaping failed");
    rejects([&] { parseHexBytes("0XG1"); }, "Invalid hex characters accepted");
    rejects([&] { parseHexBytes("123"); }, "Odd hex accepted");
    rejects(
        [&] {
            transactionRequest(
                "uart_transact", { { "send_text", "x" }, { "send_hex", "41" } }, 8000);
        },
        "Conflicting payload accepted");
    rejects([&] { transactionRequest("uart_transact", { { "channel", true } }, 8000); },
        "Boolean integer accepted");
    rejects([&] { transactionRequest("can_transact", { { "frame_id", "0x800" } }, 8000); },
        "Standard CAN ID overflow accepted");
    rejects([&] { transactionRequest("can_transact", { { "data_text", "123456789" } }, 8000); },
        "Classic CAN payload overflow accepted");
    rejects(
        [&] { transactionRequest("can_transact", { { "mode", 2 }, { "remote", true } }, 8000); },
        "CAN-FD remote frame accepted");
    rejects(
        [&] {
            transactionRequest(
                "can_transact", { { "count", 100 }, { "interval_ms", 10000 } }, 8000);
        },
        "CAN excessive duration accepted");
    auto canRequest = transactionRequest("can_transact",
        { { "extended", true }, { "frame_id", "0x1FFFFFFF" }, { "count", 10 },
            { "interval_ms", 1000 } },
        8000);
    verify(canRequest.timeoutMs == 10500, "CAN execution budget not extended");
    auto fdRequest
        = transactionRequest("can_transact", { { "mode", 2 }, { "data_text", "123456789" } }, 8000);
    verify(fdRequest.script.find("123456789") != std::string::npos, "CAN-FD data missing");
    rejects(
        [&] {
            transactionRequest("i2c_transact", { { "scan", true }, { "read_length", 1 } }, 8000);
        },
        "Mixed I2C scan accepted");
    rejects([&] { transactionRequest("spi_transact", { { "cs", 2 } }, 8000); },
        "Invalid SPI CS accepted");
    rejects(
        [&] {
            transactionRequest("uart_transact", { { "send_hex", std::string(1024, '0') } }, 8000);
        },
        "Oversized rendered script accepted");
    verify(renderTemplate("${value}", { { "value", "${value}" } }) == "${value}\n",
        "Template insertion was rescanned");
    auto uartRequest = transactionRequest("uart_transact", Json::object(), 8000);
    auto uart = transactionResult("uart_transact", uartRequest,
        "configured=1.0\nrx_len=3.0\nrx_hex=41 0A 00\nrx_text=ignored\n");
    verify(uart["result"]["data"]["ok"] == true, "UART decimal output parsing failed");
    verify(uart["result"]["data"]["rx_text"].get<std::string>() == std::string("A\n\0", 3),
        "UART binary text reconstruction failed");
    auto broken
        = transactionResult("uart_transact", uartRequest, "configured=1\nrx_len=4\nrx_hex=41\n");
    verify(broken["result"]["data"]["ok"] == false, "Truncated UART output accepted");
    auto badAck = transactionResult("i2c_transact",
        transactionRequest("i2c_transact", Json::object(), 8000),
        "initialized=1\nack[0]=1\nread_len=0\n");
    verify(badAck["result"]["data"]["ok"] == false, "I2C NACK accepted");
    auto firmwareError = transactionResult(
        "uart_transact", uartRequest, "configured=1\nrx_len=0\nparameter error\n");
    verify(firmwareError["result"]["data"]["ok"] == false, "Firmware error ignored");
    verify(ringReadPlan(8, 3, 6, 8)
            == Json({ { "available", 5 }, { "readLength", 5 }, { "firstLength", 2 },
                { "secondLength", 3 }, { "newReadOffset", 3 } }),
        "RTT wrap plan incorrect");
    verify(ringReadPlan(8, 6, 1, 2)["readLength"] == 2, "RTT maximum bytes ignored");
    verify(ringReadPlan(8, 1, 1, 4)["readLength"] == 0, "Empty ring should not read");
    rejects([&] { ringReadPlan(0, 0, 0, 1); }, "Zero RTT ring accepted");
    rejects([&] { ringReadPlan(8, 8, 0, 1); }, "Out-of-range RTT offset accepted");
    rejects(
        [&] {
            rttParameters(
                { { "ranges",
                    Json::array({ { { "address", "0xFFFFFFF0" }, { "length", 32 } } }) } },
                Json::object());
        },
        "RTT range overflow accepted");
    rejects([&] { rttParameters({ { "consume", "false" } }, Json::object()); },
        "String RTT boolean accepted");
    rejects([&] { rttParameters({ { "address", "0xFFFFFFF0" } }, Json::object()); },
        "RTT descriptor overflow accepted");
    // 假目标：8 字节环，从偏移 6 读取，先取尾部 2 字节再取头部 3 字节。
    bool consumeExecuted = false;
    uint32_t currentOffset = 6;
    std::vector<std::pair<uint32_t, int>> memoryCalls;
    LuaRunner fakeLua = [&](const LuaRequest& request) {
        if (request.scriptLabel.find("consume") != std::string::npos) {
            consumeExecuted = true;
            currentOffset = 3;
            return std::string(
                "H7TOOL_RTT_CONSUME_BEGIN\nconsume_written=1\nH7TOOL_RTT_CONSUME_END");
        }
        return std::string("RB\nfound=1\nmax_up_buffers=1\nmax_down_buffers=0\nbuffer_address="
                           "0x20000100\nbuffer_size=8\nwrite_offset=3\nread_offset=6\nRE");
    };
    MemoryReader fakeMemory = [&](uint32_t startAddress, int length) {
        memoryCalls.emplace_back(startAddress, length);
        if (startAddress == 0x20000028 && length == 4)
            return ByteArray { static_cast<unsigned char>(currentOffset), 0, 0, 0 };
        ByteArray data;
        for (int index = 0; index < length; ++index)
            data.push_back(
                static_cast<unsigned char>((startAddress - 0x20000100) + uint32_t(index)));
        return data;
    };
    auto rtt = readRtt({ { "address", "0x20000000" } }, Json::object(), fakeLua, fakeMemory);
    verify(rtt["result"]["data"]["rtt_hex"] == "06 07 00 01 02", "RTT wrapped bytes incorrect");
    verify(
        !consumeExecuted && memoryCalls.size() == 2, "Non-consuming RTT unexpectedly wrote offset");
    rtt = readRtt(
        { { "address", "0x20000000" }, { "consume", true } }, Json::object(), fakeLua, fakeMemory);
    verify(consumeExecuted && rtt["result"]["data"]["consume_written"] == 1,
        "RTT consume not verified");
    consumeExecuted = false;
    MemoryReader truncatedRead = [](uint32_t, int) { return ByteArray { }; };
    rejects(
        [&] {
            readRtt({ { "address", "0x20000000" }, { "consume", true } }, Json::object(), fakeLua,
                truncatedRead);
        },
        "RTT partial read accepted");
    verify(!consumeExecuted, "RTT offset changed after partial read");
    MemoryReader concurrentRead = [&](uint32_t startAddress, int length) {
        if (startAddress == 0x20000028)
            return ByteArray { 7, 0, 0, 0 };
        return fakeMemory(startAddress, length);
    };
    rejects(
        [&] {
            readRtt({ { "address", "0x20000000" }, { "consume", true } }, Json::object(), fakeLua,
                concurrentRead);
        },
        "Concurrent RTT consumer overwritten");
    verify(!consumeExecuted, "Concurrent RTT changed offset");
    LuaRunner notFound = [](const LuaRequest&) {
        return std::string("H7TOOL_RTT_SCAN_BEGIN\nfound=0\nH7TOOL_RTT_SCAN_END");
    };
    rtt = readRtt(Json::object(), Json::object(), notFound, truncatedRead);
    verify(rtt["result"]["data"]["found"] == 0 && rtt["result"]["data"]["ok"] == false,
        "RTT not-found reported success");
    // 2048 字节上限必须分成四次读取，不能超过单次内存通道窗口。
    LuaRunner largeRing = [](const LuaRequest&) {
        return std::string("RB\nfound=1\nmax_up_buffers=1\nbuffer_address=0x20000100\nbuffer_size="
                           "4096\nwrite_offset=3000\nread_offset=0\nRE");
    };
    int chunkCount = 0;
    MemoryReader chunks = [&](uint32_t, int length) {
        verify(length <= 512, "RTT chunk exceeds 512 bytes");
        ++chunkCount;
        return ByteArray(size_t(length), 0x41);
    };
    rtt = readRtt(
        { { "address", "0x20000000" }, { "max_bytes", 2048 } }, Json::object(), largeRing, chunks);
    verify(chunkCount == 4 && rtt["result"]["data"]["read_len"] == 2048, "RTT chunking failed");
}
