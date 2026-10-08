# bug-115 — L'état de comptabilité reste rémanent aux sorties anticipées : la durée d'une coupure est créditée au débit plateau

## Métadonnées
| | |
|---|---|
| Type | bug (comptabilité des volumes → fiabilité d'une mesure de terrain) |
| Statut | todo |
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
clic « Nouveau bidon », qui efface la mesure.

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
- [ ] La campagne de concordance en cours est **close** avant toute implémentation
- [ ] À la reprise après chaque sortie anticipée, aucun volume fantôme n'est crédité
- [ ] Le redémarrage physique est compté comme un démarrage
- [ ] Test natif de chaque branche de sortie, **validé par mutation**
- [ ] Les compteurs d'hygiène retombent à zéro sur les cas ainsi corrigés

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

### ADR attendu — ⚠️ PAS 0088 ni 0089, tous deux pris

**0088 est occupé** par `docs/adr/0088-diffusion-websocket-ciblee-sur-registre-borne-sans-verrou.md`
(bug-081, livré le 2026-10-08 pendant que ce lot attendait son verrou de calendrier).
**0089 est occupé à son tour** par `docs/adr/0089-ce-qu-un-redemarrage-a-le-droit-doublier.md`
(feature-136, implémentée le 2026-10-08 — même jour, verrou de calendrier levé en premier
pour ce lot-là). Prendre **le prochain numéro réellement libre au moment de
l'implémentation** — **0090** à ce jour (2026-10-08), à revérifier par `ls docs/adr/`
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
