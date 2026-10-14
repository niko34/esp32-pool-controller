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

   ⚠️ **Réponse d'échec enrichie (`bug-165`, v2.80.x)** — avant ce lot, tout code HTTP négatif
   produisait la **même** réponse `{"error":"GitHub API error","code":-1}`, muette sur l'étape et
   la raison. Le corps JSON porte désormais deux champs supplémentaires, dérivés de la décision
   **pure** `classifyOtaCheckFailure(httpCode, tlsErrorCode)` ([`ota_check_logic.h`](../../src/ota_check_logic.h),
   testée en natif et par mutation — le cas qui tue la régression « priorité inversée » est le
   symptôme réel du bug : `httpCode == -1` **et** `tlsErrorCode != 0` simultanément, où le signal
   TLS doit l'emporter) :
   - `step` : `"tls_handshake"` (chaîne de certificats refusée, `tlsErrorCode != 0`) /
     `"connection"` (résolution de nom ou TCP refusé, `httpCode == -1` **sans** erreur TLS) /
     `"http"` (tout autre code HTTPClient négatif — délai dépassé, encodage…).
   - `detail` : le message mbedtls réel (`client.lastError()`) pour `tls_handshake`, un texte fixe
     pour `connection`, `HTTPClient::errorToString()` pour `http` — **même** texte que celui déjà
     journalisé, aucune IP ni chemin ni secret.

   ⚠️ **Le 404 « aucune release publiée » est un cas à part, intercepté AVANT cette
   classification** (réponse `200` avec `"no_release": true` — comportement antérieur, inchangé) :
   une réponse HTTP **reçue** (404, le dépôt existe mais n'a aucune release) ne doit **jamais** se
   confondre avec une réponse **absente** (`-1`, échec réseau) — les deux produisaient le **même**
   `code: -1` tant que le défaut ci-dessous empêchait toute réponse GitHub d'être reçue. Verrouillé
   par un test dédié (`test_classify_404_is_never_connection_failure`).

   > ⚠️ **Limitation connue — core Arduino 3 uniquement** ([ADR-0034, note additive 2026-08-08](../adr/0034-migration-core3-idf5.md)) : sous core 3 / IDF 5 sur ESP32 classique (sans PSRAM), le plus gros bloc heap contigu (~43-47 Ko dès le boot) ne suffit pas à la session TLS sortante (~50 Ko en pointe) → `/check-update` et `/download-update` (GitHub) échouent systématiquement en `500` propre, sans crash — les erreurs mbedtls loggées (`X509 alloc failed`, « verify failed ») masquent un échec d'allocation. Limitation **acceptée** en son temps ; contournement : upload manuel `/update`. **Levier appliqué** : module WROVER-E + PSRAM (feature-066, [ADR-0036](../adr/0036-psram-requise-mise-a-jour-en-ligne.md)) — c'est ce qui a permis de faire du core 3 l'environnement de production ([ADR-0042](../adr/0042-core3-environnement-par-defaut.md)). Sur un module **sans** PSRAM en core 3, la limitation reste entière.
2. Client appelle `POST /download-update` avec l'URL **et le paramètre `digest=` (requis, fail-closed)**.
3. Le firmware télécharge le `.bin` via HTTPS depuis `github.com` (certificat racine embarqué dans [`github_root_ca.h`](../../src/github_root_ca.h)), en calculant le SHA-256 du flux au fil de l'eau.
4. Vérification d'intégrité (voir ci-dessous), puis flash validé dans la partition inactive (Update API ESP32) ou partition `spiffs`.
5. Reboot après `kRestartAfterOtaDelayMs = 3000` ms ([`constants.h:26`](../../src/constants.h:26)).

Pour l'upload manuel : `POST /update` (multipart `.bin`).

## Règle — une ancre de confiance est toujours la variante AUTO-SIGNÉE (bug-checkupdate-tls-core3, bug-165, [ADR-0111](../adr/0111-ancre-de-confiance-tls-toujours-auto-signee.md))

⚠️ **Cette règle a mordu deux fois** (2026-08-08 puis 2026-10-14) — elle n'est donc plus une
consigne à retenir, mais un **contrôle automatique** (voir plus bas). La règle elle-même :

> **mbedtls 3.x (core Arduino 3 / IDF 5) refuse les ancres de confiance CROSS-SIGNÉES**, que
> mbedtls 2.28 (core 2) tolérait. Une racine « croisée » (cross-signed) porte **le même nom** et
> **la même clé publique** que sa variante auto-signée — seule la **signature de l'émetteur**
> diffère (une autorité croisée signe la racine d'une autre, pour une transition de confiance
> progressive côté navigateurs). ⛔ **Vérifier le nom ne suffit pas** : il faut vérifier que le
> certificat est **auto-signé** (`subject == issuer` **et** signature vérifiable avec sa propre
> clé publique), pas seulement qu'un certificat du bon nom est présent.

### Les deux incidents, et pourquoi la règle n'a pas suffi la première fois

- **2026-08-08 (`bug-checkupdate-tls-core3`)** : GitHub servait une chaîne terminant sur les
  variantes **USERTrust cross-signées par « AAA Certificate Services »**. Remplacées par les
  variantes auto-signées (USERTrust RSA, USERTrust ECC) — 3 racines au total avec ISRG Root X1
  (CDN de téléchargement). La règle « toutes auto-signées » est posée **en commentaire**, dans
  [`github_root_ca.h`](../../src/github_root_ca.h).
- **2026-10-14 (`bug-165`)** : **la chaîne servie EN PRATIQUE a changé** — GitHub sert désormais
  `api.github.com ← Sectigo Public Server Authentication CA DV E36 ← Sectigo Public Server
  Authentication Root E46 (variante CROISÉE, signée par USERTrust ECC)`. **Le commentaire posé en
  août disait la règle, mais rien ne la réappliquait** quand la chaîne réelle a évolué — une
  consigne écrite n'est vérifiée que par la mémoire de quelqu'un, qui s'use. Corrigé : ajout des
  racines **Sectigo Root E46** (ECC, auto-signée) et **Root R46** (RSA, auto-signée — chaîne de
  repli réelle de GitHub pour les clients qui ne négocient pas ECDSA, vérifié, pas anticipé) — 5
  racines au total.

### ⚠️ La règle est désormais imposée en CI, pas seulement écrite

[`tools/check_github_root_ca.sh`](../../tools/check_github_root_ca.sh) (job `github-root-ca` du
workflow `hygiene-specs.yml`, voir [BUILD.md](../BUILD.md#intégration-continue-github-actions)) délègue à
[`tools/github_root_ca_vectors.py`](../../tools/github_root_ca_vectors.py), qui **extrait** (via
`openssl`) les certificats réellement présents dans `github_root_ca.h` et vérifie
**mécaniquement** :
1. il y en a bien **5** ;
2. aucune des **3 racines d'origine** n'a disparu (ISRG Root X1 ancre
   `objects.githubusercontent.com`, le chemin de **téléchargement** du firmware — distinct du
   chemin de vérification `api.github.com`) ;
3. **chaque certificat est auto-signé** — `openssl verify -no-CAfile -no-CApath -CAfile <cert>
   <cert>` doit réussir — et pas seulement présent sous le bon nom ;
4. les empreintes SHA-256 **annoncées en commentaire** dans le header correspondent aux
   certificats **réellement** présents (un commentaire qui ment sur l'empreinte est exactement le
   genre d'écart qu'on ne détecte qu'en le vérifiant).

⇒ Si GitHub fait **encore** évoluer sa chaîne, le contrôle échouera dès qu'une racine cross-signée
serait ajoutée par erreur (test 3) — la règle ne dépend plus de la mémoire de quelqu'un.

### ⚠️ Risque identifié, NON vérifié, à tester après déploiement

Le domaine de **téléchargement** (`objects.githubusercontent.com`, étape 3 du flow ci-dessus) sert
**aujourd'hui** (vérifié le 2026-10-14) la chaîne `*.github.io ← YR1 ← Root YR (variante CROISÉE
par ISRG Root X1)`. ⚠️ **C'est exactement la même structure** que celle qui vient de casser
`/check-update` : l'avant-dernier maillon est croisé, et l'appareil n'a embarqué que la racine
historique (ISRG Root X1, auto-signée — mais ce n'est **pas** la même racine que « Root YR »,
même si les deux sont liées à Let's Encrypt/ISRG).

⇒ **Hypothèse, non vérifiée sur appareil réel** : le **téléchargement** du firmware est
probablement **déjà cassé** par ce même mécanisme, sans que personne ne l'ait constaté — parce que
la **recherche** de mise à jour (`/check-update`) échouait **avant** d'atteindre cette étape, donc
ce chemin n'a jamais été exercé en conditions réelles depuis que GitHub sert cette chaîne.

⛔ **Non corrigé par ce lot** — à tester **après déploiement du correctif `/check-update`**, une
fois qu'une release existe pour exercer `POST /download-update` de bout en bout. ⚠️ **Ajouter
« ISRG Root X2 » ne suffirait PAS** : c'est une **autre racine** (succession d'ISRG Root X1 côté
Let's Encrypt), **sans rapport** avec « Root YR » — ajouter X2 ne couvrirait pas la chaîne
réellement observée sur `objects.githubusercontent.com`. Si l'hypothèse se confirme, la racine à
ajouter est la variante **auto-signée** de « Root YR », identifiée par la même méthode que pour
Sectigo E46/R46 ci-dessus (`openssl s_client -connect objects.githubusercontent.com:443
-showcerts`, puis retrouver l'auto-signée de même clé publique).

## Vérification d'intégrité SHA-256 (feature-026, [ADR-0022](../adr/0022-verification-integrite-ota.md))

Règle de sécurité #6 (« toujours vérifier la signature/CRC avant flash »), implémentée dans [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) avec le hacheur incrémental `OtaStreamHasher` ([`src/ota_integrity.h`](../../src/ota_integrity.h), mbedtls avec accélération matérielle, O(len) par chunk — non bloquant pour AsyncWebServer) et la logique pure [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) (`parseSha256Digest`, `sha256Equal` en temps constant, `sha256ToHex` — 22 tests natifs dans `test/test_native_ota_integrity/`).

### Politique par chemin

| Chemin | Politique | Détail |
|---|---|---|
| GitHub (`/download-update`) | **Fail-closed strict** | Paramètre `digest=` (`sha256:<64hex>`, relayé par l'UI depuis `/check-update`) **requis**. Absent → `400 integrity_digest_missing` (refus **avant** toute connexion) ; malformé → `400 integrity_digest_invalid`. Mismatch en fin de flux → **firmware** : `Update.abort()` avant tout `end()` (l'image n'est jamais activée), `422 integrity_mismatch` ; **filesystem** : `422` sans restart, remontage LittleFS best-effort, re-tentative requise (partition FS suspecte, firmware intact et bootable). Couvre le téléchargement tronqué (coupure WiFi). |
| Upload manuel (`/update`) | **Fail-closed depuis v2.67.0** — empreinte **obligatoire** | SHA-256 du flux toujours calculé et loggé (`info`). Depuis v2.67.0 (feature-118 lot B), **absente** → refus `400`, **avant tout effet de bord**, même place que le refus « malformée » (lot A). Les **deux** appelants officiels (page d'upload, `ota_update.sh`) envoient désormais systématiquement le paramètre, et le script **refuse lui-même d'envoyer** si aucun outil de hachage n'est disponible (fail-closed, plus de repli silencieux). Malformée → refus dès réception du paramètre. Non conforme → refus à `final`, avant `Update.end()`. Cette route est désormais au **même niveau d'obligation** que `/download-update` — la **nature** de la preuve (fournie par l'appelant) reste, elle, inchangée. |
| ArduinoOTA / espota | Déjà protégé | Le protocole espota transmet le MD5 de l'image, vérifié par la stack `Update`. Non modifié. |

<a id="lot-a-feature-118-empreinte-envoyee-systematiquement-verdict-journalise"></a>

## Lot A — empreinte envoyée systématiquement, verdict journalisé (v2.66.0, feature-118, [ADR-0096](../adr/0096-empreinte-sha256-obligatoire-sur-post-update.md))

**Ce que ce lot prouve, et ce qu'il ne prouve pas** — à ne jamais diluer : l'empreinte est **calculée par l'appelant** ; elle prouve que l'image écrite est celle qu'il a envoyée, **pas qui l'a produite**. Pour le **firmware**, l'IDF vérifie déjà l'image à l'activation ; l'empreinte ajoute la détection du **mauvais fichier**. Pour le **filesystem**, elle est la **seule** vérification d'intégrité. **La règle de sécurité n° 6 de `CLAUDE.md` n'est PAS satisfaite par ce lot** ; elle le sera par la signature (spec de suivi [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md)). Le mot « sécurité » ne qualifie jamais ce mécanisme — le terme imposé est **« intégrité du transfert »**.

**Trois issues arbitrées** (détail et coûts chiffrés : [ADR-0096](../adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)) : empreinte obligatoire + SHA-256 JS embarqué (**retenue**), empreinte obligatoire sauf page web exemptée (écartée — le firmware ne peut **structurellement pas** distinguer la page web d'un `curl`, tout provient de l'appelant), signature à clé publique (reportée à [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md), coût le plus élevé mais seule option qui réponde à la règle n° 6).

**Côté page web** (`data/app.js`) : SHA-256 pur JS (`createSha256`/`sha256OfFile`), car `crypto.subtle` exige un contexte sécurisé (HTTPS/`localhost`), indisponible sur HTTP clair en LAN. Hachage incrémental par tranches de 256 Kio (jamais le fichier entier en RAM). Le champ `sha256` est ajouté au `FormData` **avant** `update_file` — un champ placé après le fichier se lirait comme **absent** côté firmware (le multipart est lu au fil de l'eau). Un échec de calcul **bloque l'envoi à l'écran**, sans repli silencieux. Vecteurs de test versionnés : [`test/sha256_vectors.js`](../../test/sha256_vectors.js) (chaîne vide, « abc », 55/56/64/65 octets, bloc > 1 Mo entier et fragmenté), extraits **du texte réel livré** dans `data/app.js` (pas une recopie — une divergence future est structurellement impossible), exécutés par [`tools/check_sha256_vectors.sh`](../../tools/check_sha256_vectors.sh). **Confirmé sur navigateur mobile** (essai on-target du 2026-10-11, image de 1,87 Mo) : c'était l'inconnue de tout l'arbitrage, elle est levée.

**Côté script** (`ota_update.sh`, lot A) : calcule l'empreinte via `sha256sum` (Linux) ou `shasum -a 256` (repli macOS), l'envoie **avant** le champ fichier dans l'ordre des arguments `curl`.

**Déplacement du refus « malformée »** : prédicat pur `parseOtaSha256Param` ([`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h)), évalué à `index==0`, **hors** du bloc `if (len >= 1)` (un fichier vide produit un unique appel `len==0, final==true`). Le refus a lieu **avant** `g_uploadOwner = request` — aucun `setOtaInProgress(true)`, aucun `LittleFS.end()`, aucun `Update.begin()` n'est atteignable sur ce chemin. Garde structurelle anti-régression : [`test/test_native_ota_order_guard/`](../../test/test_native_ota_order_guard/) (lit le texte source, vérifie que le contrôle précède bien l'affectation de propriété côté firmware **et** que `sha256` précède `update_file` côté `data/app.js`).

**Observations on-target (toutes levées le 2026-10-11)** : script → firmware et filesystem, page web → firmware et filesystem (ordinateur **et** téléphone), les quatre cas journalisés « conforme ». Une **trouvaille** sur cette étape a directement motivé le contenu du lot B : voir ci-dessous.

<a id="lot-b-feature-118-empreinte-obligatoire-refus-effectif"></a>

## Lot B — empreinte obligatoire, refus effectif (v2.67.0, feature-118, [ADR-0096](../adr/0096-empreinte-sha256-obligatoire-sur-post-update.md))

**Ce que ce lot change par rapport au lot A : la décision devient réelle.** `otaUploadIntegrityVerdict()` ne change pas, mais `otaUploadIntegrityIsRejected()` (renommée depuis `otaUploadWouldLotBReject()` — ce n'est plus une projection, c'est la décision appliquée) est désormais **suivie d'effet** : `kAbsent` et `kNonConform` sont refusés (`kMalformed` l'était déjà depuis le lot A). **Ce que ce mécanisme prouve reste inchangé depuis le lot A** — intégrité du transfert, pas authenticité d'origine ; la règle n° 6 de `CLAUDE.md` reste non satisfaite, voir [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md).

