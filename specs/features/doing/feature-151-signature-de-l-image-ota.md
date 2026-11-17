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
| **D3-a** | ⛔ **Aucun refus.** Le **genre signé** devient l'autorité (cond. 2, 3, 9) + relais CORS du `.sig`. | D1 | ⛔ **Arrêt terrain obligatoire** : `/download-update` n'a JAMAIS vu de manifeste |
| **D3-b** | ⛔ **Aucun refus.** **La porte de sortie** : mode dégradé du serveur web + page de secours embarquée (cond. 6, 7, 8). | D3-a | ⭐ Récupération prouvée **en conditions réelles**, `spiffs` détruite |
| **D3-c** | ✅ **Le refus.** Les `if`, les verdicts HTTP distincts, plancher **inerte à 0.0.0**, interrupteur. | D3-b | ⛔ Ne commence pas tant que D3-b n'est pas prouvé. 🔴 **ET ne sort PAS dans le même firmware que bug-176 (2.88.0)** — voir « CONTRAINTE D'ORDRE » en fin de spec |
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
   ⛔ pas `millis()`), relu à **chaque** `index==0`, publié **MQTT**.
   ⚠️ **CORRIGÉ le 2026-11-16 (revue P1 de D3-b2, condition C10)** : ~~le seul témoin qui
   survit à un firmware hostile qui purge le journal~~ — **c'est faux**. Un firmware hostile
   lit les identifiants MQTT en NVS et peut **republier `OFF`** ; un courtier sans persistance
   perd le `retain` ; hors ligne, la valeur reste périmée ; MQTT désactivé, le témoin n'existe
   pas. ⇒ **Seule la TRANSITION historisée par un tiers survit** (d'où l'entité Home Assistant,
   historisée par le `recorder`). La borne haute de la fenêtre doit aussi être vérifiée :
   une horloge fausse **vers le futur** à l'armement ferait survivre l'échéance aux
   redémarrages (défaut G1 de la revue P1).
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
⇒ ⭐ **`/update` est du CODE, pas un fichier** : la route reste joignable sans aucune page web
— 🔴 **MAIS SEULEMENT TANT QUE L'APPAREIL NE REDÉMARRE PAS.** ⛔ Cette phrase, écrite le
2026-11-14, est **FAUSSE après un redémarrage** : voir la section finale « un redémarrage sans
système de fichiers = appareil INJOIGNABLE ». C'est le fait qui éclate D3 en trois lots.

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
2. ⛔ ~~**Un mode de secours borné** (fenêtre de N minutes après un démarrage sur échec de
   montage), pendant laquelle un envoi non signé reste accepté.~~
   🔴 **MORTE — `security-reviewer` P1-bis (2026-11-15), condition B3.** ⚠️ *Un interrupteur qui
   s'arme sur un état que l'attaquant sait provoquer n'est pas borné* : le porteur du jeton admin
   peut **détruire la partition à volonté** (conflit C1), puis redémarrer (`/reboot`, ou la
   commande MQTT). ⇒ La fenêtre s'ouvrirait **à la demande**. Et une coupure de courant pendant
   un envoi FS légitime l'armerait toute seule.
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

---

# 🔴 LE FAIT QUI ÉCLATE D3 — un redémarrage sans système de fichiers = appareil INJOIGNABLE

**Vérifié dans le code le 2026-11-15**, `src/web_server.cpp:50-54` :

```cpp
void WebServerManager::begin(AsyncWebServer* webServer, DNSServer* dnsServer) {
  // Vérifier que LittleFS est monté
  if (!LittleFS.begin()) {
    systemLogger.critical("LittleFS non disponible pour le serveur Web");
    return;                    // ⛔ setupRoutes() n'est JAMAIS appelé
  }
  setupRoutes();               // ← c'est ICI que /update et /download-update sont enregistrées
  server->begin();             // ⛔ le serveur n'est même pas DÉMARRÉ
}
```

Chaîne de démarrage mesurée : `main.cpp:380` monte en `LittleFS.begin(false)` — ⛔ **pas de
formatage** — journalise un CRITICAL et **poursuit** ; puis `main.cpp:528` appelle
`webServer.begin()`, qui **sort immédiatement**.

⇒ 🔴 **Aucune route HTTP n'existe. Ni `/update`, ni `/download-update`, ni la page de secours de
la piste 4.** L'appareil tourne, rejoint le Wi-Fi, publie en MQTT — ⛔ et n'accepte plus **aucune**
mise à jour. ⚠️ **Et ce conteneur n'a pas de port série** : il faudrait ouvrir le boîtier et
brancher un câble USB.

## ⛔ Pourquoi N3 ne l'a pas montré
L'appareil **n'a jamais redémarré** avec la partition détruite : l'envoi s'est arrêté en HTTP 500
(1,9 Mo pour 512 Ko) — ⛔ **un échec d'écriture n'est pas un redémarrage** — et les routes étaient
déjà enregistrées depuis le démarrage précédent. La réparation a tenu dans la fenêtre.
⚠️ **Pendant ces deux minutes, une coupure de courant aurait immobilisé l'appareil.**
⇒ 🔴 **La phrase « la route reste joignable sans aucune page web » (section N3) est FAUSSE après
un redémarrage**, et elle avait été donnée à l'utilisateur comme une assurance. Corrigée sur place.

## ⇒ La piste 4 est INOPÉRANTE en l'état
Une page de secours **compilée dans le firmware signé** ne vaut rien si **le serveur qui la sert
ne démarre pas**. ⛔ Elle n'est pas une fonctionnalité d'affichage : elle exige d'abord un
**mode dégradé** du serveur web — démarrer **sans** système de fichiers, avec le strict minimum.

# 🔴 D3 ÉCLATÉ EN TROIS LOTS — l'ordre n'est pas négociable

⚠️ **Première raison du découpage** : D3 tel qu'écrit rend la **condition 10** (*« le diff D1→D3
ne touche QUE le prédicat de décision »*) ⛔ **invérifiable** — elle serait noyée dans un lot qui
ajoute une autorité de genre, un relais CORS, un mode dégradé, une page embarquée et un
interrupteur. ⇒ Isoler le refus dans un lot **minuscule** rend la condition 10 **vraie à
nouveau**, et vérifiable d'un coup d'œil sur le diff.

