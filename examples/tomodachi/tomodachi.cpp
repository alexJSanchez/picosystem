// PicoSystem Tomodachi: a skater pal (Pip) to look after.
//
// Six screens, matching the design concepts:
//   ROOM    - home: stats, action bar (FEED / SKATE / DECK / SLEEP / SHOP)
//   FEED    - snack menu
//   RUN     - side scrolling street run mini-game
//   CREATE  - skater creator (face, cap, shirt, board, colour)
//   TOWN    - top-down skate park map with other skaters
//   MEET    - crew meet-up dialogue
//
// All art is drawn with primitives so there are no asset files to load.
// Runs at the native 240x240 resolution.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "picosystem.hpp"

using namespace picosystem;

enum screen_t { ROOM, FEED, RUN, CREATE, TOWN, MEET, HUB, TRICK, JUMP, HILL, JOUST };
screen_t scr = ROOM;

void run_start();
void hub_enter(screen_t from);
void meet_start();

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// set pen from a 12-bit 0xRGB colour (4 bits per channel, like the screen)
void P(uint16_t c, uint8_t a = 15) {
  pen((c >> 8) & 15, (c >> 4) & 15, c & 15, a);
}

void T(const std::string_view &s, int32_t x, int32_t y, uint16_t c) {
  P(c);
  text(s, x, y);
}

// text with a 1px drop shadow
void TS(const std::string_view &s, int32_t x, int32_t y, uint16_t c, uint16_t sh = 0x112) {
  P(sh); text(s, x + 1, y + 1);
  P(c);  text(s, x, y);
}

// centred text
void TC(const std::string_view &s, int32_t cx, int32_t y, uint16_t c, bool shadow = false) {
  int32_t w, h;
  measure(s, w, h);
  if(shadow) TS(s, cx - w / 2, y, c); else T(s, cx - w / 2, y, c);
}

int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

int32_t wrapi(int32_t v, int32_t n) {
  return ((v % n) + n) % n;
}

std::string pad2(int32_t v) {
  return (v < 10 ? "0" : "") + str(v);
}

// ---------------------------------------------------------------------------
// sound
// ---------------------------------------------------------------------------

voice_t v_blip, v_low, v_happy;

void blip(uint32_t f, uint32_t ms = 60) { play(v_blip, f, ms, 60); }
void thud()                             { play(v_low, 110, 150, 80); }
void jingle(uint32_t f)                 { play(v_happy, f, 140, 70); }

// ---------------------------------------------------------------------------
// the pal
// ---------------------------------------------------------------------------

constexpr int32_t N_FACE = 8, N_CAP = 6, N_SHIRT = 9, N_BOARD = 12, N_COLOR = 8;

struct look_t {
  int32_t face, cap, shirt, board, skin;
};

// 12-bit 0xRGB palettes
const uint16_t SKIN[N_COLOR]    = {0xFC9, 0xEB8, 0xD96, 0xB74, 0x963, 0x742, 0xAD9, 0xBBF};
const uint16_t CAPCOL[N_CAP]    = {0xE34, 0x36E, 0x3C5, 0xFC3, 0xFFF, 0x333};
const uint16_t SHIRTCOL[N_SHIRT]= {0xFFF, 0x333, 0xE34, 0x36E, 0x3C5, 0xFC3, 0xF8C, 0xF83, 0x84C};
const uint16_t DECK[N_BOARD]    = {0xD84, 0x36E, 0xE34, 0x3C5, 0xFC3, 0xF8C, 0x333, 0xFFF, 0x84C, 0x2BB, 0xF83, 0x69F};
const uint16_t STRIPE[N_BOARD]  = {0xFFF, 0xFC3, 0xFFF, 0x252, 0xE34, 0xFFF, 0xF3B, 0x3CF, 0xFC3, 0xFFF, 0x333, 0xFFF};

struct pal_t {
  look_t look;
  int32_t hunger;   // how full Pip is, 0..100
  int32_t energy;   // 0..100
  int32_t mood;     // 0..100
  int32_t coins;
  int32_t hearts;   // crew hearts
  int32_t best_trick;   // best trick attack score
  int32_t best_height;  // best sky pop height, in tenths of a foot
  int32_t best_run;     // best street run score
  int32_t best_hill;    // best hill bomb time in frames (0 = none yet)
  int32_t best_jam;     // best hill jam score
  int32_t joust_wins;   // pool joust wins (each win unlocks the next rival)
  bool    sleeping;
} pal;

// everything else the screens share
uint32_t now_f = 0;           // frame counter
std::string msg;              // toast message
int32_t msg_t = 0;

void say(const std::string &m, int32_t frames = 80) { msg = m; msg_t = frames; }

int32_t level() { return 1 + pal.hearts / 3; }

// ---------------------------------------------------------------------------
// juice: screen shake, hit-stop, slapstick bail debris, punk bassline
// ---------------------------------------------------------------------------

int32_t shake_t = 0, shake_mag = 0, freeze_t = 0;
void shake(int32_t mag, int32_t frames) { shake_mag = mag; shake_t = frames; }
void hit_stop(int32_t frames) { freeze_t = std::max<int32_t>(freeze_t, frames); }

float frand() { return (std::rand() % 1000) / 1000.0f; }

voice_t v_noise, v_bass;
void crash() { play(v_noise, 120, 220, 85); }

struct part_t { float x, y, vx, vy, rot, vr; int32_t life, kind; uint16_t col; };
std::vector<part_t> parts;      // kind 0: spinning star, 1: flying board, 2: spark

// a wipeout: the board spins off and stars burst out
void spawn_bail(float x, float y, uint16_t board_col) {
  parts.push_back({x, y - 6, (frand() - 0.5f) * 3.0f, -2.6f - frand() * 1.4f, 0.0f, 0.5f, 55, 1, board_col});
  for(int32_t i = 0; i < 5; i++) {
    float a = i * 1.2566f + frand() * 0.5f;
    parts.push_back({x, y - 10, std::cos(a) * 1.7f, std::sin(a) * 1.7f - 1.4f, 0.0f, 0.3f, 40, 0, 0xFD4});
  }
}

void spawn_sparks(float x, float y, uint16_t col) {
  for(int32_t i = 0; i < 6; i++) {
    float a = frand() * 6.2832f, s = 0.8f + frand() * 1.6f;
    parts.push_back({x, y, std::cos(a) * s, std::sin(a) * s - 0.8f, 0.0f, 0.0f, 18, 2, col});
  }
}

void update_parts() {
  for(auto &p : parts) {
    p.x += p.vx; p.y += p.vy; p.vy += 0.11f; p.rot += p.vr; p.life--;
  }
  parts.erase(std::remove_if(parts.begin(), parts.end(), [](const part_t &p) { return p.life <= 0; }), parts.end());
}

void draw_parts() {
  for(auto &p : parts) {
    int32_t x = (int32_t)p.x, y = (int32_t)p.y;
    if(p.kind == 0) {
      P(((p.life / 3) % 2) ? 0xFFF : 0xFD4);
      frect(x - 1, y - 3, 2, 6); frect(x - 3, y - 1, 6, 2);
    } else if(p.kind == 1) {
      int32_t dx = (int32_t)(std::cos(p.rot) * 9), dy = (int32_t)(std::sin(p.rot) * 9);
      P(0x112); line(x - dx, y - dy + 1, x + dx, y + dy + 1); line(x - dx, y - dy - 1, x + dx, y + dy - 1);
      P(p.col); line(x - dx, y - dy, x + dx, y + dy);
      P(0xFFF); frect(x - dx, y - dy, 2, 2);
    } else {
      P(p.col); frect(x, y, 2, 2);
    }
  }
}

// three stars circling a dazed head
void dizzy(int32_t x, int32_t y) {
  for(int32_t k = 0; k < 3; k++) {
    float a = now_f * 0.28f + k * 2.094f;
    int32_t sx = x + (int32_t)(std::cos(a) * 10), sy = y + (int32_t)(std::sin(a) * 3);
    P(((now_f / 4 + k) % 2) ? 0xFFF : 0xFD4);
    frect(sx - 1, sy - 2, 2, 4); frect(sx - 2, sy - 1, 4, 2);
  }
}

// ---- rhythmic pumping: alternate LEFT and RIGHT --------------------------
// +1 = a good alternating press, -1 = the same side twice (loses speed), 0 = nothing
int32_t pump_last = 0;
int32_t pump_press() {
  int32_t d = pressed(LEFT) ? -1 : (pressed(RIGHT) ? 1 : 0);
  if(!d) return 0;
  if(pump_last == 0 || d != pump_last) { pump_last = d; return 1; }
  return -1;
}

// ---- punk bassline on the piezo: only when no effect is playing -----------
bool music_on = true;
const uint16_t BASS[16] = {164, 164, 196, 164, 220, 164, 246, 220,
                           164, 164, 196, 164, 220, 196, 164, 0};
void music_step(uint32_t tick) {
  if(!music_on || pal.sleeping || tick % 7 != 0 || playing()) return;
  uint16_t f = BASS[(tick / 7) % 16];
  if(f) play(v_bass, f, 70, 26);
}


// Draws a skater standing on a board. (cx, by) = board bottom centre, u = pixel unit.
//   emo: -1 sad, 0 face default, 1 mouth wide open (eating)
//   fl:  >0 while the board is mid kickflip
void draw_pip(const look_t &l, int32_t cx, int32_t by, int32_t u, int32_t emo = 0, int32_t fl = 0) {
  uint16_t deck = DECK[l.board], stripe = STRIPE[l.board];
  const int32_t hu = std::max<int32_t>(1, u / 2);   // half unit, at least 1px

  // board
  int32_t dy = 0;
  if(fl > 0) dy = u;                           // board pops up a little
  P(0xCCD);
  frect(cx - 6 * u, by - u, 2 * u, u);         // wheels
  frect(cx + 4 * u, by - u, 2 * u, u);
  if(fl > 0 && (fl / 3) % 2) {
    P(deck); frect(cx - 7 * u, by - 2 * u - dy, 14 * u, hu);
  } else {
    P(deck); frect(cx - 7 * u, by - 2 * u - dy, 14 * u, u);
    P(stripe); frect(cx - 3 * u, by - 2 * u - dy, 6 * u, u);
    P(deck);
    frect(cx - 8 * u, by - 3 * u - dy, u, u);  // kicktails
    frect(cx + 7 * u, by - 3 * u - dy, u, u);
  }

  // legs and shoes
  P(0x335);
  frect(cx - 3 * u, by - 8 * u, 2 * u, 5 * u);
  frect(cx + 1 * u, by - 8 * u, 2 * u, 5 * u);
  P(0xFFF);
  frect(cx - 4 * u, by - 3 * u, 3 * u, u);
  frect(cx + 1 * u, by - 3 * u, 3 * u, u);

  // torso and arms
  P(SHIRTCOL[l.shirt]);
  frect(cx - 3 * u, by - 14 * u, 6 * u, 6 * u);
  P(SKIN[l.skin]);
  frect(cx - 5 * u, by - 13 * u, 2 * u, 4 * u);
  frect(cx + 3 * u, by - 13 * u, 2 * u, 4 * u);

  // head
  P(SKIN[l.skin]);
  frect(cx - 3 * u, by - 21 * u, 6 * u, 7 * u);

  // cap: odd caps are worn backwards
  P(CAPCOL[l.cap]);
  frect(cx - 3 * u, by - 23 * u, 6 * u, 3 * u);
  if(l.cap % 2 == 0) frect(cx + 3 * u, by - 21 * u, 3 * u, u);
  else               frect(cx - 6 * u, by - 21 * u, 3 * u, u);

  // face
  int32_t ey = by - 18 * u;     // eye row
  int32_t my = by - 16 * u;     // mouth row
  P(0x112);
  switch(l.face) {
    case 6:  // shades
      frect(cx - 3 * u, ey - u, 6 * u, 2 * u);
      break;
    case 4:  // wink
      frect(cx - 2 * u, ey, u, u);
      frect(cx + u, ey + u / 2, u, hu);
      break;
    case 5:  // big eyes
      frect(cx - 2 * u, ey - u / 2, u, 2 * u);
      frect(cx + u, ey - u / 2, u, 2 * u);
      break;
    case 7:  // sleepy
      frect(cx - 2 * u, ey + u / 2, u, hu);
      frect(cx + u, ey + u / 2, u, hu);
      break;
    default:
      frect(cx - 2 * u, ey, u, u);
      frect(cx + u, ey, u, u);
  }

  if(emo == 1) {
    P(0xE34); frect(cx - u, my, 2 * u, 2 * u);
  } else if(emo == -1) {
    P(0x112);
    frect(cx - u, my, 2 * u, u);
    frect(cx - 2 * u, my + u, u, u);
    frect(cx + u, my + u, u, u);
  } else {
    switch(l.face) {
      case 1:  // big grin
        P(0xFFF); frect(cx - 2 * u, my, 4 * u, u);
        P(0x112); frect(cx - 2 * u, my + u, 4 * u, hu);
        break;
      case 2:  // flat
        P(0x112); frect(cx - u, my + u / 2, 2 * u, hu);
        break;
      case 3:  // tongue
        P(0x112); frect(cx - u, my, 2 * u, u);
        P(0xF68); frect(cx, my + u, u, u);
        break;
      case 5:  // surprised
        P(0x112); frect(cx - u / 2, my, u, u + u / 2);
        break;
      default: // smile
        P(0x112);
        frect(cx - u, my + u / 2, 2 * u, hu);
        frect(cx - 2 * u, my, u, hu);
        frect(cx + u, my, u, hu);
    }
  }
}

// KAI, the friend in town / meet-up
const look_t KAI = {5, 1, 5, 9, 4};

// ---------------------------------------------------------------------------
// shared UI
// ---------------------------------------------------------------------------

void bar(int32_t x, int32_t y, int32_t w, int32_t val, uint16_t col) {
  P(0x112); frect(x, y, w, 6);
  P(0x445); frect(x + 1, y + 1, w - 2, 4);
  int32_t fw = ((w - 2) * clampi(val, 0, 100)) / 100;
  P(col);   frect(x + 1, y + 1, fw, 4);
}

void clock_text(int32_t x, int32_t y) {
  int32_t mins = 10 * 60 + 24 + (int32_t)(time() / 60000);
  T(pad2((mins / 60) % 24) + ":" + pad2(mins % 60), x, y, 0xFFF);
}

void top_bar(const std::string &title, bool stats) {
  P(0x214); frect(0, 0, SCREEN_WIDTH, stats ? 34 : 22);
  P(0x112); hline(0, stats ? 34 : 22, SCREEN_WIDTH);
  T(title, 6, 4, 0xFD4);
  clock_text(192, 4);
  if(stats) {
    T("MOOD", 6, 22, 0xFFF);   bar(36, 23, 44, pal.mood, 0xF59);
    T("FULL", 86, 22, 0xFFF);  bar(116, 23, 44, pal.hunger, 0xFC3);
    T("NRG", 166, 22, 0xFFF);  bar(190, 23, 44, pal.energy, 0x3CF);
  }
}

void bottom_bar(const std::string &a, const std::string &b, const std::string &c = "") {
  P(0x214); frect(0, 224, SCREEN_WIDTH, 16);
  P(0x112); hline(0, 224, SCREEN_WIDTH);
  T(a, 6, 228, 0xFFF);
  T(b, 90, 228, 0xFFF);
  T(c, 170, 228, 0xFD4);
}

// speech toast near the bottom of the playfield
void draw_msg(int32_t y) {
  if(msg_t <= 0) return;
  int32_t w, h;
  measure(msg, w, h);
  int32_t x = (SCREEN_WIDTH - w) / 2;
  P(0x112); frect(x - 6, y - 4, w + 12, h + 8);
  P(0xFFF); rect(x - 6, y - 4, w + 12, h + 8);
  T(msg, x, y, 0xFFF);
}

