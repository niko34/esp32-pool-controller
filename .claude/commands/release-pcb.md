---
description: Publie une release PCB (gerbers de fabrication) — ne touche ni au firmware ni aux PDF
argument-hint: [pcb-vX] [résumé des changements matériels]
allowed-tools: Bash(git status:*), Bash(git tag:*), Bash(git push:*), Bash(git log:*), Bash(gh auth status:*), Bash(gh release create:*), Bash(gh release view:*), Bash(./scripts/build-gerbers.sh:*), Bash(ls:*), Bash(unzip:*), Read
---

Publie une release **PCB** : génère les fichiers de fabrication et les attache à
une release GitHub, pour tracer ce qui a été envoyé au fabricant.

Arguments reçus : `$ARGUMENTS`
(Premier mot = version au format `pcb-vX` ou `pcb-vX.Y` si fourni ; le reste =
résumé des changements matériels.)

## Périmètre — ce que cette commande ne fait PAS

**Elle ne compile pas le firmware et ne régénère aucun PDF.** Le matériel et le
logiciel ont des rythmes différents : un PCB est fabriqué deux ou trois fois dans
la vie d'un projet, le firmware sort chaque semaine. Mélanger les deux
obligerait à recompiler pour envoyer un PCB, et à refabriquer un PCB pour
publier un correctif.

**Elle ne commite RIEN et ne pousse QUE le tag.**

Les gerbers produits sont **volontairement ignorés par git** : ils se régénèrent
depuis le `.kicad_pcb` et n'existent que comme assets de release.

## Étapes

### 1. Pré-vérifications

- `gh auth status` : si absent ou non authentifié, **arrête** avec les
  instructions (`brew install gh`, puis `gh auth login`).
- `git status --porcelain` : doit être **vide**. C'est important ici : les
  gerbers doivent correspondre exactement à des sources KiCad commitées, sinon
  on fabrique une carte dont le schéma n'est nulle part.
- `git fetch` puis `git rev-list --left-right --count @{u}...HEAD` : la branche
  doit être **à jour avec origin**. Un arbre propre ne suffit pas — on peut être
  propre avec vingt commits en attente. Pousser un tag sur une branche non
  poussée produit un état bancal : le tag et sa release existent sur GitHub,
  mais la branche y montre un code antérieur.
  Si la branche est en avance, **arrête et signale-le** : c'est à l'humain de
  décider de pousser, pas à la commande.

### 2. Génération des fichiers de fabrication

    ./scripts/build-gerbers.sh <version>

Le script sonde `kicad-cli` (y compris dans `/Applications/KiCad/`, où
l'installateur macOS ne l'ajoute pas au PATH) et échoue proprement s'il est
absent. Il produit `kicad/gerbers-<version>.zip`.

En cas d'échec, arrête et montre le message.

### 3. Version et résumé

Si la version n'est pas dans `$ARGUMENTS`, demande-la. Format **`pcb-vX`** ou
**`pcb-vX.Y`** — ce préfixe distingue les tags matériels des tags firmware
`vX.Y.Z` dans une liste de tags qui en compte déjà des dizaines.

Vérifie qu'**aucune référence** de ce nom n'existe — pas seulement un tag :

    git show-ref | grep -w "<version>"

`git tag -l` ne suffit pas. Une **branche** homonyme rend la référence ambiguë, et
`git push origin <version>` échoue alors avec
`src refspec <version> matches more than one` — après que le tag a été créé
localement. Le cas s'est produit à la première utilisation de cette commande : une
branche `pcb-v2` traînait, vestige d'un travail logiciel sans rapport avec la carte.

Si une référence existe, **arrête** et dis laquelle (branche ? tag ? distante ?).

Si le résumé n'est pas fourni, demande-le. Pour une révision de PCB, ce qui
compte est : **ce qui change physiquement** (empreintes, routage, connecteurs,
alimentation), **ce qui devient incompatible** avec la révision précédente, et
**ce qu'il faut commander en plus ou en moins** côté composants.

### 4. Contrôle avant publication

Affiche le contenu du zip (`unzip -l`) et sa taille, puis **demande
confirmation**. Un envoi en fabrication coûte de l'argent et des semaines : une
couche manquante ou un fichier de perçage absent ne se rattrape pas.

Vérifie en particulier la présence de : cuivre avant et arrière, masque avant et
arrière, sérigraphie, contour (`Edge_Cuts`) et perçage (`.drl`).

### 5. Tag et release

    git tag -a <version> -m "<résumé court>"
    git push origin <version>

    gh release create <version> \
      --title "<version> — <titre court>" \
      --notes-file <fichier de notes dans le scratchpad> \
      kicad/gerbers-<version>.zip

Écris les notes dans un fichier du scratchpad plutôt que de les passer en
ligne de commande.

Indique dans les notes **le fabricant visé et les options de commande** si
l'utilisateur les précise (épaisseur, finition, couleur du masque, nombre de
couches) : c'est l'information qui manque toujours quand on veut refabriquer la
même carte deux ans plus tard.

### 6. Fin

Affiche l'**URL de la release** et rappelle que le zip est prêt à être déposé
chez le fabricant.

## Rappels

- Le dépôt est **privé** : les assets ne sont téléchargeables que par toi.
- Les sources KiCad (`.kicad_pcb`, `.kicad_sch`, `.kicad_pro`, `BOM.xlsx`) sont
  versionnées ; les gerbers, le `BOM.csv` et l'état local KiCad ne le sont pas.
- Si une étape échoue après la création du tag, **dis-le explicitement**
  (`git push --delete origin <version>` et `git tag -d <version>` pour revenir
  en arrière).
