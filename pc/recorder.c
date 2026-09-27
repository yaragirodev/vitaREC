/*
 * vita-recorder — PC client for VITA-REC (fork of VITA2PC)
 *
 * Records the PS Vita screen (MJPEG over TCP) and game audio (PCM over
 * UDP) into a RIFF/AVI file. Single file, no dependencies.
 *
 * Protocol (see psvita/main.c):
 *   1. PC opens a TCP listener on <tcpport> (default 5001)
 *   2. PC sends UDP datagram "V2R;<tcpport>" to <vita>:5000
 *   3. Vita replies UDP "<width>;<height>;<hwaccel>;1"
 *   4. Vita TCP-connects to PC:<tcpport>, sends 24-byte header:
 *        'V2R1' | u32 version | u32 width | u32 height | u32 hw | u32 reserved
 *   5. Video frames: u32le size | u64le ts_us | <jpeg bytes>
 *   6. Audio: UDP <vita>:4000+ch (ch 0..7)
 *      - PC sends "request" until Vita replies 12 bytes
 *        struct { int32 len; int32 samplerate; int32 mode; }
 *      - PC sends one more "request" (consumed by the Vita side)
 *      - then raw S16LE interleaved PCM chunks of `len` bytes
 *      - channel closed when a datagram < 512 bytes arrives ("end")
 *
 * Output: AVI (MJPG video + PCM mono 48 kHz audio track).
 * Re-encode to MP4:  ffmpeg -i out.avi -c:v libx264 -crf 18 -c:a aac out.mp4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK(s) closesocket(s)
  #define sock_errno() WSAGetLastError()
  static void sock_sleep_ms(int ms){ Sleep(ms); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <signal.h>
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
  #define CLOSESOCK(s) close(s)
  #define sock_errno() errno
  #define sock_sleep_ms(ms) do { struct timespec ts = {(ms)/1000, (long)((ms)%1000)*1000000L}; nanosleep(&ts, NULL); } while(0)
#endif

#define VIDEO_PORT     5000
#define AUDIO_PORT     4000
#define DEF_TCP_PORT   5001
#define AUDIO_CHANNELS 8
#define OUT_RATE       48000   /* mixer output rate (Hz) */
#define AUDIO_CHUNK    1920    /* 20 ms of mono s16 = 960 samples */
#define MAX_FRAME      (16*1024*1024)

static volatile int g_stop = 0;

static void onSignal(int s){ (void)s; g_stop = 1; }
#ifdef _WIN32
static BOOL WINAPI onCtrl(DWORD c){ (void)c; g_stop = 1; return TRUE; }
#endif

/* ---------------- AVI writer ---------------- */

typedef struct {
    FILE* f;
    uint8_t* rbuf;              /* TCP reassembly buffer */
    uint32_t rlen;
    /* video queue */
    struct VFrame { uint64_t pts_ms; uint8_t* data; uint32_t size; } *vf;
    uint32_t vf_n, vf_cap;
    /* audio queue (ready 20 ms chunks) */
    struct AChunk { uint64_t pts_ms; uint8_t data[AUDIO_CHUNK]; } *ac;
    uint32_t ac_n, ac_cap;
    /* index */
    struct IdxEntry { uint32_t id, flags, offset, size; } *idx;
    uint32_t idx_n, idx_cap;
    /* header offsets for final patching */
    long f_riff_size_off, f_movi_size_off, f_avih_us_off, f_avih_frames_off, f_aud_total_off;
    long f_vstrh_rate_off, f_vstrh_len_off;
    uint64_t movi_end;
    /* stats */
    uint64_t frame_count, audio_chunks, vbytes;
    int32_t vdelays[512]; int vdelays_n; uint64_t prev_pts;
    uint64_t base_ts;
} AV;

static void put_cc(FILE* f, const char* c){ fwrite(c, 4, 1, f); }
static void put_le32(FILE* f, uint32_t v){ uint32_t b = v; fwrite(&b, 4, 1, f); }
static void put_le16(FILE* f, uint16_t v){ uint16_t b = v; fwrite(&b, 2, 1, f); }

