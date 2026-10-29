/*
 * tinydng_dng.c - DNG / EXIF tag semantics (metadata).
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"

#define TD_STRING_CAP (1u << 16)

static char *td_read_ascii(tinydng_context *ctx, const td_reader *r,
                           uint64_t data_off, uint64_t count,
                           tinydng_error *err) {
  char *str;
  const uint8_t *p;
  size_t n;
  if (count == 0u) {
    return NULL;
  }
  n = (count > TD_STRING_CAP) ? TD_STRING_CAP : (size_t)count;
  str = (char *)td_ctx_alloc(ctx, n + 1u, err);
  if (!str) {
    return NULL;
  }
  p = td_io_view(r->io, r->size, data_off, n, (uint8_t *)str, n);
  if (!p) {
    td_ctx_free(ctx, str);
    return NULL;
  }
  if ((const char *)p != str) {
    memcpy(str, p, n);
  }
  str[n] = '\0';
  /* Trim a single trailing NUL that TIFF ASCII counts include. */
  if (n > 0u && str[n - 1u] == '\0') {
    /* already terminated */
  }
  return str;
}

/* Saturating double->int32 (avoids UB when a tag carries an out-of-range
   value, e.g. a corrupt RATIONAL). */
static int32_t td_clamp_i32(double d) {
  if (!(d == d)) {
    return 0; /* NaN */
  }
  if (d >= 2147483647.0) {
    return 2147483647;
  }
  if (d <= -2147483648.0) {
    return (-2147483647 - 1);
  }
  return (int32_t)d;
}

static size_t td_read_reals(const td_reader *r, uint16_t type, uint64_t off,
                            uint64_t count, double *out, size_t max) {
  /* Largest caller passes max=16 doubles; 128 bytes of stack scratch covers
     any array this path reads when the io backend has no map view. */
  uint8_t sc[16u * 8u];
  size_t n = (count > (uint64_t)max) ? max : (size_t)count;
  size_t ts = td_tiff_type_size(type);
  size_t span;
  const uint8_t* base;
  if (ts == 0u || !td_safe_mul_size(n, ts, &span) || span > sizeof(sc)) {
    return 0;
  }
  base = td_bulk_span_view(r, off, span, sc, sizeof(sc));
  if (!base) {
    return 0;
  }
  return td_reals_from_buf(base, type, r->big_endian, n, out);
}

static size_t td_read_uints(const td_reader *r, uint16_t type, uint64_t off,
                            uint64_t count, uint64_t *out, size_t max) {
  uint8_t sc[16u * 8u];
  size_t n = (count > (uint64_t)max) ? max : (size_t)count;
  size_t ts = td_tiff_type_size(type);
  size_t span;
  const uint8_t* base;
  if (ts == 0u || !td_safe_mul_size(n, ts, &span) || span > sizeof(sc)) {
    return 0;
  }
  base = td_bulk_span_view(r, off, span, sc, sizeof(sc));
  if (!base) {
    return 0;
  }
  return td_uints_from_buf(base, type, r->big_endian, n, out);
}

/* ------------------------------------------------------------------ */
/* DNG OpcodeList GainMap (opcode id 9). Opcode data is always big-endian. */
/* ------------------------------------------------------------------ */

#define TD_OPCODE_WARP_RECTILINEAR 1u
#define TD_OPCODE_FIX_VIGNETTE_RADIAL 3u
#define TD_OPCODE_GAIN_MAP 9u
#define TD_MAX_OPCODES 64u
#define TD_MAX_GAINMAP_ITEMS (4u * 1024u * 1024u)
#define TD_MAX_OPCODE_BYTES (64u * 1024u * 1024u)

static int td_r_f64_be(const td_reader *br, uint64_t at, double *out) {
  uint64_t u;
  if (!td_r_u64(br, at, &u)) {
    return 0;
  }
  memcpy(out, &u, sizeof(*out));
  return 1;
}

static int td_append_gainmap(tinydng_context *ctx, tinydng_image_info *img,
                             const tinydng_gainmap *gm, tinydng_error *err) {
  size_t n = img->raw.gainmap_count;
  size_t bytes;
  tinydng_gainmap *na;
  if (!td_safe_mul_size(n + 1u, sizeof(tinydng_gainmap), &bytes)) {
    return 0;
  }
  na = (tinydng_gainmap *)td_ctx_realloc(
      ctx, img->raw.gainmaps, n * sizeof(tinydng_gainmap), bytes, err);
  if (!na) {
    return 0;
  }
  img->raw.gainmaps = na;
  na[n] = *gm;
  img->raw.gainmap_count = n + 1u;
  return 1;
}