// ---------------------------------------------------------------------------
// ROOM
// ---------------------------------------------------------------------------

int32_t room_sel = 0;     // 0..4
constexpr int32_t N_ACT = 5;
const char *ACT_NAME[N_ACT] = {"FEED", "SKATE", "DECK", "SLEEP", "PARK"};

void draw_icon(int32_t i, int32_t cx, int32_t cy, uint16_t col) {
  P(col);
  switch(i) {
    case 0:  // pizza slice
      fpoly({cx - 6, cy - 5, cx + 6, cy - 5, cx, cy + 6});
      P(0xE34); fcircle(cx - 1, cy - 2, 1); fcircle(cx + 2, cy - 3, 1);
      break;
    case 1:  // board
      frect(cx - 8, cy, 16, 3); frect(cx - 6, cy + 3, 3, 2); frect(cx + 4, cy + 3, 3, 2);
      break;
    case 2:  // spray can / deck rack
      frect(cx - 3, cy - 6, 6, 11); P(0x112); frect(cx - 2, cy - 2, 4, 2);
      break;
    case 3:  // Z
      hline(cx - 4, cy - 5, 9); line(cx + 4, cy - 5, cx - 4, cy + 5); hline(cx - 4, cy + 5, 9);
      break;
    case 4:  // flag
      frect(cx - 6, cy - 8, 2, 16); fpoly({cx - 4, cy - 8, cx + 8, cy - 5, cx - 4, cy - 2});
      break;
  }
}

void draw_room() {
  // wall + floor
  P(0x426); frect(0, 34, SCREEN_WIDTH, 116);
  P(0x315); for(int32_t x = 0; x < SCREEN_WIDTH; x += 20) vline(x, 34, 116);
  P(0x853); frect(0, 150, SCREEN_WIDTH, 54);
  P(0x642); for(int32_t y = 158; y < 204; y += 12) hline(0, y, SCREEN_WIDTH);
  P(0x112); hline(0, 150, SCREEN_WIDTH);

  // posters
  P(0xFD8); frect(14, 46, 30, 40);
  P(0xE34); frect(18, 50, 22, 14); P(0x36E); frect(18, 68, 22, 14);
  P(0x3C5); frect(60, 52, 24, 30);
  P(0x112); frect(64, 56, 16, 22); P(0xFC3); fcircle(72, 67, 5);

  // neon sign
  P(0xF3B, 6); frect(100, 42, 54, 24);
  P(0xF3B); rect(104, 46, 46, 16);
  TC("SK8", 127, 50, 0xFBE);

  // deck rack on the right wall
  P(0x642); frect(186, 46, 40, 6);
  for(int32_t i = 0; i < 4; i++) {
    P(DECK[(i * 3) % N_BOARD]); frect(190 + i * 9, 52, 7, 30);
    P(STRIPE[(i * 3) % N_BOARD]); frect(190 + i * 9, 62, 7, 4);
  }

  // mini ramp (back right)
  P(0x765); fpoly({168, 150, 236, 150, 236, 118, 224, 124, 200, 138});
  P(0x112); line(168, 150, 200, 138); line(200, 138, 224, 124); line(224, 124, 236, 118);

  // Pip
  int32_t bob = pal.sleeping ? 0 : (((now_f / 20) % 2) ? 1 : 0);
  int32_t emo = (pal.mood < 30 || pal.hunger < 15) ? -1 : 0;
  look_t l = pal.look;
  if(pal.sleeping) l.face = 7;
  draw_pip(l, 120, 190 - bob, 3, emo);

  if(pal.sleeping) {
    P(0x002, 7); frect(0, 34, SCREEN_WIDTH, 170);
    int32_t zf = (now_f / 20) % 3;
    for(int32_t i = 0; i <= zf; i++) TS("Z", 150 + i * 10, 120 - i * 12, 0xFFF);
  }

  // action bar
  P(0x214); frect(0, 204, SCREEN_WIDTH, 36);
  P(0x112); hline(0, 204, SCREEN_WIDTH);
  for(int32_t i = 0; i < N_ACT; i++) {
    int32_t x = i * 48;
    bool sel = i == room_sel;
    if(sel) { P(0x438); frect(x + 2, 207, 44, 31); P(0xFD4); rect(x + 2, 207, 44, 31); }
    draw_icon(i, x + 24, 217, sel ? 0xFFF : 0xAAC);
    TC(ACT_NAME[i], x + 24, 227, sel ? 0xFD4 : 0xCCE);
  }

  top_bar("PIP LV" + str(level()), true);
  T(str(pal.coins) + "c", 160, 4, 0xFC3);
  draw_msg(92);
}

void update_room() {
  if(pressed(LEFT))  { room_sel = wrapi(room_sel - 1, N_ACT); blip(500, 30); }
  if(pressed(RIGHT)) { room_sel = wrapi(room_sel + 1, N_ACT); blip(500, 30); }
  if(pressed(B))     { scr = TOWN; blip(700); return; }

  if(pal.sleeping) {
    if(pressed(A) || pressed(B)) { pal.sleeping = false; say("GOOD MORNING!"); blip(800); }
    return;
  }

  if(pressed(A)) {
    switch(room_sel) {
      case 0: scr = FEED; blip(700); break;
      case 1:
        hub_enter(ROOM); blip(900);
        break;
      case 2: scr = CREATE; blip(700); break;
      case 3: pal.sleeping = true; blip(300, 120); break;
      case 4: scr = TOWN; blip(700); break;
    }
  }
}

// ---------------------------------------------------------------------------
// FEED
// ---------------------------------------------------------------------------

constexpr int32_t N_FOOD = 3;
const char *FOOD_NAME[N_FOOD] = {"PIZZA", "BURGER", "SODA"};
const int32_t FOOD_FULL[N_FOOD]   = {20, 25, 5};
const int32_t FOOD_ENERGY[N_FOOD] = {0, 0, 15};
int32_t feed_sel = 0;
int32_t eat_t = 0;
int32_t eat_item = 0;

void draw_food(int32_t i, int32_t x, int32_t y) {
  switch(i) {
    case 0:
      P(0xFD4); fpoly({x, y, x + 22, y, x + 11, y + 22});
      P(0xE34); fcircle(x + 7, y + 5, 2); fcircle(x + 14, y + 6, 2); fcircle(x + 11, y + 12, 2);
      break;
    case 1:
      P(0xD84); frect(x, y, 22, 6);
      P(0x3B4); frect(x - 1, y + 6, 24, 3);
      P(0x842); frect(x, y + 9, 22, 5);
      P(0xD84); frect(x, y + 14, 22, 6);
      break;
    case 2:
      P(0xE34); frect(x + 4, y, 14, 22);
      P(0xFFF); frect(x + 4, y + 8, 14, 6);
      P(0x112); hline(x + 4, y, 14); vline(x + 11, y - 5, 5);
      break;
  }
}

void draw_feed() {
  P(0x426); frect(0, 22, SCREEN_WIDTH, 186);
  P(0xFD8); frect(6, 30, 108, 120);

  // menu panel
  P(0xFFF); rect(6, 30, 108, 120);
  P(0x112); frect(7, 31, 106, 12); TC("SNACKS", 60, 33, 0xFD4);
  for(int32_t i = 0; i < N_FOOD; i++) {
    int32_t y = 48 + i * 34;
    if(i == feed_sel) { P(0xFFF); frect(10, y - 2, 100, 32); P(0xE56); rect(10, y - 2, 100, 32); }
    draw_food(i, 14, y + 3);
    T(FOOD_NAME[i], 40, y + 3, 0x214);
    std::string b = FOOD_ENERGY[i] ? "+" + str(FOOD_ENERGY[i]) + " NRG" : "+" + str(FOOD_FULL[i]) + " FULL";
    T(b, 40, y + 15, 0x173);
  }

  // Pip on the right, mouth open while eating
  P(0x853); frect(0, 150, SCREEN_WIDTH, 58);
  int32_t ex = (eat_t > 0 && eat_t < 50) ? 1 : 0;
  draw_pip(pal.look, 176, 198, 3, ex);
  if(eat_t > 0) {
    if(eat_t > 25) draw_food(eat_item, 142 + (50 - eat_t), 120);
    P(0xFFF); frect(150, 44, 80, 22); P(0x112); rect(150, 44, 80, 22);
    TC("YUM!", 190, 51, 0x214);
  }

  top_bar("SNACK BREAK", true);
  bottom_bar("A:EAT", "B:BACK", "FULL " + str(pal.hunger) + "%");
}

void update_feed() {
  if(eat_t > 0) {
    eat_t--;
    return;
  }
  if(pressed(UP))   { feed_sel = wrapi(feed_sel - 1, N_FOOD); blip(500, 30); }
  if(pressed(DOWN)) { feed_sel = wrapi(feed_sel + 1, N_FOOD); blip(500, 30); }
  if(pressed(B))    { scr = ROOM; blip(400); }
  if(pressed(A)) {
    if(pal.hunger >= 100 && FOOD_ENERGY[feed_sel] == 0) {
      say("PIP IS STUFFED!"); thud(); scr = ROOM; return;
    }
    eat_item = feed_sel;
    eat_t = 50;
    pal.hunger = clampi(pal.hunger + FOOD_FULL[feed_sel], 0, 100);
    pal.energy = clampi(pal.energy + FOOD_ENERGY[feed_sel], 0, 100);
    pal.mood   = clampi(pal.mood + 5, 0, 100);
    jingle(660);
  }
}

// ---------------------------------------------------------------------------
// RUN: side scrolling street run
// ---------------------------------------------------------------------------

constexpr int32_t GROUND = 196;
constexpr int32_t PX = 56;          // pip's fixed screen x (centre)

struct obj_t { int32_t type; float x; float y; };   // 0 cone, 1 rail, 2 star
std::vector<obj_t> objs;

enum run_state_t { R_READY, R_PLAY, R_OVER };
run_state_t rs = R_READY;
float r_y, r_vy, r_speed, r_scroll;
bool r_grind, r_flipped, r_rewarded;
int32_t r_fl, r_lives, r_score, r_combo, r_time, r_spawn, r_inv, r_bannert;
std::string r_banner;

void banner(const std::string &s) { r_banner = s; r_bannert = 40; }

void run_start() {
  objs.clear();
  rs = R_READY;
  r_y = GROUND; r_vy = 0; r_speed = 3.0f; r_scroll = 0;
  r_grind = r_flipped = r_rewarded = false;
  r_fl = 0; r_lives = 3; r_score = 0; r_combo = 0;
  r_time = 40 * 40; r_spawn = 40; r_inv = 0; r_bannert = 0;
}

bool overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void run_spawn() {
  int32_t r = std::rand() % 10;
  if(r < 4) {
    objs.push_back({0, 250.0f, (float)GROUND});
    if(std::rand() % 2) objs.push_back({2, 253.0f, (float)GROUND - 62});
  } else if(r < 6) {
    objs.push_back({1, 250.0f, (float)GROUND});
    if(std::rand() % 2) objs.push_back({2, 276.0f, (float)GROUND - 40});
  } else {
    int32_t base = GROUND - 30 - (std::rand() % 40);
    for(int32_t i = 0; i < 3; i++) objs.push_back({2, 250.0f + i * 22, (float)base - (i == 1 ? 8 : 0)});
  }
  r_spawn = (int32_t)((110 + std::rand() % 90) / r_speed);
}

void hit_player() {
  if(r_inv > 0) return;
  r_lives--;
  r_combo = 0;
  r_inv = 60;
  r_grind = false;
  spawn_bail(PX, r_y - 4, DECK[pal.look.board]);
  shake(3, 10); hit_stop(4); crash();
  if(r_lives <= 0) rs = R_OVER;
}

void update_run() {
  if(rs == R_READY) {
    if(pressed(A)) { rs = R_PLAY; blip(900); }
    if(pressed(Y) || pressed(B)) { scr = HUB; }
    return;
  }

  if(rs == R_OVER) {
    if(!r_rewarded) {
      r_rewarded = true;
      int32_t earned = r_score / 40;
      pal.coins += earned;
      if(r_score > pal.best_run) pal.best_run = r_score;
      pal.mood   = clampi(pal.mood + 10 + earned / 4, 0, 100);
      pal.energy = clampi(pal.energy - 15, 0, 100);
      pal.hunger = clampi(pal.hunger - 10, 0, 100);
      jingle(990);
    }
    if(pressed(A) || pressed(B)) { scr = HUB; }
    return;
  }

  // ---- playing ----
  r_time--;
  if(r_time <= 0) { rs = R_OVER; return; }
  r_speed = 3.0f + (1600 - r_time) / 600.0f;
  r_scroll += r_speed;
  if(r_inv > 0) r_inv--;
  if(r_bannert > 0) r_bannert--;

  // jump / flip / grind
  bool air = r_y < GROUND && !r_grind;
  if(pressed(A) && (!air)) {
    r_vy = -6.6f; r_flipped = false; r_fl = 0;
    banner("OLLIE!");
    blip(700, 50);
    r_y -= 1;
  }
  if(pressed(X) && air && !r_flipped) {
    r_flipped = true; r_fl = 24;
    r_combo++; r_score += 30 * r_combo;
    banner("KICKFLIP!"); jingle(1200);
  }
  if(pressed(B) && air) {
    for(auto &o : objs) {
      if(o.type == 1 && o.x < PX + 10 && o.x + 64 > PX - 10 && r_y < GROUND - 4) {
        r_grind = true; r_y = GROUND - 10; r_vy = 0;
        r_combo++; banner("GRIND!"); jingle(1000);
        break;
      }
    }
  }

  // physics
  if(r_grind) {
    bool still = false;
    for(auto &o : objs) if(o.type == 1 && o.x < PX + 10 && o.x + 64 > PX - 10) still = true;
    if(!still) { r_grind = false; r_vy = 0.5f; }
    else {
      r_y = GROUND - 10;
      r_score += r_combo > 0 ? r_combo : 1;
      if(pressed(A)) { r_grind = false; r_vy = -6.0f; r_flipped = false; banner("OLLIE!"); }
    }
  } else {
    r_vy += 0.35f;
    r_y += r_vy;
    if(r_y >= GROUND) { r_y = GROUND; r_vy = 0; r_fl = 0; }
  }
  if(r_fl > 0) r_fl--;

  // objects
  r_spawn--;
  if(r_spawn <= 0) run_spawn();

  for(auto &o : objs) o.x -= r_speed;

  float py = r_y;
  for(auto &o : objs) {
    if(o.type == 0 && overlap(PX - 10, py - 44, 20, 44, o.x, GROUND - 14, 12, 14)) {
      hit_player();
      o.x = -100;
    } else if(o.type == 1 && !r_grind && overlap(PX - 10, py - 44, 20, 44, o.x, GROUND - 10, 64, 10)) {
      hit_player();
      o.x = -100;
    } else if(o.type == 2 && overlap(PX - 10, py - 44, 20, 44, o.x, o.y, 8, 8)) {
      r_combo++; r_score += 10 * r_combo;
      o.x = -100;
      blip(1000 + r_combo * 40, 50);
    }
  }
  objs.erase(std::remove_if(objs.begin(), objs.end(), [](const obj_t &o) { return o.x < -70; }), objs.end());
}

