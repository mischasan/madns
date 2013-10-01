// Copyright (c) 2004-2005 Sergey Lyubka <valenok@gmail.com>
// "THE BEER-WARE LICENSE" (Revision 42):
//  Sergey Lyubka wrote this file.  As long as you retain this notice you
//  can do whatever you want with this stuff. If we meet some day, and you think
//  this stuff is worth it, you can buy me a beer in return.
//
// Mischa Sandberg <mischasan@gmail.com> in 2012 extended Sergey's "madns"
//  for Sophos LLC, which cheerfully releases this into the Open Source world.
//  "send_request" and "parse_response" are Sergey's with little change.
//
// MADNS:
//  - uses multiple servers, taken from resolv.conf, based on latency.
//  - exposes (fileno,timeout) interface for external polling/select
//  - keeps an (in-memory) cache

#include <ctype.h>              // tolower...
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>             // malloc...
#include <string.h>
#include <time.h>               // time...
#include <unistd.h>
#include <sys/time.h>           // gettimeofday
#include <arpa/inet.h>          // inet_ntoa inet_ntop
#include <arpa/nameser.h>       // NS_MAXLABEL QUERY ...
#undef QUERY
#include "madns.h"

#define DNS_A_RECORD         1  // aka ns_t_a
#define DNS_CNAME            5  // aka ns_t_cname
#define DNS_R_NXDOMAIN       3  // aka ns_r_nxdomain
#define DNS_MAX_HOSTNAME   255  // Max host name, per RFP
#define DNS_PACKET_LEN    2048  // Buffer size for DNS packet

#define MAX_TIDS         32767

// Generate a function that maps a ptr to a link field
//  in a struct to a ptr to the struct. S-O-O-O C++ templating.
#define CASTFIELD(TYPE,FIELD) \
static inline TYPE*FIELD##_##TYPE(QLINK *lp) \
{ return (TYPE*)((uint8_t*)lp - (uint8_t*)&((TYPE*)0)->FIELD); }

//--------------|---------------------------------------------
// Tiny doubly-linked-list queue implementation.
typedef struct qlink { struct qlink *next, *prev; } QLINK;

static inline void qinit(QLINK * q);
static inline int qempty(QLINK * q);
static inline void qpush(QLINK * q, QLINK * qel);
static inline QLINK * qpull(QLINK * q);
static int qleng(QLINK const *list, int limit);
//--------------|---------------------------------------------
typedef uint32_t HASH;
typedef struct sockaddr SADDR;
typedef struct sockaddr_in INADDR;

typedef struct {
    in_addr_t ip;
    int     nreqs;
    double  latency;            // Decaying-average response time.
} SERVER;

// Active request.
typedef struct {
    QLINK   link;               // See link_QUERY()
    void   *ctx;                // Application context
    time_t  expires;            // Time when this query expires.
    uint16_t tid;               // DNS transaction ID
    char   *name;
    SERVER *server;             // entry in MADNS.serv[]
    double  started;
} QUERY;

// Info passed from parse_response to update_cache.
typedef struct {
    in_addr_t ip;
    time_t  ttl;
    uint16_t tid;
    char const *name;
} RESPONSE;

// Cached name->ip map.
typedef struct {
    HASH    hash;
    time_t  expires;
    in_addr_t ip;               // MSB-first
    char    name[1];
} CACHE_INFO;

struct madns {
    int     query_time;         // secs till a query is expired.
    int     server_reqs;        // max reqs per server.
    int     sock;               // UDP socket used for responses.
    int     nservs;
    SERVER *serv;

    // cache is an open-addr hash table with no "delete(key)".
#   define  MIN_CACHE   16      // Must be a power of 2.
    int     limit, count;       // size and used.
    CACHE_INFO **cachev;

    int     qsize;              // nservs * server_reqs
    int     nfree;              // entries in (unused)
    QUERY  *queries;            // queries[qsize]
    QLINK   active;
    QLINK   unused;
};

