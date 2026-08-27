# ADR-0049 — La cause de refus de dosage est transportée par un code machine, et discriminée sur l'état interne du filtre

- **Statut** : Accepté
- **Date** : 2026-08-27
- **Décideurs** : `architect`, `pool-chemistry` (gate : refus du discriminant fondé sur la fraîcheur de la lecture brute), `code-reviewer` (grâce de ré-amorçage, épisode d'alerte non redaté), utilisateur (report du défaut observé au démarrage)
- **Spec(s) liée(s)** : `fix-cause-de-refus-de-dosage-devinee-par-texte` (v2.25.1) ; prolonge [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure) et [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) (porte de validité des mesures) ; s'appuie sur l'exigence de parité `/data` ↔ WebSocket consignée par [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)

## Contexte

Le firmware expose la dernière cause de refus de dosage sous forme d'une **phrase française** (`phDoseBlockedReason` / `orpDoseBlockedReason`), destinée au journal et à l'affichage. L'interface web, elle, avait besoin d'une **catégorie** : pour choisir un libellé court, une couleur, et décider si le message devait être affiché ou masqué au profit d'un badge concurrent. Faute de champ dédié, elle la **devinait**, en cherchant des fragments de texte dans la phrase (`_doseBlockReasonFr`, `String.includes()`).

Deux défauts ont été constatés en production, symétriques, tous deux issus de ce seul procédé :

1. **Une panne annoncée sur une sonde saine.** Au démarrage, la porte de validité des mesures reste fermée pendant `stabilizationDelayMin` (5 min par défaut), puis le filtre exige 13 échantillons à 5 s d'intervalle. Pendant environ six minutes, la cause servie était « lecture pH/ORP filtrée indisponible (NaN) ». Le fragment « indispo » correspondait à la règle de l'UI, qui affichait **« EZO indisponible »** — une panne matérielle annoncée alors que la sonde allait parfaitement bien, à chaque démarrage.
2. **Une panne présentée comme un état normal.** La cause `FilterNotReady` était libellée « filtre capteur non prêt (warmup / EZO injoignable) » : **une seule phrase pour deux causes physiques opposées**. L'UI testait « warm » avant « ezo », donc une **vraie panne du module EZO** s'affichait « Stabilisation du filtre », côté pH comme côté ORP. C'est le défaut grave : un état d'attente rassurant recouvrait un arrêt de régulation.

Le second défaut est le plus instructif. Il ne vient pas d'une erreur de codage côté client mais du fait que la **phrase elle-même mélangeait deux causes** : aucun analyseur de texte, même correct, n'aurait pu les distinguer. Le problème est donc à la fois un problème de **transport** (deviner une catégorie depuis une chaîne destinée à l'humain) et un problème de **source** (le firmware ne savait pas lui-même laquelle des quatre raisons de `SensorFilter::ready() == false` s'appliquait).

`ready()` est en effet faux pour **quatre** raisons physiquement distinctes : fenêtre de lissage incomplète (`warmingUp()`), plus aucune mesure acceptée ou trop ancienne (`stale()`), capteur figé (`frozen()`, feature-022), porte d'alimentation fermée (`gateClosed()`, [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)). Deux sont des états d'attente normaux, deux sont des pannes.

## Décision

**1. La cause de refus est transportée par un code machine, à côté du texte.** `GET /data` et la trame WebSocket `sensor_data` publient `phDoseBlockedCode` / `orpDoseBlockedCode` : la valeur entière de l'énum `DoseRefusal` ([`src/dosing_logic.h`](../../src/dosing_logic.h)). Le texte reste publié, inchangé dans son rôle : journal, diagnostic, et repli. Le client traduit un **code** par une table (`DOSE_BLOCK_CODES`, [`data/app.js`](../../data/app.js)) et ne cherche **plus jamais** de fragment dans une phrase.

**2. Les valeurs de `DoseRefusal` sont un contrat de sérialisation figé.** Elles sont écrites **explicitement** dans l'énum, **jamais renumérotées**, **jamais insérées au milieu** : toute cause nouvelle s'ajoute **à la fin**. Vingt-trois `static_assert` verrouillent la correspondance **valeur par valeur** dans [`src/pump_controller.cpp`](../../src/pump_controller.cpp), et un `static_assert` supplémentaire garantit que l'énum ne rejoindra jamais la sentinelle 250.

**3. Deux sentinelles hors énum** ([`src/constants.h`](../../src/constants.h)) : `kDoseBlockedCodeNone` = **0** (aucun refus en cours — le champ est publié `null`), `kDoseBlockedCodeGuard` = **250** (cause posée par une garde de sécurité pH/ORP ou par l'escalade : ces messages portent des valeurs runtime, ne sont pas énumérables, et le client se rabat sur le texte). Le code est écrit **avant** le texte à chaque pose de cause : un lecteur asynchrone voit au pire le nouveau code avec l'ancien texte, et le code fait foi.

**4. Aucun libellé firmware ne mélange plus deux causes.** Les quatre raisons de `ready() == false` deviennent quatre codes, dupliqués pour les deux gardes qui les rencontrent : `ReadingNaN{Warmup,Frozen,Stale,GateClosed}` (15-18) qualifient la garde « lecture filtrée NaN », `Filter{Warmup,Frozen,Stale,GateClosed}` (19-22) la garde « filtre non prêt ». Les deux causes génériques historiques (3 et 4) subsistent et **disent explicitement qu'elles ne savent pas** (« état du filtre indéterminé »).

**5. La discrimination porte sur l'ÉTAT INTERNE du filtre, jamais sur la lecture brute.** `SensorFilter` expose quatre prédicats `const` — `warmingUp()`, `stale()`, `frozen()`, `gateClosed()` — composés par la fonction pure `resolveFilterState()` ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)), unique lieu de décision de priorité.

**6. Priorité : les pannes d'abord.** `frozen → gateClosed → stale → warmup → ready`. Un défaut ne doit jamais être masqué par un état bénin concomitant.

**7. `rearm()` arme une grâce d'observabilité, lue par `stale()` seulement.** Après un ré-amorçage, l'âge porté par le filtre est celui d'**avant** le ré-amorçage : il n'incrimine pas la sonde. La grâce dure au plus `maxAgeMs` (20 s) et est levée par la première mesure acceptée. Aucun chemin de **décision** ne la consulte : `ready()`, `unstable()` et le verdict de dosage sont inchangés au bit près. `reset()` n'arme **rien**.

**8. Le verdict de dosage est inchangé.** `DoseInputs::filterState` ne sert qu'à **qualifier** la cause. Aucune condition n'est ajoutée à `evaluateDose()`, aucun seuil n'est touché. Le champ vaut `Unknown` par défaut : un site de collecte qui l'oublierait retombe sur les causes génériques, donc sur une cause **imprécise**, jamais sur une panne maquillée en état normal.

**9. Une panne de mesure persistante devient une alerte MQTT.** `ph_dose_sensor_fault` / `orp_dose_sensor_fault` (`warning`), **une fois par épisode**, après `kDoseSensorFaultAlertDelayMs` = **60 s** de panne **continue**. L'épisode n'est pas redaté quand la panne **change de nature**.

**10. Toute fenêtre de temps est un INTERVALLE FERMÉ depuis un instant d'armement, jamais une échéance.** La grâce de ré-amorçage est mémorisée par un instant d'armement (`_staleGraceFromMs`) et une durée (`maxAgeMs`) ; le prédicat s'écrit `(now − from) < durée`. Il n'existe **aucune** variable « date de fin » ni comparaison signée vers une échéance. En complément, le drapeau est **réellement désarmé** par `_expireStaleGrace(nowMs)`, appelée en tête des deux seuls chemins non-`const` qui reçoivent l'horloge (`setContext()`, `addSample()`). Les deux protections sont **cumulatives par conception** et **testées séparément**.

### Pourquoi la forme du prédicat compte (correctif de repliement, revue v2.25.1)

La première écriture de la grâce n'évaluait l'échéance que dans `stale()`, qui est `const` : sur une sonde restée muette, le drapeau n'était **jamais** remis à faux, et la fenêtre était exprimée en **deadline** (`(int32_t)(now − until) < 0`). Cette comparaison est **juste au premier tour et fausse au second** : `2³¹ ms` après l'échéance (**24,8 jours**), la différence signée redevient négative et la grâce **se rouvre toute seule**. Une panne de sonde aurait donc été masquée à nouveau pendant 24,8 jours, l'appareil réaffichant « en stabilisation » sur une sonde morte. Le scénario est **atteignable** : `millis()` se replie à 49,7 jours et l'appareil tourne en continu.

La correction ne consiste pas seulement à désarmer le drapeau : elle change la **forme** du prédicat.

| Forme | Écriture | Comportement au repliement |
|---|---|---|
| **Deadline** (demi-droite) | `(int32_t)(now − until) < 0` | vraie sur `[until − 2³¹, until[` **puis à nouveau** sur `[until + 2³¹, …[` — le prédicat est **périodique**, il se retourne tous les 24,8 j |
| **Intervalle fermé** (retenue) | `(now − from) < durée`, non signé | vraie **exactement** pendant `durée` après l'armement, faux ensuite jusqu'au repliement **complet** de `millis()` (49,7 j — la limite intrinsèque du projet) |

La différence de fond : la deadline est monotone en `now`, donc sensible au repliement ; l'intervalle fermé est monotone en `now − from`, une **durée écoulée**, quantité intrinsèquement bornée par la durée testée.

**C'est le troisième défaut de cette famille dans le projet** — après la retentative de lecture de pente `Slope,?` (v2.21.2, résolue en extrayant la garde dans une fonction pure testable) et la comptabilité d'impulsions (`tickPulse`, rendue wrap-safe). D'où la **règle générale**, qui vaut pour tout code futur manipulant `millis()` :

> **Mémoriser l'instant d'ARMEMENT et tester la durée ÉCOULÉE (`now − from >= durée`, arithmétique non signée). Ne jamais mémoriser une date de FIN, ne jamais comparer un instant à une échéance, en particulier pas par différence signée.**
>
> Corollaires : un état à durée limitée doit être **désarmé** par un chemin qui reçoit l'horloge, jamais seulement « observé comme expiré » dans un getteur `const` ; et un getteur `const` ne doit **pas** être le seul dépositaire de l'expiration d'un état.

## Alternatives considérées

- **Continuer à analyser le texte, en corrigeant l'ordre des tests et en déconflatant les phrases** (rejetée) — c'est le correctif minimal, et il aurait suffi pour les deux cas observés. Il laisse intacte la cause : la catégorie reste dérivée d'une chaîne rédigée pour un humain, que n'importe quelle reformulation ultérieure (traduction, précision ajoutée, troncature à 96 octets) casse silencieusement. Le projet a déjà cette classe de défaut à son actif ; le coût du champ dédié est de 51 octets de payload au pire cas.
- **Remplacer le texte par le code** (rejetée) — le texte est la seule forme lisible dans le journal système et dans les alertes MQTT, où il n'y a pas de table de traduction. Il porte de plus des **valeurs runtime** (`cal=1, requis=2`, `mode=scheduled`, nombre de cycles) qu'un code seul ne peut pas transporter. Les deux canaux coexistent, avec une règle claire : le code fait foi pour la catégorie, le texte pour le détail.
- **Publier une chaîne d'identifiant stable (`"filter_frozen"`) plutôt qu'un entier** (rejetée) — plus lisible dans un `curl`, mais 10 à 20 octets par champ au lieu de 1 à 3 sur un budget de trame WebSocket déjà contraint, et surtout : une chaîne invite à la comparaison partielle (`startsWith`, `includes`), c'est-à-dire exactement au procédé qu'on retire. Un entier ne se compare que par égalité.
- **Discriminer la cause sur la fraîcheur de la lecture BRUTE** (`getPhRaw()` récent ⇒ « la sonde répond ⇒ warmup », sinon « EZO injoignable ») — **refusée par le gate `pool-chemistry`, c'est le cœur de cette décision.** Le cache brut est alimenté par **toute** lecture, y compris hors plage et y compris une valeur constante : **une sonde figée répond, donc paraît saine**. Un module EZO débitant du garbage à cadence nominale aurait été classé « stabilisation en cours », et un module figé aussi. La panne silencieuse aurait été réintroduite sous un nom rassurant, par le correctif censé la supprimer. Seul l'état **interne** du filtre distingue « la fenêtre se remplit » de « la fenêtre ne se remplira jamais ».
- **Priorité « états normaux d'abord »** (rejetée) — après une reprise de circulation sur une sonde figée, le filtre est simultanément `warmingUp` **et** `frozen` : annoncer le warmup revient à masquer la panne pendant toute la durée du remplissage, puis indéfiniment si la sonde ne bouge plus. Le test `test_V2251_figee_puis_rearm_reste_frozen_pas_warmup` verrouille ce choix.
- **`gateClosed` après `stale` dans la priorité** (rejetée) — porte fermée, l'âge de la dernière mesure acceptée croît **par conception** (rien n'est ingéré). Classer « périmé » accuserait la sonde d'un blocage voulu par le système, à chaque arrêt de filtration.
- **Falsifier la datation dans `rearm()`** (`_lastValidMs = now`, ou `_hasValid = false`) pour supprimer le faux « sonde muette » de la réouverture de porte (rejetée) — `_hasValid` et `_lastValidMs` ne décrivent **que** des échantillons réellement acceptés ; c'est l'invariant qui rend `ready()` et `ageMs()` honnêtes, et `ready()` est un chemin de sécurité chimique. On mémorise donc que l'âge courant est **imputable au ré-amorçage**, sans toucher aux données.
- **Faire lire la grâce par `ready()` aussi, par souci de cohérence** (rejetée) — ce serait autoriser le dosage sur un filtre dont on n'a aucune mesure fraîche, pour une raison purement cosmétique. La grâce est un mécanisme d'**affichage** ; l'étanchéité entre observabilité et décision est la condition de son acceptation.
- **Alerter dès la première détection de panne, sans temporisation** (rejetée) — chaque démarrage de filtration produirait une alerte « sonde muette » pendant le transitoire de ré-amorçage : un cri au loup quotidien décrédibilise l'alerte réelle. La temporisation de 60 s (12 cycles de lecture) ne masque aucune panne réelle, et l'**affichage** reste immédiat.
- **Redater l'épisode à chaque changement de code** (première écriture, corrigée en revue) — un EZO agonisant alterne `FilterFrozen` ↔ `FilterStale` par phases plus courtes que 60 s : la date de début était repoussée à chaque bascule, l'alerte ne partait **jamais**. Le correctif anti-panne-silencieuse introduisait une panne silencieuse.
- **Se contenter du désarmement effectif, en gardant la comparaison de deadline** (rejetée) — le désarmement suffit **tant que** les deux points d'appel restent en place. Or ils sont en tête de fonctions que n'importe quel refactor peut réordonner, et l'un des deux (`addSample()`) n'est pas atteint sur bus I²C mort. Une protection dont la correction dépend d'un ordre d'instructions ailleurs dans le fichier n'est pas une protection : la forme du prédicat, elle, est correcte **indépendamment de tout appelant**.
- **Se contenter de l'intervalle fermé, sans désarmement** (rejetée) — l'intervalle fermé repousse le défaut de 24,8 j à 49,7 j au lieu de le supprimer, et laisse un drapeau vrai décrire un état terminé, ce qui piège toute lecture ultérieure du champ. Les deux mesures sont donc **cumulatives, et couvertes par des tests séparés** (`test_V2251_grace_expiree_ne_se_rearme_pas_apres_2exp31_ms` pour la forme du prédicat ; `test_V2251_grace_desarmee_par_setContext_meme_sans_lecture` et `..._par_addSample_sonde_muette` pour le désarmement) : une mutation qui supprime l'une **laisse survivre** les tests de l'autre. C'est délibéré — personne ne doit pouvoir retirer une protection au motif que l'autre suffit.
- **Afficher le libellé le plus proche quand le code est inconnu** (rejetée) — un frontend plus ancien que le firmware doit **avouer son ignorance** (« Cause inconnue (code N) ») plutôt que nommer une cause plausible et fausse. C'est la même erreur que celle corrigée, transposée d'un cran.

## Conséquences

### Positives

- Une panne de mesure n'est plus affichable comme un état normal, ni l'inverse : les deux registres visuels sont **disjoints** et pilotés par le firmware, pas par une heuristique de chaîne.
- Reformuler un libellé français devient une opération **sans risque** pour l'interface, le journal et les alertes.
- L'arrêt de régulation sur sonde figée ou muette sort enfin de l'interface web : il part sur MQTT et dans le journal, une fois par épisode.
- Les quatre prédicats de `SensorFilter` et `resolveFilterState()` sont des **fonctions pures**, testées en natif ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) ; la table de priorité est vérifiée cas par cas, y compris les combinaisons simultanées réellement atteignables en production.
- Le mapping code → libellé étant total et verrouillé par `static_assert`, ajouter une cause de refus sans lui donner de libellé **ne compile pas**.
- La règle de forme des fenêtres de temps est désormais **écrite** : le projet a trois occurrences du même défaut à son actif, la quatrième n'aura plus l'excuse de l'oubli.