static void avInit(AV* av, const char* path, int w, int h){
    memset(av, 0, sizeof(*av));
    av->f = fopen(path, "wb");
    if (!av->f){ fprintf(stderr, "cannot open %s\n", path); exit(1); }
    av->rbuf = malloc(MAX_FRAME + 64);

    put_cc(av->f, "RIFF");
    av->f_riff_size_off = ftell(av->f);
    put_le32(av->f, 0);
    put_cc(av->f, "AVI ");

    /* ---- LIST hdrl ---- */
    long hdrl_list_start = ftell(av->f);
    long hdrl_size_off;
    put_cc(av->f, "LIST");
    hdrl_size_off = ftell(av->f);
    put_le32(av->f, 0);
    put_cc(av->f, "hdrl");

    /* avih (52-byte AVI 1.0 header) */
    put_cc(av->f, "avih");
    put_le32(av->f, 52);
    av->f_avih_us_off = ftell(av->f);
    put_le32(av->f, 33333);          /* us/frame, patched at close */
    put_le32(av->f, 8000000);        /* max bytes/sec (estimate) */
    put_le32(av->f, 0);              /* padding */
    put_le32(av->f, 0x10);           /* AVIF_HASINDEX */
    av->f_avih_frames_off = ftell(av->f);
    put_le32(av->f, 0);              /* total frames, patched */
    put_le32(av->f, 0);              /* initial frames */
    put_le32(av->f, 64*1024);        /* suggested buffer size */
    put_le32(av->f, (uint32_t)w);
    put_le32(av->f, (uint32_t)h);
    put_le32(av->f, 0); put_le32(av->f, 0); put_le32(av->f, 0); put_le32(av->f, 0);

    /* strl: video (strl + strh[8+56] + strf[8+40]) */
    put_cc(av->f, "LIST");
    put_le32(av->f, 4 + (8 + 56) + (8 + 40));
    put_cc(av->f, "strl");
    /* strh (56 bytes) — layout exactly as ffmpeg's avienc writes it:
       fccType fccHandler flags priority(2) language(2) initFrames
       scale rate start length bufsize quality sampleSize
       rcFrame.left(4) rcFrame.w(2) rcFrame.h(2) */
    put_cc(av->f, "strh");
    put_le32(av->f, 56);
    put_cc(av->f, "vids");
    put_cc(av->f, "MJPG");            /* fccHandler = codec tag */
    put_le32(av->f, 0);               /* flags   */
    put_le16(av->f, 0);               /* priority */
    put_le16(av->f, 0);               /* language */
    put_le32(av->f, 0);               /* initial frames */
    put_le32(av->f, 1);               /* scale   */
    av->f_vstrh_rate_off = ftell(av->f);
    put_le32(av->f, 30);              /* rate (fps), patched at close */
    put_le32(av->f, 0);               /* start */
    av->f_vstrh_len_off = ftell(av->f);
    put_le32(av->f, 0);               /* length (frames), patched at close */
    put_le32(av->f, 1 << 20);         /* suggested buffer size */
    put_le32(av->f, 0xFFFFFFFF);      /* quality */
    put_le32(av->f, 0);               /* sample size */
    put_le32(av->f, 0);               /* rcFrame.left */
    put_le16(av->f, (uint16_t)w);     /* rcFrame.w */
    put_le16(av->f, (uint16_t)h);     /* rcFrame.h */
    put_cc(av->f, "strf");
    put_le32(av->f, 40);
    put_le32(av->f, 40);              /* biSize */
    int32_t iw = w, ih = h, z = 0;
    fwrite(&iw, 4, 1, av->f); fwrite(&ih, 4, 1, av->f);
    put_le16(av->f, 1);
    put_le16(av->f, 24);
    put_cc(av->f, "MJPG");
    put_le32(av->f, (uint32_t)((long)w*h*3/2));
    fwrite(&z, 4, 1, av->f); fwrite(&z, 4, 1, av->f);
    put_le32(av->f, 0); put_le32(av->f, 0);

    /* strl: audio (strl + strh[8+56] + strf[8+16]) */
    put_cc(av->f, "LIST");
    put_le32(av->f, 4 + (8 + 56) + (8 + 16));
    put_cc(av->f, "strl");
    put_cc(av->f, "strh");
    put_le32(av->f, 56);
    put_cc(av->f, "auds");
    put_le32(av->f, 1);               /* fccHandler (audio) */
    put_le32(av->f, 0);               /* flags   */
    put_le16(av->f, 0);               /* priority */
    put_le16(av->f, 0);               /* language */
    put_le32(av->f, 0);               /* initial frames */
    put_le32(av->f, 1);               /* scale   */
    put_le32(av->f, OUT_RATE);        /* rate = sample rate */
    put_le32(av->f, 0);               /* start */
    av->f_aud_total_off = ftell(av->f);
    put_le32(av->f, 0);               /* length (samples), patched */
    put_le32(av->f, 12 * 1024);       /* suggested buffer size */
    put_le32(av->f, 0xFFFFFFFF);      /* quality */
    put_le32(av->f, 2);               /* sample size */
    put_le32(av->f, 0);               /* rcFrame.left */
    put_le16(av->f, 0);
    put_le16(av->f, 0);
    put_cc(av->f, "strf");
    put_le32(av->f, 16);
    put_le16(av->f, 1);               /* PCM */
    put_le16(av->f, 1);               /* mono */
    put_le32(av->f, OUT_RATE);
    put_le32(av->f, OUT_RATE*2);
    put_le16(av->f, 2);
    put_le16(av->f, 16);
    /* 16-byte WAVEFORMATEX (no cbSize), as ffmpeg writes it */

    long hdrl_end = ftell(av->f);
    fseek(av->f, hdrl_size_off, SEEK_SET);
    put_le32(av->f, (uint32_t)(hdrl_end - hdrl_list_start - 8));
    fseek(av->f, hdrl_end, SEEK_SET);

    /* ---- LIST movi ---- */
    put_cc(av->f, "LIST");
    av->f_movi_size_off = ftell(av->f);
    put_le32(av->f, 0);
    put_cc(av->f, "movi");
}

