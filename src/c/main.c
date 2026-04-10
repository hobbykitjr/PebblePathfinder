/**
 * Pathfinder — Compass Navigation for Pebble
 * Targets: emery (200x228), gabbro (260x260)
 *
 * Save up to 6 locations, compass points you there.
 * Shows direction, distance, and estimated walking time.
 * Three display themes: Classic, Minimal, Tech.
 */

#include <pebble.h>
#include <stdlib.h>

// ============================================================================
// CONSTANTS
// ============================================================================
#define MAX_LOCS   6
#define NAME_LEN   20
#define PI_F       3.14159265f

// Persist keys
#define P_UNIT     0
#define P_THEME    1
#define P_POLL     2
#define P_LOC_CNT  3
#define P_LOC_BASE 10  // 10-15=lat, 20-25=lon, 30-35=name

// Themes
enum { THEME_CLASSIC=0, THEME_TECH, THEME_PREMIUM };
#define NUM_THEMES 3

// Units
enum { UNIT_MI=0, UNIT_KM };

// Poll rates
enum { POLL_MANUAL=0, POLL_LOW, POLL_MED, POLL_HIGH };
static const int s_poll_sec[] = {0, 300, 120, 30};

// ============================================================================
// DATA
// ============================================================================
typedef struct {
  int32_t lat;   // x10000
  int32_t lon;   // x10000
  char name[NAME_LEN];
  bool valid;
} Location;

static Window *s_win;
static Layer *s_canvas;

static Location s_locs[MAX_LOCS];
static int s_loc_count = 0;
static int s_sel = 0;              // Selected location index

static float s_heading = 0;        // Compass heading degrees
static bool s_compass_ok = false;
static float s_gps_lat = 0;        // Current position (x10000)
static float s_gps_lon = 0;
static bool s_gps_valid = false;
static time_t s_gps_time = 0;      // Last GPS update time

static int s_unit = UNIT_MI;
static int s_theme = THEME_CLASSIC;
static int s_poll = POLL_MED;

static int s_steps_at_gps = 0;     // Step count at last GPS update
static float s_step_dist = 0.7f;   // Meters per step estimate

static AppTimer *s_gps_timer = NULL;
static GBitmap *s_compass_bmp = NULL;

// ============================================================================
// MATH
// ============================================================================
static float psin(float deg) { return (float)sin_lookup(DEG_TO_TRIGANGLE((int)deg))/(float)TRIG_MAX_RATIO; }
static float pcos(float deg) { return (float)cos_lookup(DEG_TO_TRIGANGLE((int)deg))/(float)TRIG_MAX_RATIO; }

// Haversine distance in meters
static float haversine(float lat1, float lon1, float lat2, float lon2) {
  float dlat = (lat2-lat1) * PI_F / 180.0f;
  float dlon = (lon2-lon1) * PI_F / 180.0f;
  float a = psin(dlat*90/PI_F)*psin(dlat*90/PI_F) +
            pcos(lat1)*pcos(lat2)*psin(dlon*90/PI_F)*psin(dlon*90/PI_F);
  // Approximate: for small angles, distance ≈ R * c
  // Use simpler formula for Pebble: equirectangular approximation
  float x = dlon * pcos((lat1+lat2)/2);
  float y = dlat;
  float d_rad = x*x + y*y;
  // sqrt approximation
  float s = d_rad > 0 ? d_rad : 0.0001f;
  s = 0.5f*(s + d_rad/s); s = 0.5f*(s + d_rad/s); s = 0.5f*(s + d_rad/s);
  return s * 6371000.0f;  // Earth radius in meters
}

// Bearing from point 1 to point 2 in degrees
static float bearing(float lat1, float lon1, float lat2, float lon2) {
  float dlon = lon2 - lon1;
  float y = psin(dlon) * pcos(lat2);
  float x = pcos(lat1)*psin(lat2) - psin(lat1)*pcos(lat2)*pcos(dlon);
  int32_t angle = atan2_lookup((int)(y*TRIG_MAX_RATIO), (int)(x*TRIG_MAX_RATIO));
  float deg = (float)angle * 360.0f / (float)TRIG_MAX_ANGLE;
  if(deg < 0) deg += 360;
  return deg;
}

