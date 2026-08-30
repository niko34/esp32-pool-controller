# Changelog - ESP32 Pool Controller

> ℹ️ **Réalignement des versions** : le versioning a été réaligné sur la dernière release publiée `v2.6.0`. Les entrées `[2.7.0]` et suivantes suivent cette numérotation. Les entrées historiques `[2.18.x]` / `[2.19.x]` ci-dessous correspondent à des développements antérieurs à ce réalignement (numérotation conservée telle quelle).

## [2.26.0] - 2026-08-28

> ⚠️ **La régulation pH devrait enfin atteindre sa consigne.** Jusqu'ici elle se stabilisait systématiquement **au-dessus** : 7,50-7,66 mesurés pour une consigne à 7,30, malgré ~1 000 mL d'acide injectés dans la journée. Ce n'était **ni une panne, ni un problème de dosage, ni un produit trop dilué** : c'est une propriété mathématique du correcteur utilisé.
>
> Un correcteur **proportionnel** ne délivre du produit que **proportionnellement à l'écart** au but. Face à une dérive **permanente** (le CO₂ qui s'échappe de l'eau fait remonter le pH en continu, ~80 % de la charge ; le chlore, basique, apporte le reste), il s'arrête donc là où la dose qu'il produit compense exactement la dérive — **à un écart non nul de la consigne**. Vérification : avec les réglages en place, cet écart d'équilibre vaut 0,358 pour 10,5 h de filtration par jour. **Le système était exactement à son équilibre théorique.**
>
> La correction ajoute un second terme, qui **accumule le retard** au lieu de ne regarder que l'écart instantané : la dose ne dépend plus seulement de « à combien suis-je du but », mais aussi de « depuis combien de temps je n'y suis pas ». C'est ce terme qui, une fois la consigne atteinte, fournit **toute** la dose d'entretien. Point d'équilibre attendu : **consigne + 0,01** — la valeur exacte à laquelle la régulation coupe de toute façon.
>
> ⚠️ **Résultat non vérifié sur le terrain.** La correction est fondée sur la théorie du réglage, vérifiée en boucle fermée simulée et par bilan de matière, mais **elle n'a pas encore tourné une saison sur le bassin**. Surveiller le pH et le compteur journalier pendant quelques jours. Si le pH ne descend toujours pas, c'est l'estimation de la charge (~1 800 mL/jour) qu'il faudra revoir, pas le correcteur.
>
> ✅ **Aucun réglage à changer.** Les limites journalière et horaire, les seuils de sécurité et les modes sont inchangés.

### Firmware

- **Régulation pH : de P pur à PI (`Ki` levé)** — gate `pool-chemistry` **GO SOUS CONDITIONS C1-C5**. Le `Ki = 0` posé comme **non négociable** par la feature-025 est levé sur la **seule** boucle pH. `Kd = 0` reste **impératif sur les deux chimies** et **`Ki_ORP` reste à 0** (hors périmètre : l'ORP dépend du pH, de la T° et du stabilisant, et son écart résiduel n'est pas le défaut diagnostiqué).
- **Pourquoi c'était juste en feature-025 et pourquoi ça ne l'est plus** : à l'époque, la régulation venait de basculer sur une mesure filtrée, la pause mélange n'avait aucun recul terrain et le risque d'emballement de l'intégrale était réel et non maîtrisé. L'ADR-0016 avait explicitement prévu la sortie (« infrastructure conservée pour une réactivation progressive après validation terrain »). Depuis : l'**anti-windup est complet et testé à 100 %** (bande morte ⇒ sortie nulle **et** intégrale non modifiée, clamp symétrique) et **six barrières de sécurité** ont été ajoutées (seuils pH fixes avec `static_assert`, deux butées de dernier recours sur le pH brut à 6,20 / 8,10, hystérésis par seuil, surveillance hors conjonction, escalade fail-closed à 3 refus/24 h, inhibition du chlore sur pH haut), en plus des limites journalière et horaire.
- **Le plafond de l'intégrale se dimensionne par l'AUTORITÉ, jamais « en pH·s »** : on pose le débit maximal que l'intégrale seule peut demander (`kPhIntegralAuthorityMlPerMin` = **6,0 mL/min**, soit 2× la charge mesurée de 2,86), puis `integralMax = U × Ti / kp`. Table : `slow` kp 4 / Ti 8 h / **43 200** ; `normal` kp 8 / Ti 6 h / **16 200** ; `fast` kp 12 / Ti 4 h / **7 200**. **`integralMax` VARIE donc avec la vitesse de régulation**, exactement comme `kp` — l'autorité est constante, seule la vitesse de charge change. **9 `static_assert`** verrouillent l'ensemble, dont le verrou central `ki × integralMax ≈ autorité` pour les trois vitesses.
- ⚠️ **Le piège évité, à ne jamais refaire** : l'ancien `integralMax = 50` (inerte tant que `Ki` valait 0) aurait plafonné la contribution intégrale à **0,0185 mL/min** pour un besoin de 2,86 — **0,6 %**, saturé en **140 s**. Réactiver `Ki` sans redimensionner ce plafond aurait été un **échec silencieux** : une régulation « avec intégrale » sur le papier, l'écart résiduel intact dans le bassin. C'est exactement ce que le `static_assert` central rend désormais impossible. Rappel structurant : **à l'équilibre l'erreur tend vers 0, le terme proportionnel disparaît et l'intégrale porte TOUTE la charge**.
- **Intégration SIGNÉE sur `erreur − phStopThreshold`, par quanta d'une seconde**, évaluée à **chaque tick automatique** (que l'on dose ou non ; la sortie, elle, reste gatée). La **décharge devient structurelle** : sous le seuil d'arrêt, l'intégrande devient négatif. **Le quantum n'est pas cosmétique** — à 16 200 pH·s l'*ulp* du flottant vaut 2e-3, un incrément de 5 ms × 0,01 pH aurait été **purement perdu à l'addition** et la décharge n'aurait pas existé. `dt > 10 s` **rejeté** (jamais crédité d'un bloc). Gel d'anti-windup **asymétrique** : il bloque la charge, **jamais** la décharge.
- **Défaut préexistant corrigé au passage** : la bande morte du calcul valait **toujours** `phStartThreshold` (0,05), alors que l'hystérésis de dosage poursuit jusqu'à `phStopThreshold` (0,01). La plage `]0,01 ; 0,05]` était donc une **séance vivante à débit nul**, et le point d'équilibre visé par l'intégration signée était **inatteignable par la sortie**. La bande morte suit désormais l'hystérésis. Le volume total injecté à l'équilibre est **inchangé** (bilan de matière).
- **Nouveau module de logique pure `src/ph_integral_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : charge, oubli, péremption 24 h, hystérésis de saturation. **Motif** : les conditions C2-C5 vivaient dans la coquille ESP32 et **aucune mutation ne les tuait** — et le défaut trouvé était **exactement** dans la garde de C2.
- **Garde-fous** : gel de l'accumulation si la sonde sature la demande (prédicat sur le **débit moyen soutenable** `ampFlow × budget/60`, et non sur le débit d'amplitude — comparé à l'amplitude, ce gel ne s'était **jamais** déclenché) ou si le quota journalier dépasse **80 %** ; **péremption à 24 h** sans évaluation ; **jamais de persistance NVS** (une intégrale stockée, a fortiori corrompue, serait une **autorisation de dosage pré-chargée au boot**). Purge également au changement **réel** de vitesse de régulation, à la bascule `ph_minus` ↔ `ph_plus` et en fin de mode Programmée.
- **Surveillance de saturation** : anomalie = demande **saturée** ET erreur **encore au-dessus du seuil d'arrêt** ; tenue comptée en **temps de branche automatique** (un passage en Manuel suspend l'épisode) ; hystérésis **95 / 85 %** ; `warning` à **2 h**, `critical` à **6 h**, **rappel du critical toutes les 12 h**. Une seule alerte par épisode laissait un silence **permanent** sur un état qui, lui, ne cessait pas.
- **MQTT** : 3 topics retain `{base}/ph_integral`, `/ph_integral_term`, `/ph_integral_pct` + 3 entités auto-discovery HA (« Piscine pH Rattrapage » en tête, les deux autres en `diagnostic`) + alerte **`ph_integral_saturated`**. Aucune entité existante touchée. `kDedupCacheSlots` 18 → 21, publications de discovery 49 → 52.
- **Format de journal changé** : la ligne de configuration PID devient `PID régulation: vitesse=… Kp_pH=… Ki_pH=… (Ti=…h, intMax=… pH·s, autorité=… mL/min) Kp_ORP=… Ki_ORP=0 Kd=0`. Un filtre de journal sur « P temporisée » ou « Ki=0 » **ne matche plus**.

### Fonctionnalités

- **`GET /data` et WebSocket** : 3 champs nouveaux, en **parité stricte** (mêmes noms, mêmes arrondis, `null` explicite) — `ph_integral` (pH·s, signé), `ph_integral_term` (mL/min), `ph_integral_pct` (% du plafond). Quatrième application du contrat de parité après les pentes de sonde (v2.21.2), les champs de calibration (v2.23.0) et l'état de blocage de dosage (v2.25.0). Gabarit de trame WS porté 2560 → 2688 (marge sous le tampon dur : 1 032 o).
- **Page pH — bloc « Rattrapage accumulé »** en pied de la carte Régulation, **en mode Automatique uniquement** : pourcentage de remplissage en tête, détail discret (pH·s et mL/min demandés), et encart d'avertissement au-delà de 95 %. Libellés « correction lente » / « rattrapage accumulé » — aucun jargon d'automatique. Le bloc est **masqué en bloc** sur un firmware antérieur (clé absente ≠ `0`).
- **Le pourcentage est servi par le firmware, jamais recalculé côté client** : le plafond de l'anti-windup **varie** avec la vitesse de correction, le recopier dans le navigateur rouvrirait le motif du miroir de constante, éliminé quatre fois dans ce projet.

### Documentation

- **[ADR-0052](docs/adr/0052-terme-integral-ph-dimensionne-par-autorite.md) créé** — *Levée de `Ki = 0` sur la régulation pH : terme intégral dimensionné par l'AUTORITÉ, intégré en signé sur le seuil d'arrêt*. **Amende le `Ki = 0` de l'[ADR-0016](docs/adr/0016-regulation-p-temporisee-vs-pid.md)** (dont le statut est annoté en conséquence ; `Kd = 0`, `Ki_ORP = 0`, temporisation et anti-windup strict **inchangés**). Consigne le diagnostic de l'écart résiduel, la vérification numérique, la raison pour laquelle `Ki = 0` était **juste en 2026-06** et ne l'est plus, les **sept alternatives écartées** — dont le **biais de consigne**, refusé par principe (« les règles de protection ne doivent pas empêcher la bonne régulation » : une consigne biaisée cesserait de signifier ce qu'elle affiche et exigerait une révision saisonnière, intenable sur une flotte) et `kp 8 → 12`, insuffisant (l'écart tomberait à 0,24 ; atteindre 7,35 en P pur exigerait `kp ≈ 86`) — et **ce que ça verrouille**.
- `docs/subsystems/pump-controller.md` : section « Terme intégral pH — levée de `Ki = 0` », table de dimensionnement par vitesse, points de purge, garde-fous, surveillance, journal, testabilité ; tables PID / constantes / ADR liés mises à jour ; note P5 (« aucun `dt` synthétisé ») requalifiée en garde **active**.
- `docs/API.md`, `docs/subsystems/ws-manager.md` : les 3 champs, budgets de payload, règles de consommation client (clé absente ≠ `null` ≠ `0`).
- `docs/MQTT.md`, `docs/subsystems/mqtt-manager.md` : 3 topics, 3 entités HA, alerte `ph_integral_saturated` (deux paliers, rappel 12 h), slots de déduplication.
- `docs/features/page-ph.md` : bloc « Rattrapage accumulé », règles d'affichage, pourquoi le pourcentage est servi et non recalculé, absences volontaires (tableau de bord, Réglages).
- `docs/UPDATE_GUIDE.md` : note de migration v2.26.0 — aucune action requise, effet attendu et **réserve explicite** sur la vérification terrain.
- `specs/features/todo/feature-078-*.md` : suivi v2.26.0 — la **cause n°1** (« la loi P plafonne la demande ») est traitée ; les mesures M1/M2 et la correction du modèle de comptage restent dues.

### Tests

- **823 → 849 tests** natifs, suite dédiée `test/test_native_ph_integral/`. **Cinq mutations tuent désormais**, dont **trois qui n'en tuaient aucune** avant l'extraction du module pur. Le **test de droop** vérifie la propriété qui fonde la décision : en boucle fermée, un P pur **ne converge pas**, un PI **converge**.

---

## [2.25.3] - 2026-08-28

> ⚠️ **Les injections de correcteur pH deviennent moins fréquentes mais deux fois plus longues.** La durée minimale d'une injection passe de **5 à 10 secondes**. **La quantité demandée par la régulation ne change pas** : ce sont les mêmes millilitres, servis en paquets deux fois plus gros et deux fois moins souvent.
>
> Pourquoi : sur une pompe péristaltique, **chaque démarrage coûte du produit qui n'est pas compté** (le tube se détend, se ré-occlut, la colonne se ré-amorce : 0,1 à 0,5 mL, sur une pompe dont un tour vaut 1-2 mL). Comme **toutes** les injections étaient à la durée minimale, cette perte portait sur **la totalité** du volume. Deux fois moins de démarrages pour le même volume, c'est cette perte amortie sur deux fois plus de produit utile.
>
> Ce qui a déclenché le changement : **968 mL comptés en une journée** et un pH resté à **7,50** pour une consigne à **7,30**, alors que le besoin réel est de **800-900 mL** pour un tel écart sur 40 m³ — chiffre concordant par le calcul chimique **et** par le dosage constructeur. Le pH devrait donc enfin descendre ; **c'est une hypothèse en cours de vérification**, pas un résultat mesuré.
>
> ⚠️ **Dette assumée, à connaître** : le comptage du volume reste **faux** (il ignore la perte au démarrage). En divisant les démarrages par deux à volume *compté* constant, on délivre **réellement** un peu plus de produit sous le même plafond journalier — quelques dizaines de mL par jour. La limite journalière devient donc légèrement **moins prudente**. Il est recommandé de **surveiller le plafond journalier pendant 3 jours** après la mise à jour.

### Firmware

- **Durée minimale d'une impulsion de dosage : `kPulseMinMs` 5 000 → 10 000 ms** (gate `pool-chemistry` : **GO SOUS CONDITIONS**). Un seul entier change — ni la période du train d'impulsions, ni l'amplitude des pompes, ni le modèle de comptage.
- **Le changement n'augmente PAS la dose demandée** — c'est le point contre-intuitif. `planPulseWithCarry` accumule `demande × période` et retire **exactement** le volume d'une impulsion minimale au moment du tir : la moyenne restituée est **mathématiquement indépendante** de cette constante (invariance vérifiée par le gate dans le code). Ce qui change est le **rendement** : deux fois moins de démarrages pour le même volume.
- **Chiffres** (débit à l'amplitude **mesuré** 25,75 mL/min) : volume d'une impulsion minimale **2,47 → 4,61 mL** ; seuil de bascule vers le régime proportionnel direct **2,15 → 4,29 mL/min** de demande ; à une demande de 1,76 mL/min, une impulsion toutes les **~2,7 min au lieu de ~1,4 min**.
- **Innocuité du paquet unitaire** : 4,6 mL = **32 méq**, soit **ΔpH < 0,002** même sur 15 m³ — négligeable jusqu'à ~5 m³ (réserve du gate : un spa de moins de 2 m³ sortirait de ce raisonnement, hors périmètre de la flotte).
- **Effets de bord instruits, tous inchangés** : les cycles/jour et l'anti-rafale sont comptés par **séance**, pas par impulsion ; la limite horaire est réévaluée à chaque tick, donc un dépassement reste borné par un tour de boucle et non par 10 s ; la garde de temps minimum d'injection raisonne en **temps pompe-ON cumulé**, donc la durée murale d'une séance ne bouge pas ; la pause mélange s'arme en **fin de séance** seulement.
- **La période reste à 60 s** (`kPulsePeriodMs`, décision explicite) : la fraction de temps pompe maximale est inchangée, et le refus antérieur de l'utilisateur d'allonger la période reste fondé — l'allonger dégraderait la latence de régulation sans rien gagner sur la moyenne.
- ⚠️ **Le compteur de volume reste faux** (condition n°1 du gate) : cet allongement **atténue** le défaut, il ne le corrige pas. La **mesure de la perte fixe par démarrage reste due** (24 impulsions de 5 s contre 12 de 10 s, à temps de pompe égal), après quoi le modèle de comptage devra être corrigé — et traité explicitement comme une **hausse de dose**, pas comme un correctif neutre. Suivi : [feature-078](specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md).
- **Message de journal** : `Dosage par accumulation : impulsion minimale de …` affiche désormais **10 s**. Une recherche de journal portant sur « impulsion minimale de 5 s » ne matche plus.

### Documentation

- **[ADR-0051](docs/adr/0051-pouls-minimal-double-rendement-avant-comptage.md) créé** — *Pouls minimal doublé (5 s → 10 s) : améliorer le rendement d'abord, corriger le comptage ensuite*. **Amende le pouls minimal (condition B3) de l'[ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** (dont le statut est annoté en conséquence ; le reste — proportionnel de temps, amplitude calibrée, kick, aucune interpolation — est inchangé, comme l'est la totalité de l'[ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md)). Consigne l'invariance de la dose moyenne, les six alternatives écartées (corriger le comptage d'abord, allonger la période, relever `kp`, augmenter l'amplitude, relever la limite journalière, pouls > 10 s), la **dette de comptage assumée** et ce qu'elle verrouille (pas de retour sous 10 s, pas d'empilement de deux atténuations non mesurées).
- `docs/subsystems/pump-controller.md` : nouvelle section « Pouls minimal porté à 10 s », table avant/après, recalcul des exemples chiffrés de l'accumulateur (`V_pouls` 4,61 mL, cadences), écart du volume nominal vs réel ramené à +7-8 % au pouls de 10 s, effet du kick sur les compteurs journaliers divisé par deux, tables de constantes et d'ADR liés mises à jour.
- `docs/subsystems/logger.md` : le message de tir d'accumulateur affiche `10 s`, conséquence sur les filtres de journal et sur le volume de journal produit.
- `docs/API.md` et `docs/subsystems/ws-manager.md` : description de `ph/orp_pulse_ms = 0` corrigée — la valeur du pouls minimal est mise à jour et la mention obsolète du « seuil C » (refus supprimé en v2.22.0 au profit de l'accumulateur) disparaît.
- `docs/UPDATE_GUIDE.md` : note de migration v2.25.3 — aucune action requise, changement de rythme des injections, surveillance du plafond journalier recommandée sur 3 jours.

---

## [2.25.2] - 2026-08-28

> ⚠️ **Les compteurs de dosage journaliers se remplissent légèrement plus vite.** Le débit utilisé pour créditer les brefs à-coups de démarrage des pompes (et les injections manuelles) était mesuré sur **un seul exemplaire de pompe**, puis figé en dur sur un numéro de pompe. Il devient un **majorant unique**, valable pour tous les appareils. Conséquence visible : environ **+3,6 % de volume compté sur la pompe pH** et **+1,9 % sur la pompe chlore** (soit ~+47 mL et ~+25 mL par jour sur ~1 300 mL comptés). Rien n'est injecté en plus — c'est le **comptage** qui devient prudent, donc les limites journalières mordent un peu plus tôt.
>
> Le défaut corrigé : sur un appareil dont la pompe pH aurait été de l'exemplaire le plus rapide, le firmware **sous-comptait de 11 %** — il injectait plus que ses propres limites ne le croyaient.

### Firmware

- **Un seul débit à pleine puissance pour toute la flotte.** `kPump1Flow255MlPerMin = 61,0` et `kPump2Flow255MlPerMin = 67,0` sont remplacées par **`kPumpFlow255MlPerMin = 74,0`**. Ces valeurs n'étaient **pas configurables**, provenaient d'**un seul exemplaire chacune** et servaient de **majorant de sécurité** dans trois usages : crédit de l'à-coup de démarrage de chaque impulsion, majorant du mode manuel `?duty=N`, durée d'une injection volumée sur le chemin **dégradé**. Attribuer la mesure d'un exemplaire à un **index physique de pompe** n'a aucun sens sur une flotte. Les helpers `pumpFlow255()` / `flowAssistFlow255()`, qui réalisaient ce dispatch, sont **supprimés** (11 sites d'appel ramenés à la constante).
- **Pourquoi 74,0 et non 70,0** : la valeur vaut **maximum réellement mesuré 68,0 + 8 % de dispersion inter-unités**. 70,0 a été **refusé par le gate `pool-chemistry`** — +3 % de marge alors que l'écart **déjà constaté entre deux exemplaires du même lot est de 11 %** : une marge inférieure à la dispersion observée est statistiquement nulle.
- **Pourquoi une seule valeur et non deux.** Dédoubler la constante (un majorant pour créditer, un estimateur central pour calculer la durée) a été **rejeté** : les trois usages poussent dans le **même sens**, surestimer ne sur-injecte jamais. Un « estimateur central » destiné à la durée serait par construction une valeur choisie pour **allonger la marche de la pompe**, soit exactement le risque interdit. La précision de l'injection volumée dispose déjà d'un **chemin exact** (amplitude == duty calibré ⇒ débit mesuré) ; le chemin à 255 est un **repli dégradé** dont la précision ne s'achète pas contre une garde.
- **Le débit d'usine à mi-puissance reste 28,0 mL/min, mais sa justification est refondée sur une MESURE.** Les mesures réelles du 2026-08-28 (assistant de mesure du débit, 120 s à duty 127) donnent **26,0 mL/min pour la pompe 1 et 25,5 pour la pompe 2** — les interpolations d'origine (24,7 / 27,2) étaient bonnes à **4-7 %** près. La valeur d'usine n'est donc plus « interpolation » mais **max mesuré 26,0 + 8 % de dispersion**. 26,0 comme défaut a été **refusé** pour la même raison que 70,0 : c'est le maximum de **n = 2 exemplaires du même lot**, donc une marge nulle.
- **Deux `static_assert` interdisent désormais de descendre une valeur d'usine sous le maximum réellement mesuré** : nouvelles constantes `kPumpFlow255MeasuredMaxMlPerMin = 68,0` et `kPumpTypeFlowMeasuredMaxMlPerMin = 26,0`, qui n'interviennent dans **aucun calcul** et ne changent que sur une nouvelle mesure d'exemplaire.
- **`GET /data`** : `flow_assist.flow255` sert désormais **74,0 pour les deux pompes**. Le tableau **reste de taille 2** — casser sa forme ferait tomber les clients existants sur leur repli générique.

### Interface

- **Un troisième miroir de constante de sécurité disparaît du navigateur.** `data/app.js` portait `FLOW_255_ML_PER_MIN = {1: 61.0, 2: 67.0}`, copie en dur des constantes firmware servant à annoncer la durée d'une injection manuelle — devenue fausse de **~21 %**. L'interface lit maintenant `flow_assist.flow255` servi par `/data`.
- **Le repli à 61 / 67 est conservé délibérément** : un firmware antérieur à la v2.25.2 ne publie pas ce champ et dose réellement à ces débits-là. L'aligner sur 74 reviendrait à **mentir sur un appareil ancien**.
- **Assistant de mesure du débit** : le coût budget annoncé pour une purge de 60 s passe de ~61-67 à **~74 mL** (le majorant servi a changé, la purge est inchangée).

### Documentation

- **[ADR-0050](docs/adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md) créé** — *Le débit à pleine puissance est une constante unique de flotte, majorée de la dispersion mesurée*. **Amende les valeurs d'usine de l'[ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** (dont le statut est annoté en conséquence ; le reste de l'ADR-0047 — sémantique NVS, drapeaux « mesuré », gel de l'amplitude — est inchangé). Consigne les deux arbitrages de valeur (74,0 contre 70,0 ; 28,0 contre 26,0), le **refus du dédoublement** de la constante, la règle « une marge de flotte doit dépasser la dispersion observée », l'interdiction d'indexer une constante de sécurité sur une pompe physique, et l'interdiction de recopier une valeur de sécurité dans le navigateur.
- `docs/subsystems/pump-controller.md` : nouvelle section « Débit à 255 : constante unique de flotte », table des trois usages du majorant, effet chiffré sur les compteurs, tables de constantes et de calibration de type mises à jour.
- `docs/API.md` : `flow_assist.flow255` (valeur unique de flotte, forme du tableau conservée, règle de consommation client), comptabilité du mode manuel et durée de l'injection volumée sur le chemin dégradé.
- `docs/features/page-ph.md` / `page-orp.md` : origine du débit utilisé pour la durée estimée « ≈ X min », repli historique assumé.
- `docs/features/page-settings.md` : coût budget annoncé par l'assistant de mesure.
- **Deux dettes consignées, non corrigées** : `phPumpControl` / `orpPumpControl` ([`src/config.cpp`](src/config.cpp)) sont **entièrement mortes** (déclarées, initialisées, lues nulle part dans `src/` ni `test/`) ; `MAX_INJECT_DURATION_S = 600` dans [`data/app.js`](data/app.js) est un **quatrième miroir de valeur** (copie de `kManualInjectMaxDurationS`), candidat à être servi par `/get-config`.

---

## [2.25.1] - 2026-08-27

> ⚠️ **Correctif d'observabilité — une panne matérielle était affichée comme un état normal.** Le libellé de refus de dosage « filtre capteur non prêt (warmup / EZO injoignable) » mélangeait deux causes physiques opposées dans une seule phrase, que l'interface analysait ensuite par recherche de fragments de texte. Comme « warm » était testé avant « ezo », **une vraie panne du module EZO s'affichait « Stabilisation du filtre »** — côté pH comme côté ORP. La régulation était à l'arrêt et l'appareil annonçait une attente banale. Mise à jour recommandée pour tous les appareils.
>
> Le défaut symétrique, celui qui a été rapporté, était moins grave mais visible à chaque démarrage : pendant environ six minutes, l'interface affichait **« EZO indisponible »** sur une sonde parfaitement saine (le mot « indispo » du libellé de préchauffage correspondait à la règle de l'UI).

### Fonctionnalités

- **Une panne de sonde n'est plus présentée comme une stabilisation.** L'interface distingue désormais visuellement les **pannes** (« Sonde figée », « Sonde muette » — rouge) des **états d'attente normaux** (« Stabilisation… », « Mesures non représentatives » — ambré). La catégorie est donnée par le firmware, elle n'est plus devinée à partir du texte affiché.
- **Plus de « EZO indisponible » au démarrage sur une sonde saine.** Pendant le délai de stabilisation puis le remplissage de la fenêtre de lissage, l'interface annonce ce qui se passe réellement : une stabilisation en cours.
- **Une lecture manquante depuis moins d'une minute devient « Lecture EZO manquante »** (avertissement) au lieu de « EZO indisponible » (panne) : un blip du bus I²C n'est pas une panne établie.
- **Deux messages contradictoires ne s'affichent plus côte à côte.** Sur le tableau de bord, la ligne d'état de dosage se **tait** quand le badge capteur annonce déjà « Stabilisation : ~N min ». Elle reste bavarde pour tout autre motif, et **toujours** en cas de panne.
- **Une panne de mesure persistante déclenche enfin une alerte.** Sonde figée ou muette pendant plus d'une minute → alerte Home Assistant / MQTT (`ph_dose_sensor_fault` / `orp_dose_sensor_fault`). Jusqu'ici, l'arrêt de régulation pour ce motif n'était visible que dans l'interface web.
- **L'interface avoue son ignorance plutôt que de se tromper.** Face à un firmware plus récent qu'elle, elle affiche « Cause inconnue (code N) » au lieu du libellé le plus proche.

### Firmware

- **`GET /data` et la trame WebSocket `sensor_data` : 2 champs ajoutés** — `phDoseBlockedCode` / `orpDoseBlockedCode`, **code machine** de la cause de refus, publié **à côté** du texte existant. **Parité stricte d'emblée** (mêmes noms, mêmes valeurs, mêmes règles `null` : `0` ⇒ `null` explicite), dans le même passage de sérialisation des deux canaux — la parité n'est plus rattrapée après coup comme en 2.21.2, 2.23.0 et 2.25.0. Coût : **51 o** au pire cas.
- ⚠️ **`enum class DoseRefusal` devient un CONTRAT DE SÉRIALISATION.** Valeurs **fixées explicitement**, **jamais renumérotées**, **jamais insérées au milieu** — toute cause nouvelle s'ajoute **à la fin**, une cause retirée reste **réservée**. **23 `static_assert`** verrouillent la correspondance valeur par valeur, plus un `static_assert` de non-collision avec la sentinelle. Un décalage d'une seule valeur ferait afficher une mauvaise cause, soit précisément la classe de défaut corrigée ici.
- **8 codes ajoutés en fin d'énum** : `15`-`18` `ReadingNaN{Warmup,Frozen,Stale,GateClosed}` (qualifient la garde « lecture filtrée NaN »), `19`-`22` `Filter{Warmup,Frozen,Stale,GateClosed}` (qualifient la garde « filtre non prêt »). Sentinelles hors énum : `kDoseBlockedCodeNone` = `0` (aucun refus), `kDoseBlockedCodeGuard` = `250` (cause de garde de sécurité, message à valeurs runtime non énumérable → le client lit le texte).
- **Plus aucun libellé firmware ne mélange deux causes.** Les deux causes génériques historiques (`3` et `4`) subsistent et disent explicitement « état du filtre indéterminé » : le pire cas est une cause **imprécise**, jamais une panne maquillée en état normal (fail-closed d'observabilité).
- **Les quatre raisons de `SensorFilter::ready() == false` sont enfin discriminées**, sur l'**état interne du filtre** : `warmingUp()`, `frozen()`, `stale()`, `gateClosed()` — quatre prédicats `const` composés par la fonction **pure** `resolveFilterState()` ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)), **unique lieu de décision de priorité** : `frozen → gateClosed → stale → warmup → ready`, **pannes d'abord**. `gateClosed` passe **avant** `stale` : porte fermée, l'âge croît par conception, l'annoncer « périmé » accuserait la sonde d'un blocage voulu par le système.
- ⚠️ **Un premier discriminant fondé sur la fraîcheur de la lecture BRUTE a été refusé par le gate `pool-chemistry`.** Le cache brut est alimenté par **toute** lecture, y compris hors plage et y compris une valeur constante : **une sonde figée répond, donc paraît saine**. Il aurait classé « warmup » un EZO figé ou débitant du garbage — la panne silencieuse aurait été réintroduite sous un nom rassurant, par le correctif censé la supprimer.
- **Le verdict de dosage est inchangé au bit près.** `DoseInputs::filterState` ne sert qu'à **qualifier** la cause : aucune condition ajoutée à `evaluateDose()`, aucun seuil touché, aucune garde déplacée. Initialiseur de membre `Unknown` obligatoire (l'agrégat n'est pas value-initialisé par `DoseInputs in;`).
- **Nouvelle alerte MQTT `ph_dose_sensor_fault` / `orp_dose_sensor_fault`** (`warning`, non-retained) sur sonde **figée** ou **muette**, **une fois par épisode** après `kDoseSensorFaultAlertDelayMs` = **60 s** de panne **continue**. Le délai (12 cycles de lecture EZO) écarte le transitoire de ré-amorçage du filtre à la reprise de circulation — sans lui, chaque démarrage de filtration criait « sonde muette », un cri au loup quotidien. L'**affichage**, lui, reste immédiat.
- **L'épisode d'alerte n'est plus redaté quand la panne change de nature** (correctif de revue) : un EZO agonisant alternant `FilterFrozen` ↔ `FilterStale` par phases plus courtes que la temporisation repoussait indéfiniment la date de début et réarmait le droit d'alerter — **l'alerte ne partait jamais** alors que la régulation était à l'arrêt. Le correctif anti-panne-silencieuse introduisait une panne silencieuse.
- **Une seule trace de journal par épisode** : `publishAlert()` journalise déjà le message en `warning`, y compris broker déconnecté. Le `systemLogger.warning()` supplémentaire produisait deux lignes quasi identiques dans une partition `history` de 64 Ko.
- **`SensorFilter::rearm()` arme une grâce d'observabilité sur `stale()`** (correctif de revue) : sans elle, chaque réouverture de porte affichait une puce rouge « Sonde muette » sur une sonde saine, au démarrage de la filtration. La datation n'est **pas** falsifiée (`_hasValid` / `_lastValidMs` ne décrivent que des échantillons réellement acceptés — c'est l'invariant qui rend `ready()` honnête) : on mémorise que l'âge courant est **imputable au ré-amorçage**, pendant `maxAgeMs` (20 s) au plus, levée par la première mesure acceptée. **Seul `stale()` lit ce drapeau** — `ready()`, `unstable()` et tout le chemin du verdict de dosage l'ignorent. **`reset()` n'arme rien** : une sonde muette au démarrage à froid reste annoncée sans délai.
- ⚠️ **Correctif de repliement sur cette même grâce — 3ᵉ défaut de cette famille dans le projet** (après la retentative de `Slope,?` en v2.21.2 et la comptabilité d'impulsions `tickPulse`). La première écriture n'évaluait l'échéance que dans `stale()`, qui est `const` : sur une sonde restée muette le drapeau ne redescendait **jamais**, et la comparaison de deadline `(int32_t)(now − until) < 0` **se retournait 2³¹ ms plus tard** — une panne de sonde aurait été **masquée à nouveau pendant 24,8 jours**, l'appareil réaffichant « en stabilisation » sur une sonde morte. Atteignable, `millis()` se repliant à 49,7 j sur un appareil qui tourne en continu. Correction **redondante par conception** : (1) le prédicat devient un **intervalle fermé** depuis l'instant d'armement (`now − from < durée`, non signé) au lieu d'une demi-droite vers une échéance — il ne peut plus se retourner, indépendamment de tout désarmement ; (2) un **désarmement effectif** (`_expireStaleGrace()`) en tête de `setContext()` et de `addSample()`, le premier étant atteint toutes les 5 s **inconditionnellement**, y compris bus I²C mort. Les deux protections sont **testées séparément** : une mutation supprimant l'une laisse survivre les tests de l'autre.
- **Règle générale consignée pour tout code manipulant `millis()`** : mémoriser l'**instant d'armement** et tester la **durée écoulée** (`now − from >= durée`, non signé) — jamais une date de fin, jamais une comparaison signée à une échéance, jamais une expiration confiée au seul getteur `const` qui la lit.
- **Le code de cause est écrit AVANT le texte** à chaque pose (`logRefusalOnce()`, `setGuardRefusalCause()`) : un lecteur asynchrone voit au pire le nouveau code avec l'ancien texte, et le code fait foi. `resetRefusalLogState()` / `clearGuardRefusalCause()` remettent les deux à zéro ensemble.
- **Accesseurs ajoutés** : `PumpController.getPhDoseBlockedCode()` / `getOrpDoseBlockedCode()` (`uint8_t`, aucune allocation), `SensorManager.getPhFilterState()` / `getOrpFilterState()` (lock-free, même contrat de concurrence que `is*FilterReady()`), `SensorFilter::warmingUp()` / `stale(nowMs)`.

### Interface

- **Table `DOSE_BLOCK_CODES`** (`data/app.js`) : mapping code → libellé, ton (`normal` / `fault` / `unknown`), libellé court de chip et appartenance à la famille « stabilisation des mesures ». **Plus aucun `includes()`** : `_doseBlockReasonFr()` est supprimée.
- **Trois chemins strictement distincts** (clé absente ≠ `null` ≠ `0`) : code `0`/`null` ⇒ dosage autorisé, ligne masquée ; **clé absente** ⇒ information indisponible, **seul** cas de repli sur le texte serveur, affiché tel quel sans en déduire de catégorie ; **code inconnu** ⇒ « Cause inconnue (code N) », jamais un libellé voisin — un frontend plus ancien que le firmware doit produire un doute visible.
- **Le code de refus est consulté avant le test « brut invalide »** dans `_classifyFilterState()`. Ce test condamnait l'EZO sans savoir pourquoi, ce qui produisait le « EZO indisponible » du démarrage.
- **Ton d'alerte réservé aux pannes** dans le modal « État filtre » : un préchauffage ou une porte de validité fermée restent en ton neutre.

### Documentation

- **[ADR-0049](docs/adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) créé** — *La cause de refus de dosage est transportée par un code machine, et discriminée sur l'état interne du filtre*. Consigne le contrat de sérialisation (pourquoi un code plutôt qu'un texte à parser, pourquoi un entier plutôt qu'une chaîne d'identifiant, pourquoi les valeurs ne doivent jamais être renumérotées), le **refus du discriminant fondé sur la lecture brute** (une sonde figée répond), la priorité pannes-d'abord, l'ordre `gateClosed` avant `stale`, et l'**étanchéité observabilité / décision** (la grâce est lue par `stale()` mais pas par `ready()`). Consigne également, section « Pourquoi la forme du prédicat compte », la **règle de forme des fenêtres de temps** (intervalle fermé depuis l'instant d'armement, jamais de deadline) et ce qu'elle verrouille pour tout module futur. **Douze alternatives écartées** documentées, dont trois erreurs commises puis corrigées en revue (épisode d'alerte redaté, absence de grâce de ré-amorçage, grâce se réarmant seule au repliement).
- `docs/API.md` : 2 champs `/data`, nouvelle section « Code de cause de refus de dosage » (table des 23 valeurs + sentinelle 250, contrat de sérialisation, règles de consommation client), exemple de libellé périmé corrigé, note de parité étendue à six champs.
- `docs/subsystems/ws-manager.md` : section « Code de cause de refus — parité d'emblée », budget de payload, ordre d'écriture code/texte, justification du choix entier plutôt que chaîne.
- `docs/MQTT.md` : alertes `ph_dose_sensor_fault` / `orp_dose_sensor_fault` documentées (déclencheurs, temporisation de 60 s, règle d'épisode, complémentarité avec `ph_filter_not_ready`).
- `docs/subsystems/pump-controller.md` : table des causes de refus réécrite avec les codes, contrat de sérialisation, section « Qualification par l'état interne du filtre », section « Alerte de panne de mesure » (dont la **sémantique exacte de l'épisode** : table des transitions, seule la transition non-panne → panne date l'épisode), ordre d'écriture code/texte, 4 constantes ajoutées à la table, API publique et gardes #4 / #4b mises à jour.
- `docs/subsystems/sensors.md` : section « Prédicats d'état du filtre (observabilité) » — les 4 primitifs, la composition, l'interdiction de reconstruire depuis `get*Raw()`, la grâce de ré-amorçage et son étanchéité, ses **deux protections cumulatives contre le repliement de `millis()`** et les tests qui les couvrent séparément ; table `rearm()` vs `reset()` et table des méthodes de `SensorFilter` complétées.
- `docs/features/page-ph.md` et `page-orp.md` : ordre de classification du chip d'état filtre réécrit, nouveau libellé « Lecture EZO manquante », trois chemins de traduction du code dans le modal.
- `docs/features/page-dashboard.md` : dé-duplication de la ligne d'état de dosage avec le badge capteur, limitée à la famille « stabilisation des mesures ».
- `docs/subsystems/logger.md` : effet de la déconflation des libellés sur le budget du journal (jusqu'à 3 lignes de refus au démarrage au lieu d'1), nouvel émetteur `warning` d'alerte de panne de sonde et retrait en revue du doublon de journalisation.
- `docs/UPDATE_GUIDE.md` : note de migration v2.25.1 — aucune action requise, deux types d'alerte MQTT ajoutés, changement d'affichage, firmware et filesystem à mettre à jour ensemble.
- **Deux specs de suivi créées** : [feature-076](specs/features/todo/feature-076-detection-produit-mal-declare.md) (détection d'un produit de correction mal déclaré, issue de la contre-visite `pool-chemistry`) et [feature-077](specs/features/todo/feature-077-extraction-machine-episode-panne-sonde.md) (extraction de la machine d'épisode de panne sonde en module pur testable — elle vit aujourd'hui dans la coquille, donc hors couverture native).

---

## [2.25.0] - 2026-08-27

> ⚠️ **Correctif critique de sécurité chimique et d'authentification.** Mise à jour recommandée pour tous les appareils, en particulier ceux utilisant un offset pH non nul.
>
> ⚠️ **Deux réglages changent de plage.** La **consigne pH maximale passe de 7,60 à 7,50** : une installation réglée à 7,60 est corrigée automatiquement au premier démarrage, et signalée dans le journal. Le **refus d'alcaliniser passe de 7,70 à 7,60**. Motif commun : au-dessus de 7,60 la fraction de chlore actif tombe sous 25 %, et une consigne à 7,60 pile aurait inhibé le chlore en permanence — une panne sanitaire silencieuse.
>
> ⚠️ **Deux alertes MQTT sont renommées** (`ph_raw_floor` → `ph_guard`, `orp_ph_raw_inhibit` → `orp_ph_inhibit`) et une est supprimée (`ph_target_unreachable`). Une automation Home Assistant filtrant sur les anciens noms doit être mise à jour.

### Fonctionnalités

- **Les seuils de sécurité du dosage pH sont enfin des valeurs fixes, lisibles et vérifiables.** Le plancher variable qui s'adaptait à l'offset est remplacé par des seuils **fixes** portant sur le **pH affiché** (offset compris) : refus d'acidifier sous **6,70**, refus d'alcaliniser au-dessus de **7,60**. L'ancien mécanisme était juste mais incompréhensible — le seuil annoncé bougeait à chaque changement d'offset — et c'est sa complexité qui avait désactivé la régulation en production le 25/08. Deux butées de dernier recours (6,20 et 8,10) restent sur la mesure brute du capteur, pour le cas où l'offset serait mal réglé.
- **La consigne pH redevient atteignable avec un offset large.** Sur une installation avec offset `+0,50` et consigne `7,20`, la régulation dispose désormais de **0,50 pH de marge** avant tout refus. La situation à l'origine de l'incident du 25/08 ne peut plus se reproduire.
- ⚠️ **La consigne pH est limitée à 7,20 – 7,50** (au lieu de 7,20 – 7,60), par pas de 0,10. En dessous, l'eau devient agressive pour le liner, les joints et les parties métalliques, elle est moins confortable et elle consomme le TAC ; **à 7,60 pile**, la consigne atteindrait le seuil d'inhibition du chlore et la désinfection serait suspendue **en permanence** dès que la régulation tient sa consigne. Une consigne à 7,60 enregistrée par une version antérieure est **corrigée au démarrage** et signalée.
- **Le dosage de pH+ est encadré pour la première fois.** Jusqu'ici aucun seuil ne le limitait : rien n'empêchait l'appareil d'alcaliniser l'eau jusque dans la zone où il inhibe lui-même le chlore.
- **Les réglages pH ne peuvent plus être saisis hors plage.** La consigne devient un **sélecteur à 4 positions** (7,20 / 7,30 / 7,40 / 7,50) et l'offset un **incrémenteur** `−` / valeur / `+` (pas de 0,10, boutons désactivés aux bornes). Plus de champ libre, plus de curseur : un curseur donnait 25 px par cran sur mobile pour l'offset, le réglage le plus sensible de la page.
- **Un nouvel avertissement d'eau acide à pH 7,00**, non bloquant : il prévient bien avant que le dosage ne soit refusé (6,70), sans rien arrêter.
- **Les alertes de sécurité partent enfin quand elles sont le plus nécessaires.** Elles étaient muettes quota journalier épuisé et **en mode manuel** — c'est-à-dire dans l'état exact où l'appareil se met après une bascule de sécurité. La surveillance est désormais indépendante de la décision de doser.
- **L'appareil prévient avant une injection manuelle qui contourne une sécurité.** Quand une butée sur la mesure brute est active, un encart explique la situation au-dessus du bouton et une confirmation est demandée. L'injection manuelle reste possible — c'est le dernier recours de l'opérateur — mais elle n'est plus silencieuse.
- **Le message d'alerte n'accuse plus la sonde à tort.** Quand le diagnostic électrochimique de la sonde EZO est normal, l'alerte indique explicitement que **la sonde n'est pas en cause** et oriente vers un décalage de mesure d'origine externe (courants de fuite de la pompe de filtration, équipotentialité du bassin).
- **Une sécurité ne peut plus être annulée par accident.** Quand l'appareil bascule la régulation pH en manuel par sécurité, la simple consultation de l'application pouvait réactiver la régulation et effacer la trace de l'incident. Il faut désormais **acquitter explicitement** l'alerte avec le bouton « Acquitter l'alerte » de la bannière, sur la page pH. L'acquittement **n'enclenche aucun dosage**. Le refus s'applique aussi aux commandes venues de Home Assistant.
- **Un dosage programmé refusé ne ferme plus la régulation.** Seuls les refus survenus en mode automatique comptent vers la bascule de sécurité ; le mode programmé signale son refus (au plus une fois par heure) sans escalader.

### Firmware

- **Sécurité chimique — seuils FIXES dérivés de la plage de consigne** (gate `pool-chemistry` : GO amont + GO en contre-visite ; `code-reviewer` Approved en 2ᵉ passe). Une **source unique** (`kPhTargetMin` / `kPhTargetMax` + trois marges) dont **tous** les seuils dérivent en `constexpr` — aucun littéral, sauf les deux à justification chimique propre (`kPhOrpInhibit`, `kPhLowWarn`) :

  | Seuil | Valeur | Référentiel |
  |---|---:|---|
  | Consigne | `7,20` → `7,50`, pas `0,10` | corrigé |
  | `kPhFloorCorrected` (refus d'acidifier) | `6,70` | corrigé |
  | `kPhCeilingCorrected` (refus d'alcaliniser) | `7,60` | corrigé |
  | `kPhOrpInhibit` (inhibition ORP) | `7,60` (relâche ≈ 7,55) | **corrigé** (était brut) |
  | `kPhLowWarn` (avertissement non bloquant) | `7,00` | corrigé |
  | `kPhFloorRawHard` (butée basse) | `6,20` | brut |
  | `kPhCeilingRawHard` (butée haute) | `8,10` | brut |
  | `kPhGuardReleaseHysteresis` | `0,05` sur **toutes** les gardes | — |

- **Chaque butée brute est à exactement l'offset maximal de son seuil corrigé** : à l'offset extrême, butée et seuil mordent au même point et se relâchent ensemble — aucune ne peut rendre l'autre inopérante. Une butée à **6,40** avait été proposée par le gate : refusée sur objection de l'utilisateur, car avec un offset légitime de `+0,50` elle équivaut à 6,90 corrigé et mordrait **avant** le seuil de 6,70, rendant l'offset inopérant.
- **Trois `static_assert` verrouillent les invariants chimiques** : (i) la consigne haute ne peut pas atteindre le seuil d'inhibition ORP, (ii) le plafond d'alcalinisation ne peut pas le dépasser, (iii) chaque butée brute ne peut pas mordre avant son seuil corrigé. Les `static_assert` (iii) sont écrits en **inégalité** : remonter une butée ne compile plus, l'abaisser reste possible.
- **`kPhHighMargin` abaissée de 0,20 à 0,10** : à 0,20 le plafond tombait à 7,70, soit **au-dessus** de `kPhOrpInhibit` — l'appareil s'autorisait à alcaliniser une eau dont il venait de décider que le chlore y était inactif. Aucun argument physique ne soutient cette zone.
- **`kPhTargetMax` abaissée de 7,60 à 7,50**, avec `kPhOrpInhibitMargin` = 0,10 comme garde-fou **structurel** (n'entre dans aucun calcul de dosage, n'existe que pour le `static_assert`).
- **SÉPARATION SURVEILLANCE / DÉCISION** (contre-visite) : `tickPhGuardSurveillance()` et `tickOrpPhInhibitSurveillance()` sont appelées **inconditionnellement**, une fois par tick, hors de **toute** conjonction d'éligibilité. La détection vivait auparavant dans une conjonction `&&` — le court-circuit la rendait muette dès qu'un terme amont était faux (quota épuisé, mode `manual`, régulation désactivée), c'est-à-dire précisément dans l'état où l'escalade laisse l'appareil. Les surveillances ne comptent rien et n'escaladent jamais ; elles ne lisent que des caches capteur (aucune transaction I²C supplémentaire).
- **Inhibition ORP sur le pH CORRIGÉ** (était le brut) : la fraction de HClO dépend du pH réel. Cela supprime la limitation connue de la première mouture (avec un offset négatif légitime, l'inhibition devenait **permanente**). Une butée brute haute (8,10) s'y ajoute en OU logique.
- **Deux correctifs de revue sur l'inhibition ORP.** **R1** — découplage de la mémoire d'hystérésis et du latch d'inhibition : les branches NaN armaient le latch pour des motifs étrangers à tout seuil, et l'hystérésis, croyant sortir d'un blocage, **maintenait l'inhibition du chlore indéfiniment** à chaque démarrage de filtration (cas observé : pH revenu à 7,57 sans jamais avoir dépassé 7,60). **R2** — une mémoire par seuil : les deux seuils partageaient un drapeau, le moins souvent armé perdait toute hystérésis ; un bassin à 8,03-8,12 avec offset 0,00 produisait un `critical` + une alerte MQTT par oscillation, avec deux messages contradictoires en alternance. Anti-rafale ajouté : **une alerte par gravité et par épisode**, jamais de réémission en dégradation.
- **`ph_plus` gardé** : refus d'alcaliniser au-dessus de 7,60 corrigé + butée brute 8,10. Sans la butée, un offset `−0,50` erroné laissait le pH+ pousser l'eau au-delà de 8,10 brut pendant que le chlore était inhibé — l'appareil conduisait lui-même l'eau dans l'impasse sanitaire.
- **Compteur des 3 refus / 24 h limité au mode `automatic`** (condition du gate). Le mode programmé refuse tout autant mais **signale** à la place : `warning` + alerte MQTT `ph_guard_scheduled` à débit limité (`kPhGuardScheduledAlertIntervalMs` = 1 h) — sans quoi un refus permanent y serait un no-op silencieux après la première transition.
- **Politique NaN uniforme dans les gardes** : une entrée NaN ne produit **jamais** de refus (les gardes NaN existantes couvrent déjà le cas ; fabriquer un refus ajouterait un refus **fantôme** au compteur, jusqu'à fermer la régulation sur une donnée qui n'existe pas). Côté pH, les mémoires d'hystérésis sont **réarmées** ; côté ORP, **conservées** (asymétrie documentée dans l'ADR).
- **La relâche d'une garde efface la cause de refus exposée** — c'est le seul point du firmware qui tourne à chaque tick quel que soit le mode. En `manual`, `canDose(0)` n'étant jamais appelé, la chaîne restait figée sur un blocage révolu, y compris le message d'escalade, dans le mode où l'UI expose justement l'injection manuelle.
- **Suppressions** : `phRawFloorFor()`, `kPhRealFloorTarget`, `kPhRawFloorAbsolute`, `kPhOffsetWideThreshold`, `checkPhTargetReachable()`, `isPhTargetUnreachable()`, `kPhTargetFloorMarginMin` et l'alerte `ph_target_unreachable`. La marge est devenue **structurelle** (garantie à la compilation) : il n'y a plus rien à détecter à l'exécution.
- **Renommages d'API publique** : `getPhDoseBlockedReason()` / `getOrpDoseBlockedReason()` (`String`) → `copyPhDoseBlockedReason(char*, size_t)` / `copyOrpDoseBlockedReason(...)` — rendre une `String` par valeur allouait sur le tas dans la tâche `async_tcp` à chaque appel. `isPhRawFloorBlocking()` → `isPhGuardBlocking(bool countRefusal)`. `kPhRawFloorRefusal*` → `kPhGuardRefusal*`. `kPhRawOrpInhibit` → `kPhOrpInhibit`.
- **`GET /data` : 4 champs ajoutés** — `phDoseBlockedReason`, `orpDoseBlockedReason`, `phRawGuardBlocked`, `orpPhRawGuardBlocked` — en **parité stricte** avec le WebSocket. Ils n'existaient que sur le canal WS alors que l'UI les consomme depuis les deux sources. **Troisième récidive** de ce motif (pentes de sonde en 2.21.2, calibration de pompe en 2.23.0) : l'exigence de parité est désormais consignée dans un ADR.
- **`GET /get-config` : 6 champs ajoutés** — `ph_target_min/_max/_step`, `ph_offset_min/_max/_step`, dérivés des constantes firmware. Le frontend ne code plus **aucune** borne pH en dur ; élargir la plage se fait côté firmware seul. Asymétrie assumée : ces bornes ne sont **pas** dans la trame `config` du WebSocket (constantes de compilation, budget de trame) — le client ignore en bloc un lot absent ou incohérent et conserve les bornes connues.
- **Alertes MQTT** : `ph_raw_floor` → **`ph_guard`**, `orp_ph_raw_inhibit` → **`orp_ph_inhibit`** (les seuils ne portent plus exclusivement sur le brut, l'ancien nom était faux) ; ajout de **`ph_low_warning`** et **`ph_guard_scheduled`** ; suppression de `ph_target_unreachable`. Auto-discovery HA : `min`/`max`/`step` de la consigne republiés (7,20-7,50, pas 0,10), `unique_id` **inchangé**.
- **Validation serveur conservée en défense en profondeur** malgré des contrôles UI qui rendent la saisie hors plage impossible : `/save-config` refuse en **400** hors bornes et **aligne sur le pas** dans la plage ; MQTT **refuse** + `publishTargetState()` pour resynchroniser HA ; le **chargement NVS clampe** + journalise + réécrit immédiatement. L'offset est arrondi au pas par la fonction pure `roundPhOffsetToStep()`, partagée par la route et par la migration NVS (0,47 → 0,50 ; écart maximal 0,05 pH, soit le bruit résiduel du filtre).
- **Acquittement fail-closed de la bascule de sécurité pH** : tant qu'une trace existe, `/save-config` **refuse en 409** toute sortie du mode manuel (gate placé avant toute mutation) et la commande MQTT équivalente est refusée puis **resynchronise Home Assistant** sur le mode réel, sans jamais effacer la trace. Sortie unique : le champ `ph_auto_manual_ack` (**booléen strict** — ArduinoJson rend `true` pour toute chaîne, un `"false"` aurait acquitté). Envoyé seul, il efface la trace **sans réarmer**.
- **`ph_enabled` n'est plus accepté en écriture** (dérivé pur du mode, toujours exposé en lecture) : il ouvrait un second chemin de réarmement contournant le gate. `collectConfig()` côté client n'envoie plus ni le mode ni `ph_enabled` — le repli en dur sur `"automatic"` depuis un onglet périmé était la cause directe de l'incident.
- **Message d'escalade conditionné au diagnostic EZO** : `isPhProbeHealthy()` (pentes ∈ [90 ; 110] %, \|zéro\| ≤ 15 mV, `NaN` ⇒ non concluant) sélectionne l'un des deux textes. Ces seuils ne gouvernent **aucune** décision de dosage.
- ⚠️ **Correctif d'authentification (trou préexistant sur TOUTE la route `/save-config`)** : le contrôle d'authentification n'était posé que dans le handler de complétion, exécuté **après** l'application du corps. Un `POST` **anonyme** pouvait donc écrire la configuration (RAM **et** NVS) avant que le `401` ne parte. Le contrôle est désormais en tête du handler de corps : un corps non autorisé est ignoré en totalité, IP journalisée. L'exemption « premier démarrage » (assistant) est conservée.
- **`publishAlert()` tronque proprement** : les nouveaux messages dépassaient `kMaxPayloadLen` = 128 o et sortaient en **JSON invalide** (Home Assistant rejetait l'alerte entière). Troncature respectant les frontières UTF-8, suffixe `... (voir journal)` ; texte intégral conservé dans le journal et en NVS.
- **Traçabilité** : l'IP d'origine est journalisée sur les deux issues d'un changement de mode de régulation pH (accepté / refusé 409).
- **Module pur `src/ph_guard_logic.h`** ([ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : deux comparateurs symétriques avec hystérésis (`phGuardBlocksBelow` / `phGuardBlocksAbove`), `roundPhOffsetToStep()`, `isPhProbeHealthy()`, `phModeChangeNeedsAck()`. Suite native **746/746**, `pio run` **0 warning**, flash 96,5 %.
- **Dette résiduelle tracée** : l'alerte de refus en mode `programmé` vit encore sous une conjonction d'éligibilité — même classe que le défaut corrigé ici, impact faible (la surveillance couvre la détection). Les indicateurs `phRawGuardBlocked` / `orpPhRawGuardBlocked` **gèlent** pendant les sorties anticipées de la boucle (filtration arrêtée) : voulu, la mesure n'est pas représentative en eau stagnante.

### Documentation

- **[ADR-0048](docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) réécrit** — *Seuils de sécurité pH fixes, dérivés de la plage de consigne, appliqués au pH corrigé*. Remplace la version décrivant le plancher continu asservi à l'offset (jamais publiée). Alternatives écartées consignées : (i) **plancher asservi à l'offset** — incompréhensible, seuil affiché mobile, et sa complexité avait désactivé la régulation ; (ii) escalier à deux paliers — plancher réel variable, pire cas juste avant le seuil de bascule ; (iii) **butée brute à 6,40** — mordait avant le seuil corrigé pour un offset légitime de +0,50, rendant l'offset inopérant (objection de l'utilisateur) ; (iv) **consigne max à 7,60** — inhibition permanente du chlore ; (v) **monter l'inhibition à 7,70 pour légaliser 7,60** — desserrer une sécurité pour autoriser un réglage agressif ; (vi) plafond à consigne + 0,20 — dépassait l'inhibition ORP ; (vii) **curseurs `input[type=range]`** — 25 px par cran sur mobile pour l'offset ; (viii) bornes différenciées par produit ; (ix) écrêtage silencieux ; (x) blocage de l'injection manuelle. Documente aussi les deux **asymétries assumées** (politique NaN opposée pH/ORP ; gel des indicateurs pendant les sorties anticipées) et la dette résiduelle du mode programmé.
- `docs/subsystems/pump-controller.md` : section « Gardes de sécurité pH » entièrement refondue — deux référentiels, table des seuils dérivés, démonstration des butées 6,20/8,10, invariants de compilation, table surveillance vs décision, gardes par produit, avertissement 7,00, alertes et escalade, correctifs R1/R2 de l'inhibition ORP, bornes consigne/offset et bornes servies au client, table des constantes réécrite. API publique de l'en-tête mise à jour.
- `docs/API.md` : 4 champs `/data` + exigence de parité `/data` ↔ WS et mention du gel, 6 bornes servies par `/get-config`, bornes de consigne 7,20-7,50 et alignement au pas, arrondi de l'offset au pas de 0,10, suppression de la section « Garde de cohérence consigne pH » et de `ph_target_unreachable`.
- `docs/MQTT.md` : alertes `ph_guard` / `ph_low_warning` / `ph_guard_scheduled` / `orp_ph_inhibit` documentées, note de **rupture** sur les renommages, bornes `ph_target/set` 7,20-7,50, auto-discovery `min`/`max`/`step`, clamp NVS d'une consigne héritée à 7,60.
- `docs/features/page-ph.md` : contrôle segmenté de la consigne, incrémenteur d'offset, retrait du miroir client de `phRawFloorFor()`, paliers d'avertissement reformulés, règles firmware refondues, encart + confirmation avant injection manuelle sous butée.
- `docs/features/page-orp.md` : encart + confirmation avant injection manuelle sous butée pH brute haute.
- `docs/subsystems/ws-manager.md` : section parité `/data` ↔ `sensor_data` des indicateurs de garde, budget de payload, règle « clé absente ≠ `false` », asymétrie assumée des bornes non transportées par la trame `config`.

---

## [2.24.0] - 2026-08-24

### Fonctionnalités

- **Assistant guidé « Mesure du débit » des pompes doseuses** (feature-075 ; gate `pool-chemistry` : GO amont sous conditions + GO en contre-visite). Le formulaire nu de la feature-074 est remplacé par un assistant pas à pas en **6 étapes** (Préparation → Purge à l'eau claire → Débranchement → Mesure → Volume recueilli → Remontage), plein écran dans Réglages → Avancé (pattern de la calibration pH) : bouton « Mesurer le débit » par pompe, purge minutée (60 s, 3 max), marche de mesure à l'amplitude exacte d'exploitation (120 s), conversion volume lu → ml/min avec avertissement de vraisemblance (20-35 ml/min attendus), enregistrement par le chemin de la feature-074 (drapeau « mesuré » posé). Le coût budget de sécurité est annoncé dès l'étape 1 (chaque marche est créditée au débit majorant).
- **Le mot « pesée » disparaît de l'interface** (grep `data/` vide) : « mesure du débit » partout, l'indication de provenance « pesé » devient « mesuré ». La saisie directe experte subsiste sous un dépliant discret « Saisie avancée ».
- **Les pompes sont toujours arrêtées côté serveur** : purge et mesure sont minutées par le firmware (duty et durée jamais acceptés du client) — onglet fermé ou WiFi coupé, la pompe s'arrête seule à l'échéance. L'UI signale la perte de connexion et le bouton « Arrêter » reste toujours actif.
- **Le dosage automatique est suspendu pendant l'assistant et rétabli automatiquement** à la sortie — ou par un timeout serveur de 15 min sans interaction (un assistant abandonné ne laisse jamais la régulation suspendue). Une mesure interrompue par le contrôleur (perte de filtration) n'est **jamais enregistrable** (drapeau `aborted` — un débit partiel sous-estimé provoquerait une sur-injection systématique).

### Firmware

- **3 nouvelles routes** `POST /flow-assist/start|stop|exit` (auth WRITE, params `pump`+`phase` seuls) : session « une pompe à la fois », **toutes** les gardes d'injection manuelle appliquées au volume majorant (`flow255 × durée`), refus 409 structurés (`assist_busy`, `purge_limit`, `already_injecting`, `invalid_amplitude` + codes des gardes manuelles). Véhicule d'exécution : le chemin `manualInject*` existant, aucune nouvelle mécanique de pompe.
- **Sécurité chimique (P4)** : marche d'assistant interrompue (arrêt utilisateur, sortie, timeout, perte de filtration) → volume **crédité prorata au majorant** au registre journalier (sous suspension, l'intégration par tick est sautée — sans ce crédit, une pompe alimentée serait comptée zéro). Les injections manuelles classiques (`/ph|orp/inject/start`) sont **refusées (409 `assist_active`)** pendant la session (contre-visite gate). Anti-course `exit`/réconciliation : session fermée **avant** la levée de la suspension + re-pose fail-closed au tick.
- **`GET /data`** : nouvel objet `flow_assist` (active, pump, phase, remaining_s, aborted, purges_left, purge_s, measure_s, flow255[2]) — resynchronisation du compte à rebours client, aucun nouveau rythme réseau (buffer 1920 → 2304 o).
- **`PumpController`** : `setFlowAssistSuspend()`/`isFlowAssistSuspended()` — fermeture du dosage auto+programmé par la garde structurelle existante (chemin perte-filtration), mode manuel préservé, log one-shot par transition. Constantes `kFlowAssist*` ; logique pure `src/flow_assist_logic.h` (ADR-0017), 25 tests natifs (674/674 au total).

### Documentation

- `docs/API.md` (3 routes `/flow-assist/*`, objet `flow_assist` de `/data`, code 409 `assist_active`), `docs/features/page-settings.md` (déroulé des 6 étapes, cas limites, amendement du formulaire 074), `docs/subsystems/pump-controller.md` (section suspension : timeout, invariant exit, comptabilité P4, constantes), `docs/subsystems/logger.md` (nouveaux émetteurs événementiels) mis à jour. Aucun ADR : véhicule d'exécution imposé par le gate (chemin `manualInject*` existant), assemblage de mécanismes existants — aucun critère de déclenchement rempli.

---

## [2.23.0] - 2026-08-24

### Firmware

- **Un appareil neuf dose immédiatement, sur la valeur usine du modèle de pompe** (feature-074, [ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md) — amende le « pesée obligatoire, pas de valeur provisoire » de l'ADR-0043 ; gate `pool-chemistry` : GO amont sous conditions C1-C5 + GO en contre-visite). Contexte flotte (appareils fournis à des tiers, pompe péristaltique standardisée) : les défauts de calibration passent du fail-closed (0, dosage refusé jusqu'à pesée) à la **calibration de type** — duty 127 (`kPumpTypeCalDuty`, même expression entière que l'amplitude, verrouillé par `static_assert`) et débit 28,0 mL/min (`kPumpTypePulseFlowMlPerMin`, mesures de référence du 23/08, **biais sûr** : surestimer le débit ⇒ sous-dosage + compteurs majorants ; à remplacer par la pesée de type).
- **Sémantique NVS** : clé absente (appareil vierge) → défaut de type ; clé **présente à 0** (effacement explicite ou appareil ≤ 2.22 jamais calibré) → **fail-closed conservé**, jamais de conversion silencieuse. Un appareil provisionné garde ses valeurs intactes au flash.
- **Provenance du débit persistée** : nouveaux drapeaux `pumpN_flow_weighed` (NVS `p1/p2_flow_weighed`, purement informatifs — aucun gate de dosage ne s'y fie) ; la garde de plausibilité NVS rabat aussi `weighed = false` (une pesée corrompue n'en est plus une).
- **Correctif on-target** : la trame `config` WebSocket ne portait pas les champs de calibration — le premier push (≤ 5 s) écrasait l'affichage avec « débit non caractérisé » alors que la calibration existait. Parité stricte `/get-config` ↔ trame WS rétablie (+6 champs, buffer `_buildConfigJson` 2368 → 2560 o) ET côté client, clé absente = pas d'information (early-return — même leçon que la parité `/data` des pentes en 2.21.2).

### Fonctionnalités

- **Le réglage de puissance des pompes disparaît des réglages** : l'amplitude d'impulsion est **figée à 50 %** (valeur usine) — les curseurs « Puissance max » sont retirés, remplacés par une ligne lecture seule « Amplitude d'impulsion : X % — débit Y ml/min (**valeur usine** \| **pesé**) ». Le champ `pumpN_max_duty_pct` reste modifiable **par API seule** (validation [40..100] conservée) et est purgé des payloads de save du frontend — plus jamais d'écrasement par un onglet périmé.
- **Formulaire de pesée simplifié à un champ** : seul le débit mesuré (ml/min) est saisi (vide/0/NaN refusés) — le duty calibré est déduit par le serveur de l'amplitude courante et l'indication passe à « pesé ». La pesée devient **facultative** (raffinement de l'appareil) ; le badge d'alerte « Débit non caractérisé — pesée requise » est restreint au seul état encore fail-closed (calibration effacée / NVS hérité à 0).

### Documentation

- **[ADR-0047](docs/adr/0047-calibration-de-type-par-defaut-et-gel-amplitude.md)** : calibration de type par défaut pour la flotte + gel de l'amplitude — contexte produit, conditions C1-C5 du gate, alternatives écartées (fail-closed en flotte, valeur de type + limites réduites), résidus statués (duty explicite d'un vieux client pose `weighed=true` ; appareil 2.21/2.22 n'ayant jamais sauvegardé sa config = équivalent vierge). Note d'amendement croisée dans l'ADR-0043.
- `docs/API.md` (drapeaux `pumpN_flow_weighed`, duty déduit, résidus rétrocompat `/save-config`), `docs/features/page-settings.md` (curseurs retirés, ligne lecture seule, formulaire à champ unique), `docs/subsystems/pump-controller.md` (section calibration de type, sémantique NVS) mis à jour. Tests natifs : 649/649 sans régression.

---

## [2.22.0] - 2026-08-23

### Firmware

- **Le pH atteint désormais réellement la consigne au lieu de stagner à ~0,26 au-dessus** (feature-073, [ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md) — amende le seuil C de l'ADR-0043 ; gate `pool-chemistry` : GO sous 5 conditions + GO en contre-visite). Le refus des demandes sous le pouls minimal de 5 s créait une **zone morte** : la régulation s'arrêtait dès que `kp × erreur` passait sous ~2,1 mL/min (stagnation constatée en production le 23/08). Sous les faibles demandes, la pompe **tire désormais une impulsion de 5 s toutes les quelques minutes au lieu de ne rien faire** : la demande est cumulée de période en période (accumulateur borné, par produit pH/ORP) et, dès que le cumul vaut le volume **réel kick-inclus** d'une impulsion minimale (~2,3 mL — le nominal aurait sur-livré de +10-12 %), un pouls de 5 s exactement part par le chemin standard, résidu conservé. Exemple : 0,7 mL/min → une impulsion de 5 s toutes les 3-4 min, volume restitué exact à ±5 %.
- **Sécurité inchangée** : le pouls d'accumulateur n'a **aucun passe-droit** — toutes les gardes (présence d'eau, limites journalières/horaires, cycles, calibration) restent évaluées avant chaque tir, le volume est crédité sur les débits pesés, jamais plus d'une impulsion par période ni d'impulsion < 5 s. L'accumulateur est remis à zéro à l'ouverture et à la fermeture de séance, au reboot (non persistant), et sur toute demande disparue (aucune dette sans demande vivante) ; au retour d'une demande ≥ plancher, le cumul est abandonné et le comportement redevient **bit-identique** à la v2.21 (vérifié en natif sur 255 demandes).
- **Journal** : nouveau log `INFO Dosage par accumulation : impulsion minimale de 5 s (X mL cumulés, résidu Y mL)` — un par séance ; l'ancien `INFO … injection différée (seuil C)` est supprimé.
- **Aucun changement d'interface** : l'état existant « Dosage en attente : Ns » couvre déjà l'attente entre deux impulsions d'accumulateur (calendrier ADR-0045 inchangé, zéro modification web).

### Documentation

- **[ADR-0046](docs/adr/0046-accumulateur-demande-sous-pouls-minimal.md)** : accumulateur de demande sous le pouls minimal — contexte terrain, alternatives écartées (conserver le refus, période 120 s refusée par l'utilisateur, kp plus fort, V_pouls nominal, séance par pouls, fusion, persistance), les 5 conditions du gate et F1/F2/F3 motivées. Note d'amendement croisée dans l'ADR-0043.
- `docs/subsystems/pump-controller.md` (section accumulateur : `planPulseWithCarry`/`PulseCarryPlan`, resets, invariants, log) et `docs/subsystems/logger.md` (émetteur seuil C supprimé, nouveau log par séance) mis à jour. 9 tests natifs `test_F073_*` (649/649 au total).

---

## [2.21.2] - 2026-08-23

### Firmware

- **La puce sonde pH n'affiche plus « EZO indisponible » puis « Pente non disponible » à tort pendant ~20 min après un redémarrage** (bug-slope-boot-perdue-et-data-incomplet). Cause racine : la requête `Slope,?` enfilée au boot pouvait échouer **en silence** (statut EZO 255/NoData ou parasite pendant l'init du module — un échec isolé n'était jamais journalisé) et n'était **jamais retentée** avant le cycle 24 h. Le firmware retente désormais automatiquement, borné et sécurisé (visa `pool-chemistry` GO) : 5 tentatives max (borne dure, jamais réarmée — chaque `Slope,?` bloque `loopTask` ~900 ms), 30 s d'intervalle, jamais sans module EZO détecté. Logique pure `src/slope_retry_logic.h` (ADR-0017), 15 tests natifs (640/640 au total). **L'état se résout seul en < 2 min**, sans action manuelle.
- **Journal** : un échec de `Slope,?` alors qu'aucune pente n'a jamais été lue produit un **warning dès la première occurrence** (one-shot) ; chaque retentative est tracée en info (`n/5`) ; warning unique à l'épuisement des 5 tentatives ; les statuts EZO hors protocole (≠ 1/2/254/255) sont désormais journalisés avec leur valeur (avant : totalement muets, diagnostic impossible).

### API

- `GET /data` expose les 4 champs de pente sonde pH `phSlopeAcid` / `phSlopeBase` / `phSlopeZero` / `phSlopeAgeMs` — **parité de transport** avec la trame WS qui les portait déjà depuis feature-024 (mêmes règles null/arrondi, lecture cache, zéro I²C). Un client sans WebSocket voit enfin la santé de sonde. Buffer `/data` : 1792 → 1920 octets.

### Fonctionnalités

- **Libellés transitoires honnêtes sur la puce sonde pH** : « Sonde pH : lecture en cours… » (premières secondes après boot, remplace le trompeur « EZO indisponible ») et « Pente en cours de lecture… » (pente jamais lue, se résout seul en < 2 min) sont des états **neutres** ; « Pente non disponible » (ambré) est réservé à l'erreur réelle — pente lue puis invalidée après échecs répétés. États sonde usée/à remplacer/calibration inchangés.

### Documentation

- `docs/subsystems/sensors.md` (mécanisme de retentative de boot, nouveaux logs, cause racine), `docs/API.md` (4 champs `/data`), `docs/features/page-ph.md` (les 3 états de la puce) mis à jour. Aucun ADR : fix + patterns existants (ADR-0017), aucun critère de déclenchement rempli.

---

## [2.21.1] - 2026-08-23

### Fonctionnalités

- **La carte pH/ORP du tableau de bord ne dit plus « Injection en cours » quand la pompe ne tourne pas** (bug-dosage-fantome, itération 3, défaut n°3, [ADR-0045](docs/adr/0045-etat-impulsion-expose-en-calendrier.md)). Avec le dosage en impulsions (v2.21.0), une séance alterne pouls et repos ; l'UI affichait « Injection en cours » pendant les repos — la conflation qui avait masqué le dosage fantôme d'origine. La carte montre désormais l'**état réel de la pompe** : « **Injection en cours** » (dot animé) uniquement quand la pompe est alimentée ; « **Prochaine impulsion : Ns** » (dot statique, compte à rebours) pendant les silences du train ; « **Dosage en attente : Ns** » quand la période ne contient aucun pouls (demande trop faible) ; « **Injection manuelle : Ns** » affichée distinctement du dosage automatique. Le compte à rebours est animé localement à la seconde et resynchronisé à chaque trame (±1-2 s) ; à l'échéance sans donnée fraîche il passe en « Prochaine impulsion… » — jamais de bascule optimiste vers « pompe qui tourne ».
- **Bouton « Calibrer la sonde » pH harmonisé restrictif** : sa désactivation proactive inclut désormais la séance de dosage automatique (`ph_dosing`), comme l'ORP le faisait déjà (le clic était déjà bloqué ; le bouton reflète maintenant le verrou).

### API

- `GET /data` **et** push WebSocket `sensor_data` : **7 nouveaux champs identiques** de calendrier d'impulsion — `pulse_period_ms` (période du train, 60000 ms), `ph/orp_pulse_session` (séance ouverte, auto ou programmée), `ph/orp_pulse_ms` (durée d'impulsion planifiée, 0 = deadband), `ph/orp_pulse_phase_ms` (position **relative** dans la période, clampée [0..période]). Getters `PumpController` lecture seule (`isPulseSessionActive`, `getPulseMs`, `getPulsePhaseMs`), fail-closed sur index invalide, aucun chemin de dosage touché (visa `pool-chemistry` : GO). Buffers : `/data` 1536 → 1792 o, pool WS 2176 → 2432 o, `kWsFrameBufBytes` 3072 → **3584** o (trame mesurée ~2,2 Ko). Rétrocompat : champs absents → comportement historique de l'UI.

### Documentation

- **[ADR-0045](docs/adr/0045-etat-impulsion-expose-en-calendrier.md)** : état d'impulsion exposé en **calendrier** (période/pouls/phase) plutôt qu'en booléen instantané (aliasing à 5 s de push pour des impulsions de 5-60 s) ou en `/data` seul (30 s trop lent) ; animation client resynchronisée ; agrandissement contractuel du buffer WS.
- `docs/subsystems/ws-manager.md` (contrat des 7 champs + buffer 3584), `docs/API.md`, `docs/features/page-dashboard.md` (tableau des 6 états, sémantique du dot, honnêteté du compte à rebours), `docs/features/page-ph.md` (verrou Calibrer harmonisé), `docs/subsystems/pump-controller.md` (les 3 getters) mis à jour.

---

## [2.21.0] - 2026-08-23

> ⚠️ **ACTION REQUISE APRÈS MISE À JOUR — le dosage automatique et programmé est BLOQUÉ tant que la pesée des pompes n'est pas saisie.** C'est **voulu** (fail-closed, [ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)) : l'ancien modèle débit↔puissance était faux (prouvé au banc : ×1,9 de sous-estimation en bas, ×1,5 de surestimation en haut) et créditait de l'acide qui ne sortait pas du bidon. Aucune valeur héritée n'est convertie. Procédure par pompe : **peser le volume délivré en 120 s au duty d'exploitation** (affiché à côté du réglage « Puissance max », eau claire, balance au gramme), puis saisir duty + débit dans **Réglages → Avancé → Configuration des pompes → « Enregistrer la pesée »**. Tant que ce n'est pas fait : badge « Débit non caractérisé — pesée requise » + log `CRITICAL` throttlé, et les injections volumées tournent à pleine puissance (255, seul autre débit mesuré).

### Firmware

- **Régulation pH/ORP — dosage en proportionnel de temps** (bug-dosage-fantome, itération 2, défaut n°1 ; option (B)+(C) du gate `pool-chemistry`, GO en contre-visite) : la pompe ne tourne plus à un duty interpolé mais par **impulsions** — période fixe 60 s, pouls minimal 5 s, **kick de démarrage 400 ms à pleine puissance** (couple de décollage à froid) puis plateau à l'**amplitude** (= puissance max × 255/100). Le débit moyen demandé par le régulateur est réalisé par la fraction de temps de marche. Module pur `src/pulse_dosing_logic.{h,cpp}` (ADR-0017), **625 tests natifs verts**. **Conséquence terrain : le pH descendra réellement désormais** — l'ancien régime commandait une puissance à laquelle la pompe ne tournait que par intermittence.
- **Calibration par pompe, fail-closed** : nouvelles clés `pumpN_calibrated_duty` (0 = non caractérisé) + `pumpN_pulse_flow_ml_per_min` (NVS `p1_cal_duty`/`p1_pulse_flow`/`p2_cal_duty`/`p2_pulse_flow`) ; ancienne clé partagée `pump_max_flow` **supprimée sans conversion**. Amplitude ≠ duty calibré ou pompe jamais pesée → dosage auto/programmé **refusé**. **Aucune interpolation nulle part** (`dutyToFlow`/`flowToDuty` et `kPumpMin/MaxFlowMlPerMin` supprimés). Calibration **par pompe** : +10 % d'écart mesuré entre deux pompes identiques (le tube commande).
- **Comptabilité du volume sur débits mesurés** : pouls = 0,4 s × débit-255 pesé + reste × débit d'amplitude pesé (`pulseDeltaVolumeMl`, exact au jitter près — limite connue : sous-comptage ~0,1 mL/pouls à la transition kick→plateau) ; marche manuelle `?duty=N` créditée au **majorant** débit-255 sauf duty == calibré. Constantes de banc (2026-08-23) : Dmin = 82 → `MIN_ACTIVE_DUTY` 80 → **98** (marge P2) ; P1 61,0 / P2 67,0 mL/min à 255.
- **Limite horaire et temps minimum d'injection comptés en temps POMPE-ON** ([ADR-0044](docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md), validé gate) : `usedMs` et la garde des 30 s accumulent sur les impulsions (« minutes où le produit coule »), plus sur la session. Le cycle (`cyclesToday`, anti-rafale) reste compté **par session** ; la pause mélange s'arme en fin de session, y compris fin par deadband.
- **Seuil C (deadband de refus)** : une demande exigeant un pouls < 5 s est refusée (log `INFO` throttlé 30 min) au lieu d'être réalisée par un pouls au débit non reproductible ; ne nourrit pas le WARN « régulation sans effet ».
- **Injections volumées** (`/ph|orp/inject/start`) : duty **calibré** si l'amplitude y correspond, sinon **255 forcé** (durée exacte volume/débit pesé) ; état incohérent → **409 `flow_uncharacterized`**. Plus jamais de durée déduite du modèle linéaire.

### API

- `POST /save-config` accepte les 4 clés de calibration (validation stricte, **400 explicite** — patron `ph_offset`, jamais d'écrêtage d'une pesée) ; `pump_max_flow_ml_per_min` entrant **ignoré** (rétrocompat) ; `pumpN_max_duty_pct` clampé **[40..100]** (plancher de rotation).
- `GET /get-config` expose les 4 clés ; `GET /data` expose `ph/orp_pump_flow_uncharacterized` + `ph/orp_pump_amplitude_duty`.

### Fonctionnalités

- **Réglages → Avancé** : formulaire de pesée par pompe (duty calibré + débit mesuré, « Pesée en vigueur »), badge « Débit non caractérisé — pesée requise », duty d'exploitation affiché à côté du pourcentage, slider « Puissance max » borné à **40 % minimum** (il ne descend plus en dessous — sous ce seuil la pompe cale). Champ « Débit nominal des pompes » supprimé.
- **Recommandation de validation (P3 du gate)** : abaisser les limites pendant **3 jours** (`max_ph/orp_ml_per_day` → 300 mL, limites horaires → 6/12 min, cycles/jour → 12), comparer le compteur au niveau réel des bidons, puis restaurer.

### Documentation

- **[ADR-0043](docs/adr/0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** : arbitrage complet du modèle de commande (options A/B/C, NO GO initial sur A, re-arbitrage post-mesure — pourquoi (A)+(C) est une régulation morte —, amendement amplitude + kick, fail-closed sans interpolation).
- **[ADR-0044](docs/adr/0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)** : bascule du budget horaire et du temps min en temps pompe-ON (changement de sens d'un invariant préexistant, ADR-0020).
- `docs/subsystems/pump-controller.md` : section « Dosage en proportionnel de temps » (machine à états, gate, comptabilité, getters), purge du modèle linéaire ; `docs/API.md`, `docs/features/page-settings.md`, `docs/features/page-dosages.md`, `docs/subsystems/logger.md` (nouveaux logs throttlés) mis à jour.

---

## [2.20.8] - 2026-08-23

### Fonctionnalités

- **On voit désormais la trajectoire de la grandeur qui décide de l'inhibition du dosage.** Dans la carte « Signaux capteurs » (Réglages → Avancé), un sous-graphe sous chaque graphe brut/lissé (pH puis ORP) trace l'**évolution de la dispersion** (fenêtre glissante de 13 mesures, celle du drapeau `unstable`) sur toute la trace, en **valeur absolue** (pH / mV — pas de % du seuil, décision utilisateur). Courbe ambre à points visibles, identité visuelle propre, sur le **même axe X** que les graphes du dessus : un transitoire visible sur le brut se lit à la verticale sur la dispersion.
- **La marge au seuil se lit graphiquement** : ligne de seuil en pointillé rouge (0,5 pH / 100 mV, légende « Seuil 0,50 » / « Seuil 100 mV »), et l'échelle Y inclut **toujours** le seuil — une courbe très basse montre la marge, jamais un zoom sur le bruit. Le seuil vient de `dispersion_max` (`GET /debug/sensor_filter_state`) si la route répond, sinon d'un repli local : la courbe s'affiche même route en panne (indépendance des deux chargements conservée, invariant 2.20.7).
- **Les transitoires rejetés se voient : ce sont les trous.** Une fenêtre écartée (saut ≥ 1,0 pH / 300 mV, ou trou de mesure) laisse un **trou** dans la courbe, sans interpolation — la note de la carte le précise : *« Un trou dans la courbe de dispersion signale une fenêtre écartée (transitoire rejeté), pas une absence de mesure. »* Moins de 13 points de trace → bloc masqué, pas d'erreur. Statut « indicatif » hérité de la reconstruction 2.20.7, porté par la légende « Dispersion pH (indicatif) ».

### Documentation

- `docs/features/page-settings.md` : carte « Signaux capteurs » — nouvelle sous-section « Courbes d'évolution de la dispersion » (lecture du graphe, sémantique des trous, échelle incluant le seuil, source du seuil route/repli, statut indicatif).
- Pas de nouvel ADR : décision « valeur absolue, pas de % » = décision utilisateur tracée dans la spec ; extension visible et réversible de `createTraceChart` ; arbitrage de placement purement visuel documenté dans page-settings.

---

## [2.20.7] - 2026-08-23

### Fonctionnalités

- **La carte « Signaux capteurs » (onglet Avancé) affiche enfin la grandeur qui décide de l'arrêt du dosage.** Deux nouvelles lignes « Dispersion pH » / « Dispersion ORP » montrent la dispersion **instantanée** publiée par `GET /debug/sensor_filter_state` (v2.20.6), au format « valeur / seuil », avec un état lisible : pill « **stable** » (verte) ou « **INSTABLE** » (rouge — l'inhibition du dosage est en cours). La valeur est directement comparable aux campagnes de mesure. Filtration arrêtée ou fenêtre en cours de remplissage : « — · pas de jugement (circulation arrêtée ou fenêtre en cours de remplissage) », **sans pill** — jamais « stable », car aucun verdict n'est rendu faute de données. Ces lignes s'affichent même buffer de trace vide, et une panne de la route les passe à « indisponible » sans toucher au reste de la carte.
- **Le « Δ » des stats est renommé « Δ 25 min »** pour ne plus s'y méprendre : c'est un crête-à-crête sur **tout** le buffer (≈ 25 min), dérive lente comprise — jusqu'à 5× la dispersion réelle sur les mêmes données. C'est cette confusion (prendre ce Δ pour la grandeur du drapeau `unstable`) qui a motivé la fonctionnalité.
- **Deux lignes « Dispersion trace (indicatif) »** reconstruisent la dispersion sur la trace affichée (fenêtre glissante de 13 points bruts, affichage médiane · max, fenêtres écartées si trou ou saut consécutif ≥ 1,0 pH / 300 mV ; tiret sous 13 points) pour comparaison avec les rapports de campagne. « Indicatif » car deux écarts assumés avec le firmware : sa fenêtre est gelée circulation arrêtée (la trace enregistre tout), et le rejet se fait sur sauts consécutifs et non sur l'écart à la médiane — hors régime établi, la valeur instantanée fait foi (note affichée dans la carte).

### Documentation

- `docs/features/page-settings.md` : carte « Signaux capteurs » — lignes dispersion, sémantique exacte du tiret, renommage du Δ, stats reconstruites et leurs deux limites de fidélité.
- `docs/API.md` §`sensor_filter_state` : la carte « Signaux capteurs » référencée comme consommateur frontend de la route.
- Pas de nouvel ADR : affichage en lecture seule d'un diagnostic existant, aucune contrainte nouvelle — la duplication JS des constantes de rejet est visible dans le code, commentée et réversible.

---

## [2.20.6] - 2026-08-22

### API

- **On peut enfin lire la valeur qui décide de l'arrêt du dosage automatique.** La route de diagnostic `GET /debug/sensor_filter_state` publie désormais, pour le pH comme pour le chlore, la **dispersion de la mesure** (`dispersion`) : l'écart entre la plus haute et la plus basse des 13 dernières mesures. C'est **cette** grandeur, et elle seule, qui fait basculer la mesure en « instable » et coupe la régulation — or elle n'était visible **nulle part**, ni dans l'interface ni dans l'API : l'appareil arrêtait de doser sur un critère que personne ne pouvait observer. Pour estimer la marge restante pendant une campagne de mesure, il fallait la reconstruire de l'extérieur à partir de relevés espacés de 30 s alors que le critère porte sur 13 mesures à 5 s — avec une incertitude impossible à lever.
- Le **seuil** (`dispersion_max` : 0,5 pH / 100 mV), le nombre de fenêtres avant blocage durable (`latch_cycles` = 3) et la **progression vers le déblocage automatique** (`clean_streak`, 12 requises) sont publiés à côté de la valeur : la marge se lit directement, sans connaître les réglages internes du firmware.
- ⚠️ **`dispersion` est vide (`null`) quand la filtration est arrêtée, et c'est normal** : depuis la 2.20.3, l'appareil refuse d'alimenter ses filtres avec des mesures prises dans de l'eau qui ne circule pas. Pas de mesures ⇒ pas de dispersion. **Corollaire important** : filtration arrêtée, un indicateur « instable = non » ne veut **pas** dire « la mesure est stable », il veut dire « **aucun verdict n'a été rendu** ». Confondre les deux fait conclure à tort qu'un réglage est sain. Ne juger de la stabilité que **circulation en marche**.

### Documentation

- `docs/API.md` : section `GET /debug/sensor_filter_state` — les quatre nouveaux champs, le cas `null` (porte d'alimentation), le corollaire sur `unstable`, et l'insensibilité de l'amplitude à l'offset pH. La route reste **sans authentification**.
- `docs/subsystems/sensors.md` : nouvelle section « Observabilité de la dispersion (v2.20.6) » rattachée à la porte d'alimentation et au latch EMI ; les quatre passerelles `SensorManager` (`getPhDispersion()`, `getOrpDispersion()`, `getPhDispersionCleanStreak()`, `getOrpDispersionCleanStreak()`) ajoutées à l'API publique documentée.
- Pas de nouvel ADR : exposition en lecture seule d'un diagnostic **déjà calculé**, aucun arbitrage, aucune contrainte nouvelle sur le reste du système.

---

## [2.20.5] - 2026-08-22

> ⚠️ **Ce correctif ne répare PAS la pompe qui semble ne pas tourner.** Ce symptôme a une autre cause, **non corrigée** à ce jour : pour une erreur de 0,8 pH, l'appareil commande à la pompe une puissance de **32 %**, à laquelle elle ne tourne que **par intermittence**, alors que son modèle interne la croit à 6,4 mL/min. Le compteur de produit monte donc sur de l'acide qui **ne sort pas du bidon**. La correction demande une **mesure au banc** du seuil de rotation réel de la pompe (pesée du volume recueilli) — la version 2.20.5 livre l'outil de cette mesure, pas la correction.

### Firmware

- **Chaque cycle de dosage en était compté deux.** Au premier instant d'un cycle, le régulateur ne peut pas encore calculer de débit (l'écart de temps avec le cycle précédent, ~29 min, dépasse la limite de 10 s au-delà de laquelle un delta est jugé aberrant et rejeté). L'appareil comptait pourtant déjà un démarrage, puis en comptait un **second** à l'instant suivant, celui où la pompe démarrait vraiment. Conséquences visibles : **deux lignes « Démarrage dosage » à la même seconde** dans le journal, et surtout un **budget de cycles par jour qui valait réellement la moitié** de la valeur réglée — la régulation s'arrêtait donc à mi-journée bien avant la limite affichée. Désormais un cycle n'est compté que s'il a réellement démarré.
- **Limite de cycles par jour : nouveau défaut 24 au lieu de 48.** Compte tenu du double comptage ci-dessus, l'enveloppe **réellement vécue** valait déjà 24 : corriger le comptage sans abaisser la limite aurait **doublé** le nombre d'injections quotidiennes réelles, sous couvert de correctif. 24 reste dans le régime d'entretien normal (20-40 cycles/jour).
  > ⚠️ **Ce nouveau défaut ne s'applique pas aux appareils déjà en service** : la valeur est enregistrée dans l'appareil, une mise à jour ne l'écrase pas. Si la vôtre est à 48, **abaissez-la à la main** dans Paramètres → Régulation, sinon vous doublez le nombre d'injections quotidiennes par rapport à ce que vous viviez.
- **Nouvel avertissement au journal « démarrage décidé mais aucun débit commandé »** (pH et chlore, au plus une ligne toutes les 5 min). Il signale une régulation **sans effet** : la décision de doser est prise, mais aucune puissance n'est envoyée à la pompe. Une occurrence isolée au démarrage d'un cycle est normale ; la même ligne **toutes les 5 minutes en continu** indique une anomalie. C'est le bruit involontaire de l'ancien comportement qui avait révélé le bug ci-dessus — sans cet avertissement, la situation serait devenue totalement silencieuse.

### API

- **Nouveau paramètre optionnel `?duty=N` sur `POST /pump1/on` et `POST /pump2/on`** (0-255, défaut 255 : sans le paramètre, le comportement est **strictement inchangé**). Il permet de faire tourner une pompe à puissance réduite depuis le test manuel, afin de **mesurer le seuil en dessous duquel elle s'arrête réellement** — mesure indispensable pour corriger le défaut décrit en tête d'entrée. Volontairement non borné par le plancher interne de 80/255 : sonder **sous** ce plancher est justement l'objet de la manœuvre. Le paramètre ne peut que **réduire** la puissance par rapport aux 255 déjà atteignables, et toutes les sécurités (authentification, présence d'eau, mode maintenance) restent appliquées en amont.

### Documentation

- `docs/API.md` : paramètre `?duty=N` des deux routes de test de pompe — bornes, motif, exemple de balayage au banc.
- `docs/subsystems/pump-controller.md` : nouvelle section « Comptabilisation d'un démarrage de cycle : invariant » (pourquoi « cycle compté ⇔ cycle réellement démarré » est désormais **structurel** et non une garde ajoutée à côté) ; défaut `maxCyclesPerDay` à 24 et **piège de la persistance** (aucune migration, réglage manuel requis sur un appareil déjà provisionné) ; nouvel avertissement et son throttle ; encadré « limite connue, non corrigée » sur la conversion puissance ↔ débit.
- `docs/subsystems/logger.md` : le nouvel avertissement chiffré dans le budget de rétention du journal persistant (régime nominal ≈ 0,36 Ko/h → rétention ≈ 8-10 h ; régime pathologique ≈ 4,3 Ko/h).
- Pas de nouvel ADR : correction de bug locale, tranchée par le gate `pool-chemistry` dans le cadre existant. L'arbitrage structurant (proportionnel de **temps** vs recalibrage du modèle linéaire) fera l'objet d'un ADR **lorsqu'il sera implémenté**, après la mesure au banc.

### Tests

- 4 tests natifs ajoutés (**609 au total**) sur la logique pure de comptabilisation, dont le **rejeu du scénario réel** de production traversant la garde des 10 s.

---

## [2.20.4] - 2026-08-22

> ⚠️ **Bascule structurante : le core Arduino 3.x / ESP-IDF 5 devient l'environnement par défaut du projet — bootloader ET application.** Le **bootloader** ne se met **pas** à jour en OTA : cette bascule exige un **flash USB une fois** (`./deploy.sh all`). Voir [ADR-0042](docs/adr/0042-core3-environnement-par-defaut.md) et [UPDATE_GUIDE.md](docs/UPDATE_GUIDE.md#changer-de-bootloader--flash-usb-obligatoire-core-3-par-défaut-v2204).

### Build

- **`default_envs = esp32dev_core3`** : le core 3 (pioarduino, release taguée `55.03.37` = core 3.3.7 / IDF 5.5.0) n'est plus un banc de compilation, c'est **l'environnement de production**. Les mentions « BUILD-ONLY — ne flashe JAMAIS la production » de `platformio.ini` étaient devenues fausses : l'image core 3 tournait déjà en production depuis des semaines, poussée en OTA sur un bootloader core 2. `esp32dev` (core 2) reste **compilable et déployable en repli explicite** : `pio run -e esp32dev`, `PIO_ENV=esp32dev ./deploy.sh all`.
- **Toute la chaîne d'outillage passe sur une variable unique `PIO_ENV`** (défaut `esp32dev_core3`, surchargeable) au lieu de dépendre du `default_envs` ou de chemins `.pio/build/esp32dev/` codés en dur : `deploy.sh` (passe `-e "$PIO_ENV"` à `pio run`, `pio run -t upload` et `pio device monitor` ; exporte `PIO_ENV`/`BUILD_DIR`), `build_fs.sh` (écrit `littlefs.bin` dans `$BUILD_DIR`), `ota_update.sh` et `build_all.sh` (chemins d'artefacts dérivés), `tools/decode_coredump.sh` (ELF par défaut dérivé).
- **`tools/decode_coredump.sh` prend l'ELF de l'env réellement déployé** — motif de sûreté, pas de confort : un ELF core 2 confronté à un dump core 3 ne produit **pas** d'erreur franche, il produit une pile de **noms de fonctions plausibles et FAUX**. C'est ce qui a rendu le coredump de production du 15/08 inexploitable.
- **⚠️ Piège documenté : les deux plateformes s'écrasent mutuellement.** Elles installent leur framework sous le **même nom de paquet** `framework-arduinoespressif32` (`3.20017.x` en core 2, `3.3.7` en core 3), donc dans le **même répertoire** — un build core 2 détruit le framework core 3 et réciproquement. Symptôme : échec **immédiat** (~2 s) avec `TypeError: expected str, bytes or os.PathLike object, not NoneType` dans `arduino.py` (`FRAMEWORK_DIR` vaut `None`). **Ce n'est pas aléatoire** — c'est systématiquement un build de l'autre env qui vient de passer. Remède : `pio pkg install -e <env>`. Ce défaut a été pris pour un aléa pendant tout un cycle de travail, les échecs étant contournés par simple relance.
- **Marge flash resserrée** : le core 3 occupe **1 730 053 o** sur une partition applicative de 1 835 008 o, soit **94,3 %** (marge ≈ 102 Ko), contre **84,5 %** en core 2. Le plafond de vigilance de 95 % est presque atteint — à surveiller à chaque nouvelle fonctionnalité.
- **Perte du filet de repli sans câble** : tant que le bootloader restait en IDF 4.4, une image core 3 défaillante se rattrapait en repointant `otadata` (`erase_region 0xE000 0x2000`). Avec un bootloader core 3, ce recours disparaît : **un échec impose un flash USB**, donc un accès physique au boîtier.
- **CI non impactée** : `.github/workflows/tests-natifs.yml` ne construit que l'env `native`.

- **Le moniteur série est enchaîné après tout flash USB** (`firmware`, `fs`, `all`, `factory`). Ce n'est pas un confort : l'ouverture du port bascule DTR/RTS, ce qui **redémarre l'ESP32** — le boot est donc capté **depuis sa première ligne**. Un moniteur lancé à la main dans un second terminal arrive systématiquement trop tard et manque les lignes émises une seule fois. `NO_MONITOR=1 ./deploy.sh all` pour s'en passer (scripts non interactifs). Sortir par `Ctrl+C` n'est plus compté comme un échec de déploiement.
- **Le port série est détecté au lieu d'être codé en dur.** `PORT` valait `/dev/cu.usbserial-0001` en dur : l'upload échouait dès qu'on branchait un autre module (un adaptateur neuf n'énumère pas sous le même nom). Ordre de résolution : variable `PORT`, puis le port historique s'il existe, puis le premier `usbserial` / `wchusbserial` / `SLAB_USBtoUART` trouvé. Sans port, le script s'arrête **avant toute compilation** en listant les ports disponibles, au lieu d'échouer après 2 min de build. Surcharge : `PORT=/dev/cu.usbserial-10 ./deploy.sh all`.
- **`pio run -t upload` reçoit enfin `--upload-port`.** Sans ce paramètre, PlatformIO retombait sur l'`upload_port` de `platformio.ini` et **ignorait le port détecté** : l'upload échouait alors que le module était bien branché, ailleurs.
- **`factory` réordonné — le mot de passe AP est enfin lisible.** L'effacement de flash était fait **avant** les uploads, or `upload_firmware` et `upload_filesystem` redémarrent la puce : le boot qui générait et affichait le mot de passe AP avait donc déjà eu lieu, **sans personne pour le lire**. La partition `nvs` seule (`0x9000` / `0x5000`) est désormais ré-effacée **en dernier**, avec `--after no_reset` pour que le seul reset soit celui provoqué par l'ouverture du moniteur. Le mot de passe s'affiche donc dans les premières lignes du moniteur enchaîné — **seule occasion de le lire** sur un appareil dont l'assistant a déjà été complété.
- **Compatibilité esptool 4 / 5 sondée à l'exécution.** Le paquet `tool-esptoolpy` est **partagé** entre les environnements et chacun impose sa version (`esp32dev` → 4.5.1, `esp32dev_core3` → 5.1.2) : elle bascule au gré du dernier build lancé. esptool 5 a renommé sous-commandes et valeurs d'options en tirets (`erase-region`, `--after no-reset`) là où la 4.x utilise des underscores. Le script sonde `--help` et choisit l'orthographe correspondante, au lieu de supposer une version.
- **Archivage `deploy.sh` supprimé** (`archive_elf()`). L'archivage est fait depuis la v2.20.2 par le hook post-build `tools/archive_build.py`, qui range `builds/<VERSION>/<ENV>/` et couvre **aussi** `pio run` direct et `ota_update.sh`. L'ancienne fonction déversait en parallèle des ELF non versionnés à plat dans `builds/` — un doublon qui repolluait le dossier après chaque ménage.

### Documentation

- **Nouvel [ADR-0042](docs/adr/0042-core3-environnement-par-defaut.md)** — « Le core Arduino 3.x / IDF 5 devient l'environnement par défaut (bootloader ET application) » : ce qui a rendu la porte franchissable (module WROVER-E avec PSRAM, accès USB, image core 3 éprouvée en production), le conflit de paquet framework, la perte du filet `otadata`, la marge flash 94,3 %, et les alternatives écartées (dont « rester en core 2 par défaut avec OTA core 3 »).
- `docs/adr/0034-*` : **note additive datée** — porte de bascule **franchie**, clauses « BUILD-ONLY » et « retour arrière OTA-only » déclarées **caduques**, renvoi vers l'ADR-0042.
- `docs/BUILD.md` : tableau des environnements **inversé** (core 3 = défaut/production, core 2 = repli), nouvelle section `PIO_ENV` (rôle dans chacun des 5 scripts), section dédiée au **conflit de paquet** avec symptôme exact et remède, marge flash par env, avertissement bootloader/USB, et la garde sur l'ELF de décodage de coredump. Plus, du cycle outillage : moniteur enchaîné, détection/surcharge du port, `NO_MONITOR=1`, séquencement du mode `factory`, partage de `tool-esptoolpy`, archivage corrigé (`deploy.sh` n'archive plus rien).
- `docs/UPDATE_GUIDE.md` : nouvelle section « **Changer de bootloader — flash USB obligatoire** » (tableau de ce que l'OTA peut/ne peut pas écrire, procédure, disparition du filet `otadata`, retour arrière `PIO_ENV=esp32dev`). Nouvelle section « Récupérer le mot de passe WiFi AP » — l'ancienne manœuvre (ouvrir `pio device monitor` puis redémarrer) **ne fonctionne pas** sur un appareil configuré. Trois entrées ajoutées au dépannage (port non détecté, upload sur mauvais port, mot de passe AP perdu).
- `README.md` : environnement de build par défaut et repli `PIO_ENV` ; première connexion et liste des scripts alignées sur le moniteur enchaîné et la détection de port.

---

## [2.20.3] - 2026-08-21

> 🔴 **Correctif de sécurité chimique — mise à jour fortement recommandée.** Le filtre de mesure pH pouvait se **verrouiller définitivement** et couper **toute** la régulation — pH **et** chlore — **sans aucune alarme** et sans aucun moyen de reprise depuis l'interface. C'est arrivé en production le 16/08 : le bassin est resté ≥ 3 h sans dosage, et l'anomalie n'a été détectée que parce qu'une valeur affichée semblait incohérente. Firmware **et** filesystem à mettre à jour (`./deploy.sh ota-all`).

### Fonctionnalités

- **Alerte « Régulation à l'arrêt »** : une bannière rouge s'affiche en tête du tableau de bord dès qu'un filtre de mesure est bloqué depuis plus de 5 minutes alors que la filtration tourne — c'est-à-dire dès qu'aucun dosage n'est plus effectué. Elle indique quel produit n'est plus régulé (pH, chlore ou les deux) et mène directement à la page concernée. Une purge de la chambre de mesure, un démarrage de filtration ou une filtration à l'arrêt ne la déclenchent **jamais** : elle ne signale que les blocages anormaux.
- **Bouton « Réarmer le filtre »** dans les modales « État filtre pH » et « État filtre ORP ». Jusqu'ici, la seule façon de débloquer un filtre était une URL de diagnostic non documentée — pratiquement inaccessible. Le réarmement demande une **confirmation explicite** qui prévient que la détection de sonde figée sera effacée, il est limité à **un par capteur toutes les 10 minutes**, et il agit **sur un seul capteur** : réparer le pH ne casse plus le filtre ORP.
- **Le chip d'état du filtre affiche « Régulation à l'arrêt »** en rouge, avant toute autre indication, quand ce blocage est détecté.
- **Nouvelle bannière « Bascule automatique en mode manuel »** sur la page pH : quand l'appareil coupe lui-même la régulation pH par sécurité (refus de plancher acide répétés), il l'écrit désormais **noir sur blanc**, avec la date et la cause — au lieu de laisser l'utilisateur découvrir un mode `manuel` inexpliqué. L'information est conservée même après redémarrage, et disparaît dès que vous choisissez vous-même un mode de régulation.
- **Deux nouvelles entités Home Assistant** : « Filtre pH Bloqué » et « Filtre ORP Bloqué » (`device_class: problem`), pour automatiser une notification externe.

### Firmware

- **Le filtre de mesure ne pouvait plus jamais se débloquer.** Un échantillon éloigné de plus de 1,0 pH (300 mV en ORP) de la médiane était écarté **avant** d'alimenter le filtre — mais la médiane ne pouvant évoluer que par des échantillons acceptés, dès que le pH réel s'écartait durablement, **chaque** mesure suivante était rejetée à son tour et la médiane restait figée **indéfiniment**. Relevé en production : médiane interne **4,79** contre un pH brut **6,96**, compteur de rejets saturé à 255. Sans dosage pH (filtre non prêt) **et** sans dosage chlore (suspendu par sécurité en l'absence de pH fiable).
- **Cause première : le filtre avalait des mesures que l'appareil savait fausses.** Pendant un arrêt de circulation, l'eau stagne dans la chambre de mesure — au voisinage du point d'injection d'acide, elle peut être très acide. L'appareil déclarait bien ces mesures « non représentatives » et suspendait l'injection, mais **rien n'empêchait le filtre de les ingérer** : sa médiane convergeait vers la valeur de l'eau stagnante, puis rejetait comme aberrantes les vraies mesures qui suivaient la reprise. **Le filtre n'ingère plus rien** quand l'eau est à l'arrêt ou pendant la stabilisation, avec une garde de 60 s après chaque reprise de circulation (utile même si le délai de stabilisation est réglé à 0), et il **repart de zéro** au retour à des mesures représentatives.
- **Sortie de secours** : 12 mesures rejetées d'affilée mais **proches les unes des autres** (≤ 0,20 pH / 40 mV) décrivent une nouvelle valeur, pas du bruit — le filtre abandonne alors sa médiane et se réamorce. Trois garde-fous cumulatifs empêchent d'en faire une porte de sortie pour une sonde défaillante : **30 min** minimum entre deux réamorçages, valeur plausible exigée (pH 6,50–8,20 brut, ORP 300–950 mV), et cohérence de la série vérifiée. Si l'un refuse, le filtre **reste** bloqué, un `CRITICAL` et une alerte MQTT partent, et la sonde est à vérifier. Les 12 mesures rejetées sont **jetées**, jamais réinjectées.
- **Une sonde qui rate une lecture de temps en temps peut quand même réamorcer** : au plus 2 trous par série, jamais 2 consécutifs. Sans cette tolérance, une sonde manquant un cycle sur douze pendant un vrai changement de pH n'aurait **jamais** réamorcé — l'incident se serait reproduit à l'identique.
- **Un réamorçage n'efface jamais un diagnostic de sonde** : contrairement à une réinitialisation complète, il ne touche ni la détection de sonde figée ni l'alarme de bruit électrique. Le dosage reste bloqué pendant le réamorçage complet (~1 min). Motif : l'origine de l'excursion à ~4,8 **n'est pas encore établie** — stagnation acide ou sonde défaillante — et le correctif ne doit pas masquer un défaut matériel.
- **Alarme d'absence de régulation** : filtre non prêt pendant 5 min alors que la filtration tourne et que les mesures sont représentatives → `CRITICAL` + alerte MQTT avec la cause probable, répété toutes les 30 min, `INFO` à la levée. La purge et la filtration à l'arrêt ne comptent jamais.
- **La bascule automatique de la régulation pH en manuel est persistée** (date + cause en NVS) : le journal étant un tampon circulaire, cette trace disparaissait en quelques heures et il devenait impossible de dire si le mode `manuel` venait de l'appareil ou d'une action humaine. Elle n'est acquittée que sur un **changement réel** de mode — enregistrer les réglages sans toucher au mode ne l'effacera pas.
- **Instrumentation pour trancher l'origine du défaut** : les mesures brutes pH/ORP sont désormais journalisées à la reprise de circulation **et** en fin de stabilisation, avec l'écart entre les deux. Un écart franc confirmerait la stagnation acide ; un écart nul sur une valeur aberrante orienterait vers une sonde défaillante.
- **Correctif de concurrence** : plus aucune réinitialisation de filtre n'est exécutée depuis le serveur web. Les demandes sont mises en attente et traitées par la boucle principale (< 100 ms), seul contexte autorisé à écrire dans un filtre — l'ancienne route de diagnostic violait ce contrat et pouvait corrompre le filtre pendant une lecture.
- **Le filtre ORP bénéficie des mêmes correctifs** : la logique est partagée, donc le défaut l'était aussi.

### Sécurité

- **`POST /debug/sensor_filter_reset` exige désormais une authentification administrateur.** Cette route effectuait exactement la même action destructrice que le nouveau bouton de réarmement (effacement de la détection de sonde figée), mais était **ouverte sans mot de passe** : elle contournait intégralement les garde-fous posés sur l'autre chemin. Les autres routes `/debug/*` restent inchangées.

### API

- **Nouveau `POST /reset-sensor-filter?sensor=ph|orp`** — auth administrateur, sélectif par capteur, 1 réarmement/capteur/10 min (`429` avec `retry_after_s`), réponse `{"queued":true}` (exécution au tour de boucle suivant).
- `POST /debug/sensor_filter_reset` accepte `?sensor=ph|orp` (sans paramètre : les deux filtres, comme avant).
- `GET /data` et WebSocket : nouveaux booléens `phFilterAlarm` / `orpFilterAlarm`.
- `GET /get-config` : nouveaux champs **lecture seule** `ph_auto_manual_epoch` / `ph_auto_manual_reason`.
- MQTT : nouveaux topics retain `{base}/ph_filter_stuck` et `{base}/orp_filter_stuck` (`ON`/`OFF`), et nouvelles alertes `ph|orp_filter_not_ready` et `ph|orp_filter_rearm_denied`.

### Tests

- **605 tests natifs**, zéro régression — dont le **rejeu de l'incident** à partir des valeurs réellement relevées (médiane établie à 4,79 puis échelon soutenu à 6,96 : le filtre doit redevenir prêt en un nombre d'échantillons borné et asserté), la transition purge → mesures valides, la non-régression sur aberration isolée, et la tolérance aux trous de lecture. `pio run` bi-core SUCCESS.

### Documentation

- **Nouvel [ADR-0041](docs/adr/0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md)** — « Autorité des mesures non représentatives et sortie de secours du filtre de tendance centrale » : le verdict `pool-chemistry` et son raisonnement, les six alternatives écartées (dont « geler l'alimentation seule », **inopérante** — le filtre devient non prêt de toute façon et la médiane pré-arrêt reste autoritaire — et « `ready` immédiat après réamorçage », qui masquerait une sonde morte), et ce que la décision verrouille pour la suite.
- `docs/adr/0027-filtre-tendance-centrale.md` : note additive datée — le fail-closed « spike répété » est requalifié en piège sans issue, renvoi vers l'ADR-0041 (aucune décision annulée).
- `docs/API.md` : ⚠️ **correction d'une information fausse** — le titre de `POST /debug/sensor_filter_reset` annonçait « pas d'auth » ; documentation de `POST /reset-sensor-filter`, du paramètre `?sensor=`, de la sémantique *queued*, des champs `/data` et `/get-config`.
- `docs/subsystems/sensors.md` : ⚠️ **correction** — le reset n'agit plus sur « les deux » filtres ; sections porte d'alimentation, sortie de secours et ses 3 gardes, tolérance aux trous, distinction `rearm()`/`reset()` et **pourquoi**, alarme, réarmement par capteur.
- `docs/subsystems/measure-validity.md` : le filtre est un **nouveau consommateur** du verdict de représentativité ; instrumentation des bruts au front montant / en fin de purge.
- `docs/subsystems/pump-controller.md`, `docs/subsystems/mqtt-manager.md`, `docs/subsystems/ws-manager.md`, `docs/MQTT.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md`, `docs/features/page-dashboard.md` : persistance de la bascule pH, topics et entités HA, champs WS et budget de payload, boutons et bannières.

---

## [2.20.2] - 2026-08-15

> ⚠️ **Correctif de disponibilité — mise à jour recommandée.** L'appareil tournait **sans protection contre les crashs mémoire** environ 2 h après chaque démarrage. Firmware seul, aucune mise à jour du filesystem nécessaire.

### Firmware

- **Le parachute anti-crash mémoire était MORT ~2 h après chaque boot.** Le filet de sécurité (`new_handler`) se désinscrit lui-même quand sa réserve d'urgence est épuisée, et sa ré-inscription se trouvait **derrière un `return`** : passé la deuxième famine, plus aucun chemin ne le rétablissait. C'est la cause directe des **4 crashs de production** `abort()` sur la tâche réseau `async_tcp` (le serveur web fait des allocations « lançantes » à chaque connexion HTTP et à chaque trame WebSocket). La ré-inscription est désormais faite **en tête du tick, avant toute sortie possible**.
- **Nouveau témoin permanent `oom_handler_registered`** dans `GET /get-system-info` : on peut enfin vérifier à tout instant que le filet est présent, sans dépendre du journal. Champ **additif** — aucun client existant n'est cassé.
- **Le réarmement du parachute tournait en boucle infinie** et fabriquait **~100 % de son propre signal d'alarme** : il lisait une mesure mémoire figée à 27 636 o (reliquat d'IRAM inutilisable) alors que le plus gros bloc réellement allouable valait 4 596 o, et retentait donc une allocation de 12 Ko vouée à l'échec **toutes les 10 s** — soit les 1 025 « échecs d'allocation » comptés le 15/08. Le réarmement lit maintenant la mesure **8 bits réelle**, et les échecs **auto-infligés** sont exclus du compteur.
- **Paliers de réarmement recalibrés** sur le régime réel mesuré (`maxBloc8 ∈ [4 084 ; 9 204]`, creux 3 060) : seuil du palier de repli **12 288 → 2 048**, taille de la réserve de repli **2 048 → 1 024**. Le palier nominal reste à 24 576, **délibérément inatteignable** en régime fragmenté — il repart tout seul si la fragmentation disparaît.
- **Le journal ne se détruit plus lui-même.** Le WARN d'échec d'allocation passe d'un rythme **fixe de 30 s** (~120 lignes/h, ~90 % la même ligne) à un **backoff** immédiat → 30 s → 5 min → 30 min, avec retour au rythme initial après 1 h sans incident. La trace « Heap 8bits: » passe de **5 min à 30 min** et perd sa partie `interne:`, figée et sans information. Résultat mesuré : **rétention du journal persistant de 26-34 min à 14,6-19,5 heures** (14,1 Ko/h → 0,41 Ko/h), à taille de fichier **inchangée** (8 Ko). C'est ce flot qui avait détruit le post-mortem du crash de 16:43 avant qu'il puisse être lu.
- **Identification du fragmenteur** : le nom de la fonction appelante et le masque `caps` de chaque allocation échouée, jusqu'ici **jetés**, sont désormais capturés et journalisés — depuis la boucle principale, **jamais** depuis le callback (le verrou du tas y est détenu). Seule voie vers l'origine de la fragmentation, inconnue après trois cycles.
- **⚠️ Ce que cette version NE change PAS, volontairement** : les gardes `kRestartMaxAllocHeapBytes` (redémarrage d'auto-guérison), `kHttpHeapMarginBytes`, `kWsPushMinAllocBytes`, `kLowHeapWarnBytes` et les deux seuils de la mise à jour en ligne **lisent toujours l'ancienne mesure**. Le gate de sécurité chimique a rendu un **NO GO explicite** : les basculer aux valeurs actuelles rendrait la **suspension du dosage atteignable en fonctionnement normal** (seuil 8 192 contre un minimum réel de 3 060), casserait l'interface web, les mises à jour temps réel et la mise à jour en ligne. Recalibrage après **24 h de relevés propres**.

### Build

- **Archivage automatique des artefacts à chaque `pio run`** (`tools/archive_build.py`, branché en `extra_scripts = post:` sur les deux environnements matériels) : `firmware.elf` **et** `firmware.bin` sont copiés dans `builds/<VERSION>/<ENV>/firmware_<horodatage>.{elf,bin}`, rétention de 5 par version **et par environnement**. Cause : `deploy.sh` archivait bien l'ELF, mais `pio run` direct et `./ota_update.sh` **ne passent pas par lui** — le dernier ELF archivé datait du 02/08, ce qui a rendu le **coredump du 15/08 inexploitable** (`coredump SHA256 ≠ app SHA256`). Le cloisonnement par environnement est essentiel : 5 builds core 2 auraient évincé l'ELF core 3 réellement déployé.

### Tests

- **582 tests natifs** (+16 : condition de ré-inscription du handler, escalade et saturation du backoff, exclusion des échecs auto-infligés, paliers de repli recalibrés), zéro régression. `pio run` et `pio run -e esp32dev_core3` SUCCESS (flash 83,8 % / 93,6 %).

### Documentation

- **Nouvel [ADR-0040](docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md)** — « On ne calibre pas des seuils sur une fragmentation pathologique » : acte le **régime bi-métrique assumé** (seuls les paliers de réarmement passent en 8 bits), le NO GO du gate chimique sur les 5 autres gardes, et les 4 alternatives écartées.
- `docs/adr/0035-*` : **note additive datée** — invariant de ré-inscription du `new_handler` (en tête de tick, indépendante du miroir ; miroir posé **avant** l'inscription sur les trois sites).
- `docs/adr/0038-*` : **note additive datée** — la règle `clamp(0,40 × min24h ; 8192 ; 16384)` est déclarée **inapplicable**, les mesures du 15/08 sont **invalidées** comme base de calibrage, l'invariant d'ordre des seuils est momentanément sans signification.
- `docs/subsystems/oom-guard.md` : ré-inscription et son invariant d'ordre (avec le pourquoi), réarmement 8 bits, exclusion des échecs auto-infligés, backoff du WARN, capture de l'appelant, échelle des seuils annotée **par métrique**.
- `docs/subsystems/logger.md` : rétention **réelle chiffrée** avant/après et cadences des émetteurs périodiques ; règle « chiffrer le débit en Ko/h avant d'ajouter une trace périodique persistée ».
- `docs/subsystems/system-health.md` : cadence et format de la trace « Heap 8bits: » (30 min, `interne:` retiré).
- `docs/API.md` : champ additif `oom_handler_registered` de `/get-system-info`.
- `docs/BUILD.md` : archivage ELF+BIN à chaque `pio run`, nommage, cloisonnement par version **et** par environnement, et la panne forensique qui l'a motivé.
- Spec de suivi créée : `specs/features/todo/bug-bascule-gardes-8bits-recalibrage.md` (bloc E différé).

---

## [2.20.1] - 2026-08-15

> 🔄 **Mise à jour du filesystem nécessaire** : l'interface change (carte « Mise à jour manuelle »). Faire le **firmware d'abord**, puis `./build_fs.sh` et `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi).

### Fonctionnalités

- **Le type de mise à jour est déduit du nom du fichier** dans la carte « Mise à jour manuelle » (Paramètres > Système) : sélectionner `littlefs.bin` règle automatiquement le type sur « Système de fichiers », et une notice bleue l'annonce. Le sélecteur revenait auparavant à « Firmware » **à chaque rechargement de page** — donc juste après un flash de firmware réussi, qui redémarre l'appareil et recharge l'interface : envoyer le filesystem dans la foulée partait déclaré « firmware » et échouait. C'est exactement l'incident vécu deux fois.
- **Avertissement, jamais blocage** : si vous changez le type et qu'il ne correspond plus au nom du fichier, la notice passe en orange et une confirmation est demandée à l'envoi — **franchissable en un clic**. Le cas légitime existe (image renommée), il n'est pas interdit. Le type n'est **pas mémorisé** d'une visite à l'autre : ce serait recréer le même piège dans l'autre sens.
- **La cause d'un échec est enfin affichée**, en clair et en français, sous la barre de progression : « *l'image reçue n'est pas un firmware ESP32 (octet magique 0xE1 au lieu de 0xE9) — type « Système de fichiers » attendu ?* ». Auparavant l'interface n'affichait que « FAIL ».
- **Une image envoyée avec le mauvais type est refusée sans aucun effet de bord** : les pompes doseuses ne sont pas inhibées, le filesystem n'est pas démonté, rien n'est écrit, l'appareil ne redémarre pas. Corriger le type et renvoyer suffit.

### Firmware

- **Le journal n'est plus noyé par un échec OTA.** Une tentative ratée écrivait la **même ligne ~200 fois**, ce qui **effaçait tout l'anneau de journal** (200 entrées) — y compris les lignes qui désignaient le coupable, et y compris les traces d'un incident sans rapport en cours d'analyse. Un échec produit désormais **exactement une** ligne, et le chemin d'échec d'écriture est devenu terminal.
- **La cause réelle n'est plus écrasée.** `Update.errorString()` est capturé **avant** `Update.abort()` sur les **deux** sites d'écriture (upload manuel et téléchargement GitHub) : `abort()` remplaçait le code d'erreur réel (`MAGIC_BYTE`, `ERASE`, `WRITE`, `SPACE`…) par « Aborted », rendant toute cause indéchiffrable jusque dans le message de finalisation.
- **Pré-contrôle du type au premier octet reçu**, avant l'inhibition des pompes, avant le démontage de LittleFS et avant `Update.begin()`. **Asymétrie délibérée** : refus **dur** uniquement quand le type déclaré est « firmware » et que le premier octet n'est pas `0xE9` — ce contrôle est prouvablement équivalent à celui que la couche `Update` applique déjà, donc **l'ensemble des images acceptées est INCHANGÉ**. Dans le sens inverse (image d'allure firmware déclarée « système de fichiers »), simple **avertissement** : LittleFS n'a pas d'octet magique fiable à l'offset 0, un refus produirait des faux positifs.
- **`POST /update` — le code HTTP porte enfin le verdict** : `400` pour un refus de type ou d'empreinte, `500` pour un échec de flash, `409` inchangé pour un upload concurrent, `200` avec corps exactement `OK` pour un succès (inchangé). Jusqu'ici **tout échec était servi en `200 OK`** avec un corps `FAIL` : un client qui testait le code HTTP voyait un succès. Le **texte** de la cause est destiné à l'utilisateur et peut être reformulé sans préavis — le contrat machine est le **code HTTP**.
- **Remontage du filesystem sur échec** (`LittleFS.begin(false)`, **jamais** de formatage) : l'interface web reste servie au lieu de disparaître jusqu'au prochain redémarrage. Log `CRITICAL` si le remontage réussit **après un échec** — une partie de l'ancienne image a pu être écrasée, le contenu peut être incohérent : **relancer la mise à jour**.
- **Ordre imposé (sécurité dosage)** : `setOtaInProgress(false)` **toujours avant** tout `LittleFS.begin()`. Ces handlers tournent sur la tâche AsyncTCP, **non abonnée au watchdog** : un montage long ou paniquant y laisserait les pompes doseuses inhibées sans redémarrage possible. Le chemin de téléchargement GitHub, qui avait déjà l'ordre inverse, a été corrigé.
- **Détection par nom de fichier** : le nom **nu** `littlefs.bin` produit par `build_fs.sh` est enfin reconnu (l'ancienne règle exigeait un point devant, `.littlefs.bin` — aucun fichier réel ne correspondait).
- **`ota_update.sh`** : le test de succès `grep -q "OK"` n'était pas ancré, tout corps contenant « OK » passait pour un succès. Désormais code `200` **et** corps exactement `OK` ; le corps d'erreur est affiché.
- **Tests** : nouvelle suite `test/test_native_ota_type/` (29 cas) — **565 tests natifs**, zéro régression. `pio run` et `pio run -e esp32dev_core3` SUCCESS (flash 93,5 %), `./build_fs.sh` SUCCESS (327 680 o).

### Documentation

- `docs/API.md` : contrat de réponse de `POST /update` remplacé par le tableau des statuts (l'ancien « `200 OK` / `200 OK` corps `FAIL` » était périmé), pré-contrôle de l'octet magique, règle « le contrat machine est le code HTTP, le texte de la cause peut être reformulé ».
- `docs/subsystems/ota-manager.md` : politique « une seule erreur journalisée par session » et sa raison, remontage FS sur échec et son risque, ordre imposé dosage-avant-montage et son motif watchdog, et le **principe général de l'asymétrie de refus** (« on ne bloque en dur que là où le contrôle est prouvablement équivalent à ce que la couche sous-jacente impose déjà ; ailleurs on trace »).
- `docs/features/page-settings.md` : carte « Mise à jour manuelle » — déduction du type, notice, confirmation franchissable, affichage de la cause d'échec.
- `docs/UPDATE_GUIDE.md` : procédure manuelle actualisée + section « Lire un échec de mise à jour manuelle ».
- **Aucun ADR** : les 4 critères de déclenchement ont été confrontés explicitement et aucun n'est rempli (justification consignée dans la spec).

---

## [2.20.0] - 2026-08-15

> ⚠️ **À lire avant de saisir un offset non nul — deux effets irréversibles sur vos historiques.**
>
> 1. **Home Assistant : marche dans les courbes.** Les entités `sensor.*_ph` et `sensor.*_ph_filtered` **ne sont pas renommées** (aucune automatisation, aucun dashboard n'est cassé) mais elles **changent de valeur** le jour où vous enregistrez un offset non nul. Vos graphiques HA présenteront un **saut** de la hauteur de l'offset à cette date. C'est le comportement voulu : renommer les entités aurait détruit tout l'historique long terme pour un décalage de ±0,50 pH au maximum. Pour comparer avant/après, superposez `sensor.*_ph_raw` (toujours brut) et `sensor.*_ph`.
> 2. **Historique interne : discontinuité non réversible.** `/history.json` enregistre désormais le pH **corrigé**. Les points **antérieurs** restent en brut et ne sont **pas** recalculés — le graphique de la page pH présentera donc la même marche, définitivement. Aucune migration n'est proposée : réécrire l'historique reviendrait à inventer des mesures qui n'ont jamais été faites.
>
> 🔄 **Mise à jour du filesystem obligatoire** : l'interface change (nouvelle carte sur la page pH). Après le firmware, reconstruire et reflasher le FS : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Vider le cache du navigateur si l'ancien affichage persiste.

### Fonctionnalités

- **Correction logicielle de la mesure pH** (feature-067) : nouvelle carte « **Correction de la mesure pH** » sur la page pH, avec un champ **Offset pH** au centième, de **−0,50 à +0,50** (défaut 0,00 = aucune correction). `pH corrigé = pH brut EZO + offset`. Destinée à compenser un **écart d'implantation de la sonde** (position dans le circuit, potentiel électrique de l'eau) constaté face à une mesure de référence (trousse DPD, photomètre). La carte est visible dans **tous** les modes de régulation : elle corrige la mesure, pas la régulation.
- **La calibration du module Atlas EZO n'est JAMAIS touchée.** Aucune commande `Cal,*` n'est émise : l'offset est une correction en aval de la lecture, réversible à tout moment en repassant à 0,00. Un décalage supérieur à ~0,30 pH signale d'ailleurs un problème de **sonde ou de calibration** — le message d'avertissement le dit explicitement.
- **Deux paliers d'avertissement** dans la carte, calculés **pendant la saisie** (avant d'enregistrer) : à partir de **0,15**, le système annonce le **pH réel** qu'il visera (« avec une consigne de 7,20 et cet offset, l'eau sera régulée à 7,00 ») ; à partir de **0,30**, il ajoute que la sonde devrait être recalibrée plutôt que compensée, et que le dosage d'acide sera bloqué sous 6,90.
- **Valeur refusée, jamais rabotée** : un offset hors plage est **rejeté** (erreur affichée, champ conservé tel quel), il n'est pas ramené silencieusement à la borne. Un offset écrêté serait un mensonge de mesure — vous croiriez avoir enregistré 0,80 pendant que le système appliquerait 0,50.
- **Diagnostic complet** dans le modal « État filtre pH » (clic sur le chip d'état) : brut EZO, offset appliqué, valeur corrigée, médiane corrigée, valeur filtrée corrigée — de quoi dire à tout instant ce qui vient de la sonde et ce qui vient de la correction.
- **L'écran de calibration continue d'afficher le BRUT**, exclusivement. Un repli qui pouvait y faire apparaître la valeur corrigée pendant une fenêtre de lecture périmée a été **supprimé** : on ne calibre pas une sonde sur une valeur déjà corrigée.

### Firmware

- **Point d'application : en SORTIE des accesseurs** ([ADR-0039](docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)). Le filtre pH (médiane 13 + EMA + rejet de pics + latch EMI) continue de travailler sur le signal **brut**. Médiane et EMA étant invariantes par translation, le résultat est mathématiquement identique — mais en sortie, un changement d'offset est pris en compte **immédiatement** au lieu d'être dilué sur ~65 s de fenêtre de lissage, et les seuils de rejet comme le latch EMI restent calés sur le signal capteur. Un test natif **démontre** `filtre(x+c) == filtre(x)+c` sur le filtre réel.
- **Sémantique des accesseurs pH — règle mnémotechnique : *seuls les `*Raw` sont bruts***. `getPh()`, `getPhMedian()`, `getPhFiltered()` renvoient la valeur **corrigée** ; `getPhRaw()` renvoie le **brut EZO** — il était auparavant un **alias pur** de `getPh()`, ce qui n'est plus vrai. Nouveau `getPhFilteredRaw()` (brut filtré), **réservé aux filets de sécurité**, assorti d'une **garde d'âge** : il renvoie NaN si le filtre n'est pas frais, faute de quoi une moyenne périmée pourrait déclencher un faux blocage — ou masquer un vrai.
- **Filet de sécurité #1 — plancher d'injection d'acide sur le pH BRUT** : sous **6,80** (ou **6,90** si \|offset\| > 0,30), **toute** injection d'acide est refusée, en régulation automatique **comme** programmée, quelle que soit la valeur corrigée. Log `CRITICAL` + alerte MQTT `ph_raw_floor`. Deux paliers parce que l'incertitude sur le pH réel croît avec l'ampleur de la correction. **Inerte à offset nul.**
- **Escalade : 3 refus de plancher en 24 h** → `CRITICAL` « sonde pH à recalibrer ou remplacer » et **bascule de la régulation pH en manuel** (persistée). Un plancher qui se déclenche à répétition signe une sonde morte ou un offset saisi à l'envers, pas un incident ponctuel.
- **Filet de sécurité #2 — inhibition du dosage chlore si le pH brut dépasse 7,60** : à ce pH la fraction d'acide hypochloreux (forme active du chlore) s'effondre, l'ORP mesuré chute à concentration constante, et la régulation ORP compenserait en **surdosant**. Le dosage est suspendu ; le correctif est de faire baisser le pH. `CRITICAL` + alerte MQTT `orp_ph_raw_inhibit`.
- **Filet de sécurité #3 — pH brut indisponible = chloration automatique suspendue** (fail-closed). Raison : `canDose(1)` ne consulte **jamais** le pH, ce filet est donc l'**unique** garde du couplage pH/chlore. Journalisé en `INFO` (situation transitoire au démarrage), pas en `CRITICAL`. Le dosage **programmé** reste aveugle à la mesure, par conception.
- **Écrire l'offset ne réinitialise aucun compteur de dosage** (cycles du jour, cumul journalier, pause anti-cycling) : sinon des enregistrements répétés permettraient de contourner les limites journalières. Un changement pendant une injection en cours ne l'interrompt pas — il prend effet au cycle suivant.
- **`POST /save-config`** : `ph_offset` hors de `[−0,50 ; +0,50]` → **HTTP 400** (aucun champ de la requête n'est appliqué). Champ absent ou non numérique → paramètre inchangé. C'est le **premier** champ de cette route à refuser plutôt qu'à écrêter — incohérence de style assumée. Chaque changement effectif est journalisé (`Offset pH modifié: ancien=… nouveau=…`).
- **Persistance NVS** (clé `ph_offset`, défaut 0,00) avec **fail-safe au boot** : une valeur relue hors bornes est neutralisée à 0,00 avec un `WARN` — l'offset ne peut jamais entrer corrompu dans la régulation.
- **MQTT — aucun renommage, aucune entité ajoutée** : `poolcontroller_ph` et `poolcontroller_ph_filtered` portent le **corrigé**, `poolcontroller_ph_raw` reste le **brut**. Le test de vivacité capteur (`phStale` de l'alerte `sensor_stale`) consomme le **brut** : un offset ne peut ni provoquer ni masquer une détection de sonde muette. **Aucune entité `ph_offset`** n'est créée — c'est un paramètre de configuration, pas une mesure.
- **Alerte `ph_abnormal` enrichie** : la détection porte sur le pH corrigé, mais le message cite **toujours** le brut et l'offset (`pH=5.30 (brut=4.90, offset=0.40)`) — sans quoi on ne peut pas distinguer une sonde à la dérive d'un offset mal saisi.
- **`GET /debug/sensor_trace`** : nouveau champ `phCorrected` **à côté** de `phRaw` et `phFiltered`, qui restent tous deux **bruts** dans cette trace de diagnostic (c'est le seul endroit où `phFiltered` n'est pas corrigé — l'objectif de l'outil est justement de séparer mesure et correction). Coût : échantillon 24 → 28 octets, buffer ≈ 8,4 Ko RAM, page de 50 ≈ 4,5 Ko.
- **Tests** : **537 tests natifs** (+30, nouvelle suite `test_native_ph_offset`), zéro régression. Flash : core 2 **83,5 %**, core 3 **93,3 %**. `build_fs.sh` OK.

### Documentation

- **Nouvel [ADR-0039](docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)** — « Offset logiciel de mesure pH appliqué en sortie des accesseurs ». **Alternatives écartées** : *appliquer à l'ingestion comme l'offset de température* (ce patron n'est **pas transposable** — la température corrigée est renvoyée à l'EZO dans `RT,<temp>` pour la compensation thermique, elle DOIT donc être corrigée tôt ; le pH n'est réinjecté nulle part) ; *modifier le filtre* (interdit, module pur [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) ; *corriger seulement à l'affichage* (le système régulerait sur une valeur invisible) ; *renommer les entités MQTT* (casserait toutes les automatisations HA) ; *publier une entité HA `ph_offset`* (point de commande sur l'entrée de la régulation chimique hors du chemin validé) ; *écrêter au lieu de refuser*. **Verrouillages** : sémantique publique des entités `ph` / `ph_raw` / `ph_filtered`, règle « seuls les `*Raw` sont bruts », pureté de `SensorFilter`, filets de sécurité qui lisent le brut, garde d'âge de `getPhFilteredRaw()`, interdiction de réinitialiser un compteur de dosage en écrivant l'offset.
- `docs/API.md` : champ `ph_offset` dans `/get-config` et `/save-config` (type, plage, **corps exact de la réponse 400**, comportement si absent ou non numérique), tableau brut/corrigé des valeurs pH de `/data`, champ `phCorrected` de `/debug/sensor_trace` et son exception à la règle `*Raw`.
- `docs/MQTT.md` : sémantique explicite de `ph` / `ph_raw` / `ph_filtered` / `ph_median`, mention explicite qu'**aucune entité n'a été renommée** et qu'aucune entité `ph_offset` n'existe, conséquence sur les historiques HA, nouvelles alertes `ph_raw_floor` et `orp_ph_raw_inhibit`, message `ph_abnormal` enrichi.
- `docs/features/page-ph.md` : nouvelle section « Carte Correction de la mesure pH » (champ, texte d'aide, **les deux paliers d'avertissement** avec les libellés exacts, enregistrement et gestion du refus serveur, modal de diagnostic), et section « L'écran de calibration reste sur le BRUT » (repli supprimé et pourquoi).
- `docs/subsystems/sensors.md` : section « Offset logiciel de mesure pH » — point d'application, **divergence assumée avec l'offset de température**, tableau du contrat des accesseurs, garde d'âge de `getPhFilteredRaw()` et sa justification, copie locale `_phOffset`, tableau des consommateurs brut/corrigé.
- `docs/subsystems/pump-controller.md` : section « Filets de sécurité sur le pH brut » — les trois filets, leur logique fail-closed, l'escalade des 3 refus, l'**interdiction de réinitialiser les compteurs**, le tableau des constantes ; note sur l'entrée corrigée du calcul d'erreur ; rappel que l'ordre de `canDose()` est inchangé.
- `docs/subsystems/ws-manager.md` : champ de config `ph_offset` (message `config`, pas `sensor_data`) et changement de valeur des champs `sensor_data` existants.

---

## [2.19.0] - 2026-08-15

> 🔎 **Cause racine des 3 crashs de production trouvée : la métrique mémoire du projet était fausse depuis toujours.** Cette version **ne corrige rien du comportement** — elle **mesure**. Le recalibrage des seuils et la bascule des gardes de sécurité arrivent en **2.19.1**, après relevés on-target. Risque de déploiement : **nul** (aucun seuil, aucune garde, aucune décision de l'appareil n'est modifiée).
>
> 🔄 **Mise à jour du filesystem nécessaire** : l'interface change (carte Infos système). Après le firmware, reconstruire et reflasher le FS : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Vider le cache du navigateur si l'ancien affichage persiste.

### Firmware

- **La métrique mémoire mesurait le mauvais pool — toutes les gardes étaient aveugles** (`bug-metrique-heap-inclut-iram-32bits`, [ADR-0038](docs/adr/0038-metrique-memoire-allouable-8-bits.md)) : `ESP.getFreeHeap()` et `ESP.getMaxAllocHeap()` interrogent `MALLOC_CAP_INTERNAL`, un pool qui **englobe l'IRAM accessible en accès 32 bits alignés seulement**. Cette mémoire ne peut **pas** servir à un `malloc()`/`new` ordinaire, qui exige `MALLOC_CAP_8BIT`. Le firmware surestimait donc en permanence la mémoire disponible. Trois preuves convergentes, toutes issues de l'instrumentation livrée en 2.18.8 : le « plus gros bloc » **figé à 27 584 o pendant des jours et à travers les redémarrages** (signature d'un reliquat d'IRAM, pas d'un bloc DRAM vivant) ; des allocations de **92 à 2 872 o qui échouent réellement** pendant que l'indicateur annonce 27 Ko libres (aucune contradiction : la **famine 8 bits était réelle**) ; l'**intégrité du tas vérifiée OK toutes les 5 min**, donc **aucune corruption**. C'est la **cause racine des trois crashs** du 11/08 (`new AsyncWebServerRequest`, 356 o), du 13/08 (`new AsyncFileResponse`, 156 o) et du 15/08 (verrou UART créé paresseusement dans le chemin de log → `abort()`).
- **Conséquence la plus coûteuse : l'auto-guérison mémoire n'a JAMAIS fonctionné.** Le seuil `kRestartMaxAllocHeapBytes` = 8 192 o est comparé à un indicateur qui **ne descend jamais sous ~27 Ko** : depuis sa création ([ADR-0029](docs/adr/0029-auto-guerison-heap.md)), ce filet n'a pas pu déclencher un seul redémarrage. Sont dans le même cas les gardes de push WebSocket, le WARN heap, les deux seuils de la garde TLS ([ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)) et les deux paliers de réarmement du parachute OOM ([ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)) : **9 gardes au total, toutes calibrées sur des chiffres faux**.
- **Nouveau module `heap_metrics`** (`src/heap_metrics.h` / `.cpp`) — **source unique** de la mesure : `heapFree8Bit()`, `heapLargestFreeBlock8Bit()` et `heapMetricsSnapshot()` (les quatre valeurs des deux familles en un appel) sur le pool `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, c'est-à-dire **exactement ce qu'un `malloc()`/`new` peut obtenir**. Sans log, sans allocation, sans `String` — appelable depuis les chemins critiques. Ces fonctions restent **interdites** depuis le callback d'échec d'allocation et depuis le `new_handler` (réentrance allocateur = deadlock, contrat [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)).
- **Ce qui bascule sur la mesure 8 bits : uniquement l'observation.** L'anneau de cinétique RTC et le miroir qui alimente le post-mortem d'allocation enregistrent désormais la mémoire réellement allouable. **Aucune décision ne dépend de ces zones** — c'est purement observationnel.
- **Les deux magics de RAM RTC sont bumpés** : anneau `0xB10C4EA9` → **`0xB10C4EAA`**, post-mortem `0xF00DFA11` → **`0xF00DFA12`**. Une trame écrite par un firmware 2.18.x porte l'**ancienne** métrique : la relire avec la nouvelle grille afficherait un chiffre **faux et crédible**. **Perdre la trace du boot de bascule est acceptable, afficher un chiffre faux ne l'est pas.** Conséquence assumée : la cinétique et le post-mortem du tout premier boot en 2.19.0 sont perdus. Budget RAM RTC inchangé (1073 o sur 8192).
- **Libellés du journal suffixés pour être auto-descriptifs** : le dump de l'anneau et la ligne de post-mortem écrivent `maxB8=` / `free8=` au lieu de `maxB=` / `free=`. En relisant d'anciens logs, un `maxB=` signale sans ambiguïté un firmware **≤ 2.18.9**, donc la métrique surestimée.
- **Nouvelle trace INFO de calibrage**, émise toutes les 5 min par `checkHeapHealth()` — c'est elle qui servira à recalibrer les seuils : `Heap 8bits: libre8=… maxBloc8=… minMaxBloc8=… | interne: libre=… maxBloc=… (octets)`. Les deux familles sont données **côte à côte**, en octets bruts, pour rendre l'écart mesurable. `minMaxBloc8` (le **pire creux depuis le boot**) est mis à jour à **chaque tick** du watchdog heap (60 s, ou 5 s sous seuil d'alerte) et non toutes les 5 min : un creux transitoire entre deux traces serait sinon invisible — or c'est lui qui dimensionne le seuil de redémarrage.
- **`/get-system-info` expose trois champs ADDITIFS** : `free_heap_8bit`, `max_alloc_8bit` et `max_alloc_internal`. `free_heap` et `heap_size` gardent **exactement** leur sémantique : aucun client existant n'est cassé. Budget JSON mesuré avec `measureJson()` sur des valeurs pires cas : **716 octets sur 1024** (628 auparavant).
- **Ce que cette version NE fait PAS, volontairement** : **aucun seuil de `constants.h` n'est modifié**, **aucune des 9 gardes n'est basculée** (auto-guérison, pushes WebSocket, réarmement du parachute OOM, gardes HTTP, garde TLS). Le comportement de l'appareil est **strictement inchangé**. Basculer et recalibrer dans le même cycle exigerait de **deviner** les nouvelles valeurs — le régime 8 bits réel de l'appareil n'a **jamais** été mesuré, et l'auto-guérison est la garde qui **redémarre l'appareil** : un seuil posé à l'aveugle produirait des reboots en cascade sur un système sain qui pilote des pompes doseuses. Mesurer d'abord, décider ensuite.
- **Ce qui arrive en 2.19.1** : après **≥ 24 h de relevés on-target**, recalibrage des seuils puis bascule des gardes **dans l'ordre du risque croissant, auto-guérison en DERNIER**. Règle validée par le gate de sécurité chimique : `kRestartMaxAllocHeapBytes = clamp(0,40 × min(maxBloc8 sur 24 h) ; 8192 ; 16384)`, **débounce maintenu à 3**, invariant d'ordre `restart < rearmOom ≤ kWsPushMinAllocBytes < kLowHeapWarnBytes < min(maxBloc8 sur 24 h)`. Le gate a par ailleurs **refusé** de différer un redémarrage pendant une injection en cours : le PWM des pompes est **matériel**, une boucle figée laisserait la pompe **énergisée** — différer transformerait une dose tronquée (bénigne) en **surdosage potentiel**.
- **Ce qui reste inconnu** : **quel** consommateur épuise la mémoire 8 bits. Pistes de la phase 2 : chemin de service des fichiers statiques (`AsyncStaticWebHandler`, présent dans 2 des 3 crashs), `LittleFS.begin(false)` = **10 fichiers ouverts maximum** par défaut, fuite éventuelle de `File`. Cette recherche n'avait aucun sens tant que l'instrument mentait.

### Fonctionnalités

- **Carte « Infos système » (Paramètres > Système)** : la ligne « Heap libre » devient « **Mémoire utilisable** » et affiche la mémoire **réellement allouable**, au format `<libre> · plus gros bloc : <bloc>`. Le plus gros bloc est montré à côté du total parce que c'est **lui** qui décide si une allocation passe : un total confortable avec un bloc effondré signifie mémoire fragmentée. Une **note discrète** en dessous conserve l'ancien chiffre : « *Mesure brute : X KB, dont une zone technique inutilisable pour les allocations courantes.* » — l'écart entre les deux est **normal** et volontairement rendu visible. La note ne s'affiche que si les deux mesures sont disponibles ; sur un firmware antérieur à 2.19.0, la ligne retombe silencieusement sur la mesure brute (aucun `undefined`).

### Documentation

- **Nouvel [ADR-0038](docs/adr/0038-metrique-memoire-allouable-8-bits.md)** — « La mémoire se mesure sur le pool 8 bits réellement allouable, jamais sur `MALLOC_CAP_INTERNAL` ». **Alternatives écartées** : *garder `MALLOC_CAP_INTERNAL` et relever les seuils* (c'est exactement ce qui a été fait en 2.18.5/2.18.6/2.18.8 — **et c'est ce qui a produit trois crashs** : le problème n'est pas la valeur du seuil, c'est la grandeur comparée) ; *n'exposer que la valeur 8 bits* (l'**écart entre les deux familles EST l'information** — sans lui affiché, un futur cycle rebranchera innocemment `ESP.getFreeHeap()` et le piège se refermera) ; *basculer et recalibrer dans la même version* (exigerait de deviner les seuils d'une garde qui redémarre l'appareil) ; *champ « version de format » plutôt que bump de magic* (une branche de lecture rétrocompatible pour une trame relue **une seule fois** dans la vie du produit). **Verrouillages** : interdiction permanente d'utiliser `ESP.get*Heap()` dans une garde, `heap_metrics` source unique, extension de l'interdiction réentrante au helper, magics RTC réservés (anciens compris), tout changement de métrique d'une zone RTC impose un bump de magic, invariant d'ordre des 5 seuils, et **jamais de report d'un redémarrage d'auto-guérison pendant une injection**.
- **Notes additives datées (2026-08-15)** dans [ADR-0029](docs/adr/0029-auto-guerison-heap.md) (le filet d'auto-guérison n'a jamais pu se déclencher — le mécanisme reste valide, c'est la grandeur mesurée qui était fausse), [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) (les paliers de réarmement 24 Ko / 12 Ko ont été calibrés sur des valeurs terrain surestimées), [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) (les deux seuils heap de la garde TLS sont à refaire — ce qui **renforce** le critère PSRAM fail-closed) et [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md) (le verdict `SUSPICION CORRUPTION` du 13/08 était faux : la comptabilité n'était pas « saine », elle était fausse — la règle, inchangée, redevient exploitable). **Aucun de ces ADR n'est réécrit.**
- `docs/subsystems/oom-guard.md` : avertissement en tête sur l'**état transitoire assumé** de la 2.19.0 (l'anneau mesure en 8 bits pendant que les seuils décident encore sur la mesure brute — c'est volontaire, pas un oubli), bump des deux magics et sa justification, libellés `maxB8=` / `free8=`, verdict du 13/08 corrigé, les 5 seuils marqués « à refaire en 2.19.1 » avec la règle de calibrage.
- `docs/subsystems/system-health.md` : nouvelle section « Trace de calibrage des seuils mémoire » (format exact, tableau des 4 champs, pourquoi `minMaxBloc8` est échantillonné à chaque tick et pas toutes les 5 min, **ce qu'il faut relever on-target**), avertissement clair sur le fait que l'auto-guérison **n'est pas encore basculée**, budget RAM RTC (inchangé, magics bumpés) et règle « changement de métrique ⇒ bump de magic ».
- `docs/API.md` : les 3 champs additifs de `/get-system-info`, tableau des deux familles de mesures, pourquoi les deux sont publiées, consigne d'intégration (superviser `max_alloc_8bit`, jamais `free_heap`), compatibilité ascendante et budget JSON.
- `docs/features/page-settings.md` : ligne « Mémoire utilisable », note sur la mesure brute, affichage conditionnel et repli sur firmware antérieur.

---

## [2.18.9] - 2026-08-14

> 🔄 **Mise à jour du filesystem obligatoire** : ce cycle ne modifie que l'interface web (`data/app.js`). Mettre à jour le **firmware seul ne change rien** — il faut reconstruire et reflasher le filesystem : `./build_fs.sh` puis `./deploy.sh fs` (USB) ou `./deploy.sh ota-fs` (WiFi). Pensez à vider le cache du navigateur si l'ancien affichage persiste.

### Fonctionnalités

- **Mini-graphiques de l'accueil limités à la journée en cours** (`ux-minicharts-journee-en-cours`, demande utilisateur) : les courbes pH, ORP et Température des cartes du tableau de bord affichaient une fenêtre **glissante de 24 h**, donc une partie de la veille en tête de courbe — difficile de lire « où en est-on aujourd'hui ». Elles ne montrent désormais que les points postérieurs à **minuit local**. Le filtrage est fait **côté navigateur** : la requête reste `GET /get-history?range=24h`, **aucun changement de contrat HTTP ni de firmware** ([docs/API.md](docs/API.md) inchangé).
- **Passage de minuit géré tout seul** : le jour civil affiché est mémorisé ; au premier rafraîchissement suivant le changement de date (cadence 5 min), le polling incrémental (`?since=`) laisse place à un **rechargement complet** qui remplace la série. Les points de la veille disparaissent **sans action de l'utilisateur ni rechargement de la page**.
- **Conséquence assumée** : en début de journée les courbes sont **courtes, voire vides entre minuit et le premier relevé**. C'est le comportement demandé, pas un défaut — le repli d'échelle existant (cadre cible ± tolérance) garde un graphique lisible tant qu'aucun point n'est disponible.
- **Inchangé** : les graphiques des écrans détail pH / ORP / Température conservent leurs plages (`Tout` / `30j` / `7j` / `24h`) ; l'infobulle reste **datée et typée** (`jj/mm hh:mm` + mesure / moyenne horaire / moyenne journalière, acquis v2.18.4) ; les **coupures de courbe** aux arrêts de filtration (> 90 min) fonctionnent à l'identique — le filtrage à minuit est appliqué **avant** leur calcul, pour qu'aucun faux trou n'apparaisse en tête de journée.

### Documentation

- `docs/features/page-dashboard.md` : nouvelle section « Plage des mini-graphiques : journée en cours », mécanisme de passage de minuit, cas « début de journée », rappel du reflash du filesystem ; mentions résiduelles des « 24 dernières heures » corrigées.

---

## [2.18.8] - 2026-08-14

> ⚠️ **À lire honnêtement** : ce cycle **n'explique pas** le crash du 13/08. Il corrige deux défauts de l'instrumentation — dont le fait que **le parachute OOM livré en 2.18.5 était inopérant après son premier déclenchement** — et met en place de quoi **trancher** la cause à la prochaine occurrence. La cause racine reste **inconnue**.

### Firmware

- **Le parachute OOM ne se réarmait JAMAIS** (`bug-parachute-oom-jamais-rearme-et-postmortem`, [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)) : 2ᵉ crash de production le 13/08 (v2.18.7, uptime 12,9 h) — abort dans `async_tcp` sur `new AsyncFileResponse` (**156 o**) en servant un fichier statique, puis échec de l'allocation de l'objet exception (**100 o**) → `std::terminate`. Pour que libstdc++ lève, il faut que le `new_handler` se soit **désenregistré** : le réarmement exigeait `maxBloc ≥ 32 768 o` alors que le plus gros bloc **réel** de l'appareil vaut **29 684 o à 33 min** et **27 584 o à 12,9 h** — un seuil **au-dessus du régime normal**, donc une réserve jamais ré-allouée. La protection livrée en 2.18.5 était **one-shot en pratique** ; c'est corrigé : `kOomRearmMinMaxAllocBytes` = **24 576 o** (= 2 × la réserve), plus un **palier de repli** de **2 048 o** armé dès 12 288 o de plus gros bloc (« mieux vaut un petit parachute qu'aucun » : 2 Ko couvrent 8× le duo fautif 156 + 100 o). Surclassement repli → nominal autorisé, **déclassement interdit**. Échelle complète des seuils désormais documentée : 8 192 (auto-guérison) < 12 288 (réarmement repli) < 16 384 (push WS) < 20 000 (WARN heap) < 24 576 (réarmement nominal).
- **Post-mortem d'allocation en RAM RTC** : le CRITICAL « Réserve OOM consommée » étant émis **en différé** par la loop, il est **perdu** quand l'abort suit immédiatement — comme le 13/08. Une **3ᵉ zone RTC noinit** (magic `0xF00DFA11`, **32 octets**) est désormais écrite **par les chemins d'échec eux-mêmes**, en stores bruts alignés : taille du 1ᵉʳ et du dernier échec, `caps`, compteur, uptimes, état du tas comptabilisé, drapeaux, épisodes de réserve. Elle est **dumpée au boot suivant en une ligne unique**, juste après le dump de l'anneau, avec un **verdict** (`FAMINE MÉMOIRE` / `SUSPICION CORRUPTION` / `INDÉTERMINÉ`). Le **drapeau « réserve consommée » est l'octet `flags`** de cette zone : il survit à un abort immédiat. Contrainte structurante : depuis le callback `heap_caps_register_failed_alloc_callback`, **tout appel `heap_caps_*` est interdit** (le verrou du composant heap est déjà détenu par l'allocateur → deadlock) — d'où le **miroir** du dernier échantillon de l'anneau comme seule source « free / maxBloc », à cadence **inchangée** de 2 s et sans aucun parcours de tas supplémentaire. Verdict : famine si plus de 8 échecs **en rafale** (span ≤ 60 s — fenêtre ajoutée en revue, sans elle quelques échecs bénins étalés sur des heures auraient conclu « famine » sur le scénario même à trancher), suspicion de corruption si la comptabilité reste saine (plus gros bloc ≥ 4 × la taille demandée ; le 13/08 : **176×**). Limites assumées et écrites : trace **indicative** (compteurs sans verrou), post-mortem du boot n-2 perdu si un crash survient pendant `setup()`, uptimes tronqués sur 16 bits (repliement toutes les 18 h 12, durées calculées modulo — la validité de la zone ne dépend **pas** de l'ordre des uptimes, un défaut trouvé en revue qui rendait le dump muet au-delà de 18 h).
- **Contrôle d'intégrité périodique du tas** : `heap_caps_check_integrity_all()` toutes les **5 min**, durée attendue **1-3 ms** (framework compilé en `CONFIG_HEAP_POISONING_LIGHT` : parcours des blocs, pas de scan octet par octet), **mesurée à chaque passage**, 1ʳᵉ durée journalisée en INFO, WARN au-delà de 5 ms, **désactivation définitive** au-delà de 20 ms. Détecteur **direct** de l'hypothèse de corruption.
- **Sécurité chimique du contrôle d'intégrité** (gate `pool-chemistry`) : appelé **après** `PumpController.update()` et **jamais** si l'une des **8 gardes** n'est pas remplie — mode maintenance, OTA, dosage pH, dosage ORP, injection manuelle pH, injection manuelle ORP, **pompe énergisée**, uptime minimal. La garde « pompe énergisée » s'appuie sur un **nouvel accesseur en lecture seule** `PumpController::isAnyPumpEnergized()` fondé sur les rapports cycliques réels : les indicateurs de dosage ne couvraient **que la régulation automatique**, une injection manuelle ou un test de pompe passait au travers. Retard maximal admissible établi par `pool-chemistry` : **65 ms** (0,1 mL à 90 mL/min) ; plafond retenu **20 ms** = 0,03 mL. Corruption détectée → `stopAll()` + inhibition **en tête**, puis redémarrage propre via `performFailSafeRestart()` (alerte MQTT `heap_corruption`) ou mode maintenance si le plafond de redémarrages est atteint — **jamais d'`ESP.restart()` nu**.
- **MQTT** : nouvelle alerte `heap_corruption` sur `{base}/alerts` (**type unique** publié par la branche redémarrage propre **et** par la branche mode maintenance).
- **Chiffres** : **507 tests natifs** (+41), builds bi-core verts — core 2 flash 83,2 % / RAM 20,0 %, core 3 flash 93,0 % / RAM 21,1 %. Budget RAM RTC noinit du projet : **1073 o sur 8192** (auto-guérison 9 + anneau heap 1032 + post-mortem 32).

### Documentation

- **Nouvel [ADR-0037](docs/adr/0037-postmortem-oom-et-integrite-tas.md)** — post-mortem d'allocation en RAM RTC + contrôle d'intégrité périodique du tas. Alternatives écartées : **échantillonner le tas toutes les 200-500 ms** (rejetée — `heap_caps_get_largest_free_block` s'exécute interruptions coupées et injecterait de la gigue dans le phénomène mesuré, pour zéro pouvoir discriminant) ; **poisoning comprehensive** (coût prohibitif sur chaque `malloc`/`free`) ; **`ESP.restart()` nu** (viole la sécurité chimique) ; persistance du post-mortem en LittleFS/NVS (impossible depuis le chemin d'échec). Verrouillages documentés : échelle de 5 seuils interdépendants, 3ᵉ zone RTC, interdiction réentrante `heap_caps_*`, subordination du contrôle d'intégrité à la sécurité chimique, contrat MQTT `heap_corruption`. **Note additive datée** dans [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) : le contrat du parachute change (réarmement recalibré, paliers, post-mortem) — la décision d'origine n'est pas réécrite.
- `docs/subsystems/oom-guard.md` : seuil recalibré **avec les mesures qui le justifient** et l'explication du défaut v2.18.5, palier de repli et règles de transition, post-mortem complet (format 32 o, champs, drapeaux, règle exacte du verdict, ligne de dump), contrainte « aucun `heap_caps_*` depuis le chemin d'échec » et miroir qui en découle, les 3 limites assumées, 66 tests dédiés / 507 au total.
- `docs/subsystems/system-health.md` : contrôle d'intégrité (cadence, durée, autolimitation, les 8 gardes chimiques, action sur échec), **budget RAM RTC noinit consolidé des 3 zones**, second dump au boot à côté de celui de l'anneau, alerte `heap_corruption`, constantes `kHeapIntegrity*`.
- `docs/MQTT.md` : alerte `heap_corruption` (condition, messages exacts des deux branches, non-retained, best-effort).

---

## [2.18.7] - 2026-08-13

> Amélioration d'expérience demandée par l'utilisateur après essai réel de la 2.18.6 : le refus fonctionnait, mais il se présentait mal. Aucun changement de la garde mémoire, du contrat HTTP ni des seuils.

### Fonctionnalités

- **Message d'indisponibilité de la mise à jour en ligne reformulé** (`ux-message-psram-mise-a-jour-en-ligne`) : le refus s'affichait « **Erreur vérification mise à jour :** Mise à jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée GitHub impossible). Utilisez la mise à jour par upload de fichier. » Or ce n'est **pas une erreur** mais une caractéristique du matériel, « upload de fichier » n'est pas le vocabulaire de l'interface (qui dit « **Mise à jour manuelle** ») et la mention TLS/PSRAM n'indiquait aucune action. Nouveau texte, identique sur les deux routes : « La mise à jour en ligne n'est pas disponible. Votre matériel ne dispose pas d'une mémoire PSRAM (module ESP32-DevKitC-VE nécessaire). Utilisez la mise à jour manuelle. » Il est affiché **tel quel, sans préfixe**, en toast **informatif** (et non rouge ou orange : rien n'est corrigeable, une tonalité d'échec suggérerait à tort une action). Même traitement sur le bouton d'installation.
- **Indisponibilité annoncée avant le clic** : à l'ouverture de Paramètres > Système, l'interface lit la PSRAM du module ; si elle est absente, le bouton « Vérifier » est **grisé** et une explication permanente s'affiche dans la carte « Mise à jour en ligne (GitHub) », renvoyant vers la carte « Mise à jour manuelle ». Plus besoin de déclencher un refus pour savoir que la méthode n'est pas disponible — le toast ne subsiste qu'en filet de sécurité. Neutralisation **uniquement** si la PSRAM est explicitement annoncée absente (`psram_size = 0`) : si le champ manque, l'interface ne change rien (compatibilité ascendante).

### Firmware

- **Texte du 503 `psram_required` remplacé dans le helper partagé** — donc sur `/check-update` **et** `/download-update`. Le statut **503** et le champ `"code":"psram_required"` sont **inchangés** : le contrat machine est intact, seul le texte destiné à l'utilisateur change. Les logs WARN/INFO restent volontairement techniques (diagnostic on-target). Aucune modification de la garde, des seuils ni de la logique de refus ([ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) inchangé sur le fond).
- **Trois codes machine de refus au lieu d'un seul** (correctif de revue) : `psram_required` est désormais réservé à la PSRAM **réellement absente** (`ESP.getFreePsram() == 0`, indisponibilité **permanente**) ; une PSRAM installée mais **saturée** renvoie le nouveau `psram_low` (« La mise à jour en ligne est momentanément indisponible : la mémoire PSRAM est saturée. Réessayez dans quelques minutes ou utilisez la mise à jour manuelle. », **réessayable**) et le refus heap dégradé porte le nouveau champ `"code":"tls_low_memory"` (texte inchangé : « Mémoire insuffisante pour TLS, réessayez »). Le firmware n'affirme donc plus l'absence de PSRAM sur un module qui en a. La discrimination est faite **dans la route** par un simple test de nullité de la mesure déjà en main : `oomTlsGateReason()`, les seuils, l'ordre de la garde et le parachute OOM sont **intacts**.
- **Côté interface**, le champ `code` de la réponse est désormais propagé sur l'objet `Error` (il était perdu, ce qui empêchait de distinguer une indisponibilité matérielle d'une panne) et le front **ne devine plus rien dans le texte du message** : il lit `code`. Conséquence utilisateur : seul `psram_required` neutralise le bouton et affiche le bandeau permanent ; `psram_low` et `tls_low_memory` affichent un toast informatif **sans** neutraliser (on peut réessayer) puis rafraîchissent l'état via `/get-system-info`, qui reste la source de vérité. Le bandeau `#online_update_unavailable` porte `role="status"` / `aria-live="polite"`. **466 tests natifs**, builds bi-core verts (flash 82,9 % core 2 / 92,7 % core 3), `./build_fs.sh` OK.

### Documentation

- `docs/API.md`, `docs/UPDATE_GUIDE.md`, `docs/subsystems/oom-guard.md` : nouveau texte du 503 partout où il est cité au caractère près, puis **les trois refus 503** (`psram_required` / `psram_low` / `tls_low_memory`) — condition, caractère permanent ou temporaire, message et effet UI — sur `/check-update` **comme** `/download-update`. `oom-guard.md` documente le mapping motif → code (la scission de `kOomTlsGateNoPsram` est faite par la **route**, pas par le prédicat pur). Précision maintenue : le champ `error` est un **texte utilisateur reformulable**, les clients doivent se fier au champ `code`. `UPDATE_GUIDE.md` distingue pour l'utilisateur les trois situations (matériel sans PSRAM = définitif ; PSRAM saturée et mémoire interne dégradée = réessayer) avec le même repli manuel.
- `docs/features/page-settings.md` : état « mise à jour en ligne indisponible » de la carte GitHub (avertissement permanent, boutons désactivés, condition `psram_size === 0`, renvoi vers la mise à jour manuelle, `role="status"` / `aria-live`), plus le tableau des trois codes de refus et de leur effet UI — seul `psram_required` neutralise. `docs/features/README.md` : convention `showToast` — le type `info` sert aussi aux indisponibilités matérielles, message affiché sans préfixe.
- [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) : **deux notes additives datées** (présentation du refus, puis scission en trois codes machine) — la décision technique n'est pas réécrite.

---

## [2.18.6] - 2026-08-13

### Firmware

- **Mise à jour en ligne refusée explicitement sans PSRAM** (`bug-garde-tls-trop-permissive-sans-psram`, [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)) : la garde bicritère de la v2.18.5 laissait passer le régime **normal** d'un WROOM (mesure de production du 12/08 : heap libre=87 708 o, plus gros bloc=34 804 o) ; le handshake TLS échouait ensuite **après** avoir affamé le tas — une allocation de **100 octets** refusée alors que 87 Ko restaient libres, c'est-à-dire la fenêtre exacte du crash `bad_alloc` du 11/08. La garde exige désormais **1 Mo de PSRAM LIBRE** (`kOtaTlsMinPsramBytes`), évaluée **en premier** et fail-closed, avant le bicritère heap (seuils **inchangés**). Le critère porte sur la PSRAM *libre* (`ESP.getFreePsram()`) et non sur la taille installée : une PSRAM saturée ferait retomber mbedTLS sur la RAM interne. Réponse **503** au message lisible : « Mise à jour en ligne indisponible : ce module n'a pas de PSRAM (connexion sécurisée GitHub impossible). Utilisez la mise à jour par upload de fichier. » (`code: "psram_required"`) — au lieu d'un « Erreur HTTP GitHub: -1 » opaque une seconde plus tard. Repli inchangé et pleinement fonctionnel : upload de fichier ou `deploy.sh ota-firmware`.
- **`POST /download-update` protégé lui aussi** : cette route ouvrait la **même** session TLS **sans aucune garde** — un POST direct contournait celle de `/check-update`, avec un flash à la clé. Elle applique désormais le même prédicat, placé **après** la validation url/allowlist/digest et **avant** `WiFiClientSecure`, `setOtaInProgress()`, `LittleFS.end()` et tout effacement de partition : un refus n'a **aucun effet de bord** (pompes non gelées, filesystem non démonté, rien d'effacé).
- **Prédicat partagé à motifs** : `oomTlsGateOk` (booléen) devient `oomTlsGateReason()` → `kOomTlsGateOk` / `kOomTlsGateNoPsram` (refus **définitif**, matériel) / `kOomTlsGateLowMemory` (refus **temporaire**, réessayable) — logique pure, aucune comparaison de seuil dans les handlers. Les logs INFO nominal et WARN de refus mentionnent la **PSRAM libre**, les deux mesures heap, leurs seuils et l'IP client. **466 tests natifs** (+7), builds bi-core verts (flash 82,9 % core 2 / 92,7 % core 3).

### Fonctionnalités

- **Toasts longs enfin lisibles** : un message long (comme le refus de mise à jour ci-dessus, ~220 caractères) débordait du viewport sur une seule ligne. Les toasts reviennent désormais à la ligne (`max-width` 92 vw / 560 px), et leur durée d'affichage s'adapte à la longueur du texte (40 ms/caractère, plancher **3 s** = comportement historique préservé pour les messages courts, plafond 9 s).
- **Toasts d'avertissement visibles** : quatre messages étaient émis en type `warning` (calibrations pH/ORP sans retour, notamment) alors qu'aucun style ne correspondait — ils s'affichaient **transparents/illisibles**. Style orange dédié ajouté.
- **Plus de faux « ✓ Mise à jour terminée »** (défaut pré-existant, rendu atteignable par le 503 ci-dessus) : à l'étape firmware d'une installation depuis GitHub, l'interface traitait TOUTE erreur comme « l'ESP32 a redémarré, donc le flash a réussi ». Un refus explicite du contrôleur affichait donc un succès alors que le firmware n'était pas installé — avec, si l'étape filesystem avait réussi, un filesystem neuf sur un firmware ancien à l'insu de l'utilisateur. L'interface distingue désormais une **réponse d'erreur du serveur** (affichée telle quelle) d'une **vraie coupure réseau** (seul cas où le redémarrage vaut succès).

### Documentation

- **Nouvel [ADR-0036](docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md)** — PSRAM libre requise pour toute session TLS sortante (alternatives écartées : relever le seuil maxBloc à 48-50 Ko, illusoire car la réserve du parachute fait tomber le maxBloc nominal WROOM de ~49 à ~37 Ko ; laisser tenter et rattraper ; garder `/download-update` sans garde). Notes additives datées dans [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md) et [ADR-0032](docs/adr/0032-reponses-http-bornees.md) — décisions d'origine non réécrites.
- `docs/API.md` : 503 `psram_required` de `/check-update` **et** de `/download-update` (jamais documenté jusqu'ici), corps exacts, logs, position de la garde ; la note « limitation connue — core Arduino 3 uniquement » est **rectifiée** (l'attribution réelle, mesurée en production, est l'absence de PSRAM, pas le core Arduino).
- `docs/UPDATE_GUIDE.md` : la mise à jour en ligne est **activement refusée** sans PSRAM sur les deux étapes (message exact + repli) — l'annonce « 503 Mémoire insuffisante pour TLS » était devenue fausse.
- `docs/subsystems/oom-guard.md` : prédicat à motifs, critère PSRAM **libre**, `kOtaTlsMinPsramBytes`, 466 tests.
- `docs/features/README.md` : composant transverse `showToast` documenté (types, durée adaptative, retour à la ligne).

---

## [2.18.5] - 2026-08-12

### Firmware

- **Parachute OOM global** (`bug-oom-accept-http-durcissement`, [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)) : crash de production le 11/08 au soir — `std::bad_alloc` non rattrapé dans `async_tcp` (`new AsyncWebServerRequest`, 356 o, à l'**acceptation d'une connexion HTTP**) après un effondrement du heap de ~60 Ko en ~5 s. L'investigation mesurée a **réfuté** les hypothèses save historique / clients TCP gelés / rafale de page ; la cause du pic reste **non attribuée** (candidat restant documenté dans la spec : buffers RX du driver WiFi en tempête/réassociation — corrèle avec le socket MQTT gelé au crash). Le durcissement est donc **trigger-agnostique** : réserve d'urgence de 12 Ko pré-allouée au plus tôt du boot + `std::set_new_handler` qui la libère à la première famine (le `new` en cours est re-tenté et **réussit** au lieu d'aborter), log CRITICAL différé one-shot par épisode, réarmement à hystérésis (maxBloc ≥ 32 Ko, tick 10 s, critère pointeur — course handler/réarmement fermée en revue), callback IDF diagnostic flag-seul (compte les échecs `malloc` C, WARN throttlé 30 s). Réserve épuisée → comportement d'avant (bad_alloc, coredump en filet ultime) ; le containment de fond reste l'auto-guérison [ADR-0029](docs/adr/0029-auto-guerison-heap.md). Voir [docs/subsystems/oom-guard.md](docs/subsystems/oom-guard.md).
- **Garde mémoire sur `/check-update`** : refus **503** `{"error":"Mémoire insuffisante pour TLS, réessayez"}` AVANT la session TLS (~58 Ko) si heap libre < 65 536 o OU plus gros bloc < 20 480 o — remplace l'échec lent et aléatoire en plein handshake. Le WARN de refus et le log INFO nominal incluent désormais l'**IP du client** (le crash du 11/08 avait un profil TLS compatible mais l'appelant était invérifiable). Note : seuils conservateurs sur module WROVER/PSRAM (buffers mbedTLS > 4 Ko en PSRAM), à réévaluer.
- **Dump du ring heap enrichi** : le WARN d'en-tête « Cinétique heap avant reset » inclut l'uptime absolu du dernier échantillon (mod 65 536 s) et la valeur brute d'`esp_reset_reason` — corrélation temporelle a posteriori avec journal et coredumps.
- **Purge des fichiers ressuscités par OneDrive** : `src/uart_{commands,protocol,transport}.{cpp,h}`, `src/debug_tls_verify.h`, `tools/uart_test.py` (retirés en v2.17.0, réapparus par sync) — le linker les élaguait déjà (`--gc-sections`) ; delta flash net du durcissement ≈ +3,1 Ko. Builds bi-core verts (flash 82,9 % core 2 / 92,6 % core 3), **459 tests natifs** (+18, `test_native_oom_guard`).

### Documentation

- Nouveau [docs/subsystems/oom-guard.md](docs/subsystems/oom-guard.md) : réserve, contrat du handler, réarmement, limites.
- **Nouvel [ADR-0035](docs/adr/0035-parachute-oom-new-handler.md)** — parachute OOM hybride (comportement global d'allocation invisible au point d'appel ; callback IDF seul écarté comme mécanisme principal, retenu en complément flag-seul ; les seuils de garde dépendent de la réserve).
- `docs/API.md` : 503 bicritère de `/check-update` + IP client + note WROVER/PSRAM.
- `docs/subsystems/system-health.md` : ligne de dump du ring enrichie ; renvoi vers oom-guard.

---

## [2.18.4] - 2026-08-10

### Firmware

- **Plus d'agrégat fantôme daté du futur dans l'historique** (`bug-points-futurs-et-ux-courbes`, constaté en production le 10/08 : un point « moyenne journalière » daté du jour courant — normalement impossible — polluait le bord droit des courbes). Cause : des points horodatés dans le **futur** (excursion d'horloge pendant les reboots des essais des 8-9/08) paraissaient vieux de ~136 ans par wrap `uint32` du prédicat d'âge → consolidation immédiate en agrégat journalier daté du futur. Deux gardes : les prédicats d'âge deviennent **future-safe** (`historyIsExpired` pour la purge 90 j, `historyBucketConsolidable` durcie — un point/créneau futur n'est plus jamais « ancien » ; logique pure, l'invariant de wrap d'`isOlderThan` reste inchangé) ; au chargement de `/history.json`, les points horodatés au-delà de `now + 1 h` (`kHistoryFutureToleranceSeconds`) sont **purgés** quand l'horloge est disponible (log INFO du nombre purgé) — le point fantôme disparaît au premier boot. Builds bi-core + 441 tests natifs (+5) verts.
- **Widget Éclairage de l'accueil** : la ligne « Horaires » est masquée quand la programmation est désactivée (elle affichait des horaires inertes sous un mode « Programmation désactivée »).

### Fonctionnalités

- **Tooltip des courbes toujours daté et typé** : le survol d'un point affiche désormais systématiquement la date ET l'heure (`jj/mm hh:mm` — plus d'heure seule pour « aujourd'hui ») ainsi que le type de point (« mesure », « moyenne horaire », « moyenne journalière ») — mini-graphiques du tableau de bord ET écrans détail. Lève l'ambiguïté des étiquettes courtes de l'axe X (inchangées).
- **Mini-graphiques de l'accueil sur 24 h** (au lieu de 3 jours) : lecture directe de la journée écoulée ; les écrans détail conservent leurs plages (`24h`/`7j`/`30j`/`Tout`).
- **Les arrêts de filtration se voient sur les courbes** : plus de 90 min entre deux points consécutifs → la courbe est coupée (trou), y compris lors du rafraîchissement incrémental des mini-graphiques ; sur les écrans détail (1 point/jour), un jour calendaire entièrement absent crée le même trou.

### Documentation

- `docs/subsystems/history.md` : garde anti-futur des prédicats d'âge + purge des points futurs au chargement documentées.
- `docs/features/page-dashboard.md` : mini-graphiques 24 h, tooltip daté/typé, coupures de courbe aux arrêts.
- Pas de nouvel ADR (correctif de robustesse + UX, aucune décision structurante).

---

## [2.18.3] - 2026-08-09

### Fonctionnalités

- **Les graphiques s'affichent correctement sur mobile** (`bug-minicharts-ecretes-mobile`) : sur iPhone, les mini-graphiques du dashboard et les graphiques des écrans détail étaient étirés puis écrêtés en bas de leur carte (axe Y collé au bord). Cause : une règle CSS morte héritée de Chart.js (`canvas { height: 240px !important }` sous 980 px) écrasait la hauteur posée par uPlot — d'où le rendu parfait sur ordinateur. Règles mortes supprimées et `ResizeObserver` existant durci (debounce 100 ms, garde de dimensions, re-synchronisation initiale ; la rotation portrait/paysage est couverte). Frontend uniquement (`data/app.css`, `data/app.js`), aucun changement firmware.

### Documentation

- `docs/features/page-dashboard.md` : comportement responsive des graphiques uPlot documenté (ResizeObserver partagé, interdiction de dimensionner les canvas uPlot en CSS).
- Pas de nouvel ADR (bug d'affichage pur, aucune décision structurante).

---

## [2.18.2] - 2026-08-09

### Firmware

- **Les graphiques s'affichent réellement cette fois** (suite et **fin** de `bug-history-doublons-troncature`). La vraie cause racine de la troncature de `/get-history` — démontrée au flux chunked brut sur l'appareil — est une **erreur de machine à états présente depuis la v2.14.0** (déployée le 05/08) : le cas `kFooter` chargeait le pied `],"count":N}` dans le tampon de report ET passait en phase `kDone` dans la **même itération**, or la condition de boucle sortait avant d'émettre le tampon → **le pied n'était JAMAIS envoyé** → JSON invalide sur **toutes** les réponses → graphiques morts depuis le déploiement v2.14. Fix : la boucle continue tant que le tampon de report n'est pas vide, **même en `kDone`**. Validé sur l'appareil (pied présent, JSON valide).
- **Refactor de l'export `/get-history`** : instantané filtré copié en **une seule** prise de mutex à l'ouverture de la requête (`snapshotFiltered`, timeout `kHistorySnapshotTimeoutMs` = 100 ms ; refus rare → **`503` `{"error":"historique occupé, réessayez"}`**), puis streaming **sans aucun mutex**. Le mécanisme timeout/espaces/`RESPONSE_TRY_AGAIN` introduit en v2.18.1 est **définitivement supprimé** (`getPointByIndex`, `kHistoryPointReadTimeoutMs`, `kHistoryChunkMaxRetries` retirés) — il traitait un symptôme mal diagnostiqué (cf. note sur l'entrée [2.18.1] ci-dessous). Coût borné : ~11 Ko transitoires, libérés en fin de réponse ; plus aucun glissement de buffer possible (instantané figé). Trace de clôture conservée en **DEBUG** (`get-history clos: emis=… o, points=…`). Builds bi-core + 436 tests natifs verts.

### Documentation

- Note corrective ajoutée sur l'entrée **[2.18.1]** ci-dessous : sa « cause B » (espaces de maintien de flux) était un diagnostic erroné.
- `docs/subsystems/history.md` : section export chunked réécrite (`snapshotFiltered`, cause racine C documentée) ; encadré **dette** — extraire la machine à états chunked en logique pure testable (ADR-0017) avec test « le flux se termine par le pied », dont l'absence a laissé passer le bug 5 jours et 2 revues.
- `docs/API.md` : `/get-history` — note « espaces sous contention » retirée (fausse), `503` rare documenté.
- Pas de nouvel ADR (application/correction de mécanismes existants).

---

## [2.18.1] - 2026-08-09

> ⚠️ **Note corrective (v2.18.2)** : le second diagnostic de cette entrée (« Réponse `/get-history` tronquée sous contention » / `RESPONSE_TRY_AGAIN` / espaces de maintien de flux) était **erroné** — la troncature venait d'un pied JSON jamais émis par la machine à états (cause racine corrigée en 2.18.2, présente depuis v2.14.0). Le mécanisme espaces/retries a été **retiré** en 2.18.2. Les deux autres corrections de cette entrée (doublons d'agrégats, capacité RAW) restent valides.

### Firmware

- **Les graphiques (dashboard, écrans pH/ORP) s'affichent à nouveau** (bug `bug-history-doublons-troncature`, constaté en production les 08-09/08 : `/get-history` renvoyait un JSON tronqué → `SyntaxError` côté navigateur → courbes vides). Deux causes racines corrigées :
  - **Agrégats horaires/journaliers dupliqués** : la consolidation créait un agrégat partiel à chaque passage pour le même créneau (jusqu'à 12 doublons/heure au même timestamp) — elle ne convertit désormais un créneau que lorsqu'il est **complet** (un seul agrégat par heure/jour, de meilleure qualité), et les agrégats dupliqués d'un même créneau sont **fusionnés** à la consolidation, au chargement et à l'import.
  - **Réponse `/get-history` tronquée sous contention** : sur timeout du mutex history sans octet en vol, le callback chunked renvoyait `RESPONSE_TRY_AGAIN` que la lib ne re-sonde qu'à l'ack TCP suivant — sans rien en vol, silence → timeout TCP → JSON tronqué sans pied ; il émet désormais **un espace** (légal entre deux éléments JSON) qui maintient le flux vivant, la borne `kHistoryChunkMaxRetries` (clôture propre avec pied) est conservée.
- **Assainissement automatique au premier boot** : les doublons déjà persistés par les versions ≤ 2.18.0 sont fusionnés au chargement de `/history.json` (moyennes équipondérées, **sans effacement de l'historique**) — log INFO `Historique : N agrégats dupliqués fusionnés`.
- **Capacité RAW étendue à 8 h** : `kMaxRawDataPoints` 72 → 96 — nécessaire pour que les points survivent jusqu'à la complétude de leur créneau (~7 h). Budget partition history ([ADR-0033](docs/adr/0033-budget-espace-partition-history.md)) : pire cas 13 → 14 blocs / 16, garde `historySaveAdmit` inchangée. Builds bi-core verts, 436 tests natifs (+8).

### Documentation

- `docs/adr/0033-budget-espace-partition-history.md` : note additive — budget pire cas recalculé 14/16 blocs (`kMaxRawDataPoints` 96). Pas de nouvel ADR (application de mécanismes existants).
- `docs/subsystems/history.md` : consolidation par créneau complet, déduplication (3 points d'appel), capacité RAW 8 h ; **suivi connu** documenté — les agrégats journaliers ne se forment jamais (rétention horaire 7 j < âge de consolidation 15 j, affamage préexistant hors lot).
- `docs/API.md` : des espaces peuvent apparaître dans le flux `/get-history` sous contention (maintien de flux, sans effet pour `JSON.parse`).

---

## [2.18.0] - 2026-08-08

### Firmware

- **Support PSRAM — module WROVER-E** (feature-066) : `-DBOARD_HAS_PSRAM` (+ `-mfix-esp32-psram-cache-issue` / `-mfix-esp32-psram-cache-strategy=memw`, alignés sur les libs précompilées) activé sur les **deux envs** (`esp32dev` core 2 production, `esp32dev_core3` banc core 3). **Binaire unique** WROOM/WROVER : sur un module sans PSRAM, l'init échoue proprement et le firmware continue à l'identique (dégradation gracieuse, validée sur les deux cores).
- **Log de boot PSRAM** : `PSRAM : X Mo détectés (les allocations > 4 Ko y sont placées automatiquement)` ou `PSRAM absente — RAM interne seule (module WROOM)`.
- **Infos système** : champs `psram_size` / `psram_free` ajoutés à `GET /get-system-info` (0 = absente).
- **Levier TLS core 3** : sur module **WROVER-E**, la vérification/installation de mises à jour GitHub (`/check-update`, TLS sortant) **refonctionnera sous core 3** — les allocations > 4 Ko partent automatiquement en PSRAM (`SPIRAM_USE_MALLOC` précompilé), levant la limitation acceptée en v2.16.2 (spec `bug-checkupdate-tls-core3`). Validation à réception du module (procédure de swap : spec feature-066). Builds bi-core + 428 tests natifs verts.

### Fonctionnalités

- **Ligne « PSRAM » dans la card Infos système** (Paramètres → Système) : `libre / totale KB libres` sur module WROVER, « absente » sur module WROOM.

### Documentation

- `docs/API.md` : champs `psram_size` / `psram_free` documentés sur `/get-system-info`.
- `docs/BUILD.md` : flags PSRAM des deux envs (justification `-mfix-*` / libs précompilées).
- `docs/adr/0034-migration-core3-idf5.md` : note additive — levier PSRAM implémenté côté firmware, en attente du module pour validation. Pas de nouvel ADR (mise en œuvre du levier déjà consigné dans ADR-0034).
- `docs/features/page-settings.md` : ligne PSRAM de la card Infos système.

---

## [2.17.1] - 2026-08-08

### Firmware

- **Suppression du réglage « Écran LVGL » devenu sans objet** (suivi de feature-065 / v2.17.0) : card retirée de Paramètres → Avancé (elle était déjà masquée), champ `screen_enabled` retiré du contrat `/get-config` / `/save-config` (un client legacy qui l'envoie encore est ignoré sans erreur), clé NVS plus écrite ni lue (clé orpheline inoffensive sur les appareils existants), champ `authCfg.screenEnabled` supprimé.

### Documentation

- `docs/API.md` : note de compatibilité — `screen_enabled` retiré, champ legacy ignoré depuis v2.17.1.
- `docs/features/page-settings.md` : card « Écran LVGL » documentée comme supprimée.

---

## [2.17.0] - 2026-08-08

### Firmware

- **Retrait complet du support écran externe LVGL / UART2** (feature-065) : l'écran déporté (ESP32 dédié, liaison UART2 sur GPIO 16/17) était un essai **jamais mis en production**. Suppression des trois couches `uart_transport` / `uart_protocol` / `uart_commands` (~1 000 lignes) et de toutes les émissions d'événements dispersées (`main.cpp`, `pump_controller.cpp`, `filtration.cpp`, `sensors.cpp`, `web_routes_calibration.cpp`). **Aucun changement de contrat** HTTP / WebSocket / MQTT — retrait pur, régulation et gardes de sécurité chimique strictement identiques (double GO pool-chemistry, 428/428 tests natifs).
- **Sécurité chimique — fermeture du bypass `pump_test` UART** : la commande écran `run_action/pump_test` appelait `setManualPump()` **sans passer par les gardes** `evaluateManualInject()` (bypass assumé « écran non déployé », condition pool-chemistry #4 de la feature-006). Cette voie non gardée disparaît avec `uart_commands` : toutes les activations manuelles restantes passent par les gardes web.
- **Gains flash** : core 2 (production) **−35,3 Ko** (1 481 445 o, 80,7 % d'app0) ; core 3 (banc) **−30,0 Ko** (1 667 566 o, marge app0 92,3 % → **90,9 %**).
- **GPIO 16/17 libérés** — prérequis matériel du module **WROVER-E (+8 Mo PSRAM)** levé (levier TLS core 3, cf. ADR-0034 et limitation connue v2.16.2).
- **Toggle « Écran LVGL » de Paramètres → Avancé** : la card (déjà masquée) et le champ `screen_enabled` sont **conservés mais inertes** — la valeur reste persistée en config sans aucun effet. Retrait éventuel via une spec de suivi.

### Documentation

- Purge des mentions écran/UART dans `README.md`, `docs/MQTT.md`, `docs/subsystems/` (pump-controller, sensors, filtration, mqtt-manager, measure-validity) ; avertissement « `pump_test` non gardée » de `docs/subsystems/pump-controller.md` historicisé (bypass FERMÉ en v2.17.0).
- `docs/features/page-settings.md` : card « Écran LVGL » documentée comme masquée / toggle inerte.
- `docs/adr/0034-migration-core3-idf5.md` : note additive — GPIO 16/17 libérés, prérequis WROVER-E levé, nouvelle marge app0 core 3. Pas de nouvel ADR (retrait de code mort, décision portée par la spec + ADR-0034).

---

## [2.16.2] - 2026-08-08

### ⚠️ Limitation connue — core Arduino 3 uniquement (banc, pas la production)

- **Sous core 3 / IDF 5, « Vérifier les mises à jour » et « Installer depuis GitHub » sont inopérants** (tout TLS **sortant** depuis l'ESP32) — limitation **acceptée** (décision utilisateur 2026-08-08, spec `bug-checkupdate-tls-core3`). Cause démontrée (pas supposée) : l'empreinte statique d'IDF 5 sur ESP32 classique laisse un plus gros bloc heap contigu de ~43-47 Ko **dès le boot**, insuffisant pour une session TLS sortante (~50 Ko en pointe) — les allocations refusées pendant le handshake remontent en erreurs mbedtls trompeuses (« X509 alloc failed », « certificate verification failed »). GitHub, les certificats embarqués et la crypto matérielle sont hors de cause (validation X509 rejouée sur l'ESP32 lui-même : rc=0). Le remède upstream (`CONFIG_MBEDTLS_DYNAMIC_BUFFER`) n'existe pas dans les cores 3.3.x. **L'échec est propre** : `500` + log détaillé, aucun crash, le reste du firmware n'est pas affecté. **Contournement** : mise à jour OTA par **upload manuel** des binaires de release (`/update`, flux déjà utilisé). **Levier futur** : module WROVER-E (+8 Mo PSRAM) — la config core 3 place déjà les allocations > 4 Ko en PSRAM (seuil 4096, `SPIRAM_USE_MALLOC`) → TLS restauré ; prérequis : libérer les GPIO 16/17 (retrait du code écran UART, feature-065 à venir). La production (core 2) n'est pas concernée.

### Modifié

- **Certificats racines GitHub (`github_root_ca.h`) : les racines USERTrust RSA/ECC passent en variantes auto-signées** (exp. 2038) — les variantes cross-signées par AAA Certificate Services sont retirées (hygiène et pérennité de la chaîne de confiance ; ce n'était pas la cause du bug). Empreintes SHA-256 vérifiées contre les racines officielles Sectigo, sources consignées dans le header. ISRG Root X1 (assets CDN) inchangée.

### Ajouté

- **Diagnostics `/check-update` enrichis** : état heap loggé à chaque vérification (« heap libre=… o, plus gros bloc=… o » **avant** l'ouverture de la session TLS — distingue épuisement total et fragmentation) ; en cas d'échec côté client (code HTTP négatif), le log porte désormais le **détail mbedtls** (`client.lastError()`, message + code) et le libellé `HTTPClient::errorToString()` au lieu d'un « Erreur HTTP GitHub: -1 » muet.

### Documentation

- `docs/adr/0034-migration-core3-idf5.md` : note additive 2026-08-08 — régression TLS sortant sur ESP32 classique ajoutée aux conséquences négatives (acceptation, contournement, levier WROVER-E). Pas de nouvel ADR (conséquence de la migration déjà actée).
- `docs/API.md` / `docs/subsystems/ota-manager.md` : limitation core 3 et nouveaux diagnostics documentés sur `/check-update`.

---

## [2.16.1] - 2026-08-04

### Corrigé

- **La vérification de mise à jour (`/check-update`) ne tombe plus en erreur sur un appareil en fonctionnement prolongé** (constaté en production le 2026-08-04 sur un appareil 2.14.0 : « Erreur HTTP GitHub: -1 » systématique sur heap fragmenté — l'allocation du contexte TLS ~48 Ko échouait avant toute communication — puis « JSON parse error » juste après reboot). Cause : le handler violait le patron [ADR-0032](docs/adr/0032-reponses-http-bornees.md) côté flux **entrant** — la réponse GitHub (~10 Ko, croissante à chaque release) était chargée entière en RAM (`getString()`) puis parsée en document complet (pic ~35 Ko en plus de la session TLS). Correctif : parse **en flux** (`useHTTP10(true)` + `deserializeJson` sur `getStream()` avec filtre 7 champs) et `JsonDocument` sur allocateur **plafonné net** (`BudgetJsonAllocator`, budget `kOtaCheckJsonBudgetBytes` = 8192 o) → pic mémoire ~35 Ko → **≤ 8 Ko** (hors session TLS ~48 Ko, incompressible). Corps de release hors budget → `500` propre « GitHub response too large » + log dédié (chemin d'erreur distinct du « Erreur parsing JSON GitHub », qui porte désormais le détail de l'erreur). Réponse JSON de la route **inchangée** vue du frontend (mêmes champs).
- **Les notes de release tronquées ne produisent plus de JSON invalide côté navigateur** : la coupe à `kOtaReleaseNotesMaxChars` (2048) respecte désormais les frontières UTF-8 (`utf8SafeCutLen`) — un caractère accentué ou un emoji coupé en deux au point de troncature rendait la réponse illisible (symptôme « réponse mal formée » de la 1ʳᵉ tentative post-reboot).

### Ajouté

- **Nouveau header pur `src/ota_check_logic.h`** (`AllocBudget` admit/release, `utf8SafeCutLen` — logique testable en natif, ADR-0017) + suite native `test/test_native_ota_check/` (**+13 tests, 428 au total**).

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : extension 2026-08-04 — le patron couvre désormais aussi les **réponses entrantes** d'API externes (parse en flux + budget dur), dernière ligne « bornage amont » du tableau d'audit soldée. Pas de nouvel ADR (application du patron existant).
- `docs/API.md` : `/check-update` — contrat inchangé, nouveau cas d'erreur 500 « GitHub response too large », troncature UTF-8 sûre.
- `docs/subsystems/ota-manager.md` : parse en flux borné de la réponse GitHub Releases.

---

## [2.16.0] - 2026-08-04

### Firmware

- **Alerte MQTT « pompe hors service »** (feature-064 — exigence pool-chemistry P1, condition de la porte de bascule core 3, ADR-0034) : si une pompe doseuse a échoué son init PWM au boot et a été mise en sécurité fail-closed (`pumpUsable`, feature-063 — **core Arduino 3 uniquement**, impossible en production actuelle core 2), une alerte `pump_unusable` est désormais publiée **par pompe**, en français (« Pompe 1 (pH) HORS SERVICE : échec init PWM (GPIO=25, canal LEDC=0) — aucune injection possible jusqu'au reboot »), sur le canal d'alertes existant `{base}/alerts`. Le CRITICAL local ne reste plus la seule remontée : Home Assistant voit la pompe hors service au lieu de « compter » des cycles sans injection réelle (dérive pH/ORP silencieuse). Re-publication **à chaque (re)connexion MQTT** (arbitrage Q1, décision pool-chemistry) : l'alerte est non-retained et l'état permanent jusqu'au reboot — un broker absent au boot ne la perd plus. Nouvel accesseur lecture seule stricte `PumpController.isPumpUsable(index)` (aucun setter, reboot requis pour réarmer ; core 2 : toujours utilisable). Aucun chemin d'injection modifié — visibilité uniquement (double GO pool-chemistry). Code dormant en production, actif sur le banc `esp32dev_core3` et après bascule.

### Documentation

- `docs/MQTT.md` : type d'alerte `pump_unusable` (condition, re-publication à chaque connexion, exemple de message, limitation HA assumée).
- `docs/subsystems/pump-controller.md` : accesseur `isPumpUsable()` (sémantique bi-core, fail-closed hors bornes) + exigence P1 marquée livrée.
- `docs/subsystems/mqtt-manager.md` : hook de re-publication dans `connectInTask()` (arbitrage Q1, outQueue, code dormant core 2).
- `docs/adr/0034-migration-core3-idf5.md` : item « alerte MQTT pompe inutilisable » de la porte de bascule coché (livré feature-064).

---

## [2.15.0] - 2026-08-04

### Firmware

- **Code bi-core core 2 / core 3** (feature-063, exécution de l'étude feature-062) : le firmware compile désormais à l'identique sur core Arduino 2.0.17 (production) ET core Arduino 3.x / ESP-IDF 5.x, via gardes `#if ESP_ARDUINO_VERSION_MAJOR >= 3` dans 3 fichiers — LEDC pompes doseuses (`ledcAttachChannel`/`ledcWriteChannel`, 20 kHz/8 bits/canaux strictement identiques, échec d'attache → CRITICAL + pompe inutilisable fail-closed `pumpUsable`, double GO `pool-chemistry`), watchdog (`esp_task_wdt_config_t` + `esp_task_wdt_reconfigure`), mbedtls sha256 (macros de compat 2.28/3.x) ; + include explicite `WiFiClient.h` dans `web_routes_ota.cpp` (include transitif disparu en core 3). **Le comportement production est strictement inchangé — prouvé à l'octet près** (binaire core 2 identique à la baseline, delta ±16 octets = artefact `__TIME__`) ; 415/415 tests natifs.
- **Nouvel environnement de banc `esp32dev_core3`** (pioarduino, BUILD-ONLY — jamais flashé) : compile SUCCESS avec le fork ESP32Async épinglé 3.4.10/3.11.2 (compatibilité core 3 désormais **prouvée**) — Flash 1 693 142 o = 92,27 % d'app0 (marge 141 866 o), RAM 69 248 o, versions résolues pioarduino 55.3.37 / core 3.3.7 / IDF 5.5.0. `default_envs = esp32dev` et `deploy.sh` inchangés ; la bascule production reste une porte conditionnelle (ADR-0034).
- **Retrait des flags lwIP inopérants** `-DCONFIG_LWIP_MAX_SOCKETS=24` / `-DCONFIG_LWIP_TCP_MAXRTX=5` de `platformio.ini` : flags « fantômes » jamais appliqués (lwIP précompilé dans le core — 16 sockets / 12 retransmissions effectifs depuis toujours). Retrait sans effet binaire, prouvé.

### Documentation

- `docs/adr/0034-migration-core3-idf5.md` (**nouveau** — ex-DRAFT numéroté et accepté) : décision de migration en deux phases, résultats réels du banc, check-list de la porte de bascule production (banc matériel L3, épinglage pioarduino sur release taguée, alerte MQTT pompe inutilisable, hors saison, 14 j, binaires archivés + sha256).
- `docs/adr/0011-mqtt-task-dediee.md` : note corrective « effet fantôme » (les flags lwIP n'ont jamais été appliqués — le comportement réseau réel n'a jamais été celui documenté en IT3).
- `docs/adr/0031-*.md` / `docs/adr/0033-*.md` : notes « défense CONSERVÉE » (déclencheur littlefs div/0 corrigé sous IDF 5 → défenses dormantes après bascule, PAS retirées — décision utilisateur du 2026-08-03).
- `docs/BUILD.md` : section « Les deux environnements firmware » + pièges pioarduino (pio du penv, `pio pkg install`, épinglage requis).
- `docs/subsystems/pump-controller.md` : section « Mise en sécurité `pumpUsable` » (chemin core 3 uniquement).

---

## [2.14.0] - 2026-07-30

### Corrigé

- **Crash WS récurrent : l'angle mort de la garde heap v2.11.1 est comblé** (2ᵉ coredump `bad_alloc` dans `textAll`, 30/07, v2.13.0 — garde **passée** puis prise de vitesse). Deux failles fermées : (1) **TOCTOU** — entre le contrôle `getMaxAllocHeap()` et l'allocation du buffer de trame, la tâche `async_tcp` peut consommer le bloc mesuré → `try/catch (std::bad_alloc)` autour des 3 `textAll` (filet absolu : l'exception part de `makeSharedBuffer` **avant** toute mutation de file du fork → toute course résiduelle = push sauté compté dans `skippedPushCount()`, plus jamais d'`abort()`) ; (2) **seuils adjacents** — la garde WS (8192) était **égale** au seuil de restart auto-guérison (8192), créant une zone grise 8-16 Ko où les pushes passaient sans que l'auto-guérison s'arme → `kWsPushMinAllocBytes` relevé à **16384** (marge 2×) : les pushes s'arrêtent avant la zone rouge, l'auto-guérison garde sa fenêtre d'action.

### Modifié

- **`/get-history` passe en flux chunked zéro-buffer** (ADR-0032 mécanisme (b), patron `/download-logs`) : supprime **la plus grosse allocation contiguë du firmware** — la `String` ~40 Ko épinglée pendant tout le transfert à chaque ouverture du dashboard (générateur de fond G3 de l'audit churn-heap du 24/07, différé « si récidive » — récidive constatée). Chaque point est copié sous mutex **court** (25 ms, `getPointByIndex`) ; contention (consolidation en cours) → `RESPONSE_TRY_AGAIN` borné (~200 retries consécutifs, reset après succès — tolère ~100 s vs ~1,5 s de pire consolidation), au-delà : clôture **propre** (JSON toujours valide, moins de points — jamais tronqué syntaxiquement). **Contrat JSON octet pour octet inchangé** (y compris la clé `count` émise deux fois — bizarrerie historique conservée pour compat) ; le 503 « heap fragmenté » disparaît de cette route ; UI intouchée (`data/` inchangé, chunked transparent pour `fetch()`). Post-revue : seul le token `range` **reconnu** est échoé dans l'en-tête JSON (valeur inconnue → traitée et échoée `all`).

### Ajouté

- **Buffer statique de trame WS** (`sWsFrameBuf`, `kWsFrameBufBytes = 3072`, réservé au link en `.bss` **hors heap** — pattern « réserver une fois, réutiliser toujours ») : plus aucune `String` temporaire par push (churn G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). Trame trop grande → push **sauté** (jamais tronqué) + compteur `oversizedFrameCount()` + WARN throttlé dédié. Invariant documenté : buffer réservé à `loopTask`. `kWsSensorJsonReserveBytes` supprimée.
- **Télémétrie heap post-crash** : ring de 128 entrées × 8 o en RAM RTC noinit (1032 o vérifiés à l'ELF, magic `0xB10C4EA9` distinct de l'anti-boucle auto-guérison), échantillonné toutes les **2 s** depuis la loop (uptime, maxBloc, heap libre, clients WS, delta de pushes sautés). Au boot après un reset ≠ POWERON, les **32 dernières entrées (~64 s de cinétique d'effondrement)** sont dumpées dans le journal (~8 lignes) puis le magic est invalidé. Limites assumées : dump aussi sur reboot volontaire (`SW_RESET` inclut OTA/manuel — bruit accepté, trié par le motif en en-tête) ; lignes en RAM/Serial flushées ensuite (perdues si re-crash immédiat).
- **2 nouvelles suites de tests natives** : `test_native_history_export` (cutoff `range` + format point JSON purs) et `test_native_heap_ring` (encodage 8 o, index du ring, âges modulaires) — **+27 tests, 415 au total**.

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : **extension 2026-07-30** — `/get-history` migre du patron (c) au patron (b), tableau d'audit soldé (G3 traité) ; durcissement WS (seuil 16384, catch, buffer statique) ; pas de nouvel ADR (application des mécanismes déjà actés).
- `docs/subsystems/ws-manager.md` : garde 16384 (zone grise), filet `bad_alloc`, buffer statique + invariant `loopTask`, `oversizedFrameCount()`.
- `docs/API.md` : `/get-history` chunked (contrat inchangé, comportement sous contention, double clé `count` documentée, fin du 503).
- `docs/subsystems/history.md` : `getPointByIndex` (25 ms), export chunked, glissement toléré, note `migrateLegacyHistory`.
- `docs/subsystems/system-health.md` : ring de télémétrie heap (format, cadence, dump, invalidation du magic, limites).

---

## [2.13.0] - 2026-07-29

### Ajouté

- **Les mesures pH/ORP/température ne sont plus présentées comme valides ni historisées quand l'eau ne circule pas** (feature-061). Les sondes sont montées dans le circuit de filtration : filtration arrêtée = eau stagnante autour des sondes → valeurs dérivantes **non représentatives du bassin** (plateaux/dérives nocturnes qui polluaient les courbes). Désormais : valeurs pH/ORP/température à « `--` » sur le tableau de bord et les pages dédiées avec un **badge bleu informatif** « Filtration arrêtée » (nouvelle variante `info` — état normal « attendre », distinct du rouge « Capteur indisponible » = « agir » ; « Signal filtration périmé » en ambré si le signal externe est stale) ; points d'historique enregistrés avec pH/ORP/température `null` → **trous dans les courbes** (état filtration et flags dosage toujours enregistrés ; pas de purge rétroactive des anciens points). À la **reprise** de la filtration, un **délai de purge de 5 min** (réutilise `stabilizationDelayMin`, badge « Stabilisation : ~N min » recalculé à chaque push WS) laisse l'eau stagnante de la chambre de mesure se renouveler — délai complet appliqué aussi au boot avec eau présente (conservateur). Source de décision unique : nouveau module `measure_validity` (logique pure + coquille avec cache portMUX) branché sur `resolveWaterPresence()` (feature-056) ; champs nouveaux WS + `/data` : `measures_valid`, `measures_stab_remaining_s` (+ `water_present` ajouté à `/data`). Chaîne de mesure, calibration, onglet Avancé, dosage (déjà gardé sans eau) et MQTT/HA **inchangés**. +14 tests natifs (388 au total).
  - **Itération 2 — extension à la température** : la sonde DS18B20 est dans la canalisation → à l'arrêt, l'eau du tuyau dérive vers la température ambiante du local (c'est la température du local qui était enregistrée, pas celle du bassin). Même drapeau `measures_valid` (pas de validité séparée : le délai de purge de 5 min est conservateur pour la température — l'eau du tuyau se renouvelle en < 1 min — mais une validité distincte ajouterait de la complexité pour un gain nul) ; carte/compact température à « `--` » + mêmes badges que pH/ORP ; `NAN` dans l'historique → trous dans la courbe température. Le **mode Auto de la filtration n'est pas impacté** (il lit `sensors.getTemperature()` en direct), MQTT/UART écran inchangés (live).

### Documentation

- `docs/subsystems/measure-validity.md` (**nouveau**) : règles de décision, contrat des 3 champs, concurrence (cache portMUX), logs ; itération 2 : périmètre étendu aux 3 mesures, justification « même drapeau », non-impact du mode Auto filtration.
- `docs/subsystems/history.md` : exclusion pH/ORP — puis température (itération 2) — sans circulation (NaN → `null`, agrégation, pas de purge rétroactive).
- `docs/subsystems/ws-manager.md` + `docs/API.md` : champs `measures_valid` / `measures_stab_remaining_s` / `water_present` (`/data`), note de repli HTTP (`/data` sans `filtration_state_stale` — asymétrie préexistante).
- `docs/features/page-dashboard.md`, `page-ph.md`, `page-orp.md` : badges (libellés exacts, hiérarchie danger > warning > info), valeurs `--`, trous dans les courbes, dé-duplication dosing-status ; itération 2 : carte température (badge composé, `--`, courbe avec trous).
- Pas d'ADR (confirmé architect) : politique de présentation/persistance sans contrainte structurante — la source unique de présence d'eau est déjà actée par l'ADR-0026, le pattern module pur + coquille par l'ADR-0017.

---

## [2.12.0] - 2026-07-29

### Modifié

- **Limite de cycles de dosage par jour (garde 9 anti-cycling) : défaut 20 → 48, et désormais réglable 10-60** (feature-060). Motivation : incident du 2026-07-29 — régulation ORP bloquée en début d'après-midi (« Limite cycles/jour atteinte — reprise à minuit ») avec seulement **104 mL** injectés. En régime d'entretien post-remontée du stabilisant, la régulation travaille par petites retouches (~5 mL/cycle, pause mélange 20 min) : un régime **sain** qui produit naturellement 20-40 cycles/jour et épuisait la limite de 20 — non fondée chimiquement, alors que l'ORP dérivant tout l'après-midi est lui-même un risque sanitaire. Nouveau défaut **48** (1 cycle/30 min moyen, sous le plafond physique ~70/j imposé par la pause mélange), exposé dans Paramètres → Régulation pH / ORP (champ « Cycles de dosage max / jour (par pompe) », `max_cycles_per_day` dans `/get-config` / `/save-config`). **Bornes dures 10-60** (`kCyclesPerDayMin`/`kCyclesPerDayMax`) **clampées côté serveur** (fonction pure `clampCyclesPerDay`) et au chargement NVS (clé `max_cycles_day`, migration ≤ 2.11.x automatique) ; warn journalisé sur valeur hors bornes et sur toute modification. Anti-rafale (6/min, 20/15 min), limites volumétriques/horaire et toutes les autres gardes **inchangées** ; le compteur du jour n'est pas remis à zéro à la modification. **Double validation `pool-chemistry` (2026-07-29)** : GO pré-design sous conditions (toutes implémentées) + GO sur re-validation du diff. +6 tests natifs (374 au total).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Limite de cycles/jour configurable » (fondement du défaut 48, bornes, double clamp serveur/NVS, warns de traçabilité, verdicts pool-chemistry).
- `docs/API.md` : champ `max_cycles_per_day` (`/get-config`, table de validation `/save-config` — clamp silencieux 10-60 + warn).
- `docs/features/page-settings.md` : nouveau champ de l'onglet Régulation pH / ORP ; `maxCyclesPerDay` retiré du tableau des paramètres non exposés.
- Pas d'ADR : paramétrage borné d'une garde existante, décision et alternatives tracées dans la spec + verdicts pool-chemistry (précédent : feature-025, `orpStartThreshold`).

---

## [2.11.1] - 2026-07-28

### Corrigé

- **Crash/reboot `bad_alloc` dans les broadcasts WebSocket sur heap fragmenté** (coredump du 2026-07-28, v2.11.0, décodé GDB) : `AsyncWebSocket::textAll()` alloue son buffer de trame (`makeSharedBuffer`) **sans rattraper** `std::bad_alloc` → plus gros bloc libre < ~1,8 Ko = `abort()` et reboot brutal (sans arrêt ordonné des pompes ni sauvegarde des cumuls). Correctif : **garde heap** `_heapAllowsPush()` en tête des 3 broadcasts, **avant** toute construction de JSON — plus gros bloc < `kWsPushMinAllocBytes` (8 Ko, aligné sur le seuil de restart auto-guérison) → push **sauté** (l'UI rate un cycle de 5 s, sans conséquence), compteur atomique de pushes sautés (`skippedPushCount()`) + WARN throttlé 30 s émis uniquement depuis `update()` (invariant anti-ré-entrance : le helper ne logge jamais). Extension de la philosophie ADR-0032 (réponses bornées) à la voie WebSocket.
- **Clients WebSocket zombies qui épinglaient la mémoire** (cause n°1 identifiée par l'investigation churn-heap) : un client qui n'ACK plus (téléphone verrouillé, WiFi en veille) conservait connexion ouverte une file de **32 buffers ~1,8 Ko** (~58 Ko épinglés en blocs dispersés) — de quoi effondrer le plus gros bloc en 2-3 min, plus vite que la fenêtre 60 s de l'auto-guérison. Correctifs : file d'envoi par client bornée à **5 trames** (`-DWS_MAX_QUEUED_MESSAGES=5`) + `setCloseClientOnQueueFull(true)` — le fork ferme lui-même le client saturé **sous son verrou** (la boucle manuelle sur `getClients()` a été rejetée en revue : itération sans verrou = UB face à l'érase concurrent d'`async_tcp`) ; + PING keepalive 30 s (ne couvre que les zombies inactifs, le PING n'étant envoyé que file vide — la saturation couvre les autres). En complément : `reserve` du JSON `sensor_data` 1600 → **2048** (`kWsSensorJsonReserveBytes`, payload mesuré 1777 o) — supprime ~6 réallocations de String par push.

### Modifié

- **L'auto-guérison heap réagit en ~15 s au lieu de ~3 min** : le bloc heap de `checkSystemHealth()` est extrait vers `checkHeapHealth()`, sur un timer propre à **cadence adaptative** (fonction pure `heapSampleIntervalMs()`, `self_heal_logic`) — 60 s en régime sain, **5 s** dès que le plus gros bloc passe sous le seuil d'alerte (20 Ko) → le débounce (3 ticks) confirme avant que le crash ne gagne la course. Invariants ADR-0029 strictement conservés (seuils, débounce, budget de reboots, mode maintenance, garde OTA) ; **+5 tests natifs** (368 au total).
- **Alerte `low_memory` (heap total) émise sur transition** au lieu d'être ré-émise à chaque tick de 60 s (chaque CRITICAL re-flushait le disque) : une alerte à l'entrée sous le seuil, une INFO « Mémoire rétablie » au retour au-dessus. Limite assumée : alerte MQTT non-retained, un abonné hors ligne pendant la transition rate l'épisode (borné par l'INFO de rétablissement).

### Documentation

- `docs/adr/0029-auto-guerison-heap.md` : **amendement** « échantillonnage adaptatif » (pas de nouvel ADR — la décision est inchangée, seule la fréquence d'observation évolue).
- `docs/adr/0032-reponses-http-bornees.md` : **extension** aux émissions WebSocket (garde côté appelant, buffers de file bornés).
- `docs/subsystems/ws-manager.md` : garde heap, récolte des zombies, `skippedPushCount()` ; **code mort signalé** : `broadcastLog()`/`setLogCallback()` jamais branchés depuis 2.7.0 (aucun message WS `type:"log"` n'a jamais été émis — purge reportée).
- `docs/subsystems/system-health.md` : `checkHeapHealth()` séparée, cadence adaptative, alerte `low_memory` en transition.

---

## [2.11.0] - 2026-07-27

### Corrigé

- **Cause racine des « corruptions » récurrentes de la partition historique** (2 incidents en 14 jours, boot-loop `IntegerDivideByZero` dans littlefs) : ce n'était pas une corruption mais une **saturation structurelle d'espace** — au pire moment, la réécriture copy-on-write de `/history.json` (ancien + nouveau fichiers coexistants) plus le journal et son fichier de rotation demandaient **19-22 blocs sur les 16 que compte la partition de 64 Ko**. Le manque d'espace déclenchait alors un bug littlefs amont répertorié (division par zéro dans `lfs_alloc` sur système de fichiers plein — [littlefs #1063](https://github.com/littlefs-project/littlefs/issues/1063)), non corrigeable localement (version figée par la plateforme). Correctif structurel (**ADR-0033**) : `saveToFile` mesure désormais le JSON, vérifie l'espace disponible (garde pure `historySaveAdmit`, marge de 2 blocs), **supprime l'ancien fichier avant d'écrire** (pic copy-on-write divisé par deux) et **vérifie l'écriture** (un fichier partiel est supprimé, jamais laissé sur le disque) ; en cas d'espace insuffisant, le save est sauté proprement (ancien fichier préservé, nouvelle tentative 5 min plus tard) — l'ancien code tronquait l'historique dès l'ouverture, avant toute vérification. Les plafonds du journal passent de 16/12 Ko à **8/6 Ko**. **Nouveau pire cas : 13 blocs sur 16, marge garantie de 3 blocs** — la partition ne peut structurellement plus se remplir.
- **Anti-tempête de flush** : les échecs de sauvegarde de l'historique sont journalisés en WARNING throttlé (1/h) au lieu d'ERROR — un ERROR déclenchait un flush disque immédiat… sur le système de fichiers déjà plein, toutes les 5 minutes.

### Ajouté

- **Préservation de preuve dans le disjoncteur de boot-loop** (feature-059) : avant l'effacement automatique de la partition, les 2 premiers blocs (8 Ko de métadonnées littlefs) sont dumpés en hexadécimal sur le port série (`[HISTDUMP]`, ~2,4 s, watchdog rafraîchi) et une empreinte courte (CRC32 des 2 blocs + 16 premiers octets) est conservée en NVS — de quoi discriminer, si une occurrence survenait encore, la saturation d'une éventuelle corruption mémoire persistée (hypothèse résiduelle de l'investigation).
- **Instrumentation de la persistance** : occupation de la partition journalisée (throttlée 1/h), compteur de saves refusés (`nospc_cnt`) et horodatage du dernier save réussi en NVS (écrits uniquement sur échec, pour préserver la flash NVS).

### Documentation

- `docs/adr/0033-budget-espace-partition-history.md` : budget d'espace chiffré avant/après, trade-off remove-avant-write (fenêtre de perte bornée à 5 min, couverte par le disjoncteur) vs write-tmp-puis-rename écarté (pic ×3), bug amont documenté.
- `docs/subsystems/system-health.md` (préservation de preuve), `docs/subsystems/history.md` (garde d'espace, instrumentation), `docs/subsystems/logger.md` (nouveaux plafonds 8/6 Ko).

## [2.10.5] - 2026-07-27

### Corrigé

- **Valeurs pH/ORP/température figées à l'écran quand un capteur devient indisponible** : sonde débranchée → le firmware invalide la lecture (NaN) et publie `null` (`/data` + WebSocket), mais l'UI conservait indéfiniment la **dernière valeur connue** (pattern « skip si null ») sur le tableau de bord et les widgets compacts, sans aucune indication d'indisponibilité. Désormais, **convention unique** : toute valeur capteur `null` s'affiche « `--` » sur **tous** les sites d'affichage numérique — métriques du tableau de bord (`#m-temp`, `#m-ph`, `#m-orp`), widgets compacts (`#compact-ph`, `#compact-orp`, `#compact-temperature`, unité conservée), sections détaillées — alignée sur les pages Réglages/Calibration qui faisaient déjà le bon geste. En complément, la carte **Température** gagne le badge « **Capteur indisponible** » (`#temperature-sensor-badge`), calqué sur les badges pH/ORP de la feature-022 (même garde anti-faux-positif au boot : aucun badge avant le premier message WS). Capteur rebranché → l'affichage reprend automatiquement, sans recharger la page. Graphiques et pages Réglages inchangés (sauter les `null` y est correct : trous dans les courbes). Frontend uniquement (`data/app.js`, `data/index.html`), aucun fichier firmware touché.

---

## [2.10.4] - 2026-07-24

### Corrigé

- **Crash/reboot au chargement du journal sur heap fragmenté** : `GET /get-logs` sérialisait jusqu'à 200 entrées de log (20-40 Ko) dans le `cbuf` **contigu** d'`AsyncResponseStream` (`resize()` = `new char[]` throwant) → `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot**. **2ᵉ occurrence de la classe en 2 jours** (coredump du 2026-07-24, décodé GDB — même signature que `/debug/sensor_trace`, corrigé en 2.10.1) : la garde v2.8.2 à seuil heap FIXE (`kMinHeapForLogsResponse` = 24 Ko, indépendant de la taille réelle) et son commentaire « realloc, jamais d'abort » sont **invalidés**. Correctif **systémique** après audit exhaustif des routes à réponse bufferisée (toutes les autres prouvées bornées), règle générale gravée dans l'**ADR-0032** :
  - **`/get-logs` paginé** : curseur `?since=` réutilisé, page de 30 entrées (hard cap 60 — la queue d'entrées partageant la même milliseconde que la dernière incluse est embarquée, sinon le curseur strictement supérieur les perdrait entre deux pages), champ `truncated` dans la réponse (`{uptime_ms, logs[], truncated}`), page construite en **une passe sous le mutex logger**, `measureJson` + marge `kHttpHeapMarginBytes` (4 Ko) vs `ESP.getMaxAllocHeap()` → **503** propre ou stream **pré-dimensionné exact**. Échappement manuel supprimé (délégué à `serializeJson`). **UI** : boucle de pagination transparente dans `loadLogs()` (max 8 itérations, curseur strictement croissant, compatible ancienne API sans `truncated`).
  - **`/download-logs` en flux chunked zéro-RAM** (patron `/coredump/download`) : fichier `/system.log` lu par blocs puis entrées RAM une à une (nouveau `Logger::getLogEntryByIndex`, best-effort sur timeout mutex), report de ligne entre chunks via module pur `logs_export_chunk.h` (garantie de progression), lignes > 256 o tronquées « … ». Garde heap supprimée (inutile en chunked) — plus de 503 ni de troncature d'export sous heap fragmenté.
  - **`/get-history`** : `String::reserve(points × kHistoryPointJsonMaxBytes(128) + 128)` dont le retour `bool` EST le test du bloc contigu — échec → **503** (jamais de JSON tronqué silencieux). Pire cas du point JSON **mesuré** (113-120 o, constante 96 → 128 post-revue).
  - **`/check-update`** : release notes GitHub (non bornées) tronquées à `kOtaReleaseNotesMaxChars` (2048) + « … ».
  - `Logger::forEachRecentLog` : le visiteur retourne un `bool` (`false` = arrêt anticipé — une page = une prise de mutex) ; `kMinHeapForLogsResponse` **supprimée**, `kHttpHeapMarginBytes` (4096) généralisée (alias `kSensorTraceHeapMarginBytes` conservé) ; commentaires mensongers v2.8.2 corrigés. Aucun chemin de dosage touché.

### Ajouté

- **2 nouvelles suites de tests natives** : `test_native_logs_page` (admission de page pure `logs_page.h`) et `test_native_logs_export` (report de ligne `logs_export_chunk.h`) — **21 tests**, modules purs pattern ADR-0017, **353 tests natifs au total**.

### Documentation

- `docs/adr/0032-reponses-http-bornees.md` : **nouvel ADR** — toute réponse HTTP pouvant dépasser ~4 Ko doit être (a) paginée + pré-dimensionnée exactement, (b) chunked sans accumulation RAM, ou (c) allocation unique testée (`reserve` bool) avec 503 propre ; **gardes à seuil heap fixe interdites**. Tableau d'audit des routes (corrigées + prouvées sûres), 3 patrons de référence.
- `docs/API.md` : contrats mis à jour — `/get-logs` (pagination, `truncated`, 503), `/download-logs` (chunked, limites best-effort), `/get-history` (503 possible), `/check-update` (`release_notes` tronqué).
- `docs/subsystems/logger.md` : signatures `forEachRecentLog` (visiteur `bool`) et `getLogEntryByIndex` (best-effort), section gardes heap réécrite (suppression `kMinHeapForLogsResponse`).
- `docs/features/page-settings.md` : chargement paginé transparent de la card Logs + export chunked.

---

## [2.10.3] - 2026-07-23

### Corrigé

- **Libellé de log de reconnexion WiFi en mode AP secours** : en AP+STA, le log périodique (30 s) affichait « tentative de reconnexion (n/3) » avec un compteur jamais réinitialisé (ex. « 2881/3 » après une journée) ; le message devient « WiFi déconnecté, tentative de reconnexion STA (AP secours actif) » sans compteur — le « (n/3) » reste affiché uniquement en mode STA, où il arme réellement la bascule AP secours au 3ᵉ échec (`src/main.cpp`, aucune logique modifiée).

---

## [2.10.2] - 2026-07-23

### Modifié

- **Trace heap périodique retirée du journal** : la trace INFO `Heap: libre=… maxBloc=… minLibre=…` (instrumentation v2.9.1, émise toutes les 5 min ou sur chute significative de `maxBloc`) n'est plus émise — réduction du bruit : le journal persisté de 64 Ko ne stocke plus cette télémétrie de routine. Le bloc d'émission est **mis en commentaire** dans `checkSystemHealth()` (`src/main.cpp`), ré-activable en le décommentant pour un futur diagnostic. **Détection et alertes inchangées** : l'échantillonnage de `maxAllocHeap` alimente toujours l'auto-guérison (ADR-0029) et le flush one-shot ; le WARN « Heap fragmenté sous seuil d'alerte » et le CRIT « Mémoire faible » restent actifs.

### Documentation

- `docs/subsystems/system-health.md` et `docs/subsystems/logger.md` : trace INFO heap marquée désactivée (2.10.2) avec procédure de ré-activation ; détection/alertes documentées comme inchangées.

---

## [2.10.1] - 2026-07-23

### Corrigé

- **Crash/reboot à l'ouverture de l'onglet Avancé sur heap fragmenté** : `GET /debug/sensor_trace` sérialisait les **300 échantillons** du ring buffer de trace en une seule réponse (~20 Ko JSON) dans le `cbuf` **contigu** d'`AsyncResponseStream` — sur heap fragmenté (le plus gros bloc libre peut descendre sous 8 Ko, cf. ADR-0029, dont le seuil d'auto-guérison ne protégeait pas cette allocation de ~20 Ko), `std::bad_alloc` non rattrapé sur la tâche `async_tcp` → `abort()` → **reboot**, déclenché involontairement depuis le navigateur à chaque activation de l'onglet Avancé. Diagnostic établi par **décodage GDB du coredump** du 2026-07-23 (v2.10.0). Correctif :
  - **Route paginée** : `GET /debug/sensor_trace?offset=N&count=M`, page bornée à `kSensorTraceMaxSamplesPerPage = 50` (~3,5 Ko), défauts sûrs sans paramètres (première page), **clamp silencieux** hors bornes (jamais de 400 — calcul pur `computeTracePage()` dans `src/sensor_trace_page.h`), réponse enrichie (`count` total, `offset` effectif, `returned`).
  - **Garde heap** : `measureJson` + marge `kSensorTraceHeapMarginBytes = 4096` comparés à `ESP.getMaxAllocHeap()` → **503** `{"error":"heap fragmenté, réessayez"}` au lieu d'un crash, même pour une page ; stream **pré-dimensionné** (une seule allocation exacte, zéro `resize()`).
  - **UI onglet Avancé** : agrégation **transparente** des pages avant tracé (borne dure 12 itérations), message français sur 503 (« Mémoire fragmentée, réessayez. ») — rendu inchangé vu de l'utilisateur.
  - Contrat **lock-free** du ring buffer inchangé (`SensorManager::appendSensorTraceJson(JsonArray, offset, count)`) ; glissement du ring toléré entre deux pages (~1 échantillon max, route de diagnostic). Aucun chemin de dosage touché.

### Ajouté

- **Nouvelle suite de tests native** `test_native_trace_page` : 16 tests du clamp pur de pagination (`sensor_trace_page.h`, pattern ADR-0017 — invariants `offset ≤ total`, `n ≤ max`, `offset + n ≤ total` pour tout input, y compris `SIZE_MAX`) — **332 tests natifs au total**.

### Documentation

- `docs/API.md` : contrat paginé complet de `GET /debug/sensor_trace` (paramètres, défauts, clamp, champs de réponse, 503, note sur le glissement du ring).
- `docs/subsystems/sensors.md` : signature paginée `appendSensorTraceJson`, section « Pagination », constantes `kSensorTraceMaxSamplesPerPage` / `kSensorTraceHeapMarginBytes`, renvoi vers `sensor_trace_page.h`.
- `docs/features/page-settings.md` : pagination transparente + message 503 sur la card « Signaux capteurs ». Pas d'ADR : correctif local suivant un pattern existant (logique pure ADR-0017, garde heap ADR-0029), aucune alternative structurante verrouillée.

---

## [2.10.0] - 2026-07-22

### Corrigé

- **Boot-loop sur partition history corrompue nécessitant une récupération USB** : une corruption des métadonnées littlefs de la partition `history` provoquait un **panic CPU non rattrapable** (`IntegerDivideByZero` dans `lfs_alloc`) à la **première écriture** dans `/system.log` — LittleFS monte un filesystem corrompu **sans erreur** puis panique à l'écriture, et le garde-fou `.fscheck` existant (2 octets, stockés *inline*) n'exerçait jamais le chemin fautif. Le crash survenant **avant la montée du WiFi**, l'appareil bouclait indéfiniment : incident réel du 2026-07-22 (v2.9.2), **appareil injoignable pendant 1 semaine**, récupéré manuellement en USB (`esptool erase_region 0x3E0000 0x10000`). **5ᵉ mode de défaillance « appareil injoignable » traité** (après 2.8.2, 2.9.0, 2.9.1 et 2.9.2).
- **Lignes série entrelacées entre tâches** : l'émission console du logger passait par 4 `Serial.print` séparés hors mutex — deux tâches (mqttTask core 0 / loopTask core 1) pouvaient entrelacer préfixe et message (observé sur bench). La ligne est désormais composée dans un buffer pile (`kLoggerConsoleLineMax = 512`) et émise en **un seul `Serial.write` atomique** (troncature avec fin de ligne garantie). Format inchangé. Le « spam » de 6 286 lignes `sensor_problem → ON` observé le même jour a été démontré être un **rejeu matériel du FIFO UART0** (bench), pas un défaut logiciel — le dédup MQTT est correct et inchangé (investigation complète : `specs/features/todo/bug-spam-sensor-problem-serial.md`).
- **Course use-after-free potentielle sur les topics MQTT lors des reconnexions** (bug latent, découvert par analyse statique pendant l'investigation ci-dessus) : à **chaque tentative de connexion** au broker, `refreshTopics()` réassignait les ~90 `String` de topics depuis mqttTask, pendant que loopTask pouvait lire ces mêmes topics en enfilant un message sortant — une réassignation de `String` libère/réalloue son buffer, un lecteur concurrent pouvait donc déréférencer un pointeur libéré (topic corrompu publié, corruption heap silencieuse, crash apériodique — d'autant plus probable que le broker flappe). `refreshTopics()` lit désormais la config sous mutex, **ne reconstruit que si le préfixe a réellement changé** (early-return sinon), et la reconstruction comme la lecture cross-task sont sérialisées par un nouveau `topicsMutex` (timeout → message droppé + WARN throttlé, l'état retain est republié au cycle 10 s suivant). **Bénéfice collatéral** : plus **aucune** réallocation de topics sur reconnexion sans changement de config — supprime le churn heap de ~90 `String` toutes les 5 s sur broker instable. Le changement de préfixe MQTT à chaud reste fonctionnel (appliqué à la reconnexion qui suit le save). Aucun chemin de dosage touché.

### Ajouté

- **Auto-guérison de la partition history (disjoncteur de boot-loop)** : l'appareil **se répare désormais tout seul** quand la partition `history` est corrompue au point de faire paniquer littlefs au boot. Détection **inter-boot** : un compteur `crash_cnt` (NVS, namespace `hist_meta`) est posé juste avant le montage et remis à zéro après init complète ; si le boot précédent a crashé pendant cette fenêtre **2 fois consécutives** (`kHistoryInitCrashThreshold`), la partition est **effacée automatiquement** avant remontage (log CRIT), puis le boot se termine normalement — l'historique repart de zéro, l'appareil reste joignable (**disponibilité > données**, trade-off assumé). Seuls les resets « crash » comptent (panic, watchdogs) ; POWERON, reset logiciel/OTA et **brownout** (cause électrique, pas FS) réarment sans compter. Indépendant du budget anti-boucle `kAutoHealMaxReboots` et du mode maintenance : le disjoncteur **ne redémarre jamais**, il répare puis laisse booter.
- **Alerte MQTT `history_partition_reset`** : publiée (différée, après l'init MQTT) quand la partition a été effacée automatiquement — l'effacement est traçable dans HA.
- **Nouvelle suite de tests native** `test_native_history_guard` : 24 tests de la logique pure du disjoncteur (`history_guard_logic`, pattern ADR-0017).
- **Nouvelle suite de tests native** `test_native_mqtt_topics` : 27 tests de la normalisation pure du préfixe de topics MQTT (`mqtt_topics_logic`, extraite de `refreshTopics()` pour le correctif de course ci-dessus — trim, `/` finaux, fallback `pool/sensors`, troncature) — **316 tests natifs au total**.

### Documentation

- `docs/adr/0031-disjoncteur-bootloop-history.md` : **nouvel ADR** (détection inter-boot NVS + effacement au seuil de 2 ; alternatives écartées : test d'écriture élargi — impossible face à une exception CPU non rattrapable, validation du superblock — fragile aux internals littlefs, RAM RTC — effacée au power-on alors que la corruption FS y survit ; trade-off disponibilité > historique).
- `docs/subsystems/system-health.md` : 3ᵉ volet d'auto-guérison (disjoncteur history : machine drapeau/compteur NVS, mapping des reset reasons, alerte différée, récupération manuelle de secours).
- `docs/MQTT.md` : alerte `history_partition_reset`.
- `docs/subsystems/logger.md` : nouvelle section « Émission console » (write unique atomique) + note sur les logs pré-persistance (le CRIT du disjoncteur n'atteint jamais `/system.log`).
- `docs/adr/README.md` : index complété (ADR-0030, absent de l'index depuis la v2.9.2, et ADR-0031).
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Reconstruction des topics — `refreshTopics()` + `topicsMutex` » (contrat de concurrence : writer unique mqttTask, lecture cross-task `enqueueOutbound()` seule, `configMutex`/`topicsMutex` jamais imbriqués, mutex non-récursif ; guard no-op ; module pur `mqtt_topics_logic` ; mode dégradé sur timeout) + note sur le changement de préfixe à chaud dans la section feature-018. Pas d'ADR : correctif local dans le modèle producer/consumer existant (ADR-0011), aucune alternative structurante verrouillée.

---

## [2.9.2] - 2026-07-15

### Corrigé

- **Gel de la pile réseau nécessitant une coupure secteur** : après l'auto-guérison mémoire (2.9.1), un **nouveau** gel « appareil injoignable » est survenu — mais cette fois **avec un tas parfaitement sain** (plus gros bloc contigu ≈ 37 Ko, jamais proche du seuil de 8 Ko). Diagnostic : c'est la **pile réseau lwIP elle-même qui se fige** (serveur web, mDNS et même le ping ICMP tous morts) alors que le WiFi reste **associé**. Le thread `tcpip`/lwIP n'est **pas** surveillé par le watchdog matériel, et `loopTask`/`mqttTask` continuaient de tourner → ni la TWDT, ni l'auto-guérison mémoire (2.9.1) ne pouvaient l'attraper. C'est le cousin « en gel » du crash `tcp_recved` corrigé par la migration fork (2.9.0). **4ᵉ mode de défaillance « appareil injoignable » traité.**

### Ajouté

- **Auto-guérison réseau (watchdog de connectivité)** : l'appareil **se répare désormais tout seul** aussi quand c'est la **pile réseau** qui se fige, et plus seulement la mémoire — **plus besoin de couper le secteur**. Une **sonde ICMP active** pinge la passerelle toutes les 30 s (`kNetworkProbeIntervalMs`, timeout 2 s) : un echo reply prouve que toute la pile réseau (réception → lwIP → émission) fonctionne. Ce signe de vie est **indépendant de MQTT** (qui peut être désactivé). Réponse **graduée** : si le WiFi est associé mais la pile muette depuis **2 min** (`kNetworkStallSoftMs`), une **reconnexion WiFi** est tentée (`WiFi.disconnect()/reconnect()`, alerte MQTT `network_soft_recover`) ; si le gel persiste **5 min** (`kNetworkStallRebootMs`), un **redémarrage propre fail-safe** a lieu (mêmes garanties que l'auto-guérison mémoire : pompes coupées, cumuls sauvegardés, puis reboot ; alerte MQTT `network_stall_reboot`).
- **Anti-faux-positif** : la décision est **verrouillée** sur l'état du WiFi. Une coupure WiFi légitime, un broker MQTT en panne ou un reboot de la box **ne déclenchent rien** — l'horloge de gel est gelée tant que le WiFi n'est pas associé. Seul un vrai gel de la pile *alors que le WiFi tient* déclenche une action.
- **Budget anti-boucle partagé** : les deux auto-guérisons (mémoire + réseau) partagent le **même compteur** de reboots (3 max sur 15 min, `kAutoHealMaxReboots`). Après 3 reboots cumulés (heap et/ou réseau), l'appareil bascule en **mode maintenance** (pompes OFF, dosage inhibé, joignable) au lieu d'enchaîner. Conséquence assumée : un épisode combinant fragmentation mémoire **et** gel réseau bascule en maintenance plus tôt (comportement conservateur volontaire).

> ⚠️ **Correctif de containment, pas de cause racine** : le blocage réside dans AsyncTCP/lwIP (piste racine identifiée : churn WebSocket/HTTP), à traiter ultérieurement. Le watchdog de connectivité sort l'appareil du gel automatiquement en attendant.

### Documentation

- `docs/adr/0030-watchdog-connectivite.md` : ADR de la décision (sonde ICMP vers la passerelle plutôt que dépendance MQTT, réponse graduée soft→hard, budget anti-boucle partagé avec l'auto-guérison mémoire ; alternatives écartées).
- `docs/subsystems/system-health.md` : extension avec le watchdog de connectivité (sonde ICMP passerelle, machine à états `net_health`, réponse graduée soft→hard, budget partagé heap+réseau, refactor `performFailSafeRestart`, module pur `net_health_logic`, constantes).
- `docs/MQTT.md` : alertes `network_soft_recover` et `network_stall_reboot`.

---

## [2.9.1] - 2026-07-15

### Corrigé

- **Gel récurrent nécessitant une coupure secteur** : l'appareil pouvait se **figer** (web injoignable, écriture des logs arrêtée) tout en restant sous tension — seule une coupure/remise sous tension le débloquait. Contrairement aux crashes précédents (`std::bad_alloc` sur `/logs` en 2.8.2, assert lwIP WebSocket en 2.9.0), il n'y avait **ni panic, ni coredump, ni reboot automatique** : les tâches surveillées par le watchdog (loop, MQTT) restaient vivantes, donc le chien de garde ne se déclenchait jamais. Cause diagnostiquée : **épuisement / fragmentation du tas** — le plus gros bloc contigu libre s'effondre alors que le heap total reste correct, ce qui asphyxie le serveur web et la persistance des logs. Amplifié par un WiFi instable (double NAT via prise CPL → IP qui saute), qui rejouait `MDNS.end/begin` à chaque oscillation d'adresse et accélérait la fuite mémoire. **3ᵉ mode de défaillance « appareil injoignable » traité.**
- **Ré-annonce mDNS en boucle sur réseau instable** : `poolcontroller.local` n'est plus ré-annoncé à chaque oscillation d'IP. La ré-annonce est **debouncée** (au plus une fois toutes les 30 s, `kMdnsReannounceMinIntervalMs`) et n'a lieu que si l'IP a **réellement** changé, ce qui supprime le churn `MDNS.end/begin` en roaming CPL/mesh.

### Ajouté

- **Auto-guérison mémoire** : l'appareil **se répare désormais tout seul** en cas de fragmentation critique du tas. Quand le plus gros bloc contigu libre reste sous 8 Ko (`kRestartMaxAllocHeapBytes`) pendant 3 vérifications de santé consécutives (≈ 3 min) et après au moins 3 min de fonctionnement, il effectue un **redémarrage propre** : coupure des pompes, sauvegarde des cumuls journaliers de dosage (anti-sur-dosage), log critique, alerte MQTT `auto_reboot`, puis reboot. Plus besoin de couper le secteur à la main.
- **Mode maintenance anti-boucle** : si l'auto-guérison redémarre **3 fois** (`kAutoHealMaxReboots`) dans une fenêtre glissante de 15 min (compteur en RAM RTC survivant au reboot), l'appareil **cesse d'enchaîner les reboots** et bascule en mode maintenance : pompes coupées, **dosage inhibé sur toutes les voies** (auto, programmé, manuel web, manuel écran/UART, test), auto-guérison désactivée, alerte MQTT `maintenance_mode`. L'appareil **reste joignable** pour diagnostic. Sortie du mode maintenance : redémarrage manuel du contrôleur.
- **Instrumentation du tas** : à chaque vérification de santé (60 s), une ligne INFO persistée `Heap: libre=… maxBloc=… minLibre=…` permet de visualiser la fragmentation. Un flush disque **one-shot** est déclenché au premier franchissement à la baisse du seuil d'alerte (`kLowHeapWarnBytes` = 20 Ko) pour capturer la descente même si le redémarrage n'est pas atteint.

### Documentation

- `docs/adr/0029-auto-guerison-heap.md` : ADR de la décision (auto-reboot sur fragmentation du tas + mode maintenance anti-boucle plutôt que subir le gel ; alternatives écartées ; dépendance au réglage réseau et au pulldown de gate MOSFET).
- `docs/subsystems/system-health.md` : nouvelle fiche (santé système, instrumentation heap, machine à états auto-guérison / maintenance, module pur `self_heal_logic`, debounce mDNS, constantes).
- `docs/subsystems/pump-controller.md` : `setMaintenanceInhibit()` / `isMaintenanceInhibited()` et inhibition du dosage en mode maintenance.
- `docs/subsystems/logger.md` : instrumentation heap et flush one-shot dans `checkSystemHealth()`, debounce de la ré-annonce mDNS.
- `docs/MQTT.md` : alertes `auto_reboot` et `maintenance_mode`.

---

## [2.9.0] - 2026-07-14

### Corrigé

- **Crash / reboots liés aux clients WebSocket** : l'appareil pouvait redémarrer de façon récurrente (`panic → reboot`) et devenir **injoignable** en HTTP/WebSocket jusqu'au redémarrage, notamment lors de la **fermeture ou du timeout d'un client WebSocket** (onglet fermé, WiFi qui décroche, client inactif). Un coredump a identifié un **bug de course connu** des bibliothèques web asynchrones **originales** (`me-no-dev`, non maintenues) : un assert de la pile réseau lwIP au moment du `close()`. C'est la **2ᵉ cause de crash de la pile web** diagnostiquée par coredump après le `std::bad_alloc` sur `/logs` corrigé en 2.8.2.

### Modifié

- **Migration vers le fork maintenu ESP32Async** (ex-mathieucarbou), avec **versions épinglées** : `AsyncTCP@3.4.10` et `ESPAsyncWebServer@3.11.2` (avant : dépôts Git `me-no-dev` non figés). Le fork corrige le bug de course à la source et rend le build reproductible. **Aucun changement de comportement fonctionnel** côté utilisateur (API, UI, MQTT, OTA inchangés) : seule la stabilité s'améliore. Plateforme figée inchangée (`espressif32@6.9.0`). `ESPAsyncWiFiManager` conservé (le portail WiFi est géré nativement par le projet).

### Documentation

- `docs/adr/0028-migration-fork-esp32async.md` : ADR de la décision (migration + épinglage des versions, alternatives écartées, dette de suivi des releases ESP32Async).
- `docs/subsystems/ws-manager.md` : dépendance au fork ESP32Async (versions), mode de défaillance historique (assert lwIP au close/timeout WS) et sa correction.

---

## [2.8.5] - 2026-07-13

### Amélioré

- **Statut de dosage — cause anti-cycling visible** : le widget pH/ORP affichait un statut **vide** quand le dosage était bloqué par une garde anti-cycling (**cycles/jour** atteint, **anti-rafale**), ce qui était ambigu (« ça n'injecte pas et rien n'est indiqué »). Le statut remonte désormais la **cause de refus du firmware** (`ph/orp_DoseBlockedReason`, déjà diffusée) : « Limite cycles/jour atteinte — reprise à minuit », « Anti-rafale — patienter… ». Un statut vide signifie maintenant sans ambiguïté que la mesure est **dans l'hystérésis** (rien à doser). Purement UI.

---

## [2.8.4] - 2026-07-13

### Amélioré

- **Latch d'instabilité EMI pH/ORP auto-levé** : le drapeau qui marque un capteur `unstable` (inhibant la régulation) quand la dispersion de la fenêtre de mesure dépasse le seuil sur 3 fenêtres consécutives (bruit EMI soutenu) était **collant** — seul un `reset()` (reboot ou `POST /debug/sensor_filter_reset`) ou une calibration le levait. Il s'auto-lève désormais après **12 fenêtres pleines propres consécutives** (`kSensorFilterLatchClearCycles`), soit ≈ 2 min de signal réellement propre (60 s + purge de la fenêtre médiane). Répond au cas « **ORP marqué instable alors que l'oscillation est retombée** » sans nécessiter de reset manuel après un burst passé. Asymétrie voulue : rapide à poser (≈ 15 s), lent à lever (anti-flapping).
- **Anti-flapping garanti** : la « clean streak » qui pilote l'auto-levée n'est incrémentée que sur une fenêtre **pleine et sans aucune perturbation** ; **tout** rejet (spike dur isolé, NaN, hors-plage) **ou** tout dépassement de dispersion la remet à 0. Un capteur qui crache des spikes ne peut donc jamais lever le latch à tort. `reset()` reste la levée manuelle immédiate ; les inhibitions indépendantes (`unstable()` sur dispersion courante, rejets consécutifs, ready/frozen) sont inchangées.
- **Logs de transition** (`SensorManager`) : `warning` « Filtre pH/ORP : latch EMI posé… » à la pose, `info` « … latch EMI levé (signal stable)… » à l'auto-levée (une ligne par transition, par capteur).

### Documentation

- `docs/subsystems/sensors.md` : auto-levée du latch EMI (`kSensorFilterLatchClearCycles`, clean streak, logs de transition), accesseur `dispersionCleanStreak()`.
- `docs/adr/0027-filtre-tendance-centrale.md` : note sur l'évolution du latch (auto-levée, dans le cadre de l'ADR).

---

## [2.8.3] - 2026-07-13

### Corrigé

- **Injection manuelle — estimation de durée trompeuse** : le champ d'injection affichait la durée brute pour le volume demandé (ex. « ≈ 35 min » pour 1 L) sans tenir compte du **plafond de 10 min par injection** (`kManualInjectMaxDurationS`) appliqué par le firmware. L'estimation reflète désormais le plafond : au-delà du volume délivrable en une injection, elle affiche la durée réelle (10 min) **et** le volume effectif (« plafonné à ~N mL par injection, répéter pour le reste »). Purement UI ; le plafond de sécurité est inchangé.

---

## [2.8.2] - 2026-07-13

### Corrigé

- **Crash de l'endpoint `/logs`** : la lecture des logs (`/get-logs`, `/download-logs`) plantait par `std::bad_alloc` (PANIC → reboot) sous fragmentation du tas — l'appareil devenait alors injoignable en HTTP alors que MQTT continuait de fonctionner. Cause : `Logger::getRecentLogs()` deep-copiait tout le buffer (avec ses `String`) dans un `std::vector<LogEntry>`, `operator new` levant une exception non rattrapée. La méthode est **supprimée** et remplacée par `Logger::forEachRecentLog()` (itération **en place sous mutex, sans copie**) ; les handlers sont réécrits en **réponse flux** (`beginResponseStream`, dont le buffer grandit par `realloc` — ne lève jamais). Diagnostiqué via coredump.
- **Garde mémoire basse** : nouvelle constante `kMinHeapForLogsResponse` (24 Ko) ; si le plus gros bloc contigu libre est en dessous, `/get-logs` et `/download-logs` renvoient **503** (`{"error":"low_memory"}` / message texte) sans tenter d'allocation.

### Retiré

- **Log de diagnostic `[Diag]`** périodique (introduit temporairement en 2.8.1) — sa raison d'être (diagnostic du crash `/logs`) est levée. La ré-annonce mDNS sur changement d'IP (2.8.1) et le getter `WsManager::clientCount()` sont **conservés**.

### Documentation

- `docs/API.md` : code 503 `low_memory`, comportement flux et limitation de troncature de `/get-logs` et `/download-logs`.
- `docs/subsystems/logger.md` : `forEachRecentLog` (contrat mutex / itération sans copie), garde heap, fix `bad_alloc` ; retrait de la doc du log `[Diag]`.

---

## [2.8.1] - 2026-07-12

### Corrigé

- **mDNS périmé après roaming** : `poolcontroller.local` est désormais ré-annoncé sur **tout changement d'IP** (événement WiFi `STA_GOT_IP`, drapeau consommé dans `loopTask`), y compris lors d'un roaming CPL/mesh **sans** perte de WiFi. Avant, la ré-annonce n'avait lieu que sur reconnexion manuelle → l'appareil pouvait être injoignable en HTTP (IP périmée annoncée) alors que MQTT continuait de fonctionner. Une déconnexion invalide l'IP mémorisée. Log INFO `mDNS ré-annoncé sur <IP> (<raison>)`.
- **Divergence valeur pH/ORP MQTT vs tableau de bord** : les topics MQTT `{base}/ph` et `{base}/orp` publiaient la valeur **brute** alors que le tableau de bord affiche la valeur **filtrée** (lissée) — écart visible sous bruit de mesure (ex. pH 6.39 sur MQTT vs 7.3 dans l'app). Les deux publient désormais la **valeur filtrée identique** (repli sur le brut au démarrage tant que le filtre n'est pas amorcé). Les valeurs brutes restent disponibles sur `{base}/ph_raw` et `{base}/orp_raw`.

### Ajouté

- **Diagnostic** : log INFO périodique (toutes les 60 s) `[Diag] heap=NNNNN min=NNNNN IP=x.x.x.x wsClients=N` (heap libre, min free heap, IP courante, nombre de clients WebSocket) pour diagnostiquer une panne « web injoignable » en distinguant roaming réseau et fuite heap. **Temporaire** — à retirer une fois la cause tranchée. Nouveau getter `WsManager::clientCount()`.

### Documentation

- `docs/subsystems/logger.md` : documentation du log `[Diag]` (format, but, périodicité, caractère temporaire) et de la ré-annonce mDNS sur changement d'IP.

---

## [2.8.0] - 2026-07-11

### Firmware

- **Filtre de mesure pH/ORP recalé sur la tendance centrale (feature-058)** : le filtre (entrée du PID) ne **colle** plus au haut ou au bas d'une oscillation soutenue et ne **gèle** plus sur une valeur trompeuse. Nouvelle chaîne : rejet des **vraies aberrations uniquement** (NaN / hors-plage / spike dur isolé) → **médiane sur fenêtre longue** (13 échantillons ≈ 65 s) → EMA (α pH 0,10 / ORP 0,08). La valeur filtrée est **recalculée en continu**, jamais figée.
- **Inhibition de la régulation conservée mais découplée** : `isPhFilterUnstable()` / `isOrpFilterUnstable()` bloquent toujours le dosage sous bruit extrême via un **drapeau de dispersion** (amplitude `max−min` de la fenêtre > cap, OU latch EMI soutenu, OU rejets consécutifs), sans jamais fausser ni figer la valeur publiée. Invariant iso-sécurité préservé (valeur ∈ [min, max], jamais tirée vers la consigne).
- **Latence +15 s** (fenêtre médiane 7 → 13) — négligeable devant la pause mélange hydraulique post-injection. Module `sensor_filter.*` rendu 100 % pur (testé en natif, 218 tests).
- Suppression de la mécanique re-sync / saut-max / warmup à seuil court (constantes `kSensorFilterMedianWindow`, `kPhFilterMaxStep`/`kOrpFilterMaxStep`, `kSensorFilterWarmupSamples`, `kSensorFilterResyncRejects`, `kSensorFilterMaxResyncPerWindow`, `kSensorFilterResyncWindowMs` retirées).

> ℹ️ Ce filtre livre une valeur **honnête et centrée** malgré le bruit ; le **vrai correctif de la source du bruit reste matériel** (isolation / pompe), traité séparément.

### Documentation

- `docs/adr/0027-filtre-tendance-centrale.md` : nouvel ADR (successeur de la stratégie feature-025) — estimateur central robuste + découplage valeur/latch, alternatives écartées, invariant iso-sécurité.
- `docs/subsystems/sensors.md` : section filtrage réécrite (chaîne, contrat `filtered()`, dispersion/latch découplés, paramètres, latence).
- `docs/API.md`, `docs/features/page-ph.md`, `docs/features/page-orp.md` : références aux constantes supprimées corrigées.

---

## [2.7.0] - 2026-07-11

### Ajouté

- **Trace des signaux capteurs brut / lissé (feature-057)** : nouvelle card « Signaux capteurs (brut / lissé) » dans les réglages **Avancé**, avec 2 graphes haute cadence (pH brut vs lissé, ORP brut vs lissé), stats min/max/Δ, compteur d'échantillons et boutons « Recharger la trace » / « Vider le buffer ». Permet de diagnostiquer visuellement le **bruit** ou une **oscillation** de la chaîne de mesure. Les données proviennent d'un ring buffer firmware de 300 échantillons (~7,2 Ko RAM) alimenté à chaque cycle capteur (~5 s) — bien plus fidèle que l'échantillonnage WebSocket. La trace est chargée à l'activation de l'onglet (pas de polling), et le buffer est volatil (perdu au reboot).
- Réintroduction **fidèle** (et **étendue à l'ORP**) de l'outil « Debug oscillation pH » retiré en v2.5.0 (feature-045). Les outils « pause WiFi » et « diagnostic EZO » **ne sont pas** réintroduits.

### Firmware

- **Capteurs** : ring buffer de trace `SensorManager` (`kSensorTraceBufferSize = 300`), enregistrement lock-free en fin de `_readEzoSensors()` — aucune transaction I²C ajoutée, watchdog non impacté.
- Nouveaux endpoints `GET /debug/sensor_trace` (dump JSON non bloquant, streamé) et `POST /debug/sensor_trace_clear`.

### Documentation

- `docs/API.md` : documentation des routes `/debug/sensor_trace` et `/debug/sensor_trace_clear`.
- `docs/features/page-settings.md` : card « Signaux capteurs (brut / lissé) » de l'onglet Avancé.
- `docs/subsystems/sensors.md` : section « Trace des signaux capteurs » (ring buffer, coût RAM, contrat lock-free).

---

## [2.19.1] - 2026-07-09

### Ajouté

- **Pause mélange visible sur le tableau de bord** : le widget pH/ORP affiche désormais la **pause mélange hydraulique** post-injection (« Pause mélange : X min YY s »), au même titre que la stabilisation. Auparavant ce délai (20 min ORP / 15 min pH) n'apparaissait que dans le modal « État filtre », ce qui donnait l'impression d'un dosage bloqué sans explication. Nouveaux champs WebSocket `ph_mix_remaining_s` / `orp_mix_remaining_s`.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Cycle de régulation : chronologie stabilisation + pause mélange » (diagramme temporel + cadence de régulation ~1 injection / 20 min en ORP).
- Échos utilisateur dans `docs/features/page-orp.md`, `page-ph.md`, `page-dashboard.md`.

## [2.19.0] - 2026-07-08

### Ajouté

- **Mode d'installation (feature-056)** : nouveau réglage unique décrivant le câblage réel du PoolController, dans une section « Installation » dédiée (1er onglet des Paramètres) et une étape de l'assistant de 1ᵉʳ lancement. Trois choix :
  - **PoolController pilote la filtration** — alimentation permanente + sortie 12 V vers contacteur ; le contrôleur commande le relais, la programmation de filtration est active, le dosage est autorisé quand la filtration commandée est en marche.
  - **Alimenté par le circuit de filtration** — le contrôleur est alimenté en même temps que la pompe ; il ne pilote pas le relais et l'eau est présumée présente en permanence.
  - **Filtration externe signalée** *(nouveau cas)* — le contrôleur est alimenté en permanence et une domotique tierce lui signale l'état de la filtration via `POST /filtration/external-state` ou MQTT `.../filtration_external_state/set`. Sans signal « en marche » récent (au-delà de 3 min), l'eau est considérée absente et **le dosage est suspendu** (sécurité).
- Signalement de l'état de la filtration externe depuis Home Assistant (interrupteur) et sélection du mode d'installation (liste déroulante, libellés en français).
- **Aide intégrée au mode « Filtration externe signalée »** : bloc repliable dans les Paramètres avec des exemples prêts à l'emploi (commande HTTP `curl`, publication MQTT, automatisation Home Assistant), renseignés automatiquement avec l'adresse et le topic MQTT réels du contrôleur.

### Modifié

- La garde de sécurité « présence d'eau » du dosage repose désormais sur une résolution unique selon le mode d'installation (comportement inchangé pour les installations existantes, migration automatique).
- Le mode d'installation remplace et fusionne les anciens réglages « Fonctionnement Piloté/Continu » (section Régulation pH/ORP) et « filtration gérée par le PoolController ».

### Migration

- Mise à jour automatique de la configuration : « Piloté » → *PoolController pilote la filtration*, « Continu » → *Alimenté par le circuit de filtration*. Aucune action requise. Voir `docs/UPDATE_GUIDE.md` pour les cas de câblage particuliers.

## [2.18.3] - 2026-07-07

### Corrigé

- **Mode Boost — texte d'aide incohérent** : la description sous le widget Boost promettait toujours « filtration prolongée + cible ORP relevée », même quand l'effet réel affiché était « Surchloration seule » (filtration non gérée par le contrôleur) ou « Filtration prolongée seule » (ORP non automatique). Le texte est désormais cohérent : masqué quand le Boost est actif (la ligne « Effet » fait foi), et **prospectif honnête** quand inactif (« À l'activation : … ») en fonction de la configuration réelle. Purement UI, aucun impact sur le dosage.

---

## [2.18.2] - 2026-07-07

### Amélioré

- **Mode Boost — indication d'effet persistante (feature-055, finition feature-053/054)** : le widget Boost du tableau de bord affiche désormais **en permanence** les leviers réellement actifs — « Filtration prolongée + surchloration », « Surchloration seule », « Filtration prolongée seule », ou « Sans effet » (badge orange) —, complément persistant du toast fugace de la feature-054. L'info reste visible **après une activation depuis Home Assistant** et **après un rechargement de page**, via deux nouveaux champs WebSocket `boost_filtration_extended` (= filtration gérée par PoolController) et `boost_chlorine_boosted` (= régulation ORP en mode Automatique) calculés au vol. Affichage immédiat au clic Activer (mapping de la réponse `POST /boost/start`). **Purement informatif** : aucune logique de dosage ni de filtration modifiée.

### Documentation

- `docs/subsystems/ws-manager.md` : champs WS `boost_filtration_extended` / `boost_chlorine_boosted` (calculés au vol, buffer 1600 → 1664).
- `docs/features/page-dashboard.md` : ligne « Effet » persistante du widget Boost (4 libellés + badge `--warn`).

---

## [2.18.1] - 2026-07-07

### Amélioré

- **Mode Boost — signalement des leviers inertes (feature-054, finition feature-053)** : à l'activation, le Boost indique désormais quels leviers s'appliquent réellement selon la config. Si la filtration n'est pas gérée par PoolController (`filtration_enabled = false`), la filtration n'est pas prolongée ; si la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. Deux canaux d'information : (1) `startBoost()` logue un `warning` par levier inerte — couvre l'activation HTTP **et** MQTT/Home Assistant ; (2) la réponse 200 de `POST /boost/start` renvoie `filtration_extended` et `chlorine_boosted`, à partir desquels le dashboard affiche un **toast adaptatif** (nominal « vérifiez le taux de chlore », surchloration seule, filtration prolongée seule, ou « Boost sans effet »). **Purement informatif** : aucune logique de dosage, de garde ni de filtration n'est modifiée ; l'activation n'est jamais bloquée.

### Documentation

- `docs/API.md` : réponse 200 de `POST /boost/start` complétée avec `filtration_extended` / `chlorine_boosted` + note sur le log `warning`.
- `docs/features/page-dashboard.md` : tableau des 4 cas du toast adaptatif du Boost.
- `docs/subsystems/pump-controller.md` : section « Signalement des leviers inertes » (conditions d'effet de chaque levier, warnings `startBoost()`).

---

## [2.18.0] - 2026-07-06

### Fonctionnalités

- **Mode Boost — surchloration temporaire du jour (feature-053)** : après une forte fréquentation, un geste active le Mode Boost pour assainir davantage la piscine **pour la journée** : filtration prolongée (forçage dédié) + cible ORP relevée (`+60 mV`, plafonnée à 850 mV sous l'alerte). Surcouche **non destructive** (la config n'est jamais modifiée) auto-expirant au prochain minuit local. Pilotable depuis le dashboard (carte Boost avec expiration + toast « vérifier le taux de chlore avant baignade ») et depuis Home Assistant (switch « Boost »). L'activation est refusée si l'heure n'est pas synchronisée.

### Firmware

- **Régulation ORP — valeurs effectives boostées (feature-053)** : tant que le Boost est actif et **uniquement en mode de régulation ORP `automatic`**, la cible ORP effective = `min(orp_target + 60, 850)` mV et la limite journalière chlore effective = `min(max_chlorine_ml_per_day × 1,5, 1000)` mL — **bornes de sécurité dédiées figées par pool-chemistry**, jamais abaissées. En Manuel / Programmé, seule la filtration s'étend ; l'injection manuelle reste bornée à la limite normale. **Tous les autres garde-fous sont inchangés** (limite horaire, temps min d'injection, anti-rafale, injection si filtration active, watchdog) : le Boost n'ouvre aucun chemin de dosage. Décision structurante formalisée en [ADR-0025](docs/adr/0025-mode-boost.md).
- **Persistance** : état `boostState` (`active` + `untilEpoch`) persisté en NVS dédié (namespace `poolctrl`) → survit à un reboot dans la journée ; expiration honorée avant le reset des compteurs journaliers dans les deux branches de `tickDailyRollover()`.
- **MQTT / Home Assistant** : switch « Boost » (topic retain `{base}/boost` + commande `{base}/boost/set`, payload `ON`/`OFF`). L'état retombe automatiquement sur `OFF` à minuit. Activation refusée sans heure synchronisée (commande ignorée + resync HA).
- **Web / API** : routes `POST /boost/start` (auth WRITE, `409 time_not_synced` si horloge non synchronisée) et `POST /boost/stop` ; champs `boost_active` / `boost_until` (epoch) dans `/get-config` et le WebSocket.

### Documentation

- **[ADR-0025](docs/adr/0025-mode-boost.md)** créé (Accepté) : surcouche temporaire « valeurs effectives » + relèvement borné de la limite chlore, 4 bornes de sécurité et leur justification, alternatives écartées (mutation de config, relèvement non borné, durée fixe en heures, switch de mode), conséquences (edge boost court le soir, heure synchronisée requise, vérification chimique à la charge de l'utilisateur). Indexé dans `docs/adr/README.md`.
- `docs/MQTT.md` : topic `{base}/boost` + commande `{base}/boost/set`, entité discovery `switch` « Boost », section feature-053 dédiée.
- `docs/API.md` : routes `POST /boost/start` \| `/boost/stop`, champs `boost_active` / `boost_until` de `/get-config`.
- `docs/subsystems/ws-manager.md` : champs WS `boost_active` (bool) + `boost_until` (epoch).
- `docs/subsystems/pump-controller.md` : section Mode Boost (état, cible/limite effectives, expiration minuit, gate mode automatic), 4 constantes ajoutées au récapitulatif.
- `docs/features/page-dashboard.md` : carte Boost (activation, expiration, rappel baignade).

---

## [2.17.3] - 2026-07-06

### Firmware

- **Home Assistant — modes de régulation pH/ORP affichés en français (bug-ha-regulation-mode-labels)** : les selects « Mode Régulation pH » et « Mode Régulation ORP » exposaient dans HA les valeurs brutes anglaises `automatic`/`scheduled`/`manual` (feature-009), alors que l'UI web affiche « Automatique » / « Programmée » / « Manuelle ». Les deux selects affichent désormais ces libellés français via `value_template` / `command_template`, même technique que les modes filtration et éclairage. **Le protocole MQTT sur le fil reste strictement `automatic`/`scheduled`/`manual`** (topics d'état et handlers `PhRegulationMode`/`OrpRegulationMode` inchangés) : aucune automatisation Home Assistant existante n'est cassée.

### Documentation

- `docs/MQTT.md` : options des entités discovery pH/ORP passées aux libellés français, note dédiée aux templates, mise à jour de la note « Mode Filtration » (les selects de régulation ne sont plus en anglais).
- `docs/subsystems/mqtt-manager.md` : section « Selects Mode Régulation pH/ORP » (`value_template`/`command_template`, wire inchangé).

---

## [2.17.2] - 2026-07-06

### Firmware

- **Home Assistant — mode de programmation éclairage en liste déroulante (bug-ha-eclairage-select, finition feature-052)** : la programmation horaire de l'éclairage, jusque-là exposée en `switch` ON/OFF, est désormais un `select` **« Mode Éclairage »** avec les options **« Programmation »** / **« Désactivé »**, aligné sur le select « Mode Filtration ». Même technique que les libellés filtration : `value_template` / `command_template` traduisent l'affichage HA, le protocole sur le fil reste strictement `ON`/`OFF`. Le booléen `scheduleEnabled` et le handler `LightingSchedule` sont inchangés. Migration : un payload retain vide est publié sur l'ancien topic `homeassistant/switch/{id}_lighting_schedule/config` pour retirer le switch orphelin de HA.

### Documentation

- `docs/MQTT.md` : entité éclairage passée de `switch` à `select` « Mode Éclairage » (tableau discovery, section feature-052, note dédiée aux templates et au nettoyage de l'ancien switch).
- `docs/subsystems/mqtt-manager.md` : section « Select Mode Éclairage », migration de l'orphelin, mise à jour du handler et des compteurs.

---

## [2.17.1] - 2026-07-06

### Corrigé

- **Éclairage — rafraîchissement temps réel des champs de programmation (bug-ui-eclairage-refresh-ws, finition feature-052)** : l'écran Éclairage rafraîchit désormais ses champs de programmation (mode, heures début/fin) en temps réel lors d'un changement depuis Home Assistant, comme l'écran Filtration. Auparavant, `loadConfig()` (message WS `config`) repeuplait les champs filtration mais pas éclairage : il fallait quitter/revenir sur l'écran pour voir la valeur à jour. Correctif UI uniquement (`data/app.js`, miroir du mécanisme `filtrationDirty`) ; une saisie éclairage en cours n'est pas écrasée par un message WS. Aucun firmware modifié.

---

## [2.17.0] - 2026-07-06

### Fonctionnalités

- **Éclairage — section Programmation uniformisée avec la filtration (feature-052)** : la carte Programmation de la page Éclairage adopte les mêmes libellés que la filtration — select **« Mode d'éclairage »** avec options **« Programmation »** / **« Désactivé »**, heures **Début** / **Fin** en grille 2 colonnes. Les valeurs sous-jacentes (booléen `lighting_schedule_enabled`) et le backend sont inchangés ; seuls les libellés affichés changent. Le contrôle manuel (Allumer / Éteindre) reste indépendant.

### Firmware

- **Home Assistant — planning éclairage pilotable (feature-052)** : l'activation de la programmation et les heures de l'éclairage sont désormais exposées à Home Assistant, en miroir de la filtration (feature-051). Nouvelle entité `switch` « Programmation éclairage » (topic retain `{base}/lighting_schedule` + commande `.../set`, payload `ON`/`OFF` → booléen `scheduleEnabled`) et 2 entités `text` « Éclairage début » / « Éclairage fin » (`{base}/lighting_start` / `{base}/lighting_end` + `.../set`, format `HH:MM`). Validation `ON`/`OFF` et `HH:MM` (invalide → ignoré + `warning` + resync HA) ; changement persisté en NVS, appliqué via `lighting.update()`, et reflété dans l'UI web sous ≤ 5 s (broadcast WS `config`). `publishLightingState()` enrichie et publiée au cycle périodique. Le booléen `scheduleEnabled` est conservé côté firmware (pas de stringification, cf. feature-028). Aucun chemin de dosage touché.

### Documentation

- `docs/MQTT.md` : 3 topics d'état + 3 commandes `/set` éclairage, 3 entités discovery (1 `switch` + 2 `text` `HH:MM`), section feature-052 dédiée, compte discovery total mis à jour.
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `LightingSchedule`/`LightingStart`/`LightingEnd`, handler planning éclairage, `publishLightingState()` enrichie, compteurs de publish (états 23 / discovery 20) et compte discovery (44).
- `docs/features/page-lighting.md` : nouveau libellé « Mode d'éclairage », options Programmation/Désactivé, exposition HA du planning, indépendance du contrôle manuel.

---

## [2.16.0] - 2026-07-06

### Firmware

- **Home Assistant — heures de filtration modifiables (feature-051)** : les heures de début et de fin du créneau de filtration sont désormais exposées à Home Assistant en 2 entités `text` **éditables** (« Filtration début » / « Filtration fin »), complétant le select « Mode Filtration ». Nouveaux topics retain `{base}/filtration_start` / `{base}/filtration_end` (état, format `HH:MM`) et commandes `{base}/filtration_start/set` / `{base}/filtration_end/set`. La saisie est validée `HH:MM` (format invalide → ignoré + `warning` + resync HA sur la valeur réelle) ; le changement efface les overrides marche/arrêt forcés, applique le planning immédiatement, persiste en NVS et se reflète dans l'UI web sous ≤ 5 s (broadcast WS). **Effet réel en mode Programmation** : en mode **Auto**, les heures sont recalculées par la température (une saisie HA est alors consultative et auto-corrigée). Aucun chemin de dosage touché.

### Documentation

- `docs/MQTT.md` : 2 topics d'état + 2 commandes filtration_start/end, 2 entités discovery `text` (pattern `HH:MM`), section feature-051 dédiée, sémantique Programmation/Auto, compte discovery mis à jour (41).
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `FiltrationStart`/`FiltrationEnd`, handler groupé (validation `HH:MM` avant mutex, efface overrides, `filtration.update()` hors mutex sur `loopTask`), `publishFiltrationState()` enrichi (4 topics).

---

## [2.15.0] - 2026-07-06

### Firmware

- **Home Assistant — libellés du mode filtration en français et non ambigus (bug-ha-filtration-mode-labels)** : le select « Mode Filtration » exposait dans HA les valeurs brutes `auto`/`manual`/`force`/`off`, alors que l'UI web affiche « Auto / Programmation / Manuel / Désactivé ». Au-delà de l'écart de langue, c'était trompeur : `manual` correspond à « Programmation » (créneau à heures fixées) et `force` à « Manuel » (contrôle ON/OFF sans planning) — l'inverse de l'intuition. Le select HA affiche désormais les **libellés français** via `value_template` (état brut → libellé) et `command_template` (libellé → valeur brute). **Le protocole MQTT sur le fil reste strictement `auto`/`manual`/`force`/`off`** (topic d'état et handler de commande inchangés) : aucune automatisation Home Assistant existante n'est cassée. Les selects de régulation pH/ORP restent en valeurs anglaises (non trompeuses).

### Documentation

- `docs/MQTT.md` : sémantique des 4 modes filtration détaillée sur les topics d'état/commande, note dédiée sur les templates d'affichage du select (libellés français côté HA, wire inchangé).
- `docs/subsystems/mqtt-manager.md` : nouvelle section sur les `value_template`/`command_template` du select `filtration_mode` (traduction affichage uniquement, `doc.clear()` anti-fuite, handler brut inchangé).

---

## [2.14.1] - 2026-07-06

### Firmware

- **MQTT / UI temps réel — synchronisation WebSocket (bug-sync-ws-config-mqtt)** : quand la configuration est changée depuis Home Assistant (consignes pH/ORP, modes de régulation, volumes quotidiens, mode filtration, éclairage), l'UI web se met désormais à jour automatiquement sous ≤ 5 s **sans recharger la page**. Auparavant, seul le chemin HTTP `POST /save-config` déclenchait le broadcast WebSocket `config` ; les commandes reçues par MQTT modifiaient bien la config (persistée + republiée sur le broker) mais ne notifiaient jamais les clients WS connectés. Correctif : `MqttManager::drainCommandQueue()` appelle `wsManager.requestConfigBroadcast()` une fois en fin de drain dès qu'au moins une commande (hors `Reboot`) a été traitée — le message `config` part au prochain cycle WS. Broadcast unique et idempotent (coalescé), aucun contrat modifié.

### Documentation

- `docs/subsystems/mqtt-manager.md` : `drainCommandQueue()` déclenche un broadcast WS `config` après une commande modifiant la config (cohérence UI temps réel, même effet que `POST /save-config`).
- `docs/subsystems/ws-manager.md` : le chemin MQTT (`drainCommandQueue`) documenté comme second déclencheur du message `config`.

---

## [2.14.0] - 2026-07-06

### Firmware

- **MQTT / Home Assistant — complétude de couverture (feature-050)** : issu d'un audit web/API vs MQTT/HA (périmètre « rentable »). Ajout de 5 entités auto-discovery HA. **Cumuls journaliers de dosage exposés** : nouveaux topics retain `{base}/ph_daily_ml` et `{base}/orp_daily_ml` (cumul injecté en mL, dédup au cycle 10 s, retombe à 0 à minuit) + 2 `sensor` HA « Dosage pH/Chlore aujourd'hui » (`state_class: measurement`). **Volumes quotidiens programmés commandables** : les topics `{base}/ph_daily_target_ml` / `{base}/orp_daily_target_ml` deviennent commandables via `.../set` + 2 `number` HA « Volume quotidien pH/Chlore » (min 0 / max 2000 / step 10 mL) — payload non numérique ignoré + `warning`, clamp négatif → 0, **REFUS si valeur > limite journalière VIVE** (`maxPhMlPerDay` / `maxChlorineMlPerDay`) + resync HA (condition pool-chemistry), persistance NVS. **Bouton Redémarrer** : topic `{base}/reboot/set` + `button` HA `device_class: restart` — redémarrage différé propre (flush MQTT `offline` puis restart, même séquence que `POST /reboot`).

### Documentation

- `docs/MQTT.md` : nouveaux topics d'état et de commande, 5 entités discovery (compte total 39), section feature-050 détaillée, et **nouvelle section « Exclusions volontaires »** documentant comme une politique assumée ce qui n'est délibérément pas exposé en MQTT/HA (injections manuelles, limites de sécurité, calibration/OTA/factory-reset/Wi-Fi/auth, transients d'affichage, historique).
- `docs/subsystems/mqtt-manager.md` : `InboundCmdType` `PhDailyTarget`/`OrpDailyTarget`/`Reboot`, handlers volume quotidien (garde limite vive) et reboot différé (flag consommé en tête de drain), compte discovery mis à jour.

---

## [2.13.0] - 2026-07-06

### Firmware

- **Infrastructure — repartitionnement flash, layout v4 (feature-049)** : les slots firmware `app0`/`app1` passent de 1664 à **1792 KB** (+128 KB chacun), pris sur la partition `spiffs` réduite de 576 à **320 KB** (offset déplacé à `0x390000`) — rendu possible par la pré-compression gzip (feature-048, payload FS 155 KB, soit 48 % de la nouvelle partition). Occupation firmware ≈**78,5 %** (marge par slot 264 → ~392 KB). Partitions `nvs`, `history` et `coredump` strictement inchangées : **config, historique et coredump préservés**. ⚠️ **Mise à jour par câble USB requise une seule fois** (`./deploy.sh all`) — l'OTA ne peut pas réécrire la table de partitions ; **ne jamais pousser le FS v4 en OTA sur un appareil en table v3** ; ne pas utiliser `./deploy.sh factory` (efface la NVS). L'OTA redevient normal ensuite. Quatuor aligné : `partitions.csv`, `platformio.ini` (`filesystem_size = 327680`), `build_fs.sh` (`-s 327680`), `deploy.sh` (`LITTLEFS_OFFSET=0x390000`, contrôle de taille 327680 avec reconstruction automatique d'une image de mauvaise taille).

### Documentation

- Nouvel [ADR-0024](docs/adr/0024-partitions-layout-v4.md) : partitions app à 1792 KB (layout v4) — contexte (gzip feature-048 → `spiffs` occupée à 27 %), alternatives écartées (statu quo — marge suffisante aujourd'hui mais flash USB groupable avec la release ; +64 KB — demi-mesure ; reprendre `history` — données à préserver), conséquences (flash USB unique, interdiction FS v4 en OTA sur table v3, granularité 64 KB des slots). [ADR-0019](docs/adr/0019-partition-app-1664k.md) marqué « Superseded by ADR-0024 ».
- `docs/UPDATE_GUIDE.md` : nouvelle note « Migration layout v3 → v4 (v2.13.0) » — procédure USB, interdiction explicite de l'OTA v4-sur-v3 (métadonnées LittleFS discordantes), vérifications post-flash (Infos système : FS ~320 KB, historique présent).
- `docs/BUILD.md` : taille filesystem 327 680 o = 320 KB (layout v4, ADR-0024), occupation firmware actualisée (~78,5 %, marge ~392 KB).

---

## [2.12.1] - 2026-07-06

### Frontend

- **Performance — assets web pré-compressés gzip (feature-048)** : `minify.js` gzippe (zlib niveau 9) tous les fichiers texte (`.js`, `.html`, `.css`) après minification — seule la variante `.gz` est embarquée dans le filesystem, images/icônes inchangées. ESPAsyncWebServer sert nativement ces fichiers avec `Content-Encoding: gzip` sur tous les chemins (routes HTML explicites, `serveStatic`, portail captif) — **aucune modification firmware**. Payload FS **443 → 155 KB** (partition `spiffs` 78 % → 27 %), **chargement des pages ~4× plus rapide** (le débit de l'ESP32 est le facteur limitant sur mobile). Routes API/JSON non concernées ; un `curl` brut sur un asset statique reçoit du gzip (utiliser `--compressed`).

### Documentation

- `docs/BUILD.md` : étape gzip du pipeline FS (`minify.js` → `data-build/` `.gz` → `build_fs.sh`), règle « jamais de fichier texte non compressé dans `data-build/` », caveat `curl --compressed`.
- `docs/subsystems/web-server.md` : mécanisme de résolution `.gz` d'ESPAsyncWebServer sur les deux chemins de serving (références lib vérifiées).

---

## [2.12.0] - 2026-07-06

### Frontend

- **UX — remplacement des 43 dialogues natifs (feature-031)** : plus aucun `alert()` / `confirm()` dans l'UI. Les **31 `alert()`** informatifs passent par les toasts `showToast` existants (mapping `error`/`success`/`info` selon le contexte) ; les **12 `confirm()`** (actions destructives : purge historique, effacement logs/coredump, déconnexion Wi-Fi, redémarrage, reset sondes, reset usine…) passent par une nouvelle **modale de confirmation maison** `confirmDialog({title, message, confirmLabel, cancelLabel, danger}) → Promise<boolean>` — `<dialog id="confirm-modal">` statique (pattern `probe-modal`), focus initial sur **Annuler**, fermeture `Escape`/backdrop = annulation, réentrance refusée, contenu injecté en `textContent` (anti-XSS), fallback `window.confirm` si `<dialog>` indisponible, **variante danger (bouton rouge)** pour les 7 actions irréversibles. Refus = défaut sûr sur les 12 flux async convertis (revue site par site). Le **reset usine conserve sa double confirmation** (« Réinitialiser » puis « Tout effacer »). Emojis ⚠ retirés des messages — le rouge porte la sévérité.
- **Page « À propos » mise à jour** : retrait des crédits obsolètes DFRobot_PH et Adafruit ADS1X15 (matériel remplacé par les modules Atlas Scientific EZO en I²C natif depuis la feature-021) et de Playwright (jamais utilisé) ; ajout des modules EZO et des tests natifs Unity (185 tests, CI).

### Documentation

- `docs/features/README.md` : nouvelle section « Composants transverses » — API du helper `confirmDialog`, règle « toute action destructive passe par `confirmDialog` ».
- `docs/features/page-settings.md` : confirmations via modale maison (logs, coredump, reset usine à double confirmation).

---

## [2.11.2] - 2026-07-06

### Firmware

- **Sécurité — comparaison de token à temps constant (feature-028)** : le token API n'est plus comparé via `==`/`!=` sur `String` (court-circuit au premier octet différent → canal temporel sur le contrôle d'accès central). Nouvelle comparaison `constantTimeEquals` (`auth.cpp`) : longueur vérifiée d'abord (non secrète — format fixe 32 hexa), puis accumulation XOR/OR de **tous** les octets sans court-circuit. Appliquée à l'auth **HTTP** (`checkTokenAuth`) ET à l'auth **WebSocket** via la nouvelle API publique `AuthManager::secureTokenEquals()` — la revue de code a relevé que le WS comparait encore le token en `!=` (corrigé avant merge). Plus aucune comparaison directe du token dans le code. Code mort de debug (`maskedReceived`/`maskedExpected`) supprimé. Protections préservées : refus du token en query param, token jamais loggué en clair.
- **Sécurité — retrait complet du mécanisme CORS ([ADR-0023](docs/adr/0023-politique-cors-retrait.md))** : le CORS était partiellement implémenté et trompeur (seul le **premier** origin de la liste était posé, statiquement, sans validation par requête — le multi-origines configuré était silencieusement ignoré). Décision : **politique même-origine stricte** (l'UI est servie par l'ESP32, offline first). Retirés : bloc `DefaultHeaders` CORS + `setCorsHeaders` (code mort), champ `AuthConfig::corsAllowedOrigins` + persistance NVS (valeur existante orpheline, pas de migration), champ `auth_cors_origins` de `/get-config`/`/save-config` et du message WebSocket `config`, preflight `HTTP_OPTIONS`. En-têtes CSP / `X-Content-Type-Options` / `X-Frame-Options` conservés. Toute intégration cross-origin future : reverse proxy ou nouvel ADR.

### Frontend

- **Page Paramètres → Sécurité** : card CORS supprimée (champ `#auth_cors_origins` + bouton `#cors-save-btn`) — mécanisme retiré côté firmware.

### Documentation

- Nouvel [ADR-0023](docs/adr/0023-politique-cors-retrait.md) : retrait complet du CORS, politique même-origine stricte (alternatives écartées : réparation multi-origines par requête — ESPAsyncWebServer s'y prête mal, besoin inexistant ; origin unique assumé — mécanisme conservé sans besoin réel).
- `docs/subsystems/auth.md` : section « Comparaison de token à temps constant » + règle « jamais `==`/`!=` sur le token » ; `docs/subsystems/ws-manager.md`, `docs/subsystems/web-server.md`, `docs/features/page-settings.md`, `docs/API.md` : politique même-origine documentée, mentions CORS retirées.

---

## [2.11.1] - 2026-07-06

### Firmware

- **Fiabilité — plus aucune attente infinie de mutex (feature-027)** : les **24 prises de mutex** encore en `portMAX_DELAY` (mqtt_manager ×9, logger ×7, history ×6, config ×2) sont désormais **bornées** — la `loopTask` ne peut plus être figée jusqu'au reset watchdog (30 s) par une contention (ex. mutex tenu pendant une I/O LittleFS lente). Dégradation gracieuse par site, comportement nominal strictement inchangé :
  - **Logger** (`kLoggerMutexTimeoutMs = 100 ms`) : politique **silencieuse anti-récursion** (jamais de `systemLogger` dans les chemins d'échec) — entrée perdue comptée dans le compteur diagnostic interne `_droppedLogs`, lectures → résultat vide, `clearAll()`/`flushToDisk()` abandonnent **avant** toute modification (pas d'état incohérent).
  - **Historique** (`kHistoryMutexTimeoutMs = 2000 ms`, dimensionné sur consolidation + `saveToFile` ~1–1,5 s) : point/consolidation sautés avec **retry naturel** (`lastRecord`/`lastSave` non avancés), lectures → vecteur vide, `clearHistory()` devient `bool` — la route `POST /history/clear` répond **`503` « Historique occupé — réessayer »** en cas d'échec.
  - **Config** (`kConfigMutexTimeoutMs`, récursif) : `saveMqttConfig` → `error` (config RAM appliquée, persistance NVS perdue) ; `saveProductConfig` → dirty flag conservé → retry au prochain passage.
  - **MQTT** : snapshots de publication sautés (republication naturelle au prochain cycle, topics retain) ; commande HA reçue → **abandon complet** (aucun état partiel) + **resync immédiat** de l'entité HA sur l'état réel (`publishFiltrationState()`/`publishTargetState()`).
  - Warns de timeout **throttlés à 1/min/site** (`kMutexTimeoutWarnThrottleMs = 60000`). Audit `grep portMAX_DELAY src/` à blanc. Revue de concurrence complète (Approved), 185/185 tests natifs verts.

### Documentation

- `docs/subsystems/logger.md`, `docs/subsystems/history.md`, `docs/subsystems/mqtt-manager.md` : stratégie de timeout mutex et politique d'échec par site (dont `_droppedLogs` write-only volontaire et pattern accepté de lecture sans mutex dans le resync filtration).

---

## [2.11.0] - 2026-07-06

### Firmware

- **Sécurité — vérification d'intégrité SHA-256 avant flash OTA (feature-026, [ADR-0022](docs/adr/0022-verification-integrite-ota.md))** : la règle de sécurité #6 (« toujours vérifier la signature/CRC avant flash ») est désormais satisfaite. L'empreinte SHA-256 de chaque flux OTA est calculée **au fil de l'eau** (`OtaStreamHasher`, mbedtls accéléré matériellement, non bloquant pour AsyncWebServer) et comparée à l'empreinte de référence — le champ `digest` (`sha256:<64hex>`) publié par l'API GitHub Releases pour chaque asset (présence vérifiée sur la release réelle v2.5.2, zéro changement du process de release). **Chemin GitHub fail-closed strict** : `/check-update` relaie les champs additifs `firmware_digest`/`filesystem_digest`, l'UI les repasse en paramètre `digest=` **requis** de `/download-update` (même modèle de menace que l'URL déjà relayée : whitelist d'hôtes + auth CRITICAL). Digest absent → `400 integrity_digest_missing` (refus avant toute connexion) ; malformé → `400 integrity_digest_invalid` ; mismatch (image corrompue/tronquée, coupure WiFi incluse) → **firmware** : `Update.abort()` avant tout `end()` — l'image n'est **jamais activée**, version courante conservée ; **filesystem** : `422` sans restart, remontage LittleFS best-effort, réinstallation FS requise (firmware intact et bootable). **Upload manuel `/update`** : empreinte toujours calculée et **loggée** ; champ POST `sha256` optionnel (mismatch ou valeur malformée → flash refusé) — fail-open documenté, l'utilisateur est physiquement présent. ArduinoOTA/espota inchangé (déjà protégé par le MD5 du protocole). Tout refus émet un log `CRITICAL` avec empreintes attendue/calculée. Logique pure `ota_integrity_logic` (parse, comparaison **temps constant**, formatage — pattern ADR-0017), **22 tests natifs ajoutés** (185/185 verts).
- **Corrigé — verrou anti-upload concurrent sur `/update`** : deux uploads simultanés corrompaient l'état statique du handler (hasher, empreinte attendue, progression). Une seule session propriétaire à la fois (`g_uploadOwner`) : le 2ᵉ client reçoit **`409`** ; déconnexion du client en cours de transfert → `Update.abort()` + réarmement du dosage (`setOtaInProgress(false)`) + libération du verrou ; owner périmé repris si `!Update.isRunning()`.
- **Corrigé — progression entre deux uploads (AC5)** : le throttle de log de progression (`static lastLog`) n'était pas réinitialisé entre deux uploads sans reboot — désormais remis à zéro à chaque début d'upload.
- **Corrigé — pompes inhibées après échec `Update.begin()`** : `setOtaInProgress(true)` n'était pas réarmé sur ce chemin d'échec — les pompes doseuses restaient coupées jusqu'au reboot. Appariement `setOtaInProgress` garanti sur tous les chemins terminaux (relevé en revue).

### Frontend

- **Page Mise à jour — fail-closed visible (feature-026)** : bouton **Installer désactivé** avec message explicite si la release GitHub ne fournit pas d'empreinte d'intégrité ; messages d'erreur français distincts sur refus (`Empreinte d'intégrité absente ou invalide…`, `Échec de la vérification d'intégrité — image refusée, version actuelle conservée.` + invite à réinstaller le filesystem le cas échéant) ; le digest est transmis automatiquement à `/download-update`.

### Documentation

- Nouvel [ADR-0022](docs/adr/0022-verification-integrite-ota.md) : SHA-256 comparé au digest de l'API GitHub, fail-closed chemin GitHub, empreinte loggée + param optionnel en upload manuel, transit du digest par l'UI (alternatives écartées : `Update.setMD5` — ne couvre pas le flux FS écrit via `esp_partition_write` ; re-fetch API côté firmware — double le pic RAM TLS ; signature asymétrique — feature ultérieure ; relecture flash post-write — redondante). Conséquences assumées : anciennes releases sans digest refusées par l'UI ; usure flash sur digest faux (image écrite avant refus).
- `docs/subsystems/ota-manager.md` : politique d'intégrité par chemin, verrou anti-upload concurrent, appariement `setOtaInProgress`, constantes `kOtaSha256HexLen`/`kOtaSha256Prefix`, logs CRITICAL.
- `docs/API.md` : champs `firmware_digest`/`filesystem_digest` de `/check-update`, param `digest=` requis de `/download-update` (erreurs 400/422), param `sha256` optionnel et `409` de `/update`.
- `docs/UPDATE_GUIDE.md` : section « Vérification d'intégrité SHA-256 » (cas mismatch firmware/FS, release sans digest, upload manuel) + dépannage.

---

## [2.10.1] - 2026-07-06

### Firmware

- **Corrigé — alerte « limite journalière atteinte » répétée toutes les 60 s** : après le latch de la limite journalière pH ou ORP, le health check de `main.cpp` (60 s) republiait l'alerte MQTT `ph_limit`/`orp_limit` (topic `{base}/alerts`, non-retained) et son log `WARN` **à chaque cycle**, indéfiniment. L'alerte n'est désormais émise qu'à la **transition** vers la limite atteinte (edge-trigger, même flag que l'alarme écran UART) — une seule ligne WARN par épisode. L'état permanent pour Home Assistant reste porté par le binary_sensor `{base}/ph_limit` / `{base}/orp_limit` (retain, inchangé). Après un reboot avec limite déjà latchée : une alerte unique au premier health check (~60 s), voulu. Alarme écran UART : comportement identique.
- **Corrigé — libellé pH codé en dur** : le message d'alerte disait « Limite journalière pH- atteinte » même en configuration pH+. Libellé désormais dynamique (« pH+ » / « pH- » selon `phCorrectionType`, même règle que le log CRIT de `pump_controller`). Libellé ORP (« chlore ») inchangé, toujours exact.

### Documentation

- `docs/MQTT.md` : sémantique de transition des alertes `ph_limit`/`orp_limit` précisée (événement non-retained vs binary_sensor retain, libellé dynamique).

---

## [2.10.0] - 2026-07-05

### Firmware

- **Sécurité — détection capteur pH/ORP figé (feature-022 re-scopée, P2.1/P2.2)** : un module EZO qui répond sans erreur I²C mais retourne indéfiniment la **même valeur** (variance nulle — électronique figée, sonde HS côté BNC) est désormais détecté : nouvelle classe pure `FrozenDetector` (« run ancré » O(1) : bornes `runMin`/`runMax` + longueur de run, ré-ancrage dès qu'une lecture sort de la bande) composée dans `SensorFilter` et alimentée **uniquement par les lectures brutes acceptées** (l'état figé persiste pendant une rafale de rejets). Figé = **30 lectures valides** dans une bande < epsilon (`kSensorFrozenSamples` = 30 ≈ 2,5 min ; `kSensorFrozenEpsilonPh` = 0.0005 = ½ LSB EZO pH ; `kSensorFrozenEpsilonOrp` = 0.05 mV = ½ LSB EZO ORP — un capteur vivant bruite ≥ ±1 LSB, un toggle de 1 LSB casse le run, validé pool-chemistry). Figé ⇒ `ready()` = false ⇒ **dosage bloqué fail-closed** par la garde `FilterNotReady` existante (aucune modification des gardes verrouillées) ; **sortie immédiate sans re-warmup** dès une lecture vivante. Log `critical` edge-triggered `[SENSOR_FROZEN]` (+ `info` à la levée) et alerte MQTT retain `{base}/alerts/sensor_frozen` (JSON, clear payload vide). Consigne terrain : en cas de faux positifs, **durcir N, jamais élargir epsilon**.
- **Température figée warning-only (P2.3)** : `FrozenDetector` dédié à la sonde DS18B20 « eau » (N = 900 ≈ 30 min à 2 s, epsilon 0.03 °C < ½ LSB 12 bits, alimenté par les valeurs brutes non arrondies) — log `warning` seulement, **aucun impact dosage/filtration** (effets réels : compensation EZO pH + planning auto filtration), hors alerte MQTT `sensor_frozen`.
- **MQTT/HA — états problème capteur (P1.1)** : 2 nouveaux topics state retain `{base}/ph_sensor_problem` / `{base}/orp_sensor_problem` (`ON` = capteur **stale OU figé**, `OFF` sinon, publication dédupliquée) + 2 entités auto-discovery `binary_sensor` `device_class: problem` (« Capteur pH — problème » / « Capteur ORP — problème », `unique_id: poolcontroller_ph|orp_sensor_problem`) — l'indisponibilité capteur devient visible dans HA sans parser les JSON d'alerte.
- 11 tests natifs ajoutés (FrozenDetector + intégration SensorFilter), 163/163 verts. Validé pool-chemistry (GO, 4 conditions levées).

### Frontend

- **Dashboard — segment « Capteur indisponible » (P1.2)** : nouveau segment **danger, priorité 0 (la plus haute)** dans le badge multi-états des cartes pH/ORP, piloté par les valeurs `ph`/`orp` `null` du WS (capteur stale ou figé) — aucun champ WS nouveau. Garde anti-faux-positif au boot (aucun segment tant qu'aucun message WS reçu). Échelle des priorités désormais : 0 = capteur indisponible, 1 = hors plage, 2 = limite journalière, 3 = cumul ≥ 75 %, 4 = stock faible (toujours max 2 segments affichés).

### Documentation

- `docs/MQTT.md` : nouvelle section feature-022 (2 topics `*_sensor_problem` + 2 entités discovery + alerte `alerts/sensor_frozen` ajoutée au tableau des alertes dédiées).
- `docs/subsystems/sensors.md` : nouvelle section « Détection capteur figé » (FrozenDetector, run ancré, ½ LSB, intégration `ready()`, sortie immédiate, température warning-only, consigne durcir-N) ; API, états du filtre et constantes complétés.
- `docs/subsystems/mqtt-manager.md` : blocs de publication problème capteur / sensor_frozen, caches de dédup, discovery 32 → 34 entités.
- `docs/features/page-dashboard.md` : segment « Capteur indisponible » + échelle de priorités 0-4.

Écartés explicitement (documentés dans la spec) : compteurs de refus par cause, refresh périodique du cache cal_points, topics `dose_refusal`, ring RAM injections.

---

## [2.9.2] - 2026-07-05

### Firmware

- **Corrigé — écrêtage au reliquat pour l'injection manuelle** : demander une injection manuelle supérieure au reliquat du quota journalier (ex. limite 50 mL, cumul réel 39,4 affiché « 39 », demande de 11 mL) était refusée avec un message contradictoire « reste disponible aujourd'hui : 11 mL » (`lroundf` arrondissait le reliquat au plus proche alors que la garde raisonnait en volume exact). Désormais, `/ph|orp/inject/start` **écrête automatiquement la demande au reliquat** (durée recalculée arrondie **vers le bas** : le volume effectif ne re-déborde jamais la limite) au lieu de refuser — nouveau log info « [Injection] pH|ORP : volume écrêté de X à Y mL (reliquat journalier) ». En fin naturelle, le crédit plancher (v2.9.1) porte le cumul **exactement à la limite** → latch `ph/orp_limit_reached` + badge « Limite journalière atteinte ». Le refus `daily_limit` ne subsiste que si le reliquat est **nul ou < 1 s de pompe**, et son message/champ `remaining_ml` arrondissent désormais **vers le bas** (`floorf` — plus jamais de promesse intenable). Cas double-clamp (reliquat > 10 min de pompe) : le plafond `kManualInjectMaxDurationS` gagne, crédit = volume effectif. Garde pure `evaluateManualInject()` inchangée ; symétrie pH/ORP + chemin legacy `?duration=` ; logs de démarrage d'injection à 1 décimale (reco cosmétique code-reviewer). Validé pool-chemistry (GO), 152/152 tests natifs verts.

### Documentation

- `docs/API.md` : écrêtage automatique au reliquat documenté sur `/ph|orp/inject/start` ; sémantique **floor** du `remaining_ml` du 409 `daily_limit`.
- `docs/subsystems/pump-controller.md` : section « Crédit plancher » complétée (écrêtage → `creditMl = reliquat` entier ; double-clamp → `effectiveMl`).
- `docs/features/page-ph.md`, `page-orp.md` : mention de l'écrêtage automatique dans la garde journalière prédictive.

---

## [2.9.1] - 2026-07-05

### Firmware

- **Corrigé — sous-compte du cumul journalier en fin d'injection manuelle volumée** : demander exactement le reliquat du quota journalier (ex. limite 50 mL, injection 50 mL — frontière `==` acceptée depuis feature-006) laissait le cumul de sécurité à 49,x mL : l'intégration débit × temps perdait la dernière tranche (pompe coupée avant le tick de comptage suivant) et l'arrondi à la seconde → `ph/orp_limit_reached` jamais latché, badge dashboard « Cumul 98 % » figé au lieu de « Limite journalière atteinte ». À la **fin naturelle** d'une injection manuelle volumée, le compteur journalier de sécurité est désormais **planché au volume promis** (`creditManualInjectionFloor` : `daily = max(daily, startCumul + effectiveMl)` ; rollover minuit pendant l'injection → volume entier crédité au jour nouveau, conservateur). **Jamais de crédit sur arrêt anticipé** (stop manuel `/inject/stop`, interruption filtration) : l'intégration réelle fait foi. Propriété validée pool-chemistry (GO) : le registre de sécurité **ne sous-compte jamais** (sur-compte possible de quelques dixièmes de mL) ; le suivi produit (stock) reste sur l'intégration réelle (légère sous-estimation cumulative possible, assumée). Nouveau log info « [Sécurité] Compteur journalier pH|ORP ajusté de X à Y mL (crédit fin d'injection manuelle) ». Symétrie pH/ORP. 152/152 tests natifs verts.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Crédit plancher à la fin naturelle » (propriété fail-safe, tableau des 3 chemins d'arrêt, rollover minuit, suivi produit non planché et dérive de stock, nouveaux logs) ; `creditManualInjectionFloor` ajoutée à l'API publique.

---

## [2.9.0] - 2026-07-05

### Frontend

- **Dashboard — badge multi-états des cartes pH/ORP (feature-012, périmètre réduit sur avis ux-ui-designer)** : nouveau segment **« Cumul XX % »** (warning) affiché à partir de **75 %** du quota journalier (même arithmétique que la barre des pages /ph et /orp : arrondi des entrées, plafond 100 %), masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais), quand la limite n'est pas configurée ou données indisponibles. Règle de composition **formalisée** (`composeSensorBadge`) : priorité hors plage (danger) > limite atteinte > cumul ≥ 75 % > stock faible, **maximum 2 segments** (troncature aux 2 plus prioritaires), variant `danger` si un segment danger retenu sinon `warning`. Changement visible : « hors plage + limite atteinte » simultanés affichent désormais **les 2 segments** (avant : seul « hors plage »). 100 % UI (`data/app.js`), aucun changement firmware.
- **Dashboard — stabilisation par pompe (feature-012)** : le statut « Stabilisation : X min YY s » des cartes pH/ORP consomme les champs **par pompe** `ph/orp_stab_remaining_s` (feature-006, repli sur le champ global pour les firmwares plus anciens) — chaque carte affiche la stabilisation de **sa** pompe (une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement).

### Documentation

- `docs/features/page-dashboard.md` : nouvelles sections « Badge multi-états des cartes pH/ORP » (4 segments, priorité, max 2, seuil 75 %) et « Statut dosage et stabilisation par pompe » ; champs WS consommés complétés.

---

## [2.8.0] - 2026-07-05

### Firmware

- **Régulation — répartition 24 h du mode Programmée (feature-011)** : le volume quotidien du mode `scheduled` (pH et ORP) n'est plus injecté d'un bloc en début de filtration (pic de concentration localisé) mais **réparti par fenêtres de 15 min** alignées sur l'horloge (`kScheduledWindowMinutes`) sur la plage de filtration restante, **bornée à minuit** ([ADR-0021](docs/adr/0021-repartition-scheduled.md)). Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — auto-corrigé après changement de cible, injection manuelle comptée dans le cumul, retard subi ou reboot) est borné par le **budget horaire partagé** ([ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md), warning edge-triggered si tronqué) et **reporté** si l'injection durerait moins de 30 s (`minInjectionTimeMs`, anti short-cycling — avec une pompe rapide et une petite cible, les premières fenêtres sont reportées et les injections démarrent quand l'horizon se resserre : comportement voulu). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info` dans `tickDailyRollover`). Décision extraite en fonction pure testable `evaluateScheduledDose()` (`dosing_logic`, pattern ADR-0017) + helper pur `remainingRangeMinutes()` (`schedule_logic`) ; fail-closed strict (watchdog inactif, heure locale invalide, horizon ou débit nuls → aucune injection, warning unique). Le **ring anti-rafale partagé est consulté avant tout démarrage** (6/min, 20/15 min, mêmes seuils que `canDose`) ; l'exemption `cyclesToday` de la branche scheduled est **conservée et documentée** (démarrages bornés structurellement ≤ 4/h par le cadencement — arbitrage R4 validé pool-chemistry). Mode filtration `continu` : horizon = fin de journée (le dosage hors plage reste permis). Validé pool-chemistry (GO, 5 conditions levées) ; **17 tests natifs ajoutés** (12 `evaluateScheduledDose` + 5 `remainingRangeMinutes`), 152/152 verts.
- **WebSocket** : nouveaux champs `ph_scheduled_flow_ml_per_min` / `orp_scheduled_flow_ml_per_min` (float 1 décimale = débit moyen planifié restant ; `null` hors mode scheduled / hors plage / heure invalide). Buffer `sensor_data` 1472 → 1536 octets.

### Frontend

- **Cartes Régulation pH/ORP — bloc Programmée (feature-011)** : nouvelle ligne « Débit calculé : X,X mL/min » (« — » avec infobulle « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Pas d'affichage « sur Y h restantes » (l'horloge du navigateur peut différer de celle de l'ESP32).

### Documentation

- Nouvel [ADR-0021](docs/adr/0021-repartition-scheduled.md) : répartition scheduled par fenêtres de 15 min alignées horloge (snapshot `stopTargetMl`, report < 30 s, pas de rattrapage J+1, exemption `cyclesToday`, horizon minuit en continu ; alternatives écartées : PID slow imbriqué, fenêtres relatives au début de plage, rattrapage J+1, fenêtre 30 min, limite de cycles dédiée, recalcul continu sans snapshot). Note de renvoi ajoutée dans [ADR-0002](docs/adr/0002-mode-programmee-volume-quotidien.md).
- `docs/subsystems/pump-controller.md` : §Mode scheduled réécrit (algorithme pur + coquille, ring consulté, exemption `cyclesToday` justifiée, nouveaux logs edge-triggered, `kScheduledWindowMinutes`, accesseurs `get*ScheduledPlannedFlow`).
- `docs/subsystems/filtration.md` : `remainingRangeMinutes` ajouté au tableau des fonctions pures de `schedule_logic`.
- `docs/features/page-ph.md`, `page-orp.md` : mode Programmée détaillé (répartition, report, budget horaire, reliquat, nuance pompe rapide) + ligne « Débit calculé » et hint.
- `docs/API.md`, `docs/subsystems/ws-manager.md` : champs WS `ph/orp_scheduled_flow_ml_per_min`.

---

## [2.7.0] - 2026-07-04

### Firmware

- **MQTT/HA — modes de régulation commandables (feature-009)** : deux nouvelles entités Home Assistant `select` (« Mode Régulation pH », « Mode Régulation ORP », options `automatic` / `scheduled` / `manual`) permettent de changer `ph_regulation_mode` / `orp_regulation_mode` depuis HA (automatisations d'hivernage, absence longue, dashboards). Nouveaux topics de commande `{base}/ph_regulation_mode/set` et `{base}/orp_regulation_mode/set` (non retain, souscrits à la connexion) ; les topics state existants sont **inchangés** (retain, rétrocompat dashboards HA). Validation stricte de l'enum (insensible à la casse ; valeur invalide → ignorée + log `warning`), application dans `drainCommandQueue()` (loopTask, sous `configMutex`) par la **même voie que l'UI web** — miroir `ph/orp_enabled` ([ADR-0004](docs/adr/0004-mode-regulation-enum-3-valeurs.md)) — avec persistance NVS et log `info` uniquement si changement réel, puis republication immédiate de l'état (feedback HA). Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant (validé pool-chemistry). L'UI web reste la source principale et reflète un changement initié par HA via les canaux existants (WS + `/get-config`) — aucun code UI ajouté.

### Documentation

- `docs/MQTT.md` : 2 entités `select` mode de régulation (discovery, state/command topics, options) + topics de commande ajoutés aux tableaux ; nouvelle section de détail feature-009.
- `docs/features/page-ph.md`, `page-orp.md` : tableau « Interaction MQTT » complété — mode commandable depuis HA, UI web source principale, reflet via WS.
- `docs/subsystems/mqtt-manager.md` : nouveaux `InboundCmdType::PhRegulationMode` / `OrpRegulationMode`, comportement du handler (validation, miroir ADR-0004, persistance conditionnelle, feedback `publishTargetState()`, logs), cas valeur invalide.

---

## [2.6.0] - 2026-07-04

### Firmware

- **Sécurité — injections manuelles gardées (feature-006)** : les injections manuelles (`POST /ph/inject/start`, `POST /orp/inject/start`) sont désormais soumises aux mêmes limites de sécurité chimique que la régulation automatique — limite journalière **prédictive** (cumul + volume demandé, avec reliquat dans le refus), limite horaire (**budget partagé** avec la régulation auto, [ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md) : le manuel n'était auparavant ni vérifié ni **compté**, permettant jusqu'à 2× le budget/heure), stabilisation post-calibration (par pompe), refus de double démarrage, cycles/jour et anti-rafale (ring **partagé** avec l'auto). Décision extraite en fonction pure testable `evaluateManualInject()` (`dosing_logic`, pattern ADR-0017 ; ordre des gardes et frontières `==` acceptées validés pool-chemistry ; 17 tests natifs dédiés, 135 au total, couverture `dosing_logic` 100 %). Tout refus retourne **409 JSON structuré** `{"error","message","seconds_remaining"?,"remaining_ml"?}` (8 codes) ; la garde filtration des routes de test `/pumpN/on` migre au même format. Les pompes test et commandes UART consomment aussi le budget horaire partagé. `inject/stop` reste un coupe-circuit jamais gardé.
- **fix(limites)** : le drapeau « limite journalière atteinte » (`ph/orp_limit_reached` — badges UI, blocage du bouton d'injection, alarme UART, MQTT) était **verrouillé jusqu'à minuit** : augmenter la limite journalière en cours de journée débloquait bien le dosage réel (gardes en valeurs vives) mais l'UI restait bloquée sur « limite atteinte ». Le drapeau suit désormais la condition vive et se lève dès que `cumul < limite` (limite augmentée ou compteurs réinitialisés) — validé pool-chemistry (indicateur miroir, les gardes bloquantes étaient déjà vives).

### Frontend

- **Injection manuelle — refus détaillés dans l'UI (feature-006)** : toast rouge français par code de refus (reliquat affiché pour `daily_limit`, compte à rebours pour `stabilization_in_progress`) ; bouton « ▶ Injecter » **désactivé proactivement** avec raison en hint (`#ph|orp_inject_block_hint`) quand le blocage est connu côté client (limite journalière, stabilisation, filtration arrêtée). Le firmware reste l'autorité (WS déconnecté → bouton cliquable). Le bouton « ⏹ Arrêter » n'est jamais désactivé.

### Documentation

- Nouvel [ADR-0020](docs/adr/0020-budget-horaire-dosage-unique.md) : budget horaire de dosage unique partagé auto + manuel (alternative « compteur séparé » écartée ; prédicat safety-tracking → pompes test/UART comptent aussi).
- `docs/API.md` : sections `/ph|orp/inject/start` réécrites — tableau des 8 codes de refus 409 JSON, note `/pumpN/on`.
- `docs/features/page-ph.md`, `page-orp.md` : avertissements « limites volumétriques non gardées » retirés (obsolètes) ; gardes, toasts par code et blocage proactif du bouton documentés.
- `docs/subsystems/pump-controller.md` : nouvelle section « Gardes des injections manuelles » (ordre des 9 gardes, frontières figées, budget horaire partagé, pending atomique, asymétrie stabilisation auto/manuel, note bypass UART `pump_test`).

---

## [2.5.2] - 2026-07-04

### Fonctionnalités

- **Infos système — ligne « Firmware (flash) » (feature-047)** : la carte Infos système (Paramètres → Système) affiche l'occupation de la partition app active au format `X KB / Y KB (Z%)` (taille du binaire / taille de la partition OTA courante), au même format que la ligne FS. Rend visible la marge flash gagnée par le repartitionnement layout v3 ([ADR-0019](docs/adr/0019-partition-app-1664k.md)). Côté API, `GET /get-system-info` expose un nouveau champ `sketch_size` (octets, `ESP.getSketchSize()`) ; le champ `ota_partition_size` existait déjà. Affichage `—` si les champs sont absents (compatibilité ancien firmware).

### Documentation

- `docs/API.md` : champs `sketch_size` et `ota_partition_size` ajoutés à l'exemple de `/get-system-info`.
- `docs/features/page-settings.md` : ligne « Firmware (flash) » documentée dans la carte Infos système.

---

## [2.5.1] - 2026-07-04

### Fonctionnalités

- **Paramètres — réorganisation des onglets Système / Avancé (feature-046)** : la carte « Infos système » (Firmware, Build, Uptime, Chip, CPU, Heap libre, Flash, FS, Wi-Fi RSSI, MAC + bouton Actualiser) est **déplacée** de l'onglet Avancé vers la **première position** de l'onglet Système ; la carte « Version du firmware », redondante, est **supprimée** de l'onglet Système (la version reste visible sur la ligne « Firmware » d'Infos système) ; la carte « Historique des mesures (température, pH, ORP) » (export/import/suppression CSV) est **déplacée** de l'onglet Système vers la **première position** de l'onglet Avancé. Frontend uniquement — aucun changement backend (`GET /get-system-info`, `/get-history`, `/history/import`, `/history/clear` inchangés).

### Documentation

- `docs/features/page-settings.md` : contenu des onglets Système et Avancé mis à jour (Infos système en tête de Système, Historique des mesures en tête d'Avancé, position de la card Diagnostic crash dans Avancé).

---

## [2.5.0] - 2026-07-04

### Firmware

- **Retrait des outils de diagnostic d'oscillation pH (feature-045)** : les trois outils ajoutés pour la campagne de diagnostic 2026-05/06, devenus inutiles en exploitation, sont supprimés :
  - **Debug oscillation pH** : carte UI du panel Avancé + ring buffer 300 échantillons (~6 KB RAM) + routes `GET /debug/ph_trace` et `POST /debug/ph_trace_clear` ;
  - **Pause WiFi** : routes `GET`/`POST /debug/wifi_pause` + mécanisme de pause associé ;
  - **Diagnostic EZO** : carte UI + routes `POST /debug/ezo_command` et `POST /debug/ezo_factory`.

  Les 5 routes répondent désormais **404**. Gains mesurés : **Flash −12 272 o**, **RAM statique −6 024 o**, **payload FS minifié −13 809 o**. Restent en place : `/debug/ph_slope_refresh`, `/debug/sensor_filter_reset`, `/debug/sensor_filter_state` (levier de sécurité feature-025) et les routes coredump.

  > **Réintégration** : code récupérable via `git revert` du commit de la feature-045 ; état complet avec les outils figé au tag `v2.4.0`.

### Documentation

- `docs/API.md` : sections « Diagnostic Atlas EZO » et `ph_trace`/`ph_trace_clear` retirées, remplacées par une note « Routes de debug supprimées (v2.5.0) ».
- `docs/features/page-settings.md` : cartes « Debug oscillation pH » et « Diagnostic EZO » retirées du panel Avancé (note de réintégration ajoutée).
- `docs/BUILD.md` : décompte des graphiques uPlot ramené à 6 (le chart debug oscillation est retiré).

---

## [2.4.0] - 2026-07-04

### Firmware

- **Repartitionnement flash — layout v3 (feature-044)** : les slots firmware `app0`/`app1` passent de 1536 à **1664 KB** (+128 KB chacun), pris sur la partition `spiffs` réduite de 832 à **576 KB** (offset déplacé à `0x350000`) — rendu possible par la migration uPlot (feature-043, payload FS 449 KB). Occupation firmware : 90,8 % → **83,8 %** (1 427 753 / 1 703 936 o, marge ~270 KB) ; FS à ~82 % de 576 KB (marge ~104 KB). Partitions `nvs`, `history` et `coredump` strictement inchangées : **config, historique et coredump préservés**. ⚠️ **Mise à jour par câble USB requise une seule fois** (`./deploy.sh all`) — l'OTA ne peut pas réécrire la table de partitions ; ne pas utiliser `./deploy.sh factory` (efface la NVS). L'OTA redevient normal ensuite. Fichiers alignés : `partitions.csv`, `platformio.ini` (`filesystem_size = 589824`), `build_fs.sh`, `deploy.sh`.
- **`deploy.sh`** : correction du check de taille de `littlefs.bin` resté au layout v1 (1 114 112 → 589 824 octets) — il déclenchait à tort une reconstruction du FS à chaque upload.

### Documentation

- Nouvel [ADR-0019](docs/adr/0019-partition-app-1664k.md) : partitions app à 1664 KB (layout v3) — contexte (app0 à 90,8 %, gains `CORE_DEBUG_LEVEL` −33 KB et uPlot −148 KB FS), alternatives écartées (palier +64 KB conservateur ; +192 KB impossible, FS à 100 %), conséquences (flash USB unique, données préservées). [ADR-0015](docs/adr/0015-partition-app-1.5mb.md) marqué « Superseded by ADR-0019 ».
- `docs/BUILD.md` : taille filesystem 589 824 o = 576 KB (layout v3, ADR-0019), piège « changement de table = flash USB », occupation firmware actualisée.
- `docs/UPDATE_GUIDE.md` : nouvelle note « Migration layout v2 → v3 (v2.4.0) » — procédure USB, préservé/réécrit, avertissement `factory`.

---

## [2.3.0] - 2026-07-04

### Frontend

- **Graphiques — migration Chart.js → uPlot (feature-043)** : la bibliothèque de graphiques passe de Chart.js v4.5.1 (`chart.umd.min.js`, ~208 KB, **supprimé**) à **uPlot 1.6.32** (`uPlot.iife.min.js` + `uPlot.min.css`, paquet npm `uplot@1.6.32` figé, committés dans `data/` — offline, sans bundler). **Rendu à parité stricte** sur les 7 graphiques (3 mini-charts dashboard avec coloration conditionnelle par segment et dégradé de remplissage, 3 historiques détail pH/ORP/Température avec bandes min/max, 1 debug oscillation multi-axes avec séries masquées par défaut) : labels X catégoriels (« Aujourd'hui », mois français, `HH:MM`, `-Ns`), tooltip, boutons de plage synchronisés, gel pendant calibration et polling incrémental 5 min conservés. **Payload FS réduit de −148,3 KB** (601 054 → 449 177 octets, ≈ 449 KB / partition `spiffs` 832 KB) — prépare le repartitionnement `app0`/`app1`. Les options mortes de l'ancienne usine `createLineChart` (`hideYAxis`, `showYAxisGrid`, `extraPlugins`, `annotation`, `backgroundColor`) ne sont pas reprises.

### Documentation

- Nouvel [ADR-0018](docs/adr/0018-migration-uplot.md) : uPlot au lieu de Chart.js (contrainte Flash/FS — alternatives Chart.js slim et statu quo écartées).
- `docs/BUILD.md` : nouvelle section « Bibliothèque de graphiques uPlot » (provenance npm `uplot@1.6.32` figé, procédure de mise à jour manuelle, gain mesuré, options non reprises) + correction de la taille de la partition `spiffs` (832 KB, layout v2 — la mention 1088 KB était obsolète).
- `docs/features/page-dashboard.md`, `page-ph.md`, `page-orp.md`, `page-temperature.md`, `page-settings.md` : mentions Chart.js remplacées par uPlot.

---

## [2.2.11] - 2026-06-27

### Firmware

- **Refactor interne (feature-041) — math d'agrégation de l'historique testable** : la math **scalaire** d'agrégation est extraite de `history.cpp::consolidateData()` vers le module **pur** `src/history_logic.{h,cpp}` (headers C uniquement `<stdint.h>`/`<math.h>`, sans `<vector>`/`<map>`/Arduino/FreeRTOS, pattern *Humble Object*) : `bucketTimestamp` (troncature au bucket horaire/quotidien, garde `bucketSeconds==0`), `isOlderThan` (prédicat d'ancienneté `(now-ts) > maxAge`, frontière **stricte**, wrap `uint32` conservé), `finalizeMean` (moyenne, `count==0` → `NaN`), `isMajority` (majorité stricte `> total/2`, division entière), `anyTrue` (`>0`). Les deux passes (raw → horaire, horaire → quotidien) **délèguent** ; le regroupement `std::map`/`std::vector`, l'accumulation et la persistance restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (frontières strictes, divisions entières et wrap reproduits à l'identique — revue **Approved**). Aucun comportement visible utilisateur, aucun endpoint / WS / MQTT touché.

### Tests

- **25 tests Unity natifs** (feature-041) couvrant la math d'agrégation : troncature au bucket (garde division par zéro), prédicat d'ancienneté (frontière `==`, wrap post-`0xFFFFFFFF`), moyenne (`count==0` → `NaN`), majorité stricte (`2/4`, `3/4`, `3/5`) et `anyTrue`. `history_logic.cpp` couvert à **100 % des lignes** (**118 tests au total**). `coverage.sh` et `build_src_filter` étendus au nouveau module. Le regroupement `std::vector`/`std::map` de `consolidateData()` reste **hors tests natifs** (contrainte libc++ absente en natif).

### Documentation

- `docs/subsystems/history.md` : nouvelle section « Math d'agrégation pure (`history_logic`) » — les 5 fonctions pures, leurs frontières (strictes, divisions entières, wrap `uint32`, `NaN`), la délégation des deux passes, et la note explicite que le regroupement `std::vector`/`std::map` reste hors tests natifs. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.10] - 2026-06-27

### Firmware

- **Refactor interne (feature-040) — logique d'horaire de l'éclairage testable** : la décision d'horaire de l'éclairage est extraite de `lighting.cpp` vers le module **pur** `src/schedule_logic.{h,cpp}` (créé en feature-038, **réutilisé** ici). Nouvelle fonction `decideLightingOn(manualOverride, enabledFlag, scheduleEnabled, haveTime, nowMin, startMin, endMin, currentlyOn)` ; `isMinutesInRange` gagne un 4ᵉ paramètre `bool equalMeansAlways = false`. La divergence `start == end` entre les deux domaines est **intentionnellement préservée** : filtration → `false` (plage invalide, jamais) ; éclairage → `true` (allumé toute la journée). `lighting.cpp::update()` devient une coquille mince déléguant ; RTC/`millis()`/`digitalWrite`/`publishState`/MQTT restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (ni éclairage ni filtration — la valeur par défaut `false` garantit la non-régression de la filtration). Aucun comportement visible utilisateur, aucun endpoint / WS / MQTT touché.

### Tests

- **11 nouveaux tests Unity natifs** (feature-040) couvrant la décision d'horaire éclairage : équivalence stricte de `decideLightingOn`, divergence `start == end` (éclairage → `true`, filtration → `false`), fenêtre simple et fenêtre franchissant minuit, priorités manuel/horaire, conservation de l'état quand l'heure est indisponible. `schedule_logic.cpp` reste couvert à **100 % des lignes** (**93 tests au total**).

### Documentation

- `docs/subsystems/lighting.md` : nouvelle section « Logique d'horaire pure (`schedule_logic`) » — `decideLightingOn`, paramètre `equalMeansAlways`, divergence intentionnelle `start==end` (filtration `false` / éclairage `true`), non-régression filtration, testabilité native. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.
- `docs/subsystems/filtration.md` : la ligne `isMinutesInRange` documente le 4ᵉ paramètre `equalMeansAlways` et la divergence éclairage.

---

## [2.2.9] - 2026-06-27

### Firmware

- **Refactor interne (feature-039) — anti-rafale & rollover journalier testables** : la logique d'**anti-rafale** du dosage (comptage de cycles sur fenêtres glissantes 1 min / 15 min via ring buffer, gestion du **wrap `millis()`** en `uint32_t`, écriture circulaire) et les **déclencheurs de rollover journalier** (reset des quotas à minuit date NTP / fallback 24 h) sont extraits de `pump_controller.cpp` vers le module **pur** `src/dosing_logic.{h,cpp}` : `countCyclesInWindow`, `recordCycleTimestamp`, `shouldRolloverByDate`, `shouldRolloverByMillis` — sans dépendance Arduino / `millis()` / `time()` / NVS / état membre, donc **testables en natif**. Les méthodes `recordDosingCycleStart` / `countRecentDosingCycles` / `tickDailyRollover` deviennent des coquilles minces (fournissent `millis()` / `time()` / les buffers et délèguent ; `localtime_r`/`strftime`, écriture `safetyLimits`, `saveDailyCounters`, `armStabilizationTimer`, logs et branche première-init restent dans la coquille). *Characterization refactor* : **seuils et frontières strictement préservés** (6 cycles/min, 20 cycles/15 min, ring buffer 20, rollover 24 h) — la garde anti-emballement de `canDose()` (#14/#15) et le reset des quotas sont inchangés (équivalence stricte validée par `pool-chemistry`, 2 passages). Aucun comportement visible utilisateur.

### Tests

- **15 nouveaux tests Unity natifs** (feature-039) couvrant l'anti-rafale et le rollover : comptage en fenêtre glissante (buffer vide, slots à 0, `ts` dans/hors fenêtre, `ts == now`, **wrap `millis()` post-`0xFFFFFFFF`**), écriture circulaire du ring buffer (avance d'index, écrasement du plus ancien), déclencheurs de rollover par date (vide / identique / différente) et par fallback 24 h (frontière `== 86400000`, wrap). `dosing_logic.cpp` couvert à **100 % des lignes** (**85 tests au total**).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Anti-rafale & rollover journalier (logique pure) » — les 4 fonctions pures, comptage wrap-safe `uint32_t`, déclencheurs de rollover (date NTP / fallback 24 h), coquilles `pump_controller` qui délèguent, comportement strictement préservé, testabilité native. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.8] - 2026-06-27

### Firmware

- **Refactor interne (feature-038) — logique d'horaire de filtration testable** : la décision d'horaire de `filtration` (parsing `"HH:MM"` `timeStringToMinutes`, appartenance à une fenêtre `isMinutesInRange` gérant le passage minuit, calcul du créneau auto selon température `computeAutoWindow` — durée = temp/2 bornée `[1,24]`, centrée sur `kFiltrationPivotHour`, wrap des bornes dans `[0,24)` —, et décision marche/arrêt `decideFiltrationRun` avec priorités **`forceOn` > `forceOff` > plage** et **conservation de l'état si l'heure est indisponible**) est extraite vers un module **pur** `src/schedule_logic.{h,cpp}` (pattern *Humble Object*), sans dépendance Arduino / RTC / `millis()` / NVS / FreeRTOS — donc **testable en natif**. Module **générique**, réutilisable pour l'éclairage. `update()` / `begin()` / `computeAutoSchedule()` deviennent des coquilles minces ; RTC, `millis()`, NVS, deadband 1 °C, timeout 4 h, stabilisation et commande relais restent dans la coquille. *Characterization refactor* : **aucun changement de comportement** (équivalence stricte validée par `pool-chemistry`, dont la préservation de la garde « présence d'eau » de `canDose()`). Aucun comportement visible utilisateur.

### Tests

- **19 nouveaux tests Unity natifs** (feature-038) couvrant la décision d'horaire : parsing `"HH:MM"` (cas invalides inclus), appartenance à une fenêtre traversant minuit, créneau auto borné/centré, priorités des forçages et conservation de l'état sans heure valide. `schedule_logic.cpp` couvert à **100 % des lignes** (**70 tests au total**).

### Documentation

- `docs/subsystems/filtration.md` : nouvelle section « Logique d'horaire pure (`schedule_logic`) » — les 4 fonctions pures, décision/priorités, temps indisponible → conservation d'état, fenêtre auto centrée et wrap minuit, testabilité native, coquilles `update()`/`begin()`/`computeAutoSchedule()`. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR** ; `schedule_logic` est un module générique partagé filtration/éclairage.

---

## [2.2.7] - 2026-06-27

### Firmware

- **Refactor interne (feature-037) — calcul proportionnel testable** : le cœur du calcul PID de `pump_controller` (terme proportionnel `kp × error`, termes `Ki`/`Kd`, anti-windup par gel/bornage de l'intégrale à `±integralMax`, plancher sortie négative → 0, **bornage final min/max**) est extrait de `computePID` vers une fonction **pure** `computePidPure` (`src/dosing_logic.{h,cpp}`), sans dépendance Arduino / `millis()` / état membre, donc **testable en natif**. L'état PID (intégrale + dernière erreur) est désormais **injecté en paramètre** et **renvoyé** via `struct PidResult`. Le `constrain` de débit qui était appliqué chez les appelants pH/ORP est déplacé **dans la fonction pure** (« Option Y ») : `computePidPure` renvoie le débit **final borné**, et `computePID` ainsi que les deux chemins pH/ORP deviennent des coquilles minces (gestion du temps + état uniquement). `computeFlowFromError` reste du code mort (non extrait). *Characterization refactor* : **les débits calculés et l'évolution de l'intégrale sont strictement préservés** (équivalence stricte validée par `pool-chemistry`). Aucun comportement visible utilisateur.

### Tests

- **Tests Unity natifs étendus (feature-037)** : couverture **100 % des lignes** de `computePidPure` (21 tests et plus) — terme proportionnel, gel de l'intégrale, bornage anti-windup `±integralMax`, plancher 0, bornage final min/max. Le temps et l'état PID étant injectés, chaque branche est exercée sans matériel ni attente réelle. `pio test -e native` continue de couvrir les suites `test/test_native_sensor_filter/` + `test/test_native_dosing/`.

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Calcul proportionnel pur (`computePidPure`) » — fonction pure, état PID injecté/renvoyé (`PidResult`), bornage final intégré (« Option Y »), testabilité native 100 %, `computePID` et chemins pH/ORP réduits à des coquilles. Réutilise [ADR-0017](docs/adr/0017-logique-metier-pure-humble-object-testabilite.md) (Humble Object) — **pas de nouvel ADR**.

---

## [2.2.6] - 2026-06-27

### Firmware

- **Refactor interne (feature-036) — décision de dosage testable** : la logique de décision « peut-on doser ? » de `pump_controller` est extraite dans un module **pur** `src/dosing_logic.{h,cpp}` (pattern *Humble Object*), sans dépendance Arduino / FreeRTOS / I²C, donc **testable en natif**. `canDose()` devient une coquille mince qui collecte les globals, délègue à `evaluateDose(DoseInputs) → DoseDecision`, puis mappe l'énum de cause vers la chaîne française exposée (valeurs runtime réinjectées). *Characterization refactor* : **aucun seuil, verdict, cause de refus ni ordre d'évaluation des gardes n'a changé** (équivalence stricte validée par `pool-chemistry`). Hystérésis de démarrage/arrêt et temps minimum d'injection également exposés en fonctions pures (`shouldStartDosingPure` / `shouldContinueDosingPure`, temps injecté). Aucun comportement visible utilisateur.

### Tests

- **21 nouveaux tests Unity natifs** (feature-036) couvrant la décision de dosage : chaque cause de refus de `evaluateDose`, l'hystérésis start/stop aux bornes (deadband), et un **verrou de non-régression** du bug pause-mélange v2.2.5 (une injection auto dure ≥ `minInjectionTimeMs` avant l'armement de la pause). `pio test -e native` couvre désormais deux suites (`test/test_native_sensor_filter/` + `test/test_native_dosing/`).

### Documentation

- `docs/subsystems/pump-controller.md` : nouvelle section « Décision de dosage : logique pure (`src/dosing_logic`) vs coquille hardware » (frontière POD/String, table énum `DoseRefusal` ↔ cause FR, fonctions pures). Correction d'une imprécision préexistante : limite horaire évaluée avec `>=` (et non `>`).
- `docs/BUILD.md` : section « Tests natifs (hors matériel) » — `pio test -e native` couvre deux suites, `build_src_filter` inclut `sensor_filter.cpp` + `dosing_logic.cpp`.
- `docs/adr/0017-logique-metier-pure-humble-object-testabilite.md` : nouvel ADR « Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native » (lié à ADR-0016 et feature-025).

---

## [2.2.5] - 2026-06-23

### Firmware

- **Corrections — régulation pH/ORP auto** : la pause mélange hydraulique (15 min pH / 20 min ORP, feature-025) était armée par erreur **au démarrage** de l'injection. Conséquence : la garde `canDose()` (« pause mélange en cours », #5b/#6b) sautait la branche de régulation au cycle `update()` suivant, coupant la pompe après ~un cycle de boucle. Les injections automatiques ne duraient jamais le minimum prévu (`minInjectionTimeMs` = 30 s) → régulation inefficace (le pH/ORP ne convergeait pas vers la cible, chip « Mélange en cours » affiché en continu). La pause est désormais armée à l'**arrêt** de l'injection (homogénéisation post-dose) : `notifyPhDose()` / `notifyOrpDose()` sont appelés quand l'injection en cours se termine, sans interrompre celle-ci. `minInjectionTimeMs` et `shouldContinueDosing` sont de nouveau effectifs. Bug introduit par feature-025 ; **fail-safe** (sous-dosage, aucun risque de surdosage).

### Documentation

- `docs/subsystems/pump-controller.md` : section « Pause mélange hydraulique » corrigée — pause armée à l'arrêt de l'injection (et non au démarrage), interaction avec `minInjectionTimeMs` / `shouldContinueDosing` précisée.

---

## [2.2.4] - 2026-06-23

### Fonctionnalités

- **Uniformisation de l'écran Température** (feature-035) : l'écran Température adopte le même agencement que pH/ORP — un **bloc « stats » en haut** (valeur courante + **chip d'état de calibration** + bouton **« Calibrer la sonde »**), et une **carte de calibration masquée** qui s'affiche au clic (masque les cartes Activation + Historique, bouton **Fermer** pour revenir). La calibration reste **par offset 1 point** (saisie d'une température de référence connue → offset, via `/save-config`) : logique inchangée. Le chip affiche « Calibré · aujourd'hui / il y a N j / le JJ/MM » (vert), « Calibré · ancien (il y a N mois) » si > 6 mois (ambré), ou « Non calibré » (gris). La lecture live de la température reste visible pendant la calibration (pas d'indicateur de stabilité — température lente). Les anciens callouts (`#temp_cal_date_header` / `#temp_calibrated_status`) sont **supprimés** au profit du chip.
- **Désactivation de la sonde** (feature-035) : décocher l'activation masque désormais correctement la carte du tableau de bord ainsi que les blocs stats/historique/calibration de la page Température (la carte d'activation reste accessible pour réactiver).

### Documentation

- `docs/features/page-temperature.md` : réécriture pour le nouvel agencement (feature-035) — bloc stats (valeur + chip de calibration), bouton « Calibrer la sonde », carte de calibration masquée (stepper offset + lecture live + Fermer), états du chip, données consommées et cas limites.

---

## [2.2.3] - 2026-06-23

### Fonctionnalités

- **Calibration guidée pH/ORP accessible dans tous les modes** (feature-034) : la calibration des sondes pH et ORP est désormais accessible quel que soit le mode de régulation (**automatique, programmée, manuelle**) — auparavant limitée au mode automatique. Le mode en cours est **conservé** à la sortie de la calibration (plus de bascule forcée en automatique).
- **Écran de calibration guidé** (feature-034) : nouvel accompagnement par **stepper** avec états visuels (étape faite ✓ / en cours / restante, `aria-current`), **minuterie de stabilisation** par étape d'attente (compte à rebours 60 s, non bloquant — on peut calibrer avant la fin), et **indicateur de stabilité Δ60 s** (amplitude max−min de la mesure brute, purement indicatif). Le readout reste la valeur **brute** (feature-025, non régressé).
- **Sécurité (feature-034)** : le bouton « Calibrer » est désormais aussi bloqué pendant une **injection pH** en cours (le garde n'existait que côté ORP).
- **Chip d'état de calibration** (feature-034, itération 2) : sur les pages pH et ORP, l'état de calibration est désormais résumé par un **chip** dans la rangée de chips (à côté des chips sonde et filtre) — états « Calibré 2/2 » (pH) / « Calibré » (ORP) en vert, « Calibration 1/2 » en ambré (pH partiel), « Calibration requise » en rouge (régulation auto inhibée), « EZO indisponible » en gris, et « Calibration — » tant que les données ne sont pas reçues. Les **3 anciens callouts** (calibré / régulation inhibée / EZO injoignable) sont **supprimés** ; leur information est portée par le chip + un hint texte.
- **Bouton « Calibrer la sonde » repositionné** (feature-034, itération 3) : sur les pages pH et ORP, le bouton de calibration est désormais affiché **sous la rangée de chips** (dans le bloc Statistiques) avec un **libellé fixe « Calibrer la sonde »** (remplace les libellés adaptatifs « Calibrer » / « Continuer la calibration » / « Recalibrer » de l'itération 2). L'état de calibration reste porté par le chip + le hint. Les blocs `#ph-calibration-info` / `#orp-calibration-info` en bas de la carte de régulation sont **retirés**. Le bouton reste **toujours accessible** dans tous les modes (recalibration possible) et **désactivé** pendant une injection en cours.
- **Chip pH unique « sonde + calibration »** (feature-034) : sur la page pH, le chip de calibration séparé est **supprimé** ; son information est **fusionnée** dans le chip sonde (`#ph-probe-chip`) pour lever la redondance. Le chip pH affiche « EZO indisponible » (gris) si EZO injoignable, « Calibration requise » (rouge) si 0 point, « Calibration 1/2 » (ambré) si 1 point, et le **diagnostic de pente** habituel (« Sonde excellente / correcte / usée / à remplacer · N% ») dès 2 points calibrés. La page ORP conserve son chip de calibration dédié (pas de chip sonde côté ORP). Le bouton « Calibrer la sonde » et son hint restent inchangés.

### Documentation

- `docs/features/page-ph.md` / `docs/features/page-orp.md` : calibration accessible tous modes, écran guidé (stepper d'états, minuterie de stabilisation, indicateur de stabilité Δ60 s), garde injection, **chip d'état de calibration** remplaçant les callouts (itération 2), et **bouton « Calibrer la sonde »** repositionné sous les chips avec libellé fixe (itération 3, volet A ; volet B « calibration périmée » abandonné).
- `docs/subsystems/sensors.md` : précision que le reset filtre + warmup post-calibration est **mode-indépendant** (déclenché par le succès EZO).

---

## [2.2.2] - 2026-06-21

### Firmware

- **fix(sensors)/tune** : délai de re-synchronisation du filtre réduit de ~2 min à ~1 min (feature-033) — `kSensorFilterResyncRejects` 24 → 12. Invariant fail-closed préservé (12 > 10 instable, 12 > 7 fenêtre médiane).

### Documentation

- `docs/subsystems/sensors.md` : seuil et délai de re-synchronisation (24 → 12, ~120 s → ~60 s).

---

## [2.2.1] - 2026-06-20

### Firmware

- **fix(sensors)** : correction du gel de la valeur filtrée après immersion en solution de calibration (re-synchronisation + anti-boucle). Un changement réel et durable au-delà de `maxStep` (ex. sonde plongée en solution puis retour en bassin) figeait `filtered` à vie → dosage bloqué en permanence. Le filtre se ré-amorce désormais sur la médiane des bruts rejetés après `kSensorFilterResyncRejects` (= 24, ≈ 120 s) rejets consécutifs, puis repart en warmup. Anti-boucle EMI : au-delà de `kSensorFilterMaxResyncPerWindow` (= 3) re-sync sur `kSensorFilterResyncWindowMs` (= 10 min), le capteur est déclaré instable de façon **latchée** jusqu'à un `reset()` explicite (`POST /debug/sensor_filter_reset` ou calibration réussie). Gardes `canDose()` inchangées.

### Fonctionnalités

- **fix(calibration)** : le readout live de calibration pH/ORP affiche la valeur brute (la valeur lissée figeait l'affichage en changeant de solution étalon).

### Documentation

- `docs/subsystems/sensors.md` : sections re-synchronisation (latch-up résolu) et anti-boucle latché, 3 nouvelles constantes, getters `resyncCount()` / `unstableLatched()`, levée du latch par `reset()`.

---

## [2.2.0] - 2026-06-16

### Fonctionnalités

- **Lissage des mesures pH/ORP** (feature-025) : les pages pH et ORP affichent désormais la valeur **filtrée** (médiane glissante + moyenne exponentielle) en grand, avec une ligne discrète « brut · médiane · maj » pour le diagnostic. Un **chip d'état filtre** à 5 états (`Stabilisation…` / `Mesure stable` / `Pics rejetés` / `Capteur instable` / `EZO indisponible`) résume la santé de la chaîne de mesure ; un clic ouvre un modal détail (brut/médiane/filtré, rejets, âge, raison de blocage du dosage).
- **Debug oscillation pH** : la courbe de diagnostic du panel Avancé trace désormais **deux séries pH** — « pH brut » (bleu) et « pH lissé » (orange) — sur le même axe, avec une ligne de stats « pH lissé min / max / Δ » en complément du brut. Permet de visualiser l'effet du filtre feature-025. Le payload de `GET /debug/ph_trace` expose le nouveau champ `phFiltered` par échantillon.

### Firmware

- **Filtrage capteurs** (feature-025) : nouvelle chaîne `mesure brute Atlas EZO → rejet aberrants → médiane (fenêtre 7) → EMA (alpha pH 0.10 / ORP 0.08) → valeur filtrée`. Classe dédiée `SensorFilter` déterministe (buffer fixe, testable hors matériel). Valeurs brutes conservées pour diagnostic EMI. Filtre réinitialisé (warmup) après calibration pH/ORP réussie. Paramètres centralisés dans `constants.h` (`kSensorFilter*`, `kPhEmaAlpha`, `kOrpEmaAlpha`, plages plausibles, seuils de rejet).
- **Régulation P temporisée** (feature-025, [ADR-0016](docs/adr/0016-regulation-p-temporisee-vs-pid.md)) : le PID consomme désormais la mesure **filtrée et prête** (jamais la brute). Stratégie par défaut proportionnelle pure (`Kp` pH=8 / ORP=0.3, `Ki=0`, `Kd=0` impératif). Anti-windup strict, zone morte = seuil de démarrage, nouvelles gardes `canDose()` fail-closed (filtre non prêt, capteur instable, pause mélange).
- **Pause mélange hydraulique** (feature-025) : après chaque injection, blocage de toute nouvelle décision de dosage le temps de l'homogénéisation du bassin — `kPhMixingDelayMs = 15 min`, `kOrpMixingDelayMs = 20 min` (gérée par timestamps, pas de `delay()`).
- **MQTT** : nouveaux topics retain `{base}/ph|orp_raw|median|filtered|filter_ready|filter_unstable|rejected_count` + `{base}/ph|orp_mixing_delay_active`. Les topics `{base}/ph` et `{base}/orp` publient désormais la valeur filtrée. Auto-discovery HA : sensors « pH/ORP Brut » et « pH/ORP Filtré » + binary_sensors « Filtre pH/ORP Prêt ».

### API

- **WebSocket / `GET /data`** : champs `phRaw/phMedian/phFiltered/phFilterReady/phFilterUnstable/phRejectedCount/phMixingDelayActive/phDoseBlockedReason` (+ équivalents `orp*`). `ph` / `orp` correspondent désormais à la valeur **filtrée** (fallback brut tant que le filtre n'est pas amorcé).
- **`POST /debug/sensor_filter_reset`** : réinitialise les deux filtres (warmup). **`GET /debug/sensor_filter_state`** : état JSON brut des filtres.

### Documentation

- `docs/subsystems/sensors.md` : chaîne de filtrage, classe `SensorFilter`, paramètres, reset après calibration, états warmup/unstable.
- `docs/subsystems/pump-controller.md` : entrée PID filtrée, régulation P temporisée, anti-windup, zone morte, pause mélange, nouvelles gardes `canDose()`.
- `docs/MQTT.md` / `docs/API.md` : nouveaux topics, champs WS et endpoints debug.
- `docs/features/page-ph.md` / `docs/features/page-orp.md` : affichage filtré/brut, chip d'état filtre, modal détail.
- `docs/features/page-settings.md` / `docs/API.md` : card « Debug oscillation pH » à 2 courbes (brut + lissé), champ `phFiltered` du payload `GET /debug/ph_trace`.
- **ADR-0016** créé : régulation P temporisée par défaut (Kp seul, Ki=0, Kd=0) sur mesure filtrée vs PID complet.

---

## [2.1.2] - 2026-05-10

### Sécurité chimique

- **Garde filtration sur l'injection manuelle** — bug observé en production : injection manuelle volumée pH lancée pendant filtration active continuait après l'arrêt programmé de la filtration → surdosage local d'acide dans la zone du retour d'eau (pas de circulation). Refonte de [`src/web_routes_control.cpp`](src/web_routes_control.cpp) avec deux mitigations :
  - **Refus en amont (HTTP 409)** — helper privé `injectionAllowedOrReject(req, tag)` appelé en début des handlers `/ph/inject/start`, `/orp/inject/start`, `/pump1/on`, `/pump2/on`. Vérifie `mqttCfg.regulationMode == "continu" || filtration.isRunning()` (même critère que `PumpController::canDose()`, cohérence des deux gardes). Refus = HTTP `409 Conflict` avec corps texte explicite. Les handlers d'arrêt (`/ph/inject/stop`, `/orp/inject/stop`, `/pump[12]/off`) restent **inconditionnels** — pouvoir arrêter en toute circonstance.
  - **Arrêt cyclique en cours d'injection** — `updateManualInject()` (appelée à chaque tour `loopTask`) interrompt l'injection si la filtration tombe pendant celle-ci. Latence < 100 ms après détection. Logue `critical("[Injection] {pH|ORP} INTERROMPUE — filtration arrêtée (sécurité chimique)")` et publie l'alerte MQTT correspondante.
- **Bornage durée injection 3600 → 600 s** — nouvelle constante `kManualInjectMaxDurationS = 600` ([`src/constants.h`](src/constants.h)) appliquée aux deux routes inject/start. Justification pool-chemistry : 3600 s trop long en cas d'arrêt filtration en milieu de cycle ; 10 min couvrent les usages typiques et l'utilisateur peut toujours relancer.
- **Pas de reprise automatique** après reprise filtration — choix produit. L'injection en cours est perdue, l'utilisateur doit relancer manuellement (toast UI explicite).

> Validation `pool-chemistry` : GO sous conditions, **toutes appliquées**. Cohérence avec la condition #3 de `canDose()` (filtration en marche sauf mode `continu`) — voir [docs/subsystems/pump-controller.md](docs/subsystems/pump-controller.md).

### API

- **`POST /ph/inject/start`, `POST /orp/inject/start`, `POST /pump1/on`, `POST /pump2/on`** : nouveau code retour **`409 Conflict`** « filtration arrêtée — injection refusée pour sécurité chimique » (sauf si `regulationMode == "continu"`). Voir [`docs/API.md`](docs/API.md).
- **`POST /ph/inject/start`, `POST /orp/inject/start`** : paramètre `duration` désormais borné à **600 s** (`kManualInjectMaxDurationS`) au lieu de 3600 s.

### MQTT

Deux nouveaux types d'alerte sur le topic existant `{base}/alerts` (QoS 0, no retain — mécanisme `mqttManager.publishAlert()` inchangé). Aucun nouveau topic, aucune nouvelle entité auto-discovery HA — l'utilisateur peut créer une automation HA sur le topic alerts.

| Type | Condition |
|------|-----------|
| `ph_injection_aborted` | Injection manuelle pH interrompue par la sécurité chimique (filtration arrêtée pendant injection) |
| `orp_injection_aborted` | Injection manuelle ORP/chlore interrompue par la sécurité chimique |

### Frontend

- **Refus 409 au démarrage** : `startInject(product)` dans [`data/app.js`](data/app.js) restaure le bouton et affiche un toast rouge « Injection refusée : la filtration doit être active avant d'injecter (sécurité chimique : pas de circulation = surdosage local). »
- **Interruption en cours** : `_onWsLog(entry)` détecte `entry.level === 'CRITICAL'` + message contenant `[Injection]` et `INTERROMPUE` (capté via le canal WS log existant `broadcastLog()`, pas de nouveau message WS) → toast rouge « Injection {pH|ORP/chlore} interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. »
- **Erreurs HTTP autres** : lecture du body texte et affichage si court (< 200 chars), sinon message générique.

### Documentation

- `docs/API.md` : nouveau code 409 documenté sur les 4 routes concernées, mention du nouveau bornage `kManualInjectMaxDurationS = 600 s`, encart sur l'arrêt cyclique automatique et l'alerte MQTT émise.
- `docs/MQTT.md` : ajout des deux types d'alertes `ph_injection_aborted` et `orp_injection_aborted` dans la table des alertes.
- `docs/subsystems/pump-controller.md` : nouvel encart « Garde filtration injection manuelle » dans la section sécurité chimique, ajout de `kManualInjectMaxDurationS` au tableau des constantes.
- `docs/features/page-ph.md` et `docs/features/page-orp.md` : section comportement UI mise à jour (toasts, refus, interruption), avertissement « injection manuelle non gardée » remplacé par la documentation de la nouvelle garde filtration.

### Notes

- Aucun ADR créé : c'est une mitigation d'un bug de sécurité (ajout d'une garde manquante). Pas de contrainte structurante, pas d'alternatives crédibles à arbitrer (`pool-chemistry` impose la garde), pas de verrouillage long terme.
- Pas de spec formelle — fix réactif sur bug observé en production. Le rapport `pool-chemistry` (GO sous conditions) tient lieu de cadrage.

---

## [2.1.1] - 2026-05-10

### Firmware

- **Détection corruption LittleFS au boot** (`3c8b657`) — `HistoryManager::begin()` force maintenant un test write/read sur fichier témoin `/.fscheck` après le mount LittleFS de la partition `history`. Si l'opération échoue (open / write / read / compare KO), la partition est effacée via `esp_partition_erase_range` puis remontée propre. Garde-fou contre un bug LittleFS connu : un FS corrompu peut être monté sans erreur puis crasher en `IntegerDivideByZero` à la 1ʳᵉ écriture (observé sur ESP32 PCB v1 après une longue inactivité). Logs : warning « Partition history corrompue détectée — reformatage automatique » + info « Partition history reformatée et remontée ». ⚠️ L'historique persistant est perdu si le reformatage est déclenché. Coût négligeable en cas nominal (~10 ms, ~500 B flash).
- **Underflow uint32 dans la garde 24 h Slope query** (`933f17c`, feature-024) — `SensorManager::update()` étape 5 calculait `(now - _phSlopeQueriedMs)` avec `now` figé en début de fonction. Après `_processEzoQueue()` qui peut bloquer ~900 ms, le handler met `_phSlopeQueriedMs = millis()` à un instant postérieur à `now` → underflow uint32 → ~4,3 milliards → toujours ≥ 86 400 000 → ré-enqueue immédiat → spam de query `Slope,?` ~1/s, monopolisation du mutex I²C, EZO ORP perturbé (logs « bus I²C dégradé » récurrents). Fix : recalcul de `now` après `_processEzoQueue()` + garde anti-underflow `nowAfterQueue >= _phSlopeQueriedMs`.
- **EZO ORP utilise `R` au lieu de `RT,<temp>`** (`c0f2962`) — `AtlasEzoSensor::readSingle()` envoyait indistinctement `RT,<temp>` aux deux modules Atlas. Sur l'EZO pH (0x63), cette commande compense la T° (Nernst) ET retourne la valeur compensée (statut 1 + payload). Sur l'EZO ORP (0x62), elle est ACCEPTÉE (statut 1 success) mais NE RETOURNE PAS de payload — l'ORP est potentiométrique direct sans compensation T°. Le firmware attendait un payload qui ne venait jamais → fail streak → bus I²C dégradé après 2 cycles → régulation ORP inhibée. Fix : différenciation par adresse I²C — `kEzoPhAddress` → `RT,<temp>`, `kEzoOrpAddress` → `R`. Validation empirique via `/debug/ezo_command` : `0x62 RT,25.0 → status=1 resp=""` vs `0x62 R → status=1 resp="-369.2"`.
- **Rate limit 30 → 120 req/min** (`6c79cfc`) — `kMaxRequestsPerMinute` passé de 30 à 120 (`src/constants.h`). Ancienne valeur trop basse pour navigation UI active normale (page /params ouverte + polls `/get-config` + `/data` + `/coredump/info` + clics → 30-45 req/min observés → warnings « Rate limit dépassé » fréquents). 120 req/min = 2 req/s moyenne reste une protection efficace contre brute-force auth (des années pour craquer un mot de passe 8 caractères) et DOS accidentel. WebSocket `/ws` non comptabilisé (connexion permanente).

### Fonctionnalités

- **Outil diagnostic EZO + endpoint factory reset** (`20e4a9b`) — 2 nouveaux endpoints HTTP (pas d'auth, cohérent avec les autres `/debug/*`) :
  - `POST /debug/ezo_command` — envoie une commande Atlas EZO arbitraire (1-30 chars) à n'importe quelle adresse I²C (`8..119` décimal = `0x08..0x77`) avec délai d'attente paramétrable (50-5000 ms). Retourne `{success, addr, cmd, status_code, status_label, response, raw_hex, delay_ms}`. Permet de diagnostiquer un module qui ne répond pas comme attendu, valider une réponse vide silencieuse, préparer un RMA Atlas avec preuves reproductibles.
  - `POST /debug/ezo_factory?addr=N` — restaure les paramètres usine d'un module Atlas EZO (calibration effacée, adresse I²C par défaut, baud rate UART par défaut, compensation T° reset). Le mode de communication (I²C vs UART) est PRÉSERVÉ — la commande ne touche pas le firmware EZO. Power-cycle ESP32 + recalibration recommandés après usage.
- **Carte UI « Diagnostic EZO »** dans Paramètres → Avancé (sous « Debug oscillation pH ») : sélecteur module (ORP 0x62 / pH 0x63), 10 boutons préprogrammés (`I`, `Status`, `R`, `Cal,?`, `Slope,?`, `L,?`, `L,1`, `L,0`, `Plock,?`, `Find`), champ commande personnalisée (max 30 chars) + délai paramétrable (50-5000 ms), affichage parsé (status code coloré vert/rouge selon code, réponse texte, bytes hex), historique scrollable des 30 dernières commandes (timestamp + cmd + status + réponse). Touche Entrée dans le champ perso → envoie.

### Documentation

- `docs/API.md` : nouvelle section « Outil diagnostic EZO » avec les 2 endpoints `/debug/ezo_command` et `/debug/ezo_factory`, exemples curl complets, table des status codes Atlas.
- `docs/features/page-settings.md` : nouvelle sous-section « Card Diagnostic EZO » sous le panel Avancé.
- `docs/subsystems/sensors.md` : section `readSingle()` mise à jour pour la différenciation pH/ORP, encart sur la garde 24 h `Slope,?` et l'anti-underflow `nowAfterQueue`.
- `docs/subsystems/history.md` : nouvelle sous-section « Détection de corruption LittleFS au boot » détaillant le test `/.fscheck` et la procédure de reformatage automatique.

### Notes

- Aucun ADR créé : les 5 commits sont des mitigations de bugs externes (LittleFS, firmware Atlas EZO), un outil de debug et un ajustement de seuil empirique. Pas de décision structurante avec alternative crédible à arbitrer.
- Build OK, validé en condition réelle avant publication.

---

## [2.1.0] - 2026-05-08

### Firmware
- **Pente sonde pH (feature-024)** — exposition des indicateurs d'usure de la sonde pH Atlas EZO via la commande `Slope,?`. Cache RAM `_phSlopeAcid/Base/Zero/QueriedMs` dans `Sensors`, getters publics `getPhSlopeAcid/Base/Zero()` + `getPhSlopeAgeMs()` + `enqueuePhSlopeQuery()`. Refresh automatique au boot, après chaque calibration EZO réussie (mid/low/clear) et toutes les 24 h (`kPhSlopeQueryIntervalMs = 86_400_000` ms). Dédoublonnage via flag `_phSlopeQueryPending` (anti-spam queue 4 slots). Invalidation cache à NaN après ≥ 2 échecs consécutifs (cohérent avec `_phCalCachedPoints`).
- **`AtlasEzoSensor::querySlope(PhSlopeInfo&)`** — parsing tolérant 2 ou 3 floats sur la réponse `?Slope,<acide>,<base>[,<zéro>]`. Mutex I²C tenu pendant toute la séquence (cmd + delay + read).
- **Feature strictement diagnostique** : aucun impact sur `canDose()` ni le PID. Validation `pool-chemistry` skip (passive).

### API HTTP / WebSocket
- **Nouvel endpoint** : `POST /debug/ph_slope_refresh` (sans auth, cohérent avec autres `/debug/*`) — force une re-query `Slope,?` immédiate. Réponse 200 `{success:true, queued:true}` ou 503 `{error:"queue full or already pending"}`.
- **Champs WS ajoutés à `sensor_data`** (4 champs, `null` si jamais lus / bus dégradé) : `phSlopeAcid` (% 1 décimale), `phSlopeBase` (% 1 décimale), `phSlopeZero` (mV 2 décimales), `phSlopeAgeMs` (entier ms ou `null`). Buffer WS bumpé à 1024 octets (+80).

### MQTT / Home Assistant
- **3 nouveaux topics** retain edge-triggered : `{base}/ph_slope_acid` (1 décimale), `{base}/ph_slope_base` (1 décimale), `{base}/ph_slope_zero` (2 décimales). Publication uniquement à la transition de la valeur arrondie — pas de spam.
- **3 sensors auto-discovery HA** : « Piscine pH Pente Acide » (`mdi:angle-acute`, unit `%`), « Piscine pH Pente Base » (`mdi:angle-obtuse`, unit `%`), « Piscine pH Décalage Zéro » (`mdi:sine-wave`, unit `mV`), tous avec `state_class: measurement`.
- Pas de `binary_sensor` « à remplacer » côté firmware — l'utilisateur peut le créer en automation HA selon ses propres seuils.

### Frontend
- **Chip d'état sonde pH** sur la carte « Lecture pH » (page `/ph`) — bouton focusable sous l'affichage pH 3 décimales. 5 variantes CSS (`chip--probe-good/warn/warn2/bad/unknown`) + classe `chip--probe-stale` (encadré jaune si âge > 36 h). Évaluation dans `data/app.js` (fonction `classifyPhProbe()`, constantes `PH_PROBE_*`) — seuils ajustables sans reflasher.
- **Modal détails** (`<dialog id="ph-probe-modal">`) au clic du chip : pente acide, pente base, décalage zéro, âge dernière vérif + bouton « Rafraîchir » → `POST /debug/ph_slope_refresh` (timeout 8 s, attente que `phSlopeAgeMs` redescende sous 60 s). Fallback non-cliquable si `<dialog>` non supporté (warning console).

### Fixes (correctifs code-reviewer feature-024)
- **Anti-spam HA** : trace brute `Slope,?` passée en `debug` (au lieu de `warning`) — la query auto 24 h faisait remonter du bruit dans le topic `{base}/logs` consommé par HA.
- **Cohérence WS** : `phSlopeAgeMs` désormais `null` si jamais lu (au lieu de `UINT32_MAX`), aligné avec les `phSlope*` nullables.
- **Icônes HA discriminantes** : `mdi:angle-acute` / `mdi:angle-obtuse` / `mdi:sine-wave` (au lieu de `mdi:chart-line` générique partagé).

### Documentation
- `docs/subsystems/sensors.md` : nouvelle section « Pente sonde pH — feature-024 » (méthode `querySlope()`, cache, refresh policy, invalidation), ajout `kPhSlopeQueryIntervalMs` au tableau des constantes, mention dans la liste des consommateurs WS / MQTT.
- `docs/MQTT.md` : nouvelle section « Topics et entités ajoutés en feature-024 » (3 topics + 3 sensors HA), ajout des 3 lignes dans le tableau topics capteurs.
- `docs/API.md` : nouvelle section « Endpoints ajoutés en feature-024 » (`POST /debug/ph_slope_refresh`), nouveaux champs WS dans le tableau payload `sensor_data`.
- `docs/features/page-ph.md` : nouvelle section « Chip d'état sonde » (placement, classification, modal, états stale, fallback `<dialog>`), ajout dans la table Actions et Interaction MQTT.
- Spec `specs/features/done/feature-024-pente-sonde-ph.md` (déplacée depuis `doing/`) avec section « Notes d'implémentation ».

### Build
- `pio run` SUCCESS, **RAM 18.0 %**, **Flash 99.8 %** (marge ~2.6 KB — point d'attention : prochaine feature potentiellement bloquée si pas de gain). `./build_fs.sh` SUCCESS, FS 1.1 MB.

### Risques résiduels
- Format de réponse `Slope,?` à confirmer empiriquement avec EZO réel — tolérance 2 ou 3 floats déjà en place pour les firmwares anciens.

---

## [2.0.0] - 2026-05-06

### Hardware
- **Migration PCB v2 — chaîne pH/ORP refondue sur Atlas Scientific EZO Embedded I²C (feature-021)**. Suppression matérielle de l'ADS1115 et de la sonde pH analogique. Modules EZO pH (`0x63`) et EZO ORP (`0x62`) sur le bus I²C partagé avec le DS3231. Voir [ADR-0014](docs/adr/0014-migration-atlas-ezo.md) — supersedes [ADR-0001](docs/adr/0001-capteurs-analogiques-ads1115.md).

### Firmware
- **Refonte complète chaîne pH/ORP** (`src/sensors.h/cpp`, `src/atlas_ezo.h/cpp`). Mini-classe maison `AtlasEzoSensor` (~80 lignes) encapsulant les commandes Atlas (`R`, `RT,<t>`, `Cal,*`, `Cal,?`, `Cal,clear`, `I`) et les délais EZO (600/900 ms). Mutex I²C tenu pendant la séquence atomique `RT,<temp>` + delay + `R` + delay (cond #6 pool-chemistry).
- **Queue FreeRTOS `_ezoQueue`** (4 slots) pour exécuter les calibrations EZO (~900 ms bloquantes) hors handlers HTTP. Pattern miroir de `mqttTask` ([ADR-0011](docs/adr/0011-mqtt-task-dediee.md)). Routes `/calibrate_*` retournent `{success:true, queued:true}` immédiatement.
- **`canDose(int pumpIndex)` refondu — 10 garde-fous fail-closed** dans l'ordre : index pompe valide, watchdog actif, filtration en marche, lecture pH/ORP non NaN (cond #1 stale 20 s + cond #5 bus dégradé), EZO calibré (cond #2, `cal_points >= 2` pH / `>= 1` ORP, `-1` bloque), pas de stabilisation post-cal en cours (cond #3), mode régulation = `automatic`, limite journalière, limite horaire, anti-rafale court terme (`kMaxDosingCyclesPerMinute=6` + `kMaxDosingCyclesPer15Min=20` via ring buffer 20 entrées par pompe — correctif Pass 3.5). Log « edge-triggered » : 1 entrée par transition de cause de refus.
- **Stabilisation post-calibration par pompe** : `armStabilizationTimer(int pumpIndex)` avec durée différenciée — `kStabilizationDurationPhMs = 5 min`, `kStabilizationDurationOrpMs = 3 min`. Surcharge legacy `armStabilizationTimer()` conservée pour les sites « globaux » (filtration, boot continu, rollover minuit) — applique aux 2 pompes simultanément avec `mqttCfg.stabilizationDelayMin`.
- **Stale timeout** `kSensorStaleTimeoutMs = 20000` ms : `getPh()`/`getOrp()` retournent NaN si dernière lecture > 20 s. Logger `critical` 1× à la transition. Alerte MQTT `pool/alerts/sensor_stale` edge-triggered.
- **Cache `_phCalCachedPoints` / `_orpCalCachedPoints`** mis à jour en `begin()` puis après chaque calibration. Invalidé à `-1` si `_phI2cFailStreak >= kEzoBusFailMaxConsecutive = 2` (durcissement Pass 3.5) → `_lastPh = NaN` ET cache à `-1` simultanément. Rafraîchissement opportuniste à la 1ʳᵉ lecture réussie suivante.
- **Compensation T° pH** : `RT,<temp>` envoyé avant chaque `R`. Source : `getWaterTemperature()` (feature-020). Fallback **25.0 °C** si NaN (sonde eau non identifiée ou en erreur).
- **Suppression** : libs PlatformIO `Adafruit ADS1X15` et `DFRobot_PH` retirées de `lib_deps`. 10 fonctions publiques `Sensors` legacy supprimées (`getRawPh`, `getRawOrp`, `getPhVoltageMv`, `isPhCalibrated`, `getRawTemperature`, `calibratePhNeutral/Acid/Alkaline`, `clearPhCalibration`, `detectAdsIfNeeded`, `recalculateCalibratedValues`, `publishValues`). 7 champs `MqttConfig` ORP/pH legacy supprimés (`orp_cal_*`, `ph_cal_*`).

### API HTTP / WebSocket
- **Nouvelles routes** `POST /calibrate_ph {step:"mid"|"low"}`, `POST /calibrate_orp {reference:<0..1000 mV>}`, `POST /calibrate_clear {sensor:"ph"|"orp"}`. Toutes répondent immédiatement `{success:true, queued:true}` (< 1 ms). 400 si payload invalide, 503 si queue saturée.
- **Routes supprimées** (404 désormais) : `POST /calibrate_ph_neutral`, `POST /calibrate_ph_acid`, `POST /clear_ph_calibration`.
- **Précision pH** : 3 décimales sur tous les contrats publics (`GET /data`, WS `sensor_data`, MQTT `{base}/ph`). Avant : 1 décimale.
- **Champs WS ajoutés** : `phCalPoints` (int `-1..3`), `orpCalPoints` (int `-1..1`).
- **Champs WS supprimés** : `orp_raw`, `ph_raw`, `ph_voltage_mv`, `temperature_raw` (la notion de « valeur brute » n'existe plus côté EZO).

### MQTT / Home Assistant
- **Nouveaux topics** (retain) : `{base}/ph_cal_points`, `{base}/orp_cal_points`, `{base}/alerts/calibration_required` (edge-triggered, payload JSON ou vide=clear), `{base}/alerts/sensor_stale` (idem).
- **Auto-discovery HA enrichi** : 2 nouveaux sensors « Piscine pH Points Calibrés » (`unique_id: poolcontroller_ph_cal_points`, `icon: mdi:numeric`) et « Piscine ORP Points Calibrés ».
- **`{base}/ph`** publié avec **3 décimales** (vs 1 décimale en v1.x). Tout consommateur HA qui parsait `int()` doit basculer sur `float()`.

### Frontend
- **Refonte carte calibration pH** : 2 sous-blocs **parallèles** `.cal-point-block` (point milieu pH 7.00 + point bas pH 4.00). Plus de stepper séquentiel. Chaque bloc : badge état, micro-étapes, readout live, bouton « Calibrer le point X.X ». Workflow : POST `/calibrate_ph` → toast « Calibration en cours… » → polling 15 s sur `phCalPoints` → toast succès. Lectures pH 3 décimales sur les readouts.
- **Refonte carte calibration ORP** : 1 sous-bloc `.cal-point-block` unique avec input `orp-cal-reference` (`0..1000` mV, défaut 470). Suppression du sélecteur 1pt/2pts hérité du firmware v1. Polling 15 s avec fallback succès si `orpCalPoints >= 1` (recalibration ne change pas le compteur).
- **Cartes Régulation pH / ORP** : callout vert « Calibré N points ✓ » si calibration nominale, chip ambrée d'inhibition si `phCalPoints < 2` ou `orpCalPoints < 1`, chip rouge « EZO injoignable » si compteur à `-1`.
- **Suppression** : chip tension pH legacy de la page pH, sélecteur 1pt/2pts ORP.

### Sécurité chimique
- 6 conditions impératives `pool-chemistry` intégrées (#1 stale, #2 cal_points, #3 stabilisation, #4 alerte MQTT, #5 bus I²C dégradé, #6 mutex atomique).
- 2 correctifs additionnels Pass 3.5 : invalidation simultanée cache cal_points + lecture en mode bus dégradé ; ring buffer anti-rafale 6/min + 20/15min.

### Documentation
- **Nouveau** : [`docs/adr/0014-migration-atlas-ezo.md`](docs/adr/0014-migration-atlas-ezo.md) — décision, alternatives écartées (lib EZO_pH, ADS1115 + filtre amélioré, chaîne mixte, tâche FreeRTOS dédiée, exécution synchrone), conséquences, ce que ça verrouille.
- **Annoté Superseded** : [`docs/adr/0001-capteurs-analogiques-ads1115.md`](docs/adr/0001-capteurs-analogiques-ads1115.md) — bandeau « Superseded by ADR-0014 ».
- Mis à jour : [`docs/subsystems/sensors.md`](docs/subsystems/sensors.md) (refonte majeure : architecture EZO, mini-classe `AtlasEzoSensor`, queue, cache cal_points, stale, mutex), [`docs/subsystems/pump-controller.md`](docs/subsystems/pump-controller.md) (10 garde-fous `canDose`, ring buffer anti-rafale, stabilisation par pompe, constantes ajoutées au tableau), [`docs/MQTT.md`](docs/MQTT.md) (4 nouveaux topics + 2 sensors HA), [`docs/API.md`](docs/API.md) (3 routes refondues, payloads `/data` et WS, champs `phCalPoints`/`orpCalPoints`), [`docs/features/page-ph.md`](docs/features/page-ph.md) (refonte UI 2 sous-blocs), [`docs/features/page-orp.md`](docs/features/page-orp.md) (refonte UI 1 bloc), [`docs/BUILD.md`](docs/BUILD.md) (suppression libs ADS1115/DFRobot_PH des `lib_deps`), [`docs/UPDATE_GUIDE.md`](docs/UPDATE_GUIDE.md) (procédure recalibration obligatoire v1.x → v2.0.0), [`docs/adr/README.md`](docs/adr/README.md) (index : ADR-0001 superseded, ADR-0014 ajouté).

### Migration utilisateur — v1.x → v2.0.0 (BREAKING)
- **Recalibration obligatoire** : pH 2 points (mid 7.00 + low 4.00) + ORP 1 point (kit 225 mV ou 470 mV). Tant que ce n'est pas fait, régulation auto inhibée + alerte MQTT `pool/alerts/calibration_required` retain.
- Données NVS legacy (`ph_cal_*`, `orp_cal_*`) supprimées silencieusement au 1ᵉʳ boot — aucune migration possible (chaîne de mesure totalement différente).
- Firmware v2.0.0 conçu pour PCB v2 uniquement. **NE PAS** flasher sur PCB v1 (sondes incompatibles).

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.8 %** (marge ~17 KB — point d'attention pour les futures features), 0 nouveau warning. `./build_fs.sh` SUCCESS, FS 1.1 MB.

---

## [Unreleased] - 2026-05-06

### Hardware/Firmware (PCB v2)
- **Support 2 sondes DS18B20 (eau piscine + circuit électronique) avec identification (feature-020)**. Le PCB v2 ajoute une 2ᵉ sonde DS18B20 sur le bus OneWire (GPIO 5 partagé). Identification par adresse ROM persistée NVS via workflow utilisateur (chauffer une sonde dans la main 30 s, cliquer le bon bouton dans Paramètres → Avancé). Auto-permutation activée : si l'utilisateur identifie la sonde A comme « eau » alors qu'une autre était déjà eau, cette dernière bascule automatiquement à « circuit ». Fallback gracieux : `Sensors::getTemperature()` reste un alias de `getWaterTemperature()` avec repli sur la 1ʳᵉ sonde détectée si non identifié, garantissant la rétrocompat MQTT/WS/HA. Voir [ADR-0013](docs/adr/0013-identification-sondes-onewire.md).

### Frontend
- Nouvelle card « Identification des sondes de température » dans Paramètres → Avancé (affichage temps réel adresses ROM + T° brutes, boutons d'assignation, badge état une fois identifiée, bouton réinitialiser). Polling 2 s scoped au panel-dev.
- Chip de notification Dashboard ambré (pattern `.chip` existant) tant que `sondes_identified === false && sondes_detected >= 1`. Clic → redirige vers la card.

### MQTT/HA
- Nouveau topic `pool/sensors/temperature_circuit` (retain) + entité auto-discovery HA « Piscine Température Circuit ». Topic `pool/sensors/temperature` (eau) inchangé (rétrocompat).

### API HTTP / WebSocket
- 3 nouveaux endpoints (auth) : `GET /sensors/onewire/scan`, `POST /sensors/onewire/identify`, `POST /sensors/onewire/reset`.
- 3 nouveaux champs WS `sensor_data` : `temperature_circuit`, `sondes_identified`, `sondes_detected`. Buffer 832 → 896 octets.

### Documentation
- `docs/adr/0013-identification-sondes-onewire.md` créé. ADR-README mis à jour.
- `docs/subsystems/sensors.md`, `docs/subsystems/ws-manager.md`, `docs/MQTT.md`, `docs/API.md`, `docs/features/page-settings.md`, `docs/UPDATE_GUIDE.md` mis à jour.

### Build
- `pio run` SUCCESS, RAM 16.5 %, **Flash 98.5 %** (marge ~21 KB — point d'attention pour feature-021 EZO), 0 nouveau warning. `./build_fs.sh` SUCCESS.

---

## [Unreleased] - 2026-05-05

### BREAKING / Hardware
- **Bascule cible PCB v1 → PCB v2 (feature-019)**. Le firmware n'est plus compatible avec le PCB v1. Mapping GPIO entièrement réassigné : 10 pins actifs (LED=2, OneWire DS18B20=5, I²C SDA=21, I²C SCL=22, pompe pH=25, relais filtration=26, relais éclairage=27, pompe ORP=33, bouton factory reset=35) + 3 pins réservés feature-future (`kRtcSqwPin=23`, `kCtnAuxPin=32` MOSFET 12V tableau, `kRtcIntPin=36`). L'ADS1115 et ses pins associés sont supprimés (les modules Atlas EZO pH/ORP les remplaceront en feature-020). La 2ᵉ sonde DS18B20 est câblée sur le bus OneWire mais ne sera détectée qu'en feature-021. Ce changement est unidirectionnel : la version cible 2.0.0 ne fonctionnera plus sur PCB v1. Voir [ADR-0012](docs/adr/0012-mapping-gpio-pcb-v2.md) pour la justification complète des choix de pin et les alternatives écartées

### Firmware
- **Migration des `#define` de pin vers `constexpr kXxxPin`**. Suppression de 7 `#define` historiques de `src/config.h` (`PUMP1_PWM_PIN`, `PUMP2_PWM_PIN`, `TEMP_SENSOR_PIN`, `FILTRATION_RELAY_PIN`, `LIGHTING_RELAY_PIN`, `BUILTIN_LED_PIN`, `FACTORY_RESET_BUTTON_PIN`) au profit de constantes typées `constexpr uint8_t kXxxPin` regroupées en tête de `src/constants.h` sous une section dédiée « GPIO PIN ASSIGNMENTS - PCB v2 ». Cohérent avec la convention CLAUDE.md (`kConstantes` dans `constants.h`, pas de `#define` dispersés)
  - **Logique du bouton factory reset inversée** : `pinMode(kFactoryResetButtonPin, INPUT)` (au lieu de `INPUT_PULLDOWN` interne v1) car GPIO 35 est input-only et ne supporte pas de pull-up/pull-down interne — un pull-up externe 10 kΩ vers 3V3 sur le PCB v2 est obligatoire. Lecture firmware passée à `pressed = digitalRead(kFactoryResetButtonPin) == LOW` (au lieu de `== HIGH` v1). **Comportement utilisateur identique** : appui maintenu 10 s = factory reset
  - **Convention `pumps[0]` = pH (kPumpPhPin=25), `pumps[1]` = ORP (kPumpOrpPin=33)** figée dans `pump_controller.cpp` `begin()`. Inversion = bug de sécurité chimique (mauvaise pompe activée)
  - **Pins réservés `kRtcSqwPin`, `kCtnAuxPin`, `kRtcIntPin`** déclarés mais sans `pinMode` initial — restent en haute impédance jusqu'à activation par une feature future (économie de courant pull-up, détection facile d'un court-circuit hardware)
  - 7 fichiers touchés : `src/constants.h`, `src/config.h`, `src/pump_controller.cpp`, `src/filtration.cpp`, `src/lighting.cpp`, `src/sensors.cpp`, `src/main.cpp`
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning
  - Tests dynamiques (boot, voltmètre relais, bouton 10 s) délégués à l'humain sur PCB v2 réel — le PCB v1 ne peut plus être testé fonctionnellement

### Documentation
- `docs/adr/0012-mapping-gpio-pcb-v2.md` créé — décision complète, tableau du mapping (13 pins), alternatives écartées (mapping v1 conservé / bouton sur pin avec pull-up interne / bus I²C séparé pour EZO / `pinMode(INPUT_PULLUP)` activé immédiatement sur les pins réservés / `#define` ré-injecté dans `config.h`), conséquences positives et dette assumée, ce que la décision verrouille
- `docs/adr/README.md` : index mis à jour (entrées ADR-0011 et ADR-0012 ajoutées)
- `docs/subsystems/pump-controller.md` : nouvelle section « Mapping pompes ↔ pins (PCB v2) » documentant la convention `pumps[0]`=pH/`pumps[1]`=ORP avec référence ADR-0012 et feature-019
- `docs/subsystems/sensors.md` : tableau Matériel mis à jour (pin OneWire `kTempSensorPin=5` cité, mention du bus partagé avec la 2ᵉ sonde DS18B20 à activer en feature-021), note sur la suppression de l'ADS1115 au profit des Atlas EZO (feature-020)
- `docs/subsystems/filtration.md` : `kFiltrationRelayPin = 26` (au lieu de `FILTRATION_RELAY_PIN = 25` v1)
- `docs/subsystems/lighting.md` : `kLightingRelayPin = 27` (au lieu de `LIGHTING_RELAY_PIN = 26` v1)

---

## [Unreleased] - 2026-04-30

### Firmware
- **MQTT — `requestReconnect()` n'est plus déclenché après un save de paramètres non-MQTT (feature-018)**. Évite la republication des 17 messages discovery HA et la transition transitoire « Déconnecté » du badge UI lors d'un save de modes de régulation, NTP, logs DEBUG, écran LVGL, calibrations, etc. Le handler `POST /save-config` snapshotte désormais les 6 champs MQTT (`server`, `port`, `topic`, `username`, `password`, `enabled`) après prise du `configMutex` et avant parsing JSON, puis compare après application du payload : `requestReconnect()` n'est appelé que si au moins un champ a réellement changé. Nouveau log INFO FR `"MQTT reconnect demandé (config MQTT modifiée)"` pour tracer les reconnects légitimes. Comportement déclenché par feature-015 IT1bis qui a rendu visibles ces transitions transitoires (badge MQTT en temps réel via WS) — la session MQTT était coupée puis rétablie en ~1 s à chaque save UI quel qu'il soit. Contrat externe `POST /save-config` strictement inchangé (mêmes payloads, mêmes réponses)
  - **`src/web_routes_config.cpp`** : snapshot des 6 champs + booléen `mqttChanged` + appel conditionnel `mqttManager.requestReconnect()` (+24/-1 lignes)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : nouvelle sous-section « Déclenchement conditionnel post-`POST /save-config` (feature-018) » sous « Reconnexion » — tableau des 6 champs MQTT comparés, mécanique du snapshot/comparaison/appel conditionnel, bénéfice (plus de re-discovery HA inutile, badge UI stable), cas `enabled` true → false

### Firmware
- **Toggle pour activer/désactiver les logs DEBUG (firmware + UI)** — `Logger::debug()` est désormais conditionné par `authCfg.debugLogsEnabled` (default `false`, persisté en NVS sous la clé `debug_logs`). Quand le toggle est désactivé, la fonction effectue un early return en première ligne — aucune allocation `String`, aucun lock mutex, aucun push WebSocket, aucune écriture buffer. Default désactivé pour alléger le buffer logs (200 entrées max) en production. Pattern miroir de `sensorLogsEnabled` (« Log des sondes ») introduit précédemment. Les niveaux `INFO`, `WARN`, `ERROR`, `CRITICAL` ne sont **pas** affectés. Effet immédiat (pas de redémarrage requis), persistance NVS au reboot
  - **`src/config.h`** : `bool debugLogsEnabled = false;` ajouté dans `AuthConfig`
  - **`src/config.cpp`** : write/read NVS clé `debug_logs` (default `false`) dans `saveAuthConfig()` / `loadAuthConfig()`
  - **`src/logger.cpp`** : `#include "config.h"` + early return en TOUTE PREMIÈRE LIGNE de `Logger::debug()`. Autres niveaux non touchés
  - **`src/web_routes_config.cpp`** : champ `debug_logs_enabled` exposé dans `/get-config` et accepté dans `/save-config` avec log INFO « Logs DEBUG: activés/désactivés »
  - **`src/ws_manager.cpp`** : champ `debug_logs_enabled` ajouté dans la payload WS `config` (snapshot config push WS broadcast)
  - Le filtre UI `#log_level_debug` reste indépendant (filtre d'affichage seulement, ne pilote pas la production firmware)
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Frontend
- **Nouveau switch « Logs DEBUG activés »** dans Paramètres → Avancé → card Logs, placé immédiatement sous « Log des sondes ». Description en français : « Active les messages de diagnostic détaillé. À activer uniquement pour le diagnostic. ». Lecture/écriture via `applyConfig()` (avec `=== true` pour default `false`) et `collectAuthConfig()`, autosave via event listener (pattern miroir de `sensor_logs_enabled`)
  - **`data/index.html`** : nouveau switch `#debug_logs_enabled` dans la card Logs du panneau Avancé
  - **`data/app.js`** : lecture, écriture et listener autosave

### Documentation
- `docs/features/page-settings.md` : section card Logs étoffée — nouveau toggle « Logs DEBUG activés » documenté sous « Log des sondes » (default `false`, effet immédiat, persistance NVS, indépendance du filtre UI `#log_level_debug`)
- `docs/subsystems/logger.md` : nouvelle section « Toggle DEBUG runtime » documentant l'early return dans `Logger::debug()`, la clé NVS `debug_logs`, le default `false`, l'absence d'impact sur `info/warning/error/critical`, et la complémentarité avec le filtre UI
- `docs/API.md` : champs `sensor_logs_enabled` et `debug_logs_enabled` ajoutés dans le payload de `/get-config` et la table des champs notables (validés dans `/save-config`)
- `docs/UPDATE_GUIDE.md` : nouvelle note de migration « Logs DEBUG désactivés par défaut — depuis 2026-04-30 » expliquant le comportement par défaut et la procédure d'activation via Paramètres → Avancé

### Firmware
- **IT5 — MQTT — fix déconnexions `exceeded timeout` Mosquitto** : remplacement du mode non-bloquant `O_NONBLOCK` (IT4) par un timeout d'écriture borné `SO_SNDTIMEO=500 ms` posé via `setsockopt()` après chaque `mqtt.connect()` réussi. Le PINGREQ keepalive PubSubClient (2 octets toutes les 60 s) part désormais de manière fiable même quand un publish concurrent occupe le send buffer TCP — avant IT5, un `lwip_send()` qui retournait `EAGAIN` instantanément faisait perdre silencieusement le PINGREQ (PubSubClient n'audite pas le retour de `_client->write` pour le PINGREQ), et Mosquitto coupait la session après 90 s sans paquet reçu. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 5 »
  - **Nouvelle constante** `kMqttSocketSendTimeoutMs = 500` (ms) dans `src/constants.h` avec commentaire d'unité explicite et référence ADR-0011 IT5
  - **`src/mqtt_manager.cpp` `connectInTask()`** : `fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)` remplacé par `setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))` avec `tv = {0, 500_000 µs}` ; include `<fcntl.h>` retiré, `<lwip/sockets.h>` ajouté
  - **Wrapper `safePublish()` inchangé runtime** : commentaire d'en-tête mis à jour pour refléter le nouveau mécanisme (`mqtt.publish()` borné à 500 ms par appel via `SO_SNDTIMEO`, plus de retour immédiat `EAGAIN`)
  - **Trade-off** : un publish lent peut prendre jusqu'à 500 ms (vs retour immédiat IT4 sur send buffer plein). Pire cas `publishDiscovery` (17 publishes enchaînés) = 8.5 s, sous le watchdog 30 s avec marge. Imperceptible utilisateur sur LAN sain
  - Build SUCCESS, RAM 16.4 %, Flash 97.8 %, 0 nouveau warning

### Documentation
- `docs/subsystems/mqtt-manager.md` : section « Garde-fou » renommée « `safePublish()` + socket avec `SO_SNDTIMEO` (IT5, remplace O_NONBLOCK d'IT4) » avec snippet `setsockopt`, explication du side-effect IT4 sur le PINGREQ keepalive et son fix IT5 ; tableau des paramètres tâche enrichi de `kMqttSocketSendTimeoutMs` ; sections « Keepalive », « Bornage TCP côté lwip », « Bascule de dominance entre `-3` et `-4` » et tableaux mis à jour pour refléter le timeout socket borné IT5 au lieu du non-bloquant IT4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 5 — 2026-04-30 » dans « Évolutions » détaillant la cause racine (PINGREQ silencieusement perdu en `O_NONBLOCK`), le fix `SO_SNDTIMEO=500 ms`, les fixes F17–F20, le trade-off et les tests dynamiques restants. La décision principale de l'ADR (tâche dédiée) reste retenue
- `specs/features/done/feature-014-mqtt-task-dediee.md` : statut passé à `done`, version cible 1.0.5, itération 5 marquée livrée (build vert + revue OK ; AC-IT5-3 et AC-IT5-4 délégués à l'humain post-flash)

---

## [Unreleased] - 2026-04-29

### Frontend
- **UI cards — placement uniformisé des badges d'état (feature-001)** : uniformisation du placement des badges d'état (Marche/Arrêt, Allumé/Éteint, Connecté/Déconnecté) à droite du titre dans le `card__head` des cards Filtration « Contrôle manuel », Éclairage « Contrôle manuel » et MQTT (Paramètres). Suppression des lignes « État actuel » redondantes dans le body des cards Filtration et Éclairage. Cohérence visuelle inter-pages, gain de place vertical. Côté JS, `updateFiltrationBadges()` et `updateLightingStatus()` ont été splittés (page détail = `pill ok/bad/mid` ; dashboard `card--status` = `state-badge--*` inchangé), `getFiltrationState()` expose désormais `pillClass` (mapping `warn → mid`). Règle CSS de garde `.card__head .pill { flex-shrink: 0; white-space: nowrap; }`. Aucun impact sur le dashboard ni sur les autres cards (Wi-Fi, Heure, Sécurité, Régulation, Calibrations, Produits, Historique, Système).
- **Bug fix** : Badge MQTT (Paramètres → MQTT) — propagation fiabilisée vers l'UI : la mise à jour temps réel via WS s'applique désormais en tête de `_onWsSensorData` (blindée par try/catch) et un re-render explicite est déclenché au passage sur le panel MQTT. Corrige un cas où le badge restait à « Déconnecté » après reconnexion firmware sans switch d'onglet.
- **WebSocket** : badge statut MQTT (Paramètres → MQTT) mis à jour en temps réel via push WS toutes les 5 s, sans nécessité de reload page. Quand le broker devient injoignable (câble HA débranché, broker arrêté), le badge bascule sur « Déconnecté » en moins de 5 s suivant la détection firmware ; idem pour la reconnexion. Le champ `mqtt_connected` est désormais inclus dans la payload `sensor_data` (en plus du snapshot `config` déjà présent). Source : `mqttManager.isConnected()` (single source of truth `connectedAtomic` introduit par feature-014 IT2)

### Documentation
- `docs/features/page-filtration.md` : badge Marche/Arrêt désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateFiltrationBadges()` / `getFiltrationState()`
- `docs/features/page-lighting.md` : badge Allumé/Éteint désormais documenté dans le `card__head` de la card « Contrôle manuel » ; mention de la suppression de la ligne « État actuel » redondante et du split de `updateLightingStatus()`
- `docs/features/page-settings.md` : précision du placement DOM uniformisé du badge MQTT (frère direct du `<h2>` dans `card__head`) et de la règle CSS de garde
- `docs/subsystems/ws-manager.md` : nouveau champ `mqtt_connected` documenté dans `sensor_data` avec la précision du doublon volontaire vs `config` (canal temps réel 5 s vs snapshot stable à la transition)
- `docs/features/page-settings.md` : précision sur le comportement temps réel du badge MQTT (Paramètres → MQTT) — bascule en < 5 s sans reload

---

## [Unreleased] - 2026-04-29

### Firmware
- **IT4 — Wrapper `safePublish()` + socket non-bloquante** — corrige un nouveau PANIC watchdog observé au 3ᵉ re-test D2 humain APRÈS le flash IT3. Le point de blocage avait migré vers `drainOutQueue()` (publish ~110 octets `orp_limit`), fonction qui n'avait pas été instrumentée par F8/F9 d'IT3 (oubli). Découverte parallèle : `CONFIG_LWIP_TCP_MAXRTX=5` borne en réalité à ~93 s (et non ~10 s comme estimé en IT3) à cause de `TCP_RTO_INITIAL=3 s` × backoff exponentiel — le bornage seul ne protège pas `mqttTask`. Pivot architectural : socket TCP non-bloquante via `fcntl(F_SETFL, O_NONBLOCK)` après chaque `mqtt.connect()` réussi → tout `WiFiClient::write()` retourne immédiatement avec `EAGAIN` si send buffer plein, plus de blocage dans `lwip_select`. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 4 »
  - **Mode non-bloquant via `fcntl()`** : passage de la socket en `O_NONBLOCK` après chaque connect réussi dans `connectInTask()`, avant `subscribe()`. Includes `<fcntl.h>` et `<errno.h>` ajoutés
  - **Wrapper unique `safePublish(topic, payload, retain)`** : remplace les 24 `mqtt.publish()` directs dans `mqttTask` (status `online` au connect, `drainOutQueue`, 20 publishes de `publishAllStatesInternal`, `publishDiagnosticInternal`, 17 publishes de la lambda `publishConfig` dans `publishDiscovery`). Le wrapper fait `esp_task_wdt_reset()` puis check `mqtt.connected()` avant délégation à `mqtt.publish()`
  - **Suppression des garde-fous IT3 redondants** : ~50 lignes de `esp_task_wdt_reset()` et `if (!mqtt.connected()) return;` éparpillées dans `publishAllStatesInternal()` (20 resets + 5 bail-out) et la lambda `publishConfig` (1 bail-out) ont été supprimées — factorisées dans le wrapper, plus lisible
  - **F12 annulé** : la constante `kMqttPublishHeadersOverhead` envisagée pour un check `availableForWrite()` est **inutile** car `WiFiClient::availableForWrite()` retourne toujours 0 dans Arduino-ESP32 6.9.0 (méthode héritée de `Print::availableForWrite()` non override). Le mode non-bloquant remplace ce mécanisme
  - **Trade-off accepté** : drop silencieux des publish quand le send buffer TCP est plein. Les états retain sont republiés au cycle suivant (10 s), les alertes ponctuelles peuvent être perdues — c'était déjà le cas en mode bloquant pré-IT4 (timeout 30 s puis drop), IT4 rend le drop instantané

- **IT3 — Borne TCP write côté lwip + bail-out anticipé** — corrige un nouveau PANIC watchdog observé au re-test D2 humain APRÈS le flash IT2. Le point de blocage avait migré de `mqtt.connect()` (résolu en IT2) vers `mqtt.publish()` : coredump-5 confirme `mqttTask` bloquée dans `WiFiClient::write` → `lwip_select` lors d'un publish 33 octets ("OFF") sur send buffer TCP saturé. Le reset wdt par groupes de 5 publish d'IT2 ne suffisait pas. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions » → « Itération 3 »
  - **`CONFIG_LWIP_TCP_MAXRTX=5` dans `platformio.ini`** (`build_flags`) : limite à 5 retransmissions TCP avant abandon de socket par lwip (~10 s cumulés au lieu de ~75 s avec la valeur par défaut 12). Trade-off : paramètre global à toute la pile lwip → impact aussi AsyncWebServer / OTA HTTP / NTP (abandon socket ~10 s vs ~75 s). Acceptable pour un firmware temps réel ; OTA en réseau très dégradé peut être avorté plus tôt et nécessiter un retry humain
  - **Cadence `esp_task_wdt_reset()` 1:1 dans `publishAllStatesInternal()`** : reset posé **avant chaque** `mqtt.publish()` (20 resets, vs 2 resets par groupes en IT2). Garantit qu'au pire un seul publish reste à l'intérieur de la fenêtre wdt 30 s
  - **Bail-out fail-fast** : 5 `if (!mqtt.connected()) return;` répartis dans `publishAllStatesInternal()` (tous les ~3-4 publish) + 1 bail-out en tête de la lambda `publishConfig` de `publishDiscovery()`. Dès que lwip ferme la socket (cf. `CONFIG_LWIP_TCP_MAXRTX=5`), les publish restants sont court-circuités au lieu d'enchaîner 14 erreurs de ~2 s chacune
  - **F7/F10 annulés** : `setsockopt(TCP_USER_TIMEOUT)` envisagé initialement pour borner par-socket (RFC 5482) — **non supporté par lwip dans ESP-IDF 4.4** (Arduino-ESP32 6.9.0). Le bornage TCP repose donc uniquement sur F6 (paramètre global lwip). Constante `kMqttTcpUserTimeoutMs` non créée

- **Durcissement watchdog `mqttTask` sur broker injoignable** — corrige un PANIC watchdog 30 s observé pendant le test D2 (câble Ethernet HA débranché 2–3 min) où `mqttTask` restait bloquée dans `WiFiClient::connect()` → `lwip_select` sans reset. La régulation pH/ORP, la filtration et les autres tâches n'étaient pas concernées (déjà isolées par ADR-0011), mais le PANIC du core 0 entraînait un reboot complet. Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md) section « Évolutions »
  - **Timeout client TCP corrigé de 5000 s à 2 s** : `WiFiClient::setTimeout()` (Arduino-ESP32 6.9.0) attend des **secondes**, pas des ms — le code historique `setTimeout(5000)` programmait 5000 secondes (~83 min), bug latent qui rendait le timeout côté client TCP totalement inopérant. Nouvelle constante `kMqttClientConnectTimeoutSec = 2` dans `constants.h` avec commentaire d'unité explicite
  - **`esp_task_wdt_reset()` granulaire** : reset ajouté juste après le retour de `mqtt.connect()` (couvre le cas SYN TCP retransmis sur broker injoignable jusqu'à ~75 s avant abandon lwip), reset au milieu de `publishAllStatesInternal()` (≤ 5 publish entre 2 resets), reset après chaque publish individuel dans le helper `publishConfig` de `publishDiscovery()` (17 publishes auto-discovery)
  - **`connectedAtomic` single source of truth** : store canonique posé en début de `taskLoop()` à chaque tour, complété par les transitions explicites en `connectInTask()` (succès) et `disconnect()`. Suppression de 3 stores intermédiaires redondants qui créaient une fenêtre de divergence UI/WARN observée pendant D2 (UI affichait « Déconnecté » sans WARN dans les logs)
  - **Suppression de la race au boot** : `mqttManager.requestReconnect()` retiré de `setup()` dans `main.cpp` — `mqttTask` se connecte déjà toute seule au premier tour de `taskLoop()`. Élimine le double publish d'auto-discovery au démarrage (32 messages au lieu de 17)

### Documentation
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT après validation D2 humaine du 2026-04-29 14:12:43 — avec le mode non-bloquant IT4, l'état dominant lors d'une coupure réseau est `-4` (`MQTT_CONNECTION_TIMEOUT`, keepalive PubSubClient, ~60 s) au lieu de `-3` (`MQTT_CONNECTION_LOST`, abandon TCP lwip, 90–180 s). La dominance documentée pré-IT4 (`-3` majoritaire) est inversée. Bénéfice indirect : détection plus précoce et plus propre. Tableau « États observés selon le scénario », tableau des codes d'état et workflow troubleshooting mis à jour en conséquence ; D2 marqué VALIDÉ dans la spec avec logs de référence
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Garde-fou : `safePublish()` + socket non-bloquant (IT4) » (snippet du wrapper, `fcntl(F_SETFL, O_NONBLOCK)` après chaque connect, tableau des 24 call sites, trade-off drop silencieux) ; AVERTISSEMENT en tête de section sur `WiFiClient::availableForWrite()` qui retourne toujours 0 dans Arduino-ESP32 6.9.0 ; section « Watchdog dans `mqttTask` » mise à jour (les `esp_task_wdt_reset()` IT3 ont été factorisés dans `safePublish()`) ; section « Bornage TCP côté lwip » corrigée — `CONFIG_LWIP_TCP_MAXRTX=5` borne à ~93 s (RTO_INITIAL × backoff), pas 10 s. Le mode non-bloquant IT4 est le vrai mécanisme de protection ; section « Bail-out fail-fast pendant les salves de publish » mise à jour pour pointer vers le wrapper
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 4 — 2026-04-29 » dans « Évolutions » détaillant le pivot socket non-bloquante, le wrapper `safePublish()` sur 24 call sites, l'annulation de F12 (`availableForWrite()` cassé dans Arduino-ESP32 6.9.0), la découverte du bornage réel à ~93 s pour `MAXRTX=5`, le trade-off drop silencieux. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : section « Watchdog dans `mqttTask` » mise à jour (cadence 1:1 dans `publishAllStatesInternal()`, 5 bail-out répartis, rationnel IT3) ; section « Détection des déconnexions » étendue avec sous-section « Bail-out fail-fast pendant les salves de publish (IT3) » ; nouvelle section « Bornage TCP côté lwip » documentant `CONFIG_LWIP_TCP_MAXRTX=5` et son impact global (AsyncWebServer / OTA / NTP) avec mention de l'absence de support `TCP_USER_TIMEOUT` dans lwip ESP-IDF 4.4
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la sous-section « Itération 3 — 2026-04-29 » dans « Évolutions » détaillant les 3 fixes appliqués (F6, F8, F9), l'annulation de F7/F10 (TCP_USER_TIMEOUT non supporté par lwip ESP-IDF 4.4), le coredump-5 confirmant le déplacement du blocage de connect vers publish, et le trade-off du paramètre lwip global sur AsyncWebServer / OTA / NTP. La décision principale de l'ADR (tâche dédiée) reste retenue
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Watchdog dans `mqttTask` » (tableau des 10 emplacements de `esp_task_wdt_reset()` avec cadence garantie ≤ 5 publish entre 2 resets) ; nouvelle section « Timeout client TCP — UNITÉ EN SECONDES » avec avertissement visible pour le bug latent `setTimeout(5000)` ; section « Détection des déconnexions » étendue avec le store canonique `connectedAtomic` en début de `taskLoop()` et la suppression des stores intermédiaires ; section « Reconnexion » étendue avec la sous-section « Connexion initiale au boot » documentant la suppression du `requestReconnect()` dans `setup()`
- `docs/adr/0011-mqtt-task-dediee.md` : ajout de la section « Évolutions » → « Itération 2 — 2026-04-29 » détaillant les 5 fixes appliqués, le bug latent `WiFiClient::setTimeout()` et le coredump confirmant la cause racine. La décision principale de l'ADR (tâche dédiée) reste retenue — c'est un durcissement post-test, pas un revirement

---

## [Unreleased] - 2026-04-27

### Firmware
- **MQTT déplacé dans une tâche FreeRTOS dédiée** (`mqttTask`, core 0, prio 2, stack 8 KB) — corrige les crashes `PANIC IntegerDivideByZero` watchdog 30 s observés en production lorsqu'une publication MQTT (de 33 octets dans le 3ᵉ crash) bloquait `loopTask` dans `lwip_select` à cause de la saturation du TCP send window sur CPL bruyant. La régulation pH/ORP, la filtration et le watchdog continuent désormais indépendamment de l'état du réseau MQTT. Comportement utilisateur strictement identique : mêmes topics, mêmes payloads, mêmes intervalles, même auto-discovery HA. API publique de `MqttManager::publishXxx()` inchangée — les méthodes deviennent simplement non-bloquantes (producteurs sur queue FreeRTOS). Voir [ADR-0011](docs/adr/0011-mqtt-task-dediee.md)
- **Arrêt propre OTA** : publication `status=offline` synchrone (timeout 1 s) avant `ESP.restart()` sur tous les sites concernés (OTA firmware, redémarrage mode AP, factory reset bouton, factory reset HTTP). Les clients HA voient désormais le passage à `offline` immédiatement, sans attendre les 90 s de timeout broker
- **Stabilité MQTT** : fin des déconnexions périodiques (toutes les 1–2 h) — désactivation du WiFi power save (`WiFi.setSleep(false)`), pré-résolution DNS explicite avant `WiFiClient::connect`, `setSocketTimeout` réduit à 2 s, suppression du `requestReconnect()` périodique qui réinitialisait le backoff exponentiel
- **Stabilité réseau** : latence LAN ramenée sous 10 ms (était 90–260 ms à cause du DTIM WiFi) ; loop principale jamais bloquée plus de ~7 s par opération réseau ; backoff MQTT effectif jusqu'à 120 s sur broker injoignable
- **Coredump** : ajout de la partition `coredump` dédiée (64 KB, offset `0x3F0000`) — les crashes `PANIC` sont désormais persistés en flash et accessibles via l'API HTTP ou l'UI
- **Partitions** : partition `history` réduite de 128 KB à 64 KB pour libérer l'espace nécessaire au coredump ; flash USB obligatoire pour cette migration (OTA insuffisant)
- **History** : `kMaxHourlyDataPoints` réduit de 360 à 168 (7 jours de rétention horaire, contre 15 jours précédemment) pour respecter le budget de la partition réduite
- **History** : `HistoryManager::begin()` détecte un redimensionnement de partition via NVS (`hist_meta/part_sz`) et efface le filesystem avant montage si la taille a changé — évite un crash `IntegerDivideByZero` dans `lfs_alloc`
- **Régulation** : reset journalier des compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` désormais indépendant de l'état de la filtration — extraction de la logique de bascule de date dans `tickDailyRollover()`, appelée depuis `update()` **avant** le check `canDose()`. Le bug rendait le compteur figé sur la valeur de la veille tant que la filtration n'avait pas tourné dans la journée. La transition `currentDayDate` vide → date NTP valide remet aussi `dayStartTimestamp = 0` pour invalider tout timer fallback `millis()` accumulé depuis le boot
- **Régulation** : ajout de `saveDailyCounters()` + `armStabilizationTimer()` dans la branche fallback `millis()` du reset journalier (manquaient avant le fix)
- **Régulation** : warnings/criticals du mode `scheduled` (capteur pH/ORP hors plage, daily target plafonné, débit pompe à 0) passés en **edge-triggered** — un seul log à l'entrée dans l'état + un INFO de recovery au retour à la normale. Stoppe le spam de centaines de lignes par seconde quand l'état persistait
- **Historique** : suppression de l'appel `saveToFile()` redondant dans `HistoryManager::update()` (`consolidateData()` l'appelle déjà en interne) — moitié moins d'écritures flash sur la partition `history` toutes les 5 min
- **Historique** : trace `Consolidation terminée: N points` rétrogradée de INFO à DEBUG, suppression du marqueur `DEBUG: Début consolidation historique` ; commentaire corrigé (consolidation effective toutes les 5 min, pas « toutes les heures »)
- **Logger** : intervalle de flush ramené de 60 s à **10 min** (`kFlushIntervalMs = 600000`) — le flush immédiat sur ERROR/CRITICAL et la persistance du coredump couvrent les crashes, l'écriture flash périodique en INFO/DEBUG n'apporte rien
- **Logger** : nouvelle méthode `clearAll()` qui vide RAM + `_persistBuffer` + supprime `/system.log` et `/system.log.tmp` (la méthode existante `clear()` ne touche que le buffer RAM)
- **Diagnostic réseau** : ajout d'un handler `WiFi.onEvent()` dans `setupWiFi()` qui logge les événements `STA_DISCONNECTED` (WARN, avec `reason=N`), `STA_CONNECTED` (INFO), `STA_GOT_IP` (INFO, avec IP et RSSI), `STA_LOST_IP` (WARN). Permet de distinguer un drop Wi-Fi (séquence `DISCONNECTED → CONNECTED → GOT_IP`), un DHCP renew (`LOST_IP → GOT_IP`) ou un problème non-Wi-Fi (broker, firewall) lors d'un blackout réseau
- **Diagnostic MQTT** : log de la cause de déconnexion au front de transition `connecté → déconnecté` (`WARN: MQTT déconnecté détecté — état=N` où `N` est le code `PubSubClient::state()` : `-4` timeout keepalive, `-3` TCP fermé, `-2` TCP refusé, `-1` déconnexion propre). Court-circuit DNS quand `mqttCfg.server` est déjà une IP littérale (`IPAddress::fromString()` avant `WiFi.hostByName()`) — élimine tout cycle DNS pour les installations LAN par IP fixe
- **Stabilité MQTT** : keepalive PubSubClient relevé de 30 s à **60 s** (`mqtt.setKeepAlive(60)` dans `MqttManager::begin()`). La tolérance broker passe corrélativement de 45 s à 90 s (1.5 × keepalive côté Mosquitto), ce qui absorbe les microcoupures réseau prolongées sans déclencher de `état=-4`. Cible : chemins instables type CPL/Powerline où des publishes et PINGREQ se perdent sporadiquement à cause du bruit électrique secteur. Trade-off accepté : une vraie déconnexion (broker arrêté, WiFi coupé) est détectée en 90 s au lieu de 45 s — sans impact sur la régulation pH/ORP qui ne dépend pas d'une latence sub-minute

### Fonctionnalités
- **Paramètres → Avancé** : nouvelle card "Diagnostic crash" — statut coredump (tâche, exception, PC), boutons Actualiser / Télécharger / Effacer, hint de décodage `./tools/decode_coredump.sh`
- **Paramètres → Avancé → Logs** : bouton existant « Effacer » renommé **« Effacer (écran) »** (vide uniquement la vue navigateur) ; nouveau bouton **« Effacer (firmware) »** en rouge `btn--danger` qui appelle `DELETE /logs` après confirmation et purge intégralement les logs côté ESP32 (RAM + fichier persistant)
- **Script** : `tools/decode_coredump.sh` — décode un `coredump.bin` avec `xtensa-esp32-elf-gdb` et `esp_coredump` du penv PlatformIO

### API
- `GET /coredump/info` : résumé JSON du dernier crash (tâche, PC, cause exception)
- `GET /coredump/download` : téléchargement du binaire brut `coredump.bin` (streamé, pas d'allocation 64 KB)
- `DELETE /coredump` : effacement de la partition pour le prochain crash
- `DELETE /logs` (WRITE) : efface intégralement les logs côté ESP32 (RAM + tampon de flush + `/system.log` + `/system.log.tmp`). Réponse `{"success": true}`. Une entrée INFO `Logs effacés (RAM + fichier persistant)` est écrite immédiatement après pour tracer l'action

### Documentation
- `docs/adr/0011-mqtt-task-dediee.md` : ADR créé — déplacement de toute la logique MQTT (publish, connect, loop, callback) dans une tâche FreeRTOS dédiée `mqttTask` ; producer/consumer via deux queues (`outQueue` 32 entrées, `inQueue` 16 entrées) ; arrêt propre `status=offline` avant `ESP.restart()`. Décision motivée par 3 crashes production confirmant que `setSocketTimeout(2)` (ADR-0010) ne couvre pas `lwip_write` sur réseau lossy
- `docs/adr/0010-stabilite-mqtt-reseau.md` : note de mise à jour — l'alternative « Tâche FreeRTOS dédiée pour MQTT » écartée à l'origine est désormais retenue par ADR-0011 (Superseded for MQTT decoupling). Les fixes synchrones D1–D5 restent valides et sont préservés intégralement dans la nouvelle tâche dédiée
- `docs/subsystems/mqtt-manager.md` : refonte majeure — section « Architecture producer/consumer », règles d'or (aucun `mqtt.publish()` depuis `loopTask`, aucun acteur direct depuis `mqttTask`, aucun appel `Async*` depuis `mqttTask`), tableau des paramètres `mqttTask`, refonte gestion des commandes HA (queue entrante drainée par `loopTask`), section « Arrêt propre OTA », troubleshooting drops `outQueue`
- `docs/UPDATE_GUIDE.md` : note user-facing — stabilité réseau améliorée (la régulation continue de tourner même en cas de microcoupures broker MQTT)
- `docs/adr/0010-stabilite-mqtt-reseau.md` : ADR créé — décisions de stabilité réseau (WiFi sans power save, pré-résolution DNS, backoff non réinitialisé)
- `docs/adr/0009-partition-coredump.md` : ADR créé — table de partitions avec coredump + conséquences de migration
- `docs/adr/0007-table-partitions-custom.md` : statut mis à jour → `Superseded by ADR-0009`
- `docs/subsystems/history.md` : partition 64 KB, rétention 7 jours, section protection redimensionnement, budget partition ; clarification que `consolidateData()` appelle `saveToFile()` en interne et que la trace de fin est en DEBUG
- `docs/subsystems/logger.md` : flush 10 min, flush immédiat ERROR/CRITICAL, `_persistBuffer` borné, fichier log 16 KB ; ajout de `clearAll()` et de l'endpoint `DELETE /logs`
- `docs/subsystems/pump-controller.md` : section "Reset journalier" décrivant `tickDailyRollover()` et son emplacement dans `update()` avant `canDose()` ; section "Warnings edge-triggered" listant les six logs concernés et le pattern `static bool`
- `docs/subsystems/mqtt-manager.md` : section "Pattern de connexion (DNS séparé du TCP)" ; clarification que la reconnexion est 100 % pilotée par `update()` ; lien vers ADR-0010
- `docs/API.md` : section "Diagnostic crash (coredump)" avec les 3 endpoints + nouvelle entrée `DELETE /logs`
- `docs/features/page-settings.md` : card "Diagnostic crash" dans le panneau Avancé + section dédiée à la card Logs (4 boutons : Actualiser, Effacer (écran), Télécharger, Effacer (firmware))
- `docs/subsystems/logger.md` : nouvelle section « Logs WiFi » documentant les 4 messages émis par le handler `WiFi.onEvent()` et les codes de raison Wi-Fi courants
- `docs/subsystems/mqtt-manager.md` : note de troubleshooting — vérifier d'abord les logs WiFi (`WARN: WiFi déconnecté (reason=...)`) avant de chercher la cause d'une reconnexion MQTT répétée
- `docs/subsystems/mqtt-manager.md` : section « Diagnostic — Codes d'état PubSubClient » (codes `-4` à `-1` du log `MQTT déconnecté détecté`), tableau du chemin DNS selon IP littérale vs hostname, workflow de troubleshooting basé sur le code d'état
- `docs/subsystems/logger.md` : nouvelle section « Logs MQTT » listant les 6 messages émis par `MqttManager` (incluant le nouveau `MQTT déconnecté détecté — état=N` edge-triggered)
- `docs/subsystems/mqtt-manager.md` : nouvelle section « Keepalive » (60 s côté client, 90 s de tolérance broker, trade-off de détection des vraies déconnexions) ; précision dans le tableau des codes d'état (`-4` = sans PINGREQ/PONG dans la fenêtre 90 s) ; ajout d'une étape 3 au workflow troubleshooting couvrant les chemins physiques instables (CPL/Powerline, RSSI marginal) quand WiFi et broker sont innocentés
- `docs/subsystems/mqtt-manager.md` + `specs/features/done/feature-014-mqtt-task-dediee.md` (D1/D2) : précision du comportement réel de détection des déconnexions MQTT — état dominant `-3` (`MQTT_CONNECTION_LOST`, socket TCP invalide), `-4` (timeout keepalive) quasi-inatteignable car `mqtt.connected()` est testé avant `mqtt.loop()` dans `taskLoop()` ; délai typique 100–180 s sur arrêt brutal du broker (RTO TCP lwip, pas keepalive applicatif), quasi-immédiat sur fermeture propre TCP. Nouvelle section « Détection des déconnexions » dans la doc subsystem

---

## [Unreleased] - 2026-04-24

### Firmware
- **Persistance compteurs journaliers** : `dailyPhInjectedMl` et `dailyOrpInjectedMl` sont désormais persistés en NVS (namespace `pool-daily`) — les compteurs survivent aux reboots ESP32 et sont restaurés si le jour calendaire est identique
- **Reset journalier** : aligné sur minuit local (RTC/NTP) au lieu d'une fenêtre glissante 24 h ; `armStabilizationTimer()` est armé au passage de minuit (mitigation double quota)
- **`kMinValidEpoch`** : constante consolidée dans `src/constants.h` (valeur : 1700000000, 14 nov. 2023)
- **Raison du dernier reboot** : champ `reset_reason` ajouté dans le payload WebSocket `sensor_data` — valeurs possibles : `POWER_ON`, `SW_RESET`, `WATCHDOG`, `BROWNOUT`, `PANIC`, `DEEP_SLEEP`, `EXTERNAL`, `UNKNOWN` ; constant pendant le runtime

### Fonctionnalités
- **Pages /ph et /orp** : les blocs Statistiques sont grisés (`opacity: 0.5`) lors d'une déconnexion WebSocket — indique visuellement que les données affichées ne sont plus à jour
- **Toast reboot inattendu** : un toast dismissable s'affiche une fois par session si le champ `reset_reason` indique un reboot inattendu (`WATCHDOG`, `BROWNOUT` ou `PANIC`) — libellé : « Redémarrage inattendu détecté (raison : X) »
- **Régulation pH** : remplacement du toggle binaire `ph_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation pH** : mode Programmée — volume quotidien configurable (mL), injecté pendant les plages de filtration jusqu'au quota journalier
- **Régulation pH** : migration automatique au premier boot : `ph_enabled=true` → `automatic`, `ph_enabled=false` → `manual`
- **Limites horaires** : renommage `phInjectionLimitSeconds` → `phInjectionLimitMinutes` (idem ORP) — les limites sont désormais saisies en minutes (1–60) au lieu de secondes ; migration NVS transparente au boot (`ph_limit_sec` → `ph_limit_min`)
- **Protection pompes** : suppression de `minPauseBetweenMs` — la pause inter-injections configurable est retirée ; la protection contre le short-cycling reste assurée par `minInjectionTimeMs` (30 s) et `maxCyclesPerDay` (20/24 h)
- **MQTT** : publication des champs `ph_regulation_mode` et `ph_daily_target_ml` dans `publishTargetState()`
- **Sécurité** : suppression du log du mot de passe WiFi en clair dans les traces de reconnexion
- **Régulation pH (Programmée)** : refonte de l'algorithme d'injection — la pompe injecte librement pendant la filtration jusqu'à atteindre le quota journalier (`phDailyTargetMl`), sans répartition sur 24 h ; la limite horaire (`phInjectionLimitMinutes`) reste la seule barrière contre l'injection rapide
- **Régulation ORP** : remplacement du toggle binaire `orp_enabled` par un sélecteur de mode à 3 valeurs (`automatic` / `scheduled` / `manual`)
- **Régulation ORP** : mode Programmée — volume quotidien de chlore configurable (mL), aveugle au capteur ORP, borné par `maxChlorineMlPerDay` ; PID réinitialisé au retour en mode automatique
- **Régulation ORP** : migration automatique au premier boot : `orp_enabled=true` → `automatic`, `orp_enabled=false` → `manual` ; champ `orp_enabled` conservé comme miroir pour compatibilité HA
- **MQTT** : publication des champs `orp_regulation_mode` et `orp_daily_target_ml` dans `publishTargetState()`

### API
- `GET /get-config` / `POST /save-config` : `ph_limit_seconds` → `ph_limit_minutes`, `orp_limit_seconds` → `orp_limit_minutes` ; suppression de `min_pause_between_min`
- WebSocket config : mêmes renommages (`ph_limit_minutes`, `orp_limit_minutes`)
- `GET /get-config` : ajout des champs `orp_regulation_mode`, `orp_daily_target_ml`, `max_orp_ml_per_day`, `orp_cal_valid`
- `POST /save-config` : validation de `orp_regulation_mode` (enum), `orp_daily_target_ml` (borné par `max_orp_ml_per_day`, HTTP 400 si dépassé)

### Fonctionnalités
- **Page pH** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels
- **Page pH** : mode Programmée avec saisie du volume quotidien (mL) borné par la limite journalière configurée
- **Paramètres** : champs durée max pH/ORP en minutes (1–60 min/h) au lieu de secondes ; suppression du champ « Pause entre deux injections »
- **Page ORP** : refonte complète — architecture 4 cartes (Statistiques compact / Régulation / Historique / Calibration conditionnelle)
- **Page ORP** : sélecteur de mode régulation (Automatique / Programmée / Manuelle) avec sous-blocs conditionnels (symétrie avec page pH)
- **Page ORP** : mode Programmée avec saisie du volume quotidien de chlore (mL), borné par la limite journalière de sécurité
- **Page ORP** : calibration accessible uniquement en mode Automatique (bouton Calibrer dans le sous-bloc Automatique) ; carte Calibration en superposition pendant le protocole
- **Page ORP** : bloc Statistiques compact (ORP actuelle + Dosage du jour) en en-tête de page, hors carte

---

## [1.1.0] - 2026-03-29

### Firmware
- **MQTT** : ajout des topics publiés `ph_dosing`, `orp_dosing`, `ph_limit`, `orp_limit`, `ph_target`, `orp_target`
- **MQTT** : ajout des topics de commande `ph_target/set` et `orp_target/set` (modification des consignes pH et ORP depuis HA ou MQTT)
- **MQTT** : correction du switch "Filtration Marche/Arrêt" — la commande `OFF` forçait l'arrêt de la filtration mais elle redémarrait immédiatement selon le planning
- **Home Assistant Auto-Discovery** : ajout de 6 nouvelles entités (Dosage pH Actif, Dosage Chlore Actif, Limite Journalière pH, Limite Journalière Chlore, Consigne pH, Consigne ORP)

### Documentation
- `docs/MQTT.md` : documentation complète des topics publiés, commandes et entités Home Assistant avec les noms tels qu'ils apparaissent dans l'interface HA
- `docs/API.md` : réécriture complète — tous les endpoints documentés (30+)
- `docs/UPDATE_GUIDE.md` : mise à jour avec les modes OTA de `deploy.sh`
- `deploy.sh` : ajout des modes `ota-firmware`, `ota-fs`, `ota-all` (compile + envoi OTA en une commande)
- Renommage `quick_update.sh` → `ota_update.sh`

---

## [1.0.3] - 2026-03-27

### Firmware
- **Factory reset** : détection par appui long 10s pendant le fonctionnement normal — plus besoin de couper l'alimentation
- Suppression des constantes `PH_SENSOR_PIN` / `ORP_SENSOR_PIN` (vestiges ADC interne non utilisés depuis le passage à l'ADS1115 I2C)

### Documentation
- Procédure factory reset mise à jour (fonctionnement runtime)
- Section Matériel Requis : schéma électronique et PCB illustrés, liens vers fichiers Gerber et STL
- `build_all.sh` documenté dans BUILD.md et UPDATE_GUIDE.md

---

## [1.0.1] - 2026-03-26

### Firmware
- **Bouton factory reset (GPIO32)** : appui de 10 secondes au démarrage pour réinitialisation usine complète
  - LED intégrée clignote pendant l'appui pour indiquer la progression
  - Efface la partition NVS (mot de passe, WiFi, MQTT, calibrations)
  - Préserve les consignes, limites et l'historique des mesures
  - L'ESP32 redémarre en mode AP avec l'assistant de configuration

### Hardware
- Ajout des fichiers Gerber (fabrication PCB) dans le dossier `hardware/`
- Ajout des fichiers STL du boîtier v3 (corps + couvercle) dans le dossier `hardware/`

---

## [1.0.0] - 2026-03-24 — Première release publique

### Fonctionnalités
- Régulation automatique pH et ORP (chlore) via algorithme PID
- Gestion filtration (auto / manuel / off) avec programmation horaire
- Contrôle éclairage avec programmation horaire
- Interface web avec tableau de bord temps réel (graphiques pH, ORP, température)
- Intégration Home Assistant via MQTT Auto-Discovery
- Mises à jour OTA via interface web (firmware et filesystem)
- Assistant de configuration au premier démarrage (mot de passe, WiFi, heure)
- Protocole UART pour écran LVGL externe
- Historique des mesures sur partition dédiée (préservé lors des mises à jour)
- Alertes MQTT en cas d'anomalie (valeurs aberrantes, limites atteintes, mémoire faible)
- Factory reset via bouton physique GPIO32