### Négatives / dette assumée

- **Deux canaux à maintenir en cohérence** pour la même information (code et texte). Atténuation : le code est écrit avant le texte, et `resetRefusalLogState()` / `clearGuardRefusalCause()` les remettent à zéro ensemble.
- **Quatre codes dupliqués** (15-18 et 19-22 décrivent les mêmes quatre états de filtre pour deux gardes différentes). Fusionner reviendrait à perdre l'information « laquelle des deux gardes a mordu », utile au diagnostic ; et renuméroter est désormais interdit.
- **+51 octets** au pire cas sur la trame `sensor_data` et sur la réponse `/data`.
- La grâce de ré-amorçage **retarde de 20 s au plus** l'annonce d'une sonde réellement muette au moment précis d'une réouverture de porte. Le retard est intrinsèque : on ne peut pas conclure avant d'avoir laissé sa chance à la première lecture qui suit le ré-amorçage. Une sonde muette **au démarrage à froid** reste annoncée sans délai (`reset()` n'arme pas la grâce).
- L'alerte MQTT couvre `Frozen` et `Stale`, **pas** `GateClosed` ni `Warmup` : c'est voulu (états normaux), mais un blocage durable de la porte de validité reste couvert uniquement par l'alarme `ph_filter_not_ready` existante (5 min).

### Ce que ça verrouille

- **Les valeurs numériques de `DoseRefusal` sont publiques et définitives.** Aucune renumérotation, aucune insertion, aucune suppression : une valeur retirée du firmware doit rester **réservée** dans l'énum. Toute cause nouvelle s'ajoute à la fin, avec son libellé et son `static_assert`.
- **La sentinelle 250 est réservée** aux causes non énumérables (gardes de sécurité, escalade). L'énum ne peut pas croître jusqu'à elle sans casser la compilation.
- **Tout nouveau champ affiché par l'interface doit exister sur `/data` ET sur la trame WebSocket**, même nom, même valeur, mêmes règles de nullité ([ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)).
- **Un diagnostic d'état de filtre ne se reconstruit jamais depuis un cache de lecture brute**, ni côté firmware, ni côté client. La question « la sonde répond-elle ? » n'a pas de réponse utilisable : une sonde figée répond.
- **La priorité de `resolveFilterState()` est le seul lieu où l'ordre est décidé.** Aucun appelant ne réordonne, ne court-circuite ni ne recompose ces quatre prédicats.
- **Étanchéité observabilité / décision** : tout mécanisme ajouté pour améliorer un affichage (grâce, temporisation, dé-duplication) doit rester hors du chemin du verdict de dosage. La grâce est lue par `stale()`, pas par `ready()` ; c'est une contrainte à respecter pour tout mécanisme analogue.
- **Un client ne devine jamais une catégorie.** Code absent ⇒ information indisponible (repli sur le texte, affiché tel quel) ; code inconnu ⇒ aveu d'ignorance. Jamais d'approximation par voisinage.
- **Toute fenêtre de temps future s'écrit `now − instant_d'armement` vs durée, en non signé.** Pas de date de fin mémorisée, pas de comparaison signée à une échéance, pas d'expiration confiée au seul getteur `const` qui la lit. Cette règle s'applique à `pump_controller`, `sensors`, `filtration`, `mqtt_manager` comme à tout module ajouté ensuite.
- **Les deux protections de la grâce ne se retirent pas l'une au motif que l'autre suffit.** Leurs tests sont séparés précisément pour rendre ce retrait visible en CI.

## Références

- Code : [`src/dosing_logic.h`](../../src/dosing_logic.h) (`DoseRefusal`, `DoseFilterState`, `resolveFilterState`), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`static_assert`, `logRefusalOnce`, mapping code → libellé), [`src/pump_controller.h`](../../src/pump_controller.h) (`getPhDoseBlockedCode` / `getOrpDoseBlockedCode`), [`src/sensor_filter.h`](../../src/sensor_filter.h) / [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) (`warmingUp`, `stale`, `frozen`, `gateClosed`, grâce de ré-amorçage), [`src/sensors.h`](../../src/sensors.h) (`getPhFilterState` / `getOrpFilterState`), [`src/constants.h`](../../src/constants.h) (`kDoseBlockedCodeNone`, `kDoseBlockedCodeGuard`, `kDoseSensorFaultAlertDelayMs`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp), [`src/ws_manager.cpp`](../../src/ws_manager.cpp), [`data/app.js`](../../data/app.js) (`DOSE_BLOCK_CODES`, `_doseBlockInfo`)
- Tests : `test/test_native_sensor_filter/` (prédicats d'état, priorité, grâce, **repliement** : `test_V2251_grace_expiree_ne_se_rearme_pas_apres_2exp31_ms`, `test_V2251_grace_traverse_le_repliement_de_millis`, `test_V2251_grace_desarmee_par_setContext_meme_sans_lecture`, `test_V2251_grace_desarmee_par_addSample_sonde_muette`), `test/test_native_dosing/` (qualification des causes, sentinelles)
- Suivi : [`specs/features/todo/feature-077-extraction-machine-episode-panne-sonde.md`](../../specs/features/todo/feature-077-extraction-machine-episode-panne-sonde.md) (extraction de la machine d'épisode en module pur, ADR-0017)
- Spec : [`specs/features/done/fix-cause-de-refus-de-dosage-devinee-par-texte.md`](../../specs/features/done/fix-cause-de-refus-de-dosage-devinee-par-texte.md)
- Doc externe : [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md), [`docs/subsystems/sensors.md`](../subsystems/sensors.md), [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md), [`docs/features/page-ph.md`](../features/page-ph.md), [`docs/features/page-orp.md`](../features/page-orp.md)
