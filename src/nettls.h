#ifndef NETTLS_H
#define NETTLS_H
#include <psp2/net/http.h>
/* HTTPS on the Vita: the system certificate store is old and has no modern root
 * certificates, so https:// addresses (tinyurl, github, CDNs) fail the handshake
 * with 0x80435xxx. A playlist/stream address is public data, so the certificate
 * checks are switched off for every HTTP template. */
extern int sceSslInit(unsigned int poolSize);

#define NETTLS_ALL_FLAGS 0x3Fu   /* server verify, client verify, CN, not-after, not-before, known CA */

static inline void net_tls_relax(int tpl)
{
    (void)tpl;
    sceHttpsDisableOption(NETTLS_ALL_FLAGS);   /* SDK: process-wide, not per template */
}
#endif
