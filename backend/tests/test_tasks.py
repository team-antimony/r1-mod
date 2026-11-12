import struct
import unittest
from local_auth.frames import build_task_reply, TASK_REPLY, parse_task


class TaskReplyTests(unittest.TestCase):
    def test_empty_success_reply_is_type_tagged(self):
        inner_type, body = build_task_reply(4, transaction_id=1)
        self.assertEqual(inner_type, TASK_REPLY)
        self.assertEqual(body[0], 0x0A)                       # uint64 type tag
        self.assertEqual(struct.unpack_from('<Q', body, 1)[0], 1)
        self.assertEqual(body[9], 0x08)                       # uint32 type tag
        self.assertEqual(struct.unpack_from('<I', body, 10)[0], 0)
        self.assertEqual(body[14], 0x03)                      # uint8 type tag
        self.assertEqual(body[15], 4)                         # echoed task id
        self.assertEqual(body[16], 0x08)
        self.assertEqual(struct.unpack_from('<I', body, 17)[0], 0)

    def test_error_reply_omits_result_count(self):
        inner_type, body = build_task_reply(4, transaction_id=7, error=5)
        self.assertEqual(inner_type, TASK_REPLY)
        self.assertEqual(body[0], 0x0A)
        self.assertEqual(struct.unpack_from('<Q', body, 1)[0], 7)
        self.assertEqual(body[9], 0x08)
        self.assertEqual(struct.unpack_from('<I', body, 10)[0], 5)
        self.assertEqual(len(body), 14)

    def test_parse_task_reads_after_uint8_type_tag(self):
        service, task, _ = parse_task(bytes([71, 3, 4, 19, 8]))
        self.assertEqual(service, 71)
        self.assertEqual(task, 4)


if __name__ == '__main__':
    unittest.main()