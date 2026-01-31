"""
code_tools.py - Outils CrewAI pour lecture et analyse du code

Ces outils permettent aux agents de:
- Lire des fichiers spécifiques
- Chercher dans le code (patterns, fonctions, routes)
- Lister la structure du projet
- Extraire des informations structurées
"""

import re
from pathlib import Path
from typing import Optional

from crewai.tools import BaseTool
from pydantic import Field


# Répertoires à ignorer
IGNORED_DIRS = {
    ".git", ".pio", "node_modules", ".venv", "venv",
    "__pycache__", "dist", "build", "data-build", ".cache"
}

# Extensions de fichiers de code
CODE_EXTENSIONS = {
    ".cpp", ".c", ".h", ".hpp", ".ino",  # C/C++
    ".js", ".ts", ".jsx", ".tsx",         # JavaScript
    ".html", ".css", ".scss",             # Web
    ".py",                                 # Python
    ".json", ".yaml", ".yml",             # Config
    ".md", ".txt",                        # Docs
}


def get_repo_root() -> Path:
    """Retourne la racine du repo (où se trouve platformio.ini)."""
    current = Path.cwd()
    for parent in [current] + list(current.parents):
        if (parent / "platformio.ini").exists():
            return parent
    return current


def should_ignore(path: Path) -> bool:
    """Vérifie si un chemin doit être ignoré."""
    parts = path.parts
    return any(part in IGNORED_DIRS for part in parts)


def is_code_file(path: Path) -> bool:
    """Vérifie si c'est un fichier de code."""
    return path.suffix.lower() in CODE_EXTENSIONS


class ReadFileTool(BaseTool):
    """Lit le contenu d'un fichier du projet."""

    name: str = "read_file"
    description: str = """
    Lit le contenu complet d'un fichier du projet.
    Input: chemin relatif du fichier (ex: "src/main.cpp", "data/app.js")
    Output: contenu du fichier ou message d'erreur
    """

    def _run(self, file_path: str) -> str:
        root = get_repo_root()
        target = root / file_path.strip()

        if not target.exists():
            return f"ERREUR: Fichier non trouvé: {file_path}"

        if not target.is_file():
            return f"ERREUR: {file_path} n'est pas un fichier"

        try:
            content = target.read_text(encoding="utf-8", errors="ignore")
            lines = content.split("\n")

            # Ajouter numéros de ligne
            numbered = []
            for i, line in enumerate(lines, 1):
                numbered.append(f"{i:4d} | {line}")

            result = "\n".join(numbered)

            # Tronquer si trop long (limite réduite pour éviter rate limit API)
            if len(result) > 12000:
                result = result[:12000] + "\n\n... [TRONQUÉ - utilisez get_file_context pour un résumé]"

            return f"=== {file_path} ({len(lines)} lignes) ===\n\n{result}"

        except Exception as e:
            return f"ERREUR lecture {file_path}: {e}"


class SearchCodeTool(BaseTool):
    """Cherche un pattern dans le code du projet."""

    name: str = "search_code"
    description: str = """
    Cherche un pattern (regex) dans tous les fichiers de code du projet.
    Input: pattern regex à chercher (ex: "server->on", "function\\s+\\w+", "fetchConfig")
    Output: liste des occurrences avec fichier, ligne et contexte
    """

    def _run(self, pattern: str) -> str:
        root = get_repo_root()

        try:
            regex = re.compile(pattern, re.IGNORECASE)
        except re.error as e:
            return f"ERREUR: Pattern regex invalide: {e}"

        results = []
        files_scanned = 0

        for file_path in root.rglob("*"):
            if not file_path.is_file():
                continue
            if should_ignore(file_path):
                continue
            if not is_code_file(file_path):
                continue

            files_scanned += 1
            relative_path = file_path.relative_to(root)

            try:
                content = file_path.read_text(encoding="utf-8", errors="ignore")
                lines = content.split("\n")

                for i, line in enumerate(lines, 1):
                    if regex.search(line):
                        # Contexte: 1 ligne avant/après
                        context_before = lines[i-2] if i > 1 else ""
                        context_after = lines[i] if i < len(lines) else ""

                        results.append({
                            "file": str(relative_path),
                            "line": i,
                            "match": line.strip()[:150],
                            "context_before": context_before.strip()[:100] if context_before else "",
                            "context_after": context_after.strip()[:100] if context_after else "",
                        })

                        if len(results) >= 50:
                            break

            except Exception:
                continue

            if len(results) >= 50:
                break

        if not results:
            return f"Aucun résultat pour '{pattern}' ({files_scanned} fichiers scannés)"

        output = [f"=== Recherche: {pattern} ({len(results)} résultats) ===\n"]

        for r in results:
            output.append(f"\n📍 {r['file']}:{r['line']}")
            if r['context_before']:
                output.append(f"   {r['line']-1} | {r['context_before']}")
            output.append(f" ► {r['line']} | {r['match']}")
            if r['context_after']:
                output.append(f"   {r['line']+1} | {r['context_after']}")

        return "\n".join(output)


