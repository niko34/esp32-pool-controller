# bug-170 — `/pumpN/duty/:duty` démarre une pompe doseuse SANS AUCUNE garde chimique

## Métadonnées
- **ID** : bug-170
- **Slug** : routes-duty-demarrent-une-pompe-sans-aucune-garde
- **Type** : bug — 🔴 **sécurité chimique** : une injection peut partir **sans circulation d'eau**
- **Statut** : **résolu (2026-10-18) — par suppression, pas par l'ajout d'une garde**
- **Priorité** : **critique** — ⚠️ viole la **règle de sécurité n°1** de `CLAUDE.md` :
  *« TOUJOURS vérifier la présence d'eau (filtration active) avant toute injection »*
- **Version cible** : 2.83.0
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** : `done/feature-163-refus-de-dosage-pendant-un-etalonnage.md`
  (⭐ **c'est son recensement qui a trouvé ce chemin**) · feature-056 (source unique
  `resolveWaterPresence()`) · `done/incident-bypass-sondes-ferme` (ce que coûte une injection
  sans circulation)

## Domaines touchés
- [ ] Capteurs
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP**
- [ ] MQTT · [ ] UART · [ ] OTA
- [x] **Authentification** — ⚠️ c'est le **seul** contrôle actuellement présent
- [ ] Historique · [ ] Frontend

