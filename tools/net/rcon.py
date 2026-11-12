"""Send one rcon command to the local native server and print the reply.

py -3 tools/net/rcon.py "codol_csdump build/c1_cs.txt" [port]

The password is R1MOD_RCON_PASSWORD, else r1modlocal (what tools\\dev\\run_native.ps1 sets by default).
"""
import os
import socket
import sys

cmd = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 28960
password = os.environ.get('R1MOD_RCON_PASSWORD', 'r1modlocal')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
s.sendto(b'\xff\xff\xff\xffrcon ' + password.encode() + b' ' + cmd.encode() + b'\n', ('127.0.0.1', port))
try:
    while True:
        data, _ = s.recvfrom(65536)
        print(data[4:].decode('latin-1', 'replace'))
except socket.timeout:
    pass