static int td_append_opcode(tinydng_context *ctx, tinydng_image_info *img,
                            const tinydng_opcode *op, tinydng_error *err) {
  size_t n = img->raw.opcode_count;
  size_t bytes;
  tinydng_opcode *na;
  if (!td_safe_mul_size(n + 1u, sizeof(tinydng_opcode), &bytes)) {
    return 0;
  }
  na = (tinydng_opcode *)td_ctx_realloc(ctx, img->raw.opcodes,
                                        n * sizeof(tinydng_opcode), bytes, err);
  if (!na) {
    return 0;
  }
  img->raw.opcodes = na;
  na[n] = *op;
  img->raw.opcode_count = n + 1u;
  return 1;
}

static int td_append_warp(tinydng_context *ctx, tinydng_image_info *img,
                          const tinydng_warp_rectilinear *wpr,
                          tinydng_error *err) {
  size_t n = img->raw.warp_count;
  size_t bytes;
  tinydng_warp_rectilinear *na;
  if (!td_safe_mul_size(n + 1u, sizeof(*na), &bytes)) {
    return 0;
  }
  na = (tinydng_warp_rectilinear *)td_ctx_realloc(ctx, img->raw.warps,
                                                  n * sizeof(*na), bytes, err);
  if (!na) {
    return 0;
  }
  img->raw.warps = na;
  na[n] = *wpr;
  img->raw.warp_count = n + 1u;
  return 1;
}

static int td_append_vignette(tinydng_context *ctx, tinydng_image_info *img,
                              const tinydng_vignette_radial *v,
                              tinydng_error *err) {
  size_t n = img->raw.vignette_count;
  size_t bytes;
  tinydng_vignette_radial *na;
  if (!td_safe_mul_size(n + 1u, sizeof(*na), &bytes)) {
    return 0;
  }
  na = (tinydng_vignette_radial *)td_ctx_realloc(ctx, img->raw.vignettes,
                                                 n * sizeof(*na), bytes, err);
  if (!na) {
    return 0;
  }
  img->raw.vignettes = na;
  na[n] = *v;
  img->raw.vignette_count = n + 1u;
  return 1;
}

/* Copy an opcode's raw parameter bytes and record it generically. */
static int td_capture_opcode(tinydng_context *ctx, const td_reader *br,
                             tinydng_image_info *img, uint32_t list_index,
                             uint32_t id, uint32_t ver, uint32_t flags,
                             uint64_t saved, uint32_t nbytes,
                             tinydng_error *err) {
  tinydng_opcode op;
  uint8_t *pcopy = NULL;
  memset(&op, 0, sizeof(op));
  if (nbytes > 0u && nbytes <= TD_MAX_OPCODE_BYTES) {
    const uint8_t *v;
    pcopy = (uint8_t *)td_ctx_alloc(ctx, nbytes, err);
    if (!pcopy) {
      return 0;
    }
    v = td_io_view(br->io, br->size, saved, nbytes, pcopy, nbytes);
    if (!v) {
      td_ctx_free(ctx, pcopy);
      return 1; /* benign: leave params null */
    }
    if (v != pcopy) {
      memcpy(pcopy, v, nbytes);
    }
  }
  op.list = list_index;
  op.id = id;
  op.version = ver;
  op.flags = flags;
  op.params = pcopy;
  op.params_size = (pcopy != NULL) ? nbytes : 0u;
  if (!td_append_opcode(ctx, img, &op, err)) {
    td_ctx_free(ctx, pcopy);
    return 0;
  }
  return 1;
}