void draw_run() {
  // sky
  P(0x75B); frect(0, 22, SCREEN_WIDTH, 40);
  P(0xA7C); frect(0, 62, SCREEN_WIDTH, 40);
  P(0xF9A); frect(0, 102, SCREEN_WIDTH, 40);
  P(0xFC8); frect(0, 142, SCREEN_WIDTH, 54);
  P(0xFF8); fcircle(180, 110, 18);

  // far buildings (slow parallax)
  int32_t off = (int32_t)(r_scroll * 0.3f) % 80;
  for(int32_t i = -1; i < 4; i++) {
    int32_t x = i * 80 - off;
    P(0x536); frect(x, 124, 34, 72);
    frect(x + 40, 140, 30, 56);
    P(0xFD8, 8);
    for(int32_t w = 0; w < 3; w++) frect(x + 5 + w * 10, 132, 4, 6);
  }

  // ground
  P(0x434); frect(0, GROUND, SCREEN_WIDTH, 44);
  P(0x656); frect(0, GROUND, SCREEN_WIDTH, 4);
  P(0x212);
  int32_t go = (int32_t)r_scroll % 32;
  for(int32_t x = -go; x < SCREEN_WIDTH; x += 32) frect(x, GROUND + 18, 16, 3);

  // objects
  for(auto &o : objs) {
    int32_t x = (int32_t)o.x, y = (int32_t)o.y;
    if(o.type == 0) {
      P(0xF72); fpoly({x, y, x + 12, y, x + 6, y - 14});
      P(0xFFF); frect(x + 3, y - 6, 6, 2);
    } else if(o.type == 1) {
      P(0x889); frect(x, y - 9, 3, 9); frect(x + 61, y - 9, 3, 9);
      P(0xCCD); frect(x - 2, y - 10, 68, 3);
    } else {
      P(0xFD2); frect(x + 3, y, 2, 8); frect(x, y + 3, 8, 2); frect(x + 1, y + 1, 6, 6);
      P(0xFFF); frect(x + 3, y + 3, 2, 2);
    }
  }

  // pip (flickers when hit)
  if(r_inv == 0 || (now_f / 3) % 2) {
    draw_pip(pal.look, PX, (int32_t)r_y, 2, r_inv > 0 ? -1 : 0, r_fl);
  }

  // hud
  P(0x214); frect(0, 0, SCREEN_WIDTH, 22);
  P(0x112); hline(0, 22, SCREEN_WIDTH);
  T("SCORE", 6, 4, 0xFD4); T(str(r_score), 50, 4, 0xFFF);
  T("COMBO", 100, 4, 0xFD4); T("X" + str(r_combo), 144, 4, 0xFFF);
  for(int32_t i = 0; i < 3; i++) { P(i < r_lives ? 0xF35 : 0x445); fcircle(208 + i * 10, 8, 3); }
  P(0x112); frect(6, 14, 228, 4);
  P(0x3CF); frect(6, 14, (228 * (1600 - r_time)) / 1600, 4);
  bottom_bar("A:OLLIE", "B:GRIND", "X:FLIP");

  if(r_bannert > 0) TC(r_banner, 120, 90 - (40 - r_bannert) / 3, 0xFD4, true);

  if(rs == R_READY) {
    P(0x112, 11); frect(30, 70, 180, 70); P(0xFFF); rect(30, 70, 180, 70);
    TC("STREET RUN", 120, 80, 0xFD4);
    TC("OLLIE CONES, GRIND RAILS", 120, 98, 0xFFF);
    TC("GRAB STARS, CHAIN COMBOS", 120, 108, 0xFFF);
    TC("A TO START", 120, 124, 0xFD4);
  } else if(rs == R_OVER) {
    P(0x112, 12); frect(30, 60, 180, 100); P(0xFFF); rect(30, 60, 180, 100);
    TC(r_lives <= 0 ? "WIPEOUT!" : "TIME UP!", 120, 70, 0xFD4);
    TC("SCORE " + str(r_score), 120, 90, 0xFFF);
    TC("COINS +" + str(r_score / 40), 120, 104, 0xFC3);
    TC("MOOD UP  ENERGY DOWN", 120, 118, 0xCCE);
    TC("A TO CONTINUE", 120, 138, 0xFD4);
  }
}

// ---------------------------------------------------------------------------
// CREATE
// ---------------------------------------------------------------------------

constexpr int32_t N_PART = 5;
const char *PART_NAME[N_PART] = {"FACE", "CAP", "SHIRT", "BOARD", "COLOR"};
const int32_t PART_N[N_PART]  = {N_FACE, N_CAP, N_SHIRT, N_BOARD, N_COLOR};
int32_t create_sel = 0;

int32_t *part_ptr(look_t &l, int32_t i) {
  switch(i) {
    case 0: return &l.face;
    case 1: return &l.cap;
    case 2: return &l.shirt;
    case 3: return &l.board;
    default: return &l.skin;
  }
}

void draw_create() {
  P(0x426); frect(0, 22, SCREEN_WIDTH, 202);

  // part list
  for(int32_t i = 0; i < N_PART; i++) {
    int32_t y = 34 + i * 30;
    bool sel = i == create_sel;
    P(sel ? 0x65C : 0x325); frect(8, y - 4, 100, 24);
    if(sel) { P(0xFD4); rect(8, y - 4, 100, 24); }
    T(PART_NAME[i], 16, y + 3, sel ? 0xFFF : 0xBBD);
    int32_t v = *part_ptr(pal.look, i);
    T(str(v + 1) + "/" + str(PART_N[i]), 70, y + 3, sel ? 0xFD4 : 0xBBD);
    if(sel) { T("<", 60, y + 3, 0xFD4); T(">", 100, y + 3, 0xFD4); }
  }

  // preview
  P(0x214); frect(120, 30, 112, 150);
  P(0x112); rect(120, 30, 112, 150);
  P(0x656); frect(121, 160, 110, 19);
  draw_pip(pal.look, 176, 168, 4);

  // swatch row for COLOR
  for(int32_t i = 0; i < N_COLOR; i++) {
    P(SKIN[i]); frect(124 + i * 13, 186, 11, 10);
    if(i == pal.look.skin) { P(0xFFF); rect(123 + i * 13, 185, 13, 12); }
  }

  top_bar("NEW SKATER", false);
  bottom_bar("A:DONE", "UP/DN:PART", "L/R:CHANGE");
  T("NAME:PIP_", 6, 210, 0xFFF);
}

void update_create() {
  if(pressed(UP))   { create_sel = wrapi(create_sel - 1, N_PART); blip(500, 30); }
  if(pressed(DOWN)) { create_sel = wrapi(create_sel + 1, N_PART); blip(500, 30); }
  int32_t d = (pressed(RIGHT) ? 1 : 0) - (pressed(LEFT) ? 1 : 0);
  if(d) {
    int32_t *p = part_ptr(pal.look, create_sel);
    *p = wrapi(*p + d, PART_N[create_sel]);
    blip(800, 30);
  }
  if(pressed(A) || pressed(B)) { scr = ROOM; jingle(880); }
}

// ---------------------------------------------------------------------------
// TOWN
// ---------------------------------------------------------------------------

struct npc_t { float x, y, dx, dy; look_t look; int32_t timer; };
npc_t npcs[3];
bool kai_wants = true;
int32_t kai_rearm = 0;
int32_t tx = 120, ty = 120;

struct spot_t { const char *name; int32_t x, y, w, h; };
const spot_t SPOTS[] = {
  {"BOWL",   24,  40, 76, 58},
  {"PIPE",  146,  36, 76, 26},
  {"STAIRS",136, 128, 70, 30},
  {"BOX",    24, 136, 56, 32},
  {"SHOP",  186, 170, 46, 36},
};
constexpr int32_t N_SPOT = 5;

int32_t near_spot() {
  for(int32_t i = 0; i < N_SPOT; i++) {
    const spot_t &s = SPOTS[i];
    if(tx > s.x - 12 && tx < s.x + s.w + 12 && ty > s.y - 4 && ty < s.y + s.h + 24) return i;
  }
  return -1;
}

bool near_kai() {
  int32_t dx = tx - (int32_t)npcs[0].x, dy = ty - (int32_t)npcs[0].y;
  return dx * dx + dy * dy < 28 * 28;
}

void town_init() {
  npcs[0] = {170, 100, 0.4f, 0.2f, KAI, 0};
  npcs[1] = {70, 118, -0.3f, 0.3f, {1, 3, 2, 4, 0}, 0};
  npcs[2] = {110, 190, 0.5f, -0.2f, {3, 4, 6, 7, 2}, 0};
}

void update_town() {
  int32_t mx = (button(RIGHT) ? 1 : 0) - (button(LEFT) ? 1 : 0);
  int32_t my = (button(DOWN) ? 1 : 0) - (button(UP) ? 1 : 0);
  tx = clampi(tx + mx * 2, 10, 230);
  ty = clampi(ty + my * 2, 44, 214);

  for(int32_t i = 0; i < 3; i++) {
    npc_t &n = npcs[i];
    if(i == 0 && kai_wants && near_kai()) continue;   // kai waits for you
    n.x += n.dx; n.y += n.dy;
    if(n.x < 20 || n.x > 220) n.dx = -n.dx;
    if(n.y < 50 || n.y > 210) n.dy = -n.dy;
    if(--n.timer <= 0) {
      n.timer = 60 + std::rand() % 100;
      n.dx = ((std::rand() % 9) - 4) / 8.0f;
      n.dy = ((std::rand() % 9) - 4) / 8.0f;
    }
    n.x = std::max(20.0f, std::min(220.0f, n.x));
    n.y = std::max(50.0f, std::min(210.0f, n.y));
  }

  if(!kai_wants && --kai_rearm <= 0) kai_wants = true;

  if(pressed(B)) { scr = ROOM; blip(400); return; }
  if(pressed(A)) {
    if(kai_wants && near_kai()) { scr = MEET; meet_start(); blip(900); return; }
    int32_t s = near_spot();
    if(s == 4) {
      hub_enter(TOWN); blip(900); return;
    } else if(s >= 0) {
      if(pal.energy < 10) { say("TOO TIRED"); thud(); }
      else {
        pal.energy = clampi(pal.energy - 8, 0, 100);
        pal.mood   = clampi(pal.mood + 8, 0, 100);
        pal.hunger = clampi(pal.hunger - 4, 0, 100);
        pal.coins += 3;
        say(std::string("NICE SESH AT THE ") + SPOTS[s].name + "! +3c");
        jingle(990);
      }
    }
  }
}

void draw_town() {
  // concrete
  P(0x889); frect(0, 22, SCREEN_WIDTH, 202);
  P(0x778); for(int32_t x = 0; x < SCREEN_WIDTH; x += 24) vline(x, 22, 202);
  for(int32_t y = 22; y < 224; y += 24) hline(0, y, SCREEN_WIDTH);

  // bowl
  const spot_t &bowl = SPOTS[0];
  P(0x556); fellipse(bowl.x + bowl.w / 2, bowl.y + bowl.h / 2, bowl.w / 2, bowl.h / 2);
  P(0x778); fellipse(bowl.x + bowl.w / 2, bowl.y + bowl.h / 2, bowl.w / 2 - 8, bowl.h / 2 - 8);
  P(0x445); fellipse(bowl.x + bowl.w / 2, bowl.y + bowl.h / 2, bowl.w / 2 - 20, bowl.h / 2 - 16);
  // pipe
  const spot_t &pipe = SPOTS[1];
  P(0x556); frect(pipe.x, pipe.y, pipe.w, pipe.h);
  P(0x334); frect(pipe.x + 6, pipe.y + 6, pipe.w - 12, pipe.h - 12);
  // stairs
  const spot_t &st = SPOTS[2];
  for(int32_t i = 0; i < 6; i++) {
    P(i % 2 ? 0xCCD : 0xAAB); frect(st.x, st.y + i * 5, st.w, 5);
  }
  // funbox
  const spot_t &bx = SPOTS[3];
  P(0xB84); frect(bx.x, bx.y, bx.w, bx.h);
  P(0xDA6); frect(bx.x + 4, bx.y + 4, bx.w - 8, bx.h - 8);
  P(0xCCD); hline(bx.x + 4, bx.y + 3, bx.w - 8);
  // shop
  const spot_t &sh = SPOTS[4];
  P(0xE34); frect(sh.x, sh.y, sh.w, sh.h);
  P(0xFFF); frect(sh.x + 4, sh.y + 4, sh.w - 8, 10);
  P(0x112); frect(sh.x + 16, sh.y + 20, 14, sh.h - 20);

  for(int32_t i = 0; i < N_SPOT; i++) {
    const spot_t &s = SPOTS[i];
    int32_t lx = s.x + s.w / 2;
    int32_t ly = (i == 4) ? s.y - 10 : s.y + s.h + 2;
    TS(s.name, lx - 3 * (int32_t)std::string(s.name).size(), ly, 0xFFF);
  }

  // skaters (sorted by y so nearer ones overlap)
  struct dr_t { int32_t y; int32_t idx; };
  std::vector<dr_t> order = {{(int32_t)npcs[0].y, 0}, {(int32_t)npcs[1].y, 1}, {(int32_t)npcs[2].y, 2}, {ty, -1}};
  std::sort(order.begin(), order.end(), [](const dr_t &a, const dr_t &b) { return a.y < b.y; });
  for(auto &d : order) {
    if(d.idx < 0) {
      P(0x112, 8); fellipse(tx, ty + 1, 10, 3);
      draw_pip(pal.look, tx, ty, 1);
      TS("PIP", tx - 9, ty - 31, 0xFD4);
    } else {
      npc_t &n = npcs[d.idx];
      P(0x112, 8); fellipse((int32_t)n.x, (int32_t)n.y + 1, 10, 3);
      draw_pip(n.look, (int32_t)n.x, (int32_t)n.y, 1);
      if(d.idx == 0) {
        TS("KAI", (int32_t)n.x - 9, (int32_t)n.y - 31, 0xFFF);
        if(kai_wants && (now_f / 15) % 2) TS("!", (int32_t)n.x + 12, (int32_t)n.y - 34, 0xF35);
      }
    }
  }

  top_bar("SKATE PARK", false);
  T(str(pal.coins) + "c", 160, 4, 0xFC3);
  const char *a = "A:-";
  if(kai_wants && near_kai()) a = "A:CHAT";
  else if(near_spot() == 4) a = "A:SHOP";
  else if(near_spot() >= 0) a = "A:SKATE";
  bottom_bar(a, "B:MENU", "NRG " + str(pal.energy) + "%");
  draw_msg(206 - 20);
}

// ---------------------------------------------------------------------------
// MEET
// ---------------------------------------------------------------------------

int32_t meet_sel = 0;
int32_t meet_result = 0;   // 0 choosing, 1 said yes, 2 said no
int32_t meet_t = 0;
int32_t meet_line = 0;

const char *KAI_LINES[3][3] = {
  {"YO PIP! WANNA SESH AT", "THE BOWL? I JUST LANDED", "A KICKFLIP!"},
  {"PIP! THE PARK IS EMPTY", "TODAY. WANNA GRIND THE", "RAILS TOGETHER?"},
  {"CREW NIGHT? I FOUND A", "NEW SPOT. BRING YOUR", "BEST DECK!"},
};

void meet_start() {
  meet_sel = 0; meet_result = 0; meet_t = 0;
  meet_line = std::rand() % 3;
}

void update_meet() {
  if(meet_result == 0) {
    if(pressed(LEFT) || pressed(UP))    { meet_sel = 0; blip(500, 30); }
    if(pressed(RIGHT) || pressed(DOWN)) { meet_sel = 1; blip(500, 30); }
    if(pressed(B)) { meet_sel = 1; meet_result = 2; meet_t = 60; }
    if(pressed(A)) {
      if(meet_sel == 0) {
        meet_result = 1; meet_t = 90;
        pal.hearts++;
        pal.mood = clampi(pal.mood + 20, 0, 100);
        pal.energy = clampi(pal.energy - 10, 0, 100);
        pal.coins += 10;
        jingle(1100);
      } else { meet_result = 2; meet_t = 60; blip(400); }
    }
  } else {
    if(--meet_t <= 0) {
      scr = TOWN;
      kai_wants = false; kai_rearm = 40 * 20;
      if(meet_result == 1) say("CREW LV " + str(level()) + "!  +10c", 100);
    }
  }
}

