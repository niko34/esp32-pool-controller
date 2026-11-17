# bug-188 — MQTT publie le nom du réseau et l'adresse de l'appareil en message RETENU

## Métadonnées
| | |
|---|---|
| Type | bug — 🟠 **divulgation d'informations** |
| Statut | todo |
| Priorité | basse — ⚠️ **décision produit** avant tout travail de code |
| Version cible | à définir |
| Relevé par | `security-reviewer` (P1 et P2 de [`bug-181`](../done/bug-181-get-config-expose-la-configuration-sans-authentification.md)), 2026-11-17 — ⭐ **vérifié dans le code** le même jour |
| Décision | l'utilisateur a demandé le 2026-11-17 de **garder le sujet en attente** |

## Domaines touchés
- [x] **MQTT / Home Assistant** (`src/mqtt_manager.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] Authentification — [ ] Frontend

## Agents requis
- [x] web-ui-developer *(domaine MQTT)* · [x] `security-reviewer` · [x] doc-writer
- [ ] architect · [ ] test-engineer *(à cocher si le lot dépasse le retrait de deux champs)*

## Le fait, mesuré dans le code

`src/mqtt_manager.cpp:1416-1419` — le diagnostic porte :
```cpp
doc["wifi_"] = WiFi.SSID();          // ⚠️ clé TRONQUÉE, visiblement accidentelle
doc["ip_address"] = WiFi.localIP().toString();
```
et il est publié **RETENU** (`safePublish(topics.diagnosticTopic.c_str(), payload, true)`, `:1560`).

⇒ ⛔ **Tout abonné du broker lit le nom du réseau et l'adresse de l'appareil, même appareil
éteint** — c'est ce que « retenu » signifie : le serveur garde le dernier message et le sert à
quiconque s'abonne ensuite.

`<base>/logs` relaie en outre le journal, qui contient « WiFi connecté: &lt;SSID&gt; »
(`src/main.cpp:866`).

⇒ **La protection est celle du broker, pas de l'appareil.** `bug-181` a fermé la dernière voie
**web** ; celle-ci reste ouverte et ne dépend pas du firmware.

## La décision à prendre AVANT de coder

⛔ **Ce n'est pas un arbitrage technique.** Deux issues, et c'est à l'utilisateur de choisir :
1. **Retirer** `wifi_` et `ip_address` du diagnostic retenu — ⚠️ vérifier d'abord qu'aucune carte
   Home Assistant ne les affiche ;
2. **Assumer par écrit** : le broker est sur le réseau domestique, ses identifiants le protègent,
   et la fiche se ferme en le disant.

## À corriger dans tous les cas
- [ ] ⚠️ La clé `"wifi_"` est **tronquée** (`mqtt_manager.cpp:1416`) — manifestement un
  `wifi_ssid` amputé. ⛔ La renommer **change le contrat MQTT** : vérifier Home Assistant.

## Critères d'acceptation
- [ ] La décision (retrait **ou** acceptation écrite) est tranchée et consignée
- [ ] Si retrait : `docs/MQTT.md` à jour, et aucune carte Home Assistant cassée
- [ ] La clé tronquée est traitée, ou son maintien est justifié par écrit
