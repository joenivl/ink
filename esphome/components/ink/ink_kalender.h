#pragma once
// Ink – gezinskalender voor de Seeed reTerminal E1003.
//
// Alle layout en logica die niet aan ESPHome-componenten hangt, zodat
// ink-kalender.yaml klein blijft en dit bestand ook op de pc te testen en
// te previewen is (zie tools/preview).
//
// Scherm liggend: 1872 x 1404. Twee schermen, te wisselen met de tabs
// bovenin: de week (met notities eronder) en de maand. Onderaan altijd de
// knoppenbalk.

#ifdef INK_HOST
#include "esphome_stub.h"
#else
// Geen "esphome.h": ESPHome zet dit bestand zelf in esphome.h (het is een
// external component), dus alleen wat we echt gebruiken.
#include "esphome/components/display/display.h"
#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/components/it8951/it8951.h"
#include "esphome/core/color.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <ctime>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ink {

using esphome::Color;
using esphome::display::BaseFont;
using esphome::display::Display;
using esphome::display::TextAlign;

// ---------------------------------------------------------------- maten ---

constexpr int B = 1872;  // breedte (liggend)
constexpr int H = 1404;  // hoogte

constexpr int MARGE = 26;
constexpr int BREED = B - 2 * MARGE;

constexpr int KOP_H = 96;

// Tabs "Week" / "Maand" in de kop
constexpr int TAB_Y = 18;
constexpr int TAB_H = 62;
constexpr int TAB_B = 190;
constexpr int TAB_X = 760;

// Weekscherm: week over de volle breedte, notities eronder
constexpr int WEEK_Y = 110;
constexpr int WEEK_KOL = BREED / 7;
constexpr int WEEK_KOP_H = 76;
constexpr int WEEK_EIND = 904;
constexpr int NOTITIE_Y = 918;
constexpr int NOTITIE_EIND = 1240;

// Maandscherm: volledig rooster
constexpr int MAAND_KOL = BREED / 7;
constexpr int GRID_Y = 116;
constexpr int GRID_EIND = 1240;

// Knoppenbalk
constexpr int BALK_Y = 1256;
constexpr int KNOP_Y = BALK_Y + 14;
constexpr int KNOP_H = 120;
constexpr int SPREEK_B = 620;
constexpr int KNOP_TUSSEN = 16;
constexpr int HA_KNOP_B = (BREED - SPREEK_B - 4 * KNOP_TUSSEN) / 4;

// Pop-upvenster, midden op het scherm
constexpr int VENSTER_B = 1120;
constexpr int VENSTER_H = 480;
constexpr int VENSTER_X = (B - VENSTER_B) / 2;
constexpr int VENSTER_Y = 360;
constexpr int VENSTER_KNOP_B = 440;
constexpr int VENSTER_KNOP_H = 104;
constexpr int VENSTER_KNOP_Y = VENSTER_Y + VENSTER_H - 144;

// Groot venster voor het dagoverzicht en de details van een afspraak
constexpr int GROOT_B = 1300;
constexpr int GROOT_H = 1040;
constexpr int GROOT_X = (B - GROOT_B) / 2;
constexpr int GROOT_Y = 130;
constexpr int GROOT_KNOP_Y = GROOT_Y + GROOT_H - 140;
constexpr int DAG_REGEL = 66;  // regels in het dagoverzicht, groot genoeg voor een vinger

// Wat er opnieuw getekend moet worden (bitmasker).
constexpr uint8_t VOL = 1;      // alles, GC16 (mooi grijs, knippert)
constexpr uint8_t BALK = 2;     // alleen knoppenbalk, DU (snel)
constexpr uint8_t VENSTER = 4;  // alleen pop-upvenster, DU (snel)
constexpr uint8_t NOTITIES = 8; // alleen notitievakken, DU (snel)
constexpr uint8_t GEBIED = 16;  // alles, maar alleen binnen ververs_gebied(), GC16 (knippert alleen daar)
constexpr uint8_t TABS = 32;    // alleen de tabs Week/Maand, DU (snel)

const Color ZWART(0, 0, 0);
const Color WIT(255, 255, 255);
const Color GRIJS_LICHT(225, 225, 225);
const Color GRIJS(170, 170, 170);
const Color GRIJS_DONKER(100, 100, 100);

#ifndef INK_HOST
// Toegang tot de framebuffer van de IT8951. Die velden zijn protected; via een
// afgeleide klasse mag je er een member-pointer naar maken (de klasse zelf
// wordt nooit aangemaakt).
struct Framebuffer : esphome::it8951::IT8951Display {
  static uint8_t *buffer(IT8951Display &d) { return d.*(&Framebuffer::buffer_); }
  static uint16_t rijbreedte(IT8951Display &d) { return d.*(&Framebuffer::row_width_); }
  static bool grijs(IT8951Display &d) { return d.*(&Framebuffer::grayscale_); }
  static void naar_scherm(IT8951Display &d, int &x, int &y) { (d.*(&Framebuffer::apply_transform_))(x, y); }
};
#endif

// Gevuld vlak. filled_rectangle tekent pixel voor pixel (met per pixel een
// kleuromrekening en een blik op de klok); bij grote vlakken, zoals het
// witmaken van de notities of een groot venster, kost dat honderden
// milliseconden. Hier gaat het met memset rechtstreeks in de framebuffer.
inline void vlak(Display &it, int x, int y, int w, int h, Color c) {
#ifdef INK_HOST
  it.filled_rectangle(x, y, w, h, c);
#else
  int x0 = std::max(x, 0), y0 = std::max(y, 0);
  int x1 = std::min(x + w, it.get_width()), y1 = std::min(y + h, it.get_height());
  const auto clip = it.get_clipping();
  if (clip.is_set()) {
    x0 = std::max(x0, static_cast<int>(clip.x));
    y0 = std::max(y0, static_cast<int>(clip.y));
    x1 = std::min(x1, static_cast<int>(clip.x2()));
    y1 = std::min(y1, static_cast<int>(clip.y2()));
  }
  if (x1 <= x0 || y1 <= y0)
    return;
  // Het enige scherm is een IT8951 (zie ink-kalender.yaml).
  auto &d = static_cast<esphome::it8951::IT8951Display &>(it);
  if (!Framebuffer::grijs(d) || Framebuffer::buffer(d) == nullptr) {
    it.filled_rectangle(x0, y0, x1 - x0, y1 - y0, c);
    return;
  }
  // Twee hoeken via de driver: die rekent de kleur om en houdt het gebied bij
  // dat naar het scherm moet.
  it.draw_pixel_at(x0, y0, c);
  it.draw_pixel_at(x1 - 1, y1 - 1, c);
  int ax = x0, ay = y0, bx = x1 - 1, by = y1 - 1;
  Framebuffer::naar_scherm(d, ax, ay);
  Framebuffer::naar_scherm(d, bx, by);
  uint8_t *buf = Framebuffer::buffer(d);
  const uint32_t rij = Framebuffer::rijbreedte(d);
  // Twee pixels per byte: even x in de hoge helft, oneven x in de lage.
  const uint8_t b0 = buf[ay * rij + ax / 2];
  const uint8_t grijs = (ax & 1) ? (b0 & 0x0F) : (b0 >> 4);
  const uint8_t vol = static_cast<uint8_t>(grijs << 4 | grijs);
  const int lx = std::min(ax, bx), hx = std::max(ax, bx);  // hx doet mee
  const int ly = std::min(ay, by), hy = std::max(ay, by);
  for (int ny = ly; ny <= hy; ny++) {
    uint8_t *r = buf + ny * rij;
    int van = lx, tot = hx;
    if (van & 1) {
      r[van / 2] = (r[van / 2] & 0xF0) | grijs;
      van++;
    }
    if (tot >= van && !(tot & 1)) {
      r[tot / 2] = (r[tot / 2] & 0x0F) | static_cast<uint8_t>(grijs << 4);
      tot--;
    }
    if (tot > van)
      memset(r + van / 2, vol, (tot - van + 1) / 2);
  }
#endif
}

// ---------------------------------------------------------------- datum ---

struct Datum {
  int j{2000}, m{1}, d{1};
};

inline bool operator==(const Datum &a, const Datum &b) { return a.j == b.j && a.m == b.m && a.d == b.d; }

// Dagen sinds 1970-01-01 (algoritme van Howard Hinnant).
inline int dagnummer(const Datum &dt) {
  const int y = dt.j - (dt.m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (dt.m + (dt.m > 2 ? -3 : 9)) + 2) / 5 + dt.d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

inline Datum van_dagnummer(int z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  const int y = static_cast<int>(yoe) + era * 400 + (m <= 2);
  return {y, m, d};
}

inline Datum plus_dagen(const Datum &dt, int n) { return van_dagnummer(dagnummer(dt) + n); }

// 0 = maandag ... 6 = zondag
inline int weekdag(const Datum &dt) { return ((dagnummer(dt) % 7) + 7 + 3) % 7; }

inline int iso_week(const Datum &dt) {
  const int donderdag = dagnummer(dt) - weekdag(dt) + 3;
  const int jan1 = dagnummer({van_dagnummer(donderdag).j, 1, 1});
  return (donderdag - jan1) / 7 + 1;
}

// "JJJJ-MM-DD" -> Datum
inline bool lees_datum(const std::string &s, Datum &uit) {
  int j, m, d;
  if (s.size() < 10 || std::sscanf(s.c_str(), "%4d-%2d-%2d", &j, &m, &d) != 3)
    return false;
  if (m < 1 || m > 12 || d < 1 || d > 31)
    return false;
  uit = {j, m, d};
  return true;
}

static const char *const DAG_KORT[] = {"Ma", "Di", "Wo", "Do", "Vr", "Za", "Zo"};
static const char *const DAG_LANG[] = {"Maandag", "Dinsdag", "Woensdag", "Donderdag",
                                       "Vrijdag", "Zaterdag", "Zondag"};
static const char *const MAAND[] = {"januari", "februari", "maart",     "april",   "mei",      "juni",
                                    "juli",    "augustus", "september", "oktober", "november", "december"};
static const char *const MAAND_KORT[] = {"jan", "feb", "mrt", "apr", "mei", "jun",
                                         "jul", "aug", "sep", "okt", "nov", "dec"};

// "JJJJ-MM-DD"
inline std::string datum_tekst(const Datum &dt) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.j, dt.m, dt.d);
  return buf;
}

inline std::string datum_lang(const Datum &dt) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%s %d %s", DAG_LANG[weekdag(dt)], dt.d, MAAND[dt.m - 1]);
  return buf;
}

// ---------------------------------------------------------------- iconen ---

