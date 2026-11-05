#pragma once
/** @file h7toolluatemplates.h
 *  @brief 从原 Python 实现移植的 H7-TOOL Lua 诊断模板（编译时嵌入）。
 *  ${name} 占位符仅由 C++ 单次扫描替换；用户数据必须先转义成 Lua 字符串。
 *  Lua API 保留固件原名，包括 uart_recive / i2c_bus("recive") 的拼写。
 *  运行时不加载 Python，也不依赖外部模板文件。
 */
namespace H7ToolLuaTemplates {
/** @brief uartTransaction 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *uartTransaction = R"H7LUA(print("H7TOOL_UART_BEGIN")
local COM=${channel}
local BAUD=${baudrate}
local PARITY=${parity}
local DATABITS=${data_bits}
local STOPBITS=${stop_bits}
local TX=${tx_literal}
local RX_LEN=${rx_length}
local TIMEOUT=${timeout_ms}
local function hx(b)
 local r=""
 for i=1,string.len(b),1 do
  r=r..string.format("%02X",string.byte(b,i))
  if i<string.len(b) then r=r.." " end
 end
 return r
end
if COM==7 and gpio_cfg then
 gpio_cfg(0,5)
 gpio_cfg(1,5)
end
uart_cfg(COM,BAUD,PARITY,DATABITS,STOPBITS)
print("configured=1")
print("channel="..COM)
print("baudrate="..BAUD)
print("parity="..PARITY)
print("data_bits="..DATABITS)
print("stop_bits="..STOPBITS)
if string.len(TX)>0 then uart_send(COM,TX) end
print("tx_len="..string.len(TX))
local rx_len,rx_data=uart_recive(COM,RX_LEN,TIMEOUT)
if rx_len==nil then rx_len=0 end
if rx_data==nil then rx_data="" end
print("rx_len="..rx_len)
print("rx_hex="..hx(rx_data))
print("H7TOOL_UART_END")
)H7LUA";

/** @brief canTransaction 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *canTransaction = R"H7LUA(print("H7TOOL_CAN_BEGIN")
local MODE=${mode}
local FIFO=${fifo_len}
local BITRATE=${bitrate}
local DBITRATE=${data_bitrate}
local ID=0x${frame_id}
local EXT=${extended}
local REMOTE=${remote}
local TX=${tx_literal}
local COUNT=${count}
local INTERVAL=${interval_ms}
can_bus("open",MODE,FIFO,BITRATE,DBITRATE)
print("opened=1")
print("mode="..MODE)
print("fifo_len="..FIFO)
print("bitrate="..BITRATE)
print("data_bitrate="..DBITRATE)
print(string.format("frame_id=0x%X",ID))
print("extended="..EXT)
print("remote="..REMOTE)
print("tx_len="..string.len(TX))
print("tx_hex=${tx_hex}")
for i=1,COUNT,1 do
 can_bus("send",EXT,REMOTE,ID,TX)
 if INTERVAL>0 and i<COUNT then delayms(INTERVAL) end
end
print("tx_count="..COUNT)
print("interval_ms="..INTERVAL)
print("H7TOOL_CAN_END")
)H7LUA";

/** @brief i2cTransaction 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *i2cTransaction = R"H7LUA(print("H7TOOL_I2C_BEGIN")
local CLK=${clock_hz}
local ADDR=0x${address}
local TX=${write_literal}
local RL=${read_length}
local function hx(b)
 local r=""
 for i=1,string.len(b),1 do r=r..string.format("%02X",string.byte(b,i));if i<string.len(b) then r=r.." " end end
 return r
end
i2c_bus("init",CLK)
print("initialized=1")
print("clock_hz="..CLK)
print(string.format("address=0x%02X",ADDR))
print("write_len="..string.len(TX))
i2c_bus("start")
local ack=i2c_bus("send",ADDR*2)
print("ack[0]="..ack)
for i=1,string.len(TX),1 do
 ack=i2c_bus("send",string.byte(TX,i))
 print(string.format("ack[%d]=%s",i,ack))
end
if RL>0 then
 i2c_bus("start")
 ack=i2c_bus("send",ADDR*2+1)
 print(string.format("ack[%d]=%s",string.len(TX)+1,ack))
 local rx=i2c_bus("recive",RL)
 if rx==nil then rx="" end
 print("read_len="..string.len(rx))
 print("rx_hex="..hx(rx))
else
 print("read_len=0")
end
i2c_bus("stop")
print("H7TOOL_I2C_END")
)H7LUA";

/** @brief i2cScan 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *i2cScan = R"H7LUA(print("H7TOOL_I2C_BEGIN")
local CLK=${clock_hz}
i2c_bus("init",CLK)
print("initialized=1")
print("clock_hz="..CLK)
local c=0
for a=1,127,1 do
 i2c_bus("start")
 local ack=i2c_bus("send",a*2)
 i2c_bus("stop")
 if ack==0 then print(string.format("found=0x%02X",a));c=c+1 end
end
print("scan_count="..c)
print("H7TOOL_I2C_END")
)H7LUA";

/** @brief spiTransaction 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *spiTransaction = R"H7LUA(print("H7TOOL_SPI_BEGIN")
local F=${freq_id}
local PH=${phase}
local PO=${polarity}
local CS=${cs}
local TX=${write_literal}
local RL=${read_length}
local D=${delay_us}
local function hx(b)
 local r=""
 for i=1,string.len(b),1 do r=r..string.format("%02X",string.byte(b,i));if i<string.len(b) then r=r.." " end end
 return r
end
gpio_cfg(0,1)
gpio_cfg(1,1)
gpio_write(0,1)
gpio_write(1,1)
spi_bus("init",F,PH,PO)
print("initialized=1")
print("freq_id="..F)
print("phase="..PH)
print("polarity="..PO)
print("cs="..CS)
print("write_len="..string.len(TX))
print("tx_hex=${tx_hex}")
if CS==0 or CS==1 then gpio_write(CS,0) end
if string.len(TX)>0 then spi_bus("send",TX) end
if D>0 then delayus(D) end
local rx=""
if RL>0 then rx=spi_bus("recive",RL);if rx==nil then rx="" end end
if CS==0 or CS==1 then gpio_write(CS,1) end
print("read_len="..string.len(rx))
print("rx_hex="..hx(rx))
print("delay_us="..D)
print("H7TOOL_SPI_END")
)H7LUA";

/** @brief rttDescriptor 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *rttDescriptor = R"H7LUA(P=print;F=string.format;C=0x${control_block_address};H=${channel};P("RB");function U(o)return(B:byte(o)or 0)+(B:byte(o+1)or 0)*256+(B:byte(o+2)or 0)*65536+(B:byte(o+3)or 0)*16777216 end;if pg_init then pg_init()end;o,B=pg_read_mem(C,24);if o~=1 or B==nil or B:sub(1,10)~="SEGGER RTT"then P("found=0");P(F("control_block=0x%08X",C));P("read_len=0");P("RE");return end;P("found=1");P(F("control_block=0x%08X",C));M=U(17);P("max_up_buffers="..M);P("max_down_buffers="..U(21));P("channel="..H);if H>=M then P("read_len=0");P("RE");return end;o,B=pg_read_mem(C+24+H*24,24);if o~=1 or B==nil then P("read_len=0");P("RE");return end;P(F("name_address=0x%08X",U(1)));P(F("buffer_address=0x%08X",U(5)));P("buffer_size="..U(9));P("write_offset="..U(13));P("read_offset="..U(17));P(F("flags=0x%08X",U(21)));P("RE"))H7LUA";

/** @brief rttScan 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *rttScan = R"H7LUA(print("H7TOOL_RTT_SCAN_BEGIN")
local R={${lua_ranges}};local S="SEGGER RTT";local O=#S-1
if pg_init then pg_init() end
for i=1,#R do local p=0;local v="";while p<R[i][2] do local n=512;if p+n>R[i][2] then n=R[i][2]-p end;local o,b=pg_read_mem(R[i][1]+p,n);if o==1 and b then local x=v..b;local a=string.find(x,S,1,true);if a then print("found=1");print(string.format("control_block=0x%08X",R[i][1]+p-#v+a-1));print("H7TOOL_RTT_SCAN_END");return end;if #x>O then v=string.sub(x,#x-O+1) else v=x end else v="" end;p=p+n end end
print("found=0")
print("H7TOOL_RTT_SCAN_END")
)H7LUA";

/** @brief rttConsume 的固定 Lua 模板；参数由对应请求生成器校验。 */
inline constexpr const char *rttConsume = R"H7LUA(print("H7TOOL_RTT_CONSUME_BEGIN")
if pg_write32 then pg_write32(0x${consume_address},${new_read_offset});print("consume_written=1") else print("consume_written=0") end
print("H7TOOL_RTT_CONSUME_END")
)H7LUA";

} // namespace H7ToolLuaTemplates