// DNS response header (all ints in network order)
typedef struct {
    uint16_t tid;               // Transaction ID
    uint16_t flags;             // Flags
    uint16_t nqueries;          // Questions
    uint16_t nanswers;          // Answers
    uint16_t nauth;             // Authority PRs
    uint16_t nother;            // Other PRs
    char    data[1];            // Data, variable length
} DNS_RESP;

static void *destroy_query(MADNS *, QUERY *, in_addr_t);
static int parse_response(char *pkt, int len, RESPONSE *);
static void send_request(MADNS * mp, QUERY * qp);

CASTFIELD(QUERY, link); // =>> static inline "link_QUERY()"

//--------------|---------------------------------------------
#undef MIN                      // occurs in <sys/param.h>
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define OPT(x,y) ((x) ? (x) : (y))
#define NTOHSP(_p) ntohs(*(uint16_t*)(_p))
#define NTOHLP(_p) ntohl(*(uint32_t*)(_p))

//---- Caching
static HASH fnvstr(char const *buf);
static void update_cache(MADNS *, RESPONSE const *);

//---- Auditing
FILE   *madns_log;
static void log_(int line, const char *fmt, ...);
static void log_packet(int line, char *pkt, int len);

#undef  LOG
#define LOG(...) (madns_log ? log_(__LINE__,__VA_ARGS__) : 0)
static double start;       // timestamp (elapse usec) for log.
static double tick(void);

static inline char const *ipstr(in_addr_t ip, char buf[INET_ADDRSTRLEN])
{
    return inet_ntop(AF_INET, (struct in_addr *)&ip, buf, INET_ADDRSTRLEN);
}

MADNS  *
madns_create(char const *resolv_conf, int query_time, int server_reqs)
{
    int     i, rcvbufsiz = 128 * 1024;
    MADNS  *mp = calloc(1, sizeof(MADNS));
    char    line[512];

    start = tick();
    mp->sock = -1;              // for destroy, called inside "create".
    mp->query_time = OPT(query_time, MADNS_QUERY_TIME);
    mp->limit = MIN_CACHE;

    FILE   *fp = fopen(resolv_conf ? resolv_conf : MADNS_RESOLV_CONF, "r");

    if (fp && (mp->serv = malloc(sizeof *mp->serv * fseek(fp, 0L, 2)))) {
        for (rewind(fp); fgets(line, sizeof line, fp);)
            if (1 == sscanf(line, "nameserver %s", line)) {
                mp->serv[mp->nservs].ip = inet_addr(line);
                if (mp->serv[mp->nservs].ip != INADDR_NONE)
                    mp->serv[mp->nservs++].nreqs = 0;
            }
        fclose(fp);
    }

    if (!mp->nservs)
        return madns_destroy(mp), NULL;

    mp->server_reqs = MIN(OPT(server_reqs, MADNS_SERVER_REQS),
                          MAX_TIDS / mp->nservs);
    mp->qsize = mp->nfree = mp->nservs * mp->server_reqs;
    if (mp->qsize > MAX_TIDS || mp->qsize < 2)
        return madns_destroy(mp), NULL;

    mp->serv = realloc(mp->serv, sizeof(SERVER) * mp->nservs);
    mp->cachev = calloc(mp->limit, sizeof(CACHE_INFO *));
    mp->queries = malloc(mp->qsize * sizeof(*mp->queries));

    qinit(&mp->active);
    qinit(&mp->unused);
    for (i = 0; i < mp->qsize; ++i)
        qpush(&mp->unused, &mp->queries[i].link);

    mp->sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mp->sock == -1)
        return madns_destroy(mp), NULL;

    fcntl(mp->sock, F_SETFD, FD_CLOEXEC);
    fcntl(mp->sock, F_SETFL, O_NONBLOCK | fcntl(mp->sock, F_GETFL, 0));
    setsockopt(mp->sock, SOL_SOCKET, SO_RCVBUF,
               (char *)&rcvbufsiz, sizeof rcvbufsiz);
    return mp;
}

void
madns_destroy(MADNS * mp)
{
    if (!mp)
        return;
    if (mp->sock != -1)
        (void)close(mp->sock);

    int     i;

    for (i = 0; i < mp->limit; ++i)
        free(mp->cachev[i]);

    while (!qempty(&mp->active))
        destroy_query(mp, link_QUERY(mp->active.next), 0);

    free(mp->queries), free(mp->serv), free(mp);
}

