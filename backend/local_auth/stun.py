"""Local STUN (RFC 3489 subset) responder for the QA client's address discovery.

The 3.3.3.3 client, after DW logon completes, walks a decrypted STUN config
(server 116.130.229.213:8000 in the shipped build) and sends 24-byte binding
requests until one yields a type-matching response with a 4-byte address
(`sub_B169A0`). When every attempt fails it reports `Address Fail|result = -3`
and, after 3 retries, the DW live manager raises the fatal
`@SF_UI_DW_LIVE_DISCONNECTED` dialog.

This module answers those binding requests on loopback UDP so discovery
succeeds with a reflexive 127.0.0.1 address, which is exactly what a
loopback-only preserved client wants.
"""
import socket
import struct
import threading

STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_RESPONSE = 0x0101
STUN_MAPPED_ADDRESS = 0x0001


def build_binding_response(request, source_ip, source_port, *, now=None):
    """Echo a binding request as a RFC 3489 binding response.

    Keeps the 12-byte transaction id, answers with a MAPPED-ADDRESS whose
    family/port/address match the observed datagram source, and zero-pads to
    a multiple of 4 bytes as RFC 3489 requires.
    """
    if len(request) < 20:
        raise ValueError('Short STUN request')
    req_type = struct.unpack_from('>H', request)[0]
    if req_type != STUN_BINDING_REQUEST:
        raise ValueError(f'Not a binding request: 0x{req_type:04x}')
    transaction = request[4:20]
    port = struct.pack('>H', source_port)
    address = socket.inet_aton(source_ip)
    attribute = struct.pack('>HH', STUN_MAPPED_ADDRESS, 8) + b'\x00\x01' + port + address
    body = attribute + b'\x00' * ((-len(attribute)) % 4)
    return struct.pack('>HH', STUN_BINDING_RESPONSE, len(body)) + transaction + body


QA_MAGIC = b'\x75\x72'
QA_TYPE_OFFSET = 6
QA_CONST_OFFSET = 8
QA_COUNTER_OFFSET = 12


def _qa_string(text):
    raw = text.encode('ascii') + b'\x00'
    return struct.pack('>I', len(raw)) + raw


def build_qa_probe_reply(request, address='127.0.0.1', port=8000, netmask='255.255.255.0'):
    """Answer a QA bdSDK probe ('ur' magic, type 3) with a type-4 server record.

    Layout pinned byte-for-byte against the original server's live reply
    (tests/test_stun.py LIVE_REPLY), all big endian:
      header: magic 'ur', u16 0x0010, u16 body length, u16 type 4,
              u32 11, u32 probe counter echoed from request[12:16]
      body:   u32 0, u32 0x18, u32 1, str address, u32 port, str netmask, u32 0
              (str = u32 length incl. NUL + bytes + NUL)
    The client (sub_B169A0) accepts when the echoed counter matches and the
    type is 4, then hands the record to the config walker sub_B166D0, so the
    advertised address is where later probes go: loopback keeps them local.
    """
    if len(request) < 16 or request[:2] != QA_MAGIC:
        raise ValueError('Not a QA probe')
    body = (struct.pack('>III', 0, 0x18, 1) + _qa_string(address)
            + struct.pack('>I', port) + _qa_string(netmask) + struct.pack('>I', 0))
    header = QA_MAGIC + struct.pack('>HHHI', 0x0010, len(body), 4, 11)
    return header + request[QA_COUNTER_OFFSET:QA_COUNTER_OFFSET + 4] + body


DW_IP_DISCOVERY = 30
DW_NAT_DISCOVERY = 20
DW_KEEPALIVE = 14
STUN_COOKIE = 0x2112A442