static void idxPush(AV* av, uint32_t id, uint32_t flags, uint32_t offset, uint32_t size){
    if (av->idx_n == av->idx_cap){
        av->idx_cap = av->idx_cap ? av->idx_cap*2 : 256;
        av->idx = realloc(av->idx, av->idx_cap*sizeof(*av->idx));
    }
    av->idx[av->idx_n].id = id;
    av->idx[av->idx_n].flags = flags;
    av->idx[av->idx_n].offset = offset;
    av->idx[av->idx_n].size = size;
    av->idx_n++;
}

static void writeVideoChunk(AV* av, uint8_t* data, uint32_t size){
    long offset = ftell(av->f);
    put_cc(av->f, "00dc");
    put_le32(av->f, size);
    fwrite(data, 1, size, av->f);
    if (size & 1) fputc(0, av->f); /* align next chunk to word boundary */
    idxPush(av, *(uint32_t*)"00dc", 0x80000000, (uint32_t)offset, size);
    av->frame_count++;
    av->vbytes += size;
}

static void writeAudioChunk(AV* av, const uint8_t* data){
    long offset = ftell(av->f);
    put_cc(av->f, "00wb");
    put_le32(av->f, AUDIO_CHUNK);
    fwrite(data, 1, AUDIO_CHUNK, av->f);
    idxPush(av, *(uint32_t*)"00wb", 0x10000000, (uint32_t)offset, AUDIO_CHUNK);
    av->audio_chunks++;
}

static void avClose(AV* av){
    if (!av->f) return;
    av->movi_end = ftell(av->f);

    put_cc(av->f, "idx1");
    put_le32(av->f, av->idx_n*16);
    for (uint32_t i = 0; i < av->idx_n; i++){
        put_le32(av->f, av->idx[i].id);
        put_le32(av->f, av->idx[i].flags);
        put_le32(av->f, av->idx[i].offset);
        put_le32(av->f, av->idx[i].size);
    }

    /* median frame delay over the last 512 deltas -> us/frame */
    uint32_t us = 33333;
    if (av->vdelays_n >= 2){
        int32_t tmp[512];
        int n = av->vdelays_n;
        memcpy(tmp, av->vdelays, sizeof(int32_t)*n);
        for (int i = 1; i < n; i++){
            int32_t v = tmp[i]; int j = i-1;
            while (j >= 0 && tmp[j] > v){ tmp[j+1] = tmp[j]; j--; }
            tmp[j+1] = v;
        }
        us = (uint32_t)tmp[n/2];
        if (us < 1000 || us > 1000000) us = 33333;
    }

    uint32_t riff_size  = (uint32_t)(ftell(av->f) - 8);
    uint32_t movi_size  = (uint32_t)(av->movi_end - av->f_movi_size_off - 4);
    uint32_t total_samples = (uint32_t)(av->audio_chunks * (AUDIO_CHUNK/2));

    fseek(av->f, av->f_riff_size_off, SEEK_SET);   put_le32(av->f, riff_size);
    fseek(av->f, av->f_movi_size_off, SEEK_SET);   put_le32(av->f, movi_size);
    fseek(av->f, av->f_avih_us_off, SEEK_SET);     put_le32(av->f, us);
    fseek(av->f, av->f_avih_frames_off, SEEK_SET); put_le32(av->f, (uint32_t)av->frame_count);
    fseek(av->f, av->f_aud_total_off, SEEK_SET);   put_le32(av->f, total_samples);

    fclose(av->f);
    free(av->rbuf);
}

