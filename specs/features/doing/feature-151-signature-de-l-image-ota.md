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

---

# D2 — PROCÉDURE DE PREUVE TERRAIN (rédigée le 2026-10-21, pas encore exécutée)

⚠️ **Rien de D1 ni de D1-bis n'a tourné sur l'appareil.** Ce qui suit est la procédure, pas un
compte rendu. ⛔ Ne pas cocher une case sans l'avoir relevée dans le journal de l'appareil.

## ⭐ Le fait qui rend cette campagne possible — VÉRIFIÉ dans le code

Les verdicts sont journalisés en **INFO** (nominal) ou **WARNING** (ce que D3 refuserait), et le
journal ne se persiste que **toutes les 10 minutes** (`Logger::kFlushIntervalMs = 600000`).
⚠️ Or **un OTA réussi redémarre l'appareil 3 s après la réponse** (`kRestartAfterOtaDelayMs`)
⇒ on pourrait craindre de **perdre le verdict** avant de l'avoir lu.

⭐ **Ce n'est pas le cas, et c'est prouvé par le code** : juste avant de redémarrer,
`web_server.cpp` journalise `"Redémarrage après mise à jour OTA"` en **CRITICAL** — et un
CRITICAL déclenche un `flushToDisk()` **synchrone** (`logger.cpp`, « Flush immédiat sur
ERROR/CRITICAL pour survivre aux crashes imminents »). ⇒ **Les verdicts sont persistés par ce
flush**, et relus au démarrage suivant.
⇒ ⛔ **Pas besoin de courir** pour lire le journal dans les 3 secondes.

## Vocabulaire du journal — à reconnaître, ⛔ pas à interpréter

Deux lignes par envoi, toutes deux préfixées `[feature-151 D1]` / `[feature-151 D1 B1]` :

| Ligne | Valeurs possibles |
|---|---|
| `authenticité OTA : …` | `absente` · `malformée` · `vérification impossible` · `invalide` · `genre discordant` · `sous le plancher` · **`valide`** |
| `liaison manifeste/image (…) : …` | `non applicable` · **`liée`** · `NON liée` |

⚠️ La liaison est journalisée **DEUX fois** par envoi, à deux étapes **différentes** :
`paramètre sha256 (index==0)` puis `empreinte calculée (final)`.
⭐ **La seconde est la seule qui porte sur les octets RÉELLEMENT ÉCRITS** — la première ne
compare qu'au paramètre *déclaré* par l'appelant. ⇒ **Les deux doivent dire `liée`.**

⚠️ Chaque ligne se termine par `D3 refuserait : oui|non — AUCUN refus appliqué (D1, vérification
seule)`. ⛔ **Un WARNING ne signifie PAS qu'un flash a été bloqué** — en D1 **rien** n'est refusé.

ℹ️ `sous le plancher` est **inatteignable aujourd'hui** : le plancher anti-retour-arrière vaut
`0.0.0` (`kOtaVersionFloorDefault*`). ⛔ Ne pas chercher à le déclencher.

## Préparation — ⛔ l'ordre n'est pas négociable

```bash
cd /root/esp32-pool-controller
pio run            # puis, SEULEMENT APRÈS qu'il a fini :
./build_fs.sh      # ⛔ jamais les deux en même temps (ressource exclusive)
./ota_update.sh --sign-only both    # phrase de passe demandée DEUX fois (une par binaire)
```

🔴 **SIGNER APRÈS AVOIR CONSTRUIT, jamais avant.** Toute recompilation change l'empreinte ⇒ le
`.sig` ne décrit plus le binaire ⇒ verdict `NON liée`, et on perd une demi-journée à chercher
une faute de clé qui n'existe pas.
⚠️ La phrase de passe est demandée par `openssl` lui-même. ⛔ Jamais en argument, jamais en
variable d'environnement, jamais dans un fichier.

Contrôle **avant** de toucher à l'appareil :
```bash
./tools/check_release_artefacts.sh     # doit sortir 0 — vérifie les .sig contre la clé EMBARQUÉE
head -c 80 .pio/build/esp32dev_core3/firmware.bin.sig ; echo
wc -l .pio/build/esp32dev_core3/*.sig   # 2 lignes chacun, exactement
```

