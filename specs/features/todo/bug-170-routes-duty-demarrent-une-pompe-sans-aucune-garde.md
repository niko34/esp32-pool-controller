# bug-170 — `/pumpN/duty/:duty` démarre une pompe doseuse SANS AUCUNE garde chimique

## Métadonnées
- **ID** : bug-170
- **Slug** : routes-duty-demarrent-une-pompe-sans-aucune-garde
- **Type** : bug — 🔴 **sécurité chimique** : une injection peut partir **sans circulation d'eau**
- **Statut** : todo
- **Priorité** : **critique** — ⚠️ viole la **règle de sécurité n°1** de `CLAUDE.md` :
  *« TOUJOURS vérifier la présence d'eau (filtration active) avant toute injection »*
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-17
- **Spec(s) liée(s)** : `doing/feature-163-refus-de-dosage-pendant-un-etalonnage.md`
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
- [ ] `POST /pump1/duty/200` **filtration arrêtée** ⇒ **refusé**, pompe ⛔ **non démarrée**
- [ ] Même requête pendant une **session d'étalonnage** ⇒ refusée (feature-163)
- [ ] ⛔ **Aucune divergence possible** avec `/pumpN/on` : garde **partagée**, pas recopiée
- [ ] `docs/API.md` corrigé — ⚠️ il documente aujourd'hui une route **sans mentionner** qu'elle
      ne vérifie rien
- [ ] **Mutation** : retirer la garde doit **tuer** un test
- [ ] `pio run` 0 warning · natifs sans régression

## Hors périmètre
- La garde d'étalonnage elle-même — c'est feature-163
- ⚠️ La **mise à 0** d'une pompe (`duty=0`) doit rester **toujours permise** : c'est un **arrêt**,
  ⛔ jamais une injection.
