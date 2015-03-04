/*
 *  Tvheadend - SAT-IP server - RTP part
 *
 *  Copyright (C) 2015 Jaroslav Kysela
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include "tvheadend.h"
#include "input.h"
#include "streaming.h"
#include "satip/server.h"

#define RTP_PACKETS 128
#define RTP_PAYLOAD (7*188+12)
#define RTCP_PAYLOAD (1420)

typedef struct satip_rtp_session {
  TAILQ_ENTRY(satip_rtp_session) link;
  pthread_t tid;
  void *id;
  struct sockaddr_storage peer;
  struct sockaddr_storage peer2;
  int port;
  th_subscription_t *subs;
  streaming_queue_t *sq;
  int fd_rtp;
  int fd_rtcp;
  int frontend;
  int source;
  dvb_mux_conf_t dmc;
  mpegts_apids_t pids;
  udp_multisend_t um;
  struct iovec *um_iovec;
  int um_packet;
  uint16_t seq;
  signal_status_t sig;
  pthread_mutex_t lock;
} satip_rtp_session_t;

static pthread_mutex_t satip_rtp_lock;
static pthread_t satip_rtcp_tid;
static int satip_rtcp_run;
static TAILQ_HEAD(, satip_rtp_session) satip_rtp_sessions;

static void
satip_rtp_header(satip_rtp_session_t *rtp)
{
  struct iovec *v = rtp->um_iovec + rtp->um_packet;
  uint8_t *data = v->iov_base;
  uint32_t tstamp = dispatch_clock + rtp->seq;

  rtp->seq++;

  v->iov_len = 12;
  data[0] = 0x80;
  data[1] = 33;
  data[2] = (rtp->seq >> 8) & 0xff;
  data[3] = rtp->seq & 0xff;
  data[4] = (tstamp >> 24) & 0xff;
  data[5] = (tstamp >> 16) & 0xff;
  data[6] = (tstamp >> 8) & 0xff;
  data[7] = tstamp & 0xff;
  memset(data + 8, 0xa5, 4);
}

static int
satip_rtp_send(satip_rtp_session_t *rtp)
{
  struct iovec *v = rtp->um_iovec, *v2;
  int packets, copy, len, r;
  if (v->iov_len == RTP_PAYLOAD) {
    packets = rtp->um_packet;
    v2 = v + packets;
    copy = 1;
    if (v2->iov_len == RTP_PAYLOAD) {
      packets++;
      copy = 0;
    }
    r = udp_multisend_send(&rtp->um, rtp->fd_rtp, packets);
    if (r < 0)
      return r;
    if (copy)
      memcpy(v->iov_base, v2->iov_base, len = v2->iov_len);
    else
      len = 0;
    rtp->um_packet = 0;
    udp_multisend_clean(&rtp->um);
    v->iov_len = len;
  }
  if (v->iov_len == 0)
    satip_rtp_header(rtp);
  return 0;
}

static int
satip_rtp_loop(satip_rtp_session_t *rtp, uint8_t *data, int len)
{
  int i, j, pid, last_pid = -1, r;
  mpegts_apid_t *pids = rtp->pids.pids;
  struct iovec *v = rtp->um_iovec + rtp->um_packet;

  assert((len % 188) == 0);
  for ( ; len >= 188 ; data += 188, len -= 188) {
    pid = ((data[1] & 0x1f) << 8) | data[2];
    if (pid != last_pid && !rtp->pids.all) {
      for (i = 0; i < rtp->pids.count; i++) {
        j = pids[i];
        if (pid < j) break;
        if (j == pid) goto found;
      }
      continue;
found:
      last_pid = pid;
    }
    assert(v->iov_len + 188 <= RTP_PAYLOAD);
    memcpy(v->iov_base + v->iov_len, data, 188);
    v->iov_len += 188;
    if (v->iov_len == RTP_PAYLOAD) {
      if ((rtp->um_packet + 1) == RTP_PACKETS) {
        r = satip_rtp_send(rtp);
        if (r < 0)
          return r;
      } else {
        rtp->um_packet++;
        satip_rtp_header(rtp);
      }
      v = rtp->um_iovec + rtp->um_packet;
    } else {
      assert(v->iov_len < RTP_PAYLOAD);
    }
  }
  return 0;
}

static void
satip_rtp_signal_status(satip_rtp_session_t *rtp, signal_status_t *sig)
{
  rtp->sig = *sig;
}

static void *
satip_rtp_thread(void *aux)
{
  satip_rtp_session_t *rtp = aux;
  streaming_queue_t *sq = rtp->sq;
  streaming_message_t *sm;
  th_subscription_t *subs = rtp->subs;
  pktbuf_t *pb;
  char peername[50];
  int alive = 1, fatal = 0, r;

  tcp_get_ip_str((struct sockaddr *)&rtp->peer, peername, sizeof(peername));
  tvhdebug("satips", "RTP streaming to %s:%d open", peername, rtp->port);

  pthread_mutex_lock(&sq->sq_mutex);
  while (rtp->sq && !fatal) {
    sm = TAILQ_FIRST(&sq->sq_queue);
    if (sm == NULL) {
      r = satip_rtp_send(rtp);
      if (r) {
        fatal = 1;
        continue;
      }
      pthread_cond_wait(&sq->sq_cond, &sq->sq_mutex);
      continue;
    }
    TAILQ_REMOVE(&sq->sq_queue, sm, sm_link);
    pthread_mutex_unlock(&sq->sq_mutex);

    switch (sm->sm_type) {
    case SMT_MPEGTS:
      pb = sm->sm_data;
      atomic_add(&subs->ths_bytes_out, pktbuf_len(pb));
      pthread_mutex_lock(&rtp->lock);
      r = satip_rtp_loop(rtp, pktbuf_ptr(pb), pktbuf_len(pb));
      pthread_mutex_unlock(&rtp->lock);
      if (r) fatal = 1;
      break;
    case SMT_SIGNAL_STATUS:
      satip_rtp_signal_status(rtp, sm->sm_data);
      break;
    case SMT_NOSTART:
    case SMT_EXIT:
      alive = 0;
      break;

    case SMT_START:
    case SMT_STOP:
    case SMT_PACKET:
    case SMT_GRACE:
    case SMT_SKIP:
    case SMT_SPEED:
    case SMT_SERVICE_STATUS:
    case SMT_TIMESHIFT_STATUS:
      break;
    }

    streaming_msg_free(sm);
    pthread_mutex_lock(&sq->sq_mutex);
  }
  pthread_mutex_unlock(&sq->sq_mutex);

  tvhdebug("satips", "RTP streaming to %s:%d closed (%s request)",
           peername, rtp->port, alive ? "remote" : "streaming");

  return NULL;
}

/*
 *
 */
