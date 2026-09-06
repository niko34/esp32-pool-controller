---
name: audit-securite-differe-fable-5-1
description: "L'audit de sécurité applicative complet est DIFFÉRÉ en attente de l'accès à Fable 5.1 (découverte de vulnérabilités défensive) — décision utilisateur du 02/09/2026 ; périmètre déjà cadré"
metadata: 
  node_type: memory
  type: project
  originSessionId: 82048c5b-7e8f-4cec-8242-ad9b5ed01f91
  modified: 2026-09-02T19:16:19.638Z
---

Le 02/09/2026, constat partagé : la sécurité applicative n'est traitée que
par diff (charte du code-reviewer : secrets/auth/injection) + règles CLAUDE.md
(secrets, intégrité OTA) — AUCUN audit holistique de la surface d'attaque n'a
jamais été mené, et il n'existe pas d'agent security-reviewer.

**Décision utilisateur : attendre l'accès à Claude Fable 5.1** (annoncé le
01/09/2026 — découverte de vulnérabilités permise pour le défensif, exploits
exclus, ~60 % de faux positifs de garde-fous en moins) avant de lancer l'audit.

**Périmètre déjà cadré pour le jour J :**
1. Créer l'agent `.claude/agents/security-reviewer.md` (modèle de menace :
   attaquant LAN, appareil volé/revendu, chaîne OTA ; invocation obligatoire
   sur auth.*, web_routes_*, OTA, provisioning WiFi) — pendant cyber du
   pool-chemistry.
2. Audit complet : auth (niveaux READ/WRITE/CRITICAL, rate limiting, sessions),
   routes web, OTA (intégrité, auth), provisioning WiFi, identifiants MQTT en
   NVS, UI (XSS — le projet construit déjà en DOM/textContent), et le point
   connu : **serveur HTTP en clair sur le LAN, mot de passe admin non chiffré
   en transit**. Sortie attendue : rapport hiérarchisé (grave / acceptable
   pour un LAN privé / cosmétique) + specs de durcissement.

**Why :** un pilote de pompes à ACIDE au bout de la surface d'attaque — une
compromission a une conséquence chimique, pas seulement informatique.

**How to apply :** quand l'utilisateur signale avoir accès à Fable 5.1 (ou le
demande), relancer ce chantier tel quel ; l'agent d'audit peut épingler le
modèle 5.1 dans son frontmatter. Ne pas relancer la proposition avant.
