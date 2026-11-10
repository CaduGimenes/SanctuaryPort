#!/usr/bin/env python3
"""Puts the Python the Windows package ships into DEST: python.org's embeddable Python (no
installation, no registry) with the wheels run.py's scripts import (Pillow for the window icon,
iced-x86 for the ISA rewrite on CPUs without MOVBE/BMI1/LZCNT). Downloads are kept in CACHE.

usage: fetch_python.py CACHE DEST
"""
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

PYTHON = '3.12.10'
TAG = 'cp312'
WHEELS = [('pillow', None), ('iced-x86', '1.21.0')]


def download(url, target):
    if not target.exists():
        print('fetch', url, flush=True)
        with urllib.request.urlopen(url) as response:
            data = response.read()
        target.write_bytes(data)
    return target


def wheel_url(name, version):
    url = f'https://pypi.org/pypi/{name}/{version}/json' if version else f'https://pypi.org/pypi/{name}/json'
    with urllib.request.urlopen(url) as response:
        files = json.load(response)['urls']
    for wanted in (f'-{TAG}-{TAG}-win_amd64.whl', '-abi3-win_amd64.whl'):
        for entry in files:
            if entry['filename'].endswith(wanted) and entry['filename'].split('-')[2].startswith('cp'):
                return entry['url'], entry['filename']
    raise SystemExit(f'No Windows wheel of {name} for {TAG}')


def main():
    cache, dest = Path(sys.argv[1]), Path(sys.argv[2])
    cache.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        shutil.rmtree(dest)
    embed = download(f'https://www.python.org/ftp/python/{PYTHON}/python-{PYTHON}-embed-amd64.zip',
                     cache / f'python-{PYTHON}-embed-amd64.zip')
    with zipfile.ZipFile(embed) as archive:
        archive.extractall(dest)
    site = dest / 'Lib' / 'site-packages'
    site.mkdir(parents=True)
    for name, version in WHEELS:
        url, filename = wheel_url(name, version)
        with zipfile.ZipFile(download(url, cache / filename)) as archive:
            archive.extractall(site)
    # The embeddable Python reads its search path from python3XX._pth only, without the folder of
    # the script it runs: add site-packages, the port folder (run.py) and scripts/ (the scripts
    # import each other), relative to python/.
    pth = next(dest.glob('python3*._pth'))
    lines = [line for line in pth.read_text().splitlines() if line.strip() != '#import site']
    lines[1:1] = ['Lib\\site-packages', '..', '..\\scripts']
    lines.append('import site')
    pth.write_text('\n'.join(lines) + '\n')
    print('Python', PYTHON, 'in', dest)


if __name__ == '__main__':
    main()
