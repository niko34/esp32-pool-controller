# ADR-0022 — Vérification d'intégrité OTA par SHA-256 comparé au digest de l'API GitHub

- **Statut** : Accepté
- **Date** : 2026-07-06
- **Décideurs** : architect + code-reviewer (Approved après 2 correctifs), feature-026
- **Spec(s) liée(s)** : feature-026 (vérification d'intégrité OTA)

## Contexte

La règle de sécurité absolue #6 du projet (« OTA : toujours vérifier la signature/CRC avant flash ») n'était **pas satisfaite** : l'OTA reposait uniquement sur la validation implicite de `Update.end(true)` (magic byte de l'image ESP32). Une image tronquée par une coupure réseau ou corrompue en transit pouvait être flashée — point le plus critique de la revue de code du 2026-06-20, avec risque de brique.

Contraintes :
- Trois chemins OTA distincts : téléchargement GitHub (`/download-update`), upload manuel (`/update`), push ArduinoOTA/espota ;
- Le flux **filesystem** GitHub est écrit via `esp_partition_write()`, **hors** de la classe `Update` — un mécanisme limité à `Update` ne le couvrirait pas ;
- Handler AsyncWebServer : calcul incrémental obligatoire (pas de passe finale bloquante > 50 ms) ;
- Offline first : pas de nouvelle dépendance cloud au-delà de la source de release déjà utilisée ;
- Budget RAM serré pendant l'OTA (TLS déjà actif).

Opportunité vérifiée pendant la conception : l'API GitHub `releases/latest` — déjà interrogée par `/check-update` — expose un champ **`digest`** (`"sha256:<64hex>"`) par asset. Sa présence a été **confirmée sur la release réelle v2.5.2** du projet.

## Décision

1. **SHA-256 incrémental** (mbedtls, accélération matérielle ESP32) via la classe `OtaStreamHasher` ([`src/ota_integrity.h`](../../src/ota_integrity.h)), alimentée chunk par chunk sur les trois flux (firmware GitHub, filesystem GitHub, upload manuel). Empreinte de référence : le champ `digest` des assets GitHub — **zéro changement du process de release**.
2. **Transit du digest par l'UI** : `/check-update` relaie les champs additifs `firmware_digest` / `filesystem_digest`, que `app.js` repasse en paramètre POST `digest=` à `/download-update`. Même modèle de menace que l'URL déjà relayée par le client : whitelist d'hôtes GitHub côté firmware + auth `CRITICAL` sur la route.
3. **Fail-closed strict sur le chemin GitHub** : digest absent → `400 integrity_digest_missing` (refus **avant** toute connexion) ; malformé → `400 integrity_digest_invalid` ; mismatch → firmware : `Update.abort()` avant tout `end()` (l'image n'est **jamais** activée), filesystem : `422` sans restart + remontage LittleFS best-effort (la partition FS est suspecte mais le firmware reste intact et bootable — re-tentative requise). L'UI désactive le bouton « Installer » si une release ne fournit pas de digest.
4. **Upload manuel `/update`** : empreinte SHA-256 **toujours calculée et loggée** (traçabilité), comparaison **optionnelle** via le paramètre POST `sha256` (l'utilisateur est physiquement présent — fail-open documenté, conforme AC2 « ou explicitement documenté ») ; paramètre fourni mais malformé ou mismatch → `Update.abort()`, flash refusé.
5. **ArduinoOTA/espota non modifié** : le protocole espota transmet déjà le MD5 de l'image, vérifié par la stack `Update`.
6. **Logique pure séparée** (`parseSha256Digest` / `sha256Equal` temps constant / `sha256ToHex`, [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h), pattern [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) — 22 tests natifs.

## Alternatives considérées

- **MD5 via `Update.setMD5()`** (rejetée) — mécanisme confiné à la classe `Update` : ne couvre **pas** le flux filesystem GitHub écrit via `esp_partition_write()`. Aurait exigé un second mécanisme pour le FS, et MD5 n'est pas le format exposé par l'API GitHub.
- **Re-fetch de l'API GitHub côté firmware au moment du download** (rejetée) — éviterait le transit par l'UI mais double le pic RAM (session TLS + `getString()` de la réponse API pendant que la session TLS du download se prépare) pour un gain de sécurité marginal (l'URL transite déjà par l'UI, whitelist + auth identiques).
- **Signature cryptographique asymétrique** (écartée, hors périmètre) — protège aussi contre une compromission de la source, mais exige une gestion de clés et un changement du process de release ; feature ultérieure possible, explicitement hors périmètre de la spec.
- **Relecture de la flash après écriture** (rejetée) — vérifierait l'écriture physique, mais `Update.end(true)` et `esp_partition_write` remontent déjà les erreurs d'écriture ; redondant avec le hachage du flux, coût en temps de flash.

## Conséquences

### Positives
- Règle de sécurité #6 satisfaite sur les trois chemins OTA : image tronquée (coupure WiFi incluse) ou corrompue → refus **avant activation**, version courante conservée, log `CRITICAL`, message français distinct côté UI.
- Zéro changement du process de release : le digest est généré par GitHub.
- Logique de parse/comparaison testable en natif (22 tests), comparaison en temps constant.

### Négatives / dette assumée
- **Anciennes releases sans champ `digest`** : installation refusée par l'UI (bouton désactivé) — fail-closed assumé, contournable par upload manuel.
- **Usure flash sur digest fourni mais faux** : l'image est intégralement téléchargée et **écrite** avant le refus (la comparaison ne peut se faire qu'en fin de flux) — un cycle d'écriture de partition perdu par tentative, sans danger (partition inactive / FS re-flashable).
- Après un mismatch **filesystem**, la partition FS contient une image suspecte : l'UI peut être indisponible jusqu'à un nouvel OTA FS réussi (le firmware, lui, reste bootable).

### Ce que ça verrouille
- Le chemin GitHub est **fail-closed** : aucun téléchargement sans digest valide — assouplir exigerait un nouvel ADR.
- Le format d'empreinte est `sha256:<64hex>` (format API GitHub, `kOtaSha256Prefix`) ; en changer casse la chaîne UI → firmware.
- Toute évolution du parse/comparaison passe par la logique pure `ota_integrity_logic` (ADR-0017).
- Le firmware sur mismatch n'active **jamais** l'image (`Update.abort()` avant `end()`) ; le FS sur mismatch ne redémarre **jamais** automatiquement.

## Références

- Code : [`src/ota_integrity.h`](../../src/ota_integrity.h) / [`src/ota_integrity.cpp`](../../src/ota_integrity.cpp) (`OtaStreamHasher`), [`src/ota_integrity_logic.h`](../../src/ota_integrity_logic.h) / [`src/ota_integrity_logic.cpp`](../../src/ota_integrity_logic.cpp), [`src/web_routes_ota.cpp`](../../src/web_routes_ota.cpp), [`src/constants.h`](../../src/constants.h) (`kOtaSha256HexLen`, `kOtaSha256Prefix`), [`data/app.js`](../../data/app.js) (transit du digest, messages d'erreur)
- Tests : `test/test_native_ota_integrity/` (22 tests natifs)
- Spec : `specs/features/doing/feature-026-verification-integrite-ota.md`
- Doc : [docs/subsystems/ota-manager.md](../subsystems/ota-manager.md), [docs/API.md](../API.md#mises-à-jour-ota), [docs/UPDATE_GUIDE.md](../UPDATE_GUIDE.md)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure)