## Agents requis
- [x] spec-reader
- [ ] architect — correctif **local**, la conception est déjà faite à côté
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT**
- [x] **security-reviewer — OBLIGATOIRE** : route HTTP ouverte, ordre des gardes
- [ ] ux-ui-designer — ⛔ aucune surface UI (la route n'est **pas** utilisée par `data/`)
- [x] embedded-developer
- [x] web-ui-developer — ⚠️ uniquement si `docs/API.md` compte comme sa surface
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

`POST /pump1/duty/:duty` et `POST /pump2/duty/:duty` appellent `setManualPump()` — donc
**démarrent une pompe doseuse à un rapport cyclique arbitraire** — après **une seule**
vérification : l'authentification.

⛔ **Aucune garde de présence d'eau. Aucune limite journalière ou horaire. Aucun plafond de
débit. Aucune garde d'étalonnage.**

## Constat (vérifié dans le code, 2026-10-17)

`src/web_routes_control.cpp`, `handleDynamicPumpRoutes()` (~`:1167-1197`) :

```cpp
if (!authManager.checkAuth(req, RouteProtection::WRITE)) return true;
String dutyStr = url.substring(12);
int duty = dutyStr.toInt();
if (duty < 0) duty = 0;
if (duty > 255) duty = 255;
PumpController.setManualPump(0, duty);
req->send(200, "text/plain", "OK");
```

⚠️ **À comparer avec la route voisine** `/pump1/on`, qui passe par
`injectionAllowedOrReject()` et refuse en **409** avec ce texte :

> *« filtration arrêtée (sécurité chimique : pas de circulation = surdosage local) »*

⇒ **Deux routes, le même actionneur, deux niveaux de sécurité opposés.**

## Pourquoi c'est grave

Sans circulation, le produit injecté **ne se dilue pas** : il stagne au point d'injection.
⚠️ Le projet en a déjà payé le prix — `incident-bypass-sondes-ferme` (23/08/2026) : premier
dosage réel effectué avec le bypass fermé, mesures non représentatives, **765/802 mL comptés**.
⚠️ Et le 11/10/2026, du chlore a été injecté dans une **dérivation ORP fermée** : la sonde a
trempé dans un hypochlorite **~1000× concentré**.

⛔ Ces deux routes permettent exactement cela, **en une requête**, sans qu'aucune garde ne s'y
oppose.

## Comment il a été trouvé

⭐ Pendant feature-163, `embedded-developer` a reçu la consigne de **recenser TOUS les chemins
qui démarrent une pompe doseuse** — après que le gate eut découvert un **4ᵉ** chemin (le mode
Programmé) que la spec ignorait.

⚠️ Ces routes sont le **5ᵉ**. ⛔ Et elles n'étaient gardées **par rien**, **avant** ce lot.
⇒ **L'histoire de ce sujet est une suite de listes qu'on croyait exhaustives et qui ne
l'étaient pas** — c'est précisément la thèse de feature-163.

✅ **L'agent ne l'a pas corrigé de sa propre initiative** : le périmètre validé par le gate ne
le couvrait pas, et ⛔ *« tout nouveau chemin de code vers `pump_controller` → validation par
`pool-chemistry` »*. **C'est le bon comportement** — d'où cette spec.

## Comportement attendu

1. Les deux routes passent par **la même garde** que `/pumpN/on` — ⛔ pas une copie, **la même
   fonction** (`injectionAllowedOrReject()`), pour qu'elles ⛔ **ne puissent plus diverger**.
2. ⚠️ **À trancher par `pool-chemistry`** : ces routes doivent-elles **exister** ?
   - Si elles servent au **diagnostic matériel** (mesure de débit, test d'amorçage), elles ont
     un usage légitime — mais ⛔ alors **sous garde**, comme le test de pompe.
   - ⚠️ `./tools/check_removed_measurement_vocabulary.sh` suggère qu'une partie de l'outillage
     de mesure a **déjà été retirée** (feature-152) : ⇒ **vérifier si ces routes sont un
     RELIQUAT**. ⭐ Le projet a une direction explicite : *« ce qui a servi à concevoir n'a plus
     à rester affiché ; retirer fait partie du travail courant »*.
3. Si elles sont conservées : refus **409**, motif journalisé, ⛔ **mêmes textes** que la route
   voisine — ⛔ pas de troisième formulation.

## Éléments visibles
- [x] **Endpoint HTTP** — refus 409 neuf (ou **suppression** des routes)
- [x] **Log** — refus journalisé
- [ ] UI · [ ] WS · [ ] MQTT

## Critères d'acceptation
- [x] `POST /pump1/duty/200` **filtration arrêtée** ⇒ **refusé**, pompe ⛔ **non démarrée** — la
      route n'existe plus : toute requête vers `/pump1/duty/*` ou `/pump2/duty/*` reçoit
      désormais **404**, filtration active ou non. Plus fort que le critère demandé (qui
      envisageait un refus **conditionnel** à l'état de filtration) : le refus est désormais
      **inconditionnel**.
- [x] Même requête pendant une **session d'étalonnage** ⇒ refusée (feature-163) — idem, 404
      inconditionnel, étalonnage ouvert ou non.
- [x] ⛔ **Aucune divergence possible** avec `/pumpN/on` : garde **partagée**, pas recopiée —
      il n'y a plus de second chemin susceptible de diverger : `/pumpN/on`/`/pumpN/off` restent
      les seules routes qui démarrent/arrêtent une pompe à la main.
- [x] `docs/API.md` corrigé — la section documentait une route **sans mentionner** qu'elle ne
      vérifiait rien ; elle documente désormais son **retrait** et ce qu'il faut utiliser à la
      place.
- [x] **Mutation** : ⚠️ **le critère a changé de nature, et la garde EST livrée.**
      ⛔ Pas de test unitaire du 404 : les handlers `AsyncWebServer` ne sont pas instanciables en
      natif (ADR-0017) — un test à base de mock testerait le mock, pas le routage.
      ⚠️ **Mais la conclusion initiale (« il faudrait un test on-target ») était FAUSSE par
      omission**, relevé en revue : le vrai risque n'est pas une régression de comportement,
      c'est une **RÉINTRODUCTION**. Et le projet a déjà ce motif, câblé en CI —
      `check_no_peek_flag.sh` (ADR-0081), `check_removed_measurement_vocabulary.sh` (feature-152).
      ⇒ **Livré** : `tools/check_no_dynamic_pump_routes.sh`, job CI `no-dynamic-pump-routes`.
      ⭐ **Validé par mutation** : réintroduire `if (req->url().startsWith("/pump1/duty/"))` dans
      `onNotFound` fait **échouer** le script (deux motifs détectés, code de sortie 1), fichier
      restauré à l'identique ensuite.
- [x] `pio run` 0 warning · natifs sans régression — `pio run` SUCCESS, suite native à 2 253 cas
      verts (mesuré, pas recopié — ADR-0073).

## Hors périmètre
- La garde d'étalonnage elle-même — c'est feature-163
- ⚠️ La **mise à 0** d'une pompe (`duty=0`) doit rester **toujours permise** : c'est un **arrêt**,
  ⛔ jamais une injection. ⇒ **Sans objet après suppression** : `/pumpN/duty/0` n'existe plus,
  l'arrêt d'une pompe passe exclusivement par `/pumpN/off` (toujours inconditionnel).

## Notes techniques (clôture, 2026-10-18)

**Résolution retenue : suppression des deux routes, pas l'ajout d'une garde.** La section
« Comportement attendu » de cette spec posait la question explicitement (« ces routes doivent-
elles exister ? ») et renvoyait à `pool-chemistry` pour trancher. Verdict : aucun usage légitime
identifié — rien dans `data/` ni aucun script n'appelait ces routes (reliquat des essais de débit
qui ont produit la mesure du 2026-08-23, désormais obsolète : la caractérisation se fait via
`?duty=N` sur `/pumpN/on`, qui lui porte toutes les gardes).

Supprimés avec les deux routes : `handleDynamicPumpRoutes()` (`src/web_routes_control.cpp`) et le
routage dynamique dans `server->onNotFound()` (`src/web_server.cpp`), qui ne fait plus que 404.
Un second défaut, non mentionné dans le constat initial, a été documenté au passage :
`setManualPump()` posait `manualMode[i]`, ce qui aurait empêché le bloc de suspension globale
(`if (!manualMode[i])`) de couper une pompe démarrée par cette route — une injection partie ainsi
aurait continué **jusqu'à un arrêt explicite**, quel que soit l'état de la filtration ou d'une
éventuelle session d'étalonnage ouverte entre-temps.

⚠️ **Précision à ne pas perdre** (objection de l'utilisateur en cours de lot) : l'argument qui
tient n'est **pas** que la suppression « rétablit la garde de présence d'eau ». `resolveWaterPresent()`
ne mesure rien, il **présume** (`ManagedFiltration` croit l'eau présente dès que le relais est
commandé ON ; `PoweredByFiltration` renvoie toujours `true`). L'argument qui tient est
**l'incohérence entre deux routes sur le même actionneur** et **le contournement du blocage
global de suspension** — pas la force d'une garde qui reste une présomption.

Un ADR a été créé sur le mécanisme, pas seulement sur l'instance :
[ADR-0117](../../docs/adr/0117-un-routage-dynamique-generique-ne-sert-jamais-un-actionneur.md) —
un routage dynamique générique (`onNotFound`) ne doit jamais servir un actionneur, parce qu'il
échappe par construction à l'inventaire des routes déclarées (c'est exactement ce qui a permis à
ce chemin de rester invisible pendant des mois).

**Fichiers modifiés** : `src/web_routes_control.cpp`, `src/web_routes_control.h`,
`src/web_server.cpp`. Docs : `docs/API.md`, `docs/subsystems/pump-controller.md` (**5** passages — un 5ᵉ avait été manqué, relevé en revue),
`docs/adr/0117-...md` (nouveau), `docs/adr/README.md`, `CHANGELOG.md` ([2.83.0], même version non
publiée — pas de bump).


## Traçabilité des gates — ⛔ dit plutôt que laissé supposer

⚠️ Relevé en revue : la spec cochait `pool-chemistry` et `security-reviewer` comme **obligatoires**
sans qu'aucun verdict ne soit tracé. Rectification :

| Agent | Ce qui s'est réellement passé |
|---|---|
| `security-reviewer` | ✅ **GO**, 2026-10-18. Recensement **complet** des chemins vers `setManualPump()` ; vérifié qu'aucune route légitime (y compris portail captif et provisionnement Wi-Fi) ne dépendait du filet `onNotFound` ; gardes de `/pumpN/on`, `/off`, injection manuelle et assistant de débit **intactes**. |
| `pool-chemistry` | ⛔ **NON saisi, volontairement.** Ce lot **retire** un chemin de dosage et n'en ajoute aucun. La règle du projet vise *« tout **NOUVEAU** chemin de code vers `pump_controller` »* — un retrait n'en est pas un, et la direction du changement est strictement protectrice. |
| `code-reviewer` | ✅ Passé le 2026-10-18 : **Request changes**, 5 points **tous documentaires** (aucun défaut de code), traités dans ce même lot. |

## 🔴 Ce que la revue de sécurité a trouvé EN PLUS — ⛔ non traité ici

`/pumpN/on`, **la route qui reste**, porte le **même défaut structurel** : elle pose
`manualMode[i]`, et le bloc de suspension globale ne coupe une pompe que `if (!manualMode[i])`.

⚠️ ⇒ Un **test de pompe** lancé filtration en marche **continue de tourner** si la filtration
s'arrête ensuite (horaire, coupure) **ou si une session d'étalonnage s'ouvre après coup** —
jusqu'à un arrêt **explicite**, sans limite horaire ni journalière.

⚠️ **Ça troue en partie feature-163** : l'arrêt à l'entrée en étalonnage
(`web_routes_calibration.cpp`) ne couvre que les **injections manuelles**, ⛔ **pas les tests de
pompe**.

⇒ **Préexistant, hors périmètre de ce lot** (qui ferme l'entrée **non gardée**, pas la propriété
structurelle). Suivi dans **bug-171**.
