#!/usr/bin/env python3
"""
run_feature.py - Runner principal du pipeline CrewAI

Usage:
    python -m ai.run_feature "Description de la feature"
    python -m ai.run_feature "Description" --mode quick
    python -m ai.run_feature "Description" --mode full
    python -m ai.run_feature "Description" --mode full --a11y

Modes:
    full (défaut): 3 agents (Spec Analyst → Developer → Reviewer)
    quick: 1 seul agent (Developer) pour les petites modifications
    spec: Seulement la spécification (User Stories + plan, pas de code)

Options:
    --a11y: Ajouter un audit d'accessibilité après la review (mode full uniquement)
            Pipeline complet avec A11y: Spec Analyst → Developer → Reviewer → A11y
    --iterate: Boucle de feedback si le Reviewer ou A11y trouve des problèmes
               Relance Developer jusqu'à validation (max 3 itérations par phase)
    --max-iter N: Nombre max d'itérations pour --iterate (défaut: 3)
"""

import os
import re
import sys
import json
import argparse
import difflib
from datetime import datetime
from pathlib import Path

from dotenv import load_dotenv


def setup_paths():
    """Configure les paths pour l'exécution."""
    # Trouver la racine du repo (où se trouve platformio.ini)
    current = Path(__file__).resolve().parent.parent
    if (current / "platformio.ini").exists():
        repo_root = current
    else:
        repo_root = Path.cwd()

    os.chdir(repo_root)

    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))

    return repo_root


def load_environment(repo_root: Path):
    """Charge les variables d'environnement."""
    env_file = repo_root / "ai" / ".env"
    if env_file.exists():
        load_dotenv(env_file)
    else:
        load_dotenv()  # Cherche .env dans le répertoire courant

    # Vérifier la clé API
    if not os.getenv("ANTHROPIC_API_KEY"):
        print("❌ ERREUR: ANTHROPIC_API_KEY non définie")
        print("   Créez un fichier ai/.env avec votre clé API")
        print("   Exemple: ANTHROPIC_API_KEY=sk-ant-...")
        sys.exit(1)


def create_output_dir(repo_root: Path) -> Path:
    """Crée le répertoire de sortie pour cette exécution."""
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = repo_root / "ai" / "outputs" / "features" / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir


def save_output(output_dir: Path, filename: str, content: str):
    """Sauvegarde un fichier de sortie."""
    filepath = output_dir / filename
    filepath.write_text(content, encoding="utf-8")
    print(f"   📄 {filepath.relative_to(output_dir.parent.parent.parent)}")


def find_repo_root() -> Path:
    """Trouve la racine du repo."""
    current = Path(__file__).resolve().parent.parent
    if (current / "platformio.ini").exists():
        return current
    return Path.cwd()


def generate_unified_diff(original: str, modified: str, file_path: str) -> str:
    """Génère un unified diff entre deux contenus."""
    original_lines = original.splitlines(keepends=False)
    modified_lines = modified.splitlines(keepends=False)

    diff = difflib.unified_diff(
        original_lines,
        modified_lines,
        fromfile=f'a/{file_path}',
        tofile=f'b/{file_path}',
        lineterm=''
    )

    return '\n'.join(diff) + '\n'


def extract_json_changes(content: str) -> dict | None:
    """Extrait le bloc JSON des modifications depuis le markdown."""
    # Chercher le bloc ```json ... ```
    json_pattern = re.compile(r'```json\s*\n(.*?)\n```', re.DOTALL)
    match = json_pattern.search(content)

    if not match:
        return None

    try:
        return json.loads(match.group(1))
    except json.JSONDecodeError as e:
        print(f"   ⚠️  Erreur parsing JSON: {e}")
        return None


def find_text_flexible(content: str, search_text: str) -> tuple[int, int] | None:
    """
    Trouve un texte dans le contenu avec une correspondance flexible sur les espaces.
    Retourne (start, end) ou None si non trouvé.
    """
    # 1. Essai exact
    idx = content.find(search_text)
    if idx != -1:
        return (idx, idx + len(search_text))

    # 2. Normaliser les retours à la ligne
    search_normalized = search_text.replace('\r\n', '\n')
    if search_normalized != search_text:
        idx = content.find(search_normalized)
        if idx != -1:
            return (idx, idx + len(search_normalized))

    # 3. Recherche ligne par ligne avec flexibilité sur l'indentation
    search_lines = search_text.split('\n')
    if len(search_lines) < 2:
        return None

    # Créer un pattern regex qui tolère les variations d'espaces en début de ligne
    pattern_parts = []
    for line in search_lines:
        stripped = line.strip()
        if stripped:
            # Échapper les caractères regex et autoriser des espaces flexibles en début
            escaped = re.escape(stripped)
            pattern_parts.append(r'[ \t]*' + escaped)
        else:
            # Ligne vide - autoriser des espaces optionnels
            pattern_parts.append(r'[ \t]*')

    pattern = r'\n'.join(pattern_parts)

    try:
        match = re.search(pattern, content)
        if match:
            return (match.start(), match.end())
    except re.error:
        pass

    return None


