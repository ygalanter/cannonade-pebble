#include <pebble.h>
#include <stdlib.h>
#include <time.h>

#define BUILDING_COUNT 20
#define FRAME_MS 33
#define FIELD_SCREENS 2
#define GRAVITY 9.8f

#define PERSIST_LEVEL_INDEX 1
#define PERSIST_HUMAN_SCORE 2
#define PERSIST_COMPUTER_SCORE 3
#define PERSIST_LAST_LOST_HUMAN 4

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef enum {
  MODE_TITLE,
  MODE_PLAYING,
  MODE_PAN_TO_HUMAN,
  MODE_PAN_TO_COMPUTER,
  MODE_WAIT_COMPUTER,
  MODE_SHOT_HUMAN,
  MODE_SHOT_COMPUTER,
  MODE_EXPLODING
} Mode;

typedef enum {
  HIT_MISS,
  HIT_BUILDING,
  HIT_HUMAN,
  HIT_COMPUTER
} HitType;

typedef enum {
  PENDING_NONE,
  PENDING_HUMAN_TURN,
  PENDING_COMPUTER_TURN,
  PENDING_GAME_OVER
} PendingAction;

typedef struct {
  int x;
  int top;
  int width;
  uint8_t style;
} Building;

typedef struct {
  const char *name;
  float value;
} Level;

static const Level LEVELS[] = {
  {"Beginner", 1.5f},
  {"Medium", 2.0f},
  {"Advanced", 4.0f},
  {"Sniper", 1000.0f}
};

// Cannon fire: sharp crack then low boom
static const SpeakerNote SOUND_FIRE[] = {
  {55, SpeakerWaveformSquare,    40, 127, 0},
  {48, SpeakerWaveformSquare,    60, 115, 0},
  {40, SpeakerWaveformSquare,    80,  95, 0},
  {36, SpeakerWaveformSawtooth,  60,  70, 0},
};

// Building hit: sawtooth rubble cascade
static const SpeakerNote SOUND_BUILDING_HIT[] = {
  {52, SpeakerWaveformSawtooth,  70, 127, 0},
  {48, SpeakerWaveformSawtooth,  70, 112, 0},
  {43, SpeakerWaveformSawtooth,  80,  95, 0},
  {38, SpeakerWaveformSawtooth,  90,  80, 0},
  {33, SpeakerWaveformSawtooth, 110,  60, 0},
};

// Fort destroyed: full dramatic explosion cascade
static const SpeakerNote SOUND_FORT_DESTROYED[] = {
  {60, SpeakerWaveformSquare,    50, 127, 0},
  {55, SpeakerWaveformSawtooth,  60, 127, 0},
  {52, SpeakerWaveformSawtooth,  70, 120, 0},
  {48, SpeakerWaveformSawtooth,  80, 112, 0},
  {43, SpeakerWaveformSawtooth, 100, 100, 0},
  {38, SpeakerWaveformSawtooth, 120,  85, 0},
  {31, SpeakerWaveformSawtooth, 160,  70, 0},
  { 0, SpeakerWaveformSine,      80,   0, 0},
  {36, SpeakerWaveformSquare,   220,  65, 0},
};

static Window *s_window;
static Layer *s_scene_layer;
static AppTimer *s_frame_timer;

static GFont s_font_hud;
static GFont s_font_title;
static GFont s_font_label;

static GBitmap *s_img_background;
static GBitmap *s_img_building_1;
static GBitmap *s_img_building_2;
static GBitmap *s_img_fort_green;
static GBitmap *s_img_fort_red;
static GBitmap *s_img_angle_right_green;
static GBitmap *s_img_angle_left_red;
static GBitmap *s_img_speed_green;
static GBitmap *s_img_speed_red;

static Building s_buildings[BUILDING_COUNT];
static Mode s_mode = MODE_TITLE;
static PendingAction s_pending_action = PENDING_NONE;

static int s_screen_w;
static int s_screen_h;
static int s_view_offset;
static int s_wait_frames;

static int s_human_angle = 45;
static int s_human_speed = 80;
static int s_computer_angle_display = 45;
static int s_computer_speed_display = 60;
static int s_wind;

static int s_level_index = 1;
static int s_human_score;
static int s_computer_score;
static bool s_last_lost_human = true;
static bool s_title_is_game_over;
static char s_game_over_title[16] = "CANNONADE";

