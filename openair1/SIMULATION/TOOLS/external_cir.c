/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * External CIR channel model for the OAI RFsimulator.
 * See external_cir.h and docs/OAI_SIONNA_CHANNEL_INTEGRATION.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "common/platform_types.h"
#include "external_cir.h"
#include "sim.h"
#include "common/utils/LOG/log.h"

/* OAICIRv1 on-disk layout (little-endian), produced by
 * sionna_rt_gui/oai_cir_export.py:
 *   header:  char[8] magic "OAICIRv1"; double fs; int32 nb_tx; int32 nb_rx;
 *            int32 channel_length; int32 n_snapshots; double snapshot_period;
 *            double carrier_hz;
 *   then n_snapshots records:
 *       int32 snapshot_index; int32 channel_offset; double path_loss_db;
 *       complex64 taps[nb_rx*nb_tx*channel_length]   (C-order [rx][tx][l])
 * complex64 = two little-endian float32 (real, imag).
 */

#define EXTERNAL_CIR_MAGIC "OAICIRv1"

static int read_exact(FILE *f, void *buf, size_t n)
{
  return fread(buf, 1, n, f) == n;
}

external_cir_t *external_cir_load(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) {
    LOG_E(OCM, "[EXTERNAL_CIR] cannot open '%s'\n", path);
    return NULL;
  }

  char magic[8];
  double fs = 0.0, period = 0.0, carrier = 0.0;
  int32_t nb_tx = 0, nb_rx = 0, channel_length = 0, n_snapshots = 0;
  if (!read_exact(f, magic, 8) || memcmp(magic, EXTERNAL_CIR_MAGIC, 8) != 0) {
    LOG_E(OCM, "[EXTERNAL_CIR] bad magic in '%s' (not an OAICIRv1 file)\n", path);
    fclose(f);
    return NULL;
  }
  if (!read_exact(f, &fs, sizeof(double)) || !read_exact(f, &nb_tx, 4)
      || !read_exact(f, &nb_rx, 4) || !read_exact(f, &channel_length, 4)
      || !read_exact(f, &n_snapshots, 4) || !read_exact(f, &period, sizeof(double))
      || !read_exact(f, &carrier, sizeof(double))) {
    LOG_E(OCM, "[EXTERNAL_CIR] truncated header in '%s'\n", path);
    fclose(f);
    return NULL;
  }
  if (channel_length <= 0 || n_snapshots <= 0 || nb_tx <= 0 || nb_rx <= 0) {
    LOG_E(OCM, "[EXTERNAL_CIR] invalid dimensions in '%s'\n", path);
    fclose(f);
    return NULL;
  }

  external_cir_t *ec = calloc(1, sizeof(external_cir_t));
  ec->sampling_rate_hz = fs;
  ec->snapshot_period_s = period;
  ec->carrier_hz = carrier;
  ec->nb_tx = nb_tx;
  ec->nb_rx = nb_rx;
  ec->channel_length = channel_length;
  ec->n_snapshots = n_snapshots;
  ec->current_index = -1;
  ec->snapshots = calloc(n_snapshots, sizeof(external_cir_snapshot_t));

  const int tap_count = nb_rx * nb_tx * channel_length;
  float *cbuf = malloc(sizeof(float) * 2 * tap_count);
  for (int s = 0; s < n_snapshots; s++) {
    int32_t idx = 0, channel_offset = 0;
    double path_loss_db = 0.0;
    if (!read_exact(f, &idx, 4) || !read_exact(f, &channel_offset, 4)
        || !read_exact(f, &path_loss_db, sizeof(double))
        || !read_exact(f, cbuf, sizeof(float) * 2 * tap_count)) {
      LOG_E(OCM, "[EXTERNAL_CIR] truncated snapshot %d in '%s'\n", s, path);
      free(cbuf);
      fclose(f);
      free_external_cir_state(ec);
      return NULL;
    }
    ec->snapshots[s].channel_offset = channel_offset;
    ec->snapshots[s].path_loss_dB = path_loss_db;
    ec->snapshots[s].taps = calloc(tap_count, sizeof(struct complexd));
    for (int t = 0; t < tap_count; t++) {
      ec->snapshots[s].taps[t].r = (double)cbuf[2 * t];
      ec->snapshots[s].taps[t].i = (double)cbuf[2 * t + 1];
    }
  }
  free(cbuf);
  fclose(f);

  LOG_I(OCM,
        "[EXTERNAL_CIR] loaded '%s': %d snapshots, %d taps, fs %.3f Msps, "
        "period %.1f ms, %dx%d\n",
        path, n_snapshots, channel_length, fs / 1e6, period * 1e3, nb_tx, nb_rx);
  return ec;
}

/* Copy snapshot s into desc->ch[], mapping file order [rx][tx][l] to OAI's
 * ch[rx + tx*nb_rx][l], and update the bulk delay and gain. */
void external_cir_apply_snapshot(channel_desc_t *desc, int s)
{
  external_cir_t *ec = (external_cir_t *)desc->external_cir;
  if (!ec || s < 0 || s >= ec->n_snapshots)
    return;
  const int L = ec->channel_length;
  const int nb_rx = ec->nb_rx;
  const int nb_tx = ec->nb_tx;
  const external_cir_snapshot_t *snap = &ec->snapshots[s];
  for (int tx = 0; tx < nb_tx; tx++) {
    for (int rx = 0; rx < nb_rx; rx++) {
      struct complexd *ch = desc->ch[rx + tx * nb_rx];
      for (int l = 0; l < L; l++) {
        ch[l] = snap->taps[(rx * nb_tx + tx) * L + l];
      }
    }
  }
  desc->path_loss_dB = snap->path_loss_dB;
  desc->channel_offset = (uint64_t)(snap->channel_offset > 0 ? snap->channel_offset : 0);
  ec->current_index = s;
}

void free_external_cir_state(external_cir_t *ec)
{
  if (!ec)
    return;
  if (ec->snapshots) {
    for (int s = 0; s < ec->n_snapshots; s++)
      free(ec->snapshots[s].taps);
    free(ec->snapshots);
  }
  free(ec);
}

void free_external_cir(channel_desc_t *desc)
{
  if (desc && desc->external_cir) {
    free_external_cir_state((external_cir_t *)desc->external_cir);
    desc->external_cir = NULL;
  }
}

void update_external_cir_snapshot(channel_desc_t *desc, uint64_t TS)
{
  if (!desc || desc->modelid != EXTERNAL_CIR || !desc->external_cir)
    return;
  external_cir_t *ec = (external_cir_t *)desc->external_cir;
  if (ec->snapshot_period_s <= 0.0 || ec->n_snapshots <= 1)
    return;

  const double t_sec = (double)TS / ec->sampling_rate_hz;
  int s = (int)(t_sec / ec->snapshot_period_s);
  if (s < 0)
    s = 0;
  if (s >= ec->n_snapshots)
    s = ec->n_snapshots - 1; /* hold the last snapshot at end of run */

  if (s != ec->current_index) {
    external_cir_apply_snapshot(desc, s);
    LOG_I(OCM,
          "[EXTERNAL_CIR] t=%.2fs -> snapshot %d/%d: path_loss %.1f dB, "
          "channel_offset %d samples\n",
          t_sec, s, ec->n_snapshots, desc->path_loss_dB, (int)desc->channel_offset);
  }
}