// Emoji die in f_normaal zitten (Noto Emoji, via 'extras' in ink-kalender.yaml);
// daarin staan alle titels en notities.
// Moet gelijk zijn aan die lijst; tools/preview/render.py controleert dat.
// Gesorteerd, voor binary_search.
static const uint32_t ICONEN[] = {
    0x23F0, 0x2600, 0x2615, 0x267B, 0x26A0, 0x26BD, 0x26C4, 0x26F3,               // ⏰ ☀ ☕ ♻ ⚠ ⚽ ⛄ ⛳
    0x26FA, 0x2702, 0x2705, 0x2708, 0x270F, 0x2714, 0x2728, 0x2744,               // ⛺ ✂ ✅ ✈ ✏ ✔ ✨ ❄
    0x274C, 0x2753, 0x2757, 0x2764, 0x2B50, 0x1F308, 0x1F30D, 0x1F319,            // ❌ ❓ ❗ ❤ ⭐ 🌈 🌍 🌙
    0x1F31F, 0x1F327, 0x1F331, 0x1F337, 0x1F33B, 0x1F355, 0x1F370, 0x1F374,       // 🌟 🌧 🌱 🌷 🌻 🍕 🍰 🍴
    0x1F377, 0x1F37A, 0x1F37D, 0x1F381, 0x1F382, 0x1F383, 0x1F384, 0x1F385,       // 🍷 🍺 🍽 🎁 🎂 🎃 🎄 🎅
    0x1F388, 0x1F389, 0x1F393, 0x1F39F, 0x1F3A4, 0x1F3A8, 0x1F3AB, 0x1F3AC,       // 🎈 🎉 🎓 🎟 🎤 🎨 🎫 🎬
    0x1F3AD, 0x1F3AE, 0x1F3AF, 0x1F3B5, 0x1F3B6, 0x1F3B8, 0x1F3B9, 0x1F3BE,       // 🎭 🎮 🎯 🎵 🎶 🎸 🎹 🎾
    0x1F3BF, 0x1F3C0, 0x1F3C3, 0x1F3C6, 0x1F3CA, 0x1F3CB, 0x1F3D4, 0x1F3D6,       // 🎿 🏀 🏃 🏆 🏊 🏋 🏔 🏖
    0x1F3E0, 0x1F3E1, 0x1F3E5, 0x1F3EB, 0x1F408, 0x1F415, 0x1F423, 0x1F430,       // 🏠 🏡 🏥 🏫 🐈 🐕 🐣 🐰
    0x1F431, 0x1F434, 0x1F436, 0x1F43E, 0x1F44B, 0x1F44D, 0x1F44F, 0x1F453,       // 🐱 🐴 🐶 🐾 👋 👍 👏 👓
    0x1F466, 0x1F467, 0x1F468, 0x1F469, 0x1F46A, 0x1F474, 0x1F475, 0x1F476,       // 👦 👧 👨 👩 👪 👴 👵 👶
    0x1F47B, 0x1F487, 0x1F489, 0x1F48A, 0x1F48D, 0x1F495, 0x1F4A1, 0x1F4AA,       // 👻 💇 💉 💊 💍 💕 💡 💪
    0x1F4B0, 0x1F4B6, 0x1F4BB, 0x1F4BC, 0x1F4C5, 0x1F4C6, 0x1F4CC, 0x1F4D6,       // 💰 💶 💻 💼 📅 📆 📌 📖
    0x1F4DA, 0x1F4DD, 0x1F4DE, 0x1F4E7, 0x1F4F7, 0x1F4FA, 0x1F514, 0x1F525,       // 📚 📝 📞 📧 📷 📺 🔔 🔥
    0x1F527, 0x1F528, 0x1F56F, 0x1F5D1, 0x1F600, 0x1F60A, 0x1F60D, 0x1F60E,       // 🔧 🔨 🕯 🗑 😀 😊 😍 😎
    0x1F634, 0x1F64F, 0x1F686, 0x1F68C, 0x1F697, 0x1F6A2, 0x1F6B2, 0x1F6B4,       // 😴 🙏 🚆 🚌 🚗 🚢 🚲 🚴
    0x1F6C1, 0x1F6CF, 0x1F6D2, 0x1F912, 0x1F938, 0x1F942, 0x1F95A, 0x1F973,       // 🛁 🛏 🛒 🤒 🤸 🥂 🥚 🥳
    0x1F9B7, 0x1F9D2, 0x1F9D8, 0x1F9E9, 0x1F9F3, 0x1F9F8, 0x1F9F9, 0x1F9FA,       // 🦷 🧒 🧘 🧩 🧳 🧸 🧹 🧺
    0x1FA70, 0x1FA7A, 0x1FA81, 0x1FAB4,                                           // 🩰 🩺 🪁 🪴
};

inline bool is_icoon(uint32_t cp) { return std::binary_search(std::begin(ICONEN), std::end(ICONEN), cp); }

// Leest het UTF-8-teken op positie i en zet i erachter.
inline uint32_t utf8_teken(const std::string &s, size_t &i) {
  const auto b = [&](size_t k) { return k < s.size() ? static_cast<uint8_t>(s[k]) : 0; };
  const uint8_t c = b(i);
  if (c < 0x80) {
    i += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    i += 2;
    return ((c & 0x1F) << 6) | (b(i - 1) & 0x3F);
  }
  if ((c & 0xF0) == 0xE0) {
    i += 3;
    return ((c & 0x0F) << 12) | ((b(i - 2) & 0x3F) << 6) | (b(i - 1) & 0x3F);
  }
  i += 4;
  return ((c & 0x07) << 18) | ((b(i - 3) & 0x3F) << 12) | ((b(i - 2) & 0x3F) << 6) | (b(i - 1) & 0x3F);
}

// Haalt weg wat de fonts niet kunnen tekenen (anders een leeg blokje): emoji
// buiten ICONEN en de onzichtbare emoji-hulptekens (variatiekiezer, ZWJ,
// huidskleur, keycap, vlaggen). Dubbele spaties die dat oplevert gaan ook weg.
inline std::string zonder_onbekende_iconen(const std::string &s) {
  std::string uit;
  for (size_t i = 0; i < s.size();) {
    const size_t begin = i;
    const uint32_t cp = utf8_teken(s, i);
    const bool weg = (cp >= 0x2190 && !is_icoon(cp)) || (cp >= 0x200B && cp <= 0x200F) || cp == 0x20E3;
    if (weg || (cp == ' ' && (uit.empty() || uit.back() == ' ')))
      continue;
    uit.append(s, begin, std::min(i, s.size()) - begin);
  }
  while (!uit.empty() && uit.back() == ' ')
    uit.pop_back();
  return uit;
}

// --------------------------------------------------------------- gegevens ---

struct Afspraak {
  Datum datum;
  std::string tijd;  // "HH:MM", leeg = hele dag
  std::string titel;
  std::string ruw;  // titel zoals HA hem stuurde (met alle emoji), om details op te vragen
  std::string id;   // vaste id van de afspraak (van de blueprint), om details op te vragen
};

// Regels "JJJJ-MM-DD|HH:MM|titel" of "JJJJ-MM-DD|-|titel" (hele dag),
// gescheiden door '\n'. Zo stuurt de Home Assistant-blueprint ze. Achter de
// datum kan "~id" staan; oudere firmware leest daar gewoon overheen.
inline std::vector<Afspraak> lees_afspraken(const std::string &tekst) {
  std::vector<Afspraak> uit;
  size_t pos = 0;
  while (pos < tekst.size()) {
    size_t eind = tekst.find('\n', pos);
    if (eind == std::string::npos)
      eind = tekst.size();
    const std::string regel = tekst.substr(pos, eind - pos);
    pos = eind + 1;

    const size_t a = regel.find('|');
    const size_t b = a == std::string::npos ? a : regel.find('|', a + 1);
    if (b == std::string::npos)
      continue;
    Afspraak af;
    if (!lees_datum(regel.substr(0, a), af.datum))
      continue;
    const size_t tilde = regel.find('~');
    if (tilde < a)
      af.id = regel.substr(tilde + 1, a - tilde - 1);
    af.tijd = regel.substr(a + 1, b - a - 1);
    if (af.tijd == "-")
      af.tijd.clear();
    af.titel = regel.substr(b + 1);
    if (!af.titel.empty() && af.titel.back() == '\r')
      af.titel.pop_back();
    af.ruw = af.titel;
    af.titel = zonder_onbekende_iconen(af.titel);
    uit.push_back(af);
  }
  // Per dag: hele dag (en meerdaagse) bovenaan, daarna van vroeg naar laat.
  std::stable_sort(uit.begin(), uit.end(), [](const Afspraak &x, const Afspraak &y) {
    if (!(x.datum == y.datum))
      return dagnummer(x.datum) < dagnummer(y.datum);
    if (x.tijd.empty() != y.tijd.empty())
      return x.tijd.empty();
    return x.tijd < y.tijd;
  });
  return uit;
}

inline std::vector<std::string> lees_regels(const std::string &tekst) {
  std::vector<std::string> uit;
  size_t pos = 0;
  while (pos < tekst.size()) {
    size_t eind = tekst.find('\n', pos);
    if (eind == std::string::npos)
      eind = tekst.size();
    std::string regel = tekst.substr(pos, eind - pos);
    pos = eind + 1;
    if (!regel.empty() && regel.back() == '\r')
      regel.pop_back();
    if (!regel.empty())
      uit.push_back(regel);
  }
  return uit;
}

struct Notitie {
  std::string lijst;  // todo-entiteit, bijv. "todo.notities"
  std::string uid;    // id van het item in die lijst
  std::string tekst;
  bool afgevinkt{false};
};

// Regels "todo.lijst|uid|tekst". Een regel zonder '|' is alleen tekst
// (dan kan hij niet afgevinkt worden).
inline std::vector<Notitie> lees_notities(const std::string &tekst) {
  std::vector<Notitie> uit;
  for (const auto &regel : lees_regels(tekst)) {
    Notitie n;
    const size_t a = regel.find('|');
    const size_t b = a == std::string::npos ? a : regel.find('|', a + 1);
    if (b == std::string::npos) {
      n.tekst = regel;
    } else {
      n.lijst = regel.substr(0, a);
      n.uid = regel.substr(a + 1, b - a - 1);
      n.tekst = regel.substr(b + 1);
    }
    n.tekst = zonder_onbekende_iconen(n.tekst);
    if (!n.tekst.empty())
      uit.push_back(n);
  }
  return uit;
}

enum WeerIcoon : uint8_t { GEEN_ICOON, ZON, HALF_BEWOLKT, BEWOLKT, REGEN, SNEEUW, ONWEER, MIST };

// Weertoestand van Home Assistant (sunny, rainy, ...) -> icoon.
inline WeerIcoon weer_icoon(const std::string &c) {
  if (c == "sunny" || c == "clear-night")
    return ZON;
  if (c == "partlycloudy")
    return HALF_BEWOLKT;
  if (c == "cloudy" || c == "windy" || c == "windy-variant")
    return BEWOLKT;
  if (c == "rainy" || c == "pouring")
    return REGEN;
  if (c == "snowy" || c == "snowy-rainy" || c == "hail")
    return SNEEUW;
  if (c == "lightning" || c == "lightning-rainy")
    return ONWEER;
  if (c == "fog")
    return MIST;
  return GEEN_ICOON;
}

struct Weer {
  Datum datum;
  WeerIcoon icoon{GEEN_ICOON};
  std::string max, min;  // hele graden als tekst, leeg = onbekend
};

// Regels "JJJJ-MM-DD|toestand|max|min" (min mag leeg).
inline std::vector<Weer> lees_weer(const std::string &tekst) {
  std::vector<Weer> uit;
  for (const auto &regel : lees_regels(tekst)) {
    std::vector<std::string> veld;
    size_t pos = 0;
    for (size_t eind; veld.size() < 3 && (eind = regel.find('|', pos)) != std::string::npos; pos = eind + 1)
      veld.push_back(regel.substr(pos, eind - pos));
    veld.push_back(regel.substr(pos));
    if (veld.size() < 3)
      continue;
    veld.resize(4);
    Weer w;
    if (!lees_datum(veld[0], w.datum))
      continue;
    w.icoon = weer_icoon(veld[1]);
    w.max = veld[2].substr(0, 4);
    w.min = veld[3].substr(0, 4);
    uit.push_back(w);
  }
  return uit;
}

// ------------------------------------------------------------------ staat ---

// DAG = overzicht van één dag (vanuit de maand of "+N meer"), DETAILS = één afspraak.
enum Status : uint8_t { RUST, LUISTEREN, VERWERKEN, VOORSTEL, MELDING, DAG, DETAILS };

enum Zone : int {
  GEEN = -1,
  SPREEK = 0,
  KNOP1 = 1,  // KNOP1..KNOP4 = Home Assistant-knoppen
  OPSLAAN = 10,
  ANNULEER = 11,
  OK = 12,  // ook "Sluiten"
  TERUG = 13,
  TAB_WEEK = 20,
  TAB_MAAND = 21,
  NOTITIE0 = 100,      // NOTITIE0 + i = notitie i
  AFSPRAAK0 = 1000,    // AFSPRAAK0 + i = afspraak i (in Staat::afspraken)
  DAG0 = 100000,       // DAG0 + dagnummer = overzicht van die dag
};

enum Scherm : uint8_t { SCHERM_WEEK, SCHERM_MAAND };

// Spaarstand: WAKKER = normaal; SLAAPT = balk toont "Slaapstand" (ook tijdens
// een stille wekker-ronde); WORDT_WAKKER = gebruiker heeft gewekt, wacht op HA.
enum Slaap : uint8_t { WAKKER, SLAAPT, WORDT_WAKKER };

struct Voorstel {
  std::string titel, datum, begin, eind, gehoord;
  std::string soort;  // "afspraak" of "notitie"
};

// Details van een aangetikte afspraak. Titel en tijd zijn er meteen; de rest
// komt een tel later van Home Assistant (toon_details).
struct Details {
  int afspraak{-1};      // index in Staat::afspraken
  bool geladen{false};   // false = "Even ophalen…"
  bool van_dag{false};   // geopend vanuit het dagoverzicht: knop "Terug"
  std::string titel, datum, begin, eind_datum, eind, locatie, omschrijving, kalender;
  std::string fout;
};

