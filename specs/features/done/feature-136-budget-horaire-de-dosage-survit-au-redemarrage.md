# feature-136 — Le budget horaire de dosage survit au redémarrage

## Métadonnées
- **ID** : feature-136
- **Slug** : budget-horaire-de-dosage-survit-au-redemarrage
- **Type** : feature — **fermeture d'une exposition chimique résiduelle**
- **Statut** : implémenté — tests natifs verts, revue en cours (voir « Notes d'implémentation » en fin de fichier ; ne pas déplacer vers `done/`, l'agent principal s'en charge après clôture)
- **Priorité** : **haute** — c'est la seule des trois specs de suivi du 20/09 qui porte un risque de **sur-dosage**, pas de sous-dosage
- **Version cible** : 2.54+
- **Dernière mise à jour** : 2026-09-20
- **Spec(s) liée(s)** : `specs/features/done/bug-134-…` (**c'est là que le défaut a été trouvé**, et c'est lui qui a emporté le GO de ce lot) · ADR-0034

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** — budget horaire, anti-cycling, pause de mélange
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] **Historique / persistance** — NVS, usure, atomicité
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT**
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

`DosingState` (`src/pump_controller.h`) est **entièrement en RAM** : `usedMs`,
`windowStart`, `lastStartTime`, `cyclesToday`. Seuls les **millilitres
journaliers** et les minutes de filtration sont persistés en NVS
(`src/config.cpp`).

**Conséquence : tout redémarrage remet à zéro le budget horaire, l'anti-cycling
et la pause de mélange.** À 10 min/h de pH, **deux redémarrages dans l'heure
autorisent jusqu'à 20 minutes d'acide**, seul le plafond journalier faisant
barrage.

## Contexte — comment ce défaut a été trouvé

Relevé le **2026-09-20** par le gate `pool-chemistry` de `bug-134`, en instruisant
une tout autre question : *« les 15 min de pire cas avant reboot sont-elles
acceptables ? »*

Sa réponse a inversé le compromis apparent :

> **Retarder un reboot n'assouplit rien ; le déclencher à tort assouplit une
> garde de dosage.**

C'est cet argument qui a emporté le GO de `bug-134`. Mais il laisse une dette
explicite : **`bug-134` réduit la FRÉQUENCE des redémarrages inutiles, il ne
supprime pas l'exposition.** Un redémarrage **légitime** — OTA, coupure secteur,
watchdog matériel, auto-guérison mémoire — rend exactement le même budget neuf.

### Ce que le projet sait déjà, et qui aggrave le sujet

- Le contrôleur **redémarre volontairement** dans plusieurs cas : watchdog de
  connectivité, auto-guérison mémoire, disjoncteur de boot-loop de l'historique.
- `kAutoHealMaxReboots = 3` autorise jusqu'à **trois** redémarrages automatiques
  dans une fenêtre avant d'entrer en mode maintenance.
- Le journal du 19/09 portait déjà `Auto-guérison : 1 reboot(s) auto récent(s)`.
- Les déploiements OTA sont **hebdomadaires** selon le rythme du projet.

### Ce que cela fait concrètement

| Grandeur | Persistée ? | Effet d'un redémarrage |
|---|---|---|
| millilitres journaliers | ✅ NVS | conservés |
| minutes de filtration | ✅ NVS | conservées |
| **`usedMs`** (budget horaire) | ❌ RAM | **remis à zéro** |
| **`windowStart`** (début de fenêtre) | ❌ RAM | **remis à zéro** |
| **`lastStartTime`** (anti-cycling) | ❌ RAM | **remis à zéro** |
| **`cyclesToday`** | ❌ RAM | **remis à zéro** |

## Comportement attendu

- Le **budget horaire** survit au redémarrage : un appareil qui a consommé 8 des
  10 minutes de sa fenêtre en retrouve 8 consommées, pas 0.
- L'**anti-cycling** survit : on ne redémarre pas une pompe juste après un reboot
  si le délai minimal n'est pas écoulé.
