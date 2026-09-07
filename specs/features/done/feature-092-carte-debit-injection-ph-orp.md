## Métadonnées
- **ID** : feature-092
- **Slug** : carte-debit-injection-ph-orp
- **Type** : feature (observabilité de la régulation — aucun changement de dosage)
- **Statut** : **livré — tests verts, revue OK** (v2.46.0, 2026-09-07)
- **Priorité** : normale
- **Version cible** : **2.46.0** — livrée. ⚠️ Les mentions « 2.34.x ou 2.35.0 » et la **fenêtre d'observation 72 h du 03/09** sont **périmées** : cette fenêtre est refermée depuis le 06/09, le déploiement n'a plus de contrainte de calendrier.
- **Dernière mise à jour** : 2026-09-07 (clôture : R1 rectifié, R2 tranché, check-list cochée, six références périmées nettoyées)
- **Spec(s) liée(s)** : feature-085 (dose de base apprise — origine de la ligne `#ph_ff_line`), feature-084 (intégrale, anneau `ph_load7`), feature-086 (limite d'actionneur : bloc Capacités, bandeaux), bug-089 (parité WS ↔ config — même famille de piège), bug-082 (champ WS-seul)
- **Historique** : première version de cette spec (03/09 matin, slug `dose-de-base-apprise-lisible-hors-session`) ne traitait que le tiret muet de la ligne dose de base ; élargie le même jour, sur décision utilisateur, en refonte de la présentation des débits — l'`ux-ui-designer` a été consulté en amont (pré-arbitrages ci-dessous, à confirmer pendant le cycle).

## ✅ REJOUÉ ET TRANCHÉ AU CYCLE (07/09/2026) — feature-096 était un angle mort chronologique

> Les deux points ci-dessous ont été **repris pendant le cycle** et leurs verdicts
> sont consignés en place. Cette section n'est plus une réserve d'ouverture, c'est
> une trace de décision.

Cette spec a été écrite le **03/09**. La carte « **Atteinte de la cible** »
(`feature-096`, blocs `#ph-ttt-block` / `#orp-ttt-block`) a été déployée le
**05/09 en v2.37.0**. La spec n'en contient donc **aucune mention** — ce n'est pas
un arbitrage, c'est un angle mort chronologique.

**Ce qui va bien** : il n'y a **pas de collision de placement**. Le bloc TTT vit
*à l'intérieur* de `.ph-stats` (2ᵉ colonne, `index.html` l. 719) ; la carte
`#ph-card-dosing` s'insère *après* `.ph-stats`. Aucun élément que cette spec retire
ou déplace n'appartient au bloc TTT. Les sept pastilles ne bougent pas.

**Deux points repris, et voici leurs verdicts.**

### R1 — RECTIFIÉ (07/09) : deux trames, deux gabarits, **un seul plafond dur**

Le chiffrage d'origine de cette spec (« gabarit `StaticJson<2816>`, pire cas
2 745 o ») était périmé, **et la manière de le refaire était fausse**. Ce cycle a
produit **cinq** chiffrages de budget, dont **quatre faux** — tous pour la même
raison. Voici l'état rectifié, à lire avant tout ajout futur à cette trame.

**Il y a DEUX trames et DEUX gabarits, mais UN SEUL plafond dur.**

