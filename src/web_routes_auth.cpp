#include "web_routes_auth.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "logger.h"
#include "constants.h"            // bug-185 : kAdminPasswordMinLength
#include "auth_bootstrap_logic.h"  // bug-185 : décisions pures
#include "ws_manager.h"  // bug-081 : révocation WS à la rotation de token
#include "auth_log_throttle_site.h"  // bug-183 : journal BORNÉ des échecs
#include <ArduinoJson.h>
#include <WiFi.h>
#include <atomic>

// ---------------------------------------------------------------------------
// bug-183 : journal des échecs d'authentification des routes /auth/*
// ---------------------------------------------------------------------------
// ⛔ UN SEUL site borné pour les échecs des DEUX routes : la route est nommée dans
// le TEXTE de la ligne, pas dans la clé du site. Deux sites doubleraient le budget
// de journal de qui choisit la route (ou la nature) de ses essais. Clé fixe : le
// hachage ne distingue donc aucune route.
// Statiques PRIVÉES à l'unité ; seul drainAuthRoutesWarnLogs() est exporté.
namespace {
AuthLogThrottleSite gAuthFailSite;
// Échecs depuis la dernière connexion RÉUSSIE. Compte TOUS les échecs, journalisés
// ou occultés par le bornage : c'est ce que dit la ligne de réussite. Saturant.
std::atomic<uint16_t> gFailuresSinceSuccess{0};
}  // namespace

// Journalise (borné) un échec. `what` = « /route : nature », texte FIXE choisi par
// l'appelant. ⛔ Jamais le mot de passe, aucun fragment, aucune longueur, ni le nom
// d'utilisateur essayé (attaquant-contrôlé, de longueur libre : le recopier
// permettrait d'injecter du texte dans le journal). On dit le FAIT, pas la valeur.
// MUET => aucune allocation (ADR-0082) : IP lue brute, sortie AVANT toute String.
static void logAuthFailure(AsyncWebServerRequest* req, const char* what) {
  // Incrément saturant via le helper PUR déjà testé en natif (ADR-0017) plutôt
  // qu'une saturation réécrite à la main : une logique de moins hors couverture.
  uint16_t cur = gFailuresSinceSuccess.load(std::memory_order_relaxed);
  while (cur != UINT16_MAX &&
         !gFailuresSinceSuccess.compare_exchange_weak(cur, authLogThrottleSatInc(cur),
                                                      std::memory_order_relaxed)) {
  }
  const bool hasClient = (req != nullptr && req->client() != nullptr);
  const uint32_t ip = hasClient ? static_cast<uint32_t>(req->client()->remoteIP()) : 0;
  const AuthLogSiteReport r = authLogSiteAttempt(gAuthFailSite, millis(), authLogThrottleHash("/auth"), ip,
                                                 kUnauthWarnThrottleMs, kUnauthWarnMaxShift, kUnauthWarnDecayMs);
  if (!r.emit) return;  // MUET : zéro allocation
  String msg = String("[Auth] ") + what + " depuis " +
               (hasClient ? req->client()->remoteIP().toString() : String("IP inconnue"));
  if (r.occulted > 0) {
    // ⚠️ Les occultés ne portent ni la nature ni la route annoncées ci-dessus, et
    // ⛔ PAS FORCÉMENT L'IP NOMMÉE. Sans le dire, on lit « et 12 autres essais DE
    // CETTE ADRESSE » alors qu'ils peuvent venir de douze adresses : c'est très
    // exactement la ligne sur laquelle on se trompe en cherchant qui a essayé
    // d'entrer. (Relevé par security-reviewer en passe P2 ; même défaut que celui
    // que auth.cpp/appendOcculted évitait déjà, lui, en affichant r.otherIps.)
    // ℹ️ r.otherRoutes n'est PAS affiché : la clé du site est fixe (« /auth »),
    //    donc il est toujours faux ici — l'afficher serait du code mort.
    msg += String(" ; et ") + String(r.occulted) + " " +
           authLogThrottlePlural(r.occulted, "autre échec non journalisé", "autres échecs non journalisés") +
           (r.otherIps ? " (route, nature et IP non précisées)" : " (route et nature non précisées)");
  }
  systemLogger.warning(msg);
}