// Format distance string (no %f — Pebble doesn't support it)
static void fmt_dist(char *buf, int sz, float meters) {
  if(s_unit == UNIT_KM) {
    if(meters < 500) {
      snprintf(buf, sz, "%dm", (int)meters);
    } else {
      int km10 = (int)(meters / 100.0f);  // km * 10
      snprintf(buf, sz, "%d.%dkm", km10/10, km10%10);
    }
  } else {
    if(meters < 200) {
      snprintf(buf, sz, "%dft", (int)(meters*3.281f));
    } else {
      int mi10 = (int)(meters / 160.934f);  // miles * 10
      snprintf(buf, sz, "%d.%dmi", mi10/10, mi10%10);
    }
  }
}

// ============================================================================
// PERSIST
// ============================================================================
static void save_settings(void) {
  persist_write_int(P_UNIT, s_unit);
  persist_write_int(P_THEME, s_theme);
  persist_write_int(P_POLL, s_poll);
  persist_write_int(P_LOC_CNT, s_loc_count);
  for(int i=0; i<s_loc_count; i++) {
    persist_write_int(P_LOC_BASE+i, s_locs[i].lat);
    persist_write_int(P_LOC_BASE+10+i, s_locs[i].lon);
    persist_write_string(P_LOC_BASE+20+i, s_locs[i].name);
  }
}

static void load_settings(void) {
  if(persist_exists(P_UNIT)) s_unit = persist_read_int(P_UNIT);
  if(persist_exists(P_THEME)) s_theme = persist_read_int(P_THEME);
  if(persist_exists(P_POLL)) s_poll = persist_read_int(P_POLL);
  if(persist_exists(P_LOC_CNT)) {
    s_loc_count = persist_read_int(P_LOC_CNT);
    for(int i=0; i<s_loc_count && i<MAX_LOCS; i++) {
      if(persist_exists(P_LOC_BASE+i)) s_locs[i].lat = persist_read_int(P_LOC_BASE+i);
      if(persist_exists(P_LOC_BASE+10+i)) s_locs[i].lon = persist_read_int(P_LOC_BASE+10+i);
      if(persist_exists(P_LOC_BASE+20+i)) persist_read_string(P_LOC_BASE+20+i, s_locs[i].name, NAME_LEN);
      s_locs[i].valid = true;
    }
  } else {
    // Default location: Statue of Liberty
    s_loc_count = 1;
    s_locs[0].lat = 406892;   // 40.6892 * 10000
    s_locs[0].lon = -740475;  // -74.0475 * 10000
    snprintf(s_locs[0].name, NAME_LEN, "Lady Liberty");
    s_locs[0].valid = true;
  }
}

// ============================================================================
// DRAWING HELPERS
// ============================================================================

// Draw compass needle pointing at bearing
static void draw_needle(GContext *ctx, int cx, int cy, int len, float angle, GColor color) {
  int tip_x = cx + (int)(len * psin(angle));
  int tip_y = cy - (int)(len * pcos(angle));
  int tail_x = cx - (int)(8 * psin(angle));
  int tail_y = cy + (int)(8 * pcos(angle));
  int left_x = cx + (int)(5 * psin(angle-90));
  int left_y = cy - (int)(5 * pcos(angle-90));
  int right_x = cx + (int)(5 * psin(angle+90));
  int right_y = cy - (int)(5 * pcos(angle+90));

  graphics_context_set_fill_color(ctx, color);
  GPoint tri[] = {GPoint(tip_x,tip_y), GPoint(left_x,left_y), GPoint(right_x,right_y)};
  GPath *path = gpath_create(&(GPathInfo){.num_points=3, .points=(GPoint*)tri});
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

// ============================================================================
// SHARED: info overlay with black pill backgrounds for readability
// ============================================================================
static void draw_pill(GContext *ctx, GRect r) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(r.origin.x-2, r.origin.y-1, r.size.w+4, r.size.h+2), 4, GCornersAll);
}

