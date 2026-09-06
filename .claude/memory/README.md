# Mémoire de projet — ce que l'expérience a appris

Ces fichiers sont la mémoire accumulée par Claude Code au fil des mois de travail
sur ce contrôleur : pièges rencontrés, hypothèses **réfutées**, incidents, et
principes que l'utilisateur a dû rappeler plusieurs fois.

Ils vivaient dans `~/.claude/projects/<projet>/memory/` — **hors du dépôt, donc
sur une seule machine et sans sauvegarde**. Ils sont versionnés ici depuis le
**06/09/2026**, à l'occasion d'une bascule de poste de travail.

`MEMORY.md` est l'index : une ligne par fichier.

## Pourquoi ça compte

Ce ne sont pas des notes de confort. Plusieurs y figurent parce que **l'erreur a
été commise puis répétée** :

- **« L'offset pH n'est PAS une suspicion de sonde »** — principe rappelé **trois
  fois** par l'utilisateur. L'offset corrige un décalage de *mesure* (courants de
  fuite) ; la sonde est prouvée saine.
- **Parité `/data` ↔ WebSocket** — **quatre récidives** du même défaut. Un champ
  servi par le WebSocket seul ne disparaît pas : il **clignote**, parce que
  `latestSensorData` est remplacé par les charges `/data` toutes les 30 s.
- **« Config d'usine ≠ config vivante »** — un diagnostic entier (limite
  d'actionneur) a été faussé pour avoir raisonné sur la valeur par défaut de
  5 min/h alors que l'appareil était réglé à 25. **Lire `/get-config` avant tout
  diagnostic chiffré.**
- **La métrique heap du projet était fausse** — `ESP.getFreeHeap()` mesure
  `MALLOC_CAP_INTERNAL`, IRAM 32 bits comprise, alors que la famine est en 8 bits.
  Toutes les gardes qui s'y fiaient étaient aveugles.
- **Le bruit de mesure du pH** vient des fuites de la pompe de **filtration** —
  et deux autres hypothèses (phase de pompe doseuse, panache d'injection) ont été
  **explicitement réfutées** : les sondes sont en amont de l'injection.

Reprendre l'une de ces pistes réfutées coûterait à nouveau plusieurs cycles.

## Réactiver cette mémoire sur une autre machine

Claude Code range sa mémoire dans un dossier dérivé du **chemin absolu** du
projet, qui change d'une machine à l'autre. Pour la réactiver après un clone :

```bash
# 1. Trouver le dossier de mémoire de CETTE machine
ls ~/.claude/projects/

# 2. Y copier le contenu versionné
cp .claude/memory/*.md ~/.claude/projects/<le-dossier-trouvé>/memory/
```

À défaut, ces fichiers restent **lisibles tels quels** : `MEMORY.md` d'abord.

## Tenir cette mémoire à jour

Elle est désormais versionnée : toute mémoire écrite dans
`~/.claude/projects/…/memory/` doit être **recopiée ici** pour survivre à un
changement de poste. Sans quoi le dossier redevient ce qu'il était — une
connaissance qui n'existe que sur un disque.

## Ce qui n'y est pas, volontairement

Aucun secret : ni le mot de passe de l'appareil, ni son adresse IP (vérifié).
Voir `docs/PASSATION.md` pour ce qui doit être transmis autrement.
