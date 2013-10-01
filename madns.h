
/*
 * Copyright (c) 2004-2005 Sergey Lyubka <valenok@gmail.com>
 *
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Sergey Lyubka wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.
 */

// "madns" is an extension of Sergey's original "madns",
//  by Mischa Sandberg (mischasan@gmail.com). madns is a multi-server async lookup.
//  It includes a cache because TTL from a DNS server is an agreement between
//  DNS server and client, that the client will not pester the server with
//  what the client ought to remember.

#ifndef MADNS_H
#define MADNS_H

#include <stdio.h>
#include <netinet/in.h>         // in_addr_t

typedef struct madns MADNS;

// Create a MADNS object.
// resolv_conf: a resolv.conf(5) file name. Must contain "nameserver <ip>" lines.
// query_time:  request expiry time, in secs.
// server_reqs: max active requests per server
MADNS  *madns_create(char const *resolv_conf, int query_time,
                     int server_reqs);

// For all "madns_create" parameters, (0) means "use the default":
#define MADNS_RESOLV_CONF    "/etc/resolv.conf"
#define MADNS_QUERY_TIME      10
#define MADNS_SERVER_REQS     20

void    madns_destroy(MADNS *);

// UDP socket fd, for select/epoll:
int     madns_fileno(MADNS const *);

// Seconds until the next query expires.
int     madns_expires(MADNS *);

// Number of requests madns can accept (given current pending requests).
int     madns_ready(MADNS const *);

// Look up host in cache.
// Returns host ip, or:
//      INADDR_ANY:  hostname not in cache.
//      INADDR_NONE: hostname in cache as NXDOMAIN, _OR_ hostname too long.
in_addr_t madns_lookup(MADNS const *, char const *host);

// Post request to a DNS server.
//  Returns 0 if strlen(host) > 1024 or not ready or I/O error.
// Otherwise, returns (DNS) transaction ID 1..65535.
int     madns_request(MADNS *, char const *host, void *context);

// Cancel request matching context.
//      Returns 0 if request not found.
int     madns_cancel(MADNS *, void const *context);

// Retrieve a DNS response (ip) or expiry.
//  Returns the context ptr from madns_request(), and sets *ip:
//      INADDR_ANY:  an expired request.
//      INADDR_NONE: NXDOMAIN
// Returns NULL when there are no more responses pending.
void   *madns_response(MADNS *, in_addr_t * ip);

//--------------|---------------------------------------------
typedef enum { SUMMARY = 0, QUERIES = 1, CACHE = 2 } MADNS_OPTS;
void    madns_dump(MADNS const *, FILE *, MADNS_OPTS);

// Enable diagnostics with "madns_log = stderr".
extern FILE *madns_log;

#endif // MADNS_H