### Lire le journal de l'appareil
```bash
set -a; . /root/.pool-credentials; set +a      # ⛔ ne jamais afficher $POOL_PASSWORD
TOKEN=$(curl -s -X POST http://192.168.1.48/auth/login \
  -H 'Content-Type: application/json' \
  --data-binary "{\"username\":\"admin\",\"password\":\"$POOL_PASSWORD\"}" \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
curl -s "http://192.168.1.48/get-logs" -H "X-Auth-Token: $TOKEN" \
  | python3 -m json.tool | grep -i "feature-151"
```

## ⛔ L'ordre des tests est imposé par une dépendance

T3 et T4 se font **depuis la page**, or **la page neuve n'arrive sur l'appareil qu'en T2**.
⇒ **T1 → T2 → T3 → T4 → N1 → N2.** ⛔ Ne pas commencer par la page.

---

## T1 — script × firmware

```bash
./ota_update.sh firmware 192.168.1.48
```
**Attendu** : `authenticité OTA : valide` · `liaison … (paramètre sha256 (index==0)) : liée` ·
`liaison … (empreinte calculée (final)) : liée` · les trois en **INFO**, `D3 refuserait : non`.
Puis redémarrage, et **`Firmware 2.86.0`** dans Réglages ▸ Système.

- [ ] Les **trois** lignes relevées, verbatim, dans la section « Relevés » ci-dessous.

## T2 — script × système de fichiers

```bash
./ota_update.sh filesystem 192.168.1.48
```
⚠️ **Un envoi de système de fichiers redémarre aussi l'appareil** — ⛔ ce n'est pas une mise à
jour « sans interruption ».
**Attendu** : identique à T1, avec le genre `filesystem` dans le manifeste.
⇒ À l'issue de T2, **la page neuve est en place** : deux champs, pas de sélecteur de type.

- [ ] Les trois lignes relevées · [ ] la carte « Mise à jour manuelle » montre bien **2 champs**

## T3 — page × firmware

1. Récupérer sur le poste `firmware.bin` **et** `firmware.bin.sig` (les mêmes qu'en T1).
2. Réglages ▸ Système ▸ Mise à jour manuelle : poser les deux fichiers.
3. **Avant de cliquer**, relever ce que la page affiche :
   - le récapitulatif : genre · version · empreinte (12 hex, en police à chasse fixe) ;
   - le hint sous Signature : le **nom complet du fichier**, puis « Signature lue… » ;
   - ⛔ **aucune mention d'une signature « vérifiée »** — la page ne vérifie rien.
4. Envoyer.

**Attendu** : mêmes trois lignes qu'en T1.
⚠️ **Et un contrôle qui n'existe que par la page** : le calcul de l'empreinte du `.bin` prend
plusieurs secondes — un **indicateur animé** doit s'afficher, puis céder la place au verdict.

- [ ] Les trois lignes · [ ] récapitulatif conforme · [ ] indicateur d'attente vu

## T4 — page × système de fichiers

Identique à T3 avec `littlefs.bin` + `littlefs.bin.sig`.
⚠️ **Le type est déduit de la ligne signée**, ⛔ plus d'un sélecteur : vérifier que le hint
annonce bien « Système de fichiers » **sans aucune action de l'opérateur**.

- [ ] Les trois lignes · [ ] type annoncé correctement, sans saisie

---

## N1 — mélange de fichiers : le `.sig` d'un genre avec l'image de l'autre

⭐ **C'est le scénario que `security-reviewer` a mis en tête** : la signature est **authentique**
et le manifeste **cohérent** — c'est la **liaison** qui échoue.

### N1-a — côté PAGE : ⛔ l'envoi ne doit même pas partir
Poser `littlefs.bin` avec `firmware.bin.sig`.
**Attendu** : callout ambre commençant par **« Empreinte différente. »**, bouton **désactivé**,
⛔ **aucun** moyen de passer outre. ⛔ **Rien n'est envoyé** ⇒ aucune ligne neuve au journal.
- [ ] Bouton désactivé · [ ] aucune ligne `feature-151` nouvelle au journal

### N1-b — côté SCRIPT : l'envoi part, et l'appareil le DIT
```bash
cp .pio/build/esp32dev_core3/firmware.bin.sig /tmp/mix.sig   # .sig du firmware…
# …envoyé avec l'image du système de fichiers (à faire à la main, cf. ota_update.sh)
```
**Attendu** : `authenticité OTA : valide` (la signature **est** bonne) **mais**
`liaison … : NON liée`, en **WARNING**, `D3 refuserait : oui`.
⚠️ **Et l'appareil refuse quand même le flash**, pour une raison **différente** : le
pré-contrôle de l'octet magique (une image de système de fichiers déclarée « firmware » n'a pas
`0xE9` en tête) ⇒ refus **avant tout effet de bord**. ⛔ **Ne pas confondre les deux refus** :
l'un vient de D1 (qui ne refuse rien), l'autre de la garde 2.20.1.