int
madns_fileno(MADNS const *mp)
{
    return mp->sock;
}

int
madns_ready(MADNS const *mp)
{
    return mp->nfree;
}

int
madns_expires(MADNS * mp)
{
    return qempty(&mp->active) ? mp->query_time + 1
        : link_QUERY(mp->active.next)->expires - time(0);
}

in_addr_t
madns_lookup(MADNS const *mp, char const *name)
{
    in_addr_t ip = inet_addr(name);

    if (ip != INADDR_NONE)
        return ip;
    if (strlen(name) > DNS_MAX_HOSTNAME)
        return INADDR_NONE;

    HASH    h, hash = fnvstr(name);
    time_t  now = time(0);

    for (h = hash;; ++h) {
        CACHE_INFO *cip = mp->cachev[h &= mp->limit - 1];

        if (!cip)
            return INADDR_ANY;
        if (cip->hash == hash && cip->expires >= now
            && !strcasecmp(cip->name, name))
            return cip->ip;
    }
}

int
madns_request(MADNS * mp, char const *name, void *ctx)
{
    if (!ctx || !madns_ready(mp) || strlen(name) > DNS_MAX_HOSTNAME)
        return 0;

    QUERY  *qp = link_QUERY(qpull(mp->unused.next));

    mp->nfree--;
    qp->ctx = ctx;
    qp->expires = 0;            // so failure in "send_request" causes instant expiry.
    qp->tid = qp - mp->queries + mp->qsize * ((rand() & 32767) / mp->qsize + 1);
    qp->name = strdup(name);
    qp->started = tick();
    qpush(&mp->active, &qp->link);
    send_request(mp, qp);

    return qp->tid;             // for auditing only; anything other than (-1) is okay.
}

void   *
madns_response(MADNS * mp, in_addr_t * ip)
{
    while (1) {
        char    pkt[DNS_PACKET_LEN];
        INADDR  sa;
        socklen_t salen = sizeof sa;
        RESPONSE resp;
        char    ips[99];

        int     len = recvfrom(mp->sock, pkt, sizeof pkt, 0,
                               (SADDR *) & sa, &salen);
        if (len <= 0)
            break;

        if (!parse_response(pkt, len, &resp))
            continue;

        LOG("resp: ip %s ttl %lu tid %hu name %s\n",
            ipstr(resp.ip, ips), resp.ttl, resp.tid, resp.name);

        QUERY  *qp = &mp->queries[(int)resp.tid % mp->qsize];

        if (qp->ctx && qp->tid == resp.tid && qp->server
            && qp->server->ip == sa.sin_addr.s_addr) {

            if (resp.ip != INADDR_ANY && !strcasecmp(resp.name, qp->name))
                update_cache(mp, &resp);
            return destroy_query(mp, qp, *ip = resp.ip);
        }

        log_packet(__LINE__, pkt, len);
        if (qp->server && qp->server->ip != sa.sin_addr.s_addr)
            LOG("resp.addr=%s tid=%hu ttl=%lu serv=%s\n",
                ipstr(sa.sin_addr.s_addr, pkt), resp.tid, resp.ttl,
                ipstr(qp->server->ip, pkt + 33));
    }

    if (!qempty(&mp->active)) {
        QUERY  *qp = link_QUERY(mp->active.next);

        if (qp->expires <= time(0))
            return destroy_query(mp, qp, *ip = INADDR_ANY);
    }

    return NULL;
}

int
madns_cancel(MADNS * mp, const void *context)
{
    QLINK  *lp;

    for (lp = mp->active.next; lp != &mp->active; lp = lp->next) {
        QUERY  *qp = link_QUERY(lp);

        if (qp->ctx == context) {
            int     tid = qp->tid;  // To audit what was cancelled.
            return destroy_query(mp, qp, 0), tid;
        }
    }

    return 0;
}