void draw_meet() {
  // sunset
  P(0x425); frect(0, 22, SCREEN_WIDTH, 40);
  P(0xA46); frect(0, 62, SCREEN_WIDTH, 30);
  P(0xF68); frect(0, 92, SCREEN_WIDTH, 30);
  P(0xFA6); frect(0, 122, SCREEN_WIDTH, 24);
  P(0xFE8); fcircle(120, 120, 24);
  P(0x535); frect(0, 120, 40, 30); frect(52, 108, 30, 42); frect(170, 114, 34, 36); frect(212, 126, 28, 24);
  P(0x434); frect(0, 148, SCREEN_WIDTH, 40);
  P(0x656); frect(0, 148, SCREEN_WIDTH, 3);

  draw_pip(pal.look, 70, 176, 3);
  draw_pip(KAI, 170, 176, 3);
  TS("PIP", 58, 90, 0xFD4);
  TS("KAI", 158, 90, 0xFFF);

  // floating hearts
  for(int32_t i = 0; i < 3; i++) {
    int32_t hy = 130 - ((now_f / 2 + i * 20) % 50);
    int32_t hx = 114 + i * 8 - (i == 1 ? 6 : 0);
    P(0xF35); fcircle(hx - 2, hy, 2); fcircle(hx + 2, hy, 2); fpoly({hx - 4, hy + 1, hx + 4, hy + 1, hx, hy + 5});
  }

  top_bar("CREW", false);
  T("LV." + str(level()), 150, 4, 0xFC3);

  // dialogue box
  P(0x112); frect(6, 150, 228, 84);
  P(0xFFF); rect(6, 150, 228, 84);
  P(0x214); frect(10, 142, 28, 12); T("KAI", 14, 144, 0xFD4);
  if(meet_result == 0) {
    for(int32_t i = 0; i < 3; i++) T(KAI_LINES[meet_line][i], 14, 160 + i * 12, 0xFFF);
    for(int32_t i = 0; i < 2; i++) {
      int32_t x = 20 + i * 100;
      bool sel = i == meet_sel;
      P(sel ? 0x65C : 0x325); frect(x, 200, 90, 22);
      if(sel) { P(0xFD4); rect(x, 200, 90, 22); }
      TC(i == 0 ? "LET'S GO" : "LATER", x + 45, 207, sel ? 0xFFF : 0xBBD);
    }
  } else if(meet_result == 1) {
    T("SICK! THE CREW GROWS.", 14, 160, 0xFFF);
    T("HEARTS: " + str(pal.hearts), 14, 176, 0xF68);
    T("+MOOD  +10 COINS", 14, 192, 0xFC3);
  } else {
    T("NO WORRIES, CATCH YOU", 14, 160, 0xFFF);
    T("LATER, PIP!", 14, 172, 0xFFF);
  }
}

// ---------------------------------------------------------------------------
// SKATE SHOP hub, TRICK ATTACK (half-pipe) and SKY POP (high jump)
// ---------------------------------------------------------------------------

enum ev_state_t { EV_READY, EV_PLAY, EV_OVER };

// "6'2\"" from tenths of a foot (62 -> 6'2")
std::string fmt_h(int32_t tenths) {
  return str(tenths / 10) + "'" + str(((tenths % 10) * 12) / 10) + "\"";
}

// coins, mood and tiredness after finishing an event
void event_done(int32_t coins) {
  pal.coins += coins;
  pal.mood   = clampi(pal.mood + 10 + coins / 4, 0, 100);
  pal.energy = clampi(pal.energy - 12, 0, 100);
  pal.hunger = clampi(pal.hunger - 8, 0, 100);
  jingle(990);
}

// ---- the half-pipe stage shared by the two ramp events --------------------

int32_t bowl_y[SCREEN_WIDTH];            // riding surface for every screen column

void bowl_init() {
  for(int32_t x = 0; x < SCREEN_WIDTH; x++) {
    float d = std::fabs((x - 120) / 112.0f);
    if(d > 1.0f) d = 1.0f;
    bowl_y[x] = 196 - (int32_t)(92.0f * d * d * std::sqrt(d));    // d^2.5
  }
}

void draw_stage() {
  // sky
  P(0x57C); frect(0, 22, SCREEN_WIDTH, 24);
  P(0x79E); frect(0, 46, SCREEN_WIDTH, 16);
  P(0x9BF); frect(0, 62, SCREEN_WIDTH, 14);

  // banner flags
  static const uint16_t FLAG[6] = {0xE34, 0xFD4, 0x4B5, 0xF6A, 0xF83, 0x3CB};
  for(int32_t i = 0; i < 6; i++) {
    int32_t x = 8 + i * 40;
    P(0x112); vline(x, 62, 12);
    P(FLAG[i]); fpoly({x + 1, 62, x + 14, 65, x + 1, 68});
  }

  // the crowd: a speckle of heads and shirts, a few of them bouncing
  static const uint16_t CROWD[8] = {0xE34, 0xFD4, 0x36E, 0x3C5, 0xF6A, 0xF83, 0x3CB, 0xFFF};
  P(0x224); frect(0, 76, SCREEN_WIDTH, 30);
  uint32_t seed = 7;
  for(int32_t y = 79; y < 102; y += 5) {
    for(int32_t x = 0; x < SCREEN_WIDTH; x += 4) {
      seed = seed * 1664525u + 1013904223u;
      int32_t bounce = (((now_f / 12) + (seed >> 28)) % 5 == 0) ? 1 : 0;
      P((seed & 4) ? 0xFC9 : 0xD96); frect(x + 1, y - bounce, 2, 2);
      P(CROWD[(seed >> 8) & 7]);     frect(x, y + 2 - bounce, 4, 3);
    }
  }
  P(0x112); frect(0, 104, SCREEN_WIDTH, 2);

  // the bowl: dark under the lip, lit riding surface
  P(0x338); frect(0, 106, SCREEN_WIDTH, 118);
  for(int32_t x = 0; x < SCREEN_WIDTH; x++) {
    int32_t y = bowl_y[x];
    P(((x / 24) % 2) ? 0x99B : 0xAAC); vline(x, y, 224 - y);
    P(0x112);                          vline(x, y - 1, 1);
  }
  // platforms with rails on both lips
  for(int32_t s = 0; s < 2; s++) {
    int32_t x = s ? 218 : 0;
    P(0x112); frect(x, 96, 22, 12);
    P(0x669); frect(x, 97, 22, 10);
    P(0xAAC); frect(x, 97, 22, 2);
    P(0x112); frect(x + 5, 86, 2, 11); frect(x + 15, 86, 2, 11);
    P(0xAAC); frect(x + 3, 85, 15, 2);
  }
}

// ---- TRICK ATTACK ---------------------------------------------------------

constexpr int32_t T_AIRS = 5, T_AIR_LEN = 64, T_TRICK_LEN = 16;
const char *TRICK_NAME[3] = {"GRAB", "KICKFLIP!", "360 SPIN!"};
const int32_t TRICK_PTS[3] = {100, 150, 200};

ev_state_t ts = EV_READY;
float t_phase, t_amp;
int32_t t_air, t_airs, t_score, t_combo, t_sum, t_ntricks;
int32_t t_trick, t_trick_t, t_last_done, t_pump_cd, t_side, t_lastk, t_banner_t;
bool t_rewarded;
int32_t t_bail_t = 0;
std::string t_banner;

void t_say(const std::string &s) { t_banner = s; t_banner_t = 50; }

void trick_start() {
  ts = EV_READY;
  t_phase = 0; t_amp = 0.5f;
  t_air = -1; t_airs = 0; t_score = 0; t_combo = 0; t_sum = 0; t_ntricks = 0;
  t_trick = -1; t_trick_t = 0; t_last_done = -1; t_pump_cd = 0; t_side = 1;
  t_lastk = -1; t_banner_t = 0; t_rewarded = false; t_bail_t = 0; pump_last = 0;
}

void update_trick() {
  if(ts == EV_READY) {
    if(pressed(A)) { ts = EV_PLAY; blip(900); }
    if(pressed(B)) scr = HUB;
    return;
  }
  if(ts == EV_OVER) {
    if(!t_rewarded) {
      t_rewarded = true;
      event_done(t_score / 60);
      if(t_score > pal.best_trick) pal.best_trick = t_score;
    }
    if(pressed(A) || pressed(B)) scr = HUB;
    return;
  }

  if(t_banner_t > 0) t_banner_t--;
  if(t_pump_cd > 0) t_pump_cd--;
  if(t_bail_t > 0) t_bail_t--;

  if(t_air < 0) {
    // riding the transition back and forth
    t_phase += 6.2831853f / 72.0f;
    float s = std::sin(t_phase);
    float x = t_amp * s;

    // pump through the middle of the bowl to build speed
    int32_t pp = pump_press();
    if(pp == 1 && std::fabs(x) < 0.45f && t_pump_cd == 0) {
      t_amp = std::min(1.0f, t_amp + 0.14f);
      t_pump_cd = 6;
      blip(500 + (int32_t)(t_amp * 400), 30);
    } else if(pp == -1) {                      // same side twice: out of rhythm
      t_amp = std::max(0.3f, t_amp - 0.05f);
      blip(180, 30);
    }

    // crossing the top of the swing at full speed means air
    int32_t k = (int32_t)std::floor((t_phase - 1.5707963f) / 3.1415927f);
    if(k != t_lastk) {
      t_lastk = k;
      if(t_amp >= 0.995f) {
        t_air = 0;
        t_side = (s > 0) ? 1 : -1;
        t_sum = 0; t_ntricks = 0; t_trick = -1; t_trick_t = 0; t_last_done = -1;
        blip(800, 60);
      }
    }
    return;
  }

  // in the air
  t_air++;
  if(t_trick_t > 0) {
    if(--t_trick_t == 0) {
      int32_t pts = TRICK_PTS[t_trick];
      if(t_trick == t_last_done) pts /= 2;     // repeating a trick is worth less
      t_sum += pts; t_ntricks++; t_last_done = t_trick;
      jingle(700 + t_ntricks * 150);
    }
  } else if(t_air < T_AIR_LEN - 6) {
    int32_t k = pressed(B) ? 0 : pressed(X) ? 1 : pressed(Y) ? 2 : -1;
    if(k >= 0) { t_trick = k; t_trick_t = T_TRICK_LEN; t_say(TRICK_NAME[k]); }
  }

  if(t_air >= T_AIR_LEN) {
    if(t_trick_t > 0) {                        // still mid trick: bail
      t_combo = 0; t_amp = 0.5f; t_bail_t = 45;
      t_say("BAIL!");
      spawn_bail((float)(120 + t_side * 112), 104.0f, DECK[pal.look.board]);
      shake(3, 10); hit_stop(4); crash();
    } else if(t_ntricks > 0) {                 // clean landing
      int32_t pts = t_sum * (1 + t_combo);
      t_score += pts; t_combo++;
      t_say("+" + str(pts)); jingle(990);
      if(t_combo >= 2) { shake(2, 6); hit_stop(3); }
    } else {
      t_combo = 0; t_say("NO TRICK");
    }
    t_trick_t = 0; t_air = -1;
    if(++t_airs >= T_AIRS) ts = EV_OVER;
  }
}

void draw_trick() {
  draw_stage();

  // pip
  int32_t px, by, emo = 0, fl = 0;
  if(t_air < 0) {
    px = 120 + (int32_t)(t_amp * std::sin(t_phase) * 112.0f);
    by = bowl_y[clampi(px, 0, SCREEN_WIDTH - 1)];
  } else {
    float u = (float)t_air / T_AIR_LEN;
    px = 120 + t_side * (112 + (int32_t)(5.0f * std::sin(3.1415927f * u)));
    by = bowl_y[clampi(px, 0, SCREEN_WIDTH - 1)] - (int32_t)(4.0f * 46.0f * u * (1.0f - u));
    if(t_trick_t > 0) { emo = 1; if(t_trick == 1) fl = t_trick_t; }
  }
  P(0x112, 8); fellipse(px, bowl_y[clampi(px, 0, SCREEN_WIDTH - 1)] + 1, 12, 3);
  draw_pip(pal.look, px, by, 2, emo, fl);
  if(t_bail_t > 0) dizzy(px, by - 34);

  if(t_banner_t > 0) TC(t_banner, 120, 30 - (50 - t_banner_t) / 8, 0xFD4, true);

  top_bar("PIP", false);
  TC("AIR " + str(std::min<int32_t>(t_airs + 1, T_AIRS)) + "/" + str(T_AIRS), 120, 4, 0xFFF);
  T(str(t_score), 236 - 6 * (int32_t)str(t_score).size(), 4, 0xFFF);

  P(0x214); frect(0, 224, SCREEN_WIDTH, 16);
  P(0x112); hline(0, 224, SCREEN_WIDTH);
  T("COMBO", 6, 228, 0xFD4);
  bar(52, 229, 120, clampi(t_combo * 25, 0, 100), 0xFC3);
  T("X" + str(1 + t_combo), 184, 228, 0xFFF);
  // button hints sit on the stage so the combo bar stays readable
  P(0x112, 9); frect(0, 108, 240, 12);
  TC(t_air >= 0 ? "B:GRAB  X:FLIP  Y:SPIN" : "PUMP LEFT/RIGHT IN THE MIDDLE", 120, 110, 0xFFF);

  if(ts == EV_READY) {
    P(0x112, 12); frect(24, 124, 192, 70); P(0xFFF); rect(24, 124, 192, 70);
    TC("TRICK ATTACK", 120, 132, 0xFD4);
    TC("ALTERNATE LEFT/RIGHT TO PUMP", 120, 148, 0xFFF);
    TC("CHAIN TRICKS IN THE AIR", 120, 158, 0xFFF);
    TC("A TO START", 120, 176, 0xFD4);
  } else if(ts == EV_OVER) {
    P(0x112, 12); frect(24, 122, 192, 78); P(0xFFF); rect(24, 122, 192, 78);
    TC("RUN OVER!", 120, 130, 0xFD4);
    TC("SCORE " + str(t_score), 120, 146, 0xFFF);
    TC("COINS +" + str(t_score / 60), 120, 158, 0xFC3);
    if(t_score >= pal.best_trick && t_score > 0) TC("NEW BEST!", 120, 170, 0xF6A);
    TC("A TO CONTINUE", 120, 184, 0xFD4);
  }
}

// ---- SKY POP ---------------------------------------------------------------

enum j_state_t { J_MASH, J_FLY, J_RESULT };
ev_state_t js = EV_READY;
j_state_t jst = J_MASH;
int32_t j_pass, j_t, j_h, j_cleared, j_best, j_px0, j_y0, j_banner_t;
float j_meter;
bool j_clear, j_rewarded;
std::string j_banner;

int32_t j_bar() { return 45 + 6 * j_pass; }      // bar height in tenths of a foot
int32_t bar_y(int32_t tenths) { return 100 - (tenths * 9) / 10; }

void jump_start() {
  js = EV_READY; jst = J_MASH;
  j_pass = 0; j_t = 0; j_h = 0; j_cleared = 0; j_best = 0; j_meter = 0;
  j_clear = false; j_rewarded = false; j_banner_t = 0; pump_last = 0;
}

