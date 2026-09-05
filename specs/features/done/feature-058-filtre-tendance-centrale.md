## Métadonnées
- **ID** : feature-058
- **Slug** : filtre-tendance-centrale
- **Type** : feature (amélioration filtre de mesure pH/ORP)
- **Statut** : done
- **Priorité** : haute
- **Version cible** : 2.8.0 (change la sémantique de la valeur filtrée → entrée PID)
- **Dernière mise à jour** : 2026-07-11
- **Spec(s) liée(s)** : feature-025 (filtre médian+EMA d'origine), feature-057 (trace qui a révélé le collage)

## Domaines touchés
- [x] Capteurs (`sensor_filter.*`) — logique PURE testable (ADR-0017)
- [x] Régulation / dosage → **`pool-chemistry` OBLIGATOIRE** (la valeur filtrée est l'entrée du PID)
- [ ] Web / MQTT (affichage inchangé, sauf éventuel drapeau fiabilité — cf. hors périmètre)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — **OBLIGATOIRE, double passage** (avant conception des paramètres + après implémentation) : modifie l'entrée du PID pH/ORP.
- [x] embedded-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Rendre la valeur **lissée** pH/ORP représentative de la **tendance centrale** d'un signal fortement oscillant, au lieu de **coller au haut ou au bas** de l'enveloppe (ou de geler) comme observé via la trace feature-057. La valeur doit rester **robuste aux vraies aberrations** (NaN, hors-plage, spikes isolés) et la **détection d'instabilité** (inhibition régulation) doit être **préservée** — mais **découplée** de la valeur (ne plus figer la valeur sur latch).

## Contexte
Diagnostic feature-057 : sous forte oscillation (bruit CEM/pompe, ±0,3 pH, période ~20 s), le filtre actuel (**rejet de saut → médiane(7) → EMA α=0,10**) :
1. **rejette un côté** de l'oscillation (le rejet de saut traite les transitions comme aberrations) → l'EMA n'intègre qu'un côté → **colle à une enveloppe** (bas hier, haut aujourd'hui) ;
2. **fenêtre médiane (7) < période** → ne moyenne pas symétriquement ;
3. sur rejets saturés + latch instable, **fige la valeur** à un nombre trompeur (ex. 7,13 alors que le vrai pH ≈ 7,6–7,7).
La cause racine du bruit est **matérielle** (isolation/pompe, en cours de traitement par l'utilisateur) ; en attendant, on veut une valeur filtrée **honnête (centrée)** — ce qui est **plus sûr** qu'une valeur collée/gelée.

## Comportement attendu
- **Valeur filtrée = tendance centrale** : sur une oscillation ~symétrique, le lissé se cale au **centre** (≈ moyenne/médiane sur ≥ 1 période), pas sur une enveloppe. Ne gèle pas.
- **Robustesse aberrations conservée** : NaN / hors-plage / spike isolé toujours écartés (le rejet ne concerne QUE les vraies aberrations, pas l'oscillation légitime soutenue).
- **Latence acceptée** : fenêtre plus longue = réponse plus lente → **acceptable pour pH/ORP** (variation lente) et **plus prudente** (ne chasse pas le bruit).
- **Drapeau de fiabilité découplé** : `isPhFilterUnstable()` / `isOrpFilterUnstable()` continuent d'**inhiber la régulation** sous bruit extrême, MAIS **ne figent plus** la valeur : la valeur reste le meilleur estimateur central courant, le drapeau dit séparément si on peut s'y fier pour doser.
- **Amorçage (warmup)** inchangé dans l'esprit : NaN tant que pas assez d'échantillons.

### Cas limites
- Oscillation **asymétrique** (ex. pompe qui tire vers le bas) : le centre est biaisé vers le côté dominant — inévitable sur signal corrompu, mais **mieux** qu'un collage d'enveloppe ; le drapeau instable doit alors s'activer si la variance est extrême.
- Spike isolé unique → écarté, la valeur ne bouge quasi pas.
- Signal propre → comportement au moins aussi bon qu'avant (pas de régression de réactivité utile).
- Warmup / capteur indisponible → NaN, régulation inhibée (inchangé).

## Comportement actuel
`SensorFilter` : rejet de saut (seuil) → médiane fenêtre `kSensorFilterMedianWindow=7` → EMA (`kPhEmaAlpha=0,10`, `kOrpEmaAlpha=0,08`) ; resync après N rejets ; **latch `_unstableLatched`** anti-boucle qui **fige** la valeur jusqu'à `reset()`. `filtered()` = EMA (NaN si warmup non amorcé).

## Contraintes techniques
- **Logique 100 % pure** (`sensor_filter.*`, pas d'Arduino.h/String) → **tests natifs** obligatoires (suite `test_native_sensor_filter`).
- **Zéro allocation dynamique** (buffers FIXES, comme l'existant) ; RAM bornée (fenêtre plus longue = buffer plus grand, chiffrer le surcoût).
- **Iso-sécurité ou plus sûr** : ne JAMAIS produire une valeur qui rendrait la régulation plus agressive qu'aujourd'hui ; conserver l'inhibition sur instabilité.
- Frontière pur/coquille inchangée ; `getPhFiltered()/getOrpFiltered()` restent les accesseurs consommés par le PID.

## Livrables — check-list
- [x] A1 — Estimateur de tendance centrale robuste (médiane longue `kSensorFilterCentralWindow`=13) dans `sensor_filter.*`, fenêtre/paramètres en `constants.h`
- [x] A2 — Rejet réservé aux vraies aberrations (NaN/hors-plage/spike dur isolé `spikeReject`) ; l'oscillation soutenue n'est plus rejetée en masse
- [x] A3 — Découplage valeur / latch : `filtered()` reste centrée même en état instable ; `isXxxFilterUnstable()` continue d'inhiber la régulation (dispersion > cap OU latch EMI OU rejets consécutifs)
- [x] D1 — Tests natifs : oscillation symétrique → centre ; asymétrique → borné ; spike isolé → écarté ; garbage → instable ; warmup → NaN ; non-régression signal propre. 218 tests natifs verts.
- [x] C1 — Docs : `docs/subsystems/sensors.md` (nouvelle chaîne + paramètres), `CHANGELOG.md` [2.8.0], ADR-0027 créé

## Nouvelles signatures / API
- Pas de nouvelle API publique attendue (mêmes `filtered()/median()/unstable()`), sauf éventuel accesseur d'état de fiabilité pour l'UI (optionnel, cf. hors périmètre).
- Constantes : taille de fenêtre centrale (ex. `kSensorFilterCentralWindow`), éventuel taux de troncature — valeurs **arbitrées par pool-chemistry**.

## Sécurité chimique (obligatoire)
- Dosage impliqué : **oui** — la valeur filtrée est l'entrée du PID pH (acide) et ORP (chlore).
- Risque : une valeur **biaisée** ou **en retard** pourrait sous- ou sur-doser. `pool-chemistry` doit valider : (1) le centrage ne rend jamais la régulation plus agressive ; (2) la latence ajoutée est tolérable (pas de dépassement) ; (3) le drapeau d'inhibition reste efficace sous bruit extrême ; (4) valeurs de fenêtre/troncature.
- Algorithme PID touché : non (seule l'entrée change) ; hystérésis/zone morte inchangées.

## ADR attendu ?
- **Probable** : à trancher par architect — si le changement de stratégie de filtrage (rejet→robuste central) est jugé structurant, ADR successeur/complément de la logique feature-025.

## Éléments visibles (doc-writer)
- `docs/subsystems/sensors.md` — chaîne de filtrage révisée + paramètres + contrat fiabilité découplée.
- `CHANGELOG.md` — [2.8.0].

## Critères d'acceptation
- [x] Sur une oscillation symétrique synthétique, la valeur filtrée converge vers le **centre** (± tolérance), pas vers une enveloppe, et **ne gèle pas**.
- [x] Spike isolé écarté ; garbage/NaN → instable + régulation inhibée (inchangé).
- [x] La valeur reste publiée (non figée) même en état instable ; le drapeau d'inhibition fonctionne.
- [x] Aucune combinaison ne rend la régulation plus agressive qu'avant (validé pool-chemistry, double passage GO).
- [x] `pio run` SUCCESS ; `pio test -e native` : 218 tests verts (existants + nouveaux tests filtre).
- [x] Libellés/logs en français.

## Plan de test manuel (on-target)
1. Réobserver la trace feature-057 sous oscillation pompe-ON : le lissé doit se caler au **centre**, plus coller.
2. Vérifier que la régulation reste **inhibée** tant que le drapeau instable est posé.
3. Signal propre (après fix matériel) : réactivité correcte, pas de retard excessif.

## Hors périmètre
- Correction de la **source matérielle** du bruit (isolation/pompe/TMA-0505) — traitée séparément par l'utilisateur.
- Refonte de l'UI ; un éventuel **badge « non fiable »** sur la valeur (amélioration d'observabilité) peut être ajouté ici ou dans un lot UI séparé — à décider avec l'architect (sinon hors périmètre).

## Suivi / itérations

### Itération 1 — livré — tests verts, revue OK (v2.8.0, 2026-07-11)

**Chaîne finale** : brut → rejet vraies aberrations (NaN / hors-plage / spike dur isolé `|raw − médiane| > spikeReject`) → médiane fenêtre longue `kSensorFilterCentralWindow`=13 (buffer physique 16) → EMA (α pH 0,10 / ORP 0,08).

**Contrat** : `filtered()` recalculée en continu, jamais figée, NaN uniquement tant que la fenêtre n'est pas pleine. Invariant iso-sécurité : valeur ∈ [min, max] de la fenêtre, jamais tirée vers la consigne. `unstable()` **découplé** = dispersion (`max−min` fenêtre pleine) > cap **OU** latch EMI **OU** rejets consécutifs — inhibe le dosage sans figer la valeur.

**Paramètres** (pool-chemistry, double passage GO) : `kPhFilterMaxDispersion`=0,5 / `kOrpFilterMaxDispersion`=100 ; `kPhFilterSpikeReject`=1,0 / `kOrpFilterSpikeReject`=300 ; `kSensorFilterDispersionLatchCycles`=3 ; `kSensorFilterMaxConsecutiveRejects`=10 conservé. **Retirées** : `kSensorFilterMedianWindow`, `kPhFilterMaxStep`/`kOrpFilterMaxStep`, `kSensorFilterWarmupSamples`, `kSensorFilterResyncRejects`, `kSensorFilterMaxResyncPerWindow`, `kSensorFilterResyncWindowMs` (mécanique re-sync/maxStep supprimée).

**Latence** : +15 s (fenêtre 7→13, retard médian ≈30 s) — négligeable devant la pause mélange. Module `sensor_filter.*` rendu 100 % pur (testable natif, ADR-0017).

**Fichiers modifiés** : `src/sensor_filter.h`, `src/sensor_filter.cpp`, `src/constants.h`, `test/test_native_sensor_filter/`. Build SUCCESS, 218 tests natifs verts.

**Décision structurante** : [ADR-0027](../../../docs/adr/0027-filtre-tendance-centrale.md) — estimateur central robuste + découplage valeur/latch (successeur de la stratégie feature-025).
