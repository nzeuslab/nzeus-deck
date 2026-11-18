"""Run in WSL. Compile existing SDK headers; never opens a device."""
import hashlib
import json
import struct
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
sdk = Path('/home/nzeus/r36s/kernel/linux-4.4.186')
prefix = '/home/nzeus/r36s/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-'
output = root / 'build'
output.mkdir(exist_ok=True)
subprocess.run([prefix + 'gcc', '-EL', '-mips32r2', '-c', '-I' + str(sdk / 'include/uapi'),
                str(root / 'audit-audio-abi.c'), '-o', str(output / 'audio-abi.o')], check=True)
subprocess.run([prefix + 'objcopy', '-O', 'binary', '-j', '.rodata',
                str(output / 'audio-abi.o'), str(output / 'audio-abi.bin')], check=True)
values = struct.unpack_from('<7I', (output / 'audio-abi.bin').read_bytes())
assert values[0] == 4 and values[1] == 608 and values[4] == 16, values
assert values[2] != values[3], 'Different structures must produce different ioctl numbers'
assert values[2] ^ values[3] == (608 ^ 632) << 16
names = ['pointer_bytes', 'sdk_audio_config_bytes', 'sdk_init_ioctl',
         'handoff_632_byte_init_ioctl', 'packet_header_bytes',
         'extradata_pointer_offset', 'buffering_start_offset']
report = dict(zip(names, values))
for name in ['sdk_init_ioctl', 'handoff_632_byte_init_ioctl']:
    report[name] = hex(report[name])
report['status'] = 'SDK_MIPS_ABI_VERIFIED_ON_PC; LIVE_632_BYTE_BEHAVIOR_FROM_HANDOFF'
report['source_sha256'] = {
    str(p.relative_to(sdk)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in [sdk / 'include/uapi/hcuapi/auddec.h',
              sdk / 'drivers/hcdrivers/avp-proxy/avp-proxy.c']
}
(root / 'audio-abi.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
