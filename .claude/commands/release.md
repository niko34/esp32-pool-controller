---
description: Publie une release firmware + documentation (tag, PDF, binaire, GitHub)
argument-hint: [vX.Y.Z] [résumé des changements]
allowed-tools: Bash(git status:*), Bash(git fetch:*), Bash(git rev-parse:*), Bash(git rev-list:*), Bash(git tag:*), Bash(git push:*), Bash(git log:*), Bash(pio run:*), Bash(gh auth status:*), Bash(gh release create:*), Bash(gh release view:*), Bash(./scripts/build-docs.sh:*), Bash(./tools/check_release_artefacts.sh:*), Bash(ls:*), Bash(grep:*), Read
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

ℹ️ **Sans `-e`** : `default_envs = esp32dev_core3` est le **seul** environnement
matériel depuis la v2.42.1 (ADR-0069). N'invente aucun autre nom d'env, et ne
lance aucun `pio pkg install` / `uninstall` ici — la commande réécrit
`platformio.ini` en supprimant tous ses commentaires.

Si la compilation échoue, arrête et montre l'erreur. Relève au passage la ligne
`Flash:` pour l'inclure dans les notes.

### 2bis. Non-livraison de R3 (feature-128, S1/ADR-0081) — sur l'ARTEFACT

`/debug/heap_peek` (lecture de contenu du tas) ne doit **jamais** exister dans
un binaire de release. Le mécanisme repose sur l'absence de `POOL_HEAP_PEEK`
dans tout fichier versionné (`./tools/check_no_peek_flag.sh`, job CI dédié) —
mais **ça ne prouve pas l'artefact** : `pio run` à l'étape précédente compile
**localement**, dans l'environnement de *cet appel*, pas dans celui de la CI.
Une variable `PLATFORMIO_BUILD_FLAGS` résiduelle du shell qui exécute cette
commande publierait R3 sans qu'aucun fichier du dépôt n'ait changé.

    ./tools/check_release_artefacts.sh

### 2ter. SIGNER les deux binaires (feature-151) — ⚠️ exige la phrase de passe de l'utilisateur

⛔ **Le contrôle de l'étape 2bis REFUSE désormais une release non signée** (contrôle S2) :
sans `.sig`, `/check-update` servirait une URL de signature **vide** et l'appareil n'aurait
rien à vérifier — ⚠️ la fonctionnalité mourrait **en silence**, exactement comme
`littlefs.bin` pendant six semaines (bug-170).

⚠️ **Construire le système de fichiers d'abord** (`./build_fs.sh`), sinon `littlefs.bin`
n'existe pas et le contrôle échoue.

Pour **chacun** des deux binaires, construire le manifeste **exactement** comme le firmware
le re-sérialise, puis signer :

```bash
V=$(sed -n 's/.*FIRMWARE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' src/version.h)
B=.pio/build/esp32dev_core3

for pair in "firmware:$B/firmware.bin" "filesystem:$B/littlefs.bin"; do
  genre=${pair%%:*}; bin=${pair#*:}
  d=$(sha256sum "$bin" | awk '{print $1}')
  printf '%s' "pool-ota/1 $genre $V sha256:$d" > /tmp/m.txt
  openssl dgst -sha256 -sign "${OTA_SIGNING_KEY:-$HOME/.pool-ota/signing_key.pem}" \
    -out "$bin.sig" /tmp/m.txt
done
```

⚠️ **`printf '%s'`, JAMAIS `echo`** : `echo` ajoute un saut de ligne final, la signature ne
correspondrait plus, et le diagnostic serait « signature invalide » — ⛔ indistinguable d'une
mauvaise clé. C'est **le** piège identifié par l'audit.

⚠️ **L'utilisateur devra saisir sa phrase de passe DEUX fois** (une par binaire). ⛔ Ne jamais
la passer en argument ni en variable d'environnement. ⛔ Ne jamais la stocker.

⚠️ **Signer APRÈS la compilation finale** : recompiler ensuite change l'empreinte et invalide
la signature. Le contrôle S2 le détecte, mais autant ne pas y arriver.

⇒ **Relancer `./tools/check_release_artefacts.sh`** : il vérifie les deux signatures contre la
clé publique **EXTRAITE DE `src/constants.h`** — donc celle que l'appareil utilisera
réellement, ⛔ pas un fichier à côté.

⚠️ **Une instruction « arrête » en prose ici ne serait pas une garantie** —
rien ne forcerait l'exécutant à s'arrêter. C'est pourquoi ce contrôle est un
**script à code de sortie**, pas un paragraphe : si la commande sort en erreur
(code non nul), **arrête** — n'exécute **aucune** étape suivante, et montre la
sortie du script telle quelle (elle indique déjà la cause et la marche à
suivre : `PLATFORMIO_BUILD_FLAGS` résiduel, archive périmée, etc.).

Le script vérifie, dans l'ordre : (1) `/debug/heap_peek` absente du `.bin` ET
de l'`.elf` fraîchement compilés (`grep -a -c` sur `.rodata` — pas `nm`, qui
peut rendre un faux négatif sous `-Os` si la route s'inline) ; (2) que l'ELF
archivé qui sera **réellement attaché à l'étape 6** est une copie binaire
identique (`cmp`) de celui vérifié en (1) — sélectionné par **nom**
(horodatage en tête, `sort -r`), jamais par date de modification
(`archive_build.py` documente pourquoi `shutil.copy2` rend `ls -t` non fiable
ici : la mtime copiée est celle de la SOURCE).

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