| Lot | Contenu | Refus ? | ⛔ Ce qui doit être vrai avant le lot suivant |
|---|---|---|---|
| **D3-a** « le socle » | Cond. **2** (le genre signé choisit la partition), **3** (une seule source d'empreinte), **9** (verdicts distincts, dont « vérification impossible ») + **relais du `.sig` par `/check-update`** (le CORS bloque la page, mesuré). | ⛔ **non** | ⭐ **Arrêt terrain** : prouver qu'un manifeste traverse `/download-update` |
| **D3-b** « la porte de sortie » | **Mode dégradé** du serveur web + **page de secours embarquée** + cond. **6** (absence de clé ⇒ signature exigée), **7** (interrupteur borné), **8** (gardes d'auth en tête). | ⛔ **non** | ⭐ Récupération prouvée **sur l'appareil, `spiffs` réellement détruite, après REDÉMARRAGE** |
| **D3-c** « le refus » | Les `if` de refus, les codes HTTP distincts, le plancher **inerte à 0.0.0**. ⭐ Diff **minuscule**, condition 10 redevenue vérifiable. | ✅ **oui** | D4 |

⛔ **On ne met pas en service un refus avant que la porte de sortie existe ET soit prouvée.**
C'est la leçon directe de N3 : la seule réparation qui a fonctionné le 2026-11-14 était un envoi
**sans signature**, et D3-c ferme cette porte.

## 🔴 L'ARRÊT TERRAIN DE D3-a — `/download-update` n'a JAMAIS vu de manifeste
**Mesuré** : `data/app.js:10909` et `:10961` envoient `url` + `digest` + `restart` — ⛔ **aucun
champ de signature**. La mise à jour **en ligne** (depuis une release GitHub) n'a donc **jamais**
été exercée avec un manifeste, à aucun moment de D1 ni de D2.
⇒ ⛔ **À D3-c, ce chemin serait refusé à coup sûr** — et c'est le chemin **normal** du client.
⇒ **D3-a se termine par un envoi réel par ce chemin**, verdict relevé au journal, ⛔ pas par une
lecture de code.

## 🔴 Ce que `security-reviewer` doit trancher en P1-bis — AVANT toute ligne de code
Le périmètre a changé depuis le P1 du 2026-10-18 : il y a des **clients**, la signature est
**obligatoire pour les deux genres**, et la récupération passe par un **mode dégradé**.
1. **Le mode dégradé est une surface d'attaque neuve** : un serveur qui démarre **sans** système
   de fichiers, dans un état où la configuration et l'authentification ⛔ ne sont peut-être pas
   chargées. ⚠️ Quelles routes y vivent, et **l'authentification y est-elle encore possible** ?
2. **La page de secours accepte un téléversement** — donc un chemin d'écriture en flash dans
   l'état le plus dégradé de l'appareil. ⚠️ Exige-t-elle la signature ? ⛔ Si oui, un client sans
   accès à la release reste bloqué ; si non, elle **annule** D3-c.
3. **L'angle mort nommé** : client sans accès internet. ⇒ Trancher : embarquer de quoi retrouver
   les fichiers, ou **écrire** que c'est hors périmètre.
4. **L'interrupteur borné (cond. 7)** et le mode dégradé se recouvrent-ils ? ⛔ Deux mécanismes de
   secours qui s'ignorent, c'est deux surfaces au lieu d'une.

---

# 🔒 Verdict `security-reviewer` **P1-bis** (2026-11-15) — la conception de RÉCUPÉRATION

**GO SOUS CONDITIONS — 14 conditions (B1 à B14)**, plus une **réserve de fond qui renverse la
conception** : ⛔ **le « mode dégradé » ne doit PAS être un mode.**
⚠️ Ce verdict porte sur la **conception**. ⛔ Il ne vaut pas GO en P2, et les **11 conditions du
P1 restent dues**.

## 🔴 Trois faits que ce verdict corrige — vérifiés dans le code

### 1. L'authentification NE DÉPEND PAS du système de fichiers
⭐ **C'était la question qui me gênait le plus, et la réponse est bonne.** Mot de passe admin et
jeton d'API vivent en **NVS** (`config.cpp:395-396`, relus en `:858-863`) et sont injectés dans
`authManager` en `main.cpp:402-405`. ⇒ **Un serveur démarré sans LittleFS s'authentifie
normalement.** ⛔ **Un dépôt de fichier anonyme n'a donc AUCUNE justification.**
⚠️ Au passage : ⛔ **il n'y a pas de sel** — mot de passe et jeton sont stockés **en clair**.
(Connu, inchangé par ce lot, à porter dans l'ADR.)

### 2. « Interface détruite » ⛔ ne se réduit PAS à « montage en échec »
Une écriture FS **interrompue** laisse un début d'image neuve et une fin d'ancienne : le montage
peut **réussir** sur un contenu incohérent — le code le prévoit déjà
(`web_routes_ota.cpp:271-275`, *« contenu potentiellement incohérent »*). ⇒ Dans cet état les
routes existent, l'interface est cassée, et ⛔ **aucune page de secours n'apparaîtrait** si le
déclencheur était le seul échec de montage. ⇒ **Condition B7.**

### 3. L'algorithme est **RSA-3072 PKCS#1 v1.5 / SHA-256**, ⛔ pas Ed25519
Déjà tranché dans cette spec (Q1, ligne 305 : *« Ed25519 ABSENT de mbedtls »*) et confirmé dans
le code (`constants.h:3956`, `web_routes_ota.cpp:480-481` : **exactement 384 octets**).
⚠️ L'erreur venait de **mon brief**, pas de la spec. ⛔ Aucun lot ne doit partir sur une
dépendance Ed25519.

## 🔴 LA RÉSERVE DE FOND — « le mode dégradé ne doit pas être un mode »

⛔ **Pas de table de routes réduite. Pas de drapeau `degraded`.** La récupération se fait par une
route de secours **toujours enregistrée**, qui poste sur le **`/update` existant**.

**Trois raisons, toutes mesurées :**
1. **Les routes d'API ne lisent pas `spiffs`** — configuration et produits sont en NVS. Les
   **seuls** lecteurs sont les pages statiques (404 si le fichier manque) et `/get-system-info`
   (renvoie 0). ⇒ Retirer le retour anticipé de `begin()` suffit : la surface neuve est **un seul
   GET public, statique, compilé dans le firmware**.
2. ⚠️ **Un drapeau consultable est une invitation** à écrire un jour `if (degraded)` dans un
   prédicat d'authentification ou de signature ; un second handler d'upload serait un **second
   site de garde** — ⛔ exactement le défaut de **bug-117**.
3. ⛔ **Un état qui n'existe qu'après une panne n'est jamais exercé** : il sera faux le jour où on
   en a besoin.

⭐ **Bénéfice opérationnel non demandé** : aujourd'hui, dans cet état, l'appareil **dose sans
aucune prise HTTP**. Avec la table complète, l'arrêt des pompes et la configuration redeviennent
pilotables.

## 🔴 Pourquoi la page de secours EXIGE la signature, sans exception
L'état « FS détruit » est **provocable par le porteur du jeton admin** : l'authenticité est jugée
à `index==0` **avant l'effacement** (`web_routes_ota.cpp:643-671`), la liaison seulement au
fragment final **après `LittleFS.end()`** (`:864-866`) ⇒ un `.sig` **public et authentique** avec
une image quelconque détruit la partition (conflit **C1**). `/reboot` répond au même jeton
(`web_routes_config.cpp:2624`), et une commande MQTT de redémarrage existe
(`mqtt_manager.cpp:549`).
⇒ ⛔ **Si l'état dégradé acceptait du non-signé, la chaîne « détruire → redémarrer → déposer un
`littlefs.bin` hostile » rendrait au porteur du jeton exactement ce que D3-c lui retire.**
⇒ **Coût réel pour le client : deux fichiers publics à télécharger au lieu d'un.** ⭐ Et la page
reste minuscule, parce qu'après la condition 3 du P1 (livrée en D3-a) elle ⛔ n'a **plus à
calculer de SHA-256** : elle envoie `manifest`, `signature`, puis le fichier.

## Les deux mécanismes de secours — ⛔ ils ne se composent JAMAIS
| | Page de secours | Interrupteur (cond. 7 du P1) |
|---|---|---|
| Panne couverte | FS détruit, **vérification saine** | **Vérification impossible** (clé perdue, défaut du vérificateur) |
| Utilisateur | le **client** | le **signataire** seul |
| Signature | **exigée** | **suspendue**, bornée |

⇒ Les deux survivent, sous **B3** et **B11**.

## ⛔ L'angle mort « pas d'internet » — tranché en trois cas
1. **Le poste a internet à un moment** (téléphone en 4G) : **couvert**. ⚠️ À documenter pour le
   mode point d'accès : **télécharger AVANT de s'y connecter**.
2. **L'appareil a internet, le poste non** : **couvert à surface nulle** par un bouton
   « réparer en ligne » (`/check-update` puis `/download-update`). ⭐ Possible **seulement après
   le relais du `.sig` de D3-a**. Recommandé, ⛔ non bloquant.
3. **Internet nulle part** : **hors périmètre assumé** ⇒ **B13**, à écrire dans la spec **et**
   dans `docs/UPDATE_GUIDE.md`. ⛔ **Ne jamais le résoudre par une acceptation non signée.**

## 🔴 Le seul risque que D3-a introduit alors qu'il ne refuse rien
Aujourd'hui `cmd` est fixé par `update_type`/nom de fichier (`web_routes_ota.cpp:613-633`), et
**tout ce qui suit en dépend** : `expectedKind` (`:644`), l'octet magique (`:680`),
`g_uploadIsFilesystem` (`:846`), `LittleFS.end()` (`:864`), `Update.begin()` (`:871`).
⇒ ⛔ **Si le genre signé corrige `cmd` APRÈS l'une de ces gardes, elle aura jugé l'ancien genre**
— un firmware partirait **sans contrôle `0xE9`**, ou une partition serait démontée pour rien.
⇒ **B8** : le genre est résolu **une seule fois, avant toute garde qui en dépend**.

## ⚠️ Et la preuve terrain de D3-b est l'étape la plus dangereuse du chantier
« `spiffs` détruite **puis redémarrage** » sur un appareil **sans port série** : si le code neuf
a un défaut, l'appareil est **injoignable** — et il **continue de doser**, avec MQTT pour seul
canal. ⇒ **B12**, preuve en **trois marches**.

---

## Conditions à lever (B1 à B14)

### Récupération — D3-b
- **B1.** La page de secours poste sur le **`/update` existant**. ⛔ Aucune seconde route
  d'upload, aucun second handler : `grep -c handleOtaUpdate src/web_routes_ota.cpp` ⛔ ne gagne
  **aucun** site d'enregistrement.
- **B2.** **Signature exigée à l'identique en secours, pour les deux genres.** ⛔ Le prédicat de
  refus ne prend en entrée **ni l'état de montage, ni l'origine de la requête**.
- **B3.** ⛔ **Aucune fenêtre d'acceptation non signée ne s'arme sur un état de l'appareil**
  (montage, redémarrage, absence de fichier). ⇒ **La piste 2 est retirée de la spec** (faite).
- **B4.** `setupRoutes()` et `server->begin()` s'exécutent **sans condition de montage**. La table
  de routes est **la même** dans les deux états, enregistrée par les mêmes fonctions `setup*Routes`.
- **B5.** ⛔ **Aucune variable d'état « dégradé »** n'est lue par `auth.cpp`,
  `otaUploadAuthorized`, `REQUIRE_AUTH` ni le prédicat de signature. **Vérifiable par grep.**
- **B6.** La page de secours est une **constante compilée**, servie par un **GET public à adresse
  fixe**, **toujours** enregistré, ⭐ **AVANT la garde 503** (`web_server.cpp:118-125` : *« toute
  route `HTTP_GET` ajoutée après ce point sera couverte par la garde »*). ⛔ Ni donnée
  d'exécution, ni ressource externe ; la version affichée est une **constante de compilation**.
- **B7.** `/` et `/index.html` servent la page de secours **quand l'interface est absente**.
  ⛔ Le déclencheur **n'est pas** le seul échec de montage : il couvre aussi **« fichier d'index
  absent »** et **« mise à jour FS commencée et non terminée »** (marqueur persistant posé avant
  `LittleFS.end()`, levé après succès). ⛔ **Aucun `LittleFS.begin(true)` nulle part.**
- **B10.** La page de secours fonctionne à l'état de **premier démarrage** (NVS vide **et** FS
  détruit) **par les routes existantes** — `/auth/login` répond 403
  (`web_routes_auth.cpp:46-49`), `/auth/change-password` est public dans cet état et rend le
  jeton (`:84-141`). ⛔ **Aucune exemption neuve** dans `checkAuth` ni `otaUploadAuthorized`.
  **Essai dédié.**
- **B11.** Interrupteur : **une fonction unique** de décision, lue par `/update` **et**
  `/download-update` · ⛔ **jamais armé automatiquement**, **absent** de la page de secours ·
  ⛔ **non armable sans heure valide** · en D3-b il **journalise** *« D3 refuserait : non —
  interrupteur actif, expire dans N min »*, ⭐ pour que l'armement et l'expiration soient
  **prouvés avant** D3-c.
- **B12.** Clôture de D3-b en **trois marches**, chacune lue **sur les lignes de verdict**
  (`authenticité : valide`, `liaison : liée`), **au navigateur**, ⛔ jamais en `curl` ni sur un
  code HTTP :
  1. appareil **sain**, parcours complet par la page de secours ;
  2. FS détruit **SANS** redémarrage, même parcours ;
  3. FS détruit **PUIS** redémarrage. ⛔ Exige un **feu vert explicite de l'utilisateur**, un
     **accès physique USB prêt**, et un couple `littlefs.bin` + `.sig` vérifié **juste avant** et
     ⛔ **non reconstruit** depuis.
- **B13.** La spec **et** `docs/UPDATE_GUIDE.md` écrivent le cas **« internet nulle part »** comme
  hors périmètre, et la procédure du mode point d'accès.

### Socle — D3-a
- **B8.** Le genre est résolu **UNE SEULE FOIS**, avant toute garde qui en dépend (octet magique,
  `g_uploadIsFilesystem`, `LittleFS.end()`, `Update.begin()`, choix de partition de
  `/download-update`). · Le genre signé ne fait autorité **que si le verdict est « valide »** ;
  sinon comportement actuel conservé. · Tout **désaccord** entre genre signé et genre de
  l'appelant est **journalisé** avec un libellé propre. · `test_native_ota_order_guard` est
  **étendu** pour figer cet ordre.
- **B9.** Relais du `.sig` par `/check-update` : **lecture bornée (1 Ko au plus)** · ⛔ l'appareil
  ne relaie **que ce qu'il a parsé et re-sérialisé**, plus une signature validée sur l'alphabet
  base64 et sa longueur — ⛔ **jamais les octets bruts** · la page insère ces valeurs en
  **`textContent`**, ⛔ jamais en HTML.
  ⚠️ **Raison** : le contenu relayé vient de la chaîne GitHub — **l'adversaire que la feature
  vise**.
- **B14.** L'arrêt terrain de D3-a (manifeste à travers `/download-update`) se clôt sur un
  **verdict relevé au journal dans la minute**, **pour les deux genres**.

⚠️ **Piège de preuve en D3-a et D3-b** : tant que **rien ne refuse**, ⛔ *« l'envoi a été
accepté »* **ne prouve rien**. La clôture se lit **dans les verdicts du journal**.

## ⚠️ Acceptable pour un réseau domestique privé — ⛔ mais à ÉCRIRE dans l'ADR
- **Premier démarrage ouvert** : tant que l'assistant n'est pas terminé, le mot de passe vaut
  `admin` (`auth.cpp:57-61`). `/auth/login` est bloqué, mais `checkBasicAuth` ⛔ **ne regarde pas
  l'état de premier démarrage** (`auth.cpp:107-113`, accepté par `otaUploadAuthorized` en
  `web_routes_ota.cpp:344`) ⇒ `/update` **semble** joignable en Basic `admin:admin` dans cette
  fenêtre. ⚠️ **Lu, NON essayé.** Aujourd'hui = firmware arbitraire ; après D3-c = **image signée
  seulement**.
- **`auth_enabled=false`** ouvre tout (`auth.cpp:200-202`) — choix de l'utilisateur, **borné par
  la signature après D3-c**.
- **HTTP en clair** : la page de secours fait circuler le mot de passe comme la page de connexion
  ⇒ **aucun coût neuf**.
- **Appareil volé** : mot de passe et jeton **en clair** en NVS. Connu, inchangé par ce lot.

## Cosmétique relevé
- La comparaison du **mot de passe** à la connexion ⛔ n'est **pas à temps constant**
  (`web_routes_auth.cpp:39`), alors que celle du **jeton** l'est.
- La page de secours hérite de la **CSP globale** (`web_server.cpp:86-87`) : le script **en
  ligne** y est permis, ⛔ **aucune ressource externe** ne doit y figurer.

## Specs de durcissement à ouvrir (ordre revu par ce verdict)
1. 🔴 **Mise en attente de l'image FS en PSRAM** — ⭐ **passe EN TÊTE** : avec des clients, **C1
   est la porte d'entrée** dans l'état « FS détruit », par erreur **ou** par malveillance du
   porteur du jeton.
2. **bug-112** (journal purgeable sans authentification) — ⚠️ il **conditionne la valeur** de
   toutes les preuves par journal : **B11, B12, B14**.
3. **Premier démarrage** : aligner `checkBasicAuth` sur le blocage de `/auth/login`, ou **assumer
   l'ouverture par écrit** dans l'ADR.
4. **Interrupteur par bouton PHYSIQUE** pour les appareils clients — ⭐ *le client a le bouton
   sous la main et n'a pas forcément MQTT pour témoin* ⇒ c'est la **forme naturelle** de
   l'interrupteur côté client. ⛔ Après D5.
5. **Bouton « réparer en ligne »** dans la page de secours, s'il est écarté de D3-b.
6. 🔴 **À soumettre à `pool-chemistry`** (hors mandat de `security-reviewer`) : **l'appareil
   doit-il continuer à DOSER quand son interface est détruite ?** ⚠️ C'est le cas **aujourd'hui**,
   et ce lot ⛔ **ne le change pas**.

## 🔴 DÉCISION UTILISATEUR (2026-11-15) — l'appareil CONTINUE de doser interface détruite

Question renvoyée par `security-reviewer` (point 6 de ses durcissements, hors de son mandat).
Réponse de l'utilisateur : *« oui, idéalement, l'appareil doit continuer à doser. »*

⇒ **C'est le comportement ACTUEL** : la régulation vit en RAM/NVS, ⛔ elle ne lit rien dans
`spiffs`. **Aucun code à écrire** — la décision **verrouille** ce comportement au lieu de le
changer. ⛔ `pool-chemistry` n'a donc pas de modification à valider : aucun chemin neuf vers
`pump_controller`.

⭐ **Et cette décision RENFORCE la condition B4** (table de routes complète sans système de
fichiers). Les deux se tiennent :
- un appareil qui dose **doit rester pilotable** — arrêt des pompes, consignes, mode manuel ;
- ⛔ aujourd'hui, interface détruite **après un redémarrage**, il dose **sans aucune prise HTTP**,
  avec MQTT pour seul canal. ⇒ **C'est l'état à supprimer**, et B4 le supprime.

⚠️ **Conséquence directe sur la marche 3 de B12** (FS détruite **puis** redémarrage) : pendant
l'essai, l'appareil **régule un bassin réel**. ⇒ ⛔ Ne pas lancer cette marche sans :
- la **filtration dans un état connu** et les plafonds journaliers/horaires **relus avant** ;
- MQTT **vérifié vivant** juste avant (seul témoin si le code neuf a un défaut) ;
- l'**accès physique USB prêt** (déjà exigé par B12).

---

# 🔴 D3-a — CONCEPTION RENDUE, et un constat qui RÉDUIT le lot (2026-11-15)

## ⭐ LE CONSTAT : « le genre signé fait autorité » est un **no-op**, et la condition 2 n'est PAS levée par D3-a

**Vérifié dans `src/ota_authenticity_logic.cpp`** (fonction `combineOtaAuthenticityVerdict`) :

```cpp
// Signature authentique : reste à vérifier la cohérence genre + plancher.
if (parsedKind != expectedKind) {
  return OtaAuthenticityVerdict::kKindMismatch;
}
```

⇒ **`kValid` IMPLIQUE DÉJÀ « genre signé == genre de l'appelant ».** Et comme l'autorité n'est
accordée **que** si le verdict est `kValid` (B8), **le genre effectif est toujours égal au genre
de l'appelant** : la réaffectation de `cmd` / `isFilesystem` est un **no-op prouvable**.

⛔ **Trois conséquences, à ne pas rater au moment de planifier D3-c :**
1. ⭐ **Le risque G4 du P1-bis est NUL en D3-a** (« une garde aurait jugé l'ancien genre ») : le
   genre ⛔ **ne change jamais**. L'invariant « D3-a ne refuse rien » est tenu **par
   construction**, ⛔ pas par vigilance.
2. 🔴 **La condition 2 du P1 est en réalité implémentée par le REFUS `kKindMismatch` de D3-c**,
   ⛔ pas par l'autorité de genre. ⚠️ **À écrire tel quel** — sinon D3-c sera planifié comme s'il
   restait quelque chose à faire sur le genre, alors que **tout y reste à faire**.
3. Ce que A1 apporte **malgré tout**, et qui justifie de l'écrire quand même : le **drapeau de
   désaccord** est calculé et journalisé **même hors `kValid`** (⭐ seul libellé neuf réellement
   observable en D3-a), et il existe **un seul site** de résolution le jour où le verdict cessera
   de porter le genre.

## ⇒ Ce qui reste de VRAI CONTENU dans D3-a
| Item | Contenu réel |
|---|---|
| **Condition 2** (autorité du genre) | ⚠️ **no-op** — un drapeau de désaccord journalisé, rien de plus |
| **Condition 9** (verdicts distincts) | ⚠️ **déjà en place** : `otaAuthenticityVerdictLabel` rend **7 libellés distincts**, dont « vérification impossible ». Reste à **étendre aux libellés neufs** |
| **Condition 3** (source unique d'empreinte) | ⭐ **vrai travail** — avec la tolérance ci-dessous |
| **B9** (relais du `.sig`) | ⭐ **vrai travail** — c'est le gros du lot |
| **C1-C3** (`app.js` transporte le manifeste) | ⭐ **vrai travail**, 3 lignes mais c'est l'arrêt terrain |

## Arbitrage (a) — condition 3 contre l'invariant : **tolérance NOMMÉE et DATÉE**
⚠️ Basculer la source d'empreinte vers le manifeste peut faire **apparaître un refus
d'intégrité** quand manifeste ≠ paramètre — ⛔ interdit par l'invariant n°1. En exploitation
normale l'écart est **impossible** (les deux valent le SHA-256 du même fichier) ; il n'apparaît
que sur **défaut d'outillage** (⚠️ un `.sig` d'une autre version joint à la release — **déjà vu
sur ce projet**) ou sur attaque.
⇒ **Retenu** : la source **bascule réellement** (⭐ sinon l'arrêt terrain B14 ne prouverait rien
et D3-c exercerait en production un chemin jamais parcouru), et **un bloc unique par route**
absorbe le seul refus neuf possible, en journalisant en **CRITICAL** *« divergence manifeste ↔
paramètre — TOLÉRÉ EN D3-a, D3-c refusera »*.
⭐ **Critère de clôture mécanique** : `grep -c "TOLÉRANCE D3-a" src/web_routes_ota.cpp` doit
rendre **4** à la fin de D3-a et **0** à la fin de D3-c. ⇒ Diff de suppression attendu : **≈ 16
lignes**, rien d'autre.

## Arbitrage (b) — où résoudre le genre
**Dernière instruction du bloc d'authenticité déjà figé** par les gardes structurelles [G5]
(`:644-671`) et [G6] (`:1656-1690`). ⛔ Le bloc n'est **pas déplacé** — on lui ajoute une sortie.
Les **cinq** dépendants sont en aval (+9, +175, +193, +200 lignes, et +30/+95 pour le choix de
partition), et le point unique reste **en amont** de `g_uploadOwner`, `setOtaInProgress(true)` et
de tout `esp_partition_erase_range()`.

## Arbitrage (c) — le relais du `.sig` : **session séparée, après le parse, NON BLOQUANT**
⛔ **Pas « dans la session TLS déjà ouverte »** : les `.sig` sont sur
`objects.githubusercontent.com`, **hôte différent** d'`api.github.com` ⇒ handshake neuf
obligatoire. Position : **après `heap_caps_free(rawBuffer)`** — ⭐ le **creux mémoire** de la
requête, pas son pic.
⚠️ **Coût mesuré, visible utilisateur** : `/check-update` passe de **~4 s à ~10 s pire cas**
(2 handshakes + 2 × 5 s de délai borné). TWDT 30 s tenu, `esp_task_wdt_reset()` dans la boucle.
⛔ La page n'a **aucun `AbortController`** sur cet appel (`app.js:10795`) — vérifié.
**Les 7 cas d'échec** (URL absente, hôte non autorisé, HTTP ≠ 200, corps > 1 Ko, délai, parse
hors grammaire, garde TLS fermée) ⇒ **champs vides + 200**, et les deux genres sont
**indépendants**.
⭐ **`kOtaCheckJsonBudgetBytes` reste INCHANGÉ** — et c'est le résultat important : il plafonne le
document de **parse de la réponse GitHub**, qui ne grossit pas. Le bouger déplacerait
**mécaniquement** `kOtaTlsMinPsramLargestBlockBytes` et `kCheckUpdateMinFreeHeapBytes` via les
`static_assert` de `constants.h:1371-1376` ⇒ ⛔ **durcir la garde TLS de 1,3 Ko sur le chemin
exact de bug-165/bug-173**, pour une croissance qui ne les concerne pas.

## ⚠️ Écart à déclarer (hors condition 10, mais à dire)
`refuseIfTlsGateClosed` (`:1084`) **envoie** un 503. Le relais ouvre une session sortante neuve
et doit donc être précédé de la garde — ⛔ **mais sans refuser**. ⇒ Extraire le **prédicat** en
`static bool otaTlsGateClosed(...)` ; `refuseIfTlsGateClosed` l'appelle et **conserve son 503 à
l'identique** ; le relais appelle le prédicat seul et **se saute** si la garde est fermée.
⛔ `ota_signature.*`, `ota_manifest_logic.*` et `combineOtaAuthenticityVerdict` restent **octet
pour octet identiques** — condition 10 tenue.

## ⚠️ TROISIÈME copie de la grammaire du `.sig` — le défaut le plus probable de ce lot
Elle existera en **shell** (`tools/check_release_artefacts.sh:211-246`), en **JS**
(`data/app.js:11156`) et désormais en **C++**. ⇒ La suite native **doit reprendre la matrice de
vecteurs** de `test/sig_parse_vectors.js` (ADR-0099) et le **dire en commentaire**. ⛔ Une
divergence silencieuse entre les trois est le risque n°1.

## ADR
**ADR-0122 neuf** — « Le socle de la décision OTA : genre, empreinte et relais du `.sig` ».
⛔ Pas une extension : ADR-0120 tranche le **schéma cryptographique**, ADR-0121 le **format de
transport** ; ici c'est la **conception d'exécution** (où l'on décide, à partir de quoi, qui va
chercher quoi). Il doit porter **le no-op du genre**, la **tolérance datée** et son critère de
suppression, le **CORS** comme cause du relais, et les rappels du P1 (vecteur « jeton admin
volé » **ouvert**, mot de passe et jeton **en clair en NVS, sans sel**).

🔴 **Correction à porter dans ADR-0121** (`docs/adr/0121-…md`, section Contexte, dernier
paragraphe) : il annonce le relais *« au sein de la session TLS déjà ouverte »* — ⛔ **impossible**
(hôte différent, session close avant le parse). **Une ligne d'amendement datée** renvoyant à
ADR-0122 ; ⛔ ne pas réécrire la décision.

## 🔴 DÉCISION UTILISATEUR (2026-11-15) — indicateur de progression sur « Vérifier »
Le relais du `.sig` fait passer `/check-update` de **~4 s à ~10 s pire cas**. L'utilisateur :
*« pour les 10 secondes, j'aimerais un indicateur de progression. »*
⇒ ⚠️ **C'est une surface UI** ⇒ `ux-ui-designer` passe **AVANT** `web-ui-developer` (règle non
négociable), **sur la carte entière** « Mise à jour en ligne (GitHub) » (`index.html:2645`),
⛔ pas sur le seul indicateur — et **avec les captures jointes**, à 390 px **et** 1024 px.
⚠️ Son mandat inclut l'**état d'attente** (que dit la carte pendant ces 10 s ?), l'**état
« release non signée »** et l'**état « relais indisponible »**.