- [ ] `valide` + `NON liée` relevés · [ ] le flash n'a **pas** eu lieu · [ ] version inchangée

## N2 — signature altérée

```bash
cp .pio/build/esp32dev_core3/firmware.bin.sig /tmp/bad.sig
# Modifier UN caractère de la LIGNE 2, en restant dans [A-Za-z0-9+/] et à 512 caractères.
# ⛔ NE JAMAIS toucher au .bin : seul le .sig est altéré, l'appareil reste sain.
```
**Attendu côté appareil** : `authenticité OTA : invalide`, en **WARNING**.
⭐ **Et le filet M2 doit se déclencher** : une seconde ligne WARNING affiche les **octets
canoniques re-sérialisés par l'appareil**, entre guillemets.
⚠️ **C'est le point le plus utile de toute la campagne** : il existe pour qu'un `invalide` dû à
un **saut de ligne en trop** chez le signataire cesse d'être **indistinguable** d'une mauvaise
clé. ⇒ **Relever ces octets verbatim** et les comparer à ce que `sign_manifest()` a signé.

**Attendu côté page**, si l'altération casse la grammaire (longueur ≠ 512, caractère hors
alphabet) : la page **refuse de lire le fichier** et le dit (« 512 caractères base64 attendus »)
⇒ ⛔ rien n'est envoyé. **Les deux comportements sont à relever** — ce sont deux niveaux
différents.

- [ ] `invalide` relevé · [ ] octets canoniques relevés **verbatim** · [ ] message de la page relevé

---

## Relevés — ⛔ à remplir pendant la campagne, pas après

| Test | Authenticité | Liaison (index==0) | Liaison (final) | Niveau | Conforme ? |
|---|---|---|---|---|---|
| T1 | | | | | |
| T2 | | | | | |
| T3 | | | | | |
| T4 | | | | | |
| N1-b | | | | | |
| N2 | | | | | |

## Critère de clôture de D2
⛔ **D3 ne commence pas** tant que les six lignes ci-dessus ne sont pas remplies **avec des
relevés réels**. ⚠️ Un « ça a marché » sans le texte du journal ⛔ ne vaut rien : c'est
exactement l'erreur de diagnostic qui a masqué bug-170 pendant six semaines.

---

# D2 — RELEVÉS RÉELS (campagne du 2026-11-14, appareil 192.168.1.48)

⭐ **La chaîne complète est prouvée sur le matériel** : clé privée de l'utilisateur → `openssl`
→ fichier `.sig` à 2 lignes → transport → vérification **dans l'appareil**.

| Test | Authenticité | Liaison (index==0) | Liaison (final) | Niveau | Conforme ? |
|---|---|---|---|---|---|
| **T1** script × firmware | `valide` | `liée` | `liée` | INFO | ✅ |
| **T2** script × système de fichiers | `valide` | `liée` | `liée` | INFO | ✅ |
| **N1** mélange genre/image | `valide` | ⛔ **jamais atteinte** | ⛔ **jamais atteinte** | INFO + ERROR | ⚠️ voir ci-dessous |
| **N2a** signature altérée | `invalide` | `liée` | `liée` | WARN | ✅ |
| **N2b** empreinte altérée | `invalide` | `NON liée` | `NON liée` | WARN | ✅ |
| **T3** page × firmware | `valide` | `liée` | `liée` | INFO | ✅ |
| **T4** page × système de fichiers | `valide` | `liée` | `liée` | INFO | ✅ |

