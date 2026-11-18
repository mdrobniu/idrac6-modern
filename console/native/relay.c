/*
 * relay.c - Native iDRAC6 HTML5 KVM console relay for the Dell R210 II BMC
 * ("the-bmc", <IDRAC_IP>).  ARM926EJ-S / Linux 2.6.23.1 / OABI / glibc 2.3.6.
 *
 * Serves an HTTPS console (mongoose 7.14 built-in TLS, EC prime256v1 cert) on
 * :8443 and bridges a browser WebSocket (/ws) to the iDRAC Avocent KVM on
 * 127.0.0.1:5900.  The 5900 side speaks legacy TLS that mongoose's built-in
 * stack cannot, so it is handled with raw OpenSSL 1.0.2 in a dedicated pthread
 * per WS client.  Thread -> event-loop uses mg_wakeup(); event-loop -> thread
 * uses a mutex-guarded input queue.
 *
 * Protocol ported from PROTOCOL.md and build/src/oobmgr/kvm.go (authoritative).
 *
 * Build: see build.sh (cross-compile inside the oabi-build:lenny container).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "mongoose.h"
#include "embed.h"

/* ------------------------------------------------------------------ config */
#define IDRAC_IP        "127.0.0.1"   /* relay runs on the BMC itself */
#define IDRAC_PORT      5900
#define LISTEN_URL      "https://0.0.0.0:8443"
#define CERT_PATH       "/flash/data0/etc/oob/relay_ec.crt"
#define KEY_PATH        "/flash/data0/etc/oob/relay_ec.key"
#define CERT_DIR        "/flash/data0/etc/oob"

/* ------------------------------------------------------- TLS material (PEM) */
static char *g_cert_pem = NULL;   /* loaded at startup */
static char *g_key_pem  = NULL;

/* ---------------------------------------------------------- input commands */
/* Browser -> KVM thread, queued and drained by the thread. */
enum { IN_KEY = 1, IN_MOUSE, IN_FOCUS, IN_REFRESH, IN_POWER };
struct incmd {
  int kind;
  int code, down;              /* key */
  int x, y, buttons, wheel;    /* mouse */
  int on;                      /* focus */
  int state;                   /* power */
  struct incmd *next;
};

/* ------------------------------------------------------------ output queue */
/* KVM thread -> event loop: pre-framed WS binary payloads. */
struct outmsg {
  unsigned char *buf;
  size_t len;
  struct outmsg *next;
};

/* ------------------------------------------------------------- KVM session */
struct session {
  struct mg_mgr *mgr;
  unsigned long conn_id;       /* WS connection id for mg_wakeup */

  char user[128];
  char pass[128];

  /* 5900 sockets */
  int ctrl_fd;
  int video_fd;
  SSL_CTX *ssl_ctx;
  SSL *ctrl_ssl;               /* control channel is TLS */
  SSL *video_ssl;              /* usually NULL (video is plain) */

  uint32_t crand;              /* client random, echoed into video auth */

  pthread_mutex_t lk;
  struct incmd *in_head, *in_tail;
  struct outmsg *out_head, *out_tail;

  volatile int stop;           /* set by either side to end the session */
  int refs;                    /* 2 = ws side + thread; last frees */

  int allow_power;             /* gated off by default */
};

/* ---------------------------------------------------------------- helpers */

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *read_file(const char *path, size_t *outlen) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  char *b = (char *) malloc(n + 1);
  if (!b) { fclose(f); return NULL; }
  if (fread(b, 1, n, f) != (size_t) n) { free(b); fclose(f); return NULL; }
  b[n] = 0;
  if (outlen) *outlen = (size_t) n;
  fclose(f);
  return b;
}

/* Generate an EC prime256v1 self-signed cert via the box's /bin/openssl if the
 * cached pair is missing.  mongoose built-in TLS 1.3 needs EC, not RSA. */
