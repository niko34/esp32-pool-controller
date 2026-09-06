## Métadonnées
- **ID** : feature-103
- **Slug** : gardes-ci-build-firmware-et-hygiene-specs
- **Type** : feature (outillage / CI)
- **Statut** : **done** — livré le 2026-09-06 (tests locaux verts, revue OK, docs à jour)
- **Priorité** : haute
- **Version cible** : `N/A` — aucun changement de firmware, aucun bump de version
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `feature-102` (retrait du repli core 2) et [ADR-0069](../../../docs/adr/0069-retrait-repli-core-2.md), qui ont documenté ces deux trous sans les combler

## Domaines touchés
- [ ] Capteurs — [ ] Régulation — [ ] Filtration / Éclairage — [ ] Web — [ ] MQTT / HA
- [ ] UART — [ ] OTA — [ ] Authentification — [ ] Historique — [ ] Frontend
> **Aucun domaine fonctionnel.** Le périmètre est exclusivement CI et outillage.

## Agents requis
- [x] spec-reader
- [ ] architect — décisions de conception tranchées dans cette spec (§ Choix de conception)
- [ ] pool-chemistry — aucun code de dosage touché
- [ ] ux-ui-designer — [ ] embedded-developer — [ ] web-ui-developer
- [ ] test-engineer — la validation est l'exécution du script et du workflow eux-mêmes
- [x] code-reviewer
- [x] doc-writer

