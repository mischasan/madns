MADNS: a simple, standalone async DNS client

Mischa Sandberg <mischasan@gmail.com>

MADNS is a self-contained async DNS client that sends requests to a set of servers, 
then parses, caches and returns the responses (IP).

FEATURES
--------

MADNS is an example of how to write an async library that can be plugged into somebody else's event-loop framework.
The caller is allowed to access the socket file descriptor, so that the caller can wait for events on that socket.

MADNS is also an example of how to create such a module without using callbacks. The caller has complete control over
when and how to retrieve completed/timed-out responses.

MADNS sends requests to the lowest-latency, least-loaded DNS server in its configured list. You may have a better idea.

You may find some useful ideas in the incrementally-cleaned hash table used as a cache.

GETTING STARTED
---------------

"hostip.c" is an example of how to integrate MADNS into an external "select" loop.
The standalone program "hostip" reads a stream of requests (domain names)
and writes a stream of:

    input[tab]ip 

or 

    input[tab]0.0.0.0

... as responses are received or timed out. 
A DNS server may reply INADDR_NONE (255.255.255.255).
On exit, it prints some stats to stderr.


DOCUMENTATION
-------------

Sorry. I hope "hostip.c" works for you.

LICENSE
-------

This module is based on "tadns", Copyright (c) 2004-2005 Sergey Lyubka <valenok@gmail.com> 
under "THE BEER-WARE LICENSE" (Revision 42)
It continues under Sergey's "BEERWARE" license. Have fun. Let me know what you do with it.
