// =============================================================================
// Tests natifs — assainissement des textes EXTERNES avant journal (bug-179)
// =============================================================================
// Cible : src/log_text_sanitize_logic.h (module pur, ADR-0017).
// On vérifie le COMPORTEMENT : ce qui sort est inoffensif, injectif (la charge se
// relit), et les routes légitimes restent lisibles octet pour octet.
#include <unity.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <set>
#include "constants.h"
#include "log_text_sanitize_logic.h"

void setUp(void) {}
void tearDown(void) {}

static std::string san(const std::string& s, size_t cap = kLogExternalTextBufBytes) {
  std::vector<char> buf(cap ? cap : 1, 'Z');
  size_t n = logSanitizeExternalText(s.data(), s.size(), buf.data(), cap);
  if (cap == 0) return std::string();
  TEST_ASSERT_EQUAL_UINT(strlen(buf.data()), n);
  return std::string(buf.data(), n);
}

// Décodeur d'URL de référence, indépendant du code livré.
static std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

void test_pourcent_est_echappe(void) {
  TEST_ASSERT_EQUAL_STRING("100%25", san("100%").c_str());
  TEST_ASSERT_EQUAL_STRING("%2525", san("%25").c_str());
}

void test_injectivite_aller_retour_de_tous_les_octets(void) {
  // Chaque octet 1..255 seul, puis la chaîne de tous : décodage == entrée.
  std::string all;
  for (int c = 1; c < 256; c++) all.push_back(static_cast<char>(c));
  std::string big = san(all, 2000);
  TEST_ASSERT_EQUAL_STRING(all.c_str(), urlDecode(big).c_str());
  // Deux entrées distinctes ne donnent jamais la même sortie (pairs piégeuses).
  std::set<std::string> seen;
  std::vector<std::string> in = {"%41", "A", "%2541", "%", "%25", "a%", "%%", "<", "%3C", "%253C"};
  for (auto& s : in) {
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(san(s)).second, s.c_str());
    TEST_ASSERT_EQUAL_STRING(s.c_str(), urlDecode(san(s)).c_str());
  }
}

void test_octets_de_controle_del_et_haut_sont_echappes(void) {
  for (int c = 1; c < 0x20; c++) {
    std::string o = san(std::string(1, static_cast<char>(c)));
    TEST_ASSERT_EQUAL_UINT(3, o.size());
    TEST_ASSERT_EQUAL_CHAR('%', o[0]);
  }
  TEST_ASSERT_EQUAL_STRING("%7F", san("\x7f").c_str());
  TEST_ASSERT_EQUAL_STRING("%0A", san("\n").c_str());
  TEST_ASSERT_EQUAL_STRING("%0D", san("\r").c_str());
  for (int c = 0x80; c < 256; c++) {
    std::string o = san(std::string(1, static_cast<char>(c)));
    TEST_ASSERT_EQUAL_UINT(3, o.size());
  }
  TEST_ASSERT_EQUAL_STRING("100%25%C3%A9", san("100%\xC3\xA9").c_str());
}

void test_aucun_caractere_dangereux_ne_survit(void) {
  std::string all;
  for (int c = 1; c < 256; c++) all.push_back(static_cast<char>(c));
  std::string o = san(all, 2000);
  for (char ch : std::string("<>&\"'\\`{}|^[]#$!")) {
    TEST_ASSERT_TRUE_MESSAGE(o.find(ch) == std::string::npos, "caractere dangereux en clair");
  }
  for (unsigned char ch : o) TEST_ASSERT_TRUE(ch >= 0x20 && ch < 0x7F);
  TEST_ASSERT_EQUAL_STRING("%3Cscript%3Ealert(1)%3C/script%3E", san("<script>alert(1)</script>").c_str());
}

void test_vecteur_balise_bug179_slash_reste_en_clair(void) {
  // '/' est en liste blanche : </b> -> %3C/b%3E (et NON %3C%2Fb%3E).
  TEST_ASSERT_EQUAL_STRING("/get-config/%3Cb%3EBUG179%3C/b%3E", san("/get-config/<b>BUG179</b>").c_str());
  TEST_ASSERT_EQUAL_STRING("%3C/b%3E", san("</b>").c_str());
}

