#include <pebble.h>
#include "assert.h"
#include "bluetooth_indicator.h"
#include "battery_gauge.h"
#include "config_options.h"

// Define this during development to make it easier to see animations
// in a timely fashion.
//#define FAST_TIME 1

// Define this to enable the FB-grabbing hack, which might break at
// the next SDK update.
//#define FB_HACK 1

// Define this to limit the set of sprites to just the Tardis (to
// reduce resource size).  You also need to remove the other sprites
// from the resource file, of course.
//#define TARDIS_ONLY 1

#define SCREEN_WIDTH 144
#define SCREEN_HEIGHT 168

// The frequency throughout the day at which the buzzer sounds, in seconds.
#define BUZZER_FREQ 3600

// Amount of time, in seconds, to ring the buzzer before the hour.
#define BUZZER_ANTICIPATE 2

// Number of milliseconds per animation frame
#define ANIM_TICK_MS 50

// Number of frames of animation
#define NUM_TRANSITION_FRAMES_HOUR 24
#define NUM_TRANSITION_FRAMES_STARTUP 10

typedef struct {
  GBitmap *bitmap;
  uint8_t *data;
} BitmapWithData;

Window *window;

BitmapWithData mins_background;

#ifdef FB_HACK
// The previous framebuffer data.
BitmapWithData fb_image;
bool first_update = true;
#endif  // FB_HACK

// The horizontal center point of the sprite.
int sprite_cx = 0;


Layer *face_layer;   // The "face", in both senses (and also the hour indicator).
Layer *minute_layer; // The minutes indicator.
Layer *second_layer; // The seconds indicator (a blinking colon).

int face_value;       // The current face on display (or transitioning into)
BitmapWithData face_image;  // The current face bitmap

bool face_transition; // True if the face is in transition
bool wipe_direction;  // True for left-to-right, False for right-to-left.
bool anim_direction;  // True to reverse tardis rotation.
int transition_frame; // Frame number of current transition
int num_transition_frames;  // Total frames for transition

int prev_face_value;  // The face we're transitioning from, or -1.
BitmapWithData prev_image;  // The previous face bitmap (only during a transition)

// The mask and image for the moving sprite across the wipe.
BitmapWithData sprite_mask;
BitmapWithData sprite;

// Triggered at ANIM_TICK_MS intervals for transition animations; also
// triggered occasionally to check the hour buzzer.
AppTimer *anim_timer = NULL;

// Triggered at 500 ms intervals to blink the colon.
AppTimer *blink_timer = NULL;

int minute_value;    // The current minute value displayed
int second_value;    // The current second value displayed.  Actually we only blink the colon, rather than actually display a value, but whatever.
bool hide_colon;     // Set true every half-second to blink the colon off.
int last_buzz_hour;  // The hour at which we last sounded the buzzer.

int face_resource_ids[13] = {
  RESOURCE_ID_TWELVE,
  RESOURCE_ID_ONE,
  RESOURCE_ID_TWO,
  RESOURCE_ID_THREE,
  RESOURCE_ID_FOUR,
  RESOURCE_ID_FIVE,
  RESOURCE_ID_SIX,
  RESOURCE_ID_SEVEN,
  RESOURCE_ID_EIGHT,
  RESOURCE_ID_NINE,
  RESOURCE_ID_TEN,
  RESOURCE_ID_ELEVEN,
  RESOURCE_ID_HURT,
};

#ifdef TARDIS_ONLY

#define SPRITE_TARDIS 0
#define NUM_SPRITES   1

#else

#define SPRITE_TARDIS 0
#define SPRITE_K9     1
#define SPRITE_DALEK  2
#define NUM_SPRITES   3

#endif  // TARDIS_ONLY


typedef struct {
  int tardis;
  bool flip_x;
} TardisFrame;