struct HaKnop {
  std::string naam;
  bool aan{false};
};

struct Staat {
  Datum vandaag;
  bool tijd_geldig{false};
  bool data_ontvangen{false};
  bool verbonden{true};
  std::string ruw_afspraken, ruw_notities;
  std::vector<Afspraak> afspraken;
  std::vector<Notitie> notities;
  std::string ruw_weer;
  std::vector<Weer> weer;
  Status status{RUST};
  Scherm scherm{SCHERM_WEEK};
  Voorstel voorstel;
  std::string melding;
  Datum dag_gekozen;  // voor het dagoverzicht
  Details details;
  HaKnop knoppen[4];
  float temperatuur{NAN};
  float vochtigheid{NAN};
  float accu{NAN};  // procent
  Slaap slaap{WAKKER};
  uint8_t wek_stap{0};  // tijdens WORDT_WAKKER: 0 wifi, 1 Home Assistant, 2 agenda
};

// Onder dit percentage waarschuwt het scherm om op te laden.
constexpr int ACCU_LAAG = 15;

inline Staat &staat() {
  static Staat s;
  return s;
}

inline uint8_t &ververs_mask() {
  static uint8_t m = 0;
  return m;
}

// Rechthoek die met GEBIED opnieuw getekend wordt; meerdere worden samengevoegd.
struct Gebied {
  int x0{0}, y0{0}, x1{0}, y1{0};
  bool leeg() const { return x1 <= x0 || y1 <= y0; }
};

inline Gebied &ververs_gebied() {
  static Gebied g;
  return g;
}

inline void ververs_ook(int x, int y, int w, int h) {
  auto &g = ververs_gebied();
  if (g.leeg()) {
    g = {x, y, x + w, y + h};
  } else {
    g = {std::min(g.x0, x), std::min(g.y0, y), std::max(g.x1, x + w), std::max(g.y1, y + h)};
  }
  ververs_mask() |= GEBIED;
}

inline bool venster_open(const Staat &s) {
  return s.status == VOORSTEL || s.status == MELDING || s.status == DAG || s.status == DETAILS;
}

// Waar wat staat, bijgehouden tijdens het tekenen, zodat een aanraking bij de
// juiste afspraak of dag uitkomt.
struct Vak {
  int x, y, w, h;
  int zone;
};

inline std::vector<Vak> &vakken() {  // week en maand
  static std::vector<Vak> v;
  return v;
}

inline std::vector<Vak> &venster_vakken() {  // dagoverzicht
  static std::vector<Vak> v;
  return v;
}

inline Zone afspraak_zone(const Staat &s, const Afspraak *a) {
  return static_cast<Zone>(AFSPRAAK0 + static_cast<int>(a - s.afspraken.data()));
}

inline Zone dag_zone(const Datum &d) { return static_cast<Zone>(DAG0 + dagnummer(d)); }

// Afspraak aangetikt: details klaarzetten met wat we al weten.
inline void open_details(Staat &s, int i) {
  const Afspraak &a = s.afspraken[i];
  Details d;
  d.afspraak = i;
  d.van_dag = s.status == DAG;
  d.titel = a.titel;
  d.datum = datum_tekst(a.datum);
  d.begin = a.tijd;
  s.details = d;
  s.status = DETAILS;
}

// Antwoord van Home Assistant (toon_details).
inline void zet_details(Staat &s, const std::string &gevonden, const std::string &titel, const std::string &datum,
                        const std::string &begin, const std::string &eind_datum, const std::string &eind,
                        const std::string &locatie, const std::string &omschrijving, const std::string &kalender) {
  if (s.status != DETAILS || s.details.geladen)
    return;  // al dicht, of al beantwoord
  Details &d = s.details;
  d.geladen = true;
  if (gevonden != "ja") {
    d.fout = "Deze afspraak kon ik niet (meer) vinden in de agenda.";
    return;
  }
  d.titel = zonder_onbekende_iconen(titel);
  d.datum = datum;
  d.begin = begin;
  d.eind_datum = eind_datum;
  d.eind = eind;
  d.locatie = zonder_onbekende_iconen(locatie);
  d.omschrijving = zonder_onbekende_iconen(omschrijving);
  d.kalender = zonder_onbekende_iconen(kalender);
}

inline std::string klein_begin(std::string s) {
  if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z')
    s[0] = static_cast<char>(s[0] - 'A' + 'a');
  return s;
}

// "Woensdag 7 oktober · 16:00 – 17:00", over meer dagen
// "Vrijdag 9 oktober 18:00 – zondag 11 oktober 12:00".
inline std::string details_wanneer(const Details &d) {
  Datum van, tot;
  if (!lees_datum(d.datum, van))
    return d.datum;
  const bool meer_dagen = lees_datum(d.eind_datum, tot) && !(tot == van);
  if (d.begin.empty()) {
    if (meer_dagen)
      return datum_lang(van) + " t/m " + klein_begin(datum_lang(tot)) + "  ·  hele dag";
    return datum_lang(van) + "  ·  hele dag";
  }
  if (meer_dagen)
    return datum_lang(van) + " " + d.begin + " – " + klein_begin(datum_lang(tot)) + (d.eind.empty() ? "" : " " + d.eind);
  return datum_lang(van) + "  ·  " + d.begin + (d.eind.empty() ? "" : " – " + d.eind);
}

struct Fonts {
  BaseFont *klein;    // maandvakjes
  BaseFont *normaal;  // afspraken
  BaseFont *vet;      // tijden, knoppen
  BaseFont *kop;      // dagkoppen
  BaseFont *groot;    // datum bovenaan
  // Voor lettertypes zonder vette variant (zoals Patrick Hand): vet, kop en
  // groot twee keer tekenen, 1 pixel verschoven.
  bool nep_vet{false};
};

// Fonts die als 'nep-vet' getekend worden; gezet door teken().
inline BaseFont *(&nep_vette_fonts())[3] {
  static BaseFont *f[3] = {nullptr, nullptr, nullptr};
  return f;
}

// f_normaal, f_kop en f_groot hebben de iconen; klein en vet lenen ze (dat
// scheelt flash): {font zonder, font met}; gezet door teken().
inline std::pair<BaseFont *, BaseFont *> (&iconen_lenen())[2] {
  static std::pair<BaseFont *, BaseFont *> p[2] = {};
  return p;
}

inline BaseFont *leen_iconen_van(BaseFont *f) {
#ifdef INK_HOST
  return nullptr;  // de preview tekent iconen in elk font
#else
  for (const auto &p : iconen_lenen())
    if (p.first == f)
      return p.second;
  return nullptr;
#endif
}

// De tekst in stukken: gewone tekst in f, iconen in het font waar ze van lenen.
inline std::vector<std::pair<std::string, BaseFont *>> tekst_stukken(BaseFont *f, BaseFont *iconen,
                                                                       const std::string &s) {
  std::vector<std::pair<std::string, BaseFont *>> uit;
  for (size_t i = 0; i < s.size();) {
    const size_t begin = i;
    BaseFont *g = is_icoon(utf8_teken(s, i)) ? iconen : f;
    i = std::min(i, s.size());
    if (uit.empty() || uit.back().second != g)
      uit.push_back({std::string(), g});
    uit.back().first.append(s, begin, i - begin);
  }
  return uit;
}

inline bool met_iconen(const std::string &s) {
  for (size_t i = 0; i < s.size();)
    if (is_icoon(utf8_teken(s, i)))
      return true;
  return false;
}

inline int breedte(Display &it, BaseFont *f, const std::string &s) {
  if (s.empty())
    return 0;
  if (BaseFont *l = leen_iconen_van(f); l != nullptr && met_iconen(s)) {
    int w = 0;
    for (const auto &st : tekst_stukken(f, l, s))
      w += breedte(it, st.second, st.first);
    return w;
  }
  int x1, y1, w, h;
  it.get_text_bounds(0, 0, s.c_str(), f, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
  return w;
}

// Begin van het laatste UTF-8-teken vóór positie i.
inline size_t utf8_terug(const std::string &s, size_t i) {
  if (i == 0)
    return 0;
  i--;
  while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80)
    i--;
  return i;
}

inline std::string afkappen(Display &it, BaseFont *f, const std::string &s, int max) {
  if (breedte(it, f, s) <= max)
    return s;
  std::string r = s;
  while (!r.empty()) {
    r.erase(utf8_terug(r, r.size()));
    while (!r.empty() && r.back() == ' ')
      r.pop_back();
    if (breedte(it, f, r + "…") <= max)
      return r + "…";
  }
  return "";
}

// Woordterugloop. De eerste regel mag een andere breedte hebben (bijv. als er
// een tijd voor staat). Wat niet in max_regels past, wordt afgekapt met '…'.
inline std::vector<std::string> omloop(Display &it, BaseFont *f, const std::string &s, int eerste_max, int max,
                                       int max_regels) {
  std::vector<std::string> regels;
  std::string huidig;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t spatie = s.find(' ', pos);
    if (spatie == std::string::npos)
      spatie = s.size();
    const std::string woord = s.substr(pos, spatie - pos);
    pos = spatie + 1;
    if (woord.empty())
      continue;
    const int grens = regels.empty() ? eerste_max : max;
    const std::string kandidaat = huidig.empty() ? woord : huidig + " " + woord;
    if (huidig.empty() && regels.empty() && eerste_max < max && breedte(it, f, woord) > eerste_max) {
      // Eerste woord past niet naast de tijd: begin op de volgende regel.
      regels.push_back("");
      huidig = woord;
    } else if (huidig.empty() || breedte(it, f, kandidaat) <= grens) {
      huidig = kandidaat;
    } else {
      regels.push_back(huidig);
      huidig = woord;
    }
  }
  if (!huidig.empty())
    regels.push_back(huidig);

  if (static_cast<int>(regels.size()) > max_regels) {
    std::string rest;
    for (size_t i = max_regels - 1; i < regels.size(); i++)
      rest += (rest.empty() ? "" : " ") + regels[i];
    regels.resize(max_regels);
    regels.back() = rest + " …";  // forceer afkapteken
  }
  for (size_t i = 0; i < regels.size(); i++)
    regels[i] = afkappen(it, f, regels[i], i == 0 ? eerste_max : max);
  return regels;
}

inline void tekst(Display &it, int x, int y, BaseFont *f, Color kleur, Color achter, TextAlign uitlijning,
                  const std::string &s) {
#ifndef INK_HOST
  // Iconen uit een ander font: stuk voor stuk, allemaal op dezelfde basislijn.
  if (BaseFont *l = leen_iconen_van(f); l != nullptr && met_iconen(s)) {
    const int w = breedte(it, f, s);
    if (uitlijning == TextAlign::TOP_CENTER)
      x -= w / 2;
    else if (uitlijning == TextAlign::TOP_RIGHT)
      x -= w;
    int x1, y1, bw, bh;
    it.get_text_bounds(0, 0, "A", f, TextAlign::BASELINE_LEFT, &x1, &y1, &bw, &bh);
    const int basislijn = y - y1;  // y1 = -(afstand van de bovenkant tot de basislijn)
    for (const auto &st : tekst_stukken(f, l, s)) {
      it.print(x, basislijn, st.second, kleur, TextAlign::BASELINE_LEFT, st.first.c_str(), achter);
      for (BaseFont *v : nep_vette_fonts())
        if (v == st.second)
          it.print(x + 1, basislijn, st.second, kleur, TextAlign::BASELINE_LEFT, st.first.c_str(), achter);
      x += breedte(it, st.second, st.first);
    }
    return;
  }
#endif
  it.print(x, y, f, kleur, uitlijning, s.c_str(), achter);
  for (BaseFont *v : nep_vette_fonts())
    if (v == f)
      it.print(x + 1, y, f, kleur, uitlijning, s.c_str(), achter);
}

inline void kader(Display &it, int x, int y, int w, int h, int dikte, Color kleur) {
  for (int i = 0; i < dikte; i++)
    it.rectangle(x + i, y + i, w - 2 * i, h - 2 * i, kleur);
}

// --------------------------------------------------------------- tekenen ---

inline int tab_x(int i) { return TAB_X + i * TAB_B; }

