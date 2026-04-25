# ADR-0008 — Persistance NVS des cumuls journaliers + reset aligné minuit local

- **Statut** : Accepté
- **Date** : 2026-04-24 (CHANGELOG [Unreleased])
- **Doc(s) liée(s)** : [pump-controller.md](../subsystems/pump-controller.md), [page-ph.md](../features/page-ph.md), [page-orp.md](../features/page-orp.md)

## Contexte

Avant cette décision, les cumuls de produit chimique injecté sur la journée (`dailyPhInjectedMl`, `dailyOrpInjectedMl`) étaient **uniquement en RAM**. Deux failles :

1. **Un reboot remettait le cumul à zéro.** Un watchdog qui reset l'ESP32 en fin d'après-midi laissait tout l'après-midi disponible à re-consommer la limite journalière → risque de **double quota**, donc surdosage.
2. **Fenêtre « journalière » glissante sur 24 h**, pas alignée sur minuit local. Un utilisateur qui voyait « 280 mL sur 300 » à 22 h s'attendait à repartir à zéro à minuit. En pratique, le compteur ne redescendait qu'à 22 h le lendemain → comportement non intuitif, et incohérent avec la façon dont les docs / UI présentent la limite (« aujourd'hui »).

L'ORP et le pH sont tous deux concernés par ce risque, le pH encore plus critique (acide fort).

## Décision

Deux changements couplés :

1. **Persistance NVS** — les champs `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont sauvegardés dans le namespace NVS `pool-daily`, avec la date locale `currentDayDate` (format `YYYYMMDD`). Au boot :
   - si la date locale est identique à celle persistée → les cumuls sont **restaurés** ;
   - si elle a changé → les cumuls sont **remis à zéro** (nouveau jour).
2. **Reset aligné sur minuit local** — plutôt qu'une fenêtre glissante de 24 h, le reset du cumul journalier est déclenché au passage de minuit dans le fuseau configuré (`timezoneId`). Le timer de stabilisation (`armStabilizationTimer`) est armé au passage de minuit pour éviter un démarrage d'injection dans la foulée du reset (mitigation double-quota si le firmware démarre une injection juste avant/après minuit).

La structure `SafetyLimits` ([`config.h`](../../src/config.h)) contient désormais `char currentDayDate[9]` pour tracer la date du dernier reset.

## Alternatives considérées

- **Garder la fenêtre glissante** (rejeté) — simple mais contre-intuitif, et ne résout pas la perte au reboot.
- **Reset UTC au lieu de local** (rejeté) — un utilisateur en Europe verrait son cumul repartir à 1 h ou 2 h du matin selon saison, aucune utilité.
- **Persistance sur LittleFS au lieu de NVS** (rejeté) — NVS est plus résistante aux coupures (wear leveling intégré), écriture plus rapide, mieux adaptée à un compteur mis à jour périodiquement.
- **Persistance à chaque injection** (rejeté) — use wear excessive de la flash. Flush périodique + flush sur bord descendant d'injection suffit.

## Conséquences

### Positives
- Le compteur journalier survit aux reboots (watchdog, OTA, coupure).
- Le reset à minuit local est aligné sur la mental map de l'utilisateur.
- La limite journalière redevient une vraie barrière sécuritaire, pas une suggestion effaçable par un reboot.

### Négatives / dette assumée
- Wear NVS accru : le flush se fait à un tempo modéré (voir `_lastDailySaveMs` dans [`pump_controller.h`](../../src/pump_controller.h)) mais reste une écriture de plus.
- Un changement de `timezoneId` en cours de journée peut produire un reset « avancé » ou « rétrograde ». Comportement acceptable : changer de fuseau est un événement rare.
- Si l'heure n'est pas encore synchronisée au boot (NTP pas reçu, RTC absente), le chargement des cumuls est différé (`_dailyLoaded = false`) → tant que `time_t` < `kMinValidEpoch` (14 nov. 2023), les cumuls ne sont pas restaurés depuis NVS et restent à zéro — **risque faible mais documenté**.

### Ce que ça verrouille
- Le namespace NVS `pool-daily` est réservé à ces compteurs : ne pas y écrire d'autres clés pour limiter la surface d'incompatibilité future.
- `kMinValidEpoch` est utilisé comme garde pour détecter « l'heure est-elle valide ? ». Constante consolidée dans [`constants.h`](../../src/constants.h:111).

## Références

- Code : [`src/pump_controller.h`](../../src/pump_controller.h) membres statiques `_dailyLoaded`, `_dailyCountersDirty`, `_lastDailySaveMs`
- Code : [`src/config.h`](../../src/config.h) struct `SafetyLimits` champ `currentDayDate[9]`
- Code : [`src/config.cpp`](../../src/config.cpp) `saveDailyCounters()`, `loadDailyCounters()`
- Doc régulation : [pump-controller.md](../subsystems/pump-controller.md) — gardes de sécurité et cumul journalier
- CHANGELOG [Unreleased] 2026-04-24 entrée « Persistance compteurs journaliers » + « Reset journalier »
