# bug-171 — Une pompe lancée en test échappe au blocage global, y compris pendant un étalonnage

## Métadonnées
- **ID** : bug-171
- **Slug** : une-pompe-en-mode-manuel-echappe-au-blocage-global
- **Type** : bug — 🔴 **sécurité chimique** : une pompe doseuse continue de tourner alors que
  toutes les conditions de sa suspension sont réunies
- **Statut** : todo
- **Priorité** : **haute** — ⚠️ **troue en partie feature-163**, livrée la veille
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-10-18
- **Spec(s) liée(s)** :
  `done/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md`
  (⭐ **même mécanisme** ; bug-170 a fermé l'entrée **non gardée**, ⛔ pas la propriété structurelle) ·
  `done/feature-163-refus-de-dosage-pendant-un-etalonnage.md` (c'est elle qui est trouée) ·
  ADR-0115 (une garde sur un état partagé vit au **point d'usage final**)

## Domaines touchés
- [ ] Capteurs
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP** — `/pumpN/on`
- [ ] MQTT · [ ] UART · [ ] OTA · [ ] Authentification · [ ] Historique · [ ] Frontend

## Agents requis
- [x] spec-reader
- [x] architect — ⚠️ il y a un **arbitrage réel** (voir « Options »)
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT**
- [x] **security-reviewer** — c'est lui qui l'a trouvé ; route HTTP + ordre des gardes
- [ ] ux-ui-designer — aucune surface UI
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

`POST /pumpN/on` démarre une pompe doseuse pour un **test**. La route est **correctement gardée à
l'entrée** (authentification, session d'étalonnage, présence d'eau). ⛔ **Mais une fois la pompe
démarrée, plus rien ne l'arrête** tant qu'un `/pumpN/off` explicite n'est pas envoyé.

## Le mécanisme, vérifié dans le code

`setManualPump()` pose `manualMode[i] = (duty > 0)`. Or les **trois** blocs de suspension de
`updateDosingCore()` (`pump_controller.cpp:3750`, `:3781`, `:3858`) coupent une pompe ainsi :

```cpp
if (!manualMode[0]) applyPumpDuty(0, 0);
```

⇒ ⛔ **Une pompe en mode manuel n'est jamais coupée par ces blocs.** Et seuls
`setManualPump(..., 0)` et l'OTA remettent `manualMode` à `false`.

### Ce qui est interrompu aujourd'hui, et ce qui ne l'est pas

| Chemin | Interrompu par la boucle ? | Interrompu à l'entrée en étalonnage ? |
|---|---|---|
| Injection manuelle (`/ph\|orp/inject/start`) | ✅ oui | ✅ oui (`web_routes_calibration.cpp`) |
| **Test de pompe (`/pumpN/on`)** | ⛔ **NON** | ⛔ **NON** |
| Assistant de mesure du débit | ✅ (timeout de session) | — |

## 🔴 Scénario de défaillance, concret

1. L'utilisateur lance un **test de pompe** filtration en marche. ✅ Les gardes d'entrée passent.
2. La filtration s'arrête — **horaire programmé** ou coupure.
3. ⛔ **La pompe doseuse continue à pleine puissance**, sans circulation : le produit **stagne au
   point d'injection**.

**Variante, et c'est celle qui fait mal** :
2'. L'utilisateur **ouvre un étalonnage** (sonde dans le tampon).
3'. ⛔ La pompe de test **continue de tourner** — alors que feature-163 vient précisément de
    garantir *« aucune injection pendant un étalonnage »*.

⚠️ **La garantie de feature-163 est donc exacte pour tout ce qui DÉMARRE pendant la session, et
fausse pour ce qui tournait DÉJÀ.** C'est écrit nulle part.

## ⚠️ Ce que ce bug n'est PAS
⛔ Ce n'est **pas** un défaut de garde d'entrée : `/pumpN/on` est bien gardée.
⛔ Ce n'est **pas** l'argument « présence d'eau » — ⚠️ l'utilisateur a rappelé, **à juste titre**,
que la présence d'eau n'est pas **mesurée** mais **présumée** (`ManagedFiltration` croit l'eau
présente dès que le relais est **commandé** ON ; `PoweredByFiltration` renvoie **toujours** `true`).
⇒ L'argument qui tient est : **l'appareil se croit à l'arrêt alors qu'une pompe tourne.**

## Options — ⚠️ à arbitrer, ⛔ pas à présumer

| | Option | Ce qu'on gagne | Ce qu'on paie |
|---|---|---|---|
| **A** | Retirer l'exemption `!manualMode[i]` des trois blocs | Simple, ferme tout | ⚠️ **L'assistant de mesure du débit EN DÉPEND** (`pump_controller.cpp:3860-3863`) — il serait cassé |
| **B** | **Borner `/pumpN/on` dans le temps** côté serveur + l'interrompre comme une injection manuelle | ⭐ Patron **déjà en place** pour `manualInject*` ; l'assistant garde son exemption | Un peu plus de code ; durée max à faire fixer par `pool-chemistry` |
| **C** | Interrompre le test **uniquement** à l'entrée en étalonnage | Minimal | ⛔ Laisse le cas « filtration arrêtée » ouvert |

⚠️ **`security-reviewer` recommande (B)** : *« la plus proche des patrons existants »*.

## Comportement attendu
1. Une pompe lancée en test ⛔ **ne survit pas** à une perte de présence d'eau.
2. Elle ⛔ **ne survit pas** à l'ouverture d'une session d'étalonnage.
3. ⚠️ L'**assistant de mesure du débit** continue de fonctionner — ⛔ son exemption est **voulue**.
4. L'arrêt (`/pumpN/off`, `duty=0`) reste **toujours** possible, sans condition.
5. L'interruption est **journalisée**, ⛔ jamais muette.

## Éléments visibles
- [x] **Log** — interruption d'un test
- [ ] Endpoint HTTP (⚠️ comportement changé, ⛔ pas la route)
- [ ] UI · [ ] WS · [ ] MQTT

## Critères d'acceptation
- [ ] Test lancé, puis **filtration arrêtée** ⇒ pompe **coupée**, interruption **journalisée**
- [ ] Test lancé, puis **étalonnage ouvert** ⇒ pompe **coupée**
- [ ] ⛔ L'assistant de mesure du débit **n'est pas cassé** (c'est le piège de l'option A)
- [ ] `/pumpN/off` reste inconditionnel
- [ ] **Mutation** : rétablir l'exemption doit **tuer** un test
- [ ] `pio run` 0 warning · natifs sans régression

## Hors périmètre
- Les routes `/pumpN/duty/:duty` — **supprimées**, c'est bug-170
- La nature **présomptive** de la présence d'eau — ⚠️ matériel, voir `kicad/README.md`