// Tabs "Week" / "Maand" (alleen zwart/wit, dus ook met DU te tekenen)
inline void teken_tabs(Display &it, const Staat &s, const Fonts &f) {
  static const char *const NAMEN[] = {"Week", "Maand"};
  for (int i = 0; i < 2; i++) {
    const bool actief = s.scherm == i;
    const Color a = actief ? ZWART : WIT;
    vlak(it, tab_x(i), TAB_Y, TAB_B, TAB_H, a);
    kader(it, tab_x(i), TAB_Y, TAB_B, TAB_H, 3, ZWART);
    tekst(it, tab_x(i) + TAB_B / 2, TAB_Y + 4, f.kop, actief ? WIT : ZWART, a, TextAlign::TOP_CENTER, NAMEN[i]);
  }
}

// Wissel week/maand: alles tussen kop en knoppenbalk (kop en balk blijven gelijk).
inline void ververs_inhoud() { ververs_ook(0, KOP_H + 3, B, std::max(NOTITIE_EIND + 2, GRID_EIND) - KOP_H - 3); }

inline void teken_kop(Display &it, const Staat &s, const Fonts &f) {
  // Met een waarschuwing ernaast de korte datum, anders past het niet
  const bool waarschuwing_zichtbaar = !s.verbonden || !s.data_ontvangen;
  std::string datum = "Ink kalender";
  if (s.tijd_geldig && waarschuwing_zichtbaar) {
    char kort[24];
    std::snprintf(kort, sizeof(kort), "%s %d %s", DAG_KORT[weekdag(s.vandaag)], s.vandaag.d,
                  MAAND_KORT[s.vandaag.m - 1]);
    datum = kort;
  } else if (s.tijd_geldig) {
    datum = datum_lang(s.vandaag);
  }
  tekst(it, MARGE, 4, f.groot, ZWART, WIT, TextAlign::TOP_LEFT, datum);
  teken_tabs(it, s, f);

  // Rechts, van rechts naar links: accu, temperatuur, weeknummer
  char buf[48];
  int xr = B - MARGE;
  if (!std::isnan(s.accu)) {
    const int pct = static_cast<int>(std::lround(s.accu));
    const bool laag = pct <= ACCU_LAAG;
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    tekst(it, xr, 28, laag ? f.vet : f.normaal, laag ? ZWART : GRIJS_DONKER, WIT, TextAlign::TOP_RIGHT, buf);
    xr -= breedte(it, laag ? f.vet : f.normaal, buf) + 10;
    // Accu-icoontje: 46x24 met knopje rechts, vulling naar verhouding
    const int ix = xr - 52, iy = 36;
    const Color rand = laag ? ZWART : GRIJS_DONKER;
    kader(it, ix, iy, 46, 24, 2, rand);
    vlak(it, ix + 46, iy + 7, 4, 10, rand);
    const int vul = std::max(0, std::min(40, pct * 40 / 100));
    if (vul > 0)
      vlak(it, ix + 3, iy + 3, vul, 18, rand);
    xr = ix - 36;
    if (laag) {
      tekst(it, xr, 28, f.vet, ZWART, WIT, TextAlign::TOP_RIGHT, "opladen!");
      xr -= breedte(it, f.vet, "opladen!") + 36;
    }
  }
  if (!std::isnan(s.temperatuur)) {
    if (!std::isnan(s.vochtigheid))
      std::snprintf(buf, sizeof(buf), "%.1f °C  ·  %.0f%%", s.temperatuur, s.vochtigheid);
    else
      std::snprintf(buf, sizeof(buf), "%.1f °C", s.temperatuur);
    std::string t = buf;
    for (auto &c : t)
      if (c == '.')
        c = ',';
    tekst(it, xr, 28, f.normaal, GRIJS_DONKER, WIT, TextAlign::TOP_RIGHT, t);
    xr -= breedte(it, f.normaal, t) + 36;
  }
  if (s.tijd_geldig) {
    std::snprintf(buf, sizeof(buf), "Week %d", iso_week(s.vandaag));
    // Alleen als het naast de tabs past
    if (xr - breedte(it, f.kop, buf) > tab_x(2) + 20)
      tekst(it, xr, 20, f.kop, ZWART, WIT, TextAlign::TOP_RIGHT, buf);
  }
  if (waarschuwing_zichtbaar) {
    const std::string waarschuwing = !s.verbonden ? "Geen verbinding" : "Wacht op agenda…";
    const int w = breedte(it, f.vet, waarschuwing) + 28;
    const int x = TAB_X - w - 20;
    vlak(it, x, TAB_Y + 4, w, TAB_H - 8, ZWART);
    tekst(it, x + 14, TAB_Y + 10, f.vet, WIT, ZWART, TextAlign::TOP_LEFT, waarschuwing);
  }
  vlak(it, MARGE, KOP_H, BREED, 3, ZWART);
}

// Hoogte die een afspraak in de weekkolom inneemt (en tekent als teken=true).
inline int week_afspraak(Display &it, const Fonts &f, const Afspraak &a, int x, int y, int w, bool teken) {
  constexpr int REGEL = 30;
  if (a.tijd.empty()) {
    auto regels = omloop(it, f.normaal, a.titel, w - 16, w - 16, 2);
    const int h = static_cast<int>(regels.size()) * REGEL + 10;
    if (teken) {
      vlak(it, x, y, w, h, GRIJS_LICHT);
      for (size_t i = 0; i < regels.size(); i++)
        tekst(it, x + 8, y + 1 + i * REGEL, f.normaal, ZWART, GRIJS_LICHT, TextAlign::TOP_LEFT, regels[i]);
    }
    return h;
  }
  const int tijd_b = breedte(it, f.vet, a.tijd) + 10;
  auto regels = omloop(it, f.normaal, a.titel, w - tijd_b, w, 3);
  const int h = static_cast<int>(regels.size()) * REGEL;
  if (teken) {
    tekst(it, x, y - 4, f.vet, ZWART, WIT, TextAlign::TOP_LEFT, a.tijd);
    for (size_t i = 0; i < regels.size(); i++)
      tekst(it, x + (i == 0 ? tijd_b : 0), y - 4 + i * REGEL, f.normaal, ZWART, WIT, TextAlign::TOP_LEFT,
            regels[i]);
  }
  return h;
}

// ------------------------------------------------------------- weericonen ---

// Weericonen worden getekend op schaal `m` in tienden: 10 = ±44x40 pixels.
struct Pen {
  Display &it;
  int cx, cy, m;
  Color k;
  int x(int v) const { return cx + v * m / 10; }
  int y(int v) const { return cy + v * m / 10; }
  int r(int v) const { return std::max(1, v * m / 10); }
  void cirkel(int px, int py, int pr, int rand = 0) const { it.filled_circle(x(px), y(py), r(pr) + rand, k); }
  void lijn(int x1, int y1, int x2, int y2) const {
    for (int d = m >= 8 ? -1 : 0; d <= 1; d++) {
      it.line(x(x1) + d, y(y1), x(x2) + d, y(y2), k);
      it.line(x(x1), y(y1) + d, x(x2), y(y2) + d, k);
    }
  }
  void balk(int px, int py, int w, int h) const { vlak(it, x(px), y(py), r(w), std::max(2, h * m / 10), k); }
};

// Wolkje rond (0, 0); `rand` maakt hem zoveel pixels groter.
inline void teken_wolk(const Pen &p, int dy, int rand = 0) {
  p.cirkel(-11, dy + 5, 8, rand);
  p.cirkel(1, dy - 2, 12, rand);
  p.cirkel(13, dy + 5, 8, rand);
  vlak(p.it, p.x(-11), p.y(dy + 5), p.r(25), p.r(9) + rand, p.k);
}

inline void teken_zon(const Pen &p, int px, int py, int pr) {
  p.cirkel(px, py, pr);
  static const int RICHTING[8][2] = {{10, 0}, {7, 7}, {0, 10}, {-7, 7}, {-10, 0}, {-7, -7}, {0, -10}, {7, -7}};
  for (const auto &d : RICHTING)
    p.lijn(px + d[0] * (pr + 4) / 10, py + d[1] * (pr + 4) / 10, px + d[0] * (pr + 9) / 10, py + d[1] * (pr + 9) / 10);
}

// Weericoon rond (cx, cy) op schaal `m`, in `voor` op achtergrond `achter`.
inline void teken_weericoon(Display &it, WeerIcoon icoon, int cx, int cy, int m, Color voor, Color achter) {
  const Pen p{it, cx, cy, m, voor};
  switch (icoon) {
    case ZON:
      teken_zon(p, 0, 0, 10);
      break;
    case HALF_BEWOLKT:
      teken_zon(p, -8, -5, 7);
      teken_wolk(Pen{it, p.x(4), cy, m, achter}, 4, 3);  // los van de zon
      teken_wolk(Pen{it, p.x(4), cy, m, voor}, 4);
      break;
    case BEWOLKT:
      teken_wolk(p, 0);
      break;
    case REGEN:
      teken_wolk(p, -8);
      for (int i = -1; i <= 1; i++)
        p.lijn(i * 11 + 3, 9, i * 11 - 1, 16);
      break;
    case SNEEUW:
      teken_wolk(p, -8);
      for (int i = -1; i <= 1; i++)
        p.cirkel(i * 11, 11 + (i == 0 ? 4 : 0), 3);
      break;
    case ONWEER:
      teken_wolk(p, -8);
      p.lijn(3, 8, -3, 13);
      p.lijn(-3, 13, 4, 13);
      p.lijn(4, 13, -2, 19);
      break;
    case MIST:
      for (int i = 0; i < 4; i++)
        p.balk(-18 + (i % 2) * 6, -11 + i * 7, 30, 3);
      break;
    case GEEN_ICOON:
      break;
  }
}

// Klein icoon en de hoogste temperatuur rechts op de datumregel van de dagkop.
inline void teken_dagweer(Display &it, const Weer &w, int rechts, const Fonts &f, Color voor, Color achter) {
  const std::string t = w.max.empty() ? "" : w.max + "°";
  tekst(it, rechts - 10, WEEK_Y + 40, f.klein, voor, achter, TextAlign::TOP_RIGHT, t);
  const int tb = breedte(it, f.klein, t);
  teken_weericoon(it, w.icoon, rechts - 10 - tb - (tb ? 22 : 14), WEEK_Y + 55, 6, voor, achter);
}

inline void teken_week(Display &it, const Staat &s, const Fonts &f) {
  const Datum maandag = plus_dagen(s.vandaag, -weekdag(s.vandaag));
  for (int i = 0; i < 7; i++) {
    const Datum dag = plus_dagen(maandag, i);
    const bool is_vandaag = s.tijd_geldig && dag == s.vandaag;
    const int x = MARGE + i * WEEK_KOL;
    const int w = WEEK_KOL - 10;

    const Color kop_achter = is_vandaag ? ZWART : GRIJS_LICHT;
    const Color kop_tekst = is_vandaag ? WIT : ZWART;
    vlak(it, x, WEEK_Y, w, WEEK_KOP_H, kop_achter);
    // Dagnaam groot, datum klein eronder
    tekst(it, x + 14, WEEK_Y - 2, f.kop, kop_tekst, kop_achter, TextAlign::TOP_LEFT, DAG_LANG[i]);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d %s", dag.d, MAAND[dag.m - 1]);
    tekst(it, x + 14, WEEK_Y + 40, f.klein, kop_tekst, kop_achter, TextAlign::TOP_LEFT, buf);
    // Voorbije dagen (de laatste verwachting, uit het geheugen) iets lichter.
    const bool voorbij = s.tijd_geldig && dagnummer(dag) < dagnummer(s.vandaag);
    for (const auto &weer : s.weer)
      if (weer.datum == dag) {
        teken_dagweer(it, weer, x + w, f, is_vandaag || voorbij ? GRIJS : GRIJS_DONKER, kop_achter);
        break;
      }

    if (i > 0)
      it.vertical_line(x - 5, WEEK_Y + WEEK_KOP_H + 8, WEEK_EIND - WEEK_Y - WEEK_KOP_H - 8, GRIJS);

    // Hele-dag-afspraken eerst (staan al vooraan door sortering in HA).
    std::vector<const Afspraak *> lijst;
    for (const auto &a : s.afspraken)
      if (a.datum == dag)
        lijst.push_back(&a);

    int y = WEEK_Y + WEEK_KOP_H + 12;
    const int bx = x + 6, bw = w - 10;
    for (size_t n = 0; n < lijst.size(); n++) {
      const int h = week_afspraak(it, f, *lijst[n], bx, y, bw, false);
      const bool laatste = n + 1 == lijst.size();
      if (y + h > WEEK_EIND - (laatste ? 0 : 34)) {
        char meer[24];
        std::snprintf(meer, sizeof(meer), "+%d meer", static_cast<int>(lijst.size() - n));
        tekst(it, bx, y - 4, f.vet, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT, meer);
        vakken().push_back({bx, y - 10, bw, 50, dag_zone(dag)});  // tik = hele dag
        break;
      }
      week_afspraak(it, f, *lijst[n], bx, y, bw, true);
      vakken().push_back({bx, y - 6, bw, h + 12, afspraak_zone(s, lijst[n])});
      y += h + 12;
    }
  }
}

