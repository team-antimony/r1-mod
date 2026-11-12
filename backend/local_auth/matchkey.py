"""Ask an r1-server for its match key before a join.

The QA client opens its bdDTLS association to the game server with the security key from the lobby's StartGame.
r1-server holds one such key per run (r1_matchKey) and hands it to a player's backend on request:

    request  0xffffffff "getmatchkey" [password]
    reply    0xffffffff "matchkey <32 hex key> <16 hex security id>"
             0xffffffff "matchkey refused"       (wrong join password)
             0xffffffff "matchkey unavailable"   (the server hands out no key)

A server that predates the request does not answer.
"""
import socket

OOB = b'\xff\xff\xff\xff'


class MatchKeyRefused(Exception):
    """The server answered but gave no key; the message says why."""


def build_request(password=''):
    request = OOB + b'getmatchkey'
    if password:
        request += b' "' + password.replace('"', '').encode('utf-8') + b'"'
    return request


def parse_reply(data):
    """-> (key 16 bytes, security id 8 bytes). Raises MatchKeyRefused for a refusal, ValueError for anything else."""
    if not data.startswith(OOB):
        raise ValueError('not a connectionless reply')
    words = data[len(OOB):].decode('ascii', 'replace').split()
    if not words or words[0] != 'matchkey':
        raise ValueError('not a matchkey reply')
    if len(words) == 2 and words[1] in ('refused', 'unavailable'):
        raise MatchKeyRefused('wrong join password' if words[1] == 'refused' else 'the server hands out no match key')
    if len(words) != 3 or len(words[1]) != 32 or len(words[2]) != 16:
        raise ValueError('malformed matchkey reply')
    return bytes.fromhex(words[1]), bytes.fromhex(words[2])


def fetch_match_key(ip, port, password='', *, timeout=1.0, tries=3):
    """-> (key, security id), or None when the server does not answer. Raises MatchKeyRefused."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        request = build_request(password)
        for _ in range(tries):
            sock.sendto(request, (ip, port))
            try:
                while True:
                    data, source = sock.recvfrom(2048)
                    if source[1] != port:
                        continue
                    try:
                        return parse_reply(data)
                    except ValueError:
                        continue
            except socket.timeout:
                continue
    return None