static void ensure_cert(void) {
  if (access(CERT_PATH, R_OK) == 0 && access(KEY_PATH, R_OK) == 0) return;
  MG_INFO(("generating EC prime256v1 cert via the box openssl ..."));
  char cmd[2048];
  snprintf(cmd, sizeof(cmd), "mkdir -p %s", CERT_DIR);
  system(cmd);
  /* The box's openssl has no default openssl.cnf and no writable RANDFILE, so
   * supply a minimal -config and point RANDFILE at /tmp.  prime256v1 == secp256r1,
   * which mongoose's built-in TLS 1.3 requires (RSA will not work). */
  snprintf(cmd, sizeof(cmd),
    "RANDFILE=/tmp/.oob_rnd; export RANDFILE; "
    "printf '[req]\\ndistinguished_name=dn\\nprompt=no\\n[dn]\\nCN=idrac-oob\\n' > /tmp/oob_ssl.cnf; "
    "openssl ecparam -name prime256v1 -genkey -noout -out %s 2>/dev/null && "
    "openssl req -new -x509 -config /tmp/oob_ssl.cnf -key %s -out %s -days 3650 2>/dev/null; "
    "chmod 600 %s 2>/dev/null",
    KEY_PATH, KEY_PATH, CERT_PATH, KEY_PATH);
  if (system(cmd) != 0)
    MG_ERROR(("openssl cert generation returned non-zero (will retry via embedded check)"));
}

/* read exactly n bytes from an fd or SSL; returns 0 on success, -1 on error */
static int readn_fd(int fd, void *buf, size_t n) {
  unsigned char *p = (unsigned char *) buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, p + got, n - got, 0);
    if (r == 0) return -1;
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    got += (size_t) r;
  }
  return 0;
}

static int readn_ssl(SSL *s, void *buf, size_t n) {
  unsigned char *p = (unsigned char *) buf;
  size_t got = 0;
  while (got < n) {
    int r = SSL_read(s, p + got, (int) (n - got));
    if (r <= 0) {
      int e = SSL_get_error(s, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
      return -1;
    }
    got += (size_t) r;
  }
  return 0;
}

/* channel-agnostic read/write: control uses SSL, video is plain here */
static int chan_read(struct session *s, int ctrl, void *buf, size_t n) {
  if (ctrl) return readn_ssl(s->ctrl_ssl, buf, n);
  if (s->video_ssl) return readn_ssl(s->video_ssl, buf, n);
  return readn_fd(s->video_fd, buf, n);
}

static int write_all_fd(int fd, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *) buf;
  size_t sent = 0;
  while (sent < n) {
    ssize_t r = send(fd, p + sent, n - sent, 0);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    sent += (size_t) r;
  }
  return 0;
}

static int write_all_ssl(SSL *s, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *) buf;
  size_t sent = 0;
  while (sent < n) {
    int r = SSL_write(s, p + sent, (int) (n - sent));
    if (r <= 0) {
      int e = SSL_get_error(s, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
      return -1;
    }
    sent += (size_t) r;
  }
  return 0;
}

static int ctrl_write(struct session *s, const void *buf, size_t n) {
  return write_all_ssl(s->ctrl_ssl, buf, n);
}
static int video_write(struct session *s, const void *buf, size_t n) {
  if (s->video_ssl) return write_all_ssl(s->video_ssl, buf, n);
  return write_all_fd(s->video_fd, buf, n);
}

/* BEEF control message: "BEEF" + be16 type + be16 total_len + body (>=8). */
static int beef(struct session *s, uint16_t typ, const unsigned char *body, int blen) {
  unsigned char pad[8];
  if (blen < 8) {
    memset(pad, 0, 8);
    if (body && blen > 0) memcpy(pad, body, blen);
    body = pad;
    blen = 8;
  }
  unsigned char hdr[8];
  memcpy(hdr, "BEEF", 4);
  uint16_t total = (uint16_t) (8 + blen);
  hdr[4] = typ >> 8; hdr[5] = typ & 0xFF;
  hdr[6] = total >> 8; hdr[7] = total & 0xFF;
  unsigned char *pkt = (unsigned char *) malloc(8 + blen);
  if (!pkt) return -1;
  memcpy(pkt, hdr, 8);
  memcpy(pkt + 8, body, blen);
  int rc = ctrl_write(s, pkt, 8 + blen);
  free(pkt);
  return rc;
}

/* ------------------------------------------------------- APCP + TLS setup */

/* Blocking TCP connect to IDRAC_IP:port. Returns fd or -1. */
static int tcp_connect(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t) port);
  sa.sin_addr.s_addr = inet_addr(IDRAC_IP);
  if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* APCP 53-byte hello; reads 53-byte reply. Returns caps (or 0xFFFFFFFF err).
 * Writes redirect port into *redir. */
