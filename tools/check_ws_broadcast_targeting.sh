#!/usr/bin/env bash
# =============================================================================
# check_ws_broadcast_targeting.sh — bug-081 : plus jamais de diffusion aveugle
# =============================================================================
# bug-081 : les trois `_ws->textAll(...)` de src/ws_manager.cpp diffusaient à
# TOUS les clients connectés à /ws, authentifiés ou non — la garde de
# WsManager::update() ne vérifiait qu'« au moins un client authentifié », pas
# « ce client-ci est authentifié ». Corrigé par WsManager::_sendToAuthenticated,
# qui cible explicitement `_authRegistry` (ws_client_registry_logic.h) via
# `AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)`.
#
# Ce contrôle vérifie qu'aucun appel de la famille `*All(` du fork
# ESPAsyncWebServer — `textAll`, `binaryAll`, `printfAll` — ne réapparaît dans
# src/. Toute diffusion WS doit repasser par une cible explicite (liste d'ids),
# jamais par « tous les clients de la socket ».
#
# Usage : ./tools/check_ws_broadcast_targeting.sh
# Sortie : 0 si conforme, 1 sinon (liste des sites fautifs sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

# Motif : `.textAll(` / `->textAll(` et pareil pour binaryAll/printfAll, espaces
# tolérés autour du `->`/`.`. Lignes de commentaire (`//` ou `*` en tête,
# espaces tolérés) exclues : ce script interdit l'APPEL, pas le mot dans une
# explication historique (ce fichier même en contient plusieurs).
#
# ⚠️ Revue bug-081 (P2) : le motif `(->|\.)` seul se laissait contourner par un
# appel QUALIFIÉ `AsyncWebSocket::textAll(...)`, qui n'a ni `->` ni `.`. Le
# second motif ci-dessous ferme cette porte. `pingAll` reste volontairement hors
# liste : c'est une trame de contrôle SANS donnée applicative, aucune fuite.
found=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
        | xargs -0 grep -HnE '((->|\.)[[:space:]]*|[A-Za-z_][A-Za-z0-9_]*[[:space:]]*::[[:space:]]*)(textAll|binaryAll|printfAll)[[:space:]]*\(' \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)' \
        || true)

if [ -n "$found" ]; then
  {
    echo "ERREUR bug-081 : diffusion WebSocket non ciblée détectée."
    echo "textAll()/binaryAll()/printfAll() diffusent à TOUS les clients de la"
    echo "socket, authentifiés ou non. Utiliser WsManager::_sendToAuthenticated"
    echo "(src/ws_manager.cpp) — snapshot de _authRegistry puis"
    echo "AsyncWebSocket::text(uint32_t id, AsyncWebSocketSharedBuffer)."
    echo
    echo "$found"
  } >&2
  exit 1
fi

echo "OK — aucune diffusion WS non ciblée (textAll/binaryAll/printfAll) dans src/."
