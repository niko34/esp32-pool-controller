## Métadonnées
- **ID** : feature-210
- **Slug** : dire-ce-que-le-mode-programme-garantit-et-ce-qu-il-ne-garantit-pas
- **Type** : feature (information utilisateur — ⛔ **aucune logique modifiée**)
- **Statut** : ✅ **done** — v3.1.1, livré et vérifié (2 742 cas natifs verts ; captures de documentation = seule étape humaine)
- **Priorité** : à définir
- **Version cible** : **v3.1.1** (`src/version.h` bumpé)
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : `todo/feature-209-securiser-les-injections-par-un-detecteur-de-debit.md`
  (⭐ **c'est le lot qui COMBLE le manque que celui-ci se contente de DIRE**) ·
  `done/feature-056-…` (`InstallMode`, la résolution de présence d'eau) · ADR-0137 (un état qui
  court-circuite le dosage nomme sa cause) · ADR-0140 (ce qui ne bloque pas doit être visible)

## Demande de l'utilisateur — 2026-11-22
Renforcer les avertissements sur les **limites du mode programmé** de filtration.
⛔ **La tâche ne consiste PAS à créer ni à refaire ce mode** : il existe, il est fonctionnel,
il est documenté. Elle consiste à **clarifier son niveau de sécurité**, la circulation de l'eau
n'étant pas nécessairement vérifiée par un détecteur physique.

> *« L'avertissement doit distinguer clairement « **filtration commandée** » et « **circulation
> d'eau confirmée** ». ⛔ Ne prétends pas que l'appareil vérifie le débit si le code ne le fait
> pas. ⛔ Évite aussi de présenter le mode comme dangereux de façon générale : décris factuellement
> la protection disponible et ce qu'elle ne couvre pas. »*

## Domaines touchés
- [x] **Frontend (`data/`)** — un avertissement bref et visible
- [x] **Documentation utilisateur** (`docs/user/src/*.html`) et `docs/features/page-filtration.md`
- [x] **Régulation** — ⚠️ **consommée, ⛔ PAS modifiée** → `pool-chemistry` pour **avis sur le texte**
- [ ] Capteurs · [ ] MQTT · [ ] OTA · [ ] Authentification
- [ ] Web — routes HTTP / WebSocket — ⭐ **rien à ajouter**, voir « Faisabilité »

## Agents requis
- [x] spec-reader · [x] architect
- [x] **pool-chemistry** — ⚠️ **sur le TEXTE, pas sur du code** : c'est lui qui dit ce qu'on a le
      droit d'affirmer sur une garde de dosage, et ⛔ ce qu'on n'a pas le droit de laisser croire.
- [x] **ux-ui-designer** — ⚠️ la page Filtration doit **absorber** un avertissement sans devenir
      un mur de texte ; la règle d'accumulation s'applique.
- [ ] embedded-developer — ⛔ **a priori AUCUN** (voir « Faisabilité »)
- [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

---

# 1. Ce que le code fait RÉELLEMENT — vérifié le 2026-11-22, v3.0.0

## 1.1 Comment le mode programmé commande la filtration
`src/filtration.cpp` : le mode (`auto` / `manual` dit « **Programme** » dans l'interface /
`marche` / `arret`) calcule un **état attendu du relais**, puis l'applique :
`digitalWrite(kFiltrationRelayPin, relayShouldBeOn ? HIGH : LOW)` (`:552`, et `:407` pour la
coupure). ⇒ **Le mode programmé est un calculateur d'horaires qui pilote un relais.**
⛔ **Il n'existe aucune entrée matérielle qui rendrait compte de ce qui se passe en aval.**

## 1.2 Quelle condition autorise ou bloque l'injection
La garde de dosage lit **un verdict unique** : `resolveWaterPresent()`
(`src/dosing_logic.h:70`), alimenté par `filtration.resolveWaterPresence()` et consommé dans
`src/pump_controller.cpp:1754` (`in.waterPresent = …`).

🔴 **Et le code NOMME lui-même ce qu'il mesure.** L'énumération de la source s'appelle :
```cpp
enum class WaterSource : uint8_t {
  FiltrationCommanded,  // Managed : état commandé par PoolController
  PoweredAssumed,       // Powered : présumé par câblage
  ExternalSignal        // External : dernier signal externe
};
```
et le champ d'entrée s'appelle **`bool filtrationCommandedOn;`** (`src/dosing_logic.h:53`).
⭐ **Le mot « commandé » est dans le code. Il n'est nulle part à l'écran.**

## 1.3 Le système vérifie-t-il un débit ? — ⛔ **NON. Aucun des trois modes.**
`src/dosing_logic.h:68-74`, mot pour mot :
| `InstallMode` | Ce qui fait foi | Ce que ça vaut |
|---|---|---|
| **Managed** (l'appareil pilote le relais) | `waterPresent = filtrationCommandedOn` | ⛔ **l'état COMMANDÉ du relais**, ⛔ pas une mesure |
| **Powered** (appareil alimenté par la phase filtration) | `waterPresent = true` — *« eau **présumée** présente »* | ⛔ une **déduction de câblage**, vraie en permanence tant que l'appareil est alimenté |
| **External** (filtration tierce) | dernier signal **reçu**, avec garde de fraîcheur, `fail-closed` | ⛔ une **affirmation d'un tiers**, pas une constatation |

⇒ ⭐ **Dans les trois cas, l'appareil sait ce qu'il a DEMANDÉ ou ce qu'on lui a DIT. Il ne sait
jamais ce qui COULE.**

⚠️ **À l'honneur du code** : la résolution est **fail-closed strict** (*« doute = refus »*,
condition `pool-chemistry` n°2), et le mode `External` **refuse** si le signal est inconnu ou
périmé. ⛔ Le défaut n'est pas une négligence de conception : c'est une **limite physique** —
sans capteur, il n'y a rien à lire.

## 1.4 Ce que cette limite a déjà coûté — ⛔ pas une hypothèse
| Date | Fait | Ce que la garde a vu |
|---|---|---|
| **23/08/2026** | 1ᵉʳ dosage réel, **bypass des sondes FERMÉ**, 765 mL injectés, mesures non représentatives | « filtration commandée » ⇒ ⛔ rien |
| **11/10/2026** | **Chlore injecté dans la dérivation ORP FERMÉE**, sonde dans un hypochlorite ~1000× concentré | ⇒ ⛔ rien |

---

# 2. Ce que l'avertissement doit dire — et ⛔ ce qu'il ne doit pas

## 2.1 La distinction à porter
> **« filtration commandée »** ≠ **« circulation d'eau confirmée »**

⛔ **Interdits formels :**
- ⛔ affirmer ou laisser croire que l'appareil **vérifie** le débit ;
- ⛔ présenter le mode programmé comme **dangereux en général** — il est le mode normal, il
  fonctionne, et la protection existante (fail-closed, limites par injection / horaire /
  journalière, filets bruts de pH) **n'est pas remise en cause** ;
- ⛔ désigner un **coupable** (la pompe, la sonde, l'installateur) — travers documenté du projet ;
- ⛔ un pavé anxiogène : ⚠️ *une alerte dont on apprend qu'elle ne veut rien dire ne se lit plus le
  jour où elle veut dire quelque chose.*

## 2.2 Ce qu'il doit contenir, factuellement
1. **Ce que le mode garantit** : la pompe est **commandée** selon l'horaire choisi.
2. **Ce qu'il ne garantit pas** : qu'**de l'eau circule réellement** devant les points
   d'injection — vanne fermée, dérivation isolée, amorçage perdu, crépine bouchée.
3. **La conséquence concrète**, et ⚠️ elle doit rester mesurée : un produit peut être injecté dans
   une canalisation **sans circulation**. ⇒ Produit concentré au point d'injection, mesures non
   représentatives, et ⚠️ **une régulation qui corrige sur une mesure fausse**.
4. **Les précautions pratiques** — ⭐ c'est la partie utile, et elle doit être **actionnable** :
   - vérifier que **les vannes et dérivations sont ouvertes** avant une mise en service ou une
     reprise de saison ;
   - après toute intervention sur le circuit, **regarder une mesure** avant de laisser doser ;
   - se méfier d'une mesure **qui ne bouge pas** alors que le dosage tourne.
5. ⭐ **Et dire ce qui existe pour aller plus loin** : un **détecteur de débit** (feature-209)
   remplacerait cette déduction par une constatation. ⛔ Sans le présenter comme obligatoire ni
   nommer une marque.

## 2.3 ⚠️ Le texte doit s'adapter au mode d'installation
⛔ **Un texte unique serait faux pour deux des trois modes.** Le niveau d'assurance diffère :
- **Managed** — « la filtration est **commandée** par l'appareil selon votre programme » ;
- **Powered** — ⚠️ **le plus exposé** : `waterPresent = true` **en permanence**. L'appareil
  **présume** l'eau parce qu'il est alimenté par la phase filtration. ⇒ Si la pompe tourne mais
  que l'eau ne circule pas, il **ne peut rien détecter du tout** ;
- **External** — l'appareil **répète ce qu'un tiers lui signale**, et refuse si le signal est
  périmé (⭐ seul des trois à avoir une garde de **fraîcheur**).

---

# 3. Faisabilité — ⭐ aucune ligne de firmware a priori

✅ **`install_mode` est DÉJÀ servi** par `/get-config` (`src/web_routes_config.cpp:226`) et
**déjà lu** côté client (`data/app.js:4824`, `applyInstallMode(cfg.install_mode || "managed")`).
⇒ L'interface peut adapter le texte **sans toucher à `src/`**.

⚠️ **Piège à vérifier avant de s'y fier** : `install_mode` semble **servi par HTTP seul**
(⛔ absent de la trame WebSocket). ⇒ Le même travers que `ph_offset_confirm_interval_days` :
**valider le comportement quand la clé n'est pas là**, et ⛔ ne jamais retomber sur un défaut qui
mentirait sur le mode.

⇒ **Si une ligne de `src/` devient nécessaire, le dire et le justifier** — ⛔ ne pas l'ajouter en
silence.

---


---

# ⛔ VÉRIFICATION DE LA FICHE AVANT TRAITEMENT — 2026-11-22, v3.1.0

La fiche a été écrite contre la **v3.0.0**. Reprise affirmation par affirmation contre le code
et contre l'appareil. **Le fond tient. Deux faits la contredisent, et le second change la forme
du lot.**

## Confirmé, mot pour mot

- `resolveWaterPresent()` et `WaterPresenceInputs::filtrationCommandedOn` existent et portent
  bien ces noms. Le commentaire du code dit lui-même : *Managed → waterPresent =
  filtrationCommandedOn ; Powered → waterPresent = **true** (eau présumée présente)*.
- **Aucun des trois modes ne lit un débit.** ⛔ Le mot « commandé » est dans le code et nulle
  part à l'écran.
- La résolution est **fail-closed strict**, et seul `External` porte une garde de fraîcheur.

## ⛔ FAUX — le « piège à vérifier » du § 3

> « `install_mode` semble **servi par HTTP seul** (⛔ absent de la trame WebSocket). »

Il est servi **dans DEUX trames WebSocket** : `src/ws_manager.cpp:1232` (trame capteurs) et
`:1670`. ⭐ **Et mieux que cela** : dans la trame capteurs il voyage **avec le verdict déjà
résolu** — `water_present` et `filtration_state_stale` sont servis **sur la ligne d'à côté**.
⇒ L'interface n'a pas seulement de quoi connaître le mode : elle a de quoi dire **ce que
l'appareil croit en ce moment**, et **si ce qu'il croit est périmé**.

⚠️ Un vrai point de vigilance subsiste, mais ce n'est pas celui que la fiche désignait :
`data/app.js:4831` fait `applyInstallMode(cfg.install_mode || "managed")` — **le repli par
défaut est `managed`**. Un texte qui s'appuierait sur ce repli **affirmerait le mode le moins
exposé alors qu'on ne sait pas**. ⇒ ⛔ Interdit : en l'absence de la clé, ne rien affirmer.

## 🔴 CE QUE LA FICHE N'AVAIT PAS VU, ET QUI CHANGE LA FORME DU LOT

Le § 4.1 place l'avertissement « près du mode programmé, page Filtration ».
⛔ **Cette page n'existe pas pour deux des trois modes.**

`applyInstallMode()` (`data/app.js:5797`) masque, dès que le mode n'est pas `managed` :
`#nav-filtration` (**l'entrée de navigation**), `#dashboard-filtration-card` et
`#filtration-content`. Il révèle à la place `#filtration-unmanaged-note`.

**Constaté en image** (`/tmp/claude-0/-root-esp32-pool-controller/998bfba8-fb0a-4232-bd9b-381cf4a0a742/scratchpad/f210ui/`) :
mes deux premières captures « powered » et « external » sont restées **sur le tableau de bord** —
le clic sur l'entrée Filtration n'a rien fait, puisqu'elle est invisible. En forçant la route,
la page Filtration en mode `powered` ne contient **qu'une seule phrase** :
> « La filtration n'est pas pilotée par le PoolController dans ce mode d'installation. »

⇒ ⚠️ **Le mode le PLUS EXPOSÉ est celui qui a le MOINS d'endroits pour être prévenu.** En
`Powered`, `waterPresent` vaut **true en permanence**, et la seule page qui pourrait le dire est
**injoignable par la navigation**.
⇒ ⛔ Un avertissement posé uniquement sur la page Filtration ne serait **jamais lu** par deux
tiers des configurations possibles. La place doit être tranchée en conséquence — les cartes de
choix du mode vivent, elles, dans **Réglages** (`data/index.html:2167-2185`), page accessible
dans tous les modes.

## Ce qui existe déjà et qu'il ne faut pas doublonner

- `#filtration-unmanaged-note` — un `callout--info` d'une ligne, déjà en place.
- Les trois cartes de choix du mode dans Réglages, avec un bloc d'aide `#install-ext-status`
  pour `external`.
- `water_present` et `filtration_state_stale` servis — ⭐ **rien à ajouter côté firmware**,
  la faisabilité « zéro ligne de `src/` » est **confirmée, et plus largement que prévu**.

# 4. Où l'information va

## 4.1 Interface
- **Près du mode programmé ou de son réglage**, page Filtration. ⚠️ **Forme à trancher par
  `ux-ui-designer`** : la page est déjà dense, et la règle d'accumulation s'applique.
- ⭐ **Piste à lui soumettre** : plutôt qu'un encadré permanent de plus, un **fait court et
  permanent** (sur le modèle de `bug-201 (C28)`) + le détail derrière un repliable.
  ⚠️ ⛔ Mais la **phrase qui porte la limite** ⛔ ne doit **pas** être repliée : *l'utilisateur
  n'ouvre pas ce qui est replié* (acquis de `feature-170 (C29)`).
- ⛔ **Aucun `aria-live`** sur un texte permanent : ce n'est pas une alerte, c'est un **fait**.

## 4.2 Documentation
- `docs/features/page-filtration.md` — **au même endroit** que la description du mode.
- `docs/user/src/guide-utilisation.html` et/ou `guide-regulation.html` — ⛔ **sources HTML
  seulement**, ⛔ ne pas régénérer les PDF (artefacts de release).
- ⚠️ **Et la limite doit être écrite là où la règle de sécurité est énoncée** :
  `CLAUDE.md` disait *« TOUJOURS vérifier la présence d'eau (**filtration active**) »* — ⭐ c'est
  exactement la formule qui entretenait la confusion. ⛔ `CLAUDE.md` ne se modifie **pas** sans
  l'accord explicite de l'utilisateur : la reformulation lui a été **soumise**, puis **appliquée
  sur son accord du 2026-11-22**. La règle n° 1 nomme désormais le point de passage obligatoire
  (`resolveWaterPresent()`, fail-closed, ADR-0026) au lieu de promettre une vérification, et
  **interdit explicitement** d'écrire « l'appareil vérifie la présence d'eau ».

---


---

# ⭐ VISA `pool-chemistry` SUR LE TEXTE (livrable A0) — transcrit ici

⛔ Un gate ne garde aucune mémoire entre deux passages. Sans cette section, les règles
ci-dessous seraient invérifiables par le prochain lecteur. **Verdict : GO SOUS CONDITIONS.**

## Les trois règles opposables, applicables phrase par phrase

1. **Le test du verbe.** ⛔ Interdits dès qu'ils portent sur l'eau, le débit ou la circulation :
   *vérifie, contrôle, détecte, mesure, constate, s'assure, confirme, garantit, surveille,
   empêche*. ✅ Autorisés : *commande, suppose, se fie à, considère, reçoit, attend, refuse,
   suspend*.
2. **Le test de la source.** Toute phrase qui **autorise** doit nommer **d'où vient
   l'information**. ⇒ « eau présente » n'est jamais un libellé : c'est une conclusion sans source.
3. **L'asymétrie de rigueur.** *On peut dire moins que la vérité quand on refuse ; jamais plus
   quand on autorise.* Sur-dire un refus est sans conséquence. Sur-dire une autorisation est
   précisément ce qui a coûté 765 mL le 23/08/2026.

## Conditions

| # | Condition | État |
|---|---|---|
| C1 | ⛔ Supprimer « Ce réglage garantit qu'aucun produit n'est injecté sans circulation d'eau » **partout**. Sans cela le lot se contredit, et l'assistant continue d'enseigner l'erreur. | ✅ **six** exemplaires trouvés au total, tous retirés — le visa en annonçait deux, la doc en a trouvé un troisième, la revue un quatrième et un cinquième, et le sixième (`guide-regulation.html`, autre formulation) a été trouvé en étendant la garde |
| C2 | ⛔ Aucun affichage **positif** de `water_present`. L'interface parle quand l'eau MANQUE et se tait sinon — asymétrie déjà en place, à préserver telle quelle. | ✅ aucune consommation nouvelle de `water_present` ni de `filtration_state_stale` |
| C3 | `install_mode` absent ⇒ **aucune affirmation propre à un mode**. ⛔ Le repli `\|\| "managed"` ne doit pas choisir un texte. | ✅ `installScopeText()` rend `null`, 3 vecteurs, mutation tuée |
| C4 | La 3ᵉ précaution de la fiche — « se méfier d'une mesure qui ne bouge pas » — **n'est pas fiable en l'état** : sur 30-50 m³ tamponnés, une mesure qui ne bouge pas est le cas NORMAL, et la régulation étant un **P pur**, « ça dose et ça ne bouge plus » est l'équilibre attendu, pas un défaut. ⇒ reformulée sur plusieurs jours + test manuel, **et** assortie d'une phrase de non-alarme. | ✅ les deux sont dans le repliable |
| C5 | Capteur de circulation : **documentation seulement**, une phrase, clause de non-support incluse. ⛔ Pas dans l'interface : il y deviendrait la publicité d'une fonction inexistante et affaiblirait la seule action utile aujourd'hui. | ✅ guide utilisateur uniquement |
| C6 | ⛔ **Aucune formulation invitant à couper la régulation.** Un bassin privé d'acide 48 h part à pH 8,2-8,4, où la fraction de chlore actif tombe sous 25 %. Le réflexe de doute enseigné est « ouvrir les vannes + test manuel ». | ✅ vecteur dédié |

## ⛔ Deux prémisses de la fiche que le visa déclare FAUSSES

1. **« `Powered` est le plus exposé » — non.** Dans ce mode, l'appareil **et les pompes doseuses**
   sont alimentés par le circuit de la pompe de filtration (ADR-0026 : *PC vivant ⟺ eau*). Pompe
   non alimentée ⇒ appareil éteint ⇒ **aucune injection possible, par construction physique**.
   Il est **aveugle dans le logiciel mais protégé par le câblage**, et sa protection électrique est
   *plus serrée* que celle de `managed`, qui ne sait ni si le contacteur a collé ni si le
   disjoncteur de la pompe a sauté. ⇒ **Ton identique aux deux autres**, contenu différent.
   ⚠️ Son vrai problème n'est pas le ton, c'est **l'accessibilité** — d'où le placement en Réglages.
2. **« La garde de fraîcheur d'`External` est un avantage » — à nuancer.** Elle contrôle la
   **fraîcheur**, pas la **véracité**. Une domotique qui republie « on » toutes les 30 s sans
   condition passe la garde en permanence. C'est le seul point où `external` est réellement plus
   exposé que les deux autres, et le texte livré le dit.

## ⭐ Le fait rassurant le plus solide, que la fiche ignorait

**Après chaque reprise de l'eau, l'appareil attend 10 minutes avant de doser**
(`kPurgeFloorMs`), sauf arrêt de moins de 3 minutes. Vrai dans les **trois** modes. Il rééquilibre
le texte sans rien promettre de faux, et il est désormais dit dans les trois.

## Revue de code — constats traités

| # | Constat | Correction |
|---|---|---|
| Garde contournable | L'analyse **ligne à ligne** laissait passer la phrase coupée à 80 colonnes, et l'exemption « c'est un commentaire » acceptait **toute ligne contenant un guillemet français** — soit la prose normale de l'interface. ⚠️ La garde était **verte pendant que la promesse était vivante** : exactement l'incident qu'elle existe pour éviter. | commentaires retirés avant analyse, espaces normalisés, recherche sur le document entier, découpage en **phrases** avec marque de rétractation exigée ; liste portée de 4 à **6** fichiers |
| `page-wizard.md` | La promesse y vivait encore — et c'est la doc où un développeur irait recopier la formule. | retirée, fichier ajouté à la garde |
| Cible tactile | `padding: 14px` seul ne **garantit** pas 44 px : `0.85rem` vaut 13,6 px et `line-height: normal` vaut ~1,15 sur une pile Arial ⇒ 43,6 px. | `line-height: 1.3` déclarée ⇒ 45,7 px indépendamment de la police |
| Aperçu non enregistré | Cliquer un autre raccordement **pour le lire** affichait un fait de sécurité faux pour l'installation réelle. | le texte est qualifié « Si vous enregistrez ce choix : … » tant qu'il ne décrit pas le mode enregistré |
| Job CI | Inséré entre un commentaire et le job qu'il documentait. | reposé après |

## ⛔ Reste ouvert, écrit et non traité

1. **L'autorisation reste invisible** : l'appareil ne dit jamais *pourquoi* il s'autorise à doser.
   C'est la dette assumée d'ADR-0144, levée le jour où une constatation existera (feature-209).
2. **`.tips > summary` n'a aucun padding** : même défaut de cible tactile, **non corrigé** par ce lot.
3. **ADR-0026 porte la même formule trop forte** (« plus de risque d'injection en eau stagnante »).
   Un ADR accepté est immuable ⇒ ADR-0144 le **rectifie** explicitement.
4. **`CLAUDE.md` est reformulé** (accord explicite de l'utilisateur, 2026-11-22) : la règle n° 1
   exige désormais le passage par `resolveWaterPresent()` et **interdit** la formule
   « l'appareil vérifie la présence d'eau ». ⭐ Conséquence repérée : **feature-209**
   (détecteur de débit, toujours en `todo/`) **fonde sa justification sur l'ancienne formule** —
   elle n'est pas affaiblie, elle devient le seul chemin vers une **vraie** constatation.

# 5. Ce qu'il ⛔ NE FAUT PAS toucher
⛔ La logique de programmation · ⛔ la commande des relais · ⛔ le dosage · ⛔ les sécurités
existantes · ⛔ l'interface **fonctionnelle**.
⚠️ **Seule exception admise** : une modification strictement **indispensable pour afficher**
l'information — et alors **nommée et justifiée** dans le compte rendu.

# 6. Livrables — check-list
- [x] **A0** — avis `pool-chemistry` **sur le texte** : ce qu'on a le droit d'affirmer, et ⛔ ce
      qu'on n'a pas le droit de laisser croire — vocabulaire opposable (**commande / présume /
      reçoit**), interdit d'alarmer sur un mode normal, interdit d'inviter à couper la régulation
- [x] **A1** — étude `ux-ui-designer` : forme et place, **sur captures 390 px**, avec les
      compensations si la page s'alourdit — `callout--info` à trois étages, limite jamais repliée,
      phrase **absorbée** dans l'aide existante côté page Filtration (⛔ aucun encadré de plus)
- [x] **B1** — avertissement dans l'interface, **adapté aux trois `InstallMode`**
      (`installScopeText()` / `renderInstallScope()`, `#install-scope`, `#filtration-mode-scope`,
      `wizard.html` étape 3)
- [x] **B2** — documentation utilisateur + `docs/features/page-filtration.md`
      (+ `page-settings.md`, `subsystems/filtration.md`, `DESIGN_SYSTEM.md`, **ADR-0144**)
- [x] **D1** — vecteurs : un texte **par mode**, et ⭐ le cas **`install_mode` absent**
      (⛔ ne jamais mentir sur le mode) — `test/install_scope_vectors.js`, 11 vecteurs,
      job CI `install-scope-vectors`
- [x] **D2** — capture 390 px, avant / après, **pour les trois `InstallMode`** : `avant_install_
      managed|powered|external.png` puis `apres_managed|powered|external.png`, et la vérification
      finale `final_external.png` (encadré rendu au-dessus du bloc « État du signal filtration »).
      ⚠️ Captures d'étude, non versionnées. ⛔ Les deux captures **de la documentation** restent
      à refaire (voir ci-dessous) : **seule étape humaine** de ce lot.
- [x] **C1** — `CHANGELOG` (**[3.1.1]**) + `src/version.h` → `3.1.1`.
      ✅ **`CLAUDE.md` reformulé** après soumission et **accord explicite de l'utilisateur**
      (2026-11-22) — ⛔ jamais de la seule initiative d'un agent.
- [x] **E1** — compte rendu final : **fichiers modifiés, texte ajouté, et les éléments de code qui
      justifient la formulation retenue** (exigence explicite de l'utilisateur) — ⬇️ **§ 9**

# 7. Sécurité chimique
- Dosage impliqué : ⛔ **non, aucune ligne** — mais le **texte parle d'une garde de dosage**, donc
  le gate passe.
- 🔴 **Le vrai risque de ce lot est le texte lui-même**, dans les deux sens :
  - **trop rassurant** ⇒ on laisse croire à une vérification qui n'existe pas ;
  - **trop alarmant** ⇒ l'utilisateur désactive la régulation « par prudence » et le bassin n'est
    plus traité. ⚠️ Chiffré ailleurs par le gate : *un bassin privé d'acide 48 h part à pH 8,2-8,4,
    où la fraction de chlore actif tombe sous 25 %.*
  ⇒ ⭐ **C'est pourquoi `pool-chemistry` valide le TEXTE, pas du code.**

# 8. Hors périmètre
- ⛔ Ajouter un détecteur de débit : c'est **feature-209**.
- ⛔ Modifier la résolution de présence d'eau, ou l'un des trois `InstallMode`.
- ⛔ Toucher à `CLAUDE.md` sans accord explicite.


---

# 7bis. Journal de clôture documentaire — 2026-11-22 (doc-writer)

**Version** : `src/version.h` **3.1.0 → 3.1.1** (correctif : interface + documentation, ⛔ zéro ligne
de `src/` hors `version.h`).

## Décision formalisée

🔴 **[ADR-0144](../../../docs/adr/0144-l-appareil-ne-constate-jamais-l-eau-il-ne-nomme-que-son-absence.md)
— « L'appareil ne constate jamais la présence d'eau : il ne nomme que son absence ».**
`water_present` n'est **jamais** affiché quand il autorise ; l'interface parle quand l'eau **manque**
(« Filtration arrêtée », « Signal filtration périmé ») et se tait sinon. Trois alternatives écartées,
dont l'affichage **qualifié** de l'état courant — défendable (le champ est servi, et ADR-0140 valorise
la visibilité permanente), écartée parce que le qualificatif ne survit pas à la lecture, qu'une ligne
verte permanente cesse d'être lue, et qu'en `powered` elle vaudrait `true` 100 % du temps.
L'ADR fixe aussi deux règles dérivées : ⛔ un texte ne se replie **jamais** sur un défaut quand il
**affirme** un fait (replier pour **masquer** reste sûr), et ⛔ une information vraie dans les trois
raccordements ne vit pas sur une page masquée dans deux d'entre eux.

## Documents touchés

| Fichier | Nature |
|---|---|
| `CHANGELOG.md` | section **[3.1.1]** — Fonctionnalités + Documentation |
| `src/version.h` | `3.1.1` |
| `docs/adr/0144-*.md` | **créé** |
| `docs/adr/README.md` | index |
| `docs/features/page-settings.md` | `#install-scope` : les quatre étages, les trois textes mot pour mot, l'interdit de repli, le vocabulaire opposable, **pourquoi ici et pas sur Filtration** |
| `docs/features/page-filtration.md` | nouvelle section « Ce que l'appareil sait — où vit l'information » ; note non pilotée qui cesse d'être un cul-de-sac ; phrase absorbée dans `#filtration_mode_hint` |
| `docs/subsystems/filtration.md` | ⛔ **correction d'une formule trop forte** (voir ci-dessous) + ce que nomme `WaterSource` |
| `docs/DESIGN_SYSTEM.md` | §5.10bis : **3ᵉ contexte d'emploi** (page Installation, 1ᵉʳ emploi **permanent**) ; ⛔ **correction d'une affirmation fausse** sur la cible tactile |
| `docs/user/src/guide-utilisation.html` | étape 3 complétée ; ⛔ **4ᵉ occurrence** de la promesse fausse retirée (chapitre 10) |

## Deux affirmations fausses corrigées dans la documentation existante

1. `docs/subsystems/filtration.md` — la garde de fraîcheur du mode `external` « évite l'injection dans
   une eau stagnante ». ⛔ Elle évite de doser sur un **signal perdu**. Un signal `ON` récent mais
   **faux** autorise le dosage.
2. `docs/DESIGN_SYSTEM.md` — le `padding: 10px` du `summary` « garantit » la cible tactile. ⛔ Il donnait
   ~36 px, **sous les 44 px** exigés (`0.85rem` sans `line-height`, ≈ 16 px + 2 × 10). Passé à **14 px**
   (16 + 2 × 14 = 44). ⚠️ `.tips > summary` porte le **même** défaut et n'a **pas** été corrigé —
   dette écrite, pas réparée.

⚠️ **[ADR-0026](../../../docs/adr/0026-mode-installation.md) porte la même formule trop forte**
(« plus de risque d'injection en eau stagnante ») et ⛔ **n'est pas modifiable** : ADR-0144 la rectifie.

## Captures — ce qui est à refaire, fichier regardé avant d'être nommé

| Capture | Verdict |
|---|---|
| `screenshots/10_Paramètres_1_Installation.jpg` | 🔴 **à refaire** — affiche encore « Ce réglage garantit qu'aucun produit n'est injecté sans circulation d'eau », et ne montre pas l'encadré |
| `screenshots/01_Assistant_Etape_3_Installation.jpg` | 🔴 **à refaire** — même phrase, et la précaution ajoutée est absente |
| `screenshots/04_Filtration.jpg` | ⓘ **périmée depuis feature-146** (elle montre l'ancienne aide du seul mode Auto) ; ce lot ajoute une phrase à cette même aide **sans créer la dette** |

## Reste ouvert

- ✅ `docs/user/src/guide-regulation.html` portait **la même promesse sous une autre forme**
  (« Jamais de dose sans eau en circulation ») — **6ᵉ exemplaire**, trouvé par la garde lexicale
  **durcie en revue** et **traité dans ce lot**. ⚠️ La garde précédente ne couvrait pas ce fichier.
- ✅ `CLAUDE.md` **reformulé** sur accord explicite de l'utilisateur (2026-11-22) : la règle n° 1
  nomme `resolveWaterPresent()` et interdit la formule retirée de l'interface.
  ⚠️ **Reste à faire, hors de ce lot** : **feature-209** (`todo/`) cite l'ancienne règle comme
  justification — à relire à son ouverture, elle n'est pas invalidée.
- ⛔ Les PDF utilisateur **n'ont pas été régénérés** (artefacts de release).

---

# 9. Compte rendu final (livrable E1) — 2026-11-22, v3.1.1

## 9.1 Chaque phrase livrée, et le code qui la rend VRAIE

⚠️ Exigence explicite de l'utilisateur : ⛔ aucune phrase d'interface ne doit reposer sur une
intention. Les trois textes de `installScopeText()` sont adossés, mot par mot, à
`resolveWaterPresent()` ([`src/dosing_logic.cpp`](../../../src/dosing_logic.cpp)) — **source
unique** de la présence d'eau (ADR-0026), fail-closed sur mode inconnu.

| Mode | Phrase livrée | Ce qui la prouve dans le code |
|---|---|---|
| `managed` | « Il **commande** la pompe selon votre programme, n'injecte rien en dehors, et attend **10 min** après un démarrage. » | `case ManagedFiltration` → `{ in.filtrationCommandedOn, WaterSource::FiltrationCommanded }` — l'eau est **ce qui a été commandé**. Les 10 min sont `kPurgeFloorMs = 600000UL` (`constants.h:3591`), **plancher fixe**, ⛔ pas un réglage utilisateur. |
| `powered` | « **Alimenté** par le circuit de filtration : pompe à l'arrêt, l'appareil l'est aussi et rien ne peut être injecté. » | `case PoweredByFiltration` → `{ true, WaterSource::PoweredAssumed }`. ⭐ Ce `true` **permanent** n'est pas une faiblesse : ADR-0026 (« PC vivant ⟺ eau ») — appareil **et** pompes doseuses sont sur la phase filtration, donc pompe coupée ⇒ appareil coupé ⇒ **injection impossible par construction**. |
| `external` | « Il se fie à l'état qu'un autre système **envoie**, et cesse de doser si plus rien ne parvient pendant **3 min**. » | `case ExternalFiltration` : `externalSignalKnown` testé **avant** l'âge (un signal jamais reçu n'a pas d'âge signifiant → refus), puis `age <= kExternalFiltrationStaleMs = 180000UL` (`constants.h:3318`). |

⛔ **Aucun des trois ne lit un débit.** C'est l'énoncé d'ADR-0144, et la raison d'être du lot.

## 9.2 Le mode INCONNU n'affirme rien

⭐ Le défaut le plus facile à réintroduire : `applyInstallMode()` replie déjà sur `managed` pour
**masquer l'interface** (repli légitime) et `loadConfig()` fait `cfg.install_mode || "managed"`.
Reconduire ce repli sur le **TEXTE** affirmerait un programme et un relais qui n'existent
peut-être pas. ⇒ `installScopeText()` rend `null`, le bloc est masqué, et **deux vecteurs**
(I2) le gardent. C'est aussi pourquoi `renderInstallScope()` est appelé avec la valeur **brute**,
⛔ jamais avec la valeur replieée.

## 9.3 Fichiers modifiés — 20 au total

**Interface (4)** : `data/app.js` (`installScopeText()` / `renderInstallScope()`),
`data/index.html` (encadré `#install-scope`, `aria-describedby`, phrase sur
`#filtration_mode_hint` et `#filtration-unmanaged-note`), `data/wizard.html` (étape 3),
`data/app.css` (`.callout__more > summary`, cible tactile **déterministe** à 44 px via
`line-height: 1.3` + `padding: 14px 0`).

**Tests et CI (3)** : `test/install_scope_vectors.js` (**11 vecteurs**),
`tools/check_install_scope_vectors.sh`, `.github/workflows/hygiene-specs.yml` (job
`install-scope-vectors`, placé après `daily-dose-limit-vectors`).

**Documentation (9)** : **ADR-0144** (neuf), `docs/adr/README.md`,
`docs/features/page-filtration.md`, `page-settings.md`, `page-wizard.md`,
`docs/subsystems/filtration.md`, `docs/DESIGN_SYSTEM.md`,
`docs/user/src/guide-utilisation.html`, `guide-regulation.html`.

**Projet (4)** : `CHANGELOG.md` (**[3.1.1]**), `src/version.h` (**3.1.1**), **`CLAUDE.md`**
(règle n° 1 reformulée, accord explicite de l'utilisateur), cette fiche.

## 9.4 ⭐ La garde lexicale était VERTE pendant que la promesse fausse vivait

⚠️ C'est le constat le plus important du lot, et il vient de la **revue de code**, qui a attaqué
le contrôle au lieu du code. L'analyse **ligne à ligne** était contournable de **trois** façons,
chacune démontrée par exécution :

1. la prose de ces fichiers est coupée à ~80 colonnes ⇒ la phrase fautive s'étale sur **deux**
   lignes dont **aucune** ne correspond au motif ;
2. l'exemption « c'est un commentaire » acceptait toute ligne contenant « » » — or le guillemet
   français est le guillemet **normal** de l'interface ;
3. une incise passait pour la même raison.

⇒ Commentaires retirés **avant** analyse, espaces normalisés, recherche sur le **document
entier**, découpage en **phrases**, et une **marque de rétractation** exigée (une documentation a
le droit de **citer** ce qu'elle a retiré, ⛔ pas de l'**affirmer**).

⭐ **Effet immédiat du durcissement** : la liste est passée de **4 à 6 fichiers**, et la garde a
trouvé **deux exemplaires de plus** — dont `docs/features/page-wizard.md`, c'est-à-dire
**l'endroit exact où un développeur serait allé recopier la formule**.
⇒ **La promesse fausse existait en SIX exemplaires, pas trois.**

## 9.5 Vérification finale

- `pio test -e native` : **2 742 cas, 2 742 réussis** — ⛔ zéro régression.
- Tous les `tools/check_*.sh` verts, **sauf** `check_release_artefacts.sh` — **attendu** :
  il exige les signatures, qui ne se produisent qu'à la release.
- `check_doc_links.sh` : **7 273 liens**, toutes les cibles existent (relancé **après** la
  promotion vers `done/` — c'est la promotion qui casse les liens).
- `./build_fs.sh` relancé ; rendu 390 px vérifié **visuellement** dans les trois modes.
- **11 mutations** sur les sentinelles neuves, **toutes tuées** ; sources restaurées
  **octet pour octet** (`diff`).

## 9.6 Ce qui reste à l'humain

📸 **Deux captures** — `screenshots/10_Paramètres_1_Installation.jpg` et
`screenshots/01_Assistant_Etape_3_Installation.jpg` : elles montrent **encore la phrase
retirée**. ⚠️ `screenshots/04_Filtration.jpg` est périmée **depuis feature-146**, ⛔ pas du fait
de ce lot.
