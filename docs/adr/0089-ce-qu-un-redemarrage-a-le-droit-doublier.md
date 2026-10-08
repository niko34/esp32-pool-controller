# ADR-0089 — Ce qu'un redémarrage a le droit d'oublier

- **Statut** : Accepté
- **Date** : 2026-10-08
- **Décideurs** : pool-chemistry (GO sous 12 conditions, gate de conception), architect
- **Spec(s) liée(s)** : feature-136 (budget-horaire-de-dosage-survit-au-redemarrage), bug-134 (spec qui a révélé le défaut)

## Contexte

`DosingState` (`usedMs`, `windowStart`, `cyclesToday`), la pause de mélange
(`_mixingEndMs[2]`) et le ring anti-rafale (`_dosingCycleHistory[2][20]`)
étaient entièrement en RAM. Tout redémarrage — OTA, auto-guérison mémoire,
watchdog, panic — remettait le budget horaire de dosage à zéro. Relevé sur
l'appareil le 2026-10-08 : à `ph_limit_minutes = 10`, **deux redémarrages dans
l'heure autorisaient jusqu'à 20 minutes d'acide**, seul le plafond journalier
(2 000 mL) faisant encore barrage. C'est la seule garde du projet qui
**autorisait à tort** plutôt que de refuser à tort — ce qui la place dans la
direction dangereuse, contrairement aux defaults fail-closed habituels du
projet.

Trois questions structurantes se posent, qui ne sont pas spécifiques à cette
seule feature :

1. **Quel support de persistance pour une garde temporelle de sécurité ?** Le
   projet a déjà une RAM RTC (`RTC_NOINIT_ATTR`, utilisée par le disjoncteur de
   boot-loop, `measure_validity`, `time_to_target`) et une NVS ancrée sur
   l'horloge murale. Les deux ont des garanties différentes et aucune des deux
   ne couvre, seule, tous les cas de redémarrage.
2. **Que faire quand ni l'une ni l'autre n'est exploitable ?** Bloquer le
   dosage jusqu'à ce qu'une horloge soit valide semble le choix sûr, mais cette
   garde protège une seule chimie (le chlore, via l'inhibition pH) au prix d'un
   risque sur l'autre (le pH, en dérivant sans contre-mesure).
3. **Quels champs persister, et à quelle cadence ?** Un format persisté est un
   contrat qui survit aux mises à jour — y inclure un champ mort l'alourdit
   pour rien ; en omettre un rend le correctif partiel, parfois de façon
   invisible.

## Décision

### 1. Trois étages, le troisième seulement en dernier recours

- **T2 (RAM RTC, `RTC_NOINIT_ATTR`)** — survit à un reset logiciel / panic /
  watchdog, pas à une coupure secteur. L'écoulé se mesure par un compteur
  **monotone** (`esp_rtc_get_time_us()`), indépendant de l'horloge murale :
  c'est ce qui permet de couvrir le cas le plus fréquent (OTA hebdomadaire)
  sans dépendre d'une resynchronisation NTP qui n'a peut-être pas eu lieu.
- **T1 (NVS, namespace dédié `pool-dose`)** — ancrée sur l'epoch murale, survit
  à tout y compris une coupure secteur, mais exige une heure valide pour être
  interprétée.
- **T3 (fail-closed, option « une seule fenêtre »)** — seulement si ni T1 ni
  T2 n'est exploitable : `usedMs` posé à la limite pour **une seule fenêtre
  horaire**, puis retour au nominal.

Quand T1 et T2 sont tous deux disponibles et se contredisent, le verdict
retient **le plus défavorable, grandeur par grandeur** (`usedMs` max,
`cyclesToday` max, reliquat de pause mélange max) — jamais la moyenne, jamais
la source la plus récente. Mélanger l'ancre d'une source avec la valeur de
l'autre produirait une paire incohérente : l'ancre suit systématiquement la
source qui a fourni la grandeur retenue.

### 2. T3 est une fenêtre unique posée à la limite, jamais un blocage

Un blocage inconditionnel jusqu'à horloge valide a été **examiné et refusé**.
Le motif est chimique : RTC morte + sans réseau = blocage indéfini de l'acide.
Sous charge basique permanente et régulation P pur (droop structurel), le pH
sort de la plage 7,20-7,50 et finit par franchir le seuil d'inhibition
(7,60) ; la fraction HOCl active tombe de ~52 % à pH 7,50 à ~41 % à pH 7,70,
et la boucle ORP compense alors par **surdosage de chlore**. Une protection
qui crée un surdosage sur l'autre chimie n'est pas une protection. L'option
retenue (une seule fenêtre à la limite) est préférée à un blocage muet parce
que l'état « fenêtre horaire épuisée » est déjà exposé partout (carte
Régulation, MQTT/WS, libellé de refus) : aucun chemin de refus silencieux
n'est introduit par ce choix.

