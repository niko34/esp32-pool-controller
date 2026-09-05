# Bug — `/check-update` échoue sous core 3 : « Erreur HTTP GitHub: -1 » (TLS)

- **Type** : bug (format minimal `specs/modele.md`)
- **Statut** : done — **cause identifiée, limitation documentée** (pas de fix
  logiciel possible sans libs custom) ; durcissements livrés en v2.16.2
- **Version cible** : 2.16.2
- **Créée** : 2026-08-08 (fenêtre d'observation core 3, appareil de production)
- **Domaine** : Web (`/check-update`, TLS sortant) — aucun chemin de dosage
- **Spec liée** : bug-checkupdate-heap-tls (v2.16.1), ADR-0034 (migration core 3)

## Symptôme

Sous core 3 (v2.16.1/2.16.2), « Vérifier les mises à jour » échoue TOUJOURS,
y compris sur heap fraîche après reboot — alors que le même firmware en core 2
réussissait (au moins sur heap fraîche). Erreurs successives selon l'état
mémoire : `X509 - Allocation of memory failed (-10368)`, `X509 - Certificate
verification failed (-9984)`, `SSL - Memory allocation failed (-32512)`.

## Investigation (méthode et preuves)

1. GitHub hors de cause : API 200 + quota OK depuis la même IP publique.
2. Certificats hors de cause : chaîne api.github.com (leaf ← Sectigo E36 ←
   Sectigo Root E46 cross-signé USERTrust ECC) validée contre les CA du
   firmware (a) par OpenSSL sur Mac, (b) par mbedtls 4.2 sur Mac, (c) PAR
   L'ESP32 LUI-MÊME via une route de diagnostic temporaire `/debug/tls-verify`
   rejouant `mbedtls_x509_crt_verify` hors connexion : rc=0, flags=0, deux
   passes — y compris avec maxAlloc 32 Ko. Crypto matérielle disculpée.
3. Config TLS précompilée du core 3 vérifiée : TLS 1.2 only, P-384/SHA-384
   activés, ECDHE_ECDSA activé, buffers 16 Ko symétriques, PAS de
   MBEDTLS_DYNAMIC_BUFFER ni ASYMMETRIC_CONTENT_LEN.
4. Instrumentation heap dans la route : plus gros bloc contigu **~43-47 Ko dès
   le boot** sous core 3 (bien moins que core 2). L'erreur MUTE avec l'état
   mémoire → cause unique : **allocations refusées pendant le handshake réel**
   (session ~50 Ko en pointe : 2×16 Ko de buffers + structures + X509).
   L'erreur « verify failed » est un déguisement : un échec d'allocation au
   milieu du calcul de signature est remonté comme signature invalide.

## Cause racine

**Empreinte statique d'IDF 5 sur ESP32 classique → plus gros bloc contigu
insuffisant pour une session TLS sortante.** Le remède upstream existe
(CONFIG_MBEDTLS_DYNAMIC_BUFFER) mais n'est activé que sur la branche master du
lib-builder (futur core majeur), pas dans les 3.3.x.

## Issue retenue (décision utilisateur 2026-08-08)

- **Limitation ACCEPTÉE sous core 3** : `/check-update` et « Installer depuis
  GitHub » (tout TLS sortant) inopérants — échec PROPRE (500 + log détaillé),
  aucun crash. Contournement : OTA par upload manuel des binaires de release
  (flux déjà utilisé).
- **Levier matériel planifié** : ESP32-DevKitC-VE (WROVER-E, +8 Mo PSRAM) — la
  config core 3 place déjà les allocations > 4 Ko en PSRAM
  (SPIRAM_USE_MALLOC=y, seuil 4096) → TLS restauré + oxygène pour tout le
  système. Prérequis : libérer GPIO 16/17 (pris par la PSRAM) → retrait du
  code écran UART (feature-065).

## Livré en v2.16.2 (durcissements conservés)

- `github_root_ca.h` : racines USERTrust RSA/ECC **auto-signées** (les
  variantes cross-signées AAA sont retirées — hygiène et pérennité, même si
  ce n'était pas la cause) ; empreintes sha256 vérifiées, sources consignées.
- `/check-update` : log de l'état heap (libre + plus gros bloc) à chaque
  vérification ; erreurs TLS détaillées (message mbedtls + code +
  errorToString HTTPClient) au lieu d'un « -1 » muet.
- Diagnostic temporaire (`/debug/tls-verify`, header de chaîne embarquée,
  CORE_DEBUG_LEVEL=3) : livré puis **RETIRÉ** — ne fait pas partie du lot.

## Critères d'acceptation

- [x] Cause racine démontrée (pas supposée) — cf. Investigation
- [x] Échec propre sans crash, message exploitable dans les logs
- [x] `pio run` + `esp32dev_core3` + 428 tests natifs verts
- [x] Limitation + levier WROVER documentés (ADR-0034)