def extract_and_save_patch(output_dir: Path, content: str, patch_filename: str = "changes.patch") -> bool:
    """
    Extrait les modifications JSON et génère un patch git valide.
    Retourne True si un patch a été généré.
    """
    repo_root = find_repo_root()

    # Essayer d'abord le nouveau format JSON
    json_data = extract_json_changes(content)

    if json_data and "changes" in json_data:
        all_diffs = []
        errors = []

        for change in json_data["changes"]:
            file_path = change.get("file", "")
            replacements = change.get("replacements", [])

            if not file_path or not replacements:
                continue

            full_path = repo_root / file_path

            if not full_path.exists():
                errors.append(f"{file_path}: fichier non trouvé")
                continue

            # Lire le fichier original
            try:
                original = full_path.read_text(encoding='utf-8')
            except Exception as e:
                errors.append(f"{file_path}: erreur lecture - {e}")
                continue

            # Appliquer les remplacements
            modified = original
            replacement_errors = []

            for repl in replacements:
                old_text = repl.get("old", "")
                new_text = repl.get("new", "")

                if not old_text:
                    continue

                # Utiliser la recherche flexible
                match_result = find_text_flexible(modified, old_text)

                if match_result is None:
                    replacement_errors.append(f"Texte non trouvé: {old_text[:50]}...")
                    continue

                start, end = match_result
                actual_old = modified[start:end]

                # Ajuster new_text pour conserver l'indentation originale si possible
                if actual_old != old_text:
                    # L'indentation a été ajustée - adapter new_text
                    old_lines = old_text.split('\n')
                    new_lines = new_text.split('\n')
                    actual_lines = actual_old.split('\n')

                    if len(old_lines) == len(actual_lines) == len(new_lines):
                        adjusted_new_lines = []
                        for i, (old_l, new_l, actual_l) in enumerate(zip(old_lines, new_lines, actual_lines)):
                            # Calculer la différence d'indentation
                            old_indent = len(old_l) - len(old_l.lstrip())
                            actual_indent = len(actual_l) - len(actual_l.lstrip())
                            indent_diff = actual_indent - old_indent

                            new_stripped = new_l.lstrip()
                            new_indent = len(new_l) - len(new_stripped)
                            adjusted_indent = max(0, new_indent + indent_diff)
                            adjusted_new_lines.append(' ' * adjusted_indent + new_stripped)

                        new_text = '\n'.join(adjusted_new_lines)

                modified = modified[:start] + new_text + modified[end:]

            if replacement_errors:
                for err in replacement_errors:
                    errors.append(f"{file_path}: {err}")

            # Générer le diff si des modifications ont été faites
            if modified != original:
                diff = generate_unified_diff(original, modified, file_path)
                if diff:
                    git_header = f"diff --git a/{file_path} b/{file_path}\n"
                    all_diffs.append(git_header + diff)
                    print(f"   ✓  {file_path}: patch généré")

        # Afficher les erreurs
        for err in errors:
            print(f"   ⚠️  {err}")

        if all_diffs:
            combined = '\n'.join(all_diffs)
            patch_file = output_dir / patch_filename
            patch_file.write_text(combined, encoding="utf-8")
            print(f"   🔧 {patch_file.relative_to(output_dir.parent.parent.parent)}")
            print(f"      → Appliquer avec: git apply {patch_file.relative_to(output_dir.parent.parent.parent.parent)}")
            return True

    # Fallback: ancien format diff (pour compatibilité)
    diff_pattern = re.compile(r'```diff\s*\n(.*?)\n```', re.DOTALL)
    matches = diff_pattern.findall(content)

    if matches:
        all_patches = []
        for match in matches:
            patch_content = match.strip()
            if patch_content and ('diff --git' in patch_content or '---' in patch_content):
                all_patches.append(patch_content)

        if all_patches:
            combined_patch = '\n\n'.join(all_patches)
            patch_file = output_dir / patch_filename
            patch_file.write_text(combined_patch + '\n', encoding="utf-8")
            print(f"   🔧 {patch_file.relative_to(output_dir.parent.parent.parent)} (format diff legacy)")
            print(f"      ⚠️  Patch généré par l'agent (peut nécessiter corrections manuelles)")
            return True

    return False