### 3. Ce qui est persisté, et ce qui ne l'est pas

Persisté : `usedMs`, `windowStart`, `cyclesToday`, **`cyclesDayStart`**,
**`_mixingEndMs[2]`** (pause de mélange) et le **ring anti-rafale**
(`_dosingCycleHistory[2][20]`, T2 uniquement). `cyclesDayStart` n'est pas une
grandeur accessoire : son absence ferait du correctif un **no-op silencieux**
— sans elle, le code existant de remise à zéro journalière (`cyclesDayStart
== 0` au premier tick) effacerait `cyclesToday` à chaque redémarrage, exactement
comme avant, en laissant croire la garde rétablie.

**Explicitement exclus, avec motif** : `lastStopTime` (zéro lecture dans tout
le firmware — rien ne le consulte) et `lastStartTime` (lu uniquement pour
calculer une durée journalisée, jamais pour une décision de dosage). Un format
persisté est un contrat qui survit aux mises à jour : on n'y inscrit pas un
champ mort par symétrie avec ceux qui comptent.

La pause de mélange est en outre **armée au restore** si l'instantané retenu
porte « pompe-ON à l'écriture » : le produit a été injecté mais la fonction
qui arme normalement la pause (`notifyPhDose()` / `notifyOrpDose()`) n'a
jamais pu s'exécuter, le reboot ayant interrompu le cycle avant l'arrêt
normal de l'injection. Sans cet armement, l'appareil redoserait sur une
mesure non représentative du produit déjà présent dans l'eau.

### 4. Cadence d'écriture NVS : namespace dédié, pas un passager

T1 écrit dans son propre namespace (`pool-dose`), à 30 s pendant qu'une pompe
de dosage est alimentée plus un événement à chaque transition pompe-ON ↔
pompe-OFF — **≈ 62 000 écritures/an**, soit 271 ans avant les 100 000 cycles
d'endurance garantis par le wear-levelling NVS (48 ans au plafond physique
estimé). Le flush existant des compteurs journaliers (`pool-daily`, différé
60 s, partagé par trois autres consommateurs sous le mutex de configuration)
a été écarté à dessein comme vecteur de persistance : il aurait couplé un
cycle de vie **horaire** à une transaction **journalière** sans lien
fonctionnel entre les deux, exposant chacun des deux à l'évolution future de
l'autre.

## Alternatives considérées

- **Option A (rejetée) — bloquer le dosage jusqu'à horloge valide en absence
  des deux sources.** Voir §2 : surdosage de chlore induit par compensation
  ORP, documenté avec les fractions HOCl concernées.