inline void teken_maand(Display &it, const Staat &s, const Fonts &f) {
  const Datum eerste{s.vandaag.j, s.vandaag.m, 1};
  const Datum start = plus_dagen(eerste, -weekdag(eerste));
  const Datum volgende{s.vandaag.m == 12 ? s.vandaag.j + 1 : s.vandaag.j, s.vandaag.m == 12 ? 1 : s.vandaag.m + 1, 1};
  const int dagen = dagnummer(volgende) - dagnummer(eerste);
  const int rijen = (weekdag(eerste) + dagen + 6) / 7;

  char buf[40];
  constexpr int DAGKOP_H = 40;
  for (int i = 0; i < 7; i++)
    tekst(it, MARGE + i * MAAND_KOL + 12, GRID_Y, f.vet, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT, DAG_LANG[i]);

  const int top = GRID_Y + DAGKOP_H;
  const int rij_h = (GRID_EIND - top) / rijen;
  const int links = MARGE, rechts = MARGE + 7 * MAAND_KOL;
  for (int r = 0; r <= rijen; r++)
    it.horizontal_line(links, top + r * rij_h, rechts - links, r == 0 || r == rijen ? ZWART : GRIJS);
  for (int c = 0; c <= 7; c++)
    it.vertical_line(links + c * MAAND_KOL, top, rij_h * rijen, c == 0 || c == 7 ? ZWART : GRIJS);

  constexpr int REGEL = 34;
  for (int n = 0; n < rijen * 7; n++) {
    const Datum dag = plus_dagen(start, n);
    const int x = links + (n % 7) * MAAND_KOL;
    const int y = top + (n / 7) * rij_h;
    const bool deze_maand = dag.m == s.vandaag.m;
    const bool is_vandaag = s.tijd_geldig && dag == s.vandaag;

    std::snprintf(buf, sizeof(buf), "%d", dag.d);
    if (deze_maand && dag.d == 1) {
      std::snprintf(buf, sizeof(buf), "1 %s", MAAND_KORT[dag.m - 1]);
    } else if (!deze_maand) {
      std::snprintf(buf, sizeof(buf), "%d %s", dag.d, MAAND_KORT[dag.m - 1]);
    } else {
      std::snprintf(buf, sizeof(buf), "%d", dag.d);
    }
    if (is_vandaag) {
      const int nb = breedte(it, f.kop, buf) + 20;
      vlak(it, x + 1, y + 1, nb, 40, ZWART);
      tekst(it, x + 10, y + 1, f.kop, WIT, ZWART, TextAlign::TOP_LEFT, buf);
    } else {
      tekst(it, x + 10, y + 1, f.kop, deze_maand ? ZWART : GRIJS, WIT, TextAlign::TOP_LEFT, buf);
    }

    vakken().push_back({x, y, MAAND_KOL, rij_h, dag_zone(dag)});
    const Color kleur = deze_maand ? ZWART : GRIJS_DONKER;
    const int max_regels = (rij_h - 44) / REGEL;
    std::vector<const Afspraak *> lijst;
    for (const auto &a : s.afspraken)
      if (a.datum == dag)
        lijst.push_back(&a);
    int ty = y + 38;
    for (size_t i = 0; i < lijst.size() && static_cast<int>(i) < max_regels; i++) {
      const bool laatste_plek = static_cast<int>(i) == max_regels - 1;
      if (laatste_plek && lijst.size() > i + 1) {
        std::snprintf(buf, sizeof(buf), "+%d meer", static_cast<int>(lijst.size() - i));
        tekst(it, x + 10, ty, f.normaal, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT, buf);
        break;
      }
      const Afspraak &a = *lijst[i];
      const std::string regel = a.tijd.empty() ? a.titel : a.tijd + " " + a.titel;
      if (a.tijd.empty()) {
        vlak(it, x + 4, ty + 8, MAAND_KOL - 8, REGEL - 2, GRIJS_LICHT);
        tekst(it, x + 10, ty, f.normaal, kleur, GRIJS_LICHT, TextAlign::TOP_LEFT,
              afkappen(it, f.normaal, regel, MAAND_KOL - 22));
      } else {
        tekst(it, x + 10, ty, f.normaal, kleur, WIT, TextAlign::TOP_LEFT,
              afkappen(it, f.normaal, regel, MAAND_KOL - 22));
      }
      ty += REGEL;
    }
  }
}

constexpr int NOTITIE_REGEL = 56;  // ±6 mm: groot genoeg voor een vinger
constexpr int NOTITIE_KOLOMMEN = 3;
constexpr int NOTITIE_TUSSEN = 30;
constexpr int NOTITIE_TOP = NOTITIE_Y + 42;
constexpr int NOTITIE_KOL_B = (BREED - (NOTITIE_KOLOMMEN - 1) * NOTITIE_TUSSEN) / NOTITIE_KOLOMMEN;
constexpr int NOTITIE_PER_KOLOM = (NOTITIE_EIND - NOTITIE_TOP) / NOTITIE_REGEL;
constexpr int NOTITIE_MAX = NOTITIE_PER_KOLOM * NOTITIE_KOLOMMEN;

inline int notitie_x(int i) { return MARGE + (i / NOTITIE_PER_KOLOM) * (NOTITIE_KOL_B + NOTITIE_TUSSEN); }
inline int notitie_y(int i) { return NOTITIE_TOP + (i % NOTITIE_PER_KOLOM) * NOTITIE_REGEL; }

// Welke notitieplek bij een aanraking hoort: de rij eronder en de dichtstbijzijnde
// kolom (ook in de ruimte tussen de kolommen). -1 als het buiten de notities is.
inline int notitie_op(int x, int y) {
  if (y < NOTITIE_TOP - 8 || y >= NOTITIE_TOP + NOTITIE_PER_KOLOM * NOTITIE_REGEL || x < MARGE - 10 || x >= B - MARGE + 10)
    return -1;
  const int rij = std::max(0, y - NOTITIE_TOP) / NOTITIE_REGEL;
  const int kol = std::min(NOTITIE_KOLOMMEN - 1, std::max(0, x - MARGE + NOTITIE_TUSSEN / 2) / (NOTITIE_KOL_B + NOTITIE_TUSSEN));
  return kol * NOTITIE_PER_KOLOM + rij;
}

inline void teken_notities(Display &it, const Staat &s, const Fonts &f) {
  tekst(it, MARGE, NOTITIE_Y - 8, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, "Notities");
  tekst(it, MARGE + breedte(it, f.kop, "Notities") + 24, NOTITIE_Y + 2, f.klein, GRIJS_DONKER, WIT,
        TextAlign::TOP_LEFT, "tik om af te vinken, nog eens tikken zet hem terug");
  for (int k = 0; k < NOTITIE_KOLOMMEN; k++) {
    const int x = MARGE + k * (NOTITIE_KOL_B + NOTITIE_TUSSEN);
    vlak(it, x, NOTITIE_TOP, NOTITIE_KOL_B, 2, ZWART);
    for (int r = 1; r <= NOTITIE_PER_KOLOM; r++)
      it.horizontal_line(x, NOTITIE_TOP + r * NOTITIE_REGEL, NOTITIE_KOL_B, GRIJS);
  }
  if (s.notities.empty()) {
    tekst(it, MARGE + 8, NOTITIE_TOP + 10, f.normaal, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT, "Geen notities");
    return;
  }
  const int aantal = static_cast<int>(s.notities.size());
  for (int i = 0; i < aantal && i < NOTITIE_MAX; i++) {
    const int x = notitie_x(i);
    const int y = notitie_y(i);
    if (i == NOTITIE_MAX - 1 && aantal > NOTITIE_MAX) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "+%d meer", aantal - i);
      tekst(it, x + 46, y + 10, f.normaal, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT, buf);
      break;
    }
    const Notitie &n = s.notities[i];
    // Vakje om af te vinken
    kader(it, x + 4, y + 13, 30, 30, 3, n.uid.empty() ? GRIJS : ZWART);
    if (n.afgevinkt) {
      for (int d = -1; d <= 1; d++) {
        it.line(x + 9, y + 27 + d, x + 17, y + 36 + d, ZWART);
        it.line(x + 17, y + 36 + d, x + 38, y + 9 + d, ZWART);
      }
    }
    // Alleen zwart/wit: afvinken wordt met de snelle DU-modus getekend, en die
    // kan geen grijs (grijze pixels veranderen dan niet).
    const std::string t = afkappen(it, f.normaal, n.tekst, NOTITIE_KOL_B - 54);
    tekst(it, x + 46, y + 10, f.normaal, ZWART, WIT, TextAlign::TOP_LEFT, t);
    if (n.afgevinkt)
      vlak(it, x + 44, y + 29, breedte(it, f.normaal, t) + 4, 3, ZWART);
  }
}

inline int ha_knop_x(int i) { return MARGE + SPREEK_B + KNOP_TUSSEN + i * (HA_KNOP_B + KNOP_TUSSEN); }

inline void teken_microfoon(Display &it, int mx, int my, Color voor, Color achter) {
  for (int d = 0; d < 3; d++)
    it.circle(mx, my - 2, 22 + d, voor);
  vlak(it, mx - 26, my - 30, 52, 28, achter);  // bovenste helft van de boog weg
  vlak(it, mx - 11, my - 34, 22, 40, voor);
  it.filled_circle(mx, my - 34, 11, voor);
  it.filled_circle(mx, my + 6, 11, voor);
  vlak(it, mx - 2, my + 20, 4, 18, voor);
  vlak(it, mx - 14, my + 36, 28, 4, voor);
}

// Zonnetje voor "wakker worden"
inline void teken_zon(Display &it, int mx, int my, Color kleur) {
  it.filled_circle(mx, my, 17, kleur);
  // 8 stralen; richting * 7 (schuin: 5 en 5, ongeveer even lang)
  static const int8_t richting[8][2] = {{7, 0}, {-7, 0}, {0, 7}, {0, -7}, {5, 5}, {-5, 5}, {5, -5}, {-5, -5}};
  for (const auto &r : richting)
    for (int dx = -1; dx <= 1; dx++)
      for (int dy = -1; dy <= 1; dy++)
        it.line(mx + r[0] * 25 / 7 + dx, my + r[1] * 25 / 7 + dy, mx + r[0] * 37 / 7 + dx, my + r[1] * 37 / 7 + dy, kleur);
}

// Voortgang tijdens het wakker worden: wifi -> Home Assistant -> agenda.
inline void teken_wekstappen(Display &it, const Fonts &f, int stap, Color v, Color a) {
  static const char *const namen[3] = {"Wifi", "Home Assistant", "Agenda"};
  constexpr int AFSTAND = 250, R = 15;
  const int y = KNOP_Y + 40;
  const int x0 = B - MARGE - 110 - 2 * AFSTAND;
  for (int i = 0; i < 2; i++) {  // verbindingslijnen
    const int van = x0 + i * AFSTAND + R + 8, tot = x0 + (i + 1) * AFSTAND - R - 8;
    if (i < stap)
      vlak(it, van, y - 2, tot - van, 4, v);
    else
      for (int x = van; x < tot; x += 16)
        vlak(it, x, y - 1, std::min(8, tot - x), 2, v);
  }
  for (int i = 0; i < 3; i++) {
    const int x = x0 + i * AFSTAND;
    if (i < stap) {  // klaar: dicht bolletje met vinkje
      it.filled_circle(x, y, R, v);
      for (int d = 0; d < 3; d++) {
        it.line(x - 7, y + d - 1, x - 2, y + 5 + d - 1, a);
        it.line(x - 2, y + 5 + d - 1, x + 7, y - 5 + d - 1, a);
      }
    } else {  // bezig: dikke ring met stip; nog niet: dunne ring
      for (int d = 0; d < (i == stap ? 4 : 2); d++)
        it.circle(x, y, R - d, v);
      if (i == stap)
        it.filled_circle(x, y, 5, v);
    }
    tekst(it, x, y + R + 10, i == stap ? f.vet : f.normaal, v, a, TextAlign::TOP_CENTER, namen[i]);
  }
}

