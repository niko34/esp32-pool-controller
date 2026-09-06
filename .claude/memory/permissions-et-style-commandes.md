---
name: permissions-et-style-commandes
description: Règle de validation des commandes Bash (projet vs extérieur) et style de commandes à adopter pour ne pas déclencher de prompts inutiles
metadata:
  node_type: memory
  type: feedback
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-08-27T20:43:05.658Z
---

**Règle posée par l'utilisateur (2026-08-26, redemandée le 2026-08-27)** : ne PAS
demander de validation pour une commande Bash qui ne touche aucun fichier, ou
seulement des fichiers de ce projet. Demander TOUJOURS pour ce qui sort du projet.

**Why:** deux causes distinctes produisent des prompts, et la seconde est la
principale — c'est pour ça que la règle a dû être redemandée un jour après avoir
été « traitée ».

1. **Des outils absents de l'allowlist.** Complétée le 27/08 (projet : 120 → 203
   règles ; global : 21 → 104) : compilateurs (`clang++`, `g++`, `gcc`, `c++`,
   `cc`, `xcrun`, `make`, `ld`, `ar`, `objdump`…), `perl`, `jq`, `tee`,
   `basename`/`dirname`/`realpath`, `seq`, `expr`, `strings`, `xxd`, `shasum`,
   `tar`/`zip`/`unzip`, lectures hors projet en lecture seule
   (`ls /Library/*`, `Read(//usr/include/**)`…), exécution des binaires compilés
   dans le scratchpad (`Bash(/private/tmp/claude-501/*)`), et quelques préfixes
   d'assignation (`SP=`, `R=`, `U=`, `SDKROOT=`, `CXX=`…).
   `rm` est passé de `ask` à `allow` : `rm -f .pio/build/x.o` ne touche que le
   projet. Les `deny` catastrophiques sont conservés et étendus
   (`/Library`, `/System`, `/Applications`), et des `ask` ciblés reprennent la
   main sur les cibles hors projet (`rm -rf ../*`, `/usr/*`, `/opt/*`).
   Sauvegardes : `.claude/settings.local.json.bak2`, `~/.claude/settings.json.bak2`.

2. **⭐ MON style de commandes — la vraie cause.** Une commande qu'aucune règle
   ne peut décrire ne peut pas être autorisée d'avance, quelle que soit la taille
   de l'allowlist. Sur les 17 exemples fournis le 27/08, la moitié était de ce
   type : heredocs `cat > f <<'EOF'`, définition de fonction shell `run() { … }`,
   assignations en préfixe `SP=… R=… cmd`, et **un `cd <scratchpad> && …`** —
   exactement ce qui avait été identifié comme cause n°1 la veille.

**How to apply:**

- **Créer un fichier avec l'outil Write, jamais avec un heredoc.** Vaut pour les
  scripts jetables (`.py`, `.mjs`, `.cpp`, mutations de test) comme pour les
  messages de commit (`git commit -F <fichier du scratchpad>`).
- **Jamais de `cd <chemin> && …`** : le répertoire de travail est déjà le projet
  et persiste entre les appels. Utiliser des chemins absolus ou relatifs.
- **Jamais de fonction shell** ni d'assignation de variable en préfixe : mettre
  la logique dans un script créé avec Write, puis le lancer en une ligne.
- Éviter `$(...)` et les longues chaînes `;`/`|`/`||` : chaque sous-commande est
  évaluée séparément, une seule non couverte suffit à déclencher une demande.

**Règle générale : tout ce qui dépasse une commande simple appartient à un
script.** Écrit avec Write, lancé en une ligne — c'est couvert par
`Bash(python3 *)` / `Bash(node *)` / `Bash(bash *)` et le prompt disparaît.

Voir aussi [[parite-data-websocket-recidive]] pour l'autre motif récurrent de ce
projet.
