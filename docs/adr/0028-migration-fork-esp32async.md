# ADR-0028 — Migration vers le fork maintenu ESP32Async (AsyncTCP / ESPAsyncWebServer) avec versions épinglées

- **Statut** : Accepté
- **Date** : 2026-07-14
- **Décideurs** : Nicolas Philippe (architect), debugger (analyse coredump)
- **Spec(s) liée(s)** : aucune (correctif de stabilité déclenché par coredump)

## Contexte

Un coredump ESP32 a révélé un **crash récurrent** de la pile web asynchrone : `panic → reboot`, appareil injoignable en HTTP/WebSocket jusqu'au redémarrage. Le backtrace pointe vers un **assert lwIP** dans `tcp_update_rcv_ann_wnd` (`new_rcv_ann_wnd <= 0xffff`, `lwip/src/core/tcp.c`), déclenché sur le timeout/close d'un client WebSocket :

```
AsyncWebSocketClient::_onTimeout
  → AsyncClient::close()
    → _tcp_recved(pcb, len corrompu ≈ 3,5 milliards)
      → tcp_recved (lwIP)
        → assert new_rcv_ann_wnd <= 0xffff  → panic
```

La longueur passée à `tcp_recved` est corrompue (valeur ≈ 3,5 milliards, sous-flux d'un compteur non signé) : c'est un **bug de course connu** des bibliothèques **originales** `me-no-dev` (`AsyncTCP`, `ESPAsyncWebServer`), aujourd'hui **non maintenues**. Le projet les consommait via des dépôts Git **non épinglés** (`github.com/me-no-dev/AsyncTCP.git` / `.../ESPAsyncWebServer.git`, HEAD non figé), ce qui exposait à la fois au bug et à une dérive silencieuse des dépendances entre deux builds.

C'est la **2ᵉ cause de crash de la pile web** identifiée par coredump. La 1ʳᵉ (`std::bad_alloc` sur `/logs` sous fragmentation du tas) a été corrigée en 2.8.2. Les deux venaient de la **pile web asynchrone**, ce qui rend le durcissement de cette pile prioritaire.

## Décision

**Migrer vers le fork maintenu `ESP32Async` (ex-`mathieucarbou`) et épingler les versions** dans `platformio.ini` :

| Dépendance | Avant | Après |
|---|---|---|
| AsyncTCP | `github.com/me-no-dev/AsyncTCP.git` (HEAD non figé) | `ESP32Async/AsyncTCP@3.4.10` |
| ESPAsyncWebServer | `github.com/me-no-dev/ESPAsyncWebServer.git` (HEAD non figé) | `ESP32Async/ESPAsyncWebServer@3.11.2` |
| ESPAsyncWiFiManager | `alanswx/ESPAsyncWiFiManager` | **conservé** (compile tel quel contre le fork) |

Le fork ESP32Async corrige le bug de course de `close()`/`_onTimeout` à l'origine de l'assert lwIP. La migration est **sans changement de code applicatif** : le fork est rétrocompatible avec l'API utilisée par le projet (`AsyncWebServer`, `AsyncWebSocket`, `AsyncWebServerRequest`, réponses flux). Seuls `platformio.ini` et `src/version.h` (→ 2.9.0) sont modifiés.

La plateforme reste **figée** : `espressif32@6.9.0` (arduino-esp32 2.0.x / IDF 4.4). La compatibilité 2.x est confirmée par les README des forks et par le `library.json` d'`ESPAsyncWebServer@3.11.2`, qui exige `AsyncTCP ^3.4.10`. L'arbre de dépendances a été vérifié : **un seul** `AsyncTCP` et **un seul** `ESPAsyncWebServer` (tous deux ESP32Async), aucun duplicata `me-no-dev` transitif. Build vert (Flash 80,9 %), 223/223 tests natifs OK.

## Alternatives considérées

- **Option A (rejetée) — Rester sur `me-no-dev` + patch local.** Corriger le sous-flux dans une copie locale de la lib. Fragile : patch à re-appliquer à chaque résolution de dépendance, divergence non tracée, upstream mort donc aucun retour amont. Ne résout pas la dérive du HEAD non épinglé.
- **Option B (rejetée) — Épingler `me-no-dev` à un commit précis.** Rend le build reproductible mais **ne corrige pas le bug** : l'upstream n'a jamais intégré le fix. On fige un crash connu.
- **Option C (rejetée) — Changer entièrement de pile web** (serveur HTTP synchrone IDF, ou autre stack). Coût disproportionné : réécriture du WebSocket, des routes HTTP, de l'OTA et de l'auth, pour un problème isolable à une lib.
- **Option D (retenue) — Migrer vers le fork maintenu ESP32Async, versions épinglées.** Corrige le bug de course à la source, build reproductible (versions figées), aucun changement de code applicatif, plateforme inchangée. Le fork est activement maintenu, ce qui rétablit une voie de remontée amont.

## Conséquences

### Positives

- **Crash WS résolu** : plus de `panic → reboot` sur timeout/close d'un client WebSocket ; l'appareil ne devient plus injoignable pour cette cause.
- **Build reproductible** : versions épinglées (`@3.4.10` / `@3.11.2`) au lieu d'un HEAD Git mouvant → deux builds identiques donnent le même binaire côté pile web.
- **Aucune régression fonctionnelle** : API rétrocompatible, aucun code applicatif touché, tests natifs et compilation verts.
- **Pile web enfin maintenue** : les deux crashes web (bad_alloc `/logs` en 2.8.2, assert lwIP WS ici) sont désormais traités ; la voie amont ESP32Async est vivante.

### Négatives / dette assumée

- **Suivre les releases ESP32Async** : les versions étant épinglées, un correctif ou un CVE futur du fork n'arrive pas automatiquement — il faut surveiller les releases et re-tester avant de monter de version.
- **Contrainte de compatibilité core 2.0.x** : le couple retenu (`3.4.10` / `3.11.2`) est validé contre arduino-esp32 2.0.x / IDF 4.4. Une future migration du core (3.x / IDF 5.x) exigera de revalider ou faire évoluer ce couple de versions.
- **`ESPAsyncWiFiManager` inerte à surveiller** : la dépendance est conservée et compile contre le fork, mais le WiFi/portail est géré nativement dans le projet. Elle reste un point de fragilité potentiel (non maintenue elle non plus) à retirer ou remplacer si elle casse contre une future version des forks.

### Ce que ça verrouille

- La source des libs asynchrones est **ESP32Async** (et non plus `me-no-dev`) : toute réintroduction d'une dépendance `me-no-dev` transitive est interdite (risque de duplicata et de retour du bug).
- Les versions `AsyncTCP@3.4.10` et `ESPAsyncWebServer@3.11.2` sont **épinglées** : toute montée de version passe par une revue + revalidation (build + tests natifs + vérification de l'arbre de deps : un seul AsyncTCP, un seul ESPAsyncWebServer).
- La plateforme `espressif32@6.9.0` (core 2.0.x / IDF 4.4) reste le socle validé pour ce couple de versions.

## Références

- Code : [`platformio.ini`](../../platformio.ini) (`lib_deps`), [`src/version.h`](../../src/version.h) (2.9.0)
- Diagnostic : coredump ESP32 — backtrace `AsyncWebSocketClient::_onTimeout` → `AsyncClient::close()` → `_tcp_recved` → `tcp_recved` → assert lwIP `tcp_update_rcv_ann_wnd`
- Docs : [subsystems/ws-manager.md](../subsystems/ws-manager.md), [BUILD.md](../BUILD.md)
- Antécédent : CHANGELOG `[2.8.2]` (1ʳᵉ cause de crash web — `std::bad_alloc` sur `/logs`)
