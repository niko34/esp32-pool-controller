# feature-118 — Authenticité de l'image OTA : de l'empreinte facultative à la signature

## Métadonnées
| | |
|---|---|
| Type | feature (sécurité) |
| Statut | en cours — **lot A livré** (empreinte envoyée systématiquement, verdict journalisé), **lot B non livré** (pas encore de refus sur absente/non conforme), **aucun commit** sur ce lot à la date de cette note |
| Priorité | Haute — `bug-117` est **close** (`specs/features/done/`), le prérequis est levé |
| Version cible | 2.66.0 (`src/version.h`, lot A) |
| Origine | 4ᵉ critère d'acceptation de `bug-117`, délibérément **non traité** dans son correctif |
| Specs liées | `bug-117` (la garde d'authentification), `feature-026` (vérification d'intégrité OTA), [`feature-151`](../todo/feature-151-signature-de-l-image-ota.md) (suivi : signature à clé publique, seule réponse complète à la règle n° 6) |

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`), `ota_update.sh`
- [x] Frontend (`data/app.js`) — page d'upload
- [ ] Régulation — [ ] Capteurs — [ ] MQTT

## Agents requis
- [x] architect · [x] embedded-developer · [x] web-ui-developer · [x] code-reviewer · [x] doc-writer
- [x] `security-reviewer` — **deux passages obligatoires**, verdict **contraignant**
      (GO / GO SOUS CONDITIONS / NO GO) à chaque passage :
      - **P1** sur le **design** (les trois issues ci-dessous), **avant** tout code —
        vérifie que l'arbitrage retenu répond réellement à la règle de sécurité n° 6
        et ne se fait pas passer pour une signature s'il n'en est pas une ;
      - **P2** sur le **diff**, **après** implémentation — `CLAUDE.md` rend ce passage
        obligatoire pour toute modification de `src/web_routes_ota.cpp`.
- [ ] `pool-chemistry` — sans objet directement, mais l'**impact** est total : un firmware
      arbitraire contrôle les pompes.

## Description
`POST /update` accepte une image **sans empreinte** : la comparaison SHA-256 est
conditionnée à `g_uploadHasExpected`, donc au bon vouloir de l'appelant. Un upload
authentifié mais non accompagné d'empreinte est flashé sans qu'aucune vérification de
contenu n'ait eu lieu.

Incohérence à corriger : **`/download-update` rend l'empreinte obligatoire** (fail-closed).
Deux routes qui écrivent la même partition n'ont pas la même exigence.

> ⚠️ **Correction (établie par l'audit `security-reviewer` P1, 2026-10-10) : cette
> incohérence est plus étroite qu'écrite ci-dessus.** Le paramètre `digest` de
> `/download-update` est **lui aussi fourni par l'appelant** (relayé depuis `/check-update`,
> une donnée GitHub non signée), et `isUrlAllowed()` ne contrôle que l'**hôte** — n'importe
> quel dépôt du domaine `github.com` passe. **Les deux routes sont au même niveau de
> preuve** : aucune des deux n'authentifie l'origine de l'image, seule `/download-update`
> **exige** la présence du paramètre. Ce que ce lot aligne, c'est l'**obligation**, pas la
> **nature** de la preuve. Voir le verdict complet ci-dessous et
> [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md).

## Ce que `CLAUDE.md` demande, et ce qui est réellement fait
Règle de sécurité absolue n° 6 : « OTA : toujours vérifier la signature/CRC avant flash ».
L'implémentation vérifie un condensat **que l'appelant fournit lui-même**. Cela prouve
l'**intégrité du transport** — l'image reçue est celle envoyée — mais **rien** sur son
**authenticité** : qui l'a produite.

**Formulation à ne pas diluer** (verdict `code-reviewer`, 07/09) : rendre l'empreinte
obligatoire est la bonne **étape intermédiaire**, mais il ne faut **pas l'appeler
sécurité**. Le vrai correctif est une **signature** — clé publique embarquée, ou
`esp_secure_boot`.

## Le point dur, trouvé en revue : le navigateur ne peut pas calculer l'empreinte
`ota_update.sh` a le `.bin` en local et peut calculer le SHA-256 trivialement.
**La page web d'upload, non** : `crypto.subtle` n'est disponible que dans un
**contexte sécurisé** (HTTPS ou `localhost`). L'appareil sert du **HTTP en clair sur une
IP de réseau local** — `crypto.subtle` y est donc **indisponible**.

Rendre l'empreinte obligatoire **casserait la page d'upload** (`data/app.js:8947-9045`,
bloc `FormData` avec `update_type` + `update_file`, `xhr.open("POST", "/update")`), à
moins d'embarquer une implémentation SHA-256 en JavaScript.

⚠️ **L'argument de poids FS contre cette issue est faible, mesuré le 2026-10-10** :
charge utile réelle de `data-build/` (après `node minify.js`) = **198 447 octets
(~193,8 Kio)**, sur une partition `spiffs` de 512 Kio (524 288 octets, layout v5,
ADR-0068) — soit **~325 841 octets (~318,2 Kio) encore libres**. Une implémentation
SHA-256 en JS pèse quelques kilo-octets, pas des dizaines : le poids FS ne justifie
pas, à lui seul, d'écarter l'issue 1. L'arbitrage entre les trois issues doit donc
reposer sur ce qu'elles **prouvent réellement**, pas sur une contrainte de partition
qui n'est plus la bonne depuis le layout v5.

**Trois issues à arbitrer, aucune gratuite :**
1. Empreinte obligatoire + SHA-256 en JS embarqué dans `data/` (coût : poids FS).
2. Empreinte obligatoire **sauf** pour la page web, qui reste dispensée (coût : le
   fail-closed a un trou nommé, ce qui est au moins honnête et documenté).
3. Signature à clé publique, et l'empreinte devient sans objet (coût : le plus élevé,
   la valeur la plus haute — et c'est la seule qui réponde vraiment à la règle n° 6).


## ✅ VERDICT `security-reviewer` P1 (DESIGN) — GO SOUS CONDITIONS, 2026-10-10

### Deux constats qui RECADRENT la feature
1. **Pour le FIRMWARE, l'IDF vérifie déjà l'image à l'activation** (`Update.end(true)` →
   `_verifyEnd()` → `esp_ota_set_boot_partition()`, qui renvoie `ESP_ERR_OTA_VALIDATE_FAILED`
   sur une image invalide). Une image tronquée est **déjà refusée sans empreinte**. La valeur
   ajoutée sur `U_FLASH` est **« le bon fichier »**, pas « un fichier intact ».
2. **Pour le FILESYSTEM (`U_SPIFFS`), RIEN d'autre ne vérifie** — l'empreinte y est la **seule**
   vérification d'intégrité. **C'est là qu'est le gain réel.**
3. **L'incohérence dénoncée par la spec est plus étroite qu'écrit** : le `digest` de
   `/download-update` est **lui aussi fourni par l'appelant**, et `isUrlAllowed()` ne contrôle
   que l'**hôte** — n'importe quel dépôt GitHub passe. Les deux routes sont au **même niveau de
   preuve**. Ce lot aligne l'**obligation**, pas la **nature**.
4. ⚠️ **Le rollback automatique est INERTE** : `initArduino()` marque l'image valide **avant
   `setup()`**. Il ne protège pas contre un firmware qui démarre mais dont `/update` refuse tout.

### LES NEUF CONDITIONS — contraignantes, re-vérifiées en P2
1. **Place et motif du refus** : bloc `index==0`, **après** la fin du pré-contrôle magique et
   **AVANT** `g_uploadOwner = request`, **hors** du `if (len >= 1)` (un fichier vide produit un
   unique appel `len==0, final==true`). Reproduire le motif de refus existant **à l'identique** :
   `markUploadRejected()` — **sans lui le lambda répond « OK » et planifie un redémarrage
   parasite** — puis `failOtaUpload(400, …)`, puis l'`onDisconnect` purgeant
   `g_uploadVerdictOwner`, puis `return`. ⛔ Aucun `setOtaInProgress(true)`, `LittleFS.end()`,
   `Update.begin()` ni affectation de `g_uploadOwner` atteignable sur ce chemin.
2. **Prédicat PUR extrait** (ADR-0017) : `(présent, chaîne) → {absente, malformée, conforme,
   non conforme}`, suite `test_native_*` avec **mutation prouvée** (vide, préfixe seul, 63 hex,
   65 hex, majuscules, espace final, `\n` final).
3. **Lot A = MÊME prédicat, verdict journalisé, ZÉRO régression.** Il appelle le prédicat de la
   condition 2 **à la même place** qu'en lot B et journalise « empreinte : … — lot B aurait
   refusé : oui/non — observation, non bloquant ». ⛔ Il **conserve** les refus actuels de
   feature-026 (malformée, non conforme) : les transformer en journalisation **reculerait**.
   Le refus « malformée » est déplacé à `index==0` **dès lot A**. Le passage A→B est alors le
   changement d'**une branche**, rien d'autre.
4. **Ordre de déploiement**, consigné : lot A par le **script** (`sha256` **avant** le fichier),
   firmware **ET** filesystem ; puis page testée sur **ordinateur ET téléphone**, pour les deux
   types d'image ; le journal doit montrer « conforme » **pour les quatre cas** avant d'ouvrir
   lot B. Lot B : FS puis firmware **par le script, jamais par la page** ; page vérifiée ensuite
   après **rechargement forcé** (cache navigateur).
5. **Vecteurs de test du SHA-256 JS versionnés** dans le dépôt (pas le scratchpad), exécutables
   par `node` : chaîne vide, « abc », messages de 55/56/64/65 octets, un bloc > 1 Mo.
6. **Formulation de la doc**, au sens près : « L'empreinte est calculée par l'appelant : elle
   prouve que l'image écrite est celle qu'il a envoyée, **pas qui l'a produite**. Pour le
   firmware, l'IDF vérifie déjà l'image à l'activation ; l'empreinte ajoute la détection du
   mauvais fichier. Pour le filesystem, elle est la **seule** vérification d'intégrité. » Et :
   « **La règle n° 6 de `CLAUDE.md` n'est PAS satisfaite par ce lot** ; elle le sera par la
   signature (spec de suivi). » ⛔ Le mot **« sécurité » n'apparaît pas** pour qualifier ce
   mécanisme — le terme est **« intégrité du transfert »**.
7. **ADR-0081 relu dans ce lot** : ligne datée disant que ce lot ne livre **pas** l'authenticité,
   que l'argument du plafond OTA tient, et déclencheur « à réévaluer quand… » **déplacé vers la
   spec de suivi**. Corriger aussi son libellé `todo/` au chemin `doing/`.
8. **Messages d'erreur** : le refus « absente » dit explicitement que le champ `sha256` doit
   **PRÉCÉDER** la partie fichier. Script et page affichent le corps `FAIL: …` tel quel.
9. **Spec de suivi créée AVANT la clôture** : signature à clé publique, **plancher de version
   anti-retour en arrière** dans son périmètre, référencée depuis cette spec et depuis ADR-0081.

### Specs de durcissement à ouvrir (hors périmètre, signalées par l'audit)
Signature + plancher de version · **`/download-update` : restreindre l'allowlist au dépôt du
projet** (une ligne, ferme un chemin équivalent) · bug-112 · robustesse multipart · `CRITICAL`
réel · jeton seul pour les routes critiques tant que le transport est en clair.

### Cosmétique relevé
Double champ `sha256` ⇒ la **première** occurrence est retenue (déterministe, à documenter) ·
`sha256sum` n'existe pas sur macOS (`shasum -a 256`) si le script doit rester portable.


## ✅ OBSERVATIONS ON-TARGET — 2026-10-11 (condition 2 du P2, levée)

Relevées dans le journal persistant de l'appareil (v2.66.0), lignes citées **textuellement** :

| Cas | Horodatage | Ligne |
|---|---|---|
| Script → **firmware** | 09:24:48 | `Empreinte upload OTA : conforme — lot B aurait refusé : non` |
| Script → **filesystem** | 09:21:52 · 09:23:41 | `… : conforme — lot B aurait refusé : non` |
| **Page web** → filesystem | 09:34:13 | `… : conforme — lot B aurait refusé : non` |
| **Page web** → firmware | 09:35:49 | `… : conforme — lot B aurait refusé : non` |
| **Envoi malformé** (`sha256=pasuneempreinte`) | 09:25:07 | `… : malformée — lot B aurait refusé : oui` puis `Mise à jour OTA refusée : empreinte SHA-256 fournie invalide — flash refusé` |

**Preuve du placement de la garde, sur matériel réel** : après le refus de 09:25:07, la ligne
`Début mise à jour OTA` **n'apparaît pas** (comptage à 0 sur le journal persistant). Le refus
intervient donc bien **avant la prise de propriété** et avant tout effet de bord sur la
partition — ce que la mutation m4 prouvait en théorie est constaté en production.
Empreinte reçue à 09:35:49 = `11d4147d…`, identique au `sha256sum` local du `firmware.bin` :
le calcul **JavaScript pur** du navigateur est exact (pas de `crypto.subtle`, indisponible en
HTTP clair).

### 🚨 TROUVAILLE DE L'ÉTAPE D'OBSERVATION — à traiter DANS le lot B
```
2026-10-11T09:33:17 INFO [feature-118 lot A] Empreinte upload OTA : absente — lot B aurait refusé : oui
```
Premier envoi depuis le navigateur : **aucune empreinte**. Cause : la page était **en cache**,
chargée AVANT le déploiement du nouveau `data/app.js`. L'envoi a redémarré l'appareil, la page
s'est rechargée, et l'envoi suivant (09:34:13) portait l'empreinte.

⇒ **C'est exactement le risque que l'étape d'observation existait pour découvrir.** Avec un
durcissement livré directement, l'utilisateur aurait vu son premier envoi **refusé** sans
comprendre — et le message prévu (« le champ `sha256` doit précéder la partie fichier »)
n'aide en rien quelqu'un dont la page ne l'envoie **pas du tout**.

**À inscrire au périmètre du lot B** :
1. Le message de refus « absente » doit couvrir **les deux** causes — champ placé après le
   fichier **et** page obsolète en cache — et dire explicitement de **recharger la page**
   (`Ctrl+Maj+R` / vider le cache du site).
2. Évaluer une parade structurelle : en-têtes de cache sur `app.js`, ou numéro de version de la
   page envoyé avec l'upload pour détecter l'obsolescence côté firmware.
⚠️ Sans ce point, le lot B est livrable mais **hostile au premier usage** — chaque déploiement
de FS créant une fenêtre où la page en cache est refusée.

## Comportement attendu
À trancher par l'`architect` sur les trois issues, avec le coût de chacune chiffré —
poids FS, complexité, et ce que chacune **prouve réellement**.

**Issue retenue (lot A livré) : issue 1, empreinte obligatoire à terme + SHA-256 JS embarqué.**
Détail complet de l'arbitrage et des coûts mesurés : [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md).

## ✅ VERDICT `security-reviewer` P2 (DIFF), lot A — GO, conditions reportées au lot B

Passage sur le diff livré du lot A (code non commité à la date de cette note). Les 9
conditions du P1 ci-dessus sont reprises une à une ; celles qui concernent le lot B restent
**ouvertes**, consignées pour ne pas être reperdues à l'ouverture de ce lot.

| # | Condition (résumé) | État lot A |
|---|---|---|
| 1 | Place et motif du refus « malformée » | **Livré** — `index==0`, hors du `if (len >= 1)`, avant `g_uploadOwner = request`, `markUploadRejected()` + `failOtaUpload()` + `onDisconnect` repris à l'identique. Une **garde structurelle anti-régression** a été ajoutée après une première relecture (`test/test_native_ota_order_guard/`) : elle a d'abord révélé que les **vecteurs JS** (`test/sha256_vectors.js`) **recopiaient** l'implémentation au lieu de l'**extraire** de `data/app.js` — corrigé avant cette note (extraction par ancre + comptage de profondeur, patron `test_native_ws_config_parity`) |
| 2 | Prédicat pur + mutation | **Livré** — `parseOtaSha256Param`/`otaUploadIntegrityVerdict` dans `src/ota_integrity_logic.h`, cas vide/préfixe seul/63-65 hex/majuscules/espace/`\n` couverts (`test/test_native_ota_sha256_param/`) |
| 3 | Lot A = même prédicat, verdict journalisé, zéro régression | **Livré** — refus « malformée » et « non conforme » existants **conservés** (le second n'atteint plus ce point que via `g_uploadHasExpected`, inchangé en comportement) ; `logOtaUploadIntegrityVerdict()` journalise sans introduire de refus nouveau |
| 4 | Ordre de déploiement (script avant page, 4 cas « conforme ») | **OUVERT — observation on-target due avant le lot B**, voir ci-dessous |
| 5 | Vecteurs SHA-256 JS versionnés | **Livré** — `test/sha256_vectors.js`, exécuté par `tools/check_sha256_vectors.sh`, **vecteurs vérifiés le 2026-10-10** : chaîne vide, « abc », 55/56/64/65 octets, bloc > 1 Mo entier et fragmenté — tous conformes à `node:crypto` |
| 6 | Formulation de la doc (intégrité du transfert, pas sécurité) | **Livré par ce tour** — `docs/API.md`, `docs/subsystems/ota-manager.md`, [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md) portent les trois phrases et le constat de non-satisfaction de la règle n° 6 |
| 7 | ADR-0081 relu | **Livré par ce tour** — ligne datée du 2026-10-10, déclencheur déplacé vers `feature-151` |
| 8 | Messages d'erreur (ordre des champs) | **Livré** — `kOtaSha256MissingMessage` préparé pour le lot B (pas encore utilisé) ; message « malformée » explicite ; `ota_update.sh` affiche un indice ciblé (`[O3]`) sur un refus `400` mentionnant sha256/empreinte |
| 9 | Spec de suivi créée avant clôture | **Livré par ce tour** — `specs/features/todo/feature-151-signature-de-l-image-ota.md` |

**Zéro régression confirmée** : `pio test -e native` — **65 suites / 2 152 cas, tous verts**
(mesuré le 2026-10-10, ne pas recopier ce chiffre aux cycles suivants — ADR-0073, le
remesurer avec `ls -d test/test_native_* | wc -l` + dernière ligne de `pio test -e native`).
Mutation prouvée sur le bourrage SHA-256 JS : **25/25 vecteurs échouent** si l'implémentation
extraite est altérée. 5 mutations tuées sur le prédicat pur ajouté.

### Observations on-target encore dues AVANT d'ouvrir le lot B (condition 4)
Aucune n'est levée à la date de cette note (aucun commit, aucun déploiement) :
1. Déployer le lot A par le **script** (`ota_update.sh`), firmware **ET** filesystem.
2. Vérifier la page web sur **ordinateur ET téléphone**, pour les **deux** types d'image,
   après rechargement forcé du cache.
3. Le journal du contrôleur doit afficher `… conforme … lot B aurait refusé : non` pour les
   **quatre** cas (script firmware, script FS, page firmware, page FS) avant toute ouverture
   du lot B.
4. Lot B, une fois ouvert : redéployer FS puis firmware **par le script, jamais par la
   page** ; vérifier la page **ensuite**, après rechargement forcé.

## Contraintes techniques
- ⚠️ **L'OTA est le seul canal de déploiement** (pas de port série, conteneur LXC). Toute
  exigence nouvelle qui casserait `ota_update.sh` ou la page web rendrait l'appareil non
  déployable, **sans repli**. C'est la contrainte dominante, avant toute considération de
  pureté cryptographique.
- Le déploiement de tout correctif de cette spec doit être suivi **immédiatement** d'un
  OTA de vérification, pour découvrir un blocage à la seconde et non à la prochaine
  livraison.

## Critères d'acceptation
- [ ] Une image sans preuve d'origine acceptable est **refusée** avant tout `Update.begin()`
      — **lot B seulement** ; lot A journalise sans refuser (sauf malformée)
- [ ] `ota_update.sh` fonctionne — **prouvé par un déploiement réel** — **observation
      on-target due**, voir le verdict P2 ci-dessus
- [ ] La page web d'upload fonctionne, ou son exemption est **explicite et documentée** —
      **observation on-target due** (ordinateur et téléphone) ; l'exemption a été
      **tranchée par le refus** (issue 2 écartée, voir [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)), aucune route web n'est exemptée
- [x] La documentation dit ce que le mécanisme prouve **et ce qu'il ne prouve pas** :
      transport contre authenticité. Ne pas laisser croire à une signature ce qui n'en est pas une.
      (`docs/API.md`, `docs/subsystems/ota-manager.md`, ADR-0096, `docs/UPDATE_GUIDE.md`)
- [x] ADR : le choix entre les trois issues verrouille un futur, il en relève
      ([ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md))

## Hors périmètre
- La garde d'authentification de l'upload : c'est `bug-117`, livrée en v2.46.2.
