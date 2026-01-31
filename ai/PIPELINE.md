# Pipeline CrewAI - Documentation

## Vue d'ensemble

Ce pipeline utilise CrewAI avec des agents Claude (Anthropic) pour automatiser le développement de features sur le projet ESP32 Pool Controller.

## Architecture des Agents

Le pipeline utilise 4 agents spécialisés :

| Agent | Rôle | Modèle par défaut |
|-------|------|-------------------|
| **Spec Analyst** | Définit les User Stories, critères d'acceptance et plan d'implémentation | claude-sonnet-4 |
| **Developer** | Génère le code (format JSON pour patches) | claude-sonnet-4 |
| **Reviewer** | Valide le code, identifie bugs/sécurité, génère tests et docs | claude-sonnet-4 |
| **A11y** | Audite l'accessibilité WCAG 2.1 AA (optionnel) | claude-haiku-3.5 (économique) |

Les modèles peuvent être overridés via variables d'environnement :
- `SPEC_ANALYST_MODEL`
- `DEVELOPER_MODEL`
- `REVIEWER_MODEL`
- `A11Y_MODEL`

## Modes d'exécution

### Mode `full` (défaut)

Pipeline complet avec 3 agents :

```
Spec Analyst → Developer → Reviewer
```

```bash
python -m ai.run_feature "Description de la feature"
```

### Mode `quick`

Un seul agent (Developer) pour les petites modifications :

```bash
python -m ai.run_feature "Corriger le bug X" --mode quick
```

### Mode `spec`

Seulement la spécification (pas de code) :

```bash
python -m ai.run_feature "Nouvelle feature" --mode spec
```

## Options

### `--a11y`

Ajoute un audit d'accessibilité après la review :

```
Spec Analyst → Developer → Reviewer → A11y
```

```bash
python -m ai.run_feature "Ajouter un bouton" --a11y
```

### `--iterate`

Active les boucles de feedback. Si le Reviewer ou l'A11y trouve des problèmes, le Developer corrige et le cycle recommence.

```bash
python -m ai.run_feature "Feature complexe" --iterate
```

### `--max-iter N`

Limite le nombre d'itérations (défaut: 3) :

```bash
python -m ai.run_feature "Feature" --iterate --max-iter 2
```

## Pipeline détaillé avec `--iterate --a11y`

```
┌─────────────────────────────────────────────────────────────┐
│  1. Spec Analyst                                            │
│     → User Stories + Plan technique                         │
└─────────────────────────┬───────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│  BOUCLE 1: Dev ↔ Review                                     │
│                                                             │
│  2. Developer → 3. Reviewer                                 │
│        ↑              │                                     │
│        └──────────────┘ (si problèmes bloquants)            │
└─────────────────────────┬───────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│  BOUCLE 2: A11y → Dev → Review → A11y                       │
│                                                             │
│  4. A11y ─────────────────────────────────────┐             │
│        │ (si problèmes critiques)             │             │
│        ↓                                      │             │
│  Developer → Reviewer ←─┐                     │             │
│        │         │      │ (boucle interne)    │             │
│        └─────────┘──────┘                     │             │
│        │                                      │             │
│        └──→ A11y (re-vérification) ───────────┘             │
└─────────────────────────────────────────────────────────────┘
```

## Détection des problèmes

### Reviewer - Problèmes bloquants

Le pipeline détecte automatiquement les problèmes bloquants dans la review :
- Section `🔴 Problèmes bloquants`
- Mot-clé `BLOQUANT`
- Vérifie qu'il y a du contenu réel (pas "Aucun" ou "N/A")

### A11y - Problèmes critiques

Le pipeline détecte les problèmes d'accessibilité critiques :
- Section `🔴 Problèmes Critiques`
- Score estimé ≤ 5/10
- Présence de corrections proposées (bloc JSON)

## Fichiers générés

### Structure de sortie

```
ai/outputs/features/YYYYMMDD_HHMMSS/
├── 00_request.txt              # Demande originale
├── 01_spec.md                  # Spécification (User Stories + Plan)
├── 02_implementation.md        # Code généré
├── 02_implementation_v2.md     # (si itération après review)
├── 02_implementation_a11y_v1.md    # (si correction A11y)
├── 03_review.md                # Review du code
├── 03_review_v2.md             # (si itération)
├── 03_review_a11y_v1.md        # (review après correction A11y)
├── 04_a11y.md                  # Audit accessibilité
├── 04_a11y_v2.md               # (re-vérification A11y)
├── changes.patch               # Patch git principal
├── changes_v2.patch            # (si itération)
├── changes_a11y_v1.patch       # (corrections A11y)
├── a11y_fixes.patch            # Suggestions A11y
├── generated.spec.js           # Tests Playwright extraits
└── result.md                   # Résumé combiné
```

### Format des patches

