# ADR-0025 — Mode Boost : surcouche temporaire « valeurs effectives » + relèvement borné de la limite chlore

- **Statut** : Accepté
- **Date** : 2026-07-06
- **Décideurs** : Nicolas Philippe (architect), pool-chemistry (GO pré + post)
- **Spec(s) liée(s)** : feature-053 (mode-boost)

## Contexte

Après une **forte fréquentation** (baignade nombreuse, orage, canicule), l'utilisateur veut assainir davantage la piscine **pour la journée** : augmenter le turnover (filtration) et le résidu chloré (cible ORP), sans toucher durablement à sa configuration ni relâcher les garde-fous de sécurité chimique.

Le problème structurant : donner un **effet chlore réel** suppose de relever la **limite journalière de dosage** — or cette limite est un **garde-fou de sécurité** (règle absolue du projet : « limites journalières/horaires toujours respectées »). Relever une limite de sécurité, même temporairement, est une décision qui contraint la régulation, qui n'est pas lisible dans le code sans intention explicite, et qui a plusieurs implémentations crédibles (mutation de config, durée fixe, switch mode…). Elle doit être formalisée et bornée.

## Décision

**Le Mode Boost est une surcouche temporaire NON destructive, activable d'un geste, auto-expirant au prochain minuit local.** Il n'écrit jamais dans la configuration persistée (cibles, limites, modes) : il expose des **valeurs *effectives*** consommées par la régulation et la filtration. À l'expiration, retour automatique au comportement normal — rien à « remettre ».

Tant que `isBoostActive()` est vrai :

1. **Filtration forcée en marche** via un chemin de forçage **dédié** (`boostForce`), prioritaire dans `decideFiltrationRun`, **indépendant** du `forceOn` utilisateur et de son `kForceTimeoutMs`. Dérivé de `isBoostActive()` → **ré-appliqué automatiquement au boot** sans dépendre d'un timer.
2. **Cible ORP effective relevée** : `effectiveOrpTarget = min(orpTarget + kBoostOrpDeltaMv, kBoostOrpCeilingMv)`. La régulation ORP **automatique** existante injecte naturellement vers cette cible — aucun nouveau chemin d'injection. Jamais abaissée.
3. **Limite journalière chlore effective relevée** : `effectiveMaxChlorine = min(maxChlorineMlPerDay × kBoostDailyFactor, kBoostDailyHardCapMl)`.

**Effet chlore STRICTEMENT gaté au mode ORP `automatic`** : les fonctions pures `effectiveOrpTargetPure` / `effectiveMaxChlorinePure` ne relèvent cible et limite que si le mode ORP est `automatic`. En mode **Manuel** ou **Programmé**, le Boost n'étend **que la filtration** ; l'injection manuelle reste bornée à la limite **normale**. **Tous les autres garde-fous restent inchangés** : limite horaire, temps min d'injection, anti-rafale, injection uniquement si filtration active, watchdog.

### Bornes de sécurité figées par pool-chemistry (4 constantes)

| Constante | Valeur | Justification |
|-----------|--------|---------------|
| `kBoostOrpDeltaMv` | `+60 mV` | Relèvement de cible sensible mais mesuré, cohérent avec l'écart usuel entre un ORP de confort (~700 mV) et un ORP de désinfection renforcée. |
| `kBoostOrpCeilingMv` | `850 mV` | Plafond dur, maintenu **sous** le seuil d'alerte `orp_abnormal` (> 900 mV) : le Boost ne peut jamais pousser la cible dans la zone considérée anormale. |
| `kBoostDailyFactor` | `1.5×` | Marge d'injection réelle (+50 %) sans ouvrir en grand : le turnover et la cible relevée font l'essentiel du travail, la limite ne fait que ne pas brider prématurément. |
| `kBoostDailyHardCapMl` | `1000 mL` | Plafond **absolu** en dur : même avec une limite normale déjà élevée, le boost ne peut jamais dépasser 1000 mL/jour de chlore. Garde-fou terminal. |

### 5 conditions posées par pool-chemistry (pré) et couvertes

1. Relèvement de limite **borné en dur** (`kBoostDailyHardCapMl`) ET **auto-expirant** (jamais à cheval sur deux fenêtres de compteur journalier, car budget = 1 jour calendaire).
2. Cible effective **plafonnée sous l'alerte** `orp_abnormal` (`kBoostOrpCeilingMv = 850 < 900`), jamais abaissée.
3. Effet chlore **uniquement en mode `automatic`** (gate des fonctions pures) ; Manuel/Programmé = filtration seule ; injection manuelle bornée à la limite normale.
4. **Interactions inchangées** avec anti-rafale, temps min d'injection et garde « filtration active » : le Boost n'ouvre aucun chemin d'injection, il ne fait que déplacer cible et plafond journalier consommés par la régulation auto existante.
5. **Refus d'activation sans heure synchronisée** : sans horloge valide, l'expiration à minuit ne peut être calculée → activation refusée (`409`), pas de boost « sans fin ».

## Alternatives considérées

