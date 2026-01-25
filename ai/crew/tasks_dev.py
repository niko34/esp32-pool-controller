from crewai import Task

def dev_implement(dev, ticket_md, design_md, repo_facts):
    return Task(
        description=f"""
Tu es le Dev Agent. Implémente la feature de manière MINIMALE et COHÉRENTE.

TICKET:
{ticket_md}

DESIGN:
{design_md}

INFOS REPO (fichiers/indices trouvés):
{repo_facts}

Contraintes strictes:
- Ne pas inventer de fichiers ou de routes : si nécessaire, utiliser les infos repo_facts.
- Patch minimal (pas de refacto global).
- Produire un UNIFIED DIFF prêt à appliquer.
- Ajouter les validations/erreurs nécessaires.
- Si une partie n’est pas faisable faute de contexte: lister précisément les fichiers à lire et ce qui manque.

Sortie attendue (format obligatoire):
1) Résumé (5-10 lignes)
2) Bloc ```diff ... ```
3) Notes de test (3-8 étapes)
""",
        agent=dev,
        expected_output="Résumé + unified diff + notes de test."
    )