## 🎨 Étude `ux-ui-designer` — RENDUE (2026-11-15), sur capture de la carte réelle

**Retenu : spinner DANS le bouton, et le bouton n'est PAS désactivé.**
- `.btn__spinner` 16 px + libellé **« Vérification… »** (13 caractères). ⛔ Pas de `disabled` : le
  bouton reçoit `aria-busy="true"` / `aria-disabled="true"` et un garde-fou JS ignore les clics.
  ⭐ **Trois raisons** : il garde l'**opacité pleine** (donc **lisible**), il garde le **focus
  clavier** (un `disabled` le ferait perdre au lecteur d'écran), et ⛔ il n'oblige à toucher
  **aucun composant partagé**.
- Ligne d'état neuve `#check_update_status` (`.muted small`, `role="status"`, `aria-live`)
  **SOUS** la rangée de boutons : *« Interrogation de GitHub, jusqu'à 10 s… »* (38 caractères,
  tient à 390 px). ⚠️ **Placée au-dessus, elle pousserait les boutons de 25 px sous le doigt.**
- ⛔ **Pas de barre de progression** : `.progress` porte un **pourcentage** (`width:%`, libellé
  « 0% ») — une barre indéterminée afficherait un **faux 0 %**. ⛔ Pas de bandeau non plus
  (+48 px de hauteur pour 10 s, sur un écran déjà trop haut).
- ⚠️ **Point de rendu signalé, non vérifié à l'image** : `.btn--loading` met
  `color: transparent`, donc son `::after` (`border-top-color: currentColor`) est
  **probablement invisible**. ⇒ C'est pourquoi `.btn__spinner` est retenu (sa couleur suit le
  texte). **À confirmer sur capture** après implémentation.

### ⛔ Ce qui ne s'affiche PAS — et c'est un choix argumenté
**Rien** sur « signée / non signée », l'empreinte ou le manifeste. ⭐ *Afficher « signée »
laisserait croire à une protection active alors que **rien n'est refusé**.* Les deux états neufs
(**release non signée**, **relais indisponible**) ⇒ **silence** côté page, journal côté appareil.
⇒ **À réévaluer en D3-b**, quand un refus existera : le **message de refus** sera alors le seul
élément visible.

### Ce qui DOIT disparaître de la carte — 3 lignes sur 5
- **« Version actuelle »** : répète « Firmware 2.86.0 » de la carte **juste au-dessus**.
- **« Release : »** : presque toujours égal à « Dernière version » ⇒ à garder **seulement s'il
  diffère**.
- **« Date »** : à **fusionner** sur la ligne « Dernière version : v2.87.0 · 18/10/2026 ».
- ⚠️ Hors lot, pour **feature-169** : replier dans un `<details>` les lignes d'infos système de
  faible usage (Build, CPU, Chip, Flash, MAC, PSRAM).
- ⚠️ Le texte d'aide de « Mise à jour manuelle » (*« Facultative pour l'instant… »*) **sera
  périmé en D3-b**.

