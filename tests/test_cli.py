"""Exercise the headless commands as an agent would: no display, only files and JSON."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

APP = Path(__file__).resolve().parents[1] / 'build/hype'
DECK = '# One\n\n---\n\n# Two\n\n- a\n- b\n\n---\n\n```ruby\na = 1\n---\nb = 2\n```\n'


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # No display, on a desktop that asks Qt for Wayland: commands must pick offscreen themselves.
        self.env = {key: value for key, value in os.environ.items() if key not in ('DISPLAY', 'WAYLAND_DISPLAY')}
        self.env.update(QT_QPA_PLATFORM='wayland;xcb', XDG_CONFIG_HOME=str(self.root / 'config'),
                        OMARCHY_PATH=str(self.root / 'omarchy'), HOME=str(self.root / 'home'))
        theme = self.root / 'omarchy/themes/paper'
        theme.mkdir(parents=True)
        (theme / 'colors.toml').write_text('background = "#ffffff"\nforeground = "#111111"\n')
        self.deck = self.root / 'talk/presentation.md'

    def hype(self, *arguments, code=0):
        result = subprocess.run([str(APP), *map(str, arguments)], env=self.env, cwd=self.root,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def write(self, markdown):
        self.deck.parent.mkdir(exist_ok=True)
        self.deck.write_text(markdown)

    def png_size(self, path):
        data = Path(path).read_bytes()
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        return struct.unpack('>II', data[16:24])

    def test_new_starts_a_themed_presentation_without_overwriting(self):
        created = json.loads(self.hype('new', self.deck, '--title', 'A "big" idea', '--theme', 'paper', '--json').stdout)
        self.assertEqual(created['presentation'], str(self.deck))
        text = self.deck.read_text()
        self.assertIn('title: "A \\"big\\" idea"', text)
        self.assertIn('color_background: "#ffffff"', text)
        self.assertTrue(text.endswith('# A "big" idea\n'))
        self.assertTrue((self.deck.parent / 'images').is_dir() and (self.deck.parent / 'videos').is_dir())
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])
        self.assertIn('already exists', self.hype('new', self.deck, code=1).stderr)
        self.assertIn('paper', self.hype('new', self.root / 'other.md', '--theme', 'nope', code=1).stderr)

    def test_stock_themes_work_without_omarchy_and_installed_palettes_override(self):
        stock = {'catppuccin', 'catppuccin-latte', 'ethereal', 'everforest', 'flexoki-light',
                 'gruvbox', 'hackerman', 'kanagawa', 'last-horizon', 'lumon', 'lupine',
                 'matte-black', 'miasma', 'nord', 'osaka-jade', 'retro-82', 'ristretto',
                 'rose-pine', 'solitude', 'tokyo-night', 'vantablack', 'white'}
        installed = self.env['OMARCHY_PATH']
        self.env['OMARCHY_PATH'] = str(self.root / 'missing-omarchy')
        listed = json.loads(self.hype('themes', '--json').stdout)['themes']
        self.assertTrue(stock <= set(listed))
        nord = self.root / 'nord.md'
        self.hype('new', nord, '--theme', 'nord')
        self.assertIn('color_background: "#2e3440"', nord.read_text())

        self.env['OMARCHY_PATH'] = installed
        self.assertIn('paper', json.loads(self.hype('themes', '--json').stdout)['themes'])
        override = self.root / 'omarchy/themes/tokyo-night/colors.toml'
        override.parent.mkdir(parents=True)
        override.write_text('background = "#010203"\n')
        local = self.root / 'local.md'
        self.hype('new', local, '--theme', 'tokyo-night')
        self.assertIn('color_background: "#010203"', local.read_text())

    def test_check_reports_every_problem_with_slide_and_line(self):
        self.write(DECK + '\n---\n\n![](missing.png)\n\n---\n\n![bogus=1](gone.png)\n\n---\n')
        report = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)
        self.assertFalse(report['ok'])
        self.assertEqual(report['slides'], 6)
        found = [(p['slide'], p['line'], p['severity'], p['message']) for p in report['problems']]
        self.assertEqual(found, [(4, 20, 'error', 'Missing media: missing.png'),
                                 (5, 24, 'error', 'Unknown media directive: bogus'),
                                 (5, 24, 'error', 'Missing media: gone.png'),
                                 (6, 26, 'warning', 'Empty slide')])
        text = self.hype('check', self.deck, code=1)
        self.assertIn('talk/presentation.md:20: slide 4 error: Missing media: missing.png', text.stderr)
        self.assertIn('6 slides, 3 errors, 1 warnings', text.stdout)

    def test_check_locates_an_unclosed_fence_and_warns_about_cramped_text(self):
        self.write('# One\n\n---\n\n```ruby\na = 1\n')
        problems = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)['problems']
        self.assertEqual(problems, [{'slide': None, 'line': 5, 'severity': 'error', 'message': 'Unclosed code fence'}])
        self.write('# One\n\n---\n\n' + '\n'.join(f'- point number {i}' for i in range(60)) + '\n')
        report = json.loads(self.hype('check', self.deck, '--json').stdout)
        self.assertTrue(report['ok'])
        self.assertEqual([(p['slide'], p['severity']) for p in report['problems']], [(2, 'warning')])

    def test_slides_outlines_the_presentation(self):
        self.write('---\ntitle: Talk\n---\n\n' + DECK + '\n---\n\n![fit](photo.png)\n\nJust words\n')
        outline = json.loads(self.hype('slides', self.deck, '--json').stdout)
        self.assertEqual(outline['title'], 'Talk')
        self.assertEqual([(s['slide'], s['line'], s['endLine'], s['title'], s['media']) for s in outline['slides']],
                         [(1, 5, 5, 'One', None), (2, 9, 12, 'Two', None), (3, 16, 20, 'a = 1', None),
                          (4, 24, 26, 'Just words', 'photo.png')])
        self.assertIn('Two', self.hype('slides', self.deck).stdout)
        self.write('````markdown\n```sh\n# not a headline\n```\n````\n\n# Real headline\n')
        self.assertEqual(json.loads(self.hype('slides', self.deck, '--json').stdout)['slides'][0]['title'],
                         'Real headline')

    def test_render_one_slide_or_all(self):
        self.write(DECK)
        result = json.loads(self.hype('render', self.deck, '--slide', 2, '-o', 'out/two.png', '--width', 640,
                                      '--json').stdout)
        self.assertEqual(result['image'], str(self.root / 'out/two.png'))
        self.assertEqual(self.png_size(result['image']), (640, 360))
        self.hype('render', self.deck, '--slide', 3)
        self.assertEqual(self.png_size(self.root / 'slide-003.png'), (1920, 1080))
        self.assertIn('there are 3 slides', self.hype('render', self.deck, '--slide', 4, code=1).stderr)
        self.hype('render', self.deck, '-o', 'all')
        manifest = json.loads((self.root / 'all/slides.json').read_text())
        self.assertEqual([s['image'] for s in manifest['slides']], ['slide-001.png', 'slide-002.png', 'slide-003.png'])

    def test_render_shows_a_broken_slide_but_fails(self):
        self.write('# One\n\n---\n\n![](missing.png)\n')
        result = self.hype('render', self.deck, '--slide', 2, '-o', 'broken.png', code=1)
        self.assertIn('slide 2 error: Missing media: missing.png', result.stderr)
        self.assertEqual(self.png_size(self.root / 'broken.png'), (1920, 1080))

    def test_export_chooses_the_format_from_the_extension(self):
        self.write(DECK)
        self.hype('export', self.deck, 'out/talk.pdf')
        self.assertEqual((self.root / 'out/talk.pdf').read_bytes()[:5], b'%PDF-')
        exported = json.loads(self.hype('export', self.deck, 'talk.pptx', '--json').stdout)
        self.assertEqual((exported['format'], exported['slides']), ('pptx', 3))
        self.assertEqual((self.root / 'talk.pptx').read_bytes()[:2], b'PK')
        self.assertIn('.pdf or .pptx', self.hype('export', self.deck, 'talk.key', code=1).stderr)

    def test_flags_export_without_a_display_and_need_a_presentation(self):
        self.write(DECK)
        self.hype(self.deck, '--pdf', 'flag.pdf')
        self.assertEqual((self.root / 'flag.pdf').read_bytes()[:5], b'%PDF-')
        self.assertIn('Name a Markdown presentation', self.hype('--pdf', 'none.pdf', code=1).stderr)
        self.assertFalse((self.root / 'config/hype/hype.ini').exists())

    def test_themes_and_help(self):
        self.assertEqual(json.loads(self.hype('themes', '--json').stdout)['themes'], ['paper'])
        self.assertIn('hype check', self.hype('help', 'format').stdout)
        self.assertIn('--slide', self.hype('help', 'render').stdout)
        self.assertIn('help format', self.hype('help').stdout)
        self.assertEqual(self.hype().stdout, self.hype('help').stdout)
        self.assertIn('open [presentation]', self.hype().stdout)
        self.assertIn('help format', self.hype('--help').stdout)

    def test_skill_prints_and_installs_for_agents(self):
        self.assertTrue(self.hype('skill').stdout.startswith('---\nname: hype\n'))
        home = self.root / 'home'
        (home / '.claude').mkdir(parents=True)
        for _ in range(2):  # Installing again refreshes the copy and the link.
            self.assertIn('Linked', self.hype('skill', 'install').stdout)
        self.assertEqual((home / '.agents/skills/hype/SKILL.md').read_text(), self.hype('skill').stdout)
        self.assertEqual(os.readlink(home / '.claude/skills/hype'), '../../.agents/skills/hype')
        self.assertTrue((home / '.claude/skills/hype/SKILL.md').is_file())


if __name__ == '__main__':
    unittest.main()
