#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LibraryExportTests(unittest.TestCase):
    def test_public_declarations_are_sorted_and_comments_are_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "api.h"
            header.write_text("""/* sippymodem_fake(void); */
// sippymodem_other_fake(void);
struct sippymodem_line *sippymodem_create(
    const struct sippymodem_config *config);
void *sippymodem_handle(const struct sippymodem_server *server);
enum sippymodem_result sippymodem_answer(struct sippymodem_line *, unsigned long);
void private_helper(void);
""")
            output = subprocess.check_output([
                sys.executable, str(ROOT / "tools/library_exports.py"), str(header), str(header),
            ], text=True, timeout=10)
            self.assertEqual(output, "EXPORTS\n    sippymodem_answer\n    sippymodem_create\n    sippymodem_handle\n")

    def test_empty_or_missing_headers_fail_without_partial_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "empty.h"
            header.write_text("struct sippymodem_server;\n")
            for path in (header, header.with_name("missing.h")):
                result = subprocess.run([
                    sys.executable, str(ROOT / "tools/library_exports.py"), str(path),
                ], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
