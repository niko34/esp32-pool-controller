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

   > ⚠️ **Limitation connue — core Arduino 3 uniquement** ([ADR-0034, note additive 2026-08-08](../adr/0034-migration-core3-idf5.md)) : sous core 3 / IDF 5 sur ESP32 classique (sans PSRAM), le plus gros bloc heap contigu (~43-47 Ko dès le boot) ne suffit pas à la session TLS sortante (~50 Ko en pointe) → `/check-update` et `/download-update` (GitHub) échouent systématiquement en `500` propre, sans crash — les erreurs mbedtls loggées (`X509 alloc failed`, « verify failed ») masquent un échec d'allocation. Limitation **acceptée** ; contournement : upload manuel `/update` ; levier futur : WROVER-E + PSRAM (prérequis GPIO 16/17, feature-065). La production core 2 n'est pas concernée.
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
| Upload manuel (`/update`) | Empreinte **loggée**, comparaison **optionnelle** | SHA-256 du flux toujours calculé et loggé (`info`). Paramètre POST `sha256` optionnel (avec ou sans préfixe `sha256:`) : mismatch ou valeur malformée → `Update.abort()`, réponse `FAIL`, log `CRITICAL`. Sans paramètre : flash autorisé (utilisateur physiquement présent — fail-open documenté). |
| ArduinoOTA / espota | Déjà protégé | Le protocole espota transmet le MD5 de l'image, vérifié par la stack `Update`. Non modifié. |

### Verrou anti-upload concurrent (`/update`)

Le handler d'upload est appelé par chunks et son état vit dans des statiques de fichier (`g_uploadHasher`, `g_uploadLastLog`, empreinte attendue…), réinitialisées à `index == 0`. Un second upload lancé pendant qu'un premier est en cours corromprait cet état :

- **`g_uploadOwner`** : une seule requête propriétaire de la session. Une requête concurrente pendant que `Update.isRunning()` est rejetée sans toucher aux statiques → réponse **`409`** (« Upload refusé: une mise à jour OTA est déjà en cours »), marqueur par requête via `_tempObject` (libéré par le destructeur — pas de fuite).
- **`onDisconnect`** : client déconnecté en plein transfert → `Update.abort()`, `PumpController.setOtaInProgress(false)` (réarmement du dosage), verrou libéré.
- **Owner périmé** : si `!Update.isRunning()`, une nouvelle requête reprend la main (ceinture-bretelles).
- **Fix AC5** : le throttle de progression (`g_uploadLastLog`, ex-`static lastLog`) est réinitialisé à `index == 0` — deux uploads successifs sans reboot affichent une progression correcte.

Limite cosmétique connue : après reprise d'un owner périmé, l'ancien client peut recevoir un « OK » trompeur (consigné en spec, sans impact sécurité).

### Appariement `setOtaInProgress`

Chaque `PumpController.setOtaInProgress(true)` a son `false` sur **tous** les chemins terminaux : succès, échec `Update.begin()` (fuite corrigée en revue), mismatch d'intégrité (firmware et FS), digest invalide, déconnexion client. Sans cela, les pompes doseuses resteraient inhibées jusqu'au reboot.

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
- **Partition pleine** : refus au moment du `Update.begin()` → erreur HTTP 500, log CRITICAL.
- **Version GitHub < locale** : pas de mise à jour proposée côté UI (comparaison semver).

## Fichiers liés

- [`src/ota_manager.h`](../../src/ota_manager.h), [`src/ota_manager.cpp`](../../src/ota_manager.cpp)
- [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp) — routes `/update`, `/check-update`, `/download-update`
- [`src/ota_integrity.h`](../../src/ota_integrity.h) / [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) — vérification d'intégrité SHA-256
- [`src/ota_check_logic.h`](../../src/ota_check_logic.h) — logique pure `/check-update` (`AllocBudget`, `utf8SafeCutLen` — 13 tests natifs `test/test_native_ota_check/`)
- [`src/github_root_ca.h`](../../src/github_root_ca.h) — 3 racines **auto-signées** (v2.16.2 : les variantes USERTrust cross-signées AAA sont retirées) : USERTrust RSA + USERTrust ECC (github.com / api.github.com, exp. 2038), ISRG Root X1 (objects.githubusercontent.com, exp. 2035) — empreintes SHA-256 vérifiées, sources consignées dans le header
- [`partitions.csv`](../../partitions.csv)
- [docs/UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- [ADR-0007](../adr/0007-table-partitions-custom.md), [ADR-0022](../adr/0022-verification-integrite-ota.md)