class ListFilesTool(BaseTool):
    """Liste les fichiers du projet."""

    name: str = "list_files"
    description: str = """
    Liste les fichiers du projet, optionnellement filtrés par répertoire ou extension.
    Input: filtre optionnel (ex: "src/", "*.cpp", "data/*.js")
    Output: liste des fichiers avec leur taille
    """

    def _run(self, filter_pattern: str = "") -> str:
        root = get_repo_root()
        filter_pattern = filter_pattern.strip()

        files = []

        for file_path in root.rglob("*"):
            if not file_path.is_file():
                continue
            if should_ignore(file_path):
                continue

            relative_path = str(file_path.relative_to(root))

            # Appliquer le filtre
            if filter_pattern:
                if filter_pattern.startswith("*."):
                    # Filtre par extension
                    ext = filter_pattern[1:]
                    if not relative_path.endswith(ext):
                        continue
                elif filter_pattern.endswith("/"):
                    # Filtre par répertoire
                    if not relative_path.startswith(filter_pattern):
                        continue
                else:
                    # Filtre générique
                    if filter_pattern not in relative_path:
                        continue

            size = file_path.stat().st_size
            files.append((relative_path, size))

        # Trier par chemin
        files.sort(key=lambda x: x[0])

        if not files:
            return f"Aucun fichier trouvé" + (f" pour '{filter_pattern}'" if filter_pattern else "")

        output = [f"=== Fichiers du projet ({len(files)} fichiers) ===\n"]

        current_dir = ""
        for path, size in files[:200]:  # Limiter à 200 fichiers
            dir_part = str(Path(path).parent)
            if dir_part != current_dir:
                current_dir = dir_part
                output.append(f"\n📁 {current_dir}/")

            filename = Path(path).name
            size_str = f"{size:,}".replace(",", " ") + " B"
            output.append(f"   {filename:<40} {size_str:>12}")

        if len(files) > 200:
            output.append(f"\n... et {len(files) - 200} autres fichiers")

        return "\n".join(output)


class GetProjectStructureTool(BaseTool):
    """Retourne la structure du projet avec les fichiers clés."""

    name: str = "get_project_structure"
    description: str = """
    Retourne une vue d'ensemble de la structure du projet ESP32 pool controller.
    Inclut les fichiers principaux, les routes API, et les composants UI.
    Input: aucun
    Output: structure du projet avec descriptions
    """

    def _run(self, _: str = "") -> str:
        root = get_repo_root()

        output = ["=== Structure du projet ESP32 Pool Controller ===\n"]

        # Fichiers firmware
        output.append("\n## Firmware (src/)")
        src_files = sorted((root / "src").glob("*.cpp")) + sorted((root / "src").glob("*.h"))
        for f in src_files:
            if f.exists():
                lines = len(f.read_text(errors="ignore").split("\n"))
                output.append(f"   {f.name:<30} ({lines} lignes)")

        # Fichiers UI
        output.append("\n## Web UI (data/)")
        data_files = ["index.html", "login.html", "wifi.html", "wizard.html", "app.js", "app.css"]
        for fname in data_files:
            f = root / "data" / fname
            if f.exists():
                size = f.stat().st_size
                output.append(f"   {fname:<30} ({size:,} bytes)")

        # Documentation
        output.append("\n## Documentation")
        docs = ["README.md", "API.md", "BUILD.md", "CHANGELOG.md"]
        for doc in docs:
            f = root / doc
            if f.exists():
                output.append(f"   {doc}")

        # Extraire les routes API
        output.append("\n## Routes API (extraites de web_routes_*.cpp)")
        routes = self._extract_routes(root)
        for route in routes[:30]:
            output.append(f"   {route}")

        return "\n".join(output)

    def _extract_routes(self, root: Path) -> list[str]:
        """Extrait les routes API des fichiers web_routes_*.cpp."""
        routes = []
        pattern = re.compile(r'server->on\s*\(\s*"([^"]+)"')

        for cpp_file in root.glob("src/web_routes_*.cpp"):
            try:
                content = cpp_file.read_text(errors="ignore")
                for match in pattern.finditer(content):
                    route = match.group(1)
                    if route not in routes:
                        routes.append(route)
            except Exception:
                continue

        return sorted(routes)


