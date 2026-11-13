"""Run with Python + Pillow; set FFMPEG to also exercise video transcoding."""
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from PIL import Image, JpegImagePlugin

import generate_watch_resources as resources


def jpeg(color, size=(360, 360)):
    stream = io.BytesIO()
    Image.new('RGB', size, color).save(stream, 'JPEG', quality=95, subsampling=2)
    return stream.getvalue()


def small_pack(clips):
    glyph = resources.GLYPH.pack(65, 0, 16, 1, 1, 0, 0)
    fonts = [((24, 4, 1, 14, 2, 1, 0, 0, 1), glyph, b'\xf0')]
    icons = [((b'test'.ljust(32, b'\0'), 1, 1, 0, 1), b'\xff')]
    return resources.make_pack(fonts, icons, clips, 10)


class WatchResourceTests(unittest.TestCase):
    def test_native_mjpeg_keeps_original_bytes_brightness_and_order(self):
        frames = [jpeg((224, 180, 140)), jpeg((70, 180, 235))]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.mjpeg'
            source.write_bytes(b''.join(frames))
            media = resources.probe_video(None, source)
            result = resources.video_frames(None, source, Path(directory) / 'result.mjpg', 10, media)
            self.assertEqual(result, frames)
            self.assertEqual(media['source_duration_seconds'], 0.2)
            with Image.open(io.BytesIO(result[0])) as image:
                self.assertEqual(image.getpixel((0, 0)), image.getpixel((180, 180)))
                self.assertEqual(image.getpixel((359, 359)), image.getpixel((180, 180)))

    def test_native_mjpeg_rejects_silent_geometry_or_timing_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.mjpeg'
            output = Path(directory) / 'result.mjpg'
            source.write_bytes(jpeg('white', (180, 180)))
            with self.assertRaisesRegex(ValueError, '360x360'):
                resources.video_frames(None, source, output)
            source.write_bytes(jpeg('white'))
            with self.assertRaisesRegex(ValueError, 'frame rate'):
                resources.video_frames(None, source, output, 20)

    def test_mjpeg_rejects_empty_truncated_and_progressive_frames(self):
        progressive = io.BytesIO()
        Image.new('RGB', (360, 360), 'white').save(progressive, 'JPEG', progressive=True)
        for stream in (b'', jpeg('white')[:-1], b'bad', progressive.getvalue()):
            with self.subTest(size=len(stream)), self.assertRaises(ValueError):
                resources.split_mjpeg(stream)

    def test_v4_ui_can_be_repacked_as_v5_without_font_or_icon_changes(self):
        original_frames = [[jpeg('red')], [jpeg('blue')]]
        old = bytearray(small_pack(original_frames))
        struct.pack_into('<I', old, 8, 4)
        _, fonts, icons = resources.read_ui_assets(old)
        new_frames = [[jpeg('white'), jpeg('yellow')], [jpeg('green')]]
        new = resources.make_pack(fonts, icons, new_frames, 10)
        header, _, _ = resources.read_ui_assets(new)
        self.assertEqual(header[1], 5)
        self.assertEqual(resources.verify_ui_unchanged(old, new)['icons_unchanged'], 1)
        result = resources.validate_videos(new)
        self.assertEqual((result['standby_frames'], result['speaking_frames']), (2, 1))
        self.assertEqual(result['jpeg_subsampling'], ['4:2:0'])
        self.assertEqual(resources.validate_videos(old)['decoded_frames'], 2)

    def test_pack_rejects_corrupted_clip_counts(self):
        data = bytearray(small_pack([[jpeg('white')], [jpeg('black')]]))
        resources.CLIPS.pack_into(data, resources.HEADER.size, 1, 2)
        with self.assertRaisesRegex(ValueError, 'clip counts'):
            resources.read_ui_assets(data)

    def test_pack_rejects_out_of_bounds_frame(self):
        data = bytearray(small_pack([[jpeg('white')], [jpeg('black')]]))
        header = resources.HEADER.unpack_from(data)
        resources.FRAME.pack_into(data, header[9], len(data), 100)
        with self.assertRaisesRegex(ValueError, 'frame range'):
            resources.validate_videos(data)

    @unittest.skipUnless(os.environ.get('FFMPEG'), 'Set FFMPEG for real encoder coverage')
    def test_transcoding_preserves_brightness_dimensions_and_compatible_sampling(self):
        with tempfile.TemporaryDirectory() as directory:
            still = Path(directory) / 'bright.png'
            source = Path(directory) / 'bright.mkv'
            # A uniform bright frame catches permanent radial/bottom shading.
            image = Image.new('RGB', (360, 360), (235, 195, 145))
            image.save(still)
            subprocess.run([os.environ['FFMPEG'], '-hide_banner', '-loglevel', 'error',
                            '-loop', '1', '-framerate', '10', '-i', str(still),
                            '-t', '0.2', '-c:v', 'ffv1', str(source)], check=True)
            frames = resources.video_frames(Path(os.environ['FFMPEG']), source,
                                            Path(directory) / 'result.mjpg', 10,
                                            {'width': 360, 'height': 360})
            self.assertEqual(len(frames), 2)
            with Image.open(io.BytesIO(frames[0])) as result:
                self.assertEqual(result.size, (360, 360))
                self.assertEqual(JpegImagePlugin.get_sampling(result), 2)
                for point in ((0, 0), (359, 359), (180, 180)):
                    for actual, expected in zip(result.getpixel(point), image.getpixel(point)):
                        self.assertLessEqual(abs(actual - expected), 3)


if __name__ == '__main__':
    unittest.main()