def _dw_discovery(data, address, port):
    """-> (reply, kind) for a bdSDK UDP discovery datagram on the lobby port.

    Measured QA requests: `1e 03 00` (IP discovery) and `14 02 00 00` (NAT
    discovery), sent every 0.5 s while the DW live manager connects; unanswered,
    it gives up after ~6 s, three times, then raises @SF_UI_DW_LIVE_DISCONNECTED.
    Reply shapes are the ones the 1.1.13 reference server (dw-capture.py
    stun_reply) documents as accepted by 3.3.3.3, all little endian:
      30 -> 31: u8 type, u16 proto, ip, u16 port, u32 ip^cookie, u16 port^cookie16
      20 -> 21: u8 type, u16 proto, ip, u16 port, changed ip, u16 changed port
      14 -> 15 (29-byte keepalive): echo with the address sentinel at 18/24 filled
    Limitation: NAT change requests (trailing flag 02/03) are answered from this
    socket, not an alternate address, so the client may classify NAT as moderate.
    """
    if len(data) < 3:
        raise ValueError('Short discovery datagram')
    kind = data[0]
    ip = socket.inet_aton(address)
    port_le = struct.pack('<H', port)
    if kind == DW_IP_DISCOVERY:
        ip_u32 = int.from_bytes(ip, 'little')
        return (bytes([31]) + data[1:3] + ip + port_le
                + struct.pack('<IH', ip_u32 ^ STUN_COOKIE, port ^ (STUN_COOKIE >> 16))), 'ip_discovery'
    if kind == DW_NAT_DISCOVERY:
        return bytes([21]) + data[1:3] + ip + port_le + ip + port_le, 'nat_discovery'
    if kind == DW_KEEPALIVE and len(data) == 29:
        reply = bytearray(data)
        reply[0] = 15
        reply[18:22] = ip
        reply[22:24] = port_le
        reply[24:28] = ip
        return bytes(reply), 'keepalive'
    raise ValueError(f'Not a discovery datagram: type {kind}, {len(data)} bytes')


def build_dw_discovery_reply(data, address='127.0.0.1', port=3074):
    return _dw_discovery(data, address, port)[0]


class StunServer(threading.Thread):
    """UDP loopback responder for every address-discovery format the QA client uses.

    One class serves both endpoints: the STUN host (:8000, 'ur' probes and RFC 3489)
    and the lobby port (:3074, bdSDK discovery). `name` prefixes the log events.
    """

    def __init__(self, bind='127.0.0.1', port=8000, log=None, name='stun'):
        super().__init__(name=f'{name}-server', daemon=True)
        self.event_prefix = name
        self.log = log or (lambda *a, **k: None)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind((bind, port))
        self.bind_address = self._socket.getsockname()
        self._stop = threading.Event()

    def _reply(self, data, source):
        try:
            return build_binding_response(data, source[0], source[1]), 'rfc3489'
        except ValueError:
            pass
        try:
            return build_qa_probe_reply(data, address=self.bind_address[0],
                                        port=self.bind_address[1]), 'qa_probe'
        except ValueError:
            pass
        return _dw_discovery(data, self.bind_address[0], self.bind_address[1])

    def run(self):
        prefix = self.event_prefix
        self.log(f'{prefix}_listening', endpoint=f'{self.bind_address[0]}:{self.bind_address[1]}')
        while not self._stop.is_set():
            try:
                data, source = self._socket.recvfrom(2048)
            except ConnectionResetError as exc:
                # Windows reports an ICMP port-unreachable for an earlier reply here.
                self.log(f'{prefix}_peer_reset', reason=str(exc))
                continue
            except OSError:
                break
            peer = f'{source[0]}:{source[1]}'
            self.log(f'{prefix}_request', source=peer,
                     request_bytes=len(data), request_hex=data.hex())
            try:
                response, kind = self._reply(data, source)
            except ValueError as exc:
                self.log(f'{prefix}_reject', source=peer, reason=str(exc),
                         request_bytes=len(data), request_hex=data.hex())
                continue
            try:
                self._socket.sendto(response, source)
            except OSError as exc:
                self.log(f'{prefix}_send_error', source=peer, reply_kind=kind,
                         reason=str(exc), response_hex=response.hex())
                continue
            self.log(f'{prefix}_response', source=peer, reply_kind=kind,
                     request_hex=data.hex(), response_bytes=len(response),
                     response_hex=response.hex())

    def stop(self):
        self._stop.set()
        try:
            self._socket.close()
        except OSError:
            pass


def make_stun_server(bind='127.0.0.1', port=8000, log=None, name='stun'):
    server = StunServer(bind=bind, port=port, log=log, name=name)
    server.start()
    return server