static satip_rtp_session_t *
satip_rtp_find(void *id)
{
  satip_rtp_session_t *rtp;

  TAILQ_FOREACH(rtp, &satip_rtp_sessions, link)
    if (rtp->id == id)
      break;
  return rtp;
}

/*
 *
 */
void satip_rtp_queue(void *id, th_subscription_t *subs,
                     streaming_queue_t *sq,
                     struct sockaddr_storage *peer, int port,
                     int fd_rtp, int fd_rtcp,
                     int frontend, int source, dvb_mux_conf_t *dmc,
                     mpegts_apids_t *pids)
{
  satip_rtp_session_t *rtp = calloc(1, sizeof(*rtp));

  if (rtp == NULL)
    return;

  rtp->id = id;
  rtp->peer = *peer;
  rtp->peer2 = *peer;
  IP_PORT_SET(rtp->peer2, htons(port + 1));
  rtp->port = port;
  rtp->fd_rtp = fd_rtp;
  rtp->fd_rtcp = fd_rtcp;
  rtp->subs = subs;
  rtp->sq = sq;
  mpegts_pid_init(&rtp->pids, NULL, pids->count);
  mpegts_pid_copy(&rtp->pids, pids);
  udp_multisend_init(&rtp->um, RTP_PACKETS, RTP_PAYLOAD, &rtp->um_iovec);
  satip_rtp_header(rtp);
  rtp->frontend = frontend;
  rtp->dmc = *dmc;
  rtp->source = source;
  pthread_mutex_init(&rtp->lock, NULL);

  pthread_mutex_lock(&satip_rtp_lock);
  TAILQ_INSERT_TAIL(&satip_rtp_sessions, rtp, link);
  tvhthread_create(&rtp->tid, NULL, satip_rtp_thread, rtp);
  pthread_mutex_unlock(&satip_rtp_lock);
}