**Refus « absente », même patron que « malformée »** : `OtaSha256ParamVerdict::kAbsent` déclenche désormais, à la **même place** que le refus « malformée » (`index==0`, hors du bloc `if (len >= 1)`, avant `g_uploadOwner = request`), `markUploadRejected(request)` puis `failOtaUpload(request, 400, kOtaSha256MissingMessage, true)`. **`markUploadRejected` est non négociable** : sans lui, `Update.hasError()` resterait faux et le lambda de réponse servirait « OK » avec un **redémarrage parasite**, alors que l'upload a été refusé — défaut qualifié le plus grave de ce lot par `security-reviewer`. Garde structurelle dédiée : [`test/test_native_ota_order_guard/`](../../test/test_native_ota_order_guard/) (tests `G3`) vérifie les trois propriétés (avant `g_uploadOwner`, hors du bloc `len >= 1`, `markUploadRejected` bien appelé dans la branche).

**Message de refus à DEUX causes** (`kOtaSha256MissingMessage`, [`src/constants.h`](../../src/constants.h)) — trouvaille de l'étape d'observation on-target du 2026-10-11, 09:33:17 : le premier envoi réel depuis un navigateur a été refusé « absente » parce que la page était **en cache, chargée avant le déploiement** du nouveau `data/app.js` (pas parce que l'ordre des champs était fautif). Le message cite donc **en premier** la cause qu'un utilisateur non technique rencontrera (« si l'envoi vient de la page web, elle est obsolète — rechargez-la ») et **en second** la cause scriptée (ordre des champs multipart). 317 octets UTF-8, mesurés et **non reformulables sans revalider** la mesure (marge sous `kLogMessageMaxBytes = 768`).

**Format de journal** : `[feature-118 lot B] Empreinte upload OTA : <verdict> — refusée : oui/non` (le suffixe « observation, non bloquant » du lot A disparaît — la décision est réelle). `WARNING` si refusée (malformée, absente, non conforme), `INFO` sinon (conforme).

**`ota_update.sh` devient fail-closed** : `compute_sha256()` échoue (ni `sha256sum` ni `shasum`) → le script **refuse d'envoyer lui-même**, `return 1` **avant** l'appel `curl` réel. Le repli du lot A (« envoi sans empreinte, avertissement console ») a disparu : un appareil en lot B refuserait cet envoi de toute façon ; échouer tôt évite une tentative réseau pour rien. **Prouvé par exécution** (`security-reviewer`, T4) : `PATH` privé des deux outils de hachage, `curl` remplacé par un piège qui se serait marqué s'il avait été appelé — **jamais appelé**, code de sortie 1. Garde structurelle anti-régression : [`test/test_native_ota_update_fail_closed/`](../../test/test_native_ota_update_fail_closed/) (lecture du texte bash, vérifie que l'échec de `compute_sha256()` précède bien l'appel `curl`, et qu'aucune troisième branche de repli ne produit un digest bidon).

### ⚠️ Parade cache RETIRÉE — correction d'une erreur de vérification

Une version intermédiaire de ce lot remplaçait, sur les 7 routes HTML, `req->send(LittleFS, …)` par un `beginResponse` + `.addHeader("Cache-Control", "no-cache")` manuel, pour répondre à l'incident du 2026-10-11 (empreinte « absente » rencontrée en pratique). **C'était redondant ET régressif**, établi par `security-reviewer` P2 :

- `AsyncFileResponse` pose **déjà** `ETag` + `Cache-Control: no-cache` (`WebResponses.cpp:744-745`).
- `req->send(FS, …)` teste **déjà** `If-None-Match` sur le repli gzip et peut répondre `304` (`AsyncWebServerRequest.cpp:26+`) — propriété que la version manuelle **perdait**.

La vérification initiale de cette parade avait elle-même été faite à tort (grep sur la chaîne littérale `"Cache-Control"`, alors que la bibliothèque la pose via la constante `T_Cache_Control` — le symbole, pas le texte). **Revenu à `req->send(LittleFS, …)`** sur les 7 routes HTML. Seul changement conservé : `server->serveStatic("/", LittleFS, "/")` pose désormais **explicitement** `.setCacheControl("no-cache")` ([`src/web_server.cpp`](../../src/web_server.cpp)) plutôt que de compter sur le défaut de la bibliothèque (qui le pose déjà en l'absence d'appel à cette méthode) — c'est le seul chemin du firmware où un `304` est possible, documenté pour qu'un futur lecteur ne le retire pas par erreur de lisibilité. Garde structurelle : [`test/test_native_ota_order_guard/`](../../test/test_native_ota_order_guard/) (test `G4`).

⛔ **La cause réelle de l'incident du 2026-10-11 n'était PAS un défaut d'en-tête HTTP, mais un onglet déjà ouvert avant le déploiement.** Aucun en-tête ne rattrape ce cas — la page était déjà chargée en mémoire du navigateur. Le remède est le **rechargement forcé** ; le filet de sécurité est le **message** (ci-dessus), pas une parade cache.

**Zéro régression** : `pio test -e native` — **66 suites / 2 156 cas, tous verts** (mesuré au 2026-10-11, ne pas recopier ce chiffre aux cycles suivants — ADR-0073). `pio run -e esp32dev_core3` : **SUCCESS**, aucun nouveau warning. 4 mutations tuées sur le lot B, dont le retrait de `markUploadRejected` (qui ferait répondre « OK » **et** planifier un redémarrage parasite).

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

Il apporte l'**autorisation** de l'appelant, pas l'**authenticité** de l'image. La comparaison SHA-256 de l'upload manuel est désormais **obligatoire** (lot B, v2.67.0) — mais une empreinte **fournie par l'appelant** prouve l'intégrité du **transfert**, jamais l'authenticité de l'image, obligatoire ou non : elle ne dit rien de qui l'a produite. La règle de sécurité n° 6 de `CLAUDE.md` (« vérifier la signature/CRC avant flash ») n'est donc **toujours pas** satisfaite sur ce chemin : traité par [`feature-118`](../../specs/features/done/feature-118-authenticite-de-l-image-ota.md) (lots A et B livrés), la signature reste ouverte dans [feature-151](../../specs/features/todo/feature-151-signature-de-l-image-ota.md).

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
- [`src/ota_integrity.h`](../../src/ota_integrity.h) / [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) — vérification d'intégrité SHA-256 (`parseOtaSha256Param`, `otaUploadIntegrityVerdict`, `otaUploadIntegrityIsRejected` — feature-118 lots A/B, 22+13 tests natifs `test/test_native_ota_integrity/` + `test/test_native_ota_sha256_param/`)
- [`test/test_native_ota_order_guard/`](../../test/test_native_ota_order_guard/) — garde structurelle (ordre du contrôle sha256 avant prise de propriété, refus « absente » hors du bloc `len >= 1` et `markUploadRejected` présent, `.setCacheControl` chaîné sur `serveStatic`, ordre des champs du `FormData`)
- [`test/test_native_ota_update_fail_closed/`](../../test/test_native_ota_update_fail_closed/) — garde structurelle du fail-closed de `ota_update.sh` (échec de hachage avant l'appel `curl`)
- [`test/sha256_vectors.js`](../../test/sha256_vectors.js) / [`tools/check_sha256_vectors.sh`](../../tools/check_sha256_vectors.sh) — vecteurs du SHA-256 JS extrait de `data/app.js`
- [`src/ota_type_logic.h`](../../src/ota_type_logic.h) — logique pure du type d'image (`inferOtaKindFromFilename`, `checkOtaFirstByte` — 29 tests natifs `test/test_native_ota_type/`) ; miroir JS `detectUpdateType()` dans [`data/app.js`](../../data/app.js)
- [`src/ota_check_logic.h`](../../src/ota_check_logic.h) — logique pure `/check-update` (`AllocBudget`, `utf8SafeCutLen`, `classifyOtaCheckFailure` depuis bug-165 — 20 tests natifs `test/test_native_ota_check/`, à mesurer plutôt qu'à recopier, ADR-0073)
- [`src/github_root_ca.h`](../../src/github_root_ca.h) — **5 racines auto-signées** (3 depuis v2.16.2, +2 Sectigo E46/R46 depuis bug-165, 2026-10-14) : USERTrust RSA + USERTrust ECC (github.com / api.github.com, exp. 2038), ISRG Root X1 (objects.githubusercontent.com, exp. 2035), Sectigo Public Server Authentication Root E46 (ECC, exp. 2046) + Root R46 (RSA, exp. 2046) — chaîne ECC/RSA actuelle d'api.github.com — empreintes SHA-256 vérifiées, sources consignées dans le header, conformité imposée en CI par [`tools/check_github_root_ca.sh`](../../tools/check_github_root_ca.sh) (voir [règle dédiée](#règle--une-ancre-de-confiance-est-toujours-la-variante-auto-signée-bug-checkupdate-tls-core3-bug-165-adr-0111) ci-dessus)
- [`partitions.csv`](../../partitions.csv)
- [docs/UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- [ADR-0007](../adr/0007-table-partitions-custom.md), [ADR-0022](../adr/0022-verification-integrite-ota.md), [ADR-0096](../adr/0096-empreinte-sha256-obligatoire-sur-post-update.md)
- [`specs/features/done/feature-118-authenticite-de-l-image-ota.md`](../../specs/features/done/feature-118-authenticite-de-l-image-ota.md), [`specs/features/todo/feature-151-signature-de-l-image-ota.md`](../../specs/features/todo/feature-151-signature-de-l-image-ota.md)
