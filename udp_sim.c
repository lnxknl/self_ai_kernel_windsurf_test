// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * UDP Protocol Implementation Simulation
 * Based on Linux kernel UDP implementation
 */

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/checksum.h>
#include <net/udp.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/route.h>
#include <net/inet_common.h>

/* UDP Socket Structure */
struct udp_sock {
    struct sock sk;
    __u16 pending;           /* Pending datagram queue length */
    __u16 encap_type;        /* UDP encapsulation type */
    __u16 len;              /* UDP message length */
    __u16 source;           /* Source port */
    __u16 dest;             /* Destination port */
    __u32 daddr;            /* Destination address */
    __u32 saddr;            /* Source address */
    __u8  checksum_zero:1;  /* UDP checksum not required */
    __u8  no_check6_tx:1;   /* Disable checksum on outgoing packets */
    __u8  no_check6_rx:1;   /* Disable checksum on incoming packets */
    __u8  encap_enabled:1;  /* UDP encapsulation enabled */
    int (*encap_rcv)(struct sock *sk, struct sk_buff *skb);
    void (*encap_destroy)(struct sock *sk);
};

/* UDP Statistics */
struct udp_mib {
    unsigned long udp_packets;
    unsigned long udp_errors;
    unsigned long udp_dropped;
    unsigned long udp_noports;
    unsigned long udp_rx_csum_errors;
    unsigned long udp_ignored_errors;
};

static struct udp_mib udp_statistics;

/* UDP Socket Hash Table */
#define UDP_HTABLE_SIZE 128
static struct hlist_head udp_hash[UDP_HTABLE_SIZE];
static DEFINE_SPINLOCK(udp_hash_lock);

/* UDP Socket Functions */

static inline struct udp_sock *udp_sk(const struct sock *sk)
{
    return (struct udp_sock *)sk;
}

static unsigned int udp_hash_function(struct net *net, __be16 port)
{
    return jhash_1word((__u32)port, 0) & (UDP_HTABLE_SIZE - 1);
}

static int udp_v4_get_port(struct sock *sk, unsigned short snum)
{
    unsigned int hash = udp_hash_function(sock_net(sk), snum);
    struct udp_sock *up = udp_sk(sk);
    struct hlist_node *node;
    struct sock *sk2;
    int ret = 0;

    spin_lock(&udp_hash_lock);
    
    if (!snum) {
        /* Find an available port */
        for (snum = 1024; snum < 65535; snum++) {
            hash = udp_hash_function(sock_net(sk), snum);
            hlist_for_each_entry(sk2, &udp_hash[hash], sk_node) {
                if (sk2->sk_port == snum)
                    goto next_port;
            }
            break;
next_port:
            continue;
        }
        if (snum >= 65535) {
            ret = -EADDRINUSE;
            goto out;
        }
    } else {
        /* Check if requested port is available */
        hlist_for_each_entry(sk2, &udp_hash[hash], sk_node) {
            if (sk2->sk_port == snum) {
                ret = -EADDRINUSE;
                goto out;
            }
        }
    }

    /* Assign port and add to hash table */
    sk->sk_port = snum;
    hlist_add_head(&sk->sk_node, &udp_hash[hash]);

out:
    spin_unlock(&udp_hash_lock);
    return ret;
}

/* UDP Packet Handling */

static int udp_v4_checksum(struct sk_buff *skb)
{
    struct udphdr *uh = udp_hdr(skb);
    __wsum csum;

    if (uh->check == 0)
        return 0;

    csum = skb_checksum(skb, 0, skb->len, 0);
    if (csum == 0)
        return 0;
    
    return -1;
}

static int udp_v4_rcv(struct sk_buff *skb)
{
    struct udphdr *uh;
    struct sock *sk;
    int ret;

    /* Validate UDP header */
    if (!pskb_may_pull(skb, sizeof(struct udphdr)))
        goto drop;

    uh = udp_hdr(skb);

    /* Verify checksum */
    if (udp_v4_checksum(skb) < 0)
        goto csum_error;

    /* Find matching socket */
    sk = __udp4_lib_lookup(sock_net(skb->sk), ip_hdr(skb)->saddr,
                          uh->source, ip_hdr(skb)->daddr,
                          uh->dest, skb->dev->ifindex);
    if (!sk)
        goto no_sock;

    /* Queue packet to socket */
    ret = udp_queue_rcv_skb(sk, skb);
    if (ret < 0)
        goto drop;

    return 0;

csum_error:
    udp_statistics.udp_rx_csum_errors++;
    goto drop;

no_sock:
    udp_statistics.udp_noports++;
    icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);
    goto drop;

drop:
    udp_statistics.udp_dropped++;
    kfree_skb(skb);
    return -1;
}