void satip_rtp_update_pids(void *id, mpegts_apids_t *pids)
{
  satip_rtp_session_t *rtp;

  pthread_mutex_lock(&satip_rtp_lock);
  rtp = satip_rtp_find(id);
  if (rtp) {
    pthread_mutex_lock(&rtp->lock);
    mpegts_pid_copy(&rtp->pids, pids);
    pthread_mutex_unlock(&rtp->lock);
  }
  pthread_mutex_unlock(&satip_rtp_lock);
}

void satip_rtp_close(void *id)
{
  satip_rtp_session_t *rtp;
  streaming_queue_t *sq;

  pthread_mutex_lock(&satip_rtp_lock);
  rtp = satip_rtp_find(id);
  if (rtp) {
    TAILQ_REMOVE(&satip_rtp_sessions, rtp, link);
    sq = rtp->sq;
    pthread_mutex_lock(&sq->sq_mutex);
    rtp->sq = NULL;
    pthread_cond_signal(&sq->sq_cond);
    pthread_mutex_unlock(&sq->sq_mutex);
    pthread_mutex_unlock(&satip_rtp_lock);
    pthread_join(rtp->tid, NULL);
    udp_multisend_free(&rtp->um);
    mpegts_pid_done(&rtp->pids);
    free(rtp);
  } else {
    pthread_mutex_unlock(&satip_rtp_lock);
  }
}

/*
 *
 */
static const char *
satip_rtcp_fec(int fec)
{
  static char buf[16];
  char *p = buf;
  const char *s;

  if (fec == DVB_FEC_AUTO || fec == DVB_FEC_NONE)
    return "";
  s = dvb_fec2str(fec);
  if (s == NULL)
    return "";
  strncpy(buf, s, sizeof(buf));
  buf[sizeof(buf)-1] = '\0';
  p = strchr(buf, '/');
  while (*p) {
    *p = *(p+1);
    p++;
  }
  return s;
}

/*
 *
 */
