---
description: Publie une release firmware + documentation (tag, PDF, binaire, GitHub)
argument-hint: [vX.Y.Z] [résumé des changements]
allowed-tools: Bash(git status:*), Bash(git fetch:*), Bash(git rev-parse:*), Bash(git rev-list:*), Bash(git tag:*), Bash(git push:*), Bash(git log:*), Bash(pio run:*), Bash(gh auth status:*), Bash(gh release create:*), Bash(gh release view:*), Bash(./scripts/build-docs.sh:*), Bash(ls:*), Bash(grep:*), Read
---

Publie une release **firmware + documentation** pour ce projet.

Arguments reçus : `$ARGUMENTS`
(Premier mot = version au format `vX.Y.Z` si fourni ; le reste = résumé des
changements. L'un comme l'autre peuvent être absents.)

## Règle absolue

**Cette commande ne commite RIEN et ne pousse QUE le tag.** Aucun `git add`,
aucun `git commit`, aucun `git push` de branche. Si l'arbre de travail est sale,
tu t'arrêtes et tu le signales — tu ne commites pas pour « débloquer ».

Les PDF régénérés à l'étape 3 sont **volontairement ignorés par git** : ils sont
des assets de release, jamais du contenu versionné. Ne t'étonne pas de ne pas les
voir apparaître dans `git status`, et ne cherche pas à les ajouter.

## Étapes

### 1. Pré-vérifications — tout échec arrête la commande

- `gh auth status` : si `gh` est absent ou non authentifié, **arrête** avec :
  - installation : `brew install gh` (macOS) ou https://cli.github.com
  - authentification : `gh auth login`
- `git status --porcelain` : doit être **vide**. Sinon, affiche les fichiers
  concernés et arrête — l'utilisateur décide quoi en faire.
- `git fetch` puis compare `HEAD` à `@{u}` : la branche doit être **à jour avec
  origin**, ni en avance ni en retard. Si elle diverge, affiche l'écart
  (`git rev-list --left-right --count @{u}...HEAD`) et arrête.

### 2. Compilation

    pio run

⚠️ **Sans `-e`.** Les environnements `esp32dev` (core 2) et `esp32dev_core3`
partagent le même répertoire de framework et **se détruisent mutuellement** :
ne lance jamais `pio run -e esp32dev` ici.

Si la compilation échoue, arrête et montre l'erreur. Relève au passage la ligne
`Flash:` pour l'inclure dans les notes.

### 3. Documentation

    ./scripts/build-docs.sh

Le script détecte le moteur de rendu et échoue avec un message explicite si
aucun n'est disponible. En cas d'échec, arrête : une release sans documentation
à jour n'a pas d'intérêt.

### 4. Version et résumé

Si la version n'est pas dans `$ARGUMENTS`, demande-la. Format **`vX.Y.Z`**.

Vérifie sa cohérence avec `src/version.h` (`FIRMWARE_VERSION`) : ils doivent
correspondre. En cas d'écart, **signale-le et demande confirmation** plutôt que
de choisir toi-même — c'est en général le signe que le bump a été oublié.

Vérifie aussi que le tag n'existe pas déjà (`git tag -l`). Si oui, arrête.

Si le résumé n'est pas fourni, demande-le. Pour t'aider à le rédiger, appuie-toi
sur `CHANGELOG.md` (section de cette version) et sur
`git log <dernier tag>..HEAD --oneline`.

Rédige les notes de release **en français**, orientées **utilisateur** : ce qui
change pour qui s'en sert, pas la liste des commits. Mentionne les corrections
de bugs visibles et les changements de comportement.

### 5. Tag

    git tag -a <version> -m "<résumé court>"
    git push origin <version>

**Uniquement le tag.** Jamais `git push` seul, jamais `--tags`.

### 6. Release GitHub

Trouve le binaire — le chemin exact dépend de l'environnement :

    ls .pio/build/*/firmware.bin

Prends celui de `esp32dev_core3` (environnement de production). Copie-le sous un
nom parlant pour l'asset, par exemple `firmware-<version>.bin` : un asset nommé
`firmware.bin` est indistinguable d'une release à l'autre une fois téléchargé.

**Joins aussi l'ELF** — c'est lui qui porte les symboles, et **sans lui un
coredump de cette version est indécodable**. `esp-coredump` vérifie le SHA256 de
l'image applicative et refuse tout ELF qui ne correspond pas exactement : celui
d'une version voisine ne fera pas l'affaire.

    ls builds/<version sans le v>/esp32dev_core3/*.elf   # archivé par le hook post-build

Prends le plus récent et renomme-le `firmware-<version>.elf`. Il pèse ~29 Mo,
c'est normal (symboles de débogage) et c'est le prix d'un diagnostic possible.
Le projet a déjà perdu un coredump de production faute de cet ELF.

Rassemble les PDF de `docs/user/` (ils portent le numéro de version dans leur
nom, ex. `guide-d-utilisation-v2.42.pdf`).

    gh release create <version> \
      --title "<version> — <titre court>" \
      --notes-file <fichier de notes dans le scratchpad> \
      <firmware-<version>.bin> \
      <firmware-<version>.elf> \
      docs/user/*.pdf

Écris les notes dans un fichier du répertoire scratchpad et utilise
`--notes-file` : les notes multi-lignes passées en `--notes` sur la ligne de
commande sont fragiles (guillemets, accents, retours à la ligne).

### 7. Fin

Affiche l'**URL de la release** (`gh release view <version> --json url -q .url`),
ainsi qu'un récapitulatif : version, taille du binaire, PDF attachés, occupation
flash relevée à l'étape 2.

## Rappels

- Le dépôt est **privé** : les assets ne sont téléchargeables que par toi.
- N'invente jamais un numéro de version : demande-le.
- Si une étape échoue après la création du tag, **dis-le explicitement** — un tag
  poussé sans release derrière est un état à corriger à la main
  (`git push --delete origin <version>` et `git tag -d <version>`).
