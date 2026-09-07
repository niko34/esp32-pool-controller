# Subsystem — `ota_manager`

- **Fichiers** : [`src/ota_manager.h`](../../src/ota_manager.h), [`src/ota_manager.cpp`](../../src/ota_manager.cpp)
- **Singleton** : `extern OTAManager otaManager;`
- **Lib** : ArduinoOTA (ESP32 core)

## Rôle

Gère les mises à jour OTA **firmware** et **filesystem** (LittleFS). Supporte deux canaux :
1. **Arduino OTA** (push IDE / PlatformIO) — réservé au développement, désactivé en production (`otaEnabled = false` par défaut).
2. **HTTP OTA** via les routes web `/update`, `/check-update`, `/download-update` — canal utilisé par l'UI.

## Partitions

Double-bank app0/app1 pour l'OTA firmware :

| Partition | Type | Taille |
|-----------|------|--------|
| `nvs` | data | 20 KB |
| `otadata` | data | 8 KB |
| `app0` | app | **1664 KB** |
| `app1` | app | **1664 KB** |
| `spiffs` | data | 576 KB |
| `history` | data | 64 KB |
| `coredump` | data | 64 KB |

Layout v3 — voir [`partitions.csv`](../../partitions.csv), [ADR-0007](../adr/0007-table-partitions-custom.md) et [ADR-0019](../adr/0019-partition-app-1664k.md).

## API publique

```cpp
void begin();
void handle();                       // appelé dans loop() si OTA enabled
void setPassword(const String& password);
bool isEnabled() const;
```

## Flow OTA HTTP (UI → firmware)

