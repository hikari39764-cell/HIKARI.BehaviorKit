"""Text/zh.json と Text/ja.json を作る。

  python -I -X utf8 TextSource/make_tables.py <node_types.json>

<node_types.json> は編集器の behaviortree.node_types の data（{"types": [...]}）。節の表示名・説明・分類・参数名・選択肢の
英文の原文を正確に取るためにそれを読み、translations.py の訳と合わせる。訳の無い原文が一つでもあれば止まる
（ゲームの項目が登記した節は対象外：それは項目の ProjectSettings/Text に書く）。"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from translations import NODES, UI, WORDS  # noqa: E402

OWN_PREFIXES = ('Lab.', 'MyGame.')


def main():
    types = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'))['types']
    tables = ({}, {})
    missing = []

    def put(source, pair):
        for table, text in zip(tables, pair):
            table[source] = text

    for words in (UI, WORDS):
        for source, pair in words.items():
            put(source, pair)
    for entry in types:
        if entry['type'].startswith(OWN_PREFIXES):
            continue
        node = NODES.get(entry['type'])
        if node is None:
            missing.append(f'node {entry["type"]}')
            continue
        put(entry['name'], node[0:2])
        put(entry['description'], node[2:4])
        for text in [entry['category']] + [p['label'] for p in entry['params']] + \
                [c for p in entry['params'] for c in p.get('choices', [])]:
            if any(ch.isalpha() for ch in text) and text not in tables[0]:
                missing.append(f'{entry["type"]}: {text}')
    if missing:
        sys.exit('missing translations:\n  ' + '\n  '.join(missing))
    out = HERE.parent / 'Text'
    out.mkdir(exist_ok=True)
    for name, table in zip(('zh.json', 'ja.json'), tables):
        (out / name).write_text(json.dumps(dict(sorted(table.items())), ensure_ascii=False, indent=2) + '\n',
                                encoding='utf-8', newline='\n')
    print('wrote', len(tables[0]), 'entries per language')


if __name__ == '__main__':
    main()