static int td_parse_opcode_list(tinydng_context *ctx, const td_reader *r,
                                uint32_t list_index, uint64_t data_off,
                                uint64_t count, tinydng_image_info *img,
                                tinydng_error *err) {
  td_reader br = *r;
  uint64_t pos = data_off;
  uint64_t end;
  uint32_t num_opcodes = 0;
  uint32_t i;
  br.big_endian = 1; /* opcode data is always big-endian */
  if (!td_safe_add_u64(data_off, count, &end)) {
    return 1; /* benign */
  }
  if (count < 4u || !td_r_u32(&br, pos, &num_opcodes)) {
    return 1;
  }
  if (!td_safe_add_u64(pos, 4u, &pos)) {
    return 1;
  }
  if (num_opcodes > TD_MAX_OPCODES) {
    return 1; /* suspicious: skip */
  }

  for (i = 0; i < num_opcodes; i++) {
    uint32_t id = 0, ver = 0, flags = 0, nbytes = 0;
    uint64_t saved;
    uint64_t p4, p8, p12;
    if (!td_safe_add_u64(pos, 4u, &p4) || !td_safe_add_u64(pos, 8u, &p8) ||
        !td_safe_add_u64(pos, 12u, &p12) ||
        !td_r_u32(&br, pos, &id) || !td_r_u32(&br, p4, &ver) ||
        !td_r_u32(&br, p8, &flags) || !td_r_u32(&br, p12, &nbytes)) {
      return 1;
    }
    (void)ver;
    (void)flags;
    if (!td_safe_add_u64(pos, 16u, &pos)) {
      return 1;
    }
    saved = pos;
    {
      uint64_t opcode_end;
      /* A zero/short payload is legal (an opcode without parameters); only
       * an extent running past the list aborts the remaining opcodes. */
      if (!td_safe_add_u64(saved, nbytes, &opcode_end) || opcode_end > end) {
        return 1;
      }
    }
    /* Capture every opcode generically (raw big-endian params). */
    if (!td_capture_opcode(ctx, &br, img, list_index, id, ver, flags, saved,
                           nbytes, err)) {
      return 0;
    }

    if (id == TD_OPCODE_WARP_RECTILINEAR) {
      tinydng_warp_rectilinear wpr;
      uint32_t n = 0;
      uint64_t p = saved;
      uint32_t pl, k;
      memset(&wpr, 0, sizeof(wpr));
      if (td_r_u32(&br, p, &n) && n >= 1u && n <= 4u &&
          (uint64_t)4u + (uint64_t)n * 48u + 16u <= nbytes) {
        p += 4u;
        for (pl = 0; pl < n; pl++) {
          for (k = 0; k < 6u; k++) {
            (void)td_r_f64_be(&br, p, &wpr.coeff[pl][k]);
            p += 8u;
          }
        }
        (void)td_r_f64_be(&br, p, &wpr.center[0]);
        (void)td_r_f64_be(&br, p + 8u, &wpr.center[1]);
        wpr.list = list_index;
        wpr.plane_count = n;
        if (!td_append_warp(ctx, img, &wpr, err)) {
          return 0;
        }
      }
    } else if (id == TD_OPCODE_FIX_VIGNETTE_RADIAL) {
      tinydng_vignette_radial vg;
      uint64_t p = saved;
      uint32_t j;
      memset(&vg, 0, sizeof(vg));
      if ((uint64_t)7u * 8u <= nbytes) {
        for (j = 0; j < 5u; j++) {
          (void)td_r_f64_be(&br, p, &vg.k[j]);
          p += 8u;
        }
        (void)td_r_f64_be(&br, p, &vg.center[0]);
        (void)td_r_f64_be(&br, p + 8u, &vg.center[1]);
        vg.list = list_index;
        if (!td_append_vignette(ctx, img, &vg, err)) {
          return 0;
        }
      }
    } else if (id == TD_OPCODE_GAIN_MAP) {
      tinydng_gainmap gm;
      uint32_t u[10];
      double d[4];
      uint32_t map_planes = 0;
      uint64_t num_items;
      uint64_t prod;
      uint64_t p = saved;
      uint32_t j;
      float *pixels;
      size_t pbytes;
      memset(&gm, 0, sizeof(gm));
      /* The 76-byte header (10 u32 + 4 f64 + 1 u32) must fit in this opcode. */
      if (nbytes < 76u) {
        (void)td_safe_add_u64(saved, nbytes, &pos);
        continue;
      }
      for (j = 0; j < 10u; j++) {
        if (!td_r_u32(&br, p, &u[j])) {
          return 1;
        }
        p += 4u;
      }
      for (j = 0; j < 4u; j++) {
        if (!td_r_f64_be(&br, p, &d[j])) {
          return 1;
        }
        p += 8u;
      }
      if (!td_r_u32(&br, p, &map_planes)) {
        return 1;
      }
      p += 4u;

      /* Overflow-safe map_points_v * map_points_h * map_planes. Reject if it
       * overflows, is empty, exceeds the cap, or the pixel payload (num_items
       * floats after the 76-byte header) would not fit inside this opcode.
       * Computing it raw could wrap and let the cap check pass while the
       * struct's dimension fields stay huge (inconsistent with pixel_count). */
      if (!td_safe_mul_u64((uint64_t)u[8], (uint64_t)u[9], &prod) ||
          !td_safe_mul_u64(prod, (uint64_t)map_planes, &num_items) ||
          num_items == 0u || num_items > TD_MAX_GAINMAP_ITEMS ||
          num_items > ((uint64_t)nbytes - 76u) / 4u) {
        (void)td_safe_add_u64(saved, nbytes, &pos);
        continue;
      }
      if (!td_safe_mul_size((size_t)num_items, sizeof(float), &pbytes)) {
        return 1;
      }
      pixels = (float *)td_ctx_alloc(ctx, pbytes, err);
      if (!pixels) {
        return 0;
      }
      {
        /* One bulk view for the whole pixel span (mmap: zero-copy). */
        const uint8_t* pv = td_bulk_span_view(&br, p, pbytes, NULL, 0);
        uint8_t* scr = NULL;
        if (!pv) {
          scr = (uint8_t*)td_ctx_alloc(ctx, pbytes, err);
          if (!scr) {
            td_ctx_free(ctx, pixels);
            return 0;
          }
          pv = td_bulk_span_view(&br, p, pbytes, scr, pbytes);
        }
        if (!pv) {
          td_ctx_free(ctx, scr);
          td_ctx_free(ctx, pixels);
          return 1;
        }
        for (j = 0; j < (uint32_t)num_items; j++) {
          pixels[j] = td_f32_from_buf(pv + (size_t)j * 4u, br.big_endian);
        }
        td_ctx_free(ctx, scr);
      }
      gm.opcode_list = list_index;
      gm.top = u[0];
      gm.left = u[1];
      gm.bottom = u[2];
      gm.right = u[3];
      gm.plane = u[4];
      gm.planes = u[5];
      gm.row_pitch = u[6];
      gm.col_pitch = u[7];
      gm.map_points_v = u[8];
      gm.map_points_h = u[9];
      gm.map_spacing_v = d[0];
      gm.map_spacing_h = d[1];
      gm.map_origin_v = d[2];
      gm.map_origin_h = d[3];
      gm.map_planes = map_planes;
      gm.pixels = pixels;
      gm.pixel_count = (size_t)num_items;
      if (!td_append_gainmap(ctx, img, &gm, err)) {
        td_ctx_free(ctx, pixels);
        return 0;
      }
    }
    if (!td_safe_add_u64(saved, nbytes, &pos)) {
      return 1;
    }
  }
  return 1;
}

