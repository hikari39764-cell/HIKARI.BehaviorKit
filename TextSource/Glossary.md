# 行为树模块的用词表

给界面加文字时照这张表用词。树文件、代码、操作的参数名不随语言变（仍是 `Sequence`、`reactive` 这些英文）。

| English | 中文 | 日本語 |
| --- | --- | --- |
| Sequence | 顺序 | シーケンス |
| Selector | 选择 | セレクター |
| Parallel | 并行 | パラレル |
| Decorator | 装饰 | デコレーター |
| Action | 动作 | アクション |
| Condition | 条件 | 条件 |
| Subtree | 子树 | サブツリー |
| Root | 根 | ルート |
| Blackboard | 变量 | 変数 |
| Score | 打分（节点）/ 分数（参数） | スコア |
| Utility | 效用 | ユーティリティ |
| Watch | 观察 | 表示 |

- 原文以代码里的英文为准；改了英文原文，要同时改 `translations.py` 的键，再跑 `make_tables.py`。
- 带名字、数字的句子用模板（`{0}`、`{1}`），模板也要进表。