static uint32_t apcp(int fd, unsigned char chan_type, uint16_t *redir) {
  unsigned char pkt[53];
  memset(pkt, 0, sizeof(pkt));
  memcpy(pkt, "APCP", 4);
  uint32_t total = 53;
  pkt[4] = total >> 24; pkt[5] = total >> 16; pkt[6] = total >> 8; pkt[7] = total;
  pkt[8] = 0x01; pkt[9] = 0x00;   /* type 0x0100 connect request */
  pkt[10] = 0x00; pkt[11] = 0x00;
  pkt[12] = chan_type;            /* 3 = control, 4 = video */
  pkt[13] = 2;                    /* major */
  pkt[14] = 34;                   /* minor */
  pkt[15] = 0;
  pkt[16] = 0; pkt[17] = 0; pkt[18] = 0; pkt[19] = 5;  /* caps = 5 */
  pkt[20] = 32;                   /* random length */
  RAND_bytes(pkt + 21, 32);
  if (write_all_fd(fd, pkt, 53) != 0) return 0xFFFFFFFFu;

  unsigned char hdr[8];
  if (readn_fd(fd, hdr, 8) != 0) return 0xFFFFFFFFu;
  uint32_t ln = ((uint32_t) hdr[4] << 24) | ((uint32_t) hdr[5] << 16) |
                ((uint32_t) hdr[6] << 8) | hdr[7];
  if (ln < 8 || ln > 4096) return 0xFFFFFFFFu;
  unsigned char body[4096];
  if (readn_fd(fd, body, ln - 8) != 0) return 0xFFFFFFFFu;
  /* body: u16 type, u16 z, u8 maj, u8 min, u32 caps, u16 port, ... */
  uint32_t caps = ((uint32_t) body[6] << 24) | ((uint32_t) body[7] << 16) |
                  ((uint32_t) body[8] << 8) | body[9];
  if (redir) *redir = (uint16_t) (((uint16_t) body[10] << 8) | body[11]);
  return caps;
}

/* Dial a redirect channel: APCP, follow redirect port if any. Returns fd and
 * caps via *caps_out. */
static int dial_redirect(unsigned char chan_type, uint32_t *caps_out) {
  int fd = tcp_connect(IDRAC_PORT);
  if (fd < 0) return -1;
  uint16_t redir = 0;
  uint32_t caps = apcp(fd, chan_type, &redir);
  if (caps == 0xFFFFFFFFu) { close(fd); return -1; }
  if (redir != 0) {
    close(fd);
    fd = tcp_connect(redir);
    if (fd < 0) return -1;
    /* caps from first reply stand (matches kvm.go) */
  }
  *caps_out = caps;
  return fd;
}

/* Wrap an fd in client TLS using legacy-permissive OpenSSL settings. */
static SSL *tls_wrap(SSL_CTX *ctx, int fd) {
  SSL *ssl = SSL_new(ctx);
  if (!ssl) return NULL;
  SSL_set_fd(ssl, fd);
  SSL_set_connect_state(ssl);
  for (;;) {
    int r = SSL_connect(ssl);
    if (r == 1) break;
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
    SSL_free(ssl);
    return NULL;
  }
  return ssl;
}

static SSL_CTX *make_ssl_ctx(void) {
  SSL_CTX *ctx = SSL_CTX_new(SSLv23_client_method());
  if (!ctx) return NULL;
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  /* allow old/weak ciphers (iDRAC6 legacy TLS); SECLEVEL=0 lowers OpenSSL bar */
  SSL_CTX_set_cipher_list(ctx, "ALL:@SECLEVEL=0");
  return ctx;
}

/* ----------------------------------------------- KVM login & video auth */

static void pad_copy(unsigned char *dst, const char *src, int field) {
  memset(dst, 0, field);
  int l = (int) strlen(src);
  if (l > field) l = field;
  memcpy(dst, src, l);
}

