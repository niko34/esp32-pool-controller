# Bug — `/save-config` applique et persiste la configuration AVANT la vérification d'authentification

- **Type** : bug de sécurité (format minimal `specs/modele.md`)
- **Statut** : **done** — audit conduit, 3 défauts trouvés et corrigés (v2.46.1, 2026-09-07)
- **Priorité** : haute
- **Version cible** : 2.46.1
- **Créée** : 2026-08-15 (relevé par `code-reviewer` pendant le cycle feature-067,
  signalé comme PRÉ-EXISTANT et hors périmètre de ce cycle)
- **Domaine** : Web — routes HTTP, Authentification, et indirectement Régulation
- **Specs liées** : feature-067 (offset pH — c'est elle qui a rendu le défaut visible)

## Symptôme

Dans `src/web_routes_config.cpp`, `handleSaveConfig()` est le **handler de corps**
de la route `POST /save-config`. Il **applique la configuration en mémoire ET la
persiste** (`saveMqttConfig()`, vers la ligne 597) — alors que `REQUIRE_AUTH` n'est
évalué que dans le **handler de requête**, exécuté APRÈS (vers la ligne 873).

Conséquence : un client non authentifié qui envoie un corps JSON valide voit sa
configuration **appliquée et écrite en NVS**, avant que la requête ne soit refusée.
Le refus HTTP arrive trop tard : l'effet de bord a déjà eu lieu.

## Pourquoi ça compte davantage depuis la feature-067

`ph_offset` ajoute à cette route un paramètre qui **décale la consigne effective de
la régulation pH** : écrire un offset décale le pH réel visé par le dosage d'acide.
Les autres paramètres exposés par cette route (cibles pH/ORP, limites de dosage,
mode de régulation) étaient déjà sensibles ; ce nouveau champ élargit la surface.

Les filets de sécurité chimique livrés en feature-067 (plancher acide sur le pH
brut, inhibition ORP) bornent les conséquences, mais ne suppriment pas le défaut.

## Correctif attendu

1. Déplacer la vérification d'authentification **avant** toute application ou
   persistance — soit en la faisant dans le handler de corps dès le premier
   fragment reçu, soit en différant l'application jusqu'au handler de requête
   (le corps serait alors seulement mis en tampon, ce qui est déjà le cas).
2. Vérifier le comportement des autres routes construites sur le même patron
   (handler de corps + handler de requête) : `POST /update`, calibration,
   toute route acceptant un corps JSON.
3. Ne pas casser le flux de premier démarrage (assistant de configuration), qui
   peut légitimement écrire une configuration avant qu'un mot de passe existe —
   à vérifier explicitement.

## Critères d'acceptation

- [ ] Une requête `POST /save-config` non authentifiée ne modifie NI la
      configuration en mémoire, NI la NVS (vérifiable : relire `/get-config`
      après la tentative)
- [ ] Le flux de premier démarrage reste fonctionnel
- [ ] Les autres routes à handler de corps sont auditées et le résultat consigné
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression

## Mise à jour du statut (2026-08-27) — cœur livré, spec MAINTENUE

Le défaut principal a été **corrigé au passage** du cycle de refonte des seuils
pH (v2.25.0) : `saveConfigBodyAuthorized()` vérifie l'authentification **avant**
toute mutation, le gate d'acquittement 409 est le premier bloc du handler, et
`configMutex` est pris puis rendu sur chaque sortie. Confirmé par deux revues de
code successives : « auth `/save-config` non régressée, gate 409 avant toute
mutation ».

**Cette spec reste néanmoins en `todo/`** : deux de ses critères ne sont pas
remplis, et les cocher serait faux.

- [ ] **Audit des autres routes à handler de corps** — non fait. C'est le
  critère qui donne sa valeur à cette spec : le motif « corps appliqué avant
  vérification » n'a aucune raison d'être limité à `/save-config`, et rien ne
  garantit aujourd'hui qu'il ne se répète pas ailleurs. Tant que l'audit n'est
  pas conduit et son résultat consigné, le risque est inconnu — pas absent.
- [x] ~~**Build bi-core**~~ — **critère PÉRIMÉ, retiré le 2026-09-07.** L'ADR-0069
  (v2.42.1) a supprimé le repli core 2 : `esp32dev` n'existe plus, `esp32dev_core3`
  est le seul environnement matériel. Il n'y a plus de bi-core à vérifier. Ce
  critère ne pouvait donc plus être rempli, ni utilement coché — il est **annulé**,
  pas satisfait.

Le premier critère est donc le vrai reste à faire, et il est indépendant du
correctif déjà livré.

---

## Clôture (2026-09-07, v2.46.1) — audit conduit, trois défauts réels corrigés

- **Statut** : livré — tests verts, revue OK (`code-reviewer` : *Request changes* → corrigé → approuvé ; `pool-chemistry` : **GO**)
- **Version cible** : **2.46.1**
- **Fichiers modifiés** : [`src/web_routes_calibration.cpp`](../../../src/web_routes_calibration.cpp) (+110), [`src/constants.h`](../../../src/constants.h) (+13), [`src/version.h`](../../../src/version.h)

Le correctif du site d'origine `/save-config` était **déjà en place depuis la
v2.25.0**. Le reste à faire était **l'audit des autres routes à handler de
corps** — et il a trouvé un défaut réel, sur trois routes.

### Le défaut trouvé

`/calibrate_ph`, `/calibrate_orp` et `/calibrate_clear` mettaient la commande EZO
en file dans le handler de **corps**, alors que `REQUIRE_AUTH(WRITE)` n'est évalué
que dans le handler de **requête**, exécuté **après**. Le client recevait bien son
`401`, mais `loopTask` exécutait la calibration **~900 ms plus tard**. Effet
**irréversible** : il n'existe pas de dé-calibration.

**Pourquoi il a survécu à plusieurs revues** : `REQUIRE_AUTH` **est présent**, trois
lignes au-dessus du handler de corps. Un relecteur qui cherche la macro voit une
route protégée. C'est la leçon transférable de cette spec, et c'est **exactement la
mécanique du bug d'origine sur `/save-config`** — d'où l'intérêt d'avoir maintenu la
spec ouverte pour son seul critère d'audit.

### Le correctif

- `calibrationBodyAuthorized()` en **première instruction** des trois handlers de
  corps, avant tout parse et tout `enqueue`.
- Prédicat **identique à `checkAuth()` moins le rate limiting** → aucune calibration
  légitime refusée (vérifié dans les deux sens : autorisé passe, anonyme est ignoré).
- **Sans** exemption de premier démarrage : aucune des trois routes n'appartient à
  l'assistant de configuration (`/calibrate_clear` n'a même aucun appelant d'interface).
- WARN **throttlé à 1 par 60 s et par route** (`kCalibrationAuthWarnThrottleMs`),
  avec compteur d'occultés, dont le **chemin muet n'alloue rien**.

### Résultat de l'audit — 7 sites à handler de corps

| Site | Verdict |
|---|---|
| `POST /calibrate_ph` ([`web_routes_calibration.cpp:108`](../../../src/web_routes_calibration.cpp:108)) | ❌ **défaut** — `enqueue` avant auth → **corrigé** |
| `POST /calibrate_orp` ([`:170`](../../../src/web_routes_calibration.cpp:170)) | ❌ **défaut** — idem → **corrigé** |
| `POST /calibrate_clear` ([`:226`](../../../src/web_routes_calibration.cpp:226)) | ❌ **défaut** — idem → **corrigé** |
| `POST /save-config` ([`web_routes_config.cpp:371`](../../../src/web_routes_config.cpp:371)) | ✅ **sain** — `saveConfigBodyAuthorized()` depuis v2.25.0 ; **site de référence** du patron |
| `POST /auth/login` ([`web_routes_auth.cpp:21`](../../../src/web_routes_auth.cpp:21)) | ✅ **sain** — route **publique par conception** (elle délivre le token) ; `checkRateLimit()` en première instruction |
| `POST /auth/change-password` ([`web_routes_auth.cpp:61`](../../../src/web_routes_auth.cpp:61)) | ✅ **sain** — l'authentification est vérifiée **dans** le handler de corps, **avant** toute mutation (`currentPassword` ou `checkAuth`), exemption premier démarrage explicite |
| `POST /update` (upload, [`web_routes_ota.cpp:234`](../../../src/web_routes_ota.cpp:234)) | ⚠️ **sain au sens du motif audité** (aucune décision d'auth n'y est *dupliquée* de travers), **mais réserve consignée ci-dessous** |

> ⚠️ **Réserve sur `POST /update`, à lever par une spec dédiée.** Le handler
> d'upload ne contient **aucune** vérification d'authentification (le fichier
> n'appelle `authManager` nulle part ; `REQUIRE_AUTH(CRITICAL)` est posé dans le
> lambda de réponse, [`web_routes_ota.cpp:1198`](../../../src/web_routes_ota.cpp:1198)),
> et c'est bien lui qui appelle `Update.begin()`
> ([`:400`](../../../src/web_routes_ota.cpp:400)) puis `Update.end(true)`
> ([`:497`](../../../src/web_routes_ota.cpp:497)). La topologie est donc la même que
> celle corrigée ici, avec un effet de bord plus lourd. Ce point **n'a pas été
> instruit dans ce cycle** (code figé) : il est consigné tel quel, sans correctif
> et sans conclusion, comme entrée d'une spec de durcissement OTA.

### Frontière avec `feature-070` — aucun recouvrement

| | `feature-070` (durcissement routes debug) | **cette spec** |
|---|---|---|
| Problème | **Aucune authentification** sur les routes visées | Authentification **présente**, mais **évaluée trop tard** |
| Nature | Arbitrage de politique : « ouvrir ou fermer » | **Bug d'ordonnancement** : rien à arbitrer |
| Correctif | Poser une protection là où il n'y en a pas | Déplacer une protection existante avant l'effet de bord |

Les deux specs appartiennent à la même famille (routes HTTP / authentification) mais
ne se chevauchent sur **aucun site**.

### Portée chimique — verdict `pool-chemistry` (gravité **haute**)

Deux régimes opposés, tous deux graves :

- **`Cal,clear` — et `Cal,mid` seul, l'EZO effaçant les autres points → fail-closed.**
  `calPoints < requiredPoints` ([`pump_controller.cpp:1738`](../../../src/pump_controller.cpp:1738),
  [`dosing_logic.h:180`](../../../src/dosing_logic.h:180)) **refuse** le dosage
  automatique : arrêt **silencieux** de la régulation. À CYA ~35 mg/L, la boucle ORP
  saturant déjà son quota horaire chaque jour, la sous-chloration se compte **en jours**.
- **`/calibrate_orp` → surdosage.** `requiredPoints = 1` : **un seul POST suffit**, la
  garde reste satisfaite. `reference` bornée `0..1000` mV, soit jusqu'à **±500 mV** de
  décalage sur une consigne de 530 → erreur écrêtée, P pur en **saturation permanente**.
  Même chaîne côté pH via `Cal,mid` puis `Cal,low` : mesure faussée → erreur contre
  7,30 → sortie saturée → 10 min/h × 26,0 mL/min = **260 mL/h d'acide**.

> **La phrase à retenir : les filets de sécurité ne bornent pas, ils ralentissent.**
> Les gardes 6,70 (corrigé), 6,20 (brut) et 7,60 (inhibition ORP) sont calculées **sur
> la mesure falsifiée** — dérivées de `kPhOffsetMax`, elles couvrent l'**offset**, pas
> la **calibration**. Seules bornent réellement `maxPhMlPerDay` (défaut 300 mL/j) et la
> limite horaire : elles plafonnent le **débit**, mais le quota se réarme à minuit, donc
> la **dérive cumulative n'a pas de plafond**. Le timer de stabilisation
> post-calibration ([`sensors.cpp:882-915`](../../../src/sensors.cpp:882)) ne fait que
> **différer**.

### Journal : pourquoi le WARN est throttlé

Le quota `kMaxRequestsPerMinute` ne borne **pas** un émetteur qui journalise depuis un
handler de corps (`checkRateLimit()` s'exécute plus tard, dans le handler de requête).
Une ligne persistée pèse **~230 o** : sans throttle, les **6 144 o** conservés par la
rotation (`kRotateKeepBytes`, fichier de 8 192 o) sont purgés en **~27 requêtes**, soit
**~13 s** au seul quota de 120 req/min — la fenêtre forensique de ≈ 2 h 30 gagnée par
`feature-109`, et dont dépend `bug-080`, devient effaçable à volonté.

⚠️ **L'émetteur non borné PRÉEXISTE** : `AuthManager::sendAuthRequired()`
([`auth.cpp:231`](../../../src/auth.cpp:231)) émet un WARN **sans throttle** sur toute
route protégée. Ce cycle le **multipliait par ~2,7** sur trois routes ; il ne l'a pas
créé, et le throttle n'annule que sa propre contribution. Spec dédiée **ouverte** :
[`bug-112`](../todo/bug-112-journal-non-borne-sur-route-protegee.md) (périmètre non arrêté ici). Le commentaire de `kCalibrationAuthWarnThrottleMs` chiffre
le journal à 24 Ko/32 Ko — tailles **historiques**, antérieures à l'ADR-0033 : chiffre à
reprendre, conclusion inchangée (elle est même plus sévère).

### Plan de test manuel — écrit, **non exécuté**

Le plan (POST anonyme sur les trois routes → attendre 2 s → relire `phCalPoints` /
`orpCalPoints` inchangés, puis POST authentifié → calibration effective, puis lecture
de `/system.log`) **n'a pas été exécuté**. Motif : l'appareil tourne la **2.45.0**, qui
**contient** le défaut — y lancer le test **déclencherait** la calibration au lieu de la
refuser, avec les conséquences chimiques décrites ci-dessus. À exécuter **après**
déploiement de la 2.46.1, et **seulement** après.

### Trois constats ouverts (non traités dans ce cycle)

1. **`RT,<tempC>`** ([`atlas_ezo.cpp:150`](../../../src/atlas_ezo.cpp:150)) —
   compensation de température du pH, **sans borne de plausibilité visible à ce
   niveau** : une température fausse fausse le pH **sans** toucher la calibration.
   Chemin que ni ce correctif ni `feature-070` ne couvrent. → [`bug-114`](../todo/bug-114-compensation-temperature-sans-borne-de-plausibilite.md), ouverte.
2. **Tamponnage JSON non authentifié** — `/history/import`,
   `/sensors/onewire/identify`, `/wifi/connect` : **16 Ko par requête** allouables sans
   authentification. Dégradation propre, mais vecteur réel pendant la famine mémoire de
   `bug-080`. → [`bug-113`](../todo/bug-113-tamponnage-json-non-authentifie.md), ouverte.
3. **Double-`send` préexistant** — requête **authentifiée** mais **au-dessus du quota** :
   le handler de corps répond `200`, puis le handler de requête répond `429`. Consigné en « hors
   périmètre » de [`bug-112`](../todo/bug-112-journal-non-borne-sur-route-protegee.md), sans spec propre.

### Critères d'acceptation — état final

- [x] `POST /save-config` non authentifiée ne modifie ni la RAM ni la NVS — livré en
      v2.25.0 (`saveConfigBodyAuthorized()`), confirmé par deux revues successives.
- [x] Flux de premier démarrage préservé — exemption `isFirstBootDetected()` conservée
      sur `/save-config` ; **volontairement absente** des trois routes de calibration,
      qui n'appartiennent pas à l'assistant.
- [x] **Autres routes à handler de corps auditées et résultat consigné** — 7 sites,
      **3 défauts confirmés et corrigés**, **4 sains** (dont `/save-config` comme
      référence), 1 réserve documentée sur `POST /update`, 3 constats ouverts.
- [x] Compilation et suite native vertes — `pio run -e esp32dev_core3` : **SUCCESS** (RAM 22,4 %, Flash 49,3 %) ; `pio test -e native` : **1418 cas, 1418 succès** (exécutés le 2026-09-07, code 0).
      Le volet **bi-core** de ce critère est **annulé** (ADR-0069, cf. ci-dessus) :
      `esp32dev_core3` est le seul environnement matériel.