- **Option A (rejetée) — Muter temporairement la config persistée** (écrire `orpTarget += 60`, `maxChlorine ×1.5`, restaurer à l'expiration).
  Fragile : un reboot ou un crash pendant le boost laisserait la config **modifiée en dur** (limite de sécurité relevée durablement, sans expiration). Nécessiterait de persister aussi les valeurs d'origine pour les restaurer. La surcouche « valeurs effectives » est **sans état de restauration** : à l'expiration, on lit de nouveau la config d'origine, intacte.
- **Option B (rejetée) — Relèvement non borné de la limite journalière** (facteur seul, sans plafond dur).
  Violerait la règle de sécurité : une limite normale déjà haute × 1,5 pourrait autoriser un volume de chlore excessif. Le plafond dur `kBoostDailyHardCapMl` est non négociable.
- **Option C (rejetée) — Durée fixe en heures** (ex. boost de 4 h / 8 h).
  Un boost à durée fixe peut chevaucher **deux fenêtres de compteur journalier** (minuit), rendant le budget de sécurité difficile à raisonner (2× le quota boosté possible). « Jusqu'au prochain minuit » borne trivialement le budget à **un seul** jour calendaire. Edge assumé : activé tard le soir = boost court cette nuit (bénéfice = filtration nocturne), ré-activable le lendemain.
- **Option D (rejetée) — Un simple `switch` mode de régulation dédié « boost »**.
  Aurait dupliqué toute la logique de régulation ORP dans un 4ᵉ mode, et perdu la propriété « non destructif » (le mode courant serait écrasé). La surcouche laisse le mode de régulation **inchangé** et se contente de déplacer cible + plafond.
- **Option E (retenue) — Surcouche « valeurs effectives » non destructive + relèvement borné auto-expirant.**
  Aucun état de restauration, robuste au reboot (état `boostState` persisté, expiration dérivée de l'horloge), garde-fous terminaux en dur, effet gaté au mode automatic.

## Conséquences

### Positives

- **Non destructif** : la config utilisateur (cibles, limites, modes) n'est jamais modifiée ; aucune restauration à orchestrer.
- **Robuste au reboot** : `boostState` (`active` + `untilEpoch`) persisté en NVS (namespace `poolctrl`) ; au boot, si `now >= untilEpoch` le boost est inactif, sinon il reprend, filtration forcée ré-appliquée (dérivée de `isBoostActive()`).
- **Budget de sécurité trivialement borné** : 1 jour calendaire, plafonné en dur à `kBoostDailyHardCapMl`, cible sous l'alerte ORP.
- **Pilotable UI + Home Assistant** d'un geste ; expiration affichée ; rappel « vérifier le taux de chlore avant baignade ».

### Négatives / dette assumée

- **Edge « activé tard le soir » = boost court** : le boost expirant au prochain minuit, une activation à 23 h ne dure qu'une heure. Assumé (bénéfice = filtration nocturne), ré-activable le lendemain. Une durée configurable est un hors-périmètre explicite de la v1.
- **Nécessite une heure synchronisée** : sans NTP/RTC valide, l'activation est refusée (`409 time_not_synced`). Le boost n'est pas disponible tant que l'horloge n'est pas synchronisée.
- **L'utilisateur reste responsable de la vérification chimique** : le contrôleur ne mesure pas le chlore libre (ppm). Un rappel UI enjoint de **vérifier le taux avant baignade** ; la détection automatique de « niveau baignable » est hors périmètre.

### Ce que ça verrouille

- La limite journalière de dosage chlore peut être relevée **uniquement** par le Boost, **uniquement** bornée par `kBoostDailyHardCapMl`, **uniquement** de façon auto-expirante. Tout autre chemin de relèvement d'une limite de sécurité devrait faire l'objet d'un nouvel ADR + validation pool-chemistry.
- La sémantique « valeurs effectives » (getters `effectiveOrpTarget` / `effectiveMaxChlorine`, fonctions pures gatées `automatic`) devient le patron pour toute future surcouche temporaire non destructive.
- Le contrat public exposé (routes `POST /boost/start|stop`, champs WS `boost_active` / `boost_until`, switch HA « Boost » sur `{base}/boost` + `/set`) est verrouillé.

## Références

- Code : [`src/pump_controller.cpp`](../../src/pump_controller.cpp), [`src/pump_controller.h`](../../src/pump_controller.h), [`src/dosing_logic.h`](../../src/dosing_logic.h) (`effectiveOrpTargetPure` / `effectiveMaxChlorinePure`), [`src/filtration.cpp`](../../src/filtration.cpp) (`decideFiltrationRun` / `boostForce`), [`src/constants.h`](../../src/constants.h) (`kBoostOrpDeltaMv`, `kBoostOrpCeilingMv`, `kBoostDailyFactor`, `kBoostDailyHardCapMl`)
- Spec : `specs/features/doing/feature-053-mode-boost.md`
- Docs : [MQTT.md](../MQTT.md), [API.md](../API.md), [subsystems/pump-controller.md](../subsystems/pump-controller.md), [subsystems/ws-manager.md](../subsystems/ws-manager.md), [features/page-dashboard.md](../features/page-dashboard.md)
- ADR liés : [ADR-0008](0008-persistance-cumuls-journaliers-nvs.md) (reset journalier aligné minuit local, réutilisé par l'expiration), [ADR-0002](0002-mode-programmee-volume-quotidien.md) (volume quotidien), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable — patron des fonctions `effective*Pure`)
