## Métadonnées
- **ID** : bug-200
- **Slug** : le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise
- **Type** : bug
- **Statut** : **done**
- **Priorité** : **critique**
- **Version cible** : ~~2.97.0~~ → **2.98.0** (la 2.97.0 a été publiée entre-temps, commit `72b0650`)
- **Dernière mise à jour** : 2026-11-18
- **Spec(s) liée(s)** : [bug-201](../done/bug-201-l-alerte-de-correction-ph-n-est-atteignable-qu-en-suspendant-la-regulation.md) (même famille de cécité) · [feature-195](../done/feature-195-langage-visuel-du-tableau-de-bord.md) (la grammaire de pastille qui accueillera le signal)

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
- [x] A1 — `maintenance_mode` servi dans `/data`
- [x] A2 — Même champ, **même expression**, et ⭐ `rescue_dosing` aussi
- [x] B1 — Pastilles ambre **+ texte « Arrêt sécurité »** ⛔ (la couleur seule ne suffit pas)
- [x] B2 — Bandeau dédié : cause, geste, **et la ligne EAU**
- [x] C1 — + `page-dashboard.md`, `auth.md`, `DESIGN_SYSTEM.md`, guide utilisateur, **ADR-0137**
- [x] D1 — `kSharedExpr` : **deux** clés, chacune motivée
- [x] D2 — ⭐ **13 mutants, 13 tués** + 1 mutant équivalent **déclaré**
- [x] D3 — ⛔ **impossible à provoquer** (pas d'appareil dédié) ⇒ prouvé par **forçage de champ**, 4 captures regardées

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

- [x] **M1** — ⭐ **quatre** messages traduits, pas un (décision utilisateur)
- [x] **M2** — fait, et les deux messages ne sont **plus confondables**
- [x] **M3** — `test/test_native_auth_messages/`, les deux mutants meurent

## Sécurité chimique
- Dosage impliqué : **oui** — le mode le suspend **entièrement**
- Limites concernées : aucune n'est modifiée
- Filtration requise : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : **non** — ⛔ ce lot n'ajoute **aucune** garde et n'en retire aucune
- Risque identifié : **le défaut est d'information, le remède doit être d'information.** ⛔ Ne rien
  ajouter au chemin de dosage.

---

## ✅ IMPLÉMENTATION — 2026-11-21, version 2.98.0

### ⭐ Ce que la fiche n'avait pas vu : l'écran ne se taisait pas, il MENTAIT

La fiche décrivait une **cécité**. Le gate `pool-chemistry` a établi, et j'ai vérifié, qu'il s'agit
d'un **faux témoignage** : la garde sort **avant** `resetRefusalLogState()`, donc `_lastRefusalCode`
**gèle** et `getPhDoseBlockedForS()` **continue de compter** sur l'ancienne cause.
⇒ Scénario le plus probable — l'arrêt se pose la nuit, filtration éteinte — on lit à midi
**« Filtration arrêtée — bloqué depuis 3 j »** pendant que la pompe de filtration tourne.
⚠️ **Une cause fausse assortie d'une durée fausse est pire qu'un silence** : elle fournit une
explication rassurante et close. ⛔ Ajouter un booléen à côté aurait laissé le mensonge intact.

### 🔴 CINQ bouches disaient le même mensonge — toutes fermées

| # | Mensonge | Trouvé par |
|---|---|---|
| 1 | La cause gelée et sa durée qui court | `pool-chemistry` |
| 2 | ⭐ La **chloration de secours** s'arme, publie vers Home Assistant, affiche un bandeau **pleine page** — et ne délivre **zéro millilitre** | `pool-chemistry` |
| 3 | « L'injection manuelle reste possible depuis les réglages » — **fausse** sous l'arrêt | capture, avant codage |
| 4 | La puce « **Pause mélange** » annonçait une reprise imminente qui ⛔ **ne viendrait jamais** | `web-ui-developer`, signalé honnêtement |
| 5 | Le bouton « Injecter » affichait le **texte brut du firmware**, avec le mot **banni** et le jargon « contrôleur » | `code-reviewer` |

⭐ **La leçon du lot** : le correctif de bug-200 a rejoué le défaut de bug-200 **quatre fois de
suite**, à chaque étage. ⇒ *Fermer un chemin n'est pas fermer un défaut* — c'est ce que la fiche
reprochait à bug-178, et il a fallu cinq passages pour le tenir ici.

### La conception, après arbitrage entre DEUX propositions incompatibles

`DoseRefusal::MaintenanceMode = 26` **ajouté en fin d'énum** (patron exact de `FlowAssistSuspend = 25`,
jamais retourné par `evaluateDose()`), posé par `postMaintenanceCause()` depuis
`src/pump_controller.cpp:3784-3785`, **après** `applyPumpDuty(0,0)/(1,0)` et `settlePumpAccounting()`.

⛔ **La proposition concurrente — classer côté client — a été écartée**, et son motif mérite d'être
gardé : elle aurait masqué le mensonge dans **un seul lecteur**, pendant que `/data` aurait continué
de servir « Filtration arrêtée, depuis 259 200 s » à **tout autre consommateur** (script tiers,
écran déporté, futur client). ⚠️ Une divergence serveur/client est **pire** que le défaut.

⭐ **Bénéfice gratuit** : `setRefusalCode()` redate `_refusalCodeSinceMs` à **tout changement de
code** ⇒ la durée **se ré-ancre toute seule**, sans une ligne écrite pour ça.

### Le piège de rang 0, et pourquoi le contourner est légitime ICI