static int
satip_rtcp_build(satip_rtp_session_t *rtp, uint8_t *msg)
{
  char buf[1500], pids[1400];
  const char *delsys, *msys, *pilot, *rolloff;
  const char *bw, *tmode, *gi, *plp, *t2id, *sm, *c2tft, *ds, *specinv;
  int i, len, len2, level = 0, lock = 0, quality = 0;

  if (rtp->sig.signal > 0)
    lock = 1;
  switch (rtp->sig.signal_scale) {
  case SIGNAL_STATUS_SCALE_RELATIVE:
    level = MIN(240, MAX(0, (rtp->sig.signal * 245) / 0xffff));
    break;
  case SIGNAL_STATUS_SCALE_DECIBEL:
    level = MIN(240, MAX(0, (rtp->sig.signal * 900000)));
    break;
  default:
    break;
  }
  switch (rtp->sig.snr_scale) {
  case SIGNAL_STATUS_SCALE_RELATIVE:
    quality = MIN(15, MAX(0, (rtp->sig.signal * 16) / 0xffff));
    break;
  case SIGNAL_STATUS_SCALE_DECIBEL:
    quality = MIN(15, MAX(0, (rtp->sig.signal * 100000)));
    break;
  default:
    break;
  }

  pids[0] = 0;
  for (i = len = 0; i < rtp->pids.count; i++)
    len += snprintf(pids + len, sizeof(pids) - len, "%d,", rtp->pids.pids[i]);
  if (len && pids[len-1] == ',')
    pids[len-1] = '\0';

  switch (rtp->dmc.dmc_fe_delsys) {
  case DVB_SYS_DVBS:
  case DVB_SYS_DVBS2:
    delsys = rtp->dmc.dmc_fe_delsys == DVB_SYS_DVBS ? "dvbs" : "dvbs2";
    switch (rtp->dmc.dmc_fe_modulation) {
    case DVB_MOD_QPSK:  msys = "qpsk"; break;
    case DVB_MOD_PSK_8: msys = "8psk"; break;
    default:            msys = ""; break;
    }
    switch (rtp->dmc.dmc_fe_pilot) {
    case DVB_PILOT_ON:  pilot = "on"; break;
    case DVB_PILOT_OFF: pilot = "off"; break;
    default:            pilot = ""; break;
    }
    switch (rtp->dmc.dmc_fe_rolloff) {
    case DVB_ROLLOFF_20: rolloff = "20"; break;
    case DVB_ROLLOFF_25: rolloff = "25"; break;
    case DVB_ROLLOFF_35: rolloff = "35"; break;
    default:             rolloff = ""; break;
    }
    /* ver=<major>.<minor>;src=<srcID>;tuner=<feID>,<level>,<lock>,<quality>,<frequency>,<polarisation>,\
     * <system>,<type>,<pilots>,<roll_off>,<symbol_rate>,<fec_inner>;pids=<pid0>,...,<pidn>
     */
    snprintf(buf, sizeof(buf),
      "vers=1.0;src=%d;tuner=%d,%d,%d,%d,%.f,%s,%s,%s,%s,%s,%.f,%s;pids=%s",
      rtp->source, rtp->frontend, level, lock, quality,
      (float)rtp->dmc.dmc_fe_freq / 1000000.0,
      dvb_pol2str(rtp->dmc.u.dmc_fe_qpsk.polarisation),
      delsys, msys, pilot, rolloff,
      (float)rtp->dmc.u.dmc_fe_qpsk.symbol_rate / 1000.0,
      satip_rtcp_fec(rtp->dmc.u.dmc_fe_qpsk.fec_inner),
      pids);
    break;
  case DVB_SYS_DVBT:
  case DVB_SYS_DVBT2:
    delsys = rtp->dmc.dmc_fe_delsys == DVB_SYS_DVBT ? "dvbt" : "dvbt2";
    switch (rtp->dmc.u.dmc_fe_ofdm.bandwidth) {
    case DVB_BANDWIDTH_1_712_MHZ:  bw = "1.712"; break;
    case DVB_BANDWIDTH_5_MHZ:      bw = "5"; break;
    case DVB_BANDWIDTH_6_MHZ:      bw = "6"; break;
    case DVB_BANDWIDTH_7_MHZ:      bw = "7"; break;
    case DVB_BANDWIDTH_8_MHZ:      bw = "8"; break;
    case DVB_BANDWIDTH_10_MHZ:     bw = "10"; break;
    default:                       bw = ""; break;
    }
    switch (rtp->dmc.u.dmc_fe_ofdm.transmission_mode) {
    case DVB_TRANSMISSION_MODE_1K:  tmode = "1k"; break;
    case DVB_TRANSMISSION_MODE_2K:  tmode = "2k"; break;
    case DVB_TRANSMISSION_MODE_4K:  tmode = "4k"; break;
    case DVB_TRANSMISSION_MODE_8K:  tmode = "8k"; break;
    case DVB_TRANSMISSION_MODE_16K: tmode = "16k"; break;
    case DVB_TRANSMISSION_MODE_32K: tmode = "32k"; break;
    default:                        tmode = ""; break;
    }
    switch (rtp->dmc.dmc_fe_modulation) {
    case DVB_MOD_QAM_16:  msys = "qam16"; break;
    case DVB_MOD_QAM_32:  msys = "qam32"; break;
    case DVB_MOD_QAM_64:  msys = "qam64"; break;
    case DVB_MOD_QAM_128: msys = "qam128"; break;
    default:              msys = ""; break;
    }
    switch (rtp->dmc.u.dmc_fe_ofdm.guard_interval) {
    case DVB_GUARD_INTERVAL_1_4:    gi = "14"; break;
    case DVB_GUARD_INTERVAL_1_8:    gi = "18"; break;
    case DVB_GUARD_INTERVAL_1_16:   gi = "116"; break;
    case DVB_GUARD_INTERVAL_1_32:   gi = "132"; break;
    case DVB_GUARD_INTERVAL_1_128:  gi = "1128"; break;
    case DVB_GUARD_INTERVAL_19_128: gi = "19128"; break;
    case DVB_GUARD_INTERVAL_19_256: gi = "19256"; break;
    default:                        gi = ""; break;
    }
    plp = "";
    t2id = "";
    sm = "";
    /* ver=1.1;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<tmode>,<mtype>,<gi>,\
     * <fec>,<plp>,<t2id>,<sm>;pids=<pid0>,...,<pidn>
     */
    snprintf(buf, sizeof(buf),
      "vers=1.1;tuner=%d,%d,%d,%d,%.f,%s,%s,%s,%s,%s,%s,%s,%s,%s;pids=%s",
      rtp->frontend, level, lock, quality,
      (float)rtp->dmc.dmc_fe_freq / 1000000.0,
      bw, delsys, tmode, msys, gi,
      satip_rtcp_fec(rtp->dmc.u.dmc_fe_ofdm.code_rate_HP),
      plp, t2id, sm, pids);
    break;
  case DVB_SYS_DVBC_ANNEX_A:
  case DVB_SYS_DVBC_ANNEX_C:
    delsys = rtp->dmc.dmc_fe_delsys == DVB_SYS_DVBC_ANNEX_A ? "dvbc" : "dvbc2";
    bw = "";
    switch (rtp->dmc.dmc_fe_modulation) {
    case DVB_MOD_QAM_16:  msys = "qam16"; break;
    case DVB_MOD_QAM_32:  msys = "qam32"; break;
    case DVB_MOD_QAM_64:  msys = "qam64"; break;
    case DVB_MOD_QAM_128: msys = "qam128"; break;
    default:              msys = ""; break;
    }
    c2tft = "";
    ds = "";
    plp = "";
    specinv = "";
    /* ver=1.2;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<mtype>,<sr>,<c2tft>,<ds>,<plp>,
     * <specinv>;pids=<pid0>,...,<pidn>
     */
    snprintf(buf, sizeof(buf),
      "vers=1.1;tuner=%d,%d,%d,%d,%.f,%s,%s,%s,%.f,%s,%s,%s,%s;pids=%s",
      rtp->frontend, level, lock, quality,
      (float)rtp->dmc.dmc_fe_freq / 1000000.0,
      bw, delsys, msys,
      (float)rtp->dmc.u.dmc_fe_qam.symbol_rate / 1000.0,
      c2tft, ds, plp, specinv, pids);
    break;
  default:
    return 0;
  }

  len = len2 = MIN(strlen(buf), RTCP_PAYLOAD - 16);
  if (len == 0)
    len++;
  while ((len % 4) != 0)
    buf[len++] = 0;
  memcpy(msg + 16, buf, len);

  len += 16;
  msg[0] = 0x80;
  msg[1] = 204;
  msg[2] = (((len - 1) / 4) >> 8) & 0xff;
  msg[3] = ((len - 1) / 4) & 0xff;
  msg[4] = 0;
  msg[5] = 0;
  msg[6] = 0;
  msg[7] = 0;
  msg[8] = 'S';
  msg[9] = 'E';
  msg[10] = 'S';
  msg[11] = '1';
  msg[12] = 0;
  msg[13] = 0;
  msg[14] = (len2 >> 8) & 0xff;
  msg[15] = len2 & 0xff;

  return len;
}

