/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * External CIR channel model for the OAI RFsimulator.
 *
 * Reads a time series of site-specific channel taps produced by the
 * Sionna RT digital twin (OAICIRv1 binary format, written by
 * sionna_rt_gui/oai_cir_export.py) and drives the RFsimulator channel
 * (channel_desc_t) with it instead of a statistical model. Snapshots are
 * stepped over the simulator's virtual time.
 *
 * See docs/OAI_SIONNA_CHANNEL_INTEGRATION.md in the LIT_fac_ray_tracing repo.
 */

#ifndef __EXTERNAL_CIR_H__
#define __EXTERNAL_CIR_H__

#include <stdint.h>
#include "sim.h"

/* One time snapshot of one SISO link: dense, sample-spaced taps plus the
 * bulk delay and amplitude gain (mirrors OAI's channel_offset / path_loss_dB).
 */
typedef struct {
  int channel_offset;   /* bulk propagation delay [whole samples] */
  double path_loss_dB;  /* amplitude gain; pathLossLinear = 10^(dB/20) */
  struct complexd *taps; /* [channel_length], unit-energy */
} external_cir_snapshot_t;

/* Parsed OAICIRv1 file: a time series of snapshots for one link. */
typedef struct {
  double sampling_rate_hz;   /* must match the channel's sampling rate */
  double snapshot_period_s;  /* time between snapshots */
  double carrier_hz;
  int nb_tx;
  int nb_rx;
  int channel_length;        /* dense taps per snapshot */
  int n_snapshots;
  external_cir_snapshot_t *snapshots; /* [n_snapshots] */
  int current_index;         /* last applied snapshot */
} external_cir_t;

/* Parse an OAICIRv1 file. Returns a heap external_cir_t (NULL on failure).
 * The descriptor allocation/registration lives in random_channel.c
 * (new_channel_desc_external_cir), which calls this. */
external_cir_t *external_cir_load(const char *path);

/* Copy snapshot s into desc->ch[] (mapping file [rx][tx][l] to OAI
 * ch[rx + tx*nb_rx][l]) and update desc->path_loss_dB / channel_offset.
 * Reads the external_cir_t from desc->external_cir. */
void external_cir_apply_snapshot(channel_desc_t *desc, int s);

/* Step to the snapshot selected by the virtual timestamp TS (in samples) and,
 * if it changed, re-apply it. Safe to call every received block. */
void update_external_cir_snapshot(channel_desc_t *desc, uint64_t TS);

/* Free the external CIR state attached to a descriptor. */
void free_external_cir(channel_desc_t *desc);

/* Free a parsed external_cir_t directly. */
void free_external_cir_state(external_cir_t *ec);

#endif /* __EXTERNAL_CIR_H__ */
