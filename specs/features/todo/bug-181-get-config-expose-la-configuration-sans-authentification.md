## Métadonnées
- **ID** : bug-181
- **Slug** : get-config-expose-la-configuration-sans-authentification
- **Type** : bug — 🔴 **sécurité applicative** (divulgation d'informations)
- **Statut** : todo — ⚠️ **périmètre réduit au seul `/wifi/status`** depuis le 2026-11-17
- **Priorité** : normale
- **Version cible** : 2.89.0
- **Spec(s) liée(s)** : `bug-179`, `bug-180` (relevés dans la même campagne) · [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) (a fermé `/get-config`) · [`bug-186`](bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md) (porte l'essai D3)

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] Web — routes HTTP
- [x] MQTT / Home Assistant (l'identifiant et le broker fuient)
- [ ] UART / écran LVGL
- [x] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`) — ⚠️ la page de login et l'assistant consomment ces routes

## Agents requis
- [x] spec-reader · [x] architect · [ ] pool-chemistry · [ ] ux-ui-designer
- [x] embedded-developer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

⚠️ `security-reviewer` : **OBLIGATOIRE** — c'est l'objet de la spec, et elle touche `auth` + `web_routes_*`.

## Description
`GET /get-config` répond **200 sans authentification** — c'est **délibéré** (route publique pour le
premier démarrage) et elle **masque** le mot de passe et le jeton d'administration. ⛔ **Mais le
masquage s'arrête là** : l'adresse du broker MQTT, son identifiant, le SSID Wi-Fi et 110 champs de
configuration partent en clair à quiconque est sur le réseau.

## Contexte

**Relevé le 2026-11-15** en vérifiant le livrable A0 de `bug-179`, sur l'appareil réel.

```
/get-config        → HTTP 200  (sans aucune authentification)   ⚠️
/data              → HTTP 401  ✅
/get-logs          → HTTP 401  ✅
/get-system-info   → HTTP 401  ✅
```

### Ce qui est masqué — le mécanisme EXISTE et fonctionne
`web_routes_config.cpp:118` : `isAuthenticated = checkTokenAuth || checkBasicAuth`, puis `:403-410`
masquent `auth_password` (`"******"`) et `auth_token` (`"********..."`).
✅ **Vérifié** : le jeton ainsi exposé **ne donne aucun accès** (HTTP 401 avec lui). ⛔ **Aucune
fuite d'identifiant d'administration.**

### Ce qui ne l'est PAS — 116 champs, émis inconditionnellement
| Champ | État | Source |
|---|---|---|
| `server` (broker MQTT) | **en clair** | hors du bloc `isAuthenticated` |
| `port` (1883) | **en clair** | idem |
| `username` (MQTT) | **en clair** | `web_routes_config.cpp:176` |
| `password` (MQTT) | ✅ masqué | — |
| `wifi_ssid` | **en clair** | `web_routes_config.cpp:249` |
| `wifi_ip`, `mdns_host` | **en clair** | — |
| ~110 champs fonctionnels | **en clair** | consignes, horaires, limites de dosage, étalonnages |

⇒ Un attaquant sur le réseau local obtient **la cible et la moitié du couple d'identifiants** du
broker MQTT. Il ne lui manque que le mot de passe.

### 🔴 LE SSID A UNE SECONDE VOIE — masquer `/get-config` seul ne fermerait RIEN
```
GET /wifi/status  (sans authentification)
→ {"mode":"STA","connected":true,"ssid":"<SSID>","ap_ssid":"","ap_ip":"0.0.0.0"}
```
⚠️ Route publique assumée (« utilisées depuis la page de login en mode AP »,
`web_routes_config.cpp`). ⛔ **Tout correctif qui ne traite qu'une des deux voies donne l'illusion
d'avoir réglé le problème** — c'est le motif d'application partielle que ce projet documente déjà.

## ⭐ MISE À JOUR DU 2026-11-17 — une moitié est fermée, l'autre NON

`bug-185` (livrée en **2.93.0**, déployée le 2026-11-17) a fermé **`/get-config`**, et plus
fortement que cette fiche ne le demandait : ⛔ **la route entière exige désormais une
authentification** (`REQUIRE_AUTH(WRITE)`), au lieu d'une liste blanche champ par champ.

**Mesuré sur l'appareil le 2026-11-17 :**
```
GET /get-config  sans jeton  -> 401   ✅ (était 200)
GET /get-config  avec jeton  -> 200   ✅ inchangé
GET /wifi/status sans jeton  -> 200   ⛔ SSID TOUJOURS EN CLAIR, en mode STA
```

⚠️ **La seconde voie du SSID reste ouverte** — exactement ce que cette fiche annonçait :
« tout correctif qui ne traite qu'une des deux voies donne l'illusion d'avoir réglé le problème ».
⛔ **Cette fiche RESTE en `todo`** pour ce seul motif.

⇒ **Périmètre résiduel** : livrable **A2** uniquement (`/wifi/status`, SSID restreint au mode AP),
plus ses tests. Les livrables A0/A1 sont **sans objet** pour `/get-config` : la route n'émet plus
rien sans jeton, donc il n'y a plus de liste à tenir.

⚠️ **Et le risque principal reste entier** : `/get-config` est désormais fermée **sans que le
premier démarrage ait été rejoué sur un appareil neuf**. Le livrable **D3** de cette fiche est
suivi dans [`bug-186`](bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md).

## Comportement attendu
1. ⛔ **Rien de sensible hors du bloc `isAuthenticated`.** Le non authentifié ne reçoit que le
   **strict nécessaire** au premier démarrage et à la page de login.
2. ⚠️ **Déterminer ce strict nécessaire par LECTURE du frontend**, pas par supposition : que lisent
   réellement `data/login.html`, `data/wifi.html`, `data/wizard.html` et le chemin de premier
   démarrage de `data/app.js` **avant** d'être authentifiés ?
3. `/wifi/status` : même traitement. Si le SSID y est nécessaire **en mode AP** (pour aider
   l'utilisateur à se reconnecter), le limiter **à ce mode** — ⛔ pas en mode station.
4. Le masquage doit être **par défaut** : ⚠️ aujourd'hui un champ ajouté hors du bloc est exposé
   **sans que rien ne le signale**. ⇒ Inverser la logique si possible (liste blanche publique).

### Cas limites
- ⛔ **Premier démarrage** : l'assistant doit rester **entièrement fonctionnel** sans authentification.
- **Mode point d'accès** (`PoolControllerAP`) : la page de configuration Wi-Fi doit fonctionner.
- **Authentification désactivée** (`authEnabled = false`) : comportement inchangé, tout est ouvert
  par choix de l'utilisateur.
- ⚠️ Un client tiers (Home Assistant, script) qui lirait `/get-config` sans jeton **cesserait de
  fonctionner** ⇒ à signaler dans le CHANGELOG comme **changement de comportement**.

## Comportement actuel
`handleGetConfig` ne bloque pas : elle calcule `isAuthenticated` et s'en sert uniquement pour
masquer deux champs. Les 114 autres sont émis dans tous les cas.

## Contraintes techniques
- ⛔ **Ne pas casser le premier démarrage ni la page de login** — c'est le risque principal.
- Champs de réponse : ⚠️ un champ **absent** plutôt que masqué peut casser un front qui le lit.
  ⇒ Décider **masquage** ou **omission**, et s'y tenir.
- ESP32, pas de dépendance neuve. Logique de décision **pure** et testable (ADR-0017).

## Fichiers potentiellement concernés
- `src/web_routes_config.cpp` — `handleGetConfig` (`:118`, `:176`, `:249`, `:403-410`), `/wifi/status`
- `data/login.html`, `data/wifi.html`, `data/wizard.html`, `data/app.js` — ce qu'ils lisent avant authentification
- `docs/API.md`, `CHANGELOG.md`

## Livrables — check-list
- [~] A0 — ⭐ **sans objet pour `/get-config`** depuis `bug-185` (route entière authentifiée). Reste valable pour `/wifi/status`.
- [x] A1 — ✅ **fait autrement** par `bug-185` : `/get-config` entière derrière l'authentification (mesuré 401 le 2026-11-17)
- [ ] A2 — ⛔ **TOUJOURS OUVERT** — `/wifi/status` rend le SSID en clair sans jeton, en mode STA (mesuré le 2026-11-17). **C'est le seul motif qui maintient cette fiche en `todo`.**
- [ ] A3 — Logique de décision en module **pur**, testable en natif
- [ ] C1 — `docs/API.md` (contrat des deux routes), `CHANGELOG.md` (**changement de comportement**)
- [ ] D1 — Suite native sur la décision d'exposition
- [ ] D2 — Validation par MUTATION (⚠️ dont « un champ neuf est exposé par défaut » ⇒ doit tuer)
- [ ] D3 — ⭐ **Essai de terrain** — ⚠️ **déplacé dans [`bug-186`](bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md)**, repoussé par l'utilisateur au 2026-11-17 (appareil réinitialisé, horizon début 2027)

## ADR attendu ?
- **Probable** : non — durcissement, sans alternative structurante. ⚠️ Sauf si l'inversion
  liste noire → liste blanche est jugée structurante : alors critère 1.

## Critères d'acceptation
- [ ] `/get-config` **sans** authentification n'expose ni `server`, ni `username` MQTT, ni `wifi_ssid`
- [ ] `/wifi/status` **sans** authentification n'expose le SSID **qu'en mode AP**
- [ ] ⭐ **Le premier démarrage et la page de login fonctionnent** (essai réel)
- [ ] `/get-config` **avec** authentification est **inchangée**
- [ ] Un champ ajouté par erreur hors de la liste blanche **n'est pas exposé**
- [ ] Suite native verte, mutations tuées · `pio run` sans nouveau warning · `build_fs.sh` passe

## Plan de test manuel
1. ⭐ **Premier démarrage** (appareil réinitialisé ou mode AP) : l'assistant va au bout **sans authentification**.
2. Page de login : s'affiche et permet de se connecter.
3. `curl` sans jeton sur `/get-config` et `/wifi/status` ⇒ aucun champ sensible.
4. `curl` **avec** jeton ⇒ réponse complète, inchangée.
5. Home Assistant / MQTT : toujours fonctionnels.

## Notes pour agent
- ⚠️ **Défaut PRÉEXISTANT**, trouvé en vérifiant autre chose (`bug-179` A0). Divulgation
  d'informations, ⛔ **pas** une compromission : les identifiants d'administration sont masqués et
  le jeton exposé **ne donne aucun accès** (vérifié).
- ⛔ **Le risque du correctif est de casser le premier démarrage en silence** — un appareil neuf ou
  réinitialisé deviendrait inconfigurable. L'essai D3 n'est pas optionnel.
- ⚠️ **Les deux voies du SSID doivent être traitées ENSEMBLE**, sans quoi le correctif ne corrige rien.
