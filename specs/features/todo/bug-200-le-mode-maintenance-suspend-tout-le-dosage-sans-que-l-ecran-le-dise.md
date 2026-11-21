## Métadonnées
- **ID** : bug-200
- **Slug** : le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise
- **Type** : bug
- **Statut** : todo
- **Priorité** : **critique**
- **Version cible** : 2.97.0
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [bug-201](bug-201-l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation.md) (même famille de cécité) · [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (la grammaire de pastille qui accueillera le signal)

## Domaines touchés
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Web — routes HTTP / WebSocket
- [x] Frontend (`data/`)
- [ ] MQTT / Home Assistant *(l'alerte MQTT existe déjà — voir Comportement actuel)*

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
L'appareil possède un **mode maintenance** qui **suspend tout le dosage** — automatique, manuel
et programmé — pour une durée **non bornée**. Le tableau de bord ne le dit **nulle part** : il
continue d'afficher « Mode Auto » avec une pastille verte. Le propriétaire ne peut découvrir
l'état qu'en tentant une injection manuelle, qui échoue alors en 409.

## Contexte
Trouvé le 2026-11-18 en répondant à « je ne vois pas l'alerte » sur la correction de mesure pH.
L'analyse de la famille a montré que le pH n'était **pas** le cas le plus grave.

⚠️ **La frontière de la cécité est nette, et c'est le cœur du défaut** : tout ce qui passe par
`phDoseBlockedCode` est visible sur l'accueil (20 codes dans `DOSE_BLOCK_CODES`) ; **tout ce qui
court-circuite ce canal est aveugle**. Le mode maintenance sort du dosage **avant** toute
évaluation, donc **aucun code de blocage n'est posé**.

### Faits vérifiés dans le code (relus un par un, pas repris d'un rapport)
| Fait | Emplacement |
|---|---|
| Le mode coupe tout dosage et inhibe le suivant | `src/main.cpp:1250-1260` (`enterMaintenanceMode()`) |
| Il est aussi posé sur corruption du tas | `src/main.cpp:1295-1301` |
| La garde est **avant** toute évaluation de dosage, auto/manuel/programmé | `src/pump_controller.cpp:3739-3759` |
| Elle force les deux pompes à 0 à chaque cycle et purge le mode manuel | même bloc |
| Journalisé **CRITIQUE** + `publishAlert("maintenance_mode")` | `src/main.cpp:1255-1260` |
| ⛔ **Zéro** occurrence dans la trame WebSocket | `grep -ci maintenance src/ws_manager.cpp` → **0** |
| ⛔ **Aucun champ** dans `/data` (l'unique occurrence est un commentaire sans rapport) | `src/web_routes_data.cpp:823` |
| Côté navigateur, le mot n'existe que comme **message d'échec** d'une injection refusée | `data/app.js:14185` |
| Seule trace exposée : un **409** sur `/ph|orp/inject/start` | `src/web_routes_control.cpp:100-103` |

### Pourquoi c'est critique et pas « normal »
- La durée est **non bornée** : l'inhibition ne retombe qu'au redémarrage.
- Le déclencheur est un **défaut mémoire / une rafale de reboots** — précisément l'historique
  documenté de cet appareil (corruption de partition, incidents OOM).
- Le journal de l'appareil tient **8 Ko, environ une minute** : la ligne CRITIQUE a disparu bien
  avant que quiconque la lise. ⛔ Le journal ne peut pas servir de canal d'alerte.
- L'alerte MQTT ne sauve que les installations qui ont un courtier — ⛔ **on n'exige rien de
  l'infrastructure du client**.
- ⇒ Un bassin peut rester **des jours sans régulation** pendant que l'écran affiche « Mode Auto ».
  C'est exactement le scénario que les conditions B2/C7 de feature-195 cherchaient à fermer.

## Comportement attendu
1. L'état est **servi** : un booléen dans `/data` **et** dans la trame WebSocket.
   ⚠️ **Parité d'EXPRESSION, pas seulement de clé** — c'est la 4ᵉ récidive connue du piège, et
   la 4ᵉ forme était **invisible au grep** (même clé, expressions divergentes). Les deux chemins
   appellent **le même accesseur**, ils ne recalculent rien.
2. L'accueil le montre **sans geste** : pastilles pH et ORP en **ambre**, via
   `doseBlockVariant === 'danger'`. ⭐ La grammaire arrêtée par feature-195 le permet **sans
   exception nouvelle** — l'ambre y signifie déjà « l'appareil s'écarte de son automatisme ».
3. Le bandeau de blocage de dosage porte la cause en clair et le geste : redémarrer le contrôleur.
4. ⛔ **Ni masquable, ni repliable, ni fermable** — même régime que `#dose-block-banner`
   (`data/index.html:183-186`).

### Cas limites
- Hors ligne : l'état vient de la dernière trame connue ; il ne doit **jamais** s'éteindre tout seul.
- Démarrage **sans réseau** : aucune alerte MQTT ne part. L'écran reste le seul canal — il doit tenir.
- Mode maintenance **et** filtration arrêtée : deux causes, une seule pastille. Dire laquelle prime.

## Comportement actuel
Rien n'est servi. L'écran affiche l'automatisme nominal. Le seul retour possible est l'échec 409
d'une injection manuelle, que personne ne tente sans raison.

## Contraintes techniques
- ⚠️ **RESPONSIVE** : 390 px d'abord.
- ADR-0082 : aucune allocation sur le chemin de journalisation.
- Budget de trame WebSocket : la marge est documentée dans `src/ws_manager.cpp:1484` — un booléen
  tient, le vérifier plutôt que l'affirmer.
- Vanilla JS, français dans toute l'UI.

## Fichiers potentiellement concernés
- `src/pump_controller.h/.cpp` — exposer l'accesseur unique (`isMaintenanceInhibited()` existe déjà)
- `src/web_routes_data.cpp` — nouveau champ
- `src/ws_manager.cpp` — même champ, **même expression**
- `data/app.js` — consommation, pastille, bandeau
- `data/index.html`, `data/app.css` — si un libellé neuf est nécessaire
- `docs/subsystems/pump-controller.md`, `docs/API.md`, `docs/subsystems/ws-manager.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Champ servi dans `/data`, par appel à l'accesseur
- [ ] A2 — Même champ dans la trame WS, **même expression** (sentinelle de parité)
- [ ] B1 — Pastilles pH et ORP en ambre via `doseBlockVariant === 'danger'`
- [ ] B2 — Cause et geste en clair dans le bandeau de blocage
- [ ] C1 — `docs/API.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`
- [ ] D1 — Test de parité `/data` ↔ WS sur l'**expression**
- [ ] D2 — ⭐ **Mutation exigée** : retirer le champ d'**un seul** des deux chemins doit **tuer** un test. Sinon la parité n'est pas protégée.
- [ ] D3 — Essai réel : provoquer le mode sur l'appareil **dédié** (⛔ jamais celui de production), vérifier l'écran

## 📦 Passager embarqué — deux messages à corriger (ajouté le 2026-11-21, vient de bug-187)

⚠️ **Ce n'est pas un élargissement de périmètre** : ce lot recompile déjà le firmware, et ces
deux corrections sont **deux chaînes de caractères**. Les faire voyager ici évite à l'utilisateur
une seconde cérémonie de signature OTA et un second redémarrage — décision du 2026-11-21.
⇒ Relevés sur l'appareil réel, voir [bug-187](../done/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md), section « ESSAI RÉEL ».

| # | Défaut mesuré | Où |
|---|---|---|
| **M1** | Un mot de passe d'appareil trop court rend **`New password must be at least 8 characters`** — ⚠️ **en anglais**, alors que les quatre autres verdicts de la même route sont en français (c'est bug-187 qui les a traduits ; celui-ci lui est **antérieur** et n'a pas été revu avec eux). | `src/web_routes_auth.cpp`, branche `kDenyTooShort` |
| **M2** | Le refus « famille manquante » de la **clé Wi-Fi** dit seulement « Le mot de passe doit contenir… » — **mot pour mot** celui du mot de passe de l'appareil, alors que ses **quatre** frères précisent « du point d'accès ». ⚠️ Sur un écran où les deux objets coexistent, on ne sait pas lequel est reproché. | `src/web_routes_auth.cpp`, branche `kMissingFamily` de `/auth/ap-password` |

⛔ **Aucune garde n'est en cause** : les refus sont **justes**, c'est leur **formulation** qui
cloche. ⛔ Ne pas en profiter pour changer un seuil ou un verdict.

⚠️ **Pourquoi aucun test natif ne les a vus** : les suites comparent le **verdict** rendu par
`apPwdCheck` / `decidePasswordChange`, ⛔ jamais la **phrase** envoyée au navigateur. ⇒ La
correction doit venir avec un test qui regarde la **phrase**, sinon le troisième message anglais
passera aussi.

- [ ] **M1** — message de longueur minimale en français
- [ ] **M2** — le refus de la clé Wi-Fi dit « du point d'accès » comme ses quatre frères
- [ ] **M3** — un test tue un mutant qui remet l'anglais, ou qui aligne les deux messages

## Sécurité chimique
- Dosage impliqué : **oui** — le mode le suspend **entièrement**
- Limites concernées : aucune n'est modifiée
- Filtration requise : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : **non** — ⛔ ce lot n'ajoute **aucune** garde et n'en retire aucune
- Risque identifié : **le défaut est d'information, le remède doit être d'information.** ⛔ Ne rien
  ajouter au chemin de dosage.