class GetFileContextTool(BaseTool):
    """Retourne le contexte d'un fichier (imports, exports, fonctions principales)."""

    name: str = "get_file_context"
    description: str = """
    Analyse un fichier et retourne son contexte: imports, exports, fonctions/classes principales.
    Utile pour comprendre un fichier avant de le modifier.
    Input: chemin relatif du fichier (ex: "src/sensors.cpp")
    Output: résumé structuré du fichier
    """

    def _run(self, file_path: str) -> str:
        root = get_repo_root()
        target = root / file_path.strip()

        if not target.exists():
            return f"ERREUR: Fichier non trouvé: {file_path}"

        try:
            content = target.read_text(encoding="utf-8", errors="ignore")
        except Exception as e:
            return f"ERREUR lecture: {e}"

        lines = content.split("\n")
        ext = target.suffix.lower()

        output = [f"=== Contexte: {file_path} ===\n"]
        output.append(f"Taille: {len(lines)} lignes, {len(content):,} bytes")

        if ext in {".cpp", ".c", ".h", ".hpp"}:
            output.extend(self._analyze_cpp(content, lines))
        elif ext in {".js", ".ts"}:
            output.extend(self._analyze_js(content, lines))
        elif ext == ".html":
            output.extend(self._analyze_html(content, lines))

        return "\n".join(output)

    def _analyze_cpp(self, content: str, lines: list[str]) -> list[str]:
        output = []

        # Includes
        includes = [l.strip() for l in lines if l.strip().startswith("#include")]
        if includes:
            output.append("\n## Includes")
            for inc in includes[:20]:
                output.append(f"   {inc}")

        # Fonctions (déclarations)
        func_pattern = re.compile(r'^(void|bool|int|float|String|char|uint\w+|size_t)\s+(\w+)\s*\(')
        funcs = []
        for i, line in enumerate(lines, 1):
            match = func_pattern.match(line.strip())
            if match:
                funcs.append(f"{match.group(2)}() - ligne {i}")

        if funcs:
            output.append("\n## Fonctions")
            for f in funcs[:30]:
                output.append(f"   {f}")

        return output

    def _analyze_js(self, content: str, lines: list[str]) -> list[str]:
        output = []

        # Fonctions
        func_patterns = [
            re.compile(r'function\s+(\w+)\s*\('),
            re.compile(r'const\s+(\w+)\s*=\s*(?:async\s*)?\('),
            re.compile(r'(\w+)\s*:\s*(?:async\s*)?function'),
        ]

        funcs = []
        for i, line in enumerate(lines, 1):
            for pattern in func_patterns:
                match = pattern.search(line)
                if match:
                    funcs.append(f"{match.group(1)}() - ligne {i}")
                    break

        if funcs:
            output.append("\n## Fonctions")
            for f in funcs[:40]:
                output.append(f"   {f}")

        # Fetch calls
        fetch_pattern = re.compile(r"fetch\s*\(\s*['\"`]([^'\"`]+)")
        fetches = []
        for i, line in enumerate(lines, 1):
            match = fetch_pattern.search(line)
            if match:
                fetches.append(f"{match.group(1)} - ligne {i}")

        if fetches:
            output.append("\n## Appels API (fetch)")
            for f in fetches[:20]:
                output.append(f"   {f}")

        return output

    def _analyze_html(self, content: str, lines: list[str]) -> list[str]:
        output = []

        # IDs importants
        id_pattern = re.compile(r'id\s*=\s*["\']([^"\']+)')
        ids = set()
        for line in lines:
            for match in id_pattern.finditer(line):
                ids.add(match.group(1))

        if ids:
            output.append("\n## IDs")
            for id_name in sorted(ids)[:30]:
                output.append(f"   #{id_name}")

        return output


# Liste de tous les outils disponibles
def get_all_tools():
    """Retourne la liste de tous les outils disponibles."""
    return [
        ReadFileTool(),
        SearchCodeTool(),
        ListFilesTool(),
        GetProjectStructureTool(),
        GetFileContextTool(),
    ]