/*
 *
 */
static void *
satip_rtcp_thread(void *aux)
{
  satip_rtp_session_t *rtp;
  struct timespec ts;
  uint8_t msg[RTCP_PAYLOAD];
  char addrbuf[50];
  int r, len, err;

  while (satip_rtcp_run) {
    ts.tv_sec  = 0;
    ts.tv_nsec = 150000000;
    do {
      r = nanosleep(&ts, &ts);
      if (!satip_rtcp_run)
        goto end;
    } while (r && ts.tv_nsec);
    pthread_mutex_lock(&satip_rtp_lock);
    TAILQ_FOREACH(rtp, &satip_rtp_sessions, link) {
      if (rtp->sq == NULL) continue;
      len = satip_rtcp_build(rtp, msg);
      if (len <= 0) continue;
      r = sendto(rtp->fd_rtcp, msg, len, 0,
                 (struct sockaddr*)&rtp->peer2,
                 rtp->peer2.ss_family == AF_INET6 ?
                   sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
      if (r < 0) {
        err = errno;
        tcp_get_ip_str((struct sockaddr*)&rtp->peer2, addrbuf, sizeof(addrbuf));
        tvhwarn("satips", "RTCP send to error %s:%d : %s",
                addrbuf, IP_PORT(rtp->peer2), strerror(err));
      }
    }
    pthread_mutex_unlock(&satip_rtp_lock);
  }
end:
  return NULL;
}

/*
 *
 */
void satip_rtp_init(void)
{
  TAILQ_INIT(&satip_rtp_sessions);
  pthread_mutex_init(&satip_rtp_lock, NULL);

  satip_rtcp_run = 1;
  tvhthread_create(&satip_rtcp_tid, NULL, satip_rtcp_thread, NULL);
}

/*
 *
 */
void satip_rtp_done(void)
{
  assert(TAILQ_EMPTY(&satip_rtp_sessions));
  if (satip_rtcp_run) {
    satip_rtcp_run = 0;
    pthread_kill(satip_rtcp_tid, SIGTERM);
    pthread_join(satip_rtcp_tid, NULL);
  }
}
