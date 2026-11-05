/* curl's <linux/tcp.h> in the kpp game's FreeBSD build: Linux's curl_config.h (Factorio's, the one curl has)
   says HAVE_LINUX_TCP_H, and curl then includes it instead of <netinet/tcp.h>, where FreeBSD has the same
   TCP options (TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_NODELAY). */
#include <netinet/tcp.h>