## Description
Ajouter les deux gardes automatiques qui manquent à la CI : la **compilation du
firmware** (aujourd'hui jamais vérifiée par GitHub) et un **contrôle d'hygiène des
specs** refusant qu'un même nom de spec figure dans plusieurs des dossiers `todo/`,
`doing/` et `done/`.

## Contexte
Deux constats du 2026-09-06, tous deux documentés et tous deux laissés ouverts.

**1. La CI ne compile pas le firmware.** `.github/workflows/tests-natifs.yml` ne lance
que `pio test -e native` et `native_coverage` — de la logique pure sur runner Linux.
Aucun environnement matériel n'est construit. C'est **la raison pour laquelle la casse
du core 2 est passée inaperçue pendant deux versions** : `esp_cpu_get_cycle_count()`
appelé hors garde dans `oom_guard.cpp` depuis la v2.40.0, découvert par hasard le
2026-09-06 en vérifiant un environnement de développement.

Le retrait du core 2 (feature-102) ne referme pas ce trou : il en supprime une victime
possible. La classe de défaut — « le firmware ne compile plus, et rien ne le dit »
— reste entière pour `esp32dev_core3`, seul environnement restant, et pour tout
successeur. C'est un manque **antérieur et indépendant** du repli core 2.

**2. Rien ne vérifie l'unicité d'une spec.** `CLAUDE.md` impose « une spec à la fois »,
et le cycle `todo/` → `doing/` → `done/` suppose qu'un nom vive dans un seul dossier.
Aucun mécanisme ne le contrôle. Constat du 2026-09-06 : trois specs existaient
simultanément dans deux dossiers avec des statuts **contradictoires** — « todo » d'un
côté, « livré en 2.20.x, vérifié » de l'autre. Quand cela arrive, la question « où en
est cette feature ? » n'a plus de réponse lisible dans les fichiers : elle retourne dans
une tête ou dans un historique de conversation. C'est exactement ce que la méthode du
dépôt cherche à éviter.

Une règle que rien ne vérifie n'est pas une règle — le même raisonnement que la
validation par mutation appliquée aux tests.

## Comportement attendu
- Tout push sur `main` et toute PR touchant le firmware déclenche `pio run -e esp32dev_core3`.
  Échec de compilation ⇒ **run rouge**, notification GitHub.
- Tout push et toute PR déclenche le contrôle d'hygiène des specs. Un nom de fichier
  présent dans plusieurs des trois dossiers ⇒ **run rouge**, avec en sortie la liste des
  noms fautifs et les dossiers concernés.
- Le contrôle est **exécutable en local** à l'identique : `./tools/check_specs.sh`.
  Sortie 0 si tout va bien, 1 sinon. Pas un script réservé à la CI.

### Cas limites
- Dossier `todo/`, `doing/` ou `done/` **vide ou absent** → sortie 0 avec mention explicite
  du dossier ignoré : un cycle sans spec en cours est un état légitime.
- Répertoire `specs/features/` **entièrement absent** → sortie **1**. Tranché en revue :
  un dépôt incomplet ne doit pas produire un contrôle vert. C'est un cas distinct du
  précédent, et la formulation initiale de cette spec les confondait.
- **`mktemp` en échec** (`/tmp` plein, `TMPDIR` cassé, `mktemp` de BSD qui exige un
  template) → sortie **1**, jamais 0.
- Nom contenant des espaces ou des accents → doit être géré (les specs du dépôt en ont).
- Un commit ne touchant que `docs/` ne doit pas déclencher la compilation firmware
  (économie de minutes CI) — mais doit déclencher le contrôle d'hygiène, qui coûte ~1 s.

## Comportement actuel
Aucune des deux gardes n'existe. `docs/BUILD.md:67` affirme même « **CI non impactée** :
`tests-natifs.yml` ne construit que l'env `native` » — vrai aujourd'hui, faux après cette
spec.

## Contraintes techniques
- **Le contrôle d'hygiène échouerait immédiatement sur l'état actuel du dépôt** : les
  trois doublons `feature-071`, `feature-072` et `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue`
  existent toujours dans `todo/` **et** `done/`. Leur suppression est donc un **prérequis**
  de cette spec, pas un travail annexe (item A1).
- Le build firmware télécharge la toolchain pioarduino (~2 Go décompressés :
  `framework-arduinoespressif32-libs` 1,9 Go + `toolchain-xtensa-esp-elf` 1,2 Go). Le cache
  `actions/cache` existant (clé `pio-${{ runner.os }}-${{ hashFiles('platformio.ini') }}`)
  doit être réutilisé tel quel pour ne pas payer ce téléchargement à chaque run.
- Aucune modification de `src/`, `platformio.ini`, `partitions.csv` ni des scripts de build.
- Aucun bump de version : le binaire n'est pas touché.

## Choix de conception (tranchés dans cette spec, faute d'ADR nécessaire)
1. **Deux workflows séparés plutôt qu'un job ajouté à `tests-natifs.yml`.** Un échec de
   compilation et un échec de test sont deux signaux distincts ; les mêler dans un
   workflow nommé « Tests natifs » rendrait son nom faux et son résultat ambigu. Le
   fichier existant n'est pas renommé (sa référence dans `BUILD.md` et son historique de
   runs GitHub restent valides).
2. **Le contrôle d'hygiène ne vit pas dans le workflow firmware.** Il coûte une seconde
   et n'a besoin d'aucune toolchain : le placer derrière un téléchargement de 2 Go serait
   absurde.
3. **La logique du contrôle est dans un script du dépôt, pas dans le YAML.** Un script
   `tools/check_specs.sh` est exécutable en local, testable, et réutilisable plus tard en
   hook pre-commit. Du shell inline dans un workflow n'est ni l'un ni l'autre.
4. **Filtre de chemins sur le build firmware** (`src/**`, `platformio.ini`,
   `partitions.csv`, **`tools/archive_build.py`** — script `post:` de l'env, donc
   véritable entrée de build, oubli signalé par `spec-reader` — et le workflow lui-même) : un commit purement documentaire ne doit pas
   consommer plusieurs minutes de runner. ⚠️ Contrepartie à connaître : si ce job est un
   jour déclaré *required check* sur les PR, un filtre de chemins peut bloquer la fusion
   d'une PR qui ne touche aucun de ces chemins (le statut attendu n'arrive jamais). À
   documenter dans `BUILD.md`.
