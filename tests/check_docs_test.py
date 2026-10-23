#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run with Python 3: python tests/check_docs_test.py (all scratch data: local/tmp)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TMP = ROOT / "local/tmp"
GOOD = "# 文档\n\n中文使用“弯引号”。\n"
BAD = '# 文档\n\n"中文直引号"\n'
PENDING = "# 挂账\n\n## 背景\n\n尚需复核。\n\n## 出口条件\n\n复核后删除。\n"


class GuardTests(unittest.TestCase):
    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="check-docs-", dir=TMP)
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.env = os.environ.copy()
        # Do not inherit a caller's alternate index/worktree into the fixture.
        for key in list(self.env):
            if key.startswith("GIT_"):
                del self.env[key]
        self.env.update(PYTHONUTF8="1", SKIP_DOCS="0")
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        for path in ("scripts/check-docs.sh", "scripts/check-docs.py", ".githooks/pre-commit"):
            self.put(path, (ROOT / path).read_text(encoding="utf-8"))
        self.put("README.md", GOOD)
        self.put(".gitignore", "local/\n")
        self.git("add", ".")

    def run_command(self, *args, env=None):
        return subprocess.run(args, cwd=self.repo, env=env or self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              encoding="utf-8", errors="replace")

    def git(self, *args):
        result = self.run_command("git", *args)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result

    def put(self, path, content, stage=False):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        if stage:
            self.git("add", "--", path)

    def guard(self, *args, hook=False, shell=False, env=None):
        command = (["bash", ".githooks/pre-commit"] if hook else
                   ["bash", "scripts/check-docs.sh", *args] if shell else
                   [sys.executable, "scripts/check-docs.py", *args])
        return self.run_command(*command, env=env)

    def assert_guard(self, success, *args, contains=None, **kwargs):
        result = self.guard(*args, **kwargs)
        self.assertEqual(result.returncode, 0 if success else 1, result.stdout)
        if contains:
            self.assertIn(contains, result.stdout)
        return result

    def test_partial_stage_bad_index_good_worktree(self):
        self.put("README.md", BAD, stage=True)
        self.put("README.md", GOOD)
        self.assert_guard(True)
        self.assert_guard(False, "--staged", contains="ASCII")
        self.assert_guard(False, hook=True, contains="ASCII")

    def test_partial_stage_good_index_bad_worktree(self):
        self.put("README.md", BAD)
        self.assert_guard(False, contains="ASCII")
        self.assert_guard(True, "--staged")
        self.assert_guard(True, hook=True)

    def test_staged_link_uses_index_paths_not_disk(self):
        self.put("README.md", "[目标](doc/目标.md)\n", stage=True)
        self.put("doc/目标.md", GOOD)
        self.assert_guard(True)
        self.assert_guard(False, "--staged", contains="指针悬空")
        self.git("add", "doc/目标.md")
        (self.repo / "doc/目标.md").unlink()
        self.assert_guard(True, "--staged")
        self.assert_guard(False, contains="指针悬空")

    def test_only_tracked_public_docs_not_local_nested_repo(self):
        self.put("local/AGENTS.md", BAD)
        self.put("local/doc/deep/README.md", BAD)
        nested = self.run_command("git", "-C", "local", "init", "-q")
        self.assertEqual(nested.returncode, 0, nested.stdout)
        self.put("doc/untracked.md", BAD)
        self.put("app/deep/AGENTS.md", BAD, stage=True)
        self.assert_guard(False, contains="app/deep/AGENTS.md")
        self.put("app/deep/AGENTS.md", GOOD, stage=True)
        self.assert_guard(True)
        self.assert_guard(True, "--staged")

    def test_pending_format_background_exit_and_empty_section(self):
        path = "doc/pending/261020-audio-gap.md"
        self.put(path, PENDING, stage=True)
        self.assert_guard(True)
        for heading in ("背景", "出口条件"):
            with self.subTest(heading=heading):
                self.put(path, PENDING.replace("## " + heading, "## 其他"))
                self.assert_guard(False, contains=heading)
        self.put(path, "## 背景\n内容\n## 出口条件\n\n")
        self.assert_guard(False, contains="出口条件")
        self.put(path, PENDING)
        self.put("doc/pending/261332-bad.md", PENDING, stage=True)
        self.assert_guard(False, contains="文件名")

    def test_missing_paths_links_and_private_notes(self):
        samples = (("`core/missing.hpp`\n", "指针悬空"),
                   ("[坏链接](../missing.md)\n", "指针悬空"),
                   ("[ref]: <../missing.md>\n", "指针悬空"),
                   ("`local/doc/session.md`\n", "公开文档不可依赖本机笔记"),
                   ("[笔记](../local/doc/session.md)\n", "公开文档不可依赖本机笔记"))
        for text, error in samples:
            with self.subTest(text=text):
                self.put("doc/deep.md", text, stage=True)
                self.assert_guard(False, contains=error)

    def test_recursive_prose_private_note_dependency(self):
        self.put("doc/deep/nested/notes.md", "详见 local/doc/session.md。\n", stage=True)
        self.assert_guard(False, contains="公开文档不可依赖本机笔记")
        self.assert_guard(False, "--staged", contains="doc/deep/nested/notes.md")

    def test_utf8_links_anchors_templates_and_legal_code(self):
        self.put("doc/中文 文件.md", GOOD, stage=True)
        text = GOOD + '''[中文](<中文 文件.md#任意标题>)
[编码](%E4%B8%AD%E6%96%87%20%E6%96%87%E4%BB%B6.md)
[网址](https://example.com/missing) [本页](#unchecked)
`core/*/example.hpp` `"中文代码"`
~~~python
"中文" 「代码」 `core/no.hpp` [例子](missing.md)
```
CONFLICT_EXAMPLE
~~~
````markdown
~~~
```
"中文" `doc/missing.md`
````
'''.replace("CONFLICT_EXAMPLE", "<" * 7 + " example")
        self.put("doc/examples.md", text, stage=True)
        self.assert_guard(True)
        self.assert_guard(True, "--staged")
        (self.repo / "doc/examples.md").write_bytes(b"\xff")
        self.assert_guard(False, contains="UTF-8")

    def test_corner_quotes_conflicts_and_pr31_prefix(self):
        for text, error in (("「错误引号」", "直角引号"),
                            ("<<<<<<< HEAD\n=======\n>>>>>>> other\n", "冲突标记"),
                            (("| 抽象 | 设计决策 | 格式差异收敛处 |# 标题\n") * 3, "表头前缀")):
            with self.subTest(error=error):
                self.put("README.md", text)
                self.assert_guard(False, contains=error)

    def shell_env(self, py_launcher=False):
        bin_dir = self.repo / "local/bin"
        bin_dir.mkdir(parents=True)
        scripts = {"python3": "exit 49\n", "rg": "exit 127\n"}
        executable = shlex.quote(Path(sys.executable).as_posix())
        if py_launcher:
            scripts["python"] = "exit 49\n"
            scripts["py"] = '[ "$1" = -3 ] || exit 92\nshift\nexec ' + executable + ' "$@"\n'
        else:
            scripts["python"] = "exec " + executable + ' "$@"\n'
        for name, body in scripts.items():
            target = bin_dir / name
            with target.open("w", encoding="utf-8", newline="\n") as stream:
                stream.write("#!/bin/sh\n" + body)
            target.chmod(0o755)
        env = self.env.copy()
        env["PATH"] = str(bin_dir) + os.pathsep + env["PATH"]
        return env

    def test_no_rg_broken_python3_falls_back_to_python(self):
        env = self.shell_env()
        self.assert_guard(True, shell=True, env=env)
        self.put("README.md", BAD)
        self.assert_guard(False, shell=True, env=env, contains="ASCII")

    def test_py_minus_three_fallback(self):
        self.assert_guard(True, shell=True, env=self.shell_env(py_launcher=True))


if __name__ == "__main__":
    unittest.main(verbosity=2)