| | Fabricant | Gabarit documentaire |
|---|---|---|
| trame **capteurs** | `_buildSensorJson()` — [ws_manager.cpp:484](../../../src/ws_manager.cpp#L484) | `StaticJson<3456>` (**était 3072**) |
| trame **config** | `_buildConfigJson()` — [ws_manager.cpp:944](../../../src/ws_manager.cpp#L944) | `StaticJson<3008>` |

⚠️ **Le gabarit n'est PAS une limite.** En ArduinoJson 7, `StaticJson<N>` dérive de
`JsonDocument` et `N` n'est plus que `capacity()` — le pool est alloué
dynamiquement. **Le bumper coûte 0 octet de RAM**, et un bump refusé « pour
économiser la RAM » serait un **mauvais arbitrage** : il n'économise rien. Le seul
plafond **DUR** est `kWsFrameBufBytes = 3584` o (buffer statique `.bss`
`sWsFrameBuf`), et **celui-là** se paie en RAM.

**Chiffres courants de la trame capteurs** : **119 clés**, pire cas **3 447 o**,
**marge 137 o** sous 3584.

**MÉTHODE — c'est elle qui manquait.** On compte au **PIRE CAS STRUCTUREL** (borne
du tampon ou du type qui porte la valeur), **jamais** sur une trame vivante. La
chaîne de commentaires de budget partait d'une **mesure vivante de 2 479 o**
(v2.25.1) sur laquelle **huit deltas pire-cas** ont été empilés : le total
héritait à chaque étape du sous-comptage de son point de départ. Cas d'école
rectifié dans ce cycle — les deux `*DoseBlockedReason` sont bornés par
`kDoseBlockedReasonMaxLen = 96` (95 car. + 2 guillemets = 97 o de valeur) :

| Clé | Pire cas structurel | Était comptée |
|---|---:|---:|
| `phDoseBlockedReason` | **120 o** | 62 |
| `orpDoseBlockedReason` | **121 o** | 64 |

**+115 o de correction sans qu'aucun champ soit ajouté** — contre ~8 o chacun en
fonctionnement normal. Et ce pire cas **n'est pas théorique** : les causes de refus
les plus longues sont celles de l'**escalade de sécurité pH**, c'est-à-dire le
moment précis où perdre la trame coûte le plus cher. Rappel du mode de
défaillance : le dépassement **SAUTE le push**, il ne **tronque jamais**.

**Décision de budget (07/09) : élagage REFUSÉ.** Retirer `ph_integral` (23 o) et
`ph_integral_term` (26 o) de la seule trame WS rendrait **49 o** (marge portée à
**186 o**), et ces deux clés n'ont plus **aucun consommateur client**. Refusé pour
deux motifs, à lire avant toute nouvelle tentative :

1. elles deviendraient `/data`-seules, donc **deux exceptions de plus** dans la
   sentinelle de parité — sur l'axe précis qui vient de produire un défaut réel.
   Payer en exceptions ce que l'on gagne en octets, **le jour même où l'on installe
   la garde**, est un mauvais échange ;
2. l'autre voie — relever `kWsFrameBufBytes` — coûterait **+512 o de `.bss`**,
   donc autant de RAM en moins pour le tas, pendant que **bug-080** documente une
   famine mémoire **active**.

Le levier reste **valable** pour une future feature qui manquera de place : les
**49 o** et les deux clés sont consignés ici pour que le chiffre soit déjà
disponible. Doc de référence :
[`docs/subsystems/ws-manager.md`](../../../docs/subsystems/ws-manager.md).

### R2 — TRANCHÉ (07/09) : l'ordre de la spec est retenu, sans intitulé ni séparateur

`ux-ui-designer` a été **réinvoqué pendant le cycle**, les deux blocs sous les
yeux. Verdict : **l'ordre de la spec est conservé** — la carte s'insère après
`.ph-stats` (donc après le bloc TTT, qui vit *à l'intérieur* de la 2ᵉ colonne des
stats) et avant `#ph-card-regulation`. Lecture obtenue : **mesure → performance du
jour → ce que fait la régulation maintenant → réglages**.

**Ce qui a été explicitement refusé** : ajouter un intitulé de section ou un
séparateur (`<hr>`) entre les deux blocs pour marquer le changement de
temporalité. Motif : la transition est **déjà portée par les éléments
existants** — la légende « *7 derniers jours — le plus récent à droite* » **ferme**
le bloc TTT, le **titre** et la **pastille** **ouvrent** la carte. Un titre de
section aurait créé une hiérarchie qui n'existe pas dans le modèle mental de la
page, et un filet aurait ajouté du bruit visuel sur 360 px.

Points de la maquette également validés dans le cycle : `card__head--wrap` (la
pastille passe sous le titre ≤ 500 px), contraste de la pastille neutre, **aucun
glyphe « ● / ○ »** (indiscernables à 12 px, et ils créeraient une **seconde
sémantique de vert** à 20 px des pastilles du calendrier 7 jours), **une seule
zone `aria-live`** par carte — d'où le retrait de `role="status"` sur
`#ph_integral_warning` en entrant dans la carte.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`** — **lecture seule** : accesseurs `const` ajoutés dans `pump_controller.h`, factorisation à résultat identique de deux expressions déjà existantes. AUCUNE ligne du chemin de dosage ne change. Visa attendu : GO de non-régression par diff.
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — accesseurs lecture seule + question du signe du terme P (voir § Sécurité chimique)
- [x] ux-ui-designer — **déjà consulté en amont** (03/09) ; à réinvoquer pendant le cycle pour valider l'implémentation contre la maquette (contraste pastille, mobile 360 px)
- [x] embedded-developer — accesseurs, champs `/data` + WS + MQTT
- [x] web-ui-developer — carte, fusion des points de décision, retraits
- [x] test-engineer — tests natifs des fonctions pures + mutation
- [x] code-reviewer
- [x] doc-writer

## Description
Remplacer, sur les pages pH et ORP, les morceaux empilés au fil des versions (bloc Rattrapage accumulé, ligne Dose de base apprise, ligne Débit soutenable du bloc Capacités, bandeau « Dosage au maximum autorisé ») par **une carte unique « Débit d'injection »** qui dit, à tout instant, la phrase que l'utilisateur cherche : *« la régulation demande tant, composé de ceci + cela, borné par ça — et en ce moment elle l'applique, ou non, et voici pourquoi. »* Les composants du calcul sont toujours lisibles (ils varient lentement) ; une seule pastille d'état dit l'instant.

## Contexte
Constat du 03/09/2026, premier matin d'activation réelle du feedforward :

1. **Tiret muet hors session.** Sessions de 7-8 min suivies de 15 min de pause mélange : la ligne « Dose de base apprise » n'affiche la valeur (1,41 mL/min) qu'au statut *actif*, soit un tiers du temps. `ph_ff_ml_per_min` vaut 0 hors session par construction ([pump_controller.h:1117](../../../src/pump_controller.h#L1117)) et la valeur **apprise** n'est servie nulle part (ni `/data`, ni WS, ni MQTT — seulement dans le journal à l'ouverture de session). L'utilisateur ne peut pas distinguer « rien d'appris » de « 1,41 appris, en attente ».
2. **Libellé « en apprentissage » figé** entre minuit (archivage de la 3ᵉ journée exploitable) et le premier tick FF du matin (07:09) : l'instantané `_ffStatus` n'est rafraîchi que quand `update()` atteint le tick (eau présente, hors purge). Le statut *actif* périmé est déjà traduit en *inactif* hors session ([pump_controller.h:1129-1135](../../../src/pump_controller.h#L1129-L1135)) ; le statut *historique insuffisant* périmé ne l'est pas.
3. **Plus largement** (demande utilisateur du même jour) : la page pH expose le calcul du débit en morceaux — `#ph-integral-section` (« 54 % », « N pH·s · X mL/min demandés »), `#ph_ff_line`, `#ph_capacity_block` (soutenable, maximum journalier), `#ph_budget_banner`, `#ph_undersized_banner` — sans jamais présenter l'équation. **Le terme proportionnel (kp·écart) et la demande totale ne sont servis nulle part** : impossible de reconstituer le calcul depuis l'interface.
4. **Piège de parité déjà présent** : `phMixingDelayActive` et `ph_mix_remaining_s` (nécessaires à la pastille « Pause mélange ») sont servis **par le WebSocket seulement** ([ws_manager.cpp:431-434](../../../src/ws_manager.cpp#L431-L434)) et absents de `/data` — un affichage qui s'en servirait CLIGNOTERAIT (`latestSensorData` est remplacé par les charges `/data` toutes les 30 s).

Rappel de l'équation (mode automatique, pH) : **demande = dose de base apprise (FF) + écart courant (kp·e) + rattrapage accumulé (ki·∫e)**, bornée par le **débit soutenable** (limite horaire × débit pompe). Le FF est lui-même plafonné à 60 % du soutenable. ORP : **demande = kp·e** seul (P-pure, Ki = 0, pas de FF). L'injection réelle est en tout-ou-rien par pouls (pompe ON ≈ 50 s sur une session de 505 s) : les « débits » sont des moyennes de session.

## Comportement attendu

### Carte « Débit d'injection pH » (`#ph-card-dosing`) — pré-arbitrages `ux-ui-designer`

**Placement** : nouvelle `.card` **entre `.ph-stats` et `#ph-card-regulation`** (lecture : mesure → ce que fait la régulation maintenant → réglages). Visible **en mode automatique seulement**.

**Structure : tableau 3 lignes + total, PAS de barre empilée** (une barre ment dès que le terme P est négatif et change d'échelle quand la demande dépasse le plafond ; sur 360 px, trois segments de 20-40 px sont illisibles sans les nombres qu'ils dupliquent). Réutiliser `.kv` / `.kv__row` / `.kv__k` / `.kv__v` / `.kv__note` avec un modificateur `.kv--dose` (`font-variant-numeric: tabular-nums`, pas de hover, `border-top` sur la ligne total). Aucune jauge en v1.

Desktop (≥ 500 px) — état bridé montré sur Demande :
```
┌ Débit d'injection pH   [○ Pause mélange · 12 min] ────────────────────┐
│ Dose de base apprise                                    1,41 mL/min   │
│ Écart courant   7,44 → 7,30                             0,35 mL/min   │
│ Rattrapage accumulé   réserve 54 %                      2,10 mL/min   │
│ ─────────────────────────────────────────────────────────────────────  │
│ Demande   plafonnée · 5,20 demandés                     4,33 mL/min   │
│ La limite horaire (10 min/h) plafonne l'apport à 4,33 mL/min.         │
│ Débits moyens sur la session : la pompe injecte par impulsions.       │
└───────────────────────────────────────────────────────────────────────┘
```
Mobile (360 px) — état nominal :
```
┌────────────────────────────────┐
│ Débit d'injection pH           │
│ [● Injection en cours]         │
│ Dose de base apprise 1,41 mL/min│
│ Écart courant        0,35 mL/min│
│   7,44 → 7,30                  │
│ Rattrapage accumulé  2,10 mL/min│
│   réserve 54 %                 │
│ ────────────────────────────── │
│ Demande              3,86 mL/min│
│   sur 4,33 possibles           │
└────────────────────────────────┘
```
`card__head` en `flex-wrap` (la pastille passe sous le titre ≤ 500 px) ; valeurs `white-space: nowrap`, fr-FR 2 décimales ; signe « − » affiché **uniquement** sur Écart courant si négatif (assouplissement de la règle « valeurs absolues » du bloc intégral, pour P seulement — à confirmer par `pool-chemistry`).

**Lignes** :

| Ligne | Valeur | Note (`kv__note`) | Source |
|---|---|---|---|
| Dose de base apprise | `ph_ff_learned_ml_per_min` — la valeur **apprise**, stable, affichée aux statuts 0/1/2/4 | dérivée de `ph_ff_status` : *(rien)* au statut 1 ; « appliquée à la prochaine session » au statut 0 ; « suspendue (pH sous consigne, mesure invalide ou réglages modifiés) » au 2 ; « en apprentissage (historique < 3 j) » au 3 (valeur « — ») ; « suspendue (60 % du quota atteint) » au 4 | nouveau champ + existant |
| Écart courant | `ph_p_term_ml_per_min` (signé) | « 7,44 → 7,30 » (mesure filtrée → consigne) | nouveau champ |
| Rattrapage accumulé | `ph_integral_term` (mL/min, existant) | « réserve 54 % » (`ph_integral_pct`, existant) — **le « N pH·s » disparaît** (jargon sans usage pour la cible) | existants |
| Demande (total, `border-top`) | **`ph_demand_ml_per_min`** en gras (⚠️ **pas** un « appliqué » — voir INV-C1 ci-dessous) | livré : « sous le maximum de X,XX mL/min » (`ph_sustainable_ml_per_min`) ; bridé : « apport moyen possible X,XX mL/min **en moyenne sur l'heure** » ; plafond `null` : pas de note, pied « débit non caractérisé » | nouveaux + existant |

> ⚠️ **INV-C1 — « APPLIQUÉ » N'EXISTE PAS, ET N'EXISTERA PAS (rectifié le 07/09).**
> Les champs `ph_applied_ml_per_min` / `orp_applied_ml_per_min` que cette spec
> décrivait **n'ont jamais été implémentés, et ne le seront pas.** Le tick borne la
> demande à l'**amplitude de la pompe** (26,0 mL/min sur cet appareil), **jamais**
> au débit soutenable ; la limite horaire agit **en aval**, par **refus**. Avec la
> config vivante (10 min/h), une demande de 5,20 est réellement servie
> **à 5,20 de moyenne jusqu'à épuisement du budget**, puis 0. Afficher
> `min(demande, soutenable)` comme un débit servi **sous-estimait de 17 %** ce que
> le bassin reçoit dans les dix minutes suivantes. D'où la forme livrée : le total
> est la **Demande**, et le plafond descend en **note**, sous son vrai nom
> (« apport moyen possible … **en moyenne sur l'heure** »). Un client qui veut
> `min(demande, soutenable)` peut le reconstruire — mais **ce nombre n'est pas ce
> que la pompe délivre.**

**Pieds de carte** (une ligne chacun, `muted small`) : « Débits moyens sur la session : la pompe injecte par impulsions. » toujours ; « La limite horaire ({X} min/h) plafonne l'apport à {Y} mL/min. » quand bridé (phrase `pool-chemistry` de la 086, raccourcie). `#ph_integral_warning` (callout ≥ 95 %) **déplacé** au pied de la carte, inchangé.

**Pastille d'état** (`.pill` existante dans `.card__head`, `role="status"` `aria-live="polite"` — seule zone live de la carte), **dérivée côté client** de champs servis, dans cet ordre de priorité :

| Priorité | Condition | Classe | Libellé |
|---|---|---|---|
| 1 | `phDoseBlockedCode` de ton `fault` (panne sonde / filtre) | `.pill.bad` | « Dosage suspendu · {chip du code} » |
| 2 | `ph_pulse_session === true` | `.pill.ok` | « Injection en cours » |
| 3 | `phMixingDelayActive === true` | `.pill` neutre | « Pause mélange · {ceil(ph_mix_remaining_s/60)} min » |
| 4 | code de ton `normal` (filtration arrêtée, limite horaire, anti-rafale, stabilisation…) | `.pill` neutre | « En attente · {chip du code} » |
| 5 | aucun code, pas de session | `.pill` neutre | « En attente · pH à la consigne » — **règle de dérivation à préciser avec le firmware** (hystérésis de session : pH < consigne + 0,05) plutôt que devinée côté client → item A6 |

Sémantique de couleur : **vert = pompe commandée, gris = normal, rouge = panne uniquement. Pas d'ambre** (une pause n'est pas un avertissement). Compte à rebours **uniquement** sur la pause mélange (champ existant, borné, répond exactement à la question du 03/09) ; **pas** de durée écoulée de session (aucun champ, ticking = bruit). Les libellés courts (`chip`) des codes 2, 10-14 sont à ajouter dans `DOSE_BLOCK_CODES` (source unique inchangée, [app.js ~l.4752](../../../data/app.js#L4752)).

### Carte « Débit d'injection ORP » (`#orp-card-dosing`) — même cycle, même composant, variante réduite
Régulation P-pure : lignes « Écart courant  {`orp_p_term_ml_per_min`}  (612 → 650 mV) » et « Demande  {`orp_demand_ml_per_min`}  sous le maximum de X,XX mL/min » (⚠️ **pas** d'`orp_applied_ml_per_min` : voir INV-C1). Pastille identique (les champs `orp_pulse_session`, `orpMixingDelayActive`, `orp_mix_remaining_s`, `orpDoseBlockedCode` existent déjà — `_doseBlockInfo(json, prefix)` est préfixé). La carte montre au passage *pourquoi* le chlore est régulé plus simplement. **Repli NON utilisé** : la carte ORP est livrée avec ses **deux** lignes (Écart + Demande). ⚠️ **Pas** de note « demande instantanée » côté ORP, à la différence du pH : il n'existe **pas** de porte de session ORP (`phSessionGate` est propre au pH), la décision chlore lit la **filtrée instantanée** — la même que la composition. La phrase du pH y serait **fausse**.

### Firmware — lecture seule

Nouveaux champs, servis **à l'identique** sur `GET /data`, trames WS `sensor_data`, MQTT (retain, dédup, auto-discovery HA `entity_category: diagnostic`, `unit: mL/min`, `state_class: measurement`) :

| Champ | Sémantique | `null` quand |
|---|---|---|
| `ph_ff_learned_ml_per_min` | `min(charge médiane 7 j, kPhFfMaxSustainableFraction × soutenable)` — le nombre que le journal annonce (« dose de base apprise 1.41 … plafond 2.60 »), calculé **à la lecture** depuis `_phLoadObs` (jamais depuis un instantané du tick) | historique < `kPhIntegralRestoreMinHistoryDays` ou soutenable non fini/non positif |
| `ph_p_term_ml_per_min` | kp × erreur signée courante (positive = il faut doser), depuis la mesure filtrée corrigée et la consigne | mesure invalide (filtre non prêt, figé, purge, NaN) |
| `ph_demand_ml_per_min` | FF appliqué + P + I, **avant** plafond | P `null` |
| `ph_i_term_ml_per_min` | rattrapage **SERVI** (0 dans la bande morte) — distinct de `ph_integral_term`, terme **brut** et contrat externe historique | P `null` |
| ~~`ph_applied_ml_per_min`~~ | **NON IMPLÉMENTÉ, et ne le sera pas** — voir INV-C1 | — |
| `orp_p_term_ml_per_min`, `orp_demand_ml_per_min` | idem ORP (demande = P) | idem |
| `orp_sustainable_ml_per_min` | miroir de `ph_sustainable_ml_per_min` (amplitude calibrée × limite horaire ORP) | pompe non caractérisée |
| `orp_limited_by_hourly_budget` | **verdict** « le chlore est borné par sa limite horaire en ce moment » — homologue d'`ph_limited_by_hourly_budget`, absent côté ORP jusqu'ici. **DIAGNOSTIC, pas alarme** (verdict `pool-chemistry` : à CYA 35 mg/L, saturer le quota chlore est **cohérent** avec le coût de la protection UV pour 2,5-3 mg/L de chlore libre — ce n'est **pas** à corriger en montant la consigne ou le gain) | jamais `null` |

Ajustements d'existants :
- `getPhFeedforwardStatus()` : traduire 3 (historique insuffisant) → 0 (inactif) lorsque `_phLoadObs.days ≥ kPhIntegralRestoreMinHistoryDays` — même patron que la traduction actif → inactif. Statuts 2 et 4 servis tels quels (vrais entre deux ticks).
- **Parité** : ajouter `phMixingDelayActive`, `ph_mix_remaining_s`, `orpMixingDelayActive`, `orp_mix_remaining_s` à `GET /data` (aujourd'hui WS-seuls). Vérifier également `ph_pulse_session` / `orp_pulse_session` et `*DoseBlockedCode` des deux côtés (présents : [web_routes_data.cpp:218,262](../../../src/web_routes_data.cpp#L218)).
- Contrat `PhFfStatus` **inchangé** (aucun code ajouté — le besoin est couvert par la traduction côté accesseur ; ajouter un code exigerait un ADR).

### Cas limites — dégradation : une sentinelle, des tirets, jamais de zéro
- Mode de régulation ≠ `automatic` **ou** clé `ph_demand_ml_per_min` `undefined` (firmware ancien) → **carte entière masquée** (règle existante du bloc intégral ; pas de repli sur l'ancienne présentation, qui est retirée).
- Une valeur `null` → « — » sur sa ligne, la raison en note : FF statut 3 → « en apprentissage (historique < 3 j) » ; plafond `null` → Demande sans « sur X possibles », pied « pompe non caractérisée » ; **mesure invalide** (filtration arrêtée, purge) → Écart et Demande « — », **FF et Rattrapage restent affichés** (grandeurs lentes, toujours vraies), la pastille porte la cause (« En attente · Filtration arrêtée »).
- **Archivage de minuit** portant l'anneau à 3 jours : la ligne FF passe de « en apprentissage » à la valeur apprise **au rafraîchissement `/data` suivant** (≤ 30 s), sans attendre la filtration du matin (correctif racine du défaut n° 2).
- **Charge médiane nulle** (3 jours exploitables sans injection) : « 0,00 mL/min » est une observation légitime — afficher 0,00, pas un tiret.
- **Changement de type de produit (pH- ↔ pH+)** : latch « tenue produit » jusqu'à l'archivage d'une journée complète → statut 2, valeur apprise affichée (c'est ce que l'appareil a en mémoire) + note « suspendue ».
- **Terme P négatif** (pH sous consigne) : « −0,35 mL/min » signé sur la ligne Écart ; la Demande reste ≥ 0 (bornée). Si `pool-chemistry` refuse le signe : valeur absolue + note « pH sous consigne ».
- **WebSocket déconnecté** : `/data` porte tous les champs (parité) → aucun clignotement.
- **Rétro-compatibilité** : `app.js` nouveau sur firmware sans les champs → carte masquée, aucune erreur console (`num()` protège du NaN) ; les cartes retirées ne réapparaissent pas.

## Comportement actuel
Décrit au § Contexte : présentation en morceaux (`#ph-integral-section`, `#ph_ff_line`, `#ph_capacity_block`, `#ph_budget_banner`, `#ph_undersized_banner`), pas de terme P ni de demande servis, valeur apprise non servie, pause mélange WS-seule, statut FF 3 périmé non traduit. Deux points de décision `hidden` distincts côté UI : `updatePhIntegralDisplay()` et `updatePhActuatorLimitDisplay()`.

## Contraintes techniques
- ESP32, offline first ; Vanilla JS ; français partout ; mobile first (360 px — l'appareil se consulte au bord du bassin).
- **Aucune modification de `phFeedforwardTick`, `tickPhFeedforward`, `update()`, du calcul PI, ni d'aucune constante `k*`** : spec purement observationnelle. Les expressions FF-plafonné et demande/appliqué sont **factorisées** (fonctions pures partagées entre le tick et les accesseurs), à résultat strictement identique — la suite native existante `test/test_native_ph_feedforward/` sert de preuve et **ne doit pas être modifiée**.
- Course bénigne assumée pour les accesseurs (patron `isAnyPumpEnergized` : écrits en loopTask, lus en contexte web async ; lectures 8/32 bits atomiques sur Xtensa) — à documenter dans chaque accesseur comme les voisins.
- **Budget JSON — point dur, RÉSOLU (voir R1 rectifié en tête)** : le chiffrage d'origine de cette ligne (« gabarit `StaticJson<2816>`, pire cas 2 745 o, 7 champs float ≈ 230 o ») est **périmé sur toute la ligne**. État livré : **8** champs de composition (**265 o**), les 4 champs de pause mélange ajoutés à `/data` (**122 o**, déjà présents côté WS depuis la v2.19.1), 2 booléens de caractérisation ajoutés à la trame WS (**75 o**), plus **+115 o de correction de méthode** sur les deux `*DoseBlockedReason`. Pire cas de trame **3 447 o**, gabarit porté **3072 → 3456**, **marge 137 o** sous le seul plafond dur `kWsFrameBufBytes = 3584`. Élagage **refusé** (motifs en R1). Commentaires de budget mis à jour dans `ws_manager.cpp`, `web_routes_data.cpp`, `docs/API.md`, `docs/subsystems/ws-manager.md`.
- Budget FS : la carte remplace des blocs ; le delta JS/HTML doit être ≈ neutre — vérifier avec `./build_fs.sh`.
- AsyncWebServer : aucun handler bloquant > 50 ms.
- Accessibilité : une seule zone `aria-live` par carte (la pastille) ; contraste `.pill` neutre (`--muted` #595f6b sur `--panel2`) à vérifier ≥ 4,5:1 (proche de la limite sur fond translucide) — `ux-ui-designer` pendant le cycle.

## Fichiers potentiellement concernés
- `src/ph_feedforward_logic.h` — fonctions pures `phFeedforwardLearnedFlow(obs, sustainable)` et `phFeedforwardServedStatus(raw, sessionOpen, historyDays)` ; le tick appelle la première (factorisation).
- `src/pump_controller.h` / `.cpp` — accesseurs `getPhFeedforwardLearnedFlow()`, `getPhPTermFlow()`, `getPhDemandFlow()`, `getPhAppliedFlow()` (+ ORP) ; `getPhFeedforwardStatus()` réécrit sur la fonction pure ; règle « pH à la consigne » (A6).
- `src/web_routes_data.cpp` — nouveaux champs + pause mélange (parité) + commentaire de budget.
- `src/ws_manager.cpp` — nouveaux champs + commentaire de budget/gabarit.
- `src/mqtt_manager.cpp` — topics + auto-discovery HA (« Piscine pH Dose de Base Apprise », « Piscine pH Écart Courant », « Piscine pH Demande », « Piscine pH Débit Appliqué » + ORP).
- `data/index.html` — `#ph-card-dosing`, `#orp-card-dosing` ; retraits/déplacements listés ci-dessous.
- `data/app.js` — `updatePhDosingCard()` / `updateOrpDosingCard()` (point de décision unique remplaçant `updatePhIntegralDisplay()` + `updatePhActuatorLimitDisplay()`) ; chips courts dans `DOSE_BLOCK_CODES`.
- `data/app.css` — `.kv--dose`, `card__head` flex-wrap.
- `docs/features/page-ph.md`, `page-orp.md`, `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/pump-controller.md`, `ws-manager.md`, `CHANGELOG.md`.
- `docs/user/src/guide-utilisation.html` (pages pH et ORP) et `guide-regulation.html` (la carte illustre l'équation FF + P + I — bonne occasion de la citer) → `./tools/build_user_pdfs.sh` ; **captures pH et ORP à refaire**.

### Existant à retirer / déplacer (UI)
- **Retirer** : `#ph-integral-section` (bloc + `<hr>` + paragraphe pédagogique « correction lente… » → déjà couvert par le guide régulation PDF), `#ph_ff_line`, `#ph_budget_banner` (+ `_text`), ligne « Débit soutenable » (`#ph_sustainable_value`) du `#ph_capacity_block` — ce bloc se réduit à « Maximum journalier », qui **reste** sous la barre du jour (grandeur journalière, pas un débit ; ne pas la fusionner dans l'en-tête `daily-dose` : deux « maximum » côte à côte prêteraient à confusion).
- **Déplacer** : `#ph_integral_warning` → pied de la carte ; `#ph_undersized_banner` → juste sous la carte (adjacent à ce qu'il explique ; nature différente — diagnostic 3 j — il est **conservé tel quel**) ; `#ph_integral_pct_value` → note de la ligne Rattrapage.
- **Conserver** : `#daily-dose-ph*`, `#ph_daily_max_value`, chips sonde/filtre, modal filtre (la pastille ne remplace pas le détail du code de refus).

## Livrables — check-list

- [x] A1 — `ph_feedforward_logic.h` : `phFeedforwardLearnedFlow()` factorisée depuis le § 4 de `phFeedforwardTick` (même `min`, mêmes gardes `isfinite`/`> 0`) ; le tick l'appelle.
- [x] A2 — `ph_feedforward_logic.h` : `phFeedforwardServedStatus()` regroupant la traduction actif → inactif existante et la nouvelle 3 → 0 si `days ≥ min`.
      **Précédence livrée** : le statut 3 est testé **AVANT** le fail-closed `!learnedFinite` (4ᵉ branche) — sinon « 3 journées, soutenable indisponible » serait indiscernable d'« historique insuffisant ».
- [x] A3 — fonction pure de composition livrée sous le nom `doseComposition(kp, error, kiIntegral, deadband, ffServed, ampFlow, regime)` dans **`src/dosing_logic.h/.cpp`** (et non `phDoseComposition` dans `ph_feedforward_logic.h`) → `{p, i, piServed, ff, demand, inDeadband}`. ⚠️ **Elle ne rend PAS d'« appliqué »** (INV-C1). **Bornage en deux étages, non réordonnable** : `piServed = clamp(p+i, 0, max(0, ampFlow−ff))` puis `demand = clamp(piServed+ff, 0, ampFlow)`.
- [x] A4 — `pump_controller.h` : **8** accesseurs `const` (`getPhFeedforwardLearnedFlow`, `getPhPTermFlow`, `getPhITermFlow`, `getPhDemandFlow`, `getOrpPTermFlow`, `getOrpDemandFlow`, `getOrpSustainableMlPerMin`, `isOrpLimitedByHourlyBudget`) ; `getPhFeedforwardStatus()` sur A2 ; statuts 1, 2, 4 strictement inchangés. Course bénigne documentée (patron `isAnyPumpEnergized`), copie sous spinlock `portMUX`.
- [x] A5 — `/data` + WS : **8** nouveaux champs + **4** champs pause mélange sur `/data` + **2** booléens de caractérisation sur la trame WS. **Parité prouvée non par grep mais par une SENTINELLE NATIVE** (`test_native_ws_data_parity`, 9 cas) — la preuve manuelle demandée ici a été remplacée par une garde permanente, et elle a **attrapé un défaut réel avant la revue**. Budget chiffré et commentaires mis à jour ; décision gabarit tracée (3072 → 3456, **marge 137 o**), élagage refusé avec motifs.
- [x] A6 — règle « pH à la consigne » (pastille de priorité 5) : **aucun champ nouveau, aucune règle firmware ajoutée** — la pastille est qualifiée « à la consigne » quand `ph_p_term_ml_per_min` vaut **exactement 0** (bande morte du couple, lue du verdict de porte). `null` ne qualifie **pas** (gain nul ou mesure invalide ≠ pH à la consigne). Un **6ᵉ cas** « En attente » seul couvre le régime où la porte est fermée sur le signal **lent** alors que l'erreur **instantanée** est non nulle ; la ligne Demande porte alors la note qui l'explique.
- [x] A7 — MQTT : **5** topics + **5** entités d'auto-discovery HA (`kDedupCacheSlots` 23 → 28, `publishDiscovery()` 54 → 59). **3** champs hors MQTT avec motif écrit.
- [x] A8 — **Placement rejoué avec le bloc TTT** — voir R2 tranché en tête de spec.
- [x] B1 — `index.html` : `#ph-card-dosing` conforme aux maquettes ; retraits/déplacements exécutés, **aucun id mort dans `app.js`** (`#ph-integral-section`, `#ph_ff_line`, `#ph_budget_banner`, `#ph_sustainable_value`, `#ph_integral_detail` n'existent plus que dans des commentaires d'historique).
- [x] B2 — `app.js` : `updatePhDosingCard()` point de décision unique (les deux fonctions remplacées sont **supprimées**) ; pastille 5 priorités + 6ᵉ cas ; libellés courts `pill`/`chip` avec repli sur `label` ; dégradation selon § Cas limites ; fr-FR 2 décimales ; signe « − » sur P seulement. **Note de bornage du couple** ajoutée (non prévue par la spec, exigée par la revue) : sans elle, le trait de total surmonterait des nombres qui ne s'additionnent pas.
- [x] B3 — `#orp-card-dosing` + `updateOrpDosingCard()` — **2 lignes, repli non utilisé**.
- [x] B4 — `app.css` : `.kv--dose`, `card__head--wrap` ; contraste pastille vérifié.
- [x] C1 — `docs/features/page-ph.md` et `page-orp.md` réécrits ; `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/pump-controller.md`, `docs/subsystems/ws-manager.md`, `CHANGELOG.md` ; **ADR-0071** créé et indexé (`docs/README.md`, `docs/adr/README.md`).
- [x] C2 — `guide-utilisation.html` (pages pH/ORP) + `guide-regulation.html` (équation citée en étapes 6 et 7, dose apprise en section 2). ⚠️ **Aucun PDF généré** : les PDF sortent en release et sont ignorés par git. **Deux captures à refaire** — `screenshots/07_pH.jpg` et `screenshots/08_ORP.jpg`.
- [x] D1 — Tests natifs A1/A2/A3 : `test/test_native_dose_composition/`, **41 cas**. Le **premier test du fichier** est le contre-exemple exigé par `pool-chemistry` (`ff = 1,41`, `p = −3,00`, `i = 0`, régime `Full` ⇒ `demand == 1,41`) — le seul qui distingue la formule du firmware de `clamp(p+i+ff, 0, ampFlow)`, et l'écart qu'il mesure est **un dosage réel contre un dosage nul**.
- [x] D2 — **Mutation** : **11 mutants** (9 sur la logique, 2 sur la sentinelle), **aucun survivant**, sources restaurées bit-pour-bit (sha256 vérifié).
- [x] D3 — suite `test_native_ph_feedforward` **inchangée et verte** ; `pio run` sans nouveau warning ; `pio test -e native` 100 % ; `./build_fs.sh` dans le budget.

### Livrables NON prévus par la spec, ajoutés pendant le cycle
- [x] **Sentinelle de parité `/data` ↔ `sensor_data`** (`test/test_native_ws_data_parity/`, 9 cas) + marqueurs `PARITE-DATA` + trois tables d'exceptions motivées + cliquet → **[ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md)**.
- [x] **Filet de non-divergence** (`kDoseCompositionDivergenceMlPerMin = 0,05`) : un WARN par session si la projection d'observabilité et le calcul PI cessent de coïncider. Témoin en **RAM seule**. Neutralisé quand le PI est **muet** (premier tick, intervalle > 10 s).
- [x] **Point de publication hors branche de dosage** (blocage n°1 de la revue) : sans lui, l'instantané n'était rafraîchi que sous la conjonction de dosage — sur un cycle réel de 7-8 min de session pour 15 min de pause, la carte aurait été fausse **la majeure partie du temps**.
- [x] **`getPhFeedforwardFlow()` passe au régime appliqué** — correction de sémantique annoncée au CHANGELOG.

## Nouvelles signatures / API
- Champs `/data` + WS `sensor_data` — **8 livrés** : `ph_ff_learned_ml_per_min`, `ph_p_term_ml_per_min`, `ph_i_term_ml_per_min`, `ph_demand_ml_per_min`, `orp_p_term_ml_per_min`, `orp_demand_ml_per_min`, `orp_sustainable_ml_per_min` (float 2 décimales | null) et `orp_limited_by_hourly_budget` (bool). ⚠️ **Aucun champ `*_applied_ml_per_min`** (INV-C1). `/data` gagne `phMixingDelayActive`, `ph_mix_remaining_s`, `orpMixingDelayActive`, `orp_mix_remaining_s` (étaient WS-seuls) ; **réciproquement** la trame WS gagne `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` (étaient `/data`-seuls — défaut réel du cycle, cf. § sentinelle).
- Topics MQTT — **5 livrés** (3 pH + 2 ORP), retain, dédup, auto-discovery HA `sensor` / `entity_category: diagnostic` / `unit: mL/min` / `state_class: measurement` : `{base}/ph_ff_learned_ml_per_min`, `{base}/ph_p_term_ml_per_min`, `{base}/ph_demand_ml_per_min`, `{base}/orp_p_term_ml_per_min`, `{base}/orp_demand_ml_per_min`. **Hors MQTT, avec motif** : `ph_i_term_ml_per_min` (doublon de `ph_integral_term`, déjà publié), `orp_sustainable_ml_per_min` et `orp_limited_by_hourly_budget` (miroirs de champs pH non publiés non plus).
- Fonctions publiques : `float getPhFeedforwardLearnedFlow() const; float getPhPTermFlow() const; float getPhDemandFlow() const; float getPhAppliedFlow() const;` (+ ORP) dans `pump_controller.h` ; pures : `phFeedforwardLearnedFlow`, `phFeedforwardServedStatus`, `phDoseComposition` dans `src/ph_feedforward_logic.h` (ou module pur dédié).
- Constantes : **aucune nouvelle**.
- Contrat `PhFfStatus` : **inchangé**. Contrat `DoseBlockedCode` : **inchangé** (seuls des libellés courts côté client sont ajoutés).

## Sécurité chimique
- Dosage impliqué : **non** — lecture seule. Les nouveaux champs sont des projections de grandeurs déjà calculées, jamais des entrées.
- Limites journalières / horaires concernées : aucune (le soutenable est **affiché**, pas modifié)
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : **non** — A1 et A3 factorisent des expressions existantes sans en changer le résultat (preuve : suite native existante intacte + tests D1)
- Risque identifié : aucun sur l'eau.
- **RÉPONSE `pool-chemistry` (07/09) — terme P SIGNÉ : ACCEPTÉ**, sur P seulement. Un « −0,35 mL/min » se lit « la régulation freine », ce qui est vrai et pédagogiquement utile ; la valeur absolue aurait suggéré une injection.
- **Visa `pool-chemistry` : GO de non-régression**, sous deux conditions, toutes deux implémentées :
  - **C1** — ne pas vérifier l'égalité `demande = PI servi + FF` au **journal d'ouverture de session** : `computePID()` y est muet (voir le critère d'acceptation correspondant) ;
  - **C2** — aucun accesseur de projection n'appelle `getPhDecisionValue()`, réservée à `loopTask` (gate feature-096), ni l'I²C, ni la NVS, ni LittleFS.
- **Exigence de test du gate** : un **contre-exemple** prouvant que le bornage en deux étages n'est pas `clamp(p+i+ff, 0, ampFlow)` — livré comme **premier test** de `test_native_dose_composition` (`ff = 1,41`, `p = −3,00` ⇒ `1,41` et non `0`).
- **ASYMÉTRIE GELÉE, renvoyée à une spec de suivi** : servir le feedforward sur une **erreur vive franchement négative**. Le régime `HoldZero` statue sur le **signal lent** (moyenne 10 min), pas sur l'erreur instantanée : il existe donc une fenêtre où le bassin est repassé sous la consigne alors que le régime sert encore le FF. **La corriger serait un changement de DOSAGE**, hors du périmètre observationnel de cette spec. Ce qui change ici, c'est qu'elle est désormais **lisible** (`ph_ff_ml_per_min` rapporte le FF du régime appliqué).
- **Verdict `pool-chemistry` sur `orp_limited_by_hourly_budget`** : c'est un **DIAGNOSTIC, pas une alarme**. À CYA **35 mg/L**, une boucle chlore qui sature son quota quotidien est **cohérente avec le coût de la protection UV** (chlore libre visé 2,5-3 mg/L) — ce n'est **pas** une anomalie à corriger en montant la consigne ORP ou le gain. La tentation naturelle devant cette carte est exactement l'inverse de la bonne réponse : d'où le pied de carte permanent, et la reprise du **pourquoi chimique** dans `docs/API.md`, `docs/features/page-orp.md`, `docs/MQTT.md` et le guide utilisateur.

## ADR attendu ? — **OUI, contre la prévision : [ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md)**

- **Prévu** : non. **Livré** : **un ADR**, pour un sujet que cette spec n'avait pas vu venir.
- **Pourquoi la prévision était fausse** : la spec traitait la parité `/data` ↔ WS comme un **rappel de vigilance** (« piège n° 1 du projet », § Notes pour agent) et se contentait d'exiger une **preuve par grep**. Or ce cycle a montré que la preuve manuelle ne suffit pas — le défaut est réapparu **dans le cycle même**, dans le sens inverse, et il a été trouvé par une **garde**, pas par un grep.
- **Critères de déclenchement remplis** : (1) la décision **contraint** tout ajout futur à ces deux fabricants de JSON (marqueurs, receveur unique, table d'exceptions, cliquet) ; (3) plusieurs **alternatives crédibles** existaient (règle symétrique de l'axe config, table unique, parité intégrale, fusion côté client) et méritent d'être écartées par écrit ; (4) la décision **verrouille** un contrat de transport public.
- **Les deux décisions « qui deviendraient ADR »** anticipées par cette spec ont été **évitées** : (a) aucun code n'a été ajouté à `PhFfStatus` — la traduction côté accesseur a suffi ; (b) `ph_integral` n'a **pas** été retiré de la trame WS — l'élagage est refusé et chiffré (R1).

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /data` — **8** champs de débit ajoutés ; **4** champs pause mélange ajoutés (parité WS) ; sémantique de `ph_ff_status = 3` précisée (jamais servi si l'anneau a ≥ 3 jours exploitables). Réciproquement, `ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` **rejoignent la trame WS** (ils étaient `/data`-seuls).

### Champs WebSocket
- `sensor_data` — **8** champs de débit, identiques à `/data`, servis dans le **même passage** entre les marqueurs `PARITE-DATA`.

### Topics MQTT
- **5** topics (**3 pH + 2 ORP**, voir § Signatures — et non 8 : ce chiffre de la première rédaction supposait des champs « appliqué » qui n'existent pas, cf. INV-C1), retain + dédup, **5** entités HA `sensor` / `entity_category: diagnostic` / `unit: mL/min` / `state_class: measurement`. **3 champs hors MQTT avec motif écrit.**

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/features/page-ph.md` — §§ « Rattrapage accumulé », « Ligne Dose de base apprise », « Bloc capacités », « Bandeau Dosage au maximum autorisé » **remplacés** par § « Carte Débit d'injection pH » (structure, table des lignes, table de la pastille, dégradation, retraits).
- `docs/features/page-orp.md` — § « Carte Débit d'injection ORP ».
- `docs/subsystems/pump-controller.md` — accesseurs de lecture, traduction 3 → 0, course bénigne, factorisation.
- `docs/subsystems/ws-manager.md` — champs, budget, gabarit.

#### Docs transverses
- `docs/API.md` — table des champs `/data`, budget, règles de consommation client.
- `docs/MQTT.md` — topics + entités HA.
- `CHANGELOG.md` — « Fonctionnalités : les pages pH et ORP présentent le débit d'injection en une carte (dose de base apprise, écart courant, rattrapage, demande et plafond) avec l'état instantané de l'injection ; la dose de base apprise reste lisible entre deux sessions et ne reste plus figée sur “en apprentissage” ».
- `docs/user/` — guides d'utilisation et régulation régénérés ; captures pH et ORP à refaire.

#### Docs immuables
- **[ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md)** — parité `/data` ↔ trame `sensor_data` : marqueurs, sentinelle, exceptions motivées. **Extension d'ADR-0065** au second axe. Indexé dans `docs/README.md` et `docs/adr/README.md`.

## Critères d'acceptation
- [x] Hors session, avec ≥ 3 jours exploitables, la ligne Dose de base apprise affiche la valeur apprise + « **en attente** — dose apprise X,XX, appliquée à la prochaine session » ; pendant une session, la valeur **appliquée** sans note et la pastille « Injection en cours ».
      *Écart de forme assumé* : la **valeur** de la ligne est `ph_ff_ml_per_min` (contribution appliquée, ce qui rend le total additionnable) et la **dose apprise** descend en **note** — l'inverse de ce que cette spec décrivait. « en attente » est placé **avant** le chiffre : l'œil doit lire la cause avant la valeur, sinon un zéro se lit comme une panne.
- [x] Pendant une pause mélange, la pastille affiche « Pause mélange · N min » et **ne clignote pas**. Parité désormais **assertée en permanence** par `test_native_ws_data_parity` (les 4 champs sont dans le bloc `PARITE-DATA`), et non plus par un `curl` ponctuel.
- [x] Le lendemain d'un archivage portant l'anneau à 3 jours, la carte quitte « en apprentissage » **avant** le démarrage de la filtration (≤ 30 s après minuit) — traduction 3 → 0 dans `phFeedforwardServedStatus()`, testée en natif dans les deux sens.
- [x] Filtration arrêtée : Écart et Demande « — » (porte `measuresValid` fermée ⇒ NAN ⇒ `null`), Dose de base et Rattrapage affichés (grandeurs lentes, hors de cette porte), pastille « En attente · Filtration arrêtée » en gris (jamais rouge).
- [x] Panne sonde (code de ton `fault`) : pastille rouge « Dosage suspendu · … » (priorité 1) ; le modal filtre reste la source du détail (`DOSE_BLOCK_CODES.label`, la pastille lisant `pill`/`chip`).
- [x] `#ph_budget_banner` **n'existe plus dans le DOM** ✅. ⚠️ **La première moitié de ce critère a été RÉÉCRITE par le cycle** (INV-C1) : la ligne Demande porte la **DEMANDE** (5,20), pas le soutenable, et la note dit « apport moyen possible 4,33 mL/min **en moyenne sur l'heure** » — pas « plafonnée · 5,20 demandés ». Afficher le soutenable comme valeur servie **sous-estimait de 17 %** l'apport réel des dix minutes suivantes. Le pied limite horaire est bien présent.
- [ ] Pendant une session active, `ph_ff_learned_ml_per_min == ph_ff_ml_per_min` (à l'arrondi)
      et `ph_demand_ml_per_min == piServed + ff`, où `piServed = ph_demand_ml_per_min − ph_ff_ml_per_min`.
      ⚠️ **Ne PAS vérifier ce critère au journal d'ouverture de session** (condition C1 du visa
      `pool-chemistry` du 2026-09-07) : `computePID()` a deux sorties anticipées — premier tick
      (`lastTime == 0`) et intervalle > 10 s — où le tick sert `0 + ff` sans que la projection les
      modélise. L'ouverture de session est **exactement** le tour où le PI est muet : y comparer
      conclurait à un défaut inexistant. **Vérifier au 2ᵉ tick ou plus tard.** L'écart dure un
      tour (~1 s) contre 30 s d'échantillonnage `/data`, il est donc structurellement invisible en
      usage, et il **surestime** la demande — jamais l'apport réel.
- [x] Les **8** champs (et non 7) sont présents dans `web_routes_data.cpp`, `ws_manager.cpp` et `app.js` ; **5** dans `mqtt_manager.cpp` (3 hors MQTT avec motif). `phMixingDelayActive` est bien dans `web_routes_data.cpp`. **Le grep est remplacé par la sentinelle** : une garde permanente vaut mieux qu'une vérification datée.
- [x] **11 mutations** (les 5 prévues + 4 sur la logique + 2 sur la sentinelle) tuées, **aucun survivant** ; `test_native_ph_feedforward` inchangée et verte.
- [x] Mobile 360 px : aucune valeur tronquée (`white-space: nowrap`, `tabular-nums`), pastille sous le titre (`card__head--wrap`), contraste de la pastille neutre vérifié par `ux-ui-designer`.
- [x] Tous les libellés utilisateur sont en français ; `pio run` sans nouveau warning ; `./build_fs.sh` passe sans dépasser la partition.
- [ ] **Comportement nominal testé manuellement selon le plan ci-dessous — RESTE OUVERT.** Seule vérification qui exige l'appareil en fonctionnement (cycle de session réel, pause mélange, bride à 2 min/h, entités HA, rétro-compat sur `/data` amputé). Les scénarios **1 à 3** et **7** portent sur des états qui ne se reproduisent qu'en conditions réelles ; le scénario **5** (terme P négatif) est couvert en natif par le contre-exemple `pool-chemistry` mais pas à l'écran.

## Plan de test manuel
1. **Cycle complet d'une session** (appareil avec ≥ 3 j exploitables, filtration en marche, pH au-dessus de consigne + 0,05) : observer la carte pendant « Démarrage dosage pH » (pastille verte, valeurs FF/P/I/Demande cohérentes avec le journal), pendant la pause mélange (pastille grise avec compte à rebours qui décroît à la minute, FF toujours affichée), puis à la réouverture.
2. **Statut périmé** : filtration arrêtée > 1 h → carte visible, Écart/Demande « — », pastille « En attente · Filtration arrêtée », FF = valeur apprise (pas « en apprentissage » si l'anneau a ≥ 3 j — comparer au journal d'archivage de minuit).
3. **Parité / clignotement** : ouvrir la carte, attendre 3 rafraîchissements `/data` (90 s) pendant une pause mélange → la pastille reste stable ; couper le réseau 1 min puis rétablir → pas de rendu intermédiaire différent.
4. **Bride** : abaisser temporairement la limite horaire pH (ex. 2 min/h) sur un pH nettement au-dessus de consigne → Demande bridée, note « plafonnée · X demandés », pied limite horaire ; restaurer.
5. **Terme P négatif** : pH sous consigne → Écart « −0,XX mL/min », Demande ≥ 0 ou « — » selon la règle A6 ; pastille « En attente · pH à la consigne ».
6. **Home Assistant** : les nouvelles entités apparaissent ; « Dose de Base Apprise » conserve sa valeur hors session (contrairement à « Dose de Base » qui retombe à 0).
7. **ORP** : mêmes scénarios 1-3 sur la carte ORP (2 lignes).
8. **Rétro-compat** : `app.js` nouveau + `/data` amputé des champs (mock) → cartes masquées, aucune erreur console, aucun bloc retiré ne réapparaît.

## Notes pour agent
- **Piège n° 1 du projet : parité `/data` ↔ WS** (3 récidives en 3 versions). Un champ WS-seul ne disparaît pas, il CLIGNOTE. La pause mélange est **déjà** dans ce cas : la corriger fait partie de la spec. Prouver par grep des deux côtés, consigner dans la spec.
- Le tick FF n'est PAS le bon endroit pour produire la valeur apprise : il ne tourne que sous filtration hors purge. Tout ce que la carte affiche « à froid » (FF apprise, rattrapage) est calculé **à la lecture**.
- Ne pas confondre `ph_ff_ml_per_min` (appliqué maintenant, 0 hors session — sémantique conservée pour HA et l'historique) et `ph_ff_learned_ml_per_min` (connu, stable). Les deux coexistent ; on n'en renomme aucun.
- La valeur FF affichée est `min(charge, plafond)`, comme le journal — jamais la charge brute (l'utilisateur lirait un nombre que l'appareil n'appliquera pas).
- Les factorisations A1/A3 doivent laisser la suite `test_native_ph_feedforward` **intacte** : c'est elle qui prouve que le tick n'a pas bougé. Ne pas la « moderniser » au passage.
- UI : **un seul point de décision pilote `hidden`** (`updatePhDosingCard()`), règle existante de la page. Supprimer les deux fonctions remplacées, pas les laisser mortes.
- `DOSE_BLOCK_CODES` reste la source unique des libellés de refus ; la pastille consomme un nouveau champ `chip` court, le modal garde `label`.
- Écran tactile (`esp32-pool-controller-screen`, autre repo) : la pastille 4 états serait le seul élément à porter si la page LVGL pH est un jour alignée — **hors périmètre**.
- ~~Déploiement : pas pendant la fenêtre d'observation 72 h (03/09 07:31 → ~06/09).~~ **PÉRIMÉ** : la fenêtre est refermée depuis le 06/09. Livré en **v2.46.0** le 07/09, sans contrainte de calendrier.

## Hors périmètre
- Toute modification du calcul du feedforward, du PI, des latchs, des plafonds ou des quotas.
- Ajout d'un code `PhFfStatus` ou `DoseBlockedCode`.
- Jauge/barre graphique (itération 2 éventuelle : une barre fine sous Demande, pattern `.daily-dose__bar-wrap`).
- Durée écoulée de session dans la pastille.
- Affichage détaillé de l'anneau `ph_load7` (jours, mL/min par jour) — spec « transparence de l'apprentissage » distincte si le besoin se confirme.
- Écran LVGL du projet séparé.

## Notes techniques — relevé du `test-engineer` (2026-09-07)

### Tests livrés
- `test/test_native_dose_composition/` — 41 cas. Cibles : `doseComposition()`
  (`src/dosing_logic.h/.cpp`), `phFeedforwardLearnedFlow()` et
  `phFeedforwardServedStatus()` (`src/ph_feedforward_logic.h`). Le premier test
  du fichier est le **contre-exemple exigé par `pool-chemistry`** (`ff = 1,41`,
  `p = −3,00`, `i = 0`, régime `Full` ⇒ `demand == 1,41`) : c'est le seul test
  qui distingue la formule du firmware de `clamp(p + i + ff, 0, ampFlow)`, et
  l'écart qu'il mesure est un dosage réel contre un dosage nul.
- `test/test_native_ws_data_parity/` — 9 cas, sentinelle structurelle du second
  axe de parité (`/data` ↔ trame `sensor_data`), sur le patron de
  `test_native_ws_config_parity`. Portée ancrée sur `handleGetData` /
  `_buildSensorJson` + marqueurs `PARITE-DATA`. Trois tables d'exception :
  **18** clés WS-seules d'état vivant (dette antérieure, sous **cliquet** qui
  interdit d'en ajouter), **8** clés dual-sourcées WS + autre route HTTP (route
  nommée, catégorie absente de l'axe config), **5** clés `/data`-seules dont le
  receveur imbriqué `flow_assist`. Décompte constaté : **117** clés WS,
  **96** clés `/data`, **26** WS-seules, **5** `/data`-seules.
- Campagne de **mutation** : 11 mutants (9 sur la logique, 2 sur la sentinelle),
  **aucun survivant**, sources restaurées bit-pour-bit (sha256 vérifié).

### Décomptes de l'axe `/data` ↔ `sensor_data` (après migration du 07/09)
**119** clés dans la trame `sensor_data`, **96** dans `GET /data`, **26**
WS-seules, **3** `/data`-seules. Le bloc `PARITE-DATA` porte **14** clés,
ensembles **identiques** des deux côtés (assertion dans les deux sens).
Les deux booléens `*_pump_flow_uncharacterized` sortent des `/data`-seules
**sans** entrer dans les WS-seules : ils sont désormais des **deux** côtés — le
cliquet `kWsOnlyLiveCeiling = 18` est donc inchangé, **vérifié par mesure**
(26 WS-seules constatées = 18 « état vivant » + 8 dual-sourcées) et non supposé.

### LA SENTINELLE A ATTRAPÉ UN DÉFAUT AVANT LA REVUE
Le premier passage de `test_native_ws_data_parity` a fait apparaître un défaut
**réel** de ce cycle : le pied « pompe non caractérisée » de la nouvelle carte
clignotait. `_renderDoseUncharacterizedFoot()` lisait
`ph_pump_flow_uncharacterized` / `orp_pump_flow_uncharacterized` depuis
`latestSensorData`, alors que ces clés n'étaient servies que par `GET /data` :
`latestSensorData` étant **remplacé en entier** par la dernière charge reçue
(aucune fusion), la clé valait `undefined` après chaque push WS (~5 s) et le
pied restait masqué (`!== true`), pour ne réapparaître qu'après une charge
`/data` (~30 s).

C'est le **piège n° 1 du projet dans son sens INVERSE** : d'ordinaire c'est la
charge `/data` qui écrase un champ WS-seul ; ici c'est le push WS qui effaçait
un champ `/data`-seul. L'axe config n'avait pas à traiter ce sens — c'est
précisément ce que le second axe ajoute.

Le correctif est la **parité** (migration des deux booléens dans le bloc
`PARITE-DATA`, servis dans le même passage), pas une mémorisation côté client.
La sentinelle est alors passée au rouge une seconde fois, sur les deux
**exceptions devenues mortes** — le mécanisme prévu pour empêcher la table de
pourrir a fonctionné du premier coup, sur un vrai changement. `kDataOnly` est
passée de 5 à 3 entrées.

### DÉCISION CONSIGNÉE : élagage de la trame REFUSÉ (07/09, coordinateur)
Marge de la trame capteurs : **136 o** sous `kWsFrameBufBytes = 3584` (pire cas
**3 448 o**). Le pire cas **passe**, y compris pendant l'escalade de sécurité pH.
L'élagage proposé — retirer `ph_integral` (**23 o**) et `ph_integral_term`
(**26 o**), soit **49 o** rendus, marge portée à **185 o**, ces deux champs
n'ayant plus **aucun consommateur client** — est **refusé**, pour deux motifs à
lire avant toute nouvelle tentative :
1. cela les rendrait `/data`-seuls, donc **deux exceptions de plus** dans la
   sentinelle, sur l'axe précis qui vient de produire un défaut. Payer en
   exceptions ce que l'on gagne en octets, le jour même où l'on installe la
   garde, est un mauvais échange ;
2. relever `kWsFrameBufBytes` coûterait **+512 o de `.bss`**, donc autant de RAM
   en moins pour le tas, pendant que **bug-080** documente une famine mémoire
   **active**.

Le levier reste valable pour une **future** feature qui manquera de place : les
**49 o** et les deux clés concernées (`ph_integral`, `ph_integral_term`) sont
consignés ici pour que le chiffre soit déjà disponible.

## Notes d'implémentation (clôture, 2026-09-07)

### Six défauts trouvés par LECTURE DU CODE — aucun par lecture de la spec

C'est le fait marquant de ce cycle, et il mérite d'être écrit tel quel : **les six
défauts ont été trouvés en lisant le code, jamais en relisant cette spec.** Trois
d'entre eux étaient même **propagés par la documentation** — d'où l'ampleur du lot
documentaire de clôture, qui coupe les racines au lieu de décrire le résultat.

| # | Défaut | Trouvé par | Racine documentaire coupée |
|---:|---|---|---|
| 1 | **Champ « appliqué » impossible** : `min(demande, soutenable)` n'est pas ce que la pompe délivre (le tick borne à l'**amplitude**, la limite horaire agit **en aval par refus**) — sous-estimation de **17 %** | lecture du tick | INV-C1 dans cette spec ; avertissement dans `docs/API.md`, `page-ph.md`, `MQTT.md`, guide de régulation §7 |
| 2 | **Bornage en deux étages non réordonnable** : la borne basse porte sur le **couple** `p + i`, pas sur la somme des trois termes. `ff = 1,41`, `p = −3,00` ⇒ **1,41**, pas 0 | revue du module pur | `docs/subsystems/pump-controller.md` + contre-exemple en **premier test** |
| 3 | **Chiffrage du budget WS faux 4 fois** : confusion gabarit documentaire / plafond dur, et **mesure vivante** prise pour base — **+115 o** de correction sans qu'aucun champ soit ajouté | recompte clé par clé | section normative « deux trames, deux gabarits, un seul plafond dur » + **méthode** dans `ws-manager.md` ; R1 rectifié ici |
| 4 | **Pied de carte clignotant** : `ph_pump_flow_uncharacterized` était `/data`-seul, effacé par chaque push WS — **piège n° 1 dans son sens inverse**, introduit par une demande de l'agent principal | **la sentinelle**, avant la revue | **[ADR-0071](../../../docs/adr/0071-parite-data-trame-capteurs.md)** |
| 5 | **Instantané périmé hors dosage** : sans point de publication hors branche, la carte aurait été fausse **la majeure partie du temps** (7-8 min de session pour 15 min de pause) | blocage n°1 de la revue | commentaire sur place + `pump-controller.md` |
| 6 | **`ph_ff_ml_per_min` ne suivait pas le régime appliqué**, cassant la reconstitution `demande − FF = couple servi` | lecture croisée accesseur / tick | changement de sémantique annoncé au **CHANGELOG** |

### Ce que la spec a décrit et que le cycle a réécrit

1. **La valeur de la ligne « Dose de base apprise »** : la spec voulait la valeur *apprise* en valeur et le statut en note. Livré : la contribution **appliquée** en valeur (elle rend le total additionnable) et la dose **apprise** en note. Le besoin d'origine — « distinguer *rien d'appris* de *1,41 appris, en attente* » — est satisfait par la note.
2. **La note du total** : « plafonnée · 5,20 demandés » ⇒ « apport moyen possible 4,33 mL/min **en moyenne sur l'heure** ». Le total reste la demande (INV-C1).
3. **Le nom et la signature du module pur** : `phDoseComposition(ff, p, i, sustainable)` ⇒ `doseComposition(kp, error, kiIntegral, deadband, ffServed, ampFlow, regime)` dans `src/dosing_logic.h`. Le `sustainable` a disparu des entrées — il n'a jamais borné quoi que ce soit dans le tick.
4. **La bande morte est LUE, pas recalculée** : elle vient du verdict de porte (`phSessionGate`) — 0,01 session ouverte, 0,05 fermée. La reconstruire depuis les seuils mentirait dans `]0,01 ; 0,05]`, mécanisme exact du défaut v2.26.0.
5. **La pastille a un 6ᵉ cas**, non prévu : « En attente » **seul**, quand aucun refus n'est servi mais que le terme P est non nul. Il est atteignable *par construction* (porte sur le signal lent, composition sur l'erreur instantanée) et la ligne Demande porte la note qui l'explique.
6. **Une note de bornage du couple** a été ajoutée sur la ligne Rattrapage (pH) / Écart (ORP) : un trait de total sous des nombres qui ne s'additionnent pas, **sans note**, serait le pire des deux mondes.

### Ce qui reste ouvert

- **Plan de test manuel** (dernier critère d'acceptation) — exige l'appareil en fonctionnement.
- **Spec de suivi** : asymétrie du feedforward sur erreur vive franchement négative (gelée, cf. § Sécurité chimique).
- **Fenêtre résiduelle** des trois sorties anticipées d'`update()` (OTA, maintenance, assistant de débit) : l'instantané n'y est pas rafraîchi sans que `measuresValid` soit invalidé. Bornée, déclenchée par l'opérateur, **documentée non corrigée** — et tracée **au même endroit** que l'asymétrie de comptabilité de feature-110, parce que c'est la même chaîne de sorties anticipées.
- **Dette de 18 clés WS-seules** inventoriée par la sentinelle, sous cliquet : résorption par specs dédiées.
- **Deux captures d'écran à refaire** : `screenshots/07_pH.jpg` et `screenshots/08_ORP.jpg` (seule étape humaine du lot documentaire).

### Fichiers modifiés

**Firmware** : `src/dosing_logic.h`, `src/dosing_logic.cpp`, `src/ph_feedforward_logic.h`, `src/pump_controller.h`, `src/pump_controller.cpp`, `src/constants.h`, `src/web_routes_data.cpp`, `src/ws_manager.cpp`, `src/mqtt_manager.h`, `src/mqtt_manager.cpp`, `src/version.h`.
**Frontend** : `data/index.html`, `data/app.js`, `data/app.css`.
**Tests** : `test/test_native_dose_composition/`, `test/test_native_ws_data_parity/` (neufs) ; `test/test_native_ph_feedforward/` **inchangée**.
**Docs** : `docs/adr/0071-parite-data-trame-capteurs.md` (neuf), `docs/adr/README.md`, `docs/README.md`, `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/ws-manager.md`, `docs/subsystems/pump-controller.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/user/src/guide-utilisation.html`, `docs/user/src/guide-regulation.html`, `CHANGELOG.md`.