### `.btn:disabled` — ⛔ PAS touché dans ce lot, et c'est la bonne décision
L'agent a **refusé de le faire évoluer** faute d'inventaire exhaustif garantissable — ⭐ c'est
exactement la discipline imposée (⛔ l'application partielle est interdite). Ce qu'il a **mesuré**
et qui part dans **feature-169** :
- **141** lignes `.btn` dans `index.html`, **4** dans `wizard.html`, **3** dans `wifi.html` ;
  **26** règles `:disabled`/`[disabled]` dans `app.css`.
- 🔴 **`.btn:disabled` est défini DEUX FOIS** (`app.css:539` et `:3579`) — la seconde copie n'a ni
  `transform` ni `box-shadow`. ⇒ Fusionner **avant** toute règle de lisibilité.
- S'y ajoutent les recolorations de feature-164 (DESIGN_SYSTEM §430-444) et le verrouillage
  croisé de calibration (§349).

### ⚠️ Une alerte de l'agent CORRIGÉE après vérification
Il signale `data/app.js:10817` et `:10840` comme une injection `innerHTML` de `data.message`
*« qui peut venir de la chaîne GitHub »*. **Vérifié : c'est FAUX aujourd'hui** — `message` est un
**littéral du firmware** (`web_routes_ota.cpp:1257` : *« Aucune release disponible sur GitHub »*,
et `:1596` dans un 422 construit en C). ⛔ **Ce n'est pas une faille vivante**, et ⛔ ce lot ne la
traite pas. ⚠️ **Mais le motif reste à durcir le jour où un champ relayé atteindra le DOM** — en
D3-b, quand un message de refus s'affichera.

### Design system
Le motif d'attente **existe** (§103 + `.btn__spinner`, employé à **7 endroits** de `app.js`) ⇒
⭐ **réemploi, rien de neuf**. À ajouter en **§5.4** : ce motif s'emploie **sans `disabled`**, avec
`aria-busy`, sur une carte de réglage.
⇒ **Capture à refaire** après implémentation : la carte **pendant l'attente**.

---

# 🔒 D3-a — VERDICTS DE REVUE (2026-11-15)

`security-reviewer` **P2 : GO SOUS CONDITIONS** (4) · `code-reviewer` : **Request changes** (2)
⭐ **Les deux convergent sur le même défaut matériel**, trouvé indépendamment : les sessions TLS
sortantes **ne sont pas bornées**.

## 🔴 C1 — le relais peut faire REDÉMARRER l'appareil (grave, confirmé dans le code)
⛔ **Vérifié** : `grep` ne trouve **aucun** `setTimeout` / `setHandshakeTimeout` /
`setConnectTimeout` dans `src/web_routes_ota.cpp`.
- la tâche `async_tcp` est sous watchdog **30 s** (`kWatchdogTimeoutSec`) ;
- le délai de **poignée de main TLS** par défaut est de **120 s** et **rien ne le borne**.
⇒ 🔴 Sur réseau dégradé, **« Vérifier » redémarre l'appareil** — ⚠️ et c'est le **chemin de
réparation du client**.
⚠️ **Le défaut PRÉEXISTE** sur le GET de l'API ; D3-a **triple l'exposition** en ajoutant deux
sessions. ⛔ Le commentaire « TWDT 30 s tenu » de `constants.h` **n'est donc pas garanti** — à
corriger aussi.
⇒ **Correctif demandé** : borner connexion + handshake + lecture des **trois** sessions, nourrir
le watchdog **avant chaque** GET, et **écrire le calcul du pire cas** en commentaire.

## 🔴 LE POINT LE PLUS FIN DE LA REVUE — D3-a introduit une **ACCEPTATION** neuve, non déclarée
⚠️ L'invariant était écrit *« ⛔ aucun REFUS neuf »* — et il est resté **muet sur l'acceptation**.
**Mesuré** (`web_routes_ota.cpp:842-867` et `:1948-1968`) : si le verdict est `kValid` et que
l'image calculée égale l'empreinte **du manifeste** mais **pas** le paramètre `sha256`/`digest`,
⇒ elle était **REFUSÉE avant**, elle est **FLASHÉE maintenant**.
⭐ **C'est voulu** (arbitrage (a) : « la source bascule réellement »), et c'est **borné** par une
signature RSA-3072 vérifiée **plus** le plancher de version.
⛔ **MAIS le plancher est INERTE à 0.0.0 en D3-a** ⇒ la borne haute est réelle, la borne basse ne
l'est pas.
🔴 **Vecteur à nommer** : un **asset remplacé** fait installer une **autre image signée** que
celle demandée — un retour à une version antérieure **légitimement signée**, par exemple.
⇒ ⚠️ **À traiter en D3-b** : lier l'image au **genre ET à la version demandés**, pas seulement à
un manifeste authentique. ⇒ **À écrire dans ADR-0122 maintenant**, pas au moment où ça servira.

## Conditions de l'audit P2 — ⛔ à lever
1. 🔴 **Borner** connexion, handshake et lecture de **chaque** GET, nourrir le watchdog avant
   chacun, pire cas cumulé **sous 30 s** avec marge. *(en cours)*
2. **Avant D3-c** : `/check-update` doit distinguer **« release non signée »** de **« relais
   échoué »**, et la trace de saut doit porter **le motif et les valeurs mémoire**.
3. **Déclarer l'écart de périmètre** : les retraits de lignes de la carte appartiennent au
   **chantier de simplification**, pas au socle — ⇒ déclaré ici, ⛔ pas masqué. Idem pour la
   correction de la garde `ota_update.sh`.
4. 🔴 **B14 — ce que la séquence terrain doit prouver**, pour **les deux genres** :
   - relais **« validé »** au journal ;
   - authenticité **« valide »** sur `/download-update` ;
   - **liaison conforme aux DEUX stades** ;
   - ⛔ **aucun** « désaccord de genre » ;
   - ⛔ **aucun** CRITICAL « TOLÉRÉ EN D3-a » ;
   - version **et** système de fichiers constatés **après redémarrage**.
   - ⭐ **Contre-épreuve** : une release **non signée** s'installe toujours (verdict « absente »).
   ⛔ *« L'envoi a été accepté » ne prouve RIEN* : D3-a accepte à l'identique une signature
   **absente**, **invalide**, ou un **relais sauté**.

## Écarts soumis à la revue — verdicts rendus
| Écart | Verdict |
|---|---|
| `static_assert` dans le module, pas dans `constants.h` | ✅ **correct** — `constants.h` doit rester sans dépendance |
| 4 `String` dans `/check-update` | ✅ **acceptable** — ≤ 512 o, au **creux mémoire**, sans effet sur `max_alloc_8bit` (~21 Ko). ⭐ Gratuit si souhaité : déclarer `response` **avant** les relais et y affecter `g_sigRelayPayload.canonical` (ArduinoJson copie) |
| Tolérance en **lambda** | ✅ **mieux** que deux blocs copiés — et le `grep` rend bien **4** |
| `sigParams()` tout-ou-rien | ✅ **correct** — un manifeste sans signature est inexploitable |
| Extraction `otaTlsGateClosed` | ✅ **acceptable** — décision et 503 **identiques** |

## Points vérifiés et tenus
Genre résolu **une seule fois** avant les cinq dépendants · autorité limitée à `kValid` (no-op
**prouvé** par `kKindMismatch`) · ⭐ **la tolérance ne masque aucun refus légitime** (elle exige
`calculée == paramètre appelant`, donc une image corrompue **reste refusée**) · **condition 10
tenue** · relais borné à 1 Ko, **canonique seul**, 7 échecs ⇒ 200 + champs vides · ⛔ aucun
statique pendant, **aucune fuite entre genres** · authentification **toujours première
instruction** · `textContent` partout, `finally` restaure **sur erreur comme sur succès**.

## Mineurs à ne pas perdre
- ⚠️ **Le « jusqu'à 10 s » de la page est sous-estimé** : pire cas réel ~20 s avec deux relais.
  ⇒ **À recalculer une fois les délais bornés** (C1), et à corriger dans le libellé.
- 🔴 La matrice `.sig` C++ est **RECOPIÉE** de `test/sig_parse_vectors.js`, ⛔ pas extraite
  (ADR-0099). Tolérable tant que rien ne refuse ⇒ **à mécaniser AVANT D3-c**.
- ~~`#no_update_msg` reste en `innerHTML` (`app.js:10841`)~~ — ⚠️ **PÉRIMÉ, corrigé le
  2026-11-16** : bug-178 a réécrit ce bloc en `replaceChildren()` + `createElement` +
  `textContent` (`data/app.js:11121-11181`), avec le commentaire *« Construction DOM (jamais
  innerHTML : latest_version vient de GitHub) »*. ⇒ L'item ne disparaît pas, il **change de
  nature** : il reste à poser une **garde mécanique anti-régression** (D3-c y affichera un
  message **relayé**) et à auditer les autres insertions de la carte.

## 🔴 La CI était ROUGE depuis le 2026-11-14 — et trois commits ont été poussés par-dessus
`test_native_ota_update_fail_closed` **[F1]** exigeait le contrôle d'échec de `compute_sha256`
*« EXACTEMENT une fois »*. ⛔ Le mode `--sign-only` de D2 (`44cc478`) a ajouté un **second site
d'appel LÉGITIME** (`sign_only_file()`, **hors réseau**) ⇒ la garde **comptait**, et le compte a
changé. ⚠️ **Elle n'a détecté aucun défaut : elle a vieilli.**
⇒ **Corrigé** : la garde ne compte plus. Elle vérifie **deux propriétés qui ne vieillissent pas** —
(1) ⛔ **aucun** appel à `compute_sha256` n'est **non gardé** ; (2) `update_file()` en porte un
**AVANT** l'appel `curl` réel.
⇒ **Validé par MUTATION** : un appel rendu non gardé dans `update_file()` **tue** le test
(`Expected 2 Was 1`), et `ota_update.sh` est restauré (diff vide).

## Mesures de clôture des tests — ⛔ MESURÉES, pas recopiées
**80 suites · 2 402 cas · 2 402 réussis, 0 échec.** `pio run` SUCCESS, 0 warning, Flash **51,2 %**,
RAM 29,7 %. `build_fs.sh` SUCCESS. `check_sig_parse_vectors`, `check_memory_discipline`,
`check_fopen_guard` : verts.
**Les 10 mutations imposées sont TUÉES.** ⚠️ Deux nuances honnêtes relevées par `test-engineer` :
la mutation 9 n'est tuée que si **les trois gardes** (longueur, alphabet, décodage à 384 o) sont
retirées **ensemble** — ⛔ ce sont des **mutants équivalents**, de la défense en profondeur, **pas
un trou** ; et la mutation 7 a dû muter vers une copie **brute avec CRLF**, le brut étant déjà
canonique pour une entrée strictement valide.

## ✅ C1 LEVÉE (2026-11-15) — les trois sessions sortantes sont bornées
Nouvelles constantes (`constants.h`, zone `kOta*`) : `kOtaTlsConnectTimeoutMs` **3000**,
`kOtaTlsHandshakeTimeoutSec` **5** (⚠️ cette API prend des **secondes**, pas des millisecondes),
`kOtaTlsSocketTimeoutMs` **3000**. Posées sur **les trois** sessions — les deux `.sig` **et le GET
de l'API préexistant**. `esp_task_wdt_reset()` **avant chaque** GET.

🔴 **Le développeur a corrigé MON cadrage, et il a raison.** J'avais demandé que le **cumul des
trois sessions** reste sous 30 s : ⛔ **c'est la mauvaise borne**, et elle est intenable
(~49 s). Le watchdog mesure le temps **depuis le dernier `reset()` sur la tâche**, pas le temps
total d'une requête. ⇒ **Ce qui compte est la plus longue attente SANS le nourrir.**
- Pire GET bloquant = un `.sig`, qui suit une **redirection** (`github.com` →
  `objects.githubusercontent.com`) donc **2 connexions** : `2 × (3 + 5) + 3 = ` **19 s**
  ⇒ **11 s de marge** sous le TWDT.
- GET de l'API (1 connexion) : `3 + 5 + 3 = ` **11 s** avant le corps, dont la lecture est nourrie.
- ⭐ **Un `static_assert` verrouille l'inégalité** (`constants.h:1436`) : elle ne peut plus être
  cassée en silence par un réglage de constante.

⚠️ **Non bornable, et écrit en commentaire** : la **résolution DNS** précède la connexion et
cette API ne permet pas de la borner.

## ⚠️ Le libellé « jusqu'à 10 s » était FAUX — corrigé
Avec deux relais bornés, le pire cas de « Vérifier » n'est plus 10 s. ⛔ Annoncer « jusqu'à 50 s »
serait alarmant et presque toujours faux ; annoncer « 10 s » est **mensonger dans le cas qui
compte**. ⇒ Le nombre est **retiré** : *« Interrogation de GitHub, quelques secondes… »*
(43 caractères, tient à 390 px).

## ⛔ CE QUI N'EST TOUJOURS PAS VÉRIFIÉ — l'indicateur n'a été VU par personne
Une capture a été tentée avec clic sur `#check_update_btn` : ⛔ **sans effet**. En capture sans
navigateur connecté, la page est en état **« Déconnecté »** et **tous les boutons sont
désactivés** ⇒ le clic ne part pas. ⚠️ **Cette capture ne prouve donc RIEN** sur l'état d'attente.
⇒ **L'observation se fera sur l'appareil**, après déploiement : appuyer sur « Vérifier » et
regarder. ⛔ Ne pas clore le lot en affirmant que l'indicateur fonctionne.

## Mesures après C1
`pio run` SUCCESS (Flash **51,2 %** ; seuls warnings : bibliothèque OneWire, **préexistants**) ·
`./build_fs.sh` SUCCESS · **80 suites, 2 402 cas, 2 402 réussis** · `grep -c "TOLÉRANCE D3-a"` = **4**.

## ✅ ESSAI DE TERRAIN (2026-11-15) — D3-a VALIDÉ en production, et **deux défauts trouvés**

**L'appareil est passé de 2.87.0 à 2.87.1 par la page, en ligne, de bout en bout.** C'est le
premier parcours réel complet de la chaîne signée.

### 1. L'indicateur de progression a été VU — la réserve ci-dessus est levée
Mots de l'utilisateur : *« j'ai vue l'animation du spinner avec un message qui dis que ça
vérifie sur Github, quelques secondes »*. ⇒ Le libellé et l'attente tiennent sur téléphone.

### 2. Le relais fonctionne — mesure
`GET /check-update` : **HTTP 200 en 11,4 s**, `update_available` vrai, et les deux relais
peuplés lors d'un essai en `curl` :
`pool-ota/1 firmware 2.87.1 sha256:c39b2947…4dd6` et
`pool-ota/1 filesystem 2.87.1 sha256:77fc24ae…4cfa`, signatures de 512 caractères.
⇒ Les 11,4 s **confirment le calcul de C1** et rendent le libellé « quelques secondes » honnête.

### 3. 🔴 DÉFAUT — le relais du filesystem a été SAUTÉ lors de l'essai depuis le téléphone
```
12:19:37 INFO /check-update : relais .sig firmware   — validé (612 o)
12:19:37 WARN /check-update : relais .sig filesystem — garde TLS fermée (mémoire)
```
Conséquence à l'installation, journal persistant :
```
12:32:16 WARN /download-update — authenticité OTA : ABSENTE — D3 refuserait : OUI   (filesystem)
12:32:25 INFO /download-update — authenticité OTA : valide  — D3 refuserait : non   (firmware)
12:32:55 INFO /download-update — liaison manifeste/image : LIÉE                      (firmware)
```
⇒ **Le firmware a été authentifié, le filesystem non.** En D3-c, ce filesystem aurait été refusé.

**Mesure de la cause** : heap interne 8 bits **libre = 31 024 o** contre un seuil de
**32 768 o** — ⚠️ **il a manqué 1 744 o**, soit 5 %. Les deux autres critères étaient tenus
(plus gros bloc interne 28 660 o ≥ 24 576 ; PSRAM 4 128 756 o ≥ 40 960). Cause structurelle :
**trois sessions TLS successives dans une seule requête** ; le heap ne remonte pas entre elles
(47 388 o à l'entrée, 31 024 o au troisième appel). ⚠️ **Intermittent** — le même appel en
`curl` quelques minutes plus tôt avait ramené les deux signatures.

⇒ ⛔ **BLOQUANT POUR D3-c** : livré tel quel, le refus frapperait **au hasard** les mises à
jour du filesystem. ⇒ Spec ouverte : **`bug-176`**.

### 4. ✅ La condition C2 n'est plus théorique — elle est DÉMONTRÉE
« Release non signée » (`web_routes_ota.cpp:1286`) et « relais sauté » (`:1296`) produisent
tous deux **des champs vides** : la page ne peut pas les distinguer. C'est très exactement la
situation que C2 annonçait. ⇒ Traitée dans `bug-176`, à lever avant D3-c.

### 5. 🔴 Angle mort d'observabilité
Le WARN de saut (`web_routes_ota.cpp:1296-1298`) **ne journalise aucune mesure** — ni heap, ni
seuil. Le diagnostic n'a été possible que parce que la sonde « Chute maxBloc8 » s'est
déclenchée à la même seconde, **par coïncidence**. ⇒ Sans elle, le défaut était
**indiagnosticable à distance**. Traité dans `bug-176` (livrable A3).

### 6. 🔴 DÉFAUT CONNEXE — la page abandonne l'utilisateur au redémarrage
`data/app.js` attend **quinze secondes en aveugle** puis recharge **une seule fois**, sans
jamais vérifier que l'appareil est revenu. Relevé : redémarrage journalisé à 12:33:00, mDNS
réannoncé à **12:33:11** ⇒ **quatre secondes de marge**. L'utilisateur est tombé sur la page
d'erreur de son navigateur, sans explication et sans recours, **alors que l'appareil allait
parfaitement bien** (vérifié : il répond en mDNS, `poolcontroller.local → 192.168.1.48`,
TTL 120 s). Mots de l'utilisateur : *« ce n'est pas un comportement normal. Un utilisateur
serait perdu »*. ⇒ **Hors périmètre de feature-151**, spec à ouvrir après `bug-176`.

### 7. Contre-test encore à faire
Une release **non signée** doit toujours s'installer, dans les deux genres. ⛔ Non vérifié à ce
jour — à couvrir par `bug-176` (plan de test, scénario 4).

## ✅ CONDITION C2 LEVÉE (2026-11-15, bug-176 / ADR-0123, v2.88.0)

**C2 = distinguer « release non signée » de « relais indisponible ».** Levée par ce mécanisme :

- `/check-update` mémorise les URL des `.sig` et rend `firmware_sig_status` / `filesystem_sig_status` (+ `*_sig_reason`). `absent` (`no_asset`) n'est émis **que** si la réponse GitHub a été analysée **avec succès** et ne contient pas d'asset de ce nom (état `kCheckedNoAsset`). Tout autre doute — jamais vérifié, URL trop longue, hôte refusé, mémoire, horloge, HTTP, délai, grammaire, anti-déni, **y compris une valeur d'état hors énumération** — est `unavailable` + motif d'une énumération fermée.
- `GET /ota-sig?kind=` porte le même couple `status`/`reason` au moment de la demande de signature.
- La page traite tout statut inconnu comme `unavailable`, ne dit rien pour `absent`, et affiche une ligne dédiée pour `unavailable`.
- ⇒ En D3-c, le refus pourra dire **pourquoi** : « non signée » (`absent`) n'est plus confondu avec « non récupérée » (`unavailable`). Voir [ADR-0123](../../../docs/adr/0123-une-seule-session-tls-sortante-par-requete-http.md) et [`bug-176`](bug-176-relais-sig-filesystem-saute-sous-pression-memoire.md).

⚠️ Reste à D3-c : décider **quel statut refuse** (`unavailable` ⇒ refus ? `deferred` jamais résolu ?) — une décision de lot, pas de C2.

## 🔴 CONTRAINTE D'ORDRE POUR D3-c — à ne pas rater (2026-11-15, ADR-0123)

**Le lot D3-c (le refus) ne doit PAS sortir dans le même firmware que bug-176 (2.88.0), ni dans une release dont le `littlefs.bin` n'a pas encore été installé partout.**

Pourquoi : depuis 2.88.0 la signature n'est plus dans `/check-update` mais dans `GET /ota-sig`, appelé par la **page**. Une page **ancienne** (filesystem pas encore mis à jour) lit les anciens champs `*_manifest` / `*_signature`, **désormais toujours vides**, et envoie donc des paramètres vides à `/download-update`. Avec le refus de D3-c, elle serait **refusée — y compris pour installer le filesystem qui la corrigerait** : impasse pour le client.

Conditions pour lancer D3-c :
1. une release publiée dont le **filesystem porte la page neuve** (celle de 2.88.0), et installée sur l'appareil ;
2. une **voie de réparation indépendante de la page** documentée et prouvée (script `ota_update.sh` signant, ou page de secours de D3-b) ;
3. `grep` de la page déployée : `/ota-sig` présent dans `app.js` avant d'écrire les `if` de refus.

⛔ Mécanisme de verrouillage : la case « D3-c » du plan de lots reste décochée tant que 1 et 2 ne sont pas consignés ici avec leur date.

### ✅ S6 — LES TROIS PRÉALABLES DE D3-c SONT CONSIGNÉS (2026-11-16, 21:35)

⛔ Le verrou ci-dessus (« la case D3-c reste décochée tant que 1 et 2 ne sont pas consignés ici avec
leur date ») est **levé par ce bloc**. Les trois conditions étaient **vraies dans les faits** et
n'étaient écrites nulle part — c'est exactement ce que le verrou devait empêcher.

| # | Condition | État | Preuve, datée |
|---|---|---|---|
| **S6-1** | Release publiée dont le `littlefs.bin` porte la page neuve, **et installée sur l'appareil** | ✅ | `gh release list` : **v2.88.2** (2026-11-15), assets `littlefs.bin` **et** `littlefs.bin.sig` présents. La page neuve date de 2.88.0 (ADR-0123). L'appareil tourne un filesystem construit depuis ce `data/` — voir S6-3. |
| **S6-2** | Voie de réparation **indépendante de la page**, documentée **et prouvée** | ✅ | **Deux** voies, toutes deux prouvées : (a) `./ota_update.sh --use-sig` en ligne de commande, employé six fois le 2026-11-16 ; (b) la **page de secours** de D3-b1, éprouvée par **B12 marche 3** le 2026-11-16 — filesystem détruit **puis** redémarrage, réparation au navigateur, verdicts `valide`/`liée`. ⇒ Rattachées à S6, ce qui n'avait jamais été fait. |
| **S6-3** | `/ota-sig` présent dans l'`app.js` **servi par l'appareil** | ✅ | `curl --compressed http://192.168.1.48/app.js` à **21:30 le 2026-11-16** : 310 182 o, **1 occurrence** de `ota-sig`. ⚠️ Mesuré sur ce que l'appareil SERT, pas sur `data/` local — c'est la seule mesure qui vaut. |

⚠️ **S6 complétée — l'onglet rechargé.** La condition ajoutée le 2026-11-15 (« l'onglet doit aussi
avoir été rechargé côté utilisateur ») ⛔ **ne peut pas être prouvée depuis l'appareil** : un onglet
ouvert depuis la veille exécute du JavaScript ancien, et rien côté serveur ne le voit. ⇒ Elle est
traitée **par l'interface**, pas par une preuve : le refus doit distinguer « page périmée, rechargez »
de « signature absente » (voir la décision U2 ci-dessous).

### ✅ DÉCISIONS UTILISATEUR DU 2026-11-16 (21:33) — à ne pas rouvrir

1. ⭐ **Aucun appareil chez un client.** Seul l'appareil de l'utilisateur (192.168.1.48) est en
   service. ⇒ Le risque d'immobilisation ne porte que sur lui, et la fenêtre de déploiement de D3-c
   n'a pas à être coordonnée avec une flotte. ⚠️ **Les messages de refus restent à soigner** : le
   produit aura des clients, et c'est la phrase affichée qui les sauvera, pas le `if`.
2. ⭐ **`kVerificationImpossible` est REFUSÉ** (fail-closed). Un échec d'outillage cryptographique
   (allocation, clé illisible) **ne passe pas**. ⚠️ Conséquence acceptée : un appareil en pénurie
   mémoire ne peut plus être mis à jour à distance **sans** l'autorisation temporaire — qui est la
   porte de sortie, et elle est prouvée (F0). L'alternative (fail-open) créait une condition
   **provocable** par le porteur du jeton.
3. ⭐ **Le refus TARDIF est accepté.** Une image à signature **valide** dont les octets divergent du
   manifeste sera refusée **après** le début de l'écriture, laissant le filesystem détruit. ⚠️ C'est
   une **nouvelle façon de casser un appareil**, acceptée sciemment : B12 marche 3 a prouvé ce soir
   que cet état se répare au navigateur, et ce cas suppose un outillage cassé ou une attaque, jamais
   un client ordinaire.

### ✅ U2 — LA PAGE PÉRIMÉE : option (1) seulement (2026-11-16)

La spec proposait deux voies. **Seule la (1) est retenue** : la page annonce sa version, et le refus
distingue « page périmée, rechargez » de « signature absente ».

⛔ **L'option (2) — le firmware TOLÈRE l'absence de paramètres d'une page qu'il « sait » ancienne —
est un NO GO de conception** : l'indication de version viendrait de **l'appelant**. N'importe qui
prétendrait être une vieille page et obtiendrait une acceptation non signée à la demande. Cela
**annulerait D3-c** et violerait **B2** (le prédicat ne prend en entrée ni l'état de montage ni
l'origine de la requête) et **B3** (aucune fenêtre d'acceptation armée sur un état).
⇒ ⚠️ L'indication de version fournie par la page est une **aide au message**, ⛔ **jamais une preuve**
et ⛔ jamais une entrée du prédicat de refus. À confirmer par `security-reviewer` P1.

### ✅ QUEL STATUT REFUSE — décision de lot (2026-11-16)

La question ouverte (« `unavailable` ⇒ refus ? `deferred` jamais résolu ? ») est tranchée **par la
sémantique déjà écrite dans `src/ota_sig_relay_logic.h:69-72`**, condition S2 :

> « `absent` n'est émis QUE si la réponse GitHub a été analysée AVEC SUCCÈS et ne contient pas d'asset
> de ce nom. Tout autre doute est `unavailable` AVEC son motif — sinon le refus D3-c frapperait au
> hasard. »

⇒ **`absent` et `unavailable` ne disent pas la même chose et n'appellent pas la même action :**

| Statut | Ce que ça veut dire | Côté page | Côté firmware |
|---|---|---|---|
| `absent` | La release **n'est pas signée** (constaté) | ⛔ l'installation **ne part pas** | — |
| `unavailable` | **On ne sait pas** (hôte refusé, URL trop longue, pas vérifié) | ⛔ l'installation **ne part pas**, message **distinct** | — |
| `deferred` | URL connue, signature à demander | la page demande, puis `relayed` ou `unavailable` | — |
| `relayed` | Signature récupérée et validée | l'installation part avec manifeste + signature | vérifie |

⚠️ **Le firmware, lui, ne lit AUCUN de ces statuts** : il juge ce qu'il **reçoit**. Une page qui
n'envoie rien produit `kAbsent` côté vérificateur et sera refusée. Le statut sert **uniquement** au
pré-contrôle de la page et au **message**. ⛔ Ne jamais faire entrer un statut relayé dans le prédicat
de refus (B2).
⚠️ Conséquence utilisateur assumée : **GitHub injoignable ⇒ pas de mise à jour en ligne.** La voie
manuelle `/update` reste disponible, sans plancher, y compris pour revenir en arrière.

## ⚠️ AJOUT POUR D3-c (2026-11-15) — le plancher de version est figé à `0.0.0`

Relevé par la revue de sécurité de `bug-178` : `ota_manifest_logic.h:87-95` fige le plancher de
version à **`0.0.0`**. ⇒ Même une fois D3-c livré, **une image ANCIENNE correctement signée reste
installable** : la signature prouve l'origine, pas la fraîcheur.

⚠️ **Conséquence directe sur la portée annoncée de D3-c** : le cas « release remplacée sous le même
tag » ne sera **réellement** couvert que si D3-c apporte **deux** choses — le refus effectif **ET**
un plancher de version non nul. ⛔ Le refus seul ne suffit pas.

⇒ À traiter **dans D3-c**, pas après : un plancher ajouté plus tard devrait affronter un parc déjà
figé sur l'ancienne règle.

## ⚠️ CONTRAINTE D'ORDRE RAPPELÉE — D3-c et bug-176

`bug-176` (livré en 2.88.0) a retiré les signatures de la réponse `/check-update` : elles se
récupèrent désormais par `GET /ota-sig`. ⇒ **Une page ANCIENNE enverrait des paramètres vides** et
serait **refusée** par D3-c, **y compris pour installer le filesystem qui la corrigerait**.

⛔ **D3-c ne doit donc pas sortir avant qu'une release dont le filesystem connaît `/ota-sig` soit
déployée sur le parc**, et une **voie de réparation indépendante de la page** doit être documentée.

## 🔴 CONDITION D3-c COMPLÉTÉE (2026-11-15) — déployer le filesystem NE SUFFIT PAS

**Constaté en conditions réelles**, mise à jour 2.88.1 → 2.88.2 lancée par l'utilisateur depuis son
téléphone : ⛔ **aucun appel à `/ota-sig`** dans le journal, et les deux genres journalisés
`authenticité OTA : absente — D3 refuserait : oui`.

**Cause établie** (ce n'est PAS un défaut du firmware — vérifié juste après : `/ota-sig` rend les
deux signatures, 512 car., `tag=v2.88.2`) : l'onglet du téléphone exécutait encore le JavaScript
**chargé le matin**, d'avant `bug-176`. Les déploiements de la journée étant passés par `curl` et
non par la page, celle-ci n'avait **jamais été rechargée**. L'ancienne page lisait les signatures
dans `/check-update`, qui ne les porte plus ⇒ champs vides.

⚠️ **`Cache-Control: no-cache` n'y change rien** : l'en-tête gouverne la **revalidation au
rechargement**, pas une page **déjà chargée** qui reste ouverte. Un onglet peut donc exécuter du
JavaScript périmé pendant des heures, voire des jours.

### Conséquence pour D3-c — la condition S6 était INCOMPLÈTE
Elle disait : *« D3-c ne doit pas sortir avant qu'une release dont le filesystem connaît `/ota-sig`
soit déployée »*. ⛔ **Insuffisant.** Il faut aussi que **la page ait été RECHARGÉE** chez
l'utilisateur. Sous D3-c, l'onglet observé aujourd'hui aurait été **refusé**, sans que l'utilisateur
puisse comprendre pourquoi — ⚠️ exactement *« la phrase qu'on ne peut pas donner à un client qui
aurait son appareil bloqué »*.

### ⇒ À traiter DANS D3-c, pas après
1. **Que la page annonce sa propre version** au firmware (en-tête ou paramètre), et que le refus
   distingue « signature absente » de « **page périmée, rechargez** » — ⛔ un refus muet est ici
   le pire des cas.
2. **Ou** que le firmware tolère explicitement l'absence de paramètres venant d'une page qu'il sait
   ancienne, le temps d'une version de transition.
3. ⛔ **Dans tous les cas** : la voie de réparation par **envoi manuel** (`POST /update` avec
   manifeste et signature) doit rester utilisable **sans dépendre de la page** — elle a été vérifiée
   le 2026-11-15 (`/update — authenticité OTA : valide`).

⚠️ **Ce que cet épisode prouve au passage** : le parcours réel ne se déduit pas du code. Trois lots
livrés et vérifiés dans la journée, et c'est une mise à jour faite par l'utilisateur **depuis son
téléphone** qui a révélé la faille du raisonnement.

## 🔴 DÉCISION UTILISATEUR (2026-11-15) — le retour arrière MANUEL reste possible

Question posée : au moment de D3-c, faut-il un **plancher de version** qui interdise la
réinstallation d'une version antérieure (protection contre la réinstallation d'une version
vulnérable), ou garder la **liberté de revenir en arrière** ?