/* Returns video key on success, or -1 (as uint32 0xFFFFFFFF sentinel via *ok). */
static uint32_t kvm_login(struct session *s, int *ok) {
  *ok = 0;
  unsigned char cr3[3];
  RAND_bytes(cr3, 3);
  uint32_t crand = (((uint32_t) cr3[0] << 16) | ((uint32_t) cr3[1] << 8) | cr3[2]) % 10000000u;
  s->crand = crand;

  unsigned char body[209];
  memset(body, 0, sizeof(body));
  int ulen = (int) strlen(s->user); if (ulen > 96) ulen = 96;
  int plen = (int) strlen(s->pass); if (plen > 96) plen = 96;
  body[0] = (unsigned char) ulen;
  pad_copy(body + 1, s->user, 96);
  body[97] = (unsigned char) plen;
  pad_copy(body + 98, s->pass, 96);
  /* [194..201] rip zeros already */
  body[202] = 1;   /* port */
  body[203] = 0;   /* channel */
  body[204] = crand >> 24; body[205] = crand >> 16;
  body[206] = crand >> 8;  body[207] = crand;
  body[208] = 0;   /* share mode */

  if (beef(s, 0x0102, body, sizeof(body)) != 0) return 0;

  unsigned char hdr[8];
  if (readn_ssl(s->ctrl_ssl, hdr, 8) != 0) return 0;
  uint16_t typ = ((uint16_t) hdr[4] << 8) | hdr[5];
  uint16_t ln  = ((uint16_t) hdr[6] << 8) | hdr[7];
  if (ln < 8 || ln > 15000) return 0;
  unsigned char rb[15000];
  if (readn_ssl(s->ctrl_ssl, rb, ln - 8) != 0) return 0;
  if (typ != 0x8305) return 0;
  if (rb[0] != 0) {
    MG_ERROR(("KVM login refused result=%d", rb[0]));
    return 0;
  }
  uint32_t vkey = ((uint32_t) rb[4] << 24) | ((uint32_t) rb[5] << 16) |
                  ((uint32_t) rb[6] << 8) | rb[7];
  *ok = 1;
  return vkey;
}

/* ------------------------------------------------------- output plumbing */

static void queue_out(struct session *s, unsigned char *buf, size_t len) {
  struct outmsg *m = (struct outmsg *) malloc(sizeof(*m));
  if (!m) { free(buf); return; }
  m->buf = buf; m->len = len; m->next = NULL;
  pthread_mutex_lock(&s->lk);
  if (s->out_tail) s->out_tail->next = m; else s->out_head = m;
  s->out_tail = m;
  pthread_mutex_unlock(&s->lk);
  mg_wakeup(s->mgr, s->conn_id, "", 0);
}

/* 0x01 video frame: byte 0x01 + be16 mode + be16 w + be16 h + data */
static void emit_frame(struct session *s, uint16_t mode, uint16_t w, uint16_t h,
                       const unsigned char *data, size_t dlen) {
  size_t len = 7 + dlen;
  unsigned char *b = (unsigned char *) malloc(len);
  if (!b) return;
  b[0] = 0x01;
  b[1] = mode >> 8; b[2] = mode & 0xFF;
  b[3] = w >> 8;    b[4] = w & 0xFF;
  b[5] = h >> 8;    b[6] = h & 0xFF;
  if (dlen) memcpy(b + 7, data, dlen);
  queue_out(s, b, len);
}

/* 0x02 JSON event: byte 0x02 + json */
static void emit_event(struct session *s, const char *json) {
  size_t jl = strlen(json);
  unsigned char *b = (unsigned char *) malloc(1 + jl);
  if (!b) return;
  b[0] = 0x02;
  memcpy(b + 1, json, jl);
  queue_out(s, b, 1 + jl);
}

/* ------------------------------------------------------- input plumbing */

static void push_in(struct session *s, struct incmd *c) {
  c->next = NULL;
  pthread_mutex_lock(&s->lk);
  if (s->in_tail) s->in_tail->next = c; else s->in_head = c;
  s->in_tail = c;
  pthread_mutex_unlock(&s->lk);
}

static struct incmd *pop_in(struct session *s) {
  pthread_mutex_lock(&s->lk);
  struct incmd *c = s->in_head;
  if (c) { s->in_head = c->next; if (!s->in_head) s->in_tail = NULL; }
  pthread_mutex_unlock(&s->lk);
  return c;
}