inline void teken_slaapbalk(Display &it, const Staat &s, const Fonts &f) {
  // Alleen zwart/wit: deze balk wordt met de snelle DU-modus getekend.
  const Color a = WIT, v = ZWART;
  vlak(it, MARGE, KNOP_Y, BREED, KNOP_H, a);
  kader(it, MARGE, KNOP_Y, BREED, KNOP_H, 3, ZWART);
  if (s.slaap == WORDT_WAKKER) {
    teken_zon(it, MARGE + 72, KNOP_Y + KNOP_H / 2, v);
    tekst(it, MARGE + 130, KNOP_Y + 14, f.groot, v, a, TextAlign::TOP_LEFT, "Even wakker worden…");
    teken_wekstappen(it, f, s.wek_stap, v, a);
  } else {
    // Maantje
    it.filled_circle(MARGE + 70, KNOP_Y + KNOP_H / 2, 30, v);
    it.filled_circle(MARGE + 84, KNOP_Y + KNOP_H / 2 - 12, 28, a);
    tekst(it, MARGE + 130, KNOP_Y + 14, f.groot, v, a, TextAlign::TOP_LEFT, "Slaapstand");
    tekst(it, B - MARGE - 40, KNOP_Y + 22, f.kop, v, a, TextAlign::TOP_RIGHT, "Tik twee keer op het scherm om te wekken");
    tekst(it, B - MARGE - 40, KNOP_Y + 68, f.normaal, v, a, TextAlign::TOP_RIGHT,
          "of druk op een knop · de agenda ververst vanzelf");
  }
}

inline void teken_balk(Display &it, const Staat &s, const Fonts &f) {
  vlak(it, 0, BALK_Y, B, H - BALK_Y, WIT);
  it.horizontal_line(MARGE, BALK_Y, BREED, ZWART);
  if (s.slaap != WAKKER) {
    teken_slaapbalk(it, s, f);
    return;
  }

  // Spreekknop
  std::string label = "Inspreken";
  std::string hint = "een afspraak of een notitie";
  const bool actief = s.status == LUISTEREN || s.status == VERWERKEN;
  if (s.status == LUISTEREN) {
    label = "Ik luister…";
    hint = "tik nogmaals om te stoppen";
  } else if (s.status == VERWERKEN) {
    label = "Even denken…";
    hint = "";
  }
  const Color achter = actief ? ZWART : WIT;
  const Color voor = actief ? WIT : ZWART;
  vlak(it, MARGE, KNOP_Y, SPREEK_B, KNOP_H, achter);
  kader(it, MARGE, KNOP_Y, SPREEK_B, KNOP_H, 4, ZWART);
  teken_microfoon(it, MARGE + 56, KNOP_Y + KNOP_H / 2, voor, achter);
  tekst(it, MARGE + 108, KNOP_Y + (hint.empty() ? 30 : 12), f.kop, voor, achter, TextAlign::TOP_LEFT, label);
  if (!hint.empty())
    tekst(it, MARGE + 108, KNOP_Y + 66, f.klein, voor, achter, TextAlign::TOP_LEFT,
          afkappen(it, f.klein, hint, SPREEK_B - 124));

  // Home Assistant-knoppen
  for (int i = 0; i < 4; i++) {
    const HaKnop &k = s.knoppen[i];
    if (k.naam.empty())
      continue;
    const int x = ha_knop_x(i);
    const Color a = k.aan ? ZWART : WIT;
    const Color v = k.aan ? WIT : ZWART;
    vlak(it, x, KNOP_Y, HA_KNOP_B, KNOP_H, a);
    kader(it, x, KNOP_Y, HA_KNOP_B, KNOP_H, 3, ZWART);
    auto regels = omloop(it, f.kop, k.naam, HA_KNOP_B - 24, HA_KNOP_B - 24, 2);
    const int regel_h = 34;
    const int ty = KNOP_Y + (KNOP_H - 30 - static_cast<int>(regels.size()) * regel_h) / 2 - 6;
    for (size_t r = 0; r < regels.size(); r++)
      tekst(it, x + HA_KNOP_B / 2, ty + r * regel_h, f.kop, v, a, TextAlign::TOP_CENTER, regels[r]);
    tekst(it, x + HA_KNOP_B / 2, KNOP_Y + KNOP_H - 36, f.klein, v, a, TextAlign::TOP_CENTER, k.aan ? "aan" : "uit");
  }
}

inline void venster_knop(Display &it, const Fonts &f, int x, const char *label, bool gevuld,
                         int y = VENSTER_KNOP_Y) {
  const Color a = gevuld ? ZWART : WIT;
  vlak(it, x, y, VENSTER_KNOP_B, VENSTER_KNOP_H, a);
  kader(it, x, y, VENSTER_KNOP_B, VENSTER_KNOP_H, 4, ZWART);
  tekst(it, x + VENSTER_KNOP_B / 2, y + 26, f.kop, gevuld ? WIT : ZWART, a, TextAlign::TOP_CENTER, label);
}

inline int opslaan_x() { return VENSTER_X + 60; }
inline int annuleer_x() { return VENSTER_X + VENSTER_B - 60 - VENSTER_KNOP_B; }
inline int ok_x() { return VENSTER_X + (VENSTER_B - VENSTER_KNOP_B) / 2; }

// Knoppen onder in het grote venster: "Sluiten" (en "Terug" bij details vanuit een dag).
inline bool met_terug(const Staat &s) { return s.status == DETAILS && s.details.van_dag; }
inline int sluiten_x(const Staat &s) {
  return met_terug(s) ? GROOT_X + GROOT_B - 60 - VENSTER_KNOP_B : GROOT_X + (GROOT_B - VENSTER_KNOP_B) / 2;
}
inline int terug_x() { return GROOT_X + 60; }

// Alles in zwart/wit: het venster wordt met de snelle DU-modus getekend.
inline void teken_dagoverzicht(Display &it, const Staat &s, const Fonts &f) {
  const int x = GROOT_X + 60, w = GROOT_B - 120;
  tekst(it, x, GROOT_Y + 30, f.groot, ZWART, WIT, TextAlign::TOP_LEFT, datum_lang(s.dag_gekozen));
  std::vector<const Afspraak *> lijst;
  for (const auto &a : s.afspraken)
    if (a.datum == s.dag_gekozen)
      lijst.push_back(&a);
  int y = GROOT_Y + 120;
  vlak(it, x, y, w, 2, ZWART);
  if (lijst.empty()) {
    tekst(it, x, y + 20, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, "Geen afspraken");
    return;
  }
  const int max = (GROOT_KNOP_Y - 30 - y) / DAG_REGEL;
  for (size_t n = 0; n < lijst.size() && static_cast<int>(n) < max; n++) {
    if (static_cast<int>(n) == max - 1 && lijst.size() > n + 1) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "+%d meer", static_cast<int>(lijst.size() - n));
      tekst(it, x + 8, y + 14, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, buf);
      break;
    }
    const Afspraak &a = *lijst[n];
    const std::string tijd = a.tijd.empty() ? "hele dag" : a.tijd;
    tekst(it, x + 8, y + 16, f.vet, ZWART, WIT, TextAlign::TOP_LEFT, tijd);
    tekst(it, x + 150, y + 12, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, afkappen(it, f.kop, a.titel, w - 200));
    // Pijltje: tik voor details
    for (int d = -1; d <= 1; d++) {
      it.line(x + w - 30 + d, y + 20, x + w - 16 + d, y + DAG_REGEL / 2, ZWART);
      it.line(x + w - 16 + d, y + DAG_REGEL / 2, x + w - 30 + d, y + DAG_REGEL - 20, ZWART);
    }
    venster_vakken().push_back({x, y, w, DAG_REGEL, afspraak_zone(s, &a)});
    y += DAG_REGEL;
    it.horizontal_line(x, y, w, ZWART);
  }
}

inline void teken_details(Display &it, const Staat &s, const Fonts &f) {
  const Details &d = s.details;
  const int x = GROOT_X + 60, w = GROOT_B - 120;
  int y = GROOT_Y + 30;
  for (const auto &r : omloop(it, f.groot, d.titel, w, w, 2)) {
    tekst(it, x, y, f.groot, ZWART, WIT, TextAlign::TOP_LEFT, r);
    y += 66;
  }
  y += 6;
  for (const auto &r : omloop(it, f.kop, details_wanneer(d), w, w, 2)) {
    tekst(it, x, y, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, r);
    y += 48;
  }
  y += 14;
  vlak(it, x, y, w, 2, ZWART);
  y += 22;
  if (!d.geladen || !d.fout.empty()) {
    tekst(it, x, y, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, d.geladen ? d.fout : "Even ophalen…");
    return;
  }
  constexpr int REGEL = 38;
  auto veld = [&](const char *label, const std::string &waarde) {
    if (waarde.empty())
      return;
    const int lb = breedte(it, f.vet, label) + 14;
    const auto regels = omloop(it, f.normaal, waarde, w - lb, w - lb, 2);
    tekst(it, x, y, f.vet, ZWART, WIT, TextAlign::TOP_LEFT, label);
    for (const auto &r : regels) {
      tekst(it, x + lb, y, f.normaal, ZWART, WIT, TextAlign::TOP_LEFT, r);
      y += REGEL;
    }
    y += 8;
  };
  veld("Waar:", d.locatie);
  veld("Agenda:", d.kalender);
  if (!d.omschrijving.empty()) {
    y += 10;
    const int max = std::max(1, (GROOT_KNOP_Y - 30 - y) / REGEL);
    for (const auto &r : omloop(it, f.normaal, d.omschrijving, w, w, max)) {
      tekst(it, x, y, f.normaal, ZWART, WIT, TextAlign::TOP_LEFT, r);
      y += REGEL;
    }
  }
  if (d.locatie.empty() && d.kalender.empty() && d.omschrijving.empty())
    tekst(it, x, y, f.normaal, ZWART, WIT, TextAlign::TOP_LEFT, "Geen verdere details.");
}

inline void teken_groot_venster(Display &it, const Staat &s, const Fonts &f) {
  venster_vakken().clear();
  vlak(it, GROOT_X, GROOT_Y, GROOT_B, GROOT_H, WIT);
  kader(it, GROOT_X, GROOT_Y, GROOT_B, GROOT_H, 6, ZWART);
  if (s.status == DAG)
    teken_dagoverzicht(it, s, f);
  else
    teken_details(it, s, f);
  if (met_terug(s))
    venster_knop(it, f, terug_x(), "Terug", false, GROOT_KNOP_Y);
  venster_knop(it, f, sluiten_x(s), "Sluiten", true, GROOT_KNOP_Y);
}

inline void teken_venster(Display &it, const Staat &s, const Fonts &f) {
  if (s.status == DAG || s.status == DETAILS) {
    teken_groot_venster(it, s, f);
    return;
  }
  vlak(it, VENSTER_X, VENSTER_Y, VENSTER_B, VENSTER_H, WIT);
  kader(it, VENSTER_X, VENSTER_Y, VENSTER_B, VENSTER_H, 6, ZWART);
  const int x = VENSTER_X + 60;
  const int w = VENSTER_B - 120;
  int y = VENSTER_Y + 36;

  if (s.status == MELDING) {
    for (const auto &r : omloop(it, f.kop, s.melding, w, w, 6)) {
      tekst(it, x, y, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, r);
      y += 48;
    }
    venster_knop(it, f, ok_x(), "OK", true);
    return;
  }

  const Voorstel &v = s.voorstel;
  const bool notitie = v.soort == "notitie";
  tekst(it, x, y, f.vet, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT,
        notitie ? "Notitie toevoegen?" : "Nieuwe afspraak toevoegen?");
  y += 46;
  for (const auto &r : omloop(it, f.groot, v.titel, w, w, 2)) {
    tekst(it, x, y, f.groot, ZWART, WIT, TextAlign::TOP_LEFT, r);
    y += 66;
  }
  y += 4;
  if (!notitie) {
    Datum d;
    std::string wanneer = lees_datum(v.datum, d) ? datum_lang(d) : v.datum;
    if (v.begin.empty())
      wanneer += "  ·  hele dag";
    else
      wanneer += "  ·  " + v.begin + (v.eind.empty() ? "" : " – " + v.eind);
    tekst(it, x, y, f.kop, ZWART, WIT, TextAlign::TOP_LEFT, afkappen(it, f.kop, wanneer, w));
  } else {
    tekst(it, x, y, f.kop, ZWART, WIT, TextAlign::TOP_LEFT,
          v.datum.empty() ? "op de notitielijst" : "op de notitielijst  ·  vandaag");
  }
  y += 54;
  if (!v.gehoord.empty())
    tekst(it, x, y, f.normaal, GRIJS_DONKER, WIT, TextAlign::TOP_LEFT,
          afkappen(it, f.normaal, "Gehoord: \"" + v.gehoord + "\"", w));

  venster_knop(it, f, opslaan_x(), "Opslaan", true);
  venster_knop(it, f, annuleer_x(), "Annuleer", false);
}

