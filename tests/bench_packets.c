#define _GNU_SOURCE
// SPDX-License-Identifier: BSD-3-Clause
#include <modem.h>
#include <datapump_link.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "packet_counter.h"
static void measured_process(struct modem *, enum g711_law, const u8 *, void *,
    const bool *, u8 *, void *, int, bool *);
#define modem_process_g711 measured_process
#define main integration_test_main
#include "test_fsk_integration.c"
#undef main
#undef modem_process_g711

struct bench_endpoint {
    struct integration_endpoint media; /* first member owns modem->dev_data */
    struct packet_stats ticks[2], cycles[2];
    int phase, cycle_fd;
    unsigned initial_tx, initial_rx;
    const char *worst_step[2];
};
static void measured_process(struct modem *m, enum g711_law law,
    const u8 *rx, void *rx_samples, const bool *mask, u8 *tx,
    void *tx_samples, int count, bool *exact)
{
    struct bench_endpoint *e = m->dev_data;
    uint64_t cycles_before = 0, cycles_after = 0;
    if (e->cycle_fd >= 0 && read(e->cycle_fd, &cycles_before, 8) != 8) abort();
    const char *step = m->dp && m->dp->op->progress ? m->dp->op->progress(m->dp) : "unknown";
    uint64_t before = packet_ticks();
    modem_process_g711(m, law, rx, rx_samples, mask, tx, tx_samples, count, exact);
    uint64_t elapsed = packet_ticks() - before;
    if (e->cycle_fd >= 0 && read(e->cycle_fd, &cycles_after, 8) != 8) abort();
    if (e->phase >= 0) {
        if (count != SIP_FRAME_SAMPLES) abort();
        if (elapsed > e->ticks[e->phase].maximum) e->worst_step[e->phase] = step;
        packet_stats_add(&e->ticks[e->phase], elapsed);
        if (e->cycle_fd >= 0) packet_stats_add(&e->cycles[e->phase], cycles_after - cycles_before);
    }
}
struct bench_profile { const char *name, *selection; enum DP_ID id; bool pcm, baudot; };
static const struct bench_profile profiles[] = {
    {"bell103", "AT+MS=103,0,300,300\r", DP_B103, false, false},
    {"v21", "AT+MS=21,0,300,300\r", DP_V21, false, false},
    {"v23", "AT+MS=23,0,75,1200\r", DP_V23, false, false},
    {"bell212", "AT+MS=212,0,1200,1200\r", DP_B212, false, false},
    {"v22-600", "AT+MS=22,0,600,600\r", DP_V22, false, false},
    {"v22-1200", "AT+MS=22,0,1200,1200\r", DP_V22, false, false},
    {"v22bis", "AT+MS=122,0,2400,2400\r", DP_V22BIS, false, false},
    {"baudot45", "AT+MS=45,0,45,45\r", DP_BAUDOT, false, true},
    {"baudot50", "AT+MS=45,0,50,50\r", DP_BAUDOT, false, true},
    {"v32-4800", "AT+MS=32,1,4800,4800\r", DP_V32, false, false},
    {"v32-9600", "AT+MS=32,1,9600,9600\r", DP_V32, false, false},
    {"v32bis-7200", "AT+MS=132,1,7200,7200\r", DP_V32BIS, false, false},
    {"v32bis-12000", "AT+MS=132,1,12000,12000\r", DP_V32BIS, false, false},
    {"v32bis", "AT+MS=132,1,14400,14400\r", DP_V32BIS, false, false},
    {"v34-28800", "AT+MS=34,0,28800,28800+V34B=3200\r", DP_V34, false, false},
    {"v34-33600", "AT+MS=34,0,33600,33600+V34B=3429\r", DP_V34, false, false},
    {"v90", "AT+MS=90,0,56000,56000+V90U=33600,33600,3429\r", DP_V90, true, false},
    {"v92-v34", "AT+MS=92,0,56000,56000+V90U=33600,33600,3429+V92U=0\r", DP_V92, true, false},
    {"v92-pcm40", "AT+MS=92,0,52000,52000+V92U=1,40000\r", DP_V92, true, false},
    {"v92-pcm48", "AT+MS=92,0,56000,56000+V92U=1,48000\r", DP_V92, true, false},
    {"v91", "AT+MS=91,1,28000,64000\r", DP_V91, false, false},
};
static uint8_t data_byte(size_t at, unsigned direction, bool text, bool compressible) {
    if (text) return 'A' + at % 26;
    if (compressible) return 'A' + (at / 64 + direction) % 26;
    uint32_t x = (uint32_t)at + direction * UINT32_C(0x1234567);
    x ^= x >> 16; x *= UINT32_C(0x7feb352d); x ^= x >> 15; x *= UINT32_C(0x846ca68b);
    return (uint8_t)(x ^ (x >> 16));
}
static bool data_flow(struct bench_endpoint *from, struct bench_endpoint *to,
    size_t *sent, size_t *received, unsigned direction, bool text, bool compressible, bool sending) {
    if (sending && from->media.modem->xmit.count < 1024) {
        uint8_t bytes[512];
        for (size_t i=0;i<sizeof(bytes);++i) bytes[i]=data_byte(*sent+i,direction,text,compressible);
        int count=modem_write(from->media.modem,(const char *)bytes,sizeof(bytes));
        CHECK(count>=0,"payload queue failed"); *sent += count;
    }
    uint8_t chunk[512];
    for (;;) {
        ssize_t count=read(to->media.client_fd,chunk,sizeof(chunk));
        if (count<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
        if (count<0 && errno==EINTR) continue;
        CHECK(count>0,"payload drain failed");
        for (ssize_t i=0;i<count;++i) {
            CHECK(chunk[i]==data_byte(*received,direction,text,compressible),
                "payload mismatch dir=%u byte=%zu got=%u",direction,*received,chunk[i]);
            ++*received;
        }
    }
    return true;
}
static int cycle_open(void) {
    struct perf_event_attr attr = { .type=PERF_TYPE_HARDWARE, .size=sizeof(attr),
        .config=PERF_COUNT_HW_CPU_CYCLES, .exclude_kernel=1, .exclude_hv=1, .pinned=1 };
    return syscall(__NR_perf_event_open,&attr,0,-1,-1,0);
}
static void result(const struct bench_profile *profile, const struct bench_endpoint *e,
    unsigned side, unsigned phase, unsigned link, bool digital_caller,
    enum media_codec codec, double hz, size_t bytes) {
    const struct packet_stats *s=&e->ticks[phase], *c=&e->cycles[phase];
    struct dp_link_rates rates=datapump_get_link_rates(e->media.modem->dp);
    printf("{\"profile\":\"%s\",\"codec\":\"%s\",\"side\":\"%s\",\"digital_caller\":%s,"
        "\"phase\":\"%s\",\"link\":%u,\"tx_bps\":%u,\"rx_bps\":%u,\"tx_baud\":%.6f,\"rx_baud\":%.6f,\"packets\":%llu,"
        "\"sum_ticks\":%llu,\"max_ticks\":%llu,\"max3_ticks\":%llu,\"max_index\":%llu,"
        "\"max3_index\":%llu,\"counter_hz\":%.3f,\"counter\":\"%s\",\"pmu_cycles\":%s,"
        "\"sum_cycles\":%llu,\"max_cycles\":%llu,\"max3_cycles\":%llu,\"verified_bytes\":%zu,\"worst_step\":\"%s\"}\n",
        profile->name,codec_name(codec),side?"answerer":"caller",digital_caller?"true":"false",
        phase?"data":"training",link,e->media.modem->tx_rate,e->media.modem->rx_rate,rates.transmit_baud,rates.receive_baud,
        (unsigned long long)s->count,(unsigned long long)s->sum,(unsigned long long)s->maximum,
        (unsigned long long)s->max_three,(unsigned long long)s->maximum_index,(unsigned long long)s->three_index,
        hz,PACKET_COUNTER_NAME,e->cycle_fd>=0?"true":"false",(unsigned long long)c->sum,
        (unsigned long long)c->maximum,(unsigned long long)c->max_three,bytes,e->worst_step[phase]?e->worst_step[phase]:"unknown");
}
static bool run_profile(const struct bench_profile *p, unsigned link, bool digital,
    enum media_codec codec, unsigned frames, bool compressible, double hz) {
    struct bench_endpoint ends[2]={0};
    size_t sent[2]={0},received[2]={0};
    int cycles=cycle_open(); bool online=false;
    if (cycles<0) fprintf(stderr,"CPU PMU unavailable: %s; using %s\n",strerror(errno),PACKET_COUNTER_NAME);
    const char *transport=link==0?"AT\\N0+DS=0,0\r":link==1?"AT\\N4+DS=0,0\r":"AT\\N4+DS=3,2\r";
    for (unsigned i=0;i<2;++i) {
        struct bench_endpoint *e=&ends[i]; e->cycle_fd=cycles; e->phase=0;
        CHECK(endpoint_init(&e->media,i?"benchmark answerer":"benchmark caller") &&
            endpoint_prepare_media(&e->media,codec),"endpoint init failed");
        struct modem *m=e->media.modem;
        CHECK(modem_set_sreg(m,SREG_CONNNECT_MSG_FORMAT,1)==0 &&
            modem_set_sreg(m,SREG_V90_DIGITAL_INDICATION,p->pcm?(i?!digital:digital):2)==0 &&
            modem_write(m,transport,strlen(transport))==(int)strlen(transport) &&
            modem_write(m,p->selection,strlen(p->selection))==(int)strlen(p->selection) &&
            endpoint_drain(&e->media) && !output_contains(&e->media,"ERROR"),"AT profile failed");
        if (digital && !strcmp(p->name,"v90")) {
            const char *upstream="AT+V90U=31200,33600,3429\r";
            CHECK(modem_write(m,upstream,strlen(upstream))==(int)strlen(upstream) &&
                endpoint_drain(&e->media) && !output_contains(&e->media,"ERROR"),"V90 digital originator rate policy");
        }
        e->media.output_count=0;
    }
    CHECK(modem_set_sreg(ends[1].media.modem,SREG_ANS_DELAY,0)==0 &&
        modem_write(ends[0].media.modem,"ATDpeer\r",8)==8 &&
        modem_report_outgoing_call_connected(ends[0].media.modem)==0 &&
        modem_answer(ends[1].media.modem)==0,"call start failed");
    for (unsigned frame=0;frame<3000;++frame) {
        CHECK(process_sip_frame(&ends[0].media,&ends[1].media,codec,NULL) &&
            endpoint_drain(&ends[0].media) && endpoint_drain(&ends[1].media),"training failed");
        if (modem_is_online(ends[0].media.modem,p->id) && modem_is_online(ends[1].media.modem,p->id) &&
            output_contains(&ends[0].media,"CONNECT") && output_contains(&ends[1].media,"CONNECT")) {
            online=true; break;
        }
    }
    CHECK(online,"profile did not connect: %s caller=%u/%u answer=%u/%u",p->name,
        ends[0].media.modem->tx_rate,ends[0].media.modem->rx_rate,
        ends[1].media.modem->tx_rate,ends[1].media.modem->rx_rate);
    for (unsigned i=0;i<2;++i) {
        CHECK(ends[i].media.modem->cfg.ec==(link!=0),"error correction mismatch");
        if (link==2) CHECK(modem_ec_active_compression(ends[i].media.modem)==MODEM_EC_COMPRESSION_V44,"V44 not active");
        ends[i].initial_tx=ends[i].media.modem->tx_rate; ends[i].initial_rx=ends[i].media.modem->rx_rate;
        ends[i].phase=-1; ends[i].media.output_count=0;
    }
    /* Settle the transition before the sustained payload interval. */
    for (unsigned f=0;f<50;++f) {
        CHECK(process_sip_frame(&ends[0].media,&ends[1].media,codec,NULL) &&
            endpoint_drain(&ends[0].media) && endpoint_drain(&ends[1].media),"warmup failed");
        ends[0].media.output_count=ends[1].media.output_count=0;
    }
    for (unsigned i=0;i<2;++i) ends[i].phase=1;
    for (unsigned frame=0;frame<frames;++frame) {
        CHECK(data_flow(&ends[0],&ends[1],&sent[0],&received[0],0,p->baudot,compressible,true) &&
            (p->baudot || data_flow(&ends[1],&ends[0],&sent[1],&received[1],1,false,compressible,true)) &&
            process_sip_frame(&ends[0].media,&ends[1].media,codec,NULL),"data processing failed");
        for (unsigned i=0;i<2;++i) CHECK(modem_is_online(ends[i].media.modem,p->id) &&
            ends[i].media.modem->tx_rate==ends[i].initial_tx && ends[i].media.modem->rx_rate==ends[i].initial_rx,
            "connection or rate changed during payload");
    }
    CHECK(data_flow(&ends[0],&ends[1],&sent[0],&received[0],0,p->baudot,compressible,false) &&
        (p->baudot || data_flow(&ends[1],&ends[0],&sent[1],&received[1],1,false,compressible,false)),"final drain failed");
    for (unsigned i=0;i<2;++i) {
        CHECK(modem_is_online(ends[i].media.modem,p->id) && ends[i].ticks[1].count==frames,"connection changed");
        if (i==0 || !p->baudot) CHECK(received[i]>0,"no verified data");
        for (unsigned phase=0;phase<2;++phase) result(p,&ends[i],i,phase,link,digital,codec,hz,received[i?0:1]);
    }
    endpoint_destroy(&ends[1].media); endpoint_destroy(&ends[0].media);
    if (cycles>=0) close(cycles);
    return true;
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--list")) { for(size_t i=0;i<ARRAY_LENGTH(profiles);++i) puts(profiles[i].name); return 0; }
    if (argc!=7) { fprintf(stderr,"usage: %s PROFILE LINK DIGITAL_CALLER ALAW FRAMES COMPRESSIBLE\n",argv[0]); return 2; }
    unsigned values[5];
    for (unsigned i=0;i<5;++i) {
        char *end; unsigned long v=strtoul(argv[i+2],&end,10);
        if (!*argv[i+2] || *end || v>1000000 || (i!=3 && v>(i==0?2:1))) return 2;
        values[i]=v;
    }
    if (!values[3]) return 2;
    double hz=packet_counter_hz();
    for(size_t i=0;i<ARRAY_LENGTH(profiles);++i) if(!strcmp(argv[1],profiles[i].name))
        return run_profile(&profiles[i],values[0],values[1],values[2]?MEDIA_CODEC_PCMA:MEDIA_CODEC_PCMU,
            values[3],values[4],hz)?0:1;
    return 2;
}
