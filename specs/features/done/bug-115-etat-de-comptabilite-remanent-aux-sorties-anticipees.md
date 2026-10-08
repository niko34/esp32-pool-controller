# bug-115 — L'état de comptabilité reste rémanent aux sorties anticipées : la durée d'une coupure est créditée au débit plateau

## Métadonnées
| | |
|---|---|
| Type | bug (comptabilité des volumes → fiabilité d'une mesure de terrain) |
| Statut | **implémenté (v2.61.0) — tests verts, revue OK. Déploiement différé** : la fenêtre de surveillance de `feature-136` (déployée le 2026-10-08 à 20:06, 3-5 jours + deux événements provoqués) doit se clore **avant** le déploiement de ce correctif, car les deux lots déplacent la quantité délivrée en sens opposés (condition bloquante `pool-chemistry`). Ne pas déployer avant le **2026-10-11** au plus tôt. |
| Priorité | Haute **sur le fond**, mais **NE PAS IMPLÉMENTER avant la fin de la campagne de bidon** (voir ci-dessous) |
| Version cible | à définir — **après** la campagne |
| Relevé par | `code-reviewer`, 2026-09-07, pendant le cycle `feature-110` |
| Specs liées | `feature-110` (les compteurs qui l'ont révélé), `feature-078` (la campagne qu'il pollue), `bug-116` (même chaîne de sorties) |

## ⏰ ÉTAT DU VERROU au 2026-10-08 — échéance imminente

Relevé sur l'appareil le **2026-10-08** :

| | Valeur |
|---|---|
| `orp_remaining_ml` | **2 056 mL** sur 20 000 — soit ~17 944 mL comptés |
| `orp_starts` | **355** |
| `orp_anom` / `orp_anom_ml` | **0 / 0** — campagne **non polluée** |

À 939-1 379 mL/jour, le changement de bidon est à **un ou deux jours**. C'est la
**deuxième** mesure de concordance, celle que la première (08/09, 86 démarrages)
demandait d'attendre : **355 démarrages**, soit un bras de levier **quatre fois
meilleur** pour départager les deux modèles de biais.

Les compteurs d'hygiène à zéro signifient que **le défaut décrit par cette spec
n'a pas pollué la campagne en cours** : aucun tic de reprise avec un `deltaMs`
anormal n'a été enregistré. Le relevé à venir sera donc de bonne qualité.

**Décision du 2026-10-08 (utilisateur)** : préparer la spec, le plan d'architecture
et le gate `pool-chemistry` **maintenant**, mais **ne rien écrire dans la
comptabilité des volumes** avant le relevé au jerricane. Implémenter plus tôt
détruirait cette campagne propre à un ou deux jours de son terme.

**Condition de levée** : relevé effectué (`orp_container_ml`,
`orp_total_injected_ml`, `orp_starts`, restant **mesuré au doseur**) **avant** le
clic sur l'action de remise à niveau, qui efface la mesure.
⚠️ **Elle s'appelle « Mettre à jour le niveau » depuis la v2.59.0** (feature-143) — « Nouveau
bidon » n'existe plus. Même action : elle pose le volume ET remet les compteurs à zéro.

## ⚠️ VERROU DE CALENDRIER — lire avant tout
**Ne pas implémenter tant que la campagne de concordance jerricane n'est pas close.**
Toucher à la comptabilité des volumes **invaliderait la campagne de référence en
cours**, qui démarre au prochain changement de bidon et doit produire le couple
`(V, N)` permettant de départager les deux modèles de biais de comptage.
C'est un verrou **délibéré**, pas un oubli. Voir `feature-110` § Sécurité chimique.

## Description
Les sorties anticipées de `PumpControllerClass::update()` — eau absente, stabilisation
en cours, assistant de débit (`flowAssistSuspend`), capteurs non initialisés — forcent
`applyPumpDuty(*, 0)` mais **reviennent avant la comptabilité**.

Or `_acctPumpOn[]` et `st.lastSafetyTimestamp` ne sont écrits **que** dans
`accountPumpVolume()`. Ils restent donc **rémanents** pendant toute la fenêtre aveugle.
À la reprise :
- `deltaMs` vaut **la durée entière de la coupure** ;
- `pulseDeltaVolumeMl` crédite **tout** ce delta au **débit plateau** ;
- côté compteur de démarrages, `prevOn` est resté `true` → le redémarrage physique
  **n'est pas compté**.

## Contexte — les ordres de grandeur, mesurés
Au débit plateau relevé (68 mL/min) : **5 min de coupure → ~5,7 mL · 1 h → ~68 mL ·
6 h → ~408 mL**.

Les **+240 mL** (+4,76 %) mesurés à la concordance du 06/09/2026 valent **3,5 minutes**
de crédit fantôme cumulé. Quelques reprises suffiraient donc à expliquer *tout* le
biais observé, **sans qu'aucun déficit de démarrage n'existe** — et l'assistant de débit
a été utilisé sur la fenêtre 23/08-31/08 de cette campagne.

**Les deux biais poussent δ dans le même sens** : `V` surcompte **et** `N` sous-compte.
Un déficit fixe par démarrage est donc **systématiquement favorisé** par l'asymétrie.

## Ce qui atténue déjà — et pourquoi ça ne suffit pas
Les compteurs d'hygiène livrés par `feature-110` (`*_anom`, `*_anom_ml`) **captent** ce
cas : le tick de reprise a un `deltaMs` de plusieurs ordres au-dessus de
`kAcctDeltaAnomalyMs = 5000`. Ils permettent de **disqualifier** un relevé pollué
(critère `pool-chemistry` : `anom_ml / V_bidon > 1,5 %` → non concluant).
Mais ils **signalent** le défaut ; ils ne le corrigent pas.

## Comportement attendu
Purger l'état de comptabilité dans les branches de sortie anticipée : à la reprise, le
delta doit repartir de la reprise, pas de l'arrêt, **et** le redémarrage physique doit
être compté comme un démarrage.

## Sécurité chimique
- Dosage impliqué : **oui** — c'est la comptabilité des volumes injectés.
- `pool-chemistry` **obligatoire** : la correction change les volumes rapportés, donc
  la base de toute conclusion ultérieure sur le biais.

## Critères d'acceptation
- [x] La campagne de concordance en cours est **close** avant toute implémentation
      — relevé au jerricane fait le 2026-10-08 (2,85 L / 18 221,21 mL comptés / 356
      démarrages, `orp_anom = 0`), compteurs remis à zéro ensuite
- [x] À la reprise après chaque sortie anticipée, aucun volume fantôme n'est crédité
      — point de passage unique `settlePumpAccounting()`, `nextOn = onNow` toujours
      (`acct_window_logic.h`)
- [x] Le redémarrage physique est compté comme un démarrage
      — couvert par `test_D1_seq_on3_coupure6h_reprise_zero_fantome_deux_demarrages`
- [x] Test natif de chaque branche de sortie, **validé par mutation** — réalisé
      sous la forme **structurellement équivalente** actée par le plan
      d'architecture : `pump_controller.cpp` n'étant pas compilable nativement,
      la garantie « chaque sortie règle sa comptabilité » est structurelle (le
      wrapper d'`update()`), couverte par la garde CI
      `tools/check_acct_settle_single_point.sh` (deux contrôles, chacun éprouvé en
      mutant le code en revue). Le contrat pur `evaluateAcctWindow()` et ses
      séquences, eux, sont testés nativement et par mutation dans
      `test/test_native_acct_window/`.
- [ ] Les compteurs d'hygiène retombent à zéro sur les cas ainsi corrigés —
      **vérifiable seulement après déploiement** ; différé par la fenêtre de
      surveillance de `feature-136` (pas avant le 2026-10-11)

## Notes pour agent
Un commentaire au-dessus de `composeDose()` (`src/pump_controller.cpp`, livré en
v2.46.0) nomme déjà les trois sorties anticipées pour le défaut voisin `bug-116`.
**Les deux traces doivent rester au même endroit** : quiconque touche à `update()` doit
trouver les deux avertissements côte à côte, sinon il rejouera l'un ou l'autre.


---

## Gate `pool-chemistry` P1 (conception) — 2026-10-08 : **GO SOUS CONDITIONS**

### Le principe de la spec est à CORRIGER

La spec dit « purger l'état de comptabilité ». Le gate **rejette cette forme** et impose
l'inverse : **appeler `accountPumpVolume()`** dans chaque sortie anticipée, juste après
`applyPumpDuty(…, 0)`.

Motif : un reset aveugle de `_acctPumpOn[]` / `lastSafetyTimestamp` **perdrait le volume
réellement injecté**. Deux sites le rendent inacceptable —
- `flowAssistSuspend` laisse tourner le mode **manuel** (`pump_controller.cpp:3306-3307`
  ne coupe pas `manualMode`) : c'est précisément la marche de mesure de l'assistant de débit ;
- OTA et maintenance coupent **en plein pouls**.

En appelant le comptable, le delta réel est crédité sur un tick, puis `onNow` dérivé de
`pumpDuty` met l'état à OFF de lui-même. Cela répond aussi au cas limite 3b : le volume
est **compté exactement**, ni perdu ni majoré.

### La chaîne n'est PAS neutre pour le dosage

`creditInjectedVolume()` écrit `safetyLimits.daily{Ph,Orp}InjectedMl`, lu comme **frein**
en cinq endroits (`pump_controller.cpp:2863`, `:2880`, `:4148`, `:4633`, et `:1977` pour le
gel de l'intégrale à 0,80× la limite). **Moins de surcompte ⇒ freins plus tardifs ⇒ plus
de produit réellement injecté** sur un jour affecté.

- **En nominal : ≈ 0 mL de différence.** Le fantôme n'arme que si la coupure tombe
  *pendant* `pumpDuty > 0` — d'où `orp_anom = 0` sur 355 démarrages et 17 944 mL.
- **En queue de distribution** : un arrêt nocturne de filtration attrapant un pouls rend
  **408 à 950 mL** au budget du jour (6-14 h × 68 mL/min).

⚠️ **CORRECTION du chiffrage du gate** — il a raisonné sur les **défauts d'usine**
(300 mL/j pH, 500 mL/j ORP) et en conclut qu'une occurrence de 4,4 h consommerait « la
journée entière » de pH. **Relevé sur l'appareil le 2026-10-08, les limites vivantes sont
`max_ph_ml_per_day = 2000` et `max_orp_ml_per_day = 2000`.** Le pire cas représente donc
**20 à 48 % du budget journalier**, pas 100 %. C'est matériel, ce n'est pas catastrophique
— et c'est très exactement le piège « config d'usine ≠ config vivante » que le gate
signalait lui-même en condition 7. Sa condition reste valable, son chiffre non.

### Conditions bloquantes (résumé — voir le rapport complet pour le détail)

1. **Appeler `accountPumpVolume()`**, ne pas purger.
2. **Aucun site de comptage de démarrage ajouté** : `evaluatePumpStartAccounting` sur le
   front OFF→ON de `pumpDuty` reste l'unique juge. Le redémarrage est compté *par
   construction*, pas par un `starts++` ad hoc.
3. **Couvrir les CINQ sorties** : OTA (`:3150`), maintenance (`~:3161`),
   `!sensors.isInitialized()` (`:3256`), et le triplet eau / stabilisation /
   `flowAssistSuspend` (`:3285`). Une seule oubliée rejoue le défaut.
4. **Aucun traitement spécial des coupures longues** — avec la condition 1, une coupure de
   6 h n'est plus *un* delta de 6 h mais une suite de ticks à `_acctPumpOn = false`. Un
   seuil de durée recréerait une zone grise.
5. **Battement toléré** : chaque front est un vrai arrêt/démarrage moteur. Vérifier
   qu'aucun chemin ne compte un démarrage quand la pompe n'a pas été coupée (manuel
   traversant la fenêtre : `onNow` reste vrai, donc aucun front).
6. **Rupture de série à déclarer** : δ mesuré avant et après n'est pas comparable.
   Estampiller la version firmware sur le relevé jerricane, et repartir sur un bidon neuf
   après livraison.
7. **Confirmer les plafonds avec l'utilisateur avant livraison** — fait pour la lecture :
   2 000 mL/j des deux côtés. Reste à confirmer que c'est bien le plafond chimique voulu.
   Surveillance de `daily*InjectedMl` sur 7 jours après déploiement.
8. **Conserver `*_anom` / `*_anom_ml`** : ils gardent leur rôle sur les ticks longs
   d'origine loop/OTA. Attendre leur retombée à 0 sur ce motif, pas leur suppression.
9. **Verrou de calendrier respecté** : aucune ligne avant le relevé au doseur.

### bug-116 : à SÉPARER

Avis chimique, pas de commodité. bug-115 touche le chemin d'**écriture** des compteurs de
sécurité et modifie le volume réellement injecté en queue de distribution ; bug-116 est de
la **lecture** seule. Les fusionner rendrait la semaine de surveillance illisible — un
volume inattendu ne serait plus imputable à l'un ou à l'autre — et brouillerait la
validation par mutation. bug-116 **après** la surveillance.

Les deux commentaires au-dessus de `composeDose()` restent côte à côte : celui de bug-115
devient « corrigé — ne pas rejouer », celui de bug-116 inchangé.

### Tests exigés (mutation obligatoire)
- Une suite native par sortie anticipée : coupure 5 s / 1 h / 6 h ⇒ volume crédité = celui
  du seul tick de sortie, `anom = 0`.
- Coupure **pendant un pouls** ⇒ volume partiel crédité, **1** démarrage à la reprise.
- `flowAssistSuspend` avec pompe en **manuel** ⇒ volume intégralement compté (mutant : un
  reset aveugle doit tuer le test).
- Enchaînement sortie/reprise/sortie × 10 ⇒ exactement 10 démarrages.
- Mutants à tuer : appel supprimé dans une seule branche · `onNow` forcé à `false` ·
  `starts++` déplacé hors de `evaluatePumpStartAccounting`.


---

## Plan d'architecture — 2026-10-08 (à exécuter le jour de la levée du verrou)

### Décision structurante : point de passage UNIQUE, pas purge par branche

`update()` est renommé `updateDosingCore(now)` — **corps non réindenté, les quatre
`return` intacts** — et un nouveau `update()` de six lignes l'enveloppe :

    now = millis(); _acctSettledThisTick = false;
    updateDosingCore(now);
    if (!_acctSettledThisTick) settlePumpAccounting(now);

Toute sortie **présente ou future** est comptabilisée **par construction**. Même motif
qu'ADR-0070 pour `fopen` : il n'y a plus rien à ne pas oublier.

⚠️ **Et surtout : une purge aveugle serait FAUSSE.** Les branches « capteurs non
initialisés » et « eau absente / stabilisation / flow-assist » **ne coupent pas les pompes
en mode manuel**. Une pompe manuelle tourne réellement pendant la fenêtre ; y poser
`_acctPumpOn = false` **sous-compterait** le registre journalier de sécurité — défaut
fail-**dangereux**, pire que celui qu'on corrige. Le point unique lit `pumpDuty[]` après
`applyPumpDuty` et fait la bonne chose sans avoir à la connaître.

**Bénéfice collatéral** : aujourd'hui le dernier tick ON avant la coupure est perdu. Le
point unique le crédite — micro-sous-compte supprimé au passage.

### Module pur `src/acct_window_logic.h` (ADR-0017)

    AcctWindow evaluateAcctWindow(bool prevOn, uint32_t lastTsMs, bool onNow, uint32_t nowMs);
    // nextOn          = onNow            ← TOUJOURS défini : la rémanence devient impossible
    // nextTimestampMs = onNow ? nowMs : 0
    // soustraction NON SIGNÉE → repliement millis() à 24,8 j sûr

Mutants qu'il tue réellement : `nextTimestampMs = nowMs` (le volume fantôme réapparaît) ·
`nextOn = prevOn || onNow` (le démarrage de reprise disparaît) · soustraction signée
(repliement à 24,8 j — défaut **déjà rencontré sur ce projet**).

⚠️ **Honnêteté sur le périmètre** : `pump_controller.cpp` n'est pas compilable nativement,
donc **aucun test natif ne peut démontrer que le settle est atteint sur les quatre
sorties**. Cette garantie est **structurelle** (le wrapper), pas testée. Filet proposé pour
`hygiene-specs.yml`, dans l'esprit de `check_fopen_guard.sh` : vérifier que
`accountPumpVolume(` n'apparaît qu'aux deux points attendus.

### Compteurs d'hygiène : conservés tels quels, valeur de preuve ACCRUE

`kAcctDeltaAnomalyMs = 5000` intouché. Après correctif, un `anom` non nul ne signale plus
un défaut connu mais un **chemin inconnu** (blocage de loop, gel I²C, sortie future non
prévue). Seul changement : **documentaire** — ils passent de *détecteur d'un défaut connu*
à *canari d'un chemin inconnu*. Toucher au seuil détruirait la comparabilité du critère de
disqualification `anom_ml / V_bidon > 1,5 %`.

### ⚠️ Découverte — une prémisse de feature-075 devient fausse

`src/web_routes_control.cpp:276-354` contient **trois** mécanismes bâtis explicitement sur
« sous suspension flow-assist, `update()` sort AVANT `accountPumpVolume()` ». Ce plan rend
cette phrase fausse. Vérifié :
- **Pas de double crédit** sur le registre de sécurité : `creditManualInjectionFloor` est un
  **plancher** (`if (daily < floorMl)`), que la comptabilité par tick rend simplement no-op.
- **En revanche le suivi produit (bidon) recevra désormais** le volume des marches
  d'assistant (purge + marche de mesure), aujourd'hui invisible pour lui. C'est plus fidèle
  — le produit a réellement quitté le bidon — mais **`orp_total_injected_ml` change de
  signification**. À soumettre au gate P2.
- Ne pas supprimer le refus 409 ni le crédit prorata : hors périmètre, défense en profondeur.
  Seulement corriger leurs commentaires.

### ⚠️ Discontinuité métrologique — à acter dans l'ADR

À firmware corrigé, **N augmente** (les reprises comptent) et **V diminue** (plus de crédit
fantôme) : **δ change de valeur sans que le matériel ait bougé**. Il est **interdit** de
comparer un relevé post-correctif à la campagne en cours. **La campagne jerricane qui
s'achève est la DERNIÈRE du modèle actuel.**

### ADR attendu — ⚠️ PAS 0088, 0089 ni 0090, TOUS TROIS pris

**0088 est occupé** par `docs/adr/0088-diffusion-websocket-ciblee-sur-registre-borne-sans-verrou.md`
(bug-081, livré le 2026-10-08 pendant que ce lot attendait son verrou de calendrier).
**0089 est occupé à son tour** par `docs/adr/0089-ce-qu-un-redemarrage-a-le-droit-doublier.md`
(feature-136, implémentée le 2026-10-08 — même jour, verrou de calendrier levé en premier
pour ce lot-là). Prendre **le prochain numéro réellement libre au moment de
l'implémentation** — **0091** à ce jour (2026-10-08 au soir), à revérifier par `ls docs/adr/`
plutôt qu'à recopier : trois ADR ont été posés dans la seule journée du 2026-10-08, et ce
numéro a déjà glissé deux fois pendant que cette spec attendait.
plutôt qu'à recopier d'ici : d'autres lots peuvent avoir pris un numéro entre-temps.
« La comptabilité des volumes est hors du corps décisionnel d'`update()` ». Décision
structurante *et* verrou : elle acte la discontinuité métrologique ci-dessus.

### Confirmation (doc-writer, 2026-10-08) — le plan d'architecture reste applicable sans rebase

feature-136 a été implémentée **en premier** (son verrou de calendrier à elle n'existait
pas ; celui de bug-115 — campagne jerricane en cours — reste lui en vigueur, **ne rien
implémenter avant le relevé au doseur**). Vérifié par lecture du diff de feature-136
(`src/pump_controller.{h,cpp}`) : **zéro ligne touchée** sur le périmètre que ce plan
suppose intact — `_acctPumpOn[]`, `lastSafetyTimestamp`, `accountPumpVolume()`,
`evaluatePumpStartAccounting`, et la structure d'`update()` (les quatre sorties
anticipées, leurs lignes, leur ordre). feature-136 a ajouté un bloc **après** le flush des
compteurs journaliers et **avant** la consommation des cycles manuels — un site disjoint de
celui que ce plan modifie (`update()` → `updateDosingCore()` + wrapper). Le plan
d'architecture ci-dessus reste donc applicable **sans rebase** au moment de la levée du
verrou.

---

## ⚖️ Conflit tranché : bug-116 est traité SÉPARÉMENT

**L'architecte recommande le même lot** (même chaîne de sorties ; le wrapper est justement
le crochet dont bug-116 a besoin ; éviter de toucher `update()` deux fois et de payer deux
gates).

**Le gate `pool-chemistry` recommande de séparer** : bug-115 touche le chemin d'**écriture**
des compteurs de sécurité et modifie le volume réellement injecté en queue de distribution ;
bug-116 est de la **lecture** seule. Les fusionner rendrait la semaine de surveillance
**illisible** — un volume inattendu ne serait plus imputable à l'un ou à l'autre — et
brouillerait la validation par mutation.

**Arbitrage (agent principal, 2026-10-08) : SÉPARÉS, bug-115 d'abord.** L'argument chimique
porte sur la **validité de la validation**, celui de l'architecte sur l'**économie de
cycle**. Quand un lot modifie le volume réellement injecté, pouvoir imputer une anomalie à
une cause unique pendant la fenêtre de surveillance prime sur le coût d'un second passage.
bug-116 sera repris **après** les sept jours de surveillance.

Conséquence à traiter dans bug-116 : son critère d'acceptation « aucune ligne du chemin de
dosage modifiée » devient caduc, puisque bug-115 en modifie la structure. **À réécrire dans
sa spec, pas à contourner.**


---

## ⚠️ Mise à jour du 2026-10-08 au soir — AVANT de relancer le cycle

### Le verrou de calendrier est LEVÉ
Le relevé au jerricane a été fait : **2,85 L mesurés** contre 18 221,21 mL comptés sur
**356 démarrages** (bidon de 20 000 mL **nominal**, l'utilisateur a confirmé qu'il s'agissait de
l'étiquette et non d'une mesure). Biais : **+1 071 mL, soit +6,25 %**. Campagne **non polluée**
(`orp_anom = 0`). Les compteurs ont ensuite été remis à zéro sur un bidon de 5 L **mesuré**.

➜ **Plus rien ne bloque l'implémentation.**

### MAIS le terrain a bougé : feature-136 a été livrée ET DÉPLOYÉE entre-temps
`feature-136` (v2.58.0, budget horaire persistant) touche `pump_controller.{h,cpp}` et
`main.cpp`. Son cycle a **vérifié par `git diff`** que le périmètre de bug-115 restait intact —
`_acctPumpOn[]`, `lastSafetyTimestamp`, `accountPumpVolume()`, `evaluatePumpStartAccounting` et
la structure d'`update()` : **zéro ligne touchée**. Le plan d'architecture ci-dessus devrait donc
rester applicable **sans rebase**.
⚠️ **À REVÉRIFIER quand même** : le plan a été écrit contre l'état du code du matin.
feature-136 a ajouté trois crochets dans `update()`, adjacents au bloc `_dailyCountersDirty` —
exactement la zone où bug-115 veut poser son `settlePumpAccounting()`.

### ⚠️ ET LE DÉPLOIEMENT RESTE BLOQUÉ, lui
Le gate `pool-chemistry` a posé comme **condition bloquante** que les fenêtres de surveillance de
feature-136 et bug-115 soient **strictement sérielles** : les deux lots déplacent la quantité
délivrée **en sens opposés** (feature-136 fait mordre les freins plus tôt, bug-115 rend 408 à
950 mL/jour), et *« une observation nette nulle ne permettrait pas de distinguer les deux
fonctionnent de les deux sont cassés »*.

**feature-136 a été déployée le 2026-10-08 à 20:06.** Sa fenêtre de surveillance est de
**3 jours pleins** (5 si un jour sort le pH de 7,20-7,50), **plus deux événements provoqués** :
un reboot OTA vérifiant un `usedMs` restauré **non nul**, et une coupure > 1 h.

➜ **IMPLÉMENTER maintenant : oui. DÉPLOYER : pas avant le 2026-10-11 au plus tôt**, et seulement
après clôture de la surveillance de feature-136.


## 📍 Lignes RÉELLES au 2026-10-08 au soir (le plan citait des estimations du matin)

Le fichier a dérivé de **+90 à +100 lignes** (feature-136 et les autres lots du jour).
**Utiliser ces lignes-ci, pas celles du plan.**

| Élément | Plan (matin) | **RÉEL (soir)** |
|---|---|---|
| `accountPumpVolume()` | ~2935-3004 | **2986-3055** |
| `PumpControllerClass::update()` | ~3028 | **3079** |
| Sortie **OTA** | ~3150 | `if` **3251**, `return` **3259** |
| Sortie **maintenance** | ~3161 | `if` **3265**, `return` **3274** |
| Sortie **`!sensors.isInitialized()`** | ~3256 | `if` **3357**, `return` **3370** |
| Sortie **eau / stabilisation / flowAssist** | ~3285 | `if` **3386**, `return` **3409** |
| Queue : appels normaux d'`accountPumpVolume()` | ~4815-4820 | **4920-4921** |
| Trace jumelle bug-115 / bug-116 (`composeDose`) | ~2148-2166 | **2199-2217** |
| `flowAssistStopRun()` | ~276 | **292** |
| `flowAssistCreditAbortedRun()` | ~308 | **321** |
| `flowAssistAllowsManualInjectOrReject()` | ~340 | **349** |

### ⚠️ CORRECTION — il y a QUATRE sorties anticipées, pas cinq

Le gate P1 du matin parle de **« couvrir les CINQ sorties »**. **C'est une imprécision interne
à cette spec.** Vérifié sur le code : `update()` compte **QUATRE `return` anticipés distincts**
— le triplet *eau absente / stabilisation / `flowAssistSuspend`* partage **un seul** `return`
(ligne 3409), il ne compte donc pas pour trois.

➜ Chercher une cinquième sortie serait une perte de temps, et en inventer une serait pire.
Le wrapper les couvre **toutes par construction**, quel qu'en soit le nombre : c'est précisément
l'argument qui a fait retenir le point de passage unique plutôt que la purge branche par branche.

### Confirmation : feature-136 ne gêne pas
`git show ad027f5 -- src/pump_controller.{cpp,h}` : **zéro occurrence** de `_acctPumpOn`,
`lastSafetyTimestamp`, `accountPumpVolume`, `evaluatePumpStartAccounting`. Les trois crochets
ajoutés par feature-136 (lignes **3179-3227**) vivent **avant** le `return` OTA et **ne créent
aucune sortie anticipée** : ils resteront entièrement à l'intérieur de ce qui devient
`updateDosingCore()`. **Le plan du wrapper est applicable sans rebase.**


## ⚠️ Recommandation du gate P2 ÉCARTÉE — vérification faite, le gate s'est trompé

Le gate `pool-chemistry` P2 relève, en **non bloquant**, que `settlePumpAccounting` serait
« passé de ~4920 à **:4981**, donc **après** `closePhDosingSession` (:4943) et
`_pulseSession = false` (:4953) », d'où un « log d'arrêt sous-déclaré d'un tick (< 0,1 mL) », et
il recommande de déplacer l'appel « juste après :4919 pour restaurer l'ordre d'origine ».

**Vérifié sur `git show HEAD:src/pump_controller.cpp` — l'ordre relatif est STRICTEMENT
IDENTIQUE avant et après :**

| | AVANT (HEAD) | APRÈS (v2.61.0) |
|---|---|---|
| `closePhDosingSession(now, "branche automatique suspendue…")` | 4879 | 4943 |
| `_pulseSession[0] = false` | 4889 | 4953 |
| `_pulseSession[1] = false` | 4906 | 4970 |
| **comptabilité** | `accountPumpVolume` **4920** | `settlePumpAccounting` **4981** |

La comptabilité était **déjà** après la fermeture de session et après les deux fins de pouls.
Le décalage de **+61 lignes** vient du code ajouté par feature-136 et par ce lot, **pas** d'un
déplacement relatif.

➜ **Le « sous-déclaré d'un tick » est un comportement PRÉEXISTANT**, pas une régression de
bug-115. Et l'exigence du plan — *« les deux `accountPumpVolume()` deviennent
`settlePumpAccounting(now)` À LA MÊME POSITION ⇒ ordonnancement du chemin nominal strictement
identique »* — **est tenue**.

**Aucun déplacement effectué.** Le gate a comparé des numéros de ligne **absolus** entre deux
versions d'un fichier qui a grossi de 60 lignes entre-temps. ⚠️ Si quelqu'un relit ce rapport
plus tard et « corrige » l'ordre sur sa foi, il **introduira** la régression que le gate croyait
voir.

---

## Notes d'implémentation (doc-writer, 2026-10-08)

**Livré en v2.61.0** : `src/pump_controller.{h,cpp}` (point de passage unique,
`update()`/`updateDosingCore()`/`settlePumpAccounting()`), `src/acct_window_logic.h`
(module pur), `test/test_native_acct_window/` (suite native),
`tools/check_acct_settle_single_point.sh` (garde CI, 2 contrôles) + job
`hygiene-specs.yml`, `src/web_routes_control.cpp` (commentaires corrigés, aucune
ligne de logique touchée). Doc : voir CHANGELOG `[2.61.0]`.

**Documentation mise à jour** : `docs/subsystems/pump-controller.md` (nouvelle
section « Correction de la rémanence »), `docs/MQTT.md` (changement de
signification `ph_total_ml`/`orp_total_ml`), `docs/features/page-dosages.md`
(avertissement jauge + estampillage version), `CHANGELOG.md [2.61.0]`,
[ADR-0091](../../docs/adr/0091-comptabilite-hors-du-corps-decisionnel-dupdate.md)
(nouveau — la comptabilité est hors du corps décisionnel d'`update()`), et
`specs/features/todo/bug-116-instantane-de-composition-perime-en-fenetre-operateur.md`
(critère d'acceptation « aucune ligne modifiée » marqué caduc, à reformuler à la
reprise).

**Découverte de méthode, à charge d'un lot d'hygiène séparé (non corrigée ici)** :
le test de repliement `millis()` « évident » (delta `5001` ms) **ne tue pas** le
mutant « soustraction signée » — signé et non signé coïncident sous 2³¹. Il faut un
delta **exactement à 2³¹** pour les séparer (`test_native_acct_window` applique
cette correction). Le relecteur a trouvé **un autre test du dépôt avec le même
vice** : `test/test_native_dose_block_duration/test_dose_block_duration.cpp:176`
annonce « franchir 2³² » avec un delta de 12 296 ms, qui ne distingue pas non plus
signé/non signé sous 2³¹. **Signalé ici comme dette à ouvrir en spec d'hygiène
séparée — non corrigé dans ce cycle.**

**Une recommandation du gate `pool-chemistry` P2 a été vérifiée et écartée** (voir
section ci-dessus « Recommandation du gate P2 ÉCARTÉE ») : le gate croyait
l'ordonnancement nominal modifié entre `closePhDosingSession`/fin de pouls et la
comptabilité ; `git show HEAD:src/pump_controller.cpp` montre un ordre relatif
strictement identique avant/après, le décalage de numéros de ligne venant des
+61 lignes ajoutées par ce lot et par `feature-136`. **Aucun déplacement n'a été
fait en conséquence** — à ne pas « corriger » sur la seule foi du rapport du gate.

---

## Clôture — agent principal, 2026-10-08

### Le point bloquant de la revue : la garde CI protégeait la mauvaise chose

`tools/check_acct_settle_single_point.sh` ne vérifiait que les **appelants** d'`accountPumpVolume()`.
Le relecteur a nommé le scénario de régression le plus probable, et il est juste :

> quelqu'un réintègre `updateDosingCore()` dans `update()`, retire `_acctSettledThisTick`
> — **et la garde reste verte**.

Les quatre appels locaux dans les sorties anticipées font justement **paraître le wrapper
redondant**. Il ne l'est pas : c'est **lui** qui couvre une **cinquième sortie** que quelqu'un
ajouterait sans y penser.

**Garde étendue** : elle exige désormais les trois lignes du wrapper dans le corps d'`update()`
(`_acctSettledThisTick = false;` · `updateDosingCore(` · `if (!_acctSettledThisTick)
settlePumpAccounting(`). **Éprouvée trois fois** :

| Épreuve | Attendu | Obtenu |
|---|---|---|
| drapeau retiré du wrapper | refus | **exit 1** |
| filet de sécurité retiré | refus | **exit 1** |
| commentaire indenté mentionnant `accountPumpVolume(` | **accepté** | **exit 0** |

La troisième corrige un faux positif relevé en revue — et c'était **exactement le commentaire que
l'existence de cette garde invite à écrire**.

### Formulation corrigée
« PLANCHER **NO-OP** » **surévaluait** : `creditManualInjectionFloor` est un `max()` au **majorant
flow255**. Quand le tick a crédité au débit **calibré** (plus faible), le plancher remonte encore
`daily`. Surcompte, donc fail-safe — mais **pas neutre**. Réécrit en « rarement mordant ».

### Recommandation du gate P2 écartée — et confirmée écartée par la revue
Le gate croyait l'ordonnancement nominal modifié. `git show HEAD:` montre un ordre relatif
**strictement identique** (fermeture de session → fins de pouls → comptabilité) ; le décalage de
**+61 lignes** vient du code ajouté entre-temps. Le `code-reviewer` l'a **recoupé
indépendamment**. ⚠️ **Ne pas « corriger » sur la foi du rapport du gate** : ce serait
*introduire* la régression qu'il croyait voir.

### Dette d'hygiène signalée, non traitée — à ouvrir en lot séparé
**Le test de repliement `millis()` « évident » ne prouve rien.** Avec un delta de 5 001 ms,
arithmétique signée et non signée **coïncident** : il faut dépasser **2³¹** pour que l'erreur
apparaisse. Découvert en écrivant le mutant M3, qui **survivait** au test initial.

⚠️ **Un autre test du dépôt souffre du même vice** :
`test/test_native_dose_block_duration/test_dose_block_duration.cpp:176` annonce « franchir 2³² »
avec un delta de **12 296 ms**. Cette assertion ne tue aucun mutant d'arithmétique signée.
**Chercher s'il y en a d'autres.**

### Doc périmée corrigée au passage
`docs/BUILD.md` annonçait **« Six jobs »** d'intégration continue — il y en a **huit**. Avec une
ironie à retenir : la phrase elle-même dit *« le nombre ne se recopie pas d'une revue à l'autre
(ADR-0073), il se mesure »*, et donnait un chiffre périmé. Corrigé, les deux jobs manquants
décrits.

### Résultats mesurés
- `pio run -e esp32dev_core3` : SUCCESS, aucun warning neuf (Flash 50,2 %)
- `pio test -e native` : **2 115 cas / 2 115**, **61 suites** (+1 : `test_native_acct_window`)
- Mutation : **5 mutants**, tous tués — M1, M2, M4, M5 conformes ; **M3 n'a été tué qu'après
  ajout d'un test à 2³¹**
- `code-reviewer` : **Approved après corrections** · gate `pool-chemistry` **P1 et P2 : GO**
- Les huit scripts d'hygiène verts

### ⚠️ Ce qui reste — et qui n'est PAS du code
**Déploiement interdit avant le 2026-10-11 après 20:06**, recommandé le **12/10 au matin** :
les fenêtres de surveillance de feature-136 et bug-115 doivent être **strictement sérielles**.
Les deux lots déplacent la quantité délivrée **en sens opposés** et **se masqueraient
mutuellement**.

**Protocole à exécuter AVANT l'OTA** (condition du gate P2) : relever
`orp_total_injected_ml`, `orp_starts`, `orp_anom`/`orp_anom_ml`, **et mesurer le restant au
doseur**. **APRÈS l'OTA** : **« Mettre à jour le niveau »** avec le volume mesuré.
⚠️ Le bouton « Nouveau bidon » **n'existe plus** : feature-143 (v2.59.0) l'a remplacé par
« Mettre à jour le niveau », qui fait exactement la même chose — il pose le volume **et** remet
`*_total_injected_ml`, `*_starts` et les deux compteurs d'anomalie à zéro dans la même requête. Sans cela, la campagne du
bidon de 5 L **straddle** le changement de signification et devient **non concluante**.

**Surveillance 7 jours**, relevés quotidiens à 23:50. Retour arrière vers v2.60.0 si : limite
journalière atteinte sur une chimie · consommation ORP > 2 070 mL/j deux jours consécutifs sans
cause externe · pH corrigé hors 7,20-7,50 plus de 24 h · démarrages/jour > 3 × la médiane.
Un `anom > 0` appelle une **analyse**, pas un retour arrière automatique — c'est désormais le
canari d'un chemin inconnu.