void update_jump() {
  if(js == EV_READY) {
    if(pressed(A)) { js = EV_PLAY; blip(900); }
    if(pressed(B)) scr = HUB;
    return;
  }
  if(js == EV_OVER) {
    if(!j_rewarded) {
      j_rewarded = true;
      event_done(j_cleared * 5);
      if(j_best > pal.best_height) pal.best_height = j_best;
    }
    if(pressed(A) || pressed(B)) scr = HUB;
    return;
  }

  if(j_banner_t > 0) j_banner_t--;
  j_t++;

  if(jst == J_MASH) {
    int32_t pp = pump_press();
    if(pp == 1)       { j_meter = std::min(100.0f, j_meter + 7.0f); blip(400 + (int32_t)(j_meter * 6), 20); }
    else if(pp == -1) { j_meter = std::max(0.0f, j_meter - 3.0f); }
    j_meter = std::max(0.0f, j_meter - 0.35f);
    if(pressed(B) || j_t > 280) {
      // pop! height comes from how much speed was built, with a bonus for the red zone
      j_h = 30 + (int32_t)(j_meter * 0.42f);
      if(j_meter >= 88.0f) j_h += 4;
      j_clear = j_h >= j_bar();
      float x = 120 + std::sin(now_f * 0.22f) * (j_meter / 100.0f) * 95.0f;
      j_px0 = (int32_t)x;
      j_y0 = bowl_y[clampi(j_px0, 0, SCREEN_WIDTH - 1)];
      jst = J_FLY; j_t = 0;
      blip(900, 80);
    }
  } else if(jst == J_FLY) {
    if(j_t >= 50) {
      if(j_clear) {
        j_cleared++; if(j_h > j_best) j_best = j_h;
        j_banner = "CLEARED " + fmt_h(j_h); jingle(1100);
      } else {
        j_banner = "CLIPPED THE BAR!";
        spawn_bail(196.0f, 104.0f, DECK[pal.look.board]);
        shake(3, 10); hit_stop(4); crash();
      }
      j_banner_t = 55;
      jst = J_RESULT; j_t = 0;
    }
  } else {
    if(j_t >= 60) {
      if(++j_pass >= 5) js = EV_OVER;
      else { jst = J_MASH; j_t = 0; j_meter = 0; }
    }
  }
}

void draw_jump() {
  draw_stage();

  // measuring pole on the right with the bar across it
  int32_t by_ = bar_y(j_bar());
  P(0x112); frect(205, 22, 7, 88);
  P(0xFD4); frect(206, 22, 5, 86);
  P(0x112); for(int32_t f = 1; f <= 8; f++) frect(206, 100 - f * 9, 5, 1);
  bool hit = (jst == J_RESULT && !j_clear);
  P(0x112); frect(194, by_ - 1, 24, 5);
  P(hit ? 0xA77 : 0xE34); frect(hit ? 190 : 195, hit ? by_ + 20 : by_, 22, 3);

  // pip
  int32_t px, by, emo = 0;
  if(jst == J_MASH) {
    float x = 120 + std::sin(now_f * 0.22f) * (j_meter / 100.0f) * 95.0f;
    px = (int32_t)x;
    by = bowl_y[clampi(px, 0, SCREEN_WIDTH - 1)];
  } else {
    int32_t t = (jst == J_FLY) ? j_t : 50;
    float u = t / 50.0f;
    int32_t apex = (j_h * 9) / 10 + 4;
    px = j_px0 + (int32_t)((196 - j_px0) * u);
    int32_t base = j_y0 + (int32_t)((104 - j_y0) * u);
    by = base - (int32_t)(4.0f * apex * u * (1.0f - u));
    if(jst == J_RESULT) { by = 104; px = 196; }
    emo = j_clear ? 1 : -1;
  }
  P(0x112, 8); fellipse(px, bowl_y[clampi(px, 0, SCREEN_WIDTH - 1)] + 1, 12, 3);
  draw_pip(pal.look, px, by, 2, emo);
  if(jst == J_RESULT && !j_clear) dizzy(px, by - 34);

  if(j_banner_t > 0) TC(j_banner, 100, 36, j_clear ? 0xFD4 : 0xF66, true);

  top_bar("PIP", false);
  TC("PASS " + str(std::min<int32_t>(j_pass + 1, 5)) + "/5", 120, 4, 0xFFF);
  T("BAR " + fmt_h(j_bar()), 150, 4, 0xFD4);

  // speed panel
  P(0x214); frect(0, 196, SCREEN_WIDTH, 28);
  P(0x112); hline(0, 196, SCREEN_WIDTH);
  T("SPEED", 6, 202, 0xFD4);
  P(0x112); frect(56, 200, 176, 12);
  P(0x213); frect(58, 202, 172, 8);
  for(int32_t i = 0; i < 20; i++) {
    if(i * 5 < (int32_t)j_meter) { P(i < 10 ? 0x4B5 : (i < 17 ? 0xFD4 : 0xE34)); frect(58 + i * 8 + 1, 202, 6, 8); }
  }
  P(0xFFF); frect(58 + (88 * 172) / 100, 198, 1, 14);   // the red zone starts here
  T("L/R:PUMP", 6, 214, 0xFFF);
  T("B:POP IN THE RED", 234 - 6 * 16, 214, 0xFD4);

  P(0x214); frect(0, 224, SCREEN_WIDTH, 16);
  P(0x112); hline(0, 224, SCREEN_WIDTH);
  T("BEST " + fmt_h(std::max<int32_t>(j_best, pal.best_height)), 6, 228, 0xFFF);
  T("CLEARED " + str(j_cleared), 150, 228, 0xFC3);

  if(js == EV_READY) {
    P(0x112, 12); frect(24, 110, 192, 78); P(0xFFF); rect(24, 110, 192, 78);
    TC("SKY POP", 120, 118, 0xFD4);
    TC("PUMP LEFT/RIGHT FOR SPEED", 120, 134, 0xFFF);
    TC("B TO POP IN THE RED ZONE", 120, 144, 0xFFF);
    TC("CLEAR THE BAR 5 TIMES", 120, 154, 0xFFF);
    TC("A TO START", 120, 172, 0xFD4);
  } else if(js == EV_OVER) {
    P(0x112, 12); frect(24, 110, 192, 78); P(0xFFF); rect(24, 110, 192, 78);
    TC("ALL DONE!", 120, 118, 0xFD4);
    TC("BEST " + fmt_h(j_best), 120, 134, 0xFFF);
    TC("COINS +" + str(j_cleared * 5), 120, 146, 0xFC3);
    if(j_best >= pal.best_height && j_best > 0) TC("NEW BEST!", 120, 158, 0xF6A);
    TC("A TO CONTINUE", 120, 174, 0xFD4);
  }
}

// ---- skaters seen from above (downhill and pool events) -------------------

// heading hd is in radians on the screen (0 = right, pi/2 = down)
void draw_top(int32_t cx, int32_t cy, const look_t &l, float hd, int32_t lift = 0) {
  float c = std::cos(hd), s = std::sin(hd);
  P(0x112, 9); fellipse(cx, cy + 2, 9, 4);                       // shadow stays on the ground
  cy -= lift;
  for(int32_t pass = 0; pass < 2; pass++) {                      // outline first, then the board
    float hl = 8 + (pass ? 0 : 1), hw = 3.2f + (pass ? 0 : 1);
    P(pass ? DECK[l.board] : 0x112);
    fpoly({(int32_t)(cx + c * hl - s * hw), (int32_t)(cy + s * hl + c * hw),
           (int32_t)(cx + c * hl + s * hw), (int32_t)(cy + s * hl - c * hw),
           (int32_t)(cx - c * hl + s * hw), (int32_t)(cy - s * hl - c * hw),
           (int32_t)(cx - c * hl - s * hw), (int32_t)(cy - s * hl + c * hw)});
  }
  P(0xFFF); frect((int32_t)(cx + c * 6) - 1, (int32_t)(cy + s * 6) - 1, 2, 2);
  P(0x112); fcircle(cx, cy - 4, 6);                              // body
  P(SHIRTCOL[l.shirt]); fcircle(cx, cy - 4, 5);
  P(0x112); fcircle(cx, cy - 9, 5);                              // head
  P(SKIN[l.skin]); fcircle(cx, cy - 9, 4);
  P(CAPCOL[l.cap]); fcircle(cx, cy - 10, 3);
  P(SKIN[l.skin]); frect(cx - 8, cy - 5, 3, 3); frect(cx + 5, cy - 5, 3, 3);   // hands
}

// ---- DOWNHILL: HILL BOMB (race) and HILL JAM (race with a CPU rival) -------

constexpr float DH_LEN = 2600.0f;           // course length in pixels
constexpr int32_t DH_L = 40, DH_R = 200;    // road edges
constexpr int32_t DH_PY = 96;               // where pip sits on screen
enum { OB_CAN, OB_HOLE, OB_OIL, OB_RAMP, OB_SKATER };
struct ob_t { int32_t type; float x, y, vx; bool gone; };

std::vector<ob_t> dh_obs;
bool dh_jam = false, d_rewarded = false, d_won = false;
ev_state_t dhs = EV_READY;
float d_x, d_y, d_spd, d_vx, d_boost;
int32_t d_air, d_bail, d_slick, d_time, d_pts, d_cool, d_banner_t, d_smash, d_ko;
std::string d_banner;
struct dh_rival_t { float x, y, spd; int32_t bail, cool; } dr;
const look_t DH_RIVAL_LOOK = {6, 2, 1, 6, 1};

void d_say(const std::string &s) { d_banner = s; d_banner_t = 45; }

void dh_start(bool jam) {
  dh_jam = jam; dhs = EV_READY; dh_obs.clear();
  uint32_t seed = 12345;                      // same course every time, so it can be learned
  auto rn = [&]() { seed = seed * 1664525u + 1013904223u; return (int32_t)((seed >> 8) & 0xFFFF); };
  for(float y = 260; y < DH_LEN - 160; y += 50 + rn() % 46) {
    int32_t k = rn() % 10;
    int32_t type = k < 4 ? OB_CAN : k < 6 ? OB_HOLE : k < 8 ? OB_OIL : k < 9 ? OB_RAMP : (jam ? OB_CAN : OB_SKATER);
    float x = DH_L + 22 + rn() % (DH_R - DH_L - 44);
    dh_obs.push_back({type, x, y, type == OB_SKATER ? 0.7f : 0.0f, false});
  }
  d_x = 120; d_y = 0; d_spd = 0; d_vx = 0; d_boost = 0;
  d_air = d_bail = d_slick = d_time = d_pts = d_cool = d_banner_t = d_smash = d_ko = 0;
  d_rewarded = d_won = false;
  dr = {150.0f, 0.0f, 0.0f, 0, 0};
  pump_last = 0;
}

void dh_bail(const char *why) {
  if(d_bail > 0 || d_air > 0) return;
  d_bail = 50; d_spd = 0; d_boost = 0;
  d_pts = std::max<int32_t>(0, d_pts - 25);
  d_say(why);
  spawn_bail(d_x, DH_PY - 4, DECK[pal.look.board]);
  shake(3, 10); hit_stop(4); crash();
}

void update_dh() {
  if(dhs == EV_READY) {
    if(pressed(A)) { dhs = EV_PLAY; blip(900); }
    if(pressed(B)) scr = HUB;
    return;
  }
  if(dhs == EV_OVER) {
    if(!d_rewarded) {
      d_rewarded = true;
      int32_t score = d_pts + std::max<int32_t>(0, 2400 - d_time) / 2 + (d_won ? 300 : 0);
      d_pts = score;
      event_done(d_won ? score / 80 : score / 160);
      if(dh_jam) { if(score > pal.best_jam) pal.best_jam = score; }
      else if(d_won && (pal.best_hill == 0 || d_time < pal.best_hill)) pal.best_hill = d_time;
    }
    if(pressed(A) || pressed(B)) scr = HUB;
    return;
  }

  d_time++;
  if(d_banner_t > 0) d_banner_t--;
  if(d_cool > 0) d_cool--;
  int32_t dir = (button(RIGHT) ? 1 : 0) - (button(LEFT) ? 1 : 0);
  int32_t pp = pump_press();

  if(d_bail > 0) {
    d_bail--;
  } else {
    // carving left and right in rhythm builds speed; repeating a side loses it
    if(pp == 1)       { d_boost = std::min(1.6f, d_boost + 0.3f); blip(500 + (int32_t)(d_boost * 250), 14); }
    else if(pp == -1) { d_boost = std::max(0.0f, d_boost - 0.2f); }
    d_boost = std::max(0.0f, d_boost - 0.006f);
    d_spd += ((2.2f + d_boost) - d_spd) * 0.05f;
    if(d_slick > 0) { d_slick--; d_x += d_vx; d_vx *= 0.98f; }
    else            { d_vx = dir * 1.7f; d_x += d_vx; }
    if(d_x < DH_L + 6)  { d_x = DH_L + 6;  d_spd *= 0.97f; }
    if(d_x > DH_R - 6)  { d_x = DH_R - 6;  d_spd *= 0.97f; }
    d_y += d_spd;
    if(d_air > 0) d_air--;
  }

  // obstacles
  for(auto &o : dh_obs) {
    if(o.type == OB_SKATER && !o.gone) {
      o.x += o.vx;
      if(o.x < DH_L + 12 || o.x > DH_R - 12) o.vx = -o.vx;
    }
    if(o.gone) continue;
    float dy = o.y - d_y, dx = o.x - d_x;
    if(std::fabs(dy) > 22 || d_bail > 0) continue;
    switch(o.type) {
      case OB_CAN:    if(std::fabs(dx) < 9 && std::fabs(dy) < 8) dh_bail("TRASH CAN!"); break;
      case OB_HOLE:   if(std::fabs(dx) < 9 && std::fabs(dy) < 7) dh_bail("MANHOLE!"); break;
      case OB_SKATER: if(std::fabs(dx) < 10 && std::fabs(dy) < 9) dh_bail("WATCH IT!"); break;
      case OB_OIL:
        if(std::fabs(dx) < 12 && std::fabs(dy) < 8 && d_air == 0 && d_slick == 0) {
          d_slick = 38; d_vx = dir ? dir * 1.7f : (dx > 0 ? -0.9f : 0.9f);
          d_say("OIL SLICK!"); play(v_low, 150, 160, 60);
        }
        break;
      case OB_RAMP:
        if(std::fabs(dx) < 16 && std::fabs(dy) < 7 && d_air == 0) {
          d_air = 36; d_boost = std::min(1.6f, d_boost + 0.8f); d_pts += 50;
          d_say("AIR! +50"); jingle(1000);
        }
        break;
    }
  }

  // jam: fight, and a CPU rival who races you
  if(dh_jam) {
    if(pressed(A) && d_cool == 0 && d_bail == 0) {
      d_cool = 16; blip(300, 40);
      if(dr.bail == 0 && std::fabs(dr.x - d_x) < 26 && std::fabs(dr.y - d_y) < 18) {
        dr.bail = 70; d_ko++; d_pts += 200; d_say("KNOCKDOWN! +200");
        spawn_bail(dr.x, DH_PY + (dr.y - d_y) - 4, DECK[DH_RIVAL_LOOK.board]);
        shake(4, 10); hit_stop(5); crash();
      } else {
        for(auto &o : dh_obs) {
          if(o.gone || o.type != OB_CAN) continue;
          if(std::fabs(o.x - d_x) < 24 && std::fabs(o.y - d_y) < 16) {
            o.gone = true; d_smash++; d_pts += 50; d_say("SMASH! +50");
            spawn_sparks(o.x, DH_PY + (o.y - d_y), 0xBBB);
            shake(2, 6); play(v_low, 220, 90, 70);
            break;
          }
        }
      }
    }
    float skill = std::min(1.0f, pal.joust_wins / 6.0f);
    if(dr.bail > 0) { dr.bail--; dr.spd *= 0.9f; }
    else {
      float tgt = 2.45f + 0.5f * skill;
      if(dr.y < d_y - 70) tgt += 0.6f;
      if(dr.y > d_y + 70) tgt -= 0.5f;
      dr.spd += (tgt - dr.spd) * 0.05f;
      float want = dr.x;
      for(auto &o : dh_obs) {
        if(o.gone || o.type == OB_RAMP) continue;
        float dy = o.y - dr.y;
        if(dy > 6 && dy < 48 && std::fabs(o.x - dr.x) < 16) { want = o.x + (dr.x >= o.x ? 24 : -24); break; }
      }
      float sdx = want - dr.x;
      dr.x += std::max(-1.5f, std::min(1.5f, sdx));
      dr.x = std::max<float>(DH_L + 8, std::min<float>(DH_R - 8, dr.x));
      dr.y += dr.spd;
      if(dr.cool > 0) dr.cool--;
      // it fights back when you are next to it
      if(dr.cool == 0 && d_bail == 0 && d_air == 0 && std::fabs(dr.x - d_x) < 22 && std::fabs(dr.y - d_y) < 14
         && std::rand() % 100 < 5 + (int32_t)(skill * 4)) {
        dr.cool = 70; dh_bail("KICKED!");
      }
      for(auto &o : dh_obs) {
        if(o.gone || (o.type != OB_CAN && o.type != OB_HOLE)) continue;
        if(std::fabs(o.x - dr.x) < 8 && std::fabs(o.y - dr.y) < 7) { dr.bail = 45; spawn_bail(dr.x, DH_PY + (dr.y - d_y) - 4, DECK[DH_RIVAL_LOOK.board]); }
      }
    }
    if(dr.y >= DH_LEN && d_y < DH_LEN) { d_won = false; dhs = EV_OVER; return; }
  }
  if(d_y >= DH_LEN) { d_won = true; dhs = EV_OVER; jingle(1200); }
}