// Hoofdfunctie, aangeroepen vanuit de display-lambda.
inline void teken(Display &it, const Staat &s, const Fonts &f, uint8_t mask) {
  iconen_lenen()[0] = {f.klein, f.normaal};
  iconen_lenen()[1] = {f.vet, f.normaal};
  auto &nep = nep_vette_fonts();
  nep[0] = f.nep_vet ? f.vet : nullptr;
  nep[1] = f.nep_vet ? f.kop : nullptr;
  nep[2] = f.nep_vet ? f.groot : nullptr;
  // GEBIED: alles opnieuw, maar alleen binnen de rechthoek (de rest van de
  // framebuffer klopt al), zodat alleen dat stuk knippert.
  Gebied g;
  if ((mask & GEBIED) && !(mask & VOL)) {
    g = ververs_gebied();
    if (g.leeg())
      mask &= ~GEBIED;
  }
  ververs_gebied() = {};
  if (mask & (VOL | GEBIED)) {
    if (mask & VOL) {
      it.fill(WIT);
    } else {
      it.start_clipping(g.x0, g.y0, g.x1, g.y1);
      vlak(it, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, WIT);
    }
    vakken().clear();
    teken_kop(it, s, f);
    if (s.scherm == SCHERM_MAAND) {
      teken_maand(it, s, f);
    } else {
      teken_week(it, s, f);
      teken_notities(it, s, f);
    }
    teken_balk(it, s, f);
    if (venster_open(s))
      teken_venster(it, s, f);
    if (!(mask & VOL))
      it.end_clipping();
    return;
  }
  if (mask & TABS)
    teken_tabs(it, s, f);
  if (mask & BALK)
    teken_balk(it, s, f);
  if ((mask & NOTITIES) && s.scherm == SCHERM_WEEK) {
    vlak(it, 0, NOTITIE_Y - 12, B, NOTITIE_EIND - NOTITIE_Y + 14, WIT);
    teken_notities(it, s, f);
    // Een open venster mag er niet door overschreven worden.
    if (venster_open(s))
      teken_venster(it, s, f);
  }
  if ((mask & VENSTER) && venster_open(s))
    teken_venster(it, s, f);
}

// ----------------------------------------------------------------- touch ---

inline bool binnen(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

inline Zone raak(const Staat &s, int x, int y) {
  if (s.slaap != WAKKER)
    return GEEN;  // eerst wakker worden
  if (s.status == DAG || s.status == DETAILS) {
    if (binnen(x, y, sluiten_x(s), GROOT_KNOP_Y, VENSTER_KNOP_B, VENSTER_KNOP_H))
      return OK;
    if (met_terug(s) && binnen(x, y, terug_x(), GROOT_KNOP_Y, VENSTER_KNOP_B, VENSTER_KNOP_H))
      return TERUG;
    if (s.status == DAG)
      for (const Vak &v : venster_vakken())
        if (binnen(x, y, v.x, v.y, v.w, v.h))
          return static_cast<Zone>(v.zone);
    if (binnen(x, y, GROOT_X, GROOT_Y, GROOT_B, GROOT_H))
      return GEEN;
  }
  if (venster_open(s) && s.status != DAG && s.status != DETAILS) {
    if (s.status == MELDING)
      return binnen(x, y, ok_x(), VENSTER_KNOP_Y, VENSTER_KNOP_B, VENSTER_KNOP_H) ? OK : GEEN;
    if (binnen(x, y, opslaan_x(), VENSTER_KNOP_Y, VENSTER_KNOP_B, VENSTER_KNOP_H))
      return OPSLAAN;
    if (binnen(x, y, annuleer_x(), VENSTER_KNOP_Y, VENSTER_KNOP_B, VENSTER_KNOP_H))
      return ANNULEER;
  }
  // Tabs en notities liggen onder een open venster; dan niet aanraakbaar.
  for (int i = 0; i < 2 && !venster_open(s); i++)
    if (binnen(x, y, tab_x(i), 0, TAB_B, KOP_H))
      return static_cast<Zone>(TAB_WEEK + i);
  if (s.scherm == SCHERM_WEEK && !venster_open(s)) {
    const int i = notitie_op(x, y);
    const int aantal = static_cast<int>(s.notities.size());
    if (i >= 0 && i < aantal && !(i == NOTITIE_MAX - 1 && aantal > NOTITIE_MAX))  // niet "+N meer"
      return s.notities[i].uid.empty() ? GEEN : static_cast<Zone>(NOTITIE0 + i);
  }
  // Afspraken (week) en dagen (maand). Net ernaast getikt? Dan de dichtstbijzijnde
  // afspraak in dezelfde kolom, tot 20 pixels verderop.
  if (!venster_open(s)) {
    const Vak *best = nullptr;
    int afstand = 21;
    for (const Vak &v : vakken()) {
      if (x < v.x || x >= v.x + v.w)
        continue;
      const int a = y < v.y ? v.y - y : (y >= v.y + v.h ? y - (v.y + v.h - 1) : 0);
      if (a < afstand) {
        afstand = a;
        best = &v;
      }
    }
    if (best != nullptr)
      return static_cast<Zone>(best->zone);
  }
  // Iets ruimere vlakken in de balk; vingers zijn geen stylus.
  if (binnen(x, y, MARGE - 10, KNOP_Y - 14, SPREEK_B + 18, KNOP_H + 28))
    return SPREEK;
  for (int i = 0; i < 4; i++)
    if (!s.knoppen[i].naam.empty() &&
        binnen(x, y, ha_knop_x(i) - 8, KNOP_Y - 14, HA_KNOP_B + KNOP_TUSSEN, KNOP_H + 28))
      return static_cast<Zone>(KNOP1 + i);
  return GEEN;
}

// ------------------------------------------------------------ spaarstand ---

// Vingerafdruk van wat er op het scherm staat (zonder de knoppenbalk). Een
// stille wekker-ronde ververst alleen als die verandert.
inline uint32_t inhoud_hash(const Staat &s) {
  uint32_t h = 2166136261u;  // FNV-1a
  auto voeg_toe = [&h](const std::string &t) {
    for (unsigned char c : t)
      h = (h ^ c) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
  };
  voeg_toe(s.ruw_afspraken);
  for (const auto &n : s.notities)  // zoals getoond, met de afgevinkte
    voeg_toe(n.tekst + (n.afgevinkt ? "|x" : "|"));
  voeg_toe(s.ruw_weer);
  char buf[32];
  const int accu_stap = std::isnan(s.accu) ? -2 : (s.accu <= ACCU_LAAG ? -1 : static_cast<int>(s.accu) / 10);
  std::snprintf(buf, sizeof(buf), "%d-%d-%d/%d/%d", s.vandaag.j, s.vandaag.m, s.vandaag.d, accu_stap,
                s.verbonden ? 1 : 0);
  voeg_toe(buf);
  return h;
}

// Waar het venster staat (om bij het sluiten alleen dat stuk te verversen).
inline Gebied venster_gebied(const Staat &s) {
  if (s.status == DAG || s.status == DETAILS)
    return {GROOT_X, GROOT_Y, GROOT_X + GROOT_B, GROOT_Y + GROOT_H};
  return {VENSTER_X, VENSTER_Y, VENSTER_X + VENSTER_B, VENSTER_Y + VENSTER_H};
}

inline void ververs_ook(const Gebied &g) { ververs_ook(g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0); }

// Per dag van de getoonde week een hash van wat er in die kolom staat.
inline std::array<uint32_t, 7> week_hashes(const Staat &s) {
  std::array<uint32_t, 7> uit{};
  const Datum maandag = plus_dagen(s.vandaag, -weekdag(s.vandaag));
  auto voeg_toe = [](uint32_t &h, const std::string &t) {
    for (unsigned char c : t)
      h = (h ^ c) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
  };
  for (int i = 0; i < 7; i++) {
    const Datum dag = plus_dagen(maandag, i);
    uint32_t h = 2166136261u;
    for (const auto &a : s.afspraken)
      if (a.datum == dag) {
        voeg_toe(h, a.tijd);
        voeg_toe(h, a.titel);
      }
    for (const auto &w : s.weer)
      if (w.datum == dag) {
        voeg_toe(h, std::to_string(static_cast<int>(w.icoon)));
        voeg_toe(h, w.max);
        voeg_toe(h, w.min);
      }
    uit[i] = h;
  }
  return uit;
}

// Nieuwe agenda/notities binnen terwijl het scherm wakker is: alleen de
// veranderde dagkolommen en/of de notities verversen. Een andere dag, het
// maandscherm of iets onverwachts: alles.
inline void ververs_verschil(const Staat &s, const Datum &oude_dag, const std::array<uint32_t, 7> &voor,
                             bool notities_anders) {
  if (s.scherm != SCHERM_WEEK || !(s.vandaag == oude_dag)) {
    ververs_mask() |= VOL;
    return;
  }
  const auto na = week_hashes(s);
  for (int i = 0; i < 7; i++)
    if (na[i] != voor[i])
      ververs_ook(MARGE + i * WEEK_KOL - 5, WEEK_Y, WEEK_KOL, WEEK_EIND - WEEK_Y);
  if (notities_anders)
    ververs_ook(0, NOTITIE_Y - 12, B, NOTITIE_EIND - NOTITIE_Y + 14);
}

// Slaapduur tot de volgende stille ronde: elke `elke_min` minuten, maar 's nachts
// (van `nacht_van` tot `nacht_tot` uur) in één keer door tot de ochtend.
inline uint32_t slaapduur_ms(int uur, int minuut, int elke_min, int nacht_van, int nacht_tot) {
  const bool nacht = nacht_van > nacht_tot ? (uur >= nacht_van || uur < nacht_tot)
                                           : (uur >= nacht_van && uur < nacht_tot);
  if (uur < 0 || !nacht)
    return static_cast<uint32_t>(elke_min) * 60u * 1000u;
  int tot_ochtend = (nacht_tot * 60) - (uur * 60 + minuut);
  if (tot_ochtend <= 0)
    tot_ochtend += 24 * 60;
  return static_cast<uint32_t>(tot_ochtend) * 60u * 1000u;
}

// Het weer op het scherm. Home Assistant stuurt elke ronde de nieuwste
// verwachting, maar het scherm neemt die alleen over in een nieuw tijdvak van
// `elke_uur` uur (bij 6: om 0, 6, 12 en 18 uur), anders knippert het voor elke
// graad die de verwachting verschuift.
struct WeerGeheugen {
  char tekst[400];
  int vak;  // tijdvak waarin het overgenomen is, zie weer_vak()
};

// Tijdvak: per dag 0..(24/elke_uur), een nieuwe dag is altijd een nieuw vak.
// -1 als de tijd onbekend is.
inline int weer_vak(int dag, int uur, int elke_uur) {
  if (dag < 0 || uur < 0)
    return -1;
  return dag * 100 + uur / std::max(1, elke_uur);
}

// Home Assistant stuurt alleen vandaag en later. Voor de dagen van deze week
// die al voorbij zijn, houden we de regels uit het oude weer: de laatste
// verwachting voor die dag. In een nieuwe week begint het opnieuw.
inline std::string met_verleden(const std::string &oud, const std::string &nieuw, int vandaag) {
  if (nieuw.empty() || vandaag < 0)
    return nieuw;
  const int maandag = vandaag - weekdag(van_dagnummer(vandaag));
  std::string verleden;
  for (const auto &regel : lees_regels(oud)) {
    Datum d;
    if (!lees_datum(regel, d))
      continue;
    const int n = dagnummer(d);
    if (n >= maandag && n < vandaag && nieuw.find(regel.substr(0, 10)) == std::string::npos)
      verleden += regel + "\n";
  }
  return verleden + nieuw;
}

// Neemt `nieuw` over als er nog geen weer staat of als het een nieuw tijdvak
// is (en houdt de voorbije dagen van deze week). Geeft true als het getoonde
// weer verandert.
// `altijd`: ook binnen hetzelfde tijdvak, omdat het scherm toch al ververst
// (de agenda of de notities zijn veranderd).
inline bool neem_weer_over(WeerGeheugen &g, const std::string &nieuw, int vak, bool altijd = false) {
  std::string t = met_verleden(g.tekst, nieuw, vak >= 0 ? vak / 100 : -1);
  if (t.size() >= sizeof(g.tekst)) {
    t.resize(sizeof(g.tekst) - 1);
    t.erase(t.rfind('\n') == std::string::npos ? 0 : t.rfind('\n'));  // geen halve regel
  }
  if (t == g.tekst)
    return false;
  if (!altijd && g.tekst[0] != '\0' && !t.empty() && vak >= 0 && vak == g.vak)
    return false;  // in dit tijdvak al overgenomen
  std::snprintf(g.tekst, sizeof(g.tekst), "%s", t.c_str());
  g.vak = vak;
  return true;
}

// Op het scherm afgevinkte notities blijven nog even doorgestreept staan, ook
// als Home Assistant ze niet meer meestuurt. Zo zie je wat je net gedaan hebt
// en kun je een verkeerde tik terugdraaien. Notities die in de HA-app worden
// afgevinkt, verdwijnen meteen.
struct KlaarNotitie {
  char lijst[48];
  char uid[48];
  char tekst[96];
  int64_t sinds;  // epoch-seconden; 0 = tijd onbekend
  int16_t plek;   // positie in de lijst toen hij afgevinkt werd
  bool gebruikt;
};

struct KlaarGeheugen {
  KlaarNotitie n[8];
};

inline void kopieer(char *doel, size_t grootte, const std::string &bron) {
  std::string s = bron;
  while (s.size() >= grootte)
    s.erase(utf8_terug(s, s.size()));  // niet midden in een teken afkappen
  std::snprintf(doel, grootte, "%s", s.c_str());
}

inline bool is_notitie(const KlaarNotitie &k, const Notitie &n) {
  return k.gebruikt && n.lijst == k.lijst && n.uid == k.uid;
}

inline void onthoud_klaar(KlaarGeheugen &g, const Notitie &n, int plek, int64_t nu) {
  // Hetzelfde vak als hij er al in staat, anders een leeg vak, anders het oudste.
  KlaarNotitie *vak = nullptr;
  for (auto &k : g.n)
    if (is_notitie(k, n))
      vak = &k;
  for (auto &k : g.n)
    if (!vak && !k.gebruikt)
      vak = &k;
  if (!vak) {
    vak = &g.n[0];
    for (auto &k : g.n)
      if (k.sinds < vak->sinds)
        vak = &k;
  }
  kopieer(vak->lijst, sizeof(vak->lijst), n.lijst);
  kopieer(vak->uid, sizeof(vak->uid), n.uid);
  kopieer(vak->tekst, sizeof(vak->tekst), n.tekst);
  vak->sinds = nu;
  vak->plek = static_cast<int16_t>(plek);
  vak->gebruikt = true;
}

inline void vergeet_klaar(KlaarGeheugen &g, const Notitie &n) {
  for (auto &k : g.n)
    if (is_notitie(k, n))
      k.gebruikt = false;
}

// De notities van Home Assistant plus de afgevinkte die nog `klaar_min`
// minuten getoond worden, op hun oude plek. Ruimt verlopen notities op.
inline std::vector<Notitie> met_klaar(std::vector<Notitie> lijst, KlaarGeheugen &g, int64_t nu, int klaar_min) {
  for (auto &k : g.n)
    if (k.gebruikt && nu > 0 && k.sinds > 0 && nu - k.sinds >= static_cast<int64_t>(klaar_min) * 60)
      k.gebruikt = false;
  std::vector<const KlaarNotitie *> klaar;
  for (const auto &k : g.n)
    if (k.gebruikt)
      klaar.push_back(&k);
  std::sort(klaar.begin(), klaar.end(), [](const KlaarNotitie *a, const KlaarNotitie *b) { return a->plek < b->plek; });
  for (const KlaarNotitie *k : klaar) {
    auto al = std::find_if(lijst.begin(), lijst.end(), [k](const Notitie &n) { return is_notitie(*k, n); });
    if (al != lijst.end()) {
      al->afgevinkt = true;  // HA heeft hem (nog) als open
      continue;
    }
    Notitie n{k->lijst, k->uid, k->tekst, true};
    lijst.insert(lijst.begin() + std::min<size_t>(std::max<int>(k->plek, 0), lijst.size()), n);
  }
  return lijst;
}

inline bool zelfde_notities(const std::vector<Notitie> &a, const std::vector<Notitie> &b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Notitie &x, const Notitie &y) {
    return x.lijst == y.lijst && x.uid == y.uid && x.tekst == y.tekst && x.afgevinkt == y.afgevinkt;
  });
}