void test_routes_reelles_inchangees_octet_pour_octet(void) {
  const char* routes[] = {
    "/auth", "/auth/ap-password", "/auth/change-password", "/auth/complete-wizard", "/auth/login",
    "/auth/regenerate-token", "/auth/status", "/auth/token", "/boost/start", "/boost/stop",
    "/calibrate_clear", "/calibrate_orp", "/calibrate_ph", "/calibration/enter", "/calibration/exit",
    "/check-update", "/config", "/config.html", "/coredump", "/coredump/download", "/coredump/info",
    "/data", "/debug/heap_census", "/debug/heap_peek", "/debug/ph_slope_refresh",
    "/debug/sensor_filter_reset", "/debug/sensor_filter_state", "/debug/sensor_trace",
    "/debug/sensor_trace_clear", "/download-logs", "/download-update", "/factory-reset",
    "/filtration/external-state", "/flow-assist/exit", "/flow-assist/start", "/flow-assist/stop",
    "/get-config", "/get-history", "/get-kpi", "/get-logs", "/get-system-info", "/history/clear",
    "/history/import", "/index.html", "/lighting/off", "/lighting/on", "/login.html", "/logs",
    "/orp/inject/start", "/orp/inject/stop", "/ota-sig", "/ph/inject/start", "/ph/inject/stop",
    "/pump1/off", "/pump1/on", "/pump2/off", "/pump2/on", "/reboot", "/reboot-ap", "/recovery",
    "/rescue/start", "/rescue/stop", "/reset-regulation-defaults", "/reset-sensor-filter",
    "/save-config", "/sensors/onewire/identify", "/sensors/onewire/reset", "/sensors/onewire/scan",
    "/system.log", "/time-now", "/tune-ph-offset", "/ui", "/update", "/wifi.html", "/wifi/ap/disable",
    "/wifi/connect", "/wifi/disconnect", "/wifi/scan", "/wifi/status", "/wizard.html",
    "/get-logs?since=12345", "/data?hf=1", "/get-system-info?hf=1",
    "/sensors/onewire/identify?addr=28:FF:A1:B2:C3:D4:E5:F6", "/auth/login", "MonSSID_Piscine-2.4G",
    "firmware-2.97.0.bin", "littlefs.bin", "pool/ph/state", "Livebox-A1B2 (5G)"};
  for (const char* r : routes) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r, san(r).c_str(), r);
  }
}

void test_esperluette_est_echappee_comportement_voulu(void) {
  // '&' n'est PAS en liste blanche (req->url() n'en porte pas ; un '&' externe est suspect).
  TEST_ASSERT_EQUAL_STRING("a%26b", san("a&b").c_str());
}

void test_troncature_299_chevrons(void) {
  std::string o = san(std::string(299, '<'));
  TEST_ASSERT_EQUAL_UINT(95, o.size());
  TEST_ASSERT_TRUE(o.size() >= 5 && o.compare(o.size() - 5, 5, "~299o") == 0);
  TEST_ASSERT_TRUE(o.size() <= kLogExternalTextMax);
}

void test_troncature_jamais_au_milieu_d_un_pourcent_xx(void) {
  // Pour toute capacité et toute entrée mêlant octets simples et échappés, le
  // préfixe (avant le suffixe ~No) ne doit contenir aucun '%' orphelin.
  const std::string inputs[] = {std::string(200, '<'), std::string(200, 'a'),
                                "ab<cd>ef<gh>ij<kl>mn<op>qr<st>uv<wx>yz<12>34<56>78<90>" + std::string(60, '%'),
                                std::string("\xC3\xA9") + std::string(150, 'x')};
  for (const auto& in : inputs) {
    for (size_t cap = 3; cap < 130; cap++) {
      std::string o = san(in, cap);
      TEST_ASSERT_TRUE(o.size() <= cap - 1);
      std::string pre = o;
      const std::string sfx = "~" + std::to_string(in.size()) + "o";
      if (o.size() >= sfx.size() && o.compare(o.size() - sfx.size(), sfx.size(), sfx) == 0) pre = o.substr(0, o.size() - sfx.size());
      for (size_t i = 0; i < pre.size(); i++) {
        if (pre[i] == '%') {
          TEST_ASSERT_TRUE_MESSAGE(i + 2 < pre.size(), "% coupe");
          TEST_ASSERT_TRUE(isxdigit(pre[i + 1]) && isxdigit(pre[i + 2]));
          i += 2;
        }
      }
    }
  }
}