`postSuspendCause()` sort immédiatement si `_refusalCauseFromGuard[i]` est vrai
(`src/pump_controller.cpp:862`) ⇒ ⛔ si une garde chimique possédait la cause au moment de la
bascule, la cause de l'arrêt **n'aurait JAMAIS été posée** et le bandeau aurait gardé la cause gelée
**pour toujours**. ⚠️ Un essai nominal ne l'aurait jamais montré.

`postMaintenanceCause()` **ne lit donc pas** ce drapeau. Justification **vérifiée trois fois**
(architecte, `code-reviewer`, moi) :
1. `setMaintenanceInhibit()` n'a que **deux appelants**, `src/main.cpp:1260` et `:1286`, **tous deux
   `true`** ⇒ ⛔ **aucune levée à chaud**. Le motif du rang 0 — *« la cause chimique serait perdue
   DÉFINITIVEMENT au retour »* — **ne s'applique pas** : il n'y a pas de retour.
2. ⭐ `newRefusal` et le **compteur des 3 refus / 24 h — dont l'escalade BASCULE LA RÉGULATION EN
   MANUEL** — vivent dans `tickPhGuardSurveillance()` (`:1051-1272`), ⛔ **pas** dans
   `logRefusalOnce()`. ⇒ Poser la cause ne crée **aucun** chemin vers un changement de mode.

⚠️ **Invariant écrit en commentaire** : *si un futur lot rend l'arrêt levable à chaud, ce
contournement redevient le défaut de bug-172.*

### Ce que le gate `pool-chemistry` a chiffré — et qui justifie la priorité « critique »

| Sans régulation | pH corrigé | Chlore actif |
|---|---|---|
| 8-12 h | **7,50** (plafond) | 52 % |
| 12-24 h | 7,6-7,8 | ⛔ **sous le plancher sanitaire** |
| **48 h** | **8,2-8,4** | **12-18 %** — l'épisode documenté |
| 72-96 h | 8,3-8,5 | ≈ 0, eau verte |

⚠️ Et le rattrapage **n'est pas symétrique** : au redémarrage, le pH étant au-dessus de
`kPhOrpInhibit`, la voie chlore **reste inhibée** tant qu'il n'est pas redescendu.
⇒ **1 à 3 jours de rattrapage pour 3 jours de panne.**

⭐ **Et l'appareil ne surveille plus l'eau non plus** : `tickPhGuardSurveillance()` /
`tickOrpPhInhibitSurveillance()` (`:4067-4068`) sont **en aval** de la sortie. La seule veille
restante est `ph < 5 || ph > 9` (`src/main.cpp:1950`), ⛔ qu'une dérive à 8,4 ne déclenche **jamais**.
⇒ C'est pour ça que le bandeau porte la **ligne EAU** : *« contrôlez vous-même le pH et le chlore »*.

### Décisions de l'utilisateur
- **Quatre** messages anglais traduits (pas un) : `/auth/change-password`. ⛔ Les **trois**
  « Invalid JSON » **restent en anglais** — ils ne s'affichent que sur requête malformée et servent
  au diagnostic d'un outil. ⚠️ **Écrit dans `docs/subsystems/auth.md`** pour que personne ne les
  « corrige » demain.
- Les virgules de **bug-199** ⛔ **ne voyagent pas** ici : elle a sa propre fiche et son propre
  inventaire d'écrans.

### Contrôles

`pio run` **SUCCESS, 0 warning** (flash 52,8 % · RAM 30,1 %) · `pio test -e native`
**96 suites, 2 718 cas, 0 échec** · `./build_fs.sh` SUCCESS · **toutes** les gardes vertes sauf
`check_release_artefacts.sh` (⛔ normal hors release) · budget de trame WS **recompté clé par clé**
et **réadditionné par la revue** : 3573 + 25 = **3598 o**, marge **498 o** sur 4096.
Vecteurs du tableau de bord : **31 → 113** vérifications.

### ⛔ Ce qui reste NON PROUVÉ, et ne peut pas l'être ici

La chaîne **réelle** — rafale de redémarrages ou corruption du tas → arrêt de sécurité → drapeau
vrai dans une vraie trame — n'a **jamais tourné sur matériel**. ⛔ Elle ne le peut pas : l'appareil
**dédié** de [bug-186](../todo/bug-186-cinq-essais-de-premier-demarrage-non-prouves-sur-materiel.md) n'existe
pas encore, et ⛔ provoquer cet état sur l'appareil de **production** est interdit (il ne retombe
qu'au redémarrage). ⇒ **6ᵉ chemin non prouvé, porté par bug-186.**
Le test de rang 0 lit le **texte du source** (`PumpControllerClass` n'est pas instanciable en natif) :
il attrape la réintroduction de la garde, ⛔ pas une erreur de comportement à l'exécution.

### Sorti du lot, avec sa fiche
[bug-207](../todo/bug-207-la-sortie-ota-gele-la-cause-de-blocage-comme-le-faisait-l-arret-de-securite.md) —
⭐ **la sortie OTA a EXACTEMENT le même défaut** (`src/pump_controller.cpp:3748`) : elle coupe les
pompes sans poser de cause. ⚠️ Moins grave (quelques minutes, et le propriétaire vient de la
déclencher), ⛔ mais le rang 0 s'y repose **à l'envers** : l'OTA, elle, **se termine**, donc la cause
chimique **serait** perdue au retour. ⇒ La réponse de bug-200 ⛔ **ne se recopie pas**.