void dh_text_time(int32_t frames, int32_t x, int32_t y, uint16_t c) {
  T(str(frames / 40) + "." + str((frames % 40) * 10 / 40), x, y, c);
}

void draw_dh() {
  float cam = d_y - DH_PY;                     // world y at the top of the screen
  // alley walls either side, bricks scrolling past
  P(0x634); frect(0, 22, DH_L - 6, 202); frect(DH_R + 6, 22, 240 - DH_R - 6, 202);
  P(0x412);
  int32_t off = (int32_t)cam % 12; if(off < 0) off += 12;
  for(int32_t y = 22 - off; y < 224; y += 12) {
    hline(0, y, DH_L - 6); hline(DH_R + 6, y, 240 - DH_R - 6);
    int32_t row = (int32_t)((y + cam) / 12);
    for(int32_t x = (row % 2) * 6; x < DH_L - 6; x += 12) vline(x, y, 12);
    for(int32_t x = DH_R + 6 + (row % 2) * 6; x < 240; x += 12) vline(x, y, 12);
  }
  for(int32_t wy = ((int32_t)cam / 40) * 40 - 40; wy < cam + 260; wy += 40) {   // lit windows
    int32_t sy = (int32_t)(wy - cam);
    if(sy < 24 || sy > 214) continue;
    P(((wy / 40) % 3) ? 0x112 : 0xFD4); frect(8, sy, 14, 10); frect(DH_R + 22, sy, 14, 10);
  }
  // road, kerbs and lane dashes
  P(0x889); frect(DH_L, 22, DH_R - DH_L, 202);
  int32_t ko = (int32_t)cam % 16; if(ko < 0) ko += 16;
  for(int32_t y = 22 - ko; y < 224; y += 16) {
    bool a = (((int32_t)((y + cam) / 16)) % 2) == 0;
    P(a ? 0xE34 : 0xFFF); frect(DH_L - 6, y, 6, 16); frect(DH_R, y, 6, 16);
  }
  int32_t lo = (int32_t)cam % 32; if(lo < 0) lo += 32;
  P(0xAAB); for(int32_t y = 22 - lo; y < 224; y += 32) frect(119, y, 2, 14);

  // finish line
  int32_t fy = (int32_t)(DH_LEN - cam);
  if(fy > 14 && fy < 232) {
    for(int32_t x = DH_L; x < DH_R; x += 8) { P(((x / 8) % 2) ? 0x112 : 0xFFF); frect(x, fy, 8, 6); P(((x / 8) % 2) ? 0xFFF : 0x112); frect(x, fy + 6, 8, 6); }
  }

  // obstacles
  for(auto &o : dh_obs) {
    if(o.gone) continue;
    int32_t sx = (int32_t)o.x, sy = (int32_t)(o.y - cam);
    if(sy < 14 || sy > 236) continue;
    switch(o.type) {
      case OB_CAN:
        P(0x112, 8); fellipse(sx + 1, sy + 3, 8, 3);
        P(0x112); frect(sx - 6, sy - 9, 12, 14); P(0x99B); frect(sx - 5, sy - 8, 10, 12);
        P(0xCCE); frect(sx - 6, sy - 11, 12, 3); P(0x778); frect(sx - 2, sy - 4, 1, 7); frect(sx + 2, sy - 4, 1, 7);
        break;
      case OB_HOLE:
        P(0x112); fellipse(sx, sy, 10, 5); P(0x335); fellipse(sx, sy, 8, 4);
        P(0x112); hline(sx - 6, sy, 12); vline(sx, sy - 3, 6);
        break;
      case OB_OIL:
        P(0x112, 13); fellipse(sx, sy, 14, 6); P(0x226, 12); fellipse(sx - 2, sy, 10, 4);
        P(0x66F, 9); fellipse(sx - 5, sy - 1, 3, 1);
        break;
      case OB_RAMP:
        P(0x112); frect(sx - 17, sy - 8, 34, 16); P(0xFD4); frect(sx - 16, sy - 7, 32, 14);
        P(0x112); for(int32_t k = -16; k < 16; k += 8) fpoly({sx + k, sy + 7, sx + k + 4, sy + 7, sx + k + 8, sy - 7, sx + k + 4, sy - 7});
        break;
      default:
        draw_top(sx, sy, {2, 5, 8, 11, 3}, 1.57f);
    }
  }

  // the rival
  if(dh_jam) {
    int32_t ry = (int32_t)(DH_PY + dr.y - d_y);
    if(ry > 10 && ry < 240) {
      draw_top((int32_t)dr.x, ry, DH_RIVAL_LOOK, 1.57f);
      if(dr.bail > 0) dizzy((int32_t)dr.x, ry - 16);
    }
  }

  // pip
  int32_t lift = d_air > 0 ? (int32_t)(std::sin(d_air / 36.0f * 3.1416f) * 22) : 0;
  float lean = d_slick > 0 ? 1.57f + std::sin(now_f * 0.5f) * 0.6f : 1.57f + d_vx * 0.12f;
  if(d_bail > 0) { P(0x112, 8); fellipse((int32_t)d_x, DH_PY + 2, 10, 4); P(SHIRTCOL[pal.look.shirt]); fellipse((int32_t)d_x, DH_PY - 2, 8, 4); dizzy((int32_t)d_x, DH_PY - 14); }
  else draw_top((int32_t)d_x, DH_PY, pal.look, lean, lift);

  if(d_banner_t > 0) TC(d_banner, 120, 52 - (45 - d_banner_t) / 6, 0xFD4, true);

  // hud
  P(0x214); frect(0, 0, 240, 22); P(0x112); hline(0, 22, 240);
  T(dh_jam ? "HILL JAM" : "HILL BOMB", 6, 4, 0xFD4);
  dh_text_time(d_time, 6, 13, 0xFFF);
  if(dh_jam) { T("PTS " + str(d_pts), 150, 4, 0xFFF); T("A:ATTACK", 150, 13, 0xFC3); }
  else       { T("PTS " + str(d_pts), 150, 4, 0xFFF); }
  P(0x214); frect(0, 224, 240, 16); P(0x112); hline(0, 224, 240);
  T("SPD", 6, 228, 0xFD4);
  bar(30, 229, 70, (int32_t)(d_spd / 4.0f * 100), 0x4B5);
  P(0x112); frect(112, 230, 120, 4);
  P(0x445); frect(113, 231, 118, 2);
  if(dh_jam) { P(0xE34); frect(112 + (int32_t)(std::min(dr.y, DH_LEN) / DH_LEN * 116), 227, 3, 10); }
  P(0xFD4); frect(112 + (int32_t)(std::min(d_y, DH_LEN) / DH_LEN * 116), 227, 3, 10);

  if(dhs == EV_READY) {
    P(0x112, 12); frect(20, 96, 200, 92); P(0xFFF); rect(20, 96, 200, 92);
    TC(dh_jam ? "HILL JAM" : "HILL BOMB", 120, 104, 0xFD4);
    TC("ALTERNATE LEFT/RIGHT TO", 120, 120, 0xFFF);
    TC("CARVE FOR SPEED. DODGE", 120, 130, 0xFFF);
    TC(dh_jam ? "CANS, HOLES AND OIL. HIT A" : "CANS, HOLES, OIL AND SKATERS.", 120, 140, 0xFFF);
    TC(dh_jam ? "TO BEAT THE CPU RIVAL!" : "JUMP THE YELLOW RAMPS!", 120, 150, 0xFFF);
    TC("A TO START", 120, 170, 0xFD4);
  } else if(dhs == EV_OVER) {
    P(0x112, 12); frect(20, 90, 200, 100); P(0xFFF); rect(20, 90, 200, 100);
    TC(d_won ? "FINISH!" : "RIVAL WINS!", 120, 98, d_won ? 0xFD4 : 0xF66);
    T("TIME", 40, 116, 0xFFF); dh_text_time(d_time, 150, 116, 0xFFF);
    T("SCORE", 40, 128, 0xFFF); T(str(d_pts), 150, 128, 0xFC3);
    if(dh_jam) T("KOS " + str(d_ko) + "  SMASHES " + str(d_smash), 40, 140, 0xCCE);
    int32_t coins = d_won ? d_pts / 80 : d_pts / 160;
    TC("COINS +" + str(coins), 120, 156, 0xFC3);
    TC("A TO CONTINUE", 120, 174, 0xFD4);
  }
}

// ---- POOL JOUST: first to three knockdowns --------------------------------

struct jsk_t { float x, y, vx, vy, hd; int32_t pole, cool, down, score; };
struct rival_t { const char *name, *l1, *l2, *l3; look_t look; float skill; };
const rival_t RIVALS[3] = {
  {"POSER PAT",   "HEY, GO EASY ON ME!", "I JUST GOT THESE", "KNEE PADS.", {1, 4, 3, 1, 2}, 0.35f},
  {"SKETCHY SAM", "I KNOW EVERY INCH",   "OF THIS POOL. YOU", "DO NOT.",   {6, 0, 1, 6, 1}, 0.6f},
  {"BIG VEE",     "BRING SNACKS. AFTER", "I WIN, WE EAT.",   "",           {3, 5, 7, 8, 3}, 0.85f},
};
constexpr float POOL_CX = 120, POOL_CY = 124, POOL_RX = 100, POOL_RY = 80;

ev_state_t jos = EV_READY;
jsk_t jp, jr;
int32_t jo_rival = 0, jo_msg_t = 0;
bool jo_rewarded = false, jo_won = false;
std::string jo_msg;

void jo_say(const std::string &s) { jo_msg = s; jo_msg_t = 50; }

void joust_start() {
  jos = EV_READY; jo_rival = pal.joust_wins % 3; jo_rewarded = false; jo_won = false; jo_msg_t = 0;
  jp = {70.0f, POOL_CY, 0, 0, 0.0f, 0, 0, 0, 0};
  jr = {170.0f, POOL_CY, 0, 0, 3.1416f, 0, 0, 0, 0};
}

// bowl physics: input pushes, the slope pulls everything back to the middle
void jstep(jsk_t &s, float ax, float ay) {
  if(s.down > 0) { s.vx *= 0.9f; s.vy *= 0.9f; s.x += s.vx; s.y += s.vy; return; }
  s.vx += ax * 0.17f - (s.x - POOL_CX) * 0.0036f;
  s.vy += ay * 0.17f - (s.y - POOL_CY) * 0.0048f;
  s.vx *= 0.986f; s.vy *= 0.986f;
  float sp = std::sqrt(s.vx * s.vx + s.vy * s.vy);
  if(sp > 3.4f) { s.vx *= 3.4f / sp; s.vy *= 3.4f / sp; sp = 3.4f; }
  s.x += s.vx; s.y += s.vy;
  float nx = (s.x - POOL_CX) / (POOL_RX - 6), ny = (s.y - POOL_CY) / (POOL_RY - 6);
  float d = nx * nx + ny * ny;
  if(d > 1.0f) {                               // bounce off the wall
    float k = 1.0f / std::sqrt(d);
    s.x = POOL_CX + (s.x - POOL_CX) * k; s.y = POOL_CY + (s.y - POOL_CY) * k;
    s.vx *= -0.5f; s.vy *= -0.5f;
  }
  if(sp > 0.45f) s.hd = std::atan2(s.vy, s.vx);
}

void jknock(jsk_t &victim, jsk_t &by, const look_t &vlook) {
  victim.down = 55; by.score++;
  victim.vx += std::cos(by.hd) * 2.2f; victim.vy += std::sin(by.hd) * 2.2f;
  spawn_bail(victim.x, victim.y, DECK[vlook.board]);
  shake(4, 10); hit_stop(6); crash();
}

void update_joust() {
  if(jos == EV_READY) {
    if(pressed(A)) { jos = EV_PLAY; blip(900); }
    if(pressed(B)) scr = HUB;
    return;
  }
  if(jos == EV_OVER) {
    if(!jo_rewarded) {
      jo_rewarded = true;
      if(jo_won) { pal.joust_wins++; event_done(25); } else event_done(5);
    }
    if(pressed(A) || pressed(B)) scr = HUB;
    return;
  }
  if(jo_msg_t > 0) jo_msg_t--;
  const rival_t &rv = RIVALS[jo_rival];

  // player
  float ax = (button(RIGHT) ? 1.f : 0.f) - (button(LEFT) ? 1.f : 0.f);
  float ay = (button(DOWN) ? 1.f : 0.f) - (button(UP) ? 1.f : 0.f);
  if(ax != 0 && ay != 0) { ax *= 0.707f; ay *= 0.707f; }
  if(jp.down > 0) { if(--jp.down == 0 && jr.score < 3) jo_say("GET UP!"); }
  jstep(jp, jp.down > 0 ? 0 : ax, jp.down > 0 ? 0 : ay);
  if(pressed(A) && jp.pole == 0 && jp.cool == 0 && jp.down == 0) { jp.pole = 12; jp.cool = 26; blip(260, 50); }

  // rival
  if(jr.down > 0) { if(--jr.down == 0 && jp.score < 3) jo_say("HERE HE COMES"); }
  float dx = jp.x + jp.vx * 9 - jr.x, dy = jp.y + jp.vy * 9 - jr.y;
  float dist = std::sqrt(dx * dx + dy * dy) + 0.01f;
  float rax = dx / dist, ray = dy / dist;
  if(jp.pole > 3 && jp.pole < 10 && dist < 46) { float t = rax; rax = -ray; ray = t; }       // dodge a thrust
  jstep(jr, jr.down > 0 ? 0 : rax, jr.down > 0 ? 0 : ray);
  float face = std::cos(jr.hd) * dx / dist + std::sin(jr.hd) * dy / dist;
  if(jr.pole == 0 && jr.cool == 0 && jr.down == 0 && dist < 38 && face > 0.6f
     && (std::rand() % 100) < 3 + (int32_t)(rv.skill * 9)) { jr.pole = 12; jr.cool = 34; blip(220, 50); }

  // poles
  for(jsk_t *s : {&jp, &jr}) { if(s->pole > 0) s->pole--; if(s->cool > 0) s->cool--; }
  auto hit = [&](jsk_t &a, jsk_t &d, const look_t &dlook) {
    if(a.pole < 3 || a.pole > 9 || d.down > 0 || a.down > 0) return false;
    float tx = a.x + std::cos(a.hd) * 28, ty = a.y + std::sin(a.hd) * 28 - 4;
    float ddx = tx - d.x, ddy = ty - (d.y - 4);
    if(ddx * ddx + ddy * ddy < 11 * 11) { jknock(d, a, dlook); return true; }
    return false;
  };
  bool pk = hit(jp, jr, rv.look);
  if(pk) { jo_say("NICE HIT!"); }
  if(hit(jr, jp, pal.look)) jo_say("OUCH!");
  if(jp.score >= 3 || jr.score >= 3) { jo_won = jp.score >= 3; jos = EV_OVER; if(jo_won) jingle(1200); }
}