#define NUM_TARDIS_FRAMES 7
TardisFrame tardis_frames[NUM_TARDIS_FRAMES] = {
  { RESOURCE_ID_TARDIS_01, false },
  { RESOURCE_ID_TARDIS_02, false },
  { RESOURCE_ID_TARDIS_03, false },
  { RESOURCE_ID_TARDIS_04, false },
  { RESOURCE_ID_TARDIS_04, true },
  { RESOURCE_ID_TARDIS_03, true },
  { RESOURCE_ID_TARDIS_02, true }
};

static const uint32_t tap_segments[] = { 75, 100, 75 };
VibePattern tap = {
  tap_segments,
  3,
};

// Reverse the bits of a byte.
// http://www-graphics.stanford.edu/~seander/bithacks.html#BitReverseTable
uint8_t reverse_bits(uint8_t b) {
  return ((b * 0x0802LU & 0x22110LU) | (b * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16; 
}

// Horizontally flips the indicated GBitmap in-place.  Requires
// that the width be a multiple of 8 pixels.
void flip_bitmap_x(GBitmap *image) {
  int height = image->bounds.size.h;
  int width = image->bounds.size.w;  // multiple of 8, by our convention.
  int width_bytes = width / 8;
  int stride = image->row_size_bytes; // multiple of 4, by Pebble.
  uint8_t *data = image->addr;

  for (int y = 0; y < height; ++y) {
    uint8_t *row = data + y * stride;
    for (int x1 = (width_bytes - 1) / 2; x1 >= 0; --x1) {
      int x2 = width_bytes - 1 - x1;
      uint8_t b = reverse_bits(row[x1]);
      row[x1] = reverse_bits(row[x2]);
      row[x2] = b;
    }
  }
}

#define RBUFFER_SIZE 256
typedef struct {
  ResHandle _rh;
  size_t _i;
  size_t _filled_size;
  size_t _bytes_read;
  uint8_t *_buffer;
} RBuffer;

// Begins reading from a raw resource.  Should be matched by a later
// call to rbuffer_deinit() to free this stuff.
void rbuffer_init(int resource_id, RBuffer *rb) {
  rb->_buffer = (uint8_t *)malloc(RBUFFER_SIZE);
  assert(rb->_buffer != NULL);
  
  rb->_rh = resource_get_handle(resource_id);
  rb->_i = 0;
  rb->_filled_size = resource_load_byte_range(rb->_rh, 0, rb->_buffer, RBUFFER_SIZE);
  rb->_bytes_read = rb->_filled_size;
}

// Gets the next byte from the rbuffer.  Returns EOF at end.
int rbuffer_getc(RBuffer *rb) {
  if (rb->_i >= RBUFFER_SIZE) {
    rb->_filled_size = resource_load_byte_range(rb->_rh, rb->_bytes_read, rb->_buffer, RBUFFER_SIZE);
    rb->_bytes_read += rb->_filled_size;
    rb->_i = 0;
  }
  if (rb->_i >= rb->_filled_size) {
    return EOF;
  }

  int result = rb->_buffer[rb->_i];
  rb->_i++;
  return result;
}

// Frees the resources reserved in rbuffer_init().
void rbuffer_deinit(RBuffer *rb) {
  assert(rb->_buffer != NULL);
  free(rb->_buffer);
  rb->_buffer = NULL;
}

// From bitmapgen.py:
/*
# Bitmap struct (NB: All fields are little-endian)
#         (uint16_t) row_size_bytes
#         (uint16_t) info_flags
#                         bit 0 : reserved (must be zero for bitmap files)
#                    bits 12-15 : file version
#         (int16_t)  bounds.origin.x
#         (int16_t)  bounds.origin.y
#         (int16_t)  bounds.size.w
#         (int16_t)  bounds.size.h
#         (uint32_t) image data (word-aligned, 0-padded rows of bits)
*/
typedef struct {
  uint16_t row_size_bytes;
  uint16_t info_flags;
  int16_t origin_x;
  int16_t origin_y;
  int16_t size_w;
  int16_t size_h;
} BitmapDataHeader;

BitmapWithData bwd_create(GBitmap *bitmap, void *data) {
  BitmapWithData bwd;
  bwd.bitmap = bitmap;
  bwd.data = data;
  return bwd;
}

void bwd_destroy(BitmapWithData *bwd) {
  if (bwd->bitmap != NULL) {
    gbitmap_destroy(bwd->bitmap);
    bwd->bitmap = NULL;
  }
  if (bwd->data != NULL) {
    free(bwd->data);
    bwd->data = NULL;
  }
}

// Used to unpack the integers of an rl2-encoding back into their
// original rle sequence.  See make_rle.py.
typedef struct {
  RBuffer *rb;
  int n;
  int b;
  int bi;
} Rl2Unpacker;

void rl2unpacker_init(Rl2Unpacker *rl2, RBuffer *rb, int n) {
  // assumption: n is an integer divisor of 8.
  assert(n * (8 / n) == 8);

  rl2->rb = rb;
  rl2->n = n;
  rl2->b = rbuffer_getc(rb);
  rl2->bi = 8;
}

// Gets the next integer from the rl2 encoding.  Returns EOF at end.
int rl2unpacker_getc(Rl2Unpacker *rl2) {
  if (rl2->b == EOF) {
    return EOF;
  }

  // First, count the number of zero chunks until we come to a nonzero chunk.
  int zero_count = 0;
  int bmask = (1 << rl2->n) - 1;
  int bv = (rl2->b & (bmask << (rl2->bi - rl2->n)));
  while (bv == 0) {
    ++zero_count;
    rl2->bi -= rl2->n;
    if (rl2->bi <= 0) {
      rl2->b = rbuffer_getc(rl2->rb);
      rl2->bi = 8;
      if (rl2->b == EOF) {
        return EOF;
      }
    }
    bv = (rl2->b & (bmask << (rl2->bi - rl2->n)));
  }

  // Infer from that the number of chunks, and hence the number of
  // bits, that make up the value we will extract.
  int num_chunks = (zero_count + 1);
  int bit_count = num_chunks * rl2->n;

  // OK, now we need to extract the next bitCount bits into a word.
  int result = 0;
  while (bit_count >= rl2->bi) {
    int mask = (1 << rl2->bi) - 1;
    int value = (rl2->b & mask);
    result = (result << rl2->bi) | value;
    bit_count -= rl2->bi;

    rl2->b = rbuffer_getc(rl2->rb);
    rl2->bi = 8;
    if (rl2->b == EOF) {
      break;
    }
  }

  if (bit_count > 0) {
    // A partial word in the middle of the byte.
    int bottom_count = rl2->bi - bit_count;
    assert(bottom_count > 0);
    int mask = ((1 << bit_count) - 1);
    int value = ((rl2->b >> bottom_count) & mask);
    result = (result << bit_count) | value;
    rl2->bi -= bit_count;
  }

  return result;
}
  

// Initialize a bitmap from an rle-encoded resource.  The returned
// bitmap must be released with bwd_destroy().  See make_rle.py for
// the program that generates these rle sequences.
BitmapWithData
rle_bwd_create(int resource_id) {
  RBuffer rb;
  rbuffer_init(resource_id, &rb);
  int width = rbuffer_getc(&rb);
  int height = rbuffer_getc(&rb);
  int stride = rbuffer_getc(&rb);
  int n = rbuffer_getc(&rb);

  Rl2Unpacker rl2;
  rl2unpacker_init(&rl2, &rb, n);

  size_t data_size = height * stride;
  size_t total_size = sizeof(BitmapDataHeader) + data_size;
  uint8_t *bitmap = (uint8_t *)malloc(total_size);
  assert(bitmap != NULL);
  memset(bitmap, 0, total_size);
  BitmapDataHeader *bitmap_header = (BitmapDataHeader *)bitmap;
  uint8_t *bitmap_data = bitmap + sizeof(BitmapDataHeader);
  bitmap_header->row_size_bytes = stride;
  bitmap_header->size_w = width;
  bitmap_header->size_h = height;

  // The initial value is 0.
  uint8_t *dp = bitmap_data;
  uint8_t *dp_stop = dp + stride * height;
  int value = 0;
  int b = 0;
  int count = rl2unpacker_getc(&rl2);
  assert(count > 0);
  // We discard the first, implicit black pixel; it's not part of the image.
  --count;
  while (count != EOF) {
    assert(dp < dp_stop);
    if (value) {
      // Generate count 1-bits.
      int b1 = b + count;
      if (b1 < 8) {
        // We're still within the same byte.
        int mask = ~((1 << (b)) - 1);
        mask &= ((1 << (b1)) - 1);
        *dp |= mask;
        b = b1;
      } else {
        // We've crossed over a byte boundary.
        *dp |= ~((1 << (b)) - 1);
        ++dp;
        b += 8;
        while (b1 / 8 != b / 8) {
          assert(dp < dp_stop);
          *dp = 0xff;
          ++dp;
          b += 8;
        }
        b1 = b1 % 8;
        assert(dp < dp_stop);
        *dp |= ((1 << (b1)) - 1);
        b = b1;
      }
    } else {
      // Skip over count 0-bits.
      b += count;
      dp += b / 8;
      b = b % 8;
    }
    value = 1 - value;
    count = rl2unpacker_getc(&rl2);
  }
  rbuffer_deinit(&rb);

  GBitmap *image = gbitmap_create_with_data(bitmap);
  return bwd_create(image, bitmap);
}

int check_buzzer() {
  // Rings the buzzer if it's almost time for the hour to change.
  // Returns the amount of time in ms to wait for the next buzzer.
  time_t now = time(NULL);  

  // What hour is it right now, including the anticipate offset?
  int this_hour = (now + BUZZER_ANTICIPATE) / BUZZER_FREQ;
  if (this_hour != last_buzz_hour) {
    if (last_buzz_hour != -1) {
      // Time to ring the buzzer.
      if (config.hour_buzzer) {
        vibes_enqueue_custom_pattern(tap);
        //vibes_double_pulse();
      }
    }

    // Now make sure we don't ring the buzzer again for this hour.
    last_buzz_hour = this_hour;
  }

  int next_hour = this_hour + 1;
  int next_buzzer_time = next_hour * BUZZER_FREQ - BUZZER_ANTICIPATE;

  return (next_buzzer_time - now) * 1000;
}

void set_next_timer();

// Triggered at ANIM_TICK_MS intervals for transition animations; also
// triggered occasionally to check the hour buzzer.
void handle_timer(void *data) {
  anim_timer = NULL;  // When the timer is handled, it is implicitly canceled.

  if (face_transition) {
    layer_mark_dirty(face_layer);
  }

  set_next_timer();
}

// Triggered at 500 ms intervals to blink the colon.
void handle_blink(void *data) {
  blink_timer = NULL;  // When the timer is handled, it is implicitly canceled.

  if (config.second_hand) {
    hide_colon = true;
    layer_mark_dirty(second_layer);
  }

  if (blink_timer != NULL) {
    app_timer_cancel(blink_timer);
    blink_timer = NULL;
  }
}

// Ensures the animation/buzzer timer is running.
void set_next_timer() {
  if (anim_timer != NULL) {
    app_timer_cancel(anim_timer);
    anim_timer = NULL;
  }
  int next_buzzer_ms = check_buzzer();

  if (face_transition) {
    // If the animation is underway, we need to fire the timer at
    // ANIM_TICK_MS intervals.
    anim_timer = app_timer_register(ANIM_TICK_MS, &handle_timer, 0);

  } else {
    // Otherwise, we only need a timer to tell us to buzz at (almost)
    // the top of the hour.
    anim_timer = app_timer_register(next_buzzer_ms, &handle_timer, 0);
  }
}

#ifdef FB_HACK
// Hack alert!  This is an opaque data structure, but we're looking
// inside it anyway.  Idea taken from
// http://memention.com/blog/2013/07/20/Yak-shaving-a-Pebble.html .
struct GContext {
  uint8_t *framebuffer;
};

// Initializes the indicated GBitmap with a copy of the current
// framebuffer data.  Hacky!  Free it later with bwd_destroy().
BitmapWithData
fb_bwd_create(struct GContext *ctx) {
  int width = SCREEN_WIDTH;
  int height = SCREEN_HEIGHT;
  int stride = ((width + 31) / 32) * 4;

  size_t data_size = height * stride;
  size_t total_size = sizeof(BitmapDataHeader) + data_size;
  uint8_t *bitmap = (uint8_t *)malloc(total_size);
  assert(bitmap != NULL);
  memset(bitmap, 0, total_size);
  BitmapDataHeader *bitmap_header = (BitmapDataHeader *)bitmap;
  uint8_t *bitmap_data = bitmap + sizeof(BitmapDataHeader);
  bitmap_header->row_size_bytes = stride;
  bitmap_header->size_w = width;
  bitmap_header->size_h = height;

  // This doesn't appear to be working yet.  Not sure where I should
  // be finding this data.
  uint8_t *framebuffer = ctx->framebuffer;
  memcpy(bitmap_data, framebuffer, stride * height);

  GBitmap *image = gbitmap_create_with_data(bitmap);
  return bwd_create(image, bitmap);
}
#endif  // FB_HACK


void stop_transition() {
  face_transition = false;

  // Release the transition resources.
  bwd_destroy(&prev_image);
  bwd_destroy(&sprite_mask);
  bwd_destroy(&sprite);

#ifdef FB_HACK
  bwd_destroy(&fb_image);
#endif  // FB_HACK

  // Stop the transition timer.
  if (anim_timer != NULL) {
    app_timer_cancel(anim_timer);
    anim_timer = NULL;
  }
}

void start_transition(int face_new, bool for_startup) {
  if (face_transition) {
    stop_transition();
  }

  // Update the face display.
  assert(prev_image.bitmap == NULL);
  prev_face_value = face_value;
  prev_image = face_image;

  face_value = face_new;
  face_image = rle_bwd_create(face_resource_ids[face_value]);

  face_transition = true;
  transition_frame = 0;
  num_transition_frames = NUM_TRANSITION_FRAMES_HOUR;

  int sprite_sel;

  if (for_startup) {
    // Force the right-to-left TARDIS transition at startup.
    wipe_direction = false;
    sprite_sel = 0;
    anim_direction = false;

    // We used to want this to go super-fast at startup, to match the
    // speed of the system wipe, but we no longer try to do this
    // (since the system wipe is different nowadays anyway).
    //num_transition_frames = NUM_TRANSITION_FRAMES_STARTUP;

  } else {
    // Choose a random transition at the top of the hour.
    wipe_direction = (rand() % 2) != 0;    // Sure, it's not 100% even, but whatever.
    sprite_sel = (rand() % NUM_SPRITES);
    anim_direction = (rand() % 2) != 0;
  }

  // Initialize the sprite.
  switch (sprite_sel) {
  case SPRITE_TARDIS:
    sprite_mask = rle_bwd_create(RESOURCE_ID_TARDIS_MASK);
    
    sprite_cx = 72;
    break;

#ifndef TARDIS_ONLY
  case SPRITE_K9:
    sprite_mask = rle_bwd_create(RESOURCE_ID_K9_MASK);
    sprite = rle_bwd_create(RESOURCE_ID_K9);
    sprite_cx = 41;

    if (wipe_direction) {
      flip_bitmap_x(sprite_mask.bitmap);
      flip_bitmap_x(sprite.bitmap);
      sprite_cx = sprite.bitmap->bounds.size.w - sprite_cx;
    }
    break;

  case SPRITE_DALEK:
    sprite_mask = rle_bwd_create(RESOURCE_ID_DALEK_MASK);
    sprite = rle_bwd_create(RESOURCE_ID_DALEK);
    sprite_cx = 74;

    if (wipe_direction) {
      flip_bitmap_x(sprite_mask.bitmap);
      flip_bitmap_x(sprite.bitmap);
      sprite_cx = sprite.bitmap->bounds.size.w - sprite_cx;
    }
    break;
#endif  // TARDIS_ONLY
  }

  // Start the transition timer.
  layer_mark_dirty(face_layer);
  set_next_timer();
}

void root_layer_update_callback(Layer *me, GContext* ctx) {
#ifdef FB_HACK
  if (fb_image.bitmap == NULL && first_update) {
    first_update = false;
    fb_image = fb_bwd_create(ctx);
  }
#endif
}

void face_layer_update_callback(Layer *me, GContext* ctx) {
  int ti = 0;
  
  if (face_transition) {
    // ti ranges from 0 to num_transition_frames over the transition.
    ti = transition_frame;
    transition_frame++;
    if (ti > num_transition_frames) {
      stop_transition();
    }
  }

  if (!face_transition) {
    // The simple case: no transition, so just hold the current frame.
    if (face_value >= 0 && face_image.bitmap != NULL) {
      GRect destination = layer_get_frame(me);
      destination.origin.x = 0;
      destination.origin.y = 0;
      
      graphics_context_set_compositing_mode(ctx, GCompOpAssign);
      graphics_draw_bitmap_in_rect(ctx, face_image.bitmap, destination);
    }

  } else {
    // The complex case: we animate a transition from one face to another.

    // How far is the total animation distance from offscreen to
    // offscreen?
    int sprite_width = sprite_mask.bitmap->bounds.size.w;
    int wipe_width = SCREEN_WIDTH + sprite_width;

    // Compute the current pixel position of the center of the wipe.
    // It might be offscreen on one side or the other.
    int wipe_x;
    wipe_x = wipe_width - ti * wipe_width / num_transition_frames;
    if (wipe_direction) {
      wipe_x = wipe_width - wipe_x;
    }
    wipe_x = wipe_x - (sprite_width - sprite_cx);

    GRect destination = layer_get_frame(me);
    destination.origin.x = 0;
    destination.origin.y = 0;

#ifdef FB_HACK
    if (fb_image.bitmap != NULL && prev_image.bitmap == NULL) {
      prev_image = fb_image;
      fb_image.bitmap = NULL;
      fb_image.data = NULL;
    }
#endif  // FB_HACK
    
    if (wipe_direction) {
      // First, draw the previous face.
      if (wipe_x < SCREEN_WIDTH) {
        if (prev_image.bitmap != NULL) {
          graphics_context_set_compositing_mode(ctx, GCompOpAssign);
          graphics_draw_bitmap_in_rect(ctx, prev_image.bitmap, destination);
        } else {
          graphics_context_set_fill_color(ctx, GColorBlack);
          graphics_fill_rect(ctx, destination, 0, GCornerNone);
        }
      }
      
      if (wipe_x > 0) {
        // Then, draw the new face on top of it, reducing the size to wipe
        // from right to left.
        if (face_image.bitmap != NULL) {
          destination.size.w = wipe_x;
          graphics_draw_bitmap_in_rect(ctx, face_image.bitmap, destination);
        }
      }
    } else {
      // First, draw the new face.
      if (wipe_x < SCREEN_WIDTH) {
        if (face_image.bitmap != NULL) {
          graphics_context_set_compositing_mode(ctx, GCompOpAssign);
          graphics_draw_bitmap_in_rect(ctx, face_image.bitmap, destination);
        }
      }
      
      if (wipe_x > 0) {
        // Then, draw the previous face on top of it, reducing the size to wipe
        // from right to left.
        destination.size.w = wipe_x;
        if (prev_image.bitmap != NULL) {
          graphics_draw_bitmap_in_rect(ctx, prev_image.bitmap, destination);
        } else {
          graphics_context_set_fill_color(ctx, GColorBlack);
          graphics_fill_rect(ctx, destination, 0, GCornerNone);
        }
      }
    }

    if (sprite_mask.bitmap != NULL) {
      // Then, draw the sprite on top of the wipe line.
      destination.size.w = sprite_mask.bitmap->bounds.size.w;
      destination.size.h = sprite_mask.bitmap->bounds.size.h;
      destination.origin.y = (SCREEN_HEIGHT - destination.size.h) / 2;
      destination.origin.x = wipe_x - sprite_cx;
      graphics_context_set_compositing_mode(ctx, GCompOpClear);
      graphics_draw_bitmap_in_rect(ctx, sprite_mask.bitmap, destination);

      if (sprite.bitmap != NULL) {
        // Fixed sprite case.
        graphics_context_set_compositing_mode(ctx, GCompOpOr);
        graphics_draw_bitmap_in_rect(ctx, sprite.bitmap, destination);
      } else {
        // Tardis case.  Since it's animated, but we don't have enough
        // RAM to hold all the frames at once, we have to load one
        // frame at a time as we need it.  We don't use RLE encoding
        // on the Tardis frames in an attempt to cut down on needless
        // CPU work while playing this animation.
        int af = ti % NUM_TARDIS_FRAMES;
        if (anim_direction) {
          af = (NUM_TARDIS_FRAMES - 1) - af;
        }
        GBitmap *tardis = gbitmap_create_with_resource(tardis_frames[af].tardis);
        if (tardis != NULL) {
          if (tardis_frames[af].flip_x) {
            flip_bitmap_x(tardis);
          }
          
          graphics_context_set_compositing_mode(ctx, GCompOpOr);
          graphics_draw_bitmap_in_rect(ctx, tardis, destination);
          
          gbitmap_destroy(tardis);
        }
      }
      
      // Finally, re-draw the minutes background card on top of the sprite.
      destination.size.w = 50;
      destination.size.h = 31;
      destination.origin.x = SCREEN_WIDTH - destination.size.w;
      destination.origin.y = SCREEN_HEIGHT - destination.size.h;
      graphics_context_set_compositing_mode(ctx, GCompOpOr);
      graphics_draw_bitmap_in_rect(ctx, mins_background.bitmap, destination);
    }
  }
}
  
void minute_layer_update_callback(Layer *me, GContext* ctx) {
  GFont font;
  GRect box;
  static const int buffer_size = 128;
  char buffer[buffer_size];

  font = fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);

  box = layer_get_frame(me);
  box.origin.x = 0;
  box.origin.y = 0;

  graphics_context_set_text_color(ctx, GColorBlack);

  snprintf(buffer, buffer_size, " %02d", minute_value);
  graphics_draw_text(ctx, buffer, font, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft,
                     NULL);
}
  
void second_layer_update_callback(Layer *me, GContext* ctx) {
  if (!config.second_hand || !hide_colon) {
    GFont font;
    GRect box;
  
    font = fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK);
    
    box = layer_get_frame(me);
    box.origin.x = 0;
    box.origin.y = 0;
    
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, ":", font, box,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft,
                       NULL);
  }
}


