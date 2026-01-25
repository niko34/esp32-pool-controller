from __future__ import annotations

from crewai import Task


# -----------------------------
# 0) PO TRIAGE (drives scope + iterations)
# -----------------------------
def triage_feature(product, feature_text: str, repo_context: str):
    return Task(
        description=f"""
Tu es le Product Owner. Ton objectif est de cadrer la demande de façon exécutable.

DEMANDE:
{feature_text}

CONTEXTE REPO (extraits):
{repo_context}

Ta sortie DOIT être un JSON STRICT (pas de Markdown, pas de texte autour), au format:

{{
  "ticket_id": "FEAT-YYYYMMDD-XXX",
  "areas": ["firmware", "web_ui", "api", "docs", "ci", "tests"], 
  "iterations": 0-5,
  "deliverables": ["ticket","design","tests","a11y","security","dev_patch"],
  "constraints": {{
    "backward_compatible": true/false,
    "breaking_changes_allowed": true/false
  }},
  "definition_of_done": ["...","..."],
  "notes": "..."
}}

Règles:
- areas: liste sans doublon, choisis uniquement ce qui est nécessaire.
- iterations: nombre d'itérations dev attendues (0 si juste étude).
- deliverables: inclure dev_patch uniquement si une modification de code est nécessaire.
""",
        agent=product,
        expected_output="Un JSON strict de triage PO."
    )


# -----------------------------
# 1) Ticket
# -----------------------------
def write_ticket(product, feature_text: str, repo_context: str):
    return Task(
        description=f"""
Tu produis un ticket complet pour cette demande:

DEMANDE:
{feature_text}

CONTEXTE REPO:
{repo_context}

Sortie attendue en Markdown:
- ID (si fourni par le PO) + Titre
- User story (En tant que..., je veux..., afin de...)
- Critères d'acceptation (Given/When/Then)
- Hors périmètre
- Hypothèses
- Risques & impacts (sécurité, UX, perf, données)
- Définition of Done
""",
        agent=product,
        expected_output="Ticket complet au format Markdown."
    )


# -----------------------------
# 2) Design
# -----------------------------
def design_solution(architect, ticket_md: str, repo_context: str):
    return Task(
        description=f"""
À partir du ticket:

{ticket_md}

et du contexte repo:
{repo_context}

Propose un design technique concret:
- API ESP32: routes (path, méthode), auth attendue, payloads JSON
- Modèle de données et validations
- Impacts UI: écrans, composants, états (loading/error/offline)
- Backward compatibility
- Plan de migration config si besoin
- Points de sécurité
- Plan de test technique (très court)

Format: Markdown structuré + exemples JSON.
""",
        agent=architect,
        expected_output="Design technique Markdown + exemples JSON."
    )


# -----------------------------
# 3) Tests
# -----------------------------
def plan_tests(qa, ticket_md: str, design_md: str):
    return Task(
        description=f"""
Basé sur:

TICKET:
{ticket_md}

DESIGN:
{design_md}

Fais:
1) Matrice de tests (table) : cas, type, priorité, attendu
2) Scénarios E2E au format JSON (5 à 10)
3) Non-régression minimum (3 à 5 tests à conserver)
4) Liste des données/fixtures nécessaires (mock API)

Sortie: Markdown + bloc JSON.
""",
        agent=qa,
        expected_output="Plan de tests Markdown + scénarios JSON."
    )


# -----------------------------
# 4) A11y
# -----------------------------
def a11y_review(a11y, ticket_md: str, design_md: str):
    return Task(
        description=f"""
En te basant sur le ticket et le design, donne une checklist a11y spécifique à la feature:
- focus clavier
- contrastes
- taille textes
- messages d'erreur
- zoom
- labels/aria

Propose aussi 3 tests d'accessibilité (axe) à ajouter.

TICKET:
{ticket_md}

DESIGN:
{design_md}

Sortie: Markdown concis.
""",
        agent=a11y,
        expected_output="Checklist accessibilité + tests proposés."
    )


# -----------------------------
# 5) Security
# -----------------------------
def security_review(sec, ticket_md: str, design_md: str):
    return Task(
        description=f"""
Analyse sécurité feature-level.

TICKET:
{ticket_md}

DESIGN:
{design_md}

Donne:
- menaces principales
- endpoints sensibles
- exigences d'auth (READ/WRITE)
- risques XSS/CSRF/injection
- recommandations concrètes (headers, validation, rate-limit, etc.)

Sortie: Markdown concis.
""",
        agent=sec,
        expected_output="Revue sécurité Markdown."
    )


# -----------------------------
# 6) Dev patch (unified diff)
# -----------------------------
def dev_patch(dev, ticket_md: str, design_md: str, repo_context: str, areas: list[str], iteration_index: int, total_iterations: int):
    return Task(
        description=f"""
Tu es le Dev Agent. Tu dois produire un patch concret et appliquable, au format Unified Diff.

Contexte:
- Iteration: {iteration_index}/{total_iterations}
- Zones à modifier (décidées par le PO): {areas}

TICKET:
{ticket_md}

DESIGN:
{design_md}

CONTEXTE REPO (extraits):
{repo_context}

Contraintes:
- Patch au format `diff --git` (unified diff), prêt à être appliqué.
- Modifications MINIMALES et ciblées selon {areas}.
- Si tu n'as pas assez d'infos dans repo_context, fais au mieux sans halluciner des fichiers: 
  - tu peux proposer la création de fichiers si nécessaire,
  - mais tu dois rester cohérent et conservateur.
- Termine par une courte section Markdown "Notes dev" (max 10 lignes): build, tests, points d'attention.

Sortie attendue:
1) Bloc ```diff ... ```
2) Puis "Notes dev".
""",
        agent=dev,
        expected_output="Un patch unified diff + notes dev."
    )