- La **pause de mélange** survit : elle existe pour laisser le produit se
  disperser avant de mesurer à nouveau, et un redémarrage ne la rend pas caduque.
- La **fenêtre glissante** reprend à son vrai point d'origine, pas à l'instant du
  démarrage.

### Cas limites — et ils sont le cœur du sujet
- **Horloge invalide au démarrage** (RTC morte, NTP pas encore synchronisé) :
  impossible de savoir combien de temps s'est écoulé. **Que vaut le budget dans
  ce cas ?** Fail-closed (budget considéré comme consommé) est le comportement
  sûr, mais il bloquerait le dosage au pire moment. **C'est l'arbitrage central.**
- **Coupure longue** (plusieurs heures) : la fenêtre horaire est de toute façon
  périmée, le budget doit repartir. À partir de quelle durée ?
- **Redémarrage pendant une injection en cours** : `usedMs` n'a pas été
  comptabilisé jusqu'au bout. Compter le temps écoulé, ou l'ignorer ? Le compter
  est conservateur.
- **Usure NVS** : persister à chaque tick est exclu. Quelle granularité ? Quel
  coût en écritures par an ?
- **Perte de la NVS** (effacement, partition corrompue) : fail-closed.

## Comportement actuel

`DosingState` est reconstruit **vide** à chaque démarrage. Seuls les compteurs
journaliers sont restaurés depuis la NVS — le journal le montre :
`[Config] Compteurs journaliers restaurés: pH=480 mL, ORP=1141 mL`.

Aucun message ne signale que le budget **horaire**, lui, vient d'être remis à zéro.

## Contraintes techniques

- **Usure NVS** : c'est la contrainte dimensionnante. Une écriture par cycle de
  dosage est probablement acceptable ; une par tick ne l'est pas. À chiffrer en
  écritures par an, avec l'endurance réelle (~100 k cycles, avec wear-levelling).
- **L'horloge doit être valide** pour recalculer une fenêtre glissante. Le projet
  dispose d'une RTC DS3231 **et** de NTP, plus un `isTimeValid()` déjà utilisé
  ailleurs. La mémoire RTC de l'ESP32 survit à un reset logiciel mais **pas** à
  une coupure secteur — les deux cas doivent être distingués.
- Le dosage s'exécute sous `_mutex` : ne pas allonger la section critique, et
  **aucune écriture NVS** en la tenant.
- La logique de fenêtre glissante doit être **pure** (ADR-0017), testable
  nativement, y compris ses cas d'horloge invalide.
- `esp_task_wdt_reset()` sur tout chemin allongé.
- Ne pas toucher aux plafonds eux-mêmes, ni à la garde de présence d'eau, ni aux
  limites journalières.

## Fichiers potentiellement concernés
- `src/pump_controller.{h,cpp}` — `DosingState` et son cycle de vie
- `src/config.cpp` — persistance NVS (le patron des compteurs journaliers existe)
- un module pur `src/dosing_window_logic.{h,cpp}` (ADR-0017)
- `src/constants.h` — clés NVS, granularité de persistance, seuil de péremption
- `docs/subsystems/pump-controller.md`, `docs/adr/`

