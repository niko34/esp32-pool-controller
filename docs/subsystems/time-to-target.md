# time-to-target — indicateur « temps d'atteinte de cible »

- **Fichiers** : [`src/time_to_target_logic.h`](../../src/time_to_target_logic.h) / [`.cpp`](../../src/time_to_target_logic.cpp) (logique **pure**, [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)) — [`src/time_to_target.h`](../../src/time_to_target.h) / [`.cpp`](../../src/time_to_target.cpp) (coquille)
- **Singleton** : `TimeToTargetManager timeToTarget`
- **Version d'introduction** : **v2.37.0** (feature-096)
- **Décision normative** : [ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)
- **Tests natifs** : `test/test_native_time_to_target/`

## Rôle

Chronométrer, chaque jour et pour **chaque régulation** (pH, ORP), le temps mis
pour **atteindre et tenir** la consigne, depuis le premier relevé exploitable de
la journée. C'est la **mesure de succès** du plan de septembre 2026 : c'est elle
qui arbitre les comparaisons de versions du firmware.

> ⚠️ **C'est un instrument de mesure, et le sens est UNIQUE.**
> Ce module lit `pump_controller`, `sensors`, `filtration` et `measure_validity`.
> **Aucune de ses sorties n'est lue par `pump_controller`** : ni gain, ni seuil,
> ni consigne, ni durée n'est adapté sur le KPI. Fermer la boucle exigerait une
> nouvelle spec et un gate `pool-chemistry` ([ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)).
> La garde est **un test de grep bidirectionnel** exécuté à chaque `pio test`
> (`test_H_pump_controller_ignore_totalement_l_indicateur`) : aucune occurrence
> de `time_to_target` ni de `ttt` dans `src/pump_controller.{h,cpp}`. Un `#error`
> de compilation avait été envisagé puis écarté — il aurait cassé le build de
> `time_to_target.cpp` lui-même, qui doit inclure `pump_controller.h` pour lire
> la grandeur de décision.

## Les cinq définitions qui font la métrique

### 1. On juge sur la **grandeur de décision**, pas sur la mesure filtrée

Un juge qui n'observe pas la grandeur sur laquelle le régulateur agit ne mesure
pas le même objet. La règle est **uniforme**, sa réalisation est **asymétrique**
parce que les deux régulateurs le sont :

| Voie | Grandeur observée | Accesseur |
|---|---|---|
| pH | **signal lent** de la porte de session (moyenne glissante `kPhDecisionWindowMs` = **10 min**, feature-094) | `PumpController.getPhDecisionValue()` |
| ORP | mesure **filtrée** — qui **EST** sa grandeur de décision | `sensors.getOrpFiltered()` |

> ⚠️ **Conséquence à connaître : le zéro du chronomètre pH est indexé sur
> `kPhDecisionWindowMs`.** Le signal lent n'existe pas avant que sa moyenne se
> soit constituée : t0 pH démarre donc **~10 min après** l'ouverture de la porte
> de mesure. **Toute modification future de cette constante crée une
> discontinuité de série** entre les journées archivées. Un test le démontre sur
> la journée du 04/09/2026 : la même courbe donne **233 min** si t0 part du brut
> et **223 min** sur la grandeur de décision — les 10 min d'écart sont
> exactement la fenêtre.
>
> Le firmware rend cette discontinuité **lisible plutôt que silencieuse** :
> l'anneau NVS porte une estampille `serieStamp` (la valeur de
> `kPhDecisionWindowMs` à l'écriture) et `begin()` émet un warning
> `[KPI] Discontinuité de série : fenêtre de décision X → Y ms` si elle a changé.

### 2. « Atteint » signifie « la régulation ne redémarrerait pas »

La bande d'atteinte est celle des **seuils de démarrage vivants** de la
régulation, relus à chaque tick : `pumpProtection.phStartThreshold` (**0,05 pH**)
et `pumpProtection.orpStartThreshold` (**15 mV**). Définition **opérationnelle**,
pas arbitraire : tant que l'écart est sous ce seuil, la régulation n'ouvrirait
pas de session.