void draw_pole(const jsk_t &s) {
  float c = std::cos(s.hd), sn = std::sin(s.hd);
  float len = (s.pole > 3 && s.pole < 10) ? 28.0f : 15.0f;
  int32_t hx = (int32_t)(s.x + c * 5), hy = (int32_t)(s.y - 4 + sn * 5);
  int32_t tx = (int32_t)(s.x + c * len), ty = (int32_t)(s.y - 4 + sn * len);
  P(0x112); line(hx, hy + 1, tx, ty + 1); line(hx, hy - 1, tx, ty - 1);
  P(0xB84); line(hx, hy, tx, ty);
  P(0xFFF); fcircle(tx, ty, 3); P(0xFD4); fcircle(tx, ty, 2);
}

void draw_joust() {
  P(0x558); frect(0, 22, 240, 202);
  P(0x447); for(int32_t y = 22; y < 224; y += 12) hline(0, y, 240);
  // the pool
  P(0x112); fellipse(POOL_CX, POOL_CY, POOL_RX + 8, POOL_RY + 8);
  P(0xFFF); fellipse(POOL_CX, POOL_CY, POOL_RX + 6, POOL_RY + 6);
  P(0x6CE); fellipse(POOL_CX, POOL_CY, POOL_RX, POOL_RY);
  P(0x5BD); fellipse(POOL_CX, POOL_CY, POOL_RX * 0.78f, POOL_RY * 0.78f);
  P(0x4AC); fellipse(POOL_CX, POOL_CY, POOL_RX * 0.56f, POOL_RY * 0.56f);
  P(0x39B); fellipse(POOL_CX, POOL_CY, POOL_RX * 0.34f, POOL_RY * 0.34f);
  P(0x226); fellipse(POOL_CX, POOL_CY, 11, 8); P(0x112); fellipse(POOL_CX, POOL_CY, 8, 6);
  P(0x112); for(int32_t k = -4; k <= 4; k += 4) hline(POOL_CX - 7, POOL_CY + k, 14);

  // skaters, the lower one in front
  const rival_t &rv = RIVALS[jo_rival];
  bool pf = jp.y > jr.y;
  for(int32_t pass = 0; pass < 2; pass++) {
    bool isP = (pass == 0) != pf;
    jsk_t &s = isP ? jp : jr;
    const look_t &lk = isP ? pal.look : rv.look;
    if(s.down > 0) {
      P(0x112, 8); fellipse((int32_t)s.x, (int32_t)s.y + 2, 10, 4);
      P(SHIRTCOL[lk.shirt]); fellipse((int32_t)s.x, (int32_t)s.y - 2, 8, 4);
      dizzy((int32_t)s.x, (int32_t)s.y - 12);
    } else {
      draw_top((int32_t)s.x, (int32_t)s.y, lk, s.hd);
      draw_pole(s);
    }
  }

  if(jo_msg_t > 0) TC(jo_msg, 120, 34, 0xFD4, true);

  top_bar("POOL JOUST", false);
  T("PIP " + str(jp.score), 112, 4, 0xFD4);
  T(std::string(rv.name).substr(0, 6) + " " + str(jr.score), 160, 4, 0xFFF);
  bottom_bar("A:JOUST", "D-PAD:SKATE", "FIRST TO 3");

  if(jos == EV_READY) {
    P(0x112, 12); frect(14, 84, 212, 100); P(0xFFF); rect(14, 84, 212, 100);
    TC("POOL JOUST", 120, 92, 0xFD4);
    TC(std::string("VS ") + rv.name, 120, 108, 0xFFF);
    TC(rv.l1, 120, 124, 0xCCE); TC(rv.l2, 120, 134, 0xCCE); TC(rv.l3, 120, 144, 0xCCE);
    TC("SKATE THE BOWL. A TO JOUST!", 120, 160, 0xFFF);
    TC("A TO START", 120, 172, 0xFD4);
  } else if(jos == EV_OVER) {
    P(0x112, 12); frect(24, 90, 192, 86); P(0xFFF); rect(24, 90, 192, 86);
    TC(jo_won ? "YOU WIN!" : "YOU LOSE!", 120, 98, jo_won ? 0xFD4 : 0xF66);
    TC("PIP " + str(jp.score) + " - " + str(jr.score) + " " + rv.name, 120, 114, 0xFFF);
    TC(jo_won ? "COINS +25" : "COINS +5", 120, 130, 0xFC3);
    if(jo_won) TC("NEXT RIVAL UNLOCKED", 120, 144, 0xF6A);
    TC("A TO CONTINUE", 120, 160, 0xFD4);
  }
}

// ---- HUB: the skate shop ---------------------------------------------------

constexpr int32_t N_HUB = 8;
const char *HUB_NAME[N_HUB]  = {"TRICK", "JUMP", "HILL", "JAM", "JOUST", "RUN", "BUY", "SCORE"};
const char *HUB_LONG[N_HUB]  = {"TRICK ATTACK", "SKY POP", "HILL BOMB", "HILL JAM", "POOL JOUST", "STREET RUN", "GRIP TAPE 15c", "BEST SCORES"};
int32_t hub_sel = 0;
screen_t hub_from = ROOM;
bool hub_scores = false;
int32_t hub_tip = 0;
std::string hub_l[3];            // what coach roxy is saying right now
int32_t hub_say_t = 0;

const char *TIPS[5][3] = {
  {"YO PIP!", "PICK AN EVENT", "AND SEND IT!"},
  {"PUMP LEFT, RIGHT,", "LEFT, RIGHT. KEEP", "THE RHYTHM!"},
  {"CHAIN TRICKS FOR", "A BIG COMBO. DON'T", "BAIL!"},
  {"COINS BUY GRIP", "TAPE. FRESH GRIP", "= GOOD VIBES."},
  {"IN THE POOL THE", "SLOPE IS YOUR", "FRIEND. USE IT."},
};

void hub_say(const char *a, const char *b, const char *c) {
  hub_l[0] = a; hub_l[1] = b; hub_l[2] = c;
  hub_say_t = 110;
}

void hub_enter(screen_t from) {
  scr = HUB; hub_from = from; hub_scores = false; hub_say_t = 0;
  hub_tip = std::rand() % 5;
}

bool hub_can_skate() {
  if(pal.energy < 15) { hub_say("YOU LOOK BEAT,", "PIP. GO TAKE", "A NAP FIRST!"); thud(); return false; }
  if(pal.hunger < 10) { hub_say("STOMACH GROWLING?", "GO GET A SNACK", "FIRST!"); thud(); return false; }
  return true;
}

void update_hub() {
  if(hub_say_t > 0) hub_say_t--;
  if(hub_scores) {
    if(pressed(A) || pressed(B)) { hub_scores = false; blip(400); }
    return;
  }
  if(pressed(Y)) { music_on = !music_on; blip(music_on ? 900 : 300, 40); }
  if(pressed(LEFT))  { hub_sel = wrapi(hub_sel - 1, N_HUB); blip(500, 30); }
  if(pressed(RIGHT)) { hub_sel = wrapi(hub_sel + 1, N_HUB); blip(500, 30); }
  if(pressed(B))     { scr = hub_from; blip(400); return; }
  if(!pressed(A)) return;

  switch(hub_sel) {
    case 0: if(hub_can_skate()) { scr = TRICK; trick_start(); blip(900); } break;
    case 1: if(hub_can_skate()) { scr = JUMP;  jump_start();  blip(900); } break;
    case 2: if(hub_can_skate()) { scr = HILL;  dh_start(false); blip(900); } break;
    case 3: if(hub_can_skate()) { scr = HILL;  dh_start(true);  blip(900); } break;
    case 4: if(hub_can_skate()) { scr = JOUST; joust_start(); blip(900); } break;
    case 5: if(hub_can_skate()) { scr = RUN;   run_start();   blip(900); } break;
    case 6:
      if(pal.coins >= 15) {
        pal.coins -= 15; pal.mood = clampi(pal.mood + 15, 0, 100);
        hub_say("FRESH GRIP TAPE!", "THAT'LL CHEER", "PIP UP. +MOOD"); jingle(880);
      } else { hub_say("COME BACK WITH", "15 COINS, KID.", ""); thud(); }
      break;
    case 7: hub_scores = true; blip(700); break;
  }
}

// coach roxy: spiky orange hair, goggles, denim vest
void draw_roxy(int32_t cx, int32_t cy) {
  clip(130, 22, 110, 140);
  // vest over a tee
  P(0x112); fpoly({cx - 44, cy + 70, cx - 34, cy + 34, cx + 34, cy + 34, cx + 44, cy + 70});
  P(0x37B); fpoly({cx - 42, cy + 70, cx - 32, cy + 36, cx + 32, cy + 36, cx + 42, cy + 70});
  P(0xFFF); fpoly({cx - 12, cy + 36, cx + 12, cy + 36, cx, cy + 60});
  P(0xF6A); frect(cx - 30, cy + 48, 10, 8);
  P(0xFD4); frect(cx + 22, cy + 52, 10, 8);
  P(0xD96); frect(cx - 9, cy + 22, 18, 16);

  // spiky hair behind the head
  P(0x112);
  fpoly({cx - 27, cy - 4, cx - 34, cy - 30, cx - 20, cy - 20, cx - 16, cy - 48, cx - 6, cy - 26,
         cx + 2, cy - 52, cx + 8, cy - 26, cx + 20, cy - 46, cx + 22, cy - 22, cx + 34, cy - 32, cx + 28, cy - 2});
  P(0xF83);
  fpoly({cx - 25, cy - 4, cx - 31, cy - 28, cx - 19, cy - 19, cx - 15, cy - 44, cx - 6, cy - 24,
         cx + 2, cy - 48, cx + 8, cy - 24, cx + 19, cy - 42, cx + 20, cy - 20, cx + 31, cy - 29, cx + 26, cy - 3});

  // head
  P(0x112); fellipse(cx, cy, 26, 30);
  P(0xFC9); fellipse(cx, cy, 24, 28);
  P(0xF83);
  fpoly({cx - 24, cy - 10, cx - 22, cy - 22, cx - 8, cy - 28, cx + 8, cy - 28, cx + 22, cy - 22, cx + 24, cy - 10,
         cx + 12, cy - 16, cx, cy - 18, cx - 12, cy - 16});

  // goggles pushed up on the forehead
  P(0x112); frect(cx - 26, cy - 17, 52, 4);
  fellipse(cx - 11, cy - 17, 8, 6); fellipse(cx + 11, cy - 17, 8, 6);
  P(0x6DF); fellipse(cx - 11, cy - 17, 6, 4); fellipse(cx + 11, cy - 17, 6, 4);
  P(0xFFF); frect(cx - 13, cy - 19, 3, 1); frect(cx + 9, cy - 19, 3, 1);

  // face
  for(int32_t s = -1; s <= 1; s += 2) {
    P(0xFFF); fellipse(cx + s * 9, cy - 2, 5, 4);
    P(0x112); frect(cx + s * 9 - 1, cy - 3, 3, 3);
  }
  P(0x112); frect(cx - 16, cy - 10, 12, 2); frect(cx + 4, cy - 9, 12, 2);
  P(0xD96); frect(cx - 2, cy + 2, 4, 6);
  P(0x112); fellipse(cx, cy + 14, 13, 8);
  P(0xFFF); frect(cx - 12, cy + 9, 24, 4);
  P(0xF6A); fellipse(cx, cy + 18, 7, 3);
  P(0xD96); frect(cx - 18, cy + 6, 2, 2); frect(cx + 14, cy + 6, 2, 2); frect(cx - 15, cy + 9, 2, 2);
  clip();
}

void hub_icon(int32_t i, int32_t cx, int32_t cy, bool sel) {
  uint16_t c = sel ? 0xFD4 : 0xCCE;
  P(c);
  switch(i) {
    case 0:  // half-pipe with a trick flag
      fpoly({cx - 14, cy - 12, cx - 9, cy - 12, cx - 8, cy + 4, cx + 8, cy + 4, cx + 9, cy - 12, cx + 14, cy - 12,
             cx + 12, cy + 8, cx - 12, cy + 8});
      P(0xE34); frect(cx - 4, cy - 14, 8, 2); frect(cx - 2, cy - 12, 4, 3);
      break;
    case 1:  // pole, bar and a jumper
      frect(cx + 8, cy - 14, 3, 28);
      P(0xE34); frect(cx + 2, cy - 8, 10, 2);
      P(c); frect(cx - 10, cy - 2, 5, 4); frect(cx - 9, cy + 2, 3, 6);
      P(0x3CB); frect(cx - 12, cy + 8, 10, 2);
      break;
    case 2:  // a road bending downhill with a flag
      fpoly({cx - 12, cy - 10, cx - 3, cy - 10, cx + 13, cy + 10, cx + 3, cy + 10});
      P(0xE34); frect(cx - 12, cy - 14, 2, 8); fpoly({cx - 10, cy - 14, cx - 3, cy - 12, cx - 10, cy - 10});
      break;
    case 3:  // a punch burst
      fpoly({cx, cy - 13, cx + 4, cy - 4, cx + 13, cy - 6, cx + 7, cy + 1, cx + 12, cy + 10, cx + 2, cy + 6,
             cx - 4, cy + 13, cx - 5, cy + 4, cx - 13, cy, cx - 5, cy - 5});
      P(sel ? 0xE34 : 0x778); fcircle(cx, cy, 3);
      break;
    case 4:  // pool and a joust stick
      fellipse(cx, cy, 14, 10); P(sel ? 0x6CE : 0x558); fellipse(cx, cy, 11, 7);
      P(0xB84); line(cx - 10, cy + 8, cx + 10, cy - 8); line(cx - 10, cy + 9, cx + 10, cy - 7);
      P(0xFFF); fcircle(cx + 10, cy - 8, 3);
      break;
    case 5:  // cone with speed lines
      fpoly({cx - 6, cy + 10, cx + 6, cy + 10, cx, cy - 8});
      P(0xFFF); frect(cx - 3, cy + 1, 6, 2);
      P(c); hline(cx - 14, cy - 2, 5); hline(cx - 14, cy + 3, 7); hline(cx + 9, cy - 2, 5);
      break;
    case 6:  // coin
      fcircle(cx, cy, 9); P(0xA70); circle(cx, cy, 6); frect(cx - 1, cy - 3, 2, 6);
      break;
    default: // trophy
      frect(cx - 8, cy - 12, 16, 12); frect(cx - 12, cy - 10, 4, 6); frect(cx + 8, cy - 10, 4, 6);
      frect(cx - 3, cy, 6, 6); frect(cx - 8, cy + 6, 16, 3);
      P(sel ? 0x426 : 0x214); frect(cx - 4, cy - 10, 3, 6);
  }
}