static float s_computer_speed_error;
static float s_computer_angle_error;
static float s_computer_angle;

static bool s_shell_visible;
static int s_shell_screen_x;
static int s_shell_screen_y;
static float s_shell_world_x;
static float s_shell_y;
static float s_shot_tan;
static float s_shot_speed2;
static float s_shot_cos2;
static GColor s_shell_color;

static bool s_explosion_visible;
static int s_explosion_x;
static int s_explosion_y;
static int s_explosion_frame;

static int s_anim_building_index = -1;
static int s_anim_start_top;
static int s_anim_target_top;

static int clamp_int(int value, int min, int max) {
  if (value < min) return min;
  if (value > max) return max;
  return value;
}

static int round_to_int(float value) {
  return (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

static float abs_float(float value) {
  return value < 0.0f ? -value : value;
}

static float sqrt_approx(float value) {
  if (value <= 0.0f) {
    return 0.0f;
  }

  float guess = value > 1.0f ? value : 1.0f;
  for (int i = 0; i < 8; i++) {
    guess = 0.5f * (guess + value / guess);
  }
  return guess;
}

static void trig_for_degrees(float degrees, float *sin_value, float *cos_value,
                             float *tan_value) {
  int32_t trig_angle = (int32_t)(degrees * (float)TRIG_MAX_ANGLE / 360.0f);
  float sine = (float)sin_lookup(trig_angle) / (float)TRIG_MAX_RATIO;
  float cosine = (float)cos_lookup(trig_angle) / (float)TRIG_MAX_RATIO;

  if (cosine > -0.0001f && cosine < 0.0001f) {
    cosine = cosine < 0.0f ? -0.0001f : 0.0001f;
  }

  *sin_value = sine;
  *cos_value = cosine;
  *tan_value = sine / cosine;
}

static void mark_scene_dirty(void) {
  if (s_scene_layer) {
    layer_mark_dirty(s_scene_layer);
  }
}

static void play_sound(const SpeakerNote *notes, uint32_t count, uint8_t volume) {
  speaker_stop();
  speaker_play_notes(notes, count, volume);
}

static void schedule_frame(void);

static const char *level_name(void) {
  return LEVELS[s_level_index].name;
}

static float level_value(void) {
  return LEVELS[s_level_index].value;
}

static void save_state(void) {
  persist_write_int(PERSIST_LEVEL_INDEX, s_level_index);
  persist_write_int(PERSIST_HUMAN_SCORE, s_human_score);
  persist_write_int(PERSIST_COMPUTER_SCORE, s_computer_score);
  persist_write_bool(PERSIST_LAST_LOST_HUMAN, s_last_lost_human);
}

static int read_persisted_int(int key, int fallback, int min, int max) {
  if (!persist_exists(key)) {
    return fallback;
  }
  return clamp_int(persist_read_int(key), min, max);
}

static void load_state(void) {
  s_level_index = read_persisted_int(PERSIST_LEVEL_INDEX, 1, 0, COUNT_OF(LEVELS) - 1);
  s_human_score = read_persisted_int(PERSIST_HUMAN_SCORE, 0, 0, 99);
  s_computer_score = read_persisted_int(PERSIST_COMPUTER_SCORE, 0, 0, 99);
  s_last_lost_human = persist_exists(PERSIST_LAST_LOST_HUMAN) ?
                      persist_read_bool(PERSIST_LAST_LOST_HUMAN) : true;
}

static void set_wind(void) {
  if ((rand() % 100) > 40) {
    s_wind = 1 + (rand() % 20);
  } else {
    s_wind = -(1 + (rand() % 20));
  }
}

static void reset_computer_errors(void) {
  float scaled_level = level_value() / 1.5f;
  s_computer_speed_error = -30.0f / scaled_level;
  s_computer_angle_error = -18.0f / scaled_level;
  s_computer_angle = 55.0f + (float)(1 + (rand() % 10));
}

static void start_field(void) {
  int slot_w = s_screen_w / 10;
  int building_w = slot_w - 4;
  int min_height = s_screen_h / 6;
  int max_extra = s_screen_h / 2;

  for (int i = 0; i < BUILDING_COUNT; i++) {
    int visible_height = min_height + (rand() % max_extra);
    s_buildings[i] = (Building) {
      .x = i * slot_w + 2,
      .top = s_screen_h - visible_height,
      .width = building_w,
      .style = (uint8_t)(rand() % 2)
    };
  }

  s_human_angle = 45;
  s_human_speed = 80;
  s_computer_angle_display = 45;
  s_computer_speed_display = 60;
  set_wind();
  reset_computer_errors();

  s_shell_visible = false;
  s_explosion_visible = false;
  s_anim_building_index = -1;
  s_view_offset = s_last_lost_human ? -s_screen_w + 3 : 0;
}

static void draw_text_centered(GContext *ctx, const char *text, GFont font,
                               GRect rect, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void draw_text_left(GContext *ctx, const char *text, GFont font,
                           GRect rect, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

static void draw_text_right(GContext *ctx, const char *text, GFont font,
                            GRect rect, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect, GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentRight, NULL);
}

static void draw_bitmap(GContext *ctx, GBitmap *bitmap, GRect rect, GCompOp mode) {
  if (!bitmap) {
    return;
  }

  graphics_context_set_compositing_mode(ctx, mode);
  graphics_draw_bitmap_in_rect(ctx, bitmap, rect);
}

static void draw_background(GContext *ctx) {
  if (s_img_background) {
    draw_bitmap(ctx, s_img_background, GRect(0, 0, s_screen_w, s_screen_h), GCompOpSet);
    return;
  }

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, s_screen_w, s_screen_h), 0, GCornerNone);
}

static GRect title_panel_rect(void) {
  return GRect(s_screen_w / 20, s_screen_h / 20,
               s_screen_w - s_screen_w / 10,
               s_screen_h - s_screen_h / 10);
}

static GRect title_level_rect(void) {
  GRect panel = title_panel_rect();
  return GRect(panel.origin.x + 8, panel.origin.y + 117, panel.size.w - 16, 32);
}

static bool point_in_rect(GPoint point, GRect rect) {
  return point.x >= rect.origin.x &&
         point.x < rect.origin.x + rect.size.w &&
         point.y >= rect.origin.y &&
         point.y < rect.origin.y + rect.size.h;
}

static void draw_building_bitmap(GContext *ctx, GBitmap *bitmap,
                                 int x, int top, int width) {
  if (top >= s_screen_h) {
    return;
  }

  draw_bitmap(ctx, bitmap, GRect(x, top, width, s_screen_h - top), GCompOpSet);
}

static void draw_buildings(GContext *ctx) {
  for (int i = 0; i < BUILDING_COUNT; i++) {
    Building *building = &s_buildings[i];
    int x = building->x + s_view_offset;
    if (x + building->width < 0 || x > s_screen_w) {
      continue;
    }

    if (i == 0) {
      draw_building_bitmap(ctx, s_img_fort_green, x, building->top, building->width);
    } else if (i == BUILDING_COUNT - 1) {
      draw_building_bitmap(ctx, s_img_fort_red, x, building->top, building->width);
    } else {
      draw_building_bitmap(ctx, building->style == 0 ? s_img_building_1 : s_img_building_2,
                           x, building->top, building->width);
    }
  }
}

static void draw_hud(GContext *ctx) {
  char buffer[16];

  draw_bitmap(ctx, s_img_angle_right_green, GRect(4, 6, 20, 20), GCompOpSet);
  draw_bitmap(ctx, s_img_speed_green, GRect(5, 40, 20, 20), GCompOpSet);
  snprintf(buffer, sizeof(buffer), "%d", s_human_angle);
  draw_text_left(ctx, buffer, s_font_hud, GRect(29, 0, 45, 31), GColorMintGreen);
  snprintf(buffer, sizeof(buffer), "%d", s_human_speed);
  draw_text_left(ctx, buffer, s_font_hud, GRect(29, 35, 45, 31), GColorMintGreen);

  draw_bitmap(ctx, s_img_angle_left_red, GRect(s_screen_w - 24, 6, 20, 20), GCompOpSet);
  draw_bitmap(ctx, s_img_speed_red, GRect(s_screen_w - 25, 40, 20, 20), GCompOpSet);
  snprintf(buffer, sizeof(buffer), "%d", s_computer_angle_display);
  draw_text_right(ctx, buffer, s_font_hud, GRect(s_screen_w - 75, 0, 45, 31), GColorRed);
  snprintf(buffer, sizeof(buffer), "%d", s_computer_speed_display);
  draw_text_right(ctx, buffer, s_font_hud, GRect(s_screen_w - 75, 35, 45, 31), GColorRed);

  if (s_wind >= 0) {
    snprintf(buffer, sizeof(buffer), "%d >>", s_wind);
  } else {
    snprintf(buffer, sizeof(buffer), "<< %d", -s_wind);
  }
  draw_text_centered(ctx, buffer, s_font_hud, GRect(65, 0, s_screen_w - 130, 31),
                     GColorWhite);
}

static void draw_shell_and_explosion(GContext *ctx) {
  if (s_shell_visible) {
    graphics_context_set_fill_color(ctx, s_shell_color);
    graphics_fill_circle(ctx, GPoint(s_shell_screen_x, s_shell_screen_y), 4);
  }

  if (s_explosion_visible) {
    int radius = 5 + s_explosion_frame;
    graphics_context_set_stroke_color(ctx, GColorOrange);
    graphics_draw_circle(ctx, GPoint(s_explosion_x, s_explosion_y), radius);
    graphics_context_set_stroke_color(ctx, GColorChromeYellow);
    graphics_draw_circle(ctx, GPoint(s_explosion_x, s_explosion_y), radius / 2 + 2);
  }
}

static void draw_title(GContext *ctx) {
  GRect panel = title_panel_rect();
  GRect level_rect = title_level_rect();
  char buffer[32];

  graphics_context_set_fill_color(ctx, GColorBlue);
  graphics_fill_rect(ctx, panel, 8, GCornersAll);

  const char *title = s_title_is_game_over ? s_game_over_title : "CANNONADE";
  draw_text_centered(ctx, title, s_font_title,
                     GRect(panel.origin.x + 4, panel.origin.y + 16,
                           panel.size.w - 8, 40),
                     GColorPastelYellow);

  if (s_title_is_game_over || s_human_score > 0 || s_computer_score > 0) {
    snprintf(buffer, sizeof(buffer), "Score %02d:%02d", s_human_score, s_computer_score);
  } else {
    snprintf(buffer, sizeof(buffer), "A Ballistic Game");
  }
  draw_text_centered(ctx, buffer, s_font_label,
                     GRect(panel.origin.x + 8, panel.origin.y + 78,
                           panel.size.w - 16, 32),
                     GColorWhite);

  snprintf(buffer, sizeof(buffer), "<< Level: %s >>", level_name());
  draw_text_centered(ctx, buffer, s_font_label, level_rect, GColorWhite);

  draw_text_centered(ctx, s_title_is_game_over ? "Tap to play again" :
                     (s_human_score == 0 && s_computer_score == 0 ?
                      "Tap to begin" : "Tap to continue"),
                     s_font_title,
                     GRect(panel.origin.x + 8, panel.origin.y + panel.size.h - 51,
                           panel.size.w - 16, 42),
                     GColorPastelYellow);
}

static void scene_update_proc(Layer *layer, GContext *ctx) {
  (void)layer;

  draw_background(ctx);
  draw_buildings(ctx);
  draw_hud(ctx);
  draw_shell_and_explosion(ctx);

  if (s_mode == MODE_TITLE) {
    draw_title(ctx);
  }
}

static HitType hit_building(float x, int y) {
  for (int i = 0; i < BUILDING_COUNT; i++) {
    Building *building = &s_buildings[i];
    if (x >= building->x && x < building->x + building->width &&
        y >= building->top) {
      s_explosion_x = s_shell_screen_x;
      s_explosion_y = s_shell_screen_y;

      if (i == 0 && y <= building->top + s_screen_h * 15 / 100) {
        s_anim_building_index = i;
        s_anim_start_top = building->top;
        s_anim_target_top = s_screen_h;
        return HIT_HUMAN;
      }

      if (i == BUILDING_COUNT - 1 && y <= building->top + s_screen_h * 15 / 100) {
        s_anim_building_index = i;
        s_anim_start_top = building->top;
        s_anim_target_top = s_screen_h;
        return HIT_COMPUTER;
      }

      s_anim_building_index = i;
      s_anim_start_top = building->top;
      s_anim_target_top = clamp_int(building->top + 20, 0, s_screen_h);
      return HIT_BUILDING;
    }
  }

  return HIT_MISS;
}

static void begin_explosion(PendingAction action) {
  s_pending_action = action;
  s_shell_visible = false;
  s_explosion_visible = true;
  s_explosion_frame = 0;
  s_mode = MODE_EXPLODING;
  schedule_frame();
}

static void show_game_over_title(void) {
  s_title_is_game_over = true;
  s_mode = MODE_TITLE;
  save_state();
  mark_scene_dirty();
}

static void start_computer_fire(void);

static void start_wait_for_computer(int frames) {
  s_wait_frames = frames;
  s_mode = MODE_WAIT_COMPUTER;
  schedule_frame();
}

static void handle_impact(HitType hit, bool human_shot) {
  if (hit == HIT_HUMAN) {
    snprintf(s_game_over_title, sizeof(s_game_over_title), "A.I. WON!");
    s_computer_score = clamp_int(s_computer_score + 1, 0, 99);
    s_last_lost_human = true;
    play_sound(SOUND_FORT_DESTROYED, COUNT_OF(SOUND_FORT_DESTROYED), 100);
    begin_explosion(PENDING_GAME_OVER);
  } else if (hit == HIT_COMPUTER) {
    snprintf(s_game_over_title, sizeof(s_game_over_title), "YOU WON!");
    s_human_score = clamp_int(s_human_score + 1, 0, 99);
    s_last_lost_human = false;
    play_sound(SOUND_FORT_DESTROYED, COUNT_OF(SOUND_FORT_DESTROYED), 100);
    begin_explosion(PENDING_GAME_OVER);
  } else if (hit == HIT_BUILDING) {
    play_sound(SOUND_BUILDING_HIT, COUNT_OF(SOUND_BUILDING_HIT), 85);
    begin_explosion(human_shot ? PENDING_COMPUTER_TURN : PENDING_HUMAN_TURN);
  }
}

static void update_human_shot(void) {
  s_shell_y = s_shell_world_x * s_shot_tan -
              (GRAVITY * s_shell_world_x * s_shell_world_x) / (s_shot_speed2 * s_shot_cos2) +
              (float)(s_screen_h - s_buildings[0].top);
  s_shell_screen_y = s_screen_h - round_to_int(s_shell_y);

  if (s_shell_world_x > s_screen_w - 20) {
    if (s_view_offset > -s_screen_w + 5) {
      s_view_offset -= 5;
    } else {
      s_view_offset = -s_screen_w + 3;
    }

    if (s_shell_world_x >= (FIELD_SCREENS * s_screen_w) - 20) {
      s_shell_screen_x = round_to_int(s_shell_world_x) - s_screen_w;
    } else {
      s_shell_screen_x = s_screen_w - 20;
    }
  } else {
    s_shell_screen_x = round_to_int(s_shell_world_x);
  }

  HitType hit = hit_building(s_shell_world_x, s_shell_screen_y);
  if (hit != HIT_MISS) {
    handle_impact(hit, true);
    return;
  }

  s_shell_world_x += 5.0f;

  if (s_shell_world_x >= FIELD_SCREENS * s_screen_w || s_shell_y <= 0.0f) {
    s_shell_visible = false;
    start_wait_for_computer(60);
    return;
  }

  schedule_frame();
}

static void update_computer_shot(void) {
  float shell_xx = (FIELD_SCREENS * s_screen_w) - s_shell_world_x;
  s_shell_y = shell_xx * s_shot_tan -
              (GRAVITY * shell_xx * shell_xx) / (s_shot_speed2 * s_shot_cos2) +
              (float)(s_screen_h - s_buildings[BUILDING_COUNT - 1].top);
  s_shell_screen_y = s_screen_h - round_to_int(s_shell_y);

  if (s_shell_world_x < s_screen_w + 20) {
    if (s_view_offset <= -5) {
      s_view_offset += 5;
    } else {
      s_view_offset = 0;
    }

    if (s_shell_world_x <= 20) {
      s_shell_screen_x = round_to_int(s_shell_world_x);
    } else {
      s_shell_screen_x = 20;
    }
  } else {
    s_shell_screen_x = round_to_int(s_shell_world_x) - s_screen_w;
  }

  HitType hit = hit_building(s_shell_world_x, s_shell_screen_y);
  if (hit != HIT_MISS) {
    handle_impact(hit, false);
    return;
  }

  s_shell_world_x -= 5.0f;

  if (s_shell_world_x <= 0.0f || s_shell_y <= 0.0f) {
    s_shell_visible = false;
    s_view_offset = 0;
    s_mode = MODE_PLAYING;
    mark_scene_dirty();
    return;
  }

  schedule_frame();
}

static void start_human_fire(void) {
  if (s_mode != MODE_PLAYING) {
    return;
  }

  float sine;
  float cosine;
  float tangent;
  trig_for_degrees((float)s_human_angle, &sine, &cosine, &tangent);

  float selected_speed = (float)s_human_speed;
  float speed_x = selected_speed * cosine + (float)s_wind;
  float speed_y = selected_speed * sine;
  float shell_speed2 = speed_x * speed_x + speed_y * speed_y;

  s_shell_color = GColorGreen;
  s_shell_visible = true;
  s_shell_world_x = (float)s_buildings[0].x;
  s_shot_tan = tangent;
  s_shot_speed2 = 2.0f * shell_speed2;
  s_shot_cos2 = cosine * cosine;
  s_mode = MODE_SHOT_HUMAN;

  play_sound(SOUND_FIRE, COUNT_OF(SOUND_FIRE), 80);
  light_enable_interaction();
  schedule_frame();
}

static void start_computer_fire(void) {
  s_view_offset = -s_screen_w + 3;

  float xx = -(float)(s_buildings[BUILDING_COUNT - 1].x - s_buildings[0].x);
  float yy = (float)(s_buildings[BUILDING_COUNT - 1].top - s_buildings[0].top);
  float angle = s_computer_angle;
  float sine;
  float cosine;
  float tangent;
  trig_for_degrees(angle, &sine, &cosine, &tangent);

  float landing_term = abs_float(xx * tangent + yy);
  if (landing_term < 1.0f) {
    landing_term = 1.0f;
  }
  float exact_speed = xx * sqrt_approx(GRAVITY / (2.0f * landing_term)) / cosine;
  float shell_angle = angle + s_computer_angle_error;
  float shell_sine;
  float shell_cosine;
  float shell_tangent;
  trig_for_degrees(shell_angle, &shell_sine, &shell_cosine, &shell_tangent);
  float shell_speed = -exact_speed - s_computer_speed_error;

  float display_speed_x = shell_speed * shell_cosine - (float)s_wind;
  float display_speed_y = shell_speed * shell_sine;
  s_computer_angle_display = round_to_int(shell_angle);
  s_computer_speed_display = round_to_int(sqrt_approx(display_speed_x * display_speed_x +
                                                      display_speed_y * display_speed_y));

  s_computer_angle_error = -s_computer_angle_error / level_value();
  s_computer_speed_error = -s_computer_speed_error / level_value();

  s_shell_color = GColorRed;
  s_shell_visible = true;
  s_shell_world_x = (float)(FIELD_SCREENS * s_screen_w);
  s_shot_tan = shell_tangent;
  s_shot_speed2 = 2.0f * shell_speed * shell_speed;
  s_shot_cos2 = shell_cosine * shell_cosine;
  s_mode = MODE_SHOT_COMPUTER;

  play_sound(SOUND_FIRE, COUNT_OF(SOUND_FIRE), 80);
  light_enable_interaction();
  schedule_frame();
}

static void complete_explosion(void) {
  s_explosion_visible = false;

  if (s_anim_building_index >= 0) {
    s_buildings[s_anim_building_index].top = s_anim_target_top;
    s_anim_building_index = -1;
  }

  switch (s_pending_action) {
    case PENDING_HUMAN_TURN:
      s_view_offset = 0;
      s_mode = MODE_PLAYING;
      mark_scene_dirty();
      break;
    case PENDING_COMPUTER_TURN:
      start_wait_for_computer(60);
      break;
    case PENDING_GAME_OVER:
      show_game_over_title();
      break;
    case PENDING_NONE:
    default:
      s_mode = MODE_PLAYING;
      mark_scene_dirty();
      break;
  }

  s_pending_action = PENDING_NONE;
}

static void frame_timer(void *context) {
  (void)context;
  s_frame_timer = NULL;

  switch (s_mode) {
    case MODE_PAN_TO_HUMAN:
      s_view_offset += 5;
      if (s_view_offset >= 0) {
        s_view_offset = 0;
        s_mode = MODE_PLAYING;
      } else {
        schedule_frame();
      }
      break;
    case MODE_PAN_TO_COMPUTER:
      s_view_offset -= 5;
      if (s_view_offset <= -s_screen_w + 3) {
        s_view_offset = -s_screen_w + 3;
        start_wait_for_computer(45);
      } else {
        schedule_frame();
      }
      break;
    case MODE_WAIT_COMPUTER:
      s_wait_frames--;
      if (s_wait_frames <= 0) {
        start_computer_fire();
      } else {
        schedule_frame();
      }
      break;
    case MODE_SHOT_HUMAN:
      update_human_shot();
      break;
    case MODE_SHOT_COMPUTER:
      update_computer_shot();
      break;
    case MODE_EXPLODING:
      if (s_anim_building_index >= 0) {
        Building *b = &s_buildings[s_anim_building_index];
        int step = (s_anim_target_top >= s_screen_h) ? 6 : 1;
        b->top += step;
        if (b->top >= s_anim_target_top) {
          b->top = s_anim_target_top;
          s_anim_building_index = -1;
        }
      }
      s_explosion_frame++;
      if (s_explosion_frame >= 28) {
        complete_explosion();
      } else {
        schedule_frame();
      }
      break;
    case MODE_TITLE:
    case MODE_PLAYING:
    default:
      break;
  }

  mark_scene_dirty();
}

static void schedule_frame(void) {
  if (s_frame_timer) {
    app_timer_cancel(s_frame_timer);
  }
  s_frame_timer = app_timer_register(FRAME_MS, frame_timer, NULL);
}

static void begin_from_title(void) {
  if (s_title_is_game_over) {
    start_field();
  }

  s_title_is_game_over = false;
  light_enable_interaction();

  if (s_last_lost_human) {
    s_view_offset = -s_screen_w + 3;
    s_mode = MODE_PAN_TO_HUMAN;
  } else {
    s_view_offset = 0;
    s_mode = MODE_PAN_TO_COMPUTER;
  }

  schedule_frame();
  mark_scene_dirty();
}

static void set_level_index(int level_index, bool should_persist) {
  int next_level = clamp_int(level_index, 0, COUNT_OF(LEVELS) - 1);

  if (next_level != s_level_index) {
    s_level_index = next_level;
    reset_computer_errors();
  }

  if (should_persist) {
    save_state();
  }

  mark_scene_dirty();
}

static void cycle_level(int delta) {
  int next_level = (s_level_index + delta + COUNT_OF(LEVELS)) % COUNT_OF(LEVELS);
  set_level_index(next_level, true);
}

static void compute_zones(int *fire_left, int *fire_right,
                          int *fire_top, int *fire_bottom) {
  *fire_left   = s_screen_w * 30 / 100;
  *fire_right  = s_screen_w * 70 / 100;
  *fire_top    = s_screen_h * 30 / 100;
  *fire_bottom = s_screen_h * 70 / 100;
}

// Called on touchdown — adjusts angle/speed immediately on press, like Fitbit onmousedown.
static void handle_play_aim(GPoint point) {
  if (s_mode != MODE_PLAYING) {
    return;
  }

  int fire_left, fire_right, fire_top, fire_bottom;
  compute_zones(&fire_left, &fire_right, &fire_top, &fire_bottom);

  if (point.x >= fire_left && point.x <= fire_right &&
      point.y >= fire_top  && point.y <= fire_bottom) {
    return;  // centre fire zone: ignore on press, handled on liftoff
  }

  if (point.y < fire_top) {
    s_human_angle = clamp_int(s_human_angle + 1, 5, 89);
  } else if (point.y > fire_bottom) {
    s_human_angle = clamp_int(s_human_angle - 1, 5, 89);
  } else if (point.x < fire_left) {
    s_human_speed = clamp_int(s_human_speed - 1, 5, 200);
  } else if (point.x > fire_right) {
    s_human_speed = clamp_int(s_human_speed + 1, 5, 200);
  }

  mark_scene_dirty();
}

// Called on liftoff — only fires if released inside the fire zone.
static void handle_play_touch(GPoint point) {
  if (s_mode != MODE_PLAYING) {
    return;
  }

  int fire_left, fire_right, fire_top, fire_bottom;
  compute_zones(&fire_left, &fire_right, &fire_top, &fire_bottom);

  if (point.x >= fire_left && point.x <= fire_right &&
      point.y >= fire_top  && point.y <= fire_bottom) {
    start_human_fire();
  }
}

static void handle_touch_point(GPoint point) {
  if (s_mode == MODE_TITLE) {
    GRect level_rect = title_level_rect();
    if (point_in_rect(point, level_rect)) {
      int midpoint = level_rect.origin.x + level_rect.size.w / 2;
      cycle_level(point.x >= midpoint ? 1 : -1);
      return;
    }

    begin_from_title();
    return;
  }

  handle_play_touch(point);
}

static void touch_handler(const TouchEvent *event, void *context) {
  (void)context;

  if (!event) {
    return;
  }

  if (event->type == TouchEvent_Touchdown) {
    handle_play_aim(GPoint(event->x, event->y));
    return;
  }

  if (event->type == TouchEvent_Liftoff) {
    handle_touch_point(GPoint(event->x, event->y));
  }
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;

  if (s_mode == MODE_TITLE) {
    window_stack_pop(true);
    return;
  }

  s_shell_visible = false;
  s_explosion_visible = false;
  s_anim_building_index = -1;
  s_title_is_game_over = false;
  s_mode = MODE_TITLE;
  if (s_frame_timer) {
    app_timer_cancel(s_frame_timer);
    s_frame_timer = NULL;
  }
  mark_scene_dirty();
}

static void click_config_provider(void *context) {
  (void)context;

  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void load_bitmaps(void) {
  s_img_background = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);
  s_img_building_1 = gbitmap_create_with_resource(RESOURCE_ID_BUILDING_1);
  s_img_building_2 = gbitmap_create_with_resource(RESOURCE_ID_BUILDING_2);
  s_img_fort_green = gbitmap_create_with_resource(RESOURCE_ID_FORT_GREEN);
  s_img_fort_red = gbitmap_create_with_resource(RESOURCE_ID_FORT_RED);
  s_img_angle_right_green = gbitmap_create_with_resource(RESOURCE_ID_ANGLE_RIGHT_GREEN);
  s_img_angle_left_red = gbitmap_create_with_resource(RESOURCE_ID_ANGLE_LEFT_RED);
  s_img_speed_green = gbitmap_create_with_resource(RESOURCE_ID_SPEED_GREEN);
  s_img_speed_red = gbitmap_create_with_resource(RESOURCE_ID_SPEED_RED);
}

static void destroy_bitmap(GBitmap **bitmap) {
  if (*bitmap) {
    gbitmap_destroy(*bitmap);
    *bitmap = NULL;
  }
}

static void unload_bitmaps(void) {
  destroy_bitmap(&s_img_background);
  destroy_bitmap(&s_img_building_1);
  destroy_bitmap(&s_img_building_2);
  destroy_bitmap(&s_img_fort_green);
  destroy_bitmap(&s_img_fort_red);
  destroy_bitmap(&s_img_angle_right_green);
  destroy_bitmap(&s_img_angle_left_red);
  destroy_bitmap(&s_img_speed_green);
  destroy_bitmap(&s_img_speed_red);
}

static void app_focus_handler(bool in_focus) {
  if (!in_focus) {
    if (s_frame_timer) {
      app_timer_cancel(s_frame_timer);
      s_frame_timer = NULL;
    }
  } else if (s_mode != MODE_TITLE && s_mode != MODE_PLAYING) {
    schedule_frame();
  }
}

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_screen_w = bounds.size.w;
  s_screen_h = bounds.size.h;

  window_set_background_color(window, GColorBlack);
  s_font_hud = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_font_title = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  s_font_label = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  load_bitmaps();

  s_scene_layer = layer_create(bounds);
  layer_set_update_proc(s_scene_layer, scene_update_proc);
  layer_add_child(window_layer, s_scene_layer);

  srand((unsigned int)time(NULL));
  load_state();
  start_field();

  touch_service_subscribe(touch_handler, NULL);
  app_focus_service_subscribe(app_focus_handler);
}

static void window_unload(Window *window) {
  (void)window;

  speaker_stop();
  app_focus_service_unsubscribe();
  touch_service_unsubscribe();
  if (s_frame_timer) {
    app_timer_cancel(s_frame_timer);
    s_frame_timer = NULL;
  }
  save_state();

  layer_destroy(s_scene_layer);
  s_scene_layer = NULL;
  unload_bitmaps();
}

static void init(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
  s_window = NULL;
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
