"""
tasks.py - Définition des tâches CrewAI

Workflow:
1. spec_feature - Le Spec Analyst définit les User Stories + plan technique
2. implement_feature - Le Developer génère le code
3. review_feature - Le Reviewer valide et documente
4. a11y_audit - L'agent A11y vérifie l'accessibilité (si UI modifiée)
"""

from crewai import Task


def create_spec_task(agent, feature_description: str) -> Task:
    """
    Tâche Spec Analyst: définir les User Stories ET le plan d'implémentation.
    Combine les anciens rôles PO et Analyst en une seule tâche.
    """
    return Task(
        description=f"""
Transforme cette demande en spécification complète: User Stories + plan technique.

## DEMANDE UTILISATEUR
{feature_description}

## TES ACTIONS
1. Comprends le besoin réel derrière la demande
2. Définis les User Stories avec critères d'acceptance testables
3. Utilise `get_project_structure` pour comprendre l'architecture
4. Utilise `search_code` pour trouver les fichiers pertinents
5. Utilise `read_file` pour lire le code existant
6. Produis un plan d'implémentation aligné sur les critères d'acceptance

## SORTIE ATTENDUE

# Spécification: [Titre de la feature]

## 1. User Story

**En tant que** [persona],
**Je veux** [action/fonctionnalité],
**Afin de** [bénéfice/valeur].

### Critères d'Acceptance
- [ ] **CA1**: GIVEN [contexte] WHEN [action] THEN [résultat]
- [ ] **CA2**: [Critère vérifiable]
- [ ] **CA3**: [Critère vérifiable]

### Scénarios de test
1. **Cas nominal**: [Description]
2. **Cas limite**: [Description]
3. **Cas d'erreur**: [Description]

---

## 2. Definition of Done

### Fonctionnel
- [ ] Tous les critères d'acceptance validés
- [ ] Pas de régression mémoire ESP32

### Accessibilité (WCAG 2.1 AA)
- [ ] Navigation clavier (Tab, Entrée, Échap)
- [ ] Focus visible
- [ ] Contraste ≥ 4.5:1
- [ ] Zones tactiles ≥ 44x44px
- [ ] Labels ARIA si éléments sans texte

### Documentation
- [ ] API.md si nouvelle route
- [ ] Code commenté si complexe

---

## 3. Analyse Technique

### Code existant pertinent
- **[fichier.ext]**: [raison de la modification]
- Patterns à suivre: [description]

### Plan d'implémentation

#### Étape 1: [Titre]
- **Fichier:** `chemin/fichier.ext`
- **Modification:** [description précise]
- **Code existant:** [extrait pertinent lu avec read_file]

#### Étape 2: [Titre]
...

---

## 4. Points d'attention
- Risques identifiés
- Questions ouvertes (si besoin de clarification)

## 5. Hors périmètre
- [Ce qui n'est PAS inclus]

IMPORTANT: Utilise TOUJOURS les outils pour lire le code. Ne suppose jamais le contenu d'un fichier.
""",
        agent=agent,
        expected_output="Spécification complète: User Stories + plan d'implémentation détaillé.",
    )