// Update the watch as time passes.
void handle_tick(struct tm *tick_time, TimeUnits units_changed) {
  if (face_value == -1) {
    // We haven't loaded yet.
    return;
  }

  int face_new, minute_new, second_new;

  face_new = tick_time->tm_hour % 12;
  minute_new = tick_time->tm_min;
  second_new = tick_time->tm_sec;
  if (config.hurt && face_new == 8 && minute_new >= 30) {
    // Face 8.5 is John Hurt.
    face_new = 12;
  }
#ifdef FAST_TIME
  if (config.hurt) {
    face_new = ((tick_time->tm_min * 60 + tick_time->tm_sec) / 5) % 13;
  } else {
    face_new = ((tick_time->tm_min * 60 + tick_time->tm_sec) / 5) % 12;
  }
  minute_new = tick_time->tm_sec;
#endif

  if (minute_new != minute_value) {
    // Update the minute display.
    minute_value = minute_new;
    layer_mark_dirty(minute_layer);
  }

  if (second_new != second_value) {
    // Update the second display.
    second_value = second_new;
    hide_colon = false;
    if (config.second_hand) {
      // To blink the colon once per second, draw it now, then make it
      // go away after a half-second.
      layer_mark_dirty(second_layer);

      if (blink_timer != NULL) {
        app_timer_cancel(blink_timer);
	blink_timer = NULL;
      }
      blink_timer = app_timer_register(500, &handle_blink, 0);
    }
  }

  if (face_transition) {
    layer_mark_dirty(face_layer);
  } else if (face_new != face_value) {
    start_transition(face_new, false);
  }

  set_next_timer();
}

