import json
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parent
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--executable',type=Path,default=ROOT/'build/h7tool_mcp.exe')
parser.add_argument('--reference',type=Path)
args=parser.parse_args()
EXE=args.executable
CMD = [str(EXE), '--config', str(ROOT / 'config.json')]
def run(payload):
    p = subprocess.run(CMD, input=payload, capture_output=True, timeout=20)
    assert p.returncode == 0, p.stderr.decode(errors='replace')
    assert p.stderr == b'', p.stderr
    return p.stdout
requests = [
    {'jsonrpc':'2.0','id':1,'method':'initialize','params':{'protocolVersion':'2025-06-18'}},
    {'jsonrpc':'2.0','method':'notifications/initialized'},
    {'jsonrpc':'2.0','id':2,'method':'tools/list'},
    {'jsonrpc':'2.0','id':3,'method':'ping'},
    {'jsonrpc':'2.0','id':4,'method':'tools/call','params':{'name':'read_memory','arguments':{'address':'0x100000000','length':4}}},
    {'jsonrpc':'2.0','id':5,'method':'tools/call','params':{'name':'tool_registers','arguments':{'address':'0x0000','count':True}}},
    {'jsonrpc':'2.0','id':6,'method':'tools/call','params':{'name':'target_probe','arguments':{'unexpected':1}}},
    {'jsonrpc':'2.0','id':7,'method':'tools/call','params':{'name':'read_memory','arguments':{'address':'0xFFFFFFFF','length':4}}},
    {'jsonrpc':'2.0','id':8,'method':'tools/call','params':{'name':'read_memory','arguments':{}}},
]
responses = [json.loads(s) for s in run(b''.join(json.dumps(r).encode()+b'\n' for r in requests)).splitlines()]
assert len(responses) == 8
assert responses[0]['result']['protocolVersion'] == '2025-06-18'
assert len(responses[1]['result']['tools']) == 11
assert responses[2]['result'] == {}
assert all(r['result']['isError'] for r in responses[3:])
assert all('error' in json.loads(r['result']['content'][0]['text']) for r in responses[3:])
assert json.loads(run(b'{invalid\n'))['error']['code'] == -32700
assert json.loads(run(b'[]\n'))['error']['code'] == -32600
body = json.dumps({'jsonrpc':'2.0','id':'中文帧','method':'ping'},ensure_ascii=False).encode()
framed = run(b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
header, content = framed.split(b'\r\n\r\n',1)
assert len(content) == int(header.split(b':')[1])
assert json.loads(content)['id'] == '中文帧'
print('PASS: MCP JSON lines, framing, UTF-8, notifications, invalid inputs and range limits')

# 检查所有高级 schema、dry_run 和执行前校验；不打开真实设备。
registered = {tool['name']: tool for tool in responses[1]['result']['tools']}
assert {'uart_transact','can_transact','i2c_transact','spi_transact','rtt_read'} <= registered.keys()
def call_tool(name, arguments):
    request = {'jsonrpc':'2.0','id':101,'method':'tools/call','params':{'name':name,'arguments':arguments}}
    response = json.loads(run(json.dumps(request).encode()+b'\n'))['result']
    return response, json.loads(response['content'][0]['text'])
for tool in ('uart_transact','can_transact','i2c_transact','spi_transact'):
    response, plan = call_tool(tool, {'dry_run':True})
    assert not response['isError'], plan
    assert plan['dry_run'] and plan['script_bytes'] == len(plan['script'].encode())
    assert plan['script_bytes'] + 19 <= 1024
    assert '${' not in plan['script']
invalid_arguments = [
    ('uart_transact', {'dry_run':True,'channel':9}),
    ('uart_transact', {'dry_run':True,'send_hex':'GG'}),
    ('uart_transact', {'dry_run':True,'dry_run_bad':True}),
    ('can_transact', {'dry_run':True,'frame_id':'0x800'}),
    ('can_transact', {'dry_run':True,'extended':'false'}),
    ('can_transact', {'dry_run':True,'data_text':'123456789'}),
    ('i2c_transact', {'dry_run':True,'address':'0x80'}),
    ('i2c_transact', {'dry_run':True,'scan':True,'write_hex':'AA'}),
    ('spi_transact', {'dry_run':True,'phase':2}),
    ('spi_transact', {'dry_run':True,'read_length':257}),
    ('rtt_read', {'address':'0xFFFFFFF0'}),
    ('rtt_read', {'consume':'false'}),
    ('rtt_read', {'ranges':[{'address':'0xFFFFFFF0','length':64}]}),
]
for name, arguments in invalid_arguments:
    response, data = call_tool(name,arguments)
    assert response['isError'] and 'error' in data, (name, arguments, data)
print('PASS: 11 tool schemas, four dry-run plans and advanced argument validation')

# 原Python对照仅在显式提供参考源文件时执行，独立交付不依赖旧Python实现。
if args.reference is None:
    raise SystemExit(0)
import importlib.util
import re
import sys
spec = importlib.util.spec_from_file_location('h7tool_reference', args.reference)
reference = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reference
spec.loader.exec_module(reference)
adapter = reference.H7ToolHidModbusAdapter({})
captured = {}
def capture(script, label, begin_marker, end_marker):
    captured['script'] = script.decode('ascii')
    return {'result':{'raw':''}}
adapter._run_lua_script = capture

def normalize(script):
    script = '\n'.join(line.strip() for line in script.splitlines() if line.strip())
    return re.sub(r'0x0*([0-9A-Fa-f]+)',lambda match:'0x'+match[1].upper(),script)
comparisons = [
    ('uart_transact', {'send_text':'Hi'}, lambda: adapter.run_uart_transact_script(1,115200,0,8,1,b'Hi',256,500)),
    ('can_transact', {'data_hex':'01 02'}, lambda: adapter.run_can_transact_script(0,8,500000,500000,0x321,0,0,b'\x01\x02',1,0)),
    ('i2c_transact', {'write_hex':'01','read_length':2}, lambda: adapter.run_i2c_transact_script(100000,False,0x50,b'\x01',2)),
    ('i2c_transact', {'scan':True}, lambda: adapter.run_i2c_transact_script(100000,True,0x50,b'',0)),
    ('spi_transact', {'write_hex':'9F','read_length':3}, lambda: adapter.run_spi_transact_script(0,0,0,0,b'\x9F',3,0)),
]
for name, arguments, generate_reference in comparisons:
    generate_reference()
    _, plan = call_tool(name,dict(arguments,dry_run=True))
    assert normalize(plan['script']) == normalize(captured['script']), name
print('PASS: UART, CAN, I2C scan/transaction and SPI Lua match the Python reference')
