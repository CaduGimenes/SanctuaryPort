#!/usr/bin/env python3
"""Lists the launcher's UI texts that have no translation in the given languages.

usage: python3 tools/i18n_audit.py [lang ...] [--json OUT]
The texts are the string literals passed to tr()/tr_fmt() in src/ and the option labels of
src/core/options.rs (c()/cr()); a translation is looked up the way src/core/i18n.rs does:
the 'ru' table for Russian, else 'extra' then the positional table, then UI_EXTRA.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STR = r'"((?:[^"\\]|\\.)*)"'


def unescape(text):
    return json.loads(f'"{text}"')


def ui_texts():
    found = {}
    for path in sorted((ROOT / 'src').rglob('*.rs')):
        source = path.read_text()
        for m in re.finditer(r'\btr(?:_fmt)?\(\s*&?[\w.]+\s*,\s*' + STR, source):
            found.setdefault(unescape(m.group(1)), str(path.relative_to(ROOT)))
    options = (ROOT / 'src/core/options.rs').read_text()
    for m in re.finditer(r'\bcr?\(\s*' + STR + r'\s*,\s*' + STR, options):
        found.setdefault(unescape(m.group(2)), 'src/core/options.rs')
    # Texts kept in constants: tr(lang, NAME) / tr(lang, NAME[i]) / hint(NAME).
    for path in sorted((ROOT / 'src').rglob('*.rs')):
        source = path.read_text()
        used = set(re.findall(r'\b(?:tr(?:_fmt)?\(\s*&?[\w.]+\s*,|hint\()\s*&?([A-Z][A-Z0-9_]{2,})', source))
        for m in re.finditer(r'const\s+([A-Z][A-Z0-9_]+)\s*:\s*(?:&(?:\'static\s+)?str|\[&str;\s*\d+\])\s*=(.*?);\n',
                             source, re.S):
            if m.group(1) in used:
                for literal in re.findall(STR, m.group(2)):
                    found.setdefault(unescape(literal), str(path.relative_to(ROOT)))
    return found


def tables():
    table = json.loads((ROOT / 'assets/i18n.json').read_text())
    index = {key: i for i, key in enumerate(table['keys'])}
    source = (ROOT / 'src/core/i18n.rs').read_text()
    block = source[source.index('static UI_EXTRA'):source.index('fn ui_extra')]
    ui_extra = {}
    # Each entry starts at '(' indented by 4 and holds its English text, then its (lang, text) pairs.
    starts = [m.start() for m in re.finditer(r'\n    \(', block)] + [len(block)]
    for begin, end in zip(starts, starts[1:]):
        entry = block[begin:end]
        head = re.match(r'\s*\(\s*' + STR + r'\s*,\s*&\[', entry)
        if head:
            ui_extra[unescape(head.group(1))] = {lang: unescape(text) for lang, text in
                                                 re.findall(r'\(\s*"(\w+)"\s*,\s*' + STR + r'\s*,?\s*\)', entry)}
    # cr("value", "English", "Russian"): the Russian label of an option.
    options = (ROOT / 'src/core/options.rs').read_text()
    ru_labels = {unescape(m.group(2)): unescape(m.group(3)) for m in
                 re.finditer(r'\bcr\(\s*' + STR + r'\s*,\s*' + STR + r'\s*,\s*' + STR, options)}
    return table, index, ui_extra, ru_labels


def translated(lang, en, table, index, ui_extra, ru_labels):
    if lang == 'ru':
        if table['ru'].get(en) or ru_labels.get(en):
            return True
    else:
        if table['extra'].get(lang, {}).get(en):
            return True
        row = table['translations'].get(lang, [])
        if en in index and index[en] < len(row) and row[index[en]]:
            return True
    return bool(ui_extra.get(en, {}).get(lang))


def main():
    args = sys.argv[1:]
    out = None
    if '--json' in args:
        out = args[args.index('--json') + 1]
        del args[args.index('--json'):args.index('--json') + 2]
    langs = args or ['es', 'pl', 'ru']
    texts = ui_texts()
    tabs = tables()
    missing = {lang: sorted(en for en in texts if not translated(lang, en, *tabs)) for lang in langs}
    for lang in langs:
        print(f'{lang}: {len(missing[lang])} of {len(texts)} untranslated')
    if out:
        Path(out).write_text(json.dumps({'texts': texts, 'missing': missing}, ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main()