static void apply_input(struct session *s, struct incmd *c) {
  unsigned char b[8];
  switch (c->kind) {
    case IN_KEY: {
      b[0] = 0; b[1] = c->down ? 0 : 1;
      b[2] = (c->code >> 8) & 0xFF; b[3] = c->code & 0xFF;
      beef(s, 0x0200, b, 4);
      break;
    }
    case IN_MOUSE: {
      memset(b, 0, 8);
      b[1] = (unsigned char) (c->buttons & 7);
      b[2] = (c->x >> 8) & 0xFF; b[3] = c->x & 0xFF;
      b[4] = (c->y >> 8) & 0xFF; b[5] = c->y & 0xFF;
      b[6] = (c->wheel >> 8) & 0xFF; b[7] = c->wheel & 0xFF;
      beef(s, 0x0201, b, 8);
      break;
    }
    case IN_FOCUS: {
      b[0] = c->on ? 1 : 0;
      beef(s, 0x0208, b, 1);
      break;
    }
    case IN_REFRESH: {
      beef(s, 0x0301, NULL, 0);
      break;
    }
    case IN_POWER: {
      if (s->allow_power && c->state >= 1 && c->state <= 5) {
        const char *sid = "current1";
        int sl = (int) strlen(sid);
        unsigned char pb[64];
        pb[0] = (sl >> 8) & 0xFF; pb[1] = sl & 0xFF;
        memcpy(pb + 2, sid, sl);
        pb[2 + sl] = (unsigned char) c->state;
        beef(s, 0x0424, pb, 3 + sl);
      }
      break;
    }
  }
}

/* --------------------------------------------------- startVideo on 0x84 */

static void start_video(struct session *s) {
  unsigned char b[4];
  b[0] = 1; b[1] = 1;
  beef(s, 0x030E, b, 2);               /* VideoEnable */
  b[0] = (1024 >> 8); b[1] = 1024 & 0xFF; b[2] = (768 >> 8); b[3] = 768 & 0xFF;
  beef(s, 0x0302, b, 4);               /* SetDisplayArea 1024x768 */
  beef(s, 0x0304, NULL, 0);            /* SetScaleMode1to1 */
  b[0] = 1;
  beef(s, 0x0208, b, 1);               /* focus -> enable keyboard */
}

/* ------------------------------------------------------------ KVM thread */

static void session_release(struct session *s) {
  pthread_mutex_lock(&s->lk);
  int done = (--s->refs == 0);
  pthread_mutex_unlock(&s->lk);
  if (!done) return;
  /* free queues + ssl + fds */
  struct incmd *ic = s->in_head;
  while (ic) { struct incmd *n = ic->next; free(ic); ic = n; }
  struct outmsg *om = s->out_head;
  while (om) { struct outmsg *n = om->next; free(om->buf); free(om); om = n; }
  if (s->ctrl_ssl) { SSL_shutdown(s->ctrl_ssl); SSL_free(s->ctrl_ssl); }
  if (s->video_ssl) { SSL_shutdown(s->video_ssl); SSL_free(s->video_ssl); }
  if (s->ctrl_fd >= 0) close(s->ctrl_fd);
  if (s->video_fd >= 0) close(s->video_fd);
  if (s->ssl_ctx) SSL_CTX_free(s->ssl_ctx);
  pthread_mutex_destroy(&s->lk);
  free(s);
}

