#!/usr/bin/env python3
"""
generate_patch.py - Génère un patch git à partir du résultat CrewAI

Ce script parse le markdown généré par le pipeline, extrait les blocs de code,
et génère un patch unified diff en comparant avec les fichiers existants.

Usage:
    python -m ai.generate_patch ai/outputs/features/XXXXX/result.md
    python -m ai.generate_patch ai/outputs/features/XXXXX/02_implementation.md
"""

import re
import sys
import difflib
from pathlib import Path


def find_repo_root() -> Path:
    """Trouve la racine du repo."""
    current = Path(__file__).resolve().parent.parent
    if (current / "platformio.ini").exists():
        return current
    return Path.cwd()


def extract_code_blocks(content: str) -> list[dict]:
    """
    Extrait les blocs de code du markdown avec leur chemin de fichier.

    Stratégie: trouve d'abord les déclarations de fichiers, puis le bloc de code qui suit.
    """
    blocks = []

    # Étape 1: Trouver toutes les mentions de fichiers avec chemin
    file_mentions = []

    # Pattern pour "### Fichier: data/index.html" ou "## Fichier 1: src/main.cpp"
    for match in re.finditer(r'#+\s*Fichier(?:\s*\d+)?[:\s]+([^\n]+)', content, re.IGNORECASE):
        path = match.group(1).strip()
        path = re.sub(r'^[`\s]+|[`\s]+$', '', path)
        pos = match.end()
        file_mentions.append((pos, path))

    # Pattern pour "**Fichier:** `data/index.html`"
    for match in re.finditer(r'\*\*Fichier[:\s]*\*\*\s*`?([^`\n]+)`?', content, re.IGNORECASE):
        path = match.group(1).strip()
        pos = match.end()
        file_mentions.append((pos, path))

    # Trier par position
    file_mentions.sort(key=lambda x: x[0])

    # Étape 2: Pour chaque mention de fichier, trouver le prochain bloc de code
    code_block_pattern = re.compile(r'```(\w+)\s*\n(.*?)```', re.DOTALL)

    for pos, file_path in file_mentions:
        # Nettoyer le chemin
        file_path = re.sub(r'\s*\(.*\)$', '', file_path)  # Retirer "(XXX lignes)"

        # Vérifier que c'est un fichier valide
        if not any(file_path.endswith(ext) for ext in ['.cpp', '.h', '.c', '.js', '.html', '.css', '.json', '.md', '.py', '.ino']):
            continue
        if file_path.startswith('chemin/') or file_path.startswith('path/'):
            continue

        # Trouver le prochain bloc de code après cette position
        remaining = content[pos:]
        match = code_block_pattern.search(remaining)

        if match:
            lang = match.group(1).lower()
            code = match.group(2)

            blocks.append({
                'path': file_path,
                'lang': lang,
                'code': code,
            })

    # Dédupliquer (garder le dernier bloc pour chaque fichier)
    seen = {}
    for block in blocks:
        seen[block['path']] = block

    return list(seen.values())


def generate_unified_diff(original: str, modified: str, file_path: str) -> str:
    """Génère un unified diff entre deux contenus."""
    original_lines = original.splitlines(keepends=True)
    modified_lines = modified.splitlines(keepends=True)

    # S'assurer que les lignes se terminent par \n
    if original_lines and not original_lines[-1].endswith('\n'):
        original_lines[-1] += '\n'
    if modified_lines and not modified_lines[-1].endswith('\n'):
        modified_lines[-1] += '\n'

    diff = difflib.unified_diff(
        original_lines,
        modified_lines,
        fromfile=f'a/{file_path}',
        tofile=f'b/{file_path}',
        lineterm=''
    )

    return ''.join(diff)


def main():
    if len(sys.argv) < 2:
        print("Usage: python -m ai.generate_patch <fichier_markdown>")
        print("Exemple: python -m ai.generate_patch ai/outputs/features/20260128_193727/result.md")
        sys.exit(1)

    markdown_file = Path(sys.argv[1])
    if not markdown_file.exists():
        print(f"❌ Fichier non trouvé: {markdown_file}")
        sys.exit(1)

    repo_root = find_repo_root()
    print(f"📁 Repo root: {repo_root}")
    print(f"📄 Parsing: {markdown_file}")

    content = markdown_file.read_text(encoding='utf-8')
    blocks = extract_code_blocks(content)

    if not blocks:
        print("❌ Aucun bloc de code trouvé dans le fichier.")
        print("   Le markdown doit contenir des sections comme:")
        print("   ### Fichier: data/index.html")
        print("   ```html")
        print("   ...")
        print("   ```")
        sys.exit(1)

    print(f"\n🔍 {len(blocks)} bloc(s) de code trouvé(s):")
    for block in blocks:
        print(f"   - {block['path']} ({block['lang']})")

    # Générer les diffs
    all_diffs = []

    for block in blocks:
        file_path = block['path']
        full_path = repo_root / file_path

        if not full_path.exists():
            print(f"\n⚠️  {file_path}: fichier non trouvé, création d'un nouveau fichier")
            # Pour un nouveau fichier, le diff montre tout comme ajouté
            diff = f"diff --git a/{file_path} b/{file_path}\nnew file mode 100644\n--- /dev/null\n+++ b/{file_path}\n"
            lines = block['code'].splitlines()
            diff += f"@@ -0,0 +1,{len(lines)} @@\n"
            for line in lines:
                diff += f"+{line}\n"
            all_diffs.append(diff)
            continue

        original = full_path.read_text(encoding='utf-8', errors='ignore')
        modified = block['code']

        # Nettoyer les fins de ligne
        original = original.replace('\r\n', '\n')
        modified = modified.replace('\r\n', '\n')

        if original.strip() == modified.strip():
            print(f"\n✓  {file_path}: pas de changement")
            continue

        diff = generate_unified_diff(original, modified, file_path)

        if diff:
            # Ajouter le header git
            git_header = f"diff --git a/{file_path} b/{file_path}\n"
            all_diffs.append(git_header + diff)
            print(f"\n✓  {file_path}: diff généré")
        else:
            print(f"\n⚠️  {file_path}: diff vide (fichiers identiques?)")

    if not all_diffs:
        print("\n❌ Aucun diff à générer.")
        sys.exit(1)

    # Sauvegarder le patch
    output_dir = markdown_file.parent
    patch_file = output_dir / "changes.patch"

    combined = '\n'.join(all_diffs)
    patch_file.write_text(combined, encoding='utf-8')

    print(f"\n✅ Patch généré: {patch_file}")
    print(f"\n📋 Pour appliquer:")
    print(f"   git apply {patch_file.relative_to(repo_root)}")
    print(f"\n📋 Pour prévisualiser:")
    print(f"   git apply --stat {patch_file.relative_to(repo_root)}")
    print(f"   git apply --check {patch_file.relative_to(repo_root)}")


if __name__ == "__main__":
    main()
