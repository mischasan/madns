#include <stdio.h>
#include <stdlib.h>             // getenv
#include <unistd.h>             // sleep
#include <sys/select.h>
#include <arpa/inet.h>          // inet_ntoa

#include "tap.h"
#include "madns.h"

#define iptoa(ip)    inet_ntoa((struct in_addr){ip})

char const *default_argv[] =
        { "google.com", "cookie4you.com", "abc.com", NULL };

int
main(void)
{
    plan_tests(13);

    char *conf = getenv("madns");
    int expt = asprintf(&conf, "%s/resolv.conf", conf ? conf : ".");
    MADNS *mp = madns_create(conf, /*expiry */ expt = 5, /*reqs */ 4);
    ok(mp, "created");

    int ret = madns_request(mp, "invalid.host1", (void *)(intptr_t) "INVALID host ONE");
    ok(ret >= 0, "request invalid.host.host1: %d", ret);

    char const *google = "gOOgle.com";
    int tid = madns_request(mp, google, (void *)(intptr_t) google);
    ok(tid >= 0, "request gOOgle.com: %d", tid);

    ret = madns_request(mp, "invalid.host2", (void *)(intptr_t) "INVALID host TWO");
    ok(ret >= 0, "request invalid.host2: %d", ret);

    ret = madns_request(mp, "fAcEbook.com", (void *)(intptr_t) "FaceBook.Com");
    ok(ret >= 0, "request facebook.com: %d", ret);

    ret = madns_cancel(mp, google);
    ok(ret == tid, "%s request cancelled: %d", google, ret);

    int secs = madns_expires(mp);

    ok(secs == expt, "next expiry in %d secs", secs);

    fputs("# sleep(1) to ensure facebook.com DNS response is in the pipe\n", stderr);
    sleep(1);

    secs = madns_expires(mp);
    ok(secs == expt - 1, "next expiry in %d secs", secs);

    struct timeval tv = { 2, 0 };
    fd_set rds;

    FD_ZERO(&rds);
    FD_SET(madns_fileno(mp), &rds);
    ret = select(madns_fileno(mp) + 1, &rds, NULL, NULL, &tv);
    ok(ret == 1, "response pending");

    char *cp;
    in_addr_t ip;
    int canned = 0, count = 0;

    while ((cp = madns_response(mp, &ip))) {
        fprintf(stderr, "# response: %s -> %s\n", cp,
                iptoa(ip));
        canned |= cp == google;
        ++count;
    }

    ok(count, "response returned %d times", count);
    ok(!canned,
       "response did not return the cancelled request");

    ip = madns_lookup(mp, google);
    ok(ip == INADDR_ANY, "google lookup returned: %s",
       iptoa(ip));

    ip = madns_lookup(mp, "FACEbook.COM");
    ok(ip != INADDR_ANY, "facebook lookup returned: %s",
       iptoa(ip));

    secs = madns_expires(mp);
    fprintf(stderr, "# sleep(expires=%d)\n", secs);
    sleep(secs);
    while ((cp = madns_response(mp, &ip)))
        fprintf(stderr, "# response: %s -> %s\n", cp,
                iptoa(ip));

    madns_dump(mp, stderr, -1);
    madns_destroy(mp);

    return exit_status();
}