5. **Une seule règle d'hygiène : l'unicité du nom entre les trois dossiers.** Vérifier
   aussi la cohérence entre le champ `Statut` et le dossier a été écarté : ce champ est en
   texte libre dans le dépôt (« done — livré en 2.20.7 », « **livré en 2.20.3** »,
   « todo »), et le parser produirait des faux positifs. L'unicité, elle, est binaire et
   sans interprétation. **Complété après analyse d'impact** : une seconde règle,
   **non bloquante**, signale un même numéro porté par deux slugs différents.
   `spec-reader` a trouvé le cas : `feature-067` désigne à la fois
   `todo/feature-067-etude-portage-esp32-s3.md` et `done/feature-067-offset-mesure-ph.md`.
   Un contrôle par nom de fichier ne le voit pas, et toute référence « feature-067 »
   dans un commit ou un ADR est donc ambiguë. Non bloquante à dessein : corriger
   exigerait de renuméroter une spec déjà close, donc de réécrire de l'historique.
6. **⚠️ CORRECTION d'un choix initial erroné de cette spec.** Elle prescrivait de
   réutiliser « la clé de cache EXISTANTE » de `tests-natifs.yml`. C'est faux, et le
   défaut aurait été silencieux : une clé de cache GitHub Actions est **immuable une
   fois créée**. Les deux workflows démarrant en parallèle sur le même commit, si le
   job natif gagne la course il écrit sous cette clé un cache ne contenant que l'env
   `native` (quelques Mo) ; le job firmware le restaurerait, retéléchargerait 2 Go de
   toolchain, et ne pourrait **jamais** sauvegarder le sien, la clé étant prise.
   Résultat : 2 Go à chaque run, indéfiniment. Le workflow firmware utilise donc son
   propre préfixe **`pio-fw-`**. Défaut relevé par `spec-reader`.
7. **Les ADR ne sont pas corrigés, ils sont amendés.** `docs/adr/0069-*.md` affirme
   « l'env core 3 reste sans garde CI » (l. 38 et 196) — affirmation que cette spec
   rend fausse le jour même de sa rédaction. Un ADR étant immuable, il reçoit une
   **bannière** renvoyant à feature-103, jamais une réécriture. Idem `ADR-0042:87`.

## Fichiers potentiellement concernés
- `tools/check_specs.sh` **[nouveau]** — le contrôle, exécutable en local
- `.github/workflows/build-firmware.yml` **[nouveau]** — compilation `esp32dev_core3`
- `.github/workflows/hygiene-specs.yml` **[nouveau]** — exécution du contrôle
- `specs/features/todo/{feature-071,feature-072,bug-filtre-ph-...}.md` — **suppression**
- `docs/BUILD.md` — la section CI et l'affirmation « CI non impactée » (l. 67)
- `CLAUDE.md` — la ligne « CI GitHub Actions sur push/PR » du § Tests
- `.claude/skills/spec-workflow/SKILL.md` — mentionner le contrôle d'unicité
- **Autres mentions de la CI recensées par `spec-reader`**, absentes de la liste initiale :
  `docs/BUILD.md:28`, `docs/adr/README.md:88`, `docs/UPDATE_GUIDE.md:571`,
  `docs/test-on-target.md:98`, `README.md:115`. Les entrées `CHANGELOG.md:17` et `:1106`
  et les corps d'ADR sont **historiques** : ni corrigés ni réécrits.