⭐ **Les QUATRE combinaisons exigées par D2 sont vertes, et les deux chemins — script shell et
navigateur — produisent le MÊME verdict sur l'appareil.** C'était la question que ce lot posait :
⛔ aucun des deux ne fait de cryptographie, tous deux se contentent de **transporter** les deux
lignes, et c'est l'appareil qui tranche.

## 🔴 Trois enseignements que la procédure écrite n'avait PAS prévus

### 1. Le premier envoi ne peut pas être un test — c'est un AMORÇAGE
⛔ L'appareil tournait en **2.85.0**, firmware qui **ignore** feature-151. Le premier envoi n'a
donc produit **aucun verdict** : seule la ligne `[feature-118 lot B] Empreinte upload OTA :
conforme` apparaît. ⚠️ Évident après coup, invisible avant.
⇒ **La procédure doit nommer cet envoi « amorçage »**, pas « T1 ». Les tests ne commencent
qu'une fois le firmware **porteur de la garde** en place.

### 2. `--sign-only` n'est PAS un préalable au chemin SCRIPT
⛔ Le mode d'envoi **re-signait** le manifeste à chaque fois, donc réclamait la phrase de passe,
alors que le `.sig` validé était déjà à côté du binaire. ⇒ La campagne était **impraticable sans
un humain à chaque envoi**.
✅ **Corrigé** : `--use-sig` relit le `.sig` au lieu de re-signer (fail-closed si l'empreinte ne
décrit pas le binaire), et `--use-sig-raw <fichier>` **saute** ce contrôle — ⚠️ réservé aux
essais négatifs, seul moyen de faire parler la garde de l'appareil.
⭐ C'est exactement ce que fait la page web : elle **transporte**, elle ne signe pas (ADR-0121).

### 3. ⛔ La liaison est INATTEIGNABLE pour le mélange genre/image
**Mesuré** : image du système de fichiers envoyée avec la signature **du firmware**.
```
22:20:36 INFO  authenticité OTA : valide
22:20:36 ERROR octet magique 0x03 au lieu de 0xE9 → refusé
version : 2.86.0 (INCHANGÉE)
```
⚠️ Le **pré-contrôle de l'octet magique** (garde 2.20.1) sort de la fonction **AVANT** le point
où la liaison est journalisée. ⇒ Pour ce mélange, `NON liée` **ne peut pas** apparaître.
⛔ **Ce n'est pas un défaut** : l'image est refusée **sans aucun effet de bord**, ce qui est le
bon résultat. Mais le test ne prouve pas ce qu'il prétendait prouver.
⇒ La liaison a été prouvée **autrement**, par **N2b** (empreinte altérée dans la ligne signée),
qui la fait casser **aux deux étapes**.

⚠️ **Le mélange réellement dangereux reste NON TESTÉ** : une image **firmware** écrite dans la
partition du **système de fichiers** (genre `filesystem` cohérent avec `update_type`, octet
magique seulement *averti* dans ce sens). En D1 **rien ne refuse** ⇒ la partition serait
**écrasée** et l'interface **perdue** jusqu'à un nouvel envoi. ⛔ Essai **destructif** : il exige
un feu vert explicite, et un `littlefs.bin` prêt à renvoyer.

## Le filet M2 est en place, et il fonctionne
```
22:21:21 WARN octets canoniques re-sérialisés par l'appareil
              (comparer avec ce qui a été signé) :
              "pool-ota/1 firmware 2.86.0 sha256:9e0580fc…f951e"
```
⭐ Sans cette ligne, un `invalide` dû à un **saut de ligne en trop** chez le signataire serait
⛔ **indistinguable** d'une mauvaise clé. En N2b elle affiche `ee0580fc…`, l'empreinte altérée :
⇒ **l'appareil re-sérialise depuis ce qu'il a PARSÉ**, il ne fait jamais confiance aux octets
reçus tels quels.

## État de l'appareil à la fin de la campagne
`2.86.0` · page d'accueil HTTP 200 · **`update_sig` présent**, **`update_type` absent** ⇒ la page
à deux champs est bien en service, le sélecteur de type a disparu. ⇒ **T3 et T4 sont possibles.**