// Connexion RÉUSSIE : INFO, avec le nombre d'échecs depuis la précédente. Sans elle
// le journal dit qu'on a essayé, jamais qu'on a réussi. Atteignable seulement avec
// le bon mot de passe, et sous checkRateLimit() : pas un émetteur anonyme.
static void logLoginSuccess(AsyncWebServerRequest* req) {
  const uint16_t fails = gFailuresSinceSuccess.exchange(0, std::memory_order_relaxed);
  String msg = String("[Auth] Connexion réussie depuis ") +
               ((req != nullptr && req->client() != nullptr) ? req->client()->remoteIP().toString()
                                                              : String("IP inconnue"));
  if (fails > 0) {
    // ⚠️ Le compte agrège les DEUX routes /auth/* (échecs d'identifiants de /auth/login
    // ET essais de jeton de /auth/change-password) : le libellé doit le dire, sinon on
    // lit « N mots de passe faux ». ⛔ Il est en RAM : un redémarrage le remet à zéro.
    msg += String(" (") + (fails == UINT16_MAX ? String("65535 ou plus") : String(fails)) +
           " " + authLogThrottlePlural(fails, "échec", "échecs") +
           " d'authentification sur /auth/* depuis le démarrage ou la dernière connexion réussie)";
  }
  // AUTH-LOG-EXEMPT(bug-183) : NON borné, délibérément. Atteignable seulement après
  // decideLogin() == kAllow — donc par qui détient déjà le bon mot de passe — et
  // derrière checkRateLimit() (120 req/min/IP) : ce n'est pas un émetteur anonyme,
  // et la garde check_unauth_log_guard.sh ne borne que le chemin NON authentifié.
  // ⚠️ Résidu assumé : qui détient le mot de passe peut noyer ses traces en ~27
  // connexions réussies (journal de 6 144 o conservés). Défaut déjà porté par
  // bug-112 et renvoyé à feature-172 ; ce lot y ajoute une voie, il ne la crée pas.
  systemLogger.info(msg);
}

// Drain (loop UNIQUEMENT), borné par le même recul que l'émission.
void drainAuthRoutesWarnLogs() {
  const AuthLogSiteReport r = authLogSiteDrain(gAuthFailSite, millis(), kUnauthWarnThrottleMs,
                                               kUnauthWarnMaxShift, kUnauthWarnDrainQuietMs);
  if (!r.emit) return;
  systemLogger.warning(String("[Auth] ") + String(r.occulted) + " " +
                       authLogThrottlePlural(r.occulted, "échec d'authentification non journalisé",
                                             "échecs d'authentification non journalisés") +
                       " (route et nature non précisées)");
}

