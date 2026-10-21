## Métadonnées
- **ID** : feature-151
- **Slug** : signature-de-l-image-ota
- **Type** : feature (sécurité)
- **Statut** : doing — démarrée le 2026-10-18
- **Priorité** : haute — c'est le seul mécanisme qui satisfait réellement la règle de sécurité n° 6 de `CLAUDE.md`
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-10
- **Spec(s) liée(s)** : [`feature-118`](../done/feature-118-authenticite-de-l-image-ota.md) (empreinte SHA-256, lot A livré — journalise l'intégrité du **transfert**, ne livre PAS l'authenticité d'origine), [`bug-117`](../done/bug-117-upload-ota-sans-verification-d-authentification.md) (garde d'authentification de l'upload)

## Domaines touchés
- [x] OTA (`src/web_routes_ota.cpp`, `src/ota_manager.*`), `ota_update.sh`
- [x] Frontend (`data/app.js`) — page d'upload — **voir impasse ci-dessous, à trancher en premier**
- [ ] Régulation — [ ] Capteurs — [ ] MQTT

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] web-ui-developer
- [x] `security-reviewer` — **deux passages obligatoires, verdict contraignant**, même
      protocole que `feature-118` : P1 sur le design (schéma de clé, format de signature,
      emplacement de vérification, plancher de version) **avant** tout code ; P2 sur le
      diff, après implémentation.
- [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` — sans objet directement, mais l'**impact** est total, comme pour
      `feature-118` : un firmware arbitraire contrôle les pompes doseuses.

## Description
`feature-118` a établi qu'une empreinte SHA-256 **fournie par l'appelant** ne prouve que
l'intégrité du transfert, jamais l'authenticité de l'origine — la règle de sécurité n° 6 de
`CLAUDE.md` (« toujours vérifier la signature/CRC avant flash ») reste donc non satisfaite.
Cette spec livre le mécanisme qui la satisfait réellement : une **signature à clé publique**
vérifiée par le firmware avant toute activation d'image, assortie d'un **plancher de version**
qui interdit le retour en arrière vers une image plus ancienne (donc potentiellement
vulnérable) même si elle était signée légitimement en son temps.

## Contexte
Voir [ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md) pour
l'arbitrage complet des trois issues envisagées par `feature-118` (empreinte + JS embarqué,
retenue pour ce lot-là ; exemption de la page web, écartée ; signature, reportée ici). Voir
aussi [ADR-0081](../../docs/adr/0081-recensement-du-tas-et-non-livraison-de-la-lecture-de-contenu.md),
dont le régime de sécurité R2 (recensement du tas, compilé inconditionnellement) reste
conditionné à l'absence d'authenticité OTA **jusqu'à la livraison de cette spec** — pas
jusqu'à celle de `feature-118` lot A/B, qui ne change pas la nature de la preuve.

## Comportement attendu
- Le firmware embarque une **clé publique** (format à trancher par `architect` +
  `security-reviewer` P1 — ed25519 ou RSA, taille, emplacement en flash) utilisée pour
  vérifier une signature accompagnant chaque image.
- La vérification a lieu **avant `Update.end()`** (jamais après l'activation), sur **les deux**
  routes qui écrivent une partition applicative ou filesystem : `POST /update` **et**
  `POST /download-update`. Une image non signée, ou signée par une clé inconnue, est
  **refusée sans effet de bord** — même exigence de place de la garde que `feature-118`
  (condition 1 de son audit `security-reviewer`) : la vérification doit précéder toute
  prise de propriété de la session d'upload et tout `Update.begin()`.
- Un **plancher de version anti-retour en arrière** empêche de flasher une image dont le
  numéro de version (`src/version.h`) est **inférieur** à une version plancher persistée
  (NVS), y compris si cette image porte une signature valide — une ancienne version
  légitimement signée mais depuis reconnue vulnérable ne doit pas redevenir installable.
  Mécanisme de relèvement du plancher à définir (champ de configuration ? relevé
  automatiquement à chaque flash réussi ? jamais abaissé sans geste explicite).
- `ota_update.sh` doit produire ou accepter un fichier de signature (`.sig` ou embarqué en
  en-tête du `.bin`, à trancher) et le transmettre à la route.

### Cas limites
- Image firmware valide, signée, mais version inférieure au plancher → refusée, cause
  explicite (« version antérieure au plancher de sécurité X.Y.Z »), pas confondue avec un
  échec de signature.
- Signature présente mais clé de signature différente de celle embarquée → refusée, même
  famille de message que « sha256 non conforme » côté `feature-118` (ne pas dupliquer un
  vocabulaire différent pour un même type de refus).
- Panne du mécanisme de signature côté outillage (`ota_update.sh` ne trouve pas d'outil de
  signature) → échec **explicite à l'écran/console**, jamais un envoi sans signature qui
  se ferait accepter silencieusement.

## ⚠️ Impasse connue à trancher en premier — la page web ne peut pas signer
Exactement le point dur déjà rencontré par `feature-118` (la page web ne peut pas calculer
nativement certaines primitives cryptographiques dans de bonnes conditions), mais **plus
sérieux ici** : signer exige la **clé privée**, qui ne doit **jamais** vivre dans une page
servie en HTTP clair sur une IP de LAN — la embarquer dans `data/app.js` reviendrait à la
publier à quiconque ouvre la page, ce qui annule tout l'intérêt de la signature. Deux
options, aucune gratuite, à arbitrer **avant tout code** (passage `security-reviewer` P1) :

1. **La page d'upload téléverse un `.bin` + un fichier `.sig` séparé**, signé **en amont**
   sur un poste de confiance (celui qui construit la release), jamais par le navigateur.
   Coût : un geste manuel de plus pour l'utilisateur (récupérer les deux fichiers), mais
   aucune clé privée exposée.
2. **Exemption de la page web** pour la signature (elle resterait sur le mécanisme
   `feature-118`, empreinte seule) — **ramène exactement au dilemme déjà arbitré par
   `feature-118`/[ADR-0096](../../docs/adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)** :
   le firmware ne peut **structurellement pas** distinguer la page officielle d'un `curl`
   qui imiterait ses champs — une exemption serait universellement invocable. **Ne pas
   redécouvrir ce point en cours d'implémentation : il est connu dès l'écriture de cette
   spec.**

## `esp_secure_boot` v2 — à évaluer séparément, pas dans cette spec
Noté ici pour ne pas le perdre, **hors périmètre** de cette spec : `esp_secure_boot` v2
apporterait une vérification de signature au niveau du bootloader, mais il est
**irréversible** une fois activé, exige un **accès USB** et la consommation d'**eFuses** —
donc **incompatible en l'état** avec la contrainte dominante du projet, « l'OTA est le seul
canal de déploiement » (pas de port série en environnement de développement, conteneur
LXC). Une spec séparée devra, le cas échéant, documenter comment lever cette incompatibilité
(ou l'accepter pour un sous-ensemble de la flotte) avant toute activation.

## Contraintes techniques
- ⚠️ **L'OTA est le seul canal de déploiement** (pas de port série, conteneur LXC). Toute
  exigence nouvelle qui casserait `ota_update.sh` ou la page web rendrait l'appareil non
  déployable, **sans repli**. Même contrainte dominante que `feature-118`.
- Taille de clé publique et de signature à budgéter sur la partition `spiffs` (marge connue
  à la date de cette spec : 324 181 o après `feature-118` lot A) et sur la flash applicative.
- Le déploiement du premier correctif de cette spec doit être suivi **immédiatement** d'un
  OTA de vérification complet (firmware et filesystem, script et page), pour découvrir un
  blocage à la seconde et non à la prochaine livraison — même précaution que `feature-118`.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — vérification de signature sur les deux routes d'écriture
- `src/ota_integrity_logic.h`/`.cpp` — ou nouveau module dédié à la signature (à trancher
  par `architect` : extension du module existant vs module séparé)
- `src/constants.h` — clé publique embarquée, constantes de plancher de version
- `ota_update.sh` — production/transmission du fichier de signature
- `data/app.js` — flux d'upload adapté selon l'option retenue pour l'impasse ci-dessus
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md` — documentation
  du nouveau contrat

## Nouvelles signatures / API
- À définir par `architect` : nouveau champ POST (`signature=`?) sur `/update` et
  `/download-update`, ou en-tête dédié
- Constantes : clé publique embarquée, plancher de version minimal

## Sécurité chimique
- Dosage impliqué : non directement — **impact total par ricochet** : un firmware
  arbitraire contrôle les pompes doseuses. Même raisonnement que `feature-118`.
- Risque identifié : une signature mal vérifiée (ex. comparaison non constante en temps,
  ou vérifiée après activation) donnerait une **fausse assurance** plus dangereuse que
  l'absence de mécanisme — passage `security-reviewer` P1 **non négociable** avant code.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contraint durablement le format d'image et le processus
  de release), 3 (alternatives crédibles : `.sig` séparé vs exemption page web vs
  `esp_secure_boot`), 4 (verrouille le contrat OTA pour le reste de la vie du projet)