**Réponse de l'utilisateur, mot pour mot** :
> *« pour D3-c, je veux pouvoir revenir en arrière **avec la mise à jour manuelle** »*

### Ce que cela fige
⛔ **`POST /update` (envoi manuel) ne reçoit AUCUN plancher de version.** Une image **plus ancienne
et correctement signée** doit rester installable par ce chemin, **définitivement**. C'est la voie
que l'utilisateur emprunte sciemment, avec un fichier qu'il a lui-même choisi.
⇒ ⛔ Un futur lot qui ajouterait un plancher à `/update` **contredirait une décision produit
explicite** : il faudrait la rouvrir, pas la contourner.

⚠️ Cohérent avec ce que `bug-178` a déjà établi et **démontré en acte** le même jour : l'appareil a
été ramené de 2.88.3 à 2.88.2 par envoi manuel signé. Le projet empêche qu'un retour arrière soit
**PROPOSÉ**, il ne le **BLOQUE** pas.

### ✅ LA QUESTION EST ENTIÈREMENT TRANCHÉE — AUCUN PLANCHER, NULLE PART
J'avais laissé ouverte la question du chemin **en ligne**. ⭐ **L'utilisateur l'a fermée par un
argument que je n'avais pas vu** :
> *« les mises à jour en ligne ne font que monter puisqu'il n'y a pas de mécanisme de choix de
> version dans l'interface web de mise à jour en ligne »*

**Vérifié dans le code** :
- `web_routes_ota.cpp:1559` — l'URL interrogée est `.../releases/**latest**`, **figée**, sans
  paramètre de tag ;
- `data/app.js:10949` — la page ne mémorise qu'**une seule** release ;
- `bug-178` — appareil en avance ⇒ **rien n'est proposé**, bouton désactivé.

⇒ **Le chemin en ligne ne peut que MONTER ou RESTER.** Un plancher n'y aurait jamais rien à refuser.

### 🔴 CORRECTION — ce que j'avais écrit sur le plancher était FAUX
J'ai affirmé qu'un plancher **couvrirait** le cas « release remplacée sous le même tag ».
⛔ **C'est faux** : dans ce scénario le numéro de version **ne change pas**, et un plancher compare
des **versions** — il ne se déclencherait **jamais**. Ce cas est couvert par **D3-c seul** : une
release remplacée sans signature valide est **refusée**, et une release remplacée **avec** une
signature valide ne peut venir que de la clé de l'utilisateur.

⇒ **Le plancher de version n'a AUCUN objet.** ⛔ Ne pas en introduire, ni en ligne ni en manuel.

### Les deux réserves qui subsistent (⛔ sans remettre la décision en cause)
1. ⚠️ **Si l'interface gagnait un jour un sélecteur de version** — permettant d'installer une
   release ancienne depuis la page — la question se **rouvrirait**. À rappeler dans la spec qui
   introduirait un tel sélecteur.
2. ⚠️ **`/download-update` accepte toujours une URL arbitraire** (`web_routes_ota.cpp:1999`) : un
   client **authentifié** en `curl` installe ce qu'il veut sans passer par la page. ⛔ Ce n'est pas
   l'affaire du plancher — c'est **`bug-180`** (liste blanche trop large).

### Conséquence pratique à garder en tête
⚠️ La voie de réparation manuelle devient **le filet de sécurité de tout le dispositif** : c'est
elle qui permet de sortir d'une version défectueuse, et c'est aussi elle qui doit rester utilisable
**sans dépendre de la page** (condition déjà posée, vérifiée le 2026-11-15 :
`/update — authenticité OTA : valide`). ⇒ Elle ne doit **jamais** être dégradée par un lot ultérieur.

## 🔴 D3-b — BLOCAGE DE CONCEPTION TROUVÉ AVANT TOUT CODE (2026-11-15)

### Le fait que ni P1 ni P1-bis n'avaient vu
Le verdict P1-bis écrit que la page de secours *« n'a plus à calculer de SHA-256 : elle envoie
`manifest`, `signature`, puis le fichier »*. ⛔ **C'est impossible en l'état**, et c'est **vérifié
dans le code**, pas déduit :

```
web_routes_ota.cpp:807   if (shaParamVerdict == kAbsent) → 400, refus CRITICAL
web_routes_ota.cpp:920   « Param POST sha256 OBLIGATOIRE depuis feature-118 lot B »
data/app.js:11558-11560  SHA-256 en JS pur, car crypto.subtle exige un contexte sécurisé
                         — HTTP clair sur IP de LAN ⇒ INDISPONIBLE, sans repli
```
⇒ **La page de secours décrite serait refusée par une garde préexistante.**

### ⚠️ Deux requalifications, car l'arbitrage était mal posé
- **L'option « le manifeste devient source d'empreinte » n'est PAS un affaiblissement
  cryptographique.** Le `sha256` déclaré par l'appelant **n'est pas signé** — c'est une
  affirmation du client. L'empreinte du **manifeste est SIGNÉE**. ⇒ On remplacerait une
  affirmation par une **preuve**. ⚠️ Son vrai défaut est ailleurs : elle **élargit ce que
  `/update` accepte, pour TOUS les appelants** — une **acceptation neuve**, exactement la classe
  de défaut sur laquelle D3-a s'est fait prendre. ⇒ **Relève de D3-c**, pas de D3-b.
- **Le risque de la duplication n'est pas la sécurité, c'est la DISPONIBILITÉ.** Une copie
  buggée du SHA-256 produit de mauvaises empreintes ⇒ envois **refusés** ⇒ ⛔ la page de secours
  ne marche pas **le jour où on en a besoin**. Elle échoue fermée, pas ouverte.

### ✅ OPTION RETENUE — (a′) une seule source, extraite à la construction
⭐ **La machinerie existe déjà** : `test/js_extract.js` extrait du **code vivant** de `data/app.js`
par ancre (ADR-0099), et fait tourner les harnais de vecteurs.
⇒ Le SHA-256 reste écrit **UNE FOIS** dans `data/app.js` ; un script de build l'extrait et
l'injecte dans la page de secours ; un **garde-fou CI** vérifie la correspondance, sur le patron de
`check_sig_parse_vectors.sh`.

| | (a) copie manuelle | **(a′) extraction au build** | (b) manifeste suffisant |
|---|---|---|---|
| Implémentations | **2** | **1** | 1 |
| Dérive | silencieuse | **détectée en CI** | — |
| `/update` modifié | non | **non** | ⚠️ oui |
| Acceptation élargie | non | **non** | 🔴 oui ⇒ D3-c |

⛔ **Repli assumé : (a)**, si l'extraction s'avère impossible proprement.
⚠️ **Non vérifié à ce stade** : que l'implémentation actuelle (**7 657 o** dans `app.js`) s'extraie
**sans dépendance** au reste du fichier. ⇒ **À trancher par `architect` sur le code**, et ⛔ pas à
affirmer ici. Si elle a des dépendances, (a′) devient coûteux et (a) redevient la voie simple.

## ✅ DÉCOUPAGE DE D3-b ACCEPTÉ PAR L'UTILISATEUR (2026-11-15)

| Sous-lot | Contenu | Pourquoi ici |
|---|---|---|
| **D3-b1 — la porte** | B4, B5, B6, B7, B10, B13 : mode dégradé + déclencheur + page de secours + garde-fou CI | ⭐ C'est **le défaut qui laisse l'appareil injoignable aujourd'hui**. ⛔ **Aucun prédicat de décision OTA** ⇒ diff lisible, revue courte. Marches **1 et 2** de B12. |
| **D3-b2 — les gardes inertes** | B11 (interrupteur), cond. **6**, cond. **8**, liaison **genre + version** (acceptation neuve de D3-a), `#no_update_msg` | ⛔ Ce sont des **prédicats** et des **endpoints d'administration** — la famille de bug-117. Les mêler à de l'infrastructure HTTP rendrait la revue P2 **ininterprétable**. Marche **3** de B12. |

⭐ **Pourquoi couper là** : b1 ne touche **aucun** prédicat d'authentification ou de signature —
c'est exactement la réserve de fond *« le mode dégradé ne doit pas être un mode »*. b2 ne touche
**aucune** route de service de page. ⇒ Le `grep` de B5 est **vert par construction** à la fin de b1.

## 🏗️ D3-b1 — CONCEPTION ARRÊTÉE (2026-11-15)

### ✅ (a′) PROUVÉE VIABLE — exécutée, pas estimée
L'architecte a **exécuté** l'extraction : les trois morceaux du SHA-256 de `data/app.js` sortent par
ancre **sans aucune dépendance** au reste du fichier, et le code extrait, évalué **seul**, produit
la **même empreinte que `node:crypto`** sur 300 000 o aléatoires.

| Ancre | Octets |
|---|---|
| `const SHA256_K = new Uint32Array([` | 782 |
| `function createSha256() {` | 2 962 |
| `async function sha256OfFile(file, onProgress) {` | 436 |
| `function parseOtaManifestLine(line) {` + `function parseSigFile(text) {` | 2 212 |
| **Total** | **6 392** |

⚠️ **Correctif de chiffre** : les « 7 657 o » comptaient la région **commentaires compris**.

### ⭐ Variante retenue : génération au COMMIT, pas au build
`tools/gen_recovery_page.js` (node, via `test/js_extract.js`) produit `src/ota_recovery_page.cpp`,
**commité**. `tools/check_recovery_page_generated.sh` re-génère et `diff` ⇒ **rouge à la dérive**.
⇒ `pio run` **reste sans dépendance à node** (une génération au build l'aurait imposée au gate CI
de compilation), et la page **reste lisible dans le diff** par `code-reviewer`.
⛔ Fichier généré **versionné** = exception assumée à « on ne versionne pas les générés » ⇒ ADR-0124.

### 🔴 DÉFAUT GRAVE TROUVÉ DANS LA CONCEPTION, AVANT TOUT CODE
L'architecte avait écrit *« Vérifié : `build_fs.sh` ne gzippe pas ⇒ sonder `/index.html` »*.
⛔ **FAUX, mesuré** : `minify.js:62-72` gzippe en zlib 9 et **n'écrit QUE la variante `.gz`**
(feature-048). `data-build/` ne contient **aucun** `.html`/`.js`/`.css` nu.

⇒ La sonde `exists("/index.html")` aurait rendu **`false` sur un appareil SAIN** ⇒ **la page de
secours se serait armée en permanence**, masquant l'interface sur **tous** les appareils.
⚠️ **Et les tests natifs ne l'auraient pas vu** : le module pur reçoit `indexPresent` en booléen —
c'est **l'APPELANT** qui mentait. Logique parfaitement testée, parfaitement verte, alimentée par
une fausseté.
⚠️ Erreur de méthode reconnue par l'architecte : il avait grepé `build_fs.sh`, **le script
enveloppe**, au lieu de `minify.js` qu'il appelle. ⭐ Et `docs/BUILD.md:105,112`,
`docs/subsystems/web-server.md:244-246` et `CLAUDE.md` **disaient déjà la bonne chose** : la doc
l'aurait démenti s'il l'avait lue.

**Sonde corrigée** — `fsPathResolvableIfHeapAllows()` dans `fs_guard.*` (seul fichier autorisé à
ouvrir, ADR-0070), **miroir exact** de `AsyncWebServerRequest::send(FS&, path)` : `path` puis
`path + ".gz"`. ⭐ **`.gz` sondé EN PREMIER** ⇒ **un seul accès fichier** sur un appareil normal.
Admission mémoire unique couvrant les deux.

**Contamination vérifiée, limitée à la sonde** : `GET /ui` et les 7 routes de page passent par
`req->send(LittleFS, …)`, qui résout le `.gz` **nativement** (`WebResponses.cpp:728-752`).

⭐ **Garde-fou neuf `tools/check_fs_probe_paths.sh`** : lance `minify.js`, extrait les chemins
**réellement sondés** dans le code, et **rougit si `data-build/` ne contient ni `<chemin>` ni
`<chemin>.gz`**. ⇒ Il **lie le sondeur au producteur** — ce qu'un test natif ne peut pas faire,
puisqu'il teste le prédicat et non son alimentation. Attrape aussi la rupture inverse.