void test_suffixe_porte_la_longueur_d_origine(void) {
  std::string o = san(std::string(1234, 'a'));
  TEST_ASSERT_TRUE(o.size() <= kLogExternalTextMax);
  TEST_ASSERT_TRUE(o.compare(o.size() - 6, 6, "~1234o") == 0);
  // Entrée qui tient pile : pas de suffixe.
  std::string exact(kLogExternalTextMax, 'a');
  TEST_ASSERT_EQUAL_STRING(exact.c_str(), san(exact).c_str());
  // Un octet de plus : suffixe.
  std::string over(kLogExternalTextMax + 1, 'a');
  std::string oo = san(over);
  TEST_ASSERT_TRUE(oo.find("~97o") != std::string::npos);
}

void test_capacites_limites(void) {
  char buf[8];
  memset(buf, 'Z', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText("abc", 3, buf, 0));
  TEST_ASSERT_EQUAL_CHAR('Z', buf[0]);  // outCap 0 : rien n'est écrit
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText("abc", 3, buf, 1));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
  memset(buf, 'Z', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(1, logSanitizeExternalText("abc", 3, buf, 2));
  TEST_ASSERT_EQUAL_STRING("a", buf);
  memset(buf, 'Z', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText("<<<", 3, buf, 2));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText("abc", 3, nullptr, 10));
}

void test_src_nul_et_longueur_zero(void) {
  char buf[8];
  memset(buf, 'Z', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText(nullptr, 5, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
  memset(buf, 'Z', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(0, logSanitizeExternalText("abc", 0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_octet_nul_dans_l_entree_est_echappe(void) {
  const char raw[] = {'a', '\0', 'b'};
  char buf[16];
  size_t n = logSanitizeExternalText(raw, 3, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(5, n);
  TEST_ASSERT_EQUAL_STRING("a%00b", buf);
}

void test_jamais_de_debordement_tampon(void) {
  for (size_t cap = 1; cap < 40; cap++) {
    std::vector<char> b(cap + 8, 'Q');
    logSanitizeExternalText("<a href=x>&\"'</a>%%%", 20, b.data(), cap);
    for (size_t k = 0; k < 8; k++) TEST_ASSERT_EQUAL_CHAR('Q', b[cap + k]);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pourcent_est_echappe);
  RUN_TEST(test_injectivite_aller_retour_de_tous_les_octets);
  RUN_TEST(test_octets_de_controle_del_et_haut_sont_echappes);
  RUN_TEST(test_aucun_caractere_dangereux_ne_survit);
  RUN_TEST(test_vecteur_balise_bug179_slash_reste_en_clair);
  RUN_TEST(test_routes_reelles_inchangees_octet_pour_octet);
  RUN_TEST(test_esperluette_est_echappee_comportement_voulu);
  RUN_TEST(test_troncature_299_chevrons);
  RUN_TEST(test_troncature_jamais_au_milieu_d_un_pourcent_xx);
  RUN_TEST(test_suffixe_porte_la_longueur_d_origine);
  RUN_TEST(test_capacites_limites);
  RUN_TEST(test_src_nul_et_longueur_zero);
  RUN_TEST(test_octet_nul_dans_l_entree_est_echappe);
  RUN_TEST(test_jamais_de_debordement_tampon);
  return UNITY_END();
}