/* ---------------- sockets ---------------- */

static void makeNonBlock(sock_t s){
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int sockWait(sock_t s, int us){
    fd_set rf; struct timeval tv;
    FD_ZERO(&rf); FD_SET(s, &rf);
    tv.tv_sec = us / 1000000; tv.tv_usec = us % 1000000;
    return select((int)s + 1, &rf, NULL, NULL, &tv);
}

/* read exactly n bytes; 0 on timeout/error */
static int sockRecvAll(sock_t s, void* buf, int n, int timeout_us){
    uint8_t* p = buf;
    while (n > 0){
        if (sockWait(s, timeout_us) <= 0) return 0;
        int r = (int)recv(s, p, n, 0);
        if (r <= 0) return 0;
        p += r; n -= r;
        timeout_us = 5000000;
    }
    return 1;
}

/* ---------------- audio channels ---------------- */
/* Each channel buffer stores MONO s16LE (downmixed from the port stream) */

typedef struct {
    sock_t skt;
    int state;      /* 0 = wait port info, 1 = active */
    int len, rate, mode;
    uint8_t* buf;   /* mono s16 */
    uint32_t n;     /* samples */
    uint32_t cap;
    double pos;     /* resampler position in samples */
    int ever_active;
} ACh;

static ACh ach[AUDIO_CHANNELS];
static time_t last_audio_req[AUDIO_CHANNELS];

static void chReset(ACh* c){
    c->state = 0;
    c->n = 0;
    c->pos = 0;
}

static void chPushMono(ACh* c, const int16_t* samples, uint32_t n){
    if (c->n + n + 8 > c->cap){
        c->cap = (c->n + n) * 2;
        if (c->cap < (1u<<20)) c->cap = 1u<<20;
        c->buf = realloc(c->buf, c->cap*2);
    }
    memcpy(c->buf + (size_t)c->n*2, samples, n*2);
    c->n += n;
}

static void chConsume(ACh* c, uint32_t samples){
    if (samples > c->n) samples = c->n;
    memmove(c->buf, c->buf + (size_t)samples*2, (size_t)(c->n - samples)*2);
    c->n -= samples;
    c->pos -= (double)samples;
}

static int anyAudioActive(void){
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++)
        if (ach[ch].state == 1) return 1;
    return 0;
}

/*
 * Mix all active channels into mono s16 at OUT_RATE.
 * Produces up to max_samples output samples (returns count).
 */
static int mixPump(int16_t* out, int max_samples){
    int written = 0;
    while (written < max_samples){
        double sum = 0.0;
        int contributors = 0;
        for (int ch = 0; ch < AUDIO_CHANNELS; ch++){
            ACh* c = &ach[ch];
            if (c->state != 1 || c->n < 4) continue;
            double step = (double)c->rate / (double)OUT_RATE;
            while (c->pos + step <= (double)c->n - 1.0){
                uint32_t i = (uint32_t)c->pos;
                double f = c->pos - (double)i;
                double v0 = (double)((int16_t)(c->buf[i*2] | (c->buf[i*2+1] << 8)));
                double v1 = (double)((int16_t)(c->buf[(i+1)*2] | (c->buf[(i+1)*2+1] << 8)));
                sum += v0*(1.0 - f) + v1*f;
                c->pos += step;
                contributors++;
            }
            if (c->pos > 65536.0) chConsume(c, (uint32_t)c->pos & ~1u);
        }
        if (contributors == 0){
            /* no data right now: fill with silence so audio stays in sync,
               but only when some channel is (or was) active */
            if (!anyAudioActive()) return written;
            out[written++] = 0;
            continue;
        }
        double v = sum / (double)contributors;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[written++] = (int16_t)(v + (v >= 0 ? 0.5 : -0.5));
    }
    return written;
}

/* ---------------- main ---------------- */

