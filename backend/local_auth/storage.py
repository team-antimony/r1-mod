"""Demonware storage service (10) for the QA 3.3.3.3 client: publisher files.

Task 21 = getPublisherFile (request builder sub_A5B7F0: tagged string file name,
max 128). The result is a single blob that sub_A4E8B0 copies into a per-file
client buffer (128 KB for playlists, 64 KB or 8 KB for the others).

The caller (sub_58EB50) only counts a file as loaded on success WITH a result.
Files the client marks required (byte_CDD260[idx]; in practice
mp_playlists_shipRetail_release.info and configs_s.info) are retried three
times and then raise EXE_DOWNLOAD_PUBLISHER_FILE_FAILURE. The QA research
notes say an empty file is accepted, so a missing file is served as an empty
blob. Real contents can be dropped into `publisher/<name>`.
"""
from pathlib import Path

from .frames import TAG_STRING, parse_args, w_blob

STORAGE_SERVICE = 10
PUBLISHER_FILE_TASK = 21


def publisher_file_reply(args, root):
    """-> ((blob result,), log note) for a 10/21 request; never reads outside `root`."""
    names = [value for tag, value in parse_args(args) if tag == TAG_STRING and value]
    name = names[-1].decode('latin-1') if names else ''
    note = {'file': name}
    candidate = Path(name)
    if not name or candidate.name != name or name.startswith('.'):
        note.update(source='refused', bytes=0)
        return (w_blob(b''),), note
    path = Path(root) / name
    if path.is_file():
        data = path.read_bytes()
        note.update(source='file', bytes=len(data))
    else:
        data = b''
        note.update(source='empty', bytes=0)
    return (w_blob(data),), note