/* Assign an ASCII tag, freeing any value already parsed for the same tag so a
 * duplicate occurrence cannot orphan the earlier allocation. A read that yields
 * nothing leaves the prior value intact. */
static void td_set_ascii(tinydng_context *ctx, char **dst, const td_reader *r,
                         uint64_t data_off, uint64_t count, tinydng_error *err) {
  char *s = td_read_ascii(ctx, r, data_off, count, err);
  if (s) {
    td_ctx_free(ctx, *dst);
    *dst = s;
  }
}

int td_dng_handle_tag(tinydng_context *ctx, const td_reader *r,
                      tinydng_image_info *img, uint32_t ifd_index, uint16_t tag,
                      uint16_t type, uint64_t count, uint64_t data_off,
                      tinydng_error *err) {
  (void)ifd_index;

  switch (tag) {
    case TD_TAG_MAKE:
      td_set_ascii(ctx, &img->exif.make, r, data_off, count, err);
      break;
    case TD_TAG_MODEL:
      td_set_ascii(ctx, &img->exif.model, r, data_off, count, err);
      break;
    case TD_TAG_SOFTWARE:
      td_set_ascii(ctx, &img->exif.software, r, data_off, count, err);
      break;
    case TD_TAG_DATETIME:
      td_set_ascii(ctx, &img->exif.datetime, r, data_off, count, err);
      break;
    case TD_TAG_IMAGEDESCRIPTION:
      td_set_ascii(ctx, &img->exif.image_description, r, data_off, count, err);
      break;
    case TD_TAG_ORIENTATION: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->exif.orientation = (uint16_t)v;
      }
      break;
    }
    case TD_TAG_SHUTTER_SPEED_VALUE: {
      int32_t num, den;
      if ((type == TD_TYPE_RATIONAL || type == TD_TYPE_SRATIONAL) &&
          td_r_i32(r, data_off, &num) &&
          td_r_i32(r, data_off + 4u, &den)) {
        img->exif.shutter_speed[0] = num;
        img->exif.shutter_speed[1] = den;
        img->exif.has_shutter_speed = 1;
      }
      break;
    }
    case TD_TAG_APERTURE_VALUE: {
      int32_t num, den;
      if ((type == TD_TYPE_RATIONAL || type == TD_TYPE_SRATIONAL) &&
          td_r_i32(r, data_off, &num) &&
          td_r_i32(r, data_off + 4u, &den)) {
        img->exif.aperture_value[0] = num;
        img->exif.aperture_value[1] = den;
        img->exif.has_aperture_value = 1;
      }
      break;
    }
    case TD_TAG_EXPOSURE_TIME: {
      int32_t num, den;
      if ((type == TD_TYPE_RATIONAL || type == TD_TYPE_SRATIONAL) &&
          td_r_i32(r, data_off, &num) &&
          td_r_i32(r, data_off + 4u, &den)) {
        img->exif.exposure_time[0] = num;
        img->exif.exposure_time[1] = den;
        img->exif.has_exposure_time = 1;
      }
      break;
    }
    case TD_TAG_ISO_SPEED_RATINGS: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->exif.iso = (uint32_t)v;
        img->exif.has_iso = 1;
      }
      break;
    }
    /* Converter carry: EXIF identity/exposure tags for DNG-to-DNG rewrites.
       Rationals fill both the signed pair and the unsigned copy the writer
       emits. */
