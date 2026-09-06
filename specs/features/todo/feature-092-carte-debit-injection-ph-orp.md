## Métadonnées
- **ID** : feature-092
- **Slug** : carte-debit-injection-ph-orp
- **Type** : feature (observabilité de la régulation — aucun changement de dosage)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.34.x ou 2.35.0 — **pas de déploiement isolé pendant la fenêtre d'observation 72 h ouverte le 03/09/2026 07:31** (features 084/085/086) ; embarquer avec la livraison suivante (lot B de bug-080 et/ou feature-091)
- **Dernière mise à jour** : 2026-09-06 (deux points à rejouer avant ouverture — voir en tête : budget WS périmé, placement à arbitrer avec le bloc TTT de feature-096)
- **Spec(s) liée(s)** : feature-085 (dose de base apprise — origine de la ligne `#ph_ff_line`), feature-084 (intégrale, anneau `ph_load7`), feature-086 (limite d'actionneur : bloc Capacités, bandeaux), bug-089 (parité WS ↔ config — même famille de piège), bug-082 (champ WS-seul)
- **Historique** : première version de cette spec (03/09 matin, slug `dose-de-base-apprise-lisible-hors-session`) ne traitait que le tiret muet de la ligne dose de base ; élargie le même jour, sur décision utilisateur, en refonte de la présentation des débits — l'`ux-ui-designer` a été consulté en amont (pré-arbitrages ci-dessous, à confirmer pendant le cycle).

## ⚠️ À REJOUER AVANT OUVERTURE — cette spec ignore feature-096 (ajouté le 06/09/2026)

Cette spec a été écrite le **03/09**. La carte « **Atteinte de la cible** »
(`feature-096`, blocs `#ph-ttt-block` / `#orp-ttt-block`) a été déployée le
**05/09 en v2.37.0**. La spec n'en contient donc **aucune mention** — ce n'est pas
un arbitrage, c'est un angle mort chronologique.

**Ce qui va bien** : il n'y a **pas de collision de placement**. Le bloc TTT vit
*à l'intérieur* de `.ph-stats` (2ᵉ colonne, `index.html` l. 719) ; la carte
`#ph-card-dosing` s'insère *après* `.ph-stats`. Aucun élément que cette spec retire
ou déplace n'appartient au bloc TTT. Les sept pastilles ne bougent pas.

**Deux points à reprendre, cependant.**

### R1 — Le budget WS chiffré ici est PÉRIMÉ

Le § Contraintes techniques raisonne sur « gabarit `StaticJson<2816>`, pire cas
2 745 o ». Depuis :

| | gabarit | motif |
|---|---|---|
| au 03/09 (base de ce chiffrage) | **2816** | — |
| bug-089, v2.39.0 | **2944** | 15 clés de configuration ajoutées à la trame |
| état au 06/09 | **3072** | marge de **80 o** seulement sous `kWsFrameBufBytes = 3584` |

La trame porte en outre **21 occurrences de champs `ttt`** (feature-096).

⚠️ **Refaire le chiffrage sur 3072, pas sur 2816**, avant de trancher entre bump du
gabarit et retrait compensatoire. La marge décrite dans cette spec n'existe plus.
Rappel du mode de défaillance : le dépassement ne tronque pas, il **saute le push**
— et le pire cas de taille coïncide avec l'escalade de sécurité pH, c'est-à-dire le
moment où perdre la trame coûte le plus cher (cf. bug-089).

### R2 — Le placement doit être rejoué par `ux-ui-designer`

Il a été consulté le **03/09**, donc **sans voir le bloc TTT**. Or l'enchaînement
que cette spec justifie — « mesure → ce que fait la régulation maintenant →
réglages » — place désormais un **indicateur de performance** (« la cible a été
atteinte en 3 h 44 ») juste au-dessus d'une carte disant **ce que la régulation
demande à l'instant**. Deux temporalités très différentes se succèdent sans
transition.

Ce n'est pas forcément à changer, mais **personne ne l'a arbitré**. À trancher
pendant le cycle, en ayant les deux blocs sous les yeux.

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
| Demande (total, `border-top`) | `ph_applied_ml_per_min` en gras | nominal : « sur 4,33 possibles » (`ph_sustainable_ml_per_min`) ; bridé : « plafonnée · 5,20 demandés » (`ph_demand_ml_per_min`) ; plafond `null` : pas de note, pied « pompe non caractérisée » | nouveaux + existant |

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
Régulation P-pure : lignes « Écart courant  {`orp_p_term_ml_per_min`}  (612 → 650 mV) » et « Demande  {`orp_applied_ml_per_min`}  sur X possibles ». Pastille identique (les champs `orp_pulse_session`, `orpMixingDelayActive`, `orp_mix_remaining_s`, `orpDoseBlockedCode` existent déjà — `_doseBlockInfo(json, prefix)` est préfixé). La carte montre au passage *pourquoi* le chlore est régulé plus simplement. **Repli accepté** si l'architect juge le budget trop serré cette itération : carte ORP avec pastille + ligne Demande seule (P = demande en P-pure, la ligne Écart devient redondante).

### Firmware — lecture seule

Nouveaux champs, servis **à l'identique** sur `GET /data`, trames WS `sensor_data`, MQTT (retain, dédup, auto-discovery HA `entity_category: diagnostic`, `unit: mL/min`, `state_class: measurement`) :

| Champ | Sémantique | `null` quand |
|---|---|---|
| `ph_ff_learned_ml_per_min` | `min(charge médiane 7 j, kPhFfMaxSustainableFraction × soutenable)` — le nombre que le journal annonce (« dose de base apprise 1.41 … plafond 2.60 »), calculé **à la lecture** depuis `_phLoadObs` (jamais depuis un instantané du tick) | historique < `kPhIntegralRestoreMinHistoryDays` ou soutenable non fini/non positif |
| `ph_p_term_ml_per_min` | kp × erreur signée courante (positive = il faut doser), depuis la mesure filtrée corrigée et la consigne | mesure invalide (filtre non prêt, figé, purge, NaN) |
| `ph_demand_ml_per_min` | FF appliqué + P + I, **avant** plafond | P `null` |
| `ph_applied_ml_per_min` | `min(demande, soutenable)`, borné ≥ 0 | P `null` |
| `orp_p_term_ml_per_min`, `orp_demand_ml_per_min`, `orp_applied_ml_per_min` | idem ORP (demande = P) | idem |

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
- **Budget JSON — point dur** : trame WS actuelle gabarit `StaticJson<2816>`, pire cas 2 745 o, sous `kWsFrameBufBytes = 3584` ([ws_manager.cpp:365-374](../../../src/ws_manager.cpp#L365-L374)). Ajouts pire cas : 7 champs float (~30-34 o chacun ≈ 230 o) + 4 champs pause mélange sur `/data` (~90 o). L'architect doit chiffrer et décider : bump du gabarit (marge sous 3584 à préserver) ou retrait compensatoire (`ph_integral` en pH·s n'est plus affiché par l'UI — candidat au retrait de la trame WS **si** aucun autre client ne le consomme ; MQTT le garde). Mettre à jour les commentaires de budget dans `ws_manager.cpp`, `web_routes_data.cpp`, `docs/API.md`, `ws-manager.md`.
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
- [ ] A1 — `ph_feedforward_logic.h` : `phFeedforwardLearnedFlow()` factorisée depuis le § 4 de `phFeedforwardTick` (même `min`, mêmes gardes `isfinite`/`> 0`) ; le tick l'appelle.
- [ ] A2 — `ph_feedforward_logic.h` : `phFeedforwardServedStatus()` regroupant la traduction actif → inactif existante et la nouvelle 3 → 0 si `days ≥ min`.
- [ ] A3 — fonction pure de composition `phDoseComposition(ff, p, i, sustainable)` → {demande, appliqué} (ou équivalent extrait du calcul PI existant, à résultat identique) — module pur testable.
- [ ] A4 — `pump_controller.h` : accesseurs `const` pH + ORP listés ci-dessus ; `getPhFeedforwardStatus()` sur A2 ; statuts 1, 2, 4 strictement inchangés.
- [ ] A5 — `/data` + WS : 7 nouveaux champs + 4 champs pause mélange sur `/data` ; **preuve de parité par grep des deux côtés consignée dans la spec** ; budget chiffré et commentaires mis à jour ; décision gabarit tracée.
      ⚠️ **Chiffrer sur le gabarit RÉEL (3072 au 06/09, marge 80 o), pas sur les
      2816 de ce document** — voir R1 en tête de spec.
- [ ] A8 — **Placement rejoué avec le bloc TTT** (`feature-096`, absent lors de la
      consultation `ux-ui-designer` du 03/09) — voir R2 en tête de spec.
- [ ] A6 — règle firmware « pH à la consigne » pour la pastille de priorité 5 (à défaut : champ booléen ou réutilisation d'un code de refus existant) — décision architect.
- [ ] A7 — MQTT : topics + auto-discovery HA.
- [ ] B1 — `index.html` : `#ph-card-dosing` conforme aux maquettes ; retraits/déplacements exécutés (aucun id mort dans `app.js`).
- [ ] B2 — `app.js` : `updatePhDosingCard()` point de décision unique ; pastille 5 priorités ; chips courts codes 2, 10-14 ; dégradation selon § Cas limites ; fr-FR 2 décimales ; signe « − » sur P seulement.
- [ ] B3 — `#orp-card-dosing` + `updateOrpDosingCard()` (ou repli documenté).
- [ ] B4 — `app.css` : `.kv--dose`, `card__head` flex-wrap ; contraste pastille vérifié.
- [ ] C1 — `docs/features/page-ph.md` et `page-orp.md` réécrits (sections retirées, carte documentée, table de pastille) ; `docs/API.md`, `docs/MQTT.md`, `docs/subsystems/pump-controller.md`, `ws-manager.md`, `CHANGELOG.md`.
- [ ] C2 — `guide-utilisation.html` (pages pH/ORP) + `guide-regulation.html` (équation illustrée par la carte) → `./tools/build_user_pdfs.sh` ; signaler les **deux captures** à refaire.
- [ ] D1 — Tests natifs A1/A2/A3 : NAN sous 3 jours ; NAN soutenable non fini ; plafonnement FF quand charge > fraction × soutenable ; charge nulle ⇒ 0 (pas NAN) ; traduction 3 → 0 si days ≥ 3 et 3 → 3 sinon ; 1 → 0 hors session ; 2 et 4 jamais traduits ; composition : demande < plafond ⇒ appliqué = demande ; demande > plafond ⇒ appliqué = plafond ; demande < 0 ⇒ appliqué = 0.
- [ ] D2 — **Mutation** (chacune doit tuer ≥ 1 test) : `min` → `max` dans A1 ; retrait de la traduction 3 → 0 ; `>=` → `>` sur le seuil de jours ; retrait de la borne ≥ 0 de l'appliqué ; `min` → `max` sur le plafond de composition.
- [ ] D3 — suite `test_native_ph_feedforward` existante **inchangée et verte** (preuve de non-régression du tick) ; `pio run` sans nouveau warning ; `pio test -e native` 100 % ; `./build_fs.sh` dans le budget.

## Nouvelles signatures / API
- Champs `/data` + WS `sensor_data` : `ph_ff_learned_ml_per_min`, `ph_p_term_ml_per_min`, `ph_demand_ml_per_min`, `ph_applied_ml_per_min`, `orp_p_term_ml_per_min`, `orp_demand_ml_per_min`, `orp_applied_ml_per_min` (float 2 décimales | null) ; `/data` gagne `phMixingDelayActive`, `ph_mix_remaining_s`, `orpMixingDelayActive`, `orp_mix_remaining_s` (déjà sur WS).
- Topics MQTT : `{base}/ph_ff_learned_ml_per_min`, `{base}/ph_p_term_ml_per_min`, `{base}/ph_demand_ml_per_min`, `{base}/ph_applied_ml_per_min` (+ ORP) — retain, dédup, auto-discovery HA `sensor` diagnostic.
- Fonctions publiques : `float getPhFeedforwardLearnedFlow() const; float getPhPTermFlow() const; float getPhDemandFlow() const; float getPhAppliedFlow() const;` (+ ORP) dans `pump_controller.h` ; pures : `phFeedforwardLearnedFlow`, `phFeedforwardServedStatus`, `phDoseComposition` dans `src/ph_feedforward_logic.h` (ou module pur dédié).
- Constantes : **aucune nouvelle**.
- Contrat `PhFfStatus` : **inchangé**. Contrat `DoseBlockedCode` : **inchangé** (seuls des libellés courts côté client sont ajoutés).

## Sécurité chimique
- Dosage impliqué : **non** — lecture seule. Les nouveaux champs sont des projections de grandeurs déjà calculées, jamais des entrées.
- Limites journalières / horaires concernées : aucune (le soutenable est **affiché**, pas modifié)
- Filtration requise avant action : sans objet
- Calibration capteur modifiée : non
- Algorithme PID touché : **non** — A1 et A3 factorisent des expressions existantes sans en changer le résultat (preuve : suite native existante intacte + tests D1)
- Risque identifié : aucun sur l'eau. **Question posée à `pool-chemistry`** : afficher le terme P **signé** (« −0,35 mL/min » quand le pH est sous consigne) est-il acceptable pédagogiquement, ou faut-il conserver la règle « valeurs absolues » du bloc intégral ? Recommandation UX : signé, sur P seulement.

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — application des principes déjà actés (ADR-0055 observabilité du FF, ADR-0049 codes machine mappés côté client, patron « traduction d'un instantané périmé côté accesseur »).
- **Sujet pressenti** : aucun. Deux décisions **deviendraient** ADR si prises : (a) ajouter un code à `PhFfStatus` plutôt que traduire (l'option traduction est recommandée pour l'éviter) ; (b) retirer `ph_integral` (pH·s) de la trame WS pour compenser le budget (contrat externe modifié) — l'architect tranche.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /data` — 7 champs de débit ajoutés ; 4 champs pause mélange ajoutés (parité WS) ; sémantique de `ph_ff_status = 3` précisée (jamais servi si l'anneau a ≥ 3 jours exploitables).

### Champs WebSocket
- `sensor_data` — 7 champs de débit, identiques à `/data`.

### Topics MQTT
- 8 topics (4 pH + 3 ORP + …, voir § Signatures), retain + dédup, entités HA diagnostic.

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
- aucune (sauf décision (a) ou (b) ci-dessus)

## Critères d'acceptation
- [ ] Hors session, avec ≥ 3 jours exploitables, la ligne Dose de base apprise affiche `X,XX mL/min` + « appliquée à la prochaine session » ; pendant une session, la même valeur sans note et la pastille « Injection en cours ».
- [ ] Pendant une pause mélange, la pastille affiche « Pause mélange · N min » et **ne clignote pas** entre trame WS et charge `/data` (parité vérifiée par `curl /data` : les 4 champs de pause mélange présents).
- [ ] Le lendemain d'un archivage portant l'anneau à 3 jours, la carte quitte « en apprentissage » **avant** le démarrage de la filtration (≤ 30 s après minuit).
- [ ] Filtration arrêtée : Écart et Demande « — », Dose de base et Rattrapage affichés, pastille « En attente · Filtration arrêtée » en gris (jamais rouge).
- [ ] Panne sonde (code de ton `fault`) : pastille rouge « Dosage suspendu · … » ; le modal filtre reste la source du détail.
- [ ] Demande > soutenable : ligne Demande = soutenable en gras + note « plafonnée · X demandés » + pied limite horaire ; le bandeau `#ph_budget_banner` n'existe plus dans le DOM.
- [ ] Pendant une session active, `ph_ff_learned_ml_per_min == ph_ff_ml_per_min` (à l'arrondi) et `ph_applied_ml_per_min ≈ FF + P + I` borné (vérifiable au journal d'ouverture de session).
- [ ] `grep` des 7 nouveaux champs dans `web_routes_data.cpp`, `ws_manager.cpp`, `mqtt_manager.cpp`, `app.js` → présents dans les quatre ; `grep phMixingDelayActive web_routes_data.cpp` → présent.
- [ ] Les 5 mutations D2 tuent chacune au moins un test ; `test_native_ph_feedforward` existante inchangée et verte.
- [ ] Mobile 360 px : aucune valeur tronquée, pastille sous le titre, contraste pastille neutre ≥ 4,5:1.
- [ ] Tous les libellés utilisateur sont en français ; `pio run` sans nouveau warning ; `./build_fs.sh` passe sans dépasser la partition.
- [ ] Comportement nominal testé manuellement selon le plan ci-dessous.

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
- Déploiement : **pas pendant la fenêtre d'observation 72 h** (03/09 07:31 → ~06/09). Embarquer avec la livraison suivante.

## Hors périmètre
- Toute modification du calcul du feedforward, du PI, des latchs, des plafonds ou des quotas.
- Ajout d'un code `PhFfStatus` ou `DoseBlockedCode`.
- Jauge/barre graphique (itération 2 éventuelle : une barre fine sous Demande, pattern `.daily-dose__bar-wrap`).
- Durée écoulée de session dans la pastille.
- Affichage détaillé de l'anneau `ph_load7` (jours, mL/min par jour) — spec « transparence de l'apprentissage » distincte si le besoin se confirme.
- Écran LVGL du projet séparé.