## Livrables — check-list
- [x] A1 — **PRÉREQUIS** : supprimer les 3 copies redondantes de `specs/features/todo/`
      (`feature-071-carte-signaux-dispersion-drapeau.md`, `feature-072-courbe-dispersion.md`,
      `bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue.md`). Vérifié avant
      suppression : les copies `done/` sont strictement plus riches (mêmes contenus, cases
      cochées, notes d'implémentation) et le seul paragraphe qui semblait propre à `todo/`
      — le précédent chiffré « faux max 5,425 contre 0,308 réel » sur la reconstruction de
      dispersion — **est présent dans la copie `done/`**, ainsi que dans `data/app.js` et
      `docs/features/page-settings.md`.
- [x] A2 — créer `tools/check_specs.sh` : sortie 1 et liste explicite si un nom apparaît
      dans plusieurs dossiers, sortie 0 sinon. Robuste aux dossiers vides et aux accents.
- [x] A3 — créer `.github/workflows/hygiene-specs.yml` : push + PR, sans filtre de chemins,
      appelle `./tools/check_specs.sh`. Pas de Python, pas de PlatformIO, pas de cache.
- [x] A4 — créer `.github/workflows/build-firmware.yml` : `pio run -e esp32dev_core3`,
      filtre de chemins (item 4), cache `~/.platformio` avec la clé EXISTANTE, groupe de
      concurrence propre avec `cancel-in-progress`.
- [x] A8 — **correctifs de revue** (`code-reviewer`, Request changes) :
      (a) *fail-open bloquant* — si `mktemp` échouait, `$INVENTAIRE` restait vide, la liste
      des doublons ressortait vide et le script annonçait « aucun doublon » en **sortant 0**
      alors qu'un doublon existait. Reproduit en revue. Corrigé : template explicite
      (exigé par le `mktemp` de BSD/macOS) et garde `||` qui sort en 1 ;
      (b) *emplacements faux* — le `grep -F "nom<TAB>"` n'était pas ancré : un nom suffixe
      d'un autre (`feature-202-a.md` vs `pre-feature-202-a.md`) recevait les dossiers du
      second. Détection juste, affichage trompeur. Corrigé par une comparaison de champ
      exacte en `awk` ;
      (c) même correction appliquée à l'affichage de la règle 2.
- [x] A5 — vérifier localement : `./tools/check_specs.sh` sort 0 après A1, et sort 1 avec
      la bonne liste sur un doublon fabriqué pour le test (puis retiré).
- [x] A6 — valider la syntaxe des deux YAML avant commit. ⚠️ **GitHub Actions ne
      supporte pas les ancres YAML** (`&ancre` / `*ancre`) : la liste de chemins doit
      être dupliquée littéralement dans les blocs `push` et `pull_request`.
- [x] A7 — seconde règle du script (avertissement non bloquant) : un même numéro porté
      par plusieurs slugs. Doit signaler `feature-067` sans faire échouer le run.
- [x] C1 — `docs/BUILD.md` : remplacer l'affirmation « CI non impactée » (l. 67), décrire
      les trois workflows, et documenter la contrepartie du filtre de chemins (item 4).
- [x] C2 — `CLAUDE.md` § Tests : la CI ne se limite plus aux tests natifs. ⏳ **RELIQUAT — rédigé, non appliqué** : `CLAUDE.md` est un fichier de configuration d'agent, que `doc-writer` n'édite pas sur consigne d'un autre agent. Texte prêt à coller dans les notes d'implémentation ci-dessous ; à appliquer par l'utilisateur.
- [x] C3 — ⏳ **RELIQUAT — même motif que C2** (`.claude/` = configuration d'agent). `.claude/skills/spec-workflow/SKILL.md` : mentionner que l'unicité du nom entre
      `todo/`, `doing/` et `done/` est désormais **vérifiée par la CI**.
- [x] C5 — **bannières d'amendement** sur `docs/adr/0069-*.md` (l. 38 et 196,
      « l'env core 3 reste sans garde CI » — devenu faux) et vérifier `ADR-0042:87`.
      Corps des ADR **inchangés**.
- [x] C4 — pas d'entrée `CHANGELOG.md` : aucun changement de comportement du firmware,
      aucune version. À confirmer par `doc-writer`.

## Sécurité chimique
- Dosage impliqué : **non**. Aucun fichier de `src/` touché, aucune limite, aucun PID.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — les choix de conception sont documentés ci-dessus et
  dans `BUILD.md`, aucun ne verrouille d'option future ni n'impose de contrainte au reste
  du système. `doc-writer` tranche s'il estime le contraire.

## Éléments visibles (pour doc-writer)
- Endpoints HTTP : aucun — Champs WebSocket : aucun — Topics MQTT : aucun
- Docs vivantes : aucune (aucun composant firmware, aucune page UI)
- Docs transverses : `docs/BUILD.md` (§ CI)
- Docs immuables : aucune

## Critères d'acceptation
- [x] `./tools/check_specs.sh` sort **0** sur le dépôt après A1
- [x] `./tools/check_specs.sh` sort **1** et nomme le fichier fautif sur un doublon fabriqué
- [x] Les deux fichiers YAML sont syntaxiquement valides
- [x] `git grep "CI non impactée" -- docs/ ':!docs/adr/'` → **aucun résultat**. Le
      critère initial (`docs/` entier) était **incompatible avec l'immuabilité des ADR** :
      `ADR-0042:87` contient cette phrase et ne doit pas être réécrit. Corrigé sur
      signalement de `spec-reader`.
- [x] Aucune modification de `src/`, `platformio.ini`, `partitions.csv`, ni des scripts de build
- [x] Aucun bump de `FIRMWARE_VERSION`

## Plan de test manuel
1. Après A1 : `./tools/check_specs.sh` → sortie 0 (147 specs), avertissement `feature-067`.
2. `cp specs/features/done/feature-102-*.md specs/features/todo/` → sortie 1, nommant
   `feature-102-retrait-repli-core-2.md` et les deux dossiers. Retirer la copie.
3. **Non-régression du fail-open** : avec le doublon ci-dessus en place,
   `TMPDIR=/nexistepas ./tools/check_specs.sh` → sortie **1** (et non 0).
4. **Exactitude des emplacements** : créer en plus `doing/pre-feature-102-....md` → la
   ligne de `feature-102-...` doit afficher `done todo`, sans `doing`.
3. Le comportement réel des workflows ne sera observable qu'**au premier push** : le runner
   GitHub est le seul environnement où ils s'exécutent. À vérifier sur le run suivant.

## Notes pour agent
- Le build firmware en CI exécutera `tools/archive_build.py` (script `post:` de l'env) :
  il écrira dans `builds/` sur le runner. Sans conséquence — le dossier est ignoré par git
  et le runner est détruit — mais à ne pas prendre pour une anomalie.
- Ne pas ajouter de `pio test -e native` au workflow firmware : c'est déjà le rôle de
  `tests-natifs.yml`, et le dupliquer doublerait le coût pour le même signal.

## Hors périmètre
- **Le contrôle en hook pre-commit local** : le script est écrit pour le permettre, mais
  l'installer est un choix d'environnement de travail, pas de dépôt.
- **La couverture des filtres `gcovr`** de `tests-natifs.yml`, limitée à `sensor_filter` et
  `dosing_logic` alors que d'autres modules purs sont couverts à 100 % : constat relevé au
  passage, spec propre à écrire.
- **Toute règle d'hygiène supplémentaire** (cohérence du champ `Statut`, nombre de specs
  dans `doing/`) : écartée par le choix de conception 5.

---

# Notes d'implémentation (2026-09-06)

## Ce qui est livré

| Fichier | Nature |
|---|---|
| `tools/check_specs.sh` | **nouveau** — deux règles (unicité de nom **bloquante**, numéro ambigu **non bloquant**), exécutable en local comme en CI |
| `.github/workflows/hygiene-specs.yml` | **nouveau** — push `main` + PR, sans filtre de chemins, sans toolchain, sans cache |
| `.github/workflows/build-firmware.yml` | **nouveau** — `pio run -e esp32dev_core3`, filtre de chemins, cache préfixé `pio-fw-`, `concurrency` + `cancel-in-progress` |
| `specs/features/todo/{feature-071-…,feature-072-…,bug-filtre-ph-…}.md` | **supprimés** (A1, prérequis : sans quoi la CI naissait rouge) |
| `docs/BUILD.md` | § **Intégration continue (GitHub Actions)** ajouté ; « CI non impactée » (ex-l. 67) et la cause au présent de l'ex-l. 28 corrigées |
| `docs/adr/0069-*.md`, `docs/adr/0042-*.md` | **bannières d'amendement**, corps inchangés |
| `docs/adr/README.md`, `docs/UPDATE_GUIDE.md`, `README.md` | mentions de la CI au présent devenues fausses, rectifiées |

## Trois erreurs de conception rattrapées avant d'être payées

1. **Clé de cache partagée** (relevé par `spec-reader`). La spec prescrivait initialement de
   réutiliser « la clé de cache EXISTANTE » de `tests-natifs.yml`. Une clé de cache GitHub
   Actions est **immuable une fois créée** : les deux workflows démarrant en parallèle sur le
   même commit, le job natif y aurait écrit un cache ne contenant que l'env `native` (quelques
   Mo), le job firmware l'aurait restauré, aurait retéléchargé ~2 Go de toolchain et n'aurait
   **jamais** pu sauvegarder le sien. Le défaut serait resté **silencieux** — le run reste vert,
   il coûte juste 2 Go à chaque exécution, indéfiniment. Corrigé par le préfixe `pio-fw-`.
2. **Ancre YAML** (relevé par `spec-reader`). GitHub Actions ne supporte pas `&ancre` / `*ancre` :
   la liste de chemins est **dupliquée littéralement** entre `push` et `pull_request`. Toute
   modification doit être faite **deux fois**, sous peine d'un filtre asymétrique — le workflow se
   déclencherait sur push et pas sur PR, ou l'inverse. Parité vérifiée en clôture (listes égales).
3. **Fail-open du script** (relevé par `code-reviewer`, verdict *Request changes*, reproduit).
   Si `mktemp` échouait, `$INVENTAIRE` restait vide, tous les accès fichier échouaient, la liste
   des doublons ressortait vide — et le script annonçait « **aucun doublon** » en **sortant 0**
   alors qu'un doublon existait. Une garde qui se neutralise en silence est pire que pas de garde.
   Corrigé : template explicite (exigé par le `mktemp` de BSD/macOS) + garde `||` sortant en 1.
   Corrigé au même passage : le `grep -F "nom<TAB>"` non ancré attribuait à `feature-202-a.md` les
   dossiers de `pre-feature-202-a.md` (détection juste, **affichage** trompeur) → comparaison de
   champ exacte en `awk`, sur les deux règles.

## Le cas `feature-067` — pourquoi la seconde règle n'est pas bloquante

Le script signale, sans faire échouer le run, que **`feature-067` désigne deux specs
différentes** : `todo/feature-067-etude-portage-esp32-s3.md` et
`done/feature-067-offset-mesure-ph.md`. Toute référence « feature-067 » dans un commit ou un ADR
est donc **ambiguë**, et un contrôle par nom de fichier ne le voit pas. Rendre la règle bloquante
imposerait de **renuméroter une spec déjà close**, donc de réécrire de l'historique — ce qui coûte
plus que l'ambiguïté qu'on corrige. On le signale, on ne l'impose pas.

## Validation

- `./tools/check_specs.sh` → **0**, « 147 spec(s), aucun doublon », avertissement `feature-067`.
- Doublon fabriqué (`cp done/feature-102-*.md todo/`) → **1**, ligne
  `feature-102-retrait-repli-core-2.md → done todo`. Copie retirée, retour à **0**.
- Non-régression du fail-open : `TMPDIR=/nexistepas ./tools/check_specs.sh` → **1** (et non 0).
- Les deux YAML parsent sans erreur ; listes de chemins `push` / `pull_request` **identiques**.
- Aucun fichier de `src/`, `platformio.ini`, `partitions.csv` ni script de build touché.
  `FIRMWARE_VERSION` reste **2.42.1**.

⚠️ **Le comportement réel des deux workflows n'est vérifiable qu'au premier push.** Aucun runner
GitHub n'existe dans ce conteneur : ce qui est validé ici, c'est le **script** (exécuté) et la
**syntaxe** des workflows (analysée), pas leur exécution. À contrôler sur le run suivant :
déclenchement effectif, temps de build, et **création du cache `pio-fw-`** (un premier run sans
sauvegarde de cache signalerait précisément le défaut n° 1 ci-dessus).

## `CHANGELOG.md` — pas d'entrée (décision `doc-writer`, C4)

**Non.** Ce changelog est indexé sur les **versions de firmware livrées à l'appareil** : chaque
entrée s'ouvre sur ce que l'utilisateur doit faire pour mettre à jour. Ici, aucun comportement du
firmware ne change, aucun binaire n'est touché, `FIRMWARE_VERSION` reste `2.42.1`. Écrire sous
`[2.42.1]` serait faux — cette version est **publiée et datée**, et ces workflows n'existaient pas
quand elle est sortie ; ouvrir un `[Unreleased]` pour un ajout qu'aucun appareil n'exécutera
jamais serait du bruit. Le changement est documenté **là où il est actionnable** :
[`docs/BUILD.md` § Intégration continue](../../../docs/BUILD.md#intégration-continue-github-actions),
`README.md`, les deux bannières d'ADR, et cette spec. Il sera mentionné dans la prochaine entrée de
version **seulement s'il change quelque chose pour l'utilisateur** — ce qui n'est pas le cas.
Les entrées `CHANGELOG.md:17` et `:1106` sont **historiques** et n'ont pas été réécrites.

## Pas d'ADR

Aucun des quatre critères de déclenchement n'est rempli. Les choix (deux workflows séparés, script
plutôt que shell inline, filtre de chemins, clé de cache cloisonnée, règle 2 non bloquante) ne
contraignent aucune autre partie du système, ne verrouillent aucune orientation, et sont **lisibles
là où ils s'appliquent** : commentaires en tête des deux workflows et de `check_specs.sh`, plus la
section CI de `BUILD.md`. Le § « Choix de conception » de cette spec en tient le journal. Deux ADR
existants sont en revanche **amendés** (0069, 0042), leurs corps inchangés.

## Reliquat C2 et C3 — **appliqué le 2026-09-06 sur accord de l'utilisateur**

> Les deux retouches ci-dessous ont été posées dans `CLAUDE.md` et le skill `spec-workflow`.
> Texte conservé ici pour trace.

`CLAUDE.md` et `.claude/skills/` sont des fichiers de **configuration d'agent** : `doc-writer` ne
les modifie pas sur consigne d'un autre agent. Les deux retouches sont rédigées et prêtes :

**C2 — `CLAUDE.md`, § Workflow imposé, point 6 (ligne « CI GitHub Actions sur push/PR »)** —
remplacer cette fin de phrase par :

> CI GitHub Actions sur push/PR — **trois workflows** : `tests-natifs.yml` (tests + couverture),
> `build-firmware.yml` (**`pio run -e esp32dev_core3`** — la compilation est un gate depuis
> feature-103) et `hygiene-specs.yml` (**`./tools/check_specs.sh`**, unicité d'une spec entre
> `todo/` · `doing/` · `done/` — **lançable en local avant de committer**). Voir
> [`docs/BUILD.md`](docs/BUILD.md#intégration-continue-github-actions).

**C3 — `.claude/skills/spec-workflow/SKILL.md`, § « Promotion todo → doing »** — ajouter après le
bloc `mv` :

> ⚠️ **Déplacer, jamais copier** — et c'est désormais **vérifié par la CI**
> (`.github/workflows/hygiene-specs.yml`). Un nom de spec présent dans plusieurs des dossiers
> `todo/` · `doing/` · `done/` rend le run **rouge**. Contrôle lançable en local avant de
> committer :
>
> ```bash
> ./tools/check_specs.sh    # 0 si tout va bien, 1 sinon
> ```
>
> Le script signale aussi, **sans bloquer**, un même numéro porté par deux slugs différents
> (cas connu : `feature-067`). Voir [BUILD.md](../../../docs/BUILD.md#intégration-continue-github-actions).