- **Sujet pressenti** : choix du schéma de signature et de la réponse à l'impasse page web

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /update`, `POST /download-update` — contrat enrichi d'un paramètre de signature

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/ota-manager.md` — nouvelle section signature, succédant à la section
  lot A de `feature-118`

#### Docs transverses
- `docs/API.md` — nouveau paramètre, nouveaux codes de refus
- `docs/UPDATE_GUIDE.md` — procédure de récupération du fichier de signature
- `CHANGELOG.md` — entrée dédiée, section Firmware

#### Docs immuables
- `docs/adr/NNNN-<slug>.md` — décision de schéma de signature et de réponse à l'impasse
  page web (ADR attendu ci-dessus)

## Critères d'acceptation
- [ ] Une image non signée, ou signée par une clé inconnue, est **refusée** avant tout
      `Update.end()`, sur `/update` **et** `/download-update` — ⛔ **pas avant D3** : D1
      vérifie et journalise, ne refuse rien (c'est son objet)
- [ ] Une image signée mais de version inférieure au plancher est **refusée**, avec une
      cause distincte d'un échec de signature — ⛔ **pas avant D3** (plancher inerte à 0.0.0
      en D1)
- [ ] L'impasse « la page web ne peut pas signer » est tranchée **explicitement** (option 1
      ou 2 ci-dessus) et documentée, pas redécouverte en cours d'implémentation — **reportée
      explicitement** à un lot ultérieur en D1 (documenté dans `docs/UPDATE_GUIDE.md`,
      jamais redécouverte en silence), pas encore tranchée
- [ ] `ota_update.sh` fonctionne — prouvé par un déploiement réel, firmware et filesystem —
      **c'est l'objet de D2**, non encore mené à cette étape
- [x] La documentation dit ce que la signature prouve (authenticité d'origine) par
      opposition à ce que l'empreinte de `feature-118` prouvait seule (intégrité du
      transfert) — ne pas laisser les deux mécanismes se confondre dans le texte