def create_implement_task(agent, feature_description: str, spec_context: str = "") -> Task:
    """
    Tâche d'implémentation: générer le code complet.
    """
    context_section = ""
    if spec_context:
        context_section = f"""
## SPÉCIFICATION (User Stories + Plan technique)
{spec_context}
"""

    return Task(
        description=f"""
Implémente cette feature en générant du code complet et fonctionnel.

## DEMANDE DE FEATURE
{feature_description}

{context_section}

## TES ACTIONS
1. Si la spécification est fournie, suis le plan d'implémentation
2. Utilise `read_file` pour lire SEULEMENT les lignes pertinentes des fichiers à modifier
3. Génère UNIQUEMENT un patch git (pas de fichier complet)
4. Respecte le style de code existant

## SORTIE ATTENDUE

### 1. Résumé
- Fichiers modifiés et description des changements
- Numéros de lignes affectées

### 2. MODIFICATIONS (OBLIGATOIRE - FORMAT JSON)

**IMPORTANT: Fournis les modifications dans un bloc JSON structuré.**

Pour chaque fichier à modifier, utilise ce format EXACT dans un bloc ```json:

```json
{{
  "changes": [
    {{
      "file": "chemin/vers/fichier.ext",
      "description": "Description courte du changement",
      "replacements": [
        {{
          "old": "texte exact à remplacer (copié depuis read_file)",
          "new": "nouveau texte"
        }}
      ]
    }}
  ]
}}
```

**RÈGLES CRITIQUES:**
- Le champ "old" doit contenir le texte EXACT tel qu'il apparaît dans le fichier (copie depuis read_file)
- Inclure suffisamment de contexte dans "old" pour que le texte soit unique dans le fichier
- Un seul bloc ```json avec TOUTES les modifications
- NE PAS inclure le fichier complet
- Chaque "old" doit être trouvable tel quel dans le fichier source

### 3. Test
Comment vérifier que ça fonctionne.

## RÈGLES
- C++: style Arduino/ESP32
- JavaScript: vanilla JS
- NE JAMAIS générer un fichier complet (>100 lignes), uniquement des patches
""",
        agent=agent,
        expected_output="Modifications au format JSON structuré prêtes à être converties en patch.",
    )


def create_review_task(agent, feature_description: str, implementation_context: str = "") -> Task:
    """
    Tâche de review: valider le code et générer tests/docs.
    """
    context_section = ""
    if implementation_context:
        context_section = f"""
## CODE GÉNÉRÉ PAR LE DEVELOPER
{implementation_context}
"""

    return Task(
        description=f"""
Review le code généré et produis des tests et de la documentation.

## DEMANDE DE FEATURE
{feature_description}

{context_section}

## TES ACTIONS
1. Analyse le code généré pour détecter:
   - Bugs potentiels
   - Failles de sécurité
   - Problèmes de performance
   - Incohérences avec le code existant
2. Utilise les outils pour vérifier la cohérence avec le projet
3. Génère des tests recommandés
4. Prépare la documentation à mettre à jour

## SORTIE ATTENDUE

### Validation
- Points positifs du code
- Points d'attention (mineurs)
- Problèmes bloquants (si applicable)

### Sécurité
- Checklist de sécurité

### Tests
- Scénarios de test manuels
- Exemples de tests Playwright si applicable

### Documentation
- Mises à jour README/API.md nécessaires
- Entrée CHANGELOG suggérée

IMPORTANT: Sois constructif. L'objectif est d'améliorer le code, pas de le critiquer.
Note: L'accessibilité est vérifiée par l'agent A11y dédié (si --a11y activé).
""",
        agent=agent,
        expected_output="Review complète avec validation, tests et documentation.",
    )