## 🔴 Quatrième enseignement — ⛔ « pas besoin de courir » était FAUX

⚠️ Plus haut, cette spec affirmait qu'un verdict **survit au redémarrage** grâce au `flushToDisk()`
synchrone du CRITICAL « Redémarrage après mise à jour OTA ». **C'est exact, et insuffisant.**

⛔ **`/system.log` est plafonné à 8 Ko** (`Logger::kMaxLogFileBytes`, ADR-0033) et ne garde que
les **6 derniers Ko** à la rotation. Or un envoi OTA écrit **une ligne de progression par pour
cent**, et le démarrage qui suit en ajoute une cinquantaine.
⇒ **Mesuré le 2026-11-14** : les verdicts de T4 ont bien été écrits — le fichier récupéré
commence à `22:39:26`, soit **juste après** eux — puis la rotation les a chassés **avant** la
lecture. ⛔ Le test n'avait pas échoué : **la preuve avait disparu.**

⇒ **La fenêtre utile est d'environ UNE MINUTE.** Les cinq premiers relevés n'ont tenu que parce
que la lecture suivait l'envoi immédiatement.
⇒ ⛔ **Ne jamais attendre une « fin de campagne » pour relever.** Lire après **chaque** envoi.
⚠️ Et ⛔ **ne pas guetter un redémarrage** pour déclencher la lecture : un incident réseau en
provoque un (constaté le 2026-11-14 : watchdog connectivité + MQTT en échec, sans aucun rapport
avec les tests). **Guetter l'apparition d'un verdict NEUF**, c'est le seul signal fiable.

## Reste à faire
- [x] ✅ **L'essai DESTRUCTIF — FAIT le 2026-11-14**, voir la section « N3 » en fin de spec.
      ⭐ Seul chemin où la garde de liaison est atteinte. Dégâts bien moindres qu'annoncé, et
      🔴 **une conséquence pour D3 en est sortie** (chemin de récupération).
- [ ] Cosmétique relevé pendant T3/T4 : le libellé **« Firmware (.bin) »** / **« Système de
      fichiers (.bin) »** garde un `(.bin)` hérité de l'ancien menu déroulant, où il distinguait
      deux options. ⛔ Les deux types sont des `.bin` : il n'apprend plus rien. À raccourcir.
- [ ] Corriger dans la procédure : le premier envoi est un **amorçage**, pas « T1 ».

---

# N3 — l'essai DESTRUCTIF, fait (2026-11-14, accord explicite de l'utilisateur)

**Couple envoyé** : image **firmware** (1,9 Mo) déclarée `update_type=filesystem`, avec le
manifeste de `littlefs.bin` (genre cohérent ⇒ authenticité jugeable, empreinte divergente ⇒
liaison jugeable).
⭐ **C'est le SEUL chemin où la garde de liaison est ATTEINTE** : dans ce sens le pré-contrôle de
l'octet magique ⛔ ne refuse pas, il **avertit** — une image LittleFS peut commencer par `0xE9`
par coïncidence, et refuser produirait des faux positifs (`src/ota_type_logic.h`). N1 ne pouvait
donc pas le montrer.

```
22:58:39 INFO [feature-151 D1]    authenticité OTA : valide
22:58:39 WARN [feature-151 D1 B1] liaison manifeste/image : NON liée — D3 refuserait : oui
HTTP 500 — 1,9 Mo n'entre pas dans une partition de 512 Ko
```

## Dégâts RÉELS, mesurés — ⛔ bien moindres qu'annoncé
| | |
|---|---|
| Pages web (`spiffs`) | ❌ **détruites** |
| Programme (`app0`/`app1`) | ✅ intact, 2.86.0, appareil vivant |
| Boucle de démarrage | ✅ **aucune** |
| **Configuration, calibrations, compteurs (NVS)** | ✅ **intacts** — relus : correction pH `1.2`, volume `medium` |
| Historique + journal (`history`) | ✅ intacts |

⚠️ **J'avais annoncé un risque de boucle de démarrage irrécupérable sans port série. C'était
FAUX**, et ça a failli faire renoncer à l'essai. La cause de l'erreur : un raisonnement par
analogie avec l'incident de corruption de la partition `history` (07/2026), ⛔ sans lire le
chemin de montage. Le code dit l'inverse : `LittleFS.begin(**false**)` — ⛔ **pas de formatage**
en cas d'échec — journalise un CRITICAL et **poursuit**.
⇒ ⭐ **`/update` est du CODE, pas un fichier** : la route reste joignable sans aucune page web.

