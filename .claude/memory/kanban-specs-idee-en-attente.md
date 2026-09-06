---
name: kanban-specs-idee-en-attente
description: "Vue Kanban de l'état des specs : idée évaluée le 03/09/2026, décision utilisateur « on ne fait rien là-dessus pour le moment » ; option retenue si relance = tableau HTML généré depuis le champ Statut des specs (pas d'outil externe)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-03T08:39:56.724Z
---

Le 03/09/2026, l'utilisateur a demandé quel outil donnerait une vue Kanban de
l'état de traitement des specs (rédigée → à développer → en dev → déployée en
intégration/observation → terminée), puis a tranché : **« On ne fait rien
là-dessus pour le moment. »** Ne pas re-proposer spontanément.

Cadrage déjà fait, à reprendre tel quel si l'utilisateur relance :
- Constat : les 3 dossiers `todo/doing/done` ne portent plus l'état réel
  (`doing/` mélange cycles en cours et livraisons déployées en observation ;
  `todo/` mélange spec fraîche et études anciennes).
- Option recommandée : enrichir le champ `Statut` du bloc Métadonnées
  (`redigee | a-developper | en-cours | deployee | terminee`) + script
  `tools/kanban.py` générant un HTML depuis les specs (source de vérité unique,
  hors ligne, tenu par les agents en fin de cycle ; publiable en artefact).
  La colonne « À développer » est la seule décision humaine.
- Écartés : Kanbn (impose son format), GitHub Projects (double saisie, externe),
  Obsidian Kanban (appli de plus, mais migration triviale plus tard).

**Why :** l'utilisateur veut voir où en est chaque spec sans ouvrir 32 fichiers,
mais n'a pas voulu ouvrir ce chantier pendant la fenêtre d'observation FF.

**How to apply :** si demande de « tableau », « kanban », « où en sont les
specs » → proposer directement le temps 1 (vocabulaire Statut + reclassement
des specs de todo/doing avec proposition à trancher), puis le script.