Le Developer génère les modifications au format JSON :

```json
{
  "changes": [
    {
      "file": "data/index.html",
      "description": "Ajout du bouton",
      "replacements": [
        {
          "old": "texte exact existant",
          "new": "nouveau texte"
        }
      ]
    }
  ]
}
```

Le pipeline Python convertit ce JSON en patch git unifié avec `difflib`.

## Règles anti-hallucination

Tous les agents sont configurés avec des règles strictes :

1. **TOUJOURS** utiliser les outils (`read_file`, `search_code`) avant de proposer des modifications
2. **NE JAMAIS** inventer de fichiers, fonctions ou routes qui n'existent pas
3. **NE JAMAIS** supposer le contenu d'un fichier - le lire d'abord
4. Proposer des modifications **MINIMALES** et ciblées

## Critères d'accessibilité (A11y)

L'agent A11y vérifie les critères WCAG 2.1 AA :

| Critère | Exigence |
|---------|----------|
| Contraste texte | ≥ 4.5:1 (normal), ≥ 3:1 (grand texte) |
| Zones tactiles | ≥ 44x44px |
| Navigation clavier | Tab, Entrée, Échap fonctionnels |
| Focus visible | Outline sur éléments interactifs |
| Labels | ARIA pour éléments sans texte |
| Erreurs | `aria-describedby` pour messages |

## Exemples d'utilisation

### Feature simple

```bash
python -m ai.run_feature "Ajouter un bouton de reset sur le dashboard"
```

### Feature avec accessibilité

```bash
python -m ai.run_feature "Ajouter une modale de confirmation" --a11y
```

### Feature complexe avec itérations

```bash
python -m ai.run_feature "Implémenter le graphique de température historique" --a11y --iterate
```

### Analyse seule (pas de code)

```bash
python -m ai.run_feature "Refactorer le système de dosage" --mode spec
```

### Correction rapide

```bash
python -m ai.run_feature "Corriger le texte 'Fonctionnalité à venir'" --mode quick
```

## Configuration

### Fichier `.env`

Créer `ai/.env` :

```env
ANTHROPIC_API_KEY=sk-ant-...

# Optionnel: override des modèles
# SPEC_ANALYST_MODEL=claude-sonnet-4-20250514
# DEVELOPER_MODEL=claude-sonnet-4-20250514
# REVIEWER_MODEL=claude-sonnet-4-20250514
# A11Y_MODEL=claude-sonnet-4-20250514
```

### Installation

```bash
cd ai
python -m venv venv
source venv/bin/activate  # Linux/Mac
pip install -r requirements.txt
```

## Appliquer les patches

Après exécution, appliquer le patch généré :

```bash
# Vérifier le patch
git apply --check ai/outputs/features/YYYYMMDD_HHMMSS/changes.patch

# Appliquer
git apply ai/outputs/features/YYYYMMDD_HHMMSS/changes.patch
```

## Tests Playwright générés

Le Reviewer génère des tests Playwright qui sont automatiquement extraits dans `generated.spec.js`.

### Exécution des tests

```bash
# Installation de Playwright (première fois)
npm install -D @playwright/test
npx playwright install

# Configurer l'URL de l'ESP32
export ESP32_URL=http://192.168.1.100

# Exécuter les tests
npx playwright test tests/generated.spec.js

# Mode interactif (avec navigateur visible)
npx playwright test tests/generated.spec.js --headed

# Debug mode
npx playwright test tests/generated.spec.js --debug
```

### Structure du fichier de test

Le fichier `generated.spec.js` contient :
- Import automatique de `@playwright/test`
- Configuration `BASE_URL` via variable d'environnement `ESP32_URL`
- Hook `beforeEach` pour naviguer vers la page de base
- Tests extraits de la review

### Personnalisation

Si le dossier `tests/` existe à la racine du projet, les tests sont automatiquement copiés dedans. Sinon, ils sont uniquement dans le dossier de sortie.

Pour ajuster les tests :
1. Copier `generated.spec.js` vers `tests/`
2. Modifier selon vos besoins (selectors, assertions, etc.)
3. Renommer le fichier pour éviter l'écrasement lors de la prochaine exécution

## Dépannage

### Erreur "texte non trouvé"

Le `old` dans le JSON ne correspond pas exactement au fichier. Causes possibles :
- Indentation différente
- Fichier modifié depuis la lecture par l'agent
- Agent qui a "halluciné" le contenu

Solution : vérifier manuellement et ajuster le patch.

### Boucle infinie

Si les itérations n'arrivent jamais à satisfaire le Reviewer/A11y, le pipeline s'arrête après `--max-iter` itérations.

### Erreur API

Vérifier :
- La clé `ANTHROPIC_API_KEY` est valide
- Le quota API n'est pas dépassé
- Le venv AI n'est pas actif pendant un `deploy.sh`