void setupAuthRoutes(AsyncWebServer* server) {
  // Route: Statut d'authentification (PUBLIC - vérifier si premier démarrage)
  //
  // ⛔ CONTRAT DE SONDABILITÉ (bug-177) — NE PAS DURCIR CETTE ROUTE SANS LIRE CECI.
  // La page s'en sert pour savoir si l'appareil est REVENU après un redémarrage
  // OTA. Quatre propriétés la rendent utilisable pour ça, et chacune est
  // nécessaire :
  //   1. PUBLIQUE — ni REQUIRE_AUTH ni checkAuth : elle doit répondre alors que
  //      la page n'a pas encore pu se ré-authentifier ;
  //   2. HORS QUOTA — ⛔ ne PAS y ajouter checkRateLimit() : le sondage (une
  //      tentative toutes les 2 s pendant 90 s au plus) ne doit consommer aucun
  //      des kMaxRequestsPerMinute, sinon il s'auto-refuse ;
  //   3. SANS SYSTÈME DE FICHIERS — elle ne sert que trois booléens, donc elle
  //      répond même si LittleFS n'a pas pu être remonté après un envoi FS ;
  //   4. ENREGISTRÉE EN PREMIER (web_server.cpp) — donc hors de la garde 503 de
  //      famine mémoire : un appareil sous pression doit quand même dire « je suis là ».
  // ⚠️ Ces quatre propriétés étaient des ACCIDENTS jusqu'au 2026-11-15 : rien ne les
  // déclarait. Les casser ne produirait AUCUNE erreur — seulement une page qui
  // n'arrive plus à constater le retour de l'appareil, en silence. C'est la famille
  // de défauts qui a déjà coûté six semaines à ce projet (bug-169).
  // Contrat écrit aussi dans docs/API.md.
  server->on("/auth/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["firstBoot"] = authManager.isFirstBootDetected();
    // bug-185 — DEUX NOTIONS DISTINCTES, et leur confusion était le défaut :
    //   · firstBoot : « l'assistant doit encore être montré » ;
    //   · virgin    : « AUCUN mot de passe n'a jamais été choisi ».
    // ⛔ Seul `virgin` autorise le formulaire ANONYME de choix de mot de passe.
    // Sans cette clé, un appareil dont l'assistant a été interrompu APRÈS le
    // choix du mot de passe voyait les pages lui proposer un formulaire que
    // l'appareil REFUSE — la page de secours devenait une impasse, et la seule
    // issue était l'appui long, qui efface étalonnages, consignes et limites.
    // ⚠️ Ajout ADDITIF : aucune clé existante ne change, le contrat de
    // sondabilité de bug-177 (toujours répondre, sans auth ni quota) est intact.
    // A5 : l'état vierge est PUBLIC PAR CONSTRUCTION — le droit anonyme de
    // /auth/change-password le révèle à quiconque l'essaie, indépendamment de cette clé.
    doc["virgin"] = authManager.isFactoryPassword();
    doc["authEnabled"] = authManager.isEnabled();
    doc["forceWifiConfig"] = authCfg.forceWifiConfig;
    sendJsonResponse(req, doc);
  });

  // Route: Login (PUBLIC - génère un token de session)
  server->on("/auth/login", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String username = doc["username"] | "";
      String password = doc["password"] | "";

      // bug-185 : la décision est dans le module pur. Le mot de passe d'usine est
      // refusé (403) QUEL QUE SOIT isFirstBoot : sinon un tiers qui referme la
      // fenêtre rendrait admin/admin valide pour toujours.
      // ⛔ `isFirstBoot` n'est PAS passé, délibérément : un assistant interrompu
      // APRÈS le choix du mot de passe laisse isFirstBoot vrai sur un appareil
      // déjà possédé — le tester ici enfermerait son propriétaire dehors, et
      // l'OTA est le SEUL canal de déploiement. Voir auth_bootstrap_logic.h.
      // bug-183 : `admin` est PUBLIC (== reste), le mot de passe se compare à temps fixe.
      const bool userMatches = (username == "admin");
      const bool pwMatches = authManager.passwordMatches(password);
      switch (decideLogin(userMatches, pwMatches, authManager.isFactoryPassword())) {
        case LoginVerdict::kAllow:
          break;
        case LoginVerdict::kDenyMustChangePassword:
          // A5 : 403 NON journalisé (bruit). Il est conservé, mais ce n'est pas la
          // publication de `virgin` qui le justifie : l'état vierge est public PAR
          // CONSTRUCTION (le droit anonyme de /auth/change-password le révèle).
          sendErrorResponse(req, 403, "Changement de mot de passe obligatoire au premier démarrage");
          return;
        case LoginVerdict::kDenyCredentials:
        default:
          // Le classifieur ne sert QU'au texte du journal : la réponse reste unique.
          switch (classifyLoginFailure(userMatches, pwMatches)) {
            case LoginLogKind::kUnknownUser:
              logAuthFailure(req, "/auth/login : nom d'utilisateur inconnu");
              break;
            case LoginLogKind::kBadPassword:
              logAuthFailure(req, "/auth/login : mot de passe incorrect");
              break;
            case LoginLogKind::kNone:
            default:
              break;
          }
          sendErrorResponse(req, 401, "Nom d'utilisateur ou mot de passe invalide");
          return;
      }

      // Authentification réussie - retourner le token API
      logLoginSuccess(req);
      JsonDocument response;
      response["success"] = true;
      response["token"] = authManager.getApiToken();
      response["username"] = "admin";
      sendJsonResponse(req, response);
    }
  );

  // Route: Changement de mot de passe (PUBLIC pour premier démarrage, sinon AUTH)
  server->on("/auth/change-password", HTTP_POST, [](AsyncWebServerRequest *req) {}, nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!authManager.checkRateLimit(req)) {
        authManager.sendRateLimitExceeded(req);
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        sendErrorResponse(req, 400, "Invalid JSON");
        return;
      }

      String currentPassword = doc["currentPassword"] | "";
      String newPassword = doc["newPassword"] | "";

      // bug-185 : décision dans le module pur (auth_bootstrap_logic.h). ⛔ Ordre
      // imposé là-bas : authentification AVANT longueur (401, pas 400, sinon fuite
      // de l'existence d'un compte). Droit anonyme = appareil vierge seulement.
      const bool isFirstBoot = authManager.isFirstBootDetected();
      // ⛔ LE DROIT ANONYME SUIT LE MOT DE PASSE, PAS L'ASSISTANT. Un appareil dont
      // l'assistant a été interrompu après le choix du mot de passe garde
      // isFirstBoot vrai alors qu'il est DÉJÀ possédé : lui accorder le droit
      // anonyme laisserait un inconnu écraser le mot de passe du propriétaire.
      const bool virginState = authManager.isFactoryPassword();
      PwdChangeInput in;
      in.virginState = virginState;
      in.hasCurrentPassword = !currentPassword.isEmpty();
      in.currentMatches = authManager.passwordMatches(currentPassword);  // bug-183 : temps fixe
      in.currentIsFactoryLiteral = (currentPassword == "admin");  // valeur PUBLIQUE : == reste
      // checkTokenAuth : ne répond pas lui-même (le 401 est émis ici, une seule fois).
      in.tokenValid = (!virginState && !in.hasCurrentPassword) ? authManager.checkTokenAuth(req) : false;
      in.newLenOk = (newPassword.length() >= kAdminPasswordMinLength);  // constante partagée avec /save-config (bug-185)
      in.newEqualsCurrent = in.hasCurrentPassword && (newPassword == currentPassword);

      switch (decidePasswordChange(in)) {
        case PwdChangeVerdict::kAllow:
          break;
        case PwdChangeVerdict::kDenyUnauth:
          // Essai de jeton, jusqu'ici invisible : même site borné que le mot de passe.
          logAuthFailure(req, "/auth/change-password : jeton absent ou invalide");
          sendErrorResponse(req, 401, "Authentication required");
          return;
        case PwdChangeVerdict::kDenyBadCurrent:
          logAuthFailure(req, "/auth/change-password : mot de passe actuel incorrect");
          sendErrorResponse(req, 401, "Current password incorrect");
          return;
        case PwdChangeVerdict::kDenyTooShort:
          sendErrorResponse(req, 400, "New password must be at least 8 characters");
          return;
        case PwdChangeVerdict::kDenySame:
        default:
          sendErrorResponse(req, 400, "New password must be different from current password");
          return;
      }

      const AuthEffectPlan plan = planAfterPasswordChange(isFirstBoot);

      // ⛔ ORDRE D'EFFETS FIGÉ (bug-185) — non testable en natif (NVS/HTTP),
      // protégé par ce commentaire : 1 mot de passe, 2 forceWifiConfig, 3 jeton
      // (RAM ET authCfg ensemble), 4 UNE SEULE écriture NVS, 5 révocation WS,
      // 6 réponse avec le NOUVEAU jeton.
      // 1.
      authCfg.adminPassword = newPassword;
      authManager.setPassword(newPassword);

      // 2. Si c'était un reset password, désactiver le flag forceWifiConfig
      if (authCfg.forceWifiConfig) {
        authCfg.forceWifiConfig = false;
      }

      // 3. ⛔ LES DEUX LIGNES ENSEMBLE : tourner le jeton en RAM sans le recopier
      // dans authCfg ferait RESSUSCITER l'ancien jeton de la NVS au redémarrage.
      if (plan.rotateToken) {
        authManager.regenerateApiToken();
        authCfg.apiToken = authManager.getApiToken();
      }

      // 4. UNE SEULE écriture : deux saveMqttConfig() ouvriraient une fenêtre de
      // redémarrage à l'état mi-ancien mi-neuf (drapeau persisté sans mot de passe
      // = appareil enfermé dehors). clearFirstBootFlag() sauvegarde déjà.
      if (plan.closeFirstBootWindow) {
        authManager.clearFirstBootFlag();
      } else {
        saveMqttConfig();
      }

      // 5. Les clients WS authentifiés avec l'ancien jeton sont révoqués.
      if (plan.revokeWebsockets) {
        wsManager.revokeAllAuthenticated();
      }

      // 6. Réponse : le NOUVEAU jeton (les clients stockent response.token ; la
      // page de secours OTA échoue sans jeton).
      JsonDocument response;
      response["success"] = true;
      // ⛔ `plan.returnRotatedToken` est CONSULTÉ, pas décoratif : un champ de plan
      // que la production n'applique pas ferait affirmer à ses tests une propriété
      // fausse (relevé par code-reviewer). Il vaut toujours vrai aujourd'hui — et
      // c'est le test de mutation qui le prouve, pas cette ligne.
      if (plan.returnRotatedToken) {
        response["token"] = authManager.getApiToken();
      }
      response["message"] = "Password changed successfully";
      sendJsonResponse(req, response);
    }
  );

  // Route: Régénération du token API (PROTÉGÉE - nécessite auth)
  server->on("/auth/regenerate-token", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    // Régénérer le token
    authManager.regenerateApiToken();
    authCfg.apiToken = authManager.getApiToken();
    saveMqttConfig();

    // bug-081, condition bloquante #3 (security-reviewer) : une rotation de
    // jeton ne révoquait rien côté WS — un client authentifié avec l'ANCIEN
    // token continuait de recevoir les pushes indéfiniment (keepalive). Les
    // clients WS déjà authentifiés doivent ré-authentifier avec le nouveau
    // token.
    wsManager.revokeAllAuthenticated();

    JsonDocument doc;
    doc["success"] = true;
    doc["token"] = authManager.getApiToken();
    doc["message"] = "API token regenerated";
    sendJsonResponse(req, doc);
  });

  // Route: Obtenir le token API actuel (PROTÉGÉE - nécessite auth)
  server->on("/auth/token", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    JsonDocument doc;
    doc["token"] = authManager.getApiToken();
    sendJsonResponse(req, doc);
  });

  // Route: Mot de passe AP WiFi (PROTÉGÉE - nécessite auth admin)
  server->on("/auth/ap-password", HTTP_GET, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::CRITICAL);

    JsonDocument doc;
    doc["ap_password"] = authCfg.apPassword;
    doc["ap_ssid"] = "PoolControllerAP";
    sendJsonResponse(req, doc);
  });

  // Route: Marquer le wizard comme complété (désactive le flag firstBoot)
  // bug-185 : n'est plus PUBLIQUE. Filet idempotent : exige le jeton (donc un mot
  // de passe déjà choisi, la clôture normale passant par /auth/change-password) et
  // refuse (409) si le mot de passe est encore celui d'usine.
  server->on("/auth/complete-wizard", HTTP_POST, [](AsyncWebServerRequest *req) {
    REQUIRE_AUTH(req, RouteProtection::WRITE);
    // ⛔ `tokenValid` est codé à `true` À DESSEIN : le REQUIRE_AUTH ci-dessus a DÉJÀ
    // répondu 401 et rendu la main si le jeton manquait. Rappeler checkAuth ici
    // consommerait un SECOND quota et enverrait une SECONDE réponse sur la même
    // requête. ⚠️ Conséquence assumée : la branche kDenyUnauth du module pur est
    // donc inatteignable depuis cette route — ses tests protègent le module, pas
    // ce site. La vraie garde est la ligne au-dessus.
    const WizardCloseVerdict v = decideWizardClose(authManager.isFirstBootDetected(), true,
                                                   authManager.isFactoryPassword());
    if (v == WizardCloseVerdict::kDenyFactoryPassword) {
      sendErrorResponse(req, 409, "Choisissez d'abord un mot de passe");
      return;
    }
    if (v == WizardCloseVerdict::kClose) {
      authManager.clearFirstBootFlag();
      // AUTH-LOG-EXEMPT(borné par nature : n'écrit qu'une fois, gardé par isFirstBootDetected() que clearFirstBootFlag() rend faux)
      systemLogger.info("Premier démarrage finalisé - Configuration wizard complétée");
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Wizard completed successfully";
    sendJsonResponse(req, doc);
  });
}
