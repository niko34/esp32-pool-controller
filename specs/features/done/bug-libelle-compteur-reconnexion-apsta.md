# Bug — Libellé de compteur faux « tentative de reconnexion (47/3) » en mode AP secours

- **Type** : bug simple (format minimal `specs/modele.md`)
- **Statut** : done
- **Version cible** : 2.10.3
- **Créée / résolue** : 2026-07-23 (repéré par analyse de code lors d'une question utilisateur sur le comportement du watchdog connectivité en mode AP)
- **Domaine** : logs WiFi (main.cpp) — aucun chemin de dosage touché

## Symptôme

En mode AP+STA de secours, la logique de reconnexion STA (période 30 s) loggue « WiFi déconnecté, tentative de reconnexion (n/3) » avec un compteur `wifiReconnectAttempts` qui n'est réinitialisé qu'à la reconnexion — après une journée sans box, le journal affiche « (2881/3) ». Libellé faux, aucun effet fonctionnel (la bascule AP secours exige `mode == WIFI_MODE_STA` et n'est jamais réarmée à tort).

## Correctif (main.cpp, ~l.935)

Le compteur « (n/3) » n'est affiché qu'en mode STA (où il arme réellement la bascule AP au 3ᵉ échec). En mode `WIFI_MODE_APSTA` (secours déjà actif), le message devient : « WiFi déconnecté, tentative de reconnexion STA (AP secours actif) » — sans compteur. Aucune logique modifiée (mêmes tentatives, mêmes transitions).

## Validation

- [x] `pio run` SUCCESS, 0 warning nouveau.
- [x] Aucun changement de comportement hors libellé du log.
- [x] Aucun chemin de dosage touché.
