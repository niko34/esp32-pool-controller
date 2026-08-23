# ADR-0045 — État d'impulsion exposé en calendrier (période / pouls / phase), pas en booléen instantané

- **Statut** : Accepté
- **Date** : 2026-08-23
- **Décideurs** : architect, utilisateur (revue itération 3) — visa `pool-chemistry` GO (lecture seule)
- **Spec(s) liée(s)** : [bug-dosage-fantome-duty-non-calibre](../../specs/features/doing/bug-dosage-fantome-duty-non-calibre.md) (itération 3, défaut n°3, v2.21.1)

## Contexte

Depuis le dosage en proportionnel de temps ([ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)),
une session de dosage alterne pouls (pompe ON, 5-60 s) et repos, sur une période fixe
de 60 s (`kPulsePeriodMs`). Les champs existants `ph/orp_dosing` reflètent la **session**
(l'intention commandée), jamais la sortie réelle : l'UI affichait « Injection en cours »
pendant les repos — exactement la conflation « commandé vs effectif » qui a masqué le
dosage fantôme d'origine (défaut n°3 de la spec).

Il faut donc exposer à l'UI « la pompe tourne-t-elle **maintenant**, et sinon quand
tournera-t-elle ? ». Contrainte de transport : le push WS est cadencé à **5 s** et
`GET /data` est le repli polling à **30 s**.

## Décision

Le firmware expose le **calendrier d'impulsion**, pas un instantané : 7 champs
identiques dans `GET /data` et le push WS `sensor_data` — `pulse_period_ms` (période du
train), `ph/orp_pulse_session` (séance ouverte, auto OU scheduled), `ph/orp_pulse_ms`
(durée d'impulsion planifiée, `0` = deadband) et `ph/orp_pulse_phase_ms` (position
**relative** dans la période, clampée `[0..période]` — jamais un `millis()` absolu).

Le client **ancre** la phase reçue sur sa propre horloge (`dosingAnchor`, app.js) et
anime localement : « Injection en cours » tant que `phase < pulse_ms`, compte à rebours
« Prochaine impulsion : Ns » sinon, resynchronisé à chaque trame. À l'échéance sans
nouvelle donnée → ellipsis (« Prochaine impulsion… »), **jamais de bascule optimiste**
vers un état « pompe qui tourne » ; la seule bascule locale autorisée est **restrictive**
(impulsion → attente quand la phase ancrée dépasse `pulse_ms`).

Les 3 getters firmware (`isPulseSessionActive`, `getPulseMs`, `getPulsePhaseMs`) sont
**lecture seule**, sans setter, avec gardes d'index fail-closed — aucun chemin de dosage
touché.

## Alternatives considérées

- **Booléen instantané « pompe alimentée » poussé en WS** (rejetée) — aliasing garanti :
  à 5 s de push pour des impulsions de 5-60 s, un pouls de 5 s peut être entièrement
  invisible ou paraître durer 10 s ; l'UI mentirait autrement, pas moins. Pousser à
  chaque front ON/OFF aurait multiplié les trames (jusqu'à 4 pushes/min/chimie) pour un
  résultat encore dépendant de la latence réseau.
- **`GET /data` seul, sans champs WS** (rejetée) — repli polling à 30 s : jusqu'à une
  demi-période de retard sur un train de 60 s, autant dire aucune information temps réel.
- **Calendrier + animation client resynchronisée** (retenue) — la géométrie du pouls
  (période, durée, phase relative) est stable entre deux trames ; le client peut animer
  à la seconde près sans nouvelle donnée, et chaque trame corrige la dérive (±1-2 s
  d'honnêteté affichée, assumés).

## Conséquences

### Positives
- L'UI distingue enfin « séance de dosage ouverte » (commandé) de « pompe alimentée »
  (effectif) — clôt le défaut n°3 de la spec.
- Compte à rebours fluide côté client sans augmenter la cadence de push ni la charge
  firmware (7 lectures atomiques 8/32 bits par trame).
- Phase **relative** = aucun couplage d'horloges client/firmware, wrap-safe au rollover
  de `millis()`.

### Négatives / dette assumée
- Précision d'affichage ±1-2 s (jitter de push + arrondi seconde) : acceptable pour un
  état informatif, documenté par l'ellipsis à l'échéance.
- Course bénigne assumée sur les getters (écriture loopTask / lecture async_tcp, patron
  `isAnyPumpEnergized`) : pire cas, un instantané en retard d'un tour de loop.
- **Agrandissement contractuel du buffer WS** : `kWsFrameBufBytes` 3072 → **3584** o
  (`.bss`, trame mesurée ~2,2 Ko) et pool `_buildSensorJson` 2176 → 2432 o ; `/data`
  1536 → 1792 o. +512 o de RAM statique assumés.

### Ce que ça verrouille
- Les 7 noms/types/sémantiques sont un **contrat** identique WS/`/data` (rétrocompat :
  champs absents → comportement historique côté client). Tout futur changement de la
  géométrie du train (période variable, multi-pouls) devra rester exprimable dans ce
  calendrier ou passer par un nouvel ADR.
- `ph/orp_dosing` conservent leur sémantique **session** — on ne les requalifie pas en
  « pompe alimentée ».

## Références

- Code : `src/pump_controller.{h,cpp}` (getters lecture seule), `src/ws_manager.cpp`
  (`_buildSensorJson`), `src/web_routes_data.cpp`, `data/app.js` (`dosingAnchor`,
  `updateDosingStatus`), `src/constants.h` (`kWsFrameBufBytes`, `kPulsePeriodMs`)
- Spec : `specs/features/doing/bug-dosage-fantome-duty-non-calibre.md` (itération 3)
- Doc : [ws-manager.md](../subsystems/ws-manager.md), [API.md](../API.md#get-data--write),
  [page-dashboard.md](../features/page-dashboard.md), [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md),
  [ADR-0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)
