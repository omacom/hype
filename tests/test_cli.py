"""Exercise the headless commands as an agent would: no display, only files and JSON."""
import json
import os
import shutil
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
                        XDG_DATA_HOME=str(self.root / 'home/.local/share'),
                        XDG_STATE_HOME=str(self.root / 'home/.local/state'),
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

    def test_check_reports_invalid_placement(self):
        self.write('# One\n\n![position=middle](logo.gif)\n\n---\n\n![position=left size=nan%](logo.gif)\n')
        (self.deck.parent / 'images').mkdir()
        shutil.copy2(Path(__file__).parent / 'fixtures/animated.gif', self.deck.parent / 'images/logo.gif')
        problems = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)['problems']
        self.assertEqual([(p['slide'], p['line'], p['message']) for p in problems],
                         [(1, 1, 'Unknown position'), (2, 7, 'Size must be a percentage from 1% to 100%')])
        self.write('# One\n\n![position=bottom-right size=10%](logo.gif)\n')
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])

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

    def test_components_check_render_and_icons(self):
        self.write('# Results\n\n<Row>\n<Stat value="42" label="Answer"/>\n<Crad/>\n</Row>\n')
        report = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)
        message = report['problems'][0]['message']
        self.assertIn('<Crad>', message)
        self.assertIn('did you mean <Card>', message)
        self.write('# Results\n\n<Row>\n<Stat value="42" label="Answer"/>\n<Card icon="rocket" title="Go"/>\n</Row>\n')
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])
        self.hype('render', self.deck, '--slide', '1', '-o', self.root / 'one.png')
        self.assertEqual(self.png_size(self.root / 'one.png'), (1920, 1080))
        self.assertIn('rocket', json.loads(self.hype('icons', 'rocket', '--json').stdout)['icons'])
        self.assertIn('chart-bar', self.hype('icons', 'chart').stdout.split())
        self.assertIn('No icon matches', self.hype('icons', 'zzzzqq', code=1).stderr)
        guide = self.hype('help', 'components').stdout
        for name in ('<Card', '<Chart', '<Gantt', '<Define', '<Slot'):
            self.assertIn(name, guide)
        self.assertIn('help components', self.hype('help').stdout)

    def test_new_starts_from_a_template(self):
        template = self.root / 'talk/templates/brand/template.md'
        template.parent.mkdir(parents=True)
        template.write_text('---\ncolor_accent: "#123456"\nframe: Frame\n---\n\n'
                            '<Define name="Frame">\n<Place at="bottom" inset="0" height="8" fill="accent"/>\n{children}\n</Define>\n')
        self.hype('new', self.deck, '--template', 'brand')
        text = self.deck.read_text()
        self.assertIn('template: "brand"', text)
        self.assertNotIn('color_background', text)
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])
        self.assertIn('not found', self.hype('new', self.root / 'other/p.md', '--template', 'nope', code=1).stderr)
        template.write_text('<Define name="Card">\nx\n</Define>\n')
        report = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)
        self.assertIn('built-in', report['problems'][0]['message'])

    def test_any_presentation_is_a_template(self):
        kit = self.root / 'kit/presentation.md'
        kit.parent.mkdir(parents=True)
        kit.write_text('---\ntitle: Kit\ntheme: paper\ncolor_accent: "#123456"\n---\n\n<Banner>Hi</Banner>\n\n'
                       '<Define name="Banner">\n<Card title="Banner">\n{children}\n</Card>\n</Define>\n')
        self.assertEqual(json.loads(self.hype('check', kit, '--json').stdout)['problems'], [])
        self.hype('new', self.deck, '--template', '../kit')
        self.assertIn('template: "../kit"', self.deck.read_text())
        self.write(self.deck.read_text() + '\n<Banner>Again</Banner>\n')
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])
        slides = json.loads(self.hype('slides', kit, '--json').stdout)['slides']
        self.assertEqual(slides[0]['title'], '<Banner>Hi</Banner>')
        # A definition's mistakes point at its slide.
        self.write(self.deck.read_text() + '\n<Define name="banner">\nx\n</Define>\n')
        problems = json.loads(self.hype('check', self.deck, '--json', code=1).stdout)['problems']
        self.assertEqual([p['message'] for p in problems], ['Slide line 6: <Define> needs name="PascalCase"'])

    def test_template_library(self):
        templates = json.loads(self.hype('templates', '--json').stdout)
        self.assertEqual(templates['library'], str(self.root / 'home/.local/share/hype/templates'))
        self.assertEqual([(t['name'], t['where']) for t in templates['templates']], [('omarchy', 'built-in')])
        self.assertIn('Title', templates['templates'][0]['components'])
        kit = self.root / 'kit/presentation.md'
        kit.parent.mkdir(parents=True)
        kit.write_text('---\ntitle: Kit\ncolor_accent: "#123456"\n---\n\n<Banner>Hi</Banner>\n\n'
                       '<Define name="Banner">\n<Card title="Banner">\n{children}\n</Card>\n</Define>\n')
        saved = json.loads(self.hype('template', 'save', kit, '--json').stdout)
        self.assertEqual((saved['name'], saved['copy']), ('kit', False))
        self.assertIn('already exists', self.hype('template', 'save', kit, code=1).stderr)
        self.hype('template', 'save', kit, '--name', 'kit-2026', '--copy')
        listed = {t['name']: t for t in json.loads(self.hype('templates', '--json').stdout)['templates']}
        self.assertEqual(sorted(listed), ['kit', 'kit-2026', 'omarchy'])
        self.assertEqual(listed['kit']['linked'], str(kit.resolve()))
        self.assertIsNone(listed['kit-2026']['linked'])
        self.assertEqual(listed['kit']['slides'], 1)
        self.assertEqual(listed['kit']['starters'], [{'slide': 1, 'label': 'Banner · Hi', 'section': ''}])
        # Its slides, ready to paste: a list, or one slide's Markdown without the definitions.
        self.assertIn('Banner · Hi', self.hype('template', 'slides', 'kit').stdout)
        self.assertEqual(self.hype('template', 'slides', 'kit', '1').stdout.strip(), '<Banner>Hi</Banner>')
        one = json.loads(self.hype('template', 'slides', str(kit), '--json').stdout)
        self.assertEqual(one['slides'][0]['source'], '<Banner>Hi</Banner>')
        self.assertIn('from 1 to 1', self.hype('template', 'slides', 'kit', '2', code=1).stderr)
        # Any presentation, anywhere, names a library template by its name.
        self.hype('new', self.deck, '--template', 'kit')
        self.write(self.deck.read_text() + '\n<Banner>Again</Banner>\n')
        self.assertEqual(json.loads(self.hype('check', self.deck, '--json').stdout)['problems'], [])
        self.assertIn('follows ' + str(kit.resolve()), self.hype('templates').stdout)
        self.assertIn('untouched', self.hype('template', 'remove', 'kit').stdout)
        self.assertTrue(kit.exists())
        self.assertIn('not found', self.hype('check', self.deck, code=1).stderr)
        self.assertIn('No template', self.hype('template', 'remove', 'kit', code=1).stderr)
        # The Omarchy template is built in, and checks clean.
        omarchy = self.root / 'om/presentation.md'
        self.hype('new', omarchy, '--template', 'omarchy')
        self.assertIn('title: "om"', omarchy.read_text())
        self.assertEqual(json.loads(self.hype('check', omarchy, '--json').stdout)['problems'], [])
        # The desktop's theme is a theme named current, once Omarchy has one.
        self.assertNotIn('current', json.loads(self.hype('themes', '--json').stdout)['themes'])
        desktop = self.root / 'home/.local/state/omarchy/current/theme'
        desktop.mkdir(parents=True)
        (desktop / 'colors.toml').write_text('background = "#203040"\nforeground = "#eeeeee"\n')
        self.assertIn('current', json.loads(self.hype('themes', '--json').stdout)['themes'])

    def fonts_server(self, manifest_files=True):
        """A stand-in for Google Fonts: its catalog, the download manifest, the CSS API and files."""
        import http.server, threading, urllib.parse
        font = subprocess.run(['fc-match', '-f', '%{file}', 'Liberation Serif'], capture_output=True, text=True).stdout
        if not font.endswith('.ttf'):
            self.skipTest('Liberation Serif is not installed')
        data = Path(font).read_bytes()
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                url = urllib.parse.urlparse(self.path)
                query = urllib.parse.parse_qs(url.query)
                requests.append(url.path)
                body, kind = b'', 'application/json'
                if url.path == '/metadata/fonts':
                    body = b")]}'" + json.dumps({'familyMetadataList': [
                        {'family': 'Liberation Serif', 'category': 'Serif', 'popularity': 1, 'fonts': {'400': {}}},
                        {'family': 'Poppins', 'category': 'Sans Serif', 'popularity': 2, 'fonts': {'400': {}, '900': {}}},
                    ]}).encode()
                elif url.path == '/download/list':
                    refs = [{'filename': 'static/LiberationSerif-Regular.ttf', 'url': 'https://fonts.gstatic.com/s/serif.ttf'},
                            {'filename': 'static/LiberationSerif_Condensed-Regular.ttf', 'url': 'https://fonts.gstatic.com/s/narrow.ttf'},
                            {'filename': 'LiberationSerif-VariableFont_wght.ttf', 'url': 'https://fonts.gstatic.com/s/variable.ttf'}]
                    body = b")]}'" + json.dumps({'manifest': {
                        'files': [{'filename': 'OFL.txt', 'contents': 'SIL Open Font License'}],
                        'fileRefs': refs if manifest_files else []}}).encode()
                elif url.path == '/css2':
                    self.server.family = query['family'][0]
                    body = b"@font-face { font-family: 'Liberation Serif'; font-style: normal; font-weight: 400; " \
                           b"src: url(https://fonts.gstatic.com/s/serif.ttf) format('truetype'); }"
                    kind = 'text/css'
                elif url.path.startswith('/s/'):
                    body, kind = data, 'font/ttf'
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Type', kind)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.env.update(HYPE_FONTS_SERVER=f'http://127.0.0.1:{server.server_address[1]}',
                        XDG_DATA_HOME=str(self.root / 'data'), XDG_CACHE_HOME=str(self.root / 'cache'))
        return requests

    def test_fonts_come_from_google_fonts_with_their_license(self):
        requests = self.fonts_server()
        self.write('---\nfont: "Liberation Serif"\n---\n\n# Hello\n')
        added = json.loads(self.hype('fonts', 'add', 'liberation serif', '--into', self.deck, '--json').stdout)['added']
        self.assertEqual(added[0]['family'], 'Liberation Serif')  # The catalog's spelling.
        downloaded = self.root / 'data/hype/fonts/Liberation Serif'
        # Only the family's own static files: no other widths, no variable font.
        self.assertEqual(sorted(p.name for p in downloaded.iterdir()), ['LiberationSerif-Regular.ttf', 'OFL.txt'])
        bundled = sorted(p.name for p in (self.deck.parent / 'fonts').iterdir())
        self.assertEqual(bundled, ['LiberationSerif-OFL.txt', 'LiberationSerif-Regular.ttf'])
        self.assertNotIn('/s/narrow.ttf', requests)
        # The catalog is cached, so a second family needs no new catalog request.
        catalog_requests = requests.count('/metadata/fonts')
        self.assertIn('Poppins', self.hype('fonts', 'search', 'pop').stdout.split('\n'))
        self.assertEqual(requests.count('/metadata/fonts'), catalog_requests)
        report = {f['family']: f['source'] for f in json.loads(self.hype('fonts', self.deck, '--json').stdout)['fonts']}
        self.assertIn('Liberation Serif', report)
        # A misspelled family is reported with the closest names, and the slide still renders.
        self.write('# Hello\n\n<Text font="Poppinz">Hi</Text>\n')
        problems = json.loads(self.hype('check', self.deck, '--json').stdout)['problems']
        self.assertEqual(problems[0]['slide'], 1)
        self.assertEqual(problems[0]['severity'], 'warning')
        self.assertIn('did you mean Poppins', problems[0]['message'])
        self.assertIn('not on Google Fonts', self.hype('fonts', 'add', 'Poppinz', code=1).stderr)
        self.env['HYPE_FONT_DOWNLOADS'] = '0'
        self.assertIn('turned off', self.hype('fonts', 'add', 'Poppins', code=1).stderr)

    def test_fonts_fall_back_to_the_css_api(self):
        self.fonts_server(manifest_files=False)
        self.hype('fonts', 'add', 'Liberation Serif')
        downloaded = self.root / 'data/hype/fonts/Liberation Serif'
        self.assertEqual(sorted(p.name for p in downloaded.iterdir()), ['LiberationSerif-Regular.ttf', 'OFL.txt'])


if __name__ == '__main__':
    unittest.main()
