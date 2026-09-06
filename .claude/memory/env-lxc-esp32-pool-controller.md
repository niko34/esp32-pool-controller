---
name: env-lxc-esp32-pool-controller
description: Contraintes de l'environnement LXC Debian 13 où tourne désormais esp32-pool-controller (pas d'USB, PDF via wrapper Chromium)
metadata:
  type: project
---

Depuis le 2026-09-05, `esp32-pool-controller` est développé dans un conteneur LXC
Debian 13 (`/root/esp32-pool-controller`), plus sur le Mac de l'utilisateur.

Conséquences non déductibles du dépôt :
- **Aucun port série** (`/dev/ttyUSB*` absent) → `deploy.sh` USB inutilisable ici ;
  seul `./ota_update.sh` reste possible, et `poolcontroller.local` ne résout pas
  dans le conteneur (pas de mDNS) : il faut passer l'IP du contrôleur.
- **Chromium refuse de tourner en root sans `--no-sandbox`** → wrapper
  `/usr/local/bin/chromium-pdf` créé, et `export CHROME=/usr/local/bin/chromium-pdf`
  ajouté à `~/.bashrc`. Sans lui, `scripts/build-docs.sh` sort en erreur après le
  premier guide, sans message.
- **`pio pkg install` réécrit `platformio.ini` en supprimant tous ses commentaires**
  (des centaines de lignes de justification y sont documentées). Toujours vérifier
  `git diff platformio.ini` après une commande `pio pkg`.
- `gh` est authentifié (compte niko34, protocole ssh, token OAuth `gho_`). Le
  scope `workflow` n'y est PAS : `gh auth refresh -s workflow` si un jour il faut
  modifier `.github/workflows/` via l'API.
- Au `gh auth login`, l'upload de la clé publique échoue en « key already in use » :
  c'est normal (la clé est déjà sur le compte) et **non bloquant** — le token est
  déjà stocké quand l'erreur survient. Répondre « Skip » à cette étape.
- Chaîne PCB complète : `kicad` 9.0.2 (Debian, installé SANS les recommends —
  `kicad-libraries` inutile, les empreintes sont embarquées dans le `.kicad_pcb`)
  + `zip`/`unzip` requis par `scripts/build-gerbers.sh` et `/release-pcb`.
  La version de KiCad doit rester alignée sur le `generator_version` des sources
  (9.0) : une version plus récente migrerait les fichiers versionnés.