### Autres arbitrages tranchés
- **Marqueur persistant** : NVS, **namespace neuf `pool-ota`**, clé `fsupd`. ⛔ Pas `poolctrl`
  (passerait par `config.cpp` et son mutex, depuis `async_tcp`). ⭐ La NVS vit à `0x9000`, **hors
  `spiffs`** : elle survit à la destruction de la partition. Posé avant les **deux**
  `LittleFS.end()` (`:933`, `:2273`), levé après les **deux** succès (`:1064`, `:2376`).
  ⚠️ Écriture NVS en échec ⇒ entrée **ignorée**, jamais lue comme « en cours » : ⇒ **échoue vers
  le comportement d'aujourd'hui**, ⛔ jamais vers un faux secours.
  ⚠️ **Faux positif collant assumé** : coupure de courant entre la pose et `LittleFS.end()` ⇒
  marqueur posé sur un FS intact. ⇒ Réparé par l'échappatoire **`GET /ui`** (5 lignes).
- **Adresse** : `GET /recovery`, publique, **toujours** servie, enregistrée **avant la garde 503**.
  ⚠️ **Nuance déclarée** : `/` et `/index.html` restent **sous** la garde ⇒ en famine mémoire,
  seul `/recovery` répond. ⛔ Les remonter changerait la politique d'exemption de toute
  l'interface — hors périmètre.
- **Pas de pré-gzip** de la page : ⚠️ **mesure finale ~16 Ko** (le fichier généré pèse 16 468 o),
  et non les ~10,1 Ko estimés — l'estimation portait sur le gabarit, pas sur le JS injecté.
  Coût réel du lot : **+19 120 o de flash**, soit **51,3 % → 51,8 %**. ⇒ Conclusion inchangée :
  le flash n'est pas l'argument.
  🔴 **Le point technique à ne pas rater** : servir via `send(200, "text/html", const char*)`
  passerait par `AsyncBasicResponse`, qui **copie 10 Ko dans un `String`** — pour un `maxBloc8`
  nominal de **~21 Ko**. ⇒ **Obligatoire** : surcharge `(const uint8_t*, size_t)` ⇒
  `AsyncProgmemResponse`, **zéro copie en tas**.
- **A4 — dégradation sans FS, prouvée PAR CONSTRUCTION** : `VFSImpl::exists()` et `open()`
  (`vfs_api.cpp:24-28`, `:89-93`) **sortent immédiatement** quand `_mountpoint == nullptr`, et un
  `begin()` raté ne le pose jamais ⇒ ⛔ aucun `fopen`, aucune allocation. ⇒ **404 par
  construction, pas par vigilance.**
  ⭐ `/get-history`, `/get-logs`, `/download-logs` vivent sur `historyFs`, partition **`history`
  séparée** : intactes.

## 🔒 D3-b1 — REVUE DE SÉCURITÉ P1 (2026-11-15) : **GO SOUS CONDITIONS S1-S9**

Aucun constat grave. ⭐ **Mais la revue corrige DEUX affirmations de l'architecte**, et en trouve
un défaut que la conception aurait introduit.

### 🔴 Correction 1 — « 404 par construction » est FAUX (vérifié dans la bibliothèque)
```
WebRequest.cpp:1209   beginResponse(FS&, path, …) → NULL si ni path ni path+".gz"
WebRequest.cpp:1036   _send() → send(501, "Handler did not handle the request")
```
⇒ Sans filesystem, les routes de page rendent **501**, pas 404 (404 seulement **avec**
`If-None-Match`). ⚠️ **501 est une erreur SERVEUR** : tout script de sondage ou tout test attendant
404 aurait été **faux**. De même, « aucune allocation » est inexact — une `String gzPath` est
construite. Sans danger, mais à ne pas écrire.

### 🔴 Correction 2 — « grep vert par construction » est FAUX
Les **quatre sites du marqueur** (`:933`, `:1064`, `:2273`, `:2376`) vivent dans
`web_routes_ota.cpp` — **le fichier même** qui porte `otaUploadAuthorized` et le prédicat de
signature. ⇒ Le contrôle de B5 doit porter sur **le SYMBOLE**, ⛔ pas sur le fichier.

### 🔴 Défaut que la conception aurait introduit (S3)
Le point de levée `:1064` est **partagé** entre genres ⇒ ⛔ **un succès de mise à jour FIRMWARE
aurait levé le marqueur du FILESYSTEM**. ⇒ Pose **et** levée conditionnées au genre FS, figé par
un test natif tué par mutation.

### Les 9 conditions
| | Condition |
|---|---|
| **S1** | Corriger la spec : **501 ou 404** selon `If-None-Match` ; « aucun `fopen` » et non « aucune allocation ». ⛔ Aucun test ni script ne dépend du 404. |
| **S2** | Le grep de B5 porte sur les **symboles** (`otaRecoveryActive`, lecture du marqueur) : **0** dans `auth.*` ; `web_routes_ota.cpp` n'a qu'une interface d'**écriture**. Nombre de lecteurs **figé par un contrôle CI**. |
| **S3** | Pose **et** levée **conditionnées au genre FS** — un succès firmware ne lève **jamais** le marqueur. Test natif + mutation. |
| **S4** | Déclencheur **réévalué à chaque transition** (pose, levée, remontage réussi ou raté), ⛔ pas seulement au démarrage. ⚠️ Preuve = **marche 2 de B12**, où `/` doit rendre le secours **sans redémarrage**. |
| **S5** | **`/ui` enregistrée APRÈS la garde 503** (elle ouvre un fichier) ; `/recovery` **avant**. Le lien `/ui` **figure dans la page de secours**. |
| **S6** | La page n'insère que par **`textContent`**. Le contrôle de génération **rougit** sur `innerHTML`, `insertAdjacentHTML`, `document.write`, `eval`, `location.search`, `location.hash`, et toute **URL absolue**. |
| **S7** | ⛔ Aucun mot de passe constant hors le littéral `"admin"` de B10. Le nouveau mot de passe est **saisi par l'utilisateur**. |
| **S8** | `check_recovery_page_generated.sh` **lancé par `/release`** (ou `check_release_artefacts.sh`) et **l'arrête**. Prouvé par mutation : une édition manuelle du `.cpp` le fait rougir. |
| **S9** | Diff P2 : ⛔ aucun site neuf de `handleOtaUpdate`, ⛔ aucun `server->on(…HTTP_POST…)` neuf, ⛔ aucun `LittleFS.begin(true)`, et le refus `:807` **inchangé**. |

### Réponses aux questions posées
- **Marqueur armable ?** ⛔ **Pas sans authentification** (pose derrière `otaUploadAuthorized` à
  chaque fragment, et derrière `REQUIRE_AUTH`). ⚠️ **Authentifié : oui, à volonté** — entamer un
  envoi FS puis couper. ⇒ **Pas un pouvoir neuf** : ce porteur peut déjà **détruire réellement**
  la partition. Déni de service « interface masquée », persistant mais **réparable par `/ui`**.
- **`/ui` publique ?** ✅ Acceptable : elle sert le fichier que `/` sert **déjà** publiquement.
  L'authentifier créerait une divergence sans gain. ⛔ Elle ne contourne rien — **le secours n'est
  pas une garde**.
- **`otaRecoveryActive()` est-elle « un mode » ?** ✅ Acceptable : aucune décision de sécurité n'en
  dépend. ⚠️ Mais voir S2 — l'argument de l'architecte était faux, la vérification change de forme.
- **B10** ⇒ pas de surface neuve : `/auth/change-password` est **déjà** public en premier démarrage
  et joignable sans interface.
- **`.sig` hostile** ⇒ aucun effet : `parseSigFile` ne rend que des valeurs d'**alphabet fermé**.
  ⚠️ Tient **tant que la page n'affiche jamais le texte brut** (⇒ S6).
- **Fichier généré édité à la main** ⇒ détecté **si le garde-fou tourne**. ⚠️ Or la CI **n'est pas
  bloquante** ici (elle est restée rouge trois commits durant, cf. plus haut) et `/release` ne le
  lance pas ⇒ **S8**.

### ⚠️ Cosmétique à ne pas perdre
`login.html:397,448` redirige vers `/` ⇒ **retour au secours après connexion** en cas de faux
positif collant. ⇒ Le lien `/ui` doit être **redonné après connexion**.

### Durcissements à ouvrir séparément
Mise en attente de l'image FS en **PSRAM** (⭐ c'est elle qui rend le marqueur armable) · `bug-112` ·
premier démarrage : `change-password` **rejouable sans secret** tant que l'assistant n'est pas
terminé · levée du marqueur sur les chemins d'échec où **rien n'a été écrit** (`:939`).

## 🎨 D3-b1 — ÉTUDE ERGONOMIE RENDUE (2026-11-15)

⭐ **La lecture de la capture a CHANGÉ la conception.** L'agent avait d'abord calqué `login.html` ;
après avoir regardé le tableau de bord rendu, il l'a **abandonné** pour le vocabulaire de
l'application — cartes à en-tête en majuscules espacées, boutons bleus pleins, callouts.
⚠️ Première version produite **sans** avoir lu l'image jointe ; il l'a **déclaré**, ce qui a permis
de le rattraper.