void draw_hub() {
  // brick wall
  P(0x346); frect(0, 22, SCREEN_WIDTH, 140);
  P(0x234);
  for(int32_t y = 22, row = 0; y < 162; y += 10, row++) {
    hline(0, y, SCREEN_WIDTH);
    for(int32_t x = (row % 2) * 12; x < SCREEN_WIDTH; x += 24) vline(x, y, 10);
  }

  // neon sign
  P(0x112); frect(6, 28, 112, 22);
  P(0xF6A); rect(8, 30, 108, 18); rect(9, 31, 106, 16);
  TC("SKATE SHOP", 62, 36, 0xFDE);

  // decks hung on the wall
  static const uint16_t DK1[3] = {0xE34, 0x3CB, 0x36E}, DK2[3] = {0xFD4, 0xFFF, 0xF83};
  for(int32_t i = 0; i < 3; i++) {
    int32_t x = 190 + i * 16;
    P(0x112); frect(x - 2, 26, 14, 46); fellipse(x + 5, 26, 7, 5); fellipse(x + 5, 72, 7, 5);
    P(DK1[i]); frect(x - 1, 26, 12, 46); fellipse(x + 5, 26, 6, 4); fellipse(x + 5, 72, 6, 4);
    P(DK2[i]); frect(x + 1, 38, 8, 6);
  }

  // speech bubble
  P(0x112); fpoly({10, 56, 112, 56, 116, 60, 116, 104, 112, 108, 10, 108, 6, 104, 6, 60});
  P(0xFFF); fpoly({12, 58, 110, 58, 114, 62, 114, 102, 110, 106, 12, 106, 8, 102, 8, 62});
  P(0x112); fpoly({114, 72, 142, 90, 114, 86});
  P(0xFFF); fpoly({113, 74, 136, 89, 113, 84});

  std::string l0, l1, l2;
  if(hub_say_t > 0)           { l0 = hub_l[0]; l1 = hub_l[1]; l2 = hub_l[2]; }
  else if(pal.energy < 25)    { l0 = "YOU LOOK BEAT,"; l1 = "PIP. TAKE A"; l2 = "NAP SOON!"; }
  else if(pal.hunger < 25)    { l0 = "SNACK TIME?"; l1 = "YOU'RE RUNNING"; l2 = "ON EMPTY."; }
  else                        { l0 = TIPS[hub_tip][0]; l1 = TIPS[hub_tip][1]; l2 = TIPS[hub_tip][2]; }
  T(l0, 14, 64, 0xE34);
  T(l1, 14, 77, 0x112);
  T(l2, 14, 90, 0x112);

  draw_roxy(180, 102);

  // counter, with her hands on it
  P(0x112); frect(0, 160, SCREEN_WIDTH, 24);
  P(0x842); frect(0, 162, SCREEN_WIDTH, 20);
  P(0xDA6); frect(0, 162, SCREEN_WIDTH, 3);
  P(0xD96); fellipse(150, 160, 12, 5); fellipse(210, 160, 12, 5);

  // event tiles: five at a time, scrolling with the selection
  int32_t first = clampi(hub_sel - 2, 0, N_HUB - 5);
  for(int32_t n = 0; n < 5; n++) {
    int32_t i = first + n;
    int32_t x = 2 + n * 48;
    bool sel = i == hub_sel;
    P(sel ? 0xFD4 : 0x112); frect(x, 185, 44, 38);
    P(sel ? 0x65C : 0x426); frect(x + 2, 187, 40, 34);
    hub_icon(i, x + 22, 200, sel);
    TC(HUB_NAME[i], x + 22, 211, sel ? 0xFD4 : 0xFFF);
  }
  if(first > 0)             { P(0xFD4); fpoly({2, 204, 8, 198, 8, 210}); }
  if(first < N_HUB - 5)     { P(0xFD4); fpoly({238, 204, 232, 198, 232, 210}); }

  top_bar("SKATE SHOP", false);
  T(music_on ? "Y:MUSIC ON" : "Y:MUSIC OFF", 78, 4, 0xCCE);
  T(str(pal.coins) + "c", 160, 4, 0xFC3);
  bottom_bar(HUB_LONG[hub_sel], "B:BACK", "A:GO");

  if(hub_scores) {
    P(0x112); frect(24, 36, 192, 150);
    P(0xFFF); rect(24, 36, 192, 150); rect(25, 37, 190, 148);
    TC("BEST SCORES", 120, 46, 0xFD4);
    struct row_t { const char *n; std::string v; };
    std::string hill = pal.best_hill ? str(pal.best_hill / 40) + "." + str((pal.best_hill % 40) * 10 / 40) + "s" : "--";
    row_t rows[7] = {
      {"TRICK ATTACK", str(pal.best_trick)}, {"SKY POP", pal.best_height ? fmt_h(pal.best_height) : "--"},
      {"HILL BOMB", hill}, {"HILL JAM", str(pal.best_jam)},
      {"JOUST WINS", str(pal.joust_wins)}, {"STREET RUN", str(pal.best_run)}, {"CREW LEVEL", str(level())}};
    for(int32_t i = 0; i < 7; i++) {
      T(rows[i].n, 38, 64 + i * 13, 0xFFF);
      T(rows[i].v, 202 - 6 * (int32_t)rows[i].v.size(), 64 + i * 13, i == 6 ? 0xF6A : 0xFC3);
    }
    TC("A: CLOSE", 120, 168, 0xFD4);
  }
}

// ---------------------------------------------------------------------------
// saving: pip lives on in flash between power-offs
// ---------------------------------------------------------------------------
//
// One 4kB flash sector, 1.5MB into the chip (well clear of the ~300kB program),
// is used as 16 save slots of one 256 byte page each. It has to stay below 2MB:
// the Pico SDK is built for a 2MB board and its flash functions hard-assert (and
// freeze the console) on any offset beyond that. Every save is programmed into the next blank page, so
// the sector is only erased once per 16 saves. Loading picks the valid slot
// with the highest sequence number.

constexpr uint32_t SAVE_OFFSET = 1536 * 1024;
constexpr int32_t  N_SLOTS     = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE;

// A save is: magic, sequence, a list of values, crc. The magic doubles as the
// version, and each version only ever appends values, so a newer game can read
// an older save (the missing values just keep their defaults).
//   TMD1: 10 values (pet and look)   TMD2: 13 (+ best scores)   TMD3: 16 (+ new events)
constexpr int32_t N_VALS = 16;
constexpr uint32_t SAVE_MAGIC = 0x33444D54;               // "TMD3"
constexpr uint32_t MAGICS[3]  = {0x31444D54, 0x32444D54, 0x33444D54};
constexpr int32_t  COUNTS[3]  = {10, 13, 16};

struct save_t {
  uint32_t magic;
  uint32_t seq;
  int32_t  v[N_VALS];
  uint32_t crc;
};

uint32_t save_seq = 0;
int32_t  save_next = 0;        // next blank slot, N_SLOTS means "erase first"
save_t   last_saved = {};
int32_t  saved_flash = 0;      // frames left to show the "saved" dot

const uint32_t *slot_words(int32_t i) {
  return (const uint32_t *)(XIP_BASE + SAVE_OFFSET + i * FLASH_PAGE_SIZE);
}

// FNV-1a over a run of bytes
uint32_t crc_bytes(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

uint32_t save_crc(const save_t &s) { return crc_bytes(&s, offsetof(save_t, crc)); }

// returns the number of values in a valid slot (10, 13 or 16), or 0 if it is blank or damaged
int32_t slot_values(const uint32_t *w) {
  for(int32_t k = 0; k < 3; k++) {
    if(w[0] != MAGICS[k]) continue;
    int32_t n = COUNTS[k];
    return w[2 + n] == crc_bytes(w, (2 + n) * 4) ? n : 0;
  }
  return 0;
}

save_t snapshot() {
  save_t s = {};
  s.magic = SAVE_MAGIC;
  s.seq = save_seq;
  int32_t *v = s.v;
  v[0] = pal.hunger; v[1] = pal.energy; v[2] = pal.mood; v[3] = pal.coins; v[4] = pal.hearts;
  v[5] = pal.look.face; v[6] = pal.look.cap; v[7] = pal.look.shirt; v[8] = pal.look.board; v[9] = pal.look.skin;
  v[10] = pal.best_trick; v[11] = pal.best_height; v[12] = pal.best_run;
  v[13] = pal.best_hill; v[14] = pal.best_jam; v[15] = pal.joust_wins;
  return s;
}

void reset_pal() {
  pal.look = {0, 0, 4, 3, 0};
  pal.hunger = 72;
  pal.energy = 80;
  pal.mood = 65;
  pal.coins = 20;
  pal.hearts = 0;
  pal.best_trick = pal.best_height = pal.best_run = 0;
  pal.best_hill = pal.best_jam = pal.joust_wins = 0;
  pal.sleeping = false;
}

// returns true if a save was found and applied
bool load_game() {
#ifdef NO_SAVE
  return false;
#endif
  const uint32_t *best = nullptr;
  int32_t best_n = 0;
  save_next = N_SLOTS;
  for(int32_t i = 0; i < N_SLOTS; i++) {
    const uint32_t *w = slot_words(i);
    int32_t n = slot_values(w);
    if(n && (!best || w[1] > best[1])) { best = w; best_n = n; }
    if(save_next == N_SLOTS && w[0] == 0xFFFFFFFF) save_next = i;
  }
  if(!best) return false;

  const int32_t *v = (const int32_t *)(best + 2);
  save_seq = best[1];
  pal.hunger = clampi(v[0], 0, 100);
  pal.energy = clampi(v[1], 0, 100);
  pal.mood   = clampi(v[2], 0, 100);
  pal.coins  = clampi(v[3], 0, 99999);
  pal.hearts = clampi(v[4], 0, 9999);
  pal.look.face  = clampi(v[5], 0, N_FACE - 1);
  pal.look.cap   = clampi(v[6], 0, N_CAP - 1);
  pal.look.shirt = clampi(v[7], 0, N_SHIRT - 1);
  pal.look.board = clampi(v[8], 0, N_BOARD - 1);
  pal.look.skin  = clampi(v[9], 0, N_COLOR - 1);
  if(best_n >= 13) {
    pal.best_trick  = clampi(v[10], 0, 9999999);
    pal.best_height = clampi(v[11], 0, 200);
    pal.best_run    = clampi(v[12], 0, 9999999);
  }
  if(best_n >= 16) {
    pal.best_hill   = clampi(v[13], 0, 999999);
    pal.best_jam    = clampi(v[14], 0, 9999999);
    pal.joust_wins  = clampi(v[15], 0, 9999);
  }
  pal.sleeping = false;
  last_saved = snapshot();
  return true;
}

void write_save(save_t s) {
  s.seq = ++save_seq;
  s.crc = save_crc(s);

  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xFF, sizeof(page));
  memcpy(page, &s, sizeof(s));

  bool erase = save_next >= N_SLOTS;
  if(erase) save_next = 0;

#ifndef NO_SAVE
  // flash can't be read while it is being written, so no interrupt handlers
  // (which live in flash) may run meanwhile
  uint32_t ints = save_and_disable_interrupts();
  if(erase) flash_range_erase(SAVE_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(SAVE_OFFSET + save_next * FLASH_PAGE_SIZE, page, FLASH_PAGE_SIZE);
  restore_interrupts(ints);
#endif

  save_next++;
  last_saved = s;
  saved_flash = 40;
}

// writes only if something actually changed since the last save
void save_if_changed() {
  save_t s = snapshot();
  s.seq = last_saved.seq;
  if(memcmp(&s, &last_saved, offsetof(save_t, crc)) == 0) return;
  write_save(s);
}

void wipe_save() {
  reset_pal();
  write_save(snapshot());
}

// ---------------------------------------------------------------------------
// life of the pal
// ---------------------------------------------------------------------------

bool in_event() { return scr == RUN || scr == TRICK || scr == JUMP || scr == HILL || scr == JOUST; }

void tick_life() {
  if(in_event()) return;
  if(msg_t > 0) msg_t--;

  if(pal.sleeping) {
    if(now_f % 20 == 0) pal.energy = clampi(pal.energy + 1, 0, 100);
    if(pal.energy >= 100 && now_f % 20 == 0) { pal.sleeping = false; say("FULLY RESTED!"); }
    if(now_f % 240 == 0) pal.hunger = clampi(pal.hunger - 1, 0, 100);
    return;
  }

  if(now_f % 280 == 0) pal.hunger = clampi(pal.hunger - 1, 0, 100);
  if(now_f % 420 == 0) pal.energy = clampi(pal.energy - 1, 0, 100);
  if(now_f % 200 == 0) {
    // being hungry or tired slowly drags the mood down
    int32_t d = (pal.hunger < 25 ? -1 : 0) + (pal.energy < 20 ? -1 : 0);
    pal.mood = clampi(pal.mood + d, 0, 100);
  }
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------

void init() {
  v_blip  = voice(3, 20, 70, 30);
  v_low   = voice(2, 60, 60, 60, -40, 100);
  v_happy = voice(5, 30, 80, 80, 0, 0, 40);
  v_noise = voice(1, 90, 0, 140, -40, 160, 0, 90, 0);
  v_bass  = voice(2, 30, 55, 40);

  reset_pal();
  if(load_game()) say("WELCOME BACK!");

  town_init();
  bowl_init();
  run_start();
}

int32_t reset_hold = 0;      // frames Y has been held in the room
screen_t last_scr = ROOM;

// flash writes stall the game for a moment, so never save mid event

void update(uint32_t tick) {
  now_f = tick;
  if(freeze_t > 0) { freeze_t--; return; }     // hit-stop: everything holds for a few frames
  update_parts();
  if(shake_t > 0) shake_t--;
  music_step(tick);
  tick_life();

  // save whenever the screen changes (never mid run), and every 20 seconds
  if(scr != last_scr) {
    if(!in_event()) save_if_changed();
    last_scr = scr;
  } else if(tick > 0 && tick % 800 == 0 && !in_event()) {
    save_if_changed();
  }
  if(saved_flash > 0) saved_flash--;

  // hold Y in the room to start over with a new pal
  if(scr == ROOM && button(Y)) {
    if(++reset_hold >= 120) {
      reset_hold = 0;
      wipe_save();
      say("NEW PAL! GOOD LUCK");
      return;
    }
  } else {
    reset_hold = 0;
  }

  switch(scr) {
    case ROOM:   update_room();   break;
    case FEED:   update_feed();   break;
    case RUN:    update_run();    break;
    case CREATE: update_create(); break;
    case TOWN:   update_town();   break;
    case MEET:   update_meet();   break;
    case HUB:    update_hub();    break;
    case TRICK:  update_trick();  break;
    case JUMP:   update_jump();   break;
    case HILL:   update_dh();     break;
    case JOUST:  update_joust();  break;
  }
}

void draw(uint32_t tick) {
  (void)tick;
  P(0x112); clear();
  if(shake_t > 0) {
    int32_t m = shake_mag;
    camera((int32_t)(std::rand() % (2 * m + 1)) - m, (int32_t)(std::rand() % (2 * m + 1)) - m);
  }

  switch(scr) {
    case ROOM:   draw_room();   break;
    case FEED:   draw_feed();   break;
    case RUN:    draw_run();    break;
    case CREATE: draw_create(); break;
    case TOWN:   draw_town();   break;
    case MEET:   draw_meet();   break;
    case HUB:    draw_hub();    break;
    case TRICK:  draw_trick();  break;
    case JUMP:   draw_jump();   break;
    case HILL:   draw_dh();     break;
    case JOUST:  draw_joust();  break;
  }
  draw_parts();
  camera();

  if(scr == ROOM && reset_hold > 15) {
    int32_t w = (200 * reset_hold) / 120;
    P(0x112); frect(18, 100, 204, 34); P(0xFFF); rect(18, 100, 204, 34);
    TC("HOLD Y TO START OVER", 120, 106, 0xFFF);
    P(0x445); frect(20, 120, 200, 8); P(0xF35); frect(20, 120, w, 8);
  }
  if(saved_flash > 0) { P(0x3F5); frect(232, 4, 5, 5); }
}