## Livrables — check-list
- [x] A1 — **réécrit par le gate** : `usedMs`, `windowStart`, `cyclesToday`, `cyclesDayStart`, `_mixingEndMs[2]`, ring anti-rafale persistés. `lastStartTime`/`lastStopTime` **exclus avec motif** (voir Notes d'implémentation)
- [x] A2 — **Granularité de persistance** choisie (30 s pendant pompe-ON + événements) et **chiffrée en écritures par an** (≈ 62 000/an, 271 ans avant l'endurance garantie)
- [x] A3 — Reconstruction de la fenêtre glissante au démarrage, à partir d'une horloge **vérifiée** (T1) + d'un compteur monotone indépendant de l'horloge (T2)
- [x] A4 — **Comportement sur horloge invalide**, tranché (T3, fail-closed option (d) : une seule fenêtre, pas un blocage) et testé
- [x] A5 — Seuil de **péremption** : deux horloges distinctes (fenêtre 1 h / jour des cycles 24 h), formule exacte, testée
- [x] A6 — Journalisation au démarrage : source retenue + écoulé + statut, par voie
- [x] D1 — Tests natifs (18 cas, `test/test_native_dosing_window/`) : reprise à chaud, coupure courte, coupure longue, horloge invalide, horloge qui recule, dépassement 24,8 j
- [x] D2 — **Mutation** : mutants nommés et tués (M4 = omission `cyclesDayStart`, M8 = frontière `>=`/`>`, etc.) — voir Notes d'implémentation
- [x] C1 — `docs/subsystems/pump-controller.md`
- [x] C2 — `CHANGELOG.md`
- [x] C3 — ADR-0089 créé (le compromis est structurant : critères 1, 2 et 4)

## Sécurité chimique
- **Dosage impliqué : OUI, directement.** C'est le budget d'injection lui-même.
- **Risque du défaut actuel** : **sur-dosage**. Deux redémarrages dans l'heure
  autorisent le double du budget horaire de pH. C'est le seul des trois suivis du
  20/09 qui va dans la direction dangereuse.
- **Risque du correctif** : l'inverse. Un budget restauré à tort — horloge
  fausse, fenêtre mal reconstruite — **bloquerait** le dosage sans raison. Le
  fail-closed doit être choisi sciemment, et son coût énoncé.
- Limites journalières, garde de présence d'eau, PID : **non touchés**.
- **Gate `pool-chemistry` bloquant avant toute écriture de code**, et second
  passage après implémentation.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte imposée : toute garde temporelle
  future devra choisir entre RAM et NVS) · 2 (décision invisible : la granularité
  de persistance encode un arbitrage usure/fidélité) · 4 (verrouillage : un
  format persisté est un contrat qui survit aux mises à jour)
- **Sujet pressenti** : « Ce qu'un redémarrage a le droit d'oublier »

## Éléments visibles (pour doc-writer)
- Aucun endpoint, champ WebSocket ni topic MQTT nouveau.
- **Une ligne de journal nouvelle au démarrage** (ce qui a été restauré).
- Comportement observable : après un redémarrage, le dosage peut être refusé
  alors qu'il était autorisé auparavant — **c'est voulu**, et cela doit être dit
  clairement dans le CHANGELOG pour ne pas passer pour une régression.

## Critères d'acceptation
- [x] Après un redémarrage, le budget horaire consommé est **conservé**
- [x] L'anti-cycling et la pause de mélange survivent (pause de mélange en outre **armée** au restore si pompe-ON à l'écriture, condition C5)
- [x] Horloge invalide au démarrage ⇒ **fail-closed** (T3, une seule fenêtre), et c'est testé
- [x] Une coupure plus longue que la fenêtre fait repartir le budget
- [x] Le coût en écritures NVS par an est **chiffré** et tient dans l'endurance (≈ 62 000/an, 271 ans avant 100 000 cycles)
- [x] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués (à reconfirmer par `test-engineer`/`code-reviewer` si ce n'est pas déjà fait pour ce tour)

## Plan de test manuel
1. Doser jusqu'à consommer une partie du budget horaire, redémarrer, vérifier que
   le reliquat est correct.
2. Redémarrer après une coupure secteur de plus d'une heure : le budget repart.
3. Redémarrer RTC débranchée et sans réseau : le dosage est refusé jusqu'à ce que
   l'horloge soit valide.
4. Suivre le compteur d'écritures NVS sur une semaine et extrapoler.

## Notes pour agent

⚠️ **Ce lot va dans la direction dangereuse, contrairement aux deux autres suivis
du 20/09.** bug-135 et bug-137 corrigent des gardes qui refusent **à tort** —
erreur fail-closed, sans urgence. Celui-ci corrige une garde qui **autorise à
tort**. C'est pourquoi il est en priorité haute.

⚠️ **L'horloge est le point dur, pas la persistance.** Écrire quatre champs en
NVS est mécanique. Savoir **combien de temps s'est écoulé** quand l'appareil
redémarre sans horloge fiable ne l'est pas — et s'y tromper transforme une
protection en blocage.

⚠️ Le projet a déjà une mémoire RTC utilisée par le disjoncteur de boot-loop et
par la dispense de purge : **regarder comment ces mécanismes distinguent un reset
logiciel d'une coupure secteur** avant d'en réinventer un.

## Hors périmètre
- Les **valeurs** des plafonds horaires et journaliers.
- Le mode maintenance et `kAutoHealMaxReboots` — leur révision est un sujet à part,
  déjà signalé comme tel dans bug-134.
- La réduction du **nombre** de redémarrages : c'est bug-134, livrée.

---

## Gate `pool-chemistry` P1 (conception) — 2026-10-08 : **GO SOUS 12 CONDITIONS**

### ⚠️ Le livrable A1 de cette spec est FAUX et doit être réécrit (condition C1)

Ce qui doit réellement survivre, vérifié par lecture du code :

| Champ | Pourquoi |
|---|---|
| `usedMs` · `windowStart` | budget horaire — le cœur du lot |
| `cyclesToday` · **`cyclesDayStart`** | sans le second, `pump_controller.cpp:331` remet le compteur à zéro **au premier tick** ⇒ **no-op silencieux** qui laisse croire la garde rétablie |
| **`_mixingEndMs[2]`** | pause de mélange (15 min pH / 20 min ORP) — **hors `DosingState`**, donc absente de A1 alors qu'un critère d'acceptation l'exige |
| **`_dosingCycleHistory[2][20]` + `_dosingCycleHistoryIdx[2]`** | ring anti-rafale — **absent de la spec**, voir ci-dessous |

**Exclus, motif accepté** : `lastStopTime` (0 lecture dans tout le firmware) et `lastStartTime`
(lu seulement pour une durée journalisée).

### Le ring anti-rafale entre dans le lot (condition C5 du plan, point 5 du gate)

`_dosingCycleHistory[2][20]` (fenêtres 1 min / 15 min, `kMaxDosingCyclesPerMinute = 6`,
`kMaxDosingCyclesPer15Min = 20`) est RAM-only lui aussi. Un reboot efface **100 % d'une fenêtre
dont il n'a consommé que 1,7 %**.

**Aggravant décisif du gate** : les reboots concernés — watchdog, panic, auto-guérison —
**corrèlent avec le fonctionnement anormal**. La garde contre l'emballement disparaît
**précisément quand elle sert**. Avec `kAutoHealMaxReboots = 3`, trois rafales de 20 démarrages
sont possibles dans le même quart d'heure.

⚠️ Le coût annoncé par le plan était faux : **160 octets**, pas 80 (`uint32_t[2][20]`, plus les
index). Sans incidence sur la décision, mais l'instantané fait **≈ 216 o**, pas 48 (condition C9).

### ⚠️ C5 — TROU MANQUANT AU PLAN, bloquant

Si l'instantané porte « pompe-ON à l'écriture », il faut **armer la pause de mélange au
restore** : `_mixingEndMs[pompe] = now + kPhMixingDelayMs` (15 min) ou `kOrpMixingDelayMs`
(20 min).

Motif : le produit **a été injecté**, mais `notifyPhDose()` n'a jamais tourné — le reboot l'a
interrompu. Sans cet armement, **l'appareil redose sur une mesure non représentative du produit
déjà présent dans l'eau**. C'est un chemin de surdosage que ni la spec ni le plan n'avaient vu.

### T3 — l'arbitrage central : option (d), une seule fenêtre

Quand l'horloge est invalide et qu'aucune source de temps n'est disponible : poser `usedMs` à la
limite pour **une seule fenêtre**, puis retour au nominal.

**(a) blocage jusqu'à horloge valide est REFUSÉE**, et le motif est chimique, pas ergonomique :
RTC morte + sans réseau = blocage indéfini de l'acide. Sous charge basique permanente et avec
une régulation P pur (droop structurel), le pH sort de 7,20-7,50 et franchit `kPhOrpInhibit`
= 7,60 ; la fraction HOCl tombe de **52 % à pH 7,50 à 41 % à 7,70**, et la boucle ORP compense
par **surdosage de chlore**. *« Une protection qui crée un surdosage sur l'autre chimie n'est
pas une protection. »*

**(c) budget réduit est REFUSÉE** : sémantique neuve, et elle **autorise** un dosage sur un
budget antérieur inconnu — l'exposition même qu'on ferme.

**(d) l'emporte sur (b)** à comportement identique, parce que l'état est **déjà exposé partout**
(`getPhUsedMs()`, `ph_used_ms` en MQTT/WS, libellé « fenêtre horaire épuisée ») : aucun chemin
de refus muet. **Coût assumé** : au pire une heure de capacité perdue après un démarrage à froid
sans horloge, soit ≤ 10 min d'acide différés, rattrapables à la fenêtre suivante.

### T2 (RAM RTC) est chimiquement recevable

La garde horaire a besoin de **temps écoulé**, pas de calendrier. Exiger l'horloge murale seule
renverrait le cas **le plus fréquent** (OTA hebdomadaire) en T3 — soit **une heure de blocage à
chaque déploiement**. Chimiquement pire.

### Les 12 conditions bloquantes

1. **C1** — réécrire A1 avec le périmètre ci-dessus
2. **C2** — **aucun `millis()` brut persisté** ; toute estampille est rebasée au boot
3. **C3** — péremption : fenêtre 1 h, cycles 24 h, **formule exacte** (les confondre viderait
   l'anti-cycling chaque heure). Un seuil plus court que la fenêtre serait un **assouplissement**,
   pas un durcissement — la péremption *jette* le budget restauré
4. **C4** — écoulé inconnu mais instantané intact ⇒ **ne pas basculer en T3** : restaurer `usedMs`
   intégralement avec `windowStart = now`
5. **C5** — **armer la pause de mélange au restore** si pompe-ON (voir ci-dessus)
6. **C6** — ring : entrées > 15 min écartées ; écoulé inconnu ⇒ toutes à l'âge 0
7. **C7** — nommer le compteur monotone de T2 et **prouver par test on-target** qu'il traverse
   `esp_restart()` ; sinon retomber sur C4, **jamais** sur une durée de boot supposée
8. **C8** — **anti-blocage perpétuel** : la pénalité T3 et `cyclesDayStart` doivent eux-mêmes
   vieillir. Des reboots répétés ne doivent ni ré-armer la pénalité sans fin, ni repousser le
   rollover 24 h — sinon `maxCyclesPerDay` **ferme la régulation définitivement**
9. **C9** — recalculer la taille (≈ 216 o) ; inventaire chiffré de l'occupation `RTC_NOINIT`
   totale du projet, avec marge
10. **C10** — le budget restauré couvre **auto ET manuel** (ADR-0020) : une commande manuelle ne
    le contourne pas. Test natif explicite
11. **C11** — journal au démarrage énonçant chaque grandeur **et sa source** (T1/T2/pire/T3)
12. **C12** — mutants obligatoires, dont **omettre `cyclesDayStart`** : c'est le mutant clé, il
    reproduit le no-op silencieux

### Ordre de DÉPLOIEMENT (l'implémentation, elle, peut être faite dès maintenant)

- **Fenêtres de surveillance strictement sérielles : BLOQUANT.** feature-136 réduit la quantité
  délivrée, bug-115 en rend 408 à 950 mL/jour — ordres de grandeur comparables, sens opposés.
  Déployés ensemble, *« une observation nette nulle ne permet pas de distinguer les deux
  fonctionnent de les deux sont cassés »*.
- **Surveillance feature-136 : 3 jours pleins** (5 si un jour sort le pH de 7,20-7,50), **plus
  deux événements provoqués** : un reboot OTA vérifiant un `usedMs` restauré **non nul** (critère
  anti-no-op), et une coupure > 1 h vérifiant que le budget repart. *Le calendrier seul ne prouve
  rien : ce lot ne se manifeste que sur reboot.*
- **Relever le jerricane D'ABORD, puis déployer.** feature-136 n'invalide pas la campagne — elle
  mesure un **ratio**, et numérateur comme dénominateur se réduisent ensemble — mais freiner le
  débit allonge la consommation des 2 056 mL restants et **dégrade l'incertitude du relevé
  final**. Coût de l'attente : deux jours. Bénéfice : un chiffre non contestable.
- **Exposition tolérable 8-10 jours, sous trois conditions** : aucune session de debug à reboots
  répétés ni campagne OTA pendant la fenêtre · surveillance quotidienne du pH minimum et des
  mL/jour · **si les reboots automatiques dépassent 1/jour, feature-136 repasse devant
  bug-115 immédiatement**.

---

## Notes d'implémentation (doc-writer, 2026-10-08 — v2.58.0)

### Ce qui a été livré
- Module pur [`src/dosing_window_logic.h`](../../src/dosing_window_logic.h)
  (ADR-0017) : structures `DosingWindowRtcRecord` (220 o, T2) /
  `DosingWindowNvsRecord` (56 o, T1), rebasage d'ancre wrap-safe, résolution
  « au plus défavorable », fail-closed T3 (option d), rebasage du ring
  anti-rafale.
- Coquille [`src/dosing_window.{h,cpp}`](../../src/dosing_window.h) : lecture
  `RTC_NOINIT_ATTR` + NVS `pool-dose`, `esp_reset_reason()`,
  `esp_rtc_get_time_us()`, journalisation par voie.
- Câblage `pump_controller.{h,cpp}` : `applyRestoredDosingWindow()` (appelée
  une fois depuis `begin()`), hooks `refreshRtc()` (chaque tour, bon marché) /
  `persistNvsIfDue()` (30 s pendant pompe-ON + événement) dans `update()`.
  `dosingWindow.begin()` appelé depuis `main.cpp` après
  `rtcManager.applyToSystem()`, avant `PumpController.begin()`.
- Tests natifs : `test/test_native_dosing_window/` (18 cas), dont 2 dédiés au
  lien avec l'injection manuelle (C10/S1 ci-dessous).

### Décisions du gate P1 (conception) reprises telles quelles
Voir la section « Gate pool-chemistry P1 » ci-dessus pour le détail complet
des 12 conditions (C1-C12). Résumé des arbitrages les plus engageants, repris
dans [ADR-0089](../../docs/adr/0089-ce-qu-un-redemarrage-a-le-droit-doublier.md) :
fusion au plus défavorable (jamais moyenne ni plus récent), T3 = une seule
fenêtre à la limite (jamais un blocage — motif chimique : RTC morte + hors
réseau bloquerait l'acide indéfiniment, le pH dériverait au-delà de 7,60 et la
boucle ORP compenserait par surdosage de chlore), exclusion motivée de
`lastStartTime`/`lastStopTime`, armement de la pause de mélange au restore
(C5, trou non vu par la spec ni le plan initial).

### Chiffrage NVS
Cadence dédiée 30 s pendant pompe-ON + événements, namespace `pool-dose`
séparé de `pool-daily` (le passager via `saveDailyCounters()` a été écarté :
coupler un cycle de vie horaire à une transaction journalière partagée par
trois autres consommateurs sous `configMutex`). **≈ 62 000 écritures/an ⇒ 271
ans avant 100 000 cycles garantis (48 ans au plafond physique estimé)**.

### Mutants tués (validation par mutation, condition C12)
- **M4 (clé)** — omettre `cyclesDayStart` : reproduit le no-op silencieux
  (`cyclesToday` remis à zéro au premier tick post-reboot,
  `pump_controller.cpp:3240`). Tué par `test_11_restored_cycles_day_start_prevents_silent_rollover`.
- **M8** — frontière de péremption `>=` affaiblie en `>` : tuée par
  `test_16_expiry_threshold_is_inclusive_at_exact_boundary`.
- Soustraction signée au lieu de non signée (repliement `millis()` à 24,8 j,
  défaut déjà rencontré sur ce projet) : tuée par `test_06_underflow_wraparound_is_exact`.
- Confusion des deux péremptions (1 h fenêtre / 24 h jour) : tuée par
  `test_03_long_outage_resets_window_but_conserves_cycles_under_24h`.
- Reset aveugle de la pause de mélange au lieu d'un armement plein sur
  `pumpOnAtWrite` : tuée par `test_10_pump_on_at_write_arms_full_mixing_pause`.
- Pénalité T3 / rollover journalier ré-armés sans fin sur reboots répétés
  (anti-blocage perpétuel, C8) : tuée par
  `test_13_five_reboots_in_twenty_minutes_no_perpetual_penalty`.

### Les 3 conditions du gate `pool-chemistry` P2 (revue d'implémentation) et comment elles ont été soldées
- **S1** — *« C10 (le budget restauré couvre aussi l'injection manuelle,
  ADR-0020) ne reposait que sur une lecture de code, pas sur un test. »*
  Soldée par `test_c10_restored_budget_blocks_manual_injection` (un budget
  restauré à 9/10 min bloque une commande manuelle de 2 min) **et** son
  contraste `test_c10_without_restore_manual_would_pass` (preuve que le test
  précédent mesure bien l'effet de la restauration, pas une garde préexistante).
- **S2** — *« L'écoulé retenu et son statut doivent figurer dans le journal
  de démarrage, sinon un repli conservateur C4 est indiscernable d'une
  reprise nominale, et la dette C7 ne peut jamais être levée par
  l'observation. »* Soldée : `DosingWindowManager::begin()`
  ([`dosing_window.cpp:165-220`](../../src/dosing_window.cpp:165)) construit
  explicitement `coupure=<n>s` ou `écoulé=inconnu` dans la ligne de journal.
- **S3** — *« La magie (`magic`) de chaque instantané était écrite mais
  jamais comparée au restore : un enregistrement scellé par un autre module
  de la RAM RTC partagée (`heap_ring`, `oom_guard`, `time_to_target`,
  `measure_validity`, auto-guérison) resterait auto-cohérent et passerait le
  CRC. »* Soldée : `dosingWindowRtcIntact()` / `dosingWindowNvsIntact()`
  ([`dosing_window_logic.h:133-159`](../../src/dosing_window_logic.h:133))
  comparent désormais `r.magic` à `kDosingWindowRtcMagic`/`kDosingWindowNvsMagic`
  avant le calcul du CRC complet.

### Dette assumée, à lever par l'observation (pas par ce tour)
Condition C7 — le compteur monotone `esp_rtc_get_time_us()` n'est pas prouvé
on-target traverser `esp_restart()` ; seule sa consommation (un bool
`unknownElapsed` déjà tranché par la coquille) est couverte nativement. Mode
de défaillance structurellement conservateur (voir ADR-0089 §Conséquences).
À lever par la lecture du champ « coupure=… » du journal sur plusieurs
redémarrages OTA réels.

### Coordination ADR
0088 était pris (bug-081, livré le même jour). Ce lot prend **0089**. La spec
`bug-115` (en `todo/`) prévoyait de prendre « le prochain numéro libre »
sans en préjuger — elle devra donc prendre **0090** ou au-delà au moment de
son implémentation ; `ls docs/adr/` a été revérifié, pas recopié d'une spec.

### Fenêtres de surveillance (ordre de déploiement — rappel, à charge de
l'agent principal / de l'utilisateur, pas de ce tour)
Le gate impose un déploiement strictement sériel avec bug-115 (sens opposés
sur le volume délivré) : surveiller feature-136 seule 3 jours pleins (5 si un
jour sort le pH de 7,20-7,50), plus deux événements provoqués (reboot OTA
avec `usedMs` restauré non nul, coupure > 1 h qui fait repartir le budget).
Voir la section « Ordre de DÉPLOIEMENT » du gate P1 ci-dessus pour le détail.

### Clôture — corrections de l'agent principal après les deux revues (2026-10-08)

Le `code-reviewer` a rendu **Request changes** sur trois points, tous soldés. Aucun ne portait
sur la sécurité chimique — *« aucun défaut de sécurité chimique trouvé »*.

1. **`magic` écrit mais JAMAIS comparé** (bloquant de la revue, et condition S3 du gate P2).
   `dosingWindowRtcIntact` / `dosingWindowNvsIntact` vérifiaient le schéma, la taille et
   l'empreinte — mais pas la signature, alors que `constants.h` l'annonce comme « premier
   filtre bon marché ».
   **Pourquoi ce n'était pas cosmétique** : le FNV-1a **couvre** la signature. Un enregistrement
   scellé par un **autre module** de la RAM RTC — zone partagée avec `heap_ring`, `oom_guard`,
   `time_to_target`, `measure_validity` et l'auto-guérison — resterait donc **auto-cohérent** et
   passerait le contrôle. La signature est précisément ce qui distingue « notre structure » de
   « une structure valide appartenant à quelqu'un d'autre ». Corrigé dans les deux vérificateurs,
   avec le motif en commentaire.

2. **S2 — l'écoulé retenu et son statut ajoutés au journal de démarrage.**
   Nouveau rendu : `[Dosage] Budget horaire pH restauré (source : …, coupure=142s) : …`
   ou `(…, écoulé=inconnu)`.
   **Motif** : sans cela, un repli conservateur C4 est **indiscernable** d'une reprise nominale
   dans le journal — et la **dette C7** (le compteur RTC traverse-t-il réellement
   `esp_restart()` ?) ne pourrait **jamais** être levée par l'observation terrain. L'écoulé
   rapporté est celui de la **source retenue** ; en mode « plus défavorable », le plus grand des
   deux, qui est celui qui a pu périmer une fenêtre.

3. **S1 — test natif C10 ajouté, construit PAR CONTRASTE.**
   `test_c10_restored_budget_blocks_manual_injection` : un budget restauré à 9 min sur 10 doit
   faire refuser une commande manuelle, avec la cause **`HourlyLimit`** et pas une autre garde.
   `test_c10_without_restore_manual_would_pass` : la **même** commande passait sans restauration.
   Sans ce second cas, le premier aurait pu réussir pour une tout autre raison — c'est la
   différence entre un test qui mesure et un test qui décore.
   C10 ne reposait jusque-là que sur une **lecture de code** (la restauration écrit
   `phDosingState.usedMs`, seul champ lu par le chemin manuel) : vrai, mais une lecture ne
   résiste pas au prochain remaniement.

**Version portée à 2.58.0** : le lot n'en avait pas de propre (2.57.0 = bug-081,
2.57.1 = libellé pH). Nouveau module, nouveau contrat persisté, comportement observable
nouveau — un correctif n'y suffisait pas.

### Validation finale
- `pio run -e esp32dev_core3` : SUCCESS, **zéro warning** (RAM 29,6 %, Flash 50,2 %)
- `pio test -e native` : **2 082 cas / 2 082**, **59 suites** (58 avant + `test_native_dosing_window`)
- Mutation : 9 mutants sur 11 tués nativement ; les 2 restants (filtre de motif de reset, forfait
  pompe-ON) vivent dans du code **non compilable en natif** — dit franchement, pas compté comme acquis
- `security`/`hygiène` : les cinq scripts verts
- Périmètre **bug-115 vérifié intact** par `git diff` : son plan reste applicable **sans rebase**

### ⚠️ Ce qui N'EST PAS prouvé — à faire après déploiement
- **C7** : que `esp_rtc_get_time_us()` traverse réellement `esp_restart()`. Le mode de
  défaillance est **structurellement conservateur** (retour arrière ⇒ repli testé ; redémarrage
  à zéro ⇒ sous-estime l'écoulé ⇒ **retient** le budget plus longtemps), d'où le GO du gate.
  Procédure on-target rédigée dans le rapport de test — à jouer sur les **trois** motifs de reset
  logiciel, pas seulement le redémarrage volontaire.
- **E1** — reboot OTA provoqué **pendant** un dosage : `usedMs` restauré **non nul** et ≥ 80 %
  de la valeur d'avant. Un `usedMs = 0` signerait le **no-op silencieux** ⇒ retour arrière immédiat.
- **E2** — coupure secteur > 1 h : `usedMs = 0`, source NVS, budget reparti.