def extract_task_output(task) -> str:
    """Extrait le texte de sortie d'une tâche CrewAI."""
    try:
        return task.output.raw
    except AttributeError:
        try:
            return str(task.output)
        except Exception:
            return ""


def has_blocking_issues(review_output: str) -> bool:
    """
    Détecte si la review contient des problèmes bloquants.
    Retourne True si des problèmes bloquants sont identifiés.
    """
    # Chercher la section "Problèmes bloquants"
    blocking_patterns = [
        r'🔴\s*Problèmes?\s*bloquants?',
        r'##.*Problèmes?\s*bloquants?',
        r'\*\*Problèmes?\s*bloquants?\*\*',
        r'BLOQUANT',
    ]

    for pattern in blocking_patterns:
        match = re.search(pattern, review_output, re.IGNORECASE)
        if match:
            # Vérifier qu'il y a du contenu après (pas juste "Aucun" ou "N/A")
            after_match = review_output[match.end():match.end() + 200]
            if re.search(r'(aucun|none|n/a|pas de|néant|\(si applicable\))', after_match, re.IGNORECASE):
                continue
            # Vérifier qu'il y a une vraie liste de problèmes
            if re.search(r'[-•]\s*\[?\w', after_match):
                return True

    return False


def extract_blocking_issues(review_output: str) -> str:
    """
    Extrait la description des problèmes bloquants pour les passer au Developer.
    """
    # Chercher la section problèmes bloquants
    match = re.search(
        r'(🔴.*?Problèmes?\s*bloquants?.*?)(?=\n##|\n#\s|\Z)',
        review_output,
        re.IGNORECASE | re.DOTALL
    )

    if match:
        return match.group(1).strip()

    return ""


def has_a11y_critical_issues(a11y_output: str) -> bool:
    """
    Détecte si l'audit A11y contient des problèmes critiques.
    Retourne True si des corrections sont nécessaires.
    """
    # Chercher la section "Problèmes Critiques" de l'audit A11y
    critical_patterns = [
        r'🔴\s*Problèmes?\s*Critiques?',
        r'##.*Problèmes?\s*Critiques?',
        r'\*\*Problèmes?\s*Critiques?\*\*',
        r'Score estimé[:\s]*[0-5]/10',  # Score faible
    ]

    for pattern in critical_patterns:
        match = re.search(pattern, a11y_output, re.IGNORECASE)
        if match:
            # Vérifier qu'il y a du contenu réel
            after_match = a11y_output[match.end():match.end() + 300]
            if re.search(r'(aucun|none|n/a|pas de|néant)', after_match, re.IGNORECASE):
                continue
            # Vérifier qu'il y a des corrections proposées
            if re.search(r'(Correction|```json|"changes")', after_match, re.IGNORECASE):
                return True

    return False


def extract_a11y_issues(a11y_output: str) -> str:
    """
    Extrait la description des problèmes A11y critiques pour les passer au Developer.
    """
    # Chercher la section problèmes critiques
    match = re.search(
        r'(🔴.*?Problèmes?\s*Critiques?.*?)(?=\n##\s*🟡|\n##\s*✅|\n#\s|\Z)',
        a11y_output,
        re.IGNORECASE | re.DOTALL
    )

    if match:
        return match.group(1).strip()

    return ""