## 🔴 CE QUI A RATÉ, et c'est le vrai enseignement
**La réparation automatique a été REFUSÉE** : `littlefs.bin` avait été **reconstruit à 21:50**
(passe sur les libellés) alors que sa signature datait de **21:09**. ⇒ Empreintes différentes
⇒ `--use-sig` a refusé d'envoyer, **fail-closed, correctement**.
⚠️ **C'est exactement le piège écrit dans cette spec quelques heures plus tôt** — *« ne
recompilez rien entre la signature et les tests »* — et il s'est quand même produit.
⇒ **Une signature ne vaut QUE pour un fichier exact.** Toute reconstruction, même d'un seul
libellé, la périme.

**Réparé par un envoi SANS signature** (accepté, journalisé « absente »), suivi d'une
vérification sur le fichier RÉELLEMENT servi par l'appareil.

## 🔴 CONSÉQUENCE POUR D3 — à traiter AVANT d'activer le refus
⛔ **Cette porte de sortie n'existe que parce que D1 ne refuse rien.**
⚠️ Une fois D3 en vigueur, un envoi **sans signature valide** sera **rejeté** ⇒ dans la situation
exacte de ce soir — interface détruite, signature périmée par une reconstruction — ⛔ **la
réparation aurait été impossible sans la phrase de passe de l'utilisateur**, à l'heure où
l'incident tombe.

⇒ **D3 doit livrer un chemin de récupération**, et le choisir explicitement. Pistes, à arbitrer :
1. **Le refus ne porte que sur le FIRMWARE**, pas sur le système de fichiers. ⚠️ Asymétrie à
   justifier : la partition `spiffs` ne contient aucun code exécuté.
2. **Un mode de secours borné** (fenêtre de N minutes après un démarrage sur échec de montage),
   pendant laquelle un envoi non signé reste accepté. ⚠️ Surface d'attaque à faire valider.
3. ⛔ ~~**Rien** — la phrase de passe devient un prérequis de toute réparation.~~
   🔴 **ÉCARTÉE — décision utilisateur du 2026-11-14 :**
   > *« Si la phrase secrète devient nécessaire, ça n'est pas une solution. Je ne peux pas
   > donner la phrase à un client qui aurait son appareil bloqué. »*

## 🔴 LA CONTRAINTE QUI CADRE D3 — et elle change le périmètre du projet
⚠️ **Il y a des CLIENTS.** ⛔ Ce n'est plus un appareil unique sur un bureau.
⇒ **Le chemin de récupération doit être utilisable par quelqu'un qui n'a NI la clé privée, NI sa
phrase de passe, NI un terminal.** ⛔ La clé est celle du **signataire**, pas celle du
propriétaire de l'appareil — la transmettre détruirait la seule chose que la signature protège.

⚠️ **Et ça disqualifie aussi `curl`** : c'est ainsi que j'ai réparé le 2026-11-14, mais un client
ne tapera pas de ligne de commande. ⇒ **Une récupération qui exige un terminal n'en est pas une.**

### 🔴 Piste 4 — AJOUTÉE : une page de secours EMBARQUÉE DANS LE FIRMWARE
⭐ **Elle sort du dilemme au lieu de l'arbitrer.** La partition `spiffs` peut être détruite ; la
partition **programme**, elle, est **signée** et intacte (prouvé par N3).
⇒ Servir, **quand LittleFS ne se monte pas**, une page **minimale compilée dans le firmware** —
un champ fichier et un bouton — suffisante pour renvoyer un système de fichiers.
- ⭐ **Rien d'externe n'est à faire confiance** : la page vient du binaire signé lui-même.
- ⭐ Le client répare **seul, au navigateur**, sans clé, sans phrase, sans terminal.
- Coût : quelques Ko de flash (occupation actuelle **51 %**, la marge existe).
- ⚠️ À combiner avec la piste 1 (signature exigée pour le **firmware** seulement) ou la piste 2.

