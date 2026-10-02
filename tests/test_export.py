"""Exercise native PowerPoint export using only Python's standard test tools."""
import os
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
import zlib

APP = Path(__file__).resolve().parents[1] / 'build/hype'
NS = {'p': 'http://schemas.openxmlformats.org/presentationml/2006/main',
      'a': 'http://schemas.openxmlformats.org/drawingml/2006/main',
      'r': 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'}


def image(path):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress((b'\0' + b'\x70\xa0\xf0' * 2) * 2)) +
                     chunk(b'IEND', b''))


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'images').mkdir()
        (self.root / 'videos').mkdir()
        image(self.root / 'images/photo.png')
        tools = self.root / 'tools'
        tools.mkdir()
        self.app = tools / 'hype'
        shutil.copy2(APP, self.app)
        for tool in ['ffmpeg', 'ffprobe', 'source-highlight']:
            (tools / tool).symlink_to(shutil.which(tool))
        # The exported application cannot find Python, pip, or an external ZIP tool.
        self.env = dict(os.environ, PATH=str(tools), XDG_CONFIG_HOME=str(self.root / 'config'),
                        QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='generic', QT_STYLE_OVERRIDE='Fusion')
        self.output = self.root / 'talk.pptx'

    def movie(self, size='320x180', codec='libx264'):
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        f'color=c=blue:s={size}:d=0.2', '-c:v', codec, '-pix_fmt', 'yuv420p',
                        str(self.root / 'videos/demo.mp4')], check=True)

    def export(self, markdown, success=True):
        source = self.root / 'presentation.md'
        source.write_text(markdown)
        result = subprocess.run([str(self.app), str(source), '--pptx', str(self.output)],
                                env=self.env, capture_output=True, text=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def test_still_slides_and_xml_escaping(self):
        self.export('---\ntitle: "A & B <C>"\n---\n\n# Hello\n\n---\n\n![](photo.png)\n')
        with zipfile.ZipFile(self.output) as archive:
            self.assertIsNone(archive.testzip())
            payload = self.output.read_bytes()
            for entry in archive.infolist():
                if entry.compress_type == zipfile.ZIP_STORED:
                    # LibreOffice rejects stored entries whose sizes exist only
                    # in data descriptors, even though Python can read them.
                    self.assertFalse(entry.flag_bits & 0x8)
                    sizes = struct.unpack_from('<III', payload, entry.header_offset + 14)
                    self.assertEqual(sizes, (entry.CRC, entry.compress_size, entry.file_size))
            presentation = ET.fromstring(archive.read('ppt/presentation.xml'))
            self.assertEqual(len(presentation.findall('p:sldIdLst/p:sldId', NS)), 2)
            self.assertEqual(presentation.get('autoCompressPictures'), '0')
            images = [name for name in archive.namelist() if name.endswith('.png')]
            self.assertEqual(len(images), 2)
            for name in images:
                self.assertEqual(struct.unpack('>II', archive.read(name)[16:24]), (3840, 2160))
            size = presentation.find('p:sldSz', NS)
            self.assertAlmostEqual(int(size.get('cx')) / int(size.get('cy')), 16 / 9, places=5)
            core = ET.fromstring(archive.read('docProps/core.xml'))
            self.assertEqual(core.find('{http://purl.org/dc/elements/1.1/}title').text, 'A & B <C>')
            for name in archive.namelist():
                if name.endswith(('.xml', '.rels')):
                    ET.fromstring(archive.read(name))

    def test_repeated_movie_is_embedded_once(self):
        self.movie(codec='mpeg4')
        self.export('![fit](demo.mp4)\n---\n![span muted](demo.mp4)\n')
        with zipfile.ZipFile(self.output) as archive:
            movies = [name for name in archive.namelist() if name.endswith('.mp4')]
            self.assertEqual(len(movies), 1)
            targets = []
            for number in (1, 2):
                rels = ET.fromstring(archive.read(f'ppt/slides/_rels/slide{number}.xml.rels'))
                targets.append(next(rel.get('Target') for rel in rels if rel.get('Type').endswith('/video')))
            self.assertEqual(targets[0], targets[1])

    def test_movie_embedded_with_playback_flags(self):
        self.movie()
        self.export('![loop muted](demo.mp4)\n')
        with zipfile.ZipFile(self.output) as archive:
            movies = [name for name in archive.namelist() if name.endswith('.mp4')]
            self.assertEqual(len(movies), 1)
            self.assertEqual(archive.read(movies[0]), (self.root / 'videos/demo.mp4').read_bytes())
            xml = ET.fromstring(archive.read('ppt/slides/slide1.xml'))
            node = xml.find('.//p:video/p:cMediaNode', NS)
            self.assertIsNotNone(node)
            self.assertEqual(node.get('vol'), '0')
            timing = node.find('p:cTn', NS)
            self.assertEqual(timing.get('repeatCount'), 'indefinite')
            self.assertEqual(timing.find('p:stCondLst/p:cond', NS).get('delay'), '0')
            self.assertNotIn(b'TargetMode="External"', archive.read('ppt/slides/_rels/slide1.xml.rels'))
        original = self.output.read_bytes()
        self.export('![](missing.mp4)', success=False)
        self.assertEqual(self.output.read_bytes(), original)

    def test_manual_playback_and_overlay_order(self):
        self.movie()
        self.export('![span autoplay=false](demo.mp4)\n\n# Headline\n')
        with zipfile.ZipFile(self.output) as archive:
            xml = ET.fromstring(archive.read('ppt/slides/slide1.xml'))
            node = xml.find('.//p:video/p:cMediaNode', NS)
            self.assertEqual(node.get('vol'), '100000')
            timing = node.find('p:cTn', NS)
            self.assertIsNone(timing.get('repeatCount'))
            self.assertEqual(timing.find('p:stCondLst/p:cond', NS).get('delay'), 'indefinite')
            pictures = xml.findall('p:cSld/p:spTree/p:pic', NS)
            self.assertEqual(len(pictures), 3)
            self.assertIsNotNone(pictures[1].find('.//a:videoFile', NS))
            self.assertIsNone(pictures[2].find('.//a:videoFile', NS))

    def test_invalid_video_preserves_existing_export(self):
        self.export('# Original\n')
        original = self.output.read_bytes()
        self.movie(size='240x320')
        result = self.export('![span](demo.mp4)\n', success=False)
        self.assertIn('16:9', result.stderr)
        self.assertEqual(self.output.read_bytes(), original)
        self.export('![fit](demo.mp4)\n')
        original = self.output.read_bytes()
        self.movie(codec='mpeg4')
        source = (self.root / 'videos/demo.mp4').read_bytes()
        self.export('![](demo.mp4)\n')
        self.extracted_movie(size=(320, 180))
        self.assertEqual((self.root / 'videos/demo.mp4').read_bytes(), source)

    def test_webm_converts_video_and_audio_without_changing_original(self):
        source = self.root / 'videos/demo.webm'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=blue:s=320x180:d=0.2',
                        '-f', 'lavfi', '-i', 'sine=frequency=440:duration=0.2',
                        '-c:v', 'libvpx-vp9', '-c:a', 'libopus', str(source)], check=True)
        original = source.read_bytes()
        self.export('![span](demo.webm)\n')
        movie, _, _ = self.extracted_movie(size=(320, 180), audio=True)
        info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams',
                                                  '-of', 'json', str(movie)]))
        self.assertEqual([s['codec_name'] for s in info['streams']], ['h264', 'aac'])
        self.assertEqual(source.read_bytes(), original)

    def animation(self, extension):
        source = Path(__file__).parent / 'fixtures' / f'animated.{extension}'
        target = self.root / 'images' / f'demo.{extension}'
        shutil.copy2(source, target)
        return target

    def extracted_movie(self, size=(3840, 2160), audio=False):
        with zipfile.ZipFile(self.output) as archive:
            movies = [name for name in archive.namelist() if name.endswith('.mp4')]
            self.assertEqual(len(movies), 1)
            target = self.root / 'converted.mp4'
            target.write_bytes(archive.read(movies[0]))
            xml = ET.fromstring(archive.read('ppt/slides/slide1.xml'))
        probe = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                                '-of', 'json', str(target)], capture_output=True, check=True)
        info = json.loads(probe.stdout)
        self.assertEqual(info['streams'][0]['codec_name'], 'h264')
        self.assertEqual(info['streams'][0]['pix_fmt'], 'yuv420p')
        self.assertEqual((info['streams'][0]['width'], info['streams'][0]['height']), size)
        self.assertEqual(len(info['streams']), 2 if audio else 1)
        return target, xml, info

    def movie_pixel(self, movie, time, x=96, y=54):
        result = subprocess.run(['ffmpeg', '-v', 'error', '-ss', str(time), '-i', str(movie),
                                 '-frames:v', '1', '-vf', 'scale=192:108', '-f', 'rawvideo',
                                 '-pix_fmt', 'rgb24', 'pipe:1'], capture_output=True, check=True)
        offset = (y * 192 + x) * 3
        self.assertEqual(len(result.stdout), 192 * 108 * 3)
        return tuple(result.stdout[offset:offset + 3])

    def assert_color(self, actual, expected):
        for channel, value in zip(actual, expected):
            self.assertAlmostEqual(channel, value, delta=12)

    def test_webp_conversion_preserves_motion_alpha_and_timing(self):
        source = self.animation('webp')
        original = source.read_bytes()
        self.export('![fit background=#123456](demo.webp)\n')
        movie, xml, info = self.extracted_movie()
        self.assertAlmostEqual(float(info['format']['duration']), 0.6, delta=0.04)
        self.assert_color(self.movie_pixel(movie, 0.05), (255, 0, 0))
        self.assert_color(self.movie_pixel(movie, 0.25), (0, 0, 255))
        self.assert_color(self.movie_pixel(movie, 0.45), (18, 52, 86))
        timing = xml.find('.//p:video/p:cMediaNode', NS)
        self.assertEqual(timing.get('vol'), '0')
        self.assertEqual(timing.find('p:cTn', NS).get('repeatCount'), 'indefinite')
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(list((self.root / 'videos').iterdir()), [])
        self.assertEqual(set((self.root / 'images').iterdir()), {self.root / 'images/photo.png', source})

    def test_gif_conversion_preserves_fit_layout_finite_loop_and_manual_playback(self):
        self.animation('gif')
        self.export('![fit background=#123456 autoplay=false](demo.gif)\n\n# Caption\n')
        movie, xml, info = self.extracted_movie()
        self.assertAlmostEqual(float(info['format']['duration']), 0.68, delta=0.04)
        self.assert_color(self.movie_pixel(movie, 0.05, 50, 54), (191, 0, 0))
        self.assert_color(self.movie_pixel(movie, 0.25, 50, 54), (0, 0, 191))
        self.assert_color(self.movie_pixel(movie, 0.55, 50, 54), (0, 96, 0))
        self.assert_color(self.movie_pixel(movie, 0.25, 180, 2), (13, 39, 64))
        timing = xml.find('.//p:video/p:cMediaNode/p:cTn', NS)
        self.assertEqual(timing.get('repeatCount'), '3000')
        self.assertEqual(timing.find('p:stCondLst/p:cond', NS).get('delay'), 'indefinite')
        # Arbitrary image aspect ratios can span a slide, unlike existing MP4s.
        self.export('![span loop](demo.gif)\n\n# Overlaid headline\n')
        movie, xml, _ = self.extracted_movie()
        self.assert_color(self.movie_pixel(movie, 0.25, 10, 10), (0, 0, 191))
        self.assertEqual(xml.find('.//p:video/p:cMediaNode/p:cTn', NS).get('repeatCount'), 'indefinite')

    def test_placed_animation_keeps_its_corner(self):
        self.animation('gif')
        self.export('# Headline\n\n![position=bottom-right size=20%](demo.gif)\n')
        movie, _, _ = self.extracted_movie()
        # On a 192x108 frame the 20% box ends 3px from the edges; the 35x27 GIF is 28x22.
        self.assert_color(self.movie_pixel(movie, 0.05, 180, 100), (255, 0, 0))  # Undarkened.
        self.assert_color(self.movie_pixel(movie, 0.25, 180, 100), (0, 0, 255))
        corner = self.movie_pixel(movie, 0.05, 150, 100)
        self.assertLess(corner[0], 100, corner)  # Left of the image is the theme background.
        self.assert_color(self.movie_pixel(movie, 0.05, 5, 5), corner)  # No darkening overlay.

    def test_animation_conversion_failure_preserves_export(self):
        self.export('# Original\n')
        original = self.output.read_bytes()
        self.animation('webp')
        (self.root / 'tools/ffmpeg').unlink()
        result = self.export('![](demo.webp)\n', success=False)
        self.assertIn('Slide 1', result.stderr)
        self.assertIn('ffmpeg', result.stderr)
        self.assertEqual(self.output.read_bytes(), original)
        self.assertEqual(list((self.root / 'videos').iterdir()), [])

    @unittest.skipUnless(os.environ.get('HYPE_OFFICE_TESTS') and shutil.which('libreoffice'),
                         'Set HYPE_OFFICE_TESTS=1 to verify with LibreOffice')
    def test_libreoffice_opens_export(self):
        self.movie()
        self.animation('webp')
        self.export('# Hello\n\n---\n\n![loop muted](demo.mp4)\n\n---\n\n'
                    '![span](demo.mp4)\n\n# Overlaid headline\n\n---\n\n![](demo.webp)\n')
        output = self.root / 'pdf'
        output.mkdir()
        profile = (self.root / 'office-profile').as_uri()
        result = subprocess.run(['libreoffice', f'-env:UserInstallation={profile}', '--headless',
                                 '--convert-to', 'pdf', '--outdir', str(output), str(self.output)],
                                env=dict(os.environ, SAL_USE_VCLPLUGIN='svp'),
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        pdf = output / 'talk.pdf'
        self.assertTrue(pdf.exists(), result.stdout + result.stderr)
        self.assertTrue(pdf.read_bytes().startswith(b'%PDF-'))


if __name__ == '__main__':
    unittest.main()