def extract_playwright_tests(review_output: str) -> str | None:
    """
    Extrait les tests Playwright depuis la review.
    Retourne le code JavaScript des tests ou None si non trouvé.
    """
    # Chercher les blocs de code JavaScript contenant des tests Playwright
    test_blocks = []

    # Pattern pour les blocs ```javascript avec des tests
    js_pattern = re.compile(r'```(?:javascript|js)\s*\n(.*?)```', re.DOTALL)

    for match in js_pattern.finditer(review_output):
        code = match.group(1).strip()
        # Vérifier que c'est un test Playwright (contient test() ou describe())
        if re.search(r'\b(test|describe)\s*\(', code):
            test_blocks.append(code)

    if not test_blocks:
        return None

    # Assembler les tests avec les imports nécessaires
    header = """// Tests générés automatiquement par le pipeline CrewAI
// Date: {date}
//
// Pour exécuter ces tests:
//   npx playwright test tests/generated.spec.js
//
// Prérequis:
//   npm install -D @playwright/test
//   npx playwright install

import {{ test, expect }} from '@playwright/test';

// Configuration de base pour l'ESP32
const BASE_URL = process.env.ESP32_URL || 'http://192.168.1.100';

test.beforeEach(async ({{ page }}) => {{
  await page.goto(BASE_URL);
}});

""".format(date=datetime.now().strftime("%Y-%m-%d %H:%M"))

    # Nettoyer les tests (retirer les imports dupliqués si présents)
    cleaned_tests = []
    for block in test_blocks:
        # Retirer les lignes d'import déjà présentes
        lines = block.split('\n')
        filtered_lines = [
            line for line in lines
            if not line.strip().startswith('import ')
            and not line.strip().startswith('const BASE_URL')
            and not line.strip().startswith('test.beforeEach')
        ]
        cleaned = '\n'.join(filtered_lines).strip()
        if cleaned:
            cleaned_tests.append(cleaned)

    if not cleaned_tests:
        return None

    return header + '\n\n'.join(cleaned_tests) + '\n'


def save_playwright_tests(output_dir: Path, review_output: str) -> bool:
    """
    Extrait et sauvegarde les tests Playwright depuis la review.
    Retourne True si des tests ont été sauvegardés.
    """
    tests = extract_playwright_tests(review_output)

    if not tests:
        return False

    # Sauvegarder dans le dossier de sortie
    test_file = output_dir / "generated.spec.js"
    test_file.write_text(tests, encoding="utf-8")
    print(f"   🧪 {test_file.relative_to(output_dir.parent.parent.parent)}")

    # Aussi créer dans tests/ si le dossier existe
    repo_root = find_repo_root()
    tests_dir = repo_root / "tests"

    if tests_dir.exists():
        dest_file = tests_dir / "generated.spec.js"
        dest_file.write_text(tests, encoding="utf-8")
        print(f"   🧪 {dest_file.relative_to(repo_root)} (copié dans tests/)")
        print(f"      → Exécuter avec: npx playwright test tests/generated.spec.js")

    return True


