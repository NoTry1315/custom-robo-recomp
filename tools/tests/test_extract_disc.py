import struct
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.extract_disc import output_path, walk_fst


def make_fst(names, entries):
    name_table = b''.join(name.encode('shift_jis') + b'\0' for name in names)
    fst = bytearray()
    for info, offset, size in entries:
        fst.extend(struct.pack('>III', info, offset, size))
    return bytes(fst) + name_table


class ExtractDiscTests(unittest.TestCase):
    def test_walk_fst_accepts_nested_files(self):
        fst = make_fst(
            ['folder', 'file.bin'],
            [(0x01000000, 0, 3), (0x01000000, 0, 3),
             (len('folder') + 1, 123, 4)],
        )

        self.assertEqual(list(walk_fst(fst)), [('folder/file.bin', 123, 4)])

    def test_walk_fst_rejects_unsafe_windows_names(self):
        names = [
            r'..\..\outside',
            r'folder\..\outside',
            '/rooted',
            r'C:\outside',
            r'\\server\share',
            'C:relative',
            '..',
        ]
        for name in names:
            with self.subTest(name=name):
                fst = make_fst([name], [(0, 0, 2), (0, 0, 0)])
                with self.assertRaisesRegex(ValueError, 'unsafe FST name'):
                    list(walk_fst(fst))

    def test_output_path_stays_within_resolved_root(self):
        with TemporaryDirectory() as temp:
            root = Path(temp) / 'files'
            self.assertEqual(output_path(root, 'folder/file.bin'),
                             root.resolve() / 'folder' / 'file.bin')
            for path in ('../outside', r'..\outside', r'C:\outside', '/rooted'):
                with self.subTest(path=path):
                    with self.assertRaises(ValueError):
                        output_path(root, path)

    def test_output_path_rejects_symlink_escape(self):
        with TemporaryDirectory() as temp:
            root = Path(temp) / 'files'
            outside = Path(temp) / 'outside'
            root.mkdir()
            outside.mkdir()
            link = root / 'linked'
            try:
                link.symlink_to(outside, target_is_directory=True)
            except OSError as error:
                self.skipTest(f'symlink creation unavailable: {error}')

            with self.assertRaisesRegex(ValueError, 'escapes extraction root'):
                output_path(root, 'linked/payload.bin')


if __name__ == '__main__':
    unittest.main()