void
madns_dump(MADNS const *mp, FILE * fp, MADNS_OPTS opts)
{
    QLINK  *lp;
    int     i;
    char    ips[99];

    if (!fp)
        return;
    int     nunused = qleng(mp->unused.next, mp->nfree);
    int     nactive = qleng(mp->active.next,
                            mp->nservs * mp->server_reqs - mp->nfree);

    fprintf(fp, "\n#-- MADNS:%p query_time:%d server_reqs:%d sock:%d"
            " nservs:%d qsize:%d nfree:%d #active:%d #unused:%d\n",
            mp, mp->query_time, mp->server_reqs, mp->sock,
            mp->nservs, mp->qsize, mp->nfree, nactive, nunused);

    if (opts & QUERIES) {
        fprintf(fp, "# SERVERS:\n# ..... ip............. reqs latency\n");
        for (i = 0; i < mp->nservs; ++i)
            fprintf(fp, "# %5d %-15s %4d %.4f\n",
                    i, ipstr(mp->serv[i].ip, ips), mp->serv[i].nreqs,
                    mp->serv[i].latency);

        if (nactive) {
            fprintf(fp, "# QUERIES:\n# ..... ctx....... elapsed.. tid.."
                    " server......... name\n");
            double  now = tick();

            for (lp = mp->active.next; lp != &mp->active; lp = lp->next) {
                QUERY  *qp = link_QUERY(lp);

                fprintf(fp, "# %5d %p %8.4f %5hu %-15s %s\n",
                        (int)(qp - mp->queries), qp->ctx, now - qp->started,
                        qp->tid, qp->server ? ipstr(qp->server->ip, ips) : "",
                        qp->name);
            }
        }
    }

    if (opts & CACHE) {
        fprintf(fp, "# CACHE: limit:%d count:%d\n# ..... hash.... exps."
                " ip............. name\n", mp->limit, mp->count);
        int     now = time(0);
        CACHE_INFO *cip;

        for (i = 0; i < mp->limit; ++i)
            if ((cip = mp->cachev[i]))
                fprintf(fp, "# %5d %08X %5d %-15s %s\n",
                        i, cip->hash, (int)cip->expires - now,
                        ipstr(cip->ip, ips), cip->name);
    }

    putc('\n', fp);
}

static void *
destroy_query(MADNS * mp, QUERY * qp, in_addr_t logip)
{
    void   *ret = qp->ctx;
    double  latency = tick() - qp->started;

    qp->server->nreqs--;
    qp->server->latency += (latency - qp->server->latency)
        / mp->server_reqs / 2;
    char    ips[99];

    LOG("%s %s lat %.4f -> server %s %.4f reqs=%d\n", qp->name,
        ipstr(logip, ips + 33), latency, ipstr(qp->server->ip, ips),
        qp->server->latency, qp->server->nreqs);

    free(qp->name);
    qpull(&qp->link);
    memset(qp, 0, sizeof *qp);
    qpush(&mp->unused, &qp->link);
    mp->nfree++;

    return ret;
}

// Fowler-Noll-Voh 32-bit hash.
static  HASH
fnvstr(char const *buf)
{
    uint32_t hash = 0x811C9DC5;

    while (*buf)
        hash = (hash ^ tolower(*buf++)) * 0x01000193;
    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return hash;
}

static void
log_(int line, const char *fmt, ...)
{
    fprintf(madns_log, "madns[%d]%8.4f ", line, tick() - start);
    va_list ap;

    va_start(ap, fmt);
    vfprintf(madns_log, fmt, ap);
}

static void
log_packet(int line, char *pkt, int len)
{
    if (!madns_log)
        return;
    int     i;
    char    buf[len * 3], *cp = buf;

    for (i = 12; i < len; ++i)
        cp +=
            sprintf(cp, isgraph(pkt[i]) ? " %c" : " %02X", (uint8_t) pkt[i]);

    DNS_RESP *dp = (DNS_RESP *) pkt;

    log_(line, "UDP[%d]: tid=%hu flags=%hX nquer=%hu"
         " nansw=%hu nauth=%hu noth=%hu [%s ]\n",
         len, dp->tid, ntohs(dp->flags), ntohs(dp->nqueries),
         ntohs(dp->nanswers), ntohs(dp->nauth), ntohs(dp->nother), buf);
}