def run_full_pipeline(
    feature: str,
    output_dir: Path,
    with_a11y: bool = False,
    iterate: bool = False,
    max_iterations: int = 3,
):
    """
    Exécute le pipeline complet: Spec Analyst → Developer → Reviewer [→ A11y]
    Si iterate=True, boucle Developer → Reviewer jusqu'à validation.
    """
    from crewai import Crew

    from ai.crew.agents import create_all_agents
    from ai.crew.tasks import (
        create_spec_task,
        create_implement_task,
        create_review_task,
        create_a11y_task,
    )

    agents_desc = "Spec Analyst → Developer → Reviewer"
    if with_a11y:
        agents_desc += " → A11y"
    print(f"\n🚀 Mode FULL: {agents_desc}")
    print("=" * 60)

    # Créer les agents (4 agents: spec_analyst, developer, reviewer, a11y)
    spec_analyst, developer, reviewer, a11y = create_all_agents()

    # Phase 1: Spécification (User Stories + Plan technique)
    print("\n📋 Phase 1: Spécification (User Stories + Plan technique)...")
    task_spec = create_spec_task(spec_analyst, feature)
    crew_spec = Crew(
        agents=[spec_analyst],
        tasks=[task_spec],
        verbose=True,
    )

    try:
        result_spec = crew_spec.kickoff()
        spec_output = str(result_spec)
        save_output(output_dir, "01_spec.md", spec_output)
    except Exception as e:
        save_output(output_dir, "error_spec.txt", str(e))
        raise

    # Phase 2 & 3: Implémentation + Review (avec boucle de feedback si iterate=True)
    iteration = 1
    review_feedback = ""
    implementation_output = ""
    review_output = ""

    while iteration <= max_iterations:
        # Phase 2: Implémentation
        if iteration == 1:
            print("\n💻 Phase 2: Implémentation...")
            context_for_dev = spec_output
        else:
            print(f"\n🔄 Itération {iteration}: Correction des problèmes...")
            # Ajouter le feedback du Reviewer au contexte
            context_for_dev = f"""{spec_output}

---

## FEEDBACK DU REVIEWER (Itération {iteration - 1})

{review_feedback}

IMPORTANT: Corrige les problèmes bloquants identifiés ci-dessus.
"""

        task_implement = create_implement_task(developer, feature, context_for_dev)
        crew_implement = Crew(
            agents=[developer],
            tasks=[task_implement],
            verbose=True,
        )

        try:
            result_implement = crew_implement.kickoff()
            implementation_output = str(result_implement)
            suffix = "" if iteration == 1 else f"_v{iteration}"
            save_output(output_dir, f"02_implementation{suffix}.md", implementation_output)
            patch_name = "changes.patch" if iteration == 1 else f"changes_v{iteration}.patch"
            extract_and_save_patch(output_dir, implementation_output, patch_name)
        except Exception as e:
            save_output(output_dir, f"error_implementation_v{iteration}.txt", str(e))
            raise

        # Phase 3: Review
        print("\n🔍 Phase 3: Review...")
        task_review = create_review_task(reviewer, feature, implementation_output)
        crew_review = Crew(
            agents=[reviewer],
            tasks=[task_review],
            verbose=True,
        )

        try:
            result_review = crew_review.kickoff()
            review_output = str(result_review)
            suffix = "" if iteration == 1 else f"_v{iteration}"
            save_output(output_dir, f"03_review{suffix}.md", review_output)
            # Extraire et sauvegarder les tests Playwright
            save_playwright_tests(output_dir, review_output)
        except Exception as e:
            save_output(output_dir, f"error_review_v{iteration}.txt", str(e))
            raise

        # Vérifier s'il faut itérer
        if not iterate:
            break

        if has_blocking_issues(review_output):
            review_feedback = extract_blocking_issues(review_output)
            if review_feedback and iteration < max_iterations:
                print(f"\n⚠️  Problèmes bloquants détectés, relance de l'implémentation...")
                iteration += 1
                continue
            elif iteration >= max_iterations:
                print(f"\n⚠️  Max itérations ({max_iterations}) atteint, arrêt de la boucle")
                break
        else:
            print("\n✅ Aucun problème bloquant, review validée!")
            break

        iteration += 1

    # Phase 4: A11y (optionnelle, avec boucle de feedback si iterate=True)
    # Pipeline rigoureux: A11y → Developer → Reviewer → A11y (re-vérif)
    a11y_output = ""
    if with_a11y:
        a11y_iteration = 1
        current_impl = implementation_output

        while a11y_iteration <= max_iterations:
            # Audit A11y
            if a11y_iteration == 1:
                print("\n♿ Phase 4: Audit Accessibilité...")
            else:
                print(f"\n🔄 A11y Itération {a11y_iteration}: Re-vérification après corrections...")

            task_a11y = create_a11y_task(a11y, feature, current_impl)
            crew_a11y = Crew(
                agents=[a11y],
                tasks=[task_a11y],
                verbose=True,
            )

            try:
                result_a11y = crew_a11y.kickoff()
                a11y_output = str(result_a11y)
                suffix = "" if a11y_iteration == 1 else f"_v{a11y_iteration}"
                save_output(output_dir, f"04_a11y{suffix}.md", a11y_output)
                patch_name = "a11y_fixes.patch" if a11y_iteration == 1 else f"a11y_fixes_v{a11y_iteration}.patch"
                extract_and_save_patch(output_dir, a11y_output, patch_name)
            except Exception as e:
                save_output(output_dir, f"error_a11y_v{a11y_iteration}.txt", str(e))
                raise

            # Vérifier s'il faut itérer
            if not iterate:
                break

            if has_a11y_critical_issues(a11y_output):
                a11y_feedback = extract_a11y_issues(a11y_output)
                if a11y_feedback and a11y_iteration < max_iterations:
                    print(f"\n⚠️  Problèmes A11y critiques détectés...")

                    # Étape 1: Developer corrige
                    print(f"\n💻 Developer: Correction des problèmes A11y...")
                    context_for_dev = f"""{spec_output}

---

## FEEDBACK A11Y (Itération {a11y_iteration})

{a11y_feedback}

IMPORTANT: Corrige les problèmes d'accessibilité critiques identifiés ci-dessus.
"""
                    task_implement = create_implement_task(developer, feature, context_for_dev)
                    crew_implement = Crew(
                        agents=[developer],
                        tasks=[task_implement],
                        verbose=True,
                    )

                    try:
                        result_implement = crew_implement.kickoff()
                        current_impl = str(result_implement)
                        save_output(output_dir, f"02_implementation_a11y_v{a11y_iteration}.md", current_impl)
                        extract_and_save_patch(output_dir, current_impl, f"changes_a11y_v{a11y_iteration}.patch")
                    except Exception as e:
                        save_output(output_dir, f"error_implementation_a11y_v{a11y_iteration}.txt", str(e))
                        raise

                    # Étape 2: Reviewer valide les corrections
                    print(f"\n🔍 Reviewer: Validation des corrections A11y...")
                    task_review = create_review_task(reviewer, feature, current_impl)
                    crew_review = Crew(
                        agents=[reviewer],
                        tasks=[task_review],
                        verbose=True,
                    )

                    try:
                        result_review = crew_review.kickoff()
                        current_review = str(result_review)
                        save_output(output_dir, f"03_review_a11y_v{a11y_iteration}.md", current_review)
                        save_playwright_tests(output_dir, current_review)
                    except Exception as e:
                        save_output(output_dir, f"error_review_a11y_v{a11y_iteration}.txt", str(e))
                        raise

                    # Si le Reviewer trouve des problèmes bloquants, boucler Dev→Review
                    review_sub_iter = 1
                    while iterate and has_blocking_issues(current_review) and review_sub_iter < max_iterations:
                        print(f"\n⚠️  Reviewer: problèmes détectés, correction...")
                        review_feedback = extract_blocking_issues(current_review)

                        context_for_dev = f"""{spec_output}

---

## FEEDBACK REVIEWER (après correction A11y, itération {review_sub_iter})

{review_feedback}

IMPORTANT: Corrige les problèmes bloquants identifiés ci-dessus.
"""
                        task_implement = create_implement_task(developer, feature, context_for_dev)
                        crew_implement = Crew(
                            agents=[developer],
                            tasks=[task_implement],
                            verbose=True,
                        )

                        try:
                            result_implement = crew_implement.kickoff()
                            current_impl = str(result_implement)
                            save_output(output_dir, f"02_implementation_a11y_v{a11y_iteration}_fix{review_sub_iter}.md", current_impl)
                            extract_and_save_patch(output_dir, current_impl, f"changes_a11y_v{a11y_iteration}_fix{review_sub_iter}.patch")
                        except Exception as e:
                            save_output(output_dir, f"error_implementation_a11y_v{a11y_iteration}_fix{review_sub_iter}.txt", str(e))
                            raise

                        # Re-review
                        task_review = create_review_task(reviewer, feature, current_impl)
                        crew_review = Crew(
                            agents=[reviewer],
                            tasks=[task_review],
                            verbose=True,
                        )

                        try:
                            result_review = crew_review.kickoff()
                            current_review = str(result_review)
                            save_output(output_dir, f"03_review_a11y_v{a11y_iteration}_fix{review_sub_iter}.md", current_review)
                            save_playwright_tests(output_dir, current_review)
                        except Exception as e:
                            save_output(output_dir, f"error_review_a11y_v{a11y_iteration}_fix{review_sub_iter}.txt", str(e))
                            raise

                        review_sub_iter += 1

                    if not has_blocking_issues(current_review):
                        print(f"\n✅ Reviewer: corrections validées")

                    # Mettre à jour les outputs finaux
                    implementation_output = current_impl
                    review_output = current_review

                    a11y_iteration += 1
                    continue
                elif a11y_iteration >= max_iterations:
                    print(f"\n⚠️  Max itérations A11y ({max_iterations}) atteint, arrêt de la boucle")
                    break
            else:
                print("\n✅ Aucun problème A11y critique, audit validé!")
                break

            a11y_iteration += 1

    # Résumé combiné
    num_agents = "4 agents" if with_a11y else "3 agents"
    combined = f"""# Feature: {feature}

Date: {datetime.now().strftime("%Y-%m-%d %H:%M")}
Mode: full ({num_agents})

---

# 1. Spécification (User Stories + Plan technique)

{spec_output}

---

# 2. Implémentation

{implementation_output}

---

# 3. Review

{review_output}
"""

    if with_a11y and a11y_output:
        combined += f"""
---

# 4. Audit Accessibilité

{a11y_output}
"""

    save_output(output_dir, "result.md", combined)

    return combined