La cible est déclarée atteinte quand la bande est **tenue
`kTimeToTargetHoldMin` = 30 minutes de circulation consécutives**. Le temps
rendu est celui du **début** de la tenue, pas de sa fin : la cible a été
atteinte à ce moment-là, les 30 minutes suivantes n'ont fait que le prouver.
Une **sortie de bande remet la tenue à zéro, jamais le chronomètre** (sans quoi
le temps d'atteinte repartirait à chaque oscillation autour de la consigne).

### 3. La base de temps est en **minutes de circulation**

Le compteur `SafetyLimits::filtTotalMin` (feature-093, namespace NVS
`pool-daily`, clé `filt_total`) est **réutilisé tel quel**, en **lecture seule**.

Il faut le nommer honnêtement : ce sont des minutes de **présence d'eau**, pas
de « filtration en marche ». **Libellé partout, UI comprise : « minutes de
circulation ». Jamais « de filtration ».** C'est l'assiette physiquement juste —
sans circulation, ni mélange ni mesure représentative : la régulation ne
progresse pas, une nuit à l'arrêt ne doit pas faire vieillir une journée de
régulation.

### 4. Le boost chlore ne lève **pas** le drapeau « consigne modifiée »

On **juge** contre la consigne **effective** (`PumpController.getEffectiveOrpTarget()`) :
juger contre 530 mV pendant que le régulateur vise 630 déclarerait une atteinte
fictive. On **surveille** en revanche la consigne **configurée**
(`mqttCfg.orpTarget`) : un boost n'est pas une modification utilisateur. Il a
son **propre** bit (bit 1).

### 5. Bit 4 — la règle de partage, cœur de l'honnêteté de la métrique

> **Ce que le régulateur CHOISIT compte ; ce qu'on lui IMPOSE de l'extérieur
> porte le bit.**

| Situation | Drapeau | Lecture |
|---|---|---|
| Quota journalier atteint | **aucun bit** | vraie non-atteinte — **c'est ce que la métrique doit voir** |
| Limite horaire d'injection | **aucun bit** | idem |
| Actionneur sous-dimensionné ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) | **aucun bit** | idem |
| Mode de régulation `manual` | **bit 4** | débranchée par l'opérateur |
| Mode maintenance (`isMaintenanceInhibited()`) | **bit 4** | idem |
| Assistant de mesure de débit (`isFlowAssistSuspended()`) | **bit 4** | idem |

**Enjeu réel** : les journées marquées bit 4 doivent être **exclues des moyennes
de comparaison de versions** — une semaine de maintenance ne doit pas ressembler
à une régression de la régulation. Ce n'est pas une affaire de couleur de
pastille.

## Machine d'états

Fonction pure unique `tttTick(TttLive&, const TttInputs&)`, un événement au plus
par tick.

| `state` | Sens | Ce que porte `minutes` |
|---|---|---|
| `0` `kTttStateIdle` | pas encore de départ aujourd'hui | `0` |
| `1` `kTttStateRunning` | chronomètre en cours | minutes **écoulées** depuis t0 |
| `2` `kTttStateReached` | cible atteinte et tenue | **temps d'atteinte** |
| `3` `kTttStateMissed` | journée close sans tenue | **durée totale** de circulation |

Valeurs **4-255 réservées** : une pastille archivée par un firmware futur doit
rester lisible par un firmware plus ancien.

**Enchaînement d'un tick**, dans cet ordre exact :

1. **Heure inexploitable** (`date == 0`) ⇒ suspension, état 0, rien n'est daté.
2. **Bascule de jour civil** ⇒ la journée en cours se clôt en « non atteinte »
   (cas de la filtration manuelle qui tourne la nuit). Le chronomètre ne
   traverse **jamais** minuit.
3. **Drapeaux vivants** (consigne configurée modifiée, boost, purge au plafond),
   cumulatifs, posés seulement tant que le chronomètre tourne.
4. **Clôture** : fin de journée de circulation (`dayEnd`) ⇒ « non atteinte »
   nue ; régulation débranchée par l'opérateur ⇒ « interrompue », bit 4.
5. **Départ** (état 0) : premier instant où la grandeur de décision **existe**
   et est **plausible**, circulation en marche. Un seul départ par jour civil.
6. **En cours** (état 1) : tenue dans la bande, puis atteinte.