- **Option B (rejetée) — persister un budget réduit (plutôt qu'à la limite)
  en absence des deux sources.** Sémantique neuve et surtout dangereuse à
  rebours : elle autoriserait un dosage sur un budget antérieur **inconnu** —
  l'exposition même que ce lot ferme.
- **Option C (rejetée) — retenir la source la plus récente plutôt que la plus
  défavorable en cas de double disponibilité contradictoire.** Plus simple,
  mais réintroduit exactement le risque corrigé : une source légèrement plus
  optimiste l'emporterait sans justification chimique.
- **Option D (rejetée) — faire porter la persistance du budget horaire par le
  flush différé des compteurs journaliers existant (`pool-daily`).** Coût de
  développement nul, mais couple deux cycles de vie sans rapport (voir §4).
- **Option E (retenue) — trois étages (T2 RAM RTC / T1 NVS dédiée / T3
  fail-closed à la limite d'une fenêtre), fusion au plus défavorable, exclusion
  motivée de `lastStartTime`/`lastStopTime`, armement de la pause de mélange au
  restore.**

## Conséquences

### Positives
- Le cas le plus fréquent (reboot OTA) ne dépend plus d'une resynchronisation
  d'horloge pour restaurer le budget — T2 suffit seul.
- Aucun chemin de dosage ne peut plus profiter d'un redémarrage pour dépasser
  silencieusement le budget horaire configuré.
- La dette assumée (ci-dessous) est observable : la ligne de journal au
  redémarrage rapporte la source retenue et l'écoulé, ce qui permet de la
  lever par l'observation sans instrumentation supplémentaire.

### Négatives / dette assumée
- **Comportement visible changé, volontairement** : après un redémarrage, un
  dosage qui aurait été autorisé avant ce lot peut désormais être refusé. Ce
  n'est pas une régression — c'est l'objet même du correctif — mais cela doit
  être énoncé clairement à chaque mise à jour, pas supposé évident.
- Le compteur monotone de T2 (`esp_rtc_get_time_us()`) n'a pas été prouvé, sur
  matériel réel, traverser `esp_restart()` — seule sa consommation (un indicateur
  d'écoulé inconnu déjà tranché par la coquille) est couverte par les tests
  natifs. Son mode de défaillance est structurellement conservateur (un retour
  en arrière du compteur bascule en écoulé inconnu, un retour à zéro sous-estime
  l'écoulé et ne fait que retenir le budget plus longtemps) — jugé acceptable à
  ce titre, mais à retirer par l'observation du terrain.
- Au pire cas (démarrage à froid sans aucune des deux sources), jusqu'à une
  heure de capacité de dosage est perdue — un coût assumé, borné et rattrapable
  à la fenêtre suivante.

### Ce que ça verrouille
- Toute future garde temporelle de sécurité doit choisir explicitement entre
  RAM RTC et NVS (ou les deux, fusionnées au plus défavorable) — la question
  ne peut plus être éludée par « ça finira bien par se réarmer tout seul ».
- Le format persisté `DosingWindowRtcRecord` / `DosingWindowNvsRecord` est un
  contrat : tout changement de taille exige d'incrémenter son numéro de schéma
  (`kDosingWindowRtcSchema` / `kDosingWindowNvsSchema`), jamais une
  réinterprétation silencieuse d'un ancien format.
- Le principe « au plus défavorable, jamais la moyenne ni le plus récent »
  devient le patron de référence pour toute future fusion de deux sources de
  vérité redondantes sur une grandeur de sécurité.

## Références

- Code : [`src/dosing_window_logic.h`](../../src/dosing_window_logic.h) (module
  pur), [`src/dosing_window.h`](../../src/dosing_window.h) /
  [`.cpp`](../../src/dosing_window.cpp) (coquille),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
  (`applyRestoredDosingWindow()`, hooks `refreshRtc()`/`persistNvsIfDue()` dans
  `update()`), [`src/constants.h`](../../src/constants.h) (section feature-136)
- Spec : `specs/features/done/feature-136-budget-horaire-de-dosage-survit-au-redemarrage.md`
- Tests : [`test/test_native_dosing_window/`](../../test/test_native_dosing_window/)
- Doc : [subsystems/pump-controller.md](../subsystems/pump-controller.md#survie-du-budget-horaire-de-dosage-au-redémarrage-feature-136-v2580)
- ADR liés : [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
  (logique pure testable, patron repris ici), [ADR-0020](0020-budget-horaire-dosage-unique.md)
  (budget horaire unique auto + manuel — ce lot en couvre la survie au reboot,
  n'en change pas la portée), [ADR-0053](0053-persistance-conditionnelle-integrale-ph.md)
  (autre cas de persistance conditionnelle d'une grandeur de régulation, patron
  voisin)
