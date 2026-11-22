## Métadonnées
- **ID** : feature-210
- **Slug** : dire-ce-que-le-mode-programme-garantit-et-ce-qu-il-ne-garantit-pas
- **Type** : feature (information utilisateur — ⛔ **aucune logique modifiée**)
- **Statut** : todo
- **Priorité** : à définir
- **Version cible** : à définir
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
  `CLAUDE.md` dit *« TOUJOURS vérifier la présence d'eau (**filtration active**) »* — ⭐ c'est
  exactement la formule qui entretient la confusion. ⛔ **Ne pas modifier `CLAUDE.md` sans l'accord
  explicite de l'utilisateur** : le signaler, et lui proposer la correction.

---

# 5. Ce qu'il ⛔ NE FAUT PAS toucher
⛔ La logique de programmation · ⛔ la commande des relais · ⛔ le dosage · ⛔ les sécurités
existantes · ⛔ l'interface **fonctionnelle**.
⚠️ **Seule exception admise** : une modification strictement **indispensable pour afficher**
l'information — et alors **nommée et justifiée** dans le compte rendu.

# 6. Livrables — check-list
- [ ] **A0** — avis `pool-chemistry` **sur le texte** : ce qu'on a le droit d'affirmer, et ⛔ ce
      qu'on n'a pas le droit de laisser croire
- [ ] **A1** — étude `ux-ui-designer` : forme et place, **sur captures 390 px**, avec les
      compensations si la page s'alourdit
- [ ] **B1** — avertissement dans l'interface, **adapté aux trois `InstallMode`**
- [ ] **B2** — documentation utilisateur + `docs/features/page-filtration.md`
- [ ] **D1** — vecteurs : un texte **par mode**, et ⭐ le cas **`install_mode` absent**
      (⛔ ne jamais mentir sur le mode). **Mutation** : servir un texte unique pour les trois
      modes ⇒ un vecteur **meurt**
- [ ] **D2** — capture 390 px, avant / après
- [ ] **C1** — `CHANGELOG` + proposition de correction de la formule de `CLAUDE.md`
- [ ] **E1** — compte rendu final : **fichiers modifiés, texte ajouté, et les éléments de code qui
      justifient la formulation retenue** (exigence explicite de l'utilisateur)

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