static void *kvm_thread(void *arg) {
  struct session *s = (struct session *) arg;

  s->ssl_ctx = make_ssl_ctx();
  if (!s->ssl_ctx) { emit_event(s, "{\"t\":\"error\",\"msg\":\"ssl ctx\"}"); goto done; }

  /* control channel */
  uint32_t caps = 0;
  s->ctrl_fd = dial_redirect(3, &caps);
  if (s->ctrl_fd < 0) { emit_event(s, "{\"t\":\"error\",\"msg\":\"connect 5900 (control) failed\"}"); goto done; }
  if ((caps & 1) == 0 && (caps & 4)) {
    s->ctrl_ssl = tls_wrap(s->ssl_ctx, s->ctrl_fd);
    if (!s->ctrl_ssl) { emit_event(s, "{\"t\":\"error\",\"msg\":\"control TLS handshake failed\"}"); goto done; }
  } else {
    emit_event(s, "{\"t\":\"error\",\"msg\":\"control channel not TLS as expected\"}");
    goto done;
  }

  int ok = 0;
  uint32_t vkey = kvm_login(s, &ok);
  if (!ok) { emit_event(s, "{\"t\":\"error\",\"msg\":\"KVM login refused (bad iDRAC credentials?)\"}"); goto done; }

  /* video channel */
  uint32_t vcaps = 0;
  s->video_fd = dial_redirect(4, &vcaps);
  if (s->video_fd < 0) { emit_event(s, "{\"t\":\"error\",\"msg\":\"connect 5900 (video) failed\"}"); goto done; }
  if ((vcaps & 1) == 0 && (vcaps & 4)) {
    s->video_ssl = tls_wrap(s->ssl_ctx, s->video_fd);
    if (!s->video_ssl) { emit_event(s, "{\"t\":\"error\",\"msg\":\"video TLS handshake failed\"}"); goto done; }
  }
  /* VideoChannelAuth: 00 00 00 00 01 01 00 10 + be32 crand + be32 vkey */
  {
    unsigned char va[16] = {0,0,0,0,1,1,0,0x10, 0,0,0,0, 0,0,0,0};
    va[8]  = s->crand >> 24; va[9]  = s->crand >> 16; va[10] = s->crand >> 8; va[11] = s->crand;
    va[12] = vkey >> 24;     va[13] = vkey >> 16;     va[14] = vkey >> 8;     va[15] = vkey;
    if (video_write(s, va, 16) != 0) { emit_event(s, "{\"t\":\"error\",\"msg\":\"video auth write failed\"}"); goto done; }
  }

  emit_event(s, "{\"t\":\"status\",\"msg\":\"connected\"}");

  /* main multiplex loop over control + video */
  {
    unsigned char *frame = NULL;   /* reassembly buffer */
    size_t frame_len = 0, frame_cap = 0;
    int started = 0;
    uint16_t last_type = 0;
    int vcount = 0;
    uint16_t disp_w = 1024, disp_h = 768;
    uint64_t last_ka = now_ms();

    while (!s->stop) {
      /* drain browser input onto the control channel */
      struct incmd *ic;
      while ((ic = pop_in(s)) != NULL) { apply_input(s, ic); free(ic); }

      /* keepalive every 2s */
      uint64_t t = now_ms();
      if (t - last_ka >= 2000) {
        beef(s, 0x0400, NULL, 0);
        if (disp_w) beef(s, 0x0420, NULL, 0);
        last_ka = t;
      }

      struct pollfd pfd[2];
      pfd[0].fd = s->ctrl_fd;  pfd[0].events = POLLIN; pfd[0].revents = 0;
      pfd[1].fd = s->video_fd; pfd[1].events = POLLIN; pfd[1].revents = 0;
      int pr = poll(pfd, 2, 200);
      if (pr < 0) { if (errno == EINTR) continue; break; }

      /* control: drain all buffered TLS records */
      if ((pfd[0].revents & POLLIN) || (s->ctrl_ssl && SSL_pending(s->ctrl_ssl) > 0)) {
        do {
          unsigned char hdr[8];
          if (readn_ssl(s->ctrl_ssl, hdr, 8) != 0) { s->stop = 1; break; }
          uint16_t typ = ((uint16_t) hdr[4] << 8) | hdr[5];
          uint16_t ln  = ((uint16_t) hdr[6] << 8) | hdr[7];
          if (ln < 8 || ln > 15000) { s->stop = 1; break; }
          unsigned char cb[15000];
          if (readn_ssl(s->ctrl_ssl, cb, ln - 8) != 0) { s->stop = 1; break; }
          if (typ == 0x8430) {
            if (ln - 8 > 0) {
              int on = cb[ln - 9] & 1;
              emit_event(s, on ? "{\"t\":\"power_status\",\"on\":true}"
                              : "{\"t\":\"power_status\",\"on\":false}");
            }
          } else if (typ == 0x8301) {
            s->stop = 1;
          }
        } while (!s->stop && s->ctrl_ssl && SSL_pending(s->ctrl_ssl) > 0);
      }

      /* video */
      if (!s->stop && (pfd[1].revents & POLLIN)) {
        unsigned char hdr[8];
        if (chan_read(s, 0, hdr, 8) != 0) { s->stop = 1; break; }
        uint16_t typ = ((uint16_t) hdr[4] << 8) | hdr[5];
        uint16_t ln  = ((uint16_t) hdr[6] << 8) | hdr[7];
        if (ln < 8 || ln > 15000) { s->stop = 1; break; }
        unsigned char vb[15000];
        if (chan_read(s, 0, vb, ln - 8) != 0) { s->stop = 1; break; }
        unsigned low = typ & 0xFF;
        if (low == 0x84) {
          start_video(s);
        } else if (low == 0x81 || low == 0x82 || low == 0x83 || low == 0x8A) {
          if (typ != last_type) {
            last_type = typ;
            unsigned char cd[4] = {1, 0, 0, 0};
            beef(s, 0x0403, cd, 4);
          }
          if (ln - 8 >= 12) {
            uint16_t h = ((uint16_t) vb[4] << 8) | vb[5];
            uint16_t w = ((uint16_t) vb[6] << 8) | vb[7];
            unsigned char flags = vb[8];
            unsigned char *data = vb + 12;
            size_t dlen = (ln - 8) - 12;
            if (flags & 1) { started = 1; frame_len = 0; }
            if (started) {
              if (frame_len + dlen > frame_cap) {
                size_t nc = frame_cap ? frame_cap * 2 : 65536;
                while (nc < frame_len + dlen) nc *= 2;
                unsigned char *nf = (unsigned char *) realloc(frame, nc);
                if (!nf) { s->stop = 1; break; }
                frame = nf; frame_cap = nc;
              }
              memcpy(frame + frame_len, data, dlen);
              frame_len += dlen;
              if (flags & 2) {
                if (w != disp_w || h != disp_h) {
                  disp_w = w; disp_h = h;
                  unsigned char da[4];
                  da[0] = w >> 8; da[1] = w & 0xFF; da[2] = h >> 8; da[3] = h & 0xFF;
                  beef(s, 0x0302, da, 4);
                }
                emit_frame(s, typ, w, h, frame, frame_len);
                frame_len = 0;
              }
            }
          }
          vcount++;
        } else {
          vcount++;
        }
        if (vcount >= 20) {
          /* Video Ack on the VIDEO socket: BEEF type 0x0000 total16, byte0=count */
          unsigned char ack[16];
          memset(ack, 0, sizeof(ack));
          memcpy(ack, "BEEF", 4);
          ack[4] = 0; ack[5] = 0; ack[6] = 0; ack[7] = 16;
          ack[8] = (unsigned char) (vcount & 0xFF);
          video_write(s, ack, 16);
          vcount = 0;
        }
      }
    }
    free(frame);
  }

done:
  emit_event(s, "{\"t\":\"closed\"}");
  s->stop = 1;
  session_release(s);
  return NULL;
}

