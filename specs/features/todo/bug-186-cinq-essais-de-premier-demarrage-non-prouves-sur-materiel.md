# bug-186 — Cinq comportements de premier démarrage corrigés mais JAMAIS vus sur un appareil réel

## Métadonnées
| | |
|---|---|
| Type | **vérification** — 🔴 sécurité, authentification |
| Statut | todo |
| Priorité | normale — ⛔ **mais BLOQUANTE avant toute livraison client** |
| Version cible | aucune (aucun code à écrire — sauf si un essai échoue) |
| Relevé par | clôture de [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md), 2026-11-17 |
| Échéance | **repoussée par l'utilisateur le 2026-11-17** : « dans quelques mois », avec un appareil réinitialisé (horizon début 2027) |
| Specs liées | [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) · [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) |

## Domaines touchés
- [x] **Authentification** · [x] **Web — routes HTTP** · [x] Frontend (`data/login.html`, page de secours OTA)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT

## Agents requis
- [x] test-engineer — ⚠️ essais **sur matériel**, pas en natif
- [ ] spec-reader · [ ] architect · [ ] embedded-developer · [ ] code-reviewer · [ ] doc-writer
  *(à cocher uniquement si un essai échoue)*

## Description

`bug-185` (livrée en **2.93.0**, déployée le 2026-11-17) corrige cinq comportements du **premier
démarrage**. Quatre points ont été **mesurés sur l'appareil réel**. ⛔ **Cinq ne l'ont pas été** :
ils exigent un appareil **en état vierge** ou **« assistant interrompu »**, état qui ne s'obtient
sur l'appareil de production qu'en **effaçant sa mémoire de configuration** — étalonnages,
consignes et programmation de filtration compris.

⚠️ **Le risque n'est pas théorique** : ces cinq chemins sont **exactement ceux que parcourt un
client à la livraison**, et aucun humain ne les a jamais vus fonctionner sur cette version.

⭐ **Le défaut G1 est la raison d'être de cette fiche.** Pendant le développement de `bug-185`,
une première correction, pourtant juste côté appareil, laissait l'écran de connexion renvoyer
vers l'assistant sur un appareil déjà possédé — **impasse complète**. Elle n'a été trouvée que
parce qu'un relecteur a suivi le chemin de bout en bout. Le correctif qui a suivi **n'a jamais
été vu à l'écran**.

## Les cinq essais à rejouer

| # | Essai | Attendu | Pourquoi il compte |
|---|---|---|---|
| 1 | Prendre une clé d'accès **pendant** la fenêtre d'installation, puis choisir le mot de passe | l'ancienne clé est **refusée** | c'est le cœur du correctif |
| 2 | `admin` / `admin` sur un appareil **encore au mot de passe d'usine** | **403** « changez le mot de passe », ⛔ jamais de clé | ⚠️ non observable sur l'appareil de production : son mot de passe n'est pas celui d'usine, le refus tombe plus tôt (**401**) |
| 3 | Page de secours OTA, en état **vierge** **et** en **« assistant interrompu »** | les deux aboutissent à l'écran d'envoi | ⛔ dernier filet de réparation : s'il échoue, l'appareil est perdu (pas de port série) |
| 4 | `login.html` en **« assistant interrompu »** | affiche le **formulaire de connexion** | ⭐ c'est **G1** — corrigé, jamais revu en vrai |
| 5 | Parcours d'assistant **complet** après un appui long sur le bouton | aboutit, sans impasse | le geste réel du client |

⚠️ **Rejouer l'essai 1 APRÈS un redémarrage** : c'est le seul qui attrape une clé mal enregistrée.
*(La persistance a déjà été prouvée le 2026-11-17 sur un appareil possédé — reste à la prouver
depuis l'état vierge.)*

## 📦 Deux essais OTA portés ici par feature-151 (ajoutés le 2026-11-21)

[feature-151](../done/feature-151-signature-de-l-image-ota.md) est **close** : le refus est
prouvé (4 essais négatifs, 2026-11-17) et l'acceptation aussi (4 déploiements sur 4, chemins
script **et** page, 2026-11-20/21). ⚠️ **Deux essais n'ont jamais pu être faits sur l'appareil de
production**, pour la même raison que les cinq ci-dessus : ils exigent un appareil que l'on peut
casser. Ils atterrissent donc ici.

| Essai | Ce qu'il faut observer | Pourquoi il n'a pas été fait |
|---|---|---|
| **F9 — expiration naturelle de l'autorisation temporaire** | Ouvrir l'autorisation de mise à jour non signée, **ne rien en faire**, et vérifier qu'elle se referme **seule à 60 min** (`no_entry` au journal, témoin MQTT → OFF). | Le décompte a été vu décroître, mais l'autorisation a **toujours été consommée par un flash** avant son terme. ⭐ L'expiration est mesurée sur un **horodatage NVS** ([ADR-0125](../../../docs/adr/0125-l-expiration-de-l-interrupteur-de-signature-se-mesure-sur-un-horodatage-nvs.md)) — elle doit donc survivre à un redémarrage, ce qui n'a jamais été vérifié non plus. |
| **Refus tardifs — liaison image↔manifeste DIVERGENTE** | Envoyer une image dont l'empreinte réelle **ne correspond pas** au manifeste signé, et vérifier le refus en fin de flux (400 texte / 422 `integrity_mismatch`). | Essai **destructif** : il interrompt un flash en cours. ⭐ Le **versant positif** est prouvé (verdicts *liée — divergente : non*, au paramètre d'entrée **et** sur l'empreinte calculée) ; ce qui manque est le versant négatif. |

⚠️ **Ces deux essais ne remettent rien en cause** : ils éprouvent des chemins **livrés et
revus**, pas des comportements inconnus. ⛔ Ne pas les lire comme une réserve sur la signature OTA.

## Critères d'acceptation
- [ ] Les 5 essais sont **mesurés** sur un appareil réinitialisé, et le relevé est écrit ici
- [ ] ⛔ Tout écart ouvre une fiche de correction — ⚠️ ne pas corriger à la volée dans celle-ci
- [ ] **F9** — expiration naturelle de l'autorisation à 60 min, relevée au journal (porté par feature-151)
- [ ] **Refus tardifs** — liaison image↔manifeste divergente refusée en fin de flux (porté par feature-151)

## Notes pour agent
- ⛔ **Ne jamais réinitialiser l'appareil de production** pour cette fiche : l'utilisateur a
  explicitement prévu un **appareil dédié**.
- ⚠️ `./ota_update.sh --sign-only both` puis `--use-sig` : signer est le **dernier** geste, et
  rien ne doit recompiler entre les deux.
- ⭐ La montée de version **renouvelle la clé d'accès** : un script qui garde la sienne entre
  deux envois se fait refuser le second. Observé le 2026-11-17.