1. Client appelle `GET /check-update` → le firmware interroge l'API GitHub Releases pour comparer la version courante (`FIRMWARE_VERSION` dans [`version.h`](../../src/version.h)) à la dernière release. La réponse relaie aussi les empreintes SHA-256 des assets (`firmware_digest` / `filesystem_digest`, champ `digest` de l'API GitHub). Depuis la v2.16.1 ([ADR-0032, extension 2026-08-04](../adr/0032-reponses-http-bornees.md#extension-2026-08-04--check-update-parse-en-flux-entrant-v2161)), la réponse GitHub (~10 Ko, croissante à chaque release) est parsée **en flux** (`useHTTP10(true)` + filtre 7 champs) sur un `JsonDocument` à allocateur **plafonné net** (`BudgetJsonAllocator`, `kOtaCheckJsonBudgetBytes` = 8192 o) — pic mémoire du handler ≤ 8 Ko hors session TLS (~48 Ko incompressible) ; corps hors budget → `500` « GitHub response too large » + log dédié. Les notes de release sont tronquées à `kOtaReleaseNotesMaxChars` (2048) sur **frontière UTF-8** (`utf8SafeCutLen`, [`ota_check_logic.h`](../../src/ota_check_logic.h)).

   **Diagnostics (v2.16.2, spec `bug-checkupdate-tls-core3`)** : le handler logge l'état heap **avant** d'ouvrir la session TLS — « heap libre=… o, plus gros bloc=… o » (`ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`, distingue épuisement total et fragmentation). Sur un code HTTP négatif (échec client TLS/socket), le log « Erreur HTTP GitHub » porte le détail mbedtls (`client.lastError()`, message + code) et le libellé `HTTPClient::errorToString()`.

   > ⚠️ **Limitation connue — core Arduino 3 uniquement** ([ADR-0034, note additive 2026-08-08](../adr/0034-migration-core3-idf5.md)) : sous core 3 / IDF 5 sur ESP32 classique (sans PSRAM), le plus gros bloc heap contigu (~43-47 Ko dès le boot) ne suffit pas à la session TLS sortante (~50 Ko en pointe) → `/check-update` et `/download-update` (GitHub) échouent systématiquement en `500` propre, sans crash — les erreurs mbedtls loggées (`X509 alloc failed`, « verify failed ») masquent un échec d'allocation. Limitation **acceptée** en son temps ; contournement : upload manuel `/update`. **Levier appliqué** : module WROVER-E + PSRAM (feature-066, [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md)) — c'est ce qui a permis de faire du core 3 l'environnement de production ([ADR-0042](../adr/0042-core3-environnement-par-defaut.md)). Sur un module **sans** PSRAM en core 3, la limitation reste entière.
2. Client appelle `POST /download-update` avec l'URL **et le paramètre `digest=` (requis, fail-closed)**.
3. Le firmware télécharge le `.bin` via HTTPS depuis `github.com` (certificat racine embarqué dans [`github_root_ca.h`](../../src/github_root_ca.h)), en calculant le SHA-256 du flux au fil de l'eau.
4. Vérification d'intégrité (voir ci-dessous), puis flash validé dans la partition inactive (Update API ESP32) ou partition `spiffs`.
5. Reboot après `kRestartAfterOtaDelayMs = 3000` ms ([`constants.h:26`](../../src/constants.h:26)).

Pour l'upload manuel : `POST /update` (multipart `.bin`).

## Vérification d'intégrité SHA-256 (feature-026, [ADR-0022](../adr/0022-verification-integrite-ota.md))

Règle de sécurité #6 (« toujours vérifier la signature/CRC avant flash »), implémentée dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) avec le hacheur incrémental `OtaStreamHasher` ([`src/ota_integrity.h`](../../src/ota_integrity.h), mbedtls avec accélération matérielle, O(len) par chunk — non bloquant pour AsyncWebServer) et la logique pure [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) (`parseSha256Digest`, `sha256Equal` en temps constant, `sha256ToHex` — 22 tests natifs dans `test/test_native_ota_integrity/`).

### Politique par chemin

| Chemin | Politique | Détail |
|---|---|---|
| GitHub (`/download-update`) | **Fail-closed strict** | Paramètre `digest=` (`sha256:<64hex>`, relayé par l'UI depuis `/check-update`) **requis**. Absent → `400 integrity_digest_missing` (refus **avant** toute connexion) ; malformé → `400 integrity_digest_invalid`. Mismatch en fin de flux → **firmware** : `Update.abort()` avant tout `end()` (l'image n'est jamais activée), `422 integrity_mismatch` ; **filesystem** : `422` sans restart, remontage LittleFS best-effort, re-tentative requise (partition FS suspecte, firmware intact et bootable). Couvre le téléchargement tronqué (coupure WiFi). |
| Upload manuel (`/update`) | Empreinte **loggée**, comparaison **optionnelle** | SHA-256 du flux toujours calculé et loggé (`info`). Paramètre POST `sha256` optionnel (avec ou sans préfixe `sha256:`) : mismatch ou valeur malformée → `Update.abort()`, réponse **`400 FAIL: <cause>`** (v2.20.1 — auparavant `200` avec corps `FAIL`), log `CRITICAL`. Sans paramètre : flash autorisé (utilisateur physiquement présent — fail-open documenté). |
| ArduinoOTA / espota | Déjà protégé | Le protocole espota transmet le MD5 de l'image, vérifié par la stack `Update`. Non modifié. |

<a id="authentification-du-handler-dupload-v2462"></a>

## Authentification du handler d'UPLOAD `POST /update` (v2.46.2, `bug-117`)

Jusqu'en 2.46.1 incluse, `handleOtaUpdate()` ([`src/web_routes_ota.cpp:360`](../../src/web_routes_ota.cpp:360)) — le **handler d'upload** de `POST /update` — ne contenait **aucune** vérification d'authentification : ni `authManager`, ni `checkAuth`, ni `REQUIRE_AUTH`, ni `checkTokenAuth`, ni `checkBasicAuth`. Le `REQUIRE_AUTH(req, RouteProtection::CRITICAL)` n'existait que dans le lambda de **réponse**, exécuté **après** un handler qui appelait `Update.begin()` puis `Update.end(true)`. Un client **non authentifié** du réseau local pouvait faire **flasher une image arbitraire** — donc prendre le contrôle des pompes doseuses ; le `401` partait après le flash.

**Même topologie** que les défauts corrigés sur [`/save-config`](../API.md#post-save-config--critical) (v2.25.0, `saveConfigBodyAuthorized()`) et sur les [trois routes de calibration](sensors.md#authentification-du-corps-des-routes-de-calibration-v2461) (v2.46.1, `calibrationBodyAuthorized()`), avec l'effet le plus lourd des trois : l'écriture d'une partition applicative.

### La garde `otaUploadAuthorized()`

[`src/web_routes_ota.cpp:290`](../../src/web_routes_ota.cpp:290) — appelée en **première instruction** du handler d'upload, **avant** le verrou `g_uploadOwner`, avant le pré-contrôle du type et avant tout effet de bord.

- **`fail-closed` sur pointeur nul**, puis `authManager.isEnabled()` faux → autorisé ; sinon `checkBasicAuth() || checkTokenAuth()`.
- **Aucun état touché sur le chemin de refus** : aucune statique, pas de `g_uploadOwner`, pas de `markUploadRejected()`, rien sur `Update`, ni empreinte, ni rollback, ni pré-contrôle du type. C'est `REQUIRE_AUTH`, dans le lambda de réponse, qui émet le `401`.
- **Pas de `checkRateLimit()`** : il ne peut que **refuser**, jamais accepter — l'omettre élargit l'ensemble accepté, il ne le restreint pas. Une seule réponse au client, un seul décompte de quota.
- **Pas d'exemption « premier démarrage »** : celle de `checkAuth()` est bornée aux URL commençant par `/save-config` et n'a **jamais** couvert `/update`. Son absence n'ajoute donc aucune restriction — l'ajouter serait un trou.
- La garde **ne remplace pas** `REQUIRE_AUTH` et n'en est **pas** redondante : `REQUIRE_AUTH` produit le `401` propre, la garde empêche l'**effet de bord**. Supprimer l'une ou l'autre réouvre le trou.

### Chercher la PLACE de la garde, jamais sa seule présence

C'est le livrable réutilisable de ce cycle, et il vaut au-delà de l'OTA : **trois** défauts de sécurité relevés le 2026-09-07, tous survivants à des revues pour la **même** raison — un garde-fou **visible** fait passer le relecteur.

| Garde-fou visible | Ce qu'il contrôlait réellement | Défaut |
|---|---|---|
| `REQUIRE_AUTH` présent trois lignes plus bas | la **réponse**, pas le corps | routes de calibration (v2.46.1) |
| une sentinelle de parité `/data` ↔ WebSocket | elle déclarait **légitime** le bug qu'elle devait signaler | `bug-082` |
| le pré-contrôle de l'**octet magique** | la **validité** de l'image, prise pour une **autorisation** | `bug-117`, cette section |

L'audit du 07/09 avait d'ailleurs classé `POST /update` **saine** en s'appuyant sur ce pré-contrôle (« refus AVANT `setOtaInProgress`, `LittleFS.end()` et `Update.begin()` »). Le défaut n'a été trouvé qu'en **refusant de reprendre ce verdict** et en relisant la fonction. Règle : **chercher la place de la garde dans le cycle de vie de la requête, jamais sa seule présence.**

### Réévaluation à CHAQUE fragment — et pourquoi

La garde n'est **pas** évaluée seulement à `index == 0` : elle est réévaluée à **chaque** fragment. Un client ne doit pas pouvoir la franchir une fois puis pousser la suite du flux. Aucun cache par pointeur de requête n'est admissible : un pointeur **réutilisé après `free`** accorderait l'authentification à une autre requête.

**Coût assumé, pas coût évité** — la justification initiale de l'ordre des prédicats était **inversée**, corrigée après revue : `request->authenticate()` court-circuite déjà sur `_authorization` vide, donc le chemin **jeton** n'allouait rien de toute façon. C'est le chemin **basique** — celui d'`ota_update.sh` par défaut — qui paie `generateBasicHash()` (2 `new char[]` + une `String`) à chaque fragment, soit **≈ 1 000 fois pour un firmware de 1,5 Mo**, dans la tâche réseau et en famine mémoire potentielle. **Aucun ordre des prédicats n'évite ce coût**, la réévaluation par fragment étant délibérée. Allocations transitoires et LIFO, jugées non bloquantes en revue.

Le court-circuit `hasHeader("X-Auth-Token")` reste, lui, **strictement équivalent** (`checkTokenAuth()` ne peut retourner `true` que si l'en-tête existe) et évite la `String` de son `systemLogger.debug("Aucun header X-Auth-Token trouvé")` — sinon ~1 000 lignes de journal pendant un flash.

### Disponibilité des en-têtes au premier fragment — vérifiée, pas supposée

Sur **ESPAsyncWebServer 3.11.2** : `_parseReqHeader()` renseigne `_authorization` / `_authMethod` **et** empile chaque en-tête dans `_headers` pendant l'état `PARSE_REQ_HEADERS` ; `_parseMultipartPostByte()` → `_handleUploadByte()` ne tourne qu'**ensuite**, et **rien ne purge `_headers`** avant la fin de la requête. Les deux méthodes sont donc lisibles dès `index == 0` et le restent jusqu'au fragment final. Le commentaire « check if authenticated before calling the upload » de `WebRequest.cpp:738` n'est suivi d'**aucun** contrôle en amont : la responsabilité est bien à notre charge.

**Les trois clients légitimes passent** (équivalence vérifiée dans les deux sens — indispensable, l'OTA étant le **seul** canal de déploiement de cet appareil) : `ota_update.sh` en authentification basique, le même en jeton, et la page web d'upload ([`data/app.js:7715`](../../data/app.js:7715), XHR + `X-Auth-Token`).

### Bénéfice second, non anticipé : `setOtaInProgress` n'est plus atteignable sans authentification

Relevé **en revue**, pas à la conception. `PumpController::setOtaInProgress(true)` **coupe les pompes doseuses** (voir [Interaction avec `pump_controller`](#interaction-avec-pump_controller)). Il était atteignable depuis le handler d'upload, donc **sans authentification** : un attaquant du réseau local pouvait **suspendre la régulation pH/ORP** en déclarant un OTA fantôme — sans jamais avoir à fournir d'image valide, et sans laisser autre chose qu'un « Type de mise à jour » au journal. La garde ferme ce vecteur en même temps que le flash.

### WARN throttlé — et la LIMITE de ce throttle

Une tentative non authentifiée est journalisée `WARNING` **au plus 1 fois/60 s** pour ce site (statique locale, seuil `kCalibrationAuthWarnThrottleMs` **réutilisé tel quel** : même valeur, même motif et même chiffrage que pour les routes de calibration — une constante jumelle n'aurait dupliqué que la démonstration ; le nom mériterait d'être généralisé en `kBodyAuthWarnThrottleMs` le jour où `constants.h` sera rouvert). **Une seule fois par upload**, au premier fragment :

```
[OTA] POST /update NON AUTHENTIFIÉ depuis 192.168.1.50 — upload ignoré (aucun Update.begin(), aucune écriture de partition) ; et 12 tentatives non journalisées
```

- **Le quota `kMaxRequestsPerMinute` ne borne pas cet émetteur** : `checkRateLimit()` est appelé par `checkAuth()` dans le handler de **requête**, donc après l'upload. Le handler d'upload tourne pour **chaque** requête reçue, quota dépassé ou non.
- **Invariant : le chemin muet n'alloue rien** — sortie avant toute concaténation. Ce chemin est atteignable sans authentification et s'exécute en RAM interne pendant une famine mémoire active (`bug-080`).
- L'IP est lue sans jamais déréférencer un client nul : le client peut avoir disparu entre l'arrivée du fragment et la journalisation.
- ⚠️ **LIMITE — motif initialement surévalué, corrigé après revue.** Ce throttle **réduit la pression, il ne ferme pas le vecteur**. `AuthManager::sendAuthRequired()` ([`auth.cpp:234`](../../src/auth.cpp:234)) émet déjà, dans le lambda de réponse, **un `WARNING` NON throttlé par requête** sur cette même route. La fenêtre forensique reste donc **purgeable sans authentification**, et ce throttle-ci n'y change rien à lui seul. Ne pas conclure de sa présence que le vecteur est fermé — l'émetteur générique est traité par [`bug-112`](../../specs/features/todo/bug-112-journal-non-borne-sur-route-protegee.md). L'avertissement est porté dans le commentaire du code.

### `/download-update` : audité SAIN, pour une raison structurelle

Cette route est enregistrée en `server->on("/download-update", HTTP_POST, lambda)` à **trois** arguments ([`src/web_routes_ota.cpp:1329`](../../src/web_routes_ota.cpp:1329)) : **aucun handler de corps, aucun handler d'upload**. Il n'existe donc **aucun** point d'entrée exécuté avant le lambda de requête — `REQUIRE_AUTH(req, RouteProtection::CRITICAL)` s'exécute **nécessairement** avant tout accès à `Update`. Le défaut de `POST /update` ne peut pas s'y produire.

Consigné pour **ne pas re-auditer la même route** : la question ne se rouvre que si un 4ᵉ ou 5ᵉ argument est ajouté à cet enregistrement.

### Ce que ce correctif ne fait PAS

Il apporte l'**autorisation** de l'appelant, pas l'**authenticité** de l'image. La comparaison SHA-256 de l'upload manuel reste conditionnée à `g_uploadHasExpected` — donc au bon vouloir du client — et une empreinte **fournie par l'appelant** prouve l'intégrité du **transport**, jamais l'authenticité de l'image. La règle de sécurité n° 6 de `CLAUDE.md` (« vérifier la signature/CRC avant flash ») n'est donc **pas** entièrement satisfaite sur ce chemin : point renvoyé à [`feature-118`](../../specs/features/todo/feature-118-authenticite-de-l-image-ota.md).

### Verrou anti-upload concurrent (`/update`)

Le handler d'upload est appelé par chunks et son état vit dans des statiques de fichier (`g_uploadHasher`, `g_uploadLastLog`, empreinte attendue…), réinitialisées à `index == 0`. Un second upload lancé pendant qu'un premier est en cours corromprait cet état :

- **`g_uploadOwner`** : une seule requête propriétaire de la session. Une requête concurrente pendant que `Update.isRunning()` est rejetée sans toucher aux statiques → réponse **`409`** (« Upload refusé: une mise à jour OTA est déjà en cours »), marqueur par requête via `_tempObject` (libéré par le destructeur — pas de fuite).
- **`onDisconnect`** : client déconnecté en plein transfert → `Update.abort()`, `PumpController.setOtaInProgress(false)` (réarmement du dosage), verrou libéré.
- **Owner périmé** : si `!Update.isRunning()`, une nouvelle requête reprend la main (ceinture-bretelles).
- **Fix AC5** : le throttle de progression (`g_uploadLastLog`, ex-`static lastLog`) est réinitialisé à `index == 0` — deux uploads successifs sans reboot affichent une progression correcte.

Limite cosmétique connue : après reprise d'un owner périmé, l'ancien client peut recevoir un « OK » trompeur (consigné en spec, sans impact sécurité).

### Appariement `setOtaInProgress`

Chaque `PumpController.setOtaInProgress(true)` a son `false` sur **tous** les chemins terminaux : succès, échec `Update.begin()` (fuite corrigée en revue), échec d'écriture (v2.20.1), mismatch d'intégrité (firmware et FS), digest invalide, déconnexion client. Sans cela, les pompes doseuses resteraient inhibées jusqu'au reboot.

<a id="ordre-impose-dosage-avant-montage-v2201"></a>

### ORDRE IMPOSÉ — `setOtaInProgress(false)` AVANT tout `LittleFS.begin()` (v2.20.1)

Sur **tous** les chemins terminaux, le réarmement du dosage précède le remontage du filesystem. Ce n'est pas un détail de style : ces handlers s'exécutent sur la **tâche AsyncTCP, qui n'est pas abonnée au watchdog**. Un montage LittleFS long — ou qui panique sur une partition à moitié écrasée — laisserait les pompes doseuses **inhibées sans redémarrage possible** : le watchdog ne peut pas sauver une tâche à laquelle il n'est pas abonné. En libérant le dosage d'abord, le pire cas d'un montage bloquant reste une interface web indisponible, pas une régulation chimique morte.

Gate `pool-chemistry`. L'existant du chemin URL (`/download-update`) a été corrigé dans ce sens en même temps. **Ne pas réordonner en revue.**

<a id="une-seule-erreur-journalisee-par-session-v2201"></a>

### Une seule erreur journalisée par session d'upload (v2.20.1)

`g_uploadErrorLogged` + helper `failOtaUpload()` : un échec d'upload produit **exactement une** entrée de journal, quelle que soit la taille du fichier restant à transférer.

**Pourquoi** : après `Update.abort()`, `Update.write()` retourne 0 à chaque appel (garde `hasError()`). Sans garde-fou, **chaque chunk restant** re-journalisait la même erreur — ~200 lignes identiques pour une seule tentative ratée. L'anneau de journal ne fait que `kMaxLogEntries = 200` entrées ([`constants.h`](../../src/constants.h)) : **une tentative ratée effaçait donc tout le journal**, y compris les lignes qui désignaient le coupable (`Type de mise à jour: …`, `Début mise à jour OTA: …`) et y compris les traces d'un incident sans rapport en cours d'analyse. Un outil de diagnostic qui détruit les preuves de tout autre diagnostic.

Complément indissociable : le chemin d'échec d'écriture est devenu **terminal** (libération de `g_uploadOwner` + `return`), les chunks suivants n'entrent plus dans la logique d'écriture.

**Corollaire — capturer `errorString()` AVANT `abort()`** : `Update.abort()` fait `_abort(UPDATE_ERROR_ABORT)`, qui **remplace** le code d'erreur réel (`MAGIC_BYTE`, `ERASE`, `WRITE`, `SPACE`…). Après `abort()`, la cause est perdue définitivement — c'est ce qui produisait le fameux « Aborted » sans information. Les **deux** sites d'écriture (upload manuel et téléchargement URL) capturent désormais `Update.errorString()` dans une `String` locale avant toute autre opération sur `Update`.

<a id="politique-de-refus--asymetrie-assumee-v2201"></a>

### Politique de refus — asymétrie assumée (v2.20.1)

**Principe général, à ne pas « simplifier » au prochain refactor :**

> On ne bloque en **dur** que là où le contrôle est **prouvablement équivalent** à ce que la couche sous-jacente impose déjà ; partout ailleurs, on **trace**.

Application au pré-contrôle du type d'image ([`src/ota_type_logic.h`](../../src/ota_type_logic.h), `checkOtaFirstByte`), exécuté à `index == 0` **avant** `setOtaInProgress(true)`, **avant** `LittleFS.end()` et **avant** `Update.begin()` :

| Type déclaré | Premier octet | Verdict | Justification |
|---|---|---|---|
| `firmware` (`U_FLASH`) | ≠ `0xE9` | **Refus dur** — `400`, aucun effet de bord | `UpdateClass::_writeBuffer()` vérifie déjà `ESP_IMAGE_HEADER_MAGIC` sur le premier octet du premier tampon : l'ensemble des images **acceptées est INCHANGÉ**, on ne fait que déplacer le refus en amont pour produire une cause exploitable |
| `firmware` | `0xE9` | Accepte | — |
| `filesystem` (`U_SPIFFS`) | `0xE9` | **Avertissement seul**, poursuite | LittleFS n'a **pas** de magique fiable à l'offset 0 : un `0xE9` fortuit y est parfaitement possible. Un refus dur produirait des **faux positifs** sur des images parfaitement valides — on n'a aucune équivalence à invoquer, donc on trace |
| `filesystem` | ≠ `0xE9` | Accepte | — |

**Réserve bloquante** (consignée dans le header) : l'équivalence n'est vraie que tant que le projet n'appelle **jamais** `Update.setupCrypt()` / `setCryptKey()` — vrai aujourd'hui, aucune occurrence dans `src/`. Avec le chiffrement d'image actif, le bloc crypto s'applique **en amont** du contrôle magique et l'erreur réellement produite par le framework devient « Decryption error » : notre octet 0 ne serait plus l'octet contrôlé, et le refus dur deviendrait un blocage d'images valides. Corollaire : **ne jamais parser `Update.errorString()`** pour décider — c'est ce pré-contrôle, et lui seul, qui produit le message « pas un firmware ».

Le chemin de refus pose `markUploadRejected()` : sans marqueur par requête, `Update.hasError()` vaudrait `false` (aucun `begin()` n'a eu lieu) et le lambda de réponse servirait « OK » **avec redémarrage parasite** d'un appareil qui n'a rien flashé.

<a id="remontage-du-filesystem-sur-echec-v2201"></a>

### Remontage du filesystem sur échec (v2.20.1)

`remountUploadFsIfNeeded()` : si la session a démonté LittleFS (`g_uploadFsUnmounted`, posé après un `LittleFS.end()` réussi), tout chemin terminal tente `LittleFS.begin(false)`. Sans cela, l'interface web n'était plus servie du tout jusqu'au prochain redémarrage.

- **`LittleFS.begin(false)` — JAMAIS `begin(true)`.** Le paramètre `true` formaterait la partition en cas d'échec de montage : on détruirait l'interface encore servie aux clients connectés pour « réparer » un incident réversible. **Interdiction absolue**, non négociable.
- **Une seule tentative par session** : le drapeau est remis à `false` quel que soit le résultat — pas de boucle de montage sur une image corrompue.
- **Risque assumé, d'où le log `critical`** : après un échec en cours de flash FS, une partie de l'ancienne image a pu être **écrasée**. Le remontage peut donc réussir sur un contenu **incohérent** (fichiers anciens et nouveaux mélangés). Le message le dit explicitement : `CRITICAL Filesystem remonté après une mise à jour FS interrompue : contenu potentiellement incohérent, relancer la mise à jour`. Après un flash **réussi**, le même helper journalise en `info` (`afterFailure = false`) — l'image est saine, aucune alerte.
- Échec du remontage → `WARN Impossible de remonter le filesystem après la mise à jour — redémarrage requis`.
- **No-op** pour une mise à jour firmware : le filesystem n'a jamais été démonté.

L'alternative (ne rien remonter, exiger un redémarrage) reste réversible en trois lignes si le remontage se révélait problématique — c'est l'une des raisons pour lesquelles cette décision n'a **pas** justifié d'ADR.

### Constantes et logs

- `kOtaSha256HexLen = 64`, `kOtaSha256Prefix = "sha256:"` ([`constants.h`](../../src/constants.h)).
- Tout refus d'intégrité émet un log **`CRITICAL`** avec empreintes attendue/calculée en hexadécimal ; l'empreinte de chaque upload manuel est loggée en `info`.

## Interaction avec `pump_controller`

**Sécurité critique** : dès qu'une OTA démarre, `PumpController.setOtaInProgress(true)` est appelé → `stopAll()` coupe les pompes doseuses. L'interruption d'une OTA firmware pendant une injection laisserait la pompe à un état indéterminé.

## Interaction avec `history`

L'historique est **préservé** pendant une OTA filesystem car il vit sur une partition distincte (`history` vs `spiffs`). Voir [ADR-0007](../adr/0007-table-partitions-custom.md).

## Yield pendant OTA

`kOtaYieldDelayMs = 1` ms ([`constants.h:11`](../../src/constants.h:11)) pour laisser respirer le watchdog ESP32 pendant le flash.

## Cas limites

- **Interruption WiFi pendant download** : l'empreinte SHA-256 du flux incomplet ne correspond pas au digest attendu → `Update.abort()`, log `CRITICAL`, pas de corruption, la carte reste sur la version courante.
- **Partition pleine** : refus au moment du `Update.begin()` → erreur HTTP 500 (corps `FAIL: démarrage de la mise à jour impossible (<errorString>)`), log `ERROR`, dosage réarmé et filesystem remonté.
- **Image filesystem envoyée en type « firmware »** (le piège corrigé en v2.20.1) : refus `400` au premier octet, **aucun** effet de bord — pompes jamais inhibées, filesystem jamais démonté, aucun redémarrage. Une ligne de journal.
- **Version GitHub < locale** : pas de mise à jour proposée côté UI (comparaison semver).

## Fichiers liés

- [`src/ota_manager.h`](../../src/ota_manager.h), [`src/ota_manager.cpp`](../../src/ota_manager.cpp)
- [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) — routes `/update`, `/check-update`, `/download-update`
- [`src/ota_integrity.h`](../../src/ota_integrity.h) / [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) — vérification d'intégrité SHA-256
- [`src/ota_type_logic.h`](../../src/ota_type_logic.h) — logique pure du type d'image (`inferOtaKindFromFilename`, `checkOtaFirstByte` — 29 tests natifs `test/test_native_ota_type/`) ; miroir JS `detectUpdateType()` dans [`data/app.js`](../../data/app.js)
- [`src/ota_check_logic.h`](../../src/ota_check_logic.h) — logique pure `/check-update` (`AllocBudget`, `utf8SafeCutLen` — 13 tests natifs `test/test_native_ota_check/`)
- [`src/github_root_ca.h`](../../src/github_root_ca.h) — 3 racines **auto-signées** (v2.16.2 : les variantes USERTrust cross-signées AAA sont retirées) : USERTrust RSA + USERTrust ECC (github.com / api.github.com, exp. 2038), ISRG Root X1 (objects.githubusercontent.com, exp. 2035) — empreintes SHA-256 vérifiées, sources consignées dans le header
- [`partitions.csv`](../../partitions.csv)
- [docs/UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- [ADR-0007](../adr/0007-table-partitions-custom.md), [ADR-0022](../adr/0022-verification-integrite-ota.md)
