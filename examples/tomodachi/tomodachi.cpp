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

enum screen_t { ROOM, FEED, RUN, CREATE, TOWN, MEET, HUB, TRICK, JUMP };
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
  bool    sleeping;
} pal;

// everything else the screens share
uint32_t now_f = 0;           // frame counter
std::string msg;              // toast message
int32_t msg_t = 0;

void say(const std::string &m, int32_t frames = 80) { msg = m; msg_t = frames; }

int32_t level() { return 1 + pal.hearts / 3; }

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
  thud();
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
std::string t_banner;

void t_say(const std::string &s) { t_banner = s; t_banner_t = 50; }

void trick_start() {
  ts = EV_READY;
  t_phase = 0; t_amp = 0.5f;
  t_air = -1; t_airs = 0; t_score = 0; t_combo = 0; t_sum = 0; t_ntricks = 0;
  t_trick = -1; t_trick_t = 0; t_last_done = -1; t_pump_cd = 0; t_side = 1;
  t_lastk = -1; t_banner_t = 0; t_rewarded = false;
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

  if(t_air < 0) {
    // riding the transition back and forth
    t_phase += 6.2831853f / 72.0f;
    float s = std::sin(t_phase);
    float x = t_amp * s;

    // pump through the middle of the bowl to build speed
    if(pressed(A) && std::fabs(x) < 0.45f && t_pump_cd == 0) {
      t_amp = std::min(1.0f, t_amp + 0.14f);
      t_pump_cd = 8;
      blip(500 + (int32_t)(t_amp * 400), 30);
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
      t_combo = 0; t_amp = 0.5f;
      t_say("BAIL!"); thud();
    } else if(t_ntricks > 0) {                 // clean landing
      int32_t pts = t_sum * (1 + t_combo);
      t_score += pts; t_combo++;
      t_say("+" + str(pts)); jingle(990);
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
  TC(t_air >= 0 ? "B:GRAB  X:FLIP  Y:SPIN" : "PUMP THE MIDDLE WITH A", 120, 110, 0xFFF);

  if(ts == EV_READY) {
    P(0x112, 12); frect(24, 124, 192, 70); P(0xFFF); rect(24, 124, 192, 70);
    TC("TRICK ATTACK", 120, 132, 0xFD4);
    TC("PUMP WITH A TO GO HIGH", 120, 148, 0xFFF);
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
  j_clear = false; j_rewarded = false; j_banner_t = 0;
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
    if(pressed(A)) { j_meter = std::min(100.0f, j_meter + 7.0f); blip(400 + (int32_t)(j_meter * 6), 20); }
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
        j_banner = "CLIPPED THE BAR!"; thud();
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
  T("A:MASH", 6, 214, 0xFFF);
  T("B:POP IN THE RED", 234 - 6 * 16, 214, 0xFD4);

  P(0x214); frect(0, 224, SCREEN_WIDTH, 16);
  P(0x112); hline(0, 224, SCREEN_WIDTH);
  T("BEST " + fmt_h(std::max<int32_t>(j_best, pal.best_height)), 6, 228, 0xFFF);
  T("CLEARED " + str(j_cleared), 150, 228, 0xFC3);

  if(js == EV_READY) {
    P(0x112, 12); frect(24, 110, 192, 78); P(0xFFF); rect(24, 110, 192, 78);
    TC("SKY POP", 120, 118, 0xFD4);
    TC("MASH A TO BUILD SPEED", 120, 134, 0xFFF);
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

// ---- HUB: the skate shop ---------------------------------------------------

constexpr int32_t N_HUB = 5;
const char *HUB_NAME[N_HUB]  = {"TRICK", "JUMP", "RUN", "BUY", "SCORE"};
const char *HUB_LONG[N_HUB]  = {"TRICK ATTACK", "SKY POP", "STREET RUN", "GRIP TAPE 15c", "BEST SCORES"};
int32_t hub_sel = 0;
screen_t hub_from = ROOM;
bool hub_scores = false;
int32_t hub_tip = 0;
std::string hub_l[3];            // what coach roxy is saying right now
int32_t hub_say_t = 0;

const char *TIPS[4][3] = {
  {"YO PIP!", "PICK AN EVENT", "AND SEND IT!"},
  {"MASH A FOR SPEED,", "THEN POP IN THE", "RED ZONE!"},
  {"CHAIN TRICKS FOR", "A BIG COMBO. DON'T", "BAIL!"},
  {"COINS BUY GRIP", "TAPE. FRESH GRIP", "= GOOD VIBES."},
};

void hub_say(const char *a, const char *b, const char *c) {
  hub_l[0] = a; hub_l[1] = b; hub_l[2] = c;
  hub_say_t = 110;
}

void hub_enter(screen_t from) {
  scr = HUB; hub_from = from; hub_scores = false; hub_say_t = 0;
  hub_tip = std::rand() % 4;
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
  if(pressed(LEFT))  { hub_sel = wrapi(hub_sel - 1, N_HUB); blip(500, 30); }
  if(pressed(RIGHT)) { hub_sel = wrapi(hub_sel + 1, N_HUB); blip(500, 30); }
  if(pressed(B))     { scr = hub_from; blip(400); return; }
  if(!pressed(A)) return;

  switch(hub_sel) {
    case 0: if(hub_can_skate()) { scr = TRICK; trick_start(); blip(900); } break;
    case 1: if(hub_can_skate()) { scr = JUMP;  jump_start();  blip(900); } break;
    case 2: if(hub_can_skate()) { scr = RUN;   run_start();   blip(900); } break;
    case 3:
      if(pal.coins >= 15) {
        pal.coins -= 15; pal.mood = clampi(pal.mood + 15, 0, 100);
        hub_say("FRESH GRIP TAPE!", "THAT'LL CHEER", "PIP UP. +MOOD"); jingle(880);
      } else { hub_say("COME BACK WITH", "15 COINS, KID.", ""); thud(); }
      break;
    case 4: hub_scores = true; blip(700); break;
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
    case 0:  // half-pipe with a trick banner flag
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
    case 2:  // cone with speed lines
      fpoly({cx - 6, cy + 10, cx + 6, cy + 10, cx, cy - 8});
      P(0xFFF); frect(cx - 3, cy + 1, 6, 2);
      P(c); hline(cx - 14, cy - 2, 5); hline(cx - 14, cy + 3, 7); hline(cx + 9, cy - 2, 5);
      break;
    case 3:  // coin
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

  // event tiles
  for(int32_t i = 0; i < N_HUB; i++) {
    int32_t x = 2 + i * 48;
    bool sel = i == hub_sel;
    P(sel ? 0xFD4 : 0x112); frect(x, 185, 44, 38);
    P(sel ? 0x65C : 0x426); frect(x + 2, 187, 40, 34);
    hub_icon(i, x + 22, 200, sel);
    TC(HUB_NAME[i], x + 22, 211, sel ? 0xFD4 : 0xFFF);
  }

  top_bar("SKATE SHOP", false);
  T(str(pal.coins) + "c", 160, 4, 0xFC3);
  bottom_bar(HUB_LONG[hub_sel], "B:BACK", "A:GO");

  if(hub_scores) {
    P(0x112); frect(24, 52, 192, 124);
    P(0xFFF); rect(24, 52, 192, 124); rect(25, 53, 190, 122);
    TC("BEST SCORES", 120, 62, 0xFD4);
    T("TRICK ATTACK", 38, 82, 0xFFF);   T(str(pal.best_trick), 202 - 6 * (int32_t)str(pal.best_trick).size(), 82, 0xFC3);
    T("SKY POP", 38, 98, 0xFFF);        T(fmt_h(pal.best_height), 202 - 6 * (int32_t)fmt_h(pal.best_height).size(), 98, 0xFC3);
    T("STREET RUN", 38, 114, 0xFFF);    T(str(pal.best_run), 202 - 6 * (int32_t)str(pal.best_run).size(), 114, 0xFC3);
    T("CREW LEVEL", 38, 130, 0xFFF);    T(str(level()), 202 - 6 * (int32_t)str(level()).size(), 130, 0xF6A);
    TC("A: CLOSE", 120, 158, 0xFD4);
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

constexpr uint32_t SAVE_OFFSET   = 1536 * 1024;
constexpr uint32_t SAVE_MAGIC    = 0x32444D54;      // "TMD2"
constexpr uint32_t SAVE_MAGIC_V1 = 0x31444D54;      // "TMD1": before best scores existed
constexpr int32_t  N_SLOTS       = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE;

struct save_v1_t {                                  // old layout, still readable
  uint32_t magic;
  uint32_t seq;
  int32_t  hunger, energy, mood, coins, hearts;
  int32_t  face, cap, shirt, board, skin;
  uint32_t crc;
};

struct save_t {
  uint32_t magic;
  uint32_t seq;
  int32_t  hunger, energy, mood, coins, hearts;
  int32_t  face, cap, shirt, board, skin;
  int32_t  best_trick, best_height, best_run;
  uint32_t crc;
};

uint32_t save_seq = 0;
int32_t  save_next = 0;        // next blank slot, N_SLOTS means "erase first"
save_t   last_saved = {};
int32_t  saved_flash = 0;      // frames left to show the "saved" dot

const uint8_t *slot_ptr(int32_t i) {
  return (const uint8_t *)(XIP_BASE + SAVE_OFFSET + i * FLASH_PAGE_SIZE);
}

// FNV-1a over a run of bytes
uint32_t crc_bytes(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

uint32_t save_crc(const save_t &s) { return crc_bytes(&s, offsetof(save_t, crc)); }

bool slot_valid(const uint8_t *p) {
  const save_t *s = (const save_t *)p;
  return s->magic == SAVE_MAGIC && s->crc == save_crc(*s);
}

bool slot_valid_v1(const uint8_t *p) {
  const save_v1_t *s = (const save_v1_t *)p;
  return s->magic == SAVE_MAGIC_V1 && s->crc == crc_bytes(s, offsetof(save_v1_t, crc));
}

save_t snapshot() {
  save_t s = {};
  s.magic = SAVE_MAGIC;
  s.seq = save_seq;
  s.hunger = pal.hunger; s.energy = pal.energy; s.mood = pal.mood;
  s.coins = pal.coins;   s.hearts = pal.hearts;
  s.face = pal.look.face; s.cap = pal.look.cap; s.shirt = pal.look.shirt;
  s.board = pal.look.board; s.skin = pal.look.skin;
  s.best_trick = pal.best_trick; s.best_height = pal.best_height; s.best_run = pal.best_run;
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
  pal.sleeping = false;
}

// returns true if a save was found and applied
bool load_game() {
#ifdef NO_SAVE
  return false;
#endif
  const uint8_t *best = nullptr;
  bool best_is_v1 = false;
  uint32_t best_seq = 0;
  save_next = N_SLOTS;
  for(int32_t i = 0; i < N_SLOTS; i++) {
    const uint8_t *p = slot_ptr(i);
    uint32_t seq = ((const uint32_t *)p)[1];
    bool v2 = slot_valid(p), v1 = !v2 && slot_valid_v1(p);
    if((v2 || v1) && (!best || seq > best_seq)) { best = p; best_seq = seq; best_is_v1 = v1; }
    if(save_next == N_SLOTS && ((const uint32_t *)p)[0] == 0xFFFFFFFF) save_next = i;
  }
  if(!best) return false;

  // both layouts start with the same fields, so read those from either
  const save_v1_t *c = (const save_v1_t *)best;
  save_seq = best_seq;
  pal.hunger = clampi(c->hunger, 0, 100);
  pal.energy = clampi(c->energy, 0, 100);
  pal.mood   = clampi(c->mood, 0, 100);
  pal.coins  = clampi(c->coins, 0, 99999);
  pal.hearts = clampi(c->hearts, 0, 9999);
  pal.look.face  = clampi(c->face,  0, N_FACE - 1);
  pal.look.cap   = clampi(c->cap,   0, N_CAP - 1);
  pal.look.shirt = clampi(c->shirt, 0, N_SHIRT - 1);
  pal.look.board = clampi(c->board, 0, N_BOARD - 1);
  pal.look.skin  = clampi(c->skin,  0, N_COLOR - 1);
  if(!best_is_v1) {
    const save_t *n = (const save_t *)best;
    pal.best_trick  = clampi(n->best_trick, 0, 9999999);
    pal.best_height = clampi(n->best_height, 0, 200);
    pal.best_run    = clampi(n->best_run, 0, 9999999);
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

void tick_life() {
  if(scr == RUN || scr == TRICK || scr == JUMP) return;
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

  reset_pal();
  if(load_game()) say("WELCOME BACK!");

  town_init();
  bowl_init();
  run_start();
}

int32_t reset_hold = 0;      // frames Y has been held in the room
screen_t last_scr = ROOM;

// flash writes stall the game for a moment, so never save mid event
bool in_event() { return scr == RUN || scr == TRICK || scr == JUMP; }

void update(uint32_t tick) {
  now_f = tick;
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
  }
}

void draw(uint32_t tick) {
  (void)tick;
  P(0x112); clear();

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
  }

  if(scr == ROOM && reset_hold > 15) {
    int32_t w = (200 * reset_hold) / 120;
    P(0x112); frect(18, 100, 204, 34); P(0xFFF); rect(18, 100, 204, 34);
    TC("HOLD Y TO START OVER", 120, 106, 0xFFF);
    P(0x445); frect(20, 120, 200, 8); P(0xF35); frect(20, 120, w, 8);
  }
  if(saved_flash > 0) { P(0x3F5); frect(232, 4, 5, 5); }
}