static int
parse_response(char *pkt, int len, RESPONSE * rp)
{
    DNS_RESP *header = (DNS_RESP *) pkt;
    char   *s = header->data, *e = pkt + len, *p = s;
    int     dlen;

    *rp = (RESPONSE) { INADDR_ANY, /*TTL*/ 0, /*TID*/ header->tid, /*NAME*/ s + 1};

    log_packet(__LINE__, pkt, len);

    // We sent 1 query and expect at least 1 answer.
    if (ntohs(header->nqueries) != 1 || !(0x8000 & ntohs(header->flags)))
        return 0;

    // Convert hostname e.g. "\6google\3com\0" to "google.com\0"
    for (; p < e && *p; p += len + 1)
        len = *p, *p = '.';
    if (*p)
        return log_packet(__LINE__, pkt, len), 0;
    for (p = s + 1; *p; ++p)
        *p = tolower(*p);
    ++p;                        // Skip trailing null

    if (p + 4 > e || NTOHSP(p) != DNS_A_RECORD)
        return log_packet(__LINE__, pkt, len), 0;
    p += 4;                     // Skip (type, class)

    // This must be tested AFTER decoding the hostname,
    //   else madns_response will not update cache.
    if ((ntohs(header->flags) & 0x000F) == DNS_R_NXDOMAIN)
        return rp->ip = INADDR_NONE, rp->ttl = 86400, 1;
    if (header->nanswers == 0)  // Try another server?
        return 1;

    while (p + 12 < e) {
        // Skip possible name prefixing a CNAME answer.
        if ((uint8_t) * p != 0xC0) {
            while (*p && p + 12 < e)
                p++;
            p--;
        }

        switch (NTOHSP(p + 2)) {
        case DNS_A_RECORD:
            rp->ttl = NTOHLP(p + 6);
            if (p + 12 < e) {
                dlen = NTOHSP(p + 10);
                p += 12;
                if (p + dlen <= e)  // SUCCESS!
                    return rp->ip = *(in_addr_t *) p, 1;
            }
            break;
        case DNS_CNAME:        // Skip this section. NOTREACHED?
            log_packet(__LINE__, pkt, len);
            p += 12 + NTOHSP(p + 10);
            break;
        default:               // Leave the loop.
            p = e;
        }
    }

    return 0;
}

static void
send_request(MADNS * mp, QUERY * qp)
{
    SERVER *prev = qp->server;
    char    pkt[DNS_PACKET_LEN], ips[99];
    DNS_RESP *header = (DNS_RESP *) pkt;

    // Choose lowest-latency server:
    int     i;

    for (i = 0; i < mp->nservs; ++i)
        if (&mp->serv[i] != prev && mp->serv[i].nreqs < mp->server_reqs)
            break;
    if (i == mp->nservs)
        return;

    if (prev)
        prev->nreqs--;
    for (qp->server = &mp->serv[i]; i < mp->nservs; ++i)
        if (&mp->serv[i] != prev && mp->serv[i].nreqs < mp->server_reqs
            && mp->serv[i].latency < qp->server->latency)
            qp->server = &mp->serv[i];
    qp->server->nreqs++;

    header->tid = qp->tid;
    header->flags = ntohs(0x0100);  // Recursive query.
    header->nqueries = ntohs(1);
    header->nanswers = 0;
    header->nauth = 0;
    header->nother = 0;

    // Encode "Mail.Google.com" as \4mail\6google\3com\0
    char   *p = header->data, *dst = p + 1;
    char const *src;

    for (src = qp->name; *src; ++src, ++dst) {
        if (*src != '.')
            *dst = tolower(*src);
        else if (dst - p - 1 <= NS_MAXLABEL)
            *p = dst - p - 1, p = dst;
        else
            return;             // Unencodable domain name; expiry=0.
    }

    *p = dst - p - 1, p = dst;
    *p++ = 0;                   // Mark end of host name
    *p++ = 0;                   // 1-byte DNS checksum that defaults to 0 (!?)
    *p++ = DNS_A_RECORD;        // Query Type aka (ns_t_a)
    *p++ = 0;
    *p++ = 1;                   // Class: inet aka (ns_c_in)
    int     len = p - pkt;

    INADDR  addr = { /*FAMILY*/ AF_INET, /*PORT*/ htons(NS_DEFAULTPORT),
         /*INADDR*/ {qp->server->ip}, /*ZERO*/ {}
    };
    if (len == sendto(mp->sock, &pkt, len, 0, (SADDR *) & addr, sizeof addr)
        && !qp->expires)
        qp->expires = time(0) + mp->query_time;

    LOG("%s tid=%d to %s reqs %d\n", qp->name, qp->tid,
        ipstr(qp->server->ip, ips), qp->server->nreqs);
}

