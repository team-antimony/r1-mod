"""QA task-10 tickets, laid out for the QA 3.3.3.3 client's ticket parser.

Uses the client's existing plaintext-ticket path. This is not an implementation
of Tencent's ticket encryption or server-ticket cryptographic format.
"""
import base64
import secrets
import struct
import time


def build_auth_response(request, player, *, now=None):
    if not isinstance(request, dict):
        raise ValueError('Auth request must be an object')
    numbers = {}
    for key in ('auth_task', 'title_id', 'iv_seed'):
        value = request.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, str)):
            raise ValueError('Invalid numeric auth field: ' + key)
        if isinstance(value, str) and (not value.isascii() or not value.isdecimal()):
            raise ValueError('Invalid decimal auth field: ' + key)
        numbers[key] = int(value)
        if not 0 <= numbers[key] <= 0xFFFFFFFF:
            raise ValueError('Auth number out of range: ' + key)
    if numbers['auth_task'] != 10 or numbers['title_id'] != 1002:
        raise ValueError('Only QA title 1002 / task 10 is supported')
    issued = int(time.time()) if now is None else now
    if type(issued) is not int or not 0 <= issued <= 0xFFFFFFFF - 86400:
        raise ValueError('Invalid ticket time')
    ticket = struct.pack('<IBIIIQQ64s24s3sI',
                         0xEFBDADDE, 0, int(request['title_id']),
                         issued, issued + 86400, 0, player.user_id,
                         player.name.encode('utf-8'), player.auth_key,
                         b'\0' * 3, 0)
    assert len(ticket) == 128
    return {'auth_task': '11', 'code': '700',
            'iv_seed': str(int(request['iv_seed'])),
            'client_ticket': base64.b64encode(ticket).decode('ascii'),
            'server_ticket': base64.b64encode(secrets.token_bytes(128)).decode('ascii')}