- [x] ADR créé pour le schéma retenu — [ADR-0120](../../docs/adr/0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md)

### Notes techniques — lot D1 (2026-10-18)
Lot D1 livré : modules purs (`ota_manifest_logic`, `ota_base64_logic`, `ota_authenticity_logic`)
+ vérificateur mbedtls (`ota_signature`), clé publique embarquée, vérification posée à sa place
définitive sur les deux routes d'écriture — **JOURNALISE SEULEMENT, aucun refus**. `ota_update.sh`
signe chaque envoi et refuse d'en faire sans outillage. Documentation à jour :
[`docs/API.md`](../../docs/API.md#authenticite-manifest-signature-feature-151-d1),
[`docs/subsystems/ota-manager.md`](../../docs/subsystems/ota-manager.md#d1--authenticite-par-signature-journalise-seulement-feature-151-verdict-security-reviewer-p1),
[`docs/UPDATE_GUIDE.md`](../../docs/UPDATE_GUIDE.md#signature-des-envois-ota),
[ADR-0120](../../docs/adr/0120-signature-rsa-du-manifeste-ota-verifiee-avant-refus.md).
⛔ **Spec NON déplacée vers `done/`** : D2 (preuve terrain) à D5 restent à livrer avant que la
feature ne satisfasse réellement la règle de sécurité n° 6 de `CLAUDE.md`.

### 🔒 Les deux gardes — nommées et placées ici (condition 1 du verdict P1, levée dans la spec)

Jusqu'ici, la condition 1 (« nommer les DEUX gardes et leur place ») n'était levée que dans
`docs/API.md` — pas dans la spec elle-même. Les voici, explicitement :

- **Garde 1 — authenticité du manifeste** (`computeOtaAuthenticityVerdict`, verdict à sept issues,
  `OtaAuthenticityVerdict`, [`src/ota_authenticity_logic.h`](../../src/ota_authenticity_logic.h)).
  Place : **avant** toute prise de propriété de la session d'upload (`g_uploadOwner = request;`)
  et avant le pré-contrôle de l'octet magique sur `POST /update` ; **avant**
  `refuseIfTlsGateClosed` (donc avant toute session TLS sortante) sur `POST /download-update`.
  Prouve que le manifeste a été **signé par la bonne clé**.
- **Garde 2 — liaison manifeste ↔ image** (`otaManifestBindingVerdict`, verdict
  `kSkipped`/`kBound`/`kUnbound`, lot B1 ci-dessous,
  [`src/ota_manifest_binding_logic.h`](../../src/ota_manifest_binding_logic.h)). Place : **deux**
  points sur **chacune** des deux routes — à l'**arrivée** du manifeste (comparaison à l'empreinte
  déjà fournie par l'appelant, paramètre `sha256`/`digest`) **et** après le **calcul réel** de
  l'empreinte de l'image effectivement écrite en flash (`final` sur `/update`, fin de
  téléchargement sur `/download-update`). Prouve que **ce** manifeste décrit **cette** image
  précise — ce que la Garde 1 seule ne prouve pas.

⛔ **Les deux gardes journalisent seulement en D1 : aucune des deux ne refuse.**

### 🔴 Lot B1 (2026-10-18) — correction de revue : la signature ne liait rien à l'image

**Le défaut** : l'empreinte du manifeste était extraite, re-sérialisée, **signée**… puis **jetée**.
L'empreinte réellement comparée par le mécanisme d'intégrité (feature-118) restait celle fournie en
**paramètre** par l'appelant, jamais celle déclarée par le manifeste signé. ⚠️ **Scénario** : un
manifeste authentiquement signé pour une image **A**, envoyé avec une image **B** et un paramètre
`sha256`/`digest=<B>`, obtenait un verdict d'authenticité **« valide »**.

🔴 **Conséquence réelle, établie en revue** : D3 n'aurait **pas** pu se réduire à l'ajout d'un
simple `if` de refus — il aurait fallu remanier la structure de la décision pour y introduire cette
comparaison manquante. **La condition 10 du verdict P1** (« le diff D1→D3 ne touche QUE le
prédicat de décision ») **aurait été violée sans que rien ne le signale.**

**Correctif** : module pur `ota_manifest_binding_logic` (Garde 2 ci-dessus) — deux comparaisons sur
chacune des deux routes (quatre au total), voir le détail et le tableau des quatre points dans
[`docs/subsystems/ota-manager.md`](../../docs/subsystems/ota-manager.md#b1--la-signature-ne-liait-rien-a-limage-feature-151-correction-de-revue-du-2026-10-18).
⛔ **Toujours aucun refus** : D1 journalise.

**Garde structurelle supplémentaire** : deux tests (`[G5]`, `[G6]`, `test/test_native_ota_order_guard/`)
figent **textuellement** la place des deux blocs d'appel de la Garde 1. Raison : il y a **toute la
campagne terrain D2** entre D1 et D3 — D1 ne refusant rien, un refactor qui déplacerait un bloc
après la prise de propriété de session ne casserait **aucune** suite existante, et D3 greffrait
alors son refus au **mauvais endroit**. Numérotés `[G5]`/`[G6]`, pas `[G4]`/`[G5]` : un `[G4]`
existe déjà dans le même fichier depuis feature-118 lot B (sans rapport avec feature-151).

**Autres corrections du même lot** :
- **Diagnostic du piège du LF** : les octets canoniques re-sérialisés sont désormais journalisés
  sur tout verdict d'authenticité **rejeté** (aucun secret exposé, le manifeste est public) — un
  manifeste signé via `echo` (LF final) au lieu de `printf '%s'` produisait sinon un `kInvalid`
  indiscernable d'une mauvaise clé.
- **`ota_update.sh` annonce « 2 signatures ⇒ 2 saisies »** avant la première invite, en mode
  `both`. Fusionner les deux saisies a été **refusé** : cela exigerait de garder la clé privée
  déchiffrée en mémoire entre les deux signatures, ce qui romprait la propriété centrale de la
  décision B (ci-dessus).
- **La version inscrite dans le manifeste vient de `src/version.h` de l'arbre de travail**, pas du
  binaire réellement envoyé — écart assumé, mitigé par un `pio run` systématique avant envoi ;
  deviendra un champ de sécurité (plancher) en D5.
- **`.gitignore`** : motifs `*.pem`/`*.sig` restreints à la **racine** (`/*.pem`, `/*.sig`) — un
  motif global aurait silencieusement masqué une future ancre TLS `.pem` versionnée ailleurs dans
  l'arbre.
- **Commentaire corrigé** : `kValid` ne prouve que la signature du manifeste, jamais que l'image
  reçue est celle qu'il décrit — le commentaire du code et `docs/subsystems/ota-manager.md`
  affirmaient à tort l'inverse.

**Fichiers neufs** : `src/ota_manifest_binding_logic.h`/`.cpp`,
`test/test_native_ota_manifest_binding/`. Suites natives mesurées ce jour :
`ls -d test/test_native_* | wc -l` → 77 (ne pas recopier ce chiffre aux cycles suivants,
ADR-0073).

## Plan de test manuel
1. Image correctement signée, version supérieure au plancher → flash accepté
2. Image non signée → refusée avant tout effet de bord (pompes non inhibées, FS non démonté)
3. Image signée par une clé différente → refusée, message distinct de « sha256 non conforme »
4. Image signée valide mais version inférieure au plancher → refusée, message dédié
5. `ota_update.sh` de bout en bout sur un appareil réel, firmware et filesystem
6. Page web d'upload, selon l'option retenue pour l'impasse, sur ordinateur et téléphone

## Notes pour agent
- Ne pas répéter l'erreur relevée sur `feature-118` (condition 1 de son audit
  `security-reviewer`) : la place de la vérification compte autant que sa présence — vérifier
  qu'elle précède bien toute prise de propriété de session et tout `Update.begin()`, pas
  seulement qu'elle existe quelque part dans le handler.
- `esp_secure_boot` v2 est **hors périmètre** de cette spec (voir section dédiée) — ne pas
  le mélanger avec la signature applicative demandée ici.

## Hors périmètre
- `esp_secure_boot` v2 (bootloader) — spec séparée si jugé utile un jour
- Révocation de clé en cours de vie (rotation de la clé publique embarquée) — à évaluer
  seulement si un besoin concret apparaît


---

# Conception retenue + verdict `security-reviewer` **P1** (2026-10-18)

## ⭐ Q0 — L'ORDRE DE DÉPLOIEMENT QUI REND LE BLOCAGE IMPOSSIBLE
⚠️ **Principe** : *le chemin d'ACCEPTATION est prouvé sur le terrain AVANT que le chemin de
REFUS n'existe.* ⛔ La fonction de vérification est **rigoureusement la même** en D1 et D3 ;
seul le `if` de refus change.

| # | Contenu | Déployé par | ⚠️ Ce qu'on vérifie AVANT de passer au suivant |
|---|---|---|---|
| **D0** | Paire générée **hors dépôt**, clé publique dans `constants.h`. ⛔ Aucun déploiement. | — | Clé privée sauvegardée en **2 exemplaires hors-ligne** |
| **D1** | Vérifie et **JOURNALISE SEULEMENT**, ⛔ jamais de refus. Outillage **complet** dans le **MÊME** lot. | firmware actuel ⇒ ⛔ **risque nul** | — |
| **D2** | **Preuve terrain** : 4 combinaisons (script × page) × (firmware × FS) + **2 essais négatifs**. | D1 | ⛔ Tant que ce n'est pas vert, **D3 n'est pas écrit** |
| **D3** | Refus **activé**. Plancher **inerte à 0.0.0**. Interrupteur de secours. | D1 ⇒ ⛔ ne peut pas échouer sur la signature | — |
| **D4** | Bump sans effet, firmware + FS, script **et** page. ⭐ **Premier déploiement réellement filtré.** | D3 | 4 déploiements acceptés |
| **D5** | Relèvement du plancher (geste explicite). | — | D4 vert |

## Décisions
- **Q1 — RSA-3072, PKCS#1 v1.5 sur SHA-256.** ⚠️ **Mesuré dans l'ELF** : `mbedtls_pk_verify`,
  `pk_parse_public_key`, `rsa_pkcs1_verify` **présents** ; `HARDWARE_MPI`/`HARDWARE_SHA` actifs.
  ⛔ **Ed25519 ABSENT** de mbedtls (seules les tables X25519 existent) ⇒ exigerait une dépendance
  neuve **sur le chemin OTA**. Signature **384 o**, flash **~0**.
- **Q2 — Fichier `.sig` SÉPARÉ**, ⛔ pas d'en-tête (changerait le format d'image et l'octet
  magique `0xE9`). ⭐ **La signature porte sur un MANIFESTE d'une ligne**, ⛔ pas sur les 1,5 Mo :
  `pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex>`
- **Q4 — Plancher : GESTE EXPLICITE uniquement.** ⛔ Pas de relèvement automatique — ce serait un
  **second** mécanisme de blocage définitif, qui interdirait le retour dont on a besoin pour
  sortir d'un firmware défectueux.
- **Q5 — Clé privée hors dépôt**, chiffrée, 2 copies hors-ligne. ⛔ `ota_update.sh` **refuse**
  d'envoyer sans elle.

## 🔴 CE QUE CETTE FEATURE PROTÈGE — ET CE QU'ELLE NE PROTÈGE PAS
⭐ **Gain réel** : contre la **chaîne GitHub/TLS** — compte ou dépôt compromis, jeton `gh` ou CI
fuité, CDN, autorité de certification. ⚠️ **Tous obtiennent un `digest` conforme GRATUITEMENT**
(GitHub le calcule à l'upload) ; ⛔ **aucun ne peut produire la signature.** Plus le **retour en
arrière** vers une version vulnérable.

🔴 **CE QU'ELLE NE FERME PAS : le vecteur « jeton admin volé ».** ⚠️ L'argument « l'interrupteur
n'ouvre rien de neuf » est **vrai aujourd'hui et FAUX dès D3** : ce que D3 apporte, c'est
justement de **retirer au porteur du jeton l'exécution de code arbitraire** — et l'interrupteur
la lui **rend**, en deux requêtes au lieu d'une.
⛔ **Ne JAMAIS présenter cette feature comme fermant ce vecteur.**

## Les 4 conflits — tranchés
- **C1** (FS effacé avant écriture) : le critère vise le refus d'**authenticité**, satisfait.
  Une image authentique au flux divergent détruit le FS — ⚠️ **préexistant (feature-026), non
  aggravé**, et **récupérable** par `ota_update.sh filesystem` (vérifié : l'appareil démarre et
  sert les routes OTA **même si LittleFS ne monte pas**). ⇒ Durcissement n°1.
- **C2** : découpage **validé** — authenticité avant toute propriété ; liaison au `final`.
- **C3** : interrupteur **ACCEPTÉ**, c'est le **seul parachute à distance** contre une clé perdue
  **et contre un défaut du vérificateur** découvert à D4 (⚠️ le bouton physique est au bord du
  bassin, pas dans le LXC). Encadré par les conditions 5-8.
- **C4** : abaissement du plancher **accepté** — ⚠️ `/factory-reset` le remet **de toute façon** à
  0.0.0 par HTTP ; l'interdire serait du théâtre.

## Conditions bloquantes (11) — ⛔ à lever avant ou en P2
1. **Nommer les DEUX gardes** et leur place dans la spec.
2. 🔴 **Le genre signé est L'AUTORITÉ** qui choisit la partition. ⚠️ Aujourd'hui `/update` la
   choisit par `update_type`/nom de fichier et `/download-update` par une **sous-chaîne d'URL** :
   ⛔ un manifeste `firmware` avec un appelant `filesystem` écrirait une image firmware **dans la
   partition FS**.
3. 🔴 **UNE SEULE source d'empreinte : le manifeste.** ⛔ Sinon la signature **ne lie plus rien**.
4. **Grammaire stricte + RE-SÉRIALISATION** par l'appareil depuis les champs parsés.
   ⚠️ Ambiguïtés réelles : LF final (`echo` vs `printf` — ⭐ **l'échec typique d'outillage que D2
   doit révéler**), espaces multiples, préfixe `v`, zéros de tête, casse de l'hex.
5. **ADR : écrire que le vecteur « jeton volé » reste ouvert.**
6. **Absence de clé NVS = signature EXIGÉE** ⇒ `/factory-reset` et le bouton **ré-arment**.
7. **Interrupteur BORNÉ** : expire au premier flash réussi **ou** 60 min (horodatage NVS,
   ⛔ pas `millis()`), relu à **chaque** `index==0`, publié **MQTT** (⭐ le seul témoin qui
   survit à un firmware hostile qui purge le journal).
8. Les deux endpoints d'administration : ⛔ **sans** `onBody`/`onUpload`, `REQUIRE_AUTH(CRITICAL)`
   **en tête** (⚠️ motif d'échec documenté des trois défauts du 07/09).
9. **Verdicts distincts**, dont ⭐ **« vérification impossible »** — ⛔ sinon D2/D4 ne sauront pas
   si c'est l'outillage, la clé ou le tas.
10. **D1→D3 : le diff ne touche QUE le prédicat de décision.** Vérificateur, clé, grammaire :
    **octet pour octet identiques**.
11. Clé **chiffrée**, 0600, hors dépôt ; `/release` attache les `.sig` et
    `check_release_artefacts.sh` **refuse** une release sans eux.

## ⚠️ Points établis par l'audit, à ne pas re-chercher
- ⛔ Le descripteur d'application du binaire porte un **hash git**, ⛔ pas la version SemVer ⇒
  **aucune contre-vérification** possible, le manifeste est la seule source.
- ⛔ **Temps constant sans objet** pour une vérification à **clé publique** (aucun secret côté
  vérificateur). ⚠️ Ne pas inventer cette contrainte.
- `/factory-reset` **efface toute la NVS** ⇒ plancher et interrupteur remis à zéro.

## Durcissements à ouvrir ensuite (ordre imposé)
1. **Mise en attente PSRAM de l'image FS** ⇒ ferme C1 pour de bon.
2. **Fenêtre de secours par bouton PHYSIQUE** ⇒ ⭐ seule façon d'obtenir « jeton ≠ code
   arbitraire ». ⛔ À n'ouvrir qu'**après D5**.
3. **bug-112** (journal purgeable sans authentification) — ⚠️ il conditionne la valeur de la
   trace de l'interrupteur. ⇒ **Avant** le durcissement n°2.


---

## 🔑 D0 — FAIT le 2026-10-18 : la paire existe

- **Générée par l'utilisateur, sur son poste**, ⛔ jamais dans le dépôt.
- **RSA 3072 bits**, vérifié : `Public-Key: (3072 bit)`, **422 o** en DER — conforme au budget.
- **Clé privée chiffrée** (`-aes-256-cbc`), phrase de passe connue de l'utilisateur **seul**.
  ⚠️ Signature de bout en bout **éprouvée par lui** (`openssl dgst -sign` puis `-verify` ⇒
  `Verified OK`), avec `printf '%s'` ⛔ et non `echo` — ⭐ le piège du LF final identifié par
  l'audit a donc **déjà été évité** à la main.
- ⭐ **Empreinte SHA-256 de la clé publique (DER)** — ⚠️ sert à vérifier que c'est **bien
  celle-ci** qui est embarquée dans un firmware donné :
  `844449550704da06b085d24a8ae2b079a349730d520e61824dc2816956a57ebb`
- Clé publique déposée hors dépôt : `/root/.pool-ota/signing_key.pub.pem`.

## 🔴 DÉCISION DE FONCTIONNEMENT (utilisateur, 2026-10-18) — **option B**

⚠️ **Le vrai changement n'est pas la release, c'est CHAQUE DÉPLOIEMENT.** Les releases sortent
une fois par semaine ; `ota_update.sh` tourne plusieurs fois par jour. ⇒ Après D3, **tout envoi**
exige la clé **et** la phrase de passe.

**Retenu : la clé privée CHIFFRÉE vit dans le conteneur** (`$OTA_SIGNING_KEY`, défaut
`~/.pool-ota/signing_key.pem`, `0600`), et **la phrase de passe est demandée à chaque envoi**.

⚠️ *Pourquoi, et ce que ça coûte* :
- ⭐ Le vrai secret est la **phrase de passe**, et elle reste **dans la tête de l'utilisateur**.
  Un attaquant qui prendrait le conteneur n'obtient qu'un fichier **chiffré inexploitable**.
- ⛔ Les options « clé uniquement sur le poste » et « l'utilisateur déploie lui-même » ont été
  écartées pour une raison **opérationnelle** : elles imposent un aller-retour **à chaque
  itération**, y compris pour un correctif d'une ligne. ⚠️ **Une friction pareille finit par être
  contournée — c'est ainsi que les mécanismes de sécurité meurent.**
- 🔴 **Risque assumé, à redire si la friction devient pénible** : la tentation de **stocker la
  phrase de passe** quelque part. ⛔ Le jour où elle est stockée à côté de la clé, **la signature
  ne protège plus de rien**.

⇒ ⛔ `ota_update.sh` ne doit **jamais** accepter la phrase de passe par **argument** ni par
**variable d'environnement** (elle finirait dans l'historique du shell et dans `/proc`) :
**saisie interactive uniquement**, ⛔ jamais écrite sur disque.


---

# D1-bis — l'impasse de la page web est TRANCHÉE (2026-10-21)

## ⭐ Pourquoi c'est un lot D1-**bis**, et pas un supplément de D3
⚠️ **D2 exige 4 combinaisons : (script × page) × (firmware × système de fichiers).**
⇒ ⛔ Sans ce lot, **la page ne peut pas participer à D2**. Il vient donc **AVANT** la campagne
terrain, ⛔ pas après.

## Le blocage, en une phrase
⛔ **La page ne peut pas CONSTRUIRE le manifeste** : il contient la **version**, et le
descripteur d'application du binaire porte un **hash git**, ⛔ pas la version SemVer (⭐ établi
par `security-reviewer` en P1). ⇒ Elle ne peut que **TRANSPORTER**.

## Décision : option **A** — le `.sig` devient un fichier de **2 lignes**
```
ligne 1 : pool-ota/1 <firmware|filesystem> <MAJ.MIN.PATCH> sha256:<64 hex>
ligne 2 : <signature base64>
```
⛔ **Aucun changement cryptographique** : la signature porte **toujours** sur les octets
**canoniques re-sérialisés par l'appareil**, ⛔ jamais sur le fichier reçu. ⛔ Et la ligne signée
reste **sans LF final**.

### ⚠️ Coût MESURÉ — ⛔ pas estimé
- **`src/` : 0 ligne.** ⭐ Le firmware reçoit déjà `manifest` et `signature` comme **champs
  POST** ; ⛔ il ne lit **jamais** un fichier `.sig`. ⇒ **Condition 10 intacte, aucune
  régression de D1** (commité et poussé en `ae2bf87`).
- Touchés : `tools/check_release_artefacts.sh` (bloc S2) · `.claude/commands/release.md`
  (étape 2ter) · `data/app.js` · `docs/UPDATE_GUIDE.md` · `docs/API.md`. ≈ **40 lignes**.
- ⛔ **Rien de publié n'est invalidé** : la **v2.85.0 ne porte aucun `.sig`** (vérifié sur l'API)
  — le format brut de 384 o n'a **jamais** été livré.

### ⚠️ Et il n'y a qu'UNE voie pour obtenir le manifeste
`/download-update` **le lit** lui aussi comme champ POST, ⛔ il ne le **reconstruit** pas.
⇒ ⚠️ Le format brut aurait **forcé** une reconstruction côté appareil depuis tag + digest, soit
un **second site de construction** — ⛔ exactement le défaut qu'on voulait éviter.

## 🔴 DÉCOUVERT EN MESURANT : CORS bloque la page
⚠️ Les assets GitHub ⛔ **ne renvoient aucun** `Access-Control-Allow-Origin` ⇒ **le navigateur ne
peut pas lire le `.sig`**.
⇒ ⚠️ **Conséquence pour D3** : pour la **mise à jour en ligne**, `/check-update` devra relayer
**le CONTENU** des deux lignes (~560 o), ⛔ **pas l'URL** — donc **une requête HTTPS de plus**
dans la session TLS déjà ouverte.
⭐ **Personne ne l'aurait vu avant D3**, au moment où ça aurait bloqué.

## Revalidation `security-reviewer` — **GO** (2026-10-21)
- **Format validé** : le manifeste est **public par construction** (genre, version, empreinte
  sont **déjà** exposés par `/check-update` et par l'API GitHub). ⛔ Aucune surface neuve — un
  `.sig` édité ⛔ ne peut produire qu'un **refus**, jamais une acceptation.
- 🔴 **Le MÉLANGE de fichiers doit avoir son PROPRE verdict.** Prendre le `.bin` d'une version et
  le `.sig` d'une autre donne une signature **valide** sur un manifeste **cohérent** : c'est la
  **liaison** qui échoue (`kUnbound`, lot B1).
  ⛔ **Ne JAMAIS le rendre `kInvalid`** — ce serait confondre « clé inconnue » et « mauvais
  fichier », et envoyer l'utilisateur soupçonner **sa clé**.
  ⇒ Libellé dédié : **« signature authentique, mais pas pour cette image »**.
  ⚠️ **Et l'enjeu n'est pas cosmétique** : sur `/update`, la liaison n'échoue qu'**après**
  `Update.begin()` ⇒ en mode **système de fichiers**, la partition est **déjà effacée**
  (C1, tranché) ⇒ **interface perdue** jusqu'à un nouvel envoi.
  ⭐ **Remède gratuit, côté page** : afficher **genre et version lus dans le `.sig`** AVANT
  l'envoi, et comparer l'empreinte annoncée à celle du `.bin` (⚠️ la page sait **déjà** la
  calculer, feature-118).
- **Relais du contenu par `/check-update` : acceptable, et c'est la bonne place.** ⚠️ La session
  TLS est **déjà ouverte**, le GET est minuscule et passe par le **lecteur borné de bug-173**.
  ⛔ L'alternative (un GET dans `/download-update`) vérifierait **après** la garde TLS — **trop
  tard**.
- **Borne du `.sig` : 1 024 o** (2 lignes ≈ 100 + 512 + LF). ⛔ Lecture bornée par le **même
  prédicat que bug-173**, ⛔ **jamais** un tampon dimensionné sur `Content-Length`. Au-delà ⇒
  refus explicite, ⛔ sans effet de bord.

## Le geste réel de l'utilisateur
- **Depuis une release** : télécharger `firmware.bin` **et** `firmware.bin.sig` (2 clics), les
  sélectionner dans la page. ⚠️ Première release concernée : **la prochaine**.
- **En développement** : `./ota_update.sh --sign-only <genre>` dans le conteneur, puis récupérer
  les deux fichiers. ⚠️ **Honnêtement plus pénible que le script** — ⭐ la page reste la voie de
  **secours**, ⛔ pas la voie courante.
- ⛔ Un `.sig` ne vaut que pour **CE binaire exact** et **CETTE version**. ⛔ Ne jamais mélanger
  deux releases.

## ⚠️ Ce que ça change pour la carte « Mise à jour manuelle »
Le **sélecteur de type** devient **redondant** : le genre est dans la ligne signée et en
deviendra l'**autorité** en D3 (condition 2). ⇒ **Candidat à disparaître** — étude
`ux-ui-designer` en cours.

## Étude `ux-ui-designer` — RENDUE (2026-10-21), sur capture de la carte réelle

⚠️ **Mandat ENTIER** (règle du 2026-10-11) : densité, hiérarchie, libellés, états vides et
d'erreur, cibles tactiles, responsive 390 px, **et tout choix visuel que ce lot impose**.
⭐ Capture jointe au brief (`/tmp/claude-0/ota-systeme.png`) — ⛔ l'agent n'a pas de shell,
c'est l'agent principal qui photographie.

### Ce qui DISPARAÎT
🔴 **Le sélecteur `<select id="update_type">`.** ⛔ Il demandait à l'humain une information que
la ligne signée **affirme** — et que D3 rendra **autoritaire** (condition 2). Le garder, c'est
offrir de contredire la signature.
⇒ ⭐ **Un retrait, pas un ajout** — conforme au chantier de simplification (2026-10-09).

### Ce qui APPARAÎT
1. **Deux champs fichier empilés**, ⛔ jamais côte à côte (règle responsive : à 390 px un nom
   de fichier ne partage pas sa ligne) :
   - « **Image (.bin)** »
   - « **Signature (.sig)** » — ⚠️ **facultative en D1/D2**, et le dire : sans elle, l'appareil
     **journalise** `kSkipped` et n'a **rien** à vérifier.
2. **Le type calculé vit dans le `.field__hint` du champ Signature**, ⛔ pas dans un `.field`
   à lui : ce n'est plus une saisie, c'est une **conséquence**.
3. **Récapitulatif compact au-dessus du bouton** : genre · version · empreinte (12 hex) —
   ⛔ pas les 64, illisibles à 390 px.

### Comparaison d'empreinte AVANT envoi — **bloquante, sans dérogation**
⚠️ ⛔ **C'est la seule garde de ce lot qui refuse quelque chose**, et elle est **côté page**.
**Pourquoi elle ne peut pas être « souple »** comme le garde-fou de type (feature-118) : sur
`/update`, la liaison manifeste ↔ image n'échoue qu'**après** `Update.begin()` ⇒ en mode
**système de fichiers**, la partition est **déjà effacée** ⇒ **interface perdue**.
⛔ Un « Envoyer quand même » ici coûterait l'accès à l'appareil.
⇒ `callout callout--warn`, bouton **désactivé**, ⛔ aucun bouton de passage outre.

### Le bouton passe de `.btn--ok` à `.btn`
⭐ **Le vert est réservé aux actions que la page a elle-même vérifiées.** Tant que la page ne
faisait que transmettre, il était décoratif. Maintenant qu'elle **peut refuser**, le vert
dirait « tout est vérifié » alors que l'**authenticité**, elle, est vérifiée par l'**appareil**.
⇒ Neutre.

### ⚠️ Corrigé en cours de lot (2026-10-21) — la phrase ci-dessus était FAUSSE
🔴 **« C'est la seule garde de ce lot qui refuse quelque chose » (ligne ci-dessus) est erronée,
et ce projet garde la trace de ses erreurs plutôt que de les effacer.** Il existe en réalité
**DEUX** conditions de blocage côté page :
1. La comparaison d'empreinte (B4, ci-dessus) — inchangée.
2. La **lecture du `.sig`** (fichier trop volumineux, hors grammaire, etc.) — un `.sig` choisi
   mais illisible bloque aussi l'envoi, tant qu'il n'est pas corrigé ou retiré.

**Et la mise en œuvre initiale de `web-ui-developer` avait ajouté une TROISIÈME condition, à
tort** : la divergence nom de fichier ↔ genre déclaré par la signature (ci-dessus, § 2) bloquait
**elle aussi** l'envoi — y compris quand la comparaison d'empreinte venait de **PROUVER**
(`bindingState === "match"`) que le manifeste signé décrit bien **ce fichier exact**.

⛔ **C'est un défaut, pas une prudence supplémentaire.** `detectUpdateType()` est une
**heuristique sur un nom** — un nom qu'un téléchargement, un renommage ou un navigateur peut
avoir altéré. L'empreinte, elle, est **constatée sur les octets**. Faire gagner l'heuristique
contre le constat revient à refuser un envoi que rien ne rend suspect — exactement le principe
produit déjà posé le 2026-10-08 : *une garde qui bloque plus qu'elle ne protège est un défaut*.

#### 🔴 ⚠️ Et la première rédaction de ce paragraphe disait « une **preuve** » — c'est FAUX
⭐ Relevé par `security-reviewer` en P2 (2026-10-21), et c'est **le travers que ce projet
redoute le plus** : un texte qui convainc son relecteur d'une vérification **qui n'a pas eu
lieu**.
⛔ **Une empreinte SHA-256 ne prouve RIEN sur l'origine** : n'importe qui peut en calculer une.
Ce qu'elle établit, c'est la **LIAISON** — *ce manifeste décrit bien ces octets-là* — ⛔ jamais
l'**AUTHENTICITÉ** — *ce manifeste vient bien du détenteur de la clé*.
⇒ En D1/D2, ce qui gagne contre l'heuristique de nom n'est donc **pas** une preuve : c'est un
document **non encore vérifié**, qui décrit exactement le fichier choisi. ⭐ Défendable — les
deux fichiers viennent du même téléchargement — ⛔ mais à ne pas appeler autrement que ce que
c'est.
⇒ ⚠️ **La vérification d'authenticité vit DANS L'APPAREIL**, et en D1 elle **journalise**
seulement. ⇒ **Aucun libellé de la page ne doit laisser croire qu'elle a vérifié une
signature.**

**Correctif** : la divergence nom↔genre ne bloque plus l'envoi quand `bindingState === "match"`
— la notice reste affichée (elle informe : « le nom suggère X, mais l'empreinte confirme que la
signature décrit bien Y »), mais en `callout--info`, pas `callout--warn`, et le bouton reste
actif. Dans tous les autres cas (liaison pas encore prouvée, ou divergente), le blocage reste
entier — la correction ne retire rien qui protège réellement.

Décision extraite en fonction PURE testable : `otaManualSendBlocked({hasFile, sigReadState,
typeConflict, bindingState})` (`data/app.js`), couverte par vecteurs et validée par mutation dans
`test/sig_parse_vectors.js` (5 mutations supplémentaires, toutes tuées, dont l'inversion exacte
du défaut initial).

### Ce que l'agent a signalé SANS qu'on le lui demande
- **Le nom de fichier doit être affiché en ENTIER**, pas tronqué : c'est le seul moyen pour
  l'opérateur de voir qu'il a pris le `.sig` d'**une autre release** — ⛔ et `firmware.bin.sig`
  et `firmware.bin.sig` d'une version voisine sont **visuellement identiques** sans la version.
  ⇒ ⭐ C'est le récapitulatif (genre · **version** · empreinte) qui le rattrape.
- **Aucune couleur neuve** : `callout--warn` et `callout--info` existent, ⛔ ne pas en inventer.