/* next output file name: session 1 -> base, later -> base_2, base_3 ... */
static void nextOutName(char* dst, int dstsz, const char* base, int session){
    if (session == 1){
        snprintf(dst, dstsz, "%s", base);
        return;
    }
    const char* dot = strrchr(base, '.');
    if (dot && dot > base){
        snprintf(dst, dstsz, "%.*s_%d%s", (int)(dot - base), base, session, dot);
    }else{
        snprintf(dst, dstsz, "%s_%d", base, session);
    }
}

int main(int argc, char** argv){
    const char* vita_ip = NULL;
    const char* out = NULL;
    int tcp_port = DEF_TCP_PORT;
    int video_only = 0;

    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--vita") && i+1 < argc) vita_ip = argv[++i];
        else if (!strcmp(argv[i], "--out") && i+1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--tcp-port") && i+1 < argc) tcp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--video-only")) video_only = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")){
            printf("usage: vita-recorder [--vita IP] [--out FILE] [--tcp-port N] [--video-only]\n");
            return 0;
        } else if (!vita_ip) vita_ip = argv[i];
        else { fprintf(stderr, "unexpected arg: %s\n", argv[i]); return 1; }
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    SetConsoleCtrlHandler(onCtrl, TRUE);
#else
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#endif

    if (!vita_ip){
        char tmp[64];
        printf("Enter Vita IP: ");
        fflush(stdout);
        if (!fgets(tmp, sizeof(tmp), stdin)){ fprintf(stderr, "no input\n"); return 1; }
        tmp[strcspn(tmp, "\r\n")] = 0;
        vita_ip = strdup(tmp);
    }

    if (!out){
        char buf[128];
        time_t t = time(NULL);
        struct tm* lt = localtime(&t);
        strftime(buf, sizeof(buf), "vita_rec_%Y%m%d_%H%M%S.avi", lt);
        out = buf;
    }

    printf("=== VITA-REC recorder ===\n");
    printf("Vita IP:    %s\n", vita_ip);
    printf("Output:     %s\n", out);
    printf("TCP port:   %d (listening)\n\n", tcp_port);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, vita_ip, &addr.sin_addr) != 1){
        fprintf(stderr, "invalid IP\n");
        return 1;
    }

    /* 1. TCP listener */
    sock_t lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lsock == INVALID_SOCKET){ fprintf(stderr, "socket failed\n"); return 1; }
    int on = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = 0;
    laddr.sin_port = htons((uint16_t)tcp_port);
    if (bind(lsock, (struct sockaddr*)&laddr, sizeof(laddr)) != 0){
        fprintf(stderr, "cannot bind port %d (already running?)\n", tcp_port);
        return 1;
    }
    listen(lsock, 1);

    /* 2. UDP hello socket (hello is re-sent every 2 s until the Vita connects,
          so it does not matter which side you start first) */
    sock_t usock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (usock == INVALID_SOCKET){ fprintf(stderr, "socket failed\n"); return 1; }
    addr.sin_port = htons(VIDEO_PORT);
    char hello[32];
    int hlen = snprintf(hello, sizeof(hello), "V2R;%d", tcp_port);

    int session = 0;
    for (int ch0 = 0; ch0 < AUDIO_CHANNELS; ch0++) ach[ch0].skt = INVALID_SOCKET;
    for (;;){
        if (g_stop) break;
        session++;
        addr.sin_port = htons(VIDEO_PORT);
        char cur_out[512];
        nextOutName(cur_out, sizeof(cur_out), out, session);
        printf("\n--- Session %d: waiting for Vita (on the Vita: L+Select -> \"Start Recording\") ---\n", session);

        /* wait for reply + TCP accept (no timeout; Ctrl+C to cancel) */
        char reply[64] = {0};
        int got_reply = 0, got_conn = 0;
        sock_t tcp = INVALID_SOCKET;
        time_t last_hello = 0;
        while (!g_stop && !(got_reply && got_conn)){
            fd_set rf; FD_ZERO(&rf);
            int maxfd = (int)(usock > lsock ? usock : lsock);
            FD_SET(usock, &rf); FD_SET(lsock, &rf);
            struct timeval tv = {1, 0};
            int r = select(maxfd+1, &rf, NULL, NULL, &tv);
            if (r > 0){
                if (FD_ISSET(usock, &rf) && !got_reply){
                    int n = (int)recv(usock, reply, (int)sizeof(reply)-1, 0);
                    if (n > 0){ reply[n] = 0; got_reply = 1; }
                }
                if (FD_ISSET(lsock, &rf)){
                    tcp = accept(lsock, NULL, NULL);
                    if (tcp != INVALID_SOCKET) got_conn = 1;
                }
            }
            if (time(NULL) - last_hello >= 2){
                sendto(usock, hello, hlen, 0, (struct sockaddr*)&addr, sizeof(addr));
                last_hello = time(NULL);
            }
        }
        if (g_stop) break;
        printf("Connected. Vita said: \"%s\"\n", reply);

        int w = 0, h = 0, hw = 0, proto = 0;
        sscanf(reply, "%d;%d;%d;%d", &w, &h, &hw, &proto);

        /* 3. TCP header */
        makeNonBlock(tcp);
        uint8_t hdr[24];
        if (!sockRecvAll(tcp, hdr, 24, 10000000)){
            fprintf(stderr, "timeout reading stream header\n");
            CLOSESOCK(tcp);
            continue;
        }
        if (memcmp(hdr, "V2R1", 4) != 0){
            fprintf(stderr, "bad magic (got %c%c%c%c)\n", hdr[0], hdr[1], hdr[2], hdr[3]);
            CLOSESOCK(tcp);
            continue;
        }
        uint32_t sw = (uint32_t)hdr[8] | (uint32_t)hdr[9]<<8 | (uint32_t)hdr[10]<<16 | (uint32_t)hdr[11]<<24;
        uint32_t sh = (uint32_t)hdr[12] | (uint32_t)hdr[13]<<8 | (uint32_t)hdr[14]<<16 | (uint32_t)hdr[15]<<24;
        if (sw && sh){ w = (int)sw; h = (int)sh; }
        printf("Stream: %dx%d, %s MJPEG encoder -> %s\n", w, h, hw ? "hardware" : "software", cur_out);

        /* 4. audio channels (reset per session) */
        if (!video_only){
            for (int ch = 0; ch < AUDIO_CHANNELS; ch++){
                ACh* c = &ach[ch];
                chReset(c);
                if (c->skt < 0)
                    c->skt = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (c->skt < 0) continue;
                makeNonBlock(c->skt);
                addr.sin_port = htons((uint16_t)(AUDIO_PORT + ch));
                int cret = connect(c->skt, (struct sockaddr*)&addr, sizeof(addr));
                int sret = send(c->skt, "request", 8, 0);
                if (getenv("VRA_DEBUG"))
                    fprintf(stderr, "[dbg] audio ch%d skt=%d connect=%d send=%d\n", ch, (int)c->skt, cret, sret);
                last_audio_req[ch] = time(NULL);
            }
            printf("Audio: watching 8 UDP channels (%d..%d)\n\n", AUDIO_PORT, AUDIO_PORT + AUDIO_CHANNELS - 1);
        }

        AV av;
        avInit(&av, cur_out, w ? w : 480, h ? h : 272);

        time_t last_stats = time(NULL);
        uint64_t last_frames = 0;
        time_t stream_t0 = time(NULL);

        while (!g_stop){
            fd_set rf; FD_ZERO(&rf);
            int maxfd = (int)tcp;
            FD_SET(tcp, &rf);
            if (!video_only){
                for (int ch = 0; ch < AUDIO_CHANNELS; ch++){
                    if (ach[ch].skt != INVALID_SOCKET){
                        FD_SET(ach[ch].skt, &rf);
                        if ((int)ach[ch].skt > maxfd) maxfd = (int)ach[ch].skt;
                    }
                }
            }
            struct timeval tv = {0, 30000};
            int r = select(maxfd+1, &rf, NULL, NULL, &tv);
            if (r < 0){
#ifdef _WIN32
                break;
#else
                if (errno != EINTR) break;
#endif
            }

            int tcp_dead = 0;

            /* --- TCP video --- */
            if (FD_ISSET(tcp, &rf)){
                int space = (int)(MAX_FRAME + 64 - av.rlen);
                for (int guard = 0; space > 0 && guard < 8; guard++){
                    int n = (int)recv(tcp, av.rbuf + av.rlen, space, 0);
                    if (n > 0){ av.rlen += (uint32_t)n; space -= n; continue; }
                    if (n == 0){ tcp_dead = 1; break; }
                    int e = sock_errno();
#ifdef _WIN32
                    if (e == WSAEWOULDBLOCK) break;
#else
                    if (e == EAGAIN || e == EWOULDBLOCK) break;
                    if (e == ECONNRESET) tcp_dead = 1;
#endif
                    break;
                }
            }
            if (tcp_dead){ printf("\nVita stopped the stream.\n"); break; }

            /* parse frames from rbuf */
            for (;;){
                if (av.rlen < 12) break;
                uint32_t fsize = (uint32_t)av.rbuf[0] | (uint32_t)av.rbuf[1]<<8 |
                                 (uint32_t)av.rbuf[2]<<16 | (uint32_t)av.rbuf[3]<<24;
                if (fsize > MAX_FRAME || fsize == 0){
                    printf("\nbad frame size %u, resync\n", fsize);
                    av.rlen = 0;
                    break;
                }
                if (av.rlen < 12 + fsize) break;
                uint64_t ts = 0;
                for (int i = 0; i < 8; i++) ts |= (uint64_t)av.rbuf[4+i] << (8*i);

                uint64_t pts_ms;
                if (av.frame_count == 0 && av.vf_n == 0){ av.base_ts = ts; pts_ms = 0; }
                else {
                    uint64_t base = av.base_ts ? av.base_ts : ts;
                    pts_ms = (ts > base ? (ts - base) : 0) / 1000;
                }

                uint8_t* data = malloc(fsize);
                memcpy(data, av.rbuf + 12, fsize);
                memmove(av.rbuf, av.rbuf + 12 + fsize, av.rlen - 12 - fsize);
                av.rlen -= 12 + fsize;

                if (av.vf_n == av.vf_cap){
                    av.vf_cap = av.vf_cap ? av.vf_cap*2 : 16;
                    av.vf = realloc(av.vf, av.vf_cap*sizeof(*av.vf));
                }
                av.vf[av.vf_n].pts_ms = pts_ms;
                av.vf[av.vf_n].data = data;
                av.vf[av.vf_n].size = fsize;
                av.vf_n++;
            }

            /* --- audio --- */
            if (!video_only){
                time_t now = time(NULL);
                for (int ch = 0; ch < AUDIO_CHANNELS; ch++){
                    ACh* c = &ach[ch];
                    if (c->skt == INVALID_SOCKET) continue;
                    if (c->state == 0 && now - last_audio_req[ch] >= 1){
                        send(c->skt, "request", 8, 0);
                        last_audio_req[ch] = now;
                    }
                    if (!FD_ISSET(c->skt, &rf)) continue;
                    uint8_t dbuf[1<<16];
                    int n = (int)recv(c->skt, dbuf, sizeof(dbuf), 0);
                    if (n <= 0) continue;
                    if (c->state == 0){
                        if (n == 12){
                            int32_t len, rate, mode;
                            memcpy(&len,  dbuf+0, 4);
                            memcpy(&rate, dbuf+4, 4);
                            memcpy(&mode, dbuf+8, 4);
                            if (rate >= 8000 && rate <= 192000 && len >= 128 && len <= (1<<20)){
                                c->len = len; c->rate = rate; c->mode = mode;
                                c->state = 1;
                                c->ever_active = 1;
                                send(c->skt, "request", 8, 0); /* 2nd request, consumed by Vita */
                                printf("\n[audio] channel %d active: %d Hz, %s, chunk %d\n",
                                       ch, rate, mode == 0 ? "mono" : "stereo", len);
                            }
                        }
                    }else{
                        if (n < 512){
                            printf("\n[audio] channel %d closed by Vita\n", ch);
                            chReset(c);
                            continue;
                        }
                        /* downmix interleaved s16 -> mono */
                        int chn = c->mode == 0 ? 1 : 2;
                        int frames = n / (2*chn);
                        int16_t mono[4096];
                        int mi = 0;
                        for (int i = 0; i < frames && mi < (int)sizeof(mono)/2; i++){
                            long acc = 0;
                            for (int k = 0; k < chn; k++) acc += (int16_t)(dbuf[i*2*chn + k*2] | (dbuf[i*2*chn + k*2+1] << 8));
                            long v = acc / chn;
                            if (v > 32767) v = 32767;
                            if (v < -32768) v = -32768;
                            mono[mi++] = (int16_t)v;
                        }
                        chPushMono(c, mono, (uint32_t)mi);
                    }
                }

                /* pump mixer: produce at most as many samples as are actually
                   accumulated in the channel buffers, so audio stays real-time */
                if (anyAudioActive()){
                    static int16_t mixbuf[8192];
                    double avail = 1e18;
                    int act = 0;
                    for (int ch2 = 0; ch2 < AUDIO_CHANNELS; ch2++){
                        ACh* c2 = &ach[ch2];
                        if (c2->state != 1) continue;
                        double step2 = (double)c2->rate / (double)OUT_RATE;
                        double a = ((double)c2->n - c2->pos) / step2;
                        if (a < avail) avail = a;
                        act++;
                    }
                    if (act){
                        int want = (int)avail;
                        if (want > 4096) want = 4096;      /* <= 85 ms per pump */
                        int got = want >= 16 ? mixPump(mixbuf, want) : 0;
                        while (got >= AUDIO_CHUNK/2){
                            if (av.ac_n == av.ac_cap){
                                av.ac_cap = av.ac_cap ? av.ac_cap*2 : 16;
                                av.ac = realloc(av.ac, av.ac_cap*sizeof(*av.ac));
                            }
                            uint64_t total = (uint64_t)av.ac_n * (AUDIO_CHUNK/2);
                            av.ac[av.ac_n].pts_ms = total * 1000 / OUT_RATE;
                            memcpy(av.ac[av.ac_n].data, mixbuf, AUDIO_CHUNK);
                            av.ac_n++;
                            memmove(mixbuf, mixbuf + AUDIO_CHUNK/2, (size_t)(got - AUDIO_CHUNK/2)*2);
                            got -= AUDIO_CHUNK/2;
                        }
                    }
                }
            }

            /* --- interleave & write (pts order) --- */
            for (;;){
                if (av.vf_n == 0 && av.ac_n == 0) break;
                if (av.vf_n > 0 && (av.ac_n == 0 || av.vf[0].pts_ms <= av.ac[0].pts_ms)){
                    uint64_t pts = av.vf[0].pts_ms;
                    if (av.prev_pts && pts > av.prev_pts){
                        int32_t d = (int32_t)(pts - av.prev_pts);
                        if (d > 0 && d < 2000)
                            av.vdelays[av.vdelays_n % 512] = d, av.vdelays_n++;
                    }
                    av.prev_pts = pts;
                    writeVideoChunk(&av, av.vf[0].data, av.vf[0].size);
                    free(av.vf[0].data);
                    memmove(av.vf, av.vf+1, (size_t)(av.vf_n-1)*sizeof(*av.vf));
                    av.vf_n--;
                }else{
                    writeAudioChunk(&av, av.ac[0].data);
                    memmove(av.ac, av.ac+1, (size_t)(av.ac_n-1)*sizeof(*av.ac));
                    av.ac_n--;
                }
            }

            /* stats */
            time_t now = time(NULL);
            if (now - last_stats >= 1){
                double dt = (double)(now - last_stats);
                uint64_t df = av.frame_count - last_frames;
                last_stats = now;
                last_frames = av.frame_count;
                printf("\r  %5llu frames | %4.1f fps | %6.1f MB file | %s    ",
                       (unsigned long long)av.frame_count,
                       (double)df/dt,
                       (double)av.vbytes/(1024.0*1024.0),
                       video_only ? "video only" : (anyAudioActive() ? "audio OK" : "audio: waiting..."));
                fflush(stdout);
            }
        }

        printf("\nFinalizing %s ...\n", cur_out);
        avClose(&av);
        double dur = (double)(time(NULL) - stream_t0);
        printf("Done: %llu frames, %.1f MB video, %llu audio chunks, ~%.0f s of stream.\n",
               (unsigned long long)av.frame_count, (double)av.vbytes/(1024.0*1024.0),
               (unsigned long long)av.audio_chunks, dur);
        printf("File: %s\nRe-encode to MP4:  ffmpeg -i \"%s\" -c:v libx264 -crf 18 -c:a aac out.mp4\n", cur_out, cur_out);

        if (tcp != INVALID_SOCKET) CLOSESOCK(tcp);
        for (int i = 0; i < (int)av.vf_n; i++) free(av.vf[i].data);
        free(av.vf); free(av.ac); free(av.idx);

        if (g_stop) break;
        printf("Stopped. Waiting for the next recording (on the Vita: L+Select -> \"Start Recording\")...\n");
    }

    CLOSESOCK(lsock);
    CLOSESOCK(usock);
    for (int ch = 0; ch < AUDIO_CHANNELS; ch++){
        if (ach[ch].skt != INVALID_SOCKET) CLOSESOCK(ach[ch].skt);
        free(ach[ch].buf);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
