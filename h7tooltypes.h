#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "third_party/json.hpp"

/** @file h7tooltypes.h
 * @brief 核心与高级模块共享的数据类型和通用函数声明。
 *
 * 使用标准 C++17 实现，不依赖 Qt；命名保持 Qt 常用风格。
 * helper 实现在 h7toolmcp.cpp，头文件自身可独立解析。
 */
using Json = nlohmann::json; ///< MCP/config 数据对象，外部键名不受内部命名约定影响。
using ByteArray = std::vector<unsigned char>; ///< 未解码的字节数组，用于硬件负载和内存内容。

/** @brief 可转换为 MCP isError=true 的预期参数或硬件异常。 */
struct BridgeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** @brief 输出带 0x 前缀的大写十六进制；详见实现处的参数文档。 */
std::string formatHex(uint64_t value, int width = 8);
/** @brief 去除两端空白与 NUL，保留中间内容。 */
std::string trimText(std::string text);
/** @brief 严格解析 0x 前缀的 32 位地址。 */
uint32_t parseAddress(const Json& value);
/** @brief 读取范围内的 JSON 整数，拒绝 bool / float。 */
int boundedInteger(
    const Json& arguments, const char* key, int defaultValue, int minimum, int maximum);