/* -------------------------------------------------- mongoose event handler */

static void start_session(struct mg_connection *c, const char *user, const char *pass) {
  struct session *s = (struct session *) calloc(1, sizeof(*s));
  if (!s) return;
  s->mgr = c->mgr;
  s->conn_id = c->id;
  s->ctrl_fd = s->video_fd = -1;
  s->refs = 2;             /* ws side + thread */
  s->allow_power = 0;      /* gated off by default */
  pthread_mutex_init(&s->lk, NULL);
  snprintf(s->user, sizeof(s->user), "%s", user ? user : "");
  snprintf(s->pass, sizeof(s->pass), "%s", pass ? pass : "");
  c->fn_data = s;

  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, kvm_thread, s) != 0) {
    pthread_attr_destroy(&attr);
    c->fn_data = NULL;
    s->refs = 1;           /* only ws side; release now */
    session_release(s);
    return;
  }
  pthread_attr_destroy(&attr);
}

static void drain_outq(struct mg_connection *c, struct session *s) {
  for (;;) {
    pthread_mutex_lock(&s->lk);
    struct outmsg *m = s->out_head;
    if (m) { s->out_head = m->next; if (!s->out_head) s->out_tail = NULL; }
    pthread_mutex_unlock(&s->lk);
    if (!m) break;
    mg_ws_send(c, m->buf, m->len, WEBSOCKET_OP_BINARY);
    free(m->buf);
    free(m);
  }
}

