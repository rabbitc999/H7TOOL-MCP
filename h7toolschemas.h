#pragma once
/** @file h7toolschemas.h
 * @brief 高级 MCP 工具 schema；协议名称保持原 Python 版本兼容。
 */
namespace H7ToolSchemas {
/** @brief 五个高级工具的编译时 JSON schema，dry_run 为 C++ 扩展。 */
inline constexpr const char *advancedTools=R"SCHEMA([
  {
    "name": "uart_transact",
    "description": "Configure one H7-TOOL UART channel, optionally send a short payload, and read a bounded response window.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "channel": {
          "type": "integer",
          "minimum": 1,
          "maximum": 8,
          "default": 1,
          "description": "Lua UART port. H7-TOOL UART1 uses 1; UART2 D0/D1 uses 7."
        },
        "baudrate": {
          "type": "integer",
          "minimum": 1200,
          "maximum": 3000000,
          "default": 115200
        },
        "parity": {
          "type": "integer",
          "enum": [
            0,
            1,
            2
          ],
          "default": 0
        },
        "data_bits": {
          "type": "integer",
          "enum": [
            7,
            8
          ],
          "default": 8
        },
        "stop_bits": {
          "type": "integer",
          "enum": [
            1,
            2
          ],
          "default": 1
        },
        "send_text": {
          "type": "string",
          "description": "UTF-8 text payload to send. Do not combine with send_hex."
        },
        "send_hex": {
          "type": "string",
          "description": "Hex byte payload such as '48 37 0D 0A'. Do not combine with send_text."
        },
        "rx_length": {
          "type": "integer",
          "minimum": 0,
          "maximum": 1024,
          "default": 256
        },
        "timeout_ms": {
          "type": "integer",
          "minimum": 1,
          "maximum": 10000,
          "default": 500
        },
        "dry_run": {
          "type": "boolean",
          "default": false,
          "description": "Validate and return the Lua plan without opening the hardware."
        }
      },
      "additionalProperties": false
    }
  },
  {
    "name": "rtt_read",
    "description": "Find or use a SEGGER RTT control block on the target, then read a bounded window from one up-buffer channel.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "address": {
          "type": "string",
          "pattern": "^0x[0-9A-Fa-f]+$",
          "description": "Optional known RTT control-block address."
        },
        "ranges": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "address": {
                "type": "string",
                "pattern": "^0x[0-9A-Fa-f]+$"
              },
              "length": {
                "type": "integer",
                "minimum": 1,
                "maximum": 2097152
              }
            },
            "required": [
              "address",
              "length"
            ],
            "additionalProperties": false
          }
        },
        "channel": {
          "type": "integer",
          "minimum": 0,
          "maximum": 15,
          "default": 0
        },
        "max_bytes": {
          "type": "integer",
          "minimum": 1,
          "maximum": 2048,
          "default": 512
        },
        "consume": {
          "type": "boolean",
          "default": false,
          "description": "When true, advance the RTT read offset after a successful read."
        }
      },
      "additionalProperties": false
    }
  },
  {
    "name": "can_transact",
    "description": "Open H7-TOOL CAN/CAN-FD with bounded parameters and transmit one bounded frame payload.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "mode": {
          "type": "integer",
          "enum": [
            0,
            1,
            2
          ],
          "default": 0
        },
        "fifo_len": {
          "type": "integer",
          "minimum": 0,
          "maximum": 64,
          "default": 8
        },
        "bitrate": {
          "type": "integer",
          "minimum": 1000,
          "maximum": 8000000,
          "default": 500000
        },
        "data_bitrate": {
          "type": "integer",
          "minimum": 1000,
          "maximum": 8000000
        },
        "frame_id": {
          "type": "string",
          "pattern": "^0x[0-9A-Fa-f]+$",
          "default": "0x321"
        },
        "extended": {
          "type": "boolean",
          "default": false
        },
        "remote": {
          "type": "boolean",
          "default": false
        },
        "data_hex": {
          "type": "string",
          "description": "Hex byte payload such as '01 02 03 04'. Do not combine with data_text."
        },
        "data_text": {
          "type": "string",
          "description": "UTF-8 text payload. Do not combine with data_hex."
        },
        "count": {
          "type": "integer",
          "minimum": 1,
          "maximum": 100,
          "default": 1
        },
        "interval_ms": {
          "type": "integer",
          "minimum": 0,
          "maximum": 10000,
          "default": 0
        },
        "dry_run": {
          "type": "boolean",
          "default": false,
          "description": "Validate and return the Lua plan without opening the hardware."
        }
      },
      "additionalProperties": false
    }
  },
  {
    "name": "i2c_transact",
    "description": "Initialize H7-TOOL I2C, scan 7-bit addresses or perform a bounded write/read transaction.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "clock_hz": {
          "type": "integer",
          "minimum": 1000,
          "maximum": 1000000,
          "default": 100000
        },
        "scan": {
          "type": "boolean",
          "default": false
        },
        "address": {
          "type": "string",
          "pattern": "^0x[0-9A-Fa-f]+$",
          "default": "0x50"
        },
        "write_hex": {
          "type": "string",
          "description": "Hex bytes to write after the address write phase."
        },
        "write_text": {
          "type": "string",
          "description": "UTF-8 bytes to write after the address write phase."
        },
        "read_length": {
          "type": "integer",
          "minimum": 0,
          "maximum": 256,
          "default": 0
        },
        "dry_run": {
          "type": "boolean",
          "default": false,
          "description": "Validate and return the Lua plan without opening the hardware."
        }
      },
      "additionalProperties": false
    }
  },
  {
    "name": "spi_transact",
    "description": "Initialize H7-TOOL SPI and perform one bounded CS-selected write/read transaction.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "freq_id": {
          "type": "integer",
          "minimum": 0,
          "maximum": 7,
          "default": 0,
          "description": "SPI speed index, 0..7 from low to high."
        },
        "phase": {
          "type": "integer",
          "enum": [
            0,
            1
          ],
          "default": 0
        },
        "polarity": {
          "type": "integer",
          "enum": [
            0,
            1
          ],
          "default": 0
        },
        "cs": {
          "type": "integer",
          "enum": [
            0,
            1
          ],
          "default": 0,
          "description": "H7-TOOL chip-select line: 0 for CS0, 1 for CS1."
        },
        "write_hex": {
          "type": "string",
          "description": "Hex bytes to write while CS is asserted."
        },
        "write_text": {
          "type": "string",
          "description": "UTF-8 bytes to write while CS is asserted."
        },
        "read_length": {
          "type": "integer",
          "minimum": 0,
          "maximum": 256,
          "default": 0
        },
        "delay_us": {
          "type": "integer",
          "minimum": 0,
          "maximum": 1000000,
          "default": 0,
          "description": "Delay between write and read, in microseconds."
        },
        "dry_run": {
          "type": "boolean",
          "default": false,
          "description": "Validate and return the Lua plan without opening the hardware."
        }
      },
      "additionalProperties": false
    }
  }
])SCHEMA";
}