static void draw_info(GContext *ctx, GRect b, float dist_m, const char *name,
                      bool has_dest, GColor text_c, GColor dim_c) {
  int w=b.size.w, h=b.size.h;
  bool rnd = (w==h);

  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char tbuf[8], dbuf_date[12];
  strftime(tbuf, sizeof(tbuf), clock_is_24h_style()?"%H:%M":"%I:%M", tm);
  strftime(dbuf_date, sizeof(dbuf_date), "%a %b %d", tm);

  GFont f_md = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f_sm = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  if(rnd) {
    // Round: centered text with pill backgrounds
    int top_y = 8;
    int bot_y = h - 40;

    // Top center: date + time on one line
    GRect top_r = GRect(30, top_y, w-60, 18);
    draw_pill(ctx, top_r);
    char dt_buf[20];
    snprintf(dt_buf, sizeof(dt_buf), "%s  %s", dbuf_date, tbuf);
    graphics_context_set_text_color(ctx, text_c);
    graphics_draw_text(ctx, dt_buf, f_sm, top_r,
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

    // Bottom: name + distance
    if(has_dest) {
      GRect name_r = GRect(20, bot_y, w-40, 18);
      draw_pill(ctx, name_r);
      graphics_context_set_text_color(ctx, text_c);
      graphics_draw_text(ctx, name, f_sm, name_r,
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

      char dbuf[16]; fmt_dist(dbuf, sizeof(dbuf), dist_m);
      GRect dist_r = GRect(w/2-30, bot_y+20, 60, 18);
      draw_pill(ctx, dist_r);
      graphics_draw_text(ctx, dbuf, f_md, dist_r,
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    } else {
      GRect nr = GRect(30, bot_y+4, w-60, 18);
      draw_pill(ctx, nr);
      graphics_context_set_text_color(ctx, dim_c);
      graphics_draw_text(ctx, "Add in Settings", f_sm, nr,
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
  } else {
    // Rect: 4 corners
    // Top-left: date
    graphics_context_set_text_color(ctx, dim_c);
    graphics_draw_text(ctx, dbuf_date, f_sm,
      GRect(4, 4, w/2-4, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    // Top-right: time
    graphics_context_set_text_color(ctx, text_c);
    graphics_draw_text(ctx, tbuf, f_md,
      GRect(w/2, 2, w/2-4, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    // Bottom
    if(has_dest) {
      graphics_context_set_text_color(ctx, text_c);
      graphics_draw_text(ctx, name, f_sm,
        GRect(4, h-20, w/2-4, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      char dbuf[16]; fmt_dist(dbuf, sizeof(dbuf), dist_m);
      graphics_draw_text(ctx, dbuf, f_md,
        GRect(w/2, h-22, w/2-4, 22), GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    } else {
      graphics_context_set_text_color(ctx, dim_c);
      graphics_draw_text(ctx, "Add waypoints in Settings", f_sm,
        GRect(4, h-18, w-8, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
  }
}

// ============================================================================
// THEME: CLASSIC
// ============================================================================
static void draw_classic(GContext *ctx, GRect b, float dest_bearing, float dist_m,
                         const char *name, bool has_dest) {
  int w=b.size.w, h=b.size.h, cx=w/2, cy=h/2;
  int r = (w<h?w:h)/2 - 16;

  // Warm background
  #ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromHEX(0x2a1a0a));
  GColor gold = GColorFromHEX(0xCCAA66);
  GColor gold_dk = GColorFromHEX(0x886622);
  #else
  graphics_context_set_fill_color(ctx, GColorBlack);
  GColor gold = GColorWhite;
  GColor gold_dk = GColorLightGray;
  #endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Double compass circle
  graphics_context_set_stroke_color(ctx, gold_dk);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(cx,cy), r);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(cx,cy), r-4);

  // Degree ticks
  for(int d=0; d<360; d+=10) {
    float a = d - s_heading;
    int inner = (d%90==0) ? r-14 : (d%30==0) ? r-10 : r-6;
    int x1 = cx + (int)(inner * psin(a));
    int y1 = cy - (int)(inner * pcos(a));
    int x2 = cx + (int)((r-5) * psin(a));
    int y2 = cy - (int)((r-5) * pcos(a));
    graphics_context_set_stroke_color(ctx, (d%30==0)?gold:gold_dk);
    graphics_draw_line(ctx, GPoint(x1,y1), GPoint(x2,y2));
  }

  // Cardinal directions
  GFont f_dir = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  const char *dirs[] = {"N","E","S","W"};
  float dir_az[] = {0,90,180,270};
  for(int i=0; i<4; i++) {
    float a = dir_az[i] - s_heading;
    int dx = cx + (int)((r-24) * psin(a));
    int dy = cy - (int)((r-24) * pcos(a));
    #ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, (i==0)?GColorRed:gold);
    #else
    graphics_context_set_text_color(ctx, GColorWhite);
    #endif
    graphics_draw_text(ctx, dirs[i], f_dir,
      GRect(dx-10,dy-14,20,28), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  // Destination needle
  if(has_dest) {
    float na = dest_bearing - s_heading;
    #ifdef PBL_COLOR
    draw_needle(ctx, cx, cy, r-32, na, GColorRed);
    #else
    draw_needle(ctx, cx, cy, r-32, na, GColorWhite);
    #endif
  }

  // Center dot
  graphics_context_set_fill_color(ctx, gold_dk);
  graphics_fill_circle(ctx, GPoint(cx,cy), 5);
  graphics_context_set_fill_color(ctx, gold);
  graphics_fill_circle(ctx, GPoint(cx,cy), 3);

  // Info overlay
  draw_info(ctx, b, dist_m, name, has_dest, gold, gold_dk);
}

// ============================================================================
// THEME: TECH
// ============================================================================
static void draw_tech(GContext *ctx, GRect b, float dest_bearing, float dist_m,
                      const char *name, bool has_dest) {
  int w=b.size.w, h=b.size.h, cx=w/2, cy=h/2;
  int r = (w<h?w:h)/2 - 16;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  #ifdef PBL_COLOR
  GColor gc = GColorFromHEX(0x00CC00);
  GColor gcd = GColorFromHEX(0x003300);
  #else
  GColor gc = GColorWhite;
  GColor gcd = GColorDarkGray;
  #endif

  // Grid
  graphics_context_set_stroke_color(ctx, gcd);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(cx,cy-r), GPoint(cx,cy+r));
  graphics_draw_line(ctx, GPoint(cx-r,cy), GPoint(cx+r,cy));
  graphics_draw_circle(ctx, GPoint(cx,cy), r/3);
  graphics_draw_circle(ctx, GPoint(cx,cy), r*2/3);

  // Compass ring
  graphics_context_set_stroke_color(ctx, gc);
  graphics_draw_circle(ctx, GPoint(cx,cy), r);

  // Degree ticks + numbers
  GFont f_sm = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  for(int d=0; d<360; d+=30) {
    float a = d - s_heading;
    int tx = cx + (int)((r-16) * psin(a));
    int ty = cy - (int)((r-16) * pcos(a));
    char dbuf[4]; snprintf(dbuf, sizeof(dbuf), "%03d", d);
    graphics_context_set_text_color(ctx, gc);
    graphics_draw_text(ctx, dbuf, f_sm,
      GRect(tx-14,ty-8,28,16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    int ix = cx + (int)(r * psin(a));
    int iy = cy - (int)(r * pcos(a));
    int ox = cx + (int)((r-5) * psin(a));
    int oy = cy - (int)((r-5) * pcos(a));
    graphics_draw_line(ctx, GPoint(ix,iy), GPoint(ox,oy));
  }

  // Bearing line to destination
  if(has_dest) {
    float a = dest_bearing - s_heading;
    int tx = cx + (int)((r-4) * psin(a));
    int ty = cy - (int)((r-4) * pcos(a));
    graphics_context_set_stroke_color(ctx, gc);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, GPoint(cx,cy), GPoint(tx,ty));
    // Arrow tip
    graphics_context_set_fill_color(ctx, gc);
    graphics_fill_circle(ctx, GPoint(tx,ty), 4);
    graphics_context_set_stroke_width(ctx, 1);
  }

  // Heading + bearing readout in center
  GFont f_lg = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  graphics_context_set_text_color(ctx, gc);
  char hbuf[16]; snprintf(hbuf, sizeof(hbuf), "HDG %03d", (int)s_heading);
  graphics_draw_text(ctx, hbuf, f_sm,
    GRect(0, cy-20, w, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  if(has_dest) {
    char bbuf[16]; snprintf(bbuf, sizeof(bbuf), "BRG %03d", (int)dest_bearing);
    graphics_draw_text(ctx, bbuf, f_sm,
      GRect(0, cy+4, w, 16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  // Info overlay
  draw_info(ctx, b, dist_m, name, has_dest, gc, gcd);
}

// ============================================================================
// THEME: PREMIUM (bitmap compass face + drawn elements)
// ============================================================================
static void draw_premium(GContext *ctx, GRect b, float dest_bearing, float dist_m,
                         const char *name, bool has_dest) {
  int w=b.size.w, h=b.size.h, cx=w/2, cy=h/2;
  int r = (w<h?w:h)/2 - 8;

  // Black background
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Draw compass face bitmap
  if(s_compass_bmp) {
    GRect src = gbitmap_get_bounds(s_compass_bmp);
    int bw = src.size.w, bh = src.size.h;
    graphics_draw_bitmap_in_rect(ctx, s_compass_bmp,
      GRect(cx-bw/2, cy-bh/2, bw, bh));
  }

  // Degree ticks (rotate with heading)
  #ifdef PBL_COLOR
  GColor gold = GColorFromHEX(0xCCAA66);
  GColor gold_dk = GColorFromHEX(0x886622);
  #else
  GColor gold = GColorWhite;
  GColor gold_dk = GColorLightGray;
  #endif

  graphics_context_set_stroke_width(ctx, 1);
  for(int d=0; d<360; d+=10) {
    float a = d - s_heading;
    int inner = (d%90==0) ? r-16 : (d%30==0) ? r-12 : r-8;
    int x1 = cx + (int)(inner * psin(a));
    int y1 = cy - (int)(inner * pcos(a));
    int x2 = cx + (int)((r-4) * psin(a));
    int y2 = cy - (int)((r-4) * pcos(a));
    graphics_context_set_stroke_color(ctx, (d%30==0)?gold:gold_dk);
    graphics_draw_line(ctx, GPoint(x1,y1), GPoint(x2,y2));
  }

  // Cardinal directions (larger, bold)
  GFont f_dir = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  const char *dirs[] = {"N","E","S","W"};
  float dir_az[] = {0,90,180,270};
  for(int i=0; i<4; i++) {
    float a = dir_az[i] - s_heading;
    int dx = cx + (int)((r-30) * psin(a));
    int dy = cy - (int)((r-30) * pcos(a));
    #ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, (i==0)?GColorRed:gold);
    #else
    graphics_context_set_text_color(ctx, GColorWhite);
    #endif
    graphics_draw_text(ctx, dirs[i], f_dir,
      GRect(dx-12,dy-16,24,32), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  // Intercardinal labels (NE, SE, SW, NW)
  GFont f_sm = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  const char *idirs[] = {"NE","SE","SW","NW"};
  float idir_az[] = {45,135,225,315};
  for(int i=0; i<4; i++) {
    float a = idir_az[i] - s_heading;
    int dx = cx + (int)((r-22) * psin(a));
    int dy = cy - (int)((r-22) * pcos(a));
    graphics_context_set_text_color(ctx, gold_dk);
    graphics_draw_text(ctx, idirs[i], f_sm,
      GRect(dx-10,dy-8,20,16), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  // Destination needle (thick, red with shadow)
  if(has_dest) {
    float na = dest_bearing - s_heading;
    // Shadow
    #ifdef PBL_COLOR
    draw_needle(ctx, cx+1, cy+1, r-36, na, GColorFromHEX(0x220000));
    draw_needle(ctx, cx, cy, r-36, na, GColorRed);
    #else
    draw_needle(ctx, cx, cy, r-36, na, GColorWhite);
    #endif
  }

  // Info overlay
  draw_info(ctx, b, dist_m, name, has_dest, gold, gold_dk);
}

// ============================================================================
// CANVAS
// ============================================================================
static void canvas_proc(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);

  // Calculate bearing and distance to selected waypoint
  float dest_bearing = 0;
  float dist_m = 0;
  bool has_dest = false;
  const char *dest_name = "";

  if(s_loc_count > 0 && s_sel < s_loc_count && s_gps_valid) {
    Location *loc = &s_locs[s_sel];
    float lat1 = s_gps_lat / 10000.0f;
    float lon1 = s_gps_lon / 10000.0f;
    float lat2 = (float)loc->lat / 10000.0f;
    float lon2 = (float)loc->lon / 10000.0f;
    dest_bearing = bearing(lat1, lon1, lat2, lon2);
    dist_m = haversine(lat1, lon1, lat2, lon2);

    // Step gap-fill: subtract estimated distance walked since last GPS
    #if PBL_API_EXISTS(health_service_sum_today)
    int cur_steps = (int)health_service_sum_today(HealthMetricStepCount);
    int steps_since = cur_steps - s_steps_at_gps;
    if(steps_since > 0) {
      float walked = steps_since * s_step_dist;
      dist_m -= walked;
      if(dist_m < 0) dist_m = 0;
    }
    #endif

    has_dest = true;
    dest_name = loc->name;
  }

  // Draw selected theme
  switch(s_theme) {
    case THEME_CLASSIC:  draw_classic(ctx, b, dest_bearing, dist_m, dest_name, has_dest); break;
    case THEME_TECH:     draw_tech(ctx, b, dest_bearing, dist_m, dest_name, has_dest); break;
    case THEME_PREMIUM:  draw_premium(ctx, b, dest_bearing, dist_m, dest_name, has_dest); break;
    default:             draw_classic(ctx, b, dest_bearing, dist_m, dest_name, has_dest); break;
  }

  // GPS staleness indicator
  if(s_gps_valid && s_gps_time > 0) {
    int age = time(NULL) - s_gps_time;
    if(age > 120) {
      GFont f_sm = fonts_get_system_font(FONT_KEY_GOTHIC_14);
      char abuf[12];
      snprintf(abuf, sizeof(abuf), "GPS %dm ago", age/60);
      graphics_context_set_text_color(ctx, GColorLightGray);
      graphics_draw_text(ctx, abuf, f_sm,
        GRect(0, b.size.h/2+20, b.size.w, 14), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
  }

  // Waypoint counter
  if(s_loc_count > 1) {
    GFont f_sm = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    char cbuf[8]; snprintf(cbuf, sizeof(cbuf), "%d/%d", s_sel+1, s_loc_count);
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, cbuf, f_sm,
      GRect(0, 18, b.size.w, 14), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

// ============================================================================
// COMPASS
// ============================================================================
static void compass_handler(CompassHeadingData heading_data) {
  if(heading_data.compass_status == CompassStatusDataInvalid) {
    s_compass_ok = false;
  } else {
    s_compass_ok = true;
    s_heading = (float)TRIGANGLE_TO_DEG((int)heading_data.magnetic_heading);
  }
  if(s_canvas) layer_mark_dirty(s_canvas);
}

// ============================================================================
// GPS TIMER
// ============================================================================
static void request_gps(void *data) {
  DictionaryIterator *it;
  if(app_message_outbox_begin(&it) == APP_MSG_OK) {
    dict_write_uint8(it, MESSAGE_KEY_REQUEST_GPS, 1);
    app_message_outbox_send();
  }
  // Schedule next poll
  if(s_poll > 0 && s_poll_sec[s_poll] > 0) {
    s_gps_timer = app_timer_register(s_poll_sec[s_poll]*1000, request_gps, NULL);
  }
}

// ============================================================================
// APPMESSAGE
// ============================================================================
static void inbox_cb(DictionaryIterator *it, void *c) {
  Tuple *t;

  // GPS update
  t = dict_find(it, MESSAGE_KEY_GPS_LAT);
  if(t) {
    s_gps_lat = (float)t->value->int32;
    s_gps_valid = true;
    s_gps_time = time(NULL);
    #if PBL_API_EXISTS(health_service_sum_today)
    s_steps_at_gps = (int)health_service_sum_today(HealthMetricStepCount);
    #endif
  }
  t = dict_find(it, MESSAGE_KEY_GPS_LON);
  if(t) s_gps_lon = (float)t->value->int32;

  // Settings (packed: unit | theme<<2 | poll<<4)
  t = dict_find(it, MESSAGE_KEY_SETTINGS);
  if(t) {
    int v = (int)t->value->int32;
    s_unit = v & 3;
    s_theme = (v>>2) & 3;
    s_poll = (v>>4) & 3;
  }

  // Locations
  t = dict_find(it, MESSAGE_KEY_LOC_COUNT);
  if(t) {
    s_loc_count = (int)t->value->int32;
    if(s_loc_count > MAX_LOCS) s_loc_count = MAX_LOCS;
    if(s_sel >= s_loc_count) s_sel = 0;
  }

  // Location data (check each slot)
  int key_base[] = {MESSAGE_KEY_LOC1_LAT, MESSAGE_KEY_LOC2_LAT, MESSAGE_KEY_LOC3_LAT,
                    MESSAGE_KEY_LOC4_LAT, MESSAGE_KEY_LOC5_LAT, MESSAGE_KEY_LOC6_LAT};
  for(int i=0; i<MAX_LOCS; i++) {
    t = dict_find(it, key_base[i]);
    if(t) {
      s_locs[i].lat = t->value->int32;
      s_locs[i].valid = true;
    }
    t = dict_find(it, key_base[i]+1);  // LON is always LAT+1
    if(t) s_locs[i].lon = t->value->int32;
    t = dict_find(it, key_base[i]+2);  // NAME is always LAT+2
    if(t) {
      strncpy(s_locs[i].name, t->value->cstring, NAME_LEN-1);
      s_locs[i].name[NAME_LEN-1] = '\0';
    }
  }

  save_settings();
  if(s_canvas) layer_mark_dirty(s_canvas);
}

// ============================================================================
// BUTTONS
// ============================================================================
static void up_click(ClickRecognizerRef ref, void *ctx) {
  if(s_loc_count > 0) {
    s_sel = (s_sel + s_loc_count - 1) % s_loc_count;
    if(s_canvas) layer_mark_dirty(s_canvas);
  }
}

static void down_click(ClickRecognizerRef ref, void *ctx) {
  if(s_loc_count > 0) {
    s_sel = (s_sel + 1) % s_loc_count;
    if(s_canvas) layer_mark_dirty(s_canvas);
  }
}

static void select_click(ClickRecognizerRef ref, void *ctx) {
  // Manual GPS refresh
  request_gps(NULL);
  vibes_short_pulse();
}

static void select_long(ClickRecognizerRef ref, void *ctx) {
  // Cycle theme
  s_theme = (s_theme + 1) % NUM_THEMES;
  save_settings();
  if(s_canvas) layer_mark_dirty(s_canvas);
}

static void back_click(ClickRecognizerRef ref, void *ctx) {
  window_stack_pop(true);
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// ============================================================================
// TICK (update time display)
// ============================================================================
static void tick_cb(struct tm *t, TimeUnits u) {
  if(s_canvas) layer_mark_dirty(s_canvas);
}

// ============================================================================
// WINDOW
// ============================================================================
static void win_load(Window *w) {
  Layer *wl = window_get_root_layer(w);
  GRect b = layer_get_bounds(wl);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_proc);
  layer_add_child(wl, s_canvas);
  window_set_click_config_provider(w, click_config);

  compass_service_set_heading_filter(TRIG_MAX_ANGLE / 360);
  compass_service_subscribe(compass_handler);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_cb);

  // Start GPS polling
  if(s_poll > 0) {
    s_gps_timer = app_timer_register(1000, request_gps, NULL);
  }
}

static void win_unload(Window *w) {
  compass_service_unsubscribe();
  tick_timer_service_unsubscribe();
  if(s_gps_timer) { app_timer_cancel(s_gps_timer); s_gps_timer = NULL; }
  if(s_canvas) { layer_destroy(s_canvas); s_canvas = NULL; }
}

// ============================================================================
// LIFECYCLE
// ============================================================================
static void init(void) {
  load_settings();
  // Load compass face bitmap
  #ifdef PBL_PLATFORM_GABBRO
  s_compass_bmp = gbitmap_create_with_resource(RESOURCE_ID_COMPASS_FACE);
  #else
  s_compass_bmp = gbitmap_create_with_resource(RESOURCE_ID_COMPASS_FACE_EMERY);
  #endif
  s_win = window_create();
  window_set_background_color(s_win, GColorBlack);
  window_set_window_handlers(s_win, (WindowHandlers){.load=win_load, .unload=win_unload});
  app_message_register_inbox_received(inbox_cb);
  app_message_open(1024, 64);
  window_stack_push(s_win, true);
}

static void deinit(void) {
  window_destroy(s_win);
  if(s_compass_bmp) gbitmap_destroy(s_compass_bmp);
}

int main(void) { init(); app_event_loop(); deinit(); return 0; }