def run_quick_pipeline(feature: str, output_dir: Path):
    """
    Mode rapide: un seul agent (Developer) fait tout.
    """
    from crewai import Crew

    from ai.crew.agents import create_developer, build_llms
    from ai.crew.tasks import create_quick_task

    print("\n⚡ Mode QUICK: 1 agent (Developer)")
    print("=" * 60)

    _, llm_developer, _, _ = build_llms()
    developer = create_developer(llm_developer)

    task = create_quick_task(developer, feature, "full")
    crew = Crew(
        agents=[developer],
        tasks=[task],
        verbose=True,
    )

    try:
        result = crew.kickoff()
        output = str(result)
        save_output(output_dir, "result.md", output)
        extract_and_save_patch(output_dir, output)
        return output
    except Exception as e:
        save_output(output_dir, "error.txt", str(e))
        raise


def run_spec_only(feature: str, output_dir: Path):
    """
    Mode spécification seulement: User Stories + plan, pas de code.
    """
    from crewai import Crew

    from ai.crew.agents import create_spec_analyst, build_llms
    from ai.crew.tasks import create_spec_task

    print("\n🔎 Mode SPEC: spécification seulement (pas de code)")
    print("=" * 60)

    llm_spec_analyst, _, _, _ = build_llms()
    spec_analyst = create_spec_analyst(llm_spec_analyst)

    task = create_spec_task(spec_analyst, feature)
    crew = Crew(
        agents=[spec_analyst],
        tasks=[task],
        verbose=True,
    )

    try:
        result = crew.kickoff()
        output = str(result)
        save_output(output_dir, "spec.md", output)
        return output
    except Exception as e:
        save_output(output_dir, "error.txt", str(e))
        raise