static double
tick(void)
{
    struct timeval t;

    gettimeofday(&t, 0);
    return t.tv_sec + 1E-6 * t.tv_usec;
}

static void
update_cache(MADNS * mp, RESPONSE const *rp)
{
    unsigned hash = fnvstr(rp->name), i = hash;
    CACHE_INFO *cip, **putp = NULL, *xp;
    time_t  now = time(0);

    for (; (cip = mp->cachev[i &= mp->limit - 1]); ++i) {
        if (cip->hash == hash && !strcmp(cip->name, rp->name)) {
            cip->expires = now + rp->ttl;
            return;
        }
        if (!putp && cip->expires < now)
            putp = &mp->cachev[i];
    }

    cip = malloc(sizeof(CACHE_INFO) + strlen(rp->name));
    cip->hash = hash;
    cip->expires = now + rp->ttl;
    cip->ip = rp->ip;
    strcpy(cip->name, rp->name);

    if (putp) {                 // An overwritable entry
        free(*putp), *putp = cip;
    } else {
        putp = &mp->cachev[i];
        int     j, count = mp->count + 1, limit;

        if (count >= mp->limit * 3 / 4) {
            // Do "easy sweep" removing entries that don't
            //  require other entries to be relocated.
            int     easy = !mp->cachev[0];

            for (j = mp->limit; --j >= 0;) {
                if (!(xp = mp->cachev[j]))
                    easy = 1;
                else if (xp->expires > now)
                    easy = 0;
                else if (easy)
                    --count, free(xp), mp->cachev[j] = NULL;
            }
        }
        // To avoid thrashing on easy sweeps, rebuild when there are
        //  25% "non-easy" expired entries. This also handles the need
        //  for table growth.
        if (count < mp->limit * 3 / 4 || count < mp->count - mp->limit / 4) {
            *putp = cip;
        } else {
            // Rebuild hash table with (limit) = power of 2 >= count * 4/3:
            for (limit = MIN_CACHE; limit <= count * 4 / 3; limit <<= 1);
            putp = calloc(limit, sizeof(CACHE_INFO *));
            putp[hash & (limit - 1)] = cip; // First insertion!

            for (j = 0; j < mp->limit; ++j) {
                if ((cip = mp->cachev[j])) {
                    if (cip->expires <= now) {
                        free(cip);
                    } else {
                        for (i = cip->hash; putp[i & (limit - 1)]; ++i);
                        putp[i & (limit - 1)] = cip;
                    }
                }
            }

            mp->limit = limit;
            free(mp->cachev);
            mp->cachev = putp;
        }

        mp->count = count;
    }
}

//--------------|---------------------------------------------
static inline void
qinit(QLINK * q)
{
    q->prev = q->next = q;
}

static inline int
qempty(QLINK * q)
{
    return q == q->next;
}

static int
qleng(QLINK const *list, int limit)
{
    QLINK const *qp = list;
    int     leng = 0;

    for (; leng <= limit && (qp = qp->next) != list; ++leng);
    return leng > limit ? -1 : leng;
}

static inline void
qpush(QLINK * q, QLINK * qel)
{
    *qel = (QLINK) { q, q->prev};
    qel->prev->next = q->prev = qel;
}

    // Extract (q) from whatever linked list it is in.
static inline QLINK *
qpull(QLINK * q)
{
    q->next->prev = q->prev;
    q->prev->next = q->next;
    return q;
}
//EOF