Prends celui de `esp32dev_core3` (environnement de production).

🔴 **JOINS-LE SOUS DEUX NOMS, ET JOINS AUSSI `littlefs.bin`.** Ce n'est pas un
confort, c'est ce qui fait marcher la mise à jour en ligne :

| Asset | Pourquoi |
|---|---|
| `firmware.bin` | ⛔ **NOM EXACT exigé** par `web_routes_ota.cpp` (`if (name == "firmware.bin")`). Sans lui, l'appareil trouve la release mais **aucune URL de firmware**. |
| `littlefs.bin` | ⛔ **NOM EXACT exigé** de même (`littlefs.bin`). Sans lui, l'interface web n'est **jamais** mise à jour en ligne. Le construire avec `./build_fs.sh`. |
| `firmware.bin.sig` | ⛔ **NOM EXACT exigé** (feature-151). Sans lui, `/check-update` sert une URL de signature **VIDE** et l'appareil n'a **rien à vérifier** — ⚠️ la fonctionnalité meurt **en silence**. |
| `littlefs.bin.sig` | ⛔ Idem pour le système de fichiers. |
| `firmware-<version>.bin` | Confort humain : un `firmware.bin` téléchargé est indistinguable d'une version à l'autre. |
| `firmware-<version>.elf` | Symboles de débogage (voir ci-dessous). |

⚠️ **ORIGINE DE CETTE RÈGLE (2026-10-16)** : entre v2.18.7 et v2.42.0, les assets
ont été renommés `firmware-<version>.bin` — raison valable en soi — et
`littlefs.bin` a cessé d'être joint. ⛔ **Personne n'a vu que l'updater cherchait
les anciens noms.** La mise à jour en ligne a été silencieusement cassée pendant
**six semaines**, et le défaut n'est apparu qu'en la testant pour de vrai.
⇒ ⛔ Ne retire jamais `firmware.bin`, `littlefs.bin` ni leurs `.sig` de cette liste sans avoir
d'abord changé `web_routes_ota.cpp`.

**Joins aussi l'ELF** — c'est lui qui porte les symboles, et **sans lui un
coredump de cette version est indécodable**. `esp-coredump` vérifie le SHA256 de
l'image applicative et refuse tout ELF qui ne correspond pas exactement : celui
d'une version voisine ne fera pas l'affaire.

    ls builds/<version sans le v>/esp32dev_core3/*.elf   # archivé par le hook post-build

Prends le plus récent **trié par nom** (l'horodatage `AAAAMMJJ_HHMMSS` est en
tête du nom de fichier), **jamais par date de modification** (`ls -t`) — c'est
exactement le fichier que `tools/check_release_artefacts.sh` a déjà identifié
et vérifié à l'étape 2bis (sa dernière ligne l'affiche) : réutilise-le tel
quel plutôt que d'en resélectionner un autre. Renomme-le `firmware-<version>.elf`.
Il pèse ~29 Mo, c'est normal (symboles de débogage) et c'est le prix d'un
diagnostic possible. Le projet a déjà perdu un coredump de production faute de
cet ELF.

Rassemble les PDF de `docs/user/` (ils portent le numéro de version dans leur
nom, ex. `guide-d-utilisation-v2.42.pdf`).

    gh release create <version> \
      --title "<version> — <titre court>" \
      --notes-file <fichier de notes dans le scratchpad> \
      .pio/build/esp32dev_core3/firmware.bin \
      .pio/build/esp32dev_core3/firmware.bin.sig \
      .pio/build/esp32dev_core3/littlefs.bin \
      .pio/build/esp32dev_core3/littlefs.bin.sig \
      <firmware-<version>.bin> \
      <firmware-<version>.elf> \
      docs/user/*.pdf

Puis **vérifie la liste des assets** — `firmware.bin`, `littlefs.bin` et leurs **`.sig`** doivent y
figurer **au nom exact** :

    gh release view <version> --json assets -q '.assets[].name'

Écris les notes dans un fichier du répertoire scratchpad et utilise
`--notes-file` : les notes multi-lignes passées en `--notes` sur la ligne de
commande sont fragiles (guillemets, accents, retours à la ligne).

### 7. Fin

Affiche l'**URL de la release** (`gh release view <version> --json url -q .url`),
ainsi qu'un récapitulatif : version, taille du binaire, PDF attachés, occupation
flash relevée à l'étape 2.

## Rappels

- Le dépôt est **privé** : les assets ne sont téléchargeables que par toi.
  ⚠️ **Et c'est pourquoi la mise à jour EN LIGNE depuis l'appareil ne marche pas** :
  l'API GitHub répond **404** sur un dépôt privé sans authentification, et
  l'appareil n'envoie aucun jeton. Il affiche alors « Aucune release disponible ».
  ⛔ Ce n'est **pas** un défaut de la release — voir bug-169.
- N'invente jamais un numéro de version : demande-le.
- Si une étape échoue après la création du tag, **dis-le explicitement** — un tag
  poussé sans release derrière est un état à corriger à la main
  (`git push --delete origin <version>` et `git tag -d <version>`).
