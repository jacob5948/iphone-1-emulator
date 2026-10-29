#!/usr/bin/env python3
"""Stage web/ for GitHub Pages.

    python3 tools/pages-site.py BASE_URL [RELAY_URL] OUT_DIR

BASE_URL is the published address (actions/configure-pages' base_url, e.g.
https://user.github.io/repo). The page's canonical, sharing and sitemap links
name the original site; they are pointed at BASE_URL instead. RELAY_URL, when
given, becomes the page's default Wi-Fi relay (<meta name="iphone-relay">);
Pages is served over https, so it must be a wss:// address.

index.html changes, so the offline bundle (service-worker.js), which pins the
SHA-256 of every file it caches, is regenerated before copying. Only files the
site serves are copied: sources, tests and build trees stay behind.
"""
import html
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
web = root / 'web'
ORIGINAL = 'https://2007.mobile/'

if len(sys.argv) not in (3, 4):
    raise SystemExit(__doc__)
base = sys.argv[1].rstrip('/') + '/'
relay = sys.argv[2].strip() if len(sys.argv) == 4 else ''
out = Path(sys.argv[-1]).resolve()
if not base.startswith('https://'):
    raise SystemExit(f'BASE_URL must be an https address: {base}')
if relay and not relay.startswith('wss://'):
    raise SystemExit(f'RELAY_URL must be a wss:// address (the site is https): {relay}')

for name in ('index.html', 'robots.txt', 'sitemap.xml'):
    path = web / name
    text = path.read_text()
    if ORIGINAL not in text:
        raise SystemExit(f'{name} no longer names {ORIGINAL}; update tools/pages-site.py')
    text = text.replace(ORIGINAL, base)
    if name == 'index.html' and relay:
        meta = f'  <meta name="iphone-relay" content="{html.escape(relay, quote=True)}">\n'
        text = text.replace('</head>', meta + '</head>', 1)
    path.write_text(text)

subprocess.run([sys.executable, str(root / 'tools' / 'build-offline.py')], check=True)

if out.exists():
    shutil.rmtree(out)
shutil.copytree(web, out, ignore=shutil.ignore_patterns(
    'build', 'tests', '*.c', '*.h', 'smoke.mjs', 'service-worker.template.js'))
print(f'Staged {sum(1 for p in out.rglob("*") if p.is_file())} files for {base}'
      + (f' with relay {relay}' if relay else ''))