**Une mesure non finie vaut SORTIE DE BANDE** — jamais 0, jamais « dans la
bande ». Elle empêche le départ et remet la tenue à zéro, sans jamais arrêter le
chronomètre.

**Cas « déjà à la cible »** : la tenue démarre au tick suivant t0, avec
`holdStartFiltMin == t0FiltMin` ⇒ temps final **0 min**.

**Journée qui commence en manuel** : t0 est quand même posé, le tick suivant la
clôt aussitôt avec le bit 4 et 0 minute. L'historique reste **dense** — un trou
muet se lirait comme un bug de l'indicateur. Un retour en automatique le même
jour ne repose **pas** de t0 (l'invariant « un seul départ par jour » prime).

### Garde de vraisemblance

L'indicateur **refuse de poser t0** si l'écart initial dépasse
`kTttMaxPlausibleE0Ph` = **1,0 pH** ou `kTttMaxPlausibleE0Orp` = **300 mV**.

Elle **refuse seulement de démarrer** : elle n'invente jamais de valeur, et t0
est posé au **premier échantillon plausible**. Un **seul** avertissement par
voie et par journée (réarmé au changement de jour civil) — la garde se réévalue
chaque seconde, la journaliser à chaque fois noierait le journal.

**Cas réel motivant** : le 05/09/2026 au matin, de l'air dans le circuit (niveau
d'eau bas) a fait plonger le pH brut à **5,45**, soit un écart de **−1,75** —
une mesure d'installation, pas une mesure de bassin. Sans la garde, elle aurait
pollué la mémoire de sept jours.

## Persistance à deux étages

### NVS `pool-kpi` — namespace **disjoint** de `pool-daily`

Clé `ttt7`, blob `TttHistory` de **188 octets** (schéma 1) : anneau de 7 jours ×
2 voies, `serieStamp`, `t0FiltMin[2]`, sceau **FNV-1a**.

L'argument du namespace séparé n'est **ni l'espace ni la corruption de page**
(les namespaces partagent les mêmes pages physiques) : c'est le **droit de
destruction en masse**. Le namespace est l'unité de `clear()`. Un module
d'instrumentation ne doit pas détenir de handle en **écriture** sur le namespace
des compteurs de dosage. **Moindre privilège : un non-garde-fou ne détient pas
la clé d'écriture des garde-fous.** `filt_total` y est lu en **lecture seule**.

### Exactement **2 écritures par jour et par régulation**

| Instant | Ce qui est écrit | Pourquoi |
|---|---|---|
| **Pose de t0** | `t0FiltMin`, `e0`, la date, les drapeaux, entrée « en cours, 0 min » | `t0FiltMin` est l'**unique donnée irrécupérable** |
| **Clôture** (atteinte ou non-atteinte) | l'entrée définitive, `t0FiltMin` remis à 0 | seconde et dernière écriture |

Tout le reste se **recalcule** : `minutes = filtTotalMin − t0FiltMin`, et
`filtTotalMin` survit déjà en NVS. Une date déjà présente est **remplacée** dans
l'anneau : la clôture réécrit l'entrée posée à t0 sans doubler la journée.

### RAM RTC — le volatil seulement

`TttRtcRecord` (magic `"KPIT"` = `0x4B504954`, sceau FNV-1a, 64 o,
`RTC_NOINIT_ATTR`) porte la progression de tenue et l'état courant. Il survit à
un **reset logiciel / panic / watchdog**, jamais à une **coupure secteur** (RAM
RTC aléatoire ⇒ magic/CRC faux ⇒ reprise par la NVS, fail-closed).

`TttLive` et `TttRtcRecord` n'ont **aucun initialiseur de membre par défaut**,
volontairement : un initialiseur rendrait la structure non triviale, donc
dynamiquement initialisée au démarrage — l'état vivant serait effacé à chaque
reset, exactement ce que la RAM RTC doit éviter.

### Reprise après coupure secteur, le même jour

t0 **conservé** (relu dans l'anneau NVS) ⇒ minutes **exactes** ; tenue **remise
à zéro** (fail-closed : l'eau s'est arrêtée, la purge se rejoue, une tenue ne
traverse pas une coupure) ; **bit 3** `kTttFlagPowerCycle` posé ; log
`[KPI] pH : reprise après coupure`. Jour différent ou heure invalide : état 0,
et **aucune écriture datée faux**. La consigne surveillée est
**rephotographiée** — une modification survenue pendant la coupure n'est pas
détectable, et inventer un drapeau serait pire que de l'omettre.

### Rattrapage des journées orphelines

`tttHistoryCloseOrphans(TttHistory&, uint32_t today)`, appelée par `begin()`
après les deux branches de reprise.

Une entrée est archivée dès la pose de t0, à l'état « en cours, 0 min » — c'est
ce qui rend t0 récupérable. Mais si l'appareil reste **éteint plus d'une journée
civile**, la reprise ne cherche que la date du jour : l'entrée de la veille
resterait figée « en cours, 0 min » jusqu'à sortir de l'anneau au bout de
7 jours. L'interface afficherait « 0h00 » et annoncerait « journée en cours »
pour une journée révolue — un faux positif sur la métrique qui arbitre les
comparaisons de versions.

Ces journées sont donc closes en **non-atteinte**, drapeau de coupure posé.
Leurs minutes sont irrécupérables : **laisser 0 est la seule valeur honnête** —
mieux vaut une journée manquante qu'une journée fausse.

**Trois gardes, chacune couverte par mutation** :

- `today == 0` (heure invalide) ⇒ **on ne touche à rien**. Sans savoir quel jour
  on est, rien n'est « révolu » : sinon un démarrage sans NTP détruirait sept
  journées d'observation.
- `d.date != today` ⇒ la journée **du jour** est épargnée.
- `d.state == kTttStateRunning` ⇒ une journée **atteinte** n'est **jamais**
  rouverte en non-atteinte (ce serait fabriquer une régression apparente).

N'écrit pas en NVS : la resynchronisation se fera à la prochaine archive.

## Champ de bits `flags` — encodage **figé dès le premier déploiement**

`state` et `flags` sont archivés en NVS **et** servis à l'UI : leurs valeurs sont
un **contrat**. Champ de bits et non énumération — une journée peut cumuler
plusieurs circonstances, et les bits 5-7 restent disponibles sans casser les
pastilles déjà archivées. Un client **teste des bits, jamais l'égalité**.

| Bit | Constante | Sens |
|---|---|---|
| `0x01` | `kTttFlagTargetChanged` | consigne **configurée** modifiée dans la journée |
| `0x02` | `kTttFlagBoost` | boost ORP actif dans la journée |
| `0x04` | `kTttFlagUnsettled` | purge de mesure terminée au **plafond** (feature-091) |
| `0x08` | `kTttFlagPowerCycle` | reprise après coupure secteur |
| `0x10` | `kTttFlagOperatorOff` | **régulation non automatique** — journée à exclure des moyennes |

## API publique

```cpp
void       begin();                            // loopTask uniquement
void       tick();                             // loopTask uniquement, cadencé 1 s en interne
TttSnapshot getSnapshot() const;               // lisible depuis un handler async
void       copyHistory(TttHistory& out) const; // lisible depuis un handler async
bool       isMemoryUnavailable() const;        // lisible depuis un handler async
```

- **`begin()`** doit être appelé **après** `loadDailyCounters()` (le compteur de
  circulation doit être restauré) **et après** `rtcManager.applyToSystem()`
  (l'heure doit être connue). Appelé depuis [`main.cpp:272`](../../src/main.cpp).
- **`tick()`** est appelé depuis la `loop()` ([`main.cpp:348`](../../src/main.cpp)),
  **avant** `PumpController.update()` : l'indicateur ne doit pas s'intercaler
  entre `filtration.update()` et `PumpController.update()`, qui doivent voir le
  même tour. Il lit donc la décision du **tour précédent** — **retard assumé
  d'une seconde**, sans effet sur une métrique qui compte des minutes.
- Les trois accesseurs servent une **copie mémorisée** publiée sous spinlock
  `portMUX` (patron `measure_validity`) : `/data`, le WebSocket et `/get-kpi`
  servent cet instantané, **jamais** l'accesseur de `pump_controller`.
  `isMemoryUnavailable()` lit lui aussi le cache, pas le champ nu.

## Exposition

| Canal | Contenu |
|---|---|
| `GET /data` | 8 champs `ph_ttt_*` / `orp_ttt_*` — [API.md](../API.md#get-data--write) |
| WebSocket `sensor_data` | **les mêmes 8 champs**, même passage, même ordre — [ws-manager.md](ws-manager.md#champs-sensor_data-ajoutés-en-feature-096--temps-datteinte-de-cible-v2370) |
| `GET /get-kpi` | mémoire 7 jours des deux voies — [API.md](../API.md#get-get-kpi--write) |
| MQTT | **aucun topic** — la feature les exclut explicitement |
| UI | [page-ph.md](../features/page-ph.md#atteinte-de-la-cible-feature-096-v2370) et [page-orp.md](../features/page-orp.md#atteinte-de-la-cible-feature-096-v2370). **Le tableau de bord n'est pas touché.** |

⚠️ **`/get-kpi` sert deux voies dans une entrée, `/data` et le WS n'en servent
qu'une chacune sous un préfixe plat.** Une entrée de `/get-kpi` porte `d` au
premier niveau et **une sous-structure par régulation** :

```json
{"d":20260906,"ph":{"s":2,"m":356,"e0":-0.02,"f":0},"orp":{"s":2,"m":120,"e0":-45.0,"f":0}}
```

Les valeurs `s` / `m` / `e0` / `f` sont **à l'intérieur** de `ph` / `orp`. Le
rendu de la bande, lui, travaille sur des entrées **plates** : l'interface
aplatit donc la charge pour la voie affichée (`tttDaysForChannel()` dans
[`data/app.js`](../../data/app.js)) avant d'appeler `renderTttDays()`. Lire
`entrée.s` directement rend `undefined` pour **les sept journées** : c'est
exactement `bug-111` (v2.45.1), où seule la case du jour s'affichait — elle est
**écrasée par l'état vivant de `/data`** juste avant l'affichage, et n'a donc
jamais dépendu de cet aplatissement.

Deux invariants de l'aplatissement, tous deux porteurs d'un message utilisateur :

- **`null` reste `null`.** L'historique vaut `null` après un échec de `/get-kpi`
  — « on ne sait pas », d'où la légende « Historique indisponible ». Le
  convertir en tableau vide afficherait une bande vide sous « 7 derniers
  jours », qui se lit « aucune journée enregistrée » : un mensonge à la place
  d'un aveu d'ignorance.
- **Une voie absente reste un trou à sa place** (`·`), jamais une pastille
  « en cours ». La bande étant alignée **par date**, retirer une entrée ne
  décale aucune autre.

## Journal

Un **gabarit unique** paramétré par la phase (trois messages distincts pesaient
~0,6 Ko de chaînes sur une flash occupée à 98,7 %) :

```
[KPI] <voie> : <phase> — <N> min de filtration, écart initial <e0>, écart courant <résiduel>
```

| Phase | Niveau | Quand |
|---|---|---|
| `départ` | `info` | t0 posé |
| `cible atteinte` | `info` | tenue de 30 min accomplie |
| `cible NON atteinte` | `info` | journée close sans tenue |
| `interrompue (régulation non automatique)` | `info` | clôture avec le bit 4 |
| `départ refusé (écart invraisemblable)` | `warning` | garde de vraisemblance, **une fois** par voie et par jour |
| `reprise après coupure` | `info` | t0 relu dans la NVS après coupure secteur |

Deux avertissements hors gabarit, émis **une seule fois** chacun :

- `[KPI] Mémoire NVS indisponible — indicateur suspendu` — l'écriture `pool-kpi`
  a échoué : indicateur suspendu (état 0), **aucun retry**, **aucun repli**
  d'écriture vers `pool-daily`. Ni `critical` ni alerte MQTT : ce serait crier
  au loup pour un instrument.
- `[KPI] Heure non synchronisée — indicateur suspendu (aucune journée n'est datable)`.
- `[KPI] Discontinuité de série : fenêtre de décision X → Y ms` (au `begin()`,
  si `serieStamp` a changé).

## Constantes

| Constante | Valeur | Rôle |
|---|---|---|
| `kTimeToTargetHoldMin` | **30** min de circulation | tenue exigée dans la bande — 3× `kPhDecisionWindowMs` (`static_assert` : ≥ 2×) |
| `kTimeToTargetHistoryDays` | **7** | profondeur de la mémoire glissante |
| `kTimeToTargetOffDayEndMin` | **60** min **murales** | arrêt de circulation valant fin de journée (couvre fin de plage horaire **et** passage OFF) |
| `kTttMaxPlausibleE0Ph` | **1,0** pH | garde de vraisemblance |
| `kTttMaxPlausibleE0Orp` | **300** mV | garde de vraisemblance |
| `kTttHistorySchema` | **1** | schéma de l'anneau — un enregistrement d'un autre schéma est **rejeté**, jamais relu |
| `kTttRtcMagic` | `0x4B504954` (`"KPIT"`) | magic **propre** à cet instrument (ne réutilise ni `kOutageRtcMagic` ni une structure de feature-093) |

Toutes dans [`src/constants.h`](../../src/constants.h) sauf les deux dernières,
qui vivent dans [`time_to_target_logic.h`](../../src/time_to_target_logic.h) :
ce ne sont pas des réglages mais la **définition d'un format** (patron
`PhFfStatus` / `ph_feedforward_logic.h`).

## Cas limites

- **Heure invalide au démarrage** (pas de DS3231, pas de NTP) : état 0, warning
  unique, aucune mémoire écrite avec une date fausse.
- **Filtration manuelle ON toute la nuit** : le jour civil change à minuit ⇒ la
  journée se clôt en « non atteinte » et un nouveau t0 est posé après minuit.
- **Sonde HS / mesure NaN prolongée après t0** : le chronomètre continue, la
  tenue repart de zéro ; si la journée finit ainsi ⇒ « non atteinte » avec écart
  résiduel `n/a` (jamais « 0,00 »).
- **Purge terminée au plafond** (feature-091) : t0 est **quand même** posé (la
  porte est ouverte), bit 2 mémorisé pour lecture honnête.
- **Changement de consigne** : bit 0 posé, chronomètre **poursuivi** sur la
  nouvelle consigne.
- **Compteur de circulation en recul** (cas théorique) : la tenue est relancée,
  jamais un écart négatif.
- **Anneau NVS absent ou corrompu** : rejet net, anneau vide, aucune journée
  servie — jamais de réinterprétation.
- **`minutes` borné à 1440** à l'insertion : un `uint16` ne porte pas plus
  qu'une journée, et une journée n'a pas plus de 1440 minutes.

## Dépendances

**Lues** : `pump_controller` (grandeur de décision pH, consigne ORP effective,
inhibitions maintenance / assistant de débit), `sensors` (ORP filtré),
`filtration` (`isRunning()`), `measure_validity` (porte de mesure, motif de fin
de purge), `config` (`safetyLimits.filtTotalMin`, `mqttCfg`, `pumpProtection`),
`rtc_manager` (jour civil), `logger`.

**Écrites** : aucune. Voir l'encadré « sens unique » en tête.

## Fichiers liés

- [`src/time_to_target_logic.h`](../../src/time_to_target_logic.h) / [`.cpp`](../../src/time_to_target_logic.cpp) — décisions pures
- [`src/time_to_target.h`](../../src/time_to_target.h) / [`.cpp`](../../src/time_to_target.cpp) — câblage, RTC, NVS, journal
- [`src/config.cpp`](../../src/config.cpp) — `loadTttHistory()` / `saveTttHistory()` (transport NVS `pool-kpi`, rien d'autre)
- [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — `/data` (8 champs) et `GET /get-kpi`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — trame `sensor_data`
- [`data/app.js`](../../data/app.js) — `updateTimeToTargetDisplay()`, `renderTttChannel()`, `tttDaysForChannel()` (aplatissement par voie), `renderTttDays()`

## Documentation liée

- [ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md) — définition normative de la métrique
- [ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) — le signal lent de décision pH, dont t0 pH dépend
- [regulation-vue-d-ensemble.md](regulation-vue-d-ensemble.md) — la chaîne mesure → décision → débit
- [measure-validity.md](measure-validity.md) — la porte de mesure qui autorise t0
