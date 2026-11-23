"""Quit the actual GUI while thumbnail and prefetch renderers are still busy."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

APP = Path(__file__).resolve().parents[1] / 'build/hype'


class ShutdownTests(unittest.TestCase):
    def test_quit_drains_active_renderers_before_qt_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tools = root / 'tools'
            tools.mkdir()
            # Hold real render threads inside syntax highlighting past the
            # screenshot command's 1.8-second quit deadline.
            highlighter = tools / 'source-highlight'
            highlighter.write_text('#!/bin/sh\n'
                                   'echo "start $$" >> "$HYPE_TEST_RENDER_LOG"\n'
                                   'sleep 2\n'
                                   'echo "done $$" >> "$HYPE_TEST_RENDER_LOG"\n')
            highlighter.chmod(0o755)
            deck = root / 'presentation.md'
            deck.write_text('\n---\n'.join(
                f'# Slide {i}\n\n```rust\nfn render_{i}() {{}}\n```\n' for i in range(100)))
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                       QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='generic',
                       QT_QUICK_BACKEND='software', QT_STYLE_OVERRIDE='Fusion',
                       XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'),
                       HYPE_TEST_RENDER_LOG=str(root / 'renders.log'))
            result = subprocess.run([str(APP), str(deck), '--screenshot', str(root / 'screen.png')],
                                    env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / 'screen.png').exists())
            lines = (root / 'renders.log').read_text().splitlines()
            started = {line.split()[1] for line in lines if line.startswith('start ')}
            finished = {line.split()[1] for line in lines if line.startswith('done ')}
            self.assertGreater(len(started), 0)
            self.assertEqual(started, finished, 'The application exited with live render jobs')
            self.assertGreater((root / 'renders.log').stat().st_mtime,
                               (root / 'screen.png').stat().st_mtime,
                               'The test must exercise quitting before render jobs finish')


if __name__ == '__main__':
    unittest.main()