def create_a11y_task(agent, feature_description: str, implementation_context: str = "") -> Task:
    """
    Tâche d'audit accessibilité: vérifier la conformité WCAG 2.1 AA.
    """
    context_section = ""
    if implementation_context:
        context_section = f"""
## CODE À AUDITER
{implementation_context}
"""

    return Task(
        description=f"""
Audite l'accessibilité des modifications UI proposées.

## DEMANDE DE FEATURE
{feature_description}

{context_section}

## TES ACTIONS
1. Utilise `read_file` pour lire les fichiers HTML/CSS/JS modifiés
2. Identifie tous les éléments UI ajoutés ou modifiés
3. Vérifie chaque critère d'accessibilité applicable
4. Propose des corrections avec du code précis

## CRITÈRES À VÉRIFIER

### 1. Contraste et Couleurs (WCAG 1.4.3, 1.4.11)
- Texte normal: ratio minimum 4.5:1
- Grand texte (≥18pt ou 14pt bold): ratio minimum 3:1
- Éléments UI (icônes, bordures): ratio minimum 3:1
- Ne pas utiliser la couleur seule comme indicateur

### 2. Taille et Lisibilité (WCAG 1.4.4, 1.4.10, 1.4.12)
- Taille de police minimum 16px pour le corps de texte
- Line-height minimum 1.5
- Texte redimensionnable jusqu'à 200% sans perte de fonctionnalité

### 3. Cibles Tactiles (WCAG 2.5.5)
- Boutons et liens: minimum 44x44px
- Espacement suffisant entre éléments cliquables (8px minimum)

### 4. Navigation Clavier (WCAG 2.1.1, 2.1.2, 2.4.7)
- Tous les éléments interactifs accessibles au clavier
- Focus visible sur tous les éléments (outline)
- Ordre de tabulation logique
- Pas de piège clavier

### 5. Structure et Sémantique (WCAG 1.3.1, 4.1.2)
- Balises HTML5 sémantiques (button, nav, main, etc.)
- Labels associés aux inputs (for/id)
- ARIA labels pour les éléments sans texte visible
- Hiérarchie des titres correcte

### 6. Feedback et Erreurs (WCAG 3.3.1, 4.1.3)
- Messages d'erreur clairs et associés (aria-describedby)
- aria-live pour les contenus dynamiques
- États disabled clairement indiqués

## SORTIE ATTENDUE

# Audit Accessibilité: [Feature]

## Résumé
- **Score estimé:** X/10
- **Problèmes critiques:** N
- **Améliorations:** N

## 🔴 Problèmes Critiques

### [Critère WCAG X.X.X] - [Titre]
**Fichier:** `path/file.html` ligne X
**Problème:** [Description]
**Impact:** [Utilisateurs affectés: aveugles, malvoyants, mobilité réduite, etc.]

**Correction (FORMAT JSON):**
```json
{{
  "changes": [
    {{
      "file": "path/file.html",
      "description": "Correction accessibilité: [critère]",
      "replacements": [
        {{
          "old": "code existant exact",
          "new": "code corrigé"
        }}
      ]
    }}
  ]
}}
```

## 🟡 Améliorations Recommandées
[Mêmes détails que ci-dessus]

## ✅ Points Conformes
- [Liste des critères respectés]

## Checklist de Test Manuel
- [ ] Navigation clavier (Tab, Entrée, Échap)
- [ ] Lecteur d'écran (VoiceOver/NVDA)
- [ ] Zoom 200%
- [ ] Contraste (WebAIM Contrast Checker)
- [ ] Mode sombre si applicable
""",
        agent=agent,
        expected_output="Audit A11y complet avec corrections au format JSON.",
    )


def create_quick_task(agent, feature_description: str, task_type: str = "full") -> Task:
    """
    Tâche rapide pour un seul agent (mode simplifié).
    """
    if task_type == "spec":
        return create_spec_task(agent, feature_description)
    elif task_type == "implement":
        return create_implement_task(agent, feature_description)
    elif task_type == "review":
        return create_review_task(agent, feature_description)
    elif task_type == "a11y":
        return create_a11y_task(agent, feature_description)
    else:
        # Mode full: un seul agent fait tout
        return Task(
            description=f"""
Analyse, implémente et documente cette feature.

## DEMANDE
{feature_description}

## TES ACTIONS

1. Utilise `search_code` pour trouver les fichiers pertinents
2. Utilise `read_file` pour lire SEULEMENT les lignes à modifier
3. Génère un PATCH GIT (pas de fichier complet)

## SORTIE ATTENDUE

# Feature: [Titre]

## Analyse
- Fichiers et lignes concernés

## MODIFICATIONS (OBLIGATOIRE - FORMAT JSON)

**IMPORTANT: Fournis les modifications dans un bloc JSON structuré.**

```json
{{
  "changes": [
    {{
      "file": "chemin/vers/fichier.ext",
      "description": "Description du changement",
      "replacements": [
        {{
          "old": "texte exact à remplacer (copié depuis read_file)",
          "new": "nouveau texte"
        }}
      ]
    }}
  ]
}}
```

RÈGLES:
- "old" doit être le texte EXACT du fichier (copie depuis read_file)
- Inclure assez de contexte pour que "old" soit unique
- NE JAMAIS inclure un fichier complet

## Test
Comment vérifier.
""",
            agent=agent,
            expected_output="Patch git unified diff prêt à appliquer.",
        )