def main():
    parser = argparse.ArgumentParser(
        description="Pipeline CrewAI pour développement de features",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Exemples:
    python -m ai.run_feature "Ajouter un graphe de température sur le dashboard"
    python -m ai.run_feature "Corriger le bug d'affichage pH" --mode quick
    python -m ai.run_feature "Préparer la spec pour l'architecture MQTT" --mode spec
    python -m ai.run_feature "Nouvelle feature" --iterate --max-iter 2
        """,
    )

    parser.add_argument(
        "feature",
        type=str,
        help="Description de la feature à implémenter",
    )

    parser.add_argument(
        "--mode",
        type=str,
        choices=["full", "quick", "spec"],
        default="full",
        help="Mode d'exécution (défaut: full)",
    )

    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Afficher plus de détails",
    )

    parser.add_argument(
        "--a11y",
        action="store_true",
        help="Ajouter un audit d'accessibilité (mode full uniquement)",
    )

    parser.add_argument(
        "--iterate",
        action="store_true",
        help="Boucle de feedback: relance Developer si Reviewer trouve des problèmes",
    )

    parser.add_argument(
        "--max-iter",
        type=int,
        default=3,
        help="Nombre max d'itérations pour --iterate (défaut: 3)",
    )

    args = parser.parse_args()

    # Setup
    repo_root = setup_paths()
    load_environment(repo_root)
    output_dir = create_output_dir(repo_root)

    print(f"\n{'='*60}")
    print("🤖 CrewAI Feature Pipeline")
    print(f"{'='*60}")
    print(f"\n📝 Feature: {args.feature}")
    print(f"📂 Output: {output_dir.relative_to(repo_root)}")

    # Sauvegarder la demande
    save_output(output_dir, "00_request.txt", args.feature)

    try:
        if args.mode == "full":
            result = run_full_pipeline(
                args.feature,
                output_dir,
                with_a11y=args.a11y,
                iterate=args.iterate,
                max_iterations=args.max_iter,
            )
        elif args.mode == "quick":
            result = run_quick_pipeline(args.feature, output_dir)
        elif args.mode == "spec":
            result = run_spec_only(args.feature, output_dir)

        print(f"\n{'='*60}")
        print("✅ Pipeline terminé avec succès!")
        print(f"📂 Résultats: {output_dir.relative_to(repo_root)}")
        print(f"{'='*60}\n")

    except KeyboardInterrupt:
        print("\n\n⚠️ Interrompu par l'utilisateur")
        sys.exit(1)

    except Exception as e:
        print(f"\n❌ Erreur: {e}")
        print(f"📂 Logs d'erreur: {output_dir.relative_to(repo_root)}")
        sys.exit(1)


if __name__ == "__main__":
    main()