#define TD_CARRY_RATIONAL(signed_pair, unsigned_pair, flag)                    \
  do {                                                                         \
    int32_t num_, den_;                                                        \
    if ((type == TD_TYPE_RATIONAL || type == TD_TYPE_SRATIONAL) &&            \
        td_r_i32(r, data_off, &num_) && td_r_i32(r, data_off + 4u, &den_)) { \
      if (signed_pair) {                                                       \
        ((int32_t *)(signed_pair))[0] = num_;                                  \
        ((int32_t *)(signed_pair))[1] = den_;                                  \
      }                                                                        \
      if (unsigned_pair) {                                                     \
        ((uint32_t *)(unsigned_pair))[0] = (uint32_t)num_;                     \
        ((uint32_t *)(unsigned_pair))[1] = (uint32_t)den_;                     \
      }                                                                        \
      (flag) = 1;                                                              \
    }                                                                          \
  } while (0)
#define TD_CARRY_SHORT(field, flag)                                            \
  do {                                                                         \
    uint64_t v_;                                                               \
    if (td_read_uints(r, type, data_off, count, &v_, 1)) {                     \
      (field) = (uint16_t)v_;                                                  \
      (flag) = 1;                                                              \
    }                                                                          \
  } while (0)
#define TD_CARRY_BYTES4(field, flag)                                           \
  do {                                                                         \
    uint64_t v_[4];                                                            \
    size_t i_, n_ = td_read_uints(r, type, data_off, count, v_, 4);            \
    for (i_ = 0; i_ < n_; i_++) (field)[i_] = (uint8_t)v_[i_];                 \
    if (n_ >= 4) (flag) = 1;                                                   \
  } while (0)
    case TD_TAG_FNUMBER:
      TD_CARRY_RATIONAL(img->exif.fnumber, img->exif.fnumber_rational, img->exif.has_fnumber);
      break;
    case TD_TAG_FOCAL_LENGTH:
      TD_CARRY_RATIONAL(img->exif.focal_length, img->exif.focal_length_rational,
                        img->exif.has_focal_length);
      break;
    case TD_TAG_SUBJECT_DISTANCE:
      TD_CARRY_RATIONAL(img->exif.subject_distance, img->exif.subject_distance_rational,
                        img->exif.has_subject_distance);
      break;
    case TD_TAG_MAX_APERTURE_VALUE:
      TD_CARRY_RATIONAL(img->exif.max_aperture_value, img->exif.max_aperture_rational,
                        img->exif.has_max_aperture_value);
      break;
    case TD_TAG_DIGITAL_ZOOM_RATIO:
      TD_CARRY_RATIONAL((int32_t *)NULL, img->exif.digital_zoom_rational,
                        img->exif.has_digital_zoom);
      break;
    case TD_TAG_EXPOSURE_COMPENSATION:
      TD_CARRY_RATIONAL(img->exif.exposure_compensation, (uint32_t *)NULL,
                        img->exif.has_exposure_compensation);
      break;
    case TD_TAG_FLASH:
      TD_CARRY_SHORT(img->exif.flash, img->exif.has_flash);
      break;
    case TD_TAG_SENSING_METHOD:
      TD_CARRY_SHORT(img->exif.sensing_method, img->exif.has_sensing_method);
      break;
    case TD_TAG_EXPOSURE_MODE:
      TD_CARRY_SHORT(img->exif.exposure_mode, img->exif.has_exposure_mode);
      break;
    case TD_TAG_WHITE_BALANCE:
      TD_CARRY_SHORT(img->exif.white_balance, img->exif.has_white_balance);
      break;
    case TD_TAG_LIGHT_SOURCE:
      TD_CARRY_SHORT(img->exif.light_source, img->exif.has_light_source);
      break;
    case TD_TAG_FOCAL_LENGTH_35MM:
      TD_CARRY_SHORT(img->exif.focal_length_35mm, img->exif.has_focal_length_35mm);
      break;
    case TD_TAG_SHARPNESS:
      TD_CARRY_SHORT(img->exif.sharpness, img->exif.has_sharpness);
      break;
    case TD_TAG_EXIF_VERSION:
      if (type == TD_TYPE_UNDEFINED && count == 4)
        TD_CARRY_BYTES4(img->exif.exif_version, img->exif.has_exif_version);
      break;
    case TD_TAG_DATETIME_ORIGINAL:
      td_set_ascii(ctx, &img->exif.datetime_original, r, data_off, count, err);
      break;
    case TD_TAG_DATETIME_DIGITIZED:
      td_set_ascii(ctx, &img->exif.datetime_digitized, r, data_off, count, err);
      break;
    case TD_TAG_OFFSET_TIME:
      td_set_ascii(ctx, &img->exif.offset_time, r, data_off, count, err);
      break;
    case TD_TAG_OFFSET_TIME_ORIGINAL:
      td_set_ascii(ctx, &img->exif.offset_time_original, r, data_off, count, err);
      break;
    case TD_TAG_OFFSET_TIME_DIGITIZED:
      td_set_ascii(ctx, &img->exif.offset_time_digitized, r, data_off, count, err);
      break;
    case TD_TAG_SUBSEC_TIME:
      td_set_ascii(ctx, &img->exif.subsec_time, r, data_off, count, err);
      break;
    case TD_TAG_SUBSEC_TIME_ORIGINAL:
      td_set_ascii(ctx, &img->exif.subsec_time_original, r, data_off, count, err);
      break;
    case TD_TAG_SUBSEC_TIME_DIGITIZED:
      td_set_ascii(ctx, &img->exif.subsec_time_digitized, r, data_off, count, err);
      break;
    case TD_TAG_BODY_SERIAL:
      td_set_ascii(ctx, &img->exif.body_serial, r, data_off, count, err);
      break;
    case TD_TAG_LENS_MAKE:
      td_set_ascii(ctx, &img->exif.lens_make, r, data_off, count, err);
      break;
    case TD_TAG_LENS_MODEL:
      td_set_ascii(ctx, &img->exif.lens_model, r, data_off, count, err);
      break;
    case TD_TAG_LENS_SERIAL:
      td_set_ascii(ctx, &img->exif.lens_serial, r, data_off, count, err);
      break;
    case TD_TAG_LENS_SPEC: {
      size_t n = td_read_reals(r, type, data_off, count, img->exif.lens_spec, 4);
      if (n == 4) img->exif.has_lens_spec = 1;
      break;
    }
    case TD_TAG_UNIQUE_CAMERA_MODEL:
      td_set_ascii(ctx, &img->raw.unique_camera_model, r, data_off, count, err);
      break;
    case TD_TAG_DNG_BACKWARD_VERSION:
      TD_CARRY_BYTES4(img->raw.dng_backward_version, img->raw.has_dng_backward_version);
      break;
    case TD_TAG_TIFF_EP_STANDARD_ID:
      TD_CARRY_BYTES4(img->raw.tiff_ep_standard_id, img->raw.has_tiff_ep_standard_id);
      break;
    case TD_TAG_BASELINE_EXPOSURE: {
      int32_t num, den;
      if ((type == TD_TYPE_RATIONAL || type == TD_TYPE_SRATIONAL) &&
          td_r_i32(r, data_off, &num) && td_r_i32(r, data_off + 4u, &den) && den != 0) {
        img->raw.baseline_exposure = (double)num / (double)den;
        img->raw.has_baseline_exposure = 1;
      }
      break;
    }
    case TD_TAG_BLACK_LEVEL_REPEAT_DIM: {
      uint64_t v[2];
      if (td_read_uints(r, type, data_off, count, v, 2) == 2) {
        img->raw.black_level_repeat[0] = (uint16_t)v[0];
        img->raw.black_level_repeat[1] = (uint16_t)v[1];
        img->raw.has_black_level_repeat = 1;
      }
      break;
    }
    case TD_TAG_MAKER_NOTE:
    case TD_TAG_XMP: {
      /* Opaque bounded copy; MakerNote belongs to ExifIFD and XMP to IFD0,
         and the writer puts them back there. */
      size_t ts = td_tiff_type_size(type);
      size_t span;
      const uint8_t *base;
      uint8_t *copy;
      tinydng_blob *dst =
          (tag == TD_TAG_MAKER_NOTE) ? &img->exif.maker_note : &img->raw.xmp_packet;
      if (ts == 0u || count == 0u || count > (uint64_t)(TD_BLOB_MAX_BYTES / ts)) {
        break;
      }
      span = (size_t)count * ts;
      copy = (uint8_t *)td_ctx_alloc(ctx, span, err);
      if (!copy) {
        return 0;
      }
      base = td_bulk_span_view(r, data_off, span, copy, span);
      if (!base) {
        td_ctx_free(ctx, copy);
        break;
      }
      if (base != copy) memcpy(copy, base, span);
      td_ctx_free(ctx, dst->data);
      dst->data = copy;
      dst->size = span;
      dst->type = type;
      dst->count = count;
      dst->present = 1;
      break;
    }