// Updates any runtime settings as needed when the config changes.
void apply_config() {
  app_log(APP_LOG_LEVEL_INFO, __FILE__, __LINE__, "apply_config, second_hand=%d", config.second_hand);
  tick_timer_service_unsubscribe();

#ifdef FAST_TIME
  tick_timer_service_subscribe(SECOND_UNIT, handle_tick);
#else
  if (config.second_hand) {
    tick_timer_service_subscribe(SECOND_UNIT, handle_tick);
  } else {
    tick_timer_service_subscribe(MINUTE_UNIT, handle_tick);
  }
#endif

  refresh_battery_gauge();
  refresh_bluetooth_indicator();
}

void handle_init() {
  load_config();

  app_message_register_inbox_received(receive_config_handler);
  app_message_open(64, 64);

  time_t now = time(NULL);
  struct tm *startup_time = localtime(&now);
  srand(now);

  face_transition = false;
  face_value = -1;
  last_buzz_hour = -1;
  minute_value = startup_time->tm_min;
  second_value = startup_time->tm_sec;
  hide_colon = false;
  
  window = window_create();
  // GColorClear doesn't seem to work: it is the same as GColorWhite in this context.
  window_set_background_color(window, GColorClear);
  struct Layer *root_layer = window_get_root_layer(window);
  layer_set_update_proc(root_layer, &root_layer_update_callback);

  // We'd like to pass false in an attempt to not use the window
  // animation, since we'll be animating the TARDIS transition
  // ourselves.  But this doesn't appear to work--it's always animated
  // anyway.  So whatever.
  window_stack_push(window, true);

  mins_background = rle_bwd_create(RESOURCE_ID_MINS_BACKGROUND);
  assert(mins_background.bitmap != NULL);

  face_layer = layer_create(layer_get_bounds(root_layer));
  layer_set_update_proc(face_layer, &face_layer_update_callback);
  layer_add_child(root_layer, face_layer);

  minute_layer = layer_create(GRect(95, 134, 62, 35));
  layer_set_update_proc(minute_layer, &minute_layer_update_callback);
  layer_add_child(root_layer, minute_layer);

  second_layer = layer_create(GRect(95, 134, 16, 35));
  layer_set_update_proc(second_layer, &second_layer_update_callback);
  layer_add_child(root_layer, second_layer);

  init_battery_gauge(root_layer, 125, 0, false, true);
  init_bluetooth_indicator(root_layer, 0, 0, false, true);

  start_transition(startup_time->tm_hour % 12, true);

  apply_config();
}

void handle_deinit() {
  tick_timer_service_unsubscribe();
  stop_transition();

  window_stack_pop_all(false);  // Not sure if this is needed?
  layer_destroy(minute_layer);
  layer_destroy(face_layer);
  window_destroy(window);

  bwd_destroy(&face_image);
  bwd_destroy(&mins_background);
}

int main(void) {
  handle_init();

  app_event_loop();
  handle_deinit();
}