static void handle_ws_text(struct session *s, struct mg_str d) {
  if (!s) return;
  char *t = mg_json_get_str(d, "$.t");
  if (!t) return;
  struct incmd *c = (struct incmd *) calloc(1, sizeof(*c));
  if (!c) { free(t); return; }
  if (strcmp(t, "key") == 0) {
    c->kind = IN_KEY;
    c->code = (int) mg_json_get_long(d, "$.code", 0);
    bool dn = false; mg_json_get_bool(d, "$.down", &dn); c->down = dn ? 1 : 0;
  } else if (strcmp(t, "mouse") == 0) {
    c->kind = IN_MOUSE;
    c->x = (int) mg_json_get_long(d, "$.x", 0);
    c->y = (int) mg_json_get_long(d, "$.y", 0);
    c->buttons = (int) mg_json_get_long(d, "$.buttons", 0);
    c->wheel = (int) mg_json_get_long(d, "$.wheel", 0);
  } else if (strcmp(t, "focus") == 0) {
    c->kind = IN_FOCUS;
    bool on = false; mg_json_get_bool(d, "$.on", &on); c->on = on ? 1 : 0;
  } else if (strcmp(t, "refresh") == 0) {
    c->kind = IN_REFRESH;
  } else if (strcmp(t, "power") == 0) {
    c->kind = IN_POWER;
    c->state = (int) mg_json_get_long(d, "$.state", 0);
  } else {
    free(c); free(t); return;
  }
  free(t);
  push_in(s, c);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_ACCEPT) {
    /* enable per-connection TLS (built-in, EC cert) */
    struct mg_tls_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.cert = mg_str(g_cert_pem);
    opts.key  = mg_str(g_key_pem);
    mg_tls_init(c, &opts);
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
      mg_ws_upgrade(c, hm, NULL);
      return;
    }
    if (mg_match(hm->uri, mg_str("/static/app.js"), NULL)) {
      mg_http_reply(c, 200,
        "Content-Type: application/javascript\r\nCache-Control: no-cache\r\n",
        "%.*s", (int) EMB_APPJS_len, (const char *) EMB_APPJS);
      return;
    }
    /* "/" and anything else -> index */
    mg_http_reply(c, 200,
      "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n",
      "%.*s", (int) EMB_INDEX_len, (const char *) EMB_INDEX);
  } else if (ev == MG_EV_WS_OPEN) {
    /* The console page connects anonymously; it authenticates the KVM with the
     * real iDRAC credentials.  Those are injected via env for headless use, or
     * default to the box root creds.  (A /api/login form can set them per the
     * spec; kept simple here: creds come from the relay config.) */
    const char *u = getenv("IDRAC_USER");
    const char *p = getenv("IDRAC_PASS");
    start_session(c, u ? u : "root", p ? p : "");
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    struct session *s = (struct session *) c->fn_data;
    handle_ws_text(s, wm->data);
  } else if (ev == MG_EV_WAKEUP) {
    struct session *s = (struct session *) c->fn_data;
    if (s) drain_outq(c, s);
  } else if (ev == MG_EV_CLOSE) {
    struct session *s = (struct session *) c->fn_data;
    if (s && c->is_websocket) {
      c->fn_data = NULL;
      s->stop = 1;
      /* flush anything already queued (best effort) */
      session_release(s);
    }
  }
}

int main(int argc, char **argv) {
  (void) argc; (void) argv;
  /* ignore SIGPIPE so a dropped socket never kills the relay */
  signal(SIGPIPE, SIG_IGN);
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  ensure_cert();
  size_t cl = 0, kl = 0;
  g_cert_pem = read_file(CERT_PATH, &cl);
  g_key_pem  = read_file(KEY_PATH, &kl);
  if (!g_cert_pem || !g_key_pem) {
    fprintf(stderr, "FATAL: cannot load EC cert/key (%s , %s)\n", CERT_PATH, KEY_PATH);
    return 1;
  }

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  mg_wakeup_init(&mgr);

  struct mg_connection *c = mg_http_listen(&mgr, LISTEN_URL, ev_handler, NULL);
  if (!c) {
    fprintf(stderr, "FATAL: cannot listen on %s\n", LISTEN_URL);
    return 1;
  }
  MG_INFO(("iDRAC6 KVM relay listening on %s (HTTPS + /ws)", LISTEN_URL));
  for (;;) mg_mgr_poll(&mgr, 100);
  return 0;
}
