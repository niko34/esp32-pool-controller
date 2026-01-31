from pathlib import Path
import re
from typing import List, Dict

class RepoReader:
    def __init__(self, root: str = "."):
        self.root = Path(root)

    def list_files(self, max_files: int = 1500) -> List[str]:
        files = []
        for p in self.root.rglob("*"):
            if not p.is_file():
                continue
            s = str(p)
            # ignore heavy / irrelevant dirs
            if any(x in s for x in ["/.git/", "/.venv/", "/node_modules/", "/.pio/", "/dist/", "/build/"]):
                continue
            files.append(s)
            if len(files) >= max_files:
                break
        return files

    def read_file(self, path: str, max_chars: int = 30000) -> str:
        p = self.root / path
        if not p.exists() or not p.is_file():
            return ""
        return p.read_text(encoding="utf-8", errors="ignore")[:max_chars]

    def search(self, pattern: str, max_hits: int = 50) -> List[Dict]:
        rx = re.compile(pattern)
        hits = []
        for f in self.list_files():
            # scan only likely text/code files
            if not any(f.endswith(ext) for ext in [".cpp", ".h", ".c", ".ino", ".js", ".html", ".css", ".md", ".json", ".yml", ".yaml"]):
                continue
            try:
                txt = Path(f).read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue
            for i, line in enumerate(txt.splitlines(), start=1):
                if rx.search(line):
                    hits.append({"file": f, "line": i, "text": line.strip()[:220]})
                    if len(hits) >= max_hits:
                        return hits
        return hits