/* UDP Socket Operations */

static int udp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
    struct sockaddr_in *addr = (struct sockaddr_in *)uaddr;
    struct udp_sock *up = udp_sk(sk);
    int ret;

    if (addr_len < sizeof(struct sockaddr_in))
        return -EINVAL;

    if (addr->sin_family != AF_INET)
        return -EAFNOSUPPORT;

    sk->sk_state = TCP_ESTABLISHED;
    up->daddr = addr->sin_addr.s_addr;
    up->dest = addr->sin_port;

    return 0;
}

static int udp_v4_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
    struct udp_sock *up = udp_sk(sk);
    struct sk_buff *skb;
    struct udphdr *uh;
    int err;

    /* Create new skb */
    skb = sock_alloc_send_skb(sk, len + sizeof(struct udphdr),
                             msg->msg_flags & MSG_DONTWAIT, &err);
    if (!skb)
        return err;

    /* Reserve space for headers */
    skb_reserve(skb, sizeof(struct udphdr));

    /* Copy data */
    err = memcpy_from_msg(skb_put(skb, len), msg, len);
    if (err < 0) {
        kfree_skb(skb);
        return err;
    }

    /* Build UDP header */
    uh = (struct udphdr *)skb_push(skb, sizeof(struct udphdr));
    uh->source = up->source;
    uh->dest = up->dest;
    uh->len = htons(len + sizeof(struct udphdr));
    uh->check = 0;

    /* Calculate checksum */
    if (!up->checksum_zero) {
        skb->csum = csum_partial(skb->data, len + sizeof(struct udphdr), 0);
        uh->check = csum_tcpudp_magic(up->saddr, up->daddr,
                                     len + sizeof(struct udphdr),
                                     IPPROTO_UDP, skb->csum);
        if (uh->check == 0)
            uh->check = CSUM_MANGLED_0;
    }

    /* Send packet */
    err = ip_send_skb(sock_net(sk), skb);
    if (err) {
        udp_statistics.udp_errors++;
        return err;
    }

    udp_statistics.udp_packets++;
    return len;
}

static int udp_v4_recvmsg(struct sock *sk, struct msghdr *msg, size_t len,
                         int flags, int *addr_len)
{
    struct sk_buff *skb;
    struct udphdr *uh;
    int copied, err;

    /* Get next packet from receive queue */
    skb = skb_recv_datagram(sk, flags, flags & MSG_DONTWAIT, &err);
    if (!skb)
        return err;

    uh = udp_hdr(skb);
    copied = skb->len - sizeof(struct udphdr);
    if (copied > len) {
        copied = len;
        msg->msg_flags |= MSG_TRUNC;
    }

    /* Copy data to user buffer */
    err = skb_copy_datagram_msg(skb, sizeof(struct udphdr), msg, copied);
    if (err)
        goto out_free;

    /* Fill in source address */
    if (msg->msg_name) {
        struct sockaddr_in *sin = (struct sockaddr_in *)msg->msg_name;
        sin->sin_family = AF_INET;
        sin->sin_port = uh->source;
        sin->sin_addr.s_addr = ip_hdr(skb)->saddr;
        *addr_len = sizeof(*sin);
    }

    err = copied;

out_free:
    skb_free_datagram(sk, skb);
    return err;
}

/* UDP Protocol Operations */
static const struct proto_ops udp_v4_ops = {
    .family = PF_INET,
    .connect = udp_v4_connect,
    .sendmsg = udp_v4_sendmsg,
    .recvmsg = udp_v4_recvmsg,
};

/* UDP Protocol Registration */
static struct inet_protosw udp4_protosw = {
    .type = SOCK_DGRAM,
    .protocol = IPPROTO_UDP,
    .prot = &udp_prot,
    .ops = &udp_v4_ops,
    .flags = INET_PROTOSW_PERMANENT,
};

/* UDP Protocol Initialization */
static int __init udp_v4_init(void)
{
    int i;

    /* Initialize hash table */
    for (i = 0; i < UDP_HTABLE_SIZE; i++)
        INIT_HLIST_HEAD(&udp_hash[i]);

    /* Clear statistics */
    memset(&udp_statistics, 0, sizeof(udp_statistics));

    /* Register protocol */
    proto_register(&udp_prot, 1);
    inet_register_protosw(&udp4_protosw);

    return 0;
}

/* UDP Protocol Cleanup */
static void __exit udp_v4_exit(void)
{
    inet_unregister_protosw(&udp4_protosw);
    proto_unregister(&udp_prot);
}

module_init(udp_v4_init);
module_exit(udp_v4_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UDP Protocol Implementation Simulation");
MODULE_AUTHOR("Your Name");
