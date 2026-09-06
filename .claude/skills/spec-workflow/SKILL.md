---
name: spec-workflow
description: Cycle de vie d'une spec dans ESP32 Pool Controller (todo → doing → done), format à respecter, règles de promotion entre dossiers. À charger dès qu'une spec est créée, reprise ou clôturée.
---

# Spec workflow — ESP32 Pool Controller

## Arborescence

```
specs/
  features/
    _template.md        # Template UNIQUE, obligatoire pour toute spec — NE PAS modifier sans accord
    todo/               # Specs à traiter, priorité par nom alphabétique ou horodatage
    doing/              # Spec en cours (idéalement une seule)
    done/               # Specs terminées et validées
```

## Cycle de vie

```
features/todo/X.md ──(démarrage par orchestrator)──> features/doing/X.md ──(validation QA + revue)──> features/done/X.md
```

- Une spec = un cycle. Pas de spec « en pause » dans `doing/` pendant qu'on en fait une autre.
- Si un imprévu force à suspendre : renvoyer dans `features/todo/` avec note explicite en tête.
- Pas de création de spec directement dans `features/doing/`.

## Création d'une spec

### Feature (obligatoire : template riche)

1. Copier `specs/features/_template.md` vers `specs/features/todo/feature-NNN-<kebab-case-slug>.md` (NNN = prochain numéro dispo)
2. Remplir **tous les blocs obligatoires** :
   - Métadonnées (ID, slug, priorité, version cible)
   - Domaines touchés (checklist)
   - Agents requis (checklist — cocher `pool-chemistry` si dosage/régulation/calibration)
   - Description, Contexte, Comportement attendu, Comportement actuel
   - Contraintes techniques, Fichiers potentiellement concernés
   - Sécurité chimique (si applicable)
   - Éléments visibles (pour doc-writer)
   - Critères d'acceptation + Plan de test manuel
3. Supprimer les sections optionnelles non pertinentes plutôt que les laisser vides
4. Ne pas ajouter de sections « Notes techniques », « Statut » — elles seront ajoutées en cours de route

### Bug simple ou refactor ponctuel (template allégé)

1. Copier `specs/features/_template.md` vers `specs/features/todo/bug-NNN-<slug>.md` (ou `refactor-NNN-<slug>.md`) et supprimer les sections optionnelles non pertinentes — il n'existe pas de template minimal séparé (décision du 2026-09-06)
2. Remplir type, description, contexte, comportement attendu/actuel, steps to reproduce (si bug), critères d'acceptation
3. Si le bug touche la régulation/le dosage/la calibration : basculer sur le template feature complet pour que `pool-chemistry` soit déclenché

## Promotion todo → doing

- Faite par `spec-reader` après analyse d'impact complète
- Si spec incomplète : ne PAS la promouvoir, poser les questions à l'utilisateur

```bash
mv specs/features/todo/<nom>.md specs/features/doing/<nom>.md
```

## Enrichissement pendant doing

Ajouter **en fin de fichier** (pas en remplacement) une section :

```markdown
---
# Notes d'implémentation

## Plan (architect)
...

## Validation pool-chemistry (si applicable)
Verdict : GO / GO SOUS CONDITIONS / NO GO
...

## Tests (test-engineer)
- pio run : ✅
- Tests ajoutés : ...

## Revue (code-reviewer)
Verdict : Approved / Request changes
...
```

## Promotion doing → done

Exigences avant promotion :
- [ ] `pio run` vert
- [ ] Tests ajoutés / passants
- [ ] Rapport `code-reviewer` Approved
- [ ] `pool-chemistry` GO si applicable
- [ ] `CHANGELOG.md` mis à jour (si visible utilisateur)
- [ ] Documentation à jour (si API/UX/MQTT change)

```bash
mv specs/features/doing/<nom>.md specs/features/done/<nom>.md
```

En fin de fichier, ajouter :
```markdown
## Statut final
✅ Done — <date> — commit <sha>
```

## Règles

- **Une spec = une unité** : si trop gros, découper en sous-specs avant de démarrer
- **Source de vérité** : pas de travail sans spec. Si demande verbale → créer une spec d'abord
- **Immuable après done** : ne pas modifier une spec `done/` ; créer une nouvelle spec pour toute évolution
- **Historique** : l'ordre des fichiers `done/` raconte l'histoire du projet