### Parcours retenu (option C′) — page unique, étapes révélées par `hidden`
⛔ **Aucune redirection** après connexion : la page reste, donc le **lien `/ui` reste** (condition
S5 — `login.html` redirige vers `/`, ce qui ferait **boucler** sur le secours en cas de faux
positif). ⛔ Aucune redirection vers `wizard.html` non plus : il vit dans `data/`, **absent** par
hypothèse.
Options écartées : **A** calque de `login.html` (le logo est un fichier **introuvable en secours**,
look d'une autre page, contraste insuffisant) · **B** assistant à étapes (trop de JS ; le retour
arrière après envoi n'a pas de sens).

### Libellés — quelques-uns, comptés
`Interface à restaurer` (21) · callout **`L'interface web est absente ou endommagée.`** (42) +
`La régulation continue : pH, ORP, pompes et filtration fonctionnent.` (69) ·
`Ouvrir l'interface quand même` (29, le lien `/ui`) · `Vérification du fichier : 42 %` (30) ·
`Cela peut durer quelques secondes. Ne fermez pas cette page.` (60) ·
`PoolController • Mode secours` (30).
⛔ **Aucun libellé sur une ligne partagée** : tout est empilé en pleine largeur.
Erreurs **toutes préfixées « Échec : »**, textes **fixes** — ⛔ jamais le texte brut du serveur ni
du `.sig` (condition S6), insertion par `textContent`.

⭐ **Le callout rassurant est le libellé le plus important de la page** : il dit que **la régulation
continue**. C'est vrai (décision utilisateur du 2026-11-15) et c'est ce qu'on veut lire quand
l'écran annonce une panne.

### Note d'implémentation à ne pas perdre
⚠️ **Rendre la main au navigateur entre les tranches de `sha256OfFile`**, sinon la barre de
progression **ne se repeint pas** sur téléphone — et le calcul dure plusieurs secondes sur 512 Ko.

### Budget — déclaré au-dessus de la cible, avec l'ordre de coupe
HTML+CSS ≈ **4,1 Ko** non minifié (~3,2 Ko espaces retirés) contre une cible de 3-4 Ko : le
vocabulaire du tableau de bord coûte ~0,7 Ko de plus. ⇒ Ordre de retrait si nécessaire : dégradé et
ombre · « Mode secours » · en-têtes de carte · l'aide « Les deux fichiers sont nécessaires ».
⛔ **Ne jamais retirer** : le lien `/ui`, les préfixes « Échec : » / « Terminé : », l'état d'attente
du calcul d'empreinte, **le callout rassurant**.

### Quatre contextes d'emploi nouveaux ⇒ `docs/DESIGN_SYSTEM.md`
Callout `--info` **en page autonome** (copie figée) · champ fichier à 44 px
(⛔ les 3 `<input type="file">` de l'app **ne changent pas**) · `<progress>` natif
(⚠️ vérifier d'abord qu'il n'existe pas déjà dans l'app) · **callout d'échec rouge**, variante
**locale** — ⛔ **pas** un `.callout--danger` partagé.
⇒ Aucun composant partagé n'évolue ⇒ **aucun impact à mesurer dans `data/`**.

### 🔴 DÉFAUT PRÉEXISTANT MESURÉ AU PASSAGE ⇒ `bug-182`
`login.html`, `wizard.html` et `wifi.html` partagent un style **sous AA** : bouton **≈ 3,9:1**,
bordure de champ **≈ 1,2:1** (seuil 3:1), champs à **14 px** ⇒ **zoom iOS au focus**.
⭐ L'agent a **corrigé sa propre affirmation** : il annonçait le texte d'erreur sous AA, mesure
faite il est à **4,8:1**. ⇒ Défaut **plus étroit** qu'annoncé, et mieux cerné.
⚠️ Ce sont les pages vues **en premier** et **quand ça va mal**. ⇒ **`bug-182` ouverte.**
⇒ La page de secours utilisera la palette **conforme** de `app.css` (`--accent #2563eb` ⇒ **5,2:1**).

## ✅ D3-b1 « la porte » — NOTES D'IMPLÉMENTATION (2026-11-15)

**Statut** : livré côté code — compilation propre, tests natifs verts, revue de sécurité **GO sans condition**. ⛔ **D3-b2 et la marche 3 de B12 restent OUVERTS** ; la spec **n'est pas déplacée** vers `done/` ; aucun des critères d'acceptation ci-dessus n'est coché (ils portent sur le refus de D3-c).

**Mesures** (à relire, pas à recopier ailleurs) : `pio run` SUCCESS après `clean`, **0 warning neuf**, Flash **51,8 %** (1 935 273 o, **+19 120 o**), RAM 29,8 %. Tests natifs : **82 suites, 2 445 cas, 0 échec**. **5 mutations tuées**, dont **3 garde-fous** prouvés par mutation (S3 genre filesystem, priorité du déclencheur, règles de repli).

**Livré** : B4 (retour anticipé retiré, table de routes unique), B5 (aucun drapeau — outillé **sur les symboles**, `tools/check_degraded_mode_absent.sh`), B6 (page = constante compilée, `GET /recovery`), B7 (`/` et `/index.html`), B10 (premier démarrage), B13 (documenté dans `docs/UPDATE_GUIDE.md`). Fichiers : `src/web_server.cpp`, `src/web_routes_recovery.*`, `src/ota_recovery_logic.*`, `src/ota_recovery_marker.*`, `src/ota_recovery_page.*` (+ `.html.in`, généré), `src/fs_guard.*` (`fsPathResolvableIfHeapAllows`), `src/web_routes_ota.cpp` (pose/levée du marqueur, conditionnées au genre FS), `src/constants.h`, `platformio.ini`, `tools/gen_recovery_page.js`, `tools/check_degraded_mode_absent.sh`, `tools/check_recovery_page_generated.sh`, `tools/check_fs_probe_paths.sh`, `tools/check_fopen_guard.sh` (exemption nommée), `tools/check_release_artefacts.sh` (S8), `.github/workflows/hygiene-specs.yml` (3 jobs), `test/test_native_ota_recovery/`.

**Conditions de la revue P1** : S1 (501/404 corrigé), S2 (symboles, 1 lecteur), S3 (genre FS), S4 (réévaluation à chaque transition), S5 (`/ui` après la garde, `/recovery` avant, lien présent), S6 (`textContent`, contrôle de génération), S7 (aucun mot de passe constant hors `"admin"`), S8 (`/release` lance le contrôle de fraîcheur), S9 (diff sans site `handleOtaUpdate` neuf, sans `POST` neuf, sans `LittleFS.begin(true)`, refus `:807` inchangé).

**Décision** : [ADR-0124](../../../docs/adr/0124-le-mode-degrade-n-est-pas-un-mode-la-porte-de-sortie-du-serveur-web.md). Documentation : `docs/API.md`, `docs/subsystems/web-server.md`, `docs/subsystems/ota-manager.md`, `docs/DESIGN_SYSTEM.md` §5.21, `docs/BUILD.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md` (section `[2.89.0]`), `docs/user/src/guide-utilisation.html`.

### ✅ B12 marche 1 — relevée le 2026-11-16 sur l'appareil (2.89.0)

**Parcours complet par la page de secours, appareil SAIN**, conduit **au navigateur** par
l'utilisateur : `http://192.168.1.48/recovery` → connexion → `littlefs.bin` + `littlefs.bin.sig`
→ « Vérifier et envoyer ». ⭐ L'essai **installe réellement** le système de fichiers 2.89.0 — il
ne le simule pas.

**Lignes de verdict** (journal persistant, `/download-logs`) :

```
07:34:29 Type de mise à jour: Filesystem (paramètre formulaire)
07:34:29 [feature-151 D1]    /update — authenticité OTA : valide — D3 refuserait : non
07:34:29 [feature-151 D1 B1] /update — liaison (paramètre sha256 (index==0))  : liée
07:34:29 LittleFS démonté pour mise à jour manuelle
07:34:29 Page de secours OTA ACTIVE : mise à jour du filesystem interrompue
07:34:32 [feature-151 D1 B1] /update — liaison (empreinte calculée (final))   : liée
07:34:33 Page de secours OTA ACTIVE : montage du filesystem échoué
07:34:40 LittleFS monté avec succès
```

⭐ **Deux acquis non prévus par cette marche** :
1. **S4 est observé dans le journal** — le secours s'arme à `07:34:29`, **pendant** la mise à
   jour, ⛔ sans redémarrage, par le **marqueur** (première entrée du déclencheur), puis une
   **seconde fois** à `07:34:33` par l'échec de montage (seconde entrée). Les deux entrées
   indépendantes sont donc exercées. ⚠️ Ce qui **manque encore**, et qui EST la marche 2 :
   personne n'a demandé `/` pendant cette fenêtre — le journal prouve l'**armement**, pas le
   **rendu**.
2. **La levée du marqueur fonctionne** : après redémarrage, `/` et `/index.html` rendent
   l'interface normale (21 954 o, `<title>PoolController`), ⛔ pas le secours. Le défaut
   « marqueur jamais levé ⇒ secours permanent » est écarté **sur l'appareil**.

**Firmware acheminé de la même façon** (`ota_update.sh --use-sig firmware`) : `authenticité
OTA : valide`, `liaison : liée` sur les deux points de contrôle. Mémoire après redémarrage :
`free8` 56 812, `maxalloc8` 55 284.

**⚠️ Écart relevé sur la page elle-même** : elle n'affiche **aucun numéro de version**. B6 écrit
« la version affichée est une constante de compilation » — il n'y en a pas. ⇒ Un opérateur
devant cette page ne sait pas quel `littlefs.bin` choisir. ⛔ Non corrigé ici (coût : génération
+ compilation + signature + redéploiement) ; à trancher avec D3-b2, qui touchera de toute façon
la liaison **genre + version**.

### ✅ B12 marche 2 — relevée le 2026-11-16 sur l'appareil (2.89.0)

**Système de fichiers DÉTRUIT, SANS redémarrage.** Mise en condition depuis le LXC : envoi
`/update` genre `filesystem` **coupé à 59 %** (327 197 o de 524 288), ⛔ avec un contenu
**aléatoire** — pas l'image en place. Chemin exercé : `onDisconnect` → `Update.abort()` →
`remountUploadFsIfNeeded()` **échoue** (`web_routes_ota.cpp:880`).

**État constaté, appareil NON redémarré** (uptime 330 s, continu) :

```
/            200  15 850 o   « PoolController • Mode secours »
/index.html  200  15 850 o
/recovery    200  15 850 o
/ui          501       34 o
/app.js      404
```

⇒ **S4 est prouvé au RENDU**, pas seulement à l'armement : `/` sert le secours sans qu'aucun
redémarrage n'ait eu lieu, et `/ui` répond **501** — l'interface est réellement absente, ⛔ ce
n'est pas un marqueur posé sur un filesystem intact.

**Réparation par la page de secours, au navigateur**, par l'utilisateur, `littlefs.bin` +
`littlefs.bin.sig` :

```
07:41:20 [feature-151 D1]    /update — authenticité OTA : valide — D3 refuserait : non
07:41:20 [feature-151 D1 B1] /update — liaison (paramètre sha256 (index==0))  : liée
07:41:23 [feature-151 D1 B1] /update — liaison (empreinte calculée (final))   : liée
07:41:23 Mise à jour OTA réussie. Redémarrage...
07:41:31 LittleFS monté avec succès
```

Après redémarrage : `/`, `/index.html` et `/ui` rendent l'interface (21 954 o), `/app.js` 200
(88 706 o), `/recovery` toujours servie. **Marqueur levé.**

⭐ **Contre-épreuve obtenue sans la chercher.** L'image qui a détruit l'interface était **non
signée**, et l'appareil l'a journalisé :

```
07:39:56 WARN authenticité OTA : absente — D3 refuserait : OUI — AUCUN refus appliqué
```

⇒ **D3-c aurait refusé exactement l'image qui a cassé l'interface.** C'est la justification de
D3-c **mesurée sur l'appareil**, plus un argument.

**⚠️ Premier essai de marche 2 : NUL, et consigné comme tel.** La première coupure renvoyait
`littlefs.bin` **identique à celui en place** : les 327 Ko écrits étaient octet pour octet ceux
déjà présents. `/` rendait bien le secours (marqueur posé), mais `/ui` servait **encore**
l'interface entière — ⛔ **rien n'était détruit**. Le piège : « l'envoi a été coupé » ne vaut pas
« la partition est corrompue ». ⇒ Refait avec un contenu aléatoire.

### ⚠️ Deux défauts relevés pendant les marches 1 et 2 — NON corrigés

1. **Le journal dit l'inverse de la feature.** `web_routes_ota.cpp` journalise *« Impossible de
   remonter le filesystem après la mise à jour — **redémarrage requis** »*. ⛔ **Faux depuis
   D3-b1** : aucun redémarrage n'est requis, la page de secours répare sur place. Ce message
   pousse l'opérateur vers le seul geste que D3-b1 a rendu inutile.
2. **La page de secours n'affiche aucun numéro de version** (voir marche 1). Un opérateur ne
   sait pas quel `littlefs.bin` choisir.

⇒ Les deux touchent `web_routes_ota.cpp` et la page générée — **à traiter dans le lot D3-b2**,
qui ouvre déjà ces deux fichiers pour la liaison genre + version.

**⛔ Reste ouvert**
- ~~**D3-b2** : B11 (interrupteur), conditions 6 et 8, liaison genre + version, `#no_update_msg`.~~ ✅ **livré le 2026-11-16** (voir « D3-b2 — notes d'implémentation » en fin de fichier). **Marche 3 de B12** (essai terrain destructif, exige l'accord explicite de l'utilisateur et un accès USB prêt).
- ✅ Les marches **1 et 2** de B12 sont **faites et consignées** (2026-11-16, appareil en 2.89.0) : voir les deux sections ci-dessus. ⛔ Seule la marche **3** (FS détruit **puis** redémarrage) reste ouverte.
- Durcissements à ouvrir séparément (revue P1) : image FS en PSRAM, `bug-112`, `change-password` rejouable au premier démarrage, levée du marqueur sur les chemins d'échec où rien n'a été écrit (`:939`). `bug-182` (contraste de `login/wizard/wifi.html`) reste ouvert.
- 🔴 **Rappel** : « jeton admin volé » reste **ouvert** — cette feature ne le ferme pas (ADR-0120, ADR-0124).

---

## ✅ D3-b2 « les gardes inertes » — NOTES D'IMPLÉMENTATION (2026-11-16)

⛔ **Lot INERTE** : l'appareil reste en mode D1 (« D3 refuserait : oui/non — AUCUN refus appliqué »). Aucun `return`/`send(4xx)` neuf dans `web_routes_ota.cpp`, aucune acceptation neuve. **Revues** : security-reviewer P1 (design) **GO SOUS CONDITIONS C1-C12** ; P2 (diff) **GO SOUS CONDITIONS** (C10 = documentation) ; code-reviewer **Request changes** (issue 2 = documentation), corrigé.

**Mesures** (relevées le 2026-11-16, ⛔ à relire et non à recopier ailleurs — ADR-0073) : `pio run`
SUCCESS, **0 warning neuf** (il reste 2 `#undef` préexistants dans la lib OneWire), Flash **52,1 %**
(1 944 433 o, **+9 160 o** depuis 2.89.0 à 51,8 %), RAM **29,9 %** (97 856 o). Tests natifs :
**85 suites, 2 519 cas, 0 échec** (base avant le lot : 82 suites, 2 445 cas). Page de secours :
**15 850 → 16 475 o**, soit **+625 o** — ⚠️ l'ergonome estimait les coupes compensatoires, elles ne
le sont **pas** ; l'aide « Les deux fichiers sont nécessaires » a été **gardée** malgré tout (elle
explique pourquoi le bouton est grisé, et 625 o ne se discutent pas face à 3,7 Mo de flash).

**⚠️ Ce que le REJEU des mutations a trouvé, et qui n'aurait pas été vu autrement.** Après les
corrections R1-R8, une mutation qui tuait **ne tuait plus** : le retrait de `expiry >= kWordLocked`
survivait, parce qu'**aucun test n'observait la RAISON de la décision** — seulement son résultat.
⇒ Un test de sentinelles la tue désormais. ⭐ C'est l'argument du rejeu : **une correction peut
désarmer un test sans faire rougir quoi que ce soit.**

**⛔ Deux comportements neufs ne sont couverts que par des gardes DE FORME, pas d'exécution** —
dit ici pour ne pas le découvrir plus tard : (1) R8, l'état `locked` publié en MQTT ; (2) R2, le
`null` du champ `nvs_residual_until` dans le JSON. Aucun harnais natif n'exécute `mqtt_manager.cpp`
ni `web_routes_ota_switch.cpp` ; trois assertions **de source** dans
`tools/check_ota_signature_switch.sh` rougissent si ces formes disparaissent. ⚠️ **Ce n'est pas la
même garantie.**

**Régression de test attrapée au passage** : `test_d3b2_verdict_et_levee_distincts` cherchait
l'ancienne concaténation `String`, cassée par **R6** (passage à `snprintf`). Réaligné, même
comportement vérifié.

**Observation sans conséquence** : `otaSignatureSwitchInit()` ne remet pas `sResidualEpoch` à zéro
— inoffensif au démarrage (statique initialisée à 0), mais la valeur fuit d'un test à l'autre.

**Décisions consignées** : [ADR-0125](../../../docs/adr/0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md) (horodatage NVS, `millis()` exclu ; amendement ADR-0124 : le générateur n'émet que le **jeton** de version) et [ADR-0126](../../../docs/adr/0126-la-version-demandee-ne-vient-jamais-d-une-sous-chaine-d-url.md) (la version demandée ne vient jamais d'une sous-chaîne d'URL).

🔴 **Rappel** : cette feature **ne ferme PAS** « jeton admin volé » ; l'interrupteur le **rouvre en deux requêtes au lieu d'une** (armer, puis envoyer).

### Fichiers

- **Neufs** : `src/ota_signature_switch_logic.{h,cpp}` (décision PURE), `src/ota_signature_switch.{h,cpp}` (NVS + miroir atomique), `src/ota_version_request_logic.{h,cpp}` (liaison de version, PURE), `src/web_routes_ota_switch.{h,cpp}` (3 routes) ; `test/test_native_ota_signature_switch/`, `…_nvs/`, `test/test_native_ota_version_request/`, `test/native_shim/Preferences.h`, `esp_task_wdt.h` ; `tools/check_ota_signature_switch.sh`, `tools/check_update_card_no_html.sh`.
- **Modifiés** : `src/web_routes_ota.cpp` (2 lectures, 3 consommations, libellés, désaccord de version, 2 messages), `src/constants.h`, `src/mqtt_manager.{h,cpp}`, `src/web_server.cpp` (1 init + 1 enregistrement), `src/ota_recovery_page.html.in` + `tools/gen_recovery_page.js` + `src/ota_recovery_page.cpp` (régénéré), `src/ota_recovery_marker.cpp`, `tools/check_degraded_mode_absent.sh`, `test/test_native_ota_order_guard/` (`[G7]`), `.github/workflows/hygiene-specs.yml` (2 jobs : `update-card-no-html`, `ota-signature-switch`).
- **Absents du diff (C11)** : `ota_signature.*`, `ota_manifest_logic.*`, `ota_sig_relay_logic.*`, bloc de la clé publique de `constants.h`.

### Défauts trouvés dans la CONCEPTION par la revue P1 (G1-G4) — corrigés avant code

| | Défaut | Correction |
|---|---|---|
| **G1** | Fenêtre non bornée par le **haut** : horloge fausse vers le futur à l'armement ⇒ armé jusqu'à consommation | **C1** : `remaining > kOtaSignatureSwitchWindowSec` ou `expiryEpoch == 0` ⇒ `kRequired` (`invalid_expiry`) |
| **G2** | `server->on(const char*)` ≡ `^{uri}(/.*)?$` : `/ota-signature-switch` capte `/arm` | **C2** : `AsyncURIMatcher::exact` ×3 |
| **G3** | Auth coupée ⇒ `checkAuth` rend `true`, `CRITICAL` ≡ `WRITE` ⇒ armement ouvert au LAN | **C3** : `403` si `!isEnabled()` ou `isFirstBootDetected()` |
| **G4** | Le libellé « D3 refuserait : non (interrupteur armé) » masquait le WARNING et les octets canoniques | **C4** : verdict et levée distincts ; `wouldReject` ne dépend que du verdict |

### Conditions C1-C12 — levées une par une

- **C1** ✅ borne haute et échéance nulle ⇒ `kRequired` (`ota_signature_switch_logic.cpp`) ; cas dédiés dans `test_native_ota_signature_switch`.
- **C2** ✅ `AsyncURIMatcher::exact(...)` sur les 3 routes ; `check_ota_signature_switch.sh`.
- **C3** ✅ `403 {"error":"auth_required"}` (`web_routes_ota_switch.cpp`) ; le désarmement reste permis.
- **C4** ✅ `logOtaAuthenticityVerdict` reçoit la décision de l'interrupteur et n'ajoute qu'une mention ; `combineOtaAuthenticityVerdict` et `otaAuthenticityIsRejected` **non modifiés**.
- **C5** ✅ lecture **positive** (`isKey` + `PT_U32`) seule source de `entryPresent` ; suite `…_nvs` compile le **vrai** `ota_signature_switch.cpp` contre un faux `Preferences` (`begin` en échec, clé absente, mauvais type ⇒ `kRequired`) ; script CI : aucun `= true` littéral.
- **C6** ✅ `Peek` appelé seulement par `mqtt_manager.cpp` ; `Arm` un seul appelant ; miroir fermé par défaut (script CI). Le miroir est **un seul** `std::atomic<uint32_t>` (le design en prévoyait trois : P1 l'a refusé).
- **C7** ✅ verrou RAM posé **avant** `remove()`, honoré par `Read()`, repli `putUInt(0)`, 500 avec l'état réel, CRITICAL ; idem pour la consommation. Les champs `rearms_on_reboot` / `nvs_residual_until` (`null` = inconnu) viennent de la revue de code.
- **C8** ✅ décision capturée à `index==0` (`g_uploadSwitchWaived`) / à l'entrée de `/download-update`, jamais relue pour accorder ; `[G7]` : lecture avant `Update.begin`, consommation après `Update.end(true)`, **exactement 3 sites**.
- **C9** ✅ `expected_version` re-sérialisé depuis les entiers parsés, jamais passé à `storeSigSource`, jamais prioritaire sur le tag (`otaSelectVersionReference`).
- **C10** ✅ **documentation** : ~~« seul témoin qui survit »~~ corrigé (condition 7 ci-dessus) ; écrit dans `docs/MQTT.md`, ADR-0125, `docs/UPDATE_GUIDE.md` ; la réponse d'armement et son journal disent si MQTT était **connecté** (`mqtt_connected`).
- **C11** ✅ aucun hunk dans le bloc clé de `constants.h` ; fichiers de la liste ci-dessus absents du diff.
- **C12** ✅ `check_ota_signature_switch.sh` : pas de `Update.`/`LittleFS`/`onBody`/`onUpload`/`hasParam` dans `web_routes_ota_switch.cpp`, `REQUIRE_AUTH` en première instruction, `server->on` à 3 arguments, `<!--@@VERSION@@-->` exactement une fois.

### Items de la spec marqués LEVÉS

- ✅ **B11** (interrupteur, fonction unique) · ✅ **condition 6** (absence de clé = exigée ; `/factory-reset` ré-arme sans code) · ✅ **condition 8** (endpoints d'administration `CRITICAL`, sans `onBody`/`onUpload`) · ✅ **liaison genre + version** (journalisée, aucun refus ; ⚠️ référence issue de GitHub, périmée après un envoi direct) · ✅ **`#no_update_msg`** : garde mécanique anti-régression `tools/check_update_card_no_html.sh` (job CI `update-card-no-html`) — l'item « reste à poser une garde » est levé ; l'audit des autres insertions de la carte reste la revue de D3-c.

### ✅ Les deux défauts relevés sur l'appareil le 2026-11-16 — CORRIGÉS

1. Message « redémarrage requis » **faux depuis D3-b1** ⇒ réécrit aux deux sites.
2. **Numéro de version absent de la page de secours** ⇒ affiché (jeton `FIRMWARE_VERSION`, constante de compilation). Au passage : sélecteurs de fichiers **en français** (décision utilisateur : étude d'ergonomie appliquée en entier ; dégradé, ombre, en-tête et `<h2>` de carte supprimés).

### ⚠️ Deux fautes attrapées dans NOTRE propre travail par les revues

- **(a) Un message faux, fabriqué par notre correction.** En corrigeant le message de `web_routes_ota.cpp:2479` (« redémarrage requis ») pour dire « aucun redémarrage requis », on l'avait rendu **faux** : ce site est le **succès** de `/download-update` filesystem, où le **redémarrage est planifié six lignes plus bas**. Corrigé : « la page de secours est servie jusqu'au redémarrage planifié, qui remontera le filesystem (aucune action requise) ». Les deux sites n'ont **pas** le même message, car ils n'ont pas la même vérité. Leçon : un message de journal se vérifie **à son site d'appel**, pas à son libellé.
- **(b) Une liste blanche qu'on ajoutait pour se faire taire.** Pour que `ota_signature_switch.cpp` passe `tools/check_degraded_mode_absent.sh`, on l'avait **ajouté à la liste blanche** des fichiers autorisés à citer les symboles du secours (`kOtaRecoveryNvsNamespace`). La revue a vu que c'était élargir une garde pour qu'elle laisse passer. Remplacé par le **renommage** `kOtaRecoveryNvsNamespace` → `kOtaNvsNamespace` (le namespace est partagé, son nom ne doit pas dire « secours ») : le fichier n'est plus dans la liste blanche et le contrôle 4 le couvre.

### ✅ F0 — PREUVE DE TERRAIN de l'interrupteur (2026-11-16, 19:11–19:14, non destructive)

Firmware **2.89.0 signé** déployé par OTA ; signature vérifiée **contre la clé publique extraite
de `src/constants.h`** (celle que l'appareil embarque) avant envoi : `Verified OK`.
Relevé au **journal persistant de l'appareil** (`/download-logs`), horaires appareil :

| Heure | Ligne relevée |
|---|---|
| 19:11:50 | `Interrupteur de signature OTA ARMÉ pour 60 min (échéance 1794856310) — MQTT connecté` |
| 19:11:54 | `MQTT ota_signature_switch → ON` (publication **réussie** : la ligne n'est écrite que si `safePublish` rend `true`) |
| 19:12:56 | `[feature-151 D1] /download-update — authenticité OTA : absente — D3 refuserait : **oui** — **levé par l'interrupteur** (reste 59 min)` — niveau **WARNING** |
| 19:13:48 | `[feature-151 D1] /update — authenticité OTA : valide — D3 refuserait : **non** — **interrupteur armé** (reste 59 min)` — niveau **INFO** |
| 19:14:07 | `Interrupteur de signature OTA consommé (premier flash réussi)` |
| 19:14:22 | `MQTT ota_signature_switch → OFF` (3 s après le redémarrage) |

**Ce que la paire de lignes prouve — c'est l'objet de la condition C4.** Les deux routes ont lu
la **même** décision, et pourtant elles n'écrivent pas la même chose : un verdict qui refuserait
reste **WARNING** et dit « levé par », un verdict valide reste **INFO** et dit « armé ». L'interrupteur
n'a **pas** déguisé le verdict — exactement ce que la mutation de P1 cherchait à empêcher.

**État par la route de lecture** : `{"armed":false,"reason":"no_entry"}` avant armement →
`{"armed":true,"reason":"armed","remaining_s":3600}` → `3482` 118 s plus tard (décompte réel) →
`{"armed":false,"reason":"no_entry"}` après le flash. **Aucune écriture flash** sur
`/download-update` : l'URL pointait une release inexistante, refus `Download failed` **après** la
ligne de journal — ce qui confirme au passage que la lecture a bien lieu **avant** toute écriture.

⚠️ **NON observé : l'expiration naturelle au bout de 60 min.** Le décompte a été vu décroître, et
l'expiration est couverte par les suites natives, mais **aucune échéance n'a expiré sur l'appareil**.
À relever lors d'un prochain armement laissé sans suite.

### ⚠️ La marche 3 ne requiert PAS d'accès USB — vérification du 2026-11-16

L'exigence « accès USB prêt » inscrite plus haut reposait sur une crainte **non fondée**, levée par
lecture du code :

1. **L'armement au démarrage ne dépend pas du marqueur NVS.** `web_server.cpp:74` appelle
   `otaRecoveryDrain()` dès l'initialisation, et la décision lit `in.fsMounted =
   (LittleFS.mountpoint() != nullptr)` — **un état, pas une transition**. Filesystem non monté ⇒
   secours armé, marqueur ou pas. Même fonction de décision que les marches 1 et 2.
2. **Le montage échoue proprement sur ces octets-là, c'est déjà constaté.** `main.cpp:380` monte en
   `LittleFS.begin(false)` (⛔ jamais de formatage) ; `web_routes_ota.cpp:295` appelle **le même**
   `LittleFS.begin(false)` sur **la même partition** — c'est lui qui a tourné pendant la marche 2 sur
   300 Ko d'octets aléatoires **sans planter**.
3. **Rien d'autre ne lit `spiffs` au démarrage** : configuration en NVS, historique et journal sur la
   partition `history`.

⚠️ **La boot-loop de juillet n'est pas ce cas.** C'était un filesystem **à moitié valide** (en-tête
correct, entrailles corrompues) que LittleFS explorait avant de diviser par zéro. Des octets
aléatoires n'ont pas d'en-tête valide : ils sont rejetés à la première lecture. Le disjoncteur
d'effacement automatique (feature-059, `main.cpp:500`) ne couvre d'ailleurs **que** `history`.

⇒ Risque résiduel **faible mais non nul** : le montage du démarrage a lieu plus tôt, dans un autre
contexte mémoire. ⛔ Et le filet n'existe pas ici — **aucun `/dev/ttyUSB*` dans le conteneur**, aucun
`upload_port` dans `platformio.ini` : une boot-loop exigerait une autre machine avec `esptool`.

### ✅ B12 — MARCHE 3 : filesystem détruit **PUIS redémarrage** (2026-11-16, 19:23–19:30)

⭐ **La marche que les marches 1 et 2 n'avaient jamais franchie.** Elles s'arrêtaient avant le
redémarrage ; c'est pourtant la séquence réelle d'une coupure de courant pendant une mise à jour
du filesystem chez un client.

**Protocole.** Trousse de réparation construite et signée **AVANT** toute destruction
(`littlefs.bin` sha256 `030d1cd5…`, `.sig` vérifié `Verified OK` contre la clé publique extraite de
`src/constants.h`), copiée hors du répertoire de build en lecture seule, et remise à l'utilisateur
sur sa machine. ⛔ Aucune reconstruction du FS entre la signature et la réparation
(cf. l'incident du 14/11 : `build_fs.sh` relancé entre les deux ⇒ réparation refusée).
Image hostile : **300 Ko d'octets aléatoires** (`/dev/urandom`), envoyée **NON SIGNÉE** sur `/update`
avec un `sha256` **conforme**.

| Heure | Ligne relevée au journal de l'appareil |
|---|---|
| 19:23:27 | `/update — authenticité OTA : **absente** — D3 refuserait : **oui**` |
| 19:23:29 | `Mise à jour OTA réussie. Redémarrage...` |
| 19:23:32 | `Redémarrage après mise à jour OTA` |
| 19:23:36 | `=== Démarrage ESP32 Pool Controller v2.89.0 ===` |
| 19:23:36 | `Échec montage LittleFS !` ⇒ **échec propre, aucune boucle de démarrage** |
| 19:23:42 | `Page de secours OTA ACTIVE : montage du filesystem échoué` |

**Constaté par le réseau, appareil en panne** : `GET /` → **200**, 16 475 o,
`<title>PoolController • Mode secours</title>`, **« Version 2.89.0 » affichée** (le défaut relevé le
matin même est corrigé) ; `GET /ui` → **501** ; `GET /app.js` → **404**.
**La régulation a survécu** : pH brut 5,623 · ORP brut 594 mV · 14,3 °C, sondes EZO réinitialisées.

**Réparation par l'utilisateur, à son navigateur, sur la page de secours** — verdicts :
`19:30:20 /update — authenticité OTA : **valide** — D3 refuserait : non` ·
`liaison manifeste/image (paramètre sha256 (index==0)) : **liée**` ·
`19:30:23 liaison (empreinte calculée (final)) : **liée**`. Redémarrage 19:30:35, puis
`GET /` → 21 954 o, `/app.js` → 88 706 o, `/ui` → 200. **Interface intégralement rétablie.**

⭐ **Ce que le contre-test établit, et qui justifie D3-c.** L'image destructrice portait une
empreinte SHA-256 **conforme** — `[feature-118 lot B] Empreinte upload OTA : conforme` n'aurait rien
refusé. ⛔ **L'intégrité ne protège de rien ici** : elle atteste que les octets sont arrivés intacts,
pas qu'ils viennent de nous. Seule la ligne d'**authenticité** a vu le défaut. D3-c refusera
exactement cette image.

⇒ **B12 est CLOS** (marches 1, 2 et 3).

### 🗣️ À FAIRE AVEC D3-c — renommer « interrupteur » en « autorisation temporaire »

**Demande utilisateur du 2026-11-16** : *« tu parles avec ton jargon : interrupteur, opérateur.
Explique avec des termes que je peux comprendre »*, puis *« oui, utilise le terme "autorisation
temporaire" »*.

⚠️ **Ce n'est pas qu'une affaire de conversation** : le mot « interrupteur » **fuit dans
l'interface** — il s'affiche dans le journal que l'utilisateur lit depuis l'onglet Avancé
(« Interrupteur de signature OTA ARMÉ pour 60 min… », « levé par l'interrupteur »). Un terme
interne visible par l'utilisateur est un défaut d'ergonomie, pas une fatalité.

**Périmètre du renommage** (textes seulement, aucune logique) :
- les messages de journal de `ota_signature_switch.cpp`, `web_routes_ota_switch.cpp` et la mention
  ajoutée par `logOtaAuthenticityVerdict` dans `web_routes_ota.cpp` ;
- le nom d'entité lisible côté Home Assistant ;
- `docs/UPDATE_GUIDE.md`, `docs/MQTT.md`, `docs/user/src/guide-utilisation.html`.

⛔ **Ne PAS renommer** : le topic MQTT (`…/ota_signature_switch`) ni les chemins de route — ce
sont des interfaces publiques, un appareil en service et Home Assistant les connaissent déjà.
⛔ **Ne PAS toucher aux ADR 0125/0126** : ce sont des décisions datées, immuables.
⚠️ **Attention aux gardes** : `tools/check_ota_signature_switch.sh` et plusieurs suites natives
affirment sur la **forme des messages**. Le renommage les fera rougir — c'est voulu, elles sont
à mettre à jour dans le même lot.

### ⛔ Reste ouvert (la spec RESTE dans `doing/`)

- **D3-c** (refus), **D4**, **D5**. ✅ ~~marche 3 de B12~~ **passée le 2026-11-16** (voir ci-dessus) : **B12 est clos**.
- Hors lot, à ouvrir (revue P1/P2) : **`bug-112` avant D3-c** ; `RouteProtection::CRITICAL` réellement distinct de `WRITE` ; **référence de version indépendante de GitHub** avant toute promotion du « désaccord » en refus.
- ✅ **Bouton physique : fiche ouverte** — [`feature-171`](../todo/feature-171-bouton-physique-autorisation-temporaire.md) (2026-11-16). ⚠️ Elle **renverse l'exigence centrale d'ADR-0125** : l'autorisation ne survivrait plus à un redémarrage — ce qui supprime à la fois l'impasse « sans heure valide, aucune autorisation possible » et toute la dette du résidu NVS. ⛔ Inerte tant que D3-c n'est pas livré.
- 🔴 « Jeton admin volé » : **ouvert, rouvert plus facilement** par l'interrupteur.
- ✅ ~~**Essai terrain du lot non fait**~~ — **FAIT le 2026-11-16** : 2.89.0 signée déployée (19:11), armement et désarmement réels, témoin MQTT basculé ON puis OFF, version affichée sur la page de secours. Voir « F0 — PREUVE DE TERRAIN » ci-dessus. ⚠️ **Seule réserve** : l'**expiration naturelle à 60 min** n'a jamais été observée sur l'appareil (le décompte a été vu décroître, l'autorisation a été consommée par un flash avant son terme). ⇒ essai **F9** de la campagne D3-c.
  ⚠️ Cette ligne disait encore « non fait » alors que la section F0 était déjà écrite au-dessus : **un ajout sans rature laisse deux vérités contradictoires dans le même fichier**, et c'est le lecteur suivant qui trinque (relevé par `spec-reader`, 2026-11-16).

## ✅ bug-112 clos (2.89.1, 2026-11-16) — D3-c débloqué sur ce point
Le journal des refus d'authentification est désormais borné ([ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)). Pourquoi cela compte pour D3-c : selon l'[ADR-0125](../../../docs/adr/0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md), **le journal persistant est le témoin qui survit** ; le témoin MQTT n'en est pas un (courtier sans persistance, appareil hors ligne). Il n'était plus purgeable en ~13 s par un anonyme ; il reste purgeable **à vitesse lente** (pire cas mesuré 38 lignes/24 h par site, aucun majorant établi) — les preuves B11, B12, B14 en héritent.

## ✅ D3-c « le refus » — NOTES D'IMPLÉMENTATION (2026-11-16)

⛔ **La spec RESTE dans `doing/`** : D4 et D5 restent ouverts. [ADR-0128](../../../docs/adr/0128-l-appareil-refuse-une-image-ota-non-authentique.md) acte la décision.

**Livré** : `src/ota_refusal_messages.{h,cpp}` (neufs, purs) — `otaRefusalDecide(verdict)` sans paramètre de page, `otaRefusalPhrase(verdict, pageAnnouncedVersion, site)` ; refus branchés en sortie des blocs d'authenticité figés ([G5] `/update`, [G6] `/download-update`), avant toute écriture et avant les trois `setOtaInProgress(true)` ; 4 blocs « TOLÉRANCE D3-a » supprimés ; repli d'empreinte illisible = refus précoce ; renommage « interrupteur » → « autorisation temporaire de mise à jour non signée » (entité HA « Mise à jour non signée autorisée », `unique_id`/topic inchangés) ; garde CI `tools/check_ota_manager_isolated.sh` (job `ota-manager-isole`) ; interface : phrase de l'appareil affichée, pré-contrôle (absente ≠ non récupérée), un seul `/ota-sig` par genre, 3 sélecteurs `.pick` en français.

**Table** : absente 403 `sig_absente` · malformée 400 `sig_malformee` · invalide 403 `sig_invalide` · vérification impossible 503 `sig_verif_impossible` · genre discordant 415 `sig_genre_discordant` · plancher 403 `sig_sous_plancher` (inatteignable) · liaison tardive 400 texte / 422 `integrity_mismatch`. Dissymétrie assumée : `/update` `text/plain` sans `code`, `/download-update` JSON `error` + `code`. ⛔ Jamais refusés : désaccord de version, plancher.

**Fichiers** : `src/ota_refusal_messages.{h,cpp}`, `src/web_routes_ota.cpp`, `src/web_routes_ota_switch.cpp`, `src/ota_signature_switch.{h,cpp}`, `src/ota_authenticity_logic.h`, `src/ota_recovery_page.{cpp,html.in}`, `src/mqtt_manager.cpp`, `src/constants.h`, `data/{app.js,app.css,index.html}`, `tools/check_ota_manager_isolated.sh`, `.github/workflows/hygiene-specs.yml`, `test/test_native_ota_refusal_messages/`, `test/test_native_ota_order_guard/`.

**Mesures (relevées par l'orchestrateur)** : `pio run` SUCCESS, 0 warning, flash 52,3 % / 1 952 961 o, RAM 30,0 % / 98 256 o ; `pio test -e native` 87 suites / 2 583 cas / 0 échec ; `./build_fs.sh` SUCCESS ; page de secours 16 625 o.

### ⚠️ Campagne de terrain D3-c — NON FAITE
Douze essais (G0 à F11) restent à mener sur l'appareil. ⚠️ **L'expiration naturelle à 60 min de l'autorisation temporaire n'a jamais été observée** sur l'appareil. Le lot est livré **sans** preuve de terrain du refus : ne pas le présenter comme éprouvé.

### 🔴 Consigné dans l'ADR-0128
Le vecteur « jeton admin volé » reste ouvert (deux requêtes). ADR-0022 déclare encore `ArduinoOTA` actif alors qu'il n'a aucun appelant : le rebrancher contournerait totalement le refus ; `check_ota_manager_isolated.sh` l'interdit désormais.

### ⛔ Reste ouvert
D4, D5, la campagne de terrain D3-c ci-dessus.
