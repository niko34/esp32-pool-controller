#!/usr/bin/env python3
"""
test_setup.py - Vérifie que l'installation CrewAI est correcte

Usage:
    python -m ai.test_setup
"""

import os
import sys
from pathlib import Path


def check_python_version():
    """Vérifie la version Python."""
    print("🐍 Python version:", sys.version.split()[0])
    if sys.version_info < (3, 10):
        print("   ⚠️ Python 3.10+ recommandé pour CrewAI")
        return False
    print("   ✅ OK")
    return True


def check_dependencies():
    """Vérifie les dépendances."""
    deps = {
        "crewai": "crewai",
        "crewai_tools": "crewai-tools",
        "dotenv": "python-dotenv",
        "anthropic": "anthropic",
    }

    all_ok = True
    print("\n📦 Dépendances:")

    for module, package in deps.items():
        try:
            __import__(module)
            print(f"   ✅ {package}")
        except ImportError:
            print(f"   ❌ {package} - pip install {package}")
            all_ok = False

    return all_ok


def check_api_key():
    """Vérifie la clé API Anthropic."""
    print("\n🔑 Clé API Anthropic:")

    # Charger .env si présent
    env_file = Path(__file__).parent / ".env"
    if env_file.exists():
        from dotenv import load_dotenv
        load_dotenv(env_file)
        print(f"   📄 Chargé depuis {env_file.name}")

    key = os.getenv("ANTHROPIC_API_KEY", "")
    if key:
        masked = key[:10] + "..." + key[-4:] if len(key) > 14 else "***"
        print(f"   ✅ Définie: {masked}")
        return True
    else:
        print("   ❌ Non définie")
        print("   → Créez ai/.env avec: ANTHROPIC_API_KEY=sk-ant-...")
        return False


def check_project_structure():
    """Vérifie la structure du projet."""
    print("\n📁 Structure du projet:")

    repo_root = Path(__file__).parent.parent

    required_files = [
        "platformio.ini",
        "src/main.cpp",
        "data/index.html",
        "README.md",
    ]

    all_ok = True
    for f in required_files:
        path = repo_root / f
        if path.exists():
            print(f"   ✅ {f}")
        else:
            print(f"   ⚠️ {f} (optionnel)")

    return all_ok


def check_tools():
    """Vérifie que les outils fonctionnent."""
    print("\n🔧 Outils CrewAI:")

    try:
        from ai.crew.tools.code_tools import get_all_tools
        tools = get_all_tools()
        print(f"   ✅ {len(tools)} outils chargés:")
        for tool in tools:
            print(f"      - {tool.name}")
        return True
    except Exception as e:
        print(f"   ❌ Erreur: {e}")
        return False


def check_agents():
    """Vérifie que les agents peuvent être créés."""
    print("\n🤖 Agents CrewAI:")

    # Skip si pas de clé API
    if not os.getenv("ANTHROPIC_API_KEY"):
        print("   ⏭️ Ignoré (pas de clé API)")
        return True

    try:
        from ai.crew.agents import create_all_agents
        analyst, developer, reviewer = create_all_agents()
        print(f"   ✅ Analyst: {analyst.role}")
        print(f"   ✅ Developer: {developer.role}")
        print(f"   ✅ Reviewer: {reviewer.role}")
        return True
    except Exception as e:
        print(f"   ❌ Erreur: {e}")
        return False


def main():
    print("=" * 50)
    print("🧪 Test de l'installation CrewAI")
    print("=" * 50)

    results = [
        check_python_version(),
        check_dependencies(),
        check_api_key(),
        check_project_structure(),
        check_tools(),
        check_agents(),
    ]

    print("\n" + "=" * 50)
    if all(results):
        print("✅ Tout est OK! Vous pouvez utiliser le pipeline.")
        print("\nExemple:")
        print('  python -m ai.run_feature "Ajouter un bouton de reset"')
    else:
        print("⚠️ Certaines vérifications ont échoué.")
        print("Corrigez les erreurs ci-dessus avant d'utiliser le pipeline.")
    print("=" * 50)

    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