### 🔴 PISTE 1 — MORTE. Décision utilisateur du 2026-11-14
> *« Le système de fichiers doit aussi être contrôlé. Il n'exécute pas de code sur l'ESP32, mais
> il s'affichera dans le navigateur du client et ça pourra exécuter du code JavaScript. »*

⭐ **Exact, et c'est plus grave que l'« asymétrie défendable » écrite plus haut** (rédigée par
moi-même une heure plus tôt — trop indulgente, ⛔ à ne pas réutiliser).
Un `littlefs.bin` hostile **s'exécute**, dans un autre moteur : le **navigateur**. Et sur
**l'origine de l'appareil**, ce qui lui donne :
- l'accès en **même origine** à **tous** les endpoints (`/save-config`, `/pumpN/duty`, `/update`…) ;
- le **jeton de session admin** (`sessionStorage`) ;
⇒ piloter les pompes, lire la configuration, **exfiltrer le SSID Wi-Fi et les identifiants
MQTT**, hameçonner le mot de passe sur une fausse page de connexion, et tout renvoyer ailleurs.
⚠️ Le firmware pilote les pompes **directement** ; le système de fichiers les pilote **par la
main du client**. ⛔ La seconde voie n'est pas moins grave, elle est juste indirecte.

⇒ 🔴 **La signature est OBLIGATOIRE pour LES DEUX images.** ⛔ Aucune exemption de genre.

## ⭐ Et la contrainte « pas de phrase de passe » tombe d'elle-même
⚠️ **Le client n'a JAMAIS à signer quoi que ce soit.** La signature est produite **une fois**,
par le signataire, à la publication — et le `.sig` est **public**, joint à la release (le gate
`check_release_artefacts.sh` l'impose déjà, et refuse une release sans lui).
⇒ **Récupération d'un appareil dont l'interface est détruite :**
1. l'appareil, ne montant pas son système de fichiers, sert la **page de secours embarquée**
   (piste 4, dans le binaire **signé**) ;
2. le client télécharge `littlefs.bin` **et** `littlefs.bin.sig` depuis la release ;
3. il dépose les deux. ⛔ Aucune clé, aucune phrase, aucun terminal.

⚠️ **Angle mort restant, à nommer** : un client **sans accès à la release** (pas d'internet sur
le poste) reste bloqué. ⇒ Piste à instruire : embarquer dans la page de secours de quoi
retrouver les fichiers, ou accepter ce cas comme hors périmètre — ⛔ mais l'écrire.

⛔ Ne pas trancher ça « en passant » pendant D3 : c'est `security-reviewer` qui doit statuer,
**avec cette contrainte client comme donnée d'entrée.**

## ⭐ Au passage : les six protections de la chaîne OTA, VÉRIFIÉES dans le code ce soir
1. Envoi **sans empreinte** ⇒ refusé **dès index==0**, avant toute écriture (feature-118 lot B).
2. Empreinte comparée **AVANT `Update.end(true)`** ⇒ ⛔ une image abîmée en transit **n'est
   jamais rendue démarrable** (`web_routes_ota.cpp`, commentaire explicite).
3. Octet magique `0xE9` ⇒ un non-firmware déclaré « firmware » refusé **avant effet de bord**.
4. **Deux emplacements programme** : l'OTA écrit dans l'**inactif**, l'actif n'est jamais touché.
5. **Retour arrière automatique** : `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` ⚠️ **vérifié sur
   le sdkconfig de l'ESP32 CLASSIQUE** (⛔ pas celui d'une autre puce — première lecture
   erronée). Le cœur Arduino arme le mécanisme (`esp32-hal-misc.c`) : une image
   `PENDING_VERIFY` qui ne se confirme pas ⇒ `esp_ota_mark_app_invalid_rollback_and_reboot()`.
   ⚠️ Le projet ne surcharge pas `verifyRollbackLater()` ⇒ le bloc est **actif**.
6. *Bootloader* et table des partitions ⛔ **non modifiables par OTA**.

⚠️ **Ce qu'aucune de ces six ne couvre** : un firmware **correctement transmis mais bogué**, qui
démarre et se confirme. ⇒ C'est une question de **tests avant publication**, ⛔ pas de transfert.