#ifndef INK_HOST
// Blijven bewaard tijdens deep sleep (RTC-geheugen), niet na stroomverlies.
// Gedefinieerd in ink_kalender.cpp: dit bestand komt in elk bronbestand.
extern WeerGeheugen rtc_weer;
extern KlaarGeheugen rtc_klaar;
extern uint32_t rtc_inhoud_hash;
extern time_t rtc_slaap_begin;
extern uint8_t rtc_snelle_touch_wekkers;
// Laatst gemeten accupercentage, zodat het na het wekken meteen weer op het
// scherm staat (de eerste meting volgt pas een paar seconden later).
extern float rtc_accu;

enum Wekreden : int { WEK_STROOM = 0, WEK_TIMER = 1, WEK_GEBRUIKER = 2 };

constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_2;
constexpr gpio_num_t PIN_KNOP_GROEN = GPIO_NUM_3;
constexpr gpio_num_t KNOPPEN[] = {GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5};
// Uitgangen die tijdens de slaap laag moeten (zie README, Spaarstand).
// RTC-pinnen (<= GPIO21) kunnen los vastgezet worden ...
constexpr gpio_num_t UIT_IN_SLAAP_RTC[] = {GPIO_NUM_11, GPIO_NUM_21};
// ... de rest alleen met gpio_deep_sleep_hold_en(), en dat breekt ext0 (touch-wekken).
constexpr gpio_num_t UIT_IN_SLAAP_DIGITAAL[] = {GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_45};
constexpr gpio_num_t PIN_TOUCH_RESET = GPIO_NUM_48;

// GT911-registers voor de gebarenmodus (zoals Seeeds eigen SenseCraft-firmware):
// alleen in die modus geeft de touch-chip tijdens deep sleep een tik door.
constexpr uint16_t GT911_COMMAND = 0x8040;
constexpr uint16_t GT911_COMMAND2 = 0x8046;
constexpr uint8_t GT911_GEBARENMODUS = 0x08;

inline Wekreden wekreden() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
      return WEK_TIMER;
    case ESP_SLEEP_WAKEUP_EXT0:  // touch
    case ESP_SLEEP_WAKEUP_EXT1:  // knoppen
      return WEK_GEBRUIKER;
    default:
      return WEK_STROOM;
  }
}

inline bool gewekt_door(gpio_num_t pin) {
  const auto oorzaak = esp_sleep_get_wakeup_cause();
  if (pin == PIN_TOUCH_INT)
    return oorzaak == ESP_SLEEP_WAKEUP_EXT0;
  return oorzaak == ESP_SLEEP_WAKEUP_EXT1 && (esp_sleep_get_ext1_wakeup_status() & (1ULL << pin));
}

// Direct na het opstarten: pinnen weer vrijgeven en bijhouden of de touch-chip
// ons steeds meteen wakker maakt (dan touch-wekken uitzetten).
inline void na_wakker_worden() {
  for (gpio_num_t pin : UIT_IN_SLAAP_RTC)
    gpio_hold_dis(pin);
  for (gpio_num_t pin : UIT_IN_SLAAP_DIGITAAL)
    gpio_hold_dis(pin);
  gpio_hold_dis(PIN_TOUCH_RESET);
  gpio_deep_sleep_hold_dis();
  // De touch-interrupt stond als RTC-ingang voor ext0; terug naar een gewone pin.
  rtc_gpio_pulldown_dis(PIN_TOUCH_INT);
  rtc_gpio_deinit(PIN_TOUCH_INT);
  if (gewekt_door(PIN_TOUCH_INT) && rtc_slaap_begin != 0 && std::time(nullptr) - rtc_slaap_begin < 5) {
    if (rtc_snelle_touch_wekkers < 255)
      rtc_snelle_touch_wekkers++;
  } else if (wekreden() == WEK_GEBRUIKER && !gewekt_door(PIN_TOUCH_INT)) {
    rtc_snelle_touch_wekkers = 0;  // met een knop gewekt: touch opnieuw proberen
  }
}

inline bool touch_wekken_actief(bool ingesteld) { return ingesteld && rtc_snelle_touch_wekkers < 3; }

inline bool gt911_schrijf(esphome::i2c::I2CBus *bus, uint8_t adres, uint16_t reg, uint8_t waarde) {
  const uint8_t data[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF), waarde};
  return bus->write(adres, data, sizeof(data)) == esphome::i2c::ERROR_OK;
}

// Vlak voor deep sleep: randapparatuur uit, wekbronnen instellen.
//
// Met touch-wekken werkt het zoals in Seeeds SenseCraft-firmware: GT911 in
// gebarenmodus, de INT-pin wordt hoog bij aanraking en wekt via ext0. Omdat
// gpio_deep_sleep_hold_en() ext0 breekt, worden dan alleen de RTC-pinnen
// vastgezet (iets meer slaapstroom). Zonder touch-wekken gaat alles vast uit.
inline void bereid_slaap_voor(bool touch_wekt, esphome::i2c::I2CBus *bus, uint8_t touch_adres) {
  bool touch = touch_wekken_actief(touch_wekt);
  if (touch) {
    const bool ok = gt911_schrijf(bus, touch_adres, GT911_COMMAND2, GT911_GEBARENMODUS);
    esphome::delay(1);
    touch = gt911_schrijf(bus, touch_adres, GT911_COMMAND, GT911_GEBARENMODUS) && ok;
    esphome::delay(10);
    if (!touch) {
      ESP_LOGW("ink", "Touch-chip niet in gebarenmodus gekregen; alleen wekken met de knoppen");
    }
  }

  for (gpio_num_t pin : UIT_IN_SLAAP_RTC) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
  }
  for (gpio_num_t pin : UIT_IN_SLAAP_DIGITAAL) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    if (!touch)
      gpio_hold_en(pin);
  }
  if (!touch) {
    // Touch-chip uit (reset laag) en alles vastzetten
    gpio_set_direction(PIN_TOUCH_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_TOUCH_RESET, 0);
    gpio_hold_en(PIN_TOUCH_RESET);
    gpio_deep_sleep_hold_en();
  }

  // Knoppen: actief laag, via ext1
  uint64_t masker = 0;
  for (gpio_num_t pin : KNOPPEN) {
    masker |= 1ULL << pin;
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
  }
  esp_sleep_enable_ext1_wakeup(masker, ESP_EXT1_WAKEUP_ANY_LOW);

  // Touch: actief hoog, via ext0. Eerst wachten tot de INT-pin rustig (laag) is.
  if (touch) {
    rtc_gpio_init(PIN_TOUCH_INT);
    rtc_gpio_set_direction(PIN_TOUCH_INT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(PIN_TOUCH_INT);
    rtc_gpio_pulldown_en(PIN_TOUCH_INT);
    const uint32_t start = esphome::millis();
    while (rtc_gpio_get_level(PIN_TOUCH_INT) != 0 && esphome::millis() - start < 300)
      esphome::delay(5);
    if (rtc_gpio_get_level(PIN_TOUCH_INT) != 0) {
      ESP_LOGW("ink", "Touch-INT nog hoog; het scherm wordt mogelijk meteen weer wakker");
    }
    esp_sleep_enable_ext0_wakeup(PIN_TOUCH_INT, 1);
  }

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_slaap_begin = std::time(nullptr);
}
#endif

}  // namespace ink