#undef TD_CARRY_RATIONAL
#undef TD_CARRY_SHORT
#undef TD_CARRY_BYTES4

    case TD_TAG_CFA_REPEAT_PATTERN_DIM: {
      uint64_t v[2];
      size_t n = td_read_uints(r, type, data_off, count, v, 2);
      if (n >= 2) {
        img->cfa.pattern_dim[0] = (uint16_t)v[0];
        img->cfa.pattern_dim[1] = (uint16_t)v[1];
        img->cfa.present = 1;
      }
      break;
    }
    case TD_TAG_CFA_PATTERN: {
      uint64_t v[16];
      size_t i, n = td_read_uints(r, type, data_off, count, v, 16);
      for (i = 0; i < n; i++) {
        img->cfa.pattern[i] = (uint8_t)v[i];
      }
      img->cfa.pattern_size = (uint8_t)n;
      if (n > 0) {
        img->cfa.present = 1;
      }
      break;
    }
    case TD_TAG_CFA_PLANE_COLOR: {
      uint64_t v[4];
      size_t i, n = td_read_uints(r, type, data_off, count, v, 4);
      for (i = 0; i < n; i++) {
        img->cfa.plane_color[i] = (uint8_t)v[i];
      }
      img->cfa.plane_color_count = (uint8_t)n;
      break;
    }
    case TD_TAG_CFA_LAYOUT: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->cfa.layout = (uint16_t)v;
      }
      break;
    }

    case 50719:
      (void)td_read_reals(r, type, data_off, count, img->raw.crop_origin, 2);
      break;
    case 50720:
      img->raw.has_crop = td_read_reals(r, type, data_off, count, img->raw.crop_size, 2) == 2;
      break;
    case 50740: {
      /* Retain bounded provenance including the manufacturer's NUL separator. */
      if (count > 0 && count <= 1048576) {
        char *v = (char *)td_ctx_alloc(ctx, (size_t)count + 1, err);
        if (!v) return 0;
        const uint8_t *bytes = td_io_view(r->io, r->size, data_off, (size_t)count, (uint8_t *)v, (size_t)count);
        if (!bytes) { td_ctx_free(ctx, v); break; }
        if (bytes != (uint8_t *)v) memcpy(v, bytes, (size_t)count);
        v[count] = 0;
        td_ctx_free(ctx, img->raw.rawr_private_data);
        img->raw.rawr_private_data = v;
        img->raw.rawr_private_size = (size_t)count;
      }
      break;
    }
    case TD_TAG_BLACK_LEVEL: {
      double v[4];
      size_t i, n = td_read_reals(r, type, data_off, count, v, 4);
      for (i = 0; i < n; i++) {
        img->raw.black_level[i] = td_clamp_i32(v[i]);
        img->raw.black_level_exact[i] = v[i];
      }
      if (n == 1) { for (i = 1; i < 4; i++) { img->raw.black_level[i] = img->raw.black_level[0]; img->raw.black_level_exact[i] = img->raw.black_level_exact[0]; } }
      if (n > 0) {
        img->raw.black_level_present = 1;
      }
      break;
    }
    case TD_TAG_WHITE_LEVEL: {
      double v[4];
      size_t i, n = td_read_reals(r, type, data_off, count, v, 4);
      for (i = 0; i < n; i++) {
        img->raw.white_level[i] = td_clamp_i32(v[i]);
      }
      if (n > 0) {
        img->raw.white_level_present = 1;
      }
      break;
    }
    case TD_TAG_COLOR_MATRIX1:
      if (td_read_reals(r, type, data_off, count, img->raw.color_matrix1, 9)) {
        img->raw.color_matrix_present = 1;
      }
      break;
    case TD_TAG_COLOR_MATRIX2:
      if (td_read_reals(r, type, data_off, count, img->raw.color_matrix2, 9) == 9) {
        img->raw.has_color_matrix2 = 1;
      }
      break;
    case TD_TAG_FORWARD_MATRIX1:
      if (td_read_reals(r, type, data_off, count, img->raw.forward_matrix1, 9) == 9) {
        img->raw.has_forward_matrix1 = 1;
      }
      break;
    case TD_TAG_FORWARD_MATRIX2:
      if (td_read_reals(r, type, data_off, count, img->raw.forward_matrix2, 9) == 9) {
        img->raw.has_forward_matrix2 = 1;
      }
      break;
    case TD_TAG_CAMERA_CALIBRATION1:
      if (td_read_reals(r, type, data_off, count, img->raw.camera_calibration1,
                        9)) {
        img->raw.camera_calibration_present = 1;
      }
      break;
    case TD_TAG_CAMERA_CALIBRATION2:
      if (td_read_reals(r, type, data_off, count, img->raw.camera_calibration2, 9) == 9) {
        img->raw.has_camera_calibration2 = 1;
      }
      break;
    case TD_TAG_ANALOG_BALANCE:
      if (td_read_reals(r, type, data_off, count, img->raw.analog_balance, 3)) {
        img->raw.has_analog_balance = 1;
      }
      break;
    case TD_TAG_AS_SHOT_NEUTRAL:
      if (td_read_reals(r, type, data_off, count, img->raw.as_shot_neutral, 3)) {
        img->raw.has_as_shot_neutral = 1;
      }
      break;
    case TD_TAG_CALIBRATION_ILLUMINANT1: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->raw.calibration_illuminant1 = (uint16_t)v;
      }
      break;
    }
    case TD_TAG_CALIBRATION_ILLUMINANT2: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->raw.calibration_illuminant2 = (uint16_t)v;
      }
      break;
    }
    case TD_TAG_DNG_VERSION: {
      uint64_t v[4];
      size_t i, n = td_read_uints(r, type, data_off, count, v, 4);
      for (i = 0; i < n; i++) {
        img->raw.dng_version[i] = (uint8_t)v[i];
      }
      if (n >= 4) {
        img->raw.has_dng_version = 1;
      }
      break;
    }
    case TD_TAG_ACTIVE_AREA: {
      uint64_t v[4];
      size_t i, n = td_read_uints(r, type, data_off, count, v, 4);
      for (i = 0; i < n; i++) {
        img->raw.active_area[i] = (uint32_t)v[i];
      }
      if (n >= 4) {
        img->raw.has_active_area = 1;
      }
      break;
    }
    case TD_TAG_DEFAULT_BLACK_RENDER: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        img->raw.default_black_render = (uint16_t)v;
        img->raw.has_default_black_render = 1;
      }
      break;
    }
    case TD_TAG_PROFILE_NAME:
      td_set_ascii(ctx, &img->raw.profile_name, r, data_off, count, err);
      break;
    case TD_TAG_SEMANTIC_NAME:
      td_set_ascii(ctx, &img->raw.semantic_name, r, data_off, count, err);
      break;
    case TD_TAG_PROFILE_TONE_CURVE: {
      size_t n =
          td_read_reals(r, type, data_off, count, img->raw.profile_tone_curve,
                        64);
      img->raw.profile_tone_curve_count = (uint16_t)n;
      break;
    }
    case TD_TAG_NOISE_PROFILE: {
      size_t n =
          td_read_reals(r, type, data_off, count, img->raw.noise_profile, 8);
      img->raw.noise_profile_count = (uint16_t)n;
      break;
    }
    case TD_TAG_PROFILE_EMBED_POLICY: {
      uint64_t v;
      if (td_read_uints(r, type, data_off, count, &v, 1)) {
        if (v <= 2) {
          img->raw.profile_embed_policy = (int8_t)v;
          img->raw.has_profile_embed_policy = 1;
        }
      }
      break;
    }
    case TD_TAG_CR2_SLICES: {
      uint64_t v[3];
      size_t i, n = td_read_uints(r, type, data_off, count, v, 3);
      for (i = 0; i < n; i++) {
        img->raw.cr2_slices[i] = (uint16_t)v[i];
      }
      if (n >= 3) {
        img->raw.has_cr2_slices = 1;
      }
      break;
    }
    case TD_TAG_LINEARIZATION_TABLE: {
      size_t i, n, ts;
      uint16_t *tbl;
      size_t bytes, span;
      const uint8_t* base = NULL;
      uint8_t* scr = NULL;
      ts = td_tiff_type_size(type);  /* element stride must match the type */
      if (count == 0u || count > (1u << 20) || ts == 0u) {
        break; /* benign skip */
      }
      n = (size_t)count;
      if (!td_safe_mul_size(n, sizeof(uint16_t), &bytes) ||
          !td_safe_mul_size(n, ts, &span)) {
        break;
      }
      /* One bulk view instead of a per-element read (mmap: zero-copy). */
      base = td_bulk_span_view(r, data_off, span, NULL, 0);
      if (!base) {
        scr = (uint8_t*)td_ctx_alloc(ctx, span, err);
        if (!scr) {
          return 0;
        }
        base = td_bulk_span_view(r, data_off, span, scr, span);
        if (!base) {
          td_ctx_free(ctx, scr);
          break;
        }
      }
      tbl = (uint16_t *)td_ctx_alloc(ctx, bytes, err);
      if (!tbl) {
        td_ctx_free(ctx, scr);
        return 0;
      }
      for (i = 0; i < n; i++) {
        uint64_t v;
        if (!td_val_uint_buf(base + i * ts, type, r->big_endian, &v)) {
          break;
        }
        tbl[i] = (uint16_t)v;
      }
      td_ctx_free(ctx, scr);
      img->raw.linearization_table = tbl;
      img->raw.linearization_table_count = i;
      break;
    }

    case TD_TAG_OPCODE_LIST1:
      if (!td_parse_opcode_list(ctx, r, 1u, data_off, count, img, err)) {
        return 0;
      }
      break;
    case TD_TAG_OPCODE_LIST2:
      if (!td_parse_opcode_list(ctx, r, 2u, data_off, count, img, err)) {
        return 0;
      }
      break;
    case TD_TAG_OPCODE_LIST3:
      if (!td_parse_opcode_list(ctx, r, 3u, data_off, count, img, err)) {
        return 0;
      }
      break;
    /* EXIF sub-IFD descent is handled in the container parser (tinydng_tiff.c)
       because it needs IFD-walking machinery. */
    case TD_TAG_EXIF_IFD:
    default:
      break;
  }

  /* If a string/array read failed via OOM, surface it; other read failures
     are treated as benign (missing/garbled metadata must not abort parsing). */
  if (err && err->status == TINYDNG_E_OOM) {
    return 0;
  }
  return 1;
}
