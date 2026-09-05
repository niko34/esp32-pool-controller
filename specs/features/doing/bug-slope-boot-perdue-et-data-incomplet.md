# Bug — requête Slope de boot perdue en silence + champs pente absents de /data

- **Type** : bug (diagnostic sonde pH — feature-024)
- **Priorité** : basse (cosmétique/diagnostic — la mesure et la régulation ne sont PAS affectées)
- **Statut** : todo
- **Créée** : 2026-08-23, diagnostic on-target en v2.21.1
- **Domaine** : sensors (file EZO), web_routes_data, UI puce sonde pH

## Symptôme rapporté

Après le reboot OTA v2.21.1, la carte pH affiche « EZO indisponible » (premières
secondes, cache calibration = -1) puis « Pente non disponible » — pendant ~20 min,
y compris après refresh navigateur. La carte ORP n'affiche rien d'anormal.

## Diagnostic on-target (2026-08-23)

1. **La sonde et le module vont bien** : EZO pH détecté (fw 2.17), 2 points de
   calibration, lectures à 5 s nominales. Pente mesurée une fois la requête
   passée : **acide 101,0 % / base 100,0 % / zéro −1,03 mV** (sonde excellente).
2. **La requête `Slope,?` enfilée au boot (`sensors.cpp:158-160`) ne produit RIEN** :
   ni ligne de succès (`EZO pH slope : …`) ni warning d'échec au journal, et les
   caches `_phSlope*` restent NaN pendant ~20 min (constaté jusqu'à uptime ~1150 s).
   Le premier POST `/debug/ph_slope_refresh` manuel n'a pas non plus produit de log
   immédiat ; les suivants ont réussi normalement (3 lignes de succès).
   Hypothèses à départager en debug : (a) requête de boot consommée alors que
   l'EZO est occupé par les lectures d'init → échec streak=1 SILENCIEUX
   (`kEzoBusFailMaxConsecutive = 2` : un échec isolé ne se journalise jamais) ;
   (b) file recréée/purgée après l'enqueue de boot (`vQueueDelete` sensors.cpp:80-82) ;
   (c) timing I²C au boot.
3. **Incohérence de transport** : les champs `phSlopeAcid/Base/Zero/AgeMs` sont
   dans la trame WS (`ws_manager.cpp:400-406`) mais **absents de GET /data**
   (`web_routes_data.cpp` — grep vide). Un client sans WS (poll seul) ne peut
   jamais afficher la santé de sonde. C'est aussi ce qui a compliqué CE diagnostic
   (mes lectures /data montraient null alors que la trame WS était complète).
4. L'asymétrie pH/ORP est normale : l'EZO ORP n'a pas de diagnostic de pente,
   la puce ORP n'a donc pas cet état.

## Correctif attendu

1. La requête Slope de boot doit aboutir ou se journaliser : soit retenter
   automatiquement quelques secondes plus tard tant que `_phSlopeQueriedMs == 0`
   (sans attendre le cycle 24 h), soit journaliser dès le PREMIER échec quand
   aucune pente n'a jamais été lue.
2. Ajouter `phSlopeAcid/Base/Zero/AgeMs` à GET /data (mêmes règles de null que
   la trame WS) — parité de transport.
3. (Option UI) Distinguer « Pente pas encore lue depuis le démarrage » de
   « Pente non disponible » (le premier est un état transitoire attendu).

## Critères d'acceptation

- [ ] Après un reboot, la puce sonde pH atteint son état nominal (« Calibré · sonde X % ») en < 2 min sans action manuelle
- [ ] Un échec de `Slope,?` sans pente jamais lue se voit au journal dès la première occurrence
- [ ] `curl /data | jq .phSlopeAcid` renvoie la même valeur que la trame WS
- [ ] `pio run` + `pio test -e native` sans régression

## Notes

- Contournement immédiat : `POST /debug/ph_slope_refresh` (existant, feature-024).
- La confusion initiale « EZO indisponible » (calPoints -1 les toutes premières
  secondes) est un second libellé transitoire du même reboot — voir
  `classifyPhProbe` (data/app.js:3564).
