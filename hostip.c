// "hostip" reads stdin or a file for hostnames at the same time as
//  writing DNS responses to stdout.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>             // malloc...
#include <string.h>
#include <time.h>
#include <unistd.h>             // access getopt
#include <arpa/inet.h>          // inet_ntoa

#include "madns.h"

static inline char const *
iptoa(in_addr_t ip)
{
    return inet_ntoa((struct in_addr) {ip});
}

static inline void
usage(void)
{
    fputs("Usage: hostip [-c resolv.conf] [-d] (hostfile | -)\n", stderr);
    exit(1);
}

int
main(int argc, char **argv)
{
    char const *resolv_conf = "/etc/resolv.conf";
    int     opt;

    while ((opt = getopt(argc, argv, "c:d")) != -1) {
        switch (opt) {
        case 'c':
            resolv_conf = optarg;
            break;
        case 'd':
            madns_log = stderr;
            setvbuf(stdout, 0, _IOLBF, 0);
            break;
        default:
            usage();            // '?' etc.
        }
    }
    if (optind >= argc)
        usage();
    if (access(resolv_conf, R_OK))
        return fprintf(stderr,
                       "hostip: %s not a valid (resolv.conf) file\n",
                       resolv_conf);

    MADNS  *mp = madns_create(resolv_conf, /*expiry */ 5, /*server_reqs */ 15);
    if (!mp)
        return fputs("hostip: madns_create failed\n", stderr);

    FILE   *fp = strcmp(argv[optind], "-") ? fopen(argv[optind], "r") : stdin;

    if (!fp)
        return fprintf(stderr, "hostip: unable to read '%s'\n", argv[optind]);

    int     nactive = 0, eoi = 0, nreqs = 0;
    int     inpfd = fileno(fp), dnsfd = madns_fileno(mp);
    int     nfds = (inpfd > dnsfd ? inpfd : dnsfd) + 1;
    fd_set  inp_fds;

    FD_ZERO(&inp_fds);
    FD_SET(inpfd, &inp_fds);
    FD_SET(dnsfd, &inp_fds);
    time_t  t = time(0);

    while (!eoi || nactive > 0) {
        char   *info, buf[2000];
        in_addr_t ipaddr;
        fd_set  read_fds = inp_fds;
        struct timeval tv = { madns_expires(mp), 0 };
        if (0 > select(nfds, &read_fds, NULL, NULL, &tv)) {
            fprintf(stderr, "expires=%lu\n", tv.tv_sec);
            perror("select");
            break;
        }
        // Check dns responses/expiries first; may increase madns_ready.
        for (; (info = madns_response(mp, &ipaddr)); --nactive)
            printf("%s\t%s\n", info, iptoa(ipaddr)), free(info);

        if (FD_ISSET(inpfd, &read_fds) && madns_ready(mp)) {
            if ((eoi = !fgets(buf, sizeof buf, fp))) {
                FD_CLR(inpfd, &inp_fds);
            } else {
                buf[strlen(buf) - 1] = 0;   // chomp
                ++nreqs;

                // Extract the hostname:
                char    host[999];
                int     hostlen = strcspn(buf, "\t #?/");

                memcpy(host, buf, hostlen);
                host[hostlen] = 0;

                // Check cache first; queue request if no cache entry:
                ipaddr = madns_lookup(mp, host);
                if (ipaddr == INADDR_ANY)
                    ++nactive, madns_request(mp, host, strdup(buf));
                else
                    printf("%s\t%s\n", buf, iptoa(ipaddr));
            }
        }
    }

    t = time(0) - t;
    fprintf(stderr, "HOSTIP: reqs: %d secs: %lu => %.3f r/s\n",
            nreqs, t, (double)nreqs / t);
    if (madns_log)
        madns_dump(mp, madns_log, -1);
    madns_destroy(mp);
    return 0;
}
